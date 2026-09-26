/* test_search.c — the two lanes, their fusion, the filters, and the resolver. */
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "kbc_test.h"

#include "kbc/search.h"
/* Five docs, one searchable token each beyond the shared "search rank fusion"
 * stem, so every doc scores identically on a bare "search" query and the
 * tie-break (doc id ascending) is the only thing ordering them.
 *
 *   0 kb/notes/alpha.md   NOTE
 *   1 kb/notes/beta.md    NOTE
 *   2 other/docs/gamma.md ARTIFACT
 *   3 kb/docs/delta.md    ARTIFACT
 *   4 other/notes/eps.md  MEMORY
 */
#define DOC_COUNT 5
#define DIM 2

typedef struct {
  const char *corpus;
  const char *path;
  const char *title;
  kbc_kind kind;
  const char *text;
} doc_spec;

static const doc_spec DOCS[DOC_COUNT] = {
    {"kb", "notes/alpha.md", "Alpha", KBC_KIND_NOTE, "search rank fusion alpha"},
    {"kb", "notes/beta.md", "Beta", KBC_KIND_NOTE, "search rank fusion beta"},
    {"other", "docs/gamma.md", "Gamma", KBC_KIND_ARTIFACT,
     "search rank fusion gamma"},
    {"kb", "docs/delta.md", "Delta", KBC_KIND_ARTIFACT,
     "search rank fusion delta"},
    {"other", "notes/epsilon.md", "Epsilon", KBC_KIND_MEMORY,
     "search rank fusion epsilon"},
};

static kbc_index *build_index(kbc_arena *a, kbc_err *err) {
  kbc_index *ix = kbc_index_new();
  if (ix == NULL) {
    kbc_test_fail(__FILE__, __LINE__, "kbc_index_new returned NULL");
    return NULL;
  }
  if (kbc_index_begin_build(ix, err) != KBC_OK) return ix;
  for (uint32_t i = 0; i < DOC_COUNT; i++) {
    /* kbc_tokenize grows the array into `a`, so a zeroed vector needs no
     * init/free of its own. */
    kbc_tokens toks;
    memset(&toks, 0, sizeof(toks));
    const char *text = DOCS[i].text;
    if (kbc_tokenize(a, text, strlen(text), &toks, err) != KBC_OK) return ix;
    if (kbc_index_add_doc(ix, i, DOCS[i].corpus, DOCS[i].path, DOCS[i].title,
                          DOCS[i].kind, &toks, err) != KBC_OK) {
      return ix;
    }
  }
  if (kbc_index_end_build(ix, err) != KBC_OK) return ix;
  return ix;
}

/* Angles chosen so the vector lane ranks the docs 4,0,1,2,3 — the exact
 * reverse-ish of the keyword lane's 0,1,2,3,4 — so a fused ordering that
 * differs from the keyword ordering cannot be coincidence. */
static kbc_vecstore *build_vecstore(kbc_err *err) {
  kbc_vecstore *vs = kbc_vecstore_new(DIM, 8u, err);
  if (vs == NULL) return NULL;
  const double angle[DOC_COUNT] = {0.1, 0.2, 0.3, 0.4, 0.0};
  for (uint32_t i = 0; i < DOC_COUNT; i++) {
    const float v[DIM] = {(float)cos(angle[i]), (float)sin(angle[i])};
    if (kbc_vecstore_set(vs, i, v, err) != KBC_OK) return vs;
  }
  return vs;
}

static kbc_query query(kbc_search_mode mode, const char *text) {
  kbc_query q;
  memset(&q, 0, sizeof(q));
  q.q = text;
  q.mode = mode;
  q.kind = KBC_KIND__COUNT;
  return q;
}

/* ------------------------------------------------------------- resolver -- */

typedef enum {
  RESOLVE_ALL,
  RESOLVE_DROP_DOC2, /* the store lost doc 2 to a reindex */
  RESOLVE_FAIL_DOC1  /* the store itself is broken */
} resolve_mode;

static kbc_status resolver(void *ctx, kbc_arena *a, uint32_t doc_id,
                           const char **artifact_id, const char **summary) {
  /* A NULL ctx means "resolve everything": only the resolver needs a
   * context, not the searcher. */
  const resolve_mode m =
      ctx != NULL ? *(const resolve_mode *)ctx : RESOLVE_ALL;
  if (m == RESOLVE_DROP_DOC2 && doc_id == 2)
    return kbc_err_set(NULL, KBC_ERR_NOTFOUND, "doc %u gone", doc_id);
  if (m == RESOLVE_FAIL_DOC1 && doc_id == 1)
    return kbc_err_set(NULL, KBC_ERR_IO, "sqlite: db is locked");
  char buf[KBC_MAX_ID_LEN + 1];
  snprintf(buf, sizeof(buf), "%012x", 0xa0000000u + doc_id);
  *artifact_id = kbc_arena_strdup(a, buf);
  *summary = kbc_arena_strdup(a, "summary text");
  return KBC_OK;
}

static bool doc_list_is(const kbc_search_result *r, const uint32_t *want,
                        size_t n) {
  if (r->len != n) return false;
  for (size_t i = 0; i < n; i++) {
    if (r->rows[i].doc_id != want[i]) return false;
  }
  return true;
}

/* ----------------------------------------------------------- fusion maths */

KBC_TEST(rrf_score_of_a_doc_first_in_both_lanes_is_two_over_rrf_k) {
  kbc_err err;
  kbc_err_reset(&err);
  kbc_arena *a = kbc_arena_new(4096);
  kbc_index *ix = build_index(a, &err);
  KBC_CHECK_OK(err.status);
  kbc_vecstore *vs = build_vecstore(&err);
  KBC_CHECK_OK(err.status);

  kbc_searcher *s = kbc_searcher_new(ix, resolver, NULL, &err);
  KBC_CHECK_NOT_NULL(s);
  KBC_CHECK_OK(err.status);
  KBC_CHECK_OK(kbc_searcher_set_vecstore(s, vs, &err));

  /* "epsilon" is a token only doc 4 carries, so it is rank 0 in the keyword
   * lane; its vector is (1,0), exactly the query vector, so it is also rank 0
   * in the vector lane. Fused score must be 1/60 + 1/60. */
  kbc_query q = query(KBC_MODE_HYBRID, "epsilon");
  q.rrf_k = 60;
  const float vec[DIM] = {1.0f, 0.0f};
  kbc_search_result r;
  KBC_CHECK_OK(kbc_search_run(s, a, &q, vec, DIM, &r, &err));
  KBC_CHECK_EQ_INT(r.len, 1);
  KBC_CHECK_EQ_INT(r.rows[0].doc_id, 4);
  KBC_CHECK_EQ_INT(r.rows[0].keyword_rank, 0);
  KBC_CHECK_EQ_INT(r.rows[0].vector_rank, 0);
  KBC_CHECK(r.vector_ran);
  /* ×1.5 on top: doc 4's title IS "Epsilon", and fusion.rs:155 boosts a
   * title match by (1 + TITLE_BOOST) after fusion. Undo the factor to read
   * the RRF constant itself, so this still pins k=60 rather than pinning
   * the boost twice. */
  KBC_CHECK_EQ_DBL(r.rows[0].score * (2.0 / 3.0), 2.0 / 60.0, 1e-9);
  KBC_CHECK_EQ_DBL(r.rows[0].vector_score, 1.0, 1e-6);

  /* A non-default rrf_k must move the score, or the constant is hard-wired. */
  kbc_query q2 = query(KBC_MODE_HYBRID, "epsilon");
  q2.rrf_k = 10;
  kbc_search_result r2;
  KBC_CHECK_OK(kbc_search_run(s, a, &q2, vec, DIM, &r2, &err));
  KBC_CHECK_EQ_INT(r2.len, 1);
  KBC_CHECK_EQ_DBL(r2.rows[0].score * (2.0 / 3.0), 2.0 / 10.0, 1e-9);

  kbc_searcher_free(s);
  kbc_vecstore_free(vs);
  kbc_index_free(ix);
  kbc_arena_free(a);
}

/* ----------------------------------------------------------------- modes -- */

KBC_TEST(keyword_mode_runs_only_the_keyword_lane) {
  kbc_err err;
  kbc_err_reset(&err);
  kbc_arena *a = kbc_arena_new(4096);
  kbc_index *ix = build_index(a, &err);
  kbc_vecstore *vs = build_vecstore(&err);
  kbc_searcher *s = kbc_searcher_new(ix, resolver, NULL, &err);
  KBC_CHECK_OK(kbc_searcher_set_vecstore(s, vs, &err));

  /* Even with a vector store and a query vector present, keyword mode must
   * ignore the vector lane — and must not call that a degradation. */
  kbc_query q = query(KBC_MODE_KEYWORD, "search");
  const float vec[DIM] = {1.0f, 0.0f};
  kbc_search_result r;
  KBC_CHECK_OK(kbc_search_run(s, a, &q, vec, DIM, &r, &err));
  KBC_CHECK(!r.vector_ran);
  KBC_CHECK(!r.degraded);
  KBC_CHECK_EQ_INT(r.candidates, DOC_COUNT);
  for (size_t i = 0; i < r.len; i++) {
    KBC_CHECK_EQ_INT(r.rows[i].vector_rank, UINT32_MAX);
    KBC_CHECK_EQ_DBL(r.rows[i].vector_score, 0.0, 1e-12);
  }

  kbc_searcher_free(s);
  kbc_vecstore_free(vs);
  kbc_index_free(ix);
  kbc_arena_free(a);
}

