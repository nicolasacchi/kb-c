/* index.c — the inverted index: term dictionary, postings, BM25, one file.
 *
 * Layout, and why:
 *   - terms live in one contiguous string arena; the hash table stores
 *     (offset,len) into it, so a term is stored once and the dictionary is one
 *     allocation.
 *   - the table is open addressed (FNV-1a 64 of the folded term, power-of-two
 *     capacity, linear probing, 0.7 load factor). Slot state is a sentinel in
 *     term_off: SLOT_EMPTY / SLOT_TOMB. Removal happens on the add_doc
 *     rollback path, which must tombstone because a live entry can be in
 *     someone else's probe chain.
 *   - one posting list per term, a contiguous run of the global postings
 *     array. Documents are added in ascending doc_id (the header requires it),
 *     so appends already yield doc_id-ascending lists — end_build asserts that
 *     invariant rather than re-sorting.
 *   - query: BM25 into a per-query accumulator indexed by doc_id, then a
 *     bounded min-heap of size `limit`. A 500k-doc corpus must not sort 500k
 *     scores to return 20 hits.
 *
 * Threading contract:
 *   - BUILD functions (begin_build / add_doc / end_build) are single-threaded.
 *     An open build is mutated in place; nothing here is safe to share.
 *   - Every READ function below (doc_count, doc, id_of, avg_doclen, bm25,
 *     expand_prefix, heap_bytes) takes a const index and is safe to call
 *     concurrently from any number of threads on ONE index. A sealed index
 *     is immutable, and the per-query scratch those functions need lives in
 *     per-thread storage, so no two queries ever touch the same byte.
 *   - The build->read handoff is a pointer swap done by the caller (kbc_app
 *     does it under a write lock), not anything inside this file.
 */

#include <stdio.h>

#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include "kbc/index.h"

#define SLOT_EMPTY 0xFFFFFFFFu
#define SLOT_TOMB 0xFFFFFFFEu

/* 0.7 load factor in permille, so the check stays integer. */
#define LOAD_PERMILLE 700u
#define ARENA_BLOCK 65536u
#define DREC_SIZE 32u
#define TSLOT_SIZE 24u
#define DSLOT_SIZE 16u
#define POST_SIZE 8u
#define HDR_SIZE 64u

/* ------------------------------------------------------------- records --- */

typedef struct {
  uint32_t doc;
  uint32_t tf;
} kbc_posting;

typedef struct {
  uint64_t hash;
  uint32_t term_off;
  uint32_t term_len;
  uint32_t post_off;
  uint32_t post_len;
  /* Stable term identity, assigned once at creation. The SLOT INDEX is not
   * stable: term_grow rehashes and moves every live term, so nothing that has
   * to survive a grow may name a slot. Build-time only, never written to the
   * file. */
  uint32_t id;
} kbc_term_slot;

typedef struct {
  uint64_t hash;
  uint32_t doc; /* SLOT_EMPTY when free */
} kbc_doc_slot;

/* Chunked string arena. Blocks are never reallocated, so a `const char *` into
 * one stays valid for the life of the index; that is what lets kbc_doc_meta
 * hold plain pointers instead of offsets. */
typedef struct {
  char *p;
  size_t size; /* allocated */
  size_t used; /* bytes valid */
  size_t base; /* absolute offset of this block's first byte */
} kbc_arena_block;

typedef struct {
  kbc_arena_block *blocks;
  size_t nblocks, cap_blocks;
  size_t total; /* sum of used */
} kbc_arena_str;

/* One distinct query term. Named so the dedup scratch can be reallocated
 * through a plain typed pointer. */
typedef struct {
  uint64_t h;
  const char *s;
  uint32_t len;
} kbc_qterm;

/* Query scratch, owned by the index and reused across calls. */
typedef struct {
  double *scores;
  uint32_t *stamps; /* generation stamp, so a query needs no O(N) clear */
  uint32_t *touched;
  size_t doc_cap;
  uint32_t gen;

  kbc_hit *heap;
  size_t heap_cap;

  kbc_qterm *q;
  size_t q_cap;
} kbc_acc;

/* Per-document build scratch, owned by the index while a build is open. */
typedef struct {
  uint32_t *mi; /* mcap slots, SLOT_EMPTY when free -> index into ts */
  uint64_t *mh;
  size_t mcap;

  const char **ts;
  uint32_t *tl;
  uint32_t *tf;
  uint32_t *toff; /* term-arena offset once interned */
  uint32_t *ci;   /* term slots created by the doc being added (rollback) */
  size_t ci_len, ci_cap;
  size_t cap, len;
} kbc_scratch;

struct kbc_index {
  kbc_term_slot *terms;
  size_t term_cap, term_live, term_tombs;

  kbc_doc_slot *dhash;
  size_t dhash_cap;

  kbc_arena_str tar; /* terms */
  kbc_arena_str dar; /* document strings */

  kbc_doc_meta *docs; /* by doc_id, size doc_cap */
  uint32_t doc_cap;
  uint32_t doc_count;
  uint64_t total_tokens;

  kbc_posting *post;
  size_t post_len, post_cap;
  /* Build-time only: the STABLE TERM ID (kbc_term_slot.id) of the term each
   * appended posting belongs to. The commit loop appends one posting per
   * DISTINCT TERM per document, so the global array is interleaved and
   * post_off/post_len (a count, not a span) cannot describe a term's postings.
   * This sidecar carries the missing term identity through the build so
   * end_build can materialize one contiguous run per term. It is keyed on the
   * id, never on the slot index: term_grow moves every live term, so a stale
   * slot index names a different term after a rehash and scatters that term's
   * postings under a stranger. Freed by end_build, never saved. */
  uint32_t *post_term;
  size_t post_term_cap;
  /* One past the highest term id handed out this build. Ids are never reused
   * or renumbered, so an id names the same term for the whole build. */
  uint32_t next_term_id;

  bool sealed;

  void *map; /* mmap of a saved index, else NULL */
  const char *tar_base; /* mmap: the term arena, when the index is not built */
  const char *dar_base; /* mmap: the doc arena, likewise. doc_meta pointers
                         * point into it, so it is the same condition as
                         * tar_base: an index that has never been mutated
                         * keeps BOTH arenas in the mapping and has no heap
                         * copy of either. */

  size_t map_len;

  kbc_scratch *sc;
};

/* ----------------------------------------------------------------- util -- */

static uint64_t fnv1a64(const void *data, size_t n) {
  const uint8_t *p = (const uint8_t *)data;
  uint64_t h = 1469598103934665603ULL;
  size_t i;
  for (i = 0; i < n; i++) {
    h ^= (uint64_t)p[i];
    h *= 1099511628211ULL;
  }
  return h;
}

static uint64_t hash_corpus_path(const char *corpus, const char *path) {
  uint64_t h = 1469598103934665603ULL;
  const uint8_t *p = (const uint8_t *)corpus;
  size_t i, clen = strlen(corpus);
  for (i = 0; i < clen; i++) {
    h ^= (uint64_t)p[i];
    h *= 1099511628211ULL;
  }
  h ^= 0u;
  h *= 1099511628211ULL; /* the NUL separator: "a" + "bc" != "ab" + "c" */
  p = (const uint8_t *)path;
  for (i = 0; i < strlen(path); i++) {
    h ^= (uint64_t)p[i];
    h *= 1099511628211ULL;
  }
  return h;
}

static size_t round_up_pow2(size_t n) {
  size_t c = 16;
  while (c < n) {
    if (c > SIZE_MAX / 2) {
      return 0;
    }
    c *= 2;
  }
  return c;
}

/* kbc_str_reserve's contract is "extra MORE bytes" and its return is a bool
 * (true = success; KBC_OK == 0 would read a successful reserve as a failure).
 * The doubling call is sized in absolute bytes, so the whole amount is passed
 * as `extra`; the re-read of s->cap is the authoritative check. */
static kbc_status buf_reserve(kbc_str *s, size_t extra) {
  size_t need;
  if (extra > SIZE_MAX - s->len) {
    return KBC_ERR_NOMEM;
  }
  need = s->len + extra;
  if (s->cap >= need) {
    return KBC_OK;
  }
  if (!kbc_str_reserve(s, need - s->len) && s->cap < need) {
    return KBC_ERR_NOMEM;
  }
  if (s->cap < need && !kbc_str_reserve(s, need) && s->cap < need) {
    return KBC_ERR_NOMEM;
  }
  return s->cap >= need ? KBC_OK : KBC_ERR_NOMEM;
}

/* --------------------------------------------------------- string arena -- */

static void sar_free(kbc_arena_str *ar) {
  size_t i;
  for (i = 0; i < ar->nblocks; i++) {
    free(ar->blocks[i].p);
  }
  free(ar->blocks);
  ar->blocks = NULL;
  ar->nblocks = ar->cap_blocks = 0;
  ar->total = 0;
}

static void sar_clear(kbc_arena_str *ar) {
  size_t i;
  for (i = 0; i < ar->nblocks; i++) {
    ar->blocks[i].used = 0;
    ar->blocks[i].base = 0;
  }
  ar->total = 0;
}

/* Copies n bytes plus a NUL. `off_out` receives the absolute offset. */
static kbc_status sar_put(kbc_arena_str *ar, const char *s, size_t n,
                         uint32_t *off_out) {
  kbc_arena_block *blk;
  size_t need = n + 1, sz;
  if (n > (size_t)UINT32_MAX - 1) {
    return KBC_ERR_NOMEM;
  }
  if (ar->nblocks == 0) {
    if (ar->cap_blocks == 0) {
      kbc_arena_block *nb =
          (kbc_arena_block *)realloc(ar->blocks, 4 * sizeof(kbc_arena_block));
      if (!nb) {
        return KBC_ERR_NOMEM;
      }
      ar->blocks = nb;
      ar->cap_blocks = 4;
    }
    sz = need > ARENA_BLOCK ? need : ARENA_BLOCK;
    blk = &ar->blocks[0];
    blk->p = (char *)malloc(sz);
    if (!blk->p) {
      return KBC_ERR_NOMEM;
    }
    blk->size = sz;
    blk->used = 0;
    blk->base = 0;
    ar->nblocks = 1;
  } else {
    blk = &ar->blocks[ar->nblocks - 1];
    if (blk->size - blk->used < need) {
      kbc_arena_block *nb;
      sz = need > ARENA_BLOCK ? need : ARENA_BLOCK;
      nb = (kbc_arena_block *)realloc(ar->blocks, (ar->nblocks + 1) *
                                                 sizeof(kbc_arena_block));
      if (!nb) {
        return KBC_ERR_NOMEM;
      }
      ar->blocks = nb;
      ar->cap_blocks = ar->nblocks + 1;
      blk = &ar->blocks[ar->nblocks];
      blk->p = (char *)malloc(sz);
      if (!blk->p) {
        return KBC_ERR_NOMEM; /* the slot stays unclaimed */
      }
      blk->size = sz;
      blk->used = 0;
      blk->base = ar->total;
      ar->nblocks++;
    }
  }
  memcpy(blk->p + blk->used, s, n);
  blk->p[blk->used + n] = '\0';
  *off_out = (uint32_t)(blk->base + blk->used);
  blk->used += need;
  ar->total += need;
  return KBC_OK;
}

static const char *sar_ptr(const kbc_arena_str *ar, uint32_t off) {
  size_t lo = 0, hi = ar->nblocks;
  if (hi == 0) {
    return NULL;
  }
  while (lo + 1 < hi) {
    size_t mid = lo + (hi - lo) / 2;
    if (ar->blocks[mid].base <= off) {
      lo = mid;
    } else {
      hi = mid;
    }
  }
  if (off >= ar->blocks[lo].base + ar->blocks[lo].used) {
    return NULL;
  }
  return ar->blocks[lo].p + (off - ar->blocks[lo].base);
}
/* Terms come from the build-time arena, or straight out of the mapping when
 * the index was opened. */
static const char *term_ptr(const kbc_index *ix, uint32_t off) {
  if (ix->tar_base) {
    return ix->tar_base + off;
  }
  return sar_ptr(&ix->tar, off);
}


/* ---------------------------------------------------------- term table --- */

static void term_clear(kbc_index *ix) {
  size_t i;
  for (i = 0; i < ix->term_cap; i++) {
    ix->terms[i].term_off = SLOT_EMPTY;
  }
  ix->term_live = 0;
  ix->term_tombs = 0;
}

static kbc_status term_grow(kbc_index *ix, size_t want_cap) {
  kbc_term_slot *nt;
  size_t ncap = ix->term_cap ? ix->term_cap : 1024, i;
  while (ncap < want_cap) {
    if (ncap > SIZE_MAX / 2 || ncap * 2 > SIZE_MAX / sizeof(kbc_term_slot)) {
      return KBC_ERR_NOMEM;
    }
    ncap *= 2;
  }
  if (ncap == ix->term_cap) {
    return KBC_OK;
  }
  nt = (kbc_term_slot *)malloc(ncap * sizeof(kbc_term_slot));
  if (!nt) {
    return KBC_ERR_NOMEM;
  }
  for (i = 0; i < ncap; i++) {
    nt[i].term_off = SLOT_EMPTY;
  }
  for (i = 0; i < ix->term_cap; i++) {
    const kbc_term_slot *s = &ix->terms[i];
    size_t j;
    if (s->term_off == SLOT_EMPTY || s->term_off == SLOT_TOMB) {
      continue;
    }
    j = (size_t)(s->hash & (uint64_t)(ncap - 1));
    while (nt[j].term_off != SLOT_EMPTY) {
      j = (j + 1) & (ncap - 1);
    }
    nt[j] = *s;
  }
  free(ix->terms);
  ix->terms = nt;
  ix->term_cap = ncap;
  ix->term_tombs = 0; /* the rehash dropped them */
  return KBC_OK;
}

