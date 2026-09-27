/* parse.c — the reader parse (Markdown/HTML -> blocks + anchors) and the
 * tokenizer.
 *
 * This is a streaming state machine over the caller's buffer: every byte is
 * visited a bounded number of times, nothing is copied until a block closes.
 * It is deliberately not a CommonMark renderer — see include/kbc/parse.h.
 */
#include <stdio.h>
#include <string.h>

#include "kbc/log.h"

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
  kbc_links links;    /* KBC_ARENA; outbound links, document order */
  char **anchors;     /* KBC_ARENA, NUL-terminated entries */
  size_t anchors_len;
  size_t anchors_cap;
  const char *title;  /* KBC_ARENA */
  kbc_metas metas;    /* KBC_ARENA; kb-* metadata, document order */
};

typedef struct {
  kbc_arena *a;
  kbc_err *err;
  struct kbc_parsed *out;

  kbc_str cur;         /* text of the block under construction */
  size_t cur_off;      /* source offset of that block's first byte */
  size_t src_off;      /* source offset of the byte being appended now */
  size_t line_off;     /* source offset of the current line's first byte */
  size_t prev_line_off; /* source offset of the previous line's first byte */
  int cur_level;       /* 0 = prose, 1..6 = heading */
  bool cur_code;       /* fenced code: the id is code-<lang>-<n> */
  const char *cur_id;  /* explicit id for the block being built */
  bool pending;        /* a run of whitespace is pending -> one space */

  bool in_fence;
  char fence_ch;
  size_t fence_len;
  size_t code_n;

  bool in_title; /* inside <title>...</title> */
  bool title_pending; /* whitespace run pending in the title buffer */
  kbc_str title_buf;
  /* The visible text of the <a> currently being read. The link itself is
   * pushed at the tag, before its text is known, so this buffer is what
   * fills the link's `text` when </a> closes. */
  bool in_anchor;
  bool anchor_pending; /* whitespace run pending in the anchor buffer */
  kbc_str anchor_buf;
  const char *rel; /* the document's own corpus-relative path, BORROWED */
  size_t anchor_link; /* index into out->links, or SIZE_MAX */

  const char *title_h1;    /* first h1 block, markdown or HTML */
  const char *title_tag;   /* first <title> element */
  const char *title_prose; /* first non-empty prose block */
} parser;

const kbc_links *kbc_parsed_links(const kbc_parsed *p) {
  return p == NULL ? NULL : &p->links;
}

const kbc_metas *kbc_parsed_metas(const kbc_parsed *p) {
  return p == NULL ? NULL : &p->metas;
}

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


/* A run of source whitespace collapses to one space, in the block text and
 * in the <title> buffer alike; the space itself is materialised only when
 * more text follows. A decoded entity that is whitespace goes through here
 * too, so it collapses exactly like a literal space. */
static kbc_status p_space(parser *p) {
  if (p->cur.len > 0) p->pending = true;
  if (p->in_anchor && p->anchor_buf.len > 0) p->anchor_pending = true;
  if (p->in_title && p->title_buf.len > 0) p->title_pending = true;
  return KBC_OK;
}

static kbc_status p_put(parser *p, const char *d, size_t n) {
  if (n == 0) return KBC_OK;
  kbc_status st;
  if (p->pending) {
    p->pending = false;
    st = kbc_str_putc(&p->cur, ' ');
    if (kbc_failed(st)) return st;
  }
  if (p->cur.len == 0) p->cur_off = p->src_off; /* this block starts here */
  st = kbc_str_append(&p->cur, d, n);
  if (kbc_failed(st)) return st;
  if (p->in_title) {
    if (p->title_pending) {
      p->title_pending = false;
      st = kbc_str_putc(&p->title_buf, ' ');
      if (kbc_failed(st)) return st;
    }
    st = kbc_str_append(&p->title_buf, d, n);
    if (kbc_failed(st)) return st;
  }
  if (p->in_anchor) {
    if (p->anchor_pending) {
      p->anchor_pending = false;
      st = kbc_str_putc(&p->anchor_buf, ' ');
      if (kbc_failed(st)) return st;
    }
    st = kbc_str_append(&p->anchor_buf, d, n);
    if (kbc_failed(st)) return st;
  }
  return KBC_OK;
}

static kbc_status p_put_char(parser *p, unsigned char c) {
  if (is_space(c)) return p_space(p);
  return p_put(p, (const char *)&c, 1);
}

/* ---------------------------------------------------------------- links --- */

/* A link target is resolved by SEGMENT, never by string surgery on the whole
 * path: `a/../b` and `a/b/..` are different targets, and collapsing them
 * textually is how a link ends up pointing somewhere nobody wrote. The
 * segment stack is bounded by LINK_MAX_SEGS; a target deeper than that is
 * dropped rather than trusted. */
#define LINK_MAX_SEGS 64

