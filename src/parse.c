/* parse.c — the reader parse (Markdown/HTML -> blocks + anchors) and the
 * tokenizer.
 *
 * This is a streaming state machine over the caller's buffer: every byte is
 * visited a bounded number of times, nothing is copied until a block closes.
 * It is deliberately not a CommonMark renderer — see include/kbc/parse.h.
 */
#include <stdio.h>
#include <string.h>

#include "kbc/parse.h"

/* ------------------------------------------------------------- charset -- */

static bool is_space(unsigned char c) {
  return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' ||
         c == '\v';
}

static bool is_digit(unsigned char c) { return c >= '0' && c <= '9'; }

static bool is_alpha(unsigned char c) {
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}

static bool is_alnum(unsigned char c) {
  return is_alpha(c) || is_digit(c);
}

static char lower(char c) {
  return (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
}

static bool ci_prefix(const char *s, size_t n, size_t i, const char *lit) {
  size_t l = strlen(lit);
  if (i + l > n) return false;
  for (size_t k = 0; k < l; k++) {
    if (lower(s[i + k]) != lower(lit[k])) return false;
  }
  return true;
}

static size_t ci_find(const char *s, size_t n, size_t from, const char *lit) {
  size_t l = strlen(lit);
  if (l == 0 || l > n) return n;
  for (size_t i = from; i + l <= n; i++) {
    if (ci_prefix(s, n, i, lit)) return i;
  }
  return n;
}

/* ------------------------------------------------------------- entities -- */

static kbc_status put_codepoint(kbc_str *out, uint32_t cp) {
  char b[4];
  size_t n;
  if (cp < 0x80u) {
    b[0] = (char)cp;
    n = 1;
  } else if (cp < 0x800u) {
    b[0] = (char)(0xC0u | (cp >> 6));
    b[1] = (char)(0x80u | (cp & 0x3Fu));
    n = 2;
  } else if (cp < 0x10000u) {
    b[0] = (char)(0xE0u | (cp >> 12));
    b[1] = (char)(0x80u | ((cp >> 6) & 0x3Fu));
    b[2] = (char)(0x80u | (cp & 0x3Fu));
    n = 3;
  } else {
    b[0] = (char)(0xF0u | (cp >> 18));
    b[1] = (char)(0x80u | ((cp >> 12) & 0x3Fu));
    b[2] = (char)(0x80u | ((cp >> 6) & 0x3Fu));
    b[3] = (char)(0x80u | (cp & 0x3Fu));
    n = 4;
  }
  return kbc_str_append(out, b, n);
}

/* Decodes the entity at s[i] (which is '&') into `out`, returning the number
 * of source bytes consumed. An unrecognised entity is left verbatim: the '&'
 * is emitted and 1 is consumed. Never fails silently on a half entity. */
static kbc_status decode_entity(kbc_str *out, const char *s, size_t n,
                                size_t i, size_t *consumed) {
  static const struct {
    const char *name;
    const char *rep;
  } named[] = {
      {"amp", "&"},   {"lt", "<"},     {"gt", ">"},      {"quot", "\""},
      {"apos", "'"},  {"nbsp", " "},   {"hellip", "..."}, {"mdash", "-"},
      {"ndash", "-"}, {"lsquo", "'"},  {"rsquo", "'"},   {"ldquo", "\""},
      {"rdquo", "\""}, {"copy", "\xc2\xa9"}, {"reg", "\xc2\xae"},
      {"trade", "\xe2\x84\xa2"}, {"deg", "\xc2\xb0"},
      {"plusmn", "\xc2\xb1"}, {"times", "\xc3\x97"},
      {"middot", "\xc2\xb7"}, {"bull", "\xe2\x80\xa2"},
      {"laquo", "\xc2\xab"}, {"raquo", "\xc2\xbb"},
  };
  size_t j = i + 1;
  if (j >= n) {
    kbc_status st = kbc_str_append(out, "&", 1);
    *consumed = 1;
    return st;
  }
  if (s[j] == '#') {
    size_t k = j + 1;
    uint32_t v = 0;
    size_t digits = 0;
    bool hex = (k < n) && (s[k] == 'x' || s[k] == 'X');
    if (hex) k++;
    while (k < n && digits < 7) {
      char c = s[k];
      unsigned d;
      if (is_digit((unsigned char)c)) {
        d = (unsigned)(c - '0');
      } else if (hex && c >= 'a' && c <= 'f') {
        d = (unsigned)(c - 'a' + 10);
      } else if (hex && c >= 'A' && c <= 'F') {
        d = (unsigned)(c - 'A' + 10);
      } else {
        break;
      }
      v = v * (hex ? 16u : 10u) + d;
      k++;
      digits++;
    }
    if (digits > 0 && k < n && s[k] == ';' && v > 0 && v <= 0x10FFFFu) {
      kbc_status st = put_codepoint(out, v);
      *consumed = (k + 1) - i;
      return st;
    }
    kbc_status st = kbc_str_append(out, "&", 1);
    *consumed = 1;
    return st;
  }
  if (is_alpha((unsigned char)s[j])) {
    size_t k = j;
    while (k < n && is_alnum((unsigned char)s[k])) k++;
    if (k < n && s[k] == ';') {
      size_t l = k - j;
      for (size_t e = 0; e < sizeof(named) / sizeof(named[0]); e++) {
        if (strlen(named[e].name) == l &&
            memcmp(named[e].name, s + j, l) == 0) {
          kbc_status st = kbc_str_puts(out, named[e].rep);
          *consumed = (k + 1) - i;
          return st;
        }
      }
    }
  }
  kbc_status st = kbc_str_append(out, "&", 1);
  *consumed = 1;
  return st;
}

/* ---------------------------------------------------------------- slug -- */

static bool slug_byte_ok(unsigned char c) {
  return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9');
}

static kbc_status slug_into(kbc_arena *a, const char *text, size_t len,
                            kbc_str *out) {
  bool pending_dash = false;
  bool any = false;
  size_t i = 0;
  while (i < len) {
    unsigned char c = (unsigned char)text[i];
    size_t seqlen = 1;
    if (c >= 0x80) {
      size_t l = kbc_utf8_len(c);
      if (l > 1 && i + l <= len) seqlen = l;
    }
    char folded[32];
    size_t fl = 0;
    if (seqlen > 1) {
      fl = kbc_fold_utf8(text + i, seqlen, folded);
      if (fl == 0) {
        i += 1;
        continue;
      }
    }
    for (size_t k = 0; k < (fl ? fl : 1); k++) {
      unsigned char d = fl ? (unsigned char)folded[k] : lower((char)c);
      if (slug_byte_ok(d)) {
        if (pending_dash && any) {
          kbc_status st = kbc_str_putc(out, '-');
          if (kbc_failed(st)) return st;
        }
        pending_dash = false;
        any = true;
        kbc_status st = kbc_str_putc(out, (char)d);
        if (kbc_failed(st)) return st;
      } else {
        pending_dash = true;
      }
    }
    i += seqlen;
  }
  (void)a;
  return KBC_OK;
}

kbc_status kbc_slugify(kbc_arena *a, const char *text, size_t len,
                       kbc_str *out, kbc_err *err) {
  if (a == NULL) {
    kbc_err_set(err, KBC_ERR_INVALID, "slugify: arena is NULL");
    return KBC_ERR_INVALID;
  }
  if (text == NULL) {
    kbc_err_set(err, KBC_ERR_INVALID, "slugify: text is NULL");
    return KBC_ERR_INVALID;
  }
  if (out == NULL) {
    kbc_err_set(err, KBC_ERR_INVALID, "slugify: out is NULL");
    return KBC_ERR_INVALID;
  }
  if (len > KBC_MAX_ARTIFACT_BYTES) {
    kbc_err_set(err, KBC_ERR_INVALID,
                "slugify: length %zu exceeds the %u byte ceiling", len,
                (unsigned)KBC_MAX_ARTIFACT_BYTES);
    return KBC_ERR_INVALID;
  }
  return slug_into(a, text, len, out);
}

/* ------------------------------------------------------------- records -- */

struct kbc_parsed {
  kbc_blocks blocks;  /* KBC_ARENA */
  char **anchors;     /* KBC_ARENA, NUL-terminated entries */
  size_t anchors_len;
  size_t anchors_cap;
  const char *title;  /* KBC_ARENA */
};

typedef struct {
  kbc_arena *a;
  kbc_err *err;
  struct kbc_parsed *out;

  kbc_str cur;         /* text of the block under construction */
  size_t cur_off;      /* source offset of that block's first byte */
  size_t cur_line_beg; /* length in `cur` at the start of the last line */
  int cur_level;       /* 0 = prose, 1..6 = heading */
  bool cur_code;       /* fenced code: the id is code-<lang>-<n> */
  const char *cur_id;  /* explicit id for the block being built */
  bool pending;        /* a run of whitespace is pending -> one space */

  bool in_fence;
  char fence_ch;
  size_t fence_len;
  size_t code_n;

  bool in_title; /* inside <title>...</title> */
  kbc_str title_buf;

  const char *title_h1;    /* first h1 block, markdown or HTML */
  const char *title_tag;   /* first <title> element */
  const char *title_prose; /* first non-empty prose block */
} parser;

static kbc_status p_flush(parser *p);

/* ---------------------------------------------------------- arena grow -- */

static kbc_status arena_grow(void ***items, size_t *cap, size_t need,
                             size_t elem, kbc_arena *a) {
  if (need <= *cap) return KBC_OK;
  size_t ncap = (*cap == 0) ? 8 : *cap * 2;
  if (ncap < need) ncap = need;
  if (ncap > SIZE_MAX / elem) return KBC_ERR_NOMEM;
  void *fresh = kbc_arena_calloc(a, ncap, elem);
  if (fresh == NULL) return KBC_ERR_NOMEM;
  if (*items != NULL && *cap > 0) {
    memcpy(fresh, *items, *cap * elem);
  }
  *items = fresh;
  *cap = ncap;
  return KBC_OK;
}

/* ------------------------------------------------------------- parsed --- */


const kbc_blocks *kbc_parsed_blocks(const kbc_parsed *p) {
  return p == NULL ? NULL : &p->blocks;
}

const char *kbc_parsed_title(const kbc_parsed *p) {
  return p == NULL ? NULL : p->title;
}

bool kbc_parsed_has_anchor(const kbc_parsed *p, const char *id) {
  if (p == NULL || id == NULL) return false;
  for (size_t i = 0; i < p->anchors_len; i++) {
    if (strcmp(p->anchors[i], id) == 0) return true;
  }
  return false;
}


/* ------------------------------------------------------------- parser --- */


static kbc_status p_put(parser *p, const char *d, size_t n) {
  if (n == 0) return KBC_OK;
  kbc_status st;
  if (p->pending) {
    p->pending = false;
    st = kbc_str_putc(&p->cur, ' ');
    if (kbc_failed(st)) return st;
  }
  st = kbc_str_append(&p->cur, d, n);
  if (kbc_failed(st)) return st;
  if (p->in_title) {
    st = kbc_str_append(&p->title_buf, d, n);
    if (kbc_failed(st)) return st;
  }
  return KBC_OK;
}

static kbc_status p_put_char(parser *p, unsigned char c) {
  if (is_space(c)) {
    if (p->cur.len > 0) p->pending = true;
    return KBC_OK;
  }
  return p_put(p, (const char *)&c, 1);
}

static kbc_status parsed_add_anchor(parser *p, const char *id) {
  if (id == NULL || id[0] == '\0') return KBC_OK;
  for (size_t i = 0; i < p->out->anchors_len; i++) {
    if (strcmp(p->out->anchors[i], id) == 0) return KBC_OK;
  }
  kbc_status st = arena_grow((void ***)&p->out->anchors, &p->out->anchors_cap,
                             p->out->anchors_len + 1, sizeof(char *), p->a);
  if (kbc_failed(st)) return st;
  p->out->anchors[p->out->anchors_len++] = kbc_arena_strdup(p->a, id);
  return KBC_OK;
}

static kbc_status p_flush(parser *p) {
  p->pending = false;
  if (p->cur.len == 0) {
    p->cur_level = 0;
    p->cur_id = NULL;
    p->cur_code = false;
    p->cur_line_beg = 0;
    return KBC_OK;
  }
  char *text = kbc_arena_strndup(p->a, p->cur.ptr, p->cur.len);
  char *id = NULL;
  if (p->cur_id != NULL) {
    id = kbc_arena_strdup(p->a, p->cur_id);
  } else if (p->cur_level > 0) {
    kbc_str sl;
    kbc_str_init(&sl);
    kbc_status st = slug_into(p->a, text, strlen(text), &sl);
    if (kbc_failed(st)) {
      kbc_str_free(&sl);
      return st;
    }
    id = kbc_arena_strdup(p->a, sl.ptr);
    kbc_str_free(&sl);
  }
  if (id == NULL || id[0] == '\0') {
    if (id != NULL) {
      /* Heading text with no slug-able bytes still needs a stable id. */
      kbc_str sl;
      kbc_str_init(&sl);
      kbc_status st = kbc_str_printf(&sl, "b%zu", p->out->blocks.len);
      if (kbc_failed(st)) {
        kbc_str_free(&sl);
        return st;
      }
      id = kbc_arena_strdup(p->a, sl.ptr);
      kbc_str_free(&sl);
    } else {
      id = kbc_arena_printf(p->a, "b%zu", p->out->blocks.len);
    }
  }
  kbc_status st = arena_grow((void ***)&p->out->blocks.items, &p->out->blocks.cap,
                             p->out->blocks.len + 1, sizeof(kbc_block), p->a);
  if (kbc_failed(st)) return st;
  kbc_block *b = &p->out->blocks.items[p->out->blocks.len++];
  b->id = id;
  b->text = text;
  b->text_len = strlen(text);
  b->heading_level = p->cur_level;
  b->offset = (uint32_t)(p->cur_off > UINT32_MAX ? 0 : p->cur_off);

  if (p->cur_level == 1 && p->title_h1 == NULL) p->title_h1 = text;
  if (p->cur_level == 0 && p->title_prose == NULL && !p->cur_code) {
    p->title_prose = text;
  }
  if (p->cur_level > 0 || p->cur_code) {
    st = parsed_add_anchor(p, id);
    if (kbc_failed(st)) return st;
  }
  kbc_str_clear(&p->cur);
  p->cur_level = 0;
  p->cur_id = NULL;
  p->cur_code = false;
  p->cur_line_beg = 0;
  return KBC_OK;
}

/* ---------------------------------------------------------- html tags --- */

static bool tag_name_char(unsigned char c) {
  return is_alnum(c) || c == '-' || c == '_' || c == ':';
}

static bool block_tag(const char *name, size_t len, int *level) {
  static const char *const blocks[] = {
      "p",     "div",  "li",       "ul",      "ol",     "pre",
      "blockquote", "section", "article", "table",  "tr",     "td",
      "th",    "thead", "tbody",   "tfoot",   "header", "footer",
      "main",  "nav",  "aside",    "figure",  "figcaption", "dl",
      "dt",    "dd",   "hgroup",   "hr",
  };
  *level = 0;
  if (len == 2 && name[0] == 'h' && name[1] >= '1' && name[1] <= '6') {
    *level = name[1] - '0';
    return true;
  }
  for (size_t i = 0; i < sizeof(blocks) / sizeof(blocks[0]); i++) {
    if (strlen(blocks[i]) == len && memcmp(blocks[i], name, len) == 0) {
      return true;
    }
  }
  return false;
}

static bool name_eq(const char *lower_name, size_t len, const char *lit) {
  return strlen(lit) == len && memcmp(lower_name, lit, len) == 0;
}

static size_t tag_end(const char *s, size_t n, size_t i) {
  while (i < n) {
    if (s[i] == '"' || s[i] == '\'') {
      char q = s[i++];
      while (i < n && s[i] != q) i++;
      if (i < n) i++;
      continue;
    }
    if (s[i] == '>') return i;
    i++;
  }
  return n;
}

static kbc_status p_tag(parser *p, const char *s, size_t n, size_t *pos) {
  size_t start = *pos;
  size_t i = start + 1;
  if (i >= n) { /* a trailing bare '<' */
    *pos = n;
    return p_put_char(p, '<');
  }
  unsigned char c = (unsigned char)s[i];
  if (c == '!') {
    if (ci_prefix(s, n, i, "!--")) {
      size_t e = ci_find(s, n, i + 3, "-->");
      *pos = (e >= n) ? n : e + 3;
      return KBC_OK;
    }
    *pos = tag_end(s, n, i) + 1;
    if (*pos > n) *pos = n;
    return KBC_OK;
  }
  if (c == '?') {
    *pos = tag_end(s, n, i) + 1;
    if (*pos > n) *pos = n;
    return KBC_OK;
  }
  bool closing = false;
  if (c == '/') {
    closing = true;
    i++;
    if (i >= n) {
      *pos = n;
      return p_put_char(p, '<');
    }
    c = (unsigned char)s[i];
  }
  if (!is_alpha(c)) {
    *pos = start + 1;
    return p_put_char(p, '<');
  }

  size_t ns = i;
  while (i < n && tag_name_char((unsigned char)s[i])) i++;
  size_t nlen = i - ns;
  char name[16];
  size_t namelen = nlen < sizeof(name) ? nlen : sizeof(name) - 1;
  for (size_t k = 0; k < namelen; k++) name[k] = lower(s[ns + k]);
  name[namelen] = '\0';

  bool self_closing = false;
  while (i < n && s[i] != '>') {
    if (is_space((unsigned char)s[i]) || s[i] == '/') {
      if (s[i] == '/' && i + 1 < n && s[i + 1] == '>') self_closing = true;
      i++;
      continue;
    }
    size_t as = i;
    while (i < n && s[i] != '=' && s[i] != '>' && !is_space((unsigned char)s[i]) &&
           s[i] != '/') {
      i++;
    }
    size_t alen = i - as;
    size_t v = i;
    while (v < n && is_space((unsigned char)s[v])) v++;
    const char *val = NULL;
    size_t vlen = 0;
    if (v < n && s[v] == '=') {
      v++;
      while (v < n && is_space((unsigned char)s[v])) v++;
      if (v < n && (s[v] == '"' || s[v] == '\'')) {
        char q = s[v++];
        size_t vs = v;
        while (v < n && s[v] != q) v++;
        val = s + vs;
        vlen = v - vs;
        if (v < n) v++;
      } else {
        size_t vs = v;
        while (v < n && !is_space((unsigned char)s[v]) && s[v] != '>') v++;
        val = s + vs;
        vlen = v - vs;
      }
      i = v;
    }
    if (alen == 2 && lower(s[as]) == 'i' && lower(s[as + 1]) == 'd' && val != NULL) {
      kbc_str id;
      kbc_str_init(&id);
      size_t k = 0;
      kbc_status st = KBC_OK;
      while (k < vlen) {
        if (val[k] == '&') {
          size_t used = 0;
          st = decode_entity(&id, val, vlen, k, &used);
          if (kbc_failed(st)) break;
          k += used;
        } else {
          st = kbc_str_putc(&id, val[k]);
          if (kbc_failed(st)) break;
          k++;
        }
      }
      if (!kbc_failed(st)) st = parsed_add_anchor(p, id.ptr);
      kbc_str_free(&id);
      if (kbc_failed(st)) return st;
    }
  }
  *pos = (i < n) ? i + 1 : n;

  bool is_title = name_eq(name, namelen, "title");
  int level = 0;
  bool block = block_tag(name, namelen, &level);

  if (!closing && (name_eq(name, namelen, "script") ||
                   name_eq(name, namelen, "style")) &&
      !self_closing) {
    size_t close = ci_find(s, n, *pos, "</");
    if (close >= n) {
      *pos = n;
    } else {
      *pos = tag_end(s, n, close) + 1;
      if (*pos > n) *pos = n;
    }
    return KBC_OK;
  }
  if (is_title) {
    if (closing) {
      if (p->in_title && p->title_tag == NULL && p->title_buf.len > 0) {
        p->title_tag = kbc_arena_strndup(p->a, p->title_buf.ptr, p->title_buf.len);
      }
      p->in_title = false;
      kbc_str_clear(&p->title_buf);
    } else {
      kbc_status st = p_flush(p);
      if (kbc_failed(st)) return st;
      p->cur_off = start;
      p->in_title = true;
      kbc_str_clear(&p->title_buf);
    }
    return KBC_OK;
  }
  if (!block) {
    if (name_eq(name, namelen, "br")) return p_put_char(p, ' ');
    return KBC_OK;
  }
  if (self_closing) {
    kbc_status st0 = p_flush(p);
    if (kbc_failed(st0)) return st0;
    return KBC_OK;
  }
  kbc_status st = p_flush(p);
  if (kbc_failed(st)) return st;
  if (!closing) {
    p->cur_off = start;
    p->cur_level = level;
  }
  return KBC_OK;
}

/* ------------------------------------------------------ line constructs -- */

static size_t line_end(const char *s, size_t n, size_t i) {
  while (i < n && s[i] != '\n') i++;
  return i;
}

static kbc_status p_open_code(parser *p, const char *info, size_t info_len,
                              size_t off) {
  kbc_status st = p_flush(p);
  if (kbc_failed(st)) return st;
  char lang[32];
  size_t ll = 0;
  size_t i = 0;
  while (i < info_len && is_space((unsigned char)info[i])) i++;
  while (i < info_len && !is_space((unsigned char)info[i])) {
    if (ll + 1 < sizeof(lang)) lang[ll++] = lower(info[i]);
    i++;
  }
  lang[ll] = '\0';
  p->code_n++;
  p->cur_off = off;
  p->cur_level = 0;
  p->cur_code = true;
  p->cur_line_beg = 0;
  if (ll > 0) {
    p->cur_id = kbc_arena_printf(p->a, "code-%s-%zu", lang, p->code_n);
  } else {
    p->cur_id = kbc_arena_printf(p->a, "code-%zu", p->code_n);
  }
  return KBC_OK;
}

static kbc_status p_setext(parser *p, const char *s, size_t n, size_t *pos,
                           size_t line_len) {
  char ch = s[*pos];
  size_t k = *pos;
  while (k < n && s[k] == ch) k++;
  size_t e = k;
  while (e < n && is_space((unsigned char)s[e])) e++;
  if (e != n && s[e] != '\n') return KBC_OK; /* not a setext underline */
  if (k - *pos < 1) return KBC_OK;

  kbc_str last;
  kbc_str_init(&last);
  size_t b = p->cur_line_beg;
  size_t end = p->cur.len;
  while (b < end && p->cur.ptr[b] == ' ') b++;
  kbc_status st = kbc_str_append(&last, p->cur.ptr + b, end - b);
  if (!kbc_failed(st) && end > p->cur_line_beg) st = p_flush(p);
  if (!kbc_failed(st)) {
    kbc_str_clear(&p->cur);
    st = kbc_str_append(&p->cur, last.ptr, last.len);
  }
  if (!kbc_failed(st)) {
    p->cur_level = (ch == '=') ? 1 : 2;
    p->cur_line_beg = 0;
  }
  kbc_str_free(&last);
  if (kbc_failed(st)) return st;
  *pos = (e < n) ? e + 1 : n;
  (void)line_len;
  return KBC_OK;
}

static kbc_status p_line(parser *p, const char *s, size_t n, size_t *pos) {
  size_t i = *pos;
  size_t j = i;
  while (j < n && (s[j] == ' ' || s[j] == '\t')) j++;
  if (j - i > 3) return KBC_OK; /* 4-space indent: modelled as prose */
  if (j >= n) {
    *pos = n;
    return KBC_OK;
  }
  size_t le = line_end(s, n, j);

  if (p->in_fence) {
    if (s[j] == p->fence_ch) {
      size_t k = j;
      while (k < n && s[k] == p->fence_ch) k++;
      size_t e = k;
      while (e < n && (s[e] == ' ' || s[e] == '\t')) e++;
      if (k - j >= p->fence_len && (e >= n || s[e] == '\n')) {
        p->in_fence = false;
        *pos = (e < n) ? e + 1 : n;
        return p_flush(p);
      }
    }
    kbc_status st = p_put(p, s + j, le - j);
    p->cur_line_beg = p->cur.len;
    *pos = (le < n) ? le + 1 : n;
    return st;
  }

  if ((s[j] == '`' || s[j] == '~') && j < le) {
    char ch = s[j];
    size_t k = j;
    while (k < n && s[k] == ch) k++;
    if (k - j >= 3) {
      size_t info_end = le;
      if (ch == '`') {
        /* an info string may not contain a backtick */
        for (size_t x = k; x < le; x++) {
          if (s[x] == '`') {
            *pos = (le < n) ? le + 1 : n;
            return KBC_OK;
          }
        }
        while (info_end > k && is_space((unsigned char)s[info_end - 1])) {
          info_end--;
        }
      }
      p->in_fence = true;
      p->fence_ch = ch;
      p->fence_len = k - j;
      kbc_status st = p_open_code(p, s + k, info_end - k, *pos);
      *pos = (le < n) ? le + 1 : n;
      return st;
    }
  }

  if (s[j] == '#') {
    size_t k = j;
    while (k < n && s[k] == '#') k++;
    if (k - j <= 6 && (k >= n || s[k] == ' ' || s[k] == '\t' || s[k] == '\n')) {
      size_t te = le;
      while (te > k && is_space((unsigned char)s[te - 1])) te--;
      while (te > k && s[te - 1] == '#') te--;
      while (te > k && is_space((unsigned char)s[te - 1])) te--;
      kbc_status st = p_flush(p);
      if (kbc_failed(st)) return st;
      p->cur_off = *pos;
      p->cur_level = (int)(k - j);
      st = p_put(p, s + k, te - k);
      *pos = (le < n) ? le + 1 : n;
      return st;
    }
  }

  if ((s[j] == '=' || s[j] == '-') && p->cur.len > 0 && p->cur_level == 0 &&
      !p->cur_code) {
    return p_setext(p, s, n, pos, le - j);
  }
  return KBC_OK;
}

/* ------------------------------------------------------------- sniffing -- */

static bool looks_like_html(const char *s, size_t len) {
  size_t n = len < 1024 ? len : 1024;
  size_t i = 0;
  while (i < n && is_space((unsigned char)s[i])) i++;
  if (i < n && s[i] == '<') {
    if (i + 1 < n) {
      unsigned char c = (unsigned char)s[i + 1];
      if (is_alpha(c) || c == '!' || c == '?') return true;
    }
    return false;
  }
  if (ci_find(s, n, 0, "<html") < n) return true;
  if (ci_find(s, n, 0, "<!doctype") < n) return true;
  return false;
}

kbc_parsed *kbc_parse(kbc_arena *a, const char *text, size_t len,
                      const char *rel_path, kbc_err *err) {
  if (a == NULL) {
    kbc_err_set(err, KBC_ERR_INVALID, "parse: arena is NULL");
    return NULL;
  }
  if (text == NULL) {
    kbc_err_set(err, KBC_ERR_INVALID, "parse: text is NULL");
    return NULL;
  }
  if (len > KBC_MAX_ARTIFACT_BYTES) {
    kbc_err_set(err, KBC_ERR_INVALID,
                "parse: %zu bytes exceeds the %u byte ceiling", len,
                (unsigned)KBC_MAX_ARTIFACT_BYTES);
    return NULL;
  }

  parser p;
  memset(&p, 0, sizeof(p));
  p.a = a;
  p.err = err;
  p.out = kbc_arena_calloc(a, 1, sizeof(struct kbc_parsed));
  if (p.out == NULL) {
    kbc_err_set(err, KBC_ERR_NOMEM, "parse: out of memory");
    return NULL;
  }
  kbc_str_init(&p.cur);
  kbc_str_init(&p.title_buf);
  bool html = looks_like_html(text, len);
  (void)html; /* the scanner is shape-driven; sniffing only documents intent */

  size_t pos = 0;
  kbc_status st = KBC_OK;
  while (pos < len) {
    bool at_line_start = (pos == 0 || text[pos - 1] == '\n');
    if (at_line_start) {
      size_t before = pos;
      st = p_line(&p, text, len, &pos);
      if (kbc_failed(st)) goto fail;
      if (pos > before) continue; /* the line construct consumed the line */
    }
    unsigned char c = (unsigned char)text[pos];
    if (c == '<') {
      size_t before = pos;
      st = p_tag(&p, text, len, &pos);
      if (kbc_failed(st)) goto fail;
      if (pos == before) st = KBC_ERR_INTERNAL;
      if (kbc_failed(st)) goto fail;
      continue;
    }
    if (c == '&') {
      if (p.pending) {
        p.pending = false;
        st = kbc_str_putc(&p.cur, ' ');
        if (kbc_failed(st)) goto fail;
        if (p.in_title) {
          kbc_status t2 = kbc_str_putc(&p.title_buf, ' ');
          if (kbc_failed(t2)) {
            st = t2;
            goto fail;
          }
        }
      }
      size_t used = 0;
      st = decode_entity(&p.cur, text, len, pos, &used);
      if (kbc_failed(st)) goto fail;
      if (p.in_title) {
        kbc_status t2 = kbc_str_append(&p.title_buf, text + pos, used);
        if (kbc_failed(t2)) {
          st = t2;
          goto fail;
        }
      }
      pos += used;
      continue;
    }
    if (c == '\n') {
      if (p.cur.len > 0) p.pending = true;
      p.cur_line_beg = p.cur.len;
      pos++;
      continue;
    }
    if (is_space(c)) {
      if (p.cur.len > 0) p.pending = true;
      pos++;
      continue;
    }
    st = p_put_char(&p, c);
    if (kbc_failed(st)) goto fail;
    pos++;
  }
  st = p_flush(&p);
  if (kbc_failed(st)) goto fail;

  const char *title = p.title_h1 != NULL   ? p.title_h1
                      : p.title_tag != NULL ? p.title_tag
                      : p.title_prose != NULL ? p.title_prose
                                              : kbc_title_from_path(
                                                    a, rel_path ? rel_path : "", "");
  p.out->title = kbc_arena_strdup(a, title ? title : "");
  kbc_str_free(&p.cur);
  kbc_str_free(&p.title_buf);
  return p.out;

fail:
  kbc_str_free(&p.cur);
  kbc_str_free(&p.title_buf);
  if (st == KBC_ERR_NOMEM) {
    kbc_err_set(err, KBC_ERR_NOMEM, "parse: out of memory");
  } else {
    kbc_err_set(err, st, "parse: extraction failed at byte %zu", pos);
  }
  return NULL;
}

/* ---------------------------------------------------------- stopwords --- */

const char *const KBC_STOPWORDS[] = {
    "a",     "an",   "the",  "and",  "or",   "but",  "if",   "then",
    "else",  "of",   "to",   "in",   "on",   "at",   "by",   "for",
    "with",  "from", "as",   "is",   "are",  "was",  "were", "be",
    "been",  "being", "it",  "its",  "this", "that", "these", "those",
    "i",     "you",  "he",   "she",  "they", "we",   "not",  "no",
    "nor",   "so",   "than", "too",  "very", "can",  "will", "just",
    "do",    "does", "did",  "done", "have", "has",  "had",  "having",
    "my",    "our",  "your", "their", "his", "her",  "them", "us",
    "me",    "mine", "what", "which", "who", "whom", "when", "where",
    "why",   "how",  "all",  "any",  "both", "each", "few",  "more",
    "most",  "other", "some", "such", "only", "own",  "same", "s",
    "t",     "don",  "now",
};

const size_t KBC_STOPWORDS_LEN =
    sizeof(KBC_STOPWORDS) / sizeof(KBC_STOPWORDS[0]);

/* Open-addressed stopword set, rebuilt per call from the const table: a few
 * hundred bytes of stack instead of a global mutable cache. */
#define STOP_SLOTS 256u

static bool is_stopword(const char *tok, size_t len) {
  uint32_t slot[STOP_SLOTS];
  for (unsigned i = 0; i < STOP_SLOTS; i++) slot[i] = UINT32_MAX;
  for (size_t i = 0; i < KBC_STOPWORDS_LEN; i++) {
    uint32_t h = kbc_fnv1a32(KBC_STOPWORDS[i], strlen(KBC_STOPWORDS[i]));
    unsigned k = h & (STOP_SLOTS - 1u);
    while (slot[k] != UINT32_MAX) k = (k + 1u) & (STOP_SLOTS - 1u);
    slot[k] = (uint32_t)i;
  }
  uint32_t h = kbc_fnv1a32(tok, len);
  unsigned k = h & (STOP_SLOTS - 1u);
  while (slot[k] != UINT32_MAX) {
    const char *cand = KBC_STOPWORDS[slot[k]];
    if (strlen(cand) == len && memcmp(cand, tok, len) == 0) return true;
    k = (k + 1u) & (STOP_SLOTS - 1u);
  }
  return false;
}

/* ---------------------------------------------------------- tokenizer --- */

kbc_status kbc_tokenize(kbc_arena *a, const char *text, size_t len,
                        kbc_tokens *out, kbc_err *err) {
  if (a == NULL) {
    kbc_err_set(err, KBC_ERR_INVALID, "tokenize: arena is NULL");
    return KBC_ERR_INVALID;
  }
  if (text == NULL) {
    kbc_err_set(err, KBC_ERR_INVALID, "tokenize: text is NULL");
    return KBC_ERR_INVALID;
  }
  if (out == NULL) {
    kbc_err_set(err, KBC_ERR_INVALID, "tokenize: out is NULL");
    return KBC_ERR_INVALID;
  }
  if (len > KBC_MAX_ARTIFACT_BYTES) {
    kbc_err_set(err, KBC_ERR_INVALID,
                "tokenize: %zu bytes exceeds the %u byte ceiling", len,
                (unsigned)KBC_MAX_ARTIFACT_BYTES);
    return KBC_ERR_INVALID;
  }

  char buf[KBC_MAX_TERM_LEN + 2];
  size_t blen = 0;
  bool overflow = false;
  bool in_tok = false;
  size_t tstart = 0;
  size_t tend = 0;
  kbc_status st = KBC_OK;

#define KBC_EMIT_TOKEN()                                                      \
  do {                                                                        \
    if (in_tok) {                                                             \
      if (!overflow && blen >= 2 && blen <= KBC_MAX_TERM_LEN &&                \
          !is_stopword(buf, blen)) {                                         \
        if (out->len >= (size_t)KBC_MAX_TOKENS_PER_DOC) {                     \
          kbc_err_set(err, KBC_ERR_INVALID,                                  \
                      "tokenize: more than %u tokens (offset %zu)",           \
                      (unsigned)KBC_MAX_TOKENS_PER_DOC, tstart);              \
          st = KBC_ERR_INVALID;                                               \
          goto done;                                                          \
        }                                                                     \
        st = arena_grow((void ***)&out->items, &out->cap, out->len + 1,        \
                        sizeof(kbc_token), a);                                \
        if (kbc_failed(st)) goto done;                                        \
        kbc_token *tk = &out->items[out->len++];                              \
        tk->text = kbc_arena_strndup(a, buf, blen);                            \
        tk->len = blen;                                                       \
        tk->start = (uint32_t)tstart;                                         \
        tk->end = (uint32_t)tend;                                             \
      }                                                                       \
    }                                                                         \
    in_tok = false;                                                           \
    blen = 0;                                                                 \
    overflow = false;                                                         \
  } while (0)

  size_t i = 0;
  while (i < len) {
    unsigned char c = (unsigned char)text[i];
    size_t seqlen = 1;
    if (c >= 0x80) {
      size_t l = kbc_utf8_len(c);
      if (l > 1 && i + l <= len) seqlen = l;
    }
    if (seqlen == 1) {
      if (is_alnum(c)) {
        if (!in_tok) {
          in_tok = true;
          tstart = i;
        }
        if (blen >= KBC_MAX_TERM_LEN) {
          overflow = true;
        } else {
          buf[blen++] = lower((char)c);
        }
        tend = i + 1;
        i++;
        continue;
      }
      KBC_EMIT_TOKEN();
      i++;
      continue;
    }
    char folded[32];
    size_t fl = kbc_fold_utf8(text + i, seqlen, folded);
    bool word = fl > 0;
    for (size_t k = 0; word && k < fl; k++) {
      if (!is_alnum((unsigned char)folded[k])) word = false;
    }
    if (!word) {
      KBC_EMIT_TOKEN();
      i += seqlen;
      continue;
    }
    if (!in_tok) {
      in_tok = true;
      tstart = i;
    }
    for (size_t k = 0; k < fl; k++) {
      if (blen >= KBC_MAX_TERM_LEN) {
        overflow = true;
      } else {
        buf[blen++] = folded[k];
      }
    }
    tend = i + seqlen;
    i += seqlen;
  }
  KBC_EMIT_TOKEN();

#undef KBC_EMIT_TOKEN

done:
  return st;
}