static bool term_eq(const kbc_index *ix, const kbc_term_slot *s, uint64_t h,
                    const char *t, size_t n) {
  const char *p;
  if (s->hash != h || (size_t)s->term_len != n) {
    return false;
  }
  p = term_ptr(ix, s->term_off);
  return p != NULL && memcmp(p, t, n) == 0;
}

/* Slot index, or SIZE_MAX. An index (not a pointer) so the build path can fill
 * the slot without casting const away. */
static size_t term_find_idx(const kbc_index *ix, uint64_t h, const char *t,
                            size_t n) {
  size_t j;
  if (ix->term_cap == 0) {
    return SIZE_MAX;
  }
  j = (size_t)(h & (uint64_t)(ix->term_cap - 1));
  while (ix->terms[j].term_off != SLOT_EMPTY) {
    if (ix->terms[j].term_off != SLOT_TOMB &&
        term_eq(ix, &ix->terms[j], h, t, n)) {
      return j;
    }
    j = (j + 1) & (ix->term_cap - 1);
  }
  return SIZE_MAX;
}

static const kbc_term_slot *term_find(const kbc_index *ix, uint64_t h,
                                      const char *t, size_t n) {
  size_t i = term_find_idx(ix, h, t, n);
  return i == SIZE_MAX ? NULL : &ix->terms[i];
}

/* Removal is a tombstone, never SLOT_EMPTY: a live entry may be further down
 * this slot's probe chain. */
static void term_remove_at(kbc_index *ix, size_t idx) {
  ix->terms[idx].term_off = SLOT_TOMB;
  ix->terms[idx].term_len = 0;
  ix->terms[idx].id = UINT32_MAX; /* the id is dead: end_build must not group by it */
  ix->terms[idx].post_off = 0;
  ix->terms[idx].post_len = 0;
  if (ix->term_live > 0) {
    ix->term_live--;
  }
  ix->term_tombs++;
}

/* --------------------------------------------------------- doc  hashing -- */

static void dhash_clear(kbc_index *ix) {
  size_t i;
  for (i = 0; i < ix->dhash_cap; i++) {
    ix->dhash[i].doc = SLOT_EMPTY;
  }
}

static kbc_status dhash_grow(kbc_index *ix, size_t want) {
  kbc_doc_slot *ns;
  size_t ncap = ix->dhash_cap ? ix->dhash_cap : 256, i;
  while (ncap / 2 < want) {
    if (ncap > SIZE_MAX / 2 || ncap * 2 > SIZE_MAX / sizeof(kbc_doc_slot)) {
      return KBC_ERR_NOMEM;
    }
    ncap *= 2;
  }
  if (ncap == ix->dhash_cap) {
    return KBC_OK;
  }
  ns = (kbc_doc_slot *)malloc(ncap * sizeof(kbc_doc_slot));
  if (!ns) {
    return KBC_ERR_NOMEM;
  }
  for (i = 0; i < ncap; i++) {
    ns[i].doc = SLOT_EMPTY;
  }
  for (i = 0; i < ix->dhash_cap; i++) {
    size_t j;
    if (ix->dhash[i].doc == SLOT_EMPTY) {
      continue;
    }
    j = (size_t)(ix->dhash[i].hash & (uint64_t)(ncap - 1));
    while (ns[j].doc != SLOT_EMPTY) {
      j = (j + 1) & (ncap - 1);
    }
    ns[j] = ix->dhash[i];
  }
  free(ix->dhash);
  ix->dhash = ns;
  ix->dhash_cap = ncap;
  return KBC_OK;
}

static void dhash_insert(kbc_index *ix, uint64_t h, uint32_t doc) {
  size_t j = (size_t)(h & (uint64_t)(ix->dhash_cap - 1));
  while (ix->dhash[j].doc != SLOT_EMPTY) {
    if (ix->dhash[j].hash == h && ix->dhash[j].doc == doc) {
      return;
    }
    j = (j + 1) & (ix->dhash_cap - 1);
  }
  ix->dhash[j].hash = h;
  ix->dhash[j].doc = doc;
}

static uint32_t dhash_lookup(const kbc_index *ix, uint64_t h, const char *corpus,
                             const char *path) {
  size_t j;
  if (ix->dhash_cap == 0) {
    return SLOT_EMPTY;
  }
  j = (size_t)(h & (uint64_t)(ix->dhash_cap - 1));
  while (ix->dhash[j].doc != SLOT_EMPTY) {
    if (ix->dhash[j].hash == h && ix->dhash[j].doc < ix->doc_count) {
      const kbc_doc_meta *d = &ix->docs[ix->dhash[j].doc];
      if (strcmp(d->corpus, corpus) == 0 && strcmp(d->path, path) == 0) {
        return ix->dhash[j].doc;
      }
    }
    j = (j + 1) & (ix->dhash_cap - 1);
  }
  return SLOT_EMPTY;
}

/* ------------------------------------------------------------- scratch --- */

static void scratch_free(kbc_index *ix) {
  if (!ix->sc) {
    return;
  }
  free(ix->sc->mi);
  free(ix->sc->mh);
  free(ix->sc->ts);
  free(ix->sc->tl);
  free(ix->sc->tf);
  free(ix->sc->toff);
  free(ix->sc->ci);
  free(ix->sc);
  ix->sc = NULL;
}

static kbc_status scratch_ensure(kbc_index *ix, size_t distinct) {
  kbc_scratch *sc = ix->sc;
  size_t mcap, n;
  if (!sc) {
    sc = (kbc_scratch *)calloc(1, sizeof(kbc_scratch));
    if (!sc) {
      return KBC_ERR_NOMEM;
    }
    ix->sc = sc;
  }
  mcap = round_up_pow2(distinct * 2 + 8);
  if (mcap == 0) {
    return KBC_ERR_NOMEM;
  }
  if (mcap > sc->mcap) {
    uint32_t *mi = (uint32_t *)realloc(sc->mi, mcap * sizeof(uint32_t));
    uint64_t *mh;
    size_t i;
    if (!mi) {
      return KBC_ERR_NOMEM;
    }
    sc->mi = mi;
    mh = (uint64_t *)realloc(sc->mh, mcap * sizeof(uint64_t));
    if (!mh) {
      return KBC_ERR_NOMEM;
    }
    sc->mh = mh;
    for (i = 0; i < mcap; i++) {
      sc->mi[i] = SLOT_EMPTY;
    }
    sc->mcap = mcap;
  }
  if (distinct > sc->cap) {
    n = distinct + distinct / 2 + 16;
    {
      const char **ts = (const char **)realloc(sc->ts, n * sizeof(char *));
      uint32_t *tl, *tf, *toff, *ci;
      if (!ts) {
        return KBC_ERR_NOMEM;
      }
      sc->ts = ts;
      tl = (uint32_t *)realloc(sc->tl, n * sizeof(uint32_t));
      if (!tl) {
        return KBC_ERR_NOMEM;
      }
      sc->tl = tl;
      tf = (uint32_t *)realloc(sc->tf, n * sizeof(uint32_t));
      if (!tf) {
        return KBC_ERR_NOMEM;
      }
      sc->tf = tf;
      toff = (uint32_t *)realloc(sc->toff, n * sizeof(uint32_t));
      if (!toff) {
        return KBC_ERR_NOMEM;
      }
      sc->toff = toff;
      ci = (uint32_t *)realloc(sc->ci, n * sizeof(uint32_t));
      if (!ci) {
        return KBC_ERR_NOMEM;
      }
      sc->ci = ci;
      sc->ci_cap = n;
      sc->cap = n;
    }
  }
  return KBC_OK;
}

static void scratch_reset(kbc_scratch *sc) {
  size_t i;
  for (i = 0; i < sc->mcap; i++) {
    sc->mi[i] = SLOT_EMPTY;
  }
  sc->len = 0;
  sc->ci_len = 0;
}

/* Counts tf per distinct term into the scratch map, interning nothing. */
static kbc_status scratch_count(kbc_scratch *sc, const kbc_tokens *toks,
                                kbc_err *err) {
  size_t i;
  scratch_reset(sc);
  for (i = 0; i < toks->len; i++) {
    const kbc_token *t = &toks->items[i];
    uint64_t h;
    size_t j;
    bool seen = false;
    if (t->len == 0 || t->len > KBC_MAX_TERM_LEN) {
      continue;
    }
    h = fnv1a64(t->text, t->len);
    j = (size_t)(h & (uint64_t)(sc->mcap - 1));
    while (sc->mi[j] != SLOT_EMPTY) {
      uint32_t k = sc->mi[j];
      if (sc->mh[j] == h && sc->tl[k] == t->len &&
          memcmp(sc->ts[k], t->text, t->len) == 0) {
        if (sc->tf[k] == UINT32_MAX) {
          return kbc_err_set(err, KBC_ERR_INVALID,
                             "term frequency overflow for \"%.*s\"",
                             (int)t->len, t->text);
        }
        sc->tf[k]++;
        seen = true;
        break;
      }
      j = (j + 1) & (sc->mcap - 1);
    }
    if (seen) {
      continue;
    }
    sc->mh[j] = h;
    sc->mi[j] = (uint32_t)sc->len;
    sc->ts[sc->len] = t->text;
    sc->tl[sc->len] = (uint32_t)t->len;
    sc->tf[sc->len] = 1;
    sc->len++;
  }
  return KBC_OK;
}

/* ---------------------------------------------------------------- acc ---- */

static void acc_free(kbc_acc *a) {
  if (!a) {
    return;
  }
  free(a->scores);
  free(a->stamps);
  free(a->touched);
  free(a->heap);
  free(a->q);
  free(a);
}

static kbc_acc *acc_new(void) { return (kbc_acc *)calloc(1, sizeof(kbc_acc)); }

/* Resizes the per-doc arrays; the next query starts a fresh generation, so no
 * O(N) clear is ever needed. */
static kbc_status acc_fit(kbc_acc *a, size_t docs) {
  if (a->doc_cap != docs) {
    size_t n = docs ? docs : 1;
    double *s = (double *)realloc(a->scores, n * sizeof(double));
    uint32_t *t, *u;
    if (!s) {
      return KBC_ERR_NOMEM;
    }
    a->scores = s;
    t = (uint32_t *)realloc(a->stamps, n * sizeof(uint32_t));
    if (!t) {
      return KBC_ERR_NOMEM;
    }
    a->stamps = t;
    u = (uint32_t *)realloc(a->touched, n * sizeof(uint32_t));
    if (!u) {
      return KBC_ERR_NOMEM;
    }
    a->touched = u;
    memset(a->stamps, 0, n * sizeof(uint32_t));
    a->doc_cap = docs;
    a->gen = 0;
  }
  return KBC_OK;
}

/* The query scratch is cached in THREAD-LOCAL storage, not on the index. Two
 * reasons, both load-bearing:
 *   - correctness: a sealed index is shared by every query thread (kbc_app's
 *     rwlock admits all of them at once), so a scratch reachable from it would
 *     be written concurrently. Per-thread scratch shares nothing.
 *   - cost: the scratch is O(doc_count) doubles and is reused for the life of
 *     the thread, so a keystroke search still allocates nothing after the first
 *     query on that thread. Allocating per call would be 3 reallocs + a memset
 *     of the whole score array on EVERY query — the reuse exists to avoid that.
 * The destructor is what keeps a long-lived daemon honest: a query thread that
 * exits hands its scratch back instead of leaking it. */
static pthread_key_t g_acc_key;
static pthread_once_t g_acc_once = PTHREAD_ONCE_INIT;
static bool g_acc_key_ok;

static void acc_dtor(void *p) { acc_free((kbc_acc *)p); }

static void acc_key_init(void) {
  g_acc_key_ok = pthread_key_create(&g_acc_key, acc_dtor) == 0;
}

/* Hands back the calling thread's scratch. When there is no thread-local slot
 * to own it (a failed pthread_key_create) the caller must free what it gets —
 * *ephemeral says which case this is. */
static kbc_acc *acc_get(bool *ephemeral) {
  kbc_acc *a;
  *ephemeral = false;
  if (pthread_once(&g_acc_once, acc_key_init) != 0) {
    *ephemeral = true;
    return acc_new();
  }
  if (!g_acc_key_ok) {
    *ephemeral = true;
    return acc_new();
  }
  a = (kbc_acc *)pthread_getspecific(g_acc_key);
  if (!a) {
    a = acc_new();
    if (a && pthread_setspecific(g_acc_key, a) != 0) {
      acc_free(a);
      a = NULL;
    }
  }
  if (!a) {
    *ephemeral = true;
    a = acc_new();
  }
  return a;
}

/* --------------------------------------------------------- little end --- */

static void put_u32(uint8_t *p, uint32_t v) {
  p[0] = (uint8_t)(v & 0xFFu);
  p[1] = (uint8_t)((v >> 8) & 0xFFu);
  p[2] = (uint8_t)((v >> 16) & 0xFFu);
  p[3] = (uint8_t)((v >> 24) & 0xFFu);
}

static void put_u64(uint8_t *p, uint64_t v) {
  put_u32(p, (uint32_t)(v & 0xFFFFFFFFu));
  put_u32(p + 4, (uint32_t)((v >> 32) & 0xFFFFFFFFu));
}

static uint32_t get_u32(const uint8_t *p) {
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
         ((uint32_t)p[3] << 24);
}

static uint64_t get_u64(const uint8_t *p) {
  return (uint64_t)get_u32(p) | ((uint64_t)get_u32(p + 4) << 32);
}

static size_t pad8(size_t n) { return n % 8 == 0 ? 0 : 8 - n % 8; }

/* ------------------------------------------------------------ lifetime --- */

