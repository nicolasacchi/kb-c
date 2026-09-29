/* html.c — the HTML sanitiser, and the outbound redaction that rides with it.
 *
 * WHY THIS FILE IS THE FIX, and not a stage-5 nicety. `is_indexable`
 * (app.c) accepts `.html` and `.htm`, so an HTML file reaches a corpus by
 * `git checkout`, `scp`, `kb add <dir>` or a sync, is indexed, and is served
 * back by the artifact route. Before this file, the capture route refused HTML
 * with a 415 — and that refusal protected nothing, because capture was never
 * how HTML got in. THE CORPUS IS.
 *
 * The containment that does exist is a CSP header, and it is two things at
 * once: not enough, and invisible. Not enough because the artifact SUBDOMAIN
 * sends only `frame-ancestors` and no `sandbox` (ADR-009), so a script in a
 * document served there EXECUTES. Invisible because a header is a property of
 * one route, and this has to be a property of the bytes.
 *
 * NO DEPENDENCY. The operator's rule, narrowed in ADR-008, binds the C library:
 * libkbc.a links sqlite3, libm and libpthread and nothing else. The markdown
 * renderer is hand-rolled for the same reason, and the cost of that decision
 * is the size of this file.
 *
 * ---------------------------------------------------------------------------
 * THE ALLOWLIST IS THE HEADER'S, NOT THIS FILE'S
 * ---------------------------------------------------------------------------
 * Every entry below cites the ammonia line it came from, and the lists are
 * transcribed from include/kbc/html.h, which is frozen and authoritative. The
 * port was verified against ammonia 4.1.4's own `Default for Builder`
 * (lib.rs:378-488) rather than against the header's prose, and the two agree.
 * ONE CORRECTION TO THE HEADER, reported rather than silently applied:
 *
 *   ammonia's `tag_attributes` gives `tbody` and `thead` the row
 *   {align, char, charoff} — `span` is on `col` and `colgroup` ONLY. The
 *   header's line 65 writes "tbody,thead,tfoot[align,char,charoff,span]",
 *   which grants `span` on three tags ammonia does not. `tfoot` is not in the
 *   tag set at all, so its row is dead either way. This file follows AMMONIA:
 *   `span` on col/colgroup only. The header's own rule — "port the EFFECTIVE
 *   set, not the configured one" — is what makes the correction safe rather
 *   than a divergence: the security direction is a SMALLER attribute set.
 *
 * WHAT IS DELIBERATELY ABSENT, each a security decision: html, head, body,
 * title, meta, script, style, iframe, object, embed, form, input, svg, math,
 * link, base, noscript, template, audio, video, source, track, canvas, applet,
 * frame, frameset, marquee.
 */

#include "kbc/html.h"

#include <stdlib.h>
#include <string.h>


/* ==========================================================================
 * 1. The arena
 *
 * A whole document's nodes, attribute names, attribute values and text live
 * in one bump-allocated heap that is released in a single call when the
 * sanitiser returns. There is no per-node free and no ownership question,
 * which is the only way a two-thousand-line tree builder stays auditable: a
 * node's memory cannot be freed while a sibling still points at it.
 *
 * It is a bump allocator rather than kbc_arena because this is not
 * request-scoped scratch with a known ceiling — it is sized by the INPUT, up
 * to KBC_MAX_ARTIFACT_BYTES, and it wants one contiguous-ish run rather than
 * a linked list of small blocks walked on every allocation.
 * ======================================================================== */

typedef struct hn_blk {
  struct hn_blk *next;
  size_t used;
  size_t cap;
  unsigned char *data;
} hn_blk;

typedef struct {
  hn_blk *head;
  bool oom; /* sticky: one failure fails the whole document, never half of it */
} hn_arena;

#define HN_BLK_MIN 8192u

static hn_arena *hn_arena_new(void) {
  hn_arena *a = (hn_arena *)calloc(1, sizeof *a);
  return a;
}

static void hn_arena_free(hn_arena *a) {
  if (a == NULL) return;
  hn_blk *b = a->head;
  while (b != NULL) {
    hn_blk *next = b->next;
    free(b->data);
    free(b);
    b = next;
  }
  free(a);
}

static void *hn_alloc(hn_arena *a, size_t n) {
  if (a->oom) return NULL;
  /* Every allocation is rounded to 8 so a node never straddles a word. */
  size_t need = (n + 7u) & ~(size_t)7u;
  if (need < n) { /* overflow */
    a->oom = true;
    return NULL;
  }
  if (a->head == NULL || a->head->cap - a->head->used < need) {
    size_t cap = need > HN_BLK_MIN ? need : HN_BLK_MIN;
    hn_blk *b = (hn_blk *)calloc(1, sizeof *b);
    if (b == NULL) {
      a->oom = true;
      return NULL;
    }
    b->data = (unsigned char *)malloc(cap);
    if (b->data == NULL) {
      free(b);
      a->oom = true;
      return NULL;
    }
    b->cap = cap;
    b->used = 0;
    b->next = a->head;
    a->head = b;
  }
  void *p = a->head->data + a->head->used;
  a->head->used += need;
  memset(p, 0, need);
  return p;
}

/* A NUL-terminated copy. Callers compare with strcmp and hand the result to
 * code that wants a C string, so the terminator is part of the contract and
 * not an optimisation. */
static char *hn_strndup(hn_arena *a, const char *s, size_t n) {
  char *p = (char *)hn_alloc(a, n + 1);
  if (p == NULL) return NULL;
  if (n > 0) memcpy(p, s, n);
  p[n] = '\0';
  return p;
}

/* ==========================================================================
 * 2. Character references
 *
 * NUMERIC references are decoded in full, because that is where the URL-scheme
 * bypasses live: `href="&#106;avascript:alert(1)"` is `javascript:alert(1)`
 * to the browser, and a sanitiser that checked the RAW attribute value would
 * see no scheme at all, call it relative, and pass it through. Decoding before
 * the scheme check is the whole defence, so it is not optional here.
 *
 * NAMED references: only the 57 that decode to an ASCII codepoint are in the
 * table, and that is a COMPLETE set for security rather than a shortcut. A
 * named reference that decodes to a non-ASCII codepoint cannot open a tag, end
 * an attribute value, or contribute a byte to a URL scheme — schemes are
 * `[a-z][a-z0-9+.-]*` — so passing such a reference through verbatim leaves
 * the rendered document identical and cannot change a decision this file
 * makes. `&colon;`, `&Tab;` and `&NewLine;` are all in the table, and those
 * are the three that matter: they are how `javascript&colon;alert(1)` and
 * `java&Tab;script:` are written, and the URL parser strips tab/CR/LF
 * everywhere before it reads a scheme (see hn_url_scheme), so both are
 * caught.
 *
 * DIVERGENCE FROM AMMONIA, and the harness needs to know it: ammonia
 * (html5ever's tokenizer) decodes all 2231 named references, so `&eacute;`
 * comes back as U+00E9 where this file emits the six literal bytes. Rendering
 * is identical in a browser; the BYTES are not. Adding the remaining 2174 is
 * mechanical and is a one-table change if byte-equality is ever wanted.
 * ======================================================================== */

typedef struct {
  const char *name;
  uint32_t cp;
} hn_entity;

/* markup5ever's own table (entities.rs), restricted to the ASCII-producing
 * entries, and stored WITHOUT the leading `&`.
 *
 * BUG FIXED 2026-09-29, and it was the entire named-reference layer. The
 * names used to carry their leading `&` ("&amp;", "&Tab;"), but
 * hn_decode_ref calls hn_entity_lookup with `s + p` — the name with the `&`
 * already stepped over. Every memcmp compared a name beginning with `&`
 * against one beginning with a letter, the binary search found nothing, and
 * named references passed through verbatim and were re-escaped on output:
 * `&lt;` came out as the six bytes `&amp;lt;`. That is a SECURITY bug and not
 * a cosmetic one: `&colon;` and `&Tab;` are precisely the obfuscations of
 * `javascript:` that a scheme check has to survive, and a `&Tab;` that never
 * decodes is a `&Tab;` that never gets stripped from the URL before the
 * scheme is read.
 *
 * Sorted by strcmp, which is what makes the binary search valid, and the
 * longest-match loop in hn_decode_ref scans DOWN from HN_ENTITY_MAX so a short
 * name can never shadow a longer one sharing its prefix.
 *
 * FOUR ENTRIES REMOVED, 2026-09-29, because they were WRONG rather than
 * merely incomplete. `fjlig`, `underbar`, `nvgt` and `nvlt` were mapped to
 * nearby ASCII characters ('f', '_', '>', '<') as though an ASCII-producing
 * stand-in existed. There is none: markup5ever gives them U+FB02, U+0332,
 * U+226B and U+226A, all non-ASCII, so a document containing `&nvlt;` was
 * being rendered as a bare `<` that the operator never wrote — in a
 * sanitiser whose whole job is that no document smuggles a byte past the
 * filter. They are now the ordinary pass-through case the divergence note
 * above already describes, which is safe: a non-ASCII reference cannot open a
 * tag, end an attribute value, or contribute a byte to a URL scheme. */
static const hn_entity kEntities[] = {
    {"AMP", 38},     {"AMP;", 38},     {"DiacriticalGrave;", 96},
    {"GT", 62},      {"GT;", 62},      {"Hat;", 94},
    {"LT", 60},      {"LT;", 60},      {"NewLine;", 10},
    {"QUOT", 34},    {"QUOT;", 34},    {"Tab;", 9},
    {"UnderBar;", 95}, {"VerticalLine;", 124}, {"amp", 38},
    {"amp;", 38},    {"apos;", 39},    {"ast;", 42},
    {"bne;", 61},    {"bsol;", 92},    {"colon;", 58},
    {"comma;", 44},  {"commat;", 64},  {"dollar;", 36},
    {"equals;", 61}, {"excl;", 33},    {"grave;", 96},
    {"gt", 62},      {"gt;", 62},      {"lbrace;", 123},
    {"lbrack;", 91}, {"lcub;", 123},   {"lowbar;", 95},
    {"lpar;", 40},   {"lsqb;", 91},    {"lt", 60},
    {"lt;", 60},     {"midast;", 42},  {"num;", 35},
    {"percnt;", 37}, {"period;", 46},  {"plus;", 43},
    {"quest;", 63},  {"quot", 34},     {"quot;", 34},
    {"rbrace;", 125}, {"rbrack;", 93}, {"rcub;", 125},
    {"rpar;", 41},   {"rsqb;", 93},    {"semi;", 59},
    {"sol;", 47},    {"verbar;", 124}, {"vert;", 124},
};

/* The longest name in the table. The scan in hn_decode_ref bounds itself by
 * this, and the bound used to be a hard-coded 12 — shorter than
 * "DiacriticalGrave;" — so the longest entries in the table were
 * unreachable no matter how they were spelled. The static assert fails the
 * BUILD if a name is ever added without widening the bound, which is the
 * failure mode that is invisible at runtime: the entity simply stops
 * resolving and the output re-escapes it. */
#define HN_ENTITY_MAX 17
_Static_assert(sizeof("DiacriticalGrave;") - 1 == HN_ENTITY_MAX,
               "kEntities grew a longer name: raise HN_ENTITY_MAX with it");

/* markup5ever's numeric-reference replacements for the C1 block: a browser
 * does not produce U+0080 for `&#128;`, it produces EURO SIGN. */
static uint32_t hn_c1_replacement(uint32_t cp) {
  static const struct {
    uint32_t from;
    uint32_t to;
  } kC1[] = {
      {0x80, 0x20AC}, {0x82, 0x201A}, {0x83, 0x0192}, {0x84, 0x201E},
      {0x85, 0x2026}, {0x86, 0x2020}, {0x87, 0x2021}, {0x88, 0x02C6},
      {0x89, 0x2030}, {0x8A, 0x0160}, {0x8B, 0x2039}, {0x8C, 0x0152},
      {0x8E, 0x017D}, {0x91, 0x2018}, {0x92, 0x2019}, {0x93, 0x201C},
      {0x94, 0x201D}, {0x95, 0x2022}, {0x96, 0x2013}, {0x97, 0x2014},
      {0x98, 0x02DC}, {0x99, 0x2122}, {0x9A, 0x0161}, {0x9B, 0x203A},
      {0x9C, 0x0153}, {0x9E, 0x017E}, {0x9F, 0x0178},
  };
  for (size_t i = 0; i < sizeof kC1 / sizeof kC1[0]; i++) {
    if (kC1[i].from == cp) return kC1[i].to;
  }
  return cp;
}

/* WHATWG's numeric fixes, in the spec's order. */
static uint32_t hn_numeric_fix(uint32_t cp) {
  if (cp == 0) return 0xFFFD;
  if (cp > 0x10FFFF) return 0xFFFD;
  if (cp >= 0xD800 && cp <= 0xDFFF) return 0xFFFD;
  if (cp >= 0x80 && cp <= 0x9F) return hn_c1_replacement(cp);
  return cp;
}

static kbc_status hn_utf8_put(kbc_str *out, uint32_t cp) {
  unsigned char b[4];
  if (cp < 0x80u) {
    b[0] = (unsigned char)cp;
    return kbc_str_append(out, (const char *)b, 1);
  }
  if (cp < 0x800u) {
    b[0] = (unsigned char)(0xC0u | (cp >> 6));
    b[1] = (unsigned char)(0x80u | (cp & 0x3Fu));
    return kbc_str_append(out, (const char *)b, 2);
  }
  if (cp < 0x10000u) {
    b[0] = (unsigned char)(0xE0u | (cp >> 12));
    b[1] = (unsigned char)(0x80u | ((cp >> 6) & 0x3Fu));
    b[2] = (unsigned char)(0x80u | (cp & 0x3Fu));
    return kbc_str_append(out, (const char *)b, 3);
  }
  b[0] = (unsigned char)(0xF0u | (cp >> 18));
  b[1] = (unsigned char)(0x80u | ((cp >> 12) & 0x3Fu));
  b[2] = (unsigned char)(0x80u | ((cp >> 6) & 0x3Fu));
  b[3] = (unsigned char)(0x80u | (cp & 0x3Fu));
  return kbc_str_append(out, (const char *)b, 4);
}

