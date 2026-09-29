/* test_html.c — src/html.c, and the two surfaces that serve what it produces.
 *
 * WHAT THIS FILE IS FOR. The sanitiser's claim is a security claim, and a
 * security claim with no tests is a liability: it reads as tested because it
 * exists. So the cases below are chosen to FAIL LOUDLY, not to document the
 * implementation. Each one pins a property that a reader of the output can
 * see, and each one has a plausible wrong implementation it catches:
 *
 *   - the ALLOWLIST's sharp edges, one case each;
 *   - the CHARACTER-REFERENCE layer, which is where a lookup bug hides: a
 *     reference that fails to decode is re-escaped on output, which looks
 *     like a cosmetic `&amp;lt;` and is in fact a scheme check that never
 *     ran on `javascript&colon;alert(1)`;
 *   - THE EXPOSURE TEST, end to end, over a real socket, on BOTH origins. The
 *     subdomain is the one that matters: ADR-009 gives it `frame-ancestors`
 *     and NO `sandbox`, so a script in a document served there executes. A
 *     sanitiser tested only through its own API cannot tell whether the
 *     serve path actually calls it, and that is precisely the question;
 *   - the REDACTION, including the direction that is easy to get backwards:
 *     an UNKNOWN peer must redact, not skip. A test that only covers the
 *     loopback case passes on an implementation that never redacts at all.
 */
#include <arpa/inet.h>
#include <netinet/in.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include "kbc/app.h"
#include "kbc/config.h"
#include "kbc/html.h"
#include "kbc/httpd.h"
#include "kbc/mem.h"
#include "kbc_test.h"

#define CORPUS "kb"

/* ------------------------------------------------------------ unit level -- */

/* Sanitises `in` and returns a malloc'd NUL-terminated copy the caller frees.
 * The copy exists because the cases below read better as string
 * comparisons, and it is taken from a kbc_str that has been copied into a
 * buffer sized for the terminator: kbc_str reserves exactly `len` bytes, so
 * writing a NUL past them is a heap overflow that surfaces several cases
 * later as an unrelated failure. */
static char *sanitize(const char *in, size_t *len_out) {
  kbc_str tmp;
  kbc_str_init(&tmp);
  kbc_err err;
  kbc_err_reset(&err);
  kbc_status st = kbc_html_sanitize(in, strlen(in), &tmp, &err);
  KBC_CHECK_OK(st);
  if (kbc_failed(st)) {
    kbc_str_free(&tmp);
    return NULL;
  }
  char *copy = (char *)malloc(tmp.len + 1);
  if (copy == NULL) {
    kbc_str_free(&tmp);
    return NULL;
  }
  if (tmp.len > 0) memcpy(copy, tmp.ptr, tmp.len);
  copy[tmp.len] = '\0';
  if (len_out != NULL) *len_out = tmp.len;
  kbc_str_free(&tmp);
  return copy;
}

#define SANITIZED(in) sanitize((in), NULL)

/* ---- the allowlist's sharp edges ---- */

KBC_TEST(class_and_style_are_stripped_from_every_element) {
  char *out = SANITIZED("<p class=\"x\" style=\"color:red\" id=\"keep\">t</p>");
  KBC_CHECK_NOT_NULL(out);
  if (out == NULL) return;
  KBC_CHECK_EQ_STR(out, "<p id=\"keep\">t</p>");
  free(out);
}

