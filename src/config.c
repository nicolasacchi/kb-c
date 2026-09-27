/* config.c — kb.toml: a strict TOML subset, parsed into a flat kbc_config.
 *
 * Two invariants drive the shape of this file:
 *
 *  1. Nothing is applied until the whole file has parsed. A config the daemon
 *     does not fully understand must leave the defaults standing, not half of
 *     them replaced (AGENTS.md rule 10). So the parser stages an overlay and
 *     cfg_apply() is the single place where the overlay lands.
 *  2. Every rejection names file:line and says which construct is not
 *     supported. Silently skipping a construct would run the daemon on a
 *     config the operator did not write.
 */
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L /* realpath */
#endif


#include <stdio.h>

#include <arpa/inet.h>
#include <errno.h>
#include <limits.h>
#include <netinet/in.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "kbc/config.h"

/* Bounds from the port spec, kept next to the parser that enforces them. */
#define KBC_CFG_MAX_FILE_BYTES 1048576u
#define KBC_CFG_MAX_LINE_BYTES 65536u
#define KBC_CFG_MAX_KEY_LEN 64u
#define KBC_CFG_MAX_DEBOUNCE_MS 60000u
#define KBC_CFG_MAX_WORKERS 256u
#define KBC_CFG_MAX_CHUNK_BYTES (64u * 1024u * 1024u)
#define KBC_CFG_MAX_RRF_K 1000
/* A single table may not define more distinct keys than the format defines;
 * the bound is what keeps the seen-key set below from being unbounded. */
#define CFG_MAX_SEEN_KEYS 32u

/* The tables and keys this build understands, as a closed set. */
typedef enum {
  SEC_NONE = 0,
  SEC_DAEMON,
  SEC_SEARCH,
  SEC_WATCHER,
  SEC_EMBEDDER,
  SEC_CORPUS
} cfg_section;

typedef enum {
  V_STR = 0,
  V_INT,
  V_FLOAT,
  V_BOOL,
  V_ARRAY
} cfg_vtype;

typedef struct {
  cfg_vtype t;
  char *s;         /* V_STR, KBC_OWN */
  int64_t i;       /* V_INT */
  double f;        /* V_FLOAT */
  bool b;          /* V_BOOL */
  kbc_strlist arr; /* V_ARRAY, KBC_OWN */
  const char *text; /* the literal value, for error messages */
} cfg_value;

typedef struct {
  kbc_corpus_cfg cfg;
  bool has_ignore;
} cfg_corpus_stage;
/* One key already applied in the current table. Bounded by CFG_MAX_SEEN_KEYS,
 * well above the number of keys the format actually defines. */
typedef struct {
  char name[KBC_CFG_MAX_KEY_LEN + 1u];
} cfg_seen_key;


/* The overlay: unset fields are NULL, or false under their has_* flag. */
typedef struct {
  char *bind_addr;
  char *data_dir;
  char *db_path;
  char *index_path;
  char *token_path;
  char *embedder_cmd;

  bool has_port;
  int64_t port;

  bool has_log_level;
  kbc_log_level log_level;

  bool has_json_logs;
  bool json_logs;

  bool has_workers;
  int64_t workers;

  bool has_chunk_max;
  int64_t chunk_max_bytes;

  bool has_bm25_k1;
  double bm25_k1;
  bool has_bm25_b;
  double bm25_b;
  bool has_rrf_k;
  int64_t rrf_k;
  bool has_graph_boost;
  double graph_boost;
  bool has_max_hits;
  int64_t max_hits;

  bool has_debounce_ms;
  int64_t debounce_ms;

  cfg_corpus_stage *corpora;
  size_t ncorpora;
  size_t ccorpora; /* capacity */
  bool any_corpus;
  /* Keys already applied in the current table; cleared at every table header,
   * including each new [[corpus]] block, which is its own namespace. */
  cfg_seen_key seen[CFG_MAX_SEEN_KEYS];
  size_t nseen;
} cfg_overlay;

/* ------------------------------------------------------------- helpers -- */

static char *cfg_strdup(const char *s) {
  size_t n;
  char *p;
  if (s == NULL) {
    return NULL;
  }
  n = strlen(s);
  p = (char *)malloc(n + 1u);
  if (p == NULL) {
    return NULL;
  }
  memcpy(p, s, n + 1u);
  return p;
}

/* The single error path for the parser: "file:line: what was wrong". */
static kbc_status cfg_err_at(const char *file, size_t line, kbc_err *err,
                             kbc_status st, const char *fmt, ...) {
  char body[KBC_ERR_MSG_MAX];
  char full[KBC_ERR_MSG_MAX];
  va_list ap;

  va_start(ap, fmt);
  (void)vsnprintf(body, sizeof body, fmt, ap);
  va_end(ap);
  /* KBC_ERR_MSG_MAX is smaller than file+line+body, so each field is given an
   * explicit bound: truncation is the API's, not an accident. */
  (void)snprintf(full, sizeof full, "%.100s:%zu:%.100s", file, line, body);
  full[sizeof full - 1u] = '\0';
  return kbc_err_set(err, st, "%s", full);
}

static void cfg_value_free(cfg_value *v) {
  if (v == NULL) {
    return;
  }
  free(v->s);
  v->s = NULL;
  kbc_strlist_free(&v->arr);
}

static void cfg_overlay_free(cfg_overlay *ov) {
  size_t i;
  if (ov == NULL) {
    return;
  }
  free(ov->bind_addr);
  free(ov->data_dir);
  free(ov->db_path);
  free(ov->index_path);
  free(ov->token_path);
  free(ov->embedder_cmd);
  for (i = 0; i < ov->ncorpora; i++) {
    free(ov->corpora[i].cfg.name);
    free(ov->corpora[i].cfg.path);
    kbc_strlist_free(&ov->corpora[i].cfg.ignore);
  }
  free(ov->corpora);
  memset(ov, 0, sizeof *ov);
}

static kbc_status cfg_corpus_push(cfg_overlay *ov, const char *file,
                                  size_t line, kbc_err *err) {
  if (ov->ncorpora == ov->ccorpora) {
    size_t cap = ov->ccorpora ? ov->ccorpora * 2u : 4u;
    cfg_corpus_stage *p;
    if (ov->ncorpora >= KBC_MAX_CORPORA) {
      return cfg_err_at(file, line, err, KBC_ERR_INVALID,
                        "more than %u [[corpus]] tables in one file",
                        KBC_MAX_CORPORA);
    }
    if (cap > KBC_MAX_CORPORA) {
      cap = KBC_MAX_CORPORA;
    }
    p = (cfg_corpus_stage *)realloc(ov->corpora, cap * sizeof *ov->corpora);
    if (p == NULL) {
      return cfg_err_at(file, line, err, KBC_ERR_NOMEM,
                        "out of memory growing corpora for the [[corpus]] at "
                        "this line");
    }
    ov->corpora = p;
    ov->ccorpora = cap;
  }
  memset(&ov->corpora[ov->ncorpora], 0, sizeof ov->corpora[ov->ncorpora]);
  kbc_strlist_init(&ov->corpora[ov->ncorpora].cfg.ignore);
  ov->ncorpora++;
  ov->any_corpus = true;
  return KBC_OK;
}

static bool cfg_is_space(char c) { return c == ' ' || c == '\t' || c == '\r'; }

static bool cfg_is_bare_key_char(char c) {
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
         (c >= '0' && c <= '9') || c == '_' || c == '-';
}

/* Trims in place from both ends; *off receives where the content starts. */
static size_t cfg_trim(const char *s, size_t n, size_t *off) {
  size_t i = 0;
  while (i < n && cfg_is_space(s[i])) {
    i++;
  }
  while (n > i && cfg_is_space(s[n - 1u])) {
    n--;
  }
  *off = i;
  return n - i;
}

/* Number of bytes of `s` starting at `i` shown in an error message. */
static int cfg_show(size_t n) { return (int)(n > 24u ? 24u : n); }

/* Truncates the line at the first '#' that is not inside a quoted string. */
static size_t cfg_strip_comment(const char *s, size_t n) {
  size_t i = 0;
  while (i < n) {
    char c = s[i];
    if (c == '"' || c == '\'') {
      char q = c;
      i++;
      while (i < n && s[i] != q) {
        if (q == '"' && s[i] == '\\' && i + 1u < n) {
          i += 2u;
          continue;
        }
        i++;
      }
      if (i < n) {
        i++;
      }
      continue;
    }
    if (c == '#') {
      return i;
    }
    i++;
  }
  return n;
}

/* --------------------------------------------------------- lexing bits -- */

