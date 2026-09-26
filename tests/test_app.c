/* test_app.c — src/app.c end to end: the sequence the daemon actually runs.
 *
 * A markdown corpus on disk, a kbc_app over it, reindex, search, fetch, and
 * the transitions between those states (edit, delete, restart). Everything here
 * goes through the public header only; nothing pokes at app internals, because
 * the composition root's whole job is to hide them.
 *
 * The mtime+size fast path is pinned by tampering with a file's *contents*
 * while restoring its mtime: if the unchanged check regressed to a content
 * hash or a blind re-read, the tampered word would become findable.
 */
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "kbc/app.h"
#include "kbc/mem.h"
#include "kbc/store.h"
#include "kbc_test.h"

#define CORPUS_A "alpha"
#define CORPUS_B "beta"

/* Words chosen so that no two documents share vocabulary: a hit on one of them
 * names exactly one file, which is what makes "the right document is first" a
 * real assertion rather than a coin flip on scores. */
static const char DOC_A[] = "# Alpha Ledger\n\nThe ledger reconciles accruals.\n";
static const char DOC_B[] =
    "# Beta Chronicle\n\nThe chronicle mentions quixotic towns.\n";
static const char DOC_C[] = "# Gamma Digest\n\nThe digest catalogues verdigris.\n";
/* Same byte length as DOC_B with quixotic -> zephyrus; used to prove the
 * unchanged check keys on (mtime, size) and not on the bytes. */
static const char DOC_B_TAMPERED[] =
    "# Beta Chronicle\n\nThe chronicle mentions zephyrus towns.\n";

typedef struct {
  char root[KBC_TEST_PATH_MAX];
  char data[KBC_TEST_PATH_MAX];
  char corpus_a[KBC_TEST_PATH_MAX];
  char corpus_b[KBC_TEST_PATH_MAX];
  kbc_config *cfg;
  kbc_app *app;
} fixture;

static void join(char *out, size_t cap, const char *dir, const char *name) {
  int n = snprintf(out, cap, "%s/%s", dir, name);
  if (n < 0 || (size_t)n >= cap) {
    out[0] = '\0';
    KBC_CHECK_MSG(false, "path does not fit in %zu bytes", cap);
  }
}

/* Replaces the default relative data paths with ones under the fixture, so no
 * test can touch the developer's ./data. */
static kbc_config *make_cfg(const char *data, const char *corpus_a,
                            const char *corpus_b) {
  kbc_config *cfg = kbc_config_defaults();
  if (cfg == NULL) return NULL;

  free(cfg->data_dir);
  free(cfg->db_path);
  free(cfg->index_path);
  free(cfg->token_path);
  char p[KBC_TEST_PATH_MAX];
  cfg->data_dir = strdup(data);
  join(p, sizeof p, data, "kb.db");
  cfg->db_path = strdup(p);
  join(p, sizeof p, data, "index");
  cfg->index_path = strdup(p);
  join(p, sizeof p, data, "token");
  cfg->token_path = strdup(p);

  size_t n = corpus_b ? 2u : 1u;
  cfg->corpora = (kbc_corpus_cfg *)calloc(n, sizeof *cfg->corpora);
  if (cfg->corpora == NULL || cfg->data_dir == NULL || cfg->db_path == NULL ||
      cfg->index_path == NULL || cfg->token_path == NULL) {
    kbc_config_free(cfg);
    return NULL;
  }
  cfg->ncorpora = n;
  cfg->corpora[0].name = strdup(CORPUS_A);
  cfg->corpora[0].path = strdup(corpus_a);
  kbc_strlist_init(&cfg->corpora[0].ignore);
  if (corpus_b != NULL) {
    cfg->corpora[1].name = strdup(CORPUS_B);
    cfg->corpora[1].path = strdup(corpus_b);
    kbc_strlist_init(&cfg->corpora[1].ignore);
  }
  if (cfg->corpora[0].name == NULL || cfg->corpora[0].path == NULL) {
    kbc_config_free(cfg);
    return NULL;
  }
  return cfg;
}

