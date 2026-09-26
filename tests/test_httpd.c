/* test_httpd.c — src/httpd.c: routing, the token gate, the bind guard, query
 * parsing and the request parser's refusal to be lied to.
 *
 * Two seams are used. kbc_httpd_handle is driven directly for routing and auth
 * (no socket, no threads). The request PARSER lives in conn_process, which is
 * static and only reachable from a connection, so the hostile-request cases go
 * over a real loopback socket to a one-worker daemon: that is the only place
 * where request-line, header-table and Content-Length rules actually run. */
#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include "kbc/app.h"
#include "kbc/config.h"
#include "kbc/httpd.h"
#include "kbc/mem.h"
#include "kbc_test.h"

#define TOKEN "s3cr3t-token"

/* ------------------------------------------------------------------ fixture */

typedef struct {
  char root[KBC_TEST_PATH_MAX];
  kbc_config *cfg;
  kbc_app *app;
  bool reindexed;
} fixture;

static kbc_config *make_cfg(const char *root, const char *token) {
  kbc_config *cfg = kbc_config_defaults();
  if (cfg == NULL) return NULL;
  free(cfg->data_dir);
  free(cfg->db_path);
  free(cfg->index_path);
  free(cfg->token_path);
  char buf[KBC_TEST_PATH_MAX + 64];
  snprintf(buf, sizeof buf, "%s/data", root);
  cfg->data_dir = strdup(buf);
  snprintf(buf, sizeof buf, "%s/data/kb.db", root);
  cfg->db_path = strdup(buf);
  snprintf(buf, sizeof buf, "%s/data/index", root);
  cfg->index_path = strdup(buf);
  snprintf(buf, sizeof buf, "%s/data/token", root);
  cfg->token_path = strdup(buf);
  if (token != NULL) {
    free(cfg->token);
    cfg->token = strdup(token);
  }
  cfg->corpora = (kbc_corpus_cfg *)calloc(1, sizeof *cfg->corpora);
  if (cfg->corpora == NULL) {
    kbc_config_free(cfg);
    return NULL;
  }
  cfg->ncorpora = 1;
  snprintf(buf, sizeof buf, "%s/kb", root);
  cfg->corpora[0].name = strdup("kb");
  cfg->corpora[0].path = strdup(buf);
  kbc_strlist_init(&cfg->corpora[0].ignore);
  if (cfg->data_dir == NULL || cfg->db_path == NULL || cfg->index_path == NULL ||
      cfg->token_path == NULL || cfg->corpora[0].name == NULL ||
      cfg->corpora[0].path == NULL) {
    kbc_config_free(cfg);
    return NULL;
  }
  return cfg;
}

static void fx_setup(fixture *f, const char *token) {
  memset(f, 0, sizeof *f);
  kbc_test_tmpdir(f->root, sizeof f->root);
  char sub[KBC_TEST_PATH_MAX + 64];
  snprintf(sub, sizeof sub, "%s/kb", f->root);
  kbc_test_mkdir_p(sub);
  snprintf(sub, sizeof sub, "%s/kb/alpha.md", f->root);
  /* Two documents with disjoint vocabularies, so "alpha" and "bravo" are each
   * found in exactly one file and a result count means what it says. */
  kbc_test_write_file(sub, "# Alpha\n\nalpha zebra\n");
  snprintf(sub, sizeof sub, "%s/kb/bravo.md", f->root);
  kbc_test_write_file(sub, "# Bravo\n\nbravo yak\n");

  f->cfg = make_cfg(f->root, token);
  KBC_CHECK_NOT_NULL(f->cfg);
  if (f->cfg == NULL) return;
  kbc_err err;
  kbc_err_reset(&err);
  f->app = kbc_app_open(f->cfg, &err);
  if (f->app == NULL) fprintf(stderr, "  app_open: %s\n", err.msg);
  KBC_CHECK_NOT_NULL(f->app);
  if (f->app == NULL) return;
  kbc_err_reset(&err);
  kbc_status st = kbc_app_reindex(f->app, &err);
  if (kbc_failed(st)) fprintf(stderr, "  reindex: %s\n", err.msg);
  KBC_CHECK_OK(st);
  f->reindexed = !kbc_failed(st);
}

static void fx_teardown(fixture *f) {
  kbc_app_close(f->app);
  kbc_config_free(f->cfg);
  kbc_test_rmrf(f->root);
}