KBC_TEST(relative_href_survives_and_script_schemes_do_not) {
  /* The positive and the negative in one case, because a check that drops
   * EVERY href passes the negative and a check that keeps every href passes
   * the positive. */
  static const struct {
    const char *in;
    const char *want;
  } kCases[] = {
      {"<a href=\"/rel/path\">r</a>", "<a href=\"/rel/path\" rel=\"noopener noreferrer\">r</a>"},
      {"<a href=\"sub/page.html#f\">r</a>", "<a href=\"sub/page.html#f\" rel=\"noopener noreferrer\">r</a>"},
      {"<a href=\"https://e.com/x\">r</a>", "<a href=\"https://e.com/x\" rel=\"noopener noreferrer\">r</a>"},
      {"<a href=\"mailto:a@b.c\">r</a>", "<a href=\"mailto:a@b.c\" rel=\"noopener noreferrer\">r</a>"},
      {"<a href=\"javascript:alert(1)\">r</a>", "<a rel=\"noopener noreferrer\">r</a>"},
      {"<a href=\"JaVaScRiPt:alert(1)\">r</a>", "<a rel=\"noopener noreferrer\">r</a>"},
      {"<a href=\"vbscript:msgbox\">r</a>", "<a rel=\"noopener noreferrer\">r</a>"},
      {"<a href=\"data:text/html,<script>\">r</a>", "<a rel=\"noopener noreferrer\">r</a>"},
      /* Leading whitespace and an embedded tab are both stripped by the URL
       * parser before it reads a scheme, so both are the SAME bypass. */
      {"<a href=\"  javascript:alert(1)\">r</a>", "<a rel=\"noopener noreferrer\">r</a>"},
      {"<a href=\"java&Tab;script:alert(1)\">r</a>", "<a rel=\"noopener noreferrer\">r</a>"},
      /* The obfuscations that a naive scheme check misses, and the reason the
       * character-reference layer has to decode BEFORE the scheme is read. */
      {"<a href=\"javascript&colon;alert(1)\">r</a>", "<a rel=\"noopener noreferrer\">r</a>"},
      {"<a href=\"&#106;avascript:alert(1)\">r</a>", "<a rel=\"noopener noreferrer\">r</a>"},
      {"<a href=\"&#x6a;avascript:alert(1)\">r</a>", "<a rel=\"noopener noreferrer\">r</a>"},
      {"<a href=\"java&#09;script:alert(1)\">r</a>", "<a rel=\"noopener noreferrer\">r</a>"},
      {"<a href=\"java&NewLine;script:alert(1)\">r</a>", "<a rel=\"noopener noreferrer\">r</a>"},
      /* An image source is the second URL attribute the allowlist reaches. */
      {"<img src=\"/a.png\" alt=\"a\">", "<img src=\"/a.png\" alt=\"a\">"},
      {"<img src=\"data:image/svg+xml,<svg>\" alt=\"a\">", "<img alt=\"a\">"},
  };
  for (size_t i = 0; i < sizeof kCases / sizeof kCases[0]; i++) {
    char *out = sanitize(kCases[i].in, NULL);
    KBC_CHECK_MSG(out != NULL && strcmp(out, kCases[i].want) == 0,
                  "case %zu (%s): got \"%s\", want \"%s\"", i, kCases[i].in,
                  out ? out : "(null)", kCases[i].want);
    free(out);
  }
}

KBC_TEST(a_disallowed_element_is_unwrapped_and_keeps_its_text) {
  /* `div` is not on the allowlist, and the text inside it is the document's
   * prose, not markup: unwrapping must keep it. An implementation that drops
   * the subtree instead loses content silently, which is the failure mode
   * that a security-only test suite never catches. */
  char *out = SANITIZED("<section>keep <b>me</b></section>");
  KBC_CHECK_EQ_STR(out, "keep <b>me</b>");
  free(out);
}

KBC_TEST(a_comment_is_removed) {
  /* Not escaped, not kept: gone. A conditional comment is the shape that
   * matters, because some browsers still evaluate its contents. */
  char *out = SANITIZED("<p>a</p><!-- secret --><p>b</p>");
  KBC_CHECK_EQ_STR(out, "<p>a</p><p>b</p>");
  free(out);
  out = SANITIZED("<!--[if IE]><script>alert(1)</script><![endif]--><p>x</p>");
  KBC_CHECK_MSG(out != NULL && strstr(out, "script") == NULL,
                "a conditional comment survived: %s", out ? out : "(null)");
  KBC_CHECK_MSG(out != NULL && strstr(out, "alert") == NULL,
                "a conditional comment's body survived: %s",
                out ? out : "(null)");
  free(out);
}

KBC_TEST(a_surviving_link_gains_the_link_relationship) {
  char *out = SANITIZED("<a href=\"/x\">r</a>");
  KBC_CHECK_NOT_NULL(out);
  if (out == NULL) return;
  KBC_CHECK_MSG(strstr(out, "rel=\"noopener noreferrer\"") != NULL,
                "no link relationship: %s", out);
  free(out);
  /* A document's own `rel` is not on the allowlist, so the emitted value is
   * exactly the daemon's string and never a merge with attacker input. */
  out = SANITIZED("<a href=\"/x\" rel=\"opener\">r</a>");
  KBC_CHECK_EQ_STR(out, "<a href=\"/x\" rel=\"noopener noreferrer\">r</a>");
  free(out);
}

KBC_TEST(an_unterminated_tag_is_unwrapped_not_passed_through) {
  /* EOF inside a tag: the spec drops the tag and emits nothing. What must NOT
   * happen is the tag's bytes reaching the output, which is how a filter that
   * gives up on malformed input hands the rest of the document a live tag. */
  char *out = SANITIZED("<p>a</p<div class=\"x");
  KBC_CHECK_EQ_STR(out, "<p>a</p>");
  KBC_CHECK_MSG(out != NULL && strstr(out, "class") == NULL,
                "an unterminated tag reached the output: %s", out ? out : "");
  free(out);
  out = SANITIZED("<img src=\"x\" alt=\"unterminated");
  KBC_CHECK_EQ_STR(out, "");
  /* A tag left open at EOF contributes nothing at all, not even the element
   * it named: an implementation that emits the element and drops the rest
   * still hands a caller an unterminated construct's sibling structure. */
  free(out);
  out = SANITIZED("<p>keep</p><a href=\"/x");
  KBC_CHECK_EQ_STR(out, "<p>keep</p>");
  free(out);
}

