/* test_index.c — the inverted index: BM25 arithmetic, build-time validation,
 * save/open round trip, and the untrusted-input paths of kbc_index_open. */
#include "kbc_test.h"

#include "kbc/index.h"
#include "kbc/mem.h"
#include "kbc/parse.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------- helpers -- */

/* kbc_test_write_file is strlen-based, so it cannot write the NUL padding an
 * index file is full of. */
static void write_bytes(const char *path, const void *data, size_t len) {
  FILE *f = fopen(path, "wb");
  if (f == NULL) {
    fprintf(stderr, "  FAIL cannot write %s\n", path);
    kbc_test_failures++;
    return;
  }
  (size_t)fwrite(data, 1, len, f);
  (void)fclose(f);
}

static char *read_bytes(const char *path, size_t *len) {
  FILE *f = fopen(path, "rb");
  char *buf;
  if (f == NULL) return NULL;
  buf = (char *)malloc(1);
  *len = 0;
  for (;;) {
    size_t got;
    char *nb = (char *)realloc(buf, *len + 4097);
    if (nb == NULL) {
      free(buf);
      (void)fclose(f);
      return NULL;
    }
    buf = nb;
    got = fread(buf + *len, 1, 4096, f);
    *len += got;
    if (got < 4096) break;
  }
  (void)fclose(f);
  return buf;
}

/* kbc_tokens items are arena-owned, so a token vector only needs zeroing
 * before each tokenize; the arena that owns them is freed with the test. */
static void tokens_zero(kbc_tokens *t) {
  t->items = NULL;
  t->len = 0;
  t->cap = 0;
}

/* Tokenizes `text` into the arena; every test indexes through the real
 * tokenizer so the postings under test are the ones production would have. */
static kbc_status tok(kbc_arena *a, const char *text, kbc_tokens *out,
                      kbc_err *err) {
  tokens_zero(out);
  return kbc_tokenize(a, text, strlen(text), out, err);
}

static kbc_status add(kbc_index *ix, kbc_arena *a, uint32_t id,
                      const char *corpus, const char *path, const char *title,
                      const char *body, kbc_err *err) {
  kbc_tokens t;
  kbc_status st = tok(a, body, &t, err);
  if (st != KBC_OK) return st;
  st = kbc_index_add_doc(ix, id, corpus, path, title, KBC_KIND_ARTIFACT, &t, err);
  return st;
}

/* The two documents build_two indexes, kept in one place so tests can
 * derive expectations from them instead of pinning a magic number. */
static const char *const kTwoBodies[2] = {"alpha alpha beta", "alpha gamma"};

/* Distinct terms the real tokenizer produces for kTwoBodies: tokenized and
 * de-duplicated, so a fixture change moves the expectation with it. */
static size_t two_doc_term_count(void) {
  kbc_arena *a = kbc_arena_new(1024);
  kbc_err err;
  kbc_token seen[16];
  size_t i, j, n = 0;
  memset(&err, 0, sizeof err);
  for (i = 0; i < sizeof kTwoBodies / sizeof kTwoBodies[0]; i++) {
    kbc_tokens t;
    tokens_zero(&t);
    (void)kbc_tokenize(a, kTwoBodies[i], strlen(kTwoBodies[i]), &t, &err);
    for (j = 0; j < t.len; j++) {
      size_t k;
      for (k = 0; k < n; k++) {
        if (strcmp(seen[k].text, t.items[j].text) == 0) break;
      }
      if (k == n) seen[n++] = t.items[j];
    }
  }
  kbc_arena_free(a);
  return n;
}

/* doc0: alpha x2, beta; doc1: alpha, gamma. avgdl = 2.5, N = 2. */
static kbc_index *build_two(const char *dir) {
  kbc_index *ix = kbc_index_new();
  kbc_arena *a = kbc_arena_new(4096);
  kbc_err err;
  memset(&err, 0, sizeof err);
  (void)dir;
  kbc_index_begin_build(ix, &err);
  add(ix, a, 0, "kb", "a.md", "Alpha", kTwoBodies[0], &err);
  add(ix, a, 1, "kb", "b.md", "Beta", kTwoBodies[1], &err);
  kbc_index_end_build(ix, &err);
  kbc_arena_free(a);
  return ix;
}

/* Appends into an existing kbc_hits, the way a caller re-runs a query. */
static kbc_status query_into(kbc_index *ix, kbc_arena *a, const char *q,
                             size_t limit, kbc_hits *out) {
  kbc_tokens t;
  kbc_err err;
  kbc_status st;
  memset(&err, 0, sizeof err);
  tok(a, q, &t, &err);
  st = kbc_index_bm25(ix, &t, 1.2, 0.75, limit, out, &err);
  return st;
}

static kbc_hits query(kbc_index *ix, kbc_arena *a, const char *q, size_t limit,
                      kbc_status *st_out) {
  kbc_tokens t;
  kbc_err err;
  kbc_hits h;
  memset(&err, 0, sizeof err);
  kbc_hits_init(&h);
  tok(a, q, &t, &err);
  *st_out = kbc_index_bm25(ix, &t, 1.2, 0.75, limit, &h, &err);
  return h;
}