/* ------------------------------------------------------------------ helpers */

static int call(fixture *f, const char *method, const char *path,
                const char *query, const char *auth, kbc_response *out) {
  kbc_request req;
  memset(&req, 0, sizeof req);
  req.method = method;
  req.path = path;
  req.query = query != NULL ? query : "";
  req.body = "";
  req.body_len = 0;
  req.auth = auth != NULL ? auth : "";
  req.client_addr = "test";
  kbc_err err;
  kbc_err_reset(&err);
  kbc_status st = kbc_httpd_handle(f->app, f->cfg, &req, out, &err);
  KBC_CHECK_MSG(!kbc_failed(st), "kbc_httpd_handle failed: %s", err.msg);
  return out->status;
}

/* Extracts the first "id":"..." value from a JSON body. */
static bool first_id(const kbc_response *r, char *buf, size_t cap) {
  const char *p = strstr(r->body.ptr, "\"id\":\"");
  if (p == NULL) return false;
  p += 6;
  size_t i = 0;
  while (p[i] != '\0' && p[i] != '"' && i + 1 < cap) {
    buf[i] = p[i];
    i++;
  }
  buf[i] = '\0';
  return i > 0;
}


/* Every request that must actually return rows passes offset=0 explicitly.
 * See absent_offset_is_zero — omitting it is currently a bug, and a test that
 * hid behind that bug would prove nothing about the rows it is checking. */
#define AT0 "offset=0"
/* -------------------------------------------------------------- routing ---- */

KBC_TEST(routing_status_codes) {
  fixture f;
  fx_setup(&f, NULL);
  kbc_response r;

  KBC_CHECK_EQ_INT(call(&f, "GET", "/api/health", NULL, NULL, &r), 200);
  KBC_CHECK_NOT_NULL(strstr(r.body.ptr, "\"status\":\"ok\""));
  KBC_CHECK_EQ_STR(r.content_type, "application/json; charset=utf-8");
  kbc_response_free(&r);

  KBC_CHECK_EQ_INT(call(&f, "GET", "/api/search", "q=alpha&" AT0, NULL, &r), 200);
  KBC_CHECK_NOT_NULL(strstr(r.body.ptr, "\"results\":["));
  KBC_CHECK_NOT_NULL(strstr(r.body.ptr, "\"count\":1"));
  kbc_response_free(&r);

  KBC_CHECK_EQ_INT(call(&f, "GET", "/api/artifacts", AT0, NULL, &r), 200);
  KBC_CHECK_NOT_NULL(strstr(r.body.ptr, "\"artifacts\":["));
  KBC_CHECK_NOT_NULL(strstr(r.body.ptr, "\"total\":2"));
  kbc_response_free(&r);

  KBC_CHECK_EQ_INT(call(&f, "POST", "/api/reindex", NULL, NULL, &r), 202);
  kbc_response_free(&r);

  /* An unknown path and an unknown method on a known path are different
   * failures: one means "no such resource", the other "wrong verb here". */
  KBC_CHECK_EQ_INT(call(&f, "GET", "/api/nope", NULL, NULL, &r), 404);
  kbc_response_free(&r);
  KBC_CHECK_EQ_INT(call(&f, "GET", "/api/reindex", NULL, NULL, &r), 405);
  kbc_response_free(&r);
  KBC_CHECK_EQ_INT(call(&f, "POST", "/api/artifacts", NULL, NULL, &r), 405);
  kbc_response_free(&r);
  KBC_CHECK_EQ_INT(call(&f, "PUT", "/api/search", "q=alpha&" AT0, NULL, &r), 405);
  kbc_response_free(&r);

  fx_teardown(&f);
}