KBC_TEST(script_and_event_handlers_never_survive) {
  static const char *const kIn[] = {
      "<script>alert(document.cookie)</script>",
      "<SCRIPT>alert(1)</SCRIPT>",
      "<img src=x onerror=alert(1)>",
      "<img src=x OnErRoR=alert(1)>",
      "<body onload=alert(1)>",
      "<div onclick=alert(1) onmouseover=alert(2)>x</div>",
      "<iframe src=\"javascript:alert(1)\"></iframe>",
      "<style>body{background:url(javascript:alert(1))}</style>",
      "<svg onload=alert(1)><circle/></svg>",
      "<math><mi>x</mi></math>",
      "<form action=\"javascript:alert(1)\"><input name=a></form>",
      "<object data=\"javascript:alert(1)\"></object>",
      "<embed src=\"data:text/html,<script>\">",
      "<base href=\"javascript:alert(1)\">",
      "<a href=\"javascript:alert(1)\" onclick=\"alert(2)\">x</a>",
      "<video src=\"x\" onerror=alert(1)></video>",
      "<template id=\"t\"><script>alert(1)</script></template>",
  };
  for (size_t i = 0; i < sizeof kIn / sizeof kIn[0]; i++) {
    char *out = sanitize(kIn[i], NULL);
    KBC_CHECK_MSG(out != NULL, "case %zu did not sanitise", i);
    if (out == NULL) continue;
    KBC_CHECK_MSG(strstr(out, "<script") == NULL, "case %zu kept a script: %s", i,
                  out);
    KBC_CHECK_MSG(strstr(out, "onerror") == NULL, "case %zu kept onerror: %s", i,
                  out);
    KBC_CHECK_MSG(strstr(out, "onload") == NULL, "case %zu kept onload: %s", i,
                  out);
    KBC_CHECK_MSG(strstr(out, "onclick") == NULL, "case %zu kept onclick: %s", i,
                  out);
    KBC_CHECK_MSG(strstr(out, "javascript:") == NULL,
                  "case %zu kept a javascript: URL: %s", i, out);
    KBC_CHECK_MSG(strstr(out, "vbscript:") == NULL, "case %zu kept vbscript: %s",
                  i, out);
    KBC_CHECK_MSG(strstr(out, "data:") == NULL,
                  "case %zu kept a data: URL: %s", i, out);
    KBC_CHECK_MSG(strstr(out, "<!--") == NULL, "case %zu kept a comment: %s", i,
                  out);
    free(out);
  }
}

/* ---- the character-reference layer, which is where the live bug was ---- */

KBC_TEST(named_and_numeric_references_decode) {
  /* A reference that does not resolve is re-escaped on the way out, so the
   * failure is a visible `&amp;lt;` rather than a silent wrong byte. These
   * cases are the gate: `&Tab;` and `&colon;` are the two obfuscations that
   * slip past a scheme check, so a table that silently fails to resolve is a
   * scheme check that never ran. */
  static const struct {
    const char *in;
    const char *want;
  } kCases[] = {
      {"&lt;", "&lt;"},
      {"&gt;", "&gt;"},
      {"&amp;", "&amp;"},
      {"&quot;", "\""},
      {"&apos;", "'"},
      {"&Tab;", "\t"},
      {"&colon;", ":"},
      {"&NewLine;", "\n"},
      {"&excl;", "!"},
      {"&semi;", ";"},
      {"&DiacriticalGrave;", "`"},
      {"&VerticalLine;", "|"},
      {"&UnderBar;", "_"},
      {"&#60;", "&lt;"},
      {"&#x3c;", "&lt;"},
      {"&#X3C;", "&lt;"},
      {"&#0000060;", "&lt;"},
      /* The legacy no-semicolon forms, in TEXT. */
      {"&lt", "&lt;"},
      {"&amp", "&amp;"},
      /* An unknown reference is left ALONE and re-escaped on output, which is
       * the browser's rendering too. */
      {"&nosuchentity;", "&amp;nosuchentity;"},
      {"a & b", "a &amp; b"},
  };
  for (size_t i = 0; i < sizeof kCases / sizeof kCases[0]; i++) {
    char *out = sanitize(kCases[i].in, NULL);
    KBC_CHECK_MSG(out != NULL && strcmp(out, kCases[i].want) == 0,
                  "case %zu (%s): got \"%s\", want \"%s\"", i, kCases[i].in,
                  out ? out : "(null)", kCases[i].want);
    free(out);
  }
}