static void fx_setup(fixture *f, bool two_corpora) {
  memset(f, 0, sizeof *f);
  kbc_test_tmpdir(f->root, sizeof f->root);
  join(f->data, sizeof f->data, f->root, "data");
  join(f->corpus_a, sizeof f->corpus_a, f->root, CORPUS_A);
  join(f->corpus_b, sizeof f->corpus_b, f->root, CORPUS_B);
  kbc_test_mkdir_p(f->corpus_a);
  if (two_corpora) kbc_test_mkdir_p(f->corpus_b);

  char p[KBC_TEST_PATH_MAX];
  join(p, sizeof p, f->corpus_a, "a.md");
  kbc_test_write_file(p, DOC_A);
  join(p, sizeof p, f->corpus_a, "b.md");
  kbc_test_write_file(p, DOC_B);
  join(p, sizeof p, f->corpus_a, "c.md");
  kbc_test_write_file(p, DOC_C);
  if (two_corpora) {
    join(p, sizeof p, f->corpus_b, "d.md");
    kbc_test_write_file(p, "# Delta Annex\n\nThe annex records verdigris.\n");
  }

  f->cfg = make_cfg(f->data, f->corpus_a, two_corpora ? f->corpus_b : NULL);
  KBC_CHECK_NOT_NULL(f->cfg);
  kbc_err err;
  kbc_err_reset(&err);
  f->app = f->cfg ? kbc_app_open(f->cfg, &err) : NULL;
  if (f->app == NULL) fprintf(stderr, "  app_open: %s\n", err.msg);
  KBC_CHECK_NOT_NULL(f->app);
}

static void fx_teardown(fixture *f) {
  kbc_app_close(f->app);
  kbc_config_free(f->cfg);
  kbc_test_rmrf(f->root);
}

/* The scalars of a search, copied out of the arena the result was built in:
 * the result struct points into that arena, so it cannot outlive it. */
typedef struct {
  bool degraded;
  bool vector_ran;
  double top_keyword_score;
} search_flags;

/* Runs a hybrid search in a fresh arena and copies the first row's path/title
 * out for the caller to inspect; returns the row count. */
static size_t first_hit_path(kbc_app *app, const char *q, const char *corpus,
                             char *path, size_t path_cap, char *title,
                             size_t title_cap, char *id, size_t id_cap,
                             search_flags *out_opt) {
  kbc_arena *a = kbc_arena_new(64u * 1024u);
  KBC_CHECK_NOT_NULL(a);
  if (a == NULL) return 0;

  kbc_query query;
  memset(&query, 0, sizeof query);
  query.q = q;
  query.corpus = corpus;
  query.kind = KBC_KIND__COUNT;
  query.mode = KBC_MODE_HYBRID;
  query.limit = 10;
  query.bm25_k1 = 1.2;
  query.bm25_b = 0.75;

  kbc_search_result res;
  memset(&res, 0, sizeof res);
  kbc_err err;
  kbc_err_reset(&err);
  kbc_status s = kbc_app_search(app, a, &query, &res, &err);
  KBC_CHECK_OK(s);
  if (out_opt != NULL) {
    out_opt->degraded = res.degraded;
    out_opt->vector_ran = res.vector_ran;
    out_opt->top_keyword_score = res.len > 0 ? res.rows[0].keyword_score : 0.0;
  }
  if (s == KBC_OK && res.len > 0) {
    if (path != NULL && path_cap > 0) {
      snprintf(path, path_cap, "%s", res.rows[0].path ? res.rows[0].path : "");
    }
    if (title != NULL && title_cap > 0) {
      snprintf(title, title_cap, "%s", res.rows[0].title ? res.rows[0].title : "");
    }
    if (id != NULL && id_cap > 0) {
      snprintf(id, id_cap, "%s",
               res.rows[0].artifact_id ? res.rows[0].artifact_id : "");
    }
  } else if (path != NULL && path_cap > 0) {
    path[0] = '\0';
  }
  kbc_arena_free(a);
  return res.len;
}

static int64_t store_count(const kbc_config *cfg) {
  kbc_err err;
  kbc_err_reset(&err);
  kbc_store *s = kbc_store_open(cfg, &err);
  if (s == NULL) {
    fprintf(stderr, "  store_open: %s\n", err.msg);
    return -1;
  }
  int64_t n = -1;
  if (kbc_failed(kbc_store_count_artifacts(s, NULL, &n, &err))) {
    fprintf(stderr, "  store_count: %s\n", err.msg);
  }
  kbc_store_close(s);
  return n;
}

