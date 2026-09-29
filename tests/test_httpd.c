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
#include <poll.h>
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

  /* The three corpus-scoped link routes are listed with the method and the
   * auth flag they actually run under. All three sit under /api, so all three
   * are token-gated — a row that said otherwise would document an open door,
   * and a row missing entirely is a route nobody can discover. */
  static const char *const link_routes[] = {
      "/api/kb/{kb}/notes/{id}/links",
      "/api/kb/{kb}/backlinks/{id}",
      "/api/kb/{kb}/wikilinks/suggest",
  };
  for (size_t r = 0; r < sizeof link_routes / sizeof link_routes[0]; r++) {
    bool found = false;
    for (size_t i = 0; i < KBC_ROUTES_LEN; i++) {
      if (strcmp(KBC_ROUTES[i].path, link_routes[r]) == 0) {
        found = true;
        KBC_CHECK_MSG(KBC_ROUTES[i].needs_auth, "%s is listed as open",
                      link_routes[r]);
        KBC_CHECK_MSG(strcmp(KBC_ROUTES[i].method, "GET") == 0,
                      "%s is not a GET in the route table", link_routes[r]);
        KBC_CHECK_MSG(KBC_ROUTES[i].summary[0] != '\0', "%s has no summary",
                      link_routes[r]);
      }
    }
    KBC_CHECK_MSG(found, "%s is missing from KBC_ROUTES", link_routes[r]);
  }

  /* The capture route is a WRITE, and a write listed with `needs_auth`
   * false would document an open door into the corpus. It sits under /api, so
   * the token gate really does cover it — a row claiming otherwise would be a
   * lie about a file-writing endpoint, and a missing row is a route nobody can
   * discover. */
  {
    bool found = false;
    for (size_t i = 0; i < KBC_ROUTES_LEN; i++) {
      if (strcmp(KBC_ROUTES[i].path, "/api/kb/{kb}/capture") == 0) {
        found = true;
        KBC_CHECK_MSG(KBC_ROUTES[i].needs_auth,
                      "the capture route is listed as open: it writes files");
        KBC_CHECK_MSG(strcmp(KBC_ROUTES[i].method, "POST") == 0,
                      "the capture route is not a POST in the route table");
        KBC_CHECK_MSG(KBC_ROUTES[i].summary[0] != '\0',
                      "the capture route has no summary");
      }
    }
    KBC_CHECK_MSG(found, "/api/kb/{kb}/capture is missing from KBC_ROUTES");
  }
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
 * Last-Event-ID. */
static bool sse_open_head(int port, const char *last_event_id, sse_client *c) {
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
    KBC_CHECK(sse_open_head(port, "1", &c));
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
    KBC_CHECK(sse_open_head(port, "99999", &c));
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
    KBC_CHECK(sse_open_head(port, NULL, &c));
    KBC_CHECK(sse_wait(&c, ":ok", 16));
    KBC_CHECK_MSG(!sse_saw(&c, "event: gap"),
                  "a cursorless client was probed: %s", c.buf);
    sse_hangup(&c);
  }

  kbc_httpd_stop(h);
  fx_teardown(&f);
}

/* There is deliberately NO test for the `lag` half of the probe.
 *
 * `lag` is emitted only when a consumer's 64-slot queue actually sheds frames,
 * which needs the worker's socket write to BLOCK. It does not block on a test
 * client that merely stops reading: `sse_pump` buffers up to
 * KBC_SSE_OUT_HIGH_WATER (256 KiB) and the kernel then absorbs the rest into
 * the send buffer, so a client can fall this far behind without the queue ever
 * filling. Measured on this machine: a burst of 60,000 events (~4 MiB, with
 * no reads at all) still left the queue intact in 2 runs of 6, and a
 * publish-and-check loop failed 5 runs in 15 under TSan. It is a race the
 * daemon wins often and loses sometimes, not a behaviour a test can force.
 *
 * Shrinking the CLIENT's receive buffer does not help — the absorbing buffer
 * is the server's send buffer, which a test cannot reach without a production
 * knob added purely to enable a test. A case here would be red on a correct
 * build, which is worse than no case: it trains everyone who reads CI to
 * ignore red. If the lag probe ever gets a deterministic test it will be a
 * bounded-send-buffer test written at the socket, not a burst-and-hope here. */


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

/* The rendered page carries the full 7.4 KB stylesheet, so a buffer sized for
 * a raw `.md` source truncates the answer mid-body and a test asserting on the
 * document's prose would fail for the wrong reason. Every reply buffer that
 * can receive a rendered page is this size. */
#define PAGE_REPLY 32768

/* The id the store minted for a corpus-relative path, found through the public
 * listing rather than by recomputing it: the test must not re-implement the id
 * minting to find its own fixture.
 *
 * The scan pairs each `"id"` with the `"path"` that FOLLOWS it in the same
 * object, so the id returned is the one that goes WITH the path — taking the
 * first id in the list would silently return a different document's. */
static void ofx_id_for(origin_fixture *o, const char *rel, char *out,
                       size_t cap) {
  kbc_response r;
  out[0] = '\0';
  KBC_CHECK_EQ_INT(call(&o->f, "GET", "/api/artifacts", AT0, NULL, &r), 200);
  char pat[256];
  int n = snprintf(pat, sizeof pat, "\"path\":\"%s\"", rel);
  KBC_CHECK_MSG(n > 0 && (size_t)n < sizeof pat, "bad path pattern for %s", rel);
  const char *cur = NULL;
  for (const char *q = r.body.ptr; q != NULL && *q != '\0'; q++) {
    if (strncmp(q, "\"id\":\"", 6) == 0) cur = q + 6;
    if (cur != NULL && strncmp(q, pat, (size_t)n) == 0) {
      size_t i = 0;
      while (cur[i] != '\0' && cur[i] != '"' && i < KBC_MAX_ID_LEN &&
             i + 1 < cap) {
        out[i] = cur[i];
        i++;
      }
      out[i] = '\0';
      break;
    }
  }
  kbc_response_free(&r);
  KBC_CHECK_MSG(out[0] != '\0', "no id found for %s", rel);
}

/* Writes one more document into the origin corpus and re-indexes, so the
 * store knows it. The id comes back through the listing, never from a second
 * implementation of the minting rule. */
static void ofx_add_md(origin_fixture *o, const char *name,
                       const char *body, char *id, size_t cap) {
  char p[KBC_TEST_PATH_MAX];
  path_under(p, sizeof p, o->sub, name);
  kbc_test_write_file(p, body);
  kbc_err err;
  kbc_err_reset(&err);
  KBC_CHECK_OK(kbc_app_reindex(o->f.app, &err));
  char rel[256];
  int n = snprintf(rel, sizeof rel, "sub/%s", name);
  KBC_CHECK_MSG(n > 0 && (size_t)n < sizeof rel, "bad rel for %s", name);
  ofx_id_for(o, rel, id, cap);
}

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
  char reply[PAGE_REPLY]; /* `/` is a rendered page; see PAGE_REPLY. */

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
  /* PAGE_REPLY, not 8 KiB: one.md is now served as a rendered page and the
   * stylesheet alone is 7.4 KiB, so the old buffer truncated the body. */
  char reply[PAGE_REPLY];
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

/* A `.md` is served as a RENDERED PAGE on the parent origin, and the answer
 * over a real socket is a page and not the source it was made from.
 *
 * Every assertion here is one that fails if the render is reverted to a raw
 * serve, and each names a different thing: the doctype and the `kb-md-doc`
 * container are markup the source never contained, and the source's own `# `
 * heading marker is markup the renderer consumed. A body-length check would
 * pass either way — the page is only a couple of hundred bytes more than the
 * 7.4 KiB stylesheet it wraps — so nothing here counts bytes. */
KBC_TEST(a_markdown_artifact_is_served_as_a_rendered_page) {
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
  snprintf(path, sizeof path, "/api/kb/kb/artifact/%s", o.id);
  kbc_str req;
  kbc_str_init(&req);
  (void)kbc_str_printf(&req, "GET %s HTTP/1.1\r\nHost: x\r\n\r\n", path);
  char reply[PAGE_REPLY];
  int status = 0;
  KBC_CHECK(raw_exchange(&s, req.ptr, req.len, &status, reply, sizeof reply));
  kbc_str_free(&req);
  KBC_CHECK_EQ_INT(status, 200);

  /* The wrapper the renderer emits and the source does not contain. */
  KBC_CHECK_MSG(strstr(reply, "<!doctype html>") != NULL,
                "a .md was not served as a rendered page: %s", reply);
  KBC_CHECK_MSG(strstr(reply, "<main class=\"kb-md-doc\">") != NULL,
                "the rendered page has no kb-md-doc container: %s", reply);
  /* The heading became markup: `<h1>One</h1>`, and no ATX marker anywhere. */
  KBC_CHECK_MSG(strstr(reply, "<h1>One</h1>") != NULL,
                "the heading was not rendered: %s", reply);
  KBC_CHECK_MSG(strstr(reply, "# One") == NULL,
                "the raw markdown source was served instead of the page: %s",
                reply);
  /* The body prose survives rendering, so this is not an empty shell. */
  KBC_CHECK_MSG(strstr(reply, "sub artifact") != NULL,
                "the rendered page lost the document body: %s", reply);

  /* The header set is unchanged by rendering. */
  KBC_CHECK_MSG(strstr(reply, "Content-Type: text/html; charset=utf-8") != NULL,
                "a rendered page is not text/html: %s", reply);
  KBC_CHECK_MSG(strstr(reply, "X-Content-Type-Options: nosniff") != NULL,
                "a rendered page carries no nosniff: %s", reply);
  KBC_CHECK_MSG(strstr(reply, "Content-Security-Policy: sandbox\r\n") != NULL,
                "a rendered page lost the bare `sandbox` CSP: %s", reply);
  {
    char want[128];
    snprintf(want, sizeof want, "X-Kb-Artifact-Id: %s", o.id);
    KBC_CHECK_MSG(strstr(reply, want) != NULL,
                  "a rendered page carries no X-Kb-Artifact-Id: %s", reply);
  }

  srv_stop(&s);
  ofx_teardown(&o);
}

/* The SAME document is a rendered page on the artifact subdomain. That is a
 * different handler with a different header set and a different status for a
 * bad body, so "the parent route renders it" is no evidence that this one
 * does. */
KBC_TEST(a_markdown_artifact_is_rendered_on_the_subdomain_too) {
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
  char reply[PAGE_REPLY];

  int status = origin_get(&s, host, "/", NULL, reply, sizeof reply);
  KBC_CHECK_EQ_INT(status, 200);
  KBC_CHECK_MSG(strstr(reply, "<!doctype html>") != NULL,
                "the subdomain served a .md as raw source: %s", reply);
  KBC_CHECK_MSG(strstr(reply, "<main class=\"kb-md-doc\">") != NULL,
                "the subdomain's page has no kb-md-doc container: %s", reply);
  KBC_CHECK_MSG(strstr(reply, "# One") == NULL,
                "the subdomain served the raw markdown source: %s", reply);
  KBC_CHECK_MSG(strstr(reply, "sub artifact") != NULL,
                "the subdomain's rendered page lost the body: %s", reply);
  /* The label is now the truth about the body: it is HTML, and saying
   * otherwise under `nosniff` is how a rendered page becomes a download. */
  KBC_CHECK_MSG(strstr(reply, "Content-Type: text/html; charset=utf-8") != NULL,
                "a rendered page on the subdomain is not text/html: %s", reply);
  /* The subdomain's own header set, unchanged by rendering. */
  {
    char want[128];
    snprintf(want, sizeof want, "X-Kb-Artifact-Id: %s", o.id);
    KBC_CHECK_MSG(strstr(reply, want) != NULL,
                  "the subdomain page carries no artifact id: %s", reply);
  }

  srv_stop(&s);
  ofx_teardown(&o);
}

