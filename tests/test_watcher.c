/* test_watcher.c — src/watcher.c: inotify detection, debounce, ignore rules,
 * and the lifecycle contract of kbc_watcher_start/stop.
 *
 * inotify delivery is asynchronous, so every positive expectation polls with a
 * deadline and every negative expectation waits a fixed settle window with a
 * positive control in the same watcher. */
#define _GNU_SOURCE /* pthread_timedjoin_np */
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "kbc/mem.h"
#include "kbc/watcher.h"
#include "kbc_test.h"

/* Generous: CI machines are slow and the debounce itself eats 250ms. */
#define WAIT_MS 5000
#define SETTLE_MS 1200

typedef struct {
  char type[32];
  char json[1024];
} ev_rec;

#define SINK_CAP 256

typedef struct {
  pthread_mutex_t mu;
  ev_rec evs[SINK_CAP];
  size_t n;
  size_t dropped;
} sink;

static void sink_init(sink *s) {
  memset(s, 0, sizeof *s);
  pthread_mutex_init(&s->mu, NULL);
}

static void sink_free(sink *s) { pthread_mutex_destroy(&s->mu); }

static void on_event(void *user, const char *type, const char *json) {
  sink *s = (sink *)user;
  pthread_mutex_lock(&s->mu);
  if (s->n < SINK_CAP) {
    snprintf(s->evs[s->n].type, sizeof s->evs[s->n].type, "%s", type);
    snprintf(s->evs[s->n].json, sizeof s->evs[s->n].json, "%s", json);
    s->n++;
  } else {
    s->dropped++;
  }
  pthread_mutex_unlock(&s->mu);
}

static size_t sink_n(sink *s) {
  
  size_t n;
  pthread_mutex_lock(&s->mu);
  n = s->n;
  pthread_mutex_unlock(&s->mu);
  return n;
}

/* The payload is a flat object with no nesting, so the value after "key": is
 * readable without a JSON parser — but only because the watcher builds it
 * itself. Escapes would break this, so also require the closing quote. */
static bool payload_has(const char *json, const char *key, const char *val) {
  char open[64];
  int n = snprintf(open, sizeof open, "\"%s\":\"", key);
  if (n < 0 || (size_t)n >= sizeof open) return false;
  const char *p = strstr(json, open);
  if (p == NULL) return false;
  p += n;
  size_t vlen = strlen(val);
  return strncmp(p, val, vlen) == 0 && p[vlen] == '"';
}

static int count_type_path(sink *s, const char *type, const char *path) {
  
  int n = 0;
  pthread_mutex_lock(&s->mu);
  for (size_t i = 0; i < s->n; i++) {
    if (strcmp(s->evs[i].type, type) == 0 &&
        payload_has(s->evs[i].json, "path", path))
      n++;
  }
  pthread_mutex_unlock(&s->mu);
  return n;
}

static int count_path(sink *s, const char *path) {
  
  int n = 0;
  pthread_mutex_lock(&s->mu);
  for (size_t i = 0; i < s->n; i++) {
    if (payload_has(s->evs[i].json, "path", path)) n++;
  }
  pthread_mutex_unlock(&s->mu);
  return n;
}

static void nap_ms(long ms) {
  struct timespec ts;
  ts.tv_sec = ms / 1000;
  ts.tv_nsec = (ms % 1000) * 1000000L;
  (void)nanosleep(&ts, NULL);
}

/* gcc's format-truncation warning is right: a 4096 root plus a name can
 * overflow. Fail loudly instead of feeding a truncated path to inotify. */
static void path_join(char *out, size_t cap, const char *dir, const char *name) {
  size_t d = strlen(dir), n = strlen(name);
  if (d + 1 + n + 1 > cap) {
    out[0] = '\0';
    KBC_CHECK_MSG(false, "path does not fit in %zu bytes", cap);
    return;
  }
  memcpy(out, dir, d);
  out[d] = '/';
  memcpy(out + d + 1, name, n + 1);
}

static long now_ms(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return ts.tv_sec * 1000L + ts.tv_nsec / 1000000L;
}

/* Waits until pred(sink) holds, or the deadline passes. Returns the final
 * value of pred so the caller can assert on it either way. */
static int wait_count(sink *s, int (*pred)(sink *, const void *),
                      const void *arg, long timeout_ms) {
  long deadline = now_ms() + timeout_ms;
  for (;;) {
    int v = pred(s, arg);
    if (v > 0 || now_ms() >= deadline) return v;
    nap_ms(10);
  }
}

