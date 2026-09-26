/* test_config.c — src/config.c: the kb.toml subset parser, the overlay that
 * lands on a kbc_config, validation, the bind-safety property and the dump
 * round trip that `kbc config show` depends on. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "kbc/config.h"
#include "kbc/mem.h"
#include "kbc_test.h"

/* An absolute data_dir for the cases that want one, so the assertions are
 * about resolution and not about what a relative value turns into. */
#define TESTDATA_DIR "/tmp/kbc-config-data-dir"

/* -Wwrite-strings: config fields are owned char*, so literals need copying. */
static char *dup_str(const char *s) {
  size_t n;
  char *p;
  if (s == NULL) {
    return NULL;
  }
  n = strlen(s) + 1u;
  p = (char *)malloc(n);
  if (p != NULL) {
    memcpy(p, s, n);
  }
  return p;
}

/* "<dir>/<name>", the one helper the cases need over and over. */
static void join(char *out, size_t cap, const char *dir, const char *name) {
  size_t dl = strlen(dir);
  size_t nl = strlen(name);
  if (dl + 1u + nl + 1u > cap) {
    kbc_test_fail(__FILE__, __LINE__, "path too long: %s/%s", dir, name);
    out[0] = '\0';
    return;
  }
  memcpy(out, dir, dl);
  out[dl] = '/';
  memcpy(out + dl + 1u, name, nl + 1u);
}

static void write_cfg(const char *dir, const char *name, const char *text) {
  char path[KBC_TEST_PATH_MAX];
  join(path, sizeof path, dir, name);
  kbc_test_write_file(path, text);
}

/* Loads <dir>/<name> over a fresh defaults config. */
static kbc_status load_text(kbc_config **out, const char *dir, const char *name,
                            kbc_err *err) {
  char path[KBC_TEST_PATH_MAX];
  kbc_config *cfg = kbc_config_defaults();
  kbc_status st;
  if (cfg == NULL) {
    return kbc_err_set(err, KBC_ERR_NOMEM, "out of memory");
  }
  join(path, sizeof path, dir, name);
  kbc_err_reset(err);
  st = kbc_config_load_file(cfg, path, err);
  if (st != KBC_OK) {
    kbc_config_free(cfg);
    return st;
  }
  *out = cfg;
  return KBC_OK;
}

/* Every rejection must name what was wrong and where: the message is the only
 * thing the operator ever sees. */
static void expect_reject(const char *dir, const char *text, const char *needle,
                          size_t line) {
  kbc_err err;
  kbc_config *cfg = NULL;
  kbc_status st;
  write_cfg(dir, "bad.toml", text);
  st = load_text(&cfg, dir, "bad.toml", &err);
  KBC_CHECK(st != KBC_OK);
  KBC_CHECK_ERR_MSG(err);
  if (st != KBC_OK) {
    KBC_CHECK_MSG(strstr(err.msg, needle) != NULL, "message %s does not name %s",
                  err.msg, needle);
    if (line > 0u) {
      char marker[32];
      (void)snprintf(marker, sizeof marker, ":%zu:", line);
      KBC_CHECK_MSG(strstr(err.msg, marker) != NULL,
                    "message %s does not point at line %zu", err.msg, line);
    }
  }
  kbc_config_free(cfg);
}

/* Appends a corpus built by hand, for the validate cases that never go near
 * the filesystem through the parser. */
static void add_corpus(kbc_config *c, const char *name, const char *path) {
  size_t n = c->ncorpora + 1u;
  kbc_corpus_cfg *grown =
      (kbc_corpus_cfg *)realloc(c->corpora, n * sizeof *grown);
  if (grown == NULL) {
    kbc_test_fail(__FILE__, __LINE__, "out of memory growing corpora");
    return;
  }
  c->corpora = grown;
  memset(&c->corpora[c->ncorpora], 0, sizeof c->corpora[0]);
  c->corpora[c->ncorpora].name = dup_str(name);
  c->corpora[c->ncorpora].path = dup_str(path);
  kbc_strlist_init(&c->corpora[c->ncorpora].ignore);
  c->ncorpora = n;
}

/* ------------------------------------------------------------- defaults -- */

KBC_TEST(defaults_match_the_documented_values) {
  kbc_config *c = kbc_config_defaults();
  KBC_CHECK_NOT_NULL(c);
  if (c == NULL) {
    return;
  }
  KBC_CHECK_EQ_INT(c->port, 4317);
  KBC_CHECK_EQ_STR(c->bind_addr, "127.0.0.1");
  KBC_CHECK_EQ_DBL(c->bm25_k1, 1.2, 1e-12);
  KBC_CHECK_EQ_DBL(c->bm25_b, 0.75, 1e-12);
  KBC_CHECK_EQ_INT(c->rrf_k, 60);
  KBC_CHECK_EQ_INT(c->ncorpora, 0);
  KBC_CHECK_NULL(c->token);
  /* Also the paths and sizes config.h pins, since a silent default change
   * would move an existing daemon's database. */
  KBC_CHECK_EQ_STR(c->data_dir, "data");
  KBC_CHECK_EQ_STR(c->db_path, "data/kb.db");
  KBC_CHECK_EQ_STR(c->index_path, "data/index");
  KBC_CHECK_EQ_STR(c->token_path, "data/token");
  KBC_CHECK_EQ_INT(c->chunk_max_bytes, 65536);
  KBC_CHECK_EQ_INT(c->search_max_hits, 50);
  KBC_CHECK_EQ_INT(c->watcher_debounce_ms, 250);
  KBC_CHECK_EQ_INT(c->http_workers, 4);
  KBC_CHECK(!c->json_logs);
  KBC_CHECK_EQ_INT(c->log_level, KBC_LOG_INFO);
  kbc_config_free(c);
}

/* ---------------------------------------------------------------- parse -- */

