/* app.c — the composition root.
 *
 * app.h states the invariant this file exists to protect: the index and the
 * store are replaced together, under one write lock, so no reader can observe
 * an index that references documents the store has not committed. Every
 * public entry point here is safe to call from the httpd's worker threads.
 *
 * Locking shape (explicit, never re-entered):
 *
 *   pthread_mutex_t reindex_mu is the OUTER lock of the two. It is taken at
 *   the outermost reindex entry point only — kbc_app_reindex via
 *   reindex_locked, and kbc_app_reindex_file/_remove via reindex_one — and
 *   held across everything that writes, store rows and index alike. It is not
 *   recursive: reindex_pass, reindex_one_locked and index_touch_one all
 *   ASSUME it is held and must never take it.
 *
 *   pthread_rwlock_t lock guards EXACTLY {index, vec}, and is taken under
 *   reindex_mu and only there. The search resolve callback runs with the read
 *   lock already held, so it must never take it again — a rwlock is not
 *   recursive and that would be a hard self-deadlock.
 *
 * Everything else in kbc_app is either immutable after open (config, store,
 * embedder) or carries its own internal lock (store, embedder, watcher, and
 * the event-bus mutex).
 *
 * The reindex is deliberately two passes over the same manifest. Pass one
 * (walk_dir) decides *which* files are in the index and which of those changed,
 * storing the changed ones; pass two (build_index) reads each file once more
 * to tokenize and embed it. Holding every document's tokens until the end
 * would pin the whole corpus in one arena; a per-file arena in pass two
 * pins one file at a time, which is the memory that actually matters.
 */

/* Feature test macros before any system header. The build compiles with
 * -std=c17 (extensions off), which defines __STRICT_ANSI__ and would
 * otherwise hide opendir/fstatat/rename. */
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE 1
#endif

#include <stdio.h>

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <strings.h>
#include <time.h>

#include "kbc/app.h"
#include "kbc/chunk.h"
#include "kbc/json.h"
#include "kbc/links.h"
#include "kbc/log.h"
#include "kbc/mem.h"
#include "kbc/parse.h"
#include "kbc/store.h"
#include "kbc/types.h"


/* The event bus is a fixed table, not a queue: the SSE writer has its own. */

/* search.h gives the searcher no way to learn the document embeddings, so
 * search.c exports this setter and records the header gap there. Declared
 * here because search.h is frozen; when the header grows it, this
 * declaration goes away with it. */
kbc_status kbc_searcher_set_vecstore(kbc_searcher *s, const kbc_vecstore *vs,
                                     kbc_err *err);
#define KBC_APP_MAX_SUBS 64u
/* Recursion cap for the corpus walk; symlinked directories make this a
 * correctness concern, not merely a stack one. */
#define KBC_APP_MAX_DEPTH 64u
/* Summary length, in bytes, before truncation. */
#define KBC_APP_SUMMARY_MAX 512u
/* The eight owned char* in kbc_config, counted for the deep copy. */
#define KBC_APP_CFG_FIELDS 8u
#define KBC_APP_VEC_FILE "vectors.bin"
/* search.c's `since:` value grammar, which validates the atom there and is
 * applied here; see the note there for the header line the
 * orchestrator should add. */
kbc_status kbc_since_value_ns(const char *value, int64_t *ns, kbc_err *err);
/* The query cache's ceiling, and only that: hits, misses, invalidations and
 * the live entry count are fields on the frozen kbc_app_stats, filled in
 * kbc_app_stats_get. A capacity is not a counter — it is what tells an
 * operator whether the eviction ceiling is doing any work at all. */
size_t kbc_app_query_cache_capacity(const kbc_app *app);


/* The eviction ceiling: the original's DEFAULT_CAPACITY (embed_cache.rs:52).
 * It is also the capacity the linear-scan measurement in embed.h was taken
 * at, so raising it would make that number untrue. The daemon exposes no
 * config key for it — kbc_config is frozen, and this file is not config.c —
 * so 1024 is a constant rather than a setting. */
#define APP_QUERY_CACHE_CAPACITY 1024u
/* The link graph's batch, and the one place that frees it. The element type
 * is app.h's kbc_enrich_source — a hook reads it, so it is the header's
 * shape, and a private twin of it here is the two-definitions drift this
 * project keeps paying for. Defined next to store_forget_path; declared here
 * because reindex_pass is the caller that owns the whole document set. */
static void app_edge_srcs_free(kbc_enrich_source *v, size_t n);
/* "A document went away": the ONE removal. Declared here because the reconcile
 * sweep is defined above its definition and must reach the same implementation
 * the watcher does — a second copy of this cascade is how the two drift. */
static kbc_status store_forget_path(kbc_app *app, const char *corpus,
                                    const char *rel_path, kbc_err *err);


/* Links recorded per document. A document with more outbound links than this
 * is a generated page, not curation; the cap keeps one file from turning a
 * reindex into a graph write the size of the corpus. */
#define APP_MAX_EDGES_PER_DOC 4096u

typedef struct {
  kbc_event_fn fn;
  void *user;
  uint64_t id;
} kbc_app_sub;

/* One file the index will contain, decided by the walk and consumed by the
 * build pass. Heap: it outlives the per-file arena that parsed it. */
typedef struct {
  size_t corpus_index;
  char *path;  /* KBC_OWN, corpus-relative, '/'-separated */
  char *title; /* KBC_OWN, never NULL once the row is in the manifest */
  char id[KBC_MAX_ID_LEN + 1];
  int64_t mtime_ns;
  int64_t size_bytes;
  bool changed; /* false => the stored row is still current */
} kbc_app_row;

typedef struct {
  kbc_app_row *items;
  size_t len, cap;
  size_t n_changed;
  size_t n_unchanged;
  size_t n_skipped;
} kbc_app_manifest;

/* Where the enrichment registry's candidate set comes from; see
 * ladder_candidates, which is its only reader. The full pass has the walk's
 * manifest, and the single-file path has the live index plus the one
 * document it has just ingested. */
typedef struct {
  const kbc_app_manifest *manifest; /* NULL takes the index instead */
  const char *self_id;              /* the document just ingested, or NULL */
  const char *self_path;
  const char *self_title;
} app_ladder;

static kbc_status store_write_links(kbc_app *app, kbc_enrich_source *v,
                                    size_t nsrc, const app_ladder *lad,
                                    kbc_err *err);

struct kbc_app {
  kbc_config *cfg;   /* KBC_OWN, a private copy */
  kbc_store *store;  /* owns its own mutex */
  kbc_index *index;  /* guarded by lock */
  kbc_vecstore *vec; /* guarded by lock; NULL when the lane is off */
  kbc_embedder *embed;
  kbc_watcher *watch;
  char *vec_path; /* KBC_OWN */
  /* The query-embedding LRU, KBC_OWN: created in kbc_app_open, freed in
   * kbc_app_close, never touched by a reindex, and never REPLACED — the
   * pointer is written once, before any worker can see the app, so every
   * reader needs no lock of its own and the cache's own mutex in embed.c is
   * the only synchronisation in this path. That is what makes the stale-vector
   * question unnecessary here rather than merely unaddressed: the model name
   * is in the key and is read off the live wire on every call, a sidecar that
   * fails to handshake has no model to key under, and a replacement announcing
   * a different model is a different key. An owned object rather than a global
   * is what lets it hang off the app at all (AGENTS.md rule 5), and one
   * instance shared by the daemon is the original's process-wide behaviour. */
  kbc_query_cache *qcache;

  /* NOT the rwlock, and not a second view of it. The two indexers — the full
   * rebuild and the single-file path — run on different threads (an httpd
   * worker and the watcher) and each writes BOTH the store rows and the index,
   * and the rwlock covers neither of those for long: reindex_pass takes it
   * only for the final swap, and index_touch_one only around the mutation. A
   * rwlock separates threads that take it; it cannot exclude one that is not.
   * So the exclusion is a mutex of its own, and it is taken at the outermost
   * reindex entry point — which is also why the lock order is always
   * reindex_mu then lock, and never the reverse. */
  pthread_mutex_t reindex_mu;
  /* Names the build file uniquely per writer. kbc_str_write_file_atomic opens
   * "<path>.tmp.<pid>", so two writers in one process that agreed on ONE
   * <path>.build would open the same temp file with O_TRUNC and write over
   * each other from offset 0. reindex_mu already keeps the writers apart;
   * this is the second line, for a caller that forgets it. */
  _Atomic uint64_t build_seq;
  pthread_rwlock_t lock;
  pthread_mutex_t bus_lock;
  kbc_app_sub subs[KBC_APP_MAX_SUBS];
  uint64_t next_sub_id;

  _Atomic int64_t st_indexed;
  _Atomic int64_t st_runs;
  _Atomic int64_t st_searches;
  _Atomic int64_t st_degraded;
  _Atomic int64_t st_last_ns;
  _Atomic int64_t st_last_docs;
  _Atomic int64_t st_last_us;
  _Atomic int64_t st_terms;
  _Atomic int64_t st_docs;
  _Atomic int64_t st_bytes;
  /* Query-cache observability. hits/misses/drops are per CALL, not per byte,
   * and together they account for every search that entered the query lane: a
   * hit came from the LRU, a miss embedded and wrote back, a drop embedded and
   * FAILED so nothing was written. The ratio of hits to misses is the only
   * number that says whether the ~100 ms is being saved; drops is the one that
   * says the sidecar is refusing or dying.
   *
   * The header describes query_cache_drops as counting whole-cache
   * invalidations. There are none: the app never throws the cache away (see
   * the qcache field), so the field counts failed query embeds instead and its
   * comment in app.h should say so. */
  _Atomic int64_t st_qc_hits;
  _Atomic int64_t st_qc_misses;
  _Atomic int64_t st_qc_drops;
};


/* ------------------------------------------------------------------ util */

/* strdup is not in C17 and the build sets __STRICT_ANSI__; roll our own. */
static char *dup_n(const char *s, size_t n) {
  char *p = (char *)malloc(n + 1u);
  if (!p) {
    return NULL;
  }
  if (n > 0) {
    memcpy(p, s, n);
  }
  p[n] = '\0';
  return p;
}

static char *dup_cstr(const char *s) { return dup_n(s, strlen(s)); }

static bool path_has_ext(const char *rel, const char *ext) {
  size_t rl = strlen(rel);
  size_t el = strlen(ext);
  if (rl < el) {
    return false;
  }
  const char *tail = rel + (rl - el);
  for (size_t i = 0; i < el; i++) {
    char a = tail[i];
    if (a >= 'A' && a <= 'Z') {
      a = (char)(a - 'A' + 'a');
    }
    if (a != ext[i]) {
      return false;
    }
  }
  return true;
}

static bool is_indexable(const char *rel) {
  return path_has_ext(rel, ".html") || path_has_ext(rel, ".htm") ||
         path_has_ext(rel, ".md") || path_has_ext(rel, ".markdown");
}

/* The corpus ignore list is "glob-ish substrings" (see watcher.c), so a plain
 * substring test over the relative path and the leaf name is the contract. */
static bool corpus_ignores(const kbc_corpus_cfg *cc, const char *rel,
                           const char *leaf) {
  for (size_t i = 0; i < cc->ignore.len; i++) {
    const char *pat = cc->ignore.items[i];
    if (!pat || pat[0] == '\0') {
      continue;
    }
    if (strstr(rel, pat) != NULL || strstr(leaf, pat) != NULL) {
      return true;
    }
  }
  return false;
}

static void manifest_free(kbc_app_manifest *m) {
  for (size_t i = 0; i < m->len; i++) {
    free(m->items[i].path);
    free(m->items[i].title);
  }
  free(m->items);
  m->items = NULL;
  m->len = m->cap = 0;
}

static kbc_status manifest_push(kbc_app_manifest *m, const kbc_app_row *row,
                                kbc_err *err) {
  if (m->len == m->cap) {
    size_t cap = m->cap ? m->cap * 2u : 64u;
    if (cap < m->cap || cap > (SIZE_MAX / sizeof(*m->items))) {
      return kbc_err_set(err, KBC_ERR_NOMEM, "manifest would exceed %zu rows",
                         SIZE_MAX / sizeof(*m->items));
    }
    kbc_app_row *grown = (kbc_app_row *)realloc(m->items, cap * sizeof(*grown));
    if (!grown) {
      return kbc_err_set(err, KBC_ERR_NOMEM, "realloc for %zu manifest rows",
                         cap);
    }
    m->items = grown;
    m->cap = cap;
  }
  m->items[m->len] = *row;
  m->len++;
  return KBC_OK;
}

static kbc_status join_rel(kbc_str *out, const char *prefix, const char *leaf) {
  if (prefix[0] == '\0') {
    return kbc_str_puts(out, leaf);
  }
  kbc_status s = kbc_str_printf(out, "%s/%s", prefix, leaf);
  if (kbc_failed(s)) {
    return s;
  }
  if (out->len > (size_t)KBC_MAX_PATH_LEN) {
    return kbc_err_set(NULL, KBC_ERR_INVALID, "%s/%s: over the %u byte path cap",
                       prefix, leaf, (unsigned)KBC_MAX_PATH_LEN);
  }
  return KBC_OK;
}

/* ------------------------------------------------------------- chunker --- */

/* Unit 6: the passage chunker (chunk.rs:40-84), ported whole.
 *
 * A document is split into overlapping WORD windows so that each one is small
 * enough to embed in full. The models truncate at 512 tokens, so a single
 * whole-body vector only "sees" the first few hundred words of a long
 * artifact. It REPLACES the one-row-per-parsed-block chunking, which sized a
 * chunk by markdown shape: a document that is one long paragraph produced one
 * 10,000-word chunk, and the vector lane saw the first 400 words of it and
 * nothing else.
 *
 * It is a LEAF: three strings and two window sizes in, a chunk list out. No
 * clock, no map iteration, no I/O (chunk.rs:12-14). That is what makes a
 * re-index of an unchanged file produce byte-identical chunks, and it is why
 * it is testable at all — a chunker that read the clock could not be.
 *
 * The unit is a whitespace-delimited WORD, not a byte and not a character
 * (chunk.rs:62, `body.split_whitespace()`), and a chunk's text is its window's
 * words joined by a SINGLE ASCII space (chunk.rs:73), so a newline inside a
 * window becomes a space. Chunk 0 is not a window at all: it is the title and
 * the document's heading list, a short high-signal passage that lifts short
 * queries and keeps a body-less artifact representable (chunk.rs:50-59). It
 * is emitted only when that pair is not empty after trimming.
 */

/* include/kbc/chunk.h owns the window sizes, the two structs and the leaf's
 * signature. They are NOT redeclared here: a second copy of a public contract
 * is a second thing that can drift, and the test that used to mirror them
 * caught that drift with a sizeof assertion — which fires on layout rather
 * than on behaviour, the wrong failure for the wrong reason. KBC_CHUNK_WORDS,
 * KBC_CHUNK_OVERLAP_WORDS and KBC_MAX_CHUNKS_PER_DOC are the header's, and
 * kbc_chunk_document below is this file's definition of the header's
 * declaration, so the two cannot disagree about the signature. */

/* Rust's str::split_whitespace and str::trim work on the Unicode whitespace
 * set. The difference is unreachable here: parse.c collapses every run of
 * whitespace inside a block to a single space before anything sees it, so the
 * only byte that can separate two words is one of these. */
static bool chunk_space(char c) {
  return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' ||
         c == '\v';
}

/* Trims [*b, *e) in place to the span between the first and last byte that is
 * not whitespace. Rust trims each part and then the CONCATENATION
 * (chunk.rs:51-52), so "T" + "\n" + "" is "T" and not "T\n". */
static void chunk_trim(const char *s, size_t *b, size_t *e) {
  while (*b < *e && chunk_space(s[*b])) (*b)++;
  while (*e > *b && chunk_space(s[*e - 1])) (*e)--;
}

/* Appends one chunk, or counts it and drops it when the list is already at
 * `cap`. The dropped ones still land in `total`: the cap hides rows, it does
 * not unmake the document. */
static kbc_status chunk_push(kbc_arena *a, kbc_chunks *out, size_t cap,
                             const char *text, size_t len, kbc_err *err) {
  if (out->len >= cap) {
    out->total++;
    return KBC_OK;
  }
  if (out->len == out->cap) {
    size_t want = out->cap ? out->cap * 2u : 8u;
    if (want > cap) {
      want = cap;
    }
    if (want <= out->cap) {
      return kbc_err_set(err, KBC_ERR_INTERNAL,
                         "chunk list is stuck at %zu of a %zu cap", out->cap,
                         cap);
    }
    kbc_chunk *grown = kbc_arena_calloc(a, want, sizeof(*grown));
    if (grown == NULL) {
      return kbc_err_set(err, KBC_ERR_NOMEM, "chunk list of %zu", want);
    }
    if (out->items != NULL && out->len > 0) {
      memcpy(grown, out->items, out->len * sizeof(*grown));
    }
    out->items = grown;
    out->cap = want;
  }
  kbc_chunk *slot = &out->items[out->len];
  slot->idx = (uint32_t)out->len;
  slot->text = kbc_arena_strndup(a, text, len);
  if (slot->text == NULL) {
    return kbc_err_set(err, KBC_ERR_NOMEM, "chunk %u of %zu bytes", slot->idx,
                       len);
  }
  slot->text_len = len;
  out->len++;
  out->total++;
  return KBC_OK;
}

kbc_status kbc_chunk_document(kbc_arena *a, const char *title,
                                  const char *headings, const char *body,
                                  size_t body_len, size_t chunk_words,
                                  size_t overlap_words, size_t cap,
                                  kbc_chunks *out, kbc_err *err) {
  if (a == NULL || out == NULL) {
    return kbc_err_set(err, KBC_ERR_INVALID,
                       "chunk_document: arena and out are both required");
  }
  memset(out, 0, sizeof(*out));
  const char *t = title != NULL ? title : "";
  const char *h = headings != NULL ? headings : "";

  /* Chunk 0. */
  {
    size_t tb = 0, te = strlen(t);
    size_t hb = 0, he = strlen(h);
    chunk_trim(t, &tb, &te);
    chunk_trim(h, &hb, &he);
    kbc_str head;
    kbc_str_init(&head);
    kbc_status s = kbc_str_append(&head, t + tb, te - tb);
    if (s == KBC_OK) {
      s = kbc_str_putc(&head, '\n');
    }
    if (s == KBC_OK) {
      s = kbc_str_append(&head, h + hb, he - hb);
    }
    size_t b = 0, e = s == KBC_OK ? head.len : 0;
    if (s == KBC_OK) {
      chunk_trim(head.ptr, &b, &e);
    }
    if (s == KBC_OK && e > b) {
      s = chunk_push(a, out, cap, head.ptr + b, e - b, err);
    }
    kbc_str_free(&head);
    if (kbc_failed(s)) {
      return s;
    }
  }

  if (body == NULL || body_len == 0) {
    return KBC_OK;
  }
  /* How many words the body holds, counted first: a window ends at
   * min(start + window, nwords) and the walk stops when a window reaches the
   * last word (chunk.rs:69-80), so the count is the loop's stopping rule and
   * not a convenience. */
  size_t nwords = 0;
  for (size_t i = 0; i < body_len;) {
    while (i < body_len && chunk_space(body[i])) {
      i++;
    }
    if (i >= body_len) {
      break;
    }
    nwords++;
    while (i < body_len && !chunk_space(body[i])) {
      i++;
    }
  }
  if (nwords == 0) {
    return KBC_OK;
  }

  /* window.max(1) and step = window.saturating_sub(overlap).max(1)
   * (chunk.rs:66-67). The step guard is load-bearing: an overlap >= window
   * would make the step zero and the walk would never advance. */
  const size_t window = chunk_words > 0 ? chunk_words : 1u;
  const size_t step = window > overlap_words ? window - overlap_words : 1u;
  size_t wi = 0;  /* word index of the current window's first word */
  size_t pos = 0; /* its byte offset in `body` */
  for (;;) {
    const size_t end = window >= nwords - wi ? nwords : wi + window;
    size_t resume = 0;
    bool have_resume = false;
    size_t seen = 0;
    kbc_str text;
    kbc_str_init(&text);
    kbc_status s = KBC_OK;
    size_t p = pos;
    size_t idx = wi;
    while (idx < end && s == KBC_OK) {
      while (p < body_len && chunk_space(body[p])) {
        p++;
      }
      const size_t ws = p;
      while (p < body_len && !chunk_space(body[p])) {
        p++;
      }
      /* The next window starts `step` words in, so remember where that word
       * begins and the walk never rescans from the top of the body. */
      if (idx - wi == step) {
        resume = ws;
        have_resume = true;
      }
      if (seen > 0) {
        s = kbc_str_putc(&text, ' ');
      }
      if (s == KBC_OK) {
        s = kbc_str_append(&text, body + ws, p - ws);
      }
      seen++;
      idx++;
    }
    if (s == KBC_OK) {
      s = chunk_push(a, out, cap, text.ptr, text.len, err);
    }
    kbc_str_free(&text);
    if (kbc_failed(s)) {
      return s;
    }
    if (end == nwords) {
      break;
    }
    if (!have_resume) {
      resume = p; /* step == window: the next window starts where this ended */
    }
    wi += step;
    pos = resume;
  }
  return KBC_OK;
}

/* The document's chunk rows: the window list, projected onto the store's
 * (doc_id, ord, text) shape. `*total_out` is the count before the cap, which
 * is the number a log line has to report. */
static kbc_status document_chunks(kbc_arena *a, const kbc_parsed *p,
                                  const char *doc_id, kbc_chunk_in **out,
                                  size_t *n_out, size_t *total_out,
                                  kbc_err *err) {
  const kbc_blocks *b = kbc_parsed_blocks(p);
  kbc_str headings;
  kbc_str_init(&headings);
  kbc_str body;
  kbc_str_init(&body);
  /* The heading list, in document order, one per line (parser.rs:538 joins
   * heading lines with "\n"). The body is every block's text, headings
   * included — the same "all visible text" the original chunks. */
  for (size_t i = 0; i < b->len; i++) {
    if (b->items[i].heading_level > 0) {
      if (headings.len > 0 &&
          kbc_failed(kbc_str_putc(&headings, '\n'))) {
        kbc_str_free(&headings);
        kbc_str_free(&body);
        return kbc_err_set(err, KBC_ERR_NOMEM, "heading list");
      }
      if (kbc_failed(kbc_str_append(&headings, b->items[i].text,
                                    b->items[i].text_len))) {
        kbc_str_free(&headings);
        kbc_str_free(&body);
        return kbc_err_set(err, KBC_ERR_NOMEM, "heading list");
      }
    }
    if (body.len > 0 && kbc_failed(kbc_str_putc(&body, '\n'))) {
      kbc_str_free(&headings);
      kbc_str_free(&body);
      return kbc_err_set(err, KBC_ERR_NOMEM, "chunk body");
    }
    if (kbc_failed(kbc_str_append(&body, b->items[i].text,
                                  b->items[i].text_len))) {
      kbc_str_free(&headings);
      kbc_str_free(&body);
      return kbc_err_set(err, KBC_ERR_NOMEM, "chunk body");
    }
  }

  kbc_chunks list;
  kbc_status s = kbc_chunk_document(a, kbc_parsed_title(p), headings.ptr,
                                        body.ptr, body.len, KBC_CHUNK_WORDS,
                                        KBC_CHUNK_OVERLAP_WORDS,
                                        KBC_MAX_CHUNKS_PER_DOC, &list, err);
  kbc_str_free(&headings);
  kbc_str_free(&body);
  if (kbc_failed(s)) {
    return s;
  }

  kbc_chunk_in *rows = NULL;
  if (list.len > 0) {
    rows = calloc(list.len, sizeof(*rows));
    if (rows == NULL) {
      return kbc_err_set(err, KBC_ERR_NOMEM, "%zu chunk rows", list.len);
    }
    for (size_t i = 0; i < list.len; i++) {
      rows[i].doc_id = doc_id;
      rows[i].ord = list.items[i].idx;
      rows[i].text = list.items[i].text;
      rows[i].text_len = list.items[i].text_len;
    }
  }
  *out = rows;
  *n_out = list.len;
  *total_out = list.total;
  return KBC_OK;
}

/* --------------------------------------------------------------- summary */

/* First non-heading block, whitespace-collapsed and truncated. KBC_OWN; never
 * NULL on success, "" when the document has no prose. */
static char *summary_from_blocks(const kbc_blocks *b) {
  for (size_t i = 0; i < b->len; i++) {
    const kbc_block *blk = &b->items[i];
    if (blk->heading_level != 0 || blk->text_len == 0) {
      continue;
    }
    size_t n = blk->text_len;
    if (n > (size_t)KBC_APP_SUMMARY_MAX) {
      n = (size_t)KBC_APP_SUMMARY_MAX;
    }
    char *out = (char *)malloc(n + 1u);
    if (!out) {
      return NULL;
    }
    size_t w = 0;
    bool space = false;
    for (size_t j = 0; j < n; j++) {
      char c = blk->text[j];
      if (c == '\n' || c == '\t' || c == ' ' || c == '\r') {
        space = w > 0;
        continue;
      }
      if (space) {
        out[w++] = ' ';
        space = false;
      }
      out[w++] = c;
    }
    out[w] = '\0';
    return out;
  }
  return dup_cstr("");
}

/* --------------------------------------------------------------- ingest  */

/* The metadata ingest_file persists. KBC_OWN, freed with ingested_free.
 *
 * `link_paths` are the document's outbound link targets, already resolved
 * against its own directory but NOT yet checked against the store: a target
 * that has not been ingested yet is not a document, and whether it is one
 * cannot be known until the whole pass has run. They are therefore carried
 * out of the parse and written once, after the last document is in. */
typedef struct {
  char *title;
  int32_t heading_count;
  uint32_t content_hash;
  char **link_paths; /* KBC_OWN, corpus-relative */
  size_t n_links;
  /* The document's declared facets, as parallel KBC_OWN arrays. They are
   * carried out for the same reason the links are: the store write is one
   * transaction per document, and it belongs with the rest of the document's
   * row rather than inside the parse. */
  char **meta_keys;   /* KBC_OWN */
  char **meta_values; /* KBC_OWN */
  size_t n_metas;
} kbc_app_ingested;

static void ingested_free(kbc_app_ingested *g) {
  free(g->title);
  g->title = NULL;
  g->heading_count = 0;
  g->content_hash = 0;
  for (size_t i = 0; i < g->n_links; i++) free(g->link_paths[i]);
  free(g->link_paths);
  g->link_paths = NULL;
  g->n_links = 0;
  for (size_t i = 0; i < g->n_metas; i++) {
    free(g->meta_keys[i]);
    free(g->meta_values[i]);
  }
  free(g->meta_keys);
  free(g->meta_values);
  g->meta_keys = NULL;
  g->meta_values = NULL;
  g->n_metas = 0;
}

static kbc_status read_bounded(const char *path, kbc_str *out, kbc_err *err) {
  kbc_status s = kbc_str_read_file(path, out, err);
  if (kbc_failed(s)) {
    return s;
  }
  if (out->len > (size_t)KBC_MAX_ARTIFACT_BYTES) {
    return kbc_err_set(err, KBC_ERR_IO,
                       "%s: %zu bytes exceeds the %u byte artifact limit", path,
                       out->len, (unsigned)KBC_MAX_ARTIFACT_BYTES);
  }
  return KBC_OK;
}

/* Reads, parses, stores and chunks one file. The id is minted here because
 * the store row and the index must agree on it (types.h: the same
 * (corpus, path) always yields the same 12-hex id, so an edited file updates
 * its row in place instead of colliding with the UNIQUE(corpus, path) one). */
static kbc_status ingest_file(kbc_app *app, const char *corpus_name,
                              const char *root, const char *rel,
                              int64_t mtime_ns, int64_t size_bytes,
                              kbc_app_ingested *out, kbc_err *err) {
  memset(out, 0, sizeof(*out));

  kbc_str full;
  kbc_str_init(&full);
  kbc_status s = kbc_str_printf(&full, "%s/%s", root, rel);
  if (kbc_failed(s)) {
    kbc_str_free(&full);
    return kbc_err_set(err, KBC_ERR_NOMEM, "path buffer for %s/%s", root, rel);
  }
  if (full.len > (size_t)KBC_MAX_PATH_LEN) {
    s = kbc_err_set(err, KBC_ERR_INVALID, "%s: path over %u bytes", full.ptr,
                    (unsigned)KBC_MAX_PATH_LEN);
    kbc_str_free(&full);
    return s;
  }

  kbc_arena *fa = kbc_arena_new(64u * 1024u);
  if (!fa) {
    kbc_str_free(&full);
    return kbc_err_set(err, KBC_ERR_NOMEM, "arena for %s", full.ptr);
  }

  kbc_str raw;
  kbc_str_init(&raw);
  s = read_bounded(full.ptr, &raw, err);
  if (kbc_failed(s)) {
    goto fail;
  }

  kbc_parsed *p = kbc_parse(fa, raw.ptr, raw.len, rel, err);
  if (!p) {
    s = kbc_err_set(err, KBC_ERR_PARSE, "parse %s: malformed document",
                    full.ptr);
    goto fail;
  }
  const kbc_blocks *blocks = kbc_parsed_blocks(p);

  char *summary = summary_from_blocks(blocks);
  if (!summary) {
    s = kbc_err_set(err, KBC_ERR_NOMEM, "summary for %s", full.ptr);
    goto fail;
  }

  char id[KBC_MAX_ID_LEN + 1];
  kbc_id_for_artifact(id, corpus_name, rel);

  int32_t headings = 0;
  for (size_t i = 0; i < blocks->len; i++) {
    if (blocks->items[i].heading_level > 0) {
      headings++;
    }
  }

  kbc_artifact art;
  memset(&art, 0, sizeof(art));
  art.id = id;
  art.corpus = corpus_name;
  art.path = rel;
  art.title = kbc_parsed_title(p);
  art.kind = KBC_KIND_ARTIFACT;
  art.mtime_ns = mtime_ns;
  art.size_bytes = size_bytes;
  art.content_hash = kbc_fnv1a32(raw.ptr, raw.len);
  art.heading_count = headings;
  art.summary = summary;
  /* Copied into arena memory: raw.ptr may carry an embedded NUL, and the
   * stored record is a C string. */
  art.source = kbc_arena_strndup(fa, raw.ptr, raw.len);

  s = kbc_store_upsert_artifact(app->store, &art, err);
  if (kbc_failed(s)) {
    free(summary);
    goto fail;
  }

  /* WHEN THIS DOCUMENT FIRST BECAME KNOWN, recorded once and never again.
   * The write is INSERT OR IGNORE inside the store and that is the whole
   * point: a "created" sort needs an anchor that survives a reindex and a
   * file copy, and every other candidate timestamp fails one of the two —
   * mtime drifts on every edit, and the store row's own updated_at refreshes
   * on every pass. So the call is idempotent BY DESIGN, and the second pass
   * over an unchanged corpus must leave this value exactly where the first
   * pass put it; tests/test_app.c::a_reindex_does_not_move_the_first_seen_
   * anchor is the assertion of that.
   *
   * It rides on the ingest of the document rather than on the reindex pass
   * because that is where a document becomes known: the single-file update
   * path (reindex_one_locked) reaches this same function, so a document
   * added by the watcher gets its anchor on the same transaction as its row
   * instead of waiting for the next full pass.
   *
   * A failure here is LOGGED, not propagated. The document is indexed and
   * searchable either way; the anchor is a sort order, and a store that
   * cannot accept one more row has already failed the upsert above, so
   * failing the pass too would turn one lost sort key into a lost index. */
  {
    kbc_err local;
    kbc_err_reset(&local);
    if (kbc_failed(kbc_store_first_seen(app->store, id,
                                        kbc_now_ns() / 1000000000LL,
                                        &local))) {
      KBC_LOGW("first-indexed anchor for %s/%s not recorded: %s", corpus_name,
               rel, local.msg);
    }
  }

  /* Chunks are what a comment anchors to; a doc without them is uncommentable,
   * so they are part of the same transaction, not a later backfill.
   *
   * They are WORD WINDOWS, not parsed blocks (see the chunker): a block is a
   * markdown shape, and a document that is one long paragraph would be a
   * single 10,000-word chunk that the embedder truncates away. The invariant
   * this buys is that every window of the body is embedded in full, so a
   * passage in the last thousand words is as findable as one in the first. */
  kbc_chunk_in *chunks = NULL;
  size_t n_chunks = 0, total_chunks = 0;
  s = document_chunks(fa, p, art.id, &chunks, &n_chunks, &total_chunks, err);
  if (kbc_failed(s)) {
    free(summary);
    goto fail;
  }
  if (total_chunks > n_chunks) {
    /* The cap dropped rows. It is logged with the ORIGINAL count, because a
    * log that says "512 chunks" for a document that produced 5,000 is a log
    * that hides the truncation it exists to surface. */
    KBC_LOGW("%s/%s: %zu chunks, only the first %u stored", corpus_name, rel,
             total_chunks, (unsigned)KBC_MAX_CHUNKS_PER_DOC);
  }
  s = kbc_store_replace_chunks(app->store, chunks, n_chunks, err);
  free(chunks);
  if (kbc_failed(s)) {
    free(summary);
    goto fail;
  }

  /* The outbound link targets, carried out to be written once the pass has
   * stored every document — see kbc_app_ingested. */
  {
    const kbc_links *links = kbc_parsed_links(p);
    size_t nl = links->len;
    if (nl > APP_MAX_EDGES_PER_DOC) {
      KBC_LOGW("%s/%s: %zu links, only the first %u recorded", corpus_name, rel,
               nl, (unsigned)APP_MAX_EDGES_PER_DOC);
      nl = APP_MAX_EDGES_PER_DOC;
    }
    if (nl > 0) {
      out->link_paths = calloc(nl, sizeof(*out->link_paths));
      if (out->link_paths == NULL) {
        free(summary);
        s = kbc_err_set(err, KBC_ERR_NOMEM, "%zu link targets of %s/%s", nl,
                        corpus_name, rel);
        goto fail;
      }
      for (size_t i = 0; i < nl; i++) {
        out->link_paths[i] = dup_cstr(links->items[i].target);
        if (out->link_paths[i] == NULL) {
          free(summary);
          s = kbc_err_set(err, KBC_ERR_NOMEM, "link target copy for %s/%s",
                          corpus_name, rel);
          goto fail;
        }
        out->n_links++;
      }
    }
  }

  /* The document's declared facets. They REPLACE whatever the previous
   * ingest of this path recorded — a tag the author deleted from the file
   * must stop matching on this pass, not live on as a row nothing cleans
   * up. `kbc_parsed_metas` already expands a multi-valued key into one entry
   * per element, so the store receives one row per value. */
  {
    const kbc_metas *metas = kbc_parsed_metas(p);
    const size_t nm = metas->len;
    if (nm > 0) {
      out->meta_keys = calloc(nm, sizeof(*out->meta_keys));
      out->meta_values = calloc(nm, sizeof(*out->meta_values));
      if (out->meta_keys == NULL || out->meta_values == NULL) {
        free(summary);
        s = kbc_err_set(err, KBC_ERR_NOMEM, "%zu facets of %s/%s", nm,
                        corpus_name, rel);
        goto fail;
      }
      for (size_t i = 0; i < nm; i++) {
        out->meta_keys[i] = dup_cstr(metas->items[i].key);
        out->meta_values[i] = dup_cstr(metas->items[i].value);
        if (out->meta_keys[i] == NULL || out->meta_values[i] == NULL) {
          free(summary);
          s = kbc_err_set(err, KBC_ERR_NOMEM, "facet copy for %s/%s",
                          corpus_name, rel);
          goto fail;
        }
        out->n_metas++;
      }
    }
  }

  out->title = dup_cstr(art.title ? art.title : "");
  if (!out->title) {
    free(summary);
    s = kbc_err_set(err, KBC_ERR_NOMEM, "title copy for %s", full.ptr);
    goto fail;
  }
  out->heading_count = headings;
  out->content_hash = art.content_hash;
  free(summary);

  kbc_str_free(&raw);
  kbc_str_free(&full);
  kbc_arena_free(fa);
  return KBC_OK;

fail:
  kbc_str_free(&raw);
  kbc_str_free(&full);
  kbc_arena_free(fa);
  return s;
}