KBC_TEST(hybrid_without_a_vector_store_degrades_to_keyword) {
  kbc_err err;
  kbc_err_reset(&err);
  kbc_arena *a = kbc_arena_new(4096);
  kbc_index *ix = build_index(a, &err);
  kbc_searcher *s = kbc_searcher_new(ix, resolver, NULL, &err);
  KBC_CHECK_OK(kbc_searcher_set_vecstore(s, NULL, &err));

  kbc_query q = query(KBC_MODE_HYBRID, "search");
  const float vec[DIM] = {1.0f, 0.0f};
  kbc_search_result r;
  KBC_CHECK_OK(kbc_search_run(s, a, &q, vec, DIM, &r, &err));
  KBC_CHECK(!r.vector_ran);
  KBC_CHECK(r.degraded);
  /* Degraded must not mean empty: the keyword lane still answers. */
  const uint32_t want[DOC_COUNT] = {0, 1, 2, 3, 4};
  KBC_CHECK(doc_list_is(&r, want, DOC_COUNT));

  kbc_searcher_free(s);
  kbc_index_free(ix);
  kbc_arena_free(a);
}

KBC_TEST(semantic_without_a_query_vector_is_an_empty_ok_not_an_error) {
  kbc_err err;
  kbc_err_reset(&err);
  kbc_arena *a = kbc_arena_new(4096);
  kbc_index *ix = build_index(a, &err);
  kbc_vecstore *vs = build_vecstore(&err);
  kbc_searcher *s = kbc_searcher_new(ix, resolver, NULL, &err);
  KBC_CHECK_OK(kbc_searcher_set_vecstore(s, vs, &err));

  kbc_query q = query(KBC_MODE_SEMANTIC, "search");
  kbc_search_result r;
  KBC_CHECK_OK(kbc_search_run(s, a, &q, NULL, 0, &r, &err));
  KBC_CHECK(!r.vector_ran);
  KBC_CHECK(r.degraded);
  KBC_CHECK_EQ_INT(r.len, 0);
  KBC_CHECK_EQ_INT(r.candidates, 0);

  kbc_searcher_free(s);
  kbc_vecstore_free(vs);
  kbc_index_free(ix);
  kbc_arena_free(a);
}

KBC_TEST(stopword_only_query_returns_nothing_rather_than_everything) {
  kbc_err err;
  kbc_err_reset(&err);
  kbc_arena *a = kbc_arena_new(4096);
  kbc_index *ix = build_index(a, &err);
  kbc_searcher *s = kbc_searcher_new(ix, resolver, NULL, &err);

  kbc_query q = query(KBC_MODE_HYBRID, "the and of");
  kbc_search_result r;
  KBC_CHECK_OK(kbc_search_run(s, a, &q, NULL, 0, &r, &err));
  KBC_CHECK_EQ_INT(r.len, 0);
  KBC_CHECK_EQ_INT(r.candidates, 0);
  KBC_CHECK(r.degraded);

  /* In keyword mode the vector lane was never asked for, so no degradation
   * is reported — the zero rows come from the query, not from a missing lane. */
  kbc_query q2 = query(KBC_MODE_KEYWORD, "the and of");
  kbc_search_result r2;
  KBC_CHECK_OK(kbc_search_run(s, a, &q2, NULL, 0, &r2, &err));
  KBC_CHECK_EQ_INT(r2.len, 0);
  KBC_CHECK(!r2.degraded);

  kbc_searcher_free(s);
  kbc_index_free(ix);
  kbc_arena_free(a);
}

/* -------------------------------------------------------------- resolver -- */

KBC_TEST(resolver_notfound_drops_the_row_and_the_search_still_succeeds) {
  kbc_err err;
  kbc_err_reset(&err);
  kbc_arena *a = kbc_arena_new(4096);
  kbc_index *ix = build_index(a, &err);
  resolve_mode m = RESOLVE_DROP_DOC2;
  kbc_searcher *s = kbc_searcher_new(ix, resolver, &m, &err);

  kbc_query q = query(KBC_MODE_KEYWORD, "search");
  kbc_search_result r;
  KBC_CHECK_OK(kbc_search_run(s, a, &q, NULL, 0, &r, &err));
  /* Doc 2 was ranked 3rd of 5; it disappears and the rest keep their order. */
  const uint32_t want[4] = {0, 1, 3, 4};
  KBC_CHECK(doc_list_is(&r, want, 4));
  /* A dropped row is still a candidate — candidates are the lanes' output. */
  KBC_CHECK_EQ_INT(r.candidates, DOC_COUNT);

  kbc_searcher_free(s);
  kbc_index_free(ix);
  kbc_arena_free(a);
}

KBC_TEST(resolver_error_fails_the_whole_search) {
  kbc_err err;
  kbc_err_reset(&err);
  kbc_arena *a = kbc_arena_new(4096);
  kbc_index *ix = build_index(a, &err);
  resolve_mode m = RESOLVE_FAIL_DOC1;
  kbc_searcher *s = kbc_searcher_new(ix, resolver, &m, &err);

  kbc_query q = query(KBC_MODE_KEYWORD, "search");
  kbc_search_result r;
  /* A broken store is not a missing row: the search must report the failure
   * rather than silently answer with the other four docs. */
  KBC_CHECK_ERR(kbc_search_run(s, a, &q, NULL, 0, &r, &err), KBC_ERR_IO);

  kbc_searcher_free(s);
  kbc_index_free(ix);
  kbc_arena_free(a);
}

KBC_TEST(searcher_without_a_resolver_leaves_artifact_id_null) {
  kbc_err err;
  kbc_err_reset(&err);
  kbc_arena *a = kbc_arena_new(4096);
  kbc_index *ix = build_index(a, &err);
  kbc_searcher *s = kbc_searcher_new(ix, NULL, NULL, &err);
  KBC_CHECK_NOT_NULL(s);
  KBC_CHECK_OK(err.status);

  kbc_query q = query(KBC_MODE_KEYWORD, "search");
  kbc_search_result r;
  KBC_CHECK_OK(kbc_search_run(s, a, &q, NULL, 0, &r, &err));
  KBC_CHECK_EQ_INT(r.len, DOC_COUNT);
  for (size_t i = 0; i < r.len; i++) {
    KBC_CHECK_NULL(r.rows[i].artifact_id);
    KBC_CHECK_NULL(r.rows[i].summary);
  }

  kbc_searcher_free(s);
  kbc_index_free(ix);
  kbc_arena_free(a);
}

/* ------------------------------------------------------- rows and order -- */

KBC_TEST(rows_carry_index_metadata_and_sort_by_fused_score_then_doc_id) {
  kbc_err err;
  kbc_err_reset(&err);
  kbc_arena *a = kbc_arena_new(4096);
  kbc_index *ix = build_index(a, &err);
  kbc_vecstore *vs = build_vecstore(&err);
  resolve_mode m = RESOLVE_ALL;
  kbc_searcher *s = kbc_searcher_new(ix, resolver, &m, &err);
  KBC_CHECK_OK(kbc_searcher_set_vecstore(s, vs, &err));

  /* Keyword alone: every doc scores the same, so the tie-break alone orders
   * them, ascending by doc id. */
  kbc_query kw = query(KBC_MODE_KEYWORD, "search");
  kbc_search_result rkw;
  KBC_CHECK_OK(kbc_search_run(s, a, &kw, NULL, 0, &rkw, &err));
  const uint32_t kw_want[DOC_COUNT] = {0, 1, 2, 3, 4};
  KBC_CHECK(doc_list_is(&rkw, kw_want, DOC_COUNT));

  /* Hybrid: the vector lane ranks 4,0,1,2,3, so fusion must produce an order
   * that is neither the keyword order nor the vector order. */
  kbc_query hy = query(KBC_MODE_HYBRID, "search");
  const float vec[DIM] = {1.0f, 0.0f};
  kbc_search_result rhy;
  KBC_CHECK_OK(kbc_search_run(s, a, &hy, vec, DIM, &rhy, &err));
  KBC_CHECK(rhy.vector_ran);
  const uint32_t hy_want[DOC_COUNT] = {0, 1, 4, 2, 3};
  KBC_CHECK(doc_list_is(&rhy, hy_want, DOC_COUNT));
  for (size_t i = 1; i < rhy.len; i++) {
    KBC_CHECK(rhy.rows[i - 1].score > rhy.rows[i].score);
  }

  /* Metadata travels with the row, taken from the index doc. */
  for (size_t i = 0; i < rhy.len; i++) {
    const uint32_t d = rhy.rows[i].doc_id;
    KBC_CHECK_EQ_STR(rhy.rows[i].corpus, DOCS[d].corpus);
    KBC_CHECK_EQ_STR(rhy.rows[i].path, DOCS[d].path);
    KBC_CHECK_EQ_STR(rhy.rows[i].title, DOCS[d].title);
  }
  KBC_CHECK_EQ_INT(strlen(rhy.rows[0].artifact_id), KBC_MAX_ID_LEN);
  KBC_CHECK_EQ_STR(rhy.rows[0].artifact_id, "0000a0000000");

  kbc_searcher_free(s);
  kbc_vecstore_free(vs);
  kbc_index_free(ix);
  kbc_arena_free(a);
}