KBC_TEST(full_file_lands_every_key_in_its_field) {
  char dir[KBC_TEST_PATH_MAX];
  char c1[KBC_TEST_PATH_MAX];
  char c2[KBC_TEST_PATH_MAX];
  char want1[KBC_TEST_PATH_MAX];
  char want2[KBC_TEST_PATH_MAX];
  char want3[KBC_TEST_PATH_MAX];
  char want4[KBC_TEST_PATH_MAX];
  char want5[KBC_TEST_PATH_MAX];
  char want6[KBC_TEST_PATH_MAX];
  char want_cfg[KBC_TEST_PATH_MAX];
  const kbc_corpus_cfg *beta;
  kbc_err err;
  kbc_config *cfg = NULL;

  kbc_test_tmpdir(dir, sizeof dir);
  join(c1, sizeof c1, dir, "c1");
  join(c2, sizeof c2, dir, "c2");
  kbc_test_mkdir_p(c1);
  kbc_test_mkdir_p(c2);
  join(want1, sizeof want1, dir, "c1");
  join(want2, sizeof want2, dir, "c2");
  join(want_cfg, sizeof want_cfg, dir, "full.toml");

  /* The parser never stats a corpus path; validate() is what does. */
  write_cfg(dir, "full.toml",
            "# every key this build understands\n"
            "[daemon]\n"
            "bind = \"127.0.0.1\"\n"
            "port = 8080\n"
            "data_dir = \"state\"\n"
            "db_path = \"state/kb.db\"\n"
            "index_path = \"state/idx\"\n"
            "token_file = \"state/tok\"\n"
            "workers = 12\n"
            "log_level = \"debug\"\n"
            "json_logs = true\n"
            "chunk_max_bytes = 4096\n"
            "\n"
            "[search]\n"
            "bm25_k1 = 1.5\n"
            "bm25_b = 0.5\n"
            "rrf_k = 42\n"
            "max_hits = 25\n"
            "\n"
            "[watcher]\n"
            "debounce_ms = 100\n"
            "\n"
            "[embedder]\n"
            "command = \"/usr/bin/embed --dim 8\"\n"
            "\n"
            "[[corpus]]\n"
            "name = \"alpha\"\n"
            "path = \"c1\"\n"
            "ignore = [\"tmp\", \"*.swp\"]\n"
            "\n"
            "[[corpus]]\n"
            "name = \"beta\"\n"
            "path = \"c2\"\n"
            "ignore = []\n");

  KBC_CHECK_OK(load_text(&cfg, dir, "full.toml", &err));
  if (cfg == NULL) {
    kbc_test_rmrf(dir);
    return;
  }

  KBC_CHECK_EQ_STR(cfg->bind_addr, "127.0.0.1");
  KBC_CHECK_EQ_INT(cfg->port, 8080);
  /* Every relative path in a config is relative to the CONFIG FILE's
   * directory, never to the process cwd — same rule the corpus paths use. */
  join(want3, sizeof want3, dir, "state");
  join(want4, sizeof want4, dir, "state/kb.db");
  join(want5, sizeof want5, dir, "state/idx");
  join(want6, sizeof want6, dir, "state/tok");
  KBC_CHECK_EQ_STR(cfg->data_dir, want3);
  KBC_CHECK_EQ_STR(cfg->db_path, want4);
  KBC_CHECK_EQ_STR(cfg->index_path, want5);
  KBC_CHECK_EQ_STR(cfg->token_path, want6);
  KBC_CHECK_EQ_INT(cfg->http_workers, 12);
  KBC_CHECK_EQ_INT(cfg->log_level, KBC_LOG_DEBUG);
  KBC_CHECK(cfg->json_logs);
  KBC_CHECK_EQ_INT(cfg->chunk_max_bytes, 4096);
  KBC_CHECK_EQ_DBL(cfg->bm25_k1, 1.5, 1e-12);
  KBC_CHECK_EQ_DBL(cfg->bm25_b, 0.5, 1e-12);
  KBC_CHECK_EQ_INT(cfg->rrf_k, 42);
  KBC_CHECK_EQ_INT(cfg->search_max_hits, 25);
  KBC_CHECK_EQ_INT(cfg->watcher_debounce_ms, 100);
  KBC_CHECK_EQ_STR(cfg->embedder_cmd, "/usr/bin/embed --dim 8");
  KBC_CHECK_EQ_STR(cfg->config_path, want_cfg);

  /* Relative corpus paths resolve against the config file's directory, not
   * the process cwd. */
  KBC_CHECK_EQ_INT(cfg->ncorpora, 2);
  if (cfg->ncorpora == 2u) {
    KBC_CHECK_EQ_STR(cfg->corpora[0].name, "alpha");
    KBC_CHECK_EQ_STR(cfg->corpora[0].path, want1);
    KBC_CHECK_EQ_INT(cfg->corpora[0].ignore.len, 2);
    if (cfg->corpora[0].ignore.len == 2u) {
      KBC_CHECK_EQ_STR(cfg->corpora[0].ignore.items[0], "tmp");
      KBC_CHECK_EQ_STR(cfg->corpora[0].ignore.items[1], "*.swp");
    }
    KBC_CHECK_EQ_STR(cfg->corpora[1].name, "beta");
    KBC_CHECK_EQ_STR(cfg->corpora[1].path, want2);
    KBC_CHECK_EQ_INT(cfg->corpora[1].ignore.len, 0);
  }
  beta = kbc_config_corpus(cfg, "beta");
  KBC_CHECK_NOT_NULL(beta);
  if (beta != NULL) {
    KBC_CHECK_EQ_STR(beta->name, "beta");
  }
  KBC_CHECK_NULL(kbc_config_corpus(cfg, "gamma"));

  kbc_config_free(cfg);
  kbc_test_rmrf(dir);
}

/* The whole point of deriving the three paths from data_dir: a config that
 * says only data_dir must put db, index AND token inside it. The token file
 * used to stay at its "data/token" default, which is relative to the process
 * cwd, so the daemon died with "token: token file data/token cannot be read"
 * for anyone who started it from anywhere but the data directory's parent. */
KBC_TEST(data_dir_alone_puts_every_path_inside_the_data_dir) {
  char dir[KBC_TEST_PATH_MAX];
  char want[KBC_TEST_PATH_MAX];
  kbc_err err;
  kbc_config *cfg = NULL;

  kbc_test_tmpdir(dir, sizeof dir);
  write_cfg(dir, "d.toml", "[daemon]\ndata_dir = \"" TESTDATA_DIR "\"\n");

  KBC_CHECK_OK(load_text(&cfg, dir, "d.toml", &err));
  if (cfg == NULL) {
    kbc_test_rmrf(dir);
    return;
  }
  /* Absolute data_dir, asserted on the RESOLVED path: no part of any of them
   * may still hang off the cwd. */
  join(want, sizeof want, TESTDATA_DIR, "kb.db");
  KBC_CHECK_EQ_STR(cfg->data_dir, TESTDATA_DIR);
  KBC_CHECK_EQ_STR(cfg->db_path, want);
  join(want, sizeof want, TESTDATA_DIR, "index");
  KBC_CHECK_EQ_STR(cfg->index_path, want);
  join(want, sizeof want, TESTDATA_DIR, "token");
  KBC_CHECK_EQ_STR(cfg->token_path, want);

  kbc_config_free(cfg);
  kbc_test_rmrf(dir);
}

/* A relative data_dir resolves against the config file's directory, so the
 * derived paths are absolute too and are still inside that data_dir. */
KBC_TEST(a_relative_data_dir_derives_paths_below_the_config_directory) {
  char dir[KBC_TEST_PATH_MAX];
  char want[KBC_TEST_PATH_MAX];
  kbc_err err;
  kbc_config *cfg = NULL;

  kbc_test_tmpdir(dir, sizeof dir);
  write_cfg(dir, "d.toml", "[daemon]\ndata_dir = \"sd\"\n");

  KBC_CHECK_OK(load_text(&cfg, dir, "d.toml", &err));
  if (cfg == NULL) {
    kbc_test_rmrf(dir);
    return;
  }
  join(want, sizeof want, dir, "sd");
  KBC_CHECK_EQ_STR(cfg->data_dir, want);
  join(want, sizeof want, dir, "sd/kb.db");
  KBC_CHECK_EQ_STR(cfg->db_path, want);
  join(want, sizeof want, dir, "sd/index");
  KBC_CHECK_EQ_STR(cfg->index_path, want);
  join(want, sizeof want, dir, "sd/token");
  KBC_CHECK_EQ_STR(cfg->token_path, want);

  kbc_config_free(cfg);
  kbc_test_rmrf(dir);
}