/* ----------------------------------------------------------------- walk  */

/* One directory, recursively. fstatat on the already-open dirfd — never
 * stat-then-open: the name can be swapped between the two, and a corpus is
 * hostile input by the threat model in AGENTS.md. */
static kbc_status walk_dir(kbc_app *app, const kbc_corpus_cfg *cc,
                           size_t corpus_index, const char *rel_prefix,
                           unsigned depth, kbc_app_manifest *m, kbc_err *err) {
  kbc_str dirpath;
  kbc_str_init(&dirpath);
  kbc_status s = kbc_str_printf(&dirpath, "%s/%s", cc->path, rel_prefix);
  if (kbc_failed(s)) {
    kbc_str_free(&dirpath);
    return kbc_err_set(err, KBC_ERR_NOMEM, "dir path for %s", cc->path);
  }

  DIR *d = opendir(dirpath.ptr);
  if (!d) {
    int saved = errno;
    s = kbc_err_set(err, KBC_ERR_IO, "opendir %s: %s", dirpath.ptr,
                    strerror(saved));
    kbc_str_free(&dirpath);
    return s;
  }
  const int dfd = dirfd(d);
  if (dfd < 0) {
    closedir(d);
    kbc_str_free(&dirpath);
    return kbc_err_set(err, KBC_ERR_INTERNAL, "dirfd %s: no descriptor",
                      dirpath.ptr);
  }

  struct dirent *ent;
  /* errno is cleared immediately before EVERY readdir, never once before the
   * loop. readdir signals end-of-directory by returning NULL and leaving errno
   * UNCHANGED, so whatever the last syscall in the previous iteration left
   * behind is what the check after the loop reads. The body below calls
   * fstatat, which sets errno=ENOENT for exactly the case its own comment calls
   * harmless — a file that vanished between readdir and fstatat — and recurses
   * into walk_dir, which calls opendir. Any of those made an ordinary,
   * successful walk report "readdir <path>: No such file or directory" and
   * fail the reindex. It reproduced about one run in three under a corpus
   * being rewritten underneath the walk. */
  for (;;) {
    errno = 0;
    ent = readdir(d);
    if (ent == NULL) {
      break;
    }
    if (ent->d_name[0] == '.') {
      continue; /* "." ".." and every dotfile */
    }

    kbc_str rel;
    kbc_str_init(&rel);
    if (kbc_failed(join_rel(&rel, rel_prefix, ent->d_name))) {
      s = kbc_err_set(err, KBC_ERR_INVALID, "%s/%s: path over the %u byte cap",
                      cc->path, rel_prefix, (unsigned)KBC_MAX_PATH_LEN);
      kbc_str_free(&rel);
      closedir(d);
      kbc_str_free(&dirpath);
      return s;
    }

    struct stat st;
    if (fstatat(dfd, ent->d_name, &st, 0) != 0) {
      /* Vanished between readdir and fstatat: not an error, and not a removal
       * either. The walk has no authority over what it could not stat — the
       * file is simply not in the manifest, and the store still holds its row.
       * The reconcile sweep is where that gets decided: it re-stats every
       * stored row the walk did not see and removes the ones the filesystem
       * says are gone. Treating the miss here as "it is not there any more"
       * would make the walk, which only ever LOOKS, the thing that deletes. */
      kbc_str_free(&rel);
      continue;
    }

    if (S_ISDIR(st.st_mode)) {
      if (depth + 1u >= KBC_APP_MAX_DEPTH) {
        KBC_LOGW("%s: nesting over %u levels, not descending", rel.ptr,
                 (unsigned)KBC_APP_MAX_DEPTH);
        kbc_str_free(&rel);
        continue;
      }
      if (corpus_ignores(cc, rel.ptr, ent->d_name)) {
        kbc_str_free(&rel);
        continue;
      }
      s = walk_dir(app, cc, corpus_index, rel.ptr, depth + 1u, m, err);
      kbc_str_free(&rel);
      if (kbc_failed(s)) {
        closedir(d);
        kbc_str_free(&dirpath);
        return s;
      }
      continue;
    }

    if (!S_ISREG(st.st_mode) || !is_indexable(rel.ptr) ||
        corpus_ignores(cc, rel.ptr, ent->d_name)) {
      kbc_str_free(&rel);
      continue;
    }
    if (st.st_size <= 0 ||
        (uintmax_t)st.st_size > (uintmax_t)KBC_MAX_ARTIFACT_BYTES) {
      KBC_LOGW("%s/%s: %ju bytes is out of range (1..%u), skipped", cc->path,
               rel.ptr, (uintmax_t)st.st_size,
               (unsigned)KBC_MAX_ARTIFACT_BYTES);
      m->n_skipped++;
      kbc_str_free(&rel);
      continue;
    }

    kbc_app_row row;
    memset(&row, 0, sizeof(row));
    row.corpus_index = corpus_index;
    row.path = dup_cstr(rel.ptr);
    if (!row.path) {
      s = kbc_err_set(err, KBC_ERR_NOMEM, "manifest row for %s", rel.ptr);
      kbc_str_free(&rel);
      closedir(d);
      kbc_str_free(&dirpath);
      return s;
    }
    row.mtime_ns = (int64_t)st.st_mtim.tv_sec * 1000000000LL +
                   (int64_t)st.st_mtim.tv_nsec;
    row.size_bytes = (int64_t)st.st_size;
    kbc_id_for_artifact(row.id, cc->name, row.path);
    kbc_str_free(&rel);

    /* The unchanged check is one indexed lookup against the stored row, which
     * already carries the previous mtime and size. (mtime_ns, size) is the
     * change-detection fast path, never part of the name: an unedited file
     * costs one stat and one lookup, and no read of its bytes. */
    kbc_arena *qa = kbc_arena_new(4096u);
    if (!qa) {
      s = kbc_err_set(err, KBC_ERR_NOMEM, "arena for %s", row.path);
      free(row.path);
      closedir(d);
      kbc_str_free(&dirpath);
      return s;
    }
    kbc_artifact prev;
    memset(&prev, 0, sizeof(prev));
    kbc_err local;
    kbc_err_reset(&local);
    kbc_status q = kbc_store_get_artifact_by_path(app->store, qa, cc->name,
                                                  row.path, &prev, &local);
    if (kbc_failed(q)) {
      row.changed = true;
    } else {
      row.changed = !(prev.mtime_ns == row.mtime_ns &&
                      prev.size_bytes == row.size_bytes);
    }
    row.title = dup_cstr(prev.title ? prev.title : "");
    kbc_arena_free(qa);
    if (!row.title) {
      row.title = dup_cstr("");
    }
    if (!row.title) {
      s = kbc_err_set(err, KBC_ERR_NOMEM, "title copy for %s", row.path);
      free(row.path);
      closedir(d);
      kbc_str_free(&dirpath);
      return s;
    }

    if (row.changed) {
      m->n_changed++;
    } else {
      m->n_unchanged++;
    }
    s = manifest_push(m, &row, err);
    if (kbc_failed(s)) {
      free(row.path);
      free(row.title);
      closedir(d);
      kbc_str_free(&dirpath);
      return s;
    }
  }
  const int readdir_errno = errno;

  closedir(d);
  kbc_str_free(&dirpath);
  if (readdir_errno != 0) {
    return kbc_err_set(err, KBC_ERR_IO, "readdir %s/%s: %s", cc->path,
                       rel_prefix, strerror(readdir_errno));
  }
  return KBC_OK;
}

/* ------------------------------------------------------------- embedding */

/* Embeds one text. KBC_ARENA copy on success, NULL when the sidecar is
 * unusable. A dead sidecar degrades the search lane; it is never a reason to
 * fail a reindex.
 *
 * `why`, when not NULL, receives the sidecar's own message on failure. It is
 * what the quarantine counter records, so the Errors tab names the actual
 * refusal rather than a generic one; the query lane passes NULL because a
 * failed QUERY embed belongs to no document and must not be counted. */
static float *embed_one(kbc_app *app, kbc_arena *a, const char *text,
                        size_t *dim_out, kbc_err *why) {
  *dim_out = 0;
  if (!app->embed || !kbc_embedder_healthy(app->embed)) {
    return NULL;
  }
  const char *texts[1];
  texts[0] = text;
  float *out = NULL;
  kbc_err local;
  kbc_err_reset(&local);
  kbc_status s = kbc_embedder_embed(app->embed, a, texts, 1u,
                                    kbc_embedder_dim(app->embed), &out, &local);
  if (kbc_failed(s) || !out) {
    KBC_LOGW("embedder: %s, vector lane degraded", local.msg);
    if (why != NULL) {
      *why = local;
    }
    return NULL;
  }
  *dim_out = kbc_embedder_dim(app->embed);
  return *dim_out ? out : NULL;
}

/* ------------------------------------------------------ query cache ---- */

/* Embeds a QUERY, through the LRU.
 *
 * Same sidecar and same wire as embed_one; the difference is that the second
 * identical query costs a ~19 us linear scan instead of a 50-100 ms round
 * trip, and that a hit never opens the pipe at all — kbc_embed_query consults
 * the cache before the embedder, and reports embed_ms == 0 because on a hit
 * there is no embedding step to time. The query text is the only part of the
 * key: corpus, mode, limit and top-k are not in it and cannot be, since they
 * do not change what a query embeds to.
 *
 * WHY OUTSIDE app->lock. The search takes the read lock for {index, vec} a few
 * lines on, and the cache has a mutex of its own in embed.c; reaching the
 * cache under the rwlock would put a scan on the critical path of a lock a
 * reindex swap also wants. Nothing here runs with app->lock or reindex_mu
 * held, and nothing here needs a lock of the app's own: app->qcache is written
 * once in kbc_app_open and freed once in kbc_app_close, so the cache's own
 * mutex is the only synchronisation this path has.
 *
 * A NULL cache is not a second-class mode: it is embed_one, the call this file
 * made before the cache existed. That covers an app with no sidecar and an app
 * whose cache could not be allocated. */
static float *embed_query(kbc_app *app, kbc_arena *a, const char *text,
                          size_t *dim_out, kbc_err *why) {
  *dim_out = 0;
  if (app->qcache == NULL) {
    return embed_one(app, a, text, dim_out, why);
  }
  kbc_query_outcome oc;
  kbc_err local;
  kbc_err_reset(&local);
  const kbc_status st =
      kbc_embed_query(app->embed, app->qcache, a, text, &oc, &local);

  if (oc.cache_hit) {
    atomic_fetch_add_explicit(&app->st_qc_hits, 1, memory_order_relaxed);
  } else if (st == KBC_OK) {
    atomic_fetch_add_explicit(&app->st_qc_misses, 1, memory_order_relaxed);
  }
  if (kbc_failed(st) || oc.vec == NULL || oc.dim == 0) {
    if (st == KBC_OK) {
      /* A success carrying no vector is still a failed query, and saying
       * "success" in the log would send an operator looking at the sidecar
       * for a refusal that never happened. */
      (void)kbc_err_set(&local, KBC_ERR_INTERNAL,
                        "query embed returned no vector");
    }
    KBC_LOGW("embedder: %s, query lane degraded", local.msg);
    atomic_fetch_add_explicit(&app->st_qc_drops, 1, memory_order_relaxed);
    if (why != NULL) {
      *why = local;
    }
    return NULL;
  }
  /* No query text in the log: it is caller-supplied and rule 9 says an
   * unescaped query fragment does not go into a log line. */
  KBC_LOGD("query cache: %s, %llu ms of embedding",
           oc.cache_hit ? "hit" : "miss", (unsigned long long)oc.embed_ms);
  *dim_out = oc.dim;
  return oc.vec;
}

/* --------------------------------------------------------- quarantine --- */

/* QUARANTINE_THRESHOLD (indexer.rs:39): a document whose embedding has failed
 * this many times stops being embedded. It does NOT stop being indexed — the
 * embedding column is nullable by design, so a gated document keeps its rows
 * and stays keyword-searchable, which is the whole point: an operator can
 * still find and read the document that is too big for the model. */
#define APP_QUARANTINE_THRESHOLD 3

/* Buffer for the "fnv1a32-" + 8 hex digits the helper writes. Sized here
 * rather than taken from the store's private ceiling: this is a local
 * formatting buffer, and the store still validates what arrives. */
#define APP_CONTENT_HASH_MAX 32u

/* The content hash the gate and the recorder agree on, as the STRING the
 * store's `content_hash` column holds.
 *
 * The hash is `kbc_fnv1a32` over the file's BYTES — the same function, over
 * the same input, that fills `kbc_artifact.content_hash` above, so "the same
 * document by content" means one thing everywhere in this file. Not a security
 * hash and not trying to be: its only job is to answer "are these the same
 * bytes as the ones that failed", and a collision merely lets a document keep
 * a budget it did not earn.
 *
 * BYTES, not the searchable text and not the mtime: a whitespace-only edit
 * leaves the indexed text identical while being exactly the kind of change an
 * operator makes when they are trying to fix an oversized document. Hashing
 * anything derived from the parse would let that edit look like no edit at
 * all and keep the document in quarantine forever. */
static void content_hash_str(const char *bytes, size_t len, char *out,
                             size_t out_cap) {
  snprintf(out, out_cap, "fnv1a32-%08" PRIx32, kbc_fnv1a32(bytes, len));
}

/* Whether these bytes are over their embed budget. The gate's ONLY input is
 * the errors.retry_count the store keeps for the path, which is why
 * kbc_store_record_error counts rather than duplicating: a store that
 * inserted a fresh row per failure would reset the count to zero on every
 * pass and gate nothing, forever.
 *
 * The count is asked for by (path, content_hash), not by path alone. That is
 * the whole point of the parameter: editing a document is how an operator
 * fixes one that failed, so a document whose bytes moved is a different
 * document and its failure count is not this one's. Passing NULL here instead
 * would ask the store a different question — "how bad is it", the operator's
 * question, not the gate's — and would keep a fixed document in quarantine
 * until somebody cleared the error by hand.
 *
 * A store read that FAILS is not a gate: it is a broken database, and
 * answering "not quarantined" there is the safe direction — we would embed a
 * document we should have skipped, which costs one sidecar round trip. The
 * converse would silently drop the vector lane for a document that had never
 * failed. That is a judgement call, not a proof, and it is the reason this
 * branch logs rather than returning silently. */
static bool embed_gated(kbc_app *app, const char *corpus, const char *rel_path,
                        const char *content_hash) {
  int64_t retries = 0;
  kbc_err local;
  kbc_err_reset(&local);
  if (kbc_failed(kbc_store_retry_count_for_path(app->store, corpus, rel_path,
                                                content_hash, &retries,
                                                &local))) {
    KBC_LOGW("quarantine gate for %s/%s: %s; embedding anyway", corpus,
             rel_path, local.msg);
    return false;
  }
  if (retries < (int64_t)APP_QUARANTINE_THRESHOLD) {
    return false;
  }
  /* indexer.rs:2453 logs the same thing. It is a WARNING and not a debug line
   * because a document silently losing its vector is exactly the sort of
   * degradation an operator has to be able to see in a log. */
  KBC_LOGW("%s/%s: %lld embed failures, skipping the embed (still indexed, "
           "still keyword-searchable)",
           corpus, rel_path, (long long)retries);
  return true;
}

/* The row-id mint for the store tables this file writes BY HAND, i.e. the two
 * whose contract says the CALLER names the row: an error row (store.h:
 * "e-" + 6 base32) and an index run ("r-" + 6 base32). ONE function, because
 * two id schemes in one file is how a reader ends up unable to tell which
 * rows are comparable and which are not, and because the alphabet below has
 * to be the same in both or a "r-" id is not the shape the store documents.
 *
 * The name is (what the row is about) x (what makes this attempt different)
 * x salt, hashed: the salt separates the per-corpus runs of one pass, and
 * kbc_now_ns() separates two passes. Both are folded into the SAME 32 bits
 * the way the error id already was — a collision is not a correctness
 * problem, because the row that loses is a run row that reports KBC_ERR_*
 * and is logged, never a document row. */
static void app_row_id(char *out, char prefix, const char *about_a,
                       const char *about_b, uint32_t salt) {
  uint32_t h = kbc_fnv1a32(about_a, strlen(about_a));
  h ^= kbc_fnv1a32(about_b, strlen(about_b));
  h ^= salt;
  h ^= (uint32_t)(kbc_now_ns() & 0xFFFFFFFFu);
  static const char hex[] = "0123456789abcdefghjkmnpqrstvwxyz";
  out[0] = prefix;
  out[1] = '-';
  for (unsigned i = 0; i < 6u; i++) {
    out[2u + i] = hex[h & 31u];
    h >>= 5;
  }
  out[8] = '\0';
}


/* Records one embed failure for a path, durably.
 *
 * The id is minted here, not in the store, because the store's contract says
 * the CALLER names the row (store.h: "e-" + 6 base32) and the natural stable
 * name for this failure is (corpus, path, attempt): two failures of the same
 * path are two different rows as far as the id is concerned even though
 * record_error collapses them into one row with a higher count.
 *
 * `content_hash` rides along because that is what makes the count
 * hash-scoped in the original (retry_count_for_path_hash): an edit produces a
 * new hash and a fresh budget. It is the SAME value embed_gated is handed on
 * the next pass, computed by the same helper over the same bytes — a recorder
 * and a gate that disagreed about the hash would make every future lookup
 * answer "a different document" and nothing would ever be gated again.
 *
 * A failure to RECORD is logged and not propagated: the document is already
 * indexed without a vector, which is the state the gate exists to make
 * permanent, and failing the whole reindex over a bookkeeping row would trade
 * a degraded search lane for a failed one. */
static void record_embed_failure(kbc_app *app, const char *corpus,
                                 const char *rel_path, const char *content_hash,
                                 const char *msg) {
  char id[KBC_MAX_ID_LEN + 1];
  /* What makes this failure a different ROW is the attempt, so the clock is
   * part of the name; see app_row_id for the one scheme. */
  app_row_id(id, 'e', corpus, rel_path, 0u);

  kbc_error_row row;
  memset(&row, 0, sizeof row);
  row.id = id;
  row.kind = "embed";
  row.corpus = corpus;
  row.path = rel_path;
  row.message = msg;
  /* The hash of the bytes THIS failure is about, not a placeholder: the gate
   * looks the row up by it on the next pass, so NULL here would make every
   * future hash lookup answer "a different document" and gate nothing. */
  row.content_hash = content_hash;
  row.retry_count = 0; /* record_error owns the counter; this is the first. */
  row.created_at = kbc_now_ns() / 1000000000LL;
  row.dismissed = false;

  kbc_err local;
  kbc_err_reset(&local);
  if (kbc_failed(kbc_store_record_error(app->store, &row, &local))) {
    KBC_LOGW("embed failure for %s/%s not recorded: %s", corpus, rel_path,
             local.msg);
  }
}

/* Embeds a document's text UNLESS the path is over its embed budget, and
 * records the failure when the sidecar refuses one.
 *
 * THE INVARIANT, and the reason this is one function rather than a check at
 * each call site: a gated pass must NOT clear the error row. The obvious
 * "successful index clears this path's open errors" step — which the original
 * does, indexer.rs:2958-2968 — is exactly wrong here, because the gate reads
 * that row on the NEXT pass. Clearing it un-gates the document, the next pass
 * takes a real embed attempt, the embed fails, the row comes back at count 1,
 * and three passes later the document is gated again: a document that fails
 * forever oscillates in and out of quarantine and is embedded forever, which
 * is the failure the gate was built for (the 2026-08-21 ci-host OOM loop,
 * indexer.rs:6800-6810). A gated pass proves NOTHING about whether the failure
 * condition is gone — the embed was never attempted — so it must leave the
 * row alone. The only ways out are an operator clearing the error or a content
 * change, and both are deliberate acts.
 *
 * `*out` is NULL for every reason the vector is absent: no embedder, a gated
 * path, or a sidecar that refused. The caller indexes either way; the
 * embedding column is nullable and a document without one is still a
 * document.
 *
 * LOCKING. Neither lock is taken here. The store has its own mutex, taken and
 * released inside each kbc_store_* call; the embedder has its own. The caller
 * holds reindex_mu (the OUTER lock) and is not holding app->lock — both call
 * `raw`/`raw_len` are the document's BYTES, not the searchable text, and they
 * exist only so the gate can name the content: a caller that could not say
 * what the document is could not ask "is this still the one that failed?".
 *
 * LOCKING. Neither lock is taken here. The store has its own mutex, taken and
 * released inside each kbc_store_* call; the embedder has its own. The caller
 * holds reindex_mu (the OUTER lock) and is not holding app->lock — both call
 * sites embed before the write lock, exactly as they did before this gate
 * existed. So the new path adds no edge to the lock order and cannot deadlock
 * against it. */
static float *embed_document(kbc_app *app, kbc_arena *a, const char *corpus,
                             const char *rel_path, const char *raw,
                             size_t raw_len, const char *text,
                             size_t *dim_out) {
  *dim_out = 0;
  if (app->embed == NULL) {
    return NULL;
  }
  /* Computed once, used by BOTH the gate and the recorder. Two hash values
   * would be two different documents as far as the store is concerned, and the
   * count would never be found again. */
  char hash[APP_CONTENT_HASH_MAX];
  content_hash_str(raw, raw_len, hash, sizeof hash);
  if (embed_gated(app, corpus, rel_path, hash)) {
    return NULL;
  }
  /* Health is sampled BEFORE the call, not after: kbc_embedder_embed marks
   * the sidecar unhealthy on a sidecar-reported error, so asking afterwards
   * would find it dead and conclude the failure belonged to nobody. The
   * question is "did a REACHABLE sidecar refuse this document", and only the
   * reading from before the call answers it. */
  const bool reachable = kbc_embedder_healthy(app->embed);
  kbc_err why;
  kbc_err_reset(&why);
  float *v = embed_one(app, a, text, dim_out, &why);
  if (v == NULL && reachable) {
    /* Reachable going in and no vector coming out is a refusal, not a missing
     * embedder: that is the failure the counter is for. A sidecar that was
     * already dead is not this document's fault and is not counted — every
     * document in the corpus would pay for one broken process. */
    record_embed_failure(app, corpus, rel_path, hash,
                         why.msg[0] != '\0' ? why.msg : "embed failed");
  }
  return v;
}

/* Splits the configured `[embedder] command` into an argv. The sidecar is not
 * a bare binary — `kb-embedder --model NAME --cache DIR` is the only way to
 * say which model to load — so the config value is a command LINE, not a
 * path. Splitting happens here rather than in config.c so the frozen
 * `char *embedder_cmd` contract keeps its meaning ("the command to run") while
 * what is exec'd stays honest about the arguments.
 *
 * Whitespace separates; '...' and "..." group; a backslash escapes the next
 * character inside quotes. No expansion of any kind: nothing here reaches a
 * shell, the argv is exec'd directly. `buf` is a mutable copy the caller owns
 * and frees — the words point into it, and kbc_embedder_start copies them
 * before returning, so the config value itself is never modified (it is
 * written back verbatim when the config is saved). */
static kbc_status split_command(char *buf, const char **argv, size_t max,
                                 kbc_err *err) {
  size_t n = 0;
  char *p = buf;
  while (*p != '\0') {
    while (*p == ' ' || *p == '\t' || *p == '\n') {
      p++;
    }
    if (*p == '\0') {
      break;
    }
    if (n + 1 >= max) {
      return kbc_err_set(err, KBC_ERR_INVALID,
                         "embedder command has more than %zu words", max - 1);
    }
    argv[n++] = p;
    char quote = '\0';
    while (*p != '\0') {
      if (quote != '\0') {
        if (*p == quote) {
          quote = '\0';
          p++;
          continue;
        }
        if (*p == '\\' && p[1] != '\0' && quote == '"') {
          p++;
        }
        p++;
        continue;
      }
      if (*p == '\'' || *p == '"') {
        quote = *p++;
        continue;
      }
      if (*p == ' ' || *p == '\t' || *p == '\n') {
        break;
      }
      p++;
    }
    if (quote != '\0') {
      return kbc_err_set(err, KBC_ERR_INVALID,
                         "embedder command has an unterminated %c quote",
                         quote);
    }
    if (*p != '\0') {
      *p++ = '\0';
    }
  }
  argv[n] = NULL;
  if (n == 0) {
    return kbc_err_set(err, KBC_ERR_INVALID,
                       "embedder command holds no executable");
  }
  return KBC_OK;
}

/* The text a document is indexed by: the title twice (the BM25 length-norm
 * convention, and the caller's decision per index.h), then the extracted
 * prose. Raw markup would only contribute terms nobody searches for. Shared by
 * the full build and the one-document update — two copies of this rule would
 * be two chances for the keyword lane to disagree with itself. */
static kbc_status searchable_text(const kbc_blocks *blocks, const char *title,
                                  kbc_str *out, kbc_err *err) {
  size_t b;
  for (b = 0; b < blocks->len + 2u; b++) {
    if (b < 2u) {
      if (kbc_failed(kbc_str_puts(out, title)) ||
          kbc_failed(kbc_str_putc(out, ' '))) {
        return kbc_err_set(err, KBC_ERR_NOMEM, "searchable text for \"%s\"",
                           title);
      }
      continue;
    }
    if (kbc_failed(kbc_str_putc(out, ' ')) ||
        kbc_failed(kbc_str_append(out, blocks->items[b - 2u].text,
                                  blocks->items[b - 2u].text_len))) {
      return kbc_err_set(err, KBC_ERR_NOMEM, "searchable text for \"%s\"",
                         title);
    }
  }
  return KBC_OK;
}

/* ------------------------------------------------------------- the build */

/* Pass two: turn the manifest into a sealed index, embedding as it goes.
 * Doc ids are the manifest positions, so a vecstore row and an index doc id
 * are the same number by construction. */
static kbc_status build_index(kbc_app *app, kbc_app_manifest *m,
                              kbc_index **ix_out, kbc_vecstore **vec_out,
                              int64_t *embedded, kbc_err *err) {
  *ix_out = NULL;
  *vec_out = NULL;
  *embedded = 0;

  kbc_index *ix = kbc_index_new();
  if (!ix) {
    return kbc_err_set(err, KBC_ERR_NOMEM, "kbc_index_new");
  }
  kbc_status s = kbc_index_begin_build(ix, err);
  if (kbc_failed(s)) {
    kbc_index_free(ix);
    return s;
  }

  /* The sidecar reports its dimension only once it has answered something,
   * and nothing has asked it anything yet at open time, so the store is
   * created on the first successful embed in the loop below rather than here.
   * `vec_tried` makes that a one-shot: a store that cannot be created is not
   * retried per document, and a healthy sidecar that never answers never
   * allocates one. */
  kbc_vecstore *vec = NULL;
  bool vec_tried = false;

  int64_t n_embedded = 0;
  for (size_t i = 0; i < m->len; i++) {
    kbc_app_row *row = &m->items[i];
    const kbc_corpus_cfg *cc = &app->cfg->corpora[row->corpus_index];

    kbc_str full;
    kbc_str_init(&full);
    if (kbc_failed(kbc_str_printf(&full, "%s/%s", cc->path, row->path))) {
      s = kbc_err_set(err, KBC_ERR_NOMEM, "path buffer for %s", row->path);
      kbc_str_free(&full);
      goto fail;
    }

    kbc_arena *fa = kbc_arena_new(64u * 1024u);
    if (!fa) {
      s = kbc_err_set(err, KBC_ERR_NOMEM, "arena for %s", full.ptr);
      kbc_str_free(&full);
      goto fail;
    }

    kbc_str raw;
    kbc_str_init(&raw);
    bool have_source = false;
    if (!row->changed) {
      /* The whole point of the (mtime_ns, size) fast path: an unedited file
       * must not have its bytes read. Its indexed text is whatever the last
       * committed store row holds, so re-reading the file here would smuggle
       * unindexed edits into the index behind the walk's back. */
      kbc_artifact prev;
      memset(&prev, 0, sizeof(prev));
      kbc_err local;
      kbc_err_reset(&local);
      kbc_status g = kbc_store_get_artifact(app->store, fa, row->id, true,
                                            &prev, &local);
      if (!kbc_failed(g) && prev.source) {
        s = kbc_str_append(&raw, prev.source, strlen(prev.source));
        if (kbc_failed(s)) {
          s = kbc_err_set(err, KBC_ERR_NOMEM, "stored source for %s",
                          row->path);
          kbc_str_free(&raw);
          kbc_arena_free(fa);
          kbc_str_free(&full);
          goto fail;
        }
        have_source = true;
      } else {
        /* No committed row behind the skip: the store is the authority and it
         * has nothing, so fall back to the file rather than index an empty
         * document. */
        KBC_LOGW("%s: no stored source for an unchanged file (%s), reading it",
                 row->path, local.msg);
      }
    }
    if (!have_source) {
      s = read_bounded(full.ptr, &raw, err);
    }
    if (kbc_failed(s)) {
      kbc_str_free(&raw);
      kbc_arena_free(fa);
      if (s != KBC_ERR_NOTFOUND) {
        kbc_str_free(&full);
        goto fail;
      }
      /* The file went away between the walk and this pass. Dropping the row
       * would punch a hole in the doc ids (index.h requires them dense), so
       * the doc is staged as a tombstone: zero tokens, still addressable by
       * path, matches nothing. */
      KBC_LOGW("%s vanished mid-reindex, staged as a tombstone", full.ptr);
      kbc_tokens empty;
      memset(&empty, 0, sizeof(empty));
      s = kbc_index_add_doc(ix, (uint32_t)i, cc->name, row->path, row->title,
                            KBC_KIND_ARTIFACT, &empty, err);
      kbc_str_free(&full);
      if (kbc_failed(s)) {
        goto fail;
      }
      continue;
    }

    kbc_parsed *p = kbc_parse(fa, raw.ptr, raw.len, row->path, err);
    if (!p) {
      s = kbc_err_set(err, KBC_ERR_PARSE, "parse %s: malformed document",
                      full.ptr);
      kbc_str_free(&raw);
      kbc_arena_free(fa);
      kbc_str_free(&full);
      goto fail;
    }
    if (row->title[0] == '\0') {
      const char *parsed_title = kbc_parsed_title(p);
      if (parsed_title && parsed_title[0] != '\0') {
        char *t = dup_cstr(parsed_title);
        if (!t) {
          s = kbc_err_set(err, KBC_ERR_NOMEM, "title for %s", full.ptr);
          kbc_str_free(&raw);
          kbc_arena_free(fa);
          kbc_str_free(&full);
          goto fail;
        }
        free(row->title);
        row->title = t;
      }
    }

    /* Searchable text: the title twice (the BM25 length-norm convention, and
     * the caller's decision per index.h), then the extracted prose. Raw markup
     * would only contribute terms nobody searches for. */
    kbc_str text;
    kbc_str_init(&text);
    s = searchable_text(kbc_parsed_blocks(p), row->title, &text, err);
    if (kbc_failed(s)) {
      goto text_fail;
    }

    kbc_tokens toks;
    memset(&toks, 0, sizeof(toks));
    s = kbc_tokenize(fa, text.ptr, text.len, &toks, err);
    if (kbc_failed(s)) {
      kbc_str_free(&text);
      kbc_str_free(&raw);
      kbc_arena_free(fa);
      kbc_str_free(&full);
      goto fail;
    }

    s = kbc_index_add_doc(ix, (uint32_t)i, cc->name, row->path, row->title,
                          KBC_KIND_ARTIFACT, &toks, err);
    if (kbc_failed(s)) {
      kbc_str_free(&text);
      kbc_str_free(&raw);
      kbc_arena_free(fa);
      kbc_str_free(&full);
      goto fail;
    }

    /* Embedded before the store decision: at this point nobody may know the
     * dimension yet, and a store sized for the wrong one is worse than none. */
    size_t dim = 0;
    /* Through the gate: a document over its embed budget keeps its keyword
     * row and loses only its vector. Taken while reindex_mu is held and
     * BEFORE the rwlock, exactly as the ungated embed was. */
    /* `raw` is the document's bytes — the file's, or the stored source of an
     * unchanged file, which is the same bytes by definition of unchanged. */
    float *v = embed_document(app, fa, cc->name, row->path, raw.ptr, raw.len,
                              text.ptr, &dim);
    if (v != NULL && vec == NULL && !vec_tried && dim > 0 && m->len > 0) {
      vec_tried = true;
      kbc_err local;
      kbc_err_reset(&local);
      vec = kbc_vecstore_new(dim, (uint32_t)m->len, &local);
      if (!vec) {
        KBC_LOGW("vector store: %s, keyword lane only", local.msg);
      }
    }
    if (v && vec) {
      if (dim == kbc_vecstore_dim(vec)) {
        kbc_err local;
        kbc_err_reset(&local);
        if (kbc_failed(kbc_vecstore_set(vec, (uint32_t)i, v, &local))) {
          KBC_LOGW("vector row %zu for %s: %s, dropping the vector lane", i,
                   full.ptr, local.msg);
          kbc_vecstore_free(vec);
          vec = NULL;
        } else {
          n_embedded++;
        }
      } else {
        /* Dimension drift between the sidecar and the store means the two
         * halves of the lane no longer agree; the keyword lane is still
         * correct, so degrade rather than publish mismatched rows. */
        KBC_LOGW("embedder returned dim %zu, store is %zu: vector lane off",
                 dim, kbc_vecstore_dim(vec));
        kbc_vecstore_free(vec);
        vec = NULL;
      }
    }

    kbc_str_free(&text);
    kbc_str_free(&raw);
    kbc_arena_free(fa);
    kbc_str_free(&full);
    continue;

  text_fail:
    /* searchable_text already named the document; keep its message. */
    kbc_str_free(&text);
    kbc_str_free(&raw);
    kbc_arena_free(fa);
    kbc_str_free(&full);
    goto fail;
  }

  s = kbc_index_end_build(ix, err);
  if (kbc_failed(s)) {
    goto fail;
  }
  *ix_out = ix;
  *vec_out = vec;
  *embedded = n_embedded;
  return KBC_OK;

fail:
  kbc_index_free(ix);
  if (vec) {
    kbc_vecstore_free(vec);
  }
  return s;
}