KBC_TEST(a_reference_in_an_attribute_is_decoded_before_the_scheme_is_read) {
  /* The security case, stated as its own test because the general reference
   * test cannot see it: the DECODED value is what the scheme check must
   * inspect. An implementation that checks the raw bytes sees no scheme at
   * all, calls the URL relative, and passes it through. */
  static const char *const kIn[] = {
      "<a href=\"javascript&colon;alert(1)\">x</a>",
      "<a href=\"&#106;avascript:alert(1)\">x</a>",
      "<a href=\"java&Tab;script:alert(1)\">x</a>",
      "<img src=\"vbscript&colon;msgbox\" alt=\"a\">",
  };
  for (size_t i = 0; i < sizeof kIn / sizeof kIn[0]; i++) {
    char *out = sanitize(kIn[i], NULL);
    KBC_CHECK_MSG(out != NULL, "case %zu did not sanitise", i);
    if (out == NULL) continue;
    KBC_CHECK_MSG(strstr(out, "javascript") == NULL,
                  "case %zu let a decoded javascript: through: %s", i, out);
    KBC_CHECK_MSG(strstr(out, "vbscript") == NULL,
                  "case %zu let a decoded vbscript: through: %s", i, out);
    KBC_CHECK_MSG(strstr(out, "href=\"") == NULL &&
                      strstr(out, "src=\"") == NULL,
                  "case %zu kept a URL attribute at all: %s", i, out);
    free(out);
  }
}

KBC_TEST(an_entity_in_a_query_string_is_not_decoded_in_an_attribute) {
  /* The mirror rule, and it is a real one: inside an attribute a reference
   * that is not followed by `;` is left alone when the next byte is
   * alphanumeric or `=`, because `?a=1&copy=2` is ONE parameter named
   * `copy`. An implementation that decodes it turns a relative link into a
   * different relative link, which is silent corruption. */
  char *out = SANITIZED("<a href=\"?a=1&amp=2\">q</a>");
  KBC_CHECK_NOT_NULL(out);
  if (out != NULL) {
    KBC_CHECK_MSG(strstr(out, "&amp;amp=2") != NULL,
                  "an in-attribute reference without a semicolon was decoded: %s",
                  out);
    free(out);
  }
}

/* ---- structural correctness, because a sanitiser that eats documents is a
 * ---- data-loss bug wearing a security costume ---- */

KBC_TEST(raw_text_elements_keep_their_text_escaped) {
  /* These five have a non-markup content model. Their content is TEXT and
   * must come out escaped, not dropped: `<xmp>a < b</xmp>` losing its content
   * is a correctness bug in the opposite direction from a security one, and
   * an implementation that only ever runs its security cases will ship it. */
  char *out = SANITIZED("<xmp>a < b &amp; c</xmp>");
  KBC_CHECK_EQ_STR(out, "a &lt; b &amp;amp; c");
  free(out);
  out = SANITIZED("<textarea><b>not markup</b></textarea>");
  KBC_CHECK_EQ_STR(out, "&lt;b&gt;not markup&lt;/b&gt;");
  free(out);
  out = SANITIZED("<noscript>ns text</noscript>");
  KBC_CHECK_EQ_STR(out, "ns text");
  free(out);
  /* `title` is a clean-content tag: the element AND its text go. */
  out = SANITIZED("<title>t</title><p>body</p>");
  KBC_CHECK_EQ_STR(out, "<p>body</p>");
  free(out);
}

KBC_TEST(in_table_content_is_foster_parented_out_of_the_table) {
  /* The spec's condition is the CURRENT NODE, not the insertion mode. A mode
   * test is wrong in exactly the case that matters: the mode is still
   * "in table" after a non-table element has been fostered out, so the text
   * inside that element is fostered out of IT as well — the div arrives empty
   * and its prose ends up beside the table. */
  char *out = SANITIZED("<table><div>x</div></table>");
  KBC_CHECK_EQ_STR(out, "<div>x</div><table></table>");
  free(out);
  out = SANITIZED("<table>oops</table>");
  KBC_CHECK_EQ_STR(out, "oops<table></table>");
  free(out);
  out = SANITIZED("<table><tr><td>cell</td></tr></table>");
  KBC_CHECK_NOT_NULL(out);
  if (out != NULL) {
    KBC_CHECK_MSG(strstr(out, "cell") != NULL, "a table cell lost its text: %s",
                  out);
    free(out);
  }
}

KBC_TEST(sibling_elements_are_siblings_and_not_nested) {
  /* Every element here is on the allowlist, so a document that comes out
   * NESTED is not a filtering decision at all — it is a broken tree, and it
   * is invisible in any assertion that only looks for a forbidden string.
   * `<a>` is the sharp one: closing an open anchor is a rule of its own. */
  char *out = SANITIZED("<a href=\"/a\">1</a><a href=\"/b\">2</a>");
  KBC_CHECK_EQ_STR(out,
                   "<a href=\"/a\" rel=\"noopener noreferrer\">1</a>"
                   "<a href=\"/b\" rel=\"noopener noreferrer\">2</a>");
  free(out);
  out = SANITIZED("<ul><li>a</li><li>b</li></ul><p>after</p>");
  KBC_CHECK_EQ_STR(out, "<ul><li>a</li><li>b</li></ul><p>after</p>");
  free(out);
  out = SANITIZED("<p>one</p><p>two</p><p>three</p>");
  KBC_CHECK_EQ_STR(out, "<p>one</p><p>two</p><p>three</p>");
  free(out);
}