KBC_TEST(artifact_by_id_found_and_missing) {
  fixture f;
  fx_setup(&f, NULL);
  kbc_response r;
  char id[KBC_MAX_ID_LEN + 1];

  KBC_CHECK_EQ_INT(call(&f, "GET", "/api/artifacts", AT0, NULL, &r), 200);
  bool got = first_id(&r, id, sizeof id);
  kbc_response_free(&r);
  KBC_CHECK_MSG(got, "no id in the artifact list");
  if (!got) {
    fx_teardown(&f);
    return;
  }
  KBC_CHECK_EQ_INT(strlen(id), KBC_MAX_ID_LEN);
  KBC_CHECK(kbc_id_is_valid(id));

  char path[64];
  snprintf(path, sizeof path, "/api/artifacts/%s", id);
  KBC_CHECK_EQ_INT(call(&f, "GET", path, NULL, NULL, &r), 200);
  {
    char pat[64];
    snprintf(pat, sizeof pat, "\"id\":\"%s\"", id);
    KBC_CHECK_MSG(strstr(r.body.ptr, pat) != NULL,
                  "200 body does not carry the requested id: %s", r.body.ptr);
  }
  kbc_response_free(&r);

  /* Well-formed but absent. */
  const char *miss = "000000000000";
  KBC_CHECK_MSG(kbc_id_is_valid(miss), "the miss id must be well formed");
  snprintf(path, sizeof path, "/api/artifacts/%s", miss);
  KBC_CHECK_EQ_INT(call(&f, "GET", path, NULL, NULL, &r), 404);
  kbc_response_free(&r);

  /* Malformed id: rejected on shape, not looked up. */
  KBC_CHECK_EQ_INT(call(&f, "GET", "/api/artifacts/xyz", NULL, NULL, &r), 400);
  kbc_response_free(&r);
  KBC_CHECK_EQ_INT(
      call(&f, "GET", "/api/artifacts/ABCDEF012345", NULL, NULL, &r), 400);
  kbc_response_free(&r);
  /* A slash in the id position is a different route, not a lookup. */
  KBC_CHECK_EQ_INT(
      call(&f, "GET", "/api/artifacts/aa/bb", NULL, NULL, &r), 404);
  kbc_response_free(&r);

  fx_teardown(&f);
}

KBC_TEST(handle_rejects_incomplete_request) {
  fixture f;
  fx_setup(&f, NULL);
  kbc_err err;
  kbc_response r;
  kbc_response_init(&r);
  kbc_err_reset(&err);
  KBC_CHECK_ERR(kbc_httpd_handle(f.app, f.cfg, NULL, &r, &err), KBC_ERR_INVALID);
  KBC_CHECK_ERR_MSG(err);
  kbc_response_free(&r);

  kbc_request req;
  memset(&req, 0, sizeof req);
  req.path = "/api/health";
  kbc_response_init(&r);
  kbc_err_reset(&err);
  KBC_CHECK_OK(kbc_httpd_handle(f.app, f.cfg, &req, &r, &err));
  KBC_CHECK_EQ_INT(r.status, 400);
  kbc_response_free(&r);

  fx_teardown(&f);
}

/* ------------------------------------------------------------------- auth -- */

KBC_TEST(token_gate_is_exhaustive) {
  fixture f;
  fx_setup(&f, TOKEN);
  kbc_response r;

  /* Every /api route but health is closed. */
  static const char *const guarded[] = {"/api/search", "/api/artifacts",
                                        "/api/stats", "/api/events"};
  for (size_t i = 0; i < sizeof guarded / sizeof *guarded; i++) {
    const char *q = strcmp(guarded[i], "/api/search") == 0 ? "q=alpha" : NULL;
    KBC_CHECK_MSG(call(&f, "GET", guarded[i], q, NULL, &r) == 401,
                  "GET %s without a token must be 401", guarded[i]);
    kbc_response_free(&r);
  }
  KBC_CHECK_EQ_INT(call(&f, "POST", "/api/reindex", NULL, NULL, &r), 401);
  kbc_response_free(&r);

  /* Malformed Authorization: a header that is not "Bearer <token>" at all is
   * 401 — the daemon cannot even tell who is asking. */
  KBC_CHECK_EQ_INT(
      call(&f, "GET", "/api/search", "q=alpha", TOKEN, &r), 401);
  kbc_response_free(&r);
  KBC_CHECK_EQ_INT(
      call(&f, "GET", "/api/search", "q=alpha", "Basic " TOKEN, &r), 401);
  kbc_response_free(&r);
  KBC_CHECK_EQ_INT(
      call(&f, "GET", "/api/search", "q=alpha", "bearer x", &r), 401);
  kbc_response_free(&r);
  /* An empty Authorization header is no header at all. */
  KBC_CHECK_EQ_INT(call(&f, "GET", "/api/search", "q=alpha", "", &r), 401);
  kbc_response_free(&r);

  /* Well-formed, wrong: 403, so a client can tell "who are you" from "no". An
   * EMPTY token is well-formed and wrong too, not a missing header. */
  KBC_CHECK_EQ_INT(
      call(&f, "GET", "/api/search", "q=alpha", "Bearer wrong", &r), 403);
  kbc_response_free(&r);
  KBC_CHECK_EQ_INT(call(&f, "GET", "/api/search", "q=alpha", "Bearer ", &r), 403);
  kbc_response_free(&r);
  KBC_CHECK_EQ_INT(
      call(&f, "GET", "/api/search", "q=alpha", "Bearer s3cr3t-toke", &r), 403);
  kbc_response_free(&r);

  char good[64];
  snprintf(good, sizeof good, "Bearer %s", TOKEN);
  KBC_CHECK_EQ_INT(call(&f, "GET", "/api/search", "q=alpha&" AT0, good, &r), 200);
  kbc_response_free(&r);
  KBC_CHECK_EQ_INT(call(&f, "GET", "/api/artifacts", AT0, good, &r), 200);
  kbc_response_free(&r);
  KBC_CHECK_EQ_INT(call(&f, "POST", "/api/reindex", NULL, good, &r), 202);
  kbc_response_free(&r);

  /* Health stays reachable: a liveness probe has no token to present. */
  KBC_CHECK_EQ_INT(call(&f, "GET", "/api/health", NULL, NULL, &r), 200);
  kbc_response_free(&r);
  /* ...and the wrong token is still a 403 there is no way around: health is
   * reachable, but a bad credential is not silently upgraded to an open door. */
  KBC_CHECK_EQ_INT(call(&f, "GET", "/api/artifacts", AT0, "Bearer wrong", &r),
                   403);
  kbc_response_free(&r);

  fx_teardown(&f);
}