/* --------------------------------------------------------------- bm25 --- */

/* Hand-computed for the two-document index above:
 *   N = 2, df(alpha) = 2, avgdl = 5/2
 *   idf = ln(1 + (2 - 2 + 0.5) / 2.5)                    = ln(1.2)
 *   doc0: tf 2, len 3 -> 2 + 1.2*(0.25 + 0.75*1.2) = 3.38
 *   doc1: tf 1, len 2 -> 1 + 1.2*(0.25 + 0.75*0.8) = 2.62
 * This pins k1, b and the IDF variant together: a change to any of the three
 * moves the score. */
KBC_TEST(bm25_score_matches_hand_computation) {
  kbc_index *ix = build_two(NULL);
  kbc_arena *a = kbc_arena_new(1024);
  kbc_status st;
  kbc_hits h = query(ix, a, "alpha", 10, &st);
  KBC_CHECK_OK(st);
  KBC_CHECK_EQ_INT(h.len, 2);
  KBC_CHECK_EQ_INT(h.items[0].doc, 0);
  KBC_CHECK_EQ_DBL(h.items[0].score, 0.2373416715660948, 1e-9);
  KBC_CHECK_EQ_INT(h.items[1].doc, 1);
  KBC_CHECK_EQ_DBL(h.items[1].score, 0.1985680321518318, 1e-9);
  KBC_CHECK_EQ_INT(h.items[0].rank, 0);
  KBC_CHECK_EQ_INT(h.items[1].rank, 1);
  KBC_CHECK_EQ_DBL(h.items[0].vector_score, 0.0, 0.0);
  kbc_hits_free(&h);
  kbc_arena_free(a);
  kbc_index_free(ix);
}

/* Only the documents that contain the term are returned — a query term that
 * misses never drags in the rest of the corpus. */
KBC_TEST(bm25_returns_exactly_the_matching_docs) {
  kbc_index *ix = build_two(NULL);
  kbc_arena *a = kbc_arena_new(1024);
  kbc_status st;
  kbc_hits beta = query(ix, a, "beta", 10, &st);
  kbc_hits gamma = query(ix, a, "gamma", 10, &st);
  kbc_hits both = query(ix, a, "beta gamma", 10, &st);
  KBC_CHECK_OK(st);
  KBC_CHECK_EQ_INT(beta.len, 1);
  KBC_CHECK_EQ_INT(beta.items[0].doc, 0);
  KBC_CHECK_EQ_INT(gamma.len, 1);
  KBC_CHECK_EQ_INT(gamma.items[0].doc, 1);
  KBC_CHECK_EQ_INT(both.len, 2);
  KBC_CHECK(both.items[0].doc != both.items[1].doc);
  kbc_hits_free(&beta);
  kbc_hits_free(&gamma);
  kbc_hits_free(&both);
  kbc_arena_free(a);
  kbc_index_free(ix);
}

/* idf must fall as df rises. Same corpus, tf 1 in every hit; the two
 * queries differ only in df ("common" in all 3 docs, "rare" in 1), so the
 * gap between the two scores is the idf, not a tf or length effect.
 * Pinned: the shortest-document score for "common" is the top hit (docs 1
 * and 2, len 1, tie; doc 0 has len 2 and lands below them), and "rare"
 * scores far above any of them. */
KBC_TEST(bm25_idf_falls_as_df_rises) {
  kbc_index *ix = kbc_index_new();
  kbc_arena *a = kbc_arena_new(1024);
  kbc_err err;
  kbc_status st;
  kbc_hits common, rare;
  memset(&err, 0, sizeof err);
  KBC_CHECK_OK(kbc_index_begin_build(ix, &err));
  KBC_CHECK_OK(add(ix, a, 0, "kb", "0.md", "Zero", "common rare", &err));
  KBC_CHECK_OK(add(ix, a, 1, "kb", "1.md", "One", "common", &err));
  KBC_CHECK_OK(add(ix, a, 2, "kb", "2.md", "Two", "common", &err));
  KBC_CHECK_OK(kbc_index_end_build(ix, &err));
  KBC_CHECK_EQ_DBL(kbc_index_avg_doclen(ix), 4.0 / 3.0, 1e-12);

  common = query(ix, a, "common", 10, &st);
  KBC_CHECK_OK(st);
  rare = query(ix, a, "rare", 10, &st);
  KBC_CHECK_OK(st);
  KBC_CHECK_EQ_INT(common.len, 3);
  KBC_CHECK_EQ_INT(rare.len, 1);
  /* Sorted descending: doc0 (len 2) is not the top hit. */
  KBC_CHECK_EQ_DBL(common.items[0].score, 0.14874382975896186, 1e-9);
  KBC_CHECK_EQ_DBL(rare.items[0].score, 0.8142733421229427, 1e-9);
  /* A term in every document must score far below a term in one. */
  KBC_CHECK(rare.items[0].score > common.items[0].score);
  kbc_hits_free(&common);
  kbc_hits_free(&rare);
  kbc_arena_free(a);
  kbc_index_free(ix);
}

