/* watcher.c — inotify-backed recursive watcher with per-(corpus,path) debounce.
 *
 * One thread per watcher. The thread owns every mutable field below except the
 * two atomic counters and the stop flag; the counters are the only state the
 * caller may read from another thread, and they are relaxed atomics.
 *
 * Watches are per DIRECTORY (inotify has no recursive mode), so the table maps
 * wd -> (corpus, relative dir path) and a directory created or moved into a
 * watched tree is walked immediately. The pending table coalesces raw events
 * into one publish per (corpus, path) after cfg->watcher_debounce_ms; the loop
 * is an epoll wait with a 100 ms timeout, so expiry is a real deadline check
 * rather than a sleep.
 */
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/inotify.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "kbc/watcher.h"

#include "kbc/log.h"
#include "kbc/mem.h"

/* A path we cannot even spell is a path we cannot index; drop the event
 * rather than truncate it into a wrong artifact. */
#define KBC_WATCH_PATH_MAX KBC_MAX_PATH_LEN
#define KBC_WATCH_MAX_DEPTH 64
#define KBC_WATCH_MAX_PENDING 65536u
#define KBC_WATCH_EPOLL_TIMEOUT_MS 100
#define KBC_WATCH_READ_BUF 65536

#define KBC_WATCH_MASK                                                          \
  (IN_CREATE | IN_CLOSE_WRITE | IN_MOVED_TO | IN_MOVED_FROM | IN_DELETE |       \
   IN_ATTRIB | IN_DELETE_SELF | IN_MOVE_SELF)

typedef struct {
  int wd;          /* inotify watch descriptor, kernel-owned */
  size_t corpus;   /* index into w->corpora */
  char *rel;       /* dir path relative to the corpus root; "" for the root */
} watch_entry;

typedef struct {
  size_t corpus;   /* index into w->corpora */
  char *rel;       /* file path relative to the corpus root */
  int64_t deadline_ms;
  bool removed;    /* a delete/rename-away was seen; wins over "changed" */
} pending_entry;

typedef struct {
  char *name;      /* KBC_OWN */
  char *path;      /* KBC_OWN */
  kbc_strlist ignore; /* KBC_OWN, copied out of the config */
} corpus_cfg;

struct kbc_watcher {
  corpus_cfg *corpora;
  size_t ncorpora;
  size_t debounce_ms;

  kbc_event_fn fn;
  void *user;

  int in_fd;
  int wake_r;
  int wake_w;
  int epoll_fd;
  pthread_t thread;
  bool thread_started;

  atomic_bool stop;
  atomic_bool running;
  atomic_llong events_seen;
  atomic_llong reindex_triggered;

  watch_entry *watches;
  size_t nwatches;
  size_t cap_watches;

  pending_entry *pending;
  size_t npending;
  size_t cap_pending;

  char *rbuf; /* read buffer for the inotify fd, owned */
};

static int64_t now_ms(void) {
  struct timespec ts;
  if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) return 0;
  return (int64_t)ts.tv_sec * 1000 + (int64_t)(ts.tv_nsec / 1000000);
}

/* ------------------------------------------------------------------ globs -- */

/* '*' any run, '?' exactly one char. Returns false on a non-terminating
 * pattern only if `text` runs out early, which is a plain mismatch. */
static bool wildmatch(const char *pat, const char *text) {
  const char *star = NULL;
  const char *after = NULL;
  while (*text != '\0') {
    if (*pat == '?' || *pat == *text) {
      pat++;
      text++;
    } else if (*pat == '*') {
      star = pat++;
      after = text;
    } else if (star != NULL) {
      pat = star + 1;
      text = ++after;
    } else {
      return false;
    }
  }
  while (*pat == '*') pat++;
  return *pat == '\0';
}

static bool has_glob(const char *s) {
  return strchr(s, '*') != NULL || strchr(s, '?') != NULL;
}

/* The config stores "glob-ish substrings": a pattern without a wildcard is a
 * plain substring of the relative path, a pattern with one is matched against
 * the whole relative path and against its last component. */