/* Scans a quoted string starting at s[0] (the quote). *consumed receives the
 * byte count including both quotes. Multi-line forms are rejected by name. */
static kbc_status cfg_scan_string(const char *s, size_t n, const char *file,
                                  size_t line, char **out, size_t *consumed,
                                  kbc_err *err) {
  char quote = s[0];
  size_t i = 1;
  size_t cap = 16;
  size_t len = 0;
  char *buf;

  if (n >= 3u && s[1] == quote && s[2] == quote) {
    return cfg_err_at(file, line, err, KBC_ERR_PARSE,
                      "multi-line strings are not supported, found %c%c%c", quote,
                      quote, quote);
  }
  buf = (char *)malloc(cap);
  if (buf == NULL) {
    return cfg_err_at(file, line, err, KBC_ERR_NOMEM,
                      "out of memory reading the string on this line");
  }
  while (i < n && s[i] != quote) {
    unsigned char c = (unsigned char)s[i];
    if (c == '\n') {
      free(buf);
      return cfg_err_at(file, line, err, KBC_ERR_PARSE,
                        "unterminated string on this line");
    }
    if (quote == '"' && c == '\\') {
      if (i + 1u >= n) {
        free(buf);
        return cfg_err_at(file, line, err, KBC_ERR_PARSE,
                          "dangling escape at end of string");
      }
      i++;
      switch (s[i]) {
        case 'n': c = '\n'; break;
        case 't': c = '\t'; break;
        case 'r': c = '\r'; break;
        case '"': c = '"'; break;
        case '\'': c = '\''; break;
        case '\\': c = '\\'; break;
        case '0': c = '\0'; break;
        default:
          free(buf);
          return cfg_err_at(file, line, err, KBC_ERR_PARSE,
                            "unsupported escape \\%c in string", s[i]);
      }
    }
    if (len + 1u >= cap) {
      char *nb;
      if (cap > (size_t)KBC_MAX_ARTIFACT_BYTES) {
        free(buf);
        return cfg_err_at(file, line, err, KBC_ERR_INVALID,
                          "string literal exceeds the %u-byte maximum",
                          KBC_MAX_ARTIFACT_BYTES);
      }
      cap *= 2u;
      nb = (char *)realloc(buf, cap);
      if (nb == NULL) {
        free(buf);
        return cfg_err_at(file, line, err, KBC_ERR_NOMEM,
                          "out of memory growing the string on this line");
      }
      buf = nb;
    }
    buf[len++] = (char)c;
    i++;
  }
  if (i >= n) {
    free(buf);
    return cfg_err_at(file, line, err, KBC_ERR_PARSE,
                      "unterminated string: missing closing %c", quote);
  }
  buf[len] = '\0';
  *out = buf;
  *consumed = i + 1u;
  return KBC_OK;
}

/* Scans ["a", "b"]. An element of any other type is a parse error naming
 * what was found, because a mixed array means the operator expects a shape
 * this build does not implement. */
static kbc_status cfg_scan_array(const char *s, size_t n, const char *file,
                                 size_t line, cfg_value *v, kbc_err *err) {
  size_t i = 1;
  kbc_status st;

  kbc_strlist_init(&v->arr);
  v->t = V_ARRAY;
  for (;;) {
    size_t off = 0;
    size_t m;
    char c;

    if (i > n) {
      return cfg_err_at(file, line, err, KBC_ERR_PARSE,
                        "unterminated array on this line");
    }
    m = cfg_trim(s + i, n - i, &off);
    if (m == 0) {
      return cfg_err_at(file, line, err, KBC_ERR_PARSE,
                        "unterminated array on this line");
    }
    c = s[i + off];
    if (c == ']') {
      return KBC_OK;
    }
    if (c == ',') {
      i += off + 1u;
      continue;
    }
    if (c != '"' && c != '\'') {
      return cfg_err_at(file, line, err, KBC_ERR_PARSE,
                        "only arrays of strings are supported, found %.*s",
                        cfg_show(m), s + i + off);
    }
    {
      char *item = NULL;
      size_t used = 0;
      st = cfg_scan_string(s + i + off, m - off, file, line, &item, &used,
                           err);
      if (st != KBC_OK) {
        return st;
      }
      st = kbc_strlist_push(&v->arr, item);
      free(item);
      if (st != KBC_OK) {
        return cfg_err_at(file, line, err, KBC_ERR_NOMEM,
                          "out of memory building the array on this line");
      }
      i += off + used;
    }
    m = cfg_trim(s + i, n - i, &off);
    if (m == 0) {
      return cfg_err_at(file, line, err, KBC_ERR_PARSE,
                        "unterminated array on this line");
    }
    if (s[i + off] == ',') {
      i += off + 1u;
      continue;
    }
    if (s[i + off] == ']') {
      return KBC_OK;
    }
    return cfg_err_at(file, line, err, KBC_ERR_PARSE,
                      "expected ',' or ']' in array, found %.*s",
                      cfg_show(m), s + i + off);
  }
}

static kbc_status cfg_parse_value(const char *s, size_t n, const char *file,
                                  size_t line, cfg_value *v, kbc_err *err) {
  size_t off = 0;
  size_t m = cfg_trim(s, n, &off);
  const char *p;
  char tmp[64];
  char *end = NULL;

  memset(v, 0, sizeof *v);
  if (m == 0) {
    return cfg_err_at(file, line, err, KBC_ERR_PARSE, "key has no value");
  }
  p = s + off;
  v->text = p;

  if (p[0] == '[') {
    return cfg_scan_array(p, m, file, line, v, err);
  }
  if (p[0] == '{') {
    return cfg_err_at(file, line, err, KBC_ERR_PARSE,
                      "inline tables are not supported");
  }
  if (p[0] == '"' || p[0] == '\'') {
    size_t used = 0;
    size_t after = 0;
    kbc_status st = cfg_scan_string(p, m, file, line, &v->s, &used, err);
    if (st != KBC_OK) {
      return st;
    }
    v->t = V_STR;
    (void)cfg_trim(p + used, m - used, &after);
    if (after != 0) {
      cfg_value_free(v);
      return cfg_err_at(file, line, err, KBC_ERR_PARSE,
                        "trailing characters after string value: %.*s",
                        cfg_show(after), p + used);
    }
    return KBC_OK;
  }
  if (m >= sizeof tmp) {
    return cfg_err_at(file, line, err, KBC_ERR_PARSE,
                      "unquoted value is too long: %.*s", cfg_show(m), p);
  }
  memcpy(tmp, p, m);
  tmp[m] = '\0';

  if (strcmp(tmp, "true") == 0) {
    v->t = V_BOOL;
    v->b = true;
    return KBC_OK;
  }
  if (strcmp(tmp, "false") == 0) {
    v->t = V_BOOL;
    v->b = false;
    return KBC_OK;
  }
  /* A date-time starts with four digits and a dash. Reject it by name
   * instead of letting strtod stop at the first non-digit. */
  if (m >= 5u && tmp[0] >= '0' && tmp[0] <= '9' && tmp[1] >= '0' &&
      tmp[1] <= '9' && tmp[2] >= '0' && tmp[2] <= '9' && tmp[3] >= '0' &&
      tmp[3] <= '9' && tmp[4] == '-') {
    return cfg_err_at(file, line, err, KBC_ERR_PARSE,
                      "date-time values are not supported: %s", tmp);
  }

  if (strchr(tmp, '.') == NULL && strchr(tmp, 'e') == NULL &&
      strchr(tmp, 'E') == NULL) {
    long long ll = strtoll(tmp, &end, 10);
    if (end != NULL && end != tmp && *end == '\0') {
      v->t = V_INT;
      v->i = (int64_t)ll;
      return KBC_OK;
    }
  }
  {
    double d = strtod(tmp, &end);
    if (end != NULL && end != tmp && *end == '\0') {
      v->t = V_FLOAT;
      v->f = d;
      return KBC_OK;
    }
  }
  return cfg_err_at(file, line, err, KBC_ERR_PARSE,
                    "unsupported value %s (expected a string, integer, float, "
                    "boolean or array of strings)",
                    tmp);
}

/* Bare or quoted key into `out`. A dot is a dotted key, which this subset
 * does not implement. */
