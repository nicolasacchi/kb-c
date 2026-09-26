/* mem.c — arenas, growable byte strings, string lists, file I/O, encoding.
 *
 * Everything here is allocation-disciplined: every size is either bounded by
 * a KBC_MAX_* ceiling or derived from a length that travelled with the data.
 * Nothing assumes NUL-termination on bytes that came from a file. */

#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#include "kbc/kbc.h"
#include "kbc/mem.h"

#define KBC_ARENA_DEFAULT_BLOCK 4096u
#define KBC_STR_MIN_CAP 16u

/* ============================== arena ================================== */

typedef struct kbc_arena_block {
  struct kbc_arena_block *next;
  size_t cap;  /* usable bytes in data */
  size_t used; /* bump cursor, always 8-aligned before use */
  unsigned char data[];
} kbc_arena_block;

struct kbc_arena {
  kbc_arena_block *first; /* survives kbc_arena_reset */
  kbc_arena_block *cur;
  size_t first_block_bytes;
};

static size_t align_up(size_t off) {
  return (off + 7u) & ~(size_t)7u;
}

static kbc_arena_block *block_new(size_t cap) {
  if (cap > SIZE_MAX - sizeof(kbc_arena_block)) {
    return NULL;
  }
  kbc_arena_block *b = malloc(sizeof(*b) + cap);
  if (b == NULL) {
    return NULL;
  }
  b->next = NULL;
  b->cap = cap;
  b->used = 0;
  return b;
}

kbc_arena *kbc_arena_new(size_t first_block_bytes) {
  if (first_block_bytes == 0) {
    first_block_bytes = KBC_ARENA_DEFAULT_BLOCK;
  }
  kbc_arena *a = calloc(1, sizeof(*a));
  if (a == NULL) {
    return NULL;
  }
  a->first_block_bytes = first_block_bytes;
  a->first = block_new(first_block_bytes);
  if (a->first == NULL) {
    free(a);
    return NULL;
  }
  a->cur = a->first;
  return a;
}

void kbc_arena_free(kbc_arena *a) {
  if (a == NULL) {
    return;
  }
  kbc_arena_block *b = a->first;
  while (b != NULL) {
    kbc_arena_block *next = b->next;
    free(b);
    b = next;
  }
  free(a);
}

void *kbc_arena_alloc(kbc_arena *a, size_t bytes) {
  if (a == NULL) {
    return NULL;
  }
  /* A zero-byte request still consumes a slot so the returned pointers stay
   * unique — callers use them as identity keys. */
  size_t need = bytes == 0 ? 1u : bytes;

  if (a->cur != NULL) {
    size_t off = align_up(a->cur->used);
    if (off <= a->cur->cap && need <= a->cur->cap - off) {
      a->cur->used = off + need;
      return a->cur->data + off;
    }
  }

  size_t cap = a->first_block_bytes;
  if (cap < need) {
    cap = need;
  }
  kbc_arena_block *b = block_new(cap);
  if (b == NULL) {
    return NULL;
  }
  /* Append, never prepend: first is the head every consumer (free, reset,
   * bytes) walks, so a block linked in front of it would be unreachable and
   * would leak on both reset and free. cur is by construction reachable from
   * first, so the two never describe different sets. */
  a->cur->next = b;
  a->cur = b;
  b->used = need;
  return b->data;
}

void *kbc_arena_calloc(kbc_arena *a, size_t count, size_t size) {
  if (count != 0 && size > SIZE_MAX / count) {
    return NULL;
  }
  size_t total = count * size;
  void *p = kbc_arena_alloc(a, total);
  if (p == NULL) {
    return NULL;
  }
  if (total > 0) {
    memset(p, 0, total);
  }
  return p;
}

char *kbc_arena_strndup(kbc_arena *a, const char *s, size_t n) {
  if (s == NULL) {
    return NULL;
  }
  if (n == SIZE_MAX) {
    return NULL;
  }
  char *p = kbc_arena_alloc(a, n + 1);
  if (p == NULL) {
    return NULL;
  }
  if (n > 0) {
    memcpy(p, s, n);
  }
  p[n] = '\0';
  return p;
}

char *kbc_arena_strdup(kbc_arena *a, const char *s) {
  if (s == NULL) {
    return NULL;
  }
  return kbc_arena_strndup(a, s, strlen(s));
}

char *kbc_arena_printf(kbc_arena *a, const char *fmt, ...) {
  if (a == NULL || fmt == NULL) {
    return NULL;
  }
  va_list ap;
  va_start(ap, fmt);
  va_list probe;
  va_copy(probe, ap);
  int n = vsnprintf(NULL, 0, fmt, probe);
  va_end(probe);
  va_end(ap);
  if (n < 0) {
    return NULL;
  }
  char *p = kbc_arena_alloc(a, (size_t)n + 1);
  if (p == NULL) {
    return NULL;
  }
  va_start(ap, fmt);
  int m = vsnprintf(p, (size_t)n + 1, fmt, ap);
  va_end(ap);
  if (m < 0) {
    return NULL;
  }
  return p;
}