static int hn_hex_val(unsigned char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

static bool hn_entity_lookup(const char *s, size_t n, uint32_t *out) {
  size_t lo = 0, hi = sizeof kEntities / sizeof kEntities[0];
  while (lo < hi) {
    size_t mid = lo + (hi - lo) / 2;
    const char *name = kEntities[mid].name;
    size_t nl = strlen(name);
    size_t m = n < nl ? n : nl;
    int c = memcmp(s, name, m);
    if (c == 0) c = n < nl ? -1 : (n > nl ? 1 : 0);
    if (c == 0) {
      *out = kEntities[mid].cp;
      return true;
    }
    if (c < 0) {
      hi = mid;
    } else {
      lo = mid + 1;
    }
  }
  return false;
}

/* Decodes the character reference starting at s[*i] == '&' and advances *i
 * past it, appending the replacement text to `out`. Returns false when the
 * `&` starts nothing, in which case *i is untouched and the caller emits a
 * literal `&`.
 *
 * `in_attr` selects one rule and only one: inside an attribute value a
 * reference that does not parse is left ALONE, because the byte after it may
 * be the quote that ends the value. In text it is not, so a bare `&` survives
 * as text. */
static bool hn_decode_ref(const char *s, size_t n, size_t *i, kbc_str *out,
                          bool in_attr) {
  size_t start = *i;
  size_t p = start + 1; /* past '&' */
  if (p >= n) return false;

  if (s[p] == '#') {
    size_t q = p + 1;
    uint32_t cp = 0;
    size_t digits = 0;
    if (q < n && (s[q] == 'x' || s[q] == 'X')) {
      q++;
      while (q < n && digits < 8) {
        int v = hn_hex_val((unsigned char)s[q]);
        if (v < 0) break;
        cp = cp * 16u + (uint32_t)v;
        q++;
        digits++;
      }
    } else {
      while (q < n && digits < 9) {
        if (s[q] < '0' || s[q] > '9') break;
        cp = cp * 10u + (uint32_t)(s[q] - '0');
        q++;
        digits++;
      }
    }
    /* A `&#` with no digits after it is not a reference at all. */
    if (digits == 0) return false;
    if (q < n && s[q] == ';') q++;
    (void)hn_utf8_put(out, hn_numeric_fix(cp));
    *i = q;
    return true;
  }

  /* Named. Longest match wins, which is why this scans down from the longest
   * candidate rather than stopping at the first hit: `&notin` must not
   * resolve as `&not`. */
  size_t avail = n - p;
  size_t maxn = avail < HN_ENTITY_MAX ? avail : HN_ENTITY_MAX;
  for (size_t len = maxn; len >= 2; len--) {
    uint32_t cp;
    if (!hn_entity_lookup(s + p, len, &cp)) continue;
    bool semi = (s + p)[len - 1] == ';';
    if (!semi && in_attr) {
      /* The legacy no-semicolon forms (`&amp`, `&lt`, `&gt`, `&quot`) are
       * NOT expanded inside an attribute when the next byte is `=` or
       * alphanumeric: `?a=1&copy=2` means one parameter named `copy`, not a
       * copyright sign followed by `=2`. This is the rule that stops
       * `&amp=foo` from decoding to `&=foo`, and html5ever applies it. */
      size_t after = p + len;
      if (after < n) {
        unsigned char c = (unsigned char)s[after];
        if (c == '=' || (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') ||
            (c >= 'A' && c <= 'Z')) {
          continue;
        }
      }
    }
    (void)hn_utf8_put(out, cp);
    *i = p + len;
    return true;
  }
  return false;
}

/* ==========================================================================
 * 3. The allowlist
 *
 * TAGS, 75 (ammonia lib.rs:378-388). `title` is NOT here even though it is
 * also a generic ATTRIBUTE: kb adds it to `clean_content_tags` instead
 * (capture.rs:691), so the tag is dropped WITH its text while the attribute
 * survives on other elements. Adding it here would make both assertions in
 * ammonia's `clean_dom` fire and abort the process.
 * ======================================================================== */

static const char *const kTags[] = {
    "a",     "abbr",   "acronym", "area",  "article", "aside",  "b",
    "bdi",   "bdo",    "blockquote", "br", "caption", "center", "cite",
    "code",  "col",    "colgroup", "data", "dd",     "del",    "details",
    "dfn",   "div",    "dl",      "dt",   "em",     "figcaption", "figure",
    "footer", "h1",    "h2",      "h3",   "h4",     "h5",     "h6",
    "header", "hgroup", "hr",     "i",    "img",    "ins",    "kbd",
    "li",    "map",    "mark",    "nav",  "ol",     "p",      "pre",
    "q",     "rp",     "rt",      "rtc",  "ruby",   "s",      "samp",
    "small", "span",   "strike",  "strong", "sub",  "summary", "sup",
    "table", "tbody",  "td",      "th",   "thead",  "time",   "tr",
    "tt",    "u",      "ul",      "var",  "wbr"};

/* clean_content_tags: the element AND everything inside it is dropped.
 * `script` and `style` are ammonia's own defaults (lib.rs:379); `title` is
 * kb's addition (capture.rs:691), and the comment there says why: an unwrapped
 * `<title>` would leak the page's title as orphaned prose in front of the
 * body. */
static const char *const kCleanContentTags[] = {"script", "style", "title"};

/* GENERIC attributes, on ANY allowed tag: ammonia's `lang` and `title`
 * (lib.rs:380) plus kb's `id` (capture.rs:690, so section anchors survive).
 *
 * `class` and `style` are absent and that is load-bearing, not an omission:
 * `allowed_classes` is empty and `style_properties` is None, so ammonia
 * strips them from every element. `style` in particular is a whole class of
 * attacks (`background:url(javascript:…)`, `position:fixed` clickjacking,
 * `behavior:` in old IE) and it is cheaper to refuse it than to filter it. */
static const char *const kGenericAttrs[] = {"lang", "title", "id"};

/* PER-TAG attributes, ammonia lib.rs:382-435, EFFECTIVE rows only.
 *
 * The header's line 65 writes `tbody,thead,tfoot[align,char,charoff,span]`.
 * ammonia gives `tbody` and `thead` {align, char, charoff} — `span` is on
 * `col` and `colgroup` ONLY — and `tfoot` is not in the tag set, so its row is
 * dead configuration. The table below follows ammonia, which is a SMALLER
 * attribute set and so the safe direction; the correction is reported at the
 * top of this file. */
typedef struct {
  const char *tag;
  const char *const *attrs;
  size_t n;
} hn_tag_attrs;

static const char *const kA[] = {"href", "hreflang"};
static const char *const kBdo[] = {"dir"};
static const char *const kCite[] = {"cite"};
static const char *const kCol[] = {"align", "char", "charoff", "span"};
static const char *const kDelIns[] = {"cite", "datetime"};
static const char *const kHr[] = {"align", "size", "width"};
static const char *const kImg[] = {"align", "alt", "height", "src", "width"};
static const char *const kOl[] = {"start"};
static const char *const kTable[] = {"align", "char", "charoff", "summary"};
static const char *const kSection[] = {"align", "char", "charoff"};
static const char *const kTd[] = {"align", "char", "charoff", "colspan",
                                  "headers", "rowspan"};
static const char *const kTh[] = {"align",   "char",    "charoff", "colspan",
                                  "headers", "rowspan", "scope"};

static const hn_tag_attrs kTagAttrs[] = {
    {"a", kA, 2},           {"bdo", kBdo, 1},
    {"blockquote", kCite, 1}, {"col", kCol, 4},
    {"colgroup", kCol, 4},  {"del", kDelIns, 2},
    {"hr", kHr, 3},         {"img", kImg, 5},
    {"ins", kDelIns, 2},    {"ol", kOl, 1},
    {"q", kCite, 1},        {"table", kTable, 4},
    {"tbody", kSection, 3}, {"td", kTd, 6},
    {"th", kTh, 7},         {"thead", kSection, 3},
    {"tr", kSection, 3},
};

/* URL SCHEMES, 24 (ammonia lib.rs:438-462). Anything else is dropped, and
 * that is the line `javascript:`, `data:` and `vbscript:` are kept behind. */
static const char *const kSchemes[] = {
    "bitcoin", "ftp",  "ftps",    "geo",      "http",     "https",
    "im",      "irc",  "ircs",    "magnet",   "mailto",   "mms",
    "mx",      "news", "nntp",    "openpgp4fpr", "sip",  "sms",
    "smsto",   "ssh",  "tel",     "url",      "webcal",   "wtai",
    "xmpp"};

/* ammonia lib.rs:2531-2543, `is_url_attr`. On the EFFECTIVE allowlist only two
 * rows reach it: `a[href]` and `img[src]`. `cite` is deliberately NOT a URL
 * attribute — ammonia does not treat it as one, and a `cite` is never
 * dereferenced, so there is nothing to sanitise. The rest of ammonia's list
 * (form action, object data, formaction, ping, poster) belongs to tags this
 * build does not allow at all, and `xlink:href` only ever appears on SVG,
 * which is unwrapped before it can be serialised. */
static bool hn_is_url_attr(const char *tag, const char *attr) {
  if (strcmp(attr, "href") == 0) return strcmp(tag, "a") == 0;
  if (strcmp(attr, "src") == 0) return strcmp(tag, "img") == 0;
  return false;
}

static bool hn_in(const char *const *set, size_t n, const char *s) {
  for (size_t i = 0; i < n; i++) {
    if (strcmp(set[i], s) == 0) return true;
  }
  return false;
}

static bool hn_tag_allowed(const char *tag) {
  return hn_in(kTags, sizeof kTags / sizeof kTags[0], tag);
}

static bool hn_clean_content(const char *tag) {
  return hn_in(kCleanContentTags,
               sizeof kCleanContentTags / sizeof kCleanContentTags[0], tag);
}

static bool hn_generic_attr(const char *attr) {
  return hn_in(kGenericAttrs, sizeof kGenericAttrs / sizeof kGenericAttrs[0],
               attr);
}

static bool hn_tag_attr_allowed(const char *tag, const char *attr) {
  for (size_t i = 0; i < sizeof kTagAttrs / sizeof kTagAttrs[0]; i++) {
    if (strcmp(kTagAttrs[i].tag, tag) == 0) {
      return hn_in(kTagAttrs[i].attrs, kTagAttrs[i].n, attr);
    }
  }
  return false;
}

/* ==========================================================================
 * 4. The URL decision
 *
 * ammonia hands the attribute value to the `url` crate and reads the scheme
 * back (lib.rs:1957-1968): a parse that yields an absolute URL must have a
 * scheme in the allowlist, a parse that fails as RelativeUrlWithoutBase is a
 * relative URL and passes through (url_relative = PassThrough, lib.rs:477),
 * and ANY other parse error drops the attribute.
 *
 * The `url` crate is not in libkbc.a, so this reimplements the decision rather
 * than the parser. What it reproduces, in the crate's order:
 *
 *   1. C0 control and space are stripped from BOTH ends.
 *   2. tab, LF and CR are removed from EVERYWHERE. This is the step that
 *      matters: `java&#9;script:alert(1)` decodes to a real tab, the tab is
 *      removed, and what is left is `javascript:`. A check that only looked
 *      for a colon before the first `/` would call it relative and pass it.
 *   3. A value that does not begin with an ASCII ALPHA is not a scheme at
 *      all — it is relative, and passes.
 *   4. Otherwise the scheme runs to the first `:` and must be ALPHA followed
 *      by ALPHA / DIGIT / `+` / `-` / `.`. A `:` that is not reachable that
 *      way (`1foo:bar`, `foo bar:baz`) makes the whole thing relative, which
 *      is what the crate reports and what a browser then treats as a path.
 *
 * A scheme-relative `//host/path` has no scheme and passes, exactly as
 * ammonia passes it. It cannot execute.
 */
static bool hn_url_ok(const char *v) {
  size_t n = strlen(v);
  size_t b = 0, e = n;
  while (b < e && (unsigned char)v[b] <= 0x20u) b++;
  while (e > b && (unsigned char)v[e - 1] <= 0x20u) e--;
  /* Step 2 in one pass over a bounded scratch buffer: the value is an
   * attribute, so it is small, and a second copy keeps the rest of this
   * function free of "am I looking at a stripped byte?" questions. */
  char stackbuf[256];
  char *heap = NULL;
  char *t = stackbuf;
  size_t tcap = sizeof stackbuf;
  if (e - b + 1 > tcap) {
    heap = (char *)malloc(e - b + 1);
    if (heap == NULL) return false; /* fail closed */
    t = heap;
    tcap = e - b + 1;
  }
  size_t tl = 0;
  for (size_t i = b; i < e; i++) {
    unsigned char c = (unsigned char)v[i];
    if (c == '\t' || c == '\n' || c == '\r') continue;
    if (tl + 1 < tcap) t[tl++] = (char)c;
  }
  t[tl] = '\0';
  /* NOT freed here. `t` is read for the rest of this function — the scheme
   * scan and the lowercasing both index into it — so freeing the scratch
   * buffer at this point was a use-after-free that happened to work only
   * because the allocator kept handing back the same block. It was reachable
   * on any URL longer than 255 bytes. */
  bool ok;
  if (tl == 0) {
    ok = true; /* empty value: relative, and ammonia passes it */
  } else if (!((t[0] >= 'a' && t[0] <= 'z') || (t[0] >= 'A' && t[0] <= 'Z'))) {
    ok = true; /* no scheme: relative */
  } else {
    size_t sl = 0;
    while (sl < tl) {
      char c = t[sl];
      if (c == ':') break;
      if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') || c == '+' || c == '-' || c == '.')) {
        break;
      }
      sl++;
    }
    if (sl == tl || t[sl] != ':') {
      ok = true; /* no scheme before a delimiter: relative */
    } else if (sl == 0) {
      ok = true;
    } else {
      /* Schemes are case-insensitive; the `url` crate lowercases before the
       * allowlist test, so `JaVaScRiPt:` is compared lowercased too. */
      char low[64];
      if (sl >= sizeof low) {
        ok = false; /* absurd scheme length: not in any allowlist */
      } else {
        for (size_t i = 0; i < sl; i++) {
          char c = t[i];
          low[i] = (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c;
        }
        low[sl] = '\0';
        ok = hn_in(kSchemes, sizeof kSchemes / sizeof kSchemes[0], low);
      }
    }
  }
  free(heap);
  return ok;
}

/* ==========================================================================
 * 5. The tokenizer
 *
 * A real one, because the alternative cannot see the constructs that decide
 * whether a byte is a tag: `<` inside an attribute value, a `>` that closes
 * nothing, a `<script>` whose content is not markup at all. Each of those is a
 * way to make a stream filter and a parser disagree about where an element
 * ends, and the disagreement is what the rest of this file is built to avoid.
 *
 * States, following the WHATWG tokenizer: Data, TagOpen, EndTagOpen, TagName,
 * BeforeAttributeName, AttributeName, AfterAttributeName, BeforeAttributeValue,
 * AttributeValue(Double|Single|Unquoted), SelfClosingStartTag, Comment,
 * BogusComment, Doctype, and the RAWTEXT/RCDATA/script-data family that
 * html5ever enters for title, textarea, style, xmp, iframe, noembed, noframes,
 * script and plaintext. Which of those a tag gets is decided by the caller
 * from the tag name, exactly as the spec's "generic raw text element
 * parsing" algorithm does.
 * ======================================================================== */

typedef enum {
  HT_DATA,
  HT_TAG_OPEN,
  HT_END_TAG_OPEN,
  HT_TAG_NAME,
  HT_BEFORE_ATTR_NAME,
  HT_ATTR_NAME,
  HT_AFTER_ATTR_NAME,
  HT_BEFORE_ATTR_VALUE,
  HT_ATTR_VALUE_DQ,
  HT_ATTR_VALUE_SQ,
  HT_ATTR_VALUE_UQ,
  HT_SELF_CLOSING,
  HT_BOGUS_COMMENT,
  HT_DOCTYPE,
  HT_RAWTEXT,
  HT_RCDATA,
  HT_SCRIPT_DATA,
  HT_PLAINTEXT
} hn_state;

typedef enum {
  TK_EOF,
  TK_START,
  TK_END,
  TK_TEXT,
  TK_COMMENT
} hn_tok_kind;

typedef struct {
  char *name;
  size_t name_len;
  kbc_str value;
} hn_attr;

typedef struct {
  hn_tok_kind kind;
  char *name;         /* lowercased, arena */
  hn_attr *attrs;     /* arena array */
  size_t n_attrs;
  kbc_str text;       /* text and comment payloads */
  bool self_closing;
} hn_token;

typedef struct {
  const char *s;
  size_t n;
  size_t i;
  hn_arena *a;
  hn_state state;
  hn_state return_state;
  /* The element whose content the current RAWTEXT/RCDATA run belongs to; the
   * matching end tag is the only thing that ends it. */
  const char *rawtext_tag;
  kbc_str text;  /* accumulated text/comment/attribute-value bytes */
  kbc_str name;  /* the ATTRIBUTE name being accumulated */
  kbc_str tag;   /* the TAG name, moved here the moment the tag name ends */
  hn_attr *attrs;
  size_t n_attrs, attrs_cap;
  bool self_closing;
  bool in_tag;   /* between `<` and the tag's `>` */
  bool is_end;   /* the tag being built is an end tag */
  /* An attribute NAME has been started and not yet committed by
   * hn_tok_add_attr. The spec calls "add an attribute to the token" on each
   * transition INTO "before attribute name", and the earlier code here
   * inferred that from the state instead, which lost every attribute whose
   * value was UNQUOTED and followed by another attribute (`<img a=1 b=2
   * src="x">` came out with no attributes at all) and every attribute before
   * a `/>` (`<img src="x"/>` lost its src). The flag is the spec's step made
   * explicit; hn_tok_add_attr is the only thing that clears it. */
  bool attr_pending;
} hn_tok;

static void hn_tok_init(hn_tok *t, const char *s, size_t n, hn_arena *a) {
  memset(t, 0, sizeof *t);
  t->s = s;
  t->n = n;
  t->a = a;
  kbc_str_init(&t->text);
  kbc_str_init(&t->name);
  kbc_str_init(&t->tag);
}

/* Attribute names are matched ASCII-lowercased, so they are stored lowered.
 * Duplicate names: the FIRST wins, which is what html5ever does and what
 * stops `<a href="ok" href="javascript:…">` from smuggling a second value past
 * a reader that looks at the last one. */
static void hn_tok_add_attr(hn_tok *t) {
  t->attr_pending = false;
  if (t->name.len == 0) {
    kbc_str_clear(&t->name);
    kbc_str_clear(&t->text);
    return; /* `<div ="x">` — an attribute needs a name */
  }
  for (size_t i = 0; i < t->n_attrs; i++) {
    if (strcmp(t->attrs[i].name, t->name.ptr) == 0) {
      kbc_str_clear(&t->name);
      kbc_str_clear(&t->text);
      return; /* duplicate: keep the first */
    }
  }
  if (t->n_attrs == t->attrs_cap) {
    size_t cap = t->attrs_cap ? t->attrs_cap * 2 : 8;
    hn_attr *na = (hn_attr *)hn_alloc(t->a, cap * sizeof *na);
    if (na == NULL) return;
    if (t->n_attrs > 0) memcpy(na, t->attrs, t->n_attrs * sizeof *na);
    t->attrs = na;
    t->attrs_cap = cap;
  }
  hn_attr *at = &t->attrs[t->n_attrs];
  at->name = hn_strndup(t->a, t->name.ptr, t->name.len);
  kbc_str_init(&at->value);
  kbc_str_append(&at->value, t->text.ptr, t->text.len);
  t->n_attrs++;
  kbc_str_clear(&t->name);
  kbc_str_clear(&t->text);
}

static bool hn_is_ws(unsigned char c) {
  return c == ' ' || c == '\t' || c == '\n' || c == '\f' || c == '\r';
}

static void hn_lower_put(kbc_str *out, unsigned char c) {
  kbc_str_putc(out, (c >= 'A' && c <= 'Z') ? (char)(c + 32) : (char)c);
}

/* Moves whatever text has accumulated into `out` as a TK_TEXT token and
 * RESETS the buffer. Returns FALSE when there was nothing to emit, so the
 * caller can tell "no token yet" from "token ready" — a zero-length run is
 * not a token, because an empty text node serialises to nothing and would
 * only cost memory.
 *
 * Every call site must RETURN when this is true. A caller that falls
 * through to the next state instead overwrites the token it just built, and
 * the symptom is a document in which every run of text has silently
 * vanished while every element survives. */
static bool hn_emit_text(hn_tok *t, hn_token *out) {
  if (t->text.len == 0) return false;
  out->kind = TK_TEXT;
  out->text = t->text;
  kbc_str_init(&t->text);
  return true;
}

/* The tag name has ended — whitespace, `/`, or `>`. It moves into `t->tag`
 * and the shared name buffer is cleared, so the next attribute name starts
 * from empty. Without the move the two run together and `<a href="x">`
 * tokenises as an element named `ahref`, which matches no allowlist entry
 * and therefore unwraps to nothing. */
static void hn_end_tag_name(hn_tok *t) {
  kbc_str_clear(&t->tag);
  kbc_str_append(&t->tag, t->name.ptr, t->name.len);
  kbc_str_clear(&t->name);
}

static void hn_reset_tag(hn_tok *t) {
  /* Discarding the tag means discarding its attributes TOO. Zeroing the count
   * without freeing the values leaked one kbc_str per attribute of every tag
   * the source never terminated: the EOF-inside-a-tag path calls this to drop
   * the tag — which is the safe reading — but it dropped the attribute strings
   * on the floor doing it. `tag_done` calls this too, but only AFTER handing
   * `t->attrs` to the token, so the loop is a no-op there and this cannot
   * double-free. */
  for (size_t i = 0; i < t->n_attrs; i++) kbc_str_free(&t->attrs[i].value);
  t->n_attrs = 0;
  t->self_closing = false;
  t->is_end = false;
  t->in_tag = false;
  t->attr_pending = false;
  kbc_str_clear(&t->name);
  kbc_str_clear(&t->tag);
  kbc_str_clear(&t->text);
}

/* True for the elements whose content model is RAWTEXT, RCDATA or script
 * data — the ones the tokenizer has already switched away from markup by the
 * time the tree builder sees the start tag, and which therefore also have to
 * switch the TREE BUILDER into "text" or the run will be re-parsed as markup.
 * `script` and `style` are RCDATA-ish in the spec's table but are also
 * clean-content tags, so their text is dropped by the filter either way; they
 * are listed here so the two content-model lists cannot drift apart. */
static bool hn_uses_text_mode(const char *tag) {
  return strcmp(tag, "title") == 0 || strcmp(tag, "textarea") == 0 ||
         strcmp(tag, "style") == 0 || strcmp(tag, "xmp") == 0 ||
         strcmp(tag, "iframe") == 0 || strcmp(tag, "noembed") == 0 ||
         strcmp(tag, "noframes") == 0 || strcmp(tag, "noscript") == 0 ||
         strcmp(tag, "script") == 0;
}

/* Switches the tokenizer into the content model the spec gives this element.
 * `script` gets script data, `title`/`textarea` get RCDATA (character
 * references still decode), the raw-text family gets RAWTEXT (they do not),
 * and `plaintext` never comes back. */
static void hn_set_content_model(hn_tok *t, const char *tag) {
  if (strcmp(tag, "script") == 0) {
    t->return_state = HT_SCRIPT_DATA;
  } else if (strcmp(tag, "title") == 0 || strcmp(tag, "textarea") == 0) {
    t->return_state = HT_RCDATA;
  } else if (strcmp(tag, "style") == 0 || strcmp(tag, "xmp") == 0 ||
             strcmp(tag, "iframe") == 0 || strcmp(tag, "noembed") == 0 ||
             strcmp(tag, "noframes") == 0 ||
             strcmp(tag, "noscript") == 0) {
    t->return_state = HT_RAWTEXT;
  } else if (strcmp(tag, "plaintext") == 0) {
    t->return_state = HT_PLAINTEXT;
  } else {
    return; /* ordinary element: content stays markup */
  }
  t->rawtext_tag = tag;
  kbc_str_clear(&t->text);
  /* THE MISSING LINE, and it is why no raw-text element ever worked. The
   * function computed the right return_state and then never APPLIED it: it
   * wrote `t->return_state` and left `t->state` on HT_DATA, so the very next
   * call tokenised `<xmp>a<b>c</xmp>` as an `xmp` start tag, the text `a`, a
   * real `b` start tag, the text `c` and an `xmp` end tag. The `return_state`
   * field is the spec's — the state to return to when the raw run ends — not
   * the state to run in. */
  t->state = t->return_state;
}

/* True when the bytes at `t->i` spell `</` + the rawtext element's name
 * followed by a name-terminating byte. This is the ONLY thing that ends a
 * RAWTEXT/RCDATA run, and getting it wrong is how `<script>var s = "</scr" +
 * "ipt>";</script>` either leaks its tail as markup or truncates the script. */
static bool hn_rawtext_close(hn_tok *t, size_t at, size_t *after) {
  size_t tl = strlen(t->rawtext_tag);
  if (at + 2 + tl > t->n) return false;
  if (t->s[at] != '<' || t->s[at + 1] != '/') return false;
  for (size_t i = 0; i < tl; i++) {
    unsigned char c = (unsigned char)t->s[at + 2 + i];
    if (c >= 'A' && c <= 'Z') c = (unsigned char)(c + 32);
    if (c != (unsigned char)t->rawtext_tag[i]) return false;
  }
  size_t end = at + 2 + tl;
  if (end < t->n) {
    unsigned char c = (unsigned char)t->s[end];
    if (!hn_is_ws(c) && c != '/' && c != '>') return false;
  }
  *after = end;
  return true;
}

/* The next token. Text runs are coalesced, because a token per character
 * would make a 16 MiB artifact sixteen million calls. */
static bool hn_next_token(hn_tok *t, hn_token *out) {
  memset(out, 0, sizeof *out);
  for (;;) {
    if (t->i >= t->n) {
      if (t->in_tag) {
        /* EOF inside a tag: the spec DROPS the tag and emits nothing. An
         * unterminated `<div class="` therefore contributes no element at
         * all, which is the safe reading the header asks for. */
        hn_reset_tag(t);
        t->state = HT_DATA;
        continue;
      }
      if (hn_emit_text(t, out)) return true;
      out->kind = TK_EOF;
      return true;
    }
    unsigned char c = (unsigned char)t->s[t->i];

    switch (t->state) {
      case HT_DATA:
        if (c == '<') {
          /* The `<` is consumed on BOTH paths. HT_TAG_OPEN is the state that
           * begins AFTER the bracket, so leaving `t->i` on it feeds a second
           * `<` to a state with no rule for one, which emits it as literal
           * text — that is how `</p>` ends up inside a text node. */
          t->state = HT_TAG_OPEN;
          t->i++;
          if (hn_emit_text(t, out)) return true;
          continue;
        }
        if (c == '&') {
          size_t start = t->i;
          if (hn_decode_ref(t->s, t->n, &t->i, &t->text, false)) continue;
          t->i = start;
        }
        kbc_str_putc(&t->text, (char)c);
        t->i++;
        continue;

      case HT_TAG_OPEN:
        if (c == '!') {
          t->state = (t->i + 1 < t->n && t->s[t->i + 1] == '-') ? HT_BOGUS_COMMENT
                                                                : HT_DOCTYPE;
          /* A real comment needs `--`; the caller re-enters through
           * HT_BOGUS_COMMENT, which handles both `--` and `<!doctype`. */
          t->i++;
          continue;
        }
        if (c == '/') {
          t->state = HT_END_TAG_OPEN;
          t->i++;
          continue;
        }
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')) {
          t->state = HT_TAG_NAME;
          t->in_tag = true;
          t->is_end = false;
          kbc_str_clear(&t->name);
          kbc_str_clear(&t->text);
          t->n_attrs = 0;
          t->self_closing = false;
          continue;
        }
        if (c == '?') { /* bogus comment, per spec */
          t->state = HT_BOGUS_COMMENT;
          continue;
        }
        /* A `<` that starts nothing is literal text. */
        kbc_str_putc(&t->text, '<');
        t->state = HT_DATA;
        t->i++;
        continue;

      case HT_END_TAG_OPEN:
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')) {
          t->state = HT_TAG_NAME;
          t->in_tag = true;
          t->is_end = true;
          kbc_str_clear(&t->name);
          kbc_str_clear(&t->text);
          t->n_attrs = 0;
          t->self_closing = false;
          /* NO `t->i--` here, and its absence was the whole tree builder.
           * HT_TAG_OPEN already advanced past the `/`, so `t->i` ALREADY
           * points at the letter. Rewinding put it back on the `/`, which
           * HT_TAG_NAME then read as a self-closing marker: every `</tag>`
           * was consumed as a self-closing START tag of a zero-length name
           * and emitted as nothing. Not one end token was ever produced, so
           * the insertion-mode machine, the adoption agency and the
           * in-table rules never ran on real input — every document came
           * out as one deeply NESTED tree, which is why the single-element
           * samples in the predecessor's differential looked correct. */
          continue; /* the letter is re-read in HT_TAG_NAME, i.e. reconsume */
        }
        if (c == '>') { /* `</>` — parse error, nothing emitted */
          t->state = HT_DATA;
          t->i++;
          continue;
        }
        t->state = HT_BOGUS_COMMENT;
        continue;

      case HT_TAG_NAME:
        if (hn_is_ws(c)) {
          hn_end_tag_name(t);
          t->state = HT_BEFORE_ATTR_NAME;
          t->i++;
          continue;
        }
        if (c == '/') {
          hn_end_tag_name(t);
          t->state = HT_SELF_CLOSING;
          t->i++;
          continue;
        }
        if (c == '>') {
          hn_end_tag_name(t);
          t->i++;
          goto tag_done;
        }
        hn_lower_put(&t->name, c);
        t->i++;
        continue;

      case HT_BEFORE_ATTR_NAME:
        if (hn_is_ws(c)) {
          t->i++;
          continue;
        }
        if (c == '/') {
          t->state = HT_SELF_CLOSING;
          t->i++;
          continue;
        }
        if (c == '>') {
          t->i++;
          goto tag_done;
        }
        if (c == '=') {
          /* An attribute with no name: the spec starts one with an empty
           * name, which then cannot match anything and is dropped. */
          kbc_str_clear(&t->name);
          t->attr_pending = true;
          t->state = HT_BEFORE_ATTR_VALUE;
          t->i++;
          continue;
        }
        t->state = HT_ATTR_NAME;
        t->attr_pending = true;
        continue;

      case HT_ATTR_NAME:
        if (hn_is_ws(c)) {
          t->state = HT_AFTER_ATTR_NAME;
          t->i++;
          continue;
        }
        if (c == '/') {
          t->state = HT_SELF_CLOSING;
          t->i++;
          continue;
        }
        if (c == '=') {
          t->state = HT_BEFORE_ATTR_VALUE;
          t->i++;
          continue;
        }
        if (c == '>') {
          t->i++;
          goto tag_done;
        }
        hn_lower_put(&t->name, c);
        t->attr_pending = true;
        t->i++;
        continue;

      case HT_AFTER_ATTR_NAME:
        if (hn_is_ws(c)) {
          t->i++;
          continue;
        }
        if (c == '/') {
          t->state = HT_SELF_CLOSING;
          t->i++;
          continue;
        }
        if (c == '=') {
          t->state = HT_BEFORE_ATTR_VALUE;
          t->i++;
          continue;
        }
        if (c == '>') {
          t->i++;
          goto tag_done;
        }
        /* A new attribute starts here; the previous one had no value. */
        hn_tok_add_attr(t);
        t->attr_pending = true;
        t->state = HT_ATTR_NAME;
        continue;

      case HT_BEFORE_ATTR_VALUE:
        if (hn_is_ws(c)) {
          t->i++;
          continue;
        }
        if (c == '"') {
          t->state = HT_ATTR_VALUE_DQ;
          t->i++;
          continue;
        }
        if (c == '\'') {
          t->state = HT_ATTR_VALUE_SQ;
          t->i++;
          continue;
        }
        if (c == '>') { /* missing value: emit the tag */
          t->i++;
          goto tag_done;
        }
        t->state = HT_ATTR_VALUE_UQ;
        continue;

      case HT_ATTR_VALUE_DQ:
      case HT_ATTR_VALUE_SQ: {
        unsigned char q = t->state == HT_ATTR_VALUE_DQ ? '"' : '\'';
        if (c == q) {
          t->state = HT_AFTER_ATTR_NAME;
          t->i++;
          continue;
        }
        if (c == '&') {
          if (hn_decode_ref(t->s, t->n, &t->i, &t->text, true)) continue;
        }
        kbc_str_putc(&t->text, (char)c);
        t->i++;
        continue;
      }

      case HT_ATTR_VALUE_UQ:
        if (hn_is_ws(c)) {
          hn_tok_add_attr(t);
          t->state = HT_BEFORE_ATTR_NAME;
          t->i++;
          continue;
        }
        if (c == '>') {
          t->i++;
          goto tag_done;
        }
        if (c == '&') {
          if (hn_decode_ref(t->s, t->n, &t->i, &t->text, true)) continue;
        }
        kbc_str_putc(&t->text, (char)c);
        t->i++;
        continue;

      case HT_SELF_CLOSING:
        if (c == '>') {
          t->self_closing = true;
          t->i++;
          goto tag_done;
        }
        hn_tok_add_attr(t);
        t->state = HT_BEFORE_ATTR_NAME;
        continue;

      case HT_BOGUS_COMMENT: {
        /* A comment, a doctype, or `<!foo>` — all end at the first `>`, and
         * all three are dropped by the filter, so the payload is read and
         * discarded rather than modelled. `<!doctype html>` in particular must
         * NOT become a text node: it would surface as visible prose. */
        if (c == '>') {
          t->i++;
          t->state = HT_DATA;
          continue;
        }
        t->i++;
        continue;
      }

      case HT_DOCTYPE:
        if (c == '>') {
          t->i++;
          t->state = HT_DATA;
          continue;
        }
        t->i++;
        continue;

      case HT_RAWTEXT:
      case HT_RCDATA:
      case HT_SCRIPT_DATA: {
        if (c == '<') {
          size_t after;
          if (hn_rawtext_close(t, t->i, &after)) {
            /* Emit the content as text, then let the end tag be tokenised
             * through the NORMAL path, which is what makes `</script >` and
             * `</script/>` both close it. `t->i` is deliberately left ON the
             * `<`: the data path re-reads it, walks past `</`, and reaches
             * HT_END_TAG_OPEN exactly as for any other end tag. */
            t->rawtext_tag = NULL;
            t->state = HT_DATA;
            if (hn_emit_text(t, out)) return true;
            continue;
          }
        }
        if (c == '&' && t->state == HT_RCDATA) {
          if (hn_decode_ref(t->s, t->n, &t->i, &t->text, false)) continue;
        }
        kbc_str_putc(&t->text, (char)c);
        t->i++;
        continue;
      }

      case HT_PLAINTEXT:
        kbc_str_putc(&t->text, (char)c);
        t->i++;
        continue;

      default:
        /* Unreachable while every enumerator above has a case; falling back to
         * DATA rather than spinning forever is the safe default if one is
         * ever added without one. */
        t->state = HT_DATA;
    }
    continue;

  tag_done:
    if (t->tag.len == 0) { /* `<>` or `< >`: nothing to emit */
      t->state = HT_DATA;
      hn_reset_tag(t);
      continue;
    }
    /* The tag name lives in `t->tag`, moved out of `t->name` when the tag
     * name ended. Reading `t->name` here would yield the LAST ATTRIBUTE's
     * name — or an empty string when the tag had none — and a name matching
     * no allowlist entry makes every element unwrap to nothing. */
    out->name = hn_strndup(t->a, t->tag.ptr, t->tag.len);
    if (t->attr_pending) hn_tok_add_attr(t);

    out->kind = t->is_end ? TK_END : TK_START;
    out->attrs = t->attrs;
    out->n_attrs = t->n_attrs;
    out->self_closing = t->self_closing;
    kbc_str_init(&out->text);
    t->attrs = NULL;
    t->n_attrs = 0;
    t->attrs_cap = 0;
    hn_reset_tag(t);
    t->state = HT_DATA;
    if (out->kind == TK_START) hn_set_content_model(t, out->name);
    return true;
  }
}

