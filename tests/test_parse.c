/* test_parse.c — the reader parse (Markdown/HTML -> blocks + anchors) and the
 * tokenizer. */
#include "kbc/mem.h"
#include "kbc/parse.h"
#include "kbc/types.h"
#include "kbc_test.h"

/* ---------------------------------------------------------------- utils -- */

static kbc_parsed *parse_doc(kbc_arena *a, const char *text) {
  kbc_err err;
  kbc_err_reset(&err);
  kbc_parsed *p = kbc_parse(a, text, strlen(text), "notes/doc.md", &err);
  KBC_CHECK_MSG(p != NULL, "parse returned NULL: %s", err.msg);
  return p;
}

/* Out-of-range reads return a shared zero block so a wrong block count is
 * reported as a failed assertion instead of a segfault. The len checks that
 * precede every use are what actually catch the bug. */
static const kbc_block *block_at(const kbc_blocks *bs, size_t i) {
  static const kbc_block missing = {"", "", 0, 0, 0};
  if (bs == NULL || i >= bs->len) return &missing;
  return &bs->items[i];
}

/* ------------------------------------------------------------- markdown -- */

KBC_TEST(md_atx_headings) {
  kbc_arena *a = kbc_arena_new(4096);
  kbc_parsed *p = parse_doc(a,
                            "# One\n"
                            "## Two\n"
                            "### Three\n"
                            "#### Four\n"
                            "##### Five\n"
                            "###### Six\n"
                            "####### Seven\n");
  const kbc_blocks *bs = kbc_parsed_blocks(p);
  KBC_CHECK_NOT_NULL(bs);
  KBC_CHECK_EQ_INT(bs->len, 7);

  /* the level, the heading text in the source's own case, and a slugified id */
  static const int want_level[7] = {1, 2, 3, 4, 5, 6, 0};
  static const char *const want_text[7] = {"One",  "Two",   "Three", "Four",
                                            "Five", "Six",   "Seven"};
  static const char *const want_slug[7] = {"one", "two", "three", "four",
                                            "five", "six", "seven"};
  for (size_t i = 0; i < 7; i++) {
    const kbc_block *b = block_at(bs, i);
    KBC_CHECK_NOT_NULL(b);
    if (b == NULL) continue;
    KBC_CHECK_EQ_INT(b->heading_level, want_level[i]);
    KBC_CHECK_EQ_STR(b->text, want_text[i]);
    KBC_CHECK_EQ_STR(b->id, want_slug[i]);
    KBC_CHECK(kbc_parsed_has_anchor(p, b->id));
  }
  kbc_arena_free(a);
}

KBC_TEST(md_atx_heading_trims_closing_hashes) {
  kbc_arena *a = kbc_arena_new(4096);
  kbc_parsed *p = parse_doc(a, "## Fix the Parser ##\n");
  const kbc_blocks *bs = kbc_parsed_blocks(p);
  KBC_CHECK_EQ_INT(bs->len, 1);
  const kbc_block *b = block_at(bs, 0);
  KBC_CHECK_NOT_NULL(b);
  if (b != NULL) {
    KBC_CHECK_EQ_INT(b->heading_level, 2);
    KBC_CHECK_EQ_STR(b->text, "Fix the Parser");
    KBC_CHECK_EQ_STR(b->id, "fix-the-parser");
  }
  kbc_arena_free(a);
}

KBC_TEST(md_setext_headings) {
  kbc_arena *a = kbc_arena_new(4096);
  kbc_parsed *p = parse_doc(a, "Title One\n"
                            "=========\n"
                            "\n"
                            "Title Two\n"
                            "---------\n"
                            "\n"
                            "Body text.\n");
  const kbc_blocks *bs = kbc_parsed_blocks(p);
  KBC_CHECK(bs->len >= 3);
  const kbc_block *h1 = block_at(bs, 0);
  const kbc_block *h2 = block_at(bs, 1);
  const kbc_block *body = block_at(bs, 2);
  KBC_CHECK_NOT_NULL(h1);
  KBC_CHECK_NOT_NULL(h2);
  KBC_CHECK_NOT_NULL(body);
  if (h1 != NULL) {
    KBC_CHECK_EQ_INT(h1->heading_level, 1);
    KBC_CHECK_EQ_STR(h1->text, "Title One");
    KBC_CHECK_EQ_STR(h1->id, "title-one");
  }
  if (h2 != NULL) {
    KBC_CHECK_EQ_INT(h2->heading_level, 2);
    KBC_CHECK_EQ_STR(h2->text, "Title Two");
    KBC_CHECK_EQ_STR(h2->id, "title-two");
  }
  if (body != NULL) {
    KBC_CHECK_EQ_INT(body->heading_level, 0);
    KBC_CHECK_EQ_STR(body->text, "Body text.");
  }
  /* '=' wins the title over the later h2. */
  KBC_CHECK_EQ_STR(kbc_parsed_title(p), "Title One");
  kbc_arena_free(a);
}