/* -------------------------------------------------------------- filters -- */

static void check_filter(kbc_index *ix, kbc_arena *a, kbc_query q,
                         const uint32_t *want, size_t n) {
  kbc_err err;
  kbc_err_reset(&err);
  kbc_searcher *s = kbc_searcher_new(ix, resolver, NULL, &err);
  kbc_search_result r;
  KBC_CHECK_OK(kbc_search_run(s, a, &q, NULL, 0, &r, &err));
  KBC_CHECK(doc_list_is(&r, want, n));
  kbc_searcher_free(s);
}

KBC_TEST(corpus_path_and_kind_filters_each_exclude_non_matching_docs) {
  kbc_err err;
  kbc_err_reset(&err);
  kbc_arena *a = kbc_arena_new(4096);
  kbc_index *ix = build_index(a, &err);

  kbc_query by_corpus = query(KBC_MODE_KEYWORD, "search");
  by_corpus.corpus = "kb";
  const uint32_t corpus_want[3] = {0, 1, 3};
  check_filter(ix, a, by_corpus, corpus_want, 3);

  kbc_query by_prefix = query(KBC_MODE_KEYWORD, "search");
  by_prefix.path_prefix = "docs/";
  const uint32_t prefix_want[2] = {2, 3};
  check_filter(ix, a, by_prefix, prefix_want, 2);

  /* A prefix shorter than a directory name is a plain strncmp: "note"
   * selects every path starting with those four letters. */
  kbc_query partial = query(KBC_MODE_KEYWORD, "search");
  partial.path_prefix = "note";
  const uint32_t partial_want[3] = {0, 1, 4};
  check_filter(ix, a, partial, partial_want, 3);

  kbc_query by_kind = query(KBC_MODE_KEYWORD, "search");
  by_kind.kind = KBC_KIND_ARTIFACT;
  check_filter(ix, a, by_kind, prefix_want, 2);

  kbc_index_free(ix);
  kbc_arena_free(a);
}

KBC_TEST(a_filter_that_matches_nothing_returns_zero_rows) {
  kbc_err err;
  kbc_err_reset(&err);
  kbc_arena *a = kbc_arena_new(4096);
  kbc_index *ix = build_index(a, &err);
  kbc_searcher *s = kbc_searcher_new(ix, resolver, NULL, &err);

  kbc_query q = query(KBC_MODE_KEYWORD, "search");
  q.corpus = "no-such-corpus";
  kbc_search_result r;
  KBC_CHECK_OK(kbc_search_run(s, a, &q, NULL, 0, &r, &err));
  KBC_CHECK_EQ_INT(r.len, 0);
  KBC_CHECK_EQ_INT(r.candidates, 0);

  /* The same filter with a term nothing holds. */
  kbc_query q2 = query(KBC_MODE_KEYWORD, "epsilon");
  q2.path_prefix = "docs/";
  kbc_search_result r2;
  KBC_CHECK_OK(kbc_search_run(s, a, &q2, NULL, 0, &r2, &err));
  KBC_CHECK_EQ_INT(r2.len, 0);
  KBC_CHECK_EQ_INT(r2.candidates, 0);

  kbc_searcher_free(s);
  kbc_index_free(ix);
  kbc_arena_free(a);
}

/* ---------------------------------------------------------------- limit -- */

KBC_TEST(limit_is_clamped_and_never_exceeds_the_corpus) {
  kbc_err err;
  kbc_err_reset(&err);
  kbc_arena *a = kbc_arena_new(4096);
  kbc_index *ix = build_index(a, &err);
  kbc_searcher *s = kbc_searcher_new(ix, resolver, NULL, &err);

  /* SIZE_MAX is clamped to KBC_MAX_HITS, and the result is still bounded by
   * the number of documents that actually matched. */
  kbc_query q = query(KBC_MODE_KEYWORD, "search");
  q.limit = SIZE_MAX;
  kbc_search_result r;
  KBC_CHECK_OK(kbc_search_run(s, a, &q, NULL, 0, &r, &err));
  KBC_CHECK_EQ_INT(r.len, DOC_COUNT);
  KBC_CHECK(r.len <= KBC_MAX_HITS);

  q.limit = 2;
  kbc_search_result r2;
  KBC_CHECK_OK(kbc_search_run(s, a, &q, NULL, 0, &r2, &err));
  KBC_CHECK_EQ_INT(r2.len, 2);
  KBC_CHECK_EQ_INT(r2.rows[0].doc_id, 0);
  KBC_CHECK_EQ_INT(r2.rows[1].doc_id, 1);

  /* limit 0 means "use the default", not "unlimited". */
  q.limit = 0;
  kbc_search_result r3;
  KBC_CHECK_OK(kbc_search_run(s, a, &q, NULL, 0, &r3, &err));
  KBC_CHECK_EQ_INT(r3.len, DOC_COUNT);
  KBC_CHECK(r3.cap <= KBC_MAX_HITS);

  kbc_searcher_free(s);
  kbc_index_free(ix);
  kbc_arena_free(a);
}

/* ------------------------------------------------------- cosine top-k ---- */

KBC_TEST(cosine_topk_ranks_duplicates_first_and_orthogonals_last) {
  kbc_err err;
  kbc_err_reset(&err);
  const float qvec[3] = {1.0f, 0.0f, 0.0f};
  const uint32_t ids[4] = {10, 11, 12, 13};
  const float emb[4 * 3] = {
      0.0f, 1.0f, 0.0f,  /* orthogonal */
      0.9f, 0.1f, 0.0f,  /* near-parallel */
      1.0f, 0.0f, 0.0f,  /* exact duplicate */
      0.5f, 0.5f, 0.0f,  /* 45 degrees */
  };
  kbc_hits hits;
  KBC_CHECK_OK(kbc_cosine_topk(qvec, 3, ids, emb, 4, 3, 10, &hits, &err));
  KBC_CHECK_EQ_INT(hits.len, 4);
  KBC_CHECK_EQ_INT(hits.items[0].doc, 12);
  KBC_CHECK_EQ_DBL(hits.items[0].score, 1.0, 1e-6);
  KBC_CHECK_EQ_INT(hits.items[1].doc, 11);
  /* The orthogonal row must rank below every row with a positive dot. */
  KBC_CHECK_EQ_INT(hits.items[3].doc, 10);
  KBC_CHECK_EQ_DBL(hits.items[3].score, 0.0, 1e-9);
  for (size_t i = 0; i < hits.len; i++) {
    KBC_CHECK_EQ_INT(hits.items[i].rank, (uint32_t)i);
    KBC_CHECK(!isnan(hits.items[i].score));
  }
  kbc_hits_free(&hits);
}

KBC_TEST(cosine_topk_skips_zero_norm_rows_instead_of_scoring_nan) {
  kbc_err err;
  kbc_err_reset(&err);
  const float qvec[2] = {0.0f, 1.0f};
  const uint32_t ids[3] = {0, 1, 2};
  const float emb[3 * 2] = {
      0.0f, 0.0f, /* zero norm: skipped, never divided by */
      0.0f, 2.0f, /* parallel */
      1.0f, 1.0f, /* partial */
  };
  kbc_hits hits;
  KBC_CHECK_OK(kbc_cosine_topk(qvec, 2, ids, emb, 3, 2, 10, &hits, &err));
  KBC_CHECK_EQ_INT(hits.len, 2);
  KBC_CHECK_EQ_INT(hits.items[0].doc, 1);
  KBC_CHECK_EQ_DBL(hits.items[0].score, 1.0, 1e-6);
  KBC_CHECK_EQ_INT(hits.items[1].doc, 2);
  for (size_t i = 0; i < hits.len; i++) {
    KBC_CHECK(!isnan(hits.items[i].score));
  }
  kbc_hits_free(&hits);
}

KBC_TEST(cosine_topk_zero_norm_query_is_ok_with_no_hits) {
  kbc_err err;
  kbc_err_reset(&err);
  const float qvec[2] = {0.0f, 0.0f};
  const uint32_t ids[2] = {0, 1};
  const float emb[2 * 2] = {1.0f, 0.0f, 0.0f, 1.0f};
  kbc_hits hits;
  KBC_CHECK_OK(kbc_cosine_topk(qvec, 2, ids, emb, 2, 2, 10, &hits, &err));
  KBC_CHECK_EQ_INT(hits.len, 0);
  kbc_hits_free(&hits);
}