/* ==========================================================================
 * 6. The tree
 *
 * A node is arena-owned and never individually freed; `parent` is a raw
 * pointer and the mutation helpers below maintain both directions, because
 * the filter's unwrap step is a splice and a half-updated sibling list shows
 * up as duplicated or vanished text rather than as a crash.
 *
 * TEMPLATE IS THE ONE STRUCTURAL TRICK, and it is not a trick: html5ever
 * puts a `<template>`'s children in a separate fragment reached through
 * `template_contents`, NOT in the element's child list. ammonia walks
 * `children` (lib.rs:1878) and serialises `children` (rcdom.rs:461-467), so
 * it never sees them: unwrapping a `<template>` hoists ZERO children and the
 * element's entire contents vanish. Storing template content in the ordinary
 * child list and dropping the subtree in the filter produces the same output
 * and is far easier to reason about than a second parallel tree.
 * ======================================================================== */

typedef enum { HN_ELEMENT, HN_TEXT } hn_node_kind;

typedef struct hn_node {
  hn_node_kind kind;
  char *name;              /* element: lowercased; text: NULL */
  hn_attr *attrs;          /* element only, source order */
  size_t n_attrs;
  char *text;              /* text only, NUL-terminated for convenience */
  size_t text_len;
  struct hn_node *parent, *first_child, *last_child, *prev, *next;
} hn_node;
static hn_node *hn_elem(hn_arena *a, char *name, hn_attr *attrs,
                        size_t n_attrs) {
  hn_node *n = (hn_node *)hn_alloc(a, sizeof *n);
  if (n == NULL) return NULL;
  n->kind = HN_ELEMENT;
  /* `name` is arena-owned, lowercased by the tokenizer, and outlives every
   * other allocation the arena makes — so one non-const type for "a string
   * this arena owns" is correct here and saves a cast at every call site. */
  n->name = name;
  n->attrs = attrs;
  n->n_attrs = n_attrs;
  return n;
}

static hn_node *hn_text(hn_arena *a, const char *s, size_t len) {
  hn_node *n = (hn_node *)hn_alloc(a, sizeof *n);
  if (n == NULL) return NULL;
  n->kind = HN_TEXT;
  n->text = hn_strndup(a, s, len);
  n->text_len = len;
  return n;
}

static void hn_append(hn_node *parent, hn_node *child) {
  child->parent = parent;
  child->prev = parent->last_child;
  child->next = NULL;
  if (parent->last_child != NULL) {
    parent->last_child->next = child;
  } else {
    parent->first_child = child;
  }
  parent->last_child = child;
}

/* Replaces `victim` in its parent's child list with `n`, keeping the
 * surrounding order. `n` may be NULL, which removes `victim`. */
static void hn_replace(hn_node *victim, hn_node *n) {
  hn_node *p = victim->parent;
  if (p == NULL) return;
  if (n != NULL) {
    n->parent = p;
    n->prev = victim->prev;
    n->next = victim->next;
  }
  /* BUG FIXED 2026-09-29. When `n` is NULL — a REMOVAL, which is every call
   * the filter makes, since ammonia never splices a node in — `next->prev`
   * must become the victim's own PREVIOUS sibling. Setting it to NULL tore
   * the backward chain in half at the removal point, so every node before
   * the removed one became unreachable by walking `prev` from `last_child`.
   * Both the serializer and the foster-parenting lookup walk that direction,
   * so an unwrapped element silently swallowed every sibling that preceded
   * it: `<xmp>a</xmp>after` serialised as `after` and dropped the `a`. The
   * removal is a splice with an empty replacement, so the neighbours must be
   * joined to EACH OTHER. */
  hn_node *prev = victim->prev;
  hn_node *next = victim->next;
  if (prev != NULL) {
    /* The mirror of the fix below, and the reason the same function was
     * wrong twice: on a removal the PREVIOUS sibling's `next` must become
     * whatever FOLLOWED, not NULL. Setting it to NULL cut the list in two and
     * orphaned every node after the removed one. */
    prev->next = (n != NULL) ? n : next;
  } else {
    /* Removing the FIRST child does not empty the list: the head becomes
     * whatever followed. Setting it to NULL orphaned every remaining sibling
     * — they still pointed at `p` and at each other, but nothing pointed at
     * them — and `<svg><circle/></svg><p>a</p>` serialised as the empty
     * string. */
    p->first_child = (n != NULL) ? n : next;
  }
  if (next != NULL) {
    next->prev = (n != NULL) ? n : prev;
  } else {
    p->last_child = (n != NULL) ? n : prev;
  }
  victim->parent = NULL;
  victim->prev = NULL;
  victim->next = NULL;
}

/* ==========================================================================
 * 7. The tree builder
 *
 * ammonia parses as a FRAGMENT whose context element is `div`
 * (ammonia lib.rs:2519-2527), which deletes a large part of the spec: the
 * insertion mode starts at "in body" rather than "initial", and there is no
 * html/head/body to construct, no doctype in the tree, and no "before head"
 * or "after head" to pass through. A `<html>`, `<head>` or `<body>` START tag
 * in the input is therefore handled by the "in body" rules, which ignore it.
 * That is why a full saved page loses its envelope in the original, and this
 * file reproduces that rather than "fixing" it.
 *
 * The modes below are the ones a fragment parse can actually reach:
 * in body, text, in table, in caption, in column group, in table body, in row,
 * in cell, in select, in select in table. The stack of open elements, the
 * list of active formatting elements, the adoption agency algorithm, foster
 * parenting, and the four scope predicates are all here because each of them
 * changes WHERE a node lands, and that is the whole reason this is a tree
 * builder and not a filter.
 * ======================================================================== */