KBC_TEST(md_fenced_code_blocks) {
  kbc_arena *a = kbc_arena_new(4096);
  kbc_parsed *p = parse_doc(a, "# Title\n"
                            "\n"
                            "```c\n"
                            "int main(void) { return 0; }\n"
                            "```\n"
                            "\n"
                            "~~~\n"
                            "plain fence\n"
                            "~~~\n"
                            "\n"
                            "Trailing paragraph.\n");
  const kbc_blocks *bs = kbc_parsed_blocks(p);
  KBC_CHECK_EQ_INT(bs->len, 4);

  const kbc_block *lang = block_at(bs, 1);
  const kbc_block *nolang = block_at(bs, 2);
  const kbc_block *tail = block_at(bs, 3);
  KBC_CHECK_NOT_NULL(lang);
  KBC_CHECK_NOT_NULL(nolang);
  KBC_CHECK_NOT_NULL(tail);
  if (lang != NULL) {
    KBC_CHECK_EQ_INT(lang->heading_level, 0);
    KBC_CHECK_EQ_STR(lang->text, "int main(void) { return 0; }");
    /* fenced code carries the info string in its id */
    KBC_CHECK_EQ_STR(lang->id, "code-c-1");
    KBC_CHECK(kbc_parsed_has_anchor(p, "code-c-1"));
  }
  if (nolang != NULL) {
    KBC_CHECK_EQ_STR(nolang->text, "plain fence");
    /* fences are numbered document-wide, so this is the second one */
    KBC_CHECK_EQ_STR(nolang->id, "code-2");
  }
  if (tail != NULL) {
    KBC_CHECK_EQ_INT(tail->heading_level, 0);
    KBC_CHECK_EQ_STR(tail->text, "Trailing paragraph.");
  }
  /* A fence must not swallow the prose after it. */
  KBC_CHECK(!kbc_parsed_has_anchor(p, "code-c-0"));
  kbc_arena_free(a);
}

KBC_TEST(md_document_without_headings) {
  kbc_arena *a = kbc_arena_new(4096);
  kbc_parsed *p = parse_doc(a, "Just a line of prose\nand another one.\n");
  const kbc_blocks *bs = kbc_parsed_blocks(p);
  KBC_CHECK_EQ_INT(bs->len, 1);
  const kbc_block *b = block_at(bs, 0);
  KBC_CHECK_NOT_NULL(b);
  if (b != NULL) {
    KBC_CHECK_EQ_INT(b->heading_level, 0);
    KBC_CHECK_EQ_STR(b->text, "Just a line of prose and another one.");
    KBC_CHECK_EQ_STR(b->id, "b0");
  }
  kbc_arena_free(a);
}

KBC_TEST(md_paragraphs_split_on_blank_lines) {
  kbc_arena *a = kbc_arena_new(4096);
  kbc_parsed *p = parse_doc(a, "First para.\n"
                            "\n"
                            "\n"
                            "Second para.\n"
                            "\n"
                            "Third para.\n");
  const kbc_blocks *bs = kbc_parsed_blocks(p);
  KBC_CHECK_EQ_INT(bs->len, 3);
  static const char *const want[3] = {"First para.", "Second para.",
                                      "Third para."};
  for (size_t i = 0; i < 3; i++) {
    const kbc_block *b = block_at(bs, i);
    KBC_CHECK_NOT_NULL(b);
    if (b == NULL) continue;
    KBC_CHECK_EQ_STR(b->text, want[i]);
    KBC_CHECK_EQ_INT(b->heading_level, 0);
  }
  /* prose blocks get positional ids, and distinct ones */
  KBC_CHECK_EQ_STR(block_at(bs, 0)->id, "b0");
  KBC_CHECK_EQ_STR(block_at(bs, 2)->id, "b2");
  kbc_arena_free(a);
}

/* ----------------------------------------------------------------- html -- */

KBC_TEST(html_headings_h1_to_h6) {
  kbc_arena *a = kbc_arena_new(4096);
  kbc_parsed *p = parse_doc(a, "<h1>Alpha</h1><h2>Beta</h2><h3>Gamma</h3>"
                            "<h4>Delta</h4><h5>Epsilon</h5><h6>Zeta</h6>");
  const kbc_blocks *bs = kbc_parsed_blocks(p);
  KBC_CHECK_EQ_INT(bs->len, 6);
  for (size_t i = 0; i < 6; i++) {
    const kbc_block *b = block_at(bs, i);
    KBC_CHECK_NOT_NULL(b);
    if (b == NULL) continue;
    KBC_CHECK_EQ_INT(b->heading_level, (long long)i + 1);
  }
  KBC_CHECK_EQ_STR(block_at(bs, 0)->text, "Alpha");
  KBC_CHECK_EQ_STR(block_at(bs, 5)->text, "Zeta");
  KBC_CHECK_EQ_STR(block_at(bs, 2)->id, "gamma");
  KBC_CHECK_EQ_STR(kbc_parsed_title(p), "Alpha");
  kbc_arena_free(a);
}

KBC_TEST(html_title_element) {
  kbc_arena *a = kbc_arena_new(4096);
  kbc_parsed *p =
      parse_doc(a, "<html><head><title>Site Name</title></head>"
                  "<body><h2>Section</h2><p>text</p></body></html>");
  /* no h1: <title> wins over the prose block */
  KBC_CHECK_EQ_STR(kbc_parsed_title(p), "Site Name");
  kbc_arena_free(a);
}

