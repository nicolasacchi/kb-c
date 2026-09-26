/* embed.c — the vector lane: a stdio sidecar plus a flat mmap'd vector store.
 *
 * The sidecar protocol is newline-delimited JSON (see embed.h). Two rules
 * shape everything here:
 *
 *   1. A reply is never half-trusted. Every length that came out of the pipe
 *      (dim, vector count, per-vector length) is validated against the request
 *      we actually sent before a single float is copied out. A mismatch is a
 *      protocol error: the child is killed and the embedder marked unhealthy,
 *      because a sidecar that lies about shapes may also lie about content.
 *   2. kbc_embedder_embed is the ONLY user of the pipes, so one mutex covers
 *      a whole request/response exchange. Two threads must never interleave
 *      halves of a conversation.
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <stdio.h>

#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "kbc/embed.h"

/* One JSON line may not exceed this. A longer line is a protocol error, not a
 * buffer overflow: we stop reading and kill the child. */
#define KBC_EMBED_LINE_MAX (64u * 1024u * 1024u)
#define KBC_EMBED_DEFAULT_TIMEOUT_MS 30000
#define KBC_EMBED_STOP_GRACE_MS 2000
#define KBC_EMBED_POLL_SLICE_MS 1000
#define KBC_EMBED_READ_CHUNK 8192u
#define KBC_EMBED_MAX_DIM 65536u

/* --------------------------------------------------------------- sidecar - */

struct kbc_embedder {
  pid_t pid;             /* -1 when no child */
  int in_fd;             /* -> child stdin */
  int out_fd;            /* <- child stdout */
  char **args;           /* KBC_OWN, NULL-terminated, reused by restart */
  pthread_mutex_t mu;    /* serializes one request/response exchange */
  _Atomic bool healthy;
  _Atomic size_t dim;
  _Atomic int64_t requests;
  _Atomic int64_t failures;
};

typedef enum {
  LR_OK = 0,
  LR_EOF,
  LR_TIMEOUT,
  LR_IO,
  LR_TOOLONG
} line_res;

static int64_t now_ms(void) {
  struct timespec ts;
  if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
    return 0;
  }
  return (int64_t)ts.tv_sec * 1000 + (int64_t)ts.tv_nsec / 1000000;
}

/* Reads one newline-terminated line into `out` (appended; cleared by caller).
 * Handles short reads and a line that arrives in pieces. */
static line_res read_line(int fd, kbc_str *out, int timeout_ms) {
  const int64_t deadline = now_ms() + (int64_t)timeout_ms;
  for (;;) {
    if (out->len > 0) {
      const char *nl =
          (const char *)memchr(out->ptr, '\n', out->len);
      if (nl != NULL) {
        size_t n = (size_t)(nl - out->ptr);
        if (n > 0 && out->ptr[n - 1] == '\r') {
          n--;
        }
        out->len = n;
        out->ptr[n] = '\0';
        return LR_OK;
      }
    }
    if (out->len >= KBC_EMBED_LINE_MAX) {
      return LR_TOOLONG;
    }
    int64_t left = deadline - now_ms();
    if (left <= 0) {
      return LR_TIMEOUT;
    }
    if (left > KBC_EMBED_POLL_SLICE_MS) {
      left = KBC_EMBED_POLL_SLICE_MS;
    }
    struct pollfd pfd;
    pfd.fd = fd;
    pfd.events = POLLIN;
    pfd.revents = 0;
    int pr = poll(&pfd, 1, (int)left);
    if (pr < 0) {
      if (errno == EINTR) {
        continue;
      }
      return LR_IO;
    }
    if (pr == 0) {
      continue; /* re-check the deadline, not a hard timeout */
    }
    if ((pfd.revents & (POLLIN | POLLHUP | POLLERR | POLLNVAL)) == 0) {
      continue;
    }
    char buf[KBC_EMBED_READ_CHUNK];
    ssize_t got = read(fd, buf, sizeof buf);
    if (got > 0) {
      if (kbc_failed(kbc_str_append(out, buf, (size_t)got))) {
        return LR_IO;
      }
      continue;
    }
    if (got == 0) {
      return LR_EOF;
    }
    if (errno == EINTR) {
      continue;
    }
    return LR_IO;
  }
}

/* Full-write loop: short writes and EINTR are normal, not errors. */
static kbc_status write_all(int fd, const char *data, size_t n, kbc_err *err) {
  size_t off = 0;
  while (off < n) {
    ssize_t w = write(fd, data + off, n - off);
    if (w > 0) {
      off += (size_t)w;
      continue;
    }
    if (w < 0 && errno == EINTR) {
      continue;
    }
    if (w < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
      struct pollfd pfd;
      pfd.fd = fd;
      pfd.events = POLLOUT;
      pfd.revents = 0;
      if (poll(&pfd, 1, KBC_EMBED_DEFAULT_TIMEOUT_MS) < 0 && errno != EINTR) {
        return kbc_err_set(err, KBC_ERR_IO, "write to sidecar poll: %s",
                           strerror(errno));
      }
      continue;
    }
    return kbc_err_set(err, KBC_ERR_IO, "write to sidecar: %s",
                       strerror(errno));
  }
  return KBC_OK;
}

static void close_fds(kbc_embedder *e) {
  if (e->in_fd >= 0) {
    (void)close(e->in_fd);
    e->in_fd = -1;
  }
  if (e->out_fd >= 0) {
    (void)close(e->out_fd);
    e->out_fd = -1;
  }
}

