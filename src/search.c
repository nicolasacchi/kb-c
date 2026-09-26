/* search.c — query parsing, the keyword and vector lanes, and their fusion.
 *
 * A searcher is read-only after construction: it holds borrowed pointers to
 * the index, the (optional) vector store and the resolver, and never mutates
 * any of them. It may therefore be shared across threads as long as the index
 * it points at is immutable — which it is, per the index contract (build ->
 * save -> open, and a running daemon swaps a whole new pointer instead of
 * mutating a live index). Nothing here caches per-query state, so concurrent
 * kbc_search_run calls on one searcher are safe.
 */
#include <stdio.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "kbc/embed.h"
#include "kbc/index.h"
#include "kbc/kbc.h"
#include "kbc/log.h"
#include "kbc/mem.h"
#include "kbc/parse.h"
#include "kbc/search.h"
#include "kbc/types.h"

/* Per-lane depth floor; a caller asking for more still gets at most
 * KBC_MAX_HITS, which is the ceiling the index lane enforces too. */
#define SEARCH_DEFAULT_CANDIDATES 200u
#define SEARCH_DEFAULT_LIMIT 50u
#define SEARCH_DEFAULT_RRF_K 60
/* A trailing `*` is a user asking for completions, not for a corpus dump. */
#define SEARCH_MAX_EXPANSIONS 64u

struct kbc_searcher {
  const kbc_index *ix;    /* BORROWED, immutable after end_build */
  kbc_resolve_fn resolve; /* BORROWED, may be NULL */
  void *resolve_ctx;      /* BORROWED */
  const kbc_vecstore *vs; /* BORROWED, NULL = no vector lane available */
};

/* One fused row, before the resolver and the index doc are consulted. */
typedef struct {
  uint32_t doc;
  double score;
  double kw_score;
  double vec_score;
  uint32_t kw_rank;  /* UINT32_MAX when the lane did not run */
  uint32_t vec_rank; /* UINT32_MAX when the lane did not run */
} fused_row;

/* --------------------------------------------------- lane bookkeeping --- */

typedef struct {
  fused_row *items; /* arena */
  size_t len;
  /* doc ids sorted ascending, parallel to `order`: position in that array is
   * the lane rank, so fusion can binary-search a doc in O(log n). */
  uint32_t *sorted_docs;
  uint32_t *order; /* lane position of sorted_docs[i] */
} lane;

static int cmp_fused_doc(const void *a, const void *b) {
  const fused_row *x = (const fused_row *)a;
  const fused_row *y = (const fused_row *)b;
  if (x->doc == y->doc) return 0;
  return x->doc < y->doc ? -1 : 1;
}

/* One (doc, lane position) pair, sorted by doc. */
typedef struct {
  uint32_t doc;
  uint32_t pos;
} doc_pos;

static int cmp_doc_pos(const void *a, const void *b) {
  const doc_pos *x = (const doc_pos *)a;
  const doc_pos *y = (const doc_pos *)b;
  if (x->doc == y->doc) return 0;
  return x->doc < y->doc ? -1 : 1;
}

static int cmp_fused(const void *a, const void *b) {
  const fused_row *x = (const fused_row *)a;
  const fused_row *y = (const fused_row *)b;
  if (x->score != y->score) return x->score > y->score ? -1 : 1;
  if (x->doc == y->doc) return 0;
  return x->doc < y->doc ? -1 : 1; /* deterministic tie-break */
}

/* Fills `l->sorted_docs`/`l->order`: doc ids ascending, each paired with the
 * position it held in the lane, so a doc's rank is a binary search away. */