void kbc_arena_reset(kbc_arena *a) {
  if (a == NULL) {
    return;
  }
  kbc_arena_block *b = a->first->next;
  while (b != NULL) {
    kbc_arena_block *next = b->next;
    free(b);
    b = next;
  }
  a->first->next = NULL;
  a->first->used = 0;
  a->cur = a->first;
}

size_t kbc_arena_bytes(const kbc_arena *a) {
  if (a == NULL) {
    return 0;
  }
  size_t total = 0;
  for (const kbc_arena_block *b = a->first; b != NULL; b = b->next) {
    if (total > SIZE_MAX - b->used) {
      return SIZE_MAX;
    }
    total += b->used;
  }
  return total;
}

/* ============================== kbc_str ================================ */

void kbc_str_init(kbc_str *s) {
  s->ptr = NULL;
  s->len = 0;
  s->cap = 0;
}

void kbc_str_free(kbc_str *s) {
  if (s == NULL) {
    return;
  }
  free(s->ptr);
  s->ptr = NULL;
  s->len = 0;
  s->cap = 0;
}

void kbc_str_clear(kbc_str *s) {
  if (s == NULL) {
    return;
  }
  s->len = 0;
  if (s->ptr != NULL) {
    s->ptr[0] = '\0';
  }
}

bool kbc_str_reserve(kbc_str *s, size_t extra) {
  if (s == NULL) {
    return false;
  }
  if (extra > SIZE_MAX - s->len - 1u) {
    return false;
  }
  size_t need = s->len + extra + 1u; /* room for extra bytes plus the NUL */
  if (need <= s->cap) {
    return true;
  }
  size_t cap = s->cap + s->cap / 2u + KBC_STR_MIN_CAP;
  if (cap < need) {
    cap = need;
  }
  char *p = realloc(s->ptr, cap);
  if (p == NULL) {
    return false;
  }
  s->ptr = p;
  s->cap = cap;
  if (s->len == 0) {
    s->ptr[0] = '\0';
  }
  return true;
}

kbc_status kbc_str_append(kbc_str *s, const char *data, size_t n) {
  if (s == NULL || (data == NULL && n > 0)) {
    return KBC_ERR_INVALID;
  }
  if (n == 0) {
    return KBC_OK;
  }
  if (!kbc_str_reserve(s, n)) {
    return KBC_ERR_NOMEM;
  }
  memcpy(s->ptr + s->len, data, n);
  s->len += n;
  s->ptr[s->len] = '\0';
  return KBC_OK;
}

kbc_status kbc_str_puts(kbc_str *s, const char *cstr) {
  if (cstr == NULL) {
    return KBC_ERR_INVALID;
  }
  return kbc_str_append(s, cstr, strlen(cstr));
}

kbc_status kbc_str_putc(kbc_str *s, char c) { return kbc_str_append(s, &c, 1); }

kbc_status kbc_str_printf(kbc_str *s, const char *fmt, ...) {
  if (s == NULL || fmt == NULL) {
    return KBC_ERR_INVALID;
  }
  va_list ap;
  va_start(ap, fmt);
  va_list probe;
  va_copy(probe, ap);
  int n = vsnprintf(NULL, 0, fmt, probe);
  va_end(probe);
  va_end(ap);
  if (n < 0) {
    return KBC_ERR_INTERNAL;
  }
  size_t need = (size_t)n;
  if (need > SIZE_MAX - s->len - 1u) {
    return KBC_ERR_NOMEM;
  }
  if (!kbc_str_reserve(s, need)) {
    return KBC_ERR_NOMEM;
  }
  va_start(ap, fmt);
  int m = vsnprintf(s->ptr + s->len, need + 1u, fmt, ap);
  va_end(ap);
  if (m < 0) {
    return KBC_ERR_INTERNAL;
  }
  s->len += (size_t)m;
  s->ptr[s->len] = '\0';
  return KBC_OK;
}

static kbc_status str_emit_hex4(kbc_str *s, uint32_t cp) {
  static const char digits[] = "0123456789abcdef";
  char buf[6] = {'\\', 'u', 0, 0, 0, 0};
  for (int i = 0; i < 4; i++) {
    buf[2 + (size_t)i] = digits[(cp >> (12 - 4 * i)) & 0xfu];
  }
  return kbc_str_append(s, buf, sizeof(buf));
}