KBC_TEST(an_explicit_token_file_still_wins_over_the_derived_one) {
  char dir[KBC_TEST_PATH_MAX];
  char want[KBC_TEST_PATH_MAX];
  kbc_err err;
  kbc_config *cfg = NULL;

  kbc_test_tmpdir(dir, sizeof dir);
  write_cfg(dir, "d.toml",
            "[daemon]\n"
            "data_dir = \"" TESTDATA_DIR "\"\n"
            "token_file = \"" TESTDATA_DIR "/keys/tok\"\n"
            "index_path = \"" TESTDATA_DIR "/idx\"\n");

  KBC_CHECK_OK(load_text(&cfg, dir, "d.toml", &err));
  if (cfg == NULL) {
    kbc_test_rmrf(dir);
    return;
  }
  join(want, sizeof want, TESTDATA_DIR, "keys/tok");
  KBC_CHECK_EQ_STR(cfg->token_path, want);
  join(want, sizeof want, TESTDATA_DIR, "idx");
  KBC_CHECK_EQ_STR(cfg->index_path, want);
  /* The one it did NOT pin is still derived, and still inside data_dir. */
  join(want, sizeof want, TESTDATA_DIR, "kb.db");
  KBC_CHECK_EQ_STR(cfg->db_path, want);

  kbc_config_free(cfg);
  kbc_test_rmrf(dir);
}

KBC_TEST(a_relative_explicit_token_file_resolves_against_the_config_dir) {
  char dir[KBC_TEST_PATH_MAX];
  char want[KBC_TEST_PATH_MAX];
  kbc_err err;
  kbc_config *cfg = NULL;

  kbc_test_tmpdir(dir, sizeof dir);
  write_cfg(dir, "d.toml",
            "[daemon]\ntoken_file = \"keys/tok\"\n"
            "db_path = \"db/kb.db\"\n");

  KBC_CHECK_OK(load_text(&cfg, dir, "d.toml", &err));
  if (cfg == NULL) {
    kbc_test_rmrf(dir);
    return;
  }
  join(want, sizeof want, dir, "keys/tok");
  KBC_CHECK_EQ_STR(cfg->token_path, want);
  join(want, sizeof want, dir, "db/kb.db");
  KBC_CHECK_EQ_STR(cfg->db_path, want);
  /* No data_dir in this file, so the omitted path keeps its documented
   * default rather than being invented from a data_dir that was never set. */
  KBC_CHECK_EQ_STR(cfg->index_path, "data/index");

  kbc_config_free(cfg);
  kbc_test_rmrf(dir);
}

/* The property a user actually feels: `kbc token generate` writes to
 * cfg->token_path and the daemon reads from cfg->token_path, so a token
 * generated after loading this config IS the token the daemon serves. Two
 * different resolutions here would be a silent "I generated a token and the
 * daemon still says it has none". */
KBC_TEST(the_derived_token_file_is_the_one_the_daemon_reads) {
  char dir[KBC_TEST_PATH_MAX];
  char ddir[KBC_TEST_PATH_MAX];
  char want[KBC_TEST_PATH_MAX];
  kbc_err err;
  kbc_config *cfg = NULL;

  kbc_test_tmpdir(dir, sizeof dir);
  join(ddir, sizeof ddir, dir, "sd");
  kbc_test_mkdir_p(ddir);
  write_cfg(dir, "d.toml", "[daemon]\ndata_dir = \"sd\"\n");

  KBC_CHECK_OK(load_text(&cfg, dir, "d.toml", &err));
  if (cfg == NULL) {
    kbc_test_rmrf(dir);
    return;
  }
  /* `kbc token generate` writes cfg->token_path; kbc_config_load_token — the
   * daemon's own read path — reads that same field. */
  kbc_test_write_file(cfg->token_path, "secret-token\n");
  /* What `kbc daemon` does: read the token back through this same config. */
  kbc_err_reset(&err);
  KBC_CHECK_OK(kbc_config_load_token(cfg, &err));
  KBC_CHECK_EQ_STR(cfg->token, "secret-token");
  /* And it is the path the config asked for, not a cwd-relative guess. */
  join(want, sizeof want, ddir, "token");
  KBC_CHECK_EQ_STR(cfg->token_path, want);

  kbc_config_free(cfg);
  kbc_test_rmrf(dir);
}

/* Moving data_dir must move all three, with nothing still pointing at the
 * old directory. */
KBC_TEST(moving_data_dir_moves_every_derived_path) {
  char dir[KBC_TEST_PATH_MAX];
  char a[KBC_TEST_PATH_MAX];
  char b[KBC_TEST_PATH_MAX];
  char want[KBC_TEST_PATH_MAX];
  kbc_err err;
  kbc_config *one = NULL;
  kbc_config *two = NULL;

  kbc_test_tmpdir(dir, sizeof dir);
  join(a, sizeof a, dir, "a");
  join(b, sizeof b, dir, "b");
  write_cfg(dir, "a.toml", "[daemon]\ndata_dir = \"a\"\n");
  write_cfg(dir, "b.toml", "[daemon]\ndata_dir = \"b\"\n");

  KBC_CHECK_OK(load_text(&one, dir, "a.toml", &err));
  KBC_CHECK_OK(load_text(&two, dir, "b.toml", &err));
  if (one == NULL || two == NULL) {
    kbc_config_free(one);
    kbc_config_free(two);
    kbc_test_rmrf(dir);
    return;
  }
  join(want, sizeof want, b, "kb.db");
  KBC_CHECK_EQ_STR(two->db_path, want);
  join(want, sizeof want, b, "index");
  KBC_CHECK_EQ_STR(two->index_path, want);
  join(want, sizeof want, b, "token");
  KBC_CHECK_EQ_STR(two->token_path, want);
  /* Nothing may still name the first directory. */
  KBC_CHECK(strstr(two->data_dir, "/a") == NULL);
  KBC_CHECK(strstr(two->db_path, "/a/") == NULL);
  KBC_CHECK(strstr(two->index_path, "/a/") == NULL);
  KBC_CHECK(strstr(two->token_path, "/a/") == NULL);

  kbc_config_free(one);
  kbc_config_free(two);
  kbc_test_rmrf(dir);
}

KBC_TEST(partial_file_keeps_every_default_it_omits) {
  char dir[KBC_TEST_PATH_MAX];
  kbc_err err;
  kbc_config *cfg = NULL;

  kbc_test_tmpdir(dir, sizeof dir);
  write_cfg(dir, "partial.toml", "[daemon]\nport = 9999\n");

  KBC_CHECK_OK(load_text(&cfg, dir, "partial.toml", &err));
  if (cfg != NULL) {
    KBC_CHECK_EQ_INT(cfg->port, 9999);
    KBC_CHECK_EQ_STR(cfg->bind_addr, "127.0.0.1");
    KBC_CHECK_EQ_STR(cfg->data_dir, "data");
    KBC_CHECK_EQ_STR(cfg->db_path, "data/kb.db");
    KBC_CHECK_EQ_STR(cfg->index_path, "data/index");
    KBC_CHECK_EQ_STR(cfg->token_path, "data/token");
    KBC_CHECK_EQ_DBL(cfg->bm25_k1, 1.2, 1e-12);
    KBC_CHECK_EQ_DBL(cfg->bm25_b, 0.75, 1e-12);
    KBC_CHECK_EQ_INT(cfg->rrf_k, 60);
    KBC_CHECK_EQ_INT(cfg->search_max_hits, 50);
    KBC_CHECK_EQ_INT(cfg->chunk_max_bytes, 65536);
    KBC_CHECK_EQ_INT(cfg->watcher_debounce_ms, 250);
    KBC_CHECK_EQ_INT(cfg->http_workers, 4);
    KBC_CHECK_EQ_INT(cfg->log_level, KBC_LOG_INFO);
    KBC_CHECK(!cfg->json_logs);
    KBC_CHECK_NULL(cfg->embedder_cmd);
    KBC_CHECK_EQ_INT(cfg->ncorpora, 0);
    kbc_config_free(cfg);
  }
  kbc_test_rmrf(dir);
}