/* SIGKILL then reap. Used on every teardown path, including protocol errors,
 * so no path can leave a zombie. */
static void reap_kill(kbc_embedder *e) {
  if (e->pid > 0) {
    (void)kill(e->pid, SIGKILL);
    pid_t r;
    do {
      r = waitpid(e->pid, NULL, 0);
    } while (r < 0 && errno == EINTR);
  }
  e->pid = -1;
  close_fds(e);
}

/* Close stdin, poll for exit up to grace_ms, then SIGKILL. */
static void reap_graceful(kbc_embedder *e, int grace_ms) {
  if (e->in_fd >= 0) {
    (void)close(e->in_fd);
    e->in_fd = -1;
  }
  if (e->out_fd >= 0) {
    (void)close(e->out_fd);
    e->out_fd = -1;
  }
  if (e->pid <= 0) {
    return;
  }
  int waited = 0;
  for (;;) {
    pid_t r = waitpid(e->pid, NULL, WNOHANG);
    if (r == e->pid) {
      e->pid = -1;
      return;
    }
    if (r < 0 && errno == ECHILD) {
      e->pid = -1;
      return;
    }
    if (r < 0 && errno != EINTR) {
      break;
    }
    if (waited >= grace_ms) {
      break;
    }
    struct timespec ts;
    ts.tv_sec = 0;
    ts.tv_nsec = 20L * 1000L * 1000L;
    (void)nanosleep(&ts, NULL);
    waited += 20;
  }
  reap_kill(e);
}

/* Forks and execs args[0]; pipes carry stdin/stdout, stderr is inherited. */
static kbc_status spawn(kbc_embedder *e, kbc_err *err) {
  int in_pipe[2];
  int out_pipe[2];
  if (pipe2(in_pipe, O_CLOEXEC) != 0) {
    return kbc_err_set(err, KBC_ERR_IO, "pipe2 for sidecar stdin: %s",
                       strerror(errno));
  }
  if (pipe2(out_pipe, O_CLOEXEC) != 0) {
    int saved = errno;
    (void)close(in_pipe[0]);
    (void)close(in_pipe[1]);
    return kbc_err_set(err, KBC_ERR_IO, "pipe2 for sidecar stdout: %s",
                       strerror(saved));
  }
  pid_t pid = fork();
  if (pid < 0) {
    int saved = errno;
    (void)close(in_pipe[0]);
    (void)close(in_pipe[1]);
    (void)close(out_pipe[0]);
    (void)close(out_pipe[1]);
    return kbc_err_set(err, KBC_ERR_IO, "fork for sidecar %s: %s", e->args[0],
                       strerror(saved));
  }
  if (pid == 0) {
    /* child */
    if (dup2(in_pipe[0], STDIN_FILENO) < 0 ||
        dup2(out_pipe[1], STDOUT_FILENO) < 0) {
      _exit(127);
    }
    (void)close(in_pipe[0]);
    (void)close(in_pipe[1]);
    (void)close(out_pipe[0]);
    (void)close(out_pipe[1]);
    execvp(e->args[0], e->args);
    _exit(127);
  }
  /* parent: the child's ends are CLOEXEC-closed by exec; close them now. */
  (void)close(in_pipe[0]);
  (void)close(out_pipe[1]);

  int wfd = fcntl(in_pipe[1], F_DUPFD, 3);
  int rfd = fcntl(out_pipe[0], F_DUPFD, 3);
  if (wfd < 0 || rfd < 0) {
    int saved = errno;
    if (wfd >= 0) {
      (void)close(wfd);
    }
    if (rfd >= 0) {
      (void)close(rfd);
    }
    (void)close(in_pipe[1]);
    (void)close(out_pipe[0]);
    e->pid = pid;
    reap_kill(e);
    return kbc_err_set(err, KBC_ERR_IO, "dup sidecar pipe above fd 2: %s",
                       strerror(saved));
  }
  e->in_fd = wfd;
  e->out_fd = rfd;
  e->pid = pid;
  atomic_store(&e->healthy, true);
  return KBC_OK;
}

/* Sends one pre-serialized line and reads one reply line. Caller holds mu. */
static kbc_status exchange(kbc_embedder *e, const kbc_str *req, kbc_str *line,
                           int timeout_ms, kbc_err *err) {
  kbc_str_clear(line);
  if (e->out_fd < 0 || e->in_fd < 0 || e->pid <= 0) {
    return kbc_err_set(err, KBC_ERR_IO, "sidecar is not running");
  }
  kbc_status st = write_all(e->in_fd, req->ptr, req->len, err);
  if (kbc_failed(st)) {
    return st;
  }
  switch (read_line(e->out_fd, line, timeout_ms)) {
    case LR_OK:
      return KBC_OK;
    case LR_TIMEOUT:
      return kbc_err_set(err, KBC_ERR_TIMEOUT,
                         "sidecar %s: no reply within %d ms", e->args[0],
                         timeout_ms);
    case LR_EOF:
      return kbc_err_set(err, KBC_ERR_IO, "sidecar %s: stdout closed",
                         e->args[0]);
    case LR_TOOLONG:
      return kbc_err_set(err, KBC_ERR_PARSE,
                         "sidecar %s: reply line exceeds %u bytes",
                         e->args[0], KBC_EMBED_LINE_MAX);
    case LR_IO:
    default:
      return kbc_err_set(err, KBC_ERR_IO, "read from sidecar %s: %s",
                         e->args[0], strerror(errno));
  }
}

