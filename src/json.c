/* json.c — strict RFC 8259 parser and arena-owned JSON tree.
 *
 * Two invariants drive the whole file:
 *   1. The parser reads only [base, base+len). It never calls strlen on input
 *      and never peeks past `end`; every error message carries the byte offset
 *      from `base`.
 *   2. Nothing here mallocs. The tree, the keys, the item vectors and the
 *      unescaped string bodies all live in the caller's arena, so a request
 *      frees itself.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "kbc/json.h"

/* Stack safety: each nesting level costs one parse_value frame. */
#define JSON_MAX_DEPTH 64

/* Largest finite double; a magnitude above this is a range error, not JSON. */
#define JSON_MAX_MAGNITUDE 1.7976931348623157e308

typedef struct {
  const char *base; /* first input byte — the origin for error offsets */
  const char *p;    /* cursor */
  const char *end;  /* base + len, never crossed */
  kbc_arena *a;
  kbc_err *err;
} jparse;

static kbc_json *parse_value(jparse *s, unsigned depth);

static size_t off(const jparse *s, const char *p) { return (size_t)(p - s->base); }

static void fail_at(jparse *s, const char *p, const char *fmt, ...) {
  if (!s->err || s->err->status != KBC_OK) return;
  char what[160];
  va_list ap;
  va_start(ap, fmt);
  (void)vsnprintf(what, sizeof what, fmt, ap);
  va_end(ap);
  kbc_err_set(s->err, KBC_ERR_PARSE, "%s at byte %zu", what, off(s, p));
}

/* ---------------------------------------------------------------- nodes -- */

static kbc_json *node_new(kbc_arena *a, kbc_json_type t) {
  /* calloc, not alloc: the arena hands back recycled bytes, and every field
   * below left uninitialised (arr.len/cap/items, obj.len/cap/keys) is read
   * by the first push/set on a programmatically built node. */
  kbc_json *v = kbc_arena_calloc(a, 1, sizeof *v);
  if (!v) return NULL;
  v->type = t;
  return v;
}

kbc_json *kbc_json_new_null(kbc_arena *a) { return node_new(a, KBC_JSON_NULL); }

kbc_json *kbc_json_new_bool(kbc_arena *a, bool b) {
  kbc_json *v = node_new(a, KBC_JSON_BOOL);
  if (v) v->u.boolean = b;
  return v;
}

kbc_json *kbc_json_new_num(kbc_arena *a, double d) {
  kbc_json *v = node_new(a, KBC_JSON_NUM);
  if (v) v->u.num = d;
  return v;
}

kbc_json *kbc_json_new_strn(kbc_arena *a, const char *s, size_t n) {
  kbc_json *v = node_new(a, KBC_JSON_STR);
  if (!v) return NULL;
  if (!s) {
    s = "";
    n = 0;
  }
  char *copy = kbc_arena_strndup(a, s, n);
  if (!copy) return NULL;
  v->u.str.ptr = copy;
  v->u.str.len = n;
  return v;
}

kbc_json *kbc_json_new_str(kbc_arena *a, const char *s) {
  return kbc_json_new_strn(a, s, s ? strlen(s) : 0);
}

kbc_json *kbc_json_new_arr(kbc_arena *a) {
  return node_new(a, KBC_JSON_ARR);
}

kbc_json *kbc_json_new_obj(kbc_arena *a) {
  return node_new(a, KBC_JSON_OBJ);
}

/* ------------------------------------------------------------- builders -- */

/* Doubling growth with an overflow-checked element count. */
static void *grow_vec(kbc_arena *a, size_t cap, size_t elem, size_t *ncap) {
  if (cap > SIZE_MAX / elem / 2) return NULL;
  *ncap = cap ? cap * 2 : 4;
  if (*ncap > SIZE_MAX / elem) return NULL;
  return kbc_arena_calloc(a, *ncap, elem);
}