kbc_index *kbc_index_new(void) { return (kbc_index *)calloc(1, sizeof(kbc_index)); }

void kbc_index_free(kbc_index *ix) {
  if (!ix) {
    return;
  }
  if (ix->map) {
    (void)munmap(ix->map, ix->map_len);
  }
  sar_free(&ix->tar);
  sar_free(&ix->dar);
  free(ix->terms);
  free(ix->dhash);
  free(ix->docs);
  free(ix->post);
  free(ix->post_term);
  scratch_free(ix);
  free(ix);
}

/* ------------------------------------------------------------- building -- */

kbc_status kbc_index_begin_build(kbc_index *ix, kbc_err *err) {
  if (!ix) {
    return kbc_err_set(err, KBC_ERR_INVALID, "kbc_index_begin_build: ix is NULL");
  }
  term_clear(ix);
  dhash_clear(ix);
  sar_clear(&ix->tar);
  sar_clear(&ix->dar);
  /* Both arenas are the heap's from here on: a build that reuses an index
   * opened from a file would otherwise leave the save writing the OLD mapping
   * under the new arena's lengths. */
  ix->tar_base = NULL;
  ix->dar_base = NULL;
  ix->doc_count = 0;
  ix->total_tokens = 0;
  ix->post_len = 0;
  free(ix->post_term);
  ix->post_term = NULL;
  ix->post_term_cap = 0;
  ix->next_term_id = 0;
  ix->sealed = false;
  scratch_free(ix); /* a stale scratch carries stale term offsets */
  return KBC_OK;
}

kbc_status kbc_index_add_doc(kbc_index *ix, uint32_t doc_id, const char *corpus,
                             const char *path, const char *title, kbc_kind kind,
                             const kbc_tokens *toks, kbc_err *err) {
  static const kbc_tokens empty = {NULL, 0, 0};
  const kbc_tokens *t = toks ? toks : &empty;
  kbc_scratch *sc;
  size_t n, i, post_before;
  uint32_t coff, poff, toff;

  if (!ix || !corpus || !path) {
    return kbc_err_set(err, KBC_ERR_INVALID,
                       "kbc_index_add_doc(doc %u): corpus or path is NULL",
                       doc_id);
  }
  if (ix->sealed) {
    return kbc_err_set(err, KBC_ERR_INVALID,
                       "kbc_index_add_doc(doc %u): the index is sealed", doc_id);
  }
  if (doc_id != ix->doc_count) {
    return kbc_err_set(err, KBC_ERR_INVALID,
                       "kbc_index_add_doc(doc %u): doc ids must be dense and "
                       "monotonic, expected %u",
                       doc_id, ix->doc_count);
  }
  if ((uint32_t)kind >= (uint32_t)KBC_KIND__COUNT) {
    return kbc_err_set(err, KBC_ERR_INVALID, "kbc_index_add_doc(doc %u): kind "
                                             "%d is not a kbc_kind",
                       doc_id, (int)kind);
  }
  if (strlen(corpus) > KBC_MAX_CORPORA || strlen(path) > KBC_MAX_PATH_LEN) {
    return kbc_err_set(err, KBC_ERR_INVALID,
                       "kbc_index_add_doc(doc %u): corpus/path too long "
                       "(%zu/%zu bytes)",
                       doc_id, strlen(corpus), strlen(path));
  }
  if (t->len > KBC_MAX_TOKENS_PER_DOC) {
    return kbc_err_set(err, KBC_ERR_INVALID,
                       "kbc_index_add_doc(doc %u): %zu tokens exceeds the %u "
                       "limit",
                       doc_id, t->len, KBC_MAX_TOKENS_PER_DOC);
  }
  if (t->len > 0 && !t->items) {
    return kbc_err_set(err, KBC_ERR_INVALID,
                       "kbc_index_add_doc(doc %u): %zu tokens but no items",
                       doc_id, t->len);
  }

  if (scratch_ensure(ix, t->len) != KBC_OK) {
    return kbc_err_set(err, KBC_ERR_NOMEM,
                       "kbc_index_add_doc(doc %u): scratch alloc failed", doc_id);
  }
  sc = ix->sc;
  if (scratch_count(sc, t, err) != KBC_OK) {
    return err ? err->status : KBC_ERR_INVALID;
  }
  n = sc->len;

  /* Grow everything the commit loop needs so the loop itself cannot fail. */
  if (dhash_grow(ix, (size_t)ix->doc_count + 1) != KBC_OK) {
    return kbc_err_set(err, KBC_ERR_NOMEM,
                       "kbc_index_add_doc(doc %u): doc hash alloc failed", doc_id);
  }
  if (n > 0) {
    size_t need = ((ix->term_live + ix->term_tombs + n) * 1000) / LOAD_PERMILLE +
                  2;
    /* The commit loop probes ix->terms unconditionally, so the table must
     * exist before the first document, not merely be big enough: with
     * term_cap 0 the "default 1024" makes a small document's need look
     * satisfied and leaves the table NULL. */
    if (ix->term_cap == 0 || need > ix->term_cap) {
      if (term_grow(ix, need) != KBC_OK) {
        return kbc_err_set(err, KBC_ERR_NOMEM,
                           "kbc_index_add_doc(doc %u): term table alloc failed",
                           doc_id);
      }
    }
    if (n > ix->post_cap - ix->post_len) {
      size_t cap = ix->post_cap ? ix->post_cap : 1024;
      kbc_posting *np;
      while (cap - ix->post_len < n) {
        if (cap > SIZE_MAX / 2 || cap * 2 > SIZE_MAX / sizeof(kbc_posting)) {
          return kbc_err_set(err, KBC_ERR_NOMEM,
                             "kbc_index_add_doc(doc %u): postings overflow",
                             doc_id);
        }
        cap *= 2;
      }
      np = (kbc_posting *)realloc(ix->post, cap * sizeof(kbc_posting));
      if (!np) {
        return kbc_err_set(err, KBC_ERR_NOMEM,
                           "kbc_index_add_doc(doc %u): postings alloc failed",
                           doc_id);
      }
      ix->post = np;
      ix->post_cap = cap;
    }
    /* The sidecar tracks ix->post one-for-one, so it is sized independently of
     * whether the postings array happened to need to grow — the commit loop
     * writes both and must not be able to fail halfway. */
    if (n > ix->post_term_cap - ix->post_len) {
      size_t cap = ix->post_term_cap ? ix->post_term_cap : 1024;
      uint32_t *nt;
      while (cap - ix->post_len < n) {
        if (cap > SIZE_MAX / 2 || cap * 2 > SIZE_MAX / sizeof(uint32_t)) {
          return kbc_err_set(err, KBC_ERR_NOMEM,
                             "kbc_index_add_doc(doc %u): posting term sidecar "
                             "overflow",
                             doc_id);
        }
        cap *= 2;
      }
      nt = (uint32_t *)realloc(ix->post_term, cap * sizeof(*nt));
      if (!nt) {
        return kbc_err_set(err, KBC_ERR_NOMEM,
                           "kbc_index_add_doc(doc %u): posting term sidecar "
                           "alloc failed",
                           doc_id);
      }
      ix->post_term = nt;
      ix->post_term_cap = cap;
    }
  }
  if (n > 0 && ix->post_len + n > (size_t)UINT32_MAX) {
    return kbc_err_set(err, KBC_ERR_INVALID,
                       "kbc_index_add_doc(doc %u): the index would exceed %u "
                       "postings, the format cannot hold it",
                       doc_id, UINT32_MAX);
  }

  /* Intern the new terms before touching any table. */
  for (i = 0; i < n; i++) {
    if (sar_put(&ix->tar, sc->ts[i], sc->tl[i], &sc->toff[i]) != KBC_OK) {
      return kbc_err_set(err, KBC_ERR_NOMEM,
                         "kbc_index_add_doc(doc %u): term arena alloc failed "
                         "for \"%.*s\"",
                         doc_id, (int)sc->tl[i], sc->ts[i]);
    }
  }
  if (ix->doc_count == ix->doc_cap) {
    uint32_t ncap;
    kbc_doc_meta *nd;
    if (ix->doc_cap > UINT32_MAX / 2) {
      return kbc_err_set(err, KBC_ERR_NOMEM,
                         "kbc_index_add_doc(doc %u): the doc table cannot grow "
                         "past %u entries",
                         doc_id, UINT32_MAX);
    }
    ncap = ix->doc_cap ? ix->doc_cap * 2 : 256;
    if (ncap < ix->doc_count + 1) {
      return kbc_err_set(err, KBC_ERR_NOMEM,
                         "kbc_index_add_doc(doc %u): doc table overflow", doc_id);
    }
    nd = (kbc_doc_meta *)realloc(ix->docs, (size_t)ncap * sizeof(kbc_doc_meta));
    if (!nd) {
      return kbc_err_set(err, KBC_ERR_NOMEM,
                         "kbc_index_add_doc(doc %u): doc table alloc failed",
                         doc_id);
    }
    ix->docs = nd;
    ix->doc_cap = ncap;
  }
  if (sar_put(&ix->dar, corpus, strlen(corpus), &coff) != KBC_OK ||
      sar_put(&ix->dar, path, strlen(path), &poff) != KBC_OK ||
      sar_put(&ix->dar, title ? title : "", title ? strlen(title) : 0, &toff) !=
          KBC_OK) {
    return kbc_err_set(err, KBC_ERR_NOMEM,
                       "kbc_index_add_doc(doc %u): doc string arena alloc failed",
                       doc_id);
  }
  ix->docs[ix->doc_count].corpus = sar_ptr(&ix->dar, coff);
  ix->docs[ix->doc_count].path = sar_ptr(&ix->dar, poff);
  ix->docs[ix->doc_count].title = sar_ptr(&ix->dar, toff);
  ix->docs[ix->doc_count].kind = (uint8_t)kind;
  ix->docs[ix->doc_count].token_count = (uint32_t)t->len;

  /* The commit loop below hands out term ids; check the space once, up front,
   * so exhausting it cannot leave a half-claimed slot behind. */
  if (n > 0 && ix->next_term_id == UINT32_MAX) {
    return kbc_err_set(err, KBC_ERR_INVALID,
                       "kbc_index_add_doc(doc %u): the term id space is "
                       "exhausted (over %u terms)",
                       doc_id, UINT32_MAX);
  }

  /* Commit. Invariant: doc_id is strictly increasing across calls, so each
   * posting list stays ascending by doc_id. */
  post_before = ix->post_len;
  for (i = 0; i < n; i++) {
    uint64_t th = fnv1a64(sc->ts[i], sc->tl[i]);
    size_t ti = term_find_idx(ix, th, sc->ts[i], sc->tl[i]);
    kbc_term_slot *sl;
    if (ti == SIZE_MAX) {
      size_t j = (size_t)(th & (uint64_t)(ix->term_cap - 1));
      while (ix->terms[j].term_off != SLOT_EMPTY) {
        j = (j + 1) & (ix->term_cap - 1);
      }
      ti = j;
      sl = &ix->terms[j];
      sl->hash = th;
      sl->term_off = sc->toff[i];
      sl->term_len = sc->tl[i];
      sl->post_off = (uint32_t)ix->post_len;
      sl->post_len = 0;
      sl->id = ix->next_term_id++;
      ix->term_live++;
      if (sc->ci_len < sc->ci_cap) {
        sc->ci[sc->ci_len++] = (uint32_t)ti;
      }
    }
    sl = &ix->terms[ti];
    if (sc->ci_len == 0 || sc->ci[sc->ci_len - 1] != (uint32_t)ti) {
      if (sc->ci_len < sc->ci_cap) {
        sc->ci[sc->ci_len++] = (uint32_t)ti;
      }
    }
    if (sl->post_len == UINT32_MAX) {
      /* Unreachable below 4G postings for one term, but the rollback is the
       * contract: nothing this call appended survives. */
      size_t k;
      for (k = 0; k < sc->ci_len; k++) {
        kbc_term_slot *t2 = &ix->terms[sc->ci[k]];
        if (t2->post_len == 1 && (size_t)t2->post_off >= post_before) {
          term_remove_at(ix, sc->ci[k]); /* created by this call */
        } else if (t2->post_len > 0) {
          t2->post_len--; /* existed before: drop the posting we appended */
        }
      }
      ix->post_len = post_before;
      return kbc_err_set(err, KBC_ERR_INVALID,
                         "kbc_index_add_doc(doc %u): term \"%.*s\" has %u "
                         "postings, the format cannot hold more",
                         doc_id, (int)sc->tl[i], sc->ts[i], sl->post_len);
    }
    ix->post[ix->post_len].doc = doc_id;
    ix->post[ix->post_len].tf = sc->tf[i];
    ix->post_term[ix->post_len] = sl->id;
    ix->post_len++;
    sl->post_len++;
  }
  dhash_insert(ix, hash_corpus_path(corpus, path), doc_id);
  ix->doc_count++;
  ix->total_tokens += (uint64_t)t->len;
  return KBC_OK;
}

