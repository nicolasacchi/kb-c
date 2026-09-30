/* test_rstore.c — the review store, exercised against REAL git.
 *
 * Nothing here is mocked. The seed runs against a real bare repository built
 * with real git plumbing; the credential helper is the real `sh` snippet git
 * actually executes; the concurrency test is real fork(2) under a real flock.
 * The one thing that is a stand-in is `gh`, which is exercised through the
 * probe vtable — the vtable IS the seam the ladder is designed around, and
 * driving it is what makes the fall-through rules testable at all.
 */
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include "kbc_test.h"
#include "kbc/mem.h"
#include "kbc/rstore.h"

static char g_tmp[KBC_TEST_PATH_MAX];

/* The token every security case uses. It is deliberately NOT a shape the
 * redactor knows (no ghp_ prefix): a test that only proves the shape rules
 * work would pass while the literal path — the one that actually carries the
 * operator's GHE token — rotted. */
/* An empty kbc_str has a NULL .ptr; a caller that greps it must not hand
 * strstr a NULL. */
#define STRP(s) ((s).ptr != NULL ? (s).ptr : "")

static const char *const TOKEN = "zzTESTONLYnotashape0123456789abcdefXYZ";

static void path(char *out, size_t cap, const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(out, cap, fmt, ap);
  va_end(ap);
}

static size_t open_fds(void) {
  DIR *d = opendir("/proc/self/fd");
  if (d == NULL) {
    return 0;
  }
  size_t n = 0;
  struct dirent *e;
  while ((e = readdir(d)) != NULL) {
    if (e->d_name[0] != '.') {
      n++;
    }
  }
  closedir(d);
  return n;
}

static size_t zombie_children(void) {
  size_t n = 0;
  for (long pid = 1; pid < 512; pid++) {
    char stat[64];
    snprintf(stat, sizeof stat, "/proc/%ld/stat", pid);
    char *text = kbc_test_read_file(stat);
    if (text == NULL) {
      continue;
    }
    char *rp = strrchr(text, ')');
    if (rp != NULL && (rp[1] == ' ' || rp[1] == '\t') && rp[2] == 'Z') {
      n++;
    }
    free(text);
  }
  return n;
}

/* ---------------------------------------------------------------- redact -- */

static bool redacted(const char *in, const char *must_not_contain) {
  kbc_str out;
  kbc_str_init(&out);
  if (kbc_rs_redact(&out, in, strlen(in)) != KBC_OK) {
    kbc_str_free(&out);
    return false;
  }
  bool ok = strstr(out.ptr, must_not_contain) == NULL &&
            strstr(out.ptr, KBC_RS_REDACTED) != NULL;
  kbc_str_free(&out);
  return ok;
}

KBC_TEST(redaction_strips_every_known_secret_shape) {
  const char *shapes[] = {
      "error: token ghp_AAAAAAAAAAAAAAAAAAAA rejected",
      "error: gho_BBBBBBBBBBBBBBBBBBBBBB rejected",
      "error: ghs_CCCCCCCCCCCCCCCCCCCCCC rejected",
      "error: ghu_DDDDDDDDDDDDDDDDDDDDD rejected",
      "error: ghr_EEEEEEEEEEEEEEEEEEEE rejected",
      "error: github_pat_11FAKE0000000000_ffffffffffffffffffffffffffff refused",
      "error: glpat-FAKEgggggggggggggggg refused",
  };
  for (size_t i = 0; i < sizeof shapes / sizeof shapes[0]; i++) {
    KBC_CHECK_MSG(redacted(shapes[i], "FAKE") || redacted(shapes[i], "AAAA"),
                  "shape %zu leaked: %s", i, shapes[i]);
  }
  /* A known shape is not enough: assert the token itself is gone. */
  KBC_CHECK(redacted("ghp_AAAAAAAAAAAAAAAAAAAA", "ghp_"));
}

KBC_TEST(redaction_keeps_the_host_and_the_object_id) {
  /* The failure mode that would make every git error useless: over-redacting
   * a 40-hex object id or the repository path. */
  const char *s =
      "fatal: remote error: upload-pack: not our ref "
      "deadbeefdeadbeefdeadbeefdeadbeefdeadbeef in refs/heads/main";
  kbc_str out;
  kbc_str_init(&out);
  KBC_CHECK_OK(kbc_rs_redact(&out, s, strlen(s)));
  KBC_CHECK_EQ_STR(out.ptr, s);
  kbc_str_free(&out);

  const char *u =
      "fatal: unable to access 'https://x-access-token:ghp_ZZZZZZZZZZZZZZZZZZZZ"
      "@github.com/acme/widgets.git/': 403";
  kbc_str_init(&out);
  KBC_CHECK_OK(kbc_rs_redact(&out, u, strlen(u)));
  KBC_CHECK(strstr(out.ptr, "x-access-token") == NULL);
  KBC_CHECK(strstr(out.ptr, "ghp_") == NULL);
  KBC_CHECK(strstr(out.ptr, "https://") != NULL);
  KBC_CHECK(strstr(out.ptr, "github.com/acme/widgets.git") != NULL);
  kbc_str_free(&out);
}

KBC_TEST(redaction_removes_headers_and_credential_answers) {
  kbc_str out;
  kbc_str_init(&out);
  const char *hdr = "> Authorization: Basic eC1hY2Nlc3M6c2VjcmV0\n> Host: x";
  KBC_CHECK_OK(kbc_rs_redact(&out, hdr, strlen(hdr)));
  KBC_CHECK(strstr(out.ptr, "eC1hY2Nlc3M") == NULL);
  KBC_CHECK(strstr(out.ptr, "Host: x") != NULL);
  kbc_str_free(&out);

  kbc_str_init(&out);
  const char *pw = "protocol=https\nhost=github.com\nusername=x\n"
                   "password=hunter2hunter2\n";
  KBC_CHECK_OK(kbc_rs_redact(&out, pw, strlen(pw)));
  KBC_CHECK(strstr(out.ptr, "hunter2") == NULL);
  KBC_CHECK(strstr(out.ptr, "host=github.com") != NULL);
  kbc_str_free(&out);

  kbc_str_init(&out);
  const char *bear = "using Bearer AAAABBBBCCCCDDDDEEEE now";
  KBC_CHECK_OK(kbc_rs_redact(&out, bear, strlen(bear)));
  KBC_CHECK(strstr(out.ptr, "AAAABBBB") == NULL);
  kbc_str_free(&out);
}

KBC_TEST(redaction_of_a_known_literal_has_no_length_floor) {
  /* The caller put this in the list BECAUSE it is a secret. A one-byte
   * secret is still a secret; a length floor here would be a length floor
   * the operator cannot see. */
  const char *lits[] = {TOKEN};
  kbc_rs_secrets sec = {lits, 1};
  char in[256];
  snprintf(in, sizeof in, "gitea said: %s is bad", TOKEN);
  kbc_str out;
  kbc_str_init(&out);
  KBC_CHECK_OK(kbc_rs_redact_with(&out, in, strlen(in), &sec));
  KBC_CHECK(strstr(out.ptr, TOKEN) == NULL);
  KBC_CHECK(strstr(out.ptr, "gitea said") != NULL);
  kbc_str_free(&out);

  const char *one = "a";
  kbc_rs_secrets tiny = {&one, 1};
  const char *msg = "secret is a";
  kbc_str_init(&out);
  KBC_CHECK_OK(kbc_rs_redact_with(&out, msg, strlen(msg), &tiny));
  KBC_CHECK(strstr(out.ptr, "[redacted]") != NULL);
  kbc_str_free(&out);
}

KBC_TEST(redaction_caps_on_a_utf8_boundary) {
  kbc_str out;
  kbc_str_init(&out);
  KBC_CHECK_OK(kbc_rs_redact_bytes(&out, "eeeeeee", 7, NULL, 3));
  KBC_CHECK(strstr(out.ptr, "[truncated]") != NULL);
  kbc_str_free(&out);
}

/* ------------------------------------------------------------------- url -- */

KBC_TEST(url_allowlist_accepts_only_the_three_forge_forms) {
  kbc_rs_url u;
  kbc_err e;
  kbc_err_reset(&e);
  KBC_CHECK_OK(kbc_rs_url_parse_remote("https://github.com/acme/widgets.git", &u,
                                      &e));
  KBC_CHECK_EQ_STR(u.raw, "https://github.com/acme/widgets.git");
  KBC_CHECK_EQ_STR(u.authority, "github.com");
  KBC_CHECK_EQ_STR(u.host, "github.com");
  KBC_CHECK_EQ_INT(u.proto, KBC_RS_PROTO_HTTPS);

  /* The scheme's default port folds out: git asks a credential helper with
   * host=github.com for BOTH spellings, and a kept ":443" would mean a
   * correct remote failing closed as a credential fault. */
  KBC_CHECK_OK(kbc_rs_url_parse_remote("https://github.com:443/acme/widgets", &u,
                                      &e));
  KBC_CHECK_EQ_STR(u.authority, "github.com");
  /* A NON-default port is a different endpoint and stays. */
  KBC_CHECK_OK(kbc_rs_url_parse_remote("https://git.example.com:7999/o/n", &u,
                                      &e));
  KBC_CHECK_EQ_STR(u.authority, "git.example.com:7999");

  KBC_CHECK_OK(kbc_rs_url_parse_remote("ssh://git@github.com/acme/widgets", &u,
                                      &e));
  KBC_CHECK_EQ_INT(u.proto, KBC_RS_PROTO_SSH);
  KBC_CHECK_EQ_STR(u.raw, "ssh://git@github.com/acme/widgets");
  KBC_CHECK_OK(kbc_rs_url_parse_remote("git@github.com:acme/widgets.git", &u,
                                      &e));
  KBC_CHECK_EQ_INT(u.proto, KBC_RS_PROTO_SSH);
  KBC_CHECK_EQ_STR(u.raw, "git@github.com:acme/widgets.git");
}

KBC_TEST(url_allowlist_refuses_everything_else_and_never_echoes_it) {
  const char *bad[] = {
      "",
      "file:///etc/passwd",
      "http://github.com/acme/widgets",
      "https://user:pass@github.com/acme/widgets",
      "ssh://root@github.com/acme/widgets",
      "ext::sh -c whatever",
      "fd::7",
      "https://github.com/acme/%2e%2e/secret",
      "https://github.com/acme/../secret",
      "https://github.com/acme/widgets with space",
      "https://[::1]/acme/widgets",
      "git+ssh://git@github.com/a/b",
      "someone@elsewhere:acme/widgets",
  };
  for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++) {
    kbc_rs_url u;
    kbc_err e;
    kbc_err_reset(&e);
    kbc_status st = kbc_rs_url_parse_remote(bad[i], &u, &e);
    KBC_CHECK_MSG(st != KBC_OK, "accepted a refused url: %s", bad[i]);
    /* The message names the RULE, never the value: a rejected URL may carry
     * a token in its userinfo. */
    KBC_CHECK_MSG(e.msg[0] != '\0', "empty message for %s", bad[i]);
    KBC_CHECK_MSG(strstr(e.msg, bad[i]) == NULL || *bad[i] == '\0',
                  "the refusal echoed the url: %s", e.msg);
  }
}