/* ------------------------------------------------------------------ cases -- */

KBC_TEST(ingest_indexes_every_document) {
  fixture f;
  fx_setup(&f, false);
  if (f.app == NULL) {
    fx_teardown(&f);
    return;
  }

  kbc_err err;
  kbc_err_reset(&err);
  KBC_CHECK_OK(kbc_app_reindex(f.app, &err));

  kbc_app_stats st;
  KBC_CHECK_OK(kbc_app_stats_get(f.app, &st, &err));
  KBC_CHECK_EQ_INT(st.index_docs, 3);
  KBC_CHECK_EQ_INT(st.artifacts_indexed, 3);
  KBC_CHECK_EQ_INT(st.last_reindex_docs, 3);
  KBC_CHECK_EQ_INT(st.reindex_runs, 1);
  /* Three documents of prose cannot produce an empty term table; zero here
   * would mean the walk found files but the build pass indexed nothing. */
  KBC_CHECK_MSG(st.index_terms > 0, "index has no terms after a reindex");
  KBC_CHECK_EQ_INT(store_count(f.cfg), 3);

  fx_teardown(&f);
}

KBC_TEST(search_returns_the_document_the_query_names) {
  fixture f;
  fx_setup(&f, false);
  if (f.app == NULL) {
    fx_teardown(&f);
    return;
  }
  kbc_err err;
  kbc_err_reset(&err);
  KBC_CHECK_OK(kbc_app_reindex(f.app, &err));

  char path[64], title[64], id[32];
  size_t n = first_hit_path(f.app, "quixotic", NULL, path, sizeof path, title,
                            sizeof title, id, sizeof id, NULL);
  KBC_CHECK_EQ_INT(n, 1);
  KBC_CHECK_EQ_STR(path, "b.md");
  KBC_CHECK_EQ_STR(title, "Beta Chronicle");
  KBC_CHECK_MSG(kbc_id_is_valid(id), "search row carries no artifact id: \"%s\"",
                id);

  /* A word from a different file must not return b.md at all. */
  n = first_hit_path(f.app, "verdigris", NULL, path, sizeof path, title,
                     sizeof title, id, sizeof id, NULL);
  KBC_CHECK_EQ_INT(n, 1);
  KBC_CHECK_EQ_STR(path, "c.md");

  /* The corpus filter is honoured, not ignored: a term that lives only in the
   * other corpus returns nothing. */
  n = first_hit_path(f.app, "verdigris", CORPUS_B, path, sizeof path, title,
                     sizeof title, id, sizeof id, NULL);
  KBC_CHECK_EQ_INT(n, 0);

  fx_teardown(&f);
}

KBC_TEST(unchanged_files_are_skipped_by_mtime_and_size) {
  fixture f;
  fx_setup(&f, false);
  if (f.app == NULL) {
    fx_teardown(&f);
    return;
  }
  kbc_err err;
  kbc_err_reset(&err);
  KBC_CHECK_OK(kbc_app_reindex(f.app, &err));

  kbc_app_stats st;
  KBC_CHECK_OK(kbc_app_stats_get(f.app, &st, &err));
  KBC_CHECK_EQ_INT(st.reindex_runs, 1);
  const int64_t terms_before = st.index_terms;

  /* Second run over an untouched corpus: same documents, and one more run. */
  KBC_CHECK_OK(kbc_app_reindex(f.app, &err));
  KBC_CHECK_OK(kbc_app_stats_get(f.app, &st, &err));
  KBC_CHECK_EQ_INT(st.reindex_runs, 2);
  KBC_CHECK_EQ_INT(st.index_docs, 3);
  /* A no-op rebuild must reproduce the same term table, not grow one. */
  KBC_CHECK_EQ_INT(st.index_terms, terms_before);
  KBC_CHECK_EQ_INT(store_count(f.cfg), 3);

  /* Now the real fast-path probe: change the bytes of b.md, keeping its size
   * and putting its mtime back. Only a skip can keep zephyrus unfindable —
   * a content hash, or a blind re-read, would pick it up. */
  char p[KBC_TEST_PATH_MAX];
  join(p, sizeof p, f.corpus_a, "b.md");
  struct stat sb;
  KBC_CHECK_MSG(stat(p, &sb) == 0, "stat %s: %s", p, strerror(errno));
  kbc_test_write_file(p, DOC_B_TAMPERED);
  struct timespec times[2];
  times[0] = sb.st_atim;
  times[1] = sb.st_mtim;
  KBC_CHECK_MSG(utimensat(AT_FDCWD, p, times, 0) == 0, "utimensat %s: %s", p,
                strerror(errno));

  KBC_CHECK_OK(kbc_app_reindex(f.app, &err));
  KBC_CHECK_OK(kbc_app_stats_get(f.app, &st, &err));
  KBC_CHECK_EQ_INT(st.reindex_runs, 3);
  KBC_CHECK_EQ_INT(st.index_docs, 3);

  char path[64], title[64], id[32];
  size_t n = first_hit_path(f.app, "zephyrus", NULL, path, sizeof path, title,
                            sizeof title, id, sizeof id, NULL);
  KBC_CHECK_MSG(n == 0,
                "a file unchanged in (mtime, size) was re-read: its tampered "
                "contents reached the index (app.c walk_dir promises \"no read "
                "of its bytes\")");
  n = first_hit_path(f.app, "quixotic", NULL, path, sizeof path, title,
                     sizeof title, id, sizeof id, NULL);
  KBC_CHECK_EQ_INT(n, 1);
  KBC_CHECK_EQ_STR(path, "b.md");

  fx_teardown(&f);
}