/* Publishes one writer's work. `full` is the whole-rebuild path: a build file
 * of THIS writer's own, renamed over the live index. Otherwise it is a
 * single-document update, and it does NOT rewrite anything — it appends the
 * delta to the journal beside the index and fsyncs that, which is the entire
 * point: the bytes the durability barrier has to push then scale with the
 * document that changed rather than with the size of the index.
 *
 * The build name carries a sequence number, not just ".build". Both writers
 * run in one process, and kbc_str_write_file_atomic opens "<path>.tmp.<pid>"
 * — a per-PROCESS name. A shared build name therefore meant the same temp
 * file opened with O_TRUNC by both, each writing an index from offset 0, and
 * the promoted file was a byte-level splice of two of them: kbc_index_open
 * rejected it and the daemon refused to start until an operator deleted the
 * index. reindex_mu is what actually keeps the writers apart; the sequence
 * number means a writer that forgets it still cannot corrupt the other. */
static kbc_status index_publish(kbc_app *app, const kbc_index *ix, bool full,
                                kbc_err *err) {
  if (!full) {
    /* kbc_index_checkpoint decides for itself between appending and rewriting
     * whole, so this stays correct when the delta outgrows the journal. Both
     * are still atomic: it writes through kbc_str_write_file_atomic, which is
     * temp file + fsync + rename, and reindex_mu already excludes the other
     * writer for the whole call. */
    return kbc_index_checkpoint(ix, app->cfg->index_path, err);
  }
  const uint64_t seq =
      atomic_fetch_add_explicit(&app->build_seq, 1u, memory_order_relaxed);
  kbc_str build;
  kbc_str_init(&build);
  kbc_status s =
      kbc_str_printf(&build, "%s.build.%" PRIu64, app->cfg->index_path, seq);
  if (kbc_failed(s)) {
    kbc_str_free(&build);
    return kbc_err_set(err, KBC_ERR_NOMEM, "build path for %s",
                       app->cfg->index_path);
  }
  s = kbc_index_save(ix, build.ptr, err);
  if (kbc_failed(s)) {
    /* A build file that never reached the live name is litter, and these
     * names are per-writer, so nothing would ever reuse or overwrite it. */
    (void)unlink(build.ptr);
    kbc_str_free(&build);
    return s;
  }
  /* The journal goes BEFORE the rename, and the order is the point. The
   * invariant is "index file plus journal replayed in order equals the live
   * index", and a rebuild's index already contains everything the journal
   * describes, so a surviving journal is a stale one. Rename first and a crash
   * in between leaves that stale journal to be replayed over a fresh index —
   * documents resurrected, ids renumbered against records that never
   * described them, an index that is actively wrong. Drop first and a crash
   * in between leaves the previous index with its last few deltas missing,
   * which the next walk of the corpus repairs, because the corpus on disk is
   * the authority. Wrong-but-recoverable beats corrupt. */
  s = kbc_index_drop_journal(app->cfg->index_path, err);
  if (kbc_failed(s)) {
    (void)unlink(build.ptr);
    kbc_str_free(&build);
    return s;
  }
  if (rename(build.ptr, app->cfg->index_path) != 0) {
    int saved = errno;
    (void)unlink(build.ptr);
    s = kbc_err_set(err, KBC_ERR_IO, "rename %s over %s: %s", build.ptr,
                    app->cfg->index_path, strerror(saved));
    kbc_str_free(&build);
    return s;
  }
  kbc_str_free(&build);
  return KBC_OK;
}

/* -------------------------------------------------------- orphan sweep */

/* The corpus on disk is the authority, and a full walk is the moment that
 * authority is re-established. A store row whose (corpus, path) this walk did
 * not see belongs to a file that has since been renamed or deleted on disk;
 * leaving it behind is a permanent orphan, because no later reindex will ever
 * see the path either. Deleting it here also keeps the row count in step with
 * the corpus: a rename is a delete plus an insert, never a silent second row.
 *
 * The sweep is scoped to the corpora in this config and matches on the exact
 * (corpus, path) pair, so a row of a different corpus — or the same path under
 * a different corpus — is never touched. */

/* Sorted set of "corpus\x1fpath" keys. 0x1f cannot occur in either part, so
 * the join is unambiguous. Sorted + bsearch rather than a hash table: one
 * comparison function, no per-entry allocation beyond the key itself. */
typedef struct {
  char **keys; /* KBC_OWN, each a strdup'd key */
  size_t n, cap;
} path_set;

static void path_set_free(path_set *ps) {
  for (size_t i = 0; i < ps->n; i++) {
    free(ps->keys[i]);
  }
  free(ps->keys);
  ps->keys = NULL;
  ps->n = 0;
  ps->cap = 0;
}

static int path_key_cmp(const void *a, const void *b) {
  return strcmp(*(char *const *)a, *(char *const *)b);
}

static kbc_status path_set_push(path_set *ps, const char *key, kbc_err *err) {
  if (ps->n == ps->cap) {
    size_t cap = ps->cap ? ps->cap * 2u : 64u;
    if (cap < ps->cap || cap > SIZE_MAX / sizeof(*ps->keys)) {
      return kbc_err_set(err, KBC_ERR_NOMEM, "path set would exceed %zu keys",
                         SIZE_MAX / sizeof(*ps->keys));
    }
    char **grown = (char **)realloc(ps->keys, cap * sizeof(*grown));
    if (!grown) {
      return kbc_err_set(err, KBC_ERR_NOMEM, "realloc for %zu path keys", cap);
    }
    ps->keys = grown;
    ps->cap = cap;
  }
  char *copy = dup_cstr(key);
  if (!copy) {
    return kbc_err_set(err, KBC_ERR_NOMEM, "path key copy for \"%s\"", key);
  }
  ps->keys[ps->n++] = copy;
  return KBC_OK;
}

static bool path_set_has(const path_set *ps, const char *key) {
  if (ps->n == 0) {
    return false;
  }
  return bsearch(&key, ps->keys, ps->n, sizeof(*ps->keys), path_key_cmp) !=
         NULL;
}
/* Asks the filesystem whether a stored row's file is still there. Only ENOENT
 * and ENOTDIR count as "gone": any other errno is a stat that did not ANSWER,
 * EACCES on a directory the daemon may not read being the ordinary one, and
 * answering "gone" to a question nobody could ask is how a permission problem
 * becomes data loss. The Rust original draws the same line and keeps the row
 * on a failed stat (indexer.rs:787, `Err(_) => continue`). */
static kbc_status row_file_gone(const kbc_corpus_cfg *cc, const char *rel,
                                bool *gone, kbc_err *err) {
  kbc_str full;
  kbc_str_init(&full);
  kbc_status s = kbc_str_printf(&full, "%s/%s", cc->path, rel);
  if (kbc_failed(s)) {
    kbc_str_free(&full);
    return kbc_err_set(err, KBC_ERR_NOMEM, "sweep path for %s/%s", cc->name,
                       rel);
  }
  struct stat sb;
  *gone = stat(full.ptr, &sb) != 0 && (errno == ENOENT || errno == ENOTDIR);
  kbc_str_free(&full);
  return KBC_OK;
}


/* One store row, one arena: the artifact's `path` is carved out of `qa`, so
 * the arena has to outlive every use of it — the probe key, the delete
 * warning and the log line all read `prev.path`. Keeping the arena inside
 * this function makes that lifetime the block it belongs to. */
static kbc_status sweep_one_id(kbc_app *app, const kbc_corpus_cfg *cc,
                               const char *id, const path_set *seen,
                               kbc_str *probe, size_t *removed, kbc_err *err) {
  kbc_arena *qa = kbc_arena_new(4096u);
  if (!qa) {
    return kbc_err_set(err, KBC_ERR_NOMEM, "arena for %s", id);
  }
  kbc_artifact prev;
  memset(&prev, 0, sizeof(prev));
  kbc_err local;
  kbc_err_reset(&local);
  kbc_status q =
      kbc_store_get_artifact(app->store, qa, id, false, &prev, &local);
  if (kbc_failed(q)) {
    /* Deleted between the id list and the read: nothing to orphan. */
    KBC_LOGW("sweep: artifact %s vanished mid-sweep: %s", id, local.msg);
    kbc_arena_free(qa);
    return KBC_OK;
  }

  kbc_status s = KBC_OK;
  kbc_str_clear(probe);
  s = kbc_str_printf(probe, "%s\x1f%s", cc->name,
                     prev.path ? prev.path : "");
  if (kbc_failed(s)) {
    s = kbc_err_set(err, KBC_ERR_NOMEM, "sweep key for %s", id);
  } else if (path_set_has(seen, probe->ptr)) {
    s = KBC_OK;
  } else {
    /* The corpus on disk is the authority, and this is the only place in a
     * full pass that gets to ask it. Not seeing the path in the manifest is
     * NOT the same as the file being gone: the walk declines a file it cannot
     * stat (vanished between readdir and fstatat), one whose extension is not
     * indexable, one an ignore pattern covers, and one whose size is out of
     * range. Every one of those is a rule the user can change back, and a
     * document that is still on disk is not a removal.
     *
     * The old code removed on the manifest's word alone, so a walk rule could
     * destroy a document that was sitting in the corpus, comments and all. The
     * Rust original makes the same distinction (indexer.rs:756-798) and calls
     * the opposite guard THE TRAP: `if path.try_exists() { continue }` kept
     * every still-existing file, so a de-mapped file's row — and every real
     * delete — lingered forever. Both arms are needed; only the filesystem
     * decides which one this is. */
    bool gone = false;
    s = row_file_gone(cc, prev.path != NULL ? prev.path : "", &gone, err);
    if (kbc_failed(s)) {
      kbc_arena_free(qa);
      return s;
    }
    if (!gone) {
      /* Kept, not dropped: dropping is the destructive direction and it is not
       * recoverable. `artifacts` cascades to chunks AND comments, and a
       * comment is not re-derivable from anything. Under-deleting costs a
       * stale row the next pass can still drop; over-deleting costs the
       * user's data the moment they change their mind about the rule. */
      KBC_LOGW("sweep: %s/%s is on disk but was not walked; the row stays",
               cc->name, prev.path != NULL ? prev.path : "?");
      s = KBC_OK;
    } else {
      /* The watcher's removal, not a second one: the same forget-then-delete
       * cascade, and through it the same watch.delete the watcher publishes.
       * One implementation of "a document went away" is the whole point — two
       * would drift, and the one that drifted is the one nobody tests. */
      kbc_status d = store_forget_path(app, cc->name, prev.path, err);
      if (kbc_failed(d)) {
        /* One stuck row must not cost the whole rescan: the row stays
         * visible and the next reindex tries again. */
        KBC_LOGW("sweep: cannot drop %s/%s: %s", cc->name,
                 prev.path != NULL ? prev.path : "?", err->msg);
        kbc_err_reset(err);
        s = KBC_OK;
      } else {
        (*removed)++;
        KBC_LOGI("sweep: %s/%s is no longer in the corpus, dropped", cc->name,
                 prev.path != NULL ? prev.path : "?");
      }
    }
  }
  kbc_arena_free(qa);
  return s;
}

/* Deletes the store rows for paths the walk did not see. `removed` is the
 * caller's log counter, not a manifest stat. */
static kbc_status sweep_orphans(kbc_app *app, const kbc_app_manifest *m,
                                size_t *removed, kbc_err *err) {
  path_set seen;
  memset(&seen, 0, sizeof(seen));
  kbc_str key;
  kbc_str_init(&key);

  kbc_status s = KBC_OK;
  for (size_t i = 0; i < m->len && !kbc_failed(s); i++) {
    kbc_str_clear(&key);
    s = kbc_str_printf(&key, "%s\x1f%s",
                       app->cfg->corpora[m->items[i].corpus_index].name,
                       m->items[i].path);
    if (kbc_failed(s)) {
      s = kbc_err_set(err, KBC_ERR_NOMEM, "seen-path key for %s",
                      m->items[i].path);
      break;
    }
    s = path_set_push(&seen, key.ptr, err);
  }
  if (!kbc_failed(s) && seen.n > 1) {
    qsort(seen.keys, seen.n, sizeof(*seen.keys), path_key_cmp);
  }
  kbc_str_free(&key);
  if (kbc_failed(s)) {
    path_set_free(&seen);
    return s;
  }

  for (size_t i = 0; i < app->cfg->ncorpora && !kbc_failed(s); i++) {
    const kbc_corpus_cfg *cc = &app->cfg->corpora[i];

    /* Every id for this corpus, collected before any delete: the paging
     * offset would skip rows once deleted rows shrink the result set. */
    kbc_strlist ids;
    kbc_strlist_init(&ids);
    for (size_t offset = 0; !kbc_failed(s);) {
      char **page = NULL;
      size_t np = 0;
      s = kbc_store_list_artifact_ids(app->store, cc->name, KBC_KIND_ARTIFACT,
                                     KBC_MAX_HITS, offset, &page, &np, err);
      if (kbc_failed(s)) {
        break;
      }
      for (size_t j = 0; j < np; j++) {
        kbc_status ps = kbc_strlist_push(&ids, page[j]);
        free(page[j]);
        if (kbc_failed(ps)) {
          s = kbc_err_set(err, KBC_ERR_NOMEM, "%zu stored ids for %s", np,
                          cc->name);
        }
      }
      free(page);
      if (kbc_failed(s) || np == 0) {
        break;
      }
      offset += np;
    }
    if (kbc_failed(s)) {
      kbc_strlist_free(&ids);
      break;
    }

    kbc_str probe;
    kbc_str_init(&probe);
    for (size_t j = 0; j < ids.len; j++) {
      s = sweep_one_id(app, cc, ids.items[j], &seen, &probe, removed, err);
      if (kbc_failed(s)) {
        break;
      }
    }
    kbc_str_free(&probe);
    kbc_strlist_free(&ids);
  }

  path_set_free(&seen);
  return s;
}

/* ------------------------------------------------- the index_runs ledger --
 *
 * A reindex PASS is an index run, and this is where that fact is recorded.
 * The store's row is per SOURCE — `index_runs.corpus` is NOT NULL and
 * REFERENCES sources(slug) — so a pass over N configured corpora opens N runs,
 * one per corpus, all with the same started_at, and closes them all together.
 * One row for the whole pass was the other option and it is wrong: a pass that
 * covered four corpora would be listed under whichever one the code happened
 * to name first, and `list_index_runs(corpus)` — the query the table exists to
 * answer — would answer "no runs" for the other three.
 *
 * THE COUNTS, defined here because the columns are two int64s and a reader
 * deserves to know which question they answer:
 *
 *   ok_count  documents of this corpus the pass covered without error: the
 *             changed ones it re-ingested, plus the unchanged ones it left
 *             alone, plus one that vanished mid-pass and is carried as a
 *             tombstone. It is "how much of this corpus this pass is sure
 *             about", not "how many bytes it wrote".
 *   err_count documents of this corpus the pass could NOT account for, plus,
 *             if the pass as a whole aborted, ONE — see runs_end.
 *
 * LOCKING, and the reason this is inside reindex_pass rather than in either
 * wrapper: reindex_pass is only ever reached with reindex_mu already held
 * (reindex_locked takes it for kbc_app_reindex, and reindex_one_locked holds
 * it across both of the places it calls the pass). So the run row is written
 * under the same outer lock as the store rows and the index of the pass it
 * describes. A run row written OUTSIDE reindex_mu could be written twice for
 * one pass, or interleaved with the single-file update that reindex_one runs
 * concurrently — and the store would then hold a run whose counts describe a
 * mix of two passes. There is no lock to take here, and none may be taken.
 *
 * index_touch_one writes NO run row: one file changing is not a pass, and a
 * ledger whose every watcher event is a "run" is a ledger nobody can read. The
 * single-file path's one call into reindex_pass — the full-scan fallback when
 * there is no index yet, or after a store write the single-file path could not
 * complete — IS a pass, and does open and close its runs like any other. */
typedef struct {
  char id[KBC_MAX_ID_LEN + 1];
  const char *corpus; /* BORROWED from the config */
  int64_t ok;
  int64_t err;
  bool open; /* the row is in the store and has not been finished */
} app_run;

typedef struct {
  app_run *v; /* KBC_OWN, one per configured corpus */
  size_t n;
} app_runs;

/* Opens one in-flight run per configured corpus. NEVER fails the pass: a run
 * row is a ledger entry, and refusing to index a corpus because its ledger
 * entry did not fit would trade a missing status row for a missing index. A
 * corpus whose row could not be written is marked closed and simply has no
 * run, which is the same shape as the row that was never opened. */
static void runs_begin(kbc_app *app, app_runs *runs) {
  runs->v = calloc(app->cfg->ncorpora, sizeof(*runs->v));
  runs->n = 0;
  if (runs->v == NULL) {
    KBC_LOGW("index runs for %zu corpora not recorded: out of memory",
             app->cfg->ncorpora);
    return;
  }
  runs->n = app->cfg->ncorpora;
  const int64_t now = kbc_now_ns() / 1000000000LL;
  for (size_t i = 0; i < runs->n; i++) {
    app_run *r = &runs->v[i];
    r->corpus = app->cfg->corpora[i].name;
    /* The corpus index is the salt, so the runs of ONE pass cannot mint the
     * same id twice; the clock inside app_row_id separates two passes. */
    app_row_id(r->id, 'r', r->corpus, "", (uint32_t)i);

    kbc_index_run row;
    memset(&row, 0, sizeof row);
    row.id = r->id;
    row.corpus = r->corpus;
    row.started_at = now;
    /* Negative is the store's "still in flight" (store.h: a NULL finished_at
     * expressed in a column that cannot be NULL), so the row reads as open
     * from the moment it is written until runs_end closes it. */
    row.finished_at = -1;
    row.ok_count = 0;
    row.err_count = 0;

    kbc_err local;
    kbc_err_reset(&local);
    if (kbc_failed(kbc_store_put_index_run(app->store, &row, &local))) {
      /* KBC_ERR_CONFLICT here is the foreign key: this corpus has no
       * `sources` row, which kbc_app_open writes and which a caller that
       * deleted the row out from under a running daemon can remove. The pass
       * is unaffected either way; the corpus simply has no run row. */
      KBC_LOGW("index run for corpus %s not opened: %s", r->corpus, local.msg);
      r->open = false;
      continue;
    }
    r->open = true;
  }
}

static app_run *run_of(app_runs *runs, size_t corpus_index) {
  if (runs->v == NULL || corpus_index >= runs->n) {
    return NULL;
  }
  return &runs->v[corpus_index];
}

static void runs_count_ok(app_runs *runs, size_t corpus_index) {
  app_run *r = run_of(runs, corpus_index);
  if (r != NULL) r->ok++;
}

/* Closes every open run, and is called on EVERY exit from the pass — the
 * success path and all seven failures. A run left open forever is worse than
 * no run row at all: `finished_at` negative is the ONLY thing that
 * distinguishes "this pass is still going" from "this pass died and nobody
 * noticed", and a crash that skips the close leaves a row that claims to be
 * in flight for the rest of the database's life.
 *
 * `aborted` is the pass's own status. A pass that did not complete carries
 * that fact into every run it had not finished, even one whose documents were
 * all individually fine: the index this run describes was never promoted, so
 * the run is not a statement about a corpus that is now correctly indexed. A
 * run that already counted a document-level error keeps that count. */
static void runs_end(kbc_app *app, app_runs *runs, bool aborted) {
  const int64_t now = kbc_now_ns() / 1000000000LL;
  for (size_t i = 0; i < runs->n; i++) {
    app_run *r = &runs->v[i];
    if (!r->open) continue;
    int64_t err = r->err;
    if (aborted && err == 0) {
      err = 1;
    }
    kbc_err local;
    kbc_err_reset(&local);
    if (kbc_failed(
            kbc_store_finish_index_run(app->store, r->id, now, r->ok, err,
                                       &local))) {
      KBC_LOGW("index run %s for corpus %s not finished: %s", r->id, r->corpus,
               local.msg);
    } else {
      r->open = false;
    }
  }
  free(runs->v);
  runs->v = NULL;
  runs->n = 0;
}

/* Walk -> ingest -> build -> save -> promote -> swap. On any failure before
 * the swap the old index keeps serving: the daemon never publishes a
 * half-built index, and the store keeps the rows it already committed.
 *
 * `_pass`, not `_locked`: it takes no lock of its own. The lock that makes
 * this safe against the other writer is reindex_mu, and it is taken by the
 * wrapper below for the WHOLE pass.
 *
 * SPLIT IN TWO, and the split is about the run ledger rather than about the
 * work: reindex_pass owns opening and closing the index_runs rows, and this
 * body does the pass. The body has seven early returns, each with a different
 * set of things to free, so a close on the way out of each of them is seven
 * chances to forget — and the failure that forgets is exactly the one that
 * leaves a run open forever. One wrapper, one close, every exit covered
 * including the ones nobody exercises.
 *
 * `runs` is the pass's ledger: already open, non-NULL, and every document the
 * pass covers is counted into it as the pass covers it. */
static kbc_status reindex_pass_body(kbc_app *app, app_runs *runs,
                                    kbc_err *err) {
  const int64_t t0 = kbc_now_ns();

  kbc_app_manifest m;
  memset(&m, 0, sizeof(m));
  kbc_status s = KBC_OK;
  for (size_t i = 0; i < app->cfg->ncorpora; i++) {
    if (kbc_failed(s = walk_dir(app, &app->cfg->corpora[i], i, "", 0u, &m,
                                err))) {
      manifest_free(&m);
      return s;
    }
  }
  if (m.len > (size_t)UINT32_MAX) {
    size_t n = m.len;
    manifest_free(&m);
    return kbc_err_set(err, KBC_ERR_INVALID,
                       "%zu documents exceeds the %u doc id ceiling", n,
                       (unsigned)UINT32_MAX);
  }

  /* The link graph is written after the loop, not inside it: a link target
   * that has not been ingested yet is not a document, and a document that
   * links to a sibling further down the walk must still produce an edge. */
  kbc_enrich_source *edge_srcs = calloc(m.len > 0 ? m.len : 1, sizeof(*edge_srcs));
  size_t n_edge_srcs = 0;
  if (edge_srcs == NULL) {
    manifest_free(&m);
    return kbc_err_set(err, KBC_ERR_NOMEM, "%zu link sources", m.len);
  }

  for (size_t i = 0; i < m.len; i++) {
    kbc_app_row *row = &m.items[i];
    /* Counted HERE and not derived from the manifest at the end, because the
     * manifest counts files the walk SAW and this counts documents the pass
     * ACCOUNTED FOR — a row whose ingest failed is an error for this run, and
     * counting it as covered would make a failed pass read as a complete one.
     * The unchanged rows never reach the body, so they are counted where they
     * are skipped: the pass covered those by deciding not to touch them,
     * which is the only thing it could have done. */
    if (!row->changed) {
      runs_count_ok(runs, row->corpus_index);
      continue;
    }
    const kbc_corpus_cfg *cc = &app->cfg->corpora[row->corpus_index];
    kbc_app_ingested g;
    if (kbc_failed(s = ingest_file(app, cc->name, cc->path, row->path,
                                   row->mtime_ns, row->size_bytes, &g, err))) {
      if (s != KBC_ERR_NOTFOUND) {
        app_edge_srcs_free(edge_srcs, n_edge_srcs);
        manifest_free(&m);
        return s;
      }
      /* Deleted between the walk and the ingest. The build pass will stage it
       * as a tombstone, so the doc ids stay dense and the index is correct
       * without a second full walk. */
      KBC_LOGW("%s/%s vanished mid-reindex, keeping the row as a tombstone",
               cc->name, row->path);
      kbc_err_reset(err);
      /* Carried as a tombstone: the pass accounted for the document even
       * though it is gone, so it is covered and not an error. */
      runs_count_ok(runs, row->corpus_index);
      continue;
    }
    if (g.title && g.title[0] != '\0') {
      free(row->title);
      row->title = g.title;
      g.title = NULL;
    }
    /* The facets, replaced per document, right here rather than in the
     * deferred link write: a facet names the document itself, so unlike a
     * link it has no dependency on any other document having been ingested
     * first. Every changed document reaches this call, including one that
     * declares no facets at all — an empty list is what deletes the facets a
     * previous version of the file used to carry. */
    s = kbc_store_replace_metas(app->store, cc->name, row->path,
                                (const char *const *)g.meta_keys,
                                (const char *const *)g.meta_values, g.n_metas,
                                err);
    if (kbc_failed(s)) {
      app_edge_srcs_free(edge_srcs, n_edge_srcs);
      manifest_free(&m);
      ingested_free(&g);
      return s;
    }
    /* EVERY changed document joins the write, links or not: a document that
     * lost its last link has to reach replace_edges with an empty target list,
     * which is what deletes the edges it used to have. */
    edge_srcs[n_edge_srcs].corpus = cc->name;
    edge_srcs[n_edge_srcs].src = row->path;
    edge_srcs[n_edge_srcs].targets = g.link_paths;
    edge_srcs[n_edge_srcs].n_targets = g.n_links;
    n_edge_srcs++;
    g.link_paths = NULL;
    g.n_links = 0;
    ingested_free(&g);
    /* The document is stored, faceted and about to have its links written:
     * this run covered it. Counted after the facet write, not after the
     * ingest, so a document whose facets the store rejected is not counted —
     * it exits the pass a few lines up with the error still set. */
    runs_count_ok(runs, row->corpus_index);
  }

  {
    /* The walk's manifest is the candidate set, and it is the one that costs
     * nothing: every document the corpus contains, already named. */
    const app_ladder lad = {&m, NULL, NULL, NULL};
    s = store_write_links(app, edge_srcs, n_edge_srcs, &lad, err);
  }
  app_edge_srcs_free(edge_srcs, n_edge_srcs);
  if (kbc_failed(s)) {
    manifest_free(&m);
    return s;
  }

  /* The walk is the authority on what the corpus contains, so the rows it did
   * not see go now — before the build, so the published index and the store
   * agree on the same document set. */
  size_t n_removed = 0;
  if (kbc_failed(s = sweep_orphans(app, &m, &n_removed, err))) {
    manifest_free(&m);
    return s;
  }

  /* THE DRAIN, and it runs over EVERY row the walk saw, not only the changed
   * ones: a document whose links were written before its target existed has
   * left a pending row, and only a lookup at the target can turn that into an
   * edge. Running it for the whole document set is what makes a reindex
   * order-independent — whichever document was visited first, the pass ends
   * with the same graph. It is one indexed lookup per document (the
   * (corpus, dst_path) index), and it is after the sweep so a source the
   * walk did not see has already lost its pending rows and cannot
   * contribute an edge. */
  for (size_t i = 0; i < m.len && !kbc_failed(s); i++) {
    const kbc_corpus_cfg *cc = &app->cfg->corpora[m.items[i].corpus_index];
    s = kbc_store_drain_pending(app->store, cc->name, m.items[i].path, err);
  }
  if (kbc_failed(s)) {
    manifest_free(&m);
    return s;
  }

  kbc_index *ix = NULL;
  kbc_vecstore *vec = NULL;
  int64_t embedded = 0;
  if (kbc_failed(s = build_index(app, &m, &ix, &vec, &embedded, err))) {
    manifest_free(&m);
    return s;
  }

  s = index_publish(app, ix, true, err);
  if (kbc_failed(s)) {
    manifest_free(&m);
    kbc_index_free(ix);
    if (vec) {
      kbc_vecstore_free(vec);
    }
    return s;
  }

  if (vec) {
    kbc_err local;
    kbc_err_reset(&local);
    if (kbc_failed(kbc_vecstore_save(vec, app->vec_path, &local))) {
      KBC_LOGW("vector store save: %s, keyword lane only", local.msg);
      kbc_vecstore_free(vec);
      vec = NULL;
    }
  }

  const int64_t t1 = kbc_now_ns();
  const int64_t docs = (int64_t)kbc_index_doc_count(ix);
  const int64_t terms = (int64_t)kbc_index_term_count(ix);
  const size_t n_changed = m.n_changed;
  const size_t n_unchanged = m.n_unchanged;
  const size_t n_skipped = m.n_skipped;

  /* The one place the live index changes. Both pointers move inside one write
   * lock, so a reader sees the new index only together with its vectors. */
  pthread_rwlock_wrlock(&app->lock);
  kbc_index *old_ix = app->index;
  kbc_vecstore *old_vec = app->vec;
  app->index = ix;
  app->vec = vec;
  if (old_ix) {
    kbc_index_free(old_ix);
  }
  if (old_vec) {
    kbc_vecstore_free(old_vec);
  }
  pthread_rwlock_unlock(&app->lock);

  atomic_store_explicit(&app->st_indexed, docs, memory_order_relaxed);
  atomic_store_explicit(&app->st_terms, terms, memory_order_relaxed);
  atomic_store_explicit(&app->st_docs, docs, memory_order_relaxed);
  atomic_store_explicit(&app->st_last_ns, t1, memory_order_relaxed);
  atomic_store_explicit(&app->st_last_docs, docs, memory_order_relaxed);
  atomic_store_explicit(&app->st_last_us, (t1 - t0) / 1000,
                        memory_order_relaxed);
  atomic_fetch_add_explicit(&app->st_runs, 1, memory_order_relaxed);

  manifest_free(&m);

  KBC_LOGI("reindex: %" PRId64 " docs, %" PRId64 " terms (%zu changed, %zu "
           "unchanged, %zu skipped, %zu removed, %" PRId64 " embedded) in %"
           PRId64 " us",
           docs, terms, n_changed, n_unchanged, n_skipped, n_removed, embedded,
           (t1 - t0) / 1000);
  return KBC_OK;
}