static bool ignore_hit(const corpus_cfg *c, const char *rel) {
  const char *base = strrchr(rel, '/');
  base = base ? base + 1 : rel;
  for (size_t i = 0; i < c->ignore.len; i++) {
    const char *pat = c->ignore.items[i];
    if (pat == NULL || pat[0] == '\0') continue;
    if (has_glob(pat)) {
      if (wildmatch(pat, rel) || wildmatch(pat, base)) return true;
    } else if (strstr(rel, pat) != NULL) {
      return true;
    }
  }
  return false;
}

/* A dotfile or dot-directory anywhere in the relative path is ignored, so
 * editors' swap dirs and .git never reach the index. */
static bool any_dot_component(const char *rel) {
  for (const char *p = rel; *p != '\0';) {
    if (p[0] == '.' && (p == rel || p[-1] == '/')) return true;
    const char *slash = strchr(p, '/');
    p = slash ? slash + 1 : p + strlen(p);
  }
  return false;
}

/* ----------------------------------------------------------------- tables -- */

static watch_entry *watch_find(const kbc_watcher *w, int wd) {
  for (size_t i = 0; i < w->nwatches; i++) {
    if (w->watches[i].wd == wd) return &w->watches[i];
  }
  return NULL;
}

static kbc_status watches_reserve(kbc_watcher *w, size_t extra) {
  if (w->nwatches + extra <= w->cap_watches) return KBC_OK;
  size_t cap = w->cap_watches ? w->cap_watches * 2 : 64;
  while (cap < w->nwatches + extra) {
    if (cap > SIZE_MAX / 2) return KBC_ERR_NOMEM;
    cap *= 2;
  }
  if (cap > SIZE_MAX / sizeof(*w->watches)) return KBC_ERR_NOMEM;
  watch_entry *p = realloc(w->watches, cap * sizeof(*w->watches));
  if (p == NULL) return KBC_ERR_NOMEM;
  w->watches = p;
  w->cap_watches = cap;
  return KBC_OK;
}

/* Drops the entry for `wd` (IN_IGNORED, or a directory that vanished). */
static void watch_remove(kbc_watcher *w, int wd) {
  for (size_t i = 0; i < w->nwatches; i++) {
    if (w->watches[i].wd != wd) continue;
    free(w->watches[i].rel);
    if (i + 1 < w->nwatches) {
      memmove(&w->watches[i], &w->watches[i + 1],
              (w->nwatches - i - 1) * sizeof(*w->watches));
    }
    w->nwatches--;
    return;
  }
}

/* Drops every entry whose directory is `dir_rel` or below it. The names are
 * published as removals by the caller before this runs. */
static void watch_remove_subtree(kbc_watcher *w, const char *dir_rel) {
  size_t n = strlen(dir_rel);
  for (size_t i = 0; i < w->nwatches;) {
    const char *rel = w->watches[i].rel;
    bool hit = (strcmp(rel, dir_rel) == 0) ||
               (n > 0 && strncmp(rel, dir_rel, n) == 0 && rel[n] == '/');
    if (!hit) {
      i++;
      continue;
    }
    free(w->watches[i].rel);
    if (i + 1 < w->nwatches) {
      memmove(&w->watches[i], &w->watches[i + 1],
              (w->nwatches - i - 1) * sizeof(*w->watches));
    }
    w->nwatches--;
  }
}

static kbc_status pending_reserve(kbc_watcher *w, size_t extra) {
  if (w->npending + extra <= w->cap_pending) return KBC_OK;
  size_t cap = w->cap_pending ? w->cap_pending * 2 : 64;
  while (cap < w->npending + extra) {
    if (cap > SIZE_MAX / 2) return KBC_ERR_NOMEM;
    cap *= 2;
  }
  if (cap > SIZE_MAX / sizeof(*w->pending)) return KBC_ERR_NOMEM;
  pending_entry *p = realloc(w->pending, cap * sizeof(*p));
  if (p == NULL) return KBC_ERR_NOMEM;
  w->pending = p;
  w->cap_pending = cap;
  return KBC_OK;
}

static void pending_remove_at(kbc_watcher *w, size_t i) {
  free(w->pending[i].rel);
  if (i + 1 < w->npending) {
    memmove(&w->pending[i], &w->pending[i + 1],
            (w->npending - i - 1) * sizeof(*w->pending));
  }
  w->npending--;
}