KBC_TEST(missing_file_is_ok_and_leaves_the_defaults) {
  char dir[KBC_TEST_PATH_MAX];
  char path[KBC_TEST_PATH_MAX];
  kbc_err err;
  kbc_config *cfg = kbc_config_defaults();

  kbc_test_tmpdir(dir, sizeof dir);
  join(path, sizeof path, dir, "absent.toml");
  kbc_err_reset(&err);
  KBC_CHECK_OK(kbc_config_load_file(cfg, path, &err));
  KBC_CHECK_EQ_STR(cfg->bind_addr, "127.0.0.1");
  KBC_CHECK_EQ_INT(cfg->port, 4317);
  KBC_CHECK_EQ_STR(cfg->data_dir, "data");
  KBC_CHECK_EQ_INT(cfg->ncorpora, 0);
  /* Nothing was loaded, so there is no file to report back to the operator. */
  KBC_CHECK_NULL(cfg->config_path);
  kbc_config_free(cfg);
  kbc_test_rmrf(dir);
}

KBC_TEST(unsupported_and_out_of_range_syntax_is_rejected) {
  char dir[KBC_TEST_PATH_MAX];
  kbc_test_tmpdir(dir, sizeof dir);

  expect_reject(dir, "[daemon]\nbindd = \"127.0.0.1\"\n", "bindd", 2);
  expect_reject(dir, "[daemon]\nsub.nested = 1\n", "dotted", 2);
  expect_reject(dir, "[daemon]\njson_logs = { a = 1 }\n", "inline tables", 2);
  expect_reject(dir, "[[daemon]]\n", "[[daemon]]", 1);
  expect_reject(dir, "[daemon]\nbind = \"\"\"x\"\"\"\n",
                "multi-line strings", 2);
  expect_reject(dir, "[daemon]\nport = \"8080\"\n", "expects an integer", 2);
  expect_reject(dir, "[daemon]\nport = 0\n", "port", 2);
  expect_reject(dir, "[daemon]\nport = 70000\n", "port", 2);
  expect_reject(dir, "[search]\nbm25_b = 1.5\n", "bm25_b", 2);
  expect_reject(dir, "[search]\nbm25_k1 = 0\n", "bm25_k1", 2);
  expect_reject(dir, "[watcher]\ndebounce_ms = -1\n", "debounce_ms", 2);
  expect_reject(dir, "[daemon]\nlog_level = \"loud\"\n", "log_level", 2);
  expect_reject(dir, "[daemon]\nworkers = 0\n", "workers", 2);
  expect_reject(dir, "[nope]\nport = 1\n", "[nope]", 1);
  expect_reject(dir, "[corpus]\nname = \"a\"\n", "[[corpus]]", 1);
  expect_reject(dir, "port = 1\n", "before any", 1);
  expect_reject(dir, "[daemon]\n[[corpus]]\nname = \"a\"\npath = \"/tmp\"\n"
                      "ignore = \"tmp\"\n",
                "expects an array of strings", 5);

  kbc_test_rmrf(dir);
}

/* A key repeated in one table is an operator mistake that silently changes
 * meaning, so the parser must name it rather than take the last one. */
KBC_TEST(a_repeated_key_is_rejected) {
  char dir[KBC_TEST_PATH_MAX];
  kbc_test_tmpdir(dir, sizeof dir);
  expect_reject(dir, "[daemon]\nport = 1\nport = 2\n", "port", 3);
  expect_reject(dir, "[daemon]\nbind = \"127.0.0.1\"\nbind = \"0.0.0.0\"\n",
                "bind", 3);
  expect_reject(dir, "[[corpus]]\nname = \"a\"\nname = \"b\"\npath = \"/tmp\"\n",
                "name", 3);
  kbc_test_rmrf(dir);
}

KBC_TEST(rejection_does_not_half_apply) {
  char dir[KBC_TEST_PATH_MAX];
  kbc_err err;
  kbc_config *cfg = kbc_config_defaults();

  kbc_test_tmpdir(dir, sizeof dir);
  /* port 9000 parses; the bad debounce after it must not leave port applied
   * while the rest of the file was skipped. */
  write_cfg(dir, "half.toml", "[daemon]\nport = 9000\n"
                             "\n[watcher]\ndebounce_ms = -1\n");
  kbc_err_reset(&err);
  {
    char path[KBC_TEST_PATH_MAX];
    join(path, sizeof path, dir, "half.toml");
    kbc_err_reset(&err);
    KBC_CHECK(kbc_config_load_file(cfg, path, &err) != KBC_OK);
    /* The overlay is applied only after the whole file parses, so nothing from
     * a rejected file is visible. */
    KBC_CHECK_EQ_INT(cfg->port, 4317);
    KBC_CHECK_EQ_INT(cfg->watcher_debounce_ms, 250);
  }
  kbc_config_free(cfg);
  kbc_test_rmrf(dir);
}

/* ------------------------------------------------------------ validation -- */

KBC_TEST(validate_accepts_defaults_and_a_real_corpus) {
  char dir[KBC_TEST_PATH_MAX];
  char c1[KBC_TEST_PATH_MAX];
  kbc_err err;
  kbc_config *cfg = kbc_config_defaults();

  kbc_test_tmpdir(dir, sizeof dir);
  join(c1, sizeof c1, dir, "c1");
  kbc_test_mkdir_p(c1);
  kbc_err_reset(&err);
  KBC_CHECK_OK(kbc_config_validate(cfg, &err));
  add_corpus(cfg, "alpha", c1);
  kbc_err_reset(&err);
  KBC_CHECK_OK(kbc_config_validate(cfg, &err));
  kbc_config_free(cfg);
  kbc_test_rmrf(dir);
}

KBC_TEST(validate_rejects_bad_corpora_and_bind) {
  char dir[KBC_TEST_PATH_MAX];
  char c1[KBC_TEST_PATH_MAX];
  char gone[KBC_TEST_PATH_MAX];
  kbc_err err;
  kbc_config *cfg = kbc_config_defaults();

  kbc_test_tmpdir(dir, sizeof dir);
  join(c1, sizeof c1, dir, "c1");
  join(gone, sizeof gone, dir, "gone");
  kbc_test_mkdir_p(c1);

  add_corpus(cfg, "alpha", c1);
  add_corpus(cfg, "alpha", c1);
  kbc_err_reset(&err);
  KBC_CHECK_ERR(kbc_config_validate(cfg, &err), KBC_ERR_INVALID);
  KBC_CHECK_ERR_MSG(err);
  KBC_CHECK_MSG(strstr(err.msg, "alpha") != NULL, "message %s does not name "
                "the duplicate corpus", err.msg);

  /* De-duplicate by renaming, not by dropping the entry: the config still
   * owns what add_corpus gave it. */
  free(cfg->corpora[1].name);
  cfg->corpora[1].name = dup_str("beta");
  free(cfg->corpora[0].name);
  cfg->corpora[0].name = dup_str("");
  kbc_err_reset(&err);
  KBC_CHECK_ERR(kbc_config_validate(cfg, &err), KBC_ERR_INVALID);
  KBC_CHECK_MSG(strstr(err.msg, "empty name") != NULL, "message %s does not "
                "mention the empty name", err.msg);

  free(cfg->corpora[0].name);
  cfg->corpora[0].name = dup_str("alpha");
  free(cfg->corpora[0].path);
  cfg->corpora[0].path = dup_str(gone);
  kbc_err_reset(&err);
  KBC_CHECK_ERR(kbc_config_validate(cfg, &err), KBC_ERR_INVALID);
  KBC_CHECK_MSG(strstr(err.msg, "not a directory") != NULL, "message %s does "
                "not say the path is not a directory", err.msg);

  free(cfg->corpora[0].path);
  cfg->corpora[0].path = dup_str(c1);
  kbc_strlist_push(&cfg->corpora[0].ignore, "tmp");
  kbc_err_reset(&err);
  KBC_CHECK_OK(kbc_config_validate(cfg, &err));

  free(cfg->bind_addr);
  cfg->bind_addr = dup_str("localhost");
  kbc_err_reset(&err);
  KBC_CHECK_ERR(kbc_config_validate(cfg, &err), KBC_ERR_INVALID);
  KBC_CHECK_MSG(strstr(err.msg, "literal") != NULL, "message %s does not say "
                "bind must be a literal", err.msg);

  kbc_config_free(cfg);
  kbc_test_rmrf(dir);
}