kbc_status kbc_index_end_build(kbc_index *ix, kbc_err *err) {
  size_t i;
  if (!ix) {
    return kbc_err_set(err, KBC_ERR_INVALID, "kbc_index_end_build: ix is NULL");
  }
  /* The build-time append interleaves terms in the global postings array (one
   * posting per distinct term, in document order), so a term's postings are
   * NOT the contiguous run every other layer — the header, the on-disk
   * TSLOT/POST records, kbc_index_bm25 — assumes they are, and post_len is a
   * COUNT, not a span, so the slots alone cannot say which postings are whose.
   * The build-time sidecar ix->post_term does. Materialize the runs here, once:
   * a stable counting sort keyed on the term's STABLE ID (kbc_term_slot.id),
   * with the runs laid out in slot order. Stability keeps doc_id ascending
   * inside every run (documents are appended in ascending doc_id order), so
   * nothing is compared or reordered. O(postings), no extra hashing.
   * Failure-atomic: every scratch buffer is obtained and filled before
   * anything is swapped, so a NOMEM leaves the index exactly as it was.
   *
   * The key MUST be the id, never the slot index: term_grow rehashes and
   * moves every live term, so a posting's build-time slot index names a
   * different term after the first grow (only a corpus past the initial table
   * size ever grows, which is why small tests never saw it). Ids are handed
   * out once, at creation, and term_grow does not renumber them. */
  {
    size_t total = 0, run = 0, k, nids = ix->next_term_id;
    kbc_posting *np;
    uint32_t *cursor, *slot_of = NULL;

    for (i = 0; i < ix->term_cap; i++) {
      const kbc_term_slot *s = &ix->terms[i];
      if (s->term_off == SLOT_EMPTY || s->term_off == SLOT_TOMB) {
        continue;
      }
      total += s->post_len;
    }
    if (total == 0) {
      free(ix->post_term);
      ix->post_term = NULL;
      ix->post_term_cap = 0;
      ix->sealed = true;
      scratch_free(ix);
      return KBC_OK;
    }
    np = (kbc_posting *)malloc(total * sizeof(*np));
    cursor = ix->term_cap ? (uint32_t *)malloc(ix->term_cap * sizeof(*cursor))
                          : NULL;
    if (nids > 0) {
      slot_of = (uint32_t *)malloc(nids * sizeof(*slot_of));
      if (slot_of) {
        for (k = 0; k < nids; k++) {
          slot_of[k] = UINT32_MAX;
        }
      }
    }
    if (!np || (ix->term_cap && !cursor) || (nids > 0 && !slot_of)) {
      free(np);
      free(cursor);
      free(slot_of);
      return kbc_err_set(err, KBC_ERR_NOMEM,
                         "kbc_index_end_build: cannot allocate the packed "
                         "postings array (%zu postings over %zu term slots, "
                         "%zu term ids)",
                         total, (size_t)ix->term_cap, nids);
    }
    for (i = 0; i < ix->term_cap; i++) {
      kbc_term_slot *s = &ix->terms[i];
      if (s->term_off == SLOT_EMPTY || s->term_off == SLOT_TOMB) {
        continue;
      }
      s->post_off = (uint32_t)run;
      cursor[i] = (uint32_t)run;
      if (s->id < nids && slot_of[s->id] == UINT32_MAX) {
        slot_of[s->id] = (uint32_t)i;
      }
      run += s->post_len;
    }
    for (k = 0; k < ix->post_len; k++) {
      uint32_t id = ix->post_term[k];
      uint32_t ti;
      /* A term retired mid-build (only the add_doc overflow rollback does that,
       * and it truncates ix->post_len with it) leaves no run; the postings it
       * owned are dead weight and are dropped here. The bound check is the
       * belt to that braces: a sidecar id can never be out of the id space,
       * and the UINT32_MAX lookup can never read out of the slot table. */
      if (id >= nids) {
        continue;
      }
      ti = slot_of[id];
      if (ti == UINT32_MAX) {
        continue;
      }
      np[cursor[ti]++] = ix->post[k];
    }
    free(cursor);
    free(slot_of);
    free(ix->post);
    free(ix->post_term);
    ix->post = np;
    ix->post_len = total;
    ix->post_cap = total;
    ix->post_term = NULL;
    ix->post_term_cap = 0;
  }
  /* Assert the invariant the rest of the file (and the format) depends on:
   * every term's postings are now one contiguous doc_id-ascending run. */
  for (i = 0; i < ix->term_cap; i++) {
    const kbc_term_slot *s = &ix->terms[i];
    const char *t;
    size_t j;
    if (s->term_off == SLOT_EMPTY || s->term_off == SLOT_TOMB) {
      continue;
    }
    for (j = 1; j < s->post_len; j++) {
      if (ix->post[s->post_off + j - 1].doc >= ix->post[s->post_off + j].doc) {
        t = term_ptr(ix, s->term_off);
        return kbc_err_set(err, KBC_ERR_INTERNAL,
                           "kbc_index_end_build: postings for \"%.*s\" are not "
                           "doc_id-ascending at %zu",
                           (int)s->term_len, t ? t : "", j);
      }
    }
  }
  ix->sealed = true;
  scratch_free(ix);
  return KBC_OK;
}

/* --------------------------------------------------------- incremental --
 * A live index is mutated in place, one document at a time, by the two entry
 * points at the end of this section. Everything here exists to keep the one
 * layout claim end_build made — every term owns one contiguous, doc_id
 * ascending run of the global postings array — true AFTER a mutation, so the
 * query path, the on-disk format and kbc_index_open are all unchanged by
 * incremental work. The read path gains nothing and loses nothing: a query
 * still scans one array with no indirection.
 *
 * The rewrite is a single O(postings) pass rather than one memmove per
 * touched term. In one global array an insertion into the middle of a term's
 * run shifts every LATER term's offset, so the per-term operations are not
 * independent; doing them one at a time is O(touched_terms * postings). Doing
 * them together — lay the runs out, merge each run's old postings with that
 * run's new ones — is O(postings) once, and the pass is a sequential read
 * plus a sequential write over memory the daemon is going to touch anyway.
 */

/* One posting an update asks for. The SLOT, not the term id: every term is
 * resolved before the rewrite starts and nothing rehashes the table after
 * that, so a slot index names one term for the whole call. An id would not:
 * an index loaded from disk carries none until it is rebuilt in place. */
typedef struct {
  size_t slot;
  uint32_t doc;
  uint32_t tf;
} kbc_add;

/* The update's add list keyed on slot, so the rewrite can ask "does this term
 * get a posting?" in O(1) per posting instead of scanning the list per
 * posting. Open addressed, SLOT_EMPTY when free. */
typedef struct {
  uint32_t *slot;
  uint32_t *val; /* index into the caller's adds[] */
  size_t cap;
} kbc_addmap;

static void addmap_free(kbc_addmap *m) {
  free(m->slot);
  free(m->val);
  m->slot = NULL;
  m->val = NULL;
  m->cap = 0;
}

static kbc_status addmap_build(kbc_addmap *m, const kbc_add *adds, size_t n) {
  size_t i;
  memset(m, 0, sizeof *m);
  m->cap = round_up_pow2(n * 2 + 8);
  if (m->cap == 0) {
    return KBC_ERR_NOMEM;
  }
  m->slot = (uint32_t *)malloc(m->cap * sizeof(uint32_t));
  m->val = (uint32_t *)malloc(m->cap * sizeof(uint32_t));
  if (!m->slot || !m->val) {
    addmap_free(m);
    return KBC_ERR_NOMEM;
  }
  for (i = 0; i < m->cap; i++) {
    m->slot[i] = SLOT_EMPTY;
  }
  for (i = 0; i < n; i++) {
    size_t j = adds[i].slot & (m->cap - 1);
    while (m->slot[j] != SLOT_EMPTY) {
      j = (j + 1) & (m->cap - 1);
    }
    m->slot[j] = (uint32_t)adds[i].slot;
    m->val[j] = (uint32_t)i;
  }
  return KBC_OK;
}

/* The index of this slot's add, if it has one. A term can carry at most one:
 * the update resolves the document's DISTINCT terms, so a repeated term is
 * already folded into one tf. */
static bool addmap_get(const kbc_addmap *m, size_t slot, size_t *out) {
  size_t j;
  if (m->cap == 0) {
    return false;
  }
  j = slot & (m->cap - 1);
  while (m->slot[j] != SLOT_EMPTY) {
    if (m->slot[j] == (uint32_t)slot) {
      *out = m->val[j];
      return true;
    }
    j = (j + 1) & (m->cap - 1);
  }
  return false;
}

/* Whether this term's run contains `doc`. This is the reverse direction — the
 * terms a document contributed to — answered by a binary search per term
 * rather than a doc -> terms side index. The run is ascending by
 * construction, so a removal costs O(terms * log df) and the index carries no
 * per-document reverse structure that the read path would have to keep alive.
 */
static bool run_has_doc(const kbc_index *ix, const kbc_term_slot *s,
                        uint32_t doc) {
  size_t lo = 0, hi = s->post_len;
  while (lo < hi) {
    size_t mid = lo + (hi - lo) / 2;
    uint32_t d = ix->post[s->post_off + mid].doc;
    if (d == doc) {
      return true;
    }
    if (d < doc) {
      lo = mid + 1;
    } else {
      hi = mid;
    }
  }
  return false;
}

/* The one mutation primitive. Rewrites ix->post so that afterwards:
 *   - every live term owns one contiguous, doc_id-ascending run;
 *   - no posting names `drop` (when has_drop);
 *   - every doc id >= `shift` has moved down by one (when has_shift), which is
 *     the compaction that keeps doc ids dense after a removal;
 *   - each entry of `adds` is present exactly once, at its ascending position.
 * An update passes drop = the document it is replacing, so the terms the
 * document no longer has lose their posting here — the ghost-posting case —
 * and the terms it still has get the new tf in the same pass.
 *
 * Failure-atomic: the new array is allocated and filled completely before the
 * swap, so a NOMEM (or the invariant check below) leaves every term slot, the
 * doc table and the totals exactly as they were.
 */
static kbc_status postings_rewrite(kbc_index *ix, bool has_drop, uint32_t drop,
                                   bool has_shift, uint32_t shift,
                                   const kbc_add *adds, size_t nadds,
                                   const char *who, kbc_err *err) {
  kbc_addmap m;
  kbc_posting *np = NULL;
  uint32_t *nl = NULL, *noff = NULL, *cur = NULL;
  size_t i, run = 0, total = 0;
  kbc_status st = KBC_OK;

  if (nadds > 0 && ix->term_cap == 0) {
    return kbc_err_set(err, KBC_ERR_INTERNAL,
                       "%s: %zu postings to add but the term table is empty",
                       who, nadds);
  }
  if (addmap_build(&m, adds, nadds) != KBC_OK) {
    return kbc_err_set(err, KBC_ERR_NOMEM, "%s: add map for %zu terms", who,
                       nadds);
  }
  if (ix->term_cap > 0) {
    nl = (uint32_t *)malloc(ix->term_cap * sizeof(uint32_t));
    noff = (uint32_t *)malloc(ix->term_cap * sizeof(uint32_t));
    cur = (uint32_t *)malloc(ix->term_cap * sizeof(uint32_t));
  }
  if ((ix->term_cap > 0 && (!nl || !noff || !cur))) {
    st = KBC_ERR_NOMEM;
    goto done;
  }

  /* Pass 1: every term's new run length, so the runs can be laid out. */
  for (i = 0; i < ix->term_cap; i++) {
    const kbc_term_slot *s = &ix->terms[i];
    uint32_t len = 0;
    size_t k;
    if (s->term_off == SLOT_EMPTY || s->term_off == SLOT_TOMB) {
      continue;
    }
    len = s->post_len;
    if (has_drop && run_has_doc(ix, s, drop)) {
      len--;
    }
    if (addmap_get(&m, i, &k)) {
      len++;
    }
    nl[i] = len;
    total += len;
  }
  if (total > (size_t)UINT32_MAX) {
    st = kbc_err_set(err, KBC_ERR_INVALID,
                     "%s: the index would hold %zu postings, the format cannot "
                     "hold more",
                     who, total);
    goto done;
  }
  if (total > 0) {
    np = (kbc_posting *)malloc(total * sizeof(*np));
    if (!np) {
      st = kbc_err_set(err, KBC_ERR_NOMEM, "%s: %zu postings", who, total);
      goto done;
    }
  }
  for (i = 0; i < ix->term_cap; i++) {
    if (ix->terms[i].term_off == SLOT_EMPTY || ix->terms[i].term_off == SLOT_TOMB) {
      continue;
    }
    noff[i] = (uint32_t)run;
    cur[i] = (uint32_t)run;
    run += nl[i];
  }

  /* Pass 2: merge, in slot order, so the runs stay contiguous. The slot's old
   * post_off/post_len are still intact here — the new ones are only committed
   * after the whole array is written. */
  for (i = 0; i < ix->term_cap; i++) {
    const kbc_term_slot *s = &ix->terms[i];
    size_t k, kk = 0;
    bool pending;
    if (s->term_off == SLOT_EMPTY || s->term_off == SLOT_TOMB) {
      continue;
    }
    pending = addmap_get(&m, i, &kk);
    for (k = 0; k < s->post_len; k++) {
      kbc_posting p = ix->post[s->post_off + k];
      uint32_t d;
      if (has_drop && p.doc == drop) {
        if (pending) {
          np[cur[i]].doc = adds[kk].doc;
          np[cur[i]].tf = adds[kk].tf;
          cur[i]++;
          pending = false;
        }
        continue; /* this document's previous posting for this term */
      }
      d = (has_shift && p.doc >= shift) ? p.doc - 1 : p.doc;
      if (pending && adds[kk].doc <= d) {
        np[cur[i]].doc = adds[kk].doc;
        np[cur[i]].tf = adds[kk].tf;
        cur[i]++;
        pending = false;
      }
      if (d >= ix->doc_count) {
        continue; /* defensive: no posting may name a document that is gone */
      }
      np[cur[i]].doc = d;
      np[cur[i]].tf = p.tf;
      cur[i]++;
    }
    if (pending) {
      np[cur[i]].doc = adds[kk].doc;
      np[cur[i]].tf = adds[kk].tf;
      cur[i]++;
    }
    /* The length pass and the merge must agree, or the runs would overlap. */
    if (cur[i] != noff[i] + nl[i]) {
      st = kbc_err_set(err, KBC_ERR_INTERNAL,
                       "%s: term slot %zu wrote %u postings, planned %u", who, i,
                       cur[i] - noff[i], nl[i]);
      goto done;
    }
  }

  /* The invariant the format and the query scan both depend on, re-established
   * rather than assumed: one ascending run per term. A violation here means a
   * mutation left a posting behind or inserted one twice, and publishing that
   * would silently mis-score the whole corpus — so it is an error, not a
   * warning. */
  for (i = 0; i < ix->term_cap; i++) {
    const kbc_term_slot *s = &ix->terms[i];
    size_t j;
    if (s->term_off == SLOT_EMPTY || s->term_off == SLOT_TOMB) {
      continue;
    }
    for (j = 1; j < nl[i]; j++) {
      if (np[noff[i] + j - 1].doc >= np[noff[i] + j].doc) {
        const char *t = term_ptr(ix, s->term_off);
        st = kbc_err_set(err, KBC_ERR_INTERNAL,
                         "%s: postings for \"%.*s\" are not doc_id-ascending at "
                         "%zu",
                         who, (int)s->term_len, t ? t : "", j);
        goto done;
      }
    }
  }

  /* Commit. */
  for (i = 0; i < ix->term_cap; i++) {
    kbc_term_slot *s = &ix->terms[i];
    if (s->term_off == SLOT_EMPTY || s->term_off == SLOT_TOMB) {
      continue;
    }
    s->post_off = noff[i];
    s->post_len = nl[i];
  }
  free(ix->post);
  ix->post = np;
  ix->post_len = total;
  ix->post_cap = total;
  np = NULL;

done:
  free(np);
  free(nl);
  free(noff);
  free(cur);
  addmap_free(&m);
  return st;
}