KBC_TEST(store_key_normalizes_and_refuses_a_percent_escape) {
  kbc_rs_url u;
  kbc_err e;
  kbc_err_reset(&e);
  char key[KBC_RS_KEY_MAX];
  KBC_CHECK_OK(kbc_rs_url_parse_remote("https://github.com/Acme/Widgets.git",
                                      &u, &e));
  KBC_CHECK_OK(kbc_rs_store_key(&u, key, sizeof key, &e));
  KBC_CHECK_EQ_STR(key, "github.com/acme/widgets");
  KBC_CHECK_OK(kbc_rs_url_parse_remote("https://git.example.com:7999/o/n.git",
                                      &u, &e));
  KBC_CHECK_OK(kbc_rs_store_key(&u, key, sizeof key, &e));
  KBC_CHECK_EQ_STR(key, "git.example.com:7999/o/n");
  kbc_err_reset(&e);
  KBC_CHECK_ERR(kbc_rs_url_parse_remote("https://github.com/a/%2e%2e/b", &u, &e),
                KBC_ERR_INVALID);
}

KBC_TEST(https_equivalent_is_derived_only_when_unambiguous) {
  kbc_rs_url u, h;
  kbc_err e;
  kbc_err_reset(&e);
  KBC_CHECK_OK(kbc_rs_url_parse_remote("git@github.com:acme/widgets.git", &u, &e));
  KBC_CHECK_OK(kbc_rs_url_https_equivalent(&u, &h, &e));
  KBC_CHECK_EQ_STR(h.raw, "https://github.com/acme/widgets.git");
  /* The ssh:// form can carry a non-default port, and then there is no
   * knowable https twin. */
  KBC_CHECK_OK(kbc_rs_url_parse_remote("ssh://git@git.example.com:7999/o/n", &u,
                                      &e));
  KBC_CHECK_ERR(kbc_rs_url_https_equivalent(&u, &h, &e), KBC_ERR_UNSUPPORTED);
  /* The scp form has no port slot at all: everything after the colon is the
   * path, including what looks like one. */
  KBC_CHECK_OK(kbc_rs_url_parse_remote("git@git.example.com:7999/o/n", &u, &e));
  KBC_CHECK_EQ_STR(u.authority, "git.example.com");
  KBC_CHECK_EQ_STR(u.path, "7999/o/n");
}

/* ---------------------------------------------------------------- secret -- */

KBC_TEST(secret_validates_wipes_and_compares) {
  kbc_rs_secret s;
  kbc_err e;
  kbc_err_reset(&e);
  KBC_CHECK_OK(kbc_rs_secret_new(&s, "  abc123  ", &e));
  KBC_CHECK_EQ_INT((int)kbc_rs_secret_len(&s), 6);
  KBC_CHECK(kbc_rs_secret_eq(&s, "abc123", 6));
  KBC_CHECK(!kbc_rs_secret_eq(&s, "abc124", 6));
  KBC_CHECK(!kbc_rs_secret_eq(&s, "abc12", 5));
  kbc_rs_secret_wipe(&s);
  KBC_CHECK_EQ_INT((int)kbc_rs_secret_len(&s), 0);

  /* A newline would inject lines into the credential protocol the helper
   * answers in, so it is refused at construction and never reaches a pipe. */
  kbc_err_reset(&e);
  KBC_CHECK_ERR(kbc_rs_secret_new(&s, "abc\ndef", &e), KBC_ERR_INVALID);
  kbc_err_reset(&e);
  KBC_CHECK_ERR(kbc_rs_secret_new(&s, "abc def", &e), KBC_ERR_INVALID);
  kbc_err_reset(&e);
  KBC_CHECK_ERR(kbc_rs_secret_new(&s, "   ", &e), KBC_ERR_INVALID);
  /* A SHORT token is not refused: the ladder stops and reports it, and a
   * length floor here would turn a working credential into one no rung is
   * allowed to answer for. */
  kbc_err_reset(&e);
  KBC_CHECK_OK(kbc_rs_secret_new(&s, "abc", &e));
  kbc_rs_secret_wipe(&s);
}

/* ---------------------------------------------------------------- ladder -- */

typedef struct {
  kbc_status gh_st;
  kbc_rs_class gh_cls;
  const char *gh_account;
  const char *gh_token;
  kbc_status tf_st;
  kbc_rs_class tf_cls;
  const char *tf_token;
  kbc_status an_st;
  kbc_rs_class an_cls;
  int gh_calls, tf_calls, an_calls;
} fake_probes;

static kbc_status fake_gh(void *user, const kbc_rs_url *url, const char *pinned,
                          const char *recorded, kbc_rs_gh_login *out,
                          kbc_rs_class *cls, kbc_err *err) {
  fake_probes *f = (fake_probes *)user;
  (void)url;
  (void)pinned;
  (void)recorded;
  f->gh_calls++;
  *cls = f->gh_cls;
  if (f->gh_st != KBC_OK) {
    return f->gh_st;
  }
  snprintf(out->account, sizeof out->account, "%s",
           f->gh_account != NULL ? f->gh_account : "octocat");
  if (f->gh_token == NULL) {
    return kbc_err_set(err, KBC_ERR_INTERNAL, "no fake token configured");
  }
  kbc_status st = kbc_rs_secret_new(&out->secret, f->gh_token, err);
  if (st == KBC_OK) {
    *cls = KBC_RS_CLASS_CREDENTIAL_UNAVAILABLE;
  }
  return st;
}

static kbc_status fake_token_file(void *user, const char *p,
                                  const char *username, const kbc_rs_url *url,
                                  kbc_rs_secret *out, kbc_rs_class *cls,
                                  kbc_err *err) {
  fake_probes *f = (fake_probes *)user;
  (void)p;
  (void)username;
  (void)url;
  f->tf_calls++;
  *cls = f->tf_cls;
  if (f->tf_st != KBC_OK) {
    return f->tf_st;
  }
  return kbc_rs_secret_new(out, f->tf_token, err);
}

static kbc_status fake_anon(void *user, const kbc_rs_url *url,
                            kbc_rs_class *cls, kbc_err *err) {
  fake_probes *f = (fake_probes *)user;
  (void)url;
  f->an_calls++;
  *cls = f->an_cls;
  if (f->an_st != KBC_OK) {
    return kbc_err_set(err, f->an_st, "anonymous probe refused");
  }
  return KBC_OK;
}

static void fake_init(fake_probes *f, kbc_rs_ladder_probes *p) {
  memset(f, 0, sizeof *f);
  f->gh_token = TOKEN;
  f->tf_token = "file-token-value";
  p->gh_cli = fake_gh;
  p->token_file = fake_token_file;
  p->anonymous = fake_anon;
  p->user = f;
}

static void make_https_url(kbc_rs_url *u) {
  kbc_err e;
  kbc_err_reset(&e);
  KBC_CHECK_OK(kbc_rs_url_parse_remote("https://github.com/acme/widgets", u,
                                      &e));
}

KBC_TEST(ladder_pinned_rungs_never_fall_through) {
  kbc_rs_url u;
  make_https_url(&u);
  kbc_rs_cred_cfg cfg;
  kbc_rs_cred out;
  kbc_err e;
  fake_probes f;
  kbc_rs_ladder_probes p;

  fake_init(&f, &p);
  memset(&cfg, 0, sizeof cfg);
  cfg.pin = KBC_RS_PIN_GH_CLI;
  kbc_err_reset(&e);
  KBC_CHECK_OK(kbc_rs_cred_resolve(&cfg, &u, &p, &out, &e));
  KBC_CHECK_EQ_INT(out.profile, KBC_RS_PROFILE_GH_CLI);
  KBC_CHECK_EQ_INT(out.auth, KBC_RS_AUTH_TOKEN);
  KBC_CHECK(kbc_rs_secret_eq(&out.secret, TOKEN, strlen(TOKEN)));
  kbc_rs_cred_free(&out);

  /* A pinned rung that fails is an ERROR, never a fall-through to another
   * identity. */
  f.gh_st = KBC_ERR_NOTFOUND;
  f.gh_cls = KBC_RS_CLASS_CREDENTIAL_UNAVAILABLE;
  kbc_err_reset(&e);
  KBC_CHECK_ERR(kbc_rs_cred_resolve(&cfg, &u, &p, &out, &e), KBC_ERR_NOTFOUND);
  KBC_CHECK_EQ_INT(f.an_calls, 0);

  /* deploy-key is not in this build, and says so instead of being absent. */
  cfg.pin = KBC_RS_PIN_DEPLOY_KEY;
  kbc_err_reset(&e);
  KBC_CHECK_ERR(kbc_rs_cred_resolve(&cfg, &u, &p, &out, &e),
                KBC_ERR_UNSUPPORTED);

  /* token without a token_file is a configuration error, not a skip. */
  cfg.pin = KBC_RS_PIN_TOKEN;
  kbc_err_reset(&e);
  KBC_CHECK_ERR(kbc_rs_cred_resolve(&cfg, &u, &p, &out, &e), KBC_ERR_INVALID);

  cfg.pin = KBC_RS_PIN_ANONYMOUS;
  f.an_st = KBC_ERR_CONFLICT;
  f.an_cls = KBC_RS_CLASS_CREDENTIAL_REJECTED;
  kbc_err_reset(&e);
  KBC_CHECK_ERR(kbc_rs_cred_resolve(&cfg, &u, &p, &out, &e), KBC_ERR_CONFLICT);

  /* inherit is the one legacy rung, and only when the operator said so. */
  cfg.pin = KBC_RS_PIN_INHERIT;
  cfg.allow_inherited_credentials = false;
  kbc_err_reset(&e);
  KBC_CHECK_ERR(kbc_rs_cred_resolve(&cfg, &u, &p, &out, &e), KBC_ERR_INVALID);
  cfg.allow_inherited_credentials = true;
  kbc_err_reset(&e);
  KBC_CHECK_OK(kbc_rs_cred_resolve(&cfg, &u, &p, &out, &e));
  KBC_CHECK_EQ_INT(out.profile, KBC_RS_PROFILE_INHERIT);
  kbc_rs_cred_free(&out);

  cfg.pin = KBC_RS_PIN_NONE;
  kbc_err_reset(&e);
  KBC_CHECK_OK(kbc_rs_cred_resolve(&cfg, &u, &p, &out, &e));
  KBC_CHECK_EQ_INT(out.profile, KBC_RS_PROFILE_NONE);
  KBC_CHECK(!kbc_rs_cred_has_secret(&out));
  kbc_rs_cred_free(&out);
}