/* One reindex pass, and the index_runs rows that describe it.
 *
 * The ledger is opened HERE, above the body, and closed HERE, below it, on
 * every path out — success, and each of the body's seven failures. That is the
 * whole reason this wrapper exists: an in-flight run row that is never closed
 * is indistinguishable, forever, from a pass that is still running, and the
 * one time that matters most is the time the pass died.
 *
 * LOCKING: none is taken and none may be. Both callers of the pass reach it
 * holding reindex_mu (reindex_locked, and reindex_one_locked across both of
 * its calls), so the run row is already inside the outer lock that serialises
 * this pass against a single-file update. Taking reindex_mu here would
 * self-deadlock on a plain non-recursive mutex. */
static kbc_status reindex_pass(kbc_app *app, kbc_err *err) {
  app_runs runs;
  runs.v = NULL;
  runs.n = 0;
  runs_begin(app, &runs);
  kbc_status s = reindex_pass_body(app, &runs, err);
  runs_end(app, &runs, kbc_failed(s));
  return s;
}

/* The full rebuild, serialised against the single-file path.
 *
 * The rwlock cannot do this job. reindex_pass takes it only for the final
 * swap, which is after the publish has already renamed a file, so for the
 * whole of the walk, the ingest and the build the live index is unguarded — and
 * index_touch_one, which is what the watcher calls, does exactly that
 * concurrently. A rwlock separates threads that take it; it cannot exclude a
 * thread that is not.
 *
 * Holding reindex_mu across the WHOLE pass is also the invariant behind
 * "a rebuild never installs a snapshot older than one already promoted": the
 * walk reads the corpus, so a single-file update that commits and promotes
 * while the walk is running would otherwise be overwritten by a build that
 * started before it, never saw the newer revision, and installs its older one
 * as app->index — with the store at the newer revision and every status
 * endpoint reporting success.
 *
 * The lock is taken HERE, once, rather than inside the pass: reindex_pass has
 * seven early returns, and a lock taken in the body is a lock each of them
 * has to remember. reindex_one_locked is the only other caller of the pass and
 * it holds the same mutex; neither may take it twice. */
static kbc_status reindex_locked(kbc_app *app, kbc_err *err) {
  pthread_mutex_lock(&app->reindex_mu);
  kbc_status s = reindex_pass(app, err);
  pthread_mutex_unlock(&app->reindex_mu);
  return s;
}

/* ------------------------------------------------------------------ open */

static kbc_status config_clone(kbc_config **dst, const kbc_config *src,
                               kbc_err *err) {
  kbc_config *c = (kbc_config *)calloc(1, sizeof(*c));
  if (!c) {
    return kbc_err_set(err, KBC_ERR_NOMEM, "kbc_config copy");
  }
  c->port = src->port;
  c->bm25_k1 = src->bm25_k1;
  c->bm25_b = src->bm25_b;
  c->rrf_k = src->rrf_k;
  c->graph_boost = src->graph_boost;
  c->chunk_max_bytes = src->chunk_max_bytes;
  c->search_max_hits = src->search_max_hits;
  c->watcher_debounce_ms = src->watcher_debounce_ms;
  c->http_workers = src->http_workers;
  c->json_logs = src->json_logs;
  c->log_level = src->log_level;

  /* The eight owned strings, in header order. */
  char **slots[KBC_APP_CFG_FIELDS] = {
      &c->config_path, &c->data_dir, &c->db_path,      &c->index_path,
      &c->bind_addr,   &c->token_path, &c->token,      &c->embedder_cmd};
  const char *srcs[KBC_APP_CFG_FIELDS] = {
      src->config_path, src->data_dir, src->db_path,    src->index_path,
      src->bind_addr,   src->token_path, src->token,    src->embedder_cmd};

  for (size_t i = 0; i < KBC_APP_CFG_FIELDS; i++) {
    if (!srcs[i]) {
      continue;
    }
    *slots[i] = dup_cstr(srcs[i]);
    if (!*slots[i]) {
      kbc_config_free(c);
      return kbc_err_set(err, KBC_ERR_NOMEM, "copy of config field %zu", i);
    }
  }

  c->ncorpora = src->ncorpora;
  if (src->ncorpora > 0) {
    c->corpora = (kbc_corpus_cfg *)calloc(src->ncorpora, sizeof(*c->corpora));
    if (!c->corpora) {
      kbc_config_free(c);
      return kbc_err_set(err, KBC_ERR_NOMEM, "%zu corpora", src->ncorpora);
    }
    for (size_t i = 0; i < src->ncorpora; i++) {
      c->corpora[i].name = dup_cstr(src->corpora[i].name);
      c->corpora[i].path = dup_cstr(src->corpora[i].path);
      kbc_strlist_init(&c->corpora[i].ignore);
      for (size_t j = 0; j < src->corpora[i].ignore.len; j++) {
        if (kbc_failed(kbc_strlist_push(&c->corpora[i].ignore,
                                       src->corpora[i].ignore.items[j]))) {
          kbc_config_free(c);
          return kbc_err_set(err, KBC_ERR_NOMEM, "corpus %zu ignore %zu", i, j);
        }
      }
      if (!c->corpora[i].name || !c->corpora[i].path) {
        kbc_config_free(c);
        return kbc_err_set(err, KBC_ERR_NOMEM, "corpus %zu", i);
      }
    }
  }
  *dst = c;
  return KBC_OK;
}

static kbc_index *empty_index(kbc_err *err) {
  kbc_index *ix = kbc_index_new();
  if (!ix) {
    (void)kbc_err_set(err, KBC_ERR_NOMEM, "kbc_index_new");
    return NULL;
  }
  if (kbc_failed(kbc_index_begin_build(ix, err)) ||
      kbc_failed(kbc_index_end_build(ix, err))) {
    kbc_index_free(ix);
    return NULL;
  }
  return ix;
}

kbc_app *kbc_app_open(const kbc_config *cfg, kbc_err *err) {
  if (!cfg) {
    (void)kbc_err_set(err, KBC_ERR_INVALID, "kbc_app_open: cfg is NULL");
    return NULL;
  }
  if (!cfg->data_dir || !cfg->db_path || !cfg->index_path) {
    (void)kbc_err_set(err, KBC_ERR_INVALID,
                      "kbc_app_open: data_dir, db_path and index_path are "
                      "all required");
    return NULL;
  }
  if (cfg->ncorpora > (size_t)KBC_MAX_CORPORA) {
    (void)kbc_err_set(err, KBC_ERR_INVALID, "%zu corpora exceeds the %u limit",
                      cfg->ncorpora, (unsigned)KBC_MAX_CORPORA);
    return NULL;
  }

  kbc_app *app = (kbc_app *)calloc(1, sizeof(*app));
  if (!app) {
    (void)kbc_err_set(err, KBC_ERR_NOMEM, "kbc_app");
    return NULL;
  }
  if (pthread_rwlock_init(&app->lock, NULL) != 0) {
    free(app);
    (void)kbc_err_set(err, KBC_ERR_INTERNAL, "pthread_rwlock_init failed");
    return NULL;
  }
  if (pthread_mutex_init(&app->reindex_mu, NULL) != 0) {
    pthread_rwlock_destroy(&app->lock);
    free(app);
    (void)kbc_err_set(err, KBC_ERR_INTERNAL, "pthread_mutex_init failed");
    return NULL;
  }
  if (pthread_mutex_init(&app->bus_lock, NULL) != 0) {
    pthread_mutex_destroy(&app->reindex_mu);
    pthread_rwlock_destroy(&app->lock);
    free(app);
    (void)kbc_err_set(err, KBC_ERR_INTERNAL, "pthread_mutex_init failed");
    return NULL;
  }
  app->next_sub_id = 1u;

  if (kbc_failed(config_clone(&app->cfg, cfg, err))) {
    goto fail;
  }

  kbc_str vp;
  kbc_str_init(&vp);
  kbc_status s = kbc_str_printf(&vp, "%s/%s", app->cfg->data_dir,
                                KBC_APP_VEC_FILE);
  if (kbc_failed(s) || (app->vec_path = dup_cstr(vp.ptr)) == NULL) {
    kbc_str_free(&vp);
    (void)kbc_err_set(err, KBC_ERR_NOMEM, "vector store path under %s",
                      app->cfg->data_dir);
    goto fail;
  }
  kbc_str_free(&vp);

  if (kbc_failed(kbc_mkdir_p(app->cfg->data_dir, err))) {
    goto fail;
  }

  app->store = kbc_store_open(app->cfg, err);
  if (!app->store) {
    goto fail;
  }

  /* The config DECLARES corpora; this is where they become LIVE, indexed
   * roots, and it is the only writer of the `sources` table. Two facts make
   * this the natural home rather than the first reindex:
   *
   *   - `index_runs.corpus` REFERENCES sources(slug) with foreign_keys = ON,
   *     so a pass cannot open a run row for a corpus the registry does not
   *     know. Registering at open is what makes the run ledger possible at
   *     all, and registering at reindex would make the first pass of a fresh
   *     database fail its own bookkeeping.
   *   - a corpus is live from the moment the daemon answers, not from the
   *     moment someone reindexes. A daemon that has opened a corpus and been
   *     asked to list its sources must be able to.
   *
   * ONCE per open, and put_source is an upsert that keeps `added_at` (the
   * first time the source was seen) and does NOT touch `paused` — so a corpus
   * an operator paused stays paused across every restart. That is the whole
   * reason the column is on this row and not in the config file: pausing is
   * runtime operator intent, durable in the database, and re-declaring the
   * corpus must not silently un-pause it.
   *
   * A corpus that cannot be registered is a WARNING, not a failed open: the
   * two ways it happens are a store that cannot be written at all (which the
   * first reindex reports far better than this can) and a config that aliases
   * two corpora onto one directory, which `sources.path` being UNIQUE refuses
   * and which is a real, supported configuration — refusing to start would
   * take down a daemon that indexes both corpora perfectly well. */
  for (size_t i = 0; i < app->cfg->ncorpora; i++) {
    kbc_source src;
    memset(&src, 0, sizeof src);
    src.corpus = app->cfg->corpora[i].name;
    src.path = app->cfg->corpora[i].path;
    src.added_at = kbc_now_ns() / 1000000000LL;
    src.paused = false;
    kbc_err local;
    kbc_err_reset(&local);
    if (kbc_failed(kbc_store_put_source(app->store, &src, &local))) {
      KBC_LOGW("corpus %s not registered as a source: %s", src.corpus,
               local.msg);
    }
  }

  /* A missing index is normal — first start, or before the first reindex. A
   * corrupt one is not: there are bytes on disk we cannot read, and quietly
   * starting empty would make "never indexed" and "written by another build"
   * indistinguishable to the user. index.h already returns KBC_ERR_PARSE with
   * the reason; pass it through. */
  if (kbc_path_exists(app->cfg->index_path)) {
    kbc_err local;
    kbc_err_reset(&local);
    app->index = kbc_index_open(app->cfg->index_path, &local);
    if (!app->index) {
      (void)kbc_err_set(err, local.status ? local.status : KBC_ERR_PARSE,
                        "open index %s: %s", app->cfg->index_path,
                        local.msg[0] ? local.msg : "unreadable");
      goto fail;
    }
  } else {
    app->index = empty_index(err);
    if (!app->index) {
      goto fail;
    }
  }

  if (kbc_path_exists(app->vec_path)) {
    kbc_err local;
    kbc_err_reset(&local);
    app->vec = kbc_vecstore_open(app->vec_path, &local);
    if (!app->vec) {
      KBC_LOGW("vector store %s unusable (%s), vector lane off", app->vec_path,
               local.msg);
    }
  }

  /* The query cache, created BEFORE the sidecar on purpose: a sidecar that
   * fails to start then unwinds through kbc_app_close with a live cache to
   * free, so the error path is the same code the normal path is.
   *
   * A NULL cache is survivable, not fatal, because kbc_embed_query defines it
   * as "embed unconditionally" and the query lane then runs embed_one — this
   * file's behaviour before the cache existed. Losing the cache costs the
   * 50-100 ms a repeat query used to pay; failing the open would cost the
   * daemon. */
  app->qcache = kbc_query_cache_new(APP_QUERY_CACHE_CAPACITY, NULL);
  if (app->qcache == NULL) {
    KBC_LOGW("query cache of %u entries could not be allocated, query "
             "embeddings will not be cached",
             (unsigned)APP_QUERY_CACHE_CAPACITY);
  }

  if (app->cfg->embedder_cmd && app->cfg->embedder_cmd[0] != '\0') {
    const size_t clen = strlen(app->cfg->embedder_cmd);
    char *cbuf = malloc(clen + 1);
    if (cbuf == NULL) {
      (void)kbc_err_set(err, KBC_ERR_NOMEM, "embedder command buffer");
      goto fail;
    }
    memcpy(cbuf, app->cfg->embedder_cmd, clen + 1);
    const char *argv[64];
    kbc_status cst =
        split_command(cbuf, argv, sizeof argv / sizeof argv[0], err);
    app->embed = kbc_failed(cst) ? NULL : kbc_embedder_start(argv, err);
    free(cbuf);
    if (!app->embed) {
      goto fail;
    }
  }

  const int64_t docs = (int64_t)kbc_index_doc_count(app->index);
  atomic_store_explicit(&app->st_indexed, docs, memory_order_relaxed);
  atomic_store_explicit(&app->st_docs, docs, memory_order_relaxed);
  atomic_store_explicit(&app->st_terms,
                        (int64_t)kbc_index_term_count(app->index),
                        memory_order_relaxed);
  return app;

fail:
  kbc_app_close(app);
  return NULL;
}

void kbc_app_close(kbc_app *app) {
  if (!app) {
    return;
  }
  /* The watcher thread publishes onto the bus and calls back into the store;
   * it must be joined before either goes away. */
  kbc_app_stop_watcher(app);
  if (app->embed) {
    kbc_embedder_stop(app->embed);
    app->embed = NULL;
  }
  if (app->qcache) {
    /* Freeed after the last search has returned, never during one: the httpd
     * joins its workers before the app closes, and an entry is malloc'd per
     * key, so this is the only place any of them is released. */
    kbc_query_cache_free(app->qcache);
    app->qcache = NULL;
  }
  if (app->vec) {
    kbc_vecstore_free(app->vec);
  }
  if (app->index) {
    kbc_index_free(app->index);
  }
  if (app->store) {
    kbc_store_close(app->store);
  }
  free(app->vec_path);
  kbc_config_free(app->cfg);
  pthread_mutex_destroy(&app->bus_lock);
  pthread_rwlock_destroy(&app->lock);
  pthread_mutex_destroy(&app->reindex_mu);
  free(app);
}

const kbc_config *kbc_app_config(const kbc_app *app) {
  return app ? app->cfg : NULL;
}

/* --------------------------------------------------------------- reindex */

kbc_status kbc_app_reindex(kbc_app *app, kbc_err *err) {
  if (!app) {
    return kbc_err_set(err, KBC_ERR_INVALID, "kbc_app_reindex: app is NULL");
  }
  if (app->cfg->ncorpora == 0) {
    return kbc_err_set(err, KBC_ERR_INVALID, "no corpora configured");
  }
  kbc_status s = reindex_locked(app, err);
  if (kbc_failed(s)) {
    return s;
  }

  const int64_t docs =
      atomic_load_explicit(&app->st_docs, memory_order_relaxed);
  kbc_str payload;
  kbc_str_init(&payload);
  if (!kbc_failed(kbc_str_printf(&payload, "{\"docs\":%" PRId64 "}", docs))) {
    kbc_app_publish(app, "index.updated", payload.ptr);
  }
  kbc_str_free(&payload);
  return KBC_OK;
}

/* The removal, announced. The Rust daemon emits `watch.delete` with a payload
 * of exactly {kb, path} from BOTH the watcher and its reconcile pass
 * (indexer.rs:795, routes/schema.rs:284), and kb-c emitted nothing at all: a
 * document deleted while the daemon was not watching changed the index under
 * every subscriber — an SSE client, `kb watch` — with no word about what
 * changed in it, so a client had to diff the whole document set to notice.
 *
 * Both values are escaped: a corpus name comes from a config file and a path
 * comes off the filesystem, where `a"b.md` is a perfectly legal file name and
 * a raw format would emit invalid JSON on a name the user chose. */
static void publish_document_gone(kbc_app *app, const char *corpus,
                                  const char *rel_path) {
  kbc_str payload;
  kbc_str_init(&payload);
  kbc_status s = kbc_str_puts(&payload, "{\"kb\":");
  if (s == KBC_OK) s = kbc_str_append_json_string(&payload, corpus, strlen(corpus));
  if (s == KBC_OK) s = kbc_str_puts(&payload, ",\"path\":");
  if (s == KBC_OK) {
    s = kbc_str_append_json_string(&payload, rel_path, strlen(rel_path));
  }
  if (s == KBC_OK) s = kbc_str_puts(&payload, "}");
  if (s == KBC_OK) {
    kbc_app_publish(app, "watch.delete", payload.ptr);
  } else {
    /* Observability, not correctness: a payload that would not build must not
     * fail a removal the store already committed. The removal itself is
     * logged, loudly, by its caller. */
    KBC_LOGW("watch.delete for %s/%s: no payload (%s)", corpus, rel_path,
             kbc_status_str(s));
  }
  kbc_str_free(&payload);
}

/* THE removal, and the only one: the watcher's delete event, the reconcile
 * sweep's and kbc_app_delete_path's all arrive here. A path that is not there
 * is not a failure — the caller is bringing the index in line with the
 * filesystem, or the user asked for the document to be gone, and either way
 * the document is not coming back under that id.
 *
 * The cascade, in full, because this is the site that has to say it:
 *
 *   GOES, with the document —
 *     edges (outbound deleted, inbound demoted to pending_links)   store.c
 *     doc_metas (a facet is a name the document wrote into itself)  store.c
 *     artifacts                                                   store.c
 *     chunks and comments, which the artifacts row cascades to     store.c
 *
 *   STAYS, deliberately —
 *     history, corkboard, pinned_memories, reading_sections.
 *
 * The last four are the user's, not the document's. A comment is a reply to
 * something the document said; reading history is the fact that the user read
 * it, a pin is a decision they made about it, and a corkboard entry is where
 * they put it. None of them can be reconstructed from the bytes, and none of
 * them become false because the bytes are gone — the user read it, and they
 * still have. So a removal must not take a user's reading history with it:
 * the day the document comes back, its history is still theirs. That is
 * invariant 8 of the port plan, and the reason this cascade stops where it
 * stops. It is also why the reconcile sweep refuses to run it for a file
 * that is merely unwalked — see sweep_one_id. */
static kbc_status store_forget_path(kbc_app *app, const char *corpus,
                                    const char *rel_path, kbc_err *err) {
  kbc_arena *qa = kbc_arena_new(4096u);
  if (qa == NULL) {
    return kbc_err_set(err, KBC_ERR_NOMEM, "arena for %s/%s", corpus, rel_path);
  }
  kbc_artifact prev;
  memset(&prev, 0, sizeof prev);
  kbc_status s = KBC_OK;
  kbc_err local;
  kbc_err_reset(&local);
  if (kbc_failed(kbc_store_get_artifact_by_path(app->store, qa, corpus, rel_path,
                                                &prev, &local))) {
    kbc_arena_free(qa);
    /* Not a row, but the graph rows it left are still ours to drop: a
     * document that was re-ingested without its store row is not a
     * document. */
    s = kbc_store_forget_document(app->store, corpus, rel_path, err);
    if (s == KBC_OK) {
      publish_document_gone(app, corpus, rel_path);
    }
    return s;
  }
  /* The graph rows go with the document, outbound AND inbound, and the
   * inbound edges are demoted to pending links rather than dropped: the
   * links other documents wrote are still true, and this one may come back.
   * Pending links pointing at it are left alone for the same reason. */
  s = kbc_store_forget_document(app->store, corpus, rel_path, err);
  if (kbc_failed(s)) {
    kbc_arena_free(qa);
    return s;
  }
  s = kbc_store_delete_artifact(app->store, prev.id, err);
  kbc_arena_free(qa);
  if (s == KBC_OK) {
    publish_document_gone(app, corpus, rel_path);
  }
  return s;
}

/* The one place that frees a link batch. `targets` and its entries are
 * KBC_OWN, transferred here from the parse, and BOTH callers abort part-way
 * through a pass — reindex_pass on an unreadable document or a rejected
 * facet, reindex_one on a store write. A free loop at the call site is a
 * loop some abort path skips, and both of these did: a full rebuild that
 * died on its Nth document leaked every link array the first N-1 had handed
 * over.
 *
 * The array is read back as the `char **` its allocator produced, which is
 * what free() takes. app.h types it `char *const *` so a HOOK cannot replace a
 * target, and freeing is the one place that has to be the owner instead: a
 * cast says it and -Wcast-qual forbids exactly that, so the two views sit in a
 * union rather than in a cast. */
static void app_edge_srcs_free(kbc_enrich_source *v, size_t n) {
  for (size_t i = 0; i < n; i++) {
    for (size_t j = 0; j < v[i].n_targets; j++) {
      free(v[i].targets[j]);
    }
    union {
      char *const *as_a_hook_sees_it;
      char **as_it_was_allocated;
    } owner;
    owner.as_a_hook_sees_it = v[i].targets;
    free(owner.as_it_was_allocated);
    v[i].targets = NULL;
    v[i].n_targets = 0;
  }
  free(v);
}

/* --------------------------------------------------------- enrich hooks --- */

/* The contract is app.h's, and it is stated there once: best-effort, cheap
 * pre-filter, registration order IS run order, a batch rather than a
 * document, `candidates` is everything the ladder may name. What belongs
 * HERE is the implementation and the one thing app.h cannot say — which of
 * the original's eight are registered, and which are absent.
 *
 * ONE OF THE EIGHT IS HERE. The other seven are absent because the FEATURE
 * each one serves is not in kb-c's scope (INVENTORY.md), not because they
 * were hard — and a registration whose feature does not exist is a hook that
 * cannot fire, which is worse than no table at all: it reads like coverage.
 *
 *   session-capture       no sessions corpus, no transcript parsing.
 *   memory-recall-ledger  no memory-recall ledger: no table, no writer.
 *   memory-commit-ledger  no memory-commit ledger: no table, no writer.
 *   memory-link-seed      no memory-link seed metas, no cross-kb linking.
 *   code-refs             no code-reference extraction in the parser.
 *   snapshot-capture      no artifact snapshots: no versions table, no
 *                         content-hash snapshot store.
 *   list-anchor           no list entries, no reading lists.
 *
 * Adding one is a row in enrich_registry and an enrich fn; the seven lines
 * above are what tells the next porter which rows are missing ON PURPOSE. */

/* One row of the registry. `name` is the failure log's only use of a hook —
 * the original's stable identifier (enrich.rs:103-105) — and `user` is the
 * hook's own state, NULL for a hook that has none. The table is private
 * because the hook LAYOUT is not part of the contract: kbc_enrich_run takes
 * parallel arrays so a caller never has to mirror this struct. */
typedef struct {
  const char *name;
  kbc_enrich_prefilter_fn interested;
  kbc_enrich_fn enrich;
  void *user;
} kbc_enrich_hook;


/* The ladder answers with ids; the edges table is keyed by PATH. This is the
 * one lookup that turns an answer into a row, built once per batch over the
 * same candidate array the ladder was built from, and sorted so that it is a
 * bsearch: a hub document with four thousand links in a corpus of twenty
 * thousand documents is eighty million comparisons otherwise. */
typedef struct {
  const char *id; /* BORROWED, from the candidate array */
  size_t cand;    /* index into that array */
} app_id_ref;

/* Orders by the id ALONE, and that is not a simplification: this is also the
 * comparator the lookup runs under, and a tiebreak on `cand` would make a
 * probe (whose cand is 0) compare unequal to the very element it names, so
 * the search would walk past it and report a miss. Ids are unique per
 * candidate — they are minted from (corpus, path) — so the order is total
 * anyway. */
static int app_id_ref_cmp(const void *a, const void *b) {
  const app_id_ref *x = (const app_id_ref *)a;
  const app_id_ref *y = (const app_id_ref *)b;
  return strcmp(x->id, y->id);
}

/* NULL when the id names no candidate, which a resolution out of THIS index
 * cannot produce and is checked anyway: an edge row pointing at a document
 * nobody can name is a dangling edge. */
static const char *edge_path_of_id(const kbc_resolve_doc *cand,
                                   const app_id_ref *order, size_t n,
                                   const char *id) {
  app_id_ref probe = {id, 0};
  const app_id_ref *hit =
      bsearch(&probe, order, n, sizeof(*order), app_id_ref_cmp);
  return hit != NULL ? cand[hit->cand].rel_path : NULL;
}

/* edge-record — the one hook kb-c registers, and the index-time consumer of
 * the resolution ladder. */
static bool edge_record_interested(const kbc_enrich_ctx *ctx, void *user) {
  (void)user;
  /* A batch of no documents has nothing to enrich, which is the only thing
   * this hook can be unready for. It is NOT "a document with no links": that
   * document still has to reach the write that deletes the edges it used to
   * have, so a prefilter that skipped it would leave them behind forever. */
  return ctx->n_sources > 0;
}

static kbc_status edge_record_enrich(const kbc_enrich_ctx *ctx, void *user,
                                     kbc_err *err) {
  (void)user;
  const kbc_enrich_source *v = ctx->sources;
  const size_t nsrc = ctx->n_sources;
  size_t total = 0;
  for (size_t i = 0; i < nsrc; i++) {
    total += v[i].n_targets;
  }

  /* total == 0 is NOT a no-op: every source in this batch has lost its last
   * link, and the write that deletes those edges is exactly the point. Only
   * the batch RESOLVE needs targets, so that is what is skipped. The pending
   * delete is here for the same reason: a document that has stopped naming a
   * target must stop being the reason to look for it. */
  if (total == 0) {
    for (size_t i = 0; i < nsrc; i++) {
      kbc_status s = kbc_store_replace_edges(ctx->store, v[i].corpus, v[i].src,
                                             NULL, 0, err);
      if (kbc_failed(s)) return s;
      s = kbc_store_delete_pending(ctx->store, v[i].corpus, v[i].src, err);
      if (kbc_failed(s)) return s;
    }
    KBC_LOGI("graph: %zu documents, 0 of 0 link targets recorded", nsrc);
    return KBC_OK;
  }

  kbc_err local;
  kbc_err_reset(&local);
  kbc_resolve_index *ix =
      kbc_resolve_index_new(ctx->candidates, ctx->n_candidates, &local);
  if (ix == NULL) {
    return kbc_err_set(err, KBC_ERR_NOMEM,
                       "resolution ladder over %zu documents of %s: %s",
                       ctx->n_candidates, ctx->corpus,
                       local.msg[0] ? local.msg : "out of memory");
  }
  app_id_ref *order = malloc((ctx->n_candidates + 1u) * sizeof(*order));
  if (order == NULL) {
    kbc_resolve_index_free(ix);
    return kbc_err_set(err, KBC_ERR_NOMEM, "id order for %zu documents",
                       ctx->n_candidates);
  }
  for (size_t i = 0; i < ctx->n_candidates; i++) {
    order[i].id = ctx->candidates[i].id;
    order[i].cand = i;
  }
  qsort(order, ctx->n_candidates, sizeof(*order), app_id_ref_cmp);

  /* `sa` is the ladder's SCRATCH: the normalised target and its lowercased
   * form are dead by the next call (links.h), so one arena serves the whole
   * batch and each resolution's ids are freed where they were read. */
  kbc_arena *sa = kbc_arena_new(8192u);
  const char **kept = calloc(total, sizeof(*kept));
  /* One pending slot per raw target, filled in the same pass: a target that
   * resolved to nothing is not an edge, but it IS a link the corpus states,
   * and dropping it is what made the graph depend on visit order. */
  const char **pending = calloc(total, sizeof(*pending));
  kbc_status st = KBC_OK;
  if (sa == NULL || kept == NULL || pending == NULL) {
    st = kbc_err_set(err, KBC_ERR_NOMEM, "ladder scratch for %zu links of %s",
                     total, ctx->corpus);
  }
  size_t recorded = 0;
  size_t n_pending = 0;
  size_t ambiguous = 0;
  size_t self_links = 0;
  for (size_t i = 0; st == KBC_OK && i < nsrc; i++) {
    size_t nk = 0;
    size_t np = 0;
    for (size_t j = 0; st == KBC_OK && j < v[i].n_targets; j++) {
      kbc_resolution r;
      kbc_err rl;
      kbc_err_reset(&rl);
      kbc_status rs =
          kbc_resolve_index_resolve(ix, sa, v[i].targets[j], &r, &rl);
      if (kbc_failed(rs)) {
        st = kbc_err_set(err, rs, "%s/%s: resolve link \"%s\": %s",
                         ctx->corpus, v[i].src, v[i].targets[j],
                         rl.msg[0] ? rl.msg : "ladder failed");
        break;
      }
      /* ONLY a unique answer is an edge (enrich.rs:1095). A bare basename
       * shared by ops/deploy.md and infra/deploy.md is AMBIGUOUS, and taking
       * one of them would be an edge that flips on the next reindex; what a
       * route offers for an ambiguous target is the candidates, not a
       * winner. A None is a link the corpus states that no document answers
       * to, which is a pending link rather than a silent loss. */
      if (r.kind == KBC_RESOLVE_ONE) {
        /* The resolved path is the document's own, not the link's spelling:
         * `../x/y.md` and `x/y.md` are one edge, and the primary key says
         * so. So is a target named twice by one source, which is where the
         * original's `seen` set has no counterpart here. */
        const char *path =
            edge_path_of_id(ctx->candidates, order, ctx->n_candidates,
                            r.ids.items[0]);
        if (path == NULL) {
          ambiguous++; /* unreachable from this index; counted, not written */
        } else if (strcmp(path, v[i].src) == 0) {
          self_links++; /* a document naming itself is not a backlink */
        } else {
          kept[nk++] = path;
        }
      } else if (r.kind == KBC_RESOLVE_AMBIGUOUS) {
        ambiguous++;
      } else {
        pending[np++] = v[i].targets[j];
      }
      kbc_strlist_free(&r.ids);
    }
    if (st == KBC_OK) {
      st = kbc_store_replace_edges(ctx->store, v[i].corpus, v[i].src, kept, nk,
                                   err);
      if (st == KBC_OK)
        st = kbc_store_delete_pending(ctx->store, v[i].corpus, v[i].src, err);
      if (st == KBC_OK && np > 0)
        st = kbc_store_add_pending_links(ctx->store, v[i].corpus, v[i].src,
                                         pending, np, err);
      recorded += nk;
      n_pending += np;
    }
  }
  if (sa != NULL) {
    kbc_arena_free(sa);
  }
  free(kept);
  free(pending);
  free(order);
  kbc_resolve_index_free(ix);
  if (st == KBC_OK) {
    /* Five numbers, and they are not the same number: the parser found
     * `total` link targets, `recorded` of them resolved to exactly one
     * indexed document, `n_pending` name a target that is not one yet and
     * wait for it, `ambiguous` were answered by more than one document and
     * are deliberately not edges, `self_links` are this document's own name
     * — and the graph holds `edges` rows, fewer than `recorded` because a
     * source naming one document twice has one edge. Only the last is the
     * table itself. */
    const int64_t edges = kbc_store_edge_count(ctx->store, v[0].corpus, NULL);
    KBC_LOGI("graph: %zu documents, %zu of %zu link targets resolved, %zu "
             "awaiting their target, %zu ambiguous, %zu self, %lld edges in "
             "the graph",
             nsrc, recorded, total, n_pending, ambiguous, self_links,
             (long long)edges);
  }
  return st;
}

/* The default registry, in RUN ORDER. Registration order IS run order
 * (enrich.rs:118-133), so a row's position is a decision and not an accident.
 * edge-record is the original's fifth hook, the only one whose feature is in
 * kb-c's scope, and it keeps that slot. */
static const kbc_enrich_hook *enrich_registry(size_t *n_out) {
  static const kbc_enrich_hook hooks[] = {
      {"edge-record", edge_record_interested, edge_record_enrich, NULL},
  };
  *n_out = sizeof(hooks) / sizeof(hooks[0]);
  return hooks;
}

/* The run. It has no failure to return, and that IS the contract rather than
 * an omission: a hook that fails is logged by name and stepped over, and the
 * hooks after it still run (enrich.rs:14-24, indexer.rs:2300-2308). */
static void enrich_run(const kbc_enrich_hook *hooks, size_t n,
                       const kbc_enrich_ctx *ctx) {
  for (size_t i = 0; i < n; i++) {
    if (!hooks[i].interested(ctx, hooks[i].user)) {
      continue;
    }
    kbc_err local;
    kbc_err_reset(&local);
    kbc_status s = hooks[i].enrich(ctx, hooks[i].user, &local);
    if (kbc_failed(s)) {
      KBC_LOGW("enrich: hook %s failed and was skipped: %s", hooks[i].name,
               local.msg[0] ? local.msg : "no message");
    }
  }
}

/* The registry's other entry point, and the seam a caller drives its OWN hooks
 * through. It is the same loop over the same table as the daemon's own run —
 * app.h says why the arguments are parallel arrays, and the reason it is here
 * at all is the pre-filter and the failure: neither is observable from a
 * registry of one hook that never says no. */