/* Appends `seg` as one '/'-separated segment, recording where it starts. */
static kbc_status link_push_seg(kbc_str *out, size_t *stack, size_t *depth,
                                const char *seg, size_t len) {
  if (len == 0) return KBC_OK;
  if (*depth >= LINK_MAX_SEGS) return KBC_ERR_INVALID;
  stack[*depth] = out->len;
  (*depth)++;
  kbc_status st = kbc_str_putc(out, '/');
  if (kbc_failed(st)) return st;
  return kbc_str_append(out, seg, len);
}

/* Consumes one '/'-separated run of `s`, interpreting "." as nothing and
 * ".." as one step up. False means the target tried to escape the corpus
 * root, which is a DROP, not a clamp: a link out of the corpus is not a link
 * to anything kb-c indexes. */
static bool link_walk(const char *s, size_t len, kbc_str *out, size_t *stack,
                      size_t *depth) {
  size_t i = 0;
  while (i < len) {
    while (i < len && (s[i] == '/' || s[i] == '\\')) i++;
    const size_t start = i;
    while (i < len && s[i] != '/' && s[i] != '\\') i++;
    const size_t slen = i - start;
    if (slen == 0) continue;
    if (slen == 1 && s[start] == '.') continue;
    if (slen == 2 && s[start] == '.' && s[start + 1] == '.') {
      if (*depth == 0) return false;
      (*depth)--;
      out->len = stack[*depth];
      continue;
    }
    if (kbc_failed(link_push_seg(out, stack, depth, s + start, slen)))
      return false;
  }
  return true;
}

/* Resolves a raw href/markdown target to a corpus-relative path in `out`.
 * False = not a corpus document, and the link is dropped.
 *
 * Dropped: the empty target, an anchor (`#section`), anything with a scheme
 * (`http://`, `mailto:`, `tel:`, `data:` — a ':' in the first segment is the
 * test, so a Windows drive letter is dropped with them) and anything with a
 * query string, since no indexed path carries one. A leading '/' is corpus
 * root-relative, not filesystem-absolute (links.rs:187 normalize_target
 * strips it), and '\\' is folded to '/', as that function does. */
static bool link_resolve(const char *rel, const char *t, size_t tlen,
                         kbc_str *out) {
  while (tlen > 0 && is_space((unsigned char)t[0])) {
    t++;
    tlen--;
  }
  while (tlen > 0 && is_space((unsigned char)t[tlen - 1])) tlen--;
  if (tlen == 0 || t[0] == '#') return false;
  for (size_t i = 0; i < tlen; i++) {
    if (t[i] == '#') {
      tlen = i;
      break;
    }
    if (t[i] == ':' || t[i] == '?') return false;
  }
  if (tlen == 0) return false;

  size_t stack[LINK_MAX_SEGS];
  size_t depth = 0;
  size_t i = 0;
  if (t[0] == '/') {
    while (i < tlen && t[i] == '/') i++;
  } else {
    const char *slash = rel != NULL ? strrchr(rel, '/') : NULL;
    if (slash != NULL) {
      size_t dlen = (size_t)(slash - rel);
      if (!link_walk(rel, dlen, out, stack, &depth)) return false;
    }
  }
  if (!link_walk(t + i, tlen - i, out, stack, &depth)) return false;
  /* Segments are joined with a leading '/', which is the shape the walk
   * needs; a corpus-relative path is not. */
  if (out->len > 0 && out->ptr[0] == '/') {
    memmove(out->ptr, out->ptr + 1, out->len - 1);
    out->len--;
    out->ptr[out->len] = '\0';
  }
  return out->len > 0;
}

/* Pushes one resolved link. The text is stored verbatim; the caller has
 * already decoded it if it wants the decoded form. Returns SIZE_MAX when the
 * target was not a corpus document, so the caller can leave an unopened
 * </a> harmless. */
static kbc_status push_link(parser *p, const char *t, size_t tlen,
                            const char *text, size_t text_len, size_t *idx) {
  *idx = SIZE_MAX;
  kbc_str resolved;
  kbc_str_init(&resolved);
  if (!link_resolve(p->rel, t, tlen, &resolved)) {
    kbc_str_free(&resolved);
    return KBC_OK;
  }
  kbc_status st = arena_grow((void ***)&p->out->links.items,
                             &p->out->links.cap, p->out->links.len + 1,
                             sizeof(kbc_link), p->a);
  if (kbc_failed(st)) {
    kbc_str_free(&resolved);
    return st;
  }
  kbc_link *l = &p->out->links.items[p->out->links.len];
  l->target = kbc_arena_strndup(p->a, resolved.ptr, resolved.len);
  l->target_len = resolved.len;
  l->text = kbc_arena_strndup(p->a, text != NULL ? text : "", text_len);
  kbc_str_free(&resolved);
  if (l->target == NULL || l->text == NULL) return KBC_ERR_NOMEM;
  *idx = p->out->links.len++;
  return KBC_OK;
}

/* `[text](target)`, `[text](<target>)` and `[text](target "title")`. Returns
 * the byte just past the closing ')', or 0 when this is not a link — a bare
 * '[', a reference link, or a '[' whose ']' never arrives on this line. An
 * image (`![alt](src)`) is NOT a link: the caller still consumes the text, so
 * the block's prose is byte-identical either way, and `*image` says so. */