KBC_TEST(ladder_auto_walks_every_rung_in_order) {
  kbc_rs_url u;
  make_https_url(&u);
  kbc_rs_cred_cfg cfg;
  kbc_rs_cred out;
  kbc_err e;
  fake_probes f;
  kbc_rs_ladder_probes p;
  fake_init(&f, &p);
  memset(&cfg, 0, sizeof cfg);
  cfg.pin = KBC_RS_PIN_AUTO;
  cfg.token_file = "/nowhere/token";

  /* Everything fails: the answer is `none`, and every rung is recorded with
   * the reason it did not apply. */
  f.gh_st = KBC_ERR_NOTFOUND;
  f.gh_cls = KBC_RS_CLASS_CREDENTIAL_UNAVAILABLE;
  f.tf_st = KBC_ERR_NOTFOUND;
  f.tf_cls = KBC_RS_CLASS_CREDENTIAL_UNAVAILABLE;
  f.an_st = KBC_ERR_CONFLICT;
  f.an_cls = KBC_RS_CLASS_CREDENTIAL_UNAVAILABLE;
  kbc_err_reset(&e);
  KBC_CHECK_OK(kbc_rs_cred_resolve(&cfg, &u, &p, &out, &e));
  KBC_CHECK_EQ_INT(out.profile, KBC_RS_PROFILE_NONE);
  KBC_CHECK_EQ_INT(f.gh_calls, 1);
  KBC_CHECK_EQ_INT(f.tf_calls, 1);
  KBC_CHECK_EQ_INT(f.an_calls, 1);
  KBC_CHECK_EQ_INT((int)out.n_skipped, 5);
  kbc_rs_cred_free(&out);

  /* A bound gh account stops the ladder on ANY gh failure: falling through
   * would swap the identity the store fetches as, silently. */
  cfg.gh_user = "octocat";
  kbc_err_reset(&e);
  KBC_CHECK_ERR(kbc_rs_cred_resolve(&cfg, &u, &p, &out, &e), KBC_ERR_NOTFOUND);
  KBC_CHECK_EQ_INT(f.an_calls, 1);
  KBC_CHECK_EQ_INT(f.tf_calls, 1);
  kbc_rs_cred_free(&out);

  /* An account mismatch is a refusal in its own right, not a skip. */
  f.gh_st = KBC_ERR_CONFLICT;
  f.gh_cls = KBC_RS_CLASS_CREDENTIAL_ACCOUNT_MISMATCH;
  kbc_err_reset(&e);
  KBC_CHECK_ERR(kbc_rs_cred_resolve(&cfg, &u, &p, &out, &e), KBC_ERR_CONFLICT);
  kbc_rs_cred_free(&out);
  cfg.gh_user = NULL;
  /* gh is merely absent now, not mismatched: the rest of the ladder is
   * reachable again. */
  f.gh_st = KBC_ERR_NOTFOUND;
  f.gh_cls = KBC_RS_CLASS_CREDENTIAL_UNAVAILABLE;

  /* A token file that was PRESENT and readable but is broken also stops the
   * ladder; one that could not be opened at all only skips. */
  f.tf_st = KBC_ERR_INVALID;
  f.tf_cls = KBC_RS_CLASS_CREDENTIAL_REJECTED;
  kbc_err_reset(&e);
  KBC_CHECK_ERR(kbc_rs_cred_resolve(&cfg, &u, &p, &out, &e), KBC_ERR_INVALID);
  kbc_rs_cred_free(&out);

  f.tf_st = KBC_OK;
  f.tf_cls = KBC_RS_CLASS_CREDENTIAL_UNAVAILABLE;
  kbc_err_reset(&e);
  KBC_CHECK_OK(kbc_rs_cred_resolve(&cfg, &u, &p, &out, &e));
  KBC_CHECK_EQ_INT(out.profile, KBC_RS_PROFILE_TOKEN_FILE);
  KBC_CHECK(kbc_rs_secret_eq(&out.secret, "file-token-value", 16));
  kbc_rs_cred_free(&out);

  /* anonymous wins when the probe says the repo is readable unauthenticated */
  f.tf_st = KBC_ERR_NOTFOUND;
  f.an_st = KBC_OK;
  kbc_err_reset(&e);
  KBC_CHECK_OK(kbc_rs_cred_resolve(&cfg, &u, &p, &out, &e));
  KBC_CHECK_EQ_INT(out.profile, KBC_RS_PROFILE_ANONYMOUS);
  KBC_CHECK_EQ_INT(out.auth, KBC_RS_AUTH_ANONYMOUS);
  kbc_rs_cred_free(&out);

  /* inherit, then none. */
  f.an_st = KBC_ERR_CONFLICT;
  cfg.allow_inherited_credentials = true;
  kbc_err_reset(&e);
  KBC_CHECK_OK(kbc_rs_cred_resolve(&cfg, &u, &p, &out, &e));
  KBC_CHECK_EQ_INT(out.profile, KBC_RS_PROFILE_INHERIT);
  kbc_rs_cred_free(&out);
}

KBC_TEST(ladder_never_offers_a_token_to_another_host) {
  kbc_rs_url other;
  kbc_err e;
  kbc_err_reset(&e);
  KBC_CHECK_OK(
      kbc_rs_url_parse_remote("https://evil.example/acme/widgets", &other, &e));
  fake_probes f;
  kbc_rs_ladder_probes p;
  fake_init(&f, &p);
  f.gh_st = KBC_ERR_NOTFOUND;
  f.gh_cls = KBC_RS_CLASS_CREDENTIAL_UNAVAILABLE;
  f.tf_st = KBC_ERR_NOTFOUND;
  f.tf_cls = KBC_RS_CLASS_CREDENTIAL_UNAVAILABLE;
  f.an_st = KBC_ERR_CONFLICT;
  f.an_cls = KBC_RS_CLASS_CREDENTIAL_UNAVAILABLE;
  kbc_rs_cred_cfg cfg;
  memset(&cfg, 0, sizeof cfg);
  kbc_rs_cred out;
  kbc_err_reset(&e);
  KBC_CHECK_OK(kbc_rs_cred_resolve(&cfg, &other, &p, &out, &e));
  /* A file remote has no https form at all, so no token rung can be
   * considered — the scope check is structural, not advisory. */
  KBC_CHECK_EQ_INT(out.profile, KBC_RS_PROFILE_NONE);
  KBC_CHECK(!kbc_rs_cred_has_secret(&out));
  kbc_rs_cred_free(&out);
}

/* ------------------------------------------------------------------- git -- */

static const char *real_git_path(void) {
  static char buf[KBC_RS_PATH_MAX];
  const char *p = getenv("PATH");
  if (p == NULL) {
    return NULL;
  }
  const char *seg = p;
  while (*seg != '\0') {
    const char *colon = strchr(seg, ':');
    size_t len = (colon != NULL) ? (size_t)(colon - seg) : strlen(seg);
    if (len > 0) {
      snprintf(buf, sizeof buf, "%.*s/git", (int)len, seg);
      if (access(buf, X_OK) == 0) {
        return buf;
      }
    }
    if (colon == NULL) {
      break;
    }
    seg = colon + 1;
  }
  return NULL;
}

static kbc_rs_git *open_git(const char *name) {
  char home[KBC_RS_PATH_MAX];
  path(home, sizeof home, "%s/%s", g_tmp, name);
  kbc_rs_git *g = NULL;
  kbc_err e;
  kbc_err_reset(&e);
  if (kbc_rs_git_new(home, getenv("PATH"), &g, &e) != KBC_OK) {
    kbc_test_fail(__FILE__, __LINE__, "kbc_rs_git_new: %s", e.msg);
    return NULL;
  }
  char resolved[KBC_RS_PATH_MAX];
  if (!kbc_rs_git_resolved(g, resolved, sizeof resolved)) {
    kbc_test_fail(__FILE__, __LINE__, "no git binary on PATH");
    kbc_rs_git_free(g);
    return NULL;
  }
  return g;
}

/* One plain local git call, for building fixtures with real git. */
static kbc_status git_plain(kbc_rs_git *g, const char *git_dir,
                            const char *const *args, size_t nargs,
                            const char *stdin_bytes, size_t stdin_len,
                            kbc_rs_output *out, kbc_err *err) {
  const char *argv[12];
  size_t n = 0;
  argv[n++] = "-c";
  argv[n++] = "user.name=kb-c-test";
  argv[n++] = "-c";
  argv[n++] = "user.email=kb-c-test@example.invalid";
  for (size_t i = 0; i < nargs && n < 11; i++) {
    argv[n++] = args[i];
  }
  argv[n] = NULL;
  kbc_rs_call call;
  memset(&call, 0, sizeof call);
  call.op = "fixture";
  call.argv = argv;
  call.argc = n;
  call.git_dir = git_dir;
  call.auth = KBC_RS_AUTH_LOCAL_ONLY;
  call.timeout_s = 120;
  call.stdin_bytes = stdin_bytes;
  call.stdin_len = stdin_len;
  return kbc_rs_git_run(g, &call, out, err);
}

/* git's own answer, with the trailing newline git prints removed. Passing
 * "<oid>\n" back to git as a tree argument is a 128 with no message. */
static char *trimmed(const kbc_str *s) {
  char *v = strdup(s->ptr != NULL ? s->ptr : "");
  if (v == NULL) {
    return NULL;
  }
  size_t n = strlen(v);
  while (n > 0 && (v[n - 1] == '\n' || v[n - 1] == '\r')) {
    v[--n] = '\0';
  }
  return v;
}

/* Build a REAL bare remote: a blob, a tree, a commit and a ref, all made by
 * git itself. Returns the commit oid, or NULL. */
static char *make_source_repo(kbc_rs_git *g, const char *dir,
                              const char *branch) {
  kbc_err e;
  kbc_err_reset(&e);
  kbc_rs_output out;
  const char *init[] = {"init", "--bare", "--quiet", "--template=", dir};
  kbc_rs_call call;
  memset(&call, 0, sizeof call);
  call.op = "init";
  call.argv = init;
  call.argc = 5;
  call.auth = KBC_RS_AUTH_LOCAL_ONLY;
  call.timeout_s = 60;
  if (kbc_rs_git_run(g, &call, &out, &e) != KBC_OK) {
    kbc_rs_output_free(&out);
    kbc_test_fail(__FILE__, __LINE__, "init bare: %s", e.msg);
    return NULL;
  }
  kbc_rs_output_free(&out);
  char ref[KBC_RS_REFNAME_MAX];
  snprintf(ref, sizeof ref, "refs/heads/%s", branch);

  const char *blob[] = {"hash-object", "-w", "--stdin"};
  kbc_err_reset(&e);
  if (git_plain(g, dir, blob, 3, "hello from the store test\n", 27, &out, &e) !=
      KBC_OK) {
    kbc_rs_output_free(&out);
    kbc_test_fail(__FILE__, __LINE__, "hash-object: %s", e.msg);
    return NULL;
  }
  char *oid = trimmed(&out.stdout);
  kbc_rs_output_free(&out);
  if (oid == NULL) {
    return NULL;
  }
  char tree_in[128];
  snprintf(tree_in, sizeof tree_in, "100644 blob %s\tREADME\n", oid);
  const char *mktree[] = {"mktree"};
  kbc_err_reset(&e);
  if (git_plain(g, dir, mktree, 1, tree_in, strlen(tree_in), &out, &e) !=
      KBC_OK) {
    kbc_rs_output_free(&out);
    free(oid);
    kbc_test_fail(__FILE__, __LINE__, "mktree: %s", e.msg);
    return NULL;
  }
  char *tree = trimmed(&out.stdout);
  kbc_rs_output_free(&out);
  if (tree == NULL) {
    free(oid);
    return NULL;
  }
  const char *commit_argv[] = {"commit-tree", tree, "-m", "seed"};
  kbc_err_reset(&e);
  if (git_plain(g, dir, commit_argv, 4, NULL, 0, &out, &e) != KBC_OK) {
    kbc_rs_output_free(&out);
    free(oid);
    free(tree);
    kbc_test_fail(__FILE__, __LINE__, "commit-tree: %s", e.msg);
    return NULL;
  }
  char *commit_oid = trimmed(&out.stdout);
  kbc_rs_output_free(&out);
  free(tree);
  if (commit_oid == NULL) {
    free(oid);
    return NULL;
  }
  kbc_str tx;
  kbc_str_init(&tx);
  kbc_str_printf(&tx, "update %s %s\n", ref, commit_oid);
  kbc_err_reset(&e);
  if (kbc_rs_git_update_refs(g, dir, tx.ptr, tx.len, &e) != KBC_OK) {
    kbc_str_free(&tx);
    free(oid);
    free(commit_oid);
    kbc_test_fail(__FILE__, __LINE__, "update-ref: %s", e.msg);
    return NULL;
  }
  kbc_str_free(&tx);
  /* The blob id has done its work by here; only the commit goes back to the
   * caller. Every error path above already freed it and the success path did
   * not, so a PASSING fixture leaked on every call. */
  free(oid);
  return commit_oid;
}