/* -------------------------------------------------------------- queries -- */

KBC_TEST(bm25_empty_and_stopword_queries_return_nothing) {
  kbc_index *ix = build_two(NULL);
  kbc_arena *a = kbc_arena_new(1024);
  kbc_err err;
  kbc_status st;
  kbc_hits empty, stop, absent;
  kbc_tokens none;
  memset(&err, 0, sizeof err);
  tokens_zero(&none);
  kbc_hits_init(&empty);
  kbc_hits_init(&stop);
  kbc_hits_init(&absent);

  KBC_CHECK_OK(kbc_index_bm25(ix, &none, 1.2, 0.75, 10, &empty, &err));
  KBC_CHECK_EQ_INT(empty.len, 0);

  KBC_CHECK_OK(tok(a, "the and of is", &none, &err));
  KBC_CHECK_EQ_INT(none.len, 0); /* every one of those is a stopword */
  KBC_CHECK_OK(kbc_index_bm25(ix, &none, 1.2, 0.75, 10, &stop, &err));
  KBC_CHECK_EQ_INT(stop.len, 0);

  KBC_CHECK_OK(tok(a, "unindexed", &none, &err));
  KBC_CHECK_EQ_INT(none.len, 1);
  st = kbc_index_bm25(ix, &none, 1.2, 0.75, 10, &absent, &err);
  KBC_CHECK_OK(st); /* a miss is an empty result, not an error */
  KBC_CHECK_EQ_INT(absent.len, 0);

  kbc_hits_free(&empty);
  kbc_hits_free(&stop);
  kbc_hits_free(&absent);
  kbc_arena_free(a);
  kbc_index_free(ix);
}

KBC_TEST(bm25_limits) {
  kbc_index *ix = build_two(NULL);
  kbc_arena *a = kbc_arena_new(1024);
  kbc_status st;
  kbc_hits one = query(ix, a, "alpha", 1, &st);
  KBC_CHECK_OK(st);
  KBC_CHECK_EQ_INT(one.len, 1);
  KBC_CHECK_EQ_INT(one.items[0].doc, 0); /* the best of the two survives */
  kbc_hits_free(&one);

  kbc_hits huge = query(ix, a, "alpha", 10000, &st);
  KBC_CHECK_OK(st);
  KBC_CHECK_EQ_INT(huge.len, 2); /* clamped to the corpus, not padded */
  kbc_hits_free(&huge);

  kbc_hits zero = query(ix, a, "alpha", 0, &st);
  KBC_CHECK_OK(st);
  KBC_CHECK_EQ_INT(zero.len, 0);
  kbc_hits_free(&zero);

  /* `out` is appended to: a second query adds to the first one's hits. */
  kbc_hits acc;
  kbc_hits_init(&acc);
  KBC_CHECK_OK(query_into(ix, a, "alpha", 10, &acc));
  KBC_CHECK_OK(query_into(ix, a, "gamma", 10, &acc));
  KBC_CHECK_EQ_INT(acc.len, 3);
  kbc_hits_free(&acc);
  kbc_arena_free(a);
  kbc_index_free(ix);
}

/* ---------------------------------------------------------------- ties --- */

/* Two documents with identical content score identically; the tie must break
 * by ascending doc_id so the same query always renders the same order. */
KBC_TEST(bm25_ties_break_by_ascending_doc_id) {
  kbc_index *ix = kbc_index_new();
  kbc_arena *a = kbc_arena_new(1024);
  kbc_err err;
  kbc_status st;
  kbc_hits h;
  memset(&err, 0, sizeof err);
  KBC_CHECK_OK(kbc_index_begin_build(ix, &err));
  KBC_CHECK_OK(add(ix, a, 0, "kb", "x.md", "X", "identical body", &err));
  KBC_CHECK_OK(add(ix, a, 1, "kb", "y.md", "Y", "identical body", &err));
  KBC_CHECK_OK(kbc_index_end_build(ix, &err));

  h = query(ix, a, "identical", 10, &st);
  KBC_CHECK_OK(st);
  KBC_CHECK_EQ_INT(h.len, 2);
  KBC_CHECK_EQ_DBL(h.items[0].score, h.items[1].score, 0.0);
  KBC_CHECK_EQ_INT(h.items[0].doc, 0);
  KBC_CHECK_EQ_INT(h.items[1].doc, 1);
  kbc_hits_free(&h);

  kbc_hits again = query(ix, a, "identical body", 10, &st);
  KBC_CHECK_OK(st);
  KBC_CHECK_EQ_INT(again.len, 2);
  KBC_CHECK_EQ_INT(again.items[0].doc, 0);
  KBC_CHECK_EQ_INT(again.items[1].doc, 1);
  kbc_hits_free(&again);
  kbc_arena_free(a);
  kbc_index_free(ix);
}

/* --------------------------------------------------------- empty document */