/* A term whose last posting went away is not a term: a full rebuild of the
 * same corpus would never have created it, so leaving it would make
 * kbc_index_term_count (and every prefix expansion) disagree with a rebuild. */
static void prune_empty_terms(kbc_index *ix) {
  size_t i;
  for (i = 0; i < ix->term_cap; i++) {
    const kbc_term_slot *s = &ix->terms[i];
    if (s->term_off == SLOT_EMPTY || s->term_off == SLOT_TOMB) {
      continue;
    }
    if (s->post_len == 0) {
      term_remove_at(ix, i);
    }
  }
}

/* An index opened from disk points its TERM strings into the mapping, and
 * kbc_index_save writes the heap arenas. A mutation is the first thing that
 * needs to append a term to that arena, so the first one copies it onto the
 * heap. The copy is a memcpy of one contiguous region, not a rebuild: the
 * bytes a later save writes are byte-identical to the bytes that were read.
 * (The doc arena never needs this — every mutation rebuilds it whole, see
 * doctab_build.) */
static kbc_status materialize_term_arena(kbc_index *ix) {
  uint32_t off;
  size_t declared;
  if (ix->tar_base == NULL) {
    return KBC_OK;
  }
  /* tar.total is the LENGTH THE FILE DECLARED, and the heap arena behind it is
   * still empty: the copy is what fills it, so the counter has to start at
   * zero or every byte is counted twice and the next save writes a shorter
   * file than its own header describes. */
  declared = ix->tar.total;
  ix->tar.total = 0;
  if (sar_put(&ix->tar, ix->tar_base, declared, &off) != KBC_OK) {
    ix->tar.total = declared; /* nothing was allocated: unchanged */
    return KBC_ERR_NOMEM;
  }
  ix->tar_base = NULL;
  return KBC_OK;
}

/* The document table a mutation is about to install, and the string arena that
 * goes with it. Built whole, off to the side, and only swapped in once every
 * fallible step has succeeded.
 *
 * The rebuild is not tidiness. The on-disk doc table carries string LENGTHS
 * only, and the loader rebuilds every pointer by walking the doc arena in
 * doc-id order, so the arena has to BE in doc-id order with nothing dead
 * between one document's three strings and the next. A replace that appended
 * its new strings would leave them after the documents that follow it, and
 * the file would decode into the wrong documents. Relaying the arena is a
 * memcpy of the live strings — the same order of cost as the postings rewrite
 * that has already run by the time this is called. */
typedef struct {
  kbc_doc_meta *docs;
  uint32_t count;
  kbc_arena_str dar;
  kbc_doc_slot *dhash;
  size_t dhash_cap;
} kbc_doctab;

static void doctab_free(kbc_doctab *dt) {
  free(dt->docs);
  free(dt->dhash);
  sar_free(&dt->dar);
  dt->docs = NULL;
  dt->dhash = NULL;
  dt->count = 0;
}

/* Copies one string into the flat block and hands back where it landed. The
 * arena is exactly sized, so there is no block to roll over to. */
static const char *doctab_put(kbc_arena_str *ar, const char *s) {
  kbc_arena_block *b = &ar->blocks[0];
  size_t n = strlen(s) + 1;
  char *p = b->p + b->used;
  memcpy(p, s, n);
  b->used += n;
  ar->total += n;
  return p;
}

/* Which document of the CURRENT table supplies output row `i`: itself, unless
 * the row is the caller's new document, or the removal has already shifted the
 * ones above the hole down by one. NULL means "the caller's values". */
static const kbc_doc_meta *doctab_source(const kbc_index *ix, bool remove,
                                         uint32_t slot, size_t i) {
  if (remove) {
    return &ix->docs[i < slot ? i : i + 1]; /* the hole closes up */
  }
  if (slot == UINT32_MAX) {
    /* An append: the table is unchanged and the new document is the row after
     * the last one. */
    return (i < ix->doc_count) ? &ix->docs[i] : NULL;
  }
  if (i < (size_t)slot) {
    return &ix->docs[i];
  }
  if (i == (size_t)slot) {
    return NULL; /* the caller's document */
  }
  return &ix->docs[i]; /* a replace moves nothing */
}

/* The prospective table. `remove` drops slot `slot` and slides everything above
 * it down one; otherwise `slot` is the document the caller's corpus / path /
 * title / kind / token_count replace (UINT32_MAX to append one). Either way the
 * result is a whole table, a whole arena and a whole doc hash that agree with
 * each other, built off to the side. */
static kbc_status doctab_build(kbc_doctab *out, const kbc_index *ix,
                               bool remove, uint32_t slot, const char *corpus,
                               const char *path, const char *title,
                               kbc_kind kind, uint32_t token_count,
                               const char *who, kbc_err *err) {
  /* A removal loses one, an append gains one, and a replace changes neither:
   * the document is being rewritten where it stands. */
  uint32_t n = remove ? ix->doc_count - 1u
                      : ix->doc_count + ((slot == UINT32_MAX) ? 1u : 0u);
  size_t i, total = 0;
  kbc_doc_meta *nd;
  memset(out, 0, sizeof *out);
  for (i = 0; i < n; i++) {
    const kbc_doc_meta *d = doctab_source(ix, remove, slot, i);
    const char *c = d ? d->corpus : corpus;
    const char *p = d ? d->path : path;
    const char *t = d ? (d->title ? d->title : "") : (title ? title : "");
    size_t need = strlen(c) + strlen(p) + strlen(t) + 3;
    if (total > SIZE_MAX - need) {
      return kbc_err_set(err, KBC_ERR_NOMEM, "%s: doc strings overflow", who);
    }
    total += need;
  }
  nd = (kbc_doc_meta *)malloc((n ? n : 1) * sizeof(*nd));
  out->dar.blocks = (kbc_arena_block *)malloc(sizeof(kbc_arena_block));
  out->dar.cap_blocks = 1;
  out->dar.blocks[0].p = (char *)malloc(total ? total : 1);
  if (!nd || !out->dar.blocks || !out->dar.blocks[0].p) {
    free(nd);
    free(out->dar.blocks);
    out->dar.blocks = NULL;
    out->dar.cap_blocks = 0;
    return kbc_err_set(err, KBC_ERR_NOMEM, "%s: doc table of %u documents "
                       "(%zu bytes of strings)",
                       who, n, total);
  }
  out->dar.blocks[0].size = total ? total : 1;
  out->dar.blocks[0].used = 0;
  out->dar.blocks[0].base = 0;
  out->dar.nblocks = 1;
  for (i = 0; i < n; i++) {
    const kbc_doc_meta *d = doctab_source(ix, remove, slot, i);
    nd[i].corpus = doctab_put(&out->dar, d ? d->corpus : corpus);
    nd[i].path = doctab_put(&out->dar, d ? d->path : path);
    nd[i].title = doctab_put(&out->dar, d ? (d->title ? d->title : "")
                                           : (title ? title : ""));
    nd[i].kind = (uint8_t)(d ? d->kind : kind);
    nd[i].token_count = (d ? d->token_count : token_count);
  }
  out->docs = nd;
  out->count = n;
  /* The doc hash is rebuilt with the table rather than patched. A removal
   * renumbers the documents above it and a dhash slot names a doc id, so a
   * slot left pointing at the old id would resolve — after the renumbering —
   * to a DIFFERENT document, and a slot pointing past the end is a file
   * kbc_index_open refuses outright. Rebuilding is O(documents) off to the
   * side, and it is also what keeps dead slots from accumulating over the life
   * of a daemon that churns through a corpus. */
  out->dhash_cap = round_up_pow2((size_t)n * 2 + 16);
  if (out->dhash_cap < 256) {
    out->dhash_cap = 256;
  }
  if (out->dhash_cap == 0) {
    doctab_free(out);
    return kbc_err_set(err, KBC_ERR_NOMEM, "%s: doc hash capacity overflow",
                       who);
  }
  out->dhash = (kbc_doc_slot *)malloc(out->dhash_cap * sizeof(*out->dhash));
  if (!out->dhash) {
    doctab_free(out);
    return kbc_err_set(err, KBC_ERR_NOMEM, "%s: doc hash of %zu slots", who,
                       out->dhash_cap);
  }
  for (i = 0; i < out->dhash_cap; i++) {
    out->dhash[i].doc = SLOT_EMPTY;
  }
  for (i = 0; i < n; i++) {
    uint64_t h = hash_corpus_path(nd[i].corpus, nd[i].path);
    size_t j = (size_t)(h & (uint64_t)(out->dhash_cap - 1));
    while (out->dhash[j].doc != SLOT_EMPTY) {
      j = (j + 1) & (out->dhash_cap - 1);
    }
    out->dhash[j].hash = h;
    out->dhash[j].doc = (uint32_t)i;
  }
  return KBC_OK;
}

/* Every check kbc_index_add_doc makes about its arguments, because an update
 * writes the same fields into the same structures. */
static kbc_status update_validate(const kbc_index *ix, uint32_t doc_id,
                                 const char *corpus, const char *path,
                                 kbc_kind kind, const kbc_tokens *toks,
                                 kbc_err *err) {
  static const kbc_tokens empty = {NULL, 0, 0};
  const kbc_tokens *t = toks ? toks : &empty;
  if (!ix) {
    return kbc_err_set(err, KBC_ERR_INVALID, "kbc_index_update_doc: ix is NULL");
  }
  if (!corpus || !path) {
    return kbc_err_set(err, KBC_ERR_INVALID,
                       "kbc_index_update_doc(doc %u): corpus or path is NULL",
                       doc_id);
  }
  if (!ix->sealed) {
    return kbc_err_set(err, KBC_ERR_INVALID,
                       "kbc_index_update_doc(doc %u): the index is not sealed, "
                       "finish the build first",
                       doc_id);
  }
  if (doc_id != UINT32_MAX) {
    const kbc_doc_meta *d;
    if (doc_id >= ix->doc_count) {
      return kbc_err_set(err, KBC_ERR_INVALID,
                         "kbc_index_update_doc(doc %u): the index holds %u "
                         "documents",
                         doc_id, ix->doc_count);
    }
    d = &ix->docs[doc_id];
    if (strcmp(d->corpus, corpus) != 0 || strcmp(d->path, path) != 0) {
      return kbc_err_set(err, KBC_ERR_INVALID,
                         "kbc_index_update_doc(doc %u): that id is %s/%s, not "
                         "%s/%s",
                         doc_id, d->corpus, d->path, corpus, path);
    }
  }
  if ((uint32_t)kind >= (uint32_t)KBC_KIND__COUNT) {
    return kbc_err_set(err, KBC_ERR_INVALID,
                       "kbc_index_update_doc(doc %u): kind %d is not a kbc_kind",
                       doc_id, (int)kind);
  }
  if (strlen(corpus) > KBC_MAX_CORPORA || strlen(path) > KBC_MAX_PATH_LEN) {
    return kbc_err_set(err, KBC_ERR_INVALID,
                       "kbc_index_update_doc(doc %u): corpus/path too long "
                       "(%zu/%zu bytes)",
                       doc_id, strlen(corpus), strlen(path));
  }
  if (t->len > KBC_MAX_TOKENS_PER_DOC) {
    return kbc_err_set(err, KBC_ERR_INVALID,
                       "kbc_index_update_doc(doc %u): %zu tokens exceeds the %u "
                       "limit",
                       doc_id, t->len, KBC_MAX_TOKENS_PER_DOC);
  }
  if (t->len > 0 && !t->items) {
    return kbc_err_set(err, KBC_ERR_INVALID,
                       "kbc_index_update_doc(doc %u): %zu tokens but no items",
                       doc_id, t->len);
  }
  return KBC_OK;
}

/* Interns one term, or finds it. Returns the slot, and reports through
 * `created` whether this call is what put it in the table — a caller that then
 * fails has to take it back out, exactly as kbc_index_add_doc's commit loop
 * does. */
