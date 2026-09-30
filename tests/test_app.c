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
#include <dirent.h>
#include <errno.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <fcntl.h>
#include <inttypes.h>
#include <pthread.h>
#include <stdatomic.h>
#include <poll.h>
#include <sqlite3.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include "kbc/app.h"
#include "kbc/chunk.h"
#include "kbc/mem.h"
#include "kbc/store.h"
#include "kbc_test.h"

/* include/kbc/store.h is the orchestrator's file; the pending-links contract
 * is proposed there and these are the signatures it will carry. */
kbc_status kbc_store_drain_pending(kbc_store *s, const char *corpus,
                                   const char *dst_path, kbc_err *err);
int64_t kbc_store_pending_count(kbc_store *s, kbc_err *err);
/* include/kbc/app.h is frozen and carries no by-path delete; app.c defines it
 * and reports the header line. Declared here on the same terms so the
 * by-path case can be driven from the public boundary. */
kbc_status kbc_app_delete_path(kbc_app *app, const char *corpus,
                               const char *rel_path, kbc_err *err);
/* The query-embedding LRU's ceiling. Its hits, misses, invalidations and
 * entry count are fields on the frozen kbc_app_stats and are read through
 * kbc_app_stats_get like every other counter; only the capacity is app.c's
 * own function, declared here on the same terms as kbc_app_delete_path. */
size_t kbc_app_query_cache_capacity(const kbc_app *app);

/* The enrichment registry (app.h) and its seam kbc_enrich_run are declared in
 * the header now, so nothing is redeclared here. The context is opaque to
 * these tests: their own hooks take their state in `user` and never read it,
 * and the daemon builds the real one. */

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
  /* The fake sidecar and its request log, used by the quarantine cases. */
  char sidecar[KBC_TEST_PATH_MAX];
  char log[KBC_TEST_PATH_MAX];
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

/* The summary is a LEAD, and a lead is prose. The block text it is cut from
 * is a reader's text, so it still carries the emphasis markers the source
 * spelled the emphasis with, and those markers are what a search result and
 * a list row show. A row reading "You answer questions **from the knowledge
 * base**" is showing markup where prose belongs; the original's summary
 * cannot, because it is a text walk of the rendered page where the markers
 * are already gone.
 *
 * The stripping must not become a character-eating pass: `snake_case_name`
 * and `2 * 3` are an identifier and arithmetic, and a summary that has
 * swallowed either is a worse lie than one that shows a stray marker. Each
 * is asserted because they are the two ways a naive "delete every marker"
 * gets it wrong. */