KBC_TEST(zero_token_document_is_recorded_without_postings) {
  kbc_index *ix = kbc_index_new();
  kbc_arena *a = kbc_arena_new(1024);
  kbc_err err;
  kbc_status st;
  kbc_hits h;
  memset(&err, 0, sizeof err);
  KBC_CHECK_OK(kbc_index_begin_build(ix, &err));
  KBC_CHECK_OK(add(ix, a, 0, "kb", "empty.md", "Empty", "", &err));
  KBC_CHECK_OK(add(ix, a, 1, "kb", "full.md", "Full", "zebra", &err));
  KBC_CHECK_OK(kbc_index_end_build(ix, &err));

  KBC_CHECK_EQ_INT(kbc_index_doc_count(ix), 2);
  KBC_CHECK_EQ_INT(kbc_index_posting_count(ix), 1);
  KBC_CHECK_NOT_NULL(kbc_index_doc(ix, 0));
  KBC_CHECK_EQ_STR(kbc_index_doc(ix, 0)->path, "empty.md");
  KBC_CHECK_EQ_INT(kbc_index_doc(ix, 0)->token_count, 0);
  KBC_CHECK_EQ_DBL(kbc_index_avg_doclen(ix), 0.5, 1e-12);

  h = query(ix, a, "zebra", 10, &st);
  KBC_CHECK_OK(st);
  KBC_CHECK_EQ_INT(h.len, 1);
  KBC_CHECK_EQ_INT(h.items[0].doc, 1); /* the empty doc can never match */
  kbc_hits_free(&h);
  kbc_arena_free(a);
  kbc_index_free(ix);
}

/* ------------------------------------------------------------ id_of ------ */

KBC_TEST(id_of_finds_by_corpus_and_path) {
  kbc_index *ix = build_two(NULL);
  KBC_CHECK_EQ_INT(kbc_index_id_of(ix, "kb", "a.md"), 0);
  KBC_CHECK_EQ_INT(kbc_index_id_of(ix, "kb", "b.md"), 1);
  KBC_CHECK_EQ_INT(kbc_index_id_of(ix, "kb", "missing.md"), UINT32_MAX);
  KBC_CHECK_EQ_INT(kbc_index_id_of(ix, "other", "a.md"), UINT32_MAX);
  KBC_CHECK_EQ_INT(kbc_index_id_of(ix, "kb", ""), UINT32_MAX);
  kbc_index_free(ix);
}

/* ------------------------------------------------------- add_doc rejects -- */

KBC_TEST(add_doc_rejects_bad_doc_ids) {
  kbc_index *ix = kbc_index_new();
  kbc_arena *a = kbc_arena_new(1024);
  kbc_err err;
  memset(&err, 0, sizeof err);
  KBC_CHECK_OK(kbc_index_begin_build(ix, &err));

  /* Non-monotonic / skipping: doc ids must be dense from 0. */
  KBC_CHECK_ERR(kbc_index_add_doc(ix, 5, "kb", "a.md", "A", KBC_KIND_NOTE, NULL,
                                  &err),
                KBC_ERR_INVALID);
  KBC_CHECK_ERR_MSG(err);
  KBC_CHECK(strstr(err.msg, "5") != NULL); /* names the offending id */
  KBC_CHECK(strstr(err.msg, "expected 0") != NULL); /* and the wanted one */
  KBC_CHECK_EQ_INT(kbc_index_doc_count(ix), 0);

  /* A rejected add must not have half-applied. */
  KBC_CHECK_OK(add(ix, a, 0, "kb", "a.md", "A", "alpha", &err));
  KBC_CHECK_ERR(kbc_index_add_doc(ix, 0, "kb", "b.md", "B", KBC_KIND_NOTE, NULL,
                                  &err),
                KBC_ERR_INVALID);
  KBC_CHECK(strstr(err.msg, "expected 1") != NULL);
  KBC_CHECK_EQ_INT(kbc_index_doc_count(ix), 1);

  /* Sealed index: no more documents. */
  KBC_CHECK_OK(kbc_index_end_build(ix, &err));
  KBC_CHECK_ERR(kbc_index_add_doc(ix, 1, "kb", "c.md", "C", KBC_KIND_NOTE, NULL,
                                  &err),
                KBC_ERR_INVALID);
  KBC_CHECK(strstr(err.msg, "sealed") != NULL);
  kbc_arena_free(a);
  kbc_index_free(ix);
}

KBC_TEST(add_doc_rejects_null_corpus_or_path) {
  kbc_index *ix = kbc_index_new();
  kbc_err err;
  memset(&err, 0, sizeof err);
  KBC_CHECK_OK(kbc_index_begin_build(ix, &err));

  KBC_CHECK_ERR(kbc_index_add_doc(ix, 0, NULL, "a.md", "A", KBC_KIND_NOTE, NULL,
                                  &err),
                KBC_ERR_INVALID);
  KBC_CHECK_ERR_MSG(err);
  KBC_CHECK(strstr(err.msg, "NULL") != NULL);
  KBC_CHECK_EQ_INT(kbc_index_doc_count(ix), 0);

  KBC_CHECK_ERR(kbc_index_add_doc(ix, 0, "kb", NULL, "A", KBC_KIND_NOTE, NULL,
                                  &err),
                KBC_ERR_INVALID);
  KBC_CHECK_ERR_MSG(err);
  KBC_CHECK(strstr(err.msg, "NULL") != NULL);
  KBC_CHECK_EQ_INT(kbc_index_doc_count(ix), 0);

  /* NULL err must be safe, and the doc must still be rejected. */
  KBC_CHECK_ERR(kbc_index_add_doc(ix, 0, NULL, "a.md", "A", KBC_KIND_NOTE, NULL,
                                  NULL),
                KBC_ERR_INVALID);
  kbc_index_free(ix);
}

