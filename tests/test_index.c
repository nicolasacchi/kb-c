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

/* The term hash table rehashes past its initial capacity, which MOVES every
 * live term to a new slot. Anything the build carries across a grow must be
 * keyed on something that travels with the term (its stable id), not on the
 * slot index: keying on the slot scatters a term's postings under whichever
 * term inherited the old index, and end_build's ascending-order invariant is
 * what catches it. A small fixture never rehashes, so this one is sized to
 * force several grows, and every expectation comes from a reference model
 * built in the test rather than from the index under test. */
#define GROW_DOCS 1500u
/* Enough documents that the term arena spans several 64 KiB blocks and the
 * term table has grown more than once: the save/open accounting has to hold
 * for an arena with a block boundary in it, not only for one that fits. */
#define BIG_DOCS 300u

/* Doc d: unique term "u<d>", "shared" when d % 5 == 0, "mid" when d % 7 == 0.
 * The modulus is chosen so every query below matches fewer than 512 documents: the
 * hits container refuses to grow past KBC_MAX_HITS, and a truncated result set
 * would mask the document-set check below.
 * The model mirrors exactly that, so a posting grouped under the wrong term
 * shows up as a wrong document set or a wrong score. */
static size_t grow_model_tf(uint32_t d, const char *term) {
  if (strcmp(term, "shared") == 0) {
    return (d % 5u == 0u) ? 1u : 0u;
  }
  if (strcmp(term, "mid") == 0) {
    return (d % 7u == 0u) ? 1u : 0u;
  }
  if (strncmp(term, "u", 1) == 0) {
    char mine[24];
    (void)snprintf(mine, sizeof mine, "u%u", d);
    return strcmp(mine, term) == 0 ? 1u : 0u;
  }
  return 0u;
}

static double grow_model_score(uint32_t d, const char *const *terms, size_t nq,
                               double avgdl, uint32_t ndocs, double k1,
                               double b) {
  double s = 0.0, dl;
  size_t i;
  dl = 1.0 + (double)grow_model_tf(d, "shared") + (double)grow_model_tf(d, "mid");
  for (i = 0; i < nq; i++) {
    uint32_t j;
    size_t df = 0;
    double tf, idf;
    for (j = 0; j < ndocs; j++) {
      df += grow_model_tf(j, terms[i]);
    }
    tf = (double)grow_model_tf(d, terms[i]);
    if (df == 0 || tf == 0.0) {
      continue;
    }
    idf = log(1.0 + ((double)ndocs - (double)df + 0.5) / ((double)df + 0.5));
    s += idf * (tf * (k1 + 1.0)) / (tf + k1 * (1.0 - b + b * (dl / avgdl)));
  }
  return s;
}

KBC_TEST(end_build_survives_a_term_table_rehash) {
  kbc_index *ix = kbc_index_new();
  kbc_arena *a = kbc_arena_new(1u << 20);
  kbc_err err;
  char body[128];
  double avgdl;
  uint32_t d;
  size_t i;
  static const char *const kShared[1] = {"shared"};
  static const char *const kMid[1] = {"mid"};
  static const char *const kUniq[1] = {"u7"};
  static const char *const kBoth[2] = {"shared", "mid"};
  static const struct {
    const char *text; /* what the query is asked, spaces and all */
    const char *const *q;
    size_t nq;
  } queries[] = {{"shared", kShared, 1},
                 {"mid", kMid, 1},
                 {"u7", kUniq, 1},
                 {"shared mid", kBoth, 2}};
  memset(&err, 0, sizeof err);
  KBC_CHECK_OK(kbc_index_begin_build(ix, &err));
  for (d = 0; d < GROW_DOCS; d++) {
    char uniq[24];
    (void)snprintf(uniq, sizeof uniq, "u%u", d);
    (void)snprintf(body, sizeof body, "%s%s%s", uniq,
                   (d % 5u == 0u) ? " shared" : "", (d % 7u == 0u) ? " mid" : "");
    KBC_CHECK_OK(add(ix, a, d, "kb", "grow.md", "g", body, &err));
    if (kbc_failed(err.status)) {
      break;
    }
  }
  /* The fixture only means anything if it really did force the term table
   * past its initial capacity: the unique term is the rehash driver. */
  KBC_CHECK(kbc_index_term_count(ix) >= GROW_DOCS);
  KBC_CHECK_OK(kbc_index_end_build(ix, &err));
  KBC_CHECK_EQ_INT(kbc_index_doc_count(ix), (int)GROW_DOCS);
  avgdl = kbc_index_avg_doclen(ix);
  KBC_CHECK(avgdl > 0.0);

  for (i = 0; i < sizeof queries / sizeof queries[0]; i++) {
    kbc_status st;
    kbc_hits h = query(ix, a, queries[i].text, 512u, &st);
    size_t expect_n = 0;
    KBC_CHECK_OK(st);
    /* Exactly the documents that contain the query terms — a term whose run
     * absorbed a stranger's postings shows up here — and the same scores a
     * single-threaded reference computes. */
    for (d = 0; d < GROW_DOCS; d++) {
      double want = grow_model_score(d, queries[i].q, queries[i].nq, avgdl,
                                     GROW_DOCS, 1.2, 0.75);
      size_t k;
      bool got = false;
      if (want > 0.0) {
        expect_n++;
      }
      for (k = 0; k < h.len; k++) {
        if (h.items[k].doc == d) {
          got = true;
          KBC_CHECK_EQ_DBL(h.items[k].score, want, 1e-9);
          break;
        }
      }
      KBC_CHECK_EQ_INT(got, want > 0.0);
    }
    KBC_CHECK_EQ_INT((int)h.len, (int)expect_n);
    kbc_hits_free(&h);
  }
  kbc_arena_free(a);
  kbc_index_free(ix);
}

/* ------------------------------------------------- incremental updates --
 * The incremental entry points, declared in include/kbc/index.h. */

/* A corpus as a test can state it: an ordered list of (path, body), with a
 * NULL body meaning "not in the index". The index under test is mutated one
 * document at a time; the model is the same corpus, and a full rebuild of the
 * model is the reference every expectation is compared against. */
typedef struct {
  const char *path;
  const char *body;
} inc_doc;

static const char *const kIncQueries[] = {
    "alpha",  "beta",      "gamma",  "delta",  "epsilon", "alpha beta",
    "zeta",   "theta",     "iota",   "alpha gamma", "kappa", "mu nu xi"};

