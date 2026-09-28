/* test_markdown.c — src/markdown.c: the page wrapper, title precedence, the
 * callout pre-pass, the supported grammar, and what an unsupported construct
 * renders as.
 *
 * Every case asserts something a READER of the page can see — a tag, an
 * attribute, the title — because that is the only surface this file has.
 * The Rust original's own assertions are ported with their input and their
 * expectation (markdown.rs:650-761), so a divergence in this port shows up as
 * a failure here rather than as a surprise in a browser. */
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "kbc/markdown.h"
#include "kbc/mem.h"
#include "kbc_test.h"

static bool has(const char *hay, const char *needle) {
  return hay != NULL && strstr(hay, needle) != NULL;
}

/* A cheap tag-balance check: the callout rewrite is the one place this
 * renderer splices markup it generated itself into a document, and an
 * unbalanced splice is exactly what a caller would see as a broken page. */
static int count_char(const char *s, char c) {
  int n = 0;
  for (; *s != '\0'; s++) {
    if (*s == c) n++;
  }
  return n;
}

static void render_str(const char *md, kbc_markdown_page *p) {
  kbc_err err;
  kbc_err_reset(&err);
  KBC_CHECK_OK(kbc_markdown_render(md, strlen(md), NULL, p, &err));
}

/* The page is a fixed shell around a variable body; this is where the body
 * starts, so a test can assert on the prose without repeating 7 KiB of
 * stylesheet. */
static const char *body_of(const kbc_markdown_page *p) {
  const char *m = strstr(p->html, "<main class=\"kb-md-doc\">");
  /* A missing marker means the page has no body at all. Falling back to a
   * whole-page search would let the 7 KiB stylesheet answer a question about
   * the prose — it names every `kb-callout--…` class the tests look for. */
  KBC_CHECK_NOT_NULL(m);
  if (m == NULL) return "";
  return m + strlen("<main class=\"kb-md-doc\">");
}

static void free_page(kbc_markdown_page *p) {
  free(p->html);
  free(p->title);
  p->html = NULL;
  p->title = NULL;
}

/* ---------------------------------------------------------- the shell --- */

/* Ported from markdown.rs:744-755. The original asserts the doctype, the
 * title element, the accent bar and the document container; the frontmatter
 * is consumed rather than rendered, which is the assertion that catches a
 * parser which forgot to strip it. */
KBC_TEST(page_is_a_self_contained_document) {
  kbc_markdown_page p;
  render_str("---\ntitle: Hello\n---\n# Hello\n\nworld\n", &p);
  KBC_CHECK_MSG(strncmp(p.html, "<!doctype html>\n<html lang=\"en\"><head>\n",
                        strlen("<!doctype html>\n<html lang=\"en\"><head>\n")) == 0,
                "page must open with the original's shell: %s", p.html);
  KBC_CHECK(has(p.html, "<title>Hello</title>"));
  KBC_CHECK(has(p.html, "kb-md-bar"));
  KBC_CHECK(has(p.html, "class=\"kb-md-doc\""));
  KBC_CHECK(has(body_of(&p), "<h1>Hello</h1>"));
  KBC_CHECK(!has(p.html, "title: Hello"));
  /* The three wrapper quirks a byte comparison would catch. */
  KBC_CHECK(has(p.html, "<meta charset=\"utf-8\">"));
  KBC_CHECK(has(p.html, "initial-scale=1\">\n<title>"));
  KBC_CHECK(has(p.html, "</style>\n</head>\n"));
  KBC_CHECK(has(p.html, "<style>\n/* kb markdown reading theme"));
  KBC_CHECK(has(p.html, "</main></body></html>"));
  KBC_CHECK_EQ_STR(p.title, "Hello");
  free_page(&p);
}

/* The stylesheet is the shipped asset, inlined. A page missing the reading
 * theme's own first rule is a page that fell off the wrapper. */
KBC_TEST(stylesheet_is_present) {
  kbc_markdown_page p;
  render_str("x\n", &p);
  KBC_CHECK(has(p.html, "--md-accent: #6d5ae6;"));
  KBC_CHECK(has(p.html, ".kb-md-doc .kb-callout--warning"));
  KBC_CHECK(has(p.html, "@media (max-width: 640px)"));
  free_page(&p);
}

/* --------------------------------------------------------- precedence --- */

KBC_TEST(frontmatter_title_beats_the_first_h1) {
  kbc_markdown_page p;
  render_str("---\ntitle: My Spec\nkb-tags: rust, async\n---\n# Body\n\ntext\n", &p);
  KBC_CHECK(has(p.html, "<title>My Spec</title>"));
  KBC_CHECK(!has(p.html, "<title>Body</title>"));
  KBC_CHECK(has(body_of(&p), "<h1>Body</h1>"));
  KBC_CHECK(!has(p.html, "kb-tags:"));
  free_page(&p);

  static const char md[] = "---\ntitle: My Spec\n---\n# Body\n";
  char *t = kbc_markdown_title(md, sizeof md - 1u);
  KBC_CHECK_EQ_STR(t, "My Spec");
  free(t);
}

/* Ported from markdown.rs:758-761. */
KBC_TEST(title_falls_back_to_h1_then_to_untitled) {
  kbc_markdown_page p;
  render_str("# From H1\n\nx", &p);
  KBC_CHECK(has(p.html, "<title>From H1</title>"));
  free_page(&p);

  render_str("just text, no heading", &p);
  KBC_CHECK(has(p.html, "<title>Untitled</title>"));
  KBC_CHECK_EQ_STR(p.title, "Untitled");
  free_page(&p);
}

/* A `# ` line inside a fence is sample text. The original's first_h1 is a
 * naive line scan and would name the page after a shell comment; kb-c skips
 * fenced regions so the title and the body cannot disagree. */
KBC_TEST(heading_inside_a_fence_is_not_the_title) {
  static const char md[] = "```sh\n# install the thing\n```\n\nafter\n";
  kbc_markdown_page p;
  render_str(md, &p);
  KBC_CHECK(has(p.html, "<title>Untitled</title>"));
  KBC_CHECK(has(body_of(&p), "# install the thing"));
  free_page(&p);
}

/* The title lands in an attribute-bearing element, so it is escaped there
 * exactly as it is nowhere else in the head. */
KBC_TEST(title_is_escaped_in_the_head) {
  static const char md[] = "---\ntitle: \"<b>a & b</b>\"\n---\n# H\n";
  kbc_markdown_page p;
  render_str(md, &p);
  KBC_CHECK(has(p.html, "<title>&lt;b&gt;a &amp; b&lt;/b&gt;</title>"));
  KBC_CHECK(!has(p.html, "<title><b>"));
  free_page(&p);
}


/* ------------------------------------------------------- frontmatter ---- */

