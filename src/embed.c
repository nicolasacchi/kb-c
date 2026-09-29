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

/* The health handshake waits for the sidecar to load its model, which is
 * seconds warm and ~20s cold (bge-small-en-v1.5 measured), so it gets its own
 * budget rather than the per-request one. */
#define KBC_EMBED_HANDSHAKE_TIMEOUT_MS 120000
/* A production sidecar pushes one unsolicited {"kind":"ready"} as soon as the
 * model is loaded, before it has read anything from us. If the first line we
 * read back is that handshake, exactly one reply to our own probe is still in
 * flight; the drain below bounds how long we wait for it. */
#define KBC_EMBED_DRAIN_TIMEOUT_MS 2000
#define KBC_EMBED_READ_CHUNK 8192u
#define KBC_EMBED_MAX_DIM 65536u
/* The model name the sidecar announces, and the ceiling on it. The name is
 * half the query cache key (embed_cache.rs:66-70), so it is captured from the
 * handshake rather than reconstructed from argv. Over-long is a protocol
 * error, not a truncation: a key built from a clipped name would collide
 * with every other clipped name, which is the silent-wrong-vector failure the
 * model is in the key to prevent. */
#define KBC_EMBED_MODEL_MAX 128u

/* In-memory query-cache capacity. kb's DEFAULT_CAPACITY is 1024 for the same
 * reason it is here: a 1024-dim vector is ~4 KB, so a full cache is ~4 MB
 * resident, and the linear scan it replaces is noise next to the ~50-100 ms
 * sidecar round trip the cache exists to avoid. */
#define KBC_QUERY_CACHE_DEFAULT_CAPACITY 1024u

/* Backstop on one key's query bytes. Nothing inside kb-c can reach it: the
 * search layer clamps a query to KBC_MAX_QUERY_LEN (4096) before it gets
 * here. It exists because the cache copies the key into a slot, and an
 * unbounded copy is what rule 7 forbids. */
#define KBC_QUERY_CACHE_KEY_MAX (64u * 1024u)

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
  _Atomic uint64_t req_id; /* next request id; the sidecar echoes it back */
  /* The sidecar's announced model, "" when it announced none. Guarded by its
   * own mutex, NOT by `mu`: a cache hit must not queue behind an in-flight
   * document embed to learn which model it is keyed on. */
  pthread_mutex_t mmu;
  char model[KBC_EMBED_MODEL_MAX + 1];
  /* Set once a handshake has completed, whatever it announced. It separates
   * "the sidecar named no model" (a legitimate key, the empty string) from
   * "we have not asked yet" (KBC_ERR_NOTFOUND) — a caller must be able to
   * tell those apart, or it will cache vectors under a key it never chose. */
  _Atomic bool handshaked;
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
  /* The child inherits this process's UNFLUSHED stdio buffers. The protocol
   * pipe is about to become its fd 1, so if anything in the child flushes —
   * a libc exit path, a sanitizer runtime, a library warning — every pending
   * byte the daemon had buffered is injected into the sidecar's reply stream
   * and the protocol desynchronises on text that has nothing to do with the
   * sidecar. It is not hypothetical: the test suite's own `== embed ==` banner
   * reached kbc_embedder_embed as the reply of a sidecar that never started.
   * POSIX requires the flush here; it costs nothing on the spawn path. */
  fflush(NULL);
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

/* Writes one pre-serialized request (req may be NULL to only read) and reads
 * exactly one reply line. The handshake needs this raw form: the line it reads
 * may be the sidecar's unsolicited ready, which the caller has to recognise
 * itself. Caller holds mu. */
