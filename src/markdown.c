/* markdown.c — a .md artifact to a self-contained HTML page.
 *
 * WHY THIS IS NARROWER THAN THE ORIGINAL. markdown.rs:96 renders through
 * comrak, a full CommonMark + GFM engine; this file is a hand-rolled renderer
 * covering the subset include/kbc/markdown.h lists. The page WRAPPER around
 * the grammar is reproduced byte for byte, because a reader diffing a kb-c
 * page against a kb page should find nothing but the prose itself. The
 * divergence is recorded in the header, INVENTORY.md and README.md — a
 * stated departure beats one a reader has to discover.
 *
 * Two things are read together, and neither this file nor the header
 * neutralises the other:
 *
 *   1. Raw HTML in the source PASSES THROUGH, block level and inline. That
 *      is the original's `render.unsafe = true` (markdown.rs:58) and it is
 *      what keeps the inline `<template id="kb-prompt">` alive for the
 *      outbound scrub. It is contained by the `Content-Security-Policy:
 *      sandbox` the serving route sets, not here. Do not "fix" it by
 *      escaping or stripping: either silently breaks the prompt invariant
 *      the Rust module documents at length.
 *   2. kb-c links no syntax highlighter, so a code block carries the
 *      container chrome and NO inline token colours. markdown.rs pairs
 *      comrak with syntect; the stylesheet's comment about "syntect supplies
 *      inner token colours" is inherited as-is and describes the Rust.
 *
 * NOT SUPPORTED, and each renders as ordinary text rather than as a
 * mangled tree: GFM footnotes (`[^1]` and a `[^1]: …` definition both come
 * through literally), description lists (`: term` is a paragraph), and
 * `^superscript^` (not a CommonMark construct; left alone). A document
 * using them still renders; it just does not get the extension.
 */

/* Every backward-looking scan in the INLINE pass — an emphasis run looking
 * for its closer, a `[` looking for its `]`, a `~~` looking for its `~~`, a
 * `<` looking for its `>`, a backtick run looking for its twin, an unclosed
 * link's destination — is BOUNDED, and every one of them whose bound could
 * grow with the document is bounded TWICE: by a per-opener window AND by the
 * run's shared budget. Both bounds are load-bearing on a 16 MiB corpus file.
 *
 * The window is how far one opener may look: an emphasis span or a link label
 * longer than its window is not a thing a person writes. The budget is how
 * many bytes of looking the whole inline run may do, because the window
 * alone is not enough: a document of nothing but `a_b_c_d_` gives every one
 * of its 8 M underscores a full window to fail in, and that is quadratic
 * even though each individual search is bounded. The budget is a multiple of
 * the run's own length, so it never fires on prose and always fires on a
 * pathological one.
 *
 * There are TWO WAYS OF SPENDING THE BUDGET and the difference is
 * deliberate, not an inconsistency. The delimiter scans — `emphasis_run`,
 * `strikethrough`, `inline_link`'s label search — charge their whole WINDOW
 * up front, whether or not the search finds anything: a delimiter that
 * fails to pair falls back to literal text, which reads the same either way,
 * so over-charging costs nothing a reader can see. The construct scans —
 * `raw_tag`, `autolink`'s body, the code-span closer, `inline_link`'s
 * destination and title, `label_has_link` — charge what they ACTUALLY looked
 * at, because a construct that runs out of budget is ESCAPED, and a document
 * of 100 000 short `<b>` tags charged a whole tag-window each would render
 * 99 000 tags short. The invariant is the same either way — the inline pass
 * looks at no more than MD_SCAN_BUDGET_PER_BYTE bytes per byte of input —
 * and only the failure mode differs.
 *
 * The scans that are bounded but NOT budgeted, so the claim above is not
 * quietly wrong about them: `entity` and `autolink`'s email branch are
 * capped at MD_SCHEME_MAX, a CONSTANT, so a document of 8 M `&` pays 8 M
 * bounded looks and not a multiple of that. `bare_autolink` has no bound at
 * all and does not need one: it stops at whitespace, `<` or `"`, all of
 * which also stop the next candidate, so consecutive scans are disjoint and
 * sum to the length of the input rather than a multiple of it. The block
 * layer is a different matter and is bounded by being line-oriented: every
 * backward walk there is over one line, with the single exception of
 * `html_kind`, which takes a whole line as its input and so is the block
 * layer's one unbounded-line case — it carries its own budget for that. */
#define MD_DELIM_SCAN 4096u
#define MD_LINK_SCAN 4096u
#define MD_SCHEME_MAX 32u
/* The window for a scan whose subject is markup rather than a delimiter: a
 * raw tag, an HTML comment, an autolink body, a code span. Larger than the
 * delimiter windows on purpose, because these are the constructs where a
 * long one IS real — a tag carrying a data: URI, a comment carrying a
 * licence header — and because the exact charge means an oversized window
 * costs nothing when the scan is short. */
#define MD_TAG_SCAN 65536u
#define MD_SCAN_BUDGET_PER_BYTE 4u
/* Recursion ceiling for nested blockquotes, lists and emphasis. Past it a
 * quote's content is escaped as text, a list falls back to a paragraph and
 * an emphasis run is emitted literally, so the output stays well-formed
 * instead of blowing the C stack on a hostile document. The two fallbacks do
 * not agree with the reference renderer and are documented at their sites;
 * this ceiling is not negotiable, only what happens past it is. */
#define MD_MAX_DEPTH 24u
/* Table columns per row. A wider row keeps its first MD_MAX_CELLS cells;
 * the table stays well-formed and the tail is dropped rather than trusted. */
#define MD_MAX_CELLS 128u
/* Delimiter runs examined by one emphasis scan. The window caps how far an
 * opener may look; this caps how many runs that look may stack up, and it
 * is what keeps the matcher off a document that is nothing but `*`. */
#define MD_MAX_DELIMS 128u

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "kbc/markdown.h"
#include "kbc/mem.h"

/* Charge a construct scan for the bytes it ACTUALLY looked at, and report
 * whether the run can still afford the look. Charging the window instead
 * would be simpler and would be wrong: a run-out-of-budget construct is
 * ESCAPED, so a document of 100 000 short `<b>` tags would render 99 000
 * tags short if each charge were a whole tag-window. The invariant this buys
 * is the same one the delimiter scans buy by charging up front — the inline
 * pass looks at no more than MD_SCAN_BUDGET_PER_BYTE bytes per byte of
 * input — and the total is bounded because a scan that cannot be afforded
 * does not happen at all. */
static bool scan_spend(size_t *budget, size_t bytes) {
  if (bytes > *budget) {
    *budget = 0;
    return false;
  }
  *budget -= bytes;
  return true;
}

/* How far a construct scan starting at `from` may look: its window, the rest
 * of the input, and what the budget can still pay for, whichever is
 * smallest. The returned value is a hard ceiling the caller must respect.
 * The caller charges `scan_spend(budget, walked - from)`, so the charge is
 * for the bytes the scan actually walked and not for the whole window. */
static size_t scan_limit(size_t *budget, size_t from, size_t n, size_t window) {
  size_t room = n > from ? n - from : 0u;
  if (room > window) room = window;
  if (room > *budget) room = *budget;
  return from + room;
}

/* The original's reading theme (kb-core/src/markdown_prose.css, 7425 bytes),
 * inlined so the cross-origin sandboxed iframe needs no parent-SPA assets.
 * Byte for byte, in three literals because ISO C only requires a compiler
 * to accept a 4095-byte string literal and this one is 7425; the three are
 * emitted back to back with nothing between them, so the page carries the
 * file's bytes exactly, trailing newline included. Not to be "improved":
 * it is a copy of a shipped asset, and the line lengths below are the
 * CSS's, not a style choice. */
static const char PROSE_CSS_A[] =
    "/* kb markdown reading theme — self-contained (no external fetches), served\n"
    " * into the sandboxed cross-origin artifact iframe. Mirrors the\n"
    " * session_render.css house style: own tokens, system-font stacks with the\n"
    " * kb families preferred. The slim top \"markdown\" bar signals the source\n"
    " * format while the body reads as polished long-form prose. */\n"
    "\n"
    ":root {\n"
    "  --md-bg: #fbfaf7;          /* warm paper */\n"
    "  --md-surface: #ffffff;\n"
    "  --md-ink: #1f2530;\n"
    "  --md-ink-soft: #4a5260;\n"
    "  --md-ink-dim: #7b8493;\n"
    "  --md-rule: #e7e3da;\n"
    "  --md-rule-soft: #f0ece3;\n"
    "  --md-accent: #6d5ae6;      /* kb purple */\n"
    "  --md-accent-soft: #efecff;\n"
    "  --md-code-bg: #f5f3ee;\n"
    "  --md-code-ink: #2b313c;\n"
    "  --md-font-serif: \"Source Serif 4\", Georgia, Cambria, \"Times New Roman\", serif;\n"
    "  --md-font-sans: \"Inter Tight\", system-ui, -apple-system, \"Segoe UI\", sans-serif;\n"
    "  --md-font-mono: \"JetBrains Mono\", ui-monospace, \"SF Mono\", Menlo, Consolas, monospace;\n"
    "}\n"
    "\n"
    "* { box-sizing: border-box; }\n"
    "\n"
    "html, body {\n"
    "  margin: 0;\n"
    "  padding: 0;\n"
    "  background: var(--md-bg);\n"
    "  color: var(--md-ink);\n"
    "}\n"
    "\n"
    "body.kb-md-prose {\n"
    "  font-family: var(--md-font-serif);\n"
    "  font-size: 17px;\n"
    "  line-height: 1.68;\n"
    "  -webkit-font-smoothing: antialiased;\n"
    "  text-rendering: optimizeLegibility;\n"
    "}\n"
    "\n"
    "/* Slim top bar: the unmistakable \"this is markdown\" signal. */\n"
    ".kb-md-bar {\n"
    "  position: sticky;\n"
    "  top: 0;\n"
    "  z-index: 10;\n"
    "  display: flex;\n"
    "  align-items: center;\n"
    "  gap: 7px;\n"
    "  height: 30px;\n"
    "  padding: 0 16px;\n"
    "  font-family: var(--md-font-mono);\n"
    "  font-size: 11px;\n"
    "  letter-spacing: 0.14em;\n"
    "  text-transform: uppercase;\n"
    "  color: var(--md-accent);\n"
    "  background: var(--md-accent-soft);\n"
    "  border-bottom: 1px solid var(--md-rule);\n"
    "  user-select: none;\n"
    "}\n"
    ".kb-md-bar__dot {\n"
    "  width: 7px;\n"
    "  height: 7px;\n"
    "  border-radius: 50%;\n"
    "  background: var(--md-accent);\n"
    "  box-shadow: 0 0 0 3px color-mix(in srgb, var(--md-accent) 22%, transparent);\n"
    "}\n"
    "\n"
    "/* Reading column. */\n"
    ".kb-md-doc {\n"
    "  max-width: 720px;\n";
static const char PROSE_CSS_B[] =
    "  margin: 0 auto;\n"
    "  padding: 40px 28px 96px;\n"
    "}\n"
    "\n"
    "/* Headings — serif, tight top margin, airy below. */\n"
    ".kb-md-doc h1, .kb-md-doc h2, .kb-md-doc h3,\n"
    ".kb-md-doc h4, .kb-md-doc h5, .kb-md-doc h6 {\n"
    "  font-family: var(--md-font-serif);\n"
    "  font-weight: 600;\n"
    "  line-height: 1.25;\n"
    "  color: var(--md-ink);\n"
    "  margin: 1.6em 0 0.5em;\n"
    "  scroll-margin-top: 44px;       /* clears the sticky bar on anchor jump */\n"
    "}\n"
    ".kb-md-doc h1 { font-size: 2.0em; margin-top: 0.2em; letter-spacing: -0.01em; }\n"
    ".kb-md-doc h2 { font-size: 1.5em; padding-bottom: 0.2em; border-bottom: 1px solid var(--md-rule-soft); }\n"
    ".kb-md-doc h3 { font-size: 1.24em; }\n"
    ".kb-md-doc h4 { font-size: 1.06em; }\n"
    ".kb-md-doc h5, .kb-md-doc h6 { font-size: 0.95em; color: var(--md-ink-soft); }\n"
    "/* comrak emits no heading ids (the in-iframe runtime assigns them); if a\n"
    " * raw-HTML heading carries an anchor link, keep it unobtrusive. */\n"
    ".kb-md-doc .anchor { float: left; margin-left: -0.8em; padding-right: 0.2em; opacity: 0; }\n"
    ".kb-md-doc h1:hover .anchor, .kb-md-doc h2:hover .anchor,\n"
    ".kb-md-doc h3:hover .anchor { opacity: 0.4; }\n"
    "\n"
    ".kb-md-doc p { margin: 0 0 1.05em; }\n"
    ".kb-md-doc a { color: var(--md-accent); text-decoration: underline; text-underline-offset: 2px; text-decoration-thickness: 1px; }\n"
    ".kb-md-doc a:hover { text-decoration-thickness: 2px; }\n"
    "\n"
    ".kb-md-doc ul, .kb-md-doc ol { margin: 0 0 1.05em; padding-left: 1.5em; }\n"
    ".kb-md-doc li { margin: 0.2em 0; }\n"
    ".kb-md-doc li::marker { color: var(--md-ink-dim); }\n"
    "/* GFM task lists. */\n"
    ".kb-md-doc li input[type=\"checkbox\"] { margin-right: 0.5em; transform: translateY(1px); accent-color: var(--md-accent); }\n"
    ".kb-md-doc ul:has(> li > input[type=\"checkbox\"]) { list-style: none; padding-left: 0.3em; }\n"
    "\n"
    ".kb-md-doc blockquote {\n"
    "  margin: 0 0 1.05em;\n"
    "  padding: 0.2em 1.1em;\n"
    "  border-left: 3px solid var(--md-accent);\n"
    "  background: var(--md-rule-soft);\n"
    "  color: var(--md-ink-soft);\n"
    "  border-radius: 0 4px 4px 0;\n"
    "}\n"
    ".kb-md-doc blockquote p:last-child { margin-bottom: 0; }\n"
    "\n"
    "/* Inline code + fenced blocks. syntect supplies inner token colours; we own\n"
    " * the container chrome + mono font. */\n"
    ".kb-md-doc code {\n"
    "  font-family: var(--md-font-mono);\n"
    "  font-size: 0.86em;\n"
    "  background: var(--md-code-bg);\n"
    "  color: var(--md-code-ink);\n"
    "  padding: 0.12em 0.38em;\n"
    "  border-radius: 4px;\n"
    "  border: 1px solid var(--md-rule);\n"
    "}\n"
    ".kb-md-doc pre {\n"
    "  margin: 0 0 1.2em;\n"
    "  padding: 14px 16px;\n"
    "  background: var(--md-code-bg);\n"
    "  border: 1px solid var(--md-rule);\n"
    "  border-radius: 8px;\n"
    "  overflow-x: auto;\n"
    "  line-height: 1.5;\n"
    "}\n"
    ".kb-md-doc pre code {\n"
    "  font-size: 0.85em;\n"
    "  background: none;\n"
    "  border: none;\n"
    "  padding: 0;\n";