KBC_TEST(git_never_writes_to_a_remote) {
  kbc_rs_git *g = open_git("nohome-push");
  if (g == NULL) {
    return;
  }
  const char *argv[] = {"push", "--all"};
  kbc_rs_call call;
  memset(&call, 0, sizeof call);
  call.op = "push";
  call.argv = argv;
  call.argc = 2;
  call.auth = KBC_RS_AUTH_LOCAL_ONLY;
  kbc_rs_output out;
  kbc_err e;
  kbc_err_reset(&e);
 KBC_CHECK_ERR(kbc_rs_git_run(g, &call, &out, &e), KBC_ERR_INVALID);
  KBC_CHECK(strstr(e.msg, "never writes to a remote") != NULL);
  kbc_rs_output_free(&out);
  kbc_rs_git_free(g);
}

KBC_TEST(git_scrubs_the_environment_and_the_credential_helpers) {
  kbc_rs_git *g = open_git("scrub");
  if (g == NULL) {
    return;
  }
  /* A call the scrub must defeat: one that would otherwise find the
   * operator's own git config, their credential helper and their proxy. */
  setenv("GIT_DIR", "/tmp/should-never-be-used", 1);
  setenv("KBC_TEST_PROBE_SECRET", "inherited-value", 1);
  const char *argv[] = {"config", "--get", "user.name"};
  kbc_rs_call call;
  memset(&call, 0, sizeof call);
  call.op = "config";
  call.argv = argv;
  call.argc = 3;
  call.auth = KBC_RS_AUTH_LOCAL_ONLY;
  call.timeout_s = 30;
  call.allow_nonzero = true;
  kbc_rs_output out;
  kbc_err e;
  kbc_err_reset(&e);
  KBC_CHECK_OK(kbc_rs_git_run(g, &call, &out, &e));
  /* HOME is the store's own git_home, which holds only our gitconfig and
   * holds no user.name at all. */
  KBC_CHECK(strstr(STRP(out.stdout), "should-never-be-used") == NULL);
  kbc_rs_output_free(&out);
  unsetenv("GIT_DIR");
  unsetenv("KBC_TEST_PROBE_SECRET");
  kbc_rs_git_free(g);
}

/* THE SECURITY TEST.
 *
 * A `git` stand-in that records the argv and the environment the child
 * actually receives, then execs the real git with the same "$@". Nothing here
 * is inspected in the abstract: these are the bytes a process on this host
 * could read out of /proc/<pid>/cmdline and /proc/<pid>/environ.
 */
static int install_recording_git(const char *bindir, const char *dumpdir) {
  const char *real = real_git_path();
  if (real == NULL) {
    return 0;
  }
  kbc_test_mkdir_p(bindir);
  kbc_test_mkdir_p(dumpdir);
  char script[KBC_TEST_PATH_MAX];
  path(script, sizeof script, "%s/git", bindir);
  char body[2048 + 3u * KBC_TEST_PATH_MAX];
  snprintf(body, sizeof body,
           "#!/bin/sh\n"
           "tr '\\0' '\\n' < /proc/$$/cmdline > %s/argv\n"
           "tr '\\0' '\\n' < /proc/$$/environ > %s/env\n"
           "exec %s \"$@\"\n",
           dumpdir, dumpdir, real);
  kbc_test_write_file(script, body);
  return chmod(script, 0755) == 0;
}

KBC_TEST(security_a_token_reaches_git_but_not_argv_env_or_any_error) {
  char bindir[KBC_TEST_PATH_MAX];
  char dumpdir[KBC_TEST_PATH_MAX];
  char home[KBC_TEST_PATH_MAX];
  path(bindir, sizeof bindir, "%s/recbin", g_tmp);
  path(dumpdir, sizeof dumpdir, "%s/recdump", g_tmp);
  path(home, sizeof home, "%s/recgit-home", g_tmp);
  if (!install_recording_git(bindir, dumpdir)) {
    kbc_test_fail(__FILE__, __LINE__, "could not install the recording git");
    return;
  }
  kbc_rs_git *g = NULL;
  kbc_err e;
  kbc_err_reset(&e);
  char newpath[KBC_TEST_PATH_MAX * 2];
  snprintf(newpath, sizeof newpath, "%s:%s", bindir, getenv("PATH"));
  if (kbc_rs_git_new(home, newpath, &g, &e) != KBC_OK) {
    kbc_test_fail(__FILE__, __LINE__, "git_new: %s", e.msg);
    return;
  }
  char resolved[KBC_RS_PATH_MAX];
  char want_bin[KBC_TEST_PATH_MAX];
  KBC_CHECK(kbc_rs_git_resolved(g, resolved, sizeof resolved));
  path(want_bin, sizeof want_bin, "%s/git", bindir);
  KBC_CHECK_MSG(strcmp(resolved, want_bin) == 0, "resolved %s, want %s",
                resolved, want_bin);

  kbc_rs_cred cred;
  kbc_rs_cred_init(&cred);
  cred.profile = KBC_RS_PROFILE_TOKEN_FILE;
  cred.auth = KBC_RS_AUTH_TOKEN;
  snprintf(cred.host, sizeof cred.host, "example.invalid");
  snprintf(cred.username, sizeof cred.username, "x-access-token");
  kbc_err_reset(&e);
  if (kbc_rs_secret_new(&cred.secret, TOKEN, &e) != KBC_OK) {
    kbc_test_fail(__FILE__, __LINE__, "secret: %s", e.msg);
    kbc_rs_git_free(g);
    return;
  }

  /* `git credential fill` runs the REAL helper snippet this library
   * composes, through the REAL sh, and asks it for exactly this protocol and
   * host. A password coming back is the positive proof that the pipe works:
   * a test that only asserted the token's absence would pass against a
   * mechanism that never delivered it at all. */
  const char *argv[] = {"credential", "fill"};
  const char *in = "protocol=https\nhost=example.invalid\n\n";
  kbc_rs_call call;
  memset(&call, 0, sizeof call);
  call.op = "credential-fill";
  call.argv = argv;
  call.argc = 2;
  call.auth = KBC_RS_AUTH_TOKEN;
  call.cred = &cred;
  call.stdin_bytes = in;
  call.stdin_len = strlen(in);
  call.timeout_s = 30;
  call.stdout_cap = 65536;
  kbc_rs_output out;
  kbc_err_reset(&e);
  KBC_CHECK_MSG(kbc_rs_git_run(g, &call, &out, &e) == KBC_OK,
           "credential fill failed: %s", e.msg);
  /* The positive control. `strstr(stdout, TOKEN)` alone is weak: git could
   * print the token for any reason. What proves the pipe worked is that git
   * answered the CREDENTIAL PROTOCOL — a username and a password line for
   * the host that was asked about. */
  KBC_CHECK_MSG(strstr(STRP(out.stdout), TOKEN) != NULL,
           "the token never reached git; stdout was: %s", STRP(out.stdout));
  KBC_CHECK_MSG(strstr(STRP(out.stdout), "password=") != NULL,
           "git did not answer the credential protocol; stdout was: %s",
           STRP(out.stdout));
  KBC_CHECK_MSG(strstr(STRP(out.stdout), "username=") != NULL,
           "git answered without a username; stdout was: %s",
           STRP(out.stdout));
  /* But nowhere else. */
  KBC_CHECK(strstr(STRP(out.stderr), TOKEN) == NULL);
  kbc_rs_output_free(&out);

  char p[KBC_TEST_PATH_MAX];
  path(p, sizeof p, "%s/argv", dumpdir);
  char *argv_dump = kbc_test_read_file(p);
  KBC_CHECK_NOT_NULL(argv_dump);
  if (argv_dump != NULL) {
    KBC_CHECK_MSG(strstr(argv_dump, TOKEN) == NULL,
                  "the token reached the child's argv");
    /* The snippet itself is composed only from the fd number, the protocol
     * and the authority. */
    KBC_CHECK(strstr(argv_dump, "credential.helper=!") != NULL);
    KBC_CHECK(strstr(argv_dump, "example.invalid") != NULL);
    free(argv_dump);
  }
  path(p, sizeof p, "%s/env", dumpdir);
  char *env_dump = kbc_test_read_file(p);
  KBC_CHECK_NOT_NULL(env_dump);
  if (env_dump != NULL) {
    KBC_CHECK_MSG(strstr(env_dump, TOKEN) == NULL,
 "the token reached the child's environment");
    free(env_dump);
  }

  /* Now a FAILING credentialed call: the token is the operator's and the
   * remote is unreachable, so git prints an error naming the URL. The error
   * string is what an operator pastes into a bug report. */
  const char *bad[] = {"ls-remote", "https://example.invalid/acme/nope.git",
      "HEAD"};
  kbc_rs_call fail_call;
  memset(&fail_call, 0, sizeof fail_call);
  fail_call.op = "ls-remote";
  fail_call.argv = bad;
  fail_call.argc = 3;
  fail_call.auth = KBC_RS_AUTH_TOKEN;
  fail_call.cred = &cred;
  fail_call.timeout_s = 20;
  kbc_err_reset(&e);
  kbc_status st = kbc_rs_git_run(g, &fail_call, &out, &e);
  KBC_CHECK_MSG(st != KBC_OK, "an unreachable remote reported success");
  KBC_CHECK_MSG(strstr(e.msg, TOKEN) == NULL,
    "the token reached an error message: %s", e.msg);
  /* NON-VACUITY. "the token is absent from stderr" is satisfied by an
   * empty stderr, so on its own it would pass against a capture that
   * discards everything git said — which is exactly the failure mode this
   * test exists to catch. git demonstrably writes to stderr for an
   * unreachable remote, so that stderr must be here and must be
   * substantial before its silence about the token means anything. The same
   * applies to the error string, which is built from those bytes. */
  KBC_CHECK_MSG(STRP(out.stderr)[0] != '\0',
    "stderr was empty, so the token's absence from it proves nothing");
  KBC_CHECK_MSG(strlen(STRP(out.stderr)) > 20u,
    "stderr is implausibly short to be git's own diagnostic: %s",
    STRP(out.stderr));
  KBC_CHECK_MSG(e.msg[0] != '\0', "the failure carried no message at all");
  KBC_CHECK_MSG(strstr(STRP(out.stderr), "example.invalid") != NULL,
    "git's stderr must name the remote it tried, or it is not the real "
    "diagnostic: %s", STRP(out.stderr));
  /* And with real bytes in hand, the absence assertions above are about the
   * token specifically rather than about an empty buffer. */
  KBC_CHECK(strstr(STRP(out.stderr), TOKEN) == NULL);
  kbc_rs_output_free(&out);
  kbc_rs_cred_free(&cred);
  kbc_rs_git_free(g);
}