static int pred_any_for(sink *s, const void *arg) {
  return count_path(s, (const char *)arg);
}
static int pred_type_path(sink *s, const void *arg) {
  const char *const *pair = (const char *const *)arg;
  return count_type_path(s, pair[0], pair[1]);
}

/* A config with one corpus rooted at `root`; `ignore0` may be NULL. */
static kbc_config *make_cfg(const char *root, const char *ignore0,
                            size_t debounce_ms) {
  kbc_config *cfg = kbc_config_defaults();
  if (cfg == NULL) return NULL;
  cfg->corpora = (kbc_corpus_cfg *)calloc(1, sizeof *cfg->corpora);
  if (cfg->corpora == NULL) {
    kbc_config_free(cfg);
    return NULL;
  }
  cfg->ncorpora = 1;
  cfg->corpora[0].name = strdup("c1");
  cfg->corpora[0].path = strdup(root);
  kbc_strlist_init(&cfg->corpora[0].ignore);
  if (ignore0 != NULL) (void)kbc_strlist_push(&cfg->corpora[0].ignore, ignore0);
  cfg->watcher_debounce_ms = debounce_ms;
  return cfg;
}

typedef struct {
  char root[KBC_TEST_PATH_MAX];
  kbc_config *cfg;
  kbc_watcher *w;
  sink s;
} fixture;

static void fx_setup(fixture *f, const char *ignore0, size_t debounce_ms) {
  kbc_test_tmpdir(f->root, sizeof f->root);
  sink_init(&f->s);
  f->w = NULL;
  f->cfg = make_cfg(f->root, ignore0, debounce_ms);
  KBC_CHECK_NOT_NULL(f->cfg);
  kbc_err err;
  kbc_err_reset(&err);
  f->w = kbc_watcher_start(f->cfg, on_event, &f->s, &err);
  if (f->w == NULL) fprintf(stderr, "  watcher_start: %s\n", err.msg);
  KBC_CHECK_NOT_NULL(f->w);
  /* The thread publishes nothing until something happens, but is_running must
   * settle; a NULL watcher makes every later check meaningless. */
  if (f->w != NULL) {
    long deadline = now_ms() + 2000;
    while (!kbc_watcher_is_running(f->w) && now_ms() < deadline) nap_ms(5);
    KBC_CHECK_MSG(kbc_watcher_is_running(f->w),
                  "watcher never reached the running state");
  }
}

/* A stop that never returns would hang the entire suite instead of failing it,
 * so the join happens on a helper thread we can time out on. Returns the
 * elapsed milliseconds; on timeout the helper thread is left parked (process
 * exit reclaims it) and a failure has already been recorded. */
static void *stop_helper(void *p) {
  kbc_watcher_stop(*(kbc_watcher **)p);
  return NULL;
}

static long stop_timed(kbc_watcher *w, long budget_ms) {
  kbc_watcher *arg = w;
  pthread_t th;
  if (pthread_create(&th, NULL, stop_helper, &arg) != 0) {
    kbc_test_fail(__FILE__, __LINE__, "pthread_create for the stop helper");
    return -1;
  }
  struct timespec ts;
  clock_gettime(CLOCK_REALTIME, &ts);
  ts.tv_sec += budget_ms / 1000;
  ts.tv_nsec += (budget_ms % 1000) * 1000000L;
  if (ts.tv_nsec >= 1000000000L) {
    ts.tv_sec++;
    ts.tv_nsec -= 1000000000L;
  }
  long t0 = now_ms();
  int rc = pthread_timedjoin_np(th, NULL, &ts);
  long elapsed = now_ms() - t0;
  KBC_CHECK_MSG(rc == 0,
                "kbc_watcher_stop had not returned after %ldms: the watcher "
                "thread never observed the stop request (rc=%d)",
                elapsed, rc);
  return elapsed;
}
static void fx_teardown(fixture *f) {
  stop_timed(f->w, 2000); /* the sink outlives the join */
  kbc_config_free(f->cfg);
  sink_free(&f->s);
  kbc_test_rmrf(f->root);
}

/* ------------------------------------------------------------------ cases -- */