KBC_TEST(no_token_configured_means_open) {
  fixture f;
  fx_setup(&f, NULL);
  kbc_response r;
  KBC_CHECK_EQ_INT(call(&f, "GET", "/api/search", "q=alpha&" AT0, NULL, &r), 200);
  kbc_response_free(&r);
  fx_teardown(&f);
}

/* ------------------------------------------------------------- bind guard -- */

KBC_TEST(non_loopback_bind_without_token_is_refused) {
  fixture f;
  fx_setup(&f, NULL);

  char why[128];
  /* Loopback with no token is the safe default, and must stay legal. */
  KBC_CHECK_MSG(kbc_config_bind_is_safe(f.cfg, why, sizeof why),
                "127.0.0.1 with no token must be allowed");

  /* Anything routable without a token is not. */
  free(f.cfg->bind_addr);
  f.cfg->bind_addr = strdup("0.0.0.0");
  KBC_CHECK(f.cfg->bind_addr != NULL);
  KBC_CHECK(!kbc_config_bind_is_safe(f.cfg, why, sizeof why));
  KBC_CHECK_MSG(strstr(why, "token") != NULL,
                "refusal message must name the remedy, got: %s", why);

  kbc_err err;

  kbc_err_reset(&err);
  kbc_httpd *h = kbc_httpd_start(f.app, f.cfg, &err);
  KBC_CHECK_NULL(h);
  KBC_CHECK_ERR(err.status, KBC_ERR_INVALID);
  KBC_CHECK_ERR_MSG(err);
  KBC_CHECK_MSG(strstr(err.msg, "0.0.0.0") != NULL,
                "message must name the refused address, got: %s", err.msg);
  KBC_CHECK_MSG(strstr(err.msg, "token") != NULL,
                "message must name the remedy, got: %s", err.msg);

  /* A routable bind WITH a token is allowed, so the guard is about the token
   * and not about the address. */
  free(f.cfg->token);
  f.cfg->token = strdup(TOKEN);
  KBC_CHECK(f.cfg->token != NULL);
  KBC_CHECK(kbc_config_bind_is_safe(f.cfg, why, sizeof why));

  fx_teardown(&f);
}

/* --------------------------------------------------------- query parsing --- */