/* markdown.rs:516-525: a leading BOM must not defeat frontmatter detection,
 * and a stray trailing space on the closing fence still closes the block. */
KBC_TEST(bom_does_not_eat_the_document) {
  static const char md[] = "\xef\xbb\xbf---\ntitle: T\n---\n# H\n\nbody text\n";
  kbc_markdown_page p;
  render_str(md, &p);
  KBC_CHECK(has(p.html, "<title>T</title>"));
  KBC_CHECK(has(body_of(&p), "<h1>H</h1>"));
  KBC_CHECK(has(body_of(&p), "<p>body text</p>"));
  KBC_CHECK(!has(p.html, "---"));
  free_page(&p);

  /* A BOM with no frontmatter: the document is still the document. */
  static const char plain[] = "\xef\xbb\xbf# Still Here\n\nbody\n";
  render_str(plain, &p);
  KBC_CHECK(has(p.html, "<title>Still Here</title>"));
  KBC_CHECK(has(body_of(&p), "<h1>Still Here</h1>"));
  free_page(&p);
}

/* markdown.rs:527-533: a leading `---` with no closing fence is a thematic
 * break, not metadata. Guessing would eat the rest of the document. */
KBC_TEST(leading_thematic_break_is_not_frontmatter) {
  static const char md[] = "---\njust a thematic break then text\n";
  kbc_markdown_page p;
  render_str(md, &p);
  KBC_CHECK(has(body_of(&p), "<hr />"));
  KBC_CHECK(has(body_of(&p), "<p>just a thematic break then text</p>"));
  KBC_CHECK_EQ_STR(p.title, "Untitled");
  free_page(&p);

  char *t = kbc_markdown_title(md, strlen(md));
  KBC_CHECK_EQ_STR(t, "Untitled");
  free(t);
}

/* The closing fence is trimmed before it is compared, so the invisible
 * trailing space an editor leaves still closes the block. */
KBC_TEST(frontmatter_closes_on_a_padded_fence) {
  static const char md[] = "---\ntitle: T2\n--- \n# H2\n\ntext\n";
  kbc_markdown_page p;
  render_str(md, &p);
  KBC_CHECK(has(p.html, "<title>T2</title>"));
  KBC_CHECK(has(body_of(&p), "<h1>H2</h1>"));
  KBC_CHECK(has(body_of(&p), "<p>text</p>"));
  free_page(&p);
}

/* A CRLF document is a document. */
KBC_TEST(crlf_document_renders_like_an_lf_one) {
  static const char md[] = "---\r\ntitle: T\r\n---\r\n# H\r\n\r\n- a\r\n- b\r\n";
  kbc_markdown_page p;
  render_str(md, &p);
  KBC_CHECK(has(p.html, "<title>T</title>"));
  KBC_CHECK(has(body_of(&p), "<h1>H</h1>"));
  KBC_CHECK(has(body_of(&p), "<li>a</li>"));
  KBC_CHECK(has(body_of(&p), "<li>b</li>"));
  KBC_CHECK(!has(p.html, "\r"));
  free_page(&p);
}

/* ------------------------------------------------------------ callouts -- */
/* Ported from markdown.rs:669-688, including the assertion that the marker
 * is gone and the de-quoted body is still Markdown. */
KBC_TEST(callout_renders_the_div) {
  static const char md[] = "> [!warning] Heads up\n> be careful **here**\n";
  kbc_markdown_page p;
  render_str(md, &p);
  const char *body = body_of(&p);
  KBC_CHECK_MSG(has(body, "class=\"kb-callout kb-callout--warning\""),
                "callout div + type class: %s", body);
  KBC_CHECK(has(body, "class=\"kb-callout__title\""));
  KBC_CHECK(has(body, "Heads up"));
  KBC_CHECK(has(body, "<strong>here</strong>"));
  KBC_CHECK_MSG(!has(body, "[!warning]"), "marker stripped: %s", body);
  KBC_CHECK_EQ_INT(count_char(body, '<'), count_char(body, '>'));
  free_page(&p);
}

/* Ported from markdown.rs:690-698 and :700-706. */
KBC_TEST(callout_default_title_and_type_casing) {
  kbc_markdown_page p;
  render_str("> [!tip]\n> do this\n", &p);
  KBC_CHECK(has(body_of(&p), "kb-callout--tip"));
  KBC_CHECK_MSG(has(body_of(&p), ">Tip<"),
                "default title = capitalised type: %s", p.html);
  free_page(&p);

  render_str("> [!CAUTION] x\n> y\n", &p);
  KBC_CHECK(has(body_of(&p), "kb-callout--caution"));
  KBC_CHECK(!has(body_of(&p), "kb-callout--CAUTION"));
  free_page(&p);
}

/* Ported from markdown.rs:723-730. The title is document text and lands in
 * the page, so it is escaped; a callout title is the one place a rewrite
 * turns user text into markup. */
KBC_TEST(callout_title_is_escaped) {
  static const char md[] = "> [!note] <b>x</b> & y\n> body\n";
  kbc_markdown_page p;
  render_str(md, &p);
  KBC_CHECK(has(body_of(&p), "&lt;b&gt;x&lt;/b&gt; &amp; y"));
  KBC_CHECK(!has(body_of(&p), "<b>x</b>"));
  free_page(&p);
}

/* Ported from markdown.rs:708-714. */
KBC_TEST(plain_blockquote_is_untouched) {
  static const char md[] = "> just an ordinary quote\n";
  kbc_markdown_page p;
  render_str(md, &p);
  KBC_CHECK(has(body_of(&p), "<blockquote>"));
  KBC_CHECK_MSG(!has(body_of(&p), "kb-callout"),
                "no callout wrapping: %s", body_of(&p));
  free_page(&p);
}

/* The pre-pass's fast path, made observable. Every one of these lines LOOKS
 * like a callout and none of them is one, so the whole document must come
 * through with its bytes intact and no callout div anywhere: a pre-pass that
 * rewrote a document it had no business touching would show up here. */
KBC_TEST(document_without_a_callout_is_not_rewritten) {
  static const char md[] = "# Title\n\n"
                           "A paragraph with [!note] inline.\n\n"
                           "> a plain quote\n\n"
                           "  > [!note] indented, so not a header\n\n"
                           "```\n> [!warning] sample text\n```\n";
  kbc_markdown_page p;
  render_str(md, &p);
  const char *body = body_of(&p);
  /* The body, not the page: the stylesheet itself names every kb-callout
   * class, so a whole-page search would pass or fail on the CSS. */
  KBC_CHECK_MSG(!has(body, "kb-callout"), "no callout anywhere: %s", body);
  KBC_CHECK(has(body, "[!note] inline"));
  /* Indented, so `callout_header` (which needs `>` at byte 0) declines it and
   * it stays an ordinary blockquote whose text reads literally. */
  KBC_CHECK(has(body, "<p>[!note] indented, so not a header</p>"));
  KBC_CHECK(has(body, "<blockquote>"));
  KBC_CHECK(has(body, "&gt; [!warning] sample text"));
  KBC_CHECK(has(body, "<pre><code>&gt; [!warning] sample text"));
  free_page(&p);
}