/* Counts the observable truth about a corpus: distinct terms, (document,
 * distinct term) pairs, and tokens. Derived through the real tokenizer, so a
 * fixture change moves the expectation with it instead of pinning a number. */
typedef struct {
  uint32_t terms;
  uint64_t postings;
  uint64_t tokens;
  uint32_t docs;
} inc_stats;

static void inc_stats_of(const inc_doc *m, size_t n, inc_stats *out) {
  kbc_arena *a = kbc_arena_new(1u << 16);
  kbc_token *seen = (kbc_token *)malloc(((size_t)1u << 17) * sizeof(kbc_token));
  kbc_err err;
  size_t i, j, nseen = 0;
  memset(&err, 0, sizeof err);
  memset(out, 0, sizeof *out);
  for (i = 0; i < n; i++) {
    kbc_tokens t;
    uint32_t distinct = 0;
    size_t start;
    if (m[i].body == NULL) {
      continue;
    }
    tokens_zero(&t);
    (void)kbc_tokenize(a, m[i].body, strlen(m[i].body), &t, &err);
    out->docs++;
    out->tokens += t.len;
    /* `start` is where this document's own terms begin in `seen`: the dedup
     * that counts POSTINGS is per document, and the one that counts TERMS is
     * over the whole corpus. Conflating them undercounts both. */
    start = nseen;
    for (j = 0; j < t.len; j++) {
      size_t k;
      bool known_here = false;
      bool known_at_all = false;
      for (k = 0; k < nseen; k++) {
        if (seen[k].len == t.items[j].len &&
            memcmp(seen[k].text, t.items[j].text, t.items[j].len) == 0) {
          known_at_all = true;
          if (k >= start) {
            known_here = true;
          }
          break;
        }
      }
      if (known_here) {
        continue; /* this document already counts this term */
      }
      distinct++;
      if (!known_at_all && nseen < (((size_t)1u << 17) - 1)) {
        seen[nseen++] = t.items[j];
      }
    }
    out->postings += distinct;
  }
  out->terms = (uint32_t)nseen;
  free(seen);
  kbc_arena_free(a);
}

/* A full rebuild of the model: what kbc_index_end_build produces when the
 * whole corpus arrives in one pass, which is the only thing an incremental
 * index has to agree with. */
static kbc_index *inc_reference(const inc_doc *m, size_t n) {
  kbc_index *ix = kbc_index_new();
  kbc_arena *a = kbc_arena_new(1u << 16);
  kbc_err err;
  uint32_t id = 0;
  size_t i;
  memset(&err, 0, sizeof err);
  (void)kbc_index_begin_build(ix, &err);
  for (i = 0; i < n; i++) {
    if (m[i].body == NULL) {
      continue;
    }
    (void)add(ix, a, id, "kb", m[i].path, m[i].path, m[i].body, &err);
    id++;
  }
  (void)kbc_index_end_build(ix, &err);
  kbc_arena_free(a);
  return ix;
}

/* Byte-for-byte score comparison, per document, against a full rebuild. The
 * two indexes reach the same doubles by the same arithmetic over the same
 * tf / token_count / df / N / avgdl, so anything less than exact equality is a
 * real difference and not a tolerance question. Documents are matched by doc
 * id because a removal COMPACTS: the model order is the doc id order. */
static void inc_compare(kbc_index *ix, kbc_index *ref, size_t nq,
                        const char *what) {
  kbc_arena *a = kbc_arena_new(1u << 16);
  size_t q;
  for (q = 0; q < nq; q++) {
    kbc_status sa, sb;
    size_t i;
    kbc_hits ha = query(ix, a, kIncQueries[q], KBC_MAX_HITS, &sa);
    kbc_hits hb = query(ref, a, kIncQueries[q], KBC_MAX_HITS, &sb);
    KBC_CHECK_MSG(sa == KBC_OK && sb == KBC_OK, "%s: query \"%s\" failed", what,
                  kIncQueries[q]);
    KBC_CHECK_MSG(ha.len == hb.len,
                  "%s: query \"%s\" returned %zu hits, a rebuild of the same "
                  "corpus returns %zu",
                  what, kIncQueries[q], ha.len, hb.len);
    for (i = 0; i < ha.len && i < hb.len; i++) {
      KBC_CHECK_MSG(ha.items[i].doc == hb.items[i].doc,
                    "%s: query \"%s\" hit %zu is doc %u, the rebuild says %u",
                    what, kIncQueries[q], i, ha.items[i].doc, hb.items[i].doc);
      KBC_CHECK_MSG(ha.items[i].score == hb.items[i].score,
                    "%s: query \"%s\" doc %u scored %.17g, the rebuild says "
                    "%.17g",
                    what, kIncQueries[q], ha.items[i].doc, ha.items[i].score,
                    hb.items[i].score);
    }
    kbc_hits_free(&ha);
    kbc_hits_free(&hb);
  }
  kbc_arena_free(a);
}

/* Every observable property an incrementally mutated index has to have after
 * each mutation: the counters, the doc id order, and the scores. */
static void inc_check(kbc_index *ix, const inc_doc *m, size_t n,
                      const char *what) {
  inc_stats st;
  kbc_index *ref = inc_reference(m, n);
  size_t i, nq = sizeof kIncQueries / sizeof kIncQueries[0];
  uint32_t id = 0;

  inc_stats_of(m, n, &st);
  KBC_CHECK_MSG(kbc_index_doc_count(ix) == st.docs,
                "%s: doc_count %u, the corpus holds %u", what,
                kbc_index_doc_count(ix), st.docs);
  KBC_CHECK_MSG(kbc_index_term_count(ix) == st.terms,
                "%s: term_count %u, the corpus has %u distinct terms", what,
                kbc_index_term_count(ix), st.terms);
  /* The ghost-posting detector: a document whose terms shrank, or a removal
   * that missed a term, leaves a posting behind and moves this count. */
  KBC_CHECK_MSG(kbc_index_posting_count(ix) == st.postings,
                "%s: posting_count %llu, the corpus has %llu (document, term) "
                "pairs",
                what, (unsigned long long)kbc_index_posting_count(ix),
                (unsigned long long)st.postings);
  KBC_CHECK_MSG(kbc_index_avg_doclen(ix) ==
                    (st.docs ? (double)st.tokens / (double)st.docs : 0.0),
                "%s: avg_doclen %.17g, the corpus is %llu tokens over %u "
                "documents",
                what, kbc_index_avg_doclen(ix),
                (unsigned long long)st.tokens, st.docs);
  for (i = 0; i < n; i++) {
    const kbc_doc_meta *d;
    if (m[i].body == NULL) {
      continue;
    }
    /* A removed document's postings are gone AND its id is out of range: a
     * hole left behind would divide avg_doclen by the wrong count. */
    KBC_CHECK_MSG(kbc_index_id_of(ix, "kb", m[i].path) == id,
                  "%s: %s has doc id %u, the corpus order says %u", what,
                  m[i].path, kbc_index_id_of(ix, "kb", m[i].path), id);
    d = kbc_index_doc(ix, id);
    KBC_CHECK_NOT_NULL(d);
    if (d != NULL) {
      KBC_CHECK_MSG(strcmp(d->path, m[i].path) == 0,
                    "%s: doc %u is %s, the corpus order says %s", what, id,
                    d->path, m[i].path);
      KBC_CHECK_MSG(d->token_count > 0 || strlen(m[i].body) == 0,
                    "%s: doc %u (%s) has token_count %u", what, id, d->path,
                    d->token_count);
    }
    id++;
  }
  inc_compare(ix, ref, nq, what);
  kbc_index_free(ref);
}