KBC_TEST(rewriting_a_file_indexes_the_new_words) {
  fixture f;
  fx_setup(&f, false);
  if (f.app == NULL) {
    fx_teardown(&f);
    return;
  }
  kbc_err err;
  kbc_err_reset(&err);
  KBC_CHECK_OK(kbc_app_reindex(f.app, &err));

  char p[KBC_TEST_PATH_MAX];
  join(p, sizeof p, f.corpus_a, "b.md");
  kbc_test_write_file(p, "# Beta Chronicle\n\nThe chronicle mentions halcyon "
                         "ridgeways.\n");
  /* Different length, so (mtime, size) fails and the file is re-ingested even
   * if the mtime granularity were coarse. */
  KBC_CHECK_OK(kbc_app_reindex(f.app, &err));

  char path[64], title[64], id[32];
  size_t n = first_hit_path(f.app, "halcyon", NULL, path, sizeof path, title,
                            sizeof title, id, sizeof id, NULL);
  KBC_CHECK_EQ_INT(n, 1);
  KBC_CHECK_EQ_STR(path, "b.md");
  KBC_CHECK_EQ_STR(title, "Beta Chronicle");

  /* The rewritten file's old vocabulary is gone from the index, and the
   * untouched files kept theirs. */
  n = first_hit_path(f.app, "quixotic", NULL, path, sizeof path, title,
                     sizeof title, id, sizeof id, NULL);
  KBC_CHECK_MSG(n == 0, "a term from the previous revision is still indexed");
  n = first_hit_path(f.app, "verdigris", NULL, path, sizeof path, title,
                     sizeof title, id, sizeof id, NULL);
  KBC_CHECK_EQ_INT(n, 1);
  KBC_CHECK_EQ_STR(path, "c.md");
  n = first_hit_path(f.app, "accruals", NULL, path, sizeof path, title,
                     sizeof title, id, sizeof id, NULL);
  KBC_CHECK_EQ_INT(n, 1);
  KBC_CHECK_EQ_STR(path, "a.md");

  kbc_app_stats st;
  KBC_CHECK_OK(kbc_app_stats_get(f.app, &st, &err));
  KBC_CHECK_EQ_INT(st.index_docs, 3);
  /* One artifact per path: a rewrite must upsert, not accumulate rows. */
  KBC_CHECK_EQ_INT(store_count(f.cfg), 3);

  fx_teardown(&f);
}

