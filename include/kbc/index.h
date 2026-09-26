/* index.h — the inverted index: the reason kb-c is faster than kb.
 *
 * kb's Rust build answers a keyword query through lancedb → arrow → datafusion.
 * Every keystroke pays three layers of machinery it does not need. kb-c keeps
 * its own index: an open-addressed term table plus delta+varint postings in one
 * file, mmap'd whole at query time. Scoring reads bytes it never copies.
 *
 * Lifecycle: build (mutable) -> save (immutable on disk) -> open (immutable in
 * memory). A running daemon never mutates a live index; kbc_app swaps a whole
 * new pointer under a write lock instead, so readers never see a half-built
 * index and there is no reader/writer race in here.
 */
#ifndef KBC_INDEX_H
#define KBC_INDEX_H

#include "kbc/kbc.h"
#include "kbc/mem.h"
#include "kbc/parse.h"
#include "kbc/types.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct kbc_index kbc_index;

/* Document metadata the index owns, so a search hit is answerable without
 * touching SQLite. BORROWED from the index. */
typedef struct {
  const char *corpus;
  const char *path;
  const char *title;
  uint8_t kind;
  uint32_t token_count; /* total tokens, the BM25 length norm */
} kbc_doc_meta;

kbc_index *kbc_index_new(void);
void kbc_index_free(kbc_index *ix);

/* ------------------------------------------------------------- building -- */

/* Discards any previous build. Doc ids are assigned by the caller and must be
 * dense and monotonic from 0, in the order documents are added. */
kbc_status kbc_index_begin_build(kbc_index *ix, kbc_err *err);

/* Adds one document. `toks` must come from kbc_tokenize over the document's
 * searchable text (title counted twice by convention — the caller decides).
 * A doc with zero tokens is recorded (it stays findable by path) but adds no
 * postings. */
kbc_status kbc_index_add_doc(kbc_index *ix, uint32_t doc_id, const char *corpus,
                             const char *path, const char *title, kbc_kind kind,
                             const kbc_tokens *toks, kbc_err *err);

/* Seals the build. After this the index is immutable and searchable. */
kbc_status kbc_index_end_build(kbc_index *ix, kbc_err *err);

/* Atomic save: temp file + fsync + rename. Fails rather than writing a file
 * that kbc_index_open would reject. */
kbc_status kbc_index_save(const kbc_index *ix, const char *path, kbc_err *err);

/* Opens a saved index read-only. Returns KBC_ERR_PARSE (message names the
 * expected/actual format) on a magic or version mismatch, so an index written
 * by an older build is a clear error, not a crash. */
kbc_index *kbc_index_open(const char *path, kbc_err *err);

/* ----------------------------------------------------------- inspecting -- */

uint32_t kbc_index_doc_count(const kbc_index *ix);
uint32_t kbc_index_term_count(const kbc_index *ix);
uint64_t kbc_index_posting_count(const kbc_index *ix);
double kbc_index_avg_doclen(const kbc_index *ix);
size_t kbc_index_heap_bytes(const kbc_index *ix);
/* NULL when doc_id is out of range. */
const kbc_doc_meta *kbc_index_doc(const kbc_index *ix, uint32_t doc_id);
uint32_t kbc_index_id_of(const kbc_index *ix, const char *corpus,
                         const char *path); /* UINT32_MAX when absent */

/* ------------------------------------------------------------- querying -- */

/* Okapi BM25 over the postings. k1/b come from the config (1.2 / 0.75).
 * `out` is appended to, sorted score-desc, truncated to `limit`
 * (clamped to KBC_MAX_HITS). Only documents that match at least one term are
 * returned — there is no "return everything" fallback, because a search that
 * invents results is worse than an empty one. */
kbc_status kbc_index_bm25(const kbc_index *ix, const kbc_tokens *query,
                          double k1, double b, size_t limit, kbc_hits *out,
                          kbc_err *err);

/* Terms in the index whose prefix matches `prefix` (a trailing `*` in a user
 * query is stripped before this is called). ARENA copies, sorted. */
kbc_status kbc_index_expand_prefix(const kbc_index *ix, kbc_arena *a,
                                   const char *prefix, kbc_strlist *out,
                                   kbc_err *err);

#ifdef __cplusplus
}
#endif

#endif /* KBC_INDEX_H */