static pending_entry *pending_find(kbc_watcher *w, size_t corpus,
                                   const char *rel) {
  for (size_t i = 0; i < w->npending; i++) {
    if (w->pending[i].corpus == corpus && strcmp(w->pending[i].rel, rel) == 0) {
      return &w->pending[i];
    }
  }
  return NULL;
}

/* ---------------------------------------------------------------- publish -- */

/* The payload's `path` is CORPUS-RELATIVE, and `corpus` names which corpus it
 * belongs to. The index, the store and kbc_app's own touch/remove path are all
 * keyed on the relative path, so a subscriber that joined the root on again
 * would look up a document that never existed — and a real edit would read as
 * a removal. It is also the safer wire shape: an absolute path in an event
 * leaks the server's filesystem layout into every consumer (SSE, webhooks).
 * The root is the subscriber's business, not the event's. */
static void publish(kbc_watcher *w, size_t corpus, const char *rel,
                    bool removed) {
  const corpus_cfg *c = &w->corpora[corpus];
  if (rel[0] == '\0' || strlen(rel) >= KBC_WATCH_PATH_MAX) {
    KBC_LOGW("watcher: %s: path too long to report, dropping %s", c->name, rel);
    return;
  }
  const size_t n = strlen(rel);

  kbc_str js;
  kbc_str_init(&js);
  const char *action = removed ? "removed" : "changed";
  if (kbc_str_puts(&js, "{\"corpus\":") != KBC_OK ||
      kbc_str_append_json_string(&js, c->name, strlen(c->name)) != KBC_OK ||
      kbc_str_puts(&js, ",\"path\":") != KBC_OK ||
      kbc_str_append_json_string(&js, rel, n) != KBC_OK ||
      kbc_str_puts(&js, ",\"action\":") != KBC_OK ||
      kbc_str_append_json_string(&js, action, strlen(action)) != KBC_OK ||
      kbc_str_puts(&js, "}") != KBC_OK) {
    KBC_LOGE("watcher: out of memory building the event for %s/%s", c->name, rel);
    kbc_str_free(&js);
    return;
  }

  atomic_fetch_add_explicit(&w->reindex_triggered, 1, memory_order_relaxed);
  /* The callback runs on the watcher thread; it is documented to be prompt. */
  w->fn(w->user, removed ? "file.removed" : "file.changed", js.ptr);
  kbc_str_free(&js);
}

/* Queues a (corpus, path) for publication. The FIRST event for a key sets the
 * deadline; later events within the window only refine the action, so a
 * write-then-rename storm cannot push the deadline out forever. */
static void enqueue(kbc_watcher *w, size_t corpus, const char *rel,
                    bool removed) {
  if (rel[0] == '\0') return;
  if (any_dot_component(rel)) return;
  const corpus_cfg *c = &w->corpora[corpus];
  if (ignore_hit(c, rel)) return;

  pending_entry *pe = pending_find(w, corpus, rel);
  if (pe != NULL) {
    if (removed) pe->removed = true;
    return;
  }
  /* Bounded: publish the oldest entry rather than growing without limit. */
  while (w->npending >= KBC_WATCH_MAX_PENDING) {
    publish(w, w->pending[0].corpus, w->pending[0].rel,
            w->pending[0].removed);
    pending_remove_at(w, 0);
  }
  if (pending_reserve(w, 1) != KBC_OK) {
    KBC_LOGE("watcher: out of memory coalescing %s/%s", c->path, rel);
    return;
  }
  char *copy = strdup(rel);
  if (copy == NULL) {
    KBC_LOGE("watcher: out of memory coalescing %s/%s", c->path, rel);
    return;
  }
  w->pending[w->npending].corpus = corpus;
  w->pending[w->npending].rel = copy;
  w->pending[w->npending].deadline_ms =
      now_ms() + (int64_t)(w->debounce_ms ? w->debounce_ms : 1u);
  w->pending[w->npending].removed = removed;
  w->npending++;
}