static const char PROSE_CSS_C[] =
    "  color: inherit;\n"
    "}\n"
    "\n"
    "/* GFM tables. */\n"
    ".kb-md-doc table {\n"
    "  width: 100%;\n"
    "  border-collapse: collapse;\n"
    "  margin: 0 0 1.2em;\n"
    "  font-size: 0.92em;\n"
    "  font-family: var(--md-font-sans);\n"
    "}\n"
    ".kb-md-doc th, .kb-md-doc td { padding: 7px 11px; border: 1px solid var(--md-rule); text-align: left; }\n"
    ".kb-md-doc thead th { background: var(--md-rule-soft); font-weight: 600; }\n"
    ".kb-md-doc tbody tr:nth-child(even) { background: color-mix(in srgb, var(--md-rule-soft) 50%, transparent); }\n"
    "\n"
    ".kb-md-doc hr { border: none; border-top: 1px solid var(--md-rule); margin: 2em 0; }\n"
    ".kb-md-doc img { max-width: 100%; height: auto; border-radius: 6px; }\n"
    ".kb-md-doc strong { font-weight: 600; }\n"
    ".kb-md-doc del { color: var(--md-ink-dim); }\n"
    ".kb-md-doc sup { font-size: 0.75em; }\n"
    "\n"
    "/* Footnotes (GFM). */\n"
    ".kb-md-doc .footnotes { margin-top: 2.4em; padding-top: 0.6em; border-top: 1px solid var(--md-rule); font-size: 0.9em; color: var(--md-ink-soft); }\n"
    "\n"
    ".kb-md-doc ::selection { background: var(--md-accent-soft); }\n"
    "\n"
    "/* Obsidian-style callouts: `> [!type] …` blockquotes rewritten server-side by\n"
    " * markdown::rewrite_callouts into <div class=\"kb-callout kb-callout--TYPE\">.\n"
    " * Per-type accent flows through one --cal custom property; an unknown type\n"
    " * falls back to the kb accent so the base style always applies. */\n"
    ".kb-md-doc .kb-callout {\n"
    "  --cal: var(--md-accent);\n"
    "  margin: 0 0 1.05em;\n"
    "  padding: 0.65em 1.1em;\n"
    "  border-left: 4px solid var(--cal);\n"
    "  background: color-mix(in srgb, var(--cal) 8%, var(--md-surface));\n"
    "  border-radius: 0 6px 6px 0;\n"
    "}\n"
    ".kb-md-doc .kb-callout__title {\n"
    "  font-family: var(--md-font-sans);\n"
    "  font-weight: 700;\n"
    "  font-size: 0.78em;\n"
    "  letter-spacing: 0.05em;\n"
    "  text-transform: uppercase;\n"
    "  color: var(--cal);\n"
    "  margin-bottom: 0.35em;\n"
    "}\n"
    ".kb-md-doc .kb-callout > :last-child { margin-bottom: 0; }\n"
    ".kb-md-doc .kb-callout--note, .kb-md-doc .kb-callout--info { --cal: #2f7fc9; }\n"
    ".kb-md-doc .kb-callout--abstract, .kb-md-doc .kb-callout--summary,\n"
    ".kb-md-doc .kb-callout--tldr { --cal: #1196a8; }\n"
    ".kb-md-doc .kb-callout--tip, .kb-md-doc .kb-callout--hint,\n"
    ".kb-md-doc .kb-callout--important { --cal: #1aa17e; }\n"
    ".kb-md-doc .kb-callout--success, .kb-md-doc .kb-callout--check,\n"
    ".kb-md-doc .kb-callout--done { --cal: #2ea043; }\n"
    ".kb-md-doc .kb-callout--question, .kb-md-doc .kb-callout--help,\n"
    ".kb-md-doc .kb-callout--faq { --cal: #c99a06; }\n"
    ".kb-md-doc .kb-callout--warning, .kb-md-doc .kb-callout--caution,\n"
    ".kb-md-doc .kb-callout--attention { --cal: #d97914; }\n"
    ".kb-md-doc .kb-callout--danger, .kb-md-doc .kb-callout--error,\n"
    ".kb-md-doc .kb-callout--failure, .kb-md-doc .kb-callout--fail,\n"
    ".kb-md-doc .kb-callout--bug, .kb-md-doc .kb-callout--missing { --cal: #d83a3a; }\n"
    ".kb-md-doc .kb-callout--example { --cal: #8a63d2; }\n"
    ".kb-md-doc .kb-callout--quote, .kb-md-doc .kb-callout--cite { --cal: var(--md-ink-dim); }\n"
    "\n"
    "@media (max-width: 640px) {\n"
    "  .kb-md-doc { padding: 28px 18px 72px; }\n"
    "  body.kb-md-prose { font-size: 16px; }\n"
    "}\n";

/* ----------------------------------------------------------------- sink -- */

/* The one output path. Every kbc_str call is checked here rather than at
 * two hundred call sites, and `oom` is sticky so a caller can finish its
 * block structure and the status is reported once, at the top. */
typedef struct {
  kbc_str out;
  bool oom;
  /* Set when the LAST block written was a tight-list paragraph, which ends
   * with a bare newline that the enclosing `</li>` has to absorb: the
   * original writes `<li>x</li>`, not `<li>x\n</li>`. A block that ends in a
   * tag has already terminated itself and keeps its newline. */
  bool ends_tight_para;
  /* True while a table cell is being rendered. GFM §4.10 lets a cell include
   * a pipe by escaping it "including inside other inline spans", so the
   * backslash in `x\|y` goes away in a cell and stays everywhere else. */
  bool in_table_cell;
} sink;

static void put(sink *s, const char *p, size_t n) {
  if (s->oom || n == 0) return;
  if (kbc_failed(kbc_str_append(&s->out, p, n))) s->oom = true;
}

static void lit(sink *s, const char *c) { put(s, c, strlen(c)); }

static void chr(sink *s, char c) {
  if (s->oom) return;
  if (kbc_failed(kbc_str_append(&s->out, &c, 1))) s->oom = true;
}

/* A sink over a fresh buffer. The block layer buffers whole lines into one
 * before handing them to the inline pass, and reusing the same sticky
 * out-of-memory contract keeps a single failure path for the file. */
static void sink_init(sink *s) {
  kbc_str_init(&s->out);
  s->oom = false;
  s->ends_tight_para = false;
  s->in_table_cell = false;
}

/* The forward declarations the two halves of the renderer need. `autolink`,
 * `raw_tag` and `code_span_end` sit with the inline scanner but are named
 * from `inline_link` and `html_kind`, which are earlier in the file;
 * `md_inline` recurses into itself, so it has to name itself. */
static size_t autolink(const char *p, size_t n, size_t i, size_t *budget);
static size_t raw_tag(const char *p, size_t n, size_t i, size_t *budget);
static size_t code_span_end(const char *p, size_t n, size_t i, size_t r,
                            size_t *budget);
static int html_kind(const char *p, size_t n);

/* ------------------------------------------------------- byte predicates -- */

/* ASCII only, and deliberately not <ctype.h>: the process never calls
 * setlocale, but a locale-sensitive predicate on a hostile byte is a
 * behaviour that depends on something outside this file. */
static bool sp(unsigned char c) {
  return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' ||
         c == '\v';
}
static bool alpha(unsigned char c) {
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}
static bool digit(unsigned char c) { return c >= '0' && c <= '9'; }
static bool hex(unsigned char c) {
  return digit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}
static bool alnum(unsigned char c) { return alpha(c) || digit(c); }
static bool punct(unsigned char c) {
  return c > 0x20u && c < 0x7fu && !alnum(c);
}