KBC_TEST(create_publishes_file_changed) {
  fixture f;
  fx_setup(&f, NULL, 250);
  char path[KBC_TEST_PATH_MAX];
  path_join(path, sizeof path, f.root, "created.md");
  kbc_test_write_file(path, "hello\n");

  const char *arg = path;
  int n = wait_count(&f.s, pred_any_for, arg, WAIT_MS);
  KBC_CHECK_MSG(n == 1, "expected exactly 1 event for the new file, got %d", n);

  int changed = count_type_path(&f.s, "file.changed", path);
  KBC_CHECK_MSG(changed == 1, "expected 1 file.changed, got %d", changed);
  KBC_CHECK_EQ_INT(count_type_path(&f.s, "file.removed", path), 0);

  /* The action lives in the payload, not only in the event type. */
  bool saw_changed_action = false;
  for (size_t i = 0; i < sink_n(&f.s); i++) {
    if (strcmp(f.s.evs[i].type, "file.changed") == 0 &&
        payload_has(f.s.evs[i].json, "action", "changed"))
      saw_changed_action = true;
  }
  KBC_CHECK_MSG(saw_changed_action, "no event carried action=changed");

  fx_teardown(&f);
}

KBC_TEST(write_then_rename_yields_one_event) {
  fixture f;
  fx_setup(&f, NULL, 250);
  char path[KBC_TEST_PATH_MAX], tmp[KBC_TEST_PATH_MAX];
  path_join(path, sizeof path, f.root, "target.md");
  path_join(tmp, sizeof tmp, f.root, ".stage.md");
  kbc_test_write_file(path, "v1\n");
  wait_count(&f.s, pred_any_for, path, WAIT_MS);
  const int pre = count_path(&f.s, path); /* the creation event is not part of it */
  const size_t before = sink_n(&f.s);

  /* The atomic-save shape: write a sibling, then rename it over the target. */
  kbc_test_write_file(tmp, "v2\n");
  KBC_CHECK_MSG(rename(tmp, path) == 0, "rename over the target failed");

  const char *arg = path;
  (void)wait_count(&f.s, pred_any_for, arg, WAIT_MS);
  nap_ms(SETTLE_MS);
  KBC_CHECK_MSG(count_path(&f.s, path) - pre == 1,
                "write-then-rename produced %d event(s) for the target, want 1",
                count_path(&f.s, path) - pre);
  KBC_CHECK_EQ_INT(count_type_path(&f.s, "file.removed", path), 0);
  /* Nothing else may fire either: the staging file is a dotfile, so the
   * rename contributes exactly one event, for the target. */
  KBC_CHECK_MSG(sink_n(&f.s) - before == 1,
                "the write-then-rename storm published %zu event(s) in total, "
                "want 1", sink_n(&f.s) - before);

  fx_teardown(&f);
}

KBC_TEST(rapid_writes_to_one_path_coalesce) {
  fixture f;
  fx_setup(&f, NULL, 400);
  char path[KBC_TEST_PATH_MAX];
  path_join(path, sizeof path, f.root, "hot.md");

  /* Six writes well inside the 400ms window, and the file is created by the
   * first of them — creation and every write must collapse into one event. */
  for (int i = 0; i < 6; i++) {
    kbc_test_write_file(path, "x");
    nap_ms(5);
  }
  nap_ms(WAIT_MS);
  KBC_CHECK_MSG(count_path(&f.s, path) == 1,
                "6 rapid writes produced %d event(s), want 1 (debounce)",
                count_path(&f.s, path));

  fx_teardown(&f);
}

KBC_TEST(writes_to_different_paths_do_not_coalesce) {
  fixture f;
  fx_setup(&f, NULL, 400);
  char a[KBC_TEST_PATH_MAX], b[KBC_TEST_PATH_MAX];
  path_join(a, sizeof a, f.root, "one.md");
  path_join(b, sizeof b, f.root, "two.md");
  kbc_test_write_file(a, "1");
  kbc_test_write_file(b, "2");
  nap_ms(WAIT_MS);
  KBC_CHECK_EQ_INT(count_path(&f.s, a), 1);
  KBC_CHECK_EQ_INT(count_path(&f.s, b), 1);
  KBC_CHECK_MSG(sink_n(&f.s) == 2, "expected 2 events total, got %zu",
                sink_n(&f.s));

  fx_teardown(&f);
}

KBC_TEST(delete_publishes_file_removed) {
  fixture f;
  fx_setup(&f, NULL, 250);
  char path[KBC_TEST_PATH_MAX];
  path_join(path, sizeof path, f.root, "doomed.md");
  kbc_test_write_file(path, "bye\n");
  (void)wait_count(&f.s, pred_any_for, path, WAIT_MS);

  KBC_CHECK_MSG(unlink(path) == 0, "unlink failed");

  const char *pair[2] = {"file.removed", path};
  int n = wait_count(&f.s, pred_type_path, pair, WAIT_MS);
  KBC_CHECK_MSG(n == 1, "expected exactly 1 file.removed, got %d", n);

  for (size_t i = 0; i < sink_n(&f.s); i++) {
    if (strcmp(f.s.evs[i].type, "file.removed") == 0) {
      KBC_CHECK_MSG(payload_has(f.s.evs[i].json, "action", "removed"),
                    "file.removed payload lacked action=removed");
    }
  }

  fx_teardown(&f);
}