static kbc_status cfg_parse_key(const char *s, size_t n, const char *file,
                                size_t line, char *out, size_t out_cap,
                                kbc_err *err) {
  size_t off = 0;
  size_t m = cfg_trim(s, n, &off);
  const char *p = s + off;
  size_t i;

  if (m == 0) {
    return cfg_err_at(file, line, err, KBC_ERR_PARSE, "empty key");
  }
  if (p[0] == '"' || p[0] == '\'') {
    char *q = NULL;
    size_t used = 0;
    size_t after = 0;
    kbc_status st = cfg_scan_string(p, m, file, line, &q, &used, err);
    if (st != KBC_OK) {
      return st;
    }
    (void)cfg_trim(p + used, m - used, &after);
    i = strlen(q);
    if (after != 0 || i + 1u > out_cap) {
      free(q);
      return cfg_err_at(file, line, err, KBC_ERR_PARSE,
                        "malformed key: %.*s", cfg_show(m), p);
    }
    memcpy(out, q, i + 1u);
    free(q);
    return KBC_OK;
  }
  for (i = 0; i < m && cfg_is_bare_key_char(p[i]); i++) {
    /* scan */
  }
  if (i == 0 || i + 1u > out_cap) {
    return cfg_err_at(file, line, err, KBC_ERR_PARSE, "malformed key %.*s",
                      cfg_show(m), p);
  }
  if (i != m) {
    if (p[i] == '.') {
      return cfg_err_at(file, line, err, KBC_ERR_PARSE,
                        "dotted keys are not supported");
    }
    return cfg_err_at(file, line, err, KBC_ERR_PARSE,
                      "trailing characters in key: %.*s",
                      cfg_show(m - i), p + i);
  }
  memcpy(out, p, i);
  out[i] = '\0';
  return KBC_OK;
}

static cfg_section cfg_section_named(const char *name) {
  if (strcmp(name, "daemon") == 0) {
    return SEC_DAEMON;
  }
  if (strcmp(name, "search") == 0) {
    return SEC_SEARCH;
  }
  if (strcmp(name, "watcher") == 0) {
    return SEC_WATCHER;
  }
  if (strcmp(name, "embedder") == 0) {
    return SEC_EMBEDDER;
  }
  if (strcmp(name, "corpus") == 0) {
    return SEC_CORPUS;
  }
  return SEC_NONE;
}

static const char *cfg_section_name(cfg_section s) {
  switch (s) {
    case SEC_DAEMON: return "daemon";
    case SEC_SEARCH: return "search";
    case SEC_WATCHER: return "watcher";
    case SEC_EMBEDDER: return "embedder";
    case SEC_CORPUS: return "corpus";
    case SEC_NONE: break;
  }
  return "none";
}

/* -------------------------------------------------------- applying keys -- */

static kbc_status cfg_need_str(const cfg_value *v, const char *key,
                               const char *file, size_t line, kbc_err *err) {
  if (v->t != V_STR) {
    return cfg_err_at(file, line, err, KBC_ERR_PARSE,
                      "key %s expects a string, found %s", key, v->text);
  }
  return KBC_OK;
}

static kbc_status cfg_need_int(const cfg_value *v, const char *key,
                               const char *file, size_t line, kbc_err *err) {
  if (v->t != V_INT) {
    return cfg_err_at(file, line, err, KBC_ERR_PARSE,
                      "key %s expects an integer, found %s", key, v->text);
  }
  return KBC_OK;
}

static kbc_status cfg_need_num(const cfg_value *v, const char *key,
                               const char *file, size_t line, kbc_err *err) {
  if (v->t != V_FLOAT && v->t != V_INT) {
    return cfg_err_at(file, line, err, KBC_ERR_PARSE,
                      "key %s expects a number, found %s", key, v->text);
  }
  return KBC_OK;
}

static kbc_status cfg_take_str(char **slot, const cfg_value *v, const char *key,
                               const char *file, size_t line, kbc_err *err) {
  char *dup;
  kbc_status st = cfg_need_str(v, key, file, line, err);
  if (st != KBC_OK) {
    return st;
  }
  dup = cfg_strdup(v->s);
  if (dup == NULL) {
    return cfg_err_at(file, line, err, KBC_ERR_NOMEM,
                      "out of memory copying the value of key \"%s\" (%s)", key,
                      v->s);
  }
  free(*slot);
  *slot = dup;
  return KBC_OK;
}

static kbc_status cfg_range(const char *key, int64_t val, int64_t lo, int64_t hi,
                            const char *file, size_t line, kbc_err *err) {
  if (val < lo || val > hi) {
    return cfg_err_at(file, line, err, KBC_ERR_INVALID,
                      "%s = %lld is outside %lld..%lld", key,
                      (long long)val, (long long)lo, (long long)hi);
  }
  return KBC_OK;
}
/* Records `key` as seen in the current table, or rejects it if this table
 * already carried it. A repeated key is a parse error, never a silent
 * last-one-wins: a config the daemon does not fully understand must not be
 * half-applied. State lives in the overlay, which is per-file, and is reset
 * at every table header — so the same key may appear once per table, and
 * `name`/`path`/`ignore` once per [[corpus]] block. */
static kbc_status cfg_note_key(cfg_overlay *ov, const char *key,
                               const char *file, size_t line, kbc_err *err) {
  size_t i;
  for (i = 0; i < ov->nseen; i++) {
    if (strcmp(ov->seen[i].name, key) == 0) {
      return cfg_err_at(file, line, err, KBC_ERR_PARSE,
                        "key \"%s\" is already set earlier in this table", key);
    }
  }
  if (ov->nseen >= CFG_MAX_SEEN_KEYS) {
    return cfg_err_at(file, line, err, KBC_ERR_PARSE,
                      "table defines more than %u keys (at key \"%s\")",
                      CFG_MAX_SEEN_KEYS, key);
  }
  memcpy(ov->seen[ov->nseen].name, key, strlen(key) + 1u);
  ov->nseen++;
  return KBC_OK;
}


