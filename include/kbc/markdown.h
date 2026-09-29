/* markdown.h — Markdown to page HTML, for serving a .md artifact.
 *
 * WHY THIS IS NARROWER THAN THE ORIGINAL, ON PURPOSE. The Rust original
 * renders through comrak, a full CommonMark + GFM engine (kb-core/Cargo.toml:82
 * pins `comrak = "0.52"` with strikethrough, tables, autolink, tasklists,
 * footnotes, superscript and description lists, and `render.unsafe = true`).
 * kb-core/src/markdown.rs is the ~800-line WRAPPER around it; the grammar
 * underneath is comrak's, which is tens of thousands of lines in a language
 * kb-c does not use.
 *
 * The operator's decision, 2026-09-28, is that kb-c does NOT take a
 * third-party Markdown dependency: it links sqlite3, libm and libpthread and
 * nothing else. So the renderer here is hand-rolled and DELIBERATELY narrower,
 * and the divergence is recorded in INVENTORY.md and README.md rather than
 * left for a reader to discover. This is the same posture the port already
 * takes for `cap:` and for the refused `scope:` and `since:` — a stated
 * departure beats a silent one.
 *
 * WHAT IS SUPPORTED: ATX and setext headings to six levels, ordered and
 * unordered nested lists, fenced and indented code, GFM pipe tables, links,
 * images, autolinks, emphasis and strong (both `*` and `_` delimited), GFM
 * strikethrough, GFM task lists, hard line breaks, HTML blocks, the
 * `> [!type] Title` callout pre-pass, and raw HTML passthrough.
 *
 * WHAT IS NOT, and each of these renders as ordinary text rather than as its
 * construct: reference links and link reference definitions (`[a][b]`,
 * `[a]: url`), footnotes (`[^1]`), superscript (`^x^` is not a thing in
 * CommonMark and is left alone), description lists, and entity references. A
 * document using them still renders; it just does not get the extension's
 * rendering. Reference links are the largest of these by volume — they are the
 * single biggest source of divergence from the original — so a document that
 * relies on them will look wrong, and that is the documented cost of taking no
 * third-party dependency rather than a defect.
 *
 * Entity references deserve their own line because the behaviour is a
 * pass-through, not a decode: `&copy;` reaches the page as `&copy;` and the
 * BROWSER resolves it, so named entities mostly look right by accident, while
 * an unrecognised `&nope;` is left as a bare ampersand sequence. Resolving
 * them here would need the HTML5 named-character-reference table — some two
 * thousand entries — which is a data dependency the no-third-party rule
 * excludes. Numeric references `&#169;` are decoded and are not affected.
 */
#ifndef KBC_MARKDOWN_H
#define KBC_MARKDOWN_H

#include "kbc/kbc.h"
#include "kbc/mem.h"

#ifdef __cplusplus
extern "C" {
#endif

/* One rendered page: the full document, ready to write. Both fields are
 * KBC_OWN `char *` and the caller frees them with `free()` — NOT
 * kbc_str_free, which takes a `kbc_str *` this is not.
 *
 * `title` is the RAW title, the same string kbc_markdown_title returns. It is
 * NOT the text the page's <title> element carries: a title containing markup,
 * e.g. an h1 of `# <b>x</b>`, is returned raw and lands on the page ESCAPED as
 * `&lt;b&gt;x&lt;/b&gt;`. A caller comparing the two is comparing a raw string
 * with an escaped one and will find them different. Compare against the page
 * only after escaping, or do not compare. */
typedef struct {
  char *html;  /* KBC_OWN */
  char *title; /* KBC_OWN */
} kbc_markdown_page;

/* Title precedence, matching the original: the frontmatter `title:` key, else
 * the first `# ` ATX heading, else the literal "Untitled". Exposed separately
 * because the artifact list shows titles and must not render a whole document
 * to learn one. */
char *kbc_markdown_title(const char *src, size_t len);

/* Renders `src` to a complete HTML page: the doctype, head, embedded
 * stylesheet, and a body carrying the callout bar and the document body. The
 * original's wrapper is reproduced byte for byte, including the quirks a
 * byte-comparison would catch (the viewport meta has no trailing newline
 * before <title>, <meta charset> has no self-closing slash, and the stylesheet
 * is wrapped in newlines on its own). Rendered into `a` when non-NULL so a
 * caller serving many documents pays one allocation; otherwise into a fresh
 * KBC_OWN block the caller must free.
 *
 * `src` is BYTES, not a NUL-terminated string, and may be up to
 * KBC_MAX_ARTIFACT_BYTES. A document that is not valid UTF-8 is
 * KBC_ERR_PARSE and renders nothing: the caller must not serve half a page.
 *
 * Raw HTML in the source passes through, which is the original's
 * `render.unsafe = true` (markdown.rs:58). This function does not contain it
 * and cannot: the contract is written so that the two halves are read
 * together, because the containment is the serving route's and it DIFFERS BY
 * SURFACE.
 *
 * The parent-origin route sends `Content-Security-Policy: sandbox`, so a
 * document's scripts do not run there. The artifact subdomain sends only
 * `frame-ancestors <parent>`, which is the original's design
 * (routes/artifact.rs) and is a real boundary — but a DIFFERENT and weaker one.
 * On the subdomain a hostile document's script DOES execute, and what contains
 * it is origin isolation: the artifact id is part of the HOSTNAME, so each
 * artifact is its own origin to a browser, and a script in one cannot reach
 * another's DOM.
 *
 * That makes the subdomain's hostname scheme load-bearing rather than
 * cosmetic, and it has one limit worth knowing: cookies are scoped to the
 * REGISTRABLE domain, not the origin, so a document served at
 * `abc.artifacts.localhost` can set a cookie that `def.artifacts.localhost`
 * will send. Origin isolation holds for the DOM; it does not hold for
 * anything cookie-carried. Today nothing authenticates on the subdomain, so
 * nothing is carried — but a future change that puts a session cookie in play
 * there invalidates the boundary, and that is the point at which the subdomain
 * needs `sandbox` too. */
kbc_status kbc_markdown_render(const char *src, size_t len, kbc_arena *a,
                               kbc_markdown_page *out, kbc_err *err);

#ifdef __cplusplus
}
#endif

#endif /* KBC_MARKDOWN_H */