static kbc_status term_intern(kbc_index *ix, const char *t, size_t n,
                              uint64_t h, uint32_t *off_out, size_t *slot_out,
                              bool *created) {
  size_t ti = term_find_idx(ix, h, t, n);
  if (ti == SIZE_MAX) {
    if (ix->next_term_id == UINT32_MAX) {
      return KBC_ERR_INVALID;
    }
    if (sar_put(&ix->tar, t, n, off_out) != KBC_OK) {
      return KBC_ERR_NOMEM;
    }
    ti = (size_t)(h & (uint64_t)(ix->term_cap - 1));
    while (ix->terms[ti].term_off != SLOT_EMPTY) {
      ti = (ti + 1) & (ix->term_cap - 1);
    }
    ix->terms[ti].hash = h;
    ix->terms[ti].term_off = *off_out;
    ix->terms[ti].term_len = (uint32_t)n;
    ix->terms[ti].post_off = 0;
    ix->terms[ti].post_len = 0;
    ix->terms[ti].id = ix->next_term_id++;
    ix->term_live++;
    *created = true;
  }
  *slot_out = ti;
  return KBC_OK;
}

/* Adds or replaces one document in a LIVE index, without rebuilding anything
 * else. `doc_id` is the document being replaced — kbc_index_id_of(ix, corpus,
 * path) — or UINT32_MAX to append a document that is not in the index yet.
 * Every other document's postings, doc id and token count are left exactly as
 * they were; this document's own postings are replaced wholesale, so terms it
 * no longer has do not survive it.
 *
 * Not const and not thread-safe, like every build function in this file: the
 * caller must hold whatever lock its readers take (kbc_app holds the write
 * lock, so no query is in flight). On failure NOTHING is applied — the
 * postings array, the term table, the doc table, doc_count and the token
 * totals are all as they were — except the string arenas, which may have
 * grown by the bytes a rejected document would have used. */
kbc_status kbc_index_update_doc(kbc_index *ix, uint32_t doc_id,
                                const char *corpus, const char *path,
                                const char *title, kbc_kind kind,
                                const kbc_tokens *toks, kbc_err *err) {
  static const kbc_tokens empty = {NULL, 0, 0};
  const kbc_tokens *t = toks ? toks : &empty;
  kbc_scratch *sc;
  kbc_add *adds = NULL;
  uint32_t *created = NULL;
  kbc_doctab dt;
  size_t n = 0, i, ncreated = 0;
  uint32_t new_id;
  bool is_new = (doc_id == UINT32_MAX);
  kbc_status st;

  memset(&dt, 0, sizeof dt);
  st = update_validate(ix, doc_id, corpus, path, kind, toks, err);
  if (kbc_failed(st)) {
    return st;
  }
  new_id = is_new ? ix->doc_count : doc_id;
  if (materialize_term_arena(ix) != KBC_OK) {
    return kbc_err_set(err, KBC_ERR_NOMEM,
                       "kbc_index_update_doc(%s/%s): term arena alloc failed",
                       corpus, path);
  }
  if (scratch_ensure(ix, t->len) != KBC_OK) {
    return kbc_err_set(err, KBC_ERR_NOMEM,
                       "kbc_index_update_doc(%s/%s): scratch alloc failed",
                       corpus, path);
  }
  sc = ix->sc;
  if (scratch_count(sc, t, err) != KBC_OK) {
    return err ? err->status : KBC_ERR_INVALID;
  }
  n = sc->len;

  /* Everything the commit needs is obtained before any of it is written. */
  if (n > 0) {
    size_t need = ((ix->term_live + ix->term_tombs + n) * 1000) / LOAD_PERMILLE +
                  2;
    if (ix->term_cap == 0 || need > ix->term_cap) {
      if (term_grow(ix, need) != KBC_OK) {
        return kbc_err_set(err, KBC_ERR_NOMEM,
                           "kbc_index_update_doc(%s/%s): term table alloc "
                           "failed",
                           corpus, path);
      }
    }
    adds = (kbc_add *)malloc(n * sizeof(*adds));
    created = (uint32_t *)malloc(n * sizeof(*created));
    if (!adds || !created) {
      free(adds);
      free(created);
      return kbc_err_set(err, KBC_ERR_NOMEM,
                         "kbc_index_update_doc(%s/%s): alloc failed for %zu "
                         "terms",
                         corpus, path, n);
    }
  }
  st = doctab_build(&dt, ix, false, is_new ? UINT32_MAX : new_id, corpus, path,
                    title, kind, (uint32_t)t->len, "kbc_index_update_doc", err);
  if (kbc_failed(st)) {
    free(adds);
    free(created);
    return st;
  }

  /* Intern. Nothing is committed yet, so a term this call creates is simply
   * taken back out if a later step fails. */
  for (i = 0; i < n; i++) {
    uint64_t th = fnv1a64(sc->ts[i], sc->tl[i]);
    size_t slot = 0;
    uint32_t off = 0;
    bool made = false;
    st = term_intern(ix, sc->ts[i], sc->tl[i], th, &off, &slot, &made);
    if (kbc_failed(st)) {
      goto unwind;
    }
    if (made) {
      created[ncreated++] = (uint32_t)slot;
    }
    adds[i].slot = slot;
    adds[i].doc = new_id;
    adds[i].tf = sc->tf[i];
  }

  st = postings_rewrite(ix, !is_new, new_id, false, 0, adds, n,
                        "kbc_index_update_doc", err);
  if (kbc_failed(st)) {
    goto unwind;
  }

  /* Commit. Past this line nothing can fail: the postings are already in, and
   * the table, the arena and the doc hash are three allocations made above. */
  if (!is_new) {
    /* A replace does not move doc ids, so a vecstore row and a store hit keyed
     * on one stay valid. */
    ix->total_tokens -= ix->docs[new_id].token_count;
  }
  free(ix->docs);
  ix->docs = dt.docs;
  ix->doc_cap = dt.count;
  ix->doc_count = dt.count;
  sar_free(&ix->dar);
  ix->dar = dt.dar;
  ix->dar_base = NULL; /* the heap arena is the doc arena from here on */
  free(ix->dhash);
  ix->dhash = dt.dhash;
  ix->dhash_cap = dt.dhash_cap;
  ix->total_tokens += (uint64_t)t->len;
  prune_empty_terms(ix);
  scratch_free(ix); /* the term offsets it holds are stale after a rehash */
  free(adds);
  free(created);
  return KBC_OK;

unwind:
  for (i = 0; i < ncreated; i++) {
    term_remove_at(ix, created[i]);
  }
  doctab_free(&dt);
  free(adds);
  free(created);
  return st;
}

/* Removes one document from a LIVE index: its postings leave every term it
 * contributed to, the doc ids above it move down so they stay dense, and the
 * token total follows. Threading and failure are as for kbc_index_update_doc.
 *
 * A removed document is COMPACTED out rather than left as a hole, because
 * kbc_index_avg_doclen divides by doc_count and a hole would move every score
 * in the corpus. The consequence is visible and deliberate: the documents after
 * the removed one get new doc ids, so a caller holding an id across a removal
 * must look the document up again by (corpus, path) — which is what
 * kbc_index_id_of is for. */
kbc_status kbc_index_remove_doc(kbc_index *ix, uint32_t doc_id, kbc_err *err) {
  kbc_doctab dt;
  kbc_status st;
  if (!ix) {
    return kbc_err_set(err, KBC_ERR_INVALID, "kbc_index_remove_doc: ix is NULL");
  }
  if (!ix->sealed) {
    return kbc_err_set(err, KBC_ERR_INVALID,
                       "kbc_index_remove_doc(doc %u): the index is not sealed",
                       doc_id);
  }
  if (doc_id >= ix->doc_count) {
    return kbc_err_set(err, KBC_ERR_INVALID,
                       "kbc_index_remove_doc(doc %u): the index holds %u "
                       "documents",
                       doc_id, ix->doc_count);
  }
  memset(&dt, 0, sizeof dt);
  /* The table without this document: every later document moves down one, and
   * the hash is rebuilt to match. */
  st = doctab_build(&dt, ix, true, doc_id, NULL, NULL, NULL, KBC_KIND_ARTIFACT,
                    0, "kbc_index_remove_doc", err);
  if (kbc_failed(st)) {
    return st;
  }
  st = postings_rewrite(ix, true, doc_id, true, doc_id + 1, NULL, 0,
                        "kbc_index_remove_doc", err);
  if (kbc_failed(st)) {
    doctab_free(&dt);
    return st;
  }
  /* Commit: infallible from here. */
  ix->total_tokens -= ix->docs[doc_id].token_count;
  free(ix->docs);
  ix->docs = dt.docs;
  ix->doc_cap = dt.count;
  ix->doc_count = dt.count;
  sar_free(&ix->dar);
  ix->dar = dt.dar;
  ix->dar_base = NULL; /* the heap arena is the doc arena from here on */
  free(ix->dhash);
  ix->dhash = dt.dhash;
  ix->dhash_cap = dt.dhash_cap;
  prune_empty_terms(ix);
  return KBC_OK;
}
/* ----------------------------------------------------------- inspecting -- */

uint32_t kbc_index_doc_count(const kbc_index *ix) {
  return ix ? ix->doc_count : 0;
}

uint32_t kbc_index_term_count(const kbc_index *ix) {
  return ix ? (uint32_t)ix->term_live : 0;
}

uint64_t kbc_index_posting_count(const kbc_index *ix) {
  return ix ? (uint64_t)ix->post_len : 0;
}

double kbc_index_avg_doclen(const kbc_index *ix) {
  if (!ix || ix->doc_count == 0) {
    return 0.0;
  }
  return (double)ix->total_tokens / (double)ix->doc_count;
}

const kbc_doc_meta *kbc_index_doc(const kbc_index *ix, uint32_t doc_id) {
  if (!ix || !ix->docs || doc_id >= ix->doc_count) {
    return NULL;
  }
  return &ix->docs[doc_id];
}

uint32_t kbc_index_id_of(const kbc_index *ix, const char *corpus,
                         const char *path) {
  if (!ix || !corpus || !path || !ix->docs) {
    return UINT32_MAX;
  }
  return dhash_lookup(ix, hash_corpus_path(corpus, path), corpus, path);
}

size_t kbc_index_heap_bytes(const kbc_index *ix) {
  size_t n = 0, i;
  if (!ix) {
    return 0;
  }
  n += sizeof(kbc_index);
  n += ix->term_cap * sizeof(kbc_term_slot);
  n += ix->dhash_cap * sizeof(kbc_doc_slot);
  n += (size_t)ix->doc_cap * sizeof(kbc_doc_meta);
  n += ix->post_cap * sizeof(kbc_posting);
  n += (ix->tar.cap_blocks + ix->dar.cap_blocks) * sizeof(kbc_arena_block);
  for (i = 0; i < ix->tar.nblocks; i++) {
    n += ix->tar.blocks[i].size;
  }
  for (i = 0; i < ix->dar.nblocks; i++) {
    n += ix->dar.blocks[i].size;
  }
  if (ix->sc) {
    n += sizeof(kbc_scratch);
    n += ix->sc->mcap * (sizeof(uint32_t) + sizeof(uint64_t));
    n += ix->sc->cap * (sizeof(char *) + 4 * sizeof(uint32_t));
  }
  /* Query scratch is per-thread, not reachable from the index, so it is not
   * counted here (see acc_get). */
  return n; /* the mmap is address space, not heap */
}

/* ---------------------------------------------------------- persistence --
 * One file, little-endian fixed-width, every offset and length written
 * explicitly: a struct dump would bake this compiler's padding into a file that
 * must outlive it. Section order after the 64-byte header:
 *   term arena | pad | doc arena | pad | doc table | term table | doc hash |
 *   postings
 * Every section start is 8-aligned, so the mapped structs stay aligned.
 */