kbc_embedder *kbc_embedder_start(const char *const *argv, kbc_err *err) {
  if (argv == NULL || argv[0] == NULL) {
    (void)kbc_err_set(err, KBC_ERR_INVALID, "argv[0] must not be NULL");
    return NULL;
  }
  size_t argc = 0;
  while (argv[argc] != NULL) {
    if (argc == 512) {
      (void)kbc_err_set(err, KBC_ERR_INVALID, "argv has more than 512 entries");
      return NULL;
    }
    argc++;
  }
  kbc_embedder *e = calloc(1, sizeof *e);
  if (e == NULL) {
    (void)kbc_err_set(err, KBC_ERR_NOMEM, "calloc kbc_embedder (%zu bytes)",
                      sizeof *e);
    return NULL;
  }
  e->pid = -1;
  e->in_fd = -1;
  e->out_fd = -1;
  e->args = calloc(argc + 1, sizeof *e->args);
  if (e->args == NULL) {
    free(e);
    (void)kbc_err_set(err, KBC_ERR_NOMEM, "calloc sidecar argv");
    return NULL;
  }
  for (size_t i = 0; i < argc; i++) {
    size_t n = strlen(argv[i]);
    char *copy = malloc(n + 1);
    if (copy == NULL) {
      for (size_t j = 0; j < i; j++) {
        free(e->args[j]);
      }
      free(e->args);
      free(e);
      (void)kbc_err_set(err, KBC_ERR_NOMEM, "malloc sidecar argv[%zu]", i);
      return NULL;
    }
    memcpy(copy, argv[i], n + 1);
    e->args[i] = copy;
  }
  e->args[argc] = NULL;
  if (pthread_mutex_init(&e->mu, NULL) != 0) {
    for (size_t i = 0; i < argc; i++) {
      free(e->args[i]);
    }
    free(e->args);
    free(e);
    (void)kbc_err_set(err, KBC_ERR_INTERNAL, "pthread_mutex_init for sidecar");
    return NULL;
  }
  kbc_status st = spawn(e, err);
  if (kbc_failed(st)) {
    for (size_t i = 0; i < argc; i++) {
      free(e->args[i]);
    }
    free(e->args);
    (void)pthread_mutex_destroy(&e->mu);
    free(e);
    return NULL;
  }
  return e;
}

void kbc_embedder_stop(kbc_embedder *e) {
  if (e == NULL) {
    return;
  }
  (void)pthread_mutex_lock(&e->mu);
  atomic_store(&e->healthy, false);
  reap_graceful(e, KBC_EMBED_STOP_GRACE_MS);
  (void)pthread_mutex_unlock(&e->mu);
  for (size_t i = 0; e->args != NULL && e->args[i] != NULL; i++) {
    free(e->args[i]);
  }
  free(e->args);
  (void)pthread_mutex_destroy(&e->mu);
  free(e);
}

bool kbc_embedder_healthy(const kbc_embedder *e) {
  return e != NULL && atomic_load(&e->healthy);
}

size_t kbc_embedder_dim(const kbc_embedder *e) {
  return e == NULL ? 0 : atomic_load(&e->dim);
}

void kbc_embedder_counts(const kbc_embedder *e, int64_t *requests,
                         int64_t *failures) {
  if (requests != NULL) {
    *requests = e == NULL ? 0 : atomic_load(&e->requests);
  }
  if (failures != NULL) {
    *failures = e == NULL ? 0 : atomic_load(&e->failures);
  }
}

/* Health handshake: {"op":"health"} must come back ok with a usable dim. */
static kbc_status handshake(kbc_embedder *e, kbc_err *err) {
  static const char kHealth[] = "{\"op\":\"health\"}\n";
  kbc_str req;
  kbc_str_init(&req);
  kbc_str line;
  kbc_str_init(&line);
  kbc_status st = KBC_OK;

  (void)pthread_mutex_lock(&e->mu);
  st = kbc_str_puts(&req, kHealth);
  if (st == KBC_OK) {
    st = exchange(e, &req, &line, KBC_EMBED_DEFAULT_TIMEOUT_MS, err);
  } else {
    (void)kbc_err_set(err, KBC_ERR_NOMEM, "health request buffer");
  }
  (void)pthread_mutex_unlock(&e->mu);
  if (kbc_failed(st)) {
    kbc_str_free(&req);
    kbc_str_free(&line);
    return st;
  }
  kbc_arena *a = kbc_arena_new(4096);
  if (a == NULL) {
    kbc_str_free(&req);
    kbc_str_free(&line);
    return kbc_err_set(err, KBC_ERR_NOMEM, "arena for health reply");
  }
  kbc_json *j = kbc_json_parse(a, line.ptr, line.len, err);
  if (j == NULL || !kbc_json_is(j, KBC_JSON_OBJ) ||
      !kbc_json_bool(j, "ok", false)) {
    kbc_err_set(err, KBC_ERR_PARSE, "sidecar health reply is not {\"ok\":true}");
    st = KBC_ERR_PARSE;
  } else {
    double d = kbc_json_num(j, "dim", 0.0);
    if (!(d >= 1.0) || d > (double)KBC_EMBED_MAX_DIM) {
      kbc_err_set(err, KBC_ERR_PARSE, "sidecar health dim is out of range: %g",
                  d);
      st = KBC_ERR_PARSE;
    } else {
      atomic_store(&e->dim, (size_t)d);
    }
  }
  kbc_arena_free(a);
  kbc_str_free(&req);
  kbc_str_free(&line);
  return st;
}