static kbc_status lane_index_build(kbc_arena *a, lane *l, kbc_err *err) {
  if (l->len == 0) return KBC_OK;
  if (l->len > UINT32_MAX)
    return kbc_err_set(err, KBC_ERR_INVALID, "lane index: %zu docs", l->len);
  doc_pos *pairs = kbc_arena_alloc(a, l->len * sizeof(doc_pos));
  l->sorted_docs = kbc_arena_alloc(a, l->len * sizeof(uint32_t));
  l->order = kbc_arena_alloc(a, l->len * sizeof(uint32_t));
  if (pairs == NULL || l->sorted_docs == NULL || l->order == NULL)
    return kbc_err_set(err, KBC_ERR_NOMEM, "lane index: %zu docs", l->len);
  for (size_t i = 0; i < l->len; i++) {
    pairs[i].doc = l->items[i].doc;
    pairs[i].pos = (uint32_t)i;
  }
  qsort(pairs, l->len, sizeof(doc_pos), cmp_doc_pos);
  for (size_t i = 0; i < l->len; i++) {
    l->sorted_docs[i] = pairs[i].doc;
    l->order[i] = pairs[i].pos;
  }
  return KBC_OK;
}

/* Returns the lane rank of `doc`, or UINT32_MAX when the lane missed it. */
static uint32_t lane_rank(const lane *l, uint32_t doc) {
  size_t lo = 0, hi = l->len;
  while (lo < hi) {
    const size_t mid = lo + (hi - lo) / 2;
    if (l->sorted_docs[mid] == doc) return l->order[mid];
    if (l->sorted_docs[mid] < doc) {
      lo = mid + 1;
    } else {
      hi = mid;
    }
  }
  return UINT32_MAX;
}

/* --------------------------------------------------------------- modes --- */

const char *kbc_search_mode_str(kbc_search_mode m) {
  switch (m) {
  case KBC_MODE_HYBRID:
    return "hybrid";
  case KBC_MODE_KEYWORD:
    return "keyword";
  case KBC_MODE_SEMANTIC:
    return "semantic";
  }
  return "hybrid";
}

static bool mode_eq(const char *s, const char *want) {
  const size_t n = strlen(want);
  if (strlen(s) != n) return false;
  for (size_t i = 0; i < n; i++) {
    char c = s[i];
    if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
    if (c != want[i]) return false;
  }
  return true;
}

bool kbc_search_mode_parse(const char *s, kbc_search_mode *out) {
  if (s == NULL || out == NULL) return false;
  if (mode_eq(s, "hybrid")) {
    *out = KBC_MODE_HYBRID;
  } else if (mode_eq(s, "keyword") || mode_eq(s, "keyword_only")) {
    *out = KBC_MODE_KEYWORD;
  } else if (mode_eq(s, "semantic") || mode_eq(s, "vector")) {
    *out = KBC_MODE_SEMANTIC;
  } else {
    return false;
  }
  return true;
}

/* ------------------------------------------------------------ filtering -- */

/* The index lane takes no filters, so they are applied to its output before
 * ranks are handed out: a rank is the position in the list the caller asked
 * for, and a corpus filter must not leave holes in it. */
static bool row_passes(const kbc_index *ix, uint32_t doc, const kbc_query *q) {
  const kbc_doc_meta *m = kbc_index_doc(ix, doc);
  if (m == NULL) return false;
  if (q->corpus != NULL && (m->corpus == NULL ||
                            strcmp(m->corpus, q->corpus) != 0)) {
    return false;
  }
  if (q->kind != KBC_KIND__COUNT && (uint8_t)q->kind != m->kind) return false;
  if (q->path_prefix != NULL) {
    const char *path = m->path != NULL ? m->path : "";
    const size_t n = strlen(q->path_prefix);
    if (strncmp(path, q->path_prefix, n) != 0) return false;
  }
  return true;
}

/* --------------------------------------------------------- cosine lane --- */

/* Cosine of two equal-length vectors. Returns false when the row's norm is 0:
 * such a row cannot match anything and must never divide by zero. */
static bool cosine_score(const float *qvec, double qnorm, const float *row,
                         size_t dim, double *out_score) {
  double dot = 0.0, norm = 0.0;
  for (size_t i = 0; i < dim; i++) {
    const double v = (double)row[i];
    dot += (double)qvec[i] * v;
    norm += v * v;
  }
  if (!(norm > 0.0) || !(qnorm > 0.0)) return false;
  *out_score = dot / (sqrt(qnorm) * sqrt(norm));
  return true;
}

