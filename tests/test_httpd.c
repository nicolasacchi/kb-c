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
#include <pthread.h>
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

static int call_from(fixture *f, const char *method, const char *path,
                     const char *query, const char *auth, const char *from,
                     kbc_response *out) {
  kbc_request req;
  memset(&req, 0, sizeof req);
  req.method = method;
  req.path = path;
  req.query = query != NULL ? query : "";
  req.body = "";
  req.body_len = 0;
  req.auth = auth != NULL ? auth : "";
  req.client_addr = from;
  kbc_err err;
  kbc_err_reset(&err);
  kbc_status st = kbc_httpd_handle(f->app, f->cfg, &req, out, &err);
  KBC_CHECK_MSG(!kbc_failed(st), "kbc_httpd_handle failed: %s", err.msg);
  return out->status;
}

static int call(fixture *f, const char *method, const char *path,
                const char *query, const char *auth, kbc_response *out) {
  return call_from(f, method, path, query, auth, "test", out);
}

/* Asserts the RFC 7807 shape: the four members a client dispatches on, and a
 * `status` that agrees with the status line. Anything less and a client built
 * against the kb API has to sniff the body to learn what went wrong. */
static void check_problem(const kbc_response *r, int status) {
  KBC_CHECK_EQ_INT(r->status, status);
  KBC_CHECK_MSG(strcmp(r->content_type, "application/problem+json; charset=utf-8") == 0,
                "error is not problem+json: %s", r->content_type);
  KBC_CHECK_MSG(strstr(r->body.ptr, "\"type\":\"urn:kb:errors:") != NULL,
                "no problem type: %s", r->body.ptr);
  KBC_CHECK_MSG(strstr(r->body.ptr, "\"title\":\"") != NULL,
                "no problem title: %s", r->body.ptr);
  KBC_CHECK_MSG(strstr(r->body.ptr, "\"detail\":\"") != NULL,
                "no problem detail: %s", r->body.ptr);
  char pat[64];
  snprintf(pat, sizeof pat, "\"status\":%d", status);
  KBC_CHECK_MSG(strstr(r->body.ptr, pat) != NULL,
                "problem status does not match the HTTP status %d: %s", status,
                r->body.ptr);
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

/* The connect-and-read half, keyed by PORT rather than by `server`, so a
 * per-thread caller can drive it without sharing a mutable fixture. */
static bool raw_exchange_port(int port, const char *raw, size_t n, int *status,
                              char *reply, size_t cap) {
  *status = 0;
  reply[0] = '\0';
  int fd = socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) return false;
  struct sockaddr_in a;
  memset(&a, 0, sizeof a);
  a.sin_family = AF_INET;
  a.sin_port = htons((uint16_t)port);
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

/* Sends `raw` verbatim and reads the whole reply. The daemon answers a refused
 * request with Connection: close, so read-to-EOF is bounded. */
static bool raw_exchange(server *s, const char *raw, size_t n, int *status,
                         char *reply, size_t cap) {
  return raw_exchange_port(s->port, raw, n, status, reply, cap);
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
  KBC_CHECK_MSG(strstr(reply, "Content-Type: application/problem+json") != NULL,
                "an error body is not problem+json: %s", reply);
  KBC_CHECK_MSG(strstr(reply, "\"type\":\"urn:kb:errors:not-found\"") != NULL,
                "a 404 has no problem type: %s", reply);

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

/* ------------------------------------------------------- problem+json ----- */

KBC_TEST(errors_are_problem_json) {
  fixture f;
  fx_setup(&f, NULL);
  kbc_response r;

  /* 404, 405 and 400 are different failures and each must carry its own
   * status in the body, or a client that only reads the body learns nothing. */
  KBC_CHECK_EQ_INT(call(&f, "GET", "/api/nope", NULL, NULL, &r), 404);
  check_problem(&r, 404);
  KBC_CHECK_MSG(strstr(r.body.ptr, "\"detail\":\"no route for GET /api/nope\"") != NULL,
                "detail is not the human reason: %s", r.body.ptr);
  kbc_response_free(&r);

  KBC_CHECK_EQ_INT(call(&f, "GET", "/api/reindex", NULL, NULL, &r), 405);
  check_problem(&r, 405);
  kbc_response_free(&r);

  KBC_CHECK_EQ_INT(call(&f, "GET", "/api/search", "q=alpha&limit=0", NULL, &r),
                   400);
  check_problem(&r, 400);
  kbc_response_free(&r);

  /* A detail is never empty: a client that shows `detail` must have something
   * to show. */
  KBC_CHECK_EQ_INT(call(&f, "GET", "/api/artifacts/xyz", NULL, NULL, &r), 400);
  check_problem(&r, 400);
  KBC_CHECK_MSG(strstr(r.body.ptr, "\"detail\":\"\"") == NULL,
                "an empty detail slipped through: %s", r.body.ptr);
  kbc_response_free(&r);

  /* A success is still plain JSON, not a problem document. */
  KBC_CHECK_EQ_INT(call(&f, "GET", "/api/stats", NULL, NULL, &r), 200);
  KBC_CHECK_EQ_STR(r.content_type, "application/json; charset=utf-8");
  KBC_CHECK_MSG(strstr(r.body.ptr, "\"type\"") == NULL,
                "a 200 answered with a problem body: %s", r.body.ptr);
  kbc_response_free(&r);

  fx_teardown(&f);
}

KBC_TEST(problem_json_over_a_socket) {
  fixture f;
  fx_setup(&f, TOKEN);
  server s;
  srv_start(&s, &f);
  if (s.h == NULL) {
    fx_teardown(&f);
    return;
  }
  char reply[8192];
  int status = 0;
  /* 401 with no credential, and 404 for a path that is not there once the
   * caller is admitted: both are refusals and both must be parseable by the
   * same client code. (The 404 needs a token — the auth gate runs before the
   * router, so an unauthenticated /api request never reaches a route table.) */
  const char *noauth = "GET /api/kbs HTTP/1.1\r\nHost: x\r\n\r\n";
  KBC_CHECK(raw_exchange(&s, noauth, strlen(noauth), &status, reply,
                         sizeof reply));
  KBC_CHECK_EQ_INT(status, 401);
  KBC_CHECK_MSG(strstr(reply, "Content-Type: application/problem+json") != NULL,
                "a 401 is not problem+json: %s", reply);
  KBC_CHECK_MSG(strstr(reply, "\"status\":401") != NULL,
                "a 401 body does not say 401: %s", reply);

  const char *miss = "GET /api/nope HTTP/1.1\r\nHost: x\r\n"
                     "Authorization: Bearer s3cr3t-token\r\n\r\n";
  KBC_CHECK(raw_exchange(&s, miss, strlen(miss), &status, reply, sizeof reply));
  KBC_CHECK_EQ_INT(status, 404);
  KBC_CHECK_MSG(strstr(reply, "\"type\":\"urn:kb:errors:not-found\"") != NULL,
                "a 404 has no stable type: %s", reply);

  srv_stop(&s);
  fx_teardown(&f);
}

/* ------------------------------------------------------------------ /kbs -- */

KBC_TEST(kbs_lists_the_configured_corpora) {
  fixture f;
  fx_setup(&f, NULL);
  kbc_response r;

  KBC_CHECK_EQ_INT(call(&f, "GET", "/api/kbs", NULL, NULL, &r), 200);
  KBC_CHECK_EQ_STR(r.content_type, "application/json; charset=utf-8");
  KBC_CHECK_MSG(strstr(r.body.ptr, "\"name\":\"kb\"") != NULL,
                "the configured corpus is not listed: %s", r.body.ptr);
  KBC_CHECK_MSG(strstr(r.body.ptr, "\"configured\":true") != NULL,
                "a row does not say it is configured: %s", r.body.ptr);
  /* Two documents were indexed by the fixture, and the count is per corpus. */
  KBC_CHECK_MSG(strstr(r.body.ptr, "\"docs\":2") != NULL,
                "doc count is wrong or missing: %s", r.body.ptr);
  KBC_CHECK_MSG(strstr(r.body.ptr, "\"docs_truncated\":false") != NULL,
                "a count that fit is not truncated: %s", r.body.ptr);
  KBC_CHECK_MSG(strstr(r.body.ptr, "\"root_exists\":true") != NULL,
                "the corpus root exists but the row denies it: %s", r.body.ptr);
  KBC_CHECK_MSG(strstr(r.body.ptr, "\"index_terms\":") != NULL,
                "no index term count: %s", r.body.ptr);
  KBC_CHECK_MSG(strstr(r.body.ptr, "\"path\":") != NULL,
                "a row carries no path: %s", r.body.ptr);
  KBC_CHECK_MSG(strstr(r.body.ptr, f.root) != NULL,
                "the row's path is not the configured one: %s", r.body.ptr);
  kbc_response_free(&r);

  /* The filter selects one configured corpus. */
  KBC_CHECK_EQ_INT(call(&f, "GET", "/api/kbs", "kb=kb", NULL, &r), 200);
  KBC_CHECK_MSG(strstr(r.body.ptr, "\"count\":1") != NULL,
                "the filter did not select exactly one corpus: %s", r.body.ptr);
  kbc_response_free(&r);

  /* A corpus the daemon was not configured with is a 404, not an empty 200:
   * a client that typo'd a corpus name must be able to tell. */
  KBC_CHECK_EQ_INT(call(&f, "GET", "/api/kbs", "kb=nosuch", NULL, &r), 404);
  check_problem(&r, 404);
  kbc_response_free(&r);

  /* Hostile: a traversal in the name, a separator in it, and an over-long
   * value are all refused on shape rather than quietly matching nothing. */
  KBC_CHECK_EQ_INT(call(&f, "GET", "/api/kbs", "kb=..%2f..%2fetc", NULL, &r),
                   400);
  check_problem(&r, 400);
  kbc_response_free(&r);
  KBC_CHECK_EQ_INT(call(&f, "GET", "/api/kbs", "kb=a%2fb", NULL, &r), 400);
  kbc_response_free(&r);
  {
    kbc_str q;
    kbc_str_init(&q);
    (void)kbc_str_puts(&q, "kb=");
    for (size_t i = 0; i < 300; i++) (void)kbc_str_putc(&q, 'k');
    KBC_CHECK_EQ_INT(call(&f, "GET", "/api/kbs", q.ptr, NULL, &r), 400);
    check_problem(&r, 400);
    kbc_response_free(&r);
    kbc_str_free(&q);
  }
  /* An empty filter names no corpus either. */
  KBC_CHECK_EQ_INT(call(&f, "GET", "/api/kbs", "kb=", NULL, &r), 400);
  kbc_response_free(&r);

  KBC_CHECK_EQ_INT(call(&f, "POST", "/api/kbs", NULL, NULL, &r), 405);
  kbc_response_free(&r);

  fx_teardown(&f);
}

KBC_TEST(kbs_is_token_gated) {
  fixture f;
  fx_setup(&f, TOKEN);
  kbc_response r;
  KBC_CHECK_EQ_INT(call(&f, "GET", "/api/kbs", NULL, NULL, &r), 401);
  check_problem(&r, 401);
  kbc_response_free(&r);
  char good[64];
  snprintf(good, sizeof good, "Bearer %s", TOKEN);
  KBC_CHECK_EQ_INT(call(&f, "GET", "/api/kbs", NULL, good, &r), 200);
  kbc_response_free(&r);
  /* A traversal must not slip past the gate to be answered. */
  KBC_CHECK_EQ_INT(call(&f, "GET", "/api/kbs", "kb=..%2f..%2fetc", NULL, &r),
                   401);
  kbc_response_free(&r);
  fx_teardown(&f);
}

/* ------------------------------------------------------------ /identity --- */

KBC_TEST(identity_reports_attribution_only) {
  fixture f;
  fx_setup(&f, NULL);
  kbc_response r;

  /* A loopback peer with no token is "local", resolved from the loopback. */
  KBC_CHECK_EQ_INT(
      call_from(&f, "GET", "/api/identity", NULL, NULL, "127.0.0.1", &r), 200);
  KBC_CHECK_MSG(strstr(r.body.ptr, "\"identity\":\"local\"") != NULL,
                "a loopback peer is not local: %s", r.body.ptr);
  KBC_CHECK_MSG(strstr(r.body.ptr, "\"source\":\"loopback\"") != NULL,
                "the resolution source is missing: %s", r.body.ptr);
  KBC_CHECK_MSG(strstr(r.body.ptr, "\"loopback\":true") != NULL,
                "the loopback fact is not reported: %s", r.body.ptr);
  KBC_CHECK_MSG(strstr(r.body.ptr, "\"client_addr\":\"127.0.0.1\"") != NULL,
                "the peer address is not reported: %s", r.body.ptr);
  /* The answer must say it confers nothing, or a client will treat it as a
   * capability it can spend. */
  KBC_CHECK_MSG(strstr(r.body.ptr, "\"trust_tiers\":1") != NULL,
                "the trust tier count is not stated: %s", r.body.ptr);
  KBC_CHECK_MSG(strstr(r.body.ptr, "\"authorization\":false") != NULL,
                "identity claims to authorize: %s", r.body.ptr);
  KBC_CHECK_MSG(strstr(r.body.ptr, "\"roles\":[]") != NULL,
                "roles are claimed: %s", r.body.ptr);
  kbc_response_free(&r);

  /* IPv6 loopback is loopback. */
  KBC_CHECK_EQ_INT(
      call_from(&f, "GET", "/api/identity", NULL, NULL, "::1", &r), 200);
  KBC_CHECK_MSG(strstr(r.body.ptr, "\"identity\":\"local\"") != NULL,
                "::1 is not loopback: %s", r.body.ptr);
  kbc_response_free(&r);

  /* A routable peer is NOT local, and the route says so rather than guessing. */
  KBC_CHECK_EQ_INT(
      call_from(&f, "GET", "/api/identity", NULL, NULL, "10.1.2.3", &r), 200);
  KBC_CHECK_MSG(strstr(r.body.ptr, "\"identity\":\"unattributed\"") != NULL,
                "a routable peer was called local: %s", r.body.ptr);
  KBC_CHECK_MSG(strstr(r.body.ptr, "\"loopback\":false") != NULL,
                "10.1.2.3 was called loopback: %s", r.body.ptr);
  kbc_response_free(&r);

  /* No address at all fails closed, it does not default to loopback. */
  KBC_CHECK_EQ_INT(call_from(&f, "GET", "/api/identity", NULL, NULL, "?", &r),
                   200);
  KBC_CHECK_MSG(strstr(r.body.ptr, "\"identity\":\"unattributed\"") != NULL,
                "an unknown peer was attributed: %s", r.body.ptr);
  kbc_response_free(&r);

  /* A client cannot name itself. */
  KBC_CHECK_EQ_INT(call(&f, "GET", "/api/identity", "as=admin", NULL, &r), 400);
  check_problem(&r, 400);
  kbc_response_free(&r);
  KBC_CHECK_EQ_INT(call(&f, "GET", "/api/identity", "user=admin", NULL, &r),
                   400);
  kbc_response_free(&r);
  {
    kbc_str q;
    kbc_str_init(&q);
    (void)kbc_str_puts(&q, "identity=");
    for (size_t i = 0; i < 512; i++) (void)kbc_str_putc(&q, 'a');
    KBC_CHECK_EQ_INT(call(&f, "GET", "/api/identity", q.ptr, NULL, &r), 400);
    kbc_response_free(&r);
    kbc_str_free(&q);
  }
  KBC_CHECK_EQ_INT(call(&f, "POST", "/api/identity", NULL, NULL, &r), 405);
  kbc_response_free(&r);

  fx_teardown(&f);
}

KBC_TEST(identity_names_the_admission_that_happened) {
  fixture f;
  fx_setup(&f, TOKEN);
  kbc_response r;
  char good[64];
  snprintf(good, sizeof good, "Bearer %s", TOKEN);

  /* Admitted by the token, from a routable peer: the token is what let this in,
   * and the route must report that rather than crediting the address. */
  KBC_CHECK_EQ_INT(
      call_from(&f, "GET", "/api/identity", NULL, good, "10.1.2.3", &r), 200);
  KBC_CHECK_MSG(strstr(r.body.ptr, "\"identity\":\"operator\"") != NULL,
                "a token caller is not the operator: %s", r.body.ptr);
  KBC_CHECK_MSG(strstr(r.body.ptr, "\"source\":\"token\"") != NULL,
                "the token admission is not the reported source: %s",
                r.body.ptr);
  kbc_response_free(&r);

  /* The same token from loopback is still just the operator, and the identity
   * it resolves to buys nothing: it gets the same routes, not more. */
  KBC_CHECK_EQ_INT(
      call_from(&f, "GET", "/api/identity", NULL, good, "127.0.0.1", &r), 200);
  KBC_CHECK_MSG(strstr(r.body.ptr, "\"source\":\"token\"") != NULL,
                "loopback beat the token in the ladder: %s", r.body.ptr);
  kbc_response_free(&r);

  /* And the route is gated exactly like its neighbours. */
  KBC_CHECK_EQ_INT(
      call_from(&f, "GET", "/api/identity", NULL, NULL, "127.0.0.1", &r), 401);
  check_problem(&r, 401);
  kbc_response_free(&r);
  KBC_CHECK_EQ_INT(
      call_from(&f, "GET", "/api/identity", NULL, "Bearer wrong", "127.0.0.1",
                &r),
      403);
  check_problem(&r, 403);
  kbc_response_free(&r);

  fx_teardown(&f);
}

KBC_TEST(new_routes_are_in_the_route_table) {
  /* The export is what the docs and the CLI are generated from, so a route
   * that exists but is not listed is a route nobody can discover — and, worse,
   * a route whose `needs_auth` nobody checked. */
  bool found_kbs = false, found_id = false;
  for (size_t i = 0; i < KBC_ROUTES_LEN; i++) {
    if (strcmp(KBC_ROUTES[i].path, "/api/kbs") == 0) {
      found_kbs = true;
      KBC_CHECK_MSG(KBC_ROUTES[i].needs_auth,
                    "/api/kbs is listed as open in the route table");
    }
    if (strcmp(KBC_ROUTES[i].path, "/api/identity") == 0) {
      found_id = true;
      KBC_CHECK_MSG(KBC_ROUTES[i].needs_auth,
                    "/api/identity is listed as open in the route table");
    }
  }
  KBC_CHECK_MSG(found_kbs, "/api/kbs is missing from KBC_ROUTES");
  KBC_CHECK_MSG(found_id, "/api/identity is missing from KBC_ROUTES");
}

/* ------------------------------------------------------------------ CORS -- */

/* Counts the status lines in a reply that carried several pipelined answers. */
static int count_statuses(const char *reply) {
  int n = 0;
  for (const char *p = reply; (p = strstr(p, "HTTP/1.1 ")) != NULL; p += 9) n++;
  return n;
}

/* Sends `n` identical pipelined requests down ONE connection and reads until
 * the daemon closes. The per-connection cap is per connection, so the only way
 * to observe it is to keep the connection. */
static bool pipeline_exchange(server *s, int n, char *reply, size_t cap) {
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
  const char *one = "GET /api/health HTTP/1.1\r\nHost: x\r\n\r\n";
  for (int i = 0; i < n; i++) {
    size_t len = strlen(one);
    size_t off = 0;
    while (off < len) {
      ssize_t w = send(fd, one + off, len - off, MSG_NOSIGNAL);
      if (w <= 0) break;
      off += (size_t)w;
    }
  }
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
  return got > 0;
}

KBC_TEST(cors_is_same_origin_only_unless_configured) {
  fixture f;
  fx_setup(&f, NULL);
  server s;
  char reply[8192];
  int status = 0;

  /* Default: no allowlist, so an Origin is ignored entirely — and never
   * answered with a wildcard. */
  (void)unsetenv("KBC_CORS_ORIGINS");
  srv_start(&s, &f);
  if (s.h != NULL) {
    const char *r1 =
        "GET /api/health HTTP/1.1\r\nHost: x\r\n"
        "Origin: https://ops.example\r\n\r\n";
    KBC_CHECK(raw_exchange(&s, r1, strlen(r1), &status, reply, sizeof reply));
    KBC_CHECK_EQ_INT(status, 200);
    KBC_CHECK_MSG(strstr(reply, "Vary: Origin") != NULL,
                  "no Vary: Origin on an unconfigured daemon: %s", reply);
    KBC_CHECK_MSG(strstr(reply, "Access-Control-Allow-Origin") == NULL,
                  "an origin was granted with no allowlist: %s", reply);
    srv_stop(&s);
  }

  /* Configured: that one origin is reflected, and only that one. */
  KBC_CHECK(setenv("KBC_CORS_ORIGINS", "https://ops.example", 1) == 0);
  srv_start(&s, &f);
  if (s.h != NULL) {
    const char *ok =
        "GET /api/health HTTP/1.1\r\nHost: x\r\n"
        "Origin: https://ops.example\r\n\r\n";
    KBC_CHECK(raw_exchange(&s, ok, strlen(ok), &status, reply, sizeof reply));
    KBC_CHECK_EQ_INT(status, 200);
    KBC_CHECK_MSG(strstr(reply, "Access-Control-Allow-Origin: "
                              "https://ops.example\r\n") != NULL,
                  "the configured origin was not reflected: %s", reply);
    KBC_CHECK_MSG(strstr(reply, "Access-Control-Allow-Origin: *") == NULL,
                  "a wildcard was served: %s", reply);
    KBC_CHECK_MSG(strstr(reply, "Vary: Origin") != NULL,
                  "an allowed origin was not varied on: %s", reply);

    const char *other =
        "GET /api/health HTTP/1.1\r\nHost: x\r\n"
        "Origin: https://evil.example\r\n\r\n";
    KBC_CHECK(raw_exchange(&s, other, strlen(other), &status, reply,
                           sizeof reply));
    KBC_CHECK_EQ_INT(status, 200);
    KBC_CHECK_MSG(strstr(reply, "Access-Control-Allow-Origin") == NULL,
                  "an unlisted origin was granted: %s", reply);

    /* A prefix of an allowed origin is not an allowed origin. */
    const char *prefix =
        "GET /api/health HTTP/1.1\r\nHost: x\r\n"
        "Origin: https://ops.example.evil.test\r\n\r\n";
    KBC_CHECK(raw_exchange(&s, prefix, strlen(prefix), &status, reply,
                           sizeof reply));
    KBC_CHECK_MSG(strstr(reply, "Access-Control-Allow-Origin") == NULL,
                  "a prefix of an allowed origin was granted: %s", reply);

    /* The preflight is answered, carries the methods the API really uses, and
     * is not a way past the token gate: it grants permission, not access. */
    const char *pre =
        "OPTIONS /api/stats HTTP/1.1\r\nHost: x\r\n"
        "Origin: https://ops.example\r\n"
        "Access-Control-Request-Method: GET\r\n\r\n";
    KBC_CHECK(raw_exchange(&s, pre, strlen(pre), &status, reply, sizeof reply));
    KBC_CHECK_EQ_INT(status, 204);
    KBC_CHECK_MSG(strstr(reply, "Access-Control-Allow-Methods: GET, POST, "
                                "OPTIONS") != NULL,
                  "preflight does not list the API's methods: %s", reply);
    KBC_CHECK_MSG(strstr(reply, "Access-Control-Allow-Headers: Authorization") !=
                      NULL,
                  "preflight does not allow the auth header: %s", reply);
    KBC_CHECK_MSG(strstr(reply, "Access-Control-Max-Age: 600") != NULL,
                  "preflight does not say how long the answer stands: %s", reply);
    /* A 204 has no body, so it must not claim a content type or a length. */
    KBC_CHECK_MSG(strstr(reply, "Content-Type:") == NULL,
                  "a 204 declares a content type for no body: %s", reply);
    KBC_CHECK_MSG(strstr(reply, "Content-Length:") == NULL,
                  "a 204 declares a length: %s", reply);

    /* A preflight from an origin the daemon does not serve is refused, and
     * gets no allow header. */
    const char *pre_bad =
        "OPTIONS /api/stats HTTP/1.1\r\nHost: x\r\n"
        "Origin: https://evil.example\r\n\r\n";
    KBC_CHECK(raw_exchange(&s, pre_bad, strlen(pre_bad), &status, reply,
                           sizeof reply));
    KBC_CHECK_EQ_INT(status, 403);
    KBC_CHECK_MSG(strstr(reply, "Content-Type: application/problem+json") !=
                      NULL,
                  "a refused preflight is not problem+json: %s", reply);
    KBC_CHECK_MSG(strstr(reply, "Access-Control-Allow-Origin") == NULL,
                  "a refused preflight still granted the origin: %s", reply);

    /* No Origin at all is not a preflight. */
    const char *pre_none = "OPTIONS /api/stats HTTP/1.1\r\nHost: x\r\n\r\n";
    KBC_CHECK(
        raw_exchange(&s, pre_none, strlen(pre_none), &status, reply,
                     sizeof reply));
    KBC_CHECK_EQ_INT(status, 404);

    srv_stop(&s);
  }
  (void)unsetenv("KBC_CORS_ORIGINS");
  fx_teardown(&f);
}

KBC_TEST(rate_limit_caps_one_connection) {
  fixture f;
  fx_setup(&f, NULL);
  server s;
  /* The cap protects the worker from ONE client, not the daemon from an
   * attacker: there is no untrusted-caller path to defend against here, so the
   * assertion is the fairness property, not a security claim. */
  KBC_CHECK(setenv("KBC_RATE_LIMIT_RPS", "5", 1) == 0);
  srv_start(&s, &f);
  if (s.h == NULL) {
    (void)unsetenv("KBC_RATE_LIMIT_RPS");
    fx_teardown(&f);
    return;
  }
  char reply[65536];
  bool ok = pipeline_exchange(&s, 12, reply, sizeof reply);
  KBC_CHECK(ok);
  /* Five are served, the sixth is refused and the connection is closed: the cap
   * is a ceiling, not a queue, so the client is told to come back later
   * instead of being silently slowed. */
  KBC_CHECK_MSG(count_statuses(reply) == 6,
                "expected 5 answers plus one 429, got %d",
                count_statuses(reply));
  KBC_CHECK_MSG(strstr(reply, "HTTP/1.1 429 Too Many Requests") != NULL,
                "twelve requests on one connection were never capped: %s",
                reply);
  KBC_CHECK_MSG(strstr(reply, "\"type\":\"urn:kb:errors:too-many-requests\"") !=
                    NULL,
                "a 429 is not problem+json: %s", reply);
  KBC_CHECK_MSG(strstr(reply, "Retry-After: 1") != NULL,
                "a 429 does not say when to come back: %s", reply);

  /* A second connection is not charged for the first one's spending. */
  char reply2[8192];
  int st2 = 0;
  const char *one_get = "GET /api/health HTTP/1.1\r\nHost: x\r\n\r\n";
  KBC_CHECK(
      raw_exchange(&s, one_get, strlen(one_get), &st2, reply2, sizeof reply2));
  KBC_CHECK_MSG(strncmp(reply2, "HTTP/1.1 200", 12) == 0,
                "a fresh connection was refused for another's spending: %s",
                reply2);

  srv_stop(&s);
  (void)unsetenv("KBC_RATE_LIMIT_RPS");
  fx_teardown(&f);
}

KBC_TEST(a_bad_rate_limit_threshold_refuses_to_start) {
  fixture f;
  fx_setup(&f, NULL);
  KBC_CHECK(setenv("KBC_RATE_LIMIT_RPS", "many", 1) == 0);
  free(f.cfg->bind_addr);
  f.cfg->bind_addr = strdup("127.0.0.1");
  f.cfg->port = 0;
  f.cfg->http_workers = 1;
  kbc_err err;
  kbc_err_reset(&err);
  kbc_httpd *h = kbc_httpd_start(f.app, f.cfg, &err);
  KBC_CHECK_NULL(h);
  KBC_CHECK_ERR(err.status, KBC_ERR_INVALID);
  KBC_CHECK_MSG(strstr(err.msg, "KBC_RATE_LIMIT_RPS") != NULL,
                "the refusal does not name the variable: %s", err.msg);
  (void)unsetenv("KBC_RATE_LIMIT_RPS");
  fx_teardown(&f);
}

/* ------------------------------------- concurrent accepts (regression) ------
 *
 * The daemon keeps ONE array of live connections shared by every worker, and
 * worker_accept appends to it. Growing that array used to happen WITHOUT the
 * mutex the close path and the SSE fan-out take, so two workers accepting at
 * the same moment both realloc'd the same block: one buffer was freed while
 * connections were still being written into it, and the daemon died with
 * glibc's "double free or corruption (!prev)". It only showed up at high
 * connection counts, which is why no test that opened a handful of sockets
 * ever saw it.
 *
 * The test therefore does the one thing the bug needed: it makes MANY
 * connections arrive at once, from many threads, against a MULTI-worker
 * daemon (one worker cannot race itself), and it keeps going long enough for
 * the array to be reallocated repeatedly rather than once. The array starts
 * at 8 and doubles, so the connection count here crosses several growths.
 *
 * It is a real race, so a passing run is not proof the old code always
 * failed — it is proof the shape is exercised. Under the sanitizer lanes it
 * IS proof: ASan aborts on the losing realloc's double free, and TSan reports
 * the unsynchronised write to all_conns. Both are deterministic about the
 * BUG being present; neither can be made deterministic about a fix not being
 * needed, which is why this hammers rather than asserts a count. What is
 * asserted is the invariant a user can see: every connection that was
 * accepted got a well-formed answer, and the daemon is still serving after
 * the storm. */

#define CONC_CONNS 40   /* connections per thread, opened back to back */
#define CONC_THREADS 32 /* connectors released at the same instant */
#define CONC_ROUNDS 4   /* waves, so the table is reallocated repeatedly */

typedef struct {
  int port;
  int rounds;
  pthread_barrier_t *start;
  int answered;
  int bad_reply;
} conc_arg;

static void *conc_connector(void *p) {
  conc_arg *a = (conc_arg *)p;
 /* All threads connect at the same instant, which is what puts the workers
 * in worker_accept together. */
  pthread_barrier_wait(a->start);
  static const char req[] = "GET /api/health HTTP/1.1\r\nHost: x\r\n\r\n";
  char reply[8192];
  for (int i = 0; i < a->rounds; i++) {
    int status = 0;
    if (!raw_exchange_port(a->port, req, strlen(req), &status, reply,
                           sizeof reply)) {
      a->bad_reply++;
      continue;
    }
    if (status != 200) a->bad_reply++;
    a->answered++;
  }
  return NULL;
}

KBC_TEST(concurrent_connects_do_not_corrupt_the_conn_table) {
  fixture f;
  fx_setup(&f, NULL);
  free(f.cfg->bind_addr);
  f.cfg->bind_addr = strdup("127.0.0.1");
  f.cfg->port = 0;
  /* More than one worker is the point: the race is between workers, and a
   * single-worker daemon cannot take it. */
  f.cfg->http_workers = 4;
  kbc_err err;
  kbc_err_reset(&err);
  kbc_httpd *h = kbc_httpd_start(f.app, f.cfg, &err);
  if (h == NULL) {
    fprintf(stderr, "  httpd_start: %s\n", err.msg);
    kbc_test_fail(__FILE__, __LINE__, "httpd_start: %s", err.msg);
    fx_teardown(&f);
    return;
  }
  int port = kbc_httpd_port(h);
  KBC_CHECK_MSG(port > 0, "no listening port");

  /* Several waves, not one: the table starts at 8 entries and doubles, and the
   * race is on the GROWTH, so a single small burst may never reallocate while
   * two workers are inside it. Each wave re-grows a table the previous one
   * left large, which is the state the load test that found the bug was in. */
  for (int round = 0; round < CONC_ROUNDS; round++) {
    pthread_barrier_t start;
    KBC_CHECK(pthread_barrier_init(&start, NULL, (unsigned)CONC_THREADS) == 0);
    pthread_t th[CONC_THREADS];
    conc_arg args[CONC_THREADS];
    for (int i = 0; i < CONC_THREADS; i++) {
      memset(&args[i], 0, sizeof args[i]);
      args[i].port = port;
      args[i].rounds = CONC_CONNS;
      args[i].start = &start;
      KBC_CHECK(pthread_create(&th[i], NULL, conc_connector, &args[i]) == 0);
    }
    int answered = 0, bad = 0;
    for (int i = 0; i < CONC_THREADS; i++) {
      (void)pthread_join(th[i], NULL);
      answered += args[i].answered;
      bad += args[i].bad_reply;
    }
    (void)pthread_barrier_destroy(&start);
    KBC_CHECK_MSG(bad == 0, "round %d: %d of %d connections got no 200", round,
                  bad, CONC_THREADS * CONC_CONNS);
    KBC_CHECK_MSG(answered == CONC_THREADS * CONC_CONNS,
                  "round %d: only %d of %d connections completed", round,
                  answered, CONC_THREADS * CONC_CONNS);
  }

  /* The daemon survived the storm and still answers: a corrupted connection
   * table shows up here as a dead or wedged listener, not as a bad count. */
  char reply[8192];
  int status = 0;
  static const char health[] = "GET /api/health HTTP/1.1\r\nHost: x\r\n\r\n";
  KBC_CHECK(raw_exchange_port(port, health, strlen(health), &status, reply,
                             sizeof reply));
  KBC_CHECK_MSG(status == 200, "the daemon did not survive the storm: %d",
                status);

  kbc_httpd_stop(h);
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
      {"errors_are_problem_json", errors_are_problem_json},
      {"problem_json_over_a_socket", problem_json_over_a_socket},
      {"kbs_lists_the_configured_corpora", kbs_lists_the_configured_corpora},
      {"kbs_is_token_gated", kbs_is_token_gated},
      {"identity_reports_attribution_only", identity_reports_attribution_only},
      {"identity_names_the_admission_that_happened",
       identity_names_the_admission_that_happened},
      {"new_routes_are_in_the_route_table", new_routes_are_in_the_route_table},
      {"cors_is_same_origin_only_unless_configured",
       cors_is_same_origin_only_unless_configured},
      {"rate_limit_caps_one_connection", rate_limit_caps_one_connection},
      {"a_bad_rate_limit_threshold_refuses_to_start",
       a_bad_rate_limit_threshold_refuses_to_start},
      {"concurrent_connects_do_not_corrupt_the_conn_table",
       concurrent_connects_do_not_corrupt_the_conn_table},
      {NULL, NULL},
  };
  return kbc_test_run("httpd", cases);
}