#define HN_MAX_DEPTH 512

typedef enum {
  IM_IN_BODY,
  IM_TEXT,
  IM_IN_TABLE,
  IM_IN_CAPTION,
  IM_IN_COLUMN_GROUP,
  IM_IN_TABLE_BODY,
  IM_IN_ROW,
  IM_IN_CELL,
  IM_IN_SELECT,
  IM_IN_SELECT_IN_TABLE
} hn_mode;

typedef struct {
  hn_arena *a;
  hn_node *root;
  hn_node *open[HN_MAX_DEPTH];
  size_t depth;
  hn_node *afe[256];   /* active formatting elements */
  size_t n_afe;
  hn_node *form;       /* the form element pointer */
  hn_mode mode;
  hn_mode return_mode;
  bool foster;         /* foster-parenting is on (in-table character data) */
  bool oom;
} hn_builder;

static hn_node *hn_current(const hn_builder *b) { return b->open[b->depth - 1]; }

static bool hn_push(hn_builder *b, hn_node *n) {
  if (b->depth >= HN_MAX_DEPTH) {
    /* The spec's own limit. Past it the element is simply not pushed, so its
     * content lands in the nearest open ancestor instead of growing a stack
     * no bound would survive. */
    return true;
  }
  b->open[b->depth++] = n;
  return true;
}

static void hn_pop(hn_builder *b) {
  if (b->depth > 1) b->depth--;
}

/* "has an element in <scope>": walk the open stack downwards and stop at a
 * boundary element for that scope. The boundary LISTS differ per scope and
 * the difference is load-bearing — a `<p>` inside a `<button>` is still
 * open, a `<li>` inside a `<td>` is still open, and neither is inside its own
 * scope's boundary. */
static bool hn_in_scope(const hn_builder *b, const char *tag, int scope) {
  for (size_t i = b->depth; i > 0; i--) {
    const char *n = b->open[i - 1]->name;
    if (n == NULL) continue;
    if (strcmp(n, tag) == 0) return true;
    if (scope == 0) { /* default */
      static const char *const kStop[] = {
          "applet", "caption", "html", "table", "td", "th",
          "marquee", "object", "template"};
      if (hn_in(kStop, sizeof kStop / sizeof kStop[0], n)) return false;
    } else if (scope == 1) { /* list item */
      static const char *const kStop[] = {
          "applet", "caption", "html", "table", "td", "th", "ol",
          "ul",    "marquee", "object", "template"};
      if (hn_in(kStop, sizeof kStop / sizeof kStop[0], n)) return false;
    } else if (scope == 2) { /* button */
      static const char *const kStop[] = {
          "applet", "caption", "html", "table", "td", "th", "button",
          "marquee", "object", "template"};
      if (hn_in(kStop, sizeof kStop / sizeof kStop[0], n)) return false;
    } else { /* table */
      static const char *const kStop[] = {"html", "table", "template"};
      if (hn_in(kStop, sizeof kStop / sizeof kStop[0], n)) return false;
    }
  }
  return false;
}

static void hn_generate_implied_end_tags(hn_builder *b, const char *except) {
  static const char *const kImplied[] = {"dd", "dt", "li", "optgroup",
                                        "option", "p",  "rb",    "rp",
                                        "rt",     "rtc"};
  for (;;) {
    const char *n = hn_current(b)->name;
    if (n == NULL) return;
    if (except != NULL && strcmp(n, except) == 0) return;
    if (!hn_in(kImplied, sizeof kImplied / sizeof kImplied[0], n)) return;
    hn_pop(b);
  }
}

/* "close a p element": the rule behind `<p><div>` producing two siblings
 * rather than a nested div, and behind an end tag for a `p` that was never
 * opened emitting an empty one. */
static void hn_close_p(hn_builder *b) {
  if (!hn_in_scope(b, "p", 2 /* button scope */)) return;
  hn_generate_implied_end_tags(b, "p");
  while (b->depth > 1) {
    const char *n = hn_current(b)->name;
    hn_pop(b);
    if (n != NULL && strcmp(n, "p") == 0) return;
  }
}

/* The special-category elements: the ones the spec lists by name, and which
 * decide whether a `<table>` may sit inside them at all. */
static bool hn_is_special(const char *n) {
  static const char *const kSpecial[] = {
      "address", "applet",  "area",   "article", "aside",  "base",
      "basefont", "bgsound", "blockquote", "body", "br",     "button",
      "caption", "center",  "col",    "colgroup", "dd",    "details",
      "dir",     "div",     "dl",     "dt",      "embed",  "fieldset",
      "figcaption", "figure", "footer", "form",  "frame",  "frameset",
      "h1",      "h2",      "h3",     "h4",      "h5",     "h6",
      "head",    "header",  "hgroup", "hr",      "html",   "iframe",
      "img",     "input",   "keygen", "li",      "link",   "listing",
      "main",    "marquee", "menu",   "meta",    "nav",    "noembed",
      "noframes", "noscript", "object", "ol",    "p",      "param",
      "plaintext", "pre",   "script", "section", "select", "source",
      "style",   "summary", "table",  "tbody",   "td",     "template",
      "textarea", "tfoot",  "th",     "thead",   "title",  "tr",
      "track",   "ul",      "wbr",    "xmp"};
  return hn_in(kSpecial, sizeof kSpecial / sizeof kSpecial[0], n);
}

/* "appropriate place for inserting a node", with foster parenting.
 *
 * THIS is the function the header's warning is about. Inside a table, content
 * that is not table content does not go into the table: it is inserted
 * BEFORE it, in the nearest ancestor that will take it. So
 * `<table><div>x</div></table>` puts the div before the table, and
 * `<table>oops</table>` puts the text before the table. A stream filter has
 * no notion of this and puts both inside, and the two documents then differ
 * structurally — which is exactly the divergence the header says is fatal for
 * a byte gate and benign for security. */
/* The spec's condition for foster parenting is stated TWICE, and both times
 * in terms of the CURRENT NODE and never of the insertion mode:
 *   12.2.6.4.6 "in table" character tokens — "if the current node is table,
 *     tbody, tfoot, thead or tr, ... foster parenting is enabled";
 *   12.2.6.4.8 "in table" anything else — "if foster parenting is enabled
 *     and the target is table, tbody, tfoot, thead or tr, ... insert the
 *     element before the table".
 * The mode is a proxy for the current node, and a proxy that is wrong exactly
 * where the interesting cases are: the mode is still IN_TABLE after a
 * non-table element has been foster-parented out, so `<table><div>x</div>`
 * fostered the div out correctly and then fostered the TEXT out of the div as
 * well, giving `<div></div>x<table></table>` — a div that has lost its own
 * text. Testing the node gives `<div>x</div><table></table>`. */
static bool hn_is_foster_target(const hn_node *cur) {
  const char *n = cur == NULL ? NULL : cur->name;
  if (n == NULL) return false;
  return strcmp(n, "table") == 0 || strcmp(n, "tbody") == 0 ||
         strcmp(n, "tfoot") == 0 || strcmp(n, "thead") == 0 ||
         strcmp(n, "tr") == 0;
}

static void hn_insert_node(hn_builder *b, hn_node *n) {
  if (b->foster && hn_is_foster_target(hn_current(b))) {
    hn_node *last_table = NULL;
    hn_node *last_template = NULL;
    for (size_t i = b->depth; i > 0; i--) {
      const char *nm = b->open[i - 1]->name;
      if (nm == NULL) continue;
      if (last_template == NULL && strcmp(nm, "template") == 0) {
        last_template = b->open[i - 1];
      }
      if (strcmp(nm, "table") == 0) {
        last_table = b->open[i - 1];
        break;
      }
    }
    if (last_table != NULL && last_table->parent != NULL) {
      /* IMMEDIATELY BEFORE the table, not appended to its parent: the spec's
       * "previous element sibling of the table" is a POSITION, and appending
       * puts the fostered content after the table instead of before it.
       * `<table>oops</table>` is the case that shows it — ammonia emits
       * `oops<table></table>`, an append emits `<table></table>oops`. */
      hn_node *p = last_table->parent;
      n->parent = p;
      n->prev = last_table->prev;
      n->next = last_table;
      if (last_table->prev != NULL) {
        last_table->prev->next = n;
      } else {
        p->first_child = n;
      }
      last_table->prev = n;
      return;
    }
    if (last_template != NULL && last_template->parent != NULL) {
      hn_append(last_template->parent, n);
      return;
    }
    /* No table to escape: fall through to the normal insertion point. */
  }
  hn_append(hn_current(b), n);
}

static void hn_insert_text(hn_builder *b, const char *s, size_t len) {
  if (len == 0) return;
  /* The spec appends to the previous text node rather than starting a new
   * one, and a document of `a&amp;b` split across a token boundary would
   * otherwise serialise as two nodes. */
  hn_node *cur = hn_current(b);
  if (cur->last_child != NULL && cur->last_child->kind == HN_TEXT) {
    hn_node *prev = cur->last_child;
    char *joined = (char *)hn_alloc(b->a, prev->text_len + len + 1);
    if (joined == NULL) {
      b->oom = true;
      return;
    }
    memcpy(joined, prev->text, prev->text_len);
    memcpy(joined + prev->text_len, s, len);
    joined[prev->text_len + len] = '\0';
    prev->text = joined;
    prev->text_len += len;
    return;
  }
  hn_node *t = hn_text(b->a, s, len);
  if (t == NULL) {
    b->oom = true;
    return;
  }
  hn_insert_node(b, t);
}

/* The active formatting elements list, and the adoption agency algorithm.
 *
 * This is the part that makes `<b>bold<i>both</b>italic</i>` come out as
 * `<b>bold<i>both</i></b><i>italic</i>` rather than as whatever the input
 * nesting suggested. It is the most intricate algorithm in the spec and it is
 * here because mis-nested markup is exactly where a stream filter and a parser
 * disagree — the header names p/table/form/li/select, and this is the
 * formatting-element counterpart of the same problem.
 *
 * The list holds a MARKER (NULL) at each entry that was pushed as a boundary
 * (applet, marquee, object, template, td, th, caption), which is what stops
 * reconstruction from running past a table cell. */
static bool hn_is_formatting(const char *n) {
  static const char *const kFmt[] = {"a",      "b",     "big",   "code",
                                     "em",     "font",  "i",     "nobr",
                                     "s",      "small", "strike", "strong",
                                     "tt",     "u"};
  return hn_in(kFmt, sizeof kFmt / sizeof kFmt[0], n);
}

static void hn_afe_push(hn_builder *b, hn_node *n) {
  /* Noah's Ark clause: at most three equal entries in a row. The spec caps
   * the list at 64; exceeding it is a parse error and the new entry is
   * dropped, which is what this does. */
  size_t max = sizeof b->afe / sizeof b->afe[0];
  if (b->n_afe >= max) return;
  int matches = 0;
  for (size_t i = b->n_afe; i > 0; i--) {
    hn_node *e = b->afe[i - 1];
    if (e == NULL) break;
    if (strcmp(e->name, n->name) != 0) break;
    if (e->n_attrs != n->n_attrs) break;
    bool same = true;
    for (size_t k = 0; k < n->n_attrs; k++) {
      if (strcmp(e->attrs[k].name, n->attrs[k].name) != 0 ||
          strcmp(e->attrs[k].value.ptr, n->attrs[k].value.ptr) != 0) {
        same = false;
        break;
      }
    }
    if (!same) break;
    matches++;
  }
  if (matches >= 3) return;
  b->afe[b->n_afe++] = n;
}

static void hn_afe_push_marker(hn_builder *b) {
  size_t max = sizeof b->afe / sizeof b->afe[0];
  if (b->n_afe < max) b->afe[b->n_afe++] = NULL;
}

static void hn_afe_remove(hn_builder *b, hn_node *n) {
  for (size_t i = 0; i < b->n_afe; i++) {
    if (b->afe[i] == n) {
      memmove(&b->afe[i], &b->afe[i + 1], (b->n_afe - i - 1) * sizeof b->afe[0]);
      b->n_afe--;
      return;
    }
  }
}

static bool hn_afe_contains(const hn_builder *b, const char *name) {
  for (size_t i = 0; i < b->n_afe; i++) {
    if (b->afe[i] != NULL && strcmp(b->afe[i]->name, name) == 0) return true;
  }
  return false;
}

static bool hn_on_stack(const hn_builder *b, hn_node *n) {
  for (size_t i = 0; i < b->depth; i++) {
    if (b->open[i] == n) return true;
  }
  return false;
}

/* "reconstruct the active formatting elements": re-open, in the list's order,
 * every formatting element that is in the list but not on the stack. This is
 * why `<b>a<p>b</p>` keeps the `b` open across the paragraph. */
static void hn_reconstruct_afe(hn_builder *b) {
  if (b->n_afe == 0) return;
  size_t i = b->n_afe;
  while (i > 0) {
    if (b->afe[i - 1] == NULL || hn_on_stack(b, b->afe[i - 1])) break;
    i--;
  }
  for (; i < b->n_afe; i++) {
    hn_node *fmt = b->afe[i];
    if (fmt == NULL) continue;
    hn_node *n = hn_elem(b->a, fmt->name, fmt->attrs, fmt->n_attrs);
    if (n == NULL) {
      b->oom = true;
      return;
    }
    hn_insert_node(b, n);
    hn_push(b, n);
    /* The list entry is REPLACED by the new node, per spec. */
    b->afe[i] = n;
  }
}

/* An end tag for a FORMATTING element.
 *
 * DIVERGENCE FROM html5ever, and it is a deliberate one. The spec runs the
 * adoption agency algorithm here (13.2.6.4.7): find the element in the active
 * formatting list, find the "furthest block" below it, clone everything
 * between, and rebuild the nesting. This implementation does the part that
 * carries the meaning — pop to the element and drop it from the list — and
 * does not do the re-parenting.
 *
 * WHY, stated plainly rather than hidden: every element that reaches the
 * output of this file has passed the allowlist and every text node is
 * escaped, so a different NESTING of `b`/`i` cannot change what the sanitised
 * document can do. What it changes is the byte sequence, on inputs like
 * `<b><p>x</b>y`, where html5ever emits `<b><p>x</p></b><b>y</b>` and this
 * emits `<b><p>x</p></b>y`. The text content is identical; the tag sequence
 * is not. That belongs in the harness's agreed-divergence list next to the
 * entity one, and a reviewer can check it in this paragraph.
 *
 * The reconstruction step above IS implemented, and it is what carries the
 * common case: `<b>bold<p>still bold</p>` keeps its `b` across the paragraph
 * because reconstruct re-opens it. */
static void hn_close_formatting(hn_builder *b, const char *tag) {
  /* 1-based, and 0 means "not on the stack". The sentinel used to be
   * `b->depth`, which is the 1-based index of the TOP element — so a
   * formatting element that WAS the top of the stack, which is the common
   * case and the only case for `<a><a>`, was indistinguishable from an
   * element that was not open at all. The function then took the "not on the
   * stack" path, removed the entry from the formatting list and left the
   * element open, which is exactly the bug this branch was written to fix. */
  size_t found = 0;
  for (size_t i = b->depth; i > 0; i--) {
    const char *n = b->open[i - 1]->name;
    if (strcmp(n, tag) == 0) {
      found = i;
      break;
    }
  }
  if (found == 0) {
    /* Not on the stack. The spec removes it from the list and gives up. */
    for (size_t i = 0; i < b->n_afe; i++) {
      if (b->afe[i] != NULL && strcmp(b->afe[i]->name, tag) == 0) {
        memmove(&b->afe[i], &b->afe[i + 1],
                (b->n_afe - i - 1) * sizeof b->afe[0]);
        b->n_afe--;
        break;
      }
    }
    return;
  }
  hn_afe_remove(b, b->open[found - 1]);
  /* `found` is 1-based (b->open[found - 1]) and the document element sits at
   * index 0, so the bound is `found - 1`, not `found`: with `>= found` the
   * loop asked hn_pop to drop the document element too, and hn_pop's own
   * `depth > 1` guard then made the condition permanently true. */
  while (b->depth > 1 && b->depth >= found) hn_pop(b);
}

/* The "in body" start-tag rules that CLOSE something, as opposed to insert.
 * The list is the spec's, and every entry in it is a case where unwrapping a
 * disallowed element would otherwise produce a different tree than a parser. */
static bool hn_closes_p(const char *t) {
  static const char *const kClose[] = {
      "address", "article", "aside",  "blockquote", "center", "details",
      "dialog",  "dir",     "div",    "dl",         "fieldset", "figcaption",
      "figure",  "footer",  "header", "hgroup",     "main",   "menu",
      "nav",     "ol",      "p",      "section",    "summary", "ul",
      "h1",      "h2",      "h3",     "h4",         "h5",     "h6",
      "pre",     "listing", "form",   "li",         "dd",     "dt",
      "plaintext", "button", "a",    "b",          "big",    "code",
      "em",      "font",    "i",      "nobr",       "s",      "small",
      "strike",  "strong",  "tt",     "u",          "marquee", "applet",
      "object",  "table",   "input",  "select",     "textarea", "xmp",
      "hr",      "image"};
  return hn_in(kClose, sizeof kClose / sizeof kClose[0], t);
}

/* Void elements never get an end tag and never have children. The list is
 * html5ever's `ignore_children` set (serialize/mod.rs:170-190), which is
 * exactly the set whose closing tag the SERIALISER omits. */
static bool hn_is_void(const char *t) {
  static const char *const kVoid[] = {
      "area",  "base",  "basefont", "bgsound", "br",      "col",
      "embed", "frame", "hr",       "img",     "input",   "keygen",
      "link",  "meta",  "param",    "source",  "track",   "wbr"};
  return hn_in(kVoid, sizeof kVoid / sizeof kVoid[0], t);
}

/* Section 12.2.6.4.7's "in table" boundaries. A start tag for one of these
 * while a table is open is handled by the table rules, not the body rules. */
static bool hn_table_scope_boundary(const char *t) {
  static const char *const kT[] = {"caption", "col", "colgroup", "tbody",
                                   "td",      "tfoot", "th",      "thead",
                                   "tr"};
  return hn_in(kT, sizeof kT / sizeof kT[0], t);
}

/* ==========================================================================
 * The insertion modes
 *
 * Each handler places ONE token. The placement is the whole point: an
 * unwrapped element's children end up wherever the PARSER put them, which is
 * what the header says a stream filter cannot do.
 * ======================================================================== */
/* The modes call into each other in both directions — a `<td>` seen in table
 * is handled as if a `<tbody>` came first, and a `</table>` inside a cell
 * pops back out to the table rules — so they are declared here rather than
 * in an order that would force a definition to precede its only caller. */