/* The same, but through the file: an incrementally mutated index has to save,
 * reopen, and answer identically from the reopened copy. This is also the
 * only path that exercises an mmap'd index being mutated, which is what a
 * restarted daemon does. */
static void inc_check_roundtrip(kbc_index *ix, const inc_doc *m, size_t n,
                                const char *dir, const char *what) {
  char path[KBC_TEST_PATH_MAX + 32];
  kbc_err err;
  int k = snprintf(path, sizeof path, "%s/inc.idx", dir);
  memset(&err, 0, sizeof err);
  KBC_CHECK(k > 0 && (size_t)k < sizeof path);
  inc_check(ix, m, n, what);
  kbc_err_reset(&err);
  KBC_CHECK_OK(kbc_index_save(ix, path, &err));
  kbc_err_reset(&err);
  kbc_index *reopened = kbc_index_open(path, &err);
  KBC_CHECK_MSG(reopened != NULL, "%s: reopen failed: %s", what, err.msg);
  if (reopened != NULL) {
    inc_check(reopened, m, n, what);
    kbc_index_free(reopened);
  }
}

/* Takes one document out of the model, the way a removal takes it out of the
 * index. */
static void inc_retire(inc_doc *m, size_t n, const char *path) {
  size_t i;
  for (i = 0; i < n; i++) {
    if (strcmp(m[i].path, path) == 0) {
      m[i].body = NULL;
      return;
    }
  }
  KBC_CHECK_MSG(false, "no document %s in the model", path);
}

/* Replaces or appends one document in `ix` through the incremental API, and
 * updates the model to match. */
static void inc_apply(kbc_index *ix, inc_doc *m, size_t *n, const char *path,
                      const char *body, kbc_arena *a, kbc_err *err) {
  kbc_tokens t;
  size_t i;
  uint32_t id = kbc_index_id_of(ix, "kb", path);
  if (id == UINT32_MAX) {
    /* Not in the index: it is a new document, and the model grows at the end
     * because that is where a rebuild puts it. A path the model still carries
     * from an earlier life (remove, then re-add) moves to the end with it —
     * the index appends, so the model has to as well. */
    for (i = 0; i < *n; i++) {
      if (strcmp(m[i].path, path) == 0) {
        if (i + 1 < *n) {
          memmove(&m[i], &m[i + 1], (*n - i - 1) * sizeof m[0]);
        }
        (*n)--;
        break;
      }
    }
    m[*n].path = path;
    m[*n].body = body;
    (*n)++;
  } else {
    for (i = 0; i < *n; i++) {
      if (strcmp(m[i].path, path) == 0) {
        m[i].body = body;
        break;
      }
    }
  }
  tokens_zero(&t);
  KBC_CHECK_OK(tok(a, body, &t, err));
  KBC_CHECK_OK(kbc_index_update_doc(ix, id, "kb", path, path, KBC_KIND_ARTIFACT,
                                    &t, err));
}

/* The sequence the watcher produces over a session, each step checked against
 * a full rebuild of the corpus as it stands at that moment: an edit that adds
 * and drops terms, a brand new document, an edit that leaves a document with
 * almost nothing, a removal in the middle, a removal followed by a re-add, and
 * a document emptied of tokens. The scores must not merely be close to a
 * rebuild's — they must be the same doubles, which is the only way a document
 * that lost a term is distinguishable from one that kept a ghost of it. */