kbc_status kbc_embedder_restart(kbc_embedder *e, kbc_err *err) {
  if (e == NULL) {
    return kbc_err_set(err, KBC_ERR_INVALID, "embedder must not be NULL");
  }
  (void)pthread_mutex_lock(&e->mu);
  atomic_store(&e->healthy, false);
  reap_graceful(e, KBC_EMBED_STOP_GRACE_MS);
  kbc_status st = spawn(e, err);
  (void)pthread_mutex_unlock(&e->mu);
  if (kbc_failed(st)) {
    return st;
  }
  st = handshake(e, err);
  if (kbc_failed(st)) {
    (void)pthread_mutex_lock(&e->mu);
    reap_graceful(e, KBC_EMBED_STOP_GRACE_MS);
    (void)pthread_mutex_unlock(&e->mu);
  }
  return st;
}

/* Pulls a non-negative integral dim out of a reply, or 0 when absent. */
static size_t reply_dim(const kbc_json *j, bool *bad) {
  *bad = false;
  const kbc_json *d = kbc_json_get(j, "dim");
  if (d == NULL) {
    return 0;
  }
  if (!kbc_json_is(d, KBC_JSON_NUM)) {
    *bad = true;
    return 0;
  }
  double v = d->u.num;
  if (!(v >= 1.0) || v > (double)KBC_EMBED_MAX_DIM || v != floor(v)) {
    *bad = true;
    return 0;
  }
  return (size_t)v;
}

