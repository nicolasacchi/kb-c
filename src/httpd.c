/* httpd.c — epoll + SO_REUSEPORT HTTP/1.1 daemon.
 *
 * Organised as: (1) socket + worker plumbing, (2) HTTP/1.1 parsing,
 * (3) the routing table, (4) the SSE endpoint, (5) the KBC_ROUTES export.
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
#include <sys/types.h>
#include <unistd.h>

#include "kbc/httpd.h"
#include "kbc/json.h"
#include "kbc/log.h"

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

static const char *status_text(int status) {
  switch (status) {
  case 200: return "OK";
  case 202: return "Accepted";
  case 400: return "Bad Request";
  case 401: return "Unauthorized";
  case 403: return "Forbidden";
  case 404: return "Not Found";
  case 405: return "Method Not Allowed";
  case 413: return "Payload Too Large";
  case 414: return "URI Too Long";
  case 500: return "Internal Server Error";
  case 503: return "Service Unavailable";
  default: return "Error";
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

  kbc_str in;
  size_t in_len;
  kbc_str out;
  size_t out_off;
  size_t header_end; /* offset just past CRLFCRLF, 0 until headers complete */
  size_t body_want;  /* Content-Length, 0 when absent */
  size_t body_off;   /* offset of the body start */
  size_t consumed;   /* header_end + body_want */

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
  sse_hist ring[KBC_SSE_RING_CAP];
  size_t ring_next; /* next slot to write; == the oldest when full */
  uint64_t next_id;
};

/* -------------------------------------------------------- response bits -- */

static const char *const CT_JSON = "application/json; charset=utf-8";
static const char *const CT_TEXT = "text/plain; charset=utf-8";
static const char *const CT_SSE = "text/event-stream; charset=utf-8";

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

kbc_status kbc_response_error_json(kbc_response *r, int status, kbc_status code,
                                   const char *msg) {
  if (!r) return kbc_err_set(NULL, KBC_ERR_INVALID, "kbc_response_error_json: r");
  kbc_response_free(r);
  r->status = status;
  r->content_type = CT_JSON;
  kbc_str s;
  kbc_str_init(&s);
  kbc_status st = kbc_str_puts(&s, "{\"error\":{\"status\":");
  if (kbc_failed(st)) goto done;
  st = kbc_str_printf(&s, "%d,\"code\":\"%s\",\"message\":", status,
                      kbc_status_str(code));
  if (kbc_failed(st)) goto done;
  st = kbc_str_append_json_string(&s, msg ? msg : "", msg ? strlen(msg) : 0);
  if (kbc_failed(st)) goto done;
  st = kbc_str_puts(&s, "}}");
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
    }
    if (o + 2 > n + 2) {
      return kbc_err_set(err, KBC_ERR_PARSE, "path normalization overflow");
    }
    norm[o++] = (char)c;
  }
  if (o == 1) norm[o++] = '/';
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

static kbc_status artifact_json(kbc_str *out, const kbc_artifact *art,
                                bool with_source) {
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
  st = kbc_str_append_json_string(out, art->title ? art->title : "",
                                  art->title ? strlen(art->title) : 0);
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
  int64_t off = query_int(query, "offset");
  if (query_has(query, "offset") && off < 0) {
    return kbc_err_set(err, KBC_ERR_INVALID, "offset must be an integer >= 0");
  }
  *offset = (size_t)off;
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
    st = artifact_json(out, &rows[i], false);
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
  return artifact_json(out, &art, with_source);
}

static kbc_status route_reindex(kbc_app *app, kbc_str *out, kbc_err *err) {
  int64_t t0 = kbc_now_ns();
  kbc_status st = kbc_app_reindex(app, err);
  if (kbc_failed(st)) return st;
  int64_t took_us = (kbc_now_ns() - t0) / 1000;
  kbc_app_stats stats;
  memset(&stats, 0, sizeof stats);
  kbc_err local;
  kbc_err_reset(&local);
  (void)kbc_app_stats_get(app, &stats, &local);
  return kbc_str_printf(out, "{\"docs\":%lld,\"took_us\":%lld}",
                        (long long)stats.last_reindex_docs,
                        (long long)took_us);
}

