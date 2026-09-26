/* kbc_test.c — runtime for the header-only harness in kbc_test.h. */
#include "kbc_test.h"

int kbc_test_failures = 0;
const char *kbc_test_current = "(none)";

void kbc_test_fail(const char *file, int line, const char *fmt, ...) {
  va_list ap;
  fprintf(stderr, "  FAIL %s:%d [%s] ", file, line, kbc_test_current);
  va_start(ap, fmt);
  vfprintf(stderr, fmt, ap);
  va_end(ap);
  fputc('\n', stderr);
  kbc_test_failures++;
}

int kbc_test_run(const char *suite, const kbc_test_case *cases) {
  int before = kbc_test_failures;
  printf("== %s ==\n", suite);
  for (size_t i = 0; cases[i].name != NULL; i++) {
    kbc_test_current = cases[i].name;
    int mark = kbc_test_failures;
    cases[i].fn();
    printf("  %-44s %s\n", cases[i].name,
           kbc_test_failures == mark ? "ok" : "FAILED");
  }
  kbc_test_current = "(none)";
  int failed = kbc_test_failures - before;
  printf("%s: %d check failure(s)\n", suite, failed);
  return failed == 0 ? 0 : 1;
}

void kbc_test_mkdir_p(const char *path) {
  char buf[KBC_TEST_PATH_MAX];
  if (snprintf(buf, sizeof buf, "%s", path) >= (int)sizeof buf) return;
  for (char *p = buf + 1; *p != '\0'; p++) {
    if (*p != '/') continue;
    *p = '\0';
    (void)mkdir(buf, 0700);
    *p = '/';
  }
  (void)mkdir(buf, 0700);
}

void kbc_test_rmrf(const char *path) {
  DIR *d = opendir(path);
  if (d == NULL) return;
  struct dirent *e;
  while ((e = readdir(d)) != NULL) {
    if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0) continue;
    char child[KBC_TEST_PATH_MAX];
    if (snprintf(child, sizeof child, "%s/%s", path, e->d_name) >=
        (int)sizeof child)
      continue;
    struct stat st;
    if (lstat(child, &st) == 0 && S_ISDIR(st.st_mode)) {
      kbc_test_rmrf(child);
    } else {
      (void)unlink(child);
    }
  }
  closedir(d);
  (void)rmdir(path);
}

void kbc_test_tmpdir(char *buf, size_t cap) {
  static unsigned seq;
  if (snprintf(buf, cap, "/tmp/kbc-test-%ld-%u", (long)getpid(), seq++) >=
      (int)cap)
    return;
  kbc_test_rmrf(buf); /* clear leftovers from an interrupted earlier run */
  kbc_test_mkdir_p(buf);
}

void kbc_test_write_file(const char *path, const char *content) {
  char dir[KBC_TEST_PATH_MAX];
  if (snprintf(dir, sizeof dir, "%s", path) < (int)sizeof dir) {
    char *slash = strrchr(dir, '/');
    if (slash != NULL) {
      *slash = '\0';
      kbc_test_mkdir_p(dir);
    }
  }
  FILE *f = fopen(path, "wb");
  if (f == NULL) {
    fprintf(stderr, "  FAIL cannot write %s: %s\n", path, strerror(errno));
    kbc_test_failures++;
    return;
  }
  (void)fwrite(content, 1, strlen(content), f);
  (void)fclose(f);
}

char *kbc_test_read_file(const char *path) {
  FILE *f = fopen(path, "rb");
  if (f == NULL) return NULL;
  size_t cap = 4096, len = 0;
  char *buf = malloc(cap);
  if (buf == NULL) {
    (void)fclose(f);
    return NULL;
  }
  for (;;) {
    if (len + 1024 >= cap) {
      cap *= 2;
      char *nb = realloc(buf, cap);
      if (nb == NULL) {
        free(buf);
        (void)fclose(f);
        return NULL;
      }
      buf = nb;
    }
    size_t got = fread(buf + len, 1, cap - len - 1, f);
    len += got;
    if (got == 0) break;
  }
  buf[len] = '\0';
  (void)fclose(f);
  return buf;
}