kbc_status kbc_embedder_embed(kbc_embedder *e, kbc_arena *a,
                              const char *const *texts, size_t n,
                              size_t dim_hint, float **out, kbc_err *err) {
  if (e == NULL || a == NULL || out == NULL) {
    return kbc_err_set(err, KBC_ERR_INVALID,
                       "embedder, arena and out must not be NULL");
  }
  *out = NULL;
  if (n > 0 && texts == NULL) {
    return kbc_err_set(err, KBC_ERR_INVALID, "texts must not be NULL for n=%zu",
                       n);
  }
  if (n == 0) {
    return KBC_OK;
  }
  const size_t known = kbc_embedder_dim(e);
  if (dim_hint != 0 && known != 0 && dim_hint != known) {
    return kbc_err_set(err, KBC_ERR_INVALID,
                       "dim_hint %zu does not match sidecar dim %zu", dim_hint,
                       known);
  }

  /* --- request: build, serialize, append '\n' ------------------------- */
  kbc_arena *ra = kbc_arena_new(4096);
  if (ra == NULL) {
    return kbc_err_set(err, KBC_ERR_NOMEM, "arena for embed request");
  }
  kbc_str req;
  kbc_str_init(&req);
  kbc_str line;
  kbc_str_init(&line);
  kbc_status st = KBC_OK;

  kbc_json *obj = kbc_json_new_obj(ra);
  kbc_json *arr = kbc_json_new_arr(ra);
  if (obj == NULL || arr == NULL) {
    st = kbc_err_set(err, KBC_ERR_NOMEM, "embed request json");
  }
  for (size_t i = 0; i < n && st == KBC_OK; i++) {
    if (texts[i] == NULL) {
      st = kbc_err_set(err, KBC_ERR_INVALID, "texts[%zu] is NULL", i);
      break;
    }
    kbc_json *s = kbc_json_new_str(ra, texts[i]);
    if (s == NULL || kbc_failed(kbc_json_arr_push(ra, arr, s))) {
      st = kbc_err_set(err, KBC_ERR_NOMEM, "embed request texts[%zu]", i);
      break;
    }
  }
  if (st == KBC_OK) {
    kbc_json *op = kbc_json_new_str(ra, "embed");
    st = op == NULL ? kbc_err_set(err, KBC_ERR_NOMEM, "embed op json")
                    : kbc_json_obj_set(ra, obj, "op", op);
  }
  if (st == KBC_OK) {
    st = kbc_json_obj_set(ra, obj, "texts", arr);
  }
  if (st == KBC_OK) {
    st = kbc_json_dump(obj, &req, false, err);
  }
  if (st == KBC_OK) {
    st = kbc_str_putc(&req, '\n');
  }
  kbc_arena_free(ra);
  if (kbc_failed(st)) {
    kbc_str_free(&req);
    kbc_str_free(&line);
    return st;
  }

  /* --- one exchange under the mutex; nothing else touches the pipes ---- */
  (void)atomic_fetch_add(&e->requests, 1);
  (void)pthread_mutex_lock(&e->mu);
  st = exchange(e, &req, &line, KBC_EMBED_DEFAULT_TIMEOUT_MS, err);
  (void)pthread_mutex_unlock(&e->mu);
  kbc_str_free(&req);
  if (kbc_failed(st)) {
    /* A desynced stream is unusable: stop trusting this child. */
    atomic_store(&e->healthy, false);
    (void)atomic_fetch_add(&e->failures, 1);
    (void)pthread_mutex_lock(&e->mu);
    reap_kill(e);
    (void)pthread_mutex_unlock(&e->mu);
    kbc_str_free(&line);
    return st;
  }

  /* --- validate the reply shape before reading a single float --------- */
  float *vecs = NULL;
  size_t dim = 0;
  kbc_arena *pa = kbc_arena_new(4096);
  if (pa == NULL) {
    st = kbc_err_set(err, KBC_ERR_NOMEM, "arena for embed reply");
    goto done;
  }
  kbc_json *j = kbc_json_parse(pa, line.ptr, line.len, err);
  if (j == NULL) {
    st = KBC_ERR_PARSE;
    goto parsed;
  }
  if (!kbc_json_is(j, KBC_JSON_OBJ) || !kbc_json_bool(j, "ok", false)) {
    st = kbc_err_set(err, KBC_ERR_PARSE, "sidecar reply is not {\"ok\":true}");
    goto parsed;
  }
  const kbc_json *arr2 = kbc_json_get(j, "vectors");
  if (arr2 == NULL) {
    arr2 = kbc_json_get(j, "embeddings");
  }
  if (arr2 == NULL || !kbc_json_is(arr2, KBC_JSON_ARR)) {
    st = kbc_err_set(err, KBC_ERR_PARSE, "sidecar reply has no vectors array");
    goto parsed;
  }
  if (kbc_json_len(arr2) != n) {
    st = kbc_err_set(err, KBC_ERR_PARSE,
                     "sidecar returned %zu vectors for %zu texts",
                     kbc_json_len(arr2), n);
    goto parsed;
  }
  {
    bool bad = false;
    dim = reply_dim(j, &bad);
    if (bad) {
      st = kbc_err_set(err, KBC_ERR_PARSE, "sidecar reply dim is not a "
                                           "positive integer");
      goto parsed;
    }
    if (dim == 0) {
      dim = kbc_json_len(kbc_json_at(arr2, 0));
    }
    if (dim == 0 || dim > KBC_EMBED_MAX_DIM) {
      st = kbc_err_set(err, KBC_ERR_PARSE, "sidecar returned dim 0 or %zu > %u",
                       dim, KBC_EMBED_MAX_DIM);
      goto parsed;
    }
  }
  if (dim > SIZE_MAX / n / sizeof(float)) {
    st = kbc_err_set(err, KBC_ERR_NOMEM, "n=%zu dim=%zu overflows", n, dim);
    goto parsed;
  }
  vecs = kbc_arena_alloc(a, n * dim * sizeof(float));
  if (vecs == NULL) {
    st = kbc_err_set(err, KBC_ERR_NOMEM, "arena for %zu x %zu floats", n, dim);
    goto parsed;
  }
  for (size_t i = 0; i < n; i++) {
    const kbc_json *row = kbc_json_at(arr2, i);
    if (row == NULL || !kbc_json_is(row, KBC_JSON_ARR) ||
        kbc_json_len(row) != dim) {
      st = kbc_err_set(err, KBC_ERR_PARSE,
                       "sidecar vector %zu is not an array of %zu numbers", i,
                       dim);
      goto parsed;
    }
    for (size_t k = 0; k < dim; k++) {
      const kbc_json *num = kbc_json_at(row, k);
      if (num == NULL || !kbc_json_is(num, KBC_JSON_NUM)) {
        st = kbc_err_set(err, KBC_ERR_PARSE,
                         "sidecar vector %zu element %zu is not a number", i, k);
        goto parsed;
      }
      double f = num->u.num;
      if (!isfinite(f)) {
        st = kbc_err_set(err, KBC_ERR_PARSE,
                         "sidecar vector %zu element %zu is not finite", i, k);
        goto parsed;
      }
      vecs[i * dim + k] = (float)f;
    }
  }
  if (known == 0) {
    atomic_store(&e->dim, dim);
  }
  *out = vecs;
  st = KBC_OK;

parsed:
  kbc_arena_free(pa);
done:
  if (kbc_failed(st)) {
    *out = NULL;
    (void)atomic_fetch_add(&e->failures, 1);
    atomic_store(&e->healthy, false);
    /* A lying or malformed sidecar is killed, not left half-trusted. */
    (void)pthread_mutex_lock(&e->mu);
    reap_kill(e);
    (void)pthread_mutex_unlock(&e->mu);
  }
  kbc_str_free(&line);
  return st;
}

/* ---------------------------------------------------------- vector store - */

/* On-disk header, followed immediately by capacity*dim floats. 32 bytes keeps
 * the first float naturally aligned. */
#define KBC_VEC_MAGIC "KBCVEC\x01"
#define KBC_VEC_FORMAT 1u

typedef struct {
  char magic[8];
  uint32_t version;
  uint32_t dim;
  uint32_t capacity;
  uint32_t count;
  uint32_t reserved[2];
} vec_hdr;

_Static_assert(sizeof(vec_hdr) == 32, "vector store header must be 32 bytes");

struct kbc_vecstore {
  int fd;
  void *map;         /* vec_hdr followed by floats, MAP_SHARED */
  size_t map_len;    /* bytes currently mapped */
  size_t mapped_cap; /* capacity the mapping was sized for */
  size_t dim;
  uint32_t capacity;
  uint32_t count;
  char *path; /* KBC_OWN, NULL until the first save() */
  pthread_mutex_t mu;
};