KBC_TEST(add_doc_rejects_too_many_tokens) {
  kbc_index *ix = kbc_index_new();
  kbc_err err;
  size_t over = KBC_MAX_TOKENS_PER_DOC + 1u;
  kbc_token *items = (kbc_token *)calloc(over, sizeof(kbc_token));
  size_t i;
  kbc_tokens t;
  memset(&err, 0, sizeof err);
  KBC_CHECK_NOT_NULL(items);
  for (i = 0; i < over; i++) {
    items[i].text = "ab";
    items[i].len = 2;
  }
  t.items = items;
  t.len = over;
  t.cap = over;
  KBC_CHECK_OK(kbc_index_begin_build(ix, &err));
  KBC_CHECK_ERR(kbc_index_add_doc(ix, 0, "kb", "a.md", "A", KBC_KIND_NOTE, &t,
                                  &err),
                KBC_ERR_INVALID);
  KBC_CHECK_ERR_MSG(err);
  KBC_CHECK(strstr(err.msg, "200000") != NULL); /* names the limit */
  KBC_CHECK_EQ_INT(kbc_index_doc_count(ix), 0);
  KBC_CHECK_EQ_INT(kbc_index_term_count(ix), 0);

  /* One below the limit is accepted: the boundary is the limit, not limit-1. */
  t.len = KBC_MAX_TOKENS_PER_DOC;
  KBC_CHECK_OK(kbc_index_add_doc(ix, 0, "kb", "a.md", "A", KBC_KIND_NOTE, &t,
                                 &err));
  KBC_CHECK_EQ_INT(kbc_index_doc_count(ix), 1);
  KBC_CHECK_OK(kbc_index_end_build(ix, &err));
  free(items);
  kbc_index_free(ix);
}

/* ---------------------------------------------------------- round trip --- */

KBC_TEST(save_then_open_round_trip) {
  char dir[KBC_TEST_PATH_MAX];
  char path[KBC_TEST_PATH_MAX + 32];
  kbc_index *ix = build_two(NULL);
  kbc_arena *a = kbc_arena_new(1024);
  kbc_index *rx;
  kbc_err err;
  kbc_status st, st2;
  kbc_hits before, after;
  uint32_t d;

  kbc_test_tmpdir(dir, sizeof dir);
  snprintf(path, sizeof path, "%s/ix.bin", dir);
  memset(&err, 0, sizeof err);
  KBC_CHECK_OK(kbc_index_save(ix, path, &err));
  before = query(ix, a, "alpha", 10, &st);
  KBC_CHECK_OK(st);

  rx = kbc_index_open(path, &err);
  KBC_CHECK_NOT_NULL(rx);
  if (rx == NULL) {
    kbc_hits_free(&before);
    kbc_arena_free(a);
    kbc_index_free(ix);
    kbc_test_rmrf(dir);
    return;
  }
  KBC_CHECK_EQ_INT(kbc_index_doc_count(rx), kbc_index_doc_count(ix));
  KBC_CHECK_EQ_INT(kbc_index_term_count(rx), kbc_index_term_count(ix));
  KBC_CHECK_EQ_INT(kbc_index_posting_count(rx), kbc_index_posting_count(ix));
  KBC_CHECK_EQ_DBL(kbc_index_avg_doclen(rx), kbc_index_avg_doclen(ix), 1e-12);
  KBC_CHECK_EQ_INT(kbc_index_term_count(rx), (int)two_doc_term_count());

  for (d = 0; d < 2; d++) {
    const kbc_doc_meta *a1 = kbc_index_doc(ix, d);
    const kbc_doc_meta *b1 = kbc_index_doc(rx, d);
    KBC_CHECK_NOT_NULL(b1);
    if (b1 == NULL) continue;
    KBC_CHECK_EQ_STR(b1->corpus, a1->corpus);
    KBC_CHECK_EQ_STR(b1->path, a1->path);
    KBC_CHECK_EQ_STR(b1->title, a1->title);
    KBC_CHECK_EQ_INT(b1->kind, a1->kind);
    KBC_CHECK_EQ_INT(b1->token_count, a1->token_count);
  }
  KBC_CHECK_EQ_INT(kbc_index_id_of(rx, "kb", "b.md"), 1);
  KBC_CHECK_EQ_INT(kbc_index_id_of(rx, "kb", "nope.md"), UINT32_MAX);
  KBC_CHECK_NULL(kbc_index_doc(rx, 2));

  after = query(rx, a, "alpha", 10, &st2);
  KBC_CHECK_OK(st2);
  KBC_CHECK_EQ_INT(after.len, before.len);
  if (after.len == before.len) {
    for (d = 0; d < 2; d++) {
      KBC_CHECK_EQ_INT(after.items[d].doc, before.items[d].doc);
      KBC_CHECK_EQ_DBL(after.items[d].score, before.items[d].score, 0.0);
    }
  }

  kbc_hits_free(&before);
  kbc_hits_free(&after);
  kbc_index_free(rx);
  kbc_index_free(ix);
  kbc_arena_free(a);
  kbc_test_rmrf(dir);
}