static char lower_c(char c) {
  return (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
}

/* -------------------------------------------------------------- escapes -- */

/* Text and code. `"` needs no escape in either, which is why the two
 * functions differ at all. */
static void esc(sink *s, const char *p, size_t n) {
  for (size_t i = 0; i < n; i++) {
    switch (p[i]) {
      case '&': lit(s, "&amp;"); break;
      case '<': lit(s, "&lt;"); break;
      case '>': lit(s, "&gt;"); break;
      default: chr(s, p[i]);
    }
  }
}

/* An attribute value. `"` is escaped because an unescaped one ends the
 * attribute and hands the rest of the line to the parser as attributes;
 * C0 controls and DEL are DROPPED, because a URL carrying a NUL or a bare
 * CR is the other way a hand-rolled serialiser emits a page that does not
 * mean what the document said. */
static void esc_attr(sink *s, const char *p, size_t n) {
  for (size_t i = 0; i < n; i++) {
    unsigned char c = (unsigned char)p[i];
    if (c < 0x20u || c == 0x7fu) continue;
    switch (c) {
      case '&': lit(s, "&amp;"); break;
      case '<': lit(s, "&lt;"); break;
      case '>': lit(s, "&gt;"); break;
      case '"': lit(s, "&quot;"); break;
      default: chr(s, (char)c);
    }
  }
}

/* --------------------------------------------------------------- lines -- */

typedef struct {
  const char *p;
  size_t n;    /* the line without its newline, and without a CRLF's CR */
  size_t next; /* the offset of the line after this one */
} line_t;

static void line_at(const char *s, size_t n, size_t pos, line_t *out) {
  size_t e = pos;
  while (e < n && s[e] != '\n') e++;
  size_t len = e - pos;
  if (len > 0 && s[pos + len - 1] == '\r') len--;
  out->p = s + pos;
  out->n = len;
  out->next = (e < n) ? e + 1u : n;
}

static bool blank(const line_t *l) {
  for (size_t i = 0; i < l->n; i++) {
    if (!sp((unsigned char)l->p[i])) return false;
  }
  return true;
}

/* The line's leading whitespace, in columns, with a tab advancing to the
 * next multiple of four. Byte counts would make `-` and `-\t` disagree
 * about where an item's text starts. */
static size_t columns(const char *p, size_t n) {
  size_t c = 0;
  for (size_t i = 0; i < n; i++) {
    if (p[i] == ' ') {
      c += 1;
    } else if (p[i] == '\t') {
      c += 4u - (c % 4u);
    } else {
      break;
    }
  }
  return c;
}

/* Removes up to `want` columns of leading whitespace. */
static void strip_columns(const char **p, size_t *n, size_t want) {
  size_t c = 0, i = 0;
  while (i < *n && c < want) {
    if ((*p)[i] == ' ') {
      c += 1;
      i++;
    } else if ((*p)[i] == '\t') {
      c += 4u - (c % 4u);
      i++;
    } else {
      break;
    }
  }
  *p += i;
  *n -= i;
}

/* The same, for a paragraph's own line — except that two or more trailing
 * spaces are a HARD line break (§6) and have to survive to the inline pass.
 * Trimming them was what made `md_inline`'s `<br />` branch unreachable
 * from every block context: a document asking for a break got a soft one. */
static void trim_para_line(const char *p, size_t n, const char **out,
                           size_t *out_n) {
  size_t a = 0;
  while (a < n && sp((unsigned char)p[a])) a++;
  size_t b = n;
  while (b > a && p[b - 1] == ' ') b--;
  if (n - b >= 2u) {
    /* Two or more trailing spaces ARE a hard line break, so they stay: they
     * have to reach `md_inline` for it to see them. */
    *out = p + a;
    *out_n = n - a;
    return;
  }
  while (b > a && sp((unsigned char)p[b - 1])) b--;
  *out = p + a;
  *out_n = b - a;
}

static void trim(const char *p, size_t n, const char **out, size_t *out_n) {
  size_t a = 0;
  while (a < n && sp((unsigned char)p[a])) a++;
  size_t b = n;
  while (b > a && sp((unsigned char)p[b - 1])) b--;
  *out = p + a;
  *out_n = b - a;
}

/* -------------------------------------------------------------- fences -- */

/* A fence opener: 3+ ` or ~ at up to three columns of indent, then either
 * end of line or an info string — which may not itself contain a backtick,
 * because a backtick run inside a backtick fence is never info. */
static bool fence_open(const char *p, size_t n, char *fc, size_t *fl) {
  size_t i = 0;
  while (i < 3 && i < n && p[i] == ' ') i++;
  if (i >= n || (p[i] != '`' && p[i] != '~')) return false;
  char c = p[i];
  size_t j = i;
  while (j < n && p[j] == c) j++;
  if (j - i < 3) return false;
  if (c == '`') {
    for (size_t k = j; k < n; k++) {
      if (p[k] == '`') return false;
    }
  }
  *fc = c;
  *fl = j - i;
  return true;
}

/* The closer for an open fence: the same character, at least as long, with
 * nothing but whitespace behind it. */
static bool fence_close(const char *p, size_t n, char fc, size_t fl) {
  size_t i = 0;
  while (i < 3 && i < n && p[i] == ' ') i++;
  if (i >= n || p[i] != fc) return false;
  size_t j = i;
  while (j < n && p[j] == fc) j++;
  if (j - i < fl) return false;
  for (size_t k = j; k < n; k++) {
    if (!sp((unsigned char)p[k])) return false;
  }
  return true;
}

/* --------------------------------------------------------- block probes -- */

static int atx(const char *p, size_t n) {
  size_t i = 0;
  while (i < n && p[i] == '#') i++;
  if (i == 0 || i > 6) return 0;
  if (i < n && p[i] != ' ' && p[i] != '\t') return 0;
  return (int)i;
}

/* Three or more of one of `*`, `-`, `_` with nothing else but spaces
 * between them. `- - -` is a rule, not three empty bullets. */
static bool rule(const char *p, size_t n) {
  if (n == 0) return false;
  char c = p[0];
  if (c != '*' && c != '-' && c != '_') return false;
  size_t count = 0;
  for (size_t i = 0; i < n; i++) {
    if (p[i] == c) {
      count++;
    } else if (p[i] != ' ' && p[i] != '\t') {
      return false;
    }
  }
  return count >= 3;
}

/* A setext underline: one or more of a single `=` (h1) or `-` (h2), with no
 * internal spaces. Checked BEFORE `rule` where a paragraph is open, which is
 * what makes `Title\n-----` a heading rather than text plus a rule. */
static int setext(const char *p, size_t n) {
  if (n == 0) return 0;
  char c = p[0];
  if (c != '=' && c != '-') return 0;
  for (size_t i = 0; i < n; i++) {
    if (p[i] != c) return 0;
  }
  return c == '=' ? 1 : 2;
}

/* One list item's marker. Offsets are bytes into the line, columns are for
 * the indentation tests the two are not interchangeable for. */
typedef struct {
  size_t mb, me, tb; /* marker start, marker end, first content byte */
  size_t indent;     /* columns before the marker */
  size_t text_col;   /* column the item's content starts at */
  bool ordered;
  size_t start; /* an ordered list's first number */
  char bullet;  /* the bullet character, for list continuity */
} marker;

static bool list_marker(const char *p, size_t n, marker *m) {
  size_t i = 0, cols = 0;
  while (i < n && sp((unsigned char)p[i])) {
    cols += (p[i] == '\t') ? 4u - (cols % 4u) : 1u;
    i++;
  }
  if (i >= n) return false;
  size_t mb = i;
  bool ordered = false;
  size_t num = 0;
  if (p[i] == '-' || p[i] == '*' || p[i] == '+') {
    i++;
  } else if (digit((unsigned char)p[i])) {
    size_t d = i;
    while (i < n && digit((unsigned char)p[i]) && i - d < 9) {
      num = num * 10u + (size_t)(p[i] - '0');
      i++;
    }
    if (i < n && (p[i] == '.' || p[i] == ')')) {
      i++;
      ordered = true;
    } else {
      return false;
    }
  } else {
    return false;
  }
  /* `-x` is a word, `1.2` is a number, `-` alone is an empty item. */
  if (i < n && !sp((unsigned char)p[i])) return false;
  size_t me = i;
  size_t gap = 0;
  while (i < n && p[i] == ' ') {
    gap++;
    i++;
  }
  if (i < n && p[i] == '\t') {
    gap = gap < 4u ? 4u - gap : 1u;
    i++;
  }
  if (gap == 0) gap = 1u;  /* an empty item still has a content column */
  if (gap > 4u) gap = 1u;  /* five or more: the rest is indented code */
  m->mb = mb;
  m->me = me;
  m->tb = i;
  m->indent = cols;
  m->text_col = cols + (me - mb) + gap;
  m->ordered = ordered;
  m->start = num;
  m->bullet = ordered ? 0 : p[mb];
  return true;
}

/* --------------------------------------------------------- frontmatter -- */

typedef struct {
  const char *body;
  size_t body_len;
} fm_split;

/* A leading UTF-8 BOM — common from Windows editors — would otherwise defeat
 * the `---` test at byte 0 and dump the whole frontmatter block into the
 * body as visible text. It is dropped before anything looks. */
static size_t bom(const char *s, size_t n) {
  return (n >= 3 && (unsigned char)s[0] == 0xEFu &&
          (unsigned char)s[1] == 0xBBu && (unsigned char)s[2] == 0xBFu)
             ? 3u
             : 0u;
}

/* Splits a leading `---` block off `s` and reads ONE key from it: `title`.
 *
 * This is not a second frontmatter parser. parse.c's `front_matter` owns the
 * `kb-*` facets and the index reads them from there; it is file-static, it
 * fills a kbc_metas, and it hands back neither the body offset nor a title,
 * both of which this file needs. So the fence scan below is the smallest
 * thing that answers those two questions, and it is written to agree with
 * parse.c's rule exactly where they overlap: a fence on the first line, a
 * trimmed `---` (trailing space included) to close, and — the edge the
 * original calls out at markdown.rs:230 — a leading `---` that never closes
 * is a THEMATIC BREAK, not frontmatter, because guessing would swallow the
 * whole document.
 *
 * `title` is cleared, then filled if and only if the block carries a
 * `title:` key with a non-empty value. A later duplicate wins, as in the
 * original. */
static kbc_status frontmatter(const char *s, size_t n, fm_split *out,
                              kbc_str *title) {
  size_t b = bom(s, n);
  /* The BOM is dropped whether or not a block follows, as the original does
   * at markdown.rs:236: with no frontmatter the body is the source, and a
   * document that still begins with a BOM renders a stray U+FEFF in front of
   * its first heading. */
  out->body = s + b;
  out->body_len = n - b;
  kbc_str_clear(title);

  const char *t = s + b;
  size_t tn = n - b;
  if (tn < 4 || t[0] != '-' || t[1] != '-' || t[2] != '-') return KBC_OK;
  size_t after;
  if (t[3] == '\n') {
    after = 4;
  } else if (t[3] == '\r' && tn >= 5 && t[4] == '\n') {
    after = 5;
  } else {
    return KBC_OK;
  }

  size_t pos = after, block_end = 0, body = 0;
  bool closed = false;
  while (pos < tn) {
    line_t l;
    line_at(t, tn, pos, &l);
    const char *q;
    size_t qn;
    trim(l.p, l.n, &q, &qn);
    if (qn == 3 && q[0] == '-' && q[1] == '-' && q[2] == '-') {
      block_end = pos;
      body = l.next;
      closed = true;
      break;
    }
    pos = l.next;
  }
  if (!closed) return KBC_OK; /* a thematic break, not metadata */

  out->body = t + body;
  out->body_len = tn - body;

  pos = after;
  while (pos < block_end) {
    line_t l;
    line_at(t, tn, pos, &l);
    pos = l.next;
    const char *q;
    size_t qn;
    trim(l.p, l.n, &q, &qn);
    if (qn == 0 || q[0] == '#') continue; /* blank, or a comment */
    size_t k = 0;
    while (k < qn && q[k] != ':') k++;
    if (k >= qn) continue; /* not a `key: value` line at all */
    size_t kend = k;
    while (kend > 0 && sp((unsigned char)q[kend - 1])) kend--;
    if (kend != 5 || memcmp(q, "title", 5) != 0) continue;
    size_t vs = k + 1;
    while (vs < qn && sp((unsigned char)q[vs])) vs++;
    size_t ve = qn;
    while (ve > vs && sp((unsigned char)q[ve - 1])) ve--;
    /* The original strips every surrounding quote, not a matched pair, so
     * `"a"` and `"a` both yield `a`. Matching it keeps a title carrying a
     * stray quote byte-identical rather than inventing a rule. */
    while (ve > vs && q[vs] == '"' && q[ve - 1] == '"') {
      vs++;
      ve--;
    }
    if (ve <= vs) continue; /* `title:` with no value sets no title */
    kbc_str_clear(title);
    if (kbc_failed(kbc_str_append(title, q + vs, ve - vs))) {
      return KBC_ERR_NOMEM;
    }
  }
  return KBC_OK;
}

/* The first `# ` line, which is the title fallback when the frontmatter
 * carries none (markdown.rs:419-427).
 *
 * Fenced code is skipped, which the original does not do: a `# ` line
 * inside a fence is sample text, and letting it name the document is how a
 * shell snippet becomes the title of the page that quotes it. The escape
 * hatch is not "title your page with a code sample". Everything else is
 * the original's rule — the line is trimmed, the marker is exactly `# `
 * (so `## x` and `#x` are not titles), and the rest must be non-empty. */
static kbc_status first_h1(const char *s, size_t n, kbc_str *out, bool *found) {
  *found = false;
  size_t pos = 0;
  char fc = 0;
  size_t fl = 0;
  while (pos < n) {
    line_t l;
    line_at(s, n, pos, &l);
    pos = l.next;
    const char *q;
    size_t qn;
    trim(l.p, l.n, &q, &qn);
    if (fc != 0) {
      if (fence_close(q, qn, fc, fl)) fc = 0;
      continue;
    }
    char c;
    size_t f;
    if (fence_open(q, qn, &c, &f)) {
      fc = c;
      fl = f;
      continue;
    }
    if (qn <= 2 || q[0] != '#' || q[1] != ' ') continue;
    const char *t;
    size_t tn;
    trim(q + 2, qn - 2, &t, &tn);
    if (tn == 0) continue;
    if (kbc_failed(kbc_str_append(out, t, tn))) return KBC_ERR_NOMEM;
    *found = true;
    return KBC_OK;
  }
  return KBC_OK;
}

/* The title precedence the original applies at markdown.rs:433-438:
 * frontmatter `title:`, else the first `# ` line, else the literal. */
static kbc_status title_of(const char *s, size_t n, fm_split *fm, kbc_str *out) {
  kbc_status st = frontmatter(s, n, fm, out);
  if (kbc_failed(st)) return st;
  if (out->len > 0) return KBC_OK;
  bool found = false;
  st = first_h1(fm->body, fm->body_len, out, &found);
  if (kbc_failed(st) || found) return st;
  return kbc_str_puts(out, "Untitled");
}

/* A title is text in a `<title>` element, and the corpus is hostile. C0
 * controls and DEL go: a NUL in a source document is valid UTF-8 and would
 * otherwise truncate the page's own metadata mid-string, and a bare CR in
 * a title is a line ending nobody asked for. */
static void strip_controls(kbc_str *t) {
  size_t w = 0;
  for (size_t r = 0; r < t->len; r++) {
    unsigned char c = (unsigned char)t->ptr[r];
    if (c < 0x20u || c == 0x7fu) continue;
    t->ptr[w++] = t->ptr[r];
  }
  t->len = w;
  if (t->ptr != NULL) t->ptr[w] = '\0';
}

/* ---------------------------------------------------------- callouts ---- */

/* `> [!type] Title` — the Obsidian callout header, ported from
 * markdown.rs:194-210. False for a plain blockquote, which is the whole
 * point: `> just a quote` must stay a blockquote.
 *
 * The fold marker sits flush against `]`, so it is stripped only there — a
 * title that legitimately begins with `-` survives. The type is restricted
 * to ASCII alphanumerics and `-` because it becomes a class name; anything
 * else is a blockquote that happens to begin `[!`. */
static bool callout_header(const char *p, size_t n, const char **type,
                           size_t *type_len, const char **title,
                           size_t *title_len) {
  if (n == 0 || p[0] != '>') return false;
  size_t i = 1;
  if (i < n && p[i] == ' ') i++; /* the one space `dequote` removes */
  while (i < n && sp((unsigned char)p[i])) i++;
  if (i + 1 >= n || p[i] != '[' || p[i + 1] != '!') return false;
  i += 2;
  size_t close = i;
  while (close < n && p[close] != ']') close++;
  if (close >= n || close == i) return false;
  for (size_t k = i; k < close; k++) {
    if (!alnum((unsigned char)p[k]) && p[k] != '-') return false;
  }
  size_t a = close + 1;
  if (a < n && (p[a] == '+' || p[a] == '-')) a++;
  const char *t;
  size_t tn;
  trim(p + a, n - a, &t, &tn);
  *type = p + i;
  *type_len = close - i;
  *title = t;
  *title_len = tn;
  return true;
}

/* Strips one `>` and the single space behind it (markdown.rs:187-189). */
static void dequote(const char **p, size_t *n) {
  if (*n == 0 || (*p)[0] != '>') return;
  (*p)++;
  (*n)--;
  if (*n > 0 && (*p)[0] == ' ') {
    (*p)++;
    (*n)--;
  }
}

/* Does any line open a callout? This is the same test the rewrite starts
 * from, so a document the pre-pass has no business touching is not touched:
 * below, that is the byte-identical fast path. Fenced code is skipped for
 * the reason the rewrite's own comment gives. */
static bool has_callout(const char *src, size_t n) {
  size_t pos = 0;
  char fc = 0;
  size_t fl = 0;
  while (pos < n) {
    line_t l;
    line_at(src, n, pos, &l);
    pos = l.next;
    if (fc != 0) {
      if (fence_close(l.p, l.n, fc, fl)) fc = 0;
      continue;
    }
    char c;
    size_t f;
    if (fence_open(l.p, l.n, &c, &f)) {
      fc = c;
      fl = f;
      continue;
    }
    const char *type;
    size_t tl;
    const char *t;
    size_t tn;
    if (callout_header(l.p, l.n, &type, &tl, &t, &tn)) return true;
  }
  return false;
}

/* Rewrites `> [!type] Title` blockquotes into the div the stylesheet expects
 * (markdown.rs:133-183). The blank lines around the de-quoted body are
 * load-bearing: they are what lets that body re-parse as Markdown instead of
 * arriving as one pre-formatted run.
 *
 * Two deliberate divergences from the original, both about not producing a
 * page that means something other than the document:
 *
 *   - Fenced code is skipped. The original is line-based and does not look,
 *     so a `> [!warning]` line inside a ``` fence is rewritten into raw
 *     `<div>` markup that then lands INSIDE a `<pre><code>`: a page whose
 *     callout div is never closed and which swallows the rest of the
 *     document. Raw passthrough plus a rewrite is exactly where a hand-
 *     rolled renderer breaks, so the rewrite stays out of code.
 *   - A CRLF document loses its CRs on rewritten lines only. The renderer
 *     normalises line endings everywhere anyway, so this is invisible in the
 *     output, and the no-callout fast path copies the source verbatim
 *     regardless. */
static void rewrite_callouts(sink *s, const char *src, size_t n) {
  if (!has_callout(src, n)) {
    put(s, src, n); /* byte-identical: the fast path the original documents */
    return;
  }
  size_t pos = 0;
  char fc = 0;
  size_t fl = 0;
  bool at_line_start = true;
  while (pos < n) {
    line_t l;
    line_at(src, n, pos, &l);
    const char *type = NULL;
    size_t tl = 0;
    const char *t = NULL;
    size_t tn = 0;
    if (fc == 0 && callout_header(l.p, l.n, &type, &tl, &t, &tn)) {
      pos = l.next;
      /* The contiguous blockquote run is the callout's body. */
      size_t j = pos;
      while (j < n) {
        line_t c;
        line_at(src, n, j, &c);
        if (c.n == 0 || c.p[0] != '>') break;
        j = c.next;
      }
      /* A blank line before the injected div whenever the callout is not the
       * first thing in the document. Without it the block parser reads
       * `<div …>` as a lazy paragraph continuation, never closes the `<p>`
       * above, and lands the callout's own markup inside it — the page came
       * out with a `MISMATCH </p>`, and inside a blockquote with the
       * callout escaping the `<blockquote>` that held it. The div is a
       * block; it has to be introduced like one. */
      if (!at_line_start) chr(s, '\n');
      lit(s, "<div class=\"kb-callout kb-callout--");
      for (size_t k = 0; k < tl; k++) chr(s, lower_c(type[k]));
      lit(s, "\">\n<div class=\"kb-callout__title\">");
      if (tn == 0) {
        /* No title: the type, capitalised, as markdown.rs:155-158 does. */
        chr(s, (char)((type[0] >= 'a' && type[0] <= 'z') ? type[0] - 'a' + 'A'
                                                         : type[0]));
        put(s, type + 1, tl - 1);
      } else {
        esc(s, t, tn);
      }
      lit(s, "</div>\n\n");
      for (size_t q = pos; q < j;) {
        line_t c;
        line_at(src, n, q, &c);
        q = c.next;
        const char *bp = c.p;
        size_t bn = c.n;
        dequote(&bp, &bn);
        put(s, bp, bn);
        chr(s, '\n');
      }
      lit(s, "\n</div>\n");
      at_line_start = true;
      pos = j;
      continue;
    }
    if (!at_line_start) chr(s, '\n');
    put(s, l.p, l.n);
    at_line_start = false;
    if (fc != 0) {
      if (fence_close(l.p, l.n, fc, fl)) fc = 0;
    } else {
      char c;
      size_t f;
      if (fence_open(l.p, l.n, &c, &f)) {
        fc = c;
        fl = f;
      }
    }
    pos = l.next;
  }
}

/* -------------------------------------------------------------- inline -- */

/* A `<…>` raw tag, comment or processing instruction, returned as the
 * offset just past its `>`, or 0.
 *
 * All four scans below are backward-looking — each starts at a `<` and walks
 * forward for a construct that may never close — and each is now bounded by
 * MD_TAG_SCAN and by `*budget`, through `scan_limit`, and charged for the
 * bytes it actually walked. The four were NOT equally broken, and saying so
 * is the point of writing this down rather than asserting a uniform fix.
 *
 * Two of them were quadratic. The comment scan stops at NOTHING but its
 * `-->`: a 1 MiB document of `x<!--a` — one paragraph, so the block layer
 * never claims it — took 152 s, and 2 MiB took 581 s. The `<!`
 * declaration scan stops at `>` or a newline, neither of which a document
 * made of `<!a` contains, and each `<` is a fresh candidate inside the
 * previous one's walk: 615 s at 1 MiB, 2052 s at 2 MiB. The `<?` scan is
 * that same code, and is quadratic by the same argument whenever the inline
 * pass is what reaches it — a bare `<?a` document is claimed by the block
 * layer first, which is why the bare case measures fast and `x<?a` does
 * not. All three are sub-second now at 16 MiB.
 *
 * The fourth, the attribute walk, was NOT quadratic: it stops at `<` and at
 * a newline, and a 2 MiB document of unbalanced `<a "` quotes measured 0.25 s
 * before this change. It is bounded anyway, because one tag's attribute list
 * being able to walk the rest of the document is not a property worth
 * having, and because four scanners bounding the same class of thing the
 * same way is easier to keep true than four different rules.
 *
 * A `<` that does not close within its allowance is escaped as text, which is
 * the right failure in both directions: a tag that never closes would
 * otherwise swallow the rest of the document into one attribute value, the
 * worst malformed page this file could emit. */
static size_t raw_tag(const char *p, size_t n, size_t i, size_t *budget) {
  size_t k = i + 1;
  if (k < n && p[k] == '!') {
    size_t limit = scan_limit(budget, k, n, MD_TAG_SCAN);
    if (n - k >= 3 && memcmp(p + k, "!--", 3) == 0) {
      for (size_t j = k + 3; j + 2 < limit; j++) {
        if (p[j] == '-' && p[j + 1] == '-' && p[j + 2] == '>') {
          scan_spend(budget, j + 3 - k);
          return j + 3;
        }
      }
      scan_spend(budget, limit - k);
      return 0;
    }
    for (size_t j = k + 1; j < limit; j++) {
      if (p[j] == '>') {
        scan_spend(budget, j + 1 - k);
        return j + 1;
      }
      if (p[j] == '\n') {
        scan_spend(budget, j + 1 - k);
        return 0;
      }
    }
    scan_spend(budget, limit - k);
    return 0;
  }
  if (k < n && p[k] == '?') {
    size_t limit = scan_limit(budget, k, n, MD_TAG_SCAN);
    for (size_t j = k + 1; j < limit; j++) {
      if (p[j] == '>') {
        scan_spend(budget, j + 1 - k);
        return j + 1;
      }
      if (p[j] == '\n') {
        scan_spend(budget, j + 1 - k);
        return 0;
      }
    }
    scan_spend(budget, limit - k);
    return 0;
  }
  if (k < n && p[k] == '/') k++;
  if (k >= n || !alpha((unsigned char)p[k])) return 0;
  k++;
  while (k < n && (alnum((unsigned char)p[k]) || p[k] == '-')) k++;
  if (k < n && p[k] != '>' && p[k] != '/' && !sp((unsigned char)p[k])) return 0;
  size_t base = k;
  size_t limit = scan_limit(budget, k, n, MD_TAG_SCAN);
  char quote = 0;
  for (; k < limit; k++) {
    char c = p[k];
    if (quote != 0) {
      if (c == quote) quote = 0;
      continue;
    }
    if (c == '"' || c == '\'') {
      quote = c;
      continue;
    }
    if (c == '>') {
      scan_spend(budget, k + 1 - base);
      return k + 1;
    }
    if (c == '<' || c == '\n') break;
  }
  scan_spend(budget, k - base);
  return 0;
}

/* `<scheme:…>` and `<user@host>`, the CommonMark autolinks. Returns the
 * offset just past the `>`, or 0. A space or a `<` inside is what keeps
 * `<a href="x">` from being read as an `a:` scheme.
 *
 * The scheme is capped at MD_SCHEME_MAX, which no URI scheme comes near, and
 * the email branch shares that cap, so both spend no budget of their own: a
 * scan that short cannot make the pass superlinear.
 *
 * The BODY was capped at nothing, and it now shares `raw_tag`'s window and
 * budget — but NOT because it was quadratic, which is the easy thing to
 * assume and which measurement here refutes. The body stops at `>`, a space
 * or a `<`, and two of those are stop bytes for the next candidate too, so
 * consecutive body scans are disjoint and sum to the length of the input: a
 * 2 MiB document of nothing but `<a:aaaa` took 0.13 s before this change and
 * takes 0.20 s after it. What the cap does buy is that ONE autolink can no
 * longer walk the rest of the document — `<a:` followed by sixteen megabytes
 * of prose with no `>` is a single O(n) scan today — and that the two
 * scanners bound the same class of thing the same way. A URL longer than
 * MD_TAG_SCAN is now text rather than a link, which no real URL is. */
static size_t autolink(const char *p, size_t n, size_t i, size_t *budget) {
  size_t limit = i + MD_SCHEME_MAX;
  if (limit > n) limit = n;
  size_t k = i + 1;
  size_t scheme = 0;
  while (k < limit && (alnum((unsigned char)p[k]) || p[k] == '+' ||
                      p[k] == '.' || p[k] == '-')) {
    k++;
    scheme++;
  }
  if (scheme > 0 && k < n && p[k] == ':') {
    k++;
    size_t start = k;
    size_t stop = scan_limit(budget, k, n, MD_TAG_SCAN);
    while (k < stop && p[k] != '>') {
      if (p[k] == '<' || p[k] == '\n' || sp((unsigned char)p[k])) break;
      k++;
    }
    if (k < stop && p[k] == '>') {
      if (k == start) {
        scan_spend(budget, k - start);
        return 0;
      }
      scan_spend(budget, k + 1 - start);
      return k + 1;
    }
    /* Either the body hit a character that cannot appear in one, or it ran
     * out of allowance. Either way this is not an autolink. */
    scan_spend(budget, k - start);
    return 0;
  }
  /* An email autolink: runs of the local-part alphabet around one @. */
  k = i + 1;
  bool at = false;
  while (k < limit && p[k] != '>') {
    unsigned char c = (unsigned char)p[k];
    if (c == '@') at = true;
    if (!alnum(c) && c != '@' && c != '.' && c != '-' && c != '_' && c != '+') {
      return 0;
    }
    k++;
  }
  if (k >= n || k >= limit || p[k] != '>' || !at) return 0;
  return k + 1;
}

/* An autolink's body is either an absolute URI carrying its own scheme or a
 * bare email address, and only the second needs `mailto:` prepended. The two
 * are distinguishable without asking `autolink` again: its email branch
 * accepts no `:` anywhere in the body, so a `:` means scheme and its absence
 * plus an `@` means email. */
static bool autolink_is_email(const char *body, size_t n) {
  bool at = false;
  for (size_t i = 0; i < n; i++) {
    if (body[i] == ':') return false;
    if (body[i] == '@') at = true;
  }
  return at;
}


/* A link or image title, escaped for an attribute value with the link's own
 * backslash escapes removed. `"ti\"tle"` is the title `ti"tle`; leaving the
 * backslash in emitted a `\"` the browser decodes back to a bare quote, and a
 * title is an attribute value, so a bare quote in one ends the attribute. */
/* A link destination, percent-encoded. CommonMark §6.5 requires it, and the
 * characters that need it are the ones that would otherwise change what the
 * href MEANS: a space ends it, a quote and an angle bracket do not survive
 * an HTML attribute unescaped, and a backslash is a path separator on some
 * platforms and a literal on others. */
static void esc_url(sink *s, const char *p, size_t n) {
  for (size_t i = 0; i < n; i++) {
    unsigned char c = (unsigned char)p[i];
    bool plain = c > 0x20u && c < 0x7fu && c != '"' && c != '<' && c != '>' &&
                 c != '`' && c != '\\';
    if (plain) {
      chr(s, (char)c);
      continue;
    }
    char hex[4];
    int w = snprintf(hex, sizeof hex, "%%%02X", c);
    if (w > 0) put(s, hex, (size_t)w);
  }
}

static void esc_title(sink *s, const char *p, size_t n) {
  for (size_t i = 0; i < n; i++) {
    if (p[i] == '\\' && i + 1 < n && punct((unsigned char)p[i + 1])) i++;
    esc_attr(s, p + i, 1);
  }
}

static void md_inline(sink *s, const char *p, size_t n, int depth);

/* An HTML entity, passed through unchanged: the document said `&amp;` and
 * re-escaping it would change what the page says. Every scan is capped at
 * MD_SCHEME_MAX so a run of `&` with no `;` in it costs a bounded look. */
static bool entity(const char *p, size_t n, size_t i) {
  size_t k = i + 1;
  size_t limit = i + MD_SCHEME_MAX;
  if (limit > n) limit = n;
  if (k < n && p[k] == '#') {
    k++;
    if (k < n && (p[k] == 'x' || p[k] == 'X')) {
      k++;
      size_t d = k;
      while (k < limit && hex((unsigned char)p[k])) k++;
      if (k == d || k >= n || p[k] != ';') return false;
      return true;
    }
    size_t d = k;
    while (k < limit && digit((unsigned char)p[k])) k++;
    return k > d && k < n && p[k] == ';';
  }
  size_t d = k;
  while (k < limit && alnum((unsigned char)p[k])) k++;
  return k > d && k < n && p[k] == ';';
}

/* `[label](dest "title")`, from the `[` at `i`. Fills the label, the
 * destination and the title and returns the offset just past the construct,
 * or 0. A reference link (`[a][b]`, `[a][]`) is NOT a link here: the
 * definition table is a second parser for a syntax the corpus does not use,
 * and a `[x][y]` that resolves to nothing must read as the text it is.
 *
 * EVERY scan here is backward-looking and every one of them is bounded and
 * charged. The label search charges its window up front, because a label
 * that finds nothing falls back to literal text that reads the same either
 * way. Everything after the label — the destination, the angle-bracketed
 * destination, the quoted title, the parenthesised title and the code-span
 * skip inside the label — is charged for what it walked, because a
 * construct that runs out of budget is ESCAPED and a reader can see that.
 * The destination's paren scan was the expensive one: it stopped only at an
 * unbalanced `)`, a space or a backslash, so a document of `[a](` — where
 * every `(` it meets raises the depth and nothing ever brings it back to
 * zero — gave each of its four million `[` a walk to the end. That is the
 * 16 MiB case the top-of-file comment used to claim was covered and was
 * not. */
static size_t inline_link(const char *p, size_t n, size_t i, const char **lbl,
                          size_t *lbl_n, const char **dest, size_t *dest_n,
                          const char **ttl, size_t *ttl_n, size_t *budget) {
  size_t k = i + 1;
  size_t limit = i + 1 + MD_LINK_SCAN;
  if (limit > n) limit = n;
  if (limit > k) {
    size_t want = limit - k;
    if (want > *budget) return 0;
    *budget -= want;
  }
  size_t depth = 1;
  while (k < limit) {
    if (p[k] == '\\' && k + 1 < n) {
      k += 2;
      continue;
    }
    if (p[k] == '`') {
      /* A `]` inside a code span is not the end of the label. */
      size_t r = 0;
      while (k + r < n && p[k + r] == '`') r++;
      size_t j = code_span_end(p, n, k, r, budget);
      if (j == 0) break;
      k = j + r;
      continue;
    }
    if (p[k] == '[') depth++;
    if (p[k] == ']') {
      depth--;
      if (depth == 0) break;
    }
    k++;
  }
  if (k >= limit) return 0;
  size_t close = k;
  k++;
  if (k >= n || p[k] != '(') return 0;
  k++;
  /* The destination and the title are one construct, scanned under one
   * window and one charge, because they are one backward-looking walk: a `[`
   * opens a construct that either closes or is not a construct. */
  size_t base = k;
  size_t tail = scan_limit(budget, k, n, MD_TAG_SCAN);
  while (k < tail && sp((unsigned char)p[k])) k++;
  size_t ds = k;
  if (k < tail && p[k] == '<') {
    k++;
    while (k < tail && p[k] != '>') k++;
    if (k >= tail) {
      scan_spend(budget, k - base);
      return 0;
    }
    *dest = p + ds + 1;
    *dest_n = k - ds - 1;
    k++;
  } else {
    size_t paren = 0;
    while (k < tail) {
      char c = p[k];
      if (c == '\\' && k + 1 < tail) {
        k += 2;
        continue;
      }
      if (c == '(') paren++;
      if (c == ')') {
        if (paren == 0) break;
        paren--;
      }
      if (paren == 0 && sp((unsigned char)c)) break;
      k++;
    }
    *dest = p + ds;
    *dest_n = k - ds;
    while (*dest_n > 0 && sp((unsigned char)(*dest)[*dest_n - 1])) (*dest_n)--;
  }
  size_t ts = k;
  while (k < tail && sp((unsigned char)p[k])) k++;
  if (k < tail && (p[k] == '"' || p[k] == '\'')) {
    char q = p[k];
    k++;
    ts = k;
    /* A backslash escapes the next character, so `\"` does not close the
     * title. Without this the scanner stopped at the escaped quote, the
     * whole construct failed, and the document's fallback turned the
     * destination into a bare autolink — the link came out wrong twice. */
    while (k < tail) {
      if (p[k] == '\\' && k + 1 < tail) {
        k += 2;
        continue;
      }
      if (p[k] == q) break;
      k++;
    }
    if (k >= tail) {
      scan_spend(budget, k - base);
      return 0;
    }
    *ttl = p + ts;
    *ttl_n = k - ts;
    k++;
    while (k < tail && sp((unsigned char)p[k])) k++;
  } else if (k < tail && p[k] == '(') {
    /* A parenthesised title: `[link](/url (title))`. */
    size_t d = k + 1;
    while (d < tail && sp((unsigned char)p[d])) d++;
    ts = d;
    while (d < tail && p[d] != ')') {
      if (p[d] == '\\' && d + 1 < tail) {
        d += 2;
        continue;
      }
      d++;
    }
    if (d < tail && d > ts) {
      *ttl = p + ts;
      *ttl_n = d - ts;
      k = d + 1;
    } else {
      *ttl = "";
      *ttl_n = 0;
    }
  } else {
    *ttl = "";
    *ttl_n = 0;
  }
  if (k >= tail || p[k] != ')') {
    scan_spend(budget, k - base);
    return 0;
  }
  scan_spend(budget, k + 1 - base);
  *lbl = p + i + 1;
  *lbl_n = close - i - 1;
  return k + 1;
}

/* Does a link LABEL contain a link of its own? CommonMark §6.6's "links may
 * not contain other links at any level of nesting", and the test is the
 * honest one — the same `inline_link` the caller just used, over the same
 * bytes — rather than a cheaper "there is a `](` in there" that would
 * suppress the outer link on `[a [b] c](d)`, which is a perfectly good link
 * whose label happens to carry brackets.
 *
 * This is a backward-looking scan like any other and is charged like one.
 * It is bounded twice over by the caller's label window: the label is at
 * most MD_LINK_SCAN bytes, and every `inline_link` it probes charges the
 * shared budget for its own look, so a label full of `[` cannot make the
 * check quadratic on top of the scan that found it. A run-out-of-budget
 * probe parses as "not a link", which leaves the outer link standing — the
 * same direction every other budget exhaustion in this file fails.
 *
 * Backslash escapes and code spans are stepped over exactly as
 * `opaque_at` steps over them, because a `[` inside either is text and not
 * the start of a link. */
static bool label_has_link(const char *lbl, size_t lbl_n, size_t *budget) {
  for (size_t j = 0; j < lbl_n;) {
    if (lbl[j] == '\\' && j + 1 < lbl_n) {
      j += 2;
      continue;
    }
    if (lbl[j] == '`') {
      size_t r = 0;
      while (j + r < lbl_n && lbl[j + r] == '`') r++;
      size_t e = code_span_end(lbl, lbl_n, j, r, budget);
      j = e != 0 ? e + r : j + r;
      continue;
    }
    if (lbl[j] == '[' || (lbl[j] == '!' && j + 1 < lbl_n && lbl[j + 1] == '[')) {
      bool image = lbl[j] == '!';
      const char *ilbl;
      size_t ilbl_n;
      const char *idest;
      size_t idest_n;
      const char *ittl;
      size_t ittl_n;
      size_t e = inline_link(lbl, lbl_n, image ? j + 1 : j, &ilbl, &ilbl_n,
                             &idest, &idest_n, &ittl, &ittl_n, budget);
      /* An image is not a link, and a link may contain one: CommonMark
       * example 517, `[![moon](moon.jpg)](/uri)`, is a link whose whole
       * label is an image. So an image inside a label is stepped over, not
       * counted. */
      if (e != 0) {
        if (!image) return true;
        j = e;
        continue;
      }
    }
    j++;
  }
  return false;
}

/* One delimiter run inside the emphasis window: where it is, how long it is
 * as written, how much of it is still unpaired, and whether it may open and
 * whether it may close. */
typedef struct {
  size_t at;
  size_t len;
  size_t left;
  bool open;
  bool close;
  bool gone;
} md_delim;

/* A pair the matcher settled on. The content is what lies between the two
 * runs once the delimiters each pair consumed are taken out. */
typedef struct {
  size_t open_at; /* the opener run, for ordering and for the text before it */
  size_t from;    /* first byte of the content */
  size_t to;      /* one past the last byte of the content */
  size_t after;   /* one past the closing run's consumed delimiters */
  bool strong;
} md_match;

/* CommonMark §6.2's left- and right-flanking definitions, and the two
 * extra rules `_` gets on top of them. `*` opens when left-flanking and
 * closes when right-flanking. `_` opens only when left-flanking and NOT
 * right-flanking (unless the run is preceded by punctuation) and closes only
 * when right-flanking and NOT left-flanking (unless it is followed by
 * punctuation) — that asymmetry is the whole of intraword emphasis, and it
 * is what makes `_foo_` and `snake_case_name` behave differently.
 *
 * Bytes >= 0x80 count as ordinary characters: Unicode punctuation and
 * whitespace are not distinguished here, and being wrong about one costs a
 * single emphasis run rather than the document. */
static void delim_flags(char c, bool before_ws, bool after_ws, bool before_punct,
                        bool after_punct, bool *open, bool *close) {
  bool left = !after_ws && (!after_punct || before_ws || before_punct);
  bool right = !before_ws && (!before_punct || after_ws || after_punct);
  if (c == '_') {
    *open = left && (!right || before_punct);
    *close = right && (!left || after_punct);
  } else {
    *open = left;
    *close = right;
  }
}

/* The closing backtick run of a code span opened by the `r` backticks at
 * `i`, as the offset of that run's FIRST backtick, or 0 when the span never
 * closes. Three call sites had this loop — `md_inline`'s own code-span
 * branch, `opaque_at`, and `inline_link`'s skip over a `]` inside a code
 * span — and all three were unbounded, so all three are now bounded and
 * charged once, here, because they must agree: a code span one of them can
 * see and another cannot is emphasis paired across a code span, which is
 * worse than either answer alone.
 *
 * How bad "unbounded" was, measured rather than assumed: a document of
 * backtick runs that never pair — a run of 1, then a run of 2, then a run of
 * 3 — took 1.19 s at 1 MiB and 7.05 s at 2 MiB, so each run's walk over the
 * runs after it made the pass superlinear rather than quadratic. A document
 * of plain `a` is NOT affected, because consecutive runs pair immediately;
 * the pathological shape needs run lengths that never repeat. */
static size_t code_span_end(const char *p, size_t n, size_t i, size_t r,
                            size_t *budget) {
  size_t start = i + r;
  size_t limit = scan_limit(budget, start, n, MD_TAG_SCAN);
  size_t j = start;
  while (j < limit) {
    if (p[j] != '`') {
      j++;
      continue;
    }
    size_t r2 = 0;
    while (j + r2 < limit && p[j + r2] == '`') r2++;
    if (r2 == r) {
      scan_spend(budget, j + r2 - start);
      return j;
    }
    j += r2;
  }
  scan_spend(budget, limit - start);
  return 0;
}

/* The constructs `md_inline` consumes whole before it ever looks at a
 * delimiter: a code span, an autolink, a raw tag, an image and a link. The
 * emphasis matcher has to step over exactly these, or it pairs a `*` that
 * lives inside a code span with one that lives in prose. Returns the offset
 * just past the construct, or `i` when there is none at `i`. */
static size_t opaque_at(const char *p, size_t n, size_t i, size_t *budget) {
  if (i >= n) return i;
  char c = p[i];
  if (c == '`') {
    size_t r = 0;
    while (i + r < n && p[i + r] == '`') r++;
    size_t j = code_span_end(p, n, i, r, budget);
    return j != 0 ? j + r : i + r;
  }
  if (c == '<' && i + 1 < n) {
    size_t e = autolink(p, n, i, budget);
    if (e != 0) return e;
    e = raw_tag(p, n, i, budget);
    if (e != 0) return e;
    return i;
  }
  if ((c == '[' || (c == '!' && i + 1 < n && p[i + 1] == '[')) &&
      i + 1 < n) {
    const char *lbl;
    size_t lbl_n;
    const char *dest;
    size_t dest_n;
    const char *ttl;
    size_t ttl_n;
    size_t e = inline_link(p, n, c == '!' ? i + 1 : i, &lbl, &lbl_n, &dest,
                           &dest_n, &ttl, &ttl_n, budget);
    return e != 0 ? e : i;
  }
  return i;
}

/* Emphasis and strong emphasis over the delimiter runs of one character,
 * from the run at `i` to the end of the scan window. Returns the offset just
 * past the last delimiter consumed, or 0 when nothing pairs, in which case
 * the caller emits the run as the literal text it is.
 *
 * This is CommonMark §6.2's `process_emphasis`, restricted to a single
 * character: the runs go on a stack in document order, each closer pairs
 * with the nearest opener below it that the flanking rules and the rule of
 * three allow, and a pair takes two delimiters when both runs still have
 * two. Nesting falls out of pairing the nearest opener first, which is why
 * `**a *b* c**` comes out strong outside and em inside rather than the two
 * levels swapped.
 *
 * Both bounds from the top of the file still apply, and the window is not
 * the important one: the run count is capped, and the look-back stops at
 * `openers_bottom` so a document of nothing but `*` cannot make the match
 * phase quadratic on top of the scan. */
static size_t emphasis_run(sink *s, const char *p, size_t n, size_t i, int depth,
                           size_t *budget) {
  /* KNOWN BOUNDED LIMIT — the depth cap's FALLBACK SHAPE, emphasis half.
   * Each pair nests one `md_inline` deeper, and past MD_MAX_DEPTH the run
   * is emitted as the literal text it looks like. A run of sixty `*` around
   * an `x` is thirty nested `<strong>`s in the original and, here, the
   * outer twenty-three with fourteen literal asterisks inside them. Same
   * trade as the list cap and for the same reason — `md_inline` recursing
   * into itself is a C stack overflow on a hostile document — and the same
   * caveat: the cap is not what diverges, the shape of the fallback is.
   * Two corpus documents. */
  if (depth >= (int)MD_MAX_DEPTH) return 0;
  char c = p[i];
  size_t run = 0;
  while (i + run < n && p[i + run] == c) run++;
  if (i + run >= n) return 0;
  size_t limit = i + run + MD_DELIM_SCAN;
  if (limit > n) limit = n;
  if (limit > i + run) {
    size_t want = limit - (i + run);
    if (want > *budget) return 0;
    *budget -= want;
  }

  md_delim ds[MD_MAX_DELIMS];
  md_match ms[MD_MAX_DELIMS];
  size_t nd = 0;
  for (size_t j = i; j < limit;) {
    /* A backslash-escaped `*` is a literal `*`, not a delimiter: `\*not
     * emphasized*` has no opener in it. */
    if (p[j] == '\\' && j + 1 < limit && punct((unsigned char)p[j + 1])) {
      j += 2;
      continue;
    }
    size_t skip = opaque_at(p, n, j, budget);
    if (skip > j) {
      j = skip;
      continue;
    }
    if (p[j] != c) {
      j++;
      continue;
    }
    size_t r = 0;
    while (j + r < limit && p[j + r] == c) r++;
    bool before_ws = j == 0 || sp((unsigned char)p[j - 1]);
    bool after_ws = (j + r >= n) || sp((unsigned char)p[j + r]);
    bool before_punct = j > 0 && punct((unsigned char)p[j - 1]);
    bool after_punct = (j + r < n) && punct((unsigned char)p[j + r]);
    bool can_open;
    bool can_close;
    delim_flags(c, before_ws, after_ws, before_punct, after_punct, &can_open,
                &can_close);
    if (nd < MD_MAX_DELIMS) {
      ds[nd].at = j;
      ds[nd].len = r;
      ds[nd].left = r;
      ds[nd].open = can_open;
      ds[nd].close = can_close;
      ds[nd].gone = false;
      nd++;
    }
    j += r;
  }
  if (nd == 0) return 0;

  size_t nm = 0;
  size_t openers_bottom = 0;
  for (size_t k = 0; k < nd; k++) {
    while (!ds[k].gone && ds[k].left > 0u && ds[k].close) {
      size_t o = nd;
      for (size_t j = k; j-- > openers_bottom;) {
        if (ds[j].gone || !ds[j].open) continue;
        /* Rule of three: when either run can both open and close, the sum
         * of the two lengths must not be a multiple of three unless both
         * lengths are. This is what keeps `a*b **c` from pairing the `*`
         * after `a` with the one before `c`. */
        bool both = (ds[j].open && ds[j].close) || (ds[k].open && ds[k].close);
        if (both && (ds[j].len + ds[k].len) % 3u == 0u &&
            !(ds[j].len % 3u == 0u && ds[k].len % 3u == 0u)) {
          continue;
        }
        o = j;
        break;
      }
      if (o == nd) {
        /* A closer with no opener below it: it is ordinary text, and it
         * also marks the bottom of the region a later closer may reach. */
        openers_bottom = k == 0 ? 0 : k - 1;
        if (!ds[k].open) ds[k].gone = true;
        break;
      }
      size_t use = (ds[o].left >= 2u && ds[k].left >= 2u) ? 2u : 1u;
      if (nm < MD_MAX_DELIMS) {
        /* The opener's delimiters are consumed from the RIGHT of its run and
         * the closer's from the LEFT, so a run of three pairs as strong
         * first and em second — `<em><strong>foo</strong></em>` for
         * `***foo***`, not a strong wrapped around a stray asterisk. The
         * content is what is left between the two halves. */
        ms[nm].open_at = ds[o].at;
        ms[nm].from = ds[o].at + ds[o].left;
        ms[nm].to = ds[k].at + (ds[k].len - ds[k].left);
        ms[nm].after = ms[nm].to + use;
        ms[nm].strong = use >= 2u;
        nm++;
      }
      ds[o].left -= use;
      ds[k].left -= use;
      /* Anything between the pair can no longer match: the opener is spent
       * on this closer. */
      for (size_t j = o + 1; j < k; j++) ds[j].gone = true;
      if (ds[o].left == 0u) ds[o].gone = true;
      if (ds[k].left == 0u) ds[k].gone = true;
    }
  }
  if (nm == 0) return 0;

  /* Emit outermost first: a pair that starts earlier and ends later encloses
   * the ones between, and the recursion into its content re-derives those.
   * Insertion sort, because `nm` is bounded and this runs on every opener. */
  for (size_t a = 1; a < nm; a++) {
    md_match key = ms[a];
    size_t b = a;
    while (b > 0 && (ms[b - 1].from > key.from ||
                     (ms[b - 1].from == key.from && ms[b - 1].to < key.to))) {
      ms[b] = ms[b - 1];
      b--;
    }
    ms[b] = key;
  }
  size_t pos = i;
  for (size_t a = 0; a < nm; a++) {
    if (ms[a].from < pos) continue; /* already inside an emitted pair */
    size_t pre = ms[a].open_at > pos ? ms[a].open_at - pos : 0u;
    md_inline(s, p + pos, pre, depth + 1);
    lit(s, ms[a].strong ? "<strong>" : "<em>");
    md_inline(s, p + ms[a].from, ms[a].to - ms[a].from, depth + 1);
    lit(s, ms[a].strong ? "</strong>" : "</em>");
    pos = ms[a].after;
  }
  return pos > i ? pos : 0;
}

/* `~~struck~~`, GFM strikethrough. Only the double form counts: a single
 * tilde is a tilde, and treating every `~a~` as a strike is how ordinary
 * prose loses characters. A run of three or more is not a strike either —
 * `~~~not~~~` is the literal text it looks like — and the closer has to be
 * right-flanking, so `~~not ~~` keeps its trailing run where it is.
 * Bounded and budgeted exactly as `emphasis_run` is. */
static size_t strikethrough(sink *s, const char *p, size_t n, size_t i,
                           int depth, size_t *budget) {
  if (i + 2 > n || p[i + 1] != '~' || p[i + 2] == '~') return 0;
  if (depth >= (int)MD_MAX_DEPTH) return 0;
  bool before_ws = i == 0 || sp((unsigned char)p[i - 1]);
  bool after_ws = (i + 2 >= n) || sp((unsigned char)p[i + 2]);
  bool can_open;
  bool can_close;
  delim_flags('~', before_ws, after_ws,
              i > 0 && punct((unsigned char)p[i - 1]),
              (i + 2 < n) && punct((unsigned char)p[i + 2]), &can_open,
              &can_close);
  if (!can_open) return 0;
  size_t limit = i + 2 + MD_DELIM_SCAN;
  if (limit > n) limit = n;
  if (limit > i + 2) {
    size_t want = limit - (i + 2);
    if (want > *budget) return 0;
    *budget -= want;
  }
  for (size_t j = i + 2; j + 1 < limit; j++) {
    if (p[j] == '\\' && j + 1 < limit && punct((unsigned char)p[j + 1])) {
      j++;
      continue;
    }
    size_t skip = opaque_at(p, n, j, budget);
    if (skip > j) {
      j = skip - 1;
      continue;
    }
    if (p[j] != '~' || p[j + 1] != '~' || p[j + 2] == '~') continue;
    bool cb_ws = sp((unsigned char)p[j - 1]);
    bool ca_ws = (j + 2 >= n) || sp((unsigned char)p[j + 2]);
    bool c_open;
    bool c_close;
    delim_flags('~', cb_ws, ca_ws, punct((unsigned char)p[j - 1]),
                (j + 2 < n) && punct((unsigned char)p[j + 2]), &c_open,
                &c_close);
    if (!c_close || c_open) continue;
    lit(s, "<del>");
    md_inline(s, p + i + 2, j - (i + 2), depth + 1);
    lit(s, "</del>");
    return j + 2;
  }
  return 0;
}

/* GFM literal autolinks. The original enables comrak's autolink extension,
 * so a URL typed in prose is a link there and must be one here; `www.` gets
 * the `http://` the extension prepends.
 *
 * THE ONE BACKWARD-LOOKING SCAN IN THIS FILE WITH NO BUDGET, and it does
 * not need one. This loop stops at the first byte that is whitespace, `<` or
 * `"`, so two of the three are stop bytes for the NEXT candidate too: any
 * candidate that starts inside this scan's span is preceded by one of them,
 * and a candidate that starts after it is scanned from after it. The scans
 * are therefore disjoint and their total is the length of the input, not a
 * multiple of it. A call that finds no scheme prefix returns without having
 * scanned at all. The trailing-punctuation trim walks back over bytes this
 * scan already walked, so it is covered by the same argument. */
static size_t bare_autolink(sink *s, const char *p, size_t n, size_t i) {
  bool www = n - i >= 4 && memcmp(p + i, "www.", 4) == 0;
  size_t scheme_end;
  if (www) {
    scheme_end = i + 4;
  } else if (n - i >= 7 && memcmp(p + i, "http://", 7) == 0) {
    scheme_end = i + 7;
  } else if (n - i >= 8 && memcmp(p + i, "https://", 8) == 0) {
    scheme_end = i + 8;
  } else {
    return 0;
  }
  size_t k = scheme_end;
  while (k < n) {
    unsigned char c = (unsigned char)p[k];
    if (c <= 0x20u || c == '<' || c == '"') break;
    k++;
  }
  /* Sentence punctuation at the end of a URL is the sentence's, not the
   * link's, and a `)` only belongs to the URL if something opened it. */
  while (k > scheme_end && strchr(".,;:!?'", p[k - 1]) != NULL) k--;
  if (k <= scheme_end) return 0;
  lit(s, "<a href=\"");
  if (www) lit(s, "http://");
  esc_attr(s, p + i, k - i);
  lit(s, "\">");
  esc(s, p + i, k - i);
  lit(s, "</a>");
  return k;
}

static void md_inline(sink *s, const char *p, size_t n, int depth) {
  /* One scan budget for the whole run: every backward-looking scan spends
   * from it, so the inline pass is linear in `n` by construction rather than
   * by the accident of which characters a hostile document happens to use. */
  size_t i = 0;
  /* Spaces seen since the last non-space, written only once it is known
   * they are not a hard break's marker. */
  size_t sp_pending = 0;
  size_t budget = n * MD_SCAN_BUDGET_PER_BYTE + MD_DELIM_SCAN;
  while (i < n) {
    char c = p[i];
    /* Any construct that starts here comes AFTER the spaces held back, so
     * they are written out first — except a newline, which may be a hard
     * break that swallows them. */
    if (c != ' ' && c != '\n') {
      while (sp_pending > 0) {
        chr(s, ' ');
        sp_pending--;
      }
    }
    if (c == '\\' && i + 1 < n && p[i + 1] == '\n') {
      /* The other half of a hard line break: a backslash at the end of a
       * line is one, exactly as two trailing spaces are. */
      lit(s, "<br />\n");
      i += 2;
      continue;
    }
    if (c == '\\' && i + 1 < n && punct((unsigned char)p[i + 1])) {
      /* A backslash escape yields the CHARACTER it escapes, as text — so
       * `\<script>` is a literal `<script>`, not a script element. Emitting
       * the character raw inverted the escape into the very markup it was
       * written to suppress. */
      esc(s, p + i + 1, 1);
      i += 2;
      continue;
    }
    if (c == '`') {
      size_t r = 0;
      while (i + r < n && p[i + r] == '`') r++;
      size_t end = code_span_end(p, n, i, r, &budget);
      if (end == 0) {
        put(s, p + i, r);
        i += r;
        continue;
      }
      /* A code span is the one context that must never emit markup: the
       * author wrote backticks precisely to say "this is literal". The
       * content is therefore ESCAPED, exactly as the block-code path
       * (`md_code`) escapes it, and a line ending inside it becomes a
       * space, as the original's does. Emitting it verbatim put a live
       * `<img onerror=…>` on the page for any document quoting a log
       * line. */
      lit(s, "<code>");
      size_t cs = i + r, ce = end;
      if (ce > cs + 1 && p[cs] == ' ' && p[ce - 1] == ' ') {
        cs++;
        ce--;
      }
      for (size_t k = cs; k < ce; k++) {
        if (s->in_table_cell && p[k] == '\\' && k + 1 < ce && p[k + 1] == '|') {
          k++;
        }
        if (p[k] == '\n') chr(s, ' ');
        else esc(s, p + k, 1);
      }
      lit(s, "</code>");
      i = end + r;
      continue;
    }
    if (c == '<' && i + 1 < n) {
      size_t ae = autolink(p, n, i, &budget);
      if (ae != 0) {
        /* The angle brackets delimit the autolink; they are not part of
         * the URL, and escaping them into the href would produce
         * `&lt;https://x&gt;` as the link target. The email form gets the
         * `mailto:` the original prepends: without it `href="a@b.c"` names
         * a RELATIVE url and the link 404s. */
        lit(s, "<a href=\"");
        if (autolink_is_email(p + i + 1, ae - i - 2)) lit(s, "mailto:");
        esc_attr(s, p + i + 1, ae - i - 2);
        lit(s, "\">");
        esc(s, p + i + 1, ae - i - 2);
        lit(s, "</a>");
        i = ae;
        continue;
      }
      size_t te = raw_tag(p, n, i, &budget);
      if (te != 0) {
        put(s, p + i, te - i);
        i = te;
        continue;
      }
    }
    if (c == '!' && i + 1 < n && p[i + 1] == '[') {
      const char *lbl;
      size_t lbl_n;
      const char *dest;
      size_t dest_n;
      const char *ttl;
      size_t ttl_n;
      size_t e = inline_link(p, n, i + 1, &lbl, &lbl_n, &dest, &dest_n, &ttl,
                             &ttl_n, &budget);
      if (e != 0) {
        lit(s, "<img src=\"");
        esc_url(s, dest, dest_n);
        lit(s, "\" alt=\"");
        esc_attr(s, lbl, lbl_n);
        lit(s, "\"");
        if (ttl_n > 0) {
          lit(s, " title=\"");
          esc_title(s, ttl, ttl_n);
          lit(s, "\"");
        }
        lit(s, " />");
        i = e;
        continue;
      }
    }
    if (c == '[') {
      const char *lbl;
      size_t lbl_n;
      const char *dest;
      size_t dest_n;
      const char *ttl;
      size_t ttl_n;
      size_t e = inline_link(p, n, i, &lbl, &lbl_n, &dest, &dest_n, &ttl,
                             &ttl_n, &budget);
      if (e != 0 && !label_has_link(lbl, lbl_n, &budget)) {
        lit(s, "<a href=\"");
        esc_url(s, dest, dest_n);
        lit(s, "\"");
        if (ttl_n > 0) {
          lit(s, " title=\"");
          esc_title(s, ttl, ttl_n);
          lit(s, "\"");
        }
        lit(s, ">");
        md_inline(s, lbl, lbl_n, depth + 1);
        lit(s, "</a>");
        i = e;
        continue;
      }
      /* CommonMark §6.6: a link may not contain another link. The one that
       * loses is the OUTER, not the inner — spec example 518,
       * `[foo [bar](/uri)](/uri)`, renders as `[foo <a href="/uri">bar</a>](/uri)`
       * — and comrak agrees. So a `[` whose label holds a link of its own is
       * not a link: its bracket is emitted as the text it is and the scan
       * resumes one byte later, which finds the inner link on the next step.
       * This is not a flag threaded through `md_inline`, which is what it
       * would take to suppress the inner link instead, and it does not need
       * to be: the outer never becomes a link, so there is nothing for the
       * inner one to be nested inside. The label is still rendered by
       * `md_inline` when there is no inner link, so emphasis, code spans and
       * images inside a label keep working — CommonMark ex. 516 and 517. */
      if (e != 0) {
        esc(s, &c, 1);
        i++;
        continue;
      }
    }
    if (c == '*' || c == '_') {
      size_t e = emphasis_run(s, p, n, i, depth, &budget);
      if (e != 0) {
        i = e;
        continue;
      }
      /* Nothing pairs with this run, so the WHOLE run is the literal text it
       * looks like. Emitting it in one go is what stops `***` from being
       * re-examined as a fresh one-character run at the next byte. */
      size_t skip = 1;
      while (i + skip < n && p[i + skip] == c) skip++;
      esc(s, p + i, skip);
      i += skip;
      continue;
    }
    if (c == '~') {
      size_t e = strikethrough(s, p, n, i, depth, &budget);
      if (e != 0) {
        i = e;
        continue;
      }
    }
    if ((c == 'h' || c == 'w' || c == 'm') && (i == 0 || !alnum((unsigned char)p[i - 1]))) {
      size_t e = bare_autolink(s, p, n, i);
      if (e != 0) {
        i = e;
        continue;
      }
    }
    if (c == '&' && entity(p, n, i)) {
      size_t e = i + 1;
      while (e < n && p[e] != ';') e++;
      put(s, p + i, e - i + 1);
      i = e + 1;
      continue;
    }
    if (c == '\n') {
      /* A hard break is two or more trailing spaces; anything less is a soft
       * one and stays a newline in the source of the paragraph. The spaces
       * are held back rather than written as they are scanned, because a
       * hard break swallows them and the original does not emit them. */
      if (sp_pending >= 2u) {
        lit(s, "<br />\n");
      } else {
        while (sp_pending > 0) {
          chr(s, ' ');
          sp_pending--;
        }
        lit(s, "\n");
      }
      sp_pending = 0;
      i++;
      continue;
    }
    if (c == ' ') {
      sp_pending++;
      i++;
      continue;
    }
    esc(s, &c, 1);
    i++;
  }
  while (sp_pending > 0) {
    chr(s, ' ');
    sp_pending--;
  }
}

/* -------------------------------------------------------------- blocks -- */

static void md_blocks(sink *s, const char *src, size_t n, int depth, bool tight);

/* A GFM task marker at the head of an item's content: `[ ]`, `[x]` or
 * `[X]`, followed by whitespace or the end of the text. Returns the offset
 * just past it, or 0. */
static size_t task_mark(const char *p, size_t n, bool *checked) {
  if (n < 3 || p[0] != '[' || p[2] != ']') return 0;
  if (p[1] != ' ' && p[1] != 'x' && p[1] != 'X') return 0;
  if (n > 3 && !sp((unsigned char)p[3])) return 0;
  *checked = p[1] != ' ';
  return n > 3 ? 4u : 3u;
}

/* Can this line interrupt a paragraph? CommonMark §5.2 is narrower than
 * "is a list marker": an ordered marker only interrupts when the list it
 * starts runs from 1, and an EMPTY item never interrupts at all. Without
 * those two clauses the sentence
 *
 *     The number of windows in my house is
 *     14.  The number of doors is 6.
 *
 * renders as a paragraph followed by a list, and a bare `*` on its own line
 * splits the paragraph above it. */
static bool interrupts_para(const char *p, size_t n) {
  marker m;
  if (!list_marker(p, n, &m)) return false;
  if (n == m.tb) return false;         /* `-` alone: an empty item */
  return !m.ordered || m.start == 1u;
}

/* Is this line a paragraph continuation, or does it start a new block? The
 * tests that can interrupt a paragraph are CommonMark's; a line that only
 * LOOKS like one (a `- ` bullet) does not. */
static bool starts_block(const char *p, size_t n) {
  if (n == 0) return true;
  char fc;
  size_t fl;
  if (fence_open(p, n, &fc, &fl)) return true;
  if (p[0] == '>') return true;
  if (atx(p, n) > 0) return true;
  if (rule(p, n)) return true;
  if (interrupts_para(p, n)) return true;
  if (n < 2 || p[0] != '<') return false;
  /* HTML block types 1 to 6 interrupt a paragraph; type 7, a tag alone on
   * its line, does not — `a <b> c` stays one paragraph. */
  int hk = html_kind(p, n);
  return hk == 1 || hk == 2 || hk == 6;
}

/* Consumes a paragraph: every line until a blank one, a line that starts a
 * new block, or a setext underline. The underline closes the paragraph it
 * follows WHATEVER that paragraph's length — CommonMark §4.3 has no
 * "exactly one line" rule, and `Foo\nBar\n---` is an h2 whose text is two
 * lines, not a paragraph followed by a thematic break. The check runs
 * before `starts_block` so that `Title\n-----` is a heading rather than
 * text plus a rule, and it runs only once a line has been collected, so a
 * `---` opening a document is still the thematic break the frontmatter
 * pre-pass is looking for. */
static size_t md_para(sink *s, const char *src, size_t n, size_t pos,
                      int depth, bool tight) {
  size_t cur = pos, count = 0;
  sink text;
  sink_init(&text);
  while (cur < n) {
    line_t l;
    line_at(src, n, cur, &l);
    if (blank(&l)) break;
    const char *t;
    size_t tn;
    trim(l.p, l.n, &t, &tn);
    if (count >= 1) {
      int sv = setext(t, tn);
      if (sv > 0) {
        /* KNOWN BOUNDED LIMIT — setext underline after a list item's lazy
         * continuation. A `---` under a paragraph that is a list item's
         * lazy continuation should close that ITEM's paragraph as a
         * heading; here it does not, because `md_para` cannot see the
         * list. `item_body` has already de-indented the item's content into
         * a buffer by the time `md_blocks` runs, and the buffer carries no
         * record of which container the lines came from, so the setext test
         * sees an ordinary paragraph. Fixing it means the setext test asks
         * the container stack where the line is, which is the container
         * parsing this block layer does not do. Chosen, not missed: the
         * alternative is a second, partial model of block structure
         * threaded through every block function. Two corpus documents. */
        lit(s, sv == 1 ? "<h1>" : "<h2>");
        md_inline(s, text.out.ptr != NULL ? text.out.ptr : "", text.out.len,
                  depth + 1);
        lit(s, sv == 1 ? "</h1>\n" : "</h2>\n");
        kbc_str_free(&text.out);
        if (text.oom) s->oom = true;
        return l.next;
      }
    }
    if (count > 0) {
      if (starts_block(t, tn)) break;
      chr(&text, '\n');
    }
    trim_para_line(l.p, l.n, &t, &tn);
    put(&text, t, tn);
    count++;
    cur = l.next;
  }
  if (count == 0) {
    kbc_str_free(&text.out);
    if (text.oom) s->oom = true;
    return cur;
  }
  if (!tight) lit(s, "<p>");
  md_inline(s, text.out.ptr != NULL ? text.out.ptr : "", text.out.len,
            depth + 1);
  if (!tight) lit(s, "</p>");
  chr(s, '\n');
  s->ends_tight_para = tight;
  kbc_str_free(&text.out);
  if (text.oom) s->oom = true;
  return cur;
}

/* An ATX heading, `#` through `######`. The closing run of `#` is dropped,
 * which is what makes `# Title #` and `# Title` the same heading. */
static void md_atx(sink *s, const char *p, size_t n, int level, int depth) {
  size_t i = (size_t)level;
  while (i < n && sp((unsigned char)p[i])) i++;
  size_t e = n;
  while (e > i && sp((unsigned char)p[e - 1])) e--;
  /* A closing run of `#` is a closing run only when a space precedes it:
   * `# foo#` is a heading whose text is `foo#`, not `foo`. */
  size_t h = e;
  while (h > i && p[h - 1] == '#') h--;
  if (h < e && (h == i || sp((unsigned char)p[h - 1]))) e = h;
  while (e > i && sp((unsigned char)p[e - 1])) e--;
  lit(s, "<h");
  chr(s, (char)('0' + level));
  lit(s, ">");
  md_inline(s, p + i, e - i, depth + 1);
  lit(s, "</h");
  chr(s, (char)('0' + level));
  lit(s, ">\n");
}

/* A fenced or indented code block. The content is escaped and nothing else
 * is interpreted inside it: that is the whole contract of a code block, and
 * it is the reason the callout pre-pass refuses to touch fenced regions. */
static void md_code(sink *s, const char *p, size_t n, const char *lang,
                    size_t lang_n) {
  lit(s, "<pre><code");
  if (lang_n > 0) {
    lit(s, " class=\"language-");
    esc_attr(s, lang, lang_n);
    lit(s, "\"");
  }
  lit(s, ">");
  esc(s, p, n);
  lit(s, "</code></pre>\n");
}

/* A GFM table. The header row, the `|---|:--:|` delimiter row and the body
 * rows; a row whose cell count does not match the header is a paragraph, so
 * a stray `|` in prose does not become a one-cell table. */
static size_t md_table(sink *s, const char *src, size_t n, size_t pos,
                       int depth);

/* Splits a table row on its unescaped pipes into trimmed cell spans, and
 * reports whether the row is a valid `|---|:--:|` delimiter row.
 *
 * Returns 0 when the row has no pipe at all (so prose containing a `|` in a
 * word is not a table), MD_MAX_CELLS + 1 when it is wider than the bound —
 * a defined "this is a paragraph" rather than a truncated table — and the
 * cell count otherwise. */
static size_t row_cells(const char *p, size_t n, const char **starts,
                        size_t *lens, bool *is_delim) {
  size_t count = 0;
  size_t start = 0;
  size_t i = 0;
  bool any_pipe = false;
  if (i < n && p[i] == '|') {
    i++;
    start = 1;
  }
  for (; i <= n; i++) {
    bool boundary = (i == n);
    if (!boundary && p[i] == '|') {
      if (i > 0 && p[i - 1] == '\\') {
        boundary = false;
      } else {
        boundary = true;
        any_pipe = true;
      }
    }
    if (!boundary) continue;
    /* A row that ends with `|` closes its last cell; the empty span past it
     * is not a column. Without this, `| a | b |` counts four cells and no
     * table ever matches its own delimiter row. */
    if (i == n && start >= n) break;
    if (count >= MD_MAX_CELLS) return MD_MAX_CELLS + 1u;
    const char *t;
    size_t tn;
    trim(p + start, i - start, &t, &tn);
    starts[count] = t;
    lens[count] = tn;
    count++;
    start = i + 1;
  }
  if (!any_pipe) return 0;
  *is_delim = count > 0;
  for (size_t c = 0; c < count && *is_delim; c++) {
    const char *q = starts[c];
    size_t ql = lens[c];
    size_t dashes = 0;
    for (size_t k = 0; k < ql; k++) {
      if (q[k] == '-') {
        dashes++;
      } else if (q[k] == ':' && (k == 0 || k == ql - 1)) {
        continue;
      } else {
        dashes = 0;
        break;
      }
    }
    if (dashes == 0) *is_delim = false;
  }
  return count;
}

static size_t md_table(sink *s, const char *src, size_t n, size_t pos,
                       int depth) {
  const char *hs[MD_MAX_CELLS];
  size_t hl[MD_MAX_CELLS];
  bool delim = false;
  line_t head;
  line_at(src, n, pos, &head);
  const char *ht;
  size_t htn;
  trim(head.p, head.n, &ht, &htn);
  size_t hcols = row_cells(ht, htn, hs, hl, &delim);
  if (hcols == 0 || hcols > MD_MAX_CELLS) return 0;
  line_t sep;
  line_at(src, n, head.next, &sep);
  const char *st;
  size_t stn;
  trim(sep.p, sep.n, &st, &stn);
  const char *ss[MD_MAX_CELLS];
  size_t sl[MD_MAX_CELLS];
  bool is_sep = false;
  size_t scols = row_cells(st, stn, ss, sl, &is_sep);
  if (!is_sep || scols != hcols) return 0;

  s->in_table_cell = true;
  lit(s, "<table>\n<thead>\n<tr>\n");
  /* The alignment attribute, the way the original writes it: `align` on the
   * header cell AND on every body cell, not a style on the header alone. The
   * two render the same, and matching the original's bytes is the whole
   * point of the page wrapper. */
  char align[MD_MAX_CELLS];
  for (size_t c = 0; c < hcols; c++) {
    const char *q = ss[c];
    size_t ql = sl[c];
    bool left = ql > 0 && q[0] == ':';
    bool right = ql > 0 && q[ql - 1] == ':';
    align[c] = (left && right) ? 'c' : right ? 'r' : left ? 'l' : 0;
    lit(s, "<th");
    if (align[c] != 0) {
      lit(s, " align=\"");
      lit(s, align[c] == 'c' ? "center" : align[c] == 'r' ? "right" : "left");
      lit(s, "\"");
    }
    lit(s, ">");
    md_inline(s, hs[c], hl[c], depth + 1);
    lit(s, "</th>\n");
  }
  lit(s, "</tr>\n</thead>\n");

  /* An empty `<tbody></tbody>` is not what the original writes: a table with
   * no body rows goes straight from `</thead>` to `</table>`. */
  bool any_row = false;
  for (size_t q = sep.next; q < n;) {
    line_t b;
    line_at(src, n, q, &b);
    if (blank(&b)) break;
    const char *bt;
    size_t btn;
    trim(b.p, b.n, &bt, &btn);
    if (starts_block(bt, btn)) break;
    const char *bcs[MD_MAX_CELLS];
    size_t bcl[MD_MAX_CELLS];
    bool bd = false;
    if (row_cells(bt, btn, bcs, bcl, &bd) == 0) break;
    any_row = true;
    q = b.next;
  }
  if (any_row) lit(s, "<tbody>\n");

  size_t cur = sep.next;
  while (cur < n) {
    line_t l;
    line_at(src, n, cur, &l);
    if (blank(&l)) break;
    const char *t;
    size_t tn;
    trim(l.p, l.n, &t, &tn);
    if (starts_block(t, tn)) break;
    const char *cs[MD_MAX_CELLS];
    size_t cl[MD_MAX_CELLS];
    bool d2 = false;
    size_t cols = row_cells(t, tn, cs, cl, &d2);
    /* GFM §4.10: the body rows may vary in width. A short row gets empty
     * cells and a long one is cut at the header's width. Bailing out of the
     * table instead spilled the rest of it into the page as a paragraph of
     * raw pipes, which is the most visibly broken of the supported
     * constructs. */
    if (cols == 0) break;
    lit(s, "<tr>\n");
    for (size_t c = 0; c < hcols; c++) {
      lit(s, "<td");
      if (align[c] != 0) {
        lit(s, " align=\"");
        lit(s, align[c] == 'c' ? "center" : align[c] == 'r' ? "right" : "left");
        lit(s, "\"");
      }
      lit(s, ">");
      if (c < cols) md_inline(s, cs[c], cl[c], depth + 1);
      lit(s, "</td>\n");
    }
    lit(s, "</tr>\n");
    cur = l.next;
  }
  if (any_row) lit(s, "</tbody>\n");
  lit(s, "</table>\n");
  s->in_table_cell = false;
  return cur;
}

/* Do two markers belong to the same list? A change of bullet character or of
 * indent starts a NEW list, as does a change between ordered and unordered.
 * The delimiter (`.` against `)`) does not: `1. a\n2) b` is one list, and
 * the original renders it as one. */
static bool same_list(const marker *a, const marker *b) {
  if (a->ordered != b->ordered) return false;
  size_t lo = a->indent < b->indent ? a->indent : b->indent;
  size_t hi = a->indent < b->indent ? b->indent : a->indent;
  /* Up to three columns of drift is the same list, which is what makes
   *
   *     - a
   *      - b
   *        - c
   *
   * one list of progressively indented items rather than three sibling
   * lists. Four or more columns is a child list, and starts a new one. */
  if (hi - lo >= 4u) return false;
  return a->ordered || a->bullet == b->bullet;
}

/* One list item's content, de-indented to the item's text column: the rest of
 * the marker line, then every following line reaching that column, ACROSS a
 * blank line as long as the content resumes at that column. A list item may
 * contain any number of blocks (CommonMark §5), and stopping at the first
 * blank line is what used to push an indented quote, a nested list or an
 * indented code block out of its item and into the document as a sibling of
 * the whole list. The blank line is carried into the item's text as a blank
 * line, because that is what makes the item two blocks rather than one run.
 *
 * `body` may be NULL, in which case nothing is collected and the walk is a
 * measurement — the looseness pre-pass below needs the same answer without
 * the copy. `*blank_inside` reports a blank line that the item spans, which
 * is one of the two ways a list becomes loose. Returns the offset of the
 * first line that is not the item's. */
static size_t item_body(const char *src, size_t n, size_t pos, const marker *m,
                        sink *body, bool *blank_inside) {
  *blank_inside = false;
  line_t l;
  line_at(src, n, pos, &l);
  if (body != NULL) put(body, l.p + m->tb, l.n - m->tb);
  size_t j = l.next;
  while (j < n) {
    line_t c;
    line_at(src, n, j, &c);
    if (blank(&c)) {
      size_t k = c.next;
      while (k < n) {
        line_t b;
        line_at(src, n, k, &b);
        if (!blank(&b)) break;
        k = b.next;
      }
      if (k >= n) break;
      line_t nx;
      line_at(src, n, k, &nx);
      if (columns(nx.p, nx.n) < m->text_col) break;
      *blank_inside = true;
      if (body != NULL) chr(body, '\n');
      j = k;
      continue;
    }
    if (columns(c.p, c.n) < m->text_col) break;
    if (body != NULL) {
      const char *cp = c.p;
      size_t cn = c.n;
      strip_columns(&cp, &cn, m->text_col);
      chr(body, '\n');
      put(body, cp, cn);
    }
    j = c.next;
  }
  return j;
}

/* A list. Nesting is by column: an item's content is everything indented to
 * its text column, and that content is rendered as blocks, so a nested list
 * inside an item is a real nested list rather than a flat run of bullets.
 */
/* Looseness is decided BEFORE the first `<li>` is emitted, because CommonMark
 * §5.2 makes it a property of the whole list: "if a list is loose, all its
 * constituent list items are loose". Discovering it while emitting meant the
 * items before the first blank line came out tight and the ones after came
 * out loose, so the first bullet of every ordinary loose list lost its
 * paragraph margin. Two things make a list loose: two items separated by a
 * blank line, and an item that spans a blank line itself.
 *
 * KNOWN BOUNDED LIMIT — outer-list looseness when a nested list absorbs the
 * continuation. For
 *
 *     - alpha
 *       - inner one
 *       - inner two
 *
 *         indented code
 *
 * the blank line belongs to the INNER list, and the outer one is tight, so
 * the original writes `<li>alpha` with no paragraph. Here the outer item
 * comes out loose, because `item_body` walks the outer item by COLUMN: the
 * blank line and the code block beneath it are indented past the outer
 * item's text column, so the outer item spans it and `blank_inside` fires.
 * The looseness rule is right and its input is wrong — deciding it needs to
 * know that those lines opened a nested list, which is container parsing,
 * not a line-oriented walk. One corpus document. Chosen, not missed. */
static size_t md_list(sink *s, const char *src, size_t n, size_t pos,
                      int depth) {
  if (depth >= (int)MD_MAX_DEPTH) {
    /* KNOWN BOUNDED LIMIT — the depth cap's FALLBACK SHAPE. Past
     * MD_MAX_DEPTH the list stops nesting and its content is rendered as
     * paragraphs, so the source below the cap comes out as literal `- d24`
     * text inside a `<p>` where the original has one more `<ul><li>`. The
     * CAP is not negotiable and is not what is wrong here: `md_blocks`,
     * `md_list` and `md_inline` recurse into one another and a hostile
     * document of 200 000 nested `- ` markers is a C stack overflow, which
     * is a crash and not a rendering difference. What diverges is the SHAPE
     * of the fallback: the content is emitted, not dropped, so no text is
     * lost and the page stays well-formed, but it is emitted as prose rather
     * than as a list. Emitting the remaining markers as a further level of
     * list instead would need the recursion this cap exists to bound.
     * Five corpus documents. */
    return md_para(s, src, n, pos, depth, false);
  }
  line_t first;
  line_at(src, n, pos, &first);
  marker m0;
  if (!list_marker(first.p, first.n, &m0)) return md_para(s, src, n, pos, depth, false);

  bool loose = false;
  size_t end = pos;
  for (size_t scan = pos; scan < n;) {
    line_t l;
    line_at(src, n, scan, &l);
    bool blank_inside = false;
    size_t j = item_body(src, n, scan, &m0, NULL, &blank_inside);
    if (blank_inside) loose = true;
    scan = j;
    if (scan >= n) {
      end = scan;
      break;
    }
    line_t nx;
    line_at(src, n, scan, &nx);
    marker mn;
    bool cont = list_marker(nx.p, nx.n, &mn) && same_list(&mn, &m0);
    if (!cont) {
      if (!blank(&nx)) {
        end = scan;
        break;
      }
      size_t k = nx.next;
      while (k < n) {
        line_t b;
        line_at(src, n, k, &b);
        if (!blank(&b)) break;
        k = b.next;
      }
      if (k >= n) {
        end = k;
        break;
      }
      line_t n2;
      line_at(src, n, k, &n2);
      if (list_marker(n2.p, n2.n, &mn) && same_list(&mn, &m0)) {
        cont = true;
      } else if (columns(n2.p, n2.n) >= m0.text_col) {
        cont = true;
      }
      if (!cont) {
        end = k;
        break;
      }
      scan = k;
      /* Reaching here means a BLANK line separated two items, or a blank
       * line inside one — either way the list is loose. An item that simply
       * ends where the next one begins is not. */
      loose = true;
    }
  }

  lit(s, m0.ordered ? "<ol" : "<ul");
  if (m0.ordered && m0.start != 1) {
    char num[24];
    int w = snprintf(num, sizeof num, "%zu", m0.start);
    if (w > 0 && (size_t)w < sizeof num) {
      lit(s, " start=\"");
      put(s, num, (size_t)w);
      lit(s, "\"");
    }
  }
  lit(s, ">\n");

  for (size_t cur = pos; cur < end;) {
    line_t l;
    line_at(src, n, cur, &l);
    if (blank(&l)) {
      /* Blank lines BETWEEN items belong to no item; step over them. */
      cur = l.next;
      continue;
    }
    marker m;
    if (!list_marker(l.p, l.n, &m)) break;
    bool blank_inside = false;
    sink body;
    sink_init(&body);
    cur = item_body(src, n, cur, &m, &body, &blank_inside);
    const char *bp = body.out.ptr != NULL ? body.out.ptr : "";
    size_t bn = body.out.len;
    bool checked = false;
    size_t tm = task_mark(bp, bn, &checked);
    /* A loose item's content starts on its own line, a tight item's does
     * not, and a tight item's `</li>` follows its text directly. Both are
     * the original's shape, and both are visible in a diff of two pages
     * that are supposed to differ only in the prose.
     *
     * "Tight" is not the whole rule. A tight item whose FIRST BLOCK is not
     * a paragraph still gets the newline, because that is the only thing
     * that can go on the `<li>` line: the original writes `<li>` then a
     * newline then `<blockquote>`, `<h1>`, `<pre>` or a nested `<ul>`, and
     * writes `<li>text` for the one case where a paragraph can follow it
     * directly. Getting this wrong glued the block tag to the `<li>`:
     * `<li><blockquote>`. The test is the item's own first line, since a
     * later block in the same item is already preceded by a block that
     * ended itself. */
    /* An item with no content at all has no first block, so it gets no
     * newline either: the original writes `<li></li>`, not `<li>\n</li>`. */
    bool para_first = true;
    if (bn > 0) {
      line_t fl;
      line_at(bp, bn, 0, &fl);
      const char *ft;
      size_t ftn;
      trim(fl.p, fl.n, &ft, &ftn);
      para_first = ftn == 0 ||
                   (columns(fl.p, fl.n) < 4u && !starts_block(ft, ftn));
    }
    lit(s, "<li>");
    if (tm == 0 && (loose || !para_first)) chr(s, '\n');
    if (tm > 0) {
      /* GFM task item: the checkbox goes INSIDE the <li>, which is where the
       * stylesheet's `li input[type="checkbox"]` rule expects to find it. */
      lit(s, "<input type=\"checkbox\"");
      if (checked) lit(s, " checked=\"\"");
      lit(s, " disabled=\"\" /> ");
      if (loose) chr(s, '\n');
      bp += tm;
      bn -= tm;
    }
    sink item;
    sink_init(&item);
    md_blocks(&item, bp, bn, depth + 1, !loose);
    if (item.ends_tight_para && item.out.len > 0 &&
        item.out.ptr[item.out.len - 1] == '\n') {
      item.out.len--;
      item.out.ptr[item.out.len] = '\0';
    }
    put(s, item.out.ptr, item.out.len);
    lit(s, "</li>\n");
    if (item.oom) s->oom = true;
    kbc_str_free(&item.out);
    if (body.oom) s->oom = true;
    kbc_str_free(&body.out);
  }
  lit(s, m0.ordered ? "</ol>\n" : "</ul>\n");
  return end;
}

/* Does this line contain `needle`? Used only on an HTML block's own lines,
 * to find the construct that closes it. */
static bool line_has(const char *p, size_t n, const char *needle) {
  size_t m = strlen(needle);
  if (m > n) return false;
  for (size_t i = 0; i + m <= n; i++) {
    if (memcmp(p + i, needle, m) == 0) return true;
  }
  return false;
}

/* The tag names CommonMark §4.6 lists as block-level for start condition 6,
 * and the four it lists as raw text for condition 1. Both lists are the
 * original's; a document using them is inside the declared subset. */
static bool html_name_is(const char *name, size_t n, const char *list) {
  size_t k = 0;
  for (size_t i = 0;; i++) {
    if (list[i] != ' ' && list[i] != '\0') continue;
    if (i - k == n && memcmp(name, list + k, n) == 0) return true;
    if (list[i] == '\0') return false;
    k = i + 1;
  }
}

#define MD_RAW_TEXT_TAGS "pre script style textarea"
#define MD_BLOCK_TAGS                                                          \
  "address article aside base basefont blockquote body caption center col "    \
  "colgroup dd details dialog dir div dl dt fieldset figcaption figure footer " \
  "form frame frameset h1 h2 h3 h4 h5 h6 head header hr html iframe legend li " \
  "link main menu menuitem nav noframes ol optgroup option p param search "    \
  "section source summary table tbody td tfoot th thead title tr track ul"

/* CommonMark §4.6's block start conditions, as far as this renderer goes.
 * 0 is "not a raw-HTML block": the line becomes a paragraph, which is what
 * keeps `a < b` prose rather than markup. 1 is a raw-text element
 * (`script`, `pre`, `style`, `textarea`) or a declaration or PI, whose block
 * runs to a closing construct and NOT to the next blank line, because a
 * blank line inside one is content. 2 is a comment, which runs to the line
 * carrying `-->`. 6 is a known block-level element. 7 is a complete tag
 * alone on its line.
 *
 * Every case but a comment and a declaration needs the tag to PARSE. An
 * unterminated tag is never a block: the block would copy every following
 * line verbatim — the page's own `</main></body></html>` included — into one
 * attribute value, and a line of prose beginning with `<div` would switch
 * the Markdown around it off. The inline path has always had this guard
 * (`raw_tag`'s own comment says a `<` that never closes must be escaped
 * instead); this is the same guard at block level. It is a DELIBERATE
 * divergence from the original, which does promote `<div class="a` with no
 * `>` to a block: kb-c refuses a construct it cannot bound, and the
 * divergence is in the safe direction.
 *
 * The line handed in is one LINE, so `raw_tag` cannot look past it and the
 * budget here is not about bounding this call — it is about the case the
 * line-oriented block layer does not exclude: a 16 MiB document with no
 * newline in it is a single line, and its `raw_tag` would walk the whole
 * thing. The budget is sized from the line so the sum over the document is
 * the same multiple-of-the-input the inline pass holds itself to. */
static int html_kind(const char *p, size_t n) {
  if (n < 2 || p[0] != '<') return 0;
  if (n >= 5 && memcmp(p + 1, "!--", 3) == 0) return 2;
  if (n >= 4 && p[1] == '!' && (p[2] == '[' || p[2] == 'D')) return 1;
  if (n >= 3 && p[1] == '?') return 1;
  size_t ns = p[1] == '/' ? 2u : 1u;
  if (ns >= n || !alpha((unsigned char)p[ns])) return 0;
  size_t ne = ns;
  while (ne < n && (alnum((unsigned char)p[ne]) || p[ne] == '-')) ne++;
  if (html_name_is(p + ns, ne - ns, MD_RAW_TEXT_TAGS)) return 1;
  size_t budget = n * MD_SCAN_BUDGET_PER_BYTE + MD_TAG_SCAN;
  size_t te = raw_tag(p, n, 0, &budget);
  if (te == 0) return 0;
  if (html_name_is(p + ns, ne - ns, MD_BLOCK_TAGS)) return 6;
  /* Type 7: a complete tag and nothing else on the line. A tag with text
   * beside it is a paragraph whose inline raw tags pass through, which is
   * both what the original does and what keeps a sentence mentioning
   * `<div>` from swallowing the rest of the document. */
  while (te < n && sp((unsigned char)p[te])) te++;
  return te == n ? 7 : 0;
}

/* Raw HTML at block level: the opening line and everything up to the
 * construct that closes it. Passed through untouched — the
 * `render.unsafe = true` contract, and the reason the kb-prompt template
 * survives to be scrubbed outbound. `kind` is `html_kind`'s classification
 * of the opening line and is what says where the block ends: a comment at
 * its `-->`, a raw-text element at its closing tag, everything else at the
 * next blank line. Ending a `<script>` or a `<pre>` at a blank line is how
 * a `<p>` used to land in the middle of a stylesheet. */
static size_t md_html(sink *s, const char *src, size_t n, size_t pos, int kind) {
  /* `line_has` takes a NUL-terminated needle, so `close_tag` must be one.
   * It was not: only `close_n` bytes were ever written and the terminator
   * was left to whatever was on the stack, so `strlen` read past the tag
   * name into garbage and a `<script>` block whose `</script>` sat on its own
   * line did not end there. It ran to the next blank line instead, which put
   * every following paragraph inside the script — a `<script>` in a document
   * swallowed the rest of the page as script source. */
  char close_tag[32];
  size_t close_n = 0;
  if (kind == 2) {
    memcpy(close_tag, "-->", 3);
    close_n = 3;
  } else if (kind == 1) {
    line_t open;
    line_at(src, n, pos, &open);
    const char *t;
    size_t tn;
    trim(open.p, open.n, &t, &tn);
    size_t ns = tn > 1 && t[1] == '/' ? 2u : 1u;
    size_t ne = ns;
    while (ne < tn && (alnum((unsigned char)t[ne]) || t[ne] == '-')) ne++;
    /* The needle is the WHOLE closing tag, `</script>` and not `</script`,
     * because that is what the original matches: a line carrying `</script`
     * with no `>` does not end the block. Verified against the reference in
     * both directions. */
    if (ne > ns && ne - ns + 3u < sizeof close_tag) {
      close_tag[0] = '<';
      close_tag[1] = '/';
      memcpy(close_tag + 2, t + ns, ne - ns);
      close_tag[ne - ns + 2u] = '>';
      close_n = ne - ns + 3u;
    }
  }
  close_tag[close_n] = '\0';

  size_t cur = pos;
  bool first = true;
  while (cur < n) {
    line_t l;
    line_at(src, n, cur, &l);
    if (!first && close_n == 0 && blank(&l)) break;
    /* The line CARRYING the closing tag is part of the block and is emitted
     * before the block ends. There used to be a second `break` on the same
     * test ahead of the `put`, which made the `closing` flag below dead and
     * dropped that line out of the block entirely: a `<script>` ending on
     * `foo </script> bar` lost the line and re-parsed it as a paragraph, so
     * the tail of a script became visible prose. */
    if (!first) chr(s, '\n');
    put(s, l.p, l.n);
    bool closing = close_n > 0 && line_has(l.p, l.n, close_tag);
    first = false;
    cur = l.next;
    if (closing) break;
  }
  if (!first) chr(s, '\n');
  return cur;
}

static void md_blocks(sink *s, const char *src, size_t n, int depth, bool tight) {
  size_t pos = 0;
  while (pos < n) {
    /* Only the block that ends the run may claim the enclosing `</li>`'s
     * newline; a tight paragraph earlier in the item does not. */
    s->ends_tight_para = false;
    line_t l;
    line_at(src, n, pos, &l);
    if (blank(&l)) {
      pos = l.next;
      continue;
    }
    size_t ind = columns(l.p, l.n);
    const char *t;
    size_t tn;
    trim(l.p, l.n, &t, &tn);

    char fc;
    size_t fl;
    if (fence_open(t, tn, &fc, &fl)) {
      /* The info string is the fence's first word; the rest is ignored,
       * because there is no highlighter to hand it to. */
      size_t k = 0;
      while (k < tn && t[k] == fc) k++;
      size_t e = tn;
      while (e > k && sp((unsigned char)t[e - 1])) e--;
      const char *info = t + k;
      size_t info_n = e - k;
      size_t w = 0;
      while (w < info_n && !sp((unsigned char)info[w])) w++;
      /* Body: up to the closing fence, or to the end of the document. */
      sink body;
      sink_init(&body);
      size_t cur = l.next;
      while (cur < n) {
        line_t c;
        line_at(src, n, cur, &c);
        if (fence_close(c.p, c.n, fc, fl)) {
          cur = c.next;
          break;
        }
        put(&body, c.p, c.n);
        chr(&body, '\n');
        cur = c.next;
      }
      md_code(s, body.out.ptr != NULL ? body.out.ptr : "", body.out.len, info,
              w);
      if (body.oom) s->oom = true;
      kbc_str_free(&body.out);
      pos = cur;
      continue;
    }

    if (ind >= 4) {
      /* Indented code: four spaces at the start of a line that is not a
       * paragraph continuation. */
      sink body;
      sink_init(&body);
      size_t cur = pos;
      bool any = false;
      while (cur < n) {
        line_t c;
        line_at(src, n, cur, &c);
        if (blank(&c)) {
          size_t j = c.next;
          while (j < n) {
            line_t b;
            line_at(src, n, j, &b);
            if (!blank(&b)) break;
            j = b.next;
          }
          if (j >= n) {
            cur = j;
            break;
          }
          line_t nx;
          line_at(src, n, j, &nx);
          if (columns(nx.p, nx.n) < 4) {
            cur = j;
            break;
          }
          chr(&body, '\n');
          cur = c.next;
          continue;
        }
        if (columns(c.p, c.n) < 4) break;
        const char *cp = c.p;
        size_t cn = c.n;
        strip_columns(&cp, &cn, 4);
        put(&body, cp, cn);
        chr(&body, '\n');
        any = true;
        cur = c.next;
      }
      if (any) md_code(s, body.out.ptr, body.out.len, "", 0);
      if (body.oom) s->oom = true;
      kbc_str_free(&body.out);
      pos = cur;
      continue;
    }

    if (t[0] == '>') {
      if (depth >= (int)MD_MAX_DEPTH) {
        pos = md_para(s, src, n, pos, depth, false);
        continue;
      }
      sink inner;
      sink_init(&inner);
      size_t cur = pos;
      while (cur < n) {
        line_t c;
        line_at(src, n, cur, &c);
        if (blank(&c)) break;
        /* The `>` is matched after the line's own indentation, not at
         * column 0: an indented quote is still a quote, and testing the raw
         * byte made `  > x` re-enter here forever with its indent intact. */
        const char *cp;
        size_t cn;
        trim(c.p, c.n, &cp, &cn);
        if (cn == 0 || cp[0] != '>') {
          /* Lazy continuation: an unmarked line directly under a quote line
           * stays in the quote, as CommonMark specifies. */
          if (starts_block(cp, cn)) break;
        } else {
          dequote(&cp, &cn);
        }
        if (inner.out.len > 0) chr(&inner, '\n');
        put(&inner, cp, cn);
        cur = c.next;
      }
      lit(s, "<blockquote>\n");
      md_blocks(s, inner.out.ptr != NULL ? inner.out.ptr : "", inner.out.len,
                depth + 1, false);
      lit(s, "</blockquote>\n");
      if (inner.oom) s->oom = true;
      kbc_str_free(&inner.out);
      pos = cur;
      continue;
    }

    int level = atx(t, tn);
    if (level > 0) {
      md_atx(s, t, tn, level, depth);
      pos = l.next;
      continue;
    }

    if (rule(t, tn)) {
      lit(s, "<hr />\n");
      pos = l.next;
      continue;
    }

    /* The raw-HTML block gate. `html_kind` is the whole rule, and every
     * clause of it is load-bearing:
     *
     *   - the line must OPEN with `<` and something tag-shaped, so a line
     *     of prose that merely mentions `<div>` is escaped rather than
     *     promoted to markup;
     *   - the tag must PARSE and close. Without that a line like
     *     `<a title="`, whose tag never ends, opened an HTML block that
     *     copied every following line verbatim — including the page's own
     *     `</main></body></html>` — into one attribute value, and turned a
     *     partial tag into an off switch for the Markdown around it. The
     *     inline path has always had this guard (`raw_tag`'s own comment
     *     says a `<` that never closes must be escaped instead); this is
     *     the same guard at block level;
     *   - a `<https://…>` or `<user@host>` autolink is not a tag — the `:`
     *     ends the tag name — so it falls through to a paragraph and the
     *     inline autolink runs, which is what makes the construct work at
     *     the start of a line as well as in the middle of one. */
    int hk = html_kind(t, tn);
    if (hk > 0) {
      pos = md_html(s, src, n, pos, hk);
      continue;
    }

    {
      size_t after = md_table(s, src, n, pos, depth);
      if (after != 0) {
        pos = after;
        continue;
      }
    }

    {
      marker m;
      if (list_marker(t, tn, &m)) {
        pos = md_list(s, src, n, pos, depth);
        continue;
      }
    }

    pos = md_para(s, src, n, pos, depth, tight);
  }
}

/* ----------------------------------------------------------------- api -- */

/* CommonMark §2.1: "the Unicode character U+0000 must be replaced with the
 * REPLACEMENT CHARACTER (U+FFFD)". It has to be done here, at the door,
 * because a NUL is valid UTF-8 — `kbc_utf8_validate` accepts it — and the
 * public contract is a bare `char *` (markdown.h:49). A page carrying one
 * stops at that byte for every `strlen` in the process, so a document
 * carrying a NUL lost its own text AND the page's `</p></main></body></html>`
 * after it.
 *
 * A document with no NUL — which is every document but the one the corpus
 * keeps for this — pays one `memchr` and copies nothing; the substitution
 * path is not on the hot path and the two halves of the output never have to
 * * know about it. `strip_controls` does the same job for the title, which
 * drops the NUL rather than replacing it. */
static kbc_status de_nul(const char *src, size_t len, char **owned,
                         const char **clean, size_t *clean_len) {
  *owned = NULL;
  *clean = src;
  *clean_len = len;
  if (memchr(src, 0, len) == NULL) return KBC_OK;
  char *copy = malloc(len * 3u + 1u);
  if (copy == NULL) return KBC_ERR_NOMEM;
  size_t w = 0;
  for (size_t r = 0; r < len; r++) {
    if (src[r] == '\0') {
      copy[w++] = (char)0xEFu;
      copy[w++] = (char)0xBFu;
      copy[w++] = (char)0xBDu;
    } else {
      copy[w++] = src[r];
    }
  }
  copy[w] = '\0';
  *owned = copy;
  *clean = copy;
  *clean_len = w; /* each NUL became three bytes, so the length grew */
  return KBC_OK;
}

/* The document's title, per the precedence the original applies at
 * markdown.rs:433-438. Exposed on its own because the artifact list shows
 * titles and must not render a whole document to learn one. */
char *kbc_markdown_title(const char *src, size_t len) {
  if (src == NULL) return NULL;
  if (len > KBC_MAX_ARTIFACT_BYTES) return NULL;
  size_t bad = 0;
  if (!kbc_utf8_validate(src, len, &bad)) return NULL;
  kbc_str t;
  kbc_str_init(&t);
  char *owned = NULL;
  const char *clean = src;
  size_t clean_len = len;
  if (kbc_failed(de_nul(src, len, &owned, &clean, &clean_len))) return NULL;
  fm_split fm;
  if (kbc_failed(title_of(clean, clean_len, &fm, &t))) {
    kbc_str_free(&t);
    free(owned);
    return NULL;
  }
  strip_controls(&t);
  free(owned);
  if (t.ptr == NULL) {
    kbc_str_free(&t);
    return NULL;
  }
  return t.ptr; /* KBC_OWN: the caller frees with kbc_str_free */
}

/* Hands `s`'s buffer to the caller when there is no arena, or copies it in
 * when there is. Either way `s` is left empty and the caller ends up with
 * exactly one owner. */
static char *finish(kbc_str *s, kbc_arena *a) {
  if (s->ptr == NULL) return NULL;
  if (a != NULL) {
    char *p = kbc_arena_strndup(a, s->ptr, s->len);
    kbc_str_free(s);
    return p;
  }
  char *p = s->ptr;
  s->ptr = NULL;
  s->len = 0;
  s->cap = 0;
  return p; /* KBC_OWN */
}



kbc_status kbc_markdown_render(const char *src, size_t len, kbc_arena *a,
                               kbc_markdown_page *out, kbc_err *err) {
  if (out == NULL) {
    return kbc_err_set(err, KBC_ERR_INVALID, "markdown: out is NULL");
  }
  out->html = NULL;
  out->title = NULL;
  if (src == NULL) {
    return kbc_err_set(err, KBC_ERR_INVALID, "markdown: src is NULL");
  }
  if (len > KBC_MAX_ARTIFACT_BYTES) {
    return kbc_err_set(err, KBC_ERR_INVALID,
                       "markdown: %zu bytes exceeds the %u byte limit", len,
                       (unsigned)KBC_MAX_ARTIFACT_BYTES);
  }
  size_t bad = 0;
  if (!kbc_utf8_validate(src, len, &bad)) {
    /* A page that serves half a document is worse than a 400: the caller
     * must never write one. */
    return kbc_err_set(err, KBC_ERR_PARSE,
                       "markdown: invalid UTF-8 at byte %zu", bad);
  }
  char *owned = NULL;
  size_t src_len = len;
  kbc_status nul = de_nul(src, len, &owned, &src, &src_len);
  if (kbc_failed(nul)) {
    return kbc_err_set(err, nul, "markdown: replacing NUL in %zu bytes", len);
  }

  kbc_str title;
  kbc_str_init(&title);
  fm_split fm;
  /* One parse: the frontmatter split gives the body offset the renderer
   * needs AND the title candidate, so there is no second pass over the same
   * bytes and no chance of the two disagreeing about where the body starts. */
  kbc_status st = title_of(src, src_len, &fm, &title);
  if (kbc_failed(st)) {
    kbc_str_free(&title);
    free(owned);
    return kbc_err_set(err, st, "markdown: reading the title");
  }
  strip_controls(&title);

  /* The callout pre-pass, then the block renderer over whatever it left. */
  sink pre;
  pre.oom = false;
  kbc_str_init(&pre.out);
  rewrite_callouts(&pre, fm.body, fm.body_len);

  sink body;
  body.oom = false;
  kbc_str_init(&body.out);
  md_blocks(&body, pre.out.ptr != NULL ? pre.out.ptr : "", pre.out.len, 0,
            false);
  kbc_str_free(&pre.out);

  sink page;
  page.oom = false;
  kbc_str_init(&page.out);
  /* The wrapper, byte for byte as markdown.rs:441-456 builds it, including
   * the three quirks a byte comparison catches: no newline after the
   * viewport meta, no self-closing slash on <meta charset>, and the
   * stylesheet wrapped in newlines of its own. */
  lit(&page, "<!doctype html>\n<html lang=\"en\"><head>\n");
  lit(&page, "<meta charset=\"utf-8\">");
  lit(&page, "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">");
  lit(&page, "\n<title>");
  esc_attr(&page, title.ptr != NULL ? title.ptr : "", title.len);
  lit(&page, "</title>\n<style>\n");
  put(&page, PROSE_CSS_A, sizeof PROSE_CSS_A - 1);
  put(&page, PROSE_CSS_B, sizeof PROSE_CSS_B - 1);
  put(&page, PROSE_CSS_C, sizeof PROSE_CSS_C - 1);
  lit(&page, "\n</style>\n</head>\n");
  lit(&page, "<body class=\"kb-md-prose\">");
  lit(&page, "<div class=\"kb-md-bar\" aria-hidden=\"true\">"
             "<span class=\"kb-md-bar__dot\"></span>markdown</div>");
  lit(&page, "<main class=\"kb-md-doc\">");
  if (body.out.ptr != NULL) put(&page, body.out.ptr, body.out.len);
  lit(&page, "</main></body></html>");

  kbc_str_free(&body.out);
  if (pre.oom || body.oom || page.oom) {
    kbc_str_free(&page.out);
    kbc_str_free(&title);
    free(owned);
    return kbc_err_set(err, KBC_ERR_NOMEM, "markdown: rendering %zu bytes",
                       len);
  }
  out->html = finish(&page.out, a);
  out->title = finish(&title, a);
  if (out->html == NULL || out->title == NULL) {
    free(out->html);
    free(out->title);
    out->html = NULL;
    out->title = NULL;
    free(owned);
    return kbc_err_set(err, KBC_ERR_NOMEM, "markdown: arena for the page");
  }
  free(owned);
  return KBC_OK;
}