KBC_TEST(deleting_a_file_removes_it_from_index_and_store) {
  fixture f;
  fx_setup(&f, false);
  if (f.app == NULL) {
    fx_teardown(&f);
    return;
  }
  kbc_err err;
  kbc_err_reset(&err);
  KBC_CHECK_OK(kbc_app_reindex(f.app, &err));
  KBC_CHECK_EQ_INT(store_count(f.cfg), 3);

  char p[KBC_TEST_PATH_MAX];
  join(p, sizeof p, f.corpus_a, "c.md");
  KBC_CHECK_MSG(unlink(p) == 0, "unlink %s: %s", p, strerror(errno));
  /* The path the daemon's watcher takes on a delete event. */
  KBC_CHECK_OK(kbc_app_reindex_remove(f.app, CORPUS_A, "c.md", &err));

  char path[64], title[64], id[32];
  size_t n = first_hit_path(f.app, "verdigris", NULL, path, sizeof path, title,
                            sizeof title, id, sizeof id, NULL);
  KBC_CHECK_MSG(n == 0, "a deleted document is still searchable");
  n = first_hit_path(f.app, "quixotic", NULL, path, sizeof path, title,
                     sizeof title, id, sizeof id, NULL);
  KBC_CHECK_EQ_INT(n, 1);
  KBC_CHECK_EQ_STR(path, "b.md");

  kbc_app_stats st;
  KBC_CHECK_OK(kbc_app_stats_get(f.app, &st, &err));
  KBC_CHECK_EQ_INT(st.index_docs, 2);
  KBC_CHECK_EQ_INT(store_count(f.cfg), 2);

  fx_teardown(&f);
}

KBC_TEST(get_artifact_round_trips_and_rejects_unknown_ids) {
  fixture f;
  fx_setup(&f, false);
  if (f.app == NULL) {
    fx_teardown(&f);
    return;
  }
  kbc_err err;
  kbc_err_reset(&err);
  KBC_CHECK_OK(kbc_app_reindex(f.app, &err));

  char id[32];
  first_hit_path(f.app, "quixotic", NULL, NULL, 0, NULL, 0, id, sizeof id, NULL);
  KBC_CHECK_MSG(kbc_id_is_valid(id), "no id to look up: \"%s\"", id);

  kbc_arena *a = kbc_arena_new(64u * 1024u);
  KBC_CHECK_NOT_NULL(a);
  if (a != NULL) {
    kbc_artifact art;
    memset(&art, 0, sizeof art);
    KBC_CHECK_OK(kbc_app_get_artifact(f.app, a, id, true, &art, &err));
    KBC_CHECK_EQ_STR(art.id, id);
    KBC_CHECK_EQ_STR(art.corpus, CORPUS_A);
    KBC_CHECK_EQ_STR(art.path, "b.md");
    KBC_CHECK_EQ_STR(art.title, "Beta Chronicle");
    KBC_CHECK_EQ_INT(art.kind, KBC_KIND_ARTIFACT);
    KBC_CHECK_MSG(art.size_bytes == (int64_t)strlen(DOC_B),
                  "size_bytes: got %lld, want %zu", (long long)art.size_bytes,
                  strlen(DOC_B));
    KBC_CHECK_MSG(art.source != NULL && strstr(art.source, "quixotic"),
                  "with_source=true did not return the raw text");
    /* The summary is the first non-heading block, so it exists for any
     * document with a paragraph after its heading. */
    KBC_CHECK_MSG(art.summary != NULL && art.summary[0] != '\0',
                  "summary is empty: parse produced no non-heading block");

    /* A well-formed id that was never minted is a miss, not an error. */
    KBC_CHECK_ERR(kbc_app_get_artifact(f.app, a, "000000000000", true, &art, &err),
                  KBC_ERR_NOTFOUND);
    KBC_CHECK_ERR_MSG(err);
    /* A malformed id is rejected before it reaches the store. */
    KBC_CHECK_ERR(kbc_app_get_artifact(f.app, a, "not-an-id", true, &art, &err),
                  KBC_ERR_INVALID);
    KBC_CHECK_ERR_MSG(err);
    kbc_arena_free(a);
  }

  fx_teardown(&f);
}