KBC_TEST(unwrapping_one_element_keeps_its_neighbours) {
  /* The list surgery: an unwrapped element is hoisted out of its parent, and
   * both the sibling before it and the sibling after it must survive. A
   * removal that fails to re-link the tail loses everything after it — a
   * silent, total data loss that a security assertion cannot see. */
  char *out = SANITIZED("<p>before</p><section><p>middle</p></section><p>after</p>");
  KBC_CHECK_EQ_STR(out, "<p>before</p><p>middle</p><p>after</p>");
  free(out);
  out = SANITIZED("<div>a</div><marquee>b</marquee><div>c</div>");
  KBC_CHECK_EQ_STR(out, "<div>a</div>b<div>c</div>");
  free(out);
}

KBC_TEST(every_attribute_on_a_multi_attribute_tag_survives_filtering) {
  /* The attribute name is committed on the transition INTO "before attribute
   * name". An implementation that commits it on the wrong transition drops
   * every attribute whose value is unquoted and followed by another
   * attribute — and `src` is exactly the attribute whose loss is a security
   * decision made on nothing. */
  char *out = SANITIZED("<img a=1 b=2 src=\"/x\" alt=\"a\">");
  KBC_CHECK_EQ_STR(out, "<img src=\"/x\" alt=\"a\">");
  free(out);
  out = SANITIZED("<img src=\"/x\"/>");
  KBC_CHECK_EQ_STR(out, "<img src=\"/x\">");
  free(out);
}

/* ------------------------------------------------------------- redaction -- */

KBC_TEST(the_prompt_template_is_never_served) {
  /* The prompt template is the daemon's own construction, not a document. It
   * is selected on `<template id="kb-prompt">`, which is why the strip has to
   * run BEFORE the sanitiser: the allowlist unwraps `template` and would
   * destroy the very attribute the selector matches on. */
  static const char kDoc[] =
      "<p>prose</p><template id=\"kb-prompt\">SYSTEM: do things</template>"
      "<p>more</p>";
  kbc_str out;
  kbc_str_init(&out);
  kbc_err err;
  kbc_err_reset(&err);
  KBC_CHECK_OK(kbc_html_strip_kb_prompt(kDoc, strlen(kDoc), &out, &err));
  KBC_CHECK_MSG(strstr(out.ptr, "kb-prompt") == NULL,
                "the prompt template was served: %s", out.ptr);
  KBC_CHECK_MSG(strstr(out.ptr, "SYSTEM") == NULL,
                "the prompt template's contents were served: %s", out.ptr);
  KBC_CHECK_MSG(strstr(out.ptr, "prose") != NULL && strstr(out.ptr, "more") != NULL,
                "the strip took the document with it: %s", out.ptr);
  kbc_str_free(&out);
  /* An id that merely CONTAINS the name is somebody else's template. */
  static const char kOther[] = "<template id=\"not-kb-prompt\">keep</template>";
  kbc_str_init(&out);
  KBC_CHECK_OK(kbc_html_strip_kb_prompt(kOther, strlen(kOther), &out, &err));
  KBC_CHECK_MSG(strstr(out.ptr, "keep") != NULL,
                "an unrelated template was stripped: %s", out.ptr);
  kbc_str_free(&out);
}

KBC_TEST(an_unknown_peer_redacts_rather_than_skipping) {
  /* The direction that is easy to get backwards, and the one the header
   * documents: the function FAILS CLOSED. An absent, empty or unknown peer
   * means nobody could establish the origin, and the answer is "remote", so
   * the redaction applies. An implementation that returns FALSE for an
   * unknown peer passes every test that only exercises loopback, and skips
   * redaction for exactly the requests nobody can account for. */
  KBC_CHECK(kbc_html_looks_non_loopback(NULL, NULL, NULL, 0));
  KBC_CHECK(kbc_html_looks_non_loopback("", NULL, NULL, 0));
  KBC_CHECK(kbc_html_looks_non_loopback("?", NULL, NULL, 0));
  /* A peer that is not a trusted hop is a real, remote client. */
  KBC_CHECK(kbc_html_looks_non_loopback("203.0.113.9", NULL, NULL, 0));

  /* The one case that is NOT remote: a trusted proxy with no forwarded
   * header, which is the operator's own curl. This is here so the
   * fail-closed case above cannot be passed by a function that always says
   * true. */
  static const char *const kTrusted[] = {"127.0.0.1"};
  KBC_CHECK(!kbc_html_looks_non_loopback("127.0.0.1", NULL, kTrusted, 1));
  /* A trusted proxy WITH a forwarded chain: the rightmost untrusted hop is
   * the client, and it is remote. */
  KBC_CHECK(kbc_html_looks_non_loopback("127.0.0.1", "203.0.113.9", kTrusted, 1));
  KBC_CHECK(!kbc_html_looks_non_loopback("127.0.0.1", "127.0.0.1", kTrusted, 1));
  /* A chain that is entirely trusted hops is a loopback client. */
  KBC_CHECK(!kbc_html_looks_non_loopback("127.0.0.1", "127.0.0.1, 127.0.0.1",
                                         kTrusted, 1));
}