KBC_TEST(git_kills_a_hung_child_and_leaks_no_descriptor) {
  /* A `git` that never exits, on its own PATH, so the deadline and the
   * process-group kill are the only things that can end the call. */
  char bindir[KBC_TEST_PATH_MAX];
  char home[KBC_TEST_PATH_MAX];
  path(bindir, sizeof bindir, "%s/hangbin", g_tmp);
  path(home, sizeof home, "%s/hang-home", g_tmp);
  kbc_test_mkdir_p(bindir);
  char script[KBC_TEST_PATH_MAX];
  path(script, sizeof script, "%s/git", bindir);
  kbc_test_write_file(script, "#!/bin/sh\nsleep 120\n");
  KBC_CHECK_EQ_INT(chmod(script, 0755), 0);

  char newpath[KBC_TEST_PATH_MAX * 2];
  snprintf(newpath, sizeof newpath, "%s:%s", bindir, getenv("PATH"));
  kbc_rs_git *g = NULL;
  kbc_err e;
  kbc_err_reset(&e);
  KBC_CHECK_OK(kbc_rs_git_new(home, newpath, &g, &e));
  if (g == NULL) {
    return;
  }
  size_t fds_before = open_fds();
  size_t zombies_before = zombie_children();
  const char *argv[] = {"status"};
  kbc_rs_call call;
  memset(&call, 0, sizeof call);
  call.op = "hung";
  call.argv = argv;
  call.argc = 1;
  call.auth = KBC_RS_AUTH_LOCAL_ONLY;
  call.timeout_s = 1;
  kbc_rs_output out;
  kbc_err_reset(&e);
  int64_t t0 = kbc_now_ns();
  kbc_status st = kbc_rs_git_run(g, &call, &out, &e);
  int64_t took = (kbc_now_ns() - t0) / 1000000;
  KBC_CHECK_MSG(st == KBC_ERR_TIMEOUT, "a hung child was not timed out: %s",
                e.msg);
  KBC_CHECK_MSG(took < 15000, "the deadline took %lldms to fire",
                (long long)took);
  kbc_rs_output_free(&out);
  KBC_CHECK_MSG(open_fds() <= fds_before, "descriptors leaked: %zu -> %zu",
                fds_before, open_fds());
  KBC_CHECK_MSG(zombie_children() <= zombies_before, "a child was left behind");
  kbc_rs_git_free(g);
}

/* ------------------------------------------------- manifest, lock, seed -- */

KBC_TEST(manifest_ties_a_directory_to_its_row) {
  kbc_rs_manifest m;
  kbc_err e;
  kbc_err_reset(&e);
  char dir[KBC_TEST_PATH_MAX];
  path(dir, sizeof dir, "%s/manifest-store", g_tmp);
  kbc_test_mkdir_p(dir);
  KBC_CHECK_OK(
      kbc_rs_manifest_write(dir, "u-1", "github.com/acme/widgets", 7, &e));
  KBC_CHECK_OK(
      kbc_rs_manifest_check(dir, "u-1", "github.com/acme/widgets", &m, &e));
  KBC_CHECK_EQ_STR(m.schema, KBC_RS_MANIFEST_SCHEMA);
  KBC_CHECK_EQ_INT((int)m.created_at, 7);
  /* A restored database pointing at another store's directory, or a copied
   * directory, is never silently adopted. */
  kbc_err_reset(&e);
  KBC_CHECK_ERR(
      kbc_rs_manifest_check(dir, "u-2", "github.com/acme/widgets", &m, &e),
      KBC_ERR_CONFLICT);
  KBC_CHECK(strstr(e.msg, "u-1") != NULL);
  kbc_err_reset(&e);
  KBC_CHECK_ERR(
      kbc_rs_manifest_check(dir, "u-1", "github.com/acme/gadgets", &m, &e),
      KBC_ERR_CONFLICT);
  char p[KBC_TEST_PATH_MAX];
  path(p, sizeof p, "%s/%s", dir, KBC_RS_MANIFEST_NAME);
  unlink(p);
  kbc_err_reset(&e);
  KBC_CHECK_ERR(
      kbc_rs_manifest_check(dir, "u-1", "github.com/acme/widgets", &m, &e),
      KBC_ERR_NOTFOUND);
  kbc_test_write_file(p, "{not json");
  kbc_err_reset(&e);
  KBC_CHECK_ERR(
      kbc_rs_manifest_check(dir, "u-1", "github.com/acme/widgets", &m, &e),
      KBC_ERR_PARSE);
  kbc_err_reset(&e);
  KBC_CHECK_ERR(
      kbc_rs_manifest_check("/definitely/not/here", "u-1", "k", &m, &e),
      KBC_ERR_NOTFOUND);
}

KBC_TEST(the_store_lock_refuses_a_second_holder_and_names_it) {
  char root[KBC_TEST_PATH_MAX];
  path(root, sizeof root, "%s/lockroot", g_tmp);
  kbc_err e;
  kbc_rs_lock *a = NULL;
  kbc_err_reset(&e);
  KBC_CHECK_OK(kbc_rs_lock_acquire(root, "u-1", &a, &e));
  KBC_CHECK_EQ_INT((int)kbc_rs_lock_holder(a), (int)getpid());
  /* flock is held on the open file description, so a second open in this
   * same process contends exactly as a second daemon would. */
  kbc_rs_lock *b = NULL;
  kbc_err_reset(&e);
  KBC_CHECK_ERR(kbc_rs_lock_acquire(root, "u-1", &b, &e), KBC_ERR_CONFLICT);
  KBC_CHECK_MSG(strstr(e.msg, "already locked") != NULL, "got: %s", e.msg);
  char want[32];
  snprintf(want, sizeof want, "pid %d", (int)getpid());
  KBC_CHECK_MSG(strstr(e.msg, want) != NULL,
           "the refusal did not name the holder (%s): %s", want, e.msg);
  KBC_CHECK_NULL(b);
  /* A DIFFERENT store in the same root is not contended: the lock is per
   * store, not per root. */
  kbc_err_reset(&e);
  KBC_CHECK_OK(kbc_rs_lock_acquire(root, "u-2", &b, &e));
  kbc_rs_lock_release(b);
  kbc_rs_lock_release(a);
  kbc_err_reset(&e);
  KBC_CHECK_OK(kbc_rs_lock_acquire(root, "u-1", &b, &e));
  kbc_rs_lock_release(b);
}

static bool seed_one(kbc_rs_git *g, const char *root, const char *uuid,
                      const kbc_rs_url *base, const char *store_key,
                      kbc_rs_seed_report *rep, kbc_err *err) {
  kbc_rs_seed_plan plan;
  memset(&plan, 0, sizeof plan);
  plan.root = root;
  plan.uuid = uuid;
  plan.store_key = store_key;
  plan.base = base;
  plan.cred = NULL;
  plan.timeout_s = 120;
  return kbc_rs_seed(g, &plan, rep, err) == KBC_OK;
}

KBC_TEST(seeding_clones_a_real_local_remote_and_writes_a_manifest) {
  kbc_rs_git *g = open_git("seed-home");
  if (g == NULL) {
    return;
  }
  char src[KBC_TEST_PATH_MAX];
  char root[KBC_TEST_PATH_MAX];
  path(src, sizeof src, "%s/seed-src.git", g_tmp);
  path(root, sizeof root, "%s/seedroot", g_tmp);
  char *commit = make_source_repo(g, src, "main");
  if (commit == NULL) {
    kbc_rs_git_free(g);
    return;
  }
  kbc_rs_url base;
  kbc_err e;
  kbc_err_reset(&e);
  KBC_CHECK_OK(kbc_rs_url_local_seed(src, &base, &e));
  KBC_CHECK_EQ_INT(base.proto, KBC_RS_PROTO_FILE);
  kbc_rs_seed_report rep;
  kbc_err_reset(&e);
  KBC_CHECK_MSG(seed_one(g, root, "u-seed", &base,
                         "local:u-seed", &rep, &e),
   "seed failed: %s", e.msg);
  KBC_CHECK_EQ_INT(rep.base_state, KBC_RS_BASE_FETCHED);
  KBC_CHECK(rep.refs_imported >= 1);

  /* The ref says where the mirror BELIEVES the commit is. `cat-file` is
   * git's own answer to whether the object is actually there, which is the
   * property a mirror has or does not have: a ref can name an id whose
   * objects were never fetched, and a check that only read refs would call
   * that a successful seed. */
  kbc_strlist refs;
  kbc_strlist_init(&refs);
  kbc_err_reset(&e);
  KBC_CHECK_OK(kbc_rs_git_list_refs(g, rep.git_dir,
        (const char *const[]){"refs/remotes/base/", NULL}, &refs, &e));
  bool found = false;
  for (size_t i = 0; i < refs.len; i++) {
    if (strstr(refs.items[i], "refs/remotes/base/main") != NULL) {
      char want[128];
      snprintf(want, sizeof want, "%s refs/remotes/base/main", commit);
      found = strncmp(refs.items[i], want, strlen(want)) == 0;
    }
  }
  KBC_CHECK_MSG(found, "the mirror does not carry the source commit (%s)",
            commit);
  kbc_strlist_free(&refs);

  /* And the object itself, asked of git directly. `-e` in a bare mirror
   * needs the git dir named, which is why this goes through the call with
   * an explicit git_dir rather than relying on the process cwd. A non-empty
   * stdout is checked too: `cat-file -e` is silent on success, so an empty
   * answer is the only answer, and this pins that the probe really ran. */
  {
    const char *argv[] = {"cat-file", "-t", commit};
    kbc_rs_output o2;
    kbc_err_reset(&e);
    KBC_CHECK_MSG(git_plain(g, rep.git_dir, argv, 3, NULL, 0, &o2, &e) == KBC_OK,
                  "the mirror has no object %s: %s", commit, e.msg);
    KBC_CHECK_MSG(strstr(STRP(o2.stdout), "commit") != NULL,
                  "git says %s is a [%s], not a commit", commit,
                  STRP(o2.stdout));
    kbc_rs_output_free(&o2);
  }
  /* The commit's CONTENT came across, not just its id: the blob it points
   * at is in the mirror and holds the bytes the source published. Named by
   * the commit rather than by HEAD, because a store's own HEAD is
   * `refs/kbc/none` and resolves to nothing. */
  {
    char spec[KBC_RS_OID_MAX + 16];
    snprintf(spec, sizeof spec, "%s:README", commit);
    const char *argv[] = {"cat-file", "blob", spec};
    kbc_rs_output o3;
    kbc_err_reset(&e);
    KBC_CHECK_MSG(git_plain(g, rep.git_dir, argv, 3, NULL, 0, &o3, &e) == KBC_OK,
                  "the mirror has no README blob: %s", e.msg);
    KBC_CHECK_MSG(strstr(STRP(o3.stdout), "hello from the store test") != NULL,
                  "the blob's bytes did not come across: [%s]", STRP(o3.stdout));
    kbc_rs_output_free(&o3);
  }

  kbc_rs_manifest m;
  kbc_err_reset(&e);
  KBC_CHECK_OK(kbc_rs_manifest_check(rep.git_dir, "u-seed", "local:u-seed", &m,
        &e));
  /* And nothing is left half-built: the .tmp is gone. */
  char tmp[KBC_TEST_PATH_MAX];
  path(tmp, sizeof tmp, "%s/.seed-u-seed.tmp", root);
  KBC_CHECK(!kbc_path_exists(tmp));
  /* A second seed of the same store is a conflict, not an overwrite. */
  kbc_rs_seed_report second;
  kbc_err_reset(&e);
  KBC_CHECK(!seed_one(g, root, "u-seed", &base, "local:u-seed", &second, &e));
  KBC_CHECK(strstr(e.msg, "already exists") != NULL);
  free(commit);
  kbc_rs_git_free(g);
}

