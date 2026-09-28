/* chunk.h — passage chunking: the sliding word window.
 *
 * A document is split into overlapping windows rather than stored as one row
 * per parsed block, because a single long paragraph makes one oversized chunk
 * that the embedder truncates away, and a windowed document is both embeddable
 * and findable by a term that appears only late in the text.
 *
 * The port is exact (kb-core/src/chunk.rs:40-84): whitespace-delimited WORDS,
 * a 280-word window with a 60-word overlap, chunk 0 the title passage rather
 * than a window, and a 512-chunk cap that keeps the first 512 and reports the
 * ORIGINAL count. That last one is the point of `total`: a caller logging "N
 * chunks" must report how many the document really produced, not how many
 * survived the cap.
 *
 * This is a leaf, like the original: no clock, no I/O, no map iteration, and
 * every string it returns lives in the caller's arena. A chunker that reads
 * the clock cannot be tested.
 */
#ifndef KBC_CHUNK_H
#define KBC_CHUNK_H

#include "kbc/kbc.h"
#include "kbc/mem.h"

#ifdef __cplusplus
extern "C" {
#endif

/* chunk.rs:19 and :23. A caller may override them; the defaults are what the
 * original ships. */
#define KBC_CHUNK_WORDS 280u
#define KBC_CHUNK_OVERLAP_WORDS 60u

/* indexer.rs:56. The cap exists so one pathological document cannot fill the
 * chunk table; it keeps the FIRST n, chunk 0 included. */
#define KBC_MAX_CHUNKS_PER_DOC 512u

typedef struct {
  uint32_t idx;     /* position in reading order, contiguous from 0 */
  const char *text; /* KBC_ARENA */
  size_t text_len;
} kbc_chunk;

typedef struct {
  kbc_chunk *items; /* KBC_ARENA */
  size_t len, cap;  /* `cap` is the allocation, `len` the kept count */
  size_t total;     /* chunks BEFORE the cap, not the kept count */
} kbc_chunks;

/* Splits `body` into windows and returns them in `out`.
 *
 * `title` and `headings` become chunk 0, trimmed and joined with a newline,
 * and chunk 0 is emitted ONLY when that is non-empty — so a body-only document
 * starts at window 1 being the first body window, with no empty leading chunk.
 * Body windows follow, each a run of at most `chunk_words` whitespace-
 * delimited words joined by a single ASCII space, so a newline inside a window
 * becomes a space. The step is `chunk_words - overlap_words`, floored at 1, so
 * a degenerate overlap still terminates instead of looping forever.
 *
 * `chunk_words` and `overlap_words` are the caller's; pass 0 to take the
 * defaults above. `cap` likewise, and 0 takes KBC_MAX_CHUNKS_PER_DOC.
 *
 * On failure nothing is written to `out` and `err` names what failed. */
kbc_status kbc_chunk_document(kbc_arena *a, const char *title,
                              const char *headings, const char *body,
                              size_t body_len, size_t chunk_words,
                              size_t overlap_words, size_t cap,
                              kbc_chunks *out, kbc_err *err);

#ifdef __cplusplus
}
#endif

#endif /* KBC_CHUNK_H */