static kbc_status route_banner(kbc_str *out) {
  return kbc_str_printf(
      out,
      "kb-c %s - self-hosted system of record for agent artifacts.\n"
      "This port serves a JSON API only; there is no reader UI here.\n"
      "Routes:\n"
      "  GET  /api/health\n"
      "  GET  /api/search?q=<terms>&kb=<corpus>&kind=&mode=&limit=&offset=\n"
      "  GET  /api/artifacts?kb=<corpus>&kind=&limit=&offset=\n"
      "  GET  /api/artifacts/{id}?source=1\n"
      "  POST /api/reindex\n"
      "  GET  /api/events\n"
      "  GET  /api/stats\n",
      KBC_VERSION);
}

/* No token configured -> open. Otherwise a well-formed Bearer token must match
 * in constant time: a missing or malformed header is 401, a wrong one 403. */
static kbc_status check_auth(const kbc_config *cfg, const kbc_request *req,
                             kbc_response *out) {
  if (cfg == NULL || cfg->token == NULL || cfg->token[0] == '\0') return KBC_OK;
  const char *auth = req->auth != NULL ? req->auth : "";
  static const char kPrefix[] = "Bearer ";
  if (strncmp(auth, kPrefix, sizeof kPrefix - 1) != 0) {
    (void)kbc_response_error_json(
        out, 401, KBC_ERR_INVALID, "Authorization: Bearer <token> is required");
    return KBC_ERR_INVALID;
  }
  const char *tok = auth + sizeof kPrefix - 1;
  size_t want = strlen(cfg->token);
  size_t got = strlen(tok);
  /* Never compare past the presented token: it lives in a request arena and a
   * wrong-length token may be shorter than the real one. On a length mismatch
   * we still do the same work over the expected token, so the length of what
   * was presented is not an obvious timing oracle. */
  if (got != want) {
    (void)kbc_const_time_eq(cfg->token, cfg->token, want);
    (void)kbc_response_error_json(out, 403, KBC_ERR_INVALID,
                                  "invalid bearer token");
    return KBC_ERR_INVALID;
  }
  if (!kbc_const_time_eq(tok, cfg->token, want)) {
    (void)kbc_response_error_json(out, 403, KBC_ERR_INVALID,
                                  "invalid bearer token");
    return KBC_ERR_INVALID;
  }
  return KBC_OK;
}

static kbc_status method_not_allowed(kbc_response *out, const char *m,
                                     const char *p) {
  return resp_error(out, 405, KBC_ERR_INVALID, "%s is not allowed on %s", m, p);
}

static const char *err_msg(const kbc_err *err, kbc_status st) {
  if (err == NULL) return kbc_status_str(st);
  return err->msg[0] != '\0' ? err->msg : kbc_status_str(st);
}