/* A `.md` that is not valid UTF-8 is refused, and the refusal carries no bytes
 * of the document. This is the case the render exists to make decidable: the
 * renderer returns KBC_ERR_PARSE and renders nothing, and the route had
 * already decided such a document is a 400 rather than something to serve
 * under a text/html label.
 *
 * "Serves no bytes" is the assertion that matters, and the one a plausible fix
 * fails: a route that renders, fails, and then falls back to the raw source
 * would still be a 200 with the whole document in it, and a test that checked
 * only the status would call that correct. */
KBC_TEST(a_non_utf8_markdown_artifact_is_refused_and_serves_no_bytes) {
  origin_fixture o;
  ofx_setup(&o);
  /* 0xff 0xfe is never valid UTF-8, and these bytes cannot appear in the
   * rendered output of a document that was refused. */
  static const char kBad[] = "# Bad\n\nMARKER-BYTES \xff\xfe not utf8\n";
  char bad_id[KBC_MAX_ID_LEN + 1];
  ofx_add_md(&o, "bad.md", kBad, bad_id, sizeof bad_id);
  server s;
  srv_start(&s, &o.f);
  if (s.h == NULL || bad_id[0] == '\0') {
    srv_stop(&s);
    ofx_teardown(&o);
    return;
  }
  char path[256];
  snprintf(path, sizeof path, "/api/kb/kb/artifact/%s", bad_id);
  kbc_str req;
  kbc_str_init(&req);
  (void)kbc_str_printf(&req, "GET %s HTTP/1.1\r\nHost: x\r\n\r\n", path);
  char reply[PAGE_REPLY];
  int status = 0;
  KBC_CHECK(raw_exchange(&s, req.ptr, req.len, &status, reply, sizeof reply));
  kbc_str_free(&req);
  KBC_CHECK_EQ_INT(status, 400);
  /* Not one byte of the document, and certainly not a half-rendered page. */
  KBC_CHECK_MSG(strstr(reply, "MARKER-BYTES") == NULL,
                "a refused .md leaked its source: %s", reply);
  KBC_CHECK_MSG(strstr(reply, "<!doctype html>") == NULL,
                "a refused .md was served as a page anyway: %s", reply);
  KBC_CHECK_MSG(strstr(reply, "problem+json") != NULL,
                "the refusal is not a problem+json 400: %s", reply);
  /* The artifact headers must not ride along on a refusal: a sandbox CSP on an
   * error body is a header set for a document that was not served. */
  KBC_CHECK_MSG(strstr(reply, "X-Kb-Artifact-Id") == NULL,
                "a refused .md published artifact headers: %s", reply);

  /* The subdomain refuses the same document too, and with the status IT uses:
   * 500, not the parent origin's 400. The two handlers disagree about this in
   * the original (`artifact.rs:715` vs `docs.rs:1489`) and the disagreement is
   * deliberately preserved, so the assertion pins 500 rather than "some 4xx".
   * What matters on both surfaces is the same: a refusal, carrying no bytes. */
  char host[KBC_TEST_PATH_MAX];
  snprintf(host, sizeof host, "%s.artifacts.localhost", bad_id);
  status = origin_get(&s, host, "/", NULL, reply, sizeof reply);
  KBC_CHECK_EQ_INT(status, 500);
  KBC_CHECK_MSG(strstr(reply, "MARKER-BYTES") == NULL,
                "the subdomain leaked a refused document's bytes: %s", reply);

  srv_stop(&s);
  ofx_teardown(&o);
}

/* A render that fails for a reason OTHER than bad UTF-8 is reported as the
 * error it is, and the raw source is NOT served in its place.
 *
 * This is the test that makes the no-fallback rule observable, and it is the
 * one the UTF-8 case above cannot be. The UTF-8 refusal is decided by the
 * ROUTE, before the renderer is ever called, so a route that rendered, got a
 * failure and then fell back to raw bytes would still pass it. Here the
 * renderer itself refuses — the document is larger than
 * KBC_MAX_ARTIFACT_BYTES — and the only way to pass is to surface that.
 *
 * The file is grown on disk AFTER indexing, which is how this state is
 * reached in practice: the indexer caps what it will ingest, so an artifact
 * over the limit never gets an id, but a document that was a kilobyte at index
 * time and is twenty megabytes now does. The renderer re-checks the limit on
 * the bytes it is actually handed, so it refuses.
 *
 * Without the render, this route would serve those twenty megabytes as a 200.
 * That is the specific outcome this asserts against. */
KBC_TEST(a_failed_markdown_render_is_an_error_not_a_raw_serve) {
  origin_fixture o;
  ofx_setup(&o);
  /* `one.md` is already indexed and has an id; it is about to stop being
   * renderable. */
  char p[KBC_TEST_PATH_MAX];
  path_under(p, sizeof p, o.sub, "one.md");
  FILE *f = fopen(p, "wb");
  KBC_CHECK_NOT_NULL(f);
  if (f == NULL) {
    ofx_teardown(&o);
    return;
  }
  /* Just over the 16 MiB limit, so the renderer's size check fires. The
   * marker rides along so a raw fallback would be unmistakable. */
  static const char kChunk[65536] = {'A'};
  for (unsigned i = 0; i < 257u; i++) { /* 257 * 64 KiB = 16.06 MiB */
    if (fwrite(kChunk, 1, sizeof kChunk, f) != sizeof kChunk) break;
  }
  (void)fputs("MARKER-OVERSIZE", f);
  (void)fclose(f);
  {
    struct stat sb;
    KBC_CHECK_EQ_INT(stat(p, &sb), 0);
    KBC_CHECK_MSG((size_t)sb.st_size > (size_t)KBC_MAX_ARTIFACT_BYTES,
                  "the fixture did not exceed the limit: %lld bytes",
                  (long long)sb.st_size);
  }

  server s;
  srv_start(&s, &o.f);
  if (s.h == NULL || o.id[0] == '\0') {
    srv_stop(&s);
    ofx_teardown(&o);
    return;
  }
  char path[256];
  snprintf(path, sizeof path, "/api/kb/kb/artifact/%s", o.id);
  kbc_str req;
  kbc_str_init(&req);
  (void)kbc_str_printf(&req, "GET %s HTTP/1.1\r\nHost: x\r\n\r\n", path);
  /* The fallback would put sixteen megabytes in the body, so the buffer has to
   * be big enough that a fallback is detected as "far too much" rather than
   * as a truncated success. It only has to hold the ERROR body in the correct
   * case. */
  static char reply[1 << 20];
  int status = 0;
  KBC_CHECK(raw_exchange(&s, req.ptr, req.len, &status, reply, sizeof reply));
  kbc_str_free(&req);
  /* An error, not a 200 carrying the source. */
  KBC_CHECK_MSG(status >= 400,
                "a failed render served status %d instead of an error", status);
  KBC_CHECK_MSG(strstr(reply, "MARKER-OVERSIZE") == NULL,
                "a failed render fell back to serving the raw source: %s",
                reply);
  KBC_CHECK_MSG(strstr(reply, "<!doctype html>") == NULL,
                "a failed render served a page anyway: %s", reply);
  KBC_CHECK_MSG(strstr(reply, "Content-Security-Policy") == NULL,
                "a failed render published artifact headers: %s", reply);
  /* And the reason is in the answer, so an operator can act on it. */
  KBC_CHECK_MSG(strstr(reply, "markdown") != NULL,
                "the render failure does not name the renderer: %s", reply);

  srv_stop(&s);
  ofx_teardown(&o);
}

/* A zero-byte `.md` is served as an empty PAGE, not refused.
 *
 * This is a real corpus state — a `touch`, a writer that created the file and
 * never filled it, a truncation — and the route has one specific way to get it
 * wrong. `read_whole_file` appends to a `kbc_str` and appends nothing for a
 * zero-byte file, so it hands back `ptr == NULL, len == 0`. That is a correct
 * description of "no bytes" and a NULL `const char *` is not how the render
 * API spells a document: `kbc_markdown_render` rejects a NULL `src` as
 * KBC_ERR_INVALID, which the route's status mapping turns into a 400 — a
 * "bad request" blaming the caller for a file the caller never wrote. The
 * render helper therefore passes "" explicitly, and this is the test that
 * holds that line in place.
 *
 * The file is TRUNCATED after indexing, and that is forced rather than
 * convenient: the indexer skips zero-byte files outright (app.c:1050, "1..N
 * bytes is out of range"), so a file that was empty all along never gets an id
 * and the route is unreachable. The route reads the bytes off DISK rather than
 * out of the store, so a document that HAD content and no longer does reaches
 * exactly the state under test — the same way the oversize case does, and for
 * the same reason: the store's row and the file behind it can disagree.
 *
 * The assertions are the shape of a rendered page plus the ABSENCE of a
 * failure. A body-length check alone would not do — "rendered an empty page"
 * and "served no body" are the same size — so the wrapper is what separates
 * them. */
KBC_TEST(an_empty_markdown_artifact_is_an_empty_page_not_an_error) {
  origin_fixture o;
  ofx_setup(&o);
  /* `one.md` is indexed with content and has an id; it is about to be
   * emptied underneath that id. */
  char p[KBC_TEST_PATH_MAX];
  path_under(p, sizeof p, o.sub, "one.md");
  FILE *f = fopen(p, "wb");
  KBC_CHECK_NOT_NULL(f);
  /* A real zero-byte file, not one holding "": kbc_test_write_file writes
   * strlen(content) bytes, and the bug is specifically about a file with none.
   * fopen("wb") alone truncates to zero, which is the honest way to make one. */
  if (f != NULL) (void)fclose(f);
  {
    struct stat sb;
    KBC_CHECK_EQ_INT(stat(p, &sb), 0);
    KBC_CHECK_MSG(sb.st_size == 0, "the fixture is not zero bytes: %lld",
                  (long long)sb.st_size);
  }

  server s;
  srv_start(&s, &o.f);
  if (s.h == NULL || o.id[0] == '\0') {
    srv_stop(&s);
    ofx_teardown(&o);
    return;
  }
  char path[256];
  snprintf(path, sizeof path, "/api/kb/kb/artifact/%s", o.id);
  kbc_str req;
  kbc_str_init(&req);
  (void)kbc_str_printf(&req, "GET %s HTTP/1.1\r\nHost: x\r\n\r\n", path);
  char reply[PAGE_REPLY];
  int status = 0;
  KBC_CHECK(raw_exchange(&s, req.ptr, req.len, &status, reply, sizeof reply));
  kbc_str_free(&req);
  /* Not a 500. This is the assertion the NULL-source bug would break. */
  KBC_CHECK_EQ_INT(status, 200);
  KBC_CHECK_MSG(strstr(reply, "<!doctype html>") != NULL,
                "an empty .md did not render the page shell: %s", reply);
  /* The body container is present and EMPTY — that is what "rendered, with
   * nothing in it" looks like, and it is not the same as serving no body. */
  KBC_CHECK_MSG(strstr(reply, "<main class=\"kb-md-doc\"></main>") != NULL,
                "an empty .md did not render an empty kb-md-doc: %s", reply);
  /* The document has no title of its own, so it takes the renderer's literal. */
  KBC_CHECK_MSG(strstr(reply, "<title>Untitled</title>") != NULL,
                "an empty .md is not titled Untitled: %s", reply);
  KBC_CHECK_MSG(strstr(reply, "</main></body></html>") != NULL,
                "the empty page is truncated: %s", reply);
  /* Nothing of the old document survived: it is empty, not stale. */
  KBC_CHECK_MSG(strstr(reply, "sub artifact") == NULL,
                "an emptied .md still serves its old body: %s", reply);
  /* The header set is still the artifact set, not an error's. */
  KBC_CHECK_MSG(strstr(reply, "Content-Security-Policy: sandbox\r\n") != NULL,
                "an empty page lost the bare `sandbox` CSP: %s", reply);

  srv_stop(&s);
  ofx_teardown(&o);
}