kbc_status kbc_enrich_run(size_t n, const char *const *names,
                          kbc_enrich_prefilter_fn pre[], kbc_enrich_fn fns[],
                          void *const *users, const kbc_enrich_ctx *ctx,
                          kbc_err *err) {
  if (ctx == NULL || (n > 0 && (names == NULL || pre == NULL ||
                                 fns == NULL))) {
    return kbc_err_set(err, KBC_ERR_INVALID,
                       "enrich_run: %zu hooks with a NULL table or context",
                       n);
  }
  if (n > KBC_ENRICH_MAX_HOOKS) {
    return kbc_err_set(err, KBC_ERR_INVALID,
                       "enrich_run: %zu hooks is over the %u cap", n,
                       KBC_ENRICH_MAX_HOOKS);
  }
  kbc_enrich_hook table[KBC_ENRICH_MAX_HOOKS];
  for (size_t i = 0; i < n; i++) {
    if (pre[i] == NULL || fns[i] == NULL) {
      return kbc_err_set(err, KBC_ERR_INVALID, "enrich_run: hook %zu has no "
                                               "function",
                         i);
    }
    table[i].name = names[i] != NULL ? names[i] : "?";
    table[i].interested = pre[i];
    table[i].enrich = fns[i];
    table[i].user = users != NULL ? users[i] : NULL;
  }
  enrich_run(table, n, ctx);
  return KBC_OK;
}

/* Appends one candidate, or hands back the slot it took so that a caller
 * REPLACING an entry can copy into the position the old one held: the
 * ladder's first-wins rule (links.h) makes a document's place in the array
 * part of what it resolves to. */
static kbc_resolve_doc *ladder_push(kbc_resolve_doc **v, size_t *n, size_t *cap,
                                    kbc_arena *a, const char *id,
                                    const char *path, const char *title,
                                    kbc_err *err) {
  if (*n == *cap) {
    size_t want = *cap != 0 ? *cap * 2u : 64u;
    if (want < *cap || want > SIZE_MAX / sizeof(**v)) {
      (void)kbc_err_set(err, KBC_ERR_NOMEM, "candidate set would exceed %zu",
                        SIZE_MAX / sizeof(**v));
      return NULL;
    }
    kbc_resolve_doc *grown =
        (kbc_resolve_doc *)realloc(*v, want * sizeof(*grown));
    if (grown == NULL) {
      (void)kbc_err_set(err, KBC_ERR_NOMEM, "realloc for %zu candidates",
                        want);
      return NULL;
    }
    *v = grown;
    *cap = want;
  }
  char *cid = kbc_arena_strdup(a, id);
  char *cpath = kbc_arena_strdup(a, path);
  char *ctitle = kbc_arena_strdup(a, title);
  if (cid == NULL || cpath == NULL || ctitle == NULL) {
    (void)kbc_err_set(err, KBC_ERR_NOMEM, "candidate copy for %s", path);
    return NULL;
  }
  (*v)[*n].id = cid;
  (*v)[*n].rel_path = cpath;
  (*v)[*n].title = ctitle;
  return &(*v)[(*n)++];
}

/* Every document in the corpus, as the ladder's three fields. Which array it
 * is read from depends on who is asking, and both name the same set of
 * documents by app.h's invariant:
 *
 *   - The full pass has the WALK's manifest: every file the corpus contains,
 *     in walk order, under the id and title it is being stored with. It
 *     costs no store read at all, which is why a rebuild does not pay a
 *     corpus-sized query to keep the graph honest.
 *   - The single-file path has only the live INDEX, which app.h keeps
 *     replaced together with the store, plus the document it has just
 *     ingested — the index has not been told about that one yet, and its
 *     title is the fresh one. A target the index cannot see resolves to
 *     nothing and is recorded as a PENDING link, so an edge missed because
 *     the index was a generation behind heals on the next full pass instead
 *     of never existing.
 */
static kbc_status ladder_candidates(kbc_app *app, const char *corpus,
                                    const app_ladder *lad, kbc_arena *a,
                                    kbc_resolve_doc **out, size_t *n_out,
                                    kbc_err *err) {
  kbc_resolve_doc *v = NULL;
  size_t n = 0;
  size_t cap = 0;
  kbc_status s = KBC_OK;

  if (lad->manifest != NULL) {
    const kbc_app_manifest *m = lad->manifest;
    for (size_t ci = 0; s == KBC_OK && ci < app->cfg->ncorpora; ci++) {
      if (strcmp(app->cfg->corpora[ci].name, corpus) != 0) {
        continue;
      }
      for (size_t i = 0; s == KBC_OK && i < m->len; i++) {
        const kbc_app_row *row = &m->items[i];
        if (row->corpus_index != ci) {
          continue;
        }
        s = ladder_push(&v, &n, &cap, a, row->id, row->path, row->title, err)
                == NULL
                ? KBC_ERR_NOMEM
                : KBC_OK;
      }
    }
  } else {
    /* The index is read under the lock and copied into `a` before the lock is
     * dropped, which is the whole reason this is cheap: no store read, and no
     * per-document query to fetch a path and a title the index already
     * holds. app->lock is taken under reindex_mu and only there (the locking
     * note at the top of this file), and nothing else is held here, so this
     * cannot invert that order. */
    pthread_rwlock_rdlock(&app->lock);
    const kbc_index *ix = app->index;
    const uint32_t count = ix != NULL ? kbc_index_doc_count(ix) : 0u;
    for (uint32_t d = 0; s == KBC_OK && d < count; d++) {
      const kbc_doc_meta *meta = kbc_index_doc(ix, d);
      if (meta == NULL || meta->corpus == NULL || meta->path == NULL ||
          strcmp(meta->corpus, corpus) != 0 ||
          (kbc_kind)meta->kind != KBC_KIND_ARTIFACT) {
        continue;
      }
      char id[KBC_MAX_ID_LEN + 1];
      /* The index's own doc id is a dense handle, not the artifact id its
       * first tier keys on; types.h mints the artifact id from (corpus,
       * path), which is the same value for the same document. */
      kbc_id_for_artifact(id, corpus, meta->path);
      s = ladder_push(&v, &n, &cap, a, id, meta->path,
                      meta->title != NULL ? meta->title : "", err) == NULL
              ? KBC_ERR_NOMEM
              : KBC_OK;
    }
    pthread_rwlock_unlock(&app->lock);

    /* The document this call ingested, refreshed where the index already
     * knows it: the previous generation's title would answer a link that the
     * new title answers differently. The position is kept, because the
     * ladder's first-wins rule is positional. */
    if (s == KBC_OK && lad->self_path != NULL) {
      size_t at = 0;
      bool seen = false;
      for (size_t i = 0; i < n; i++) {
        if (strcmp(v[i].rel_path, lad->self_path) == 0) {
          at = i;
          seen = true;
          break;
        }
      }
      kbc_resolve_doc *slot =
          ladder_push(&v, &n, &cap, a, lad->self_id, lad->self_path,
                      lad->self_title != NULL ? lad->self_title : "", err);
      if (slot == NULL) {
        s = KBC_ERR_NOMEM;
      } else if (seen) {
        v[at] = *slot;
        n--;
      }
    }
  }
  if (kbc_failed(s)) {
    free(v);
    return s;
  }
  *out = v;
  *n_out = n;
  return KBC_OK;
}

/* The link graph, kept in step with the documents.
 *
 * Every re-ingest REPLACES the edges leaving that path, and a removal drops
 * them, so a file whose links changed cannot leave the previous edges behind.
 * A target that is not an indexed document is not an edge at all (store.h:
 * the table is a subset of what the parser found, never a superset), which
 * is why this runs after the whole pass has stored its documents: whether a
 * target is a document is not knowable while the pass is still half done, and
 * an order-dependent graph is a graph that flips on the next reindex.
 *
 * It is a DRIVER and not a second edge path: it groups the batch by corpus,
 * builds the candidate set, and hands both to the registry. The write is the
 * edge-record hook's and lives in it, so there is exactly one place in this
 * file that turns a link into an edge.
 *
 * The grouping is by corpus because a corpus is the ladder's whole world —
 * the original builds one ResolveIndex per kb over that kb's documents — so
 * a link must not resolve into another corpus that happens to share a
 * basename. reindex_pass appends its sources in corpus order already; this
 * does not rely on that, and a corpus split across two runs simply has its
 * index built twice. */
static kbc_status store_write_links(kbc_app *app, kbc_enrich_source *v,
                                    size_t nsrc, const app_ladder *lad,
                                    kbc_err *err) {
  size_t start = 0;
  while (start < nsrc) {
    const char *corpus = v[start].corpus;
    size_t end = start + 1u;
    while (end < nsrc && strcmp(v[end].corpus, corpus) == 0) {
      end++;
    }
    kbc_arena *a = kbc_arena_new(64u * 1024u);
    if (a == NULL) {
      return kbc_err_set(err, KBC_ERR_NOMEM, "ladder arena for %s", corpus);
    }
    kbc_resolve_doc *docs = NULL;
    size_t n_docs = 0;
    kbc_status s = ladder_candidates(app, corpus, lad, a, &docs, &n_docs, err);
    if (!kbc_failed(s)) {
      kbc_enrich_ctx ctx = {app->store, corpus, &v[start], end - start, docs,
                            n_docs};
      size_t n_hooks = 0;
      const kbc_enrich_hook *hooks = enrich_registry(&n_hooks);
      enrich_run(hooks, n_hooks, &ctx);
    }
    /* The array is the caller's; the strings in it are the arena's, and the
     * hook is finished with both. */
    free(docs);
    kbc_arena_free(a);
    if (kbc_failed(s)) {
      return s;
    }
    start = end;
  }
  return KBC_OK;
}

/* One document, in place: read it, tokenize it, rewrite only its postings.
 *
 * The mutation runs under the write lock — a reader must never see a
 * half-rewritten postings array — and the result is published before the lock
 * is released, so a crash mid-update leaves the previous generation on disk
 * and the daemon comes back up on it. The store row is committed before any of
 * this (reindex_one), so app.h's invariant holds throughout: the index never
 * references a document the store has not committed.
 *
 * `vanished` means the file is not on disk, which is a removal rather than an
 * update.
 *
 * `rel_path` is CORPUS-RELATIVE — the same shape the watcher publishes and the
 * store and index are keyed on. The corpus root is joined once, into `full`,
 * and never prepended to the key.
 */

/* The mutation window: the WRITE lock only.
 *
 * reindex_mu is deliberately NOT taken here. It is a plain, non-recursive
 * mutex and reindex_one — the only caller — already holds it across the store
 * writes that precede this, so taking it again would self-deadlock. Holding it
 * at the reindex_one call site instead is what excludes this path from a full
 * rebuild on BOTH halves: the store rows and the index. */
static void index_lock(kbc_app *app) { pthread_rwlock_wrlock(&app->lock); }

static void index_unlock(kbc_app *app) { pthread_rwlock_unlock(&app->lock); }

static kbc_status index_touch_one(kbc_app *app, const kbc_corpus_cfg *cc,
                                  const char *rel_path, bool vanished,
                                  kbc_err *err) {
  const int64_t t0 = kbc_now_ns();
  kbc_arena *fa = kbc_arena_new(64u * 1024u);
  if (fa == NULL) {
    return kbc_err_set(err, KBC_ERR_NOMEM, "arena for %s/%s", cc->name,
                       rel_path);
  }
  kbc_str full, text, raw;
  kbc_str_init(&full);
  kbc_str_init(&text);
  kbc_str_init(&raw);
  kbc_tokens toks;
  memset(&toks, 0, sizeof toks);
  const char *title = "";
  float *v = NULL;
  size_t dim = 0;
  kbc_status s;
  uint32_t doc_id, target = UINT32_MAX, docs_before = 0;
  bool dropped_vec = false;

  s = kbc_str_printf(&full, "%s/%s", cc->path, rel_path);
  if (kbc_failed(s)) {
    s = kbc_err_set(err, KBC_ERR_NOMEM, "path buffer for %s/%s", cc->name,
                    rel_path);
    goto out;
  }
  if (!vanished) {
    s = read_bounded(full.ptr, &raw, err);
    if (kbc_failed(s)) {
      goto out;
    }
    kbc_parsed *p = kbc_parse(fa, raw.ptr, raw.len, rel_path, err);
    if (p == NULL) {
      s = kbc_err_set(err, KBC_ERR_PARSE, "parse %s: malformed document",
                      full.ptr);
      goto out;
    }
    const char *parsed_title = kbc_parsed_title(p);
    if (parsed_title != NULL) {
      title = parsed_title;
    }
    s = searchable_text(kbc_parsed_blocks(p), title, &text, err);
    if (kbc_failed(s)) {
      goto out;
    }
    s = kbc_tokenize(fa, text.ptr, text.len, &toks, err);
    if (kbc_failed(s)) {
      goto out;
    }
    /* Embedded before the write lock, because the embedder is a subprocess
     * round trip and the lock is the one thing searches queue behind. */
    v = embed_document(app, fa, cc->name, rel_path, raw.ptr, raw.len, text.ptr,
                       &dim);
  }

  index_lock(app);
  doc_id = kbc_index_id_of(app->index, cc->name, rel_path);
  docs_before = kbc_index_doc_count(app->index);
  if (vanished) {
    if (doc_id != UINT32_MAX) {
      s = kbc_index_remove_doc(app->index, doc_id, err);
      if (kbc_failed(s)) {
        index_unlock(app);
        goto out;
      }
      /* A removal renumbers every document above it, so the vector rows keyed
       * on those ids now describe the wrong documents. The store has no
       * per-row delete, and a vector lane that returns another document's
       * neighbours is worse than no vector lane: drop it, loudly, and let the
       * next full reindex rebuild it. Removing the LAST document renumbers
       * nothing, so that case keeps its lane. */
      if (app->vec != NULL && doc_id + 1u < docs_before) {
        kbc_vecstore_free(app->vec);
        app->vec = NULL;
        dropped_vec = true;
      }
    }
  } else {
    target = (doc_id == UINT32_MAX) ? docs_before : doc_id;
    s = kbc_index_update_doc(app->index, doc_id, cc->name, rel_path, title,
                             KBC_KIND_ARTIFACT, &toks, err);
    if (kbc_failed(s)) {
      index_unlock(app);
      goto out;
    }
    /* A replace keeps every doc id, and an append uses one past the old end,
     * so `target` is the row this text belongs to. */
    if (v != NULL && app->vec != NULL) {
      if (dim != kbc_vecstore_dim(app->vec)) {
        KBC_LOGW("embedder returned dim %zu, store is %zu; vector lane off", dim,
                 kbc_vecstore_dim(app->vec));
      } else {
        kbc_err local;
        kbc_err_reset(&local);
        if (kbc_failed(kbc_vecstore_set(app->vec, target, v, &local))) {
          KBC_LOGW("vector row %u for %s: %s, dropping the vector lane", target,
                   rel_path, local.msg);
          kbc_vecstore_free(app->vec);
          app->vec = NULL;
          dropped_vec = true;
        }
      }
    }
  }

  s = index_publish(app, app->index, false, err);
  if (kbc_failed(s)) {
    index_unlock(app);
    goto out;
  }
  const int64_t docs = (int64_t)kbc_index_doc_count(app->index);
  atomic_store_explicit(&app->st_indexed, docs, memory_order_relaxed);
  atomic_store_explicit(&app->st_docs, docs, memory_order_relaxed);
  atomic_store_explicit(&app->st_terms, (int64_t)kbc_index_term_count(app->index),
                        memory_order_relaxed);
  atomic_store_explicit(&app->st_last_ns, kbc_now_ns(), memory_order_relaxed);
  atomic_store_explicit(&app->st_last_docs, docs, memory_order_relaxed);
  /* st_runs counts FULL scans, and this is not one; last_reindex_* is "when the
 * index last changed", which this is. */
  atomic_store_explicit(&app->st_last_us, (kbc_now_ns() - t0) / 1000,
                        memory_order_relaxed);
  index_unlock(app);

  kbc_str payload;
  kbc_str_init(&payload);
  if (!kbc_failed(kbc_str_printf(&payload, "{\"docs\":%" PRId64 "}", docs))) {
    kbc_app_publish(app, "index.updated", payload.ptr);
  }
  kbc_str_free(&payload);
  KBC_LOGI("reindex: %s/%s %s, %" PRId64 " docs in %" PRId64 " us%s", cc->name,
           rel_path, vanished ? "removed" : "updated", docs,
           (kbc_now_ns() - t0) / 1000,
           dropped_vec ? ", vector lane off" : "");

out:
  kbc_str_free(&text);
  kbc_str_free(&raw);
  kbc_str_free(&full);
  kbc_arena_free(fa);
  return s;
}

/* Bring one path up to date, without touching any other. The filesystem is the
 * authority, exactly as it is for a full scan: a delete event for a file that
 * is still there re-ingests it rather than dropping it, and a save event for a
 * file that has since been deleted drops it.
 *
 * Everything below `reindex_one_locked` runs holding reindex_mu, and the store
 * writes are the reason it is taken at all: the full pass writes the same rows
 * for the same documents — upsert_artifact, replace_chunks, replace_metas, the
 * edges — and `chunks` is UNIQUE(doc_id, ord), so a rebuild ingesting a
 * document while the single-file path ingests the same one collided. Serialising
 * only the index, as this did, left the half that actually collides unprotected.
 *
 * A CORRECTION, since this comment named the wrong constraint. Measurement
 * later showed the ord collision is UNREACHABLE even between two apps over one
 * data dir: `replace_chunks` holds BEGIN IMMEDIATE across its DELETE and its
 * INSERT, so SQLite serialises the connections itself. What actually fired was
 * a FOREIGN KEY violation — `chunks.doc_id REFERENCES artifacts(id)` — because
 * ingest is two transactions and another app's removal lands between the
 * artifact upsert and the chunk write. That is why the lock spans several store
 * calls rather than one: the window is between them, not inside either. The
 * message was "duplicate ord" because kb-c leaves sqlite's extended result
 * codes off, so every constraint returns the primary code 19; the store now
 * reads the extended code and names the constraint that actually fired.
 *
 * The lock covers the whole body rather than the store phase and the index
 * phase separately, because the two must not be separable: split, the store
 * would hold the new revision while the index still held the old one, which
 * is the half-applied state app.h exists to prevent. It is taken HERE, once,
 * so every return in the body — including the error unwinds — releases it. The
 * body calls the `_pass` form of the full rebuild and the index mutation
 * assumes the mutex is held; NEITHER may take it again. */
static kbc_status reindex_one_locked(kbc_app *app, const char *corpus,
                                     const char *rel_path, kbc_err *err) {
  const kbc_corpus_cfg *cc = kbc_config_corpus(app->cfg, corpus);
  if (!cc) {
    return kbc_err_set(err, KBC_ERR_NOTFOUND, "corpus %s is not configured",
                       corpus);
  }
  /* No index to update in place (first start, before any reindex): a full scan
   * is not a fallback here, it is the only thing that can work. */
  if (app->index == NULL) {
    return reindex_pass(app, err);
  }

  /* The filesystem is the authority. A path that is not there is a removal:
   * the store row goes first, so the index is never the side that still knows
   * about a document the store has dropped. */
  struct stat sb;
  kbc_str full;
  kbc_str_init(&full);
  kbc_status s = kbc_str_printf(&full, "%s/%s", cc->path, rel_path);
  bool vanished = kbc_failed(s) || stat(full.ptr, &sb) != 0;

  if (vanished) {
    kbc_str_free(&full);
    /* `s` is assigned here or not at all. It used to be left holding the
     * kbc_str_printf result from two lines up — which is KBC_OK, because
     * `vanished` came from the stat, not the printf — so a store_forget_path
     * failure fell through to `done` and was reported to the watcher as
     * success: the artifact row and the graph rows stayed, the document
     * stayed searchable, and nothing anywhere said it had failed. */
    s = store_forget_path(app, corpus, rel_path, err);
    if (kbc_failed(s)) {
      /* Reported, not papered over. Nothing is half-applied here — the row is
       * still in the store AND still in the index, so the state is the old
       * consistent one — and `done`'s full-scan fallback is for the opposite
       * case, the store row committed while the index was not. */
      return s;
    }
    s = index_touch_one(app, cc, rel_path, true, err);
    goto done;
  }

  kbc_app_ingested g;
  s = ingest_file(app, cc->name, cc->path, rel_path,
                  (int64_t)sb.st_mtim.tv_sec * 1000000000LL +
                      (int64_t)sb.st_mtim.tv_nsec,
                  (int64_t)sb.st_size, &g, err);
  kbc_str_free(&full);
  if (kbc_failed(s)) {
    if (s != KBC_ERR_NOTFOUND) {
      return s;
    }
    /* Vanished between the stat and the read. */
    KBC_LOGW("%s/%s vanished mid-reindex, dropping it from the index", corpus,
             rel_path);
    kbc_err_reset(err);
    s = store_forget_path(app, corpus, rel_path, err);
    if (kbc_failed(s)) {
      return s;
    }
    s = index_touch_one(app, cc, rel_path, true, err);
    goto done;
  }
  /* The facets, replaced for this document alone — the same replace the full
   * pass does, so a watched edit that removes a tag drops the match without
   * waiting for a full scan. */
  s = kbc_store_replace_metas(app->store, corpus, rel_path,
                              (const char *const *)g.meta_keys,
                              (const char *const *)g.meta_values, g.n_metas,
                              err);
  if (kbc_failed(s)) {
    ingested_free(&g);
    return s;
  }
  /* The single-document path writes its own edges here, through the same
   * registry: the rest of the corpus is already in the index, so a target
   * that is a document resolves now. A target that is not yet indexed records
   * no edge but DOES record a pending link, and the drain below is what picks
   * it up when the target arrives — the watcher's single-file event, not a
   * full scan. */
  if (g.n_links > 0) {
    kbc_enrich_source one;
    one.corpus = corpus;
    one.src = rel_path;
    one.targets = g.link_paths;
    one.n_targets = g.n_links;
    char self_id[KBC_MAX_ID_LEN + 1];
    kbc_id_for_artifact(self_id, corpus, rel_path);
    const app_ladder lad = {NULL, self_id, rel_path, g.title};
    s = store_write_links(app, &one, 1, &lad, err);
  } else {
    s = kbc_store_delete_edges(app->store, corpus, rel_path, err);
    if (s == KBC_OK)
      s = kbc_store_delete_pending(app->store, corpus, rel_path, err);
  }
  /* This document is the TARGET of whatever was waiting for it. */
  if (s == KBC_OK)
    s = kbc_store_drain_pending(app->store, corpus, rel_path, err);
  if (kbc_failed(s)) {
    ingested_free(&g);
    return s;
  }
  ingested_free(&g);
  s = index_touch_one(app, cc, rel_path, false, err);

done:
  if (kbc_failed(s)) {
    /* The store row is committed and the index is not, so this file's
     * keywords are stale until something else reindexes it. A watcher does
     * not re-fire for a file it has already reported, so a full scan now is
     * what keeps the invariant app.h exists for — the rare path, and the only
     * one that cannot leave the daemon a reindex behind. */
    KBC_LOGW("reindex %s/%s: %s; falling back to a full scan", corpus, rel_path,
             err->msg);
    kbc_err_reset(err);
    return reindex_pass(app, err);
   }
  return KBC_OK;
 }

/* The validation, unlocked, and the body, locked. Argument checking touches
 * nothing shared, so it stays outside the mutex; everything that writes —
 * store rows and the index — is inside it, which is the invariant this file
 * exists to keep: ONE writer of the store and of the index at a time, so the
 * two indexers are excluded from each other on both halves rather than one.
 *
 * The lock is taken once, here, because reindex_one_locked returns from seven
 * places and a lock taken in the body is a lock each of them has to remember. */
static kbc_status reindex_one(kbc_app *app, const char *corpus,
                              const char *rel_path, kbc_err *err) {
  if (!app) {
    return kbc_err_set(err, KBC_ERR_INVALID, "reindex: app is NULL");
  }
  if (!corpus || !rel_path || rel_path[0] == '\0') {
    return kbc_err_set(err, KBC_ERR_INVALID,
                       "reindex: corpus and path are both required");
  }
  if (strlen(rel_path) > (size_t)KBC_MAX_PATH_LEN) {
    return kbc_err_set(err, KBC_ERR_INVALID, "%s/%s: path over the %u byte cap",
                       corpus, rel_path, (unsigned)KBC_MAX_PATH_LEN);
  }
  /* A path from the watcher or a request body reaches the filesystem; '..' is
   * refused before it does. */
  if (strstr(rel_path, "..") != NULL) {
    return kbc_err_set(err, KBC_ERR_INVALID, "%s/%s: '..' is not allowed",
                       corpus, rel_path);
  }
  pthread_mutex_lock(&app->reindex_mu);
  kbc_status s = reindex_one_locked(app, corpus, rel_path, err);
  pthread_mutex_unlock(&app->reindex_mu);
  return s;
}

kbc_status kbc_app_reindex_file(kbc_app *app, const char *corpus,
                                const char *rel_path, kbc_err *err) {
  return reindex_one(app, corpus, rel_path, err);
}

kbc_status kbc_app_reindex_remove(kbc_app *app, const char *corpus,
                                  const char *rel_path, kbc_err *err) {
  return reindex_one(app, corpus, rel_path, err);
}

/* include/kbc/app.h is frozen and has no by-path delete, so the declaration
 * lives here and the header line is reported rather than edited. The line
 * app.h should carry, verbatim:
 *
 *   kbc_status kbc_app_delete_path(kbc_app *app, const char *corpus,
 *                                  const char *rel_path, kbc_err *err);
 */
kbc_status kbc_app_delete_path(kbc_app *app, const char *corpus,
                               const char *rel_path, kbc_err *err);

/* Deletes ONE document, named by its path, whether or not the file is still
 * on disk. That is the difference from kbc_app_reindex_remove, which asks
 * the filesystem what happened and follows it: this one is the statement, and
 * the filesystem is not consulted. A file that is still there comes back at
 * the next full pass, because the corpus on disk is the authority — but it
 * comes back as a fresh ingest, and the history, corkboard entries, pins and
 * reading sections it left behind are still the user's.
 *
 * The cascade is store_forget_path's and is documented there in full: edges
 * and doc_metas and artifacts go, chunks and comments cascade with the
 * artifacts row, and history, corkboard, pinned_memories and reading_sections
 * stay. A removal must not take a user's reading history with it.
 *
 * LOCKING. reindex_mu, the OUTER lock, taken here and released on every
 * return: this writes the same store rows and the same index the full pass
 * does, so it is excluded from exactly the same set of callers, and it is
 * taken BEFORE the rwlock. The rwlock is taken by index_touch_one, once,
 * around the index mutation — never before the mutex, never twice. Nothing
 * below re-enters a reindex entry point: reindex_one and reindex_locked would
 * both self-deadlock on this mutex, and neither is what this does.
 *
 * Not a full-scan fallback on an index failure, deliberately, where
 * reindex_one_locked has one. A full scan would re-ingest the file if it is
 * still on disk — silently undoing the delete the caller asked for, which is
 * the one outcome worse than reporting the failure. Nor is it needed for
 * consistency: the store row is already gone, and resolve_rows drops an index
 * row the store cannot resolve, so the document is unreachable from search
 * either way. What is left is a posting list the next full pass rebuilds. */
kbc_status kbc_app_delete_path(kbc_app *app, const char *corpus,
                               const char *rel_path, kbc_err *err) {
  if (!app) {
    return kbc_err_set(err, KBC_ERR_INVALID, "delete_path: app is NULL");
  }
  if (!corpus || !rel_path || rel_path[0] == '\0') {
    return kbc_err_set(err, KBC_ERR_INVALID,
                       "delete_path: corpus and path are both required");
  }
  /* The same two gates reindex_one applies, for the same reason: this path
   * reaches the filesystem, and a corpus name from a request body is hostile
   * input like anything else. */
  if (strlen(rel_path) > (size_t)KBC_MAX_PATH_LEN) {
    return kbc_err_set(err, KBC_ERR_INVALID, "%s/%s: path over the %u byte cap",
                       corpus, rel_path, (unsigned)KBC_MAX_PATH_LEN);
  }
  if (strstr(rel_path, "..") != NULL) {
    return kbc_err_set(err, KBC_ERR_INVALID, "%s/%s: '..' is not allowed",
                       corpus, rel_path);
  }
  const kbc_corpus_cfg *cc = kbc_config_corpus(app->cfg, corpus);
  if (!cc) {
    return kbc_err_set(err, KBC_ERR_NOTFOUND, "corpus %s is not configured",
                       corpus);
  }

  pthread_mutex_lock(&app->reindex_mu);
  /* The store first, so the index is never the side that still knows about a
   * document the store has dropped. */
  kbc_status s = store_forget_path(app, corpus, rel_path, err);
  if (s == KBC_OK && app->index != NULL) {
    s = index_touch_one(app, cc, rel_path, true, err);
  }
  pthread_mutex_unlock(&app->reindex_mu);
  if (kbc_failed(s)) {
    KBC_LOGW("delete_path %s/%s: %s", corpus, rel_path,
             err != NULL ? err->msg : kbc_status_str(s));
  }
  return s;
}


/* ========================================================== capture ===== */

/* Stage 4's capture engine, ported from kb-core/src/capture.rs.
 *
 * WHAT IS HERE AND WHAT IS NOT, because the boundary is not a preference:
 *
 *   here    the multipart DECODER (kbc_multipart_parse), the frontmatter
 *           stamp, the filename policy and the atomic write.
 *   NOT     the ROUTE. kb-c has no capture route, and httpd.c — where a route
 *           belongs, and where another agent is working right now — is not
 *           this file's to touch. The seam is the pair below: the route
 *           decodes a request, calls kbc_app_capture, answers. Nothing here
 *           opens a socket, reads a header or knows what an HTTP status is.
 *
 * THE STAMP IS BYTE-COMPARABLE with the original's, and that is the whole
 * reason this file bothers with a key ORDER. The same bytes through either
 * implementation must produce the same file, so the order below is a
 * compatibility contract, not a style choice: a capture that wrote the same
 * seven keys in a different order is a DIFFERENT file, and the diff that
 * proves parity would fail for a reason nobody can see in the bytes.
 *
 * The order is the original's — one call per key, left to right, each on the
 * running document (capture.rs:517, 518, 523, 529, 531, 541, 545):
 *
 *   1 kb-category          517   "capture" unless overridden; never empty
 *   2 kb-tags              518   the resolved tag list, ", " joined
 *   3 kb-capture-original  523   the multipart filename, when it has one
 *   4 kb-capture-url       529   the shared/saved-page URL, when it has one
 *   5 kb-capture-at        531   the capture's unix seconds, ALWAYS
 *   6 kb-session           541   only when absent from the UPLOADED content
 *   7 kb-expires-at        545   only when a display-only expiry was given
 *
 * `title` is deliberately not one of them. A capture preserves the uploaded
 * file's own authored title; the caller's title steers the OUTPUT FILENAME
 * and nothing else (capture.rs:80-85). Stamping it would overwrite a
 * document's title with the name of the button that uploaded it.
 *
 * Each key goes through the original's own line-oriented setter
 * (markdown.rs:327 set_frontmatter_field), and THAT is what decides where a
 * key lands: a key the document already carries is EDITED IN PLACE, and a key
 * it does not is APPENDED to the end of the block (markdown.rs:384-413). So
 * the order above is the order of first insertion, not a layout — a document
 * that arrived with `kb-category` already set keeps it first and grows the
 * other six after it. The setter rewrites the whole document per key, seven
 * times, exactly as the original does; that is O(7n) over a body the caller
 * has already capped, and matching the reference's algorithm is worth more
 * here than saving six copies would be.
 *
 * WHAT IS DELIBERATELY ABSENT: the HTML pipeline. capture.rs stamps an HTML
 * capture by splicing <meta> tags into <head> and, opt-in, running ammonia
 * over the page. kb-c has no sanitiser (PORT_PLAN.md §1 lists ammonia as a
 * known gap, "writes captures un-sanitised"), and a head-splice without one
 * is a way to persist attacker markup into a trusted origin. Markdown only,
 * until the sanitiser exists. */

/* The per-kb capture subfolder (capture.rs:27). kb-c's frozen config has no
 * `[server.capture].capture_dir` override, so the default is the only
 * spelling; the field on the input is there so the seam can change. */
#define CAPTURE_DIR_DEFAULT "capture"
/* capture.rs:38 DEFAULT_MAX_FILE_BYTES. This is ONE file; the per-request
 * budget on top of it belongs to the route. */
#define CAPTURE_MAX_FILE_BYTES (10u * 1024u * 1024u)
/* capture.rs:376 unique_filename, and the slug cap at capture.rs:322. */
#define CAPTURE_SLUG_MAX 60u
/* The collision loop's backstop. The original has no ceiling, which on a
 * directory of a million same-second captures is a spin; this many probes
 * means something is wrong that a report will say out loud. */
#define CAPTURE_NAME_PROBES 10000u
/* capture.rs:60 MAX_CAPTURE_FILES. */
#define MULTIPART_MAX_PARTS 50u
/* RFC 2046 caps a boundary at 70 characters. */
#define MULTIPART_MAX_BOUNDARY 70u