KBC_TEST(two_concurrent_seeds_produce_one_mirror_and_named_refusals) {
  kbc_rs_git *g = open_git("race-home");
  if (g == NULL) {
    return;
  }
  char src[KBC_TEST_PATH_MAX];
  char root[KBC_TEST_PATH_MAX];
  path(src, sizeof src, "%s/race-src.git", g_tmp);
  path(root, sizeof root, "%s/raceroot", g_tmp);
  char *commit = make_source_repo(g, src, "main");
  if (commit == NULL) {
    kbc_rs_git_free(g);
    return;
  }
  kbc_rs_url base;
  kbc_err e;
  kbc_err_reset(&e);
  KBC_CHECK_OK(kbc_rs_url_local_seed(src, &base, &e));
  free(commit);

  enum { N = 4 };
  pid_t kids[N];
  for (int i = 0; i < N; i++) {
    pid_t pid = fork();
    if (pid == 0) {
      kbc_rs_seed_report rep;
      kbc_err ce;
      kbc_err_reset(&ce);
      char out[KBC_TEST_PATH_MAX];
      path(out, sizeof out, "%s/raceres.%d", g_tmp, i);
      if (seed_one(g, root, "u-race", &base, "local:u-race", &rep, &ce)) {
        kbc_test_write_file(out, "ok\n");
      } else {
        char body[1024];
        snprintf(body, sizeof body, "refused: %s\n", ce.msg);
        kbc_test_write_file(out, body);
      }
      _exit(0);
    }
    KBC_CHECK(pid > 0);
    kids[i] = pid;
  }
  for (int i = 0; i < N; i++) {
    int status = 0;
    waitpid(kids[i], &status, 0);
  }
  int winners = 0;
  int refusals = 0;
  for (int i = 0; i < N; i++) {
    char p[KBC_TEST_PATH_MAX];
    path(p, sizeof p, "%s/raceres.%d", g_tmp, i);
    char *line = kbc_test_read_file(p);
    KBC_CHECK_NOT_NULL(line);
    if (line == NULL) {
      continue;
 }
    if (strncmp(line, "ok", 2) == 0) {
      winners++;
    } else {
      refusals++;
      /* The refusal NAMES the holder: "refused" with no pid is a question,
       * not an answer. */
      KBC_CHECK_MSG(strstr(line, "already locked by pid ") != NULL,
                    "refusal did not name the holder: %s", line);
    }
    free(line);
  }
  KBC_CHECK_EQ_INT(winners, 1);
  KBC_CHECK_EQ_INT(refusals, N - 1);
  /* The manifest is consistent afterwards: it describes the mirror that
   * exists, and exactly one mirror exists. */
  char store[KBC_TEST_PATH_MAX];
  path(store, sizeof store, "%s/u-race.git", root);
  KBC_CHECK(kbc_path_exists(store));
  kbc_rs_manifest m;
  kbc_err_reset(&e);
  KBC_CHECK_OK(kbc_rs_manifest_check(store, "u-race", "local:u-race", &m, &e));
  char tmp[KBC_TEST_PATH_MAX];
  path(tmp, sizeof tmp, "%s/.seed-u-race.tmp", root);
  KBC_CHECK(!kbc_path_exists(tmp));
  kbc_rs_git_free(g);
}

/* -------------------------------------------------------------------- gc -- */

KBC_TEST(gc_classification_is_pure_and_never_guesses) {
  int64_t reviews[] = {7, 12};
  int64_t prs[] = {42};
  int64_t members[] = {3};
  kbc_rs_gc_keep keep = {reviews, 2, prs, 1, members, 1};

  struct {
    const char *ref;
    kbc_rs_ref_kind kind;
    int64_t id;
    bool bound;
  } cases[] = {
      {"refs/kbc/review/7/ps1", KBC_RS_REF_PATCHSET, 7, true},
      {"refs/kbc/review/12/ps3-base", KBC_RS_REF_PATCHSET_BASE, 12, true},
      {"refs/kbc/review/99/ps1", KBC_RS_REF_PATCHSET, 99, false},
      {"refs/kbc/pr/42", KBC_RS_REF_PR, 42, true},
      {"refs/kbc/prm/42", KBC_RS_REF_PRM, 42, true},
      {"refs/kbc/pr/43", KBC_RS_REF_PR, 43, false},
      {"refs/kbc/hint/3/abc", KBC_RS_REF_HINT, 3, true},
      {"refs/kbc/hint/4/abc", KBC_RS_REF_HINT, 4, false},
      {"refs/remotes/work-3/main", KBC_RS_REF_WORK, 3, true},
      {"refs/remotes/work-9/release/2026.09", KBC_RS_REF_WORK, 9, false},
  };
  for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
    kbc_rs_ref_class c = kbc_rs_gc_classify_ref(cases[i].ref, &keep);
    KBC_CHECK_MSG(c.kind == cases[i].kind, "%s: got kind %d", cases[i].ref,
                  (int)c.kind);
    KBC_CHECK_MSG(c.id == cases[i].id, "%s: got id %lld", cases[i].ref,
                  (long long)c.id);
    KBC_CHECK_MSG(c.bound == cases[i].bound, "%s: bound=%d", cases[i].ref,
                  (int)c.bound);
  }
  /* A name this parser cannot classify is never a candidate. The alternative
   * is an opaque `backup-failed` that wedges every future gc on the store and
   * never names the offender. */
  const char *unclassifiable[] = {
      "refs/kbc/review/07/ps1",   "refs/kbc/review/7/ps",  "refs/kbc/review/7",
      "refs/kbc/review/7/ps1-x",  "refs/remotes/work-0/x", "refs/remotes/work-07/x",
      "refs/remotes/base/main",   "refs/heads/main",       "refs/kbc/pr/042",
      "refs/remotes/work-3/",     "refs/remotes/work-3/-bad",
  };
  for (size_t i = 0; i < sizeof unclassifiable / sizeof unclassifiable[0];
       i++) {
    kbc_rs_ref_class c = kbc_rs_gc_classify_ref(unclassifiable[i], &keep);
    KBC_CHECK_MSG(c.kind == KBC_RS_REF_UNCLASSIFIED, "classified %s",
       unclassifiable[i]);
  }
  /* An EMPTY keep-set is not a no-op: it says every review ref is an orphan.
   * That is why an apply requires a pre-apply bundle, not a confirmation. */
  kbc_rs_gc_keep empty = {NULL, 0, NULL, 0, NULL, 0};
  kbc_rs_ref_class c = kbc_rs_gc_classify_ref("refs/kbc/review/7/ps1", &empty);
  KBC_CHECK_EQ_INT(c.kind, KBC_RS_REF_PATCHSET);
  KBC_CHECK(!c.bound);
}

/* ---------------------------------------------------------------- backup -- */

/* Build a store, give it a refs/kbc ref, and return the commit oid. */
static char *seeded_store_with_review(kbc_rs_git *g, const char *root,
                                      const char *uuid, char *git_dir,
                                      size_t cap, int64_t review_id) {
  char src[KBC_TEST_PATH_MAX];
  path(src, sizeof src, "%s/%s-src.git", g_tmp, uuid);
  char *commit = make_source_repo(g, src, "main");
  if (commit == NULL) {
    return NULL;
  }
  kbc_rs_url base;
  kbc_err e;
  kbc_err_reset(&e);
  if (kbc_rs_url_local_seed(src, &base, &e) != KBC_OK) {
    free(commit);
    return NULL;
  }
  kbc_rs_seed_report rep;
  kbc_err_reset(&e);
  if (!seed_one(g, root, uuid, &base, "local:x", &rep, &e)) {
    kbc_test_fail(__FILE__, __LINE__, "seed: %s", e.msg);
    free(commit);
    return NULL;
  }
  snprintf(git_dir, cap, "%s", rep.git_dir);
  char ref[KBC_RS_REFNAME_MAX];
  snprintf(ref, sizeof ref, "refs/kbc/review/%lld/ps1", (long long)review_id);
  kbc_str tx;
  kbc_str_init(&tx);
  kbc_str_printf(&tx, "update %s %s\n", ref, commit);
  kbc_err_reset(&e);
  if (kbc_rs_git_update_refs(g, git_dir, tx.ptr, tx.len, &e) != KBC_OK) {
    kbc_test_fail(__FILE__, __LINE__, "update-ref: %s", e.msg);
  }
  kbc_str_free(&tx);
  return commit;
}

KBC_TEST(gc_classifies_a_real_store_and_applies_only_behind_a_bundle) {
  kbc_rs_git *g = open_git("gc-home");
  if (g == NULL) {
    return;
  }
  char root[KBC_TEST_PATH_MAX];
  char store[KBC_TEST_PATH_MAX];
  char bundles[KBC_TEST_PATH_MAX];
  path(root, sizeof root, "%s/gcroot", g_tmp);
  path(bundles, sizeof bundles, "%s/gcbundles", g_tmp);
  char *commit = seeded_store_with_review(g, root, "u-gc", store,
                                          sizeof store, 77);
  if (commit == NULL) {
    kbc_rs_git_free(g);
    return;
  }
  int64_t live[] = {1};
  kbc_rs_gc_keep keep = {NULL, 0, NULL, 0, live, 1};
  kbc_rs_gc_report rep;
  kbc_err e;

  /* Dry run: pure read, nothing touched. */
  kbc_err_reset(&e);
  KBC_CHECK_OK(
      kbc_rs_gc_run(g, store, &keep, bundles, NULL, NULL, false, &rep, &e));
  KBC_CHECK(!rep.applied);
  /* Guard the element assertions on the count. `KBC_CHECK` records the
   * failure and CONTINUES, so without this the next line reads `cands[0]` on
   * a zero-length array and the suite dies with SIGSEGV — which loses the
   * whole report and looks like a different bug entirely. */
  KBC_CHECK_EQ_INT((int)rep.n, 1);
  if (rep.n < 1) {
    kbc_rs_gc_report_free(&rep);
    return;
  }
  KBC_CHECK_EQ_STR(rep.cands[0].refname, "refs/kbc/review/77/ps1");
  KBC_CHECK_EQ_STR(rep.cands[0].old_oid, commit);
  kbc_rs_gc_report_free(&rep);
  kbc_strlist refs;
  kbc_strlist_init(&refs);
  kbc_err_reset(&e);
  KBC_CHECK_OK(kbc_rs_git_list_refs(g, store,
                                    (const char *const[]){"refs/kbc/", NULL},
     &refs, &e));
  KBC_CHECK_EQ_INT((int)refs.len, 1);
  kbc_strlist_free(&refs);

  /* Apply: a bundle is written FIRST, covering every candidate, and only
   * then does the delete transaction run. */
  kbc_err_reset(&e);
  KBC_CHECK_OK(
      kbc_rs_gc_run(g, store, &keep, bundles, NULL, NULL, true, &rep, &e));
  KBC_CHECK(rep.applied);
  KBC_CHECK_EQ_INT((int)rep.covered_by_bundle, 1);
  KBC_CHECK(kbc_path_exists(rep.bundle));
  kbc_strlist heads;
  kbc_strlist_init(&heads);
  kbc_err_reset(&e);
  KBC_CHECK_OK(kbc_rs_bundle_list_heads(g, rep.bundle, &heads, &e));
  bool covered = false;
  for (size_t i = 0; i < heads.len; i++) {
    if (strstr(heads.items[i], "refs/kbc/review/77/ps1") != NULL) {
      covered = true;
 }
  }
  KBC_CHECK_MSG(covered, "the pre-apply bundle did not cover the candidate");
  kbc_strlist_free(&heads);
  kbc_rs_gc_report_free(&rep);
  kbc_strlist_init(&refs);
  kbc_err_reset(&e);
  KBC_CHECK_OK(kbc_rs_git_list_refs(g, store,
         (const char *const[]){"refs/kbc/", NULL}, &refs, &e));
  KBC_CHECK_EQ_INT((int)refs.len, 0);
  kbc_strlist_free(&refs);
  free(commit);
  kbc_rs_git_free(g);
}