KBC_TEST(validate_rejects_a_public_bind_with_no_token) {
  kbc_err err;
  kbc_config *cfg = kbc_config_defaults();

  free(cfg->bind_addr);
  cfg->bind_addr = dup_str("0.0.0.0");
  kbc_err_reset(&err);
  KBC_CHECK_ERR(kbc_config_validate(cfg, &err), KBC_ERR_INVALID);
  KBC_CHECK_MSG(strstr(err.msg, "token") != NULL, "message %s does not "
                "mention the token", err.msg);
  kbc_config_free(cfg);
}

/* ----------------------------------------------------------- bind safety -- */

/* The security property: loopback is safe with no token, anything else needs
 * one, and the refusal text is what the CLI prints. */
static void check_bind(const char *addr, const char *token, bool want_safe) {
  kbc_config *cfg = kbc_config_defaults();
  char why[KBC_ERR_MSG_MAX];
  bool safe;

  free(cfg->bind_addr);
  cfg->bind_addr = dup_str(addr);
  cfg->token = dup_str(token);
  memset(why, 'x', sizeof why);
  safe = kbc_config_bind_is_safe(cfg, why, sizeof why);
  KBC_CHECK_MSG(safe == want_safe, "bind %s (token %s): got safe=%d", addr,
                token != NULL ? "set" : "(none)", (int)safe);
  if (safe) {
    KBC_CHECK_MSG(why[0] == '\0', "a safe bind must leave why empty, got %s",
                  why);
  } else {
    KBC_CHECK_MSG(why[0] != '\0', "an unsafe bind must explain itself");
    /* The message is printed to the operator: it must not carry the secret. */
    if (token != NULL) {
      KBC_CHECK_MSG(strstr(why, token) == NULL,
                    "refusal text leaks the token: %s", why);
    }
  }
  kbc_config_free(cfg);
}

KBC_TEST(bind_safety_follows_the_token_rule) {
  check_bind("127.0.0.1", NULL, true);
  check_bind("127.0.0.2", NULL, true); /* still 127/8 loopback */
  check_bind("::1", NULL, true);
  check_bind("0.0.0.0", NULL, false);
  check_bind("0.0.0.0", "s3cret-token", true);
  check_bind("192.168.1.5", NULL, false);
  check_bind("192.168.1.5", "s3cret-token", true);
  check_bind("10.0.0.1", NULL, false);
  check_bind("8.8.8.8", NULL, false);
  check_bind("::", NULL, false);
  check_bind("not-an-ip", "s3cret-token", false);
  check_bind("127.0.0.1.evil", NULL, false);
  check_bind("127.0.0.1 ", NULL, false); /* not a literal, despite the prefix */
  check_bind("", NULL, false);
}

KBC_TEST(bind_refusal_names_the_token_remedy) {
  kbc_config *cfg = kbc_config_defaults();
  char why[KBC_ERR_MSG_MAX];

  free(cfg->bind_addr);
  cfg->bind_addr = dup_str("0.0.0.0");
  KBC_CHECK(!kbc_config_bind_is_safe(cfg, why, sizeof why));
  KBC_CHECK_MSG(strstr(why, "token") != NULL, "refusal omits the token: %s",
                why);
  KBC_CHECK_MSG(strstr(why, "kbc token generate") != NULL,
                "refusal omits the remedy: %s", why);
  KBC_CHECK_MSG(strstr(why, "127.0.0.1") != NULL,
                "refusal omits the loopback alternative: %s", why);
  KBC_CHECK_MSG(strstr(why, "0.0.0.0") != NULL,
                "refusal does not name the address: %s", why);
  KBC_CHECK_MSG(strstr(why, "4317") != NULL, "refusal does not name the port: "
                "%s", why);
  kbc_config_free(cfg);
}

KBC_TEST(bind_safety_is_null_safe) {
  char why[KBC_ERR_MSG_MAX];
  KBC_CHECK(!kbc_config_bind_is_safe(NULL, why, sizeof why));
  KBC_CHECK(why[0] != '\0');
  /* A NULL/zero buffer must not be written to. */
  KBC_CHECK(!kbc_config_bind_is_safe(NULL, NULL, 0u));
}

/* ------------------------------------------------------- token resolution -- */

/* A config whose only non-default is where the token lives. */
static kbc_config *token_cfg(const char *dir, const char *path) {
  kbc_config *cfg = kbc_config_defaults();
  KBC_CHECK_NOT_NULL(cfg);
  if (cfg == NULL) {
    return NULL;
  }
  free(cfg->token_path);
  cfg->token_path = dup_str(path);
  (void)dir;
  return cfg;
}

KBC_TEST(token_resolution_prefers_a_literal_over_the_file) {
  char dir[KBC_TEST_PATH_MAX];
  char path[KBC_TEST_PATH_MAX];
  kbc_err err;
  kbc_config *cfg;

  kbc_test_tmpdir(dir, sizeof dir);
  join(path, sizeof path, dir, "tok");
  kbc_test_write_file(path, "from-the-file\n");
  cfg = token_cfg(dir, path);
  cfg->token = dup_str("from-the-config");
  kbc_err_reset(&err);
  KBC_CHECK_OK(kbc_config_load_token(cfg, &err));
  KBC_CHECK_EQ_STR(cfg->token, "from-the-config");
  kbc_config_free(cfg);
  kbc_test_rmrf(dir);
}

