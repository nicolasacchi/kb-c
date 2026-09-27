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

/* The app keeps a private copy of the config from the moment it opens, so
 * changing a config value means closing and reopening — the same thing an
 * operator does after editing kb.toml. Reindexes, so the corpus on disk is
 * the corpus the ranking sees. */
static void reopen_with_graph_boost(fixture *f, double weight) {
  kbc_err err;
  kbc_app_close(f->app);
  f->app = NULL;
  f->cfg->graph_boost = weight;
  kbc_err_reset(&err);
  f->app = kbc_app_open(f->cfg, &err);
  if (f->app == NULL) fprintf(stderr, "  app_open: %s\n", err.msg);
  KBC_CHECK_NOT_NULL(f->app);
  if (f->app != NULL) KBC_CHECK_OK(kbc_app_reindex(f->app, &err));
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

/* A search resolves every hit it returns to a stored document, and the
 * resolver reads the row WITHOUT its `source` column — a batch lookup, one
 * locked round trip, the same rows the per-row function would return. So the
 * ids, titles and summaries a client sees must be exactly the store's, or the
 * API changes shape with nothing to notice. */
KBC_TEST(search_rows_carry_the_stored_ids_titles_and_summaries) {
  fixture f;
  fx_setup(&f, false);
  if (f.app == NULL) {
    fx_teardown(&f);
    return;
  }
  kbc_err err;
  kbc_err_reset(&err);
  KBC_CHECK_OK(kbc_app_reindex(f.app, &err));

  kbc_store *st = kbc_store_open(f.cfg, &err);
  KBC_CHECK_MSG(st != NULL, "store_open: %s", err.msg);
  if (st == NULL) {
    fx_teardown(&f);
    return;
  }

  kbc_arena *a = kbc_arena_new(64u * 1024u);
  KBC_CHECK_NOT_NULL(a);
  /* The three terms name the three documents, so this is a real batch and
   * not the single-hit case that hides a mis-indexed resolve. */
  kbc_query q;
  memset(&q, 0, sizeof q);
  q.q = "quixotic verdigris accruals";
  q.kind = KBC_KIND__COUNT;
  q.mode = KBC_MODE_KEYWORD;
  q.limit = 10;
  kbc_search_result res;
  memset(&res, 0, sizeof res);
  KBC_CHECK_OK(kbc_app_search(f.app, a, &q, &res, &err));
  KBC_CHECK_EQ_INT(res.len, 3);

  for (size_t i = 0; i < res.len; i++) {
    const kbc_result_row *row = &res.rows[i];
    KBC_CHECK_MSG(row->artifact_id != NULL, "row %zu resolved no id", i);
    KBC_CHECK_MSG(kbc_id_is_valid(row->artifact_id),
                  "row %zu carries an invalid id \"%s\"", i,
                  row->artifact_id ? row->artifact_id : "");
    if (row->artifact_id == NULL) continue;

    kbc_arena *one = kbc_arena_new(0);
    kbc_artifact want;
    KBC_CHECK_OK(
        kbc_store_get_artifact(st, one, row->artifact_id, true, &want, &err));
    KBC_CHECK_MSG(strcmp(row->title, want.title) == 0,
                  "row %zu title \"%s\" != stored \"%s\"", i, row->title,
                  want.title);
    KBC_CHECK_MSG(want.summary != NULL && row->summary != NULL &&
                      strcmp(row->summary, want.summary) == 0,
                  "row %zu summary \"%s\" != stored \"%s\"", i,
                  row->summary ? row->summary : "(null)",
                  want.summary ? want.summary : "(null)");
    KBC_CHECK_MSG(strcmp(row->path, want.path) == 0,
                  "row %zu path \"%s\" != stored \"%s\"", i, row->path,
                  want.path);
    /* The document is really there, source and all: the resolve reads less of
     * the row, not a different row. */
    KBC_CHECK_MSG(want.source != NULL && strstr(want.source, "The") != NULL,
                  "row %zu resolved a document with no stored source", i);
    kbc_arena_free(one);
  }
  kbc_arena_free(a);
  kbc_store_close(st);
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

/* Every row a search returns, by path, so a test can ask what ONE document
 * scores — not just which document came first. */
typedef struct {
  char path[64];
  double score;
} score_row;

static size_t scores_by_path(kbc_app *app, const char *q, score_row *out,
                             size_t cap) {
  kbc_arena *a = kbc_arena_new(64u * 1024u);
  KBC_CHECK_NOT_NULL(a);
  if (a == NULL) return 0;
  kbc_query query;
  memset(&query, 0, sizeof query);
  query.q = q;
  query.kind = KBC_KIND__COUNT;
  query.mode = KBC_MODE_KEYWORD;
  query.limit = 20;
  query.bm25_k1 = 1.2;
  query.bm25_b = 0.75;
  kbc_search_result res;
  memset(&res, 0, sizeof res);
  kbc_err err;
  kbc_err_reset(&err);
  kbc_status s = kbc_app_search(app, a, &query, &res, &err);
  KBC_CHECK_OK(s);
  size_t n = 0;
  for (size_t i = 0; s == KBC_OK && i < res.len && i < cap; i++) {
    snprintf(out[n].path, sizeof out[n].path, "%s",
             res.rows[i].path ? res.rows[i].path : "");
    out[n].score = res.rows[i].keyword_score;
    n++;
  }
  kbc_arena_free(a);
  return n;
}

/* The product guarantee the watcher depends on: saving ONE file updates that
 * file and leaves every other document's postings, and therefore its score,
 * exactly as they were. A full rebuild satisfies this too, so the assertion
 * that matters is the one that would catch a rebuild that quietly did NOT
 * cover the corpus — and the ones that would catch an incremental update that
 * moved a neighbour's postings. Run twice: over an index this process built,
 * and over one it opened from disk, which is the state a restarted daemon is
 * in when the first file is saved.
 *
 * The corpus here carries a term ("quorum") that all three untouched documents
 * share, so a query for it scores every one of them, and the scores can be
 * compared before and after. */
KBC_TEST(reindexing_one_file_leaves_the_other_documents_alone) {
  fixture f;
  fx_setup(&f, false);
  if (f.app == NULL) {
    fx_teardown(&f);
    return;
  }
  char p[KBC_TEST_PATH_MAX];
  join(p, sizeof p, f.corpus_a, "q1.md");
  kbc_test_write_file(p, "# Quorum One\n\nThe quorum sits.\n");
  join(p, sizeof p, f.corpus_a, "q2.md");
  kbc_test_write_file(p, "# Quorum Two\n\nThe quorum stands.\n");
  join(p, sizeof p, f.corpus_a, "q3.md");
  kbc_test_write_file(p, "# Quorum Three\n\nThe quorum waits.\n");
  join(p, sizeof p, f.corpus_a, "t.md");
  kbc_test_write_file(p, "# Touched\n\nThe quorum doc mentions beforeword.\n");

  kbc_err err;
  kbc_err_reset(&err);
  KBC_CHECK_OK(kbc_app_reindex(f.app, &err));

  score_row before[8], after[8];
  size_t nb = scores_by_path(f.app, "quorum", before, 8);
  KBC_CHECK_EQ_INT(nb, 4);
  KBC_CHECK_EQ_INT(scores_by_path(f.app, "beforeword", after, 8), 1);
  KBC_CHECK_EQ_STR(after[0].path, "t.md");

  /* The watcher's path: one file changed, nothing else. */
  join(p, sizeof p, f.corpus_a, "t.md");
  kbc_test_write_file(p, "# Touched\n\nThe quorum doc mentions afterword now.\n");
  KBC_CHECK_OK(kbc_app_reindex_file(f.app, CORPUS_A, "t.md", &err));

  size_t na = scores_by_path(f.app, "quorum", after, 8);
  KBC_CHECK_EQ_INT(na, nb);
  for (size_t i = 0; i < na && i < nb; i++) {
    KBC_CHECK_MSG(strcmp(after[i].path, before[i].path) == 0,
                  "row %zu is %s, it was %s", i, after[i].path, before[i].path);
    /* Exactly equal, not close: a neighbour's posting that moved would change
     * its tf or its length norm, and either shows up here. */
    KBC_CHECK_MSG(after[i].score == before[i].score,
                  "%s scored %.17g after touching t.md, it scored %.17g before",
                  after[i].path, after[i].score, before[i].score);
  }
  /* And the touched document is the only one whose keywords changed. */
  KBC_CHECK_EQ_INT(scores_by_path(f.app, "beforeword", after, 8), 0);
  KBC_CHECK_EQ_INT(scores_by_path(f.app, "afterword", after, 8), 1);
  KBC_CHECK_EQ_STR(after[0].path, "t.md");
  kbc_app_stats st;
  KBC_CHECK_OK(kbc_app_stats_get(f.app, &st, &err));
  KBC_CHECK_EQ_INT(st.index_docs, 7);
  KBC_CHECK_EQ_INT(store_count(f.cfg), 7);

  /* Now the same thing over an index this process did not build: reopen, and
   * the live index is whatever kbc_index_open produced. */
  kbc_app_close(f.app);
  f.app = NULL;
  f.app = kbc_app_open(f.cfg, &err);
  KBC_CHECK_NOT_NULL(f.app);
  if (f.app == NULL) {
    fx_teardown(&f);
    return;
  }
  nb = scores_by_path(f.app, "quorum", before, 8);
  KBC_CHECK_EQ_INT(nb, 4);
  /* The replacement body has the same TOKEN COUNT as the one it replaces, and
   * that is load-bearing: BM25's length norm divides by the corpus average, so
   * an edit that changes a document's token count moves every other document's
   * score too, correctly. Holding the count still is what isolates "the other
   * documents' postings did not move" from "the average moved". ("now" is a
   * stopword, so it contributes nothing either way.) */
  join(p, sizeof p, f.corpus_a, "t.md");
  kbc_test_write_file(p, "# Touched\n\nThe quorum doc mentions thirdword now.\n");
  KBC_CHECK_OK(kbc_app_reindex_file(f.app, CORPUS_A, "t.md", &err));
  na = scores_by_path(f.app, "quorum", after, 8);
  KBC_CHECK_EQ_INT(na, nb);
  for (size_t i = 0; i < na && i < nb; i++) {
    KBC_CHECK_MSG(strcmp(after[i].path, before[i].path) == 0,
                  "after restart, row %zu is %s, it was %s", i, after[i].path,
                  before[i].path);
    KBC_CHECK_MSG(after[i].score == before[i].score,
                  "after restart, %s scored %.17g, it scored %.17g before the "
 "file was touched",
                  after[i].path, after[i].score, before[i].score);
  }
  KBC_CHECK_EQ_INT(scores_by_path(f.app, "afterword", after, 8), 0);
  KBC_CHECK_EQ_INT(scores_by_path(f.app, "thirdword", after, 8), 1);

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

/* Everything above calls kbc_app_reindex*_file/_remove directly, which is why
 * a watcher that published an ABSOLUTE path could pass all of it: the daemon's
 * own path is inotify -> event -> reindex, and this is the first case in the
 * suite that drives it. */

static const char DOC_D[] = "# Delta Dispatch\n\nThe dispatch lists wombat "
                            "pallets.\n";
static const char DOC_D_EDITED[] = "# Delta Dispatch\n\nThe dispatch now lists "
                                   "quokka crates instead.\n";

/* Generous: inotify delivery is asynchronous and the watcher debounces before
 * it publishes, so a tight deadline would teach the next reader to re-run
 * rather than trust this. */
#define LIVE_WAIT_MS 10000

static long now_ms(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (long)ts.tv_sec * 1000L + ts.tv_nsec / 1000000L;
}

static void nap_ms(long ms) {
  struct timespec ts;
  ts.tv_sec = ms / 1000;
  ts.tv_nsec = (ms % 1000) * 1000000L;
  (void)nanosleep(&ts, NULL);
}

/* Polls until the index holds `want` documents; returns what it holds when the
 * deadline passed, so the caller can assert either way. */
static int64_t wait_docs(kbc_app *app, int64_t want, long timeout_ms) {
  long deadline = now_ms() + timeout_ms;
  for (;;) {
    kbc_err err;
    kbc_err_reset(&err);
    kbc_app_stats st;
    if (!kbc_failed(kbc_app_stats_get(app, &st, &err)) && st.index_docs == want) {
      return want;
    }
    if (now_ms() >= deadline) {
      kbc_err_reset(&err);
      if (kbc_failed(kbc_app_stats_get(app, &st, &err))) return -1;
      return st.index_docs;
    }
    nap_ms(25);
  }
}

/* Polls until `q` returns (or stops returning) `want` rows. */
static size_t wait_hits(kbc_app *app, const char *q, size_t want,
                        long timeout_ms) {
  long deadline = now_ms() + timeout_ms;
  for (;;) {
    char path[64], title[64], id[32];
    size_t n =
        first_hit_path(app, q, NULL, path, sizeof path, title, sizeof title, id,
                       sizeof id, NULL);
    if (n == want || now_ms() >= deadline) return n;
    nap_ms(25);
  }
}

static int64_t wait_store_count(const kbc_config *cfg, int64_t want,
                                long timeout_ms) {
  long deadline = now_ms() + timeout_ms;
  for (;;) {
    int64_t n = store_count(cfg);
    if (n == want || now_ms() >= deadline) return n;
    nap_ms(25);
  }
}

/* The logger writes to stderr and has no sink hook, so "the edit was recorded
 * as a removal" is only observable by capturing the descriptor for the length
 * of the phase under test. */
typedef struct {
  int saved_fd;
  char path[KBC_TEST_PATH_MAX];
} log_capture;

static void log_capture_begin(log_capture *c) {
  kbc_test_tmpdir(c->path, sizeof c->path);
  char file[KBC_TEST_PATH_MAX];
  join(file, sizeof file, c->path, "log.txt");
  snprintf(c->path, sizeof c->path, "%s", file);
  c->saved_fd = dup(STDERR_FILENO);
  int fd = open(file, O_WRONLY | O_CREAT | O_TRUNC, 0600);
  if (fd < 0) {
    c->saved_fd = -1;
    return;
  }
  (void)dup2(fd, STDERR_FILENO);
  close(fd);
}

/* Restores stderr and returns the captured text; the caller frees it. */
static char *log_capture_end(log_capture *c) {
  fflush(stderr);
  if (c->saved_fd >= 0) {
    (void)dup2(c->saved_fd, STDERR_FILENO);
    close(c->saved_fd);
  }
  char *text = kbc_test_read_file(c->path);
  kbc_test_rmrf(c->path);
  return text;
}

KBC_TEST(a_save_in_a_running_daemon_updates_the_document) {
  fixture f;
  fx_setup(&f, false);
  if (f.app == NULL) {
    fx_teardown(&f);
    return;
  }
  kbc_err err;
  kbc_err_reset(&err);
  KBC_CHECK_OK(kbc_app_reindex(f.app, &err));
  KBC_CHECK_OK(kbc_app_start_watcher(f.app, &err));

  char p[KBC_TEST_PATH_MAX], path[64], title[64], id[32];
  join(p, sizeof p, f.corpus_a, "d.md");

  log_capture cap;
  log_capture_begin(&cap);

  /* A new file arrives. */
  kbc_test_write_file(p, DOC_D);
  KBC_CHECK_EQ_INT(wait_docs(f.app, 4, LIVE_WAIT_MS), 4);
  KBC_CHECK_EQ_INT(wait_hits(f.app, "wombat", 1, LIVE_WAIT_MS), 1);
  size_t n = first_hit_path(f.app, "wombat", NULL, path, sizeof path, title,
                            sizeof title, id, sizeof id, NULL);
  KBC_CHECK_EQ_STR(path, "d.md");
  KBC_CHECK_EQ_INT(wait_store_count(f.cfg, 4, LIVE_WAIT_MS), 4);

  /* Then it is saved over, in place — the most common thing a user does. */
  kbc_test_write_file(p, DOC_D_EDITED);
  KBC_CHECK_EQ_INT(wait_hits(f.app, "quokka", 1, LIVE_WAIT_MS), 1);
  n = first_hit_path(f.app, "quokka", NULL, path, sizeof path, title,
                     sizeof title, id, sizeof id, NULL);
  KBC_CHECK_EQ_STR(path, "d.md");
  n = first_hit_path(f.app, "wombat", NULL, path, sizeof path, title,
                    sizeof title, id, sizeof id, NULL);
  KBC_CHECK_MSG(n == 0, "the previous revision is still searchable after a save");
  KBC_CHECK_EQ_INT(wait_docs(f.app, 4, LIVE_WAIT_MS), 4);
  KBC_CHECK_EQ_INT(wait_store_count(f.cfg, 4, LIVE_WAIT_MS), 4);

  char *log_text = log_capture_end(&cap);
  KBC_CHECK_NOT_NULL(log_text);
  if (log_text != NULL) {
    /* The defect's own symptom: an edit logged as a removal of a document that
     * was never indexed under that key. */
    bool logged_removal = false;
    for (const char *line = log_text; line != NULL && *line != '\0';) {
      const char *eol = strchr(line, '\n');
      size_t len = eol != NULL ? (size_t)(eol - line) : strlen(line);
      char buf[512];
      if (len >= sizeof buf) len = sizeof buf - 1;
      memcpy(buf, line, len);
      buf[len] = '\0';
      if (strstr(buf, "d.md") != NULL && strstr(buf, "removed") != NULL) {
        logged_removal = true;
      }
      line = eol != NULL ? eol + 1 : NULL;
    }
    KBC_CHECK_MSG(!logged_removal,
                  "the daemon logged a removal for d.md during a save");
    free(log_text);
  }

  /* The removal path must still work: a fix that made edits work by ignoring
   * removals would get this far and no further. */
  KBC_CHECK_MSG(unlink(p) == 0, "unlink %s: %s", p, strerror(errno));
  KBC_CHECK_EQ_INT(wait_docs(f.app, 3, LIVE_WAIT_MS), 3);
  KBC_CHECK_EQ_INT(wait_store_count(f.cfg, 3, LIVE_WAIT_MS), 3);
  KBC_CHECK_EQ_INT(wait_hits(f.app, "quokka", 0, LIVE_WAIT_MS), 0);
  /* And the documents nobody touched are all still there. */
  KBC_CHECK_EQ_INT(wait_hits(f.app, "quixotic", 1, LIVE_WAIT_MS), 1);
  KBC_CHECK_EQ_INT(wait_hits(f.app, "accruals", 1, LIVE_WAIT_MS), 1);
  KBC_CHECK_EQ_INT(wait_hits(f.app, "verdigris", 1, LIVE_WAIT_MS), 1);

  kbc_app_stop_watcher(f.app);
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
      /* Degraded means a lane the CALLER asked for could not run. A keyword
       * query never asked for the vector lane, so losing it costs it
       * nothing and must not show up in /api/stats as a degraded search. */
      KBC_CHECK_MSG(r.degraded == ((kbc_search_mode)mode != KBC_MODE_KEYWORD),
                    "mode %s reported degraded=%d",
                    kbc_search_mode_str((kbc_search_mode)mode), r.degraded);
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


/* ----------------------------------------------------------------- since -- */

/* `since:` is a predicate on the store's mtime, so it is applied where the
 * mtime is: on the resolved rows, alongside the drop-on-unresolved. A filter
 * that excludes everything must return nothing — a filter that returned
 * everything would be indistinguishable from an ignored one. */
static size_t search_len(kbc_app *app, const char *q, const char *corpus,
                         kbc_status *st_out, kbc_err *err) {
  kbc_arena *a = kbc_arena_new(64u * 1024u);
  KBC_CHECK_NOT_NULL(a);
  if (a == NULL) return 0;
  kbc_query query;
  memset(&query, 0, sizeof query);
  query.q = q;
  query.corpus = corpus;
  query.kind = KBC_KIND__COUNT;
  query.mode = KBC_MODE_KEYWORD;
  query.limit = 10;
  kbc_search_result res;
  memset(&res, 0, sizeof res);
  kbc_err_reset(err);
  const kbc_status s = kbc_app_search(app, a, &query, &res, err);
  if (st_out != NULL) *st_out = s;
  const size_t n = res.len;
  kbc_arena_free(a);
  return n;
}

KBC_TEST(since_filters_on_the_store_mtime_and_an_impossible_since_returns_nothing) {
  fixture f;
  fx_setup(&f, false);
  kbc_err err;
  kbc_err_reset(&err);
  KBC_CHECK_OK(kbc_app_reindex(f.app, &err));

  /* "verdigris" is in c.md alone, so the row count is exactly this document's
   * fate and nothing else. */
  kbc_status st = KBC_OK;
  KBC_CHECK_EQ_INT(search_len(f.app, "verdigris", CORPUS_A, &st, &err), 1);
  KBC_CHECK_OK(st);

  /* A window that contains the document keeps it… */
  KBC_CHECK_EQ_INT(search_len(f.app, "verdigris since:7d", CORPUS_A, &st, &err), 1);
  KBC_CHECK_OK(st);
  /* …and one that starts after the corpus was written does not. A raw unix
   * timestamp is the other accepted form (query.rs:276). */
  KBC_CHECK_EQ_INT(search_len(f.app, "verdigris since:4102444800", CORPUS_A,
                              &st, &err),
                   0);
  KBC_CHECK_OK(st);
  /* since:all is the absence of a filter and the original refuses it. */
  (void)search_len(f.app, "verdigris since:all", CORPUS_A, &st, &err);
  KBC_CHECK(st != KBC_OK);
  KBC_CHECK(err.msg[0] != '\0');
  /* An unparsable unit is an error too, never a silently dropped filter. */
  (void)search_len(f.app, "verdigris since:7y", CORPUS_A, &st, &err);
  KBC_CHECK(st != KBC_OK);
  KBC_CHECK(err.msg[0] != '\0');
  /* …and the filter is not what dropped those rows: an errored query returns
   * no rows, which is the same shape as a working filter and must not be
   * mistaken for one. */

  fx_teardown(&f);
}

/* The link graph is written from the corpus's own links, and only for targets
 * that resolve: a document that links out records an edge, a document that is
 * linked to has an in-degree, and a link to a file that does not exist is not
 * an edge at all. */
KBC_TEST(reindex_records_only_edges_whose_target_is_an_indexed_document) {
  fixture f;
  fx_setup(&f, false);
  char p[KBC_TEST_PATH_MAX];
  join(p, sizeof p, f.corpus_a, "links.md");
  kbc_test_write_file(p,
                      "# Links\n\n"
                      "See [alpha](a.md) and [ghost](nope.md) and "
                      "[outside](../../etc/passwd) and [web](https://x.test/).\n");
  kbc_err err;
  kbc_err_reset(&err);
  KBC_CHECK_OK(kbc_app_reindex(f.app, &err));

  kbc_store *store = kbc_store_open(f.cfg, &err);
  KBC_CHECK_NOT_NULL(store);
  int64_t edges = kbc_store_edge_count(store, CORPUS_A, &err);
  /* exactly the one link that resolves */
  KBC_CHECK_EQ_INT(edges, 1);
  KBC_CHECK_EQ_INT(kbc_store_edge_count(store, CORPUS_B, &err), 0);

  /* The degree is asked for by PATH: the store cannot name a document by its
   * index doc id, so a.md is named the way the parser named it. */
  static const char *const want[] = {"a.md", "links.md"};
  uint32_t deg[2] = {9, 9};
  KBC_CHECK_OK(kbc_store_edge_degrees_for(store, CORPUS_A, want, 2, deg, &err));
  KBC_CHECK_EQ_INT(deg[0], 1); /* linked from links.md */
  KBC_CHECK_EQ_INT(deg[1], 0); /* links out, is linked to by nobody */

  /* Editing the document so the link disappears must not leave the edge
   * behind: replace, never append. */
  kbc_test_write_file(p, "# Links\n\nNo links any more.\n");
  KBC_CHECK_OK(kbc_app_reindex(f.app, &err));
  KBC_CHECK_EQ_INT(kbc_store_edge_count(store, CORPUS_A, &err), 0);

  kbc_store_close(store);
  fx_teardown(&f);
}

/* The graph boost is opt-in and its weight comes from the config. The hub is
 * deliberately a poor text match — long, with the query term once — so the
 * only thing that can promote it is in-degree: the ordering is then a real
 * reordering, not a coincidence. */
static void order_of_hits(kbc_app *app, const char *q, const char *corpus,
                          char *out, size_t cap) {
  kbc_arena *a = kbc_arena_new(64u * 1024u);
  size_t off = 0;
  KBC_CHECK_NOT_NULL(a);
  if (a == NULL) return;
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
  KBC_CHECK_OK(kbc_app_search(app, a, &query, &res, &err));
  out[0] = '\0';
  for (size_t i = 0; i < res.len; i++) {
    const char *p = res.rows[i].path;
    int n = snprintf(out + off, cap - off, "%s%s", i == 0 ? "" : ",",
                     p != NULL ? p : "?");
    if (n < 0 || (size_t)n >= cap - off) {
      KBC_CHECK_MSG(false, "ordering does not fit in %zu bytes", cap);
      break;
    }
    off += (size_t)n;
  }
  kbc_arena_free(a);
}

/* Membership in a comma-joined ordering, counted. The three linkers share an
 * in-degree, so they share a fused score and their relative order is decided by
 * the index doc id tie-break, which the corpus walk assigns in readdir order.
 * That differs between filesystems, so no test may pin their sequence — only
 * the set. */
static int order_count(const char *order, const char *name) {
  int n = 0;
  size_t len = strlen(name);
  for (const char *p = order; (p = strstr(p, name)) != NULL; p += len) {
    bool left = (p == order) || p[-1] == ',';
    bool right = p[len] == '\0' || p[len] == ',';
    if (left && right) n++;
  }
  return n;
}

/* The first entry of a comma-joined ordering, or "" when it is empty. */
static bool order_starts_with(const char *order, const char *name) {
  size_t len = strlen(name);
  return strncmp(order, name, len) == 0 &&
         (order[len] == '\0' || order[len] == ',');
}


KBC_TEST(the_graph_boost_is_off_by_default_and_reorders_when_configured) {
  fixture f;
  fx_setup(&f, false);
  char p[KBC_TEST_PATH_MAX];
  join(p, sizeof p, f.corpus_a, "hub.md");
  kbc_test_write_file(p,
                      "# Hub\n\n"
                      "Alabaster brimstone cinnabar dolomite eflin fudge "
                      "gamboge halite icicle jasper krypton lilac mullet "
                      "nimbus onyx peridot quartz rhodonite siderite topaz "
                      "uranium verdigris wurtzite xenon yttrium zoisite "
                      "amber beryl cordierite dravite euclase forsterite "
                      "grossular hackmanite iolite kaersutite leucite "
                      "mellite nepheline olivine pectolite rutile "
                      "spodumene tundrite vesuvianite wiluite.\n");
  /* Three documents linking to hub.md, so its in-degree is 3. */
  for (int i = 0; i < 3; i++) {
    char name[64];
    snprintf(name, sizeof name, "linker%d.md", i);
    char body[256];
    snprintf(body, sizeof body,
             "# Linker\n\nSee [hub](hub.md) and the verdigris ledger.\n");
    join(p, sizeof p, f.corpus_a, name);
    kbc_test_write_file(p, body);
  }
  kbc_err err;
  kbc_err_reset(&err);
  KBC_CHECK_OK(kbc_app_reindex(f.app, &err));

  /* No key in the config: the graph is not even fetched, so the ordering is
   * the one the text alone produces, and the hub is not first. */
  char off[KBC_TEST_PATH_MAX * 2];
  order_of_hits(f.app, "verdigris", CORPUS_A, off, sizeof off);
  KBC_CHECK_MSG(strncmp(off, "hub.md", 6) != 0,
                "with the graph off the hub wins on text: %s", off);
  /* ...and it is the same ordering every time in one process, so what follows
   * compares a rerun, not a coin flip. */
  char off2[KBC_TEST_PATH_MAX * 2];
  order_of_hits(f.app, "verdigris", CORPUS_A, off2, sizeof off2);
  KBC_CHECK_EQ_STR(off2, off);

  /* The same corpus, the same query, one config key added: the most-linked
   * document takes the top rank. The app takes its own copy of the config at
   * open, so this is a reopen — which is what an operator editing kb.toml
   * does. */
  reopen_with_graph_boost(&f, 2.0);
  char on[KBC_TEST_PATH_MAX * 2];
  order_of_hits(f.app, "verdigris", CORPUS_A, on, sizeof on);
  KBC_CHECK_MSG(order_starts_with(on, "hub.md"),
                "with the graph on the in-degree does not win: %s", on);
  KBC_CHECK_EQ_INT(order_count(on, "linker0.md"), 1);
  KBC_CHECK_EQ_INT(order_count(on, "linker1.md"), 1);
  KBC_CHECK_EQ_INT(order_count(on, "linker2.md"), 1);
  KBC_CHECK_EQ_INT(order_count(on, "c.md"), 1);

  /* The key removed: the off ranking returns, byte for byte. */
  reopen_with_graph_boost(&f, 0.0);
  char again[KBC_TEST_PATH_MAX * 2];
  order_of_hits(f.app, "verdigris", CORPUS_A, again, sizeof again);
  KBC_CHECK_EQ_STR(again, off);

  fx_teardown(&f);
}

int main(void) {
  static const kbc_test_case cases[] = {
      {"ingest_indexes_every_document", ingest_indexes_every_document},
      {"search_returns_the_document_the_query_names",
       search_returns_the_document_the_query_names},
      {"search_rows_carry_the_stored_ids_titles_and_summaries",
       search_rows_carry_the_stored_ids_titles_and_summaries},
      {"unchanged_files_are_skipped_by_mtime_and_size",
       unchanged_files_are_skipped_by_mtime_and_size},
      {"rewriting_a_file_indexes_the_new_words",
       rewriting_a_file_indexes_the_new_words},
      {"reindexing_one_file_leaves_the_other_documents_alone",
       reindexing_one_file_leaves_the_other_documents_alone},
      {"deleting_a_file_removes_it_from_index_and_store",
       deleting_a_file_removes_it_from_index_and_store},
      {"a_save_in_a_running_daemon_updates_the_document",
       a_save_in_a_running_daemon_updates_the_document},
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
      {"since_filters_on_the_store_mtime_and_an_impossible_since_returns_nothing",
       since_filters_on_the_store_mtime_and_an_impossible_since_returns_nothing},
      {"reindex_records_only_edges_whose_target_is_an_indexed_document",
       reindex_records_only_edges_whose_target_is_an_indexed_document},
      {"the_graph_boost_is_off_by_default_and_reorders_when_configured",
       the_graph_boost_is_off_by_default_and_reorders_when_configured},
      {NULL, NULL},
  };
  return kbc_test_run("app", cases);
}