KBC_TEST(list_artifacts_filters_by_corpus_and_honours_limit) {
  fixture f;
  fx_setup(&f, true);
  if (f.app == NULL) {
    fx_teardown(&f);
    return;
  }
  kbc_err err;
  kbc_err_reset(&err);
  KBC_CHECK_OK(kbc_app_reindex(f.app, &err));

  kbc_arena *a = kbc_arena_new(256u * 1024u);
  KBC_CHECK_NOT_NULL(a);
  if (a == NULL) {
    fx_teardown(&f);
    return;
  }
  kbc_artifact *rows = NULL;
  size_t n = 0;

  KBC_CHECK_OK(kbc_app_list_artifacts(f.app, a, NULL, KBC_KIND__COUNT, 100, 0,
                                      &rows, &n, &err));
  KBC_CHECK_EQ_INT(n, 4);
  for (size_t i = 0; i < n; i++) {
    bool in_b = rows[i].corpus != NULL && strcmp(rows[i].corpus, CORPUS_B) == 0;
    KBC_CHECK_MSG(in_b || strcmp(rows[i].corpus, CORPUS_A) == 0,
                  "row %zu is from corpus \"%s\"", i, rows[i].corpus);
  }

  rows = NULL;
  n = 0;
  KBC_CHECK_OK(kbc_app_list_artifacts(f.app, a, CORPUS_A, KBC_KIND__COUNT, 100,
                                      0, &rows, &n, &err));
  KBC_CHECK_EQ_INT(n, 3);
  for (size_t i = 0; i < n; i++) {
    KBC_CHECK_EQ_STR(rows[i].corpus, CORPUS_A);
  }

  /* A corpus with nothing in it is an empty page, not an error. */
  rows = NULL;
  n = 0;
  KBC_CHECK_OK(kbc_app_list_artifacts(f.app, a, "nosuch", KBC_KIND__COUNT, 10,
                                      0, &rows, &n, &err));
  KBC_CHECK_EQ_INT(n, 0);
  KBC_CHECK_NULL(rows);

  /* limit and offset page the same corpus without repeating or dropping a row:
   * the union of two pages of 2 is the full set of 3, disjointly. */
  kbc_artifact *p0 = NULL;
  size_t n0 = 0;
  KBC_CHECK_OK(kbc_app_list_artifacts(f.app, a, CORPUS_A, KBC_KIND__COUNT, 2, 0,
                                      &p0, &n0, &err));
  KBC_CHECK_EQ_INT(n0, 2);
  kbc_artifact *p1 = NULL;
  size_t n1 = 0;
  KBC_CHECK_OK(kbc_app_list_artifacts(f.app, a, CORPUS_A, KBC_KIND__COUNT, 2, 2,
                                      &p1, &n1, &err));
  KBC_CHECK_EQ_INT(n1, 1);
  bool overlap = false;
  for (size_t i = 0; i < n0; i++) {
    for (size_t j = 0; j < n1; j++) {
      if (strcmp(p0[i].id, p1[j].id) == 0) overlap = true;
    }
  }
  KBC_CHECK_MSG(!overlap, "offset 2 re-returned a row from offset 0");

  /* An offset past the end is an empty page. */
  rows = NULL;
  n = 0;
  KBC_CHECK_OK(kbc_app_list_artifacts(f.app, a, CORPUS_A, KBC_KIND__COUNT, 2, 99,
                                      &rows, &n, &err));
  KBC_CHECK_EQ_INT(n, 0);

  kbc_arena_free(a);
  fx_teardown(&f);
}

KBC_TEST(the_index_survives_a_restart_without_reindexing) {
  fixture f;
  fx_setup(&f, false);
  if (f.app == NULL) {
    fx_teardown(&f);
    return;
  }
  kbc_err err;
  kbc_err_reset(&err);
  KBC_CHECK_OK(kbc_app_reindex(f.app, &err));
  kbc_app_stats before;
  KBC_CHECK_OK(kbc_app_stats_get(f.app, &before, &err));
  KBC_CHECK_EQ_INT(before.index_docs, 3);
  kbc_app_close(f.app);
  f.app = NULL;

  kbc_app *app2 = kbc_app_open(f.cfg, &err);
  if (app2 == NULL) fprintf(stderr, "  reopen: %s\n", err.msg);
  KBC_CHECK_NOT_NULL(app2);
  if (app2 == NULL) {
    fx_teardown(&f);
    return;
  }
  kbc_app_stats st;
  kbc_err_reset(&err);
  KBC_CHECK_OK(kbc_app_stats_get(app2, &st, &err));
  KBC_CHECK_EQ_INT(st.index_docs, 3);
  KBC_CHECK_EQ_INT(st.index_terms, before.index_terms);
  /* A fresh process has run no reindexes; the counts must say so, or the
   * persisted index is not what is being read. */
  KBC_CHECK_EQ_INT(st.reindex_runs, 0);

  char path[64], title[64], id[32];
  size_t n = first_hit_path(app2, "quixotic", NULL, path, sizeof path, title,
                            sizeof title, id, sizeof id, NULL);
  KBC_CHECK_EQ_INT(n, 1);
  KBC_CHECK_EQ_STR(path, "b.md");
  n = first_hit_path(app2, "verdigris", NULL, path, sizeof path, title,
                     sizeof title, id, sizeof id, NULL);
  KBC_CHECK_EQ_INT(n, 1);
  KBC_CHECK_EQ_STR(path, "c.md");

  kbc_app_close(app2);
  fx_teardown(&f);
}