KBC_TEST(html_explicit_ids_recorded_as_anchors) {
  kbc_arena *a = kbc_arena_new(4096);
  kbc_parsed *p = parse_doc(a, "<h2 id=\"sec-two\">Two</h2>\n"
                            "<p>body</p>\n"
                            "<div id='sec-three'>three</div>\n"
                            "<span id=\"inline-thing\">inline</span>\n");
  KBC_CHECK(kbc_parsed_has_anchor(p, "sec-two"));
  KBC_CHECK(kbc_parsed_has_anchor(p, "sec-three"));
  KBC_CHECK(kbc_parsed_has_anchor(p, "inline-thing"));
  /* the heading's own slug is an anchor too */
  KBC_CHECK(kbc_parsed_has_anchor(p, "two"));
  /* and a miss is a miss, not a fuzzy match */
  KBC_CHECK(!kbc_parsed_has_anchor(p, "sec-missing"));
  KBC_CHECK(!kbc_parsed_has_anchor(p, ""));
  kbc_arena_free(a);
}

KBC_TEST(html_script_and_style_excluded) {
  kbc_arena *a = kbc_arena_new(4096);
  kbc_parsed *p = parse_doc(a, "<h1>Visible</h1>\n"
                            "<script>var s = 'secret';</script>\n"
                            "<style>p { color: red; }</style>\n"
                            "<p>after</p>\n");
  const kbc_blocks *bs = kbc_parsed_blocks(p);
  KBC_CHECK_EQ_INT(bs->len, 2);
  for (size_t i = 0; i < bs->len; i++) {
    KBC_CHECK_MSG(strstr(bs->items[i].text, "secret") == NULL,
                  "script body leaked into block %zu: %s", i,
                  bs->items[i].text);
    KBC_CHECK_MSG(strstr(bs->items[i].text, "color") == NULL,
                  "style body leaked into block %zu: %s", i, bs->items[i].text);
  }
  KBC_CHECK_EQ_STR(block_at(bs, 0)->text, "Visible");
  KBC_CHECK_EQ_STR(block_at(bs, 1)->text, "after");
  kbc_arena_free(a);
}

KBC_TEST(html_entity_decoding) {
  kbc_arena *a = kbc_arena_new(4096);
  kbc_parsed *p = parse_doc(a, "<p>&amp;&lt;&gt;&quot;&#39;&nbsp;&copy;&nosuch;"
                            "&#65;&#x42;&#x2603;</p>");
  const kbc_blocks *bs = kbc_parsed_blocks(p);
  KBC_CHECK_EQ_INT(bs->len, 1);
  const kbc_block *b = block_at(bs, 0);
  KBC_CHECK_NOT_NULL(b);
  if (b != NULL) {
    /* &amp; &lt; &gt; &quot; &#39; ... &nosuch; stays verbatim, then the
     * numeric forms. The leading &nbsp; is a single space. */
    KBC_CHECK_EQ_STR(b->text, "&<>\"' \xc2\xa9&nosuch;AB\xe2\x98\x83");
    KBC_CHECK_EQ_INT(b->text_len, strlen("&<>\"' \xc2\xa9&nosuch;AB\xe2\x98\x83"));
  }
  kbc_arena_free(a);
}

KBC_TEST(html_br_becomes_space) {
  kbc_arena *a = kbc_arena_new(4096);
  kbc_parsed *p = parse_doc(a, "<p>one<br>two<br/>three</p>");
  const kbc_blocks *bs = kbc_parsed_blocks(p);
  KBC_CHECK_EQ_INT(bs->len, 1);
  KBC_CHECK_EQ_STR(block_at(bs, 0)->text, "one two three");
  kbc_arena_free(a);
}

KBC_TEST(html_nested_block_elements_each_a_block) {
  kbc_arena *a = kbc_arena_new(4096);
  kbc_parsed *p = parse_doc(a, "<div><section><p>inner</p></section></div>"
                            "<article>outer</article>");
  const kbc_blocks *bs = kbc_parsed_blocks(p);
  KBC_CHECK_EQ_INT(bs->len, 2);
  KBC_CHECK_EQ_STR(block_at(bs, 0)->text, "inner");
  KBC_CHECK_EQ_STR(block_at(bs, 1)->text, "outer");
  kbc_arena_free(a);
}

/* ------------------------------------------------ text hygiene + offsets -- */

KBC_TEST(block_text_collapses_whitespace) {
  kbc_arena *a = kbc_arena_new(4096);
  kbc_parsed *p = parse_doc(a, "  \t Alpha   beta\t\tgamma \n delta \n");
  const kbc_blocks *bs = kbc_parsed_blocks(p);
  KBC_CHECK_EQ_INT(bs->len, 1);
  const kbc_block *b = block_at(bs, 0);
  KBC_CHECK_NOT_NULL(b);
  if (b != NULL) {
    KBC_CHECK_EQ_STR(b->text, "Alpha beta gamma delta");
    KBC_CHECK(b->text[0] != ' ');
    KBC_CHECK(b->text[b->text_len - 1] != ' ');
    KBC_CHECK_EQ_INT(b->text_len, strlen(b->text));
  }
  kbc_arena_free(a);
}

KBC_TEST(block_text_collapses_whitespace_around_nbsp) {
  /* &nbsp; decodes to a space byte, so it is whitespace like any other and
   * must go through the same collapse as a literal space. */
  kbc_arena *a = kbc_arena_new(4096);
  kbc_parsed *p = parse_doc(a, "<p>alpha &nbsp; beta</p>");
  const kbc_blocks *bs = kbc_parsed_blocks(p);
  KBC_CHECK_EQ_INT(bs->len, 1);
  const kbc_block *b = block_at(bs, 0);
  KBC_CHECK_NOT_NULL(b);
  if (b != NULL) {
    KBC_CHECK_EQ_STR(b->text, "alpha beta");
    KBC_CHECK_EQ_INT(b->text_len, strlen("alpha beta"));
  }
  kbc_arena_free(a);
}

