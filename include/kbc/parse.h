/* parse.h — Markdown/HTML -> blocks with stable anchor ids, and the tokenizer.
 *
 * The parse is deliberately a *reader* parse, not a CommonMark implementation:
 * it extracts the prose, the heading structure and the explicit element ids an
 * author put in the file. It never rewrites the file; the bytes on disk stay
 * the source of truth, which is the invariant the whole system rests on.
 */
#ifndef KBC_PARSE_H
#define KBC_PARSE_H

#include "kbc/kbc.h"
#include "kbc/mem.h"
#include "kbc/types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Parses `text` (detected as Markdown or HTML by sniffing the first 1 KiB).
 * Returns an arena-scoped parse: kbc_parsed_blocks/title/anchor_index. */
kbc_parsed *kbc_parse(kbc_arena *a, const char *text, size_t len,
                      const char *rel_path, kbc_err *err);

const kbc_blocks *kbc_parsed_blocks(const kbc_parsed *p);
/* The first h1, else the first non-empty block, else the filename stem. */
const char *kbc_parsed_title(const kbc_parsed *p);
/* Index of an explicit id="" attribute found in the source, for anchor
 * resolution. Returns false when the id is not present. */
bool kbc_parsed_has_anchor(const kbc_parsed *p, const char *id);

/* ------------------------------------------------------------ tokenizer -- */

typedef struct {
  const char *text; /* KBC_ARENA, NUL-terminated, already case-folded */
  size_t len;
  uint32_t start;   /* byte offset into the source it came from */
  uint32_t end;
} kbc_token;

typedef struct {
  kbc_token *items; /* KBC_ARENA */
  size_t len, cap;
} kbc_tokens;

/* Splits on non-alphanumeric, case-folds, strips diacritics, drops terms
 * longer than KBC_MAX_TERM_LEN and 1-byte tokens, and applies the stopword
 * list (see KBC_STOPWORDS). Never returns a zero-length token. */
kbc_status kbc_tokenize(kbc_arena *a, const char *text, size_t len,
                        kbc_tokens *out, kbc_err *err);

/* The stopword set, exposed for tests: 60+ common English words plus the
 * markdown/HTML scaffolding words that carry no retrieval signal. */
extern const char *const KBC_STOPWORDS[];
extern const size_t KBC_STOPWORDS_LEN;

/* "how to fix the parser" -> "how-to-fix-the-parser" */
kbc_status kbc_slugify(kbc_arena *a, const char *text, size_t len,
                       kbc_str *out, kbc_err *err);

#ifdef __cplusplus
}
#endif

#endif /* KBC_PARSE_H */