kbc_status kbc_json_arr_push(kbc_arena *a, kbc_json *arr, kbc_json *val) {
  if (!a || !arr || !val)
    return kbc_err_set(NULL, KBC_ERR_INVALID, "json_arr_push: null %s",
                       !a ? "arena" : !arr ? "array" : "value");
  if (arr->type != KBC_JSON_ARR)
    return kbc_err_set(NULL, KBC_ERR_INVALID, "json_arr_push: node type %d is not an array",
                       (int)arr->type);
  if (arr->u.arr.len == arr->u.arr.cap) {
    size_t ncap = 0;
    kbc_json **items = grow_vec(a, arr->u.arr.cap, sizeof *items, &ncap);
    if (!items)
      return kbc_err_set(NULL, KBC_ERR_NOMEM, "json_arr_push: %zu elements",
                         arr->u.arr.len + 1);
    if (arr->u.arr.items)
      memcpy(items, arr->u.arr.items, arr->u.arr.len * sizeof *items);
    arr->u.arr.items = items;
    arr->u.arr.cap = ncap;
  }
  arr->u.arr.items[arr->u.arr.len++] = val;
  return KBC_OK;
}

kbc_status kbc_json_obj_set(kbc_arena *a, kbc_json *obj, const char *key,
                            kbc_json *val) {
  if (!a || !obj || !key || !val)
    return kbc_err_set(NULL, KBC_ERR_INVALID, "json_obj_set: null %s",
                       !a ? "arena" : !obj ? "object" : !key ? "key" : "value");
  if (obj->type != KBC_JSON_OBJ)
    return kbc_err_set(NULL, KBC_ERR_INVALID,
                       "json_obj_set: node type %d is not an object",
                       (int)obj->type);
  size_t klen = strlen(key);
  for (size_t i = 0; i < obj->u.obj.len; i++) {
    if (obj->u.obj.key_lens[i] == klen &&
        memcmp(obj->u.obj.keys[i], key, klen) == 0) {
      obj->u.obj.vals[i] = val; /* overwrite in place, keep the first key copy */
      return KBC_OK;
    }
  }
  if (obj->u.obj.len == obj->u.obj.cap) {
    size_t ncap = 0;
    char **keys = grow_vec(a, obj->u.obj.cap, sizeof *keys, &ncap);
    size_t *klens = keys ? grow_vec(a, obj->u.obj.cap, sizeof *klens, &ncap)
                         : NULL;
    kbc_json **vals =
        klens ? grow_vec(a, obj->u.obj.cap, sizeof *vals, &ncap) : NULL;
    if (!vals)
      return kbc_err_set(NULL, KBC_ERR_NOMEM, "json_obj_set: %zu entries",
                         obj->u.obj.len + 1);
    if (obj->u.obj.keys) {
      memcpy(keys, obj->u.obj.keys, obj->u.obj.len * sizeof *keys);
      memcpy(klens, obj->u.obj.key_lens, obj->u.obj.len * sizeof *klens);
      memcpy(vals, obj->u.obj.vals, obj->u.obj.len * sizeof *vals);
    }
    obj->u.obj.keys = keys;
    obj->u.obj.key_lens = klens;
    obj->u.obj.vals = vals;
    obj->u.obj.cap = ncap;
  }
  char *kcopy = kbc_arena_strndup(a, key, klen);
  if (!kcopy)
    return kbc_err_set(NULL, KBC_ERR_NOMEM, "json_obj_set: key copy");
  size_t i = obj->u.obj.len++;
  obj->u.obj.keys[i] = kcopy;
  obj->u.obj.key_lens[i] = klen;
  obj->u.obj.vals[i] = val;
  return KBC_OK;
}

/* ------------------------------------------------------------- accessors -- */

kbc_json_type kbc_json_type_of(const kbc_json *v) {
  return v ? v->type : KBC_JSON_NULL;
}

bool kbc_json_is(const kbc_json *v, kbc_json_type t) {
  return v && v->type == t;
}

kbc_json *kbc_json_get(const kbc_json *obj, const char *key) {
  if (!obj || obj->type != KBC_JSON_OBJ || !key) return NULL;
  size_t klen = strlen(key);
  /* Last duplicate wins, matching the parser and JS JSON.parse. */
  for (size_t i = obj->u.obj.len; i-- > 0;) {
    if (obj->u.obj.key_lens[i] == klen &&
        memcmp(obj->u.obj.keys[i], key, klen) == 0)
      return obj->u.obj.vals[i];
  }
  return NULL;
}

const kbc_json *kbc_json_at(const kbc_json *arr, size_t i) {
  if (!arr || arr->type != KBC_JSON_ARR || i >= arr->u.arr.len) return NULL;
  return arr->u.arr.items[i];
}