static size_t md_link(const char *s, size_t n, size_t i, bool *image,
                      size_t *text_off, size_t *text_len, size_t *tgt_off,
                      size_t *tgt_len) {
  size_t j = i + 1;
  while (j < n && s[j] != ']' && s[j] != '[' && s[j] != '\n') j++;
  if (j >= n || s[j] != ']' || j + 1 >= n || s[j + 1] != '(') return 0;
  *image = i > 0 && s[i - 1] == '!';
  *text_off = i + 1;
  *text_len = j - (i + 1);

  size_t t = j + 2;
  int depth = 1;
  char quote = '\0';
  while (t < n) {
    const char c = s[t];
    if (quote != '\0') {
      if (c == quote) quote = '\0';
    } else if (c == '"' || c == '\'') {
      quote = c;
    } else if (c == '(') {
      depth++;
    } else if (c == ')') {
      if (--depth == 0) break;
    } else if (c == '\n') {
      return 0; /* a link does not run across a line: not this grammar */
    }
    t++;
  }
  if (t >= n || depth != 0) return 0;

  size_t ts = j + 2, te = t;
  while (ts < te && is_space((unsigned char)s[ts])) ts++;
  while (te > ts && is_space((unsigned char)s[te - 1])) te--;
  if (ts < te && s[ts] == '<') {
    size_t gt = ts + 1;
    while (gt < te && s[gt] != '>') gt++;
    if (gt < te) {
      ts++;
      te = gt;
    }
  }
  else {
    /* an optional title follows the target: `[a](b "t")` -> `b` */
    size_t k = ts;
    while (k < te && !is_space((unsigned char)s[k])) k++;
    if (k > ts && s[k - 1] == '\\') k--; /* an escaped space stays in it */
    te = k;
  }
  *tgt_off = ts;
  *tgt_len = te - ts;
  return t + 1;
}
/* `[[target]]` and `[[target|alias]]` — the Obsidian wikilink forms the kb
 * corpus is written in. Resolution is link_resolve, the SAME normaliser the
 * markdown and href forms use: a wikilink normalised differently from a
 * markdown link would give the graph two incompatible notions of identity.
 * What link_resolve drops matches links.rs normalize_target (links.rs:186,
 * trim -> drop '#fragment' -> strip leading '/' -> fold '\\'), and it adds
 * the corpus-local segment walk on top.
 *
 * Returns the byte just past the closing "]]", or 0 when this is not a
 * wikilink: a '[' with no "[[", a construct that does not close on this line,
 * and `[[]]` / `[[|x]]`, which name no target. Those fall through to the
 * ordinary byte path, so the prose keeps the brackets exactly as written.
 *
 * `*image` is `![...]]` (an embed): not a link, as `![alt](src)` is not one
 * either — the caller still consumes the display text, so the prose stays
 * byte-identical with and without the syntax. */
static size_t wikilink(const char *s, size_t n, size_t i, bool *image,
                       size_t *tgt_off, size_t *tgt_len, size_t *text_off,
                       size_t *text_len) {
  if (i + 1 >= n || s[i + 1] != '[') return 0;
  *image = i > 0 && s[i - 1] == '!';

  size_t j = i + 2;
  while (j < n && s[j] != '\n' &&
         !(s[j] == ']' && j + 1 < n && s[j + 1] == ']')) {
    j++;
  }
  if (j + 1 >= n || s[j] != ']' || s[j + 1] != ']') return 0;

  size_t ts = i + 2, te = j;
  size_t pipe = SIZE_MAX;
  for (size_t k = ts; k < te; k++) {
    if (s[k] == '|') {
      pipe = k;
      break;
    }
  }
  while (ts < te && is_space((unsigned char)s[ts])) ts++;
  size_t tend = (pipe != SIZE_MAX) ? pipe : te;
  while (tend > ts && is_space((unsigned char)s[tend - 1])) tend--;
  if (ts >= tend) return 0; /* `[[]]` and `[[   ]]` name no target */

  /* A bare `[[target]]` has no alias, and the display text IS the target —
   * comrak builds the no-pipe form's label from its url, and links.rs:90
   * collapses an equal label to `None` (links.rs:78 parse_wikilinks). An
   * alias is display text only: it never reaches link_resolve, so it cannot
   * mint a second identity for the same document. */
  *tgt_off = ts;
  *tgt_len = tend - ts;
  *text_off = ts;
  *text_len = tend - ts;
  if (pipe != SIZE_MAX) {
    size_t xs = pipe + 1, xe = te;
    while (xs < xe && is_space((unsigned char)s[xs])) xs++;
    while (xe > xs && is_space((unsigned char)s[xe - 1])) xe--;
    if (xs >= xe) return 0; /* `[[|x]]` names no target either */
    *text_off = xs;
    *text_len = xe - xs;
  }
  return j + 2;
}

/* Feeds a byte range through exactly the path the main loop takes for one
 * byte, so a link's visible text lands in the block prose the way it would
 * have without the link syntax around it. */
