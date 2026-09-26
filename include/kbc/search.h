/* search.h — query parsing, the two lanes, and their fusion.
 *
 * Lane 1 (keyword): kbc_index_bm25 over the query tokens. Always available.
 * Lane 2 (vector):  cosine over the embedding the sidecar returns for the
 *                   query. Present only when an embedder is configured and
 *                   reachable; its absence downgrades `hybrid` to keyword
 *                   rather than failing the request.
 * Fusion: reciprocal rank fusion, score = Σ_lane 1 / (rrf_k + rank_lane).
 *         rrf_k defaults to 60, matching the Rust daemon.
 */
#ifndef KBC_SEARCH_H
#define KBC_SEARCH_H

#include "kbc/embed.h"
#include "kbc/index.h"
#include "kbc/kbc.h"
#include "kbc/mem.h"
#include "kbc/parse.h"
#include "kbc/types.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
  KBC_MODE_HYBRID = 0,
  KBC_MODE_KEYWORD = 1,
  KBC_MODE_SEMANTIC = 2
} kbc_search_mode;

const char *kbc_search_mode_str(kbc_search_mode m);
bool kbc_search_mode_parse(const char *s, kbc_search_mode *out);

typedef struct {
  const char *q;        /* the raw query text, required, non-empty */
  const char *corpus;   /* filter, NULL = every corpus */
  const char *path_prefix; /* filter, NULL = any */
  kbc_kind kind;        /* KBC_KIND__COUNT = every kind */
  kbc_search_mode mode;
  size_t limit;         /* 0 -> config default; clamped to KBC_MAX_HITS */
  size_t candidate_k;   /* 0 -> 200; per-lane depth before fusion */
  int rrf_k;            /* <= 0 -> 60 */
  double bm25_k1, bm25_b;

  /* In-degree (backlink count) per index doc id, parallel to `in_deg_doc_ids`,
   * or NULL for no graph boost. KBC_ARENA / BORROWED, supplied by the caller,
   * which is the only layer that can see the edge table. Applied AFTER fusion
   * and BEFORE filtering and truncation, exactly as the Rust daemon does:
   *     score += weight * (1.0 / rrf_k) * sqrt(in) / sqrt(in_max)
   * sqrt and not ln, deliberately: the Rust daemon chose it so the value is
   * bit-identical across libm implementations. weight 0.0 disables it, as
   * apply_graph_boost's own early return does. */
  double graph_boost_weight;
  const uint32_t *in_deg_doc_ids; /* parallel to in_deg */
  const uint32_t *in_deg;
  size_t in_deg_len;

  /* Drop rows whose artifact is older than this (nanoseconds since the epoch).
   * 0 disables the filter. Applied by the caller, which is where the store's
   * mtime lives — the index deliberately does not carry it. */
  int64_t since_ns;
} kbc_query;

/* One result row. Strings are ARENA copies taken from the index (and, for
 * artifact_id/summary, from the resolver) so the row outlives any lock. */
typedef struct {
  uint32_t doc_id;
  double score;        /* the fused score actually used for ordering */
  double keyword_score;
  double vector_score;
  uint32_t keyword_rank; /* 0-based; UINT32_MAX when the lane did not run */
  uint32_t vector_rank;  /* 0-based; UINT32_MAX when the lane did not run */
  const char *artifact_id; /* ARENA, 12 hex; NULL if the resolver missed it */
  const char *corpus;      /* ARENA */
  const char *path;        /* ARENA */
  const char *title;       /* ARENA */
  const char *summary;     /* ARENA, may be NULL */
} kbc_result_row;

typedef struct {
  kbc_result_row *rows; /* KBC_ARENA */
  size_t len, cap;
  size_t candidates;    /* rows the lanes produced before the limit */
  int64_t took_us;
  bool vector_ran;
  bool degraded;        /* true when a lane was skipped (see the log line) */
} kbc_search_result;

typedef struct kbc_searcher kbc_searcher;

/* Resolves an index doc id to the artifact's 12-hex id (and, optionally, a
 * summary). Implemented by kbc_app over SQLite. Returning KBC_ERR_NOTFOUND
 * drops the row rather than failing the whole search: the index may be one
 * reindex ahead of the store. */
typedef kbc_status (*kbc_resolve_fn)(void *ctx, kbc_arena *a, uint32_t doc_id,
                                     const char **artifact_id,
                                     const char **summary);

/* BORROWS `ix`; the searcher does not own it. */
kbc_searcher *kbc_searcher_new(const kbc_index *ix, kbc_resolve_fn resolve,
                               void *resolve_ctx, kbc_err *err);
void kbc_searcher_free(kbc_searcher *s);

/* Hands the searcher the document embeddings it scores the vector lane
 * against. BORROWS `vs`; pass NULL to take the vector lane off. Without one,
 * `hybrid` degrades to keyword and `semantic` returns nothing — both set
 * out->degraded, neither is an error. */
kbc_status kbc_searcher_set_vecstore(kbc_searcher *s, const kbc_vecstore *vs,
                                     kbc_err *err);

/* `vec` is the query embedding (may be NULL, or length 0 = lane skipped).
 * The result is ARENA-scoped. `q->limit` rows at most. */
kbc_status kbc_search_run(kbc_searcher *s, kbc_arena *a, const kbc_query *q,
                          const float *vec, size_t vec_len,
                          kbc_search_result *out, kbc_err *err);

/* Parses a `since:` atom's value into a nanosecond cutoff, using the forms the
 * Rust overlay accepts: `<n>d` (days), `<n>h` (hours), or a raw unix timestamp
 * in SECONDS. The unit count is clamped rather than allowed to overflow.
 *
 * `value` is BORROWED; `*ns` is 0 when there is no filter. `all` — the Rust
 * meaning for "drop the atom" — is an ERROR here (KBC_ERR_INVALID), as is an
 * empty value, an unknown unit, or trailing junk. It is never a silent "no
 * filter": a `since:` the caller believes it applied must not quietly not
 * apply. */
kbc_status kbc_since_value_ns(const char *value, int64_t *ns, kbc_err *err);

/* Cosine top-k over a caller-supplied (doc_id, embedding) table. Exposed so
 * the vector lane is testable without a sidecar.
 *
 * Unlike kbc_index_bm25, this one REPLACES rather than appends: it calls
 * kbc_hits_init(out) on entry, which zeroes the struct without releasing
 * whatever buffer it already held. So a caller that reuses a kbc_hits across
 * calls must kbc_hits_free it in between, or the first result leaks. The
 * asymmetry is deliberate — the caller owns the hits it passed to bm25 and
 * keeps appending to them, while a top-k is a complete answer of its own. */
kbc_status kbc_cosine_topk(const float *qvec, size_t qdim,
                           const uint32_t *doc_ids, const float *emb, size_t n,
                           size_t dim, size_t limit, kbc_hits *out,
                           kbc_err *err);

#ifdef __cplusplus
}
#endif

#endif /* KBC_SEARCH_H */