KBC_TEST(two_apps_over_one_data_dir_do_not_corrupt_each_other) {
  fixture f;
  fx_setup(&f, false);
  if (f.app == NULL) {
    fx_teardown(&f);
    return;
  }
  kbc_err err;
  kbc_err_reset(&err);
  KBC_CHECK_OK(kbc_app_reindex(f.app, &err));

  kbc_app *second = kbc_app_open(f.cfg, &err);
  if (second == NULL) fprintf(stderr, "  second open: %s\n", err.msg);
  KBC_CHECK_NOT_NULL(second);
  if (second == NULL) {
    fx_teardown(&f);
    return;
  }

  /* `second` was opened after the first reindex, so it serves the on-disk
   * index; after `f.app` reindexes, the two disagree by construction. Reading
   * through the older handle must still be coherent. */
  char path[64], title[64], id[32];
  size_t n = first_hit_path(second, "quixotic", NULL, path, sizeof path, title,
                            sizeof title, id, sizeof id, NULL);
  KBC_CHECK_EQ_INT(n, 1);
  KBC_CHECK_EQ_STR(path, "b.md");

  char p[KBC_TEST_PATH_MAX];
  join(p, sizeof p, f.corpus_a, "c.md");
  kbc_test_write_file(p, "# Gamma Digest\n\nThe digest catalogues lapis.\n");
  KBC_CHECK_OK(kbc_app_reindex(f.app, &err));

  n = first_hit_path(f.app, "lapis", NULL, path, sizeof path, title, sizeof title,
                     id, sizeof id, NULL);
  KBC_CHECK_EQ_INT(n, 1);
  KBC_CHECK_EQ_STR(path, "c.md");
  kbc_app_stats st;
  KBC_CHECK_OK(kbc_app_stats_get(f.app, &st, &err));
  KBC_CHECK_EQ_INT(st.index_docs, 3);

  /* A third reader opened now sees the newest generation and the whole store. */
  kbc_app *third = kbc_app_open(f.cfg, &err);
  KBC_CHECK_NOT_NULL(third);
  if (third != NULL) {
    n = first_hit_path(third, "lapis", NULL, path, sizeof path, title,
                       sizeof title, id, sizeof id, NULL);
    KBC_CHECK_EQ_INT(n, 1);
    KBC_CHECK_EQ_STR(path, "c.md");
    kbc_app_close(third);
  }
  KBC_CHECK_EQ_INT(store_count(f.cfg), 3);

  kbc_app_close(second);
  fx_teardown(&f);
}

KBC_TEST(a_corrupt_index_is_refused_not_ignored) {
  fixture f;
  fx_setup(&f, false);
  if (f.app == NULL) {
    fx_teardown(&f);
    return;
  }
  kbc_err err;
  kbc_err_reset(&err);
  KBC_CHECK_OK(kbc_app_reindex(f.app, &err));
  kbc_app_close(f.app);
  f.app = NULL;

  kbc_test_write_file(f.cfg->index_path, "not an index at all, just bytes");

  kbc_err err2;
  kbc_err_reset(&err2);
  kbc_app *bad = kbc_app_open(f.cfg, &err2);
  KBC_CHECK_NULL(bad);
  KBC_CHECK_EQ_INT(err2.status, KBC_ERR_PARSE);
  KBC_CHECK_ERR_MSG(err2);
  /* The refusal must name the file, or the operator cannot act on it. */
  KBC_CHECK_MSG(strstr(err2.msg, f.cfg->index_path) != NULL,
                "error message does not name the index path: \"%s\"", err2.msg);

  fx_teardown(&f);
}