static kbc_status cfg_apply_key(cfg_overlay *ov, cfg_section sec,
                                cfg_corpus_stage *corpus, const char *key,
                                cfg_value *v, const char *file, size_t line,
                                kbc_err *err) {
  kbc_status st;
  if (sec == SEC_CORPUS) {
    if (corpus == NULL) {
      return cfg_err_at(file, line, err, KBC_ERR_PARSE,
                        "key \"%s\" appears outside any [[corpus]] table", key);
    }
    if (strcmp(key, "name") == 0) {
      return cfg_take_str(&corpus->cfg.name, v, key, file, line, err);
    }
    if (strcmp(key, "path") == 0) {
      return cfg_take_str(&corpus->cfg.path, v, key, file, line, err);
    }
    if (strcmp(key, "ignore") == 0) {
      size_t i;
      if (v->t != V_ARRAY) {
        return cfg_err_at(file, line, err, KBC_ERR_PARSE,
                          "key ignore expects an array of strings, found %s",
                          v->text);
      }
      corpus->has_ignore = true;
      for (i = 0; i < v->arr.len; i++) {
        st = kbc_strlist_push(&corpus->cfg.ignore, v->arr.items[i]);
        if (st != KBC_OK) {
          return cfg_err_at(file, line, err, KBC_ERR_NOMEM,
                            "out of memory building the ignore list of "
                            "corpus \"%s\"",
                            corpus->cfg.name != NULL ? corpus->cfg.name : "(unnamed)");
        }
      }
      return KBC_OK;
    }
    return cfg_err_at(file, line, err, KBC_ERR_PARSE,
                      "unknown key \"%s\" in [[corpus]]", key);
  }

  if (sec == SEC_DAEMON) {
    if (strcmp(key, "bind") == 0) {
      return cfg_take_str(&ov->bind_addr, v, key, file, line, err);
    }
    if (strcmp(key, "port") == 0) {
      st = cfg_need_int(v, key, file, line, err);
      if (st != KBC_OK) {
        return st;
      }
      st = cfg_range(key, v->i, 1, 65535, file, line, err);
      if (st != KBC_OK) {
        return st;
      }
      ov->port = v->i;
      ov->has_port = true;
      return KBC_OK;
    }
    if (strcmp(key, "data_dir") == 0) {
      return cfg_take_str(&ov->data_dir, v, key, file, line, err);
    }
    if (strcmp(key, "db_path") == 0) {
      return cfg_take_str(&ov->db_path, v, key, file, line, err);
    }
    if (strcmp(key, "index_path") == 0) {
      return cfg_take_str(&ov->index_path, v, key, file, line, err);
    }
    if (strcmp(key, "token_file") == 0) {
      return cfg_take_str(&ov->token_path, v, key, file, line, err);
    }
    if (strcmp(key, "json_logs") == 0) {
      if (v->t != V_BOOL) {
        return cfg_err_at(file, line, err, KBC_ERR_PARSE,
                          "key json_logs expects a boolean, found %s", v->text);
      }
      ov->json_logs = v->b;
      ov->has_json_logs = true;
      return KBC_OK;
    }
    if (strcmp(key, "workers") == 0) {
      st = cfg_need_int(v, key, file, line, err);
      if (st != KBC_OK) {
        return st;
      }
      st = cfg_range(key, v->i, 1, KBC_CFG_MAX_WORKERS, file, line, err);
      if (st != KBC_OK) {
        return st;
      }
      ov->workers = v->i;
      ov->has_workers = true;
      return KBC_OK;
    }
    if (strcmp(key, "chunk_max_bytes") == 0) {
      st = cfg_need_int(v, key, file, line, err);
      if (st != KBC_OK) {
        return st;
      }
      st = cfg_range(key, v->i, 1024, (int64_t)KBC_CFG_MAX_CHUNK_BYTES,
                     file, line, err);
      if (st != KBC_OK) {
        return st;
      }
      ov->chunk_max_bytes = v->i;
      ov->has_chunk_max = true;
      return KBC_OK;
    }
    if (strcmp(key, "log_level") == 0) {
      kbc_log_level lvl = KBC_LOG_INFO;
      if (v->t != V_STR) {
        return cfg_err_at(file, line, err, KBC_ERR_PARSE,
                          "key log_level expects a string, found %s", v->text);
      }
      if (!kbc_log_level_parse(v->s, &lvl)) {
        return cfg_err_at(file, line, err, KBC_ERR_PARSE,
                          "log_level = %s is not one of error|warn|info|debug",
                          v->s);
      }
      ov->log_level = lvl;
      ov->has_log_level = true;
      return KBC_OK;
    }
  } else if (sec == SEC_SEARCH) {
    if (strcmp(key, "bm25_k1") == 0) {
      st = cfg_need_num(v, key, file, line, err);
      if (st != KBC_OK) {
        return st;
      }
      if (!(v->f > 0.0)) {
        return cfg_err_at(file, line, err, KBC_ERR_INVALID,
                          "bm25_k1 must be > 0, found %s", v->text);
      }
      ov->bm25_k1 = v->f;
      ov->has_bm25_k1 = true;
      return KBC_OK;
    }
    if (strcmp(key, "bm25_b") == 0) {
      st = cfg_need_num(v, key, file, line, err);
      if (st != KBC_OK) {
        return st;
      }
      if (!(v->f >= 0.0 && v->f <= 1.0)) {
        return cfg_err_at(file, line, err, KBC_ERR_INVALID,
                          "bm25_b must be within 0..1, found %s", v->text);
      }
      ov->bm25_b = v->f;
      ov->has_bm25_b = true;
      return KBC_OK;
    }
    if (strcmp(key, "rrf_k") == 0) {
      st = cfg_need_int(v, key, file, line, err);
      if (st != KBC_OK) {
        return st;
      }
      st = cfg_range(key, v->i, 1, KBC_CFG_MAX_RRF_K, file, line, err);
      if (st != KBC_OK) {
        return st;
      }
      ov->rrf_k = v->i;
      ov->has_rrf_k = true;
      return KBC_OK;
    }
    if (strcmp(key, "graph_boost") == 0) {
      st = cfg_need_num(v, key, file, line, err);
      if (st != KBC_OK) {
        return st;
      }
      /* 0 is not a weight, it is the absence of one: writing it means the
       * operator believes they are configuring something while the graph
       * stays off. Name that rather than accept a no-op. */
      if (!(v->f > 0.0 && v->f <= 4.0)) {
        return cfg_err_at(file, line, err, KBC_ERR_PARSE,
                          "key graph_boost must be within (0, 4], found %s "
                          "(0 means the graph is off, which is the default — "
                          "omit the key instead)",
                          v->text);
      }
      ov->graph_boost = v->f;
      ov->has_graph_boost = true;
      return KBC_OK;
    }
    if (strcmp(key, "max_hits") == 0) {
      st = cfg_need_int(v, key, file, line, err);
      if (st != KBC_OK) {
        return st;
      }
      st = cfg_range(key, v->i, 1, (int64_t)KBC_MAX_HITS, file, line, err);
      if (st != KBC_OK) {
        return st;
      }
      ov->max_hits = v->i;
      ov->has_max_hits = true;
      return KBC_OK;
    }
    if (strcmp(key, "chunk_max_bytes") == 0) {
      /* Also legal here: it bounds the search-time chunk copy, and
       * [[search]] is where an operator looks for it. */
      return cfg_apply_key(ov, SEC_DAEMON, NULL, key, v, file, line, err);
    }
  } else if (sec == SEC_WATCHER) {
    if (strcmp(key, "debounce_ms") == 0) {
      st = cfg_need_int(v, key, file, line, err);
      if (st != KBC_OK) {
        return st;
      }
      st = cfg_range(key, v->i, 0, (int64_t)KBC_CFG_MAX_DEBOUNCE_MS, file,
                     line, err);
      if (st != KBC_OK) {
        return st;
      }
      ov->debounce_ms = v->i;
      ov->has_debounce_ms = true;
      return KBC_OK;
    }
  } else if (sec == SEC_EMBEDDER) {
    if (strcmp(key, "command") == 0) {
      return cfg_take_str(&ov->embedder_cmd, v, key, file, line, err);
    }
  }

  return cfg_err_at(file, line, err, KBC_ERR_PARSE,
                    "unknown key \"%s\" in [%s]", key, cfg_section_name(sec));
}

/* ------------------------------------------------------------- parsing -- */

static kbc_status cfg_parse_line(cfg_overlay *ov, cfg_section *sec,
                                 cfg_corpus_stage **cur, const char *line,
                                 size_t n, size_t lineno, const char *file,
                                 kbc_err *err) {
  size_t off = 0;
  size_t m = cfg_strip_comment(line, n);
  char key[KBC_CFG_MAX_KEY_LEN + 1u];
  cfg_value v;
  size_t eq = 0;
  kbc_status st;

  m = cfg_trim(line, m, &off);
  line += off;
  if (m == 0) {
    return KBC_OK;
  }
  if (m > KBC_CFG_MAX_LINE_BYTES) {
    return cfg_err_at(file, lineno, err, KBC_ERR_INVALID,
                      "line is longer than %u bytes", KBC_CFG_MAX_LINE_BYTES);
  }

  if (line[0] == '[') {
    if (m >= 2u && line[1] == '[') {
      if (m < 5u || line[m - 1u] != ']' || line[m - 2u] != ']') {
        return cfg_err_at(file, lineno, err, KBC_ERR_PARSE,
                          "malformed array-of-tables header: %.*s", cfg_show(m),
                          line);
      }
      st = cfg_parse_key(line + 2, m - 4u, file, lineno, key, sizeof key, err);
      if (st != KBC_OK) {
        return st;
      }
      if (strcmp(key, "corpus") != 0) {
        return cfg_err_at(file, lineno, err, KBC_ERR_PARSE,
                          "arrays of tables are only supported for [[corpus]], "
                          "found [[%s]]",
                          key);
      }
      st = cfg_corpus_push(ov, file, lineno, err);
      if (st != KBC_OK) {
        return st;
      }
      *sec = SEC_CORPUS;
      ov->nseen = 0; /* a new [[corpus]] block is its own key namespace */
      *cur = &ov->corpora[ov->ncorpora - 1u];
      return KBC_OK;
    }
    if (line[m - 1u] != ']') {
      return cfg_err_at(file, lineno, err, KBC_ERR_PARSE,
                        "unterminated table header: %.*s", cfg_show(m), line);
    }
    st = cfg_parse_key(line + 1, m - 2u, file, lineno, key, sizeof key, err);
    if (st != KBC_OK) {
      return st;
    }
    ov->nseen = 0;
    *sec = cfg_section_named(key);
    *cur = NULL;
    if (*sec == SEC_NONE) {
      return cfg_err_at(file, lineno, err, KBC_ERR_PARSE, "unknown table [%s]",
                        key);
    }
    if (*sec == SEC_CORPUS) {
      return cfg_err_at(file, lineno, err, KBC_ERR_PARSE,
                        "corpora are declared with [[corpus]], not [corpus]: "
                        "[%s]",
                        key);
    }
    return KBC_OK;
  }

  /* Locate the '=' that separates key from value, skipping quoted keys. */
  {
    size_t i = 0;
    while (i < m && line[i] != '=') {
      if (line[i] == '"' || line[i] == '\'') {
        char q = line[i];
        for (i++; i < m && line[i] != q; i++) {
          /* advance */
        }
      }
      i++;
    }
    eq = i;
  }
  if (eq >= m) {
    return cfg_err_at(file, lineno, err, KBC_ERR_PARSE,
                      "expected \"key = value\", found %.*s", cfg_show(m),
                      line);
  }
  st = cfg_parse_key(line, eq, file, lineno, key, sizeof key, err);
  if (st != KBC_OK) {
    return st;
  }
  if (*sec == SEC_NONE) {
    return cfg_err_at(file, lineno, err, KBC_ERR_PARSE,
                      "key \"%s\" appears before any [table] header", key);
  }
  st = cfg_parse_value(line + eq + 1u, m - eq - 1u, file, lineno, &v, err);
  if (st != KBC_OK) {
    return st;
  }
  st = cfg_note_key(ov, key, file, lineno, err);
  if (st != KBC_OK) {
    cfg_value_free(&v);
    return st;
  }
  st = cfg_apply_key(ov, *sec, *cur, key, &v, file, lineno, err);
  cfg_value_free(&v);
  return st;
}

