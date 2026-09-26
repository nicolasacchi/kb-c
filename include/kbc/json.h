/* json.h — a small, arena-owned JSON tree.
 *
 * Every value lives in the arena that created it, so there is no free() to get
 * wrong: when the request ends, the whole tree goes with it. Parsed strings are
 * unescaped in place into arena memory; escapes never alias the input buffer.
 *
 * The parser is strict RFC 8259 (no comments, no trailing commas, no NaN/Inf,
 * no single quotes) with one documented relaxation: a duplicate object key
 * keeps the LAST occurrence, matching what JS JSON.parse does.
 */
#ifndef KBC_JSON_H
#define KBC_JSON_H

#include "kbc/kbc.h"
#include "kbc/mem.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
  KBC_JSON_NULL = 0,
  KBC_JSON_BOOL,
  KBC_JSON_NUM,
  KBC_JSON_STR,
  KBC_JSON_ARR,
  KBC_JSON_OBJ
} kbc_json_type;

typedef struct kbc_json kbc_json;

struct kbc_json {
  kbc_json_type type;
  union {
    bool boolean;
    double num;
    struct {
      char *ptr; /* KBC_ARENA, NUL-terminated */
      size_t len;
    } str;
    struct {
      kbc_json **items; /* KBC_ARENA array of KBC_ARENA */
      size_t len, cap;
    } arr;
    struct {
      char **keys; /* KBC_ARENA */
      size_t *key_lens;
      kbc_json **vals; /* KBC_ARENA */
      size_t len, cap;
    } obj;
  } u;
};

/* Parsing. Returns NULL and fills `err` (KBC_ERR_PARSE, message carries the
 * byte offset) on malformed input. Trailing whitespace is allowed; trailing
 * garbage is not. Depth is capped at 64 to stop stack exhaustion. */
kbc_json *kbc_json_parse(kbc_arena *a, const char *text, size_t len,
                         kbc_err *err);

/* Construction. All take the arena that will own the result. */
kbc_json *kbc_json_new_null(kbc_arena *a);
kbc_json *kbc_json_new_bool(kbc_arena *a, bool v);
kbc_json *kbc_json_new_num(kbc_arena *a, double v);
kbc_json *kbc_json_new_str(kbc_arena *a, const char *s);
kbc_json *kbc_json_new_strn(kbc_arena *a, const char *s, size_t n);
kbc_json *kbc_json_new_arr(kbc_arena *a);
kbc_json *kbc_json_new_obj(kbc_arena *a);

/* Object/array building. `set` on a duplicate key overwrites in place (the
 * arena copy of the first key is kept; the value is replaced). */
kbc_status kbc_json_obj_set(kbc_arena *a, kbc_json *obj, const char *key,
                            kbc_json *val);
kbc_status kbc_json_arr_push(kbc_arena *a, kbc_json *arr, kbc_json *val);

/* Access. All BORROWED. kbc_json_get returns NULL when absent OR when the value
 * is not an object — use kbc_json_type first when that matters. */
kbc_json *kbc_json_get(const kbc_json *obj, const char *key);
const kbc_json *kbc_json_at(const kbc_json *arr, size_t i);
size_t kbc_json_len(const kbc_json *v); /* array/object count, 0 for scalars */
kbc_json_type kbc_json_type_of(const kbc_json *v);
bool kbc_json_is(const kbc_json *v, kbc_json_type t);

/* Typed getters with defaults. Return `dflt` on type mismatch or absence. */
const char *kbc_json_str(const kbc_json *obj, const char *key,
                         const char *dflt);
double kbc_json_num(const kbc_json *obj, const char *key, double dflt);
int64_t kbc_json_i64(const kbc_json *obj, const char *key, int64_t dflt);
bool kbc_json_bool(const kbc_json *obj, const char *key, bool dflt);

/* Serialization. `out` is appended to. pretty=true uses two-space indent.
 * Output is compact-by-default: no space after ':' or ','. */
kbc_status kbc_json_dump(const kbc_json *v, kbc_str *out, bool pretty,
                         kbc_err *err);

/* Escape a bare string (no quotes) into a JSON string body. */
kbc_status kbc_json_escape(kbc_str *out, const char *s, size_t n);

#ifdef __cplusplus
}
#endif

#endif /* KBC_JSON_H */