KBC_TEST(block_offsets_point_at_the_real_start) {
  static const char *const doc = "Intro sentence here.\n"
                                 "\n"
                                 "Second paragraph body.\n"
                                 "\n"
                                 "## A Heading\n"
                                 "\n"
                                 "Third paragraph body.\n";
  kbc_arena *a = kbc_arena_new(4096);
  kbc_parsed *p = parse_doc(a, doc);
  const kbc_blocks *bs = kbc_parsed_blocks(p);
  KBC_CHECK(bs->len >= 4);
  for (size_t i = 0; i < bs->len; i++) {
    const kbc_block *b = &bs->items[i];
    size_t o = b->offset;
    KBC_CHECK_MSG(o < strlen(doc), "block %zu offset %zu past end of source", i,
                  o);
    if (o >= strlen(doc)) continue;
    /* Skip the whitespace that precedes the block, then the source at the
     * recorded offset must spell the block's text. */
    while (doc[o] == ' ' || doc[o] == '\t' || doc[o] == '\n' || doc[o] == '#')
      o++;
    KBC_CHECK_MSG(memcmp(doc + o, b->text, b->text_len) == 0,
                  "block %zu offset %zu does not point at its text \"%s\" "
                  "(source there is \"%.20s\")",
                  i, b->offset, b->text, doc + b->offset);
  }
  kbc_arena_free(a);
}

/* ----------------------------------------------------------- robustness -- */

/* Every one of these must parse without crashing and without an error. */
static void assert_parses(const char *what, const char *text, size_t len) {
  kbc_arena *a = kbc_arena_new(4096);
  kbc_err err;
  kbc_err_reset(&err);
  kbc_parsed *p = kbc_parse(a, text, len, "notes/doc.md", &err);
  KBC_CHECK_MSG(p != NULL, "%s: parse failed: %s", what, err.msg);
  if (p != NULL) {
    const kbc_blocks *bs = kbc_parsed_blocks(p);
    KBC_CHECK_NOT_NULL(bs);
    for (size_t i = 0; i < bs->len; i++) {
      KBC_CHECK_MSG(bs->items[i].text != NULL && bs->items[i].id != NULL,
                    "%s: block %zu has a NULL field", what, i);
      if (bs->items[i].text == NULL) continue;
      KBC_CHECK_MSG(bs->items[i].text_len == strlen(bs->items[i].text),
                    "%s: block %zu text_len %zu != strlen %zu", what, i,
                    bs->items[i].text_len, strlen(bs->items[i].text));
    }
    KBC_CHECK_NOT_NULL(kbc_parsed_title(p));
  }
  kbc_arena_free(a);
}

KBC_TEST(robust_empty_and_whitespace_only) {
  assert_parses("empty", "", 0);
  assert_parses("spaces", "   \t  ", 7);
  assert_parses("newlines", "\n\n\n\n", 4);
}

KBC_TEST(robust_truncated_documents) {
  static const char *const md = "# Heading\n\nSome prose that gets cut off";
  assert_parses("md minus last byte", md, strlen(md) - 1);
  assert_parses("html minus last byte", "<h1>Ti", 6);
  assert_parses("half an entity", "text &am", 9);
  assert_parses("bare hash", "#", 1);
  assert_parses("bare fence", "```", 3);
}

KBC_TEST(robust_single_less_than) {
  kbc_arena *a = kbc_arena_new(4096);
  kbc_parsed *p = parse_doc(a, "<");
  const kbc_blocks *bs = kbc_parsed_blocks(p);
  KBC_CHECK_EQ_INT(bs->len, 1);
  KBC_CHECK_EQ_STR(block_at(bs, 0)->text, "<");
  kbc_arena_free(a);
}

KBC_TEST(robust_document_with_nul_byte) {
  static const char doc[] = "alpha\0beta";
  assert_parses("embedded NUL", doc, sizeof doc - 1);
}

KBC_TEST(robust_unterminated_html_tag) {
  kbc_arena *a = kbc_arena_new(4096);
  kbc_parsed *p = parse_doc(a, "<div class=\"unclosed\"><p>text</p>");
  const kbc_blocks *bs = kbc_parsed_blocks(p);
  KBC_CHECK(bs->len >= 1);
  kbc_arena_free(a);
}

KBC_TEST(robust_unclosed_code_fence) {
  kbc_arena *a = kbc_arena_new(4096);
  kbc_parsed *p = parse_doc(a, "# Title\n\n```c\nint x = 1;\n");
  const kbc_blocks *bs = kbc_parsed_blocks(p);
  KBC_CHECK_EQ_INT(bs->len, 2);
  KBC_CHECK_EQ_STR(block_at(bs, 1)->id, "code-c-1");
  KBC_CHECK_EQ_STR(block_at(bs, 1)->text, "int x = 1;");
  kbc_arena_free(a);
}

