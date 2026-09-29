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
#include <fcntl.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
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

  /* Well-formed, wrong: 403, so a client can tell "who are you" from "no". A
   * carrier that presents NOTHING is 401, whatever it looked like.
   *
   * The 401/403 split is a DELIBERATE kb-c narrowing, not parity: the original
   * has only `unauthorized` on this path and answers 401 for a wrong secret
   * too (`middleware.rs:302-304`). kb-c keeps the second status because the
   * CLI keys on 401 for its "run `kbc token generate`" hint and because the
   * distinction is what lets a client tell a missing credential from a stale
   * one. What the original establishes, and what is pinned here, is that a
   * MISSING carrier is 401 and a PRESENTED-AND-WRONG one is 403 — including
   * for the second carrier, in token_carriers_admit_and_refuse. */
  KBC_CHECK_EQ_INT(
      call(&f, "GET", "/api/search", "q=alpha", "Bearer wrong", &r), 403);
  kbc_response_free(&r);
  /* An EMPTY token is 401, not 403, and used to be asserted as 403 here.
   * Over a socket the parser strips the optional whitespace around a field
   * value (RFC 7230 §3.2.4), so `Authorization: Bearer ` reaches the ladder as
   * the six bytes "Bearer": the prefix does not match and no credential was
   * presented. The 403 this asserted was reachable only through this socketless
   * seam, which hands kbc_httpd_handle a `kbc_request.auth` that never went
   * through a parser — a fiction no HTTP client could produce. A secret of
   * length zero is now treated as no secret at all, so the seam and the wire
   * agree; the padded-but-present cases the trim exists for are in
   * token_carriers_admit_and_refuse. */
  KBC_CHECK_EQ_INT(call(&f, "GET", "/api/search", "q=alpha", "Bearer ", &r), 401);
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

/* --------------------------------------------- the two token carriers ----
 *
 * X-Kb-Token exists only ON THE WIRE — it is a header, and the frozen
 * `kbc_request` (httpd.h:28-36) carries just the Authorization value, so the
 * socketless seam cannot even express it. Every carrier case therefore goes
 * over a real loopback socket, which is also the path a caller actually uses.
 */

/* Sends `raw` and reports the status plus the whole reply. */
static bool carrier_exchange(server *s, const char *raw, int *status,
                             char *reply, size_t cap) {
  return raw_exchange(s, raw, strlen(raw), status, reply, cap);
}

KBC_TEST(token_carriers_admit_and_refuse) {
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
  char good[64];
  snprintf(good, sizeof good, "Bearer %s", TOKEN);

  struct {
    const char *hdrs;   /* the whole header block, Host included */
    int want;
    const char *what;
  } cases[] = {
      {"Authorization: Bearer s3cr3t-token\r\n", 200, "bearer alone"},
      {"X-Kb-Token: s3cr3t-token\r\n", 200, "x-kb-token alone"},
      /* Both, agreeing: admitted once, on the first carrier. */
      {"Authorization: Bearer s3cr3t-token\r\nX-Kb-Token: s3cr3t-token\r\n",
       200, "both carriers agreeing"},
      /* Both, DISAGREEING. The original does not compare them: the first
       * match wins and the loser is never examined (middleware.rs:379-391),
       * so a valid bearer beside a bogus header is still admitted. */
      {"Authorization: Bearer s3cr3t-token\r\nX-Kb-Token: not-a-secret\r\n",
       200, "both carriers, bearer right and x-kb wrong"},
      /* The mirror: a bogus bearer does not veto a valid X-Kb-Token, which is
       * what makes the second carrier usable when a proxy has already
       * overwritten Authorization. */
      {"Authorization: Bearer not-a-secret\r\nX-Kb-Token: s3cr3t-token\r\n",
       200, "both carriers, bearer wrong and x-kb right"},
      {"", 401, "neither carrier"},
      {"Authorization: Bearer wrong\r\n", 403, "wrong bearer"},
      {"X-Kb-Token: wrong\r\n", 403, "wrong x-kb-token"},
      /* A blank Authorization is 401, NOT the 403 a wrong secret gets, and
       * the reason is upstream of the ladder: RFC 7230 §3.2.4 requires a
       * parser to strip the optional whitespace around a field value, so this
       * arrives as the six bytes "Bearer" with no trailing space and the
       * "Bearer " prefix does not match. No credential was presented.
       *
       * The socketless seam disagrees, and deliberately so: it hands
       * kbc_httpd_handle a `kbc_request` whose `auth` never went through a
       * parser, so "Bearer " there really does carry a prefix and an empty
       * secret, and is a 403 (token_gate_is_exhaustive). The wire is the
       * contract a caller meets; the seam is a test convenience. */
      {"Authorization: Bearer \r\n", 401, "blank bearer"},
      /* Whitespace the parser CANNOT remove stays in the secret, so this is
       * the well-formed-but-wrong case: the leading space is part of " x". */
      {"Authorization: Bearer  x\r\n", 403, "bearer with an inner space"},
      /* A BLANK X-Kb-Token is likewise absent (middleware.rs:421-427 filters
       * it on `!s.is_empty()`), so it is 401 for the same reason. */
      {"X-Kb-Token: \r\n", 401, "blank x-kb-token"},
      {"X-Kb-Token:    \r\n", 401, "whitespace-only x-kb-token"},
      /* The X-Kb-Token trim is load-bearing, not cosmetic: a padded value
       * carries the SAME secret and must be admitted. This is the case that
       * needs the trimmed LENGTH rather than a NUL — a compare that took
       * strlen of the untrimmed value would see the trailing spaces, miss, and
       * 403 a caller that presented the right secret. */
      {"X-Kb-Token:   s3cr3t-token  \r\n", 200, "padded x-kb-token"},
      {"X-Kb-Token:\ts3cr3t-token\t\r\n", 200, "tab-padded x-kb-token"},
      /* A blank Authorization is 401, NOT the 403 a wrong secret gets, and
       * the reason is upstream of the ladder: RFC 7230 §3.2.4 requires a
       * parser to strip the optional whitespace around a field value, so this
       * arrives as the six bytes "Bearer" with no trailing space and the
       * "Bearer " prefix does not match. No credential was presented.
       *
       * The socketless seam disagrees, and deliberately so: it hands
       * kbc_httpd_handle a `kbc_request` whose `auth` never went through a
       * parser, so "Bearer " there really does carry a prefix and an empty
       * secret, and is a 403 (token_gate_is_exhaustive). The wire is the
       * contract a caller meets; the seam is a test convenience. */
      {"Authorization: Bearer \r\n", 401, "blank bearer"},
      /* Whitespace the parser CANNOT remove stays in the secret, so this is
       * the well-formed-but-wrong case: the leading space is part of " x". */
      {"Authorization: Bearer  x\r\n", 403, "bearer with an inner space"},
      /* Case-sensitive on the scheme, like strip_prefix("Bearer ") — a
       * lower-case scheme is not a credential at all. */
      {"Authorization: bearer s3cr3t-token\r\n", 401, "lower-case scheme"},
      {"Authorization: Basic s3cr3t-token\r\n", 401, "wrong scheme"},
  };
  for (size_t i = 0; i < sizeof cases / sizeof *cases; i++) {
    char req[512];
    int n = snprintf(req, sizeof req,
                     "GET /api/identity HTTP/1.1\r\nHost: x\r\n%s\r\n",
                     cases[i].hdrs);
    KBC_CHECK_MSG(n > 0 && (size_t)n < sizeof req, "bad fixture for %s",
                  cases[i].what);
    KBC_CHECK_MSG(carrier_exchange(&s, req, &status, reply, sizeof reply),
                  "%s: no HTTP reply", cases[i].what);
    KBC_CHECK_MSG(status == cases[i].want,
                  "%s: got %d, want %d (%s)", cases[i].what, status,
                  cases[i].want, reply);
    if (cases[i].want >= 400) {
      KBC_CHECK_MSG(strstr(reply, "application/problem+json") != NULL,
                    "%s: refusal is not problem+json: %s", cases[i].what,
                    reply);
      if (cases[i].want == 401) {
        KBC_CHECK_MSG(strstr(reply, "WWW-Authenticate: Bearer realm=\"kb\"") !=
                          NULL,
                      "%s: a 401 must challenge: %s", cases[i].what, reply);
      }
    }
  }

  srv_stop(&s);
  fx_teardown(&f);
}