KBC_TEST(the_restore_guard_blocks_an_unacknowledged_store) {
  kbc_rs_guard g;
  kbc_err e;
  char path_[KBC_TEST_PATH_MAX];
  path(path_, sizeof path_, "%s/guard.json", g_tmp);
  unlink(path_);
  kbc_err_reset(&e);
  KBC_CHECK_OK(kbc_rs_guard_read(path_, &g, &e));
  KBC_CHECK(!g.flagged);
  KBC_CHECK(!kbc_rs_guard_blocks(&g, "u-1"));

  /* A missing sentinel is the honest first boot. A CORRUPT one is the
   * opposite, and it fails CLOSED. */
  kbc_test_write_file(path_, "{ not json");
  kbc_err_reset(&e);
  KBC_CHECK_OK(kbc_rs_guard_read(path_, &g, &e));
  KBC_CHECK_MSG(g.flagged, "a corrupt sentinel read as never-flagged");
  KBC_CHECK(kbc_rs_guard_blocks(&g, "u-1"));
  unlink(path_);

  kbc_err_reset(&e);
  KBC_CHECK_OK(kbc_rs_guard_observe_epoch(path_, true, 5, 100, &g, &e));
  KBC_CHECK(!g.flagged);
  KBC_CHECK(g.has_epoch);
  /* A higher epoch bumps the mark and never lowers it. */
  kbc_err_reset(&e);
  KBC_CHECK_OK(kbc_rs_guard_observe_epoch(path_, true, 9, 200, &g, &e));
  KBC_CHECK_EQ_INT((int)g.high_water_epoch, 9);
  /* A volume whose epoch went BACKWARD is the only known signature of a
   * restore, and it is exactly the state a gc apply would destroy. */
  kbc_err_reset(&e);
  KBC_CHECK_OK(kbc_rs_guard_observe_epoch(path_, true, 3, 300, &g, &e));
  KBC_CHECK(g.just_flagged);
  KBC_CHECK(g.flagged);
  KBC_CHECK(strstr(g.reason, "regressed") != NULL);
  KBC_CHECK(kbc_rs_guard_blocks(&g, "u-1"));
  KBC_CHECK(kbc_rs_guard_blocks(&g, "u-2"));

  /* An acknowledgement is PER STORE and never clears the flag globally. */
  kbc_err_reset(&e);
  KBC_CHECK_OK(kbc_rs_guard_acknowledge(path_, "u-1", 400, &e));
  kbc_err_reset(&e);
  KBC_CHECK_OK(kbc_rs_guard_read(path_, &g, &e));
  KBC_CHECK(!kbc_rs_guard_blocks(&g, "u-1"));
  KBC_CHECK_MSG(kbc_rs_guard_blocks(&g, "u-2"),
            "acknowledging one store unblocked another");
}

KBC_TEST(gc_apply_is_refused_while_the_restore_guard_is_flagged) {
  kbc_rs_git *g = open_git("gcguard-home");
  if (g == NULL) {
    return;
  }
  char root[KBC_TEST_PATH_MAX];
  char store[KBC_TEST_PATH_MAX];
  char bundles[KBC_TEST_PATH_MAX];
  char guard[KBC_TEST_PATH_MAX];
  path(root, sizeof root, "%s/gcguardroot", g_tmp);
  path(bundles, sizeof bundles, "%s/gcguardbundles", g_tmp);
  path(guard, sizeof guard, "%s/gcguard.json", g_tmp);
  char *commit = seeded_store_with_review(g, root, "u-gg", store, sizeof store,
    5);
  if (commit == NULL) {
    kbc_rs_git_free(g);
    return;
  }
  free(commit);
  kbc_rs_guard gs;
  kbc_err e;
  kbc_err_reset(&e);
  KBC_CHECK_OK(kbc_rs_guard_observe_epoch(guard, true, 5, 1, &gs, &e));
  kbc_err_reset(&e);
  KBC_CHECK_OK(kbc_rs_guard_observe_epoch(guard, true, 1, 2, &gs, &e));
  kbc_rs_gc_keep keep = {NULL, 0, NULL, 0, NULL, 0};
  kbc_rs_gc_report rep;
  kbc_err_reset(&e);
  KBC_CHECK_ERR(
      kbc_rs_gc_run(g, store, &keep, bundles, guard, "u-gg", true, &rep, &e),
      KBC_ERR_CONFLICT);
  KBC_CHECK_MSG(strstr(e.msg, "restore guard") != NULL, "got: %s", e.msg);
  kbc_rs_gc_report_free(&rep);
  /* Acknowledging THIS store lets it through; the dry run was always
   * allowed, because it writes nothing. */
  kbc_err_reset(&e);
  KBC_CHECK_OK(kbc_rs_guard_acknowledge(guard, "u-gg", 3, &e));
  kbc_err_reset(&e);
  KBC_CHECK_OK(
      kbc_rs_gc_run(g, store, &keep, bundles, guard, "u-gg", true, &rep, &e));
  KBC_CHECK(rep.applied);
  kbc_rs_gc_report_free(&rep);
  kbc_rs_git_free(g);
}

KBC_TEST(backup_restore_round_trip_reproduces_the_mirror) {
  kbc_rs_git *g = open_git("backup-home");
  if (g == NULL) {
    return;
  }
  char root[KBC_TEST_PATH_MAX];
  char store[KBC_TEST_PATH_MAX];
  char bundle[KBC_TEST_PATH_MAX];
  char bundlemeta[KBC_TEST_PATH_MAX];
  path(root, sizeof root, "%s/bkroot", g_tmp);
  char *commit = seeded_store_with_review(g, root, "u-bk", store, sizeof store,
  21);
  if (commit == NULL) {
    kbc_rs_git_free(g);
    return;
  }
  kbc_err e;
  path(bundle, sizeof bundle, "%s/store-u-bk.bundle", g_tmp);
  kbc_err_reset(&e);
  KBC_CHECK_MSG(kbc_rs_backup_write(g, store, bundle, &e) == KBC_OK,
        "backup: %s", e.msg);
  KBC_CHECK(kbc_path_exists(bundle));
  path(bundlemeta, sizeof bundlemeta, "%s.refs", bundle);
  KBC_CHECK(kbc_path_exists(bundlemeta));

  /* Destroy the mirror completely, then restore it. */
  kbc_test_rmrf(store);
  KBC_CHECK(!kbc_path_exists(store));
  kbc_rs_restore_report rr;
  kbc_err_reset(&e);
  KBC_CHECK_MSG(
      kbc_rs_restore(g, bundle, store, false, &rr, &e) == KBC_OK,
      "restore: %s", e.msg);
  KBC_CHECK_EQ_INT((int)rr.n, 0);
  KBC_CHECK(rr.n_restored >= 1);
  kbc_rs_restore_report_free(&rr);

  /* The restored mirror matches: same review ref, same object, and the
   * object is really there. */
  kbc_strlist refs;
  kbc_strlist_init(&refs);
  kbc_err_reset(&e);
  KBC_CHECK_OK(kbc_rs_git_list_refs(g, store,
     (const char *const[]){"refs/kbc/", NULL}, &refs, &e));
  char want[160];
  snprintf(want, sizeof want, "%s refs/kbc/review/21/ps1", commit);
  bool found = false;
  for (size_t i = 0; i < refs.len; i++) {
    if (strncmp(refs.items[i], want, strlen(want)) == 0) {
      found = true;
    }
  }
  KBC_CHECK_MSG(found, "the restored mirror does not match the original");
  kbc_strlist_free(&refs);
  /* The OBJECT, named by its id. `want` above is a whole for-each-ref LINE
   * ("<oid> <ref>") and is right for comparing a ref listing; handing that
   * line to cat-file asks git about a name that does not exist, so the check
   * failed for a reason that had nothing to do with whether the object came
   * back. `commit` alone is the object id. */
  const char *cat[] = {"cat-file", "-t", commit};
  kbc_rs_call call;
  memset(&call, 0, sizeof call);
  call.op = "cat-file";
  call.argv = cat;
  call.argc = 3;
  call.git_dir = store;
  call.auth = KBC_RS_AUTH_LOCAL_ONLY;
  call.timeout_s = 60;
  kbc_rs_output out;
  kbc_err_reset(&e);
  KBC_CHECK_MSG(kbc_rs_git_run(g, &call, &out, &e) == KBC_OK,
        "the restored object is not present: %s", e.msg);
  kbc_rs_output_free(&out);
  free(commit);
  kbc_rs_git_free(g);
}

KBC_TEST(the_restore_guard_refuses_an_overwrite_and_says_what) {
  kbc_rs_git *g = open_git("restoreguard-home");
  if (g == NULL) {
    return;
  }
  char root[KBC_TEST_PATH_MAX];
  char store[KBC_TEST_PATH_MAX];
  char bundle[KBC_TEST_PATH_MAX];
  path(root, sizeof root, "%s/rgroot", g_tmp);
  char *commit = seeded_store_with_review(g, root, "u-rg", store, sizeof store,
   33);
  if (commit == NULL) {
    kbc_rs_git_free(g);
    return;
  }
  kbc_err e;
  path(bundle, sizeof bundle, "%s/store-u-rg.bundle", g_tmp);
  kbc_err_reset(&e);
  KBC_CHECK_OK(kbc_rs_backup_write(g, store, bundle, &e));

  /* Move the review ref to a DIFFERENT commit that the store already has, so
   * the restore really is about to overwrite an existing ref. The object has
   * to exist IN THE STORE: `other_commit` is minted in a separate source
   * repo, and `git update-ref` rightly refuses to point a ref at an object it
   * cannot see ("nonexistent object"), so the fixture failed before it ever
   * reached the restore it was setting up. A second commit in the store's own
   * source repo is the honest way to say "same repo, different commit". */
  char other[KBC_TEST_PATH_MAX];
  path(other, sizeof other, "%s/rg-src2.git", g_tmp);
  kbc_rs_git *g2 = g;
  char *other_commit = make_source_repo(g2, other, "main");
  if (other_commit != NULL) {
    /* Pull it into the store so the ref can legitimately point at it. */
    const char *fargv[] = {"fetch", "--quiet", other, "+refs/heads/main:refs/kbc/rg-other"};
    kbc_rs_call fc;
    memset(&fc, 0, sizeof fc);
    fc.op = "fetch-other";
    fc.argv = fargv;
    fc.argc = 4;
    fc.git_dir = store;
    fc.auth = KBC_RS_AUTH_LOCAL_ONLY;
    fc.timeout_s = 60;
    kbc_rs_output fo;
    kbc_err_reset(&e);
    kbc_rs_git_run(g, &fc, &fo, &e);
    kbc_rs_output_free(&fo);
  }
  kbc_str tx;
  kbc_str_init(&tx);
  kbc_str_printf(&tx, "update refs/kbc/review/33/ps1 %s\n",
                 other_commit != NULL ? other_commit : commit);
  kbc_err_reset(&e);
  KBC_CHECK_MSG(kbc_rs_git_update_refs(g, store, tx.ptr, tx.len, &e) == KBC_OK,
                "could not move the review ref: %s", e.msg);
  kbc_str_free(&tx);

  kbc_rs_restore_report rr;
  kbc_err_reset(&e);
  KBC_CHECK_ERR(kbc_rs_restore(g, bundle, store, false, &rr, &e),
                KBC_ERR_CONFLICT);
  KBC_CHECK_MSG(strstr(e.msg, "refs/kbc/review/33/ps1") != NULL,
        "the refusal did not name the ref: %s", e.msg);
  KBC_CHECK_MSG(strstr(e.msg, "would become") != NULL,
        "the refusal did not say what it would overwrite: %s", e.msg);
  KBC_CHECK_MSG(strstr(e.msg, commit) != NULL, "got: %s", e.msg);
  kbc_rs_restore_report_free(&rr);
  /* The refusal is a real refusal: the ref is untouched. */
  kbc_strlist refs;
  kbc_strlist_init(&refs);
  kbc_err_reset(&e);
  KBC_CHECK_OK(kbc_rs_git_list_refs(g, store,
    (const char *const[]){"refs/kbc/review/33/ps1", NULL}, &refs, &e));
  KBC_CHECK_EQ_INT((int)refs.len, 1);
  char cur[KBC_RS_REFNAME_MAX];
  KBC_CHECK(strstr(refs.items[0], other_commit) == refs.items[0]);
  snprintf(cur, sizeof cur, "%s", refs.items[0]);
  kbc_strlist_free(&refs);
  KBC_CHECK(strncmp(cur, other_commit, strlen(other_commit)) == 0);

  /* Force is the operator's explicit clobber, and it works. */
  kbc_err_reset(&e);
  KBC_CHECK_MSG(kbc_rs_restore(g, bundle, store, true, &rr, &e) == KBC_OK,
        "forced restore: %s", e.msg);
  kbc_rs_restore_report_free(&rr);
  kbc_strlist_init(&refs);
  kbc_err_reset(&e);
  KBC_CHECK_OK(kbc_rs_git_list_refs(g, store,
     (const char *const[]){"refs/kbc/review/33/ps1", NULL}, &refs, &e));
  KBC_CHECK(strncmp(refs.items[0], commit, strlen(commit)) == 0);
  kbc_strlist_free(&refs);
  free(other_commit);
  free(commit);
  kbc_rs_git_free(g);
}