size_t kbc_json_len(const kbc_json *v) {
  if (!v) return 0;
  if (v->type == KBC_JSON_ARR) return v->u.arr.len;
  if (v->type == KBC_JSON_OBJ) return v->u.obj.len;
  return 0;
}

const char *kbc_json_str(const kbc_json *obj, const char *key,
                         const char *dflt) {
  const kbc_json *v = kbc_json_get(obj, key);
  return (v && v->type == KBC_JSON_STR) ? v->u.str.ptr : dflt;
}

double kbc_json_num(const kbc_json *obj, const char *key, double dflt) {
  const kbc_json *v = kbc_json_get(obj, key);
  return (v && v->type == KBC_JSON_NUM) ? v->u.num : dflt;
}

int64_t kbc_json_i64(const kbc_json *obj, const char *key, int64_t dflt) {
  const kbc_json *v = kbc_json_get(obj, key);
  if (!v || v->type != KBC_JSON_NUM) return dflt;
  double d = v->u.num;
  /* Out-of-range values saturate rather than invoke undefined behaviour. */
  if (d >= 9223372036854775808.0) return INT64_MAX;
  if (d <= -9223372036854775808.0) return INT64_MIN;
  return (int64_t)d;
}

bool kbc_json_bool(const kbc_json *obj, const char *key, bool dflt) {
  const kbc_json *v = kbc_json_get(obj, key);
  return (v && v->type == KBC_JSON_BOOL) ? v->u.boolean : dflt;
}

/* --------------------------------------------------------------- parser -- */

static bool at_end(const jparse *s) { return s->p >= s->end; }

static void skip_ws(jparse *s) {
  while (s->p < s->end) {
    char c = *s->p;
    if (c == ' ' || c == '\t' || c == '\n' || c == '\r')
      s->p++;
    else
      break;
  }
}