kbc_status kbc_str_append_json_string(kbc_str *s, const char *data,
                                      size_t n) {
  if (s == NULL || (data == NULL && n > 0)) {
    return KBC_ERR_INVALID;
  }
  kbc_status st = kbc_str_putc(s, '"');
  if (st != KBC_OK) {
    return st;
  }

  const unsigned char *p = (const unsigned char *)data;
  size_t i = 0;
  while (i < n) {
    unsigned char c = p[i];
    if (c == '"' || c == '\\') {
      char esc[2] = {'\\', (char)c};
      st = kbc_str_append(s, esc, 2);
      if (st != KBC_OK) {
        return st;
      }
      i++;
      continue;
    }
    if (c < 0x20u || c == 0x7fu) {
      /* Control characters go out as \u00XX; DEL is the one printable-looking
       * byte that would break a naive terminal. */
      st = str_emit_hex4(s, c);
      if (st != KBC_OK) {
        return st;
      }
      i++;
      continue;
    }
    if (c < 0x80u) {
      st = kbc_str_putc(s, (char)c);
      if (st != KBC_OK) {
        return st;
      }
      i++;
      continue;
    }
    /* Multi-byte: pass valid UTF-8 through byte for byte, escape anything
     * malformed so the output is always parseable JSON. */
    size_t seq = kbc_utf8_len(c);
    if (seq == 0 || seq > n - i) {
      st = str_emit_hex4(s, c);
      if (st != KBC_OK) {
        return st;
      }
      i++;
      continue;
    }
    size_t bad = 0;
    if (!kbc_utf8_validate((const char *)(p + i), seq, &bad)) {
      st = str_emit_hex4(s, c);
      if (st != KBC_OK) {
        return st;
      }
      i++;
      continue;
    }
    st = kbc_str_append(s, (const char *)(p + i), seq);
    if (st != KBC_OK) {
      return st;
    }
    i += seq;
  }
  return kbc_str_putc(s, '"');
}

/* ============================= kbc_strlist ============================= */

void kbc_strlist_init(kbc_strlist *l) {
  l->items = NULL;
  l->len = 0;
  l->cap = 0;
}

void kbc_strlist_free(kbc_strlist *l) {
  if (l == NULL) {
    return;
  }
  for (size_t i = 0; i < l->len; i++) {
    free(l->items[i]);
  }
  free(l->items);
  l->items = NULL;
  l->len = 0;
  l->cap = 0;
}

static kbc_status strlist_grow(kbc_strlist *l) {
  if (l->len < l->cap) {
    return KBC_OK;
  }
  size_t cap = l->cap == 0 ? 8u : l->cap * 2u;
  if (cap > SIZE_MAX / sizeof(char *)) {
    return KBC_ERR_NOMEM;
  }
  char **items = realloc(l->items, cap * sizeof(*items));
  if (items == NULL) {
    return KBC_ERR_NOMEM;
  }
  l->items = items;
  l->cap = cap;
  return KBC_OK;
}

kbc_status kbc_strlist_push(kbc_strlist *l, const char *s) {
  if (l == NULL || s == NULL) {
    return KBC_ERR_INVALID;
  }
  kbc_status st = strlist_grow(l);
  if (st != KBC_OK) {
    return st;
  }
  size_t n = strlen(s);
  char *copy = malloc(n + 1u);
  if (copy == NULL) {
    return KBC_ERR_NOMEM;
  }
  memcpy(copy, s, n + 1u);
  l->items[l->len++] = copy;
  return KBC_OK;
}

kbc_status kbc_strlist_push_owned(kbc_strlist *l, char *s) {
  if (l == NULL || s == NULL) {
    return KBC_ERR_INVALID;
  }
  kbc_status st = strlist_grow(l);
  if (st != KBC_OK) {
    return st;
  }
  l->items[l->len++] = s;
  return KBC_OK;
}

bool kbc_strlist_contains(const kbc_strlist *l, const char *s) {
  if (l == NULL || s == NULL) {
    return false;
  }
  for (size_t i = 0; i < l->len; i++) {
    if (strcmp(l->items[i], s) == 0) {
      return true;
    }
  }
  return false;
}

static int strlist_cmp(const void *a, const void *b) {
  const char *const *pa = a;
  const char *const *pb = b;
  return strcmp(*pa, *pb);
}

void kbc_strlist_sort(kbc_strlist *l) {
  if (l == NULL || l->len < 2) {
    return;
  }
  qsort(l->items, l->len, sizeof(*l->items), strlist_cmp);
}

/* ================================ files ================================ */

bool kbc_path_exists(const char *path) {
  if (path == NULL) {
    return false;
  }
  struct stat st;
  return stat(path, &st) == 0;
}

kbc_status kbc_str_read_file(const char *path, kbc_str *out, kbc_err *err) {
  if (path == NULL || out == NULL) {
    return kbc_err_set(err, KBC_ERR_INVALID, "kbc_str_read_file: null argument");
  }
  int fd = open(path, O_RDONLY | O_CLOEXEC);
  if (fd < 0) {
    return kbc_err_set(err, KBC_ERR_IO, "open %s: %s", path, strerror(errno));
  }

  struct stat st;
  if (fstat(fd, &st) != 0) {
    kbc_status s = kbc_err_set(err, KBC_ERR_IO, "fstat %s: %s", path,
                               strerror(errno));
    close(fd);
    return s;
  }
  if (S_ISDIR(st.st_mode)) {
    close(fd);
    return kbc_err_set(err, KBC_ERR_INVALID, "%s is a directory", path);
  }
  if (S_ISREG(st.st_mode)) {
    if ((uintmax_t)st.st_size > (uintmax_t)KBC_MAX_ARTIFACT_BYTES) {
      close(fd);
      return kbc_err_set(err, KBC_ERR_INVALID,
                         "%s is %ju bytes, over the %u byte limit", path,
                         (uintmax_t)st.st_size, KBC_MAX_ARTIFACT_BYTES);
    }
  }

  /* Pre-size from the stat so a regular file is one reserve and one read. */
  size_t want = S_ISREG(st.st_mode) ? (size_t)st.st_size : 0u;
  if (want > 0 && !kbc_str_reserve(out, want)) {
    close(fd);
    return kbc_err_set(err, KBC_ERR_NOMEM, "read %s: %zu bytes", path, want);
  }

  char buf[65536];
  kbc_status status = KBC_OK;
  size_t got = 0;
  for (;;) {
    ssize_t n = read(fd, buf, sizeof(buf));
    if (n < 0) {
      if (errno == EINTR) {
        continue;
      }
      status = kbc_err_set(err, KBC_ERR_IO, "read %s: %s", path,
                           strerror(errno));
      break;
    }
    if (n == 0) {
      break;
    }
    got += (size_t)n;
    if (got > KBC_MAX_ARTIFACT_BYTES) {
      status = kbc_err_set(err, KBC_ERR_INVALID, "%s exceeds the %u byte limit",
                           path, KBC_MAX_ARTIFACT_BYTES);
      break;
    }
    status = kbc_str_append(out, buf, (size_t)n);
    if (status != KBC_OK) {
      status = kbc_err_set(err, status, "read %s: out of memory", path);
      break;
    }
  }

  if (close(fd) != 0 && status == KBC_OK) {
    status = kbc_err_set(err, KBC_ERR_IO, "close %s: %s", path,
                         strerror(errno));
  }
  return status;
}