/* A callout's body re-parses as Markdown, fenced code included. The original
 * gets this from comrak; the blank-line rule in the rewrite is what makes it
 * work, so the test is that the code block is a real <pre><code> and not
 * pre-formatted text. */
KBC_TEST(fenced_code_inside_a_callout_still_renders_as_code) {
  static const char md[] = "> [!warning] Heads up\n"
                           "> ```rust\n"
                           "> fn main() {}\n"
                           "> ```\n"
                           "\nafter the callout\n";
  kbc_markdown_page p;
  render_str(md, &p);
  const char *body = body_of(&p);
  KBC_CHECK(has(body, "<pre><code class=\"language-rust\">fn main() {}\n"));
  KBC_CHECK(has(body, "</code></pre>"));
  KBC_CHECK_MSG(!has(body, "<div class=\"kb-callout kb-callout--warning\">\n"
                           "<div class=\"kb-callout__title\">Heads up</div>\n"
                           "<div"),
                "the rewritten div must not swallow the body: %s", body);
  KBC_CHECK(has(body, "<p>after the callout</p>"));
  free_page(&p);
}

/* ------------------------------------------------------------ grammar --- */

/* Ported from markdown.rs:649-655, minus the syntect assertion: kb-c links
 * no highlighter, so a code block carries the container and no token spans. */
KBC_TEST(table_and_task_list) {
  static const char md[] = "| a | b |\n|---|---|\n| 1 | 2 |\n\n"
                           "- [x] done\n- [ ] todo\n";
  kbc_markdown_page p;
  render_str(md, &p);
  const char *body = body_of(&p);
  KBC_CHECK_MSG(has(body, "<table>"), "GFM table: %s", body);
  KBC_CHECK(has(body, "<th>a</th>"));
  KBC_CHECK(has(body, "<td>2</td>"));
  KBC_CHECK(has(body, "type=\"checkbox\""));
  KBC_CHECK(has(body, "checked=\"\""));
  KBC_CHECK(!has(body, "|---|"));
  free_page(&p);
}

/* A code block's content is escaped. This is the one place raw passthrough
 * must NOT apply, and getting it wrong is how a corpus file injects a
 * <script> into a page it merely quoted. */
KBC_TEST(code_content_is_escaped_not_passed_through) {
  static const char md[] = "```\n<script>alert(1)</script>\n```\n";
  kbc_markdown_page p;
  render_str(md, &p);
  const char *body = body_of(&p);
  KBC_CHECK(has(body, "<pre><code>"));
  KBC_CHECK(has(body, "&lt;script&gt;alert(1)&lt;/script&gt;"));
  KBC_CHECK_MSG(!has(body, "<script>"), "no live script: %s", body);
  free_page(&p);
}

/* Ported from markdown.rs:732-742: the inline kb-prompt template must survive
 * the render so the outbound scrub can strip it later. */
KBC_TEST(raw_html_passes_through) {
  static const char md[] =
      "# Doc\n\n<template id=\"kb-prompt\">secret prompt</template>\n";
  kbc_markdown_page p;
  render_str(md, &p);
  KBC_CHECK_MSG(has(p.html, "<template id=\"kb-prompt\">"),
                "template preserved: %s", p.html);
  KBC_CHECK(has(body_of(&p), "</template>"));
  free_page(&p);
}

KBC_TEST(inline_grammar) {
  static const char md[] = "Text with *em*, **strong**, ~~del~~, `code`, "
                           "[t](u \"ti\"), ![a](i.png), <https://x.y> and "
                           "https://z.example/q.\n";
  kbc_markdown_page p;
  render_str(md, &p);
  const char *body = body_of(&p);
  KBC_CHECK(has(body, "<em>em</em>"));
  KBC_CHECK(has(body, "<strong>strong</strong>"));
  KBC_CHECK(has(body, "<del>del</del>"));
  KBC_CHECK(has(body, "<code>code</code>"));
  KBC_CHECK(has(body, "<a href=\"u\" title=\"ti\">t</a>"));
  KBC_CHECK(has(body, "<img src=\"i.png\" alt=\"a\" />"));
  KBC_CHECK(has(body, "<a href=\"https://x.y\">https://x.y</a>"));
  KBC_CHECK(has(body, "<a href=\"https://z.example/q\">https://z.example/q</a>"));
  free_page(&p);
}

/* A quote inside an attribute is the difference between a link and an
 * injection, and the source is a corpus file. */
KBC_TEST(link_destination_cannot_break_out_of_the_attribute) {
  static const char md[] = "[t](x\"y) and [u](a&amp;b)\n";
  kbc_markdown_page p;
  render_str(md, &p);
  const char *body = body_of(&p);
  KBC_CHECK_MSG(has(body, "href=\"x%22y\""),
                "a quote in a destination is percent-encoded: %s", body);
  KBC_CHECK_MSG(!has(body, "alt=\"a\" onerror"), "unrelated: %s", body);
  KBC_CHECK(has(body, "href=\"a&amp;b\""));
  free_page(&p);
}

KBC_TEST(nested_lists_and_setext_headings) {
  static const char md[] = "Title\n=====\n\nSub\n----\n\n"
                           "- a\n  - b\n- c\n\n1. one\n2. two\n";
  kbc_markdown_page p;
  render_str(md, &p);
  const char *body = body_of(&p);
  KBC_CHECK(has(body, "<h1>Title</h1>"));
  KBC_CHECK(has(body, "<h2>Sub</h2>"));
  KBC_CHECK(has(body, "<ul>\n<li>a\n<ul>\n<li>b</li>\n</ul>\n</li>\n<li>c</li>\n</ul>"));
  KBC_CHECK(has(body, "<ol>\n<li>one</li>\n<li>two</li>\n</ol>"));
  free_page(&p);
}

/* ------------------------------------------------------- not supported -- */

/* The header says footnotes and description lists render as ordinary text
 * rather than as their construct. This is that promise, written down: the
 * marker survives as the characters the author typed, the definition line
 * stays a paragraph, and nothing in the page claims to be a footnote. */