kbc_status kbc_cosine_topk(const float *qvec, size_t qdim,
                           const uint32_t *doc_ids, const float *emb, size_t n,
                           size_t dim, size_t limit, kbc_hits *out,
                           kbc_err *err) {
  if (out == NULL)
    return kbc_err_set(err, KBC_ERR_INVALID, "cosine_topk: out is NULL");
  kbc_hits_init(out);
  if (qvec == NULL)
    return kbc_err_set(err, KBC_ERR_INVALID, "cosine_topk: qvec is NULL");
  if (qdim == 0 || dim == 0)
    return kbc_err_set(err, KBC_ERR_INVALID,
                       "cosine_topk: zero dimension (query %zu, table %zu)",
                       qdim, dim);
  /* A dimension mismatch is a caller error, never an out-of-bounds read: the
   * shorter of the two would be read past its end. */
  if (qdim != dim)
    return kbc_err_set(err, KBC_ERR_INVALID,
                       "cosine_topk: query dim %zu != table dim %zu", qdim,
                       dim);
  if (n > 0 && (doc_ids == NULL || emb == NULL))
    return kbc_err_set(err, KBC_ERR_INVALID,
                       "cosine_topk: %zu rows but doc_ids/emb is NULL", n);
  if (n > 0 && (n > SIZE_MAX / dim || n * dim > SIZE_MAX / sizeof(float)))
    return kbc_err_set(err, KBC_ERR_INVALID,
                       "cosine_topk: %zu rows of dim %zu overflows size_t", n,
                       dim);
  if (limit == 0 || limit > KBC_MAX_HITS) limit = KBC_MAX_HITS;

  double qnorm = 0.0;
  for (size_t i = 0; i < qdim; i++) {
    const double v = (double)qvec[i];
    qnorm += v * v;
  }
  /* A zero-norm query vector matches nothing; an empty lane is the honest
   * answer, NaN scores are not. */
  if (!(qnorm > 0.0)) return KBC_OK;

  for (size_t i = 0; i < n; i++) {
    double score = 0.0;
    if (!cosine_score(qvec, qnorm, emb + i * dim, dim, &score)) continue;
    const kbc_status st = kbc_hits_push(out, doc_ids[i], score);
    if (st != KBC_OK) {
      kbc_hits_free(out);
      return kbc_err_set(err, st, "cosine_topk: %zu rows: out of memory", n);
    }
  }
  kbc_hits_sort_desc(out);
  kbc_hits_truncate(out, limit);
  for (size_t i = 0; i < out->len; i++) {
    out->items[i].rank = (uint32_t)i;
    out->items[i].vector_score = out->items[i].score;
  }
  return KBC_OK;
}

/* ------------------------------------------------------------- searcher -- */

kbc_searcher *kbc_searcher_new(const kbc_index *ix, kbc_resolve_fn resolve,
                               void *resolve_ctx, kbc_err *err) {
  if (ix == NULL) {
    kbc_err_set(err, KBC_ERR_INVALID, "searcher_new: index is NULL");
    return NULL;
  }
  kbc_searcher *s = calloc(1, sizeof(*s));
  if (s == NULL) {
    kbc_err_set(err, KBC_ERR_NOMEM, "searcher_new: %zu bytes",
                sizeof(*s));
    return NULL;
  }
  s->ix = ix;
  s->resolve = resolve;
  s->resolve_ctx = resolve_ctx;
  s->vs = NULL;
  return s;
}

/* CONTRACT GAP: search.h gives the searcher no way to learn the document
 * embeddings, so the vector lane needs this setter, declared here because the
 * frozen header cannot be edited by this unit. BORROWS `vs`; NULL removes the
 * lane. The orchestrator should add to search.h:
 *   kbc_status kbc_searcher_set_vecstore(kbc_searcher *s,
 *                                        const kbc_vecstore *vs,
 *                                        kbc_err *err);
 * (needs #include "kbc/embed.h"). */
