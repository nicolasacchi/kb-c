/* mem.h — arenas, growable byte strings, string lists, file helpers.
 *
 * Arena = request-scoped scratch. Allocating is a pointer bump; freeing is one
 * call. Use it for anything that dies at the end of a request (parsed JSON,
 * search results, artifact rows). Use heap only for things that outlive the
 * request: the store, the index, the config, the httpd.
 */
#ifndef KBC_MEM_H
#define KBC_MEM_H

#include "kbc/kbc.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---------------------------------------------------------------- arena -- */

typedef struct kbc_arena kbc_arena;

kbc_arena *kbc_arena_new(size_t first_block_bytes);
void kbc_arena_free(kbc_arena *a);

/* All returned memory is 8-byte aligned, zeroed for calloc, and dies with `a`.
 * kbc_arena_alloc(a, 0) returns a unique non-NULL pointer. */
void *kbc_arena_alloc(kbc_arena *a, size_t bytes);
void *kbc_arena_calloc(kbc_arena *a, size_t count, size_t size);
char *kbc_arena_strndup(kbc_arena *a, const char *s, size_t n);
char *kbc_arena_strdup(kbc_arena *a, const char *s);
char *kbc_arena_printf(kbc_arena *a, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));

/* Keeps the first block, releases the rest. All prior pointers die. */
void kbc_arena_reset(kbc_arena *a);
size_t kbc_arena_bytes(const kbc_arena *a);

/* ----------------------------------------------------------- kbc_str ----- */

/* Owned, growable, always NUL-terminated at .ptr[.len]. Bytes may contain
 * embedded NULs; always pass .len explicitly. */
typedef struct {
  char *ptr;
  size_t len;
  size_t cap;
} kbc_str;

void kbc_str_init(kbc_str *s);
void kbc_str_free(kbc_str *s);
bool kbc_str_reserve(kbc_str *s, size_t extra);
void kbc_str_clear(kbc_str *s);
kbc_status kbc_str_append(kbc_str *s, const char *data, size_t n);
kbc_status kbc_str_puts(kbc_str *s, const char *cstr);
kbc_status kbc_str_putc(kbc_str *s, char c);
kbc_status kbc_str_printf(kbc_str *s, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));
/* JSON string literal, quotes and all, with \u escaping for control chars. */
kbc_status kbc_str_append_json_string(kbc_str *s, const char *data, size_t n);

/* --------------------------------------------------------- kbc_strlist -- */

typedef struct {
  char **items; /* KBC_OWN, each NUL-terminated */
  size_t len;
  size_t cap;
} kbc_strlist;

void kbc_strlist_init(kbc_strlist *l);
void kbc_strlist_free(kbc_strlist *l);
kbc_status kbc_strlist_push(kbc_strlist *l, const char *s);      /* copies */
kbc_status kbc_strlist_push_owned(kbc_strlist *l, char *s);       /* takes ownership */
bool kbc_strlist_contains(const kbc_strlist *l, const char *s);
void kbc_strlist_sort(kbc_strlist *l);

/* ------------------------------------------------------------- files ---- */

/* Reads at most KBC_MAX_ARTIFACT_BYTES. `out` is appended to, not cleared. */
kbc_status kbc_str_read_file(const char *path, kbc_str *out, kbc_err *err);

/* Writes to "<path>.tmp.<pid>", fsyncs, renames over `path`, fsyncs the
 * directory. A crash therefore leaves either the old file or the new one. */
kbc_status kbc_str_write_file_atomic(const char *path, const char *data,
                                     size_t n, kbc_err *err);
bool kbc_path_exists(const char *path);
kbc_status kbc_mkdir_p(const char *path, kbc_err *err);

/* ------------------------------------------------------------- bytes ---- */

uint32_t kbc_fnv1a32(const void *data, size_t n);
/* Base64 of `n` bytes into `out` (appends, no line breaks). */
kbc_status kbc_base64_encode(kbc_str *out, const void *data, size_t n);
/* Strict: rejects invalid alphabet, bad padding and embedded whitespace. */
kbc_status kbc_base64_decode(kbc_arena *a, const char *in, size_t n,
                             const char **out, size_t *out_len, kbc_err *err);
kbc_status kbc_hex_encode(kbc_str *out, const void *data, size_t n);
bool kbc_hex_decode(const char *in, size_t n, uint8_t *out, size_t out_cap,
                    size_t *out_len);

/* Constant-time compare, for token checks. */
bool kbc_const_time_eq(const void *a, const void *b, size_t n);

int64_t kbc_now_ns(void);
int64_t kbc_now_iso8601(char *buf, size_t cap); /* returns length written */

/* UTF-8: returns the byte length of the sequence at s, or 0 if it is not a
 * valid sequence. Never reads past s[min(remaining, 4)). */
size_t kbc_utf8_len(unsigned char c);
bool kbc_utf8_validate(const char *s, size_t n, size_t *bad_offset);
size_t kbc_utf8_count(const char *s, size_t n);

/* Case-fold + strip diacritics for one UTF-8 sequence, into a 32-byte buffer.
 * ASCII is exact; Latin-1/Latin Extended-A decompose; everything else is
 * copied unchanged. */
size_t kbc_fold_utf8(const char *s, size_t n, char out[32]);

#ifdef __cplusplus
}
#endif

#endif /* KBC_MEM_H */