/* An opened index is immutable: the build path must refuse to add to it. */
KBC_TEST(opened_index_is_sealed) {
  char dir[KBC_TEST_PATH_MAX];
  char path[KBC_TEST_PATH_MAX + 32];
  kbc_index *ix = build_two(NULL);
  kbc_index *rx;
  kbc_err err;
  memset(&err, 0, sizeof err);
  kbc_test_tmpdir(dir, sizeof dir);
  snprintf(path, sizeof path, "%s/ix.bin", dir);
  KBC_CHECK_OK(kbc_index_save(ix, path, &err));
  rx = kbc_index_open(path, &err);
  KBC_CHECK_NOT_NULL(rx);
  if (rx != NULL) {
    KBC_CHECK_ERR(kbc_index_add_doc(rx, 2, "kb", "c.md", "C", KBC_KIND_NOTE,
                                    NULL, &err),
                  KBC_ERR_INVALID);
    KBC_CHECK_EQ_INT(kbc_index_doc_count(rx), 2);
    kbc_index_free(rx);
  }
  kbc_index_free(ix);
  kbc_test_rmrf(dir);
}

/* --------------------------------------------------- untrusted open input -- */

static void save_valid(char *path, size_t cap, const char *dir) {
  kbc_index *ix = build_two(NULL);
  kbc_err err;
  memset(&err, 0, sizeof err);
  snprintf(path, cap, "%s/ix.bin", dir);
  kbc_index_save(ix, path, &err);
  kbc_index_free(ix);
}

KBC_TEST(open_rejects_missing_file) {
  char dir[KBC_TEST_PATH_MAX];
  char path[KBC_TEST_PATH_MAX + 32];
  kbc_err err;
  kbc_index *ix;
  memset(&err, 0, sizeof err);
  kbc_test_tmpdir(dir, sizeof dir);
  snprintf(path, sizeof path, "%s/absent.bin", dir);
  ix = kbc_index_open(path, &err);
  KBC_CHECK_NULL(ix);
  KBC_CHECK(kbc_failed(err.status));
  KBC_CHECK_ERR_MSG(err);
  KBC_CHECK(strstr(err.msg, "absent.bin") != NULL); /* names the file */
  kbc_test_rmrf(dir);
}

KBC_TEST(open_rejects_bad_magic) {
  char dir[KBC_TEST_PATH_MAX];
  char path[KBC_TEST_PATH_MAX + 32];
  kbc_err err;
  kbc_index *ix;
  uint8_t buf[128];
  memset(buf, 'x', sizeof buf);
  kbc_test_tmpdir(dir, sizeof dir);
  snprintf(path, sizeof path, "%s/bad.bin", dir);
  write_bytes(path, buf, sizeof buf);
  memset(&err, 0, sizeof err);
  ix = kbc_index_open(path, &err);
  KBC_CHECK_NULL(ix);
  KBC_CHECK_ERR(err.status, KBC_ERR_PARSE);
  KBC_CHECK_ERR_MSG(err);
  KBC_CHECK(strstr(err.msg, "magic") != NULL);
  kbc_test_rmrf(dir);
}

KBC_TEST(open_rejects_wrong_format_version) {
  char dir[KBC_TEST_PATH_MAX];
  char path[KBC_TEST_PATH_MAX + 32];
  kbc_err err;
  kbc_index *ix;
  size_t len = 0;
  char *buf;
  kbc_test_tmpdir(dir, sizeof dir);
  save_valid(path, sizeof path, dir);
  buf = read_bytes(path, &len);
  KBC_CHECK_NOT_NULL(buf);
  if (buf == NULL) {
    kbc_test_rmrf(dir);
    return;
  }
  buf[8] = (char)0x7f; /* format version 127: a file from a future build */
  write_bytes(path, buf, len);
  free(buf);
  memset(&err, 0, sizeof err);
  ix = kbc_index_open(path, &err);
  KBC_CHECK_NULL(ix);
  KBC_CHECK_ERR(err.status, KBC_ERR_PARSE);
  KBC_CHECK_ERR_MSG(err);
  KBC_CHECK(strstr(err.msg, "127") != NULL); /* names the version found */
  kbc_test_rmrf(dir);
}