KBC_TEST(token_resolution_reads_the_first_line_of_the_file) {
  char dir[KBC_TEST_PATH_MAX];
  char path[KBC_TEST_PATH_MAX];
  kbc_err err;
  kbc_config *cfg;

  kbc_test_tmpdir(dir, sizeof dir);
  join(path, sizeof path, dir, "tok");
  /* A trailing newline and a second line must not leak into the token: a
   * token is one line, and the newline is what `kbc token generate` writes. */
  kbc_test_write_file(path, "first-line-token\nsecond-line\n\n");
  cfg = token_cfg(dir, path);
  kbc_err_reset(&err);
  KBC_CHECK_OK(kbc_config_load_token(cfg, &err));
  KBC_CHECK_EQ_STR(cfg->token, "first-line-token");
  kbc_config_free(cfg);

  /* CRLF: the \r is part of the line terminator, not of the secret. */
  kbc_test_write_file(path, "crlf-token\r\n");
  cfg = token_cfg(dir, path);
  kbc_err_reset(&err);
  KBC_CHECK_OK(kbc_config_load_token(cfg, &err));
  KBC_CHECK_EQ_STR(cfg->token, "crlf-token");
  kbc_config_free(cfg);

  /* No trailing newline at all. */
  kbc_test_write_file(path, "unterminated");
  cfg = token_cfg(dir, path);
  kbc_err_reset(&err);
  KBC_CHECK_OK(kbc_config_load_token(cfg, &err));
  KBC_CHECK_EQ_STR(cfg->token, "unterminated");
  kbc_config_free(cfg);
  kbc_test_rmrf(dir);
}

KBC_TEST(an_empty_token_file_resolves_to_no_token) {
  char dir[KBC_TEST_PATH_MAX];
  char path[KBC_TEST_PATH_MAX];
  char why[KBC_ERR_MSG_MAX];
  kbc_err err;
  kbc_config *cfg;

  kbc_test_tmpdir(dir, sizeof dir);
  join(path, sizeof path, dir, "tok");
  kbc_test_write_file(path, "\n");
  cfg = token_cfg(dir, path);
  free(cfg->bind_addr);
  cfg->bind_addr = dup_str("0.0.0.0");
  kbc_err_reset(&err);
  KBC_CHECK_OK(kbc_config_load_token(cfg, &err));
  KBC_CHECK(cfg->token != NULL);
  KBC_CHECK_EQ_STR(cfg->token, "");
  /* An accidentally blank token file must NOT quietly open a public bind. */
  KBC_CHECK(!kbc_config_bind_is_safe(cfg, why, sizeof why));
  kbc_config_free(cfg);
  kbc_test_rmrf(dir);
}

/* A configured token file that cannot be read is an ERROR, not "no token":
 * treating it as no token is how a public bind ends up unauthenticated. */
KBC_TEST(a_missing_token_file_is_an_io_error_naming_the_path) {
  char dir[KBC_TEST_PATH_MAX];
  char path[KBC_TEST_PATH_MAX];
  kbc_err err;
  kbc_config *cfg;

  kbc_test_tmpdir(dir, sizeof dir);
  join(path, sizeof path, dir, "absent-token");
  cfg = token_cfg(dir, path);
  kbc_err_reset(&err);
  KBC_CHECK_ERR(kbc_config_load_token(cfg, &err), KBC_ERR_IO);
  KBC_CHECK_ERR_MSG(err);
  KBC_CHECK_MSG(strstr(err.msg, path) != NULL,
                "the error must name the unreadable path, got: %s", err.msg);
  /* And it must not have invented a token out of the failure. */
  KBC_CHECK_NULL(cfg->token);
  kbc_config_free(cfg);
  kbc_test_rmrf(dir);
}

KBC_TEST(no_token_file_configured_is_ok_with_no_token) {
  kbc_err err;
  kbc_config *cfg = kbc_config_defaults();

  free(cfg->token_path);
  cfg->token_path = NULL;
  kbc_err_reset(&err);
  KBC_CHECK_OK(kbc_config_load_token(cfg, &err));
  KBC_CHECK_NULL(cfg->token);
  kbc_config_free(cfg);

  /* An empty path is "not configured" too, not a read of the cwd. */
  cfg = kbc_config_defaults();
  free(cfg->token_path);
  cfg->token_path = dup_str("");
  kbc_err_reset(&err);
  KBC_CHECK_OK(kbc_config_load_token(cfg, &err));
  KBC_CHECK_NULL(cfg->token);
  kbc_config_free(cfg);
}

/* Idempotent: a second call must not append to, re-read, or leak the first
 * result. The contract is "cfg->token if it already carries one", so a value
 * loaded from a file is a value from then on; a rotation is picked up by
 * reloading the config, which is what a restarting daemon does. */
KBC_TEST(token_resolution_is_idempotent) {
  char dir[KBC_TEST_PATH_MAX];
  char path[KBC_TEST_PATH_MAX];
  kbc_err err;
  kbc_config *cfg;

  kbc_test_tmpdir(dir, sizeof dir);
  join(path, sizeof path, dir, "tok");
  kbc_test_write_file(path, "rotated-a\n");
  cfg = token_cfg(dir, path);
  kbc_err_reset(&err);
  KBC_CHECK_OK(kbc_config_load_token(cfg, &err));
  KBC_CHECK_EQ_STR(cfg->token, "rotated-a");
  for (int i = 0; i < 3; i++) {
    KBC_CHECK_OK(kbc_config_load_token(cfg, &err));
    KBC_CHECK_EQ_STR(cfg->token, "rotated-a");
  }
  kbc_config_free(cfg);
  kbc_test_rmrf(dir);
}

KBC_TEST(a_second_resolve_after_the_file_changes_sees_the_new_value) {
  char dir[KBC_TEST_PATH_MAX];
  char path[KBC_TEST_PATH_MAX];
  kbc_err err;
  kbc_config *cfg;

  kbc_test_tmpdir(dir, sizeof dir);
  join(path, sizeof path, dir, "tok");
  kbc_test_write_file(path, "token-one\n");
  cfg = token_cfg(dir, path);
  kbc_err_reset(&err);
  KBC_CHECK_OK(kbc_config_load_token(cfg, &err));
  KBC_CHECK_EQ_STR(cfg->token, "token-one");
  kbc_config_free(cfg);

  /* The rotated file, resolved by a config loaded after the rotation. */
  kbc_test_write_file(path, "token-two\n");
  cfg = token_cfg(dir, path);
  kbc_err_reset(&err);
  KBC_CHECK_OK(kbc_config_load_token(cfg, &err));
  KBC_CHECK_EQ_STR(cfg->token, "token-two");
  kbc_config_free(cfg);
  kbc_test_rmrf(dir);
}

KBC_TEST(token_resolution_is_null_safe) {
  kbc_err err;
  kbc_err_reset(&err);
  KBC_CHECK_ERR(kbc_config_load_token(NULL, &err), KBC_ERR_INVALID);
}

/* The property the whole function exists for: a token FILE, not a literal,
 * makes a routable bind safe — and its absence still refuses, with the
 * refusal text. Before the resolver existed, cfg->token was never set from a
 * file, so this first case was impossible and the second was the only one
 * reachable. */