static kbc_status exchange_raw(kbc_embedder *e, const kbc_str *req,
                               kbc_str *line, int timeout_ms, kbc_err *err) {
  kbc_str_clear(line);
  if (e->out_fd < 0 || e->in_fd < 0 || e->pid <= 0) {
    return kbc_err_set(err, KBC_ERR_IO, "sidecar is not running");
  }
  if (req != NULL) {
    kbc_status st = write_all(e->in_fd, req->ptr, req->len, err);
    if (kbc_failed(st)) {
      return st;
    }
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

/* Forward declarations: the envelope helpers live below, with the rest of the
 * wire-format code. */
static const char *reply_kind(const kbc_json *j);
static kbc_status sidecar_error(const char *msg, kbc_err *err);
static size_t reply_dim(const kbc_json *j, bool *bad);
static kbc_status note_model(kbc_embedder *e, const kbc_json *j, kbc_err *err);


/* Request ids are ours to allocate; the sidecar only echoes them, so all this
 * has to guarantee is that two in-flight requests never share one. */
static uint64_t next_req_id(kbc_embedder *e) {
  return (uint64_t)atomic_fetch_add(&e->req_id, 1) + 1u;
}

/* Reads reply lines, absorbing the sidecar's unsolicited ready handshake, and
 * stops at the first line that answers `sent_id`. A ready line is the only
 * thing a sidecar may push at us unasked, so skipping it is safe; a line whose
 * req_id is somebody else's means the stream is out of step and the caller
 * must treat the child as dead. Caller holds mu. */
static kbc_status exchange(kbc_embedder *e, const kbc_str *req, kbc_str *line,
                           int timeout_ms, uint64_t sent_id, kbc_err *err) {
  kbc_status st = exchange_raw(e, req, line, timeout_ms, err);
  if (kbc_failed(st)) {
    return st;
  }
  for (int guard = 0; guard < 3; guard++) {
    kbc_arena *a = kbc_arena_new(1024);
    if (a == NULL) {
      return kbc_err_set(err, KBC_ERR_NOMEM, "arena for a reply envelope");
    }
    kbc_err scratch;
    kbc_err_reset(&scratch);
    kbc_json *j = kbc_json_parse(a, line->ptr, line->len, &scratch);
    const char *kind = reply_kind(j);
    bool ready = kind != NULL && strcmp(kind, "ready") == 0;
    /* Copy the numbers out before the arena goes: they are reported in an
     * error message, and the arena owns the JSON they point into. */
    uint64_t got_id = sent_id;
    bool stale = false;
    if (!ready && kind != NULL) {
      const kbc_json *rid = kbc_json_get(j, "req_id");
      if (kbc_json_is(rid, KBC_JSON_NUM)) {
        got_id = (uint64_t)rid->u.num;
        stale = got_id != sent_id;
      }
    }
    /* The ready line is the handshake; take its dimension and keep reading. */
    bool bad = false;
    size_t d = ready ? reply_dim(j, &bad) : 0;
    /* The model arrives on the same ready line. Taken before the arena dies;
     * a name we cannot hold is a protocol error, and the stream is already
     * untrustworthy at that point. */
    st = ready ? note_model(e, j, err) : KBC_OK;
    kbc_arena_free(a);
    if (kbc_failed(st)) {
      return st;
    }
    if (stale) {
      return kbc_err_set(err, KBC_ERR_PARSE,
                         "sidecar %s: reply to request %llu, expected %llu",
                         e->args[0], (unsigned long long)got_id,
                         (unsigned long long)sent_id);
    }
    if (!ready) {
      return KBC_OK;
    }
    if (!bad && d > 0) {
      atomic_store(&e->dim, d);
    }
    /* A ready absorbed mid-request IS a handshake: the sidecar told us what
     * it loaded, so the model is now a known fact even though no explicit
     * health exchange ran. */
    atomic_store(&e->handshaked, true);
    /* Read the line that actually answers the request. */
    kbc_str tmp;
    kbc_str_init(&tmp);
    st = exchange_raw(e, NULL, &tmp, timeout_ms, err);
    if (kbc_failed(st)) {
      kbc_str_free(&tmp);
      return st;
    }
    kbc_str old = *line; /* swap rather than copy a multi-KB reply */
    *line = tmp;
    kbc_str_free(&old);
  }
  return kbc_err_set(err, KBC_ERR_PARSE,
                     "sidecar %s: %d unsolicited handshakes in a row",
                     e->args[0], 3);
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
  if (pthread_mutex_init(&e->mmu, NULL) != 0) {
    for (size_t i = 0; i < argc; i++) {
      free(e->args[i]);
    }
    free(e->args);
    (void)pthread_mutex_destroy(&e->mu);
    free(e);
    (void)kbc_err_set(err, KBC_ERR_INTERNAL,
                      "pthread_mutex_init for the model name");
    return NULL;
  }
  kbc_status st = spawn(e, err);
  if (kbc_failed(st)) {
    for (size_t i = 0; i < argc; i++) {
      free(e->args[i]);
    }
    free(e->args);
    (void)pthread_mutex_destroy(&e->mu);
    (void)pthread_mutex_destroy(&e->mmu);
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
  (void)pthread_mutex_destroy(&e->mmu);
  free(e);
}

bool kbc_embedder_healthy(const kbc_embedder *e) {
  return e != NULL && atomic_load(&e->healthy);
}

size_t kbc_embedder_dim(const kbc_embedder *e) {
  return e == NULL ? 0 : atomic_load(&e->dim);
}

/* The model the sidecar ACTUALLY loaded, copied into the caller's buffer.
 *
 * Copying rather than returning a pointer into `e` is what lets the caller
 * hold the name across a concurrent handshake without re-reading a buffer
 * that handshake is rewriting.
 *
 * Every failure leaves `out` EMPTY rather than partially filled. A half
 * copied name is the worst possible result here: it is a valid C string that
 * is not the model's name, and it would key a cache entry under it. */
kbc_status kbc_embedder_model(kbc_embedder *e, char *out, size_t cap,
                              kbc_err *err) {
  if (out == NULL || cap == 0) {
    return kbc_err_set(err, KBC_ERR_INVALID,
                       "out buffer and capacity must be non-zero");
  }
  out[0] = '\0';
  if (e == NULL) {
    return kbc_err_set(err, KBC_ERR_INVALID, "embedder must not be NULL");
  }
  if (!atomic_load(&e->handshaked)) {
    /* Distinct from a sidecar that handshook and named nothing: that one
     * hands back "" and is cacheable, this one we simply have not asked. */
    return kbc_err_set(err, KBC_ERR_NOTFOUND,
                       "sidecar has not completed a handshake, so its model "
                       "is not known yet");
  }
  (void)pthread_mutex_lock(&e->mmu);
  size_t n = strlen(e->model);
  if (n > KBC_EMBED_MODEL_MAX) {
    /* Unreachable while note_model refuses an over-long name at the wire, but
     * a truncation here would still be a silent key collision, so it is
     * reported rather than clipped. */
    (void)pthread_mutex_unlock(&e->mmu);
    out[0] = '\0';
    return kbc_err_set(err, KBC_ERR_PARSE,
                       "recorded model name is %zu bytes, over the %u limit",
                       n, KBC_EMBED_MODEL_MAX);
  }
  if (n >= cap) {
    (void)pthread_mutex_unlock(&e->mmu);
    out[0] = '\0';
    return kbc_err_set(err, KBC_ERR_INVALID,
                       "model name needs %zu bytes, buffer holds %zu", n + 1,
                       cap);
  }
  memcpy(out, e->model, n + 1);
  (void)pthread_mutex_unlock(&e->mmu);
  return KBC_OK;
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
/* Pulls a non-negative integral dim out of a reply, or 0 when absent. */
static size_t reply_dim(const kbc_json *j, bool *bad) {
  *bad = false;
  if (!kbc_json_is(j, KBC_JSON_OBJ)) {
    return 0;
  }
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

/* Records the model the sidecar says it is serving. This is the `model`
 * half of the query cache key, taken from the wire rather than from argv
 * because argv is the operator's REQUEST and the wire is what the model
 * actually loaded — the case where they disagree is exactly the case where a
 * wrong cache key is silently wrong.
 *
 * A missing model is not an error: a sidecar that names nothing still gets a
 * cache, keyed on the empty name. Refusing to cache would trade a slow query
 * for a search lane that stops working.
 *
 * An over-long name IS an error. Truncating it would make two different
 * models share a key prefix, which is the collision the model name in the key
 * exists to prevent. */
static kbc_status note_model(kbc_embedder *e, const kbc_json *j,
                             kbc_err *err) {
  const char *m = kbc_json_str(j, "model", NULL);
  if (m == NULL) {
    return KBC_OK;
  }
  size_t n = strlen(m);
  if (n > KBC_EMBED_MODEL_MAX) {
    return kbc_err_set(err, KBC_ERR_PARSE,
                       "sidecar model name is %zu bytes, over the %u limit", n,
                       KBC_EMBED_MODEL_MAX);
  }
  (void)pthread_mutex_lock(&e->mmu);
  memcpy(e->model, m, n + 1);
  (void)pthread_mutex_unlock(&e->mmu);
  return KBC_OK;
}

/* The production sidecar tags every envelope with "kind" (serde's internally
 * tagged enum): "ready" is pushed unsolicited once the model is loaded,
 * "embed_ok" carries the vectors, "error" carries a message. The older
 * {"ok":true,...} shape is still recognised, so a sidecar written against the
 * previous wire description keeps working; WHICH replies are accepted is
 * unchanged, only how one is recognised. */
static const char *reply_kind(const kbc_json *j) {
  if (!kbc_json_is(j, KBC_JSON_OBJ)) {
    return NULL;
  }
  const kbc_json *k = kbc_json_get(j, "kind");
  return kbc_json_is(k, KBC_JSON_STR) ? k->u.str.ptr : NULL;
}

/* A sidecar-reported error is a failed request, not a dead pipe. The status
 * follows the message: a request it could not parse is a protocol error,
 * anything else is a runtime failure inside the child. The message is passed
 * through verbatim — it names what the model rejected, and hiding it is how a
 * broken lane looks like an empty result set. */
static kbc_status sidecar_error(const char *msg, kbc_err *err) {
  if (msg != NULL && strncmp(msg, "parse:", 6) == 0) {
    return kbc_err_set(err, KBC_ERR_PARSE, "sidecar: %s", msg);
  }
  return kbc_err_set(err, KBC_ERR_IO, "sidecar: %s",
                     msg != NULL ? msg : "unspecified error");
}

/* Health handshake.
 *
 * The production kb-embedder has no health request: it loads the model, then
 * pushes {"kind":"ready","model":...,"dim":N} unprompted and only afterwards
 * reads stdin. So a probe is still written (a sidecar that answers one, and
 * the older {"ok":true} shape, must not be treated as dead), and the reply
 * that comes back is the ready line we were already going to receive. Exactly
 * one line is then still owed to us — the sidecar's answer to the probe — and
 * it is drained, or the next embed request would read it as its own reply. */
static kbc_status handshake(kbc_embedder *e, kbc_err *err) {
  kbc_str req;
  kbc_str_init(&req);
  kbc_str line;
  kbc_str_init(&line);
  kbc_status st;
  const uint64_t id = next_req_id(e);

  (void)pthread_mutex_lock(&e->mu);
  st = kbc_str_printf(&req, "{\"kind\":\"health\",\"req_id\":%llu}\n",
                      (unsigned long long)id);
  if (kbc_failed(st)) {
    (void)kbc_err_set(err, KBC_ERR_NOMEM, "health request buffer");
    (void)pthread_mutex_unlock(&e->mu);
    kbc_str_free(&req);
    return st;
  }
  st = exchange_raw(e, &req, &line, KBC_EMBED_HANDSHAKE_TIMEOUT_MS, err);
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
  const char *kind = reply_kind(j);
  bool ready = kind != NULL && strcmp(kind, "ready") == 0;
  if (j == NULL || !kbc_json_is(j, KBC_JSON_OBJ)) {
    kbc_err_set(err, KBC_ERR_PARSE, "sidecar handshake reply is not an object");
    st = KBC_ERR_PARSE;
  } else if (kind != NULL && strcmp(kind, "error") == 0) {
    st = sidecar_error(kbc_json_str(j, "msg", NULL), err);
  } else if (ready) {
    bool bad = false;
    size_t d = reply_dim(j, &bad);
    if (bad || d == 0) {
      kbc_err_set(err, KBC_ERR_PARSE,
                  "sidecar ready dim is missing or out of range");
      st = KBC_ERR_PARSE;
    } else {
      atomic_store(&e->dim, d);
    }
  } else if (kind != NULL) {
    kbc_err_set(err, KBC_ERR_PARSE, "sidecar handshake kind is \"%s\"", kind);
    st = KBC_ERR_PARSE;
  } else if (!kbc_json_bool(j, "ok", false)) {
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
  if (st == KBC_OK) {
    /* Both handshake shapes carry the model: {"kind":"ready",...} from the
     * production sidecar and the older {"ok":true,...}. It is read here
     * rather than in either branch so a future shape cannot forget it. */
    st = note_model(e, j, err);
  }
  kbc_arena_free(a);
  kbc_str_free(&req);

  if (ready) {
    /* Our probe's answer is still queued. It carries nothing we need, but it
     * must not be left for the next request to read. */
    kbc_str drain;
    kbc_str_init(&drain);
    (void)pthread_mutex_lock(&e->mu);
    if (e->out_fd >= 0) {
      kbc_err scratch;
      kbc_err_reset(&scratch);
      (void)read_line(e->out_fd, &drain, KBC_EMBED_DRAIN_TIMEOUT_MS);
    }
    (void)pthread_mutex_unlock(&e->mu);
    kbc_str_free(&drain);
  }
  kbc_str_free(&line);
  /* Set only on success. A restart that leaves the embedder without a
   * handshake must report NOTFOUND for the model rather than serving the
   * PREVIOUS sidecar's name: the child is gone, and its name is not a fact
   * about whatever loads next. */
  atomic_store(&e->handshaked, st == KBC_OK);
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


/* Appends one element of the "texts" array, quoted and escaped so the line is
 * valid UTF-8 JSON whatever bytes the document held. A byte >= 0x80 is not
 * passed through: the sidecar reads stdin as UTF-8 and exits on a line that
 * is not, taking the whole vector lane with it. */
static kbc_status append_text_literal(kbc_str *out, const char *s, size_t i,
                                      kbc_err *err) {
  static const char kHex[] = "0123456789abcdef";
  kbc_status st = kbc_str_putc(out, '"');
  for (const unsigned char *p = (const unsigned char *)s; st == KBC_OK && *p;
       p++) {
    unsigned char c = *p;
    switch (c) {
    case '"':
      st = kbc_str_puts(out, "\\\"");
      break;
    case '\\':
      st = kbc_str_puts(out, "\\\\");
      break;
    case '\b':
      st = kbc_str_puts(out, "\\b");
      break;
    case '\f':
      st = kbc_str_puts(out, "\\f");
      break;
    case '\n':
      st = kbc_str_puts(out, "\\n");
      break;
    case '\r':
      st = kbc_str_puts(out, "\\r");
      break;
    case '\t':
      st = kbc_str_puts(out, "\\t");
      break;
    default:
      if (c < 0x20 || c >= 0x80) {
        char esc[6] = {'\\', 'u', '0', '0', kHex[c >> 4], kHex[c & 0x0f]};
        st = kbc_str_append(out, esc, sizeof esc);
      } else {
        st = kbc_str_append(out, (const char *)&c, 1);
      }
      break;
    }
    if (kbc_failed(st)) {
      (void)kbc_err_set(err, st, "embed request texts[%zu] byte %zu", i,
                        (size_t)(p - (const unsigned char *)s));
      return st;
    }
  }
  if (kbc_failed(st)) {
    return st;
  }
  return kbc_str_putc(out, '"');
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

  /* --- request: assembled here, not through kbc_json -------------------- */
  /* Two reasons, both measured against the real sidecar:
   *   1. Its request enum is internally tagged ("kind") and requires a
   *      req_id, and it rejects the older {"op":...} shape outright.
   *   2. It parses stdin as UTF-8 and EXITS on a line that is not valid
   *      UTF-8 — "Error: stream did not contain valid UTF-8", observed on the
   *      404th document of the benchmark corpus, which carries raw 8-bit
   *      bytes. kbc_json_escape passes bytes >= 0x80 through verbatim, which
   *      is right for kb-c's own HTTP responses and fatal here, so the text
   *      is escaped on this side of the wire instead: every byte outside
   *      printable ASCII becomes \u00XX. A mis-decoded byte becomes a
   *      different token in the model rather than a dead lane. */
  kbc_str req;
  kbc_str_init(&req);
  kbc_str line;
  kbc_str_init(&line);
  kbc_status st = KBC_OK;
  const uint64_t req_id = next_req_id(e);

  st = kbc_str_printf(&req, "{\"kind\":\"embed\",\"req_id\":%llu,\"texts\":[",
                      (unsigned long long)req_id);
  for (size_t i = 0; i < n && st == KBC_OK; i++) {
    if (texts[i] == NULL) {
      st = kbc_err_set(err, KBC_ERR_INVALID, "texts[%zu] is NULL", i);
      break;
    }
    st = append_text_literal(&req, texts[i], i, err);
    if (st == KBC_OK && i + 1 < n) {
      st = kbc_str_putc(&req, ',');
    }
  }
  if (st == KBC_OK) {
    st = kbc_str_puts(&req, "]}\n");
  }
  if (kbc_failed(st)) {
    kbc_str_free(&req);
    kbc_str_free(&line);
    return st;
  }

  /* --- one exchange under the mutex; nothing else touches the pipes ---- */
  (void)atomic_fetch_add(&e->requests, 1);
  (void)pthread_mutex_lock(&e->mu);
  st = exchange(e, &req, &line, KBC_EMBED_DEFAULT_TIMEOUT_MS, req_id, err);
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
  /* "embed_ok" is the production success envelope; {"ok":true} is the older
   * one. Anything else — including an error the sidecar reported about THIS
   * request — is a failure, with its message intact. */
  const char *kind = reply_kind(j);
  if (kind != NULL && strcmp(kind, "error") == 0) {
    st = sidecar_error(kbc_json_str(j, "msg", NULL), err);
    goto parsed;
  }
  if (!kbc_json_is(j, KBC_JSON_OBJ)) {
    st = kbc_err_set(err, KBC_ERR_PARSE, "sidecar reply is not an object");
    goto parsed;
  }
  if (kind != NULL) {
    if (strcmp(kind, "embed_ok") != 0) {
      st = kbc_err_set(err, KBC_ERR_PARSE,
                       "sidecar replied kind \"%s\" to an embed request", kind);
      goto parsed;
    }
  } else if (!kbc_json_bool(j, "ok", false)) {
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

/* ----------------------------------------------------- query cache ------- */

/* The query-embedding LRU, ported from kb-server/src/embed_cache.rs.
 *
 * The whole cache exists because `kbc_embedder_embed` is a ~50-100 ms
 * round trip through the sidecar's stdio, paid again on every keystroke and
 * again in every corpus that shares a model.
 *
 * Four decisions are load-bearing and all four come from the original. They
 * are collected here because each one looks like an improvement waiting to
 * happen, and each improvement is a bug:
 *
 *  1. The key is (model, query) and NOTHING ELSE. No corpus, no mode, no
 *     limit, no top-k, no dim, no embedder identity. The original says so
 *     twice and means it (embed_cache.rs:9-12, :320-322 — "the cache key is
 *     (model, query) — not a corpus"). A query's embedding does not depend on
 *     which corpus asked for it, so a corpus in the key is pure duplication.
 *  2. The key is the RAW query string. No trim, no lowercase, no whitespace
 *     collapse, no hash of a normalised form. The embedder's own 32 KiB
 *     input cap (kb-core/src/embed.rs:59, applied at :395) lives INSIDE the
 *     embedder, downstream of this key, so two queries differing only past
 *     byte 32768 do land on the same vector by the first-to-populate. We
 *     reproduce that: the cap is not applied here, so we do not accidentally
 *     give those two queries different entries.
 *  3. The model NAME, never the dim. Two distinct models can share a dim
 *     (bge-base-en-v1.5 and jina-embeddings-v2-base-code are both 768), and
 *     serving one's vector under the other's key is silently wrong with no
 *     dim error to catch it (embed_cache.rs:154-158).
 *  4. There is NO TTL. No timestamp on the key, no expiry, no sweeper. An
 *     entry lives until eviction or process exit. A model's output for a
 *     fixed input is deterministic, so an expiry would buy nothing and cost
 *     a timestamp in every comparison.
 *
 * Lookup is a LINEAR SCAN with tuple equality, not a hash, and that is
 * deliberate: capacity is 1024, so a 1024-entry scan is noise next to the
 * ~100 ms embed it replaces, and a hash table would be more code for no
 * measurable win (embed_cache.rs:9-12, :105). The scan is why the model is
 * compared first — it is a short string and it discriminates immediately.
 *
 * SCOPE. In the original this is process-wide, held in an Arc and shared by
 * every kb. kb-c has no global mutable state (rule 5), so the store is an
 * ordinary owned object the caller creates once and passes to every query.
 * One instance shared by the daemon is the same thing without the global;
 * the key carrying the model is what makes that safe for two embedders
 * running different models in one process, which is also why it is in the
 * key. A caller who wants per-embedder isolation passes a separate cache.
 *
 * PERSISTENCE. The original saves the top PERSIST_CAP=256 entries to
 * <state>/query-embed-cache.json and reloads them, so hot queries survive a
 * restart. kb-c does NOT persist, and the reason is that a cold start is
 * cheap here in a way it was not there: the file is a state-directory
 * contract (kb-core/src/paths.rs:221) that kb-c has not frozen a path for,
 * and the win is only "hot queries stay warm across a restart", which is
 * strictly less than the ~100 ms it costs to re-embed. Persistence is
 * deferred until kb-c has a state directory to name, not forgotten.
 *
 * CONCURRENCY. One mutex covers the whole scan-and-mutate, as in the
 * original's `Mutex<VecDeque<..>>`. The lock is held across the whole
 * scan-and-touch, so it is the only contention point in the query path; its
 * cost is measured in the port report rather than assumed negligible. The
 * sidecar exchange is NOT under it: a hit must never queue behind a document
 * embed, and a miss must not hold this lock across a ~100 ms round trip. */

typedef struct qc_entry {
  struct qc_entry *prev, *next; /* LRU order; head = most recently used */
  char *model;                   /* KBC_OWN, interned per entry */
  char *query;                   /* KBC_OWN, the raw query bytes */
  size_t query_len;
  float *vec;                    /* KBC_OWN, dim floats */
  size_t dim;
  size_t model_len;
} qc_entry;

struct kbc_query_cache {
  qc_entry *head, *tail; /* head = MRU, tail = next to evict */
  size_t len, capacity;
  pthread_mutex_t mu;
};

/* Tuple equality: model name AND raw query bytes. Nothing else compares. */
static bool qc_key_eq(const qc_entry *e, const char *model, size_t model_len,
                      const char *query, size_t query_len) {
  return e->model_len == model_len && e->query_len == query_len &&
         memcmp(e->model, model, model_len) == 0 &&
         memcmp(e->query, query, query_len) == 0;
}

static void qc_unlink(kbc_query_cache *c, qc_entry *e) {
  if (e->prev != NULL) {
    e->prev->next = e->next;
  } else {
    c->head = e->next;
  }
  if (e->next != NULL) {
    e->next->prev = e->prev;
  } else {
    c->tail = e->prev;
  }
  e->prev = e->next = NULL;
  c->len--;
}

static void qc_push_front(kbc_query_cache *c, qc_entry *e) {
  e->prev = NULL;
  e->next = c->head;
  if (c->head != NULL) {
    c->head->prev = e;
  } else {
    c->tail = e;
  }
  c->head = e;
  c->len++;
}

static void qc_entry_free(qc_entry *e) {
  if (e == NULL) {
    return;
  }
  free(e->model);
  free(e->query);
  free(e->vec);
  free(e);
}

/* Evicts from the tail until there is room for one more. The original is
 * `while g.len() > self.capacity { g.pop_back() }` AFTER the push, so it
 * transiently holds capacity+1 entries; evicting BEFORE the push keeps the
 * invariant true at every observable instant, which is strictly stronger at
 * no cost — the same number of pops for the same sequence of puts. */
static void qc_make_room(kbc_query_cache *c) {
  while (c->len >= c->capacity && c->tail != NULL) {
    qc_entry *victim = c->tail;
    qc_unlink(c, victim);
    qc_entry_free(victim);
  }
}

kbc_query_cache *kbc_query_cache_new(size_t capacity, kbc_err *err) {
  if (capacity == 0) {
    (void)kbc_err_set(err, KBC_ERR_INVALID,
                       "query cache capacity must be at least 1");
    return NULL;
  }
  if (capacity > SIZE_MAX / sizeof(qc_entry)) {
    (void)kbc_err_set(err, KBC_ERR_NOMEM, "query cache capacity %zu overflows",
                      capacity);
    return NULL;
  }
  kbc_query_cache *c = calloc(1, sizeof *c);
  if (c == NULL) {
    (void)kbc_err_set(err, KBC_ERR_NOMEM, "calloc kbc_query_cache");
    return NULL;
  }
  c->capacity = capacity;
  if (pthread_mutex_init(&c->mu, NULL) != 0) {
    free(c);
    (void)kbc_err_set(err, KBC_ERR_INTERNAL,
                      "pthread_mutex_init for the query cache");
    return NULL;
  }
  return c;
}

void kbc_query_cache_free(kbc_query_cache *c) {
  if (c == NULL) {
    return;
  }
  (void)pthread_mutex_lock(&c->mu);
  qc_entry *e = c->head;
  while (e != NULL) {
    qc_entry *next = e->next;
    qc_entry_free(e);
    e = next;
  }
  c->head = c->tail = NULL;
  c->len = 0;
  (void)pthread_mutex_unlock(&c->mu);
  (void)pthread_mutex_destroy(&c->mu);
  free(c);
}

size_t kbc_query_cache_len(const kbc_query_cache *c) {
  if (c == NULL) {
    return 0;
  }
  kbc_query_cache *w = (kbc_query_cache *)(uintptr_t)(const void *)c;
  (void)pthread_mutex_lock(&w->mu);
  size_t n = c->len;
  (void)pthread_mutex_unlock(&w->mu);
  return n;
}

/* On a hit, copies the vector into `a` and MOVES the entry to the front.
 * The move is the point: `get` is a mutating touch in the original
 * (embed_cache.rs:103-107 — remove(pos) then push_front), which is what
 * makes the eviction order least-recently-USED rather than
 * least-recently-inserted. A read that does not touch degrades the cache
 * into a FIFO and a hot query still gets evicted.
 *
 * The copy is not an optimisation artefact: the caller gets a KBC_ARENA
 * vector that outlives the lock, and the entry's own buffer is freed or
 * overwritten on the next eviction of that key. */
bool kbc_query_cache_get(kbc_query_cache *c, const char *model,
                          const char *query, size_t query_len, kbc_arena *a,
                          const float **vec, size_t *dim) {
  if (c == NULL || model == NULL || query == NULL || a == NULL || vec == NULL ||
      dim == NULL) {
    return false;
  }
  *vec = NULL;
  *dim = 0;
  size_t model_len = strlen(model);
  (void)pthread_mutex_lock(&c->mu);
  qc_entry *hit = NULL;
  for (qc_entry *e = c->head; e != NULL; e = e->next) {
    if (qc_key_eq(e, model, model_len, query, query_len)) {
      hit = e;
      break;
    }
  }
  if (hit == NULL) {
    (void)pthread_mutex_unlock(&c->mu);
    return false;
  }
  size_t d = hit->dim;
  if (d > SIZE_MAX / sizeof(float)) {
    (void)pthread_mutex_unlock(&c->mu);
    return false;
  }
  float *copy = kbc_arena_alloc(a, d * sizeof(float));
  if (copy == NULL) {
    /* A failed copy is a miss, not a wrong answer: the caller falls through
     * to the sidecar and re-caches. Returning false keeps the arena OOM from
     * turning into a silently truncated vector. */
    (void)pthread_mutex_unlock(&c->mu);
    return false;
  }
  memcpy(copy, hit->vec, d * sizeof(float));
  qc_unlink(c, hit);
  qc_push_front(c, hit);
  (void)pthread_mutex_unlock(&c->mu);
  *vec = copy;
  *dim = d;
  return true;
}

/* Inserts or replaces (model, query). Replacing does NOT grow the cache: the
 * original removes the old entry before pushing (embed_cache.rs:113-116),
 * and a put that appended instead would let a repeatedly-queried key grow
 * the cache without bound while evicting everything else. */
kbc_status kbc_query_cache_put(kbc_query_cache *c, const char *model,
                               const char *query, size_t query_len,
                               const float *vec, size_t dim, kbc_err *err) {
  if (c == NULL || model == NULL || query == NULL || vec == NULL) {
    return kbc_err_set(err, KBC_ERR_INVALID,
                       "cache, model, query and vec must not be NULL");
  }
  if (dim == 0 || dim > KBC_EMBED_MAX_DIM) {
    return kbc_err_set(err, KBC_ERR_INVALID, "cache vector dim %zu is out of "
                                            "range",
                       dim);
  }
  size_t model_len = strlen(model);
  if (model_len > KBC_EMBED_MODEL_MAX) {
    return kbc_err_set(err, KBC_ERR_INVALID, "model name %zu is over the %u "
                                            "limit",
                       model_len, KBC_EMBED_MODEL_MAX);
  }
  if (query_len > KBC_QUERY_CACHE_KEY_MAX) {
    return kbc_err_set(err, KBC_ERR_INVALID,
                       "query is %zu bytes, over the %u cache key limit",
                       query_len, KBC_QUERY_CACHE_KEY_MAX);
  }
  if (dim > SIZE_MAX / sizeof(float)) {
    return kbc_err_set(err, KBC_ERR_NOMEM, "dim %zu overflows", dim);
  }

  /* Build the replacement before touching the list, so an allocation failure
   * leaves the cache exactly as it was (rule 10: no half-applied put). */
  qc_entry *fresh = calloc(1, sizeof *fresh);
  if (fresh == NULL) {
    return kbc_err_set(err, KBC_ERR_NOMEM, "calloc query cache entry");
  }
  fresh->model = malloc(model_len + 1);
  fresh->query = malloc(query_len + 1);
  fresh->vec = malloc(dim * sizeof(float));
  if (fresh->model == NULL || fresh->query == NULL || fresh->vec == NULL) {
    qc_entry_free(fresh);
    return kbc_err_set(err, KBC_ERR_NOMEM,
                       "query cache entry for model \"%s\" (%zu x %zu floats)",
                       model, query_len, dim);
  }
  memcpy(fresh->model, model, model_len + 1);
  memcpy(fresh->query, query, query_len);
  fresh->query[query_len] = '\0';
  memcpy(fresh->vec, vec, dim * sizeof(float));
  fresh->model_len = model_len;
  fresh->query_len = query_len;
  fresh->dim = dim;

  (void)pthread_mutex_lock(&c->mu);
  qc_entry *old = NULL;
  for (qc_entry *e = c->head; e != NULL; e = e->next) {
    if (qc_key_eq(e, model, model_len, query, query_len)) {
      old = e;
      break;
    }
  }
  if (old != NULL) {
    qc_unlink(c, old);
  }
  qc_make_room(c);
  qc_push_front(c, fresh);
  (void)pthread_mutex_unlock(&c->mu);
  qc_entry_free(old);
  return KBC_OK;
}


/* Embeds one query, consulting and then populating `c`.
 *
 * The two out-fields are NOT counters, they are per-call facts about THIS
 * call, and the pairing is what makes them readable:
 *
 *   embed_ms   wall time of the EMBEDDING step, in ms. ZERO on a cache hit,
 *              because on a hit there is no embedding step to time
 *              (embed_cache.rs:224-229, :284-290). It does not time the
 *              lookup, so it is not "how long the query took".
 *   cache_hit  whether the vector came from the cache rather than the
 *              sidecar.
 *
 * A caller reading embed_ms==0 therefore knows it did no inference, without
 * having to correlate two fields. If embed_ms timed the whole call it would
 * be non-zero on a hit and the field would say nothing.
 *
 * The cache is consulted BEFORE the embedder is locked, so a hit never waits
 * behind an in-flight document embed. On a miss the vector is put back into
 * the cache before returning, so a burst of identical queries costs one
 * round trip rather than one per caller. */
kbc_status kbc_embed_query(kbc_embedder *e, kbc_query_cache *c, kbc_arena *a,
                           const char *query, kbc_query_outcome *out,
                           kbc_err *err) {
  if (e == NULL || a == NULL || out == NULL || query == NULL) {
    return kbc_err_set(err, KBC_ERR_INVALID,
                       "embedder, arena, query and out must not be NULL");
  }
  memset(out, 0, sizeof *out);
  if (query[0] == '\0') {
    return kbc_err_set(err, KBC_ERR_INVALID, "query must not be empty");
  }
  size_t query_len = strlen(query);
  if (query_len > KBC_QUERY_CACHE_KEY_MAX) {
    return kbc_err_set(err, KBC_ERR_INVALID,
                       "query is %zu bytes, over the %u cache key limit",
                       query_len, KBC_QUERY_CACHE_KEY_MAX);
  }

  /* `c == NULL` embeds unconditionally, so a caller with no cache is not a
   * special case. No key is built and none is needed, so an embedder that
   * has not handshook is not in the way here. */
  char model[KBC_EMBED_MODEL_MAX + 1];
  if (c != NULL) {
    /* A failure here is reported, not swallowed. Embedding anyway and then
     * skipping the write-back would return a correct vector now and make
     * every later identical query a miss, with nothing saying why. */
    kbc_status mst = kbc_embedder_model(e, model, sizeof model, err);
    if (kbc_failed(mst)) {
      return mst;
    }

    const float *hit_vec = NULL;
    size_t hit_dim = 0;
    if (kbc_query_cache_get(c, model, query, query_len, a, &hit_vec, &hit_dim)) {
      /* The arena owns this copy; the const on hit_vec only records that the
       * cache never handed out its own buffer. */
      out->vec = (float *)(uintptr_t)(const void *)hit_vec;
      out->dim = hit_dim;
      out->embed_ms = 0; /* no embedding ran, so there is nothing to time */
      out->cache_hit = true;
      return KBC_OK;
    }
  }

  const char *texts[1];
  texts[0] = query;
  float *vec = NULL;
  int64_t started = now_ms();
  kbc_status st = kbc_embedder_embed(e, a, texts, 1u, 0, &vec, err);
  int64_t elapsed = now_ms() - started;
  if (kbc_failed(st)) {
    return st;
  }
  size_t dim = kbc_embedder_dim(e);
  if (vec == NULL || dim == 0) {
    return kbc_err_set(err, KBC_ERR_INTERNAL,
                      "sidecar returned no vector for a query");
  }
  out->vec = vec;
  out->dim = dim;
  out->embed_ms = (uint64_t)(elapsed > 0 ? elapsed : 0);
  out->cache_hit = false;
  /* A failed write-back is not a failed query: the vector is already in hand
   * and the caller must have it. The next identical query just misses again.
   * `model` is only initialised when `c != NULL`, so the put is guarded by
   * the same condition rather than reading an uninitialised key. */
  if (c != NULL) {
    kbc_err scratch;
    kbc_err_reset(&scratch);
    (void)kbc_query_cache_put(c, model, query, query_len, vec, dim, &scratch);
  }
  return KBC_OK;
}


size_t kbc_query_cache_capacity(const kbc_query_cache *c) {
  return c == NULL ? 0 : c->capacity;
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