/* The capture surface — the four entry points and the four types they take —
 * is app.h's, and app.h is the ONLY place it is written. This file is its
 * definition, and the header comment there is the contract, the key order
 * with the Rust line behind each key, and the reasons a capture carries no
 * title and never fetches its url. */

/* The shared write path of both public entry points, with `tags` already
 * resolved: the original's two public functions resolve their own tag lists
 * and then call one private writer (capture.rs:154, 166, 201), because the
 * url-stub's raw `kind:url-stub` provenance tag must not be slugified — the
 * transform would collapse its colon to a dash. */
static kbc_status capture_write(kbc_app *app, const kbc_corpus_cfg *cc,
                                const kbc_capture_input *in,
                                const char *tags_joined, kbc_capture_result *out,
                                kbc_err *err);

/* The original's `parser::slugify_tag` (parser.rs:668-684): ASCII
 * alphanumerics kept and lowercased, every other byte — a colon included —
 * collapsed to a single dash, no dash leading and none trailing. It is
 * deliberately NOT kbc_slugify, which folds diacritics and would turn "café"
 * into "cafe" where the original drops the bytes: a tag is compared byte for
 * byte by the overlay, so these must not be two different functions. */
static kbc_status slug_tag_into(kbc_str *out, const char *s, size_t b,
                                size_t e) {
  bool pending = false, any = false;
  for (size_t i = b; i < e; i++) {
    unsigned char c = (unsigned char)s[i];
    /* The original iterates chars() of a trimmed string. A multi-byte char is
     * a run of non-alphanumerics here, and its first byte decides it, so the
     * bytes are walked one at a time and the result is the same. */
    const bool alnum = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') ||
                       (c >= 'A' && c <= 'Z');
    if (!alnum) {
      pending = true;
      continue;
    }
    if (pending && any) {
      kbc_status st = kbc_str_putc(out, '-');
      if (kbc_failed(st)) return st;
    }
    pending = false;
    any = true;
    kbc_status st = kbc_str_putc(out, (char)(c >= 'A' && c <= 'Z' ? c + 32 : c));
    if (kbc_failed(st)) return st;
  }
  return KBC_OK;
}

static void trim_span(const char *s, size_t *b, size_t *e) {
  while (*b < *e && chunk_space(s[*b])) (*b)++;
  while (*e > *b && chunk_space(s[*e - 1])) (*e)--;
}

/* capture.rs:310-329, capture_slug_checked. A dash is only ever emitted
 * AHEAD of an alphanumeric, so the output never leads or trails one — except
 * after the 60-character cap cuts in the middle of a run, which is the
 * second trim the original does. NULL (empty) rather than the "capture"
 * fallback; base_stem applies that. */
static kbc_status capture_slug_into(kbc_str *out, const char *s, size_t b,
                                    size_t e) {
  kbc_str tmp;
  kbc_str_init(&tmp);
  kbc_status st = slug_tag_into(&tmp, s, b, e);
  if (kbc_failed(st)) {
    kbc_str_free(&tmp);
    return st;
  }
  size_t take = tmp.len < (size_t)CAPTURE_SLUG_MAX ? tmp.len
                                                   : (size_t)CAPTURE_SLUG_MAX;
  while (take > 0 && tmp.ptr[take - 1] == '-') take--;
  if (take > 0) st = kbc_str_append(out, tmp.ptr, take);
  kbc_str_free(&tmp);
  return st;
}

/* The same policy with the "capture" fallback the filename path uses
 * (capture.rs:301-303). */
static kbc_status capture_slug_or_default(kbc_str *out, const char *s, size_t b,
                                         size_t e) {
  kbc_status st = capture_slug_into(out, s, b, e);
  if (kbc_failed(st)) return st;
  if (out->len == 0) st = kbc_str_puts(out, "capture");
  return st;
}

/* `Path::file_stem` (capture.rs:341-344) over the FINAL path component. A
 * leading dot is not an extension, so ".bashrc" stems to itself. This runs
 * before the slug collapses the dots anyway; it is here because the original
 * has it, and a name like "report.2024.md" must stem to "report.2024" and
 * slug to "report-2024", not to "report". */
static kbc_status file_stem_into(kbc_str *out, const char *name) {
  const char *base = name;
  for (const char *p = name; *p != '\0'; p++) {
    if (*p == '/' || *p == '\\') base = p + 1;
  }
  size_t n = strlen(base);
  for (size_t i = n; i > 0; i--) {
    if (base[i - 1] == '.') {
      if (base + i - 1 != base) n = i - 1;
      break;
    }
  }
  return kbc_str_append(out, base, n);
}

/* Title wins when non-empty, else the original filename's stem, else
 * "capture" (capture.rs:336-348). A traversal attempt in the filename has
 * nothing left to traverse WITH by this point: every separator and dot is a
 * dash in the slug. */
static kbc_status base_stem(kbc_str *out, const char *title,
                            const char *original_filename) {
  if (title != NULL) {
    size_t b = 0, e = strlen(title);
    trim_span(title, &b, &e);
    if (e > b) return capture_slug_or_default(out, title, b, e);
  }
  if (original_filename != NULL) {
    kbc_str stem;
    kbc_str_init(&stem);
    kbc_status st = file_stem_into(&stem, original_filename);
    if (kbc_failed(st)) {
      kbc_str_free(&stem);
      return st;
    }
    st = capture_slug_or_default(out, stem.ptr != NULL ? stem.ptr : "",
                                 0, stem.len);
    kbc_str_free(&stem);
    return st;
  }
  return kbc_str_puts(out, "capture");
}

/* capture.rs:356-363. The caller's extension map has already validated the
 * shape; this is defence in depth, because a broken value here would
 * otherwise land a trailing-dot filename on disk. */
static kbc_status capture_ext(char out[16], const char *ext) {
  const char *b = ext != NULL ? ext : "";
  size_t s = 0, e = strlen(b);
  trim_span(b, &s, &e);
  while (s < e && b[s] == '.') s++;
  size_t n = 0;
  for (size_t i = s; i < e && n < 15u; i++, n++) {
    char c = b[i];
    if (c >= 'A' && c <= 'Z') c = (char)(c + 32);
    if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'z'))) {
      n = 0; /* one non-alphanumeric and the whole value is refused */
      break;
    }
    out[n] = c;
  }
  if (n == 0) {
    memcpy(out, "md", 3);
  } else {
    out[n] = '\0';
  }
  return KBC_OK;
}

/* ----------------------------------------------------------------- fence */

/* markdown.rs:373-381's `is_match`, verbatim in behaviour: a blank line and
 * a `#` comment are never a key, and a line matches only when the text
 * before its first colon trims to exactly `key`. `n` includes the trailing
 * newline, which no amount of trimming can reach. */
static bool fm_key_is(const char *line, size_t n, const char *key) {
  size_t b = 0, e = n;
  trim_span(line, &b, &e);
  if (b == e || line[b] == '#') return false;
  const size_t klen = strlen(key);
  for (size_t i = b; i < e; i++) {
    if (line[i] != ':') continue;
    size_t kb = b, ke = i;
    trim_span(line, &kb, &ke);
    return (ke - kb) == klen && memcmp(line + kb, key, klen) == 0;
  }
  return false;
}

/* markdown.rs:372-415, edit_keys_region. Non-matching lines (other keys,
 * comments, blanks) ride through verbatim; a DUPLICATE key line collapses to
 * the single edited one, so the value the reader gets — last-wins — is
 * unambiguous. A key the block does not carry is appended after the last
 * line, in the block's own line ending. */
static kbc_status fm_edit_keys(kbc_str *out, const char *region, size_t rlen,
                               const char *key, const char *val, bool crlf) {
  bool emitted = false;
  size_t i = 0;
  while (i < rlen) {
    size_t j = i;
    while (j < rlen && region[j] != '\n') j++;
    const size_t line_end = (j < rlen) ? j + 1 : rlen;
    const size_t linelen = line_end - i;
    if (fm_key_is(region + i, linelen, key)) {
      if (val != NULL && !emitted) {
        /* The line's OWN ending, or none at all when it has none
         * (markdown.rs:386-393) — a rewritten key must not gain a newline the
         * document did not have. */
        const char *eol = "";
        if (linelen >= 2 && region[i + linelen - 2] == '\r' &&
            region[i + linelen - 1] == '\n') {
          eol = "\r\n";
        } else if (linelen >= 1 && region[i + linelen - 1] == '\n') {
          eol = "\n";
        }
        kbc_status st = kbc_str_printf(out, "%s: %s%s", key, val, eol);
        if (kbc_failed(st)) return st;
        emitted = true;
      }
    } else {
      kbc_status st = kbc_str_append(out, region + i, linelen);
      if (kbc_failed(st)) return st;
    }
    i = line_end;
 }
  if (val != NULL && !emitted) {
    const char *eol = crlf ? "\r\n" : "\n";
    if (out->len > 0 && out->ptr[out->len - 1] != '\n') {
      kbc_status st = kbc_str_puts(out, eol);
      if (kbc_failed(st)) return st;
    }
    kbc_status st = kbc_str_printf(out, "%s: %s%s", key, val, eol);
    if (kbc_failed(st)) return st;
  }
  return KBC_OK;
}

/* markdown.rs:327-366, set_frontmatter_field. ONE thing this file relies on:
 * `out` is always a fresh buffer and `src` is only ever READ, so a caller
 * may pass a document that lives in the buffer it is replacing. `val` must
 * NOT alias `src` — the values here are always built separately.
 *
 * A leading "---" with no closing fence is NOT front matter
 * (markdown.rs:359-360): the block is prepended fresh and the "---" stays a
 * thematic break in the body, which is what the shared fence predicate
 * (kbc_fm_fence) says about the same document. */
static kbc_status fm_set_field(kbc_str *out, const char *src, size_t n,
                               const char *key, const char *val) {
  size_t bom = 0, start = 0, end = 0, body = 0;
  kbc_str work;
  kbc_str_init(&work);
  kbc_status st;
  if (kbc_fm_fence(src, n, &bom, &start, &end, &body)) {
    size_t open = 0;
    bool crlf = false;
    if (n - bom >= 5 && src[bom + 3] == '\r' && src[bom + 4] == '\n') {
      open = 5;
      crlf = true;
    } else {
      open = 4;
    }
    st = kbc_str_append(&work, src, bom);
    if (kbc_failed(st)) goto done;
    st = kbc_str_append(&work, src + bom, open);
    if (kbc_failed(st)) goto done;
    st = fm_edit_keys(&work, src + start, end - start, key, val, crlf);
    if (kbc_failed(st)) goto done;
    st = kbc_str_append(&work, src + end, n - end); /* fence + body, verbatim */
    if (kbc_failed(st)) goto done;
  } else if (val == NULL) {
    st = kbc_str_append(&work, src, n);
    if (kbc_failed(st)) goto done;
  } else {
    st = kbc_str_printf(&work, "---\n%s: %s\n---\n", key, val);
    if (st == KBC_OK) st = kbc_str_append(&work, src + bom, n - bom);
    if (kbc_failed(st)) goto done;
 }
  *out = work;
  return KBC_OK;
done:
  kbc_str_free(&work);
  return st;
}

/* capture.rs:539-540: is `key` already in the UPLOADED content? The original
 * asks its own parser; this is the same question over the same block, which
 * is why the fence predicate is shared rather than re-derived. */
static bool fm_src_has_key(const char *src, size_t n, const char *key) {
  size_t bom = 0, start = 0, end = 0, body = 0;
  if (!kbc_fm_fence(src, n, &bom, &start, &end, &body)) return false;
  size_t i = start;
  while (i < end) {
    size_t j = i;
    while (j < end && src[j] != '\n') j++;
    const size_t line_end = (j < end) ? j + 1 : end;
    if (fm_key_is(src + i, line_end - i, key)) return true;
    i = line_end;
  }
  return false;
}

/* The one-call form the stamp uses: edit a key in the document `doc` already
 * holds and swap the result in. Seven of these is seven full rewrites, which
 * is what the original does too (each set_frontmatter_field returns a new
 * String) — see the note on the key order at the top of this section. */
static kbc_status fm_apply(kbc_str *doc, const char *key, const char *val) {
  kbc_str next;
  kbc_str_init(&next);
  kbc_status st =
      fm_set_field(&next, doc->ptr != NULL ? doc->ptr : "", doc->len, key, val);
  if (kbc_failed(st)) {
    kbc_str_free(&next);
    return st;
  }
  kbc_str_free(doc);
  *doc = next;
  return KBC_OK;
}

/* capture.rs:465-467, sanitize_frontmatter_value. The writer is
 * line-oriented, so a newline inside an attacker-controlled value — the
 * multipart filename, a shared URL — would inject a frontmatter line, and a
 * line reading `---` would close the fence early and splice arbitrary
 * "frontmatter" into the document. Every newline becomes a space, which
 * makes that unreachable. */
static kbc_status fm_value_clean(kbc_str *out, const char *s) {
  size_t b = 0, e = strlen(s);
  trim_span(s, &b, &e);
  for (size_t i = b; i < e; i++) {
    const char c = (s[i] == '\r' || s[i] == '\n') ? ' ' : s[i];
    kbc_status st = kbc_str_putc(out, c);
    if (kbc_failed(st)) return st;
  }
  return KBC_OK;
}

/* capture.rs:441-454, build_tag_list. `source:upload` always; `from:<slug>`
 * when the caller named a source that slugifies to something; then every
 * caller tag through slugify_tag, skipping the ones that slugify to empty.
 * The two provenance tags keep their colon — the original writes them
 * literally, and only the VALUE of `from` is slugified. */
static kbc_status build_tag_list(kbc_str *out, const char *from,
                                 const char *const *tags, size_t n_tags) {
  kbc_status st = kbc_str_puts(out, "source:upload");
  if (kbc_failed(st)) return st;
  if (from != NULL) {
    kbc_str slug;
    kbc_str_init(&slug);
    size_t b = 0, e = strlen(from);
    trim_span(from, &b, &e);
    st = slug_tag_into(&slug, from, b, e);
    if (st == KBC_OK && slug.len > 0) {
      st = kbc_str_puts(out, ", from:");
      if (st == KBC_OK) st = kbc_str_append(out, slug.ptr, slug.len);
    }
    kbc_str_free(&slug);
    if (kbc_failed(st)) return st;
 }
  for (size_t i = 0; i < n_tags; i++) {
    kbc_str slug;
    kbc_str_init(&slug);
    size_t b = 0, e = strlen(tags[i]);
    trim_span(tags[i], &b, &e);
    st = slug_tag_into(&slug, tags[i], b, e);
    if (st == KBC_OK && slug.len > 0) {
      st = kbc_str_puts(out, ", ");
      if (st == KBC_OK) st = kbc_str_append(out, slug.ptr, slug.len);
    }
    kbc_str_free(&slug);
    if (kbc_failed(st)) return st;
 }
  return KBC_OK;
}

/* The stamp, in the order documented at the top of this section. */
static kbc_status stamp_capture(kbc_str *out, const char *src, size_t n,
                                const kbc_capture_input *in,
                                const char *tags_joined, int64_t ts) {
  kbc_str doc, val;
  kbc_str_init(&doc);
  kbc_str_init(&val);
  kbc_status st = kbc_str_append(&doc, src, n);
  if (kbc_failed(st)) goto done;

  /* 1 kb-category — capture.rs:511-517. Sanitised, and empty falls back to
   * "capture" rather than writing a bare `kb-category:`. */
  kbc_str_clear(&val);
  st = fm_value_clean(&val, in->category != NULL ? in->category : "");
  if (st == KBC_OK && val.len == 0) st = kbc_str_puts(&val, "capture");
  if (st != KBC_OK) goto done;
  st = fm_apply(&doc, "kb-category", val.ptr);
  if (kbc_failed(st)) goto done;

  /* 2 kb-tags — capture.rs:518. Always present, even empty: a capture with
   * no tags and no `from` still declares where it came from. */
  st = fm_apply(&doc, "kb-tags", tags_joined);
  if (kbc_failed(st)) goto done;

  /* 3 kb-capture-original — capture.rs:519-524. */
  kbc_str_clear(&val);
  st = fm_value_clean(&val, in->original_filename != NULL
                                   ? in->original_filename
                                   : "");
  if (kbc_failed(st)) goto done;
  if (val.len > 0) {
    st = fm_apply(&doc, "kb-capture-original", val.ptr);
    if (kbc_failed(st)) goto done;
  }

  /* 4 kb-capture-url — capture.rs:525-530.
   *
   * THE URL IS INERT TEXT. The daemon NEVER fetches it: that is the
   * project's SSRF ruling (capture.rs:16-18, "enrichment is agent-layer
   * work, later"), not an omission and not a TODO. A capture records where
   * something came from; resolving it is the agent's job on a machine that
   * is allowed to reach the internet. If you are reading this while adding
   * a fetch, stop: you are adding a server-side request forgery to a daemon
   * that binds to a network. */
  kbc_str_clear(&val);
  st = fm_value_clean(&val, in->url != NULL ? in->url : "");
  if (kbc_failed(st)) goto done;
  if (val.len > 0) {
    st = fm_apply(&doc, "kb-capture-url", val.ptr);
    if (kbc_failed(st)) goto done;
  }

  /* 5 kb-capture-at — capture.rs:531. The only unconditional stamp after the
   * category: a capture without a clock is not provenance. */
  kbc_str_clear(&val);
  st = kbc_str_printf(&val, "%lld", (long long)ts);
  if (kbc_failed(st)) goto done;
  st = fm_apply(&doc, "kb-capture-at", val.ptr);
  if (kbc_failed(st)) goto done;

  /* 6 kb-session — capture.rs:532-543. Never overwrites a session marker
   * the UPLOADED content already carries: the check is against the SOURCE,
   * not against the document this loop has been editing, or a capture
   * would stamp a second kb-session on every retry. */
  kbc_str_clear(&val);
  st = fm_value_clean(&val, in->session_id != NULL ? in->session_id : "");
  if (kbc_failed(st)) goto done;
  if (val.len > 0 && !fm_src_has_key(src, n, "kb-session")) {
    st = fm_apply(&doc, "kb-session", val.ptr);
    if (kbc_failed(st)) goto done;
  }

  /* 7 kb-expires-at — capture.rs:544-546. Display-only; there is no sweeper,
   * and an absent expiry is byte-identical to not stamping one at all. */
  if (in->has_expires_at) {
    kbc_str_clear(&val);
    st = kbc_str_printf(&val, "%lld", (long long)in->expires_at);
    if (kbc_failed(st)) goto done;
    st = fm_apply(&doc, "kb-expires-at", val.ptr);
    if (kbc_failed(st)) goto done;
  }

  *out = doc;
  kbc_str_init(&doc); /* ownership moved; the free below is a no-op */
done:
  kbc_str_free(&doc);
  kbc_str_free(&val);
  return st;
}

static kbc_status capture_taken(const char *dir_abs, const char *name) {
  kbc_str p;
  kbc_str_init(&p);
  bool taken = false;
  if (kbc_str_printf(&p, "%s/%s", dir_abs, name) == KBC_OK) {
    taken = kbc_path_exists(p.ptr);
  }
  kbc_str_free(&p);
  return taken ? KBC_OK : KBC_ERR_NOTFOUND;
}

/* capture.rs:376-384, unique_filename. Two same-name captures in the same
 * second land on distinct paths, and since the id is minted from the path
 * (ids.c) that means distinct ids, so nothing silently overwrites. */
static kbc_status capture_filename(kbc_str *out, const char *dir_abs,
                                   const char *base, const char *ext,
                                   int64_t ts, kbc_err *err) {
  kbc_status st =
      kbc_str_printf(out, "%s-%lld.%s", base, (long long)ts, ext);
  if (kbc_failed(st)) return st;
  for (unsigned n = 1; n <= CAPTURE_NAME_PROBES; n++) {
    if (capture_taken(dir_abs, out->ptr) != KBC_OK) return KBC_OK;
    kbc_str_clear(out);
    st = kbc_str_printf(out, "%s-%lld-%u.%s", base, (long long)ts, n, ext);
    if (kbc_failed(st)) return st;
  }
  return kbc_err_set(err, KBC_ERR_CONFLICT,
                     "%s-%lld: %u captures already share this name", base,
                     (long long)ts, CAPTURE_NAME_PROBES);
}

/* ------------------------------------------------------------------ write */

static kbc_status capture_write(kbc_app *app, const kbc_corpus_cfg *cc,
                                const kbc_capture_input *in,
                                const char *tags_joined, kbc_capture_result *out,
                                kbc_err *err) {
  const char *dir_rel =
      (in->capture_dir != NULL && in->capture_dir[0] != '\0')
          ? in->capture_dir
          : CAPTURE_DIR_DEFAULT;
  /* Rule 9: this joins onto a corpus root and is caller-supplied. */
  if (strstr(dir_rel, "..") != NULL) {
    return kbc_err_set(err, KBC_ERR_INVALID,
                       "capture: capture_dir %s may not contain '..'",
                       dir_rel);
 }
  char ext[16];
  capture_ext(ext, in->ext);

  kbc_str dir_abs, name, rel, stamped;
  kbc_str_init(&dir_abs);
  kbc_str_init(&name);
  kbc_str_init(&rel);
  kbc_str_init(&stamped);
  kbc_status st = kbc_str_printf(&dir_abs, "%s/%s", cc->path, dir_rel);
  /* The mutex is taken once, halfway down, and released once at `done` — the
   * shape reindex_one uses, because a function with this many exits takes a
   * lock in as many places otherwise. */
  bool locked = false;
  if (kbc_failed(st)) {
    st = kbc_err_set(err, KBC_ERR_NOMEM, "capture dir for %s/%s", cc->name,
                     dir_rel);
    goto done;
  }
  if (dir_abs.len > (size_t)KBC_MAX_PATH_LEN) {
    st = kbc_err_set(err, KBC_ERR_INVALID,
                     "%s: capture dir over the %u byte path cap", dir_abs.ptr,
                     (unsigned)KBC_MAX_PATH_LEN);
    goto done;
  }

  const int64_t ts = in->now_unix > 0 ? in->now_unix : (int64_t)time(NULL);
  kbc_str base;
  kbc_str_init(&base);
  st = base_stem(&base, in->title, in->original_filename);
  if (kbc_failed(st)) {
    kbc_str_free(&base);
    goto done;
  }
  /* The collision probe and the write are one window, so it runs under the
   * one-writer mutex. The original probes `.exists()` and then writes too
   * (capture.rs:379), so it has the same race; holding the lock here is what
   * makes "two same-second captures land on distinct paths" true rather than
   * usually true. Nothing below re-enters a reindex entry point. */
  pthread_mutex_lock(&app->reindex_mu);
  locked = true;
  st = capture_filename(&name, dir_abs.ptr, base.ptr, ext, ts, err);
  kbc_str_free(&base);
  if (kbc_failed(st)) goto done;

  st = kbc_str_printf(&rel, "%s/%s", dir_rel, name.ptr);
  if (kbc_failed(st)) {
    st = kbc_err_set(err, KBC_ERR_NOMEM, "capture path for %s", name.ptr);
    goto done;
  }
  if (rel.len > (size_t)KBC_MAX_PATH_LEN) {
    st = kbc_err_set(err, KBC_ERR_INVALID, "%s: over the %u byte path cap",
                     rel.ptr, (unsigned)KBC_MAX_PATH_LEN);
    goto done;
  }

  st = stamp_capture(&stamped, in->body, in->body_len, in, tags_joined, ts);
  if (kbc_failed(st)) {
    st = kbc_err_set(err, KBC_ERR_NOMEM, "capture stamp for %s", rel.ptr);
    goto done;
  }

  /* The directory before the file: a capture into a corpus that has never
 * had one is the normal first run, not an error. */
  st = kbc_mkdir_p(dir_abs.ptr, err);
  if (kbc_failed(st)) goto done;

  kbc_str abs;
  kbc_str_init(&abs);
  st = kbc_str_printf(&abs, "%s/%s", dir_abs.ptr, name.ptr);
  if (kbc_failed(st)) {
    kbc_str_free(&abs);
    st = kbc_err_set(err, KBC_ERR_NOMEM, "capture path for %s", name.ptr);
    goto done;
  }
  /* Atomic: a reader of the corpus sees the whole file or the old one, and
   * a capture that crashes mid-write leaves no half-stamped provenance. */
  st = kbc_str_write_file_atomic(abs.ptr, stamped.ptr, stamped.len, err);
  kbc_str_free(&abs);
  if (kbc_failed(st)) goto done;

  kbc_id_for_artifact(out->id, cc->name, rel.ptr);
  memcpy(out->path, rel.ptr, rel.len);
  out->path[rel.len] = '\0';
  out->bytes = stamped.len;

done:
  if (locked) pthread_mutex_unlock(&app->reindex_mu);
  kbc_str_free(&dir_abs);
  kbc_str_free(&name);
  kbc_str_free(&rel);
  kbc_str_free(&stamped);
  return st;
}

kbc_status kbc_app_capture(kbc_app *app, const char *corpus,
                           const kbc_capture_input *in, kbc_capture_result *out,
                           kbc_err *err) {
  if (app == NULL) {
    return kbc_err_set(err, KBC_ERR_INVALID, "capture: app is NULL");
  }
  if (corpus == NULL || in == NULL || out == NULL || in->body == NULL) {
    return kbc_err_set(err, KBC_ERR_INVALID,
                       "capture: corpus, input, result and body are all "
                       "required");
  }
  if (in->body_len > (size_t)CAPTURE_MAX_FILE_BYTES) {
    return kbc_err_set(err, KBC_ERR_INVALID,
                       "capture: %zu bytes is over the %u byte per-file cap",
                       in->body_len, (unsigned)CAPTURE_MAX_FILE_BYTES);
  }
  if (in->n_tags > 0 && in->tags == NULL) {
    return kbc_err_set(err, KBC_ERR_INVALID,
                       "capture: %zu tags with a NULL tag array", in->n_tags);
  }
  const kbc_corpus_cfg *cc = kbc_config_corpus(app->cfg, corpus);
  if (cc == NULL) {
    return kbc_err_set(err, KBC_ERR_NOTFOUND, "corpus %s is not configured",
                       corpus);
  }
  memset(out, 0, sizeof *out);

  kbc_str tags;
  kbc_str_init(&tags);
  kbc_status st = build_tag_list(&tags, in->from, in->tags, in->n_tags);
  if (kbc_failed(st)) {
    kbc_str_free(&tags);
    return kbc_err_set(err, KBC_ERR_NOMEM, "capture tag list for %s", corpus);
  }
  st = capture_write(app, cc, in, tags.ptr, out, err);
  kbc_str_free(&tags);
  return st;
}

/* capture.rs:281-290, build_url_stub. The body is an H1, the URL as a
 * Markdown autolink when there is one, and the shared text as a paragraph.
 * No escaping beyond what Markdown authoring already implies: this is the
 * artifact's own content, not attacker markup spliced into a trusted
 * document. */
static kbc_status build_url_stub(kbc_str *out, const char *title,
                                 const char *url, const char *text) {
  kbc_status st = kbc_str_printf(out, "# %s\n", title);
  if (kbc_failed(st)) return st;
  if (url != NULL) {
    size_t b = 0, e = strlen(url);
    trim_span(url, &b, &e);
    if (e > b) {
      st = kbc_str_putc(out, '\n');
      if (st == KBC_OK) st = kbc_str_putc(out, '<');
      if (st == KBC_OK) st = kbc_str_append(out, url + b, e - b);
      if (st == KBC_OK) st = kbc_str_puts(out, ">\n");
      if (kbc_failed(st)) return st;
 }
  }
  if (text != NULL) {
    size_t b = 0, e = strlen(text);
    trim_span(text, &b, &e);
    if (e > b) {
      st = kbc_str_putc(out, '\n');
      if (st == KBC_OK) st = kbc_str_append(out, text + b, e - b);
      if (st == KBC_OK) st = kbc_str_putc(out, '\n');
      if (kbc_failed(st)) return st;
    }
  }
  return KBC_OK;
}

kbc_status kbc_app_capture_url_stub(kbc_app *app, const char *corpus,
                                    const kbc_capture_url_input *in,
                                    kbc_capture_result *out, kbc_err *err) {
  if (app == NULL) {
    return kbc_err_set(err, KBC_ERR_INVALID, "capture_url_stub: app is NULL");
  }
  if (corpus == NULL || in == NULL || out == NULL) {
    return kbc_err_set(err, KBC_ERR_INVALID,
                       "capture_url_stub: corpus, input and result are all "
                       "required");
  }
  if (in->n_tags > 0 && in->tags == NULL) {
    return kbc_err_set(err, KBC_ERR_INVALID,
                       "capture_url_stub: %zu tags with a NULL tag array",
                       in->n_tags);
  }
  const kbc_corpus_cfg *cc = kbc_config_corpus(app->cfg, corpus);
  if (cc == NULL) {
    return kbc_err_set(err, KBC_ERR_NOTFOUND, "corpus %s is not configured",
                       corpus);
  }

  /* capture.rs:167-171: an empty title is "Untitled capture", not a bare
 * "#" that the indexer would read as an h1 with no text. */
  const char *title = in->title;
  if (title != NULL) {
    size_t b = 0, e = strlen(title);
    trim_span(title, &b, &e);
    if (e == b) title = NULL;
  }
  if (title == NULL) title = "Untitled capture";

  kbc_str body, tags;
  kbc_str_init(&body);
  kbc_str_init(&tags);
  kbc_status st = build_url_stub(&body, title, in->url, in->text);
  if (st == KBC_OK) st = build_tag_list(&tags, in->from, in->tags, in->n_tags);
  /* The raw provenance tag, appended AFTER the slugify so its colon
 * survives (capture.rs:174-175, and slugify_tag's own doc: a colon would
 * collapse to a dash). */
  if (st == KBC_OK && tags.len > 0) st = kbc_str_puts(&tags, ", ");
  if (st == KBC_OK) st = kbc_str_puts(&tags, "kind:url-stub");
  if (kbc_failed(st)) {
    kbc_str_free(&body);
    kbc_str_free(&tags);
    return kbc_err_set(err, KBC_ERR_NOMEM, "url stub for %s", corpus);
  }

  kbc_capture_input cap;
  memset(&cap, 0, sizeof cap);
  cap.corpus = corpus;
  cap.capture_dir = in->capture_dir;
  cap.from = in->from;
  cap.title = title;
  cap.url = in->url;
  cap.body = body.ptr;
  cap.body_len = body.len;
  cap.ext = "md";
  cap.now_unix = in->now_unix;
  st = capture_write(app, cc, &cap, tags.ptr, out, err);
  kbc_str_free(&body);
  kbc_str_free(&tags);
  return st;
}

/* ------------------------------------------------------------- multipart */

/* Byte-exact search for a short needle; SIZE_MAX when absent. The multipart
 * splitter cannot use strstr: a body is a byte string that may carry NULs
 * (rule 8 — lengths travel with strings). */
static size_t find_bytes(const char *hay, size_t hlen, const char *needle,
                         size_t nlen, size_t from) {
  if (nlen == 0 || hlen < nlen || from > hlen - nlen) return SIZE_MAX;
  for (size_t i = from; i + nlen <= hlen; i++) {
    if (hay[i] == needle[0] && memcmp(hay + i, needle, nlen) == 0) return i;
  }
  return SIZE_MAX;
}

static bool ascii_lower_eq(const char *s, size_t n, const char *lit) {
  return strlen(lit) == n && strncasecmp(s, lit, n) == 0;
}

/* The `boundary` parameter of a Content-Type, quoted or bare
 * (RFC 2046 §5.1.1). Returns the boundary with no dashes — the delimiter
 * adds them — and refuses the shapes a real body cannot contain. */
static kbc_status ct_boundary(kbc_str *out, const char *ct, kbc_err *err) {
  static const char key[] = "boundary=";
  const size_t keylen = sizeof key - 1;
  size_t n = strlen(ct);
  for (size_t i = 0; i + keylen <= n; i++) {
    if (!ascii_lower_eq(ct + i, keylen, "boundary=")) continue;
    size_t j = i + keylen;
    while (j < n && (ct[j] == ' ' || ct[j] == '\t')) j++;
    if (j >= n) break;
    if (ct[j] == '"') {
      j++;
      while (j < n && ct[j] != '"') {
        if ((unsigned char)ct[j] < 0x20u) {
          return kbc_err_set(err, KBC_ERR_INVALID,
                             "content-type: control byte in the boundary");
        }
        kbc_status st = kbc_str_putc(out, ct[j]);
        if (kbc_failed(st)) return st;
        j++;
      }
      if (j >= n) {
        return kbc_err_set(err, KBC_ERR_INVALID,
                           "content-type: unterminated quoted boundary");
      }
    } else {
      while (j < n && ct[j] != ';' && ct[j] != ' ' && ct[j] != '\t') {
        if ((unsigned char)ct[j] < 0x20u) {
          return kbc_err_set(err, KBC_ERR_INVALID,
                             "content-type: control byte in the boundary");
        }
        kbc_status st = kbc_str_putc(out, ct[j]);
        if (kbc_failed(st)) return st;
        j++;
      }
    }
    if (out->len == 0) {
      return kbc_err_set(err, KBC_ERR_INVALID, "content-type: empty boundary");
 }
    if (out->len > (size_t)MULTIPART_MAX_BOUNDARY) {
      return kbc_err_set(err, KBC_ERR_INVALID,
                         "content-type: boundary over the %u byte cap",
                         (unsigned)MULTIPART_MAX_BOUNDARY);
    }
    return KBC_OK;
  }
  return kbc_err_set(err, KBC_ERR_INVALID,
                     "content-type: no boundary parameter");
}

