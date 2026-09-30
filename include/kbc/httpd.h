/* httpd.h — the daemon's HTTP surface: epoll, one worker per core slice, and
 * the route table.
 *
 * Concurrency: http_workers threads, each with its own epoll set, all bound to
 * the same listening socket via SO_REUSEPORT — the kernel load-balances
 * accepts, there is no shared accept lock. Each connection is handled start to
 * finish on the thread that accepted it, so a request never needs a handoff.
 * SSE responses are the one exception: they write until the client goes away,
 * which is why a daemon must not run one worker.
 *
 * Safety: binds 127.0.0.1 by default and REFUSES a non-loopback bind without a
 * token (kbc_config_bind_is_safe). A token, when set, is required on every
 * /api route except /api/health.
 */
#ifndef KBC_HTTPD_H
#define KBC_HTTPD_H

#include "kbc/app.h"
#include "kbc/kbc.h"
#include "kbc/mem.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct kbc_httpd kbc_httpd;

typedef struct {
  const char *method;  /* borrowed, uppercase */
  const char *path;    /* borrowed, query string stripped, '/' -normalized */
  const char *query;   /* borrowed, raw, "" when absent */
  const char *body;    /* BORROWED, NUL-terminated by the server */
  size_t body_len;
  const char *auth;    /* borrowed Authorization header value, "" if absent */
  const char *x_kb_token; /* borrowed X-Kb-Token value, "" when absent; the
                           * second auth carrier beside Authorization. The
                           * ladder gives Authorization PRECEDENCE and does
                           * not require the two to agree, matching the
                           * original; `/api/identity` reports which one was
                           * used, because a silent preference is invisible
                           * on the wire. */
  /* Borrowed header values the socketless seam could not otherwise see.
   * Without them `kbc_httpd_handle` cannot drive a multipart route at all —
   * there is no boundary, so it is a 400 — which makes every test of such a
   * route a socket test whether it wanted to be one or not. Empty when the
   * header is absent, like `auth` above. */
  const char *content_type;     /* "" when absent */
  const char *x_requested_by;  /* "" when absent; the capture route's `from` */
  const char *client_addr;
} kbc_request;

/* One response header beyond Content-Type. BORROWED, like `content_type`
 * above: the name and value must outlive the write, and a handler that
 * formats one into a local must keep that local alive. */
typedef struct {
  const char *name;
  const char *value;
} kbc_header;

/* Eight is not a guess: it is what the routes need at once. The artifact
 * route sets Content-Disposition and Cache-Control; an attachment adds
 * X-Content-Type-Options; the subdomain adds CSP; a registered handler can
 * add its own. A handler that needs a ninth gets a refusal naming the count,
 * not a truncated response with a missing security header on it. */
#define KBC_RESPONSE_MAX_HEADERS 8

typedef struct {
  int status;             /* HTTP status code */
  const char *content_type;
  kbc_str body;           /* KBC_OWN, moved out to the socket */
  bool sse;               /* stream until the client disconnects */
  bool close_after;       /* send Connection: close */
  kbc_header headers[KBC_RESPONSE_MAX_HEADERS];
  size_t n_headers;
} kbc_response;

/* Adds a response header, replacing any existing one with the same name
 * (case-insensitively, as HTTP requires). KBC_ERR_INVALID past the bound,
 * with the count in the message.
 *
 * THIS EXISTS because the alternative was a thread-local side channel that
 * a registered handler wrote to and `dispatch` copied out. A hidden channel
 * between a handler and the write path is a bug waiting for a better home,
 * and the header that needed it was `Content-Disposition` — which for an
 * attachment is the XSS guard, not a nicety. A security control must not
 * travel by a route the type system does not describe. */
kbc_status kbc_response_header(kbc_response *r, const char *name,
                               const char *value);

void kbc_response_init(kbc_response *r);
void kbc_response_free(kbc_response *r);
kbc_status kbc_response_json(kbc_response *r, int status, const char *json);
kbc_status kbc_response_error_json(kbc_response *r, int status, kbc_status code,
                                   const char *msg);

/* Binds and serves until kbc_httpd_stop. Returns KBC_ERR_CONFLICT when the
 * address is taken, KBC_ERR_INVALID for a non-loopback bind with no token. */
kbc_httpd *kbc_httpd_start(kbc_app *app, const kbc_config *cfg, kbc_err *err);
void kbc_httpd_stop(kbc_httpd *h);
int kbc_httpd_port(const kbc_httpd *h);

/* Handles one already-parsed request. Exposed so the integration tests can
 * drive routing without a socket — same code path, no TCP. */
kbc_status kbc_httpd_handle(kbc_app *app, const kbc_config *cfg,
                            const kbc_request *req, kbc_response *out,
                            kbc_err *err);