static vec_hdr *vec_hdr_of(const kbc_vecstore *v) {
  return (vec_hdr *)v->map;
}

static float *vec_data(const kbc_vecstore *v) {
  return (float *)((unsigned char *)v->map + sizeof(vec_hdr));
}

/* Bytes needed for `cap` rows, with the multiply checked: dim and cap are
 * both attacker-influenced when they come off disk. */
static kbc_status vec_bytes(size_t dim, size_t cap, size_t *out, kbc_err *err) {
  if (dim == 0 || dim > KBC_EMBED_MAX_DIM) {
    return kbc_err_set(err, KBC_ERR_INVALID, "vector dim %zu is out of range",
                       dim);
  }
  if (cap > (SIZE_MAX - sizeof(vec_hdr)) / (dim * sizeof(float))) {
    return kbc_err_set(err, KBC_ERR_INVALID,
                       "capacity %zu at dim %zu overflows the address space",
                       cap, dim);
  }
  *out = sizeof(vec_hdr) + cap * dim * sizeof(float);
  return KBC_OK;
}

/* Creates the mapping for `newcap`: a NEW mapping, copy, swap, unmap. A grow
 * never moves bytes inside the live mapping, because callers may be holding a
 * const float * from kbc_vecstore_get() across a set(). */
static kbc_status vec_remap(kbc_vecstore *v, size_t newcap, kbc_err *err) {
  size_t need = 0;
  kbc_status st = vec_bytes(v->dim, newcap, &need, err);
  if (kbc_failed(st)) {
    return st;
  }
  if (need <= v->map_len && newcap <= v->mapped_cap) {
    v->mapped_cap = newcap;
    return KBC_OK;
  }
  if (ftruncate(v->fd, (off_t)need) != 0) {
    return kbc_err_set(err, KBC_ERR_IO, "ftruncate vector store to %zu: %s",
                       need, strerror(errno));
  }
  void *nm = mmap(NULL, need, PROT_READ | PROT_WRITE, MAP_SHARED, v->fd, 0);
  if (nm == MAP_FAILED) {
    return kbc_err_set(err, KBC_ERR_IO, "mmap vector store %zu bytes: %s",
                       need, strerror(errno));
  }
  size_t copy = v->map_len < need ? v->map_len : need;
  if (copy > 0) {
    memcpy(nm, v->map, copy);
  }
  if (v->map != NULL) {
    (void)munmap(v->map, v->map_len);
  }
  v->map = nm;
  v->map_len = need;
  v->mapped_cap = newcap;
  vec_hdr_of(v)->capacity = (uint32_t)newcap;
  return KBC_OK;
}

/* Doubles until doc_id fits, then remaps. Caller holds mu. */
static kbc_status vec_ensure_capacity(kbc_vecstore *v, uint32_t doc_id,
                                      kbc_err *err) {
  if (doc_id < v->capacity) {
    return KBC_OK;
  }
  size_t cap = v->capacity == 0 ? 16u : (size_t)v->capacity;
  while (cap <= (size_t)doc_id) {
    /* capacity and count are uint32_t on disk: stop before the cast wraps. */
    if (cap > UINT32_MAX / 2) {
      return kbc_err_set(err, KBC_ERR_INVALID,
                         "doc_id %u exceeds the largest representable store",
                         doc_id);
    }
    cap *= 2;
  }
  kbc_status st = vec_remap(v, cap, err);
  if (kbc_failed(st)) {
    return st;
  }
  v->capacity = (uint32_t)cap;
  return KBC_OK;
}

