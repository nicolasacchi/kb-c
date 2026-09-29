/* httpd.c — epoll + SO_REUSEPORT HTTP/1.1 daemon.
 * Organised as: (1) socket + worker plumbing, (2) HTTP/1.1 parsing,
 * (3) the routing table, (4) the SSE endpoint, (5) the serving surface —
 * artifact bytes, the artifact subdomain, the static fallback and /metrics —
 * (6) the KBC_ROUTES export.
 *
 * Concurrency: cfg->http_workers threads, one listening socket each
 * (SO_REUSEPORT, so the kernel balances accepts without a lock), one epoll set
 * each. A normal request is served start to finish on the accepting thread. An
 * SSE connection is registered with the worker's epoll instead and woken
 * through its own eventfd, so a streaming client occupies no thread.
 *
 * Every buffer a request touches is bounded by a KBC_MAX_* constant; the only
 * unbounded-looking number, Content-Length, is rejected (413) before a single
 * body byte is read.
 */

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "kbc/httpd.h"
#include "kbc/json.h"
#include "kbc/log.h"
#include "kbc/html.h"
#include "kbc/markdown.h"
#include "kbc/parse.h"

#define KBC_EPOLL_MAX_EVENTS 64
#define KBC_EPOLL_TIMEOUT_MS 200
#define KBC_SSE_KEEPALIVE_NS (15ll * 1000000000ll)
#define KBC_SSE_QUEUE_CAP 64u
#define KBC_SSE_RING_CAP 256u /* the history Last-Event-ID replays from */
#define KBC_SSE_OUT_HIGH_WATER (256u * 1024u)
#define KBC_MAX_HEADERS 48u
#define KBC_LISTEN_BACKLOG 512
#define KBC_WORKER_MIN 1u
#define KBC_WORKER_MAX 64u
#define KBC_REQ_BUF_MAX                                                       \
  (KBC_HTTP_MAX_REQUEST_LINE + KBC_HTTP_MAX_HEADER_BYTES + KBC_MAX_SNIFF_BYTES)

/* Per-connection request cap. The daemon is loopback-by-default and refuses a
 * routable bind with no token, so there is no untrusted-caller path to rate
 * limit against; what this protects is the worker, not the corpus. One client
 * that pipelines a million requests must not be able to keep a worker busy
 * past the point where other clients are served. A fixed window is enough for
 * that, and a token bucket would only be a more expensive lie. */
#define KBC_RATE_LIMIT_DEFAULT 120u /* requests per connection per second */
#define KBC_RATE_WINDOW_NS (1000000000ll)
/* A preflight is a browser's question, not work: it is counted separately so
 * that a page opening twenty connections does not spend the budget it has for
 * the data it actually asked for. */
#define KBC_CORS_MAX_ORIGINS 32u
#define KBC_PEER_ADDR_MAX 64u

/* The origin split's two constants. `artifact_host_suffix` and
 * `parent_origin` are `[server]` keys in the original; kbc_config has no
 * server section, so they come from the environment (KBC_ARTIFACT_HOST_SUFFIX,
 * KBC_PARENT_ORIGIN) and are read once at bring-up. The parent-origin default
 * is the "not configured for production" sentinel: the subdomain serve sends
 * NO frame-ancestors CSP for it, exactly as `artifact.rs:36-49` does. */
#define KBC_HOST_SUFFIX_DEFAULT ".artifacts.localhost"
#define KBC_PARENT_ORIGIN_DEFAULT "http://localhost:4000"

/* ------------------------------------------------------------------ util -- */

static bool str_ieq(const char *a, const char *b) {
  if (!a || !b) return false;
  for (size_t i = 0;; i++) {
    unsigned char ca = (unsigned char)a[i];
    unsigned char cb = (unsigned char)b[i];
    unsigned char la = (ca >= 'a' && ca <= 'z') ? (unsigned char)(ca - 32) : ca;
    unsigned char lb = (cb >= 'a' && cb <= 'z') ? (unsigned char)(cb - 32) : cb;
    if (la != lb) return false;
    if (la == '\0') return true;
  }
}

static int hex_val(unsigned char c) {
  if (c >= '0' && c <= '9') return (int)(c - '0');
  if (c >= 'a' && c <= 'f') return (int)(c - 'a') + 10;
  if (c >= 'A' && c <= 'F') return (int)(c - 'A') + 10;
  return -1;
}

/* RFC 7807 `title`: the reason phrase for the status, so a client that drops
 * the body still has something to show. 429 is here because the per-connection
 * request cap answers with it; 201 and 415 are here because the capture route
 * creates documents and gates their extension, and a reason phrase that read
 * "Error" on a 201 would be the daemon disagreeing with itself on the wire. */
static const char *status_text(int status) {
  switch (status) {
  case 200: return "OK";
  case 201: return "Created";
  case 202: return "Accepted";
  case 204: return "No Content";
  case 400: return "Bad Request";
  case 401: return "Unauthorized";
  case 403: return "Forbidden";
  case 404: return "Not Found";
  case 405: return "Method Not Allowed";
  case 409: return "Conflict";
  case 413: return "Payload Too Large";
  case 414: return "URI Too Long";
  case 415: return "Unsupported Media Type";
  case 429: return "Too Many Requests";
  case 500: return "Internal Server Error";
  case 503: return "Service Unavailable";
  default: return "Error";
  }
}

/* RFC 7807 `type`, a stable URN per status. A client dispatches on this, never
 * on the human `title`; the mapping is deliberately one slug per status so a
 * new call site cannot invent a new type by accident. 409 and 415 are the
 * capture route's: a destination that cannot be written RIGHT NOW, and a file
 * whose extension this corpus will not index. */
static const char *problem_type(int status) {
  switch (status) {
  case 400: return "urn:kb:errors:bad-request";
  case 401: return "urn:kb:errors:unauthorized";
  case 403: return "urn:kb:errors:forbidden";
  case 404: return "urn:kb:errors:not-found";
  case 405: return "urn:kb:errors:method-not-allowed";
  case 409: return "urn:kb:errors:conflict";
  case 413: return "urn:kb:errors:payload-too-large";
  case 414: return "urn:kb:errors:uri-too-long";
  case 415: return "urn:kb:errors:unsupported-media-type";
  case 429: return "urn:kb:errors:too-many-requests";
  case 500: return "urn:kb:errors:internal";
  case 503: return "urn:kb:errors:unavailable";
  default: return "urn:kb:errors:error";
  }
}


/* ---------------------------------------------------------- (1) plumbing -- */

typedef enum { CONN_TAG_FD = 1, CONN_TAG_EV } conn_tag;

typedef struct sse_frame {
  char *data; /* KBC_OWN (malloc): the fan-out runs on the publishing thread,
                  not on the worker that writes it */
  size_t len;
} sse_frame;

typedef struct kbc_worker kbc_worker;

typedef struct conn {
  kbc_httpd *h;
  kbc_worker *w;
  int tag;
  int fd;
  int event_fd; /* SSE wake fd, -1 otherwise */
  uint32_t registered; /* events currently armed in the worker's epoll */
  bool close_after;
  bool eof;
  bool sse;
  kbc_arena *arena;

  kbc_str in;         /* the buffered request bytes; `in.len` IS the length */
  kbc_str out;
  size_t out_off;
  size_t header_end; /* offset just past CRLFCRLF, 0 until headers complete */
  size_t body_want;  /* Content-Length, 0 when absent */
  size_t body_off;   /* offset of the body start */
  size_t consumed;   /* header_end + body_want */

  char peer[KBC_PEER_ADDR_MAX]; /* the accept() address, "?" when unknown */

  /* Per-connection request cap: a fixed window owned by this connection alone,
   * so it needs no lock and no other connection can be starved by it. */
  size_t rl_count;
  int64_t rl_win_ns;
  /* Headers the write path adds to the next response: the matched CORS origin
   * (BORROWED from the httpd allowlist, NULL when none matched) and a one-shot
   * extra header line (Retry-After on a 429). */
  const char *cors_origin;
  const char *extra_hdr;
  /* Header lines the NEXT response adds, already formatted as "Name: value".
   * Emitted verbatim by the write path just before its own nosniff, and
   * cleared there, so a handler that sets them on a 200 cannot leak them onto
   * the next response on the same connection. The frozen kbc_response carries
   * no header list (see the port report), so this is where a route's own
   * headers travel. */
  kbc_str extra;

  /* SSE state. `closed` and the conn list are guarded by h->conns_mu; the
   * frame queue is guarded by `mu`. Lock order: conns_mu, then mu. */
  pthread_mutex_t mu;
  sse_frame *q; /* ring of qcap frames */
  size_t qcap, q_head, qcount, q_dropped;
  bool closed;
  int64_t last_write_ns;
} conn;

typedef struct {
  uint64_t id;
  char *type;
  char *json;
} sse_hist;

/* Latency histogram. 12 boundaries + one overflow slot, the same 13 the
 * original carries (`kb-core/src/metrics.rs:31-33`). A sample at `ms` lands in
 * the first slot whose boundary it does not exceed, so slot 12 is the ">10s"
 * overflow. Counters, not locks: `observe` runs on every worker. */
#define KBC_LAT_BOUNDARIES 12u
#define KBC_LAT_SLOTS 13u

typedef struct {
  _Atomic uint64_t count;
  _Atomic uint64_t slot[KBC_LAT_SLOTS]; /* cumulative per boundary */
} route_hist;

/* `RouteKind::ALL` order (`kb-server/src/state.rs:542-552`): the exposition
 * lists the families in enum order, so the array order IS the wire order. */
#define KBC_ROUTE_KINDS 9u

/* The counters `/metrics` renders. There is no registry and no naming layer
 * in the original either (`state.rs:396-437` is a plain struct of atomics);
 * what the port owes it is the same fixed cardinality, so a family is either
 * always emitted or not emitted at all — never emitted only once it has a
 * value. */
typedef struct {
  _Atomic uint64_t total;              /* kb_http_requests_total */
  route_hist route[KBC_ROUTE_KINDS];  /* kb_route_* */
  route_hist per_kb[KBC_MAX_CORPORA]; /* pre-seeded from the kb set */
  size_t n_kb;                         /* live entries of per_kb */
  bool detailed;                       /* the `[server] metrics` layer */
  /* BORROWED from cfg->corpora, which outlives the httpd. Sorted at EMIT time
   * only: the index space stays the bring-up order, so an observation is a
   * linear scan over a fixed array instead of a lookup in a moving order. */
  const char *kb_names[KBC_MAX_CORPORA];
} metrics_reg;

/* One configured corpus and the CANONICAL path of its source root. The root
 * is canonicalised once, at bring-up: the containment guard compares against
 * it on every artifact request, and a per-request realpath(3) would make the
 * trust boundary depend on a filesystem state the request controls. */
typedef struct {
  char *name; /* KBC_OWN */
  char *root; /* KBC_OWN, realpath(3) of the corpus path, NULL when absent */
} kb_root;


struct kbc_worker {
  kbc_httpd *h;
  int idx;
  int epfd;
  int lfd;
  conn **sse; /* streaming conns this worker must service */
  size_t sse_len, sse_cap;
  conn **zomb; /* closed this batch, freed once the events are dispatched */
  size_t zomb_len, zomb_cap;
};

/* One entry of the STALE set: the (artifact_id, comment_id) pair the original
 * keys its `anchor_state` map on (indexer.rs:3036). Fixed-width, because the
 * store mints both ids through `mint_id` at KBC_MAX_ID_LEN hex characters and
 * a comment id that did not fit would be a row this pass cannot key — the
 * pass refuses it loudly rather than tracking half of it. */
typedef struct {
  char doc_id[KBC_MAX_ID_LEN + 1];
  char comment_id[KBC_MAX_ID_LEN + 1];
} anchor_key;

struct kbc_httpd {
  kbc_app *app;
  const kbc_config *cfg; /* BORROWED; the daemon outlives its httpd anyway */
  char *bind_addr;       /* KBC_OWN */
  int port;
  int workers;
  int started_threads;
  int *listen_fd;
  kbc_worker *w;
  pthread_t *threads;
  uint64_t sub_id;
  bool subscribed;

  int wake_rd;
  int wake_wr;

  atomic_int conns;
  atomic_bool stopping;
  int64_t started_ns;

  pthread_mutex_t conns_mu;
  conn **all_conns;
  size_t all_len, all_cap;

  pthread_mutex_t ring_mu;

  /* The origin split, brought up once (see section 5). The corpus roots are
   * canonicalised HERE so the per-request guard is a pure string comparison
   * (`routes/artifact.rs:597-602`), and neither getenv nor realpath runs on
   * the request path. kbc_config has no field for any of them — the config
   * keys they should become are in the port report. */
  char *host_suffix;   /* KBC_OWN, default KBC_HOST_SUFFIX_DEFAULT */
  char *parent_origin; /* KBC_OWN, default KBC_PARENT_ORIGIN_DEFAULT */
  char *spa_root;      /* KBC_OWN, canonicalised; NULL when no SPA dist */
  kb_root *roots;      /* KBC_OWN, cfg->ncorpora entries */
  size_t nroots;
  metrics_reg m;       /* lock-free: every worker writes it */
  sse_hist ring[KBC_SSE_RING_CAP];
  size_t ring_next; /* next slot to write; == the oldest when full */
  uint64_t next_id;

  /* Exact-match CORS origin allowlist, read once at start from the environment
   * because kbc_config has no field for it (see the note above KBC_ROUTES).
   * Empty means same-origin only, which is both the default and the safe one:
   * kb-c serves no web UI, so there is no legitimate cross-origin caller. */
  kbc_strlist cors;
  size_t rate_limit; /* requests per connection per second, 0 disables */
  /* Anchor re-evaluation, the state behind `comment.anchor_stale` and
   * `comment.anchor_resolved`. Guarded by `anchors_mu`, which is ALSO the
   * single-owner lock for the pass: the workers' tick takes it with trylock,
   * so N workers tick and exactly one of them scans. That is the same
   * discipline as `all_conns`/`conns_mu` — shared state, mutated from more
   * than one thread, one lock, initialised in start and destroyed in stop
   * alongside the other two. Nothing here is read without it. */
  pthread_mutex_t anchors_mu;
  anchor_key *anchors; /* KBC_OWN; the STALE set */
  size_t anchors_len, anchors_cap;
  atomic_bool anchors_dirty; /* set by `index.updated`, consumed by the pass */

};

/* -------------------------------------------------------- response bits -- */

static const char *const CT_JSON = "application/json; charset=utf-8";
static const char *const CT_TEXT = "text/plain; charset=utf-8";
static const char *const CT_SSE = "text/event-stream; charset=utf-8";
static const char *const CT_PROBLEM =
    "application/problem+json; charset=utf-8";
/* Scrapers branch on `version=0.0.4`; `charset=utf-8` is the form the
 * exposition spec names (`routes/metrics.rs:115`). */
static const char *const KBC_PROM_CONTENT_TYPE =
    "text/plain; version=0.0.4; charset=utf-8";

void kbc_response_init(kbc_response *r) {
  if (!r) return;
  r->status = 200;
  r->content_type = CT_JSON;
  kbc_str_init(&r->body);
  r->sse = false;
  r->close_after = false;
}

void kbc_response_free(kbc_response *r) {
  if (!r) return;
  kbc_str_free(&r->body);
  r->body.ptr = NULL;
  r->body.len = 0;
  r->body.cap = 0;
}

kbc_status kbc_response_json(kbc_response *r, int status, const char *json) {
  if (!r) return kbc_err_set(NULL, KBC_ERR_INVALID, "kbc_response_json: r");
  kbc_response_free(r);
  r->status = status;
  r->content_type = CT_JSON;
  return kbc_str_puts(&r->body, json ? json : "");
}

/* RFC 7807. The shape is the contract: type (stable URN), title (the reason
 * phrase for the status), status (echoed so a client that lost the status line
 * can still read it), detail (why, in the daemon's own words) and code (the
 * kbc_status name, which is what the CLI matches on). A bare {"ok":false} told
 * a client nothing it could branch on; this is what anything built against the
 * kb API expects to parse. */
kbc_status kbc_response_error_json(kbc_response *r, int status, kbc_status code,
                                   const char *msg) {
  if (!r) return kbc_err_set(NULL, KBC_ERR_INVALID, "kbc_response_error_json: r");
  kbc_response_free(r);
  r->status = status;
  r->content_type = CT_PROBLEM;
  kbc_str s;
  kbc_str_init(&s);
  kbc_status st = kbc_str_printf(&s, "{\"type\":\"%s\",\"title\":\"%s\","
                                    "\"status\":%d,\"code\":\"%s\",\"detail\":",
                                 problem_type(status), status_text(status),
                                 status, kbc_status_str(code));
  if (kbc_failed(st)) goto done;
  st = kbc_str_append_json_string(&s, msg != NULL && msg[0] != '\0'
                                            ? msg
                                            : status_text(status),
                                  msg != NULL ? strlen(msg) : 0u);
  if (kbc_failed(st)) goto done;
  st = kbc_str_putc(&s, '}');
  if (kbc_failed(st)) goto done;
  st = kbc_str_append(&r->body, s.ptr, s.len);
done:
  kbc_str_free(&s);
  return st;
}

static kbc_status resp_error(kbc_response *r, int status, kbc_status code,
                             const char *fmt, ...) {
  char msg[KBC_ERR_MSG_MAX];
  va_list ap;
  va_start(ap, fmt);
  int n = vsnprintf(msg, sizeof msg, fmt, ap);
  va_end(ap);
  if (n < 0) {
    return kbc_response_error_json(r, status, code, "message formatting failed");
  }
  return kbc_response_error_json(r, status, code, msg);
}

/* ------------------------------------------------------ (2) HTTP parsing -- */

typedef struct {
  char *name;
  const char *value;
} hdr_entry;

typedef struct {
  kbc_arena *a;
  hdr_entry hdrs[KBC_MAX_HEADERS];
  size_t nhdrs;
  char *method;
  char *raw_target;
  char *path;  /* percent-decoded, normalized, validated */
  char *query; /* percent-decoded; "" when absent */
  const char *auth;
  /* The second token carrier, named by the ORIGINAL's edge-lane contract
   * (`middleware.rs:209-212`: a traefik `kb-inject-bearer` overwrites
   * Authorization with the shared daemon token, so agents and scripts carry
   * their secret here). BORROWED from the arena-backed header table for the
   * life of the request. */
  const char *x_kb_token;
  const char *last_event_id;
  bool expect_continue;
  size_t header_end;
  size_t body_want;
  size_t body_off;
  size_t consumed;
} http_req;

/* Percent-decodes [s,s+n) into fresh arena memory. A truncated or non-hex
 * escape is an error; a decoded NUL is reported, never silently kept. */
static kbc_status pct_decode(kbc_arena *a, const char *s, size_t n, char **out,
                             bool *had_nul, kbc_err *err) {
  char *buf = kbc_arena_alloc(a, n + 1);
  if (!buf) return kbc_err_set(err, KBC_ERR_NOMEM, "pct_decode: %zu bytes", n);
  size_t o = 0;
  *had_nul = false;
  for (size_t i = 0; i < n; i++) {
    unsigned char c = (unsigned char)s[i];
    if (c == '%') {
      if (i + 2 >= n) {
        return kbc_err_set(err, KBC_ERR_PARSE,
                           "truncated percent escape at offset %zu", i);
      }
      int hi = hex_val((unsigned char)s[i + 1]);
      int lo = hex_val((unsigned char)s[i + 2]);
      if (hi < 0 || lo < 0) {
        return kbc_err_set(err, KBC_ERR_PARSE,
                           "bad percent escape \"%c%c\" at offset %zu", s[i + 1],
                           s[i + 2], i);
      }
      c = (unsigned char)((hi << 4) | lo);
      i += 2;
    }
    if (c == 0) *had_nul = true;
    buf[o++] = (char)c;
  }
  buf[o] = '\0';
  *out = buf;
  return KBC_OK;
}

/* ------------------------------------------------------------- query ----- */

/* Hand-rolled: split on '&' then '=', percent-decode both sides, no regex.
 * The first occurrence of a key wins. */
static bool query_pair(const char *query, const char *key, const char **vs,
                       size_t *vn) {
  if (!query) return false;
  size_t klen = strlen(key);
  const char *p = query;
  while (*p != '\0') {
    const char *amp = strchr(p, '&');
    size_t seglen = amp ? (size_t)(amp - p) : strlen(p);
    const char *eq = (const char *)memchr(p, '=', seglen);
    if (eq != NULL && (size_t)(eq - p) == klen &&
        memcmp(p, key, klen) == 0) {
      *vs = eq + 1;
      *vn = seglen - (size_t)(eq - p) - 1;
      return true;
    }
    p = amp ? amp + 1 : p + seglen;
  }
  return false;
}

static bool query_has(const char *query, const char *key) {
  const char *vs;
  size_t vn;
  return query_pair(query, key, &vs, &vn);
}

static kbc_status query_get(kbc_arena *a, const char *query, const char *key,
                            const char **out, kbc_err *err) {
  *out = NULL;
  const char *vs;
  size_t vn;
  if (!query_pair(query, key, &vs, &vn)) return KBC_OK;
  bool nul = false;
  char *dec = NULL;
  kbc_status st = pct_decode(a, vs, vn, &dec, &nul, err);
  if (kbc_failed(st)) return st;
  if (nul) {
    return kbc_err_set(err, KBC_ERR_PARSE,
                       "query parameter \"%s\" contains a NUL after decoding",
                       key);
  }
  *out = dec;
  return KBC_OK;
}

/* Strict non-negative integer, or -1 when absent or malformed, so the caller
 * can tell "reject me" from "use the default". */
static int64_t query_int(const char *query, const char *key) {
  const char *vs;
  size_t vn;
  if (!query_pair(query, key, &vs, &vn) || vn == 0 || vn > 18) return -1;
  int64_t acc = 0;
  for (size_t i = 0; i < vn; i++) {
    unsigned char c = (unsigned char)vs[i];
    if (c < '0' || c > '9') return -1;
    acc = acc * 10 + (int64_t)(c - '0');
  }
  return acc;
}

/* A boolean query flag, which is not an integer: the original's `?cm=on`
 * accepts exactly `on`, `1` and `true` and nothing else
 * (`routes/artifact.rs:933-940`). Reading it with query_int answers -1 for
 * "on" and would quietly turn the annotator payload off. */
static bool query_flag(const char *query, const char *key) {
  const char *vs;
  size_t vn;
  if (!query_pair(query, key, &vs, &vn)) return false;
  return (vn == 2 && vs[0] == 'o' && vs[1] == 'n') ||
         (vn == 1 && vs[0] == '1') ||
         (vn == 4 && memcmp(vs, "true", 4) == 0);
}

/* Decodes, then rejects NUL / '\' / "..", collapses '//' and drops a trailing
 * '/'. Runs before the router and before anything reaches the filesystem. */
static kbc_status path_normalize(kbc_arena *a, const char *raw, char **out,
                                 kbc_err *err) {
  bool had_nul = false;
  char *dec = NULL;
  kbc_status st = pct_decode(a, raw, strlen(raw), &dec, &had_nul, err);
  if (kbc_failed(st)) return st;
  if (had_nul) {
    return kbc_err_set(err, KBC_ERR_PARSE, "path contains a NUL after decoding");
  }
  size_t n = strlen(dec);
  char *norm = kbc_arena_alloc(a, n + 2);
  if (!norm) return kbc_err_set(err, KBC_ERR_NOMEM, "path_normalize: %zu", n);
  size_t o = 0;
  norm[o++] = '/';
  for (size_t i = 0; i < n; i++) {
    unsigned char c = (unsigned char)dec[i];
    if (c < 0x20u || c == 0x7fu) {
      return kbc_err_set(err, KBC_ERR_PARSE,
                         "path contains control byte 0x%02x", c);
    }
    if (c == '\\') {
      return kbc_err_set(err, KBC_ERR_PARSE, "path contains a backslash");
    }
    if (c == '.' && dec[i + 1] == '.' &&
        (i + 2 == n || dec[i + 2] == '/')) {
      return kbc_err_set(err, KBC_ERR_PARSE, "path contains a \"..\" segment");
    }
    if (c == '/') {
      while (i + 1 < n && dec[i + 1] == '/') i++;
      if (i + 1 == n) break; /* trailing slash: drop it */
      if (o == 1) continue;  /* the leading '/' is already written */
    }
    if (o + 2 > n + 2) {
      return kbc_err_set(err, KBC_ERR_PARSE, "path normalization overflow");
    }
    norm[o++] = (char)c;
  }
  /* Only an EMPTY target needs the root written here. The loop already wrote
   * the leading '/' before it, so a target of "/" left `o == 1` on the
   * trailing-slash break above and this used to append a second one, turning
   * "GET /" into the path "//" — which matched no route and fell through to the
   * origin fallback. The condition is on the INPUT, not on the output. */
  if (n == 0) norm[o++] = '/';
  norm[o] = '\0';
  *out = norm;
  return KBC_OK;
}

static const char *hdr_find(const http_req *r, const char *name) {
  for (size_t i = 0; i < r->nhdrs; i++) {
    if (str_ieq(r->hdrs[i].name, name)) return r->hdrs[i].value;
  }
  return NULL;
}

/* Case-insensitive compare of a header value against a literal, ignoring the
 * optional whitespace around it and without mutating the const value. */
static bool value_ieq(const char *v, const char *want) {
  if (v == NULL) return false;
  while (*v == ' ' || *v == '\t') v++;
  size_t n = strlen(v);
  while (n > 0 && (v[n - 1] == ' ' || v[n - 1] == '\t')) n--;
  size_t wn = strlen(want);
  if (n != wn) return false;
  for (size_t i = 0; i < n; i++) {
    unsigned char a = (unsigned char)v[i];
    unsigned char b = (unsigned char)want[i];
    if (a >= 'A' && a <= 'Z') a = (unsigned char)(a + 32);
    if (b >= 'A' && b <= 'Z') b = (unsigned char)(b + 32);
    if (a != b) return false;
  }
  return true;
}

static char *trim_ws(char *s) {
  while (*s == ' ' || *s == '\t') s++;
  size_t n = strlen(s);
  while (n > 0 && (s[n - 1] == ' ' || s[n - 1] == '\t')) s[--n] = '\0';
  return s;
}

static bool valid_method(const char *m) {
  if (m[0] == '\0') return false;
  for (size_t i = 0; m[i]; i++) {
    unsigned char c = (unsigned char)m[i];
    if (c < 'A' || c > 'Z') return false;
  }
  return true;
}

/* First half of parsing: the request line and the header table.
 * `*http_status` is the status to answer with when the status is an error;
 * header_end == 0 on return means "need more bytes". Never reads past
 * buf + len. */
static kbc_status req_parse(http_req *r, const char *buf, size_t len,
                            int *http_status, kbc_err *err) {
  *http_status = 400;
  const char *end = buf + len;
  const char *hend = NULL;
  for (const char *p = buf; p + 3 < end; p++) {
    if (p[0] == '\r' && p[1] == '\n' && p[2] == '\r' && p[3] == '\n') {
      hend = p + 4;
      break;
    }
  }
  if (hend == NULL) {
    if (len > KBC_HTTP_MAX_REQUEST_LINE + KBC_HTTP_MAX_HEADER_BYTES) {
      return kbc_err_set(err, KBC_ERR_PARSE,
                         "header block exceeds %u bytes with no CRLFCRLF",
                         KBC_HTTP_MAX_REQUEST_LINE + KBC_HTTP_MAX_HEADER_BYTES);
    }
    return KBC_OK; /* need more */
  }
  size_t header_end = (size_t)(hend - buf);
  if (header_end > KBC_HTTP_MAX_HEADER_BYTES) {
    return kbc_err_set(err, KBC_ERR_PARSE, "header block is %zu bytes, max %u",
                       header_end, KBC_HTTP_MAX_HEADER_BYTES);
  }

  /* --- request line ------------------------------------------------- */
  size_t line_len = 0;
  while (line_len < header_end && buf[line_len] != '\n') line_len++;
  if (line_len > KBC_HTTP_MAX_REQUEST_LINE) {
    *http_status = 414;
    return kbc_err_set(err, KBC_ERR_PARSE, "request line is %zu bytes, max %u",
                       line_len, KBC_HTTP_MAX_REQUEST_LINE);
  }
  char *rl = kbc_arena_strndup(r->a, buf, line_len);
  if (rl == NULL) return kbc_err_set(err, KBC_ERR_NOMEM, "req_parse: line");
  while (rl[0] != '\0' &&
         (rl[strlen(rl) - 1] == '\r' || rl[strlen(rl) - 1] == ' ')) {
    rl[strlen(rl) - 1] = '\0';
  }
  char *sp1 = strchr(rl, ' ');
  if (sp1 == NULL) goto bad_line;
  *sp1 = '\0';
  char *rest = sp1 + 1;
  while (*rest == ' ') rest++;
  char *sp2 = strchr(rest, ' ');
  if (sp2 == NULL) goto bad_line;
  *sp2 = '\0';
  char *ver = sp2 + 1;
  while (*ver == ' ') ver++;
  if (!valid_method(rl) || rest[0] != '/' || strcmp(ver, "HTTP/1.1") != 0) {
    goto bad_line;
  }
  r->method = rl;
  r->raw_target = rest;

  /* --- headers ------------------------------------------------------ */
  const char *p = buf + line_len + 1;
  while (p < hend - 2) {
    const char *eol = p;
    while (eol < hend - 2 && !(eol[0] == '\r' && eol[1] == '\n')) eol++;
    if (eol == p) break;
    size_t hl = (size_t)(eol - p);
    if (r->nhdrs >= KBC_MAX_HEADERS) {
      return kbc_err_set(err, KBC_ERR_PARSE, "more than %u headers",
                         KBC_MAX_HEADERS);
    }
    char *hcopy = kbc_arena_strndup(r->a, p, hl);
    if (hcopy == NULL) return kbc_err_set(err, KBC_ERR_NOMEM, "req_parse: hdr");
    char *colon = strchr(hcopy, ':');
    if (colon == NULL || colon == hcopy) {
      return kbc_err_set(err, KBC_ERR_PARSE, "malformed header line");
    }
    *colon = '\0';
    for (char *q = hcopy; *q; q++) {
      unsigned char ch = (unsigned char)*q;
      if (ch <= 0x20u || ch >= 0x7fu) {
        return kbc_err_set(err, KBC_ERR_PARSE, "illegal byte in header name");
      }
    }
    r->hdrs[r->nhdrs].name = hcopy;
    r->hdrs[r->nhdrs].value = trim_ws(colon + 1);
    r->nhdrs++;
    p = eol + 2;
  }

  if (hdr_find(r, "Transfer-Encoding") != NULL) {
    return kbc_err_set(err, KBC_ERR_PARSE,
                       "Transfer-Encoding is not supported: this daemon reads "
                       "Content-Length bodies only");
  }

  /* --- Content-Length: digits, at most one, within the body ceilings --- */
  size_t clen = 0;
  int n_cl = 0;
  for (size_t i = 0; i < r->nhdrs; i++) {
    if (!str_ieq(r->hdrs[i].name, "Content-Length")) continue;
    n_cl++;
    const char *v = r->hdrs[i].value;
    if (v[0] == '\0') {
      return kbc_err_set(err, KBC_ERR_PARSE, "empty Content-Length");
    }
    unsigned long long acc = 0;
    for (const char *d = v; *d; d++) {
      if (*d < '0' || *d > '9') {
        return kbc_err_set(err, KBC_ERR_PARSE, "non-numeric Content-Length");
      }
      acc = acc * 10ull + (unsigned long long)(*d - '0');
      if (acc > (unsigned long long)KBC_HTTP_MAX_BODY_BYTES) {
        *http_status = 413;
        return kbc_err_set(err, KBC_ERR_PARSE, "Content-Length exceeds %u bytes",
                           KBC_HTTP_MAX_BODY_BYTES);
      }
    }
    clen = (size_t)acc;
  }
  if (n_cl > 1) {
    return kbc_err_set(err, KBC_ERR_PARSE, "duplicate Content-Length header");
  }
  /* The read buffer is bounded by KBC_MAX_SNIFF_BYTES of body, so a larger
   * declared body is refused before a byte of it is accepted. */
  if (clen > KBC_MAX_SNIFF_BYTES) {
    *http_status = 413;
    return kbc_err_set(err, KBC_ERR_PARSE,
                       "Content-Length %zu exceeds the %u byte body ceiling",
                       clen, KBC_MAX_SNIFF_BYTES);
  }

  r->header_end = header_end;
  r->body_want = clen;
  r->body_off = header_end;
  r->consumed = header_end + clen;
  r->auth = hdr_find(r, "Authorization");
  r->x_kb_token = hdr_find(r, "X-Kb-Token");
  r->last_event_id = hdr_find(r, "Last-Event-ID");
  r->expect_continue = value_ieq(hdr_find(r, "Expect"), "100-continue");
  return KBC_OK;

bad_line:
  return kbc_err_set(err, KBC_ERR_PARSE,
                     "malformed request line (want \"METHOD /path HTTP/1.1\")");
}

/* Second half, once the body bytes are in the buffer: decode the target, split
 * the query, validate the path. */
static kbc_status req_finish(http_req *r, int *http_status, kbc_err *err) {
  *http_status = 400;
  const char *qm = strchr(r->raw_target, '?');
  char *raw_path = r->raw_target;
  if (qm != NULL) {
    raw_path = kbc_arena_strndup(r->a, r->raw_target,
                                 (size_t)(qm - r->raw_target));
    if (raw_path == NULL) return kbc_err_set(err, KBC_ERR_NOMEM, "path copy");
  }
  kbc_status st = path_normalize(r->a, raw_path, &r->path, err);
  if (kbc_failed(st)) return st;
  r->query = kbc_arena_strdup(r->a, "");
  if (r->query == NULL) return kbc_err_set(err, KBC_ERR_NOMEM, "query copy");
  if (qm != NULL) {
    bool nul = false;
    char *q = NULL;
    st = pct_decode(r->a, qm + 1, strlen(qm + 1), &q, &nul, err);
    if (kbc_failed(st)) return st;
    if (nul) {
      return kbc_err_set(err, KBC_ERR_PARSE,
                         "query contains a NUL after decoding");
    }
    r->query = q;
  }
  return KBC_OK;
}

/* ------------------------------------------------------ (3) the routes ---- */

static kbc_status route_health(kbc_app *app, kbc_str *out, int64_t uptime_s) {
  kbc_app_stats st;
  memset(&st, 0, sizeof st);
  if (app != NULL) {
    kbc_err local;
    kbc_err_reset(&local);
    (void)kbc_app_stats_get(app, &st, &local);
  }
  kbc_status rc = kbc_str_puts(out, "{\"status\":\"ok\",\"version\":\"");
  if (kbc_failed(rc)) return rc;
  rc = kbc_str_puts(out, KBC_VERSION);
  if (kbc_failed(rc)) return rc;
  return kbc_str_printf(out, "\",\"uptime_s\":%lld,\"docs\":%lld}",
                        (long long)uptime_s, (long long)st.index_docs);
}

static kbc_status route_stats(kbc_app *app, kbc_str *out, kbc_err *err) {
  kbc_app_stats st;
  memset(&st, 0, sizeof st);
  kbc_status rc = kbc_app_stats_get(app, &st, err);
  if (kbc_failed(rc)) return rc;
  return kbc_str_printf(
      out,
      "{\"artifacts_indexed\":%lld,\"reindex_runs\":%lld,"
      "\"searches_served\":%lld,\"searches_degraded\":%lld,"
      "\"last_reindex_ns\":%lld,\"last_reindex_docs\":%lld,"
      "\"last_reindex_us\":%lld,\"index_terms\":%lld,\"index_docs\":%lld,"
      "\"db_bytes\":%lld}",
      (long long)st.artifacts_indexed, (long long)st.reindex_runs,
      (long long)st.searches_served, (long long)st.searches_degraded,
      (long long)st.last_reindex_ns, (long long)st.last_reindex_docs,
      (long long)st.last_reindex_us, (long long)st.index_terms,
      (long long)st.index_docs, (long long)st.db_bytes);
}

/* Defined with the serving surface, below: the title of a `.md` comes from the
 * renderer, and the renderer is not needed to serve bytes. */
static bool is_markdown(const char *path);
static const char *markdown_title_for(kbc_arena *a, const char *src,
                                      size_t len, const char *fallback);

/* The title to report for `art`, which for a `.md` is the RENDERER's and not
 * the store's (see `markdown_title_for` for why the two disagree).
 *
 * `art->source` is only populated when the caller asked for it, and the
 * artifact LIST never does. So a `.md` row in a list page has no source to
 * read a title from, and this fetches one — into a THROWAWAY arena, freed
 * before this returns. That bound is the whole reason it is a separate arena:
 * a page of 50 markdown rows at the 16 MiB artifact ceiling would otherwise
 * pin 800 MiB of source in the request arena to extract 50 short strings.
 * Peak is one document.
 *
 * A fetch that fails, or a document the renderer will not title, leaves the
 * store's title standing: a list row is a summary of the store's record, and
 * one unreadable document must not empty the page. */

/* The renderer's name for a document it could not name. `kbc_markdown_title`
 * returns exactly this string, and exactly this string, when the source has
 * no frontmatter `title:` and no `# ` heading (markdown.c:919). It is
 * compared rather than assumed, because it is the one place the two title
 * rules meet: everywhere else a title is a title, and here it is the
 * difference between naming a document and not naming it. */
#define KBC_UNTITLED "Untitled"

/* A document's own name when neither the renderer nor the store has one: the
 * filename stem, which is what the original's indexer falls back to
 * (indexer.rs:2705, `path.file_stem()`). ARENA. */
static const char *title_from_stem(kbc_arena *a, const char *path) {
  const char *base = path != NULL ? strrchr(path, '/') : NULL;
  base = base != NULL ? base + 1 : path;
  if (base == NULL) return NULL;
  const char *dot = strrchr(base, '.');
  size_t n = (dot != NULL && dot != base) ? (size_t)(dot - base) : strlen(base);
  if (n == 0 || n > 200) return NULL;
  char buf[208];
  memcpy(buf, base, n);
  buf[n] = '\0';
  return kbc_arena_strdup(a, buf);
}

/* One title per document, and never the word "Untitled" where a name was
 * available: the renderer produced no name at all, so the store's title and
 * the renderer's agree that this document is unnamed, and the filename stem
 * is the only name either of them has. The original's indexer falls back the
 * same way (indexer.rs:2705) and a corpus of hook and skill files — no
 * frontmatter `title:`, no `# ` heading, all of them named by their filename
 * — is listed, bookmarked and commented on under that stem rather than under
 * one indistinguishable row per document. */
static const char *named_title_for(kbc_arena *a, const char *renderer,
                                   const char *path) {
  if (renderer == NULL || strcmp(renderer, KBC_UNTITLED) != 0) {
    return renderer;
  }
  const char *stem = title_from_stem(a, path);
  return stem != NULL ? stem : renderer;
}
static const char *artifact_title_for(kbc_app *app, kbc_arena *a,
                                      const kbc_artifact *art) {
  const char *stored = art->title != NULL ? art->title : "";
  if (art->path == NULL || !is_markdown(art->path)) return stored;
  if (art->source != NULL) {
    return named_title_for(a, markdown_title_for(a, art->source,
                                                 strlen(art->source), stored),
                           art->path);
  }
  kbc_arena *scratch = kbc_arena_new(4096);
  if (scratch == NULL) return stored;
  kbc_artifact full;
  memset(&full, 0, sizeof full);
  kbc_err local;
  kbc_err_reset(&local);
  const char *t = stored;
  if (!kbc_failed(kbc_app_get_artifact(app, scratch, art->id, true, &full,
                                       &local)) &&
      full.source != NULL) {
    t = named_title_for(a, markdown_title_for(a, full.source,
                                               strlen(full.source), stored),
                        art->path);
  }
  kbc_arena_free(scratch);
  return t;
}

static kbc_status artifact_json(kbc_str *out, kbc_app *app, kbc_arena *a,
                                const kbc_artifact *art, bool with_source) {
  /* Resolved once, up front: for a `.md` this is the renderer's title, not
   * the store's, so that a list row and the page it links to name the document
   * the same way. `app` and `a` are passed in rather than read off `art`
   * because `kbc_artifact` is a frozen record of the store's row. */
  const char *title = artifact_title_for(app, a, art);
  kbc_status st = kbc_str_puts(out, "{\"id\":");
  if (kbc_failed(st)) return st;
  st = kbc_str_append_json_string(out, art->id, strlen(art->id));
  if (kbc_failed(st)) return st;
  st = kbc_str_printf(out, ",\"kind\":\"%s\",\"mtime_ns\":%lld,"
                           "\"size_bytes\":%lld,\"content_hash\":%lu,"
                           "\"heading_count\":%d,\"corpus\":",
                      kbc_kind_str(art->kind), (long long)art->mtime_ns,
                      (long long)art->size_bytes,
                      (unsigned long)art->content_hash,
                      (int)art->heading_count);
  if (kbc_failed(st)) return st;
  st = kbc_str_append_json_string(out, art->corpus, strlen(art->corpus));
  if (kbc_failed(st)) return st;
  st = kbc_str_puts(out, ",\"path\":");
  if (kbc_failed(st)) return st;
  st = kbc_str_append_json_string(out, art->path, strlen(art->path));
  if (kbc_failed(st)) return st;
  st = kbc_str_puts(out, ",\"title\":");
  if (kbc_failed(st)) return st;
  st = kbc_str_append_json_string(out, title, strlen(title));
  if (kbc_failed(st)) return st;
  st = kbc_str_puts(out, ",\"summary\":");
  if (kbc_failed(st)) return st;
  st = kbc_str_append_json_string(out, art->summary ? art->summary : "",
                                  art->summary ? strlen(art->summary) : 0);
  if (kbc_failed(st)) return st;
  if (with_source) {
    st = kbc_str_puts(out, ",\"source\":");
    if (kbc_failed(st)) return st;
    st = art->source != NULL
             ? kbc_str_append_json_string(out, art->source, strlen(art->source))
             : kbc_str_puts(out, "null");
    if (kbc_failed(st)) return st;
  }
  return kbc_str_putc(out, '}');
}

/* Parses limit/offset once, so search and list agree on what a bad value is. */
static kbc_status query_window(const char *query, size_t dflt_limit,
                               size_t *limit, size_t *offset, kbc_err *err) {
  *limit = dflt_limit != 0 ? dflt_limit : 50u;
  int64_t lim = query_int(query, "limit");
  if (query_has(query, "limit")) {
    if (lim < 1) {
      return kbc_err_set(err, KBC_ERR_INVALID, "limit must be an integer >= 1");
    }
    *limit = (size_t)lim;
  }
  if (*limit > KBC_MAX_HITS) *limit = KBC_MAX_HITS;
  /* An absent offset means the first page. query_int() cannot tell "absent"
   * from "present but not a number" — both return -1 — so presence is asked
   * for separately and only a value the client actually sent is copied. A
   * present offset that is negative is a client error; one past the store's
   * ceiling is rejected downstream, not silently clamped here. */
  *offset = 0;
  if (query_has(query, "offset")) {
    int64_t off = query_int(query, "offset");
    if (off < 0) {
      return kbc_err_set(err, KBC_ERR_INVALID,
                         "offset must be an integer >= 0");
    }
    *offset = (size_t)off;
  }
  return KBC_OK;
}

static kbc_status route_search(kbc_app *app, const kbc_config *cfg,
                               kbc_arena *a, const char *query, kbc_str *out,
                               kbc_err *err) {
  kbc_query kq;
  memset(&kq, 0, sizeof kq);
  kbc_status st = query_get(a, query, "q", &kq.q, err);
  if (kbc_failed(st)) return st;
  if (kq.q == NULL || kq.q[0] == '\0') {
    return kbc_err_set(err, KBC_ERR_INVALID, "missing required query \"q\"");
  }
  if (strlen(kq.q) > KBC_MAX_QUERY_LEN) {
    return kbc_err_set(err, KBC_ERR_INVALID, "q is %zu bytes, max %u",
                       strlen(kq.q), KBC_MAX_QUERY_LEN);
  }
  st = query_get(a, query, "kb", &kq.corpus, err);
  if (kbc_failed(st)) return st;
  st = query_get(a, query, "path", &kq.path_prefix, err);
  if (kbc_failed(st)) return st;

  kq.kind = KBC_KIND__COUNT;
  const char *kind_s = NULL;
  st = query_get(a, query, "kind", &kind_s, err);
  if (kbc_failed(st)) return st;
  if (kind_s != NULL && kind_s[0] != '\0') {
    kbc_kind k;
    kbc_err local;
    kbc_err_reset(&local);
    if (kbc_failed(kbc_kind_from_str(kind_s, &k, &local))) {
      return kbc_err_set(err, KBC_ERR_INVALID, "unknown kind \"%s\"", kind_s);
    }
    kq.kind = k;
  }
  const char *mode_s = NULL;
  st = query_get(a, query, "mode", &mode_s, err);
  if (kbc_failed(st)) return st;
  kq.mode = KBC_MODE_HYBRID;
  if (mode_s != NULL && mode_s[0] != '\0') {
    kbc_search_mode m;
    if (!kbc_search_mode_parse(mode_s, &m)) {
      return kbc_err_set(err, KBC_ERR_INVALID, "unknown mode \"%s\"", mode_s);
    }
    kq.mode = m;
  }

  size_t limit = 0;
  size_t offset = 0;
  st = query_window(query, cfg != NULL ? cfg->search_max_hits : 50u, &limit,
                    &offset, err);
  if (kbc_failed(st)) return st;
  kq.limit = limit;
  kq.rrf_k = cfg != NULL ? cfg->rrf_k : 60;

  kbc_search_result res;
  memset(&res, 0, sizeof res);
  st = kbc_app_search(app, a, &kq, &res, err);
  if (kbc_failed(st)) return st;

  size_t emitted = 0;
  st = kbc_str_puts(out, "{\"results\":[");
  if (kbc_failed(st)) return st;
  for (size_t i = offset; i < res.len && emitted < limit; i++) {
    const kbc_result_row *row = &res.rows[i];
    if (emitted > 0) {
      st = kbc_str_putc(out, ',');
      if (kbc_failed(st)) return st;
    }
    st = kbc_str_printf(out, "{\"doc_id\":%lu,\"score\":%.6f,"
                             "\"keyword_score\":%.6f,\"vector_score\":%.6f,"
                             "\"keyword_rank\":%ld,\"vector_rank\":%ld,"
                             "\"artifact_id\":",
                        (unsigned long)row->doc_id, row->score,
                        row->keyword_score, row->vector_score,
                        (long)row->keyword_rank, (long)row->vector_rank);
    if (kbc_failed(st)) return st;
    st = row->artifact_id != NULL
             ? kbc_str_append_json_string(out, row->artifact_id,
                                         strlen(row->artifact_id))
             : kbc_str_puts(out, "null");
    if (kbc_failed(st)) return st;
    st = kbc_str_puts(out, ",\"corpus\":");
    if (kbc_failed(st)) return st;
    st = kbc_str_append_json_string(out, row->corpus ? row->corpus : "",
                                    row->corpus ? strlen(row->corpus) : 0);
    if (kbc_failed(st)) return st;
    st = kbc_str_puts(out, ",\"path\":");
    if (kbc_failed(st)) return st;
    st = kbc_str_append_json_string(out, row->path ? row->path : "",
                                    row->path ? strlen(row->path) : 0);
    if (kbc_failed(st)) return st;
    st = kbc_str_puts(out, ",\"title\":");
    if (kbc_failed(st)) return st;
    st = kbc_str_append_json_string(out, row->title ? row->title : "",
                                    row->title ? strlen(row->title) : 0);
    if (kbc_failed(st)) return st;
    st = kbc_str_puts(out, ",\"summary\":");
    if (kbc_failed(st)) return st;
    st = row->summary != NULL
             ? kbc_str_append_json_string(out, row->summary, strlen(row->summary))
             : kbc_str_puts(out, "null");
    if (kbc_failed(st)) return st;
    st = kbc_str_putc(out, '}');
    if (kbc_failed(st)) return st;
    emitted++;
  }
  return kbc_str_printf(
      out, "],\"took_us\":%lld,\"candidates\":%zu,\"degraded\":%s,\"limit\":%zu,"
           "\"offset\":%zu,\"count\":%zu}",
      (long long)res.took_us, res.candidates, res.degraded ? "true" : "false",
      limit, offset, emitted);
}

static kbc_status route_artifacts(kbc_app *app, kbc_arena *a, const char *query,
                                  kbc_str *out, kbc_err *err) {
  const char *kb = NULL;
  kbc_status st = query_get(a, query, "kb", &kb, err);
  if (kbc_failed(st)) return st;
  kbc_kind kind = KBC_KIND__COUNT;
  const char *kind_s = NULL;
  st = query_get(a, query, "kind", &kind_s, err);
  if (kbc_failed(st)) return st;
  if (kind_s != NULL && kind_s[0] != '\0') {
    kbc_err local;
    kbc_err_reset(&local);
    if (kbc_failed(kbc_kind_from_str(kind_s, &kind, &local))) {
      return kbc_err_set(err, KBC_ERR_INVALID, "unknown kind \"%s\"", kind_s);
    }
  }
  size_t limit = 0;
  size_t offset = 0;
  st = query_window(query, 50u, &limit, &offset, err);
  if (kbc_failed(st)) return st;

  kbc_artifact *rows = NULL;
  size_t n = 0;
  st = kbc_app_list_artifacts(app, a, kb, kind, limit, offset, &rows, &n, err);
  if (kbc_failed(st)) return st;
  st = kbc_str_puts(out, "{\"artifacts\":[");
  if (kbc_failed(st)) return st;
  for (size_t i = 0; i < n; i++) {
    if (i > 0) {
      st = kbc_str_putc(out, ',');
      if (kbc_failed(st)) return st;
    }
    st = artifact_json(out, app, a, &rows[i], false);
    if (kbc_failed(st)) return st;
  }
  return kbc_str_printf(out, "],\"total\":%zu,\"limit\":%zu,\"offset\":%zu}",
                        n, limit, offset);
}

static kbc_status route_artifact_one(kbc_app *app, kbc_arena *a, const char *id,
                                     const char *query, kbc_str *out,
                                     kbc_err *err) {
  if (strlen(id) != KBC_MAX_ID_LEN || !kbc_id_is_valid(id)) {
    return kbc_err_set(err, KBC_ERR_INVALID,
                       "artifact id \"%s\" is not %u lowercase hex chars", id,
                       KBC_MAX_ID_LEN);
  }
  bool with_source = query_int(query, "source") == 1;
  kbc_artifact art;
  memset(&art, 0, sizeof art);
  kbc_status st = kbc_app_get_artifact(app, a, id, with_source, &art, err);
  if (kbc_failed(st)) return st;
  return artifact_json(out, app, a, &art, with_source);
}

/* Defined with the event plumbing, below: the re-index has just committed, so
 * this is the one place the anchor pass can run where its events are in the
 * ring before the response is written. */
static void anchors_run(kbc_httpd *h);

static kbc_status route_reindex(kbc_app *app, kbc_httpd *h, kbc_str *out,
                                kbc_err *err) {
  int64_t t0 = kbc_now_ns();
  kbc_status st = kbc_app_reindex(app, err);
  if (kbc_failed(st)) return st;
  int64_t took_us = (kbc_now_ns() - t0) / 1000;
  /* The re-index's own `index.updated` has already been published and fanned
   * out by the time this returns, so the pass below is not racing the fan-out
   * and its events are already in the ring. That is the whole reason this
   * route runs the pass synchronously and the worker tick only has to cover
   * the watcher, which has no route. */
  anchors_run(h);
  kbc_app_stats stats;
  memset(&stats, 0, sizeof stats);
  kbc_err local;
  kbc_err_reset(&local);
  (void)kbc_app_stats_get(app, &stats, &local);
  return kbc_str_printf(out, "{\"docs\":%lld,\"took_us\":%lld}",
                        (long long)stats.last_reindex_docs,
                        (long long)took_us);
}

/* ------------------------------------------------- identity (attribution) --
 *
 * This port has ONE trust tier, by design. It binds loopback unless a token is
 * configured, and a token names the single operator of this daemon — there is
 * no user registry, no roles, no ACLs and no per-user tokens, because there is
 * nothing here they would gate. So identity is ATTRIBUTION (who the daemon
 * thinks is on the other end of this request), never AUTHORIZATION: the same
 * identity gets the same routes whatever it is, and knowing it grants nothing.
 * The response says so in the body, so a client cannot mistake the answer for
 * a capability it may spend.
 *
 * The ladder below resolves three tiers, not the original's four. Two of the
 * original's need configuration this port does not have and are NOT built
 * rather than faked: the multi-user `Token` registry (`auth.tokens`, one
 * secret per username) and the `Header` tier (a trusted proxy's identity
 * header, `resolve_identity` step 2, middleware.rs:337-355). Honouring a fixed
 * `Remote-User` with no way to switch it off would impose a proxy-trust
 * decision on every operator instead of letting them make one; the exact
 * `kbc_config` fields that would lift that ceiling are in the port report. */
typedef enum {
  TIER_OPEN = 0,     /* no token configured: admission needed nothing */
  TIER_LOOPBACK,     /* admitted because the peer is on the loopback */
  TIER_TOKEN,        /* admitted by presenting the configured token */
} auth_tier;

/* Which carrier the winning secret arrived on. The two carriers are NOT
 * required to agree and a disagreement is NOT an error — the rule lives in
 * first_matching_carrier, where it is written down rather than implied. */
typedef enum {
  CARRIER_NONE = 0,
  CARRIER_AUTHORIZATION,
  CARRIER_X_KB_TOKEN,
} auth_carrier;

/* Fails closed: an absent, empty or unparsable address is NOT loopback, and
 * no proxy header is consulted (P7 — a spoofed X-Forwarded-For must never
 * buy loopback). */
static bool addr_is_loopback(const char *addr) {
  if (addr == NULL || addr[0] == '\0' || addr[0] == '?') return false;
  if (strcmp(addr, "::1") == 0) return true;
  struct in_addr v4;
  memset(&v4, 0, sizeof v4);
  if (inet_pton(AF_INET, addr, &v4) == 1) {
    return (ntohl(v4.s_addr) >> 24) == 127u;
  }
  return false;
}

static const char *tier_source(auth_tier t) {
  switch (t) {
  case TIER_TOKEN: return "token";
  case TIER_LOOPBACK: return "loopback";
  default: return "unresolved";
  }
}

static const char *tier_identity(auth_tier t) {
  switch (t) {
  case TIER_TOKEN: return "operator";
  case TIER_LOOPBACK: return "local";
  default: return "unattributed";
  }
}

static const char *carrier_name(auth_carrier c) {
  switch (c) {
  case CARRIER_AUTHORIZATION: return "authorization";
  case CARRIER_X_KB_TOKEN: return "x-kb-token";
  default: return "none";
  }
}

/* A corpus name arrives from the query string and is only ever COMPARED, never
 * used to build a path; the shape checks below exist so that a traversal or an
 * over-long value is refused outright instead of quietly matching nothing. */
static kbc_status corpus_filter(kbc_arena *a, const char *query,
                                const kbc_config *cfg, const char **out,
                                kbc_err *err) {
  *out = NULL;
  if (!query_has(query, "kb")) return KBC_OK;
  kbc_status st = query_get(a, query, "kb", out, err);
  if (kbc_failed(st)) return st;
  if (*out == NULL || (*out)[0] == '\0') {
    return kbc_err_set(err, KBC_ERR_INVALID, "kb must name a configured corpus");
  }
  size_t n = strlen(*out);
  if (n > 256u) {
    return kbc_err_set(err, KBC_ERR_INVALID,
                       "kb is %zu bytes, max 256", n);
  }
  for (size_t i = 0; i < n; i++) {
    unsigned char ch = (unsigned char)(*out)[i];
    if (ch < 0x20u || ch == 0x7fu || ch == '/' || ch == '\\') {
      return kbc_err_set(err, KBC_ERR_INVALID,
                         "kb contains a character no corpus name may hold");
    }
  }
  if (strstr(*out, "..") != NULL) {
    return kbc_err_set(err, KBC_ERR_INVALID, "kb must not contain \"..\"");
  }
  if (kbc_config_corpus(cfg, *out) == NULL) {
    return kbc_err_set(err, KBC_ERR_NOTFOUND,
                       "no corpus named \"%s\" is configured", *out);
  }
  return KBC_OK;
}

/* GET /api/kbs — the corpora this daemon was configured with, one row each.
 * `docs` is the number of artifact rows stored for that corpus, counted through
 * the public listing API, which clamps at KBC_MAX_HITS; a corpus at the clamp
 * says so in `docs_truncated` rather than reporting a fake total. */
static kbc_status route_kbs(kbc_app *app, kbc_arena *a, const char *query,
                            const kbc_config *cfg, kbc_str *out,
                            kbc_err *err) {
  const char *only = NULL;
  kbc_status st = corpus_filter(a, query, cfg, &only, err);
  if (kbc_failed(st)) return st;

  kbc_app_stats stats;
  memset(&stats, 0, sizeof stats);
  st = kbc_app_stats_get(app, &stats, err);
  if (kbc_failed(st)) return st;

  size_t n = cfg != NULL ? cfg->ncorpora : 0u;
  st = kbc_str_puts(out, "{\"kbs\":[");
  if (kbc_failed(st)) return st;
  size_t emitted = 0;
  for (size_t i = 0; i < n; i++) {
    const kbc_corpus_cfg *c = &cfg->corpora[i];
    if (only != NULL && strcmp(only, c->name) != 0) continue;
    if (emitted > 0) {
      st = kbc_str_putc(out, ',');
      if (kbc_failed(st)) return st;
    }
    kbc_artifact *rows = NULL;
    size_t nrows = 0;
    kbc_err local;
    kbc_err_reset(&local);
    st = kbc_app_list_artifacts(app, a, c->name, KBC_KIND__COUNT, KBC_MAX_HITS,
                                0, &rows, &nrows, &local);
    if (kbc_failed(st)) return st;
    struct stat sb;
    bool root_exists = stat(c->path, &sb) == 0 && S_ISDIR(sb.st_mode);
    st = kbc_str_puts(out, "{\"name\":");
    if (kbc_failed(st)) return st;
    st = kbc_str_append_json_string(out, c->name, strlen(c->name));
    if (kbc_failed(st)) return st;
    st = kbc_str_puts(out, ",\"path\":");
    if (kbc_failed(st)) return st;
    st = kbc_str_append_json_string(out, c->path, strlen(c->path));
    if (kbc_failed(st)) return st;
    st = kbc_str_printf(out,
                        ",\"configured\":true,\"root_exists\":%s,"
                        "\"docs\":%zu,\"docs_truncated\":%s}",
                        root_exists ? "true" : "false", nrows,
                        nrows >= KBC_MAX_HITS ? "true" : "false");
    if (kbc_failed(st)) return st;
    emitted++;
  }
  return kbc_str_printf(out,
                        "],\"count\":%zu,\"index_terms\":%lld,"
                        "\"index_docs\":%lld}",
                        emitted, (long long)stats.index_terms,
                        (long long)stats.index_docs);
}

/* GET /api/identity — who the daemon resolved this request to, and the fact
 * that resolving it changed nothing about what the caller may do. */
static kbc_status route_identity(const char *query, const kbc_request *req,
                                 auth_tier tier, auth_carrier carrier,
                                 kbc_str *out, kbc_err *err) {
  /* Identity is resolved, never requested. A client that tries to name itself
   * is refused rather than believed: a route that accepted ?as=admin would be
   * an authz system with one hardcoded role, which is worse than none. */
  if (query_has(query, "as") || query_has(query, "identity") ||
      query_has(query, "user")) {
    return kbc_err_set(err, KBC_ERR_INVALID,
                       "identity is resolved by the daemon; a caller cannot "
                       "claim one");
  }
  const char *client = req->client_addr != NULL ? req->client_addr : "?";
  kbc_status st = kbc_str_puts(out, "{\"identity\":");
  if (kbc_failed(st)) return st;
  st = kbc_str_append_json_string(out, tier_identity(tier),
                                  strlen(tier_identity(tier)));
  if (kbc_failed(st)) return st;
  st = kbc_str_puts(out, ",\"source\":");
  if (kbc_failed(st)) return st;
  st = kbc_str_append_json_string(out, tier_source(tier),
                                  strlen(tier_source(tier)));
  if (kbc_failed(st)) return st;
  /* Which carrier decided. Reported because the two-carrier rule is a silent
   * PREFERENCE (first_matching_carrier): a request that carried both is
   * otherwise indistinguishable on the wire from one that carried only the
   * winner, and an operator debugging "why is this attributed to the
   * operator" has no other way to see that a per-user secret arrived on the
   * other carrier and was discarded. */
  st = kbc_str_puts(out, ",\"carrier\":");
  if (kbc_failed(st)) return st;
  st = kbc_str_append_json_string(out, carrier_name(carrier),
                                  strlen(carrier_name(carrier)));
  if (kbc_failed(st)) return st;
  st = kbc_str_puts(out, ",\"client_addr\":");
  if (kbc_failed(st)) return st;
  st = kbc_str_append_json_string(out, client, strlen(client));
  if (kbc_failed(st)) return st;
  st = kbc_str_puts(out,
                    ",\"loopback\":");
  if (kbc_failed(st)) return st;
  st = kbc_str_puts(out, addr_is_loopback(client) ? "true" : "false");
  if (kbc_failed(st)) return st;
  /* Everything a client would want to know about what this answer does NOT
   * confer, stated once, in the body. */
  return kbc_str_puts(
      out,
      ",\"trust_tiers\":1,\"authorization\":false,\"roles\":[],"
      "\"note\":\"single-operator loopback daemon: identity is attribution, "
      "not authorization; there are no roles, ACLs or per-user tokens\"}");
}

static kbc_status route_banner(kbc_str *out) {
  return kbc_str_printf(
      out,
      "kb-c %s - self-hosted system of record for agent artifacts.\n"
      "This port serves a JSON API only; there is no reader UI here.\n"
      "Routes:\n"
      "  GET  /api/health\n"
      "  GET  /api/identity\n"
      "  GET  /api/kbs?kb=<corpus>\n"
      "  GET  /api/search?q=<terms>&kb=<corpus>&kind=&mode=&limit=&offset=\n"
      "  GET  /api/artifacts?kb=<corpus>&kind=&limit=&offset=\n"
      "  GET  /api/artifacts/{id}?source=1\n"
      "  GET  /api/kb/<corpus>/artifact/{id}?download=1\n"
      "  POST /api/reindex\n"
      "  GET  /api/events\n"
      "  GET  /api/stats\n"
      "  GET  /metrics\n"
      "Errors are RFC 7807 application/problem+json.\n"
      "One trust tier: identity is attribution, not authorization.\n"
      "A token is accepted on Authorization: Bearer or on X-Kb-Token;\n"
      "when both are present Authorization decides, and /api/identity says so.\n"
      "CORS is same-origin only unless KBC_CORS_ORIGINS names origins;\n"
      "KBC_RATE_LIMIT_RPS caps requests per connection per second.\n"
      "A Host of <id>.artifacts.localhost serves one artifact per origin;\n"
      "anything else falls back to the static root in KB_SPA_DIST.\n",
      KBC_VERSION);
}

/* One presented secret: a borrowed pointer plus the length of the bytes that
 * are actually part of it. The length is carried rather than implied by a NUL
 * because the two carriers trim by DIFFERENT amounts — the X-Kb-Token trim
 * shortens the secret, and a compare that used strlen would see the untrimmed
 * tail and reject a token the daemon is supposed to accept. */
typedef struct {
  const char *p;
  size_t len;
} secret_ref;

/* The two secret carriers, as this request presented them. A `len` of 0 means
 * "this carrier said nothing", which is what separates a 401 (nobody
 * introduced themselves) from a 403 (somebody did, and was wrong) — so the two
 * are never collapsed into a bare empty string that would read as a match
 * against an unset token. */
typedef struct {
  secret_ref bearer;
  secret_ref x_kb;
} presented;

static bool secret_is_set(const secret_ref *s) { return s->len > 0; }

/* Reads both carriers. The two are parsed by DIFFERENT rules on purpose, and
 * the difference is the original's, not an inconsistency:
 *
 *   Authorization — `strip_prefix("Bearer ")`, case-SENSITIVE, remainder NOT
 *     trimmed (`middleware.rs:414-419`). "bearer x" and "Basic x" are not
 *     credentials at all, and "Bearer x " presents the secret "x " with the
 *     space, which is a WRONG token rather than a well-formed one.
 *   X-Kb-Token — trimmed, then filtered on `!s.is_empty()`
 *     (`middleware.rs:421-427`), so a blank value is ABSENT rather than an
 *     empty secret.
 *
 * Two consequences, both pinned by token_carriers_admit_and_refuse. A blank
 * X-Kb-Token is ABSENT, so it gets 401 and not the 403 a wrong secret gets.
 * And a blank Authorization is absent TOO, but for a different and earlier
 * reason: RFC 7230 §3.2.4 requires a parser to strip the optional whitespace
 * around a field value, so `Authorization: Bearer ` arrives here as the six
 * bytes "Bearer" with no trailing space, the prefix does not match, and the
 * request is 401. The not-trimmed rule therefore shows up only on whitespace
 * the parser cannot remove: `Bearer  x` (two spaces) presents the secret
 * " x", leading space included, and is a WRONG token rather than a good one. */
static presented read_presented(const char *auth, const char *x_kb) {
  presented p;
  p.bearer.p = NULL;
  p.bearer.len = 0;
  p.x_kb.p = NULL;
  p.x_kb.len = 0;
  static const char kPrefix[] = "Bearer ";
  if (auth != NULL && strncmp(auth, kPrefix, sizeof kPrefix - 1) == 0) {
    const char *rest = auth + sizeof kPrefix - 1;
    p.bearer.p = rest;
    p.bearer.len = strlen(rest);
  }
  if (x_kb != NULL) {
    const char *v = x_kb;
    while (*v == ' ' || *v == '\t') v++;
    size_t n = strlen(v);
    while (n > 0 && (v[n - 1] == ' ' || v[n - 1] == '\t')) n--;
    /* A blank value is ABSENT, not an empty secret: `!s.is_empty()` in the
     * original. The zero length is what makes "blank" and "not sent" the same
     * case downstream. */
    p.x_kb.p = v;
    p.x_kb.len = n;
  }
  return p;
}

/* Constant-time compare of one presented secret against the configured one,
 * over exactly the bytes that carrier's own parse rules selected.
 *
 * On a length mismatch the same work is still done over the EXPECTED token, so
 * neither the length nor the position of the first difference is an obvious
 * timing oracle (`kbc_const_time_eq` is only constant-time in n, and n here is
 * always the length of the secret the daemon itself holds). */
static bool secret_matches(secret_ref got, const char *expected) {
  size_t want = strlen(expected);
  if (got.len != want) {
    (void)kbc_const_time_eq(expected, expected, want);
    return false;
  }
  return kbc_const_time_eq(got.p, expected, want);
}

/* THE BOTH-CARRIERS RULE, and the one place it is decided.
 *
 * The original's `registry_match` takes its candidates in the fixed order
 * `[bearer_token(headers), x_kb_token(headers)]` and returns the FIRST that
 * matches a registry entry (`middleware.rs:379-391`); `request_is_admitted`
 * consults the same function before the legacy shared token
 * (`middleware.rs:292`). So when BOTH carriers are present:
 *
 *   • they are NOT required to agree, and disagreeing is NOT an error — there
 *     is no comparison between them anywhere in the original;
 *   • the FIRST match wins, so `Authorization` silently outranks
 *     `X-Kb-Token`, and a second, valid, DIFFERENT secret on the other
 *     carrier is never examined.
 *
 * That preference has a security consequence and is therefore not left as an
 * implicit "if (auth) … else …": in the deployment the second carrier exists
 * for, a proxy overwrites `Authorization` with the shared daemon token
 * (`middleware.rs:209-212`), so a request carrying both will be attributed to
 * the SHARED token's holder — the operator — and the per-user secret on
 * `X-Kb-Token` is discarded without a word. `*carrier` records which carrier
 * actually decided, and /api/identity reports it, so the preference is
 * observable from the wire instead of only from this comment.
 *
 * Reproduced as-is rather than "fixed": the order is the original's
 * documented one, and a port that silently reversed it would change which
 * principal a proxied request is attributed to. */
static bool first_matching_carrier(const presented *p, const char *expected,
                                   auth_carrier *carrier) {
  if (secret_is_set(&p->bearer) && secret_matches(p->bearer, expected)) {
    *carrier = CARRIER_AUTHORIZATION;
    return true;
  }
  if (secret_is_set(&p->x_kb) && secret_matches(p->x_kb, expected)) {
    *carrier = CARRIER_X_KB_TOKEN;
    return true;
  }
  *carrier = CARRIER_NONE;
  return false;
}

/* Admission, in the original's order: loopback admits unconditionally; with a
 * token configured, a matching secret on EITHER carrier admits; anything else
 * is refused. An invalid `X-Kb-Token` never causes a 401 on its own — with a
 * valid bearer alongside it, the bearer is what admits and the bad header is
 * ignored (`middleware.rs:279-280`, pinned by the original's
 * `auth_bearer_registry_admits_and_x_kb_token_ignored_when_bad`).
 *
 * kb-c splits the refusal 401/403 where the original does not: the original
 * answers 401 for a wrong secret too (`middleware.rs:302-304` and the
 * `Bearer nope` → 401 assertion at `middleware.rs:1301-1309`), because it has
 * no second status to spend. kb-c keeps the split deliberately — 401 says "I
 * do not know who you are", 403 says "I do, and the secret is wrong" — and it
 * is the status PORT_PLAN.md records as the contract. What the original does
 * establish, and what is kept here, is that the WWW-Authenticate challenge and
 * the 401-for-no-credential are unchanged.
 *
 * `*tier` and `*carrier` report WHICH path admitted this request, which is
 * what /api/identity reports back. */
static kbc_status check_auth(const kbc_config *cfg, const kbc_request *req,
                             const char *x_kb_token, kbc_response *out,
                             auth_tier *tier, auth_carrier *carrier) {
  *tier = addr_is_loopback(req->client_addr) ? TIER_LOOPBACK : TIER_OPEN;
  *carrier = CARRIER_NONE;
  if (cfg == NULL || cfg->token == NULL || cfg->token[0] == '\0') return KBC_OK;
  presented p = read_presented(req->auth, x_kb_token);
  if (first_matching_carrier(&p, cfg->token, carrier)) {
    *tier = TIER_TOKEN;
    return KBC_OK;
  }
  if (!secret_is_set(&p.bearer) && !secret_is_set(&p.x_kb)) {
    (void)kbc_response_error_json(
        out, 401, KBC_ERR_INVALID,
        "a token is required: send it as `Authorization: Bearer <token>` or "
        "`X-Kb-Token: <token>`");
    return KBC_ERR_INVALID;
  }
  (void)kbc_response_error_json(out, 403, KBC_ERR_INVALID, "invalid token");
  return KBC_ERR_INVALID;
}

static kbc_status method_not_allowed(kbc_response *out, const char *m,
                                     const char *p) {
  return resp_error(out, 405, KBC_ERR_INVALID, "%s is not allowed on %s", m, p);
}

static const char *err_msg(const kbc_err *err, kbc_status st) {
  if (err == NULL) return kbc_status_str(st);
  return err->msg[0] != '\0' ? err->msg : kbc_status_str(st);
}

/* Everything a request needs that is not on the request itself: where a
 * route's own response headers go, the metrics registry, the second token
 * carrier, and the `unmatched` out-flag. The registry is NULL for a socketless
 * call — there is no daemon, so nothing has been counted — and `unmatched` lets
 * the connection layer take the origin fallback (`routes/dispatch.rs:46-50`)
 * without the router having to know what that fallback serves.
 *
 * `x_kb_token` rides here rather than on `kbc_request` because that struct is
 * the frozen contract (`httpd.h:28-36`) and carries only the `Authorization`
 * value. `kbc_request` is also what a socketless caller builds, and widening it
 * would change a published struct for every caller to carry a header only the
 * auth ladder reads. The carrier is borrowed for the dispatch and is "" (not
 * NULL) when absent, so callers that memset the context behave. */
typedef struct {
  kbc_str *extra;       /* header lines for this response; may be NULL */
  const metrics_reg *m; /* NULL for a socketless handle */
  bool *unmatched;      /* out; may be NULL */
  const char *x_kb_token; /* borrowed X-Kb-Token value, "" when absent */
  /* The request's Content-Type, BORROWED from the connection's header table,
   * "" when absent. It rides here for the reason x_kb_token does, but this
   * one is load-bearing rather than optional: a multipart body's boundary
   * lives in it, so the capture route cannot split a body without it. */
  const char *content_type;
  /* `X-Requested-By`, the provenance header the original's from_default reads
   * (capture.rs:129-137). BORROWED, "" when absent. */
  const char *x_requested_by;
  /* The daemon this request arrived on, or NULL for a socketless one. The only
   * route that needs it is `POST /api/reindex`, which owns the anchor pass's
   * synchronous half — the pass's state lives on the httpd, and there is no
   * back-pointer from a kbc_app to the httpd serving it. A NULL here is not a
   * degraded mode: `anchors_run(NULL)` is a no-op, so the socketless handle
   * simply re-indexes without judging anchors, exactly as a build with no
   * httpd at all would. */
  struct kbc_httpd *h;
} req_ctx;

/* (5)'s two dispatched routes, defined with the rest of the serving surface. */
static kbc_status route_artifact_bytes(kbc_app *app, kbc_arena *a,
                                       const kbc_config *cfg, const char *kb,
                                       const char *id, const char *query,
                                       kbc_str *hdrs, kbc_str *out,
                                       kbc_err *err);
static kbc_status route_prometheus(kbc_app *app, const metrics_reg *m,
                                   kbc_str *hdrs, kbc_str *out, kbc_err *err);

/* `/api` and everything under it. The origin split never claims these: an
 * unmatched `/api` path is a 404 problem+json, never the static shell
 * (`dispatch.rs:51-59`, `:88-96`). */
static bool path_is_api(const char *p) {
  return strncmp(p, "/api", 4) == 0 && (p[4] == '\0' || p[4] == '/');
}
/* ------------------------------------------------------ the /api/links --
 *
 * Three read endpoints, ported from `routes/links.rs`. What they have in
 * common is that every row they emit is a fact the graph already holds, so
 * the whole section is built on the SAME two primitives the write path uses:
 * `kbc_parsed_links` for extraction and `kbc_resolve_index` for resolution.
 * That is not a convenience — it is the invariant. The original says so at
 * links.rs:16-18 ("the SAME resolution the edge hook used, so the rendered
 * link and the recorded edge agree"), and a read path that resolved its own
 * way would describe links the graph does not have.
 *
 * The two CT-F3 endpoints of the same file, `GET …/links/suggest` and `POST
 * …/links/apply`, are NOT here: both are pure functions of
 * `kb_core::mentions` (find_mentions / applicability / apply_wikilink), and
 * that engine is OUT-OF-SCOPE in INVENTORY.md:141. The routes are not half
 * ported; their only input is a subsystem this port does not have.
 */

/* The corpus, shaped for the two consumers here.
 *
 * `docs` is the ladder's candidate set and `title_lower`/`base_lower` are the
 * autocomplete's ranking keys, computed ONCE per request rather than once per
 * keystroke. The original gets that for free from a memo keyed on
 * (kb, index-generation) (`links_index`, links.rs:136-185); kb-c has no such
 * memo and no field to hang one on, so the keys are recomputed per request
 * and the set is capped at KBC_MAX_HITS. A corpus larger than that cap
 * resolves against a PREFIX of itself, so the note-links body says so rather
 * than answering confidently about a document the candidate set never saw.
 *
 * The ORDER of `arts` is the store's, and the ladder's first-wins rule
 * (links.h:67-75) makes that order part of what a target resolves to. The
 * write path feeds the ladder the walk manifest's order instead, so a
 * target two documents collide on can resolve to a different one on each
 * path; that is a real, narrow divergence and it is the ladder's, not this
 * route's. */
typedef struct {
  const kbc_artifact **arts; /* KBC_ARENA, the listing, in store order */
  kbc_resolve_doc *docs;     /* KBC_ARENA, parallel to arts */
  const char **title_lower;  /* KBC_ARENA, parallel to arts */
  const char **base_lower;   /* KBC_ARENA, parallel to arts */
  size_t len;
} link_corpus;

/* ASCII case folding. The original's `to_lowercase()` is Unicode-aware
 * (links.rs:154), so a title written in Greek matches `?q=` case-insensitively
 * there and byte-exactly here. ASCII is what the rest of this file compares
 * (str_ieq), and a partial case fold is a worse lie than a documented one. */
static const char *ascii_lower(kbc_arena *a, const char *s) {
  size_t n = strlen(s);
  char *out = kbc_arena_alloc(a, n + 1);
  if (out == NULL) return NULL;
  for (size_t i = 0; i < n; i++) {
    unsigned char c = (unsigned char)s[i];
    out[i] = (char)(c >= 'A' && c <= 'Z' ? c + 32u : c);
  }
  out[n] = '\0';
  return out;
}

static kbc_status link_corpus_load(kbc_app *app, kbc_arena *a, const char *kb,
                                   link_corpus *out, kbc_err *err) {
  memset(out, 0, sizeof *out);
  kbc_artifact *rows = NULL;
  size_t n = 0;
  kbc_status st = kbc_app_list_artifacts(app, a, kb, KBC_KIND__COUNT,
                                         KBC_MAX_HITS, 0, &rows, &n, err);
  if (kbc_failed(st)) return st;
  /* Every parallel array is sized from the SAME n, so they cannot disagree
   * about how long the corpus is. One allocation each, zero-initialised so a
   * short fill is visible rather than a wild pointer. */
  size_t slots = n > 0 ? n : 1u;
  out->arts = kbc_arena_calloc(a, slots, sizeof *out->arts);
  out->docs = kbc_arena_calloc(a, slots, sizeof *out->docs);
  out->title_lower = kbc_arena_calloc(a, slots, sizeof *out->title_lower);
  out->base_lower = kbc_arena_calloc(a, slots, sizeof *out->base_lower);
  if (out->arts == NULL || out->docs == NULL || out->title_lower == NULL ||
      out->base_lower == NULL) {
    return kbc_err_set(err, KBC_ERR_NOMEM, "links: %zu candidates of %s", n, kb);
  }
  for (size_t i = 0; i < n; i++) {
    const char *rel = rows[i].path != NULL ? rows[i].path : "";
    out->arts[i] = &rows[i];
    out->docs[i].id = rows[i].id;
    out->docs[i].rel_path = rel;
    out->docs[i].title = rows[i].title != NULL ? rows[i].title : "";
    const char *base = strrchr(rel, '/');
    out->title_lower[i] = ascii_lower(a, out->docs[i].title);
    out->base_lower[i] = ascii_lower(a, base != NULL ? base + 1 : rel);
    if (out->title_lower[i] == NULL || out->base_lower[i] == NULL) {
      return kbc_err_set(err, KBC_ERR_NOMEM, "links: ranking keys for %s", rel);
    }
  }
  out->len = n;
  return KBC_OK;
}

/* The one metadata lookup a resolved id needs. Linear over the candidate set
 * rather than hashed: a note's own link count is small, and the set is capped
 * at KBC_MAX_HITS, so the worst case is a few thousand strcmp calls on links
 * a human wrote. The original builds a HashMap here (links.rs:216-217)
 * because its candidate set is the whole corpus and its link lists are
 * generated; neither is true of this port. */
static size_t link_index_of_id(const link_corpus *lc, const char *id) {
  for (size_t i = 0; i < lc->len; i++) {
    if (strcmp(lc->docs[i].id, id) == 0) return i;
  }
  return SIZE_MAX;
}

/* One `ResolvedLink` (links.rs:52-70), in the original's field order, with
 * `alias` never emitted. kb-c's parser collapses a link's display text into
 * `kbc_link.text` without recording whether it came from a `[label](target)`
 * or a `[[target|label]]`, and it stores the RAW target in that field for the
 * bare wikilink form — so `[[../x/y.md]]` and `[[y.md|../x/y.md]]` are
 * indistinguishable here, and any `alias` this route invented would be a
 * value the corpus never contained. The field is optional on the wire
 * (`skip_serializing_if = "Option::is_none"`), so a client that reads it must
 * already treat its absence as the normal case.
 *
 * `state` is the ladder's outcome spelled the original's way, and the three
 * of them are three different facts about the corpus, not three severities:
 * a dangling link is a "not yet created" affordance and is never an error
 * (links.rs:45-49). */
static kbc_status resolved_link_json(kbc_str *out, const link_corpus *lc,
                                     const char *kb, const char *target,
                                     const kbc_resolution *r) {
  const char *state = "dangling";
  size_t at = SIZE_MAX;
  if (r->kind == KBC_RESOLVE_ONE) {
    state = "resolved";
    at = link_index_of_id(lc, r->ids.items[0]);
  } else if (r->kind == KBC_RESOLVE_AMBIGUOUS) {
    state = "ambiguous";
  }
  kbc_status st = kbc_str_puts(out, "{\"target\":");
  if (kbc_failed(st)) return st;
  st = kbc_str_append_json_string(out, target, strlen(target));
  if (kbc_failed(st)) return st;
  st = kbc_str_puts(out, ",\"state\":");
  if (kbc_failed(st)) return st;
  st = kbc_str_append_json_string(out, state, strlen(state));
  if (kbc_failed(st)) return st;
  if (at != SIZE_MAX) {
    st = kbc_str_puts(out, ",\"id\":");
    if (kbc_failed(st)) return st;
    const kbc_artifact *art = lc->arts[at];
    st = kbc_str_append_json_string(out, art->id, strlen(art->id));
    if (kbc_failed(st)) return st;
  }
  st = kbc_str_puts(out, ",\"kb\":");
  if (kbc_failed(st)) return st;
  st = kbc_str_append_json_string(out, kb, strlen(kb));
  if (kbc_failed(st)) return st;
  /* The next three are skipped exactly where the original skips them: an
   * ambiguous or dangling target has no document, so there is no title, no
   * path and no kind to report, and the candidates that DO exist are dropped
   * rather than smuggled through a field the original does not have. */
  if (at != SIZE_MAX) {
    const kbc_artifact *art = lc->arts[at];
    st = kbc_str_puts(out, ",\"title\":");
    if (kbc_failed(st)) return st;
    const char *title = art->title != NULL ? art->title : "";
    st = kbc_str_append_json_string(out, title, strlen(title));
    if (kbc_failed(st)) return st;
    st = kbc_str_puts(out, ",\"source_relative\":");
    if (kbc_failed(st)) return st;
    st = kbc_str_append_json_string(out, art->path, strlen(art->path));
    if (kbc_failed(st)) return st;
  }
  return kbc_str_printf(out, ",\"is_note\":%s}",
                        (at != SIZE_MAX && lc->arts[at]->kind == KBC_KIND_NOTE)
                            ? "true"
                            : "false");
}

/* `resolve_outgoing` (links.rs:201-264) as the `outgoing` array.
 *
 * THE DEDUP. The original keys its `seen` set on `normalize_target(target)` —
 * the trimmed, fragment-stripped SPELLING (links.rs:220-224). This keys it on
 * `kbc_link.target`, the parser's corpus-relative path, which is the same
 * identity the write path's `edges` primary key collapses on: app.c's
 * edge-record hook has no `seen` set at all and says why. So two spellings of
 * one destination are two rows in the original's `outgoing` and ONE here, and
 * one is also what the graph holds — which is the property that matters,
 * because a route that listed a link twice would be describing a row that
 * does not exist.
 *
 * THE SELF-LINK IS REPORTED, NOT SKIPPED. `[[self]]` resolves to the document
 * itself, so it takes the ordinary ONE branch and comes back `resolved` with
 * its own id (links.rs:196-200: "still reported as resolved so the body
 * renders it"). The write path drops it, because a document is not a backlink
 * of itself; the read path reports it, because the body still has to render
 * the link. Those are one decision seen from two sides, and a read path that
 * skipped it would leave the body unable to draw a link it drew before.
 *
 * Membership is a linear scan over the note's own links, not a hash set. A
 * note has tens of links and the set dies with the request, so the original's
 * HashSet buys nothing at this cardinality. */
static kbc_status outgoing_json(kbc_str *out, kbc_arena *a,
                                const kbc_resolve_index *ix,
                                const link_corpus *lc, const kbc_links *links,
                                const char *kb, kbc_err *err) {
  kbc_status st = kbc_str_putc(out, '[');
  if (kbc_failed(st)) return st;
  kbc_strlist seen;
  kbc_strlist_init(&seen);
  size_t emitted = 0;
  for (size_t i = 0; i < links->len; i++) {
    const char *target = links->items[i].target;
    if (target == NULL || target[0] == '\0') continue;
    if (kbc_strlist_contains(&seen, target)) continue;
    st = kbc_strlist_push(&seen, target);
    if (kbc_failed(st)) break;
    kbc_resolution r;
    kbc_err rl;
    kbc_err_reset(&rl);
    /* `a` is the ladder's scratch arena (links.h:88-90): the normalised target
     * dies at the next call, which is why the whole note shares one. */
    kbc_status rs = kbc_resolve_index_resolve(ix, a, target, &r, &rl);
    if (kbc_failed(rs)) {
      (void)kbc_err_set(err, rs, "links: resolve \"%s\": %s", target,
                        rl.msg[0] != '\0' ? rl.msg : "ladder failed");
      break;
    }
    if (emitted > 0) st = kbc_str_putc(out, ',');
    if (kbc_failed(st)) {
      kbc_strlist_free(&r.ids);
      break;
    }
    st = resolved_link_json(out, lc, kb, target, &r);
    kbc_strlist_free(&r.ids);
    if (kbc_failed(st)) break;
    emitted++;
  }
  kbc_strlist_free(&seen);
  if (kbc_failed(st)) return st;
  return kbc_str_putc(out, ']');
}

/* The parent directory of a corpus-relative path, "" at the top level —
 * `kb_core::paths::doc_folder` (paths.rs:404-411), whose `rsplit_once('/')`
 * returning None is exactly its "foo.html" case. */
static void doc_folder(kbc_arena *a, const char *rel, const char **out) {
  const char *slash = strrchr(rel, '/');
  *out = slash != NULL ? kbc_arena_strndup(a, rel, (size_t)(slash - rel)) : "";
}

/* One `BacklinkRef` (links.rs:75-82). `kb` is the corpus the LINK lives in,
 * which is the corpus being asked about: edges are intra-kb by construction,
 * so a backlink never crosses one. */
static kbc_status backlink_json(kbc_str *out, kbc_arena *a, const char *kb,
                                const kbc_artifact *art) {
  const char *folder = NULL;
  doc_folder(a, art->path, &folder);
  kbc_status st = kbc_str_puts(out, "{\"id\":");
  if (kbc_failed(st)) return st;
  st = kbc_str_append_json_string(out, art->id, strlen(art->id));
  if (kbc_failed(st)) return st;
  st = kbc_str_puts(out, ",\"kb\":");
  if (kbc_failed(st)) return st;
  st = kbc_str_append_json_string(out, kb, strlen(kb));
  if (kbc_failed(st)) return st;
  st = kbc_str_puts(out, ",\"title\":");
  if (kbc_failed(st)) return st;
  const char *title = art->title != NULL ? art->title : "";
  st = kbc_str_append_json_string(out, title, strlen(title));
  if (kbc_failed(st)) return st;
  st = kbc_str_puts(out, ",\"source_relative\":");
  if (kbc_failed(st)) return st;
  st = kbc_str_append_json_string(out, art->path, strlen(art->path));
  if (kbc_failed(st)) return st;
  st = kbc_str_puts(out, ",\"folder\":");
  if (kbc_failed(st)) return st;
  st = kbc_str_append_json_string(out, folder, strlen(folder));
  if (kbc_failed(st)) return st;
  return kbc_str_printf(out, ",\"is_note\":%s}",
                        art->kind == KBC_KIND_NOTE ? "true" : "false");
}

/* Notes first, then alphabetical by title — `load_backlinks`'s final sort
 * (links.rs:316-320), which is what actually orders the response; the store's
 * row order is not. The comparison is on the STORED title, like every other
 * title this file emits off a list row. */
static int backlink_cmp(const void *va, const void *vb) {
  const kbc_artifact *const *a = (const kbc_artifact *const *)va;
  const kbc_artifact *const *b = (const kbc_artifact *const *)vb;
  bool an = (*a)->kind == KBC_KIND_NOTE;
  bool bn = (*b)->kind == KBC_KIND_NOTE;
  if (an != bn) return an ? -1 : 1;
  return strcmp((*a)->title != NULL ? (*a)->title : "",
                (*b)->title != NULL ? (*b)->title : "");
}

/* The `backlinks` array, shared by `GET …/backlinks/{id}` and the note-links
 * route. An empty result is `[]` and never a 404: "nothing links here" is an
 * ordinary fact about an ordinary document (links.rs:324-325).
 *
 * The linkers are resolved in ONE `get_artifacts_by_path` round trip. A
 * per-linker `get_artifact` is the same defect the edge write was bitten by,
 * and a hub note is exactly the case that makes it expensive.
 *
 * `get_artifacts_by_path` takes PARALLEL corpus and path arrays of n and hands
 * back an array of exactly n slots, a NULL slot meaning that pair has no row
 * (store.h:56-64). The slot array is KBC_OWN and is freed here; the artifacts
 * belong to the arena. A NULL slot is a real case, not a defensive one: an
 * edge whose source document has since been removed stays in the table until
 * the removal rewrites it, and such a row has no artifact to describe. */
static kbc_status backlinks_json(kbc_str *out, kbc_app *app, kbc_arena *a,
                                 const char *kb, const char *path,
                                 kbc_err *err) {
  kbc_strlist srcs;
  kbc_strlist_init(&srcs);
  kbc_status st =
      kbc_store_list_backlinks(kbc_app_store(app), kb, path, &srcs, err);
  if (kbc_failed(st)) {
    kbc_strlist_free(&srcs);
    return st;
  }
  st = kbc_str_putc(out, '[');
  if (kbc_failed(st)) {
    kbc_strlist_free(&srcs);
    return st;
  }
  kbc_artifact **slots = NULL;
  /* Taken BEFORE the list is freed: `kbc_strlist_free` resets len, and every
   * bound below is a function of how many paths there were. */
  const size_t n_src = srcs.len;
  if (n_src > 0) {
    const char **corpora = kbc_arena_alloc(a, n_src * sizeof(*corpora));
    if (corpora == NULL) {
      kbc_strlist_free(&srcs);
      return kbc_err_set(err, KBC_ERR_NOMEM, "backlinks: %zu corpora", n_src);
    }
    /* One corpus, n pairs: every backlink of a document in `kb` is an edge
     * recorded in `kb`, because edges are intra-kb by construction. */
    for (size_t i = 0; i < n_src; i++) corpora[i] = kb;
    st = kbc_store_get_artifacts_by_path(
        kbc_app_store(app), a, corpora, (const char *const *)srcs.items,
        n_src, &slots, err);
  }
  kbc_strlist_free(&srcs);
  if (kbc_failed(st)) {
    free(slots);
    return st;
  }
  size_t n = 0;
  const kbc_artifact **ordered = NULL;
  if (n_src > 0) {
    ordered = kbc_arena_alloc(a, n_src * sizeof *ordered);
    if (ordered == NULL) {
      free(slots);
      return kbc_err_set(err, KBC_ERR_NOMEM, "backlinks: %zu slots of %s", n_src,
                         kb);
    }
    for (size_t i = 0; i < n_src; i++) {
      if (slots[i] != NULL) ordered[n++] = slots[i];
    }
    if (n > 1) qsort(ordered, n, sizeof *ordered, backlink_cmp);
  }
  free(slots);
  for (size_t i = 0; st == KBC_OK && i < n; i++) {
    if (i > 0) st = kbc_str_putc(out, ',');
    if (kbc_failed(st)) break;
    st = backlink_json(out, a, kb, ordered[i]);
  }
  if (kbc_failed(st)) return st;
  return kbc_str_putc(out, ']');
}

/* `GET /api/kb/{kb}/notes/{id}/links` (notes.rs:743-772, mounted at
 * router.rs:680) — one note's outgoing wikilinks and its backlinks.
 *
 * 404: the corpus is not configured, or `id` names no document.
 * 400: the document's source will not parse.
 * 500: anything else the store or the parser layer reports as fatal.
 *
 * THE NOTE ASSERTION IS DELIBERATELY ABSENT, and this is the one place this
 * route knowingly differs from the original. `note_row` asserts
 * `notes::is_note` before it will answer (notes.rs:305-317), so an artifact id
 * is a 404 there. kb-c's equivalent predicate is the stored `kind`, and its
 * ingest assigns `KBC_KIND_ARTIFACT` to EVERY document unconditionally
 * (app.c:789) — so asserting the kind would 404 the whole corpus and make the
 * route unanswerable rather than precise. The row's `is_note` still reports
 * the stored kind, so the day ingest starts minting notes this route starts
 * labelling them with no other change, and the assertion is then a single
 * `if` away. */
static kbc_status route_note_links(kbc_app *app, kbc_arena *a,
                                   const kbc_config *cfg, const char *kb,
                                   const char *id, kbc_str *out,
                                   kbc_err *err) {
  if (kbc_config_corpus(cfg, kb) == NULL) {
    return kbc_err_set(err, KBC_ERR_NOTFOUND, "no corpus named \"%s\"", kb);
  }
  kbc_artifact art;
  memset(&art, 0, sizeof art);
  kbc_status st = kbc_app_get_artifact(app, a, id, true, &art, err);
  if (kbc_failed(st)) return st;
  /* The kind is reported, not enforced — see the note above. */
  const char *src = art.source != NULL ? art.source : "";
  kbc_parsed *p = kbc_parse(a, src, strlen(src), art.path, err);
  if (p == NULL) return KBC_ERR_PARSE;

  link_corpus lc;
  st = link_corpus_load(app, a, kb, &lc, err);
  if (kbc_failed(st)) return st;
  kbc_resolve_index *ix = kbc_resolve_index_new(lc.docs, lc.len, err);
  if (ix == NULL) return KBC_ERR_NOMEM;

  st = kbc_str_printf(out, "{\"outgoing\":");
  if (kbc_failed(st)) {
    kbc_resolve_index_free(ix);
    return st;
  }
  st = outgoing_json(out, a, ix, &lc, kbc_parsed_links(p), kb, err);
  if (kbc_failed(st)) {
    kbc_resolve_index_free(ix);
    return st;
  }
  st = kbc_str_puts(out, ",\"backlinks\":");
  if (kbc_failed(st)) {
    kbc_resolve_index_free(ix);
    return st;
  }
  st = backlinks_json(out, app, a, kb, art.path, err);
  kbc_resolve_index_free(ix);
  if (kbc_failed(st)) return st;
  /* Additive, and not decoration: `lc.len` is the store's page size, so a
   * corpus past KBC_MAX_HITS resolves its links against a prefix of itself.
   * A client that cannot see that would be reading a confident answer about
   * a document the candidate set never held. */
  return kbc_str_printf(out, ",\"candidates\":%zu,\"candidates_truncated\":%s}",
                        lc.len, lc.len >= KBC_MAX_HITS ? "true" : "false");
}

/* `GET /api/kb/{kb}/backlinks/{id}` (links.rs:326-338) — inbound references
 * to ANY artifact, not only a note. 404 for an unknown corpus or an id the
 * store does not hold; `[]` with a 200 for a document nothing links to, which
 * is a different fact and gets a different answer. */
static kbc_status route_backlinks(kbc_app *app, kbc_arena *a,
                                  const kbc_config *cfg, const char *kb,
                                  const char *id, kbc_str *out,
                                  kbc_err *err) {
  if (kbc_config_corpus(cfg, kb) == NULL) {
    return kbc_err_set(err, KBC_ERR_NOTFOUND, "no corpus named \"%s\"", kb);
  }
  kbc_artifact art;
  memset(&art, 0, sizeof art);
  kbc_status st = kbc_app_get_artifact(app, a, id, false, &art, err);
  if (kbc_failed(st)) return st;
  st = kbc_str_puts(out, "{\"backlinks\":");
  if (kbc_failed(st)) return st;
  st = backlinks_json(out, app, a, kb, art.path, err);
  if (kbc_failed(st)) return st;
  return kbc_str_putc(out, '}');
}

/* The suggest ordering, as a predicate so the sort and its comment cannot
 * drift apart: rank first, then the LOWERCASED title (links.rs:381). */
static int suggest_cmp(const char *la, const char *lb) {
  int c = strcmp(la, lb);
  return c != 0 ? c : 0;
}

/* `GET /api/kb/{kb}/wikilinks/suggest?q=&limit=` (links.rs:344-395) — the
 * composer's `[[` autocomplete.
 *
 * Ranking is three tiers over the LOWERCASED keys: a title prefix (0)
 * outranks a title substring (1) outranks a basename substring (2), and
 * within a tier the lowercased title decides (links.rs:364-381). Notes get
 * no preference — you link research write-ups too — so `is_note` is carried
 * for the icon and never used to order. An empty `q` lists everything, which
 * is the original's rank-0 arm (links.rs:370) and the reason the endpoint
 * answers without one.
 *
 * `?q=` is REQUIRED, and a request without it is a 400: `SuggestQuery` types
 * it as a non-Option String (links.rs:113-118), so axum's extractor rejects
 * the request before the handler runs. Defaulting it to "" would have made
 * the whole corpus a legal answer to a typo.
 *
 * The limit defaults to 12 and clamps to 50 (links.rs:120-121). Unlike the
 * note-links route, hitting the candidate ceiling here costs ranked rows AT
 * THE TAIL rather than a wrong `state`, so it is logged, not reported. */
#define KBC_SUGGEST_DEFAULT_LIMIT 12u
#define KBC_SUGGEST_MAX_LIMIT 50u
static kbc_status route_wikilinks_suggest(kbc_app *app, kbc_arena *a,
                                          const char *kb, const char *query,
                                          kbc_str *out, kbc_err *err) {
  const char *q = NULL;
  kbc_status st = query_get(a, query, "q", &q, err);
  if (kbc_failed(st)) return st;
  if (q == NULL) {
    return kbc_err_set(err, KBC_ERR_INVALID, "missing required query \"q\"");
  }
  const char *needle = ascii_lower(a, q);
  if (needle == NULL) return kbc_err_set(err, KBC_ERR_NOMEM, "wikilinks: q");
  size_t nq = strlen(needle);
  size_t limit = KBC_SUGGEST_DEFAULT_LIMIT;
  if (query_has(query, "limit")) {
    int64_t lim = query_int(query, "limit");
    if (lim < 1) {
      return kbc_err_set(err, KBC_ERR_INVALID,
                         "limit must be an integer >= 1");
    }
    limit = (size_t)lim;
  }
  if (limit > KBC_SUGGEST_MAX_LIMIT) limit = KBC_SUGGEST_MAX_LIMIT;

  link_corpus lc;
  st = link_corpus_load(app, a, kb, &lc, err);
  if (kbc_failed(st)) return st;
  if (lc.len >= KBC_MAX_HITS) {
    KBC_LOGW("wikilinks suggest for %s ranked a %zu-document prefix of the "
             "corpus: the store page size is %u",
             kb, lc.len, KBC_MAX_HITS);
  }
  typedef struct {
    uint8_t rank;
    const char *title_lower;
    size_t at;
  } scored;
  scored *rows = kbc_arena_alloc(a, (lc.len > 0 ? lc.len : 1u) * sizeof *rows);
  if (rows == NULL) {
    return kbc_err_set(err, KBC_ERR_NOMEM, "wikilinks: %zu candidates", lc.len);
  }
  size_t n = 0;
  for (size_t i = 0; i < lc.len; i++) {
    const char *tl = lc.title_lower[i];
    uint8_t rank;
    if (nq == 0 || (strlen(tl) >= nq && strncmp(tl, needle, nq) == 0)) {
      rank = 0;
    } else if (strstr(tl, needle) != NULL) {
      rank = 1;
    } else if (strstr(lc.base_lower[i], needle) != NULL) {
      rank = 2;
    } else {
      continue;
    }
    rows[n].rank = rank;
    rows[n].title_lower = tl;
    rows[n].at = i;
    n++;
  }
  /* Insertion sort by (rank, lowercased title). The tie-break is the
   * lowercased title and never the store's row order, so two runs over one
   * corpus return the same suggestions in the same sequence — which is the
   * property that makes an autocomplete usable at all. Insertion rather than
   * qsort because the comparator's first key is a tiny integer and the array
   * is capped at the page size; the original's `sort_by` is the same total
   * order, not the same algorithm. */
  for (size_t i = 1; i < n; i++) {
    scored key = rows[i];
    size_t j = i;
    while (j > 0 && (rows[j - 1].rank > key.rank ||
                     (rows[j - 1].rank == key.rank &&
                      suggest_cmp(rows[j - 1].title_lower,
                                  key.title_lower) > 0))) {
      rows[j] = rows[j - 1];
      j--;
    }
    rows[j] = key;
  }
  if (n > limit) n = limit;
  st = kbc_str_puts(out, "{\"suggestions\":[");
  if (kbc_failed(st)) return st;
  for (size_t i = 0; st == KBC_OK && i < n; i++) {
    const kbc_artifact *art = lc.arts[rows[i].at];
    if (i > 0) st = kbc_str_putc(out, ',');
    if (kbc_failed(st)) break;
    st = kbc_str_puts(out, "{\"id\":");
    if (kbc_failed(st)) break;
    st = kbc_str_append_json_string(out, art->id, strlen(art->id));
    if (kbc_failed(st)) break;
    st = kbc_str_puts(out, ",\"title\":");
    if (kbc_failed(st)) break;
    const char *title = art->title != NULL ? art->title : "";
    st = kbc_str_append_json_string(out, title, strlen(title));
    if (kbc_failed(st)) break;
    st = kbc_str_puts(out, ",\"source_relative\":");
    if (kbc_failed(st)) break;
    st = kbc_str_append_json_string(out, art->path, strlen(art->path));
    if (kbc_failed(st)) break;
    st = kbc_str_printf(out, ",\"is_note\":%s}",
                        art->kind == KBC_KIND_NOTE ? "true" : "false");
  }
  if (kbc_failed(st)) return st;
  return kbc_str_puts(out, "]}");
}

/* The capture section below needs this file's three path primitives, and they
 * are defined further down beside the traversal guard they implement. They are
 * declared here rather than moved up because they belong there, and a forward
 * declaration is the shape this file already uses for a route defined away
 * from its call site (see route_artifact_bytes). */
static char *canon(const char *path);
static char *path_join(const char *base, const char *rel);
static bool path_within(const char *root, const char *path);

/* ------------------------------------------------------------- capture ----
 *
 * `POST /api/kb/{kb}/capture`, ported from `routes/capture.rs`. The Rust has
 * exactly two capture routes: this one, and `POST /capture` — the Web Share
 * Target action, mounted OUTSIDE the /api nest because the browser's share
 * POST hits a bare path. kb-c has no web manifest, no share target and no
 * reader UI to redirect a shared page into, so the second route has no
 * counterpart here and is not invented: it would be a 303 to a page this port
 * does not serve. There is no GET on this path in the original either —
 * capture is a write and nothing else — so there is no read surface to port.
 *
 * The field names are the original's and are load-bearing, because a client
 * sends them by name: `files` (repeated), `title`, `tags`, `from`, `url`,
 * `text`, `sanitize` (capture.rs:196-198, :240-247). `tags` splits on ','
 * with each piece trimmed and empties dropped (:146-152). `sanitize` is read
 * and then deliberately IGNORED, for the reason the original itself gives:
 * it only ever affects the HTML pipeline, which is a no-op for Markdown
 * ("Markdown ignores it either way — U1 no-op", :307) and which kb-c does not
 * have at all — see the note on the extension gate below.
 *
 * THE TWO CAPS, and why both are checked rather than one. capture.rs:60
 * `MAX_CAPTURE_FILES` is 50 and is a DoS backstop against a request made of
 * thousands of tiny parts, each individually under every byte cap;
 * capture.rs:38 `DEFAULT_MAX_FILE_BYTES` is 10 MiB and bounds ONE file. They
 * are independent in the original (the U2 follow-up is entirely about a
 * request budget sized off the wrong one), so they are independent here.
 *
 * A note a reader needs, because it decides which of the two a client
 * actually meets: this port's TRANSPORT refuses a Content-Length over
 * KBC_MAX_SNIFF_BYTES (64 KiB) before a route is chosen at all (httpd.c's
 * req_parse), so on the wire the 64 KiB ceiling answers first and the 10 MiB
 * per-file check below is unreachable through it. It is enforced anyway, and
 * the reason is not optimism: it is the check that keeps the cap a property
 * of the CAPTURE SURFACE rather than a coincidence of a buffer size in
 * another file, and the request-level budget beside it is the one that would
 * bite first the day that ceiling moves. Nothing in the response depends on
 * which of the two fired; both answer 413 with a detail naming the cap.
 */
#define CAPTURE_MAX_FILES 50u    /* capture.rs:60 */
#define CAPTURE_MAX_FILE_BYTES (10u * 1024u * 1024u) /* capture.rs:38 */
#define CAPTURE_MAX_REQUEST_BYTES (64u * 1024u * 1024u) /* capture.rs:46 */
/* The decoder counts EVERY part, the file cap counts only `files` parts, and
 * the six named text fields travel in the same body — so the array is sized
 * for both or a legal 50-file request with a title would be refused as a
 * buffer overflow. */
#define CAPTURE_MAX_PARTS (CAPTURE_MAX_FILES + 6u)
/* The indexable spellings, and this is now the FULL set: app.c's
 * is_indexable accepts `.md`, `.markdown`, `.html` and `.htm`, and the two
 * HTML ones are here because there is a sanitiser to run them through
 * (kbc_html_sanitize, in app.c's capture path) rather than because a 415
 * would protect anything. It never did: the corpus is how HTML arrives
 * anyway, by `kb add`, `git checkout` or a sync, and a gate that only covers
 * the upload path covers the upload path and nothing else. */
static const char *const kCaptureExts[] = {"md", "markdown", "html", "htm"};

/* capture.rs:129-137, from_default. The `X-Requested-By` header is how kb-cli
 * and the SPA name themselves; the `kb-` prefix is stripped so the stamped
 * tag reads `from:cli`, and a caller that sent neither gets "api" — which is
 * a curl, and says so in the corpus rather than blaming a client that is not
 * there. The result is KBC_ARENA: it is a lowercased copy, not the header. */
static const char *capture_from_default(kbc_arena *a, const char *hdr) {
  const char *v = hdr;
  if (v != NULL) {
    while (*v == ' ' || *v == '\t') v++;
  }
  size_t n = v != NULL ? strlen(v) : 0;
  while (n > 0 && (v[n - 1] == ' ' || v[n - 1] == '\t')) n--;
  if (n == 0) return "api";
  static const char pfx[] = "kb-";
  const size_t pl = sizeof pfx - 1;
  if (n > pl && strncmp(v, pfx, pl) == 0) {
    v += pl;
    n -= pl;
  }
  /* Bounded by the header ceiling upstream; copied because the tag is stamped
   * lowercased and the header is not. */
  char *out = kbc_arena_alloc(a, n + 1);
  if (out == NULL) return "api";
  for (size_t i = 0; i < n; i++) {
    unsigned char c = (unsigned char)v[i];
    out[i] = (char)(c >= 'A' && c <= 'Z' ? c + 32 : c);
  }
  out[n] = '\0';
  return out[0] != '\0' ? out : "api";
}

/* capture.rs:146-152, split_tags: split on ',', trim each piece, drop the
 * empties. The array is KBC_ARENA and parallel to nothing — kbc_capture_input
 * takes (tags, n_tags) — so it dies with the request. The upper bound on the
 * count is the comma count, which is why this allocates before it counts. */
static kbc_status capture_tags(kbc_arena *a, const char *s,
                                const char ***out, size_t *n_out,
                                kbc_err *err) {
  size_t commas = 0;
  for (const char *p = s; *p != '\0'; p++) {
    if (*p == ',') commas++;
  }
  const char **v = kbc_arena_alloc(a, (commas + 1) * sizeof *v);
  if (v == NULL) {
    return kbc_err_set(err, KBC_ERR_NOMEM, "capture: %zu tags", commas + 1);
  }
  size_t n = 0;
  const char *p = s;
  for (;;) {
    const char *comma = strchr(p, ',');
    const size_t seg = comma != NULL ? (size_t)(comma - p) : strlen(p);
    size_t b = 0, e = seg;
    while (b < e && (p[b] == ' ' || p[b] == '\t')) b++;
    while (e > b && (p[e - 1] == ' ' || p[e - 1] == '\t')) e--;
    if (e > b) {
      char *piece = kbc_arena_strndup(a, p + b, e - b);
      if (piece == NULL) {
        return kbc_err_set(err, KBC_ERR_NOMEM, "capture: tag %zu", n);
      }
      v[n++] = piece;
    }
    if (comma == NULL) break;
    p = comma + 1;
  }
  *out = v;
  *n_out = n;
  return KBC_OK;
}

/* The dot-less, lowercased extension of a multipart filename, or NULL when it
 * has none. Only the FINAL component is considered and a leading dot is not
 * an extension, which is the same rule app.c's file_stem_into uses — a
 * filename is attacker text here, so the two must not disagree about which
 * half of ".bashrc" is the name. */
static const char *capture_ext_of(const char *filename) {
  if (filename == NULL) return NULL;
  const char *base = filename;
  for (const char *p = filename; *p != '\0'; p++) {
    if (*p == '/' || *p == '\\') base = p + 1;
  }
  const char *dot = NULL;
  for (const char *p = base; *p != '\0'; p++) {
    if (*p == '.') dot = p;
  }
  if (dot == NULL || dot == base || dot[1] == '\0') return NULL;
  static const char *const kMap[][2] = {
      {"md", "md"}, {"MD", "md"}, {"markdown", "markdown"},
      {"MARKDOWN", "markdown"}, {"Markdown", "markdown"}};
  for (size_t i = 0; i < sizeof kMap / sizeof kMap[0]; i++) {
    if (str_ieq(dot + 1, kMap[i][0])) return kMap[i][1];
  }
  return NULL;
}

/* The extension gate. The original resolves every file against the corpus's
 * extension map and 415s an unmapped one (capture.rs:340-352) so a capture
 * can never write a file the indexer would then refuse to pick up. kb-c's
 * indexer reads .html too, so the gate is the same four spellings
 * `is_indexable` accepts.
 *
 * HTML was refused here until the sanitiser existed, and the reason it was
 * refused is still the reason it is not: an HTML capture cannot be stamped
 * with a front matter block, because front matter in an HTML file is a
 * comment-shaped lie. What changed is that there is now a real HTML pipeline
 * to stamp into — kbc_html_sanitize then a `<meta>` splice into the head
 * (app.c, stamp_capture_html) — so `.html` and `.htm` are accepted and the
 * provenance lands where a reader of the file will find it. Sanitise BEFORE
 * stamping, always; see the comment on stamp_capture_html.
 *
 * Checked for EVERY file BEFORE the first write, so a mixed batch fails
 * whole: capture.rs:309-315 is explicit that only the VALIDATION is
 * all-or-nothing, and a `good.md` left behind a 415 would be a retry that
 * duplicates it. */
static bool capture_ext_indexable(const char *filename) {
  const char *e = capture_ext_of(filename);
  if (e == NULL) return false;
  for (size_t i = 0; i < sizeof kCaptureExts / sizeof kCaptureExts[0]; i++) {
    if (strcmp(e, kCaptureExts[i]) == 0) return true;
  }
  return false;
}

/* One `{"kb","id","source_relative"}` item — the three things a caller needs
 * to find the document it just made. The original's item also carries `title`
 * and an echoed `url` (capture.rs:71-82); NEITHER is reproduced. The title is
 * the indexer's own `title.or(h1).or(stem)` chain, which kbc_app already runs
 * and which is not final until the watcher has ingested the file, so a title
 * in this response would be a second implementation of it and a second thing
 * to drift. The url is the request's own field, echoed back to a caller that
 * sent it and to nobody else. */
static kbc_status capture_item_json(kbc_str *out, const char *kb,
                                    const kbc_capture_result *r) {
  kbc_status st = kbc_str_puts(out, "{\"kb\":");
  if (kbc_failed(st)) return st;
  st = kbc_str_append_json_string(out, kb, strlen(kb));
  if (kbc_failed(st)) return st;
  st = kbc_str_puts(out, ",\"id\":");
  if (kbc_failed(st)) return st;
  st = kbc_str_append_json_string(out, r->id, strlen(r->id));
  if (kbc_failed(st)) return st;
  st = kbc_str_puts(out, ",\"source_relative\":");
  if (kbc_failed(st)) return st;
  /* Attacker-influenced: the filename is a slug of the caller's `title`, and
   * a slug is not a guarantee of anything printable. */
  st = kbc_str_append_json_string(out, r->path, strlen(r->path));
  if (kbc_failed(st)) return st;
  return kbc_str_puts(out, "}");
}

/* The write-direction containment check. capture_write composes
 * `<corpus path>/<capture_dir>/<slug>` from a caller-supplied `title` and a
 * caller-supplied multipart filename, and its own defences are the slug
 * (which has no separator left to traverse with) and a `..` check on
 * `capture_dir`. Those hold, but they are defences in ANOTHER file, and this
 * is the first caller that can reach them from a network. So the route
 * re-checks the composed result against the same component-wise guard the
 * read path uses (path_within, httpd.c's own comment on why strncmp is the
 * bug): a capture that resolved outside its corpus root is a REMOTE FILE
 * WRITE, which is a different and worse failure than a read escaping, and
 * nothing in the read-side traversal cases covers it.
 *
 * It is checked on the RESULT rather than the inputs because that is the only
 * place the composed path exists, and the composition is exactly what a future
 * change to the filename policy would alter. */
static kbc_status capture_contained(const kbc_corpus_cfg *cc,
                                    const kbc_capture_result *r,
                                    kbc_err *err) {
  char *root = canon(cc->path);
  if (root == NULL) {
    return kbc_err_set(err, KBC_ERR_NOTFOUND, "corpus %s: %s is not there",
                       cc->name, cc->path);
  }
  char *joined = path_join(root, r->path);
  if (joined == NULL) {
    free(root);
    return kbc_err_set(err, KBC_ERR_INVALID, "capture path over %u bytes",
                       (unsigned)KBC_MAX_PATH_LEN);
  }
  /* realpath: the destination now exists, so this also resolves a symlink
   * somebody planted inside the corpus between the write and here. */
  char *canon_dest = canon(joined);
  const bool within = canon_dest != NULL && path_within(root, canon_dest);
  if (!within) {
    kbc_err_set(err, KBC_ERR_INVALID, "capture %s resolved outside corpus %s",
                r->path, cc->name);
  }
  free(canon_dest);
  free(joined);
  free(root);
  return within ? KBC_OK : KBC_ERR_INVALID;
}

/* The route. `content_type` and `x_requested_by` ride on req_ctx because
 * kbc_request is the frozen contract (httpd.h) and carries neither — the
 * same reason the second token carrier rides there. A socketless
 * kbc_httpd_handle leaves them "", which is not a degraded mode: a capture
 * with no Content-Type has no boundary and is a 400, exactly as it would be
 * over a socket. */
static kbc_status route_capture(kbc_app *app, const kbc_config *cfg,
                                const char *kb, const char *content_type,
                                const char *x_requested_by, const char *body,
                                size_t body_len, kbc_response *out,
                                kbc_err *err) {
  const kbc_corpus_cfg *cc = kbc_config_corpus(cfg, kb);
  if (cc == NULL) {
    return resp_error(out, 404, KBC_ERR_NOTFOUND, "no corpus named %s", kb);
  }
  if (body == NULL || body_len == 0) {
    return resp_error(out, 400, KBC_ERR_INVALID,
                      "capture needs a multipart/form-data body");
  }
  /* capture.rs:283-299, reject_oversized_request. Checked against the body
   * as received, before any parsing, so an oversized batch is one cheap
   * comparison rather than a parse that allocates for a request already known
   * to be too big. */
  if (body_len > (size_t)CAPTURE_MAX_REQUEST_BYTES) {
    return resp_error(out, 413, KBC_ERR_INVALID,
                      "combined upload exceeds the %u byte request budget",
                      (unsigned)CAPTURE_MAX_REQUEST_BYTES);
  }

  /* One arena for the request: the decoded parts are copies of the body, and
   * the body's own bytes belong to the connection buffer. */
  kbc_arena *a = kbc_arena_new(body_len + 4096u);
  if (a == NULL) {
    return resp_error(out, 500, KBC_ERR_NOMEM, "capture: no arena");
  }
  kbc_status st = KBC_OK;
  /* Every created document, in request order. A kbc_capture_result is ~4 KiB
   * of path, so 50 of them is 200 KiB — an arena, never a worker stack. */
  kbc_capture_result *items =
      kbc_arena_alloc(a, CAPTURE_MAX_FILES * sizeof *items);
  if (items == NULL) {
    st = resp_error(out, 500, KBC_ERR_NOMEM, "capture: %u results",
                    (unsigned)CAPTURE_MAX_FILES);
    goto done;
  }
  size_t n_items = 0;

  kbc_multipart_part *parts =
      kbc_arena_alloc(a, CAPTURE_MAX_PARTS * sizeof *parts);
  if (parts == NULL) {
    st = resp_error(out, 500, KBC_ERR_NOMEM, "capture: %u parts",
                    (unsigned)CAPTURE_MAX_PARTS);
    goto done;
  }
  size_t n_parts = 0;
  st = kbc_multipart_parse(content_type, body, body_len, parts,
                           CAPTURE_MAX_PARTS, &n_parts, a, err);
  if (kbc_failed(st)) {
    /* The decoder's ONE contract about *n_out: it is the number of parts in
     * the body on success AND on failure, and out[0..min(n,cap)) is readable
     * either way. So an overflow names itself — a caller told "too many parts"
     * can shrink the request, a caller told "malformed" cannot — and the two
     * must not be conflated, because only one of them is fixable by retrying
     * with less in it. Both are 400 here, as they are in the original
     * (capture.rs:190-196, :223-228). */
    const bool overflow = strstr(err_msg(err, st), "more than") != NULL;
    (void)resp_error(out, 400, st, "multipart: %s",
                     overflow ? "too many parts in one capture (max 56)"
                              : err_msg(err, st));
    st = KBC_OK;
    goto done;
  }

  /* --- the text fields, in one pass over the parts ---------------------- */
  const char *title = NULL, *from_field = NULL, *url = NULL, *text = NULL;
  const char **tags = NULL;
  size_t n_tags = 0;
  const kbc_multipart_part *files[CAPTURE_MAX_FILES];
  size_t n_files = 0;
  for (size_t i = 0; i < n_parts; i++) {
    const char *name = parts[i].name;
    /* A part's bytes may carry NULs (rule 8), so the length travels with the
     * string — and a text field that does is malformed, not a shorter
     * string: `str*` on it would silently read a prefix. */
    char *val = kbc_arena_strndup(a, parts[i].data, parts[i].len);
    if (val == NULL) {
      st = resp_error(out, 500, KBC_ERR_NOMEM, "capture: part %zu", i);
      goto done;
    }
    if (strlen(val) != parts[i].len) {
      st = resp_error(out, 400, KBC_ERR_INVALID,
                       "multipart: field `%s` contains a NUL", name);
      goto done;
    }
    if (strcmp(name, "files") == 0) {
      /* capture.rs:220-222: an empty part is not a file. */
      if (parts[i].len == 0) continue;
      if (n_files >= CAPTURE_MAX_FILES) {
        st = resp_error(out, 400, KBC_ERR_INVALID,
                        "too many files in one capture (max %u)",
                        (unsigned)CAPTURE_MAX_FILES);
        goto done;
      }
      files[n_files++] = &parts[i];
      continue;
    }
    char *v = trim_ws(val);
    if (strcmp(name, "title") == 0) {
      title = v[0] != '\0' ? v : NULL; /* non_empty, capture.rs:154-157 */
    } else if (strcmp(name, "from") == 0) {
      from_field = v[0] != '\0' ? v : NULL;
    } else if (strcmp(name, "url") == 0) {
      url = v[0] != '\0' ? v : NULL;
    } else if (strcmp(name, "text") == 0) {
      text = v[0] != '\0' ? v : NULL;
    } else if (strcmp(name, "tags") == 0) {
      st = capture_tags(a, v, &tags, &n_tags, err);
      if (kbc_failed(st)) {
        st = resp_error(out, 500, st, "capture: tags");
        goto done;
      }
    }
    /* `sanitize` and every unknown field are read past deliberately. The
     * original answers `sanitize` with a bool it only spends on HTML
     * (capture.rs:307), and there is no HTML pipeline here — see
     * capture_ext_indexable. An unknown field is ignored there too
     * (capture.rs:247, `_ => {}`). */
  }

  const char *from = from_field != NULL ? from_field
                                         : capture_from_default(a,
                                                                x_requested_by);

  /* --- validate the whole batch, then write it -------------------------- */
  for (size_t i = 0; i < n_files; i++) {
    if (files[i]->len > (size_t)CAPTURE_MAX_FILE_BYTES) {
      st = resp_error(out, 413, KBC_ERR_INVALID,
                      "capture file %zu exceeds the %u byte per-file limit",
                      i, (unsigned)CAPTURE_MAX_FILE_BYTES);
      goto done;
    }
    if (!capture_ext_indexable(files[i]->filename)) {
      st = resp_error(out, 415, KBC_ERR_INVALID,
                      "`%s` has no indexable capture extension for this kb "
 "(only .md and .markdown: this port has no HTML capture pipeline)",
                      files[i]->filename != NULL ? files[i]->filename : "");
      goto done;
    }
  }

  if (n_files == 0) {
    /* capture.rs:344-364: no file but a url/text share is a STUB, and no
     * file and neither is a 400 rather than an empty success. The url is
     * NEVER dereferenced — that is the project's SSRF ruling, not an
     * omission, and app.h says so where a future reader will look. */
    if (url == NULL && text == NULL) {
      st = resp_error(out, 400, KBC_ERR_INVALID,
                      "capture requires at least one file, or a url/text "
                      "share");
      goto done;
    }
    kbc_capture_url_input u;
    memset(&u, 0, sizeof u);
    u.corpus = kb;
    /* capture_dir is NEVER taken from the request. The original resolves it
     * from the kb's config (capture.rs:258-271); there is no config key here,
     * so NULL is the only spelling and the field is left absent rather than
     * offered and refused. */
    u.capture_dir = NULL;
    u.from = from;
    u.title = title;
    u.url = url;
    u.text = text;
    u.tags = tags;
    u.n_tags = n_tags;
    u.now_unix = 0; /* the wall clock; the frozen second is a test's lever */
    kbc_capture_result r;
    st = kbc_app_capture_url_stub(app, kb, &u, &r, err);
    if (kbc_failed(st)) {
      st = resp_error(out, st == KBC_ERR_NOTFOUND ? 404 : 400, st,
                      "capture stub: %s", err_msg(err, st));
      goto done;
    }
    st = capture_contained(cc, &r, err);
    if (kbc_failed(st)) {
      st = resp_error(out, 500, st, "capture: %s", err_msg(err, st));
      goto done;
    }
    items[n_items++] = r;
  } else {
    for (size_t i = 0; i < n_files; i++) {
      kbc_capture_input in;
      memset(&in, 0, sizeof in);
      in.corpus = kb;
      in.capture_dir = NULL; /* as above: never request-derived */
      in.from = from;
      /* `title` steers the OUTPUT FILENAME and is never stamped into the
       * document — app.h is the contract and explains why (a capture
       * preserves the uploaded file's own authored title). */
      in.title = title;
      in.url = url; /* provenance only; NEVER dereferenced */
      in.original_filename = files[i]->filename;
      in.category = NULL;   /* -> "capture" */
      in.session_id = NULL; /* no kb-session stamp */
      in.tags = tags;
      in.n_tags = n_tags;
      in.has_expires_at = false;
      in.body = files[i]->data;
      in.body_len = files[i]->len;
      in.ext = capture_ext_of(files[i]->filename);
      in.now_unix = 0;
      kbc_capture_result r;
      st = kbc_app_capture(app, kb, &in, &r, err);
      if (kbc_failed(st)) {
        /* capture.rs:113-122, map_capture_error: a read-only corpus is a 409
         * and not a 500, because the request was well formed and the
         * destination is the problem — telling a client "internal error"
         * sends it looking for a daemon bug that is not there. */
        int status = st == KBC_ERR_CONFLICT ? 409 : st == KBC_ERR_IO ? 500 : 400;
        st = resp_error(out, status, st, "capture: %s", err_msg(err, st));
        goto done;
      }
      st = capture_contained(cc, &r, err);
      if (kbc_failed(st)) {
        st = resp_error(out, 500, st, "capture: %s", err_msg(err, st));
        goto done;
      }
      items[n_items++] = r;
    }
  }

  /* 201 CREATED, per capture.rs:436-440. The items array is the whole
   * response: a caller that sent N files is told about N documents, in the
   * order it sent them — the order of the multipart parts, not a directory
   * listing's. */
  st = kbc_str_puts(&out->body, "{\"items\":[");
  for (size_t i = 0; st == KBC_OK && i < n_items; i++) {
    if (i > 0) st = kbc_str_putc(&out->body, ',');
    if (st == KBC_OK) st = capture_item_json(&out->body, kb, &items[i]);
  }
  if (kbc_failed(st)) {
    kbc_str_clear(&out->body);
    goto done;
  }
  st = kbc_str_puts(&out->body, "]}");
  if (kbc_failed(st)) {
    kbc_str_clear(&out->body);
    goto done;
  }
  out->status = 201;

done:
  kbc_arena_free(a);
  return st;
}

/* ------------------------------------------------------------ the registry --
 *
 * One array of routes, fixed before the daemon serves anything, that twenty
 * subsystems can extend without anyone editing this file. A subsystem that
 * implements a feature calls kbc_httpd_routes_add from its initialiser and is
 * done: the dispatcher finds its handler, the derived view lists its route,
 * and the daemon's own switch never learns the route exists.
 *
 * WHY THERE IS NO LOCK ON THE READ PATH. The array is written by exactly one
 * function, kbc_httpd_routes_add, and that function REFUSES to run once the
 * daemon has started: kbc_httpd_start sets `g_registry_sealed` and
 * kbc_httpd_stop clears it, and add() returns KBC_ERR_CONFLICT while it is
 * set. So there is no window in which a request can observe a half-written
 * table, which is the whole safety argument — the dispatcher reads
 * g_view/g_view_n with plain loads and takes no lock, and it is correct to.
 *
 * What would break it, and must therefore stay forbidden: registering from a
 * request path (a handler, or a lazy first-call registration), registering
 * after kbc_httpd_start, or registering from a signal handler. Each of those
 * puts a write concurrent with the reads above. The seal is what makes them
 * impossible rather than merely discouraged.
 *
 * WHY THE STORAGE IS STATIC AND BOUNDED. KBC_ROUTES is declared
 * `const kbc_route *const` (httpd.h:127) — a pointer to const rows, itself
 * const — so it has to be initialised by a LINK-TIME constant and can never be
 * repointed. A heap array that grows with realloc cannot be aliased by such a
 * pointer, so the derived view is a static array and the registry is bounded
 * by KBC_ROUTE_VIEW_CAP rather than grown on demand. The same const-ness is
 * why KBC_ROUTES_LEN is the BUILT-IN row count and not the live count: a
 * `const size_t` is fixed at link time too. The live count is what
 * kbc_httpd_routes() reports, and the two agree on every row they share,
 * because both are the same storage.
 *
 * The handler lives in a parallel array rather than in the row because
 * kbc_route is the published, frozen shape (httpd.h:79-84) and a function
 * pointer in it would change an ABI twenty subsystems are about to compile
 * against. Both arrays are indexed by the same i, and both are written only
 * by add(). */

#define KBC_ROUTE_SEED_LEN 18u
#define KBC_ROUTE_VIEW_CAP 128u

/* The daemon's own rows, written down ONCE. A registered route is appended
 * after them, so the first KBC_ROUTE_SEED_LEN entries of the view are these,
 * in this order, forever — which is what lets KBC_ROUTES keep pointing at
 * slot 0 and keep meaning what it meant before the registry existed.
 *
 * Two of these rows are ORIGIN-scoped rather than path-scoped, and the path
 * column says so: a request is routed by its `Host:` before it is routed by
 * its path (`routes/dispatch.rs:46-50`). They are listed with `needs_auth`
 * false because the Host is chosen by the client and is therefore not
 * admission control — the same posture the original takes, where the origin
 * split sits outside the /api auth layer (`router.rs:1000-1025`). */
static kbc_route g_view[KBC_ROUTE_VIEW_CAP] = {
    {"GET", "/api/health", "liveness, version, uptime, indexed doc count",
     false},
    {"GET", "/api/identity",
     "resolved identity and its source; attribution, not authorization", true},
    {"GET", "/api/kbs", "configured corpora with doc counts, ?kb= filters one",
     true},
    {"GET", "/api/stats", "daemon counters as JSON", true},
    {"GET", "/api/search", "search, ?q=&kb=&kind=&mode=&limit=&offset=", true},
    {"GET", "/api/artifacts", "list artifacts, ?kb=&kind=&limit=&offset=",
     true},
    {"GET", "/api/artifacts/{id}", "one artifact, ?source=1 adds the raw text",
     true},
    {"GET", "/api/kb/{kb}/artifact/{id}",
     "artifact bytes, sandboxed CSP + nosniff, ?download=1 attaches", true},
    {"GET", "/api/kb/{kb}/notes/{id}/links",
     "a document's outgoing wikilinks (state resolved|ambiguous|dangling, an "
     "ambiguous one carries no ids) and its backlinks",
     true},
    {"GET", "/api/kb/{kb}/backlinks/{id}",
     "documents linking to any artifact, notes first then by title; nothing "
     "linking here is an empty array, not a 404",
     true},
    {"GET", "/api/kb/{kb}/wikilinks/suggest",
     "[[ autocomplete over titles and basenames, ?q= required, ?limit= <= 50",
     true},
    {"POST", "/api/kb/{kb}/capture",
     "multipart upload into the corpus: `files` parts, or a `url`/`text` share "
     "with none; 201 with the created ids and source-relative paths, at most "
     "50 files and 10 MiB each, and a `url` is recorded, never fetched",
     true},
    {"POST", "/api/reindex", "synchronous full rescan, answers 202", true},
    {"GET", "/api/events",
     "server-sent event stream; Last-Event-ID replays, and an unusable cursor "
     "gets a synthetic `gap` frame instead (a stalled client gets `lag`)",
     true},
    {"GET", "/metrics", "Prometheus text exposition 0.0.4, no-store", true},
    {"GET", "/", "plain-text API banner (no reader UI in this port)", false},
    {"GET", "*", "parent-origin static files from KB_SPA_DIST, else 404",
     false},
    {"GET", "<id>.artifacts.localhost/*",
     "artifact subdomain: one origin per artifact, canonicalised under the "
     "corpus root",
     false},
};

/* NULL for every built-in row: those are served by the switch below, and a
 * NULL handler is what the dispatcher skips, so the switch keeps serving
 * everything nobody has claimed. Only a subsystem's row is non-NULL. */
static kbc_route_handler g_handlers[KBC_ROUTE_VIEW_CAP];
static size_t g_view_n = KBC_ROUTE_SEED_LEN;
static pthread_mutex_t g_registry_mu = PTHREAD_MUTEX_INITIALIZER;
static atomic_bool g_registry_sealed;

/* A template is well formed or it is refused at registration, so a
 * subsystem cannot ship a route that silently never matches. Rules, taken
 * from the original's router (matchit 0.8.4, tree.rs:350-400):
 *   - a segment is EITHER a literal OR a whole `{name}` / `{*name}`;
 *   - `{name}` is one segment and `{*name}` is the REST, so they are spelled
 *     differently on purpose: the original writes `/kb/{kb}/docs/by-path/
 *     {*path}` (router.rs:121) and never gives a bare `{name}` a multi-segment
 *     meaning. A trailing `{name}` that swallowed `/` would make
 *     `/api/artifacts/{id}` answer for `/api/artifacts/a/b`, which is a
 *     different document and a 404 in the original;
 *   - `{*name}` is only legal as the final segment (matchit returns
 *     InvalidCatchAll otherwise, tree.rs:373);
 *   - at most KBC_ROUTE_MAX_PARAMS parameters. The bound is enforced HERE,
 *     at registration, and not at match time, because the alternative is the
 *     one failure mode a routing layer must never have: a caller that shipped
 *     nine params, got five, and read a wrong value as a right one. A
 *     truncated match is a silent wrong answer; a refused registration is a
 *     startup error naming the route. */
static bool tmpl_well_formed(const char *t, size_t *n_params) {
  if (t == NULL || t[0] != '/') return false;
  size_t params = 0;
  if (t[1] == '\0') {
    *n_params = 0;
    return true; /* "/" */
  }
  const char *s = t + 1;
  for (;;) {
    /* s is at the start of a segment. The three ways to leave the loop are the
     * three ways a template can end, and they are checked where the segment
     * that ends it is — a template that ENDS in `{name}` is well formed, which
     * is the commonest shape there is, and only a literal '/' after it would
     * be the trailing-slash error. */
    if (*s == '{') {
      const char *close = strchr(s, '}');
      if (close == NULL || close == s + 1) return false; /* unclosed, or {} */
      /* Counted here rather than at match time: see the note on
       * KBC_ROUTE_MAX_PARAMS above. A catch-all counts like any other. */
      if (++params > KBC_ROUTE_MAX_PARAMS) {
        *n_params = params;
        return false;
      }
      if (s[1] == '*') { /* catch-all ends the template */
        *n_params = params;
        return close[1] == '\0';
      }
      const char *inner_open = strchr(s + 1, '{');
      if (inner_open != NULL && inner_open < close) return false; /* nested */
      s = close + 1;
      if (*s == '\0') { /* the template ends on this param */
        *n_params = params;
        return true;
      }
      if (*s != '/') return false; /* a param is a WHOLE segment */
      s++;
      continue;
    }
    if (*s == '}') return false; /* unmatched close */
    if (*s == '\0' || *s == '/') return false; /* trailing slash / empty seg */
    const char *slash = strchr(s, '/');
    if (slash == NULL) {
      *n_params = params;
      return true; /* a final literal segment */
    }
    s = slash + 1;
  }
}

/* Where a match's parameter values live.
 *
 * The values are COPIED out of the path rather than pointed at in place, and
 * the reason is the header's promise that each value is NUL-terminated: an
 * interior segment is not. In `/api/kb/notes/thing/abc` the bytes of `notes`
 * are followed by '/', not by a NUL, so a pointer into the path would hand a
 * handler a `const char *` that runs on into the next segment. A catch-all is
 * always last and so is already terminated by the path itself, but it is
 * copied too, so that every value has one lifetime and one owner.
 *
 * `scratch` is bounded by KBC_HTTP_MAX_REQUEST_LINE, which is exactly what a
 * request line can be, so a request that arrived over a socket can never
 * overflow it. A socketless caller may hand `kbc_httpd_handle` a longer path;
 * a bind that would not fit fails the match, giving a 404, rather than
 * truncating a value. That asymmetry is deliberate: a truncated value is a
 * silently WRONG answer, and a 404 is a safe one. */
typedef struct {
  kbc_route_params p;
  char *scratch;
  size_t cap;
  size_t used;
} route_bind;

static void bind_init(route_bind *b, char *scratch, size_t cap) {
  memset(&b->p, 0, sizeof b->p);
  b->scratch = scratch;
  b->cap = cap;
  b->used = 0;
}

/* Records one parameter. BOTH the name and the value are COPIED into the
 * scratch and NUL-terminated.
 *
 * The value needs copying because an interior segment is not terminated in
 * the path: in `/api/kb/notes/thing/abc` the bytes of `notes` are followed by
 * '/', so a pointer into the path would run on into the next segment.
 *
 * The name needs it for a reason that is easy to miss, and that a test caught:
 * the name lives inside the TEMPLATE, where it is followed by `}`. Handing a
 * handler that pointer would make `strcmp(name, "kb")` read "kb}/thing/{id}",
 * so a borrowed name is a name no handler can usefully compare. Copying both
 * fields gives every one of them a single owner and a single lifetime.
 *
 * A repeated name is NOT deduplicated: `{a}/x/{a}` is two entries, because a
 * handler that names a parameter twice means it, and collapsing them would
 * make the count depend on the template's spelling rather than the request. */
static bool bind_one(route_bind *b, const char *name, size_t nlen,
                     const char *value, size_t vlen) {
  if (b->p.n >= KBC_ROUTE_MAX_PARAMS) return false; /* unreachable: capped */
  if (b->used + nlen + 1 + vlen + 1 > b->cap) return false; /* see above */
  memcpy(b->scratch + b->used, name, nlen);
  b->scratch[b->used + nlen] = '\0';
  const char *name_copy = b->scratch + b->used;
  b->used += nlen + 1;
  memcpy(b->scratch + b->used, value, vlen);
  b->scratch[b->used + vlen] = '\0';
  b->p.v[b->p.n].name = name_copy;
  b->p.v[b->p.n].value = b->scratch + b->used;
  b->p.v[b->p.n].len = vlen;
  b->p.n++;
  b->used += vlen + 1;
  return true;
}

/* Template against concrete path, binding the parameters it matches.
 *
 * Segment-for-segment, and the segment counts must agree:
 * `/api/kb/{kb}/capture` does not match `/api/kb/notes` and does not match
 * `/api/kb/notes/capture/x`, because a path one segment short of a template
 * is not that route (the switch says the same about the id-shaped tails,
 * httpd.c:2994-3000).
 *
 * Case-sensitive on the method and on every literal segment, as the original
 * is: matchit compares bytes, and so does strcmp here. A `{name}` segment
 * matches exactly one NON-EMPTY segment (matchit rejects an empty one:
 * tree.rs:490-511 finds no value and backtracks), and `{*name}` matches a
 * non-empty remainder that may contain `/`, which is how by-path and the
 * artifact subtree work.
 *
 * `b` is left holding whatever was bound before a FAILED match, which is
 * harmless because the caller resets it per candidate route and only reads it
 * after a true return. */
static bool tmpl_match(const char *t, const char *p, route_bind *b) {
  if (t[0] != '/' || p[0] != '/') return false;
  t++;
  p++;
  for (;;) {
    if (*t == '\0') return *p == '\0';
    if (*p == '\0') return false;
    if (*t == '{') {
      const char *close = strchr(t, '}');
      if (close == NULL) return false;
      if (t[1] == '*') {
        if (close[1] != '\0' || *p == '\0') return false;
        /* The catch-all takes the whole remainder, slashes and all. */
        return bind_one(b, t + 2, (size_t)(close - t - 2), p, strlen(p));
      }
      /* One non-empty segment: it ends at the next '/', or at the end. */
      const char *pslash = strchr(p, '/');
      size_t plen = pslash != NULL ? (size_t)(pslash - p) : strlen(p);
      if (plen == 0) return false;
      if (!bind_one(b, t + 1, (size_t)(close - t - 1), p, plen)) return false;
      p = pslash != NULL ? pslash + 1 : p + plen;
      t = close + 1;
      if (*t == '/') t++;
      continue;
    }
    /* A literal segment. The two segment ends are independent — the template
     * can run out first, or the path can — and each case has to be answered
     * before advancing, because advancing past a NULL strchr result is the
     * classic `NULL + 1` read. A path that ends inside a longer template is
     * NOT a prefix match: `/api/registry/seg/kb/thing` is one segment short
     * of `/api/registry/seg/{kb}/thing/{id}` and names no route. */
    const char *tslash = strchr(t, '/');
    size_t tlen = tslash != NULL ? (size_t)(tslash - t) : strlen(t);
    const char *pslash = strchr(p, '/');
    size_t plen = pslash != NULL ? (size_t)(pslash - p) : strlen(p);
    if (tlen != plen) return false;
    if (tlen != 0 && memcmp(t, p, tlen) != 0) return false;
    if (tslash == NULL || pslash == NULL) return tslash == pslash;
    t = tslash + 1;
    p = pslash + 1;
  }
}

/* A registered route's OWN admission, enforced from its `needs_auth` and from
 * nothing else. It runs BEFORE check_auth, and that placement is the point: a
 * route that declares needs_auth cannot be served by accident, because the
 * flag on the matched row is the only thing consulted. Enforcement living
 * anywhere else — a list beside the switch, a path prefix — is exactly how a
 * registered route ends up public by omission.
 *
 * The ladder is check_auth's, called rather than restated, so a registered
 * route cannot drift from the daemon's own: both carriers, Authorization
 * first, and the same 401-for-none / 403-for-wrong split. With no token
 * configured there is no secret to present, and the daemon's posture is
 * loopback-only (kbc_config_bind_is_safe refuses a routable bind without one),
 * so that is what admits. */
static kbc_status reg_check_auth(const kbc_config *cfg, const kbc_request *req,
                                 const req_ctx *ctx, kbc_response *out) {
  if (cfg == NULL || cfg->token == NULL || cfg->token[0] == '\0') {
    if (addr_is_loopback(req->client_addr)) return KBC_OK;
    (void)kbc_response_error_json(out, 401, KBC_ERR_INVALID,
                                  "a token is required: configure one, or "
                                  "reach the daemon over loopback");
    return KBC_ERR_INVALID;
  }
  presented p = read_presented(req->auth, ctx->x_kb_token);
  auth_carrier carrier = CARRIER_NONE;
  if (first_matching_carrier(&p, cfg->token, &carrier)) return KBC_OK;
  if (!secret_is_set(&p.bearer) && !secret_is_set(&p.x_kb)) {
    (void)kbc_response_error_json(
        out, 401, KBC_ERR_INVALID,
        "a token is required: send it as `Authorization: Bearer <token>` or "
        "`X-Kb-Token: <token>`");
    return KBC_ERR_INVALID;
  }
  (void)kbc_response_error_json(out, 403, KBC_ERR_INVALID, "invalid token");
  return KBC_ERR_INVALID;
}

/* A match is a (method, template) row WITH a handler. A built-in row has a
 * NULL handler and is therefore never returned here, which is what keeps the
 * switch serving it: this is the whole of "additive". */
typedef struct {
  const kbc_route *route;
  kbc_route_handler handler;
  kbc_route_params params; /* the bound values; the storage is the caller's
                            * scratch, valid for the handler call only */
} reg_hit;

/* Finds the route for (method, path) and binds its parameters.
 *
 * `b` is re-initialised PER CANDIDATE, not once for the whole scan: a route
 * that matched its first two segments and then failed on the third would
 * otherwise leave those two values behind for the next candidate to inherit,
 * and the winning route would be handed parameters that belong to a route
 * that lost. Only the values of the route that actually wins survive. */
static bool registry_lookup(const char *m, const char *p, reg_hit *hit,
                            char *scratch, size_t cap) {
  route_bind b;
  for (size_t i = 0; i < g_view_n; i++) {
    if (g_handlers[i] == NULL) continue;
    if (strcmp(g_view[i].method, m) != 0) continue;
    bind_init(&b, scratch, cap);
    if (!tmpl_match(g_view[i].path, p, &b)) continue;
    hit->route = &g_view[i];
    hit->handler = g_handlers[i];
    hit->params = b.p;
    return true;
  }
  return false;
}

kbc_status kbc_httpd_routes_add(const kbc_route_entry *routes, size_t n,
                                kbc_err *err) {
  if (n > 0 && routes == NULL)
    return kbc_err_set(err, KBC_ERR_INVALID, "kbc_httpd_routes_add: routes");
  /* Validate everything before taking the lock or writing anything, so a
   * rejected batch leaves the table exactly as it was. */
  for (size_t i = 0; i < n; i++) {
    const kbc_route_entry *e = &routes[i];
    if (e->method == NULL || e->method[0] == '\0')
      return kbc_err_set(err, KBC_ERR_INVALID, "route %zu: no method", i);
    if (e->summary == NULL)
      return kbc_err_set(err, KBC_ERR_INVALID, "route %zu: no summary", i);
    size_t n_params = 0;
    if (!tmpl_well_formed(e->path, &n_params)) {
      /* Too many parameters is its OWN message, and it is checked before the
       * generic one, because "not a valid path template" would send someone
       * looking for a syntax error in a template that is perfectly well
       * formed. The cap is the reason this is refused at all rather than
       * truncated at match time. */
      if (n_params > KBC_ROUTE_MAX_PARAMS)
        return kbc_err_set(err, KBC_ERR_INVALID,
                           "route %zu: %s has %zu parameters, over the "
                           "KBC_ROUTE_MAX_PARAMS limit of %d",
                           i, e->path != NULL ? e->path : "(null)", n_params,
                           KBC_ROUTE_MAX_PARAMS);
      return kbc_err_set(err, KBC_ERR_INVALID,
                         "route %zu: %s is not a valid path template", i,
                         e->path != NULL ? e->path : "(null)");
    }
  }
  pthread_mutex_lock(&g_registry_mu);
  if (g_registry_sealed) {
    pthread_mutex_unlock(&g_registry_mu);
    return kbc_err_set(err, KBC_ERR_CONFLICT,
                       "kbc_httpd_routes_add: the daemon is serving; the route "
                       "table is fixed once start-up is over");
  }
  for (size_t i = 0; i < n; i++) {
    /* A duplicate is refused rather than resolved by array order: two
     * handlers claiming one route is a bug, and "whichever registered first"
     * is not an answer. The built-in rows are IN this space, so a subsystem
     * cannot quietly take over a route the switch still serves. */
    for (size_t j = 0; j < g_view_n; j++) {
      if (strcmp(g_view[j].method, routes[i].method) == 0 &&
          strcmp(g_view[j].path, routes[i].path) == 0) {
        pthread_mutex_unlock(&g_registry_mu);
        return kbc_err_set(err, KBC_ERR_INVALID,
                           "route %zu: %s %s is already registered", i,
                           routes[i].method, routes[i].path);
      }
    }
    if (g_view_n >= KBC_ROUTE_VIEW_CAP) {
      pthread_mutex_unlock(&g_registry_mu);
      return kbc_err_set(err, KBC_ERR_NOMEM,
                         "kbc_httpd_routes_add: the route table is full (%u "
                         "rows); raise KBC_ROUTE_VIEW_CAP",
                         KBC_ROUTE_VIEW_CAP);
    }
    g_view[g_view_n].method = routes[i].method;
    g_view[g_view_n].path = routes[i].path;
    g_view[g_view_n].summary = routes[i].summary;
    g_view[g_view_n].needs_auth = routes[i].needs_auth;
    g_handlers[g_view_n] = routes[i].handler;
    g_view_n++;
  }
  pthread_mutex_unlock(&g_registry_mu);
  return KBC_OK;
}

const kbc_route *kbc_httpd_routes(size_t *n_out) {
  if (n_out != NULL) *n_out = g_view_n;
  return g_view;
}

static kbc_status dispatch(kbc_app *app, const kbc_config *cfg,
                           const kbc_request *req, const req_ctx *ctx,
                           kbc_response *out, kbc_err *err, int64_t uptime_s) {
  const char *m = req->method;
  const char *p = req->path;
  bool is_get = strcmp(m, "GET") == 0;
  bool is_post = strcmp(m, "POST") == 0;
  /* A route's own header lines are built here and published to the response
   * only on success, so an error path can never leave a half-written header
   * set on a problem+json answer. */
  kbc_str local_hdrs;
  kbc_str_init(&local_hdrs);
  kbc_str *hdrs = &local_hdrs;
  kbc_status result = KBC_OK;
  /* THE REGISTRY, consulted before the switch. A registered handler WINS; the
   * switch keeps serving everything nobody has claimed, which is what makes
   * registration additive — no route has to move out of the switch to be
   * implemented, and a subsystem that ships a handler never touches this
   * file again.
   *
   * It sits AFTER the two unauthenticated routes and BEFORE check_auth,
   * because a registered route brings its own admission: reg_check_auth
   * decides from the matched row's `needs_auth` and nothing else. Running the
   * daemon's global ladder first would have applied the /api rule to routes
   * the daemon knows nothing about, and would have left `needs_auth: false`
   * unable to say anything at all.
   *
   * `ctx->unmatched` is deliberately NOT set: a registered route that
   * matched is not a candidate for the origin fallback, however it answered. */
  /* The bound parameter values live in a caller-owned buffer, sized to the
   * request line because that is the most a path can be over a socket. It is
   * a plain stack array, so nothing is allocated to pass a request's own
   * parameters, and it dies with this frame — which is exactly the lifetime
   * the handler is told its values have. */
  char param_scratch[KBC_HTTP_MAX_REQUEST_LINE + 1];

  reg_hit hit;
  if (registry_lookup(m, p, &hit, param_scratch, sizeof param_scratch)) {
    if (hit.route->needs_auth &&
        kbc_failed(reg_check_auth(cfg, req, ctx, out))) {
      goto done;
    }
    result = hit.handler(app, req, &hit.params, out, err);
    /* A handler owns its response: status, content type and body are whatever
     * it set, and `out` was already initialised for it. A handler that
     * returns a failure WITHOUT having written a body is a bug in the
     * handler, and the daemon's own answer for that is the 500 below rather
     * than a 200 with an empty body. */
    if (kbc_failed(result) && out->body.len == 0) {
      kbc_str_clear(&out->body);
      result = resp_error(out, 500, result, "%s %s: %s", m, p, err_msg(err, result));
    }
    goto done;
  }

  if (is_get && strcmp(p, "/") == 0) {
    out->content_type = CT_TEXT;
    result = route_banner(&out->body);
    goto done;
  }
  if (is_get && strcmp(p, "/api/health") == 0) {
    result = route_health(app, &out->body, uptime_s);
    goto done;
  }


  auth_tier tier = TIER_OPEN;
  auth_carrier carrier = CARRIER_NONE;
  if (kbc_failed(check_auth(cfg, req, ctx->x_kb_token, out, &tier, &carrier))) {
    goto done;
  }

  if (strcmp(p, "/api/identity") == 0) {
    if (!is_get) {
      result = method_not_allowed(out, m, p);
      goto done;
    }
    /* No arena: this route decodes nothing and allocates nothing a request
     * outlives. */
    kbc_status st =
        route_identity(req->query, req, tier, carrier, &out->body, err);
    if (kbc_failed(st)) {
      result = resp_error(out, 400, st, "%s", err_msg(err, st));
      goto done;
    }
    /* This is the one route whose 200 body depends on WHICH credential the
     * caller presented — the `source` and `carrier` members are the answer to
     * exactly that question — so it is the one response that must declare the
     * credential headers it varies on. Without them a shared cache is free to
     * hand one caller's identity to another, which leaks no secret but does
     * leak an attribution. Vary is a set, so this combines with the
     * `Vary: Origin` the write path puts on every response.
     *
     * Authorization was already a varying input here and was not declared; it
     * is named now for the same reason, not because the second carrier
     * introduced it. The other /api routes answer 200 with a body that does
     * not depend on the carrier, so they are left alone. */
    result = kbc_str_puts(hdrs, "Vary: Authorization, X-Kb-Token\r\n");
    goto done;
  }
  if (strcmp(p, "/api/kbs") == 0) {
    if (!is_get) {
      result = method_not_allowed(out, m, p);
      goto done;
    }
    kbc_arena *a = kbc_arena_new(16384);
    if (a == NULL) {
      result = resp_error(out, 500, KBC_ERR_NOMEM, "no arena");
      goto done;
    }
    kbc_status st = route_kbs(app, a, req->query, cfg, &out->body, err);
    kbc_arena_free(a);
    if (st == KBC_ERR_NOTFOUND) {
      result = resp_error(out, 404, st, "%s", err_msg(err, st));
      goto done;
    }
    if (kbc_failed(st)) {
      result = resp_error(out, 400, st, "%s", err_msg(err, st));
      goto done;
    }
    goto done;
  }

  if (strcmp(p, "/api/stats") == 0) {
    if (!is_get) {
      result = method_not_allowed(out, m, p);
      goto done;
    }
    kbc_status st = route_stats(app, &out->body, err);
    if (kbc_failed(st)) {
      result = resp_error(out, 500, st, "stats: %s", err_msg(err, st));
      goto done;
    }
    goto done;
  }
  if (strcmp(p, "/api/search") == 0) {
    if (!is_get) {
      result = method_not_allowed(out, m, p);
      goto done;
    }
    kbc_arena *a = kbc_arena_new(16384);
    if (a == NULL) {
      result = resp_error(out, 500, KBC_ERR_NOMEM, "no arena");
      goto done;
    }
    kbc_status st = route_search(app, cfg, a, req->query, &out->body, err);
    kbc_arena_free(a);
    if (kbc_failed(st)) {
      result = resp_error(out, 400, st, "%s", err_msg(err, st));
      goto done;
    }
    goto done;
  }
  if (strcmp(p, "/api/artifacts") == 0) {
    if (!is_get) {
      result = method_not_allowed(out, m, p);
      goto done;
    }
    kbc_arena *a = kbc_arena_new(16384);
    if (a == NULL) {
      result = resp_error(out, 500, KBC_ERR_NOMEM, "no arena");
      goto done;
    }
    kbc_status st = route_artifacts(app, a, req->query, &out->body, err);
    kbc_arena_free(a);
    if (kbc_failed(st)) {
      result = resp_error(out, 400, st, "%s", err_msg(err, st));
      goto done;
    }
    goto done;
  }
  static const char kOne[] = "/api/artifacts/";
  if (strncmp(p, kOne, sizeof kOne - 1) == 0 && p[sizeof kOne - 1] != '\0') {
    if (!is_get) {
      result = method_not_allowed(out, m, p);
      goto done;
    }
    const char *id = p + sizeof kOne - 1;
    if (strchr(id, '/') != NULL) {
      result = resp_error(out, 404, KBC_ERR_NOTFOUND, "no route for %s", p);
      goto done;
    }
    kbc_arena *a = kbc_arena_new(16384);
    if (a == NULL) {
      result = resp_error(out, 500, KBC_ERR_NOMEM, "no arena");
      goto done;
    }
    kbc_status st = route_artifact_one(app, a, id, req->query, &out->body, err);
    kbc_arena_free(a);
    if (st == KBC_ERR_NOTFOUND) {
      result = resp_error(out, 404, st, "no artifact with id %s", id);
      goto done;
    }
    if (kbc_failed(st)) {
      result = resp_error(out, 400, st, "%s", err_msg(err, st));
      goto done;
    }
    goto done;
  }
  if (strcmp(p, "/api/reindex") == 0) {
    if (!is_post) {
      result = method_not_allowed(out, m, p);
      goto done;
    }
    kbc_status st = route_reindex(app, ctx->h, &out->body, err);
    if (kbc_failed(st)) {
      result = resp_error(out, 500, st, "reindex: %s", err_msg(err, st));
      goto done;
    }
    out->status = 202;
    goto done;
  }
  /* Top-level, not under /api: the original mounts it on its own router so the
   * api tree's auth does not re-wrap it, and so it answers on EVERY origin
   * (`router.rs:1003-1025`). Being outside that tree is also why it is neither
   * counted nor classified as an API route below. */
  if (strcmp(p, "/metrics") == 0) {
    if (!is_get) {
      result = method_not_allowed(out, m, p);
      goto done;
    }
    /* A socketless call has no daemon and therefore no counters; the
     * fixed-cardinality families still render, at 0. */
    metrics_reg zero;
    memset(&zero, 0, sizeof zero);
    kbc_status st = route_prometheus(app, ctx->m != NULL ? ctx->m : &zero,
                                     hdrs, &out->body, err);
    if (kbc_failed(st)) {
      kbc_str_clear(hdrs);
      result = resp_error(out, 500, st, "metrics: %s", err_msg(err, st));
      goto done;
    }
    out->content_type = KBC_PROM_CONTENT_TYPE;
    goto done;
  }
  /* `/api/kb/{kb}/…` — every corpus-scoped route, split on the SECOND
   * segment. The kb name is a path SEGMENT, copied out rather than
   * NUL-terminated in place: `req.path` is the connection's parsed target and
   * the fallback below still reads it. */
  static const char kKb[] = "/api/kb/";
  if (strncmp(p, kKb, sizeof kKb - 1) == 0) {
    const char *rest = p + sizeof kKb - 1;
    const char *slash = strchr(rest, '/');
    char kb[256];
    if (slash != NULL) {
      size_t kbl = (size_t)(slash - rest);
      if (kbl == 0 || kbl >= sizeof kb) {
        result = resp_error(out, 404, KBC_ERR_NOTFOUND, "no route for %s", p);
        goto done;
      }
      memcpy(kb, rest, kbl);
      kb[kbl] = '\0';
    }
    const char *tail = slash != NULL ? slash + 1 : rest;
    /* One id-shaped tail: `artifact/{id}`, `backlinks/{id}`, and the
     * `{id}/links` of a note. Each is matched with its full template, so a
     * path one segment short of a template falls through to the 404 below
     * rather than being served by a longer route's prefix.
     *
     * The note's id is the only one that is not the tail's remainder — it is
     * the tail minus its `/links` — so it is copied into a buffer bounded by
     * KBC_MAX_ID_LEN and refused outright when it does not fit. Truncating it
     * would turn a wrong id into a RIGHT id for a different document, which
     * is the one answer a lookup must never give. */
    const char *id = NULL;
    char idbuf[KBC_MAX_ID_LEN + 1];
    int link_route = 0; /* 0 artifact bytes, 1 backlinks, 2 note links */
    if (slash != NULL) {
      static const char kArtifact[] = "artifact/";
      static const char kBacklinks[] = "backlinks/";
      static const char kNotes[] = "notes/";
      static const char kLinks[] = "/links";
      if (strncmp(tail, kArtifact, sizeof kArtifact - 1) == 0) {
        id = tail + sizeof kArtifact - 1;
      } else if (strncmp(tail, kBacklinks, sizeof kBacklinks - 1) == 0) {
        id = tail + sizeof kBacklinks - 1;
        link_route = 1;
      } else if (strncmp(tail, kNotes, sizeof kNotes - 1) == 0) {
        const char *nid = tail + sizeof kNotes - 1;
        size_t nl = strlen(nid);
        size_t ll = sizeof kLinks - 1;
        if (nl > ll && strcmp(nid + nl - ll, kLinks) == 0 &&
            nl - ll <= KBC_MAX_ID_LEN) {
          memcpy(idbuf, nid, nl - ll);
          idbuf[nl - ll] = '\0';
          id = idbuf;
          link_route = 2;
        }
      }
      /* An id is ONE segment. `artifact/a/b` and `backlinks/a/b` name no
       * route, and answering them with a lookup for the whole remainder
       * would be a store query whose 404 means something else. */
      if (id != NULL && (id[0] == '\0' || strchr(id, '/') != NULL)) {
        result = resp_error(out, 404, KBC_ERR_NOTFOUND, "no route for %s", p);
        goto done;
      }
    }
    if (id != NULL) {
      /* `wikilinks/suggest` is corpus-scoped but takes no id, and it is the
       * one kb route whose query is REQUIRED rather than filtered, so it is
       * matched before the id shape is decided on. */
      if (!is_get) {
        result = method_not_allowed(out, m, p);
        goto done;
      }
      kbc_arena *a = kbc_arena_new(16384);
      if (a == NULL) {
        result = resp_error(out, 500, KBC_ERR_NOMEM, "no arena");
        goto done;
      }
      kbc_status st;
      int status = 200;
      if (link_route == 0) {
        st = route_artifact_bytes(app, a, cfg, kb, id, req->query, hdrs,
                                  &out->body, err);
        /* The answers are the original's: an unknown kb or id is a 404, a
         * file that will not read is a 500 (`Io(_) => 500`), and a source that
         * cannot be rendered under a text/html label is a 400. An allocation
         * failure is a 500 too, and saying 400 would be a lie about whose
         * fault it is — a render that runs out of memory is the daemon's
         * problem, not the caller's, and the renderer reports it as
         * KBC_ERR_NOMEM precisely so this mapping can be honest. */
        status = st == KBC_ERR_NOTFOUND ? 404
                 : st == KBC_ERR_IO || st == KBC_ERR_NOMEM ? 500
                                                            : 400;
        if (st == KBC_OK) out->content_type = "text/html; charset=utf-8";
      } else if (link_route == 1) {
        st = route_backlinks(app, a, cfg, kb, id, &out->body, err);
        /* An unknown corpus or id is a 404; a bad `?limit=`-shaped query is
         * the caller's error. Nothing here is a 500 the caller can act on. */
        status = st == KBC_ERR_NOTFOUND ? 404
                 : st == KBC_ERR_IO || st == KBC_ERR_NOMEM ? 500
                                                            : 400;
      } else {
        st = route_note_links(app, a, cfg, kb, id, &out->body, err);
        status = st == KBC_ERR_NOTFOUND ? 404
                 : st == KBC_ERR_IO || st == KBC_ERR_NOMEM ? 500
                                                            : 400;
      }
      kbc_arena_free(a);
      if (kbc_failed(st)) {
        kbc_str_clear(hdrs);
        result = resp_error(out, status, st, "%s", err_msg(err, st));
        goto done;
      }
      goto done;
    }
    if (slash != NULL) {
      static const char kSuggest[] = "wikilinks/suggest";
      if (strcmp(tail, kSuggest) == 0) {
        if (!is_get) {
          result = method_not_allowed(out, m, p);
          goto done;
        }
        if (kbc_config_corpus(cfg, kb) == NULL) {
          result = resp_error(out, 404, KBC_ERR_NOTFOUND, "no corpus named %s",
                              kb);
          goto done;
        }
        kbc_arena *a = kbc_arena_new(16384);
        if (a == NULL) {
          result = resp_error(out, 500, KBC_ERR_NOMEM, "no arena");
          goto done;
        }
        kbc_status st =
            route_wikilinks_suggest(app, a, kb, req->query, &out->body, err);
        kbc_arena_free(a);
        if (kbc_failed(st)) {
          result = resp_error(out, st == KBC_ERR_NOMEM ? 500 : 400, st, "%s",
                              err_msg(err, st));
          goto done;
        }
        goto done;
      }
      /* `capture` is corpus-scoped and takes no id, so it is matched here
       * beside the other id-free kb route rather than in the id shape above.
       * POST only: the original mounts it with `post(...)` and there is no
       * GET on this path, so a GET is a 405 and not a read surface nobody
       * wrote. */
      static const char kCapture[] = "capture";
      if (strcmp(tail, kCapture) == 0) {
        if (!is_post) {
          result = method_not_allowed(out, m, p);
          goto done;
        }
        result = route_capture(app, cfg, kb, ctx->content_type,
                               ctx->x_requested_by, req->body, req->body_len,
                               out, err);
        goto done;
      }
    }
  }
  if (strcmp(p, "/api/events") == 0) {
    if (!is_get) {
      result = method_not_allowed(out, m, p);
      goto done;
    }
    /* Over a socket this becomes a live stream; a socketless caller gets the
     * head of that stream, which is the whole non-streaming part of it. */
    out->status = 200;
    out->content_type = CT_SSE;
    out->sse = true;
    result = kbc_str_puts(&out->body, "retry: 3000\n\n:ok\n\n");
    goto done;
  }
  if (ctx->unmatched != NULL) *ctx->unmatched = true;
  result = resp_error(out, 404, KBC_ERR_NOTFOUND, "no route for %s %s", m, p);
done:
  /* Publish the route's own header lines only when the route SUCCEEDED, so a
   * 404 problem+json can never carry an artifact id or a sandbox CSP. */
  if (ctx->extra != NULL && local_hdrs.len > 0 &&
      kbc_failed(kbc_str_append(ctx->extra, local_hdrs.ptr, local_hdrs.len))) {
    kbc_str_clear(ctx->extra);
  }
  kbc_str_free(&local_hdrs);
  return result;
}

kbc_status kbc_httpd_handle(kbc_app *app, const kbc_config *cfg,
                            const kbc_request *req, kbc_response *out,
                            kbc_err *err) {
  if (req == NULL || out == NULL) {
    return kbc_err_set(err, KBC_ERR_INVALID, "kbc_httpd_handle: %s",
                       req == NULL ? "req" : "out");
  }
  kbc_response_init(out);
  if (req->method == NULL || req->path == NULL) {
    (void)kbc_response_error_json(out, 400, KBC_ERR_INVALID,
                                  "a request needs a method and a path");
    return KBC_OK;
  }
  /* No server clock here: uptime_s is 0 for a socketless call, because the only
   * uptime that means anything belongs to a running kbc_httpd. */
  /* No server clock here: uptime_s is 0 for a socketless call, because the only
   * uptime that means anything belongs to a running kbc_httpd. Nothing is
   * counted either — there is no daemon to count it against. */
  req_ctx ctx;
  memset(&ctx, 0, sizeof ctx);
  /* The two capture headers, stated as absent. A socketless caller has no
   * header table, so a capture through this seam has no boundary and is a
   * 400 — which is the same answer the socket gives a POST with no
   * Content-Type, not a second behaviour. */
  ctx.content_type = "";
  ctx.x_requested_by = "";
  return dispatch(app, cfg, req, &ctx, out, err, 0);
}

/* ---------------------------------------------------------- (4) the SSE --- */

/* "event: <type>\nid: <n>\ndata: <json>\n\n", with one data: line per JSON
 * line so a pretty-printed payload still parses per the SSE spec. */
static kbc_status sse_frame_build(kbc_str *out, const char *type,
                                  const char *json, uint64_t id) {
  kbc_status st = kbc_str_printf(out, "event: %s\nid: %llu\n", type,
                                 (unsigned long long)id);
  if (kbc_failed(st)) return st;
  const char *p = json != NULL ? json : "";
  for (;;) {
    const char *nl = strchr(p, '\n');
    size_t l = nl != NULL ? (size_t)(nl - p) : strlen(p);
    st = kbc_str_puts(out, "data: ");
    if (kbc_failed(st)) return st;
    st = kbc_str_append(out, p, l);
    if (kbc_failed(st)) return st;
    st = kbc_str_putc(out, '\n');
    if (kbc_failed(st)) return st;
    if (nl == NULL) break;
    p = nl + 1;
  }
  return kbc_str_puts(out, "\n");
}

/* A synthetic probe frame: `event: <name>\ndata: <json>\n\n` and NO `id:`
 * line, because the original's two synthetic frames carry none
 * (`routes/events.rs:139-153` builds them with `.event()` and `.data()` only).
 * The absence is load-bearing rather than cosmetic: an `id:` would tell a
 * reconnecting client that the probe is the newest event, so its next
 * Last-Event-ID would skip everything published since. The probe describes
 * the stream, it is not an event IN the stream, and it must not consume a
 * position in the id space.
 *
 * `data:` is split per line for the same reason the real frames are. */
static kbc_status sse_probe_build(kbc_str *out, const char *name,
                                  const char *json) {
  kbc_status st = kbc_str_printf(out, "event: %s\n", name);
  if (kbc_failed(st)) return st;
  const char *p = json != NULL ? json : "";
  for (;;) {
    const char *nl = strchr(p, '\n');
    size_t l = nl != NULL ? (size_t)(nl - p) : strlen(p);
    st = kbc_str_puts(out, "data: ");
    if (kbc_failed(st)) return st;
    st = kbc_str_append(out, p, l);
    if (kbc_failed(st)) return st;
    st = kbc_str_putc(out, '\n');
    if (kbc_failed(st)) return st;
    if (nl == NULL) break;
    p = nl + 1;
  }
  return kbc_str_puts(out, "\n");
}

/* Ring push; drops the OLDEST frame on overflow and counts it, so a stalled
 * client can never make the daemon grow without limit. Caller holds c->mu.
 *
 * A drop is reported to the client, not just counted: `c->q_dropped` is the
 * lag the original's `EventFrame::Lag { skipped }` carries
 * (`kb-core/src/events.rs:272`, emitted when the broadcast receiver finds it
 * fell behind). The count is flushed by sse_emit_lag_locked on the next push
 * rather than here, because the queue is FULL at this point — there is no
 * room for a probe frame until the next push has made some. Coalescing to one
 * frame per flush is also what the original does: `Lagged(n)` arrives with the
 * whole count, not one frame per lost event. */
static void sse_push_locked(conn *c, char *data, size_t len) {
  if (c->qcount == c->qcap) {
    sse_frame *old = &c->q[c->q_head];
    free(old->data);
    old->data = NULL;
    old->len = 0;
    c->q_head = (c->q_head + 1u) % c->qcap;
    c->qcount--;
    c->q_dropped++;
  }
  size_t slot = (c->q_head + c->qcount) % c->qcap;
  c->q[slot].data = data;
  c->q[slot].len = len;
  c->qcount++;
}

/* Emits the accumulated lag as one `event: lag` frame and zeroes the counter,
 * so a client that fell behind learns HOW FAR behind it was rather than
 * silently receiving a stream with a hole in it. Caller holds c->mu, and the
 * caller has just made room, so the push below cannot itself drop.
 *
 * NOT COVERED BY A TEST, deliberately. Provoking the precondition needs the
 * worker's socket write to block, and a client that merely stops reading does
 * not do it: sse_pump buffers to KBC_SSE_OUT_HIGH_WATER and the kernel then
 * absorbs the rest, so a measured burst of 60,000 events (~4 MiB, no reads at
 * all) still left the 64-slot queue intact often enough to be a coin flip. The
 * only lever that would make it deterministic is bounding the send buffer,
 * which a test cannot reach. See the note beside the SSE cases in
 * tests/test_httpd.c. The `gap` half of the same probe IS tested. */
static void sse_emit_lag_locked(conn *c) {
  if (c->q_dropped == 0) return;
  kbc_str body;
  kbc_str_init(&body);
  kbc_status st = kbc_str_printf(&body, "{\"skipped\":%zu}", c->q_dropped);
  kbc_str f;
  kbc_str_init(&f);
  char *copy = NULL;
  size_t len = 0;
  if (!kbc_failed(st)) st = sse_probe_build(&f, "lag", body.ptr);
  if (!kbc_failed(st)) {
    copy = malloc(f.len + 1);
    if (copy != NULL) {
      memcpy(copy, f.ptr, f.len);
      copy[f.len] = '\0';
      len = f.len;
    }
  }
  /* Zeroed only once the frame exists, so an allocation failure leaves the
   * count standing and the next flush reports the whole lag rather than
   * quietly swallowing it. */
  if (copy != NULL) {
    c->q_dropped = 0;
    sse_push_locked(c, copy, len);
  }
  kbc_str_free(&body);
  kbc_str_free(&f);
}

static void httpd_on_event(void *user, const char *type, const char *json) {
  kbc_httpd *h = (kbc_httpd *)user;
  if (h == NULL || type == NULL) return;
  /* The anchor pass's trigger. Set on the ONE event that means "the corpus was
   * re-read", and consumed by the worker tick rather than run here: a bus
   * callback is forbidden from re-entering kbc_app (app.h:56-61), and this one
   * would have to call kbc_app_get_artifact and kbc_app_publish to do its job.
   * The flag is the whole hand-off, and a flag rather than a queue because the
   * pass is idempotent — what matters is that it runs after the re-index, not
   * how many notifications got there first. */
  if (strcmp(type, "index.updated") == 0) {
    atomic_store(&h->anchors_dirty, true);
  }
  kbc_str f;
  kbc_str_init(&f);
  char *tcopy = NULL;
  char *jcopy = NULL;
  uint64_t id = 0;

  pthread_mutex_lock(&h->ring_mu);
  id = ++h->next_id;
  tcopy = strdup(type);
  jcopy = strdup(json != NULL ? json : "");
  if (tcopy != NULL && jcopy != NULL) {
    sse_hist *slot = &h->ring[h->ring_next];
    free(slot->type);
    free(slot->json);
    slot->id = id;
    slot->type = tcopy;
    slot->json = jcopy;
    tcopy = NULL;
    jcopy = NULL;
    h->ring_next = (h->ring_next + 1u) % KBC_SSE_RING_CAP;
  }
  pthread_mutex_unlock(&h->ring_mu);
  if (kbc_failed(sse_frame_build(&f, type, json, id))) goto done;

  /* Fan out. conns_mu keeps a conn from being freed under our feet; c->mu
   * keeps the queue consistent AND carries the publication of c->sse. The
   * flag is therefore READ under the lock sse_attach writes it with, so a
   * publisher can never see it set on a conn whose ring does not exist yet —
   * see sse_attach. The frame is copied because the worker frees it once
   * written. */
  pthread_mutex_lock(&h->conns_mu);
  for (size_t i = 0; i < h->all_len; i++) {
    conn *c = h->all_conns[i];
    if (c->closed) continue; /* written under conns_mu, which is held here */
    pthread_mutex_lock(&c->mu);
    if (c->sse) {
      char *copy = malloc(f.len + 1);
      if (copy != NULL) {
        memcpy(copy, f.ptr, f.len);
        copy[f.len] = '\0';
        /* The lag probe goes BEFORE the frame that caused the queue to
         * overflow, so a client sees "you missed N" ahead of the frames that
         * did survive — which is the only order in which the count and the
         * remaining stream can be read as a pair. The push above has already
         * freed the slot the probe needs, so this cannot itself drop. */
        sse_emit_lag_locked(c);
        sse_push_locked(c, copy, f.len);
        uint64_t one = 1;
        /* EAGAIN here only means the counter is already non-zero, i.e. the
         * worker has a wake-up pending anyway. */
        ssize_t ignored = write(c->event_fd, &one, sizeof one);
        (void)ignored;
      }
    }
    pthread_mutex_unlock(&c->mu);
  }
  pthread_mutex_unlock(&h->conns_mu);

done:
  free(tcopy);
  free(jcopy);
  kbc_str_free(&f);
}

/* ----------------------------------------------- comment anchor events --
 *
 * `comment.anchor_stale` and `comment.anchor_resolved`, ported from the
 * indexer's post-upsert pass (`indexer.rs:2990-3113`). The field names are the
 * contract — a client subscribes to these two by name and reads these keys —
 * so they are reproduced exactly, including the two that are easy to "tidy":
 * `fuzzy_score` on the stale event, and `score` on the resolved one.
 *
 * WHEN AN ANCHOR IS EVALUATED, and why here and not on the reindex path
 * itself. The original evaluates inside `index_one`, so the trigger is
 * "this document was re-indexed" and the work is O(that document's open
 * comments). kb-c's httpd cannot see that trigger: the only event the app
 * publishes for a re-index is `index.updated`, whose payload is `{"docs":N}`
 * and names no document, so a pass keyed on it would re-check EVERY comment
 * in the corpus after every single-file save. The two options were therefore
 *
 *   (a) scan on `POST /api/reindex` only — cheap, but a save that breaks an
 *       anchor stays silent until someone reindexes by hand, and the
 *       watcher's single-file path would never fire the event at all;
 *   (b) scan on every `index.updated`, which covers both, and pay for the
 *       over-broad trigger.
 *
 * (b) is what this does, because the extra cost buys back almost nothing:
 * the STALE set is what makes the pass quiet. A comment whose anchor is
 * already stale is re-checked and emits NOTHING (indexer.rs:3083-3086), so
 * the event volume is bounded by TRANSITIONS, not by re-indexes — the
 * "fires events on documents nobody is watching" failure cannot happen here,
 * because a document nobody changed cannot produce a transition. What the
 * over-broad trigger does cost is one comment listing per document that has
 * comments, per re-index, which is the figure store.h's comment on
 * `kbc_store_list_comment_docs` records: the enumeration is O(documents with
 * comments), not O(corpus).
 *
 * WHAT RESOLVES AN ANCHOR. kb-c's comment store holds one shape — an element
 * id, optionally prefixed `section:` (store.h:209) — and the original has four
 * serde-tagged scopes (review.rs:225-262) of which the other three cannot be
 * written here at all. So `anchor_kind` is always "section": that is the
 * original's scope for "an `<id>` or `data-kb-id` attribute"
 * (review.rs:373-375), which is exactly what both stored forms name. The id
 * is looked up in the document's current ANCHOR INDEX, which parse.c builds
 * from "a heading, a fenced block, or any block that carries an explicit id"
 * (parse.c:715-720) — the same "is this still addressable" question the
 * original asks of `[id]` and `[data-kb-id]` in the re-rendered HTML.
 *
 * THE STATE IS IN-PROCESS AND IS NOT PERSISTED, matching `anchor_state`
 * (indexer.rs:3045-3048). A daemon restart therefore re-fires `anchor_stale`
 * for every anchor that is still stale, because the tracker no longer knows it
 * was. That is the original's behaviour, not a leak here: a subscriber that
 * reconnects after a restart wants to be told the state it cannot remember
 * either.
 */

/* The element id an anchor names. `section:` is the only prefix kb-c's store
 * accepts, and it is stripped rather than searched for: `[[section:x]]` in a
 * document that declares `id="section:x"` is a DIFFERENT anchor from one that
 * declares `id="x"`, and searching for the whole string would collapse them. */
static const char *anchor_element_id(const char *anchor) {
  static const char kSection[] = "section:";
  if (strncmp(anchor, kSection, sizeof kSection - 1) == 0) {
    return anchor + sizeof kSection - 1;
  }
  return anchor;
}

static size_t anchors_find(const kbc_httpd *h, const char *doc,
                           const char *cid) {
  for (size_t i = 0; i < h->anchors_len; i++) {
    if (strcmp(h->anchors[i].doc_id, doc) == 0 &&
        strcmp(h->anchors[i].comment_id, cid) == 0) {
      return i;
    }
  }
  return SIZE_MAX;
}

/* GROWS under `anchors_mu` by doubling, with the overflow checked: the set is
 * one entry per stale comment, so its size is bounded by the number of
 * comments the corpus has, and a corpus whose comment count is large enough to
 * overflow a size_t would have exhausted the address space first. The check is
 * here anyway because "provably in range" is the rule, not "it cannot
 * happen". */
static kbc_status anchors_add(kbc_httpd *h, const char *doc, const char *cid,
                              kbc_err *err) {
  /* Both ids are minted at KBC_MAX_ID_LEN by `mint_id`; a longer one is a row
   * this fixed-width key cannot represent, and half-tracking it would be worse
   * than not tracking it — so it is refused, and the caller says so. */
  if (strlen(doc) > KBC_MAX_ID_LEN || strlen(cid) > KBC_MAX_ID_LEN) {
    return kbc_err_set(err, KBC_ERR_INVALID,
                       "anchor key (%zu, %zu bytes) exceeds the %u an id has",
                       strlen(doc), strlen(cid), KBC_MAX_ID_LEN);
  }
  if (h->anchors_len == h->anchors_cap) {
    size_t want = h->anchors_cap != 0 ? h->anchors_cap * 2u : 16u;
    if (want < h->anchors_cap || want > SIZE_MAX / sizeof(*h->anchors)) {
      return kbc_err_set(err, KBC_ERR_NOMEM, "anchor set would exceed %zu",
                         SIZE_MAX / sizeof(*h->anchors));
    }
    anchor_key *grown = realloc(h->anchors, want * sizeof(*grown));
    if (grown == NULL) {
      return kbc_err_set(err, KBC_ERR_NOMEM, "anchor set: %zu entries", want);
    }
    h->anchors = grown;
    h->anchors_cap = want;
  }
  /* memcpy at the MEASURED length, not a formatted copy into a fixed buffer:
   * both bounds were just checked against KBC_MAX_ID_LEN, which is the size of
   * each field minus its terminator. */
  size_t dl = strlen(doc);
  size_t cl = strlen(cid);
  memcpy(h->anchors[h->anchors_len].doc_id, doc, dl);
  h->anchors[h->anchors_len].doc_id[dl] = '\0';
  memcpy(h->anchors[h->anchors_len].comment_id, cid, cl);
  h->anchors[h->anchors_len].comment_id[cl] = '\0';
  h->anchors_len++;
  return KBC_OK;
}

static void anchors_del(kbc_httpd *h, size_t at) {
  if (at == SIZE_MAX || at >= h->anchors_len) return;
  h->anchors[at] = h->anchors[h->anchors_len - 1u];
  h->anchors_len--;
}

/* The `comment.anchor_stale` payload (indexer.rs:3067-3081). `fuzzy_score` is
 * the resolver's best-tried similarity and `Resolution::Stale` is a unit
 * variant that does not report one, so the original sends a literal 0.0 — and
 * sends it as a FLOAT, which is why this is `0.0` and not `0`. kb-c has no
 * fuzzy tier at all, so 0.0 is also the only value it could ever be right
 * about. */
static void publish_anchor_stale(kbc_app *app, const kbc_artifact *art,
                                 const char *cid) {
  kbc_str p;
  kbc_str_init(&p);
  kbc_status s = kbc_str_puts(&p, "{\"kb\":");
  if (s == KBC_OK)
    s = kbc_str_append_json_string(&p, art->corpus, strlen(art->corpus));
  if (s == KBC_OK) s = kbc_str_puts(&p, ",\"artifact_id\":");
  if (s == KBC_OK)
    s = kbc_str_append_json_string(&p, art->id, strlen(art->id));
  if (s == KBC_OK) s = kbc_str_puts(&p, ",\"comment_id\":");
  if (s == KBC_OK) s = kbc_str_append_json_string(&p, cid, strlen(cid));
  /* The anchor is corpus-controlled text: a document may declare any id it
   * likes, so it is escaped like everything else here. */
  if (s == KBC_OK) s = kbc_str_puts(&p, ",\"anchor_kind\":\"section\"");
  if (s == KBC_OK) s = kbc_str_puts(&p, ",\"fuzzy_score\":0.0");
  if (s == KBC_OK) s = kbc_str_puts(&p, ",\"source_relative\":");
  if (s == KBC_OK)
    s = kbc_str_append_json_string(&p, art->path, strlen(art->path));
  if (s == KBC_OK) s = kbc_str_puts(&p, "}");
  if (s == KBC_OK) {
    kbc_app_publish(app, "comment.anchor_stale", p.ptr);
  } else {
    KBC_LOGW("comment.anchor_stale for %s/%s: no payload (%s)", art->id, cid,
             kbc_status_str(s));
  }
  kbc_str_free(&p);
}

/* The `comment.anchor_resolved` payload (indexer.rs:3101-3109). `score` is
 * `Option<f32>`: the number for a FUZZY match and `null` for an exact one
 * (indexer.rs:3097-3100). kb-c resolves exactly or not at all, so it is
 * always null — and it is EMITTED as null rather than dropped, because the
 * original emits it as null and a client that found the key missing would be
 * reading a shape the field never has. */
static void publish_anchor_resolved(kbc_app *app, const kbc_artifact *art,
                                    const char *cid) {
  kbc_str p;
  kbc_str_init(&p);
  kbc_status s = kbc_str_puts(&p, "{\"kb\":");
  if (s == KBC_OK)
    s = kbc_str_append_json_string(&p, art->corpus, strlen(art->corpus));
  if (s == KBC_OK) s = kbc_str_puts(&p, ",\"artifact_id\":");
  if (s == KBC_OK)
    s = kbc_str_append_json_string(&p, art->id, strlen(art->id));
  if (s == KBC_OK) s = kbc_str_puts(&p, ",\"comment_id\":");
  if (s == KBC_OK) s = kbc_str_append_json_string(&p, cid, strlen(cid));
  if (s == KBC_OK) s = kbc_str_puts(&p, ",\"score\":null}");
  if (s == KBC_OK) {
    kbc_app_publish(app, "comment.anchor_resolved", p.ptr);
  } else {
    KBC_LOGW("comment.anchor_resolved for %s/%s: no payload (%s)", art->id, cid,
             kbc_status_str(s));
  }
  kbc_str_free(&p);
}

/* ONE document's open comments, and the only place a transition is decided.
 * The four arms are the original's table (indexer.rs:3049-3112) exactly:
 *
 *   resolves && !was_stale → nothing (a steady state, and the common one)
 *   !resolves && !was_stale → record + `comment.anchor_stale`
 *   !resolves && was_stale  → nothing (indexer.rs:3083-3086: the SPA already
 *                             painted the badge)
 *   resolves && was_stale  → forget  + `comment.anchor_resolved`
 *
 * A comment is visited ONCE per pass and takes ONE arm, so an anchor that
 * flaps cannot produce two events out of one evaluation: a document edited
 * into staleness and back within one re-index is not two observations, it is
 * one, and the one observation is the state the re-indexed bytes leave behind.
 *
 * The return value is the number of EVENTS this document fired, which is what
 * the pass logs. A key whose comment was DELETED is pruned here — the
 * original's R5 (indexer.rs:3114-3123) — by matching the document's keys
 * against the ids it actually still has. A comment that is merely RESOLVED
 * keeps its key, exactly as it does there: a resolved comment is still in the
 * file, and dropping its key would make reopening it re-fire a stale event the
 * subscriber has already seen. */
static size_t anchors_one_doc(kbc_httpd *h, kbc_store *store, kbc_arena *a,
                              const kbc_artifact *art) {
  kbc_err local;
  kbc_comment *cs = NULL;
  size_t n = 0;
  kbc_err_reset(&local);
  if (kbc_failed(kbc_store_list_comments(store, a, art->id, KBC_MAX_HITS, &cs,
                                         &n, &local))) {
    KBC_LOGW("anchors: comments of %s: %s", art->id, local.msg);
    return 0;
  }
  const char *src = art->source != NULL ? art->source : "";
  kbc_parsed *p = NULL;
  size_t transitions = 0;
  for (size_t i = 0; i < n; i++) {
    if (cs[i].resolved) continue; /* the original walks `c.is_open()` only */
    if (p == NULL) {
      kbc_err_reset(&local);
      p = kbc_parse(a, src, strlen(src), art->path, &local);
      if (p == NULL) {
        /* One parse serves every open comment on this document. A document
         * whose bytes will not parse cannot resolve any anchor, and saying so
         * as "everything is stale" would flag every comment on it. */
        KBC_LOGW("anchors: %s/%s will not parse, no anchor judged: %s", art->id,
                 art->path, local.msg);
        return transitions;
      }
    }
    const char *id = anchor_element_id(cs[i].anchor);
    bool resolves = kbc_parsed_has_anchor(p, id);
    size_t at = anchors_find(h, art->id, cs[i].id);
    if (!resolves && at == SIZE_MAX) {
      kbc_err ke;
      kbc_err_reset(&ke);
      if (kbc_failed(anchors_add(h, art->id, cs[i].id, &ke))) {
        KBC_LOGW("anchors: %s/%s not tracked: %s", art->id, cs[i].id, ke.msg);
        continue;
      }
      publish_anchor_stale(h->app, art, cs[i].id);
      transitions++;
    } else if (resolves && at != SIZE_MAX) {
      anchors_del(h, at);
      publish_anchor_resolved(h->app, art, cs[i].id);
      transitions++;
    }
  }
  /* R5: a key whose comment no longer exists is dropped, so a deleted comment
   * cannot leave a phantom the next re-index re-publishes. This runs after the
   * loop, so a key added above is matched against the ids this document really
   * has and cannot be pruned by its own addition. */
  size_t i = 0;
  while (i < h->anchors_len) {
    if (strcmp(h->anchors[i].doc_id, art->id) == 0) {
      bool still_there = false;
      for (size_t j = 0; j < n; j++) {
        if (strcmp(h->anchors[i].comment_id, cs[j].id) == 0) {
          still_there = true;
          break;
        }
      }
      if (!still_there) {
        anchors_del(h, i);
        continue;
      }
    }
    i++;
  }
  return transitions;
}

/* The pass. `anchors_mu` is HELD; the caller is either the reindex route or
 * the worker tick that won the trylock. */
static void anchors_scan_locked(kbc_httpd *h) {
  /* Cleared FIRST, not last: a publish that lands while this runs re-sets the
   * flag, and clearing at the end would swallow the one re-index this scan
   * could not see. */
  atomic_store(&h->anchors_dirty, false);
  kbc_store *store = kbc_app_store(h->app);
  if (store == NULL) return;
  kbc_arena *a = kbc_arena_new(65536u);
  if (a == NULL) {
    KBC_LOGW("anchors: no arena, the pass is skipped this re-index");
    return;
  }
  kbc_strlist docs;
  kbc_strlist_init(&docs);
  kbc_err local;
  kbc_err_reset(&local);
  kbc_status st = kbc_store_list_comment_docs(store, &docs, &local);
  if (kbc_failed(st)) {
    KBC_LOGW("anchors: documents with comments: %s", local.msg);
    kbc_strlist_free(&docs);
    kbc_arena_free(a);
    return;
  }
  size_t fired = 0;
  for (size_t i = 0; i < docs.len; i++) {
    kbc_artifact art;
    memset(&art, 0, sizeof art);
    kbc_err_reset(&local);
    if (kbc_failed(kbc_app_get_artifact(h->app, a, docs.items[i], true, &art,
                                         &local))) {
      /* The document is gone. Its comment rows went with it (the cascade
       * deletes comments by foreign key, INVENTORY.md:182), so there is
       * nothing to re-resolve and nothing to report: a comment on a deleted
       * document is not a stale anchor, it is a deleted thread. */
      size_t at = 0;
      while (at < h->anchors_len) {
        if (strcmp(h->anchors[at].doc_id, docs.items[i]) == 0) {
          anchors_del(h, at);
          continue;
        }
        at++;
      }
      continue;
    }
    fired += anchors_one_doc(h, store, a, &art);
  }
  if (fired > 0) {
    KBC_LOGI("anchors: %zu stale/resolved transitions over %zu commented "
             "documents",
             fired, docs.len);
  }
  kbc_strlist_free(&docs);
  kbc_arena_free(a);
}

/* The route's half: `POST /api/reindex` runs the pass SYNCHRONOUSLY, so the
 * events are in the ring before the 202 is written and a client that reads
 * them needs no sleep. The tick below covers the watcher, which has no route. */
static void anchors_run(kbc_httpd *h) {
  if (h == NULL || h->app == NULL) return;
  pthread_mutex_lock(&h->anchors_mu);
  anchors_scan_locked(h);
  pthread_mutex_unlock(&h->anchors_mu);
}

/* The tick, from the worker's 200 ms epoll loop. `trylock` is what makes this
 * safe with N workers: the first one to get the lock does the scan and the
 * rest skip, so a corpus-wide pass runs once per re-index rather than once per
 * core. A skip is not a lost pass — the winner is scanning the same state. */
static void anchors_tick(kbc_httpd *h) {
  if (h == NULL) return;
  if (!atomic_load_explicit(&h->anchors_dirty, memory_order_relaxed)) return;
  if (pthread_mutex_trylock(&h->anchors_mu) != 0) return;
  anchors_scan_locked(h);
  pthread_mutex_unlock(&h->anchors_mu);
}

/* The GAP half of the probe, and the decision of WHEN it fires.
 *
 * Ported from `events_stream` (`kb-core/src/events.rs:214-277`), which is
 * where the original defines it — the four words "SSE gap probe" in the port
 * plan name a real thing, not an invention. Two distinct conditions, both
 * checked before anything is replayed:
 *
 *   1. the cursor PREDATES the ring: `last_id < oldest - 1`, so at least one
 *      event the client has not seen has already been evicted;
 *   2. the cursor is AHEAD of anything this daemon process ever assigned:
 *      `last_id > newest`. The ids restart at 1 on a restart, so a client
 *      reconnecting across one holds a cursor from a previous process. The
 *      original added this for exactly that (`events.rs:224-228`: without it a
 *      post-restart reconnect got a silent empty stream).
 *
 * `last_id == 0` is a COLD client, not a gapped one: no cursor means no
 * position to have fallen behind from, and the original deliberately does not
 * probe it. When the probe fires, the original replays NOTHING — `snapshot` is
 * forced empty (`events.rs:253-254`) — because replaying a ring the client can
 * no longer line up against is pure waste, which was a measured 100% CPU
 * reconnect bug. This function therefore returns true and the caller skips
 * sse_replay entirely; that is the behaviour, not an omission.
 *
 * Returns true when the client must full-resync. */
static bool sse_gap_detect(kbc_httpd *h, uint64_t last_id, uint64_t *oldest_out) {
  pthread_mutex_lock(&h->ring_mu);
  /* The oldest id still in the ring, or 0 when the ring is empty — the same
   * 0-means-empty convention as `EventBus::oldest_id`
   * (`kb-core/src/events.rs:174-181`). ring_next is the next slot to write, so
   * in a full ring it indexes the OLDEST entry and in a partial one it indexes
   * an empty slot, which is the case that must read as 0 rather than as a
   * stale id. */
  uint64_t oldest = 0;
  if (h->ring_next < KBC_SSE_RING_CAP) {
    const sse_hist *front = &h->ring[h->ring_next];
    if (front->type != NULL) oldest = front->id;
  }
  uint64_t newest = h->next_id;
  pthread_mutex_unlock(&h->ring_mu);

  *oldest_out = oldest;
  if (last_id == 0) return false;
  bool evicted = oldest > 0 && last_id + 1 < oldest;
  bool ahead = last_id > newest;
  return evicted || ahead;
}

/* Replays everything newer than Last-Event-ID out of the in-process ring. */
static void sse_replay(kbc_httpd *h, conn *c, uint64_t last_id) {
  pthread_mutex_lock(&h->ring_mu);
  for (size_t k = 0; k < KBC_SSE_RING_CAP; k++) {
    size_t i = (h->ring_next + k) % KBC_SSE_RING_CAP;
    sse_hist *e = &h->ring[i];
    if (e->type == NULL || e->id <= last_id) continue;
    kbc_str f;
    kbc_str_init(&f);
    if (kbc_failed(sse_frame_build(&f, e->type, e->json, e->id))) {
      kbc_str_free(&f);
      continue;
    }
    char *copy = malloc(f.len + 1);
    if (copy != NULL) {
      memcpy(copy, f.ptr, f.len);
      copy[f.len] = '\0';
      pthread_mutex_lock(&c->mu);
      sse_push_locked(c, copy, f.len);
      pthread_mutex_unlock(&c->mu);
    }
    kbc_str_free(&f);
  }
  pthread_mutex_unlock(&h->ring_mu);
}

/* ------------------------------------------------- (5) the serving surface --
 *
 * Four surfaces hang off the daemon beside the JSON API: the artifact bytes
 * on the PARENT origin, the artifact SUBDOMAIN (one origin per artifact, so a
 * hostile artifact cannot reach the trusted app origin), the parent-origin
 * static handler the daemon falls back to for every path no route claimed,
 * and the Prometheus text endpoint.
 *
 * The origin split is the Rust `routes::dispatch::fallback` branch
 * (`dispatch.rs:46-50`): a `Host:` that parses as an artifact subdomain goes
 * to the artifact serve, anything else to the static handler. Both sit
 * OUTSIDE the token gate there, and they do here for the same reason — `Host:`
 * is chosen by the client, so gating on it would be theatre; what protects the
 * corpus is the bind guard (a routable bind with no token is refused outright)
 * and the token on /api. What the artifact surfaces carry instead is origin
 * isolation: a sandbox CSP on the parent origin, a frame-ancestors CSP on the
 * subdomain. */

/* Defined with the write path below; every origin handler answers through it. */
static void conn_queue_response(conn *c, int status, const char *ct,
                                const char *body, size_t body_len,
                                bool close_after, bool sse);

/* The trust boundary, and the one place in this file where the obvious
 * implementation is a vulnerability.
 *
 * The original compares `resolved.starts_with(&source_root)`
 * (`routes/artifact.rs:648`, the walk-up; `:687`, the unconditional re-check;
 * `routes/spa.rs:290`, the static handler). Rust's `Path::starts_with` is
 * COMPONENT-WISE: `/root/data` is a prefix of `/root/data` and of
 * `/root/data/x`, and is NOT a prefix of `/root/data-secret`. A C port that
 * wrote `strncmp(path, root, strlen(root)) == 0` would accept the sibling
 * directory `/root/data-secret/secret.txt` and serve it.
 *
 * A `../` test does not catch that, and that is the point: strncmp rejects
 * `../` too. Only the sibling case separates the two implementations, which is
 * why the traversal test in tests/test_httpd.c pins `<root>-secret` and not
 * just `..`.
 *
 * Both arguments must already be canonical (see `canon`). `..` and symlink
 * resolution are the filesystem's job; the guard's job is the component
 * comparison, and running it on a raw path would be the same bug in a
 * different hat. */
static bool path_within(const char *root, const char *path) {
  if (root == NULL || path == NULL || root[0] == '\0') return false;
  const char *r = root;
  const char *p = path;
  for (;;) {
    while (*r == '/') r++;
    while (*p == '/') p++;
    size_t rl = 0;
    size_t pl = 0;
    while (r[rl] != '\0' && r[rl] != '/') rl++;
    while (p[pl] != '\0' && p[pl] != '/') pl++;
    /* The root has run out of components: `path` IS the root or lives under
     * it. This is the only place containment succeeds. */
    if (rl == 0) return true;
    if (rl != pl || memcmp(r, p, rl) != 0) return false;
    r += rl;
    p += pl;
  }
}

/* Rust's `Path::canonicalize`: resolve every symlink and every "..", hand back
 * an absolute path, and answer NULL when nothing is there. realpath(3) is the
 * same operation with the same all-or-nothing answer, which is what lets the
 * walk-up below treat "no such file" as "look one level up" — there is no
 * partially-resolved answer to second-guess. */
static char *canon(const char *path) { return realpath(path, NULL); }

/* Parent directory, KBC_OWN. NULL when there is none, which for an absolute
 * path means the root itself — the walk-up uses NULL as "exhausted"
 * (`artifact.rs:658-662`). */
static char *path_dir(const char *p) {
  size_t n = strlen(p);
  while (n > 1 && p[n - 1] == '/') n--;
  while (n > 0 && p[n - 1] != '/') n--;
  if (n == 0) return NULL; /* no separator: relative, and we only walk abs */
  while (n > 1 && p[n - 1] == '/') n--;
  if (n == 1) {
    char *slash = malloc(2);
    if (slash == NULL) return NULL;
    slash[0] = '/';
    slash[1] = '\0';
    return slash;
  }
  char *d = malloc(n + 1);
  if (d == NULL) return NULL;
  memcpy(d, p, n);
  d[n] = '\0';
  return d;
}

/* KBC_OWN join, bounded by KBC_MAX_PATH_LEN. `rel` is request-derived, so the
 * bound is the guard: an unbounded join is an unbounded malloc on a hostile
 * input. */
static char *path_join(const char *base, const char *rel) {
  size_t bn = strlen(base);
  size_t rn = strlen(rel);
  bool need_sep = bn > 0 && base[bn - 1] != '/';
  if (rn + bn + 2 > KBC_MAX_PATH_LEN) return NULL;
  char *p = malloc(bn + rn + 2);
  if (p == NULL) return NULL;
  memcpy(p, base, bn);
  size_t o = bn;
  if (need_sep) p[o++] = '/';
  memcpy(p + o, rel, rn);
  p[o + rn] = '\0';
  return p;
}

/* Whole file into `out`. Deliberately UNBOUNDED: the original's `std::fs::read`
 * is unbounded on both serve paths (`docs.rs:1472`, `artifact.rs:693`) and
 * every 413 in the Rust corpus is on an upload or an export
 * (`routes/download.rs:194`, `routes/review.rs:158`). A cap here would be a
 * rejection the original does not have; the real bound is that an indexed
 * corpus file is already capped at KBC_MAX_ARTIFACT_BYTES. */
static kbc_status read_whole_file(const char *path, kbc_str *out,
                                  kbc_err *err) {
  int fd = open(path, O_RDONLY | O_CLOEXEC);
  if (fd < 0) {
    return kbc_err_set(err, KBC_ERR_IO, "open %s: %s", path, strerror(errno));
  }
  kbc_status st = KBC_OK;
  char buf[65536];
  for (;;) {
    ssize_t n = read(fd, buf, sizeof buf);
    if (n < 0) {
      if (errno == EINTR) continue;
      st = kbc_err_set(err, KBC_ERR_IO, "read %s: %s", path, strerror(errno));
      goto done;
    }
    if (n == 0) break;
    st = kbc_str_append(out, buf, (size_t)n);
    if (kbc_failed(st)) goto done;
  }
done:
  if (close(fd) != 0 && !kbc_failed(st)) {
    st = kbc_err_set(err, KBC_ERR_IO, "close %s: %s", path, strerror(errno));
  }
  return st;
}

/* Both handlers that label a body text/html refuse invalid UTF-8 rather than
 * serve bytes the declared type does not match (`docs.rs:1489`,
 * `artifact.rs:712`). Overlong forms, surrogates and >U+10FFFF are rejected
 * too, because Rust's `str::from_utf8` rejects them. */
static bool is_utf8(const char *p, size_t n) {
  size_t i = 0;
  while (i < n) {
    unsigned char c = (unsigned char)p[i];
    size_t need;
    uint32_t cp;
    if (c < 0x80u) {
      i++;
      continue;
    }
    if ((c & 0xe0u) == 0xc0u) {
      need = 1;
      cp = c & 0x1fu;
    } else if ((c & 0xf0u) == 0xe0u) {
      need = 2;
      cp = c & 0x0fu;
    } else if ((c & 0xf8u) == 0xf0u) {
      need = 3;
      cp = c & 0x07u;
    } else {
      return false;
    }
    if (n - i <= need) return false;
    for (size_t k = 1; k <= need; k++) {
      unsigned char cc = (unsigned char)p[i + k];
      if ((cc & 0xc0u) != 0x80u) return false;
      cp = (cp << 6) | (uint32_t)(cc & 0x3fu);
    }
    if ((need == 1 && cp < 0x80u) || (need == 2 && cp < 0x800u) ||
        (need == 3 && cp < 0x10000u)) {
      return false;
    }
    if (cp > 0x10ffffu || (cp >= 0xd800u && cp <= 0xdfffu)) return false;
    i += need + 1;
  }
  return true;
}

/* `routes::guess_content_type` (`routes/mod.rs:58-83`), fallthrough included.
 * There is deliberately NO unsupported-extension rejection: the function
 * never fails, and the only 415 in the corpus is on the upload path
 * (`routes/artifacts.rs:547`), never on serve. */
static const char *guess_content_type(const char *path) {
  static const char *const kMap[][2] = {
      {"html", "text/html; charset=utf-8"},
      {"htm", "text/html; charset=utf-8"},
      {"css", "text/css; charset=utf-8"},
      {"js", "application/javascript; charset=utf-8"},
      {"mjs", "application/javascript; charset=utf-8"},
      {"json", "application/json"},
      {"webmanifest", "application/manifest+json"},
      {"svg", "image/svg+xml"},
      {"png", "image/png"},
      {"jpg", "image/jpeg"},
      {"jpeg", "image/jpeg"},
      {"gif", "image/gif"},
      {"ico", "image/x-icon"},
      {"woff", "font/woff"},
      {"woff2", "font/woff2"},
      {"map", "application/json"},
      {"txt", "text/plain; charset=utf-8"},
  };
  const char *dot = strrchr(path, '.');
  const char *slash = strrchr(path, '/');
  if (dot == NULL || (slash != NULL && dot < slash)) {
    return "application/octet-stream";
  }
  for (size_t i = 0; i < sizeof kMap / sizeof kMap[0]; i++) {
    if (str_ieq(dot + 1, kMap[i][0])) return kMap[i][1];
  }
  return "application/octet-stream";
}

static const char *path_ext(const char *path) {
  const char *dot = strrchr(path, '.');
  const char *slash = strrchr(path, '/');
  if (dot == NULL || (slash != NULL && dot < slash)) return NULL;
  return dot + 1;
}

/* The original reads this out of the per-kb resolved extension map
 * (`ctx.ext_map.is_markdown`, SC5) because a `[indexer.indexable_extensions]`
 * mapping can name another extension. kb-c has no extension map and no config
 * key for one, so the two built-in extensions are the whole rule. */
static bool is_markdown(const char *path) {
  const char *e = path_ext(path);
  return e != NULL && (str_ieq(e, "md") || str_ieq(e, "markdown"));
}

static bool is_html_doc(const char *path) {
  const char *e = path_ext(path);
  return e != NULL && (str_ieq(e, "html") || str_ieq(e, "htm"));
}

/* The outbound pass every document leaving the daemon takes, and the ORDER
 * matters: the prompt-template strip runs FIRST because it selects on
 * `<template id="kb-prompt">`, an element the sanitiser would unwrap and
 * thereby destroy the very attribute the selector matches on. Scrubbing
 * afterwards would find nothing. Then the allowlist filter runs on what is
 * left.
 *
 * `peer`/`xff` are the connection's own view of the client. kb-c's frozen
 * config has no trusted-proxy list, so the chain is empty and
 * kbc_html_looks_non_loopback answers "remote" for anything but a loopback
 * peer — which FAILS CLOSED, and is the right default for a document that is
 * about to be handed to a browser. */
static kbc_status outbound_html(kbc_str *out, const char *src, size_t len,
                                const char *peer, const char *xff,
                                kbc_err *err) {
  kbc_str scrubbed;
  kbc_str_init(&scrubbed);
  kbc_status st = kbc_html_scrub_outbound(peer, xff, NULL, 0, src, len,
                                          &scrubbed, err);
  if (st == KBC_OK) {
    st = kbc_html_sanitize(scrubbed.ptr != NULL ? scrubbed.ptr : "",
                           scrubbed.len, out, err);
  }
  kbc_str_free(&scrubbed);
  return st;
}

/* Renders a `.md` body to a complete page, appended to `out`.
 *
 * `body` (the source) and `out` (the page) are SEPARATE buffers, and must be:
 * the renderer reads its input while building its output, so handing it one
 * buffer to read and write is a use-after-free wearing a plausible hat.
 *
 * A failure is RETURNED, never papered over by falling back to the raw source.
 * That fallback is the specific thing this must not do. A route that serves
 * raw bytes when the renderer fails passes every test whether or not the
 * renderer works, which makes the renderer untestable in production — the one
 * place its correctness has to hold. The caller decides the status; the page
 * is either rendered or refused.
 *
 * The renderer's scratch arena is NULL, not the caller's: the result is
 * copied into `out`, which the caller owns, and the caller's arena is dead
 * before the response is written (both routes free theirs before the write
 * path runs). Handing it an arena that is about to be freed would move the
 * page into memory nobody owns past the end of the request. */
static kbc_status render_markdown_page(const kbc_str *body, kbc_str *out,
                                       kbc_err *err) {
  kbc_markdown_page page;
  memset(&page, 0, sizeof page);
  /* The empty case is spelled "" EXPLICITLY, and the reason is worth writing
   * down because it looks like noise and is not:
   *
   *   - `read_whole_file` appends to a `kbc_str` and appends nothing for a
   *     zero-byte file, so `body->ptr` is NULL and `body->len` is 0. That is
   *     a correct description of "no bytes" and a NULL `const char *` is NOT
   *     how this API spells it.
   *   - `kbc_markdown_render` rejects a NULL `src` as KBC_ERR_INVALID
   *     ("markdown: src is NULL"), and the caller's status mapping sends
   *     anything that is not NOTFOUND/IO/NOMEM to a 400.
   *
   * So passing `body->ptr` straight through turns an EMPTY DOCUMENT — a
   * zero-byte `.md`, which is a real corpus state and not an exotic one —
   * into a 400 "bad request" blaming the caller for a file the caller never
   * wrote. Passing "" instead renders the empty shell the renderer documents
   * ("an empty document is a document"). Do not "simplify" the ternary away:
   * the two pointers are both correct descriptions of the same file and mean
   * different things, and only one of them is a document. */
  kbc_status st =
      kbc_markdown_render(body->ptr != NULL ? body->ptr : "", body->len, NULL,
                          &page, err);
  if (kbc_failed(st)) return st;
  st = kbc_str_append(out, page.html, strlen(page.html));
  /* The page came back KBC_OWN because the arena was NULL. `out` is never
   * aliased to it: the copy above already happened. */
  free(page.html);
  free(page.title);
  return st;
}

/* The title the RENDERED page carries, for a `.md` — which is NOT always the
 * title the store holds for the same document.
 *
 * The store's title is `kbc_parsed_title` (parse.c:1690): first h1, else a
 * `<title>` element, else the first prose block, else the filename stem. The
 * renderer implements the precedence the original applies at
 * markdown.rs:433-438: frontmatter `title:`, else the first `# ` heading,
 * else the literal "Untitled". Measured over one document each, the two agree
 * only when a document has a single h1 and no frontmatter. A document with
 * `title: X` in its frontmatter above an `# Y` heading is "Y" to the store
 * and "X" to the page it is served as; a document with neither heading nor
 * frontmatter is "notes/rfc" to the store and "Untitled" to the page.
 *
 * So a list row showing the store's title beside a page whose <title> is the
 * renderer's is showing two names for one document. The served page is the
 * renderer's, so the two JSON surfaces take the renderer's too: one document,
 * one title, wherever it is named. A non-`.md` keeps the store's title, which
 * is the only title it has.
 *
 * `src`/`len` are the document's bytes. A NULL `src`, a source that is not
 * valid UTF-8, and one past the renderer's size limit all yield `fallback`:
 * there is no title to derive, and the route that serves those bytes has
 * already refused them. The copy is ARENA, matching every other string this
 * file hands to a JSON writer. */
static const char *markdown_title_for(kbc_arena *a, const char *src,
                                      size_t len, const char *fallback) {
  if (src == NULL) return fallback;
  char *t = kbc_markdown_title(src, len);
  if (t == NULL) return fallback;
  char *copy = kbc_arena_strdup(a, t);
  free(t); /* KBC_OWN, and the arena copy is what outlives this call */
  return copy != NULL ? copy : fallback;
}

/* `kb_core::iframe::parse_artifact_id`: the LABEL between the host and the
 * configured subdomain suffix, with the four rejections that make a label safe
 * to map onto a filesystem — never empty, no leading or trailing dot, no "..",
 * and nothing outside `[A-Za-z0-9._-]`. A Host that fails any of them is the
 * parent origin, not an artifact, which is how `dispatch.rs:46-50` tells the
 * two apart. */
static bool parse_artifact_host(const char *host, const char *suffix,
                                const char **label, size_t *label_len) {
  if (host == NULL || suffix == NULL || suffix[0] == '\0') return false;
  size_t sl = strlen(suffix);
  size_t hl = strcspn(host, ":"); /* the port is not part of the label */
  if (hl <= sl) return false;
  const char *id = host;
  size_t n = hl - sl;
  if (strncmp(id + n, suffix, sl) != 0) return false;
  if (id[0] == '.' || id[n - 1] == '.') return false;
  for (size_t i = 0; i + 1 < n; i++) {
    if (id[i] == '.' && id[i + 1] == '.') return false;
  }
  for (size_t i = 0; i < n; i++) {
    unsigned char c = (unsigned char)id[i];
    bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.';
    if (!ok) return false;
  }
  *label = id;
  *label_len = n;
  return true;
}

/* A plain-text answer, which is what the two origin serves return for a
 * "not found": the original builds those with `(StatusCode, "...")` and never
 * reaches the problem+json helper (`artifact.rs:550`, `:594`, `:626`). */
static void origin_plain(conn *c, int status, const char *msg) {
  kbc_response r;
  kbc_response_init(&r);
  r.status = status;
  r.content_type = CT_TEXT;
  (void)kbc_str_puts(&r.body, msg);
  conn_queue_response(c, r.status, r.content_type, r.body.ptr, r.body.len, false,
                      false);
  kbc_response_free(&r);
}

/* The `frame-ancestors` CSP for a subdomain response, or NULL when none is
 * sent. It is omitted entirely in the DEFAULT dev configuration
 * (`artifact.rs:66-78`): an empty parent origin, or the default sentinel
 * itself, means "not configured for production", and a strict target would
 * break the dev flows the sentinel exists for. Reproduced rather than always
 * emitted. The value is header-injection-safe: the origin comes from the
 * operator's environment, and a value carrying a control byte is dropped
 * instead of written into the response. */
static const char *artifact_csp(const kbc_httpd *h) {
  const char *p = h->parent_origin;
  if (p == NULL || p[0] == '\0' || strcmp(p, KBC_PARENT_ORIGIN_DEFAULT) == 0) {
    return NULL;
  }
  for (const char *q = p; *q != '\0'; q++) {
    unsigned char c = (unsigned char)*q;
    if (c < 0x20u || c == 0x7fu) return NULL;
  }
  return p;
}

/* The subdomain's own header set, and it is NOT the parent origin's set, in a
 * different order: Content-Type and X-Kb-Artifact-Id first, then the cache
 * headers (?cm=on forces private, no-store and widens Vary to Authorization so
 * no proxy hands one user's comment payload to another), and the frame-ancestors
 * CSP LAST (`artifact.rs:828-861`). The daemon's own `Vary: Origin` is emitted
 * by the write path on every response; Vary is a set, so the two combine. */
static kbc_status artifact_origin_headers(kbc_str *hdrs, const char *id,
                                          bool cm_on, const kbc_httpd *h) {
  kbc_status st = kbc_str_printf(hdrs, "X-Kb-Artifact-Id: %s\r\n", id);
  if (kbc_failed(st)) return st;
  if (cm_on) {
    st = kbc_str_puts(hdrs, "Cache-Control: private, no-store\r\n"
                           "Vary: Authorization, Accept, Cookie\r\n");
  } else {
    st = kbc_str_puts(hdrs, "Vary: Accept, Cookie\r\n");
  }
  if (kbc_failed(st)) return st;
  const char *csp = artifact_csp(h);
  if (csp != NULL) {
    st = kbc_str_printf(hdrs, "Content-Security-Policy: frame-ancestors %s;\r\n",
                        csp);
  }
  return st;
}

/* Exactly 12 lowercase hex chars — `kb_core::ids::ArtifactId`'s shape
 * (`iframe.rs:158-163`). Length-checked against the slice, never a NUL: the
 * label is a substring of the Host header, not a C string. */
static bool id_is_hex12(const char *p, size_t n) {
  if (n != KBC_MAX_ID_LEN) return false;
  for (size_t i = 0; i < n; i++) {
    if (!((p[i] >= '0' && p[i] <= '9') || (p[i] >= 'a' && p[i] <= 'f'))) {
      return false;
    }
  }
  return true;
}

/* `[a-z0-9][a-z0-9-]*` — a non-empty, DNS-label-safe `kb_enc`
 * (`iframe.rs:122-129`). */
static bool kb_enc_shape(const char *p, size_t n) {
  if (n == 0) return false;
  if (!((p[0] >= 'a' && p[0] <= 'z') || (p[0] >= '0' && p[0] <= '9'))) {
    return false;
  }
  for (size_t i = 1; i < n; i++) {
    char c = p[i];
    if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-')) {
      return false;
    }
  }
  return true;
}

/* A corpus name with every '_' folded to '-' (`encode_kb_name`,
 * `iframe.rs:127-131`). The encoding is lossy ON PURPOSE, so this is a linear
 * scan over the configured corpora, never a direct lookup. */
static bool encode_kb_name_eq(const char *name, const char *enc, size_t n) {
  size_t i = 0;
  for (const char *p = name; *p != '\0'; p++, i++) {
    if (i >= n) return false;
    char c = *p == '_' ? '-' : *p;
    if (c != enc[i]) return false;
  }
  return i == n;
}

/* Resolve a subdomain LABEL to the artifact it names. The bare form is a
 * 12-hex id; the qualified form is `{kb_enc}--{id}` and names its kb
 * explicitly, split at the RIGHTMOST `--` so a kb_enc that itself contains
 * `--` still parses deterministically (`iframe.rs:145-156`). The original
 * needs the qualified form because two kbs sharing a source-relative path hash
 * to the same id; kb-c mints ids from (corpus, path) so they cannot collide,
 * and the split is kept anyway because it is the grammar on the wire. */
static bool resolve_host_artifact(kbc_app *app, kbc_arena *a,
                                  const kbc_httpd *h, const char *label,
                                  size_t llen, kbc_artifact *art,
                                  const kb_root **root_out) {
  const char *kb_enc = NULL;
  size_t kb_len = 0;
  const char *id = label + llen; /* points at the NUL when there is no `--` */
  size_t idlen = 0;
  for (size_t i = 0; i + 1 < llen; i++) {
    if (label[i] == '-' && label[i + 1] == '-') {
      kb_enc = label;
      kb_len = i;
      id = label + i + 2;
      idlen = llen - i - 2;
    }
  }
  const kb_root *want = NULL;
  if (kb_enc != NULL && kb_enc_shape(kb_enc, kb_len)) {
    for (size_t i = 0; i < h->nroots; i++) {
      if (encode_kb_name_eq(h->roots[i].name, kb_enc, kb_len)) {
        want = &h->roots[i];
        break;
      }
    }
  }
  if (kb_enc == NULL) {
    /* Bare: the label is the id. kb-c mints no legacy file-stem ids, so the
     * original's "or whose source tree contains <id>.html" fallback has no
     * counterpart here — a label that is not a 12-hex id resolves to nothing. */
    id = label;
    idlen = llen;
  } else if (want == NULL) {
    return false; /* qualified, but this daemon has no such kb */
  }
  if (!id_is_hex12(id, idlen)) return false;
  /* The label is a SLICE of the Host header, not a C string: the storage API
   * takes a NUL-terminated id, and handing it the header tail would look up
   * "<id>.artifacts.localhost". */
  char idbuf[KBC_MAX_ID_LEN + 1];
  memcpy(idbuf, id, idlen);
  idbuf[idlen] = '\0';
  if (kbc_failed(kbc_app_get_artifact(app, a, idbuf, false, art, NULL))) {
    return false;
  }
  for (size_t i = 0; i < h->nroots; i++) {
    if (art->corpus == NULL || strcmp(art->corpus, h->roots[i].name) != 0) {
      continue;
    }
    /* A qualified label is a claim about WHICH kb, and a claim that does not
     * match the owner resolves to nothing (404) rather than to the other
     * corpus's copy. */
    if (want != NULL && want != &h->roots[i]) return false;
    *root_out = &h->roots[i];
    return true;
  }
  return false;
}

/* Serves a resolved file with the daemon's own response machinery, on a body
 * already read into memory, and adds the handler's own header lines. The
 * Content-Type travels on `kbc_response` (the write path always emits it) and
 * everything else through `c->extra`, which is emitted verbatim just before
 * the write path's own `nosniff`. */
static void origin_file(conn *c, const char *ct, kbc_str *body,
                        const kbc_str *extra) {
  kbc_response resp;
  kbc_response_init(&resp);
  resp.content_type = ct;
  (void)kbc_str_append(&resp.body, body->ptr, body->len);
  kbc_status st = extra != NULL
                      ? kbc_str_append(&c->extra, extra->ptr, extra->len)
                      : KBC_OK;
  if (kbc_failed(st)) kbc_str_clear(&c->extra);
  conn_queue_response(c, resp.status, resp.content_type, resp.body.ptr,
                      resp.body.len, false, false);
  kbc_response_free(&resp);
}

/* The artifact subdomain (`routes/artifact.rs:539-866`).
 *
 * One Origin per artifact, so a hostile artifact that gets script execution
 * cannot reach the trusted app origin, its cookies or its storage. Everything
 * below the resolution is the containment walk, and the walk IS the security
 * boundary: it changes WHERE the resolver looks — upward, so a nearer
 * `_assets/` shadows a further one — and never WHAT is reachable. */
static void serve_artifact_origin(conn *c, const http_req *r) {
  const kbc_httpd *h = c->h;
  const char *label = NULL;
  size_t llen = 0;
  if (!parse_artifact_host(hdr_find(r, "Host"), h->host_suffix, &label, &llen)) {
    origin_plain(c, 404, "not an artifact subdomain");
    return;
  }
  kbc_arena *a = kbc_arena_new(16384);
  if (a == NULL) {
    origin_plain(c, 500, "no arena");
    return;
  }
  kbc_artifact art;
  memset(&art, 0, sizeof art);
  const kb_root *root = NULL;
  bool found = resolve_host_artifact(h->app, a, h, label, llen, &art, &root);
  char *entry = NULL;
  char art_id[KBC_MAX_ID_LEN + 1];
  art_id[0] = '\0';
  if (found && root != NULL && root->root != NULL && art.path != NULL) {
    char *abs = path_join(root->root, art.path);
    if (abs != NULL) {
      entry = canon(abs);
      free(abs);
    }
    /* The id is copied out before the arena dies: every field of `art` points
     * into it, and the header set below is written long after the lookup. */
    if (art.id != NULL && strlen(art.id) <= KBC_MAX_ID_LEN) {
      memcpy(art_id, art.id, strlen(art.id) + 1);
    }
  }
  kbc_arena_free(a);
  if (entry == NULL) {
    /* Unresolvable kb+id, and a `/` whose entrypoint no longer canonicalises
     * (`artifact.rs:594`, `:626`) — the same answer for both. */
    origin_plain(c, 404, "artifact not found");
    return;
  }
  /* The entrypoint is the row's own path, so it still has to be INSIDE the
   * root, and the check is unconditional: the `/` branch never enters the
   * walk-up loop, so nothing else would look at it (`artifact.rs:687-690`). */
  if (!path_within(root->root, entry)) {
    free(entry);
    kbc_response pr;
    kbc_response_init(&pr);
    (void)resp_error(&pr, 400, KBC_ERR_INVALID, "path traversal");
    conn_queue_response(c, pr.status, pr.content_type, pr.body.ptr, pr.body.len,
                        false, false);
    kbc_response_free(&pr);
    return;
  }

  const char *rel = r->path[0] == '/' ? r->path + 1 : r->path;
  char *resolved = NULL;
  if (rel[0] == '\0') {
    /* `/` is the artifact itself. Ownership moves to `resolved`; freeing
     * `entry` again below would hand the read path a freed pointer. */
    resolved = entry;
    entry = NULL;
  } else {
    char *base = path_dir(entry);
    if (base == NULL) {
      free(entry);
      origin_plain(c, 404, "artifact not found");
      return;
    }
    /* Walk up from the entrypoint's own directory toward the source root,
     * probing each level; first hit wins (`artifact.rs:633-664`). An
     * Ok-but-outside answer is refused AT EVERY LEVEL, not only the last: a
     * guard that ran only after the loop would still have opened every
     * intermediate one. And `probe == source_root` ends the walk, which is
     * what makes "/etc/passwd" a 400 rather than an unbounded climb to "/". */
    for (;;) {
      char *cand = path_join(base, rel);
      if (cand == NULL) break;
      char *p = canon(cand);
      free(cand);
      if (p != NULL) {
        if (path_within(root->root, p)) {
          resolved = p;
          break;
        }
        free(p);
        free(base);
        free(entry);
        kbc_response pr;
        kbc_response_init(&pr);
        (void)resp_error(&pr, 400, KBC_ERR_INVALID, "path traversal");
        conn_queue_response(c, pr.status, pr.content_type, pr.body.ptr,
                            pr.body.len, false, false);
        kbc_response_free(&pr);
        return;
      }
      if (strcmp(base, root->root) == 0) break;
      char *up = path_dir(base);
      free(base);
      if (up == NULL) break;
      base = up;
    }
    free(base);
  }
  free(entry);
  if (resolved == NULL) {
    origin_plain(c, 404, "artifact not found");
    return;
  }

  kbc_str body;
  kbc_str_init(&body);
  kbc_err err;
  kbc_err_reset(&err);
  kbc_status st = read_whole_file(resolved, &body, &err);
  /* The name is read while the path is still ours: the content type is a
   * function of the extension, and guessing it after free(3) would be a
   * use-after-free. */
  const char *ct = guess_content_type(resolved);
  bool md = is_markdown(resolved);
  free(resolved);
  if (kbc_failed(st)) {
    /* The read fails after a successful canonicalize when the file went away
     * or is unreadable, and the original answers 404, not 500
     * (`artifact.rs:692-696`). */
    char msg[256];
    snprintf(msg, sizeof msg, "read failed: %.180s", err.msg);
    kbc_str_free(&body);
    origin_plain(c, 404, msg);
    return;
  }
  bool html_branch = md || strcmp(ct, "text/html; charset=utf-8") == 0;
  if (html_branch && !is_utf8(body.ptr, body.len)) {
    /* A body the declared text type does not match is refused rather than
     * served: 500 here and 400 on the parent origin, because the two handlers
     * disagree about it in the original (`artifact.rs:715`, `docs.rs:1489`) and
     * the disagreement is preserved. */
    kbc_str_free(&body);
    origin_plain(c, 500, "non-utf8 artifact");
    return;
  }
  kbc_str hdrs;
  kbc_str_init(&hdrs);
  if (html_branch) {
    st = artifact_origin_headers(&hdrs, art_id, query_flag(r->query, "cm"), h);
  }
  if (kbc_failed(st)) {
    kbc_str_free(&hdrs);
    kbc_str_free(&body);
    origin_plain(c, 500, "no headers");
    return;
  }
  /* A `.md` is served as the RENDERED PAGE here too, not as its source. The
   * renderer passes raw HTML in the document straight through, and what
   * contains it on THIS surface is the origin isolation above — one origin per
   * artifact — rather than the parent route's `sandbox` CSP, which this
   * surface does not send. `nosniff` (emitted by the write path on every
   * response) is what stops the rendered page being sniffed into something
   * else, and the `text/html` label below is now the truth about the body
   * rather than a courtesy to the `.md` extension.
   *
   * A failure is a visible 500 and never a silent raw serve: the same
   * reasoning as the parent route, and the status matches THIS surface's
   * existing answer for a body it cannot serve as declared. */
  if (md) {
    kbc_str page;
    kbc_str_init(&page);
    st = render_markdown_page(&body, &page, &err);
    kbc_str_free(&body);
    if (kbc_failed(st)) {
      kbc_str_free(&page);
      kbc_str_free(&hdrs);
      origin_plain(c, 500, "markdown render failed");
      return;
    }
    ct = "text/html; charset=utf-8";
    body = page;
  } else if (strcmp(ct, "text/html; charset=utf-8") == 0) {
    /* THE SURFACE THAT MATTERS. ADR-009: the artifact subdomain sends
     * `frame-ancestors` and NO `sandbox`, so a script in a document served
     * here EXECUTES with the origin's own privileges. There is no header that
     * fixes that, which is why the bytes are filtered instead. A `.md` is
     * still rendered rather than filtered, for the reason given at the
     * parent-origin branch above. */
    kbc_str clean;
    kbc_str_init(&clean);
    st = outbound_html(&clean, body.ptr != NULL ? body.ptr : "", body.len,
                       c->peer, NULL, &err);
    kbc_str_free(&body);
    if (kbc_failed(st)) {
      kbc_str_free(&clean);
      kbc_str_free(&hdrs);
      origin_plain(c, 500, "html sanitize failed");
      return;
    }
    body = clean;
  }
  origin_file(c, ct, &body, html_branch ? &hdrs : NULL);
  kbc_str_free(&hdrs);
  kbc_str_free(&body);
}

/* The parent-origin static handler (`routes/spa.rs:33-70`, `:285-317`),
 * reached for every path the router did not claim on a non-artifact Host. The
 * containment guard is the SAME one the subdomain uses and for the same
 * reason: `spa.rs:290` is a `starts_with` against the dist root, so a
 * byte-prefix comparison would serve `<dist>-backup/…` exactly as happily as
 * the subdomain would serve `<root>-secret/…`. */
static void serve_static_origin(conn *c, const http_req *r) {
  const kbc_httpd *h = c->h;
  const char *trimmed = r->path[0] == '/' ? r->path + 1 : r->path;
  if (h->spa_root == NULL) {
    /* No dist: a 404 that says WHY, so the answer is actionable
     * (`spa.rs:400-411`). */
    kbc_response pr;
    kbc_response_init(&pr);
    (void)resp_error(&pr, 404, KBC_ERR_NOTFOUND,
                     "the daemon was started without a static root; set "
                     "KB_SPA_DIST to a directory holding index.html");
    conn_queue_response(c, pr.status, pr.content_type, pr.body.ptr, pr.body.len,
                        false, false);
    kbc_response_free(&pr);
    return;
  }
  /* A ".." segment never reaches the filesystem. The guard already stops an
   * escape; this stops a request that obviously MEANT to traverse from being
   * answered with the shell instead of a 404 (`spa.rs:37-41`). */
  for (const char *q = trimmed;;) {
    const char *slash = strchr(q, '/');
    size_t seg = slash != NULL ? (size_t)(slash - q) : strlen(q);
    if (seg == 2 && q[0] == '.' && q[1] == '.') {
      origin_plain(c, 404, "not found");
      return;
    }
    if (slash == NULL) break;
    q = slash + 1;
  }
  /* An asset request is the file or a 404. Vite content-hashes everything under
   * assets/, so those are cacheable forever; the PWA manifest is the unhashed
   * install descriptor and must stay revalidated (`spa.rs:293-310`). */
  char *cand = path_join(h->spa_root, trimmed);
  char *p = cand != NULL ? canon(cand) : NULL;
  free(cand);
  if (p != NULL && !path_within(h->spa_root, p)) {
    free(p);
    p = NULL;
  }
  if (p != NULL) {
    kbc_str body;
    kbc_str_init(&body);
    kbc_err err;
    kbc_err_reset(&err);
    kbc_status st = read_whole_file(p, &body, &err);
    if (kbc_failed(st)) {
      free(p);
      kbc_str_free(&body);
      origin_plain(c, 404, "not found");
      return;
    }
    kbc_str hdrs;
    kbc_str_init(&hdrs);
    const char *e = path_ext(p);
    bool manifest = e != NULL && str_ieq(e, "webmanifest");
    bool hashed = strncmp(p, h->spa_root, strlen(h->spa_root)) == 0 &&
                  strncmp(p + strlen(h->spa_root), "/assets", 7) == 0;
    st = kbc_str_printf(&hdrs, "Cache-Control: %s\r\n",
                        manifest ? "no-cache"
                                 : (hashed ? "public, max-age=31536000, immutable"
                                           : "public, max-age=3600"));
    if (!kbc_failed(st)) {
      origin_file(c, guess_content_type(p), &body, &hdrs);
    } else {
      kbc_str_clear(&c->extra);
    }
    free(p);
    kbc_str_free(&hdrs);
    kbc_str_free(&body);
    return;
  }
  /* No file. A client-routed URL (`/a/{kb}/{rel}`) still gets the shell so the
   * SPA's own router can answer it; anything else is a plain 404
   * (`spa.rs:65-70`). kb-c has no SPA router and no per-artifact OpenGraph
   * splice, so the shell is the whole of that branch. */
  if (strncmp(trimmed, "a/", 2) != 0) {
    origin_plain(c, 404, "not found");
    return;
  }
  char *shell = path_join(h->spa_root, "index.html");
  char *sh = shell != NULL ? canon(shell) : NULL;
  free(shell);
  if (sh == NULL || !path_within(h->spa_root, sh)) {
    free(sh);
    origin_plain(c, 404, "not found");
    return;
  }
  kbc_str body;
  kbc_str_init(&body);
  kbc_err err;
  kbc_err_reset(&err);
  kbc_status st = read_whole_file(sh, &body, &err);
  free(sh);
  if (kbc_failed(st)) {
    kbc_str_free(&body);
    origin_plain(c, 404, "not found");
    return;
  }
  kbc_str hdrs;
  kbc_str_init(&hdrs);
  st = kbc_str_puts(&hdrs, "Cache-Control: no-cache\r\n");
  if (!kbc_failed(st)) {
    origin_file(c, "text/html; charset=utf-8", &body, &hdrs);
  } else {
    kbc_str_clear(&c->extra);
  }
  kbc_str_free(&hdrs);
  kbc_str_free(&body);
}

/* `attachment_disposition` (`docs.rs:1574-1596`): an ASCII fallback name plus
 * the RFC 5987 percent-encoded form carrying the real name. The whole value is
 * ASCII, so it can never be rejected as a header — which is the point, since
 * the name comes off the filesystem and a corpus file may hold anything. */
static kbc_status append_attachment(kbc_str *hdrs, const char *name) {
  kbc_str ascii;
  kbc_str_init(&ascii);
  kbc_status st = KBC_OK;
  for (const char *p = name; *p != '\0'; p++) {
    unsigned char c = (unsigned char)*p;
    bool keep = c < 0x80u && c != '"' && c != '\\' && c >= 0x20u && c != 0x7fu;
    st = kbc_str_putc(&ascii, keep ? (char)c : '_');
    if (kbc_failed(st)) goto done;
  }
  if (ascii.len == 0) {
    st = kbc_str_puts(&ascii, "download");
    if (kbc_failed(st)) goto done;
  }
  st = kbc_str_printf(hdrs, "Content-Disposition: attachment; filename=\"%.*s\"; "
                           "filename*=UTF-8''",
                      (int)ascii.len, ascii.ptr);
  if (kbc_failed(st)) goto done;
  for (const char *p = name; *p != '\0'; p++) {
    unsigned char c = (unsigned char)*p;
    bool bare = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                (c >= '0' && c <= '9') || c == '-' || c == '.' || c == '_' ||
                c == '~';
    st = bare ? kbc_str_putc(hdrs, (char)c)
              : kbc_str_printf(hdrs, "%%%02X", (unsigned)c);
    if (kbc_failed(st)) goto done;
  }
  st = kbc_str_putc(hdrs, '\r');
done:
  kbc_str_free(&ascii);
  return st;
}

/* A corpus-relative path as stored by the indexer. The original does NOT
 * check it (`routes/docs.rs:1472` reads `row.path` straight into fs::read —
 * containment is established at index time), but that column is read out of a
 * database file rather than out of the request, and kb-c's rule 9 says any
 * path that reaches the filesystem is checked. A row that fails is corrupt
 * storage, not a client error, so it is a 500 and not a 400. */
static bool stored_rel_is_safe(const char *rel) {
  if (rel == NULL || rel[0] == '\0' || rel[0] == '/') return false;
  for (const char *p = rel;;) {
    const char *slash = strchr(p, '/');
    size_t seg = slash != NULL ? (size_t)(slash - p) : strlen(p);
    if (seg == 0) return false; /* "//" or a trailing '/' */
    if (seg == 2 && p[0] == '.' && p[1] == '.') return false;
    for (size_t i = 0; i < seg; i++) {
      unsigned char c = (unsigned char)p[i];
      if (c < 0x20u || c == 0x7fu || c == '\\') return false;
    }
    if (slash == NULL) break;
    p = slash + 1;
  }
  return strlen(rel) < KBC_MAX_PATH_LEN;
}

/* GET /api/kb/{kb}/artifact/{id} — the artifact's bytes on the PARENT origin
 * (`routes/docs.rs:1450-1542`).
 *
 * This route serves artifact HTML INLINE, in the trusted app origin, which is
 * why the header set below is not decoration: `Content-Security-Policy:
 * sandbox` — literally that, nothing else — forces the response into a unique
 * opaque origin with scripts, forms and same-origin access disabled, and
 * `nosniff` stops a browser from sniffing a mislabelled body into something
 * active. Without both, a hostile artifact navigated to here would execute in
 * the app origin: a stored-XSS and sandbox-bypass hole the moment a corpus can
 * hold untrusted content. The value is the whole policy; a `default-src` or a
 * `frame-ancestors` added to it would be a different policy. */
static kbc_status route_artifact_bytes(kbc_app *app, kbc_arena *a,
                                       const kbc_config *cfg, const char *kb,
                                       const char *id, const char *query,
                                       kbc_str *hdrs, kbc_str *out,
                                       kbc_err *err) {
  const kbc_corpus_cfg *c = kbc_config_corpus(cfg, kb);
  if (c == NULL) {
    return kbc_err_set(err, KBC_ERR_NOTFOUND, "no corpus named \"%s\"", kb);
  }
  kbc_artifact art;
  memset(&art, 0, sizeof art);
  kbc_status st = kbc_app_get_artifact(app, a, id, false, &art, err);
  if (kbc_failed(st)) return st;
  if (!stored_rel_is_safe(art.path)) {
    return kbc_err_set(err, KBC_ERR_IO,
                       "artifact %s has a stored path this daemon will not "
                       "join to %s",
                       id, c->path);
  }
  char *abs = path_join(c->path, art.path);
  if (abs == NULL) {
    return kbc_err_set(err, KBC_ERR_INVALID, "path for %s exceeds %u bytes", id,
                       KBC_MAX_PATH_LEN);
  }
  /* The source is read into its OWN buffer, not into `out`. A `.md` is
   * rendered, and the renderer reads its input while writing its output, so
   * the bytes it consumes cannot be the bytes it is overwriting. */
  kbc_str src;
  kbc_str_init(&src);
  st = read_whole_file(abs, &src, err);
  bool md = is_markdown(art.path);
  bool download = query_int(query, "download") == 1;
  free(abs);
  if (kbc_failed(st)) {
    kbc_str_free(&src);
    return st;
  }
  if (md && !is_utf8(src.ptr, src.len)) {
    /* A `.md` that is not valid UTF-8 cannot be rendered, and serving the raw
     * source under a text/html label is worse than refusing: 400, mirroring
     * `docs.rs:1489-1493`. Nothing is written to `out`, so the answer carries
     * no bytes of the document at all. */
    kbc_str_free(&src);
    return kbc_err_set(err, KBC_ERR_INVALID,
                       "markdown source is not valid UTF-8");
  }
  /* A `.md` is served as the RENDERED PAGE. The renderer deliberately passes
   * raw HTML in the source straight through (markdown.h: "render.unsafe"),
   * and what contains that is the `Content-Security-Policy: sandbox` set two
   * lines below — the two are read together, and neither is safe without the
   * other. A failure here is the caller's error to report, never a silent
   * fall back to the raw bytes. */
  if (md && !download) {
    st = render_markdown_page(&src, out, err);
    kbc_str_free(&src);
    if (kbc_failed(st)) return st;
  } else if (is_html_doc(art.path)) {
    /* An HTML artifact goes out FILTERED on this origin too, and not only
     * because the subdomain needs it. The `sandbox` CSP two lines below is a
     * containment measure, not a sanitiser: it is a header, so it is a
     * property of this route rather than of the bytes, and a header is exactly
     * what a document can leave without. A `.md` is deliberately NOT filtered
     * here — its raw-HTML passthrough is documented in markdown.h and the
     * sandbox is what contains it — but the allowlist would strip the
     * renderer's own doctype and stylesheet, so filtering it would break the
     * page rather than protect it. */
    /* NULL peer: this handler is reached through the socketless seam as well
     * as the socket, and a request whose origin cannot be established is
     * treated as remote — kbc_html_looks_non_loopback fails closed, which is
     * the property that makes "redact when in doubt" safe. NULL XFF because
     * kb-c never consults the header at all (P7, addr_is_loopback): a
     * spoofed X-Forwarded-For must never buy anything, and there is no
     * trusted-proxy list in the frozen config to walk it against. */
    kbc_str clean;
    kbc_str_init(&clean);
    st = outbound_html(&clean, src.ptr != NULL ? src.ptr : "", src.len, NULL,
                       NULL, err);
    kbc_str_free(&src);
    if (kbc_failed(st)) {
      kbc_str_free(&clean);
      return st;
    }
    /* The allowlist is ammonia's, and it has no `html`, `head`, `body` or
     * doctype in it (html.c:395-413), so what comes back is the document's
     * CONTENT and not a document. Served as that, it is a bare fragment: no
     * doctype puts the browser in quirks mode, where the box model, table
     * layout and vertical alignment are all a decade out of date, and no
     * `<head>` means no `<title>` either, so the tab, the bookmark and the
     * window history all say "". Neither is a parser difference — it is the
     * difference between handing a browser a document and handing it some
     * markup — and it is repaired HERE, in the serve path, because the
     * allowlist is right: it is a filter, and a filter that grew a doctype
     * would be a filter that manufactured a token it was asked to remove.
     *
     * The envelope is this daemon's, and it is the minimum a page needs: the
     * charset declaration (the bytes are UTF-8 whatever the source claimed),
     * and the document's own name as the title, escaped because it is text
     * and a title element is parsed as markup. */
    st = kbc_str_puts(out, "<!DOCTYPE html>\n<html lang=\"en\">\n<head>\n"
                           "<meta charset=\"utf-8\">\n<title>");
    if (!kbc_failed(st) && art.title != NULL) {
      for (const char *t = art.title; *t != '\0' && !kbc_failed(st); t++) {
        switch (*t) {
          case '&': st = kbc_str_puts(out, "&amp;"); break;
          case '<': st = kbc_str_puts(out, "&lt;"); break;
          case '>': st = kbc_str_puts(out, "&gt;"); break;
          default: st = kbc_str_putc(out, *t); break;
        }
      }
    }
    if (!kbc_failed(st)) {
      st = kbc_str_puts(out, "</title>\n</head>\n<body>\n");
    }
    if (!kbc_failed(st)) st = kbc_str_append(out, clean.ptr, clean.len);
    if (!kbc_failed(st)) st = kbc_str_puts(out, "\n</body>\n</html>\n");
    kbc_str_free(&clean);
    if (kbc_failed(st)) return st;
  } else {
    /* `?download=1` is a download: the SOURCE, not the page. A caller asking
     * for the file wants the file. */
    st = kbc_str_append(out, src.ptr, src.len);
    kbc_str_free(&src);
    if (kbc_failed(st)) return st;
  }
  /* Header order is the original's: Content-Type (carried on the response),
   * nosniff (emitted by the write path on every response), then the sandbox
   * CSP, then the artifact id (`docs.rs:1512-1542`). */
  st = kbc_str_printf(hdrs, "Content-Security-Policy: sandbox\r\n"
                           "X-Kb-Artifact-Id: %s\r\n",
                      art.id);
  if (kbc_failed(st)) return st;
  if (!download) return KBC_OK;
  /* A markdown download keeps the `.html` name rewrite (`docs.rs:1545-1553`).
   * The original's stated reason was that the body is HTML; here the body is
   * the source, so the name is now the only thing `.html` about it. The
   * rewrite is kept because it is the original's wire contract and a client
   * matching on the filename is downstream of it — but it is a rename, not a
   * claim that the attachment is rendered. */
  const char *base = strrchr(art.path, '/');
  base = base != NULL ? base + 1 : art.path;
  char name[256];
  if (md) {
    const char *dot = strrchr(base, '.');
    if (dot != NULL && (size_t)(dot - base) < sizeof name - 6) {
      snprintf(name, sizeof name, "%.*s.html", (int)(dot - base), base);
    } else {
      snprintf(name, sizeof name, "%.200s", base);
    }
  } else {
    snprintf(name, sizeof name, "%.200s", base);
  }
  if (name[0] == '\0') snprintf(name, sizeof name, "%s.html", art.id);
  return append_attachment(hdrs, name);
}

/* ---------------------------------------------------------- /metrics ----- */

static const char *const kRouteLabels[KBC_ROUTE_KINDS] = {
    "search", "atlas",  "history", "review", "events",
    "read",   "ops",    "sessions", "other"};
static const uint32_t kLatMs[KBC_LAT_BOUNDARIES] = {
    1, 5, 10, 25, 50, 100, 250, 500, 1000, 2500, 5000, 10000};
static const char *const kQuantiles[3] = {"0.5", "0.95", "0.99"};
/* `SearchStage::ALL` (`state.rs:455-459`). */
static const char *const kStageLabels[4] = {"embed", "bm25", "vector", "hybrid"};
/* `StorageKind::ALL` (`kb-core/src/metrics.rs:154-163`). */
static const char *const kStorageKinds[8] = {"query", "upsert", "delete", "read",
                                             "history", "enrich", "admin",
                                             "other"};

/* `percentile_ms` (`kb-core/src/metrics.rs:105-118`): the UPPER boundary of
 * the bucket the p-th observation falls in, 0 on an empty histogram, and
 * last-boundary + 1 as the ">10s" sentinel for the overflow slot. The quantile
 * is exact rational arithmetic, not a double: `ceil` is not available without
 * libm and 0.95 is not representable anyway. */
static uint64_t lat_percentile(const uint64_t *b, uint64_t num, uint64_t den) {
  uint64_t total = 0;
  for (size_t i = 0; i < KBC_LAT_SLOTS; i++) total += b[i];
  if (total == 0) return 0;
  uint64_t target = total > UINT64_MAX / den
                        ? total
                        : (total * num + den - 1) / den; /* ceil */
  if (target == 0) target = 1;
  uint64_t cum = 0;
  for (size_t i = 0; i < KBC_LAT_SLOTS; i++) {
    cum += b[i];
    if (cum >= target) {
      return i < KBC_LAT_BOUNDARIES ? (uint64_t)kLatMs[i]
                                    : (uint64_t)kLatMs[KBC_LAT_BOUNDARIES - 1] + 1;
    }
  }
  return (uint64_t)kLatMs[KBC_LAT_BOUNDARIES - 1] + 1;
}

static void lat_snapshot(const route_hist *h, uint64_t *out) {
  for (size_t i = 0; i < KBC_LAT_SLOTS; i++) {
    out[i] = atomic_load_explicit(&h->slot[i], memory_order_relaxed);
  }
}

/* THREE substitutions and nothing else (`routes/metrics.rs:583-601`): a
 * backslash, a newline, a double quote. Carriage return is deliberately NOT
 * escaped, because the original does not escape it either — and the corpus
 * names that reach here are validated at config load, so no CR can be in one.
 * Escaping a byte the original leaves raw would make kb-c's exposition differ
 * from kb's for the same input. */
static kbc_status prom_escape(kbc_str *o, const char *v) {
  for (const char *p = v; *p != '\0'; p++) {
    kbc_status st;
    switch (*p) {
    case '\\': st = kbc_str_puts(o, "\\\\"); break;
    case '\n': st = kbc_str_puts(o, "\\n"); break;
    case '"': st = kbc_str_puts(o, "\\\""); break;
    default: st = kbc_str_putc(o, *p); break;
    }
    if (kbc_failed(st)) return st;
  }
  return KBC_OK;
}

/* `# HELP` then `# TYPE`, once per family, immediately before that family's
 * samples (`metrics.rs:563-575`). Single spaces, no quoting of the help text,
 * `\n` terminators. */
static kbc_status prom_family(kbc_str *o, const char *name, const char *kind,
                              const char *help) {
  return kbc_str_printf(o, "# HELP %s %s\n# TYPE %s %s\n", name, help, name,
                        kind);
}

/* `sample` (`metrics.rs:578-601`): name, then the label set only if it is
 * non-empty, then one space, then the UNSIGNED DECIMAL INTEGER, then `\n`. No
 * timestamp, no float, no `_sum`/`_count`. The label-order asymmetry the
 * original has is preserved by the caller: `kb_route_latency_ms` is
 * {route,quantile} while `kb_indexer_latency_ms` is {quantile} alone. */
static kbc_status prom_sample(kbc_str *o, const char *name, const char *k1,
                              const char *v1, const char *k2, const char *v2,
                              uint64_t value) {
  kbc_status st = kbc_str_puts(o, name);
  if (kbc_failed(st)) return st;
  if (k1 != NULL) {
    st = kbc_str_printf(o, "{%s=\"", k1);
    if (kbc_failed(st)) return st;
    st = prom_escape(o, v1);
    if (kbc_failed(st)) return st;
    if (k2 != NULL) {
      st = kbc_str_printf(o, "\",%s=\"", k2);
      if (kbc_failed(st)) return st;
      st = prom_escape(o, v2);
      if (kbc_failed(st)) return st;
    }
    st = kbc_str_puts(o, "\"}");
    if (kbc_failed(st)) return st;
  }
  return kbc_str_printf(o, " %llu\n", (unsigned long long)value);
}

static kbc_status prom_route_hist(kbc_str *o, const char *name,
                                  const char *k1, const char *v1,
                                  const route_hist *h) {
  uint64_t b[KBC_LAT_SLOTS];
  lat_snapshot(h, b);
  for (size_t i = 0; i < KBC_LAT_SLOTS; i++) {
    /* `le_ms` is an ORDINARY label, not the histogram `le` convention, and
     * the series is NOT cumulative: one bucket per observation, no `_sum`, no
     * `_count`. Rendering it as a real Prometheus histogram — cumulative
     * `_bucket{le=…}` plus `_sum`/`_count` and `# TYPE … histogram` — is the
     * obvious thing to do here and it is wrong: the exposition format requires
     * a histogram's buckets to be cumulative, and these are not, so a scraper
     * would read a CDF that over-counts. The overflow slot's label is the
     * literal `+Inf` (`metrics.rs:326-341`). */
    char le[16];
    if (i < KBC_LAT_BOUNDARIES) {
      snprintf(le, sizeof le, "%u", kLatMs[i]);
    } else {
      snprintf(le, sizeof le, "+Inf");
    }
    kbc_status st = prom_sample(o, name, k1, v1, "le_ms", le, b[i]);
    if (kbc_failed(st)) return st;
  }
  return KBC_OK;
}

static kbc_status prom_route_percentiles(kbc_str *o, const char *name,
                                         const char *k1, const char *v1,
                                         const route_hist *h) {
  static const uint64_t kNum[3] = {1, 19, 99};
  static const uint64_t kDen[3] = {2, 20, 100};
  uint64_t b[KBC_LAT_SLOTS];
  lat_snapshot(h, b);
  for (size_t i = 0; i < 3; i++) {
    kbc_status st = prom_sample(o, name, k1, v1, "quantile", kQuantiles[i],
                                lat_percentile(b, kNum[i], kDen[i]));
    if (kbc_failed(st)) return st;
  }
  return KBC_OK;
}

/* `classify_route` (`state.rs:583-621`), ported whole: most-specific first,
 * then the per-kb segment match, then the top-level metadata. A per-path
 * counter map would be the lazy version, and it is what makes the exposition
 * unbounded — the enum is the bound. */
static size_t classify_route(const char *path) {
  const char *p = path;
  while (*p == '/') p++;
  if (strncmp(p, "api/", 4) == 0) p += 4;
  if (strcmp(p, "search") == 0) return 0;
  if (strcmp(p, "events") == 0 || strcmp(p, "events.schema.json") == 0 ||
      strncmp(p, "events/", 7) == 0) {
    return 4;
  }
  if (strcmp(p, "queries") == 0 || strncmp(p, "queries/", 8) == 0) return 5;
  if (strcmp(p, "sessions") == 0 || strncmp(p, "sessions/", 9) == 0) return 7;
  if (strncmp(p, "kb/", 3) == 0) {
    static const struct {
      const char *seg;
      size_t kind;
    } kSeg[] = {
        {"atlas", 1},        {"history", 2},   {"review", 3},
        {"sessions", 7},     {"docs", 5},      {"artifact", 5},
        {"graph", 5},        {"edges", 5},     {"tags", 5},
        {"folders", 5},      {"stats", 5},     {"runs", 5},
        {"queries", 5},      {"sources", 6},   {"errors", 6},
        {"reindex", 6},      {"exclusions", 6}};
    const char *after = strchr(p + 3, '/');
    if (after == NULL) return 8;
    after++;
    const char *end = strchr(after, '/');
    size_t n = end != NULL ? (size_t)(end - after) : strlen(after);
    for (size_t i = 0; i < sizeof kSeg / sizeof kSeg[0]; i++) {
      if (strlen(kSeg[i].seg) == n && memcmp(after, kSeg[i].seg, n) == 0) {
        return kSeg[i].kind;
      }
    }
    return 8;
  }
  if (strcmp(p, "identity") == 0 || strcmp(p, "kbs") == 0 ||
      strcmp(p, "stats") == 0 || strcmp(p, "settings") == 0 ||
      strcmp(p, "metrics") == 0) {
    return 5;
  }
  return 8;
}

/* The kb a `/kb/{kb}/…` path is scoped to, or NULL (`kb_from_path`,
 * `state.rs:637-649`). The same nest-stripped-vs-full normalisation as
 * classify_route, so both agree on which request belongs to which kb. */
static const char *kb_from_path(const char *path, size_t *len) {
  const char *p = path;
  while (*p == '/') p++;
  if (strncmp(p, "api/", 4) == 0) p += 4;
  if (strncmp(p, "kb/", 3) != 0) return NULL;
  const char *rest = p + 3;
  const char *slash = strchr(rest, '/');
  size_t n = slash != NULL ? (size_t)(slash - rest) : strlen(rest);
  if (n == 0) return NULL;
  *len = n;
  return rest;
}

/* One observation. The histogram is CUMULATIVE in memory (that is what makes
 * `percentile_ms` a single pass) even though the rendered series is not. */
static void metrics_observe(metrics_reg *m, const char *path, uint64_t ms) {
  atomic_fetch_add_explicit(&m->total, 1, memory_order_relaxed);
  size_t idx = 0;
  while (idx + 1 < KBC_LAT_BOUNDARIES && ms > (uint64_t)kLatMs[idx]) idx++;
  route_hist *hists[2] = {&m->route[classify_route(path)], NULL};
  size_t nlen = 0;
  const char *kb = kb_from_path(path, &nlen);
  for (size_t i = 0; i < m->n_kb; i++) {
    if (strlen(m->kb_names[i]) == nlen &&
        memcmp(m->kb_names[i], kb, nlen) == 0) {
      hists[1] = &m->per_kb[i];
      break;
    }
  }
  for (size_t i = 0; i < 2; i++) {
    if (hists[i] == NULL) continue;
    atomic_fetch_add_explicit(&hists[i]->slot[idx], 1, memory_order_relaxed);
    atomic_fetch_add_explicit(&hists[i]->count, 1, memory_order_relaxed);
  }
}

/* GET /metrics — Prometheus text exposition 0.0.4 (`routes/metrics.rs:178-186`).
 *
 * Always 200, always `text/plain; version=0.0.4; charset=utf-8`, always
 * `Cache-Control: no-store`. Mounted on its own router so the /api tree's auth
 * does not re-wrap it, and it sits OUTSIDE that tree, so it is neither counted
 * nor rate-limited as an API request — the same placement the original has. */
static kbc_status route_prometheus(kbc_app *app, const metrics_reg *m,
                                   kbc_str *hdrs, kbc_str *out,
                                   kbc_err *err) {
  kbc_app_stats st;
  memset(&st, 0, sizeof st);
  kbc_status s = kbc_app_stats_get(app, &st, err);
  if (kbc_failed(s)) return s;
  s = kbc_str_puts(hdrs, "Cache-Control: no-store\r\n");
  if (kbc_failed(s)) return s;

  /* The six scalars, then the route series, then — only when the detailed
   * layer is on — search/per-kb, then the index pipeline, then the storage
   * families. The order is hand-written and fixed (`metrics.rs:198-205`); a
   * scrape diffs by line, so it is part of the contract. */
  s = prom_family(out, "kb_http_requests_total", "counter",
                  "HTTP requests since daemon boot (GET /api/metrics "
                  "requests_total).");
  if (kbc_failed(s)) return s;
  s = prom_sample(out, "kb_http_requests_total", NULL, NULL, NULL, NULL,
                  atomic_load(&m->total));
  if (kbc_failed(s)) return s;
  s = prom_family(out, "kb_storage_channel_depth", "gauge",
                  "Busiest kb storage-actor queue depth.");
  if (kbc_failed(s)) return s;
  /* kb-c has no storage actor: a synchronous call is the whole "queue", so
   * the honest reading of both the depth and the capacity is 0. A fixed-
   * cardinality family renders at 0 when it has observed nothing — that is the
   * format's own answer, not a placeholder (`metrics.rs:266-271`). */
  s = prom_sample(out, "kb_storage_channel_depth", NULL, NULL, NULL, NULL, 0);
  if (kbc_failed(s)) return s;
  s = prom_family(out, "kb_storage_channel_capacity", "gauge",
                  "Storage-actor channel capacity.");
  if (kbc_failed(s)) return s;
  s = prom_sample(out, "kb_storage_channel_capacity", NULL, NULL, NULL, NULL, 0);
  if (kbc_failed(s)) return s;
  s = prom_family(out, "kb_embedder_degraded", "gauge",
                  "1 if any embedder subprocess is currently unrecoverable.");
  if (kbc_failed(s)) return s;
  s = prom_sample(out, "kb_embedder_degraded", NULL, NULL, NULL, NULL, 0);
  if (kbc_failed(s)) return s;
  s = prom_family(out, "kb_embedder_respawns_total", "counter",
                  "Embedder subprocess respawns since boot.");
  if (kbc_failed(s)) return s;
  s = prom_sample(out, "kb_embedder_respawns_total", NULL, NULL, NULL, NULL, 0);
  if (kbc_failed(s)) return s;
  s = prom_family(out, "kb_metrics_detailed", "gauge",
                  "1 if the detailed metrics layer is enabled.");
  if (kbc_failed(s)) return s;
  s = prom_sample(out, "kb_metrics_detailed", NULL, NULL, NULL, NULL,
                  m->detailed ? 1u : 0u);
  if (kbc_failed(s)) return s;

  s = prom_family(out, "kb_route_requests_total", "counter",
                  "Requests since boot by route family.");
  if (kbc_failed(s)) return s;
  for (size_t i = 0; i < KBC_ROUTE_KINDS; i++) {
    s = prom_sample(out, "kb_route_requests_total", "route", kRouteLabels[i],
                    NULL, NULL,
                    atomic_load(&m->route[i].count));
    if (kbc_failed(s)) return s;
  }
  s = prom_family(out, "kb_route_latency_ms", "gauge",
                  "Bucket-approximated route latency percentile, "
                  "milliseconds.");
  if (kbc_failed(s)) return s;
  for (size_t i = 0; i < KBC_ROUTE_KINDS; i++) {
    s = prom_route_percentiles(out, "kb_route_latency_ms", "route",
                               kRouteLabels[i], &m->route[i]);
    if (kbc_failed(s)) return s;
  }
  s = prom_family(out, "kb_route_latency_bucket", "counter",
                  "Non-cumulative latency-bucket observations by route (le_ms "
                  "is the upper bound; +Inf is overflow).");
  if (kbc_failed(s)) return s;
  for (size_t i = 0; i < KBC_ROUTE_KINDS; i++) {
    s = prom_route_hist(out, "kb_route_latency_bucket", "route",
                        kRouteLabels[i], &m->route[i]);
    if (kbc_failed(s)) return s;
  }

  /* The ENTIRE detailed block is absent when the layer is off — not zero, not
   * commented (`metrics.rs:202-204`, `:661`). */
  if (!m->detailed) return KBC_OK;

  s = prom_family(out, "kb_search_stage_requests_total", "counter",
                  "Search-stage observations since boot.");
  if (kbc_failed(s)) return s;
  for (size_t i = 0; i < 4; i++) {
    /* kb-c's search runs its stages inline with no per-stage timing surface,
     * so these are 0 for the same reason the storage families are. */
    s = prom_sample(out, "kb_search_stage_requests_total", "stage",
                    kStageLabels[i], NULL, NULL, 0);
    if (kbc_failed(s)) return s;
  }
  s = prom_family(out, "kb_search_stage_latency_ms", "gauge",
                  "Bucket-approximated search-stage latency percentile, "
                  "milliseconds.");
  if (kbc_failed(s)) return s;
  for (size_t i = 0; i < 4; i++) {
    s = prom_sample(out, "kb_search_stage_latency_ms", "stage", kStageLabels[i],
                    "quantile", kQuantiles[0], 0);
    if (kbc_failed(s)) return s;
    s = prom_sample(out, "kb_search_stage_latency_ms", "stage", kStageLabels[i],
                    "quantile", kQuantiles[1], 0);
    if (kbc_failed(s)) return s;
    s = prom_sample(out, "kb_search_stage_latency_ms", "stage", kStageLabels[i],
                    "quantile", kQuantiles[2], 0);
    if (kbc_failed(s)) return s;
  }
  /* per_kb is the one SORTED set: the original's map iterates in random order
   * and sorts by name for a stable response (`metrics.rs:131-136`). Sorted here
   * rather than at bring-up, so the observation index space never moves under a
   * worker mid-scrape. The entries are pre-seeded from the fixed kb set, so a
   * configured kb renders at 0 before its first request instead of being
   * absent. */
  size_t order[KBC_MAX_CORPORA];
  for (size_t i = 0; i < m->n_kb; i++) order[i] = i;
  for (size_t i = 1; i < m->n_kb; i++) {
    size_t v = order[i];
    size_t j = i;
    while (j > 0 && strcmp(m->kb_names[order[j - 1]], m->kb_names[v]) > 0) {
      order[j] = order[j - 1];
      j--;
    }
    order[j] = v;
  }
  s = prom_family(out, "kb_per_kb_requests_total", "counter",
                  "Requests since boot attributed to a kb.");
  if (kbc_failed(s)) return s;
  for (size_t i = 0; i < m->n_kb; i++) {
    s = prom_sample(out, "kb_per_kb_requests_total", "kb",
                    m->kb_names[order[i]], NULL, NULL,
                    atomic_load(&m->per_kb[order[i]].count));
    if (kbc_failed(s)) return s;
  }
  s = prom_family(out, "kb_per_kb_latency_ms", "gauge",
                  "Bucket-approximated per-kb request latency percentile, "
                  "milliseconds.");
  if (kbc_failed(s)) return s;
  for (size_t i = 0; i < m->n_kb; i++) {
    s = prom_route_percentiles(out, "kb_per_kb_latency_ms", "kb",
                               m->kb_names[order[i]], &m->per_kb[order[i]]);
    if (kbc_failed(s)) return s;
  }

  s = prom_family(out, "kb_pipeline_enabled", "gauge",
                  "1 if ingest-pipeline timing is being recorded.");
  if (kbc_failed(s)) return s;
  s = prom_sample(out, "kb_pipeline_enabled", NULL, NULL, NULL, NULL, 0);
  if (kbc_failed(s)) return s;
  s = prom_family(out, "kb_indexer_files_total", "counter",
                  "Files observed by the indexer.");
  if (kbc_failed(s)) return s;
  s = prom_sample(out, "kb_indexer_files_total", NULL, NULL, NULL, NULL,
                  st.artifacts_indexed > 0 ? (uint64_t)st.artifacts_indexed : 0u);
  if (kbc_failed(s)) return s;
  s = prom_family(out, "kb_indexer_latency_ms", "gauge",
                  "Bucket-approximated indexer file latency percentile, "
                  "milliseconds.");
  if (kbc_failed(s)) return s;
  for (size_t i = 0; i < 3; i++) {
    /* {quantile} ALONE — no `route` label. The original's label sets differ
     * between families and a port that "tidied" them would break the golden
     * line `kb_route_latency_ms{route="search",quantile="0.5"} 5`
     * (`metrics.rs:657-659`). */
    s = prom_sample(out, "kb_indexer_latency_ms", "quantile", kQuantiles[i],
                    NULL, NULL, 0);
    if (kbc_failed(s)) return s;
  }
  s = prom_family(out, "kb_index_embed_calls_total", "counter",
                  "Index-side embed calls.");
  if (kbc_failed(s)) return s;
  s = prom_sample(out, "kb_index_embed_calls_total", NULL, NULL, NULL, NULL, 0);
  if (kbc_failed(s)) return s;
  s = prom_family(out, "kb_index_embed_docs_total", "counter",
                  "Documents passed to the index-side embedder.");
  if (kbc_failed(s)) return s;
  s = prom_sample(out, "kb_index_embed_docs_total", NULL, NULL, NULL, NULL, 0);
  if (kbc_failed(s)) return s;
  s = prom_family(out, "kb_index_embed_latency_ms", "gauge",
                  "Bucket-approximated index-side embed latency percentile, "
                  "milliseconds.");
  if (kbc_failed(s)) return s;
  for (size_t i = 0; i < 3; i++) {
    s = prom_sample(out, "kb_index_embed_latency_ms", "quantile", kQuantiles[i],
                    NULL, NULL, 0);
    if (kbc_failed(s)) return s;
  }
  s = prom_family(out, "kb_storage_ops_total", "counter",
                  "Storage-actor operations by kind.");
  if (kbc_failed(s)) return s;
  for (size_t i = 0; i < 8; i++) {
    s = prom_sample(out, "kb_storage_ops_total", "kind", kStorageKinds[i], NULL,
                    NULL, 0);
    if (kbc_failed(s)) return s;
  }
  s = prom_family(out, "kb_storage_handler_latency_ms", "gauge",
                  "Bucket-approximated storage-handler latency percentile, "
                  "milliseconds.");
  if (kbc_failed(s)) return s;
  for (size_t i = 0; i < 8; i++) {
    for (size_t j = 0; j < 3; j++) {
      s = prom_sample(out, "kb_storage_handler_latency_ms", "kind",
                      kStorageKinds[i], "quantile", kQuantiles[j], 0);
      if (kbc_failed(s)) return s;
    }
  }
  s = prom_family(out, "kb_storage_queue_wait_ms", "gauge",
                  "Bucket-approximated storage-actor queue-wait percentile, "
                  "milliseconds.");
  if (kbc_failed(s)) return s;
  for (size_t i = 0; i < 8; i++) {
    for (size_t j = 0; j < 3; j++) {
      s = prom_sample(out, "kb_storage_queue_wait_ms", "kind", kStorageKinds[i],
                      "quantile", kQuantiles[j], 0);
      if (kbc_failed(s)) return s;
    }
  }
  return KBC_OK;
}

/* ------------------------------------------------- connection bookkeeping -- */

static void conn_epoll(conn *c, uint32_t events) {
  if (c->registered == events) return;
  struct epoll_event ev;
  memset(&ev, 0, sizeof ev);
  ev.events = events;
  ev.data.ptr = c;
  int op = c->registered == 0 ? EPOLL_CTL_ADD : EPOLL_CTL_MOD;
  if (epoll_ctl(c->w->epfd, op, c->fd, &ev) == 0) c->registered = events;
}

static bool ptr_push(conn ***arr, size_t *len, size_t *cap, size_t hard,
                     conn *c) {
  if (*len == *cap) {
    size_t next = *cap != 0 ? *cap * 2u : 8u;
    if (next > hard) next = hard;
    if (next <= *cap) return false;
    conn **grown = realloc(*arr, next * sizeof *grown);
    if (grown == NULL) return false;
    *arr = grown;
    *cap = next;
  }
  if (*len >= *cap) return false;
  (*arr)[(*len)++] = c;
  return true;
}

static void worker_sse_del(kbc_worker *w, conn *c) {
  for (size_t i = 0; i < w->sse_len; i++) {
    if (w->sse[i] == c) {
      w->sse[i] = w->sse[w->sse_len - 1];
      w->sse_len--;
      return;
    }
  }
}

static void conn_destroy(conn *c) {
  if (c->event_fd >= 0) {
    (void)epoll_ctl(c->w->epfd, EPOLL_CTL_DEL, c->event_fd, NULL);
    close(c->event_fd);
  }
  if (c->fd >= 0) {
    (void)epoll_ctl(c->w->epfd, EPOLL_CTL_DEL, c->fd, NULL);
    close(c->fd);
  }
  worker_sse_del(c->w, c);
  if (c->q_dropped > 0) {
    KBC_LOGW("sse: dropped %zu queued events for a stalled client",
             c->q_dropped);
  }
  for (size_t i = 0; i < c->qcap; i++) free(c->q[i].data);
  free(c->q);
  kbc_str_free(&c->in);
  kbc_str_free(&c->out);
  kbc_str_free(&c->extra);
  if (c->arena != NULL) kbc_arena_free(c->arena);
  pthread_mutex_destroy(&c->mu);
  atomic_fetch_sub(&c->h->conns, 1);
  free(c);
}

/* Unlink under conns_mu so the fan-out can never touch a freed conn, then defer
 * the free to the end of the worker's event batch: a conn owns two fds, and the
 * second one's event may still be sitting in the same epoll_wait result. */
static void conn_close(kbc_httpd *h, conn *c) {
  if (c->closed) return;
  pthread_mutex_lock(&h->conns_mu);
  c->closed = true;
  for (size_t i = 0; i < h->all_len; i++) {
    if (h->all_conns[i] == c) {
      h->all_conns[i] = h->all_conns[h->all_len - 1];
      h->all_len--;
      break;
    }
  }
  pthread_mutex_unlock(&h->conns_mu);
  if (c->w != NULL) {
    /* The return is checked, not cast away. A conn is unlinked from
     * h->all_conns above, so a push that fails here has nowhere left to be
     * reached and leaks. It cannot be destroyed inline instead: the deferral
     * exists because the conn owns two fds and the second one's event may
     * still be sitting in this batch's epoll_wait result, and freeing it now
     * is the use-after-free the deferral was written to prevent. So the
     * failure is logged and the connection is lost slowly rather than
     * crashed into — the only one of the three ptr_push call sites that could
     * do otherwise, and the only one that discarded the answer.
     *
     * Reachable only on realloc failure: zomb_len counts connections closed
     * in this one batch, and every conn_push-able connection is already
     * capped at KBC_HTTP_MAX_CONNECTIONS, so the (cap + 1)th push — the only
     * one the hard limit could reject — cannot happen. */
    if (!ptr_push(&c->w->zomb, &c->w->zomb_len, &c->w->zomb_cap,
                  KBC_HTTP_MAX_CONNECTIONS, c)) {
      KBC_LOGE("conn_close: no room to defer conn %p, it leaks", (void *)c);
    }
  } else {
    conn_destroy(c);
  }
}

static void worker_drain_zombies(kbc_worker *w) {
  for (size_t i = 0; i < w->zomb_len; i++) conn_destroy(w->zomb[i]);
  w->zomb_len = 0;
}

/* all_conns is ONE array shared by every worker, and ptr_push grows it with
 * realloc. Growing it unlocked let two workers accept at the same instant,
 * both realloc the same block, and one silently discard the other's pointer:
 * the loser's buffer was freed while conns were still being written into it,
 * which is the "double free or corruption (!prev)" the load test hit at high
 * connection counts. The lock is the same conns_mu the close path and the SSE
 * fan-out already hold, so the array has exactly one writer discipline. */
static bool httpd_track(kbc_httpd *h, conn *c) {
  pthread_mutex_lock(&h->conns_mu);
  bool ok = ptr_push(&h->all_conns, &h->all_len, &h->all_cap,
                     KBC_HTTP_MAX_CONNECTIONS, c);
  pthread_mutex_unlock(&h->conns_mu);
  if (ok) atomic_fetch_add(&h->conns, 1);
  return ok;
}

/* ------------------------------------------------------------ write path -- */

static kbc_status queue_headers(conn *c, int status, const char *ct,
                                size_t body_len, bool close_after, bool sse) {
  kbc_status st = kbc_str_printf(&c->out, "HTTP/1.1 %d %s\r\n", status,
                                 status_text(status));
  if (kbc_failed(st)) return st;
  /* A 204 carries no body, so it declares no content type and no length: a
   * Content-Type on an empty response describes nothing. */
  if (status != 204) {
    st = kbc_str_printf(&c->out, "Content-Type: %s\r\n", ct);
    if (kbc_failed(st)) return st;
  }
  /* An SSE response has no Content-Length: the body never ends, and claiming a
   * length would tell the client to wait for bytes that are not coming. */
  if (!sse && status != 204) {
    st = kbc_str_printf(&c->out, "Content-Length: %zu\r\n", body_len);
    if (kbc_failed(st)) return st;
  }
  /* `Vary: Origin` goes on EVERY response, allowed origin or not: a shared
   * cache must not hand an allowed-origin answer to a browser that sent a
   * different one, and that is only true if every answer varies. */
  st = kbc_str_puts(&c->out, "Vary: Origin\r\n");
  if (kbc_failed(st)) return st;
  /* Reflected only for an origin that matched the configured allowlist. There
   * is deliberately no `Access-Control-Allow-Origin: *` fallback: a daemon
   * holding an entire corpus has no reason to make itself readable by any page
   * the operator happens to have open. */
  if (c->cors_origin != NULL) {
    st = kbc_str_printf(
        &c->out,
        "Access-Control-Allow-Origin: %s\r\n"
        "Access-Control-Allow-Methods: GET, POST, OPTIONS\r\n"
        "Access-Control-Allow-Headers: Authorization, Content-Type, "
        "Last-Event-ID\r\n"
        "Access-Control-Expose-Headers: Retry-After\r\n"
        "Access-Control-Max-Age: 600\r\n",
        c->cors_origin);
    if (kbc_failed(st)) return st;
  }
  if (c->extra_hdr != NULL) {
    st = kbc_str_printf(&c->out, "%s\r\n", c->extra_hdr);
    if (kbc_failed(st)) return st;
    c->extra_hdr = NULL; /* one-shot: the next response has its own headers */
  }
  st = kbc_str_puts(&c->out, "X-Content-Type-Options: nosniff\r\n");
  if (kbc_failed(st)) return st;
  /* The route's own header lines, in the order the handler set them. They land
   * after nosniff, which the daemon emits on every response anyway, so the
   * RELATIVE order the original establishes (nosniff, then the sandbox CSP,
   * then the artifact id) is preserved on the wire. */
  if (c->extra.len > 0) {
    st = kbc_str_append(&c->out, c->extra.ptr, c->extra.len);
    if (kbc_failed(st)) return st;
    kbc_str_clear(&c->extra);
  }
  if (status == 401) {
    st = kbc_str_puts(&c->out, "WWW-Authenticate: Bearer realm=\"kb\"\r\n");
    if (kbc_failed(st)) return st;
  }
  if (sse) {
    st = kbc_str_puts(&c->out, "Cache-Control: no-store\r\n");
    if (kbc_failed(st)) return st;
  }
  return kbc_str_puts(&c->out, close_after ? "Connection: close\r\n\r\n"
                                           : "Connection: keep-alive\r\n\r\n");
}

static void conn_queue_response(conn *c, int status, const char *ct,
                                const char *body, size_t body_len,
                                bool close_after, bool sse) {
  if (kbc_failed(queue_headers(c, status, ct, body_len, close_after, sse)) ||
      kbc_failed(kbc_str_append(&c->out, body != NULL ? body : "", body_len))) {
    c->close_after = true;
  }
}

typedef enum { FLUSH_DONE = 0, FLUSH_WOULDBLOCK, FLUSH_ERROR } flush_result;

static flush_result conn_flush(conn *c) {
  while (c->out_off < c->out.len) {
    ssize_t n = send(c->fd, c->out.ptr + c->out_off, c->out.len - c->out_off,
                     MSG_NOSIGNAL);
    if (n > 0) {
      c->out_off += (size_t)n;
      c->last_write_ns = kbc_now_ns();
      continue;
    }
    if (n < 0 && errno == EINTR) continue;
    if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
      return FLUSH_WOULDBLOCK;
    }
    return FLUSH_ERROR;
  }
  kbc_str_clear(&c->out);
  c->out_off = 0;
  return FLUSH_DONE;
}

static void conn_arm(conn *c) {
  if (c->sse) {
    conn_epoll(c, c->out_off < c->out.len ? (EPOLLIN | EPOLLOUT) : EPOLLIN);
    return;
  }
  /* While a response is in flight we stop reading: the only thing left to do
   * is finish writing it. */
  conn_epoll(c, c->out_off < c->out.len ? EPOLLOUT : EPOLLIN);
}

/* Moves queued SSE frames into the write buffer, never past the high water
 * mark, so a client that stops reading throttles its own queue instead of the
 * daemon's memory. */
static void sse_pump(conn *c) {
  for (;;) {
    if (c->out.len - c->out_off >= KBC_SSE_OUT_HIGH_WATER) return;
    pthread_mutex_lock(&c->mu);
    if (c->qcount == 0) {
      pthread_mutex_unlock(&c->mu);
      return;
    }
    sse_frame *f = &c->q[c->q_head];
    char *data = f->data;
    size_t len = f->len;
    c->q[c->q_head].data = NULL;
    c->q_head = (c->q_head + 1u) % c->qcap;
    c->qcount--;
    pthread_mutex_unlock(&c->mu);
    bool ok = !kbc_failed(kbc_str_append(&c->out, data, len));
    free(data);
    if (!ok) {
      c->close_after = true;
      return;
    }
  }
}

/* Defined with the start/stop plumbing below; used by the accept and
 * eventfd paths. */
static int set_cloexec_nonblock(int fd);

/* ------------------------------------------------------------ read path --- */
static void sse_attach(conn *c, const char *last_event_id) {
  /* c->sse is the ONLY thing that makes this connection visible to the event
   * fan-out: httpd_track appended it to h->all_conns at accept time, and
   * httpd_on_event walks that array looking for exactly this flag. So every
   * field the fan-out reaches for once it has seen the flag — c->q, c->qcap,
   * c->event_fd — must be BUILT before the flag is published, and the flag is
   * published under c->mu, the lock the fan-out reads it with. The old order
   * set c->sse first and allocated after it, so a publisher landing in that
   * window took the SSE branch with q == NULL and qcap == 0: sse_push_locked
   * then computed `% c->qcap` and dereferenced the NULL ring, and the daemon
   * died of SIGFPE on a thread that had done nothing wrong. */
  c->qcap = KBC_SSE_QUEUE_CAP;
  c->q = calloc(c->qcap, sizeof *c->q);
  if (c->q == NULL) c->qcap = 0;

  /* EFD_NONBLOCK|EFD_CLOEXEC are GNU extensions; the portable equivalent is
   * a plain eventfd plus the same two fcntls used for accepted sockets. */
  c->event_fd = eventfd(0, 0);
  if (c->event_fd >= 0 && set_cloexec_nonblock(c->event_fd) < 0) {
    close(c->event_fd);
    c->event_fd = -1;
  }
  if (c->q == NULL || c->event_fd < 0) {
    /* Nothing is published, so the fan-out skips this conn and conn_destroy
     * walks zero ring entries instead of 64 NULL frames. c->q is released
     * here so a ring that was allocated and then abandoned does not leak. */
    free(c->q);
    c->q = NULL;
    c->close_after = true;
    return;
  }
  struct epoll_event ev;
  memset(&ev, 0, sizeof ev);
  ev.events = EPOLLIN;
  ev.data.ptr = c;
  c->tag = CONN_TAG_EV;
  if (epoll_ctl(c->w->epfd, EPOLL_CTL_ADD, c->event_fd, &ev) != 0) {
    c->close_after = true;
    return;
  }
  if (!ptr_push(&c->w->sse, &c->w->sse_len, &c->w->sse_cap,
                KBC_HTTP_MAX_CONNECTIONS, c)) {
    c->close_after = true;
    return;
  }
  c->last_write_ns = kbc_now_ns();
  /* The publication point: the ring exists, the wake fd is armed and this
   * worker owns the sweep, so from here the conn is a live stream — and only
   * a publisher holding c->mu can see that it is one. */
  pthread_mutex_lock(&c->mu);
  c->sse = true;
  pthread_mutex_unlock(&c->mu);
  if (last_event_id != NULL && last_event_id[0] != '\0') {
    char *end = NULL;
    errno = 0;
    unsigned long long v = strtoull(last_event_id, &end, 10);
    if (end != last_event_id && *end == '\0' && errno == 0) {
      uint64_t oldest = 0;
      if (sse_gap_detect(c->h, (uint64_t)v, &oldest)) {
        /* The gap probe, and the replay it SUPPRESSES. The original sends the
         * frame and then goes straight to live events
         * (`kb-core/src/events.rs:253-259`): a client whose cursor the ring
         * cannot serve must resync wholesale, and replaying a ring it can no
         * longer line up against was a measured 100% CPU reconnect bug. */
        kbc_str body;
        kbc_str_init(&body);
        kbc_str f;
        kbc_str_init(&f);
        kbc_status gst = kbc_str_printf(&body,
                                        "{\"requested_id\":%llu,"
                                        "\"oldest_available_id\":%llu}",
                                        (unsigned long long)v,
                                        (unsigned long long)oldest);
        if (!kbc_failed(gst)) gst = sse_probe_build(&f, "gap", body.ptr);
        if (!kbc_failed(gst)) {
          char *copy = malloc(f.len + 1);
          if (copy != NULL) {
            memcpy(copy, f.ptr, f.len);
            copy[f.len] = '\0';
            pthread_mutex_lock(&c->mu);
            sse_push_locked(c, copy, f.len);
            pthread_mutex_unlock(&c->mu);
          }
        }
        kbc_str_free(&body);
        kbc_str_free(&f);
      } else {
        sse_replay(c->h, c, (uint64_t)v);
      }
    }
  }
}

/* Exact match against the configured allowlist. An origin is never pattern
 * matched and never wildcarded: a subdomain rule would be a rule somebody has
 * to reason about before trusting it with a whole corpus. */
static const char *cors_match(const kbc_httpd *h, const char *origin) {
  if (origin == NULL || origin[0] == '\0') return NULL;
  return kbc_strlist_contains(&h->cors, origin) ? origin : NULL;
}

/* A preflight is a browser asking "may I?" before it sends anything. It
 * carries no credentials, so it is answered without the token gate and it
 * never reaches a handler — but it is still not free to ask about an origin the
 * daemon does not serve. */
static void serve_preflight(conn *c, const http_req *r) {
  const char *origin = hdr_find(r, "Origin");
  kbc_response resp;
  kbc_response_init(&resp);
  if (origin == NULL || origin[0] == '\0') {
    c->cors_origin = NULL;
    (void)resp_error(&resp, 404, KBC_ERR_NOTFOUND,
                     "OPTIONS %s without an Origin is not a preflight", r->path);
  } else {
    const char *ok = cors_match(c->h, origin);
    c->cors_origin = ok;
    if (ok == NULL) {
      (void)resp_error(&resp, 403, KBC_ERR_INVALID,
                       "origin %s is not in KBC_CORS_ORIGINS", origin);
    } else {
      resp.status = 204;
    }
  }
  conn_queue_response(c, resp.status, resp.content_type, resp.body.ptr,
                      resp.body.len, false, false);
  kbc_response_free(&resp);
}

static void serve_request(conn *c, const http_req *r) {
  const char *origin = hdr_find(r, "Origin");
  c->cors_origin = cors_match(c->h, origin);
  c->extra_hdr = NULL;
  kbc_str_clear(&c->extra);
  if (strcmp(r->method, "OPTIONS") == 0 && strncmp(r->path, "/api/", 5) == 0) {
    serve_preflight(c, r);
    return;
  }

  /* Per-connection fixed window. What this protects is the worker: one client
   * that pipelines faster than the daemon can answer must not be able to keep
   * that worker's loop busy enough to delay every other connection it holds.
   * It is NOT a defence against an attacker — there is no untrusted-caller
   * path to defend against (loopback-only bind, no routable bind without a
   * token) — and it is deliberately per connection, not global, so one noisy
   * client cannot spend another client's budget. */
  size_t cap = c->h->rate_limit;
  if (cap > 0) {
    int64_t now = kbc_now_ns();
    if (c->rl_win_ns == 0 || now - c->rl_win_ns >= KBC_RATE_WINDOW_NS) {
      c->rl_win_ns = now;
      c->rl_count = 0;
    }
    if (c->rl_count >= cap) {
      kbc_response resp;
      kbc_response_init(&resp);
      (void)resp_error(&resp, 429, KBC_ERR_INVALID,
                       "more than %zu requests in one second on this "
                       "connection; raise KBC_RATE_LIMIT_RPS to lift this",
                       cap);
      c->extra_hdr = "Retry-After: 1";
      conn_queue_response(c, resp.status, resp.content_type, resp.body.ptr,
                          resp.body.len, true, false);
      kbc_response_free(&resp);
      c->close_after = true;
      return;
    }
    c->rl_count++;
  }

  /* The origin split (`routes/dispatch.rs:46-50`), taken before the router and
   * for the same reason it is a FALLBACK there: an artifact Host owns every
   * non-/api path, `/` included, because `<id>.artifacts.<suffix>/` is how an
   * artifact is loaded. `/metrics` is the one exception — it is an explicit
   * route in the original too, and an explicit route answers on every origin
   * (`router.rs:1013-1021`). */
  const char *label = NULL;
  size_t llen = 0;
  if (!path_is_api(r->path) && strcmp(r->path, "/metrics") != 0 &&
      parse_artifact_host(hdr_find(r, "Host"), c->h->host_suffix, &label,
                          &llen)) {
    serve_artifact_origin(c, r);
    if (c->eof) c->close_after = true;
    return;
  }

  kbc_request req;
  memset(&req, 0, sizeof req);
  req.method = r->method;
  req.path = r->path;
  req.query = r->query;
  req.auth = r->auth != NULL ? r->auth : "";
  req.client_addr = c->peer;
  req.body = c->in.ptr + c->body_off;
  req.body_len = c->body_want;

  kbc_response resp;
  kbc_response_init(&resp);
  kbc_err err;
  kbc_err_reset(&err);
  bool unmatched = false;
  req_ctx ctx;
  memset(&ctx, 0, sizeof ctx);
  ctx.extra = &c->extra;
  ctx.m = &c->h->m;
  ctx.unmatched = &unmatched;
  /* The second carrier, straight off the parsed header table. A memset context
   * would leave it NULL, which read_presented already treats as absent, but ""
   * is stated so the field's contract matches the one on req_ctx. */
  ctx.x_kb_token = r->x_kb_token != NULL ? r->x_kb_token : "";
  /* The capture route's two headers, off the same parsed table. Stated as ""
   * when absent so the field's contract is the one req_ctx documents, and so
   * a capture with no Content-Type is a 400 rather than a NULL deref. */
  ctx.content_type = hdr_find(r, "Content-Type");
  if (ctx.content_type == NULL) ctx.content_type = "";
  ctx.x_requested_by = hdr_find(r, "X-Requested-By");
  if (ctx.x_requested_by == NULL) ctx.x_requested_by = "";
  /* The daemon itself, which the reindex route needs to reach the anchor
   * pass's state. A socketless `kbc_httpd_handle` leaves this NULL and
   * `anchors_run(NULL)` is a no-op. */
  ctx.h = c->h;
  int64_t t0 = kbc_now_ns();
  kbc_status st = dispatch(c->h->app, c->h->cfg, &req, &ctx, &resp, &err,
                           (kbc_now_ns() - c->h->started_ns) / 1000000000ll);
  /* Only /api requests are counted, because only /api requests are counted in
   * the original: `count_requests` is a layer INSIDE the /api nest, so
   * /metrics, the artifact subdomain and the static fallback are all outside
   * it (`router.rs:955-963`). */
  if (path_is_api(r->path)) {
    uint64_t ms = (uint64_t)((kbc_now_ns() - t0) / 1000000ll);
    metrics_observe(&c->h->m, r->path, ms);
  }
  if (!kbc_failed(st) && unmatched && !path_is_api(r->path)) {
    kbc_response_free(&resp);
    serve_static_origin(c, r);
    if (c->eof) c->close_after = true;
    return;
  }
  if (kbc_failed(st)) {
    kbc_response_free(&resp);
    kbc_response_init(&resp);
    (void)resp_error(&resp, 500, st, "%s", err.msg);
  }
  if (resp.sse) sse_attach(c, r->last_event_id);
  if (resp.close_after) c->close_after = true;
  conn_queue_response(c, resp.status, resp.content_type, resp.body.ptr,
                      resp.body.len, resp.close_after, resp.sse);
  kbc_response_free(&resp);
  if (c->eof) c->close_after = true;
}

static void conn_fail_and_close(conn *c, int status, kbc_status code,
                                const char *msg) {
  kbc_response r;
  kbc_response_init(&r);
  (void)resp_error(&r, status, code, "%s", msg);
  conn_queue_response(c, r.status, r.content_type, r.body.ptr, r.body.len, true,
                      false);
  kbc_response_free(&r);
  (void)conn_flush(c);
  conn_close(c->h, c);
}

/* Parses and answers as many complete pipelined requests as the buffer holds.
 * A malformed request answers 4xx and closes; it is never dropped. */
static void conn_process(conn *c) {
  for (;;) {
    if (c->sse) {
      sse_pump(c);
      conn_arm(c);
      if (conn_flush(c) == FLUSH_ERROR) conn_close(c->h, c);
      return;
    }
    if (c->in.len == 0) {
      if (c->eof) conn_close(c->h, c);
      return;
    }
    http_req r;
    memset(&r, 0, sizeof r);
    r.a = c->arena;
    int status = 400;
    kbc_err err;
    kbc_err_reset(&err);
    kbc_status st = req_parse(&r, c->in.ptr, c->in.len, &status, &err);
    if (kbc_failed(st)) {
      conn_fail_and_close(c, status, st, err.msg);
      return;
    }
    if (r.header_end == 0) {
      if (c->eof) conn_close(c->h, c);
      return; /* need more bytes */
    }
    if (c->in.len < r.header_end + r.body_want) {
      if (!c->eof) return; /* need the body */
      char msg[128];
      snprintf(msg, sizeof msg, "connection ended with %zu of %zu body bytes",
               c->in.len - r.header_end, r.body_want);
      conn_fail_and_close(c, 400, KBC_ERR_PARSE, msg);
      return;
    }
    st = req_finish(&r, &status, &err);
    if (kbc_failed(st)) {
      conn_fail_and_close(c, status, st, err.msg);
      return;
    }
    c->header_end = r.header_end;
    c->body_off = r.body_off;
    c->body_want = r.body_want;
    c->consumed = r.consumed;
    if (r.expect_continue && r.body_want > 0) {
      (void)kbc_str_puts(&c->out, "HTTP/1.1 100 Continue\r\n\r\n");
    }
    /* kbc_request.body is documented as NUL-terminated by the server, so a
     * pipelined request behind this one gets a terminator for the length of
     * the dispatch and gets its own byte back afterwards. */
    bool patched = c->consumed < c->in.len;
    char saved = '\0';
    if (patched) {
      saved = c->in.ptr[c->consumed];
      c->in.ptr[c->consumed] = '\0';
    }
    serve_request(c, &r);
    if (patched) c->in.ptr[c->consumed] = saved;

    size_t left = c->in.len - c->consumed;
    if (left > 0) memmove(c->in.ptr, c->in.ptr + c->consumed, left);
    c->in.len = left; /* the consumed request leaves the buffer */
    kbc_arena_reset(c->arena);
    c->header_end = 0;
    c->body_want = 0;
    c->body_off = 0;
    c->consumed = 0;
    if (c->eof) c->close_after = true;

    conn_arm(c);
    if (conn_flush(c) != FLUSH_DONE) return; /* response still in flight */
    if (c->close_after) {
      conn_close(c->h, c);
      return;
    }
  }
}

static void conn_on_readable(conn *c) {
  char tmp[16384];
  for (;;) {
    ssize_t n = recv(c->fd, tmp, sizeof tmp, 0);
    if (n > 0) {
      if (c->in.len + (size_t)n > KBC_REQ_BUF_MAX) {
        conn_fail_and_close(c, 413, KBC_ERR_PARSE,
                            "request exceeds the maximum request size");
        return;
      }
      if (kbc_failed(kbc_str_append(&c->in, tmp, (size_t)n))) {
        conn_close(c->h, c);
        return;
      }
      continue;
    }
    if (n == 0) {
      c->eof = true;
      break;
    }
    if (errno == EINTR) continue;
    if (errno == EAGAIN || errno == EWOULDBLOCK) break;
    conn_close(c->h, c);
    return;
  }
  conn_process(c);
}

static void conn_on_writable(conn *c) {
  if (c->sse) sse_pump(c);
  if (conn_flush(c) != FLUSH_DONE) {
    conn_arm(c);
    return;
  }
  if (c->close_after || c->eof) {
    conn_close(c->h, c);
    return;
  }
  if (c->sse) {
    conn_arm(c);
    return;
  }
  conn_arm(c);
  if (c->in.len > 0) conn_process(c);
}

static void conn_on_wake(conn *c) {
  uint64_t ticks = 0;
  ssize_t n = read(c->event_fd, &ticks, sizeof ticks);
  (void)n;
  (void)ticks;
  sse_pump(c);
  if (conn_flush(c) == FLUSH_ERROR) {
    conn_close(c->h, c);
    return;
  }
  conn_arm(c);
}

/* Keepalive tick for every stream this worker owns. */
static void worker_service_sse(kbc_worker *w) {
  int64_t now = kbc_now_ns();
  size_t i = 0;
  while (i < w->sse_len) {
    conn *c = w->sse[i];
    if (c->closed) {
      i++;
      continue;
    }
    size_t len_before = w->sse_len;
    sse_pump(c);
    if (now - c->last_write_ns >= KBC_SSE_KEEPALIVE_NS) {
      (void)kbc_str_puts(&c->out, ":keepalive\n\n");
      c->last_write_ns = now;
    }
    bool dead = false;
    if (c->out.len > c->out_off && conn_flush(c) == FLUSH_ERROR) {
      conn_close(w->h, c);
      dead = true;
    }
    if (!dead) conn_arm(c);
    /* worker_sse_del is reached only from conn_destroy, and conn_close
     * DEFERS that to worker_drain_zombies, which worker_main runs after this
     * sweep — so w->sse[] cannot shrink underneath this loop today, and a
     * plain i++ would be equivalent. The index advances only when the array
     * did not shrink, which is what keeps the sweep correct if that deferral
     * ever changes: worker_sse_del does not compact, it moves the LAST entry
     * into the vacated slot, so an unconditional increment would step over
     * the conn moved into it and the shortened length would end the tick
     * early. The `while` condition is what terminates the sweep once every
     * entry is gone. */
    if (w->sse_len == len_before) i++;
  }
}

static void worker_accept(kbc_worker *w) {
  kbc_httpd *h = w->h;
  for (;;) {
    /* The peer address is read here, at accept, because that is the only point
     * where it is the kernel's answer and not a claim by the client. It is what
     * /api/identity and the loopback admission tier are decided from. */
    struct sockaddr_storage ss;
    socklen_t slen = sizeof ss;
    memset(&ss, 0, sizeof ss);
    int fd = accept(w->lfd, (struct sockaddr *)&ss, &slen);
    if (fd < 0) {
      if (errno == EINTR) continue;
      if (errno == EAGAIN || errno == EWOULDBLOCK) {
        return; /* the backlog is drained */
      }
      if (errno == EMFILE || errno == ENFILE) {
        /* Out of descriptors: the kernel has already dropped the connection,
         * so retrying would spin. Return and let the next listener wakeup
         * try again. */
        return;
      }
      return;
    }
    /* accept4() is a GNU extension this build cannot name; the portable
     * equivalent of SOCK_NONBLOCK | SOCK_CLOEXEC is these two fcntls. */
    if (set_cloexec_nonblock(fd) < 0) {
      close(fd);
      continue;
    }
    int one = 1;
    (void)setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
    if (atomic_load(&h->conns) >= (int)KBC_HTTP_MAX_CONNECTIONS) {
      /* Refused before any conn exists, so the body is written by hand — and
       * it is the same problem+json every other error is, with a
       * Content-Length computed rather than counted by hand. */
      static const char kBusyBody[] =
          "{\"type\":\"urn:kb:errors:unavailable\",\"title\":\"Service "
          "Unavailable\",\"status\":503,\"code\":\"conflict\",\"detail\":\"too "
          "many connections\"}";
      char busy[512];
      int bn = snprintf(busy, sizeof busy,
                        "HTTP/1.1 503 Service Unavailable\r\n"
                        "Content-Type: application/problem+json; "
                        "charset=utf-8\r\n"
                        "Content-Length: %zu\r\n"
                        "Vary: Origin\r\n"
                        "X-Content-Type-Options: nosniff\r\n"
                        "Connection: close\r\n\r\n%s",
                        sizeof kBusyBody - 1, kBusyBody);
      ssize_t ignored =
          bn > 0 ? send(fd, busy, (size_t)bn, MSG_NOSIGNAL) : (ssize_t)-1;
      (void)ignored;
      close(fd);
      continue;
    }
    conn *c = calloc(1, sizeof *c);
    if (c == NULL) {
      close(fd);
      continue;
    }
    c->h = h;
    c->w = w;
    c->tag = CONN_TAG_FD;
    c->fd = fd;
    c->event_fd = -1;
    /* "?" is not an address: addr_is_loopback() fails closed on it, so a
     * connection whose peer the kernel did not name is never admitted as
     * loopback. */
    snprintf(c->peer, sizeof c->peer, "%s", "?");
    if (ss.ss_family == AF_INET) {
      char ip[INET_ADDRSTRLEN];
      const struct sockaddr_in *v4 = (const struct sockaddr_in *)(const void *)&ss;
      if (inet_ntop(AF_INET, &v4->sin_addr, ip, sizeof ip) != NULL) {
        snprintf(c->peer, sizeof c->peer, "%s", ip);
      }
    } else if (ss.ss_family == AF_INET6) {
      char ip[INET6_ADDRSTRLEN];
      const struct sockaddr_in6 *v6 =
          (const struct sockaddr_in6 *)(const void *)&ss;
      if (inet_ntop(AF_INET6, &v6->sin6_addr, ip, sizeof ip) != NULL) {
        snprintf(c->peer, sizeof c->peer, "%s", ip);
      }
    }
    kbc_str_init(&c->in);
    kbc_str_init(&c->out);
    kbc_str_init(&c->extra);
    pthread_mutex_init(&c->mu, NULL);
    c->arena = kbc_arena_new(8192);
    if (c->arena == NULL || !httpd_track(h, c)) {
      if (c->arena != NULL) kbc_arena_free(c->arena);
      pthread_mutex_destroy(&c->mu);
      kbc_str_free(&c->in);
      kbc_str_free(&c->out);
      kbc_str_free(&c->extra);
      close(fd);
      free(c);
      continue;
    }
    conn_epoll(c, EPOLLIN);
  }
}

static void *worker_main(void *arg) {
  kbc_worker *w = (kbc_worker *)arg;
  kbc_httpd *h = w->h;
  struct epoll_event events[KBC_EPOLL_MAX_EVENTS];
  while (!atomic_load(&h->stopping)) {
    int n = epoll_wait(w->epfd, events, KBC_EPOLL_MAX_EVENTS,
                       KBC_EPOLL_TIMEOUT_MS);
    if (n < 0) {
      if (errno == EINTR) continue;
      break;
    }
    for (int i = 0; i < n; i++) {
      /* The listener and the wake pipe register their own address as the
       * epoll payload, so they are recognised by identity and never
       * dereferenced as a conn. */
      void *p = events[i].data.ptr;
      if (p == (void *)&w->lfd) {
        worker_accept(w);
        continue;
      }
      if (p == (void *)&h->wake_rd) {
        char drain[64];
        while (read(h->wake_rd, drain, sizeof drain) > 0) {
        }
        continue;
      }
      conn *c = (conn *)p;
      if (c->closed) continue;
      uint32_t ev = events[i].events;
      if ((ev & (EPOLLHUP | EPOLLERR)) != 0) {
        conn_close(h, c);
        continue;
      }
      if (c->tag == CONN_TAG_EV) {
        conn_on_wake(c);
        continue;
      }
      if ((ev & EPOLLOUT) != 0) {
        conn_on_writable(c);
        continue;
      }
      if ((ev & EPOLLIN) != 0) conn_on_readable(c);
    }
    worker_service_sse(w);
    worker_drain_zombies(w);
    /* The watcher's single-file re-index has no route, so the tick is what
     * makes an anchor transition observable there. 200 ms of latency on a
     * document a human just saved is not worth optimising away, and the flag
     * means an idle daemon does no work at all. */
    anchors_tick(h);
  }
  /* Drain: nothing this worker owns may outlive it, or stop() leaks fds. */
  worker_drain_zombies(w);
  while (w->sse_len > 0) {
    conn *c = w->sse[0];
    conn_close(h, c);
    worker_drain_zombies(w);
  }
  return NULL;
}

/* ------------------------------------------------------------ start/stop -- */

static int set_cloexec_nonblock(int fd) {
  int fl = fcntl(fd, F_GETFL, 0);
  if (fl < 0) return -1;
  if (fcntl(fd, F_SETFL, fl | O_NONBLOCK) < 0) return -1;
  int fd_flags = fcntl(fd, F_GETFD, 0);
  if (fd_flags >= 0) (void)fcntl(fd, F_SETFD, fd_flags | FD_CLOEXEC);
  return 0;
}

static int make_listener(const char *addr, int port, kbc_err *err) {
  struct addrinfo hints;
  memset(&hints, 0, sizeof hints);
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;
  hints.ai_flags = AI_PASSIVE | AI_NUMERICHOST;
  char portbuf[16];
  snprintf(portbuf, sizeof portbuf, "%d", port);
  struct addrinfo *res = NULL;
  int gai = getaddrinfo(addr, portbuf, &hints, &res);
  if (gai != 0 || res == NULL) {
    (void)kbc_err_set(err, KBC_ERR_INVALID, "resolve %s: %s", addr,
                      gai_strerror(gai));
    return -1;
  }
  int fd = -1;
  for (struct addrinfo *ai = res; ai != NULL; ai = ai->ai_next) {
    fd = socket(ai->ai_family, ai->ai_socktype | SOCK_NONBLOCK | SOCK_CLOEXEC,
                ai->ai_protocol);
    if (fd < 0) continue;
    int one = 1;
    (void)setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
#ifdef SO_REUSEPORT
    (void)setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, &one, sizeof one);
#endif
    if (bind(fd, ai->ai_addr, ai->ai_addrlen) == 0 &&
        listen(fd, KBC_LISTEN_BACKLOG) == 0) {
      break;
    }
    close(fd);
    fd = -1;
  }
  freeaddrinfo(res);
  if (fd < 0) {
    (void)kbc_err_set(err, KBC_ERR_CONFLICT, "bind %s:%d: %s", addr, port,
                      strerror(errno));
    return -1;
  }
  (void)set_cloexec_nonblock(fd);
  return fd;
}

static int listener_port(int fd) {
  struct sockaddr_storage ss;
  socklen_t len = sizeof ss;
  if (getsockname(fd, (struct sockaddr *)&ss, &len) != 0) return -1;
  if (ss.ss_family == AF_INET) {
    return ntohs(((struct sockaddr_in *)&ss)->sin_port);
  }
  if (ss.ss_family == AF_INET6) {
    return ntohs(((struct sockaddr_in6 *)&ss)->sin6_port);
  }
  return -1;
}

kbc_httpd *kbc_httpd_start(kbc_app *app, const kbc_config *cfg, kbc_err *err) {
  if (app == NULL || cfg == NULL) {
    (void)kbc_err_set(err, KBC_ERR_INVALID, "kbc_httpd_start: %s",
                      app == NULL ? "app" : "cfg");
    return NULL;
  }
  /* The token rule is enforced before any socket exists: a daemon that would
   * serve an open API on a routable address does not start at all. */
  char why[128];
  if (!kbc_config_bind_is_safe(cfg, why, sizeof why)) {
    (void)kbc_err_set(err, KBC_ERR_INVALID, "refusing to bind %s: %s",
                      cfg->bind_addr != NULL ? cfg->bind_addr : "?", why);
    return NULL;
  }
  size_t want = cfg->http_workers != 0 ? cfg->http_workers : 4u;
  if (want < KBC_WORKER_MIN) want = KBC_WORKER_MIN;
  if (want > KBC_WORKER_MAX) want = KBC_WORKER_MAX;

  kbc_httpd *h = calloc(1, sizeof *h);
  if (h == NULL) {
    (void)kbc_err_set(err, KBC_ERR_NOMEM, "kbc_httpd_start: httpd");
    return NULL;
  }
  h->app = app;
  h->cfg = cfg;
  h->workers = (int)want;
  h->port = cfg->port;
  h->wake_rd = -1;
  h->wake_wr = -1;
  /* Everything kbc_httpd_stop tears down is made valid HERE, ahead of the
   * first failure path: the three mutexes it destroys, the three atomics it
   * stores to, and the CORS list it frees. calloc hands back zeros, and a
   * zero is not an initialised mutex. */
  atomic_init(&h->conns, 0);
  atomic_init(&h->stopping, false);
  atomic_init(&h->anchors_dirty, false);
  kbc_strlist_init(&h->cors);
  pthread_mutex_init(&h->conns_mu, NULL);
  pthread_mutex_init(&h->ring_mu, NULL);
  pthread_mutex_init(&h->anchors_mu, NULL);

  h->bind_addr = strdup(cfg->bind_addr != NULL ? cfg->bind_addr : "127.0.0.1");
  h->listen_fd = calloc(want, sizeof *h->listen_fd);
  h->w = calloc(want, sizeof *h->w);
  h->threads = calloc(want, sizeof *h->threads);
  if (h->bind_addr == NULL || h->listen_fd == NULL || h->w == NULL ||
      h->threads == NULL) {
    (void)kbc_err_set(err, KBC_ERR_NOMEM, "kbc_httpd_start: worker arrays");
    kbc_httpd_stop(h);
    return NULL;
  }
  /* kbc_httpd_stop closes every listen_fd and every worker epfd that reads
   * >= 0, and calloc leaves those slots at 0 — which is a VALID descriptor.
   * A failure before this sweep therefore made stop close the CALLER's
   * descriptor 0, its stdin, once per worker. The sweep sits directly after
   * the allocations it covers, so every path below hands stop a fully-formed
   * object. */
  for (size_t i = 0; i < want; i++) {
    h->listen_fd[i] = -1;
    h->w[i].epfd = -1;
    h->w[i].lfd = -1;
  }

  /* The two knobs that have no home in kbc_config. They are read ONCE here,
   * into httpd-owned memory, and never again: a worker must not call getenv
   * on the request path, and a per-connection cap has to have one threshold
   * for the life of the daemon. The config fields they should become are in
   * the port report; until the header owns them, the environment is the
   * honest place for them. */
  h->rate_limit = KBC_RATE_LIMIT_DEFAULT;
  const char *origins = getenv("KBC_CORS_ORIGINS");
  if (origins != NULL) {
    /* Comma-separated, exact origins. Whitespace around a value is trimmed,
     * an empty element is skipped: a trailing comma is a typo, not an origin. */
    const char *p = origins;
    while (*p != '\0' && h->cors.len < KBC_CORS_MAX_ORIGINS) {
      const char *comma = strchr(p, ',');
      size_t n = comma != NULL ? (size_t)(comma - p) : strlen(p);
      while (n > 0 && (*p == ' ' || *p == '\t')) {
        p++;
        n--;
      }
      while (n > 0 && (p[n - 1] == ' ' || p[n - 1] == '\t')) n--;
      if (n > 0) {
        char *one = malloc(n + 1);
        if (one == NULL) {
          (void)kbc_err_set(err, KBC_ERR_NOMEM, "KBC_CORS_ORIGINS: alloc");
          kbc_httpd_stop(h);
          return NULL;
        }
        memcpy(one, p, n);
        one[n] = '\0';
        /* push copies, so `one` is ours to release either way — the list holds
         * its own copy and nothing else points at this buffer. */
        kbc_status ps = kbc_strlist_push(&h->cors, one);
        KBC_LOGI("httpd: allowing CORS origin %s", one);
        free(one);
        if (kbc_failed(ps)) {
          (void)kbc_err_set(err, KBC_ERR_NOMEM, "KBC_CORS_ORIGINS: list");
          kbc_httpd_stop(h);
          return NULL;
        }
      }
      if (comma == NULL) break;
      p = comma + 1;
    }
  }
  const char *rps = getenv("KBC_RATE_LIMIT_RPS");
  if (rps != NULL && rps[0] != '\0') {
    char *end = NULL;
    errno = 0;
    unsigned long v = strtoul(rps, &end, 10);
    if (end == rps || *end != '\0' || errno != 0) {
      (void)kbc_err_set(err, KBC_ERR_INVALID,
                         "KBC_RATE_LIMIT_RPS: \"%s\" is not a number", rps);
      kbc_httpd_stop(h);
      return NULL;
    }
    h->rate_limit = v;
  }

  /* The origin split's bring-up. Everything here is read ONCE: a worker must
   * not call getenv on the request path, and the corpus roots in particular
   * must not be canonicalised per request — the containment guard's soundness
   * depends on the root it compares against being a fixed, already-resolved
   * path (`artifact.rs:597-602`). */
  const char *suffix = getenv("KBC_ARTIFACT_HOST_SUFFIX");
  if (suffix == NULL || suffix[0] == '\0') suffix = KBC_HOST_SUFFIX_DEFAULT;
  const char *parent = getenv("KBC_PARENT_ORIGIN");
  if (parent == NULL) parent = KBC_PARENT_ORIGIN_DEFAULT;
  h->host_suffix = strdup(suffix);
  h->parent_origin = strdup(parent);
  if (h->host_suffix == NULL || h->parent_origin == NULL) {
    (void)kbc_err_set(err, KBC_ERR_NOMEM, "httpd: origin strings");
    kbc_httpd_stop(h);
    return NULL;
  }
  /* A suffix the host parser can never match is a misconfiguration worth
   * naming at start rather than at the first request: every Host would fall
   * through to the parent origin and the subdomain would be unreachable. */
  for (const char *q = h->host_suffix; *q != '\0'; q++) {
    if (!(strchr("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ"
                 "0123456789.-_", *q) != NULL)) {
      (void)kbc_err_set(err, KBC_ERR_INVALID,
                        "KBC_ARTIFACT_HOST_SUFFIX: \"%s\" is not a hostname "
                        "suffix",
                        h->host_suffix);
      kbc_httpd_stop(h);
      return NULL;
    }
  }
  const char *detailed = getenv("KBC_METRICS_DETAILED");
  h->m.detailed = detailed != NULL && strcmp(detailed, "1") == 0;
  h->nroots = cfg->ncorpora;
  if (h->nroots > 0) {
    h->roots = calloc(h->nroots, sizeof *h->roots);
    if (h->roots == NULL) {
      (void)kbc_err_set(err, KBC_ERR_NOMEM, "httpd: corpus roots");
      kbc_httpd_stop(h);
      return NULL;
    }
  }
  for (size_t i = 0; i < h->nroots; i++) {
    const kbc_corpus_cfg *c = &cfg->corpora[i];
    h->roots[i].name = c->name;
    /* NULL when the corpus does not exist yet: there is nothing to serve from
     * it, and a guard against a NULL root would be a guard that cannot fail. */
    h->roots[i].root = canon(c->path);
    if (i < KBC_MAX_CORPORA) {
      h->m.kb_names[i] = c->name;
      /* Pre-seeded at bring-up so an observation is a lookup, never an insert
       * on the hot path — and so a configured kb renders at 0 in the
       * exposition before its first request (`state.rs:432-436`). */
      atomic_init(&h->m.per_kb[i].count, 0);
      for (size_t b = 0; b < KBC_LAT_SLOTS; b++) {
        atomic_init(&h->m.per_kb[i].slot[b], 0);
      }
      h->m.n_kb = i + 1;
    }
    if (h->roots[i].root == NULL) {
      KBC_LOGW("httpd: corpus %s has no readable root at %s", c->name, c->path);
    }
  }
  atomic_init(&h->m.total, 0);
  for (size_t i = 0; i < KBC_ROUTE_KINDS; i++) {
    atomic_init(&h->m.route[i].count, 0);
    for (size_t b = 0; b < KBC_LAT_SLOTS; b++) {
      atomic_init(&h->m.route[i].slot[b], 0);
    }
  }
  /* The static root, resolved the same way: KB_SPA_DIST when it holds an
   * index.html, and no root at all otherwise, which is a 404 that says why
   * (`spa.rs:412-427`). */
  const char *dist = getenv("KB_SPA_DIST");
  if (dist != NULL && dist[0] != '\0') {
    char *shell = path_join(dist, "index.html");
    if (shell != NULL) {
      char *c = canon(shell);
      free(shell);
      if (c != NULL) {
        h->spa_root = canon(dist);
        free(c);
      }
    }
    if (h->spa_root == NULL) {
      (void)kbc_err_set(err, KBC_ERR_INVALID,
                        "KB_SPA_DIST: \"%s\" has no index.html to serve",
                        dist);
      kbc_httpd_stop(h);
      return NULL;
    }
  }
  /* The wake pipe is the only cross-thread wake-up a worker needs: one read end
   * is registered in every epoll set. */
  int fds[2];
  if (pipe(fds) != 0) {
    (void)kbc_err_set(err, KBC_ERR_IO, "pipe: %s", strerror(errno));
    kbc_httpd_stop(h);
    return NULL;
  }
  h->wake_rd = fds[0];
  h->wake_wr = fds[1];
  (void)set_cloexec_nonblock(h->wake_rd);
  (void)set_cloexec_nonblock(h->wake_wr);
  h->started_ns = kbc_now_ns();

  /* One listening socket per worker. Port 0 would hand a DIFFERENT port to
   * each SO_REUSEPORT socket, so the first one picks the port for the rest. */
  for (size_t i = 0; i < want; i++) {
    int fd = make_listener(h->bind_addr, h->port, err);
    if (fd < 0) {
      kbc_httpd_stop(h);
      return NULL;
    }
    if (i == 0) {
      int p = listener_port(fd);
      if (p > 0) h->port = p;
    }
    h->listen_fd[i] = fd;
  }

  for (size_t i = 0; i < want; i++) {
    kbc_worker *w = &h->w[i];
    w->h = h;
    w->idx = (int)i;
    w->lfd = h->listen_fd[i];
    w->epfd = epoll_create1(EPOLL_CLOEXEC);
    if (w->epfd < 0) {
      (void)kbc_err_set(err, KBC_ERR_IO, "epoll_create1: %s", strerror(errno));
      kbc_httpd_stop(h);
      return NULL;
    }
    struct epoll_event ev;
    memset(&ev, 0, sizeof ev);
    ev.events = EPOLLIN;
    ev.data.ptr = &w->lfd;
    if (epoll_ctl(w->epfd, EPOLL_CTL_ADD, w->lfd, &ev) != 0) {
      (void)kbc_err_set(err, KBC_ERR_IO, "epoll_ctl(listen %zu): %s", i,
                        strerror(errno));
      kbc_httpd_stop(h);
      return NULL;
    }
    ev.data.ptr = &h->wake_rd;
    if (epoll_ctl(w->epfd, EPOLL_CTL_ADD, h->wake_rd, &ev) != 0) {
      (void)kbc_err_set(err, KBC_ERR_IO, "epoll_ctl(wake %zu): %s", i,
                        strerror(errno));
      kbc_httpd_stop(h);
      return NULL;
    }
  }
  /* SEAL THE ROUTE TABLE. From here the workers exist and can be dispatching,
   * so kbc_httpd_routes_add must refuse: that refusal is what makes the
   * dispatcher's lock-free read of g_view correct. Cleared again in
   * kbc_httpd_stop, so a test that stops its daemon can register again. */
  atomic_store(&g_registry_sealed, true);


  h->sub_id = kbc_app_subscribe(app, httpd_on_event, h);
  h->subscribed = true;

  for (size_t i = 0; i < want; i++) {
    if (pthread_create(&h->threads[i], NULL, worker_main, &h->w[i]) != 0) {
      (void)kbc_err_set(err, KBC_ERR_IO, "pthread_create(worker %zu)", i);
      break;
    }
    h->started_threads++;
  }
  if (h->started_threads != h->workers) {
    kbc_httpd_stop(h);
    return NULL;
  }
  KBC_LOGI("httpd listening on %s:%d with %zu workers", h->bind_addr, h->port,
           want);
  return h;
}

void kbc_httpd_stop(kbc_httpd *h) {
  if (h == NULL) return;
  if (h->subscribed && h->app != NULL) kbc_app_unsubscribe(h->app, h->sub_id);
  h->subscribed = false;
  /* The epoll timeout is 200ms, so every thread leaves its loop well inside 2s
   * of this write even if the wake-up itself is missed. */
  atomic_store(&h->stopping, true);
  if (h->wake_wr >= 0) {
    ssize_t ignored = write(h->wake_wr, "x", 1);
    (void)ignored;
  }
  for (int i = 0; i < h->started_threads; i++) {
    (void)pthread_join(h->threads[i], NULL);
  }
  /* UNSEAL the route table, AFTER the joins above: until the last worker has
   * been reaped a request can still be reading g_view, so the table stays
   * sealed across the whole of stop. Once it is clear, registration is safe
   * again, which is what lets a test stop its daemon and register another
   * route. */
  atomic_store(&g_registry_sealed, false);
  if (h->w != NULL) {
    for (int i = 0; i < h->workers; i++) {
      free(h->w[i].sse);
      h->w[i].sse = NULL;
      free(h->w[i].zomb);
      h->w[i].zomb = NULL;
      if (h->w[i].epfd >= 0) close(h->w[i].epfd);
    }
  }
  if (h->listen_fd != NULL) {
    for (int i = 0; i < h->workers; i++) {
      if (h->listen_fd[i] >= 0) close(h->listen_fd[i]);
    }
  }
  if (h->wake_rd >= 0) close(h->wake_rd);
  if (h->wake_wr >= 0) close(h->wake_wr);
  free(h->all_conns);
  for (size_t i = 0; i < KBC_SSE_RING_CAP; i++) {
    free(h->ring[i].type);
    free(h->ring[i].json);
  }
  for (size_t i = 0; i < h->nroots; i++) free(h->roots[i].root);
  free(h->roots);
  free(h->host_suffix);
  free(h->parent_origin);
  free(h->spa_root);
  kbc_strlist_free(&h->cors);
  free(h->bind_addr);
  free(h->listen_fd);
  free(h->w);
  free(h->threads);
  pthread_mutex_destroy(&h->conns_mu);
  free(h->anchors);
  pthread_mutex_destroy(&h->anchors_mu);
  pthread_mutex_destroy(&h->ring_mu);
  free(h);
}

int kbc_httpd_port(const kbc_httpd *h) { return h != NULL ? h->port : 0; }

/* CORS, the per-connection request cap, the artifact-subdomain suffix, the
 * parent origin, the static root and the detailed metrics layer have no field
 * in kbc_config, so they are read once at start from KBC_CORS_ORIGINS,
 * KBC_RATE_LIMIT_RPS, KBC_ARTIFACT_HOST_SUFFIX, KBC_PARENT_ORIGIN,
 * KB_SPA_DIST and KBC_METRICS_DETAILED; see kbc_httpd_start. */
/* ------------------------------------------------------ (6) route export --
 *
 * KBC_ROUTES and KBC_ROUTES_LEN are a VIEW over the registry's storage, not
 * a second copy of it: KBC_ROUTES is g_view[0] and the length is the built-in
 * row count, which is exactly what both symbols meant before the registry
 * existed. The live count — built-ins plus everything any subsystem has
 * registered — is kbc_httpd_routes()'s, and it reads the same array, so the
 * two cannot disagree about any row they share. See the registry section for
 * why the length is fixed at link time. */
const kbc_route *const KBC_ROUTES = g_view;
const size_t KBC_ROUTES_LEN = KBC_ROUTE_SEED_LEN;
