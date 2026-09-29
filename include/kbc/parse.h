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
#include "kbc/meta.h"
#include "kbc/types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Parses `text` (detected as Markdown or HTML by sniffing the first 1 KiB).
 * Returns an arena-scoped parse: kbc_parsed_blocks/title/anchor_index. */
kbc_parsed *kbc_parse(kbc_arena *a, const char *text, size_t len,
                      const char *rel_path, kbc_err *err);

const kbc_blocks *kbc_parsed_blocks(const kbc_parsed *p);
/* The document's title, in the order the original resolves it: the `<title>`
 * ELEMENT first, then a front-matter `title:`, then the first heading, then
 * the filename stem. The `<title>`-before-heading order is not a preference —
 * it is what parser.rs:345 does, and the parity harness found the cost of
 * getting it backwards: `canon/pm/00-summary.html` has its first `<h1>`
 * halfway down the page, so an h1-first resolver listed a four-page incident
 * report under a section heading, and that title is what the backlinks
 * surface names the document by. */
const char *kbc_parsed_title(const kbc_parsed *p);
/* Index of an explicit id="" attribute found in the source, for anchor
 * resolution. Returns false when the id is not present. */

/* Outbound links found in the source, in document order, each already
 * resolved to a corpus-relative path. KBC_ARENA.
 *
 * Markdown `[text](target)`, HTML `<a href="target">` and Obsidian
 * `[[target]]` / `[[target|alias]]` are all read, and all three resolve through
 * ONE normaliser, so `[[y.md#part]]` and `[y](y.md#part)` produce the identical
 * `y.md`. For a bare `[[target]]` the link's `text` is the target; an alias is
 * display text only and never a second identity. `![[embed]]` is an image, not
 * a link, and a construct inside a fenced code block is not a link at all.
 * A target is resolved against the document's own directory, so `../x/y.md`
 * becomes `x/y.md`; `..` may not escape the corpus root, and a target that
 * escapes is DROPPED rather than clamped — a link out of the corpus is not a
 * link to anything kb-c indexes. Absolute URLs, mailto:, tel: and anchors
 * (`#section`) are not corpus documents and are not links in this sense.
 * `rel` (the path kbc_parse was given) is what relative targets resolve
 * against; the caller decides whether a resolved target exists. */
typedef struct {
  const char *target; /* KBC_ARENA, corpus-relative, '/' separated */
  const char *text;   /* KBC_ARENA, the link's visible text, may be "" */
  size_t target_len;
} kbc_link;

typedef struct {
  kbc_link *items; /* KBC_ARENA */
  size_t len, cap;
} kbc_links;

/* May be empty; never NULL. A document with no links yields len == 0. */
const kbc_links *kbc_parsed_links(const kbc_parsed *p);

/* Filterable metadata declared by the document's own markup, e.g.
 * `<meta name="kb-tags" content="search, index">`. Only kb-* metas are
 * collected, the key is lowercased, and a comma-separated content attribute
 * yields ONE ENTRY PER ELEMENT, so the overlay can match a single value.
 * Markdown front matter in a leading --- fenced block is read the same way.
 * May be empty; never NULL. */
const kbc_metas *kbc_parsed_metas(const kbc_parsed *p);
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


/* Zeroes the vector so kbc_tokenize can be handed a fresh one. Required:
 * kbc_tokenize appends into whatever len/cap it finds and does not clear.
 * There is deliberately no kbc_tokens_free: the items array is arena-owned,
 * so it dies with the arena and freeing it separately would be a free() of
 * memory this library never allocated. */
void kbc_tokens_init(kbc_tokens *t);
/* Splits on non-alphanumeric, case-folds, strips diacritics, drops terms
 * longer than KBC_MAX_TERM_LEN and 1-byte tokens, and applies the stopword
 * list (see KBC_STOPWORDS). Never returns a zero-length token. */
kbc_status kbc_tokenize(kbc_arena *a, const char *text, size_t len,
                        kbc_tokens *out, kbc_err *err);

/* The stopword set, exposed for tests: 60+ common English words plus the
 * markdown/HTML scaffolding words that carry no retrieval signal. */
extern const char *const KBC_STOPWORDS[];

/* THE front-matter fence, as ONE predicate, because two callers disagreed and
 * the disagreement was a correctness bug rather than a style difference.
 *
 * `parse.c` and `markdown.c` each grew their own scan. `parse.c`'s skipped the
 * first line unconditionally and looked for a closing fence anywhere after it,
 * so a document that opened with prose, mentioned `kb-tags:` anywhere, and
 * contained a `---` line later had that facet INDEXED while the renderer
 * treated the whole document as body. `tag:x` then matched a document that
 * never declared the tag, which is the exact inverse of what
 * `include/kbc/meta.h` requires. It also accepted a `...` closing fence, which
 * the original does not, and did not strip a leading BOM.
 *
 * The rule, matching the original's `parse_frontmatter`: a leading UTF-8 BOM
 * is skipped, the opener must be exactly `---` at byte 0 after it, and the
 * block closes only on a line whose trimmed content is `---`. An unclosed
 * opener is NOT front matter.
 *
 * On true, `*start` and `*end` bound the fenced block and `*body` is the first
 * byte after the closing fence line. On false, only `*bom` and `*body` are
 * meaningful and `*body` is the BOM-stripped document — the whole thing, so a
 * caller that finds no front matter knows where the body begins. */
bool kbc_fm_fence(const char *s, size_t n, size_t *bom, size_t *start,
                  size_t *end, size_t *body);

/* "how to fix the parser" -> "how-to-fix-the-parser" */
kbc_status kbc_slugify(kbc_arena *a, const char *text, size_t len,
                       kbc_str *out, kbc_err *err);

#ifdef __cplusplus
}
#endif

#endif /* KBC_PARSE_H */