KBC_TEST(open_rejects_zero_length_file) {
  char dir[KBC_TEST_PATH_MAX];
  char path[KBC_TEST_PATH_MAX + 32];
  kbc_err err;
  kbc_index *ix;
  kbc_test_tmpdir(dir, sizeof dir);
  snprintf(path, sizeof path, "%s/zero.bin", dir);
  write_bytes(path, "", 0);
  memset(&err, 0, sizeof err);
  ix = kbc_index_open(path, &err);
  KBC_CHECK_NULL(ix);
  KBC_CHECK_ERR(err.status, KBC_ERR_PARSE);
  KBC_CHECK_ERR_MSG(err);
  KBC_CHECK(strstr(err.msg, "0 bytes") != NULL);
  kbc_test_rmrf(dir);
}

/* Half a valid file: the header promises sections the mapping does not have. */
KBC_TEST(open_rejects_truncated_file) {
  char dir[KBC_TEST_PATH_MAX];
  char path[KBC_TEST_PATH_MAX + 32];
  kbc_err err;
  kbc_index *ix;
  size_t len = 0;
  char *buf;
  kbc_test_tmpdir(dir, sizeof dir);
  save_valid(path, sizeof path, dir);
  buf = read_bytes(path, &len);
  KBC_CHECK_NOT_NULL(buf);
  if (buf == NULL) {
    kbc_test_rmrf(dir);
    return;
  }
  KBC_CHECK(len > 64);
  write_bytes(path, buf, len / 2);
  free(buf);
  memset(&err, 0, sizeof err);
  ix = kbc_index_open(path, &err);
  KBC_CHECK_NULL(ix);
  KBC_CHECK_ERR(err.status, KBC_ERR_PARSE);
  KBC_CHECK_ERR_MSG(err);
  kbc_test_rmrf(dir);
}

/* A header whose doc_count/posting_count are inflated past the bytes present.
 * Offsets 16 (doc_count) and 24 (posting_count) are the first two counts a
 * corrupt writer would get wrong. */
KBC_TEST(open_rejects_lying_header_counts) {
  char dir[KBC_TEST_PATH_MAX];
  char path[KBC_TEST_PATH_MAX + 32];
  kbc_err err;
  kbc_index *ix;
  size_t len = 0;
  char *buf;
  int which;
  for (which = 0; which < 2; which++) {
    kbc_test_tmpdir(dir, sizeof dir);
    save_valid(path, sizeof path, dir);
    buf = read_bytes(path, &len);
    KBC_CHECK_NOT_NULL(buf);
    if (buf == NULL) break;
    /* 0xFFFFFFFF docs / postings: the bounds check must catch this before any
     * of the counts reaches a malloc or a loop bound. */
    memset(buf + (which == 0 ? 16 : 24), 0xff, 4);
    write_bytes(path, buf, len);
    free(buf);
    memset(&err, 0, sizeof err);
    ix = kbc_index_open(path, &err);
    KBC_CHECK_NULL(ix);
    KBC_CHECK_ERR(err.status, KBC_ERR_PARSE);
    KBC_CHECK_ERR_MSG(err);
    kbc_test_rmrf(dir);
  }
}

/* A term slot pointing outside the term arena: the kind of damage a partial
 * write leaves behind, and the one that would read out of the mapping. */
KBC_TEST(open_rejects_term_slot_outside_section) {
  char dir[KBC_TEST_PATH_MAX];
  char path[KBC_TEST_PATH_MAX + 32];
  kbc_err err;
  kbc_index *ix;
  size_t len = 0;
  char *buf;
  kbc_index *live = build_two(NULL);
  kbc_test_tmpdir(dir, sizeof dir);
  snprintf(path, sizeof path, "%s/ix.bin", dir);
  memset(&err, 0, sizeof err);
  KBC_CHECK_OK(kbc_index_save(live, path, &err));
  kbc_index_free(live);
  buf = read_bytes(path, &len);
  KBC_CHECK_NOT_NULL(buf);
  if (buf == NULL) {
    kbc_test_rmrf(dir);
    return;
  }
  /* Scribble every 0xFF byte over the payload; any structure the loader trusts
   * blindly (offsets, lengths) turns into a wild pointer or a huge count.
   * The header is left intact so the file still looks like a real index. */
  memset(buf + 64, 0xff, len - 64);
  write_bytes(path, buf, len);
  free(buf);
  memset(&err, 0, sizeof err);
  ix = kbc_index_open(path, &err);
  if (ix != NULL) {
    /* A structurally valid scramble is allowed; it must then answer queries
     * without reading outside the mapping, so poke it once. */
    kbc_arena *a = kbc_arena_new(1024);
    kbc_hits h;
    kbc_hits_init(&h);
    KBC_CHECK_OK(query_into(ix, a, "alpha", 10, &h));
    kbc_hits_free(&h);
    kbc_arena_free(a);
    kbc_index_free(ix);
  } else {
    KBC_CHECK(kbc_failed(err.status));
    KBC_CHECK_ERR_MSG(err);
  }
  kbc_test_rmrf(dir);
}

/* ------------------------------------------------------------- prefixes -- */