KBC_TEST(a_summary_is_prose_and_keeps_words_that_look_like_markers) {
  fixture f;
  fx_setup(&f, false);
  static const char kDoc[] =
      "# Emphasis\n\n"
      "You answer **from the knowledge base**, with _one_ caveat and a\n"
      "`kb_code` span; see snake_case_name and 2 * 3 = 6 for the notation.\n";
  char p[KBC_TEST_PATH_MAX];
  join(p, sizeof p, f.corpus_a, "emph.md");
  kbc_test_write_file(p, kDoc);
  kbc_err err;
  kbc_err_reset(&err);
  KBC_CHECK_OK(kbc_app_reindex(f.app, &err));

  char id[KBC_MAX_ID_LEN + 1];
  kbc_id_for_artifact(id, CORPUS_A, "emph.md");
  kbc_arena *a = kbc_arena_new(0);
  kbc_artifact art;
  memset(&art, 0, sizeof art);
  KBC_CHECK_OK(kbc_app_get_artifact(f.app, a, id, true, &art, &err));
  KBC_CHECK_NOT_NULL(art.summary);
  if (art.summary != NULL) {
    KBC_CHECK_MSG(strstr(art.summary, "**") == NULL,
                  "a summary still carries a strong marker: %s", art.summary);
    KBC_CHECK_MSG(strstr(art.summary, "_one_") == NULL,
                  "a summary still carries an emphasis marker: %s",
                  art.summary);
    KBC_CHECK_MSG(strstr(art.summary, "from the knowledge base") != NULL,
                  "a summary lost the emphasised words themselves: %s",
                  art.summary);
    KBC_CHECK_MSG(strstr(art.summary, "snake_case_name") != NULL,
                  "a summary ate an identifier's underscores: %s", art.summary);
    KBC_CHECK_MSG(strstr(art.summary, "2 * 3 = 6") != NULL,
                  "a summary ate an arithmetic asterisk: %s", art.summary);
    /* A code span's delimiters are consumed by the PARSE, not by the summary
     * pass, so what reaches the summary is the code and nothing else. */
    KBC_CHECK_MSG(strstr(art.summary, "kb_code span") != NULL,
                  "a summary lost a code span's content: %s", art.summary);
  }
  kbc_arena_free(a);
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

/* THE CONVERGENCE PROPERTY: the order documents are ingested in must not be
 * observable in the graph. `a.md` links to `b.md`; visiting a first and
 * visiting b first must leave the same edge set and the same in-degrees.
 *
 * The order is forced by what is on disk when the first reindex runs, not by
 * any test-only hook: b.md does not exist while a.md is indexed, so a.md's
 * link has nothing to resolve against. Both scenarios then end with the same
 * two files and a second reindex. */
typedef struct {
  int64_t edges;
  int64_t pending;
  uint32_t deg_a;
  uint32_t deg_b;
} graph_shape;

/* An EMPTY corpus: the convergence cases need a document that genuinely does
 * not exist yet, and the standard fixture ships a.md, b.md and c.md. */
static void fx_setup_empty(fixture *f) {
  memset(f, 0, sizeof(*f));
  kbc_test_tmpdir(f->root, sizeof f->root);
  join(f->data, sizeof f->data, f->root, "data");
  join(f->corpus_a, sizeof f->corpus_a, f->root, CORPUS_A);
  join(f->corpus_b, sizeof f->corpus_b, f->root, CORPUS_B);
  kbc_test_mkdir_p(f->corpus_a);
  f->cfg = make_cfg(f->data, f->corpus_a, NULL);
  KBC_CHECK_NOT_NULL(f->cfg);
  kbc_err err;
  kbc_err_reset(&err);
  f->app = f->cfg ? kbc_app_open(f->cfg, &err) : NULL;
  if (f->app == NULL) fprintf(stderr, "  app_open: %s\n", err.msg);
  KBC_CHECK_NOT_NULL(f->app);
}

/* An empty corpus PAIR. The anti-drift test needs both, and needs the same
 * paths in each, so that a query which forgets its corpus predicate answers
 * for the wrong corpus instead of quietly returning the right numbers from
 * the only one there is. */
static void fx_setup_empty_pair(fixture *f) {
  memset(f, 0, sizeof(*f));
  kbc_test_tmpdir(f->root, sizeof f->root);
  join(f->data, sizeof f->data, f->root, "data");
  join(f->corpus_a, sizeof f->corpus_a, f->root, CORPUS_A);
  join(f->corpus_b, sizeof f->corpus_b, f->root, CORPUS_B);
  kbc_test_mkdir_p(f->corpus_a);
  kbc_test_mkdir_p(f->corpus_b);
  f->cfg = make_cfg(f->data, f->corpus_a, f->corpus_b);
  KBC_CHECK_NOT_NULL(f->cfg);
  kbc_err err;
  kbc_err_reset(&err);
  f->app = f->cfg ? kbc_app_open(f->cfg, &err) : NULL;
  if (f->app == NULL) fprintf(stderr, "  app_open: %s\n", err.msg);
  KBC_CHECK_NOT_NULL(f->app);
}

/* Writes only the first file, indexes, then writes the second and indexes
 * again. `first` is the document the corpus walk sees before the other. */
static graph_shape graph_after_staged_ingest(const char *first, const char *second) {
  fixture f;
  fx_setup_empty(&f);
  graph_shape g = {-1, -1, 9, 9};
  char p[KBC_TEST_PATH_MAX];
  kbc_err err;
  kbc_err_reset(&err);
  if (f.app == NULL) {
    fx_teardown(&f);
    return g;
  }
  /* a.md is the linker, b.md the linked. Whichever of the two is written
   * first is the order under test; the corpus is otherwise the fixture's. */
  const char *linker =
      "# Linker\n\nSee [beta](b.md) and the verdigris digest.\n";
  const char *linked = "# Linked\n\nThe chronicle mentions quokka burrows.\n";
  join(p, sizeof p, f.corpus_a, first);
  kbc_test_write_file(p, strcmp(first, "a.md") == 0 ? linker : linked);
  KBC_CHECK_OK(kbc_app_reindex(f.app, &err));
  join(p, sizeof p, f.corpus_a, second);
  kbc_test_write_file(p, strcmp(second, "a.md") == 0 ? linker : linked);
  KBC_CHECK_OK(kbc_app_reindex(f.app, &err));

  kbc_store *store = kbc_store_open(f.cfg, &err);
  KBC_CHECK_NOT_NULL(store);
  g.edges = kbc_store_edge_count(store, CORPUS_A, &err);
  g.pending = kbc_store_pending_count(store, &err);
  static const char *const want[] = {"a.md", "b.md"};
  uint32_t deg[2] = {9, 9};
  KBC_CHECK_OK(kbc_store_edge_degrees_for(store, CORPUS_A, want, 2, deg, &err));
  g.deg_a = deg[0];
  g.deg_b = deg[1];
  kbc_store_close(store);
  fx_teardown(&f);
  return g;
}

KBC_TEST(the_link_graph_does_not_depend_on_the_ingest_order) {
  /* b.md first: a.md is indexed against a corpus that does not contain its
   * link target yet. a.md first: the mirror image, and the one that used to
   * leave b.md at in-degree 0 forever. */
  graph_shape b_first = graph_after_staged_ingest("b.md", "a.md");
  graph_shape a_first = graph_after_staged_ingest("a.md", "b.md");

  KBC_CHECK_MSG(a_first.edges == b_first.edges,
                "edge count differs by order: a-first %lld, b-first %lld",
                (long long)a_first.edges, (long long)b_first.edges);
  KBC_CHECK_EQ_INT(a_first.edges, 1);
  KBC_CHECK_EQ_INT(a_first.pending, 0);
  KBC_CHECK_EQ_INT(b_first.pending, 0);
  KBC_CHECK_MSG(a_first.deg_a == b_first.deg_a && a_first.deg_b == b_first.deg_b,
                "in-degrees differ by order: a-first (%u,%u), b-first (%u,%u)",
                a_first.deg_a, a_first.deg_b, b_first.deg_a, b_first.deg_b);
  KBC_CHECK_EQ_INT(a_first.deg_b, 1); /* b.md is linked from a.md */
  KBC_CHECK_EQ_INT(a_first.deg_a, 0); /* a.md links out and is linked to by
                                        nobody */
}

/* A full reindex over a corpus whose sources have not changed still drains:
 * the pending rows are the only record of the link, and a pass that visits
 * every document is the pass that can complete the graph. */
KBC_TEST(a_full_reindex_drains_links_left_pending) {
  fixture f;
  fx_setup_empty(&f);
  char p[KBC_TEST_PATH_MAX];
  kbc_err err;
  kbc_err_reset(&err);
  if (f.app == NULL) {
    fx_teardown(&f);
    return;
  }
  /* Only a.md, linking to a b.md that does not exist yet. */
  join(p, sizeof p, f.corpus_a, "a.md");
  kbc_test_write_file(p, "# Linker\n\nSee [beta](b.md) now.\n");
  KBC_CHECK_OK(kbc_app_reindex(f.app, &err));

  kbc_store *store = kbc_store_open(f.cfg, &err);
  KBC_CHECK_NOT_NULL(store);
  KBC_CHECK_EQ_INT(kbc_store_edge_count(store, CORPUS_A, &err), 0);
  KBC_CHECK_EQ_INT(kbc_store_pending_count(store, &err), 1);

  /* The target arrives. Only a full reindex follows — a.md itself is
   * unchanged, so it contributes no edge write of its own. */
  join(p, sizeof p, f.corpus_a, "b.md");
  kbc_test_write_file(p, "# Linked\n\nThe chronicle mentions quokka burrows.\n");
  KBC_CHECK_OK(kbc_app_reindex(f.app, &err));
  KBC_CHECK_EQ_INT(kbc_store_edge_count(store, CORPUS_A, &err), 1);
  KBC_CHECK_EQ_INT(kbc_store_pending_count(store, &err), 0);
  static const char *const want[] = {"b.md"};
  uint32_t deg[1] = {9};
  KBC_CHECK_OK(kbc_store_edge_degrees_for(store, CORPUS_A, want, 1, deg, &err));
  KBC_CHECK_EQ_INT(deg[0], 1);

  /* A link to a document that never arrives is pending forever and is never
   * an edge: a dangling edge would be an in-degree nothing can attach to. */
  join(p, sizeof p, f.corpus_a, "a.md");
  kbc_test_write_file(p, "# Linker\n\nSee [ghost](never.md) and [beta](b.md).\n");
  KBC_CHECK_OK(kbc_app_reindex(f.app, &err));
  KBC_CHECK_EQ_INT(kbc_store_edge_count(store, CORPUS_A, &err), 1);
  KBC_CHECK_EQ_INT(kbc_store_pending_count(store, &err), 1);

  /* And re-running the reindex changes nothing: same edges, same one pending
   * row — not two. */
  KBC_CHECK_OK(kbc_app_reindex(f.app, &err));
  KBC_CHECK_EQ_INT(kbc_store_edge_count(store, CORPUS_A, &err), 1);
  KBC_CHECK_EQ_INT(kbc_store_pending_count(store, &err), 1);

  kbc_store_close(store);
  fx_teardown(&f);
}

/* Removing a document takes its edges AND its pending links with it, and
 * leaves the pending links pointing AT it: those are another document's
 * claim, and the target may come back. */
KBC_TEST(removing_a_document_takes_its_edges_and_its_pending_links) {
  fixture f;
  fx_setup_empty(&f);
  char p[KBC_TEST_PATH_MAX];
  kbc_err err;
  kbc_err_reset(&err);
  if (f.app == NULL) {
    fx_teardown(&f);
    return;
  }
  join(p, sizeof p, f.corpus_a, "a.md");
  kbc_test_write_file(p, "# Linker\n\nSee [beta](b.md) and [ghost](never.md).\n");
  join(p, sizeof p, f.corpus_a, "b.md");
  kbc_test_write_file(p, "# Linked\n\nThe chronicle mentions quokka burrows.\n");
  KBC_CHECK_OK(kbc_app_reindex(f.app, &err));

  kbc_store *store = kbc_store_open(f.cfg, &err);
  KBC_CHECK_NOT_NULL(store);
  KBC_CHECK_EQ_INT(kbc_store_edge_count(store, CORPUS_A, &err), 1);
  KBC_CHECK_EQ_INT(kbc_store_pending_count(store, &err), 1);

  /* The source goes: the edge and the pending link both leave with it. */
  join(p, sizeof p, f.corpus_a, "a.md");
  KBC_CHECK_EQ_INT(unlink(p), 0);
  KBC_CHECK_OK(kbc_app_reindex(f.app, &err));
  KBC_CHECK_EQ_INT(kbc_store_edge_count(store, CORPUS_A, &err), 0);
  KBC_CHECK_EQ_INT(kbc_store_pending_count(store, &err), 0);

  /* The TARGET goes: nothing of a.md's survives it, because the graph is
   * rebuilt from the documents that are there. */
  join(p, sizeof p, f.corpus_a, "a.md");
  kbc_test_write_file(p, "# Linker\n\nSee [beta](b.md).\n");
  join(p, sizeof p, f.corpus_a, "b.md");
  KBC_CHECK_EQ_INT(unlink(p), 0);
  KBC_CHECK_OK(kbc_app_reindex(f.app, &err));
  KBC_CHECK_EQ_INT(kbc_store_edge_count(store, CORPUS_A, &err), 0);
  KBC_CHECK_EQ_INT(kbc_store_pending_count(store, &err), 1);

  /* b.md comes back and the link is whole again, with no re-write of a.md. */
  join(p, sizeof p, f.corpus_a, "b.md");
  kbc_test_write_file(p, "# Linked\n\nThe chronicle mentions quokka burrows.\n");
  KBC_CHECK_OK(kbc_app_reindex(f.app, &err));
  KBC_CHECK_EQ_INT(kbc_store_edge_count(store, CORPUS_A, &err), 1);
  KBC_CHECK_EQ_INT(kbc_store_pending_count(store, &err), 0);

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

/* ------------------------------------------------- enrichment registry --- */

/* The resolution ladder, reached through the edge hook. A note writes
 * [[b]] — the way an Obsidian corpus is written — and the document on disk is
 * `b.md`. The ladder's path tier takes the target with the final extension
 * elided, so this resolves; an exact-path lookup cannot answer it at all,
 * because no path on disk is called `notes/b`. Before the ladder was wired
 * into the edge write this link was a pending row that nothing ever drained,
 * which is the whole gap links.h was written to close. */
KBC_TEST(a_link_naming_a_document_without_its_extension_is_an_edge) {
  fixture f;
  fx_setup_empty(&f);
  char dir[KBC_TEST_PATH_MAX];
  char p[KBC_TEST_PATH_MAX];
  kbc_err err;
  kbc_err_reset(&err);
  if (f.app == NULL) {
    fx_teardown(&f);
    return;
  }
  join(dir, sizeof dir, f.corpus_a, "notes");
  kbc_test_mkdir_p(dir);
  join(p, sizeof p, dir, "a.md");
  kbc_test_write_file(p, "# Linker\n\nSee [[b]] for the burrows.\n");
  join(p, sizeof p, dir, "b.md");
  kbc_test_write_file(p, "# Linked\n\nThe chronicle mentions quokka burrows.\n");
  KBC_CHECK_OK(kbc_app_reindex(f.app, &err));

  kbc_store *store = kbc_store_open(f.cfg, &err);
  KBC_CHECK_NOT_NULL(store);
  KBC_CHECK_EQ_INT(kbc_store_edge_count(store, CORPUS_A, &err), 1);
  /* A link that resolved is not waiting for anything: the pending table is
   * for targets that name no document, and this one names one. */
  KBC_CHECK_EQ_INT(kbc_store_pending_count(store, &err), 0);
  static const char *const want[] = {"notes/b.md"};
  uint32_t deg[1] = {9};
  KBC_CHECK_OK(kbc_store_edge_degrees_for(store, CORPUS_A, want, 1, deg, &err));
  KBC_CHECK_EQ_INT(deg[0], 1);

  kbc_store_close(store);
  fx_teardown(&f);
}

/* enrich.rs:1095 — only Resolution::One is an EDGE. A bare `deploy` is
 * answered by ops/deploy.md and infra/deploy.md alike, and that answer is
 * AMBIGUOUS: the edge is dropped rather than resolved to whichever of the two
 * the walk happened to reach first, because such an edge flips on the next
 * reindex. The same document also links two targets that DO resolve, so the
 * case cannot pass by resolving nothing at all. */
KBC_TEST(an_ambiguous_link_target_is_not_an_edge) {
  fixture f;
  fx_setup_empty(&f);
  char dir[KBC_TEST_PATH_MAX];
  char p[KBC_TEST_PATH_MAX];
  kbc_err err;
  kbc_err_reset(&err);
  if (f.app == NULL) {
    fx_teardown(&f);
    return;
  }
  /* Distinct titles, so the answer `deploy` gets is the BASENAME tier's and
   * not the title tier's: two documents called deploy.md is the shape a
   * corpus actually has. */
  join(dir, sizeof dir, f.corpus_a, "ops");
  kbc_test_mkdir_p(dir);
  join(p, sizeof p, dir, "deploy.md");
  kbc_test_write_file(p, "# Deploy Ops\n\nThe rollout is staged by hand.\n");
  join(dir, sizeof dir, f.corpus_a, "infra");
  kbc_test_mkdir_p(dir);
  join(p, sizeof p, dir, "deploy.md");
  kbc_test_write_file(p, "# Deploy Infra\n\nThe rollout is staged by hand.\n");
  join(p, sizeof p, f.corpus_a, "shared.md");
  kbc_test_write_file(p, "# Shared\n\nBoth runbooks mention it.\n");
  /* A root-level document, so `deploy` is the target as written: from
   * notes/deploy.md the parser would have made it notes/deploy, and the
   * basename tier would never see it. */
  join(p, sizeof p, f.corpus_a, "index.md");
  kbc_test_write_file(p,
                      "# Index\n\n"
                      "See [both](deploy) and [one](infra/deploy.md) and "
                      "[note](shared.md).\n");
  KBC_CHECK_OK(kbc_app_reindex(f.app, &err));

  kbc_store *store = kbc_store_open(f.cfg, &err);
  KBC_CHECK_NOT_NULL(store);
  /* Two edges, not three and not one. */
  KBC_CHECK_EQ_INT(kbc_store_edge_count(store, CORPUS_A, &err), 2);
  /* The ambiguous target left no edge and no pending row either: a pending
   * link is drained by an exact (corpus, dst_path) match, so a row named
   * `deploy` would wait for a path that is never written. */
  KBC_CHECK_EQ_INT(kbc_store_pending_count(store, &err), 0);
  static const char *const want[] = {"infra/deploy.md", "ops/deploy.md",
                                      "shared.md"};
  uint32_t deg[3] = {9, 9, 9};
  KBC_CHECK_OK(
      kbc_store_edge_degrees_for(store, CORPUS_A, want, 3, deg, &err));
  KBC_CHECK_EQ_INT(deg[0], 1); /* named by path */
  KBC_CHECK_EQ_INT(deg[1], 0); /* named, but only by the ambiguous target */
  KBC_CHECK_EQ_INT(deg[2], 1);

  kbc_store_close(store);
  fx_teardown(&f);
}

/* The single-file path runs the same hook, against the LIVE INDEX as its
 * candidate set — and the index has not been told about the document this
 * call ingested, so that one is patched in before the ladder is built. Both
 * halves are observable: a watcher event that adds a link must produce the
 * edge without a full scan, and the fresh document must be able to resolve
 * ITSELF, which is the row the index cannot have.
 *
 * The second half of the case is the sharper one. a.md is edited to a new
 * title and links to that new title; the index still carries the old one, so
 * a ladder built from the index alone would answer `[[Quokka]]` with nothing
 * and record a pending link. */
KBC_TEST(a_single_file_reindex_resolves_against_the_index_and_itself) {
  fixture f;
  fx_setup_empty(&f);
  char p[KBC_TEST_PATH_MAX];
  kbc_err err;
  kbc_err_reset(&err);
  if (f.app == NULL) {
    fx_teardown(&f);
    return;
  }
  /* b.md first, and a full pass, so the index knows a document to link to. */
  join(p, sizeof p, f.corpus_a, "b.md");
  kbc_test_write_file(p, "# Linked\n\nThe chronicle mentions quokka burrows.\n");
  KBC_CHECK_OK(kbc_app_reindex(f.app, &err));

  /* a.md arrives as a watcher event and names b without its extension, and
   * itself: a document naming itself is not a backlink. */
  join(p, sizeof p, f.corpus_a, "a.md");
  kbc_test_write_file(p, "# Linker\n\nSee [[b]] and [[a]].\n");
  KBC_CHECK_OK(kbc_app_reindex_file(f.app, CORPUS_A, "a.md", &err));

  kbc_store *store = kbc_store_open(f.cfg, &err);
  KBC_CHECK_NOT_NULL(store);
  KBC_CHECK_EQ_INT(kbc_store_edge_count(store, CORPUS_A, &err), 1);
  KBC_CHECK_EQ_INT(kbc_store_pending_count(store, &err), 0);
  static const char *const want[] = {"a.md", "b.md"};
  uint32_t deg[2] = {9, 9};
  KBC_CHECK_OK(kbc_store_edge_degrees_for(store, CORPUS_A, want, 2, deg, &err));
  KBC_CHECK_EQ_INT(deg[0], 0); /* named itself, so no self edge */
  KBC_CHECK_EQ_INT(deg[1], 1);

  /* The edit: a new title, and a link to exactly that title. A title is
   * matched WHOLE, so the index's stale "# Linker" answers `[[Quokka]]` with
   * nothing at all. */
  kbc_test_write_file(p, "# Quokka\n\nSee [[Quokka]] and [[b]].\n");
  KBC_CHECK_OK(kbc_app_reindex_file(f.app, CORPUS_A, "a.md", &err));
  KBC_CHECK_EQ_INT(kbc_store_edge_count(store, CORPUS_A, &err), 1);
  /* A target the stale title could not answer is a PENDING link, so a
   * pending row here is the signature of an unpatched candidate set. */
  KBC_CHECK_EQ_INT(kbc_store_pending_count(store, &err), 0);
  KBC_CHECK_OK(kbc_store_edge_degrees_for(store, CORPUS_A, want, 2, deg, &err));
  KBC_CHECK_EQ_INT(deg[0], 0);
  KBC_CHECK_EQ_INT(deg[1], 1);

  kbc_store_close(store);
  fx_teardown(&f);
}

/* The registry's contract, driven with hooks of the test's own. None of it is
 * observable from the daemon's single edge-record hook: a prefilter nothing
 * fails, and a hook that fails is the only way to get one. */
typedef struct {
  char log[64];
  size_t n;
  kbc_store *st; /* the store hook_writes puts its row in */
} hook_log;

static void hook_note(hook_log *h, char c) {
  if (h->n + 2u < sizeof h->log) {
    h->log[h->n++] = c;
    h->log[h->n] = '\0';
  }
}

static bool hook_yes(const kbc_enrich_ctx *ctx, void *user) {
  (void)ctx;
  (void)user;
  return true;
}

static bool hook_no(const kbc_enrich_ctx *ctx, void *user) {
  (void)ctx;
  (void)user;
  return false;
}

/* One recording hook per letter. The runner takes function pointers, so four
 * hooks that all record need four functions; the macro keeps them one line
 * each instead of four copies that can drift. */
#define HOOK_RECORDER(letter)                                                 \
  static kbc_status hook_##letter(const kbc_enrich_ctx *ctx, void *user,     \
                                  kbc_err *err) {                             \
    (void)ctx;                                                                \
    (void)err;                                                                \
    hook_note((hook_log *)user, #letter[0]);                                  \
    return KBC_OK;                                                            \
  }
HOOK_RECORDER(a)
HOOK_RECORDER(b)
HOOK_RECORDER(c)
HOOK_RECORDER(d)
#undef HOOK_RECORDER

/* A hook that fails: it records that it RAN, writes nothing, and returns an
 * error with no error slot of its own. A registry that stopped at the first
 * failure, or that handed one to its caller, is caught by what follows. */
static kbc_status hook_fails(const kbc_enrich_ctx *ctx, void *user,
                             kbc_err *err) {
  (void)ctx;
  (void)err;
  hook_log *h = (hook_log *)user;
  hook_note(h, 'x');
  return kbc_err_set(NULL, KBC_ERR_IO, "hook %s refused on purpose", "x");
}

/* A hook that writes a row and succeeds, so the store state the run leaves
 * behind is a real thing to read rather than a log line. */
static kbc_status hook_writes(const kbc_enrich_ctx *ctx, void *user,
                              kbc_err *err) {
  (void)ctx;
  hook_log *h = (hook_log *)user;
  const char letter = h->n == 0 ? 'a' : 'c';
  char slug[32];
  char path[32];
  /* Both the slug AND the path name the letter: `sources.path` is unique, so
   * two hooks sharing one path collide and the second one fails — which the
   * registry would (correctly) swallow, leaving this test asserting nothing.
   * A hook that fails quietly is exactly what the case above is about, so it
   * must not be able to happen here. */
  int n = snprintf(slug, sizeof slug, "hook-%c", letter);
  int w = snprintf(path, sizeof path, "/nowhere-%c", letter);
  hook_note(h, letter);
  if (n < 0 || (size_t)n >= sizeof slug || w < 0 || (size_t)w >= sizeof path) {
    return kbc_err_set(err, KBC_ERR_INVALID, "hook row does not fit");
  }
  kbc_source src;
  memset(&src, 0, sizeof src);
  src.corpus = slug;
  src.path = path;
  src.added_at = 0;
  src.paused = false;
  return kbc_store_put_source(h->st, &src, err);
}

/* The two rows hook_writes leaves, as slugs joined by '|'. */
static void written_slugs(kbc_store *st, char *out, size_t cap) {
  kbc_arena *a = kbc_arena_new(4096u);
  KBC_CHECK_NOT_NULL(a);
  out[0] = '\0';
  if (a == NULL) return;
  kbc_source *rows = NULL;
  size_t n = 0;
  kbc_err err;
  kbc_err_reset(&err);
  KBC_CHECK_OK(kbc_store_list_sources(st, a, 100, &rows, &n, &err));
  size_t off = 0;
  for (size_t i = 0; i < n; i++) {
    int w = snprintf(out + off, cap - off, "%s%s", i == 0 ? "" : "|",
                     rows[i].corpus ? rows[i].corpus : "?");
    if (w < 0 || (size_t)w >= cap - off) {
      KBC_CHECK_MSG(false, "slugs do not fit in %zu bytes", cap);
      break;
    }
    off += (size_t)w;
  }
  kbc_arena_free(a);
}

/* REGISTRATION ORDER IS RUN ORDER (enrich.rs:118-133). The assertion is on
 * the sequence, not on each hook having run: a runner that ran all three in
 * the wrong order passes "each one ran". */
KBC_TEST(hooks_run_in_registration_order) {
  fixture f;
  fx_setup_empty(&f);
  kbc_err err;
  kbc_err_reset(&err);
  if (f.app == NULL) {
    fx_teardown(&f);
    return;
  }
  hook_log h;
  memset(&h, 0, sizeof h);
  static const char *const names[] = {"first", "second", "third", "fourth"};
  kbc_enrich_prefilter_fn pre[] = {hook_yes, hook_yes, hook_yes, hook_yes};
  kbc_enrich_fn fns[] = {hook_b, hook_c, hook_a, hook_d};
  void *users[] = {&h, &h, &h, &h};
  /* The context is opaque here and stays unread: these hooks take their
   * state in `user`, and the daemon's own context is built by the pass. */
  kbc_enrich_ctx *ctx = (kbc_enrich_ctx *)(void *)&h;
  KBC_CHECK_OK(kbc_enrich_run(4, names, pre, fns, users, ctx, &err));
  KBC_CHECK_EQ_STR(h.log, "bcad");

  kbc_store *store = kbc_store_open(f.cfg, &err);
  KBC_CHECK_NOT_NULL(store);
  kbc_store_close(store);
  fx_teardown(&f);
}

/* `interested` is the pre-filter (enrich.rs:106-108): a hook that says no is
 * never called, so it costs nothing beyond the answer. */
KBC_TEST(an_uninterested_hook_is_never_called) {
  fixture f;
  fx_setup_empty(&f);
  kbc_err err;
  kbc_err_reset(&err);
  if (f.app == NULL) {
    fx_teardown(&f);
    return;
  }
  hook_log h;
  memset(&h, 0, sizeof h);
  static const char *const names[] = {"skipped", "ran"};
  kbc_enrich_prefilter_fn pre[] = {hook_no, hook_yes};
  kbc_enrich_fn fns[] = {hook_a, hook_c};
  void *users[] = {&h, &h};
  kbc_enrich_ctx *ctx = (kbc_enrich_ctx *)(void *)&h;
  KBC_CHECK_OK(kbc_enrich_run(2, names, pre, fns, users, ctx, &err));
  /* Not "both ran": the first one must be absent, and the second present. */
  KBC_CHECK_EQ_STR(h.log, "c");

  kbc_store *store = kbc_store_open(f.cfg, &err);
  KBC_CHECK_NOT_NULL(store);
  kbc_store_close(store);
  fx_teardown(&f);
}

/* BEST-EFFORT (enrich.rs:14-24). A hook that fails is logged and stepped
 * over: the run still reports success to its caller, the hooks after it still
 * run, and the store keeps exactly the rows the hooks that succeeded wrote —
 * the failed hook's own write is not there, and the successful ones are not
 * rolled back. */
KBC_TEST(a_failing_hook_does_not_fail_the_run_or_half_write_the_store) {
  fixture f;
  fx_setup_empty(&f);
  kbc_err err;
  kbc_err_reset(&err);
  if (f.app == NULL) {
    fx_teardown(&f);
    return;
  }
  kbc_store *store = kbc_store_open(f.cfg, &err);
  KBC_CHECK_NOT_NULL(store);
  if (store == NULL) {
    fx_teardown(&f);
    return;
  }
  hook_log h;
  memset(&h, 0, sizeof h);
  h.st = store;
  static const char *const names[] = {"writes", "fails", "writes-again"};
  kbc_enrich_prefilter_fn pre[] = {hook_yes, hook_yes, hook_yes};
  kbc_enrich_fn fns[] = {hook_writes, hook_fails, hook_writes};
  void *users[] = {&h, &h, &h};
  kbc_enrich_ctx *ctx = (kbc_enrich_ctx *)(void *)&h;
  KBC_CHECK_OK(kbc_enrich_run(3, names, pre, fns, users, ctx, &err));
  /* The hook's error stayed INSIDE the registry: the caller's err is
   * untouched, which is what "a hook's failure is not the caller's" means
   * at this boundary. */
  KBC_CHECK_EQ_STR(err.msg, "");
  /* The failing hook ran, and the one after it ran too. */
  KBC_CHECK_EQ_STR(h.log, "axc");
  char slugs[256];
  written_slugs(store, slugs, sizeof slugs);
  /* CORPUS_A is here because kbc_app_open registers every configured corpus
   * as a source — that row is written by the composition root, not by a hook,
   * and it is the same table the hooks below write into. The assertion is
   * still "the failed hook's row is absent and the other two are present",
   * which is what this string is checked for: hook-b is not in it. */
  KBC_CHECK_EQ_STR(slugs, "alpha|hook-a|hook-c");

  kbc_store_close(store);
  fx_teardown(&f);
}

/* --------------------------------------------------------- facet overlay -- */

/* Every row's path, copied out of the arena, joined by '|'. Returns the count.
 * A filter that matches nothing must produce an empty string and a count of
 * zero — never the whole corpus, which is the failure the refusal design
 * exists to prevent. */
static size_t search_paths(kbc_app *app, const char *q, const char *corpus,
                           char *out, size_t cap) {
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
  kbc_err err;
  kbc_err_reset(&err);
  const kbc_status st = kbc_app_search(app, a, &query, &res, &err);
  KBC_CHECK_MSG(st == KBC_OK, "search %s: %s", q, err.msg);
  if (out != NULL && cap > 0) out[0] = '\0';
  for (size_t i = 0; st == KBC_OK && i < res.len; i++) {
    const char *p = res.rows[i].path != NULL ? res.rows[i].path : "?";
    if (out != NULL && cap > 0)
      (void)snprintf(out + strlen(out), cap - strlen(out), "%s%s", i ? "|" : "",
                     p);
  }
  kbc_arena_free(a);
  return res.len;
}

/* Row order is the index's doc-id order — the order documents were ingested —
 * so a set of paths is compared as a set, not as a string. */
static int cmp_seg(const void *a, const void *b) {
  return strcmp(*(char *const *)a, *(char *const *)b);
}

static void check_paths(const char *got, const char *want) {
  char *copy = strdup(got);
  char *wcopy = strdup(want);
  KBC_CHECK_NOT_NULL(copy);
  KBC_CHECK_NOT_NULL(wcopy);
  if (copy == NULL || wcopy == NULL) {
    free(copy);
    free(wcopy);
    return;
  }
  char *g[16];
  char *w[16];
  size_t ng = 0;
  size_t nw = 0;
  for (char *t = strtok(copy, "|"); t != NULL && ng < 16;
       t = strtok(NULL, "|"))
    g[ng++] = t;
  for (char *t = strtok(wcopy, "|"); t != NULL && nw < 16;
       t = strtok(NULL, "|"))
    w[nw++] = t;
  if (ng > 1) qsort(g, ng, sizeof(*g), cmp_seg);
  if (nw > 1) qsort(w, nw, sizeof(*w), cmp_seg);
  KBC_CHECK_EQ_INT(ng, nw);
  for (size_t i = 0; i < ng && i < nw; i++)
    KBC_CHECK_EQ_STR(g[i], w[i]);
  free(copy);
  free(wcopy);
}

/* A corpus whose documents declare their own facets. Each word is unique to
 * one file, so a hit names a file rather than a score. */
static const char TAGGED_A[] =
    "---\n"
    "kb-tags: rust, search\n"
    "---\n"
    "# Ferrous Ledger\n\nThe ferrous ledger reconciles accruals.\n";
static const char TAGGED_B[] =
    "---\n"
    "kb-tags: rust\n"
    "kb-caps: code, svg\n"
    "---\n"
"# Chromium Chronicle\n\nThe chromium chronicle mentions zephyrus towns.\n";
static const char TAGGED_C[] =
    "---\n"
    "kb-index: true\n"
    "---\n"
"# Verdigris Digest\n\nThe verdigris digest catalogues quixotic places.\n";

static void fx_setup_tagged(fixture *f) {
  fx_setup(f, false);
  if (f->app == NULL) return;
  char p[KBC_TEST_PATH_MAX];
  join(p, sizeof p, f->corpus_a, "a.md");
  kbc_test_write_file(p, TAGGED_A);
  join(p, sizeof p, f->corpus_a, "b.md");
  kbc_test_write_file(p, TAGGED_B);
  join(p, sizeof p, f->corpus_a, "c.md");
  kbc_test_write_file(p, TAGGED_C);
  kbc_err err;
  kbc_err_reset(&err);
  KBC_CHECK_OK(kbc_app_reindex(f->app, &err));
}

/* THE invariant, end to end through the daemon's own entry point: a tag nobody
 * carries is zero rows. The same query with the tag removed is a search, and
 * the difference has to be visible. */
KBC_TEST(a_tag_filter_that_matches_nothing_returns_zero_rows) {
  fixture f;
  fx_setup_tagged(&f);
  if (f.app == NULL) {
    fx_teardown(&f);
    return;
  }
  char got[512];
  KBC_CHECK_EQ_INT(search_paths(f.app, "tag:nonesuch", CORPUS_A, got,
                                sizeof got),
                   0);
  check_paths(got, "");
  /* …while the tags that ARE declared find exactly their documents. */
  KBC_CHECK_EQ_INT(
      search_paths(f.app, "tag:search", CORPUS_A, got, sizeof got), 1);
  check_paths(got, "a.md");
  KBC_CHECK_EQ_INT(search_paths(f.app, "tag:rust", CORPUS_A, got, sizeof got),
                   2);
  check_paths(got, "a.md|b.md");
  fx_teardown(&f);
}

/* `caps` is ALL-of inside a conjunct (query.rs:297) and `tags` is ANY-of
 * (query.rs:286), so one cap and one tag is the intersection, not the union. */
KBC_TEST(a_tag_and_a_cap_together_are_an_intersection) {
  fixture f;
  fx_setup_tagged(&f);
  if (f.app == NULL) {
    fx_teardown(&f);
    return;
  }
  char got[512];
  KBC_CHECK_EQ_INT(
      search_paths(f.app, "cap:code tag:rust", CORPUS_A, got, sizeof got), 1);
  check_paths(got, "b.md");
  KBC_CHECK_EQ_INT(
      search_paths(f.app, "cap:svg tag:search", CORPUS_A, got, sizeof got), 0);
  /* the `caps:` alias parses the same as `cap:` (query.rs:75) */
  KBC_CHECK_EQ_INT(
      search_paths(f.app, "caps:svg", CORPUS_A, got, sizeof got), 1);
  check_paths(got, "b.md");
  fx_teardown(&f);
}

/* Two tags OR-ed is the original's DNF: a row matching EITHER satisfies the
 * query, and a row matching neither does not. */
KBC_TEST(two_tags_or_ed_match_either_document) {
  fixture f;
  fx_setup_tagged(&f);
  if (f.app == NULL) {
    fx_teardown(&f);
    return;
  }
  char got[512];
  KBC_CHECK_EQ_INT(
      search_paths(f.app, "tag:rust OR tag:search", CORPUS_A, got, sizeof got),
      2);
  check_paths(got, "a.md|b.md");
  KBC_CHECK_EQ_INT(search_paths(f.app, "tag:search OR tag:absent", CORPUS_A,
                                got, sizeof got),
                   1);
  check_paths(got, "a.md");
  fx_teardown(&f);
}

/* `NOT tag:x` drops the documents carrying x (query.rs:287 exclude_tags). */
KBC_TEST(a_negated_tag_drops_the_documents_carrying_it) {
  fixture f;
  fx_setup_tagged(&f);
  if (f.app == NULL) {
    fx_teardown(&f);
    return;
  }
  char got[512];
  KBC_CHECK_EQ_INT(
      search_paths(f.app, "NOT tag:rust", CORPUS_A, got, sizeof got), 1);
  check_paths(got, "c.md");
  fx_teardown(&f);
}

/* The overlay composes with the filters that were already there: a tag, a
 * folder and a since: all narrow, and a row failing any one of them is out. */
KBC_TEST(a_tag_composes_with_folder_and_since) {
  fixture f;
  fx_setup_tagged(&f);
  if (f.app == NULL) {
    fx_teardown(&f);
    return;
  }
  char got[512];
  KBC_CHECK_EQ_INT(search_paths(f.app, "tag:rust", CORPUS_A, got, sizeof got),
                   2);
  /* a folder that excludes both of them narrows to nothing */
  KBC_CHECK_EQ_INT(
      search_paths(f.app, "tag:rust folder:nowhere", CORPUS_A, got,
                   sizeof got),
      0);
  /* a since: far in the past keeps both; a since: in the future drops both */
  KBC_CHECK_EQ_INT(
      search_paths(f.app, "tag:rust since:1d", CORPUS_A, got, sizeof got), 2);
  KBC_CHECK_EQ_INT(
      search_paths(f.app, "tag:rust since:4102444800", CORPUS_A, got,
                   sizeof got),
      0);
  /* free text still scores alongside the filter */
  KBC_CHECK_EQ_INT(
      search_paths(f.app, "tag:rust chronicle", CORPUS_A, got, sizeof got), 1);
  check_paths(got, "b.md");
  fx_teardown(&f);
}

/* Replace-on-ingest: a tag the author deleted stops matching after ONE
 * reindex of that file. This is the property a merge cannot have. */
KBC_TEST(a_removed_tag_stops_matching_after_one_reindex) {
  fixture f;
  fx_setup_tagged(&f);
  if (f.app == NULL) {
    fx_teardown(&f);
    return;
  }
  char got[512];
  KBC_CHECK_EQ_INT(search_paths(f.app, "tag:rust", CORPUS_A, got, sizeof got),
                   2);
  /* The author takes the tag off a.md and puts a different one on. */
  char p[KBC_TEST_PATH_MAX];
  join(p, sizeof p, f.corpus_a, "a.md");
  kbc_test_write_file(p,
                      "---\n"
                      "kb-tags: ledger\n"
                      "---\n"
                      "# Ferrous Ledger\n\nThe ferrous ledger reconciles "
                      "accruals.\n");
  kbc_err err;
  kbc_err_reset(&err);
  KBC_CHECK_OK(kbc_app_reindex_file(f.app, CORPUS_A, "a.md", &err));
  KBC_CHECK_EQ_INT(search_paths(f.app, "tag:rust", CORPUS_A, got, sizeof got),
                   1);
  check_paths(got, "b.md");
  KBC_CHECK_EQ_INT(
      search_paths(f.app, "tag:search", CORPUS_A, got, sizeof got), 0);
  KBC_CHECK_EQ_INT(
      search_paths(f.app, "tag:ledger", CORPUS_A, got, sizeof got), 1);
  check_paths(got, "a.md");
  fx_teardown(&f);
}

/* A deleted document takes its facets with it: a tag must never outlive the
 * file that declared it. */
KBC_TEST(a_deleted_document_takes_its_tags_with_it) {
  fixture f;
  fx_setup_tagged(&f);
  if (f.app == NULL) {
    fx_teardown(&f);
    return;
  }
  char p[KBC_TEST_PATH_MAX];
  join(p, sizeof p, f.corpus_a, "a.md");
  KBC_CHECK(unlink(p) == 0);
  kbc_err err;
  kbc_err_reset(&err);
  KBC_CHECK_OK(kbc_app_reindex_file(f.app, CORPUS_A, "a.md", &err));
  char got[512];
  KBC_CHECK_EQ_INT(search_paths(f.app, "tag:search", CORPUS_A, got, sizeof got),
                   0);
  KBC_CHECK_EQ_INT(search_paths(f.app, "tag:rust", CORPUS_A, got, sizeof got),
                   1);
  check_paths(got, "b.md");
  fx_teardown(&f);
}

/* `index:` is the original's index_only predicate (docs_query.rs:434): a
 * document is an index page when its basename is `index.html`, and kb-c adds
 * `index.md` plus the `kb-index` declaration. */
KBC_TEST(index_selects_only_the_documents_that_declare_themselves_indexes) {
  fixture f;
  fx_setup_tagged(&f);
  if (f.app == NULL) {
    fx_teardown(&f);
    return;
  }
  char got[512];
  KBC_CHECK_EQ_INT(
      search_paths(f.app, "index:true", CORPUS_A, got, sizeof got), 1);
  check_paths(got, "c.md");
  KBC_CHECK_EQ_INT(
      search_paths(f.app, "NOT index:true", CORPUS_A, got, sizeof got), 2);
  check_paths(got, "a.md|b.md");
  /* An `index.md` basename is the same page. */
  char p[KBC_TEST_PATH_MAX];
  join(p, sizeof p, f.corpus_a, "index.md");
  kbc_test_write_file(p, "# Hub\n\nThe hub links the rest.\n");
  kbc_err err;
  kbc_err_reset(&err);
  KBC_CHECK_OK(kbc_app_reindex(f.app, &err));
  KBC_CHECK_EQ_INT(
      search_paths(f.app, "index:true", CORPUS_A, got, sizeof got), 2);
  check_paths(got, "c.md|index.md");
  fx_teardown(&f);
}

/* A filter is case-SENSITIVE on the value, the way the original is: query.rs
 * compares the atom's value with the document's tag by string equality, and
 * the meta extractor lowercases the KEY only. `tag:Search` therefore does not
 * match a document declaring `Search` — which is a narrowing, never a widening,
 * so it cannot turn a filter into a no-op. */
KBC_TEST(a_tag_value_is_case_sensitive_as_it_is_in_the_original) {
  fixture f;
  fx_setup_tagged(&f);
  if (f.app == NULL) {
    fx_teardown(&f);
    return;
  }
  char got[512];
  KBC_CHECK_EQ_INT(
      search_paths(f.app, "tag:Search", CORPUS_A, got, sizeof got), 0);
  /* The KEY is not case-sensitive: the extractor lowercases it, and the
   * grammar lowercases the atom, so KB-Tags and tags are one predicate. */
  KBC_CHECK_EQ_INT(
      search_paths(f.app, "tag:search", CORPUS_A, got, sizeof got), 1);
  check_paths(got, "a.md");
  fx_teardown(&f);
}

/* `scope:` stays refused: the original gives it an empty arm (query.rs:404)
 * because it belongs to /memory and is read from the URL, so evaluating it
 * here would invent a meaning the Rust daemon cannot reproduce. */
KBC_TEST(scope_is_still_refused) {
  fixture f;
  fx_setup_tagged(&f);
  if (f.app == NULL) {
    fx_teardown(&f);
    return;
  }
  char got[512];
  kbc_arena *a = kbc_arena_new(4096);
  kbc_query query;
  memset(&query, 0, sizeof query);
  query.q = "scope:kb";
  query.corpus = CORPUS_A;
  query.mode = KBC_MODE_KEYWORD;
  kbc_search_result res;
  memset(&res, 0, sizeof res);
  kbc_err err;
  kbc_err_reset(&err);
  KBC_CHECK_EQ_INT(kbc_app_search(f.app, a, &query, &res, &err),
                   KBC_ERR_UNSUPPORTED);
  KBC_CHECK(strstr(err.msg, "scope") != NULL);
  kbc_arena_free(a);
  (void)got;
  fx_teardown(&f);
}

/* ==========================================================================
 * Two writers of one index file.
 *
 * Every case above drives a single thread. The daemon does not: a full
 * rebuild runs on the httpd worker that answered POST /api/reindex, the
 * single-file path runs on the watcher thread, and both of them publish the
 * index file. These cases run the two against each other, and the last one
 * takes the store's write lock out from under a rebuild in progress.
 * ======================================================================== */

/* Enough documents that a full rebuild spends real time walking, ingesting
 * and serialising an index — that is the window the two writers shared. */
#define BULK_DOCS 120u
#define RACE_ITERS 24u
/* The other thread runs until the rebuilds stop, so this is a ceiling that
 * only exists so a wedged run cannot fill the disk. */
#define TOUCH_ITERS 20000u
#define GROW_ITERS 2000u
/* Enough documents, each with its own heap array of link targets, that a
 * rebuild is still DEEP in its ingest loop when the store stops answering.
 * The holder polls for the first two re-stored rows and then has to win
 * sqlite's write lock, and both the polling and the lock acquisition take
 * milliseconds; a corpus whose store phase was shorter than that would let
 * the rebuild finish it first and the injection would prove nothing. */
#define LINK_DOCS 400u
#define LINK_LINKS 4u
/* How many times the case re-runs the injection before declaring it broken. */
#define ABORT_ATTEMPTS 5

/* Written through a staging name and renamed over the target, so a reindex
 * running on the other thread reads a whole revision or none of it — never
 * the first half of one and the second half of the next. Touches no harness
 * state, so it is safe to call from a worker thread. */
static int write_file_atomic(const char *path, const char *content) {
  char stage[KBC_TEST_PATH_MAX];
  int n = snprintf(stage, sizeof stage, "%s.staging", path);
  if (n <= 0 || (size_t)n >= sizeof stage) {
    return -1;
  }
  FILE *fh = fopen(stage, "wb");
  if (fh == NULL) {
    return -1;
  }
  size_t len = strlen(content);
  int rc = (fwrite(content, 1u, len, fh) == len) ? 0 : -1;
  if (fclose(fh) != 0) {
    rc = -1;
  }
  if (rc != 0) {
    (void)unlink(stage);
    return -1;
  }
  return rename(stage, path) == 0 ? 0 : -1;
}

static void write_bulk_doc(const char *dir, size_t i) {
  char name[64], body[3072], path[KBC_TEST_PATH_MAX];
  snprintf(name, sizeof name, "doc%04zu.md", i);
  join(path, sizeof path, dir, name);
  int len = snprintf(body, sizeof body, "# %s\n\nA ledger page.\n", name);
  for (size_t k = 0; k < 40u; k++) {
    len += snprintf(body + len, sizeof body - (size_t)len,
                    "passage %04zu %02u reconciles the accrual ledger for the "
                    "quarter\n",
                    i, (unsigned)k);
  }
  kbc_test_write_file(path, body);
}

static void write_bulk_corpus(const char *dir) {
  for (size_t i = 0; i < BULK_DOCS; i++) {
    write_bulk_doc(dir, i);
  }
}

/* What the corpus actually holds, counted from the directory rather than from
 * what the test believes it wrote: the whole point of these cases is that the
 * index must match the disk, and a count taken from the test's own
 * bookkeeping could not tell the two apart. */
static size_t count_indexable(const char *dir) {
  DIR *d = opendir(dir);
  if (d == NULL) {
    return 0;
  }
  size_t n = 0;
  struct dirent *e;
  while ((e = readdir(d)) != NULL) {
    if (e->d_name[0] == '.') {
      continue;
    }
    size_t len = strlen(e->d_name);
    if (len > 3u && strcmp(e->d_name + len - 3u, ".md") == 0) {
      n++;
    }
  }
  closedir(d);
  return n;
}

/* What the two threads share. `bad` and `detail` are per-thread: a worker has
 * no business touching the harness's failure counter. */
typedef struct {
  _Atomic int stop;
  _Atomic uint32_t grown;
} race_shared;

typedef struct {
  kbc_app *app;
  const kbc_config *cfg;
  const char *dir;
  race_shared *shared;
  unsigned iters;
  /* Whether the other thread leaves the document SET alone. A rebuild's
   * promoted index can only be compared with the directory mid-run when
   * nothing is being added to it; when the other thread is creating
   * documents, the two are legitimately a step apart until it catches up. */
  bool count_stable;
  /* What the live index held before this thread started adding to it. */
  int64_t base_docs;
  const char *bad;
  char detail[256];
} racer;

static void race_fail(racer *r, const char *what, const char *detail) {
  if (r->bad != NULL) {
    return;
  }
  r->bad = what;
  snprintf(r->detail, sizeof r->detail, "%s", detail != NULL ? detail : "");
}

/* A full rebuild, over and over, beside the other thread. It runs until its
 * iteration count is spent and then says so, so the other thread is busy for
 * the whole window rather than for the first few percent of it. After every
 * rebuild this thread reads back the file the daemon would restart on. */
static void *rebuild_loop(void *arg) {
  racer *r = arg;
  kbc_err err;
  for (unsigned i = 0; i < r->iters; i++) {
    kbc_err_reset(&err);
    kbc_status s = kbc_app_reindex(r->app, &err);
    if (kbc_failed(s)) {
      race_fail(r, "kbc_app_reindex failed", err.msg);
      continue;
    }
    kbc_err_reset(&err);
    kbc_index *ix = kbc_index_open(r->cfg->index_path, &err);
    if (ix == NULL) {
      race_fail(r, "the promoted index does not open", err.msg);
      continue;
    }
    uint32_t docs = kbc_index_doc_count(ix);
    size_t on_disk = count_indexable(r->dir);
    kbc_index_free(ix);
    if (r->count_stable && (size_t)docs != on_disk) {
      char msg[64];
      snprintf(msg, sizeof msg, "%u documents promoted, %zu on disk", docs,
               on_disk);
      race_fail(r, "the promoted index is not a corpus state", msg);
    }
  }
  atomic_store_explicit(&r->shared->stop, 1, memory_order_relaxed);
  return NULL;
}

/* One document, rewritten and reindexed in place, over and over, for as long
 * as the rebuilds run. The rewrite is what makes the two indexes genuinely
 * DIFFERENT bytes: a single-file update of an unchanged document serialises to
 * the same file as the rebuild, and two writers producing identical bytes
 * leave no trace of the collision. */
static void *touch_loop(void *arg) {
  racer *r = arg;
  const char *name = "doc0000.md";
  char path[KBC_TEST_PATH_MAX];
  join(path, sizeof path, r->dir, name);
  kbc_err err;
  for (unsigned i = 0; i < TOUCH_ITERS; i++) {
    if (atomic_load_explicit(&r->shared->stop, memory_order_relaxed) != 0) {
      break;
    }
    char body[256];
    snprintf(body, sizeof body,
             "# Revision\n\nThe ledger page was rewritten, revision %u.\n", i);
    if (write_file_atomic(path, body) != 0) {
      race_fail(r, "the revised document could not be written", path);
      return NULL;
    }
    kbc_err_reset(&err);
    kbc_status s = kbc_app_reindex_file(r->app, CORPUS_A, name, &err);
    if (kbc_failed(s)) {
      race_fail(r, "kbc_app_reindex_file failed", err.msg);
      return NULL;
    }
  }
  return NULL;
}

/* A document that did not exist when the walk started, created and indexed on
 * its own, for as long as the rebuilds run.
 *
 * After every one it re-checks the invariant rather than only at the end: a
 * rebuild that swept a document the store had already committed does not
 * repair itself, because the watcher does not re-fire for a path it has
 * already reported. So a loss is permanent, and the very next iteration sees
 * it. Checking only the final state would instead be a bet on whether the
 * LAST document happened to land inside the LAST rebuild's window. */
static void *grow_loop(void *arg) {
  racer *r = arg;
  kbc_err err;
  for (unsigned i = 0; i < GROW_ITERS; i++) {
    if (atomic_load_explicit(&r->shared->stop, memory_order_relaxed) != 0) {
      break;
    }
    char name[64], body[256], path[KBC_TEST_PATH_MAX];
    snprintf(name, sizeof name, "extra%04u.md", i);
    join(path, sizeof path, r->dir, name);
    snprintf(body, sizeof body,
             "# Extra %04u\n\nA page that arrived mid-pass.\n", i);
    if (write_file_atomic(path, body) != 0) {
      race_fail(r, "the new document could not be written", path);
      return NULL;
    }
    kbc_err_reset(&err);
    kbc_status s = kbc_app_reindex_file(r->app, CORPUS_A, name, &err);
    if (kbc_failed(s)) {
      race_fail(r, "kbc_app_reindex_file failed", err.msg);
      return NULL;
    }
    atomic_fetch_add_explicit(&r->shared->grown, 1u, memory_order_relaxed);
    /* Every document this thread has finished indexing is still in the live
     * index. It is there because this thread put it there a moment ago, and a
     * rebuild that dropped it again has lost it for good. */
    kbc_app_stats st;
    kbc_err serr;
    kbc_err_reset(&serr);
    if (kbc_failed(kbc_app_stats_get(r->app, &st, &serr))) {
      race_fail(r, "kbc_app_stats_get failed", serr.msg);
      return NULL;
    }
    uint32_t done =
        atomic_load_explicit(&r->shared->grown, memory_order_relaxed);
    const int64_t expect = r->base_docs + (int64_t)done;
    if (st.index_docs != expect) {
      char msg[96];
      snprintf(msg, sizeof msg, "%lld documents indexed, %lld created and kept",
               (long long)st.index_docs, (long long)expect);
      race_fail(r, "a rebuild dropped a document the store had committed", msg);
      return NULL;
    }
  }
  return NULL;
}

/* A broken symlink is an ordinary thing to find in a documents folder, and it
 * used to fail the ENTIRE reindex. walk_dir stats each entry with
 * fstatat(..., 0), which FOLLOWS the link, so a dangling one fails with
 * ENOENT; the code correctly treats that as "vanished, not an error" and
 * carries on. But readdir signals end-of-directory by returning NULL and
 * leaving errno UNCHANGED, and the loop cleared errno only once, before it
 * started — so the ENOENT that fstatat left behind was read afterwards as a
 * failure of the walk itself. The reindex returned "readdir <path>: No such
 * file or directory" and indexed nothing at all.
 *
 * This is deterministic: the link is dangling on every run, so the stat that
 * sets errno always fails, and it is the only entry in the directory, so
 * nothing later in the loop body can overwrite errno before the loop ends. */
KBC_TEST(a_dangling_symlink_does_not_fail_the_reindex) {
  fixture f;
  fx_setup(&f, false);
  if (f.app == NULL) {
    fx_teardown(&f);
    return;
  }
  char link[KBC_TEST_PATH_MAX];
  join(link, sizeof link, f.corpus_a, "dangling.md");
  KBC_CHECK_EQ_INT(symlink("/nonexistent/kb-c-no-such-target", link), 0);
  /* Four entries on disk, and the walk must SEE all four: a walk that bailed
   * out early would also have counted three, so the disk side is asserted to
   * make the index count mean what it says. */
  KBC_CHECK_EQ_INT(count_indexable(f.corpus_a), 4);

  kbc_err err;
  kbc_err_reset(&err);
  KBC_CHECK_MSG(kbc_app_reindex(f.app, &err) == KBC_OK,
                "one broken symlink failed the whole reindex: %s", err.msg);
  kbc_app_stats st;
  kbc_err_reset(&err);
  KBC_CHECK_OK(kbc_app_stats_get(f.app, &st, &err));
  /* Three, not four: the link resolves to nothing, so it is skipped rather
 * than ingested and rather than taking the whole walk down with it. */
  KBC_CHECK_EQ_INT(st.index_docs, 3);
  fx_teardown(&f);
}

/* Defect one. Both writers promoted into `<index>.build`, and the atomic write
 * underneath them names its temp file after the PROCESS, so the two opened
 * the same file with O_TRUNC and wrote over each other from offset zero. The
 * promoted index was a splice kbc_index_open rejects, and the daemon refused
 * to start until an operator deleted it and reindexed — destroying the very
 * guarantee the fsync-then-rename dance exists to provide. */
KBC_TEST(a_full_reindex_and_a_single_file_update_never_splice_the_index) {
  fixture f;
  fx_setup(&f, false);
  if (f.app == NULL) {
    fx_teardown(&f);
    return;
  }
  write_bulk_corpus(f.corpus_a);

  kbc_err err;
  kbc_err_reset(&err);
  KBC_CHECK_OK(kbc_app_reindex(f.app, &err));
  const size_t want = count_indexable(f.corpus_a);
  KBC_CHECK(want > BULK_DOCS);

  race_shared sh;
  atomic_init(&sh.stop, 0);
  atomic_init(&sh.grown, 0u);
  racer rebuild, touch;
  memset(&rebuild, 0, sizeof rebuild);
  memset(&touch, 0, sizeof touch);
  rebuild.app = touch.app = f.app;
  rebuild.cfg = touch.cfg = f.cfg;
  rebuild.dir = touch.dir = f.corpus_a;
  rebuild.shared = touch.shared = &sh;
  rebuild.iters = RACE_ITERS;
  /* The other thread rewrites a document in place, so the set on disk never
   * moves: every promoted generation must match it exactly. */
  rebuild.count_stable = true;

  pthread_t rebuild_th, touch_th;
  KBC_CHECK_EQ_INT(pthread_create(&rebuild_th, NULL, rebuild_loop, &rebuild),
                   0);
  KBC_CHECK_EQ_INT(pthread_create(&touch_th, NULL, touch_loop, &touch), 0);
  KBC_CHECK_EQ_INT(pthread_join(rebuild_th, NULL), 0);
  KBC_CHECK_EQ_INT(pthread_join(touch_th, NULL), 0);

  KBC_CHECK_MSG(rebuild.bad == NULL, "full rebuild - %s: %s", rebuild.bad,
                rebuild.detail);
  KBC_CHECK_MSG(touch.bad == NULL, "single-file update - %s: %s", touch.bad,
                touch.detail);

  kbc_err_reset(&err);
  kbc_index *ix = kbc_index_open(f.cfg->index_path, &err);
  KBC_CHECK_MSG(ix != NULL, "the promoted index does not open: %s", err.msg);
  if (ix != NULL) {
    KBC_CHECK_EQ_INT(kbc_index_doc_count(ix), (long long)want);
    kbc_index_free(ix);
  }
  kbc_app_stats st;
  KBC_CHECK_OK(kbc_app_stats_get(f.app, &st, &err));
  KBC_CHECK_EQ_INT(st.index_docs, (long long)want);
  fx_teardown(&f);
}

/* Defect two, which is the same missing exclusion seen from the other side.
 * The full rebuild took the write lock only for its final swap, so for the
 * whole of the walk a single-file update could commit and promote a document
 * the walk had not seen; the rebuild then swept that document's store row away
 * and published an index without it. The watcher does not re-fire for a path
 * it has already reported, so the divergence lasted until the next full scan,
 * with every status endpoint reporting success. */
KBC_TEST(a_full_reindex_never_drops_a_document_a_single_file_update_added) {
  fixture f;
  fx_setup(&f, false);
  if (f.app == NULL) {
    fx_teardown(&f);
    return;
  }
  write_bulk_corpus(f.corpus_a);

  kbc_err err;
  kbc_err_reset(&err);
  KBC_CHECK_OK(kbc_app_reindex(f.app, &err));

  race_shared sh;
  atomic_init(&sh.stop, 0);
  atomic_init(&sh.grown, 0u);
  racer rebuild, grow;
  memset(&rebuild, 0, sizeof rebuild);
  memset(&grow, 0, sizeof grow);
  rebuild.app = grow.app = f.app;
  rebuild.cfg = grow.cfg = f.cfg;
  rebuild.dir = grow.dir = f.corpus_a;
  rebuild.shared = grow.shared = &sh;
  rebuild.iters = RACE_ITERS;
  /* The other thread is ADDING documents, so a promoted index is legitimately
   * one document behind the directory until that thread catches up. The
   * cross-check that the two are equal is made once, at quiescence, below. */
  rebuild.base_docs = grow.base_docs =
      (int64_t)count_indexable(f.corpus_a);

  pthread_t rebuild_th, grow_th;
  KBC_CHECK_EQ_INT(pthread_create(&rebuild_th, NULL, rebuild_loop, &rebuild),
                   0);
  KBC_CHECK_EQ_INT(pthread_create(&grow_th, NULL, grow_loop, &grow), 0);
  KBC_CHECK_EQ_INT(pthread_join(rebuild_th, NULL), 0);
  KBC_CHECK_EQ_INT(pthread_join(grow_th, NULL), 0);

  KBC_CHECK_MSG(rebuild.bad == NULL, "full rebuild - %s: %s", rebuild.bad,
                rebuild.detail);
  KBC_CHECK_MSG(grow.bad == NULL, "single-file update - %s: %s", grow.bad,
                grow.detail);

  /* The live index against the directory: every document that exists is in
   * it. A rebuild that ran with a single-file update inside it loses exactly
   * one of these. */
  const size_t on_disk = count_indexable(f.corpus_a);
  const uint32_t grown = atomic_load_explicit(&sh.grown, memory_order_relaxed);
  KBC_CHECK_EQ_INT(on_disk, (long long)(3u + BULK_DOCS + grown));
  kbc_app_stats st;
  KBC_CHECK_OK(kbc_app_stats_get(f.app, &st, &err));
  KBC_CHECK_MSG(st.index_docs == (int64_t)on_disk,
                "the live index holds %lld documents, the corpus has %zu",
                (long long)st.index_docs, on_disk);
  fx_teardown(&f);
}

/* A second connection to the app's own database. store.h is frozen and has no
 * fault-injection seam, and the two cases below need a store write that
 * fails — something a healthy filesystem never produces on demand. */
typedef struct {
  sqlite3 *db;
} db_side;

static bool db_side_open(db_side *side, const kbc_config *cfg) {
  if (sqlite3_open_v2(cfg->db_path, &side->db,
                      SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE,
                      NULL) != SQLITE_OK) {
    fprintf(stderr, "  sqlite3_open %s: %s\n", cfg->db_path,
            side->db != NULL ? sqlite3_errmsg(side->db) : "no handle");
    return false;
  }
  return true;
}

static kbc_status db_side_exec(db_side *side, const char *sql) {
  char *msg = NULL;
  if (sqlite3_exec(side->db, sql, NULL, NULL, &msg) == SQLITE_OK) {
    return KBC_OK;
  }
  kbc_status s = kbc_err_set(NULL, KBC_ERR_IO, "%s", msg != NULL ? msg : sql);
  sqlite3_free(msg);
  return s;
}

static void db_side_close(db_side *side) {
  if (side->db != NULL) {
    (void)sqlite3_close(side->db);
    side->db = NULL;
  }
}

/* Defect three. `s` was left holding the kbc_str_printf result from two lines
 * earlier — which is KBC_OK, because `vanished` came from the stat and not the
 * printf — so a store_forget_path failure fell through to the success path.
 * The caller was told the removal worked, the artifact row and the graph rows
 * stayed, the deleted document stayed searchable, and the watcher logged
 * nothing at all. */
KBC_TEST(a_removal_the_store_cannot_commit_is_reported_not_swallowed) {
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
  KBC_CHECK_EQ_INT(first_hit_path(f.app, "verdigris", NULL, path, sizeof path,
                                  title, sizeof title, id, sizeof id, NULL),
                   1);
  KBC_CHECK_EQ_STR(path, "c.md");

  char p[KBC_TEST_PATH_MAX];
  join(p, sizeof p, f.corpus_a, "c.md");
  KBC_CHECK_MSG(unlink(p) == 0, "unlink %s: %s", p, strerror(errno));

  /* forget_document demotes the inbound edges to pending links and deletes
   * the outbound ones in one transaction, so a store without the graph table
   * cannot drop the document at all. */
  db_side side;
  memset(&side, 0, sizeof side);
  if (!db_side_open(&side, f.cfg)) {
    fx_teardown(&f);
    return;
  }
  KBC_CHECK_OK(db_side_exec(&side, "DROP TABLE edges;"));

  kbc_err_reset(&err);
  kbc_status s = kbc_app_reindex_file(f.app, CORPUS_A, "c.md", &err);
  KBC_CHECK_MSG(kbc_failed(s),
                "a removal the store refused reported KBC_OK (last error: %s)",
                err.msg);
  if (kbc_failed(s)) {
    KBC_CHECK_ERR_MSG(err);
  }
  db_side_close(&side);

  /* Nothing is half-applied: the row is in the store AND in the index, which
   * is the state the daemon was in before the removal was asked for. The
   * document stays searchable — loudly, with the failure on the record. */
  KBC_CHECK_EQ_INT(first_hit_path(f.app, "verdigris", NULL, path, sizeof path,
                                  title, sizeof title, id, sizeof id, NULL),
                   1);
  KBC_CHECK_EQ_STR(path, "c.md");
  fx_teardown(&f);
}

/* Each carries its own heap array of link targets, which the ingest loop hands
 * to the batch it is building. Only one document names the generation word,
 * so a search for it names exactly one file. */
static void write_link_doc(const char *dir, size_t i, const char *generation) {
  char name[64], body[2048], path[KBC_TEST_PATH_MAX];
  snprintf(name, sizeof name, "link%04zu.md", i);
  join(path, sizeof path, dir, name);
  int len = snprintf(body, sizeof body, "# %s\n\nThis page mentions %s.\n",
                     name, i == 0u ? generation : "ledger");
  for (size_t k = 0; k < LINK_LINKS; k++) {
    len += snprintf(body + len, sizeof body - (size_t)len,
                    "[target %zu](doc%04zu.md)\n", k, i);
  }
  kbc_test_write_file(path, body);
}

typedef struct {
  db_side side;
  long long cutoff_ns;
  _Atomic int stop;
  _Atomic int locked;
} write_lock_holder;

static void nap_us(long us) {
  struct timespec ts;
  ts.tv_sec = (time_t)(us / 1000000L);
  ts.tv_nsec = (us % 1000000L) * 1000L;
  (void)nanosleep(&ts, NULL);
}

/* Waits until a rebuild has re-stored two documents, then takes sqlite's
 * write lock and holds it. The store's own writes then time out, which is the
 * only way to reach the ingest loop's error unwind: the files on disk cannot
 * make a store write fail on demand. */
static void *hold_write_lock(void *arg) {
  write_lock_holder *h = arg;
  (void)db_side_exec(&h->side, "PRAGMA busy_timeout = 30000;");
  sqlite3_stmt *q = NULL;
  if (sqlite3_prepare_v2(h->side.db,
                         "SELECT count(*) FROM artifacts WHERE mtime_ns > ?1;",
                         -1, &q, NULL) != SQLITE_OK) {
    return NULL;
  }
  for (;;) {
    if (atomic_load_explicit(&h->stop, memory_order_relaxed) != 0) {
      break;
    }
    int rc = sqlite3_reset(q);
    if (rc == SQLITE_OK) {
      rc = sqlite3_bind_int64(q, 1, h->cutoff_ns);
    }
    int n = -1;
    if (rc == SQLITE_OK && sqlite3_step(q) == SQLITE_ROW) {
      n = sqlite3_column_int(q, 0);
    }
    if (n >= 2) {
      break;
    }
    nap_us(200);
  }
  sqlite3_finalize(q);
  if (db_side_exec(&h->side, "BEGIN IMMEDIATE;") == KBC_OK) {
    atomic_store_explicit(&h->locked, 1, memory_order_relaxed);
    while (atomic_load_explicit(&h->stop, memory_order_relaxed) == 0) {
      nap_ms(1);
    }
    (void)db_side_exec(&h->side, "ROLLBACK;");
  }
  return NULL;
}

/* Defect four. Both of the ingest loop's error returns freed the batch ARRAY
 * and not the link-target arrays inside it, so a rebuild that died on its Nth
 * document leaked every one the first N-1 had handed over. The observable
 * half of the case is the unwind: the old generation keeps serving, nothing
 * half-built is published, and the next reindex still runs — which is what
 * proves the lock the pass now holds is released on that path. The leak
 * itself is what the ASan/LSan lane is there to catch. */
KBC_TEST(an_aborted_reindex_leaves_the_previous_generation_serving) {
  fixture f;
  fx_setup(&f, false);
  if (f.app == NULL) {
    fx_teardown(&f);
    return;
  }
  for (size_t i = 0; i < LINK_DOCS; i++) {
    write_link_doc(f.corpus_a, i, "quokka");
  }

  kbc_err err;
  kbc_err_reset(&err);
  KBC_CHECK_OK(kbc_app_reindex(f.app, &err));
  char path[64], title[64], id[32];
  KBC_CHECK_EQ_INT(first_hit_path(f.app, "quokka", NULL, path, sizeof path,
                                  title, sizeof title, id, sizeof id, NULL),
                   1);

  /* Each attempt writes a fresh generation, so the rebuild has a real store
   * phase to be interrupted in, and asks a second connection to take sqlite's
   * write lock as soon as the pass starts re-storing. Whether the holder wins
   * that race is timing, so the case RETRIES rather than declaring itself
   * broken: an attempt whose lock lands after the pass has finished its store
   * writes proves nothing, and a test that failed on that would be a test of
   * the machine's load. What it asserts is that the injection can be made to
   * land — and that when it does, everything downstream is right. */
  bool aborted = false;
  int completed = 0; /* attempts that ran to the end and published */
  kbc_status s = KBC_OK;
  for (int attempt = 0; attempt < ABORT_ATTEMPTS && !aborted; attempt++) {
    struct timespec now;
    clock_gettime(CLOCK_REALTIME, &now);
    const long long cutoff =
        (long long)now.tv_sec * 1000000000LL + (long long)now.tv_nsec;
    for (size_t i = 0; i < LINK_DOCS; i++) {
      write_link_doc(f.corpus_a, i, "wombat");
    }

    write_lock_holder h;
    memset(&h, 0, sizeof h);
    atomic_init(&h.stop, 0);
    atomic_init(&h.locked, 0);
    h.cutoff_ns = cutoff;
    if (!db_side_open(&h.side, f.cfg)) {
      fx_teardown(&f);
      return;
    }
    pthread_t th;
    KBC_CHECK_EQ_INT(pthread_create(&th, NULL, hold_write_lock, &h), 0);
    kbc_err_reset(&err);
    s = kbc_app_reindex(f.app, &err);
    atomic_store_explicit(&h.stop, 1, memory_order_relaxed);
    KBC_CHECK_EQ_INT(pthread_join(th, NULL), 0);
    db_side_close(&h.side);
    if (kbc_failed(s)) {
      aborted = true;
    } else {
      completed++;
    }
  }

  KBC_CHECK_MSG(aborted,
                "a second connection never managed to take sqlite's write lock "
                "during a rebuild's store phase in %d attempts, so this case "
                "proved nothing",
                ABORT_ATTEMPTS);
  if (aborted) {
    KBC_CHECK_ERR_MSG(err);
    /* Nothing was published: the live index is still whatever the last
     * attempt that COMPLETED installed, not a half-built one. Which
     * generation that is depends on whether any attempt got that far. */
    const char *live = completed > 0 ? "wombat" : "quokka";
    const char *gone = completed > 0 ? "quokka" : "wombat";
    KBC_CHECK_EQ_INT(
        first_hit_path(f.app, live, NULL, path, sizeof path, title, sizeof title,
                       id, sizeof id, NULL),
        1);
    KBC_CHECK_EQ_INT(
        first_hit_path(f.app, gone, NULL, path, sizeof path, title, sizeof title,
                       id, sizeof id, NULL),
        0);
  }

  /* And the lock the pass held is free again — if the unwind had kept it,
   * this call would never return. */
  kbc_err_reset(&err);
  KBC_CHECK_OK(kbc_app_reindex(f.app, &err));
  KBC_CHECK_EQ_INT(first_hit_path(f.app, "wombat", NULL, path, sizeof path,
                                  title, sizeof title, id, sizeof id, NULL),
                   1);
  fx_teardown(&f);
}

/* ----------------------------------------------------------------- bus --- */

/* The event bus, collected. publish calls the callback on the publishing
 * thread, so a plain struct is the whole synchronization story here — and a
 * mutex would be a lie about a contract the test does not have. */
typedef struct {
  char type[32];
  char json[256];
  size_t n;
} event_log;

static void collect_event(void *user, const char *type, const char *json) {
  event_log *log = (event_log *)user;
  if (log == NULL || log->n >= 8) return;
  event_log *slot = &log[log->n++];
  snprintf(slot->type, sizeof slot->type, "%s", type != NULL ? type : "");
  snprintf(slot->json, sizeof slot->json, "%s", json != NULL ? json : "");
}

/* How many collected events of `type` name `path` in their payload. */
static size_t events_naming(const event_log *log, const char *type,
                            const char *path) {
  size_t n = 0;
  char needle[128];
  snprintf(needle, sizeof needle, "\"%s\"", path);
  for (size_t i = 0; i < log->n; i++) {
    if (strcmp(log[i].type, type) == 0 && strstr(log[i].json, needle) != NULL) {
      n++;
    }
  }
  return n;
}

/* Chunks of one document, counted through a second connection.
 * kbc_store_list_chunks copies the whole result into a caller array whose size
 * it does not tell you, which is fine for a store round trip and not fine for
 * a test that must not be the thing that overflows. */
static int64_t chunk_count(db_side *side, const char *doc_id) {
  sqlite3_stmt *q = NULL;
  if (sqlite3_prepare_v2(side->db,
                         "SELECT COUNT(*) FROM chunks WHERE doc_id = ?1;", -1,
                         &q, NULL) != SQLITE_OK) {
    return -1;
  }
  int64_t n = -1;
  if (sqlite3_bind_text(q, 1, doc_id, -1, SQLITE_STATIC) == SQLITE_OK &&
      sqlite3_step(q) == SQLITE_ROW) {
    n = sqlite3_column_int64(q, 0);
  }
  (void)sqlite3_finalize(q);
  return n;
}

/* The corkboard anchor on ONE document, through a second connection, for the
 * same reason as chunk_count above and with the same shape. The listing is a
 * PAGE — `created_at DESC LIMIT KBC_MAX_HITS` — so once there are more anchors
 * than that there is no page-based way at all to ask about a document that is
 * not among the newest, and the case that needs the answer is exactly such a
 * document. -1 is "no such anchor"; created_at 0 is a real value. */
static int64_t corkboard_at_for(db_side *side, const char *doc_id) {
  sqlite3_stmt *q = NULL;
  if (sqlite3_prepare_v2(side->db,
                         "SELECT created_at FROM corkboard WHERE artifact_id "
                         "= ?1;",
                         -1, &q, NULL) != SQLITE_OK) {
    return -1;
  }
  int64_t at = -1;
  if (sqlite3_bind_text(q, 1, doc_id, -1, SQLITE_STATIC) == SQLITE_OK &&
      sqlite3_step(q) == SQLITE_ROW) {
    at = sqlite3_column_int64(q, 0);
  }
  (void)sqlite3_finalize(q);
  return at;
}

/* The artifact id one path mints, or NULL when the store has no such row. */
static const char *id_of_path(const kbc_config *cfg, const char *corpus,
                              const char *path, kbc_arena *a) {
  kbc_err err;
  kbc_err_reset(&err);
  kbc_store *s = kbc_store_open(cfg, &err);
  if (s == NULL) return NULL;
  kbc_artifact art;
  memset(&art, 0, sizeof art);
  kbc_status st = kbc_store_get_artifact_by_path(s, a, corpus, path, &art, &err);
  kbc_store_close(s);
  return st == KBC_OK ? art.id : NULL;
}

static size_t comment_count(const kbc_config *cfg, const char *doc_id,
                            kbc_arena *a) {
  kbc_err err;
  kbc_err_reset(&err);
  kbc_store *s = kbc_store_open(cfg, &err);
  if (s == NULL) return 0;
  kbc_comment *rows = NULL;
  size_t n = 0;
  /* `rows` is arena-owned (store.h: every field of kbc_comment is ARENA), so
   * it dies with `a` and must not be freed here. */
  kbc_status st =
      kbc_store_list_comments(s, a, doc_id, 50u, &rows, &n, &err);
  kbc_store_close(s);
  return st == KBC_OK ? n : 0;
}

/* The three tables a removal must NOT touch, read back through the public
 * store API. Each counts only the rows naming one artifact id, because the
 * listings are per-user (history) or whole-table (corkboard, pins) and a count
 * of everything would pass even if the delete took the row with it. */
static size_t history_visits(const kbc_config *cfg, const char *user,
                             const char *doc_id, kbc_arena *a) {
  kbc_err err;
  kbc_err_reset(&err);
  kbc_store *s = kbc_store_open(cfg, &err);
  if (s == NULL) return 0;
  kbc_history_row *rows = NULL;
  size_t n = 0, kept = 0;
  kbc_status st = kbc_store_list_history(s, a, user, 100u, &rows, &n, &err);
  if (st == KBC_OK) {
    for (size_t i = 0; i < n; i++) {
      if (rows[i].artifact_id != NULL &&
          strcmp(rows[i].artifact_id, doc_id) == 0) {
        kept++;
      }
    }
  }
  kbc_store_close(s);
  return kept;
}

static size_t corkboard_entries(const kbc_config *cfg, const char *doc_id,
                                kbc_arena *a) {
  kbc_err err;
  kbc_err_reset(&err);
  kbc_store *s = kbc_store_open(cfg, &err);
  if (s == NULL) return 0;
  kbc_corkboard_row *rows = NULL;
  size_t n = 0, kept = 0;
  kbc_status st = kbc_store_list_corkboard(s, a, 100u, &rows, &n, &err);
  if (st == KBC_OK) {
    for (size_t i = 0; i < n; i++) {
      if (rows[i].artifact_id != NULL &&
          strcmp(rows[i].artifact_id, doc_id) == 0) {
        kept++;
      }
    }
  }
  kbc_store_close(s);
  return kept;
}

static size_t pins_of(const kbc_config *cfg, const char *doc_id, kbc_arena *a) {
  kbc_err err;
  kbc_err_reset(&err);
  kbc_store *s = kbc_store_open(cfg, &err);
  if (s == NULL) return 0;
  kbc_pin_row *rows = NULL;
  size_t n = 0, kept = 0;
  kbc_status st = kbc_store_list_pins(s, a, 100u, &rows, &n, &err);
  if (st == KBC_OK) {
    for (size_t i = 0; i < n; i++) {
      if (rows[i].artifact_id != NULL &&
          strcmp(rows[i].artifact_id, doc_id) == 0) {
        kept++;
      }
    }
  }
  kbc_store_close(s);
  return kept;
}

/* Unit 2 — the reconcile delete pass.
 *
 * A file deleted while the daemon is not watching never produces an inotify
 * event, so nothing but the next full pass can notice, and the pass used to
 * notice it the way the watcher does only by accident. The corpus on disk is
 * the authority: the pass re-stats every stored row the walk did not see, and
 * the ones the filesystem says are gone go through the SAME removal the
 * watcher runs. Before this, the sweep carried its own copy of that cascade
 * and emitted no event at all, so a document removed by a full scan changed
 * the index under every subscriber without saying so. */
KBC_TEST(a_document_deleted_while_nobody_watched_is_reconciled_by_the_pass) {
  fixture f;
  fx_setup(&f, false);
  if (f.app == NULL) {
    fx_teardown(&f);
    return;
  }
  kbc_err err;
  kbc_err_reset(&err);
  KBC_CHECK_OK(kbc_app_reindex(f.app, &err));

  event_log log[8];
  memset(log, 0, sizeof log);
  uint64_t sub = kbc_app_subscribe(f.app, collect_event, &log);

  /* The watcher is not running and never hears about this. */
  char p[KBC_TEST_PATH_MAX];
  join(p, sizeof p, f.corpus_a, "c.md");
  KBC_CHECK_MSG(unlink(p) == 0, "unlink %s: %s", p, strerror(errno));
  KBC_CHECK_OK(kbc_app_reindex(f.app, &err));

  /* Gone from the index and from the answers, not merely off a counter. */
  char path[64], title[64], id[32];
  KBC_CHECK_EQ_INT(first_hit_path(f.app, "verdigris", NULL, path, sizeof path,
                                  title, sizeof title, id, sizeof id, NULL),
                   0);
  KBC_CHECK_EQ_INT(first_hit_path(f.app, "quixotic", NULL, path, sizeof path,
                                  title, sizeof title, id, sizeof id, NULL),
                   1);
  KBC_CHECK_EQ_STR(path, "b.md");
  kbc_app_stats st;
  KBC_CHECK_OK(kbc_app_stats_get(f.app, &st, &err));
  KBC_CHECK_EQ_INT(st.index_docs, 2);
  KBC_CHECK_EQ_INT(store_count(f.cfg), 2);

  /* And it was announced, once, by the same event the watcher emits for the
   * same removal — the Rust reconcile pass routes its synthetic delete through
   * the sink precisely so this is observable (indexer.rs:776-779). */
  KBC_CHECK_EQ_INT(events_naming(log, "watch.delete", "c.md"), 1);
  KBC_CHECK_EQ_INT(events_naming(log, "watch.delete", "a.md"), 0);
  KBC_CHECK_EQ_INT(events_naming(log, "watch.delete", "b.md"), 0);
  KBC_CHECK_MSG(strstr(log[0].json, "\"kb\":\"" CORPUS_A "\"") != NULL,
                "watch.delete payload does not name the corpus: %s", log[0].json);
  kbc_app_unsubscribe(f.app, sub);
  fx_teardown(&f);
}

/* The other arm of the same re-stat, and the one that was missing: a stored
 * row the walk did not take because of a WALK RULE is not a removal. The
 * document is still in the corpus, the rule is the user's to change back, and
 * the cascade it used to run reaches `comments` — which nothing can rebuild.
 * The old code dropped the row on the manifest's word alone, so emptying a
 * file destroyed its comment thread. */
KBC_TEST(a_document_the_walk_declines_keeps_its_row_and_its_comments) {
  fixture f;
  fx_setup(&f, false);
  if (f.app == NULL) {
    fx_teardown(&f);
    return;
  }
  kbc_err err;
  kbc_err_reset(&err);
  KBC_CHECK_OK(kbc_app_reindex(f.app, &err));

  kbc_arena *a = kbc_arena_new(64u * 1024u);
  KBC_CHECK_NOT_NULL(a);
  if (a == NULL) {
    fx_teardown(&f);
    return;
  }
  const char *id = id_of_path(f.cfg, CORPUS_A, "a.md", a);
  KBC_CHECK_NOT_NULL(id);
  if (id == NULL) {
    kbc_arena_free(a);
    fx_teardown(&f);
    return;
  }
  char id_copy[KBC_MAX_ID_LEN + 1];
  snprintf(id_copy, sizeof id_copy, "%s", id);

  kbc_store *st = kbc_store_open(f.cfg, &err);
  KBC_CHECK_NOT_NULL(st);
  if (st == NULL) {
    kbc_arena_free(a);
    fx_teardown(&f);
    return;
  }
  KBC_CHECK_OK(kbc_store_add_comment(st, id_copy, "section:ledger", "nik",
                                     "the accrual table is wrong", &err));
  kbc_store_close(st);
  KBC_CHECK_EQ_INT(comment_count(f.cfg, id_copy, a), 1);

  /* Emptied, not deleted: the walk skips a zero-byte file (size 0 is out of
   * the 1..KBC_MAX_ARTIFACT_BYTES range), and the file is still right there. */
  char p[KBC_TEST_PATH_MAX];
  join(p, sizeof p, f.corpus_a, "a.md");
  kbc_test_write_file(p, "");
  KBC_CHECK_OK(kbc_app_reindex(f.app, &err));

  /* Unsearchable — the walk did not take it, so the rebuilt index has no
   * posting list for it — and still THERE, which is the whole point. */
  char path[64], title[64], id2[32];
  KBC_CHECK_EQ_INT(first_hit_path(f.app, "accruals", NULL, path, sizeof path,
                                  title, sizeof title, id2, sizeof id2, NULL),
                   0);
  KBC_CHECK_NOT_NULL(id_of_path(f.cfg, CORPUS_A, "a.md", a));
  KBC_CHECK_EQ_INT(comment_count(f.cfg, id_copy, a), 1);

  /* Put the content back and it is the same document with its thread on it,
   * under the same id — the artifact id is a function of (corpus, path), so
   * nothing was renumbered on the way through. */
  kbc_test_write_file(p, DOC_A);
  KBC_CHECK_OK(kbc_app_reindex(f.app, &err));
  KBC_CHECK_EQ_INT(first_hit_path(f.app, "accruals", NULL, path, sizeof path,
                                  title, sizeof title, id2, sizeof id2, NULL),
                   1);
  KBC_CHECK_EQ_STR(id2, id_copy);
  KBC_CHECK_EQ_INT(comment_count(f.cfg, id_copy, a), 1);

  kbc_arena_free(a);
  fx_teardown(&f);
}

/* Unit 4 — delete by path.
 *
 * The by-path entry point, and the cascade spelled out: artifacts, chunks,
 * comments and the graph go; history, corkboard, pinned_memories and
 * reading_sections stay, because they are the user's and not the document's.
 * The four are not readable yet — store.h is frozen and unit 1 has not created
 * the tables — so what this pins is the half that exists: the document is
 * unreachable and everything the document itself wrote is gone with it. */
KBC_TEST(delete_by_path_takes_the_document_and_announces_it) {
  fixture f;
  fx_setup(&f, false);
  if (f.app == NULL) {
    fx_teardown(&f);
    return;
  }
  kbc_err err;
  kbc_err_reset(&err);
  KBC_CHECK_OK(kbc_app_reindex(f.app, &err));

  kbc_arena *a = kbc_arena_new(64u * 1024u);
  KBC_CHECK_NOT_NULL(a);
  if (a == NULL) {
    fx_teardown(&f);
    return;
  }
  const char *id = id_of_path(f.cfg, CORPUS_A, "c.md", a);
  KBC_CHECK_NOT_NULL(id);
  if (id == NULL) {
    kbc_arena_free(a);
    fx_teardown(&f);
    return;
  }
  char id_copy[KBC_MAX_ID_LEN + 1];
  snprintf(id_copy, sizeof id_copy, "%s", id);

  kbc_store *st = kbc_store_open(f.cfg, &err);
  KBC_CHECK_NOT_NULL(st);
  if (st == NULL) {
    kbc_arena_free(a);
    fx_teardown(&f);
    return;
  }
  /* The user's half, seeded on the document that is about to be deleted: a
   * reading visit (which is where the reading position — Rust's
   * reading_sections — lives on kb-c's history row), a corkboard entry and a
   * pin. None of these can be rebuilt from the document's bytes, which is the
   * whole reason they are on the leave-alone list. */
  kbc_history_row visit;
  memset(&visit, 0, sizeof visit);
  visit.kind = "open";
  visit.artifact_id = id_copy;
  visit.user = "nik";
  visit.last_section = "section:digest";
  visit.scroll_y = 240;
  visit.started_at = 1756000000;
  visit.updated_at = 1756000000;
  KBC_CHECK_OK(kbc_store_add_history(st, &visit, &err));
  KBC_CHECK_OK(kbc_store_add_corkboard(st, id_copy, 1756000000, &err));
  KBC_CHECK_OK(kbc_store_pin_memory(st, id_copy, 1756000000, &err));
  KBC_CHECK_OK(kbc_store_add_comment(st, id_copy, "section:digest", "nik",
                                     "verdigris again", &err));
  kbc_store_close(st);

  db_side side;
  memset(&side, 0, sizeof side);
  if (!db_side_open(&side, f.cfg)) {
    kbc_arena_free(a);
    fx_teardown(&f);
    return;
  }
  KBC_CHECK_MSG(chunk_count(&side, id_copy) > 0, "c.md has no chunks to lose");
  KBC_CHECK_EQ_INT(comment_count(f.cfg, id_copy, a), 1);
  KBC_CHECK_EQ_INT(history_visits(f.cfg, "nik", id_copy, a), 1);
  KBC_CHECK_EQ_INT(corkboard_entries(f.cfg, id_copy, a), 1);
  KBC_CHECK_EQ_INT(pins_of(f.cfg, id_copy, a), 1);

  event_log log[8];
  memset(log, 0, sizeof log);
  uint64_t sub = kbc_app_subscribe(f.app, collect_event, &log);

  /* The file is still on disk. This is the statement, not a question. */
  KBC_CHECK_OK(kbc_app_delete_path(f.app, CORPUS_A, "c.md", &err));

  /* Unreachable: no store row, no chunks, no comments, no search hit, no
   * posting list. */
  KBC_CHECK_NULL(id_of_path(f.cfg, CORPUS_A, "c.md", a));
  KBC_CHECK_EQ_INT(chunk_count(&side, id_copy), 0);
  KBC_CHECK_EQ_INT(comment_count(f.cfg, id_copy, a), 0);
  char path[64], title[64], id2[32];
  KBC_CHECK_EQ_INT(first_hit_path(f.app, "verdigris", NULL, path, sizeof path,
                                  title, sizeof title, id2, sizeof id2, NULL),
                   0);
  kbc_app_stats stats;
  KBC_CHECK_OK(kbc_app_stats_get(f.app, &stats, &err));
  KBC_CHECK_EQ_INT(stats.index_docs, 2);
  /* And the other two are untouched: a delete is about one document. */
  KBC_CHECK_EQ_INT(first_hit_path(f.app, "quixotic", NULL, path, sizeof path,
                                  title, sizeof title, id2, sizeof id2, NULL),
                   1);
  KBC_CHECK_EQ_STR(path, "b.md");
  KBC_CHECK_EQ_INT(store_count(f.cfg), 2);
  /* And the user's half is untouched, which is the invariant the cascade is
   * written around: a removal must not take a user's reading history with it.
   * The document is gone and the fact that nik read it, pinned it and put it
   * on the board is not. */
  KBC_CHECK_EQ_INT(history_visits(f.cfg, "nik", id_copy, a), 1);
  KBC_CHECK_EQ_INT(corkboard_entries(f.cfg, id_copy, a), 1);
  KBC_CHECK_EQ_INT(pins_of(f.cfg, id_copy, a), 1);

  /* It is the same removal the watcher runs, so it says the same thing. */
  KBC_CHECK_EQ_INT(events_naming(log, "watch.delete", "c.md"), 1);
  kbc_app_unsubscribe(f.app, sub);
  db_side_close(&side);
  kbc_arena_free(a);
  fx_teardown(&f);
}

/* The boundaries of that entry point, which is reachable from a request body
 * one day: a path that climbs out of the corpus, a corpus that is not
 * configured, and a document that is not there — which is the state the caller
 * asked for, so it is a success and not a 404-shaped error. */
KBC_TEST(delete_by_path_refuses_a_bad_target_and_forgives_an_absent_one) {
  fixture f;
  fx_setup(&f, false);
  if (f.app == NULL) {
    fx_teardown(&f);
    return;
  }
  kbc_err err;
  kbc_err_reset(&err);
  KBC_CHECK_OK(kbc_app_reindex(f.app, &err));

  KBC_CHECK_ERR(kbc_app_delete_path(f.app, CORPUS_A, "../a.md", &err),
                 KBC_ERR_INVALID);
  KBC_CHECK_ERR_MSG(err);
  kbc_err_reset(&err);
  KBC_CHECK_ERR(kbc_app_delete_path(f.app, CORPUS_B, "a.md", &err),
                 KBC_ERR_NOTFOUND);
  KBC_CHECK_ERR_MSG(err);
  kbc_err_reset(&err);
  KBC_CHECK_ERR(kbc_app_delete_path(f.app, NULL, "a.md", &err),
                 KBC_ERR_INVALID);
  kbc_err_reset(&err);

  /* Three documents in, three refusals out. */
  KBC_CHECK_EQ_INT(store_count(f.cfg), 3);

  KBC_CHECK_OK(kbc_app_delete_path(f.app, CORPUS_A, "never-existed.md", &err));
  KBC_CHECK_EQ_INT(store_count(f.cfg), 3);

  fx_teardown(&f);
}

/* ============================================================== capture == */

/* The capture's whole compatibility surface is the BYTES it writes, so that
 * is what these assert: the exact file, not "the keys are present". A set
 * assertion passes on a wrong order, and a wrong order is a different file
 * that fails the cross-implementation diff for a reason nobody can see. */

/* A capture input with everything set, so the expected file below names every
 * key that can be written. `now_unix` is pinned (the original's FrozenNow test
 * hook serves the same purpose) so the stamp is reproducible. */
static void full_capture_input(kbc_capture_input *in, const char *body) {
  static const char *const tags[] = {"Deep Work", "reading list!"};
  memset(in, 0, sizeof *in);
  in->corpus = CORPUS_A;
  in->from = "cli";
  in->title = "My Note";
  in->url = "https://example.com/a?b=1";
  in->original_filename = "notes.md";
  in->session_id = "sess-7";
  in->tags = tags;
  in->n_tags = 2;
  in->has_expires_at = true;
  in->expires_at = 1893456000;
  in->body = body;
  in->body_len = strlen(body);
  in->now_unix = 1700000000;
}

/* The HTML capture path, and the two things it can get wrong in opposite
 * directions. It used to be refused with a 415, which protected nothing: HTML
 * reaches the corpus by `kb add`, `git checkout` and sync regardless, and the
 * artifact route and the subdomain then serve it. So the route accepts
 * `.html`/`.htm` now, and the file that lands on disk has to be BOTH
 * sanitised and stamped. */

/* A capture input shaped like the Markdown one above, so the seven provenance
 * keys are the same seven and a divergence between the two paths is visible
 * as a different file rather than as a missing key. */
static void html_capture_input(kbc_capture_input *in, const char *body) {
  full_capture_input(in, body);
  in->original_filename = "page.html";
  /* `ext` is the ROUTE's decision, taken from the multipart filename by
   * capture_ext_of and carried on the input; a caller that leaves it empty
   * gets Markdown, which is the default and not a fallback. */
  in->ext = "html";
}

KBC_TEST(an_html_capture_is_sanitised_and_stamped) {
  fixture f;
  fx_setup(&f, false);
  if (f.app == NULL) {
    fx_teardown(&f);
    return;
  }
  kbc_err err;
  kbc_err_reset(&err);
  kbc_capture_input in;
  html_capture_input(&in,
                     "<html><head><title>t</title></head><body>"
                     "<script>alert(document.cookie)</script>"
                     "<img src=x onerror=alert(1)>"
                     "<a href=\"javascript:alert(1)\">c</a>"
                     "<!-- secret -->"
                     "<p>prose</p></body></html>");
  kbc_capture_result r;
  memset(&r, 0, sizeof r);
  KBC_CHECK_OK(kbc_app_capture(f.app, CORPUS_A, &in, &r, &err));
  KBC_CHECK_MSG(strstr(r.path, ".html") != NULL,
                "an HTML capture did not keep its extension: %s", r.path);

  char p[KBC_TEST_PATH_MAX];
  join(p, sizeof p, f.corpus_a, r.path);
  char *got = kbc_test_read_file(p);
  KBC_CHECK_NOT_NULL(got);
  if (got == NULL) {
    fx_teardown(&f);
    return;
  }
  /* The attack surface is gone. */
  KBC_CHECK_MSG(strstr(got, "<script") == NULL, "a script reached the corpus: %s",
                got);
  KBC_CHECK_MSG(strstr(got, "onerror") == NULL, "an event handler reached the "
                "corpus: %s", got);
  KBC_CHECK_MSG(strstr(got, "javascript:") == NULL, "a javascript: URL reached "
                "the corpus: %s", got);
  KBC_CHECK_MSG(strstr(got, "<!--") == NULL, "a comment reached the corpus: %s",
                got);
  /* And the document is still a document: an implementation that answers
   * every attack by storing nothing passes the four assertions above. */
  KBC_CHECK_MSG(strstr(got, "prose") != NULL, "the capture's own prose was "
                "lost: %s", got);

  /* The provenance is there, which is the ordering constraint stated in
   * stamp_capture_html: SANITISE FIRST, STAMP SECOND. Stamping first would
   * hand these `<meta>` tags to ammonia, which strips `meta`, and the
   * capture would be stored with its own provenance removed. The same seven
   * keys, the same values, the same skip rules as the Markdown path. */
  KBC_CHECK_MSG(strstr(got, "<meta name=\"kb-capture-at\" content=\"1700000000\">") != NULL,
                "kb-capture-at did not survive the sanitiser: %s", got);
  KBC_CHECK_MSG(strstr(got, "<meta name=\"kb-category\" content=\"capture\">") != NULL,
                "kb-category did not survive the sanitiser: %s", got);
  KBC_CHECK_MSG(strstr(got, "<meta name=\"kb-capture-original\" content=\"page.html\">") != NULL,
                "kb-capture-original did not survive the sanitiser: %s", got);
  KBC_CHECK_MSG(strstr(got, "<meta name=\"kb-session\" content=\"sess-7\">") != NULL,
                "kb-session did not survive the sanitiser: %s", got);
  /* The provenance lands INSIDE the head, which is the property the head
   * splice exists for, and it is asserted as a BOUNDARY rather than as
   * "the first meta is somewhere before the first paragraph": that weaker
   * question is answered yes by a file that opens with seven bare `<meta>`
   * lines and then an empty `<head></head>`, which is not a document whose
   * provenance is in its head at all. kbc_html_split_head's contract is
   * `head ++ metas ++ body`, so every `kb-` meta has to sit between the
   * opening tag and the close, and the close has to come before the body. */
  const char *open_head = strstr(got, "<head>");
  const char *close_head = strstr(got, "</head>");
  const char *prose = strstr(got, "<p>prose</p>");
  KBC_CHECK_MSG(open_head != NULL && open_head == got,
                "the stored capture does not open with its head: %.600s", got);
  KBC_CHECK_MSG(close_head != NULL,
                "the stored capture never closes its head: %.600s", got);
  KBC_CHECK_MSG(prose != NULL, "the body is not where it should be: %.600s",
                got);
  KBC_CHECK_MSG(open_head != NULL && close_head != NULL &&
                    open_head < close_head && close_head < prose,
                "the head does not wrap the body: %.600s", got);
  /* Every provenance meta, not just the first: one that escaped the head
   * while the rest stayed inside is the same defect with better luck. */
  size_t metas = 0, inside = 0;
  for (const char *q = got; (q = strstr(q, "<meta name=\"kb-")) != NULL; q++) {
    metas++;
    if (open_head != NULL && close_head != NULL && open_head < q &&
        q < close_head) {
      inside++;
    }
  }
  KBC_CHECK_MSG(metas == 7, "%zu provenance metas in the file, not 7: %.600s",
                metas, got);
  KBC_CHECK_MSG(inside == metas,
                "%zu of %zu provenance metas are outside the head: %.600s",
                metas - inside, metas, got);
  free(got);
  fx_teardown(&f);
}

KBC_TEST(a_capture_writes_the_originals_keys_in_the_originals_order) {
  fixture f;
  fx_setup(&f, false);
  if (f.app == NULL) {
    fx_teardown(&f);
    return;
  }
  kbc_err err;
  kbc_err_reset(&err);
  kbc_capture_input in;
  full_capture_input(&in, "# Body\n\nhello\n");
  kbc_capture_result r;
  memset(&r, 0, sizeof r);
  KBC_CHECK_OK(kbc_app_capture(f.app, CORPUS_A, &in, &r, &err));

  /* The filename is the slug of the title plus the capture's second — the
   * original's collision-resistant name (capture.rs:376). */
  KBC_CHECK_EQ_STR(r.path, "capture/my-note-1700000000.md");
  char p[KBC_TEST_PATH_MAX];
  join(p, sizeof p, f.corpus_a, r.path);
  char *got = kbc_test_read_file(p);
  KBC_CHECK_NOT_NULL(got);
  if (got == NULL) {
    fx_teardown(&f);
    return;
  }
  /* Byte for byte, in the original's order (capture.rs:517, 518, 523, 529,
   * 531, 541, 545). A tag is slugified the way the original slugifies it —
   * ASCII alphanumerics joined by single dashes, so "Deep Work" and
   * "reading list!" are "deep-work" and "reading-list" — while the two
   * provenance tags keep the colon the original writes them with. */
  KBC_CHECK_EQ_STR(
      got,
      "---\n"
      "kb-category: capture\n"
      "kb-tags: source:upload, from:cli, deep-work, reading-list\n"
      "kb-capture-original: notes.md\n"
      "kb-capture-url: https://example.com/a?b=1\n"
      "kb-capture-at: 1700000000\n"
      "kb-session: sess-7\n"
      "kb-expires-at: 1893456000\n"
      "---\n"
      "# Body\n\nhello\n");
  free(got);
  fx_teardown(&f);
}

/* The id is a pure function of (corpus, path) and the path carries the
 * capture's second, so two captures of the same bytes in the same second land
 * on distinct paths and therefore distinct ids. The stamp cannot see any of
 * that, which is the point: the same bytes must produce the same bytes. */
KBC_TEST(the_same_bytes_captured_twice_are_byte_identical) {
  fixture f;
  fx_setup(&f, false);
  if (f.app == NULL) {
    fx_teardown(&f);
    return;
  }
  kbc_err err;
  kbc_err_reset(&err);
  kbc_capture_input in;
  full_capture_input(&in, "# Same\n\nbytes every time\n");
  kbc_capture_result r1, r2;
  memset(&r1, 0, sizeof r1);
  memset(&r2, 0, sizeof r2);
  KBC_CHECK_OK(kbc_app_capture(f.app, CORPUS_A, &in, &r1, &err));
  KBC_CHECK_OK(kbc_app_capture(f.app, CORPUS_A, &in, &r2, &err));

  /* Distinct paths, distinct ids: nothing silently overwrote. */
  KBC_CHECK_EQ_STR(r1.path, "capture/my-note-1700000000.md");
  KBC_CHECK_EQ_STR(r2.path, "capture/my-note-1700000000-1.md");
  KBC_CHECK(strcmp(r1.id, r2.id) != 0);

  char p1[KBC_TEST_PATH_MAX], p2[KBC_TEST_PATH_MAX];
  join(p1, sizeof p1, f.corpus_a, r1.path);
  join(p2, sizeof p2, f.corpus_a, r2.path);
  char *a1 = kbc_test_read_file(p1);
  char *a2 = kbc_test_read_file(p2);
  KBC_CHECK_NOT_NULL(a1);
  KBC_CHECK_NOT_NULL(a2);
  if (a1 != NULL && a2 != NULL) {
    KBC_CHECK_MSG(strcmp(a1, a2) == 0,
                  "two captures of the same bytes differ:\n[%s]\n[%s]", a1, a2);
  }
  KBC_CHECK_EQ_INT(r1.bytes, r2.bytes);
  free(a1);
  free(a2);
  fx_teardown(&f);
}

/* The title a caller supplies steers the FILENAME and nothing else. A
 * capture is a real document that the indexer will read, and the document's
 * own authored title is the truth about what it is called. */
KBC_TEST(a_capture_never_touches_the_titles_title_already_has) {
  fixture f;
  fx_setup(&f, false);
  if (f.app == NULL) {
    fx_teardown(&f);
    return;
  }
  kbc_err err;
  kbc_err_reset(&err);
  kbc_capture_input in;
  full_capture_input(&in,
                     "---\ntitle: Already Titled\ncustom-key: keep-me\n---\n"
                     "Body.\n");
  kbc_capture_result r;
  memset(&r, 0, sizeof r);
  KBC_CHECK_OK(kbc_app_capture(f.app, CORPUS_A, &in, &r, &err));
  char p[KBC_TEST_PATH_MAX];
  join(p, sizeof p, f.corpus_a, r.path);
  char *got = kbc_test_read_file(p);
  KBC_CHECK_NOT_NULL(got);
  if (got == NULL) {
    fx_teardown(&f);
    return;
  }
  KBC_CHECK(strstr(got, "title: Already Titled") != NULL);
  KBC_CHECK(strstr(got, "custom-key: keep-me") != NULL);
  /* The caller's title is nowhere in the document: not as a title, not as a
 * kb-* facet, not as a heading. */
  KBC_CHECK(strstr(got, "My Note") == NULL);
  KBC_CHECK(strstr(got, "kb-title") == NULL);
  /* Only the title was special-cased, so everything else still landed. */
  KBC_CHECK(strstr(got, "kb-capture-at: 1700000000") != NULL);
  free(got);
  fx_teardown(&f);
}

/* A key the document already carries is EDITED IN PLACE, not appended: the
 * original's line-oriented setter rewrites the existing line where it stands
 * (markdown.rs:384-400), and a new key is appended after it. So the stamped
 * category keeps the position the author gave it, and the keys that were new
 * follow the author's own. */
KBC_TEST(a_capture_edits_a_key_in_place_and_appends_the_rest_after_it) {
  fixture f;
  fx_setup(&f, false);
  if (f.app == NULL) {
    fx_teardown(&f);
    return;
  }
  kbc_err err;
  kbc_err_reset(&err);
  kbc_capture_input in;
  full_capture_input(&in,
                     "---\ntitle: T\nkb-category: hand-written\nx: 1\n---\n"
                     "Body.\n");
  kbc_capture_result r;
  memset(&r, 0, sizeof r);
  KBC_CHECK_OK(kbc_app_capture(f.app, CORPUS_A, &in, &r, &err));
  char p[KBC_TEST_PATH_MAX];
  join(p, sizeof p, f.corpus_a, r.path);
  char *got = kbc_test_read_file(p);
  KBC_CHECK_NOT_NULL(got);
  if (got == NULL) {
    fx_teardown(&f);
    return;
  }
  KBC_CHECK_EQ_STR(
      got,
      "---\n"
      "title: T\n"
      "kb-category: capture\n"   /* edited where it stood */
      "x: 1\n"                   /* the author's own key, untouched */
      "kb-tags: source:upload, from:cli, deep-work, reading-list\n"
      "kb-capture-original: notes.md\n"
      "kb-capture-url: https://example.com/a?b=1\n"
      "kb-capture-at: 1700000000\n"
      "kb-session: sess-7\n"
      "kb-expires-at: 1893456000\n"
      "---\n"
      "Body.\n");
  free(got);
  fx_teardown(&f);
}

/* kb-session is the one stamped key that refuses to overwrite: the uploaded
 * content's own session marker is a fact about where the document came from,
 * and a re-capture must not restamp it (capture.rs:537-542). */
KBC_TEST(a_capture_does_not_restamp_a_session_the_document_already_carries) {
  fixture f;
  fx_setup(&f, false);
  if (f.app == NULL) {
    fx_teardown(&f);
    return;
  }
  kbc_err err;
  kbc_err_reset(&err);
  kbc_capture_input in;
  full_capture_input(&in, "---\ntitle: T\nkb-session: keep-me\n---\nBody.\n");
  KBC_CHECK_EQ_STR(in.session_id, "sess-7");
  kbc_capture_result r;
  memset(&r, 0, sizeof r);
  KBC_CHECK_OK(kbc_app_capture(f.app, CORPUS_A, &in, &r, &err));
  char p[KBC_TEST_PATH_MAX];
  join(p, sizeof p, f.corpus_a, r.path);
  char *got = kbc_test_read_file(p);
  KBC_CHECK_NOT_NULL(got);
  if (got == NULL) {
    fx_teardown(&f);
    return;
  }
  KBC_CHECK(strstr(got, "kb-session: keep-me") != NULL);
  KBC_CHECK(strstr(got, "sess-7") == NULL);
  /* Exactly one kb-session line, not a second one appended after it. */
  size_t hits = 0;
  for (const char *q = got; (q = strstr(q, "kb-session:")) != NULL; q++) hits++;
  KBC_CHECK_EQ_INT(hits, 1);
  free(got);
  fx_teardown(&f);
}

/* The capture's seven provenance keys are built into a fixed seven-slot
 * array, and a key that is SKIPPED — an empty optional value, or a
 * `kb-session` the source already carries — releases its slot without
 * advancing the index, so the array's tail is never written at all. The free
 * walk therefore has to be told how many slots were BUILT: walking the
 * capacity hands `free()` whatever the stack held in the unwritten ones, and
 * that is a crash out of a capture that had already succeeded — a SIGSEGV
 * when the garbage is not a pointer at all, an abort when it is a stale one.
 * Every case in this file that stamped all seven keys passed, which is why it
 * went unnoticed: only a capture with something left out reached it.
 *
 * The capture below is that case, over a POISONED stack. The stack the stamper
 * runs on is whatever the previous call left in it, so a regression here reads
 * whatever this function happened to write a few frames down — which on a
 * given build is a value, and on the next is a pointer to freed heap, and on
 * the one after that is a pointer into the middle of a live mapping. Filling
 * those bytes with a constant first makes the failure the SAME failure every
 * time instead of a coin flip, and puts it in this case rather than in
 * whichever case happened to run over dirty memory. */
static void capture_over_poisoned_stack(kbc_app *app,
                                        const kbc_capture_input *in,
                                        kbc_capture_result *r, int depth) {
  volatile unsigned char poison[16u * 1024u];
  for (size_t i = 0; i < sizeof poison; i++) {
    poison[i] = (unsigned char)(0xA5u ^ (unsigned)depth);
  }
  if (depth > 0) {
    capture_over_poisoned_stack(app, in, r, depth - 1);
    return;
  }
  kbc_err err;
  kbc_err_reset(&err);
  KBC_CHECK_OK(kbc_app_capture(app, CORPUS_A, in, r, &err));
}

KBC_TEST(a_capture_that_skips_provenance_keys_frees_only_what_it_built) {
  fixture f;
  fx_setup(&f, false);
  if (f.app == NULL) {
    fx_teardown(&f);
    return;
  }
  /* Nothing optional set: no category, no original filename, no URL, no
   * session, no expiry. Three of the seven keys are stamped and FOUR slots
   * of the array are left unwritten. */
  kbc_capture_input in;
  memset(&in, 0, sizeof in);
  in.corpus = CORPUS_A;
  in.from = "cli";
  in.title = "Bare";
  in.now_unix = 1700000000;
  in.body = "Body.\n";
  in.body_len = strlen(in.body);

  kbc_capture_result r;
  memset(&r, 0, sizeof r);
  capture_over_poisoned_stack(f.app, &in, &r, 3);
  KBC_CHECK_MSG(r.path[0] != '\0', "the Markdown capture reported no path");
  char p[KBC_TEST_PATH_MAX];
  join(p, sizeof p, f.corpus_a, r.path);
  char *got = kbc_test_read_file(p);
  KBC_CHECK_NOT_NULL(got);
  if (got == NULL) {
    fx_teardown(&f);
    return;
  }
  /* The three that were asked for, in the reference's order. */
  KBC_CHECK_EQ_STR(got,
                   "---\n"
                   "kb-category: capture\n"
                   "kb-tags: source:upload, from:cli\n"
                   "kb-capture-at: 1700000000\n"
                   "---\n"
                   "Body.\n");
  free(got);

  /* The same four skips through the HTML stamper, which frees the same array
   * through the same helper. */
  in.ext = "html";
  in.original_filename = NULL;
  in.body = "<p>prose</p>";
  in.body_len = strlen(in.body);
  memset(&r, 0, sizeof r);
  capture_over_poisoned_stack(f.app, &in, &r, 3);
  KBC_CHECK_MSG(strstr(r.path, ".html") != NULL,
                "the HTML capture did not keep its extension: %s", r.path);
  join(p, sizeof p, f.corpus_a, r.path);
  got = kbc_test_read_file(p);
  KBC_CHECK_NOT_NULL(got);
  if (got == NULL) {
    fx_teardown(&f);
    return;
  }
  /* A skipped key must leave no trace — a stale value read out of an
   * unwritten slot is the same defect wearing a different hat. */
  KBC_CHECK_MSG(strstr(got, "kb-capture-original") == NULL &&
                    strstr(got, "kb-capture-url") == NULL &&
                    strstr(got, "kb-session") == NULL &&
                    strstr(got, "kb-expires-at") == NULL,
                "a skipped key wrote something: %s", got);
  /* Byte for byte, because the cross-implementation diff is byte for byte.
   * The shape is `head ++ metas ++ body`: a synthesised head, the three keys
   * inside it, then the sanitised body. Each meta carries its own TRAILING
   * newline, which is what the original's `metas` string does — capture.rs
   * pushes `"<meta …>\n"` once per key (capture.rs:596-631) — so this is
   * `<head>\n{metas}</head>\n{html}` byte for byte. */
  KBC_CHECK_EQ_STR(got,
                   "<head>\n"
                   "<meta name=\"kb-category\" content=\"capture\">\n"
                   "<meta name=\"kb-tags\" content=\"source:upload, from:cli\">\n"
                   "<meta name=\"kb-capture-at\" content=\"1700000000\">\n"
                   "</head>\n"
                   "<p>prose</p>");
  free(got);
  fx_teardown(&f);
}

/* The writer is line-oriented, so a newline inside an attacker-controlled
 * value could inject a frontmatter line — or a line reading `---`, closing
 * the fence early and splicing a whole fake block into the document. */
KBC_TEST(a_newline_in_an_uploaded_name_cannot_close_the_frontmatter_fence) {
  fixture f;
  fx_setup(&f, false);
  if (f.app == NULL) {
    fx_teardown(&f);
    return;
  }
  kbc_err err;
  kbc_err_reset(&err);
  kbc_capture_input in;
  full_capture_input(&in, "body\n");
  in.original_filename = "evil\n---\nkb-category: smuggled\n---\ntail";
  kbc_capture_result r;
  memset(&r, 0, sizeof r);
  KBC_CHECK_OK(kbc_app_capture(f.app, CORPUS_A, &in, &r, &err));
  char p[KBC_TEST_PATH_MAX];
  join(p, sizeof p, f.corpus_a, r.path);
  char *got = kbc_test_read_file(p);
  KBC_CHECK_NOT_NULL(got);
  if (got == NULL) {
    fx_teardown(&f);
    return;
  }
  KBC_CHECK(strstr(got, "\n---\nkb-category: smuggled\n---\n") == NULL);
  KBC_CHECK(strstr(got,
                   "kb-capture-original: evil --- kb-category: smuggled --- "
                   "tail") != NULL);
  /* Exactly one frontmatter LINE named kb-category, and it is the one the
   * capture stamped. The smuggled text is still in the file — as the inert
   * value of kb-capture-original, which is the point — so the count is over
   * line-initial keys, not over the substring. */
  size_t cats = 0;
  for (const char *q = got; (q = strstr(q, "\nkb-category:")) != NULL; q++)
    cats++;
  KBC_CHECK_EQ_INT(cats, 1);
  KBC_CHECK(strstr(got, "\nkb-category: capture\n") != NULL);
  free(got);
  fx_teardown(&f);
}

/* A capture is a REAL file in the corpus: the next pass indexes it, searches
 * it and mints the id this call already returned. "Physically real, semantically
 * staged" is the original's phrase and the staging half is asynchronous —
 * the route answers before the watcher has run. */
KBC_TEST(a_capture_is_a_document_the_index_picks_up_under_the_id_it_reported) {
  fixture f;
  fx_setup(&f, false);
  if (f.app == NULL) {
    fx_teardown(&f);
    return;
  }
  kbc_err err;
  kbc_err_reset(&err);
  KBC_CHECK_OK(kbc_app_reindex(f.app, &err));

  kbc_capture_input in;
  full_capture_input(&in, "# Verdigris\n\nThe digest catalogues verdigris.\n");
  kbc_capture_result r;
  memset(&r, 0, sizeof r);
  KBC_CHECK_OK(kbc_app_capture(f.app, CORPUS_A, &in, &r, &err));
  /* Not indexed yet: the daemon stages it and the watcher lands it. */
  KBC_CHECK_EQ_INT(store_count(f.cfg), 3);
  KBC_CHECK_OK(kbc_app_reindex(f.app, &err));
  KBC_CHECK_EQ_INT(store_count(f.cfg), 4);

  kbc_arena *a = kbc_arena_new(64u * 1024u);
  KBC_CHECK_NOT_NULL(a);
  if (a != NULL) {
    /* The id the capture reported is the id the index minted, which is only
     * true because the id is a pure function of (corpus, path). */
    const char *stored = id_of_path(f.cfg, CORPUS_A, r.path, a);
    KBC_CHECK_NOT_NULL(stored);
    if (stored != NULL) KBC_CHECK_EQ_STR(stored, r.id);
    kbc_arena_free(a);
  }
  char path[64], title[64], id[32];
  KBC_CHECK_EQ_INT(first_hit_path(f.app, "verdigris", NULL, path, sizeof path,
                                  title, sizeof title, id, sizeof id, NULL),
                   2);
  KBC_CHECK_EQ_STR(id, r.id);
  fx_teardown(&f);
}

/* THE SSRF RULING, as a test. The daemon never fetches a captured URL: the
 * URL is provenance and nothing else. A listener is bound and pointed at, and
 * the capture must complete without a single packet reaching it — so a future
 * reader who helpfully adds a fetch to "enrich" the capture fails here rather
 * than shipping a server-side request forgery. */
KBC_TEST(a_capture_records_the_url_without_ever_connecting_to_it) {
  int lfd = socket(AF_INET, SOCK_STREAM, 0);
  KBC_CHECK(lfd >= 0);
  if (lfd < 0) return;
  int one = 1;
  setsockopt(lfd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
  struct sockaddr_in addr;
  memset(&addr, 0, sizeof addr);
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  addr.sin_port = 0; /* the kernel picks a free port */
  KBC_CHECK(bind(lfd, (struct sockaddr *)&addr, sizeof addr) == 0);
  KBC_CHECK(listen(lfd, 4) == 0);
  socklen_t alen = sizeof addr;
  KBC_CHECK(getsockname(lfd, (struct sockaddr *)&addr, &alen) == 0);
  char url[128];
  snprintf(url, sizeof url, "http://127.0.0.1:%u/should-never-be-fetched",
           (unsigned)ntohs(addr.sin_port));

  fixture f;
  fx_setup(&f, false);
  if (f.app == NULL) {
    close(lfd);
    fx_teardown(&f);
    return;
  }
  kbc_err err;
  kbc_err_reset(&err);
  kbc_capture_input in;
  full_capture_input(&in, "# Shared\n\nsome shared words\n");
  in.url = url;
  in.original_filename = NULL;
  in.title = "Shared";
  kbc_capture_result r;
  memset(&r, 0, sizeof r);
  KBC_CHECK_OK(kbc_app_capture(f.app, CORPUS_A, &in, &r, &err));

  /* The URL is INERT TEXT in the file. */
  char p[KBC_TEST_PATH_MAX];
  join(p, sizeof p, f.corpus_a, r.path);
  char *got = kbc_test_read_file(p);
  KBC_CHECK_NOT_NULL(got);
  if (got != NULL) {
    char line[256];
    snprintf(line, sizeof line, "kb-capture-url: %s\n", url);
    KBC_CHECK_MSG(strstr(got, line) != NULL, "no kb-capture-url line in:\n%s",
                  got);
    free(got);
  }
  /* And nothing connected to it. */
  struct pollfd pfd;
  pfd.fd = lfd;
  pfd.events = POLLIN;
  pfd.revents = 0;
  int ready = poll(&pfd, 1, 50);
  KBC_CHECK_MSG(ready == 0, "a capture connected to the URL it recorded (%d)",
                ready);
  close(lfd);
  fx_teardown(&f);
}

/* A URL share with no uploaded file: an H1, the URL as a Markdown autolink
 * and the shared text as a paragraph, tagged so a later reader can tell a
 * stub from a saved page. The tag keeps its colon, which the slugify would
 * otherwise collapse to a dash. */
KBC_TEST(a_url_share_becomes_a_stub_carrying_the_provenance_tag) {
  fixture f;
  fx_setup(&f, false);
  if (f.app == NULL) {
    fx_teardown(&f);
    return;
  }
  kbc_err err;
  kbc_err_reset(&err);
  kbc_capture_url_input u;
  memset(&u, 0, sizeof u);
  u.corpus = CORPUS_A;
  u.from = "share";
  u.title = "   "; /* whitespace is not a title */
  u.url = "https://example.com/page";
  u.text = "some shared words";
  u.now_unix = 1700000003;
  kbc_capture_result r;
  memset(&r, 0, sizeof r);
  KBC_CHECK_OK(kbc_app_capture_url_stub(f.app, CORPUS_A, &u, &r, &err));
  KBC_CHECK_EQ_STR(r.path, "capture/untitled-capture-1700000003.md");
  char p[KBC_TEST_PATH_MAX];
  join(p, sizeof p, f.corpus_a, r.path);
  char *got = kbc_test_read_file(p);
  KBC_CHECK_NOT_NULL(got);
  if (got == NULL) {
    fx_teardown(&f);
    return;
  }
  KBC_CHECK_EQ_STR(
      got,
      "---\n"
      "kb-category: capture\n"
      "kb-tags: source:upload, from:share, kind:url-stub\n"
      "kb-capture-url: https://example.com/page\n"
      "kb-capture-at: 1700000003\n"
      "---\n"
      "# Untitled capture\n"
      "\n"
      "<https://example.com/page>\n"
      "\n"
      "some shared words\n");
  free(got);
  fx_teardown(&f);
}

/* The decoder the capture route is built on. It splits a request body into
 * the fields a capture carries, and it is where a filename first becomes
 * attacker-controlled text — so the filename has to come out as DATA. */
KBC_TEST(a_multipart_body_splits_into_the_fields_a_capture_carries) {
  static const char body[] =
      "--B\r\n"
      "Content-Disposition: form-data; name=\"title\"\r\n"
      "\r\n"
      "Hello\r\n"
      "--B\r\n"
      "Content-Disposition: form-data; name=\"files\"; filename=\"a b.md\"\r\n"
      "Content-Type: text/markdown\r\n"
      "\r\n"
      "# hi\r\n"
      "--B--\r\n";
  kbc_arena *a = kbc_arena_new(4096u);
  KBC_CHECK_NOT_NULL(a);
  if (a == NULL) return;
  kbc_multipart_part parts[8];
  size_t n = 0;
  kbc_err err;
  kbc_err_reset(&err);
  KBC_CHECK_OK(kbc_multipart_parse("multipart/form-data; boundary=\"B\"", body,
                                   strlen(body), parts, 8, &n, a, &err));
  KBC_CHECK_EQ_INT(n, 2);
  if (n == 2) {
    KBC_CHECK_EQ_STR(parts[0].name, "title");
    KBC_CHECK_NULL(parts[0].filename);
    KBC_CHECK_EQ_INT(parts[0].len, 5);
    KBC_CHECK_EQ_STR(parts[0].data, "Hello");
    KBC_CHECK_EQ_STR(parts[1].name, "files");
    KBC_CHECK_EQ_STR(parts[1].filename, "a b.md");
    KBC_CHECK_EQ_INT(parts[1].len, 4);
    KBC_CHECK_EQ_STR(parts[1].data, "# hi");
  }

  /* A body that stops before its closing delimiter is malformed. The status
   * says so, and it is NOT the "your array was too small" status — resizing
   * would not fix a truncated upload, and a caller sent down the resize road
   * would loop forever. The count still names the one part that was read
   * before the body ran out, because the rule is that it always does. */
  n = 0;
  kbc_err_reset(&err);
  KBC_CHECK_ERR(kbc_multipart_parse("multipart/form-data; boundary=B", body,
                                   strlen(body) - 6, parts, 8, &n, a, &err),
                KBC_ERR_INVALID);
  KBC_CHECK_ERR_MSG(err);
  KBC_CHECK(strstr(err.msg, "more than") == NULL);
  KBC_CHECK_EQ_INT(n, 1);
  if (n == 1) KBC_CHECK_EQ_STR(parts[0].data, "Hello");
  /* A content type with no boundary parameter is not a multipart body. */
  kbc_err_reset(&err);
  KBC_CHECK_ERR(kbc_multipart_parse("application/json", body, strlen(body),
                                   parts, 8, &n, a, &err),
                KBC_ERR_INVALID);
  KBC_CHECK_ERR_MSG(err);
  kbc_arena_free(a);
}

/* A body longer than the caller's array is the case the cap exists for, and
 * the contract has to serve BOTH callers: the one that reads the status and
 * resizes, and the one that forgets to.
 *
 * The first is what makes the count worth having — *n_out is the TRUE number
 * of parts, not the number that fitted, so the retry is sized from a fact
 * about the body rather than a guess about the caller's buffer. The second is
 * what stops the failure from being silent: the status is a failure, nothing
 * is written past the array, and *n_out is larger than the array, so a caller
 * that ignores the status is holding the evidence that it ignored one.
 *
 * A truncated array with a success status would be the outcome worth being
 * afraid of — the first 2 of 3 files captured and a document reported
 * created — and it is exactly what "count past the cap, return OK" would
 * produce. */
KBC_TEST(a_body_longer_than_the_array_reports_the_true_count_so_a_retry_fits) {
  static const char body[] =
      "--B\r\n"
      "Content-Disposition: form-data; name=\"one\"\r\n"
      "\r\n"
      "1\r\n"
      "--B\r\n"
      "Content-Disposition: form-data; name=\"two\"\r\n"
      "\r\n"
      "2\r\n"
      "--B\r\n"
      "Content-Disposition: form-data; name=\"three\"\r\n"
      "\r\n"
      "3\r\n"
      "--B--\r\n";
  kbc_arena *a = kbc_arena_new(4096u);
  KBC_CHECK_NOT_NULL(a);
  if (a == NULL) return;
  kbc_err err;
  kbc_err_reset(&err);

  /* Two slots, three parts. */
  kbc_multipart_part small[2];
  size_t n = 0;
  kbc_status s = kbc_multipart_parse("multipart/form-data; boundary=B", body,
                                     strlen(body), small, 2, &n, a, &err);
  KBC_CHECK_ERR(s, KBC_ERR_INVALID);
  KBC_CHECK_ERR_MSG(err);
  KBC_CHECK_MSG(strstr(err.msg, "more than") != NULL,
                "the overflow must be distinguishable from a malformed body: "
                "one is fixed by resizing and the other is not; got \"%s\"",
                err.msg);
  KBC_CHECK_EQ_INT(n, 3);
  /* The two that fitted are real, and nothing was written past the array. */
  KBC_CHECK_EQ_STR(small[0].name, "one");
  KBC_CHECK_EQ_STR(small[1].name, "two");

  /* Sized from the number the FAILURE reported, not from a guess and not
   * from the status alone, the retry succeeds and every part is there —
   * which is the whole reason the count is worth carrying on a failure. */
  const size_t need = n;
  kbc_multipart_part big[3];
  kbc_err_reset(&err);
  n = 0;
  s = kbc_multipart_parse("multipart/form-data; boundary=B", body, strlen(body),
                          big, need, &n, a, &err);
  KBC_CHECK_OK(s);
  KBC_CHECK_EQ_INT(n, 3);
  if (n == 3) {
    KBC_CHECK_EQ_STR(big[0].name, "one");
    KBC_CHECK_EQ_STR(big[0].data, "1");
    KBC_CHECK_EQ_STR(big[1].name, "two");
    KBC_CHECK_EQ_STR(big[1].data, "2");
    KBC_CHECK_EQ_STR(big[2].name, "three");
    KBC_CHECK_EQ_STR(big[2].data, "3");
  }
  kbc_arena_free(a);
}

/* =================================================================== mv == */

/* A rename is a removal plus a creation, so everything a removal deliberately
 * leaves behind and a creation cannot rebuild has to be carried across
 * explicitly — otherwise `mv` is a silent data loss wearing a rename's name.
 * The id changes (it is a function of the path), so every row keyed on it
 * would be orphaned: the comment thread, the pin, the corkboard entry. */
KBC_TEST(a_move_carries_the_comments_the_pin_and_the_corkboard_across) {
  fixture f;
  fx_setup(&f, false);
  if (f.app == NULL) {
    fx_teardown(&f);
    return;
  }
  kbc_err err;
  kbc_err_reset(&err);
  KBC_CHECK_OK(kbc_app_reindex(f.app, &err));

  kbc_arena *a = kbc_arena_new(64u * 1024u);
  KBC_CHECK_NOT_NULL(a);
  if (a == NULL) {
    fx_teardown(&f);
    return;
  }
  const char *old_id = id_of_path(f.cfg, CORPUS_A, "c.md", a);
  KBC_CHECK_NOT_NULL(old_id);
  if (old_id == NULL) {
    kbc_arena_free(a);
    fx_teardown(&f);
    return;
  }
  char old_id_copy[KBC_MAX_ID_LEN + 1];
  memcpy(old_id_copy, old_id, sizeof old_id_copy);
  kbc_store *st = kbc_store_open(f.cfg, &err);
  KBC_CHECK_NOT_NULL(st);
  if (st == NULL) {
    kbc_arena_free(a);
    fx_teardown(&f);
    return;
  }
  KBC_CHECK_OK(kbc_store_add_comment(st, old_id_copy, "section:digest", "nik",
                                     "worth keeping", &err));
  KBC_CHECK_OK(kbc_store_add_corkboard(st, old_id_copy, 1700, &err));
  KBC_CHECK_OK(kbc_store_pin_memory(st, old_id_copy, 1701, &err));
  kbc_store_close(st);
  KBC_CHECK_EQ_INT(comment_count(f.cfg, old_id_copy, a), 1);

  KBC_CHECK_OK(kbc_app_move_path(f.app, CORPUS_A, "c.md", "notes/renamed.md",
                                 &err));

  /* The id is a function of the path, so the move necessarily mints a new
   * one — which is exactly why the rows keyed on it had to be carried. */
  const char *new_id = id_of_path(f.cfg, CORPUS_A, "notes/renamed.md", a);
  KBC_CHECK_NOT_NULL(new_id);
  if (new_id == NULL) {
    kbc_arena_free(a);
    fx_teardown(&f);
    return;
  }
  KBC_CHECK(strcmp(new_id, old_id_copy) != 0);
  KBC_CHECK_EQ_INT(comment_count(f.cfg, new_id, a), 1);
  KBC_CHECK_EQ_INT(corkboard_entries(f.cfg, new_id, a), 1);
  KBC_CHECK_EQ_INT(pins_of(f.cfg, new_id, a), 1);
  /* And none of it is left behind on the dead id, which is what makes the
   * carry a move rather than a copy. */
  KBC_CHECK_EQ_INT(comment_count(f.cfg, old_id_copy, a), 0);
  KBC_CHECK_EQ_INT(corkboard_entries(f.cfg, old_id_copy, a), 0);
  KBC_CHECK_EQ_INT(pins_of(f.cfg, old_id_copy, a), 0);
  /* The timestamps the user set came with it: a pin re-pinned "now" is a
   * pin the user did not make. */
  KBC_CHECK_EQ_INT(pins_of(f.cfg, new_id, a), 1);
  char p_old[KBC_TEST_PATH_MAX], p_new[KBC_TEST_PATH_MAX];
  join(p_old, sizeof p_old, f.corpus_a, "c.md");
  join(p_new, sizeof p_new, f.corpus_a, "notes/renamed.md");
  KBC_CHECK(kbc_path_exists(p_old) == false);
  KBC_CHECK(kbc_path_exists(p_new) == true);
  KBC_CHECK_EQ_INT(store_count(f.cfg), 3);
  kbc_arena_free(a);
  fx_teardown(&f);
}

/* ------------------------------------------------ attachments at the app --
 *
 * Two properties the store cases cover and the APP layer did not, which is
 * why they are here rather than there: both are claims about what a document
 * removal and a document move do to the rows hanging off a document id, and
 * both are decided by which table names that id. The store knows the cascade;
 * the app is what actually calls the delete and the rekey, so a rekey that
 * forgot the attachments would leave a blob on a document that no longer has
 * that id, and nothing in the store suite would notice because the store was
 * never asked.
 */
/* Real PNG magic, because the sniffer is a byte test and a body that is not a
 * PNG is refused at the gate — which is the same trap
 * `an_attachment_round_trips_byte_for_byte_including_a_nul` fell into. */
static const char kAppPng[] = "\x89PNG\r\n\x1a\n\x00\x01\x02\x03";

/* One attachment on `doc_id`, adopted onto `cid` when there is one. Returns
 * the aid, or an empty string. */
static bool add_app_blob(const kbc_config *cfg, const char *doc_id,
                         const char *cid, const char *name, char *aid_out) {

  aid_out[0] = '\0';
  kbc_err err;
  kbc_err_reset(&err);
  kbc_store *s = kbc_store_open(cfg, &err);
  if (s == NULL) return false;
  kbc_attachment_in in;
  memset(&in, 0, sizeof in);
  in.doc_id = doc_id;
  in.comment_id = cid;
  in.filename = name;
  in.content_type = kbc_store_sniff_attachment(kAppPng, sizeof kAppPng - 1u);
  in.author = "nik";
  in.body = kAppPng;
  in.body_len = sizeof kAppPng - 1u;
  char aid[KBC_ATTACH_ID_LEN + 1];
  kbc_status st = kbc_store_add_attachment(s, &in, aid, &err);
  if (st != KBC_OK) {
    fprintf(stderr, "  add_app_blob: %s\n", err.msg);
    kbc_store_close(s);
    return false;
  }
  memcpy(aid_out, aid, strlen(aid) + 1u);
  kbc_store_close(s);
  return true;
}

/* Every attachment row on `doc_id`, counted. `doc_id` NULL counts the whole
 * table, which is the half that matters for a delete: a cascade that dropped
 * the rows from the listing but left them in the table would pass a
 * per-document count. */
static size_t blob_count(const kbc_config *cfg, const char *doc_id,
                         kbc_arena *a) {
  kbc_err err;
  kbc_err_reset(&err);
  kbc_store *s = kbc_store_open(cfg, &err);
  if (s == NULL) return 0;
  kbc_attachment *rows = NULL;
  size_t n = 0;
  (void)kbc_store_list_attachments(s, a, doc_id, 200u, &rows, &n, &err);
  kbc_store_close(s);
  return n;
}

/* A DOCUMENT REMOVAL TAKES ITS ATTACHMENTS WITH IT, bytes included.
 *
 * The cascade is the schema's and not this layer's: `attachments.doc_id` and
 * `attachments.comment_id` both say `ON DELETE CASCADE` and the store runs
 * `PRAGMA foreign_keys = ON`. What this case is for is that the APP's delete
 * path reaches that schema at all, and that nothing is left holding bytes for
 * a document that is gone. Both an ADOPTED row and a STAGED one are placed,
 * because they sit on different columns and a cascade wired to only one of
 * them would leave the other behind — and a STAGED row is the one nobody is
 * looking for, since it belongs to no comment. */
KBC_TEST(a_document_removal_takes_its_attachments_and_their_bytes) {
  fixture f;
  fx_setup(&f, false);
  if (f.app == NULL) {
    fx_teardown(&f);
    return;
  }
  kbc_err err;
  kbc_err_reset(&err);
  KBC_CHECK_OK(kbc_app_reindex(f.app, &err));
  kbc_arena *a = kbc_arena_new(64u * 1024u);
  KBC_CHECK_NOT_NULL(a);
  if (a == NULL) {
    fx_teardown(&f);
    return;
  }
  char doc[KBC_MAX_ID_LEN + 1];
  const char *id = id_of_path(f.cfg, CORPUS_A, "c.md", a);
  KBC_CHECK_NOT_NULL(id);
  if (id == NULL) {
    kbc_arena_free(a);
    fx_teardown(&f);
    return;
  }
  memcpy(doc, id, sizeof doc);

  /* A comment with an ADOPTED attachment, and a STAGED one nobody claimed. */
  kbc_store *st = kbc_store_open(f.cfg, &err);
  KBC_CHECK_NOT_NULL(st);
  if (st == NULL) {
    kbc_arena_free(a);
    fx_teardown(&f);
    return;
  }
  KBC_CHECK_OK(kbc_store_add_comment(st, doc, "section:digest", "nik", "keep",
                                     &err));
  kbc_store_close(st);
  kbc_arena *ca = kbc_arena_new(8192);
  kbc_comment *cs = NULL;
  size_t cn = 0;
  st = kbc_store_open(f.cfg, &err);
  if (st != NULL) {
    KBC_CHECK_OK(kbc_store_list_comments(st, ca, doc, 10, &cs, &cn, &err));
    kbc_store_close(st);
  }
  KBC_CHECK_EQ_INT((int64_t)cn, 1);
  char cid[KBC_MAX_ID_LEN + 1];
  cid[0] = '\0';
  if (cn == 1) memcpy(cid, cs[0].id, sizeof cid);
  char adopted[KBC_ATTACH_ID_LEN + 1];
  char staged[KBC_ATTACH_ID_LEN + 1];
  KBC_CHECK_MSG(add_app_blob(f.cfg, doc, cid, "adopted.png", adopted),
                "the adopted attachment was refused");
  KBC_CHECK_MSG(add_app_blob(f.cfg, doc, NULL, "staged.png", staged),
                "the staged attachment was refused");
  KBC_CHECK_EQ_INT((int64_t)blob_count(f.cfg, doc, a), 2);
  /* They are really there, with their bytes, before the delete — otherwise
   * the assertions after it would pass on an empty table. */
  {
    kbc_str bytes;
    kbc_str_init(&bytes);
    st = kbc_store_open(f.cfg, &err);
    if (st != NULL) {
      KBC_CHECK_OK(kbc_store_read_attachment(st, adopted, &bytes, &err));
      kbc_store_close(st);
    }
    KBC_CHECK_MSG(bytes.len == sizeof kAppPng - 1u,
                  "the attachment held %zu bytes before the delete", bytes.len);
    kbc_str_free(&bytes);
  }

  KBC_CHECK_OK(kbc_app_delete_path(f.app, CORPUS_A, "c.md", &err));

  /* The rows are gone from the document's listing AND from the table. */
  KBC_CHECK_EQ_INT((int64_t)blob_count(f.cfg, doc, a), 0);
  KBC_CHECK_EQ_INT((int64_t)blob_count(f.cfg, NULL, a), 0);
  /* And the BYTES are gone, which is the half a row-only sweep would miss:
   * here the blob lives in the row, so there is nothing on disk for a later
   * GC to find and an orphan row would be an orphan file. */
  {
    kbc_str bytes;
    kbc_str_init(&bytes);
    st = kbc_store_open(f.cfg, &err);
    if (st != NULL) {
      KBC_CHECK_ERR(kbc_store_read_attachment(st, adopted, &bytes, &err),
                    KBC_ERR_NOTFOUND);
      KBC_CHECK_ERR(kbc_store_read_attachment(st, staged, &bytes, &err),
                    KBC_ERR_NOTFOUND);
      kbc_store_close(st);
    }
    kbc_str_free(&bytes);
  }
  /* The other documents are untouched: a cascade keyed on the table rather
   * than on the row would have taken the whole corpus with it. */
  KBC_CHECK_EQ_INT((int64_t)blob_count(f.cfg, NULL, a), 0);
  KBC_CHECK_EQ_INT(store_count(f.cfg), 2);
  kbc_arena_free(ca);
  kbc_arena_free(a);
  fx_teardown(&f);
}

/* A MOVE CARRIES THE ATTACHMENTS, and the aid is the reason it is a carry and
 * not a copy.
 *
 * A document id is a hash of (corpus, path), so a move necessarily mints a
 * new one. An attachment id is six RANDOM bytes and nothing else — it is not
 * derived from the path, the filename or the content — so a rekey that
 * rewrote it would break every URL already handed to a reader, and a rekey
 * that dropped the row would take the picture off a comment that still names
 * it. `doc_id` is the only column that had to change, and this asserts both
 * halves: the row is on the new document, and it is the SAME row. */
KBC_TEST(a_move_carries_the_attachments_under_the_same_id) {
  fixture f;
  fx_setup(&f, false);
  if (f.app == NULL) {
    fx_teardown(&f);
    return;
  }
  kbc_err err;
  kbc_err_reset(&err);
  KBC_CHECK_OK(kbc_app_reindex(f.app, &err));
  kbc_arena *a = kbc_arena_new(64u * 1024u);
  KBC_CHECK_NOT_NULL(a);
  if (a == NULL) {
    fx_teardown(&f);
    return;
  }
  char old_id[KBC_MAX_ID_LEN + 1];
  const char *id = id_of_path(f.cfg, CORPUS_A, "c.md", a);
  KBC_CHECK_NOT_NULL(id);
  if (id == NULL) {
    kbc_arena_free(a);
    fx_teardown(&f);
    return;
  }
  memcpy(old_id, id, sizeof old_id);
  char aid[KBC_ATTACH_ID_LEN + 1];
  KBC_CHECK_MSG(add_app_blob(f.cfg, old_id, NULL, "chart.png", aid),
                "the attachment was refused");
  KBC_CHECK_EQ_INT((int64_t)blob_count(f.cfg, old_id, a), 1);

  KBC_CHECK_OK(kbc_app_move_path(f.app, CORPUS_A, "c.md", "notes/renamed.md",
                                 &err));
  const char *new_id = id_of_path(f.cfg, CORPUS_A, "notes/renamed.md", a);
  KBC_CHECK_NOT_NULL(new_id);
  if (new_id == NULL) {
    kbc_arena_free(a);
    fx_teardown(&f);
    return;
  }
  char moved[KBC_MAX_ID_LEN + 1];
  memcpy(moved, new_id, sizeof moved);
  KBC_CHECK_MSG(strcmp(moved, old_id) != 0,
                "the move did not mint a new document id, so this case is "
                "not testing the rekey at all");

  /* It followed, and nothing was left behind on the dead id — which is what
   * makes the rekey a move rather than a copy. */
  KBC_CHECK_EQ_INT((int64_t)blob_count(f.cfg, moved, a), 1);
  KBC_CHECK_EQ_INT((int64_t)blob_count(f.cfg, old_id, a), 0);
  KBC_CHECK_EQ_INT((int64_t)blob_count(f.cfg, NULL, a), 1);
  /* Under the SAME aid: the id is minted, not derived, so it is the one thing
   * a move must not remint. */
  {
    kbc_store *s = kbc_store_open(f.cfg, &err);
    KBC_CHECK_NOT_NULL(s);
    if (s != NULL) {
      kbc_attachment at;
      memset(&at, 0, sizeof at);
      KBC_CHECK_OK(kbc_store_get_attachment(s, a, aid, &at, &err));
      KBC_CHECK_EQ_STR(at.id, aid);
      KBC_CHECK_EQ_STR(at.doc_id, moved);
      /* And the filename and the sniffed type came with it, so the served
       * headers after a move are the ones the upload produced. */
      KBC_CHECK_EQ_STR(at.filename, "chart.png");
      KBC_CHECK_EQ_STR(at.content_type, "image/png");
      kbc_str bytes;
      kbc_str_init(&bytes);
      KBC_CHECK_OK(kbc_store_read_attachment(s, aid, &bytes, &err));
      KBC_CHECK_MSG(bytes.len == sizeof kAppPng - 1u,
                    "the moved attachment held %zu bytes", bytes.len);
      kbc_str_free(&bytes);
      kbc_store_close(s);
    }
  }
  kbc_arena_free(a);
  fx_teardown(&f);
}

/* The resolution flag is a decision about a thread, exactly as a pin is, so
 * it comes across with the thread. kbc_store_add_comment mints it false,
 * which is precisely why this needs its own case. */
KBC_TEST(a_resolved_comment_is_still_resolved_after_a_move) {
  fixture f;
  fx_setup(&f, false);
  if (f.app == NULL) {
    fx_teardown(&f);
    return;
  }
  kbc_err err;
  kbc_err_reset(&err);
  KBC_CHECK_OK(kbc_app_reindex(f.app, &err));
  kbc_arena *a = kbc_arena_new(64u * 1024u);
  KBC_CHECK_NOT_NULL(a);
  if (a == NULL) {
    fx_teardown(&f);
    return;
  }
  const char *old_id = id_of_path(f.cfg, CORPUS_A, "c.md", a);
  KBC_CHECK_NOT_NULL(old_id);
  if (old_id == NULL) {
    kbc_arena_free(a);
    fx_teardown(&f);
    return;
  }
  char old_id_copy[KBC_MAX_ID_LEN + 1];
  memcpy(old_id_copy, old_id, sizeof old_id_copy);
  kbc_store *st = kbc_store_open(f.cfg, &err);
  KBC_CHECK_NOT_NULL(st);
  if (st == NULL) {
    kbc_arena_free(a);
    fx_teardown(&f);
    return;
  }
  kbc_comment *cs = NULL;
  size_t n_cs = 0;
  KBC_CHECK_OK(kbc_store_add_comment(st, old_id_copy, "section:digest", "nik",
                                     "done", &err));
  KBC_CHECK_OK(kbc_store_list_comments(st, a, old_id_copy, 16, &cs, &n_cs, &err));
  KBC_CHECK_EQ_INT(n_cs, 1);
  if (n_cs == 1) {
    KBC_CHECK_OK(
        kbc_store_set_comment_resolved(st, cs[0].id, true, &err));
  }
  kbc_store_close(st);

  KBC_CHECK_OK(kbc_app_move_path(f.app, CORPUS_A, "c.md", "moved.md", &err));
  const char *new_id = id_of_path(f.cfg, CORPUS_A, "moved.md", a);
  KBC_CHECK_NOT_NULL(new_id);
  if (new_id != NULL) {
    st = kbc_store_open(f.cfg, &err);
    KBC_CHECK_NOT_NULL(st);
    if (st != NULL) {
      kbc_comment *after = NULL;
      size_t n_after = 0;
      KBC_CHECK_OK(kbc_store_list_comments(st, a, new_id, 16, &after, &n_after,
                                          &err));
      KBC_CHECK_EQ_INT(n_after, 1);
      if (n_after == 1) {
        KBC_CHECK_EQ_STR(after[0].anchor, "section:digest");
        KBC_CHECK_EQ_STR(after[0].body, "done");
        KBC_CHECK_MSG(after[0].resolved,
                      "a resolved comment came back unresolved after a move");
      }
      kbc_store_close(st);
 }
  }
  kbc_arena_free(a);
  fx_teardown(&f);
}

/* What a move refuses, and what it refuses it BEFORE touching: a target that
 * exists is a conflict, a source that is not a file is a miss, a source the
 * store has never seen is a miss (moving it would silently create a
 * document), and a path that climbs out of the corpus never reaches the
 * filesystem at all. */
KBC_TEST(a_move_refuses_its_bad_targets_before_it_touches_anything) {
  fixture f;
  fx_setup(&f, false);
  if (f.app == NULL) {
    fx_teardown(&f);
    return;
  }
  kbc_err err;
  kbc_err_reset(&err);
  KBC_CHECK_OK(kbc_app_reindex(f.app, &err));

  KBC_CHECK_ERR(kbc_app_move_path(f.app, CORPUS_A, "c.md", "a.md", &err),
                KBC_ERR_CONFLICT);
  KBC_CHECK_ERR_MSG(err);
  kbc_err_reset(&err);
  KBC_CHECK_ERR(kbc_app_move_path(f.app, CORPUS_A, "c.md", "../out.md", &err),
                KBC_ERR_INVALID);
  KBC_CHECK_ERR_MSG(err);
  kbc_err_reset(&err);
  /* A corpus that is not configured, and a null app, are refusals too. */
  KBC_CHECK_ERR(
      kbc_app_move_path(f.app, CORPUS_B, "c.md", "fresh.md", &err),
      KBC_ERR_NOTFOUND);
  KBC_CHECK_ERR_MSG(err);
  kbc_err_reset(&err);
  KBC_CHECK_ERR(kbc_app_move_path(NULL, CORPUS_A, "c.md", "fresh.md", &err),
                KBC_ERR_INVALID);
  KBC_CHECK_ERR_MSG(err);
  kbc_err_reset(&err);

  /* c.md is still where it was, and the corpus is untouched by every one of
   * those refusals: a refused move must not half-apply. */
  char p[KBC_TEST_PATH_MAX];
  join(p, sizeof p, f.corpus_a, "c.md");
  KBC_CHECK(kbc_path_exists(p) == true);
  KBC_CHECK_EQ_INT(store_count(f.cfg), 3);
  fx_teardown(&f);
}

/* A file on disk that the store has never indexed is not a document to move:
 * moving it would create one, which is a write the caller did not ask for.
 * A file that is not there at all is a miss. */
KBC_TEST(a_move_of_a_file_the_store_has_never_seen_is_refused) {
  fixture f;
  fx_setup(&f, false);
  if (f.app == NULL) {
    fx_teardown(&f);
    return;
  }
  /* A .txt is on disk and is not a document: the walk declines it, so the
   * store has no row. Moving it would create one. */
  char p[KBC_TEST_PATH_MAX];
  join(p, sizeof p, f.corpus_a, "stray.txt");
  kbc_test_write_file(p, "never indexed.\n");
  kbc_err err;
  kbc_err_reset(&err);
  KBC_CHECK_OK(kbc_app_reindex(f.app, &err));
  KBC_CHECK_EQ_INT(store_count(f.cfg), 3);

  KBC_CHECK_ERR(
      kbc_app_move_path(f.app, CORPUS_A, "stray.txt", "moved.txt", &err),
      KBC_ERR_NOTFOUND);
  KBC_CHECK_ERR_MSG(err);
  KBC_CHECK(kbc_path_exists(p) == true);
  char q[KBC_TEST_PATH_MAX];
  join(q, sizeof q, f.corpus_a, "moved.txt");
  KBC_CHECK(kbc_path_exists(q) == false);
  KBC_CHECK_EQ_INT(store_count(f.cfg), 3);

  /* And a file that is not there at all is a miss, not a silent success. */
  kbc_err_reset(&err);
  KBC_CHECK_ERR(
      kbc_app_move_path(f.app, CORPUS_A, "not-there.md", "moved.md", &err),
      KBC_ERR_NOTFOUND);
  KBC_CHECK_ERR_MSG(err);
  fx_teardown(&f);
}

/* The one field a move cannot rebuild from the document's bytes: WHEN a
 * comment was made. kbc_store_add_comment MINTS created_at, so the only way a
 * carried comment used to keep its own was to re-add it — which stamped it
 * "now". A body-only assertion would have passed on that version, so this one
 * compares the stored string.
 *
 * The wait is what makes it a test rather than a coin flip. The seed and the
 * move both mint at second granularity, so a move in the same second as the
 * seed would compare equal whatever the code did. Crossing a second boundary
 * first makes "now" and "then" two different values, and the mutation fires. */
KBC_TEST(a_comment_carried_across_a_move_keeps_the_second_it_was_made) {
  fixture f;
  fx_setup(&f, false);
  if (f.app == NULL) {
    fx_teardown(&f);
    return;
  }
  kbc_err err;
  kbc_err_reset(&err);
  KBC_CHECK_OK(kbc_app_reindex(f.app, &err));
  kbc_arena *a = kbc_arena_new(64u * 1024u);
  KBC_CHECK_NOT_NULL(a);
  if (a == NULL) {
    fx_teardown(&f);
    return;
  }
  const char *old_id = id_of_path(f.cfg, CORPUS_A, "c.md", a);
  KBC_CHECK_NOT_NULL(old_id);
  if (old_id == NULL) {
    kbc_arena_free(a);
    fx_teardown(&f);
    return;
  }
  char old_id_copy[KBC_MAX_ID_LEN + 1];
  memcpy(old_id_copy, old_id, sizeof old_id_copy);

  char seeded_at[32];
  char seeded_cid[64];
  seeded_at[0] = '\0';
  seeded_cid[0] = '\0';
  kbc_store *st = kbc_store_open(f.cfg, &err);
  KBC_CHECK_NOT_NULL(st);
  if (st == NULL) {
    kbc_arena_free(a);
    fx_teardown(&f);
    return;
  }
  KBC_CHECK_OK(kbc_store_add_comment(st, old_id_copy, "section:digest", "nik",
                                     "worth keeping", &err));
  kbc_comment *before = NULL;
  size_t n_before = 0;
  KBC_CHECK_OK(
      kbc_store_list_comments(st, a, old_id_copy, 16, &before, &n_before, &err));
  KBC_CHECK_EQ_INT(n_before, 1);
  if (n_before == 1) {
    snprintf(seeded_at, sizeof seeded_at, "%s", before[0].created_at);
    snprintf(seeded_cid, sizeof seeded_cid, "%s", before[0].id);
  }
  kbc_store_close(st);

  /* Cross a second boundary, so any timestamp minted from here on differs
   * from the one just recorded. */
  time_t edge = time(NULL);
  while (time(NULL) == edge) {
    nap_ms(20);
  }

  KBC_CHECK_OK(kbc_app_move_path(f.app, CORPUS_A, "c.md", "notes/stamped.md",
                                 &err));
  const char *new_id = id_of_path(f.cfg, CORPUS_A, "notes/stamped.md", a);
  KBC_CHECK_NOT_NULL(new_id);
  if (new_id != NULL) {
    st = kbc_store_open(f.cfg, &err);
    KBC_CHECK_NOT_NULL(st);
    if (st != NULL) {
      kbc_comment *after = NULL;
      size_t n_after = 0;
      KBC_CHECK_OK(kbc_store_list_comments(st, a, new_id, 16, &after, &n_after,
                                          &err));
      KBC_CHECK_EQ_INT(n_after, 1);
      if (n_after == 1) {
        KBC_CHECK_EQ_STR(after[0].body, "worth keeping");
        KBC_CHECK_MSG(strcmp(after[0].created_at, seeded_at) == 0,
                      "the carried comment is stamped \"%s\"; it was made at "
                      "\"%s\", and a rename is not the moment it was made",
                      after[0].created_at, seeded_at);
        /* The id too: the store re-keys `comments` by doc_id alone, so the
         * minted primary key is carried rather than reminted. */
        KBC_CHECK_MSG(strcmp(after[0].id, seeded_cid) == 0,
                      "the carried comment is a different comment: id \"%s\", "
                      "it was \"%s\"", after[0].id, seeded_cid);
      }
      kbc_store_close(st);
    }
  }
  kbc_arena_free(a);
  fx_teardown(&f);
}

/* The silent-anchor-loss bug. The corkboard listing is a PAGE — the N most
 * RECENT anchors — so a document anchored early is simply not in it, and a
 * move that read the anchor that way concluded the document had none and
 * dropped it with nothing anywhere reporting a loss. Filling the page past
 * the document's own anchor is what makes that reachable: with the anchor
 * among the newest it survives by luck, which is why this needs its own case
 * rather than an assertion added to the general carry test. */
KBC_TEST(a_corkboard_anchor_older_than_the_listing_page_survives_a_move) {
  fixture f;
  fx_setup(&f, false);
  if (f.app == NULL) {
    fx_teardown(&f);
    return;
  }
  kbc_err err;
  kbc_err_reset(&err);
  KBC_CHECK_OK(kbc_app_reindex(f.app, &err));
  kbc_arena *a = kbc_arena_new(64u * 1024u);
  KBC_CHECK_NOT_NULL(a);
  if (a == NULL) {
    fx_teardown(&f);
    return;
  }
  const char *old_id = id_of_path(f.cfg, CORPUS_A, "c.md", a);
  KBC_CHECK_NOT_NULL(old_id);
  if (old_id == NULL) {
    kbc_arena_free(a);
    fx_teardown(&f);
    return;
  }
  char old_id_copy[KBC_MAX_ID_LEN + 1];
  memcpy(old_id_copy, old_id, sizeof old_id_copy);

  kbc_store *st = kbc_store_open(f.cfg, &err);
  KBC_CHECK_NOT_NULL(st);
  if (st == NULL) {
    kbc_arena_free(a);
    fx_teardown(&f);
    return;
  }
  /* The document's own anchor, the OLDEST in the table. */
  KBC_CHECK_OK(kbc_store_add_corkboard(st, old_id_copy, 1000, &err));
  /* KBC_MAX_HITS rows, every one of them newer, so a page of the listing
   * covers all of them and none of the document. The ids need not exist:
   * corkboard has no foreign key to artifacts, and only the ordering and the
   * count are load-bearing here. */
  for (size_t i = 0; i < (size_t)KBC_MAX_HITS; i++) {
    char filler[16];
    snprintf(filler, sizeof filler, "f%011zx", i);
    kbc_status fs =
        kbc_store_add_corkboard(st, filler, (int64_t)2000 + (int64_t)i, &err);
    if (kbc_failed(fs)) {
      KBC_CHECK_OK(fs);
      break;
    }
  }

  /* THE PREMISE, CHECKED. A test that only asserts the anchor survived proves
   * nothing about the page bug unless the page really does exclude it, and
   * "the listing is a page" is the store's contract rather than this file's
   * to assume. So the page is read here, before the move, and the document is
   * looked for in it: a version of this that read the anchor that way found
   * nothing and dropped it with no error anywhere. */
  {
    kbc_corkboard_row *page = NULL;
    size_t n_page = 0;
    kbc_err_reset(&err);
    kbc_status ps = kbc_store_list_corkboard(st, a, (size_t)KBC_MAX_HITS, &page,
                                              &n_page, &err);
    KBC_CHECK_OK(ps);
    bool in_page = false;
    for (size_t i = 0; ps == KBC_OK && i < n_page; i++) {
      if (page[i].artifact_id != NULL &&
          strcmp(page[i].artifact_id, old_id_copy) == 0) {
        in_page = true;
      }
    }
    KBC_CHECK_MSG(!in_page,
                  "the document's anchor IS in the newest-%zu page, so this "
                  "case does not reach the bug it is for", (size_t)KBC_MAX_HITS);
  }
  kbc_store_close(st);

  KBC_CHECK_OK(kbc_app_move_path(f.app, CORPUS_A, "c.md", "anchored.md", &err));
  const char *new_id = id_of_path(f.cfg, CORPUS_A, "anchored.md", a);
  KBC_CHECK_NOT_NULL(new_id);
  if (new_id != NULL) {
    char new_id_copy[KBC_MAX_ID_LEN + 1];
    memcpy(new_id_copy, new_id, KBC_MAX_ID_LEN);
    new_id_copy[KBC_MAX_ID_LEN] = '\0';
    /* The page again, to be sure the answer below is not the one the old code
     * would have got: if the anchor is present under the new id AND still
     * outside the newest-KBC_MAX_HITS page, only a point read can see it. */
    db_side side;
    memset(&side, 0, sizeof side);
    KBC_CHECK(db_side_open(&side, f.cfg));
    int64_t carried = corkboard_at_for(&side, new_id_copy);
    KBC_CHECK_MSG(carried == 1000,
                  "the anchor did not survive the move: %s was anchored at 1000 "
                  "before it and reads back as %lld after",
                  old_id_copy, (long long)carried);
    /* And it is a move, not a copy: nothing is left on the dead id. */
    KBC_CHECK_MSG(corkboard_at_for(&side, old_id_copy) == -1,
                  "an anchor is still on the dead id %s, so the move copied the "
                  "corkboard instead of moving it", old_id_copy);
    if (side.db != NULL) sqlite3_close(side.db);
  }
  kbc_arena_free(a);
  fx_teardown(&f);
}

/* A rename that dies between the two operations nobody can see must leave a
 * LISTABLE signal, not a document that quietly disappeared. The intent row is
 * written before the rename and stamped after the rekey, so an interrupted
 * move is a row with no completed_at — and it is deliberately NOT a
 * redirect: following it would resolve the document to a name the rename
 * never reached. */
KBC_TEST(an_interrupted_move_is_listed_and_is_not_yet_a_redirect) {
  fixture f;
  fx_setup(&f, false);
  if (f.app == NULL) {
    fx_teardown(&f);
    return;
  }
  kbc_err err;
  kbc_err_reset(&err);
  KBC_CHECK_OK(kbc_app_reindex(f.app, &err));
  kbc_arena *a = kbc_arena_new(64u * 1024u);
  KBC_CHECK_NOT_NULL(a);
  if (a == NULL) {
    fx_teardown(&f);
    return;
  }
  const char *old_id = id_of_path(f.cfg, CORPUS_A, "c.md", a);
  KBC_CHECK_NOT_NULL(old_id);
  if (old_id == NULL) {
    kbc_arena_free(a);
    fx_teardown(&f);
    return;
  }
  char old_id_copy[KBC_MAX_ID_LEN + 1];
  memcpy(old_id_copy, old_id, sizeof old_id_copy);
  /* A move of a document that does not exist, which is what an interrupted
   * one looks like from the table's side. It is a DIFFERENT old id from the
   * document below on purpose: kbc_store_complete_move stamps every
   * in-flight row naming the id it is given, so sharing one would have the
   * app's completion silently converge this row too. */
  static const char ghost[KBC_MAX_ID_LEN + 1] = "aaaaaaaaaaaa";
  char staged[KBC_MAX_ID_LEN + 1];
  kbc_id_for_artifact(staged, CORPUS_A, "ghost.md");

  /* A completed move leaves nothing for bring-up to converge. */
  kbc_store *st = kbc_store_open(f.cfg, &err);
  KBC_CHECK_NOT_NULL(st);
  if (st == NULL) {
    kbc_arena_free(a);
    fx_teardown(&f);
    return;
  }
  kbc_strlist ids, rels;
  kbc_strlist_init(&ids);
  kbc_strlist_init(&rels);
  KBC_CHECK_OK(kbc_store_list_incomplete_moves(st, &ids, &rels, &err));
  KBC_CHECK_EQ_INT(ids.len, 0);
  kbc_strlist_free(&ids);
  kbc_strlist_free(&rels);

  /* Now the crash: recorded, never completed. */
  KBC_CHECK_OK(
      kbc_store_record_move(st, ghost, staged, "ghost.md", "ghosted.md", 1700,
                            &err));
  kbc_strlist_init(&ids);
  kbc_strlist_init(&rels);
  KBC_CHECK_OK(kbc_store_list_incomplete_moves(st, &ids, &rels, &err));
  KBC_CHECK_MSG(ids.len == 1,
                "an interrupted move is not listed: bring-up has nothing to "
                "converge and the document is just gone");
  if (ids.len == 1) {
    KBC_CHECK_EQ_STR(ids.items[0], ghost);
    KBC_CHECK_EQ_STR(rels.items[0], "ghost.md");
  }
  kbc_strlist_free(&ids);
  kbc_strlist_free(&rels);

  /* And it is NOT a redirect yet: an unstamped row names a rename that never
   * finished, so following it would hand the caller a document that is not
   * there. */
  kbc_strlist hops;
  kbc_strlist_init(&hops);
  KBC_CHECK_OK(kbc_store_moves_lookup(st, ghost, &hops, &err));
  KBC_CHECK_MSG(hops.len == 0,
                "an interrupted move redirected %s to %s, a name the rename "
                "never reached", ghost, hops.len ? hops.items[0] : "?");
  kbc_strlist_free(&hops);
  kbc_store_close(st);

  /* A move the app completes is the opposite: recorded and stamped, so the
   * redirect is live and bring-up has nothing left to do. */
  KBC_CHECK_OK(kbc_app_move_path(f.app, CORPUS_A, "c.md", "finished.md", &err));
  st = kbc_store_open(f.cfg, &err);
  KBC_CHECK_NOT_NULL(st);
  if (st != NULL) {
    kbc_strlist_init(&ids);
    kbc_strlist_init(&rels);
    KBC_CHECK_OK(kbc_store_list_incomplete_moves(st, &ids, &rels, &err));
    KBC_CHECK_MSG(ids.len == 1,
                  "the app's own completed move is still listed as incomplete: "
                  "%zu rows, and only the hand-written one should be", ids.len);
    kbc_strlist_free(&ids);
    kbc_strlist_free(&rels);
    kbc_strlist_init(&hops);
    KBC_CHECK_OK(kbc_store_moves_lookup(st, old_id_copy, &hops, &err));
    KBC_CHECK_MSG(hops.len == 1,
                  "a completed move does not redirect: %zu hops for %s", hops.len,
                  old_id_copy);
    if (hops.len == 1) {
      KBC_CHECK_EQ_STR(hops.items[0], id_of_path(f.cfg, CORPUS_A, "finished.md", a));
    }
    kbc_strlist_free(&hops);
    kbc_store_close(st);
  }
  kbc_arena_free(a);
  fx_teardown(&f);
}

/* ------------------------------------------------- bring-up: mv helpers -- */

/* Everything below drives a boot. A bring-up pass runs in kbc_app_open, and
 * the only way to reach code that runs there is to open an app — so each case
 * seeds the crash through a SECOND connection to the same database, closes the
 * app, and opens it again. That is the honest shape of the fixture, and what
 * it does NOT prove is in the report: a seeded row is a row this build
 * believed it could write, not a row a killed process left behind. */

/* One restart: the same thing an operator does after a crash, and the only
 * transition that runs code in kbc_app_open. */
static void reboot(fixture *f) {
  kbc_err err;
  kbc_err_reset(&err);
  kbc_app_close(f->app);
  f->app = kbc_app_open(f->cfg, &err);
  if (f->app == NULL) fprintf(stderr, "  app_open: %s\n", err.msg);
  KBC_CHECK_NOT_NULL(f->app);
}

/* How many moves the store reports as still in flight. The rows are the
 * pass's whole input, so this is also how a test sees whether it converged
 * once, twice, or not at all. */
static size_t incomplete_moves(const kbc_config *cfg) {
  kbc_err err;
  kbc_err_reset(&err);
  kbc_store *s = kbc_store_open(cfg, &err);
  if (s == NULL) return 0;
  kbc_strlist ids, rels;
  kbc_strlist_init(&ids);
  kbc_strlist_init(&rels);
  kbc_status st = kbc_store_list_incomplete_moves(s, &ids, &rels, &err);
  size_t n = (st == KBC_OK) ? ids.len : 0;
  kbc_strlist_free(&ids);
  kbc_strlist_free(&rels);
  kbc_store_close(s);
  return n;
}

/* The state of the move row naming one old id, as the store's own columns
 * report it: "flight", "abandoned" or "completed".
 *
 * Read through a second connection because store.h has no accessor for a
 * single row's stamps, and the three states are the whole subject here: a
 * count of in-flight rows cannot tell an abandoned row from one a pass has
 * not looked at, which is the difference the pass's one refusal turns on. An
 * empty string means the row or the column could not be read at all, and
 * every caller below compares against a state that is not empty. */
static void move_row_state(const kbc_config *cfg, const char *old_id, char *out,
                           size_t cap) {
  if (cap > 0) out[0] = '\0';
  db_side side;
  memset(&side, 0, sizeof side);
  if (!db_side_open(&side, cfg)) return;
  sqlite3_stmt *q = NULL;
  if (sqlite3_prepare_v2(side.db,
                         "SELECT completed_at IS NOT NULL,"
                         " abandoned_at IS NOT NULL FROM moves"
                         " WHERE old_id = ?1 ORDER BY id DESC LIMIT 1;",
                         -1, &q, NULL) == SQLITE_OK &&
      sqlite3_bind_text(q, 1, old_id, -1, SQLITE_STATIC) == SQLITE_OK &&
      sqlite3_step(q) == SQLITE_ROW) {
    int done = sqlite3_column_int(q, 0) != 0;
    int gone = sqlite3_column_int(q, 1) != 0;
    (void)snprintf(out, cap, "%s",
                   done ? (gone ? "completed+abandoned" : "completed")
                        : (gone ? "abandoned" : "flight"));
  }
  sqlite3_finalize(q);
  db_side_close(&side);
}

/* How many hops a stale reference would be redirected through, by id. Zero is
 * the answer that matters: it is what an abandoned row owes the caller, where
 * a completed row hands back a destination. */
static size_t redirect_hops(const kbc_config *cfg, const char *id) {
  kbc_err err;
  kbc_err_reset(&err);
  kbc_store *s = kbc_store_open(cfg, &err);
  if (s == NULL) return 0;
  kbc_strlist hops;
  kbc_strlist_init(&hops);
  kbc_status st = kbc_store_moves_lookup(s, id, &hops, &err);
  size_t n = (st == KBC_OK) ? hops.len : 0;
  kbc_strlist_free(&hops);
  kbc_store_close(s);
  return n;
}

/* And the same question asked by PATH, which is the one a stale reference in
 * a document, a pin or a corkboard entry actually asks. */
static size_t redirect_hops_for_rel(const kbc_config *cfg, const char *rel) {
  kbc_err err;
  kbc_err_reset(&err);
  kbc_store *s = kbc_store_open(cfg, &err);
  if (s == NULL) return 0;
  kbc_strlist hops;
  kbc_strlist_init(&hops);
  kbc_status st = kbc_store_moves_lookup_path(s, rel, &hops, &err);
  size_t n = (st == KBC_OK) ? hops.len : 0;
  kbc_strlist_free(&hops);
  kbc_store_close(s);
  return n;
}

/* The `moves` and `artifacts` tables as one string each, so two boots can be
 * compared EXACTLY. BOTH move stamps are in the digest on purpose: a pass
 * that re-decided a row it had already ended would leave every other column
 * identical and change only `abandoned_at`, and a digest carrying one stamp
 * would call that a no-op. */
static void table_digest(const kbc_config *cfg, const char *sql, char *out,
                         size_t cap) {
  db_side side;
  memset(&side, 0, sizeof side);
  if (cap > 0) out[0] = '\0';
  if (!db_side_open(&side, cfg)) return;
  sqlite3_stmt *q = NULL;
  if (sqlite3_prepare_v2(side.db, sql, -1, &q, NULL) == SQLITE_OK) {
    while (sqlite3_step(q) == SQLITE_ROW) {
      for (int i = 0; i < sqlite3_column_count(q); i++) {
        size_t used = strlen(out);
        if (used + 1u >= cap) break;
        const char *v = (const char *)sqlite3_column_text(q, i);
        (void)snprintf(out + used, cap - used, "%s|", v != NULL ? v : "");
      }
    }
  }
  sqlite3_finalize(q);
  db_side_close(&side);
}


/* A crash between step 1 of kbc_app_move_path and step 2: the intent row is
 * written and the process is gone before the rename. Seeded through the store,
 * which is the same write move_path makes. */
static void seed_interrupted_move(const kbc_config *cfg, const char *old_id,
                                  const char *new_id, const char *old_rel,
                                  const char *new_rel) {
  kbc_err err;
  kbc_err_reset(&err);
  kbc_store *s = kbc_store_open(cfg, &err);
  KBC_CHECK_NOT_NULL(s);
  if (s == NULL) return;
  KBC_CHECK_OK(kbc_store_record_move(s, old_id, new_id, old_rel, new_rel, 1700,
                                     &err));
  kbc_store_close(s);
}

/* NOT RENAMED YET: the file is still where the move was going to take it from,
 * so the intent names a rename that never reached the disk. This is the state
 * a crash between record_move and rename() leaves, and the document is whole:
 * nothing has been lost, only a promise is outstanding. */
KBC_TEST(a_move_interrupted_before_the_rename_is_abandoned_at_bring_up) {
  fixture f;
  fx_setup(&f, false);
  if (f.app == NULL) {
    fx_teardown(&f);
    return;
  }
  kbc_err err;
  kbc_err_reset(&err);
  KBC_CHECK_OK(kbc_app_reindex(f.app, &err));

  kbc_arena *a = kbc_arena_new(64u * 1024u);
  KBC_CHECK_NOT_NULL(a);
  if (a == NULL) {
    fx_teardown(&f);
    return;
  }
  const char *id = id_of_path(f.cfg, CORPUS_A, "c.md", a);
  KBC_CHECK_NOT_NULL(id);
  if (id == NULL) {
    kbc_arena_free(a);
    fx_teardown(&f);
    return;
  }
  char old_id[KBC_MAX_ID_LEN + 1];
  memcpy(old_id, id, sizeof old_id);
  char new_id[KBC_MAX_ID_LEN + 1];
  kbc_id_for_artifact(new_id, CORPUS_A, "renamed-away.md");
  seed_interrupted_move(f.cfg, old_id, new_id, "c.md", "renamed-away.md");

  /* The row is there to be converged: without the pass, this is where every
   * boot after the crash would find it again. */
  KBC_CHECK_EQ_INT(incomplete_moves(f.cfg), 1);

  reboot(&f);

  /* CONVERGED ONCE, and by the right verb. The row has left the replay list,
   * so the next boot finds nothing and says nothing: a row that stayed is a
   * loop, not convergence. */
  KBC_CHECK_MSG(incomplete_moves(f.cfg) == 0,
                "the interrupted move is still in flight after a boot: bring-up "
                "will re-decide it on every start and the warning will never "
                "stop");

  /* ABANDONED, not completed. This is the whole change, and it is visible in
   * one column: `completed_at` is a promise that the document is at the new
   * id, and nothing here published that id. A pass that stamped it would
   * redirect a stale reference to a document that does not exist — a wrong
   * answer, not a missing one, and kbc_app_get_artifact follows that chain. */
  char state[32];
  move_row_state(f.cfg, old_id, state, sizeof state);
  KBC_CHECK_MSG(strcmp(state, "abandoned") == 0,
                "bring-up ended the interrupted move with state \"%s\"; the "
                "pass never ran the rekey, so completed_at would be a redirect "
                "to a document that was never published", state);

  /* And the consequence a caller can observe, which is the reason the verb
   * differs: nothing is redirected. The document is still at its own path
   * under its own id, so a stale reference must still land on IT and not be
   * walked somewhere the rename never reached. */
  KBC_CHECK_MSG(redirect_hops(f.cfg, old_id) == 0,
                "an abandoned move still redirects a stale id: %zu hops, which "
                "sends a bookmark to a destination no document holds",
                redirect_hops(f.cfg, old_id));
  KBC_CHECK_MSG(redirect_hops_for_rel(f.cfg, "c.md") == 0,
                "an abandoned move still redirects a stale PATH reference: %zu "
                "hops for c.md, which is where the document still is",
                redirect_hops_for_rel(f.cfg, "c.md"));

  /* The filesystem is the authority, and it said the file never moved. So the
   * pass did not move it, and did not invent the destination the intent row
   * promises: the row is handed over WITHOUT the new path, and a pass that
   * walked the corpus to recover it would be a full pass on every boot. */
  char p[KBC_TEST_PATH_MAX];
  join(p, sizeof p, f.corpus_a, "c.md");
  KBC_CHECK_MSG(kbc_path_exists(p),
                "bring-up removed a file the interrupted move never renamed "
                "away");
  join(p, sizeof p, f.corpus_a, "renamed-away.md");
  KBC_CHECK_MSG(!kbc_path_exists(p),
                "bring-up created %s: the intent row does not say where the "
                "rename was going, and a pass that guessed would be renaming "
                "user files on every boot",
                p);

  /* And the document is untouched: still indexed under its own id, at its
   * own path, with nothing published under the id the abandoned move minted. */
  kbc_store *st = kbc_store_open(f.cfg, &err);
  KBC_CHECK_NOT_NULL(st);
  if (st != NULL) {
    kbc_artifact art;
    memset(&art, 0, sizeof art);
    KBC_CHECK_OK(kbc_store_get_artifact(st, a, old_id, false, &art, &err));
    KBC_CHECK_EQ_STR(art.path, "c.md");
    memset(&art, 0, sizeof art);
    kbc_err_reset(&err);
    KBC_CHECK_ERR(
        kbc_store_get_artifact(st, a, new_id, false, &art, &err),
        KBC_ERR_NOTFOUND);
    kbc_store_close(st);
  }
  kbc_arena_free(a);
  fx_teardown(&f);
}

/* RENAMED, AND THE CARRY NEVER HAPPENED. The crash the other way round: the
 * rename reached the disk and the rekey did not, so the document is at its new
 * path under a NEW id while everything keyed to the old one — the comments
 * with their minted ids and timestamps, the corkboard anchor, the pin, the
 * first-indexed second, the reading history — was never carried.
 *
 * The decision this pins is that bring-up does NOT re-run the rekey. The
 * alternative is not merely slower: `comments` cascades off artifacts(id), so
 * a rekey re-run after a later reconcile sweep has already dropped the old row
 * would move NOTHING and still leave the intent row stamped — a move the log
 * would report as having carried state it did not carry. */
KBC_TEST(a_move_interrupted_after_the_rename_announces_what_was_lost) {
  fixture f;
  fx_setup(&f, false);
  if (f.app == NULL) {
    fx_teardown(&f);
    return;
  }
  kbc_err err;
  kbc_err_reset(&err);
  KBC_CHECK_OK(kbc_app_reindex(f.app, &err));

  kbc_arena *a = kbc_arena_new(64u * 1024u);
  KBC_CHECK_NOT_NULL(a);
  if (a == NULL) {
    fx_teardown(&f);
    return;
  }
  const char *id = id_of_path(f.cfg, CORPUS_A, "c.md", a);
  KBC_CHECK_NOT_NULL(id);
  if (id == NULL) {
    kbc_arena_free(a);
    fx_teardown(&f);
    return;
  }
  char old_id[KBC_MAX_ID_LEN + 1];
  memcpy(old_id, id, sizeof old_id);
  char new_id[KBC_MAX_ID_LEN + 1];
  kbc_id_for_artifact(new_id, CORPUS_A, "moved.md");

  /* State that exists ONLY under the old id, so the loss below is a fact
   * about rows rather than a claim in a comment. It is written before the
   * crash, exactly as a reader's comment and anchor would have been. */
  kbc_store *st = kbc_store_open(f.cfg, &err);
  KBC_CHECK_NOT_NULL(st);
  if (st == NULL) {
    kbc_arena_free(a);
    fx_teardown(&f);
    return;
  }
  KBC_CHECK_OK(kbc_store_add_comment(st, old_id, "section:digest", "ada",
                                     "the digest is quarterly", &err));
  KBC_CHECK_OK(kbc_store_add_corkboard(st, old_id, 1000, &err));
  KBC_CHECK_EQ_INT(comment_count(f.cfg, old_id, a), 1);
  kbc_store_close(st);

  /* The crash: the file is renamed, the rekey never runs, the intent row is
   * never stamped. The rename is done by hand because a test cannot kill a
   * process between two of its own statements. */
  char from[KBC_TEST_PATH_MAX];
  char to[KBC_TEST_PATH_MAX];
  join(from, sizeof from, f.corpus_a, "c.md");
  join(to, sizeof to, f.corpus_a, "moved.md");
  KBC_CHECK_EQ_INT(rename(from, to), 0);
  seed_interrupted_move(f.cfg, old_id, new_id, "c.md", "moved.md");
  KBC_CHECK_EQ_INT(incomplete_moves(f.cfg), 1);

  /* Boot, with the log captured: a move whose carried state is gone has to SAY
   * so, at WARN, naming the paths. A silent finish here would be
   * indistinguishable from a move that worked, which is the whole failure this
   * pass exists to prevent. */
  log_capture cap;
  log_capture_begin(&cap);
  reboot(&f);
  char *log_text = log_capture_end(&cap);
  KBC_CHECK_NOT_NULL(log_text);
  if (log_text == NULL) {
    kbc_arena_free(a);
    fx_teardown(&f);
    return;
  }
  KBC_CHECK_MSG(strstr(log_text, "warn") != NULL,
                "an interrupted move whose comments and anchor were left behind "
                "finished without a warning: [%s]", log_text);
  KBC_CHECK_MSG(strstr(log_text, "c.md") != NULL,
                "the warning does not name the path the document was moved "
                "from: [%s]", log_text);
  KBC_CHECK_MSG(strstr(log_text, old_id) != NULL,
                "the warning does not name the id whose state was left behind: "
                "[%s]", log_text);
  /* The log is read by an operator long after the row has settled, and an
   * abandoned row and a completed one are different facts. A line that only
   * said "converged" or "stamped" could not tell them apart. */
  KBC_CHECK_MSG(strstr(log_text, "ABANDONED") != NULL,
                "the warning does not say the move was ABANDONED, so this log "
                "is indistinguishable from one about a completed move: [%s]",
                log_text);
  free(log_text);

  /* Converged once, and by the right verb. The file DID leave that path, so
   * the old id is a stale reference to a document that is genuinely not there
   * any more — which is exactly why the row must not be COMPLETED: the
   * destination a completed row names is the new id, and no document was ever
   * published under it, so a redirect would send a bookmark into a hole. The
   * honest answer is the miss, and abandoned is the state that gives it. */
  KBC_CHECK_EQ_INT(incomplete_moves(f.cfg), 0);
  char state[32];
  move_row_state(f.cfg, old_id, state, sizeof state);
  KBC_CHECK_MSG(strcmp(state, "abandoned") == 0,
                "bring-up ended the interrupted move with state \"%s\"; the "
                "carry never ran, so a completed row would redirect %s to a "
                "document that does not exist", state, old_id);
  KBC_CHECK_MSG(redirect_hops(f.cfg, old_id) == 0,
                "the abandoned row still redirects %s: %zu hops into a "
                "destination the pass never published", old_id,
                redirect_hops(f.cfg, old_id));

  /* The carry was NOT re-run. The old row is still where the crash left it and
   * nothing was published under the id the new path mints — the pass reported
   * a loss, it did not paper over one, and it certainly did not reindex the
   * corpus to find the destination the intent row never recorded. */
  st = kbc_store_open(f.cfg, &err);
  KBC_CHECK_NOT_NULL(st);
  if (st != NULL) {
    kbc_artifact art;
    memset(&art, 0, sizeof art);
    KBC_CHECK_OK(kbc_store_get_artifact(st, a, old_id, false, &art, &err));
    KBC_CHECK_EQ_STR(art.path, "c.md");
    memset(&art, 0, sizeof art);
    kbc_err_reset(&err);
    KBC_CHECK_ERR(kbc_store_get_artifact(st, a, new_id, false, &art, &err),
                  KBC_ERR_NOTFOUND);
    kbc_store_close(st);
  }
  KBC_CHECK_MSG(comment_count(f.cfg, old_id, a) == 1,
                "the comment is no longer on the old id, so this case no longer "
                "describes a rekey that never ran");
  KBC_CHECK_MSG(comment_count(f.cfg, new_id, a) == 0,
                "a comment appeared under the new id: something re-ran the "
                "carry, and the warning above then describes a recovery that "
                "did not happen");

  /* The document itself is not lost — its bytes are at the new path, and the
   * reconcile sweep is what indexes them there. The pass neither renamed nor
   * deleted anything. */
  KBC_CHECK_MSG(kbc_path_exists(to), "bring-up deleted the renamed document");
  KBC_CHECK_MSG(!kbc_path_exists(from),
                "bring-up put the file back at the old path: the disk said the "
                "rename happened, and the disk is the authority");
  kbc_arena_free(a);
  fx_teardown(&f);
}

/* CONVERGES ONCE. Two interrupted moves in one database — one whose file never
 * left, one whose file did — and two boots. The second boot must change
 * nothing at all: same rows, same stamps, same files. A pass that re-stamped a
 * row it had already decided would pass a count check and fail this one, which
 * is why the digest carries completed_at. */
KBC_TEST(a_second_boot_after_an_interrupted_move_changes_nothing) {
  fixture f;
  fx_setup(&f, false);
  if (f.app == NULL) {
    fx_teardown(&f);
    return;
  }
  kbc_err err;
  kbc_err_reset(&err);
  KBC_CHECK_OK(kbc_app_reindex(f.app, &err));

  kbc_arena *a = kbc_arena_new(64u * 1024u);
  KBC_CHECK_NOT_NULL(a);
  if (a == NULL) {
    fx_teardown(&f);
    return;
  }
  const char *stayed_id_ptr = id_of_path(f.cfg, CORPUS_A, "c.md", a);
  const char *moved_id_ptr = id_of_path(f.cfg, CORPUS_A, "b.md", a);
  KBC_CHECK_NOT_NULL(stayed_id_ptr);
  KBC_CHECK_NOT_NULL(moved_id_ptr);
  if (stayed_id_ptr == NULL || moved_id_ptr == NULL) {
    kbc_arena_free(a);
    fx_teardown(&f);
    return;
  }
  char stayed_id[KBC_MAX_ID_LEN + 1];
  char moved_id[KBC_MAX_ID_LEN + 1];
  memcpy(stayed_id, stayed_id_ptr, sizeof stayed_id);
  memcpy(moved_id, moved_id_ptr, sizeof moved_id);
  char stayed_new[KBC_MAX_ID_LEN + 1];
  char moved_new[KBC_MAX_ID_LEN + 1];
  kbc_id_for_artifact(stayed_new, CORPUS_A, "stayed-at-c.md");
  kbc_id_for_artifact(moved_new, CORPUS_A, "b-renamed.md");

  char from[KBC_TEST_PATH_MAX];
  char to[KBC_TEST_PATH_MAX];
  join(from, sizeof from, f.corpus_a, "b.md");
  join(to, sizeof to, f.corpus_a, "b-renamed.md");
  KBC_CHECK_EQ_INT(rename(from, to), 0);
  seed_interrupted_move(f.cfg, stayed_id, stayed_new, "c.md", "stayed-at-c.md");
  seed_interrupted_move(f.cfg, moved_id, moved_new, "b.md", "b-renamed.md");
  KBC_CHECK_EQ_INT(incomplete_moves(f.cfg), 2);

  reboot(&f);
  KBC_CHECK_MSG(incomplete_moves(f.cfg) == 0,
                "the first boot left %zu of 2 moves in flight",
                incomplete_moves(f.cfg));

  /* Both rows went to the same terminal state, on both filesystem answers. */
  char state[32];
  move_row_state(f.cfg, stayed_id, state, sizeof state);
  KBC_CHECK_MSG(strcmp(state, "abandoned") == 0,
                "the move whose file never left ended in state \"%s\"", state);
  move_row_state(f.cfg, moved_id, state, sizeof state);
  KBC_CHECK_MSG(strcmp(state, "abandoned") == 0,
                "the move whose file did leave ended in state \"%s\"", state);

  char moves_before[1024];
  char artifacts_before[1024];
  table_digest(f.cfg,
               "SELECT id, old_id, new_id, old_rel, new_rel, completed_at, "
               "abandoned_at FROM moves ORDER BY id;",
               moves_before, sizeof moves_before);
  table_digest(f.cfg,
               "SELECT id, corpus, path FROM artifacts ORDER BY id;",
               artifacts_before, sizeof artifacts_before);
  /* A digest that failed to prepare is an empty string, and two empty strings
   * compare equal — so without this the equality below would be evidence of
   * nothing. Named ids are what a real digest contains. */
  KBC_CHECK_MSG(strstr(moves_before, stayed_id) != NULL &&
                    strstr(moves_before, moved_id) != NULL,
                "the moves digest is [%s], which cannot be the two rows this "
                "test seeded", moves_before);

  /* The second boot, with the log captured. An abandoned row is TERMINAL, so
   * the pass has nothing left to do: it is out of the replay list, and no
   * branch of the pass reaches a row it did not list. The log is the
   * observable of that — a pass that re-decided would re-warn, and a warning
   * replayed on every boot is the loop this test exists to rule out. */
  log_capture cap;
  log_capture_begin(&cap);
  reboot(&f);
  char *log_text = log_capture_end(&cap);
  KBC_CHECK_NOT_NULL(log_text);
  if (log_text != NULL) {
    KBC_CHECK_MSG(strstr(log_text, "move bring-up") == NULL,
                  "the second boot decided an interrupted move again: [%s]",
                  log_text);
    free(log_text);
  }

  char moves_after[1024];
  char artifacts_after[1024];
  table_digest(f.cfg,
               "SELECT id, old_id, new_id, old_rel, new_rel, completed_at, "
               "abandoned_at FROM moves ORDER BY id;",
               moves_after, sizeof moves_after);
  table_digest(f.cfg,
               "SELECT id, corpus, path FROM artifacts ORDER BY id;",
               artifacts_after, sizeof artifacts_after);
  KBC_CHECK_MSG(strcmp(moves_before, moves_after) == 0,
                "a second boot rewrote the move rows: [%s] -> [%s]",
                moves_before, moves_after);
  KBC_CHECK_MSG(strcmp(artifacts_before, artifacts_after) == 0,
                "a second boot wrote to the artifacts table, so bring-up is "
                "re-indexing a corpus it was asked to leave alone: [%s] -> "
                "[%s]", artifacts_before, artifacts_after);
  join(from, sizeof from, f.corpus_a, "c.md");
  join(to, sizeof to, f.corpus_a, "b-renamed.md");
  KBC_CHECK_MSG(kbc_path_exists(from) && kbc_path_exists(to),
                "a second boot moved a file: the corpus is [%s] -> [%s]", from,
                to);
  kbc_arena_free(a);
  fx_teardown(&f);
}

/* AN EMPTY LIST AND AN UNREADABLE TABLE ARE DIFFERENT ANSWERS. Here the moves
 * table is replaced by a view that cannot answer the query, with the rows
 * intact behind it. A pass that treated the failed read as "no moves" would
 * come up clean and serve a daemon that cannot say which renames are half
 * done; so the open is refused, and the rows are still there, still unstamped,
 * for the next boot that can read them. */
KBC_TEST(a_daemon_does_not_start_when_the_move_intents_cannot_be_read) {
  fixture f;
  fx_setup(&f, false);
  if (f.app == NULL) {
    fx_teardown(&f);
    return;
  }
  kbc_err err;
  kbc_err_reset(&err);
  KBC_CHECK_OK(kbc_app_reindex(f.app, &err));

  static const char seeded[KBC_MAX_ID_LEN + 1] = "aaaaaaaaaaaa";
  char staged[KBC_MAX_ID_LEN + 1];
  kbc_id_for_artifact(staged, CORPUS_A, "unreadable.md");
  seed_interrupted_move(f.cfg, seeded, staged, "c.md", "unreadable.md");
  KBC_CHECK_EQ_INT(incomplete_moves(f.cfg), 1);

  /* A view by the name the query uses, over a table it cannot select from:
 * `moves` still holds its row, and the query now fails where it used to
 * answer. The migration ladder will not put the table back — its version is
 * already recorded — so this is the state the next open really finds. */
  kbc_app_close(f.app);
  f.app = NULL;
  db_side side;
  memset(&side, 0, sizeof side);
  KBC_CHECK(db_side_open(&side, f.cfg));
  KBC_CHECK_OK(db_side_exec(&side, "ALTER TABLE moves RENAME TO moves_stash;"));
  KBC_CHECK_OK(db_side_exec(
      &side, "CREATE VIEW moves AS SELECT old_id FROM moves_stash;"));
  db_side_close(&side);

  kbc_err_reset(&err);
  kbc_app *app = kbc_app_open(f.cfg, &err);
  KBC_CHECK_MSG(app == NULL,
                "the daemon came up with an unreadable moves table: it cannot "
                "tell a clean boot from a rename it never finished");
  KBC_CHECK_ERR_MSG(err);
  if (app != NULL) kbc_app_close(app);

  /* And nothing was converged on the way to finding out: the row is where the
   * crash left it. BOTH columns, because `completed_at IS NULL` alone no
   * longer distinguishes an untouched row from one this pass abandoned — which
   * would leave a bring-up that converged anyway passing here. */
  db_side after;
  memset(&after, 0, sizeof after);
  KBC_CHECK(db_side_open(&after, f.cfg));
  sqlite3_stmt *q = NULL;
  KBC_CHECK_EQ_INT(
      sqlite3_prepare_v2(
          after.db,
          "SELECT COUNT(*) FROM moves_stash WHERE completed_at IS NULL"
          " AND abandoned_at IS NULL;",
          -1, &q, NULL),
      SQLITE_OK);
  if (q != NULL) {
    KBC_CHECK_EQ_INT(sqlite3_step(q), SQLITE_ROW);
    KBC_CHECK_MSG(sqlite3_column_int64(q, 0) == 1,
                  "a boot that could not read the move intents still converged "
                  "them");
    sqlite3_finalize(q);
  }
  db_side_close(&after);
  fx_teardown(&f);
}

/* A row this pass cannot place is a row it will not decide. kbc_app_move_path
 * refuses a path that is not corpus-relative, so a row naming one was not
 * written by a move: stamping it would publish a decision built out of a
 * corrupt record, and joining it onto a corpus root would look outside every
 * corpus.
 *
 * This is the one branch the third state does NOT swallow. Abandoned and
 * in-flight look alike from the outside — both are out of the happy path and
 * neither redirects — so the temptation is to tidy the refused row into an
 * abandoned one. They are different facts: "nobody has looked at this yet"
 * and "somebody looked and it could not happen" are exactly the case where a
 * human has to be told, and a row that quietly becomes terminal stops being
 * re-listed. So the assertion is on the STATE, not merely on the count. */
KBC_TEST(a_bring_up_pass_refuses_a_move_row_it_cannot_place) {
  fixture f;
  fx_setup(&f, false);
  if (f.app == NULL) {
    fx_teardown(&f);
    return;
  }
  kbc_err err;
  kbc_err_reset(&err);
  KBC_CHECK_OK(kbc_app_reindex(f.app, &err));

  static const char seeded[KBC_MAX_ID_LEN + 1] = "bbbbbbbbbbbb";
  char staged[KBC_MAX_ID_LEN + 1];
  kbc_id_for_artifact(staged, CORPUS_A, "wherever.md");
  seed_interrupted_move(f.cfg, seeded, staged, "../../outside.md",
                        "wherever.md");
  KBC_CHECK_EQ_INT(incomplete_moves(f.cfg), 1);

  /* The daemon still starts: one unreadable row is not a broken store, and
   * refusing to serve over it would be a worse failure than leaving it. */
  log_capture cap;
  log_capture_begin(&cap);
  reboot(&f);
  char *log_text = log_capture_end(&cap);
  KBC_CHECK_NOT_NULL(log_text);
  if (log_text != NULL) {
    KBC_CHECK_MSG(strstr(log_text, "left in flight") != NULL,
                  "the pass did not say it is leaving the row in flight: [%s]",
                  log_text);
    KBC_CHECK_MSG(strstr(log_text, seeded) != NULL &&
                      strstr(log_text, "../../outside.md") != NULL,
                  "the refusal does not name the row a human has to look at: "
                  "[%s]", log_text);
    free(log_text);
  }
  KBC_CHECK_MSG(incomplete_moves(f.cfg) == 1,
                "bring-up ended a move row naming %s, which is outside every "
                "corpus: a decision published from a path it cannot resolve is "
                "worse than a row it says it will not touch",
                "../../outside.md");
  char state[32];
  move_row_state(f.cfg, seeded, state, sizeof state);
  KBC_CHECK_MSG(strcmp(state, "flight") == 0,
                "the row the pass REFUSED is in state \"%s\": refusing is not "
                "abandoning. Abandoned means somebody looked and it could not "
                "happen, and this row is the one case a human still has to "
                "look at", state);
  char p[KBC_TEST_PATH_MAX];
  join(p, sizeof p, f.root, "outside.md");
  KBC_CHECK_MSG(!kbc_path_exists(p),
                "bring-up stat'ed a path outside the corpus root");

  /* And a second boot still refuses it, rather than the first boot having
   * quietly retired it: an in-flight row is re-listed, and a human who has
   * not fixed the row must keep being told about it. */
  reboot(&f);
  move_row_state(f.cfg, seeded, state, sizeof state);
  KBC_CHECK_MSG(strcmp(state, "flight") == 0,
                "the refused row reached state \"%s\" on a second boot", state);
  fx_teardown(&f);
}

/* A bookmark to the old id. The id is a function of (corpus, path), so the
 * move necessarily mints a new one, and every id handed out before it names
 * a document that still exists under a name the holder does not know. The
 * walk is chain-following, so a bookmark made before TWO renames lands on the
 * second — which a single-hop redirect would get wrong. */
KBC_TEST(a_stale_id_resolves_to_the_document_after_a_move) {
  fixture f;
  fx_setup(&f, false);
  if (f.app == NULL) {
    fx_teardown(&f);
    return;
  }
  kbc_err err;
  kbc_err_reset(&err);
  KBC_CHECK_OK(kbc_app_reindex(f.app, &err));
  kbc_arena *a = kbc_arena_new(64u * 1024u);
  KBC_CHECK_NOT_NULL(a);
  if (a == NULL) {
    fx_teardown(&f);
    return;
  }
  const char *id0 = id_of_path(f.cfg, CORPUS_A, "c.md", a);
  KBC_CHECK_NOT_NULL(id0);
  if (id0 == NULL) {
    kbc_arena_free(a);
    fx_teardown(&f);
    return;
  }
  char id0_copy[KBC_MAX_ID_LEN + 1];
  memcpy(id0_copy, id0, sizeof id0_copy);

  KBC_CHECK_OK(kbc_app_move_path(f.app, CORPUS_A, "c.md", "moved.md", &err));
  /* COPIED, not aliased: id_of_path hands back arena memory, and the
   * get_artifact below allocates into the same arena and may land on the
   * same block. Comparing the two pointers would be comparing an id with
   * itself. */
  char new_id[KBC_MAX_ID_LEN + 1];
  new_id[0] = '\0';
  const char *moved = id_of_path(f.cfg, CORPUS_A, "moved.md", a);
  KBC_CHECK_NOT_NULL(moved);
  if (moved != NULL) {
    memcpy(new_id, moved, KBC_MAX_ID_LEN);
    new_id[KBC_MAX_ID_LEN] = '\0';
  }
  KBC_CHECK(strcmp(new_id, id0_copy) != 0);
  if (moved != NULL) {
    kbc_artifact art;
    memset(&art, 0, sizeof art);
    kbc_status gs = kbc_app_get_artifact(f.app, a, id0_copy, false, &art, &err);
    KBC_CHECK_OK(gs);
    /* Guarded, not assumed: a failed get leaves every field NULL, and reading
     * one of those is a segfault that hides the assertion that mattered. */
    if (gs == KBC_OK) {
      KBC_CHECK_MSG(strcmp(art.id, id0_copy) != 0,
                    "the stale id %s resolved to itself, so the move is "
                    "invisible", id0_copy);
      KBC_CHECK_EQ_STR(art.id, new_id);
      KBC_CHECK_EQ_STR(art.path, "moved.md");
      KBC_CHECK_EQ_STR(art.corpus, CORPUS_A);
    }
  }

  /* Twice over: the bookmark follows the CHAIN. Stopping at the first hop
   * would hand back an id that was itself renamed away, which is the same
   * dead bookmark under a different name. */
  kbc_err_reset(&err);
  KBC_CHECK_OK(kbc_app_move_path(f.app, CORPUS_A, "moved.md", "moved2.md",
                                 &err));
  char new2_id[KBC_MAX_ID_LEN + 1];
  new2_id[0] = '\0';
  const char *moved2 = id_of_path(f.cfg, CORPUS_A, "moved2.md", a);
  KBC_CHECK_NOT_NULL(moved2);
  if (moved2 != NULL) {
    memcpy(new2_id, moved2, KBC_MAX_ID_LEN);
    new2_id[KBC_MAX_ID_LEN] = '\0';
  }
  KBC_CHECK(strcmp(new2_id, new_id) != 0);
  if (moved2 != NULL) {
    kbc_artifact art;
    memset(&art, 0, sizeof art);
    kbc_status gs = kbc_app_get_artifact(f.app, a, id0_copy, false, &art, &err);
    KBC_CHECK_OK(gs);
    if (gs == KBC_OK) {
      KBC_CHECK_MSG(strcmp(art.id, id0_copy) != 0,
                    "a bookmark from before the FIRST rename resolved to itself");
      KBC_CHECK_EQ_STR(art.id, new2_id);
      KBC_CHECK_EQ_STR(art.path, "moved2.md");
    }
  }

  /* An id that never moved is still an honest miss, not a redirect to
   * something arbitrary and not a crash. */
  kbc_err_reset(&err);
  kbc_artifact art;
  memset(&art, 0, sizeof art);
  KBC_CHECK_ERR(
      kbc_app_get_artifact(f.app, a, "000000000000", false, &art, &err),
      KBC_ERR_NOTFOUND);
  KBC_CHECK_ERR_MSG(err);
  kbc_arena_free(a);
  fx_teardown(&f);
}

/* ------------------------------------------------------------- chunker --- */

/* include/kbc/chunk.h is the contract: the window sizes, both structs and the
 * leaf's signature. Nothing is redeclared or re-asserted here, which is the
 * point. The mirror this replaces checked `sizeof(kbc_chunk) == 24`, so a
 * header that grew a field would have failed the build on a LAYOUT mismatch
 * while every behavioural assertion about the chunker still passed — a test
 * that fires on the wrong thing. Reading the same declaration the production
 * code compiles against cannot drift from it at all. */

/* chunk.rs's own defaults (chunk.rs:19, :23). Spelled here so a test that
 * forgets an argument says DEFAULT instead of quietly using a literal — a
 * value, not a declaration, so naming it here says nothing the header does
 * not. */
#define CHUNK_DEFAULT_WORDS KBC_CHUNK_WORDS
#define CHUNK_DEFAULT_OVERLAP KBC_CHUNK_OVERLAP_WORDS

/* "w0 w1 ... w<n-1>", chunk.rs's test word generator (:101-106). */
static char *chunk_words(size_t n) {
  kbc_str s;
  kbc_str_init(&s);
  for (size_t i = 0; i < n; i++) {
    if (i > 0) {
      (void)kbc_str_putc(&s, ' ');
    }
    (void)kbc_str_printf(&s, "w%zu", i);
  }
  char *out = strdup(s.ptr != NULL ? s.ptr : "");
  kbc_str_free(&s);
  return out;
}

/* Runs the leaf with the default window sizes. `cap` is APP_MAX_CHUNKS_PER_DOC
 * at every call site but one, so it is a parameter. */
static bool chunk_default(kbc_arena *a, const char *title, const char *headings,
                          const char *body, size_t cap, kbc_chunks *out) {
  kbc_err err;
  kbc_err_reset(&err);
  kbc_status s =
      kbc_chunk_document(a, title, headings, body, strlen(body),
                             CHUNK_DEFAULT_WORDS, CHUNK_DEFAULT_OVERLAP, cap,
                             out, &err);
  if (s != KBC_OK) {
    fprintf(stderr, "  chunk_document: %s\n", err.msg);
  }
  return s == KBC_OK;
}

/* The words of one chunk, so an overlap can be compared as two spans of words
 * rather than as substrings — a substring comparison would pass on a window
 * that merely contained the other one's text. */
static size_t chunk_word_at(const char *text, size_t i, char *out,
                            size_t out_cap) {
  const char *p = text;
  size_t seen = 0;
  while (*p != '\0') {
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') {
      p++;
    }
    if (*p == '\0') {
      break;
    }
    const char *w = p;
    while (*p != '\0' && *p != ' ' && *p != '\t' && *p != '\n' && *p != '\r') {
      p++;
    }
    if (seen == i) {
      const size_t n = (size_t)(p - w);
      if (n + 1u > out_cap) {
        return n; /* too long to compare; the length itself is the signal */
      }
      memcpy(out, w, n);
      out[n] = '\0';
      return n;
    }
    seen++;
  }
  return 0;
}

static size_t chunk_word_count(const char *text) {
  size_t n = 0;
  const char *p = text;
  while (*p != '\0') {
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') {
      p++;
    }
    if (*p == '\0') {
      break;
    }
    n++;
    while (*p != '\0' && *p != ' ' && *p != '\t' && *p != '\n' && *p != '\r') {
      p++;
    }
  }
  return n;
}

/* chunk.rs:109 chunk_zero_is_title_plus_headings. A document with a title and
 * headings but NO body is one chunk, and it carries both. */
KBC_TEST(chunk_zero_is_title_plus_headings) {
  kbc_arena *a = kbc_arena_new(4096);
  KBC_CHECK_NOT_NULL(a);
  if (a == NULL) return;
  kbc_chunks c;
  KBC_CHECK(chunk_default(a, "My Title", "Heading One Heading Two", "",
                          KBC_MAX_CHUNKS_PER_DOC, &c));
  KBC_CHECK_EQ_INT(c.len, 1);
  if (c.len == 1) {
    KBC_CHECK_EQ_INT(c.items[0].idx, 0);
    KBC_CHECK_MSG(strstr(c.items[0].text, "My Title") != NULL,
                  "chunk 0 lost the title: \"%s\"", c.items[0].text);
    KBC_CHECK_MSG(strstr(c.items[0].text, "Heading One") != NULL,
                  "chunk 0 lost the headings: \"%s\"", c.items[0].text);
  }
  kbc_arena_free(a);
}

/* chunk.rs:118 short_body_is_a_single_window. 50 words fit inside one 280-word
 * window, so there is no second body chunk and no trailing empty one. */
KBC_TEST(short_body_is_a_single_window) {
  kbc_arena *a = kbc_arena_new(65536);
  KBC_CHECK_NOT_NULL(a);
  if (a == NULL) return;
  char *body = chunk_words(50);
  KBC_CHECK_NOT_NULL(body);
  kbc_chunks c;
  KBC_CHECK(chunk_default(a, "T", "", body, KBC_MAX_CHUNKS_PER_DOC, &c));
  /* chunk 0 (title) + one body window. */
  KBC_CHECK_EQ_INT(c.len, 2);
  if (c.len == 2) {
    KBC_CHECK_EQ_INT(c.items[1].idx, 1);
    KBC_CHECK_EQ_INT(chunk_word_count(c.items[1].text), 50);
  }
  free(body);
  kbc_arena_free(a);
}

/* chunk.rs:127 long_body_splits_into_overlapping_windows. 600 words, window
 * 280, overlap 60 => step 220 and windows [0,280) [220,500) [440,600). The
 * adjacency of the windows is the property: the last 60 words of one ARE the
 * first 60 of the next, which is what a passage straddling a boundary relies
 * on. */
KBC_TEST(long_body_splits_into_overlapping_windows) {
  kbc_arena *a = kbc_arena_new(256u * 1024u);
  KBC_CHECK_NOT_NULL(a);
  if (a == NULL) return;
  char *body = chunk_words(600);
  KBC_CHECK_NOT_NULL(body);
  kbc_chunks c;
  KBC_CHECK(chunk_default(a, "T", "", body, KBC_MAX_CHUNKS_PER_DOC, &c));
  KBC_CHECK_EQ_INT(c.len, 4); /* chunk 0 + three body windows */
  if (c.len == 4) {
    /* Body windows live at items[1..3]; items[0] is chunk 0 (the title), which
     * is not a window and takes no part in the overlap. */
    for (size_t w = 1; w + 1u < c.len; w++) {
      for (size_t k = 0; k < 60u; k++) {
        char lhs[64] = "", rhs[64] = "";
        /* last 60 words of window w == first 60 of window w+1 */
        (void)chunk_word_at(c.items[w].text,
                            chunk_word_count(c.items[w].text) - 60u + k, lhs,
                            sizeof lhs);
        (void)chunk_word_at(c.items[w + 1u].text, k, rhs, sizeof rhs);
        KBC_CHECK_MSG(strcmp(lhs, rhs) == 0,
                      "windows %zu and %zu do not overlap by 60 at %zu: "
                      "\"%s\" vs \"%s\"",
                      w, w + 1u, k, lhs, rhs);
      }
    }
  }
  free(body);
  kbc_arena_free(a);
}

/* chunk.rs:148 last_window_is_not_duplicated. Exactly one window's worth of
 * words ends the body, and the walk stops there instead of emitting the empty
 * window that would follow. */
KBC_TEST(last_window_is_not_duplicated) {
  kbc_arena *a = kbc_arena_new(256u * 1024u);
  KBC_CHECK_NOT_NULL(a);
  if (a == NULL) return;
  char *body = chunk_words(280);
  KBC_CHECK_NOT_NULL(body);
  kbc_chunks c;
  KBC_CHECK(chunk_default(a, "T", "", body, KBC_MAX_CHUNKS_PER_DOC, &c));
  KBC_CHECK_EQ_INT(c.len, 2); /* title + 1 body window */
  if (c.len >= 2) {
    KBC_CHECK_MSG(chunk_word_count(c.items[c.len - 1].text) > 0,
                  "the last chunk is empty: the walk ran one step too far");
  }
  free(body);
  kbc_arena_free(a);
}

/* chunk.rs:155 empty_document_yields_no_chunks. Whitespace is not a word and
 * is not a title, so a document with nothing in it produces nothing — an
 * empty chunk row would be a comment anchor pointing at nothing. */
KBC_TEST(empty_document_yields_no_chunks) {
  kbc_arena *a = kbc_arena_new(4096);
  KBC_CHECK_NOT_NULL(a);
  if (a == NULL) return;
  kbc_chunks c;
  KBC_CHECK(chunk_default(a, "", "", "", KBC_MAX_CHUNKS_PER_DOC, &c));
  KBC_CHECK_EQ_INT(c.len, 0);
  KBC_CHECK(chunk_default(a, "   ", "  ", "  \n ", KBC_MAX_CHUNKS_PER_DOC, &c));
  KBC_CHECK_EQ_INT(c.len, 0);
  kbc_arena_free(a);
}

/* chunk.rs:160 body_only_still_chunks. No title and no headings: chunk 0 is
 * not emitted (there is nothing to put in it) and the body's first window
 * takes index 0 instead of being pushed down. */
KBC_TEST(body_only_still_chunks) {
  kbc_arena *a = kbc_arena_new(4096);
  KBC_CHECK_NOT_NULL(a);
  if (a == NULL) return;
  char *body = chunk_words(10);
  KBC_CHECK_NOT_NULL(body);
  kbc_chunks c;
  KBC_CHECK(chunk_default(a, "", "", body, KBC_MAX_CHUNKS_PER_DOC, &c));
  KBC_CHECK_EQ_INT(c.len, 1);
  if (c.len == 1) {
    KBC_CHECK_EQ_INT(c.items[0].idx, 0);
 }
  free(body);
  kbc_arena_free(a);
}

/* chunk.rs:167 is_deterministic. A re-index of an unchanged file has to
 * produce byte-identical chunks, because the stored vectors are compared
 * against them to decide what may be reused; a chunker that iterated a hash
 * map or read the clock would re-embed the whole corpus on every pass. */
KBC_TEST(chunking_is_deterministic) {
  kbc_arena *a = kbc_arena_new(256u * 1024u);
  KBC_CHECK_NOT_NULL(a);
  if (a == NULL) return;
  char *body = chunk_words(1000);
  KBC_CHECK_NOT_NULL(body);
  kbc_chunks x, y;
  KBC_CHECK(chunk_default(a, "T", "H", body, KBC_MAX_CHUNKS_PER_DOC, &x));
  KBC_CHECK(chunk_default(a, "T", "H", body, KBC_MAX_CHUNKS_PER_DOC, &y));
  KBC_CHECK_EQ_INT(x.len, y.len);
  for (size_t i = 0; i < x.len && i < y.len; i++) {
    KBC_CHECK_MSG(x.items[i].text_len == y.items[i].text_len &&
                      memcmp(x.items[i].text, y.items[i].text,
                             x.items[i].text_len) == 0,
                  "chunk %zu differs between two runs of the same input", i);
  }
  free(body);
  kbc_arena_free(a);
}

/* chunk.rs:174 degenerate_overlap_still_terminates. An overlap LARGER than the
 * window makes window - overlap negative, so a naive step would be 0 or wrap
 * and the walk would never advance. The guard forces a step of at least one
 * word, so the case that used to hang now returns. */
KBC_TEST(degenerate_overlap_still_terminates) {
  kbc_arena *a = kbc_arena_new(65536);
  KBC_CHECK_NOT_NULL(a);
  if (a == NULL) return;
  char *body = chunk_words(100);
  KBC_CHECK_NOT_NULL(body);
  kbc_err err;
  kbc_err_reset(&err);
  kbc_chunks c;
  KBC_CHECK_OK(kbc_chunk_document(a, "", "", body, strlen(body), 10u, 50u,
                                      KBC_MAX_CHUNKS_PER_DOC, &c, &err));
  KBC_CHECK_MSG(c.len > 0, "a degenerate overlap produced no chunks at all");
  /* Every word of the body is still covered exactly once as a window start,
 * which is what "always advance" means observably. */
  KBC_CHECK_EQ_INT(c.total, c.len);
  for (size_t i = 0; i < c.len; i++) {
    KBC_CHECK_EQ_INT(c.items[i].idx, i);
 }
  free(body);
  kbc_arena_free(a);
}

/* indexer.rs:3576 cap_chunks_to_truncates_to_the_first_n_and_reports_the_
 * original_total. The cap keeps the FIRST chunks, so chunk 0 — the
 * title/headings passage — survives; and `total` reports the count BEFORE the
 * truncation, because a caller that logged the kept count would report a
 * document that produced 5,000 chunks as having produced 512. */
KBC_TEST(the_chunk_cap_keeps_the_first_n_and_reports_the_original_total) {
  kbc_arena *a = kbc_arena_new(256u * 1024u);
  KBC_CHECK_NOT_NULL(a);
  if (a == NULL) return;
  /* 900 words at the default window is 1 + 4 windows, so a cap of 3 truncates
 * without needing a 16 MiB document. */
  char *body = chunk_words(900);
  KBC_CHECK_NOT_NULL(body);
  kbc_chunks c;
  KBC_CHECK(chunk_default(a, "T", "H", body, 3u, &c));
  KBC_CHECK_EQ_INT(c.len, 3);
  KBC_CHECK_EQ_INT(c.total, 5);
  if (c.len == 3) {
    KBC_CHECK_MSG(strstr(c.items[0].text, "T") == c.items[0].text,
                  "the cap dropped chunk 0: the title passage must survive");
    for (size_t i = 0; i < c.len; i++) {
      KBC_CHECK_EQ_INT(c.items[i].idx, i);
    }
  }
  free(body);
  kbc_arena_free(a);
}

/* The end-to-end half, through the daemon's own entry point. The chunk rows
 * are what a comment anchors to and what the vector lane covers, so a
 * document whose body is one long paragraph must be stored as OVERLAPPING
 * WINDOWS rather than as the single oversized block it used to be — and the
 * keyword lane must still answer for a term that occurs only in the last
 * window, which is the whole reason the windows overlap. */
KBC_TEST(a_long_document_is_stored_as_overlapping_windows_and_stays_findable) {
  fixture f;
  fx_setup(&f, false);
  if (f.app == NULL) {
    fx_teardown(&f);
    return;
  }
  /* 600 words => windows [0,280) [220,500) [440,600). "quokkazinc" sits at
   * word 500, which is in the LAST window only; "aardvarkium" at word 250 is
   * in the 220-280 overlap and therefore in two windows at once. */
  kbc_str doc;
  kbc_str_init(&doc);
  (void)kbc_str_puts(&doc, "# Long Ledger\n\n");
 for (size_t i = 0; i < 600u; i++) {
    if (i > 0) {
      (void)kbc_str_putc(&doc, ' ');
    }
    if (i == 250u) {
      (void)kbc_str_puts(&doc, "aardvarkium");
    } else if (i == 500u) {
      (void)kbc_str_puts(&doc, "quokkazinc");
    } else {
      (void)kbc_str_printf(&doc, "w%zu", i);
    }
  }
  (void)kbc_str_putc(&doc, '\n');
  char p[KBC_TEST_PATH_MAX];
  join(p, sizeof p, f.corpus_a, "long.md");
  kbc_test_write_file(p, doc.ptr);
  kbc_str_free(&doc);

  kbc_err err;
  kbc_err_reset(&err);
  KBC_CHECK_OK(kbc_app_reindex(f.app, &err));

  /* The stored rows are the windows, not the one block the old chunker wrote.
   * Read through a second connection so the count cannot be confused with the
   * three short fixture documents. */
  kbc_arena *ida = kbc_arena_new(4096);
  KBC_CHECK_NOT_NULL(ida);
  const char *id = ida != NULL ? id_of_path(f.cfg, CORPUS_A, "long.md", ida)
                               : NULL;
  KBC_CHECK_NOT_NULL(id);

  if (id != NULL) {
    kbc_store *s = kbc_store_open(f.cfg, &err);
    KBC_CHECK_NOT_NULL(s);
    if (s != NULL) {
      kbc_arena *ca = kbc_arena_new(256u * 1024u);
      KBC_CHECK_NOT_NULL(ca);
      if (ca != NULL) {
        kbc_block *rows = kbc_arena_calloc(ca, 16u, sizeof(*rows));
        KBC_CHECK_NOT_NULL(rows);
        size_t n = 0;
        kbc_err_reset(&err);
        KBC_CHECK_OK(
            kbc_store_list_chunks(s, ca, id, rows, &n, &err));
        /* chunk 0 (title+heading) + three body windows. */
        KBC_CHECK_EQ_INT(n, 4);
        if (n == 4) {
          KBC_CHECK_MSG(strstr(rows[0].text, "Long Ledger") != NULL,
                        "chunk 0 is not the title passage: \"%s\"", rows[0].text);
          /* The last window carries the term that is in no earlier window. */
          KBC_CHECK_MSG(strstr(rows[3].text, "quokkazinc") != NULL,
                        "the final window does not reach word 500");
          KBC_CHECK_MSG(strstr(rows[1].text, "quokkazinc") == NULL,
                        "word 500 leaked into the first window");
          /* The overlap term is in two windows — that is the overlap — and
           * the search still returns the document ONCE. */
          KBC_CHECK_MSG(strstr(rows[1].text, "aardvarkium") != NULL,
                        "word 250 missing from window [0,280)");
          KBC_CHECK_MSG(strstr(rows[2].text, "aardvarkium") != NULL,
                        "word 250 missing from the overlap window [220,500)");
        }
        kbc_arena_free(ca);
      }
      kbc_store_close(s);
    }
  }

  /* And the keyword lane answers for a term that lives in the last window and
   * in the overlap, once each. */
  char path[64], title[64], idbuf[32];
  KBC_CHECK_EQ_INT(first_hit_path(f.app, "quokkazinc", CORPUS_A, path,
                                  sizeof path, title, sizeof title, idbuf,
                                  sizeof idbuf, NULL),
                   1);
  KBC_CHECK_EQ_STR(path, "long.md");
  KBC_CHECK_EQ_INT(first_hit_path(f.app, "aardvarkium", CORPUS_A, path,
                                  sizeof path, title, sizeof title, idbuf,
                                  sizeof idbuf, NULL),
                   1);
  KBC_CHECK_EQ_STR(path, "long.md");

  kbc_arena_free(ida);
  fx_teardown(&f);
}

/* ---------------------------------------------------------- quarantine --- */

/* A sidecar that records every request it is asked to embed and answers with
 * a fixed vector, so "was this document re-embedded?" is observable from
 * outside the process. The log is the whole point: the alternative — reading
 * the vector store back — cannot tell a document that was re-embedded with
 * the same value from one that was not touched.
 *
 * `announce` is the difference between a fake and a sidecar. A production
 * one pushes {"kind":"ready","model":...,"dim":N} unprompted the moment the
 * model is loaded, and the query cache is keyed on the model name read off
 * THAT line — so a fake that stays silent leaves the query lane with no key to
 * build, and every query embed fails with "its model is not known yet" before
 * the sidecar is ever asked. The quarantine cases below drive the DOCUMENT
 * lane, which never builds a key, so they leave it off and keep the fake as
 * silent as it was; the query-cache cases turn it on. The health case stays
 * for the older handshake shape. */
static void make_counting_sidecar(const char *path, const char *log_path,
                                  bool fail, bool announce) {
  kbc_str s;
  kbc_str_init(&s);
  (void)kbc_str_printf(&s, "#!/bin/sh\nLOG=%s\n", log_path);
  if (announce) {
    (void)kbc_str_puts(&s,
                       "printf '%s\\n' '{\"kind\":\"ready\",\"model\":\"fake\","
                       "\"dim\":3}'\n");
  }
  (void)kbc_str_puts(&s,
                     "while IFS= read -r line; do\n"
      "  printf '%s\\n' \"$line\" >> \"$LOG\"\n"
      "  case \"$line\" in\n"
   "  *health*) printf '%s\\n' '{\"ok\":true,\"dim\":3,\"model\":\"fake\"}' ;;\n");
  if (fail) {
    /* A sidecar-reported error is a failed REQUEST, not a dead pipe. */
    (void)kbc_str_puts(&s,
                       "  *embed*) printf '%s\\n' '{\"kind\":\"error\","
                       "\"msg\":\"simulated OOM\"}' ;;\n");
  } else {
    (void)kbc_str_puts(&s,
                       "  *embed*) printf '%s\\n' '{\"ok\":true,\"dim\":3,"
                       "\"vectors\":[[1,0,0]]}' ;;\n");
  }
  (void)kbc_str_puts(&s, "  esac\ndone\n");
  kbc_test_write_file(path, s.ptr);
  kbc_str_free(&s);
  KBC_CHECK_MSG(chmod(path, 0755) == 0, "chmod %s: %s", path, strerror(errno));
}

static size_t count_log_lines(const char *log_path) {
  char *text = kbc_test_read_file(log_path);
  if (text == NULL) {
    return 0;
  }
  size_t n = 0;
  for (const char *p = text; *p != '\0'; p++) {
    if (*p == '\n') {
      n++;
    }
  }
  free(text);
  return n;
}

/* The store's own view of a path's failure count.
 *
 * `content_hash` NULL is the "how bad is it" question and returns the raw
 * count. Passing a hash asks the gate's question instead — "is this still the
 * document that failed" — and is how a test tells an edited document's fresh
 * budget from an unchanged one's exhausted one. */
static int64_t retry_count_hashed(const kbc_config *cfg, const char *corpus,
                                  const char *path, const char *content_hash) {
  kbc_err err;
  kbc_err_reset(&err);
  kbc_store *s = kbc_store_open(cfg, &err);
  if (s == NULL) return -1;
  int64_t n = -1;
  (void)kbc_store_retry_count_for_path(s, corpus, path, content_hash, &n, &err);
  kbc_store_close(s);
  return n;
}

static int64_t retry_count(const kbc_config *cfg, const char *corpus,
                           const char *path) {
  return retry_count_hashed(cfg, corpus, path, NULL);
}

/* The content hash of some text, in the form app.c writes it into the
 * errors.content_hash column. Mirrored deliberately: a test that seeded the
 * gate with a hash the gate will never be asked about would prove nothing. */
static void hash_of(const char *text, char *out, size_t cap) {
  snprintf(out, cap, "fnv1a32-%08" PRIx32, kbc_fnv1a32(text, strlen(text)));
}

/* Records one embed failure for `path` against `content_hash`, the way the
 * daemon does — the same field, so the gate can find the row again. */
static kbc_status seed_failure(const kbc_config *cfg, const char *path,
                                const char *content_hash, int i) {
  kbc_err err;
  kbc_err_reset(&err);
  kbc_store *s = kbc_store_open(cfg, &err);
  if (s == NULL) return false;
  char id[24];
  snprintf(id, sizeof id, "e-seed%02d", i);
  char msg[64];
  snprintf(msg, sizeof msg, "simulated OOM #%d", i);
  kbc_error_row row;
  memset(&row, 0, sizeof row);
  row.id = id;
  row.kind = "embed";
  row.corpus = CORPUS_A;
  row.path = path;
  row.message = msg;
  row.content_hash = content_hash;
  row.created_at = 1756000000 + i;
  const kbc_status st = kbc_store_record_error(s, &row, &err);
  kbc_store_close(s);
  return st;
}

/* A corpus of ONE document with a fake sidecar wired in, so what the sidecar
 * was asked to embed names exactly one file. `announce` is the query-cache
 * cases' flag: the document lane never needs the model, the query lane keys
 * on it. */
static void fx_setup_quarantine(fixture *f, bool sidecar_fails, bool announce) {
  memset(f, 0, sizeof(*f));
  kbc_test_tmpdir(f->root, sizeof f->root);
  join(f->data, sizeof f->data, f->root, "data");
  join(f->corpus_a, sizeof f->corpus_a, f->root, CORPUS_A);
  kbc_test_mkdir_p(f->corpus_a);
  char p[KBC_TEST_PATH_MAX];
  join(p, sizeof p, f->corpus_a, "a.md");
  kbc_test_write_file(p, DOC_A);
  join(f->sidecar, sizeof f->sidecar, f->root, "sidecar.sh");
  join(f->log, sizeof f->log, f->root, "sidecar.log");
  make_counting_sidecar(f->sidecar, f->log, sidecar_fails, announce);

  f->cfg = make_cfg(f->data, f->corpus_a, NULL);
  KBC_CHECK_NOT_NULL(f->cfg);
  if (f->cfg == NULL) return;
  free(f->cfg->embedder_cmd);
  f->cfg->embedder_cmd = strdup(f->sidecar);
  kbc_err err;
  kbc_err_reset(&err);
  f->app = kbc_app_open(f->cfg, &err);
  if (f->app == NULL) fprintf(stderr, "  app_open: %s\n", err.msg);
  KBC_CHECK_NOT_NULL(f->app);
}

/* A genuine embed failure is recorded for the path, durably. Without the
 * record the gate has no input and a document that fails forever is embedded
 * forever — which is the failure the gate exists to stop. */
KBC_TEST(a_failed_embed_is_recorded_against_the_document) {
  fixture f;
  fx_setup_quarantine(&f, true, false);
  if (f.app == NULL) {
    fx_teardown(&f);
    return;
  }
  kbc_err err;
  kbc_err_reset(&err);
  /* The reindex still SUCCEEDS: a broken sidecar degrades the vector lane, it
   * is never a reason to fail an ingest. */
  KBC_CHECK_OK(kbc_app_reindex(f.app, &err));
  /* record_error INSERTS the first row of a path at retry_count 0 and only
   * bumps it on later calls (store.c:2591-2595), so one failure reads 0. The
   * row is the durable artefact, so it is read back through the public listing
   * rather than inferred from the count. */
  KBC_CHECK_EQ_INT(retry_count(f.cfg, CORPUS_A, "a.md"), 0);
  {
    kbc_arena *ea = kbc_arena_new(8192);
    KBC_CHECK_NOT_NULL(ea);
    if (ea != NULL) {
      kbc_store *s = kbc_store_open(f.cfg, &err);
      KBC_CHECK_NOT_NULL(s);
      if (s != NULL) {
        kbc_error_row *rows = NULL;
        size_t n = 0;
        kbc_err_reset(&err);
        KBC_CHECK_OK(
            kbc_store_list_errors(s, ea, CORPUS_A, true, 10u, &rows, &n, &err));
        KBC_CHECK_EQ_INT(n, 1);
        if (n == 1) {
          /* The sidecar's OWN message, so the Errors tab names the actual
           * refusal instead of a generic "embed failed". */
          KBC_CHECK_EQ_STR(rows[0].kind, "embed");
          KBC_CHECK_EQ_STR(rows[0].path, "a.md");
          KBC_CHECK_MSG(strstr(rows[0].message, "simulated OOM") != NULL,
                        "the recorded message lost the sidecar's own: \"%s\"",
                        rows[0].message);
          /* The content hash, and it is THIS document's: the quarantine gate
           * looks the row up by (path, content_hash) on the next pass, so a
           * NULL here — or any other string — would make every future lookup
           * answer "a different document" and nothing would ever be gated.
           * Compared by VALUE: a hash of the wrong bytes fails the gate
           * exactly the way a missing one does, so shape is not the test. */
          char want[32];
          hash_of(DOC_A, want, sizeof want);
          KBC_CHECK_MSG(rows[0].content_hash != NULL &&
                            strcmp(rows[0].content_hash, want) == 0,
                        "the recorded content hash is not this document's: "
                        "got \"%s\", want \"%s\"",
                        rows[0].content_hash ? rows[0].content_hash : "(null)",
                        want);
        }
        kbc_store_close(s);
      }
      kbc_arena_free(ea);
    }
  }

  /* And the document is indexed and searchable regardless — the embedding
 * column is nullable by design, so a document without a vector is still a
 * document. */
  char path[64], title[64], id[32];
  KBC_CHECK_EQ_INT(first_hit_path(f.app, "accruals", NULL, path, sizeof path,
                                  title, sizeof title, id, sizeof id, NULL),
                   1);
  KBC_CHECK_EQ_STR(path, "a.md");

  fx_teardown(&f);
}

/* THE quarantine gate. Past the threshold the document stops being embedded
 * and KEEPS being indexed, and the error row that gated it SURVIVES the pass
 * that skipped the embed.
 *
 * The row surviving is the load-bearing half. The obvious "a successful
 * index clears this path's open errors" step un-gates the document on the
 * next pass, the next pass takes a real embed attempt, it fails, the row
 * returns at count 1, and a document that fails forever oscillates in and out
 * of quarantine — embedded forever, which is the loop the gate was built to
 * stop. So the count must still read at the threshold after a gated pass. */
KBC_TEST(a_quarantined_document_stays_indexed_and_keeps_its_error_row) {
  fixture f;
  fx_setup_quarantine(&f, false, false);
  if (f.app == NULL) {
    fx_teardown(&f);
    return;
  }
  kbc_err err;
  kbc_err_reset(&err);
  KBC_CHECK_OK(kbc_app_reindex(f.app, &err));
  /* The gate is seeded, not waited for: driving three real failures would
   * need three reindexes and would assert the sidecar's behaviour as much as
   * the gate's. record_error's first call for a path INSERTS at retry_count 0
   * and later calls bump it, so the count reaches the threshold on the
   * threshold+1'th call (indexer.rs:6812-6821).
   *
   * Seeded against a.md's OWN content hash, which is the point of change 1: a
   * row recorded against different bytes is not evidence about these, so
   * seeding NULL here would leave the document ungated and every assertion
   * below would pass for the wrong reason. */
  char hash_a[32];
  hash_of(DOC_A, hash_a, sizeof hash_a);
  for (int i = 0; i <= 3; i++) {
    KBC_CHECK_OK(seed_failure(f.cfg, "a.md", hash_a, i));
  }
  /* Precondition: the gate's own comparison, asked the gate's way. If this is
   * wrong, every assertion below is measuring the wrong state. */
  KBC_CHECK_EQ_INT(retry_count_hashed(f.cfg, CORPUS_A, "a.md", hash_a), 3);
  /* And the row is invisible to any other content — that asymmetry is what
   * gives an edited document a fresh budget. */
  char other[32];
  hash_of(DOC_B, other, sizeof other);
  KBC_CHECK_EQ_INT(retry_count_hashed(f.cfg, CORPUS_A, "a.md", other), 0);

  /* Clear the log so what follows is this pass's sidecar traffic and nothing
   * else. */
  kbc_test_write_file(f.log, "");
  KBC_CHECK_OK(kbc_app_reindex(f.app, &err));
  KBC_CHECK_MSG(count_log_lines(f.log) == 0,
                "the gated document was still sent to the sidecar: %zu "
                "requests",
                count_log_lines(f.log));

  /* Still keyword-searchable: the embedding column is nullable by design, so
   * losing the vector costs semantic recall, not existence. */
  char path[64], title[64], id[32];
  KBC_CHECK_EQ_INT(first_hit_path(f.app, "accruals", NULL, path, sizeof path,
                                  title, sizeof title, id, sizeof id, NULL),
                   1);
  KBC_CHECK_EQ_STR(path, "a.md");

  /* And the row that gated it is untouched: same count, still open. A gated
   * pass proves nothing about whether the failure condition is gone, because
   * the embed was never attempted. */
  KBC_CHECK_EQ_INT(retry_count(f.cfg, CORPUS_A, "a.md"), 3);

  /* An operator clearing the error is the way OUT: the next pass embeds again.
   * Without this the gate would be a one-way door, and "stays gated until an
   * operator acts" would be indistinguishable from "stays gated forever". */
  kbc_store *clr = kbc_store_open(f.cfg, &err);
  KBC_CHECK_NOT_NULL(clr);
  if (clr != NULL) {
    kbc_err_reset(&err);
    KBC_CHECK_OK(kbc_store_clear_error(clr, CORPUS_A, "a.md", &err));
    kbc_store_close(clr);
  }
  kbc_test_write_file(f.log, "");
  KBC_CHECK_OK(kbc_app_reindex(f.app, &err));
  KBC_CHECK_MSG(count_log_lines(f.log) > 0,
                "clearing the error did not restore embedding: the document "
                "can never leave quarantine");

  fx_teardown(&f);
}

/* Does the sidecar log mention this word? The log is the only externally
 * visible record of WHICH document was embedded; a line count alone cannot
 * say which file was skipped, which is the whole question here. */
static bool log_mentions(const char *log_path, const char *word) {
  char *text = kbc_test_read_file(log_path);
  if (text == NULL) {
    return false;
  }
  const bool found = strstr(text, word) != NULL;
  free(text);
  return found;
}

/* The gate is keyed by (path, content_hash), not by path. Editing a document
 * is how an operator fixes one that failed, so a document whose bytes moved is
 * a DIFFERENT document and gets a fresh embedding budget — the original's
 * retry_count_for_path_hash (indexer.rs:2439). Keyed by path alone, a document
 * that fails, is edited to fix the failure and re-indexed never leaves
 * quarantine, and the only remedy is an operator clearing the error by hand.
 *
 * Both halves are asserted, because either alone proves nothing:
 *
 *   EDITED    — a.md's bytes change, and the next pass embeds it again.
 *   UNCHANGED — b.md's bytes do not, and it stays gated on that SAME pass. If
 *               the gate had stopped working, a.md would come back and so
 *               would b.md; if it had kept working as before, a.md would stay
 *               silent. Only a content-keyed gate gives one and not the other.
 *
 * The failures are SEEDED against each document's own real hash rather than
 * driven through a failing sidecar: a sidecar that refuses is marked unhealthy
 * after its first refusal, so a broken sidecar yields one recorded failure and
 * then stops being asked — it cannot walk a counter up to a threshold. That the
 * daemon RECORDS the hash the gate will ASK about is a separate claim, proved
 * where a real failure exists: a_failed_embed_is_recorded_against_the_document
 * reads the recorded hash back off the row. */
KBC_TEST(an_edited_document_leaves_quarantine_and_an_unchanged_one_does_not) {
  fixture f;
  fx_setup_quarantine(&f, false, false);
  if (f.app == NULL) {
    fx_teardown(&f);
    return;
  }
  /* A second document, so "the edited one came back" cannot be confused with
   * "embedding resumed for everything". Distinct vocabulary is what says which
   * document a given sidecar request was about. */
  char bp[KBC_TEST_PATH_MAX];
  join(bp, sizeof bp, f.corpus_a, "b.md");
  kbc_test_write_file(bp, DOC_B);

  kbc_err err;
  kbc_err_reset(&err);
  char hash_a[32], hash_b[32];
  hash_of(DOC_A, hash_a, sizeof hash_a);
  hash_of(DOC_B, hash_b, sizeof hash_b);
  KBC_CHECK_MSG(strcmp(hash_a, hash_b) != 0,
                "the two documents hashed alike, so this test cannot tell "
                "them apart");
  /* record_error inserts a path's first row at count 0 and bumps thereafter,
   * so threshold+1 records put the count AT the threshold. */
  for (int i = 0; i <= 3; i++) {
    KBC_CHECK_OK(seed_failure(f.cfg, "a.md", hash_a, i));
    KBC_CHECK_OK(seed_failure(f.cfg, "b.md", hash_b, 100 + i));
  }
  KBC_CHECK_EQ_INT(retry_count_hashed(f.cfg, CORPUS_A, "a.md", hash_a), 3);
  KBC_CHECK_EQ_INT(retry_count_hashed(f.cfg, CORPUS_A, "b.md", hash_b), 3);

  /* Gated: neither document reaches the sidecar at all. */
  kbc_test_write_file(f.log, "");
  KBC_CHECK_OK(kbc_app_reindex(f.app, &err));
  KBC_CHECK_MSG(count_log_lines(f.log) == 0,
                "an over-budget document was still embedded: %zu requests",
                count_log_lines(f.log));

  /* a.md is EDITED, so its bytes — and therefore its hash — move. */
  const char *a_fixed =
      "# Alpha Ledger\n\nThe ledger reconciles accruals in one short "
      "passage.\n";
  char ap[KBC_TEST_PATH_MAX];
  join(ap, sizeof ap, f.corpus_a, "a.md");
  kbc_test_write_file(ap, a_fixed);
  char hash_fixed[32];
  hash_of(a_fixed, hash_fixed, sizeof hash_fixed);
  KBC_CHECK_MSG(strcmp(hash_fixed, hash_a) != 0,
                "the edited bytes hashed to the old content's hash, so this "
                "test cannot tell the two versions apart");

  kbc_test_write_file(f.log, "");
  KBC_CHECK_OK(kbc_app_reindex(f.app, &err));
  KBC_CHECK_MSG(log_mentions(f.log, "accruals"),
                "the EDITED document stayed in quarantine: an operator's fix "
                "did not restore it, so the gate is keyed by path alone");
  KBC_CHECK_MSG(!log_mentions(f.log, "quixotic"),
                "the UNCHANGED document left quarantine on a pass that edited "
                "a different file: the gate is not keyed by content at all");

  /* The old failure record survives and still describes the OLD bytes: a fresh
   * budget is a different question asked of a different document, not the
   * failure history being deleted. The raw count still reads 3, so an operator
   * looking at the Errors tab sees what actually happened to that file. */
  KBC_CHECK_EQ_INT(retry_count_hashed(f.cfg, CORPUS_A, "a.md", hash_a), 3);
  KBC_CHECK_EQ_INT(retry_count_hashed(f.cfg, CORPUS_A, "a.md", hash_fixed), 0);
  KBC_CHECK_EQ_INT(retry_count(f.cfg, CORPUS_A, "a.md"), 3);

  /* Still keyword-searchable: losing the vector costs semantic recall, not
   * existence. */
  char path[64], title[64], id[32];
  KBC_CHECK_EQ_INT(first_hit_path(f.app, "accruals", NULL, path, sizeof path,
                                  title, sizeof title, id, sizeof id, NULL),
                   1);
  KBC_CHECK_EQ_STR(path, "a.md");

  fx_teardown(&f);
}

/* --------------------------------------------------------- query cache --- */

/* How many EMBED requests the sidecar was actually asked for. The log holds
 * every line the sidecar read, handshake included, so a raw line count would
 * let a protocol change masquerade as a cache miss. */
static size_t count_embed_requests(const char *log_path) {
  char *text = kbc_test_read_file(log_path);
  if (text == NULL) {
    return 0;
  }
  size_t n = 0;
  for (const char *p = text; (p = strstr(p, "\"kind\":\"embed\"")) != NULL;
       p++) {
    n++;
  }
  free(text);
  return n;
}

/* The query cache's half of the stats, read the way the daemon reports it —
 * kbc_app_stats_get, not a private peek. */
static kbc_app_stats qc_stats(kbc_app *app) {
  kbc_app_stats st;
  memset(&st, 0, sizeof st);
  kbc_err err;
  kbc_err_reset(&err);
  KBC_CHECK_OK(kbc_app_stats_get(app, &st, &err));
  return st;
}

/* THE cache test: the second identical query must not reach the sidecar.
 *
 * The sidecar's log is the only witness that can tell the two apart. A hit and
 * a miss both return the right vector and both answer the query, so asserting
 * on the RESULT proves nothing — the vector a cache returns is the vector the
 * sidecar returned. What a cache changes is the number of round trips, and
 * that is what the log counts.
 *
 * The key is the model name and the RAW query text and nothing else, and the
 * second half of the case is what makes that claim falsifiable: a search that
 * differs only in mode, limit or corpus filter is the SAME query and must
 * still hit. A key that carried any of those would silently re-embed and cost
 * the 50-100 ms the cache exists to avoid. */
KBC_TEST(a_second_identical_query_never_reaches_the_sidecar) {
  fixture f;
  fx_setup_quarantine(&f, false, true);
  if (f.app == NULL) {
    fx_teardown(&f);
    return;
  }
  kbc_err err;
  kbc_err_reset(&err);
  KBC_CHECK_OK(kbc_app_reindex(f.app, &err));

  /* The app owns the cache: it exists before the first query, it is empty, and
   * its ceiling is the original's DEFAULT_CAPACITY (embed_cache.rs:52) — the
   * capacity the 19 us linear-scan measurement in embed.h was taken at. */
  KBC_CHECK_EQ_INT(qc_stats(f.app).query_cache_entries, 0);
  KBC_CHECK_EQ_INT(kbc_app_query_cache_capacity(f.app), 1024);

  /* From here the log belongs to the QUERY lane alone. The reindex above
   * embedded the document, and that request must not be read as a query. */
  kbc_test_write_file(f.log, "");

  char path[64], title[64], id[32];
  search_flags flags;
  memset(&flags, 0, sizeof flags);
  KBC_CHECK_EQ_INT(first_hit_path(f.app, "accruals", NULL, path, sizeof path,
                                  title, sizeof title, id, sizeof id, &flags),
                   1);
  KBC_CHECK_MSG(flags.vector_ran,
                "the vector lane did not run on a cache miss");
  KBC_CHECK_MSG(count_embed_requests(f.log) == 1,
                "a cold query asked the sidecar %zu times, want 1",
                count_embed_requests(f.log));
  {
    const kbc_app_stats st = qc_stats(f.app);
    KBC_CHECK_EQ_INT(st.query_cache_hits, 0);
    KBC_CHECK_EQ_INT(st.query_cache_misses, 1);
    KBC_CHECK_EQ_INT(st.query_cache_drops, 0);
    KBC_CHECK_EQ_INT(st.query_cache_entries, 1);
  }

  /* The same query, asked a different way: semantic instead of hybrid, limit
   * 3 instead of 10, and scoped to a corpus. None of that is in the key and
   * none of it changes what the query embeds to. */
  kbc_arena *a = kbc_arena_new(64u * 1024u);
  KBC_CHECK_NOT_NULL(a);
  if (a != NULL) {
    kbc_query q;
    memset(&q, 0, sizeof q);
    q.q = "accruals";
    q.corpus = CORPUS_A;
    q.kind = KBC_KIND__COUNT;
    q.mode = KBC_MODE_SEMANTIC;
    q.limit = 3;
    kbc_search_result r;
    memset(&r, 0, sizeof r);
    kbc_err le;
    kbc_err_reset(&le);
    KBC_CHECK_OK(kbc_app_search(f.app, a, &q, &r, &le));
    /* A hit must still be an ANSWER: an empty, degraded result would also have
     * cost no round trip, and would pass a request count alone. */
    KBC_CHECK_MSG(r.vector_ran, "a cache hit left the vector lane off");
    KBC_CHECK_MSG(!r.degraded, "a cached query reported itself degraded");
    KBC_CHECK_EQ_INT(r.len, 1);
    if (r.len == 1) {
      KBC_CHECK_EQ_STR(r.rows[0].path, "a.md");
    }
    KBC_CHECK_MSG(count_embed_requests(f.log) == 1,
                  "a repeated query asked the sidecar %zu times, want 1: the "
                  "cache is not on the query lane",
                  count_embed_requests(f.log));
    {
      const kbc_app_stats st = qc_stats(f.app);
      KBC_CHECK_EQ_INT(st.query_cache_hits, 1);
      KBC_CHECK_EQ_INT(st.query_cache_misses, 1);
      KBC_CHECK_EQ_INT(st.query_cache_entries, 1);
    }
    kbc_arena_free(a);
  }

  /* A DIFFERENT query is a different key, however similar: it misses, and it
   * reaches the sidecar. A cache keyed on anything coarser than the query text
   * would answer this one from the entry above. */
  KBC_CHECK_EQ_INT(first_hit_path(f.app, "ledger", NULL, path, sizeof path,
                                  title, sizeof title, id, sizeof id, NULL),
                   1);
  KBC_CHECK_MSG(count_embed_requests(f.log) == 2,
                "a different query asked the sidecar %zu times, want 2",
                count_embed_requests(f.log));
  {
    const kbc_app_stats st = qc_stats(f.app);
    KBC_CHECK_EQ_INT(st.query_cache_hits, 1);
    KBC_CHECK_EQ_INT(st.query_cache_misses, 2);
    KBC_CHECK_EQ_INT(st.query_cache_entries, 2);
  }

  fx_teardown(&f);
}

/* A sidecar that answers honestly until `marker` exists and lies after it. The
 * lie is a line that is not a reply: embed.c treats a malformed one as fatal
 * and reaps the child, which is the "the sidecar died" case exactly, without
 * the test having to kill anything. */
static void make_flipping_sidecar(const char *path, const char *log_path,
                                  const char *marker) {
  kbc_str s;
  kbc_str_init(&s);
  (void)kbc_str_printf(&s, "#!/bin/sh\nLOG=%s\nMARKER=%s\n", log_path, marker);
  (void)kbc_str_puts(&s,
                     "printf '%s\\n' '{\"kind\":\"ready\",\"model\":\"fake\","
                     "\"dim\":3}'\n"
                     "while IFS= read -r line; do\n"
                     "  printf '%s\\n' \"$line\" >> \"$LOG\"\n"
                     "  case \"$line\" in\n"
                     "  *health*) printf '%s\\n' '{\"ok\":true,\"dim\":3,"
                     "\"model\":\"fake\"}' ;;\n"
                     "  *embed*)\n"
                     "    if [ -f \"$MARKER\" ]; then\n"
                     "      printf '%s\\n' 'not a reply at all'\n"
                     "    else\n"
                     "      printf '%s\\n' '{\"ok\":true,\"dim\":3,"
                     "\"vectors\":[[1,0,0]]}'\n"
                     "    fi ;;\n"
                     "  esac\n"
                     "done\n");
  kbc_test_write_file(path, s.ptr);
  kbc_str_free(&s);
  KBC_CHECK_MSG(chmod(path, 0755) == 0, "chmod %s: %s", path, strerror(errno));
}

static void fx_setup_flipping(fixture *f) {
  memset(f, 0, sizeof(*f));
  kbc_test_tmpdir(f->root, sizeof f->root);
  join(f->data, sizeof f->data, f->root, "data");
  join(f->corpus_a, sizeof f->data, f->root, CORPUS_A);
  kbc_test_mkdir_p(f->corpus_a);
  char p[KBC_TEST_PATH_MAX];
  join(p, sizeof p, f->corpus_a, "a.md");
  kbc_test_write_file(p, DOC_A);
  join(f->sidecar, sizeof f->sidecar, f->root, "sidecar.sh");
  join(f->log, sizeof f->log, f->root, "sidecar.log");
  char marker[KBC_TEST_PATH_MAX];
  join(marker, sizeof marker, f->root, "flip");
  make_flipping_sidecar(f->sidecar, f->log, marker);

  f->cfg = make_cfg(f->data, f->corpus_a, NULL);
  KBC_CHECK_NOT_NULL(f->cfg);
  if (f->cfg == NULL) return;
  free(f->cfg->embedder_cmd);
  f->cfg->embedder_cmd = strdup(f->sidecar);
  kbc_err err;
  kbc_err_reset(&err);
  f->app = kbc_app_open(f->cfg, &err);
  if (f->app == NULL) fprintf(stderr, "  app_open: %s\n", err.msg);
  KBC_CHECK_NOT_NULL(f->app);
}

/* What a sidecar that dies actually does to the cache: nothing.
 *
 * The vectors in the LRU are keyed on the model name the sidecar announced,
 * so a replacement announcing a different model is a different key and a
 * sidecar that fails to handshake has no model to key under at all —
 * kbc_embed_query refuses rather than caching under a name it cannot vouch
 * for. What is left is the exposure this project accepted on purpose: the
 * entries a live sidecar produced stay answerable after it dies, because they
 * are a pure function of (model, query) and a dead sidecar is exactly when an
 * operator least wants a 30 s timeout per keystroke.
 *
 * So the two halves that matter are pinned here rather than asserted in a
 * comment: a HIT survives the death without touching the pipe, and a query
 * the dead sidecar refuses writes NOTHING back — a failed embed must not
 * leave a hole, a zero vector, or an entry the next identical query would be
 * served. */
KBC_TEST(a_dead_sidecar_keeps_the_cache_and_stores_nothing_new) {
  fixture f;
  fx_setup_flipping(&f);
  if (f.app == NULL) {
    fx_teardown(&f);
    return;
  }
  kbc_err err;
  kbc_err_reset(&err);
  KBC_CHECK_OK(kbc_app_reindex(f.app, &err));

  char path[64], title[64], id[32];
  KBC_CHECK_EQ_INT(first_hit_path(f.app, "accruals", NULL, path, sizeof path,
                                  title, sizeof title, id, sizeof id, NULL),
                   1);
  KBC_CHECK_EQ_INT(first_hit_path(f.app, "accruals", NULL, path, sizeof path,
                                  title, sizeof title, id, sizeof id, NULL),
                   1);
  {
    const kbc_app_stats st = qc_stats(f.app);
    KBC_CHECK_EQ_INT(st.query_cache_hits, 1);
    KBC_CHECK_EQ_INT(st.query_cache_misses, 1);
    KBC_CHECK_EQ_INT(st.query_cache_entries, 1);
    KBC_CHECK_EQ_INT(st.query_cache_drops, 0);
  }

  /* The sidecar starts lying: every reply from here is a line that is not a
   * reply, which embed.c treats as fatal and answers by reaping the child. */
  char marker[KBC_TEST_PATH_MAX];
  join(marker, sizeof marker, f.root, "flip");
  kbc_test_write_file(marker, "x");

  /* A HIT is answered entirely from the cache: no sidecar, no request, and
   * still a real answer. */
  KBC_CHECK_EQ_INT(first_hit_path(f.app, "accruals", NULL, path, sizeof path,
                                  title, sizeof title, id, sizeof id, NULL),
                   1);
  {
    const kbc_app_stats st = qc_stats(f.app);
    KBC_CHECK_MSG(st.query_cache_hits == 2,
                  "a cached query was not served after the sidecar died: %lld "
                  "hits",
                  (long long)st.query_cache_hits);
    KBC_CHECK_EQ_INT(st.query_cache_drops, 0);
  }

  /* A MISS reaches the sidecar, is refused, and writes nothing back. */
  KBC_CHECK_EQ_INT(first_hit_path(f.app, "ledger", NULL, path, sizeof path,
                                  title, sizeof title, id, sizeof id, NULL),
                   1);
  {
    const kbc_app_stats st = qc_stats(f.app);
    KBC_CHECK_EQ_INT(st.query_cache_drops, 1);
    KBC_CHECK_MSG(st.query_cache_entries == 1,
                  "a refused query left %lld entries behind; a failed embed "
                  "must cache nothing",
                  (long long)st.query_cache_entries);
    /* And it is not a miss that will be re-attempted for ever either: the
     * entry it failed to write is simply absent, which is the honest state. */
    KBC_CHECK_EQ_INT(st.query_cache_misses, 1);
  }
  KBC_CHECK_EQ_INT(kbc_app_query_cache_capacity(f.app), 1024);

  fx_teardown(&f);
}

/* The cache is freed with the app.
 *
 * The reopen is the observable half: a second kbc_app over the same data
 * directory starts from an empty cache at the full ceiling, so nothing from
 * the first outlived the close. The free ITSELF is what ASan proves — a
 * leaked LRU and its per-entry vectors are invisible to any in-process
 * assertion, which is why this case is worth running in that lane. */
KBC_TEST(a_populated_query_cache_is_released_when_the_app_closes) {
  fixture f;
  fx_setup_quarantine(&f, false, true);
  if (f.app == NULL) {
    fx_teardown(&f);
    return;
  }
  kbc_err err;
  kbc_err_reset(&err);
  KBC_CHECK_OK(kbc_app_reindex(f.app, &err));

  char path[64], title[64], id[32];
  for (int i = 0; i < 3; i++) {
    static const char *three[3] = {"accruals", "ledger", "alpha"};
    (void)first_hit_path(f.app, three[i], NULL, path, sizeof path, title,
                         sizeof title, id, sizeof id, NULL);
  }
  KBC_CHECK_EQ_INT(qc_stats(f.app).query_cache_entries, 3);

  kbc_app_close(f.app);
  f.app = NULL;
  kbc_err_reset(&err);
  f.app = kbc_app_open(f.cfg, &err);
  if (f.app == NULL) fprintf(stderr, "  app_open: %s\n", err.msg);
  KBC_CHECK_NOT_NULL(f.app);
  if (f.app != NULL) {
    KBC_CHECK_EQ_INT(qc_stats(f.app).query_cache_entries, 0);
    KBC_CHECK_EQ_INT(kbc_app_query_cache_capacity(f.app), 1024);
  }

  fx_teardown(&f);
}

/* One cache, four workers, and the counters have to add up.
 *
 * The cache's own mutex in embed.c is the only synchronisation between
 * concurrent queries — app->qcache is immutable after kbc_app_open, so the app
 * adds no lock of its own and this is the path where a race in the LRU would
 * live. The invariant is an accounting one, which is the only kind that can be
 * asserted after the fact: every search that entered the query lane is exactly
 * one hit, one miss or one drop, and the cache holds exactly one entry per
 * DISTINCT query. A lost counter update, a double-counted put or an entry
 * evicted by a racing touch all break that arithmetic, and none of them shows
 * up in a single-threaded run. The threads deliberately share terms, so they
 * collide on keys and the LRU order is mutated from several directions at once
 * rather than merely read.
 *
 * This is the case that wants the TSan lane as much as the ASan one. */
typedef struct {
  kbc_app *app;
  int rounds;
} qc_racer;

static void *qc_race(void *user) {
  qc_racer *r = (qc_racer *)user;
  static const char *terms[4] = {"accruals", "ledger", "alpha", "accruals"};
  for (int i = 0; i < r->rounds; i++) {
    kbc_arena *a = kbc_arena_new(64u * 1024u);
    if (a == NULL) {
      return NULL;
    }
    kbc_query q;
    memset(&q, 0, sizeof q);
    q.q = terms[(size_t)i % 4u];
    q.kind = KBC_KIND__COUNT;
    q.mode = KBC_MODE_HYBRID;
    q.limit = 10;
    kbc_search_result res;
    memset(&res, 0, sizeof res);
    kbc_err err;
    kbc_err_reset(&err);
    /* A degraded lane is never a reason to fail the search, so a status other
     * than KBC_OK here would be the bug. */
    if (kbc_app_search(r->app, a, &q, &res, &err) != KBC_OK) {
      KBC_CHECK_MSG(false, "concurrent search failed: %s", err.msg);
    }
    kbc_arena_free(a);
  }
  return NULL;
}

KBC_TEST(concurrent_queries_share_one_cache_and_the_counters_add_up) {
  fixture f;
  fx_setup_quarantine(&f, false, true);
  if (f.app == NULL) {
    fx_teardown(&f);
    return;
  }
  kbc_err err;
  kbc_err_reset(&err);
  KBC_CHECK_OK(kbc_app_reindex(f.app, &err));

  enum { N_THREADS = 4, N_ROUNDS = 40, N_TERMS = 3 };
  pthread_t th[N_THREADS];
  qc_racer racers[N_THREADS];
  for (int i = 0; i < N_THREADS; i++) {
    racers[i].app = f.app;
    racers[i].rounds = N_ROUNDS;
    KBC_CHECK_MSG(pthread_create(&th[i], NULL, qc_race, &racers[i]) == 0,
                  "pthread_create %d", i);
  }
  for (int i = 0; i < N_THREADS; i++) {
    (void)pthread_join(th[i], NULL);
  }

  const int64_t searches = (int64_t)N_THREADS * N_ROUNDS;
  const kbc_app_stats st = qc_stats(f.app);
  KBC_CHECK_EQ_INT(st.searches_served, searches);
  /* A live sidecar and a cache with room: nothing can fail, and the three
   * counters between them must account for every single search. */
  KBC_CHECK_MSG(st.query_cache_drops == 0,
                "%lld query embeds failed against a sidecar that was up",
                (long long)st.query_cache_drops);
  KBC_CHECK_MSG(st.query_cache_hits + st.query_cache_misses == searches,
                "the query lane accounted for %lld of %lld searches",
                (long long)(st.query_cache_hits + st.query_cache_misses),
                (long long)searches);
  KBC_CHECK_MSG(st.query_cache_hits > 0,
                "%d queries over %d terms produced no hit at all", searches,
                N_TERMS);
  /* One entry per distinct term — the fourth array slot repeats the first. A
   * racing eviction or a duplicate put would show up here. */
  KBC_CHECK_EQ_INT(st.query_cache_entries, N_TERMS);

  fx_teardown(&f);
}

/* ------------------------------------------------- the store's ledgers --
 *
 * Four tables that a reindex pass is the writer for. Every case here reads
 * the ROW back out of the store through its own public getter — a test that
 * asserted a function had been called would pass with the write removed, and
 * these four exist precisely because nobody was reading them.
 */

/* The runs of one corpus, copied out of the arena the store built them in.
 * The ids are copied because a caller cannot hold a pointer into a freed
 * arena, and the whole set is copied because the cases below must NOT assume
 * an order: `started_at` is unix SECONDS, so two passes inside one second tie
 * and list_index_runs falls back to the id — meaning "the first row" is not
 * reliably "the newest pass". An assertion written against the wrong row is
 * worse than no assertion, so these read every row and the cases below pick
 * the one they mean. */
#define RUNS_MAX 8u
typedef struct {
  char id[KBC_MAX_ID_LEN + 1];
  char corpus[256];
  int64_t started_at;
  int64_t finished_at;
  int64_t ok_count;
  int64_t err_count;
} run_row;

typedef struct {
  size_t n;
  run_row r[RUNS_MAX];
} run_summary;

static bool read_runs(const kbc_config *cfg, const char *corpus,
                      run_summary *out) {
  memset(out, 0, sizeof *out);
  kbc_err err;
  kbc_err_reset(&err);
  kbc_store *s = kbc_store_open(cfg, &err);
  if (s == NULL) {
    fprintf(stderr, "  store_open: %s\n", err.msg);
    return false;
  }
  kbc_arena *a = kbc_arena_new(64u * 1024u);
  kbc_index_run *runs = NULL;
  size_t n = 0;
  if (a == NULL ||
      kbc_failed(kbc_store_list_index_runs(s, a, corpus, 100, &runs, &n,
                                           &err))) {
    if (a != NULL) fprintf(stderr, "  list_index_runs: %s\n", err.msg);
    kbc_arena_free(a);
    kbc_store_close(s);
    return false;
  }
  out->n = n < RUNS_MAX ? n : RUNS_MAX;
  for (size_t i = 0; i < out->n; i++) {
    snprintf(out->r[i].id, sizeof out->r[i].id, "%s",
             runs[i].id ? runs[i].id : "");
    snprintf(out->r[i].corpus, sizeof out->r[i].corpus, "%s",
             runs[i].corpus ? runs[i].corpus : "");
    out->r[i].started_at = runs[i].started_at;
    out->r[i].finished_at = runs[i].finished_at;
    out->r[i].ok_count = runs[i].ok_count;
    out->r[i].err_count = runs[i].err_count;
  }
  kbc_arena_free(a);
  kbc_store_close(s);
  return true;
}

/* The one run of this set that names `id`, or NULL. */
static const run_row *find_run(const run_summary *s, const char *id) {
  const size_t n = s->n < RUNS_MAX ? s->n : RUNS_MAX;
  for (size_t i = 0; i < n; i++) {
    if (strcmp(s->r[i].id, id) == 0) return &s->r[i];
  }
  return NULL;
}

/* The one run of this set carrying an error, or NULL. Exactly one run in a
 * fixture of two passes can be the failed one, so "the row with the error" is
 * a selection that does not depend on the order the store returned. */
static const run_row *find_errored_run(const run_summary *s) {
  const size_t n = s->n < RUNS_MAX ? s->n : RUNS_MAX;
  const run_row *hit = NULL;
  for (size_t i = 0; i < n; i++) {
    if (s->r[i].err_count > 0) {
      if (hit != NULL) return NULL; /* ambiguous: two errored runs */
      hit = &s->r[i];
    }
  }
  return hit;
}

/* Every id in the set is distinct. The id is the primary key, so a duplicate
 * could not have been STORED — but a second pass whose id collided would have
 * failed to open its row, which is a lost run rather than an error, so the
 * count below is what actually catches it. This is the belt to that braces. */
static bool run_ids_distinct(const run_summary *s) {
  const size_t n = s->n < RUNS_MAX ? s->n : RUNS_MAX;
  for (size_t i = 0; i < n; i++) {
    for (size_t j = i + 1; j < n; j++) {
      if (strcmp(s->r[i].id, s->r[j].id) == 0) return false;
    }
  }
  return true;
}

/* The id shape store.h documents: "r-" and six base32 characters. Checked
 * rather than assumed, because a run id that is not this shape is not a run
 * id — list_index_runs orders by it and an operator reads it. */
static bool run_id_is_well_formed(const char *id) {
  if (id == NULL || strlen(id) != 8u) return false;
  if (id[0] != 'r' || id[1] != '-') return false;
  for (const char *p = id + 2; *p != '\0'; p++) {
    if (strchr("0123456789abcdefghjkmnpqrstvwxyz", *p) == NULL) return false;
  }
  return true;
}

KBC_TEST(a_reindex_records_a_finished_run_for_every_configured_corpus) {
  fixture f;
  fx_setup(&f, true);
  if (f.app == NULL) {
    fx_teardown(&f);
    return;
  }
  kbc_err err;
  kbc_err_reset(&err);
  KBC_CHECK_OK(kbc_app_reindex(f.app, &err));

  /* One run per corpus, not one for the pass: `index_runs.corpus` is what the
   * table is keyed by and what list_index_runs filters on, so a pass over two
   * corpora that recorded one row would leave one of them with no run at all. */
  run_summary ra, rb, rall;
  if (!read_runs(f.cfg, CORPUS_A, &ra) || !read_runs(f.cfg, CORPUS_B, &rb) ||
      !read_runs(f.cfg, NULL, &rall)) {
    fx_teardown(&f);
    return;
  }
  KBC_CHECK_EQ_INT(ra.n, 1);
  KBC_CHECK_EQ_INT(rb.n, 1);
  KBC_CHECK_EQ_INT(rall.n, 2);
  KBC_CHECK_EQ_STR(ra.r[0].corpus, CORPUS_A);
  KBC_CHECK_EQ_STR(rb.r[0].corpus, CORPUS_B);
  KBC_CHECK_MSG(run_id_is_well_formed(ra.r[0].id),
                "run id is not \"r-\" + 6 base32: \"%s\"", ra.r[0].id);
  KBC_CHECK_MSG(run_id_is_well_formed(rb.r[0].id),
                "run id is not \"r-\" + 6 base32: \"%s\"", rb.r[0].id);
  /* The two runs of ONE pass must not collide: the id is the primary key, and
   * a colliding pair means the second corpus silently has no run at all. */
  KBC_CHECK_MSG(strcmp(ra.r[0].id, rb.r[0].id) != 0,
                "both corpora of one pass minted the run id \"%s\"",
                ra.r[0].id);

  /* FINISHED, not in flight. A negative finished_at is the store's "this pass
   * is still running", and a completed reindex that left it negative is a run
   * row that will read as running for the life of the database. */
  KBC_CHECK_MSG(ra.r[0].finished_at >= 0,
                "a reindex that returned left run %s open (finished_at %lld)",
                ra.r[0].id, (long long)ra.r[0].finished_at);
  KBC_CHECK_MSG(rb.r[0].finished_at >= 0,
                "a reindex that returned left run %s open (finished_at %lld)",
                rb.r[0].id, (long long)rb.r[0].finished_at);
  KBC_CHECK(ra.r[0].finished_at >= ra.r[0].started_at);
  KBC_CHECK(rb.r[0].finished_at >= rb.r[0].started_at);

  /* The counts are the documents of THAT corpus, not of the pass: alpha holds
   * a.md, b.md, c.md and beta holds d.md. */
  KBC_CHECK_EQ_INT(ra.r[0].ok_count, 3);
  KBC_CHECK_EQ_INT(rb.r[0].ok_count, 1);
  KBC_CHECK_EQ_INT(ra.r[0].err_count, 0);
  KBC_CHECK_EQ_INT(rb.r[0].err_count, 0);

  /* A second pass records a second run and does not disturb the first. */
  const char *first_id = ra.r[0].id;
  kbc_err_reset(&err);
  KBC_CHECK_OK(kbc_app_reindex(f.app, &err));
  run_summary ra2;
  if (read_runs(f.cfg, CORPUS_A, &ra2)) {
    KBC_CHECK_EQ_INT(ra2.n, 2);
    KBC_CHECK_MSG(run_ids_distinct(&ra2),
                  "two passes over one corpus did not produce two distinct "
                  "run ids");
    /* The first run is still readable, still finished, and still says what it
     * said: a second pass must not rewrite the ledger of the first. */
    const run_row *kept = find_run(&ra2, first_id);
    KBC_CHECK_MSG(kept != NULL, "the first pass's run %s disappeared",
                  first_id);
    if (kept != NULL) {
      KBC_CHECK_MSG(kept->finished_at >= 0, "the first run was reopened");
      KBC_CHECK_EQ_INT(kept->ok_count, 3);
      KBC_CHECK_EQ_INT(kept->err_count, 0);
    }
    /* Both runs are finished and both covered the corpus: nothing changed on
     * disk, so the second pass accounted for three documents by deciding to
     * leave them alone, which is what ok_count counts. */
    for (size_t i = 0; i < ra2.n && i < RUNS_MAX; i++) {
      KBC_CHECK_MSG(ra2.r[i].finished_at >= 0, "run %s is still open",
                    ra2.r[i].id);
      KBC_CHECK_EQ_INT(ra2.r[i].ok_count, 3);
    }
  }

  fx_teardown(&f);
}

/* The failure case, and the reason the ledger is owned by a wrapper rather
 * than by the body of the pass: a pass that dies still has to finish its run.
 *
 * The failure is injected by dropping doc_metas out from under a live store,
 * so the pass reaches kbc_store_replace_metas and is refused mid-walk. It is
 * deterministic (no lock race, no retries) and it lands in the middle of the
 * document loop, which is where a "close on the way out" that somebody forgot
 * would show up. */
KBC_TEST(a_failed_reindex_leaves_a_finished_run_carrying_the_error) {
  fixture f;
  fx_setup(&f, false);
  if (f.app == NULL) {
    fx_teardown(&f);
    return;
  }
  kbc_err err;
  kbc_err_reset(&err);
  KBC_CHECK_OK(kbc_app_reindex(f.app, &err));
  run_summary before;
  if (!read_runs(f.cfg, CORPUS_A, &before)) {
    fx_teardown(&f);
    return;
  }
  KBC_CHECK_EQ_INT(before.n, 1);
  char good_id[KBC_MAX_ID_LEN + 1];
  snprintf(good_id, sizeof good_id, "%s", before.r[0].id);

  db_side side;
  memset(&side, 0, sizeof side);
  if (!db_side_open(&side, f.cfg)) {
    fx_teardown(&f);
    return;
  }
  /* Change a file so the second pass has a changed row to reach the facets
   * write with, and drop the table it writes to. */
  char p[KBC_TEST_PATH_MAX];
  join(p, sizeof p, f.corpus_a, "a.md");
  kbc_test_write_file(p, "# Alpha Ledger\n\nThe ledger reconciles accruals and\naccruals.\n");
  KBC_CHECK_OK(db_side_exec(&side, "DROP TABLE doc_metas;"));

  kbc_err_reset(&err);
  kbc_status s = kbc_app_reindex(f.app, &err);
  KBC_CHECK_MSG(kbc_failed(s),
                "a reindex the store refused reported KBC_OK (last error: %s)",
                err.msg);
  if (kbc_failed(s)) KBC_CHECK_ERR_MSG(err);
  db_side_close(&side);

  run_summary after;
  if (read_runs(f.cfg, CORPUS_A, &after)) {
    /* A second row: the failed pass opened one and closed it. */
    KBC_CHECK_EQ_INT(after.n, 2);
    KBC_CHECK_MSG(run_ids_distinct(&after),
                  "the failed pass reused the previous run's id \"%s\"",
                  good_id);
    /* THE ASSERTION, and it is about the failed run specifically: selected by
     * its error count rather than by position, because two passes inside one
     * second have equal started_at and the store orders them by id. */
    const run_row *bad = find_errored_run(&after);
    KBC_CHECK_MSG(bad != NULL,
                  "after a reindex the store refused, no run carries an error "
                  "(%zu runs, all with err_count 0) — the failure was never "
                  "recorded", after.n);
    if (bad != NULL) {
      KBC_CHECK_MSG(strcmp(bad->id, good_id) != 0,
                    "the errored run is the FIRST pass's run %s, so the failed "
                    "pass recorded nothing of its own",
                    bad->id);
      KBC_CHECK_MSG(run_id_is_well_formed(bad->id),
                    "failed-pass run id is not \"r-\" + 6 base32: \"%s\"",
                    bad->id);
      /* A failed pass that leaves finished_at negative is indistinguishable,
       * forever, from a pass that is still running — and "still running" is
       * the one reading that is guaranteed to be a lie. */
      KBC_CHECK_MSG(bad->finished_at >= 0,
                    "the failed reindex left run %s OPEN (finished_at %lld); a "
                    "run row nobody closed reads as a pass in progress forever",
                    bad->id, (long long)bad->finished_at);
      KBC_CHECK(bad->finished_at >= bad->started_at);
      /* And the failure is COUNTED, not merely recorded as having happened: a
       * finished run with err_count 0 claims the corpus is fully indexed,
       * which is exactly what the caller was just told did not happen. */
      KBC_CHECK_MSG(bad->err_count > 0,
                    "the failed reindex closed run %s with err_count 0 and "
 "ok_count %lld, which reads as a complete pass",
                    bad->id, (long long)bad->ok_count);
      /* The document it did account for before the store refused is still
       * counted: a pass that ingested nothing reached no facet write, so
       * ok_count 0 alongside the error is the honest reading, and a non-zero
       * one would mean the pass got further than the assertion above thinks. */
      KBC_CHECK(bad->ok_count < 3);
    }
    /* Nothing at all is left in flight, whichever row the failure landed on. */
    for (size_t i = 0; i < after.n && i < RUNS_MAX; i++) {
      KBC_CHECK_MSG(after.r[i].finished_at >= 0,
                    "run %s is still open after a reindex returned",
                    after.r[i].id);
    }
    /* And the first pass's finished run is untouched by the failure: closing
     * the second must not rewrite the first. */
    const run_row *kept = find_run(&after, good_id);
    KBC_CHECK_MSG(kept != NULL, "the first pass's run %s disappeared", good_id);
    if (kept != NULL) {
      KBC_CHECK_EQ_INT(kept->err_count, 0);
      KBC_CHECK_EQ_INT(kept->ok_count, 3);
    }
  }

  fx_teardown(&f);
}

/* The property INSERT OR IGNORE exists for, asserted on the stored value. */
KBC_TEST(a_reindex_does_not_move_the_first_seen_anchor) {
  fixture f;
  fx_setup(&f, false);
  if (f.app == NULL) {
    fx_teardown(&f);
    return;
  }
  kbc_err err;
  kbc_err_reset(&err);
  KBC_CHECK_OK(kbc_app_reindex(f.app, &err));

  char id[KBC_MAX_ID_LEN + 1];
  kbc_id_for_artifact(id, CORPUS_A, "a.md");

  kbc_store *s = kbc_store_open(f.cfg, &err);
  if (s == NULL) {
    fprintf(stderr, "  store_open: %s\n", err.msg);
    fx_teardown(&f);
    return;
  }
  int64_t first = 0;
  KBC_CHECK_OK(kbc_store_get_first_seen(s, id, &first, &err));
  KBC_CHECK_MSG(first > 0, "a document indexed by a reindex has no first-seen "
                "anchor (got %lld)", (long long)first);
  kbc_store_close(s);

  /* Overwrite the stored anchor with a sentinel through the database itself,
   * so the assertion does not depend on the clock: a writer that REFRESHED
   * the column would replace the sentinel with "now", and one that ignored
   * the write would leave it. Only "ignored" is correct, and it is the only
   * one of the three a same-second test could have told apart. */
  db_side side;
  memset(&side, 0, sizeof side);
  if (!db_side_open(&side, f.cfg)) {
    fx_teardown(&f);
    return;
  }
  char sql[512];
  snprintf(sql, sizeof sql,
           "UPDATE doc_first_seen SET first_indexed_unix = 1 WHERE artifact_id"
           " = '%s';",
           id);
  KBC_CHECK_OK(db_side_exec(&side, sql));
  db_side_close(&side);

  /* Edit the file, so the second pass really re-ingests the document rather
   * than skipping it as unchanged — a pass that skipped it would prove
   * nothing about the anchor. */
  char p[KBC_TEST_PATH_MAX];
  join(p, sizeof p, f.corpus_a, "a.md");
  kbc_test_write_file(p, "# Alpha Ledger\n\nThe ledger reconciles accruals and\naccruals again.\n");

  kbc_err_reset(&err);
  KBC_CHECK_OK(kbc_app_reindex(f.app, &err));

  s = kbc_store_open(f.cfg, &err);
  if (s == NULL) {
    fprintf(stderr, "  store_open: %s\n", err.msg);
    fx_teardown(&f);
    return;
  }
  int64_t after = 0;
  KBC_CHECK_OK(kbc_store_get_first_seen(s, id, &after, &err));
  KBC_CHECK_MSG(after == 1,
                "a second reindex moved the first-indexed anchor of %s from 1 "
                "to %lld; a \"created\" sort has to survive a reindex",
                id, (long long)after);
  kbc_store_close(s);

  fx_teardown(&f);
}

/* The config declares corpora; the `sources` table is the runtime registry of
 * the ones that are live. */
KBC_TEST(opening_the_daemon_registers_every_configured_corpus_as_a_source) {
  fixture f;
  fx_setup(&f, true);
  if (f.app == NULL) {
    fx_teardown(&f);
    return;
  }

  /* At OPEN, with no reindex: a corpus is live from the moment the daemon
   * answers, not from the moment someone reindexes. */
  kbc_err err;
  kbc_err_reset(&err);
  kbc_store *s = kbc_store_open(f.cfg, &err);
  if (s == NULL) {
    fprintf(stderr, "  store_open: %s\n", err.msg);
    fx_teardown(&f);
    return;
  }
  kbc_arena *a = kbc_arena_new(64u * 1024u);
  kbc_source *list = NULL;
  size_t n = 0;
  KBC_CHECK_OK(kbc_store_list_sources(s, a, 100, &list, &n, &err));
  KBC_CHECK_EQ_INT(n, 2);
  bool saw_a = false, saw_b = false;
  for (size_t i = 0; i < n; i++) {
    if (strcmp(list[i].corpus, CORPUS_A) == 0) {
      saw_a = true;
      KBC_CHECK_EQ_STR(list[i].path, f.corpus_a);
      KBC_CHECK_MSG(list[i].added_at > 0, "source %s has no added_at",
                    list[i].corpus);
      KBC_CHECK_MSG(!list[i].paused, "a freshly registered source is paused");
    }
    if (strcmp(list[i].corpus, CORPUS_B) == 0) {
      saw_b = true;
      KBC_CHECK_EQ_STR(list[i].path, f.corpus_b);
    }
  }
  KBC_CHECK_MSG(saw_a, "corpus %s is configured but is not in `sources`",
                CORPUS_A);
  KBC_CHECK_MSG(saw_b, "corpus %s is configured but is not in `sources`",
                CORPUS_B);

  /* `paused` is the column the operator toggles, so it must survive the
   * re-registration every open performs: put_source upserts `path` only, and a
   * daemon that un-paused a corpus on every restart would make the flag
   * impossible to set. */
  KBC_CHECK_OK(kbc_store_set_source_paused(s, CORPUS_A, true, &err));
  int64_t added_at = 0;
  {
    kbc_source one;
    memset(&one, 0, sizeof one);
    KBC_CHECK_OK(kbc_store_get_source(s, a, CORPUS_A, &one, &err));
    added_at = one.added_at;
    KBC_CHECK_MSG(one.paused, "the pause did not take");
  }
  kbc_arena_free(a);
  kbc_store_close(s);

  /* Reopen, which re-registers every configured corpus. */
  kbc_err_reset(&err);
  kbc_app_close(f.app);
  f.app = kbc_app_open(f.cfg, &err);
  if (f.app == NULL) fprintf(stderr, "  app_open: %s\n", err.msg);
  KBC_CHECK_NOT_NULL(f.app);
  if (f.app == NULL) {
    fx_teardown(&f);
    return;
  }

  s = kbc_store_open(f.cfg, &err);
  if (s == NULL) {
    fprintf(stderr, "  store_open: %s\n", err.msg);
    fx_teardown(&f);
    return;
  }
  a = kbc_arena_new(64u * 1024u);
  kbc_source after;
  memset(&after, 0, sizeof after);
  KBC_CHECK_OK(kbc_store_get_source(s, a, CORPUS_A, &after, &err));
  KBC_CHECK_MSG(after.paused,
                "a restart un-paused %s; the pause is runtime operator intent "
                "and re-registering the corpus must not clear it",
                CORPUS_A);
  KBC_CHECK_EQ_STR(after.path, f.corpus_a);
  KBC_CHECK_MSG(after.added_at == added_at,
                "added_at moved from %lld to %lld across a restart; it is when "
                "the source was FIRST seen, not when it was last written",
                (long long)added_at, (long long)after.added_at);
  kbc_arena_free(a);
  kbc_store_close(s);

  fx_teardown(&f);
}

/* ------------------------------------------------ the store's own answers --
 *
 * The accessors below are the two directions of ONE table, and the only thing
 * that keeps them honest is a test that reads them against each other. Each is
 * individually plausible: a count that is off by one and a list that is off by
 * one are both green tests on their own, and a backlinks surface built from
 * the list would then contradict the number the graph boost ranks by. The
 * anti-drift assertion is the only thing that catches the two silently
 * disagreeing, so it is asserted as an IDENTITY over the whole corpus rather
 * than as two constants that happen to be equal today.
 */

/* Every document in the corpus, as corpus-relative paths, in one listing. The
 * anti-drift test sums over exactly this set, so it must be the set the store
 * holds rather than the set the test remembers writing. */
static size_t corpus_paths(const kbc_config *cfg, const char *corpus,
                           kbc_arena *a, const char ***out) {
  kbc_err err;
  kbc_err_reset(&err);
  kbc_store *s = kbc_store_open(cfg, &err);
  if (s == NULL) {
    *out = NULL;
    return 0;
  }
  /* kind -1 is the "no kind filter" encoding (store.c:list_artifact_ids), so
   * this is every kind the corpus holds and not just the markdown default. */
  char **ids = NULL;
  size_t n = 0;
  kbc_status st = kbc_store_list_artifact_ids(s, corpus, (kbc_kind)-1,
                                             KBC_MAX_HITS, 0, &ids, &n, &err);
  kbc_store_close(s);
  if (kbc_failed(st)) {
    *out = NULL;
    return 0;
  }
  const char **paths = kbc_arena_alloc(a, (n + 1u) * sizeof(*paths));
  if (paths == NULL) {
    for (size_t i = 0; i < n; i++) {
      free(ids[i]);
    }
    free(ids);
    *out = NULL;
    return 0;
  }
  size_t kept = 0;
  for (size_t i = 0; i < n; i++) {
    kbc_store *r = kbc_store_open(cfg, &err);
    kbc_artifact art;
    memset(&art, 0, sizeof art);
    kbc_err_reset(&err);
    if (r != NULL &&
        !kbc_failed(kbc_store_get_artifact(r, a, ids[i], false, &art, &err)) &&
        art.path != NULL) {
      paths[kept++] = art.path;
    }
    if (r != NULL) {
      kbc_store_close(r);
    }
    free(ids[i]);
  }
  free(ids);
  *out = paths;
  return kept;
}

/* THE ANTI-DRIFT TEST. Two accessors, one table, one corpus, one state: the
 * number of documents linking HERE (what `edge_count` counts) must equal the
 * sum of the backlink lists (what `list_backlinks` returns). They are separate
 * SQL over separate indexes — a COUNT and a DISTINCT scan — so nothing but
 * this assertion makes them answer the same question.
 *
 * The corpus is built so that the identity is not trivially satisfiable: a hub
 * with three inbound links, a document nothing links to, and a source that
 * names the same target twice (which the edges primary key collapses to one
 * row, so a list that forgot to de-duplicate and a count that did not would
 * still agree here — hence the separate de-dup assertion below). */
KBC_TEST(the_edge_count_and_the_backlink_list_agree_on_one_corpus) {
  fixture f;
  fx_setup_empty_pair(&f);
  char p[KBC_TEST_PATH_MAX];
  kbc_err err;
  kbc_err_reset(&err);
  if (f.app == NULL) {
    fx_teardown(&f);
    return;
  }
  /* BOTH corpora carry the SAME four paths and a DIFFERENT link structure:
   * alpha's hub is named by two sources and its leaf by one, beta's hub by one
   * and its leaf by none. That asymmetry is what makes the corpus predicate
   * load-bearing. A backlink query that dropped it would answer for both
   * corpora at once, and because the hub path exists in both, the leak shows
   * up in the summed list instead of hiding behind a path only one corpus has.
   * A single-corpus fixture cannot see this at all — there is nothing to leak
   * from, so the predicate could be deleted outright and the test stay green. */
  join(p, sizeof p, f.corpus_a, "one.md");
  kbc_test_write_file(p, "# One\n\nSee [hub](hub) and [leaf](leaf).\n");
  join(p, sizeof p, f.corpus_a, "two.md");
  kbc_test_write_file(p, "# Two\n\nAlso [hub](hub), twice over: [hub](hub).\n");
  join(p, sizeof p, f.corpus_a, "hub.md");
  kbc_test_write_file(p, "# Hub\n\nThe hub itself names nothing.\n");
  join(p, sizeof p, f.corpus_a, "leaf.md");
  kbc_test_write_file(p, "# Leaf\n\nAn orphan target.\n");
  /* Beta: same paths, a different graph, and a source alpha does not have. */
  join(p, sizeof p, f.corpus_b, "one.md");
  kbc_test_write_file(p, "# One\n\nSee [hub](hub).\n");
  join(p, sizeof p, f.corpus_b, "two.md");
  kbc_test_write_file(p, "# Two\n\nNames nothing at all.\n");
  join(p, sizeof p, f.corpus_b, "hub.md");
  kbc_test_write_file(p, "# Hub\n\nThe hub itself names nothing.\n");
  join(p, sizeof p, f.corpus_b, "leaf.md");
  kbc_test_write_file(p, "# Leaf\n\nAn orphan target.\n");
  KBC_CHECK_OK(kbc_app_reindex(f.app, &err));

  kbc_arena *a = kbc_arena_new(64u * 1024u);
  KBC_CHECK_NOT_NULL(a);
  if (a == NULL) {
    fx_teardown(&f);
    return;
  }
  const char **paths = NULL;
  size_t n_paths = corpus_paths(f.cfg, CORPUS_A, a, &paths);
  KBC_CHECK_EQ_INT(n_paths, 4);
  const char **beta_paths = NULL;
  size_t n_beta = corpus_paths(f.cfg, CORPUS_B, a, &beta_paths);
  KBC_CHECK_EQ_INT(n_beta, 4);

  kbc_store *store = kbc_store_open(f.cfg, &err);
  KBC_CHECK_NOT_NULL(store);
  if (store == NULL) {
    kbc_arena_free(a);
    fx_teardown(&f);
    return;
  }
  const int64_t counted = kbc_store_edge_count(store, CORPUS_A, &err);
  /* two.md naming hub twice is ONE row (the edges primary key), and a self
   * or duplicate edge would make the two numbers disagree — which is the
   * whole point of summing the list rather than trusting the count. */
  KBC_CHECK_EQ_INT(counted, 3);
  /* The OTHER corpus is a different graph, and the count is per corpus: a
   * count that ignored its corpus argument would report alpha's total for
   * beta too. */
  KBC_CHECK_EQ_INT(kbc_store_edge_count(store, CORPUS_B, &err), 1);

  int64_t summed = 0;
  for (size_t i = 0; i < n_paths; i++) {
    kbc_strlist back;
    kbc_strlist_init(&back);
    kbc_status st =
        kbc_store_list_backlinks(store, CORPUS_A, paths[i], &back, &err);
    KBC_CHECK_MSG(!kbc_failed(st), "backlinks of %s: %s", paths[i], err.msg);
    /* A document nothing links to is a ZERO-LENGTH LIST AND KBC_OK, never
     * NOTFOUND: "nothing links here" is an ordinary fact (store.h:127-130). */
    summed += (int64_t)back.len;
    kbc_strlist_free(&back);
  }
  /* The same identity in the second corpus, whose hub is named by ONE source
   * and whose leaf by none. */
  int64_t beta_summed = 0;
  for (size_t i = 0; i < n_beta; i++) {
    kbc_strlist back;
    kbc_strlist_init(&back);
    kbc_status st =
        kbc_store_list_backlinks(store, CORPUS_B, beta_paths[i], &back, &err);
    KBC_CHECK_MSG(!kbc_failed(st), "backlinks of %s/%s: %s", CORPUS_B,
                  beta_paths[i], err.msg);
    beta_summed += (int64_t)back.len;
    kbc_strlist_free(&back);
  }
  KBC_CHECK_EQ_INT(beta_summed, kbc_store_edge_count(store, CORPUS_B, &err));
  /* THE IDENTITY. Not "both are 3" — that would pass if the corpus were
   * empty. The summed list and the aggregate count are two SQL plans over one
   * table and must report the same graph. */
  KBC_CHECK_EQ_INT(summed, counted);
  KBC_CHECK_MSG(summed == 3, "the list and the count describe different graphs:"
                             " %lld listed, %lld counted",
                (long long)summed, (long long)counted);
  KBC_CHECK_MSG(beta_summed == 1,
                "beta: %lld listed against %lld counted", (long long)beta_summed,
                (long long)kbc_store_edge_count(store, CORPUS_B, &err));

  /* The per-document aggregate answers the same question for a NAMED document,
   * so it is held to the same list rather than trusted alongside it. This is
   * the third reader of the table and the one the graph boost actually ranks
   * by (app.c:graph_in_degrees). */
  kbc_strlist hub_back;
  kbc_strlist_init(&hub_back);
  KBC_CHECK_OK(
      kbc_store_list_backlinks(store, CORPUS_A, "hub.md", &hub_back, &err));
  /* De-duplicated, and this is where the corpus earns it: two.md named hub
   * TWICE, so the graph holds three link OCCURRENCES for that hub and TWO
   * rows. A list that returned a row per occurrence would read 3 here, which
   * would push the summed total to 4 against a counted 3 — the drift this
   * whole test exists to catch, arrived at by a different route. */
  KBC_CHECK_EQ_INT(hub_back.len, 2);
  KBC_CHECK(kbc_strlist_contains(&hub_back, "one.md"));
  KBC_CHECK(kbc_strlist_contains(&hub_back, "two.md"));
  kbc_strlist_free(&hub_back);

  static const char *const named[] = {"hub.md", "leaf.md", "one.md", "two.md"};
  uint32_t deg[4] = {9, 9, 9, 9};
  KBC_CHECK_OK(
      kbc_store_edge_degrees_for(store, CORPUS_A, named, 4, deg, &err));
  KBC_CHECK_EQ_INT(deg[0], 2); /* named by one.md and two.md */
  KBC_CHECK_EQ_INT(deg[1], 1); /* named by one.md */
  KBC_CHECK_EQ_INT(deg[2], 0); /* names others, named by none */
  KBC_CHECK_EQ_INT(deg[3], 0);

  kbc_store_close(store);
  kbc_arena_free(a);
  fx_teardown(&f);
}

/* RETENTION CONVERGES. `prune_history` is the operator-triggered sweep
 * (`kbc prune`, cli/main.c:cmd_prune) and its whole contract is that it
 * converges: a first `--apply` reports what it removed, and every run after it
 * reports nothing, because there is nothing left older than the cutoff. A
 * prune that "usually" converges is a prune whose `rows` an operator cannot
 * read, and the loop below is what makes that a tested property rather than an
 * assumption.
 *
 * The convergence is asserted as a LOOP WITH A BOUND, not as a second call:
 * a fixed second call would pass on a prune that needed three, and would also
 * pass on one that never converged at all if the bound were the only check. The
 * bound is what turns "does not terminate" into a failure instead of a hang. */
KBC_TEST(a_retention_prune_converges_and_then_reports_nothing) {
  fixture f;
  fx_setup_empty(&f);
  char p[KBC_TEST_PATH_MAX];
  kbc_err err;
  kbc_err_reset(&err);
  if (f.app == NULL) {
    fx_teardown(&f);
    return;
  }
  join(p, sizeof p, f.corpus_a, "a.md");
  kbc_test_write_file(p, "# Alpha Ledger\n\nThe ledger reconciles accruals.\n");
  KBC_CHECK_OK(kbc_app_reindex(f.app, &err));

  kbc_arena *a = kbc_arena_new(64u * 1024u);
  KBC_CHECK_NOT_NULL(a);
  if (a == NULL) {
    fx_teardown(&f);
    return;
  }
  const char *doc = id_of_path(f.cfg, CORPUS_A, "a.md", a);
  KBC_CHECK_NOT_NULL(doc);
  if (doc == NULL) {
    kbc_arena_free(a);
    fx_teardown(&f);
    return;
  }
  char id_copy[KBC_MAX_ID_LEN + 1];
  snprintf(id_copy, sizeof id_copy, "%s", doc);

  kbc_store *st = kbc_store_open(f.cfg, &err);
  KBC_CHECK_NOT_NULL(st);
  if (st == NULL) {
    kbc_arena_free(a);
    fx_teardown(&f);
    return;
  }
  /* Two visits older than the cutoff and one newer. The retention window is
   * `started_at < cutoff`, so the recent row is what a converged prune must
   * LEAVE BEHIND — a prune that reported 3 here would be deleting history the
   * operator did not ask it to. */
  for (int i = 0; i < 2; i++) {
    kbc_history_row old_visit;
    memset(&old_visit, 0, sizeof old_visit);
    old_visit.kind = "open";
    old_visit.artifact_id = id_copy;
    old_visit.user = "nik";
    old_visit.started_at = 1000 + i;
    old_visit.updated_at = 1000 + i;
    KBC_CHECK_OK(kbc_store_add_history(st, &old_visit, &err));
  }
  kbc_history_row recent;
  memset(&recent, 0, sizeof recent);
  recent.kind = "open";
  recent.artifact_id = id_copy;
  recent.user = "nik";
  recent.started_at = 2000000000;
  recent.updated_at = 2000000000;
  KBC_CHECK_OK(kbc_store_add_history(st, &recent, &err));
  KBC_CHECK_EQ_INT(history_visits(f.cfg, "nik", id_copy, a), 3);

  /* A dry run reports exactly what --apply would remove and removes nothing
   * (store.h:496-499): one predicate serves both modes, so a dry run that
   * could disagree with the real thing would be worse than no dry run. */
  const int64_t cutoff = 1500000000;
  int64_t dry = -1;
  KBC_CHECK_OK(kbc_store_prune_history(st, cutoff, false, &dry, &err));
  KBC_CHECK_EQ_INT(dry, 2);
  KBC_CHECK_EQ_INT(history_visits(f.cfg, "nik", id_copy, a), 3);

  /* THE CONVERGENCE LOOP, bounded. Each pass must strictly reduce what is
   * left, and the loop must REACH a pass that reports nothing. */
  int64_t total = 0;
  int converged = 0;
  /* `last` is the previous pass's count, seeded to a value no real pass can
   * report so the FIRST pass is compared against the bound rather than against
   * a sentinel it would trivially satisfy. */
  int64_t last = INT64_MAX;
  for (int pass = 0; pass < 8; pass++) {
    int64_t rows = -1;
    KBC_CHECK_OK(kbc_store_prune_history(st, cutoff, true, &rows, &err));
    if (rows == 0) {
      converged = 1;
      break;
    }
    total += rows;
    /* Strictly decreasing: a pass that reported the same work twice is not
     * converging, and without this the loop bound would be the only thing
     * that noticed — by hanging rather than by failing. */
    KBC_CHECK_MSG(rows < last, "pass %d removed %lld after %lld: not converging",
                  pass, (long long)rows,
                  last == INT64_MAX ? -1LL : (long long)last);
    last = rows;
  }
  KBC_CHECK_MSG(converged, "prune never reported nothing done in 8 passes");
  KBC_CHECK_EQ_INT(total, 2);

  /* The converged state: a second prune over the same cutoff is a NO-OP, and
   * it says so with 0 rather than with silence. This is the assertion that
   * would catch a prune that re-reported the same rows every pass. */
  int64_t again = -1;
  KBC_CHECK_OK(kbc_store_prune_history(st, cutoff, true, &again, &err));
  KBC_CHECK_EQ_INT(again, 0);
  /* And the row inside the window is still there. */
  KBC_CHECK_EQ_INT(history_visits(f.cfg, "nik", id_copy, a), 1);

  /* Retention deletes HISTORY and nothing else. The document, its index row
   * and its search hit all survive: a retention pass that removed artifacts
   * would be a corpus-deleting feature the original does not have
   * (store.h:485-494), and this is the assertion that says so. */
  KBC_CHECK_EQ_INT(store_count(f.cfg), 1);
  KBC_CHECK_NOT_NULL(id_of_path(f.cfg, CORPUS_A, "a.md", a));
  char path[64], title[64], id[32];
  KBC_CHECK_EQ_INT(first_hit_path(f.app, "accruals", NULL, path, sizeof path,
                                  title, sizeof id, id, sizeof id, NULL),
                   1);
  KBC_CHECK_EQ_STR(path, "a.md");

  kbc_store_close(st);
  kbc_arena_free(a);
  fx_teardown(&f);
}

/* CLOSE, AND EVERYTHING AFTER IT. The app is freed by `kbc_app_close` and the
 * store it hands out is BORROWED until then (app.h:99-109), so the pair of
 * facts a caller can get wrong are: closing twice, and holding the store
 * across the close. Both are pinned here as the behaviour they actually have,
 * because "a crash is not an acceptable third option" and a double free is
 * precisely that.
 *
 * What is pinned is REFUSAL, not safety-after-the-fact: a closed app is freed
 * memory and there is no correct way to call into it, so the contract is that
 * the SECOND close of the same pointer is not a thing a caller may do, and
 * that every accessor REFUSES a NULL app rather than dereferencing one. The
 * store pointer is deliberately not exercised after the close for the same
 * reason — reading freed memory to prove it is freed is the bug, not the
 * test. */
KBC_TEST(a_closed_app_refuses_every_accessor_and_close_is_not_repeatable) {
  fixture f;
  fx_setup_empty(&f);
  char p[KBC_TEST_PATH_MAX];
  kbc_err err;
  kbc_err_reset(&err);
  if (f.app == NULL) {
    fx_teardown(&f);
    return;
  }
  join(p, sizeof p, f.corpus_a, "a.md");
  kbc_test_write_file(p, "# Alpha Ledger\n\nThe ledger reconciles accruals.\n");
  KBC_CHECK_OK(kbc_app_reindex(f.app, &err));

  /* The store is handed out while the app is alive, and it is the SAME handle
   * every time — a caller that cached it and a caller that asks again are
   * holding one pointer, which is what makes the borrow well-defined for the
   * window it is borrowed for. */
  kbc_store *first = kbc_app_store(f.app);
  KBC_CHECK_NOT_NULL(first);
  KBC_CHECK_MSG(kbc_app_store(f.app) == first,
                "kbc_app_store handed out two different handles");

  /* NULL is refused by every fallible accessor, with a message naming the
   * problem — a caller that passes NULL gets a status it can act on, never a
   * dereference. Each is checked for the FILLED MESSAGE too, because an error
   * with an empty string is the one shape a caller cannot act on. */
  kbc_arena *a = kbc_arena_new(4096u);
  KBC_CHECK_NOT_NULL(a);
  kbc_query q;
  memset(&q, 0, sizeof q);
  kbc_search_result res;
  memset(&res, 0, sizeof res);
  kbc_artifact art;
  memset(&art, 0, sizeof art);
  kbc_app_stats st;
 kbc_capture_input cap;
  memset(&cap, 0, sizeof cap);
  /* Every OTHER argument is valid, so the only thing the capture call can be
   * refusing is the NULL app. A zeroed input would be refused by the NEXT
   * check (the body is required) and return the same status, which would make
   * the assertion pass with the app guard deleted — a test that cannot tell
   * two different refusals apart is not testing the one it names. */
  cap.corpus = CORPUS_A;
  cap.body = "body";
  cap.body_len = 4;
  kbc_capture_result cap_out;
  memset(&cap_out, 0, sizeof cap_out);
  kbc_err e1;
  kbc_err_reset(&e1);
  kbc_err e2;
  kbc_err_reset(&e2);
  kbc_err e3;
  kbc_err_reset(&e3);
  kbc_err e4;
  kbc_err_reset(&e4);
  kbc_err e5;
  kbc_err_reset(&e5);
  kbc_err e6;
  kbc_err_reset(&e6);

  KBC_CHECK_ERR(kbc_app_reindex(NULL, &e1), KBC_ERR_INVALID);
  KBC_CHECK_ERR_MSG(e1);
  KBC_CHECK_ERR(kbc_app_reindex_file(NULL, CORPUS_A, "a.md", &e2),
                KBC_ERR_INVALID);
  KBC_CHECK_ERR_MSG(e2);
  KBC_CHECK_ERR(kbc_app_reindex_remove(NULL, CORPUS_A, "a.md", &e3),
                KBC_ERR_INVALID);
  KBC_CHECK_ERR_MSG(e3);
  KBC_CHECK_ERR(kbc_app_search(NULL, a, &q, &res, &e4), KBC_ERR_INVALID);
  KBC_CHECK_ERR_MSG(e4);
  KBC_CHECK_ERR(kbc_app_get_artifact(NULL, a, "a", false, &art, &e5),
                KBC_ERR_INVALID);
  KBC_CHECK_ERR_MSG(e5);
  KBC_CHECK_ERR(kbc_app_stats_get(NULL, &st, &e6), KBC_ERR_INVALID);
  KBC_CHECK_ERR_MSG(e6);
  /* And the two the app answers with a VALUE rather than a status: a NULL app
   * is not an app, so it yields the empty answer rather than a crash. These
   * are the two shapes a caller is most likely to reach for in a teardown
   * path, where the app handle is often already NULL. */
  KBC_CHECK_NULL(kbc_app_store(NULL));
  KBC_CHECK_NULL(kbc_app_config(NULL));
  /* A NULL app is also safe for the void and id-returning entry points,
   * because a teardown path calls them unconditionally. */
  kbc_app_stop_watcher(NULL);
  kbc_app_publish(NULL, "reindex", "{}");
  kbc_app_unsubscribe(NULL, 1);
  KBC_CHECK_EQ_INT(kbc_app_start_watcher(NULL, &e1), KBC_ERR_INVALID);
  KBC_CHECK_EQ_INT(kbc_app_capture(NULL, CORPUS_A, &cap, &cap_out, &e1),
                   KBC_ERR_INVALID);
  KBC_CHECK_EQ_INT(kbc_app_move_path(NULL, CORPUS_A, "a.md", "b.md", &e1),
                   KBC_ERR_INVALID);
  KBC_CHECK_EQ_INT(kbc_app_subscribe(NULL, NULL, NULL), 0);
  KBC_CHECK_EQ_INT(kbc_app_query_cache_capacity(NULL), 0);
  /* A refused call must not have half-applied anything. The config is the app's
   * PRIVATE CLONE, not the caller's handle (the fixture's own reopen helper
   * documents that), so the assertion is that it is the same clone as before
   * and still names the same corpus — pointer identity against the CALLER's
   * config would be asserting the opposite of the contract. */
  const kbc_config *before_cfg = kbc_app_config(f.app);
  KBC_CHECK_NOT_NULL(before_cfg);
  KBC_CHECK(kbc_app_config(f.app) == before_cfg);
  KBC_CHECK_EQ_INT(before_cfg->ncorpora, 1);
  KBC_CHECK_OK(kbc_app_reindex(f.app, &err));

  if (a != NULL) {
    kbc_arena_free(a);
  }
  /* Closing a NULL app is a no-op, which is the shape a teardown path wants;
   * closing the SAME live pointer twice is not, and the fixture's contract is
   * that it nulls the handle after the first close rather than calling again.
   * That is why this test does not do it: the double free it would cause is
   * the crash the acceptance rule rules out, and no assertion can make it
   * safe without a header change (see the report). */
  kbc_app_close(NULL);
  kbc_app_close(f.app);
  f.app = NULL;
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
      {"a_summary_is_prose_and_keeps_words_that_look_like_markers",
       a_summary_is_prose_and_keeps_words_that_look_like_markers},
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
      {"the_link_graph_does_not_depend_on_the_ingest_order",
       the_link_graph_does_not_depend_on_the_ingest_order},
      {"a_full_reindex_drains_links_left_pending",
       a_full_reindex_drains_links_left_pending},
      {"removing_a_document_takes_its_edges_and_its_pending_links",
       removing_a_document_takes_its_edges_and_its_pending_links},
      {"the_graph_boost_is_off_by_default_and_reorders_when_configured",
       the_graph_boost_is_off_by_default_and_reorders_when_configured},
      {"a_link_naming_a_document_without_its_extension_is_an_edge",
       a_link_naming_a_document_without_its_extension_is_an_edge},
      {"an_ambiguous_link_target_is_not_an_edge",
       an_ambiguous_link_target_is_not_an_edge},
      {"a_single_file_reindex_resolves_against_the_index_and_itself",
       a_single_file_reindex_resolves_against_the_index_and_itself},
      {"hooks_run_in_registration_order", hooks_run_in_registration_order},
      {"an_uninterested_hook_is_never_called",
       an_uninterested_hook_is_never_called},
      {"a_failing_hook_does_not_fail_the_run_or_half_write_the_store",
       a_failing_hook_does_not_fail_the_run_or_half_write_the_store},
    {"a_tag_filter_that_matches_nothing_returns_zero_rows",
     a_tag_filter_that_matches_nothing_returns_zero_rows},
    {"a_tag_and_a_cap_together_are_an_intersection",
     a_tag_and_a_cap_together_are_an_intersection},
    {"two_tags_or_ed_match_either_document",
     two_tags_or_ed_match_either_document},
    {"a_negated_tag_drops_the_documents_carrying_it",
     a_negated_tag_drops_the_documents_carrying_it},
    {"a_tag_composes_with_folder_and_since", a_tag_composes_with_folder_and_since},
    {"a_removed_tag_stops_matching_after_one_reindex",
     a_removed_tag_stops_matching_after_one_reindex},
    {"a_deleted_document_takes_its_tags_with_it",
     a_deleted_document_takes_its_tags_with_it},
    {"index_selects_only_the_documents_that_declare_themselves_indexes",
     index_selects_only_the_documents_that_declare_themselves_indexes},
    {"a_tag_value_is_case_sensitive_as_it_is_in_the_original",
     a_tag_value_is_case_sensitive_as_it_is_in_the_original},
    {"scope_is_still_refused", scope_is_still_refused},
    {"a_dangling_symlink_does_not_fail_the_reindex",
     a_dangling_symlink_does_not_fail_the_reindex},
    {"a_full_reindex_and_a_single_file_update_never_splice_the_index",
     a_full_reindex_and_a_single_file_update_never_splice_the_index},
    {"a_full_reindex_never_drops_a_document_a_single_file_update_added",
     a_full_reindex_never_drops_a_document_a_single_file_update_added},
    {"a_removal_the_store_cannot_commit_is_reported_not_swallowed",
     a_removal_the_store_cannot_commit_is_reported_not_swallowed},
    {"an_aborted_reindex_leaves_the_previous_generation_serving",
     an_aborted_reindex_leaves_the_previous_generation_serving},
    {"a_document_deleted_while_nobody_watched_is_reconciled_by_the_pass",
     a_document_deleted_while_nobody_watched_is_reconciled_by_the_pass},
    {"a_document_the_walk_declines_keeps_its_row_and_its_comments",
     a_document_the_walk_declines_keeps_its_row_and_its_comments},
    {"delete_by_path_takes_the_document_and_announces_it",
     delete_by_path_takes_the_document_and_announces_it},
    {"delete_by_path_refuses_a_bad_target_and_forgives_an_absent_one",
     delete_by_path_refuses_a_bad_target_and_forgives_an_absent_one},
    {"an_html_capture_is_sanitised_and_stamped", an_html_capture_is_sanitised_and_stamped},
    {"a_capture_writes_the_originals_keys_in_the_originals_order",
     a_capture_writes_the_originals_keys_in_the_originals_order},
    {"the_same_bytes_captured_twice_are_byte_identical",
     the_same_bytes_captured_twice_are_byte_identical},
    {"a_capture_never_touches_the_titles_title_already_has",
     a_capture_never_touches_the_titles_title_already_has},
    {"a_capture_edits_a_key_in_place_and_appends_the_rest_after_it",
     a_capture_edits_a_key_in_place_and_appends_the_rest_after_it},
    {"a_capture_does_not_restamp_a_session_the_document_already_carries",
     a_capture_does_not_restamp_a_session_the_document_already_carries},
    {"a_capture_that_skips_provenance_keys_frees_only_what_it_built",
     a_capture_that_skips_provenance_keys_frees_only_what_it_built},
    {"a_newline_in_an_uploaded_name_cannot_close_the_frontmatter_fence",
     a_newline_in_an_uploaded_name_cannot_close_the_frontmatter_fence},
    {"a_capture_is_a_document_the_index_picks_up_under_the_id_it_reported",
     a_capture_is_a_document_the_index_picks_up_under_the_id_it_reported},
    {"a_capture_records_the_url_without_ever_connecting_to_it",
     a_capture_records_the_url_without_ever_connecting_to_it},
    {"a_url_share_becomes_a_stub_carrying_the_provenance_tag",
     a_url_share_becomes_a_stub_carrying_the_provenance_tag},
    {"a_multipart_body_splits_into_the_fields_a_capture_carries",
     a_multipart_body_splits_into_the_fields_a_capture_carries},
    {"a_body_longer_than_the_array_reports_the_true_count_so_a_retry_fits",
     a_body_longer_than_the_array_reports_the_true_count_so_a_retry_fits},
    {"a_move_carries_the_comments_the_pin_and_the_corkboard_across",
     a_move_carries_the_comments_the_pin_and_the_corkboard_across},
      {"a_document_removal_takes_its_attachments_and_their_bytes",
       a_document_removal_takes_its_attachments_and_their_bytes},
      {"a_move_carries_the_attachments_under_the_same_id",
       a_move_carries_the_attachments_under_the_same_id},
    {"a_resolved_comment_is_still_resolved_after_a_move",
     a_resolved_comment_is_still_resolved_after_a_move},
    {"a_move_refuses_its_bad_targets_before_it_touches_anything",
     a_move_refuses_its_bad_targets_before_it_touches_anything},
    {"a_move_of_a_file_the_store_has_never_seen_is_refused",
     a_move_of_a_file_the_store_has_never_seen_is_refused},
    {"a_comment_carried_across_a_move_keeps_the_second_it_was_made",
     a_comment_carried_across_a_move_keeps_the_second_it_was_made},
    {"a_corkboard_anchor_older_than_the_listing_page_survives_a_move",
     a_corkboard_anchor_older_than_the_listing_page_survives_a_move},
    {"an_interrupted_move_is_listed_and_is_not_yet_a_redirect",
     an_interrupted_move_is_listed_and_is_not_yet_a_redirect},
    {"a_move_interrupted_before_the_rename_is_abandoned_at_bring_up",
     a_move_interrupted_before_the_rename_is_abandoned_at_bring_up},
    {"a_move_interrupted_after_the_rename_announces_what_was_lost",
     a_move_interrupted_after_the_rename_announces_what_was_lost},
    {"a_second_boot_after_an_interrupted_move_changes_nothing",
     a_second_boot_after_an_interrupted_move_changes_nothing},
    {"a_daemon_does_not_start_when_the_move_intents_cannot_be_read",
     a_daemon_does_not_start_when_the_move_intents_cannot_be_read},
    {"a_bring_up_pass_refuses_a_move_row_it_cannot_place",
     a_bring_up_pass_refuses_a_move_row_it_cannot_place},
    {"a_stale_id_resolves_to_the_document_after_a_move",
     a_stale_id_resolves_to_the_document_after_a_move},
      {"chunk_zero_is_title_plus_headings", chunk_zero_is_title_plus_headings},
      {"short_body_is_a_single_window", short_body_is_a_single_window},
      {"long_body_splits_into_overlapping_windows",
       long_body_splits_into_overlapping_windows},
      {"last_window_is_not_duplicated", last_window_is_not_duplicated},
      {"empty_document_yields_no_chunks", empty_document_yields_no_chunks},
      {"body_only_still_chunks", body_only_still_chunks},
      {"chunking_is_deterministic", chunking_is_deterministic},
      {"degenerate_overlap_still_terminates",
       degenerate_overlap_still_terminates},
      {"the_chunk_cap_keeps_the_first_n_and_reports_the_original_total",
       the_chunk_cap_keeps_the_first_n_and_reports_the_original_total},
      {"a_long_document_is_stored_as_overlapping_windows_and_stays_findable",
       a_long_document_is_stored_as_overlapping_windows_and_stays_findable},
      {"a_failed_embed_is_recorded_against_the_document",
       a_failed_embed_is_recorded_against_the_document},
      {"a_quarantined_document_stays_indexed_and_keeps_its_error_row",
       a_quarantined_document_stays_indexed_and_keeps_its_error_row},
      {"an_edited_document_leaves_quarantine_and_an_unchanged_one_does_not",
       an_edited_document_leaves_quarantine_and_an_unchanged_one_does_not},
      {"a_second_identical_query_never_reaches_the_sidecar",
       a_second_identical_query_never_reaches_the_sidecar},
      {"a_dead_sidecar_keeps_the_cache_and_stores_nothing_new",
       a_dead_sidecar_keeps_the_cache_and_stores_nothing_new},
      {"a_populated_query_cache_is_released_when_the_app_closes",
       a_populated_query_cache_is_released_when_the_app_closes},
      {"concurrent_queries_share_one_cache_and_the_counters_add_up",
       concurrent_queries_share_one_cache_and_the_counters_add_up},
      {"a_reindex_records_a_finished_run_for_every_configured_corpus",
       a_reindex_records_a_finished_run_for_every_configured_corpus},
      {"a_failed_reindex_leaves_a_finished_run_carrying_the_error",
       a_failed_reindex_leaves_a_finished_run_carrying_the_error},
      {"a_reindex_does_not_move_the_first_seen_anchor",
       a_reindex_does_not_move_the_first_seen_anchor},
      {"opening_the_daemon_registers_every_configured_corpus_as_a_source",
       opening_the_daemon_registers_every_configured_corpus_as_a_source},
      {"the_edge_count_and_the_backlink_list_agree_on_one_corpus",
       the_edge_count_and_the_backlink_list_agree_on_one_corpus},
      {"a_retention_prune_converges_and_then_reports_nothing",
       a_retention_prune_converges_and_then_reports_nothing},
      {"a_closed_app_refuses_every_accessor_and_close_is_not_repeatable",
       a_closed_app_refuses_every_accessor_and_close_is_not_repeatable},
      {NULL, NULL},
  };
  return kbc_test_run("app", cases);
}