KBC_TEST(query_parsing_rejects_instead_of_defaulting) {
  fixture f;
  fx_setup(&f, NULL);
  kbc_response r;

  KBC_CHECK_EQ_INT(
      call(&f, "GET", "/api/search", "q=alpha&mode=telepathy", NULL, &r), 400);
  kbc_response_free(&r);
  KBC_CHECK_EQ_INT(
      call(&f, "GET", "/api/search", "q=alpha&kind=haiku", NULL, &r), 400);
  kbc_response_free(&r);
  KBC_CHECK_EQ_INT(call(&f, "GET", "/api/artifacts", "kind=haiku", NULL, &r),
                   400);
  kbc_response_free(&r);

  /* A known kind and a known mode are accepted. */
  KBC_CHECK_EQ_INT(call(&f, "GET", "/api/search",
                        "q=alpha&kind=note&mode=keyword&" AT0, NULL, &r), 200);
  kbc_response_free(&r);

  /* q is required and must be non-empty. */
  KBC_CHECK_EQ_INT(call(&f, "GET", "/api/search", NULL, NULL, &r), 400);
  kbc_response_free(&r);
  KBC_CHECK_EQ_INT(call(&f, "GET", "/api/search", "q=", NULL, &r), 400);
  kbc_response_free(&r);

  /* limit/offset are integers, not suggestions. */
  KBC_CHECK_EQ_INT(call(&f, "GET", "/api/search", "q=alpha&limit=0", NULL, &r),
                   400);
  kbc_response_free(&r);
  KBC_CHECK_EQ_INT(
      call(&f, "GET", "/api/search", "q=alpha&limit=many", NULL, &r), 400);
  kbc_response_free(&r);
  KBC_CHECK_EQ_INT(
      call(&f, "GET", "/api/search", "q=alpha&offset=-1", NULL, &r), 400);
  kbc_response_free(&r);

  /* In range but above the ceiling: clamped, and the body says so. */
  KBC_CHECK_EQ_INT(
      call(&f, "GET", "/api/search", "q=alpha&limit=99999&" AT0, NULL, &r), 200);
  {
    char pat[64];
    snprintf(pat, sizeof pat, "\"limit\":%u", KBC_MAX_HITS);
    KBC_CHECK_MSG(strstr(r.body.ptr, pat) != NULL,
                  "limit was not clamped to %u: %s", KBC_MAX_HITS, r.body.ptr);
  }
  kbc_response_free(&r);

  /* offset past the end is an empty page, not an error. */
  KBC_CHECK_EQ_INT(
      call(&f, "GET", "/api/search", "q=alpha&offset=50", NULL, &r), 200);
  KBC_CHECK_NOT_NULL(strstr(r.body.ptr, "\"count\":0"));
  kbc_response_free(&r);

  fx_teardown(&f);
}

KBC_TEST(query_is_percent_decoded_before_search) {
  fixture f;
  fx_setup(&f, NULL);
  kbc_response r;

  /* "%20bravo" decoded to " bravo" tokenizes to "bravo" and finds its one
   * document. Left undecoded it would be the single term "20bravo" and find
   * nothing, so the count is the observation. */
  KBC_CHECK_EQ_INT(call(&f, "GET", "/api/search", "q=%20bravo&" AT0, NULL, &r),
                   200);
  KBC_CHECK_MSG(strstr(r.body.ptr, "\"count\":1") != NULL,
                "percent-decoded q did not reach the search: %s", r.body.ptr);
  kbc_response_free(&r);

  /* A NUL arriving through an escape is refused, not silently truncated. */
  KBC_CHECK_EQ_INT(call(&f, "GET", "/api/search", "q=al%00pha", NULL, &r), 400);
  kbc_response_free(&r);
  /* A truncated or non-hex escape is a parse error too. */
  KBC_CHECK_EQ_INT(call(&f, "GET", "/api/search", "q=al%2", NULL, &r), 400);
  kbc_response_free(&r);
  KBC_CHECK_EQ_INT(call(&f, "GET", "/api/search", "q=al%zz", NULL, &r), 400);
  kbc_response_free(&r);
  /* q longer than the ceiling is rejected, not truncated. */
  {
    kbc_str big;
    kbc_str_init(&big);
    (void)kbc_str_puts(&big, "q=");
    for (size_t i = 0; i < KBC_MAX_QUERY_LEN + 1; i++) (void)kbc_str_putc(&big, 'a');
    KBC_CHECK_EQ_INT(call(&f, "GET", "/api/search", big.ptr, NULL, &r), 400);
    kbc_response_free(&r);
    kbc_str_free(&big);
  }

  fx_teardown(&f);
}