KBC_TEST(hybrid_search_degrades_to_keyword_without_an_embedder) {
  fixture f;
  fx_setup(&f, false);
  if (f.app == NULL) {
    fx_teardown(&f);
    return;
  }
  kbc_err err;
  kbc_err_reset(&err);
  KBC_CHECK_OK(kbc_app_reindex(f.app, &err));

  /* No embedder_cmd: the product must still answer, and say so. */
  char path[64], title[64], id[32];
  search_flags flags;
  memset(&flags, 0, sizeof flags);
  size_t n = first_hit_path(f.app, "quixotic", NULL, path, sizeof path, title,
                            sizeof title, id, sizeof id, &flags);
  KBC_CHECK_EQ_INT(n, 1);
  KBC_CHECK_EQ_STR(path, "b.md");
  KBC_CHECK_MSG(flags.degraded,
                "a keyword-only search was not marked degraded");
  KBC_CHECK_MSG(!flags.vector_ran, "the vector lane ran with no embedder");
  KBC_CHECK_MSG(flags.top_keyword_score > 0.0,
                "keyword lane scored nothing");

  kbc_app_stats st;
  KBC_CHECK_OK(kbc_app_stats_get(f.app, &st, &err));
  KBC_CHECK_EQ_INT(st.searches_served, 1);
  KBC_CHECK_EQ_INT(st.searches_degraded, 1);

  /* And the other two modes survive the missing model the same way. */
  kbc_arena *a = kbc_arena_new(64u * 1024u);
  KBC_CHECK_NOT_NULL(a);
  if (a != NULL) {
    for (int mode = KBC_MODE_KEYWORD; mode <= KBC_MODE_SEMANTIC; mode++) {
      kbc_query q;
      memset(&q, 0, sizeof q);
      q.q = "quixotic";
      q.kind = KBC_KIND__COUNT;
      q.mode = (kbc_search_mode)mode;
      q.limit = 10;
      kbc_search_result r;
      memset(&r, 0, sizeof r);
      kbc_err le;
      kbc_err_reset(&le);
      KBC_CHECK_MSG(kbc_app_search(f.app, a, &q, &r, &le) == KBC_OK,
                    "mode %s failed: %s", kbc_search_mode_str((kbc_search_mode)mode),
                    le.msg);
      KBC_CHECK_MSG(r.degraded, "mode %s did not report degradation",
                    kbc_search_mode_str((kbc_search_mode)mode));
      if ((kbc_search_mode)mode == KBC_MODE_SEMANTIC) {
        /* No embedder means no vector lane, and a semantic search that invents
         * results would be worse than an empty answer. */
        KBC_CHECK_EQ_INT(r.len, 0);
      } else {
        KBC_CHECK_EQ_INT(r.len, 1);
        KBC_CHECK_EQ_STR(r.rows[0].path, "b.md");
      }
    }
    kbc_arena_free(a);
  }

  fx_teardown(&f);
}

int main(void) {
  static const kbc_test_case cases[] = {
      {"ingest_indexes_every_document", ingest_indexes_every_document},
      {"search_returns_the_document_the_query_names",
       search_returns_the_document_the_query_names},
      {"unchanged_files_are_skipped_by_mtime_and_size",
       unchanged_files_are_skipped_by_mtime_and_size},
      {"rewriting_a_file_indexes_the_new_words",
       rewriting_a_file_indexes_the_new_words},
      {"deleting_a_file_removes_it_from_index_and_store",
       deleting_a_file_removes_it_from_index_and_store},
      {"get_artifact_round_trips_and_rejects_unknown_ids",
       get_artifact_round_trips_and_rejects_unknown_ids},
      {"list_artifacts_filters_by_corpus_and_honours_limit",
       list_artifacts_filters_by_corpus_and_honours_limit},
      {"the_index_survives_a_restart_without_reindexing",
       the_index_survives_a_restart_without_reindexing},
      {"two_apps_over_one_data_dir_do_not_corrupt_each_other",
       two_apps_over_one_data_dir_do_not_corrupt_each_other},
      {"a_corrupt_index_is_refused_not_ignored",
       a_corrupt_index_is_refused_not_ignored},
      {"hybrid_search_degrades_to_keyword_without_an_embedder",
       hybrid_search_degrades_to_keyword_without_an_embedder},
      {NULL, NULL},
  };
  return kbc_test_run("app", cases);
}