/* --------------------------------------------------------------- paths -- */

/* "<dir>/<name>" with exactly one separator. */
static char *cfg_join(const char *dir, const char *name) {
  size_t dl = strlen(dir);
  size_t nl = strlen(name);
  bool slash = dl > 0u && dir[dl - 1u] == '/';
  char *p = (char *)malloc(dl + nl + 2u);
  if (p == NULL) {
    return NULL;
  }
  memcpy(p, dir, dl);
  if (!slash) {
    p[dl] = '/';
    dl++;
  }
  memcpy(p + dl, name, nl + 1u);
  return p;
}

/* The path as the operator sees it, made absolute against the process's cwd.
 * Deliberately not realpath(): canonicalising here would make config_path
 * depend on symlinks, and a path the operator can paste back into a bug
 * report is worth more than one that is pretty. */
static char *cfg_abspath(const char *path) {
  char *cwd;
  char *out;
  if (path[0] == '/') {
    return cfg_strdup(path);
  }
  cwd = getcwd(NULL, 0);
  if (cwd == NULL) {
    return cfg_strdup(path);
  }
  out = cfg_join(cwd, path);
  free(cwd);
  return out;
}

static char *cfg_dirname_of(const char *path) {
  const char *slash = strrchr(path, '/');
  size_t n;
  char *d;
  if (slash == NULL) {
    return cfg_strdup(".");
  }
  if (slash == path) {
    return cfg_strdup("/");
  }
  n = (size_t)(slash - path);
  d = (char *)malloc(n + 1u);
  if (d == NULL) {
    return NULL;
  }
  memcpy(d, path, n);
  d[n] = '\0';
  return d;
}

/* Rejects a ".." component, which AGENTS.md rule 9 requires for anything that
 * reaches the filesystem. */
static bool cfg_has_dotdot(const char *p) {
  const char *s = p;
  while (*s != '\0') {
    if ((s == p || s[-1] == '/') && s[0] == '.' && s[1] == '.' &&
        (s[2] == '\0' || s[2] == '/')) {
      return true;
    }
    s++;
  }
  return false;
}

static bool cfg_is_dir(const char *path) {
  struct stat sb;
  if (stat(path, &sb) != 0) {
    return false;
  }
  return S_ISDIR(sb.st_mode);
}

/* ------------------------------------------------------------ applying -- */

static kbc_status cfg_put(char **slot, char *value, const char *what,
                          kbc_err *err) {
  if (value == NULL) {
    return kbc_err_set(err, KBC_ERR_NOMEM, "out of memory setting %s", what);
  }
  free(*slot);
  *slot = value;
  return KBC_OK;
}
/* A path as the config file's author wrote it, made absolute against the
 * config file's own directory: the corpus rule, applied to data_dir,
 * db_path, index_path and token_file too. Nothing a config names is relative
 * to whatever directory the operator happened to be standing in. */
static char *cfg_resolve(const char *dir, const char *p) {
  if (p[0] == '/') {
    return cfg_strdup(p);
  }
  return cfg_join(dir, p);
}

/* Move an overlay path into its config slot, resolved against `dir`. The
 * overlay's own copy dies here: cfg_put takes ownership of the resolved
 * string, and nulling the overlay slot without freeing it would leak the
 * original. */
static kbc_status cfg_take_path(char **slot, char *owned, const char *dir,
                                const char *what, kbc_err *err) {
  char *p = cfg_resolve(dir, owned);
  free(owned);
  return cfg_put(slot, p, what, err);
}

/* The one place the overlay lands. `dir` is the config file's directory,
 * against which every relative path in the file resolves. */
static kbc_status cfg_apply(kbc_config *cfg, cfg_overlay *ov, const char *dir,
                            const char *abs_path, kbc_err *err) {
  kbc_status st;
  bool data_dir_given = false;

  if (ov->bind_addr != NULL) {
    st = cfg_put(&cfg->bind_addr, ov->bind_addr, "bind", err);
    if (st != KBC_OK) {
      return st;
    }
    ov->bind_addr = NULL;
  }
  if (ov->data_dir != NULL) {
    st = cfg_take_path(&cfg->data_dir, ov->data_dir, dir, "data_dir", err);
    if (st != KBC_OK) {
      return st;
    }
    ov->data_dir = NULL;
    data_dir_given = true;
  }
  /* db, index and token follow data_dir by name unless the file pinned them,
   * so a config that only sets data_dir points every path it owns at one
   * directory. Deriving each independently (rather than all-or-nothing) is
   * what makes "data_dir + one explicit path" coherent too. */
  if (ov->db_path != NULL) {
    st = cfg_take_path(&cfg->db_path, ov->db_path, dir, "db_path", err);
    if (st != KBC_OK) {
      return st;
    }
    ov->db_path = NULL;
  } else if (data_dir_given) {
    st = cfg_put(&cfg->db_path, cfg_join(cfg->data_dir, "kb.db"), "db_path",
                 err);
    if (st != KBC_OK) {
      return st;
    }
  }
  if (ov->index_path != NULL) {
    st = cfg_take_path(&cfg->index_path, ov->index_path, dir, "index_path",
                       err);
    if (st != KBC_OK) {
      return st;
    }
    ov->index_path = NULL;
  } else if (data_dir_given) {
    st = cfg_put(&cfg->index_path, cfg_join(cfg->data_dir, "index"),
                 "index_path", err);
    if (st != KBC_OK) {
      return st;
    }
  }
  if (ov->token_path != NULL) {
    st = cfg_take_path(&cfg->token_path, ov->token_path, dir, "token_file",
                       err);
    if (st != KBC_OK) {
      return st;
    }
    ov->token_path = NULL;
  } else if (data_dir_given) {
    st = cfg_put(&cfg->token_path, cfg_join(cfg->data_dir, "token"),
                 "token_file", err);
    if (st != KBC_OK) {
      return st;
    }
  }

  if (ov->embedder_cmd != NULL) {
    st = cfg_put(&cfg->embedder_cmd, ov->embedder_cmd, "embedder command", err);
    if (st != KBC_OK) {
      return st;
    }
    ov->embedder_cmd = NULL;
  }

  if (ov->has_port) {
    cfg->port = (int)ov->port;
  }
  if (ov->has_log_level) {
    cfg->log_level = ov->log_level;
  }
  if (ov->has_json_logs) {
    cfg->json_logs = ov->json_logs;
  }
  if (ov->has_workers) {
    cfg->http_workers = (size_t)ov->workers;
  }
  if (ov->has_chunk_max) {
    cfg->chunk_max_bytes = (size_t)ov->chunk_max_bytes;
  }
  if (ov->has_bm25_k1) {
    cfg->bm25_k1 = ov->bm25_k1;
  }
  if (ov->has_bm25_b) {
    cfg->bm25_b = ov->bm25_b;
  }
  if (ov->has_rrf_k) {
    cfg->rrf_k = (int)ov->rrf_k;
  }
  if (ov->has_graph_boost) {
    cfg->graph_boost = ov->graph_boost;
  }
  if (ov->has_max_hits) {
    cfg->search_max_hits = (size_t)ov->max_hits;
  }
  if (ov->has_debounce_ms) {
    cfg->watcher_debounce_ms = (size_t)ov->debounce_ms;
  }

  if (ov->any_corpus) {
    size_t i;
    kbc_corpus_cfg *arr = NULL;
    if (ov->ncorpora > 0u) {
      arr = (kbc_corpus_cfg *)calloc(ov->ncorpora, sizeof *arr);
      if (arr == NULL) {
        return kbc_err_set(err, KBC_ERR_NOMEM,
                           "out of memory applying %zu corpora", ov->ncorpora);
      }
      for (i = 0; i < ov->ncorpora; i++) {
        char *p = ov->corpora[i].cfg.path;
        arr[i].name = ov->corpora[i].cfg.name;
        arr[i].ignore = ov->corpora[i].cfg.ignore;
        ov->corpora[i].cfg.name = NULL;
        ov->corpora[i].cfg.path = NULL;
        kbc_strlist_init(&ov->corpora[i].cfg.ignore);
        if (p == NULL) {
          arr[i].path = NULL;
          continue;
        }
        if (p[0] == '/') {
          arr[i].path = p;
          continue;
        }
        arr[i].path = cfg_join(dir, p);
        free(p);
        if (arr[i].path == NULL) {
          size_t j;
          for (j = 0; j < i; j++) {
            free(arr[j].name);
            free(arr[j].path);
            kbc_strlist_free(&arr[j].ignore);
          }
          free(arr);
          return kbc_err_set(err, KBC_ERR_NOMEM,
                             "out of memory resolving corpus path");
        }
      }
    }
    for (i = 0; i < cfg->ncorpora; i++) {
      free(cfg->corpora[i].name);
      free(cfg->corpora[i].path);
      kbc_strlist_free(&cfg->corpora[i].ignore);
    }
    free(cfg->corpora);
    cfg->corpora = arr;
    cfg->ncorpora = ov->ncorpora;
  }

  if (abs_path != NULL) {
    st = cfg_put(&cfg->config_path, cfg_strdup(abs_path), "config_path", err);
    if (st != KBC_OK) {
      return st;
    }
  }
  return KBC_OK;
}