KBC_TEST(duplicate_q_takes_the_first) {
  fixture f;
  fx_setup(&f, NULL);
  kbc_response r;
  KBC_CHECK_EQ_INT(
      call(&f, "GET", "/api/search", "q=alpha&q=bravo&" AT0, NULL, &r), 200);
  KBC_CHECK_MSG(strstr(r.body.ptr, "\"count\":1") != NULL,
                "expected the first q to win: %s", r.body.ptr);
  KBC_CHECK_MSG(strstr(r.body.ptr, "alpha.md") != NULL,
                "expected alpha.md, got: %s", r.body.ptr);
  KBC_CHECK_MSG(strstr(r.body.ptr, "bravo.md") == NULL,
                "bravo.md leaked in despite first-wins: %s", r.body.ptr);
  kbc_response_free(&r);
  fx_teardown(&f);
}

KBC_TEST(absent_offset_is_zero) {
  fixture f;
  fx_setup(&f, NULL);
  kbc_response r;

  /* A client that says nothing about offset means the first page. The body
   * echoes the window it used, so "offset":0 in the reply is the contract. */
  KBC_CHECK_EQ_INT(call(&f, "GET", "/api/search", "q=alpha", NULL, &r), 200);
  KBC_CHECK_MSG(strstr(r.body.ptr, "\"offset\":0") != NULL,
                "an absent offset must default to 0: %s", r.body.ptr);
  KBC_CHECK_MSG(strstr(r.body.ptr, "\"count\":1") != NULL,
                "the first page must hold the one match: %s", r.body.ptr);
  kbc_response_free(&r);

  KBC_CHECK_EQ_INT(call(&f, "GET", "/api/artifacts", NULL, NULL, &r), 200);
  KBC_CHECK_MSG(strstr(r.body.ptr, "\"offset\":0") != NULL,
                "listing without offset must default to 0: %s", r.body.ptr);
  kbc_response_free(&r);

  fx_teardown(&f);
}

/* ------------------------------------------------- hostile input over TCP ---- */

typedef struct {
  int port;
  kbc_httpd *h;
} server;

/* Sends `raw` verbatim and reads the whole reply. The daemon answers a refused
 * request with Connection: close, so read-to-EOF is bounded. */
static bool raw_exchange(server *s, const char *raw, size_t n, int *status,
                         char *reply, size_t cap) {
  *status = 0;
  reply[0] = '\0';
  int fd = socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) return false;
  struct sockaddr_in a;
  memset(&a, 0, sizeof a);
  a.sin_family = AF_INET;
  a.sin_port = htons((uint16_t)s->port);
  a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  if (connect(fd, (struct sockaddr *)&a, sizeof a) != 0) {
    close(fd);
    return false;
  }
  size_t off = 0;
  while (off < n) {
    ssize_t w = send(fd, raw + off, n - off, MSG_NOSIGNAL);
    if (w <= 0) break;
    off += (size_t)w;
  }
  shutdown(fd, SHUT_WR);
  struct timespec tv = {5, 0};
  (void)setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
  size_t got = 0;
  for (;;) {
    if (got + 1 >= cap) break;
    ssize_t r = recv(fd, reply + got, cap - 1 - got, 0);
    if (r <= 0) break;
    got += (size_t)r;
    reply[got] = '\0';
  }
  close(fd);
  if (strncmp(reply, "HTTP/1.1 ", 9) != 0) return false;
  *status = atoi(reply + 9);
  return true;
}

static void srv_start(server *s, fixture *f) {
  memset(s, 0, sizeof *s);
  free(f->cfg->bind_addr);
  f->cfg->bind_addr = strdup("127.0.0.1");
  f->cfg->port = 0; /* let the kernel pick, so the suite never clashes */
  f->cfg->http_workers = 1;
  kbc_err err;
  kbc_err_reset(&err);
  s->h = kbc_httpd_start(f->app, f->cfg, &err);
  if (s->h == NULL) fprintf(stderr, "  httpd_start: %s\n", err.msg);
  KBC_CHECK_NOT_NULL(s->h);
  s->port = s->h != NULL ? kbc_httpd_port(s->h) : 0;
  KBC_CHECK_MSG(s->port > 0, "no listening port");
}

static void srv_stop(server *s) {
  kbc_httpd_stop(s->h);
  s->h = NULL;
}

static void expect_4xx(server *s, const char *raw, size_t n, const char *what) {
  char reply[8192];
  int status = 0;
  bool ok = raw_exchange(s, raw, n, &status, reply, sizeof reply);
  KBC_CHECK_MSG(ok, "%s: no HTTP reply", what);
  if (!ok) return;
  KBC_CHECK_MSG(status >= 400 && status < 500, "%s: got status %d (%s)", what,
                status, reply);
  /* Every answer, refusal included, must be a well-formed HTTP/1.1 response. */
  KBC_CHECK_MSG(strstr(reply, "\r\n\r\n") != NULL, "%s: no header terminator",
                what);
}