/* Raw HTML in a `.md` is passed through by the renderer — the original's
 * `render.unsafe = true` — and what contains it is the
 * `Content-Security-Policy: sandbox` this route sets, which forces the
 * response into an opaque origin with scripts disabled. The two are read
 * together, so this asserts BOTH: the passthrough happened, and the policy
 * that makes it safe is still the bare word `sandbox`.
 *
 * A test that only checked the passthrough would pass on a route that had
 * dropped or widened the CSP, which is the dangerous direction. */
KBC_TEST(raw_html_in_markdown_passes_through_under_the_sandbox) {
  origin_fixture o;
  ofx_setup(&o);
  static const char kHtml[] = "# Raw\n\nMARKER-HTML <b class=\"x\">bold</b> "
                              "<script>MARKER-SCRIPT</script>\n";
  char html_id[KBC_MAX_ID_LEN + 1];
  ofx_add_md(&o, "raw.md", kHtml, html_id, sizeof html_id);
  server s;
  srv_start(&s, &o.f);
  if (s.h == NULL || html_id[0] == '\0') {
    srv_stop(&s);
    ofx_teardown(&o);
    return;
  }
  char path[256];
  snprintf(path, sizeof path, "/api/kb/kb/artifact/%s", html_id);
  kbc_str req;
  kbc_str_init(&req);
  (void)kbc_str_printf(&req, "GET %s HTTP/1.1\r\nHost: x\r\n\r\n", path);
  char reply[PAGE_REPLY];
  int status = 0;
  KBC_CHECK(raw_exchange(&s, req.ptr, req.len, &status, reply, sizeof reply));
  kbc_str_free(&req);
  KBC_CHECK_EQ_INT(status, 200);
  /* The source's own markup reached the page unescaped. */
  KBC_CHECK_MSG(strstr(reply, "<b class=\"x\">bold</b>") != NULL,
                "raw HTML in a .md was escaped instead of passed through: %s",
                reply);
  KBC_CHECK_MSG(strstr(reply, "<script>MARKER-SCRIPT</script>") != NULL,
                "a <script> in a .md was not passed through: %s", reply);
  /* And the policy that contains it is the whole value: `sandbox`, literally,
   * with no directive appended that would weaken it into a partial sandbox. */
  KBC_CHECK_MSG(strstr(reply, "Content-Security-Policy: sandbox\r\n") != NULL,
                "the passthrough is not contained by a bare `sandbox` CSP: %s",
                reply);
  KBC_CHECK_MSG(strstr(reply, "Content-Security-Policy: sandbox allow-") == NULL,
                "the sandbox CSP was widened: %s", reply);
  KBC_CHECK_MSG(strstr(reply, "X-Content-Type-Options: nosniff") != NULL,
                "a page carrying raw HTML has no nosniff: %s", reply);

  srv_stop(&s);
  ofx_teardown(&o);
}

/* `?download=1` is a download: the SOURCE, not the page. Rendering it would be
 * wrong — the caller asked for the file, and a file that silently became a
 * styled HTML document is not the file.
 *
 * The `.md` -> `.html` filename rewrite is kept, because it is the original's
 * wire contract and this route has always sent it; what is asserted here is
 * the BODY, which is the part that is a behaviour rather than a name. */
KBC_TEST(a_markdown_download_is_the_source_not_the_page) {
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
  snprintf(path, sizeof path, "/api/kb/kb/artifact/%s?download=1", o.id);
  kbc_str req;
  kbc_str_init(&req);
  (void)kbc_str_printf(&req, "GET %s HTTP/1.1\r\nHost: x\r\n\r\n", path);
  char reply[PAGE_REPLY];
  int status = 0;
  KBC_CHECK(raw_exchange(&s, req.ptr, req.len, &status, reply, sizeof reply));
  kbc_str_free(&req);
  KBC_CHECK_EQ_INT(status, 200);
  /* The source's own bytes, ATX marker and all. */
  KBC_CHECK_MSG(strstr(reply, "# One") != NULL,
                "?download=1 did not serve the markdown source: %s", reply);
  KBC_CHECK_MSG(strstr(reply, "sub artifact") != NULL,
                "?download=1 lost the document body: %s", reply);
  /* And emphatically not a rendered page. */
  KBC_CHECK_MSG(strstr(reply, "<!doctype html>") == NULL,
                "?download=1 rendered the document instead of serving it: %s",
                reply);
  KBC_CHECK_MSG(strstr(reply, "kb-md-doc") == NULL,
                "?download=1 served a rendered page: %s", reply);
  /* The disposition and the name rewrite are unchanged. */
  KBC_CHECK_MSG(strstr(reply, "Content-Disposition: attachment;") != NULL,
                "?download=1 does not attach: %s", reply);
  KBC_CHECK_MSG(strstr(reply, "filename=\"one.html\"") != NULL,
                "a markdown download is not offered as .html: %s", reply);

  srv_stop(&s);
  ofx_teardown(&o);
}

/* The title a `.md` is REPORTED under is the renderer's, not the store's, and
 * for this document the two are provably different — which is the only way
 * this test can fail.
 *
 * `title: Frontmatter Name` above an `# Ignored Heading` is titled
 * "Ignored Heading" by `kbc_parsed_title` (first h1 wins there) and
 * "Frontmatter Name" by the renderer's precedence, which is the one the served
 * page's <title> uses. A document where both functions agree would pass
 * whether or not the renderer is consulted, so the fixture is chosen to make
 * them disagree.
 *
 * Both JSON surfaces are checked because they are separate code paths, and a
 * fix that wired only one of them would leave a list row and the artifact it
 * links to named differently. */