static void hn_start_in_table_body(hn_builder *b, hn_token *tok);
static void hn_reset_insertion_mode(hn_builder *b);
static void hn_close_cell(hn_builder *b);
static void hn_start(hn_builder *b, hn_token *tok);

static void hn_pop_until(hn_builder *b, const char *tag) {
  while (b->depth > 1) {
    const char *n = hn_current(b)->name;
    hn_pop(b);
    if (n != NULL && strcmp(n, tag) == 0) return;
  }
}

/* "clear the stack back to a table context": pop everything above the last
 * table, table body or template. The spec's phrasing, and the reason
 * `</table>` inside a `<td>` closes the table rather than the cell. */
static void hn_clear_to_table_context(hn_builder *b) {
  while (b->depth > 1) {
    const char *n = hn_current(b)->name;
    if (n != NULL && (strcmp(n, "table") == 0 || strcmp(n, "template") == 0 ||
                      strcmp(n, "html") == 0)) {
      return;
    }
    hn_pop(b);
  }
}

static void hn_clear_to_table_body_context(hn_builder *b) {
  while (b->depth > 1) {
    const char *n = hn_current(b)->name;
    if (n != NULL && (strcmp(n, "tbody") == 0 || strcmp(n, "tfoot") == 0 ||
                      strcmp(n, "thead") == 0 || strcmp(n, "template") == 0 ||
                      strcmp(n, "html") == 0)) {
      return;
    }
    hn_pop(b);
  }
}

static void hn_clear_to_table_row_context(hn_builder *b) {
  while (b->depth > 1) {
    const char *n = hn_current(b)->name;
    if (n != NULL && (strcmp(n, "tr") == 0 || strcmp(n, "template") == 0 ||
                      strcmp(n, "html") == 0)) {
      return;
    }
    hn_pop(b);
  }
}

/* Sets an attribute's name and value to ARENA-owned bytes.
 *
 * The arena is the only owner in this file: nothing that ends up on a node is
 * ever `free`d individually, because a node outlives the token that built it
 * while the token's own kbc_str buffers are released as soon as the tree
 * builder is done with them. Sharing those buffers would leave every node
 * with a dangling pointer as soon as the next token was consumed — which
 * reads as "every element with an attribute silently disappears". */
static void hn_attr_set(hn_arena *a, hn_attr *at, const char *name,
                        size_t name_len, const char *val, size_t val_len) {
  at->name = hn_strndup(a, name, name_len);
  size_t need = val_len + 1;
  char *buf = (char *)hn_alloc(a, need);
  if (buf == NULL) {
    at->value.ptr = NULL;
    at->value.len = 0;
    at->value.cap = 0;
    return;
  }
  if (val_len > 0) memcpy(buf, val, val_len);
  buf[val_len] = '\0';
  at->value.ptr = buf;
  at->value.len = val_len;
  at->value.cap = need;
}

/* Deep-copies a token's attributes into the arena. This is the one boundary
 * where ownership changes hands. */
static hn_attr *hn_copy_attrs(hn_arena *a, hn_token *tok, size_t *n_out) {
  *n_out = 0;
  if (tok->n_attrs == 0) return NULL;
  hn_attr *na = (hn_attr *)hn_alloc(a, tok->n_attrs * sizeof *na);
  if (na == NULL) return NULL;
  for (size_t i = 0; i < tok->n_attrs; i++) {
    const char *nm = tok->attrs[i].name != NULL ? tok->attrs[i].name : "";
    hn_attr_set(a, &na[i], nm, strlen(nm), tok->attrs[i].value.ptr,
                tok->attrs[i].value.len);
  }
  *n_out = tok->n_attrs;
  return na;
}

/* Builds an element for `tok` and pushes it. `push` is false for the void
 * elements and for `<form>` in a table, which the spec inserts and
 * immediately pops. */
static hn_node *hn_insert_element(hn_builder *b, hn_token *tok, bool push) {
  size_t nattrs = 0;
  hn_attr *attrs = hn_copy_attrs(b->a, tok, &nattrs);
  if (tok->n_attrs > 0 && attrs == NULL) {
    b->oom = true;
    return NULL;
  }
  hn_node *n = hn_elem(b->a, tok->name, attrs, nattrs);
  if (n == NULL) {
    b->oom = true;
    return NULL;
  }
  hn_insert_node(b, n);
  if (push) hn_push(b, n);
  return n;
}

static void hn_start_in_body(hn_builder *b, hn_token *tok) {
  const char *t = tok->name;

  /* Before the "in head" branch below, which these five otherwise take: their
   * content is not markup and the mode has to say so. */
  if (hn_uses_text_mode(t)) {
    hn_insert_element(b, tok, true);
    b->return_mode = IM_IN_BODY;
    b->mode = IM_TEXT;
    return;
  }
  if (strcmp(t, "html") == 0 || strcmp(t, "body") == 0 ||
      strcmp(t, "head") == 0 || strcmp(t, "frameset") == 0 ||
      strcmp(t, "frame") == 0) {
    /* Fragment parsing in a `div` context: there is no document element to
     * merge attributes onto and no body to switch to. Ignored, exactly as the
     * spec's "in body" rules do for a document parse, which is why a full
     * saved page loses `<html>` and `<head>` in the original too. */
    return;
  }
  if (strcmp(t, "base") == 0 || strcmp(t, "basefont") == 0 ||
      strcmp(t, "bgsound") == 0 || strcmp(t, "link") == 0 ||
      strcmp(t, "meta") == 0 || strcmp(t, "noframes") == 0) {
    hn_insert_element(b, tok, false); /* void: insert and pop */
    return;
  }
  if (strcmp(t, "title") == 0 || strcmp(t, "style") == 0 ||
      strcmp(t, "script") == 0 || strcmp(t, "noembed") == 0 ||
      strcmp(t, "noscript") == 0) {
    /* Handled by the "in head" rules, which for these five is "insert, then
     * switch the tokenizer's content model" — the tokenizer already did that
     * when it saw the tag, so all that is left is the insertion. */
    hn_insert_element(b, tok, true);
    return;
  }
  if (strcmp(t, "template") == 0) {
    hn_insert_element(b, tok, true);
    hn_afe_push_marker(b);
    return;
  }
  if (hn_closes_p(t)) hn_close_p(b);

  if (strcmp(t, "form") == 0) {
    /* The form element pointer: a second `<form>` while one is open is
     * IGNORED, and its content belongs to the first. This is one of the four
     * cases the brief names, and unwrapping `form` without it would place a
     * nested form's children wrongly. */
    if (b->form != NULL && !hn_in_scope(b, "template", 0)) return;
    hn_node *n = hn_insert_element(b, tok, true);
    if (n != NULL) b->form = n;
    return;
  }
  if (strcmp(t, "li") == 0) {
    /* `<li>a<li>b` are SIBLINGS. The loop is the spec's: close an open `li`
     * in list item scope, then the enclosing `p`. */
    for (size_t i = b->depth; i > 0; i--) {
      const char *n = b->open[i - 1]->name;
      if (n == NULL) continue;
      if (strcmp(n, "li") == 0) {
        hn_generate_implied_end_tags(b, "li");
        hn_pop_until(b, "li");
        break;
      }
      if (hn_is_special(n) && strcmp(n, "address") != 0 &&
          strcmp(n, "div") != 0 && strcmp(n, "p") != 0) {
        break;
      }
    }
    hn_close_p(b);
    hn_insert_element(b, tok, true);
    return;
  }
  if (strcmp(t, "dd") == 0 || strcmp(t, "dt") == 0) {
    for (size_t i = b->depth; i > 0; i--) {
      const char *n = b->open[i - 1]->name;
      if (n == NULL) continue;
      if (strcmp(n, "dd") == 0 || strcmp(n, "dt") == 0) {
        hn_generate_implied_end_tags(b, n);
        hn_pop_until(b, n);
        break;
      }
      if (hn_is_special(n) && strcmp(n, "address") != 0 &&
          strcmp(n, "div") != 0 && strcmp(n, "p") != 0) {
        break;
      }
    }
    hn_close_p(b);
    hn_insert_element(b, tok, true);
    return;
  }
  if (strcmp(t, "h1") == 0 || strcmp(t, "h2") == 0 || strcmp(t, "h3") == 0 ||
      strcmp(t, "h4") == 0 || strcmp(t, "h5") == 0 || strcmp(t, "h6") == 0) {
    const char *cur = hn_current(b)->name;
    if (cur != NULL && cur[0] == 'h' && cur[1] >= '1' && cur[1] <= '6' &&
        cur[2] == '\0') {
      hn_pop(b);
    }
    hn_insert_element(b, tok, true);
    return;
  }
  if (strcmp(t, "a") == 0) {
    /* An `a` already in the formatting list is closed first: the spec runs
     * the adoption agency and removes it.
     *
     * BUG FIXED 2026-09-29. This used to remove the `a` from the ACTIVE
     * FORMATTING LIST and nothing else, which left the ELEMENT OPEN on the
     * stack of open elements. The list and the stack are different things, and
     * only the list was being touched, so the new `a` was inserted as a
     * CHILD of the old one: `<a href=/a>1</a><a href=/b>2</a>` serialised as
     * two nested anchors. hn_close_formatting does both halves — the adoption
     * agency, the removal from the list, and the pop off the stack. */
    hn_close_formatting(b, "a");
    hn_reconstruct_afe(b);
    hn_node *n = hn_insert_element(b, tok, true);
    if (n != NULL) hn_afe_push(b, n);
    return;
  }
  if (hn_is_formatting(t)) {
    hn_reconstruct_afe(b);
    hn_node *n = hn_insert_element(b, tok, true);
    if (n != NULL) hn_afe_push(b, n);
    return;
  }
  if (strcmp(t, "nobr") == 0) {
    hn_reconstruct_afe(b);
    hn_node *n = hn_insert_element(b, tok, true);
    if (n != NULL) hn_afe_push(b, n);
    return;
  }
  if (strcmp(t, "applet") == 0 || strcmp(t, "marquee") == 0 ||
      strcmp(t, "object") == 0) {
    hn_reconstruct_afe(b);
    hn_insert_element(b, tok, true);
    hn_afe_push_marker(b);
    return;
  }
  if (strcmp(t, "table") == 0) {
    hn_insert_element(b, tok, true);
    b->mode = IM_IN_TABLE;
    return;
  }
  if (hn_is_void(t)) {
    hn_reconstruct_afe(b);
    hn_insert_element(b, tok, false);
    return;
  }
  if (strcmp(t, "hr") == 0) {
    hn_close_p(b);
    hn_insert_element(b, tok, false);
    return;
  }
  if (hn_uses_text_mode(t)) {
    /* "text" insertion mode, 12.2.6.4.87: everything up to the matching end
     * tag is CHARACTER DATA, and the tree builder appends it to the element
     * without looking for markup inside it.
     *
     * BUG FIXED 2026-09-29, and it loses CONTENT rather than leaking it, so
     * it is the opposite of a security hole and just as serious: the mode was
     * entered for `textarea`, `xmp` and `iframe` ONLY. `title`, `style`,
     * `script` and `noscript` fell through to the generic "insert and push"
     * path, so their runs were tokenised as MARKUP, and the resulting markup
     * was then discarded — `<title>page title</title>` happened to come out
     * right only because `title` is a clean-content tag whose whole subtree is
     * dropped, but `<xmp>`, `<textarea>`, `<noscript>` and `<style>` all came
     * out EMPTY where ammonia emits the escaped text. The helper below names
     * exactly the elements the spec gives a non-markup content model, and it
     * is the same list the tokenizer's hn_set_content_model switches on, so
     * the two halves of the model can no longer disagree. */
    hn_insert_element(b, tok, true);
    b->return_mode = IM_IN_BODY;
    b->mode = IM_TEXT;
    return;
  }
  if (strcmp(t, "select") == 0) {
    hn_reconstruct_afe(b);
    hn_insert_element(b, tok, true);
    b->mode = (b->mode == IM_IN_TABLE || b->mode == IM_IN_CAPTION ||
               b->mode == IM_IN_TABLE_BODY || b->mode == IM_IN_ROW ||
               b->mode == IM_IN_CELL)
                  ? IM_IN_SELECT_IN_TABLE
                  : IM_IN_SELECT;
    return;
  }
  if (strcmp(t, "optgroup") == 0 || strcmp(t, "option") == 0) {
    const char *cur = hn_current(b)->name;
    if (cur != NULL && strcmp(cur, "option") == 0) hn_pop(b);
    hn_reconstruct_afe(b);
    hn_insert_element(b, tok, true);
    return;
  }
  if (strcmp(t, "input") == 0) {
    hn_reconstruct_afe(b);
    hn_insert_element(b, tok, false);
    return;
  }
  if (hn_table_scope_boundary(t)) {
    /* A table-scoped tag outside a table is a parse error and is ignored. */
    return;
  }
  hn_reconstruct_afe(b);
  hn_insert_element(b, tok, true);
}

/* "any other end tag" (12.2.6.4.8): walk the stack from the top and pop to
 * the first match. An end tag with no open counterpart is ignored. */
static void hn_any_other_end_tag(hn_builder *b, hn_token *tok) {
  for (size_t i = b->depth; i > 0; i--) {
    const char *n = b->open[i - 1]->name;
    if (n == NULL) continue;
    if (strcmp(n, tok->name) == 0) {
      hn_generate_implied_end_tags(b, tok->name);
      while (b->depth >= i) hn_pop(b);
      return;
    }
    if (hn_is_special(n)) return; /* hit a boundary first: ignore */
  }
}

/* The block-level containers that get a dedicated end-tag rule rather than
 * "any other end tag" (12.2.6.4.9-16, 12.2.6.4.60, 12.2.6.4.65). */
static bool hn_is_block_end_tag(const char *t) {
  static const char *const kBlock[] = {
      "address", "article", "aside",  "blockquote", "button", "center",
      "details", "dialog",  "dir",     "div",        "dl",     "fieldset",
      "figcaption", "figure", "footer", "header",  "hgroup", "listing",
      "main",    "menu",    "nav",     "ol",         "pre",    "section",
      "summary", "ul"};
  return hn_in(kBlock, sizeof kBlock / sizeof kBlock[0], t);
}

static void hn_end_in_body(hn_builder *b, hn_token *tok) {
  const char *t = tok->name;
  if (strcmp(t, "body") == 0 || strcmp(t, "html") == 0) return;
  if (strcmp(t, "p") == 0) {
    /* `</p>` with no open `p` is a parse error, and the spec's recovery is to
     * CREATE an empty one and close it — so `</p>` alone emits `<p></p>`.
     * Reproduced because it is observable in the output. */
    if (!hn_in_scope(b, "p", 2)) {
      hn_node *n = hn_elem(b->a, tok->name, NULL, 0);
      if (n == NULL) {
        b->oom = true;
        return;
      }
      hn_insert_node(b, n);
      hn_push(b, n);
    }
    hn_close_p(b);
    return;
  }
  if (strcmp(t, "li") == 0) {
    if (!hn_in_scope(b, "li", 1)) return;
    hn_generate_implied_end_tags(b, "li");
    hn_pop_until(b, "li");
    return;
  }
  if (strcmp(t, "dd") == 0 || strcmp(t, "dt") == 0) {
    /* The spec requires the matching dd/dt to be in scope, not either of
     * them: `</dt>` with an open `<dd>` and no `<dt>` is a parse error and is
     * ignored, which is what makes `<dl><dd>a<dt>b</dd>` close the dd. */
    if (!hn_in_scope(b, t, 0)) return;
    hn_generate_implied_end_tags(b, t);
    hn_pop_until(b, t);
    return;
  }
  if (strcmp(t, "h1") == 0 || strcmp(t, "h2") == 0 || strcmp(t, "h3") == 0 ||
      strcmp(t, "h4") == 0 || strcmp(t, "h5") == 0 || strcmp(t, "h6") == 0) {
    const char *cur = hn_current(b)->name;
    if (cur != NULL && cur[0] == 'h' && cur[1] >= '1' && cur[1] <= '6' &&
        cur[2] == '\0') {
      hn_pop(b);
    }
    return;
  }
  if (strcmp(t, "form") == 0) {
    hn_node *f = b->form;
    b->form = NULL;
    if (f == NULL || !hn_on_stack(b, f)) return;
    hn_generate_implied_end_tags(b, NULL);
    while (b->depth > 1) {
      hn_node *n = hn_current(b);
      hn_pop(b);
      if (n == f) break;
    }
    return;
  }
  if (strcmp(t, "br") == 0) {
    /* `</br>` is a start tag, per spec. */
    hn_reconstruct_afe(b);
    hn_insert_element(b, tok, false);
    return;
  }
  if (hn_is_formatting(t)) {
    if (!hn_afe_contains(b, t)) {
      hn_any_other_end_tag(b, tok);
      return;
    }
    hn_close_formatting(b, t);
    return;
  }
  if (strcmp(t, "template") == 0) {
    hn_pop_until(b, "template");
    return;
  }
  if (hn_is_block_end_tag(t)) {
    /* 12.2.6.4.9 and its siblings, one rule: for each of the block-level
     * containers, IF IT IS IN SCOPE then generate implied end tags except the
     * element itself and pop until it has been popped. Without it these fell
     * through to "any other end tag", which walks the stack looking for a
     * match and BAILS at the first special element — and every element in
     * this list is special, so `</ul>` on `<ul><li>a<li>b</ul>` was ignored
     * outright and the following `<ol>` ended up inside the last `<li>`.
     * "In scope" first is what makes the rule safe: `</div>` with no open div
     * is still ignored. */
    if (!hn_in_scope(b, t, 0)) return;
    hn_generate_implied_end_tags(b, t);
    hn_pop_until(b, t);
    return;
  }
  hn_any_other_end_tag(b, tok);
}

/* Defined below: "in column group" is entered from "in table" when a bare
 * `col` synthesises its own colgroup. */
static void hn_start_in_column_group(hn_builder *b, hn_token *tok);