KBC_TEST(a_token_file_makes_a_routable_bind_safe) {
  char dir[KBC_TEST_PATH_MAX];
  char path[KBC_TEST_PATH_MAX];
  char why[KBC_ERR_MSG_MAX];
  kbc_err err;
  kbc_config *cfg;

  kbc_test_tmpdir(dir, sizeof dir);
  join(path, sizeof path, dir, "tok");
  kbc_test_write_file(path, "file-supplied-secret\n");
  cfg = token_cfg(dir, path);
  free(cfg->bind_addr);
  cfg->bind_addr = dup_str("0.0.0.0");
  kbc_err_reset(&err);
  KBC_CHECK_OK(kbc_config_load_token(cfg, &err));
  memset(why, 'x', sizeof why);
  KBC_CHECK_MSG(kbc_config_bind_is_safe(cfg, why, sizeof why),
                "a token file must make a public bind safe");
  KBC_CHECK_EQ_STR(why, "");
  /* And the daemon gate is live, not the loopback tier: validate agrees. */
  kbc_err_reset(&err);
  KBC_CHECK_OK(kbc_config_validate(cfg, &err));
  kbc_config_free(cfg);

  /* No token file at all: still refused, still explained. */
  join(path, sizeof path, dir, "no-such-token");
  cfg = token_cfg(dir, path);
  free(cfg->bind_addr);
  cfg->bind_addr = dup_str("0.0.0.0");
  kbc_err_reset(&err);
  KBC_CHECK_ERR(kbc_config_load_token(cfg, &err), KBC_ERR_IO);
  /* Nothing resolved, so the guard refuses rather than opening the bind. */
  KBC_CHECK(!kbc_config_bind_is_safe(cfg, why, sizeof why));
  KBC_CHECK(strstr(why, "kbc token generate") != NULL);
  kbc_config_free(cfg);
  kbc_test_rmrf(dir);
}

/* `kbc config show` prints the dump. A secret in there is a secret in every
 * bug report, every log collector, every paste into a chat. */
KBC_TEST(dump_never_contains_the_token_value) {
  char dir[KBC_TEST_PATH_MAX];
  char path[KBC_TEST_PATH_MAX];
  kbc_err err;
  kbc_str out;
  kbc_config *cfg;

  kbc_test_tmpdir(dir, sizeof dir);
  join(path, sizeof path, dir, "tok");
  kbc_test_write_file(path, "the-secret-value-9f3a\n");
  cfg = token_cfg(dir, path);
  kbc_err_reset(&err);
  KBC_CHECK_OK(kbc_config_load_token(cfg, &err));
  KBC_CHECK_EQ_STR(cfg->token, "the-secret-value-9f3a");
  kbc_str_init(&out);
  KBC_CHECK_OK(kbc_config_dump(cfg, &out, &err));
  KBC_CHECK_MSG(strstr(out.ptr, "the-secret-value-9f3a") == NULL,
 "the dump leaks the token value: %s", out.ptr);
 /* The path is not a secret and is expected in the dump. */
  KBC_CHECK_MSG(strstr(out.ptr, path) != NULL,
  "the dump should still name the token FILE: %s", out.ptr);
  kbc_str_free(&out);
  kbc_config_free(cfg);
  kbc_test_rmrf(dir);
}

/* --------------------------------------------------------- dump round trip -- */

static void same_config(const kbc_config *a, const kbc_config *b) {
  size_t i;
  KBC_CHECK_EQ_STR(a->bind_addr, b->bind_addr);
  KBC_CHECK_EQ_INT(a->port, b->port);
  KBC_CHECK_EQ_STR(a->data_dir, b->data_dir);
  KBC_CHECK_EQ_STR(a->db_path, b->db_path);
  KBC_CHECK_EQ_STR(a->index_path, b->index_path);
  KBC_CHECK_EQ_STR(a->token_path, b->token_path);
  KBC_CHECK_EQ_INT(a->http_workers, b->http_workers);
  KBC_CHECK_EQ_INT(a->log_level, b->log_level);
  KBC_CHECK_EQ_INT(a->json_logs, b->json_logs);
  KBC_CHECK_EQ_INT(a->chunk_max_bytes, b->chunk_max_bytes);
  KBC_CHECK_EQ_INT(a->watcher_debounce_ms, b->watcher_debounce_ms);
  KBC_CHECK_EQ_INT(a->search_max_hits, b->search_max_hits);
  KBC_CHECK_EQ_INT(a->rrf_k, b->rrf_k);
  /* %.17g in the dump: a double must come back bit-identical, not merely
   * close, or ranking changes under `kbc config show`. */
  KBC_CHECK(a->bm25_k1 == b->bm25_k1);
  KBC_CHECK(a->bm25_b == b->bm25_b);
  if (a->embedder_cmd == NULL || b->embedder_cmd == NULL) {
    KBC_CHECK(a->embedder_cmd == b->embedder_cmd);
  } else {
    KBC_CHECK_EQ_STR(a->embedder_cmd, b->embedder_cmd);
  }
  KBC_CHECK_EQ_INT(a->ncorpora, b->ncorpora);
  if (a->ncorpora != b->ncorpora) {
    return;
  }
  for (i = 0; i < a->ncorpora; i++) {
    size_t k;
    KBC_CHECK_EQ_STR(a->corpora[i].name, b->corpora[i].name);
    KBC_CHECK_EQ_STR(a->corpora[i].path, b->corpora[i].path);
    KBC_CHECK_EQ_INT(a->corpora[i].ignore.len, b->corpora[i].ignore.len);
    if (a->corpora[i].ignore.len != b->corpora[i].ignore.len) {
      continue;
    }
    for (k = 0; k < a->corpora[i].ignore.len; k++) {
      KBC_CHECK_EQ_STR(a->corpora[i].ignore.items[k],
                       b->corpora[i].ignore.items[k]);
    }
  }
}

KBC_TEST(dump_then_load_is_a_fixed_point) {
  char dir[KBC_TEST_PATH_MAX];
  char c1[KBC_TEST_PATH_MAX];
  kbc_str out;
  kbc_err err;
  kbc_config *first = NULL;
  kbc_config *second = NULL;

  kbc_test_tmpdir(dir, sizeof dir);
  join(c1, sizeof c1, dir, "c1");
  kbc_test_mkdir_p(c1);
  write_cfg(dir, "src.toml",
            "[daemon]\n"
            "bind = \"127.0.0.1\"\n"
            "port = 4321\n"
            "data_dir = \"state\"\n"
            "db_path = \"state/kb.db\"\n"
            "index_path = \"state/idx\"\n"
            "token_file = \"state/tok\"\n"
            "workers = 7\n"
            "log_level = \"warn\"\n"
            "json_logs = true\n"
            "chunk_max_bytes = 8192\n"
            "\n"
            "[search]\n"
            "bm25_k1 = 1.2345678901234567\n"
            "bm25_b = 0.3333333333333333\n"
            "rrf_k = 17\n"
            "max_hits = 9\n"
            "\n"
            "[watcher]\n"
            "debounce_ms = 0\n"
            "\n"
            "[embedder]\n"
            "command = \"/bin/embed\"\n"
            "\n"
            "[[corpus]]\n"
            "name = \"alpha\"\n"
            "path = \"c1\"\n"
            "ignore = [\"tmp\", \"a b\"]\n");

  KBC_CHECK_OK(load_text(&first, dir, "src.toml", &err));
  if (first == NULL) {
    kbc_test_rmrf(dir);
    return;
  }

  kbc_str_init(&out);
  kbc_err_reset(&err);
  KBC_CHECK_OK(kbc_config_dump(first, &out, &err));
  KBC_CHECK(out.len > 0u);

  {
    char dump_path[KBC_TEST_PATH_MAX];
    join(dump_path, sizeof dump_path, dir, "dump.toml");
    kbc_test_write_file(dump_path, out.ptr);
    kbc_config *loaded = kbc_config_defaults();
    kbc_err_reset(&err);
    KBC_CHECK_OK(kbc_config_load_file(loaded, dump_path, &err));
    second = loaded;
  }
  if (second != NULL) {
    same_config(first, second);
  }
  kbc_str_free(&out);
  kbc_config_free(first);
  kbc_config_free(second);
  kbc_test_rmrf(dir);
}