KBC_TEST(incremental_mutations_match_a_full_rebuild) {
  char dir[KBC_TEST_PATH_MAX];
  kbc_test_tmpdir(dir, sizeof dir);
  kbc_index *ix = kbc_index_new();
  kbc_arena *a = kbc_arena_new(1u << 18);
  kbc_err err;
  inc_doc m[16];
  size_t n = 0;
  memset(&err, 0, sizeof err);
  memset(m, 0, sizeof m);

  m[n++] = (inc_doc){"a.md", "alpha alpha beta one two"};
  m[n++] = (inc_doc){"b.md", "beta gamma gamma three"};
  m[n++] = (inc_doc){"c.md", "gamma delta four five"};
  m[n++] = (inc_doc){"d.md", "delta epsilon six seven"};
  m[n++] = (inc_doc){"e.md", "epsilon zeta eight nine"};
  m[n++] = (inc_doc){"f.md", "theta iota kappa ten"};

  KBC_CHECK_OK(kbc_index_begin_build(ix, &err));
  {
    uint32_t id = 0;
    for (size_t i = 0; i < n; i++) {
      KBC_CHECK_OK(add(ix, a, id, "kb", m[i].path, m[i].path, m[i].body, &err));
      id++;
    }
  }
  KBC_CHECK_OK(kbc_index_end_build(ix, &err));
  inc_check(ix, m, n, "fresh build");

  /* An edit: keeps one term, drops two, gains one. */
  inc_apply(ix, m, &n, "a.md", "alpha zeta two", a, &err);
  inc_check_roundtrip(ix, m, n, dir, "edit a.md");

  /* A document the index has never seen. */
  inc_apply(ix, m, &n, "g.md", "mu nu xi kappa", a, &err);
  inc_check_roundtrip(ix, m, n, dir, "add g.md");

  /* An edit that leaves almost nothing: every term this document used to have
   * except one has to leave with it. */
  inc_apply(ix, m, &n, "b.md", "beta", a, &err);
  inc_check_roundtrip(ix, m, n, dir, "shrink b.md");

  /* A removal in the middle of the corpus: the documents after it move down,
   * and everything that moves must still score what it scored. */
  {
    uint32_t id = kbc_index_id_of(ix, "kb", "c.md");
    KBC_CHECK(id != UINT32_MAX);
    inc_retire(m, n, "c.md");
    KBC_CHECK_OK(kbc_index_remove_doc(ix, id, &err));
  }
  inc_check_roundtrip(ix, m, n, dir, "remove c.md");

  /* Remove then re-add: the re-added document is a new document at the end,
   * which is exactly what a rebuild of that corpus would produce. */
  {
    uint32_t id = kbc_index_id_of(ix, "kb", "d.md");
    KBC_CHECK(id != UINT32_MAX);
    inc_retire(m, n, "d.md");
    KBC_CHECK_OK(kbc_index_remove_doc(ix, id, &err));
  }
  inc_check_roundtrip(ix, m, n, dir, "remove d.md");
  inc_apply(ix, m, &n, "d.md", "delta delta zeta eleven", a, &err);
  inc_check_roundtrip(ix, m, n, dir, "re-add d.md");

  /* A document with no tokens at all: recorded, addressable, matching
   * nothing — and contributing no posting. */
  inc_apply(ix, m, &n, "e.md", "", a, &err);
  inc_check_roundtrip(ix, m, n, dir, "empty e.md");

  /* Removing the LAST document renumbers nothing, so the vectors in the rest
   * of the corpus keep their doc ids. */
  {
    uint32_t id = kbc_index_id_of(ix, "kb", "d.md");
    KBC_CHECK(id != UINT32_MAX);
    KBC_CHECK_EQ_INT(id, kbc_index_doc_count(ix) - 1);
    inc_retire(m, n, "d.md");
    KBC_CHECK_OK(kbc_index_remove_doc(ix, id, &err));
  }
  inc_check_roundtrip(ix, m, n, dir, "remove the last document");

  /* And back again, so the corpus is not left in a state only removals
   * produce. */
  inc_apply(ix, m, &n, "d.md", "alpha beta", a, &err);
  inc_check_roundtrip(ix, m, n, dir, "re-add d.md again");

  kbc_arena_free(a);
  kbc_index_free(ix);
  kbc_test_rmrf(dir);
}

/* The counter case, separately: doc_count / term_count / avg_doclen after
 * every single mutation, on a corpus where each document contributes a term
 * nobody else has, so a term left behind by a removal is visible in
 * term_count and not only in the scores. */
KBC_TEST(incremental_counters_track_reality) {
  kbc_index *ix = kbc_index_new();
  kbc_arena *a = kbc_arena_new(1u << 16);
  kbc_err err;
  memset(&err, 0, sizeof err);
  KBC_CHECK_OK(kbc_index_begin_build(ix, &err));
  for (uint32_t d = 0; d < 5u; d++) {
    char body[64], path[16];
    (void)snprintf(body, sizeof body, "only%u common", d);
    (void)snprintf(path, sizeof path, "p%u.md", d);
    KBC_CHECK_OK(add(ix, a, d, "kb", path, path, body, &err));
  }
  KBC_CHECK_OK(kbc_index_end_build(ix, &err));
  KBC_CHECK_EQ_INT(kbc_index_doc_count(ix), 5);
  KBC_CHECK_EQ_INT(kbc_index_term_count(ix), 6); /* five unique + "common" */
  KBC_CHECK_EQ_INT(kbc_index_posting_count(ix), 10);

  /* A replace that drops the document's unique term entirely: the term goes
   * with it, because a rebuild of this corpus would never have made it. */
  {
    kbc_tokens t;
    tokens_zero(&t);
    KBC_CHECK_OK(tok(a, "common common common", &t, &err));
    KBC_CHECK_OK(kbc_index_update_doc(ix, 2, "kb", "p2.md", "p2",
                                      KBC_KIND_ARTIFACT, &t, &err));
  }
  KBC_CHECK_EQ_INT(kbc_index_doc_count(ix), 5);
  KBC_CHECK_EQ_INT(kbc_index_term_count(ix), 5);
  KBC_CHECK_EQ_INT(kbc_index_posting_count(ix), 9);
  /* Ten tokens over five documents, less the two the replaced document had,
   * plus the three it has now. */
  KBC_CHECK_EQ_DBL(kbc_index_avg_doclen(ix), 11.0 / 5.0, 0.0);

  /* A removal takes one document, one posting per term it had, and the
   * documents below it move down by one. The document that moved is still
   * findable, under its new id: a stale id that resolved to the wrong document
   * would be worse than no lookup at all. */
  KBC_CHECK_OK(kbc_index_remove_doc(ix, 0, &err));
  KBC_CHECK_EQ_INT(kbc_index_doc_count(ix), 4);
  KBC_CHECK_EQ_INT(kbc_index_term_count(ix), 4);
  KBC_CHECK_EQ_INT(kbc_index_posting_count(ix), 7);
  KBC_CHECK_EQ_DBL(kbc_index_avg_doclen(ix), 9.0 / 4.0, 0.0);
  KBC_CHECK_EQ_INT(kbc_index_id_of(ix, "kb", "p0.md"), UINT32_MAX);
  KBC_CHECK_EQ_INT(kbc_index_id_of(ix, "kb", "p1.md"), 0);
  KBC_CHECK_EQ_INT(kbc_index_id_of(ix, "kb", "p2.md"), 1);

  /* An append restores the count, and the term it brings is new. */
  {
    kbc_tokens t;
    tokens_zero(&t);
    KBC_CHECK_OK(tok(a, "only0 common", &t, &err));
    KBC_CHECK_OK(kbc_index_update_doc(ix, UINT32_MAX, "kb", "q.md", "q",
                                      KBC_KIND_ARTIFACT, &t, &err));
  }
  KBC_CHECK_EQ_INT(kbc_index_doc_count(ix), 5);
  KBC_CHECK_EQ_INT(kbc_index_term_count(ix), 5);
  KBC_CHECK_EQ_INT(kbc_index_posting_count(ix), 9);

  /* Refusals. An index mid-build is not a live index, a doc id that belongs to
   * another document is a caller bug, and neither may half-apply. */
  {
    kbc_tokens t;
    tokens_zero(&t);
    KBC_CHECK_OK(tok(a, "zzz", &t, &err));
    kbc_index *fresh = kbc_index_new();
    kbc_err_reset(&err);
    KBC_CHECK_OK(kbc_index_begin_build(fresh, &err));
    KBC_CHECK_ERR(kbc_index_update_doc(fresh, UINT32_MAX, "kb", "a.md", "a",
                                       KBC_KIND_ARTIFACT, &t, &err),
                  KBC_ERR_INVALID);
    KBC_CHECK_ERR_MSG(err);
    KBC_CHECK_EQ_INT(kbc_index_doc_count(fresh), 0);
    kbc_index_free(fresh);
    kbc_err_reset(&err);
    KBC_CHECK_ERR(kbc_index_update_doc(ix, 3, "kb", "not-this-one.md", "p",
                                       KBC_KIND_ARTIFACT, &t, &err),
                  KBC_ERR_INVALID);
    KBC_CHECK_ERR_MSG(err);
    KBC_CHECK_EQ_INT(kbc_index_doc_count(ix), 5);
    kbc_err_reset(&err);
    KBC_CHECK_ERR(kbc_index_remove_doc(ix, 99, &err), KBC_ERR_INVALID);
    KBC_CHECK_ERR_MSG(err);
    KBC_CHECK_EQ_INT(kbc_index_doc_count(ix), 5);
    /* kbc_index_add_doc still refuses a live index: the build path has not
     * quietly become a second way to write one. */
    kbc_err_reset(&err);
    KBC_CHECK_ERR(add(ix, a, 5, "kb", "r.md", "r", "common", &err),
                  KBC_ERR_INVALID);
  }

  kbc_arena_free(a);
  kbc_index_free(ix);
}