static kbc_status dispatch(kbc_app *app, const kbc_config *cfg,
                           const kbc_request *req, kbc_response *out,
                           kbc_err *err, int64_t uptime_s) {
  const char *m = req->method;
  const char *p = req->path;
  bool is_get = strcmp(m, "GET") == 0;
  bool is_post = strcmp(m, "POST") == 0;

  if (is_get && strcmp(p, "/") == 0) {
    out->content_type = CT_TEXT;
    return route_banner(&out->body);
  }
  if (is_get && strcmp(p, "/api/health") == 0) {
    return route_health(app, &out->body, uptime_s);
  }

  if (kbc_failed(check_auth(cfg, req, out))) return KBC_OK;

  if (strcmp(p, "/api/stats") == 0) {
    if (!is_get) return method_not_allowed(out, m, p);
    kbc_status st = route_stats(app, &out->body, err);
    if (kbc_failed(st)) return resp_error(out, 500, st, "stats: %s", err_msg(err, st));
    return KBC_OK;
  }
  if (strcmp(p, "/api/search") == 0) {
    if (!is_get) return method_not_allowed(out, m, p);
    kbc_arena *a = kbc_arena_new(16384);
    if (a == NULL) return resp_error(out, 500, KBC_ERR_NOMEM, "no arena");
    kbc_status st = route_search(app, cfg, a, req->query, &out->body, err);
    kbc_arena_free(a);
    if (kbc_failed(st)) {
      return resp_error(out, 400, st, "%s", err_msg(err, st));
    }
    return KBC_OK;
  }
  if (strcmp(p, "/api/artifacts") == 0) {
    if (!is_get) return method_not_allowed(out, m, p);
    kbc_arena *a = kbc_arena_new(16384);
    if (a == NULL) return resp_error(out, 500, KBC_ERR_NOMEM, "no arena");
    kbc_status st = route_artifacts(app, a, req->query, &out->body, err);
    kbc_arena_free(a);
    if (kbc_failed(st)) {
      return resp_error(out, 400, st, "%s", err_msg(err, st));
    }
    return KBC_OK;
  }
  static const char kOne[] = "/api/artifacts/";
  if (strncmp(p, kOne, sizeof kOne - 1) == 0 && p[sizeof kOne - 1] != '\0') {
    if (!is_get) return method_not_allowed(out, m, p);
    const char *id = p + sizeof kOne - 1;
    if (strchr(id, '/') != NULL) {
      return resp_error(out, 404, KBC_ERR_NOTFOUND, "no route for %s", p);
    }
    kbc_arena *a = kbc_arena_new(16384);
    if (a == NULL) return resp_error(out, 500, KBC_ERR_NOMEM, "no arena");
    kbc_status st = route_artifact_one(app, a, id, req->query, &out->body, err);
    kbc_arena_free(a);
    if (st == KBC_ERR_NOTFOUND) {
      return resp_error(out, 404, st, "no artifact with id %s", id);
    }
    if (kbc_failed(st)) {
      return resp_error(out, 400, st, "%s", err_msg(err, st));
    }
    return KBC_OK;
  }
  if (strcmp(p, "/api/reindex") == 0) {
    if (!is_post) return method_not_allowed(out, m, p);
    kbc_status st = route_reindex(app, &out->body, err);
    if (kbc_failed(st)) {
      return resp_error(out, 500, st, "reindex: %s", err_msg(err, st));
    }
    out->status = 202;
    return KBC_OK;
  }
  if (strcmp(p, "/api/events") == 0) {
    if (!is_get) return method_not_allowed(out, m, p);
    /* Over a socket this becomes a live stream; a socketless caller gets the
     * head of that stream, which is the whole non-streaming part of it. */
    out->status = 200;
    out->content_type = CT_SSE;
    out->sse = true;
    return kbc_str_puts(&out->body, "retry: 3000\n\n:ok\n\n");
  }
  return resp_error(out, 404, KBC_ERR_NOTFOUND, "no route for %s %s", m, p);
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
  return dispatch(app, cfg, req, out, err, 0);
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

/* Ring push; drops the OLDEST frame on overflow and counts it, so a stalled
 * client can never make the daemon grow without limit. Caller holds c->mu. */
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

static void httpd_on_event(void *user, const char *type, const char *json) {
  kbc_httpd *h = (kbc_httpd *)user;
  if (h == NULL || type == NULL) return;
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
   * keeps the queue consistent. The frame is copied because the worker frees
   * it once written. */
  pthread_mutex_lock(&h->conns_mu);
  for (size_t i = 0; i < h->all_len; i++) {
    conn *c = h->all_conns[i];
    if (!c->sse || c->closed) continue;
    char *copy = malloc(f.len + 1);
    if (copy == NULL) continue;
    memcpy(copy, f.ptr, f.len);
    copy[f.len] = '\0';
    pthread_mutex_lock(&c->mu);
    sse_push_locked(c, copy, f.len);
    pthread_mutex_unlock(&c->mu);
    uint64_t one = 1;
    /* EAGAIN here only means the counter is already non-zero, i.e. the worker
     * has a wake-up pending anyway. */
    ssize_t ignored = write(c->event_fd, &one, sizeof one);
    (void)ignored;
  }
  pthread_mutex_unlock(&h->conns_mu);

done:
  free(tcopy);
  free(jcopy);
  kbc_str_free(&f);
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
    (void)ptr_push(&c->w->zomb, &c->w->zomb_len, &c->w->zomb_cap,
                   KBC_HTTP_MAX_CONNECTIONS, c);
  } else {
    conn_destroy(c);
  }
}

static void worker_drain_zombies(kbc_worker *w) {
  for (size_t i = 0; i < w->zomb_len; i++) conn_destroy(w->zomb[i]);
  w->zomb_len = 0;
}

