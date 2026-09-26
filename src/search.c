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
/* Bounded top-k accumulator. It keeps the best `cap` rows seen so far in a
 * heap whose root is the WORST of them (score asc, then doc desc), so a
 * candidate either joins the set or is dropped in O(log cap) with no memory
 * beyond the `cap` slots the caller supplies. Because (score, doc) is a
 * total order, the survivors and their final order are exactly what a
 * gather-everything-then-sort pass produces — same rows, same ranking — but
 * the peak footprint is O(limit) instead of O(corpus), which is what lets a
 * semantic query over a 100k-doc store run inside a small request arena. */
typedef struct {
  kbc_hit *items; /* BORROWED storage, not owned by the heap */
  size_t len;
  size_t cap;
} topk_heap;

/* True when `a` should be evicted before `b` in the vector lane: a strictly
 * lower score, or the same score with the larger doc id (the deterministic
 * tie-break the lanes already sort by). */
static bool hit_worse(const kbc_hit *a, const kbc_hit *b) {
  if (a->score != b->score) return a->score < b->score;
  return a->doc > b->doc;
}

static void topk_sift_up(topk_heap *h, size_t i) {
  while (i > 0) {
    const size_t parent = (i - 1) / 2;
    if (!hit_worse(&h->items[i], &h->items[parent])) break;
    const kbc_hit t = h->items[i];
    h->items[i] = h->items[parent];
    h->items[parent] = t;
    i = parent;
  }
}

static void topk_sift_down(topk_heap *h, size_t i) {
  for (;;) {
    const size_t l = 2 * i + 1, r = l + 1;
    size_t worst = i;
    if (l < h->len && hit_worse(&h->items[l], &h->items[worst])) worst = l;
    if (r < h->len && hit_worse(&h->items[r], &h->items[worst])) worst = r;
    if (worst == i) return;
    const kbc_hit t = h->items[i];
    h->items[i] = h->items[worst];
    h->items[worst] = t;
    i = worst;
  }
}

/* Folds one scored row in. Never fails and never allocates: the row is
 * dropped unless it beats the current worst survivor. */
static void topk_push(topk_heap *h, uint32_t doc, double score) {
  kbc_hit c;
  c.doc = doc;
  c.score = score;
  c.rank = 0;
  c.vector_score = 0.0;
  if (h->len < h->cap) {
    h->items[h->len++] = c;
    topk_sift_up(h, h->len - 1);
    return;
  }
  if (h->cap == 0 || !hit_worse(&h->items[0], &c)) return;
  h->items[0] = c;
  topk_sift_down(h, 0);
}

/* Moves the kept rows out in lane order: score desc, doc asc, ranks handed
 * out by position. `out` takes ownership of `storage`. */