/* A live index that came from disk — which is what a restarted daemon holds —
 * points its strings into the mapping, and a save writes the heap arenas. The
 * first mutation has to reconcile the two without a rebuild, and the file it
 * writes has to be one kbc_index_open accepts. */
KBC_TEST(an_index_loaded_from_disk_can_be_updated_in_place) {
  char dir[KBC_TEST_PATH_MAX];
  char path[KBC_TEST_PATH_MAX + 32];
  kbc_test_tmpdir(dir, sizeof dir);
  (void)snprintf(path, sizeof path, "%s/loaded.idx", dir);
  kbc_index *ix = kbc_index_new();
  kbc_arena *a = kbc_arena_new(1u << 16);
  kbc_err err;
  inc_doc m[8];
  size_t n = 0;
  memset(&err, 0, sizeof err);
  memset(m, 0, sizeof m);
  m[n++] = (inc_doc){"a.md", "alpha beta gamma"};
  m[n++] = (inc_doc){"b.md", "beta delta epsilon"};

  KBC_CHECK_OK(kbc_index_begin_build(ix, &err));
  for (uint32_t d = 0; d < n; d++) {
    KBC_CHECK_OK(add(ix, a, d, "kb", m[d].path, m[d].path, m[d].body, &err));
  }
  KBC_CHECK_OK(kbc_index_end_build(ix, &err));
  KBC_CHECK_OK(kbc_index_save(ix, path, &err));
  kbc_index_free(ix);

  kbc_err_reset(&err);
  ix = kbc_index_open(path, &err);
  KBC_CHECK_MSG(ix != NULL, "open: %s", err.msg);
  if (ix == NULL) {
    kbc_arena_free(a);
    kbc_test_rmrf(dir);
    return;
 }
  inc_check(ix, m, n, "loaded index");

  /* Edit, add and remove, all on the mapped copy, then save it again. */
  {
    kbc_tokens t;
    tokens_zero(&t);
    KBC_CHECK_OK(tok(a, "alpha zeta", &t, &err));
    KBC_CHECK_OK(kbc_index_update_doc(ix, kbc_index_id_of(ix, "kb", "a.md"),
                                      "kb", "a.md", "a.md", KBC_KIND_ARTIFACT,
                                      &t, &err));
    m[0].body = "alpha zeta";
  }
  inc_check(ix, m, n, "loaded index, edited");

  {
    kbc_tokens t;
    tokens_zero(&t);
    KBC_CHECK_OK(tok(a, "theta iota", &t, &err));
    KBC_CHECK_OK(kbc_index_update_doc(ix, UINT32_MAX, "kb", "c.md", "c.md",
                                      KBC_KIND_ARTIFACT, &t, &err));
    m[n++] = (inc_doc){"c.md", "theta iota"};
  }
  inc_check(ix, m, n, "loaded index, appended");

  {
    uint32_t id = kbc_index_id_of(ix, "kb", "b.md");
    KBC_CHECK(id != UINT32_MAX);
    KBC_CHECK_OK(kbc_index_remove_doc(ix, id, &err));
    m[1].body = NULL;
  }
  inc_check(ix, m, n, "loaded index, removed");

  /* The whole point: the file this daemon is about to serve is one a fresh
   * open accepts, and the reopened copy scores the same. */
  kbc_err_reset(&err);
  KBC_CHECK_OK(kbc_index_save(ix, path, &err));
  kbc_err_reset(&err);
  kbc_index *re = kbc_index_open(path, &err);
  KBC_CHECK_MSG(re != NULL, "reopen after incremental mutation: %s", err.msg);
  if (re != NULL) {
    inc_check(re, m, n, "reopened after incremental mutation");
    kbc_index_free(re);
  }

  kbc_index_free(ix);
  kbc_arena_free(a);
  kbc_test_rmrf(dir);
}

/* The rehash case that already bit the build, driven through the incremental
 * path instead: enough distinct terms to force the term table to grow while
 * an update is in flight, and a term that only the mutated document has. A
 * grow MOVES every live term, so anything the update carries across it must be
 * keyed on the term and not on where the term happened to sit — which is the
 * bug the reference model below is built to catch. */
