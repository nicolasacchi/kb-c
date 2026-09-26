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
  const char *client_addr;
} kbc_request;

typedef struct {
  int status;             /* HTTP status code */
  const char *content_type;
  kbc_str body;           /* KBC_OWN, moved out to the socket */
  bool sse;               /* stream until the client disconnects */
  bool close_after;       /* send Connection: close */
} kbc_response;

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

extern const kbc_route KBC_ROUTES[];
extern const size_t KBC_ROUTES_LEN;

#ifdef __cplusplus
}
#endif

#endif /* KBC_HTTPD_H */