kbc_status kbc_searcher_set_vecstore(kbc_searcher *s, const kbc_vecstore *vs,
                                     kbc_err *err);

kbc_status kbc_searcher_set_vecstore(kbc_searcher *s, const kbc_vecstore *vs,
                                     kbc_err *err) {
  if (s == NULL) return kbc_err_set(err, KBC_ERR_INVALID, "set_vecstore: s");
  s->vs = vs;
  return KBC_OK;
}

void kbc_searcher_free(kbc_searcher *s) {
  if (s == NULL) return;
  free(s); /* everything else is borrowed */
}

/* ----------------------------------------------------------- query prep -- */

/* Builds the token list the keyword lane consumes: the query's own tokens
 * plus the prefix completions of a trailing `*`. */
static kbc_status expand_query_tokens(kbc_arena *a, const kbc_index *ix,
                                      const kbc_query *q, kbc_tokens *toks,
                                      kbc_tokens *out, kbc_err *err) {
  const size_t qlen = strlen(q->q);
  const bool star = qlen > 0 && q->q[qlen - 1] == '*';

  size_t extra = 0;
  if (star && toks->len > 0) {
    kbc_strlist terms;
    kbc_strlist_init(&terms);
    const char *prefix = toks->items[toks->len - 1].text;
    const kbc_status st = kbc_index_expand_prefix(ix, a, prefix, &terms, err);
    if (st != KBC_OK) {
      kbc_strlist_free(&terms);
      return st;
    }
    if (terms.len > SEARCH_MAX_EXPANSIONS)
      KBC_LOGD("search: prefix expansion capped at %u of %zu terms for \"%s\"",
               SEARCH_MAX_EXPANSIONS, terms.len, prefix);
    extra =
        terms.len < SEARCH_MAX_EXPANSIONS ? terms.len : SEARCH_MAX_EXPANSIONS;
    for (size_t i = 0; i < extra; i++) {
      const char *t = terms.items[i];
      const size_t n = strlen(t);
      kbc_token tok;
      tok.text = t;
      tok.len = n;
      tok.start = 0;
      tok.end = 0;
      memcpy(out->items + toks->len + i, &tok, sizeof(tok));
    }
    out->len = toks->len + extra;
    kbc_strlist_free(&terms);
    return KBC_OK;
  }

  memcpy(out->items, toks->items, toks->len * sizeof(kbc_token));
  out->len = toks->len;
  return KBC_OK;
}

/* ----------------------------------------------------------------- run --- */

