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
  KBC_CHECK_EQ_DBL(r.rows[0].score, 2.0 / 60.0, 1e-9);
  KBC_CHECK_EQ_DBL(r.rows[0].vector_score, 1.0, 1e-6);

  /* A non-default rrf_k must move the score, or the constant is hard-wired. */
  kbc_query q2 = query(KBC_MODE_HYBRID, "epsilon");
  q2.rrf_k = 10;
  kbc_search_result r2;
  KBC_CHECK_OK(kbc_search_run(s, a, &q2, vec, DIM, &r2, &err));
  KBC_CHECK_EQ_INT(r2.len, 1);
  KBC_CHECK_EQ_DBL(r2.rows[0].score, 2.0 / 10.0, 1e-9);

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
                          {NULL, NULL},
                      });
}
