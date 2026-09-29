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
  KBC_CHECK(has(body, "<code>&lt;a href=&quot;x&quot;&gt;</code>"));
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
  KBC_CHECK_MSG(has(body, "&lt;a title=&quot;"), "escaped as text: %s", body);
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
  KBC_CHECK_MSG(has(body, "[!note&quot; onclick=&quot;alert(1)] T"),
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

/* CommonMark §6.5: the alt attribute "contains a plain string", and a
 * label's BYTES are not that — `![a *b*](i)` is `alt="a b"`, not
 * `alt="a *b*"`. Before the fix the raw label was escaped into the
 * attribute verbatim, so every inline construct inside an image label
 * reached the page as its own markup: an unclosed emphasis, an unterminated
 * code span, a stray backslash. The reference (comrak, which forces every
 * node under an image to plain text) produces the text on all of these. */
KBC_TEST(image_alt_is_the_labels_text_not_its_bytes) {
  kbc_markdown_page p;

  render_str("![a *b*](i)\n", &p);
  KBC_CHECK_MSG(has(body_of(&p), "<img src=\"i\" alt=\"a b\" />"),
                "emphasis in a label is markup, not alt text: %s", body_of(&p));
  KBC_CHECK_MSG(!has(body_of(&p), "alt=\"a *b*\""),
                "the raw label reached the attribute: %s", body_of(&p));
  free_page(&p);

  render_str("![a `c`](i)\n", &p);
  KBC_CHECK(has(body_of(&p), "alt=\"a c\""));
  free_page(&p);

  /* A backslash escape is a literal CHARACTER, so the backslash is markup
   * too and the alt carries the character alone. */
  render_str("![a \\[x\\]](i)\n", &p);
  KBC_CHECK(has(body_of(&p), "alt=\"a [x]\""));
  free_page(&p);

  /* Spec example 574: a nested image contributes its own alt. Its own alt is
   * computed by this same call one level down, so the nesting is not bounded
   * to one. */
  render_str("![foo ![bar](/url)](/url2)\n", &p);
  KBC_CHECK_MSG(has(body_of(&p), "<img src=\"/url2\" alt=\"foo bar\" />"),
                "a nested image inlines its alt: %s", body_of(&p));
  free_page(&p);
  render_str("![a ![b *c*](j) d](i)\n", &p);
  KBC_CHECK(has(body_of(&p), "alt=\"a b c d\""));
  free_page(&p);

  /* A link in a label keeps its TEXT and loses its anchor, and so does raw
   * HTML — comrak escapes an inline tag under an image rather than passing
   * it through, so a document that quotes `<b>` in an alt gets the literal
   * text and not a bold run inside an attribute. */
  render_str("![a [b](c) d](i)\n", &p);
  KBC_CHECK(has(body_of(&p), "alt=\"a b d\""));
  free_page(&p);
  render_str("![a <b>c</b>](i)\n", &p);
  KBC_CHECK_MSG(has(body_of(&p), "alt=\"a &lt;b&gt;c&lt;/b&gt;\""),
                "raw HTML in a label is escaped text: %s", body_of(&p));
  free_page(&p);

  /* Either kind of line break is ONE space in the alt — the two trailing
   * spaces a hard break swallows are still swallowed — and the double space
   * an author wrote is NOT collapsed, because nothing here trims. */
  render_str("![a  \nb](i)\n", &p);
  KBC_CHECK(has(body_of(&p), "alt=\"a b\""));
  free_page(&p);
  render_str("![a\nb](i)\n", &p);
  KBC_CHECK(has(body_of(&p), "alt=\"a b\""));
  free_page(&p);
  render_str("![a &  b](i)\n", &p);
  KBC_CHECK(has(body_of(&p), "alt=\"a &amp;  b\""));
  free_page(&p);

  /* The strip does not re-escape: `esc` has already run over every one of
   * these bytes, and a second pass over `&` is what would leave the alt
   * reading `a &amp; b`. The `"` still has to be escaped, because the raw
   * label's was not — the quote is what ends the attribute. */
  render_str("![a \"b\" c](i)\n", &p);
  KBC_CHECK_MSG(has(body_of(&p), "alt=\"a &quot;b&quot; c\""),
                "a quote in a label must not close the alt: %s", body_of(&p));
  KBC_CHECK_MSG(!has(body_of(&p), "alt=\"a \"b\""),
                "the alt broke out of its attribute: %s", body_of(&p));
  free_page(&p);

  /* An entity in a label stays a pass-through, exactly as it does in a
   * paragraph and for the reason the header gives: the document said
   * `&amp;` and the browser is what resolves it. */
  render_str("![a &amp; b](i)\n", &p);
  KBC_CHECK(has(body_of(&p), "alt=\"a &amp; b\""));
  free_page(&p);
}

/* CommonMark §6.6's raw HTML is a GRAMMAR, and an attribute name is
 * `[A-Za-z_:][A-Za-z0-9_.:-]*`. `raw_tag` used to be a quote-tracking search
 * for the next `>`, so `<a b@c>` and `<a @click="x">` came out as live tags:
 * a passthrough that accepts bytes the original escapes, on the one path the
 * `render.unsafe = true` contract and the kb-prompt template both depend on.
 * Each assertion below is a shape, checked in both directions: what the
 * grammar ACCEPTS must still pass through untouched, and only what it
 * rejects becomes text. */
KBC_TEST(a_raw_tag_attribute_name_must_parse) {
  kbc_markdown_page p;

  /* The accept set. `_` and `:` are the two first characters the rule
   * admits beyond a letter, and `<div :prop="y">` is the shape the report
   * that found this used to show the two renderers AGREEING, so it has to
   * keep agreeing. */
  static const char *const pass[] = {
      "<a b=\"1\">\n",   "<a b = \"1\">\n", "<a b='1'>\n",
      "<a b=1>\n",       "<a b>\n",         "<a _x=\"1\">\n",
      "<a :x=\"1\">\n",   "<a data-x=\"1\">\n", "<a x-y.z:w=\"1\">\n",
      "<a b c=\"1\">\n", "<a/>\n",          "<a />\n",
      "<a b=\"1\"/>\n",  "<a b=\"1>\">\n",   "<div :prop=\"y\">\n",
      "<div prop=y>\n",  "<img src=x onerror=alert(1)>\n\nafter\n",
      "</a>\n",          "</a >\n",
  };
  for (size_t i = 0; i < sizeof pass / sizeof pass[0]; i++) {
    render_str(pass[i], &p);
    KBC_CHECK_MSG(!has(body_of(&p), "&lt;"),
                  "the grammar ACCEPTS this and it must pass through: %s -> %s",
                  pass[i], body_of(&p));
    free_page(&p);
  }

  /* The reject set. Every one of these is `&lt;…&gt;` in the reference, and
   * a name that does not start with a letter, `_` or `:` is the rule. */
  static const char *const fail[] = {
      "<a b@c>\n",       "<a @click=\"x\">\n", "<a 1b=\"1\">\n",
      "<a b\"1\">\n",    "<a =\"1\">\n",       "<a b=\"1\"@c>\n",
      "<a 1>\n",        "<a b=>\n",          "<a b=\"1\"c=\"2\">\n",
      "<a //>\n",       "<a / >\n",
  };
  for (size_t i = 0; i < sizeof fail / sizeof fail[0]; i++) {
    render_str(fail[i], &p);
    KBC_CHECK_MSG(has(body_of(&p), "&lt;a ") || has(body_of(&p), "&lt;div "),
                  "the grammar REJECTS this and it must be text: %s -> %s",
                  fail[i], body_of(&p));
    free_page(&p);
  }

  /* A closing tag carries no attributes at all, so `</a b="1">` is text. */
  render_str("</a b=\"1\">\n", &p);
  KBC_CHECK_MSG(has(body_of(&p), "&lt;/a b=&quot;1&quot;&gt;"),
                "a closing tag takes no attributes: %s", body_of(&p));
  free_page(&p);

  /* A tag may still SPAN lines — the spec's whitespace is `space_or_tab |
   * newline` — and an unquoted value still may not swallow the newline, so
   * a tag that never closes is still bounded rather than eating the rest of
   * the paragraph. */
  render_str("<a\nb=\"1\">\n", &p);
  KBC_CHECK_MSG(!has(body_of(&p), "&lt;a"),
                "a newline is whitespace inside a tag: %s", body_of(&p));
  free_page(&p);
  render_str("<a b=\"1\n\nVISIBLE CONTENT\n", &p);
  KBC_CHECK_MSG(has(body_of(&p), "&lt;a b=&quot;1"),
                "an unclosed tag is still text: %s", body_of(&p));
  KBC_CHECK(has(body_of(&p), "VISIBLE CONTENT"));
  free_page(&p);

  /* Type 6 is decided by the tag NAME and nothing else, so tightening the
   * grammar must not have demoted `<div @click="x">` from a block to a
   * paragraph. That one is a block in the original. */
  render_str("<div @click=\"x\">\n\npara\n", &p);
  KBC_CHECK_MSG(has(body_of(&p), "<div @click=\"x\">"),
                "a block tag is a block whatever its attributes say: %s",
                body_of(&p));
  free_page(&p);

  /* And the passthrough itself is untouched: well-formed raw HTML still
   * reaches the page as markup, which is the contract, not a bug. */
  render_str("<span class=\"x\">hi</span>\n", &p);
  KBC_CHECK(has(body_of(&p), "<span class=\"x\">hi</span>"));
  free_page(&p);
}

/* A backtick run that never closes is not a code span, it is literal text,
 * and the `]` after it still ends the label. The label scanner used to
 * abort the whole construct on one, so a single stray backtick anywhere in
 * a label lost the link or the image — `[a`](b)` came out as literal text
 * where the original has `<a href="b">a`</a>`. */
KBC_TEST(a_stray_backtick_in_a_label_is_not_a_code_span) {
  kbc_markdown_page p;

  render_str("[a`](b)\n", &p);
  KBC_CHECK_MSG(has(body_of(&p), "<a href=\"b\">a`</a>"),
                "an unclosed backtick must not lose the link: %s", body_of(&p));
  free_page(&p);

  render_str("![a`](i)\n", &p);
  KBC_CHECK_MSG(has(body_of(&p), "<img src=\"i\" alt=\"a`\" />"),
                "an unclosed backtick must not lose the image: %s",
                body_of(&p));
  free_page(&p);

  /* The rule is about CLOSING, not about counting: one backtick, two, and
   * a code span that does close all behave the way the label scan says. */
  render_str("[a``](b)\n", &p);
  KBC_CHECK(has(body_of(&p), "<a href=\"b\">a``</a>"));
  free_page(&p);
  render_str("[a `b](c)\n", &p);
  KBC_CHECK(has(body_of(&p), "<a href=\"c\">a `b</a>"));
  free_page(&p);
  /* A `]` INSIDE a closed code span still is not the end of the label, which
   * is the other half of the same test and the reason the scan skips. */
  render_str("[a `]b`](c)\n", &p);
  KBC_CHECK(has(body_of(&p), "<a href=\"c\">a <code>]b</code></a>"));
  free_page(&p);
  /* And in prose, a `]` after an unclosed backtick is still just a `]`. */
  render_str("a `b] c\n", &p);
  KBC_CHECK(has(body_of(&p), "<p>a `b] c</p>"));
  free_page(&p);
}
/* A hard line break is two trailing spaces, and it has to survive in EVERY
 * block that feeds a paragraph's own lines. The blockquote arm trimmed its
 * content with `trim`, which stripped them, so a quote asked for a break and
 * got a soft one — the one context each of the earlier passes had tested a
 * break in except, which is how it stayed. */
KBC_TEST(a_hard_line_break_survives_in_a_blockquote) {
  kbc_markdown_page p;

  render_str("> a  \n> b\n", &p);
  KBC_CHECK_MSG(has(body_of(&p), "<p>a<br />\nb</p>"),
                "two trailing spaces in a quote are a hard break: %s",
                body_of(&p));
  free_page(&p);

  /* The mid-paragraph case that already worked, so the fix is not "strip the
   * spaces": the break is still a break where a line follows it. */
  render_str("> a  \n> b  \n> c\n", &p);
  KBC_CHECK(has(body_of(&p), "<p>a<br />\nb<br />\nc</p>"));
  free_page(&p);

  /* And the two spaces are a break, not literal text, in a list and at top
   * level — the same rule in every context. */
  render_str("- a  \n  b\n", &p);
  KBC_CHECK(has(body_of(&p), "<li>a<br />\nb</li>"));
  free_page(&p);
  render_str("a  \nb\n", &p);
  KBC_CHECK(has(body_of(&p), "<p>a<br />\nb</p>"));
  free_page(&p);
}

/* A GFM table's header + delimiter pair interrupts a paragraph. The
 * interrupt test lived in `starts_block`, which sees ONE line and so cannot
 * know whether the next is the delimiter row — which is why a table could
 * only ever open at a block boundary, and `item\n| a | b |\n|---|---|` came
 * out as one paragraph. */
KBC_TEST(a_table_can_interrupt_a_paragraph) {
  kbc_markdown_page p;

  render_str("item\n| a | b |\n|---|---|\n| 1 | 2 |\n", &p);
  KBC_CHECK_MSG(has(body_of(&p), "<p>item</p>"),
                "the text above the table stays its own paragraph: %s",
                body_of(&p));
  KBC_CHECK(has(body_of(&p), "<table>"));
  KBC_CHECK(has(body_of(&p), "<th>a</th>"));
  KBC_CHECK(has(body_of(&p), "<td>2</td>"));
  /* The header alone is not a table, with or without a paragraph above it:
   * the DELIMITER row is what makes the pair, and that is the whole reason
   * the test needs the next line. */
  free_page(&p);
  render_str("item\n| a | b |\n", &p);
  KBC_CHECK_MSG(!has(body_of(&p), "<table>"),
                "a header row with no delimiter row is not a table: %s",
                body_of(&p));
  free_page(&p);
  render_str("| a | b |\n|---|---|\n| 1 | 2 |\n", &p);
  KBC_CHECK(has(body_of(&p), "<table>"));
  free_page(&p);
}

/* A `<li>` gets a newline after it when its FIRST BLOCK is not a paragraph.
 * The test was the item's first LINE, so a setext heading — whose underline
 * is the block's SECOND line — read as a paragraph and came out
 * `<li><h1>title</h1>`. A GFM task mark is the same shape: it is only a
 * task marker when the content starts with a paragraph, so `- [x] t\n  ===`
 * is a heading whose text is `[x] t` and carries no checkbox. */
KBC_TEST(li_newline_when_the_first_block_is_not_a_paragraph) {
  kbc_markdown_page p;

  render_str("- t\n  ===\n- u\n", &p);
  KBC_CHECK_MSG(has(body_of(&p), "<li>\n<h1>t</h1>"),
                "a setext heading as the item's first block needs the newline: %s",
                body_of(&p));
  free_page(&p);
  render_str("- t\n  ---\n- u\n", &p);
  KBC_CHECK(has(body_of(&p), "<li>\n<h2>t</h2>"));
  free_page(&p);

  /* The task mark is text when the first block is a heading, not a checkbox:
   * the mark is read only where a paragraph can follow it on the `<li>` line. */
  render_str("- [x] t\n  ===\n", &p);
  KBC_CHECK_MSG(!has(body_of(&p), "type=\"checkbox\""),
                "a task mark before a heading is literal text: %s",
                body_of(&p));
  KBC_CHECK(has(body_of(&p), "<h1>[x] t</h1>"));
  free_page(&p);
  /* And it IS a checkbox when the item does start with a paragraph. */
  render_str("- [x] a\n- u\n", &p);
  KBC_CHECK(has(body_of(&p), "<li><input type=\"checkbox\" checked=\"\""));
  free_page(&p);
  /* A tight item whose first block IS a paragraph keeps the text on the
   * `<li>` line — the one case the newline rule is not for. */
  render_str("- a\n  b\n- c\n", &p);
  KBC_CHECK(has(body_of(&p), "<li>a\nb</li>"));
  free_page(&p);
}

/* A paragraph's LAST line loses its trailing spaces, including the two that
 * would be a hard line break: with nothing after it the break is not a
 * break, and the original strips them rather than emitting a `<br />` or the
 * spaces. Mid-paragraph they are a break and stay one. */
KBC_TEST(a_trailing_hard_break_marker_is_stripped) {
  kbc_markdown_page p;

  render_str("a  \n", &p);
  KBC_CHECK_MSG(has(body_of(&p), "<p>a</p>"),
                "a hard break at the end of a paragraph is not a break: %s",
                body_of(&p));
  KBC_CHECK(!has(body_of(&p), "<br />"));
  free_page(&p);
  render_str("- a  \n", &p);
  KBC_CHECK(has(body_of(&p), "<li>a</li>"));
  free_page(&p);
  /* Mid-paragraph it is still a break: this is not "the spaces are gone". */
  render_str("a  \nb\n", &p);
  KBC_CHECK(has(body_of(&p), "<p>a<br />\nb</p>"));
  free_page(&p);
  /* A single trailing space is stripped the same way. */
  render_str("a \n", &p);
  KBC_CHECK(has(body_of(&p), "<p>a</p>"));
  free_page(&p);
}

/* The space BETWEEN two emphasis runs is ordinary inline text.
 *
 * `emphasis_run` renders the text between a pair by recursing into
 * `md_inline` on exactly those bytes, so the separator in `*a* *b*` reaches
 * the end of a run as a whole one-space run with nothing after it. A flush
 * that dropped the pending spaces at the end of a run — the shape a fix for
 * the trailing hard-break marker takes — therefore ate it, and every pair of
 * adjacent emphasis runs rendered glued together. Whether spaces are
 * TRAILING is a property of the paragraph, not of the run, and this is the
 * case that says the two were confused. */
KBC_TEST(the_space_between_two_emphasis_runs_is_text) {
  kbc_markdown_page p;

  render_str("*a* *b*\n", &p);
  KBC_CHECK_MSG(has(body_of(&p), "<em>a</em> <em>b</em>"),
                "the separator between two runs must survive: %s",
                body_of(&p));
  free_page(&p);
  /* Any order, any delimiter, and inside a paragraph rather than a heading —
   * this is the shape that made a golden document a STRUCT_DIFF. */
  render_str("**a** *b*\n", &p);
  KBC_CHECK(has(body_of(&p), "<strong>a</strong> <em>b</em>"));
  free_page(&p);
  render_str("_a_ __b__\n", &p);
  KBC_CHECK(has(body_of(&p), "<em>a</em> <strong>b</strong>"));
  free_page(&p);
  render_str("x *b* **c** y\n", &p);
  KBC_CHECK(has(body_of(&p), "x <em>b</em> <strong>c</strong> y"));
  free_page(&p);
  render_str("# *b* **c**\n", &p);
  KBC_CHECK(has(body_of(&p), "<h1><em>b</em> <strong>c</strong></h1>"));
  free_page(&p);
  /* More than one space is more than one space. */
  render_str("*a*  *b*\n", &p);
  KBC_CHECK(has(body_of(&p), "<em>a</em>  <em>b</em>"));
  free_page(&p);
}

/* CR is a line ending (CommonMark §2.1), so a CR-only document is not one
 * line. `line_at` split on `\n` alone and stripped a trailing `\r`, so every
 * construct in such a document — headings, quotes, lists, hard breaks —
 * collapsed into a single paragraph. */
KBC_TEST(a_cr_only_document_has_its_lines) {
  kbc_markdown_page p;

  render_str("a\rb\r# h\r", &p);
  KBC_CHECK_MSG(has(body_of(&p), "<p>a\nb</p>"),
                "a bare CR ends a line: %s", body_of(&p));
  KBC_CHECK_MSG(has(body_of(&p), "<h1>h</h1>"),
                "and a heading after one is a heading: %s", body_of(&p));
  free_page(&p);

  /* A hard break is a break there too, which is the same `line_at`. */
  render_str("a  \rb\r", &p);
  KBC_CHECK(has(body_of(&p), "<p>a<br />\nb</p>"));
  free_page(&p);

  /* CRLF is still ONE terminator and the CR is still not part of the line —
   * the case that already worked, so the change did not break it. */
  render_str("a\r\nb\r\n", &p);
  KBC_CHECK(has(body_of(&p), "<p>a\nb</p>"));
  free_page(&p);
  /* A CRLF document's title is still its first h1. */
  render_str("a\r\n# T\r\n", &p);
  KBC_CHECK_MSG(p.title != NULL && strcmp(p.title, "T") == 0,
                "CRLF title: %s", p.title == NULL ? "(null)" : p.title);
  free_page(&p);
  /* And a CR-ONLY document's title is NOT its heading: the original finds
   * the title by splitting the source on `\n` (markdown.rs:250) and trimming,
   * while the parser it hands the document to treats a bare CR as a line
   * ending. The two are the reference's own asymmetry, and the renderer
   * inherits it rather than papering over it. */
  render_str("a\rb\r# h\r", &p);
  KBC_CHECK_MSG(p.title != NULL && strcmp(p.title, "Untitled") == 0,
                "the title scan splits on LF only: %s",
                p.title == NULL ? "(null)" : p.title);
  free_page(&p);
}

/* A link destination may not contain a line ending, inside angle brackets
 * or not (CommonMark example 491), so `[a](<foo\nbar>)` is not a link at all.
 * The scan ran to the `>` regardless and the construct succeeded with the
 * newline percent-encoded into the href. */
KBC_TEST(a_link_destination_cannot_span_a_line) {
  kbc_markdown_page p;

  render_str("[a](<foo\nbar>)\n", &p);
  KBC_CHECK_MSG(!has(body_of(&p), "<a "),
                "a destination with a newline in it is not a link: %s",
                body_of(&p));
  KBC_CHECK(!has(body_of(&p), "%0A"));
  free_page(&p);
  render_str("![a](<x\ny>)\n", &p);
  KBC_CHECK(!has(body_of(&p), "<img "));
  free_page(&p);
  /* The bare form already refused, and a destination on ONE line is still a
   * link: this is about the line ending, not about angle brackets. */
  render_str("[a](b)\n", &p);
  KBC_CHECK(has(body_of(&p), "<a href=\"b\">a</a>"));
  free_page(&p);
  render_str("[a](<b>)\n", &p);
  KBC_CHECK(has(body_of(&p), "<a href=\"b\">a</a>"));
  free_page(&p);
}

/* A comment opens with `<!--` and a processing instruction with `<?` — four
 * and two bytes. The guards were one byte too long each, so a line that was
 * nothing but its own opener fell through to a paragraph and was ESCAPED,
 * which is the sequence a comment most often starts on and the one whose
 * escaping is what stops the comment being reinterpreted downstream. */
KBC_TEST(a_bare_html_comment_opener_is_a_block) {
  kbc_markdown_page p;

  render_str("<!--\nx\n-->\n", &p);
  KBC_CHECK_MSG(has(body_of(&p), "<!--\nx\n-->"),
                "a bare `<!--` opens an HTML block: %s", body_of(&p));
  KBC_CHECK_MSG(!has(body_of(&p), "&lt;!--"),
                "and is not escaped into the prose: %s", body_of(&p));
  free_page(&p);

  render_str("<?\nx\n?>\n", &p);
  KBC_CHECK_MSG(has(body_of(&p), "<?\nx\n?>"),
                "a bare `<?` opens an HTML block: %s", body_of(&p));
  free_page(&p);

  /* A comment with content, and one inline, both unchanged: the fix is the
   * exact length of each opener, not a widening of the class. */
  render_str("<!-- Foo -->\n", &p);
  KBC_CHECK(has(body_of(&p), "<!-- Foo -->"));
  free_page(&p);
  render_str("a <!-- c --> b\n", &p);
  KBC_CHECK(has(body_of(&p), "<p>a <!-- c --> b</p>"));
  free_page(&p);
  /* A `<` that is not an opener is still a paragraph, and `<?php` — longer
   * than the bare form, so it always worked — still is. */
  render_str("a < b\n", &p);
  KBC_CHECK(has(body_of(&p), "<p>a &lt; b</p>"));
  free_page(&p);
  render_str("<?php echo 1; ?>\n", &p);
  KBC_CHECK(has(body_of(&p), "<?php echo 1; ?>"));
  free_page(&p);
}

/* A setext underline that arrives as a blockquote's LAZY continuation is
 * text, not a heading: a lazy line carries no `>`, so it cannot be the
 * underline of a paragraph inside the quote. The arm appended it unmarked
 * and `md_para` — which sees a flat buffer with no record of which lines
 * carried a marker — read `===` as an underline and closed the paragraph as
 * an `<h1>` inside the quote. */
KBC_TEST(a_lazy_underline_in_a_quote_is_not_a_heading) {
  kbc_markdown_page p;

  render_str("> foo\nbar\n===\n", &p);
  KBC_CHECK_MSG(!has(body_of(&p), "<h1>"),
                "a lazy `===` is not a heading: %s", body_of(&p));
  KBC_CHECK(has(body_of(&p), "<p>foo\nbar\n===</p>"));
  free_page(&p);

  /* A lazy `---` leaves the quote instead: it is a thematic break, and a
   * thematic break cannot be inside a paragraph. */
  render_str("> foo\nbar\n---\n", &p);
  KBC_CHECK(has(body_of(&p), "<blockquote>\n<p>foo\nbar</p>\n</blockquote>"));
  KBC_CHECK(has(body_of(&p), "<hr />"));
  free_page(&p);

  /* An explicit `> ---` IS the heading it looks like — the arm can see the
   * difference, which is what the fix rests on. */
  render_str("> foo\n> ===\n", &p);
  KBC_CHECK(has(body_of(&p), "<h1>foo</h1>"));
  free_page(&p);
  render_str("> t\n> ===\n", &p);
  KBC_CHECK(has(body_of(&p), "<h1>t</h1>"));
  free_page(&p);
}

/* The page is a function of the document and of nothing else.
 *
 * `kbc_markdown_render` builds its output buffers as locals of its own, and
 * `md_para` renders a paragraph through the sink `md_blocks` was HANDED
 * rather than one of its own — so a flag on that struct is a byte of
 * uninitialised stack, and whatever the frame below happened to hold decides
 * the output. It did: the same source at `-O3` and under ASan rendered
 * `Foo\nBar` as `<h2>Foo\nBar</h2>`, and under TSan as `<h2>Foo Bar</h2>`,
 * because TSan happened to leave a non-zero byte in the slot. Neither
 * sanitizer reports an uninitialised read, so a green lane proved nothing.
 *
 * So this asks the question a test can actually ask: render the same
 * documents twice, from two call stacks deliberately filled with different
 * bytes, and require one answer. `poison_below` recurses so its frames sit
 * BELOW the frame `kbc_markdown_render` is about to occupy — filling the
 * caller's own frame would be above it and prove nothing. */
static void poison_below(int depth, unsigned char fill) {
  volatile unsigned char buf[1024];
  for (size_t i = 0; i < sizeof buf; i++) buf[i] = fill;
  if (depth > 0) poison_below(depth - 1, fill);
  (void)buf[0];
}

static void render_over_poison(const char *md, unsigned char fill, char *dst,
                               size_t cap) {
  kbc_markdown_page p;
  kbc_err err;
  memset(&p, 0, sizeof p);
  poison_below(12, fill);
  kbc_err_reset(&err);
  /* No arena: `render_str` takes the same NULL and hands the page back
   * `free`-owned, which is what lets `free_page` below be `free_page`. */
  KBC_CHECK_OK(kbc_markdown_render(md, strlen(md), NULL, &p, &err));
  snprintf(dst, cap, "%s", body_of(&p));
  free_page(&p);
}

KBC_TEST(the_page_does_not_depend_on_the_callers_stack) {
  /* The three shapes the uninitialised flag used to decide: a soft break
   * (a newline written as a space), a hard break (the same, plus the two
   * spaces), and the raw-tag passthrough (escaped instead of passed
   * through). Each is stated once, as the bytes a reader of the page gets. */
  static const struct {
    const char *md;
    const char *want;
  } cases[] = {
      {"Foo\nBar\n---\n\nBaz\nQux\n===\n",
       "<h2>Foo\nBar</h2>\n<h1>Baz\nQux</h1>\n"},
      {"> a  \n> b\n", "<blockquote>\n<p>a<br />\nb</p>\n</blockquote>\n"},
      {"<span class=\"x\">hi</span>\n", "<p><span class=\"x\">hi</span></p>\n"},
      /* A tag that SPANS lines is one tag: the newline is the spec's
       * `space_or_tab | newline` whitespace. No closing tag is invented —
       * that is the author's business — so this `want` stops at `</p>`. */
      {"<a\nb=\"1\">\n", "<p><a\nb=\"1\"></p>\n"},
      {"- a  \n  b\n", "<ul>\n<li>a<br />\nb</li>\n</ul>\n"},
  };
  char clean[4096], dirty[4096];
  for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
    render_over_poison(cases[i].md, 0x00, clean, sizeof clean);
    render_over_poison(cases[i].md, 0xFF, dirty, sizeof dirty);
    KBC_CHECK_MSG(strcmp(clean, dirty) == 0,
                  "case %zu renders two ways from the same document: "
                  "<<%s>> vs <<%s>>",
                  i, clean, dirty);
    KBC_CHECK_MSG(strncmp(clean, cases[i].want, strlen(cases[i].want)) == 0,
                  "case %zu: got <<%s>> want <<%s>>", i, clean,
                  cases[i].want);
  }
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
      {"image_alt_is_the_labels_text_not_its_bytes",
       image_alt_is_the_labels_text_not_its_bytes},
      {"a_raw_tag_attribute_name_must_parse",
       a_raw_tag_attribute_name_must_parse},
      {"a_stray_backtick_in_a_label_is_not_a_code_span",
       a_stray_backtick_in_a_label_is_not_a_code_span},
      {"a_hard_line_break_survives_in_a_blockquote",
       a_hard_line_break_survives_in_a_blockquote},
      {"a_table_can_interrupt_a_paragraph", a_table_can_interrupt_a_paragraph},
      {"li_newline_when_the_first_block_is_not_a_paragraph",
       li_newline_when_the_first_block_is_not_a_paragraph},
      {"a_trailing_hard_break_marker_is_stripped",
       a_trailing_hard_break_marker_is_stripped},
      {"the_space_between_two_emphasis_runs_is_text",
       the_space_between_two_emphasis_runs_is_text},
      {"a_cr_only_document_has_its_lines", a_cr_only_document_has_its_lines},
      {"a_link_destination_cannot_span_a_line",
       a_link_destination_cannot_span_a_line},
      {"a_bare_html_comment_opener_is_a_block",
       a_bare_html_comment_opener_is_a_block},
      {"a_lazy_underline_in_a_quote_is_not_a_heading",
       a_lazy_underline_in_a_quote_is_not_a_heading},
      {"the_page_does_not_depend_on_the_callers_stack",
       the_page_does_not_depend_on_the_callers_stack},
      {NULL, NULL},
  };
  return kbc_test_run("markdown", cases);
}