kbc_status kbc_search_run(kbc_searcher *s, kbc_arena *a, const kbc_query *q,
                          const float *vec, size_t vec_len,
                          kbc_search_result *out, kbc_err *err) {
  const int64_t started = kbc_now_ns();
  if (s == NULL) return kbc_err_set(err, KBC_ERR_INVALID, "search_run: s");
  if (a == NULL) return kbc_err_set(err, KBC_ERR_INVALID, "search_run: a");
  if (q == NULL) return kbc_err_set(err, KBC_ERR_INVALID, "search_run: q");
  if (out == NULL)
    return kbc_err_set(err, KBC_ERR_INVALID, "search_run: out is NULL");
  memset(out, 0, sizeof(*out));
  if (q->q == NULL || q->q[0] == '\0')
    return kbc_err_set(err, KBC_ERR_INVALID, "search_run: empty query");
  const size_t qlen = strlen(q->q);
  if (qlen > KBC_MAX_QUERY_LEN)
    return kbc_err_set(err, KBC_ERR_INVALID,
                       "search_run: query length %zu exceeds %u", qlen,
                       (unsigned)KBC_MAX_QUERY_LEN);
  if (q->mode != KBC_MODE_HYBRID && q->mode != KBC_MODE_KEYWORD &&
      q->mode != KBC_MODE_SEMANTIC)
    return kbc_err_set(err, KBC_ERR_INVALID, "search_run: mode %d",
                       (int)q->mode);

  size_t limit = q->limit == 0 ? SEARCH_DEFAULT_LIMIT : q->limit;
  if (limit > KBC_MAX_HITS) limit = KBC_MAX_HITS;
  size_t depth = q->candidate_k == 0 ? SEARCH_DEFAULT_CANDIDATES : q->candidate_k;
  if (depth > KBC_MAX_HITS) depth = KBC_MAX_HITS;
  const double rrf_k = q->rrf_k <= 0 ? (double)SEARCH_DEFAULT_RRF_K
                                      : (double)q->rrf_k;
  const double k1 = q->bm25_k1 > 0.0 ? q->bm25_k1 : 1.2;
  const double b = q->bm25_b >= 0.0 ? q->bm25_b : 0.75;

  const bool kw_expected = q->mode != KBC_MODE_SEMANTIC;
  const bool vec_expected = q->mode != KBC_MODE_KEYWORD;

  kbc_tokens toks;
  memset(&toks, 0, sizeof(toks));
  kbc_status st = kbc_tokenize(a, q->q, qlen, &toks, err);
  if (st != KBC_OK) return st;
  if (toks.len == 0) {
    /* Only stopwords and punctuation. Returning "everything" here is how a
     * search starts lying; an empty result is the truth. */
    out->degraded = vec_expected;
    out->took_us = (kbc_now_ns() - started) / 1000;
    return KBC_OK;
  }

  kbc_tokens qt;
  memset(&qt, 0, sizeof(qt));
  qt.items = kbc_arena_calloc(a, toks.len + SEARCH_MAX_EXPANSIONS,
                              sizeof(kbc_token));
  if (qt.items == NULL)
    return kbc_err_set(err, KBC_ERR_NOMEM, "search_run: %zu query tokens",
                       toks.len);
  st = expand_query_tokens(a, s->ix, q, &toks, &qt, err);
  if (st != KBC_OK) return st;

  lane kw, vl;
  memset(&kw, 0, sizeof(kw));
  memset(&vl, 0, sizeof(vl));
  kw.items = kbc_arena_calloc(a, depth, sizeof(fused_row));
  vl.items = kbc_arena_calloc(a, depth, sizeof(fused_row));
  if (kw.items == NULL || vl.items == NULL)
    return kbc_err_set(err, KBC_ERR_NOMEM, "search_run: %zu candidates",
                       depth);

  /* lane 1: keyword --------------------------------------------------- */
  if (kw_expected) {
    kbc_hits hits;
    kbc_hits_init(&hits);
    st = kbc_index_bm25(s->ix, &qt, k1, b, depth, &hits, err);
    if (st != KBC_OK) {
      kbc_hits_free(&hits);
      return st;
    }
    for (size_t i = 0; i < hits.len && kw.len < depth; i++) {
      if (!row_passes(s->ix, hits.items[i].doc, q)) continue;
      fused_row *r = &kw.items[kw.len++];
      r->doc = hits.items[i].doc;
      r->kw_score = hits.items[i].score;
      r->vec_score = 0.0;
      r->score = 0.0;
      r->kw_rank = UINT32_MAX;
      r->vec_rank = UINT32_MAX;
    }
    kbc_hits_free(&hits);
    st = lane_index_build(a, &kw, err);
    if (st != KBC_OK) return st;
  }

  /* lane 2: vector ---------------------------------------------------- */
  bool vector_ran = false;
  double qnorm = 0.0;
  for (size_t i = 0; i < vec_len; i++) {
    const double v = (double)vec[i];
    qnorm += v * v;
  }
  if (!vec_expected) {
    /* keyword-only: the vector is ignored, not degraded. */
  } else if (vec == NULL || vec_len == 0) {
    KBC_LOGD("search: no query embedding; vector lane skipped (mode %s)",
             kbc_search_mode_str(q->mode));
  } else if (!(qnorm > 0.0)) {
    KBC_LOGD("search: query embedding has zero norm; vector lane empty");
  } else if (s->vs == NULL) {
    KBC_LOGD("search: no vector store; vector lane skipped (mode %s)",
             kbc_search_mode_str(q->mode));
  } else if (kbc_vecstore_dim(s->vs) != vec_len) {
    KBC_LOGD("search: embedding dim %zu != store dim %zu; vector lane skipped",
             vec_len, kbc_vecstore_dim(s->vs));
  } else {
    const size_t dim = vec_len;
    vector_ran = true;
    kbc_hits hits;
    kbc_hits_init(&hits);
    if (q->mode == KBC_MODE_SEMANTIC) {
      /* No keyword shortlist to narrow the scan, so walk the whole store
       * rather than materializing it: a 100k-doc store is 150 MB of floats
       * and the daemon must not allocate that per request. */
      const uint32_t count = kbc_vecstore_count(s->vs);
      for (uint32_t i = 0; i < count; i++) {
        const float *row = kbc_vecstore_get(s->vs, i);
        double score = 0.0;
        if (row == NULL) continue;
        if (!cosine_score(vec, qnorm, row, dim, &score)) continue;
        if (!row_passes(s->ix, i, q)) continue;
        const kbc_status ps = kbc_hits_push(&hits, i, score);
        if (ps != KBC_OK) {
          kbc_hits_free(&hits);
          return kbc_err_set(err, ps, "search: %u docs: out of memory",
                             (unsigned)count);
        }
      }
      kbc_hits_sort_desc(&hits);
      kbc_hits_truncate(&hits, depth);
    } else {
      /* Hybrid: only the keyword shortlist can contribute, so gather just
       * those rows (depth * dim floats, bounded by KBC_MAX_HITS). */
      uint32_t ids[KBC_MAX_HITS];
      size_t n = 0;
      const uint32_t count = kbc_vecstore_count(s->vs);
      for (size_t i = 0; i < kw.len && n < KBC_MAX_HITS; i++) {
        if (kw.items[i].doc < count) ids[n++] = kw.items[i].doc;
      }
      if (n > 0) {
        const size_t nfloats = n * dim;
        if (nfloats > SIZE_MAX / sizeof(float))
          return kbc_err_set(err, KBC_ERR_INVALID,
                             "search: %zu rows of dim %zu overflows size_t", n,
                             dim);
        float *rows = kbc_arena_alloc(a, nfloats * sizeof(float));
        if (rows == NULL)
          return kbc_err_set(err, KBC_ERR_NOMEM, "search: %zu floats",
                             nfloats);
        st = kbc_vecstore_gather(s->vs, ids, n, a, rows, err);
        if (st != KBC_OK) {
          kbc_hits_free(&hits);
          return st;
        }
        st = kbc_cosine_topk(vec, dim, ids, rows, n, dim, depth, &hits, err);
        if (st != KBC_OK) {
          kbc_hits_free(&hits);
          return st;
        }
      }
    }
    for (size_t i = 0; i < hits.len && vl.len < depth; i++) {
      fused_row *r = &vl.items[vl.len++];
      r->doc = hits.items[i].doc;
      r->vec_score = hits.items[i].score;
      r->kw_score = 0.0;
      r->score = 0.0;
      r->kw_rank = UINT32_MAX;
      r->vec_rank = UINT32_MAX;
    }
    kbc_hits_free(&hits);
    st = lane_index_build(a, &vl, err);
    if (st != KBC_OK) return st;
  }

  /* fusion ------------------------------------------------------------- */
  const size_t total = kw.len + vl.len;
  fused_row *rows =
      kbc_arena_calloc(a, total > 0 ? total : 1, sizeof(fused_row));
  if (rows == NULL)
    return kbc_err_set(err, KBC_ERR_NOMEM, "search: %zu fused rows", total);
  size_t n = 0;
  for (size_t i = 0; i < kw.len; i++) {
    rows[n++] = kw.items[i];
    rows[n - 1].kw_rank = (uint32_t)i;
  }
  for (size_t i = 0; i < vl.len; i++) {
    rows[n++] = vl.items[i];
    rows[n - 1].vec_rank = (uint32_t)i;
  }
  /* Union by doc id, then score each doc from the ranks both lanes gave it. */
  qsort(rows, n, sizeof(fused_row), cmp_fused_doc);
  size_t uniq = 0;
  for (size_t i = 0; i < n; i++) {
    if (uniq > 0 && rows[uniq - 1].doc == rows[i].doc) {
      /* Merge: keep the higher score per lane field, union the ranks. */
      fused_row *keep = &rows[uniq - 1];
      if (rows[i].kw_rank != UINT32_MAX) {
        keep->kw_rank = rows[i].kw_rank;
        keep->kw_score = rows[i].kw_score;
      }
      if (rows[i].vec_rank != UINT32_MAX) {
        keep->vec_rank = rows[i].vec_rank;
        keep->vec_score = rows[i].vec_score;
      }
      continue;
    }
    rows[uniq++] = rows[i];
  }
  for (size_t i = 0; i < uniq; i++) {
    fused_row *r = &rows[i];
    r->kw_rank = lane_rank(&kw, r->doc);
    r->vec_rank = lane_rank(&vl, r->doc);
    r->score = 0.0;
    if (r->kw_rank != UINT32_MAX) r->score += 1.0 / (rrf_k + (double)r->kw_rank);
    if (r->vec_rank != UINT32_MAX) r->score += 1.0 / (rrf_k + (double)r->vec_rank);
  }
  qsort(rows, uniq, sizeof(fused_row), cmp_fused);

  out->candidates = uniq;
  out->vector_ran = vector_ran;
  out->degraded = vec_expected &&
                   (!vector_ran || (q->mode == KBC_MODE_SEMANTIC && uniq == 0));

  /* resolve ------------------------------------------------------------ */
  kbc_result_row *dst = kbc_arena_calloc(a, limit, sizeof(kbc_result_row));
  if (dst == NULL)
    return kbc_err_set(err, KBC_ERR_NOMEM, "search: %zu rows", limit);
  size_t kept = 0;
  for (size_t i = 0; i < uniq && kept < limit; i++) {
    const fused_row *r = &rows[i];
    const char *id = NULL;
    const char *summary = NULL;
    if (s->resolve != NULL) {
      const kbc_status rs =
          s->resolve(s->resolve_ctx, a, r->doc, &id, &summary);
      if (rs == KBC_ERR_NOTFOUND) {
        /* The index may be one reindex ahead of the store: a doc the store
         * no longer knows is a dropped row, not a failed search. */
        KBC_LOGD("search: doc %u not in store, dropping", r->doc);
        continue;
      }
      if (rs != KBC_OK) return rs;
    }
    const kbc_doc_meta *m = kbc_index_doc(s->ix, r->doc);
    if (m == NULL) {
      KBC_LOGD("search: doc %u out of range, dropping", r->doc);
      continue;
    }
    kbc_result_row *row = &dst[kept++];
    row->doc_id = r->doc;
    row->score = r->score;
    row->keyword_score = r->kw_score;
    row->vector_score = r->vec_score;
    row->keyword_rank = r->kw_rank;
    row->vector_rank = r->vec_rank;
    row->artifact_id = id;
    row->summary = summary;
    row->corpus = m->corpus != NULL ? kbc_arena_strdup(a, m->corpus) : NULL;
    row->path = m->path != NULL ? kbc_arena_strdup(a, m->path) : NULL;
    row->title = m->title != NULL ? kbc_arena_strdup(a, m->title) : NULL;
  }
  out->rows = dst;
  out->len = kept;
  out->cap = limit;
  out->took_us = (kbc_now_ns() - started) / 1000;
  return KBC_OK;
}