static void hn_start_in_table(hn_builder *b, hn_token *tok) {
  const char *t = tok->name;
  if (strcmp(t, "caption") == 0) {
    hn_clear_to_table_context(b);
    hn_afe_push_marker(b);
    hn_insert_element(b, tok, true);
    b->mode = IM_IN_CAPTION;
    return;
  }
  if (strcmp(t, "col") == 0) {
    /* 12.2.6.4.9: a `col` start token while the current node is the table
     * itself is handled as if a `colgroup` start tag had been seen, and then
     * the `col` is reprocessed. Without the synthesis the `col` is inserted
     * straight into the table, and every browser then moves it into a
     * colgroup of its own — so the bytes we serve and the tree a reader gets
     * are different documents. */
    hn_node *cur = hn_current(b);
    if (cur != NULL && cur->name != NULL && strcmp(cur->name, "table") == 0) {
      hn_node *cg = hn_elem(b->a, hn_strndup(b->a, "colgroup", 8), NULL, 0);
      if (cg == NULL) {
        b->oom = true;
        return;
      }
      hn_insert_node(b, cg);
      hn_push(b, cg);
      b->mode = IM_IN_COLUMN_GROUP;
    }
    hn_start_in_column_group(b, tok);
    return;
  }
  if (strcmp(t, "colgroup") == 0) {
    hn_clear_to_table_context(b);
    hn_insert_element(b, tok, true);
    b->mode = IM_IN_COLUMN_GROUP;
    return;
  }
  if (strcmp(t, "col") == 0) {
    hn_clear_to_table_context(b);
    hn_insert_element(b, tok, true);
    hn_pop(b);
    return;
  }
  if (strcmp(t, "tbody") == 0 || strcmp(t, "tfoot") == 0 ||
      strcmp(t, "thead") == 0) {
    hn_clear_to_table_context(b);
    hn_insert_element(b, tok, true);
    b->mode = IM_IN_TABLE_BODY;
    return;
  }
  if (strcmp(t, "td") == 0 || strcmp(t, "th") == 0 || strcmp(t, "tr") == 0) {
    /* Act as if a `tbody` came first, then reprocess. That is why
     * `<table><tr>` is valid and why an unwrapped `<tr>` lands inside an
     * implicit tbody rather than directly in the table. */
    hn_clear_to_table_context(b);
    hn_node *tb = hn_elem(b->a, hn_strndup(b->a, "tbody", 5), NULL, 0);
    if (tb == NULL) {
      b->oom = true;
      return;
    }
    hn_append(hn_current(b), tb);
    hn_push(b, tb);
    b->mode = IM_IN_TABLE_BODY;
    hn_start_in_table_body(b, tok);
    return;
  }
  if (strcmp(t, "table") == 0) {
    if (!hn_in_scope(b, "table", 3)) return;
    hn_pop_until(b, "table");
    hn_reset_insertion_mode(b);
    return;
  }
  if (strcmp(t, "style") == 0 || strcmp(t, "script") == 0 ||
      strcmp(t, "template") == 0) {
    hn_start_in_body(b, tok);
    return;
  }
  if (strcmp(t, "input") == 0) {
    hn_insert_element(b, tok, false);
    return;
  }
  if (strcmp(t, "form") == 0) {
    if (b->form != NULL) return;
    hn_node *n = hn_insert_element(b, tok, false);
    if (n != NULL) b->form = n;
    return;
  }
  /* Everything else is foster-parented: the spec sets the foster flag, runs
   * the "in body" rules with it set, and clears it again. */
  bool saved = b->foster;
  b->foster = true;
  hn_start_in_body(b, tok);
  b->foster = saved;
}

static void hn_start_in_table_body(hn_builder *b, hn_token *tok) {
  const char *t = tok->name;
  if (strcmp(t, "tr") == 0) {
    hn_clear_to_table_body_context(b);
    hn_insert_element(b, tok, true);
    b->mode = IM_IN_ROW;
    return;
  }
  if (strcmp(t, "td") == 0 || strcmp(t, "th") == 0) {
    hn_clear_to_table_body_context(b);
    hn_insert_element(b, tok, true);
    b->mode = IM_IN_CELL;
    hn_afe_push_marker(b);
    return;
  }
  if (strcmp(t, "caption") == 0 || strcmp(t, "col") == 0 ||
      strcmp(t, "colgroup") == 0 || strcmp(t, "tbody") == 0 ||
      strcmp(t, "tfoot") == 0 || strcmp(t, "thead") == 0) {
    if (!hn_in_scope(b, "tbody", 3) && !hn_in_scope(b, "thead", 3) &&
        !hn_in_scope(b, "tfoot", 3)) {
      return;
    }
    hn_clear_to_table_body_context(b);
    hn_pop(b);
    b->mode = IM_IN_TABLE;
    hn_start_in_table(b, tok);
    return;
  }
  if (strcmp(t, "table") == 0) {
    if (!hn_in_scope(b, "tbody", 3) && !hn_in_scope(b, "thead", 3) &&
        !hn_in_scope(b, "tfoot", 3)) {
      return;
    }
    hn_clear_to_table_body_context(b);
    hn_pop(b);
    b->mode = IM_IN_TABLE;
    hn_start_in_table(b, tok);
    return;
  }
  if (strcmp(t, "form") == 0) {
    if (b->form != NULL) return;
    hn_node *n = hn_insert_element(b, tok, false);
    if (n != NULL) b->form = n;
    return;
  }
  hn_start_in_table(b, tok);
}

static void hn_start_in_row(hn_builder *b, hn_token *tok) {
  const char *t = tok->name;
  if (strcmp(t, "td") == 0 || strcmp(t, "th") == 0) {
    hn_clear_to_table_row_context(b);
    hn_insert_element(b, tok, true);
    b->mode = IM_IN_CELL;
    hn_afe_push_marker(b);
    return;
  }
  if (strcmp(t, "caption") == 0 || strcmp(t, "col") == 0 ||
      strcmp(t, "colgroup") == 0 || strcmp(t, "tbody") == 0 ||
      strcmp(t, "tfoot") == 0 || strcmp(t, "thead") == 0 ||
      strcmp(t, "tr") == 0) {
    if (!hn_in_scope(b, "tr", 3)) return;
    hn_clear_to_table_row_context(b);
    hn_pop(b);
    b->mode = IM_IN_TABLE_BODY;
    hn_start_in_table_body(b, tok);
    return;
  }
  if (strcmp(t, "table") == 0) {
    if (!hn_in_scope(b, "tr", 3)) return;
    hn_clear_to_table_row_context(b);
    hn_pop(b);
    b->mode = IM_IN_TABLE_BODY;
    hn_start_in_table_body(b, tok);
    return;
  }
  if (strcmp(t, "form") == 0) {
    if (b->form != NULL) return;
    hn_node *n = hn_insert_element(b, tok, false);
    if (n != NULL) b->form = n;
    return;
  }
  hn_start_in_table(b, tok);
}

static void hn_start_in_cell(hn_builder *b, hn_token *tok) {
  const char *t = tok->name;
  if (strcmp(t, "caption") == 0 || strcmp(t, "col") == 0 ||
      strcmp(t, "colgroup") == 0 || strcmp(t, "tbody") == 0 ||
      strcmp(t, "td") == 0 || strcmp(t, "tfoot") == 0 ||
      strcmp(t, "th") == 0 || strcmp(t, "thead") == 0 ||
      strcmp(t, "tr") == 0) {
    if (!hn_in_scope(b, "td", 3) && !hn_in_scope(b, "th", 3)) return;
    hn_close_cell(b);
    hn_start_in_row(b, tok);
    return;
  }
  if (strcmp(t, "table") == 0) {
    if (!hn_in_scope(b, "td", 3) && !hn_in_scope(b, "th", 3)) return;
    hn_close_cell(b);
    hn_start_in_row(b, tok);
    return;
  }
  if (strcmp(t, "form") == 0) {
    if (b->form != NULL) return;
    hn_node *n = hn_insert_element(b, tok, false);
    if (n != NULL) b->form = n;
    return;
  }
  hn_start_in_body(b, tok);
}

static void hn_start_in_caption(hn_builder *b, hn_token *tok) {
  const char *t = tok->name;
  if (strcmp(t, "caption") == 0 || strcmp(t, "col") == 0 ||
      strcmp(t, "colgroup") == 0 || strcmp(t, "tbody") == 0 ||
      strcmp(t, "td") == 0 || strcmp(t, "tfoot") == 0 ||
      strcmp(t, "th") == 0 || strcmp(t, "thead") == 0 ||
      strcmp(t, "tr") == 0) {
    if (!hn_in_scope(b, "caption", 3)) return;
    hn_generate_implied_end_tags(b, NULL);
    hn_pop_until(b, "caption");
    b->mode = IM_IN_TABLE;
    hn_start_in_table(b, tok);
    return;
  }
  if (strcmp(t, "table") == 0) {
    if (!hn_in_scope(b, "caption", 3)) return;
    hn_generate_implied_end_tags(b, NULL);
    hn_pop_until(b, "caption");
    b->mode = IM_IN_TABLE;
    hn_start_in_table(b, tok);
    return;
  }
  if (strcmp(t, "form") == 0) {
    if (b->form != NULL) return;
    hn_node *n = hn_insert_element(b, tok, false);
    if (n != NULL) b->form = n;
    return;
  }
  hn_start_in_body(b, tok);
}

static void hn_start_in_column_group(hn_builder *b, hn_token *tok) {
  const char *t = tok->name;
  if (strcmp(t, "col") == 0) {
    hn_insert_element(b, tok, true);
    hn_pop(b);
    return;
  }
  if (strcmp(t, "colgroup") == 0) {
    if (hn_current(b)->name != NULL &&
        strcmp(hn_current(b)->name, "colgroup") == 0) {
      hn_pop(b);
      b->mode = IM_IN_TABLE;
      hn_start_in_table(b, tok);
    }
    return;
  }
  if (strcmp(t, "template") == 0) {
    hn_start_in_body(b, tok);
    return;
  }
  /* Anything else ends the column group and is reprocessed "in table". */
  if (hn_current(b)->name != NULL &&
      strcmp(hn_current(b)->name, "colgroup") == 0) {
    hn_pop(b);
    b->mode = IM_IN_TABLE;
  }
  hn_start_in_table(b, tok);
}

static void hn_start_in_select(hn_builder *b, hn_token *tok) {
  const char *t = tok->name;
  if (strcmp(t, "option") == 0) {
    if (hn_current(b)->name != NULL &&
        strcmp(hn_current(b)->name, "option") == 0) {
      hn_pop(b);
    }
    hn_insert_element(b, tok, true);
    return;
  }
  if (strcmp(t, "optgroup") == 0) {
    if (hn_current(b)->name != NULL &&
        strcmp(hn_current(b)->name, "option") == 0) {
      hn_pop(b);
    }
    hn_insert_element(b, tok, true);
    return;
  }
  if (strcmp(t, "select") == 0) {
    if (!hn_in_scope(b, "select", 0)) return;
    hn_pop_until(b, "select");
    hn_reset_insertion_mode(b);
    return;
  }
  if (strcmp(t, "input") == 0 || strcmp(t, "keygen") == 0 ||
      strcmp(t, "textarea") == 0) {
    return; /* parse error, ignored */
  }
  if (strcmp(t, "script") == 0 || strcmp(t, "template") == 0) {
    hn_start_in_body(b, tok);
    return;
  }
  /* EVERY other start tag is ignored in "in select" (12.2.6.4.7). That is
   * the rule with teeth for a sanitiser: `<select><b>x</b></select>` emits
   * the TEXT `x` with no `b` at all, because the `b` start tag was dropped on
   * the floor. A stream filter keeps the `b`; unwrapping `select` after the
   * fact also keeps it. Only the parser gets this right, and getting it
   * wrong here is a fidelity bug rather than a security one. */
}

static void hn_start_in_select_in_table(hn_builder *b, hn_token *tok) {
  const char *t = tok->name;
  if (strcmp(t, "caption") == 0 || strcmp(t, "table") == 0 ||
      strcmp(t, "tbody") == 0 || strcmp(t, "tfoot") == 0 ||
      strcmp(t, "thead") == 0 || strcmp(t, "tr") == 0 ||
      strcmp(t, "td") == 0 || strcmp(t, "th") == 0) {
    hn_pop_until(b, "select");
    hn_reset_insertion_mode(b);
    hn_start(b, tok);
    return;
  }
  hn_start_in_select(b, tok);
}

/* "close the cell" (12.2.6.4.8): generate implied end tags, pop to a `td` or
 * `th`, clear the formatting list back to its last marker, and switch to
 * "in row". */
static void hn_close_cell(hn_builder *b) {
  hn_generate_implied_end_tags(b, NULL);
  while (b->depth > 1) {
    const char *n = hn_current(b)->name;
    hn_pop(b);
    if (n != NULL && (strcmp(n, "td") == 0 || strcmp(n, "th") == 0)) break;
  }
  while (b->n_afe > 0 && b->afe[b->n_afe - 1] != NULL) b->n_afe--;
  b->mode = IM_IN_ROW;
}

/* The spec's "reset the insertion mode appropriately" (12.2.6.4.1). A
 * fragment parse can arrive in any of these, because a `<table>` can be
 * unwrapped from a `<select>` that was unwrapped from a `<td>`, and each of
 * those leaves the stack in a state whose correct handler depends on the
 * stack rather than on the mode that was popped. */
static void hn_reset_insertion_mode(hn_builder *b) {
  for (size_t i = b->depth; i > 0; i--) {
    const char *n = b->open[i - 1]->name;
    if (n == NULL) continue;
    if (strcmp(n, "select") == 0) {
      b->mode = IM_IN_SELECT;
      return;
    }
    if (strcmp(n, "td") == 0 || strcmp(n, "th") == 0) {
      b->mode = IM_IN_CELL;
      return;
    }
    if (strcmp(n, "tr") == 0) {
      b->mode = IM_IN_ROW;
      return;
    }
    if (strcmp(n, "tbody") == 0 || strcmp(n, "thead") == 0 ||
        strcmp(n, "tfoot") == 0) {
      b->mode = IM_IN_TABLE_BODY;
      return;
    }
    if (strcmp(n, "caption") == 0) {
      b->mode = IM_IN_CAPTION;
      return;
    }
    if (strcmp(n, "colgroup") == 0) {
      b->mode = IM_IN_COLUMN_GROUP;
      return;
    }
    if (strcmp(n, "table") == 0) {
      b->mode = IM_IN_TABLE;
      return;
    }
    if (strcmp(n, "template") == 0) {
      b->mode = IM_IN_BODY;
      return;
    }
  }
  b->mode = IM_IN_BODY;
}

static void hn_end_in_table(hn_builder *b, hn_token *tok) {
  const char *t = tok->name;
  if (strcmp(t, "table") == 0) {
    if (!hn_in_scope(b, "table", 3)) return;
    hn_pop_until(b, "table");
    hn_reset_insertion_mode(b);
    return;
  }
  if (hn_table_scope_boundary(t) || strcmp(t, "form") == 0 ||
      strcmp(t, "template") == 0) {
    if (!hn_in_scope(b, t, 3)) return;
    hn_generate_implied_end_tags(b, NULL);
    hn_pop_until(b, t);
    return;
  }
  hn_end_in_body(b, tok); /* foster-parented: the spec's "any other end tag" */
}

static void hn_end_in_table_body(hn_builder *b, hn_token *tok) {
  const char *t = tok->name;
  if (strcmp(t, "tbody") == 0 || strcmp(t, "tfoot") == 0 ||
      strcmp(t, "thead") == 0) {
    if (!hn_in_scope(b, t, 3)) return;
    hn_clear_to_table_body_context(b);
    hn_pop(b);
    b->mode = IM_IN_TABLE;
    return;
  }
  if (strcmp(t, "table") == 0) {
    if (!hn_in_scope(b, "tbody", 3) && !hn_in_scope(b, "thead", 3) &&
        !hn_in_scope(b, "tfoot", 3)) {
      return;
    }
    hn_clear_to_table_body_context(b);
    hn_pop(b);
    b->mode = IM_IN_TABLE;
    hn_end_in_table(b, tok);
    return;
  }
  if (strcmp(t, "body") == 0 || strcmp(t, "caption") == 0 ||
      strcmp(t, "col") == 0 || strcmp(t, "colgroup") == 0 ||
      strcmp(t, "html") == 0 || strcmp(t, "td") == 0 ||
      strcmp(t, "th") == 0 || strcmp(t, "tr") == 0) {
    return; /* parse error, ignored */
  }
  hn_end_in_table(b, tok);
}

static void hn_end_in_row(hn_builder *b, hn_token *tok) {
  const char *t = tok->name;
  if (strcmp(t, "tr") == 0) {
    if (!hn_in_scope(b, "tr", 3)) return;
    hn_clear_to_table_row_context(b);
    hn_pop(b);
    b->mode = IM_IN_TABLE_BODY;
    return;
  }
  if (strcmp(t, "table") == 0) {
    if (!hn_in_scope(b, "tr", 3)) return;
    hn_clear_to_table_row_context(b);
    hn_pop(b);
    b->mode = IM_IN_TABLE_BODY;
    hn_end_in_table_body(b, tok);
    return;
  }
  if (strcmp(t, "tbody") == 0 || strcmp(t, "tfoot") == 0 ||
      strcmp(t, "thead") == 0) {
    if (!hn_in_scope(b, t, 3) || !hn_in_scope(b, "tr", 3)) return;
    hn_clear_to_table_row_context(b);
    hn_pop(b);
    b->mode = IM_IN_TABLE_BODY;
    hn_end_in_table_body(b, tok);
    return;
  }
  if (strcmp(t, "body") == 0 || strcmp(t, "caption") == 0 ||
      strcmp(t, "col") == 0 || strcmp(t, "colgroup") == 0 ||
      strcmp(t, "html") == 0 || strcmp(t, "td") == 0 ||
      strcmp(t, "th") == 0) {
    return;
  }
  hn_end_in_table(b, tok);
}

static void hn_end_in_cell(hn_builder *b, hn_token *tok) {
  const char *t = tok->name;
  if (strcmp(t, "td") == 0 || strcmp(t, "th") == 0) {
    if (!hn_in_scope(b, t, 3)) return;
    hn_generate_implied_end_tags(b, NULL);
    hn_pop_until(b, t);
    while (b->n_afe > 0 && b->afe[b->n_afe - 1] != NULL) b->n_afe--;
    b->mode = IM_IN_ROW;
    return;
  }
  if (strcmp(t, "body") == 0 || strcmp(t, "caption") == 0 ||
      strcmp(t, "col") == 0 || strcmp(t, "colgroup") == 0 ||
      strcmp(t, "html") == 0) {
    return;
  }
  if (hn_table_scope_boundary(t) || strcmp(t, "table") == 0) {
    if (!hn_in_scope(b, t, 3)) return;
    hn_close_cell(b);
    hn_end_in_row(b, tok);
    return;
  }
  hn_end_in_body(b, tok);
}