static void drain_expired(kbc_watcher *w) {
  int64_t t = now_ms();
  for (size_t i = 0; i < w->npending;) {
    if (w->pending[i].deadline_ms > t) {
      i++;
      continue;
    }
    size_t corpus = w->pending[i].corpus;
    bool removed = w->pending[i].removed;
    publish(w, corpus, w->pending[i].rel, removed);
    pending_remove_at(w, i);
  }
}

/* ------------------------------------------------------------- watch tree -- */

/* Joins the corpus root and a relative directory path, NUL-terminated.
 * False means the joined path does not fit `cap` — the caller must not use
 * `out`, because snprintf already truncated it. */
static bool join_rel(const corpus_cfg *c, const char *rel, char *out,
                     size_t cap) {
  int n;
  if (rel[0] == '\0') {
    n = snprintf(out, cap, "%s", c->path);
  } else {
    n = snprintf(out, cap, "%s/%s", c->path, rel);
  }
  return n >= 0 && (size_t)n < cap;
}

static kbc_status add_watch_dir(kbc_watcher *w, size_t corpus, const char *rel,
                                int depth) {
  if (depth > KBC_WATCH_MAX_DEPTH) {
    KBC_LOGW("watcher: %s/%s deeper than %d levels, not watched",
             w->corpora[corpus].name, rel, KBC_WATCH_MAX_DEPTH);
    return KBC_OK;
  }
  if (watches_reserve(w, 1) != KBC_OK) return KBC_ERR_NOMEM;

  char full[KBC_WATCH_PATH_MAX];
  if (!join_rel(&w->corpora[corpus], rel, full, sizeof full)) {
    KBC_LOGW("watcher: %s/%s path too long, not watched",
             w->corpora[corpus].name, rel);
    return KBC_OK;
  }
  int wd = inotify_add_watch(w->in_fd, full, KBC_WATCH_MASK);
  if (wd < 0) {
    KBC_LOGW("watcher: inotify_add_watch %s: %s", full, strerror(errno));
    return KBC_OK; /* a vanished or unreadable dir is not fatal */
  }
  if (watch_find(w, wd) != NULL) return KBC_OK; /* already known */

  char *rel_copy = strdup(rel);
  if (rel_copy == NULL) return KBC_ERR_NOMEM;
  w->watches[w->nwatches].wd = wd;
  w->watches[w->nwatches].corpus = corpus;
  w->watches[w->nwatches].rel = rel_copy;
  w->nwatches++;

  DIR *d = opendir(full);
  if (d == NULL) return KBC_OK;
  struct dirent *de;
  while ((de = readdir(d)) != NULL) {
    if (de->d_name[0] == '.') continue; /* "." ".." and every dotfile */
    char sub[KBC_WATCH_PATH_MAX];
    int n;
    if (rel[0] == '\0') {
      n = snprintf(sub, sizeof sub, "%s", de->d_name);
    } else {
      n = snprintf(sub, sizeof sub, "%s/%s", rel, de->d_name);
    }
    if (n < 0 || (size_t)n >= sizeof sub) continue;
    if (any_dot_component(sub) || ignore_hit(&w->corpora[corpus], sub)) {
      continue;
    }
    /* lstat, not d_type alone: d_type is DT_UNKNOWN on some filesystems, and
     * a symlinked dir must not become a second watch on the same tree. */
    char subfull[KBC_WATCH_PATH_MAX];
    if (!join_rel(&w->corpora[corpus], sub, subfull, sizeof subfull)) {
      continue;
    }
    struct stat sb;
    if (lstat(subfull, &sb) != 0) continue;
    if (!S_ISDIR(sb.st_mode)) continue;
    if (add_watch_dir(w, corpus, sub, depth + 1) != KBC_OK) {
      closedir(d);
      return KBC_ERR_NOMEM;
    }
  }
  closedir(d);
  return KBC_OK;
}

/* ----------------------------------------------------------- inotify read -- */