KBC_TEST(incremental_update_survives_a_term_table_rehash) {
  kbc_index *ix = kbc_index_new();
  kbc_index *ref = kbc_index_new();
  kbc_arena *a = kbc_arena_new(1u << 20);
  kbc_err err;
  char body[128];
  double avgdl;
  uint32_t d;
  size_t i;
  memset(&err, 0, sizeof err);
  KBC_CHECK_OK(kbc_index_begin_build(ix, &err));
  for (d = 0; d < GROW_DOCS; d++) {
    char uniq[24];
    (void)snprintf(uniq, sizeof uniq, "u%u", d);
    (void)snprintf(body, sizeof body, "%s%s%s", uniq,
                   (d % 5u == 0u) ? " shared" : "", (d % 7u == 0u) ? " mid" : "");
    KBC_CHECK_OK(add(ix, a, d, "kb", "grow.md", "g", body, &err));
  }
  KBC_CHECK_OK(kbc_index_end_build(ix, &err));
  /* The fixture is only meaningful if it really did force a grow. */
  KBC_CHECK(kbc_index_term_count(ix) >= GROW_DOCS);

  /* Replace one document with a body that introduces terms the table has
   * never seen: the update grows the table mid-flight. The reference is the
   * same corpus built whole, which is the only way the two doc ids can line
   * up. */
  {
    kbc_tokens t;
    tokens_zero(&t);
    KBC_CHECK_OK(tok(a, "brandnew shared shared freshword", &t, &err));
    KBC_CHECK_OK(kbc_index_update_doc(ix, 3, "kb", "grow.md", "g",
                                      KBC_KIND_ARTIFACT, &t, &err));
  }
  kbc_index_free(ref);
  ref = kbc_index_new();
  KBC_CHECK_OK(kbc_index_begin_build(ref, &err));
  for (d = 0; d < GROW_DOCS; d++) {
    if (d == 3) {
      KBC_CHECK_OK(add(ref, a, d, "kb", "grow.md", "g",
                       "brandnew shared shared freshword", &err));
      continue;
    }
    char uniq[24];
    (void)snprintf(uniq, sizeof uniq, "u%u", d);
    (void)snprintf(body, sizeof body, "%s%s%s", uniq,
                   (d % 5u == 0u) ? " shared" : "", (d % 7u == 0u) ? " mid" : "");
    KBC_CHECK_OK(add(ref, a, d, "kb", "grow.md", "g", body, &err));
  }
  KBC_CHECK_OK(kbc_index_end_build(ref, &err));

  KBC_CHECK_EQ_INT(kbc_index_doc_count(ix), kbc_index_doc_count(ref));
  KBC_CHECK_EQ_INT(kbc_index_term_count(ix), kbc_index_term_count(ref));
  KBC_CHECK_EQ_INT(kbc_index_posting_count(ix), kbc_index_posting_count(ref));
  avgdl = kbc_index_avg_doclen(ix);
  KBC_CHECK(avgdl > 0.0);
  for (i = 0; i < 2; i++) {
    kbc_status sa, sb;
    kbc_hits ha = query(ix, a, i == 0 ? "shared" : "brandnew", GROW_DOCS, &sa);
    kbc_hits hb =
        query(ref, a, i == 0 ? "shared" : "brandnew", GROW_DOCS, &sb);
    size_t k;
    KBC_CHECK_OK(sa);
    KBC_CHECK_OK(sb);
    KBC_CHECK_EQ_INT((int)ha.len, (int)hb.len);
    for (k = 0; k < ha.len && k < hb.len; k++) {
      KBC_CHECK_MSG(ha.items[k].doc == hb.items[k].doc,
                    "query %zu hit %zu: doc %u vs %u", i, k, ha.items[k].doc,
                    hb.items[k].doc);
      KBC_CHECK_MSG(ha.items[k].score == hb.items[k].score,
                    "query %zu doc %u: %.17g vs %.17g", i, ha.items[k].doc,
                    ha.items[k].score, hb.items[k].score);
      /* And against the hand-written model, which knows nothing about how the
       * index is laid out. The two new terms exist only in document 3. */
      if (i == 1) {
        KBC_CHECK_EQ_INT(ha.items[k].doc == 3, 1);
      }
    }
    kbc_hits_free(&ha);
    kbc_hits_free(&hb);
  }

  /* A rehash also has to survive the FILE. The grown table is the one whose
   * slot indices the postings were never keyed on, and the save that follows
   * writes term_cap slots, so a header that counts the table before the grow
   * describes a file the loader walks past the end of. */
  {
    char dir[KBC_TEST_PATH_MAX];
    char path[KBC_TEST_PATH_MAX + 32];
    kbc_test_tmpdir(dir, sizeof dir);
    (void)snprintf(path, sizeof path, "%s/rehash.idx", dir);
    kbc_err_reset(&err);
    KBC_CHECK_OK(kbc_index_save(ix, path, &err));
    kbc_err_reset(&err);
    kbc_index *re = kbc_index_open(path, &err);
    KBC_CHECK_MSG(re != NULL, "reopen after the rehash: %s", err.msg);
    if (re != NULL) {
      KBC_CHECK_EQ_INT(kbc_index_doc_count(re), kbc_index_doc_count(ref));
      KBC_CHECK_EQ_INT(kbc_index_term_count(re), kbc_index_term_count(ref));
      KBC_CHECK_EQ_INT((int)kbc_index_posting_count(re),
                       (int)kbc_index_posting_count(ref));
      KBC_CHECK_EQ_DBL(kbc_index_avg_doclen(re), kbc_index_avg_doclen(ref), 0.0);
      for (i = 0; i < 2; i++) {
        kbc_status sa;
        kbc_hits ha = query(re, a, i == 0 ? "shared" : "brandnew", GROW_DOCS,
                            &sa);
        size_t k;
        KBC_CHECK_OK(sa);
        KBC_CHECK_MSG(ha.len > 0, "reopened index answered nothing for %s",
                      i == 0 ? "shared" : "brandnew");
        for (k = 0; k < ha.len; k++) {
          /* Only the term the replaced document introduced lives in doc 3
           * alone; "shared" is in a third of the corpus. */
          KBC_CHECK_MSG(i != 1 || ha.items[k].doc == 3,
                        "query \"brandnew\" hit doc %u; the term was introduced "
                        "by document 3 alone",
                        ha.items[k].doc);
        }
        kbc_hits_free(&ha);
      }
      kbc_index_free(re);
    }
    kbc_test_rmrf(dir);
  }
  kbc_index_free(ref);
  kbc_arena_free(a);
  kbc_index_free(ix);
}

/* Every document's identity, not just its id: a save/reopen that lost or
 * swapped a title or a kind still passes an id-order check, because the ids
 * are in order either way. */
static void rt_check_meta(kbc_index *ix, const inc_doc *m, size_t n,
                          const char *what) {
  size_t i;
  uint32_t id = 0;
  for (i = 0; i < n; i++) {
    const kbc_doc_meta *d;
    if (m[i].body == NULL) {
      continue;
    }
    d = kbc_index_doc(ix, id);
    KBC_CHECK_NOT_NULL(d);
    if (d == NULL) {
      return;
    }
    KBC_CHECK_MSG(strcmp(d->corpus, "kb") == 0, "%s: doc %u corpus is \"%s\"",
                  what, id, d->corpus);
    KBC_CHECK_MSG(strcmp(d->path, m[i].path) == 0, "%s: doc %u path is \"%s\"",
                  what, id, d->path);
    KBC_CHECK_MSG(strcmp(d->title ? d->title : "", m[i].path) == 0,
                  "%s: doc %u title is \"%s\"", what, id, d->title);
    KBC_CHECK_MSG(d->kind == (uint8_t)KBC_KIND_ARTIFACT,
                  "%s: doc %u kind is %u", what, id, (unsigned)d->kind);
    id++;
  }
}