/* One parameter out of a Content-Disposition header, quoted or bare
 * (RFC 7578 §4.2). Found only at a parameter boundary, so a field named
 * "xname" is not a "name". The quoted form honours the backslash escape the
 * grammar allows and nothing else: a filename is bytes, and a filename is
 * not a security boundary here — it is provenance text and a slug source,
 * and both are sanitised downstream. */
static void cd_param(kbc_str *out, const char *hdr, size_t hlen,
                     const char *key) {
  const size_t klen = strlen(key);
  for (size_t i = 0; i + klen + 1 <= hlen; i++) {
    if (i != 0 && hdr[i - 1] != ';' && hdr[i - 1] != ' ') continue;
    if (!ascii_lower_eq(hdr + i, klen, key) || hdr[i + klen] != '=') {
      continue;
    }
    size_t j = i + klen + 1;
    if (j < hlen && hdr[j] == '"') {
      j++;
      while (j < hlen && hdr[j] != '"') {
        if (hdr[j] == '\\' && j + 1 < hlen) j++;
        (void)kbc_str_putc(out, hdr[j]);
        j++;
      }
      return;
    }
    while (j < hlen && hdr[j] != ';' && hdr[j] != ' ') {
      (void)kbc_str_putc(out, hdr[j]);
      j++;
    }
    return;
  }
}
/* THE COUNT IS ALWAYS RIGHT, and that is the whole contract. `*n_out` is the
 * number of parts in the body — on success AND on every failure — and
 * `out[0..*n_out)` is the first min(*n_out, out_cap) of them, written and
 * readable. The STATUS says why the parse stopped:
 *
 *   KBC_OK                  the body was consumed and every part fitted.
 *   KBC_ERR_INVALID, "N parts, more than the M this array holds"
 *                           the body is well formed and LONGER than the
 *                           array. Resize to *n_out and call again.
 *   KBC_ERR_INVALID, other  the body is malformed. Resizing will not help.
 *   KBC_ERR_NOMEM           the arena ran out mid-part; *n_out counts only
 *                           the parts fully built, and out[0..*n_out) is
 *                           still readable.
 *
 * A caller therefore gets two facts at once — what went wrong, and how big
 * its array needs to be — and the second is the one a caller that IGNORED
 * the status is still holding, which is what stops "silently truncated
 * upload" from needing a second thing to go right. */
kbc_status kbc_multipart_parse(const char *content_type, const char *body,
                               size_t body_len, kbc_multipart_part *out,
                               size_t out_cap, size_t *n_out, kbc_arena *a,
                               kbc_err *err) {
  if (content_type == NULL || body == NULL || out == NULL || n_out == NULL ||
      a == NULL) {
    return kbc_err_set(err, KBC_ERR_INVALID,
                       "multipart_parse: content_type, body, out, n_out and "
                       "arena are all required");
  }
  /* Parts FOUND, not parts stored: the two differ once the array fills, and
   * the number a caller needs is the first. `overflow` records that they
   * have, so the scan can finish and the message can name the real total. */
  size_t found = 0;
  bool overflow = false;
  *n_out = 0;

  kbc_str bnd;
  kbc_str_init(&bnd);
  kbc_status st = ct_boundary(&bnd, content_type, err);
  if (kbc_failed(st)) {
    kbc_str_free(&bnd);
    return st;
  }
  kbc_str dash;
  kbc_str_init(&dash);
  st = kbc_str_printf(&dash, "--%s", bnd.ptr);
  kbc_str_free(&bnd);
  if (kbc_failed(st)) {
    kbc_str_free(&dash);
    return kbc_err_set(err, KBC_ERR_NOMEM, "multipart delimiter");
  }

  /* The preamble before the first delimiter is legal and ignored, so the
   * scan starts wherever the first one is. */
  size_t pos = find_bytes(body, body_len, dash.ptr, dash.len, 0);
  while (pos != SIZE_MAX) {
    size_t after = pos + dash.len;
    if (after + 1 < body_len && body[after] == '-' && body[after + 1] == '-') {
      break; /* the closing delimiter; the epilogue is not a part */
 }
    if (after + 1 >= body_len || body[after] != '\r' || body[after + 1] != '\n') {
      st = kbc_err_set(err, KBC_ERR_INVALID,
                       "multipart: delimiter at byte %zu is not followed by "
                       "CRLF", pos);
      goto done;
 }
    const size_t start = after + 2;
    const size_t next = find_bytes(body, body_len, dash.ptr, dash.len, start);
    if (next == SIZE_MAX) {
      st = kbc_err_set(err, KBC_ERR_INVALID,
                       "multipart: unterminated body, no closing delimiter");
 goto done;
 }
    /* The CRLF in front of a delimiter belongs to the delimiter, not to the
 * part's content (RFC 2046 §5.1.1). */
    const size_t end =
        (next >= 2) ? next - 2 : next;
    const size_t hdr_end_rel = find_bytes(body + start, end - start, "\r\n\r\n", 4, 0);
    if (hdr_end_rel == SIZE_MAX) {
      st = kbc_err_set(err, KBC_ERR_INVALID,
                       "multipart: part at byte %zu has no header terminator",
                       start);
      goto done;
    }
    const size_t hdr_end = start + hdr_end_rel;

    /* Past the array's end the part is COUNTED but not stored, and the scan
     * carries on to the end of the body. Counting is the point: the caller
     * learns the true total (in *n_out, set on the way out whatever the
     * status) AND the fact that its array was too small, which is the only
     * way it can size a retry without guessing. Stopping at the cap would
     * report a number that is a property of the caller's buffer rather than
     * of the body, and a caller that grew and retried would have to guess how
     * much to grow by.
     *
     * Nothing is written to `out` past out_cap, so a caller that IGNORES the
     * status still cannot read a slot that was never filled: *n_out is the
     * number of parts in the body, and `out` holds the first min(that,
     * out_cap) of them. A truncated array with a success status would be the
     * failure the cap exists to prevent, and forgetting one check is exactly
     * how it would happen. */
    if (found >= out_cap) {
      overflow = true;
      found++;
      pos = next;
      continue;
    }

    kbc_str name, filename;
    kbc_str_init(&name);
    kbc_str_init(&filename);
    cd_param(&name, body + start, hdr_end - start, "name");
    cd_param(&filename, body + start, hdr_end - start, "filename");

    kbc_multipart_part *part = &out[found];
    part->name =
        kbc_arena_strndup(a, name.ptr != NULL ? name.ptr : "", name.len);
    part->filename = filename.len > 0
                         ? kbc_arena_strndup(a, filename.ptr, filename.len)
                         : NULL;
    const size_t data_len = (hdr_end + 4 <= end) ? end - (hdr_end + 4) : 0;
    part->data = kbc_arena_strndup(a, body + hdr_end + 4, data_len);
    part->len = data_len;
    kbc_str_free(&name);
    kbc_str_free(&filename);
    if (part->name == NULL || part->data == NULL) {
      /* The count names only parts that were FULLY materialised, so
       * out[0..*n_out) is always readable. This one is not: it is half-built
       * and is left unwritten rather than pointed at a hole. */
      part->name = NULL;
      part->filename = NULL;
      part->data = NULL;
      part->len = 0;
      st = kbc_err_set(err, KBC_ERR_NOMEM, "multipart part %zu", found);
      goto done;
    }
    found++;
    pos = next;
  }
  /* A MALFORMED body outranks an overflow: resizing the array will not fix a
   * truncated upload, and saying "your buffer was too small" to a caller
   * whose problem is a cut-off request sends it down the wrong road. */
  if (overflow) {
    st = kbc_err_set(err, KBC_ERR_INVALID,
                     "multipart: %zu parts, more than the %zu this array holds",
                     found, out_cap);
  } else {
    st = KBC_OK;
  }
done:
  kbc_str_free(&dash);
  /* The one rule about *n_out: it is the number of parts found, on success
   * and on every failure, and out[0..*n_out) is written and readable. A
   * caller gets two facts at once — what went wrong, and how big the array
   * needs to be — and the second one is the one a caller that ignored the
   * first is still holding. */
  *n_out = found;
  return st;
}

/* =============================================================== mv ===== */

/* A rename, and the one thing it has to be: a rename that loses the
 * document's ID-KEYED STATE is a delete plus a create wearing a rename's
 * name. kbc_app_delete_path's rule — a removal must not take a user's
 * reading history with it — applies here with the opposite polarity: here
 * the document did NOT go away, so nothing keyed to it may go away either.
 *
 * It is FOUR steps, and the third of them is ONE call:
 *
 *   1 record the `moves` intent row: old_id -> new_id, old_rel -> new_rel
 *   2 rename the file on disk
 *   3 kbc_store_rekey_artifact — every artifact-referencing row, in ONE
 *     transaction
 *   4 stamp the intent row complete
 *
 * WHY THE INTENT ROW GOES IN FIRST, and why it is stamped LAST. It is
 * written before the rename, so a crash between the rename and the rekey
 * leaves a durable, LISTABLE signal — kbc_store_list_incomplete_moves — and
 * not a document that quietly vanished between two operations nobody could
 * see. It is stamped only after the rekey committed, because
 * kbc_store_moves_lookup deliberately refuses to follow an unstamped row: an
 * interrupted move has to keep resolving to the name the document is
 * actually at, not to a name the rename never reached.
 *
 * WHY THERE IS NO DELETE-SUPPRESSION GUARD (the original's step 4, at
 * relocate.rs:1-15). It exists there because the watcher's debounce
 * synthesises a Deleted event for the old path after the rename, and a
 * delete-cascade would then take the just-migrated state with it. kb-c's
 * watcher delete resolves the event against the STORE row for that path
 * (watcher_cb -> reindex_one -> store_forget_path), and this function holds
 * reindex_mu across the record, the rename, the rekey AND the completion, so
 * by the time any watcher event can be processed the old path has no row to
 * cascade and the new path carries the migrated one. The window the original
 * needs a journal for does not open while the mutex is held, which is why
 * this file wants no moves_suppresses_delete.
 *
 * WHAT A RENAME PRESERVES, table by table:
 *
 *   REBUILT from the bytes by the re-ingest at the new path: the artifacts
 *   row's content fields, chunks, doc_metas and the document's own OUTBOUND
 *   edges. None of them need carrying — the same bytes derive them again.
 *
 *   CARRIED by the rekey, because none of it can be rebuilt from a file:
 *   every comment, with its own id AND its created_at (the rekey names only
 *   `comments.doc_id`, so the two columns the only public writer MINTS are
 *   never written and cannot change), the anchor, the author, the body and
 *   the resolution flag; the corkboard entry and the pin with the timestamps
 *   the user set; the doc_first_seen anchor; the reading history; and the
 *   INBOUND edges and pending links, which the store rewrites on BOTH path
 *   columns because a document that is itself a link target has its
 *   backlinks named at the old path.
 *
 *   NOT CARRIED, and the only one left is `doc_metas`: that table is keyed by
 *   (corpus, path) and the rekey works on ids, so the facets the document
 *   wrote at its old path are dropped by kbc_store_forget_document below. It
 *   runs AFTER the rekey and not before, so its graph half — which demotes
 *   inbound edges to pending links and deletes the outbound ones — finds
 *   nothing left naming this path, and its doc_metas half is the whole of
 *   what is left to do. Before the rekey it would throw those backlinks away
 *   and rebuild them from the demotions instead. */

kbc_status kbc_app_move_path(kbc_app *app, const char *corpus,
                             const char *old_rel, const char *new_rel,
                             kbc_err *err) {
  if (app == NULL) {
    return kbc_err_set(err, KBC_ERR_INVALID, "move_path: app is NULL");
  }
  if (corpus == NULL || old_rel == NULL || new_rel == NULL) {
    return kbc_err_set(err, KBC_ERR_INVALID,
                       "move_path: corpus and both paths are required");
  }
  /* The same two gates reindex_one applies, for the same reason: both of
 * these reach the filesystem, and a corpus name from a request body is
 * hostile input like anything else. */
  if (strstr(old_rel, "..") != NULL || strstr(new_rel, "..") != NULL) {
    return kbc_err_set(err, KBC_ERR_INVALID, "move_path: '..' is not allowed");
  }
  if (strlen(old_rel) > (size_t)KBC_MAX_PATH_LEN ||
      strlen(new_rel) > (size_t)KBC_MAX_PATH_LEN) {
    return kbc_err_set(err, KBC_ERR_INVALID,
                       "move_path: a path is over the %u byte cap",
                       (unsigned)KBC_MAX_PATH_LEN);
  }
  if (old_rel[0] == '\0' || new_rel[0] == '\0' || old_rel[0] == '/' ||
      new_rel[0] == '/') {
    return kbc_err_set(err, KBC_ERR_INVALID,
                       "move_path: paths are corpus-relative and non-empty");
  }
  if (strcmp(old_rel, new_rel) == 0) return KBC_OK; /* a no-op needs no lock */
  const kbc_corpus_cfg *cc = kbc_config_corpus(app->cfg, corpus);
  if (cc == NULL) {
    return kbc_err_set(err, KBC_ERR_NOTFOUND, "corpus %s is not configured",
                       corpus);
  }

  kbc_arena *a = kbc_arena_new(64u * 1024u);
  if (a == NULL) {
    return kbc_err_set(err, KBC_ERR_NOMEM, "move_path arena");
  }
  kbc_str old_abs, new_abs;
  kbc_str_init(&old_abs);
  kbc_str_init(&new_abs);
  kbc_status s = kbc_str_printf(&old_abs, "%s/%s", cc->path, old_rel);
  if (s == KBC_OK) s = kbc_str_printf(&new_abs, "%s/%s", cc->path, new_rel);
  if (kbc_failed(s)) {
    kbc_str_free(&old_abs);
    kbc_str_free(&new_abs);
    kbc_arena_free(a);
    return kbc_err_set(err, KBC_ERR_NOMEM, "move_path paths for %s", corpus);
  }

  pthread_mutex_lock(&app->reindex_mu);

  /* relocate.rs:293-311, in the original's order: the source must be a
 * file, the target must not exist, and the source must be INDEXED. A file
 * on disk that the store has never seen is not a document to move — moving
   * it would silently create one, which is a write the caller did not ask
   * for. */
  struct stat sb;
  if (stat(old_abs.ptr, &sb) != 0 || !S_ISREG(sb.st_mode)) {
    s = kbc_err_set(err, KBC_ERR_NOTFOUND, "move_path: no such source %s/%s",
                    corpus, old_rel);
    goto out;
  }
  if (kbc_path_exists(new_abs.ptr)) {
    s = kbc_err_set(err, KBC_ERR_CONFLICT, "move_path: %s/%s already exists",
                    corpus, new_rel);
    goto out;
  }

  kbc_artifact prev;
  memset(&prev, 0, sizeof prev);
  s = kbc_store_get_artifact_by_path(app->store, a, corpus, old_rel, &prev,
                                     err);
  if (kbc_failed(s)) {
    kbc_err_reset(err);
    s = kbc_err_set(err, KBC_ERR_NOTFOUND, "move_path: %s/%s is not indexed",
                    corpus, old_rel);
    goto out;
  }
  /* Rule 7: the copy below is a fixed 13 bytes, so the id is checked to be
   * the width the store promises before it is copied rather than trusted. */
  if (prev.id == NULL || strlen(prev.id) != (size_t)KBC_MAX_ID_LEN) {
    s = kbc_err_set(err, KBC_ERR_IO,
                    "move_path: %s/%s has a malformed artifact id", corpus,
                    old_rel);
    goto out;
  }
  char old_id[KBC_MAX_ID_LEN + 1];
  char new_id[KBC_MAX_ID_LEN + 1];
  memcpy(old_id, prev.id, KBC_MAX_ID_LEN);
  old_id[KBC_MAX_ID_LEN] = '\0';
  kbc_id_for_artifact(new_id, corpus, new_rel);
  /* relocate.rs:315-321: a path-hash collision is vanishingly rare, and
   * no-oping one would leave two paths serving one id. Refuse. */
  if (strcmp(old_id, new_id) == 0) {
    s = kbc_err_set(err, KBC_ERR_CONFLICT,
                    "move_path: %s and %s hash to the same artifact id %s",
                    old_rel, new_rel, old_id);
    goto out;
  }

  /* Step 1: the intent row, BEFORE the rename and under the same lock. Written
   * last, it would be a redirect to a file that had not moved yet. */
  s = kbc_store_record_move(app->store, old_id, new_id, old_rel, new_rel,
                            kbc_now_ns() / 1000000000LL, err);
  if (kbc_failed(s)) goto out;

  /* Step 2: the rename, still inside the mutex: see the header of this
   * section for why holding it across the rename AND the rekey closes the
   * watcher's delete window without a journal. A failure from here on has
   * already moved the file, and it is reported rather than undone — the
   * reconcile pass converges a stale row to whatever is on disk, and half of
   * a rename is not a state the store can represent. The unstamped intent row
   * left behind is the report: kbc_store_list_incomplete_moves names it. */
  kbc_str parent;
  kbc_str_init(&parent);
  const char *slash = strrchr(new_abs.ptr, '/');
  if (slash != NULL && slash != new_abs.ptr) {
    kbc_str_append(&parent, new_abs.ptr, (size_t)(slash - new_abs.ptr));
    s = kbc_mkdir_p(parent.ptr, err);
  }
  kbc_str_free(&parent);
  if (kbc_failed(s)) goto out;
  if (rename(old_abs.ptr, new_abs.ptr) != 0) {
    s = kbc_err_set(err, KBC_ERR_IO, "rename %s -> %s: %s", old_abs.ptr,
                    new_abs.ptr, strerror(errno));
    goto out;
  }

  /* Step 3: the rekey. ONE call, ONE transaction, and it carries everything
   * the document's bytes cannot rebuild — every comment with its minted id
   * and created_at, the corkboard anchor and the pin with the timestamps the
   * user set, the first-indexed second, the reading history, and the inbound
   * links on both path columns. It also re-keys the artifacts row itself, so
   * there is no delete-then-recreate here: a delete of the parent row is an
   * ON DELETE CASCADE, and doing it while the children still named the old id
   * would take the chunks and every comment with it.
   *
   * The first-indexed anchor has to be in place BEFORE the re-ingest below,
   * and this is what puts it there: the ingest's anchor write is an
   * INSERT OR IGNORE (store.c), so an anchor written after it would be too
   * late and the document's true first-indexed second would be lost. The
   * anchor does not move on a reindex (there is a test for that), and a
   * rename is not a reindex. */
  s = kbc_store_forget_document(app->store, corpus, old_rel, err);
  if (kbc_failed(s)) goto out;
  s = kbc_store_rekey_artifact(app->store, old_id, new_id, old_rel, new_rel,
                               err);
  if (kbc_failed(s)) goto out;

  /* The store first, then the index, exactly as delete_path does it, so the
   * index is never the side that still knows about a document the store has
   * dropped. The re-ingest is what rebuilds the chunks, the facets and the
   * outbound edges from the same bytes under the new id. */
  s = reindex_one_locked(app, corpus, new_rel, err);
  if (kbc_failed(s)) goto out;
  if (app->index != NULL) {
    s = index_touch_one(app, cc, old_rel, true, err);
    if (kbc_failed(s)) goto out;
  }

  /* Step 4: the intent is fulfilled, and only now. Stamped any earlier it
   * would tell kbc_store_moves_lookup to follow a rename that had not
   * happened, which is the one answer a redirect must never give. */
  s = kbc_store_complete_move(app->store, old_id, err);
  if (kbc_failed(s)) goto out;

  /* Announced AFTER the reindex, so a subscriber that reacts by searching
   * finds the document at its new path rather than nothing at all. Only the
   * departure is published: this codebase has no per-document arrival event
   * (index.updated is a whole-pass event), and inventing one here would be a
   * contract change in a file that is not this one. */
  publish_document_gone(app, corpus, old_rel);

out:
  pthread_mutex_unlock(&app->reindex_mu);
  if (kbc_failed(s)) {
    KBC_LOGW("move_path %s/%s -> %s: %s", corpus, old_rel, new_rel,
             err != NULL && err->msg[0] != '\0' ? err->msg
                                               : kbc_status_str(s));
  }
  kbc_str_free(&old_abs);
  kbc_str_free(&new_abs);
  kbc_arena_free(a);
  return s;
}


/* ---------------------------------------------------------------- search */
/* Resolves every surviving row in ONE store round trip.
 *
 * The searcher asked the store once per row, and the store guards one
 * connection with one mutex: a query returning k hits took that mutex k
 * times, and the convoy behind it — not the scoring, which is thread-local by
 * design — is what capped query throughput under concurrency. The index may
 * legitimately be one reindex ahead of the store, so a row the batch does not
 * resolve is dropped with a debug line and the search still succeeds. */
static kbc_status resolve_rows(kbc_app *app, kbc_arena *a,
                               kbc_search_result *out, int64_t since_ns,
                               kbc_err *err) {
  if (out->len == 0) return KBC_OK;

  const char **corpora = kbc_arena_calloc(a, out->len, sizeof(*corpora));
  const char **paths = kbc_arena_calloc(a, out->len, sizeof(*paths));
  if (corpora == NULL || paths == NULL)
    return kbc_err_set(err, KBC_ERR_NOMEM, "search: %zu hit pairs", out->len);
  for (size_t i = 0; i < out->len; i++) {
    /* A row with no corpus or no path cannot name a store row. The empty
     * string is the batch's own "unlookupable" marker: that slot comes back
     * NULL, exactly as a row with no store record does. */
    corpora[i] = out->rows[i].corpus != NULL ? out->rows[i].corpus : "";
    paths[i] = out->rows[i].path != NULL ? out->rows[i].path : "";
  }

  kbc_artifact **arts = NULL;
  kbc_err local;
  kbc_err_reset(&local);
  kbc_status s =
      kbc_store_get_artifacts_by_path(app->store, a, corpora, paths, out->len,
                                      &arts, &local);
  if (kbc_failed(s)) {
    free(arts);
    return kbc_err_set(err, s, "resolve %zu search hits: %s", out->len,
                       local.msg[0] ? local.msg : "store failed");
  }

  size_t kept = 0;
  for (size_t i = 0; i < out->len; i++) {
    kbc_result_row *r = &out->rows[i];
    if (arts[i] == NULL) {
      KBC_LOGD("search: doc %u not in store, dropping", r->doc_id);
      continue;
    }
    /* `since:` is a mtime predicate and the mtime lives here, one layer above
     * the index. It shares the unresolved-row path on purpose: a row that
     * cannot be resolved is dropped either way, so a since: filter changes
     * WHICH rows survive, never WHETHER an unresolvable one does. */
    if (since_ns > 0 && arts[i]->mtime_ns < since_ns) {
      KBC_LOGD("search: %s older than since:, dropping", arts[i]->path);
      continue;
    }
    r->artifact_id = arts[i]->id;
    r->summary = arts[i]->summary;
    if (kept != i) out->rows[kept] = *r;
    kept++;
  }
  out->len = kept;
  free(arts);
  return KBC_OK;
}

/* `since:<value>` out of the raw query string, in the same token shape the
 * grammar reads (query.rs:580): words split on whitespace, '(', ')', and
 * '"'; a ':' is its own token between a key and its value. The grammar
 * validates the value and drops the atom; the THRESHOLD is applied here,
 * because the mtime is the store's and the store is this layer. Two atoms
 * AND together, so the strictest wins — the same fold search.c does.
 *
 * `NOT since:` is skipped, matching the grammar: there is no upper bound to
 * negate, so the original warns and ignores it, and filtering on an atom the
 * grammar dropped would be two different queries under one name. */
typedef struct {
  const char *p;
  size_t len;
  bool quoted;
  bool colon;
} app_tok;

/* One token, or false at the end of the string. `quoted` carries the `"…"`
 * form, whose contents are a value even when they contain a space. */
static bool app_next_tok(const char *q, size_t len, size_t *i, app_tok *t) {
  while (*i < len) {
    const char c = q[*i];
    if (c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '(' ||
        c == ')') {
      (*i)++;
      continue;
    }
    if (c == ':') {
      (*i)++;
      t->p = q + *i - 1;
      t->len = 1;
      t->quoted = false;
      t->colon = true;
      return true;
    }
    if (c == '"') {
      const size_t s = ++(*i);
      while (*i < len && q[*i] != '"') (*i)++;
      t->p = q + s;
      t->len = *i - s;
      t->quoted = true;
      t->colon = false;
      if (*i < len) (*i)++;
      return true;
    }
    const size_t s = *i;
    while (*i < len && q[*i] != ' ' && q[*i] != '\t' && q[*i] != '\n' &&
           q[*i] != '\r' && q[*i] != '(' && q[*i] != ')' && q[*i] != ':') {
      (*i)++;
    }
    t->p = q + s;
    t->len = *i - s;
    t->quoted = false;
    t->colon = false;
    return true;
  }
  return false;
}

static kbc_status query_since_ns(const char *q, int64_t *ns, kbc_err *err) {
  *ns = 0;
  const size_t len = strlen(q);
  size_t i = 0;
  /* Four tokens of look-behind: `NOT` `since` `:` `value` is the longest
   * shape that matters, and the negation is the one thing that must be seen
   * before the atom is acted on. */
  app_tok ring[5];
  memset(ring, 0, sizeof(ring));
  size_t filled = 0;
  app_tok t;
  while (app_next_tok(q, len, &i, &t)) {
    const size_t slot = filled % 5;
    /* ring[slot-1] is the most recent previous token, so the pattern reads
     * backwards: `:` , `since` , whatever came before it. */
    const app_tok *p1 = &ring[(slot + 2) % 5]; /* the token before `since` */
    const app_tok *p2 = &ring[(slot + 3) % 5]; /* `since` */
    const app_tok *p3 = &ring[(slot + 4) % 5]; /* `:` */
    const bool shape = filled >= 3 && !p1->quoted && !p2->quoted && p2->len == 5 &&
                       strncasecmp(p2->p, "since", 5) == 0 && p3->colon &&
                       !t.colon && t.len > 0;
    const bool negated = shape && !p1->quoted && p1->len > 0 &&
                         (strncasecmp(p1->p, "not", 3) == 0 ||
                          strncasecmp(p1->p, "!", 1) == 0);
    if (shape && !negated) {
      char value[KBC_MAX_QUERY_LEN + 1];
      if (t.len >= sizeof(value))
        return kbc_err_set(err, KBC_ERR_INVALID, "since: value is too long");
      memcpy(value, t.p, t.len);
      value[t.len] = '\0';
      int64_t one = 0;
      kbc_status st = kbc_since_value_ns(value, &one, err);
      if (st != KBC_OK) return st;
      if (one > *ns) *ns = one;
    }
    ring[slot] = t;
    filled++;
  }
  return KBC_OK;
}

/* The corpus link graph, in the searcher's doc-id space.
 *
 * The store names documents by PATH and cannot name them by index doc id, so
 * the walk starts on the index side: the doc table is a flat array, and one
 * pass over it gives every path this corpus has, in doc-id order. Those paths
 * go to the store in ONE statement, and the degrees come back parallel to the
 * array — no positional join against the corpus's artifact order, and no
 * per-document store lookup. Only documents with a non-zero in-degree are
 * carried across; the rest cannot change an ordering.
 *
 * KBC_OWN out: free with free(). *ids_out / *deg_out are NULL when the corpus
 * has no linked document, which is the "no graph" case and is not an error. */
static kbc_status graph_in_degrees(kbc_app *app, const char *corpus,
                                    uint32_t **ids_out, uint32_t **deg_out,
                                    size_t *n_out, kbc_err *err) {
  *ids_out = NULL;
  *deg_out = NULL;
  *n_out = 0;
  const uint32_t n_docs = kbc_index_doc_count(app->index);
  size_t m = 0;
  for (uint32_t d = 0; d < n_docs; d++) {
    const kbc_doc_meta *meta = kbc_index_doc(app->index, d);
    if (meta != NULL && meta->corpus != NULL && meta->path != NULL &&
        strcmp(meta->corpus, corpus) == 0)
      m++;
  }
  if (m == 0) return KBC_OK;

  const char **paths = calloc(m, sizeof(*paths));
  uint32_t *doc_ids = calloc(m, sizeof(*doc_ids));
  uint32_t *deg = calloc(m, sizeof(*deg));
  if (paths == NULL || doc_ids == NULL || deg == NULL) {
    free(paths);
    free(doc_ids);
    free(deg);
    return kbc_err_set(err, KBC_ERR_NOMEM, "edge in-degrees: %zu documents", m);
  }
  size_t k = 0;
  for (uint32_t d = 0; d < n_docs; d++) {
    const kbc_doc_meta *meta = kbc_index_doc(app->index, d);
    if (meta != NULL && meta->corpus != NULL && meta->path != NULL &&
        strcmp(meta->corpus, corpus) == 0) {
      paths[k] = meta->path;
      doc_ids[k] = d;
      k++;
    }
  }

  kbc_status s = kbc_store_edge_degrees_for(app->store, corpus, paths, m, deg,
                                            err);
  free(paths);
  if (kbc_failed(s)) {
    free(doc_ids);
    free(deg);
    return s;
  }

  size_t kept = 0;
  for (size_t i = 0; i < m; i++) {
    if (deg[i] == 0) continue;
    doc_ids[kept] = doc_ids[i];
    deg[kept] = deg[i];
    kept++;
  }
  /* doc_ids was built by walking the index upward, so it is already ascending
   * and the searcher's binary search over it is valid as it stands. */
  if (kept == 0) {
    free(doc_ids);
    free(deg);
    return KBC_OK;
  }
  *ids_out = doc_ids;
  *deg_out = deg;
  *n_out = kept;
  return KBC_OK;
}

/* ------------------------------------------------------- facet overlay --- */

/* A sorted, unique set of corpus-relative paths. Sorted because every set
 * operation below is a merge and because the index walk tests membership
 * once per document; unique because the (corpus, key, value) index can hand
 * back the same path once per value it carries, and a duplicate in the
 * membership set would be a duplicate search hit. */
typedef struct {
  char **items; /* KBC_OWN */
  size_t len, cap;
} app_pathset;

static void pset_free(app_pathset *s) {
  if (s == NULL) return;
  for (size_t i = 0; i < s->len; i++) free(s->items[i]);
  free(s->items);
  s->items = NULL;
  s->len = 0;
  s->cap = 0;
}

static int cmp_path(const void *a, const void *b) {
  return strcmp(*(char *const *)a, *(char *const *)b);
}

static void pset_normalise(app_pathset *s) {
  if (s->len < 2) return;
  qsort(s->items, s->len, sizeof(*s->items), cmp_path);
  size_t w = 1;
  for (size_t i = 1; i < s->len; i++) {
    if (strcmp(s->items[w - 1], s->items[i]) == 0) {
      free(s->items[i]);
      continue;
    }
    s->items[w++] = s->items[i];
  }
  s->len = w;
}

static bool pset_has(const app_pathset *s, const char *p) {
  size_t lo = 0;
  size_t hi = s->len;
  while (lo < hi) {
    const size_t mid = lo + (hi - lo) / 2;
    if (strcmp(s->items[mid], p) < 0) {
      lo = mid + 1;
    } else {
      hi = mid;
    }
  }
  return lo < s->len && strcmp(s->items[lo], p) == 0;
}

static kbc_status pset_merge(const app_pathset *a, const app_pathset *b,
                             bool keep_only_both, app_pathset *out,
                             kbc_err *err) {
  /* NULL means the empty set: "the first positive literal" is a merge with
   * nothing, and spelling that as a compound literal keeps the merge itself
   * free of special cases. */
  static const app_pathset EMPTY = {NULL, 0, 0};
  if (a == NULL) a = &EMPTY;
  if (b == NULL) b = &EMPTY;
  memset(out, 0, sizeof(*out));
  const size_t cap = keep_only_both ? (a->len < b->len ? a->len : b->len)
                                    : a->len + b->len;
  if (cap == 0) return KBC_OK;
  out->items = calloc(cap, sizeof(*out->items));
  if (out->items == NULL)
    return kbc_err_set(err, KBC_ERR_NOMEM, "facet overlay: %zu paths", cap);
  out->cap = cap;
  size_t i = 0;
  size_t j = 0;
  while (i < a->len || (!keep_only_both && j < b->len)) {
    int c;
    const char *pick;
    if (j >= b->len) {
      pick = a->items[i++];
    } else if (i >= a->len) {
      pick = b->items[j++];
    } else {
      c = strcmp(a->items[i], b->items[j]);
      pick = c <= 0 ? a->items[i++] : b->items[j++];
      if (keep_only_both && c != 0) continue;
      if (c == 0 && !keep_only_both) j++; /* a duplicate, already taken */
    }
    out->items[out->len++] = strdup(pick);
    if (out->items[out->len - 1] == NULL) {
      pset_free(out);
      return kbc_err_set(err, KBC_ERR_NOMEM, "facet overlay: path copy");
    }
  }
  return KBC_OK;
}

/* The documents carrying (key, value) in `corpus`, as a set. ONE indexed
 * query; an empty answer is an empty set, which is a conjunct that matches
 * nothing rather than a conjunct that matches everything. */