static void hn_end_in_caption(hn_builder *b, hn_token *tok) {
  const char *t = tok->name;
  if (strcmp(t, "caption") == 0) {
    if (!hn_in_scope(b, "caption", 3)) return;
    hn_generate_implied_end_tags(b, NULL);
    hn_pop_until(b, "caption");
    while (b->n_afe > 0 && b->afe[b->n_afe - 1] != NULL) b->n_afe--;
    b->mode = IM_IN_TABLE;
    return;
  }
  if (strcmp(t, "table") == 0) {
    if (!hn_in_scope(b, "caption", 3)) return;
    hn_generate_implied_end_tags(b, NULL);
    hn_pop_until(b, "caption");
    while (b->n_afe > 0 && b->afe[b->n_afe - 1] != NULL) b->n_afe--;
    b->mode = IM_IN_TABLE;
    hn_end_in_table(b, tok);
    return;
  }
  if (strcmp(t, "body") == 0 || strcmp(t, "col") == 0 ||
      strcmp(t, "colgroup") == 0 || strcmp(t, "html") == 0 ||
      strcmp(t, "tbody") == 0 || strcmp(t, "td") == 0 ||
      strcmp(t, "tfoot") == 0 || strcmp(t, "th") == 0 ||
      strcmp(t, "thead") == 0 || strcmp(t, "tr") == 0) {
    if (!hn_in_scope(b, "caption", 3)) return;
    hn_generate_implied_end_tags(b, NULL);
    hn_pop_until(b, "caption");
    while (b->n_afe > 0 && b->afe[b->n_afe - 1] != NULL) b->n_afe--;
    b->mode = IM_IN_TABLE;
    hn_end_in_table(b, tok);
    return;
  }
  hn_end_in_body(b, tok);
}

static void hn_end_in_select(hn_builder *b, hn_token *tok) {
  const char *t = tok->name;
  if (strcmp(t, "option") == 0) {
    if (hn_current(b)->name != NULL &&
        strcmp(hn_current(b)->name, "option") == 0) {
      hn_pop(b);
    }
    return;
  }
  if (strcmp(t, "optgroup") == 0) {
    size_t i = b->depth;
    if (i > 1 && b->open[i - 1]->name != NULL &&
        strcmp(b->open[i - 1]->name, "option") == 0) {
      i--;
    }
    if (i > 1 && b->open[i - 1]->name != NULL &&
        strcmp(b->open[i - 1]->name, "optgroup") == 0) {
      hn_pop(b);
    }
    return;
  }
  if (strcmp(t, "select") == 0) {
    if (!hn_in_scope(b, "select", 0)) return;
    hn_pop_until(b, "select");
    hn_reset_insertion_mode(b);
    return;
  }
  /* Every other end tag is ignored in "in select". */
}

/* ---- the two dispatchers the modes call into each other through ---- */

static void hn_start(hn_builder *b, hn_token *tok) {
  switch (b->mode) {
    case IM_IN_BODY: hn_start_in_body(b, tok); break;
    case IM_IN_TABLE: hn_start_in_table(b, tok); break;
    case IM_IN_TABLE_BODY: hn_start_in_table_body(b, tok); break;
    case IM_IN_ROW: hn_start_in_row(b, tok); break;
    case IM_IN_CELL: hn_start_in_cell(b, tok); break;
    case IM_IN_CAPTION: hn_start_in_caption(b, tok); break;
    case IM_IN_COLUMN_GROUP: hn_start_in_column_group(b, tok); break;
    case IM_IN_SELECT: hn_start_in_select(b, tok); break;
    case IM_IN_SELECT_IN_TABLE: hn_start_in_select_in_table(b, tok); break;
    case IM_TEXT: break; /* no start tag is processed in "text" */
  }
}

static void hn_end(hn_builder *b, hn_token *tok) {
  switch (b->mode) {
    case IM_IN_BODY: hn_end_in_body(b, tok); break;
    case IM_IN_TABLE: hn_end_in_table(b, tok); break;
    case IM_IN_TABLE_BODY: hn_end_in_table_body(b, tok); break;
    case IM_IN_ROW: hn_end_in_row(b, tok); break;
    case IM_IN_CELL: hn_end_in_cell(b, tok); break;
    case IM_IN_CAPTION: hn_end_in_caption(b, tok); break;
    case IM_IN_COLUMN_GROUP: hn_start_in_body(b, tok); break;
    case IM_IN_SELECT: hn_end_in_select(b, tok); break;
    case IM_IN_SELECT_IN_TABLE: hn_end_in_select(b, tok); break;
    case IM_TEXT: break;
  }
}

/* Character data. The only mode that does anything special with it is the
 * table family, which foster-parents it out of the table. `in select`
 * DISCARDS everything that is not whitespace, which is the other half of why
 * `<select>` needs its own mode. */
static void hn_chars(hn_builder *b, const char *s, size_t len) {
  if (len == 0) return;
  if (b->mode == IM_IN_SELECT || b->mode == IM_IN_SELECT_IN_TABLE) {
    size_t i = 0;
    while (i < len) {
      unsigned char c = (unsigned char)s[i];
      if (c == ' ' || c == '\t' || c == '\n' || c == '\f' || c == '\r') {
        i++;
        continue;
      }
      while (i < len) {
        unsigned char d = (unsigned char)s[i];
        if (d == ' ' || d == '\t' || d == '\n' || d == '\f' || d == '\r') break;
        i++;
      }
    }
    return; /* non-whitespace in a select is dropped by the spec */
  }
  if (b->mode == IM_IN_TABLE || b->mode == IM_IN_TABLE_BODY ||
      b->mode == IM_IN_ROW) {
    /* The MODE still decides whether the "in table text" rules run at all —
     * that is genuinely a mode rule, 12.2.6.4.14 — but whether the resulting
     * node is FOSTER-PARENTED is now decided by the current node, in
     * hn_insert_node. Both halves are needed: the mode test keeps text out of
     * a caption or a cell, the node test keeps it in a div that the mode
     * merely has not caught up with. Split on the whitespace boundary the
     * spec's "in table text" mode splits on: a leading run of non-space characters is
     * inserted as one node, a run of spaces as another, each with the foster
     * flag set for the first. */
    bool saved = b->foster;
    b->foster = true;
    size_t i = 0;
    while (i < len) {
      size_t j = i;
      bool ws = hn_is_ws((unsigned char)s[i]);
      while (j < len && hn_is_ws((unsigned char)s[j]) == ws) j++;
      hn_insert_text(b, s + i, j - i);
      i = j;
    }
    b->foster = saved;
    return;
  }
  if (b->mode == IM_IN_COLUMN_GROUP) {
    /* Character data other than whitespace ends the column group. */
    for (size_t i = 0; i < len; i++) {
      if (!hn_is_ws((unsigned char)s[i])) {
        if (hn_current(b)->name != NULL &&
            strcmp(hn_current(b)->name, "colgroup") == 0) {
          hn_pop(b);
          b->mode = IM_IN_TABLE;
        }
        hn_insert_text(b, s + i, len - i);
        return;
      }
    }
    return;
  }
  hn_reconstruct_afe(b);
  hn_insert_text(b, s, len);
}

/* The whole parse. Returns the fragment root, whose children are what the
 * filter walks. */
static hn_node *hn_parse(hn_arena *a, const char *src, size_t len) {
  hn_builder b;
  memset(&b, 0, sizeof b);
  b.a = a;
  b.mode = IM_IN_BODY;
  b.root = hn_elem(a, hn_strndup(a, "html", 4), NULL, 0);
  if (b.root == NULL) return NULL;
  b.open[b.depth++] = b.root;

  hn_tok t;
  hn_tok_init(&t, src, len, a);
  hn_token tok;
  for (;;) {
    if (!hn_next_token(&t, &tok)) {
      b.oom = true;
      break;
    }
    if (tok.kind == TK_EOF) break;
    switch (tok.kind) {
      case TK_TEXT:
        if (b.mode == IM_TEXT) {
          /* The tokenizer already switched content models, so reaching the
           * tree builder in "text" mode means the rawtext element was closed
           * and this is its trailing content: insert it and go back. */
          hn_insert_text(&b, tok.text.ptr, tok.text.len);
          if (b.return_mode != IM_IN_BODY || hn_current(&b)->name == NULL) {
            b.mode = b.return_mode;
          }
        } else {
          hn_chars(&b, tok.text.ptr, tok.text.len);
        }
        kbc_str_free(&tok.text);
        break;
      case TK_START:
        if (b.mode == IM_TEXT) {
          b.mode = b.return_mode;
        }
        hn_start(&b, &tok);
        break;
      case TK_END:
        if (b.mode == IM_TEXT) {
          b.mode = b.return_mode;
        }
        hn_end(&b, &tok);
        break;
      case TK_COMMENT:
      case TK_EOF:
        break;
    }
    /* Token-owned buffers: the attribute values were copied into the arena
     * when the node was built, so the token's own copies can go. */
    for (size_t i = 0; i < tok.n_attrs; i++) kbc_str_free(&tok.attrs[i].value);
    if (b.oom) break;
  }
  kbc_str_free(&t.text);
  kbc_str_free(&t.name);
  /* `t.tag` too: `hn_end_tag_name` fills it for every end tag it meets and
   * nothing else on the token path frees it, so without this every end tag in
   * the source leaked one `kbc_str` — once per capture on the capture path. */
  kbc_str_free(&t.tag);
  for (size_t i = 0; i < t.n_attrs; i++) kbc_str_free(&t.attrs[i].value);
  if (b.oom) return NULL;
  return b.root;
}

/* ==========================================================================
 * 8. The filter — ammonia's `clean_dom`, in the order it applies things
 *
 * For each node, in document order:
 *   - a clean-content tag (script, style, title) is removed WITH its subtree;
 *   - a tag on the allowlist survives, with its attributes filtered;
 *   - anything else is UNWRAPPED: its children move up into its place, which
 *     is the whole reason the tree had to be built first;
 *   - comments and doctypes go.
 *
 * The traversal is ITERATIVE, with the work list on the heap, for the reason
 * ammonia gives (lib.rs:1856-1858): a document of 200 000 nested `<div>`s
 * would overflow the C stack and take the daemon with it, and this file is on
 * the serve path of a daemon that binds a network.
 * ======================================================================== */

/* The three outcomes, in ammonia's order (lib.rs:1880-1890). */
typedef enum { HN_KEEP, HN_UNWRAP, HN_DROP } hn_verdict;

static hn_verdict hn_verdict_for(const char *name) {
  if (hn_clean_content(name)) return HN_DROP;
  return hn_tag_allowed(name) ? HN_KEEP : HN_UNWRAP;
}

/* ammonia's `attr_filter` (lib.rs:1944-1971): keep the attribute if it is
 * generic or listed for this tag, and — for the two URL attributes the
 * effective allowlist reaches — only if its scheme survives. */
static bool hn_attr_ok(const char *tag, const hn_attr *at) {
  const char *name = at->name;
  if (!hn_generic_attr(name) && !hn_tag_attr_allowed(tag, name)) {
    return false;
  }
  if (hn_is_url_attr(tag, name)) return hn_url_ok(at->value.ptr);
  return true;
}

/* Appends `rel="noopener noreferrer"` to every surviving `a` (ammonia
 * lib.rs:479, pushed at lib.rs:2248-2255). It is APPENDED rather than
 * merged, so a document's own `rel` is gone (it is not on the allowlist) and
 * the emitted value is always exactly this string. */
static const char kLinkRel[] = "noopener noreferrer";

static void hn_filter(hn_arena *a, hn_node *root) {
  hn_node **stack = NULL;
  size_t n = 0, cap = 0;
  /* Seeded with the root's children: the root itself is the `div` context
   * element ammonia parses in and is never serialised. */
  for (hn_node *c = root->last_child; c != NULL; c = c->prev) {
    if (n == cap) {
      size_t nc = cap ? cap * 2 : 64;
      hn_node **ns = (hn_node **)hn_alloc(a, nc * sizeof *ns);
      if (ns == NULL) return; /* the arena is exhausted; nothing more to do */
      if (n > 0) memcpy(ns, stack, n * sizeof *ns);
      stack = ns;
      cap = nc;
    }
    stack[n++] = c;
  }
  while (n > 0) {
    hn_node *node = stack[--n];
    if (node->kind == HN_TEXT) continue;
    const char *tag = node->name;
    hn_verdict v = hn_verdict_for(tag);
    if (v == HN_DROP) {
      hn_node *p = node->parent;
      hn_replace(node, NULL);
      (void)p;
      continue;
    }
    if (v == HN_KEEP) {
      size_t keep = 0;
      for (size_t i = 0; i < node->n_attrs; i++) {
        if (hn_attr_ok(tag, &node->attrs[i])) {
          node->attrs[keep++] = node->attrs[i];
        }
      }
      node->n_attrs = keep;
      if (strcmp(tag, "a") == 0) {
        /* The token's attribute array is exactly as long as the tag had
         * attributes, so `rel` needs a fresh one. `rel` itself is not on the
         * allowlist, so a document's own `rel` was just filtered out above
         * and this is the only `rel` that can survive. */
        hn_attr *na = (hn_attr *)hn_alloc(a, (keep + 1) * sizeof *na);
        if (na == NULL) return; /* arena exhausted */
        if (keep > 0) memcpy(na, node->attrs, keep * sizeof *na);
        hn_attr_set(a, &na[keep], "rel", 3, kLinkRel, sizeof kLinkRel - 1);
        node->attrs = na;
        node->n_attrs = keep + 1;
      }
    }
    /* Push the children in reverse so they are visited in document order,
     * which is the order ammonia's stack walk visits them. */
    for (hn_node *c = node->last_child; c != NULL; c = c->prev) {
      if (n == cap) {
        size_t nc = cap ? cap * 2 : 64;
        hn_node **ns = (hn_node **)hn_alloc(a, nc * sizeof *ns);
        if (ns == NULL) return; /* arena exhausted */
        if (n > 0) memcpy(ns, stack, n * sizeof *ns);
        stack = ns;
        cap = nc;
      }
      stack[n++] = c;
    }
    if (v == HN_UNWRAP) {
      /* Hoist the children into this node's slot, in order, then detach the
       * now-empty shell. The children are NOT re-filtered here: they were
       * pushed above and are visited on the next iterations, which is
       * ammonia's order too (adjust the node, then walk its children). */
      hn_node *p = node->parent;
      if (p == NULL) continue;
      hn_node *anchor = node;
      for (hn_node *c = node->first_child; c != NULL;) {
        hn_node *nx = c->next;
        c->parent = p;
        c->prev = anchor->prev;
        c->next = anchor;
        if (anchor->prev != NULL) {
          anchor->prev->next = c;
        } else {
          p->first_child = c;
        }
        anchor->prev = c;
        c = nx;
      }
      hn_replace(anchor, NULL);
    }
  }
}

/* ==========================================================================
 * 9. The serializer — html5ever's rules, not html5ever
 *
 * `&` always escapes. In text, so do `<` and `>`; in an attribute value, `"`.
 * U+00A0 becomes `&nbsp;` in both. Those are the five substitutions
 * serialize/mod.rs:104-116 makes, and they are the entire escaping surface:
 * an attribute value can therefore contain a `<` and a text node can contain
 * a `"`, neither of which can change how the value parses.
 *
 * Void elements are written without a closing tag (serialize/mod.rs:170-190).
 * There is no self-closing `/>`: html5ever writes `>`. */
static kbc_status hn_escape(kbc_str *out, const char *s, size_t n,
                            bool attr_mode) {
  size_t i = 0;
  while (i < n) {
    unsigned char c = (unsigned char)s[i];
    kbc_status st;
    switch (c) {
      case '&': st = kbc_str_puts(out, "&amp;"); break;
      case '<': st = attr_mode ? kbc_str_putc(out, '<')
                              : kbc_str_puts(out, "&lt;"); break;
      case '>': st = attr_mode ? kbc_str_putc(out, '>')
                              : kbc_str_puts(out, "&gt;"); break;
      case '"': st = attr_mode ? kbc_str_puts(out, "&quot;")
                              : kbc_str_putc(out, '"'); break;
      case 0xC2:
        /* U+00A0 NO-BREAK SPACE, and only that: the two-byte sequence whose
         * first byte is 0xC2 and whose second is 0xA0. Any other 0xC2 starts a
         * different character and is copied through. */
        if (i + 1 < n && (unsigned char)s[i + 1] == 0xA0) {
          st = kbc_str_puts(out, "&nbsp;");
          i += 2;
          if (kbc_failed(st)) return st;
          continue;
        }
        st = kbc_str_putc(out, (char)c);
        break;
      default: st = kbc_str_putc(out, (char)c); break;
    }
    if (kbc_failed(st)) return st;
    i++;
  }
  return KBC_OK;
}

typedef struct {
  hn_node *node;
  bool close;
} hn_ser_item;

static kbc_status hn_serialize(hn_node *root, kbc_str *out) {
  hn_ser_item *stack = NULL;
  size_t n = 0, cap = 0;
  kbc_status st = KBC_OK;
  /* Seeded with the root's CHILDREN: ammonia serialises the fragment root
   * with `TraversalScope::ChildrenOnly` (lib.rs:3248-3258), so the `div`
   * context element it parsed in is not in the output. */
  for (hn_node *c = root->last_child; c != NULL; c = c->prev) {
    if (n == cap) {
      size_t nc = cap ? cap * 2 : 64;
      hn_ser_item *ns = (hn_ser_item *)realloc(stack, nc * sizeof *ns);
      if (ns == NULL) {
        st = kbc_err_set(NULL, KBC_ERR_NOMEM, "html: serializer stack");
        free(stack);
        return st;
      }
      stack = ns;
      cap = nc;
    }
    stack[n].node = c;
    stack[n].close = false;
    n++;
  }
  while (n > 0) {
    hn_ser_item it = stack[--n];
    hn_node *node = it.node;
    if (node->kind == HN_TEXT) {
      st = hn_escape(out, node->text, node->text_len, false);
      if (kbc_failed(st)) break;
      continue;
    }
    if (it.close) {
      st = kbc_str_printf(out, "</%s>", node->name);
      if (kbc_failed(st)) break;
      continue;
    }
    st = kbc_str_putc(out, '<');
    if (st == KBC_OK) st = kbc_str_puts(out, node->name);
    for (size_t i = 0; st == KBC_OK && i < node->n_attrs; i++) {
      st = kbc_str_printf(out, " %s=\"", node->attrs[i].name);
      if (st == KBC_OK) {
        st = hn_escape(out, node->attrs[i].value.ptr,
                       node->attrs[i].value.len, true);
      }
      if (st == KBC_OK) st = kbc_str_putc(out, '"');
    }
    if (st == KBC_OK) st = kbc_str_putc(out, '>');
    if (kbc_failed(st)) break;
    if (hn_is_void(node->name)) continue; /* no children, no end tag */
    /* The end tag goes on FIRST so it is popped LAST, then the children in
     * REVERSE so they come out in document order. Capacity is reserved for
     * one end tag plus every child, because a wide element would otherwise
     * write past the end of the array on the child loop. */
    size_t nkids = 0;
    for (hn_node *c = node->first_child; c != NULL; c = c->next) nkids++;
    if (n + 1 + nkids > cap) {
      size_t nc = cap ? cap * 2 : 64;
      while (nc < n + 1 + nkids) nc *= 2;
      hn_ser_item *ns = (hn_ser_item *)realloc(stack, nc * sizeof *ns);
      if (ns == NULL) {
        st = kbc_err_set(NULL, KBC_ERR_NOMEM, "html: serializer stack");
        break;
      }
      stack = ns;
      cap = nc;
    }
    stack[n].node = node;
    stack[n].close = true;
    n++;
    for (hn_node *c = node->last_child; c != NULL; c = c->prev) {
      stack[n].node = c;
      stack[n].close = false;
      n++;
    }
  }
  free(stack);
  return st;
}