static int hexval(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

/* Reads exactly 4 hex digits at s->p, advancing it. */
static bool read_hex4(jparse *s, uint32_t *out) {
  if ((size_t)(s->end - s->p) < 4) return false;
  uint32_t v = 0;
  for (int i = 0; i < 4; i++) {
    int h = hexval(s->p[i]);
    if (h < 0) return false;
    v = (v << 4) | (uint32_t)h;
  }
  s->p += 4;
  *out = v;
  return true;
}

/* Appends one code point to a caller-managed arena buffer. `*len` and `*cap`
 * are updated in place. */
static bool buf_putc(kbc_arena *a, char **buf, size_t *len, size_t *cap,
                     char ch) {
  if (*len + 1 >= *cap) {
    size_t ncap = (*cap > SIZE_MAX / 2) ? SIZE_MAX : *cap * 2;
    if (ncap <= *len + 1) return false;
    char *nb = kbc_arena_alloc(a, ncap);
    if (!nb) return false;
    memcpy(nb, *buf, *len);
    *buf = nb;
    *cap = ncap;
  }
  (*buf)[(*len)++] = ch;
  return true;
}

static bool buf_put_cp(kbc_arena *a, char **buf, size_t *len, size_t *cap,
                       uint32_t cp) {
  char b[4];
  size_t n;
  if (cp < 0x80) {
    b[0] = (char)cp;
    n = 1;
  } else if (cp < 0x800) {
    b[0] = (char)(0xC0 | (cp >> 6));
    b[1] = (char)(0x80 | (cp & 0x3F));
    n = 2;
  } else if (cp < 0x10000) {
    b[0] = (char)(0xE0 | (cp >> 12));
    b[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
    b[2] = (char)(0x80 | (cp & 0x3F));
    n = 3;
  } else {
    b[0] = (char)(0xF0 | (cp >> 18));
    b[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
    b[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
    b[3] = (char)(0x80 | (cp & 0x3F));
    n = 4;
  }
  for (size_t i = 0; i < n; i++)
    if (!buf_putc(a, buf, len, cap, b[i])) return false;
  return true;
}

/* Parses a string literal with the cursor on the opening quote. The unescaped
 * body is arena-allocated and never aliases the input. Raw bytes that are not
 * escapes — including invalid UTF-8 — are copied through byte for byte: this
 * is a JSON parser, not a UTF-8 validator. */
static kbc_json *parse_string(jparse *s) {
  const char *open = s->p;
  s->p++;
  size_t cap = 16, len = 0;
  char *buf = kbc_arena_alloc(s->a, cap);
  if (!buf) {
    fail_at(s, open, "out of memory");
    return NULL;
  }
  while (true) {
    if (at_end(s)) {
      fail_at(s, s->end, "unterminated string");
      return NULL;
    }
    unsigned char c = (unsigned char)*s->p;
    if (c == '"') {
      s->p++;
      break;
    }
    if (c < 0x20) {
      fail_at(s, s->p, "raw control character 0x%02x in string", (unsigned)c);
      return NULL;
    }
    if (c != '\\') {
      if (!buf_putc(s->a, &buf, &len, &cap, (char)c)) {
        fail_at(s, s->p, "out of memory");
        return NULL;
      }
      s->p++;
      continue;
    }
    const char *esc = s->p;
    if ((size_t)(s->end - s->p) < 2) {
      fail_at(s, esc, "truncated escape");
      return NULL;
    }
    char e = s->p[1];
    s->p += 2;
    char simple = 0;
    switch (e) {
    case '"': simple = '"'; break;
    case '\\': simple = '\\'; break;
    case '/': simple = '/'; break;
    case 'b': simple = '\b'; break;
    case 'f': simple = '\f'; break;
    case 'n': simple = '\n'; break;
    case 'r': simple = '\r'; break;
    case 't': simple = '\t'; break;
    case 'u': break;
    default:
      fail_at(s, esc, "unknown escape \\%c", e);
      return NULL;
    }
    if (simple) {
      if (!buf_putc(s->a, &buf, &len, &cap, simple)) {
        fail_at(s, esc, "out of memory");
        return NULL;
      }
      continue;
    }
    uint32_t cp;
    if (!read_hex4(s, &cp)) {
      fail_at(s, esc, "bad \\u escape");
      return NULL;
    }
    if (cp >= 0xD800 && cp <= 0xDBFF) {
      if ((size_t)(s->end - s->p) < 2 || s->p[0] != '\\' || s->p[1] != 'u') {
        fail_at(s, esc, "unpaired high surrogate \\u%04X", (unsigned)cp);
        return NULL;
      }
      const char *low = s->p;
      s->p += 2;
      uint32_t lo;
      if (!read_hex4(s, &lo) || lo < 0xDC00 || lo > 0xDFFF) {
        fail_at(s, low, "unpaired high surrogate \\u%04X", (unsigned)cp);
        return NULL;
      }
      cp = 0x10000u + ((cp - 0xD800u) << 10) + (lo - 0xDC00u);
    } else if (cp >= 0xDC00 && cp <= 0xDFFF) {
      fail_at(s, esc, "unpaired low surrogate \\u%04X", (unsigned)cp);
      return NULL;
    }
    if (!buf_put_cp(s->a, &buf, &len, &cap, cp)) {
      fail_at(s, esc, "out of memory");
      return NULL;
    }
  }
  kbc_json *v = node_new(s->a, KBC_JSON_STR);
  if (!v) {
    fail_at(s, open, "out of memory");
    return NULL;
  }
  buf[len] = '\0';
  v->u.str.ptr = buf;
  v->u.str.len = len;
  return v;
}

/* Numbers: the RFC 8259 grammar is checked byte by byte, then the token goes
 * to strtod through an arena copy. The grammar check is what actually rejects
 * NaN, Infinity, 0x1, 01, 1. and .1 — strtod would happily accept them all. */
static kbc_json *parse_number(jparse *s) {
  const char *start = s->p;
  if (s->p < s->end && *s->p == '-') s->p++;
  if (at_end(s)) {
    fail_at(s, s->p, "truncated number");
    return NULL;
  }
  if (*s->p == '0') {
    s->p++; /* a leading zero may not be followed by more digits */
  } else if (*s->p >= '1' && *s->p <= '9') {
    while (s->p < s->end && *s->p >= '0' && *s->p <= '9') s->p++;
  } else {
    fail_at(s, s->p, "unexpected '%c' in number", (unsigned char)*s->p);
    return NULL;
  }
  if (s->p < s->end && *s->p == '.') {
    const char *dot = s->p++;
    if (at_end(s) || *s->p < '0' || *s->p > '9') {
      fail_at(s, dot, "missing fraction digits");
      return NULL;
    }
    while (s->p < s->end && *s->p >= '0' && *s->p <= '9') s->p++;
  }
  if (s->p < s->end && (*s->p == 'e' || *s->p == 'E')) {
    const char *ex = s->p++;
    if (s->p < s->end && (*s->p == '+' || *s->p == '-')) s->p++;
    if (at_end(s) || *s->p < '0' || *s->p > '9') {
      fail_at(s, ex, "missing exponent digits");
      return NULL;
    }
    while (s->p < s->end && *s->p >= '0' && *s->p <= '9') s->p++;
  }
  char *tok = kbc_arena_strndup(s->a, start, (size_t)(s->p - start));
  if (!tok) {
    fail_at(s, start, "out of memory");
    return NULL;
  }
  char *endp = NULL;
  double d = strtod(tok, &endp);
  if (!endp || *endp != '\0') {
    fail_at(s, start, "malformed number '%s'", tok);
    return NULL;
  }
  if (!(d == d) || d > JSON_MAX_MAGNITUDE || d < -JSON_MAX_MAGNITUDE) {
    fail_at(s, start, "number '%s' out of range", tok);
    return NULL;
  }
  kbc_json *v = node_new(s->a, KBC_JSON_NUM);
  if (!v) {
    fail_at(s, start, "out of memory");
    return NULL;
  }
  v->u.num = d;
  return v;
}

static bool lit(jparse *s, const char *word) {
  size_t n = strlen(word);
  if ((size_t)(s->end - s->p) < n) return false;
  if (memcmp(s->p, word, n) != 0) return false;
  s->p += n;
  return true;
}

static kbc_json *parse_array(jparse *s, unsigned depth) {
  const char *open = s->p++;
  kbc_json *arr = node_new(s->a, KBC_JSON_ARR);
  if (!arr) {
    fail_at(s, open, "out of memory");
    return NULL;
  }
  skip_ws(s);
  if (!at_end(s) && *s->p == ']') {
    s->p++;
    return arr;
  }
  while (true) {
    kbc_json *item = parse_value(s, depth + 1);
    if (!item) return NULL;
    if (kbc_json_arr_push(s->a, arr, item) != KBC_OK) {
      fail_at(s, s->p, "out of memory growing array");
      return NULL;
    }
    skip_ws(s);
    if (at_end(s)) {
      fail_at(s, s->end, "unterminated array");
      return NULL;
    }
    if (*s->p == ',') {
      s->p++;
      skip_ws(s);
      if (!at_end(s) && *s->p == ']') {
        fail_at(s, s->p, "trailing comma in array");
        return NULL;
      }
      continue;
    }
    if (*s->p == ']') {
      s->p++;
      return arr;
    }
    fail_at(s, s->p, "unexpected '%c' in array", (unsigned char)*s->p);
    return NULL;
  }
}

static kbc_json *parse_object(jparse *s, unsigned depth) {
  const char *open = s->p++;
  kbc_json *obj = node_new(s->a, KBC_JSON_OBJ);
  if (!obj) {
    fail_at(s, open, "out of memory");
    return NULL;
  }
  skip_ws(s);
  if (!at_end(s) && *s->p == '}') {
    s->p++;
    return obj;
  }
  while (true) {
    if (at_end(s)) {
      fail_at(s, s->end, "unterminated object");
      return NULL;
    }
    if (*s->p != '"') {
      fail_at(s, s->p, "expected string key, got '%c'", (unsigned char)*s->p);
      return NULL;
    }
    kbc_json *key = parse_string(s);
    if (!key) return NULL;
    skip_ws(s);
    if (at_end(s) || *s->p != ':') {
      fail_at(s, s->p, "expected ':' after key");
      return NULL;
    }
    s->p++;
    kbc_json *val = parse_value(s, depth + 1);
    if (!val) return NULL;
    if (kbc_json_obj_set(s->a, obj, key->u.str.ptr, val) != KBC_OK) {
      fail_at(s, s->p, "out of memory storing key");
      return NULL;
    }
    skip_ws(s);
    if (at_end(s)) {
      fail_at(s, s->end, "unterminated object");
      return NULL;
    }
    if (*s->p == ',') {
      s->p++;
      skip_ws(s);
      if (!at_end(s) && *s->p == '}') {
        fail_at(s, s->p, "trailing comma in object");
        return NULL;
      }
      continue;
    }
    if (*s->p == '}') {
      s->p++;
      return obj;
    }
    fail_at(s, s->p, "unexpected '%c' in object", (unsigned char)*s->p);
    return NULL;
  }
}

static kbc_json *parse_value(jparse *s, unsigned depth) {
  /* The top-level value parses at depth 0, so JSON_MAX_DEPTH nested levels
   * below it are admitted and the next one is refused. */
  if (depth >= JSON_MAX_DEPTH) {
    fail_at(s, s->p, "nesting too deep");
    return NULL;
  }
  skip_ws(s);
  if (at_end(s)) {
    fail_at(s, s->end, "unexpected end of input");
    return NULL;
  }
  char c = *s->p;
  switch (c) {
  case '{': return parse_object(s, depth);
  case '[': return parse_array(s, depth);
  case '"': return parse_string(s);
  case 't':
    if (lit(s, "true")) return kbc_json_new_bool(s->a, true);
    fail_at(s, s->p, "expected 'true'");
    return NULL;
  case 'f':
    if (lit(s, "false")) return kbc_json_new_bool(s->a, false);
    fail_at(s, s->p, "expected 'false'");
    return NULL;
  case 'n':
    if (lit(s, "null")) return kbc_json_new_null(s->a);
    fail_at(s, s->p, "expected 'null'");
    return NULL;
  default:
    if (c == '-' || (c >= '0' && c <= '9')) return parse_number(s);
    fail_at(s, s->p, "unexpected '%c'", (unsigned char)c);
    return NULL;
  }
}

kbc_json *kbc_json_parse(kbc_arena *a, const char *text, size_t len,
                         kbc_err *err) {
  if (err) kbc_err_reset(err);
  if (!a || (!text && len > 0)) {
    kbc_err_set(err, KBC_ERR_INVALID, "json_parse: null %s",
                !a ? "arena" : "text");
    return NULL;
  }
  jparse s = {.base = text, .p = text, .end = text + len, .a = a, .err = err};
  kbc_json *v = parse_value(&s, 0);
  if (!v) {
    /* A NULL node with a KBC_PARSE error already carries the offset; when the
     * caller passed err == NULL there is nothing left to say. */
    return NULL;
  }
  skip_ws(&s);
  if (!at_end(&s)) {
    kbc_err_set(err, KBC_ERR_PARSE, "trailing garbage '%c' at byte %zu",
                (unsigned char)*s.p, off(&s, s.p));
    return NULL;
  }
  return v;
}

/* ---------------------------------------------------------- serializing -- */

kbc_status kbc_json_escape(kbc_str *out, const char *s, size_t n) {
  if (!out) return kbc_err_set(NULL, KBC_ERR_INVALID, "json_escape: null out");
  if (!s) {
    s = "";
    n = 0;
  }
  for (size_t i = 0; i < n; i++) {
    unsigned char c = (unsigned char)s[i];
    const char *esc = NULL;
    char ubuf[7];
    char one[2];
    switch (c) {
    case '"': esc = "\\\""; break;
    case '\\': esc = "\\\\"; break;
    case '\b': esc = "\\b"; break;
    case '\f': esc = "\\f"; break;
    case '\n': esc = "\\n"; break;
    case '\r': esc = "\\r"; break;
    case '\t': esc = "\\t"; break;
    default:
      if (c < 0x20) {
        (void)snprintf(ubuf, sizeof ubuf, "\\u%04x", (unsigned)c);
        esc = ubuf;
      } else {
        one[0] = (char)c;
        one[1] = '\0';
        esc = one;
      }
    }
    kbc_status st = kbc_str_append(out, esc, strlen(esc));
    if (st != KBC_OK)
      return kbc_err_set(NULL, st, "json_escape: append at byte %zu", i);
  }
  return KBC_OK;
}

static kbc_status dump_num(double d, kbc_str *out) {
  /* A non-finite or out-of-range double has no JSON representation; emit null
   * rather than the bare tokens `NaN` / `Infinity`, which are not JSON. */
  if (!(d == d) || d > JSON_MAX_MAGNITUDE || d < -JSON_MAX_MAGNITUDE)
    return kbc_str_puts(out, "null");
  char buf[40];
  int n = snprintf(buf, sizeof buf, "%.17g", d);
  if (n <= 0 || (size_t)n >= sizeof buf)
    return kbc_err_set(NULL, KBC_ERR_INTERNAL, "json_dump: number formatting");
  return kbc_str_append(out, buf, (size_t)n);
}

static kbc_status dump_val(const kbc_json *v, kbc_str *out, bool pretty,
                           unsigned depth, kbc_err *err);

static kbc_status indent_to(kbc_str *out, unsigned depth) {
  for (unsigned i = 0; i < depth; i++) {
    kbc_status st = kbc_str_puts(out, "  ");
    if (st != KBC_OK) return st;
  }
  return KBC_OK;
}

static kbc_status dump_str_body(const char *s, size_t n, kbc_str *out) {
  kbc_status st = kbc_str_putc(out, '"');
  if (st == KBC_OK) st = kbc_json_escape(out, s, n);
  if (st == KBC_OK) st = kbc_str_putc(out, '"');
  return st;
}

static kbc_status dump_val(const kbc_json *v, kbc_str *out, bool pretty,
                           unsigned depth, kbc_err *err) {
  if (depth >= JSON_MAX_DEPTH)
    return kbc_err_set(err, KBC_ERR_INTERNAL, "json_dump: tree deeper than %d",
                       JSON_MAX_DEPTH);
  kbc_status st = KBC_OK;
  if (!v) return kbc_str_puts(out, "null");
  switch (v->type) {
  case KBC_JSON_NULL:
    st = kbc_str_puts(out, "null");
    break;
  case KBC_JSON_BOOL:
    st = kbc_str_puts(out, v->u.boolean ? "true" : "false");
    break;
  case KBC_JSON_NUM:
    st = dump_num(v->u.num, out);
    break;
  case KBC_JSON_STR:
    st = dump_str_body(v->u.str.ptr, v->u.str.len, out);
    break;
  case KBC_JSON_ARR:
    st = kbc_str_putc(out, '[');
    for (size_t i = 0; i < v->u.arr.len && st == KBC_OK; i++) {
      if (i) st = kbc_str_putc(out, ',');
      if (st == KBC_OK && pretty) {
        st = kbc_str_putc(out, '\n');
        if (st == KBC_OK) st = indent_to(out, depth + 1);
      }
      if (st == KBC_OK)
        st = dump_val(v->u.arr.items[i], out, pretty, depth + 1, err);
    }
    if (st == KBC_OK && pretty && v->u.arr.len) {
      st = kbc_str_putc(out, '\n');
      if (st == KBC_OK) st = indent_to(out, depth);
    }
    if (st == KBC_OK) st = kbc_str_putc(out, ']');
    break;
  case KBC_JSON_OBJ:
    st = kbc_str_putc(out, '{');
    for (size_t i = 0; i < v->u.obj.len && st == KBC_OK; i++) {
      if (i) st = kbc_str_putc(out, ',');
      if (st == KBC_OK && pretty) {
        st = kbc_str_putc(out, '\n');
        if (st == KBC_OK) st = indent_to(out, depth + 1);
      }
      if (st == KBC_OK) {
        st = dump_str_body(v->u.obj.keys[i], v->u.obj.key_lens[i], out);
        if (st == KBC_OK) st = kbc_str_putc(out, ':');
        if (st == KBC_OK && pretty) st = kbc_str_putc(out, ' ');
      }
      if (st == KBC_OK)
        st = dump_val(v->u.obj.vals[i], out, pretty, depth + 1, err);
    }
    if (st == KBC_OK && pretty && v->u.obj.len) {
      st = kbc_str_putc(out, '\n');
      if (st == KBC_OK) st = indent_to(out, depth);
    }
    if (st == KBC_OK) st = kbc_str_putc(out, '}');
    break;
  }
  if (st != KBC_OK)
    return kbc_err_set(err, st, "json_dump: output append failed");
  return KBC_OK;
}

kbc_status kbc_json_dump(const kbc_json *v, kbc_str *out, bool pretty,
                         kbc_err *err) {
  if (err) kbc_err_reset(err);
  if (!out) return kbc_err_set(err, KBC_ERR_INVALID, "json_dump: null out");
  return dump_val(v, out, pretty, 0, err);
}