static bool httpd_track(kbc_httpd *h, conn *c) {
  bool ok = ptr_push(&h->all_conns, &h->all_len, &h->all_cap,
                     KBC_HTTP_MAX_CONNECTIONS, c);
  if (ok) atomic_fetch_add(&h->conns, 1);
  return ok;
}

/* ------------------------------------------------------------ write path -- */

static kbc_status queue_headers(conn *c, int status, const char *ct,
                                size_t body_len, bool close_after, bool sse) {
  kbc_status st = kbc_str_printf(&c->out, "HTTP/1.1 %d %s\r\n", status,
                                 status_text(status));
  if (kbc_failed(st)) return st;
  st = kbc_str_printf(&c->out, "Content-Type: %s\r\n", ct);
  if (kbc_failed(st)) return st;
  /* An SSE response has no Content-Length: the body never ends, and claiming a
   * length would tell the client to wait for bytes that are not coming. */
  if (!sse) {
    st = kbc_str_printf(&c->out, "Content-Length: %zu\r\n", body_len);
    if (kbc_failed(st)) return st;
  }
  st = kbc_str_puts(&c->out, "X-Content-Type-Options: nosniff\r\n");
  if (kbc_failed(st)) return st;
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

  /* EFD_NONBLOCK|EFD_CLOEXEC are GNU extensions; the portable equivalent is
   * a plain eventfd plus the same two fcntls used for accepted sockets. */
  c->event_fd = eventfd(0, 0);
  if (c->event_fd >= 0 && set_cloexec_nonblock(c->event_fd) < 0) {
    close(c->event_fd);
    c->event_fd = -1;
  }
  c->sse = true;
  c->qcap = KBC_SSE_QUEUE_CAP;
  c->q = calloc(c->qcap, sizeof *c->q);
  if (c->q == NULL || c->event_fd < 0) {
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
  if (last_event_id != NULL && last_event_id[0] != '\0') {
    char *end = NULL;
    errno = 0;
    unsigned long long v = strtoull(last_event_id, &end, 10);
    if (end != last_event_id && *end == '\0' && errno == 0) {
      sse_replay(c->h, c, (uint64_t)v);
    }
  }
}

static void serve_request(conn *c, const http_req *r) {
  kbc_request req;
  memset(&req, 0, sizeof req);
  req.method = r->method;
  req.path = r->path;
  req.query = r->query;
  req.auth = r->auth != NULL ? r->auth : "";
  req.client_addr = "peer";
  req.body = c->in.ptr + c->body_off;
  req.body_len = c->body_want;

  kbc_response resp;
  kbc_response_init(&resp);
  kbc_err err;
  kbc_err_reset(&err);
  kbc_status st = dispatch(c->h->app, c->h->cfg, &req, &resp, &err,
                           (kbc_now_ns() - c->h->started_ns) / 1000000000ll);
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
    if (c->in_len == 0) {
      if (c->eof) conn_close(c->h, c);
      return;
    }
    http_req r;
    memset(&r, 0, sizeof r);
    r.a = c->arena;
    int status = 400;
    kbc_err err;
    kbc_err_reset(&err);
    kbc_status st = req_parse(&r, c->in.ptr, c->in_len, &status, &err);
    if (kbc_failed(st)) {
      conn_fail_and_close(c, status, st, err.msg);
      return;
    }
    if (r.header_end == 0) {
      if (c->eof) conn_close(c->h, c);
      return; /* need more bytes */
    }
    if (c->in_len < r.header_end + r.body_want) {
      if (!c->eof) return; /* need the body */
      char msg[128];
      snprintf(msg, sizeof msg, "connection ended with %zu of %zu body bytes",
               c->in_len - r.header_end, r.body_want);
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
    bool patched = c->consumed < c->in_len;
    char saved = '\0';
    if (patched) {
      saved = c->in.ptr[c->consumed];
      c->in.ptr[c->consumed] = '\0';
    }
    serve_request(c, &r);
    if (patched) c->in.ptr[c->consumed] = saved;

    size_t left = c->in_len - c->consumed;
    if (left > 0) memmove(c->in.ptr, c->in.ptr + c->consumed, left);
    c->in_len = left;
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
      if (c->in_len + (size_t)n > KBC_REQ_BUF_MAX) {
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
  if (c->in_len > 0) conn_process(c);
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
  for (size_t i = 0; i < w->sse_len; i++) {
    conn *c = w->sse[i];
    if (c->closed) continue;
    sse_pump(c);
    if (now - c->last_write_ns >= KBC_SSE_KEEPALIVE_NS) {
      (void)kbc_str_puts(&c->out, ":keepalive\n\n");
      c->last_write_ns = now;
    }
    if (c->out.len > c->out_off && conn_flush(c) == FLUSH_ERROR) {
      conn_close(w->h, c);
      continue;
    }
    conn_arm(c);
  }
}

static void worker_accept(kbc_worker *w) {
  kbc_httpd *h = w->h;
  for (;;) {
    int fd = accept(w->lfd, NULL, NULL);
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
      static const char kBusy[] =
          "HTTP/1.1 503 Service Unavailable\r\n"
          "Content-Type: application/json; charset=utf-8\r\n"
          "Content-Length: 80\r\n"
          "X-Content-Type-Options: nosniff\r\n"
          "Connection: close\r\n\r\n"
          "{\"error\":{\"status\":503,\"code\":\"conflict\",\"message\":\"too "
          "many connections\"}}\n";
      ssize_t ignored = send(fd, kBusy, sizeof kBusy - 1, MSG_NOSIGNAL);
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
    kbc_str_init(&c->in);
    kbc_str_init(&c->out);
    pthread_mutex_init(&c->mu, NULL);
    c->arena = kbc_arena_new(8192);
    if (c->arena == NULL || !httpd_track(h, c)) {
      if (c->arena != NULL) kbc_arena_free(c->arena);
      pthread_mutex_destroy(&c->mu);
      kbc_str_free(&c->in);
      kbc_str_free(&c->out);
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
  h->bind_addr = strdup(cfg->bind_addr != NULL ? cfg->bind_addr : "127.0.0.1");
  h->listen_fd = calloc(want, sizeof *h->listen_fd);
  h->w = calloc(want, sizeof *h->w);
  h->threads = calloc(want, sizeof *h->threads);
  h->wake_rd = -1;
  h->wake_wr = -1;
  if (h->bind_addr == NULL || h->listen_fd == NULL || h->w == NULL ||
      h->threads == NULL) {
    (void)kbc_err_set(err, KBC_ERR_NOMEM, "kbc_httpd_start: worker arrays");
    kbc_httpd_stop(h);
    return NULL;
  }
  for (size_t i = 0; i < want; i++) {
    h->listen_fd[i] = -1;
    h->w[i].epfd = -1;
    h->w[i].lfd = -1;
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
  atomic_init(&h->conns, 0);
  atomic_init(&h->stopping, false);
  h->started_ns = kbc_now_ns();
  pthread_mutex_init(&h->conns_mu, NULL);
  pthread_mutex_init(&h->ring_mu, NULL);

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
  free(h->bind_addr);
  free(h->listen_fd);
  free(h->w);
  free(h->threads);
  pthread_mutex_destroy(&h->conns_mu);
  pthread_mutex_destroy(&h->ring_mu);
  free(h);
}

int kbc_httpd_port(const kbc_httpd *h) { return h != NULL ? h->port : 0; }

/* ------------------------------------------------------ (5) route export -- */

const kbc_route KBC_ROUTES[] = {
    {"GET", "/api/health", "liveness, version, uptime, indexed doc count",
     false},
    {"GET", "/api/stats", "daemon counters as JSON", true},
    {"GET", "/api/search", "search, ?q=&kb=&kind=&mode=&limit=&offset=", true},
    {"GET", "/api/artifacts", "list artifacts, ?kb=&kind=&limit=&offset=",
     true},
    {"GET", "/api/artifacts/{id}", "one artifact, ?source=1 adds the raw text",
     true},
    {"POST", "/api/reindex", "synchronous full rescan, answers 202", true},
    {"GET", "/api/events", "server-sent event stream", true},
    {"GET", "/", "plain-text API banner (no reader UI in this port)", false},
};
const size_t KBC_ROUTES_LEN = sizeof KBC_ROUTES / sizeof KBC_ROUTES[0];