KBC_TEST(robust_one_mebibyte_document) {
  kbc_str doc;
  kbc_str_init(&doc);
  for (int i = 0; i < 15000; i++) {
    kbc_status st = kbc_str_printf(&doc, "## Heading %d\n\n"
                                         "Body paragraph number %d with some "
                                         "filler words in it.\n\n",
                                   i, i);
    KBC_CHECK_OK(st);
  }
  KBC_CHECK(doc.len > 1000000u);
  KBC_CHECK(doc.len <= KBC_MAX_ARTIFACT_BYTES);
  assert_parses("1 MiB markdown", doc.ptr, doc.len);

  /* over the hard ceiling is an error, not a parse */
  kbc_arena *a = kbc_arena_new(4096);
  kbc_err err;
  kbc_err_reset(&err);
  KBC_CHECK_NULL(kbc_parse(a, doc.ptr, (size_t)KBC_MAX_ARTIFACT_BYTES + 1,
                           "notes/doc.md", &err));
  KBC_CHECK_ERR_MSG(err);
  KBC_CHECK_NULL(kbc_parse(a, NULL, 0, "notes/doc.md", &err));
  KBC_CHECK_ERR_MSG(err);
  KBC_CHECK_NULL(kbc_parse(NULL, "x", 1, "notes/doc.md", &err));
  KBC_CHECK_ERR_MSG(err);
  kbc_arena_free(a);
  kbc_str_free(&doc);
}

KBC_TEST(robust_deeply_nested_html) {
  enum { DEPTH = 4000 };
  kbc_str doc;
  kbc_str_init(&doc);
  for (int i = 0; i < DEPTH; i++) {
    KBC_CHECK_OK(kbc_str_puts(&doc, "<div>"));
  }
  KBC_CHECK_OK(kbc_str_puts(&doc, "deep text"));
  for (int i = 0; i < DEPTH; i++) {
    KBC_CHECK_OK(kbc_str_puts(&doc, "</div>"));
  }
  assert_parses("4000 nested divs", doc.ptr, doc.len);

  kbc_str_free(&doc);
}

KBC_TEST(robust_deeply_nested_html_keeps_only_the_text) {
  enum { DEPTH = 500 };
  kbc_str doc;
  kbc_str_init(&doc);
  for (int i = 0; i < DEPTH; i++) KBC_CHECK_OK(kbc_str_puts(&doc, "<section>"));
  KBC_CHECK_OK(kbc_str_puts(&doc, "deep text"));
  for (int i = 0; i < DEPTH; i++) KBC_CHECK_OK(kbc_str_puts(&doc, "</section>"));

  kbc_arena *a = kbc_arena_new(4096);
  kbc_err err;
  kbc_err_reset(&err);
  kbc_parsed *p = kbc_parse(a, doc.ptr, doc.len, "notes/doc.md", &err);
  KBC_CHECK_MSG(p != NULL, "%s", err.msg);
  if (p != NULL) {
    const kbc_blocks *bs = kbc_parsed_blocks(p);
    KBC_CHECK(bs->len >= 1);
    const kbc_block *last = block_at(bs, bs->len - 1);
    KBC_CHECK_NOT_NULL(last);
    if (last != NULL) KBC_CHECK_EQ_STR(last->text, "deep text");
  }
  kbc_arena_free(a);
  kbc_str_free(&doc);
}

/* --------------------------------------------------------------- title --- */

KBC_TEST(title_first_h1_wins) {
  kbc_arena *a = kbc_arena_new(4096);
  kbc_parsed *p = parse_doc(a, "# First Title\n\n# Second Title\n\nprose\n");
  KBC_CHECK_EQ_STR(kbc_parsed_title(p), "First Title");
  kbc_arena_free(a);
}

KBC_TEST(title_falls_back_to_title_element_then_prose) {
  kbc_arena *a = kbc_arena_new(4096);
  kbc_parsed *p =
      parse_doc(a, "<html><head><title>Doc Title</title></head>"
                  "<body><h2>Sub</h2><p>prose line</p></body></html>");
  KBC_CHECK_EQ_STR(kbc_parsed_title(p), "Doc Title");
  kbc_arena_free(a);

  a = kbc_arena_new(4096);
  p = parse_doc(a, "No headings here.\n\nSecond para.\n");
  KBC_CHECK_EQ_STR(kbc_parsed_title(p), "No headings here.");
  kbc_arena_free(a);
}

KBC_TEST(title_falls_back_to_filename_stem) {
  kbc_arena *a = kbc_arena_new(4096);
  kbc_err err;
  kbc_err_reset(&err);
  kbc_parsed *p = kbc_parse(a, "", 0, "notes/2026/rfc.md", &err);
  KBC_CHECK_MSG(p != NULL, "%s", err.msg);
  if (p != NULL) KBC_CHECK_EQ_STR(kbc_parsed_title(p), "rfc");
  kbc_arena_free(a);

  a = kbc_arena_new(4096);
  kbc_err_reset(&err);
  p = kbc_parse(a, "   \n\t\n", 6, "notes/2026/rfc.md", &err);
  KBC_CHECK_MSG(p != NULL, "%s", err.msg);
  if (p != NULL) {
    KBC_CHECK_EQ_INT(kbc_parsed_blocks(p)->len, 0);
    KBC_CHECK_EQ_STR(kbc_parsed_title(p), "rfc");
  }
  kbc_arena_free(a);

  a = kbc_arena_new(4096);
  kbc_err_reset(&err);
  p = kbc_parse(a, "", 0, "index", &err);
  KBC_CHECK_MSG(p != NULL, "%s", err.msg);
  if (p != NULL) KBC_CHECK_EQ_STR(kbc_parsed_title(p), "index");
  kbc_arena_free(a);
}