static kbc_status write_all(int fd, const char *data, size_t n,
                            const char *path, kbc_err *err) {
  size_t done = 0;
  while (done < n) {
    ssize_t w = write(fd, data + done, n - done);
    if (w < 0) {
      if (errno == EINTR) {
        continue;
      }
      return kbc_err_set(err, KBC_ERR_IO, "write %s: %s", path,
                         strerror(errno));
    }
    if (w == 0) {
      return kbc_err_set(err, KBC_ERR_IO, "write %s: zero-length write", path);
    }
    done += (size_t)w;
  }
  return KBC_OK;
}

kbc_status kbc_str_write_file_atomic(const char *path, const char *data,
                                     size_t n, kbc_err *err) {
  if (path == NULL || (data == NULL && n > 0)) {
    return kbc_err_set(err, KBC_ERR_INVALID,
                       "kbc_str_write_file_atomic: null argument");
  }
  if (strlen(path) + 32u >= KBC_MAX_PATH_LEN) {
    return kbc_err_set(err, KBC_ERR_INVALID, "path too long (%zu bytes)",
                       strlen(path));
  }

  char tmp[KBC_MAX_PATH_LEN];
  int ntmp = snprintf(tmp, sizeof(tmp), "%s.tmp.%ld", path,
                      (long)getpid());
  if (ntmp < 0 || (size_t)ntmp >= sizeof(tmp)) {
    return kbc_err_set(err, KBC_ERR_INVALID, "temp path for %s does not fit",
                       path);
  }

  int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
  if (fd < 0) {
    return kbc_err_set(err, KBC_ERR_IO, "open %s: %s", tmp, strerror(errno));
  }

  kbc_status st = write_all(fd, data, n, tmp, err);
  if (st == KBC_OK && fsync(fd) != 0) {
    st = kbc_err_set(err, KBC_ERR_IO, "fsync %s: %s", tmp, strerror(errno));
  }
  if (close(fd) != 0 && st == KBC_OK) {
    st = kbc_err_set(err, KBC_ERR_IO, "close %s: %s", tmp, strerror(errno));
  }
  if (st != KBC_OK) {
    unlink(tmp);
    return st;
  }

  if (rename(tmp, path) != 0) {
    st = kbc_err_set(err, KBC_ERR_IO, "rename %s -> %s: %s", tmp, path,
                     strerror(errno));
    unlink(tmp);
    return st;
  }

  /* The rename itself is only durable once the directory entry is synced. */
  char dir[KBC_MAX_PATH_LEN];
  int ndir = snprintf(dir, sizeof(dir), "%s", path);
  if (ndir < 0 || (size_t)ndir >= sizeof(dir)) {
    return kbc_err_set(err, KBC_ERR_INVALID, "path too long to split: %s",
                       path);
  }
  char *slash = strrchr(dir, '/');
  if (slash == NULL) {
    dir[0] = '.';
    dir[1] = '\0';
  } else if (slash == dir) {
    slash[1] = '\0';
  } else {
    *slash = '\0';
  }
  int dfd = open(dir, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
  if (dfd < 0) {
    return kbc_err_set(err, KBC_ERR_IO, "open dir %s: %s", dir,
                       strerror(errno));
  }
  int rc = fsync(dfd);
  int saved = errno;
  close(dfd);
  if (rc != 0) {
    return kbc_err_set(err, KBC_ERR_IO, "fsync dir %s: %s", dir,
                       strerror(saved));
  }
  return KBC_OK;
}

kbc_status kbc_mkdir_p(const char *path, kbc_err *err) {
  if (path == NULL || path[0] == '\0') {
    return kbc_err_set(err, KBC_ERR_INVALID, "kbc_mkdir_p: empty path");
  }
  if (strlen(path) >= KBC_MAX_PATH_LEN) {
    return kbc_err_set(err, KBC_ERR_INVALID, "kbc_mkdir_p: path over %u bytes",
                       KBC_MAX_PATH_LEN);
  }

  char buf[KBC_MAX_PATH_LEN];
  size_t n = strlen(path);
  memcpy(buf, path, n + 1u);
  while (n > 1 && buf[n - 1] == '/') {
    buf[--n] = '\0';
  }

  for (size_t i = 1; i <= n; i++) {
    if (buf[i] != '/' && buf[i] != '\0') {
      continue;
    }
    char saved = buf[i];
    buf[i] = '\0';
    if (mkdir(buf, 0777) != 0 && errno != EEXIST) {
      kbc_status st = kbc_err_set(err, KBC_ERR_IO, "mkdir %s: %s", buf,
                                  strerror(errno));
      buf[i] = saved;
      return st;
    }
    buf[i] = saved;
  }

  struct stat st;
  if (stat(path, &st) != 0) {
    return kbc_err_set(err, KBC_ERR_IO, "stat %s: %s", path, strerror(errno));
  }
  if (!S_ISDIR(st.st_mode)) {
    return kbc_err_set(err, KBC_ERR_CONFLICT, "%s exists and is not a directory",
                       path);
  }
  return KBC_OK;
}

/* ================================ bytes ================================ */

uint32_t kbc_fnv1a32(const void *data, size_t n) {
  const unsigned char *p = data;
  uint32_t h = 0x811c9dc5u;
  if (p == NULL) {
    return h;
  }
  for (size_t i = 0; i < n; i++) {
    h ^= (uint32_t)p[i];
    h *= 0x01000193u;
  }
  return h;
}

static const char base64_alphabet[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

kbc_status kbc_base64_encode(kbc_str *out, const void *data, size_t n) {
  if (out == NULL || (data == NULL && n > 0)) {
    return KBC_ERR_INVALID;
  }
  if (n == 0) {
    return KBC_OK;
  }
  const unsigned char *p = data;
  if (n > (SIZE_MAX / 4u) * 3u) {
    return KBC_ERR_NOMEM;
  }
  /* out_len is ceil(n/3)*4, computed without overflow thanks to the check. */
  size_t need = ((n + 2u) / 3u) * 4u;
  if (!kbc_str_reserve(out, need)) {
    return KBC_ERR_NOMEM;
  }

  char *w = out->ptr + out->len;
  size_t i = 0;
  while (i + 3u <= n) {
    uint32_t v = ((uint32_t)p[i] << 16) | ((uint32_t)p[i + 1u] << 8) |
                 (uint32_t)p[i + 2u];
    *w++ = base64_alphabet[(v >> 18) & 0x3fu];
    *w++ = base64_alphabet[(v >> 12) & 0x3fu];
    *w++ = base64_alphabet[(v >> 6) & 0x3fu];
    *w++ = base64_alphabet[v & 0x3fu];
    i += 3u;
  }
  size_t rest = n - i;
  if (rest == 1) {
    uint32_t v = (uint32_t)p[i] << 16;
    *w++ = base64_alphabet[(v >> 18) & 0x3fu];
    *w++ = base64_alphabet[(v >> 12) & 0x3fu];
    *w++ = '=';
    *w++ = '=';
  } else if (rest == 2) {
    uint32_t v = ((uint32_t)p[i] << 16) | ((uint32_t)p[i + 1u] << 8);
    *w++ = base64_alphabet[(v >> 18) & 0x3fu];
    *w++ = base64_alphabet[(v >> 12) & 0x3fu];
    *w++ = base64_alphabet[(v >> 6) & 0x3fu];
    *w++ = '=';
  }
  out->len += need;
  out->ptr[out->len] = '\0';
  return KBC_OK;
}

static int base64_value(unsigned char c) {
  if (c >= 'A' && c <= 'Z') {
    return (int)(c - 'A');
  }
  if (c >= 'a' && c <= 'z') {
    return (int)(c - 'a') + 26;
  }
  if (c >= '0' && c <= '9') {
    return (int)(c - '0') + 52;
  }
  if (c == '+') {
    return 62;
  }
  if (c == '/') {
    return 63;
  }
  return -1;
}

kbc_status kbc_base64_decode(kbc_arena *a, const char *in, size_t n,
                             const char **out, size_t *out_len, kbc_err *err) {
  if (out != NULL) {
    *out = NULL;
  }
  if (out_len != NULL) {
    *out_len = 0;
  }
  if (a == NULL || in == NULL || out == NULL || out_len == NULL) {
    return kbc_err_set(err, KBC_ERR_INVALID, "kbc_base64_decode: null argument");
  }
  if (n % 4u != 0u) {
    return kbc_err_set(err, KBC_ERR_INVALID,
                       "base64: length %zu is not a multiple of 4", n);
  }
  if (n == 0) {
    char *empty = kbc_arena_alloc(a, 1);
    if (empty == NULL) {
      return kbc_err_set(err, KBC_ERR_NOMEM, "base64: empty allocation");
    }
    empty[0] = '\0';
    *out = empty;
    *out_len = 0;
    return KBC_OK;
  }

  size_t pad = 0;
  if (in[n - 1u] == '=') {
    pad++;
  }
  if (n >= 2u && in[n - 2u] == '=') {
    pad++;
  }

  size_t need = (n / 4u) * 3u - pad;
  unsigned char *buf = kbc_arena_alloc(a, need);
  if (buf == NULL) {
    return kbc_err_set(err, KBC_ERR_NOMEM, "base64: %zu bytes", need);
  }

  size_t written = 0;
  for (size_t i = 0; i < n; i += 4u) {
    int v[4];
    for (int k = 0; k < 4; k++) {
      unsigned char c = (unsigned char)in[i + (size_t)k];
      if (c == '=') {
        /* Padding is legal only in the final quantum's last two slots. */
        if (i + 4u != n || k < 2) {
          kbc_err_set(err, KBC_ERR_INVALID,
                      "base64: misplaced padding at offset %zu",
                      i + (size_t)k);
          return KBC_ERR_INVALID;
        }
        v[k] = 0;
        continue;
      }
      int d = base64_value(c);
      if (d < 0) {
        kbc_err_set(err, KBC_ERR_INVALID,
                    "base64: invalid character 0x%02x at offset %zu", c,
                    i + (size_t)k);
        return KBC_ERR_INVALID;
      }
      v[k] = d;
    }
    uint32_t bits = ((uint32_t)v[0] << 18) | ((uint32_t)v[1] << 12) |
                    ((uint32_t)v[2] << 6) | (uint32_t)v[3];
    size_t emit = 3;
    if (i + 4u == n) {
      emit = 3u - pad;
    }
    for (size_t k = 0; k < emit; k++) {
      buf[written++] = (unsigned char)((bits >> (16 - 8 * k)) & 0xffu);
    }
  }

  *out = (const char *)buf;
  *out_len = written;
  return KBC_OK;
}

kbc_status kbc_hex_encode(kbc_str *out, const void *data, size_t n) {
  if (out == NULL || (data == NULL && n > 0)) {
    return KBC_ERR_INVALID;
  }
  if (n == 0) {
    return KBC_OK;
  }
  if (n > (SIZE_MAX - 1u) / 2u) {
    return KBC_ERR_NOMEM;
  }
  const unsigned char *p = data;
  size_t need = n * 2u;
  if (!kbc_str_reserve(out, need)) {
    return KBC_ERR_NOMEM;
  }
  static const char hex_digits[] = "0123456789abcdef";
  char *w = out->ptr + out->len;
  for (size_t i = 0; i < n; i++) {
    *w++ = hex_digits[p[i] >> 4];
    *w++ = hex_digits[p[i] & 0x0fu];
  }
  out->len += need;
  out->ptr[out->len] = '\0';
  return KBC_OK;
}

static int hex_value(unsigned char c) {
  if (c >= '0' && c <= '9') {
    return (int)(c - '0');
  }
  if (c >= 'a' && c <= 'f') {
    return (int)(c - 'a') + 10;
  }
  if (c >= 'A' && c <= 'F') {
    return (int)(c - 'A') + 10;
  }
  return -1;
}

bool kbc_hex_decode(const char *in, size_t n, uint8_t *out, size_t out_cap,
                    size_t *out_len) {
  if (out_len != NULL) {
    *out_len = 0;
  }
  if (in == NULL || out == NULL || out_len == NULL) {
    return false;
  }
  if (n % 2u != 0u) {
    return false;
  }
  if (n / 2u > out_cap) {
    return false;
  }
  for (size_t i = 0; i < n; i += 2u) {
    int hi = hex_value((unsigned char)in[i]);
    int lo = hex_value((unsigned char)in[i + 1u]);
    if (hi < 0 || lo < 0) {
      return false;
    }
    out[i / 2u] = (uint8_t)((hi << 4) | lo);
  }
  *out_len = n / 2u;
  return true;
}

bool kbc_const_time_eq(const void *a, const void *b, size_t n) {
  if (n == 0) {
    return true;
  }
  if (a == NULL || b == NULL) {
    return false;
  }
  const unsigned char *pa = a;
  const unsigned char *pb = b;
  unsigned diff = 0;
  for (size_t i = 0; i < n; i++) {
    diff |= (unsigned)(pa[i] ^ pb[i]);
  }
  /* No early return: the loop always runs to n so the comparison time does
   * not reveal the position of the first differing byte. */
  return diff == 0;
}

int64_t kbc_now_ns(void) {
  struct timespec ts;
  if (clock_gettime(CLOCK_REALTIME, &ts) != 0) {
    return 0;
  }
  return (int64_t)ts.tv_sec * 1000000000 + (int64_t)ts.tv_nsec;
}

int64_t kbc_now_iso8601(char *buf, size_t cap) {
  /* "YYYY-MM-DDTHH:MM:SSZ" plus the NUL. */
  static const size_t need = 21u;
  if (buf == NULL || cap < need) {
    return 0;
  }
  time_t now = (time_t)kbc_now_ns() / 1000000000;
  struct tm tm_buf;
  struct tm *tm = gmtime_r(&now, &tm_buf);
  if (tm == NULL) {
    buf[0] = '\0';
    return 0;
  }
  int n = snprintf(buf, cap, "%04d-%02d-%02dT%02d:%02d:%02dZ",
                   tm->tm_year + 1900, tm->tm_mon + 1, tm->tm_mday, tm->tm_hour,
                   tm->tm_min, tm->tm_sec);
  if (n < 0 || (size_t)n >= cap) {
    buf[0] = '\0';
    return 0;
  }
  return (int64_t)n;
}

/* ================================ utf-8 ================================ */

size_t kbc_utf8_len(unsigned char c) {
  /* Lead byte only: the length a well-formed sequence would have. 0xC0/0xC1
   * (overlong two-byte) and 0xF5..0xFF (beyond U+10FFFF) are rejected. */
  if (c < 0x80u) {
    return 1u;
  }
  if (c >= 0xc2u && c <= 0xdfu) {
    return 2u;
  }
  if (c >= 0xe0u && c <= 0xefu) {
    return 3u;
  }
  if (c >= 0xf0u && c <= 0xf4u) {
    return 4u;
  }
  return 0u;
}

/* Decodes one sequence in [s, s+n). Returns the codepoint and sets *used, or
 * returns false for overlongs, surrogates and anything past U+10FFFF. */
static bool utf8_decode(const char *s, size_t n, uint32_t *cp, size_t *used) {
  const unsigned char *p = (const unsigned char *)s;
  unsigned char c = p[0];
  size_t len = kbc_utf8_len(c);
  if (len == 0 || len > n) {
    return false;
  }

  uint32_t v;
  switch (len) {
  case 1:
    v = c;
    break;
  case 2:
    v = (uint32_t)(c & 0x1fu);
    break;
  case 3:
    v = (uint32_t)(c & 0x0fu);
    break;
  default:
    v = (uint32_t)(c & 0x07u);
    break;
  }
  for (size_t i = 1; i < len; i++) {
    if ((p[i] & 0xc0u) != 0x80u) {
      return false;
    }
    v = (v << 6) | (uint32_t)(p[i] & 0x3fu);
  }

  static const uint32_t min_for_len[5] = {0u, 0u, 0x80u, 0x800u, 0x10000u};
  if (v < min_for_len[len] || v > 0x10ffffu) {
    return false;
  }
  if (v >= 0xd800u && v <= 0xdfffu) {
    return false;
  }
  *cp = v;
  *used = len;
  return true;
}

bool kbc_utf8_validate(const char *s, size_t n, size_t *bad_offset) {
  if (s == NULL) {
    if (bad_offset != NULL) {
      *bad_offset = 0;
    }
    return false;
  }
  size_t i = 0;
  while (i < n) {
    uint32_t cp = 0;
    size_t used = 0;
    if (!utf8_decode(s + i, n - i, &cp, &used)) {
      if (bad_offset != NULL) {
        *bad_offset = i;
      }
      return false;
    }
    i += used;
  }
  if (bad_offset != NULL) {
    *bad_offset = n;
  }
  return true;
}

size_t kbc_utf8_count(const char *s, size_t n) {
  if (s == NULL) {
    return 0;
  }
  size_t i = 0;
  size_t count = 0;
  while (i < n) {
    uint32_t cp = 0;
    size_t used = 0;
    if (!utf8_decode(s + i, n - i, &cp, &used)) {
      /* A malformed byte counts as one unit so the caller still advances and
       * never loops forever on hostile input. */
      count++;
      i++;
      continue;
    }
    count++;
    i += used;
  }
  return count;
}

/* Latin-1 (0xC0..0xFF) and Latin Extended-A (0x100..0x17F) folded to ASCII.
 * Each entry packs up to four non-NUL ASCII bytes, low byte first; 0 means
 * "no letters" (the multiplication and division signs).
 *
 * The fold is case-canonical: every codepoint folds to LOWERCASE ASCII (or a
 * lowercase digraph, or nothing), so the upper- and lowercase forms of a
 * letter agree and a folded search cannot miss a differently-cased document.
 * Digraphs: ß->ss, Æ/æ->ae, Œ/œ->oe, Ĳ/ĳ->ij, þ->th. Ŋ/ŋ fold to "n", not
 * "ng", because they are one case pair and must agree.
 *
 * GENERATED, not hand-written — one row per 16 codepoints, row N holds
 * 0xC0+16*N. Reproduce with:
 *
 *   python3 - <<'PY' > /tmp/rows.txt
 *   import unicodedata
 *   SPECIAL={0xDF:'ss',0xC6:'ae',0xE6:'ae',0x152:'oe',0x153:'oe',0x132:'ij',
 *     0x133:'ij',0x14A:'n',0x14B:'n',0x149:'n',0xD7:'',0xF7:'',
 *     0xD0:'d',0xF0:'d',0xD8:'o',0xF8:'o',0xDE:'th',0xFE:'th',
 *     0x110:'d',0x111:'d',0x126:'h',0x127:'h',0x131:'i',0x138:'k',0x13F:'l',
 *     0x140:'l',0x141:'l',0x142:'l',0x166:'t',0x167:'t'}
 *   def fold(ch):
 *     cp=ord(ch)
 *     if cp in SPECIAL: return SPECIAL[cp]
 *     d=unicodedata.normalize('NFKD',ch)
 *     d=''.join(c for c in d if not unicodedata.combining(c)).lower()
 *     assert all(ord(c)<128 for c in d),(hex(cp),d)
 *     return d
 *   vals=[]
 *   for cp in range(0xC0,0x180):
 *     b=fold(chr(cp))
 *     u=chr(cp).upper()
 *     if len(u)==1 and 0x100<=ord(u)<=0x17F:
 *       assert fold(u)==b,(hex(cp),b,hex(ord(u)),fold(u))  # case agreement
 *     assert b==b.lower() and all('a'<=c<='z' for c in b) and len(b)<=4
 *     vals.append(b)
 *   FMT='    '+chr(47)+'* 0x%03X *'+chr(47)+' %s,'
 *   for i in range(0,192,16):
 *     c=[('0' if not v else "'"+v+"'" if len(v)==1 else
 *         "'"+v[0]+"'<<8|'"+v[1]+"'") for v in vals[i:i+16]]
 *     print(FMT % (0xC0+i, ', '.join(c)))
 *   PY
 */
static const uint32_t latin_fold[0xc0u] = {
    /* 0x0C0 */ 'a', 'a', 'a', 'a', 'a', 'a', 'a'<<8|'e', 'c', 'e', 'e', 'e', 'e', 'i', 'i', 'i', 'i',
    /* 0x0D0 */ 'd', 'n', 'o', 'o', 'o', 'o', 'o', 0, 'o', 'u', 'u', 'u', 'u', 'y', 't'<<8|'h', 's'<<8|'s',
    /* 0x0E0 */ 'a', 'a', 'a', 'a', 'a', 'a', 'a'<<8|'e', 'c', 'e', 'e', 'e', 'e', 'i', 'i', 'i', 'i',
    /* 0x0F0 */ 'd', 'n', 'o', 'o', 'o', 'o', 'o', 0, 'o', 'u', 'u', 'u', 'u', 'y', 't'<<8|'h', 'y',
    /* 0x100 */ 'a', 'a', 'a', 'a', 'a', 'a', 'c', 'c', 'c', 'c', 'c', 'c', 'c', 'c', 'd', 'd',
    /* 0x110 */ 'd', 'd', 'e', 'e', 'e', 'e', 'e', 'e', 'e', 'e', 'e', 'e', 'g', 'g', 'g', 'g',
    /* 0x120 */ 'g', 'g', 'g', 'g', 'h', 'h', 'h', 'h', 'i', 'i', 'i', 'i', 'i', 'i', 'i', 'i',
    /* 0x130 */ 'i', 'i', 'i'<<8|'j', 'i'<<8|'j', 'j', 'j', 'k', 'k', 'k', 'l', 'l', 'l', 'l', 'l', 'l', 'l',
    /* 0x140 */ 'l', 'l', 'l', 'n', 'n', 'n', 'n', 'n', 'n', 'n', 'n', 'n', 'o', 'o', 'o', 'o',
    /* 0x150 */ 'o', 'o', 'o'<<8|'e', 'o'<<8|'e', 'r', 'r', 'r', 'r', 'r', 'r', 's', 's', 's', 's', 's', 's',
    /* 0x160 */ 's', 's', 't', 't', 't', 't', 't', 't', 'u', 'u', 'u', 'u', 'u', 'u', 'u', 'u',
    /* 0x170 */ 'u', 'u', 'u', 'u', 'w', 'w', 'y', 'y', 'y', 'z', 'z', 'z', 'z', 'z', 'z', 's'};

size_t kbc_fold_utf8(const char *s, size_t n, char out[32]) {
  if (out == NULL) {
    return 0;
  }
  out[0] = '\0';
  if (s == NULL || n == 0) {
    return 0;
  }

  const unsigned char *p = (const unsigned char *)s;
  size_t len = kbc_utf8_len(p[0]);
  if (len == 0 || len > n) {
    /* Not a sequence we can fold: copy the byte through so the caller keeps
     * a same-length result and no sequence is ever split. */
    out[0] = (char)p[0];
    out[1] = '\0';
    return 1;
  }

  if (len == 1) {
    unsigned char c = p[0];
    if (c >= 'A' && c <= 'Z') {
      c = (unsigned char)(c - 'A' + 'a');
    }
    out[0] = (char)c;
    out[1] = '\0';
    return 1;
  }

  uint32_t cp = 0;
  size_t used = 0;
  if (!utf8_decode(s, n, &cp, &used)) {
    memcpy(out, s, len);
    out[len] = '\0';
    return len;
  }

  if (cp >= 0xc0u && cp - 0xc0u < sizeof(latin_fold) / sizeof(latin_fold[0])) {
    uint32_t packed = latin_fold[cp - 0xc0u];
    if (packed != 0) {
      size_t w = 0;
      for (size_t k = 0; k < 4; k++) {
        unsigned char ch = (unsigned char)((packed >> (8 * k)) & 0xffu);
        if (ch == 0) {
          break;
        }
        out[w++] = (char)ch;
      }
      out[w] = '\0';
      return w;
    }
    out[0] = '\0';
    return 0;
  }

  memcpy(out, s, len);
  out[len] = '\0';
  return len;
}