static void topk_finish(topk_heap *h, kbc_hit *storage, size_t limit,
                        kbc_hits *out) {
  kbc_hits_init(out);
  out->items = storage;
  out->len = h->len;
  out->cap = h->len;
  kbc_hits_sort_desc(out);
  kbc_hits_truncate(out, limit);
  for (size_t i = 0; i < out->len; i++) {
    out->items[i].rank = (uint32_t)i;
    out->items[i].vector_score = out->items[i].score;
  }
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

  /* Streaming: `limit` slots, never `n`. A store larger than KBC_MAX_HITS
   * used to fail here, because every scored row was appended to `out`
   * before the sort. */
  kbc_hit *storage = malloc(limit * sizeof(*storage));
  if (storage == NULL)
    return kbc_err_set(err, KBC_ERR_NOMEM, "cosine_topk: %zu slots", limit);
  topk_heap heap;
  heap.items = storage;
  heap.len = 0;
  heap.cap = limit;
  for (size_t i = 0; i < n; i++) {
    double score = 0.0;
    if (!cosine_score(qvec, qnorm, emb + i * dim, dim, &score)) continue;
    topk_push(&heap, doc_ids[i], score);
  }
  topk_finish(&heap, storage, limit, out);
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

/* ======================================================== query grammar == */

/* A port of the slice of kb-core/src/query.rs that the kb-c index can
 * actually answer, and nothing more.
 *
 * Where the grammar lives in the original matters. The kb SEARCH route hands
 * `?q=` straight to BM25 (crates/kb-server/src/routes/search.rs:1163) and
 * never parses it; query.rs is a GALLERY filter overlay that
 * routes/docs.rs:620 layers on top of the flat ?tags=&folder= params. So in
 * the Rust build the boolean structure of a search query has no effect on the
 * lexical arm at all — the free text is collected across the whole
 * expression into ONE space-joined string and scored once (query.rs:419),
 * and the AND/OR/NOT tree only ever shapes facet predicates.
 *
 * kb-c is the lexical arm, so that is exactly what this implements: pull the
 * facet atoms out of the string, score the remaining text, and refuse —
 * loudly — every atom the index cannot evaluate. The failure this exists to
 * prevent is a user typing `tag:rust` and getting a search for the literal
 * words "tag" and "rust": a query that silently means something else is
 * worse than an error.
 *
 * Ported shape, from query.rs: a tokenizer (:580) with `"…"` values and `\`
 * escapes, `AND`/`and`/`&&`, `OR`/`or`/`||`, `NOT`/`not`/`!`, `(…)` groups
 * and implicit AND between adjacent terms; recursive descent at
 * MAX_PARSE_DEPTH 64 (:532); a bareword with no colon becomes free text
 * (:944); De Morgan, which in disjunctive normal form is exactly "flip every
 * literal"; MAX_DNF_CONJUNCTS 64 with the original's warning text (:305).
 * There is no phrase search — the original has none either; see the report. */

#define GRAM_MAX_DEPTH 64      /* query.rs:532 MAX_PARSE_DEPTH */
#define GRAM_MAX_CONJUNCTS 64  /* query.rs:305 MAX_DNF_CONJUNCTS */
/* The cartesian product in the AND arm is what makes a DNF blow up; the
 * conjunct cap bounds it, this bounds the literals behind them. */
#define GRAM_MAX_FACTS 1024

typedef enum {
  GK_TEXT,
  GK_FOLDER,
  GK_TAG,
  GK_CAP,
  GK_SINCE,
  GK_INDEX,
  GK_SCOPE,
  GK_OTHER
} gram_key;

static const char *gram_key_name(gram_key k) {
  switch (k) {
  case GK_TEXT: return "text";
  case GK_FOLDER: return "folder";
  case GK_TAG: return "tag";
  case GK_CAP: return "cap";
  case GK_SINCE: return "since";
  case GK_INDEX: return "index";
  case GK_SCOPE: return "scope";
  case GK_OTHER: return NULL;
  }
  return NULL;
}

typedef enum {
  GT_WORD,
  GT_QUOTED,
  GT_COLON,
  GT_LPAREN,
  GT_RPAREN,
  GT_AND,
  GT_OR,
  GT_NOT
} gram_tok_kind;

typedef struct {
  gram_tok_kind kind;
  const char *text; /* arena; non-NULL for GT_WORD/GT_QUOTED only */
} gram_tok;

typedef struct {
  gram_key key;
  const char *name;  /* arena, the bareword as typed — for the error text */
  const char *value; /* arena */
  bool negated;
  /* Index of the atom this literal was first parsed as. The DNF lowering
   * copies literals into every conjunct that contains them, so the pool holds
   * N copies of the atom the user typed once; the fold below reads each atom
   * exactly once, at its own index, which is document order. */
  size_t origin;
} gram_fact;

/* One conjunct is a run of literals in the shared fact pool. */
typedef struct {
  size_t start, len;
} gram_span;

typedef struct {
  gram_span *items; /* arena */
  size_t len, cap;
} gram_list;

typedef struct {
  kbc_arena *a;
  const gram_tok *toks;
  size_t ntok, pos;
  gram_fact *facts; /* arena, GRAM_MAX_FACTS entries */
  size_t nfacts;
  bool capped; /* the OR expansion hit GRAM_MAX_CONJUNCTS */
  kbc_status status;
  char msg[KBC_ERR_MSG_MAX];
} gram_parser;

static bool gram_fail(gram_parser *p, kbc_status st, const char *fmt, ...)
    __attribute__((format(printf, 3, 4)));

static bool gram_fail(gram_parser *p, kbc_status st, const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(p->msg, sizeof(p->msg), fmt, ap);
  va_end(ap);
  p->status = st;
  return false;
}

static bool g_ascii_space(unsigned char c) {
  return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\v' ||
         c == '\f';
}

static bool g_word_delim(unsigned char c) {
  return g_ascii_space(c) || c == ':' || c == '(' || c == ')';
}

static char g_lower(char c) {
  return (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
}

/* ASCII alphanumerics, plus every byte of a multi-byte UTF-8 sequence: the
 * original splits on `!char::is_alphanumeric`, so a non-ASCII letter is part
 * of a term there and must be part of one here. `isalnum()` is
 * locale-dependent and would not say that. */
static bool g_alnum(unsigned char c) {
  return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') ||
         (c >= 'A' && c <= 'Z') || c >= 0x80;
}

static bool g_keyword_is(const char *s, const char *want) {
  size_t i = 0;
  for (; s[i] != '\0' && want[i] != '\0'; i++) {
    if (g_lower(s[i]) != want[i]) return false;
  }
  return s[i] == '\0' && want[i] == '\0';
}

/* query.rs:65 Key::from_lex — `caps` aliases `cap`, `q` aliases `text`, and
 * an unrecognised bareword becomes Key::Other rather than being dropped. */
static gram_key gram_key_of(const char *s) {
  if (g_keyword_is(s, "tag")) return GK_TAG;
  if (g_keyword_is(s, "folder")) return GK_FOLDER;
  if (g_keyword_is(s, "cap") || g_keyword_is(s, "caps")) return GK_CAP;
  if (g_keyword_is(s, "since")) return GK_SINCE;
  if (g_keyword_is(s, "index")) return GK_INDEX;
  if (g_keyword_is(s, "scope")) return GK_SCOPE;
  if (g_keyword_is(s, "text") || g_keyword_is(s, "q")) return GK_TEXT;
  return GK_OTHER;
}

/* 1 = appended, 0 = dropped because the conjunct cap is reached (the caller
 * flags the query capped), -1 = out of memory. */
static int span_push(gram_parser *p, gram_list *l, gram_span s) {
  if (l->len == l->cap) {
    size_t ncap = l->cap != 0 ? l->cap * 2 : 8;
    if (ncap > GRAM_MAX_CONJUNCTS) ncap = GRAM_MAX_CONJUNCTS;
    if (l->len == ncap) return 0;
    gram_span *ni = kbc_arena_alloc(p->a, ncap * sizeof(gram_span));
    if (ni == NULL) return -1;
    if (l->len > 0) memcpy(ni, l->items, l->len * sizeof(gram_span));
    l->items = ni;
    l->cap = ncap;
  }
  l->items[l->len++] = s;
  return 1;
}

static kbc_status gram_tokenize(kbc_arena *a, const char *s, size_t n,
                                gram_tok **out, size_t *out_n, kbc_err *err) {
  /* One token per input byte is an upper bound: every token consumes at
   * least one byte and none is ever dropped. */
  gram_tok *toks = kbc_arena_alloc(a, (n + 2) * sizeof(gram_tok));
  if (toks == NULL)
    return kbc_err_set(err, KBC_ERR_NOMEM, "grammar: %zu tokens", n + 2);
  size_t len = 0, i = 0;
  while (i < n) {
    const unsigned char b = (unsigned char)s[i];
    if (g_ascii_space(b)) {
      i++;
      continue;
    }
    if (b == '(' || b == ')' || b == ':') {
      toks[len].kind =
          b == '(' ? GT_LPAREN : (b == ')' ? GT_RPAREN : GT_COLON);
      toks[len].text = NULL;
      len++;
      i++;
      continue;
    }
    if (b == '"') {
      i++;
      kbc_str v;
      kbc_str_init(&v);
      bool closed = false, ok = true;
      size_t start = i;
      while (i < n) {
        if (s[i] == '\\' && i + 1 < n) {
          if (kbc_str_append(&v, s + start, i - start) != KBC_OK) {
            ok = false;
            break;
          }
          /* An escaped unit is one whole UTF-8 character, so slicing on the
           * byte after the backslash would mojibake a multi-byte char. */
          size_t cl = kbc_utf8_len((unsigned char)s[i + 1]);
          if (cl == 0 || i + 1 + cl > n) cl = 1;
          if (kbc_str_append(&v, s + i + 1, cl) != KBC_OK) {
            ok = false;
            break;
          }
          i += 1 + cl;
          start = i;
        } else if (s[i] == '"') {
          if (kbc_str_append(&v, s + start, i - start) != KBC_OK) {
            ok = false;
            break;
          }
          i++;
          closed = true;
          break;
        } else {
          i++;
        }
      }
      if (ok) {
        toks[len].kind = GT_QUOTED;
        toks[len].text = kbc_arena_strdup(a, v.ptr != NULL ? v.ptr : "");
        len++;
      }
      if (v.ptr != NULL) kbc_str_free(&v);
      if (!ok) return kbc_err_set(err, KBC_ERR_NOMEM, "grammar: quoted value");
      if (!closed)
        return kbc_err_set(err, KBC_ERR_PARSE,
                           "grammar: unterminated quoted string");
      continue;
    }
    const size_t start = i;
    while (i < n && !g_word_delim((unsigned char)s[i])) i++;
    const char *w = kbc_arena_strndup(a, s + start, i - start);
    gram_tok_kind k = GT_WORD;
    /* query.rs:592 matches the WHOLE bareword, so "And" stays a word. */
    if (strcmp(w, "AND") == 0 || strcmp(w, "and") == 0 ||
        strcmp(w, "&&") == 0) {
      k = GT_AND;
    } else if (strcmp(w, "OR") == 0 || strcmp(w, "or") == 0 ||
               strcmp(w, "||") == 0) {
      k = GT_OR;
    } else if (strcmp(w, "NOT") == 0 || strcmp(w, "not") == 0 ||
               strcmp(w, "!") == 0) {
      k = GT_NOT;
    }
    toks[len].kind = k;
    toks[len].text = w;
    len++;
  }
  *out = toks;
  *out_n = len;
  return KBC_OK;
}

static bool parse_expr_or(gram_parser *p, size_t depth, gram_list *out);
static bool parse_expr_and(gram_parser *p, size_t depth, gram_list *out);

static bool parse_expr_term(gram_parser *p, size_t depth, gram_list *out) {
  /* Each `(` group and each `NOT` descends one level; the query string is
   * request-supplied, so the depth is a stack bound, not a style rule. */
  if (depth > GRAM_MAX_DEPTH)
    return gram_fail(p, KBC_ERR_PARSE,
                     "grammar: query nesting too deep (max %d levels)",
                     GRAM_MAX_DEPTH);
  if (p->pos >= p->ntok)
    return gram_fail(p, KBC_ERR_PARSE, "grammar: unexpected end of input");
  const gram_tok_kind k = p->toks[p->pos].kind;
  if (k == GT_NOT) {
    p->pos++;
    if (!parse_expr_term(p, depth + 1, out)) return false;
    /* De Morgan without a second AST: the child already parsed into DNF, and
     * the negation of a disjunction of conjunctions of literals is the same
     * disjunction with every literal flipped (query.rs:773 negate). */
    for (size_t i = 0; i < out->len; i++) {
      for (size_t j = 0; j < out->items[i].len; j++) {
        /* Flip the ATOM, not the copy: the fold only ever reads the atom. */
        gram_fact *f = &p->facts[p->facts[out->items[i].start + j].origin];
        f->negated = !f->negated;
      }
    }
    return true;
  }
  if (k == GT_LPAREN) {
    p->pos++;
    if (!parse_expr_or(p, depth + 1, out)) return false;
    if (p->pos >= p->ntok)
      return gram_fail(p, KBC_ERR_PARSE,
                       "grammar: unexpected end of input, expected ')'");
    if (p->toks[p->pos].kind != GT_RPAREN)
      return gram_fail(p, KBC_ERR_PARSE,
                       "grammar: unexpected token, expected ')' to close "
                       "group");
    p->pos++;
    return true;
  }
  if (k != GT_WORD && k != GT_QUOTED)
    return gram_fail(p, KBC_ERR_PARSE,
                     "grammar: unexpected token, expected a key");
  const char *word = p->toks[p->pos].text;
  p->pos++;
  gram_key key = GK_TEXT;
  const char *value = word;
  if (p->pos < p->ntok && p->toks[p->pos].kind == GT_COLON) {
    p->pos++;
    if (p->pos >= p->ntok)
      return gram_fail(p, KBC_ERR_PARSE,
                       "grammar: unexpected end of input, expected a value "
                       "after ':'");
    const gram_tok_kind vk = p->toks[p->pos].kind;
    if (vk != GT_WORD && vk != GT_QUOTED)
      return gram_fail(p, KBC_ERR_PARSE,
                       "grammar: unexpected token, expected a value after ':'");
    key = gram_key_of(word);
    value = p->toks[p->pos].text;
    p->pos++;
  }
  if (p->nfacts >= GRAM_MAX_FACTS)
    return gram_fail(p, KBC_ERR_INVALID,
                     "grammar: query expands to more than %d literals",
                     GRAM_MAX_FACTS);
  p->facts[p->nfacts].key = key;
  p->facts[p->nfacts].name = word;
  p->facts[p->nfacts].value = value;
  p->facts[p->nfacts].negated = false;
  p->facts[p->nfacts].origin = p->nfacts;
  p->nfacts++;
  const gram_span s = {p->nfacts - 1, 1};
  const int rc = span_push(p, out, s);
  if (rc < 0) return gram_fail(p, KBC_ERR_NOMEM, "grammar: out of memory");
  if (rc == 0) p->capped = true;
  return true;
}

static bool parse_expr_and(gram_parser *p, size_t depth, gram_list *out) {
  if (!parse_expr_term(p, depth, out)) return false;
  for (;;) {
    if (p->pos >= p->ntok) return true;
    const gram_tok_kind k = p->toks[p->pos].kind;
    /* AND/`&&`, or the implicit AND between adjacent terms (query.rs:676). */
    if (k != GT_AND && k != GT_WORD && k != GT_QUOTED && k != GT_LPAREN &&
        k != GT_NOT)
      return true;
    /* Only an explicit `AND`/`&&` is a separate token; with the implicit AND
     * the next token IS the term, so consuming it here would eat a term. */
    if (k == GT_AND) p->pos++;
    gram_list rhs;
    memset(&rhs, 0, sizeof(rhs));
    if (!parse_expr_term(p, depth, &rhs)) return false;
    /* Cartesian product: a row satisfies `a AND (b OR c)` when it matches
     * one of a∧b, a∧c (query.rs:339 dnf_build, And arm). */
    gram_list product;
    memset(&product, 0, sizeof(product));
    for (size_t i = 0; i < out->len; i++) {
      for (size_t j = 0; j < rhs.len; j++) {
        const gram_span base = out->items[i];
        const gram_span add = rhs.items[j];
        if (p->nfacts + base.len + add.len > GRAM_MAX_FACTS)
          return gram_fail(p, KBC_ERR_INVALID,
                           "grammar: query expands to more than %d literals",
                           GRAM_MAX_FACTS);
        /* Both runs are copied into one fresh, contiguous conjunct. dest is
         * always >= each src, so the forward copies are memmoves. */
        const size_t start = p->nfacts;
        for (size_t c = 0; c < base.len; c++)
          p->facts[p->nfacts++] = p->facts[base.start + c];
        for (size_t c = 0; c < add.len; c++)
          p->facts[p->nfacts++] = p->facts[add.start + c];
        const gram_span merged = {start, base.len + add.len};
        const int rc = span_push(p, &product, merged);
        if (rc < 0) return gram_fail(p, KBC_ERR_NOMEM, "grammar: out of memory");
        if (rc == 0) p->capped = true;
      }
    }
    *out = product;
  }
}

static bool parse_expr_or(gram_parser *p, size_t depth, gram_list *out) {
  for (;;) {
    if (!parse_expr_and(p, depth, out)) return false;
    if (p->pos < p->ntok && p->toks[p->pos].kind == GT_OR) {
      p->pos++;
      continue;
    }
    return true;
  }
}

/* What the index can be told, in its own vocabulary. */
typedef struct {
  const char *text;   /* arena, space-joined free text; "" when none */
  const char *folder; /* arena, NULL when the query names no folder */
  /* query.rs:276 parse_since, in nanoseconds. The grammar cannot APPLY it —
   * the index carries no mtime — so it is reported here for the caller that
   * does, and the atom never reaches the free text. 0 = no filter. */
  int64_t since_ns;
} gram_query;

/* query.rs:276 parse_since. Accepted: `Nd` (days), `Nh` (hours) and a raw
 * unix timestamp in seconds; `since:all` is the ABSENCE of a filter and is
 * an error, as it is in the original — the way to write it is to drop the
 * atom. Returns 0 on a value the original would also refuse. The unit count
 * is clamped so `since:99999999999d` cannot overflow the arithmetic into a
 * threshold that silently means the opposite. */
#define SEARCH_SINCE_MAX_UNITS 1000000000LL
/* CONTRACT GAP: the value grammar of `since:` is needed by the layer that
 * APPLIES the filter, not only by the one that validates it, and search.h is
 * frozen. Declared here for app.c on the same terms as
 * kbc_searcher_set_vecstore above; the orchestrator should add to search.h:
 *   kbc_status kbc_since_value_ns(const char *value, int64_t *ns,
 *                                  kbc_err *err);
 * 0 in *ns means "no filter". */
kbc_status kbc_since_value_ns(const char *value, int64_t *ns, kbc_err *err);

kbc_status kbc_since_value_ns(const char *value, int64_t *ns, kbc_err *err) {
  *ns = 0;
  if (value[0] == '\0')
    return kbc_err_set(err, KBC_ERR_INVALID, "search: since: has no value");
  if (strcmp(value, "all") == 0) {
    return kbc_err_set(
        err, KBC_ERR_INVALID,
        "search: since:all is the absence of a filter — drop the atom");
  }
  const size_t len = strlen(value);
  int64_t units = 0;
  int64_t mult = 0;
  bool relative = false;
  if (value[len - 1] == 'd') {
    mult = 86400;
    relative = true;
  } else if (value[len - 1] == 'h') {
    mult = 3600;
    relative = true;
  }
  if (relative) {
    char digits[32];
    const size_t dlen = len - 1;
    if (dlen == 0 || dlen >= sizeof(digits))
      return kbc_err_set(err, KBC_ERR_INVALID,
                         "search: since %s: not a number of days/hours",
                         value);
    for (size_t i = 0; i < dlen; i++) {
      const char c = value[i];
      if (c < '0' || c > '9')
        return kbc_err_set(err, KBC_ERR_INVALID,
                           "search: since %s: not a number of days/hours",
                           value);
      digits[i] = c;
    }
    digits[dlen] = '\0';
    units = strtoll(digits, NULL, 10);
  } else {
    char digits[32];
    if (len >= sizeof(digits))
      return kbc_err_set(err, KBC_ERR_INVALID,
                         "search: since: unsupported unit in \"%s\"", value);
    memcpy(digits, value, len + 1);
    char *end = NULL;
    units = strtoll(digits, &end, 10);
    if (end == digits || *end != '\0')
      return kbc_err_set(err, KBC_ERR_INVALID,
                         "search: since: unsupported unit in \"%s\"", value);
    mult = 1;
  }
  /* A raw timestamp IS the threshold; only the relative forms are measured
   * back from now, and a negative one means a future moment, which no
   * document can satisfy — 0 there would silently disable the filter, so it
   * is refused instead. */
  if (!relative) {
    if (units <= 0)
      return kbc_err_set(err, KBC_ERR_INVALID,
                         "search: since %s: a unix timestamp in seconds must "
                         "be positive",
                         value);
    /* Clamping an ABSOLUTE timestamp would move it into the past and turn a
     * filter that excludes everything into one that excludes nothing; the
     * only bound here is the one that keeps the multiply in range. */
    if (units > INT64_MAX / 1000000000LL)
      return kbc_err_set(err, KBC_ERR_INVALID,
                         "search: since %s: timestamp out of range", value);
    *ns = units * 1000000000LL;
    return KBC_OK;
  }
  if (units > SEARCH_SINCE_MAX_UNITS) units = SEARCH_SINCE_MAX_UNITS;
  const int64_t now_s = kbc_now_ns() / 1000000000LL;
  const int64_t delta = units * mult;
  *ns = (delta >= now_s ? 0 : (now_s - delta) * 1000000000LL);
  return KBC_OK;
}

/* CONTRACT GAP: this is the whole of the query grammar as the index can
 * express it, and a test cannot reach it because search.h is frozen. The
 * orchestrator should add to search.h:
 *   typedef struct { const char *text; const char *folder; bool capped; }
 *       kbc_gram;
 *   kbc_status kbc_query_grammar(kbc_arena *a, const char *q, kbc_gram *out,
 *                                kbc_err *err);
 * The `capped` field is the "OR expansion capped at 64 alternatives" warning,
 * which today is a log line: loud, but not renderable as a chip the way the
 * original's SPA does. */
kbc_status kbc_query_grammar(kbc_arena *a, const char *q, gram_query *out,
                             kbc_err *err);

kbc_status kbc_query_grammar(kbc_arena *a, const char *q, gram_query *out,
                             kbc_err *err) {
  if (out == NULL)
    return kbc_err_set(err, KBC_ERR_INVALID, "query_grammar: out is NULL");
  out->text = "";
  out->folder = NULL;
  out->since_ns = 0;
  if (q == NULL)
    return kbc_err_set(err, KBC_ERR_INVALID, "query_grammar: q is NULL");
  const size_t n = strlen(q);
  gram_tok *toks = NULL;
  size_t ntok = 0;
  const kbc_status st = gram_tokenize(a, q, n, &toks, &ntok, err);
  if (st != KBC_OK) return st;
  if (ntok == 0) return KBC_OK; /* an empty query is the caller's to reject */

  gram_fact *facts = kbc_arena_alloc(a, GRAM_MAX_FACTS * sizeof(gram_fact));
  if (facts == NULL)
    return kbc_err_set(err, KBC_ERR_NOMEM, "query_grammar: %d literals",
                       GRAM_MAX_FACTS);
  gram_parser p;
  memset(&p, 0, sizeof(p));
  p.a = a;
  p.toks = toks;
  p.ntok = ntok;
  p.facts = facts;
  p.status = KBC_OK;

  gram_list top;
  memset(&top, 0, sizeof(top));
  if (!parse_expr_or(&p, 0, &top)) return kbc_err_set(err, p.status, "%s", p.msg);
  if (p.pos != p.ntok)
    return kbc_err_set(err, KBC_ERR_PARSE,
                       "grammar: unexpected token after expression");
  if (p.capped)
    KBC_LOGW("search: OR expansion capped at %d alternatives; refine the query",
             GRAM_MAX_CONJUNCTS);

  /* Free text is collected across the WHOLE expression, in document order,
   * and scored as one BM25 string (query.rs:419 collect_text): the boolean
   * shape around it never reaches the lexical arm. */
  kbc_str text;
  kbc_str_init(&text);
  const char *folder = NULL;
  int64_t since_ns = 0;
  for (size_t i = 0; i < p.nfacts; i++) {
    const gram_fact *f = &p.facts[i];
    if (f->origin != i) continue; /* a DNF copy of an atom already folded */
    if (f->key == GK_TEXT) {
      if (f->negated) {
        /* Rust warns rather than misfiltering; so do we. */
        KBC_LOGW("search: NOT text:\"%s\" is unsupported; ignoring", f->value);
        continue;
      }
      if (f->value[0] == '\0') continue;
      if (text.len > 0 && kbc_str_putc(&text, ' ') != KBC_OK) {
        kbc_str_free(&text);
        return kbc_err_set(err, KBC_ERR_NOMEM, "query_grammar: free text");
      }
      if (kbc_str_puts(&text, f->value) != KBC_OK) {
        kbc_str_free(&text);
        return kbc_err_set(err, KBC_ERR_NOMEM, "query_grammar: free text");
      }
      continue;
    }
    if (f->key == GK_FOLDER) {
      if (f->negated) {
        kbc_str_free(&text);
        return kbc_err_set(
            err, KBC_ERR_UNSUPPORTED,
            "search: NOT folder:\"%s\" is unsupported (kb-c has no "
            "exclude-path filter)",
            f->value);
      }
      if (folder == NULL) {
        folder = f->value;
      } else if (strcmp(folder, f->value) != 0) {
        /* A folder in two different DNF conjuncts is an OR over folders, and
         * kbc_query carries exactly one path prefix. */
        kbc_str_free(&text);
        return kbc_err_set(err, KBC_ERR_UNSUPPORTED,
                           "search: OR over more than one folder is "
                           "unsupported (got \"%s\" and \"%s\")",
                           folder, f->value);
      }
      continue;
    }
    if (f->key == GK_SINCE) {
      if (f->negated) {
        /* query.rs:381 — there is no upper bound to negate, so the original
         * warns and drops it. So do we: a filter silently applied backwards
         * would be worse than one that is visibly absent. */
        KBC_LOGW("search: NOT since:\"%s\" is unsupported (no upper-bound "
                 "filter); ignoring",
                 f->value);
        continue;
      }
      int64_t ns = 0;
      kbc_status ss = kbc_since_value_ns(f->value, &ns, err);
      if (ss != KBC_OK) {
        kbc_str_free(&text);
        return ss;
      }
      /* Validated, then dropped: the index carries no mtime, so applying it
       * is the caller's job (kbc_query.since_ns). Two `since:` atoms AND
       * together, so the strictest threshold wins. */
      if (ns > since_ns) since_ns = ns;
      continue;
    }
    if (f->key == GK_OTHER) {
      kbc_str_free(&text);
      return kbc_err_set(err, KBC_ERR_INVALID,
                         "search: unknown query key \"%s\"; valid keys: "
                         "folder, since, text",
                         f->name);
    }
    kbc_str_free(&text);
    return kbc_err_set(err, KBC_ERR_UNSUPPORTED,
                       "search: query key \"%s\" is not supported by the "
                       "kb-c index (valid keys: folder, since, text)",
                       gram_key_name(f->key));
  }
  out->since_ns = since_ns;
  out->text = kbc_arena_strndup(a, text.ptr != NULL ? text.ptr : "", text.len);
  if (text.ptr != NULL) kbc_str_free(&text);
  out->folder = folder;
  return KBC_OK;
}

/* ------------------------------------------------------------ title boost */

/* fusion.rs:145. The boost multiplies the FUSED score by (1 + factor) for a
 * hit whose title contains ANY query term — once, however many terms match,
 * and whether or not the term is also in the body. It is applied after fusion
 * and before filtering/truncation (routes/search.rs:1185), so a boosted hit
 * that was just past the limit can still surface. */
#define SEARCH_TITLE_BOOST 0.5
/* fusion.rs:161 — a term shorter than this carries no signal. */
#define SEARCH_TITLE_TERM_MIN 3

/* ASCII-lowercases `s` into `out`, which the caller sized. Byte-for-byte, not
 * kbc_fold_utf8: the original lowercases but does NOT strip diacritics, and a
 * boost that matched more titles than the original's would be a behaviour
 * change wearing a port's clothes. */
static void ascii_lower_copy(const char *s, char *out) {
  size_t i = 0;
  for (; s[i] != '\0'; i++) out[i] = g_lower(s[i]);
  out[i] = '\0';
}

/* True when any term occurs anywhere in the (already lowercased) title.
 * Substring, not word-boundary: fusion.rs:168 documents that a word-boundary
 * variant bench-measured WORSE (the query "embedding" has to reach the title
 * "Embeddings") and asks for a re-bench before changing it. */
static bool title_matches(const kbc_strlist *terms, const char *lowered) {
  for (size_t i = 0; i < terms->len; i++) {
    if (strstr(lowered, terms->items[i]) != NULL) return true;
  }
  return false;
}

static kbc_status apply_title_boost(kbc_arena *a, const kbc_index *ix,
                                    fused_row *rows, size_t n, const char *text,
                                    double factor, kbc_err *err) {
  if (factor == 0.0 || n == 0 || text == NULL) return KBC_OK;
  kbc_strlist terms;
  kbc_strlist_init(&terms);
  /* split(|c| !c.is_alphanumeric) .filter(|t| t.len() >= 3) .to_ascii_lowercase
   * (fusion.rs:159) — the length test is in BYTES, as it is in Rust. */
  const size_t nlen = strlen(text);
  size_t i = 0;
  while (i < nlen) {
    while (i < nlen && !g_alnum((unsigned char)text[i])) i++;
    const size_t start = i;
    while (i < nlen && g_alnum((unsigned char)text[i])) i++;
    const size_t len = i - start;
    if (len < SEARCH_TITLE_TERM_MIN) continue;
    char *term = kbc_arena_alloc(a, len + 1);
    if (term == NULL) {
      kbc_strlist_free(&terms);
      return kbc_err_set(err, KBC_ERR_NOMEM, "title boost: %zu bytes", len);
    }
    for (size_t j = 0; j < len; j++) term[j] = g_lower(text[start + j]);
    term[len] = '\0';
    /* push, not push_owned: the list frees what it owns, and `term` is
     * arena memory. */
    const size_t before = terms.len;
    const kbc_status ps = kbc_strlist_push(&terms, term);
    if (ps != KBC_OK) {
      kbc_strlist_free(&terms);
      return kbc_err_set(err, KBC_ERR_NOMEM, "title boost: %zu terms",
                         before + 1);
    }
  }
  if (terms.len == 0) {
    kbc_strlist_free(&terms);
    return KBC_OK;
  }
  /* One buffer, sized to the longest title, reused for every row. */
  size_t cap = 0;
  for (size_t r = 0; r < n; r++) {
    const kbc_doc_meta *m = kbc_index_doc(ix, rows[r].doc);
    if (m == NULL || m->title == NULL) continue;
    const size_t tl = strlen(m->title);
    if (tl > cap) cap = tl;
  }
  char *low = NULL;
  if (cap > 0) {
    low = kbc_arena_alloc(a, cap + 1);
    if (low == NULL) {
      kbc_strlist_free(&terms);
      return kbc_err_set(err, KBC_ERR_NOMEM, "title boost: %zu bytes", cap);
    }
  }
  for (size_t r = 0; r < n; r++) {
    const kbc_doc_meta *m = kbc_index_doc(ix, rows[r].doc);
    if (m == NULL || m->title == NULL) continue;
    ascii_lower_copy(m->title, low);
    if (title_matches(&terms, low)) rows[r].score *= 1.0 + factor;
  }
  kbc_strlist_free(&terms);
  return KBC_OK;
}

/* fusion.rs:214 apply_graph_boost. In-degree (backlinks) is the endorsement
 * axis; out-degree is deliberately ignored, so a note that links everything
 * cannot rank itself up by fan-out. The most-linked document in the corpus
 * gains `weight / rrf_k` — one extra top-rank RRF arm — and everything else
 * scales by sqrt(in)/sqrt(in_max).
 *
 * `sqrt` and not `ln`, deliberately: IEEE-754 sqrt is correctly rounded and so
 * bit-identical across libm implementations, while `ln` is not, and a boost
 * that flipped equal-score ties across glibc/musl would be a determinism bug
 * wearing a ranking feature's clothes.
 *
 * weight 0.0 returns before a single floating-point operation happens, which
 * is what makes the default off byte-identical to no boost at all. */
static void apply_graph_boost(fused_row *rows, size_t n, const kbc_query *q,
                              double rrf_k) {
  if (q->graph_boost_weight == 0.0 || n == 0 || q->in_deg == NULL ||
      q->in_deg_len == 0) {
    return;
  }
  /* The corpus-wide max, not the max over this query's candidates: the
   * normalisation must not change with what a query happened to match. */
  uint32_t in_max = 0;
  for (size_t i = 0; i < q->in_deg_len; i++) {
    if (q->in_deg[i] > in_max) in_max = q->in_deg[i];
  }
  if (in_max == 0) return; /* no graph, or every degree is zero: a no-op */
  const double denom = sqrt((double)in_max);
  for (size_t r = 0; r < n; r++) {
    /* A doc the array does not name counts 0 — a lookup, not a skip. */
    const uint32_t doc = rows[r].doc;
    uint32_t in = 0;
    if (q->in_deg_doc_ids != NULL) {
      size_t lo = 0, hi = q->in_deg_len;
      while (lo < hi) {
        const size_t mid = lo + (hi - lo) / 2;
        if (q->in_deg_doc_ids[mid] < doc) {
          lo = mid + 1;
        } else {
          hi = mid;
        }
      }
      if (lo < q->in_deg_len && q->in_deg_doc_ids[lo] == doc)
        in = q->in_deg[lo];
    }
    if (in == 0) continue;
    rows[r].score += q->graph_boost_weight * (1.0 / rrf_k) *
                     (sqrt((double)in) / denom);
  }
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
    /* The query's own tokens stay in the list: the completions are ADDED to
     * them, not substituted for them. Copying them here (the arena array is
     * zeroed) is what makes `alph*` still carry `alph`. */
    memcpy(out->items, toks->items, toks->len * sizeof(kbc_token));
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
      /* The list owns these strings and is freed at the end of this
       * function, so the token must point at an arena copy — a token holding
       * the freed pointer would make BM25 look up a term that no longer
       * exists, and the whole expansion would silently match nothing. */
      const char *t = kbc_arena_strdup(a, terms.items[i]);
      if (t == NULL) {
        kbc_strlist_free(&terms);
        return kbc_err_set(err, KBC_ERR_NOMEM, "search: %zu expansion terms",
                           extra);
      }
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

  kbc_status st;
  /* The kb query grammar: pull `folder:` out of the string, keep the rest as
   * free text, and refuse anything the index cannot evaluate. An unparsable
   * or unsupported query is an error here, never a literal-term search. */
  gram_query gq;
  st = kbc_query_grammar(a, q->q, &gq, err);
  if (st != KBC_OK) return st;
  /* The rest of the function works on the EFFECTIVE query: the grammar's
   * text, and the grammar's folder as the path filter (the DSL folder
   * overrides the flat param, as routes/docs.rs:625 has it). */
  kbc_query eq = *q;
  eq.q = gq.text;
  if (gq.folder != NULL) eq.path_prefix = gq.folder;

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
  st = kbc_tokenize(a, eq.q, strlen(eq.q), &toks, err);
  if (st != KBC_OK) return st;
  /* A facet-only query (`folder:notes`) filters the corpus instead of
   * scoring it: every doc that passes the filter is a hit, all scoring zero
   * and tie-breaking by doc id. Without a filter, zero tokens means stopwords
   * and punctuation only, and returning "everything" there is how a search
   * starts lying — an empty result is the truth. */
  const bool filter_only = toks.len == 0 && gq.folder != NULL;
  if (toks.len == 0 && !filter_only) {
    out->degraded = vec_expected;
    out->took_us = (kbc_now_ns() - started) / 1000;
    return KBC_OK;
  }

  kbc_tokens qt;
  memset(&qt, 0, sizeof(qt));
  if (!filter_only) {
    qt.items = kbc_arena_calloc(a, toks.len + SEARCH_MAX_EXPANSIONS,
                                sizeof(kbc_token));
    if (qt.items == NULL)
      return kbc_err_set(err, KBC_ERR_NOMEM, "search_run: %zu query tokens",
                         toks.len);
    st = expand_query_tokens(a, s->ix, &eq, &toks, &qt, err);
    if (st != KBC_OK) return st;
  }

  lane kw, vl;
  memset(&kw, 0, sizeof(kw));
  memset(&vl, 0, sizeof(vl));
  kw.items = kbc_arena_calloc(a, depth, sizeof(fused_row));
  vl.items = kbc_arena_calloc(a, depth, sizeof(fused_row));
  if (kw.items == NULL || vl.items == NULL)
    return kbc_err_set(err, KBC_ERR_NOMEM, "search_run: %zu candidates",
                       depth);

  /* lane 1: keyword --------------------------------------------------- */
  if (kw_expected || filter_only) {
    kbc_hits hits;
    kbc_hits_init(&hits);
    if (!filter_only) {
      st = kbc_index_bm25(s->ix, &qt, k1, b, depth, &hits, err);
      if (st != KBC_OK) {
        kbc_hits_free(&hits);
        return st;
      }
    } else {
      const uint32_t count = kbc_index_doc_count(s->ix);
      for (uint32_t d = 0; d < count; d++) {
        if (!row_passes(s->ix, d, &eq)) continue;
        const kbc_status ps = kbc_hits_push(&hits, d, 0.0);
        if (ps != KBC_OK) {
          kbc_hits_free(&hits);
          return kbc_err_set(err, ps, "search: %u docs: out of memory",
                             (unsigned)count);
        }
      }
      kbc_hits_sort_desc(&hits);
    }
    for (size_t i = 0; i < hits.len && kw.len < depth; i++) {
      if (!row_passes(s->ix, hits.items[i].doc, &eq)) continue;
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
      /* No keyword shortlist to narrow the scan, so walk the whole store and
       * fold each row into a bounded top-k: `depth` slots, never `count`.
       * The old pass appended every scored row to the hit list first, which
       * capped the store at KBC_MAX_HITS rows and turned any bigger corpus
       * into a hard error. */
      kbc_hit *storage = malloc(depth * sizeof(*storage));
      if (storage == NULL) {
        kbc_hits_free(&hits);
        return kbc_err_set(err, KBC_ERR_NOMEM, "search: %zu candidates",
                           depth);
      }
      topk_heap heap;
      heap.items = storage;
      heap.len = 0;
      heap.cap = depth;
      const uint32_t count = kbc_vecstore_count(s->vs);
      for (uint32_t i = 0; i < count; i++) {
        const float *row = kbc_vecstore_get(s->vs, i);
        double score = 0.0;
        if (row == NULL) continue;
        if (!cosine_score(vec, qnorm, row, dim, &score)) continue;
        if (!row_passes(s->ix, i, &eq)) continue;
        topk_push(&heap, i, score);
      }
      topk_finish(&heap, storage, depth, &hits);
    } else {
      /* Hybrid: only the keyword shortlist can contribute, so score just
       * those rows. They are read straight out of the store one at a time —
       * gathering them into the request arena first cost depth * dim floats
       * (307 KB at 384 dims) for no benefit, and pushed a whole hybrid
       * query over the arena's ceiling. */
      const uint32_t count = kbc_vecstore_count(s->vs);
      if (kw.len > 0) {
        kbc_hit *storage = malloc(depth * sizeof(*storage));
        if (storage == NULL) {
          kbc_hits_free(&hits);
          return kbc_err_set(err, KBC_ERR_NOMEM, "search: %zu candidates",
                             depth);
        }
        topk_heap heap;
        heap.items = storage;
        heap.len = 0;
        heap.cap = depth;
        for (size_t i = 0; i < kw.len; i++) {
          const uint32_t doc = kw.items[i].doc;
          if (doc >= count) continue;
          const float *row = kbc_vecstore_get(s->vs, doc);
          if (row == NULL) continue;
          double score = 0.0;
          if (!cosine_score(vec, qnorm, row, dim, &score)) continue;
          topk_push(&heap, doc, score);
        }
        topk_finish(&heap, storage, depth, &hits);
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
  /* fusion.rs:155 — the title boost, on the full candidate pool, before
   * filtering and truncation. Multiplying here (not inside the keyword lane)
   * is what makes it a fusion-stage boost: the raw BM25 score each lane
   * reported is untouched, and a doc that outranked another by a hair can
   * still come back on its title. */
  st = apply_title_boost(a, s->ix, rows, uniq, gq.text, SEARCH_TITLE_BOOST,
                         err);
  if (st != KBC_OK) return st;
  /* fusion.rs:214 — the graph boost, on the full candidate pool, after fusion
   * and after the title boost and before filtering and truncation, so a
   * well-linked hit just past the limit can still surface. `eq` rather than
   * `q`: the grammar may have overridden other fields, and the boost reads
   * the caller's graph either way. */
  apply_graph_boost(rows, uniq, &eq, rrf_k);
  /* Rust re-sorts stably by score after the boost. kb-c's fused order is
   * already score-desc with a deterministic tie-break, so a plain re-sort by
   * that same comparator IS the stable sort. */
  qsort(rows, uniq, sizeof(fused_row), cmp_fused);

  out->candidates = uniq;
  out->vector_ran = vector_ran;
  out->degraded = vec_expected &&
                   (!vector_ran || (q->mode == KBC_MODE_SEMANTIC && uniq == 0));
  if (out->degraded) {
    /* A query that quietly answered with no vector scores at all looks
     * exactly like a working one, which is how a failing vector lane stayed
     * invisible. Say so, with the reason. */
    KBC_LOGW("search: mode %s returned %zu rows with no vector score "
             "(vector lane ran=%d)",
             kbc_search_mode_str(q->mode), uniq, (int)vector_ran);
  }

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