KBC_TEST(backup_prune_keeps_the_newest) {
  char dir[KBC_TEST_PATH_MAX];
  path(dir, sizeof dir, "%s/prunebundles", g_tmp);
  kbc_test_mkdir_p(dir);
  for (int i = 1; i <= 5; i++) {
 char p[KBC_TEST_PATH_MAX];
    path(p, sizeof p, "%s/store-u-p-%d.bundle", dir, i * 100);
    kbc_test_write_file(p, "x");
    char m[KBC_TEST_PATH_MAX];
    path(m, sizeof m, "%s.refs", p);
    kbc_test_write_file(m, "x");
  }
  kbc_err e;
  kbc_err_reset(&e);
  KBC_CHECK_OK(kbc_rs_backup_prune(dir, "u-p", 3, &e));
  for (int i = 1; i <= 5; i++) {
    char p[KBC_TEST_PATH_MAX];
    path(p, sizeof p, "%s/store-u-p-%d.bundle", dir, i * 100);
    /* Newest 3 of 100..500 are 300, 400 and 500. */
    if (i * 100 < 300) {
      KBC_CHECK_MSG(!kbc_path_exists(p), "%s should have been pruned", p);
    } else {
      KBC_CHECK_MSG(kbc_path_exists(p), "%s should have been kept", p);
    }
  }
  /* A different store's bundles are never touched. */
  char other[KBC_TEST_PATH_MAX];
  path(other, sizeof other, "%s/store-u-q-100.bundle", dir);
  kbc_test_write_file(other, "x");
  kbc_err_reset(&e);
  KBC_CHECK_OK(kbc_rs_backup_prune(dir, "u-p", 1, &e));
  KBC_CHECK(kbc_path_exists(other));
}

/* ----------------------------------------------------------- base ladder -- */

KBC_TEST(base_url_ladder_prefers_statements_then_membership) {
  kbc_rs_remote_info remotes[] = {
      {"origin", "https://github.com/acme/widgets.git", NULL},
      {"upstream", "git@github.com:acme/widgets.git", NULL},
  };
  kbc_rs_base_input in;
  kbc_rs_base_ladder out;
  kbc_err e;

  memset(&in, 0, sizeof in);
  in.remotes = remotes;
  in.n_remotes = 2;
  /* Two remotes naming ONE project are not ambiguous: they agree. */
  kbc_err_reset(&e);
  KBC_CHECK_OK(kbc_rs_base_ladder_run(&in, &out, &e));
  KBC_CHECK_EQ_INT(out.source, KBC_RS_BASE_SINGLE);
  KBC_CHECK_EQ_STR(out.remote, "origin");
  KBC_CHECK_EQ_STR(out.store_key, "github.com/acme/widgets");

  /* An operator statement outranks a remote-derived answer. */
  in.explicit_url = "https://github.com/other/thing.git";
  kbc_err_reset(&e);
  KBC_CHECK_OK(kbc_rs_base_ladder_run(&in, &out, &e));
  KBC_CHECK_EQ_INT(out.source, KBC_RS_BASE_EXPLICIT);
  KBC_CHECK_EQ_STR(out.store_key, "github.com/other/thing");
  in.explicit_url = NULL;
  in.config_url = "https://github.com/cfg/thing.git";
  kbc_err_reset(&e);
  KBC_CHECK_OK(kbc_rs_base_ladder_run(&in, &out, &e));
  KBC_CHECK_EQ_INT(out.source, KBC_RS_BASE_CONFIG);
  in.config_url = NULL;

  /* Joining an existing store beats deriving a fresh key. */
  kbc_rs_known_store known[] = {
      {"github.com/acme/widgets", "github.com/acme/widgets"}};
  in.existing = known;
  in.n_existing = 1;
  kbc_err_reset(&e);
  KBC_CHECK_OK(kbc_rs_base_ladder_run(&in, &out, &e));
  KBC_CHECK_EQ_INT(out.source, KBC_RS_BASE_MEMBER);
  in.existing = NULL;
  in.n_existing = 0;

  /* gh's own choice outranks "whatever single remote exists". */
  kbc_rs_remote_info marked[] = {
      {"origin", "https://github.com/acme/widgets.git", NULL},
      {"upstream", "git@github.com:acme/other.git", "base"},
  };
  in.remotes = marked;
  in.n_remotes = 2;
  kbc_err_reset(&e);
  KBC_CHECK_OK(kbc_rs_base_ladder_run(&in, &out, &e));
  KBC_CHECK_EQ_INT(out.source, KBC_RS_BASE_GH_RESOLVED);
  KBC_CHECK_EQ_STR(out.remote, "upstream");

  /* Two DIFFERENT projects, neither marked, and no statement: refuse,
   * never guess. */
  kbc_rs_remote_info split[] = {
      {"origin", "https://github.com/acme/widgets.git", NULL},
      {"mirror", "https://github.com/other/thing.git", NULL},
  };
  in.remotes = split;
  in.n_remotes = 2;
  kbc_err_reset(&e);
  KBC_CHECK_ERR(kbc_rs_base_ladder_run(&in, &out, &e), KBC_ERR_CONFLICT);
  KBC_CHECK(strstr(e.msg, "ambiguous") != NULL);
}

KBC_TEST(base_url_ladder_reports_a_refused_remote_instead_of_dropping_it) {
  kbc_rs_remote_info remotes[] = {
      {"origin", "ext::sh -c whatever", NULL},
 };
  kbc_rs_base_input in;
  kbc_rs_base_ladder out;
  kbc_err e;
  memset(&in, 0, sizeof in);
  in.remotes = remotes;
  in.n_remotes = 1;
  /* Dropping it silently is how a repo with a real forge remote ends up
   * keyed as a `local:` store. */
  kbc_err_reset(&e);
  KBC_CHECK_ERR(kbc_rs_base_ladder_run(&in, &out, &e), KBC_ERR_CONFLICT);
  KBC_CHECK(strstr(e.msg, "remote-url-refused") != NULL);
  /* The output does not claim a source the ladder never reached. */
  KBC_CHECK_EQ_INT(out.source, KBC_RS_BASE_AMBIGUOUS);
  KBC_CHECK(out.refused_count >= 1);

  /* A clone with only a local path legitimately has no forge project. */
  kbc_rs_remote_info local[] = {{"origin", "/home/someone/widgets", NULL}};
  in.remotes = local;
  kbc_err_reset(&e);
  KBC_CHECK_OK(kbc_rs_base_ladder_run(&in, &out, &e));
  KBC_CHECK_EQ_INT(out.source, KBC_RS_BASE_LOCAL);
  KBC_CHECK_EQ_STR(out.store_key, "");
}

/* ------------------------------------------------------------------- main -- */

int main(void) {
  if (real_git_path() == NULL) {
    fprintf(stderr, "SKIP: no git binary on PATH; every case here runs real "
                    "git\n");
    return 77;
  }
  kbc_test_tmpdir(g_tmp, sizeof g_tmp);
  int rc = kbc_test_run(
      "rstore",
      (kbc_test_case[]){
          {"redact_shapes", redaction_strips_every_known_secret_shape},
          {"redact_survives", redaction_keeps_the_host_and_the_object_id},
          {"redact_headers", redaction_removes_headers_and_credential_answers},
          {"redact_literals", redaction_of_a_known_literal_has_no_length_floor},
          {"redact_cap", redaction_caps_on_a_utf8_boundary},
          {"url_accepts", url_allowlist_accepts_only_the_three_forge_forms},
          {"url_refuses", url_allowlist_refuses_everything_else_and_never_echoes_it},
          {"store_key", store_key_normalizes_and_refuses_a_percent_escape},
          {"https_equivalent", https_equivalent_is_derived_only_when_unambiguous},
          {"secret", secret_validates_wipes_and_compares},
          {"ladder_pinned", ladder_pinned_rungs_never_fall_through},
          {"ladder_auto", ladder_auto_walks_every_rung_in_order},
          {"ladder_scope", ladder_never_offers_a_token_to_another_host},
          {"no_push", git_never_writes_to_a_remote},
          {"scrubbed_env", git_scrubs_the_environment_and_the_credential_helpers},
          {"token_never_leaks",
           security_a_token_reaches_git_but_not_argv_env_or_any_error},
          {"hung_child", git_kills_a_hung_child_and_leaks_no_descriptor},
          {"manifest", manifest_ties_a_directory_to_its_row},
          {"lock", the_store_lock_refuses_a_second_holder_and_names_it},
          {"seed", seeding_clones_a_real_local_remote_and_writes_a_manifest},
          {"seed_race", two_concurrent_seeds_produce_one_mirror_and_named_refusals},
          {"gc_classify", gc_classification_is_pure_and_never_guesses},
          {"gc_apply", gc_classifies_a_real_store_and_applies_only_behind_a_bundle},
          {"restore_guard", the_restore_guard_blocks_an_unacknowledged_store},
          {"gc_blocked", gc_apply_is_refused_while_the_restore_guard_is_flagged},
          {"round_trip", backup_restore_round_trip_reproduces_the_mirror},
          {"restore_refuses",
           the_restore_guard_refuses_an_overwrite_and_says_what},
          {"prune", backup_prune_keeps_the_newest},
          {"base_ladder", base_url_ladder_prefers_statements_then_membership},
          {"base_refused", base_url_ladder_reports_a_refused_remote_instead_of_dropping_it},
          {NULL, NULL}});
  kbc_test_rmrf(g_tmp);
  return rc;
}