KBC_TEST(cosine_topk_rejects_a_dim_mismatch_naming_both_dims) {
  kbc_err err;
  kbc_err_reset(&err);
  const float qvec[3] = {1.0f, 0.0f, 0.0f};
  const uint32_t ids[1] = {0};
  const float emb[2] = {1.0f, 0.0f};
  kbc_hits hits;
  kbc_status st = kbc_cosine_topk(qvec, 3, ids, emb, 1, 2, 10, &hits, &err);
  KBC_CHECK_ERR(st, KBC_ERR_INVALID);
  KBC_CHECK_ERR_MSG(err);
  KBC_CHECK(strstr(err.msg, "3") != NULL);
  KBC_CHECK(strstr(err.msg, "2") != NULL);
  KBC_CHECK_EQ_INT(hits.len, 0);
  kbc_hits_free(&hits);

  /* The mirror case is the same error, not a read past the shorter vector. */
  kbc_err_reset(&err);
  const float q2[2] = {1.0f, 0.0f};
  const float e2[3] = {1.0f, 0.0f, 0.0f};
  KBC_CHECK_ERR(kbc_cosine_topk(q2, 2, ids, e2, 1, 3, 10, &hits, &err),
                KBC_ERR_INVALID);
  KBC_CHECK(strstr(err.msg, "3") != NULL);
  kbc_hits_free(&hits);
}

KBC_TEST(cosine_topk_limit_beyond_n_returns_n) {
  kbc_err err;
  kbc_err_reset(&err);
  const float qvec[2] = {1.0f, 0.0f};
  const uint32_t ids[2] = {0, 1};
  const float emb[4] = {1.0f, 0.0f, 0.0f, 1.0f};
  kbc_hits hits;
  KBC_CHECK_OK(kbc_cosine_topk(qvec, 2, ids, emb, 2, 2, 100, &hits, &err));
  KBC_CHECK_EQ_INT(hits.len, 2);

  /* kbc_cosine_topk inits the out-hits itself, so a live one must be released
   * before reuse — the caller owns it. */
  kbc_hits_free(&hits);
  /* Re-running into a used hits struct must not append to the old contents. */
  KBC_CHECK_OK(kbc_cosine_topk(qvec, 2, ids, emb, 2, 2, 1, &hits, &err));
  KBC_CHECK_EQ_INT(hits.len, 1);
  KBC_CHECK_EQ_INT(hits.items[0].doc, 0);
  kbc_hits_free(&hits);
}

/* ---------------------------------------------------------------- modes -- */

KBC_TEST(search_mode_strings_round_trip_and_unknown_modes_are_rejected) {
  kbc_search_mode m = KBC_MODE_HYBRID;
  KBC_CHECK(kbc_search_mode_parse("SEMANTIC", &m));
  KBC_CHECK_EQ_INT(m, KBC_MODE_SEMANTIC);
  KBC_CHECK_EQ_STR(kbc_search_mode_str(KBC_MODE_SEMANTIC), "semantic");
  KBC_CHECK(kbc_search_mode_parse("keyword_only", &m));
  KBC_CHECK_EQ_INT(m, KBC_MODE_KEYWORD);
  KBC_CHECK(kbc_search_mode_parse("vector", &m));
  KBC_CHECK_EQ_INT(m, KBC_MODE_SEMANTIC);
  KBC_CHECK(!kbc_search_mode_parse("fuzzy", &m));
  KBC_CHECK(!kbc_search_mode_parse(NULL, &m));

  /* An out-of-range mode is a caller error, not a silent hybrid. */
  kbc_err err;
  kbc_err_reset(&err);
  kbc_arena *a = kbc_arena_new(4096);
  kbc_index *ix = build_index(a, &err);
  kbc_searcher *s = kbc_searcher_new(ix, resolver, NULL, &err);
  kbc_query q = query((kbc_search_mode)99, "search");
  kbc_search_result r;
  KBC_CHECK_ERR(kbc_search_run(s, a, &q, NULL, 0, &r, &err), KBC_ERR_INVALID);
  KBC_CHECK_ERR_MSG(err);
  kbc_searcher_free(s);
  kbc_index_free(ix);
  kbc_arena_free(a);
}

KBC_TEST(empty_query_is_rejected_before_any_lane_runs) {
  kbc_err err;
  kbc_err_reset(&err);
  kbc_arena *a = kbc_arena_new(4096);
  kbc_index *ix = build_index(a, &err);
  kbc_searcher *s = kbc_searcher_new(ix, resolver, NULL, &err);
  kbc_query q = query(KBC_MODE_HYBRID, "");
  kbc_search_result r;
  KBC_CHECK_ERR(kbc_search_run(s, a, &q, NULL, 0, &r, &err), KBC_ERR_INVALID);
  KBC_CHECK_ERR_MSG(err);

  /* Punctuation only tokenizes to nothing: that is the empty stopword case,
   * not an error. */
  kbc_query q2 = query(KBC_MODE_HYBRID, "---");
  kbc_search_result r2;
  KBC_CHECK_OK(kbc_search_run(s, a, &q2, NULL, 0, &r2, &err));
  KBC_CHECK_EQ_INT(r2.len, 0);

  kbc_searcher_free(s);
  kbc_index_free(ix);
  kbc_arena_free(a);
}


/* =================================================== query grammar ====== */

/* A second, grammar-shaped fixture. The main DOCS table gives every doc a
 * one-word title that is also its distinguishing token, which is exactly the
 * shape that makes a title boost indistinguishable from a ranking change. */
typedef struct {
  const char *corpus;
  const char *path;
  const char *title;
  const char *text;
} gram_doc;

static const gram_doc GRAM_DOCS[] = {
    {"kb", "notes/alpha.md", "Beta", "alpha alpha"},
    {"kb", "notes/beta.md", "Gamma", "alpha alpha"},
    {"kb", "deep notes/delta.md", "Alpha notes", "alpha"},
    {"kb", "other/epsilon.md", "Zulu", "gamma"},
};

static kbc_index *build_gram_index(kbc_arena *a, kbc_err *err) {
  kbc_index *ix = kbc_index_new();
  if (ix == NULL) return NULL;
  if (kbc_index_begin_build(ix, err) != KBC_OK) return ix;
  const size_t n = sizeof(GRAM_DOCS) / sizeof(GRAM_DOCS[0]);
  for (size_t i = 0; i < n; i++) {
    kbc_tokens toks;
    memset(&toks, 0, sizeof(toks));
    if (kbc_tokenize(a, GRAM_DOCS[i].text, strlen(GRAM_DOCS[i].text), &toks,
                     err) != KBC_OK)
      return ix;
    if (kbc_index_add_doc(ix, (uint32_t)i, GRAM_DOCS[i].corpus, GRAM_DOCS[i].path,
                          GRAM_DOCS[i].title, KBC_KIND_NOTE, &toks, err) !=
        KBC_OK)
      return ix;
  }
  if (kbc_index_end_build(ix, err) != KBC_OK) return ix;
  return ix;
}

static kbc_status gram_run(kbc_searcher *s, kbc_arena *a, const char *text,
                           kbc_search_result *r, kbc_err *err) {
  kbc_query q;
  memset(&q, 0, sizeof(q));
  q.q = text;
  q.mode = KBC_MODE_KEYWORD;
  q.kind = KBC_KIND__COUNT;
  return kbc_search_run(s, a, &q, NULL, 0, r, err);
}

/* fusion.rs:175 multiplies the score by (1 + TITLE_BOOST) when ANY query
 * term of 3+ bytes occurs anywhere in the title. Docs 0 and 1 have identical
 * bodies, so BM25 alone cannot order them; only the title can. */
KBC_TEST(title_match_outranks_an_equal_body_match) {
  kbc_err err;
  kbc_err_reset(&err);
  kbc_arena *a = kbc_arena_new(4096);
  kbc_index *ix = build_gram_index(a, &err);
  KBC_CHECK_OK(err.status);
  kbc_searcher *s = kbc_searcher_new(ix, resolver, NULL, &err);
  KBC_CHECK_NOT_NULL(s);

  /* "alpha" is in both bodies, in doc 0's title? no — in neither. Add the
   * term to the query so both docs tie on BM25, then boost only by title. */
  kbc_search_result r;
  KBC_CHECK_OK(gram_run(s, a, "alpha", &r, &err));
  KBC_CHECK_EQ_INT(r.len, 3);
  /* Doc 2 holds "alpha" once in a one-token body, so BM25 ranks it first and
   * its title ("Alpha notes") is the only one that contains a query term.
   * Docs 0 and 1 have identical bodies, so the boost is the only thing in
   * play here. */
  KBC_CHECK_EQ_INT(r.rows[0].doc_id, 2);
  KBC_CHECK_EQ_DBL(r.rows[0].score, 1.5 / 62.0, 1e-9);
  /* …and it does not leak to the other two: ranks 0 and 1 keep their bare RRF
   * scores, so the 1.5 landed on the title hit and nowhere else. */
  KBC_CHECK_EQ_INT(r.rows[1].doc_id, 0);
  KBC_CHECK_EQ_DBL(r.rows[1].score, 1.0 / 60.0, 1e-9);
  KBC_CHECK_EQ_INT(r.rows[2].doc_id, 1);
  KBC_CHECK_EQ_DBL(r.rows[2].score, 1.0 / 61.0, 1e-9);

  kbc_searcher_free(s);
  kbc_index_free(ix);
  kbc_arena_free(a);
}