KBC_TEST(dump_preserves_a_string_that_looks_like_syntax) {
  char dir[KBC_TEST_PATH_MAX];
  char path[KBC_TEST_PATH_MAX];
  char want[KBC_TEST_PATH_MAX];
  kbc_str out;
  kbc_err err;
  kbc_config *cfg = kbc_config_defaults();
  kbc_config *back = NULL;

  kbc_test_tmpdir(dir, sizeof dir);
  /* A quote, a backslash and a '#' must survive the dump as data, not be
   * re-read as TOML syntax. */
  free(cfg->data_dir);
  cfg->data_dir = dup_str("a\"b\\c#d");
  kbc_str_init(&out);
  kbc_err_reset(&err);
  KBC_CHECK_OK(kbc_config_dump(cfg, &out, &err));
  join(path, sizeof path, dir, "tricky.toml");
  kbc_test_write_file(path, out.ptr);
  back = kbc_config_defaults();
  kbc_err_reset(&err);
  KBC_CHECK_OK(kbc_config_load_file(back, path, &err));
  if (back != NULL) {
    join(want, sizeof want, dir, "a\"b\\c#d");
    /* The escaping round-trips, and the value still resolves as a relative
     * path against the config file's directory, like any other. */
    KBC_CHECK_EQ_STR(back->data_dir, want);
  }
  kbc_str_free(&out);
  kbc_config_free(cfg);
  kbc_config_free(back);
  kbc_test_rmrf(dir);
}

KBC_TEST(dump_refuses_a_config_with_nothing_to_dump) {
  kbc_str out;
  kbc_err err;
  kbc_config *cfg = kbc_config_defaults();

  kbc_str_init(&out);
  free(cfg->bind_addr);
  cfg->bind_addr = NULL;
  kbc_err_reset(&err);
  KBC_CHECK_ERR(kbc_config_dump(cfg, &out, &err), KBC_ERR_INVALID);
  KBC_CHECK_ERR_MSG(err);
  kbc_str_free(&out);
  kbc_config_free(cfg);
}

/* ------------------------------------------------------------------ free -- */

KBC_TEST(free_tolerates_a_partially_built_config) {
  kbc_config *cfg = kbc_config_defaults();

  /* Half-built: a NULL string the dump path would have refused, a corpus with
   * no name, and a corpus carrying a populated ignore list. */
  free(cfg->bind_addr);
  cfg->bind_addr = NULL;
  free(cfg->token_path);
  cfg->token_path = NULL;
  cfg->token = NULL;
  cfg->embedder_cmd = NULL;

  cfg->corpora = (kbc_corpus_cfg *)calloc(2, sizeof *cfg->corpora);
  KBC_CHECK_NOT_NULL(cfg->corpora);
  if (cfg->corpora != NULL) {
    cfg->corpora[0].name = NULL;
    cfg->corpora[0].path = dup_str("/tmp");
    kbc_strlist_init(&cfg->corpora[0].ignore);
    cfg->corpora[1].name = dup_str("beta");
    cfg->corpora[1].path = dup_str("/tmp");
    kbc_strlist_init(&cfg->corpora[1].ignore);
    KBC_CHECK_OK(kbc_strlist_push(&cfg->corpora[1].ignore, "tmp"));
    KBC_CHECK_OK(kbc_strlist_push(&cfg->corpora[1].ignore, "*.swp"));
    cfg->ncorpora = 2u;
  }

  kbc_config_free(cfg);
  /* kbc_config_free() takes ownership of the struct itself (KBC_OWN on
   * kbc_config_defaults), so it is called exactly once per config; a second
   * call on the same pointer would be a use-after-free, not a no-op. The
   * NULL call below is the only repeat a caller may make. */
  kbc_config_free(NULL);
}

int main(void) {
  static const kbc_test_case cases[] = {
      {"defaults_match_the_documented_values", defaults_match_the_documented_values},
      {"full_file_lands_every_key_in_its_field", full_file_lands_every_key_in_its_field},
      {"data_dir_alone_puts_every_path_inside_the_data_dir",
       data_dir_alone_puts_every_path_inside_the_data_dir},
      {"a_relative_data_dir_derives_paths_below_the_config_directory",
       a_relative_data_dir_derives_paths_below_the_config_directory},
      {"an_explicit_token_file_still_wins_over_the_derived_one",
       an_explicit_token_file_still_wins_over_the_derived_one},
      {"a_relative_explicit_token_file_resolves_against_the_config_dir",
       a_relative_explicit_token_file_resolves_against_the_config_dir},
      {"the_derived_token_file_is_the_one_the_daemon_reads",
       the_derived_token_file_is_the_one_the_daemon_reads},
      {"moving_data_dir_moves_every_derived_path",
       moving_data_dir_moves_every_derived_path},
      {"partial_file_keeps_every_default_it_omits",
       partial_file_keeps_every_default_it_omits},
      {"missing_file_is_ok_and_leaves_the_defaults",
       missing_file_is_ok_and_leaves_the_defaults},
      {"unsupported_and_out_of_range_syntax_is_rejected",
       unsupported_and_out_of_range_syntax_is_rejected},
      {"a_repeated_key_is_rejected", a_repeated_key_is_rejected},
      {"rejection_does_not_half_apply", rejection_does_not_half_apply},
      {"validate_accepts_defaults_and_a_real_corpus",
       validate_accepts_defaults_and_a_real_corpus},
      {"validate_rejects_bad_corpora_and_bind", validate_rejects_bad_corpora_and_bind},
      {"validate_rejects_a_public_bind_with_no_token",
       validate_rejects_a_public_bind_with_no_token},
      {"bind_safety_follows_the_token_rule", bind_safety_follows_the_token_rule},
      {"bind_refusal_names_the_token_remedy", bind_refusal_names_the_token_remedy},
      {"bind_safety_is_null_safe", bind_safety_is_null_safe},
      {"token_resolution_prefers_a_literal_over_the_file",
       token_resolution_prefers_a_literal_over_the_file},
      {"token_resolution_reads_the_first_line_of_the_file",
       token_resolution_reads_the_first_line_of_the_file},
      {"an_empty_token_file_resolves_to_no_token",
       an_empty_token_file_resolves_to_no_token},
      {"a_missing_token_file_is_an_io_error_naming_the_path",
       a_missing_token_file_is_an_io_error_naming_the_path},
      {"no_token_file_configured_is_ok_with_no_token",
       no_token_file_configured_is_ok_with_no_token},
      {"token_resolution_is_idempotent", token_resolution_is_idempotent},
      {"a_second_resolve_after_the_file_changes_sees_the_new_value",
       a_second_resolve_after_the_file_changes_sees_the_new_value},
      {"token_resolution_is_null_safe", token_resolution_is_null_safe},
      {"a_token_file_makes_a_routable_bind_safe",
       a_token_file_makes_a_routable_bind_safe},
      {"dump_never_contains_the_token_value", dump_never_contains_the_token_value},
      {"dump_then_load_is_a_fixed_point", dump_then_load_is_a_fixed_point},
      {"dump_preserves_a_string_that_looks_like_syntax",
       dump_preserves_a_string_that_looks_like_syntax},
      {"dump_refuses_a_config_with_nothing_to_dump",
       dump_refuses_a_config_with_nothing_to_dump},
      {"free_tolerates_a_partially_built_config",
       free_tolerates_a_partially_built_config},
      {NULL, NULL},
  };
  return kbc_test_run("config", cases);
}