/* The route table, for docs and tests. */
typedef struct {
  const char *method;
  const char *path;
  const char *summary;
  bool needs_auth;
} kbc_route;

/* REGISTRATION, which is why the table is no longer one array in httpd.c.
 *
 * A single `KBC_ROUTES[]` in the daemon's own file is a contention point: the
 * twenty-odd route rows still to build are independent features, and every one
 * of them wants to append a line to the same array in the same file, so they
 * cannot be built in parallel at all. Here each subsystem OWNS its routes —
 * the file that implements the feature declares them and registers them — and
 * the daemon never edits for a feature it does not implement.
 *
 * `handler` is the route's implementation. It is NULL for the handful of
 * routes that are still served by the daemon's own switch, which is what makes
 * this additive: a subsystem registers with a handler and the switch stops
 * growing, and nothing has to be rewritten to get there.
 *
 * A handler receives its path parameters ALREADY SPLIT, because the obvious
 * alternative — making every handler re-derive `/api/kb/{kb}/thing/{id}` from
 * `req->path` by hand — is a chance to get the index wrong per route, and
 * there are twenty of them landing here.
 *
 * Matching follows matchit 0.8.4, the router behind axum 0.8, and the
 * distinction is the one every implementer gets wrong: `{name}` is exactly
 * ONE non-empty segment and never spans a `/`; `{*name}` is the catch-all,
 * spans a remainder, and is legal only as the final segment (matchit refuses
 * it anywhere else, InvalidCatchAll). The original uses both —
 * `/kb/{kb}/docs/by-path/{*path}` at router.rs:121.
 */
#define KBC_ROUTE_MAX_PARAMS 8

typedef struct {
  const char *name;  /* the literal text between the braces, NUL-terminated */
  const char *value; /* NUL-terminated */
  size_t len;
} kbc_route_param;
/* LIFETIME, and it is sharper than it looks. `name` AND `value` are both
 * NUL-terminated, but NEITHER is a borrow of the template: a template's name
 * is followed by `}` (dispatch copies it out precisely because a borrow would
 * make `strcmp(name, "kb")` read `"kb}/thing/{id}"`). Both point into a
 * scratch buffer owned by the caller and are valid only until the handler
 * returns. Nothing in the type system stops a handler from storing a pointer
 * past that, and an ASan stack-use-after-return is what it looks like. DO NOT
 * RETAIN either string; deep-copy if a parameter must outlive the call.
 *
 * A catch-all's bound name DROPS the `*`: `{*path}` binds the name `"path"`.
 * Both readings are defensible from the syntax; this is the useful one, so it
 * is pinned by a test rather than left to each author's guess.
 *
 * Params are bound per CANDIDATE route, not once per scan — otherwise a route
 * that matched two segments and failed on the third leaves its values behind
 * for the next candidate to inherit, and the winner gets another route's
 * parameters.
 */

typedef struct {
  kbc_route_param v[KBC_ROUTE_MAX_PARAMS]; /* fixed: a request path allocates
                                             * nothing to pass its own
                                             * parameters */
  size_t n;
} kbc_route_params;

typedef kbc_status (*kbc_route_handler)(kbc_app *app, const kbc_request *req,
                                         const kbc_route_params *params,
                                         kbc_response *out, kbc_err *err);

typedef struct {
  const char *method;
  const char *path;   /* template, with {params} */
  const char *summary;
  bool needs_auth;
  kbc_route_handler handler; /* NULL: still served by the daemon's switch */
} kbc_route_entry;

/* Registers `n` routes. Called from a subsystem's initialiser, before
 * kbc_httpd_start, and NOT from a request path: the registry is fixed once
 * start-up is over, which is what lets the dispatcher read it without a lock.
 * Re-registering a (method, path) pair is KBC_ERR_INVALID rather than a
 * silent duplicate, because two handlers claiming one route is a bug that would
 * otherwise be decided by array order. */
kbc_status kbc_httpd_routes_add(const kbc_route_entry *routes, size_t n,
                                kbc_err *err);

/* Every registered route, in registration order, for docs and tests. Derived
 * from the registry rather than maintained by hand, so it cannot disagree with
 * what the daemon actually serves.
 *
 * KBC_ROUTES and KBC_ROUTES_LEN remain, unchanged and pointing at the SAME
 * storage, so nothing that reads the table has to change in the same commit
 * that introduces the registry. There is one array, not two that can drift. */
const kbc_route *kbc_httpd_routes(size_t *n_out);
extern const kbc_route *const KBC_ROUTES;
extern const size_t KBC_ROUTES_LEN;

#ifdef __cplusplus
}
#endif

#endif /* KBC_HTTPD_H */