KBC_TEST(the_outbound_pass_redacts_with_an_unknown_peer) {
  /* The end of the chain, asserted where it is used: not "is the function
   * reachable" but "does an unaccountable request still get redacted". */
  static const char kDoc[] = "<p>keep</p><template id=\"kb-prompt\">X</template>";
  kbc_str out;
  kbc_str_init(&out);
  kbc_err err;
  kbc_err_reset(&err);
  KBC_CHECK_OK(kbc_html_scrub_outbound(NULL, NULL, NULL, 0, kDoc, strlen(kDoc),
                                       &out, &err));
  KBC_CHECK_MSG(strstr(out.ptr, "kb-prompt") == NULL,
                "an unknown peer skipped the redaction: %s", out.ptr);
  KBC_CHECK_MSG(strstr(out.ptr, "keep") != NULL,
                "the redaction took the document with it: %s", out.ptr);
  kbc_str_free(&out);
}

/* ------------------------------------------------------- end-to-end, over
 * ------------------------------------------------------- a REAL SOCKET */

typedef struct {
  char root[KBC_TEST_PATH_MAX];
  kbc_config *cfg;
  kbc_app *app;
  kbc_httpd *h;
  int port;
  char id[KBC_MAX_ID_LEN + 1];
} e2e;

static kbc_config *e2e_cfg(const char *root) {
  kbc_config *cfg = kbc_config_defaults();
  if (cfg == NULL) return NULL;
  char buf[KBC_TEST_PATH_MAX + 64];
  free(cfg->data_dir);
  free(cfg->db_path);
  free(cfg->index_path);
  free(cfg->token_path);
  snprintf(buf, sizeof buf, "%s/data", root);
  cfg->data_dir = strdup(buf);
  snprintf(buf, sizeof buf, "%s/data/kb.db", root);
  cfg->db_path = strdup(buf);
  snprintf(buf, sizeof buf, "%s/data/index", root);
  cfg->index_path = strdup(buf);
  snprintf(buf, sizeof buf, "%s/data/token", root);
  cfg->token_path = strdup(buf);
  cfg->corpora = (kbc_corpus_cfg *)calloc(1, sizeof *cfg->corpora);
  if (cfg->corpora == NULL) {
    kbc_config_free(cfg);
    return NULL;
  }
  cfg->ncorpora = 1;
  snprintf(buf, sizeof buf, "%s/kb", root);
  cfg->corpora[0].name = strdup(CORPUS);
  cfg->corpora[0].path = strdup(buf);
  kbc_strlist_init(&cfg->corpora[0].ignore);
  return cfg;
}

/* The corpus. It is written to DISK, not injected: the exposure test is
 * about what the corpus does to the serve path, and a file that arrived by
 * `git checkout` is the case the 415 on the capture route never covered. */
static const char kAttack[] =
    "<html><head><title>doc</title></head><body>\n"
    "<h1>Quarterly</h1>\n"
    "<script>alert(document.cookie)</script>\n"
    "<img src=x onerror=alert(1)>\n"
    "<a href=\"javascript:alert(1)\">click</a>\n"
    "<a href=\"javascript&colon;alert(2)\">obfuscated</a>\n"
    "<!-- a comment that must not reach a reader -->\n"
    "<div class=\"x\" style=\"color:red\">body prose</div>\n"
    "<svg onload=alert(3)><circle/></svg>\n"
    "<iframe src=\"javascript:alert(4)\"></iframe>\n"
    "</body></html>\n";

static void e2e_setup(e2e *f) {
  memset(f, 0, sizeof *f);
  kbc_test_tmpdir(f->root, sizeof f->root);
  char path[KBC_TEST_PATH_MAX + 64];
  snprintf(path, sizeof path, "%s/kb", f->root);
  kbc_test_mkdir_p(path);
  snprintf(path, sizeof path, "%s/kb/attack.html", f->root);
  kbc_test_write_file(path, kAttack);

  f->cfg = e2e_cfg(f->root);
  KBC_CHECK_NOT_NULL(f->cfg);
  if (f->cfg == NULL) return;
  kbc_err err;
  kbc_err_reset(&err);
  f->app = kbc_app_open(f->cfg, &err);
  if (f->app == NULL) fprintf(stderr, "  app_open: %s\n", err.msg);
  KBC_CHECK_NOT_NULL(f->app);
  if (f->app == NULL) return;
  kbc_err_reset(&err);
  KBC_CHECK_OK(kbc_app_reindex(f->app, &err));

  /* The id, from the store rather than by scraping a JSON listing, so the
   * test is asking the same question a client would. */
  for (int kind = 0; kind < KBC_KIND__COUNT && f->id[0] == '\0'; kind++) {
    kbc_arena *a = kbc_arena_new(4096);
    kbc_artifact *arts = NULL;
    size_t n = 0;
    kbc_err_reset(&err);
    if (kbc_app_list_artifacts(f->app, a, CORPUS, (kbc_kind)kind, 100, 0, &arts,
                               &n, &err) == KBC_OK) {
      for (size_t i = 0; i < n; i++) {
        if (arts[i].path != NULL &&
            strstr(arts[i].path, "attack.html") != NULL) {
          snprintf(f->id, sizeof f->id, "%s", arts[i].id);
          break;
        }
      }
    }
    kbc_arena_free(a);
  }
  KBC_CHECK_MSG(f->id[0] != '\0', "the HTML artifact was not indexed");

  free(f->cfg->bind_addr);
  f->cfg->bind_addr = strdup("127.0.0.1");
  f->cfg->port = 0; /* the kernel picks, so the suite never clashes */
  f->cfg->http_workers = 1;
  kbc_err_reset(&err);
  f->h = kbc_httpd_start(f->app, f->cfg, &err);
  if (f->h == NULL) fprintf(stderr, "  httpd_start: %s\n", err.msg);
  KBC_CHECK_NOT_NULL(f->h);
  f->port = f->h != NULL ? kbc_httpd_port(f->h) : 0;
  KBC_CHECK_MSG(f->port > 0, "no listening port");
}