static kbc_status pset_from_store(kbc_store *store, const char *corpus,
                                  const char *key, const char *value,
                                  app_pathset *out, kbc_err *err) {
  memset(out, 0, sizeof(*out));
  char **paths = NULL;
  size_t n = 0;
  kbc_status s = kbc_store_docs_with_meta(store, corpus, key, value, &paths,
                                          &n, err);
  if (kbc_failed(s)) return s;
  out->items = paths;
  out->len = n;
  out->cap = n;
  pset_normalise(out);
  return KBC_OK;
}

/* docs_query.rs:434 is_index_page — the basename is `index.html`, compared
 * case-insensitively. kb-c indexes Markdown as well as HTML, so `index.md` is
 * the same page here. A document may also DECLARE itself an index with a
 * truthy `kb-index` meta, which is the kb-c spelling of the category the
 * original reads from `kb_category`; that half is a store query, and the
 * basename half needs no store at all, so the two are applied where they can
 * be: the declaration in the conjunct's set, the basename in the index walk
 * (path_is_index). A non-truthy value never un-declares one: the flag says
 * "this is an index", not "this is not". */
static bool path_is_index(const char *path) {
  const char *base = strrchr(path, '/');
  base = base != NULL ? base + 1 : path;
  return strcasecmp(base, "index.html") == 0 ||
         strcasecmp(base, "index.md") == 0;
}

static kbc_status index_page_set(kbc_store *store, const char *corpus,
                                 app_pathset *out, kbc_err *err) {
  memset(out, 0, sizeof(*out));
  static const char *const TRUTHY[] = {"true", "1", "yes", ""};
  app_pathset acc;
  memset(&acc, 0, sizeof(acc));
  for (size_t i = 0; i < sizeof(TRUTHY) / sizeof(TRUTHY[0]); i++) {
    app_pathset part;
    kbc_status s = pset_from_store(store, corpus, "index", TRUTHY[i], &part,
                                   err);
    if (kbc_failed(s)) {
      pset_free(&acc);
      return s;
    }
    app_pathset merged;
    s = pset_merge(&acc, &part, false, &merged, err);
    pset_free(&acc);
    pset_free(&part);
    if (kbc_failed(s)) return s;
    acc = merged;
  }
  *out = acc;
  return KBC_OK;
}
/* One DNF conjunct, over one corpus, as the two sets the original's
 * matches_conjunct applies: `pos` is what the positive literals admit and
 * `neg` is what the negated ones drop.
 *
 * `pos` is NOT a fold over the atoms in the order they were typed. Inside one
 * conjunct the original ANDs the PREDICATES and compares like with like:
 * every tag literal is one any-of test (query.rs:286), every cap literal is
 * its own test (query.rs:297), and a document has to pass all of them. So the
 * tags union together, the caps intersect, and the two then intersect — which
 * is why `cap:code tag:rust` is a narrower query than `cap:code` and not a
 * wider one.
 *
 * An EMPTY `pos` means the conjunct constrains nothing, and the caller reads
 * that as "every document" — the original's default `DocsQuery`, and the
 * reason `NOT tag:x` on its own excludes rather than includes. */
static kbc_status conjunct_set(kbc_store *store, const char *corpus,
                               const kbc_facet_atom *atoms, size_t start,
                               size_t end, app_pathset *pos_out,
                               app_pathset *neg_out, bool *constrained,
                               bool *index_only, kbc_err *err) {
  memset(pos_out, 0, sizeof(*pos_out));
  memset(neg_out, 0, sizeof(*neg_out));
  *constrained = false;
  *index_only = false;
  app_pathset tags;
  app_pathset caps;
  memset(&tags, 0, sizeof(tags));
  memset(&caps, 0, sizeof(caps));
  bool have_tags = false;
  bool have_caps = false;
  bool want_index = false;
  bool drop_index = false;
  kbc_status s = KBC_OK;
  for (size_t i = start; s == KBC_OK && i < end; i++) {
    const kbc_facet_atom *at = &atoms[i];
    if (at->key == KBC_FACET_INDEX) {
      if (strcmp(at->value, "index_only") == 0) {
        want_index = true;
      } else {
        drop_index = true;
      }
      continue;
    }
    const bool is_tag = at->key == KBC_FACET_TAG;
    const char *key = is_tag ? "tags" : "caps";
    app_pathset part;
    s = pset_from_store(store, corpus, key, at->value, &part, err);
    if (kbc_failed(s)) break;
    /* A negated literal is an EXCLUSION (query.rs:290 exclude_tags,
     * query.rs:302 exclude_caps): it drops the documents carrying the value
     * and never widens the conjunct, so it never makes it constrained. */
    if (at->negated) {
      app_pathset next;
      s = pset_merge(neg_out, &part, false, &next, err);
      pset_free(&part);
      if (kbc_failed(s)) break;
      pset_free(neg_out);
      *neg_out = next;
      continue;
    }
    app_pathset *acc = is_tag ? &tags : &caps;
    const bool have = is_tag ? have_tags : have_caps;
    /* tags union (any-of), caps intersect (all-of). */
    const bool combine = have && !is_tag;
    app_pathset next;
    s = pset_merge(acc, &part, combine, &next, err);
    pset_free(&part);
    if (kbc_failed(s)) break;
    pset_free(acc);
    *acc = next;
    if (is_tag) {
      have_tags = true;
    } else {
      have_caps = true;
    }
  }
  if (s == KBC_OK && want_index) {
    app_pathset ipages;
    s = index_page_set(store, corpus, &ipages, err);
    if (s == KBC_OK) {
      app_pathset next;
      s = pset_merge(have_tags ? &tags : &ipages, have_caps ? &caps : &ipages,
                     have_tags || have_caps, &next, err);
      pset_free(&ipages);
      if (s == KBC_OK) {
        pset_free(&tags);
        pset_free(&caps);
        tags = next;
        have_tags = true;
      }
    }
  }
  if (s == KBC_OK && drop_index) {
    app_pathset ipages;
    s = index_page_set(store, corpus, &ipages, err);
    if (s == KBC_OK) {
      app_pathset next;
      s = pset_merge(neg_out, &ipages, false, &next, err);
      pset_free(&ipages);
      if (s == KBC_OK) {
        pset_free(neg_out);
        *neg_out = next;
      }
    }
  }
  if (s == KBC_OK && have_tags && have_caps) {
    app_pathset next;
    s = pset_merge(&tags, &caps, true, &next, err);
    pset_free(&tags);
    pset_free(&caps);
    if (s == KBC_OK) tags = next;
  }
  if (kbc_failed(s)) {
    pset_free(&tags);
    pset_free(&caps);
    return s;
  }
  /* Only the tags accumulator survives when there were no caps; `caps` is
   * empty in that case and merging it would intersect everything away. */
  *constrained = have_tags || have_caps || want_index;
  *index_only = want_index;
  if (have_tags) {
    *pos_out = tags;
    pset_free(&caps);
  } else if (have_caps) {
    *pos_out = caps;
    pset_free(&tags);
  } else {
    pset_free(&tags);
    pset_free(&caps);
  }
  return KBC_OK;
}

/* The whole overlay, resolved once per query. Returns the index doc ids the
 * facets admit, ascending; an empty result means the filter admits nothing,
 * and the caller must answer zero rows.
 *
 * The walk at the end is where the boolean structure is applied, because that
 * is the only place that sees a document: for each document the evaluator
 * asks, per conjunct, "does this path satisfy it" — in the set, and not in
 * the exclusion set — and a document is admitted when ANY conjunct says yes.
 * That is query.rs:246 verbatim. */
static kbc_status app_facets(void *ctx, const char *corpus,
                             const kbc_facets *facets, uint32_t **ids_out,
                             size_t *n_out, kbc_err *err) {
  kbc_app *app = ctx;
  *ids_out = NULL;
  *n_out = 0;
  if (app == NULL || facets == NULL)
    return kbc_err_set(err, KBC_ERR_INVALID, "facet overlay: null argument");

  kbc_strlist corpora;
  kbc_strlist_init(&corpora);
  kbc_status s = KBC_OK;
  if (corpus != NULL) {
    s = kbc_strlist_push(&corpora, corpus);
  } else {
    /* No corpus filter: the facets are evaluated in every corpus, because a
     * membership set is doc ids and a doc id names exactly one corpus. */
    s = kbc_store_list_corpora(app->store, &corpora, err);
  }
  if (kbc_failed(s)) {
    kbc_strlist_free(&corpora);
    return s;
  }

  const size_t nc = corpora.len;
  const size_t nj = facets->n_conj;
  /* One positive set, one exclusion set and one "does this conjunct constrain
   * anything at all" flag per (corpus, conjunct). The flag is what separates
   * "this conjunct names no facet, so it matches every document" from "this
   * conjunct names a tag nobody carries, so it matches none" — the whole
   * difference between an OR branch that is wide and one that is empty. */
  app_pathset *pos = calloc(nc * nj > 0 ? nc * nj : 1, sizeof(*pos));
  app_pathset *neg = calloc(nc * nj > 0 ? nc * nj : 1, sizeof(*neg));
  bool *constrained = calloc(nc * nj > 0 ? nc * nj : 1, sizeof(*constrained));
  bool *index_only = calloc(nc * nj > 0 ? nc * nj : 1, sizeof(*index_only));
  if (pos == NULL || neg == NULL || constrained == NULL || index_only == NULL) {
    free(index_only);
    free(constrained);
    free(pos);
    free(neg);
    kbc_strlist_free(&corpora);
    return kbc_err_set(err, KBC_ERR_NOMEM, "facet overlay: %zu x %zu", nc, nj);
  }
  for (size_t c = 0; s == KBC_OK && c < nc; c++) {
    for (size_t j = 0; s == KBC_OK && j < nj; j++) {
      s = conjunct_set(app->store, corpora.items[c], facets->atoms,
                       facets->conj[j], facets->conj[j + 1], &pos[c * nj + j],
                       &neg[c * nj + j], &constrained[c * nj + j],
                       &index_only[c * nj + j], err);
    }
  }
  if (kbc_failed(s)) {
    for (size_t i = 0; i < nc * nj; i++) {
      pset_free(&pos[i]);
      pset_free(&neg[i]);
    }
    free(pos);
    free(neg);
    free(constrained);
    free(index_only);
    kbc_strlist_free(&corpora);
    return s;
  }

  /* The store answers in paths; the searcher speaks in doc ids. One walk of
   * the index's doc table turns the paths into ids, in ascending order
   * because the walk is ascending — which is what the searcher's binary
   * search over the membership set needs. This is the same shape as
   * graph_in_degrees above: the store cannot name a doc id, so the join
   * starts on the index side. */
  const uint32_t n_docs = kbc_index_doc_count(app->index);
  uint32_t *ids = calloc(n_docs > 0 ? n_docs : 1, sizeof(*ids));
  if (ids == NULL) {
    for (size_t i = 0; i < nc * nj; i++) {
      pset_free(&pos[i]);
      pset_free(&neg[i]);
    }
    free(pos);
    free(neg);
    free(constrained);
    free(index_only);
    kbc_strlist_free(&corpora);
    return kbc_err_set(err, KBC_ERR_NOMEM, "facet overlay: %u documents",
                       (unsigned)n_docs);
  }
  size_t kept = 0;
  for (uint32_t d = 0; d < n_docs; d++) {
    const kbc_doc_meta *m = kbc_index_doc(app->index, d);
    if (m == NULL || m->corpus == NULL || m->path == NULL) continue;
    /* Each set belongs to ONE corpus: a path alone is not a document, and two
     * corpora may carry the same relative path. */
    for (size_t c = 0; c < nc; c++) {
      if (strcmp(corpora.items[c], m->corpus) != 0) continue;
      for (size_t j = 0; j < nj; j++) {
        const app_pathset *p = &pos[c * nj + j];
        if (constrained[c * nj + j] && !pset_has(p, m->path) &&
            !(index_only[c * nj + j] && path_is_index(m->path)))
          continue;
        if (pset_has(&neg[c * nj + j], m->path)) continue;
        ids[kept++] = d;
        break;
      }
      break;
    }
  }
  for (size_t i = 0; i < nc * nj; i++) {
    pset_free(&pos[i]);
    pset_free(&neg[i]);
  }
  free(pos);
  free(neg);
  free(constrained);
  free(index_only);
  kbc_strlist_free(&corpora);
  if (kept == 0) {
    free(ids);
    KBC_LOGD("search: no document carries the requested facets");
    return KBC_OK;
  }
  *ids_out = ids;
  *n_out = kept;
  return KBC_OK;
}


kbc_status kbc_app_search(kbc_app *app, kbc_arena *a, const kbc_query *q,
                          kbc_search_result *out, kbc_err *err) {
  if (!app || !a || !q || !out) {
    return kbc_err_set(err, KBC_ERR_INVALID, "kbc_app_search: NULL argument");
  }
  if (!q->q || q->q[0] == '\0') {
    return kbc_err_set(err, KBC_ERR_INVALID, "query text is empty");
  }
  if (strlen(q->q) > (size_t)KBC_MAX_QUERY_LEN) {
    return kbc_err_set(err, KBC_ERR_INVALID, "query of %zu bytes exceeds %u",
                       strlen(q->q), (unsigned)KBC_MAX_QUERY_LEN);
  }
  memset(out, 0, sizeof(*out));

  const int64_t t0 = kbc_now_ns();

  /* `since:` is resolved here, not in the index: the mtime is the store's.
   * An unparsable value is an error, never a silently ignored filter. */
  int64_t since_ns = 0;
  kbc_status pre = query_since_ns(q->q, &since_ns, err);
  if (kbc_failed(pre)) return pre;
  if (q->since_ns > since_ns) since_ns = q->since_ns;
  /* The query vector is computed BEFORE the read lock, and the reason is the
   * lock, not the ordering. Everything this needs — the embedder, the cache,
   * both immutable after open — is stable without it, and the only thing the
   * lock guards is {index, vec}, which the query vector does not depend on.
   * So the 50-100 ms sidecar round trip (or the 19 us cache scan that avoids
   * it) is no longer held against a reindex that is waiting to swap the index
   * under the WRITE lock: the old comment here said the read lock kept a slow
   * sidecar from stalling that swap, and holding nothing keeps that promise
   * strictly better. The health test moved with the call: with a cache a dead
   * sidecar must not block a HIT, and kbc_embed_query already fails a miss
   * against a reaped child in one exchange_raw call. */
  kbc_arena *ea = NULL;
  float *vec = NULL;
  size_t vec_len = 0;
  if (app->embed) {
    ea = kbc_arena_new(16u * 1024u);
    if (ea) {
      vec = embed_query(app, ea, q->q, &vec_len, NULL);
    }
  }

  pthread_rwlock_rdlock(&app->lock);

  /* The searcher is handed no resolver: the rows come back with their corpus
   * and path already copied into the caller's arena, which is everything the
   * batch needs. Resolving here, after the index lock is released, is what
   * lets one query take the store's lock once. */

  kbc_err local;
  kbc_err_reset(&local);
  kbc_searcher *s = kbc_searcher_new(app->index, NULL, NULL, &local);
  if (!s) {
    if (ea) {
      kbc_arena_free(ea);
    }
    pthread_rwlock_unlock(&app->lock);
    return kbc_err_set(err, local.status ? local.status : KBC_ERR_INTERNAL,
                       "kbc_searcher_new: %s",
                       local.msg[0] ? local.msg : "failed");
  }

  /* The searcher scores the vector lane against the document embeddings, and
   * search.h has no way to learn them; search.c exports this setter and
   * documents the gap. Without this call the lane never runs and every
   * semantic query degrades to an empty result. app->vec is BORROWED for the
   * search, and the read lock held here is what keeps it alive. */
  (void)kbc_searcher_set_vecstore(s, app->vec, &local);

  /* The metadata overlay. The facets live in the store, so the searcher
   * cannot evaluate them and app is the only layer that can: it holds both
   * the store (which answers in corpus-relative paths) and the index (whose
   * doc ids the searcher filters on). Same contract-gap shape as the
   * vecstore setter above, and the same read lock keeping app->index stable
   * for the duration. */
  (void)kbc_searcher_set_facet_fn(s, app_facets, app, &local);

  /* The effective query: the caller's, plus the graph the store knows about.
   * The weight comes from the config, read once per search, and 0.0 means the
   * graph is off — so nothing is fetched and every score is byte-identical to
   * a build with no edge table. (0, 4] is enforced by the config layer. */
  const double weight = app->cfg != NULL ? app->cfg->graph_boost : 0.0;
  kbc_query eq = *q;
  uint32_t *deg_ids = NULL;
  uint32_t *deg_vals = NULL;
  size_t deg_len = 0;
  if (weight > 0.0 && q->corpus != NULL) {
    kbc_err gerr;
    kbc_err_reset(&gerr);
    if (kbc_failed(graph_in_degrees(app, q->corpus, &deg_ids, &deg_vals,
                                    &deg_len, &gerr))) {
      KBC_LOGW("search: graph boost off for this query: %s", gerr.msg);
      deg_ids = NULL;
      deg_vals = NULL;
      deg_len = 0;
    } else {
      eq.graph_boost_weight = weight;
      eq.in_deg_doc_ids = deg_ids;
      eq.in_deg = deg_vals;
      eq.in_deg_len = deg_len;
      KBC_LOGD("search: graph boost weight %.3f over %zu linked documents",
               weight, deg_len);
    }
  }

  kbc_status rc_st = kbc_search_run(s, a, &eq, vec, vec_len, out, err);
  kbc_searcher_free(s);
  free(deg_ids);
  free(deg_vals);
  if (ea) {
    kbc_arena_free(ea);
  }
  pthread_rwlock_unlock(&app->lock);

  if (kbc_failed(rc_st)) {
    return rc_st;
  }

  kbc_status rs = resolve_rows(app, a, out, since_ns, err);
  if (kbc_failed(rs)) {
    return rs;
  }

  out->took_us = (kbc_now_ns() - t0) / 1000;
  /* The searcher knows what was asked for and what ran; degraded means a lane
   * the caller wanted could not run, which is not the same as "this build has
   * no embedder". A mode=keyword query never wanted the vector lane. */
  if (out->degraded) {
    atomic_fetch_add_explicit(&app->st_degraded, 1, memory_order_relaxed);
  }
  atomic_fetch_add_explicit(&app->st_searches, 1, memory_order_relaxed);
  return KBC_OK;
}

/* The redirect for a stale id, and WHERE it lives.
 *
 * A rename mints a new id, so every id handed out BEFORE it — a bookmark, a
 * URL, an id typed by a person — is a reference to a document that still
 * exists under a name the caller does not know. kbc_store_moves_lookup walks
 * the completed-move chain (64 hops, cycle-guarded, and it refuses to follow
 * an UNSTAMPED row), so the answer is always a name the rename actually
 * reached.
 *
 * It is HERE, at the app's id-resolution entry point, and not inside
 * kbc_store_get_artifact, because the store cannot tell the two reasons a row
 * is absent apart: a typo and a rename both read as "no such id", and only
 * the caller knows which one it has. A caller that wanted the raw miss can
 * say so — this is the app's public getter, and the store's is one layer
 * under it.
 *
 * It costs one extra round trip, and only on a MISS: an id that resolves is
 * never walked, so the hot path is the one call it already was. */
static kbc_status get_artifact_following_moves(kbc_app *app, kbc_arena *a,
                                               const char *id, bool with_source,
                                               kbc_artifact *out, kbc_err *err) {
  kbc_err local;
  kbc_err_reset(&local);
  kbc_status s =
      kbc_store_get_artifact(app->store, a, id, with_source, out, &local);
  if (s != KBC_ERR_NOTFOUND) {
    if (kbc_failed(s)) return kbc_err_set(err, s, "get %s: %s", id, local.msg);
    return KBC_OK;
  }
  /* Held, not re-derived: a chain that leads nowhere has to report the miss
   * the caller would have seen without the redirect, because "no such
   * document" is the honest answer for an id that never moved. */
  kbc_err miss = local;

  kbc_strlist hops;
  kbc_strlist_init(&hops);
  kbc_err_reset(&local);
  s = kbc_store_moves_lookup(app->store, id, &hops, &local);
  if (kbc_failed(s)) {
    kbc_strlist_free(&hops);
    return kbc_err_set(err, s, "get %s: the moves table: %s", id, local.msg);
  }
  if (hops.len == 0) {
    kbc_strlist_free(&hops);
    return kbc_err_set(err, miss.status, "%s", miss.msg);
  }
  /* The walk appends each hop and the chain's destination LAST, so the tail
   * is where the document ended up: a bookmark made before TWO renames has
   * to arrive at the second, not stop at the first. */
  const char *home = hops.items[hops.len - 1];
  kbc_err_reset(&local);
  s = kbc_store_get_artifact(app->store, a, home, with_source, out, &local);
  if (s == KBC_OK) {
    KBC_LOGI("get %s: the document was renamed, resolving to %s", id, home);
    kbc_strlist_free(&hops);
    return KBC_OK;
  }
  /* `home` dies with the list, so the message is composed BEFORE the free.
   * A tail that is itself absent is the same honest miss: the document was
   * renamed again by something that did not record it, or it is gone, and
   * neither is something a redirect may paper over. */
  kbc_err wrapped;
  kbc_err_reset(&wrapped);
  if (s == KBC_ERR_NOTFOUND) {
    kbc_err_set(&wrapped, miss.status, "%s", miss.msg);
  } else {
    kbc_err_set(&wrapped, s, "get %s: renamed to %s: %s", id, home, local.msg);
  }
  kbc_strlist_free(&hops);
  return kbc_err_set(err, wrapped.status, "%s", wrapped.msg);
}

kbc_status kbc_app_get_artifact(kbc_app *app, kbc_arena *a, const char *id,
                                bool with_source, kbc_artifact *out,
                                kbc_err *err) {
  if (!app || !a || !id || !out) {
    return kbc_err_set(err, KBC_ERR_INVALID, "kbc_app_get_artifact: NULL "
                                            "argument");
  }
  if (!kbc_id_is_valid(id)) {
    return kbc_err_set(err, KBC_ERR_INVALID, "%s: not a %u-hex artifact id", id,
                       (unsigned)KBC_MAX_ID_LEN);
  }
  return get_artifact_following_moves(app, a, id, with_source, out, err);
}

kbc_status kbc_app_list_artifacts(kbc_app *app, kbc_arena *a, const char *corpus,
                                  kbc_kind kind, size_t limit, size_t offset,
                                  kbc_artifact **out, size_t *n_out,
                                  kbc_err *err) {
  if (!app || !a || !out || !n_out) {
    return kbc_err_set(err, KBC_ERR_INVALID, "kbc_app_list_artifacts: NULL "
                                            "argument");
  }
  *out = NULL;
  *n_out = 0;

  char **ids = NULL;
  size_t n = 0;
  kbc_status s =
      kbc_store_list_artifact_ids(app->store, corpus, kind, limit, offset, &ids,
                                  &n, err);
  if (kbc_failed(s)) {
    return s;
  }
  if (n == 0) {
    free(ids);
    return KBC_OK;
  }

  kbc_artifact *rows = (kbc_artifact *)kbc_arena_calloc(a, n, sizeof(*rows));
  if (!rows) {
    for (size_t i = 0; i < n; i++) {
      free(ids[i]);
    }
    free(ids);
    return kbc_err_set(err, KBC_ERR_NOMEM, "%zu artifact rows", n);
  }
  size_t kept = 0;
  for (size_t i = 0; i < n; i++) {
    kbc_err local;
    kbc_err_reset(&local);
    if (kbc_failed(kbc_store_get_artifact(app->store, a, ids[i], false,
                                          &rows[kept], &local))) {
      /* Deleted between the id list and the read: skip it, keep the page
       * dense rather than reporting a row with no record behind it. */
      KBC_LOGW("list: artifact %s vanished mid-page: %s", ids[i], local.msg);
      continue;
    }
    kept++;
  }
  for (size_t i = 0; i < n; i++) {
    free(ids[i]);
  }
  free(ids);
  *out = rows;
  *n_out = kept;
  return KBC_OK;
}

/* --------------------------------------------------------------- watcher */

/* Runs on the watcher thread. It hands off to a rebuild, so a burst of
 * inotify events has to be debounced upstream — watcher.c coalesces per
 * (corpus, path) for cfg->watcher_debounce_ms, which is what keeps this from
 * being a rebuild per write. */
static void watcher_cb(void *user, const char *type, const char *json_payload) {
  kbc_app *app = (kbc_app *)user;
  kbc_arena *a = kbc_arena_new(8192u);
  if (!a) {
    return;
  }
  kbc_err local;
  kbc_err_reset(&local);
  kbc_json *j = kbc_json_parse(a, json_payload, strlen(json_payload), &local);
  if (!j || !kbc_json_is(j, KBC_JSON_OBJ)) {
    KBC_LOGW("watcher event %s: unparsable payload: %s", type, local.msg);
    kbc_arena_free(a);
    return;
  }
  /* The event contract: `corpus` names the corpus, `path` is CORPUS-RELATIVE.
   * Everything below — the store, the index and the touch/remove path — is
   * keyed on exactly that, so the root is joined here and nowhere else. An
   * absolute path in the event would key every lookup on a path that does not
   * exist, and an edit would read as a removal. */
  const char *corpus = kbc_json_str(j, "corpus", "");
  const char *path = kbc_json_str(j, "path", "");
  if (corpus[0] == '\0' || path[0] == '\0') {
    KBC_LOGW("watcher event %s: no corpus/path in the payload", type);
    kbc_arena_free(a);
    return;
  }

  /* The payload's own flag is advisory; the filesystem is the authority. */
  bool removed = kbc_json_bool(j, "removed", false);
  const kbc_corpus_cfg *cc = kbc_config_corpus(app->cfg, corpus);
  if (cc) {
    kbc_str full;
    kbc_str_init(&full);
    if (!kbc_failed(kbc_str_printf(&full, "%s/%s", cc->path, path))) {
      removed = removed || !kbc_path_exists(full.ptr);
    }
    kbc_str_free(&full);
  }

  kbc_err ignored;
  kbc_err_reset(&ignored);
  kbc_status s = removed ? kbc_app_reindex_remove(app, corpus, path, &ignored)
                         : kbc_app_reindex_file(app, corpus, path, &ignored);
  if (kbc_failed(s)) {
    KBC_LOGW("watcher %s %s/%s: %s", type, corpus, path, ignored.msg);
  }
  kbc_arena_free(a);
}

kbc_status kbc_app_start_watcher(kbc_app *app, kbc_err *err) {
  if (!app) {
    return kbc_err_set(err, KBC_ERR_INVALID, "kbc_app_start_watcher: app is "
                                            "NULL");
  }
  if (app->watch) {
    return KBC_OK; /* idempotent */
  }
  kbc_err local;
  kbc_err_reset(&local);
  app->watch = kbc_watcher_start(app->cfg, watcher_cb, app, &local);
  if (!app->watch) {
    return kbc_err_set(err, local.status ? local.status : KBC_ERR_IO,
                       "kbc_watcher_start: %s",
                       local.msg[0] ? local.msg : "failed");
  }
  return KBC_OK;
}

void kbc_app_stop_watcher(kbc_app *app) {
  if (!app || !app->watch) {
    return;
  }
  kbc_watcher_stop(app->watch);
  app->watch = NULL;
}

/* ------------------------------------------------------------------- bus */

uint64_t kbc_app_subscribe(kbc_app *app, kbc_event_fn fn, void *user) {
  if (!app || !fn) {
    return 0;
  }
  uint64_t id = 0;
  pthread_mutex_lock(&app->bus_lock);
  for (size_t i = 0; i < KBC_APP_MAX_SUBS; i++) {
    if (app->subs[i].fn != NULL) {
      continue;
    }
    id = app->next_sub_id++;
    app->subs[i].fn = fn;
    app->subs[i].user = user;
    app->subs[i].id = id;
    break;
  }
  pthread_mutex_unlock(&app->bus_lock);
  if (id == 0) {
    KBC_LOGW("event bus: all %u subscription slots are in use",
             (unsigned)KBC_APP_MAX_SUBS);
  }
  return id;
}

void kbc_app_unsubscribe(kbc_app *app, uint64_t id) {
  if (!app || id == 0) {
    return;
  }
  pthread_mutex_lock(&app->bus_lock);
  for (size_t i = 0; i < KBC_APP_MAX_SUBS; i++) {
    if (app->subs[i].fn != NULL && app->subs[i].id == id) {
      memset(&app->subs[i], 0, sizeof(app->subs[i]));
      break;
    }
  }
  pthread_mutex_unlock(&app->bus_lock);
}

void kbc_app_publish(kbc_app *app, const char *type, const char *json_payload) {
  if (!app || !type || !json_payload) {
    return;
  }
  /* Snapshot the subscriber list and drop the lock before calling. A
   * callback is forbidden from re-entering kbc_app, but the bus must not
   * deadlock if one subscribes or unsubscribes while it is being called. */
  kbc_app_sub snap[KBC_APP_MAX_SUBS];
  size_t n = 0;
  pthread_mutex_lock(&app->bus_lock);
  for (size_t i = 0; i < KBC_APP_MAX_SUBS && n < KBC_APP_MAX_SUBS; i++) {
    if (app->subs[i].fn != NULL) {
      snap[n] = app->subs[i];
      n++;
    }
  }
  pthread_mutex_unlock(&app->bus_lock);

  for (size_t i = 0; i < n; i++) {
    snap[i].fn(snap[i].user, type, json_payload);
  }
}

/* ----------------------------------------------------------------- stats */

kbc_status kbc_app_stats_get(kbc_app *app, kbc_app_stats *out, kbc_err *err) {
  if (!app || !out) {
    return kbc_err_set(err, KBC_ERR_INVALID, "kbc_app_stats_get: NULL "
                                            "argument");
  }
  memset(out, 0, sizeof(*out));
  out->artifacts_indexed =
      atomic_load_explicit(&app->st_indexed, memory_order_relaxed);
  out->reindex_runs = atomic_load_explicit(&app->st_runs, memory_order_relaxed);
  out->searches_served =
      atomic_load_explicit(&app->st_searches, memory_order_relaxed);
  out->searches_degraded =
      atomic_load_explicit(&app->st_degraded, memory_order_relaxed);
  out->last_reindex_ns =
      atomic_load_explicit(&app->st_last_ns, memory_order_relaxed);
  out->last_reindex_docs =
      atomic_load_explicit(&app->st_last_docs, memory_order_relaxed);
  out->last_reindex_us =
      atomic_load_explicit(&app->st_last_us, memory_order_relaxed);
  out->index_terms = atomic_load_explicit(&app->st_terms, memory_order_relaxed);
  out->index_docs = atomic_load_explicit(&app->st_docs, memory_order_relaxed);

  int64_t bytes = 0;
  kbc_err local;
  kbc_err_reset(&local);
  if (!kbc_failed(kbc_store_total_bytes(app->store, &bytes, &local))) {
    atomic_store_explicit(&app->st_bytes, bytes, memory_order_relaxed);
  }
  out->db_bytes = atomic_load_explicit(&app->st_bytes, memory_order_relaxed);
  /* The query cache. app->qcache is immutable after kbc_app_open, so reading
   * the pointer needs nothing; the entry count is a sampled number taken under
   * the cache's own lock, which is a lock this file does not hold anywhere
   * else. */
  out->query_cache_entries = (int64_t)kbc_query_cache_len(app->qcache);
  out->query_cache_hits =
      atomic_load_explicit(&app->st_qc_hits, memory_order_relaxed);
  out->query_cache_misses =
      atomic_load_explicit(&app->st_qc_misses, memory_order_relaxed);
  out->query_cache_drops =
      atomic_load_explicit(&app->st_qc_drops, memory_order_relaxed);
  return KBC_OK;
}

/* The store, for a reader that needs it. The app owns the handle and the store
 * owns its own mutex, so handing the pointer out does not hand out authority:
 * every store call still takes the store's lock internally. The one caller at
 * present is the httpd's anchor re-evaluation, which needs the comments and the
 * edge graph and has no other way to reach them. */
kbc_store *kbc_app_store(kbc_app *app) { return app != NULL ? app->store : NULL; }

/* The one query-cache number that is not a counter. Safe on NULL, so a caller
 * holding no app reads 0 rather than crashing. */
size_t kbc_app_query_cache_capacity(const kbc_app *app) {
  return app == NULL ? 0 : kbc_query_cache_capacity(app->qcache);
}

void kbc_app_set_index(kbc_app *app, kbc_index *ix) {
  if (!app) {
    kbc_index_free(ix);
    return;
  }
  if (!ix) {
    return;
  }
  /* Takes ownership of `ix`; the previous one dies here, under the lock, so
   * no reader is still inside it. */
  pthread_rwlock_wrlock(&app->lock);
  kbc_index *old = app->index;
  app->index = ix;
  atomic_store_explicit(&app->st_indexed, (int64_t)kbc_index_doc_count(ix),
                        memory_order_relaxed);
  atomic_store_explicit(&app->st_docs, (int64_t)kbc_index_doc_count(ix),
                        memory_order_relaxed);
  atomic_store_explicit(&app->st_terms, (int64_t)kbc_index_term_count(ix),
                        memory_order_relaxed);
  if (old) {
    kbc_index_free(old);
  }
  pthread_rwlock_unlock(&app->lock);
}