KBC_TEST(both_carriers_authorization_decides_and_says_so) {
  /* The rule is a silent PREFERENCE, so the only way an operator can see it
   * is if the daemon reports which carrier decided. Without the `carrier`
   * field these three requests are indistinguishable on the wire, and a
   * per-user secret silently discarded behind a proxy's shared token is
   * invisible. */
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

  /* Agreeing: Authorization is the carrier that decided, by position. */
  {
    const char *req = "GET /api/identity HTTP/1.1\r\nHost: x\r\n"
                      "Authorization: Bearer s3cr3t-token\r\n"
                      "X-Kb-Token: s3cr3t-token\r\n\r\n";
    KBC_CHECK(carrier_exchange(&s, req, &status, reply, sizeof reply));
    KBC_CHECK_EQ_INT(status, 200);
    KBC_CHECK_MSG(strstr(reply, "\"carrier\":\"authorization\"") != NULL,
                  "agreeing carriers did not report the first: %s", reply);
  }

  /* Disagreeing, bearer valid: still Authorization, and the bogus
   * X-Kb-Token neither refuses the request nor changes the attribution. */
  {
    const char *req = "GET /api/identity HTTP/1.1\r\nHost: x\r\n"
                      "Authorization: Bearer s3cr3t-token\r\n"
                      "X-Kb-Token: someone-elses-secret\r\n\r\n";
    KBC_CHECK(carrier_exchange(&s, req, &status, reply, sizeof reply));
    KBC_CHECK_EQ_INT(status, 200);
    KBC_CHECK_MSG(strstr(reply, "\"carrier\":\"authorization\"") != NULL,
                  "a disagreeing X-Kb-Token overrode the bearer: %s", reply);
    KBC_CHECK_MSG(strstr(reply, "\"identity\":\"operator\"") != NULL,
                  "identity changed: %s", reply);
  }

  /* Disagreeing the other way: the bearer does NOT veto a valid X-Kb-Token,
   * which is the case the second carrier exists for. */
  {
    const char *req = "GET /api/identity HTTP/1.1\r\nHost: x\r\n"
                      "Authorization: Bearer not-a-secret\r\n"
                      "X-Kb-Token: s3cr3t-token\r\n\r\n";
    KBC_CHECK(carrier_exchange(&s, req, &status, reply, sizeof reply));
    KBC_CHECK_EQ_INT(status, 200);
    KBC_CHECK_MSG(strstr(reply, "\"carrier\":\"x-kb-token\"") != NULL,
                  "the fallback carrier was not reported: %s", reply);
  }

  /* A loopback caller with no credential was admitted by neither, and saying
   * "none" is what distinguishes it from a token admission. */
  {
    kbc_response r;
    KBC_CHECK_EQ_INT(
        call_from(&f, "GET", "/api/identity", NULL, NULL, "127.0.0.1", &r),
        401);
    kbc_response_free(&r);
  }

  /* The identity body answers "which credential did you present", so the
   * response must declare the headers it varies on — otherwise a shared cache
   * is free to hand one caller's attribution to another. Vary is a set, so
   * this also has to survive alongside the Origin the write path always
   * adds; a second Vary line replacing the first would silently re-open the
   * cross-origin hole the first one closed. */
  {
    const char *req = "GET /api/identity HTTP/1.1\r\nHost: x\r\n"
                      "Authorization: Bearer s3cr3t-token\r\n\r\n";
    KBC_CHECK(carrier_exchange(&s, req, &status, reply, sizeof reply));
    KBC_CHECK_EQ_INT(status, 200);
    KBC_CHECK_MSG(strstr(reply, "Authorization") != NULL &&
                      strstr(reply, "X-Kb-Token") != NULL,
                  "the identity response does not declare what it varies on: "
                  "%s",
                  reply);
    KBC_CHECK_MSG(strstr(reply, "Origin") != NULL,
                  "the Origin variance was dropped: %s", reply);
  }

  srv_stop(&s);
  fx_teardown(&f);
}