/* ----------------------------------------------------------- tokenizer --- */

static kbc_tokens *tok(kbc_arena *a, const char *text, kbc_err *err) {
  kbc_tokens *out = kbc_arena_calloc(a, 1, sizeof *out);
  kbc_tokens_init(out);
  kbc_status st = kbc_tokenize(a, text, strlen(text), out, err);
  KBC_CHECK_OK(st);
  return out;
}

KBC_TEST(tokenize_content_words_with_offsets) {
  kbc_arena *a = kbc_arena_new(4096);
  kbc_err err;
  kbc_err_reset(&err);
  kbc_tokens *t = tok(a, "The Quick, brown fox!", &err);
  KBC_CHECK_EQ_INT(t->len, 3);
  if (t->len == 3) {
    static const char *const want[3] = {"quick", "brown", "fox"};
    /* "The" is a stopword; the content words sit at 4, 11 and 17. */
    static const long long start[3] = {4, 11, 17};
    for (size_t i = 0; i < 3; i++) {
      KBC_CHECK_EQ_STR(t->items[i].text, want[i]);
      KBC_CHECK_EQ_INT(t->items[i].start, start[i]);
      KBC_CHECK_EQ_INT(t->items[i].end, start[i] + (long long)strlen(want[i]));
      /* the offset range must really be the source bytes, case aside */
      KBC_CHECK_EQ_INT(t->items[i].end - t->items[i].start,
                       (long long)strlen(t->items[i].text));
    }
  }
  kbc_arena_free(a);
}

KBC_TEST(tokenize_emits_duplicates_per_occurrence) {
  kbc_arena *a = kbc_arena_new(4096);
  kbc_err err;
  kbc_err_reset(&err);
  kbc_tokens *t = tok(a, "alpha beta alpha", &err);
  KBC_CHECK_EQ_INT(t->len, 3);
  KBC_CHECK_EQ_STR(t->items[0].text, "alpha");
  KBC_CHECK_EQ_STR(t->items[1].text, "beta");
  KBC_CHECK_EQ_STR(t->items[2].text, "alpha");
  /* the repeat is later in the source than the first */
  KBC_CHECK(t->items[2].start > t->items[0].start);
  kbc_arena_free(a);
}

KBC_TEST(tokenize_drops_stopwords) {
  kbc_arena *a = kbc_arena_new(4096);
  kbc_err err;
  kbc_err_reset(&err);
  kbc_tokens *t = tok(a, "the quick brown and the lazy dog", &err);
  KBC_CHECK_EQ_INT(t->len, 4);
  if (t->len == 4) {
    KBC_CHECK_EQ_STR(t->items[0].text, "quick");
    KBC_CHECK_EQ_STR(t->items[1].text, "brown");
    KBC_CHECK_EQ_STR(t->items[2].text, "lazy");
    KBC_CHECK_EQ_STR(t->items[3].text, "dog");
  }
  /* a word that is only a stopword yields nothing at all */
  kbc_tokens *u = tok(a, "the and of", &err);
  KBC_CHECK_EQ_INT(u->len, 0);
  kbc_arena_free(a);
}

KBC_TEST(tokenize_drops_overlong_terms) {
  kbc_arena *a = kbc_arena_new(4096);
  kbc_err err;
  kbc_err_reset(&err);
  char big[KBC_MAX_TERM_LEN + 8];
  memset(big, 'x', sizeof big);
  big[sizeof big - 1] = '\0';

  /* exactly at the ceiling is kept */
  char ok[KBC_MAX_TERM_LEN + 1];
  memset(ok, 'x', KBC_MAX_TERM_LEN);
  ok[KBC_MAX_TERM_LEN] = '\0';
  kbc_tokens *t = tok(a, ok, &err);
  KBC_CHECK_EQ_INT(t->len, 1);
  KBC_CHECK_EQ_INT(t->items[0].len, KBC_MAX_TERM_LEN);

  /* one byte over is dropped entirely, not truncated */
  char over[KBC_MAX_TERM_LEN + 2];
  memset(over, 'x', KBC_MAX_TERM_LEN + 1);
  over[KBC_MAX_TERM_LEN + 1] = '\0';
  kbc_tokens *u = tok(a, over, &err);
  KBC_CHECK_EQ_INT(u->len, 0);

  /* the neighbours of a dropped overlong term survive */
  char mixed[512];
  snprintf(mixed, sizeof mixed, "alpha %s omega", big);
  kbc_tokens *v = tok(a, mixed, &err);
  KBC_CHECK_EQ_INT(v->len, 2);
  KBC_CHECK_EQ_STR(v->items[0].text, "alpha");
  KBC_CHECK_EQ_STR(v->items[1].text, "omega");
  kbc_arena_free(a);
}

KBC_TEST(tokenize_drops_one_character_tokens) {
  kbc_arena *a = kbc_arena_new(4096);
  kbc_err err;
  kbc_err_reset(&err);
  kbc_tokens *t = tok(a, "q x z 7 ab", &err);
  KBC_CHECK_EQ_INT(t->len, 1);
  KBC_CHECK_EQ_STR(t->items[0].text, "ab");
  kbc_arena_free(a);
}