/* One factor of (1 + 0.5) per hit, however many terms matched and whether or
 * not they are also in the body — fusion.rs:167-179 applies `1.0 + factor`
 * once, inside `if any(...)`. A per-term or per-field multiplication would
 * score 1.5^2 or 2.0 here. */
KBC_TEST(title_boost_is_applied_once_however_many_terms_match) {
  kbc_err err;
  kbc_err_reset(&err);
  kbc_arena *a = kbc_arena_new(4096);
  kbc_index *ix = build_gram_index(a, &err);
  KBC_CHECK_OK(err.status);
  kbc_searcher *s = kbc_searcher_new(ix, resolver, NULL, &err);
  KBC_CHECK_NOT_NULL(s);

  /* Doc 2's title "Alpha notes" contains BOTH query terms and its body holds
   * one of them, so "two terms matched" and "the term is in the body too"
   * cannot be confused with each other. Doc 3 holds neither. One factor of
   * (1 + 0.5) covers both terms: a per-term or per-field multiply would land
   * on 2.25x or 3.0x. */
  kbc_search_result r;
  KBC_CHECK_OK(gram_run(s, a, "alpha notes", &r, &err));
  KBC_CHECK_EQ_INT(r.len, 3);
  KBC_CHECK_EQ_INT(r.rows[0].doc_id, 2);
  KBC_CHECK_EQ_DBL(r.rows[0].score, 1.5 / 62.0, 1e-9);
  KBC_CHECK_EQ_DBL(r.rows[1].score, 1.0 / 60.0, 1e-9);

  kbc_searcher_free(s);
  kbc_index_free(ix);
  kbc_arena_free(a);
}

/* Two docs with byte-identical bodies, so BM25 cannot separate them and the
 * boost is the only thing that can. Built inline because the assertion is
 * about one specific title/body pair. */
static kbc_index *build_pair_index(kbc_arena *a, const char *title0,
                                   const char *title1, const char *body,
                                   kbc_err *err) {
  kbc_index *ix = kbc_index_new();
  if (ix == NULL) return NULL;
  if (kbc_index_begin_build(ix, err) != KBC_OK) return ix;
  static const char *const paths[2] = {"notes/one.md", "notes/two.md"};
  const char *titles[2] = {title0, title1};
  for (uint32_t i = 0; i < 2; i++) {
    kbc_tokens toks;
    memset(&toks, 0, sizeof(toks));
    if (kbc_tokenize(a, body, strlen(body), &toks, err) != KBC_OK) return ix;
    if (kbc_index_add_doc(ix, i, "kb", paths[i], titles[i], KBC_KIND_NOTE,
                          &toks, err) != KBC_OK)
      return ix;
  }
  if (kbc_index_end_build(ix, err) != KBC_OK) return ix;
  return ix;
}

/* fusion.rs:161 drops terms shorter than 3 bytes before the title is read, so
 * a 2-byte term that occurs in a title must NOT lift it. Both docs carry "ax"
 * identically, so the first is ahead only by doc id — a leaked boost would
 * put 1.5/60 where 1/60 belongs. */
KBC_TEST(title_boost_ignores_terms_shorter_than_three_bytes) {
  kbc_err err;
  kbc_err_reset(&err);
  kbc_arena *a = kbc_arena_new(4096);
  kbc_index *ix = build_pair_index(a, "Axolotl guide", "Eta", "ax ax", &err);
  KBC_CHECK_OK(err.status);
  kbc_searcher *s = kbc_searcher_new(ix, resolver, NULL, &err);
  KBC_CHECK_NOT_NULL(s);

  kbc_search_result r;
  KBC_CHECK_OK(gram_run(s, a, "ax", &r, &err));
  KBC_CHECK_EQ_INT(r.len, 2);
  KBC_CHECK_EQ_INT(r.rows[0].doc_id, 0);
  KBC_CHECK_EQ_DBL(r.rows[0].score, 1.0 / 60.0, 1e-9);
  KBC_CHECK_EQ_DBL(r.rows[1].score, 1.0 / 61.0, 1e-9);

  kbc_searcher_free(s);
  kbc_index_free(ix);
  kbc_arena_free(a);
}

/* Substring, not word-boundary: fusion.rs:168 documents that a word-boundary
 * variant bench-measured WORSE, because the query "embed" has to reach the
 * title "Embeddings". "embed" is not a whole word there, so a word-boundary
 * matcher would leave doc 0 on its bare 1/60 and this test fails. */
KBC_TEST(title_boost_is_a_substring_match_not_a_word_match) {
  kbc_err err;
  kbc_err_reset(&err);
  kbc_arena *a = kbc_arena_new(4096);
  kbc_index *ix = build_pair_index(a, "Embeddings guide", "Zeta", "embed",
                                   &err);
  KBC_CHECK_OK(err.status);
  kbc_searcher *s = kbc_searcher_new(ix, resolver, NULL, &err);
  KBC_CHECK_NOT_NULL(s);

  kbc_search_result r;
  KBC_CHECK_OK(gram_run(s, a, "embed", &r, &err));
  KBC_CHECK_EQ_INT(r.len, 2);
  KBC_CHECK_EQ_INT(r.rows[0].doc_id, 0);
  KBC_CHECK_EQ_DBL(r.rows[0].score, 1.5 / 60.0, 1e-9);
  KBC_CHECK_EQ_DBL(r.rows[1].score, 1.0 / 61.0, 1e-9);

  kbc_searcher_free(s);
  kbc_index_free(ix);
  kbc_arena_free(a);
}

/* An atom the index cannot evaluate is an ERROR, never a search for the
 * literal words "tag" and "rust". `tag:` is a real key in the original
 * (query.rs:47) whose predicate — a tag facet — has no counterpart in a
 * kbc_doc_meta, so kb-c must refuse it by name. */
KBC_TEST(an_unsupported_key_is_rejected_loudly_not_searched_literally) {
  kbc_err err;
  kbc_err_reset(&err);
  kbc_arena *a = kbc_arena_new(4096);
  kbc_index *ix = build_gram_index(a, &err);
  KBC_CHECK_OK(err.status);
  kbc_searcher *s = kbc_searcher_new(ix, resolver, NULL, &err);
  KBC_CHECK_NOT_NULL(s);

  static const char *const keys[] = {"tag:rust",  "cap:table",
                                     "since:7d",  "index:true",
                                     "scope:kb",  "wibble:1"};
  for (size_t i = 0; i < sizeof(keys) / sizeof(keys[0]); i++) {
    kbc_search_result r;
    kbc_err_reset(&err);
    const kbc_status st = gram_run(s, a, keys[i], &r, &err);
    KBC_CHECK(st == KBC_ERR_UNSUPPORTED || st == KBC_ERR_INVALID);
    KBC_CHECK(err.msg[0] != '\0');
    KBC_CHECK_EQ_INT(r.len, 0);
  }

  kbc_searcher_free(s);
  kbc_index_free(ix);
  kbc_arena_free(a);
}

/* The error names the offending key: a user who typed `tag:rust` must be
 * able to see which construct was refused. */
KBC_TEST(an_unsupported_key_error_names_the_key) {
  kbc_err err;
  kbc_err_reset(&err);
  kbc_arena *a = kbc_arena_new(4096);
  kbc_index *ix = build_gram_index(a, &err);
  KBC_CHECK_OK(err.status);
  kbc_searcher *s = kbc_searcher_new(ix, resolver, NULL, &err);
  KBC_CHECK_NOT_NULL(s);

  kbc_search_result r;
  KBC_CHECK_EQ_INT(gram_run(s, a, "tag:rust", &r, &err), KBC_ERR_UNSUPPORTED);
  KBC_CHECK(strstr(err.msg, "tag") != NULL);
  kbc_err_reset(&err);
  KBC_CHECK_EQ_INT(gram_run(s, a, "wibble:1", &r, &err), KBC_ERR_INVALID);
  KBC_CHECK(strstr(err.msg, "wibble") != NULL);

  kbc_searcher_free(s);
  kbc_index_free(ix);
  kbc_arena_free(a);
}

/* `folder:` is the one facet the index can answer: it is kbc_query's
 * path_prefix. It filters, and the free text alongside it still scores. */