/* Saves the index, reopens the file, checks the reopened copy against a full
 * rebuild of the same model, and hands BACK the reopened index: the next
 * mutation therefore lands on a mapped copy, which is the state a restarted
 * daemon holds, not the state a freshly built index is in. */
static kbc_index *rt_step(kbc_index *ix, const inc_doc *m, size_t n,
                          const char *path, const char *what) {
  kbc_err err;
  kbc_index *re;
  memset(&err, 0, sizeof err);
  KBC_CHECK_OK(kbc_index_save(ix, path, &err));
  kbc_err_reset(&err);
  re = kbc_index_open(path, &err);
  KBC_CHECK_MSG(re != NULL, "%s: the saved file does not reopen: %s", what,
                err.msg);
  if (re == NULL) {
    kbc_index_free(ix);
    return NULL;
  }
  inc_check(re, m, n, what);
  rt_check_meta(re, m, n, what);
  kbc_index_free(ix);
  return re;
}

/* Save, reopen, save again: the two files must be byte-identical. A second
 * save that differs from the first is the same accounting bug seen from the
 * other side — the header and the sections disagree — and it is invisible to
 * every other check here, because each file opens on its own. */
static void rt_check_stable(kbc_index *ix, const char *dir, const char *what) {
  char a[KBC_TEST_PATH_MAX + 32], b[KBC_TEST_PATH_MAX + 32];
  char *ba = NULL, *bb = NULL;
  size_t la = 0, lb = 0;
  kbc_err err;
  memset(&err, 0, sizeof err);
  (void)snprintf(a, sizeof a, "%s/stable-a.idx", dir);
  (void)snprintf(b, sizeof b, "%s/stable-b.idx", dir);
  KBC_CHECK_OK(kbc_index_save(ix, a, &err));
  kbc_index_free(ix);
  kbc_err_reset(&err);
  kbc_index *re = kbc_index_open(a, &err);
  KBC_CHECK_MSG(re != NULL, "%s: reopen for the stability check: %s", what,
                err.msg);
  if (re == NULL) {
    return;
  }
  kbc_err_reset(&err);
  KBC_CHECK_OK(kbc_index_save(re, b, &err));
  kbc_index_free(re);
  ba = read_bytes(a, &la);
  bb = read_bytes(b, &lb);
  KBC_CHECK_MSG(ba != NULL && bb != NULL && la == lb && la > 0,
                "%s: re-saving an unchanged index produced %zu bytes against "
                "the original's %zu",
                what, lb, la);
  if (ba != NULL && bb != NULL && la == lb) {
    KBC_CHECK_MSG(memcmp(ba, bb, la) == 0,
                  "%s: re-saving an unchanged index produced a DIFFERENT file",
                  what);
  }
  free(ba);
  free(bb);
}

/* The whole class, in one place: whatever sequence of mutations an index has
 * been through, the file it saves has to be one kbc_index_open accepts, it has
 * to decode into the same corpus, and saving it again has to be a no-op.
 *
 * The trigger was a save of an index that was OPENED but never mutated: it
 * keeps its term and doc strings in the mapping and has no heap copy of
 * either, so the writer described both sections in the header and wrote
 * neither — a file shorter than its own header by exactly the two arenas,
 * which is 38% of a 3 MB index and which the daemon's next start rejected.
 * Every step below therefore begins from a MAPPED index. */
KBC_TEST(every_mutation_saves_a_reopenable_stable_file) {
  char dir[KBC_TEST_PATH_MAX];
  char path[KBC_TEST_PATH_MAX + 32];
  static inc_doc m[8];
  kbc_arena *a = kbc_arena_new(1u << 20);
  kbc_err err;
  size_t n = 4;
  kbc_index *ix;
  memset(&err, 0, sizeof err);
  memset(m, 0, sizeof m);
  kbc_test_tmpdir(dir, sizeof dir);
  (void)snprintf(path, sizeof path, "%s/seq.idx", dir);

  /* A corpus small enough that the reference rebuild after every step is
   * cheap. The rehash case needs a real vocabulary to force one and gets its
   * own test, which also has to survive a save. */
  m[0] = (inc_doc){"a.md", "alpha alpha beta one two"};
  m[1] = (inc_doc){"b.md", "beta gamma gamma three"};
  m[2] = (inc_doc){"c.md", "gamma delta four five"};
  m[3] = (inc_doc){"d.md", "delta epsilon six seven"};

  ix = kbc_index_new();
  KBC_CHECK_OK(kbc_index_begin_build(ix, &err));
  for (uint32_t d = 0; d < n; d++) {
    KBC_CHECK_OK(add(ix, a, d, "kb", m[d].path, m[d].path, m[d].body, &err));
  }
  KBC_CHECK_OK(kbc_index_end_build(ix, &err));
  KBC_CHECK_OK(kbc_index_save(ix, path, &err));
  kbc_index_free(ix);

  /* Step 0: no mutation at all. A daemon that starts, sees an event it cannot
   * match, and saves anyway is exactly this call. */
  kbc_err_reset(&err);
  ix = kbc_index_open(path, &err);
  KBC_CHECK_MSG(ix != NULL, "open: %s", err.msg);
  if (ix == NULL) {
    kbc_arena_free(a);
    kbc_test_rmrf(dir);
    return;
  }
  ix = rt_step(ix, m, n, path, "mapped index, unmutated save");
  if (ix == NULL) {
    kbc_arena_free(a);
    kbc_test_rmrf(dir);
    return;
  }
  rt_check_stable(ix, dir, "unmutated");
  kbc_err_reset(&err);
  ix = kbc_index_open(path, &err);
  KBC_CHECK_NOT_NULL(ix);
  if (ix == NULL) {
    kbc_arena_free(a);
    kbc_test_rmrf(dir);
    return;
  }

  /* A replace that keeps one term, drops hundreds and gains one. */
  inc_apply(ix, m, &n, "a.md", "alpha zeta two", a, &err);
  ix = rt_step(ix, m, n, path, "replace a.md");
  if (ix == NULL) goto done;

  /* A replace that SHRINKS the document to almost nothing: every term it used
   * to have except one has to leave with it, and the doc arena it is written
   * from is far smaller than the one it replaces. */
  inc_apply(ix, m, &n, "b.md", "beta", a, &err);
  ix = rt_step(ix, m, n, path, "shrink b.md");
  if (ix == NULL) goto done;

  /* A removal that COMPACTS: everything after the hole moves down, so the doc
   * arena is re-laid in doc-id order and the doc hash is rebuilt. */
  {
    uint32_t id = kbc_index_id_of(ix, "kb", "c.md");
    KBC_CHECK(id != UINT32_MAX);
    inc_retire(m, n, "c.md");
    KBC_CHECK_OK(kbc_index_remove_doc(ix, id, &err));
  }
  ix = rt_step(ix, m, n, path, "remove c.md");
  if (ix == NULL) goto done;

  /* An append AFTER a removal: the new document lands at the new end, and the
   * term arena grows by terms nobody in the corpus has had before. */
  inc_apply(ix, m, &n, "e.md", "mu nu xi kappa", a, &err);
  ix = rt_step(ix, m, n, path, "append e.md after a removal");
  if (ix == NULL) goto done;

  /* A last replace that keeps a term and brings several nobody in the corpus
   * has, so the term table and the term arena both move before the final
   * save. The rehash itself is incremental_update_survives_a_term_table_rehash,
   * which has the vocabulary to force one. */
  inc_apply(ix, m, &n, "d.md", "delta kappa lambda mu", a, &err);
  ix = rt_step(ix, m, n, path, "replace d.md");
  if (ix == NULL) goto done;

  /* And the whole sequence, byte for byte: nothing above corrupted a counter,
   * so nothing above may change the file a second save produces. */
  rt_check_stable(ix, dir, "after the whole sequence");

done:
  kbc_arena_free(a);
  kbc_test_rmrf(dir);
}