KBC_TEST(hostile_requests_are_refused) {
  fixture f;
  fx_setup(&f, NULL);
  server s;
  srv_start(&s, &f);
  if (s.h == NULL) {
    fx_teardown(&f);
    return;
  }

  /* A ".." segment never reaches the router. */
  expect_4xx(&s, "GET /api/../etc/passwd HTTP/1.1\r\nHost: x\r\n\r\n",
             strlen("GET /api/../etc/passwd HTTP/1.1\r\nHost: x\r\n\r\n"),
             "dot-dot path");
  {
    const char *r = "GET /api/artifacts/..%2f..%2fetc%2fpasswd HTTP/1.1\r\n\r\n";
    expect_4xx(&s, r, strlen(r), "encoded dot-dot");
  }
  /* An encoded NUL in the path is refused rather than truncating the path. */
  {
    const char *r =
        "GET /api/artifacts/aaaaaaaaaaaa%00 HTTP/1.1\r\nHost: x\r\n\r\n";
    expect_4xx(&s, r, strlen(r), "encoded NUL in path");
  }
  /* A backslash is not a separator here and must not become one. */
  {
    const char *r = "GET /api\\artifacts HTTP/1.1\r\nHost: x\r\n\r\n";
    expect_4xx(&s, r, strlen(r), "backslash path");
  }
  /* NUL in the query is refused as well. */
  {
    const char *r = "GET /api/search?q=al%00pha HTTP/1.1\r\nHost: x\r\n\r\n";
    expect_4xx(&s, r, strlen(r), "encoded NUL in query");
  }

  /* Request line over KBC_HTTP_MAX_REQUEST_LINE. */
  {
    kbc_str req;
    kbc_str_init(&req);
    size_t pad = KBC_HTTP_MAX_REQUEST_LINE + 16u;
    (void)kbc_str_puts(&req, "GET /api/search?q=");
    for (size_t i = 0; i < pad; i++) (void)kbc_str_putc(&req, 'a');
    (void)kbc_str_puts(&req, " HTTP/1.1\r\nHost: x\r\n\r\n");
    expect_4xx(&s, req.ptr, req.len, "over-long request line");
    kbc_str_free(&req);
  }

  /* Header block over KBC_HTTP_MAX_HEADER_BYTES. */
  {
    kbc_str req;
    kbc_str_init(&req);
    (void)kbc_str_puts(&req, "GET /api/health HTTP/1.1\r\n");
    while (req.len < (size_t)KBC_HTTP_MAX_HEADER_BYTES + 1024u) {
      (void)kbc_str_puts(&req, "X-Pad: ");
      (void)kbc_str_putc(&req, 'p');
      (void)kbc_str_puts(&req, "\r\n");
    }
    (void)kbc_str_puts(&req, "\r\n");
    expect_4xx(&s, req.ptr, req.len, "over-long header block");
    kbc_str_free(&req);
  }

  /* Smuggling shapes. */
  {
    const char *r = "POST /api/reindex HTTP/1.1\r\nHost: x\r\n"
                    "Content-Length: 0\r\nContent-Length: 0\r\n\r\n";
    expect_4xx(&s, r, strlen(r), "duplicate Content-Length");
  }
  {
    const char *r = "POST /api/reindex HTTP/1.1\r\nHost: x\r\n"
                    "Transfer-Encoding: chunked\r\n\r\n";
    expect_4xx(&s, r, strlen(r), "Transfer-Encoding");
  }
  {
    const char *r =
        "POST /api/reindex HTTP/1.1\r\nHost: x\r\nContent-Length: 12x\r\n\r\n";
    expect_4xx(&s, r, strlen(r), "non-numeric Content-Length");
  }
  {
    const char *r =
        "POST /api/reindex HTTP/1.1\r\nHost: x\r\nContent-Length:\r\n\r\n";
    expect_4xx(&s, r, strlen(r), "empty Content-Length");
  }
  /* A declared body over the ceiling is refused before a byte is read. */
  {
    char r[128];
    snprintf(r, sizeof r,
             "POST /api/reindex HTTP/1.1\r\nHost: x\r\nContent-Length: %u\r\n\r\n",
             KBC_MAX_SNIFF_BYTES + 1u);
    expect_4xx(&s, r, strlen(r), "body over the sniff ceiling");
  }
  /* A malformed request line, and a method that is not uppercase. */
  {
    const char *r = "GET\r\n\r\n";
    expect_4xx(&s, r, strlen(r), "request line with no target");
  }
  {
    const char *r = "get /api/health HTTP/1.1\r\nHost: x\r\n\r\n";
    expect_4xx(&s, r, strlen(r), "lowercase method");
  }
  {
    const char *r = "GET /api/health HTTP/2.0\r\nHost: x\r\n\r\n";
    expect_4xx(&s, r, strlen(r), "wrong HTTP version");
  }
  /* A body that ends short of its Content-Length. */
  {
    const char *r = "POST /api/reindex HTTP/1.1\r\nHost: x\r\n"
                    "Content-Length: 10\r\n\r\nshort";
    expect_4xx(&s, r, strlen(r), "truncated body");
  }

  srv_stop(&s);
  fx_teardown(&f);
}