KBC_TEST(tokenize_folds_diacritics) {
  kbc_arena *a = kbc_arena_new(4096);
  kbc_err err;
  kbc_err_reset(&err);
  kbc_tokens *t = tok(a, "caf\u00e9 cafe", &err);
  KBC_CHECK_EQ_INT(t->len, 2);
  if (t->len == 2) {
    KBC_CHECK_EQ_STR(t->items[0].text, "cafe");
    KBC_CHECK_EQ_STR(t->items[1].text, "cafe");
    /* folding does not change where the token started */
    KBC_CHECK_EQ_INT(t->items[0].start, 0);
    /* "café" is 5 bytes, so the following "cafe" starts at byte 6 */
    KBC_CHECK_EQ_INT(t->items[1].start, 6);
  }
  kbc_arena_free(a);
}

KBC_TEST(tokenize_empty_string) {
  kbc_arena *a = kbc_arena_new(4096);
  kbc_err err;
  kbc_err_reset(&err);
  kbc_tokens t;
  kbc_tokens_init(&t);
  KBC_CHECK_OK(kbc_tokenize(a, "", 0, &t, &err));
  KBC_CHECK_EQ_INT(t.len, 0);
  kbc_arena_free(a);
}

KBC_TEST(tokenize_rejects_null_arguments) {
  kbc_arena *a = kbc_arena_new(4096);
  kbc_err err;
  kbc_err_reset(&err);
  kbc_tokens t;
  kbc_tokens_init(&t);
  KBC_CHECK_ERR(kbc_tokenize(NULL, "x", 1, &t, &err), KBC_ERR_INVALID);
  KBC_CHECK_ERR_MSG(err);
  KBC_CHECK_ERR(kbc_tokenize(a, NULL, 0, &t, &err), KBC_ERR_INVALID);
  KBC_CHECK_ERR_MSG(err);
  KBC_CHECK_ERR(kbc_tokenize(a, "x", 1, NULL, &err), KBC_ERR_INVALID);
  KBC_CHECK_ERR_MSG(err);
  kbc_arena_free(a);
}

KBC_TEST(tokenize_enforces_token_count_cap) {
  kbc_arena *a = kbc_arena_new(4096);
  kbc_err err;
  kbc_err_reset(&err);
  kbc_tokens t;
  kbc_tokens_init(&t);

  /* one 2-byte word every 3 bytes: exactly the cap must succeed, one more
   * word must be refused */
  size_t cap = KBC_MAX_TOKENS_PER_DOC;
  char *doc = malloc(cap * 3 + 8);
  KBC_CHECK_NOT_NULL(doc);
  if (doc == NULL) {
    kbc_arena_free(a);
    return;
  }
  for (size_t i = 0; i < cap; i++) memcpy(doc + i * 3, "ab ", 3);
  size_t at_cap = cap * 3 - 1; /* drop the trailing space */
  KBC_CHECK_OK(kbc_tokenize(a, doc, at_cap, &t, &err));
  KBC_CHECK_EQ_INT(t.len, cap);

  /* one more token must be refused, with a message and no partial write */
  memcpy(doc + cap * 3, "cd", 2);
  size_t over_cap = cap * 3 + 2;
  kbc_err_reset(&err);
  KBC_CHECK_ERR(kbc_tokenize(a, doc, over_cap, &t, &err), KBC_ERR_INVALID);
  KBC_CHECK_ERR_MSG(err);
  KBC_CHECK_EQ_INT(t.len, cap);
  free(doc);
  kbc_arena_free(a);
}

KBC_TEST(tokens_init_zeroes_the_vector) {
  kbc_tokens t;
  memset(&t, 0xAA, sizeof t);
  kbc_tokens_init(&t);
  KBC_CHECK_NULL(t.items);
  KBC_CHECK_EQ_INT(t.len, 0);
  KBC_CHECK_EQ_INT(t.cap, 0);
}

KBC_TEST(tokenize_appends_into_an_existing_vector) {
  kbc_arena *a = kbc_arena_new(4096);
  kbc_err err;
  kbc_err_reset(&err);
  kbc_tokens t;
  kbc_tokens_init(&t);

  KBC_CHECK_OK(kbc_tokenize(a, "alpha beta", 10, &t, &err));
  KBC_CHECK_EQ_INT(t.len, 2);
  size_t first_cap = t.cap;

  KBC_CHECK_OK(kbc_tokenize(a, "gamma", 5, &t, &err));
  KBC_CHECK_EQ_INT(t.len, 3);
  KBC_CHECK(t.cap >= first_cap);
  KBC_CHECK_EQ_STR(t.items[0].text, "alpha");
  KBC_CHECK_EQ_STR(t.items[1].text, "beta");
  KBC_CHECK_EQ_STR(t.items[2].text, "gamma");
  /* the pre-existing tokens were not shifted or clobbered by the realloc */
  KBC_CHECK_EQ_INT(t.items[0].start, 0);
  KBC_CHECK_EQ_INT(t.items[1].start, 6);
  KBC_CHECK_EQ_INT(t.items[2].start, 0);
  kbc_arena_free(a);
}

/* ------------------------------------------------------------- slugify --- */

static void check_slug(const char *in, const char *want) {
  kbc_arena *a = kbc_arena_new(4096);
  kbc_err err;
  kbc_err_reset(&err);
  kbc_str out;
  kbc_str_init(&out);
  kbc_status st = kbc_slugify(a, in, strlen(in), &out, &err);
  KBC_CHECK_OK(st);
  KBC_CHECK_EQ_STR(out.ptr, want);
  kbc_str_free(&out);
  kbc_arena_free(a);
}