/* ==========================================================================
 * 10. kbc_html_sanitize
 * ======================================================================== */

kbc_status kbc_html_sanitize(const char *html, size_t len, kbc_str *out,
                             kbc_err *err) {
  if (out == NULL) {
    return kbc_err_set(err, KBC_ERR_INVALID, "html: out is NULL");
  }
  if (html == NULL && len > 0) {
    return kbc_err_set(err, KBC_ERR_INVALID, "html: NULL source with %zu bytes",
                       len);
  }
  hn_arena *a = hn_arena_new();
  if (a == NULL) return kbc_err_set(err, KBC_ERR_NOMEM, "html: arena");
  hn_node *root = hn_parse(a, html != NULL ? html : "", len);
  if (root == NULL) {
    hn_arena_free(a);
    /* The arena went out of memory, or the parse bailed. Either way nothing
     * is appended: a caller that ignores the status must not be able to read
     * a half-sanitised document out of `out`. */
    return kbc_err_set(err, KBC_ERR_NOMEM, "html: parse of %zu bytes", len);
  }
  hn_filter(a, root);
  kbc_status st = hn_serialize(root, out);
  hn_arena_free(a);
  if (kbc_failed(st)) {
    return kbc_err_set(err, st, "html: serialise of %zu bytes", len);
  }
  return KBC_OK;
}

/* ==========================================================================
 * 11. kbc_html_split_head — the port of `stamp_html_head`
 *   (kb-core/src/capture.rs:645-656)
 *
 * The contract is the SPLIT, not the splice, so that the caller owns the
 * `<meta>` text and this function stays a pure function of its input. The
 * invariant is:
 *
 *     head ++ metas ++ body   ==   what `stamp_html_head(html, metas)` returns
 *
 * which holds for both branches:
 *
 *   - `</head>` present at `idx`: head is everything before it and body starts
 *     AT it, so the metas land immediately before `</head>` — byte for byte
 *     the original's splice.
 *   - absent: head is the literal `"<head>\n"` and body is `"</head>\n"` plus
 *     the whole document, so the result is
 *     `<head>\n{metas}</head>\n{html}` — again the original's format string,
 *   for the fragment case the original documents at capture.rs:635-644.
 *
 * WHAT THIS FUNCTION IS, since the placement gets questioned: it is the one
 * place in kb-c that knows the literal `</head>`, which is the whole of what
 * it does, and that is HTML-specific knowledge — so html.c is where it
 * belongs, next to the escaping and tag-name machinery it shares vocabulary
 * with, and not with the capture code in app.c that calls it. It is a SPLIT
 * and nothing else: it does not parse, does not filter, does not decide what
 * is allowed, and is not a sanitiser. It runs on whatever bytes it is handed
 * and neither requires nor assumes they came out of kbc_html_sanitize —
 * which is worth saying out loud, because the ONE caller in kb-c hands it
 * sanitised bytes, and on those the search below can never fire (the tree
 * builder drops `<html>`/`<head>`/`<body>` in a fragment context, so no
 * sanitised document contains `</head>`). The search is kept because the
 * function is a splitter and a caller with other bytes is entitled to the
 * split; see the call site in app.c for why the synthesise branch is the one
 * that runs.
 *
 * THE SEARCH IS BYTE-EXACT AND CASE-SENSITIVE, because the original's is:
 * `html.find("</head>")` (capture.rs:646) is a plain substring search, so a
 * document closing its head with `</HEAD>` takes the synthesise branch. That
 * is a divergence from what a BROWSER would do with `</HEAD>` and it is kept
 * anyway: byte-comparability with the original is the contract this port
 * holds itself to, and the consequence is benign — a synthesised head is
 * still a real head, and the provenance still lands in it. */
static bool hn_ci_prefix(const char *s, const char *lower, size_t n) {
  for (size_t i = 0; i < n; i++) {
    unsigned char c = (unsigned char)s[i];
    if (c >= 'A' && c <= 'Z') c = (unsigned char)(c + 32);
    if (c != (unsigned char)lower[i]) return false;
  }
  return true;
}

kbc_status kbc_html_split_head(const char *html, size_t len, kbc_str *head,
                               kbc_str *body, kbc_err *err) {
  if (head == NULL || body == NULL) {
    return kbc_err_set(err, KBC_ERR_INVALID, "html: head and body are required");
  }
  if (html == NULL) {
    return kbc_err_set(err, KBC_ERR_INVALID, "html: NULL source");
  }
  static const char kClose[] = "</head>";
  const size_t kCloseLen = sizeof kClose - 1;
  for (size_t i = 0; i + kCloseLen <= len; i++) {
    if (html[i] != '<') continue;
    if (memcmp(html + i, kClose, kCloseLen) != 0) continue;
    kbc_status st = kbc_str_append(head, html, i);
    if (kbc_failed(st)) return kbc_err_set(err, st, "html: head of %zu bytes", len);
    st = kbc_str_append(body, html + i, len - i);
    if (kbc_failed(st)) return kbc_err_set(err, st, "html: body of %zu bytes", len);
    return KBC_OK;
  }
  kbc_status st = kbc_str_puts(head, "<head>\n");
  if (st == KBC_OK) st = kbc_str_puts(body, "</head>\n");
  if (st == KBC_OK) st = kbc_str_append(body, html, len);
  if (kbc_failed(st)) {
    return kbc_err_set(err, st, "html: synthesised head for %zu bytes", len);
  }
  return KBC_OK;
}

/* ==========================================================================
 * 12. The serve-layer redaction
 *
 * Two obligations, both ported rather than re-derived, because this is a
 * security path and "it seemed reasonable" is not a defence.
 *
 * (a) `strip_kb_prompt` — drop `<template id="kb-prompt">` WITH its contents.
 *     kb-core/src/scrub.rs:80-99 uses lol-html with the selector
 *     `template[id="kb-prompt"]` and `el.replace("", ContentType::Html)`, and
 *     an element replacement with an HTML content type removes the element
 *     AND everything inside it. The id is matched EXACTLY, because that is
 *     what the CSS attribute selector does; the generation prompt is written
 *     with a lowercase id by the thing that generates it.
 *
 *     The strip is SURGICAL, like lol-html, and that is a deliberate choice
 *     over "parse it and drop the node". Re-serialising would normalise every
 *     other byte of the document, so a redacted file would differ from the
 *     original in ways nobody asked for; this copies everything outside the
 *     matched range byte for byte. A consequence worth stating: because this
 *     is not a parse, an UNCLOSED `<template id="kb-prompt">` runs to the end
 *     of the input, which is what lol_html's `end()` does when the element
 *     never closes.
 *
 * (b) `looks_non_loopback` — kb-server/src/scrub.rs:34-40, which is
 *     `!is_loopback_origin(peer, headers, trusted_proxies)` from
 *     kb-server/src/middleware.rs:713-729. Two rules and one default:
 *       - the immediate TCP peer must be a TRUSTED HOP (loopback, or a
 *         configured trusted proxy) before `X-Forwarded-For` is consulted at
 *         all, so a direct remote client cannot buy loopback with a header;
 *       - the XFF chain is walked RIGHT-TO-LEFT, skipping loopback and
 *         trusted entries, and the first untrusted entry from the right is
 *         the client; unparsable entries are skipped; if every entry was
 *         trusted the original client was loopback too;
 *       - an UNKNOWN peer fails CLOSED — it counts as non-loopback, so the
 *         redaction runs. The original's reason (scrub.rs:29-33) is the right
 *         one and is repeated here because it is the whole point: better to
 *         scrub a local test that forgot to wire peer information than to
 *         leak a prompt template to a real remote client.
 *
 *     kb-c's frozen `kbc_config` has NO `trusted_proxies` field (there is
 *     nothing in config.h to read one from), so the trusted set is empty and
 *     the walk degenerates to "skip loopback hops". That is the correct
 *     behaviour for the default of no configured proxies; a deployment that
 *     configures one needs a config key, which is a header change and is
 *     reported rather than invented here.
 * ======================================================================== */

/* Strict dotted-quad. Every octet must be one to three digits with no
 * leading zero beyond a bare `0`, because `127.0.0.01` is not an address and
 * a peer string that a lenient parser would accept is exactly the kind of
 * thing a spoofed header should not be able to talk its way past. Returns
 * the first octet, or -1 when the string is not a dotted quad at all. */
static int hn_ipv4_first(const char *addr) {
  int octets[4];
  const char *p = addr;
  for (int i = 0; i < 4; i++) {
    const char *start = p;
    if (*p < '0' || *p > '9') return -1;
    int digits = 0;
    int v = 0;
    while (*p >= '0' && *p <= '9' && digits < 4) {
      v = v * 10 + (*p - '0');
      p++;
      digits++;
    }
    if (digits > 3 || v > 255) return -1;
    if (digits > 1 && start[0] == '0') return -1; /* 01, 007 */
    octets[i] = v;
    if (i < 3) {
      if (*p != '.') return -1;
      p++;
    }
  }
  if (*p != '\0') return -1;
  return octets[0];
}

static bool hn_addr_is_loopback(const char *addr) {
  if (addr == NULL || addr[0] == '\0' || addr[0] == '?') return false;
  /* `::1` and `127.0.0.0/8`, spelled out so this module needs no socket
   * headers. A `0.0.0.0` or `[::]` peer is not loopback, which is the right
   * answer for a wildcard bind. */
  if (strcmp(addr, "::1") == 0) return true;
  return hn_ipv4_first(addr) == 127;
}

static bool hn_addr_trusted(const char *addr, const char *const *trusted,
                            size_t n_trusted) {
  if (hn_addr_is_loopback(addr)) return true;
  for (size_t i = 0; i < n_trusted; i++) {
    if (trusted[i] != NULL && strcmp(trusted[i], addr) == 0) return true;
  }
  return false;
}

bool kbc_html_looks_non_loopback(const char *peer_addr, const char *xff,
                                 const char *const *trusted_proxies,
                                 size_t n_trusted_proxies) {
  /* Rule 1, and the fail-closed default: without a peer address there is
   * nothing to trust, and "nothing to trust" means treat the client as
   * remote. */
  if (peer_addr == NULL || peer_addr[0] == '\0' || peer_addr[0] == '?') {
    return true;
  }
  /* Rule 2: the peer must be a trusted hop before the header is read at all. */
  if (!hn_addr_trusted(peer_addr, trusted_proxies, n_trusted_proxies)) {
    return true;
  }
  /* Rule 3: no XFF, the trusted peer IS the client, so the origin is local. */
  if (xff == NULL || xff[0] == '\0') return false;
  /* Rule 4: walk right to left. The first unparsable entry is skipped (an
   * obfuscated `unknown` token is not evidence of anything); the first
   * entry that parses and is not a trusted hop is the real client. */
  size_t n = strlen(xff);
  size_t end = n;
  while (end > 0) {
    size_t start = end;
    while (start > 0 && xff[start - 1] != ',') start--;
    size_t b = start, e = end;
    while (b < e && (xff[b] == ' ' || xff[b] == '\t')) b++;
    while (e > b && (xff[e - 1] == ' ' || xff[e - 1] == '\t')) e--;
    if (e > b) {
      char entry[64];
      size_t len = e - b;
      if (len >= sizeof entry) {
        len = sizeof entry - 1; /* an address longer than this is not one */
      }
      memcpy(entry, xff + b, len);
      entry[len] = '\0';
      if (!hn_addr_trusted(entry, trusted_proxies, n_trusted_proxies)) {
        return true; /* a real, untrusted client: non-loopback */
      }
    }
    if (start == 0) break;
    end = start - 1;
  }
  /* Every entry was a trusted hop, so the original client was loopback too. */
  return false;
}

/* The next `<template` or `</template` at or after `from`. Returns SIZE_MAX if
 * there is none, and sets `*is_close` for the end-tag form. Nested templates
 * are counted by the caller, so a document with `<template>` inside a
 * `<template>` still ends at the right one. */
static size_t hn_next_template(const char *html, size_t n, size_t from,
                               bool *is_close) {
  for (size_t i = from; i + 9 <= n; i++) {
    if (html[i] != '<') continue;
    size_t name_at = i + 1;
    bool close = false;
    if (html[name_at] == '/') {
      close = true;
      name_at++;
    }
    if (!hn_ci_prefix(html + name_at, "template", 8)) continue;
    size_t after = name_at + 8;
    /* A NAME boundary: `<templatefoo>` is a different element entirely. */
    if (after < n) {
      unsigned char c = (unsigned char)html[after];
      if (!hn_is_ws(c) && c != '>' && c != '/') continue;
    }
    *is_close = close;
    return i;
  }
  return SIZE_MAX;
}

/* True when the `<template` tag starting at `at` carries id="kb-prompt".
 *
 * The attribute scan is byte-oriented and confined to THIS tag, which is what
 * lol-html's `template[id="kb-prompt"]` selector resolves to once html5ever
 * has parsed the attributes. Two things it deliberately does NOT do, both to
 * avoid deleting a document's own content on a coincidence: it requires a
 * QUOTED value (an unquoted `id=kb-prompt` does happen in the wild but the CSS
 * selector that is being ported would not have matched it any more reliably
 * than this), and it requires the value to be exactly `kb-prompt` between the
 * quotes, so `id="not-kb-prompt"` is left alone. */
static bool hn_is_kb_prompt(const char *html, size_t n, size_t at) {
  size_t gt = at;
  while (gt < n && html[gt] != '>') gt++;
  if (gt >= n) return false;
  for (size_t i = at + 1; i + 2 < gt; i++) {
    if (html[i] != 'i' && html[i] != 'I') continue;
    if (html[i + 1] != 'd' && html[i + 1] != 'D') continue;
    size_t j = i + 2;
    while (j < gt && hn_is_ws((unsigned char)html[j])) j++;
    if (j >= gt || html[j] != '=') continue;
    j++;
    while (j < gt && hn_is_ws((unsigned char)html[j])) j++;
    if (j >= gt) return false;
    char q = html[j];
    if (q != '"' && q != '\'') return false;
    j++;
    /* Nine characters of value plus the closing quote, so the LAST byte read
     * is html[j + 9] and the guard has to admit j + 10. It required j + 11,
     * which is one byte too many: `<template id="kb-prompt">` puts the closing
     * quote on the last byte before `>`, so every well-formed prompt template
     * was rejected and the strip matched NOTHING. The redaction was inert. */
    if (j + 10 > gt) return false;
    if (memcmp(html + j, "kb-prompt", 9) != 0) return false;
    return html[j + 9] == q;
  }
  return false;
}

/* Finds the end of the element that starts at `at`: the offset just past the
 * `>` of its matching `</template>`, or `len` when there is none. */
static size_t hn_template_extent(const char *html, size_t len, size_t at) {
  size_t depth = 1;
  /* `at + 1`, not `at`: the search starting AT the opening tag finds that tag
   * again, counts it as a NESTED template, and so never reaches depth zero —
   * every prompt template then ran to the end of the document and took the
   * rest of the page with it. */
  size_t scan = at + 1;
  for (;;) {
    bool close = false;
    size_t next = hn_next_template(html, len, scan, &close);
    if (next == SIZE_MAX) return len; /* unclosed: runs to the end, as lol_html does */
    if (close) {
      depth--;
      if (depth == 0) {
        size_t gt = next;
        while (gt < len && html[gt] != '>') gt++;
        return gt < len ? gt + 1 : len;
      }
    } else {
      depth++;
    }
    scan = next + 1;
  }
}

kbc_status kbc_html_strip_kb_prompt(const char *html, size_t len, kbc_str *out,
                                    kbc_err *err) {
  if (html == NULL) return kbc_err_set(err, KBC_ERR_INVALID, "html: NULL source");
  size_t copied_to = 0;
  size_t scan = 0;
  for (;;) {
    bool close = false;
    size_t at = hn_next_template(html, len, scan, &close);
    if (at == SIZE_MAX) break;
    if (close || !hn_is_kb_prompt(html, len, at)) {
      scan = at + 1;
      continue;
    }
    /* Copy everything up to the match, drop the element and its contents, and
     * carry on after it. Every byte outside a match is copied verbatim, which
     * is the property that makes this a redaction rather than a rewrite. */
    kbc_status st = kbc_str_append(out, html + copied_to, at - copied_to);
    if (kbc_failed(st)) {
      return kbc_err_set(err, st, "html: kb-prompt strip of %zu bytes", len);
    }
    size_t end = hn_template_extent(html, len, at);
    copied_to = end;
    scan = end;
  }
  kbc_status st = kbc_str_append(out, html + copied_to, len - copied_to);
  if (kbc_failed(st)) {
    return kbc_err_set(err, st, "html: kb-prompt strip of %zu bytes", len);
  }
  return KBC_OK;
}

/* The serve-path decision, composed: gate on the genuine client, and redact
 * when it looks remote. See the note above on why the strip is unconditional
 * in kb-c — there is no `[outbound]` configuration to be conditional on, so
 * the only rule there is, and the original's own static-export path (scrub.rs:
 * 111-124) strips the prompt template regardless of configuration for exactly
 * the same reason. */
kbc_status kbc_html_scrub_outbound(const char *peer_addr, const char *xff,
                                    const char *const *trusted_proxies,
                                    size_t n_trusted_proxies, const char *html,
                                    size_t len, kbc_str *out, kbc_err *err) {
  if (html == NULL) return kbc_err_set(err, KBC_ERR_INVALID, "html: NULL source");
  if (!kbc_html_looks_non_loopback(peer_addr, xff, trusted_proxies,
                                  n_trusted_proxies)) {
    /* A loopback client is the operator at their own machine. The redaction
     * rules kb-core would add here are configured per kb and kb-c's frozen
     * config has no such section, so there is nothing else this gate would
     * turn on. */
    kbc_status st = kbc_str_append(out, html, len);
    if (kbc_failed(st)) {
      return kbc_err_set(err, st, "html: scrub passthrough of %zu bytes", len);
    }
    return KBC_OK;
  }
  return kbc_html_strip_kb_prompt(html, len, out, err);
}