kbc_vecstore *kbc_vecstore_new(size_t dim, uint32_t capacity, kbc_err *err) {
  size_t need = 0;
  if (capacity == 0) {
    capacity = 1u;
  }
  kbc_status st = vec_bytes(dim, capacity, &need, err);
  if (kbc_failed(st)) {
    return NULL;
  }
  char tmpl[] = "/tmp/kbc-vecstore-XXXXXX";
  int fd = mkstemp(tmpl);
  if (fd < 0) {
    (void)kbc_err_set(err, KBC_ERR_IO, "mkstemp %s: %s", tmpl, strerror(errno));
    return NULL;
  }
  /* The backing file is a scratch file: it stays unlinked until save(). */
  (void)unlink(tmpl);
  kbc_vecstore *v = calloc(1, sizeof *v);
  if (v == NULL) {
    (void)close(fd);
    (void)kbc_err_set(err, KBC_ERR_NOMEM, "calloc kbc_vecstore");
    return NULL;
  }
  v->fd = fd;
  v->dim = dim;
  v->capacity = capacity;
  v->mapped_cap = capacity;
  v->count = 0;
  if (ftruncate(fd, (off_t)need) != 0) {
    (void)kbc_err_set(err, KBC_ERR_IO, "ftruncate vector store to %zu: %s",
                      need, strerror(errno));
    (void)close(fd);
    free(v);
    return NULL;
  }
  void *map = mmap(NULL, need, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
  if (map == MAP_FAILED) {
    (void)kbc_err_set(err, KBC_ERR_IO, "mmap vector store %zu bytes: %s", need,
                      strerror(errno));
    (void)close(fd);
    free(v);
    return NULL;
  }
  v->map = map;
  v->map_len = need;
  vec_hdr *h = vec_hdr_of(v);
  memset(h, 0, sizeof *h);
  memcpy(h->magic, KBC_VEC_MAGIC, sizeof h->magic);
  h->version = KBC_VEC_FORMAT;
  h->dim = (uint32_t)dim;
  h->capacity = capacity;
  h->count = 0;
  if (pthread_mutex_init(&v->mu, NULL) != 0) {
    (void)munmap(map, need);
    (void)close(fd);
    free(v);
    (void)kbc_err_set(err, KBC_ERR_INTERNAL, "pthread_mutex_init for vecstore");
    return NULL;
  }
  return v;
}

kbc_vecstore *kbc_vecstore_open(const char *path, kbc_err *err) {
  if (path == NULL) {
    (void)kbc_err_set(err, KBC_ERR_INVALID, "path must not be NULL");
    return NULL;
  }
  int fd = open(path, O_RDWR | O_CLOEXEC);
  if (fd < 0) {
    (void)kbc_err_set(err, KBC_ERR_IO, "open %s: %s", path, strerror(errno));
    return NULL;
  }
  struct stat sb;
  if (fstat(fd, &sb) != 0) {
    int saved = errno;
    (void)close(fd);
    (void)kbc_err_set(err, KBC_ERR_IO, "stat %s: %s", path, strerror(saved));
    return NULL;
  }
  if (sb.st_size < (off_t)sizeof(vec_hdr)) {
    (void)close(fd);
    (void)kbc_err_set(err, KBC_ERR_PARSE,
                      "%s: %lld bytes is shorter than the %zu byte header",
                      path, (long long)sb.st_size, sizeof(vec_hdr));
    return NULL;
  }
  kbc_vecstore *v = calloc(1, sizeof *v);
  if (v == NULL) {
    (void)close(fd);
    (void)kbc_err_set(err, KBC_ERR_NOMEM, "calloc kbc_vecstore");
    return NULL;
  }
  v->fd = fd;
  v->map_len = (size_t)sb.st_size;
  void *map = mmap(NULL, v->map_len, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
  if (map == MAP_FAILED) {
    (void)kbc_err_set(err, KBC_ERR_IO, "mmap %s: %s", path, strerror(errno));
    (void)close(fd);
    free(v);
    return NULL;
  }
  v->map = map;
  const vec_hdr *h = vec_hdr_of(v);
  if (memcmp(h->magic, KBC_VEC_MAGIC, sizeof h->magic) != 0) {
    (void)kbc_err_set(err, KBC_ERR_PARSE, "%s: bad magic", path);
    goto fail;
  }
  if (h->version != KBC_VEC_FORMAT) {
    (void)kbc_err_set(err, KBC_ERR_PARSE, "%s: format version %u, expected %u",
                      path, h->version, KBC_VEC_FORMAT);
    goto fail;
  }
  size_t need = 0;
  if (kbc_failed(vec_bytes(h->dim, h->capacity, &need, err))) {
    (void)kbc_err_set(err, KBC_ERR_PARSE, "%s: dim %u capacity %u is invalid",
                      path, h->dim, h->capacity);
    goto fail;
  }
  if (h->capacity == 0 || h->count > h->capacity) {
    (void)kbc_err_set(err, KBC_ERR_PARSE, "%s: count %u exceeds capacity %u",
                      path, h->count, h->capacity);
    goto fail;
  }
  if (need > v->map_len) {
    (void)kbc_err_set(err, KBC_ERR_PARSE,
                      "%s: %zu bytes for %u x %u floats, file holds %zu", path,
                      need, h->capacity, h->dim, v->map_len);
    goto fail;
  }
  v->dim = h->dim;
  v->capacity = h->capacity;
  v->mapped_cap = h->capacity;
  v->count = h->count;
  v->path = strdup(path);
  if (v->path == NULL) {
    (void)kbc_err_set(err, KBC_ERR_NOMEM, "strdup %s", path);
    goto fail;
  }
  if (pthread_mutex_init(&v->mu, NULL) != 0) {
    (void)kbc_err_set(err, KBC_ERR_INTERNAL, "pthread_mutex_init for vecstore");
    goto fail;
  }
  return v;

fail:
  (void)munmap(v->map, v->map_len);
  (void)close(v->fd);
  free(v->path);
  free(v);
  return NULL;
}

void kbc_vecstore_free(kbc_vecstore *v) {
  if (v == NULL) {
    return;
  }
  (void)pthread_mutex_lock(&v->mu);
  if (v->map != NULL) {
    (void)msync(v->map, v->map_len, MS_SYNC);
    (void)munmap(v->map, v->map_len);
    v->map = NULL;
  }
  if (v->fd >= 0) {
    (void)close(v->fd);
    v->fd = -1;
  }
  free(v->path);
  v->path = NULL;
  (void)pthread_mutex_unlock(&v->mu);
  (void)pthread_mutex_destroy(&v->mu);
  free(v);
}

size_t kbc_vecstore_dim(const kbc_vecstore *v) {
  return v == NULL ? 0 : v->dim;
}

uint32_t kbc_vecstore_count(const kbc_vecstore *v) {
  return v == NULL ? 0 : v->count;
}

const float *kbc_vecstore_get(const kbc_vecstore *v, uint32_t doc_id) {
  if (v == NULL || v->map == NULL || doc_id >= v->count) {
    return NULL;
  }
  return vec_data(v) + (size_t)doc_id * v->dim;
}

kbc_status kbc_vecstore_set(kbc_vecstore *v, uint32_t doc_id, const float *vec,
                            kbc_err *err) {
  if (v == NULL || vec == NULL) {
    return kbc_err_set(err, KBC_ERR_INVALID,
                       "store and vector must not be NULL");
  }
  (void)pthread_mutex_lock(&v->mu);
  kbc_status st = vec_ensure_capacity(v, doc_id, err);
  if (kbc_failed(st)) {
    (void)pthread_mutex_unlock(&v->mu);
    return st;
  }
  memcpy(vec_data(v) + (size_t)doc_id * v->dim, vec, v->dim * sizeof(float));
  if (doc_id >= v->count) {
    v->count = doc_id + 1u;
    vec_hdr_of(v)->count = v->count;
  }
  (void)pthread_mutex_unlock(&v->mu);
  return KBC_OK;
}

/* Copies the whole mapping to `path`: create/truncate, full-write loop with
 * EINTR and short writes, fsync, close. */
static kbc_status vec_write_file(const char *path, const void *data, size_t n,
                                 kbc_err *err) {
  int out = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
  if (out < 0) {
    return kbc_err_set(err, KBC_ERR_IO, "open %s: %s", path, strerror(errno));
  }
  const char *p = (const char *)data;
  size_t left = n;
  while (left > 0) {
    ssize_t w = write(out, p, left);
    if (w > 0) {
      p += (size_t)w;
      left -= (size_t)w;
      continue;
    }
    if (w < 0 && errno == EINTR) {
      continue;
    }
    int saved = errno;
    (void)close(out);
    return kbc_err_set(err, KBC_ERR_IO, "write %s: %s", path, strerror(saved));
  }
  if (fsync(out) != 0) {
    int saved = errno;
    (void)close(out);
    return kbc_err_set(err, KBC_ERR_IO, "fsync %s: %s", path, strerror(saved));
  }
  if (close(out) != 0) {
    return kbc_err_set(err, KBC_ERR_IO, "close %s: %s", path, strerror(errno));
  }
  return KBC_OK;
}

kbc_status kbc_vecstore_save(kbc_vecstore *v, const char *path, kbc_err *err) {
  if (v == NULL || path == NULL) {
    return kbc_err_set(err, KBC_ERR_INVALID, "store and path must not be NULL");
  }
  (void)pthread_mutex_lock(&v->mu);
  /* A grow past the current mapping is ftruncate + remap, so the file on disk
   * always covers every row the header claims. */
  kbc_status st = vec_remap(v, v->capacity, err);
  if (kbc_failed(st)) {
    (void)pthread_mutex_unlock(&v->mu);
    return st;
  }
  if (msync(v->map, v->map_len, MS_SYNC) != 0) {
    st = kbc_err_set(err, KBC_ERR_IO, "msync vector store: %s",
                     strerror(errno));
    (void)pthread_mutex_unlock(&v->mu);
    return st;
  }
  if (v->path != NULL && strcmp(v->path, path) == 0) {
    /* Saving in place: the mapping is already the file. */
    if (fsync(v->fd) != 0) {
      st = kbc_err_set(err, KBC_ERR_IO, "fsync %s: %s", path, strerror(errno));
      (void)pthread_mutex_unlock(&v->mu);
      return st;
    }
    (void)pthread_mutex_unlock(&v->mu);
    return KBC_OK;
  }
  if (v->path == NULL) {
    /* First save of a scratch store: copy the whole mapping out and adopt the
     * path, so later saves of the same path are in place. */
    st = vec_write_file(path, v->map, v->map_len, err);
    if (kbc_failed(st)) {
      (void)pthread_mutex_unlock(&v->mu);
      return st;
    }
    char *copy = strdup(path);
    if (copy == NULL) {
      st = kbc_err_set(err, KBC_ERR_NOMEM, "strdup %s", path);
      (void)pthread_mutex_unlock(&v->mu);
      return st;
    }
    free(v->path);
    v->path = copy;
    (void)pthread_mutex_unlock(&v->mu);
    return KBC_OK;
  }
  /* A different path: same contents, copied out. The store keeps pointing at
   * its original backing file. */
  st = vec_write_file(path, v->map, v->map_len, err);
  (void)pthread_mutex_unlock(&v->mu);
  return st;
}

kbc_status kbc_vecstore_gather(const kbc_vecstore *v, const uint32_t *ids,
                               size_t n, kbc_arena *a, float *out,
                               kbc_err *err) {
  if (v == NULL || a == NULL || out == NULL) {
    return kbc_err_set(err, KBC_ERR_INVALID,
                       "store, arena and out must not be NULL");
  }
  if (n > 0 && ids == NULL) {
    return kbc_err_set(err, KBC_ERR_INVALID, "ids must not be NULL for n=%zu",
                       n);
  }
  for (size_t i = 0; i < n; i++) {
    if (ids[i] >= v->count) {
      return kbc_err_set(err, KBC_ERR_INVALID,
                         "ids[%zu] = %u is not below count %u", i, ids[i],
                         v->count);
    }
  }
  const float *data = vec_data(v);
  for (size_t i = 0; i < n; i++) {
    memcpy(out + i * v->dim, data + (size_t)ids[i] * v->dim,
           v->dim * sizeof(float));
  }
  return KBC_OK;
}