static void e2e_teardown(e2e *f) {
  if (f->h != NULL) kbc_httpd_stop(f->h);
  if (f->app != NULL) kbc_app_close(f->app);
  kbc_config_free(f->cfg);
  kbc_test_rmrf(f->root);
}

/* One raw request on a real socket, so the routing, the Host split and the
 * serve path all run exactly as they do for a client. */
static int e2e_get(e2e *f, const char *host, const char *path, char *reply,
                   size_t cap) {
  reply[0] = '\0';
  int fd = socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) return 0;
  struct sockaddr_in a;
  memset(&a, 0, sizeof a);
  a.sin_family = AF_INET;
  a.sin_port = htons((uint16_t)f->port);
  a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  if (connect(fd, (struct sockaddr *)&a, sizeof a) != 0) {
    close(fd);
    return 0;
  }
  kbc_str req;
  kbc_str_init(&req);
  (void)kbc_str_printf(&req, "GET %s HTTP/1.1\r\nHost: %s\r\nConnection: "
                              "close\r\n\r\n",
                       path, host);
  size_t off = 0;
  while (off < req.len) {
    ssize_t w = send(fd, req.ptr + off, req.len - off, MSG_NOSIGNAL);
    if (w <= 0) break;
    off += (size_t)w;
  }
  kbc_str_free(&req);
  shutdown(fd, SHUT_WR);
  struct timeval tv;
  tv.tv_sec = 5;
  tv.tv_usec = 0;
  (void)setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
  size_t got = 0;
  for (;;) {
    if (got + 1 >= cap) break;
    ssize_t r = recv(fd, reply + got, cap - 1 - got, 0);
    if (r <= 0) break;
    got += (size_t)r;
    reply[got] = '\0';
  }
  close(fd);
  if (strncmp(reply, "HTTP/1.1 ", 9) != 0) return 0;
  return atoi(reply + 9);
}

/* The exposure assertion, stated once and applied to BOTH origins, because
 * the point of the test is that it does not depend on which route served the
 * bytes. */
static void assert_not_exposed(const char *label, const char *reply,
                               int status) {
  KBC_CHECK_MSG(status == 200, "%s: expected 200, got %d: %.200s", label,
                status, reply);
  KBC_CHECK_MSG(strstr(reply, "<script") == NULL,
                "%s served a script tag: %.400s", label, reply);
  KBC_CHECK_MSG(strstr(reply, "onerror=") == NULL,
                "%s served an event handler: %.400s", label, reply);
  KBC_CHECK_MSG(strstr(reply, "onload=") == NULL,
                "%s served an event handler: %.400s", label, reply);
  KBC_CHECK_MSG(strstr(reply, "javascript:") == NULL,
                "%s served a javascript: URL: %.400s", label, reply);
  KBC_CHECK_MSG(strstr(reply, "vbscript:") == NULL,
                "%s served a vbscript: URL: %.400s", label, reply);
  KBC_CHECK_MSG(strstr(reply, "<!--") == NULL,
                "%s served a comment: %.400s", label, reply);
  KBC_CHECK_MSG(strstr(reply, "alert(") == NULL,
                "%s served an alert payload: %.400s", label, reply);
  KBC_CHECK_MSG(strstr(reply, "<iframe") == NULL,
                "%s served an iframe: %.400s", label, reply);
  /* And the document is still a document: a filter that answers every attack
   * by serving nothing passes every assertion above. */
  KBC_CHECK_MSG(strstr(reply, "body prose") != NULL,
                "%s dropped the document's own prose: %.400s", label, reply);
}