KBC_TEST(folder_atom_filters_by_path_prefix_and_leaves_the_text_alone) {
  kbc_err err;
  kbc_err_reset(&err);
  kbc_arena *a = kbc_arena_new(4096);
  kbc_index *ix = build_gram_index(a, &err);
  KBC_CHECK_OK(err.status);
  kbc_searcher *s = kbc_searcher_new(ix, resolver, NULL, &err);
  KBC_CHECK_NOT_NULL(s);

  kbc_search_result r;
  KBC_CHECK_OK(gram_run(s, a, "folder:notes alpha", &r, &err));
  KBC_CHECK_EQ_INT(r.len, 2);
  KBC_CHECK(r.rows[0].doc_id == 0 && r.rows[1].doc_id == 1);

  /* A quoted value carries a space through: "deep notes" is one prefix, and
   * a bare `folder:deep notes` would be the two atoms `deep` and `notes`. */
  kbc_err_reset(&err);
  KBC_CHECK_OK(gram_run(s, a, "folder:\"deep notes\" alpha", &r, &err));
  KBC_CHECK_EQ_INT(r.len, 1);
  KBC_CHECK_EQ_INT(r.rows[0].doc_id, 2);

  /* A facet-only query filters the corpus rather than scoring it. */
  kbc_err_reset(&err);
  KBC_CHECK_OK(gram_run(s, a, "folder:notes", &r, &err));
  KBC_CHECK_EQ_INT(r.len, 2);

  kbc_searcher_free(s);
  kbc_index_free(ix);
  kbc_arena_free(a);
}

/* What a filter-only query must NOT do: a query the grammar reduces to
 * nothing searchable still returns nothing rather than everything. */
KBC_TEST(a_stopword_or_punctuation_query_returns_nothing_not_everything) {
  kbc_err err;
  kbc_err_reset(&err);
  kbc_arena *a = kbc_arena_new(4096);
  kbc_index *ix = build_gram_index(a, &err);
  KBC_CHECK_OK(err.status);
  kbc_searcher *s = kbc_searcher_new(ix, resolver, NULL, &err);
  KBC_CHECK_NOT_NULL(s);

  static const char *const qs[] = {"the and of", "...", "???", "  ,.;  "};
  for (size_t i = 0; i < sizeof(qs) / sizeof(qs[0]); i++) {
    kbc_search_result r;
    kbc_err_reset(&err);
    KBC_CHECK_OK(gram_run(s, a, qs[i], &r, &err));
    KBC_CHECK_EQ_INT(r.len, 0);
  }

  kbc_searcher_free(s);
  kbc_index_free(ix);
  kbc_arena_free(a);
}

/* The kb SEARCH route hands `?q=` to BM25 unparsed
 * (routes/search.rs:1163); query.rs is the gallery's filter overlay. So the
 * boolean shape over free text changes nothing there, and must change nothing
 * here: `a AND b`, `a OR b` and `a b` are the same one BM25 string
 * (query.rs:419). An implementation that AND-ed terms would return fewer
 * rows for the first form. */
KBC_TEST(and_or_and_not_do_not_reshape_free_text) {
  kbc_err err;
  kbc_err_reset(&err);
  kbc_arena *a = kbc_arena_new(4096);
  kbc_index *ix = build_gram_index(a, &err);
  KBC_CHECK_OK(err.status);
  kbc_searcher *s = kbc_searcher_new(ix, resolver, NULL, &err);
  KBC_CHECK_NOT_NULL(s);

  kbc_search_result plain, conj, disj, only_alpha, negated;
  KBC_CHECK_OK(gram_run(s, a, "alpha gamma", &plain, &err));
  KBC_CHECK_OK(gram_run(s, a, "alpha AND gamma", &conj, &err));
  KBC_CHECK_OK(gram_run(s, a, "alpha OR gamma", &disj, &err));
  KBC_CHECK_OK(gram_run(s, a, "alpha", &only_alpha, &err));
  KBC_CHECK_OK(gram_run(s, a, "alpha NOT gamma", &negated, &err));
  KBC_CHECK_EQ_INT(plain.len, 4);
  KBC_CHECK_EQ_INT(conj.len, plain.len);
  KBC_CHECK_EQ_INT(disj.len, plain.len);
  for (size_t i = 0; i < plain.len; i++) {
    KBC_CHECK_EQ_INT(conj.rows[i].doc_id, plain.rows[i].doc_id);
    KBC_CHECK_EQ_INT(disj.rows[i].doc_id, plain.rows[i].doc_id);
  }
  /* `NOT text:` is the one atom the original refuses to filter on: it warns
   * and DROPS the term (query.rs:428), so `a NOT b` scores `a` alone. It must
   * not quietly become a b-exclusion, and it must not warn-and-keep either. */
  KBC_CHECK_EQ_INT(negated.len, only_alpha.len);
  for (size_t i = 0; i < only_alpha.len; i++)
    KBC_CHECK_EQ_INT(negated.rows[i].doc_id, only_alpha.rows[i].doc_id);

  kbc_searcher_free(s);
  kbc_index_free(ix);
  kbc_arena_free(a);
}

/* Groups still have to PARSE: the shape is carried and then discarded, so a
 * malformed one is an error rather than a silent literal search. */
KBC_TEST(a_malformed_or_over_deep_group_is_a_parse_error) {
  kbc_err err;
  kbc_err_reset(&err);
  kbc_arena *a = kbc_arena_new(4096);
  kbc_index *ix = build_gram_index(a, &err);
  KBC_CHECK_OK(err.status);
  kbc_searcher *s = kbc_searcher_new(ix, resolver, NULL, &err);
  KBC_CHECK_NOT_NULL(s);

  kbc_search_result r;
  KBC_CHECK_EQ_INT(gram_run(s, a, "(alpha", &r, &err), KBC_ERR_PARSE);
  KBC_CHECK_EQ_INT(gram_run(s, a, "alpha)", &r, &err), KBC_ERR_PARSE);
  KBC_CHECK_EQ_INT(gram_run(s, a, "\"unterminated", &r, &err), KBC_ERR_PARSE);
  KBC_CHECK_EQ_INT(gram_run(s, a, "tag:", &r, &err), KBC_ERR_PARSE);

  /* 65 nested groups: query.rs:532 caps the recursion at 64 so a hostile
   * `((((…))))` cannot blow the stack. */
  char deep[2 * 65 + 8];
  size_t n = 0;
  for (int i = 0; i < 65; i++) deep[n++] = '(';
  deep[n] = '\0';
  kbc_err_reset(&err);
  KBC_CHECK_EQ_INT(gram_run(s, a, deep, &r, &err), KBC_ERR_PARSE);
  KBC_CHECK(strstr(err.msg, "deep") != NULL);

  /* The same shape at the cap parses: the bound is a bound, not a rejection
   * of grouping. */
  n = 0;
  for (int i = 0; i < 60; i++) deep[n++] = '(';
  deep[n++] = 'a';
  for (int i = 0; i < 60; i++) deep[n++] = ')';
  deep[n] = '\0';
  kbc_err_reset(&err);
  KBC_CHECK_OK(gram_run(s, a, deep, &r, &err));

  kbc_searcher_free(s);
  kbc_index_free(ix);
  kbc_arena_free(a);
}

/* There is NO phrase search in the original: the Rust search route tokenizes
 * `?q=` and hands the terms to BM25 (routes/search.rs:1163), and query.rs
 * quotes an atom VALUE (`folder:"deep notes"`), never a phrase. So `"alpha
 * gamma"` is two terms, and a document holding them in the opposite order
 * still matches. This test exists to pin that reading of the original: if a
 * positional phrase search is ever added, this is the test that must change,
 * and it is the only thing standing between the two implementations. */
KBC_TEST(a_quoted_phrase_is_two_terms_not_an_ordered_phrase) {
  kbc_err err;
  kbc_err_reset(&err);
  kbc_arena *a = kbc_arena_new(4096);
  kbc_index *ix = build_gram_index(a, &err);
  KBC_CHECK_OK(err.status);
  kbc_searcher *s = kbc_searcher_new(ix, resolver, NULL, &err);
  KBC_CHECK_NOT_NULL(s);

  /* Doc 3's body is "gamma" and doc 0's is "alpha alpha": the words appear
   * in that order nowhere, yet a phrase query must return both. */
  kbc_search_result r;
  KBC_CHECK_OK(gram_run(s, a, "\"alpha gamma\"", &r, &err));
  KBC_CHECK_EQ_INT(r.len, 4);

  kbc_searcher_free(s);
  kbc_index_free(ix);
  kbc_arena_free(a);
}

/* A negated facet the single-prefix filter cannot express is an error, not a
 * silently-ignored atom. */