KBC_TEST(slugify_basic) {
  check_slug("How to Fix the Parser!", "how-to-fix-the-parser");
  check_slug("Section 12: Basics", "section-12-basics");
  check_slug("Caf\u00e9 au lait", "cafe-au-lait");
}

KBC_TEST(slugify_trims_and_collapses_separators) {
  check_slug("  --Hello---World--  ", "hello-world");
  check_slug("a", "a");
  check_slug("...trailing...", "trailing");
}

KBC_TEST(slugify_all_punctuation_is_empty) {
  kbc_arena *a = kbc_arena_new(4096);
  kbc_err err;
  kbc_err_reset(&err);
  kbc_str out;
  kbc_str_init(&out);
  KBC_CHECK_OK(kbc_slugify(a, "!!!", 3, &out, &err));
  KBC_CHECK_EQ_INT(out.len, 0);
  KBC_CHECK(out.ptr == NULL || out.ptr[0] == '\0');
  kbc_str_free(&out);
  KBC_CHECK_OK(kbc_slugify(a, "", 0, &out, &err));
  KBC_CHECK_EQ_INT(out.len, 0);
  kbc_str_free(&out);

  kbc_err_reset(&err);
  KBC_CHECK_ERR(kbc_slugify(NULL, "x", 1, &out, &err), KBC_ERR_INVALID);
  KBC_CHECK_ERR_MSG(err);
  KBC_CHECK_ERR(kbc_slugify(a, NULL, 0, &out, &err), KBC_ERR_INVALID);
  KBC_CHECK_ERR_MSG(err);
  KBC_CHECK_ERR(kbc_slugify(a, "x", 1, NULL, &err), KBC_ERR_INVALID);
  KBC_CHECK_ERR_MSG(err);
  kbc_arena_free(a);
}

/* ---------------------------------------------------------------- main --- */

static const kbc_test_case cases[] = {
    {"md_atx_headings", md_atx_headings},
    {"md_atx_heading_trims_closing_hashes", md_atx_heading_trims_closing_hashes},
    {"md_setext_headings", md_setext_headings},
    {"md_fenced_code_blocks", md_fenced_code_blocks},
    {"md_document_without_headings", md_document_without_headings},
    {"md_paragraphs_split_on_blank_lines", md_paragraphs_split_on_blank_lines},
    {"html_headings_h1_to_h6", html_headings_h1_to_h6},
    {"html_title_element", html_title_element},
    {"html_explicit_ids_recorded_as_anchors",
     html_explicit_ids_recorded_as_anchors},
    {"html_script_and_style_excluded", html_script_and_style_excluded},
    {"html_entity_decoding", html_entity_decoding},
    {"html_br_becomes_space", html_br_becomes_space},
    {"html_nested_block_elements_each_a_block",
     html_nested_block_elements_each_a_block},
    {"block_text_collapses_whitespace", block_text_collapses_whitespace},
    {"block_text_collapses_whitespace_around_nbsp",
     block_text_collapses_whitespace_around_nbsp},
    {"block_offsets_point_at_the_real_start", block_offsets_point_at_the_real_start},
    {"robust_empty_and_whitespace_only", robust_empty_and_whitespace_only},
    {"robust_truncated_documents", robust_truncated_documents},
    {"robust_single_less_than", robust_single_less_than},
    {"robust_document_with_nul_byte", robust_document_with_nul_byte},
    {"robust_unterminated_html_tag", robust_unterminated_html_tag},
    {"robust_unclosed_code_fence", robust_unclosed_code_fence},
    {"robust_one_mebibyte_document", robust_one_mebibyte_document},
    {"robust_deeply_nested_html", robust_deeply_nested_html},
    {"robust_deeply_nested_html_keeps_only_the_text",
     robust_deeply_nested_html_keeps_only_the_text},
    {"title_first_h1_wins", title_first_h1_wins},
    {"title_falls_back_to_title_element_then_prose",
     title_falls_back_to_title_element_then_prose},
    {"title_falls_back_to_filename_stem", title_falls_back_to_filename_stem},
    {"tokenize_content_words_with_offsets", tokenize_content_words_with_offsets},
    {"tokenize_emits_duplicates_per_occurrence",
     tokenize_emits_duplicates_per_occurrence},
    {"tokenize_drops_stopwords", tokenize_drops_stopwords},
    {"tokenize_drops_overlong_terms", tokenize_drops_overlong_terms},
    {"tokenize_drops_one_character_tokens", tokenize_drops_one_character_tokens},
    {"tokenize_folds_diacritics", tokenize_folds_diacritics},
    {"tokenize_empty_string", tokenize_empty_string},
    {"tokenize_rejects_null_arguments", tokenize_rejects_null_arguments},
    {"tokenize_enforces_token_count_cap", tokenize_enforces_token_count_cap},
    {"tokens_init_zeroes_the_vector", tokens_init_zeroes_the_vector},
    {"tokenize_appends_into_an_existing_vector",
     tokenize_appends_into_an_existing_vector},
    {"slugify_basic", slugify_basic},
    {"slugify_trims_and_collapses_separators",
     slugify_trims_and_collapses_separators},
    {"slugify_all_punctuation_is_empty", slugify_all_punctuation_is_empty},
    {NULL, NULL},
};

int main(void) { return kbc_test_run("parse", cases); }