KBC_TEST(no_origin_serves_the_corpus_script) {
  e2e f;
  e2e_setup(&f);
  if (f.h == NULL || f.id[0] == '\0') {
    e2e_teardown(&f);
    return;
  }
  char reply[16384];
  char host[KBC_TEST_PATH_MAX + 64];
  char path[512];

  /* The SUBDOMAIN, which is the surface that matters: ADR-009 gives it
   * `frame-ancestors` and NO `sandbox`, so a script in a document served
   * here EXECUTES. The parent origin sends `sandbox`, which is containment
   * and not a filter, and a containment measure is not a property of the
   * bytes. */
  snprintf(host, sizeof host, "%s.artifacts.localhost", f.id);
  snprintf(path, sizeof path, "/attack.html");
  int status = e2e_get(&f, host, path, reply, sizeof reply);
  assert_not_exposed("artifact subdomain", reply, status);

  /* And the parent origin, which serves the same artifact under a different
   * path. A test on one origin proves nothing about the other. */
  snprintf(path, sizeof path, "/api/kb/%s/artifact/%s", CORPUS, f.id);
  status = e2e_get(&f, "kb.localhost", path, reply, sizeof reply);
  assert_not_exposed("parent origin", reply, status);

  e2e_teardown(&f);
}

/* DELIBERATELY ABSENT: an end-to-end "no origin serves the prompt template"
 * case.
 *
 * It cannot be written, and saying so is more useful than writing a version
 * that passes. The outbound pass gates the redaction on the client being
 * non-loopback, and a test client on 127.0.0.1 IS loopback — the operator at
 * their own machine — so a socket test always takes the passthrough branch and
 * would be asserting that the template IS served. Reaching the redaction needs
 * a non-loopback peer, which needs a second host, which the test harness has
 * no way to present.
 *
 * The property is therefore pinned at the unit level instead, where the peer
 * is an argument rather than a fact about the test machine:
 * `the_prompt_template_is_never_served` for the strip and
 * `the_outbound_pass_redacts_with_an_unknown_peer` for the gate. What is NOT
 * covered by anything is the ORDER inside httpd's outbound_html — that the
 * strip runs before the sanitiser, because the sanitiser unwraps `template` and
 * would destroy the `id` the strip matches on. A corpus document carrying a
 * template, requested by a non-loopback client, is the test for it.
 *
 */KBC_TEST(the_subdomain_response_carries_no_sandbox_so_the_bytes_are_filtered) {
  /* The precondition of the case above, asserted so the test cannot keep
   * passing after someone adds a `sandbox` and relaxes the filter: on the
   * subdomain there is no sandbox, so the filter is the only thing standing
   * between the corpus and execution. */
  e2e f;
  e2e_setup(&f);
  if (f.h == NULL || f.id[0] == '\0') {
    e2e_teardown(&f);
    return;
  }
  char reply[16384];
  char host[KBC_TEST_PATH_MAX + 64];
  snprintf(host, sizeof host, "%s.artifacts.localhost", f.id);
  int status = e2e_get(&f, host, "/attack.html", reply, sizeof reply);
  KBC_CHECK_EQ_INT(status, 200);
  KBC_CHECK_MSG(strcasestr(reply, "sandbox") == NULL,
                "the subdomain now sends a sandbox; this test is asserting the "
                "wrong thing: %.400s",
                reply);
  e2e_teardown(&f);
}

int main(void) {
  return kbc_test_run("html", (kbc_test_case[]){
                          {"class_and_style_are_stripped", class_and_style_are_stripped_from_every_element},
                          {"relative_href_survives", relative_href_survives_and_script_schemes_do_not},
                          {"unwrapped_keeps_text", a_disallowed_element_is_unwrapped_and_keeps_its_text},
                          {"comment_removed", a_comment_is_removed},
                          {"link_relationship", a_surviving_link_gains_the_link_relationship},
                          {"unterminated_tag", an_unterminated_tag_is_unwrapped_not_passed_through},
                          {"scripts_never_survive", script_and_event_handlers_never_survive},
                          {"references_decode", named_and_numeric_references_decode},
                          {"reference_in_attribute", a_reference_in_an_attribute_is_decoded_before_the_scheme_is_read},
                          {"entity_in_query", an_entity_in_a_query_string_is_not_decoded_in_an_attribute},
                          {"raw_text_kept", raw_text_elements_keep_their_text_escaped},
                          {"foster_parenting", in_table_content_is_foster_parented_out_of_the_table},
                          {"siblings_are_siblings", sibling_elements_are_siblings_and_not_nested},
                          {"unwrapping_keeps_neighbours", unwrapping_one_element_keeps_its_neighbours},
                          {"multi_attribute_tag", every_attribute_on_a_multi_attribute_tag_survives_filtering},
                          {"prompt_template", the_prompt_template_is_never_served},
                          {"unknown_peer_redacts", an_unknown_peer_redacts_rather_than_skipping},
                          {"outbound_pass", the_outbound_pass_redacts_with_an_unknown_peer},
                          {"no_origin_serves_script", no_origin_serves_the_corpus_script},
                          {"subdomain_has_no_sandbox", the_subdomain_response_carries_no_sandbox_so_the_bytes_are_filtered},
                          {NULL, NULL}});
}
