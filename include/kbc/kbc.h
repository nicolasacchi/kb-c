/* kbc.h — umbrella header and the error/status contract for libkbc.
 *
 * Ownership rules that hold across the whole library (violating them is a bug):
 *
 *   KBC_OWN   the caller must kbc_*_free() it, exactly once.
 *   KBC_ARENA the memory dies with the kbc_arena it was made in; never free it
 *             individually, never store it past the arena's reset.
 *   BORROWED  valid only as long as the input it came from; never mutate, never
 *             free.
 *
 * Every fallible function takes a `kbc_err *` (may be NULL to discard) and
 * returns kbc_status. KBC_OK is 0; every failure is non-zero. A function that
 * returns a pointer returns NULL on failure and has already filled `err`.
 */
#ifndef KBC_KBC_H
#define KBC_KBC_H

#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define KBC_VERSION "0.1.0"
#define KBC_PROJECT "kb-c"

/* Index file magic + on-disk format version. Bump the version whenever the
 * layout in index.c changes; kbc_index_open refuses a mismatch instead of
 * reading garbage. */
#define KBC_INDEX_MAGIC "KBCIDX\x01"
#define KBC_INDEX_FORMAT 1u

/* Hard ceilings. Every unbounded-looking API takes a limit and enforces it;
 * these are the backstops that keep one pathological input from eating the
 * daemon's address space. */
#define KBC_MAX_ARTIFACT_BYTES (16u * 1024u * 1024u)
#define KBC_MAX_QUERY_LEN 4096u
#define KBC_MAX_PATH_LEN 4096u
#define KBC_MAX_ID_LEN 12u
#define KBC_MAX_CORPORA 256u
#define KBC_MAX_HITS 1000u
#define KBC_MAX_TOKENS_PER_DOC 200000u
#define KBC_MAX_TERM_LEN 128u
#define KBC_MAX_SNIFF_BYTES 65536u
#define KBC_HTTP_MAX_HEADER_BYTES (64u * 1024u)
#define KBC_HTTP_MAX_BODY_BYTES (8u * 1024u * 1024u)
#define KBC_HTTP_MAX_REQUEST_LINE 8192u
#define KBC_HTTP_MAX_CONNECTIONS 512u

typedef enum {
  KBC_OK = 0,
  KBC_ERR_INVALID,     /* caller passed a value that cannot be right */
  KBC_ERR_NOTFOUND,    /* looked up, not there */
  KBC_ERR_CONFLICT,    /* exists already / version clash */
  KBC_ERR_IO,          /* open/read/write/rename failed */
  KBC_ERR_SQL,         /* sqlite3 reported an error */
  KBC_ERR_PARSE,       /* malformed input the parser rejected */
  KBC_ERR_NOMEM,       /* allocation failed */
  KBC_ERR_UNSUPPORTED, /* understood, deliberately not implemented */
  KBC_ERR_TIMEOUT,     /* deadline passed */
  KBC_ERR_CANCELED,    /* caller went away mid-operation */
  KBC_ERR_INTERNAL,    /* invariant violated — a bug */
  KBC_STATUS__COUNT
} kbc_status;

#define KBC_ERR_MSG_MAX 256

/* A status plus a human message. Cheap to pass by pointer, safe to memset. */
typedef struct {
  kbc_status status;
  char msg[KBC_ERR_MSG_MAX];
} kbc_err;

const char *kbc_status_str(kbc_status s);
bool kbc_failed(kbc_status s);

/* Clears `e` to KBC_OK. Always returns s, so call sites read:
 *     return kbc_err_set(err, KBC_ERR_IO, "open %s: %s", path, strerror(saved)); */
kbc_status kbc_err_set(kbc_err *e, kbc_status s, const char *fmt, ...)
    __attribute__((format(printf, 3, 4)));
void kbc_err_reset(kbc_err *e);

#ifdef __cplusplus
}
#endif

#endif /* KBC_KBC_H */