KBC_TEST(identity_reports_the_carrier_for_a_socketless_token) {
  fixture f;
  fx_setup(&f, TOKEN);
  kbc_response r;
  char good[64];
  snprintf(good, sizeof good, "Bearer %s", TOKEN);
  KBC_CHECK_EQ_INT(
      call_from(&f, "GET", "/api/identity", NULL, good, "10.1.2.3", &r), 200);
  KBC_CHECK_MSG(strstr(r.body.ptr, "\"carrier\":\"authorization\"") != NULL,
                "a bearer admission did not name its carrier: %s", r.body.ptr);
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

/* ------------------------------------------------- live SSE streams over TCP --
 *
 * An SSE stream never ends on its own, so it cannot be read the way the other
 * exchanges in this file are: it is read with a deadline, and the client
 * decides when to hang up. */

typedef struct {
  int fd;
  char buf[16384];
  size_t len;
} sse_client;

/* Opens a stream with an arbitrary request head, so a test can supply a
 * Last-Event-ID. `rcvbuf` shrinks the kernel receive buffer before the
 * connect, which is what makes a stall observable at all: with the default
 * buffer the kernel absorbs the whole burst and the daemon's own queue never
 * fills, so the lag path is never taken and the test would pass on a build
 * with the probe removed. */
static bool sse_open_head(int port, const char *last_event_id, int rcvbuf,
                          sse_client *c) {
  memset(c, 0, sizeof *c);
  c->fd = socket(AF_INET, SOCK_STREAM, 0);
  if (c->fd < 0) return false;
  if (rcvbuf > 0) {
    (void)setsockopt(c->fd, SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof rcvbuf);
  }
  struct sockaddr_in a;
  memset(&a, 0, sizeof a);
  a.sin_family = AF_INET;
  a.sin_port = htons((uint16_t)port);
  a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  if (connect(c->fd, (struct sockaddr *)&a, sizeof a) != 0) {
    close(c->fd);
    c->fd = -1;
    return false;
  }
  char req[256];
  int n = snprintf(req, sizeof req,
                   "GET /api/events HTTP/1.1\r\nHost: x\r\n%s%s%s\r\n",
                   last_event_id != NULL ? "Last-Event-ID: " : "",
                   last_event_id != NULL ? last_event_id : "",
                   last_event_id != NULL ? "\r\n" : "");
  if (n <= 0 || (size_t)n >= sizeof req) {
    close(c->fd);
    c->fd = -1;
    return false;
  }
  size_t off = 0;
  while (off < (size_t)n) {
    ssize_t w = send(c->fd, req + off, (size_t)n - off, MSG_NOSIGNAL);
    if (w <= 0) {
      close(c->fd);
      c->fd = -1;
      return false;
    }
    off += (size_t)w;
  }
  struct timeval tv;
  tv.tv_sec = 5;
  tv.tv_usec = 0;
  (void)setsockopt(c->fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
  return true;
}

static bool sse_open(int port, sse_client *c) {
  memset(c, 0, sizeof *c);
  c->fd = socket(AF_INET, SOCK_STREAM, 0);
  if (c->fd < 0) return false;
  struct sockaddr_in a;
  memset(&a, 0, sizeof a);
  a.sin_family = AF_INET;
  a.sin_port = htons((uint16_t)port);
  a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  if (connect(c->fd, (struct sockaddr *)&a, sizeof a) != 0) {
    close(c->fd);
    c->fd = -1;
    return false;
  }
  static const char req[] = "GET /api/events HTTP/1.1\r\nHost: x\r\n\r\n";
  size_t off = 0;
  while (off < sizeof req - 1) {
    ssize_t w = send(c->fd, req + off, sizeof req - 1 - off, MSG_NOSIGNAL);
    if (w <= 0) {
      close(c->fd);
      c->fd = -1;
      return false;
    }
    off += (size_t)w;
  }
  struct timeval tv;
  tv.tv_sec = 2;
  tv.tv_usec = 0;
  (void)setsockopt(c->fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
  return true;
}

/* Exactly one recv. True means "the stream is still open", whether or not it
 * had anything to say: only EOF is a closed stream, a timeout is an idle
 * one, and an idle stream is the normal case for /api/events. */
static bool sse_poll(sse_client *c) {
  if (c->fd < 0) return false;
  if (c->len + 1 >= sizeof c->buf) return true;
  ssize_t n = recv(c->fd, c->buf + c->len, sizeof c->buf - 1 - c->len, 0);
  if (n > 0) {
    c->len += (size_t)n;
    c->buf[c->len] = '\0';
    return true;
  }
  if (n == 0) return false;
  if (errno == EINTR) return true;
  return errno == EAGAIN || errno == EWOULDBLOCK;
}

static bool sse_saw(const sse_client *c, const char *needle) {
  return strstr(c->buf, needle) != NULL;
}

static bool sse_wait(sse_client *c, const char *needle, int tries) {
  for (int i = 0; i < tries; i++) {
    if (sse_saw(c, needle)) return true;
    if (!sse_poll(c)) return false;
  }
  return sse_saw(c, needle);
}

static void sse_hangup(sse_client *c) {
  if (c->fd >= 0) {
    close(c->fd);
    c->fd = -1;
  }
}

/* ------------------------------------------ SSE attach vs the event fan-out --
 *
 * httpd_track puts a connection in h->all_conns at ACCEPT time, and
 * httpd_on_event walks that array on every published event looking for
 * c->sse. sse_attach used to set c->sse first and build the frame ring after
 * it, so a publisher landing in that window took the SSE branch of the
 * fan-out on a connection whose ring was still q == NULL / qcap == 0:
 * sse_push_locked computed `% c->qcap` and dereferenced the NULL ring, and
 * the daemon died of SIGFPE on a thread that had done nothing wrong.
 *
 * What this test can and cannot prove, stated plainly: the interleaving is a
 * C data race rather than a scheduling accident, and on a TSO target it is
 * not reachable — the store that publishes c->sse cannot be observed before
 * the store that fills c->q, and the compiler will not move a store across
 * the calloc. So this does NOT fail on x86-64 when the fix is reverted;
 * eight runs of a reverted build all pass. What it does is keep the shape
 * exercised at volume — thousands of streams opened against threads that
 * publish continuously (every reindex publishes index.updated) — and a
 * fan-out that reached a half-built ring would take the whole test process
 * down, which ctest reports. On a weak-ordering target it is the detector.
 * Read a green run as "not disproven", not as proof. */

#define RACE_STREAMS 8
#define RACE_ROUNDS 200
#define RACE_PUBLISHERS 2

typedef struct {
  int port;
  int rounds;
  pthread_barrier_t *start;
  int opened;
  int bad;
} stream_arg;

typedef struct {
  int port;
  int rounds;
  pthread_barrier_t *start;
  int published;
  int bad;
} publish_arg;

static void *race_stream(void *p) {
  stream_arg *a = (stream_arg *)p;
  pthread_barrier_wait(a->start);
  for (int i = 0; i < a->rounds; i++) {
    sse_client c;
    if (!sse_open(a->port, &c) || !sse_wait(&c, ":ok", 8)) {
      a->bad++;
    } else if (strncmp(c.buf, "HTTP/1.1 200", 12) == 0 &&
               strstr(c.buf, "text/event-stream") != NULL) {
      a->opened++;
    } else {
      a->bad++;
    }
    sse_hangup(&c);
  }
  return NULL;
}

static void *race_publisher(void *p) {
  publish_arg *a = (publish_arg *)p;
  static const char req[] = "POST /api/reindex HTTP/1.1\r\nHost: x\r\n\r\n";
  char reply[8192];
  pthread_barrier_wait(a->start);
  for (int i = 0; i < a->rounds; i++) {
    int status = 0;
    bool ok = raw_exchange_port(a->port, req, sizeof req - 1, &status, reply,
                                sizeof reply);
    if (ok && status == 202) {
      a->published++;
    } else {
      a->bad++;
    }
  }
  return NULL;
}

KBC_TEST(sse_attach_racing_the_event_fan_out_keeps_the_daemon) {
  fixture f;
  fx_setup(&f, NULL);
  free(f.cfg->bind_addr);
  f.cfg->bind_addr = strdup("127.0.0.1");
  f.cfg->port = 0;
  /* The attach and the publish must be on DIFFERENT threads for the race to
   * exist at all, and with SO_REUSEPORT the kernel only spreads them across
   * workers when there is more than one. */
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

  pthread_barrier_t start;
  KBC_CHECK(pthread_barrier_init(&start, NULL,
                                 (unsigned)(RACE_STREAMS + RACE_PUBLISHERS)) ==
            0);
  pthread_t th[RACE_STREAMS + RACE_PUBLISHERS];
  stream_arg sa[RACE_STREAMS];
  publish_arg pa[RACE_PUBLISHERS];
  for (int i = 0; i < RACE_STREAMS; i++) {
    memset(&sa[i], 0, sizeof sa[i]);
    sa[i].port = port;
    sa[i].rounds = RACE_ROUNDS;
    sa[i].start = &start;
    KBC_CHECK(pthread_create(&th[i], NULL, race_stream, &sa[i]) == 0);
  }
  for (int i = 0; i < RACE_PUBLISHERS; i++) {
    memset(&pa[i], 0, sizeof pa[i]);
    pa[i].port = port;
    pa[i].rounds = RACE_ROUNDS;
    pa[i].start = &start;
    KBC_CHECK(pthread_create(&th[RACE_STREAMS + i], NULL, race_publisher,
                             &pa[i]) == 0);
  }
  int opened = 0, bad_stream = 0, published = 0, bad_publish = 0;
  for (int i = 0; i < RACE_STREAMS; i++) {
    (void)pthread_join(th[i], NULL);
    opened += sa[i].opened;
    bad_stream += sa[i].bad;
  }
  for (int i = 0; i < RACE_PUBLISHERS; i++) {
    (void)pthread_join(th[RACE_STREAMS + i], NULL);
    published += pa[i].published;
    bad_publish += pa[i].bad;
  }
  (void)pthread_barrier_destroy(&start);

  KBC_CHECK_MSG(bad_stream == 0, "%d of %d streams never got a well-formed SSE "
                "head while the fan-out was publishing", bad_stream,
                RACE_STREAMS * RACE_ROUNDS);
  KBC_CHECK_MSG(opened == RACE_STREAMS * RACE_ROUNDS,
                "only %d of %d streams completed", opened,
                RACE_STREAMS * RACE_ROUNDS);
  KBC_CHECK_MSG(bad_publish == 0, "%d of %d reindexes got no 202", bad_publish,
                RACE_PUBLISHERS * RACE_ROUNDS);

  /* The daemon survived the storm and still answers: a fan-out that touched a
   * half-built ring shows up here as a dead listener, not as a bad count. */
  char reply[8192];
  int status = 0;
  static const char health[] = "GET /api/health HTTP/1.1\r\nHost: x\r\n\r\n";
  KBC_CHECK(
      raw_exchange_port(port, health, sizeof health - 1, &status, reply,
                        sizeof reply));
  KBC_CHECK_MSG(status == 200, "the daemon did not survive the storm: %d",
                status);
  KBC_CHECK_MSG(published > 0, "the storm published nothing, so it proved "
                "nothing");

  kbc_httpd_stop(h);
  fx_teardown(&f);
}

/* ------------------------------------------------- the SSE gap/lag probe --
 *
 * This is not an invention: the original emits two synthetic frames from
 * `events_stream` (`kb-core/src/events.rs:190-204, 214-277`), rendered by
 * `routes/events.rs:139-153`. `gap` fires when the client's Last-Event-ID is
 * a position the ring cannot serve; `lag` fires when a consumer fell behind
 * and the broadcast dropped events for it. Both carry NO `id:` line. */

/* Starts a daemon on a loopback port the kernel picked. */
static int probe_server(fixture *f, kbc_httpd **out) {
  free(f->cfg->bind_addr);
  f->cfg->bind_addr = strdup("127.0.0.1");
  f->cfg->port = 0;
  f->cfg->http_workers = 1;
  kbc_err err;
  kbc_err_reset(&err);
  *out = kbc_httpd_start(f->app, f->cfg, &err);
  if (*out == NULL) {
    fprintf(stderr, "  httpd_start: %s\n", err.msg);
    return 0;
  }
  return kbc_httpd_port(*out);
}

KBC_TEST(sse_gap_probe_replaces_the_replay_it_cannot_honour) {
  fixture f;
  fx_setup(&f, NULL);
  kbc_httpd *h = NULL;
  int port = probe_server(&f, &h);
  if (h == NULL) {
    fx_teardown(&f);
    return;
  }
  /* Three events, so the ring holds ids 1..3 and nothing has been evicted. */
  for (int i = 0; i < 3; i++) {
    kbc_app_publish(f.app, "probe.event", "{\"n\":1}");
  }

  /* A cursor the ring CAN serve replays. This is the control: without it the
   * assertions below would also pass on a daemon that never replays at all. */
  {
    sse_client c;
    KBC_CHECK(sse_open_head(port, "1", 0, &c));
    KBC_CHECK(sse_wait(&c, "id: 3", 16));
    KBC_CHECK_MSG(sse_saw(&c, "id: 2") && sse_saw(&c, "id: 3"),
                  "a serviceable cursor did not replay: %s", c.buf);
    KBC_CHECK_MSG(!sse_saw(&c, "event: gap"),
                  "a serviceable cursor was probed as gapped: %s", c.buf);
    sse_hangup(&c);
  }

  /* A cursor AHEAD of everything this process ever assigned — the shape a
   * client holds after the daemon restarts, since ids restart at 1. */
  {
    sse_client c;
    KBC_CHECK(sse_open_head(port, "99999", 0, &c));
    KBC_CHECK(sse_wait(&c, "event: gap", 16));
    KBC_CHECK_MSG(sse_saw(&c, "\"requested_id\":99999"),
                  "the gap did not name the cursor: %s", c.buf);
    /* oldest_available_id is 0: three events fit in the ring, so nothing has
     * been evicted and the ahead-of-daemon case is the only one in play. */
    KBC_CHECK_MSG(sse_saw(&c, "\"oldest_available_id\":0"),
                  "the gap did not name the ring's oldest id: %s", c.buf);
    /* The probe describes the stream, it is not IN it. An `id:` between the
     * event line and the data line would make a reconnecting client treat the
     * probe as the newest event and skip everything after it. */
    KBC_CHECK_MSG(sse_saw(&c, "event: gap\ndata: "),
                  "the gap probe carried an id: line: %s", c.buf);
    /* And the replay it cannot honour is SUPPRESSED, not sent alongside. */
    KBC_CHECK_MSG(!sse_saw(&c, "id: 2") && !sse_saw(&c, "id: 3"),
                  "a gapped client was replayed the ring anyway: %s", c.buf);
    sse_hangup(&c);
  }

  /* A COLD client has no cursor to have fallen behind from, so it is not
   * probed. The original checks `last_id > 0` before anything else
   * (`events.rs:232-233`), and probing cold clients would hand every
   * EventSource a spurious gap. */
  {
    sse_client c;
    KBC_CHECK(sse_open_head(port, NULL, 0, &c));
    KBC_CHECK(sse_wait(&c, ":ok", 16));
    KBC_CHECK_MSG(!sse_saw(&c, "event: gap"),
                  "a cursorless client was probed: %s", c.buf);
    sse_hangup(&c);
  }

  kbc_httpd_stop(h);
  fx_teardown(&f);
}

KBC_TEST(sse_lag_probe_tells_a_stalled_client_how_far_behind_it_is) {
  fixture f;
  fx_setup(&f, NULL);
  kbc_httpd *h = NULL;
  int port = probe_server(&f, &h);
  if (h == NULL) {
    fx_teardown(&f);
    return;
  }
  sse_client c;
  KBC_CHECK(sse_open_head(port, NULL, 128, &c));
  KBC_CHECK(sse_wait(&c, ":ok", 16));
  /* A short per-recv deadline, unlike the five seconds the other streams use:
   * the check below polls often, and a long deadline would turn each empty
   * poll into a second of dead time. */
  {
    struct timeval quick;
    quick.tv_sec = 1;
    quick.tv_usec = 0;
    (void)setsockopt(c.fd, SOL_SOCKET, SO_RCVTIMEO, &quick, sizeof quick);
  }
  /* Publish in chunks, checking for the probe between them, rather than
   * firing one fixed burst and hoping.
   *
   * The earlier version published 400 events and then waited. That races the
   * worker: EPOLLOUT is level-triggered, so once the socket is writable the
   * worker spins, drains the whole queue in one sse_pump and goes back to
   * sleep — and on a many-core box it sometimes kept up with the publisher
   * well enough that the 64-slot queue never filled. Two runs in ten failed
   * with no bug present. Chunking removes the race instead of tuning around
   * it: if the worker kept up this round, another round follows, and the only
   * way out of the loop without a probe is a daemon that never emits one.
   *
   * The 128-byte receive buffer keeps the stall real between checks — the
   * kernel cannot absorb the burst, so the worker's flush blocks and the queue
   * starts shedding its oldest entries. */
  bool saw_lag = false;
  for (int chunk = 0; chunk < 60 && !saw_lag; chunk++) {
    for (int i = 0; i < 200; i++) {
      kbc_app_publish(f.app, "probe.lag", "{\"n\":1}");
    }
    for (int p = 0; p < 4 && !saw_lag; p++) {
      (void)sse_poll(&c);
      if (sse_saw(&c, "event: lag")) saw_lag = true;
    }
  }
  KBC_CHECK_MSG(saw_lag, "a client that fell behind was never told: %s", c.buf);
  /* The count is the point: a probe with no number is a shrug, and the client
   * still cannot tell a one-event slip from a four-hundred-event one. */
  {
    const char *sk = strstr(c.buf, "\"skipped\":");
    KBC_CHECK_MSG(sk != NULL, "the lag carried no count: %s", c.buf);
    if (sk != NULL) {
      long skipped = strtol(sk + 10, NULL, 10);
      KBC_CHECK_MSG(skipped > 0, "the lag reported nothing dropped: %s", c.buf);
    }
  }
  /* Same reason as the gap probe: no id, so it cannot be mistaken for an
   * event and cannot advance a reconnecting cursor past the frames it
   * describes. */
  KBC_CHECK_MSG(sse_saw(&c, "event: lag\ndata: "),
                "the lag probe carried an id: line: %s", c.buf);
  sse_hangup(&c);

  kbc_httpd_stop(h);
  fx_teardown(&f);
}

/* ------------------------------------- a refused start must not close fd 0 --
 *
 * kbc_httpd is calloc'd, so an untouched listen_fd[] / w[i].epfd slot reads as
 * 0 — a perfectly valid descriptor. kbc_httpd_stop closes everything that
 * reads >= 0, so a failure path that runs BEFORE those slots are swept to -1
 * tears the daemon down with the caller's descriptor 0 in them, and the
 * caller loses its stdin once per configured worker. Three paths run before
 * the sweep: the CORS allocation, the CORS list push, and an unparseable
 * KBC_RATE_LIMIT_RPS. Only the last is reachable from a test — the other two
 * need malloc to fail — so it is the one driven here, and the assertion is
 * about the descriptor rather than about the error message. */

KBC_TEST(a_refused_start_leaves_the_callers_descriptors_open) {
  static const char *const bad[] = {"many", "12x", "99999999999999999999999"};
  fixture f;
  fx_setup(&f, NULL);
  free(f.cfg->bind_addr);
  f.cfg->bind_addr = strdup("127.0.0.1");
  f.cfg->port = 0;
  f.cfg->http_workers = 4; /* stop closes one descriptor per worker */

  for (size_t i = 0; i < sizeof bad / sizeof *bad; i++) {
    /* Install a known-good descriptor 0 first, so what is asserted below is
     * this failure path and not whatever an earlier case left behind. */
    int null = open("/dev/null", O_RDONLY);
    KBC_CHECK_MSG(null >= 0, "cannot open /dev/null");
    if (null < 0) break;
    if (null != 0) {
      bool moved = dup2(null, 0) == 0;
      close(null);
      KBC_CHECK_MSG(moved, "cannot install a guard on descriptor 0");
    }
    KBC_CHECK(setenv("KBC_RATE_LIMIT_RPS", bad[i], 1) == 0);
    kbc_err err;
    kbc_err_reset(&err);
    kbc_httpd *h = kbc_httpd_start(f.app, f.cfg, &err);
    KBC_CHECK_MSG(h == NULL, "KBC_RATE_LIMIT_RPS=\"%s\" was accepted", bad[i]);
    if (h != NULL) kbc_httpd_stop(h);
    KBC_CHECK_ERR_MSG(err);
    KBC_CHECK_MSG(fcntl(0, F_GETFD) != -1,
                  "a refused start (KBC_RATE_LIMIT_RPS=\"%s\") closed the "
                  "caller's descriptor 0", bad[i]);
  }
  (void)unsetenv("KBC_RATE_LIMIT_RPS");
  fx_teardown(&f);
}

/* ------------------------------------------------------- the serving surface */

/* The corpus root the artifact serve is bounded by, and everything reachable
 * only from OUTSIDE it. The sibling directory `<root>-secret` is the case that
 * separates a component-wise containment check from a byte-prefix one: it
 * shares every byte of the root's name and none of its components. */
#define OUT_IN_ROOT "IN-ROOT-BYTES"
#define OUT_SIBLING "OUTSIDE-SIBLING-SECRET"
#define OUT_ESCAPE "OUTSIDE-ROOT-SECRET"

typedef struct {
  fixture f;
  char sub[KBC_TEST_PATH_MAX];   /* <root>/kb/sub — the asset base */
  char secret_dir[KBC_TEST_PATH_MAX]; /* <root>/kb-secret */
  char outside[KBC_TEST_PATH_MAX];    /* <root>/outside */
  char id[KBC_MAX_ID_LEN + 1];  /* the id of kb/sub/one.md */
} origin_fixture;

/* Assembled with a length check, never snprintf: the build runs
 * -Werror=format-truncation, and a 4096-byte root plus a leaf is a warning no
 * matter how carefully the sizes are written. A silently truncated path in a
 * traversal test would assert the wrong thing and pass. */
static void path_under(char *buf, size_t cap, const char *root,
                       const char *rel) {
  size_t rl = strlen(root), ll = strlen(rel);
  if (rl + 1 + ll + 1 > cap) {
    kbc_test_fail(__FILE__, __LINE__, "%s/%s does not fit in %zu bytes", root,
                  rel, cap);
    buf[0] = '\0';
    return;
  }
  memcpy(buf, root, rl);
  buf[rl] = '/';
  memcpy(buf + rl + 1, rel, ll + 1);
}

/* Adds the origin layout on top of the standard fixture and re-indexes, so the
 * artifact ids in the store match the files the traversal cases create. */
static void ofx_setup(origin_fixture *o) {
  fx_setup(&o->f, NULL);
  path_under(o->sub, sizeof o->sub, o->f.root, "kb/sub");
  path_under(o->secret_dir, sizeof o->secret_dir, o->f.root, "kb-secret");
  path_under(o->outside, sizeof o->outside, o->f.root, "outside");
  char p[KBC_TEST_PATH_MAX];
  kbc_test_mkdir_p(o->sub);
  kbc_test_mkdir_p(o->secret_dir);
  kbc_test_mkdir_p(o->outside);
  path_under(p, sizeof p, o->sub, "one.md");
  kbc_test_write_file(p, "# One\n\nsub artifact\n");
  /* A legitimate sibling asset, INSIDE the root: the guard must not refuse
   * this, or "refuse everything" would pass every escape test. */
  path_under(p, sizeof p, o->sub, "sibling.txt");
  kbc_test_write_file(p, OUT_IN_ROOT);
  path_under(p, sizeof p, o->secret_dir, "secret.txt");
  kbc_test_write_file(p, OUT_SIBLING);
  path_under(p, sizeof p, o->outside, "secret.txt");
  kbc_test_write_file(p, OUT_ESCAPE);
  /* Two escapes, both symlinks because that is the only way a canonicalised
   * path under a probed base can leave the root: one into the SIBLING
   * directory (the byte-prefix trap) and one into an unrelated directory. */
  char target[KBC_TEST_PATH_MAX];
  path_under(p, sizeof p, o->sub, "into-sibling");
  path_under(target, sizeof target, o->secret_dir, "secret.txt");
  KBC_CHECK(symlink(target, p) == 0);
  path_under(p, sizeof p, o->sub, "escape.txt");
  path_under(target, sizeof target, o->outside, "secret.txt");
  KBC_CHECK(symlink(target, p) == 0);

  kbc_err err;
  kbc_err_reset(&err);
  KBC_CHECK_OK(kbc_app_reindex(o->f.app, &err));

  /* The id of sub/one.md, found through the public listing rather than
   * recomputed: the test must not re-implement the id minting to find its
   * own fixture. */
  kbc_response r;
  KBC_CHECK_EQ_INT(call(&o->f, "GET", "/api/artifacts", AT0, NULL, &r), 200);
  o->id[0] = '\0';
  {
    /* The id that goes WITH the path, not the first one in the list: walking
     * the objects and remembering the most recent id is the only way to pair
     * them without re-implementing the JSON shape. */
    const char *cur = NULL;
    for (const char *q = r.body.ptr; *q != '\0'; q++) {
      if (strncmp(q, "\"id\":\"", 6) == 0) cur = q + 6;
      if (strncmp(q, "\"path\":\"sub/one.md\"", 19) == 0 && cur != NULL) {
        size_t i = 0;
        while (cur[i] != '\0' && cur[i] != '"' && i < KBC_MAX_ID_LEN) {
          o->id[i] = cur[i];
          i++;
        }
        o->id[i] = '\0';
        break;
      }
    }
  }
  kbc_response_free(&r);
  KBC_CHECK_MSG(o->id[0] != '\0', "no id found for kb/sub/one.md: %s", "");
}

static void ofx_teardown(origin_fixture *o) { fx_teardown(&o->f); }

/* One request on the artifact subdomain, spelled out raw so the Host is
 * exactly what the test means it to be. */
static int origin_get(server *s, const char *host, const char *path,
                      const char *query, char *reply, size_t cap) {
  kbc_str req;
  kbc_str_init(&req);
  (void)kbc_str_printf(&req, "GET %s%s HTTP/1.1\r\nHost: %s\r\n\r\n", path,
                       query != NULL ? query : "", host);
  int status = 0;
  bool ok = raw_exchange(s, req.ptr, req.len, &status, reply, cap);
  kbc_str_free(&req);
  KBC_CHECK_MSG(ok, "no HTTP reply for %s %s", path, host);
  return status;
}

/* The traversal suite. Every case asserts the same two things: a 4xx, and no
 * byte of anything outside the source root in the answer. The sibling case is
 * the one that fails for a `strncmp(path, root, strlen(root))` guard — a
 * `../` case does not, because strncmp rejects `../` too. */
KBC_TEST(no_path_outside_the_source_root_is_ever_served) {
  origin_fixture o;
  ofx_setup(&o);
  server s;
  srv_start(&s, &o.f);
  if (s.h == NULL || o.id[0] == '\0') {
    srv_stop(&s);
    ofx_teardown(&o);
    return;
  }
  char host[KBC_TEST_PATH_MAX];
  snprintf(host, sizeof host, "%s.artifacts.localhost", o.id);
  char reply[8192];
  int status = 0;

  /* The control: a sibling asset inside the root IS served, byte for byte. A
   * guard that refuses everything would pass every case below. */
  status = origin_get(&s, host, "/sibling.txt", NULL, reply, sizeof reply);
  KBC_CHECK_EQ_INT(status, 200);
  KBC_CHECK_MSG(strstr(reply, OUT_IN_ROOT) != NULL,
                "an in-root sibling asset is not served: %s", reply);

  /* 1. SIBLING DIRECTORY. `/into-sibling` canonicalises to <root>-secret/
   * secret.txt, which shares every byte of the root's name. A byte-prefix
   * containment check accepts it and serves the file; the component-wise one
   * refuses it as path traversal. */
  status = origin_get(&s, host, "/into-sibling", NULL, reply, sizeof reply);
  KBC_CHECK_MSG(status != 200, "the sibling directory was served: %d %s",
                status, reply);
  KBC_CHECK_MSG(status == 400 || status == 404, "expected 400 or 404, got %d",
                status);
  KBC_CHECK_MSG(strstr(reply, OUT_SIBLING) == NULL,
                "a byte outside the root was served: %s", reply);
  KBC_CHECK_MSG(strstr(reply, "\"type\":\"urn:kb:errors:bad-request\"") != NULL,
                "a resolved-outside-root path is not a problem+json 400: %s",
                reply);

  /* 2. SYMLINK OUT OF THE ROOT. The e2e assertion in the original: a symlink
   * that escapes must be 400 (kb-server/tests/end_to_end.rs:4891-4897). */
  status = origin_get(&s, host, "/escape.txt", NULL, reply, sizeof reply);
  KBC_CHECK_EQ_INT(status, 400);
  KBC_CHECK_MSG(strstr(reply, OUT_ESCAPE) == NULL,
                "a symlink escape served host bytes: %s", reply);

  /* 3. ABSOLUTE PATH. Not found under the root, and the walk-up never climbs
   * past it, so it is a miss and never a read of /etc/passwd. */
  status = origin_get(&s, host, "/etc/passwd", NULL, reply, sizeof reply);
  KBC_CHECK_MSG(status == 400 || status == 404,
                "an absolute path must be 400 or 404, got %d: %s", status,
                reply);
  KBC_CHECK_MSG(strstr(reply, "root:") == NULL,
                "an absolute path served /etc/passwd: %s", reply);

  /* 4. ".." IN A SEGMENT, plain and percent-encoded. The request parser
   * refuses both before the router, so the guard is never reached — and the
   * point is that the answer is a refusal either way
   * (kb-server/tests/end_to_end.rs:3459-3467). */
  {
    const char *dots = "GET /../outside/secret.txt HTTP/1.1\r\nHost: ";
    kbc_str req;
    kbc_str_init(&req);
    (void)kbc_str_puts(&req, dots);
    (void)kbc_str_puts(&req, host);
    (void)kbc_str_puts(&req, "\r\n\r\n");
    KBC_CHECK(raw_exchange(&s, req.ptr, req.len, &status, reply,
                           sizeof reply));
    KBC_CHECK_MSG(status >= 400 && status < 500, "\"..\" answered %d", status);
    KBC_CHECK_MSG(strstr(reply, OUT_ESCAPE) == NULL,
                  "\"..\" served a byte outside the root: %s", reply);
    kbc_str_free(&req);
  }
  {
    const char *enc = "GET /%2e%2e/outside/secret.txt HTTP/1.1\r\nHost: ";
    kbc_str req;
    kbc_str_init(&req);
    (void)kbc_str_puts(&req, enc);
    (void)kbc_str_puts(&req, host);
    (void)kbc_str_puts(&req, "\r\n\r\n");
    KBC_CHECK(raw_exchange(&s, req.ptr, req.len, &status, reply,
                           sizeof reply));
    KBC_CHECK_MSG(status >= 400 && status < 500,
                  "an encoded \"..\" answered %d", status);
    KBC_CHECK_MSG(strstr(reply, OUT_ESCAPE) == NULL,
                  "an encoded \"..\" served a byte outside the root: %s",
                  reply);
    kbc_str_free(&req);
  }

  srv_stop(&s);
  ofx_teardown(&o);
}

/* The subdomain is selected by Host, and only by Host: the same path on the
 * parent origin is the banner, on an artifact host it is the artifact. */
KBC_TEST(the_artifact_subdomain_is_chosen_by_host) {
  origin_fixture o;
  ofx_setup(&o);
  server s;
  srv_start(&s, &o.f);
  if (s.h == NULL || o.id[0] == '\0') {
    srv_stop(&s);
    ofx_teardown(&o);
    return;
  }
  char host[KBC_TEST_PATH_MAX];
  snprintf(host, sizeof host, "%s.artifacts.localhost", o.id);
  char reply[8192];

  /* `/` on the artifact host is the artifact itself. */
  int status = origin_get(&s, host, "/", NULL, reply, sizeof reply);
  KBC_CHECK_EQ_INT(status, 200);
  KBC_CHECK_MSG(strstr(reply, "sub artifact") != NULL,
                "the artifact subdomain did not serve the artifact: %s", reply);
  char want[128];
  snprintf(want, sizeof want, "X-Kb-Artifact-Id: %s", o.id);
  KBC_CHECK_MSG(strstr(reply, want) != NULL,
                "no X-Kb-Artifact-Id for the served artifact: %s", reply);
  KBC_CHECK_MSG(strstr(reply, "Vary: Accept, Cookie") != NULL,
                "no Vary on a plain artifact serve: %s", reply);
  KBC_CHECK_MSG(strstr(reply, "Cache-Control: private, no-store") == NULL,
                "a plain artifact serve is not no-store: %s", reply);
  /* The default parent origin is the "not configured for production"
   * sentinel, and the original sends NO frame-ancestors CSP for it
   * (artifact.rs:36-49). Emitting one anyway would break the dev flows the
   * sentinel exists for. */
  KBC_CHECK_MSG(strstr(reply, "Content-Security-Policy") == NULL,
                "a default-dev subdomain response carries a frame-ancestors "
                "CSP: %s",
                reply);

  /* ?cm=on is the per-user comment payload: private, no-store, and Vary
   * widened to Authorization so no proxy hands it to another user. */
  status = origin_get(&s, host, "/", "?cm=on", reply, sizeof reply);
  KBC_CHECK_EQ_INT(status, 200);
  KBC_CHECK_MSG(strstr(reply, "Cache-Control: private, no-store") != NULL,
                "?cm=on is not private, no-store: %s", reply);
  KBC_CHECK_MSG(strstr(reply, "Vary: Authorization, Accept, Cookie") != NULL,
                "?cm=on does not widen Vary to Authorization: %s", reply);

  /* A configured parent origin turns the CSP on, and it is the LAST header the
   * handler sets. */
  KBC_CHECK(setenv("KBC_PARENT_ORIGIN", "https://kb.example.com", 1) == 0);
  {
    server s2;
    srv_start(&s2, &o.f);
    if (s2.h != NULL) {
      status = origin_get(&s2, host, "/", NULL, reply, sizeof reply);
      KBC_CHECK_EQ_INT(status, 200);
      KBC_CHECK_MSG(
          strstr(reply, "Content-Security-Policy: frame-ancestors "
                        "https://kb.example.com;") != NULL,
          "a configured parent origin sends no frame-ancestors CSP: %s", reply);
      srv_stop(&s2);
    }
  }
  (void)unsetenv("KBC_PARENT_ORIGIN");

  /* An unresolvable label is a 404, and the parent origin is never an
   * artifact: `Host: kb.artifacts.localhost` is not a 12-hex id, so it must
   * not resolve to anything. */
  status = origin_get(&s, "notanid.artifacts.localhost", "/", NULL, reply,
                      sizeof reply);
  KBC_CHECK_EQ_INT(status, 404);
  KBC_CHECK_MSG(strstr(reply, "artifact not found") != NULL,
                "an unresolvable label is not 404 artifact-not-found: %s",
                reply);

  /* A host that is not an artifact subdomain is the parent origin, and `/`
   * there is the API banner — the split is a branch, not a takeover. */
  status = origin_get(&s, "kb.example.com", "/", NULL, reply, sizeof reply);
  KBC_CHECK_EQ_INT(status, 200);
  KBC_CHECK_MSG(strstr(reply, "self-hosted system of record") != NULL,
                "the parent origin did not answer with its banner: %s", reply);

  /* An unmatched /api path is never the subdomain: the api tree answers it. */
  {
    const char *req = "GET /api/health HTTP/1.1\r\nHost: ";
    kbc_str r2;
    kbc_str_init(&r2);
    (void)kbc_str_puts(&r2, req);
    (void)kbc_str_puts(&r2, host);
    (void)kbc_str_puts(&r2, "\r\n\r\n");
    KBC_CHECK(raw_exchange(&s, r2.ptr, r2.len, &status, reply, sizeof reply));
    KBC_CHECK_EQ_INT(status, 200);
    KBC_CHECK_MSG(strstr(reply, "\"status\":\"ok\"") != NULL,
                  "an artifact host hijacked an /api route: %s", reply);
    kbc_str_free(&r2);
  }

  srv_stop(&s);
  ofx_teardown(&o);
}

/* The parent-origin artifact route: the header set and its order, the status
 * codes, and the download disposition. */
KBC_TEST(artifact_bytes_on_the_parent_origin) {
  origin_fixture o;
  ofx_setup(&o);
  server s;
  srv_start(&s, &o.f);
  if (s.h == NULL || o.id[0] == '\0') {
    srv_stop(&s);
    ofx_teardown(&o);
    return;
  }
  char path[256];
  char reply[8192];
  int status = 0;

  snprintf(path, sizeof path, "/api/kb/kb/artifact/%s", o.id);
  {
    kbc_str req;
    kbc_str_init(&req);
    (void)kbc_str_printf(&req, "GET %s HTTP/1.1\r\nHost: x\r\n\r\n", path);
    KBC_CHECK(raw_exchange(&s, req.ptr, req.len, &status, reply, sizeof reply));
    kbc_str_free(&req);
  }
  KBC_CHECK_EQ_INT(status, 200);
  KBC_CHECK_MSG(strstr(reply, "sub artifact") != NULL,
                "the artifact bytes were not served: %s", reply);
  KBC_CHECK_MSG(strstr(reply, "Content-Type: text/html; charset=utf-8") != NULL,
                "artifact bytes are not text/html: %s", reply);
  KBC_CHECK_MSG(strstr(reply, "X-Content-Type-Options: nosniff") != NULL,
                "artifact bytes carry no nosniff: %s", reply);
  /* The CSP value is the whole policy — literally `sandbox`, nothing else. */
  KBC_CHECK_MSG(strstr(reply, "Content-Security-Policy: sandbox\r\n") != NULL,
                "artifact bytes carry no bare `sandbox` CSP: %s", reply);
  {
    char want[128];
    snprintf(want, sizeof want, "X-Kb-Artifact-Id: %s", o.id);
    KBC_CHECK_MSG(strstr(reply, want) != NULL,
                  "artifact bytes carry no X-Kb-Artifact-Id: %s", reply);
  }
  KBC_CHECK_MSG(strstr(reply, "Content-Disposition") == NULL,
                "an attachment was offered without ?download=1: %s", reply);

  /* ?download=1 attaches, and a markdown source is offered as .html because
   * the body is served as HTML. */
  {
    kbc_str req;
    kbc_str_init(&req);
    (void)kbc_str_printf(&req, "GET %s?download=1 HTTP/1.1\r\nHost: x\r\n\r\n",
                         path);
    KBC_CHECK(raw_exchange(&s, req.ptr, req.len, &status, reply, sizeof reply));
    kbc_str_free(&req);
  }
  KBC_CHECK_EQ_INT(status, 200);
  KBC_CHECK_MSG(strstr(reply, "Content-Disposition: attachment;") != NULL,
                "?download=1 does not attach: %s", reply);
  KBC_CHECK_MSG(strstr(reply, "filename=\"one.html\"") != NULL,
                "a markdown download is not offered as .html: %s", reply);

  /* Status codes. An unknown corpus and an unknown id are both 404; a
   * malformed id is 400 — the shape is checked before the lookup. */
  {
    kbc_response r;
    char p2[256];
    snprintf(p2, sizeof p2, "/api/kb/nosuchkb/artifact/%s", o.id);
    KBC_CHECK_EQ_INT(call(&o.f, "GET", p2, NULL, NULL, &r), 404);
    kbc_response_free(&r);
    snprintf(p2, sizeof p2, "/api/kb/kb/artifact/000000000000");
    KBC_CHECK_EQ_INT(call(&o.f, "GET", p2, NULL, NULL, &r), 404);
    kbc_response_free(&r);
    snprintf(p2, sizeof p2, "/api/kb/kb/artifact/xyz");
    KBC_CHECK_EQ_INT(call(&o.f, "GET", p2, NULL, NULL, &r), 400);
    kbc_response_free(&r);
    /* A slash in the id position is a different route, not a lookup. */
    KBC_CHECK_EQ_INT(call(&o.f, "GET", "/api/kb/kb/artifact/aa/bb", NULL, NULL,
                          &r),
                     404);
    kbc_response_free(&r);
    /* A method that is not GET is 405, not a silent 200. */
    KBC_CHECK_EQ_INT(call(&o.f, "POST", path, NULL, NULL, &r), 405);
    kbc_response_free(&r);
  }

  srv_stop(&s);
  ofx_teardown(&o);
}

/* The Prometheus text endpoint. The shape is the contract: a scraper that
 * cannot parse this sees no metrics at all, whatever the numbers are. */
KBC_TEST(metrics_is_prometheus_text_exposition) {
  fixture f;
  fx_setup(&f, NULL);
  kbc_response r;
  KBC_CHECK_EQ_INT(call(&f, "GET", "/metrics", NULL, NULL, &r), 200);
  KBC_CHECK_EQ_STR(r.content_type, "text/plain; version=0.0.4; charset=utf-8");
  const char *b = r.body.ptr;

  /* A counter: HELP, TYPE, then the sample. */
  KBC_CHECK_MSG(strstr(b, "# HELP kb_http_requests_total HTTP requests since "
                          "daemon boot (GET /api/metrics requests_total).\n"
                          "# TYPE kb_http_requests_total counter\n"
                          "kb_http_requests_total 0\n") != NULL,
                "the counter family is not HELP+TYPE+sample: %s", b);
  /* A gauge. */
  KBC_CHECK_MSG(strstr(b, "# TYPE kb_storage_channel_depth gauge\n"
                          "kb_storage_channel_depth 0\n") != NULL,
                "the gauge family is not TYPE+sample: %s", b);
  /* The latency "histogram" is NOT a Prometheus histogram: it is a counter
   * with `le_ms` as an ordinary label, non-cumulative, with the overflow slot
   * labelled `+Inf`. Rendering `_sum`/`_count`/cumulative `_bucket{le=}` here
   * would be the obvious thing and is wrong — a scraper reads a histogram's
   * buckets as a CDF, and these are not one. */
  KBC_CHECK_MSG(strstr(b, "# TYPE kb_route_latency_bucket counter\n") != NULL,
                "kb_route_latency_bucket is not typed counter: %s", b);
  KBC_CHECK_MSG(strstr(b, "kb_route_latency_bucket{route=\"search\",le_ms="
                          "\"1\"} 0\n") != NULL,
                "the first latency bucket line is wrong: %s", b);
  KBC_CHECK_MSG(strstr(b, "kb_route_latency_bucket{route=\"search\",le_ms="
                          "\"10000\"} 0\n") != NULL,
                "the last bounded bucket line is wrong: %s", b);
  KBC_CHECK_MSG(strstr(b, "kb_route_latency_bucket{route=\"search\",le_ms="
                          "\"+Inf\"} 0\n") != NULL,
                "the overflow bucket is not labelled +Inf: %s", b);
  KBC_CHECK_MSG(strstr(b, "_sum") == NULL && strstr(b, "_count{") == NULL,
                "the latency series grew a _sum/_count: %s", b);
  /* A real Prometheus histogram writes `{le="1"}`; this one writes
   * `{route="search",le_ms="1"}`. `quantile="` also ENDS in `le="`, so the
   * check is for the label START, not for the suffix. */
  KBC_CHECK_MSG(strstr(b, "{le=\"") == NULL && strstr(b, ",le=\"") == NULL &&
                   strstr(b, "TYPE kb_route_latency_bucket histogram") == NULL,
                "the latency series is being rendered as a real histogram: %s",
                b);
  /* All nine route families render, in enum order, even with nothing
   * observed — a family that appears only once it has a value is a family a
   * dashboard cannot rely on. */
  for (size_t i = 0; i < 9; i++) {
    char pat[128];
    snprintf(pat, sizeof pat, "kb_route_requests_total{route=\"%s\"} 0\n",
             i == 0 ? "search" : i == 1 ? "atlas" : i == 2   ? "history"
             : i == 3 ? "review" : i == 4 ? "events" : i == 5 ? "read"
             : i == 6 ? "ops" : i == 7   ? "sessions"
                                      : "other");
    KBC_CHECK_MSG(strstr(b, pat) != NULL, "missing route family line %s", pat);
  }
  /* The percentile gauges: {route,quantile} for this family, and the value is
   * the spanning bucket's UPPER boundary, 0 on an empty histogram. */
  KBC_CHECK_MSG(strstr(b, "kb_route_latency_ms{route=\"search\",quantile="
                          "\"0.5\"} 0\n") != NULL,
                "the p50 line is wrong: %s", b);
  KBC_CHECK_MSG(strstr(b, "kb_route_latency_ms{route=\"other\",quantile="
                          "\"0.99\"} 0\n") != NULL,
                "the p99 line for the last route is wrong: %s", b);
  /* The detailed block is ABSENT, not zero, when the layer is off. */
  KBC_CHECK_MSG(strstr(b, "# TYPE kb_metrics_detailed gauge\n"
                          "kb_metrics_detailed 0\n") != NULL,
                "kb_metrics_detailed is not reported as 0: %s", b);
  KBC_CHECK_MSG(strstr(b, "kb_search_stage_requests_total") == NULL,
                "the detailed block is present with the layer off: %s", b);
  KBC_CHECK_MSG(strstr(b, "kb_per_kb_requests_total") == NULL,
                "the per-kb families are present with the layer off: %s", b);
  /* Every line ends in \n, the last one included. */
  KBC_CHECK_MSG(b[0] != '\0' && strrchr(b, '\n')[1] == '\0',
                "the exposition does not end with a newline");
  kbc_response_free(&r);

  /* A method that is not GET is 405, not an empty 200 a scraper would parse
   * as an empty metric set. */
  KBC_CHECK_EQ_INT(call(&f, "POST", "/metrics", NULL, NULL, &r), 405);
  kbc_response_free(&r);

  fx_teardown(&f);
}

/* Over a socket the endpoint is no-store and it reports what the daemon has
 * actually served — a metrics endpoint that renders zeros forever is a lie a
 * dashboard cannot detect. */
KBC_TEST(metrics_counts_served_api_requests) {
  fixture f;
  fx_setup(&f, NULL);
  server s;
  srv_start(&s, &f);
  if (s.h == NULL) {
    fx_teardown(&f);
    return;
  }
  char reply[16384];
  int status = 0;
  /* Two /api requests. `/api/health` is a Read-family route
   * (`classify_route`: "health" is not a named segment, so it falls to the
   * top-level match — which lists stats/kbs/identity/metrics, not health, and
   * therefore Other). `/api/kbs` is unambiguously Read. */
  for (int i = 0; i < 2; i++) {
    const char *req = "GET /api/kbs HTTP/1.1\r\nHost: x\r\n\r\n";
    KBC_CHECK(raw_exchange(&s, req, strlen(req), &status, reply, sizeof reply));
    KBC_CHECK_EQ_INT(status, 200);
 }
  const char *m = "GET /metrics HTTP/1.1\r\nHost: x\r\n\r\n";
  KBC_CHECK(raw_exchange(&s, m, strlen(m), &status, reply, sizeof reply));
  KBC_CHECK_EQ_INT(status, 200);
  KBC_CHECK_MSG(strstr(reply, "Content-Type: text/plain; version=0.0.4; "
                              "charset=utf-8") != NULL,
                "/metrics is not text/plain 0.0.4: %s", reply);
  KBC_CHECK_MSG(strstr(reply, "Cache-Control: no-store") != NULL,
                "/metrics is cacheable: %s", reply);
  KBC_CHECK_MSG(strstr(reply, "kb_http_requests_total 2\n") != NULL,
                "two served /api requests did not count as 2: %s", reply);
  KBC_CHECK_MSG(strstr(reply, "kb_route_requests_total{route=\"read\"} 2\n") !=
                    NULL,
                "the requests were not attributed to the read family: %s",
                reply);
  /* /metrics is outside the /api tree in the original, so it is not counted
   * against itself. */
  KBC_CHECK_MSG(strstr(reply, "kb_route_requests_total{route=\"other\"} 0\n") !=
                   NULL,
                "/metrics counted itself: %s", reply);

  srv_stop(&s);
  fx_teardown(&f);
}

/* With the detailed layer on, the families the original gates behind
 * `[server] metrics` appear — and the per-kb set is sorted, because the
 * original's map iterates in random order and a response that reshuffles
 * between scrapes is not a response anyone can diff. */
KBC_TEST(the_detailed_metrics_layer_appears_when_enabled) {
  fixture f;
  fx_setup(&f, NULL);
  KBC_CHECK(setenv("KBC_METRICS_DETAILED", "1", 1) == 0);
  server s;
  srv_start(&s, &f);
  if (s.h == NULL) {
    (void)unsetenv("KBC_METRICS_DETAILED");
    fx_teardown(&f);
    return;
  }
  const char *req = "GET /api/kbs HTTP/1.1\r\nHost: x\r\n\r\n";
  char reply[16384];
  int status = 0;
  KBC_CHECK(raw_exchange(&s, req, strlen(req), &status, reply, sizeof reply));
  KBC_CHECK_EQ_INT(status, 200);
  const char *m = "GET /metrics HTTP/1.1\r\nHost: x\r\n\r\n";
  KBC_CHECK(raw_exchange(&s, m, strlen(m), &status, reply, sizeof reply));
  KBC_CHECK_EQ_INT(status, 200);
  KBC_CHECK_MSG(strstr(reply, "kb_metrics_detailed 1\n") != NULL,
                "the detailed layer is on but does not say so: %s", reply);
  KBC_CHECK_MSG(strstr(reply, "# TYPE kb_search_stage_requests_total "
                              "counter\n") != NULL,
                "the search-stage family is missing with the layer on: %s",
                reply);
  KBC_CHECK_MSG(strstr(reply, "kb_per_kb_requests_total{kb=\"kb\"} 0\n") !=
                    NULL,
                "a configured kb does not render at 0 before its first "
                "request: %s",
                reply);
  KBC_CHECK_MSG(strstr(reply, "kb_storage_ops_total{kind=\"upsert\"} 0\n") !=
                    NULL,
                "the storage families are missing with the layer on: %s",
                reply);
  /* The one detailed family kb-c can answer with a real number: the files the
   * indexer has observed. The rest of the block renders 0 because the layers
   * behind it — storage actor, pipeline timing, search stages — do not exist
   * in this port, and 0 is the format's own answer for an unobserved
   * fixed-cardinality family. */
  KBC_CHECK_MSG(strstr(reply, "kb_indexer_files_total 2\n") != NULL,
                "the indexed file count is not the real one: %s", reply);
  srv_stop(&s);
  (void)unsetenv("KBC_METRICS_DETAILED");
  fx_teardown(&f);
}

/* The static fallback runs only where a static root is configured, and it is
 * bounded by the same component-wise containment check as the artifact
 * serve. */
KBC_TEST(the_static_fallback_is_bounded_by_its_root) {
  fixture f;
  fx_setup(&f, NULL);
  char dist[KBC_TEST_PATH_MAX];
  path_under(dist, sizeof dist, f.root, "dist");
  char p[KBC_TEST_PATH_MAX];
  kbc_test_mkdir_p(dist);
  path_under(p, sizeof p, dist, "index.html");
  kbc_test_write_file(p, "<html>shell</html>");
  path_under(p, sizeof p, dist, "app.js");
  kbc_test_write_file(p, "console.log(1)");
  /* A sibling of the static root, named so a byte-prefix guard would take it
   * for a child. */
  char sibling[KBC_TEST_PATH_MAX];
  path_under(sibling, sizeof sibling, f.root, "dist-backup");
  kbc_test_mkdir_p(sibling);
  path_under(p, sizeof p, sibling, "leak.js");
  kbc_test_write_file(p, "STATIC-ROOT-ESCAPE");
  /* Reached through a symlink, because that is the only way a canonicalised
   * path under the root can leave it — and it is exactly the shape a
   * byte-prefix `strncmp` guard waves through. */
  char target[KBC_TEST_PATH_MAX];
  path_under(p, sizeof p, dist, "into-backup.js");
  path_under(target, sizeof target, sibling, "leak.js");
  KBC_CHECK(symlink(target, p) == 0);
  KBC_CHECK(setenv("KB_SPA_DIST", dist, 1) == 0);
  server s;
  srv_start(&s, &f);
  if (s.h == NULL) {
    (void)unsetenv("KB_SPA_DIST");
    fx_teardown(&f);
    return;
  }
  char reply[8192];
  int status = 0;
  {
    const char *req = "GET /app.js HTTP/1.1\r\nHost: x\r\n\r\n";
    KBC_CHECK(raw_exchange(&s, req, strlen(req), &status, reply, sizeof reply));
    KBC_CHECK_EQ_INT(status, 200);
    KBC_CHECK_MSG(strstr(reply, "console.log(1)") != NULL,
                  "a static asset inside the root is not served: %s", reply);
    KBC_CHECK_MSG(strstr(reply, "Content-Type: application/javascript; "
                                "charset=utf-8") != NULL,
                  "a .js asset has the wrong content type: %s", reply);
 }
  {
    const char *req = "GET /dist-backup/leak.js HTTP/1.1\r\nHost: x\r\n\r\n";
    KBC_CHECK(raw_exchange(&s, req, strlen(req), &status, reply, sizeof reply));
    KBC_CHECK_MSG(status != 200, "a file in the static root's SIBLING was "
                                  "served: %d %s",
                  status, reply);
    KBC_CHECK_MSG(strstr(reply, "STATIC-ROOT-ESCAPE") == NULL,
                  "a byte outside the static root was served: %s", reply);
 }
  {
    const char *req = "GET /into-backup.js HTTP/1.1\r\nHost: x\r\n\r\n";
    KBC_CHECK(raw_exchange(&s, req, strlen(req), &status, reply, sizeof reply));
    KBC_CHECK_MSG(status != 200,
                  "a symlink into the static root's SIBLING was served: %d %s",
                  status, reply);
    KBC_CHECK_MSG(strstr(reply, "STATIC-ROOT-ESCAPE") == NULL,
                  "the static fallback followed a symlink out of its root: %s",
                  reply);
  }
  srv_stop(&s);
  (void)unsetenv("KB_SPA_DIST");
  fx_teardown(&f);
}

/* Without a static root the fallback says WHY it is a 404 instead of looking
 * like a missing page. */
KBC_TEST(no_static_root_says_so) {
  fixture f;
  fx_setup(&f, NULL);
  (void)unsetenv("KB_SPA_DIST");
  server s;
  srv_start(&s, &f);
  if (s.h == NULL) {
    fx_teardown(&f);
    return;
  }
  char reply[8192];
  int status = 0;
  const char *req = "GET /index.html HTTP/1.1\r\nHost: x\r\n\r\n";
  KBC_CHECK(raw_exchange(&s, req, strlen(req), &status, reply, sizeof reply));
  KBC_CHECK_EQ_INT(status, 404);
  KBC_CHECK_MSG(strstr(reply, "KB_SPA_DIST") != NULL,
                "the 404 does not name the remedy: %s", reply);
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
      {"errors_are_problem_json", errors_are_problem_json},
      {"problem_json_over_a_socket", problem_json_over_a_socket},
      {"kbs_lists_the_configured_corpora", kbs_lists_the_configured_corpora},
      {"kbs_is_token_gated", kbs_is_token_gated},
      {"identity_reports_attribution_only", identity_reports_attribution_only},
      {"identity_names_the_admission_that_happened",
       identity_names_the_admission_that_happened},
      {"token_carriers_admit_and_refuse", token_carriers_admit_and_refuse},
      {"both_carriers_authorization_decides_and_says_so",
       both_carriers_authorization_decides_and_says_so},
      {"identity_reports_the_carrier_for_a_socketless_token",
       identity_reports_the_carrier_for_a_socketless_token},
      {"new_routes_are_in_the_route_table", new_routes_are_in_the_route_table},
      {"cors_is_same_origin_only_unless_configured",
       cors_is_same_origin_only_unless_configured},
      {"rate_limit_caps_one_connection", rate_limit_caps_one_connection},
      {"a_bad_rate_limit_threshold_refuses_to_start",
       a_bad_rate_limit_threshold_refuses_to_start},
      {"concurrent_connects_do_not_corrupt_the_conn_table",
       concurrent_connects_do_not_corrupt_the_conn_table},
      {"sse_attach_racing_the_event_fan_out_keeps_the_daemon",
       sse_attach_racing_the_event_fan_out_keeps_the_daemon},
      {"sse_gap_probe_replaces_the_replay_it_cannot_honour",
       sse_gap_probe_replaces_the_replay_it_cannot_honour},
      {"sse_lag_probe_tells_a_stalled_client_how_far_behind_it_is",
       sse_lag_probe_tells_a_stalled_client_how_far_behind_it_is},
      {"a_refused_start_leaves_the_callers_descriptors_open",
       a_refused_start_leaves_the_callers_descriptors_open},
      {"no_path_outside_the_source_root_is_ever_served",
       no_path_outside_the_source_root_is_ever_served},
      {"the_artifact_subdomain_is_chosen_by_host",
       the_artifact_subdomain_is_chosen_by_host},
      {"artifact_bytes_on_the_parent_origin",
       artifact_bytes_on_the_parent_origin},
      {"metrics_is_prometheus_text_exposition",
       metrics_is_prometheus_text_exposition},
      {"metrics_counts_served_api_requests", metrics_counts_served_api_requests},
      {"the_detailed_metrics_layer_appears_when_enabled",
       the_detailed_metrics_layer_appears_when_enabled},
      {"the_static_fallback_is_bounded_by_its_root",
       the_static_fallback_is_bounded_by_its_root},
      {"no_static_root_says_so", no_static_root_says_so},
      {NULL, NULL},
  };
  return kbc_test_run("httpd", cases);
}