KBC_TEST(ignored_and_dotfile_names_publish_nothing) {
  fixture f;
  fx_setup(&f, "*.tmp.md", 250);
  char ignored[KBC_TEST_PATH_MAX], dot[KBC_TEST_PATH_MAX],
      control[KBC_TEST_PATH_MAX];
  path_join(ignored, sizeof ignored, f.root, "scratch.tmp.md");
  path_join(dot, sizeof dot, f.root, ".hidden.md");
  path_join(control, sizeof control, f.root, "kept.md");

  kbc_test_write_file(ignored, "x");
  kbc_test_write_file(dot, "x");
  kbc_test_write_file(control, "x");

  /* The control must arrive; otherwise "no event" below would prove nothing. */
  (void)wait_count(&f.s, pred_any_for, control, WAIT_MS);
  KBC_CHECK_MSG(count_path(&f.s, control) == 1,
                "the non-ignored control file did not publish, test is vacuous");
  nap_ms(SETTLE_MS);
  KBC_CHECK_EQ_INT(count_path(&f.s, ignored), 0);
  KBC_CHECK_EQ_INT(count_path(&f.s, dot), 0);

  fx_teardown(&f);
}

KBC_TEST(stop_is_prompt_and_null_safe) {
  fixture f;
  fx_setup(&f, NULL, 250);
  kbc_watcher *w = f.w;
  f.w = NULL; /* fx_teardown must not stop it again */
  /* stop_timed fails if the stop has not returned within the 2s budget, so a
   * stuck stop is reported here instead of hanging the suite. */
  (void)stop_timed(w, 2000);
  kbc_watcher_stop(NULL);
  kbc_watcher_stop(NULL);
  /* A second, independent watcher starts and stops cleanly after the first,
   * so stop leaves no global state behind. */
  kbc_err err;
  kbc_err_reset(&err);
  sink s2;
  sink_init(&s2);
  kbc_watcher *w2 = kbc_watcher_start(f.cfg, on_event, &s2, &err);
  KBC_CHECK_NOT_NULL(w2);
  (void)stop_timed(w2, 2000);
  sink_free(&s2);
  kbc_config_free(f.cfg);
  kbc_test_rmrf(f.root);
}

KBC_TEST(start_rejects_null_arguments) {
  kbc_err err;
  kbc_err_reset(&err);
  kbc_watcher *w = kbc_watcher_start(NULL, on_event, NULL, &err);
  KBC_CHECK_NULL(w);
  KBC_CHECK_ERR_MSG(err);
  KBC_CHECK_EQ_INT(err.status, KBC_ERR_INVALID);

  char root[KBC_TEST_PATH_MAX];
  kbc_test_tmpdir(root, sizeof root);
  kbc_config *cfg = make_cfg(root, NULL, 250);
  KBC_CHECK_NOT_NULL(cfg);
  kbc_err_reset(&err);
  w = kbc_watcher_start(cfg, NULL, NULL, &err);
  KBC_CHECK_NULL(w);
  KBC_CHECK_ERR_MSG(err);
  KBC_CHECK_EQ_INT(err.status, KBC_ERR_INVALID);
  kbc_config_free(cfg);
  kbc_test_rmrf(root);
}

int main(void) {
  static const kbc_test_case cases[] = {
      {"create_publishes_file_changed", create_publishes_file_changed},
      {"write_then_rename_yields_one_event", write_then_rename_yields_one_event},
      {"rapid_writes_to_one_path_coalesce", rapid_writes_to_one_path_coalesce},
      {"writes_to_different_paths_do_not_coalesce",
       writes_to_different_paths_do_not_coalesce},
      {"delete_publishes_file_removed", delete_publishes_file_removed},
      {"ignored_and_dotfile_names_publish_nothing",
       ignored_and_dotfile_names_publish_nothing},
      {"stop_is_prompt_and_null_safe", stop_is_prompt_and_null_safe},
      {"start_rejects_null_arguments", start_rejects_null_arguments},
      {NULL, NULL},
  };
  return kbc_test_run("watcher", cases);
}