static void handle_dir_removed(kbc_watcher *w, size_t corpus,
                               const char *rel) {
  /* The caller may hand us a pointer INTO the watch table, and
   * watch_remove_subtree frees those strings, so work from a copy. */
  char dir[KBC_WATCH_PATH_MAX];
  size_t n = strlen(rel);
  if (n >= sizeof dir) return;
  memcpy(dir, rel, n + 1);
  for (size_t i = 0; i < w->npending;) {
    pending_entry *pe = &w->pending[i];
    bool hit = strcmp(pe->rel, dir) == 0 ||
               (n > 0 && strncmp(pe->rel, dir, n) == 0 && pe->rel[n] == '/');
    if (!hit) {
      i++;
      continue;
    }
    if (pe->corpus == corpus) publish(w, corpus, pe->rel, true);
    pending_remove_at(w, i);
  }
  watch_remove_subtree(w, dir);
}

static void handle_event(kbc_watcher *w, const struct inotify_event *ev) {
  watch_entry *we = watch_find(w, ev->wd);
  if (we == NULL) return;
  size_t corpus = we->corpus;
  char *dir_rel = we->rel; /* handle_dir_removed copies before pruning */

  if ((ev->mask & IN_IGNORED) != 0U) {
    watch_remove(w, ev->wd);
    return;
  }

  bool is_dir = (ev->mask & IN_ISDIR) != 0U;
  if (ev->len == 0) {
    /* IN_DELETE_SELF / IN_MOVE_SELF on a watched dir: inotify follows with
     * IN_IGNORED, which is where the table entry actually goes. */
    if ((ev->mask & (IN_DELETE_SELF | IN_MOVE_SELF)) != 0U) {
      handle_dir_removed(w, corpus, dir_rel);
    }
    return;
  }

  char name[KBC_WATCH_PATH_MAX];
  size_t name_len = strlen(ev->name);
  if (name_len >= sizeof name) return;
  memcpy(name, ev->name, name_len + 1);

  char rel[KBC_WATCH_PATH_MAX];
  int n;
  if (dir_rel[0] == '\0') {
    n = snprintf(rel, sizeof rel, "%s", name);
  } else {
    n = snprintf(rel, sizeof rel, "%s/%s", dir_rel, name);
  }
  if (n < 0 || (size_t)n >= sizeof rel) {
    KBC_LOGW("watcher: %s: path too long, event dropped",
             w->corpora[corpus].name);
    return;
  }

  if (is_dir) {
    if ((ev->mask & (IN_CREATE | IN_MOVED_TO)) != 0U) {
      /* A new subdirectory gets its own watch right away; the files created
       * inside it before this point are picked up by the caller's reindex. */
      if (add_watch_dir(w, corpus, rel, 1) != KBC_OK) {
        KBC_LOGE("watcher: out of memory watching %s/%s",
                 w->corpora[corpus].name, rel);
      }
      return;
    }
    if ((ev->mask & (IN_DELETE | IN_MOVED_FROM)) != 0U) {
      handle_dir_removed(w, corpus, rel);
      return;
    }
    return;
  }

  bool gone = (ev->mask & (IN_DELETE | IN_MOVED_FROM)) != 0U;
  enqueue(w, corpus, rel, gone);
}

static void read_inotify(kbc_watcher *w) {
  for (;;) {
    ssize_t n = read(w->in_fd, w->rbuf, KBC_WATCH_READ_BUF);
    if (n < 0) {
      if (errno == EINTR) continue;
      if (errno == EAGAIN || errno == EWOULDBLOCK) return;
      KBC_LOGE("watcher: read inotify: %s", strerror(errno));
      return;
    }
    if (n == 0) return;
    size_t off = 0;
    while (off + sizeof(struct inotify_event) <= (size_t)n) {
      const struct inotify_event *ev =
          (const struct inotify_event *)(const void *)(w->rbuf + off);
      size_t sz = sizeof(struct inotify_event) + (size_t)ev->len;
      if (off + sz > (size_t)n) break; /* truncated tail, wait for more */
      atomic_fetch_add_explicit(&w->events_seen, 1, memory_order_relaxed);
      handle_event(w, ev);
      off += sz;
    }
    if ((size_t)n < KBC_WATCH_READ_BUF) return;
  }
}

/* ------------------------------------------------------------------- loop -- */