kbc_status kbc_index_save(const kbc_index *ix, const char *path, kbc_err *err) {
  kbc_str out;
  uint8_t hdr[HDR_SIZE];
  size_t i, need, pad;
  kbc_status st = KBC_OK;

  if (!ix || !path) {
    return kbc_err_set(err, KBC_ERR_INVALID, "kbc_index_save: ix or path is NULL");
  }
  if (ix->term_cap > UINT32_MAX || ix->dhash_cap > UINT32_MAX ||
      ix->post_len > UINT32_MAX || ix->term_live > UINT32_MAX) {
    return kbc_err_set(err, KBC_ERR_INVALID,
                       "kbc_index_save %s: the index is too large for the "
                       "format (cap %zu, %zu postings)",
                       path, ix->term_cap, ix->post_len);
  }

  memset(hdr, 0, sizeof(hdr));
  memcpy(hdr, KBC_INDEX_MAGIC, 7);
  put_u32(hdr + 8, KBC_INDEX_FORMAT);
  put_u32(hdr + 12, 0);
  put_u32(hdr + 16, ix->doc_count);
  put_u32(hdr + 20, (uint32_t)ix->term_live);
  put_u32(hdr + 24, (uint32_t)ix->post_len);
  put_u32(hdr + 28, ix->doc_cap);
  put_u32(hdr + 32, (uint32_t)ix->term_cap);
  put_u32(hdr + 36, (uint32_t)ix->dhash_cap);
  put_u64(hdr + 40, ix->total_tokens);
  put_u64(hdr + 48, (uint64_t)ix->tar.total);
  put_u64(hdr + 56, (uint64_t)ix->dar.total);

  need = sizeof(hdr) + ix->tar.total + pad8(sizeof(hdr) + ix->tar.total) +
         ix->dar.total + pad8(sizeof(hdr) + ix->tar.total + pad8(ix->tar.total) +
                             ix->dar.total) +
         (size_t)ix->doc_count * DREC_SIZE + ix->term_cap * TSLOT_SIZE +
         ix->dhash_cap * DSLOT_SIZE + ix->post_len * POST_SIZE;
  kbc_str_init(&out);
  if (buf_reserve(&out, need) != KBC_OK) {
    return kbc_err_set(err, KBC_ERR_NOMEM, "kbc_index_save %s: cannot buffer "
                                           "%zu bytes",
                       path, need);
  }
  st = kbc_str_append(&out, (const char *)hdr, sizeof(hdr));
  /* The section bytes come from wherever the arena LIVES, not from the heap
   * copy that only a mutation fills in. An index opened from a file points
   * its term and doc strings into the mapping and has no heap arena at all
   * until the first mutation, so writing `ix->tar` for it produced a file
   * that declared an arena it had not written: shorter than its own header
   * by exactly the arena, which is what kbc_index_open's bounds check
   * (rightly) rejects. Writing the mapping directly is also the cheap answer:
   * the bytes are already in the page cache. */
  if (ix->tar_base != NULL) {
    st = kbc_str_append(&out, ix->tar_base, ix->tar.total);
  }
  for (i = 0; i < ix->tar.nblocks && st == KBC_OK; i++) {
    if (ix->tar.blocks[i].used) {
      st = kbc_str_append(&out, ix->tar.blocks[i].p, ix->tar.blocks[i].used);
    }
  }
  /* The pad length is computed ONCE. Re-evaluating pad8(out.len) inside the
   * loop condition made the bound drift as out.len grew, so the loop stopped
   * short (3 bytes instead of 5 after the term arena) and every section after
   * it landed at the wrong offset — a file this same loader rejected. */
  pad = pad8(out.len);
  for (i = pad; i > 0 && st == KBC_OK; i--) {
    st = kbc_str_putc(&out, '\0');
  }
  if (ix->dar_base != NULL) {
    st = kbc_str_append(&out, ix->dar_base, ix->dar.total);
  }
  for (i = 0; i < ix->dar.nblocks && st == KBC_OK; i++) {
    if (ix->dar.blocks[i].used) {
      st = kbc_str_append(&out, ix->dar.blocks[i].p, ix->dar.blocks[i].used);
    }
  }
  pad = pad8(out.len);
  for (i = pad; i > 0 && st == KBC_OK; i--) {
    st = kbc_str_putc(&out, '\0');
  }
  /* Doc table: lengths only. The writer emits corpus, path, title in that
   * order per document, so the loader walks the arena with the same lengths. */
  for (i = 0; i < ix->doc_count && st == KBC_OK; i++) {
    const kbc_doc_meta *d = &ix->docs[i];
    uint8_t rec[DREC_SIZE];
    memset(rec, 0, sizeof(rec));
    put_u32(rec + 0, (uint32_t)strlen(d->corpus));
    put_u32(rec + 4, (uint32_t)strlen(d->path));
    put_u32(rec + 8, (uint32_t)strlen(d->title ? d->title : ""));
    put_u32(rec + 12, d->token_count);
    rec[16] = d->kind;
    st = kbc_str_append(&out, (const char *)rec, DREC_SIZE);
  }
  for (i = 0; i < ix->term_cap && st == KBC_OK; i++) {
    uint8_t rec[TSLOT_SIZE];
    put_u64(rec + 0, ix->terms[i].hash);
    put_u32(rec + 8, ix->terms[i].term_off);
    put_u32(rec + 12, ix->terms[i].term_len);
    put_u32(rec + 16, ix->terms[i].post_off);
    put_u32(rec + 20, ix->terms[i].post_len);
    st = kbc_str_append(&out, (const char *)rec, TSLOT_SIZE);
  }
  for (i = 0; i < ix->dhash_cap && st == KBC_OK; i++) {
    uint8_t rec[DSLOT_SIZE];
    put_u64(rec + 0, ix->dhash[i].hash);
    put_u32(rec + 8, ix->dhash[i].doc);
    st = kbc_str_append(&out, (const char *)rec, DSLOT_SIZE);
  }
  for (i = 0; i < ix->post_len && st == KBC_OK; i++) {
    uint8_t rec[POST_SIZE];
    put_u32(rec + 0, ix->post[i].doc);
    put_u32(rec + 4, ix->post[i].tf);
    st = kbc_str_append(&out, (const char *)rec, POST_SIZE);
  }
  if (st != KBC_OK) {
    size_t got = out.len;
    kbc_str_free(&out);
    return kbc_err_set(err, KBC_ERR_NOMEM, "kbc_index_save %s: could not buffer "
                                           "the %zu-byte file (%zu written)",
                       path, need, got);
  }
  /* The header's own arithmetic, checked against the file that was actually
   * produced. kbc_index_open walks exactly `need` bytes and rejects anything
   * shorter, so a mismatch here is a file the daemon's own loader would
   * refuse — the failure this save is documented never to produce. Refusing
   * to write it is the whole point: a loud INTERNAL here keeps the previous
   * generation on disk, a short file does not. */
  if (out.len != need) {
    size_t got = out.len;
    kbc_str_free(&out);
    return kbc_err_set(err, KBC_ERR_INTERNAL,
                       "kbc_index_save %s: the header describes %zu bytes but "
                       "%zu were written (doc_count %u, term_cap %zu, "
                       "dhash_cap %zu, postings %zu, term arena %zu, doc arena "
                       "%zu)",
                       path, need, got, ix->doc_count, ix->term_cap,
                       ix->dhash_cap, ix->post_len, ix->tar.total,
                       ix->dar.total);
  }
  st = kbc_str_write_file_atomic(path, out.ptr, out.len, err);
  kbc_str_free(&out);
  return st;
}

/* Bounds-checks one section against the mapped size. */
static bool need_room(size_t *cur, uint64_t len, size_t file_size) {
  if (len > (uint64_t)SIZE_MAX) {
    return false;
  }
  if (*cur > file_size || (size_t)len > file_size - *cur) {
    return false;
  }
  *cur += (size_t)len;
  return true;
}

kbc_index *kbc_index_open(const char *path, kbc_err *err) {
  int fd;
  struct stat sb;
  void *map = MAP_FAILED;
  kbc_index *ix = NULL;
  const uint8_t *p;
  size_t size = 0, cur, docs_at;
  uint32_t doc_count, term_count, posting_count, doc_cap, term_cap, dhash_cap;
  uint64_t total_tokens, term_arena_len, doc_arena_len;
  uint32_t i;

  if (!path) {
    (void)kbc_err_set(err, KBC_ERR_INVALID, "kbc_index_open: path is NULL");
    return NULL;
  }
#if defined(__BYTE_ORDER__) && defined(__ORDER_LITTLE_ENDIAN__)
  if (__BYTE_ORDER__ != __ORDER_LITTLE_ENDIAN__) {
    (void)kbc_err_set(err, KBC_ERR_UNSUPPORTED,
                      "index %s: the zero-copy mmap load needs a little-endian "
                      "host",
                      path);
    return NULL;
  }
#endif
  fd = open(path, O_RDONLY | O_CLOEXEC);
  if (fd < 0) {
    (void)kbc_err_set(err, KBC_ERR_IO, "open %s: %s", path, strerror(errno));
    return NULL;
  }
  if (fstat(fd, &sb) != 0) {
    (void)kbc_err_set(err, KBC_ERR_IO, "stat %s: %s", path, strerror(errno));
    (void)close(fd);
    return NULL;
  }
  if (!S_ISREG(sb.st_mode) || sb.st_size < (off_t)HDR_SIZE) {
    (void)kbc_err_set(err, KBC_ERR_PARSE,
                      "index %s: %lld bytes is shorter than the %u-byte header",
                      path, (long long)sb.st_size, HDR_SIZE);
    (void)close(fd);
    return NULL;
  }
  if ((uint64_t)sb.st_size > (uint64_t)SIZE_MAX) {
    (void)kbc_err_set(err, KBC_ERR_PARSE, "index %s: %lld bytes does not fit "
                                           "in this address space",
                      path, (long long)sb.st_size);
    (void)close(fd);
    return NULL;
  }
  size = (size_t)sb.st_size;
  map = mmap(NULL, size, PROT_READ, MAP_PRIVATE, fd, 0);
  (void)close(fd);
  if (map == MAP_FAILED) {
    (void)kbc_err_set(err, KBC_ERR_IO, "mmap %s: %s", path, strerror(errno));
    return NULL;
  }
  p = (const uint8_t *)map;

  if (memcmp(p, KBC_INDEX_MAGIC, 7) != 0) {
    (void)kbc_err_set(err, KBC_ERR_PARSE, "index %s: bad magic %02x%02x%02x%02x",
                      path, (unsigned)p[0], (unsigned)p[1], (unsigned)p[2],
                      (unsigned)p[3]);
    goto fail;
  }
  {
    uint32_t ver = get_u32(p + 8);
    if (ver != KBC_INDEX_FORMAT) {
      (void)kbc_err_set(err, KBC_ERR_PARSE,
                        "index %s: format version %u, this build reads %u", path,
                        ver, KBC_INDEX_FORMAT);
      goto fail;
    }
  }
  doc_count = get_u32(p + 16);
  term_count = get_u32(p + 20);
  posting_count = get_u32(p + 24);
  doc_cap = get_u32(p + 28);
  term_cap = get_u32(p + 32);
  dhash_cap = get_u32(p + 36);
  total_tokens = get_u64(p + 40);
  term_arena_len = get_u64(p + 48);
  doc_arena_len = get_u64(p + 56);

  /* Every section is bounds-checked against the mapped size before any of them
   * is dereferenced: a truncated or corrupt file is rejected, never read. */
  cur = HDR_SIZE;
  if (!need_room(&cur, term_arena_len, size) ||
      !need_room(&cur, (uint64_t)pad8(cur), size) ||
      !need_room(&cur, doc_arena_len, size) ||
      !need_room(&cur, (uint64_t)pad8(cur), size) ||
      !need_room(&cur, (uint64_t)doc_count * DREC_SIZE, size) ||
      !need_room(&cur, (uint64_t)term_cap * TSLOT_SIZE, size) ||
      !need_room(&cur, (uint64_t)dhash_cap * DSLOT_SIZE, size) ||
      !need_room(&cur, (uint64_t)posting_count * POST_SIZE, size)) {
    (void)kbc_err_set(err, KBC_ERR_PARSE,
                      "index %s: corrupt, its sections need more than the %zu "
                      "bytes mapped",
                      path, size);
    goto fail;
  }

  ix = (kbc_index *)calloc(1, sizeof(kbc_index));
  if (!ix) {
    (void)kbc_err_set(err, KBC_ERR_NOMEM, "index %s: out of memory", path);
    goto fail;
  }
  ix->map = map;
  ix->map_len = size;
  ix->sealed = true;
  ix->doc_count = doc_count;
  ix->doc_cap = doc_cap > doc_count ? doc_cap : doc_count; /* never under-read */
  ix->term_live = term_count;
  ix->post_len = posting_count;
  ix->total_tokens = total_tokens;
  ix->term_cap = term_cap;
  ix->dhash_cap = dhash_cap;
  ix->tar.total = (size_t)term_arena_len;
  ix->dar.total = (size_t)doc_arena_len;
  ix->tar_base = (const char *)p + HDR_SIZE; /* terms are the first section */


  docs_at = HDR_SIZE + (size_t)term_arena_len;
  docs_at += pad8(docs_at);
  cur = docs_at + (size_t)doc_arena_len;
  cur += pad8(cur);
  ix->dar_base = (const char *)p + docs_at;

  if (doc_count > 0) {
    size_t at = docs_at;
    ix->docs = (kbc_doc_meta *)malloc((size_t)doc_count * sizeof(kbc_doc_meta));
    if (!ix->docs) {
      (void)kbc_err_set(err, KBC_ERR_NOMEM, "index %s: doc table out of memory",
                        path);
      kbc_index_free(ix);
      return NULL;
    }
    for (i = 0; i < doc_count; i++) {
      const uint8_t *rec = p + cur + (size_t)i * DREC_SIZE;
      size_t base = at; /* this doc's first string; `at` runs forward only */
      uint32_t l[3];
      int k;
      l[0] = get_u32(rec + 0);
      l[1] = get_u32(rec + 4);
      l[2] = get_u32(rec + 8);
      /* The writer emits corpus, path, title per document in that order, each
       * NUL terminated, so one checked walk rebuilds every pointer and no
       * string read can leave the arena. `at` is a running cursor: rewinding
       * it to each doc's base after the walk (as this once did) made every
       * document re-read document 0's strings. */
      for (k = 0; k < 3; k++) {
        if ((uint64_t)(at - docs_at) + l[k] + 1 > doc_arena_len ||
            p[at + l[k]] != 0) {
          (void)kbc_err_set(err, KBC_ERR_PARSE,
                            "index %s: doc %u string %d (%u bytes) is not a NUL "
                            "terminated string inside the %llu-byte string arena",
                            path, i, k, l[k], (unsigned long long)doc_arena_len);
          kbc_index_free(ix);
          return NULL;
        }
        at += l[k] + 1;
      }
      ix->docs[i].corpus = (const char *)p + base;
      ix->docs[i].path = (const char *)p + base + l[0] + 1;
      ix->docs[i].title = (const char *)p + base + l[0] + 1 + l[1] + 1;
      ix->docs[i].kind = rec[16];
      ix->docs[i].token_count = get_u32(rec + 12);
    }
  }
  cur += (size_t)doc_count * DREC_SIZE;
  /* The term table and the postings are decoded into host-order heap copies
   * rather than pointed at inside the mapping: the file is little-endian by
   * contract, and a const-correct zero-copy view would mean const-qualifying
   * the build path's term and posting arrays too. The cost is one linear pass
   * at open; every query after it reads RAM. */

  if (term_cap > 0) {
    ix->terms = (kbc_term_slot *)malloc((size_t)term_cap * sizeof(kbc_term_slot));
    if (!ix->terms) {
      (void)kbc_err_set(err, KBC_ERR_NOMEM, "index %s: term table out of memory",
                        path);
      kbc_index_free(ix);
      return NULL;
    }
    for (i = 0; i < term_cap; i++) {
      const uint8_t *rec = p + cur + (size_t)i * TSLOT_SIZE;
      ix->terms[i].hash = get_u64(rec + 0);
      ix->terms[i].term_off = get_u32(rec + 8);
      ix->terms[i].term_len = get_u32(rec + 12);
      ix->terms[i].post_off = get_u32(rec + 16);
      ix->terms[i].post_len = get_u32(rec + 20);
      ix->terms[i].id = UINT32_MAX; /* a loaded index is never rebuilt in place */
      if (ix->terms[i].term_off == SLOT_EMPTY) {
        continue;
      }
      if (ix->terms[i].term_off == SLOT_TOMB) {
        continue; /* a tombstone is legal on disk; it is simply not a term */
      }
      if ((uint64_t)ix->terms[i].term_off + ix->terms[i].term_len + 1 >
              term_arena_len ||
          (uint64_t)ix->terms[i].post_off + ix->terms[i].post_len >
              (uint64_t)posting_count) {
        (void)kbc_err_set(err, KBC_ERR_PARSE,
                          "index %s: term slot %u points outside its section",
                          path, i);
        kbc_index_free(ix);
        return NULL;
      }
    }
  }
  cur += (size_t)term_cap * TSLOT_SIZE;

  if (dhash_cap > 0) {
    ix->dhash = (kbc_doc_slot *)malloc((size_t)dhash_cap * sizeof(kbc_doc_slot));
    if (!ix->dhash) {
      (void)kbc_err_set(err, KBC_ERR_NOMEM, "index %s: doc hash out of memory",
                        path);
      kbc_index_free(ix);
      return NULL;
    }
    for (i = 0; i < dhash_cap; i++) {
      const uint8_t *rec = p + cur + (size_t)i * DSLOT_SIZE;
      ix->dhash[i].hash = get_u64(rec + 0);
      ix->dhash[i].doc = get_u32(rec + 8);
      if (ix->dhash[i].doc != SLOT_EMPTY && ix->dhash[i].doc >= doc_count) {
        (void)kbc_err_set(err, KBC_ERR_PARSE,
                          "index %s: doc hash slot %u names doc %u of %u", path, i,
                          ix->dhash[i].doc, doc_count);
        kbc_index_free(ix);
        return NULL;
      }
    }
  }
  cur += (size_t)dhash_cap * DSLOT_SIZE;

  ix->post_cap = posting_count;
  if (posting_count > 0) {
    ix->post = (kbc_posting *)malloc((size_t)posting_count * sizeof(kbc_posting));
    if (!ix->post) {
      (void)kbc_err_set(err, KBC_ERR_NOMEM, "index %s: postings out of memory",
                        path);
      kbc_index_free(ix);
      return NULL;
    }
    for (i = 0; i < posting_count; i++) {
      const uint8_t *rec = p + cur + (size_t)i * POST_SIZE;
      ix->post[i].doc = get_u32(rec + 0);
      ix->post[i].tf = get_u32(rec + 4);
      if (ix->post[i].doc >= doc_count || ix->post[i].tf == 0) {
        (void)kbc_err_set(err, KBC_ERR_PARSE,
                          "index %s: posting %u names doc %u tf %u of %u docs",
                          path, i, ix->post[i].doc, ix->post[i].tf, doc_count);
        kbc_index_free(ix);
        return NULL;
      }
    }
  }
  return ix;

fail:
  if (ix) {
    kbc_index_free(ix);
  } else if (map != MAP_FAILED) {
    (void)munmap(map, size);
  }
  return NULL;
}