KBC_TEST(unsupported_constructs_render_as_text) {
  static const char md[] = "Text[^1] more.\n\n[^1]: the note\n\n"
                           ": term\n: definition\n\nx^2^\n";
  kbc_markdown_page p;
  render_str(md, &p);
  const char *body = body_of(&p);
  KBC_CHECK(has(body, "<p>Text[^1] more.</p>"));
  KBC_CHECK(has(body, "<p>[^1]: the note</p>"));
  KBC_CHECK(has(body, ": term\n: definition"));
  KBC_CHECK(has(body, "<p>x^2^</p>"));
  KBC_CHECK(!has(body, "footnote"));
  KBC_CHECK(!has(body, "<sup>"));
  free_page(&p);
}

/* A reference link has no definition table in this renderer, so `[a][b]`
 * reads as the text it is rather than as a link to nowhere. */
KBC_TEST(reference_link_renders_as_text) {
  static const char md[] = "[text][ref]\n\n[ref]: ./elsewhere.md\n";
  kbc_markdown_page p;
  render_str(md, &p);
  const char *body = body_of(&p);
  KBC_CHECK(!has(body, "<a href=\"./elsewhere.md\""));
  KBC_CHECK(has(body, "<p>[text][ref]</p>"));
  KBC_CHECK(has(body, "[ref]: ./elsewhere.md"));
  free_page(&p);
}

/* ------------------------------------------------------------- limits --- */

/* An artifact over the ceiling is REFUSED, not truncated: a page that stops
 * mid-document is worse than an error, because the caller would serve it. */
KBC_TEST(oversized_document_is_refused_not_truncated) {
  size_t n = (size_t)KBC_MAX_ARTIFACT_BYTES + 1u;
  char *big = malloc(n + 1u);
  KBC_CHECK_NOT_NULL(big);
  if (big == NULL) return;
  memset(big, 'a', n);
  big[n] = '\0';
  kbc_err err;
  kbc_err_reset(&err);
  kbc_markdown_page p;
  KBC_CHECK_ERR(kbc_markdown_render(big, n, NULL, &p, &err), KBC_ERR_INVALID);
  KBC_CHECK_ERR_MSG(err);
  KBC_CHECK_NULL(p.html);
  KBC_CHECK_NULL(p.title);
  KBC_CHECK_NULL(kbc_markdown_title(big, n));
  free(big);
}

KBC_TEST(invalid_utf8_is_refused) {
  static const char md[] = "# H\n\nbad byte: \xff\xfe end\n";
  kbc_err err;
  kbc_err_reset(&err);
  kbc_markdown_page p;
  KBC_CHECK_ERR(kbc_markdown_render(md, sizeof md - 1u, NULL, &p, &err),
                KBC_ERR_PARSE);
  KBC_CHECK_ERR_MSG(err);
  KBC_CHECK_NULL(p.html);
  KBC_CHECK_NULL(kbc_markdown_title(md, sizeof md - 1u));
}

KBC_TEST(null_arguments_are_rejected) {
  kbc_err err;
  kbc_err_reset(&err);
  kbc_markdown_page p;
  KBC_CHECK_ERR(kbc_markdown_render(NULL, 0, NULL, &p, &err), KBC_ERR_INVALID);
  KBC_CHECK_ERR_MSG(err);
  KBC_CHECK_ERR(kbc_markdown_render("x", 1, NULL, NULL, &err),
                KBC_ERR_INVALID);
  KBC_CHECK_NULL(kbc_markdown_title(NULL, 0));
}

/* An empty document is a document; it renders an empty shell, not a crash
 * and not an error. */
KBC_TEST(empty_document_renders_an_empty_shell) {
  kbc_err err;
  kbc_err_reset(&err);
  kbc_markdown_page p;
  KBC_CHECK_OK(kbc_markdown_render("", 0, NULL, &p, &err));
  KBC_CHECK(has(p.html, "<title>Untitled</title>"));
  KBC_CHECK(has(p.html, "<main class=\"kb-md-doc\"></main>"));
  free_page(&p);
}

/* ------------------------------------------------- escaping and safety -- */

/* A code span is the one context a reader trusts as INERT: the author wrote
 * backticks precisely to say "this is a log line, not markup". The content
 * therefore goes out escaped, exactly as the block-code path above escapes
 * it. Emitting it verbatim put a live `<img onerror=…>` on the page for any
 * document quoting an untrusted transcript. */
KBC_TEST(code_span_content_is_escaped) {
  static const char md[] = "x `<script>alert(1)</script>` y\n";
  kbc_markdown_page p;
  render_str(md, &p);
  const char *body = body_of(&p);
  KBC_CHECK_MSG(has(body, "<code>&lt;script&gt;alert(1)&lt;/script&gt;</code>"),
                "code span not escaped: %s", body);
  KBC_CHECK_MSG(!has(body, "<script>"), "live script from a code span: %s",
                body);
  free_page(&p);

  static const char attrs[] = "`<a href=\"x\">` and `a & b`\n";
  render_str(attrs, &p);
  body = body_of(&p);
  KBC_CHECK(has(body, "<code>&lt;a href=\"x\"&gt;</code>"));
  KBC_CHECK(has(body, "<code>a &amp; b</code>"));
  KBC_CHECK(!has(body, "<a href=\"x\">"));
  free_page(&p);
}

/* A backslash escape yields the CHARACTER it escapes, as text. `\<script>` is
 * a literal `<script>`; emitting the character raw turned the escape into the
 * very markup it was written to suppress. */
KBC_TEST(backslash_escape_yields_a_literal_character) {
  static const char md[] = "\\<script>alert(1)\\</script>\n\n"
                           "a \\< b and \\*not em\\*\n";
  kbc_markdown_page p;
  render_str(md, &p);
  const char *body = body_of(&p);
  KBC_CHECK_MSG(has(body, "<p>&lt;script&gt;alert(1)&lt;/script&gt;</p>"),
                "escaped angle brackets became markup: %s", body);
  KBC_CHECK(!has(body, "<script"));
  KBC_CHECK(has(body, "<p>a &lt; b and *not em*</p>"));
  free_page(&p);
}

/* A tag that never closes must not open an HTML block. When it did, the
 * block copied every following line verbatim — the page's own
 * `</main></body></html>` included — into one attribute value, and a
 * fragment of prose beginning with `<x` switched the Markdown around it off
 * for the rest of the document. */
KBC_TEST(unterminated_tag_does_not_swallow_the_document) {
  static const char md[] = "<a title=\"\nVISIBLE CONTENT\n";
  kbc_markdown_page p;
  render_str(md, &p);
  const char *body = body_of(&p);
  KBC_CHECK_MSG(has(body, "&lt;a title=\""), "escaped as text: %s", body);
  KBC_CHECK_MSG(has(body, "VISIBLE CONTENT"), "content intact: %s", body);
  KBC_CHECK_MSG(!has(body, "<a title="), "an unclosed tag became markup: %s",
                body);
 free_page(&p);
}