static void *watcher_main(void *arg) {
  kbc_watcher *w = arg;
  atomic_store_explicit(&w->running, true, memory_order_release);

  while (!atomic_load_explicit(&w->stop, memory_order_acquire)) {
    struct epoll_event evs[4];
    int n = epoll_wait(w->epoll_fd, evs, 4, KBC_WATCH_EPOLL_TIMEOUT_MS);
    if (n < 0) {
      if (errno == EINTR) {
        drain_expired(w);
        continue;
      }
      KBC_LOGE("watcher: epoll_wait: %s", strerror(errno));
      break;
    }
    for (int i = 0; i < n; i++) {
      int fd = evs[i].data.fd;
      if (fd == w->in_fd) {
        read_inotify(w);
      } else if (fd == w->wake_r) {
        char drain[64];
        while (read(w->wake_r, drain, sizeof drain) > 0) {
          /* one wakeup byte is all the contract needs */
        }
      }
    }
    drain_expired(w);
  }

  /* Publish whatever is still inside its debounce window, so a stop never
   * silently drops a change the caller was told would be reported. */
  while (w->npending > 0) {
    publish(w, w->pending[0].corpus, w->pending[0].rel,
            w->pending[0].removed);
    pending_remove_at(w, 0);
  }
  atomic_store_explicit(&w->running, false, memory_order_release);
  return NULL;
}

/* ----------------------------------------------------------------- public -- */

static void watcher_free(kbc_watcher *w) {
  if (w == NULL) return;
  if (w->in_fd >= 0) close(w->in_fd);
  if (w->wake_r >= 0) close(w->wake_r);
  if (w->wake_w >= 0) close(w->wake_w);
  if (w->epoll_fd >= 0) close(w->epoll_fd);
  for (size_t i = 0; i < w->nwatches; i++) free(w->watches[i].rel);
  free(w->watches);
  for (size_t i = 0; i < w->npending; i++) free(w->pending[i].rel);
  free(w->pending);
  free(w->rbuf);
  for (size_t i = 0; i < w->ncorpora; i++) {
    free(w->corpora[i].name);
    free(w->corpora[i].path);
    kbc_strlist_free(&w->corpora[i].ignore);
  }
  free(w->corpora);
  free(w);
}