/* ------------------------------------------------------------- querying -- */

static bool hit_better(const kbc_hit *a, const kbc_hit *b) {
  if (a->score != b->score) {
    return a->score > b->score;
  }
  return a->doc < b->doc; /* ties: ascending doc_id, so results are stable */
}

/* Min-heap: the root is the *worst* hit kept, which is the one to evict. */
static void heap_push(kbc_hit *h, size_t *n, const kbc_hit *v) {
  size_t i = (*n)++;
  h[i] = *v;
  while (i > 0) {
    size_t parent = (i - 1) / 2;
    kbc_hit t;
    if (!hit_better(&h[i], &h[parent])) {
      break;
    }
    t = h[parent];
    h[parent] = h[i];
    h[i] = t;
    i = parent;
  }
}

static kbc_hit heap_pop(kbc_hit *h, size_t *n) {
  kbc_hit top = h[0];
  size_t i = 0;
  h[0] = h[--(*n)];
  for (;;) {
    size_t l = 2 * i + 1, r = l + 1, m = i;
    kbc_hit t;
    if (l < *n && hit_better(&h[l], &h[m])) {
      m = l;
    }
    if (r < *n && hit_better(&h[r], &h[m])) {
      m = r;
    }
    if (m == i) {
      break;
    }
    t = h[m];
    h[m] = h[i];
    h[i] = t;
    i = m;
  }
  return top;
}

/* The scoring body, given its scratch. `a` is private to one call on one
 * thread; nothing here writes to `ix`. */
static kbc_status bm25_run(const kbc_index *ix, kbc_acc *a,
                           const kbc_tokens *query, double k1, double b,
                           size_t limit, kbc_hits *out, kbc_err *err) {
  size_t ndocs, nq, qlen = 0, touched = 0, hn = 0, i;
  double avgdl, N;
  uint32_t gen;

  if (!(k1 >= 0.0) || !(b >= 0.0) || !(b <= 1.0)) {
    return kbc_err_set(err, KBC_ERR_INVALID, "kbc_index_bm25: k1=%g b=%g are out "
                                             "of range",
                       k1, b);
  }
  if (limit > KBC_MAX_HITS) {
    limit = KBC_MAX_HITS;
  }
  if (limit == 0) {
    return KBC_OK;
  }
  if (acc_fit(a, ix->doc_count) != KBC_OK) {
    return kbc_err_set(err, KBC_ERR_NOMEM,
                       "kbc_index_bm25: accumulator for %u docs", ix->doc_count);
  }
  if (a->heap_cap < limit) {
    kbc_hit *h = (kbc_hit *)realloc(a->heap, limit * sizeof(kbc_hit));
    if (!h) {
      return kbc_err_set(err, KBC_ERR_NOMEM, "kbc_index_bm25: heap of %zu hits",
                         limit);
    }
    a->heap = h;
    a->heap_cap = limit;
  }
  ndocs = ix->doc_count;
  nq = query ? query->len : 0;
  if (ndocs == 0 || nq == 0) {
    return KBC_OK;
  }
  if (nq > KBC_MAX_TOKENS_PER_DOC) {
    return kbc_err_set(err, KBC_ERR_INVALID,
                       "kbc_index_bm25: query of %zu terms exceeds the %u limit",
                       nq, KBC_MAX_TOKENS_PER_DOC);
  }
  if (a->q_cap < nq) {
    kbc_qterm *nq_mem = (kbc_qterm *)realloc(a->q, nq * sizeof(kbc_qterm));
    if (!nq_mem) {
      return kbc_err_set(err, KBC_ERR_NOMEM,
                         "kbc_index_bm25: query dedup scratch for %zu terms", nq);
    }
    a->q = nq_mem;
    a->q_cap = nq;
  }

  gen = a->gen + 1;
  if (gen == 0) { /* wrapped: only after 4 billion queries on one index */
    memset(a->stamps, 0, (a->doc_cap ? a->doc_cap : 1) * sizeof(uint32_t));
    gen = 1;
  }
  a->gen = gen;
  avgdl = kbc_index_avg_doclen(ix);
  N = (double)ndocs;
  if (!(avgdl > 0.0)) {
    return KBC_OK; /* nothing was ever tokenized, so nothing can match */
  }

  for (i = 0; i < nq; i++) {
    const kbc_token *t = &query->items[i];
    uint64_t h;
    const kbc_term_slot *sl;
    double idf;
    size_t k, df;
    bool dup = false;
    if (t->len == 0 || t->len > KBC_MAX_TERM_LEN) {
      continue;
    }
    h = fnv1a64(t->text, t->len);
    for (k = 0; k < qlen; k++) { /* a repeated query term is charged once */
      if (a->q[k].h == h && a->q[k].len == t->len &&
          memcmp(a->q[k].s, t->text, t->len) == 0) {
        dup = true;
        break;
      }
    }
    if (dup) {
      continue;
    }
    a->q[qlen].h = h;
    a->q[qlen].s = t->text;
    a->q[qlen].len = (uint32_t)t->len;
    qlen++;

    sl = term_find(ix, h, t->text, t->len);
    if (!sl || sl->post_len == 0) {
      continue;
    }
    df = (size_t)sl->post_len;
    idf = log(1.0 + (N - (double)df + 0.5) / ((double)df + 0.5));
    for (k = 0; k < df; k++) {
      uint32_t d = ix->post[sl->post_off + k].doc;
      double tf, norm, w;
      if (d >= ndocs) {
        continue;
      }
      tf = (double)ix->post[sl->post_off + k].tf;
      norm = tf + k1 * (1.0 - b + b * ((double)ix->docs[d].token_count / avgdl));
      if (!(norm > 0.0)) {
        continue;
      }
      w = idf * (tf * (k1 + 1.0)) / norm;
      if (a->stamps[d] != gen) {
        a->stamps[d] = gen;
        a->scores[d] = w;
        a->touched[touched++] = d;
      } else {
        a->scores[d] += w;
      }
    }
  }

  /* Partial selection: keep the best `limit`, never sort every matching doc. */
  for (i = 0; i < touched; i++) {
    uint32_t d = a->touched[i];
    kbc_hit hit;
    hit.doc = d;
    hit.score = a->scores[d];
    hit.rank = 0;
    hit.vector_score = 0.0;
    if (hn < limit) {
      heap_push(a->heap, &hn, &hit);
    } else if (hit_better(&hit, &a->heap[0])) {
      (void)heap_pop(a->heap, &hn);
      heap_push(a->heap, &hn, &hit);
    }
  }
  while (hn > 0) {
    kbc_hit hit = heap_pop(a->heap, &hn);
    kbc_status st = kbc_hits_push(out, hit.doc, hit.score);
    if (st != KBC_OK) {
      return kbc_err_set(err, st, "kbc_index_bm25: cannot hold %zu hits",
                         out->len);
    }
  }
  kbc_hits_sort_desc(out);
  for (i = 0; i < out->len; i++) {
    out->items[i].rank = (uint32_t)i;
    out->items[i].vector_score = 0.0;
  }
  return KBC_OK;
}

/* Okapi BM25 over the postings. Thread-safe: any number of threads may call
 * this concurrently on one index with no external locking. The index is only
 * read; all per-query state lives in the calling thread's own scratch (see
 * acc_get), so concurrent queries cannot observe each other's accumulators,
 * heaps or dedup sets. kbc_index_free must not race with a call in flight —
 * that is the caller's to arrange, as it is for any borrowed reference. */
kbc_status kbc_index_bm25(const kbc_index *ix, const kbc_tokens *query,
                          double k1, double b, size_t limit, kbc_hits *out,
                          kbc_err *err) {
  kbc_acc *a;
  bool ephemeral;
  kbc_status st;
  if (!ix || !out) {
    return kbc_err_set(err, KBC_ERR_INVALID, "kbc_index_bm25: ix or out is NULL");
  }
  a = acc_get(&ephemeral);
  if (!a) {
    return kbc_err_set(err, KBC_ERR_NOMEM,
                       "kbc_index_bm25: no query scratch for this thread");
  }
  st = bm25_run(ix, a, query, k1, b, limit, out, err);
  if (ephemeral) {
    acc_free(a);
  }
  return st;
}

/* Reads only the immutable term table and the caller's arena/list: no scratch
 * is cached on the index, so this is safe to call concurrently on one index
 * exactly as bm25 is. */
kbc_status kbc_index_expand_prefix(const kbc_index *ix, kbc_arena *a,
                                   const char *prefix, kbc_strlist *out,
                                   kbc_err *err) {
  size_t plen, i, found = 0;
  const char *pre;
  if (!ix || !out) {
    return kbc_err_set(err, KBC_ERR_INVALID,
                       "kbc_index_expand_prefix: ix or out is NULL");
  }
  if (!a) {
    return kbc_err_set(err, KBC_ERR_INVALID,
                       "kbc_index_expand_prefix: arena is NULL");
  }
  pre = prefix ? prefix : "";
  plen = strlen(pre);
  if (plen > KBC_MAX_TERM_LEN) {
    return KBC_OK; /* no term is that long, so nothing can match */
  }
  for (i = 0; i < ix->term_cap; i++) {
    const kbc_term_slot *s = &ix->terms[i];
    const char *t;
    if (s->term_off == SLOT_EMPTY || s->term_off == SLOT_TOMB ||
        s->term_len < plen) {
      continue;
    }
    t = term_ptr(ix, s->term_off);
    if (!t || memcmp(t, pre, plen) != 0) {
      continue;
    }
    /* kbc_strlist_free free()s what it owns, and the caller's arena is not the
     * heap: an owned arena pointer would abort in free(). Push a heap copy. */
    if (kbc_strlist_push(out, t) != KBC_OK) {
      return kbc_err_set(err, KBC_ERR_NOMEM,
                         "kbc_index_expand_prefix(\"%s\"): out of memory", pre);
    }
    if (++found >= KBC_MAX_HITS) {
      break; /* bounded: a trailing '*' must not pull 500k terms into a request */
    }
  }
  kbc_strlist_sort(out);
  return KBC_OK;
}
