/* kbc_test.h — the whole test framework. Header-only, no dependency.
 *
 * A test file declares cases with KBC_TEST(name), asserts with the KBC_CHECK*
 * macros, and ends with:
 *
 *     int main(void) {
 *       kbc_test_tmpdir(g_tmp, sizeof g_tmp);
 *       int rc = kbc_test_run("parse", (kbc_test_case[]){
 *           {"blocks", test_blocks}, {"anchors", test_anchors}, {NULL, NULL}});
 *       kbc_test_rmrf(g_tmp);
 *       return rc;
 *     }
 *
 * A failing check prints file:line and keeps going, so one run reports every
 * broken expectation in the file rather than only the first. The exit code is
 * non-zero iff at least one case failed.
 */
#ifndef KBC_TEST_H
#define KBC_TEST_H

#include <dirent.h>
#include <errno.h>
#include <math.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "kbc/kbc.h"

#define KBC_TEST_PATH_MAX 4096

typedef void (*kbc_test_fn)(void);

typedef struct {
  const char *name;
  kbc_test_fn fn;
} kbc_test_case;

extern int kbc_test_failures;
extern const char *kbc_test_current;

void kbc_test_fail(const char *file, int line, const char *fmt, ...);
int kbc_test_run(const char *suite, const kbc_test_case *cases);
/* Creates a fresh, empty, unique directory under /tmp. */
void kbc_test_tmpdir(char *buf, size_t cap);
void kbc_test_rmrf(const char *path);
void kbc_test_mkdir_p(const char *path);
void kbc_test_write_file(const char *path, const char *content);
char *kbc_test_read_file(const char *path);

#define KBC_TEST(name) static void name(void)

#define KBC_CHECK(cond)                                                       \
  do {                                                                        \
    if (!(cond))                                                              \
      kbc_test_fail(__FILE__, __LINE__, "expected: %s", #cond);                \
  } while (0)

#define KBC_CHECK_MSG(cond, ...)                                              \
  do {                                                                        \
    if (!(cond))                                                              \
      kbc_test_fail(__FILE__, __LINE__, __VA_ARGS__);                         \
  } while (0)

#define KBC_CHECK_EQ_INT(actual, expected)                                    \
  do {                                                                        \
    long long a_ = (long long)(actual), e_ = (long long)(expected);            \
    if (a_ != e_)                                                             \
      kbc_test_fail(__FILE__, __LINE__, "%s: got %lld, want %lld", #actual,    \
                    a_, e_);                                                  \
  } while (0)

#define KBC_CHECK_EQ_DBL(actual, expected, eps)                               \
  do {                                                                        \
    double a_ = (double)(actual), e_ = (double)(expected);                     \
    if (!(fabs(a_ - e_) <= (eps)))                                            \
      kbc_test_fail(__FILE__, __LINE__, "%s: got %.10g, want %.10g (+-%g)",     \
                    #actual, a_, e_, (double)(eps));                           \
  } while (0)

#define KBC_CHECK_EQ_STR(actual, expected)                                    \
  do {                                                                        \
    const char *a_ = (actual), *e_ = (expected);                               \
    if (a_ == NULL || e_ == NULL || strcmp(a_, e_) != 0)                      \
      kbc_test_fail(__FILE__, __LINE__, "%s: got \"%s\", want \"%s\"",        \
                    #actual, a_ ? a_ : "(null)", e_ ? e_ : "(null)");          \
  } while (0)

#define KBC_CHECK_NULL(p)                                                     \
  do {                                                                        \
    if ((p) != NULL)                                                          \
      kbc_test_fail(__FILE__, __LINE__, "%s: expected NULL, got %p", #p, (p)); \
  } while (0)

#define KBC_CHECK_NOT_NULL(p)                                                 \
  do {                                                                        \
    if ((p) == NULL)                                                          \
      kbc_test_fail(__FILE__, __LINE__, "%s: unexpectedly NULL", #p);          \
  } while (0)

#define KBC_CHECK_OK(status)                                                  \
  do {                                                                        \
    kbc_status s_ = (status);                                                 \
    if (s_ != KBC_OK)                                                         \
      kbc_test_fail(__FILE__, __LINE__, "%s: got %s", #status,                 \
                    kbc_status_str(s_));                                       \
  } while (0)

#define KBC_CHECK_ERR(status, want)                                           \
  do {                                                                        \
    kbc_status s_ = (status);                                                 \
    if (s_ != (want))                                                         \
      kbc_test_fail(__FILE__, __LINE__, "%s: got %s, want %s", #status,        \
                    kbc_status_str(s_), kbc_status_str(want));                 \
  } while (0)

/* For a KBC_ERR_* result, also assert the message is actually filled in —
 * an error with an empty message is a bug users cannot act on. */
#define KBC_CHECK_ERR_MSG(err)                                                \
  do {                                                                        \
    if ((err).msg[0] == '\0')                                                 \
      kbc_test_fail(__FILE__, __LINE__, "error message is empty");             \
  } while (0)

#endif /* KBC_TEST_H */