kbc_watcher *kbc_watcher_start(const kbc_config *cfg, kbc_event_fn fn,
                              void *user, kbc_err *err) {
  if (cfg == NULL || fn == NULL) {
    kbc_err_set(err, KBC_ERR_INVALID, "kbc_watcher_start: %s is NULL",
                cfg == NULL ? "cfg" : "the event callback");
    return NULL;
  }

  kbc_watcher *w = calloc(1, sizeof *w);
  if (w == NULL) {
    kbc_err_set(err, KBC_ERR_NOMEM, "kbc_watcher_start: %zu bytes",
                sizeof *w);
    return NULL;
  }
  w->in_fd = w->wake_r = w->wake_w = w->epoll_fd = -1;
  w->fn = fn;
  w->user = user;
  w->debounce_ms = cfg->watcher_debounce_ms;
  atomic_init(&w->stop, false);
  atomic_init(&w->running, false);
  atomic_init(&w->events_seen, 0);
  atomic_init(&w->reindex_triggered, 0);

  if (cfg->ncorpora > 0) {
    w->corpora = calloc(cfg->ncorpora, sizeof *w->corpora);
    if (w->corpora == NULL) goto oom;
    w->ncorpora = cfg->ncorpora;
  }
  for (size_t i = 0; i < w->ncorpora; i++) {
    const kbc_corpus_cfg *src = &cfg->corpora[i];
    w->corpora[i].name = strdup(src->name ? src->name : "");
    w->corpora[i].path = strdup(src->path ? src->path : "");
    kbc_strlist_init(&w->corpora[i].ignore);
    if (w->corpora[i].name == NULL || w->corpora[i].path == NULL) goto oom;
    for (size_t k = 0; k < src->ignore.len; k++) {
      if (kbc_strlist_push(&w->corpora[i].ignore, src->ignore.items[k]) !=
          KBC_OK) {
        goto oom;
      }
    }
  }

  w->rbuf = malloc(KBC_WATCH_READ_BUF);
  if (w->rbuf == NULL) goto oom;

  w->in_fd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
  if (w->in_fd < 0) {
    kbc_err_set(err, KBC_ERR_IO, "inotify_init1: %s", strerror(errno));
    watcher_free(w);
    return NULL;
  }
  int pipefd[2];
  if (pipe(pipefd) != 0) {
    kbc_err_set(err, KBC_ERR_IO, "pipe for watcher wakeup: %s",
                strerror(errno));
    watcher_free(w);
    return NULL;
  }
  w->wake_r = pipefd[0];
  w->wake_w = pipefd[1];
  /* The read end MUST be non-blocking: the loop drains it in a
   * `while (read(...) > 0)` and the second read on an empty pipe would park
   * the thread forever, so stop() would hang in pthread_join. A blocking
   * write end is still what we want — a full pipe already means a wakeup is
   * pending, and stop() must not fail or block. */
  (void)fcntl(w->wake_r, F_SETFL, O_NONBLOCK);
  (void)fcntl(w->wake_w, F_SETFD, FD_CLOEXEC);
  (void)fcntl(w->wake_r, F_SETFD, FD_CLOEXEC);

  w->epoll_fd = epoll_create1(EPOLL_CLOEXEC);
  if (w->epoll_fd < 0) {
    kbc_err_set(err, KBC_ERR_IO, "epoll_create1: %s", strerror(errno));
    watcher_free(w);
    return NULL;
  }
  struct epoll_event ee;
  memset(&ee, 0, sizeof ee);
  ee.events = EPOLLIN;
  ee.data.fd = w->in_fd;
  if (epoll_ctl(w->epoll_fd, EPOLL_CTL_ADD, w->in_fd, &ee) != 0) {
    kbc_err_set(err, KBC_ERR_IO, "epoll_ctl inotify: %s", strerror(errno));
    watcher_free(w);
    return NULL;
  }
  ee.data.fd = w->wake_r;
  if (epoll_ctl(w->epoll_fd, EPOLL_CTL_ADD, w->wake_r, &ee) != 0) {
    kbc_err_set(err, KBC_ERR_IO, "epoll_ctl wakeup: %s", strerror(errno));
    watcher_free(w);
    return NULL;
  }

  for (size_t i = 0; i < w->ncorpora; i++) {
    if (add_watch_dir(w, i, "", 0) != KBC_OK) {
      kbc_err_set(err, KBC_ERR_NOMEM, "watching %s: out of memory",
                  w->corpora[i].path);
      watcher_free(w);
      return NULL;
    }
  }

  int rc = pthread_create(&w->thread, NULL, watcher_main, w);
  if (rc != 0) {
    kbc_err_set(err, KBC_ERR_INTERNAL, "pthread_create for watcher: %s",
                strerror(rc));
    watcher_free(w);
    return NULL;
  }
  w->thread_started = true;
  return w;

oom:
  kbc_err_set(err, KBC_ERR_NOMEM, "kbc_watcher_start: out of memory");
  watcher_free(w);
  return NULL;
}

void kbc_watcher_stop(kbc_watcher *w) {
  if (w == NULL) return;
  atomic_store_explicit(&w->stop, true, memory_order_release);
  /* Wake the epoll wait even if the thread is parked in read(). */
  if (w->wake_w >= 0) {
    const char byte = 'x';
    ssize_t n;
    do {
      n = write(w->wake_w, &byte, 1);
    } while (n < 0 && errno == EINTR);
    if (n < 0 && errno != EAGAIN) {
      KBC_LOGW("watcher: wakeup write: %s", strerror(errno));
    }
  }
  if (w->thread_started) {
    (void)pthread_join(w->thread, NULL);
  }
  watcher_free(w);
}

bool kbc_watcher_is_running(const kbc_watcher *w) {
  if (w == NULL) return false;
  return atomic_load_explicit(&w->running, memory_order_acquire);
}

void kbc_watcher_counts(const kbc_watcher *w, int64_t *events_seen,
                        int64_t *reindex_triggered) {
  if (events_seen != NULL) {
    *events_seen = (w == NULL)
                       ? 0
                       : atomic_load_explicit(&w->events_seen,
                                              memory_order_relaxed);
  }
  if (reindex_triggered != NULL) {
    *reindex_triggered = (w == NULL)
                             ? 0
                             : atomic_load_explicit(&w->reindex_triggered,
                                                    memory_order_relaxed);
  }
}