/* A partial tag at the start of a line is prose, not a block opener. */
KBC_TEST(a_partial_tag_keeps_the_paragraph_in_markdown) {
  static const char md[] = "<x\n**bold** and [l](u)\n";
  kbc_markdown_page p;
  render_str(md, &p);
  const char *body = body_of(&p);
  KBC_CHECK_MSG(has(body, "<strong>bold</strong>"),
                "a partial tag disabled Markdown: %s", body);
  KBC_CHECK(has(body, "<a href=\"u\">l</a>"));
  free_page(&p);
}

/* CommonMark §2.1: U+0000 becomes U+FFFD. A NUL is valid UTF-8, so the
 * document is not refused — and the page's public contract is a bare
 * `char *`, so a NUL left in the output truncates the page at that byte for
 * every `strlen` in the process, dropping the document text AND the page's
 * own closing tags. */
KBC_TEST(nul_byte_becomes_the_replacement_character) {
  static const char md[] = "before\0after\n";
  kbc_markdown_page p;
  /* The length is explicit: `strlen` would stop at the NUL the test is about. */
  kbc_err err;
  kbc_err_reset(&err);
  KBC_CHECK_OK(kbc_markdown_render(md, sizeof md - 1u, NULL, &p, &err));
  KBC_CHECK_MSG(strstr(p.html, "<p>before\xef\xbf\xbd" "after</p>") != NULL,
                "NUL not replaced; the page stops mid-document");
  KBC_CHECK_MSG(has(p.html, "</main></body></html>"),
                "the page's own closing tags were eaten: %zu bytes", strlen(p.html));
  free_page(&p);
}

/* --------------------------------------------------- blocks and lists --- */

/* The callout pre-pass injects a `<div>`. Injected without a blank line in
 * front of it, the block parser reads that div as a lazy paragraph
 * continuation, never closes the `<p>` above, and lands the callout's own
 * markup inside it — a `MISMATCH </p>` on the page, and inside a blockquote
 * a callout that escapes the quote holding it. */
KBC_TEST(callout_after_a_paragraph_is_well_formed) {
  static const char md[] = "para text\n\n> [!note] T\n> body\n";
  kbc_markdown_page p;
  render_str(md, &p);
  const char *body = body_of(&p);
  KBC_CHECK_MSG(has(body, "<p>para text</p>\n<div class=\"kb-callout "
                          "kb-callout--note\">"),
                "the paragraph must close before the callout: %s", body);
  KBC_CHECK_MSG(!has(body, "T</div></p>"),
                "callout markup landed inside the paragraph: %s", body);
  KBC_CHECK_MSG(!has(body, "</blockquote>\n<p>body</p>\n</div>"),
                "the callout body escaped its quote: %s", body);
  KBC_CHECK_EQ_INT(count_char(body, '<'), count_char(body, '>'));
  free_page(&p);

  static const char quoted[] = "> quoted\n> more\n\n> [!note] T\n> body\n";
  render_str(quoted, &p);
  body = body_of(&p);
  KBC_CHECK_MSG(has(body, "</blockquote>\n<div class=\"kb-callout"),
                "the callout escaped its blockquote: %s", body);
  free_page(&p);
}

/* A `<https://…>` alone on a line is an autolink. It is not an HTML tag —
 * the `:` ends the tag name — and routing it to the raw-HTML block put a
 * bare `<http: …>` element on the page. Mid-paragraph it always worked;
 * working in one position and not the other is the defect. */
KBC_TEST(autolink_at_the_start_of_a_line) {
  static const char md[] = "<https://a.b/?x=1&y=2>\n";
  kbc_markdown_page p;
  render_str(md, &p);
  const char *body = body_of(&p);
  KBC_CHECK_MSG(has(body, "<p><a href=\"https://a.b/?x=1&amp;y=2\">"
                          "https://a.b/?x=1&amp;y=2</a></p>"),
                "line-start autolink not recognised: %s", body);
  free_page(&p);
}

/* CommonMark §6.9: an email autolink's href carries `mailto:`. Without it
 * the href is a relative URL and the link 404s. */
KBC_TEST(email_autolink_carries_mailto) {
  static const char md[] = "see <foo@bar.example.com> now\n";
  kbc_markdown_page p;
  render_str(md, &p);
  const char *body = body_of(&p);
  KBC_CHECK_MSG(has(body, "href=\"mailto:foo@bar.example.com\""),
                "the email autolink has no scheme: %s", body);
  KBC_CHECK(!has(body, "href=\"foo@bar"));
  free_page(&p);
}

/* A list is loose or it is not, for the WHOLE list: "if a list is loose, all
 * its constituent list items are loose" (§5.2). Discovering looseness while
 * emitting meant every item before the first blank line came out tight, so
 * the first bullet of an ordinary loose list lost its paragraph margin. */
KBC_TEST(loose_list_wraps_every_item) {
  static const char md[] = "- first\n\n- second\n\n- third\n";
  kbc_markdown_page p;
  render_str(md, &p);
  const char *body = body_of(&p);
  KBC_CHECK_MSG(has(body, "<li>\n<p>first</p>"),
                "the first item of a loose list rendered tight: %s", body);
  KBC_CHECK(has(body, "<li>\n<p>second</p>"));
  KBC_CHECK(has(body, "<li>\n<p>third</p>"));
  free_page(&p);

  /* A tight list stays tight: the first item's tightness is the whole
   * difference, so a renderer that simply always went loose would pass the
   * assertion above. */
  static const char tight[] = "- a\n- b\n";
  render_str(tight, &p);
  KBC_CHECK(has(body_of(&p), "<li>a</li>"));
  free_page(&p);
}

/* "a list item can contain any number of additional blocks" (§5). Ending an
 * item's body at the first blank line pushed an indented quote, a nested
 * list or an indented code block out of the item and into the document as a
 * sibling of the whole list. */
KBC_TEST(a_list_item_may_contain_several_blocks) {
  static const char quoted[] = "- item\n\n  > quoted\n";
  kbc_markdown_page p;
  render_str(quoted, &p);
  const char *body = body_of(&p);
  KBC_CHECK_MSG(has(body, "<blockquote>"), "the quote was lost: %s", body);
  KBC_CHECK_MSG(strstr(body, "</ul>") > strstr(body, "<blockquote>"),
                "the quote escaped the list: %s", body);
  free_page(&p);

  static const char nested[] = "- a\n\n  - b\n";
  render_str(nested, &p);
  body = body_of(&p);
  KBC_CHECK_MSG(has(body, "<li>\n<p>a</p>\n<ul>\n<li>b</li>\n</ul>\n</li>"),
                "the nested list became a sibling: %s", body);
  free_page(&p);
}

/* CommonMark §4.3: a setext underline closes a paragraph of ANY length. The
 * "exactly one line" rule that was here made `Foo\nBar\n---` a paragraph
 * plus a thematic break. */