KBC_TEST(a_negated_folder_is_rejected_rather_than_ignored) {
  kbc_err err;
  kbc_err_reset(&err);
  kbc_arena *a = kbc_arena_new(4096);
  kbc_index *ix = build_gram_index(a, &err);
  KBC_CHECK_OK(err.status);
  kbc_searcher *s = kbc_searcher_new(ix, resolver, NULL, &err);
  KBC_CHECK_NOT_NULL(s);

  kbc_search_result r;
  KBC_CHECK_EQ_INT(gram_run(s, a, "NOT folder:notes", &r, &err),
                   KBC_ERR_UNSUPPORTED);
  KBC_CHECK_EQ_INT(gram_run(s, a, "folder:notes OR folder:other", &r, &err),
                   KBC_ERR_UNSUPPORTED);

  kbc_searcher_free(s);
  kbc_index_free(ix);
  kbc_arena_free(a);
}

/* The trailing `*` completion is part of the query string, so it must
 * survive the grammar: the text the grammar hands BM25 still ends in `*`. */
KBC_TEST(a_trailing_star_still_expands_through_the_grammar) {
  kbc_err err;
  kbc_err_reset(&err);
  kbc_arena *a = kbc_arena_new(4096);
  kbc_index *ix = build_gram_index(a, &err);
  KBC_CHECK_OK(err.status);
  kbc_searcher *s = kbc_searcher_new(ix, resolver, NULL, &err);
  KBC_CHECK_NOT_NULL(s);

  kbc_search_result r;
  KBC_CHECK_OK(gram_run(s, a, "alph*", &r, &err));
  KBC_CHECK_EQ_INT(r.len, 3);

  kbc_searcher_free(s);
  kbc_index_free(ix);
  kbc_arena_free(a);
}

/* ------------------------------------------------- large-store streaming -- */

/* The regression: a semantic query used to materialise the whole store into
 * the hit list before sorting, so any corpus past KBC_MAX_HITS rows failed
 * outright. A 1,114-doc bge-small store is what the daemon saw as HTTP 400
 * "search: 1114 docs: out of memory". Small dims keep these cases fast: the
 * defect was per-ROW, not per-float. */
#define BIG_DIM 4

/* Deterministic, well-spread unit-ish vectors: an LCG walk over a sphere is
 * not needed, only rows that are all distinct and all non-degenerate. */
static void big_vec(uint32_t i, float out[BIG_DIM]) {
  uint32_t x = i * 2654435761u + 12345u;
  for (size_t j = 0; j < BIG_DIM; j++) {
    x = x * 1664525u + 1013904223u;
    out[j] = (float)(int32_t)(x >> 8) / (float)(1 << 23) + 0.5f; /* (0,1] */
  }
}

static kbc_index *build_big_index(kbc_arena *a, uint32_t n, kbc_err *err) {
  kbc_index *ix = kbc_index_new();
  if (ix == NULL) return NULL;
  if (kbc_index_begin_build(ix, err) != KBC_OK) return ix;
  for (uint32_t i = 0; i < n; i++) {
    char path[64], text[96];
    snprintf(path, sizeof(path), "notes/doc%06u.md", i);
    /* Every doc shares the stem so the keyword lane has something to rank,
     * and adds a unique token so the docs are not byte-identical. */
    snprintf(text, sizeof(text), "search rank fusion corpus %u", i);
    kbc_tokens toks;
    memset(&toks, 0, sizeof(toks));
    if (kbc_tokenize(a, text, strlen(text), &toks, err) != KBC_OK) return ix;
    const char *title = "Corpus";
    if (kbc_index_add_doc(ix, i, "big", kbc_arena_strdup(a, path), title,
                          KBC_KIND_NOTE, &toks, err) != KBC_OK) {
      return ix;
    }
  }
  if (kbc_index_end_build(ix, err) != KBC_OK) return ix;
  return ix;
}

static kbc_vecstore *build_big_vecstore(uint32_t n, kbc_err *err) {
  kbc_vecstore *vs = kbc_vecstore_new(BIG_DIM, n, err);
  if (vs == NULL) return NULL;
  for (uint32_t i = 0; i < n; i++) {
    float v[BIG_DIM];
    big_vec(i, v);
    if (kbc_vecstore_set(vs, i, v, err) != KBC_OK) return vs;
  }
  return vs;
}

/* The reference the streaming lane must match: score every row, insert into
 * a descending list by (score desc, doc id asc), keep the head. This is the
 * pre-fix algorithm, written out independently of the heap under test.
 * The caller must pass room for `limit + 1` rows: a candidate that lands
 * past the head is written there and then dropped. */
static void brute_force_topk(const float *q, uint32_t n, size_t limit,
                             uint32_t *out_docs, double *out_scores) {
  double qnorm = 0.0;
  for (size_t j = 0; j < BIG_DIM; j++) qnorm += (double)q[j] * q[j];
  size_t m = 0;
  for (uint32_t i = 0; i < n; i++) {
    float v[BIG_DIM];
    big_vec(i, v);
    double dot = 0.0, norm = 0.0;
    for (size_t j = 0; j < BIG_DIM; j++) {
      dot += (double)q[j] * v[j];
      norm += (double)v[j] * v[j];
    }
    if (!(norm > 0.0) || !(qnorm > 0.0)) continue;
    const double score = dot / (sqrt(qnorm) * sqrt(norm));
    size_t at = m;
    while (at > 0) {
      const double prev = out_scores[at - 1];
      if (prev > score || (prev == score && out_docs[at - 1] < i)) break;
      out_scores[at] = prev;
      out_docs[at] = out_docs[at - 1];
      at--;
    }
    out_scores[at] = score;
    out_docs[at] = i;
    if (m < limit) m++;
  }
}

/* A semantic query over a store far past KBC_MAX_HITS rows must succeed and
 * return the same ranking a full scan would. */
KBC_TEST(a_semantic_query_over_a_store_larger_than_the_hit_ceiling_ranks_exactly) {
  const uint32_t n = 3000u;
  kbc_err err;
  kbc_err_reset(&err);
  kbc_arena *a = kbc_arena_new(1u << 20);
  kbc_index *ix = build_big_index(a, n, &err);
  KBC_CHECK_OK(err.status);
  kbc_vecstore *vs = build_big_vecstore(n, &err);
  KBC_CHECK_OK(err.status);
  kbc_searcher *s = kbc_searcher_new(ix, resolver, NULL, &err);
  KBC_CHECK_NOT_NULL(s);
  KBC_CHECK_OK(kbc_searcher_set_vecstore(s, vs, &err));

  const float q[BIG_DIM] = {0.25f, 0.5f, 0.75f, 1.0f};
  kbc_query qq = query(KBC_MODE_SEMANTIC, "corpus");
  qq.limit = 20;
  kbc_search_result r;
  kbc_status st = kbc_search_run(s, a, &qq, q, BIG_DIM, &r, &err);
  KBC_CHECK_OK(st);
  KBC_CHECK(!r.degraded);
  KBC_CHECK(r.vector_ran);

  /* One slot more than the limit: see brute_force_topk. */
  uint32_t want[21];
  double scores[21];
  brute_force_topk(q, n, 20, want, scores);
  KBC_CHECK_EQ_INT(r.len, 20);
  for (size_t i = 0; i < r.len && i < 20; i++) {
    KBC_CHECK_EQ_INT(r.rows[i].doc_id, want[i]);
    KBC_CHECK_EQ_DBL(r.rows[i].vector_score, scores[i], 1e-6);
    KBC_CHECK_EQ_INT(r.rows[i].vector_rank, (uint32_t)i);
  }

  kbc_searcher_free(s);
  kbc_vecstore_free(vs);
  kbc_index_free(ix);
  kbc_arena_free(a);
}

/* The property, stated as a test: peak arena use for a semantic query does
 * not grow with the corpus. Ten times the rows must cost about the same. */
KBC_TEST(semantic_peak_arena_use_is_independent_of_the_store_size) {
  const float q[BIG_DIM] = {0.25f, 0.5f, 0.75f, 1.0f};
  size_t peaks[2] = {0, 0};
  const uint32_t sizes[2] = {500u, 5000u};
  for (int c = 0; c < 2; c++) {
    kbc_err err;
    kbc_err_reset(&err);
    kbc_arena *a = kbc_arena_new(4096);
    kbc_index *ix = build_big_index(a, sizes[c], &err);
    KBC_CHECK_OK(err.status);
    kbc_vecstore *vs = build_big_vecstore(sizes[c], &err);
    KBC_CHECK_OK(err.status);
    kbc_searcher *s = kbc_searcher_new(ix, resolver, NULL, &err);
    KBC_CHECK_NOT_NULL(s);
    KBC_CHECK_OK(kbc_searcher_set_vecstore(s, vs, &err));

    const size_t before = kbc_arena_bytes(a);
    kbc_query qq = query(KBC_MODE_SEMANTIC, "corpus");
    qq.limit = 10;
    kbc_search_result r;
    KBC_CHECK_OK(kbc_search_run(s, a, &qq, q, BIG_DIM, &r, &err));
    KBC_CHECK_EQ_INT(r.len, 10);
    peaks[c] = kbc_arena_bytes(a) - before;

    kbc_searcher_free(s);
    kbc_vecstore_free(vs);
    kbc_index_free(ix);
    kbc_arena_free(a);
  }
  /* Same query, same limit, same depth: only the store differs. Anything
   * proportional to 10x the rows would blow this wide open. */
  KBC_CHECK(peaks[0] > 0);
  KBC_CHECK(peaks[1] <= peaks[0] * 2u + 4096u);
}