KBC_TEST(a_markdown_artifact_is_reported_under_the_renderers_title) {
  origin_fixture o;
  ofx_setup(&o);
  static const char kDoc[] = "---\ntitle: Frontmatter Name\n---\n\n"
                             "# Ignored Heading\n\nbody\n";
  char fm_id[KBC_MAX_ID_LEN + 1];
  ofx_add_md(&o, "fm.md", kDoc, fm_id, sizeof fm_id);

  kbc_response r;
  /* The single-artifact route. */
  char p[256];
  snprintf(p, sizeof p, "/api/artifacts/%s", fm_id);
  KBC_CHECK_EQ_INT(call(&o.f, "GET", p, NULL, NULL, &r), 200);
  KBC_CHECK_MSG(strstr(r.body.ptr, "\"title\":\"Frontmatter Name\"") != NULL,
                "the artifact route does not report the renderer's title: %s",
                r.body.ptr);
  KBC_CHECK_MSG(strstr(r.body.ptr, "Ignored Heading") == NULL,
                "the store's title leaked onto the artifact route: %s",
                r.body.ptr);
  kbc_response_free(&r);

  /* The list, which has no source loaded and must fetch one to answer. */
  KBC_CHECK_EQ_INT(call(&o.f, "GET", "/api/artifacts", AT0, NULL, &r), 200);
  KBC_CHECK_MSG(strstr(r.body.ptr, "\"title\":\"Frontmatter Name\"") != NULL,
                "the list does not report the renderer's title: %s", r.body.ptr);
  KBC_CHECK_MSG(strstr(r.body.ptr, "Ignored Heading") == NULL,
                "the list shows the store's title for a .md: %s", r.body.ptr);
  /* A `.md` with an h1 and no frontmatter is one the two AGREE on, so it
   * must still be titled normally — a title resolver that answered
   * "Untitled" for everything would pass the case above and fail this. */
  KBC_CHECK_MSG(strstr(r.body.ptr, "\"title\":\"One\"") != NULL,
                "a plain h1 .md is no longer titled from its heading: %s",
                r.body.ptr);
  kbc_response_free(&r);

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

/* ================================================== the /api/links routes --
 *
 * Every case here goes over a real socket, because the thing under test is a
 * WIRE shape: the three `state` values, which of the optional members each one
 * carries, and the order the backlink rows come back in. Driving
 * kbc_httpd_handle would answer the same bytes, but a socket also proves the
 * route is REACHABLE, which is a different failure from a wrong body and the
 * one a route-table row alone cannot catch. */

/* Writes one file into the corpus, making its directory first. The routes read
 * the corpus from disk, so a document added mid-test has to exist there. */
static void corpus_file(fixture *f, const char *rel, const char *body) {
  char path[KBC_TEST_PATH_MAX + 64];
  snprintf(path, sizeof path, "%s/kb/%s", f->root, rel);
  char dir[KBC_TEST_PATH_MAX + 64];
  snprintf(dir, sizeof dir, "%s", path);
  char *slash = strrchr(dir, '/');
  if (slash != NULL) {
    *slash = '\0';
    kbc_test_mkdir_p(dir);
  }
  kbc_test_write_file(path, body);
}

/* A GET over the socket, body only, into `out`. Returns the status. */
static int get_body(server *s, const char *path, char *out, size_t cap) {
  char req[1024];
  int n = snprintf(req, sizeof req, "GET %s HTTP/1.1\r\nHost: x\r\n\r\n", path);
  out[0] = '\0';
  if (n <= 0 || (size_t)n >= sizeof req) return 0;
  char reply[16384];
  int status = 0;
  if (!raw_exchange(s, req, (size_t)n, &status, reply, sizeof reply)) return 0;
  const char *body = strstr(reply, "\r\n\r\n");
  if (body != NULL) {
    body += 4;
    size_t len = strlen(body);
    if (len >= cap) len = cap - 1;
    memcpy(out, body, len);
    out[len] = '\0';
  }
  return status;
}

/* Counts non-overlapping occurrences of `needle` in `hay`. */
static int count_of(const char *hay, const char *needle) {
  int n = 0;
  size_t l = strlen(needle);
  for (const char *p = hay; (p = strstr(p, needle)) != NULL; p += l) n++;
  return n;
}

/* The corpus the link cases share:
 *
 *   ops/deploy.md    "Deploy runbook"   links back to alpha
 *   infra/deploy.md  "Deploy infra"     links back to alpha
 *   alpha.md         the subject: one resolved link written two ways, one
 *                   ambiguous, one dangling, and one that names itself
 *
 * `[[deploy]]` is ambiguous because the corpus holds TWO documents whose bare
 * basename is `deploy` — the case the ladder exists for, and the one a port
 * that helpfully picks the first of gets wrong. */
static void links_fixture(fixture *f) {
  fx_setup(f, NULL);
  corpus_file(f, "ops/deploy.md", "# Deploy runbook\n\n[[/alpha.md]]\n");
  corpus_file(f, "infra/deploy.md", "# Deploy infra\n\n[[/alpha.md]]\n");
  corpus_file(
      f, "alpha.md",
      "# Alpha\n\n[[/ops/deploy.md]] and [[./ops/deploy.md]] are one document.\n"
      "[[deploy]] is not.\n[[nowhere]] does not exist.\n[[/alpha.md]] is this "
      "one.\n");
  kbc_err err;
  kbc_err_reset(&err);
  KBC_CHECK_OK(kbc_app_reindex(f->app, &err));
  f->reindexed = true;
}

KBC_TEST(links_report_resolved_ambiguous_and_dangling) {
  fixture f;
  links_fixture(&f);
  server s;
  srv_start(&s, &f);
  if (s.h == NULL) {
    fx_teardown(&f);
    return;
  }
  char alpha[KBC_MAX_ID_LEN + 1];
  char ops[KBC_MAX_ID_LEN + 1];
  kbc_id_for_artifact(alpha, "kb", "alpha.md");
  kbc_id_for_artifact(ops, "kb", "ops/deploy.md");

  char path[256];
  snprintf(path, sizeof path, "/api/kb/kb/notes/%s/links", alpha);
  char body[16384];
  int status = get_body(&s, path, body, sizeof body);
  KBC_CHECK_MSG(status == 200, "note links: status %d (%s)", status, body);
  KBC_CHECK_MSG(strstr(body, "\"outgoing\":[") != NULL,
                "no outgoing array: %s", body);
  KBC_CHECK_MSG(strstr(body, "\"backlinks\":[") != NULL,
                "no backlinks array: %s", body);

  /* ONE row for the destination written two ways. This is the dedup, and it
   * is keyed on the parser's corpus-relative target — the same identity the
   * `edges` row is keyed on, so the route cannot describe a link twice that
   * the graph holds once. */
  KBC_CHECK_MSG(count_of(body, "\"target\":\"ops/deploy.md\"") == 1,
                "two spellings of one destination did not collapse: %s", body);

  char want_resolved[512];
  snprintf(want_resolved, sizeof want_resolved,
           "{\"target\":\"ops/deploy.md\",\"state\":\"resolved\",\"id\":\"%s\","
           "\"kb\":\"kb\",\"title\":\"Deploy runbook\","
           "\"source_relative\":\"ops/deploy.md\",\"is_note\":false}",
           ops);
  KBC_CHECK_MSG(strstr(body, want_resolved) != NULL,
                "the resolved row is not the original's shape: %s", body);

  /* THE AMBIGUOUS ROW, WHOLY. This is the original's deliberate information
   * loss (links.rs:240-249): the ladder's candidate ids are dropped, and with
   * them the title, the path and the kind, because there is no document to
   * describe. Asserting the WHOLE row rather than the mere absence of `"id"`
   * is what makes this a test of the port's decision — a build that smuggled
   * the candidates through some other member still fails here. */
  KBC_CHECK_MSG(
      strstr(body,
             "{\"target\":\"deploy\",\"state\":\"ambiguous\",\"kb\":\"kb\","
             "\"is_note\":false}") != NULL,
      "the ambiguous row is not the original's id-less shape: %s", body);

  KBC_CHECK_MSG(
      strstr(body,
             "{\"target\":\"nowhere\",\"state\":\"dangling\",\"kb\":\"kb\","
             "\"is_note\":false}") != NULL,
      "a target no document answers to is not `dangling`: %s", body);

  /* THE SELF-LINK IS REPORTED. The write path drops it — a document is not a
   * backlink of itself — and the read path must not, because the body still
   * has to render the link (links.rs:196-200). */
  char want_self[512];
  snprintf(want_self, sizeof want_self,
           "{\"target\":\"alpha.md\",\"state\":\"resolved\",\"id\":\"%s\","
           "\"kb\":\"kb\",\"title\":\"Alpha\","
           "\"source_relative\":\"alpha.md\",\"is_note\":false}",
           alpha);
  KBC_CHECK_MSG(strstr(body, want_self) != NULL,
                "a note's link to itself is not reported: %s", body);

  /* And it is NOT in its own backlinks: the other half of the same decision,
   * reported as outgoing and absent as inbound. The two inbound edges are
   * `infra/deploy.md` and `ops/deploy.md`, and the order is the original's —
   * notes first, then alphabetical by title (links.rs:316-320). */
  snprintf(path, sizeof path, "/api/kb/kb/backlinks/%s", alpha);
  status = get_body(&s, path, body, sizeof body);
  KBC_CHECK_MSG(status == 200, "backlinks: status %d (%s)", status, body);
  const char *inf = strstr(body, "\"source_relative\":\"infra/deploy.md\"");
  const char *opsr = strstr(body, "\"source_relative\":\"ops/deploy.md\"");
  KBC_CHECK_MSG(inf != NULL && opsr != NULL,
                "alpha is missing an inbound edge: %s", body);
  KBC_CHECK_MSG(inf != NULL && opsr != NULL && inf < opsr,
                "backlinks are not in the original's order: %s", body);
  KBC_CHECK_MSG(strstr(body, "\"folder\":\"infra\"") != NULL &&
                    strstr(body, "\"folder\":\"ops\"") != NULL,
                "a backlink row has no folder: %s", body);
  KBC_CHECK_MSG(strstr(body, "\"source_relative\":\"alpha.md\"") == NULL,
                "a document is a backlink of itself: %s", body);

  srv_stop(&s);
  fx_teardown(&f);
}

KBC_TEST(backlinks_of_an_unlinked_document_is_an_empty_array_not_a_404) {
  fixture f;
  links_fixture(&f);
  corpus_file(&f, "zeta.md", "# Zeta\n\nnothing links here\n");
  kbc_err err;
  kbc_err_reset(&err);
  KBC_CHECK_OK(kbc_app_reindex(f.app, &err));
  server s;
  srv_start(&s, &f);
  if (s.h == NULL) {
    fx_teardown(&f);
    return;
  }
  char zeta[KBC_MAX_ID_LEN + 1];
  kbc_id_for_artifact(zeta, "kb", "zeta.md");
  char path[256];
  char body[8192];

  /* "Nothing links here" is an ordinary fact about an ordinary document
   * (links.rs:324-325), so it is a 200 with an empty array. */
  snprintf(path, sizeof path, "/api/kb/kb/backlinks/%s", zeta);
  int status = get_body(&s, path, body, sizeof body);
  KBC_CHECK_MSG(status == 200, "an unlinked document answered %d: %s", status,
                body);
  KBC_CHECK_MSG(strstr(body, "\"backlinks\":[]") != NULL,
                "nothing links here is not an empty array: %s", body);

  /* An id the store does not hold IS a 404 — a different fact, and it gets a
   * different answer. */
  status = get_body(&s, "/api/kb/kb/backlinks/000000000000", body, sizeof body);
  KBC_CHECK_MSG(status == 404, "an unknown id answered %d: %s", status, body);

  /* So is an unknown corpus. */
  snprintf(path, sizeof path, "/api/kb/nope/backlinks/%s", zeta);
  status = get_body(&s, path, body, sizeof body);
  KBC_CHECK_MSG(status == 404, "an unknown kb answered %d: %s", status, body);

  /* The verb is not negotiable: a POST here is a 405, not a 404, so a client
   * can tell "wrong verb" from "no such thing". */
  {
    char req[512];
    int n = snprintf(req, sizeof req,
                     "POST /api/kb/kb/backlinks/%s HTTP/1.1\r\nHost: x\r\n"
                     "Content-Length: 2\r\n\r\n{}",
                     zeta);
    char reply[4096];
    int st2 = 0;
    KBC_CHECK(raw_exchange(&s, req, (size_t)n, &st2, reply, sizeof reply));
    KBC_CHECK_MSG(st2 == 405, "POST on a GET route answered %d: %s", st2, reply);
  }
  srv_stop(&s);
  fx_teardown(&f);
}

KBC_TEST(wikilinks_suggest_ranks_a_title_prefix_above_a_substring) {
  fixture f;
  fx_setup(&f, NULL);
  corpus_file(&f, "deploy.md", "# Deploy\n\nthe plain one\n");
  corpus_file(&f, "ops/deploy-runbook.md", "# Runbook for deploy\n\nx\n");
  corpus_file(&f, "ops/rollback.md", "# Rollback\n\ny\n");
  kbc_err err;
  kbc_err_reset(&err);
  KBC_CHECK_OK(kbc_app_reindex(f.app, &err));
  f.reindexed = true;
  server s;
  srv_start(&s, &f);
  if (s.h == NULL) {
    fx_teardown(&f);
    return;
  }
  char body[16384];
  /* `deploy` is a PREFIX of the title "Deploy" and a SUBSTRING of "Runbook
   * for deploy", so both tiers are represented and their order IS the
   * assertion: a corpus-order answer puts "Runbook for deploy" first, and the
   * store's order is not the ranking. */
  int status =
      get_body(&s, "/api/kb/kb/wikilinks/suggest?q=deploy", body, sizeof body);
  KBC_CHECK_MSG(status == 200, "suggest: status %d (%s)", status, body);
  const char *prefix = strstr(body, "\"source_relative\":\"deploy.md\"");
  const char *sub = strstr(body, "\"source_relative\":\"ops/deploy-runbook.md\"");
  KBC_CHECK_MSG(prefix != NULL, "the rank-0 title prefix is missing: %s", body);
  KBC_CHECK_MSG(sub != NULL, "the rank-1 title substring is missing: %s", body);
  KBC_CHECK_MSG(prefix != NULL && sub != NULL && prefix < sub,
                "a title substring outranked a title prefix: %s", body);
  KBC_CHECK_MSG(strstr(body, "\"source_relative\":\"ops/rollback.md\"") == NULL,
                "a document matching nothing was suggested: %s", body);

  /* `?q=` is required: SuggestQuery types it as a non-Option String
   * (links.rs:113-118), so the request is rejected before the handler runs. A
   * default of "" would make the whole corpus a legal answer to a typo. */
  status = get_body(&s, "/api/kb/kb/wikilinks/suggest", body, sizeof body);
  KBC_CHECK_MSG(status == 400, "a missing q answered %d: %s", status, body);
  status = get_body(&s, "/api/kb/kb/wikilinks/suggest?q=deploy&limit=0", body,
                    sizeof body);
  KBC_CHECK_MSG(status == 400, "limit=0 answered %d: %s", status, body);

  srv_stop(&s);
  fx_teardown(&f);
}

/* ============================================ the comment anchor events ---- */

/* One document, two OPEN comments, and the two headings their anchors name.
 * Open is the only kind the pass walks; the original filters `c.is_open()`
 * (indexer.rs:3035) and so does this. */
static void anchor_fixture(fixture *f) {
  fx_setup(f, NULL);
  corpus_file(f, "notes.md",
              "# Notes\n\n## Ledger\n\nledger line\n\n## Digest\n\ndigest "
              "line\n");
  kbc_err err;
  kbc_err_reset(&err);
  KBC_CHECK_OK(kbc_app_reindex(f->app, &err));
  char doc[KBC_MAX_ID_LEN + 1];
  kbc_id_for_artifact(doc, "kb", "notes.md");
  KBC_CHECK_OK(
      kbc_store_add_comment(kbc_app_store(f->app), doc, "section:ledger", "nik",
                            "on the ledger", &err));
  KBC_CHECK_OK(
      kbc_store_add_comment(kbc_app_store(f->app), doc, "section:digest", "nik",
                            "on the digest", &err));
  f->reindexed = true;
}

/* The id of the comment whose anchor is `section:<which>`. add_comment mints
 * the id and does not hand it back, so a test reads it back the way any other
 * caller would. */
static bool comment_id_for(fixture *f, const char *doc, const char *which,
                           char *out, size_t cap) {
  kbc_arena *a = kbc_arena_new(4096);
  if (a == NULL) return false;
  kbc_comment *cs = NULL;
  size_t n = 0;
  kbc_err err;
  kbc_err_reset(&err);
  kbc_status st =
      kbc_store_list_comments(kbc_app_store(f->app), a, doc, 16, &cs, &n, &err);
  bool ok = false;
  char want[64];
  snprintf(want, sizeof want, "section:%s", which);
  for (size_t i = 0; st == KBC_OK && i < n; i++) {
    if (strcmp(cs[i].anchor, want) == 0 && strlen(cs[i].id) + 1 <= cap) {
      snprintf(out, cap, "%s", cs[i].id);
      ok = true;
      break;
    }
  }
  kbc_arena_free(a);
  return ok;
}

/* Reconnects a stream with Last-Event-ID: 0, which replays the WHOLE ring and
 * is not probed as gapped (a cursor of 0 is a cold client, events.rs:232-233).
 * Reconnecting per step is what makes each assertion about the ring's
 * CONTENTS rather than about a race between the test and the fan-out.
 *
 * Its own socket with a SHORT receive timeout, not sse_open_head's 5 s one: a
 * stream never ends, so the loop terminates on the first idle poll, and a
 * 5 s timeout would make each of the four drains below cost five seconds. */
static bool drain_ring(int port, sse_client *c) {
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
  static const char req[] =
      "GET /api/events HTTP/1.1\r\nHost: x\r\nLast-Event-ID: 0\r\n\r\n";
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
  tv.tv_sec = 0;
  tv.tv_usec = 300000;
  (void)setsockopt(c->fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
  for (int i = 0; i < 6; i++) {
    if (!sse_poll(c)) break;
  }
  return true;
}

static bool reindex_over(int port, char *reply, size_t cap, int *status) {
  static const char req[] =
      "POST /api/reindex HTTP/1.1\r\nHost: x\r\nContent-Length: 2\r\n\r\n{}";
  return raw_exchange_port(port, req, strlen(req), status, reply, cap);
}

KBC_TEST(anchor_events_fire_on_the_transition_and_only_on_the_transition) {
  fixture f;
  anchor_fixture(&f);
  char doc[KBC_MAX_ID_LEN + 1];
  kbc_id_for_artifact(doc, "kb", "notes.md");
  char ledger[KBC_MAX_ID_LEN + 1];
  char digest[KBC_MAX_ID_LEN + 1];
  KBC_CHECK(comment_id_for(&f, doc, "ledger", ledger, sizeof ledger));
  KBC_CHECK(comment_id_for(&f, doc, "digest", digest, sizeof digest));
  KBC_CHECK_MSG(strcmp(ledger, digest) != 0, "both comments got one id: %s",
                ledger);

  kbc_httpd *h = NULL;
  int port = probe_server(&f, &h);
  if (h == NULL) {
    fx_teardown(&f);
    return;
  }
  char reply[4096];
  int status = 0;
  sse_client c;

  /* PASS 1 — both anchors resolve. A comment whose anchor is intact is a
   * steady state and must be SILENT; an `anchor_stale` here would be the
   * daemon inventing a problem the corpus does not have. */
  KBC_CHECK(reindex_over(port, reply, sizeof reply, &status));
  KBC_CHECK_MSG(status == 202, "reindex answered %d: %s", status, reply);
  KBC_CHECK(drain_ring(port, &c));
  KBC_CHECK_MSG(count_of(c.buf, "event: comment.anchor_stale") == 0,
                "a resolved anchor reported itself stale: %s", c.buf);
  KBC_CHECK_MSG(count_of(c.buf, "event: comment.anchor_resolved") == 0,
                "a comment that was never stale reported itself resolved: %s",
                c.buf);
  sse_hangup(&c);

  /* PASS 2 — the Ledger heading is gone from the bytes, so `section:ledger`
   * names an element the document does not have. That is the 0 -> 1 edge the
   * original calls Stale (indexer.rs:3050-3081). The digest comment is
   * untouched and must stay silent IN THE SAME PASS. */
  corpus_file(&f, "notes.md", "# Notes\n\n## Digest\n\ndigest line\n");
  KBC_CHECK(reindex_over(port, reply, sizeof reply, &status));
  KBC_CHECK(drain_ring(port, &c));
  KBC_CHECK_MSG(count_of(c.buf, "event: comment.anchor_stale") == 1,
                "the stale transition did not fire exactly once: %s", c.buf);
  KBC_CHECK_MSG(count_of(c.buf, "event: comment.anchor_resolved") == 0,
                "an untouched comment fired a transition: %s", c.buf);
  /* The PAYLOAD, member for member. These names are the contract: a client
   * subscribes to the event and reads these keys, so a renamed or dropped
   * member is a broken client and not a cosmetic change. `fuzzy_score` is the
   * float 0.0 because `Resolution::Stale` is a unit variant that reports no
   * similarity (indexer.rs:3074). */
  char want_stale[768];
  snprintf(want_stale, sizeof want_stale,
           "event: comment.anchor_stale\nid: 3\ndata: {\"kb\":\"kb\","
           "\"artifact_id\":\"%s\",\"comment_id\":\"%s\","
           "\"anchor_kind\":\"section\",\"fuzzy_score\":0.0,"
           "\"source_relative\":\"notes.md\"}",
           doc, ledger);
  KBC_CHECK_MSG(strstr(c.buf, want_stale) != NULL,
                "the stale payload is not the original's: %s", c.buf);
  sse_hangup(&c);

  /* PASS 3 — re-index the SAME bytes. The anchor is still stale, and the
   * original is explicit that this arm is silent (indexer.rs:3083-3086): the
   * subscriber already painted the badge. This is the case that fails if the
   * stale set is not consulted. */
  KBC_CHECK(reindex_over(port, reply, sizeof reply, &status));
  KBC_CHECK(drain_ring(port, &c));
  KBC_CHECK_MSG(count_of(c.buf, "event: comment.anchor_stale") == 1,
                "a still-stale anchor fired a second time: %s", c.buf);
  sse_hangup(&c);

  /* PASS 4 — Ledger comes back and Digest goes. TWO transitions in ONE pass,
   * in OPPOSITE directions: the ledger comment resolves (1 -> 0) while the
   * digest comment goes stale (0 -> 1). A pass that stopped at the first
   * transition would report only one of them, and the two counts below are
   * what say so. */
  corpus_file(&f, "notes.md", "# Notes\n\n## Ledger\n\nledger line\n");
  KBC_CHECK(reindex_over(port, reply, sizeof reply, &status));
  KBC_CHECK(drain_ring(port, &c));
  KBC_CHECK_MSG(count_of(c.buf, "event: comment.anchor_resolved") == 1,
                "the resolved transition did not fire exactly once: %s", c.buf);
  KBC_CHECK_MSG(count_of(c.buf, "event: comment.anchor_stale") == 2,
                "the two-direction pass did not fire both arms: %s", c.buf);
  /* `score` is Option<f32>: a number for a fuzzy match and NULL for an exact
   * one (indexer.rs:3097-3100). kb-c resolves exactly, so it is null — and it
   * is EMITTED as null, because a client that found the key missing would be
   * reading a shape the field never has. */
  char want_resolved[512];
  snprintf(want_resolved, sizeof want_resolved,
           "\"comment_id\":\"%s\",\"score\":null}", ledger);
  KBC_CHECK_MSG(strstr(c.buf, want_resolved) != NULL,
                "the resolved payload is not the original's: %s", c.buf);
  sse_hangup(&c);

  kbc_httpd_stop(h);
  fx_teardown(&f);
}

KBC_TEST(a_resolved_comment_never_reports_a_stale_anchor) {
  /* The original filters `c.is_open()` before it judges anything
   * (indexer.rs:3035), so a RESOLVED comment cannot drag the pass into
   * reporting its anchor as stale. A pass that walked every row would flag
   * every closed thread in the corpus on the first re-index. */
  fixture f;
  anchor_fixture(&f);
  char doc[KBC_MAX_ID_LEN + 1];
  kbc_id_for_artifact(doc, "kb", "notes.md");
  char digest[KBC_MAX_ID_LEN + 1];
  KBC_CHECK(comment_id_for(&f, doc, "digest", digest, sizeof digest));
  kbc_err err;
  kbc_err_reset(&err);
  KBC_CHECK_OK(
      kbc_store_set_comment_resolved(kbc_app_store(f.app), digest, true, &err));
  /* The digest heading is removed, so the comment WOULD be stale if it were
   * still open. The ledger comment is left intact and must not fire either. */
  corpus_file(&f, "notes.md", "# Notes\n\n## Ledger\n\nledger line\n");
  kbc_httpd *h = NULL;
  int port = probe_server(&f, &h);
  if (h == NULL) {
    fx_teardown(&f);
    return;
  }
  char reply[4096];
  int status = 0;
  KBC_CHECK(reindex_over(port, reply, sizeof reply, &status));
  sse_client c;
  KBC_CHECK(drain_ring(port, &c));
  KBC_CHECK_MSG(count_of(c.buf, "event: comment.anchor_stale") == 0,
                "a resolved comment reported a stale anchor: %s", c.buf);
  sse_hangup(&c);
  kbc_httpd_stop(h);
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

/* ================================================================ capture ==
 *
 * `POST /api/kb/{kb}/capture`, over a REAL socket and not the socketless
 * seam. That is forced, not preferred: a multipart body's boundary lives in
 * its Content-Type, and kbc_request (the frozen contract, httpd.h) carries
 * no headers beyond Authorization — so a capture driven through
 * kbc_httpd_handle has no boundary and is a 400, the same answer the socket
 * gives a POST with no Content-Type. Anything that wanted to drive this route
 * without a socket would be testing a fiction no HTTP client could produce.
 */

/* `<fixture root>/kb/<rel>`, with the length CHECKED rather than trusted.
 * `f.root` is already KBC_TEST_PATH_MAX bytes, so a bare snprintf into a
 * KBC_TEST_PATH_MAX buffer is a truncation the compiler is right to flag — and
 * a silently truncated path in a traversal assertion is an assertion about
 * the wrong file, which is worse than no assertion. Returns false rather than
 * writing a partial path. */
static bool corpus_under(fixture *f, const char *rel, char *out, size_t cap) {
  int n = snprintf(out, cap, "%s/kb/%s", f->root, rel);
  return n > 0 && (size_t)n < cap;
}

/* `<fixture root>/<name>` — the sibling-directory and escape cases, which live
 * BESIDE the corpus rather than inside it. Same check, same reason. */
static bool under_root(fixture *f, const char *name, char *out, size_t cap) {
  int n = snprintf(out, cap, "%s/%s", f->root, name);
  return n > 0 && (size_t)n < cap;
}


/* A capture with `n` file parts and no text fields, for the count cap. */
static char *cap_many_files(const char *boundary, size_t n, size_t *out_len) {
  kbc_str b;
  kbc_str_init(&b);
  for (size_t i = 0; i < n; i++) {
    (void)kbc_str_printf(&b, "--%s\r\n", boundary);
    (void)kbc_str_printf(&b,
                         "Content-Disposition: form-data; name=\"files\"; "
                         "filename=\"f%zu.md\"\r\n\r\nx\r\n",
                         i);
  }
  (void)kbc_str_printf(&b, "--%s--\r\n", boundary);
  *out_len = b.len;
  return b.ptr;
}

/* Reads the JSON body out of a raw reply, into `out`. */
static void reply_body(const char *reply, char *out, size_t cap) {
  const char *b = strstr(reply, "\r\n\r\n");
  out[0] = '\0';
  if (b == NULL) return;
  b += 4;
  size_t len = strlen(b);
  if (len >= cap) len = cap - 1;
  memcpy(out, b, len);
  out[len] = '\0';
}

/* The value of a top-level-ish `"key":"..."` in a capture response. */
static bool json_field(const char *json, const char *key, char *out,
                       size_t cap) {
  char pat[64];
  snprintf(pat, sizeof pat, "\"%s\":\"", key);
  const char *p = strstr(json, pat);
  if (p == NULL) return false;
  p += strlen(pat);
  size_t i = 0;
  while (p[i] != '\0' && p[i] != '"' && i + 1 < cap) {
    out[i] = p[i];
    i++;
  }
  out[i] = '\0';
  return i > 0;
}

/* THE HAPPY PATH, and the whole point of the route: a multipart upload comes
 * back as a document the indexer picks up under the id the RESPONSE reported.
 *
 * The id is minted from (corpus, path) on ingest, so a response that named a
 * different id than the index assigns would be a response a client cannot use
 * to find what it just created — which is why the assertion is on the STORE's
 * id and not on the response echoing itself back.
 *
 * It also pins the field names, because a client sends them by name and a
 * rename here is a silent no-op at the far end: `title` steers the output
 * FILENAME and is never stamped into the document, `tags` split on ',' and
 * reach the front matter slugified, and the `from:` tag is derived from the
 * X-Requested-By header (the original's from_default, capture.rs:129-137),
 * which is what `kb-spa` sends. */
KBC_TEST(a_capture_becomes_a_document_the_index_picks_up_under_the_reported_id) {
  fixture f;
  fx_setup(&f, NULL);
  server s;
  srv_start(&s, &f);
  if (s.h == NULL) {
    fx_teardown(&f);
    return;
  }
  static const char bnd[] = "kbcBOUND42";
  static const char doc[] = "# Verdigris\n\nThe digest catalogues verdigris.\n";
  kbc_str b;
  kbc_str_init(&b);
  (void)kbc_str_printf(&b, "--%s\r\n", bnd);
  (void)kbc_str_puts(&b, "Content-Disposition: form-data; name=\"title\"\r\n\r\n"
                         "My Note\r\n");
  (void)kbc_str_printf(&b, "--%s\r\n", bnd);
  (void)kbc_str_puts(&b, "Content-Disposition: form-data; name=\"tags\"\r\n\r\n"
                         "urgent, Delta\r\n");
  (void)kbc_str_printf(&b, "--%s\r\n", bnd);
  (void)kbc_str_puts(&b,
                     "Content-Disposition: form-data; name=\"files\"; "
                     "filename=\"notes.md\"\r\nContent-Type: text/markdown\r\n\r\n");
  (void)kbc_str_append(&b, doc, strlen(doc));
  (void)kbc_str_printf(&b, "\r\n--%s--\r\n", bnd);
  kbc_str rq;
  kbc_str_init(&rq);
  (void)kbc_str_printf(
      &rq,
      "POST /api/kb/kb/capture HTTP/1.1\r\nHost: x\r\n"
      "Content-Type: multipart/form-data; boundary=%s\r\n"
      "X-Requested-By: kb-spa\r\nContent-Length: %zu\r\n\r\n",
      bnd, b.len);
  (void)kbc_str_append(&rq, b.ptr, b.len);
  kbc_str_free(&b);

  char reply[16384];
  int status = 0;
  KBC_CHECK(raw_exchange(&s, rq.ptr, rq.len, &status, reply, sizeof reply));
  KBC_CHECK_MSG(status == 201, "capture did not answer 201: %d (%s)", status,
                reply);
  KBC_CHECK_MSG(strstr(reply, "\"status\":201") == NULL,
                "a 201 must not be dressed as a problem: %s", reply);
  char json[8192];
  reply_body(reply, json, sizeof json);
  char id[KBC_MAX_ID_LEN + 1], rel[KBC_MAX_PATH_LEN + 1];
  KBC_CHECK_MSG(json_field(json, "id", id, sizeof id), "no id in: %s", json);
  KBC_CHECK_MSG(json_field(json, "source_relative", rel, sizeof rel),
                "no source_relative in: %s", json);
  char kbname[64];
  KBC_CHECK_MSG(json_field(json, "kb", kbname, sizeof kbname) &&
                    strcmp(kbname, "kb") == 0,
                "the item does not name its kb: %s", json);
  /* The response has no title, deliberately: it is the indexer's own
   * title.or(h1).or(stem) chain and is not final until the watcher has run. */
  KBC_CHECK_MSG(strstr(json, "\"title\"") == NULL,
                "the response carries a title, which is not the indexer's: %s",
                json);

  /* The file is really on disk, with the stamp the route exists to write. */
  char p[KBC_TEST_PATH_MAX + 64];
  KBC_CHECK_MSG(corpus_under(&f, rel, p, sizeof p), "corpus path over the cap");
  char *written = kbc_test_read_file(p);
  KBC_CHECK_MSG(written != NULL, "no file at %s", p);
  if (written != NULL) {
    KBC_CHECK_MSG(strstr(written, "kb-category: capture") != NULL,
                  "no kb-category in:\n%s", written);
    KBC_CHECK_MSG(strstr(written, "kb-capture-original: notes.md") != NULL,
                  "no kb-capture-original in:\n%s", written);
    /* `title` steers the FILENAME and is never stamped. */
    KBC_CHECK_MSG(strstr(written, "My Note") == NULL,
                  "the title was stamped into the document:\n%s", written);
    /* tags split on ',' and each is slugified; `from` came from the header
     * with its `kb-` prefix stripped, so the tag reads from:spa. */
    KBC_CHECK_MSG(strstr(written, "kb-tags: source:upload, from:spa, urgent, "
                                  "delta") != NULL,
                  "the tag list is not the one the fields describe:\n%s",
                  written);
    free(written);
  }
  /* And the filename is the slug of the title, plus the capture's second. */
  KBC_CHECK_MSG(strncmp(rel, "capture/my-note-", 16) == 0,
                "the title did not steer the filename: %s", rel);

  /* The next pass indexes it under exactly the id the response reported. */
  kbc_err err;
  kbc_err_reset(&err);
  KBC_CHECK_OK(kbc_app_reindex(f.app, &err));
  kbc_arena *a = kbc_arena_new(64u * 1024u);
  KBC_CHECK_NOT_NULL(a);
  if (a != NULL) {
    kbc_artifact art;
    memset(&art, 0, sizeof art);
    KBC_CHECK_OK(kbc_app_get_artifact(f.app, a, id, false, &art, &err));
    KBC_CHECK_EQ_STR(art.corpus, "kb");
    KBC_CHECK_EQ_STR(art.path, rel);
    KBC_CHECK_MSG(strstr(art.summary, "verdigris") != NULL,
                  "the indexed document is not the one uploaded: %s",
                  art.summary);
    kbc_arena_free(a);
  }
  kbc_str_free(&rq);
  srv_stop(&s);
  fx_teardown(&f);
}

/* THE CAP ON THE NUMBER OF FILES, and the failure mode it exists to prevent.
 *
 * 51 files is REFUSED with a 400 and nothing is written. The outcome worth
 * being afraid of is the other one: the decoder's array holds 56 parts, so a
 * caller that sized its own array to the wrong number and reported success
 * would have captured 50 of 51 files and told the client all 51 were created.
 * So the assertion is not merely "some 4xx came back" — it is that the corpus
 * gained NO file, which is the difference between a refusal and a truncated
 * upload reported as success.
 *
 * 60 parts also trips the DECODER's own cap (56), and must be refused on the
 * same grounds rather than quietly reading 56. */
KBC_TEST(a_capture_past_the_file_count_cap_is_refused_and_writes_nothing) {
  fixture f;
  fx_setup(&f, NULL);
  server s;
  srv_start(&s, &f);
  if (s.h == NULL) {
    fx_teardown(&f);
    return;
  }
  for (size_t n = 51; n <= 60; n += 9) {
    static const char bnd[] = "kbcBOUND43";
    size_t blen = 0;
    char *body = cap_many_files(bnd, n, &blen);
    KBC_CHECK_NOT_NULL(body);
    if (body == NULL) continue;
    kbc_str rq;
    kbc_str_init(&rq);
    (void)kbc_str_printf(
        &rq,
        "POST /api/kb/kb/capture HTTP/1.1\r\nHost: x\r\n"
        "Content-Type: multipart/form-data; boundary=%s\r\n"
        "Content-Length: %zu\r\n\r\n%s",
        bnd, blen, body);
    free(body);
    char reply[16384];
    int status = 0;
    KBC_CHECK(raw_exchange(&s, rq.ptr, rq.len, &status, reply, sizeof reply));
    KBC_CHECK_MSG(status == 400, "%zu files: got status %d, want 400: %s", n,
                  status, reply);
    KBC_CHECK_MSG(strstr(reply, "too many") != NULL,
                  "%zu files: the 400 does not say what was too many: %s", n,
 reply);
    kbc_str_free(&rq);
  }
  /* Not one file: the whole point. */
  char dir[KBC_TEST_PATH_MAX + 64];
  KBC_CHECK(corpus_under(&f, "capture", dir, sizeof dir));
  KBC_CHECK_MSG(!kbc_path_exists(dir),
                "a refused capture created %s anyway", dir);
  srv_stop(&s);
  fx_teardown(&f);
}

/* AN OVERSIZED PART IS REFUSED. On this port the ceiling that answers is the
 * request parser's own: a Content-Length over KBC_MAX_SNIFF_BYTES is a 413
 * before a route is chosen, which is strictly below the capture route's 10 MiB
 * per-file budget (capture.rs:38) and its 64 MiB request budget (:46). Both
 * are enforced in the route as well — a cap that lives only in a buffer size
 * in another file is a cap that stops existing the day that size moves — but
 * the wire cannot reach them, and a test that pretended otherwise would be
 * asserting a fiction.
 *
 * What this pins is the observable contract: a body over the ceiling is a 413
 * and the corpus gains nothing. Both halves fail loudly if the guard is
 * removed: without it the capture would be attempted, and without the "writes
 * nothing" half a partial write would pass. */
KBC_TEST(an_oversized_capture_body_is_refused_and_writes_nothing) {
  fixture f;
  fx_setup(&f, NULL);
  server s;
  srv_start(&s, &f);
  if (s.h == NULL) {
    fx_teardown(&f);
    return;
  }
  /* One part just over 64 KiB, which is over KBC_MAX_SNIFF_BYTES. */
  const size_t big = 70000u;
  char *body = malloc(big + 512);
  KBC_CHECK_NOT_NULL(body);
  if (body != NULL) {
    static const char bnd[] = "kbcBOUND44";
    int n = snprintf(body, 512,
                     "--%s\r\n"
                     "Content-Disposition: form-data; name=\"files\"; "
                     "filename=\"big.md\"\r\n\r\n",
                     bnd);
    KBC_CHECK(n > 0);
    memset(body + n, 'A', big);
    n += (int)big;
    n += snprintf(body + n, 64, "\r\n--%s--\r\n", bnd);
    kbc_str rq;
    kbc_str_init(&rq);
    (void)kbc_str_printf(
        &rq,
        "POST /api/kb/kb/capture HTTP/1.1\r\nHost: x\r\n"
        "Content-Type: multipart/form-data; boundary=%s\r\n"
        "Content-Length: %d\r\n\r\n%s",
        bnd, n, body);
    char reply[8192];
    int status = 0;
    KBC_CHECK(raw_exchange(&s, rq.ptr, rq.len, &status, reply, sizeof reply));
    KBC_CHECK_MSG(status == 413, "an oversized capture got status %d: %s",
                  status, reply);
    KBC_CHECK_MSG(strstr(reply, "application/problem+json") != NULL,
                  "the 413 is not problem+json: %s", reply);
    kbc_str_free(&rq);
    free(body);
  }
  char dir[KBC_TEST_PATH_MAX + 64];
  KBC_CHECK(corpus_under(&f, "capture", dir, sizeof dir));
  KBC_CHECK_MSG(!kbc_path_exists(dir),
                "a refused oversized capture created %s anyway", dir);
  srv_stop(&s);
  fx_teardown(&f);
}

/* THE WRITE DIRECTION'S CONTAINMENT, which the read-side traversal cases do
 * not cover. A capture is the one route where a caller-supplied string is
 * turned into a PATH ON DISK rather than looked up, so a capture steered with
 * traversal in every field it accepts must land inside the corpus and nowhere
 * else.
 *
 * The sibling case is the one that matters, and it is why this is not a plain
 * "../" test: a guard written as a string prefix accepts `<root>/kb-secret`
 * for a root of `<root>/kb`, and only a component-wise comparison refuses it.
 * A `..` test passes under both implementations and so separates nothing.
 *
 * `capture_dir` is the load-bearing field here: the original resolves it from
 * the kb's config and kb-c has no config key, so the route leaves it NULL. A
 * future change that let the request supply it is exactly the change this
 * fails on. */
KBC_TEST(a_capture_steered_out_of_the_corpus_root_writes_nothing_outside_it) {
  fixture f;
  fx_setup(&f, NULL);
  /* The sibling of the corpus root, sharing its first path components. */
  char sibling[KBC_TEST_PATH_MAX + 64];
  KBC_CHECK(under_root(&f, "kb-secret", sibling, sizeof sibling));
  kbc_test_mkdir_p(sibling);
  char canary[KBC_TEST_PATH_MAX + 128];
  {
    int n = snprintf(canary, sizeof canary, "%s/canary.md", sibling);
    KBC_CHECK(n > 0 && (size_t)n < sizeof canary);
  }
  kbc_test_write_file(canary, "# Canary\n\nuntouched\n");
  server s;
  srv_start(&s, &f);
  if (s.h == NULL) {
    fx_teardown(&f);
    return;
  }
  static const char bnd[] = "kbcBOUND45";
  kbc_str b;
  kbc_str_init(&b);
  (void)kbc_str_puts(&b, "--");
  (void)kbc_str_puts(&b, bnd);
  (void)kbc_str_puts(&b, "\r\nContent-Disposition: form-data; "
                       "name=\"title\"\r\n\r\n../../kb-secret/pwned\r\n");
  (void)kbc_str_puts(&b, "--");
  (void)kbc_str_puts(&b, bnd);
  (void)kbc_str_puts(&b, "\r\nContent-Disposition: form-data; "
                       "name=\"capture_dir\"\r\n\r\n../../kb-secret\r\n");
  (void)kbc_str_puts(&b, "--");
  (void)kbc_str_puts(&b, bnd);
  (void)kbc_str_puts(&b, "\r\nContent-Disposition: form-data; name=\"files\"; "
                       "filename=\"../../../etc/pwn.md\"\r\n\r\n");
  (void)kbc_str_puts(&b, "# Pwn\n\nx\n");
  (void)kbc_str_puts(&b, "\r\n--");
  (void)kbc_str_puts(&b, bnd);
  (void)kbc_str_puts(&b, "--\r\n");
  kbc_str rq;
  kbc_str_init(&rq);
  (void)kbc_str_printf(
      &rq,
      "POST /api/kb/kb/capture HTTP/1.1\r\nHost: x\r\n"
      "Content-Type: multipart/form-data; boundary=%s\r\n"
      "Content-Length: %zu\r\n\r\n%s",
      bnd, b.len, b.ptr);
  kbc_str_free(&b);
  char reply[16384];
  int status = 0;
  KBC_CHECK(raw_exchange(&s, rq.ptr, rq.len, &status, reply, sizeof reply));
  /* The upload is accepted — the slug has nothing left to traverse with — and
   * what matters is WHERE it landed. */
  KBC_CHECK_MSG(status == 201, "the steered capture got status %d: %s", status,
                reply);
  char json[8192];
  reply_body(reply, json, sizeof json);
  char rel[KBC_MAX_PATH_LEN + 1];
  KBC_CHECK_MSG(json_field(json, "source_relative", rel, sizeof rel),
                "no source_relative in: %s", json);
  /* Inside the corpus, under the default capture dir, with every separator
 * and dot collapsed to a dash by the slug. */
  KBC_CHECK_MSG(strncmp(rel, "capture/", 8) == 0,
                "the capture landed outside the capture dir: %s", rel);
  KBC_CHECK_MSG(strstr(rel, "..") == NULL && strstr(rel, "/") == rel + 7,
                "the path still carries a separator or a dot-dot: %s", rel);
  /* And the sibling of the corpus root is untouched — same first component,
   * so a string-prefix guard would have let this through. */
  char *left = kbc_test_read_file(canary);
  KBC_CHECK_NOT_NULL(left);
  if (left != NULL) {
    KBC_CHECK_MSG(strstr(left, "untouched") != NULL,
                  "the sibling directory was written to:\n%s", left);
    free(left);
  }
  char escaped[KBC_TEST_PATH_MAX + 128];
  for (size_t i = 0; i < 2; i++) {
    const char *leaf = i == 0 ? "pwned.md" : "pwn.md";
    int n = snprintf(escaped, sizeof escaped, "%s/%s", sibling, leaf);
    KBC_CHECK(n > 0 && (size_t)n < sizeof escaped);
    KBC_CHECK_MSG(!kbc_path_exists(escaped), "a capture wrote %s", escaped);
  }
  kbc_str_free(&rq);
  srv_stop(&s);
  fx_teardown(&f);
}

/* THE SSRF RULING, again, now that there is a NETWORK PATH to it. The route
 * records a `url` and never dereferences it; a listener is bound and pointed
 * at, and the capture must complete without a single packet reaching it. This
 * is the case that fails if someone helpfully adds a fetch to "enrich" the
 * capture, which is why it is here and not only in test_app.c. */
KBC_TEST(a_capture_with_a_url_field_never_connects_to_it) {
  int lfd = socket(AF_INET, SOCK_STREAM, 0);
  KBC_CHECK(lfd >= 0);
  if (lfd < 0) return;
  int one = 1;
  setsockopt(lfd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
  struct sockaddr_in addr;
  memset(&addr, 0, sizeof addr);
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  addr.sin_port = 0;
  KBC_CHECK(bind(lfd, (struct sockaddr *)&addr, sizeof addr) == 0);
  KBC_CHECK(listen(lfd, 4) == 0);
  socklen_t alen = sizeof addr;
  KBC_CHECK(getsockname(lfd, (struct sockaddr *)&addr, &alen) == 0);
  char url[128];
  snprintf(url, sizeof url, "http://127.0.0.1:%u/never-fetched",
           (unsigned)ntohs(addr.sin_port));

  fixture f;
  fx_setup(&f, NULL);
  server s;
  srv_start(&s, &f);
  if (s.h == NULL) {
    close(lfd);
    fx_teardown(&f);
    return;
  }
  static const char bnd[] = "kbcBOUND46";
  kbc_str b;
  kbc_str_init(&b);
  (void)kbc_str_printf(&b, "--%s\r\n", bnd);
  (void)kbc_str_puts(&b, "Content-Disposition: form-data; name=\"url\"\r\n\r\n");
  (void)kbc_str_puts(&b, url);
  (void)kbc_str_puts(&b, "\r\n");
  (void)kbc_str_printf(&b, "--%s\r\n", bnd);
  (void)kbc_str_puts(&b,
                     "Content-Disposition: form-data; name=\"files\"; "
                     "filename=\"shared.md\"\r\n\r\n");
  (void)kbc_str_puts(&b, "# Shared\n\nsome shared words\n");
  (void)kbc_str_printf(&b, "\r\n--%s--\r\n", bnd);
  kbc_str rq;
  kbc_str_init(&rq);
  (void)kbc_str_printf(
      &rq,
      "POST /api/kb/kb/capture HTTP/1.1\r\nHost: x\r\n"
      "Content-Type: multipart/form-data; boundary=%s\r\n"
      "Content-Length: %zu\r\n\r\n%s",
      bnd, b.len, b.ptr);
  kbc_str_free(&b);
  char reply[16384];
  int status = 0;
  KBC_CHECK(raw_exchange(&s, rq.ptr, rq.len, &status, reply, sizeof reply));
  KBC_CHECK_MSG(status == 201, "the capture got status %d: %s", status, reply);
  char json[8192];
  reply_body(reply, json, sizeof json);
  char rel[KBC_MAX_PATH_LEN + 1];
  KBC_CHECK_MSG(json_field(json, "source_relative", rel, sizeof rel),
                "no source_relative in: %s", json);
  char p[KBC_TEST_PATH_MAX + 64];
  KBC_CHECK(corpus_under(&f, rel, p, sizeof p));
  char *written = kbc_test_read_file(p);
  KBC_CHECK_NOT_NULL(written);
  if (written != NULL) {
    char line[256];
    snprintf(line, sizeof line, "kb-capture-url: %s", url);
    KBC_CHECK_MSG(strstr(written, line) != NULL,
                  "the url was not recorded as inert text:\n%s", written);
    free(written);
  }
  struct pollfd pfd;
  pfd.fd = lfd;
  pfd.events = POLLIN;
  pfd.revents = 0;
  int ready = poll(&pfd, 1, 100);
  KBC_CHECK_MSG(ready == 0, "the capture route CONNECTED to the url it "
                            "recorded (%d)",
                ready);
  kbc_str_free(&rq);
  close(lfd);
  srv_stop(&s);
  fx_teardown(&f);
}

/* The route's own admission and its shape: POST only, token-gated like every
 * other /api route, a 404 for a corpus that is not configured, and a 400 for
 * a body that is not multipart. The last one is the socketless seam's answer
 * too — kbc_httpd_handle has no Content-Type, so the same request with no
 * boundary is a 400 rather than a capture of nothing. */
KBC_TEST(the_capture_route_is_post_only_and_gated) {
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
  const char *get =
      "GET /api/kb/kb/capture HTTP/1.1\r\nHost: x\r\nContent-Length: 0\r\n\r\n";
  KBC_CHECK(raw_exchange(&s, get, strlen(get), &status, reply, sizeof reply));
  KBC_CHECK_EQ_INT(status, 405);
  KBC_CHECK_MSG(strstr(reply, "\"status\":405") != NULL,
                "the 405 is not problem+json: %s", reply);

  /* No Content-Type: no boundary, so nothing to split. */
  const char *plain =
      "POST /api/kb/kb/capture HTTP/1.1\r\nHost: x\r\nContent-Length: 2\r\n\r\n{}";
  KBC_CHECK(raw_exchange(&s, plain, strlen(plain), &status, reply, sizeof reply));
  KBC_CHECK_EQ_INT(status, 400);
  KBC_CHECK_MSG(strstr(reply, "boundary") != NULL,
                "the 400 does not say what was missing: %s", reply);

  /* An unknown corpus is a 404, and it is refused BEFORE the body is looked
   * at — a capture into a corpus that does not exist has no destination. */
  const char *nokb =
      "POST /api/kb/nope/capture HTTP/1.1\r\nHost: x\r\n"
      "Content-Type: multipart/form-data; boundary=B\r\nContent-Length: 2\r\n\r\n{}";
  KBC_CHECK(
      raw_exchange(&s, nokb, strlen(nokb), &status, reply, sizeof reply));
  KBC_CHECK_EQ_INT(status, 404);
  KBC_CHECK_MSG(strstr(reply, "no corpus named nope") != NULL,
                "the 404 does not name the corpus: %s", reply);

  /* An empty batch is a 400 and not an empty 201: a caller that sent nothing
   * and got `{"items":[]}` would have to guess whether the upload landed. */
  static const char bnd[] = "kbcBOUND47";
  const char empty[] =
      "--kbcBOUND47--\r\n";
  char req[512];
  int n = snprintf(req, sizeof req,
                   "POST /api/kb/kb/capture HTTP/1.1\r\nHost: x\r\n"
                   "Content-Type: multipart/form-data; boundary=%s\r\n"
                   "Content-Length: %zu\r\n\r\n%s",
                   bnd, sizeof empty - 1, empty);
  KBC_CHECK(n > 0 && (size_t)n < sizeof req);
  if (n > 0 && (size_t)n < sizeof req) {
    KBC_CHECK(raw_exchange(&s, req, (size_t)n, &status, reply, sizeof reply));
    KBC_CHECK_EQ_INT(status, 400);
    KBC_CHECK_MSG(strstr(reply, "at least one file") != NULL,
                  "the 400 does not say what was missing: %s", reply);
  }
  srv_stop(&s);
  fx_teardown(&f);
}

/* An extension the corpus will not index is a 415, and the batch fails WHOLE:
 * a `good.md` left behind beside a 415 would make a retry duplicate it, which
 * is the whole reason the original pre-resolves every file's extension before
 * the first write (capture.rs:309-315). */
KBC_TEST(a_mixed_batch_with_one_unindexable_extension_writes_nothing) {
  fixture f;
  fx_setup(&f, NULL);
  server s;
  srv_start(&s, &f);
  if (s.h == NULL) {
    fx_teardown(&f);
    return;
  }
  static const char bnd[] = "kbcBOUND48";
  kbc_str b;
  kbc_str_init(&b);
  (void)kbc_str_printf(&b, "--%s\r\n", bnd);
  (void)kbc_str_puts(&b,
                     "Content-Disposition: form-data; name=\"files\"; "
                     "filename=\"good.md\"\r\n\r\n# Good\n\nx\n");
  (void)kbc_str_printf(&b, "\r\n--%s\r\n", bnd);
  (void)kbc_str_puts(&b,
                     "Content-Disposition: form-data; name=\"files\"; "
                     "filename=\"bad.xyz\"\r\n\r\n# Bad\n\ny\n");
  (void)kbc_str_printf(&b, "\r\n--%s--\r\n", bnd);
  kbc_str rq;
  kbc_str_init(&rq);
  (void)kbc_str_printf(
      &rq,
      "POST /api/kb/kb/capture HTTP/1.1\r\nHost: x\r\n"
      "Content-Type: multipart/form-data; boundary=%s\r\n"
      "Content-Length: %zu\r\n\r\n%s",
      bnd, b.len, b.ptr);
  kbc_str_free(&b);
  char reply[16384];
  int status = 0;
  KBC_CHECK(raw_exchange(&s, rq.ptr, rq.len, &status, reply, sizeof reply));
  KBC_CHECK_EQ_INT(status, 415);
  KBC_CHECK_MSG(strstr(reply, "bad.xyz") != NULL,
                "the 415 does not name the offending file: %s", reply);
  char dir[KBC_TEST_PATH_MAX + 64];
  KBC_CHECK(corpus_under(&f, "capture", dir, sizeof dir));
  KBC_CHECK_MSG(!kbc_path_exists(dir),
                "a 415'd batch still wrote into %s", dir);
  kbc_str_free(&rq);
  srv_stop(&s);
  fx_teardown(&f);
}

/* A `url`/`text` share with no file is the OTHER half of the route: it writes
 * one text stub instead of a copy of an upload, and it is the path on which
 * the url could most plausibly be "helpfully" fetched. */
KBC_TEST(a_url_share_with_no_file_writes_a_stub) {
  fixture f;
  fx_setup(&f, NULL);
  server s;
  srv_start(&s, &f);
  if (s.h == NULL) {
    fx_teardown(&f);
    return;
  }
  static const char bnd[] = "kbcBOUND49";
  kbc_str b;
  kbc_str_init(&b);
  (void)kbc_str_printf(&b, "--%s\r\n", bnd);
  (void)kbc_str_puts(&b, "Content-Disposition: form-data; name=\"title\"\r\n\r\n"
                         "A Shared Page\r\n");
  (void)kbc_str_printf(&b, "--%s\r\n", bnd);
  (void)kbc_str_puts(&b, "Content-Disposition: form-data; name=\"url\"\r\n\r\n"
                         "https://example.com/page\r\n");
  (void)kbc_str_printf(&b, "--%s\r\n", bnd);
  (void)kbc_str_puts(&b, "Content-Disposition: form-data; name=\"text\"\r\n\r\n"
                         "the shared words\n");
  (void)kbc_str_printf(&b, "\r\n--%s--\r\n", bnd);
  kbc_str rq;
  kbc_str_init(&rq);
  (void)kbc_str_printf(
      &rq,
      "POST /api/kb/kb/capture HTTP/1.1\r\nHost: x\r\n"
      "Content-Type: multipart/form-data; boundary=%s\r\n"
      "Content-Length: %zu\r\n\r\n%s",
      bnd, b.len, b.ptr);
  kbc_str_free(&b);
  char reply[16384];
  int status = 0;
  KBC_CHECK(raw_exchange(&s, rq.ptr, rq.len, &status, reply, sizeof reply));
  KBC_CHECK_EQ_INT(status, 201);
  char json[8192];
  reply_body(reply, json, sizeof json);
  char rel[KBC_MAX_PATH_LEN + 1];
  KBC_CHECK_MSG(json_field(json, "source_relative", rel, sizeof rel),
                "no source_relative in: %s", json);
  char p[KBC_TEST_PATH_MAX + 64];
  KBC_CHECK(corpus_under(&f, rel, p, sizeof p));
  char *written = kbc_test_read_file(p);
  KBC_CHECK_NOT_NULL(written);
  if (written != NULL) {
    KBC_CHECK_MSG(strstr(written, "# A Shared Page") != NULL,
 "the stub has no title heading:\n%s", written);
    KBC_CHECK_MSG(strstr(written, "<https://example.com/page>") != NULL,
                  "the url is not in the body as an autolink:\n%s", written);
    KBC_CHECK_MSG(strstr(written, "the shared words") != NULL,
                  "the shared text is missing:\n%s", written);
    /* The raw provenance tag, colon intact: it is appended AFTER the slugify,
     * which would otherwise collapse the colon to a dash. `from:api` is the
     * default the original's from_default gives a caller that named nobody
     * (capture.rs:137) — this request sent no X-Requested-By. */
    KBC_CHECK_MSG(strstr(written,
                         "kb-tags: source:upload, from:api, kind:url-stub") !=
                      NULL,
                  "the url-stub tag is missing:\n%s", written);
    free(written);
  }
  kbc_str_free(&rq);
  srv_stop(&s);
  fx_teardown(&f);
}

/* ------------------------------------------------------------------- main -- */

int main(void) {
  static const kbc_test_case cases[] = {
      {"links_report_resolved_ambiguous_and_dangling",
       links_report_resolved_ambiguous_and_dangling},
      {"backlinks_of_an_unlinked_document_is_an_empty_array_not_a_404",
       backlinks_of_an_unlinked_document_is_an_empty_array_not_a_404},
      {"wikilinks_suggest_ranks_a_title_prefix_above_a_substring",
       wikilinks_suggest_ranks_a_title_prefix_above_a_substring},
      {"anchor_events_fire_on_the_transition_and_only_on_the_transition",
       anchor_events_fire_on_the_transition_and_only_on_the_transition},
      {"a_resolved_comment_never_reports_a_stale_anchor",
       a_resolved_comment_never_reports_a_stale_anchor},
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
      {"a_capture_becomes_a_document_the_index_picks_up_under_the_reported_id",
       a_capture_becomes_a_document_the_index_picks_up_under_the_reported_id},
      {"a_capture_past_the_file_count_cap_is_refused_and_writes_nothing",
       a_capture_past_the_file_count_cap_is_refused_and_writes_nothing},
      {"an_oversized_capture_body_is_refused_and_writes_nothing",
       an_oversized_capture_body_is_refused_and_writes_nothing},
      {"a_capture_steered_out_of_the_corpus_root_writes_nothing_outside_it",
       a_capture_steered_out_of_the_corpus_root_writes_nothing_outside_it},
      {"a_capture_with_a_url_field_never_connects_to_it",
       a_capture_with_a_url_field_never_connects_to_it},
      {"the_capture_route_is_post_only_and_gated",
       the_capture_route_is_post_only_and_gated},
      {"a_mixed_batch_with_one_unindexable_extension_writes_nothing",
       a_mixed_batch_with_one_unindexable_extension_writes_nothing},
      {"a_url_share_with_no_file_writes_a_stub",
       a_url_share_with_no_file_writes_a_stub},
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
      {"a_refused_start_leaves_the_callers_descriptors_open",
       a_refused_start_leaves_the_callers_descriptors_open},
      {"no_path_outside_the_source_root_is_ever_served",
       no_path_outside_the_source_root_is_ever_served},
      {"the_artifact_subdomain_is_chosen_by_host",
       the_artifact_subdomain_is_chosen_by_host},
      {"artifact_bytes_on_the_parent_origin",
       artifact_bytes_on_the_parent_origin},
      {"a_markdown_artifact_is_served_as_a_rendered_page",
       a_markdown_artifact_is_served_as_a_rendered_page},
      {"a_markdown_artifact_is_rendered_on_the_subdomain_too",
       a_markdown_artifact_is_rendered_on_the_subdomain_too},
      {"a_non_utf8_markdown_artifact_is_refused_and_serves_no_bytes",
       a_non_utf8_markdown_artifact_is_refused_and_serves_no_bytes},
      {"a_failed_markdown_render_is_an_error_not_a_raw_serve",
       a_failed_markdown_render_is_an_error_not_a_raw_serve},
      {"an_empty_markdown_artifact_is_an_empty_page_not_an_error",
       an_empty_markdown_artifact_is_an_empty_page_not_an_error},
      {"raw_html_in_markdown_passes_through_under_the_sandbox",
       raw_html_in_markdown_passes_through_under_the_sandbox},
      {"a_markdown_download_is_the_source_not_the_page",
       a_markdown_download_is_the_source_not_the_page},
      {"a_markdown_artifact_is_reported_under_the_renderers_title",
       a_markdown_artifact_is_reported_under_the_renderers_title},
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