KBC_TEST(setext_underline_closes_a_paragraph_of_any_length) {
  static const char md[] = "Foo\nBar\n---\n\nBaz\nQux\n===\n";
  kbc_markdown_page p;
  render_str(md, &p);
  const char *body = body_of(&p);
  KBC_CHECK_MSG(has(body, "<h2>Foo\nBar</h2>"), "h2 over two lines: %s", body);
  KBC_CHECK(has(body, "<h1>Baz\nQux</h1>"));
  KBC_CHECK(!has(body, "<hr />"));
  free_page(&p);
}

/* ------------------------------------------------- constructs claimed --- */

/* Indented code is a supported construct and the header says so. */
KBC_TEST(indented_code_is_a_code_block) {
  static const char md[] = "para\n\n    a < b\n    x\n\nafter\n";
  kbc_markdown_page p;
  render_str(md, &p);
  const char *body = body_of(&p);
  KBC_CHECK_MSG(has(body, "<pre><code>a &lt; b\nx\n</code></pre>"),
                "indented code: %s", body);
  KBC_CHECK(has(body, "<p>after</p>"));
  free_page(&p);
}

/* A `<`, an `&` and a `"` in ordinary text are escaped in EVERY block, which
 * is the shape a hostile corpus file actually takes. */
KBC_TEST(plain_text_is_escaped_in_every_block) {
  static const char md[] = "a < b & c in a paragraph\n\n"
                           "# heading < & >\n\n"
                           "- item < & >\n\n"
                           "> quote < & >\n\n"
                           "| h < & |\n|---|\n| c < & |\n\n"
                           "[t](u \"ti < & >\")\n";
  kbc_markdown_page p;
  render_str(md, &p);
  const char *body = body_of(&p);
  KBC_CHECK(has(body, "a &lt; b &amp; c in a paragraph"));
  KBC_CHECK(has(body, "<h1>heading &lt; &amp; &gt;</h1>"));
  KBC_CHECK(has(body, "<li>item &lt; &amp; &gt;"));
  KBC_CHECK(has(body, "<p>quote &lt; &amp; &gt;</p>"));
  KBC_CHECK(has(body, "<th>h &lt; &amp;</th>"));
  KBC_CHECK(has(body, "<td>c &lt; &amp;</td>"));
  KBC_CHECK(has(body, "title=\"ti &lt; &amp; &gt;\""));
  free_page(&p);
}

/* An image's alt and a link's title are attribute values; an unescaped quote
 * in either ends the attribute and hands the rest to the parser. */
KBC_TEST(image_alt_and_link_title_are_escaped) {
  static const char md[] = "![a\" onerror=\"x](i.png)\n\n[t](u \"q\\\"uote\")\n";
  kbc_markdown_page p;
  render_str(md, &p);
  const char *body = body_of(&p);
  KBC_CHECK_MSG(!has(body, "alt=\"a\" onerror"),
                "the image alt broke out of its attribute: %s", body);
  KBC_CHECK(has(body, "alt=\"a&quot; onerror=&quot;x\""));
  free_page(&p);
}

/* The callout TYPE becomes a class name, so the alphanumerics-and-`-`
 * restriction is the only thing between a document and an injected
 * attribute. */
KBC_TEST(callout_type_cannot_break_out_of_the_class) {
  static const char md[] = "> [!note\" onclick=\"alert(1)] T\n> b\n";
  kbc_markdown_page p;
  render_str(md, &p);
  const char *body = body_of(&p);
  KBC_CHECK_MSG(!has(body, "kb-callout"),
                "the type escaped into the class attribute: %s", body);
  KBC_CHECK_MSG(has(body, "[!note\" onclick=\"alert(1)] T"),
                "the marker must survive as the literal text it is: %s", body);
  KBC_CHECK(has(body, "<blockquote>"));
  free_page(&p);
}

KBC_TEST(whitespace_only_document) {
  kbc_err err;
  kbc_err_reset(&err);
  kbc_markdown_page p;
  KBC_CHECK_OK(kbc_markdown_render("   \n\t\n", 6, NULL, &p, &err));
  KBC_CHECK(has(p.html, "<main class=\"kb-md-doc\"></main>"));
  KBC_CHECK(has(p.html, "<title>Untitled</title>"));
  free_page(&p);
}

/* `javascript:` hrefs are emitted, and that is DELIBERATE: the Rust original
 * emits them too, byte for byte (markdown.rs renders with
 * `render.unsafe = true`), and containment is the serving route's
 * `Content-Security-Policy: sandbox` — see src/httpd.c and
 * tests/test_httpd.c. Pinning it here means the next person to "harden" the
 * renderer has to argue with a test rather than with a reader of a comment. */
KBC_TEST(javascript_url_is_passed_through_deliberately) {
  static const char md[] = "[c](javascript:alert(1))\n";
  kbc_markdown_page p;
  render_str(md, &p);
  KBC_CHECK_MSG(has(body_of(&p), "href=\"javascript:alert(1)\""),
                "javascript: href: %s", body_of(&p));
  free_page(&p);
}

/* The header promises `len` UP TO KBC_MAX_ARTIFACT_BYTES, so exactly the
 * ceiling has to render; only MAX+1 is a refusal. */
KBC_TEST(title_at_the_size_ceiling_is_accepted) {
  size_t n = (size_t)KBC_MAX_ARTIFACT_BYTES;
  char *big = malloc(n + 1u);
  KBC_CHECK_NOT_NULL(big);
  if (big == NULL) return;
  memset(big, 'a', n);
  big[n] = '\0';
  kbc_err err;
  kbc_err_reset(&err);
  kbc_markdown_page p;
  KBC_CHECK_MSG(kbc_markdown_render(big, n, NULL, &p, &err) == KBC_OK,
                "the ceiling itself must render, not be refused: %s", err.msg);
  free_page(&p);
  free(big);
}

/* The callout pre-pass's fast path copies the source verbatim. A document
 * with no callout and no trailing newline is the only shape in which losing
 * that last byte is visible, which is why every other document missed it. */
KBC_TEST(a_document_with_no_callout_is_byte_identical) {
  static const char md[] = "# T\n\nx";
  kbc_markdown_page p;
  render_str(md, &p);
  KBC_CHECK_MSG(has(p.html, "<p>x</p>\n</main>"),
                "the fast path dropped the document's last byte: %s",
                body_of(&p));
  free_page(&p);
}

/* The arena path has to be OBSERVABLE, not merely produce the same bytes:
 * ignoring the arena argument entirely satisfies every comparison against
 * the heap path, which is why comparing the two renderings is not the test.
 * What the arena contract actually promises is that the page lives in the
 * arena and dies with it, so that is what is asserted: the arena's byte
 * count has to have grown by at least the size of what it was handed. */