/* ------------------------------------------------------------- defaults -- */

kbc_config *kbc_config_defaults(void) {
  kbc_config *c = (kbc_config *)calloc(1, sizeof *c);
  if (c == NULL) {
    return NULL;
  }
  c->bind_addr = cfg_strdup("127.0.0.1");
  c->data_dir = cfg_strdup("data");
  c->db_path = cfg_strdup("data/kb.db");
  c->index_path = cfg_strdup("data/index");
  c->token_path = cfg_strdup("data/token");
  c->port = 4317;
  c->bm25_k1 = 1.2;
  c->bm25_b = 0.75;
  c->rrf_k = 60;
  c->graph_boost = 0.0; /* the graph is OFF until an operator asks for it */
  c->chunk_max_bytes = 65536u;
  c->search_max_hits = 50u;
  c->watcher_debounce_ms = 250u;
  c->http_workers = 4u;
  c->json_logs = false;
  c->log_level = KBC_LOG_INFO;
  if (c->bind_addr == NULL || c->data_dir == NULL || c->db_path == NULL ||
      c->index_path == NULL || c->token_path == NULL) {
    kbc_config_free(c);
    return NULL;
  }
  return c;
}

void kbc_config_free(kbc_config *cfg) {
  size_t i;
  if (cfg == NULL) {
    return;
  }
  free(cfg->config_path);
  free(cfg->data_dir);
  free(cfg->db_path);
  free(cfg->index_path);
  free(cfg->bind_addr);
  free(cfg->token_path);
  free(cfg->token);
  free(cfg->embedder_cmd);
  for (i = 0; i < cfg->ncorpora; i++) {
    free(cfg->corpora[i].name);
    free(cfg->corpora[i].path);
    kbc_strlist_free(&cfg->corpora[i].ignore);
  }
  free(cfg->corpora);
  /* The struct is caller-owned memory too (KBC_OWN on kbc_config_defaults), so
   * it goes with everything it owns. Every free above tolerates a NULL field,
   * but this function is called EXACTLY ONCE per config: KBC_OWN means once, so
   * a second call on the same pointer is a use-after-free, not a no-op. The
   * memset is there so a post-free read of the caller's own pointer is more
   * likely to fault loudly than to read a stale pointer. */
  memset(cfg, 0, sizeof *cfg);
  free(cfg);
}

/* The one place the bearer token is resolved. Everything that reads
 * cfg->token — the bind guard, the httpd's auth gate, the CLI's own outbound
 * requests — must go through here, or the daemon and the CLI can disagree
 * about what the token is.
 *
 * cfg->token already carrying a value is the caller saying "here it is"
 * (there is deliberately no kb.toml key for a literal secret), so that wins
 * and this is a no-op: idempotent, and re-resolving picks up a changed token
 * FILE only after the caller has dropped the previous value. */
kbc_status kbc_config_load_token(kbc_config *cfg, kbc_err *err) {
  kbc_str body;
  char *tok;
  size_t start;
  size_t end;

  kbc_err_reset(err);
  if (cfg == NULL) {
    return kbc_err_set(err, KBC_ERR_INVALID,
                       "kbc_config_load_token needs a config");
  }
  if (cfg->token != NULL) {
    return KBC_OK;
  }
  if (cfg->token_path == NULL || cfg->token_path[0] == '\0') {
    /* No token file configured at all: the loopback-with-no-auth case. This is
     * NOT an error, and it is deliberately distinguishable from a configured
     * file that cannot be read, which would be an error. */
    return KBC_OK;
  }

  kbc_str_init(&body);
  if (kbc_failed(kbc_str_read_file(cfg->token_path, &body, err))) {
    /* Name the path, because a bare "cannot read" leaves the operator guessing
     * which file, and because the distinction that matters here is
     * "configured but unreadable", not "no token". The path is bounded and the
     * underlying reason is dropped rather than risking a truncated copy. */
    kbc_str_free(&body);
    return kbc_err_set(err, KBC_ERR_IO,
                       "token file %.180s cannot be read", cfg->token_path);
  }

  /* First line only, \r\n tolerated. An empty or all-newline file resolves to
   * the empty string, which every reader already treats as "no token" — the
   * bind guard refuses a public bind on it, so an accidentally blank file
   * cannot quietly open a bind. */
  start = 0;
  end = 0;
  while (end < body.len && body.ptr[end] != '\n' && body.ptr[end] != '\r') {
    end++;
  }
  tok = malloc(end - start + 1u);
  if (tok == NULL) {
    kbc_str_free(&body);
    return kbc_err_set(err, KBC_ERR_NOMEM,
                       "out of memory reading the token from %s",
                       cfg->token_path);
  }
  /* An empty token file leaves the kbc_str with a NULL ptr, and
   * memcpy(dst, NULL, 0) is undefined even though it is benign in
   * practice — UBSan is right to flag it. The early return above
   * already proved the file is readable, so a NULL body.ptr here can
   * only mean the file was empty. */
  if (end > start) {
    (void)memcpy(tok, body.ptr + start, end - start);
  }
  tok[end - start] = '\0';
  kbc_str_free(&body);

  /* KBC_OWN: free whatever was there before replacing it. cfg->token is NULL
   * here (the early return above proved it), but the free keeps this correct
   * if that ever changes. */
  free(cfg->token);
  cfg->token = tok;
  return KBC_OK;
}

/* ------------------------------------------------------------- loading -- */

kbc_status kbc_config_load_file(kbc_config *cfg, const char *path,
                                kbc_err *err) {
  FILE *f;
  kbc_str buf;
  cfg_overlay ov;
  char *abs = NULL;
  char *dir = NULL;
  size_t lineno = 0;
  size_t pos = 0;
  cfg_section sec = SEC_NONE;
  cfg_corpus_stage *cur = NULL;
  kbc_status st = KBC_OK;

  kbc_err_reset(err);
  if (cfg == NULL || path == NULL) {
    return kbc_err_set(err, KBC_ERR_INVALID,
                       "kbc_config_load_file needs a config and a path");
  }

  f = fopen(path, "rb");
  if (f == NULL) {
    /* A missing config is fine: the defaults stand. Anything else — a
     * directory, a permission problem — is the operator's problem to see. */
    if (errno == ENOENT || errno == ENOTDIR) {
      return KBC_OK;
    }
    return kbc_err_set(err, KBC_ERR_IO, "open %s: %s", path, strerror(errno));
  }

  kbc_str_init(&buf);
  memset(&ov, 0, sizeof ov);
  abs = cfg_abspath(path);
  if (abs == NULL) {
    st = kbc_err_set(err, KBC_ERR_NOMEM, "out of memory loading %s", path);
    goto done;
  }
  dir = cfg_dirname_of(abs);
  if (dir == NULL) {
    st = kbc_err_set(err, KBC_ERR_NOMEM, "out of memory loading %s", path);
    goto done;
  }

  for (;;) {
    char chunk[4096];
    size_t got = fread(chunk, 1, sizeof chunk, f);
    if (got > 0u) {
      if (buf.len + got > KBC_CFG_MAX_FILE_BYTES) {
        st = cfg_err_at(path, lineno + 1u, err, KBC_ERR_INVALID,
                        "config file is larger than %u bytes",
                        KBC_CFG_MAX_FILE_BYTES);
        goto done;
      }
      if (kbc_str_append(&buf, chunk, got) != KBC_OK) {
        st = kbc_err_set(err, KBC_ERR_NOMEM, "out of memory reading %s", path);
        goto done;
      }
    }
    if (got < sizeof chunk) {
      if (ferror(f) != 0) {
        st = kbc_err_set(err, KBC_ERR_IO, "read %s: %s", path, strerror(errno));
        goto done;
      }
      break;
    }
  }
  (void)fclose(f);
  f = NULL;

  while (pos < buf.len) {
    size_t e = pos;
    size_t n;
    while (e < buf.len && buf.ptr[e] != '\n') {
      e++;
    }
    n = e - pos;
    lineno++;
    st = cfg_parse_line(&ov, &sec, &cur, buf.ptr + pos, n, lineno, path, err);
    if (st != KBC_OK) {
      goto done;
    }
    pos = e + 1u;
  }

  st = cfg_apply(cfg, &ov, dir, abs, err);

done:
  if (f != NULL) {
    (void)fclose(f);
  }
  kbc_str_free(&buf);
  cfg_overlay_free(&ov);
  free(abs);
  free(dir);
  return st;
}