/* Hybrid over the same large store must come back with BOTH lanes populated
 * per row — a vector score of 0 and vector_rank UINT32_MAX would be a
 * keyword search wearing a hybrid label. */
KBC_TEST(hybrid_over_a_large_store_scores_both_lanes_on_the_same_row) {
  const uint32_t n = 3000u;
  kbc_err err;
  kbc_err_reset(&err);
  kbc_arena *a = kbc_arena_new(1u << 20);
  kbc_index *ix = build_big_index(a, n, &err);
  KBC_CHECK_OK(err.status);
  kbc_vecstore *vs = build_big_vecstore(n, &err);
  KBC_CHECK_OK(err.status);
  kbc_searcher *s = kbc_searcher_new(ix, resolver, NULL, &err);
  KBC_CHECK_NOT_NULL(s);
  KBC_CHECK_OK(kbc_searcher_set_vecstore(s, vs, &err));

  const float q[BIG_DIM] = {0.25f, 0.5f, 0.75f, 1.0f};
  kbc_query qq = query(KBC_MODE_HYBRID, "corpus");
  qq.limit = 20;
  kbc_search_result r;
  KBC_CHECK_OK(kbc_search_run(s, a, &qq, q, BIG_DIM, &r, &err));
  KBC_CHECK(!r.degraded);
  KBC_CHECK(r.vector_ran);
  KBC_CHECK(r.len > 0);

  size_t both = 0;
  for (size_t i = 0; i < r.len; i++) {
    if (r.rows[i].keyword_score > 0.0 && r.rows[i].vector_score != 0.0 &&
        r.rows[i].vector_rank != UINT32_MAX) {
      both++;
    }
  }
  KBC_CHECK(both > 0);

  kbc_searcher_free(s);
  kbc_vecstore_free(vs);
  kbc_index_free(ix);
  kbc_arena_free(a);
}

/* The public streaming entry point must also handle a store past the old
 * ceiling, with the same top-k a full scan gives. */
KBC_TEST(cosine_topk_ranks_a_store_past_the_hit_ceiling) {
  const uint32_t n = 2500u;
  kbc_err err;
  kbc_err_reset(&err);
  const float q[BIG_DIM] = {0.25f, 0.5f, 0.75f, 1.0f};
  uint32_t *ids = malloc(n * sizeof(*ids));
  float *emb = malloc((size_t)n * BIG_DIM * sizeof(*emb));
  KBC_CHECK_NOT_NULL(ids);
  KBC_CHECK_NOT_NULL(emb);
  if (ids == NULL || emb == NULL) {
    free(ids);
    free(emb);
    return;
  }
  for (uint32_t i = 0; i < n; i++) {
    ids[i] = 9000u + i;
    big_vec(i, emb + (size_t)i * BIG_DIM);
  }
  kbc_hits hits;
  KBC_CHECK_OK(kbc_cosine_topk(q, BIG_DIM, ids, emb, n, BIG_DIM, 10, &hits,
                               &err));
  KBC_CHECK_EQ_INT(hits.len, 10);
  for (size_t i = 1; i < hits.len; i++) {
    KBC_CHECK(hits.items[i - 1].score > hits.items[i].score ||
              (hits.items[i - 1].score == hits.items[i].score &&
               hits.items[i - 1].doc < hits.items[i].doc));
  }
  kbc_hits_free(&hits);
  free(ids);
  free(emb);
}

int main(void) {
  return kbc_test_run("search", (const kbc_test_case[]){
                          {"rrf_score_of_a_doc_first_in_both_lanes_is_two_over_rrf_k",
                           rrf_score_of_a_doc_first_in_both_lanes_is_two_over_rrf_k},
                          {"keyword_mode_runs_only_the_keyword_lane",
                           keyword_mode_runs_only_the_keyword_lane},
                          {"hybrid_without_a_vector_store_degrades_to_keyword",
                           hybrid_without_a_vector_store_degrades_to_keyword},
                          {"semantic_without_a_query_vector_is_an_empty_ok_not_an_error",
                           semantic_without_a_query_vector_is_an_empty_ok_not_an_error},
                          {"stopword_only_query_returns_nothing_rather_than_everything",
                           stopword_only_query_returns_nothing_rather_than_everything},
                          {"resolver_notfound_drops_the_row_and_the_search_still_succeeds",
                           resolver_notfound_drops_the_row_and_the_search_still_succeeds},
                          {"resolver_error_fails_the_whole_search",
                           resolver_error_fails_the_whole_search},
                          {"searcher_without_a_resolver_leaves_artifact_id_null",
                           searcher_without_a_resolver_leaves_artifact_id_null},
                          {"rows_carry_index_metadata_and_sort_by_fused_score_then_doc_id",
                           rows_carry_index_metadata_and_sort_by_fused_score_then_doc_id},
                          {"corpus_path_and_kind_filters_each_exclude_non_matching_docs",
                           corpus_path_and_kind_filters_each_exclude_non_matching_docs},
                          {"a_filter_that_matches_nothing_returns_zero_rows",
                           a_filter_that_matches_nothing_returns_zero_rows},
                          {"limit_is_clamped_and_never_exceeds_the_corpus",
                           limit_is_clamped_and_never_exceeds_the_corpus},
                          {"cosine_topk_ranks_duplicates_first_and_orthogonals_last",
                           cosine_topk_ranks_duplicates_first_and_orthogonals_last},
                          {"cosine_topk_skips_zero_norm_rows_instead_of_scoring_nan",
                           cosine_topk_skips_zero_norm_rows_instead_of_scoring_nan},
                          {"cosine_topk_zero_norm_query_is_ok_with_no_hits",
                           cosine_topk_zero_norm_query_is_ok_with_no_hits},
                          {"cosine_topk_rejects_a_dim_mismatch_naming_both_dims",
                           cosine_topk_rejects_a_dim_mismatch_naming_both_dims},
                          {"cosine_topk_limit_beyond_n_returns_n",
                           cosine_topk_limit_beyond_n_returns_n},
                          {"search_mode_strings_round_trip_and_unknown_modes_are_rejected",
                           search_mode_strings_round_trip_and_unknown_modes_are_rejected},
                          {"empty_query_is_rejected_before_any_lane_runs",
                           empty_query_is_rejected_before_any_lane_runs},
                          {"title_match_outranks_an_equal_body_match",
                           title_match_outranks_an_equal_body_match},
                          {"title_boost_is_applied_once_however_many_terms_match",
                           title_boost_is_applied_once_however_many_terms_match},
                          {"title_boost_ignores_terms_shorter_than_three_bytes",
                           title_boost_ignores_terms_shorter_than_three_bytes},
                          {"title_boost_is_a_substring_match_not_a_word_match",
                           title_boost_is_a_substring_match_not_a_word_match},
                          {"an_unsupported_key_is_rejected_loudly_not_searched_literally",
                           an_unsupported_key_is_rejected_loudly_not_searched_literally},
                          {"an_unsupported_key_error_names_the_key",
                           an_unsupported_key_error_names_the_key},
                          {"folder_atom_filters_by_path_prefix_and_leaves_the_text_alone",
                           folder_atom_filters_by_path_prefix_and_leaves_the_text_alone},
                          {"a_stopword_or_punctuation_query_returns_nothing_not_everything",
                           a_stopword_or_punctuation_query_returns_nothing_not_everything},
                          {"and_or_and_not_do_not_reshape_free_text",
                           and_or_and_not_do_not_reshape_free_text},
                          {"a_malformed_or_over_deep_group_is_a_parse_error",
                           a_malformed_or_over_deep_group_is_a_parse_error},
                          {"a_quoted_phrase_is_two_terms_not_an_ordered_phrase",
                           a_quoted_phrase_is_two_terms_not_an_ordered_phrase},
                          {"a_semantic_query_over_a_store_larger_than_the_hit_ceiling_ranks_exactly",
                           a_semantic_query_over_a_store_larger_than_the_hit_ceiling_ranks_exactly},
                          {"semantic_peak_arena_use_is_independent_of_the_store_size",
                           semantic_peak_arena_use_is_independent_of_the_store_size},
                          {"hybrid_over_a_large_store_scores_both_lanes_on_the_same_row",
                           hybrid_over_a_large_store_scores_both_lanes_on_the_same_row},
                          {"cosine_topk_ranks_a_store_past_the_hit_ceiling",
                           cosine_topk_ranks_a_store_past_the_hit_ceiling},
                          {"a_negated_folder_is_rejected_rather_than_ignored",
                           a_negated_folder_is_rejected_rather_than_ignored},
                          {"a_trailing_star_still_expands_through_the_grammar",
                           a_trailing_star_still_expands_through_the_grammar},
                          {NULL, NULL},
                      });
}