KBC_TEST(arena_render_puts_the_page_inside_the_arena) {
  static const char md[] = "---\ntitle: T\n---\n# H\n\n> [!note] n\n> body\n";
  kbc_arena *a = kbc_arena_new(4096);
  KBC_CHECK_NOT_NULL(a);
  if (a == NULL) return;
  kbc_err err;
  kbc_err_reset(&err);
  kbc_markdown_page heap, aren;
  KBC_CHECK_OK(kbc_markdown_render(md, strlen(md), NULL, &heap, &err));
  size_t before = kbc_arena_bytes(a);
  KBC_CHECK_OK(kbc_markdown_render(md, strlen(md), a, &aren, &err));
  KBC_CHECK_EQ_STR(aren.html, heap.html);
  KBC_CHECK_EQ_STR(aren.title, heap.title);
  KBC_CHECK_MSG(kbc_arena_bytes(a) >= before + strlen(aren.html) + 1u,
                "the arena grew by %zu bytes but holds an %zu byte page: "
                "the page came from the heap",
                kbc_arena_bytes(a) - before, strlen(aren.html));
  free_page(&heap);
  kbc_arena_free(a); /* aren.html and aren.title die with the arena */
}

/* CommonMark §6.6: a link may not contain another link, and the one that
 * loses is the OUTER. Spec example 518, `[foo [bar](/uri)](/uri)`, renders as
 * `[foo <a href="/uri">bar</a>](/uri)`, and comrak agrees — so kb-c does too,
 * because the original is what a reader diffs against. Before the fix
 * `[a [b](c)](d)` emitted `<a href="d">a <a href="c">b</a></a>`: nested
 * anchors, which the browser resolves to the inner href and which the
 * reference never emits at all. */
KBC_TEST(a_link_may_not_contain_another_link) {
  kbc_markdown_page p;
  render_str("[a [b](c)](d)\n", &p);
  KBC_CHECK_MSG(!has(body_of(&p), "<a href=\"d\">"),
                "the OUTER link is the one §6.6 drops: %s", body_of(&p));
  KBC_CHECK(has(body_of(&p), "[a <a href=\"c\">b</a>](d)"));
  KBC_CHECK_MSG(!has(body_of(&p), "<a href=\"c\"><a"),
                "no anchor inside an anchor: %s", body_of(&p));
  free_page(&p);

  /* The rule is about links, not about brackets. A label holding brackets
   * that are not a link is still a perfectly good link — example 513's
   * mirror — and a label holding an IMAGE is a link whose whole content is
   * that image, which is spec example 517 and must keep working. */
  render_str("[a [b] c](d)\n", &p);
  KBC_CHECK(has(body_of(&p), "<a href=\"d\">a [b] c</a>"));
  free_page(&p);
  render_str("[![i](s.png)](l)\n", &p);
  KBC_CHECK(has(body_of(&p),
                "<a href=\"l\"><img src=\"s.png\" alt=\"i\" /></a>"));
  free_page(&p);

  /* §6.6 says "at any level of nesting": a link inside emphasis inside a
   * label still suppresses the outer one (example 519). */
  render_str("[a *[b](c)* d](e)\n", &p);
  KBC_CHECK_MSG(!has(body_of(&p), "<a href=\"e\">"),
                "emphasis does not launder a nested link: %s", body_of(&p));
  KBC_CHECK(has(body_of(&p), "<em><a href=\"c\">b</a></em>"));
  free_page(&p);
}

/* A raw-text HTML block ends at the line carrying its closing tag, and that
 * line is PART of the block. Two bugs met here. `close_tag` was never
 * NUL-terminated, so `strlen` read past the tag name into whatever was on
 * the stack and the end condition usually did not fire: a `<script>` ran on
 * to the next blank line and the paragraphs after it were handed to the
 * browser as script source. And the loop's second `break` dropped the
 * closing line out of the block before it was written, so a script ending
 * on `foo </script> bar` lost that line and re-rendered it as prose. */
KBC_TEST(raw_text_html_block_ends_at_its_closing_tag) {
  kbc_markdown_page p;
  render_str("<script>\nvar a = 1 < 2;\n</script>\n\ntext\n", &p);
  KBC_CHECK_MSG(has(body_of(&p), "<script>\nvar a = 1 < 2;\n</script>"),
                "the block must reach its closing tag: %s", body_of(&p));
  KBC_CHECK_MSG(has(body_of(&p), "<p>text</p>"),
                "what follows the block is a paragraph again: %s",
                body_of(&p));
  KBC_CHECK_MSG(!has(body_of(&p), "<p>var a"),
                "the script body must not become a paragraph: %s",
                body_of(&p));
  free_page(&p);

  /* The closing line is emitted, not consumed and re-parsed. */
  render_str("<script>\nvar a;\nfoo </script> bar\n\nafter\n", &p);
  KBC_CHECK_MSG(has(body_of(&p), "foo </script> bar\n<p>after</p>"),
                "the closing line stays inside the block: %s", body_of(&p));
  KBC_CHECK_MSG(!has(body_of(&p), "<p>foo"),
                "the closing line must not be re-parsed as prose: %s",
                body_of(&p));
  free_page(&p);
}

/* A tight list item still gets a newline after `<li>` when its first block
 * is not a paragraph, because a paragraph is the only thing that can sit on
 * the `<li>` line. The original writes `<li>` newline `<blockquote>` and
 * `<li>text`; the rule used to be "loose or not", which glued the block tag
 * to the `<li>`. An item with no content at all has no first block and gets
 * neither. */
KBC_TEST(tight_item_starts_a_non_paragraph_block_on_its_own_line) {
  kbc_markdown_page p;
  render_str("- > x\n", &p);
  KBC_CHECK_MSG(has(body_of(&p), "<li>\n<blockquote>"),
                "a quote starts its own line inside a tight item: %s",
                body_of(&p));
  free_page(&p);
  render_str("- a\n", &p);
  KBC_CHECK_MSG(has(body_of(&p), "<li>a</li>"),
                "a tight paragraph still sits on the <li> line: %s",
                body_of(&p));
  free_page(&p);
  render_str("- \n- real\n-\n", &p);
  KBC_CHECK_MSG(has(body_of(&p), "<li></li>"),
                "an empty item has no first block: %s", body_of(&p));
  KBC_CHECK_MSG(!has(body_of(&p), "<li>\n</li>"),
                "an empty item must not gain a newline: %s", body_of(&p));
  free_page(&p);
}

/* Every backward-looking scan in the inline pass is bounded twice, and this
 * is the bound doing its job: four megabytes of `[a](` is a million openers
 * each looking for a `)` that the document never closes. The destination scan
 * used to walk to the end of the document every time — 86 s for this input,
 * and four times that for four times the bytes. It is now linear, at about
 * half a second. The bound is 10 s, which is twenty times the fixed cost and
 * an order of magnitude under the quadratic one, so this is not a tight
 * race: it fails by a factor of eight before the fix and passes by a factor
 * of twenty after it. The output is asserted too, so the case cannot pass by
 * rendering nothing. */