KBC_TEST(response_headers_are_set) {
  fixture f;
  fx_setup(&f, NULL);
  server s;
  srv_start(&s, &f);
  if (s.h == NULL) {
    fx_teardown(&f);
    return;
  }
  char reply[8192];
  int status = 0;
  const char *ok = "GET /api/health HTTP/1.1\r\nHost: x\r\n\r\n";
  KBC_CHECK(raw_exchange(&s, ok, strlen(ok), &status, reply, sizeof reply));
  KBC_CHECK_EQ_INT(status, 200);
  KBC_CHECK_MSG(strstr(reply, "Content-Type: application/json") != NULL,
                "no JSON Content-Type: %s", reply);
  KBC_CHECK_MSG(strstr(reply, "X-Content-Type-Options: nosniff") != NULL,
                "no nosniff on a 200: %s", reply);

  const char *nf = "GET /api/nope HTTP/1.1\r\nHost: x\r\n\r\n";
  KBC_CHECK(raw_exchange(&s, nf, strlen(nf), &status, reply, sizeof reply));
  KBC_CHECK_EQ_INT(status, 404);
  KBC_CHECK_MSG(strstr(reply, "X-Content-Type-Options: nosniff") != NULL,
                "no nosniff on a 404: %s", reply);
  KBC_CHECK_MSG(strstr(reply, "Content-Type: application/json") != NULL,
                "an error body is not JSON: %s", reply);

  /* An SSE response declares itself and never claims a length it cannot keep. */
  const char *ev = "GET /api/events HTTP/1.1\r\nHost: x\r\n\r\n";
  KBC_CHECK(raw_exchange(&s, ev, strlen(ev), &status, reply, sizeof reply));
  KBC_CHECK_EQ_INT(status, 200);
  KBC_CHECK_MSG(strstr(reply, "Content-Type: text/event-stream") != NULL,
                "no SSE content type: %s", reply);
  KBC_CHECK_MSG(strstr(reply, "Content-Length:") == NULL,
                "SSE response claims a Content-Length: %s", reply);

  srv_stop(&s);
  fx_teardown(&f);
}

/* ------------------------------------------------------------------- main -- */

int main(void) {
  static const kbc_test_case cases[] = {
      {"routing_status_codes", routing_status_codes},
      {"artifact_by_id_found_and_missing", artifact_by_id_found_and_missing},
      {"handle_rejects_incomplete_request", handle_rejects_incomplete_request},
      {"token_gate_is_exhaustive", token_gate_is_exhaustive},
      {"no_token_configured_means_open", no_token_configured_means_open},
      {"non_loopback_bind_without_token_is_refused",
       non_loopback_bind_without_token_is_refused},
      {"query_parsing_rejects_instead_of_defaulting",
       query_parsing_rejects_instead_of_defaulting},
      {"query_is_percent_decoded_before_search",
       query_is_percent_decoded_before_search},
      {"duplicate_q_takes_the_first", duplicate_q_takes_the_first},
      {"absent_offset_is_zero", absent_offset_is_zero},
      {"hostile_requests_are_refused", hostile_requests_are_refused},
      {"response_headers_are_set", response_headers_are_set},
      {NULL, NULL},
  };
  return kbc_test_run("httpd", cases);
}