/* ----------------------------------------------------------- validation -- */

const kbc_corpus_cfg *kbc_config_corpus(const kbc_config *cfg,
                                        const char *name) {
  size_t i;
  if (cfg == NULL || name == NULL) {
    return NULL;
  }
  for (i = 0; i < cfg->ncorpora; i++) {
    if (cfg->corpora[i].name != NULL &&
        strcmp(cfg->corpora[i].name, name) == 0) {
      return &cfg->corpora[i];
    }
  }
  return NULL;
}

/* Splits the bind address into its family and returns whether it is a
 * loopback literal. A non-literal is reported separately by the caller. */
typedef enum { BIND_BAD = 0, BIND_V4, BIND_V6 } bind_kind;

static bind_kind cfg_bind_kind(const char *addr, bool *loopback) {
  struct in_addr a4;
  struct in6_addr a6;
  *loopback = false;
  if (addr == NULL) {
    return BIND_BAD;
  }
  if (inet_pton(AF_INET, addr, &a4) == 1) {
    *loopback = (ntohl(a4.s_addr) >> 24) == 127u;
    return BIND_V4;
  }
  if (inet_pton(AF_INET6, addr, &a6) == 1) {
    *loopback = IN6_IS_ADDR_LOOPBACK(&a6) != 0;
    return BIND_V6;
  }
  return BIND_BAD;
}

bool kbc_config_bind_is_safe(const kbc_config *cfg, char *why, size_t why_cap) {
  bool loopback = false;
  bind_kind k;

  if (why != NULL && why_cap > 0u) {
    why[0] = '\0';
  }
  if (cfg == NULL) {
    if (why != NULL && why_cap > 0u) {
      (void)snprintf(why, why_cap, "refusing to bind with no configuration");
    }
    return false;
  }
  k = cfg_bind_kind(cfg->bind_addr, &loopback);
  if (k == BIND_BAD) {
    if (why != NULL && why_cap > 0u) {
      (void)snprintf(why, why_cap,
                     "refusing to bind %s:%d — bind must be an IPv4 or IPv6 "
                     "literal such as 127.0.0.1 or ::1",
                     cfg->bind_addr != NULL ? cfg->bind_addr : "(unset)",
                     cfg->port);
    }
    return false;
  }
  if (loopback) {
    return true;
  }
  if (cfg->token != NULL && cfg->token[0] != '\0') {
    return true;
  }
  if (why != NULL && why_cap > 0u) {
    /* Stable text: the CLI prints it and a test asserts it. It must never
     * contain the token value. */
    (void)snprintf(why, why_cap,
                   "refusing to bind %s:%d without a token — generate one with "
                   "`kbc token generate` or set bind = 127.0.0.1",
                   cfg->bind_addr, cfg->port);
  }
  return false;
}

kbc_status kbc_config_validate(const kbc_config *cfg, kbc_err *err) {
  size_t i;
  size_t j;
  char why[KBC_ERR_MSG_MAX];

  kbc_err_reset(err);
  if (cfg == NULL) {
    return kbc_err_set(err, KBC_ERR_INVALID,
                       "kbc_config_validate needs a config");
  }
  if (cfg->ncorpora > KBC_MAX_CORPORA) {
    return kbc_err_set(err, KBC_ERR_INVALID,
                       "configuration declares %zu corpora, the maximum is %u",
                       cfg->ncorpora, KBC_MAX_CORPORA);
  }
  for (i = 0; i < cfg->ncorpora; i++) {
    const kbc_corpus_cfg *c = &cfg->corpora[i];
    if (c->name == NULL || c->name[0] == '\0') {
      return kbc_err_set(err, KBC_ERR_INVALID,
                         "corpus %zu has an empty name", i);
    }
    for (j = 0; j < i; j++) {
      if (cfg->corpora[j].name != NULL &&
          strcmp(cfg->corpora[j].name, c->name) == 0) {
        return kbc_err_set(err, KBC_ERR_INVALID,
                           "duplicate corpus name \"%s\" (corpora %zu and %zu)",
                           c->name, j, i);
      }
    }
    if (c->path == NULL || c->path[0] == '\0') {
      return kbc_err_set(err, KBC_ERR_INVALID,
                         "corpus \"%s\" has an empty path", c->name);
    }
    if (strlen(c->path) >= KBC_MAX_PATH_LEN) {
      return kbc_err_set(err, KBC_ERR_INVALID,
                         "corpus \"%s\" path is longer than %u bytes",
                         c->name, KBC_MAX_PATH_LEN);
    }
    if (cfg_has_dotdot(c->path)) {
      return kbc_err_set(err, KBC_ERR_INVALID,
                         "corpus \"%s\" path \"%s\" contains \"..\"", c->name,
                         c->path);
    }
    if (!cfg_is_dir(c->path)) {
      return kbc_err_set(err, KBC_ERR_INVALID,
                         "corpus \"%s\" path \"%s\" is not a directory",
                         c->name, c->path);
    }
  }
  if (cfg->bind_addr == NULL) {
    return kbc_err_set(err, KBC_ERR_INVALID, "bind is not set");
  }
  {
    bool loopback = false;
    if (cfg_bind_kind(cfg->bind_addr, &loopback) == BIND_BAD) {
      return kbc_err_set(err, KBC_ERR_INVALID,
                         "bind \"%s\" is not an IPv4 or IPv6 literal",
                         cfg->bind_addr);
    }
  }
  if (cfg->port < 1 || cfg->port > 65535) {
    return kbc_err_set(err, KBC_ERR_INVALID, "port %d is outside 1..65535",
                       cfg->port);
  }
  if (!(cfg->bm25_k1 > 0.0)) {
    return kbc_err_set(err, KBC_ERR_INVALID, "bm25_k1 %g must be > 0",
                       cfg->bm25_k1);
  }
  if (!(cfg->bm25_b >= 0.0 && cfg->bm25_b <= 1.0)) {
    return kbc_err_set(err, KBC_ERR_INVALID, "bm25_b %g must be within 0..1",
                       cfg->bm25_b);
  }
  /* The parser refuses these too; the check is here so a caller that built a
   * config by hand cannot smuggle a weight past every caller downstream. */
  if (!(cfg->graph_boost >= 0.0 && cfg->graph_boost <= 4.0)) {
    return kbc_err_set(err, KBC_ERR_INVALID,
                       "graph_boost %g must be 0.0 (the graph off) or within "
                       "(0, 4]",
                       cfg->graph_boost);
  }
  if (cfg->watcher_debounce_ms > (size_t)KBC_CFG_MAX_DEBOUNCE_MS) {
    return kbc_err_set(err, KBC_ERR_INVALID,
                       "watcher debounce_ms %zu is outside 0..%u",
                       cfg->watcher_debounce_ms, KBC_CFG_MAX_DEBOUNCE_MS);
  }
  if (!kbc_config_bind_is_safe(cfg, why, sizeof why)) {
    return kbc_err_set(err, KBC_ERR_INVALID, "%s", why);
  }
  return KBC_OK;
}

/* ---------------------------------------------------------------- dump -- */