static kbc_status feed_inline(parser *p, const char *s, size_t from, size_t to) {
  size_t pos = from;
  while (pos < to) {
    p->src_off = pos;
    if (s[pos] == '&') {
      kbc_str dec;
      kbc_str_init(&dec);
      size_t used = 0;
      kbc_status st = decode_entity(&dec, s, to, pos, &used);
      if (!kbc_failed(st)) {
        for (size_t x = 0; x < dec.len && !kbc_failed(st); x++) {
          st = p_put_char(p, (unsigned char)dec.ptr[x]);
        }
      }
      kbc_str_free(&dec);
      if (kbc_failed(st)) return st;
      pos += used;
      continue;
    }
    kbc_status st = p_put_char(p, (unsigned char)s[pos]);
    if (kbc_failed(st)) return st;
    pos++;
  }
  return KBC_OK;
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
  /* a heading, a fenced block, or any block that carries an explicit id is
   * addressable */
  if (p->cur_level > 0 || p->cur_code || p->cur_id != NULL) {
    st = parsed_add_anchor(p, id);
    if (kbc_failed(st)) return st;
  }
  kbc_str_clear(&p->cur);
  p->cur_level = 0;
  p->cur_id = NULL;
  p->cur_code = false;
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

  /* any byte p_tag appends comes from the tag's own offset */
  p->src_off = start;
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

  const char *href = NULL;
  size_t href_len = 0;
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
    if (alen == 4 && lower(s[as]) == 'h' && lower(s[as + 1]) == 'r' &&
        lower(s[as + 2]) == 'e' && lower(s[as + 3]) == 'f' && val != NULL) {
      href = val;
      href_len = vlen;
    }
  }
  *pos = (i < n) ? i + 1 : n;

  bool is_title = name_eq(name, namelen, "title");
  int level = 0;
  bool block = block_tag(name, namelen, &level);

  if (name_eq(name, namelen, "a")) {
    /* <a> is inline: it must not flush the block, and its href is one of the
     * two link forms parse.h names. The text is not known until </a>, so the
     * link goes in now and is completed there. */
    if (closing) {
      if (p->in_anchor) {
        if (p->anchor_link != SIZE_MAX) {
          p->out->links.items[p->anchor_link].text = kbc_arena_strndup(
              p->a, p->anchor_buf.ptr != NULL ? p->anchor_buf.ptr : "",
              p->anchor_buf.len);
          if (p->out->links.items[p->anchor_link].text == NULL) {
            p->in_anchor = false;
            p->anchor_link = SIZE_MAX;
            return KBC_ERR_NOMEM;
          }
        }
        p->in_anchor = false;
        p->anchor_pending = false;
        p->anchor_link = SIZE_MAX;
        kbc_str_clear(&p->anchor_buf);
      }
      return KBC_OK;
    }
    size_t idx = SIZE_MAX;
    kbc_status ls = push_link(p, href, href_len, "", 0, &idx);
    if (kbc_failed(ls)) return ls;
    p->in_anchor = true;
    p->anchor_pending = false;
    p->anchor_link = idx;
    kbc_str_clear(&p->anchor_buf);
    return KBC_OK;
  }

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
      p->title_pending = false;
      kbc_str_clear(&p->title_buf);
    } else {
      kbc_status st = p_flush(p);
      if (kbc_failed(st)) return st;
      p->cur_off = start;
      p->in_title = true;
      p->title_pending = false;
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
  if (ll > 0) {
    p->cur_id = kbc_arena_printf(p->a, "code-%s-%zu", lang, p->code_n);
  } else {
    p->cur_id = kbc_arena_printf(p->a, "code-%zu", p->code_n);
  }
  return KBC_OK;
}

/* A setext underline promotes the line above it to a heading. The candidate
 * is the last line appended to the block, which is located by scanning back
 * to the previous newline rather than by a stored index: the block may have
 * been extended by several lines since the last one was recorded. */