KBC_TEST(expand_prefix_matches_sorted) {
  kbc_index *ix = kbc_index_new();
  kbc_arena *a = kbc_arena_new(1024);
  kbc_arena *out_a = kbc_arena_new(1024);
  kbc_err err;
  kbc_strlist l;
  kbc_strlist long_prefix;
  char big[KBC_MAX_TERM_LEN + 64];
  memset(&err, 0, sizeof err);
  memset(big, 'q', sizeof big - 1);
  big[sizeof big - 1] = '\0';
  KBC_CHECK_OK(kbc_index_begin_build(ix, &err));
  KBC_CHECK_OK(add(ix, a, 0, "kb", "0.md", "0", "zebra alpine alpha", &err));
  KBC_CHECK_OK(add(ix, a, 1, "kb", "1.md", "1", "beta", &err));
  KBC_CHECK_OK(kbc_index_end_build(ix, &err));

  kbc_strlist_init(&l);
  KBC_CHECK_OK(kbc_index_expand_prefix(ix, out_a, "al", &l, &err));
  KBC_CHECK_EQ_INT(l.len, 2);
  if (l.len == 2) {
    KBC_CHECK_EQ_STR(l.items[0], "alpha");
    KBC_CHECK_EQ_STR(l.items[1], "alpine");
  }
  kbc_strlist_free(&l);

  kbc_strlist_init(&l);
  KBC_CHECK_OK(kbc_index_expand_prefix(ix, out_a, "z", &l, &err));
  KBC_CHECK_EQ_INT(l.len, 1); /* a one-letter prefix still matches "zebra" */
  KBC_CHECK_EQ_STR(l.items[0], "zebra");
  kbc_strlist_free(&l);

  /* Longer than every term in the index, and longer than a term may be. */
  kbc_strlist_init(&l);
  KBC_CHECK_OK(kbc_index_expand_prefix(ix, out_a, "alpin", &l, &err));
  KBC_CHECK_EQ_INT(l.len, 1);
  KBC_CHECK_EQ_STR(l.items[0], "alpine"); /* exact prefix of one term */
  kbc_strlist_free(&l);

  kbc_strlist_init(&l);
  KBC_CHECK_OK(kbc_index_expand_prefix(ix, out_a, "alphaXYZ", &l, &err));
  KBC_CHECK_EQ_INT(l.len, 0); /* no term starts with it: empty, not an error */
  kbc_strlist_free(&l);

  kbc_strlist_init(&long_prefix);
  KBC_CHECK_OK(kbc_index_expand_prefix(ix, out_a, big, &long_prefix, &err));
  KBC_CHECK_EQ_INT(long_prefix.len, 0); /* past KBC_MAX_TERM_LEN: safe no-op */
  kbc_strlist_free(&long_prefix);

  kbc_strlist_init(&l);
  KBC_CHECK_OK(kbc_index_expand_prefix(ix, out_a, "", &l, &err));
  KBC_CHECK_EQ_INT(l.len, kbc_index_term_count(ix)); /* empty prefix = all */
  kbc_strlist_free(&l);

  kbc_arena_free(out_a);
  kbc_arena_free(a);
  kbc_index_free(ix);
}

/* ------------------------------------------------------------- main ------ */

int main(void) {
  static const kbc_test_case cases[] = {
      {"bm25_score_matches_hand_computation", bm25_score_matches_hand_computation},
      {"bm25_returns_exactly_the_matching_docs",
       bm25_returns_exactly_the_matching_docs},
      {"bm25_idf_falls_as_df_rises", bm25_idf_falls_as_df_rises},
      {"bm25_empty_and_stopword_queries", bm25_empty_and_stopword_queries_return_nothing},
      {"bm25_limits", bm25_limits},
      {"bm25_ties_break_by_ascending_doc_id", bm25_ties_break_by_ascending_doc_id},
      {"zero_token_document_recorded", zero_token_document_is_recorded_without_postings},
      {"id_of_finds_by_corpus_and_path", id_of_finds_by_corpus_and_path},
      {"add_doc_rejects_bad_doc_ids", add_doc_rejects_bad_doc_ids},
      {"add_doc_rejects_null_corpus_or_path", add_doc_rejects_null_corpus_or_path},
      {"add_doc_rejects_too_many_tokens", add_doc_rejects_too_many_tokens},
      {"save_then_open_round_trip", save_then_open_round_trip},
      {"opened_index_is_sealed", opened_index_is_sealed},
      {"open_rejects_missing_file", open_rejects_missing_file},
      {"open_rejects_bad_magic", open_rejects_bad_magic},
      {"open_rejects_wrong_format_version", open_rejects_wrong_format_version},
      {"open_rejects_zero_length_file", open_rejects_zero_length_file},
      {"open_rejects_truncated_file", open_rejects_truncated_file},
      {"open_rejects_lying_header_counts", open_rejects_lying_header_counts},
      {"open_rejects_term_slot_outside_section",
       open_rejects_term_slot_outside_section},
      {"expand_prefix_matches_sorted", expand_prefix_matches_sorted},
      {NULL, NULL},
  };
  return kbc_test_run("index", cases);
}