static kbc_status cfg_put_toml_string(kbc_str *out, const char *s,
                                      const char *what, kbc_err *err) {
  kbc_status st;
  const unsigned char *p;

  if (s == NULL) {
    return kbc_err_set(err, KBC_ERR_INVALID, "cannot dump %s: it is unset",
                       what);
  }
  st = kbc_str_putc(out, '"');
  if (st != KBC_OK) {
    return kbc_err_set(err, st, "out of memory dumping %s", what);
  }
  for (p = (const unsigned char *)s; *p != '\0'; p++) {
    char buf[8];
    size_t n = 0;
    switch (*p) {
      case '"': buf[n++] = '\\'; buf[n++] = '"'; break;
      case '\\': buf[n++] = '\\'; buf[n++] = '\\'; break;
      case '\n': buf[n++] = '\\'; buf[n++] = 'n'; break;
      case '\r': buf[n++] = '\\'; buf[n++] = 'r'; break;
      case '\t': buf[n++] = '\\'; buf[n++] = 't'; break;
      default:
        if (*p < 0x20u) {
          n = (size_t)snprintf(buf, sizeof buf, "\\u%04x", (unsigned)*p);
        } else {
          buf[n++] = (char)*p;
        }
        break;
    }
    st = kbc_str_append(out, buf, n);
    if (st != KBC_OK) {
      return kbc_err_set(err, st, "out of memory dumping %s", what);
    }
  }
  return kbc_str_putc(out, '"') == KBC_OK
             ? KBC_OK
             : kbc_err_set(err, KBC_ERR_NOMEM, "out of memory dumping %s",
                           what);
}

static kbc_status cfg_line(kbc_str *out, const char *text, kbc_err *err) {
  kbc_status st = kbc_str_puts(out, text);
  if (st != KBC_OK) {
    return kbc_err_set(err, st, "out of memory dumping config");
  }
  return kbc_str_putc(out, '\n') == KBC_OK
             ? KBC_OK
             : kbc_err_set(err, KBC_ERR_NOMEM, "out of memory dumping config");
}

/* "key = " followed by a quoted string, on one line. */
static kbc_status cfg_kv_str(kbc_str *out, const char *key, const char *value,
                             const char *what, kbc_err *err) {
  kbc_status st = kbc_str_puts(out, key);
  if (st != KBC_OK) {
    return kbc_err_set(err, st, "out of memory dumping %s", what);
  }
  st = cfg_put_toml_string(out, value, what, err);
  if (st != KBC_OK) {
    return st;
  }
  return cfg_line(out, "", err);
}
static kbc_status cfg_kv_fmt(kbc_str *out, const char *key, const char *fmt,
                             kbc_err *err, ...)
    __attribute__((format(printf, 3, 5)));


static kbc_status cfg_kv_fmt(kbc_str *out, const char *key, const char *fmt,
                             kbc_err *err, ...) {
  char body[128];
  va_list ap;
  kbc_status st = kbc_str_puts(out, key);
  if (st != KBC_OK) {
    return kbc_err_set(err, st, "out of memory dumping config");
  }
  va_start(ap, err);
  (void)vsnprintf(body, sizeof body, fmt, ap);
  va_end(ap);
  st = cfg_line(out, body, err);
  if (st != KBC_OK) {
    return st;
  }
  return cfg_line(out, "", err);
}

kbc_status kbc_config_dump(const kbc_config *cfg, kbc_str *out, kbc_err *err) {
  kbc_status st;
  size_t i;

  kbc_err_reset(err);
  if (cfg == NULL || out == NULL) {
    return kbc_err_set(err, KBC_ERR_INVALID,
                       "kbc_config_dump needs a config and a buffer");
  }
  if (cfg->bind_addr == NULL || cfg->data_dir == NULL || cfg->db_path == NULL ||
      cfg->index_path == NULL || cfg->token_path == NULL) {
    return kbc_err_set(err, KBC_ERR_INVALID,
                       "cannot dump a config with no bind/data_dir/db_path/"
                       "index_path/token_file");
  }

  st = cfg_line(out, "# generated by kbc — comments are not preserved", err);
  if (st != KBC_OK) {
    return st;
  }
  st = cfg_line(out, "", err);
  if (st != KBC_OK) {
    return st;
  }

  st = cfg_line(out, "[daemon]", err);
  if (st != KBC_OK) {
    return st;
  }
  st = cfg_kv_str(out, "bind = ", cfg->bind_addr, "bind", err);
  if (st != KBC_OK) {
    return st;
  }
  st = cfg_kv_fmt(out, "port = ", "%d", err, cfg->port);
  if (st != KBC_OK) {
    return st;
  }
  st = cfg_kv_str(out, "data_dir = ", cfg->data_dir, "data_dir", err);
  if (st != KBC_OK) {
    return st;
  }
  st = cfg_kv_str(out, "db_path = ", cfg->db_path, "db_path", err);
  if (st != KBC_OK) {
    return st;
  }
  st = cfg_kv_str(out, "index_path = ", cfg->index_path, "index_path", err);
  if (st != KBC_OK) {
    return st;
  }
  st = cfg_kv_str(out, "token_file = ", cfg->token_path, "token_file", err);
  if (st != KBC_OK) {
    return st;
  }
  st = cfg_kv_fmt(out, "workers = ", "%zu", err, cfg->http_workers);
  if (st != KBC_OK) {
    return st;
  }
  st = cfg_kv_str(out, "log_level = ", kbc_log_level_str(cfg->log_level),
                  "log_level", err);
  if (st != KBC_OK) {
    return st;
  }
  st = cfg_kv_fmt(out, "json_logs = ", "%s", err,
                  cfg->json_logs ? "true" : "false");
  if (st != KBC_OK) {
    return st;
  }
  st = cfg_kv_fmt(out, "chunk_max_bytes = ", "%zu", err, cfg->chunk_max_bytes);
  if (st != KBC_OK) {
    return st;
  }

  st = cfg_line(out, "[search]", err);
  if (st != KBC_OK) {
    return st;
  }
  /* %.17g round-trips an IEEE double exactly, so load(dump(x)) == x holds. */
  st = cfg_kv_fmt(out, "bm25_k1 = ", "%.17g", err, cfg->bm25_k1);
  if (st != KBC_OK) {
    return st;
  }
  st = cfg_kv_fmt(out, "bm25_b = ", "%.17g", err, cfg->bm25_b);
  if (st != KBC_OK) {
    return st;
  }
  st = cfg_kv_fmt(out, "rrf_k = ", "%d", err, cfg->rrf_k);
  if (st != KBC_OK) {
    return st;
  }
  /* Dumped only when the graph is on: 0 means off, and a dump that cannot be
   * loaded back is not a dump. */
  if (cfg->graph_boost > 0.0) {
    st = cfg_kv_fmt(out, "graph_boost = ", "%.17g", err, cfg->graph_boost);
    if (st != KBC_OK) {
      return st;
    }
  }
  st = cfg_kv_fmt(out, "max_hits = ", "%zu", err, cfg->search_max_hits);
  if (st != KBC_OK) {
    return st;
  }

  st = cfg_line(out, "[watcher]", err);
  if (st != KBC_OK) {
    return st;
  }
  st = cfg_kv_fmt(out, "debounce_ms = ", "%zu", err, cfg->watcher_debounce_ms);
  if (st != KBC_OK) {
    return st;
  }

  if (cfg->embedder_cmd != NULL) {
    st = cfg_line(out, "[embedder]", err);
    if (st != KBC_OK) {
      return st;
    }
    st = cfg_kv_str(out, "command = ", cfg->embedder_cmd, "embedder command",
                    err);
    if (st != KBC_OK) {
      return st;
    }
  }

  for (i = 0; i < cfg->ncorpora; i++) {
    const kbc_corpus_cfg *c = &cfg->corpora[i];
    size_t k;
    if (c->name == NULL || c->path == NULL) {
      return kbc_err_set(err, KBC_ERR_INVALID,
                         "corpus %zu has no name or path to dump", i);
    }
    st = cfg_line(out, "[[corpus]]", err);
    if (st != KBC_OK) {
      return st;
    }
    st = cfg_kv_str(out, "name = ", c->name, "corpus name", err);
    if (st != KBC_OK) {
      return st;
    }
    st = cfg_kv_str(out, "path = ", c->path, "corpus path", err);
    if (st != KBC_OK) {
      return st;
    }
    st = kbc_str_puts(out, "ignore = [") == KBC_OK
             ? KBC_OK
             : kbc_err_set(err, KBC_ERR_NOMEM, "out of memory dumping ignore");
    if (st != KBC_OK) {
      return st;
    }
    for (k = 0; k < c->ignore.len; k++) {
      if (kbc_str_puts(out, k == 0u ? " " : ", ") != KBC_OK) {
        return kbc_err_set(err, KBC_ERR_NOMEM, "out of memory dumping ignore");
      }
      st = cfg_put_toml_string(out, c->ignore.items[k], "ignore entry", err);
      if (st != KBC_OK) {
        return st;
      }
    }
    st = cfg_line(out, c->ignore.len == 0u ? "]" : " ]", err);
    if (st != KBC_OK) {
      return st;
    }
  }
  return KBC_OK;
}