static kbc_status p_setext(parser *p, const char *s, size_t n, size_t *pos,
                           size_t line_len) {
  (void)line_len;
  char ch = s[*pos];
  size_t k = *pos;
  while (k < n && s[k] == ch) k++;
  size_t e = k;
  /* only horizontal whitespace may follow the underline run: a newline ends
   * the line and must not be swallowed by the scan */
  while (e < n && (s[e] == ' ' || s[e] == '\t' || s[e] == '\r')) e++;
  if (e != n && s[e] != '\n') return KBC_OK; /* not a setext underline */
  if (k - *pos < 1) return KBC_OK;

  /* the pending line, as a slice of the block text */
  size_t end = p->cur.len;
  size_t b = 0;
  for (size_t x = end; x > 0; x--) {
    if (p->cur.ptr[x - 1] == '\n') {
      b = x;
      break;
    }
  }
  while (b < end && p->cur.ptr[b] == ' ') b++;
  if (end == b) return KBC_OK; /* nothing on the line to promote */

  if (b == 0) {
    /* the line is the whole block: promote it in place */
    p->cur_off = p->prev_line_off;
    p->cur_level = (ch == '=') ? 1 : 2;
    *pos = (e < n) ? e + 1 : n;
    return KBC_OK;
  }

  /* earlier lines share the block: flush them, then promote the last line */
  kbc_str last;
  kbc_str_init(&last);
  kbc_status st = kbc_str_append(&last, p->cur.ptr + b, end - b);
  if (!kbc_failed(st)) st = p_flush(p);
  if (!kbc_failed(st)) {
    kbc_str_clear(&p->cur);
    st = kbc_str_append(&p->cur, last.ptr, last.len);
  }
  if (!kbc_failed(st)) {
    p->cur_off = p->prev_line_off;
    p->cur_level = (ch == '=') ? 1 : 2;
  }
  kbc_str_free(&last);
  if (kbc_failed(st)) return st;
  *pos = (e < n) ? e + 1 : n;
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
    p->src_off = *pos;
    kbc_status st = p_put(p, s + j, le - j);
    *pos = (le < n) ? le + 1 : n;
    return st;
  }

  /* A blank line is a block boundary. It is handled here rather than in the
   * main loop so that a blank line inside a fenced block — consumed by the
   * branch above — never closes the block. */
  if (le == j) {
    *pos = (le < n) ? le + 1 : n;
    if (p->cur.len == 0) return KBC_OK;
    return p_flush(p);
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
    bool separated = (k >= n || s[k] == ' ' || s[k] == '\t' || s[k] == '\n');
    if (k - j <= 6 && separated) {
      size_t te = le;
      while (te > k && is_space((unsigned char)s[te - 1])) te--;
      while (te > k && s[te - 1] == '#') te--;
      while (te > k && is_space((unsigned char)s[te - 1])) te--;
      /* the separator after the '#' run is not part of the heading text */
      size_t hs = k;
      while (hs < te && is_space((unsigned char)s[hs])) hs++;
      kbc_status st = p_flush(p);
      if (kbc_failed(st)) return st;
      p->cur_off = *pos;
      p->cur_level = (int)(k - j);
      p->src_off = hs;
      for (size_t x = hs; x < te; x++) {
        st = p_put_char(p, (unsigned char)s[x]);
        if (kbc_failed(st)) return st;
      }
      /* a line that continues the heading is separated by one space */
      p->pending = p->cur.len > 0;
      *pos = (le < n) ? le + 1 : n;
      return KBC_OK;
    }
    if (separated) {
      /* More than six hashes: not a heading, but the run is a marker rather
       * than text, so the line becomes prose and keeps its slug as an id. */
      size_t hs = k;
      while (hs < le && is_space((unsigned char)s[hs])) hs++;
      size_t te = le;
      while (te > hs && is_space((unsigned char)s[te - 1])) te--;
      kbc_status st = p_flush(p);
      if (kbc_failed(st)) return st;
      p->cur_off = *pos;
      p->src_off = hs;
      for (size_t x = hs; x < te; x++) {
        st = p_put_char(p, (unsigned char)s[x]);
        if (kbc_failed(st)) return st;
      }
      kbc_str sl;
      kbc_str_init(&sl);
      st = slug_into(p->a, p->cur.ptr, p->cur.len, &sl);
      if (!kbc_failed(st) && sl.len > 0) {
        p->cur_id = kbc_arena_strdup(p->a, sl.ptr);
      }
      kbc_str_free(&sl);
      if (kbc_failed(st)) return st;
      p->pending = p->cur.len > 0;
      *pos = (le < n) ? le + 1 : n;
      return KBC_OK;
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

/* -------------------------------------------------------------- metas --- */

/* The filterable metadata a document declares about itself. This runs as a
 * SEPARATE pass over the source, after the block/anchor scan has finished,
 * and it appends to p->out->metas and nothing else: a `<meta>` or a front
 * matter block contributes no block text and no link, so extraction cannot
 * move a byte of what the index tokenizes. */

/* A document is hostile input, so the number of metas one file may declare is
 * bounded. Past the bound the extras are dropped with a log line rather than
 * silently, because a filter that matches a subset of what the document said
 * is a filter that lies. */
#define PARSE_MAX_METAS 256
/* The value of one meta. Generous for a tag, and it is the same order as the
 * path cap, but it keeps a single <meta content="..."> from claiming a
 * 16 MiB document's worth of arena. */
#define PARSE_MAX_META_VALUE 1024

/* `tags` and `caps` are multi-valued: a comma-separated content is ONE ENTRY
 * PER ELEMENT (meta.h), so the overlay can match a single value and a
 * document declaring two tags is two rows in the store, not one row that has
 * to be split again at query time. */
static bool meta_multi_valued(const char *key) {
  return strcmp(key, KBC_META_TAGS) == 0 || strcmp(key, KBC_META_CAPS) == 0;
}

/* Appends one already-lower-cased key with `value`, splitting a multi-valued
 * key on commas. Returns KBC_ERR_NOMEM on failure.
 *
 * A SCALAR key always contributes exactly one entry, even an empty one:
 * `<meta name="kb-index">` is a declaration whose value IS the empty string,
 * and a flag that is only recorded when it carries text is a flag that can
 * never be set the way the markup spells it. A multi-valued key contributes
 * one entry per NON-EMPTY element, because an empty element of a list is not
 * a value. */
static kbc_status meta_push(kbc_arena *a, kbc_metas *out, const char *key,
                            const char *value, size_t vlen) {
  if (vlen > PARSE_MAX_META_VALUE) vlen = PARSE_MAX_META_VALUE;
  const bool multi = meta_multi_valued(key);
  size_t i = 0;
  while (i <= vlen) {
    size_t start = i;
    while (start < vlen && is_space((unsigned char)value[start])) start++;
    size_t end = multi ? start : vlen;
    if (multi) {
      while (end < vlen && value[end] != ',') end++;
    }
    size_t stop = end;
    while (stop > start && is_space((unsigned char)value[stop - 1])) stop--;
    if (stop > start || !multi) {
      if (out->len >= PARSE_MAX_METAS) {
        KBC_LOGW("parse: more than %d kb-* metas; the rest are dropped",
                 PARSE_MAX_METAS);
        return KBC_OK;
      }
      if (out->len == out->cap) {
        const size_t ncap = out->cap == 0 ? 8 : out->cap * 2;
        kbc_meta *grown =
            (kbc_meta *)kbc_arena_calloc(a, ncap, sizeof(*grown));
        if (grown == NULL) return KBC_ERR_NOMEM;
        if (out->items != NULL && out->cap > 0)
          memcpy(grown, out->items, out->cap * sizeof(*grown));
        out->items = grown;
        out->cap = ncap;
      }
      char *k = kbc_arena_strndup(a, key, strlen(key));
      char *v = kbc_arena_strndup(a, value + start, stop - start);
      if (k == NULL || v == NULL) return KBC_ERR_NOMEM;
      out->items[out->len].key = k;
      out->items[out->len].value = v;
      out->len++;
    }
    if (!multi || end >= vlen) break;
    i = end + 1;
  }
  return KBC_OK;
}

/* A YAML-ish scalar, minus the quotes and brackets a front matter list wears
 * around its elements. Returns the value with its length; `*skip` is set past
 * the scalar so a bracketed list can be walked. */
static void fm_scalar(const char *s, size_t n, size_t *i, const char **out,
                      size_t *out_len) {
  size_t start = *i;
  while (start < n && is_space((unsigned char)s[start])) start++;
  char quote = 0;
  if (start < n && (s[start] == '"' || s[start] == '\'')) {
    quote = s[start];
    start++;
    const size_t e = start;
    size_t j = start;
    while (j < n && s[j] != quote) j++;
    *out = s + start;
    *out_len = j > e ? j - e : 0;
    *i = j < n ? j + 1 : n;
    return;
  }
  size_t end = start;
  while (end < n && s[end] != '\n' && s[end] != '#') end++;
  while (end > start && is_space((unsigned char)s[end - 1])) end--;
  *out = s + start;
  *out_len = end - start;
  *i = end;
}

/* Markdown front matter: a `---` fence on the FIRST line, closed by a `---`
 * or `...` line. Only `kb-*` keys are read, exactly as for the HTML form, so
 * a `title:` or a `date:` in a document's front matter stays a document and
 * does not become a filterable facet. */
static kbc_status front_matter(kbc_arena *a, const char *s, size_t n,
                               kbc_metas *out) {
  size_t i = 0;
  while (i < n && s[i] != '\n') i++; /* the opening line */
  size_t body = i < n ? i + 1 : n;
  /* A file that opens with `---` and never closes it is not front matter; a
 * horizontal rule at the top of a document is more common than a truncated
 * fence, and guessing would read the whole document as metadata. */
  size_t p = body;
  size_t end = 0;
  bool closed = false;
  while (p <= n) {
    const size_t ls = p;
    size_t le = ls;
    while (le < n && s[le] != '\n') le++;
    size_t t = ls;
    while (t < le && is_space((unsigned char)s[t])) t++;
    size_t u = le;
    while (u > t && is_space((unsigned char)s[u - 1])) u--;
    if (u - t == 3 && s[t] == '-' && s[t + 1] == '-' && s[t + 2] == '-') {
      end = ls;
      closed = true;
      break;
    }
    if (u - t == 3 && s[t] == '.' && s[t + 1] == '.' && s[t + 2] == '.') {
      end = ls;
      closed = true;
      break;
    }
    if (le >= n) break;
    p = le + 1;
  }
  if (!closed) return KBC_OK;

  /* The key the previous `- item` lines belong to; a block list is how
 * `kb-tags:\n  - rust\n  - c` is written, and dropping it would silently
 * lose every tag in that form. */
  char *list_key = NULL;
  i = body;
  kbc_status st = KBC_OK;
  while (st == KBC_OK && i < end) {
    const size_t ls = i;
    size_t le = ls;
    while (le < end && s[le] != '\n') le++;
    i = le + 1;
    size_t t = ls;
    while (t < le && is_space((unsigned char)s[t])) t++;
    if (t >= le || s[t] == '#') continue;
    if (s[t] == '-' && t + 1 < le && is_space((unsigned char)s[t + 1])) {
      if (list_key == NULL) continue;
      const char *v = NULL;
      size_t vlen = 0;
      size_t j = t + 1;
      fm_scalar(s, le, &j, &v, &vlen);
      st = meta_push(a, out, list_key, v, vlen);
      continue;
    }
    size_t colon = t;
    while (colon < le && s[colon] != ':') colon++;
    if (colon >= le) continue; /* not a key: value line, so not metadata */
    size_t kend = colon;
    while (kend > t && is_space((unsigned char)s[kend - 1])) kend--;
    if (kend - t < 4 || !ci_prefix(s, le, t, "kb-")) {
      list_key = NULL;
      continue;
    }
    char *key = kbc_arena_strndup(a, s + t + 3, kend - t - 3);
    if (key == NULL) return KBC_ERR_NOMEM;
    for (char *k = key; *k != '\0'; k++) *k = lower(*k);
    const char *v = NULL;
    size_t vlen = 0;
    size_t j = colon + 1;
    fm_scalar(s, le, &j, &v, &vlen);
    /* `kb-tags: [a, b]` — the bracket is part of the scalar, so it is peeled
     * here and the elements split below exactly as the HTML form's. */
    if (vlen >= 2 && v[0] == '[' && v[vlen - 1] == ']') {
      v++;
      vlen -= 2;
    }
    st = meta_push(a, out, key, v, vlen);
    list_key = vlen == 0 ? key : NULL;
  }
  return st;
}

/* One `<meta>` element's attributes. Only `name` and `content` are read, and
 * only a `name` that starts with `kb-` produces a facet: the value is the
 * trimmed content, or the empty string for a valueless element — which is
 * still an entry, because `<meta name="kb-index">` is a declaration. */
static kbc_status html_meta(kbc_arena *a, const char *s, size_t n,
                            size_t *i, kbc_metas *out) {
  size_t p = *i + 5; /* past "<meta" */
  const char *name = NULL;
  size_t name_len = 0;
  const char *content = NULL;
  size_t content_len = 0;
  while (p < n) {
    while (p < n && is_space((unsigned char)s[p])) p++;
    if (p < n && s[p] == '>') {
      p++;
      break;
    }
    if (p < n && s[p] == '/') {
      p++;
      continue;
    }
    const size_t as = p;
    while (p < n && !is_space((unsigned char)s[p]) && s[p] != '=' &&
           s[p] != '>')
      p++;
    const size_t alen = p - as;
    if (alen == 0) {
      p++;
      continue;
    }
    while (p < n && is_space((unsigned char)s[p])) p++;
    const char *val = NULL;
    size_t vlen = 0;
    if (p < n && s[p] == '=') {
      p++;
      while (p < n && is_space((unsigned char)s[p])) p++;
      if (p < n && (s[p] == '"' || s[p] == '\'')) {
        const char q = s[p++];
        val = s + p;
        while (p < n && s[p] != q) p++;
        vlen = (size_t)(s + p - val);
        if (p < n) p++;
      } else {
        val = s + p;
        while (p < n && !is_space((unsigned char)s[p]) && s[p] != '>') p++;
        vlen = (size_t)(s + p - val);
      }
    }
    if (ci_prefix(s, n, as, "name") && alen == 4) {
      name = val;
      name_len = vlen;
    } else if (ci_prefix(s, n, as, "content") && alen == 7) {
      content = val;
      content_len = vlen;
    }
  }
  /* The LAST byte consumed, not the first unconsumed one: the caller's loop
   * advances by one itself, so handing it the position after the element
   * steps over the `<` of the next `<meta>`. `p` is never <= *i: the loop
   * above starts at *i + 5. */
  *i = p - 1;
  if (name == NULL || name_len < 4 || !ci_prefix(name, name_len, 0, "kb-")) {
    return KBC_OK;
  }
  char *key = kbc_arena_strndup(a, name + 3, name_len - 3);
  if (key == NULL) return KBC_ERR_NOMEM;
  for (char *k = key; *k != '\0'; k++) *k = lower(*k);
  if (key[0] == '\0') return KBC_OK;
  return meta_push(a, out, key, content != NULL ? content : "", content_len);
}

static kbc_status extract_metas(kbc_arena *a, const char *s, size_t n,
                                kbc_metas *out) {
  kbc_status st = front_matter(a, s, n, out);
  if (kbc_failed(st)) return st;
  for (size_t i = 0; i + 5 < n; i++) {
    if (s[i] != '<') continue;
    /* "<metadata" and "<meta-" are not a meta element. */
    const unsigned char after = (unsigned char)s[i + 5];
    if (after != '>' && !is_space(after) && after != '/') continue;
    if (!ci_prefix(s, n, i, "<meta")) continue;
    st = html_meta(a, s, n, &i, out);
    if (kbc_failed(st)) return st;
    if (out->len >= PARSE_MAX_METAS) break;
  }
  return KBC_OK;
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
  kbc_str_init(&p.anchor_buf);
  p.rel = rel_path;
  p.anchor_link = SIZE_MAX;
  bool html = looks_like_html(text, len);
  (void)html; /* the scanner is shape-driven; sniffing only documents intent */

  size_t pos = 0;
  kbc_status st = KBC_OK;
  while (pos < len) {
    bool at_line_start = (pos == 0 || text[pos - 1] == '\n');
    if (at_line_start) {
      p.prev_line_off = p.line_off;
      p.line_off = pos;
      p.src_off = pos;
      size_t before = pos;
      st = p_line(&p, text, len, &pos);
      if (kbc_failed(st)) goto fail;
      if (pos > before) continue; /* the line construct consumed the line */
    }
    p.src_off = pos;
    unsigned char c = (unsigned char)text[pos];
    if (c == '[') {
      /* A markdown link, read inline. The visible text is fed through the
       * ordinary append path and the whole construct is consumed, so the
       * block's prose is byte-identical to the same document without link
       * syntax — only the links vector learns about it. */
      /* A wikilink is tried first: `[[x]]` is not markdown-link grammar, so
       * md_link would decline it anyway, but trying it here keeps the two
       * syntaxes on the same code path (consume, feed the display text,
       * record one link) rather than on two. */
      bool wimage = false;
      size_t wtoff = 0, wtlen = 0, wgoff = 0, wglen = 0;
      const size_t wend = wikilink(text, len, pos, &wimage, &wgoff, &wglen,
                                   &wtoff, &wtlen);
      if (wend > 0) {
        if (!wimage) {
          size_t idx = SIZE_MAX;
          st = push_link(&p, text + wgoff, wglen, text + wtoff, wtlen, &idx);
          if (kbc_failed(st)) goto fail;
        }
        st = feed_inline(&p, text, wtoff, wtoff + wtlen);
        if (kbc_failed(st)) goto fail;
        pos = wend;
        continue;
      }
      bool image = false;
      size_t toff = 0, tlen2 = 0, goff = 0, glen = 0;
      const size_t end =
          md_link(text, len, pos, &image, &toff, &tlen2, &goff, &glen);
      if (end > 0) {
        if (!image) {
          size_t idx = SIZE_MAX;
          st = push_link(&p, text + goff, glen, text + toff, tlen2, &idx);
          if (kbc_failed(st)) goto fail;
        }
        st = feed_inline(&p, text, toff, toff + tlen2);
        if (kbc_failed(st)) goto fail;
        pos = end;
        continue;
      }
    }
    if (c == '<') {
      size_t before = pos;
      st = p_tag(&p, text, len, &pos);
      if (kbc_failed(st)) goto fail;
      if (pos == before) st = KBC_ERR_INTERNAL;
      if (kbc_failed(st)) goto fail;
      continue;
    }
    if (c == '&') {
      /* Decode first, then feed the bytes through the ordinary append path so
       * that a decoded space collapses like a literal one. */
      kbc_str dec;
      kbc_str_init(&dec);
      size_t used = 0;
      st = decode_entity(&dec, text, len, pos, &used);
      if (!kbc_failed(st)) {
        for (size_t x = 0; x < dec.len; x++) {
          st = p_put_char(&p, (unsigned char)dec.ptr[x]);
          if (kbc_failed(st)) break;
        }
      }
      kbc_str_free(&dec);
      if (kbc_failed(st)) goto fail;
      pos += used;
      continue;
    }
    if (is_space(c)) { /* a newline is whitespace here: a blank line is
                        * consumed by p_line, never as whitespace */
      st = p_space(&p);
      if (kbc_failed(st)) goto fail;
      pos++;
      continue;
    }
    st = p_put_char(&p, c);
    if (kbc_failed(st)) goto fail;
    pos++;
  }
  st = p_flush(&p);
  if (kbc_failed(st)) goto fail;

  /* After the block scan, and writing only p.out->metas: the metadata a
   * document declares is read without touching a byte of what the index
   * tokenizes, which is the invariant the link and block tests pin. */
  st = extract_metas(a, text, len, &p.out->metas);
  if (kbc_failed(st)) goto fail;

  const char *title = p.title_h1 != NULL   ? p.title_h1
                      : p.title_tag != NULL ? p.title_tag
                      : p.title_prose != NULL ? p.title_prose
                                              : kbc_title_from_path(
                                                    a, rel_path ? rel_path : "", "");
  p.out->title = kbc_arena_strdup(a, title ? title : "");
  kbc_str_free(&p.cur);
  kbc_str_free(&p.title_buf);
  kbc_str_free(&p.anchor_buf);
  return p.out;

fail:
  kbc_str_free(&p.cur);
  kbc_str_free(&p.title_buf);
  kbc_str_free(&p.anchor_buf);
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

void kbc_tokens_init(kbc_tokens *t) {
  if (t == NULL) {
    return;
  }
  t->items = NULL;
  t->len = 0;
  t->cap = 0;
}

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