KBC_TEST(pathological_inline_input_is_bounded_not_quadratic) {
  static const size_t reps = 1u << 20; /* 4 MiB of "[a](" */
  char *md = malloc(reps * 4u + 1u);
  KBC_CHECK_NOT_NULL(md);
  if (md == NULL) return;
  for (size_t i = 0; i < reps; i++) memcpy(md + i * 4u, "[a](", 4);
  md[reps * 4u] = '\0';
  kbc_err err;
  kbc_err_reset(&err);
  kbc_markdown_page p = {NULL, NULL};
  clock_t t0 = clock();
  KBC_CHECK_OK(kbc_markdown_render(md, reps * 4u, NULL, &p, &err));
  double secs = (double)(clock() - t0) / (double)CLOCKS_PER_SEC;
  KBC_CHECK_MSG(secs < 10.0,
                "4 MiB of \"[a](\" took %.2fs: a backward-looking scan is "
                "unbudgeted again",
                secs);
  /* No `[a](` is a link — the construct never closes — so the whole
   * document is one paragraph of literal text. */
  KBC_CHECK_MSG(has(body_of(&p), "[a](<p>") || has(body_of(&p), "[a]("),
                "the text must survive: %.80s", body_of(&p));
  KBC_CHECK_MSG(!has(body_of(&p), "<a href="),
                "an unclosed destination is not a link: %.80s", body_of(&p));
  free_page(&p);
  free(md);
}



int main(void) {
  static const kbc_test_case cases[] = {
      {"page_is_a_self_contained_document", page_is_a_self_contained_document},
      {"stylesheet_is_present", stylesheet_is_present},
      {"frontmatter_title_beats_the_first_h1",
       frontmatter_title_beats_the_first_h1},
      {"title_falls_back_to_h1_then_to_untitled",
       title_falls_back_to_h1_then_to_untitled},
      {"heading_inside_a_fence_is_not_the_title",
       heading_inside_a_fence_is_not_the_title},
      {"title_is_escaped_in_the_head", title_is_escaped_in_the_head},
      {"bom_does_not_eat_the_document", bom_does_not_eat_the_document},
      {"leading_thematic_break_is_not_frontmatter",
       leading_thematic_break_is_not_frontmatter},
      {"frontmatter_closes_on_a_padded_fence",
       frontmatter_closes_on_a_padded_fence},
      {"crlf_document_renders_like_an_lf_one",
       crlf_document_renders_like_an_lf_one},
      {"callout_renders_the_div", callout_renders_the_div},
      {"callout_default_title_and_type_casing",
       callout_default_title_and_type_casing},
      {"callout_title_is_escaped", callout_title_is_escaped},
      {"plain_blockquote_is_untouched", plain_blockquote_is_untouched},
      {"document_without_a_callout_is_not_rewritten",
       document_without_a_callout_is_not_rewritten},
      {"fenced_code_inside_a_callout_still_renders_as_code",
       fenced_code_inside_a_callout_still_renders_as_code},
      {"table_and_task_list", table_and_task_list},
      {"code_content_is_escaped_not_passed_through",
       code_content_is_escaped_not_passed_through},
      {"raw_html_passes_through", raw_html_passes_through},
      {"inline_grammar", inline_grammar},
      {"link_destination_cannot_break_out_of_the_attribute",
       link_destination_cannot_break_out_of_the_attribute},
      {"nested_lists_and_setext_headings", nested_lists_and_setext_headings},
      {"unsupported_constructs_render_as_text",
       unsupported_constructs_render_as_text},
      {"reference_link_renders_as_text", reference_link_renders_as_text},
      {"oversized_document_is_refused_not_truncated",
       oversized_document_is_refused_not_truncated},
      {"invalid_utf8_is_refused", invalid_utf8_is_refused},
      {"null_arguments_are_rejected", null_arguments_are_rejected},
      {"empty_document_renders_an_empty_shell",
       empty_document_renders_an_empty_shell},
      {"code_span_content_is_escaped", code_span_content_is_escaped},
      {"backslash_escape_yields_a_literal_character",
       backslash_escape_yields_a_literal_character},
      {"unterminated_tag_does_not_swallow_the_document",
       unterminated_tag_does_not_swallow_the_document},
      {"nul_byte_becomes_the_replacement_character",
       nul_byte_becomes_the_replacement_character},
      {"callout_after_a_paragraph_is_well_formed",
       callout_after_a_paragraph_is_well_formed},
      {"autolink_at_the_start_of_a_line", autolink_at_the_start_of_a_line},
      {"email_autolink_carries_mailto", email_autolink_carries_mailto},
      {"loose_list_wraps_every_item", loose_list_wraps_every_item},
      {"a_list_item_may_contain_several_blocks",
       a_list_item_may_contain_several_blocks},
      {"setext_underline_closes_a_paragraph_of_any_length",
       setext_underline_closes_a_paragraph_of_any_length},
      {"indented_code_is_a_code_block", indented_code_is_a_code_block},
      {"plain_text_is_escaped_in_every_block",
       plain_text_is_escaped_in_every_block},
      {"image_alt_and_link_title_are_escaped",
       image_alt_and_link_title_are_escaped},
      {"callout_type_cannot_break_out_of_the_class",
       callout_type_cannot_break_out_of_the_class},
      {"whitespace_only_document", whitespace_only_document},
      {"javascript_url_is_passed_through_deliberately",
       javascript_url_is_passed_through_deliberately},
      {"title_at_the_size_ceiling_is_accepted",
       title_at_the_size_ceiling_is_accepted},
      {"a_partial_tag_keeps_the_paragraph_in_markdown",
       a_partial_tag_keeps_the_paragraph_in_markdown},
      {"a_document_with_no_callout_is_byte_identical",
       a_document_with_no_callout_is_byte_identical},
      {"arena_render_puts_the_page_inside_the_arena",
       arena_render_puts_the_page_inside_the_arena},
      {"a_link_may_not_contain_another_link",
       a_link_may_not_contain_another_link},
      {"raw_text_html_block_ends_at_its_closing_tag",
       raw_text_html_block_ends_at_its_closing_tag},
      {"tight_item_starts_a_non_paragraph_block_on_its_own_line",
       tight_item_starts_a_non_paragraph_block_on_its_own_line},
      {"pathological_inline_input_is_bounded_not_quadratic",
       pathological_inline_input_is_bounded_not_quadratic},
      {NULL, NULL},
  };
  return kbc_test_run("markdown", cases);
}