/* The same, on a corpus two orders of magnitude larger, because a fix that
 * holds at one size is not a fix: the term arena there spans many blocks and
 * the term table has room to double, and neither may desynchronize the header
 * from the sections. */
KBC_TEST(a_large_corpus_saves_a_reopenable_stable_file) {
  char dir[KBC_TEST_PATH_MAX];
  char path[KBC_TEST_PATH_MAX + 32];
  static char bodies[BIG_DOCS][96];
  static char paths[BIG_DOCS][32];
  static inc_doc m[BIG_DOCS + 4];
  kbc_arena *a = kbc_arena_new(1u << 20);
  kbc_err err;
  size_t n = BIG_DOCS;
  kbc_index *ix = kbc_index_new();
  uint32_t d;
  memset(&err, 0, sizeof err);
  kbc_test_tmpdir(dir, sizeof dir);
  (void)snprintf(path, sizeof path, "%s/big.idx", dir);
  for (d = 0; d < BIG_DOCS; d++) {
    (void)snprintf(paths[d], sizeof paths[d], "corpus/dir%u/doc%04u.md", d % 7u,
                   d);
    (void)snprintf(bodies[d], sizeof bodies[d],
                   "alpha beta gamma delta epsilon zeta theta iota kappa "
                   "doc%u common shared word%u rare%u",
                   d, d % 37u, d % 11u);
    m[d].path = paths[d];
    m[d].body = bodies[d];
  }
  KBC_CHECK_OK(kbc_index_begin_build(ix, &err));
  for (d = 0; d < BIG_DOCS; d++) {
    KBC_CHECK_OK(add(ix, a, d, "kb", paths[d], paths[d], bodies[d], &err));
  }
  KBC_CHECK_OK(kbc_index_end_build(ix, &err));
  KBC_CHECK_OK(kbc_index_save(ix, path, &err));
  kbc_index_free(ix);
  kbc_err_reset(&err);
  ix = kbc_index_open(path, &err);
  KBC_CHECK_MSG(ix != NULL, "open: %s", err.msg);
  if (ix == NULL) {
    kbc_arena_free(a);
    kbc_test_rmrf(dir);
    return;
  }
  /* The corpus is big enough that the arenas span blocks; say so, so a fixture
   * that stops being big fails here rather than passing vacuously. */
  KBC_CHECK(kbc_index_term_count(ix) > 64u);
  KBC_CHECK_MSG(strncmp(dir, "/tmp/kbc-test-", 14) == 0, "GUARD before %s: [%s]", "step", dir);
  ix = rt_step(ix, m, n, path, "large corpus, unmutated save");
  if (ix == NULL) {
    kbc_arena_free(a);
    kbc_test_rmrf(dir);
    return;
  }
  inc_apply(ix, m, &n, paths[3], "replaced body with a handful of new terms",
            a, &err);
  KBC_CHECK_MSG(strncmp(dir, "/tmp/kbc-test-", 14) == 0, "GUARD before %s: [%s]", "step", dir);
  ix = rt_step(ix, m, n, path, "large corpus, one document replaced");
  if (ix == NULL) {
    kbc_arena_free(a);
    kbc_test_rmrf(dir);
    return;
  }
  inc_retire(m, n, paths[100]);
  KBC_CHECK_OK(kbc_index_remove_doc(ix, 100u, &err));
  KBC_CHECK_MSG(strncmp(dir, "/tmp/kbc-test-", 14) == 0, "GUARD before %s: [%s]", "step", dir);
  ix = rt_step(ix, m, n, path, "large corpus, one document removed");
  if (ix == NULL) {
    kbc_arena_free(a);
    kbc_test_rmrf(dir);
    return;
  }
  rt_check_stable(ix, dir, "large corpus");
  kbc_arena_free(a);
  kbc_test_rmrf(dir);
}


/* ------------------------------------------------------------- main ------ */

int main(void) {
  static const kbc_test_case cases[] = {
      {"end_build_survives_a_term_table_rehash",
       end_build_survives_a_term_table_rehash},
      {"incremental_mutations_match_a_full_rebuild",
       incremental_mutations_match_a_full_rebuild},
      {"incremental_counters_track_reality",
       incremental_counters_track_reality},
      {"an_index_loaded_from_disk_can_be_updated_in_place",
       an_index_loaded_from_disk_can_be_updated_in_place},
      {"incremental_update_survives_a_term_table_rehash",
       incremental_update_survives_a_term_table_rehash},
      {"every_mutation_saves_a_reopenable_stable_file",
       every_mutation_saves_a_reopenable_stable_file},
      {"a_large_corpus_saves_a_reopenable_stable_file",
       a_large_corpus_saves_a_reopenable_stable_file},
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
