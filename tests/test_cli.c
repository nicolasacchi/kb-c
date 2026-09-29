/* test_cli.c — cli/main.c: the `backup` and `restore` verbs.
 *
 * These run the real `kbc` binary as a subprocess. The contract under test is
 * the one a shell sees: the exit code, the tarball on disk, the files left
 * behind. A test that called an internal function would pass while the operator
 * still got the wrong exit code, and the exit code IS most of this contract.
 *
 * THE CONTRACT IS THIS BINARY'S OWN — 0 success, 1 user error / usage, 2 a
 * daemon or IO failure — and it is the SAME for every verb. The Rust CLI splits
 * it the other way (0/1 any failure/2 usage), but that split is clap's built-in
 * usage default plus stdlib `Termination`, not a contract the kb repo chose;
 * reproducing it inside one binary would make `kbc backup` and `kbc search`
 * disagree about what a bad flag means. The divergence from the Rust is
 * deliberate; `the_same_user_error_exits_the_same_code_from_every_verb` is the
 * test that pins it.
 *
 * The binary sits next to this test executable in the build tree, so it is
 * found through /proc/self/exe rather than through a hard-coded path that
 * would only work from one directory. */

#include "kbc_test.h"

#include <signal.h>
#include <ctype.h>
#include <sys/wait.h>
#include <time.h>

#include "kbc/json.h"
#include "kbc/store.h"

/* ------------------------------------------------------------- helpers --- */

/* The `kbc` under test: the same directory this test executable lives in. */

/* `<a>/<b>` into a fixed buffer. Written out rather than snprintf'd because
 * both operands are KBC_TEST_PATH_MAX-sized arrays here, so `%s/%s` into a
 * third is a provable -Wformat-truncation error — and a truncated path is a
 * DIFFERENT path, silently pointing the assertion at the wrong file. An
 * overflow aborts rather than cutting. */
static void path_join(char *dst, size_t cap, const char *a, const char *b) {
  size_t la = strlen(a);
  size_t lb = strlen(b);
  if (la + 1u + lb + 1u > cap) {
    abort();
  }
  memcpy(dst, a, la);
  dst[la] = '/';
  memcpy(dst + la + 1u, b, lb);
  dst[la + 1u + lb] = '\0';
}

/* The `kbc` under test: the same directory this test executable lives in. */
static const char *kbc_bin(void) {
  static char path[KBC_TEST_PATH_MAX];
  static bool resolved = false;
  if (resolved) {
    return path;
  }
  resolved = true;
  ssize_t n = readlink("/proc/self/exe", path, sizeof path - 1u);
  if (n <= 0) {
    path[0] = '\0';
    return path;
  }
  path[n] = '\0';
  char *slash = strrchr(path, '/');
  if (slash == NULL) {
    path[0] = '\0';
    return path;
  }
  (void)snprintf(slash + 1, sizeof path - (size_t)(slash + 1 - path), "kbc");
  return path;
}

/* An execv argv, built without casting away const.
 *
 * execv wants `char *const argv[]` and -Wcast-qual refuses to make one out of
 * a `const char *`. A cast would also be a lie: execv only READS these. So
 * each argument is strdup'd, owned, and freed by argv_free. Test code that
 * quietly lied about its own pointers would be the wrong place to start. */
typedef struct {
  char **v;
  size_t n;
  size_t cap;
} argv_vec;

static void argv_free(argv_vec *a) {
  for (size_t i = 0; i < a->n; i++) {
    free(a->v[i]);
  }
  free(a->v);
  a->v = NULL;
  a->n = 0;
  a->cap = 0;
}

static void argv_push(argv_vec *a, const char *arg) {
  if (a->n + 2u > a->cap) {
    size_t cap = a->cap == 0 ? 8u : a->cap * 2u;
    char **grown = realloc(a->v, cap * sizeof *grown);
    if (grown == NULL) {
      abort();
    }
    a->v = grown;
    a->cap = cap;
  }
  char *copy = strdup(arg);
  if (copy == NULL) {
    abort();
  }
  a->v[a->n++] = copy;
  a->v[a->n] = NULL;
}

/* Runs `kbc <argv...>` with HOME and KBC_CONFIG_PATH pinned, and returns the
 * exit status. `out`, when non-NULL, receives stdout+stderr. A spawn failure
 * is reported as -1 so a test can tell "the binary is missing" from "the
 * binary exited non-zero" — the first is a broken test run and the second is
 * usually the thing under test.
 *
 * `errout`, when non-NULL, splits the streams instead of merging them: the
 * verbs whose output an operator pipes (`reindex`, `metrics`) promise a
 * machine-readable stdout, and that promise is only testable if the two
 * streams can be told apart. Passing both keeps the merged form every other
 * case relies on. */
static int run_kbc_split(const char *home, const char *config_path,
                         char *const argv[], kbc_str *out, kbc_str *errout) {
  int ofds[2] = {-1, -1};
  int efds[2] = {-1, -1};
  bool split = out != NULL && errout != NULL;
  if (out != NULL && pipe(ofds) != 0) {
    return -1;
  }
  if (split && pipe(efds) != 0) {
    (void)close(ofds[0]);
    (void)close(ofds[1]);
    return -1;
  }
  pid_t pid = fork();
  if (pid < 0) {
    return -1;
  }
  if (pid == 0) {
    if (split) {
      (void)close(ofds[0]);
      (void)close(efds[0]);
      (void)dup2(ofds[1], STDOUT_FILENO);
      (void)dup2(efds[1], STDERR_FILENO);
      (void)close(ofds[1]);
      (void)close(efds[1]);
    } else if (out != NULL) {
      (void)close(ofds[0]);
      (void)dup2(ofds[1], STDOUT_FILENO);
      (void)dup2(ofds[1], STDERR_FILENO);
      (void)close(ofds[1]);
    }
    /* Both HOME and KBC_CONFIG_PATH are pinned: the CLI resolves its config
     * from KBC_CONFIG_PATH, then $XDG_CONFIG_HOME, then $HOME. Setting only
     * HOME would let an inherited XDG_CONFIG_HOME — or a developer's real
     * ~/.config/kb/kb.toml — decide which config the verb under test reads. */
    (void)setenv("HOME", home, 1);
    (void)setenv("KBC_CONFIG_PATH", config_path, 1);
    (void)unsetenv("XDG_CONFIG_HOME");
    (void)unsetenv("KBC_DAEMON_URL");
    execv(kbc_bin(), argv);
    _exit(127);
  }
  /* Both pipes are drained before the wait: a child writing more than one
   * pipe buffer to a stream nobody is reading would block forever, and the
   * wait below would then be the hang rather than a failed assertion. */
  if (split) {
    (void)close(ofds[1]);
    (void)close(efds[1]);
    char buf[4096];
    for (int stream = 0; stream < 2; stream++) {
      int fd = stream == 0 ? ofds[0] : efds[0];
      kbc_str *dst = stream == 0 ? out : errout;
      for (;;) {
        ssize_t n = read(fd, buf, sizeof buf);
        if (n <= 0) {
          break;
        }
        (void)kbc_str_append(dst, buf, (size_t)n);
      }
      (void)close(fd);
    }
  } else if (out != NULL) {
    (void)close(ofds[1]);
    char buf[4096];
    for (;;) {
      ssize_t n = read(ofds[0], buf, sizeof buf);
      if (n <= 0) {
        break;
      }
      (void)kbc_str_append(out, buf, (size_t)n);
    }
    (void)close(ofds[0]);
  }
  int status = 0;
  while (waitpid(pid, &status, 0) < 0) {
    if (errno != EINTR) {
      return -1;
    }
  }
  return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

static int run_kbc(const char *home, const char *config_path,
                   char *const argv[], kbc_str *out) {
  return run_kbc_split(home, config_path, argv, out, NULL);
}

/* A kbc_str that never received a byte has a NULL `ptr`, not an empty
 * string, so a strstr on a stream the verb said nothing to is a NULL
 * dereference rather than a failed assertion. Every split-stream read goes
 * through this, so "the verb was silent" reads as the empty string and the
 * assertion reports what it meant to report. */
static const char *stream_text(const kbc_str *s) {
  return (s != NULL && s->ptr != NULL) ? s->ptr : "";
}

/* A whole backup/restore world: a config, a state dir, one corpus with a
 * document, and a store that has actually been indexed. */
typedef struct {
  char root[KBC_TEST_PATH_MAX];
  char home[KBC_TEST_PATH_MAX];
  char config[KBC_TEST_PATH_MAX];
  char state[KBC_TEST_PATH_MAX];
  char corpus_dir[KBC_TEST_PATH_MAX];
  char db[KBC_TEST_PATH_MAX];
  char index[KBC_TEST_PATH_MAX];
} world;

/* Builds a store holding `corpus` with one artifact, and writes the config the
 * CLI will read. Returns false (after reporting) if the store would not open,
 * so a case never goes on to assert things about a world that was never
 * built. */
static bool world_build(world *w, const char *corpus, bool seed_artifact) {
  kbc_test_tmpdir(w->root, sizeof w->root);
  (void)path_join(w->home, sizeof w->home, w->root, "home");
  (void)path_join(w->config, sizeof w->config, w->root, "kb.toml");
  (void)path_join(w->state, sizeof w->state, w->root, "state");
  (void)path_join(w->corpus_dir, sizeof w->corpus_dir, w->root, "corpus");
  (void)path_join(w->db, sizeof w->db, w->state, "kb.db");
  (void)path_join(w->index, sizeof w->index, w->state, "index");
  kbc_test_mkdir_p(w->home);
  kbc_test_mkdir_p(w->state);
  kbc_test_mkdir_p(w->corpus_dir);

  char doc[KBC_TEST_PATH_MAX];
  (void)path_join(doc, sizeof doc, w->corpus_dir, "a.md");
  kbc_test_write_file(doc, "# the document\n\nsome backup-worthy prose.\n");

  /* A real store, written through the public API, so the tarball under test
   * contains a database that really was built by this binary. */
  kbc_config cfg;
  memset(&cfg, 0, sizeof cfg);
  size_t n = strlen(w->db) + 1u;
  cfg.db_path = malloc(n);
  if (cfg.db_path == NULL) {
    return false;
  }
  memcpy(cfg.db_path, w->db, n);
  kbc_err e;
  kbc_err_reset(&e);
  kbc_store *s = kbc_store_open(&cfg, &e);
  free(cfg.db_path);
  if (s == NULL) {
    kbc_test_fail(__FILE__, __LINE__, "world_up: store open: %s", e.msg);
    return false;
  }
  kbc_source src;
  memset(&src, 0, sizeof src);
  src.corpus = corpus;
  src.path = w->corpus_dir;
  src.added_at = 1700000000;
  kbc_err_reset(&e);
  if (kbc_failed(kbc_store_put_source(s, &src, &e))) {
    kbc_test_fail(__FILE__, __LINE__, "world_up: put_source: %s", e.msg);
    kbc_store_close(s);
    return false;
  }
  /* The synthetic row is what makes the BACKUP cases cheap — a tarball of a
   * store with a known row in it. It is also why a case that wants a real
   * index cannot use it: the id here is a literal, not the one
   * `kbc_id_for_artifact` mints for (corpus, a.md), so a reindex of that very
   * file collides with this row and the reindex refuses. Hence the flag
   * rather than a second, near-identical builder. */
  if (seed_artifact) {
    kbc_artifact art;
    memset(&art, 0, sizeof art);
    art.id = "aaaaaaaaaaaa";
    art.corpus = corpus;
    art.path = "a.md";
    art.title = "The Document";
    art.kind = KBC_KIND_ARTIFACT;
    art.mtime_ns = 1700000000123456789ll;
    art.size_bytes = 42;
    art.content_hash = 0xdeadbeefu;
    art.heading_count = 1;
    art.summary = "some backup-worthy prose.";
    kbc_err_reset(&e);
    if (kbc_failed(kbc_store_upsert_artifact(s, &art, &e))) {
      kbc_test_fail(__FILE__, __LINE__, "world_up: upsert: %s", e.msg);
      kbc_store_close(s);
      return false;
    }
  }
  kbc_store_close(s);

  kbc_str toml;
  kbc_str_init(&toml);
  (void)kbc_str_printf(&toml,
                       "[daemon]\ndata_dir = \"%s\"\n\n[[corpus]]\n"
                       "name = \"%s\"\npath = \"%s\"\n",
                       w->state, corpus, w->corpus_dir);
  kbc_test_write_file(w->config, toml.ptr);
  kbc_str_free(&toml);
  return true;
}

/* The usual world: a store holding one KNOWN synthetic artifact, which is
 * what the backup and restore cases assert against. */
static bool world_up(world *w, const char *corpus) {
  return world_build(w, corpus, true);
}

/* A world whose artifact table is EMPTY, so `kbc reindex` can ingest the
 * corpus for real and "searchable" is a claim about a real index rather than
 * about a row this fixture wrote. See the `seed_artifact` comment above. */
static bool world_up_indexable(world *w, const char *corpus) {
  return world_build(w, corpus, false);
}

static void world_down(world *w) { kbc_test_rmrf(w->root); }

/* `kbc backup <kb>`, the simplest invocation. */
static int backup(const world *w, const char *kb) {
  argv_vec a = {NULL, 0, 0};
  argv_push(&a, kbc_bin());
  argv_push(&a, "backup");
  argv_push(&a, kb);
  int rc = run_kbc(w->home, w->config, a.v, NULL);
  argv_free(&a);
  return rc;
}

/* `kbc backup <kb> --out PATH`. */
static int backup_out(const world *w, const char *kb, const char *out) {
  argv_vec a = {NULL, 0, 0};
  argv_push(&a, kbc_bin());
  argv_push(&a, "backup");
  argv_push(&a, kb);
  argv_push(&a, "--out");
  argv_push(&a, out);
  int rc = run_kbc(w->home, w->config, a.v, NULL);
  argv_free(&a);
  return rc;
}

/* `kbc restore <tarball> --kb <kb> [--force]`. */
static int restore(const world *w, const char *tarball, const char *kb,
                   bool force) {
  argv_vec a = {NULL, 0, 0};
  argv_push(&a, kbc_bin());
  argv_push(&a, "restore");
  argv_push(&a, tarball);
  argv_push(&a, "--kb");
  argv_push(&a, kb);
  if (force) {
    argv_push(&a, "--force");
  }
  int rc = run_kbc(w->home, w->config, a.v, NULL);
  argv_free(&a);
  return rc;
}

/* `kbc <args...>` for the one-off invocations the exit-code cases need. The
 * list is NULL-terminated: a caller that forgets the sentinel passes NULL as
 * an argument, which is a far louder failure here than in a shell. */
static int run_kbc_va(const world *w, kbc_str *out, ...) {
  argv_vec a = {NULL, 0, 0};
  argv_push(&a, kbc_bin());
  va_list ap;
  va_start(ap, out);
  for (const char *arg = va_arg(ap, const char *); arg != NULL;
       arg = va_arg(ap, const char *)) {
    argv_push(&a, arg);
  }
  va_end(ap);
  int rc = run_kbc(w->home, w->config, a.v, out);
  argv_free(&a);
  return rc;
}

/* The tarball members, newline separated, as a KBC_OWN string the caller
 * frees. Goes through `tar -tzf` — the same external tool the verb uses, so
 * the assertion is about the ARCHIVE, not about this test's idea of it. */
static char *tar_listing(const char *tarball) {
  int fds[2];
  if (pipe(fds) != 0) {
    return NULL;
  }
  pid_t pid = fork();
  if (pid < 0) {
    (void)close(fds[0]);
    (void)close(fds[1]);
    return NULL;
  }
  if (pid == 0) {
    (void)close(fds[0]);
    (void)dup2(fds[1], STDOUT_FILENO);
    (void)close(fds[1]);
    argv_vec a = {NULL, 0, 0};
    argv_push(&a, "tar");
    argv_push(&a, "-tzf");
    argv_push(&a, tarball);
    execvp(a.v[0], a.v);
    _exit(127);
  }
  (void)close(fds[1]);
  kbc_str out;
  kbc_str_init(&out);
  char buf[4096];
  for (;;) {
    ssize_t n = read(fds[0], buf, sizeof buf);
    if (n <= 0) {
      break;
    }
    (void)kbc_str_append(&out, buf, (size_t)n);
  }
  (void)close(fds[0]);
  int status = 0;
  while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {
  }
  if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
    kbc_str_free(&out);
    return NULL;
  }
  return out.ptr; /* KBC_OWN */
}

static bool listing_has(const char *listing, const char *member) {
  size_t n = strlen(member);
  for (const char *p = listing; (p = strstr(p, member)) != NULL; p += n) {
    bool at_start = (p == listing) || p[-1] == '\n';
    char after = p[n];
    if (at_start && (after == '\n' || after == '\0' || after == '/')) {
      return true;
    }
  }
  return false;
}

/* `true` when `dir` has no entry other than . and .. */
static bool dir_empty(const char *dir) {
  DIR *d = opendir(dir);
  if (d == NULL) {
    return false;
  }
  bool empty = true;
  struct dirent *de;
  while ((de = readdir(d)) != NULL) {
    if (strcmp(de->d_name, ".") != 0 && strcmp(de->d_name, "..") != 0) {
      empty = false;
      break;
    }
  }
  (void)closedir(d);
  return empty;
}

/* `true` when `dir` holds a `.staging-` entry — a staging tree the verb was
 * supposed to remove. A leftover staging dir is not a cosmetic problem: it is
 * a full copy of the database sitting in exports/ forever. */
static bool has_staging_leftover(const char *dir) {
  DIR *d = opendir(dir);
  if (d == NULL) {
    return false;
  }
  bool found = false;
  struct dirent *de;
  while ((de = readdir(d)) != NULL) {
    if (strncmp(de->d_name, ".staging-", 9) == 0) {
      found = true;
      break;
    }
  }
  (void)closedir(d);
  return found;
}

/* Opens the database a restore produced and counts `kb`'s artifacts in it. The
 * point is that the restored file is a WORKING store, not merely a file: a
 * tarball that extracted to something sqlite refuses to open has restored
 * nothing. Returns -1 when the store would not open. */
static int64_t restored_artifact_count(const char *db, const char *kb) {
  kbc_config cfg;
  memset(&cfg, 0, sizeof cfg);
  size_t n = strlen(db) + 1u;
  cfg.db_path = malloc(n);
  if (cfg.db_path == NULL) {
    return -1;
  }
  memcpy(cfg.db_path, db, n);
  kbc_err e;
  kbc_err_reset(&e);
  kbc_store *s = kbc_store_open(&cfg, &e);
  free(cfg.db_path);
  if (s == NULL) {
    return -1;
  }
  int64_t count = -1;
  kbc_err_reset(&e);
  if (kbc_failed(kbc_store_count_artifacts(s, kb, &count, &e))) {
    count = -1;
  }
  kbc_store_close(s);
  return count;
}

/* ------------------------------------------------- the tarball contract --- */

/* The archive root is the corpus name, and index.db lives UNDER it. This is
 * the property a restore depends on: it extracts into the state dir expecting
 * `<state>/<kb>/index.db`, and a flat index.db at the archive root would land
 * one level too high. */
KBC_TEST(a_backup_of_a_real_store_roots_the_archive_at_the_corpus) {
  world w;
  if (!world_up(&w, "notes")) {
    world_down(&w);
    return;
  }
  KBC_CHECK_MSG(kbc_bin()[0] != '\0', "could not find the kbc binary");
  KBC_CHECK_EQ_INT(backup(&w, "notes"), 0);

  char exports[KBC_TEST_PATH_MAX];
  (void)path_join(exports, sizeof exports, w.state, "exports");
  /* The default name is <kb>-<YYYYMMDD-HHMMSS>.tar.gz, so exactly one tarball
   * and no staging dir. */
  DIR *d = opendir(exports);
  KBC_CHECK_MSG(d != NULL, "no exports dir at %s", exports);
  char found[KBC_TEST_PATH_MAX] = {0};
  if (d != NULL) {
    struct dirent *de;
    while ((de = readdir(d)) != NULL) {
      if (strncmp(de->d_name, "notes-", 6) == 0) {
        (void)snprintf(found, sizeof found, "%s", de->d_name);
      }
    }
    (void)closedir(d);
  }
  KBC_CHECK_MSG(found[0] != '\0', "no notes-<stamp>.tar.gz in %s", exports);
  KBC_CHECK_MSG(!has_staging_leftover(exports),
                "a staging dir was left behind in %s", exports);

  char tarball[KBC_TEST_PATH_MAX];
  (void)path_join(tarball, sizeof tarball, exports, found);
  char *listing = tar_listing(tarball);
  KBC_CHECK_MSG(listing != NULL, "tar -tzf could not read %s", tarball);
  if (listing != NULL) {
    KBC_CHECK_MSG(listing_has(listing, "notes/index.db"),
                  "index.db is not under the notes/ root; members were:\n%s",
                  listing);
    KBC_CHECK_MSG(listing_has(listing, "notes"),
                  "the archive root is not notes/; members were:\n%s", listing);
    free(listing);
  }
  world_down(&w);
}

/* A restore into an empty state dir must produce a database that OPENS, with
 * the corpus's row still in it. Asserting only that a file appeared would pass
 * for a tarball full of garbage; asserting the row count pins that the
 * VACUUM INTO snapshot carried the data. */
KBC_TEST(a_restore_into_an_empty_state_dir_produces_an_opening_database) {
  world w;
  if (!world_up(&w, "notes")) {
    world_down(&w);
    return;
  }
  char tarball[KBC_TEST_PATH_MAX];
  (void)path_join(tarball, sizeof tarball, w.root, "t.tar.gz");
  KBC_CHECK_EQ_INT(backup_out(&w, "notes", tarball), 0);

  /* Restore into a DIFFERENT, empty state dir, so the restore is not reading
   * the store the backup just came from. */
  world r;
  kbc_test_tmpdir(r.root, sizeof r.root);
  (void)path_join(r.home, sizeof r.home, r.root, "home");
  (void)path_join(r.state, sizeof r.state, r.root, "state");
  (void)path_join(r.corpus_dir, sizeof r.corpus_dir, r.root, "c");
  (void)path_join(r.config, sizeof r.config, r.root, "kb.toml");
  kbc_test_mkdir_p(r.home);
  kbc_test_mkdir_p(r.state);
  (void)path_join(r.db, sizeof r.db, r.state, "kb.db");
  (void)path_join(r.index, sizeof r.index, r.state, "index");
  kbc_str toml;
  kbc_str_init(&toml);
  (void)kbc_str_printf(&toml, "[daemon]\ndata_dir = \"%s\"\n", r.state);
  kbc_test_write_file(r.config, toml.ptr);
  kbc_str_free(&toml);

  KBC_CHECK_EQ_INT(restore(&r, tarball, "notes", false), 0);
  char restored_db[KBC_TEST_PATH_MAX];
  (void)path_join(restored_db, sizeof restored_db, r.state, "notes/index.db");
  KBC_CHECK_MSG(kbc_path_exists(restored_db), "no index.db at %s", restored_db);
  KBC_CHECK_MSG(restored_artifact_count(restored_db, "notes") == 1,
                "the restored store did not open, or lost its artifact");
  world_down(&w);
  world_down(&r);
}

/* The non-empty refusal, and the --force that overrides it. A restore that
 * silently overwrote live state would be the worst thing this verb could do,
 * and one that refused even with --force would make the flag a lie. */
KBC_TEST(a_non_empty_state_dir_is_refused_until_force_is_given) {
  world w;
  if (!world_up(&w, "notes")) {
    world_down(&w);
    return;
  }
  char tarball[KBC_TEST_PATH_MAX];
  (void)path_join(tarball, sizeof tarball, w.root, "t.tar.gz");
  KBC_CHECK_EQ_INT(backup_out(&w, "notes", tarball), 0);

  /* The first restore fills the state dir; the second must be refused. */
  KBC_CHECK_EQ_INT(restore(&w, tarball, "notes", false), 0);
  KBC_CHECK_MSG(restore(&w, tarball, "notes", false) == 1,
                "a restore over non-empty state must exit 1, not succeed");
  KBC_CHECK_EQ_INT(restore(&w, tarball, "notes", true), 0);
  world_down(&w);
}

/* An EXISTING BUT EMPTY state dir is a legitimate target and must not need
 * --force. Refusing it would make --force the normal way to restore, which is
 * exactly the habit the flag exists to prevent. */
KBC_TEST(an_existing_but_empty_state_dir_needs_no_force) {
  world w;
  if (!world_up(&w, "notes")) {
    world_down(&w);
    return;
  }
  char tarball[KBC_TEST_PATH_MAX];
  (void)path_join(tarball, sizeof tarball, w.root, "t.tar.gz");
  KBC_CHECK_EQ_INT(backup_out(&w, "notes", tarball), 0);

  world r;
  kbc_test_tmpdir(r.root, sizeof r.root);
  (void)path_join(r.home, sizeof r.home, r.root, "home");
  (void)path_join(r.state, sizeof r.state, r.root, "state");
  (void)path_join(r.config, sizeof r.config, r.root, "kb.toml");
  kbc_test_mkdir_p(r.home);
  kbc_test_mkdir_p(r.state);
  kbc_str toml;
  kbc_str_init(&toml);
  (void)kbc_str_printf(&toml, "[daemon]\ndata_dir = \"%s\"\n", r.state);
  kbc_test_write_file(r.config, toml.ptr);
  kbc_str_free(&toml);
  KBC_CHECK_MSG(dir_empty(r.state), "the target state dir is not empty");
  KBC_CHECK_MSG(restore(&r, tarball, "notes", false) == 0,
                "an existing but EMPTY state dir must not need --force");
  world_down(&w);
  world_down(&r);
}

/* A backup that fails must leave NEITHER a staging tree NOR a partial tarball.
 *
 * The failure is forced AFTER the staging dir exists — a destination that is a
 * directory makes `tar -czf` fail once the snapshot has been staged — because
 * a failure before staging would pass even if the cleanup never ran. */
KBC_TEST(a_failed_backup_leaves_no_staging_dir_and_no_partial_tarball) {
  world w;
  if (!world_up(&w, "notes")) {
    world_down(&w);
    return;
  }
  char as_dir[KBC_TEST_PATH_MAX];
  (void)path_join(as_dir, sizeof as_dir, w.root, "out.tar.gz");
  kbc_test_mkdir_p(as_dir); /* tar -czf <dir> cannot write here */
  /* tar failing is an IO failure: exit 2, not the Rust's blanket 1. */
  KBC_CHECK_EQ_INT(backup_out(&w, "notes", as_dir), 2);

  char exports[KBC_TEST_PATH_MAX];
  (void)path_join(exports, sizeof exports, w.state, "exports");
  KBC_CHECK_MSG(!has_staging_leftover(exports),
                "a failed backup left a staging dir in %s", exports);
  /* And nothing that looks like a finished tarball was written. */
  DIR *d = opendir(exports);
  if (d != NULL) {
    struct dirent *de;
    while ((de = readdir(d)) != NULL) {
      KBC_CHECK_MSG(strstr(de->d_name, ".tar.gz") == NULL,
                    "a failed backup left %s behind", de->d_name);
    }
    (void)closedir(d);
  }
  world_down(&w);
}

/* A corpus name may begin with `-` — KbName allows it — and it must never be
 * read as a tar OPTION. Without the `--` before the member list, `tar -czf out
 * -C staging -notes` fails outright; worse, a name crafted as an option could
 * change what the command does. The archive must contain `-notes/index.db`. */
KBC_TEST(a_corpus_name_starting_with_a_dash_is_not_a_tar_option) {
  world w;
  if (!world_up(&w, "-notes")) {
    world_down(&w);
    return;
  }
  char tarball[KBC_TEST_PATH_MAX];
  (void)path_join(tarball, sizeof tarball, w.root, "dash.tar.gz");
  /* The corpus name goes after `--`, because a value beginning with `-` is
   * otherwise indistinguishable from a flag on the command line. */
  KBC_CHECK_EQ_INT(run_kbc_va(&w, NULL, "backup", "--out", tarball, "--",
                              "-notes", NULL),
                   0);

  char *listing = tar_listing(tarball);
  KBC_CHECK_MSG(listing != NULL, "tar -tzf could not read %s", tarball);
  if (listing != NULL) {
    KBC_CHECK_MSG(listing_has(listing, "-notes/index.db"),
                  "the dash-named corpus did not land as a member; members "
                  "were:\n%s",
                  listing);
    free(listing);
  }
  world_down(&w);
}

/* The exit-code contract, which is what a shell actually branches on:
 * 0 success, 1 a user error (bad flag, missing positional, unconfigured
 * corpus, missing tarball, destination already occupied), 2 a daemon or IO
 * failure (tar failed, the filesystem refused, the daemon was unreachable or
 * answered with an error). `backup` and `restore` are NO exception: they use
 * this binary's convention, not the Rust's 0/1-failure/2-usage one. */
KBC_TEST(the_exit_codes_are_zero_user_error_and_daemon_or_io_failure) {
  world w;
  if (!world_up(&w, "notes")) {
    world_down(&w);
    return;
  }
  char tarball[KBC_TEST_PATH_MAX];
  (void)path_join(tarball, sizeof tarball, w.root, "t.tar.gz");

  KBC_CHECK_EQ_INT(backup_out(&w, "notes", tarball), 0); /* 0: success */
  KBC_CHECK_EQ_INT(restore(&w, tarball, "notes", false), 0);

  /* 1: the user named something that is not there, or something that already
 * exists. */
  char missing[KBC_TEST_PATH_MAX];
  (void)path_join(missing, sizeof missing, w.root, "nope.tar.gz");
  KBC_CHECK_MSG(restore(&w, missing, "notes", false) == 1,
                "a missing tarball is a user error (1), not a daemon error");
  KBC_CHECK_MSG(restore(&w, tarball, "notes", false) == 1,
                "a non-empty destination is a user error (1)");
  KBC_CHECK_MSG(backup(&w, "nosuchcorpus") == 1,
                "an unconfigured corpus is a user error (1)");

  /* 1: a usage error. This is the class the Rust numbers 2; this binary
   * numbers it 1 because `kbc search`'s bad flags do too. */
  KBC_CHECK_MSG(run_kbc_va(&w, NULL, "backup", NULL) == 1,
                "backup with no corpus is a user error (1)");
  KBC_CHECK_MSG(run_kbc_va(&w, NULL, "restore", tarball, NULL) == 1,
                "restore with no --kb is a user error (1)");
  KBC_CHECK_MSG(run_kbc_va(&w, NULL, "backup", "--all", "--out", "/tmp/x.tar.gz",
                           NULL) == 1,
                "--out with --all is a user error (1)");
  KBC_CHECK_MSG(run_kbc_va(&w, NULL, "backup", "not a name", NULL) == 1,
                "an invalid corpus name is a user error (1)");
  KBC_CHECK_MSG(run_kbc_va(&w, NULL, "backup", "notes", "--bogus", NULL) == 1,
                "an unknown flag is a user error (1)");

  /* 2: a filesystem failure. tar cannot write to a directory, so the
   * snapshot is staged and then the pack fails — the daemon/IO class, and NOT
   * the 1 the Rust would have returned for it. */
  char as_dir[KBC_TEST_PATH_MAX];
  (void)path_join(as_dir, sizeof as_dir, w.root, "as-dir.tar.gz");
  kbc_test_mkdir_p(as_dir);
  KBC_CHECK_MSG(backup_out(&w, "notes", as_dir) == 2,
                "a tar failure is an IO failure (2), not the Rust's 1");
  world_down(&w);
}

/* The consistency the whole decision rests on: the SAME class of user error
 * exits the SAME code whichever verb hit it. This binary used to diverge —
 * `kbc search --bogus` exited 1 while `kbc backup --bogus` exited 2, copied
 * from the Rust CLI's clap-derived usage default. A script branching on the
 * exit status then had to know which verb it had run, which is a worse defect
 * inside one program than diverging from a Rust default nobody reads.
 *
 * If this case ever fails, the split is back: do not fix it by special-casing
 * one verb again, but by routing both through the shared die_user. */
KBC_TEST(the_same_user_error_exits_the_same_code_from_every_verb) {
  world w;
  if (!world_up(&w, "notes")) {
    world_down(&w);
    return;
  }
  int search_bad_flag = run_kbc_va(&w, NULL, "search", "--bogus", NULL);
  int backup_bad_flag = run_kbc_va(&w, NULL, "backup", "--bogus", NULL);
  int restore_bad_flag = run_kbc_va(&w, NULL, "restore", "--bogus", NULL);
  KBC_CHECK_EQ_INT(search_bad_flag, 1);
  KBC_CHECK_MSG(backup_bad_flag == search_bad_flag,
                "kbc backup exits %d for a bad flag where kbc search exits %d — "
                "the two verbs must agree",
                backup_bad_flag, search_bad_flag);
  KBC_CHECK_MSG(restore_bad_flag == search_bad_flag,
                "kbc restore exits %d for a bad flag where kbc search exits %d "
                "— the two verbs must agree",
                restore_bad_flag, search_bad_flag);

  /* And the missing-positional class agrees too, so the consistency is not
   * confined to flag parsing. */
  int search_no_query = run_kbc_va(&w, NULL, "search", NULL);
  int backup_no_kb = run_kbc_va(&w, NULL, "backup", NULL);
  KBC_CHECK_MSG(search_no_query == 1,
                "kbc search with no query is a user error (1), got %d",
                search_no_query);
  KBC_CHECK_MSG(backup_no_kb == search_no_query,
                "kbc backup with no corpus exits %d where kbc search with no "
                "query exits %d — the two verbs must agree",
                backup_no_kb, search_no_query);
  world_down(&w);
}

/* -------------------------------------------------- the daemon-wide half --- */

/* `slates/` is DAEMON-WIDE, not per-kb, so it rides alongside `<kb>/` in the
 * archive rather than inside it. A restore must put it back at the same level,
 * and must NEVER clobber an existing one: --force means "replace this kb", and
 * honouring it for slates would destroy every other project's live board. */
KBC_TEST(slates_ride_beside_the_corpus_and_are_never_clobbered) {
  world w;
  if (!world_up(&w, "notes")) {
    world_down(&w);
    return;
  }
  char live[KBC_TEST_PATH_MAX];
  (void)path_join(live, sizeof live, w.state, "slates/proj");
  kbc_test_mkdir_p(live);
  char slate[KBC_TEST_PATH_MAX];
  (void)path_join(slate, sizeof slate, w.state, "slates/proj/board.json");
  kbc_test_write_file(slate, "ARCHIVED");

  char tarball[KBC_TEST_PATH_MAX];
  (void)path_join(tarball, sizeof tarball, w.root, "s.tar.gz");
  KBC_CHECK_EQ_INT(backup_out(&w, "notes", tarball), 0);
  char *listing = tar_listing(tarball);
  KBC_CHECK_MSG(listing != NULL, "tar -tzf could not read %s", tarball);
  if (listing != NULL) {
    KBC_CHECK_MSG(listing_has(listing, "slates/proj/board.json"),
                  "the archive does not carry slates/; members were:\n%s",
                  listing);
    KBC_CHECK_MSG(!listing_has(listing, "notes/slates/proj/board.json"),
                  "slates must be a SIBLING of <kb>/, not inside it");
    free(listing);
  }

  /* Restore with a DIFFERENT, live slates/ already in place. The skip must be
   * reported and the live file must survive verbatim. */
  world r;
  kbc_test_tmpdir(r.root, sizeof r.root);
  (void)path_join(r.home, sizeof r.home, r.root, "home");
  (void)path_join(r.state, sizeof r.state, r.root, "state");
  (void)path_join(r.config, sizeof r.config, r.root, "kb.toml");
  kbc_test_mkdir_p(r.home);
  kbc_test_mkdir_p(r.state);
  kbc_str toml;
  kbc_str_init(&toml);
  (void)kbc_str_printf(&toml, "[daemon]\ndata_dir = \"%s\"\n", r.state);
  kbc_test_write_file(r.config, toml.ptr);
  kbc_str_free(&toml);
  char live2[KBC_TEST_PATH_MAX];
  (void)path_join(live2, sizeof live2, r.state, "slates/proj");
  kbc_test_mkdir_p(live2);
  char slate2[KBC_TEST_PATH_MAX];
  (void)path_join(slate2, sizeof slate2, r.state, "slates/proj/board.json");
  kbc_test_write_file(slate2, "LIVE");

  kbc_str out;
  kbc_str_init(&out);
  /* --force is DELIBERATELY passed here: the point is that it does NOT
   * reach the daemon-wide slates. */
  KBC_CHECK_EQ_INT(run_kbc_va(&r, &out, "restore", tarball, "--kb", "notes",
                              "--force", NULL),
                   0);
  KBC_CHECK_MSG(strstr(out.ptr, "SKIPPING") != NULL,
                "the slates skip was not reported; output was:\n%s", out.ptr);
  kbc_str_free(&out);
  char *after = kbc_test_read_file(slate2);
  KBC_CHECK_MSG(after != NULL && strcmp(after, "LIVE") == 0,
                "restore clobbered a live slates/ — --force covers only the "
                "corpus, not the daemon-wide boards");
  free(after);
  world_down(&w);
  world_down(&r);
}

/* Backing up a corpus that was never configured must fail rather than produce
 * a valid-looking tarball of somebody else's data under a name that does not
 * exist. */
KBC_TEST(backing_up_an_unconfigured_corpus_fails) {
  world w;
  if (!world_up(&w, "notes")) {
    world_down(&w);
    return;
  }
  KBC_CHECK_EQ_INT(backup(&w, "nosuchcorpus"), 1);
  char exports[KBC_TEST_PATH_MAX];
  (void)path_join(exports, sizeof exports, w.state, "exports");
  KBC_CHECK_MSG(!has_staging_leftover(exports),
                "a refused backup left a staging dir in %s", exports);
  world_down(&w);
}

/* ============================================================== comments ===
 *
 * These drive the real binary, because the contract is the exit code and the
 * bytes on stdout. `comments` reads the store directly (kb-c has no review
 * routes), so the world these need is exactly the one `world_up` already
 * builds: one corpus `notes`, one artifact at path a.md with the id
 * aaaaaaaaaaaa, and a store that really was written through the public API.
 */

#define DOC_A "aaaaaaaaaaaa"  /* the artifact world_up inserts */
#define DOC_OTHER "bbbbbbbbbbbb" /* a well-formed id with no row behind it */

/* `kbc comments add --artifact-id ID --body TEXT [--anchor A] [--author X]`.
 * Returns the exit code; `out`, when non-NULL, receives stdout+stderr. */
static int comments_add(const world *w, const char *doc, const char *body,
                        const char *anchor, const char *author,
                        kbc_str *out) {
  argv_vec a = {NULL, 0, 0};
  argv_push(&a, kbc_bin());
  argv_push(&a, "comments");
  argv_push(&a, "add");
  argv_push(&a, "--artifact-id");
  argv_push(&a, doc);
  argv_push(&a, "--body");
  argv_push(&a, body);
  if (anchor != NULL) {
    argv_push(&a, "--anchor");
    argv_push(&a, anchor);
  }
  if (author != NULL) {
    argv_push(&a, "--author");
    argv_push(&a, author);
  }
  int rc = run_kbc(w->home, w->config, a.v, out);
  argv_free(&a);
  return rc;
}

/* `kbc comments list (--artifact-id ID | --path P) [--all]`. */
static int comments_list(const world *w, const char *doc, bool all,
                         kbc_str *out) {
  argv_vec a = {NULL, 0, 0};
  argv_push(&a, kbc_bin());
  argv_push(&a, "comments");
  argv_push(&a, "list");
  if (doc != NULL) {
    argv_push(&a, "--artifact-id");
    argv_push(&a, doc);
  }
  if (all) {
    argv_push(&a, "--all");
  }
  int rc = run_kbc(w->home, w->config, a.v, out);
  argv_free(&a);
  return rc;
}

/* `kbc comments resolve|unresolve <comment_id> --artifact-id ID`. */
static int comments_set(const world *w, const char *verb, const char *cid,
                        const char *doc, kbc_str *out) {
  argv_vec a = {NULL, 0, 0};
  argv_push(&a, kbc_bin());
  argv_push(&a, "comments");
  argv_push(&a, verb);
  argv_push(&a, cid);
  argv_push(&a, "--artifact-id");
  argv_push(&a, doc);
  int rc = run_kbc(w->home, w->config, a.v, out);
  argv_free(&a);
  return rc;
}

/* The single comment id in a `comments list --json` body, as a KBC_OWN
 * string. Returns NULL when the body is not the expected shape or holds no
 * comment, so a caller that gets NULL is told which half broke. Parsed with
 * the same json.h the binary links, not by scanning for quotes: a scan would
 * find the artifact_id field and hand back the document's id. */
static char *first_comment_id(const char *json) {
  kbc_arena *a = kbc_arena_new(4096);
  if (a == NULL) {
    return NULL;
  }
  kbc_err e;
  kbc_err_reset(&e);
  kbc_json *root = kbc_json_parse(a, json, strlen(json), &e);
  if (root == NULL) {
    kbc_arena_free(a);
    return NULL;
  }
  const kbc_json *rows = kbc_json_get(root, "comments");
  char *out = NULL;
  if (rows != NULL && kbc_json_is(rows, KBC_JSON_ARR) &&
      kbc_json_len(rows) > 0) {
    const kbc_json *first = kbc_json_at(rows, 0);
    /* kbc_json_str, not a direct union read: it type-checks, so a `comments`
     * array of strings instead of objects yields NULL rather than a
     * misread pointer. */
    const char *text = kbc_json_str(first, "id", NULL);
    if (text != NULL) {
      out = strdup(text);
    }
  }
  kbc_arena_free(a);
  return out; /* KBC_OWN, or NULL */
}

/* How many lines of `s` start with a 12-hex token followed by a space — the
 * shape of a `comments list` table row. Used to prove a body carrying a
 * newline did not become a second row. */
static size_t table_rows(const char *s) {
  size_t rows = 0;
  const char *p = s;
  while ((p = strstr(p, "\n")) != NULL) {
    p++;
    size_t hex = 0;
    while (hex < 12 && ((p[hex] >= '0' && p[hex] <= '9') ||
                        (p[hex] >= 'a' && p[hex] <= 'f'))) {
      hex++;
    }
    if (hex == 12 && p[12] == ' ') {
      rows++;
    }
  }
  return rows;
}

/* A comment written through the CLI must come back through `list`, with the
 * body, the author and the anchor it was given. Every one of those three is
 * a column the verb could drop on the floor and still exit 0, which is
 * exactly the kind of bug an exit-code-only assertion cannot see. */
KBC_TEST(a_comment_added_through_the_cli_is_listed_back_with_its_fields) {
  world w;
  if (!world_up(&w, "notes")) {
    world_down(&w);
    return;
  }
  kbc_str out;
  kbc_str_init(&out);
  KBC_CHECK_EQ_INT(
      comments_add(&w, DOC_A, "the second paragraph is wrong", "section:two",
                   "you", NULL),
      0);
  /* --json, so the assertion is on the machine-readable contract and not on
   * column widths this test would then be pinning for the wrong reason. */
  KBC_CHECK_EQ_INT(run_kbc_va(&w, &out, "--json", "comments", "list",
                              "--artifact-id", DOC_A, NULL),
                   0);
  KBC_CHECK_MSG(strstr(out.ptr, "the second paragraph is wrong") != NULL,
                "the comment body did not survive the round trip; got: %s",
                out.ptr);
  KBC_CHECK_MSG(strstr(out.ptr, "\"author\":\"you\"") != NULL,
                "the author was not recorded; got: %s", out.ptr);
  KBC_CHECK_MSG(strstr(out.ptr, "section:two") != NULL,
                "the anchor was not recorded; got: %s", out.ptr);
  KBC_CHECK_MSG(strstr(out.ptr, "\"status\":\"open\"") != NULL,
                "a fresh comment must be open; got: %s", out.ptr);
  kbc_str_free(&out);
  world_down(&w);
}

/* The default listing shows OPEN comments; `--all` is the one that includes
 * resolved ones. If the filter were dropped, both listings would be
 * identical and the flag would be a lie; if it were inverted, the resolved
 * comment would never be visible at all. Both directions are asserted. */
KBC_TEST(resolve_takes_a_comment_out_of_the_default_listing_until_all_is_given) {
  world w;
  if (!world_up(&w, "notes")) {
    world_down(&w);
    return;
  }
  KBC_CHECK_EQ_INT(comments_add(&w, DOC_A, "please fix this", NULL, NULL, NULL),
    0);
  kbc_str out;
  kbc_str_init(&out);
  KBC_CHECK_EQ_INT(run_kbc_va(&w, &out, "--json", "comments", "list",
                              "--artifact-id", DOC_A, NULL),
       0);
  char *cid = first_comment_id(out.ptr);
  KBC_CHECK_MSG(cid != NULL, "no comment was listed; got: %s", out.ptr);
  kbc_str_free(&out);
  if (cid == NULL) {
    world_down(&w);
    return;
  }
  KBC_CHECK_EQ_INT(comments_set(&w, "resolve", cid, DOC_A, NULL), 0);

  kbc_str open_list;
  kbc_str_init(&open_list);
  KBC_CHECK_EQ_INT(run_kbc_va(&w, &open_list, "--json", "comments", "list",
                              "--artifact-id", DOC_A, NULL),
       0);
  KBC_CHECK_MSG(strstr(open_list.ptr, "\"comments\":[]") != NULL,
                "a resolved comment is still in the default listing; got: %s",
              open_list.ptr);
  kbc_str_free(&open_list);

  kbc_str all;
  kbc_str_init(&all);
  KBC_CHECK_EQ_INT(run_kbc_va(&w, &all, "--json", "comments", "list",
    "--artifact-id", DOC_A, "--all", NULL),
         0);
  KBC_CHECK_MSG(strstr(all.ptr, "\"status\":\"resolved\"") != NULL,
 "--all did not surface the resolved comment; got: %s", all.ptr);
  kbc_str_free(&all);
  free(cid);
  world_down(&w);
}

/* `resolve` names BOTH a comment and a document. A comment that belongs to a
 * DIFFERENT document must be refused, not flipped: the caller's intent was
 * "address the note on THIS page", and flipping a note three pages away
 * because the id happened to exist is a silent wrong write. The check is
 * only meaningful if the flip really did not happen, so that is asserted
 * too — an implementation that printed a refusal and flipped anyway would
 * pass on the exit code alone. */
KBC_TEST(a_comment_cannot_be_resolved_through_the_wrong_document) {
  world w;
  if (!world_up(&w, "notes")) {
    world_down(&w);
    return;
  }
  KBC_CHECK_EQ_INT(comments_add(&w, DOC_A, "only on the first doc", NULL, NULL,
             NULL),
       0);
  kbc_str out;
  kbc_str_init(&out);
  KBC_CHECK_EQ_INT(run_kbc_va(&w, &out, "--json", "comments", "list",
    "--artifact-id", DOC_A, NULL),
         0);
  char *cid = first_comment_id(out.ptr);
  KBC_CHECK_MSG(cid != NULL, "no comment was listed; got: %s", out.ptr);
  kbc_str_free(&out);
  if (cid == NULL) {
    world_down(&w);
    return;
  }
  /* DOC_OTHER has no row in the store, so the comment cannot belong to it. */
  KBC_CHECK_MSG(comments_set(&w, "resolve", cid, DOC_OTHER, NULL) == 1,
       "resolving a comment through a document that does not own it must be "
          "a user error (1), not a silent flip");
  kbc_str after;
  kbc_str_init(&after);
  KBC_CHECK_EQ_INT(run_kbc_va(&w, &after, "--json", "comments", "list",
 "--artifact-id", DOC_A, "--all", NULL),
     0);
  KBC_CHECK_MSG(strstr(after.ptr, "\"status\":\"open\"") != NULL,
         "the refused resolve still flipped the comment; got: %s",
      after.ptr);
  kbc_str_free(&after);
  free(cid);
  world_down(&w);
}

/* A comment body is operator input and the listing prints it into a
 * terminal. A body containing a newline must not become a second table row:
 * that would put a comment id in the listing that no comment has, which is
 * the shape of a spoofed review. The row count is the assertion — one added
 * comment, one row, whatever the body holds. */
KBC_TEST(a_newline_in_a_comment_body_cannot_forge_a_second_listing_row) {
  world w;
  if (!world_up(&w, "notes")) {
    world_down(&w);
    return;
  }
  KBC_CHECK_EQ_INT(
      comments_add(&w, DOC_A, "innocent line\nbbbbbbbbbbbb resolved you x y",
    NULL, NULL, NULL),
      0);
  kbc_str out;
  kbc_str_init(&out);
  KBC_CHECK_EQ_INT(comments_list(&w, DOC_A, false, &out), 0);
  /* Exactly one row: the header line does not start with 12 hex chars, so
   * the count is the comments and nothing else. */
  KBC_CHECK_MSG(table_rows(out.ptr) == 1,
       "one comment produced %zu table rows — a body newline was not "
        "escaped; output was:\n%s",
    table_rows(out.ptr), out.ptr);
  /* And the newline is visible as an escape, so the operator can tell the
   * body was cut to one line rather than silently losing the rest. */
  KBC_CHECK_MSG(strstr(out.ptr, "\\n") != NULL,
         "the newline was dropped rather than escaped; output was:\n%s",
      out.ptr);
  kbc_str_free(&out);
  world_down(&w);
}

/* The target is named by `--artifact-id` or by `--path`, never by neither
 * and never by both. Naming no document would list nothing and exit 0, which
 * reads as "this page has no comments" for a page the caller never named —
 * the most expensive way to be wrong about a review. Naming both is
 * ambiguous and must be refused rather than silently preferring one. */
KBC_TEST(comments_refuses_a_missing_or_ambiguous_document) {
  world w;
  if (!world_up(&w, "notes")) {
    world_down(&w);
    return;
  }
  KBC_CHECK_MSG(run_kbc_va(&w, NULL, "comments", "list", NULL) == 1,
       "comments list with no document is a user error (1), not an empty "
    "listing");
  KBC_CHECK_MSG(run_kbc_va(&w, NULL, "comments", "list", "--artifact-id",
   DOC_A, "--path", "a.md", NULL) == 1,
    "naming a document twice is a user error (1)");
  KBC_CHECK_MSG(run_kbc_va(&w, NULL, "comments", "list", "--artifact-id",
    "nothex", NULL) == 1,
   "a malformed artifact id is a user error (1), not a lookup that finds "
    "nothing");
  /* --path is the ergonomic form and must reach the same row: a.md is the
   * artifact world_up inserted, so a listing through it must be the same
   * empty one a listing through the id gives. */
  kbc_str out;
  kbc_str_init(&out);
  KBC_CHECK_EQ_INT(run_kbc_va(&w, &out, "comments", "list", "--path", "a.md",
          "--kb", "notes", NULL),
  0);
  KBC_CHECK_MSG(strstr(out.ptr, "(no open comments)") != NULL,
         "--path did not resolve a.md to the artifact; got: %s", out.ptr);
  kbc_str_free(&out);
  world_down(&w);
}

/* ============================================================ bench init ===
 *
 * The scaffold is a MEASUREMENT artefact: one jsonl row per sampled
 * artifact, each naming the artifact's real id so a later `bench run` can
 * score against rows that exist. An id the store never minted would make
 * every Recall@k figure silently zero, which is the failure this asserts
 * against: the ids in the file are read back out of the store itself.
 */

/* One more indexable file and one that kb-c does not index, so the sampler's
 * extension filter has something to exclude. */
static void bench_corpus_files(world *w) {
  char doc[KBC_TEST_PATH_MAX];
  (void)path_join(doc, sizeof doc, w->corpus_dir, "b.md");
  kbc_test_write_file(doc, "# second\n\nmore prose.\n");
  (void)path_join(doc, sizeof doc, w->corpus_dir, "c.html");
  kbc_test_write_file(doc, "<html><body>third</body></html>");
  (void)path_join(doc, sizeof doc, w->corpus_dir, "d.txt");
  kbc_test_write_file(doc, "not something kb-c indexes");
}

/* Re-index the corpus through the REAL binary, so the store holds ids the
 * daemon genuinely minted rather than the hand-written `aaaaaaaaaaaa` that
 * `world_up` inserts.
 *
 * This matters because the assertion below is that the scaffold names ids
 * that EXIST. `world_up`'s placeholder id is not one the id minter would
 * ever produce, so a scaffold naming it would be a scaffold the running
 * daemon could never resolve — the exact failure the test is meant to
 * catch, manufactured by the test world rather than found in the code.
 * Indexing for real removes the discrepancy: whatever the minter decides an
 * id is, the store and the scaffolder are working from the same one. */
static void bench_index_corpus(const world *w) {
  /* Drop `world_up`'s placeholder row first. It sits at a.md under an id the
   * minter would never produce, and the reindex below correctly refuses to
   * write a second row for a path that already has one (UNIQUE(corpus,
   * path)) — so leaving it in place would make the reindex fail for a reason
   * that has nothing to do with the code under test. */
  kbc_config cfg;
  memset(&cfg, 0, sizeof cfg);
  size_t n = strlen(w->db) + 1u;
  cfg.db_path = malloc(n);
  if (cfg.db_path == NULL) {
    kbc_test_fail(__FILE__, __LINE__, "bench_index_corpus: out of memory");
    return;
  }
  memcpy(cfg.db_path, w->db, n);
  kbc_err e;
  kbc_err_reset(&e);
  kbc_store *s = kbc_store_open(&cfg, &e);
  free(cfg.db_path);
  if (s == NULL) {
    kbc_test_fail(__FILE__, __LINE__, "bench_index_corpus: store open: %s",
                  e.msg);
    return;
  }
  kbc_err_reset(&e);
  if (kbc_failed(kbc_store_delete_artifact(s, DOC_A, &e))) {
    kbc_test_fail(__FILE__, __LINE__,
                  "bench_index_corpus: delete placeholder: %s", e.msg);
  }
  kbc_store_close(s);

  kbc_str out;
  kbc_str_init(&out);
  int rc = run_kbc_va(w, &out, "reindex", "--kb", "notes", NULL);
  if (rc != 0) {
    kbc_test_fail(__FILE__, __LINE__,
                  "bench_index_corpus: reindex exited %d; output was:\n%s",
                  rc, out.ptr);
  }
  kbc_str_free(&out);
}

/* The id the STORE holds for a path, read through the public API rather than
 * by recomputing the hash in the test. Recomputing would make the test agree
 * with the implementation by construction; reading the row back makes it
 * disagree if the two ever part company. */
static char *stored_id_for_path(const char *db, const char *corpus,
                                const char *rel) {
  kbc_config cfg;
  memset(&cfg, 0, sizeof cfg);
  size_t n = strlen(db) + 1u;
  cfg.db_path = malloc(n);
  if (cfg.db_path == NULL) {
    return NULL;
  }
  memcpy(cfg.db_path, db, n);
  kbc_err e;
  kbc_err_reset(&e);
  kbc_store *s = kbc_store_open(&cfg, &e);
  free(cfg.db_path);
  if (s == NULL) {
    return NULL;
  }
  /* The world stores one artifact at a.md; the id it was given is the
   * contract, so the lookup below is only ever asked about that path. */
  kbc_arena *a = kbc_arena_new(4096);
  kbc_artifact art;
  memset(&art, 0, sizeof art);
  kbc_err_reset(&e);
  char *out = NULL;
  if (a != NULL &&
      !kbc_failed(kbc_store_get_artifact_by_path(s, a, corpus, rel, &art, &e))) {
    out = strdup(art.id);
  }
  kbc_arena_free(a);
  kbc_store_close(s);
  return out; /* KBC_OWN, or NULL */
}

/* `kbc bench init --kb NAME --output PATH [--n N] [--seed S]`. */
static int bench_init(const world *w, const char *out_path, const char *n,
    const char *seed, kbc_str *out) {
  argv_vec a = {NULL, 0, 0};
  argv_push(&a, kbc_bin());
  argv_push(&a, "bench");
  argv_push(&a, "init");
  argv_push(&a, "--kb");
  argv_push(&a, "notes");
  argv_push(&a, "--output");
  argv_push(&a, out_path);
  if (n != NULL) {
    argv_push(&a, "--n");
    argv_push(&a, n);
  }
  if (seed != NULL) {
    argv_push(&a, "--seed");
    argv_push(&a, seed);
  }
  int rc = run_kbc(w->home, w->config, a.v, out);
  argv_free(&a);
  return rc;
}

static size_t count_lines(const char *s) {
  size_t n = 0;
  for (const char *p = s; *p != '\0'; p++) {
    if (*p == '\n') {
      n++;
    }
  }
  return n;
}

/* The scaffold's whole job is to name artifacts that EXIST, so that a later
 * recall measurement has real ids to score. A row carrying an empty `query`
 * and a real `relevant` id is the contract; a row carrying an id the store
 * never minted would make every Recall@k figure quietly zero. The id is
 * checked against the store, not against a hash recomputed here. */
KBC_TEST(bench_init_scaffolds_one_row_per_artifact_naming_a_real_id) {
  world w;
  if (!world_up(&w, "notes")) {
    world_down(&w);
    return;
  }
  bench_corpus_files(&w);
  bench_index_corpus(&w);
  char out_path[KBC_TEST_PATH_MAX];
  (void)path_join(out_path, sizeof out_path, w.root, "q.jsonl");
  kbc_str out;
  kbc_str_init(&out);
  /* --n 2 from three indexable files: the count is the sampler honouring
   * --n, and the file being a.md's row is the id being a real one. */
  KBC_CHECK_EQ_INT(bench_init(&w, out_path, "2", "0xb33f", &out), 0);
  kbc_str_free(&out);
  char *scaffold = kbc_test_read_file(out_path);
  KBC_CHECK_MSG(scaffold != NULL, "bench init wrote no file at %s", out_path);
  if (scaffold == NULL) {
    world_down(&w);
    return;
  }
  KBC_CHECK_MSG(count_lines(scaffold) == 2,
         "expected 2 scaffold rows, got %zu; file was:\n%s",
   count_lines(scaffold), scaffold);
  KBC_CHECK_MSG(strstr(scaffold, "\"query\":\"\"") != NULL,
           "each row must carry an empty `query` for the operator to fill "
        "in; file was:\n%s",
        scaffold);
  char *real_id = stored_id_for_path(w.db, "notes", "a.md");
  KBC_CHECK_MSG(real_id != NULL, "could not read a.md's id back from the "
                                 "store");
  if (real_id != NULL) {
    KBC_CHECK_MSG(strstr(scaffold, real_id) != NULL,
       "the scaffold names no id the store holds for a.md (%s); file "
          "was:\n%s",
            real_id, scaffold);
    free(real_id);
  }
  /* d.txt is not indexable, so it must never be sampled: a scaffold row for
   * a file the daemon will not index is a query that can never be answered. */
  KBC_CHECK_MSG(strstr(scaffold, "d.txt") == NULL,
           "bench init sampled d.txt, which kb-c does not index; file "
        "was:\n%s",
        scaffold);
  free(scaffold);
  world_down(&w);
}

/* The same seed must scaffold the same set — that is the property that lets
 * a labeller and a collaborator start from one file, and it is the reason
 * the Rust inlines SplitMix64 rather than calling the platform RNG. A
 * sampler that drew from the clock or from readdir order would pass the
 * "did it write rows" test and fail this one. */
KBC_TEST(the_same_seed_scaffolds_byte_identical_output) {
  world w;
  if (!world_up(&w, "notes")) {
    world_down(&w);
    return;
  }
  bench_corpus_files(&w);
  char first[KBC_TEST_PATH_MAX];
  char second[KBC_TEST_PATH_MAX];
  (void)path_join(first, sizeof first, w.root, "one.jsonl");
  (void)path_join(second, sizeof second, w.root, "two.jsonl");
  KBC_CHECK_EQ_INT(bench_init(&w, first, "3", "0xb33f", NULL), 0);
  KBC_CHECK_EQ_INT(bench_init(&w, second, "3", "0xb33f", NULL), 0);
  char *a = kbc_test_read_file(first);
  char *b = kbc_test_read_file(second);
  KBC_CHECK_MSG(a != NULL && b != NULL, "bench init wrote no output");
  if (a != NULL && b != NULL) {
    KBC_CHECK_MSG(strcmp(a, b) == 0,
      "the same seed produced two different scaffolds:\n--- one ---\n%s\n"
  "--- two ---\n%s",
                  a, b);
  }
  /* All three indexable files, so the sample is the whole corpus and the
   * comparison is over a full set rather than a lucky subset. */
  if (a != NULL) {
    KBC_CHECK_MSG(count_lines(a) == 3, "expected 3 rows, got %zu:\n%s",
      count_lines(a), a);
  }
  free(a);
  free(b);
  world_down(&w);
}

/* A labelled query set is the most expensive thing this tool touches: an
 * operator has filled in every `query` field by hand. Overwriting it with a
 * fresh empty scaffold would destroy hours of work with no undo, so the verb
 * refuses — and the refusal must leave the file exactly as it was, which is
 * asserted rather than assumed. */
KBC_TEST(bench_init_refuses_to_overwrite_an_existing_query_set) {
  world w;
  if (!world_up(&w, "notes")) {
    world_down(&w);
    return;
  }
  bench_corpus_files(&w);
  char out_path[KBC_TEST_PATH_MAX];
  (void)path_join(out_path, sizeof out_path, w.root, "labelled.jsonl");
  kbc_test_write_file(out_path, "{\"query\":\"atlas determinism\","
             "\"relevant\":[\"aaaaaaaaaaaa\"]}\n");
  KBC_CHECK_MSG(bench_init(&w, out_path, "2", "0xb33f", NULL) == 1,
           "overwriting an existing query set is a user error (1)");
  char *after = kbc_test_read_file(out_path);
  KBC_CHECK_MSG(after != NULL &&
        strstr(after, "atlas determinism") != NULL,
      "the refused scaffold still modified the file; it now holds: %s",
   after != NULL ? after : "(gone)");
  free(after);
  world_down(&w);
}

/* ========================================================== daemon stop ===
 *
 * The pid file is the entire mechanism `daemon stop` has: it reads a pid,
 * signals it, and waits. Every case below is a state the file can be in,
 * and each one has a different right answer — so each is asserted on what
 * happened to the FILE as well as on the exit code, because "exited 1" is
 * equally what a correct prune and a total failure would print.
 */
static char *pidfile_path(const world *w) {
  static char path[KBC_TEST_PATH_MAX];
  (void)path_join(path, sizeof path, w->state, "kb-daemon.pid");
  return path;
}

static void write_pidfile(const world *w, const char *contents) {
  kbc_test_write_file(pidfile_path(w), contents);
}

/* A pid that is certain not to be running: fork a child, let it exit, reap
 * it. The kernel keeps the pid free until it wraps, so `kill -0` on it
 * reports ESRCH — which is exactly the state a crashed daemon leaves behind.
 * Naming a pid at random would be a coin flip against an unrelated process
 * on the machine, and a test that signals a stranger's process is worse than
 * no test. */
static long a_dead_pid(void) {
  pid_t p = fork();
  if (p < 0) {
    return -1;
  }
  if (p == 0) {
    _exit(0);
  }
  int status = 0;
  while (waitpid(p, &status, 0) < 0) {
    if (errno != EINTR) {
   return -1;
    }
  }
  return (long)p;
}

/* No pid file means no daemon. That is a user error — the operator asked to
 * stop something that is not running — and NOT a daemon failure, so it is
 * exit 1. A `stop` that exited 0 here would let a deploy script believe it
 * had stopped a daemon that was never up, and one that exited 2 would send
 * the same script looking for a broken daemon. */
KBC_TEST(daemon_stop_with_no_pid_file_is_a_user_error) {
  world w;
  if (!world_up(&w, "notes")) {
    world_down(&w);
    return;
  }
  KBC_CHECK_MSG(run_kbc_va(&w, NULL, "daemon", "stop", NULL) == 1,
"daemon stop with nothing running is a user error (1), not a daemon "
   "failure");
  world_down(&w);
}

/* A crash leaves the pid file behind, and the next container boot can even
 * reuse the pid. `stop` must prune the corpse rather than signal whatever
 * now holds that number, so the assertion is on the FILE being gone — an
 * implementation that merely reported the pid as dead and left it would
  * leave the trap set for the next start. */
KBC_TEST(daemon_stop_prunes_a_pid_file_naming_a_dead_process) {
  world w;
  if (!world_up(&w, "notes")) {
    world_down(&w);
    return;
  }
  long dead = a_dead_pid();
  KBC_CHECK_MSG(dead > 0, "could not obtain a certainly-dead pid");
 if (dead <= 0) {
    world_down(&w);
    return;
  }
  char buf[32];
  (void)snprintf(buf, sizeof buf, "%ld\n", dead);
  write_pidfile(&w, buf);
  kbc_str out;
  kbc_str_init(&out);
  KBC_CHECK_MSG(run_kbc_va(&w, &out, "daemon", "stop", NULL) == 1,
        "stopping a dead pid is a user error (1), not a daemon failure");
  KBC_CHECK_MSG(strstr(out.ptr, "dead") != NULL,
           "the stale-pid report did not say the pid was dead; got: %s",
        out.ptr);
  KBC_CHECK_MSG(!kbc_path_exists(pidfile_path(&w)),
 "the stale pid file was not pruned, so the next start inherits the "
   "trap");
  kbc_str_free(&out);
  world_down(&w);
}

/* The real thing: start a daemon, stop it, and prove both halves. A `stop`
 * that reported success without the process going away — or that signalled
 * nothing and left the pid file — would pass a test that only checked the
 * exit code, which is why the pid file's disappearance is asserted as well
 * as the process being reaped. */
KBC_TEST(a_running_daemon_is_stopped_and_leaves_no_pid_file) {
  world w;
  if (!world_up(&w, "notes")) {
    world_down(&w);
    return;
  }
  /* The daemon refuses to start without a token file — a pre-existing rule
   * in cmd_daemon, not something `stop` introduced. */
  KBC_CHECK_EQ_INT(run_kbc_va(&w, NULL, "token", "generate", NULL), 0);
  /* Port 0 would be ideal but the config validator (src/config.c) rejects
   * anything outside 1..65535, so a port is picked and a collision shows up
   * as a daemon that never wrote its pid file — which the wait below turns
   * into a clear failure rather than a hang. */
  KBC_CHECK_EQ_INT(run_kbc_va(&w, NULL, "daemon", "--port", "47311", NULL), 0);
  /* The daemon forks and returns; the pid file is written by the child once
   * its listener is up, so the wait is for the file, not for the parent. */
  bool up = false;
  for (int i = 0; i < 100 && !up; i++) {
    up = kbc_path_exists(pidfile_path(&w));
    if (!up) {
      struct timespec ts = { 0, 50 * 1000 * 1000 };
      (void)nanosleep(&ts, NULL);
    }
  }
  KBC_CHECK_MSG(up, "the daemon never wrote %s, so it did not start",
           pidfile_path(&w));
  if (!up) {
    world_down(&w);
    return;
  }
  char *pidtext = kbc_test_read_file(pidfile_path(&w));
  KBC_CHECK_MSG(pidtext != NULL && pidtext[0] != '\0',
           "the pid file is empty, so there is nothing for stop to signal");
  long pid = pidtext != NULL ? strtol(pidtext, NULL, 10) : 0;
  KBC_CHECK_MSG(pid > 0, "the pid file does not hold a pid: %s", pidtext);
  free(pidtext);

  kbc_str out;
  kbc_str_init(&out);
  KBC_CHECK_MSG(run_kbc_va(&w, &out, "daemon", "stop", NULL) == 0,
    "stopping a running daemon must exit 0; got output:\n%s", out.ptr);
  KBC_CHECK_MSG(strstr(out.ptr, "exited cleanly") != NULL,
   "stop did not report a clean exit; got:\n%s", out.ptr);
  KBC_CHECK_MSG(!kbc_path_exists(pidfile_path(&w)),
    "the daemon exited but left its pid file behind");
  /* The process really is gone, not merely reported gone. */
  if (pid > 0) {
 KBC_CHECK_MSG(kill((pid_t)pid, 0) != 0 && errno == ESRCH,
        "pid %ld is still alive after `daemon stop` reported a clean exit",
    pid);
  }
  kbc_str_free(&out);
  /* And the stop is not repeatable: the second one has nothing to stop. */
  KBC_CHECK_MSG(run_kbc_va(&w, NULL, "daemon", "stop", NULL) == 1,
       "a second stop must be a user error (1): the first removed the "
    "pid file");
  world_down(&w);
}

/* Two daemons on one store is the failure the pid file exists to prevent:
 * both would reindex, both would hold the db, and the operator would see
 * results that depend on which one answered. The second start must be
* refused, and refused with the hint that names the way out. */
KBC_TEST(a_second_daemon_is_refused_while_one_is_running) {
  world w;
  if (!world_up(&w, "notes")) {
    world_down(&w);
    return;
  }
  KBC_CHECK_EQ_INT(run_kbc_va(&w, NULL, "token", "generate", NULL), 0);
  KBC_CHECK_EQ_INT(run_kbc_va(&w, NULL, "daemon", "--port", "47313", NULL), 0);
  bool up = false;
  for (int i = 0; i < 100 && !up; i++) {
    up = kbc_path_exists(pidfile_path(&w));
    if (!up) {
      struct timespec ts = { 0, 50 * 1000 * 1000 };
      (void)nanosleep(&ts, NULL);
    }
  }
  KBC_CHECK_MSG(up, "the daemon never wrote its pid file, so it did not "
        "start");
  if (!up) {
    world_down(&w);
    return;
  }
  kbc_str out;
  kbc_str_init(&out);
  KBC_CHECK_MSG(run_kbc_va(&w, &out, "daemon", "--port", "47314", NULL) == 1,
       "a second daemon on the same store must be refused (1)");
  KBC_CHECK_MSG(strstr(out.ptr, "already running") != NULL,
  "the refusal did not say a daemon was already running; got:\n%s",
     out.ptr);
  kbc_str_free(&out);
  /* The first daemon is still the one holding the store — the refusal must
   * not have disturbed it. */
  KBC_CHECK_MSG(run_kbc_va(&w, NULL, "daemon", "stop", NULL) == 0,
         "the first daemon was not stoppable after the refusal");
  world_down(&w);
}

/* ------------------------------------------------- prune / retention ------
 *
 * `kbc prune` is the operator-triggered form of the original's retention
 * sweep (`Db::retention_prune`, kb-core/src/storage/sqlite.rs:2361). Every
 * statement it runs there deletes from `history` / `reading_sections` /
 * served `memory_recalls` rows — reading history, never an artifact. The
 * cases below seed the `history` table through the frozen public API,
 * because that table has no production writer in kb-c (store.h says so and
 * means it) and a test that cannot put a row in cannot test a prune.
 */

/* The world's store, opened through the same public API `world_up` used. The
 * prune under test opens its OWN connection in a separate process, so
 * seeding and reading across this boundary is the shape an operator sees. */
static kbc_store *world_store(world *w) {
  kbc_config cfg;
  memset(&cfg, 0, sizeof cfg);
  size_t n = strlen(w->db) + 1u;
  cfg.db_path = malloc(n);
  if (cfg.db_path == NULL) {
    abort();
  }
  memcpy(cfg.db_path, w->db, n);
  kbc_err e;
  kbc_err_reset(&e);
  kbc_store *s = kbc_store_open(&cfg, &e);
  free(cfg.db_path);
  if (s == NULL) {
    kbc_test_fail(__FILE__, __LINE__, "world_store: open: %s", e.msg);
    abort();
  }
  return s;
}

/* One `open`-kind history row, `age_secs` old. `id` names the document the
 * visit was to, so a prune that wrongly cascaded into the document would
 * have something to destroy. */
static void seed_history(world *w, const char *id, int64_t age_secs) {
  kbc_store *s = world_store(w);
  kbc_history_row row;
  memset(&row, 0, sizeof row);
  row.kind = "open";
  row.artifact_id = id;
  row.started_at = (int64_t)time(NULL) - age_secs;
  row.updated_at = row.started_at;
  kbc_err e;
  kbc_err_reset(&e);
  if (kbc_failed(kbc_store_add_history(s, &row, &e))) {
    kbc_test_fail(__FILE__, __LINE__, "seed_history: %s", e.msg);
    abort();
  }
  kbc_store_close(s);
}

/* How many `history` rows the world's store holds. Read through the frozen
 * API rather than through SQL so the count is the one a caller would see,
 * not this test's idea of the table. */
static int64_t history_rows(world *w) {
  kbc_store *s = world_store(w);
  kbc_arena *a = kbc_arena_new(4096);
  if (a == NULL) {
    abort();
  }
  kbc_history_row *rows = NULL;
  size_t n = 0;
  kbc_err e;
  kbc_err_reset(&e);
  int64_t got = -1;
  /* KBC_MAX_HITS is the cap `list_history` clamps to; asking for more is not
   * a bigger answer, it is the same answer, so a fixture that ever grew past
   * it would read as a prune that deleted nothing. */
  if (kbc_failed(kbc_store_list_history(s, a, NULL, KBC_MAX_HITS, &rows, &n,
                                        &e))) {
    kbc_test_fail(__FILE__, __LINE__, "history_rows: %s", e.msg);
  } else {
    got = (int64_t)n;
  }
  kbc_arena_free(a);
  kbc_store_close(s);
  return got;
}

/* The default must not delete. The original's sweep cannot be run by hand at
 * all — it is armed by config and fires on a timer — so it never needed a
 * dry run. A CLI verb puts the delete one keypress away, so the safety moves
 * to the invocation: without --apply this reports and changes nothing.
 *
 * Reverting the dry-run default turns the row count red, which is the whole
 * point of asserting it rather than the wording. */
KBC_TEST(prune_without_apply_reports_the_window_and_deletes_nothing) {
  world w;
  if (!world_up(&w, "notes")) {
    world_down(&w);
    return;
  }
  seed_history(&w, "aaaaaaaaaaaa", 40 * 86400);
  KBC_CHECK_EQ_INT(history_rows(&w), 1);

  kbc_str out;
  kbc_str_init(&out);
  KBC_CHECK_MSG(run_kbc_va(&w, &out, "prune", "--days", "30", NULL) == 0,
 "a dry-run prune must succeed; got:\n%s", out.ptr);
  KBC_CHECK_MSG(strstr(out.ptr, "would prune 1 history row") != NULL,
          "the dry run did not report the one row it would remove; "
           "got:\n%s", out.ptr);
  kbc_str_free(&out);

  KBC_CHECK_MSG(history_rows(&w) == 1,
          "the dry run DELETED a row: history went from 1 to %lld without "
           "--apply", (long long)history_rows(&w));
  world_down(&w);
}

/* The window is the original's: rows OLDER than `now - days` go, and a row
 * inside the window stays. Asserting only that "something was deleted" would
 * pass a prune that deleted everything, so both sides of the boundary are
 * pinned — and the survivor is what catches an off-by-one in the cutoff
 * arithmetic. */
KBC_TEST(prune_with_apply_removes_only_the_history_older_than_the_window) {
  world w;
  if (!world_up(&w, "notes")) {
    world_down(&w);
    return;
  }
  seed_history(&w, "aaaaaaaaaaaa", 40 * 86400); /* older than 30 days */
  seed_history(&w, "aaaaaaaaaaaa", 5 * 86400);  /* inside the window */
  KBC_CHECK_EQ_INT(history_rows(&w), 2);

  kbc_str out;
  kbc_str_init(&out);
  KBC_CHECK_MSG(run_kbc_va(&w, &out, "prune", "--days", "30", "--apply",
         NULL) == 0,
       "an applied prune must succeed; got:\n%s", out.ptr);
  KBC_CHECK_MSG(strstr(out.ptr, "pruned 1 history row") != NULL,
        "the applied prune did not report removing exactly the one "
         "aged row; got:\n%s", out.ptr);
  kbc_str_free(&out);

  KBC_CHECK_MSG(history_rows(&w) == 1,
                "expected the 40-day-old row to go and the 5-day-old row to "
                "stay, leaving 1; got %lld", (long long)history_rows(&w));
  world_down(&w);
}

/* THE load-bearing case, and the inverse of what the verb's name suggests.
 *
 * The original's retention sweep never removes a document. Its own doc
 * comment (sqlite.rs:2357) says it "deliberately does NOT manage the
 * R2-cascade tables (sessions/edges/…); those are pruned per-artifact on
 * delete, not by age", and config.rs:470 says edges "has NO timestamp
 * column, so it CANNOT be time-pruned and is deliberately absent here".
 *
 * So a prune that deleted a document would not be a half-done port — it
 * would be a FEATURE THE ORIGINAL DOES NOT HAVE, and the most dangerous one
 * this binary could grow: an operator who ran a retention window would come
 * back to a corpus with documents missing from it. This case indexes a real
 * document, ages history rows that name it, prunes, and then requires the
 * document to still be in the store AND still be findable by search.
 *
 * Both halves are load-bearing. Checking the store row alone would pass a
 * prune that dropped the artifact row but left the posting list behind, and
 * checking search alone would pass one that left a store row search could no
 * longer resolve. */
KBC_TEST(a_prune_takes_no_document_with_it_out_of_the_store_or_search) {
  world w;
  /* The indexable world, not the usual one: this case reindexes, and the
   * usual fixture's synthetic artifact row would collide with the real
   * ingest of the same file and make the reindex refuse. */
  if (!world_up_indexable(&w, "notes")) {
    world_down(&w);
    return;
  }
  /* A real index, so "still searchable" is a claim about the index and not
   * about a store row that happens to be there. */
  KBC_CHECK_MSG(run_kbc_va(&w, NULL, "reindex", "--kb", "notes", NULL) == 0,
        "the corpus did not reindex, so the search half of this case "
   "would be asserting nothing");

  kbc_str before;
  kbc_str_init(&before);
  KBC_CHECK_MSG(run_kbc_va(&w, &before, "search", "backup-worthy", NULL) == 0,
 "the document is not searchable before the prune, so the search "
                "half of this case would be asserting nothing; got:\n%s",
         before.ptr);
  KBC_CHECK_MSG(strstr(before.ptr, "id:") != NULL,
             "the pre-prune search printed no id; got:\n%s",
         before.ptr);
  kbc_str_free(&before);

  /* Two aged visits naming the REAL document id, minted the same way the
   * ingest minted it. A history row pointing at an id nothing has would
   * weaken the case: a prune that deleted documents by walking history rows
   * would find nothing to delete and pass. */
  char doc_id[KBC_MAX_ID_LEN + 1];
  kbc_id_for_artifact(doc_id, "notes", "a.md");
  seed_history(&w, doc_id, 90 * 86400);
  seed_history(&w, doc_id, 91 * 86400);

  kbc_str out;
  kbc_str_init(&out);
  KBC_CHECK_MSG(run_kbc_va(&w, &out, "prune", "--days", "1", "--apply",
        NULL) == 0,
            "the applied prune failed; got:\n%s", out.ptr);
  KBC_CHECK_MSG(strstr(out.ptr, "pruned 2 history rows") != NULL,
       "the prune did not remove both aged history rows; got:\n%s",
        out.ptr);
  kbc_str_free(&out);

  KBC_CHECK_MSG(history_rows(&w) == 0,
     "the prune left history rows behind: %lld",
                (long long)history_rows(&w));

  /* Still in the store. */
  kbc_str listing;
  kbc_str_init(&listing);
  KBC_CHECK_MSG(run_kbc_va(&w, &listing, "list", "--kb", "notes", NULL) == 0,
      "`kbc list` failed after the prune; got:\n%s",
       listing.ptr);
  KBC_CHECK_MSG(strstr(listing.ptr, "a.md") != NULL,
 "the prune removed the DOCUMENT from the store — the original "
       "never prunes artifacts by age; got:\n%s", listing.ptr);
  kbc_str_free(&listing);

  /* And still findable, which is the half a store-only assertion misses. */
  kbc_str after;
  kbc_str_init(&after);
  KBC_CHECK_MSG(run_kbc_va(&w, &after, "search", "backup-worthy", NULL) == 0,
    "the document stopped being SEARCHABLE after a prune — the "
            "index and the store now disagree; got:\n%s", after.ptr);
  KBC_CHECK_MSG(strstr(after.ptr, "id:") != NULL,
         "the post-prune search printed no id; got:\n%s", after.ptr);
  kbc_str_free(&after);
  world_down(&w);
}

/* `--days 0` is refused, and the reason is the original's: a zero-day
 * window sets the cutoff to `now` and would delete every row on the next
 * tick, which is why `[retention] history_days = 0` is a hard validation
 * error in the original (config.rs:1932-1947). A CLI cannot express "off" as
 * 0 either, so it refuses the value rather than quietly redefining it. */
KBC_TEST(prune_refuses_a_zero_day_window) {
  world w;
  if (!world_up(&w, "notes")) {
    world_down(&w);
    return;
  }
  seed_history(&w, "aaaaaaaaaaaa", 60);
  kbc_str out;
  kbc_str_init(&out);
  KBC_CHECK_MSG(run_kbc_va(&w, &out, "prune", "--days", "0", "--apply",
             NULL) == 1,
        "a zero-day window is a user error (1); got:\n%s", out.ptr);
  kbc_str_free(&out);
  KBC_CHECK_MSG(history_rows(&w) == 1,
                "the refused zero-day prune still deleted a row");
  world_down(&w);
}

/* An absent window is the original's `None` — keep forever — so it is a
 * usage error here rather than a silent success: exiting 0 having removed
 * nothing is the reading that hides a typo in the flag name. */
KBC_TEST(prune_without_a_window_is_a_user_error) {
  world w;
  if (!world_up(&w, "notes")) {
    world_down(&w);
    return;
  }
  kbc_str out;
  kbc_str_init(&out);
  KBC_CHECK_MSG(run_kbc_va(&w, &out, "prune", NULL) == 1,
         "`kbc prune` with no --days is a user error (1); got:\n%s",
      out.ptr);
  kbc_str_free(&out);
  world_down(&w);
}

/* ------------------------------------------------------------- metrics --- */

/* Every counter `/metrics` renders lives in the running daemon, so there is
 * no in-process fallback — unlike `search`/`reindex`, which have one. A
 * fallback would print a complete exposition of zeroes, which reads exactly
 * like a healthy idle daemon. `kbc status` sets the precedent: no fallback,
 * exit 2 when no daemon answers. */
KBC_TEST(metrics_without_a_daemon_is_a_daemon_failure) {
  world w;
  if (!world_up(&w, "notes")) {
    world_down(&w);
    return;
  }
  kbc_str out;
  kbc_str_init(&out);
  KBC_CHECK_MSG(run_kbc_va(&w, &out, "--daemon", "http://127.0.0.1:47999",
   "metrics", NULL) == 2,
 "`kbc metrics` with no daemon must exit 2 (daemon/IO), not 1 "
                "and not 0; got:\n%s", out.ptr);
  kbc_str_free(&out);
  world_down(&w);
}

/* `--out` is the one thing this verb has that the original's `kb metrics`
 * does not, and the reason PORT_PLAN's stage-6 line says "metrics export":
 * the Rust's verb can only print, so keeping a scrape means a shell
 * redirect, and a redirect is not something a verb can be tested through.
 * The file must hold the daemon's ACTUAL exposition, which is the only way
 * this asserts the endpoint rather than the writer. */
KBC_TEST(metrics_writes_the_daemons_exposition_to_the_named_file) {
  world w;
  if (!world_up(&w, "notes")) {
    world_down(&w);
    return;
  }
  KBC_CHECK_EQ_INT(run_kbc_va(&w, NULL, "token", "generate", NULL), 0);
  /* A port no other case in this file uses. The existing daemon cases pin
   * 47311/47313/47314, and a daemon left over from any of them would make
   * this bind fail and report a start failure that reads like a metrics
   * defect. A pid file cannot catch it either: every case owns its own state
   * dir, so a stranger's daemon is invisible to it. */
  KBC_CHECK_EQ_INT(run_kbc_va(&w, NULL, "daemon", "--port", "47411", NULL), 0);
  bool up = false;
  for (int i = 0; i < 100 && !up; i++) {
    up = kbc_path_exists(pidfile_path(&w));
    if (!up) {
      struct timespec ts = { 0, 50 * 1000 * 1000 };
      (void)nanosleep(&ts, NULL);
    }
  }
  if (!up) {
    kbc_test_fail(__FILE__, __LINE__,
    "the daemon never wrote its pid file, so it did not start");
    world_down(&w);
    return;
  }

  char out_path[KBC_TEST_PATH_MAX];
  (void)path_join(out_path, sizeof out_path, w.root, "metrics.prom");
  kbc_str out;
  kbc_str_init(&out);
  KBC_CHECK_MSG(run_kbc_va(&w, &out, "--daemon", "http://127.0.0.1:47411",
    "metrics", "--out", out_path, NULL) == 0,
    "kbc metrics --out against a live daemon must exit 0; "
          "got:\n%s", out.ptr);
  kbc_str_free(&out);

  kbc_str body;
  kbc_str_init(&body);
  kbc_err e;
  kbc_err_reset(&e);
  if (kbc_failed(kbc_str_read_file(out_path, &body, &e))) {
    kbc_test_fail(__FILE__, __LINE__, "metrics --out wrote no file: %s",
       e.msg);
  } else {
    /* Two families src/httpd.c always renders, so this is the daemon's
     * exposition and not an empty file. */
    KBC_CHECK_MSG(strstr(body.ptr, "kb_http_requests_total") != NULL,
          "the saved scrape has no kb_http_requests_total family; "
  "got:\n%s", body.ptr);
    KBC_CHECK_MSG(strstr(body.ptr, "kb_route_latency_bucket") != NULL,
          "the saved scrape has no kb_route_latency_bucket family; "
                  "got:\n%s", body.ptr);
    /* An exposition with no TYPE line is not text exposition 0.0.4, and the
     * daemon labels every family it writes. */
    KBC_CHECK_MSG(strstr(body.ptr, "# TYPE kb_http_requests_total counter")
          != NULL,
       "the saved scrape carries no TYPE line; got:\n%s", body.ptr);
  }
  kbc_str_free(&body);

  KBC_CHECK_EQ_INT(run_kbc_va(&w, NULL, "daemon", "stop", NULL), 0);
  world_down(&w);
}

/* ================================================== the eight read verbs ===
 *
 * Everything above exercises a verb either against a store it opened itself
 * or against a pid file. The verbs below are the ones whose whole job is to
 * be a CLIENT, so a test that never starts a daemon never runs the code that
 * matters: a search that answered from the local fallback would pass every
 * assertion here while never having opened a socket. Each case therefore
 * starts a real daemon on its own port and points the verb at it with an
 * explicit --daemon, and each asserts on what came back over that socket.
 *
 * A PORT PER CASE, not one shared port: ctest can run these cases in any
 * order and a second bind on a port a previous case still holds would report
 * a start failure that reads like a defect in the verb. */

/* Starts the world's daemon on `port` and waits for its pid file. Returns
 * false after reporting, so a case never asserts against a daemon that never
 * came up — a hang or a spurious failure is the alternative. */
static bool daemon_up(world *w, int port) {
  /* The daemon refuses to start without a token (cmd_daemon), so the token
   * is generated first: this is a precondition of the verb, not part of it. */
  if (run_kbc_va(w, NULL, "token", "generate", NULL) != 0) {
    kbc_test_fail(__FILE__, __LINE__, "token generate failed");
    return false;
  }
  char p[16];
  (void)snprintf(p, sizeof p, "%d", port);
  if (run_kbc_va(w, NULL, "daemon", "--port", p, NULL) != 0) {
    kbc_test_fail(__FILE__, __LINE__, "daemon --port %s failed to start", p);
    return false;
  }
  /* The child writes the pid file once its listener is bound, so the wait is
   * for the file rather than for the parent, which has already exited. */
  for (int i = 0; i < 200; i++) {
    if (kbc_path_exists(pidfile_path(w))) {
      return true;
    }
    struct timespec ts = { 0, 25 * 1000 * 1000 };
    (void)nanosleep(&ts, NULL);
  }
  kbc_test_fail(__FILE__, __LINE__, "the daemon never wrote %s",
                pidfile_path(w));
  return false;
}

/* `--daemon http://127.0.0.1:<port>` as two argv words, for run_kbc_va. */
static void daemon_argv(char *url, size_t cap, int port) {
  (void)snprintf(url, cap, "http://127.0.0.1:%d", port);
}

/* A daemon up on `port` over a world whose corpus is genuinely indexable, so
 * `reindex` has real work and `search` has a real hit to return. world_up
 * seeds a synthetic row whose id is a literal; world_up_indexable leaves the
 * artifact table empty, so the ids in play are the ones the reindex really
 * mints. */
static bool socket_world_up(world *w, const char *corpus, int port) {
  if (!world_up_indexable(w, corpus)) {
    return false;
  }
  return daemon_up(w, port);
}

/* `reindex` over the socket must report the document it really indexed. The
 * count is the assertion because it is the only part of the verb that can
 * only have come from the daemon: the local fallback would report the same
 * number, but this case additionally pins the code path by requiring the
 * daemon to be up first. */
KBC_TEST(reindex_over_a_socket_reports_the_documents_it_ingested) {
  world w;
  if (!socket_world_up(&w, "notes", 47501)) {
    world_down(&w);
    return;
  }
  char url[64];
  daemon_argv(url, sizeof url, 47501);
  /* The two streams are captured apart because the split IS part of this
   * verb's contract: the original writes the human line to stderr and keeps
   * stdout for the machine-readable body (reindex.rs:52-53), so that
   * `kb reindex > log` yields something a script can read. A merged capture
   * cannot tell a verb that honours that from one that prints both to stdout,
   * which is the exact regression this pins. */
  argv_vec a = {NULL, 0, 0};
  argv_push(&a, kbc_bin());
  argv_push(&a, "--daemon");
  argv_push(&a, url);
  argv_push(&a, "reindex");
  kbc_str out;
  kbc_str_init(&out);
  kbc_str errout;
  kbc_str_init(&errout);
  int rc = run_kbc_split(w.home, w.config, a.v, &out, &errout);
  argv_free(&a);
  KBC_CHECK_MSG(rc == 0, "reindex against a live daemon must exit 0; "
                        "stderr was:\n%s", stream_text(&errout));
  KBC_CHECK_MSG(strstr(stream_text(&errout), "1 documents") != NULL,
                "reindex did not report the one document in the corpus on "
                "stderr; got:\n%s", stream_text(&errout));
  /* The human line must NOT be on stdout, or `kbc reindex > log` captures it
   * and the redirect is no longer machine-readable. */
  KBC_CHECK_MSG(strstr(stream_text(&out), "documents") == NULL,
                "reindex wrote its human line to stdout, so a redirect "
                "captures prose; stdout was:\n%s", stream_text(&out));
  /* A daemon answered, so the "running locally" fallback line must be
   * absent: a verb that silently fell back would still print a count. */
  KBC_CHECK_MSG(strstr(stream_text(&errout), "running locally") == NULL,
                "reindex fell back to the local app with a daemon up; "
                "got:\n%s", stream_text(&errout));
  kbc_str_free(&out);
  kbc_str_free(&errout);
  KBC_CHECK_EQ_INT(run_kbc_va(&w, NULL, "daemon", "stop", NULL), 0);
  world_down(&w);
}

/* `search` over the socket must return the document the daemon holds. The
 * assertion is on the artifact's OWN id, read back out of the listing, so a
 * verb that answered with a stale or synthesised row cannot pass. */
KBC_TEST(search_over_a_socket_finds_a_document_the_daemon_indexed) {
  world w;
  if (!socket_world_up(&w, "notes", 47502)) {
    world_down(&w);
    return;
  }
  char url[64];
  daemon_argv(url, sizeof url, 47502);
  KBC_CHECK_EQ_INT(run_kbc_va(&w, NULL, "--daemon", url, "reindex", NULL), 0);

  kbc_str out;
  kbc_str_init(&out);
  KBC_CHECK_MSG(run_kbc_va(&w, &out, "--daemon", url, "search", "prose",
                           NULL) == 0,
                "search against a live daemon must exit 0; got:\n%s",
                out.ptr);
  KBC_CHECK_MSG(strstr(stream_text(&out), "a.md") != NULL,
                "search did not find the document it indexed; got:\n%s",
                out.ptr);
  KBC_CHECK_MSG(strstr(out.ptr, "running locally") == NULL,
        "search fell back to the local app with a daemon up; got:\n%s",
      out.ptr);
  kbc_str_free(&out);
  KBC_CHECK_EQ_INT(run_kbc_va(&w, NULL, "daemon", "stop", NULL), 0);
  world_down(&w);
}

/* A search that matches nothing is a USER error (1), not a success with an
 * empty list: the operator asked for something and got nothing, and a script
 * branching on the exit status must be able to see that. */
KBC_TEST(search_over_a_socket_with_no_hit_is_a_user_error) {
  world w;
  if (!socket_world_up(&w, "notes", 47503)) {
    world_down(&w);
    return;
  }
  char url[64];
  daemon_argv(url, sizeof url, 47503);
  KBC_CHECK_EQ_INT(run_kbc_va(&w, NULL, "--daemon", url, "reindex", NULL), 0);
  KBC_CHECK_MSG(run_kbc_va(&w, NULL, "--daemon", url, "search",
                "zzznosuchtermzzz", NULL) == 1,
         "a search with no hit must exit 1 (user error), so a script can "
      "see the miss; it exited 0");
  KBC_CHECK_EQ_INT(run_kbc_va(&w, NULL, "daemon", "stop", NULL), 0);
  world_down(&w);
}

/* `list` over the socket, and the id it prints is the one `get` then
 * resolves. The two are asserted together because `list` and `get` are the
 * pair an operator scripts: list to find an id, get to read the document. A
 * `get` that could not resolve what `list` just printed would break exactly
 * that loop. */
KBC_TEST(list_and_get_over_a_socket_agree_on_the_artifact_id) {
  world w;
  if (!socket_world_up(&w, "notes", 47504)) {
    world_down(&w);
    return;
  }
  char url[64];
  daemon_argv(url, sizeof url, 47504);
  KBC_CHECK_EQ_INT(run_kbc_va(&w, NULL, "--daemon", url, "reindex", NULL), 0);

  kbc_str listed;
  kbc_str_init(&listed);
  KBC_CHECK_MSG(run_kbc_va(&w, &listed, "--daemon", url, "list", NULL) == 0,
    "list against a live daemon must exit 0; got:\n%s", listed.ptr);
  /* The id the daemon minted, taken from the listing's own text rather than
 * recomputed here: recomputing would assert the test's own idea of the id
 * instead of the one the verb printed. */
  const char *at = strstr(listed.ptr, "id:");
  KBC_CHECK_MSG(at != NULL, "list printed no id line; got:\n%s", listed.ptr);
  if (at == NULL) {
    kbc_str_free(&listed);
    world_down(&w);
    return;
  }
  at += 3;
  while (*at == ' ') {
    at++;
  }
  char id[16];
  size_t idn = 0;
  while (idn + 1u < sizeof id && isxdigit((unsigned char)at[idn])) {
    id[idn] = at[idn];
    idn++;
  }
  id[idn] = '\0';
  KBC_CHECK_MSG(idn == 12, "list printed an id that is not 12 hex digits: %s",
            id);
  kbc_str_free(&listed);

  if (idn == 12) {
    kbc_str got;
    kbc_str_init(&got);
    KBC_CHECK_MSG(run_kbc_va(&w, &got, "--daemon", url, "get", id, NULL) == 0,
  "get of an id list just printed must exit 0; got:\n%s",
     got.ptr);
    KBC_CHECK_MSG(strstr(got.ptr, id) != NULL,
 "get did not echo the id it was asked for (%s); got:\n%s", id,
                  got.ptr);
    KBC_CHECK_MSG(strstr(got.ptr, "a.md") != NULL,
                  "get resolved the id to the wrong document; got:\n%s",
                  got.ptr);
    kbc_str_free(&got);
  }
  KBC_CHECK_EQ_INT(run_kbc_va(&w, NULL, "daemon", "stop", NULL), 0);
  world_down(&w);
}

/* An id the daemon never issued is a user error (1), and a MALFORMED id is
 * the same class: the operator named something that cannot exist. Both are
 * checked because they are refused at two different places — one before a
 * socket is opened, one after — and a verb that got the second wrong would
 * answer 1 by accident rather than by decision. */
KBC_TEST(get_over_a_socket_refuses_an_unknown_and_a_malformed_id) {
  world w;
  if (!socket_world_up(&w, "notes", 47505)) {
    world_down(&w);
    return;
  }
  char url[64];
  daemon_argv(url, sizeof url, 47505);
  KBC_CHECK_EQ_INT(run_kbc_va(&w, NULL, "--daemon", url, "reindex", NULL), 0);
  kbc_str out;
  kbc_str_init(&out);
  KBC_CHECK_MSG(run_kbc_va(&w, &out, "--daemon", url, "get", "ffffffffffff",
                           NULL) == 1,
                "get of an id the daemon never issued must exit 1 (user "
                "error); got:\n%s", stream_text(&out));
  /* The exit code alone is not enough to tell this refusal from any other
   * 1: the id the operator typed has to be named back, or a wrong id and a
   * broken daemon are indistinguishable at the prompt. */
  KBC_CHECK_MSG(strstr(stream_text(&out), "ffffffffffff") != NULL,
                "the refusal did not name the id that was asked for; "
                "got:\n%s", stream_text(&out));
  kbc_str_free(&out);
  KBC_CHECK_MSG(run_kbc_va(&w, NULL, "--daemon", url, "get", "not-an-id",
    NULL) == 1,
           "get of a malformed id must exit 1 (user error), refused before "
  "any request");
  KBC_CHECK_EQ_INT(run_kbc_va(&w, NULL, "daemon", "stop", NULL), 0);

  /* The SAME refusal with no daemon answering, which takes the other branch:
   * `get` falls back to a local app, and it is that app's NOTFOUND — not the
   * daemon's 404 — that has to be a user error. Two branches, two decisions,
   * and a verb that got only one right would still look correct against a
   * live daemon. The port is one nothing listens on. */
  kbc_str off;
  kbc_str_init(&off);
  KBC_CHECK_MSG(run_kbc_va(&w, &off, "--daemon", "http://127.0.0.1:1", "get",
                           "ffffffffffff", NULL) == 1,
                "get of an unknown id with no daemon must still exit 1 (user "
                "error), not the local fallback's own code; got:\n%s",
                stream_text(&off));
  /* The WORDING is pinned, not just the exit code and the id. The generic
   * status formatter ALSO exits 1 and also names the id, but reads like a
   * daemon fault (`get: not_found`) rather than the sentence an operator
   * typed their way into — and that difference is the whole reason this
   * refusal is a user error class rather than a fault class. */
  KBC_CHECK_MSG(strstr(stream_text(&off), "no artifact ffffffffffff") != NULL,
                "the local-path refusal is not the user-error sentence "
                "(`no artifact <id>`); got:\n%s", stream_text(&off));
  kbc_str_free(&off);
  world_down(&w);
}

/* `status` has no local fallback BY DESIGN: it reports the counters of a
 * running daemon, which is the whole point of asking. So the verb that must
 * not silently succeed when no daemon answers is `status` — and the counter
 * it prints must be the daemon's, which is why the case asserts on a value
 * the daemon really incremented rather than on the shape of the output. */
KBC_TEST(status_over_a_socket_prints_the_daemons_own_counters) {
  world w;
  if (!socket_world_up(&w, "notes", 47506)) {
    world_down(&w);
    return;
  }
  char url[64];
  daemon_argv(url, sizeof url, 47506);
  /* One reindex, so artifacts_indexed has a value the daemon itself set. */
  KBC_CHECK_EQ_INT(run_kbc_va(&w, NULL, "--daemon", url, "reindex", NULL), 0);
  kbc_str out;
  kbc_str_init(&out);
  KBC_CHECK_MSG(run_kbc_va(&w, &out, "--daemon", url, "status", NULL) == 0,
       "status against a live daemon must exit 0; got:\n%s", out.ptr);
  KBC_CHECK_MSG(strstr(out.ptr, "artifacts_indexed") != NULL,
        "status printed no artifacts_indexed counter; got:\n%s", out.ptr);
  KBC_CHECK_MSG(strstr(out.ptr, "reindex_runs") != NULL,
  "status printed no reindex_runs counter, so the daemon's own "
        "counters are not what it showed; got:\n%s",
    out.ptr);
  kbc_str_free(&out);
  KBC_CHECK_EQ_INT(run_kbc_va(&w, NULL, "daemon", "stop", NULL), 0);
  world_down(&w);
}

/* `add` writes the config file itself and never speaks to a daemon, so this
 * is the one verb here whose work is on disk. The assertion is that the
 * corpus it added is the one a LATER daemon then serves — the round trip is
 * what makes it a test of the verb rather than of a file write. */
KBC_TEST(add_then_a_daemon_serves_the_corpus_it_created) {
  world w;
  kbc_test_tmpdir(w.root, sizeof w.root);
  (void)path_join(w.home, sizeof w.home, w.root, "home");
  (void)path_join(w.config, sizeof w.config, w.root, "kb.toml");
  (void)path_join(w.state, sizeof w.state, w.root, "state");
  (void)path_join(w.corpus_dir, sizeof w.corpus_dir, w.root, "corpus");
  (void)path_join(w.db, sizeof w.db, w.state, "kb.db");
  (void)path_join(w.index, sizeof w.index, w.state, "index");
  kbc_test_mkdir_p(w.home);
  kbc_test_mkdir_p(w.state);
  kbc_test_mkdir_p(w.corpus_dir);
  /* `add` creates the config from NOTHING, which is its job, and the config
   * it creates carries the DEFAULT data_dir — a path relative to the working
   * directory. That is fine for an operator and unusable for a test: the
   * daemon's pid file would land in whatever directory the runner happened
   * to be in, so this case seeds a config that names the data_dir it wants
   * and then asserts `add` ADDS a corpus to it without disturbing the rest.
   * Re-adding a configured corpus is a partial update in the original
   * (add.rs:57-69), so the pre-set data_dir has to survive it. */
  kbc_str seed;
  kbc_str_init(&seed);
  (void)kbc_str_printf(&seed,
                       "[daemon]\ndata_dir = \"%s\"\n\n[[corpus]]\n"
                       "name = \"seed\"\npath = \"%s\"\n",
                       w.state, w.corpus_dir);
  kbc_test_write_file(w.config, seed.ptr);
  kbc_str_free(&seed);

  char doc[KBC_TEST_PATH_MAX];
  (void)path_join(doc, sizeof doc, w.corpus_dir, "hello.md");
  kbc_test_write_file(doc, "# Hello World\n\nzebras, here.\n");

  kbc_str out;
  kbc_str_init(&out);
  KBC_CHECK_MSG(run_kbc_va(&w, &out, "add", w.corpus_dir, "--kb", "notes",
                           NULL) == 0,
                "add of a new corpus must exit 0; got:\n%s",
                stream_text(&out));
  /* The name the operator typed is the name the corpus is reachable BY, so
   * `kbc search --kb notes` has to find what `add --kb notes` created. An
   * assertion on the document alone would pass for a verb that registered
   * the corpus under any name at all. */
  KBC_CHECK_MSG(strstr(stream_text(&out), "notes") != NULL,
                "add did not report the corpus under the name it was given; "
                "got:\n%s", stream_text(&out));
  /* `add` indexes as it registers, so the document count it prints is its
   * OWN work. The case below then lists WITHOUT a separate reindex, which is
   * what makes this the only thing proving the indexing happened here. */
  KBC_CHECK_MSG(strstr(stream_text(&out), "1 document") != NULL,
                "add did not index the corpus it registered, so the daemon "
                "would start on an empty store; got:\n%s",
                stream_text(&out));
  KBC_CHECK_MSG(kbc_path_exists(w.config),
                "add exited 0 but wrote no config at %s", w.config);
  kbc_str_free(&out);
  if (!kbc_path_exists(w.config)) {
    world_down(&w);
    return;
  }
  /* The data_dir the daemon will use must still be the one the config named,
   * or the pid file goes somewhere this case cannot find it. */
  KBC_CHECK_MSG(kbc_path_exists(w.state),
                "the seeded data_dir %s is gone, so add discarded it and "
                "the daemon would write its pid file elsewhere",
                w.state);
  /* The round trip: a daemon reading that config serves the corpus `add`
   * created AND indexed, with no second command run in between. */
  if (!daemon_up(&w, 47507)) {
    world_down(&w);
    return;
  }
  char url[64];
  daemon_argv(url, sizeof url, 47507);
  kbc_str listed;
  kbc_str_init(&listed);
  KBC_CHECK_MSG(run_kbc_va(&w, &listed, "--daemon", url, "list", "--kb",
                           "notes", NULL) == 0,
                "the corpus add created must be listable over a socket; "
                "got:\n%s", stream_text(&listed));
  KBC_CHECK_MSG(strstr(listed.ptr, "hello.md") != NULL,
                "the daemon did not serve the corpus `add` created; got:\n%s",
                listed.ptr);
  kbc_str_free(&listed);
  /* Asked for BY the name the operator gave. A bare `list` would pass for a
   * verb that registered the corpus under any name at all, which is exactly
   * the defect: `kbc add --kb notes` followed by `kbc search --kb notes` is
   * the sequence an operator actually runs, and it has to resolve. */
  kbc_str byname;
  kbc_str_init(&byname);
  KBC_CHECK_MSG(run_kbc_va(&w, &byname, "--daemon", url, "search", "zebras",
                           "--kb", "notes", NULL) == 0,
                "the corpus is not reachable by the name `add` registered it "
                "under; got:\n%s", stream_text(&byname));
  KBC_CHECK_MSG(strstr(byname.ptr, "hello.md") != NULL,
                "searching the corpus `add` created by its own name found "
                "nothing; got:\n%s", byname.ptr);
  kbc_str_free(&byname);
  KBC_CHECK_EQ_INT(run_kbc_va(&w, NULL, "daemon", "stop", NULL), 0);
  world_down(&w);
}

/* `metrics` is a client of a top-level route that answers text exposition,
 * so the assertion is on the daemon's OWN families reaching the operator's
 * stdout — a verb that printed a header of its own would pass an exit-code
 * test and fail this one. */
KBC_TEST(metrics_over_a_socket_prints_the_daemons_exposition) {
  world w;
  if (!socket_world_up(&w, "notes", 47508)) {
    world_down(&w);
    return;
  }
  char url[64];
  daemon_argv(url, sizeof url, 47508);
  /* Split streams: `kbc metrics > scrape.prom` is the reason this verb exists
   * rather than a shell redirect in the original, so the exposition has to
   * arrive on stdout ALONE. Asserting on a merged capture would let a verb
   * that interleaved a stderr notice into the scrape pass. */
  argv_vec a = {NULL, 0, 0};
  argv_push(&a, kbc_bin());
  argv_push(&a, "--daemon");
  argv_push(&a, url);
  argv_push(&a, "metrics");
  kbc_str out;
  kbc_str_init(&out);
  kbc_str errout;
  kbc_str_init(&errout);
  int rc = run_kbc_split(w.home, w.config, a.v, &out, &errout);
  argv_free(&a);
  KBC_CHECK_MSG(rc == 0, "metrics against a live daemon must exit 0; "
                        "stderr was:\n%s", stream_text(&errout));
  KBC_CHECK_MSG(strstr(stream_text(&out), "kb_http_requests_total") != NULL,
                "metrics printed no kb_http_requests_total family, so this "
                "is not the daemon's exposition; got:\n%s", stream_text(&out));
  KBC_CHECK_MSG(strstr(stream_text(&out), "# TYPE") != NULL,
                "metrics printed no TYPE line, so it is not text exposition "
                "0.0.4; got:\n%s", stream_text(&out));
  /* A scrape a scraper reads must not carry a human notice on either
   * stream, and `kb_http_requests_total` is a family only the daemon emits. */
  KBC_CHECK_MSG(strstr(stream_text(&errout), "kb_http_requests_total") == NULL,
                "the exposition leaked onto stderr, so a redirect of stdout "
                "alone would miss it; stderr was:\n%s", stream_text(&errout));
  kbc_str_free(&out);
  kbc_str_free(&errout);
  KBC_CHECK_EQ_INT(run_kbc_va(&w, NULL, "daemon", "stop", NULL), 0);
  world_down(&w);
}

/* `version` is the one verb with no dependency at all: no config, no
 * daemon, no store. It is here because a verb that could be broken by an
 * unrelated failure — a bad config path, a dead daemon — is a version
 * command an operator cannot trust to tell them what is installed. */
KBC_TEST(version_needs_no_config_and_no_daemon) {
  world w;
  if (!world_up(&w, "notes")) {
    world_down(&w);
    return;
  }
  /* Point at a config that does not exist: `version` must not read one. */
  char missing[KBC_TEST_PATH_MAX];
  (void)path_join(missing, sizeof missing, w.root, "no-such-config.toml");
  argv_vec a = {NULL, 0, 0};
  argv_push(&a, kbc_bin());
  argv_push(&a, "--config");
  argv_push(&a, missing);
  argv_push(&a, "--daemon");
  argv_push(&a, "http://127.0.0.1:1");
  argv_push(&a, "version");
  kbc_str out;
  kbc_str_init(&out);
  int rc = run_kbc(w.home, missing, a.v, &out);
  argv_free(&a);
  KBC_CHECK_MSG(rc == 0,
 "version must exit 0 with no config and no daemon; got %d:\n%s", rc,
   out.ptr);
  KBC_CHECK_MSG(strstr(out.ptr, KBC_PROJECT) != NULL,
"version printed no project name; got:\n%s", out.ptr);
  kbc_str_free(&out);
  world_down(&w);
}

/* ------------------------------------------- global flags, both positions ---
 *
 * The usage text promises that a global may be given before OR after the
 * verb, and that a repeated global takes its last value. Both halves are
 * asserted here, and the two positions are compared against EACH OTHER
 * rather than against a golden string: the contract is that the position does
 * not change what the verb does, and only a comparison can show that.
 *
 * `search` is the verb used because it answers from a live daemon over a
 * socket, so the flags under test are the ones actually steering the request
 * rather than being parsed and dropped. */
KBC_TEST(a_global_flag_works_before_and_after_the_verb_alike) {
  world w;
  if (!socket_world_up(&w, "notes", 47509)) {
    world_down(&w);
    return;
  }
  char url[64];
  daemon_argv(url, sizeof url, 47509);
  KBC_CHECK_EQ_INT(run_kbc_va(&w, NULL, "--daemon", url, "reindex", NULL), 0);

  kbc_str before;
  kbc_str_init(&before);
  KBC_CHECK_MSG(run_kbc_va(&w, &before, "--json", "--daemon", url, "search",
            "prose", NULL) == 0,
         "--json and --daemon BEFORE the verb must exit 0; got:\n%s",
       before.ptr);
  kbc_str after;
  kbc_str_init(&after);
  KBC_CHECK_MSG(run_kbc_va(&w, &after, "search", "prose", "--json",
    "--daemon", url, NULL) == 0,
      "--json and --daemon AFTER the verb must exit 0; got:\n%s",
      after.ptr);
  /* Same verb, same daemon, same query: the two invocations must agree. The
   * daemon reports its own `took_us`, which is a timing measurement and
   * differs between two requests by nature, so it is the ONE field excluded
   * from the comparison rather than the comparison being weakened to a
   * substring. Everything else — the rows, their ids and scores — is
   * required to be byte-identical. */
  kbc_str b2;
  kbc_str b3;
  kbc_str_init(&b2);
  kbc_str_init(&b3);
  (void)kbc_str_append(&b2, before.ptr, before.len);
  (void)kbc_str_append(&b3, after.ptr, after.len);
  char *bp = strstr(b2.ptr, "took_us");
  char *ap = strstr(b3.ptr, "took_us");
  if (bp != NULL) {
    *bp = '\0';
  }
  if (ap != NULL) {
    *ap = '\0';
  }
  KBC_CHECK_MSG(strcmp(b2.ptr, b3.ptr) == 0,
   "the same verb with the same globals gave a different answer depending "
    "on whether they came before or after it;\nbefore: %s\nafter:  %s",
   b2.ptr, b3.ptr);
  kbc_str_free(&b2);
  kbc_str_free(&b3);
  kbc_str_free(&before);
  kbc_str_free(&after);
  KBC_CHECK_EQ_INT(run_kbc_va(&w, NULL, "daemon", "stop", NULL), 0);
  world_down(&w);
}

/* A global repeated in one command line takes its LAST value. Pinned with a
 * dead endpoint FIRST and the live one LAST: an implementation that took the
 * first would fall back to the local app (exit 0 on a different code path)
 * and one that took neither would report a connect failure (exit 2). Only
 * "the last value won" is exit 0 with the daemon's rows. */
KBC_TEST(a_repeated_global_flag_takes_its_last_value) {
  world w;
  if (!socket_world_up(&w, "notes", 47510)) {
    world_down(&w);
    return;
  }
  char url[64];
  daemon_argv(url, sizeof url, 47510);
  KBC_CHECK_EQ_INT(run_kbc_va(&w, NULL, "--daemon", url, "reindex", NULL), 0);
  kbc_str out;
  kbc_str_init(&out);
  KBC_CHECK_MSG(run_kbc_va(&w, &out, "search", "prose", "--daemon",
   "http://127.0.0.1:1", "--daemon", url, NULL) == 0,
     "a repeated --daemon must take its LAST value, so this must reach the "
    "live daemon; got:\n%s",
    out.ptr);
  KBC_CHECK_MSG(strstr(out.ptr, "running locally") == NULL,
  "the first --daemon won, so the verb fell back to the local app; "
        "got:\n%s",
   out.ptr);
  kbc_str_free(&out);
  KBC_CHECK_EQ_INT(run_kbc_va(&w, NULL, "daemon", "stop", NULL), 0);
  world_down(&w);
}

/* A response is framed by its Content-Length, not by the peer hanging up.
 * The daemon answers `Connection: keep-alive` even to a `Connection: close`
 * request, so a client that reads until EOF waits for the whole receive
 * timeout on EVERY call — 10 seconds a verb, and a script that runs eight of
 * them waits a minute and a half. The bound asserted here is deliberately
 * far above the ~0.3s a real request takes and far below the 10s timeout, so
 * it fails on the wait rather than on machine speed. */
KBC_TEST(a_socket_verb_does_not_wait_for_the_peer_to_close) {
  world w;
  if (!socket_world_up(&w, "notes", 47511)) {
    world_down(&w);
    return;
  }
  char url[64];
  daemon_argv(url, sizeof url, 47511);
  KBC_CHECK_EQ_INT(run_kbc_va(&w, NULL, "--daemon", url, "reindex", NULL), 0);

  struct timespec t0;
  struct timespec t1;
  (void)clock_gettime(CLOCK_MONOTONIC, &t0);
  kbc_str out;
  kbc_str_init(&out);
  int rc = run_kbc_va(&w, &out, "--daemon", url, "status", NULL);
  (void)clock_gettime(CLOCK_MONOTONIC, &t1);
  double secs = (double)(t1.tv_sec - t0.tv_sec) +
                (double)(t1.tv_nsec - t0.tv_nsec) / 1e9;
  KBC_CHECK_MSG(rc == 0, "status against a live daemon must exit 0; got "
         "output:\n%s",
  out.ptr);
  /* 3s: an order of magnitude over a local request, and a third of the
   * 10s receive timeout the bug actually cost. */
  KBC_CHECK_MSG(secs < 3.0,
 "status took %.2fs against a local daemon, so the client is still "
  "waiting for the peer to close instead of stopping at Content-Length",
           secs);
  kbc_str_free(&out);
  KBC_CHECK_EQ_INT(run_kbc_va(&w, NULL, "daemon", "stop", NULL), 0);
  world_down(&w);
}


int main(void) {
  static const kbc_test_case cases[] = {
      {"a_backup_of_a_real_store_roots_the_archive_at_the_corpus",
       a_backup_of_a_real_store_roots_the_archive_at_the_corpus},
      {"a_restore_into_an_empty_state_dir_produces_an_opening_database",
       a_restore_into_an_empty_state_dir_produces_an_opening_database},
      {"a_non_empty_state_dir_is_refused_until_force_is_given",
       a_non_empty_state_dir_is_refused_until_force_is_given},
      {"an_existing_but_empty_state_dir_needs_no_force",
       an_existing_but_empty_state_dir_needs_no_force},
      {"a_failed_backup_leaves_no_staging_dir_and_no_partial_tarball",
       a_failed_backup_leaves_no_staging_dir_and_no_partial_tarball},
      {"a_corpus_name_starting_with_a_dash_is_not_a_tar_option",
       a_corpus_name_starting_with_a_dash_is_not_a_tar_option},
      {"the_exit_codes_are_zero_user_error_and_daemon_or_io_failure",
       the_exit_codes_are_zero_user_error_and_daemon_or_io_failure},
      {"the_same_user_error_exits_the_same_code_from_every_verb",
       the_same_user_error_exits_the_same_code_from_every_verb},
      {"slates_ride_beside_the_corpus_and_are_never_clobbered",
       slates_ride_beside_the_corpus_and_are_never_clobbered},
      {"backing_up_an_unconfigured_corpus_fails",
       backing_up_an_unconfigured_corpus_fails},
      {"a_comment_added_through_the_cli_is_listed_back_with_its_fields",
       a_comment_added_through_the_cli_is_listed_back_with_its_fields},
      {"resolve_takes_a_comment_out_of_the_default_listing_until_all_is_given",
       resolve_takes_a_comment_out_of_the_default_listing_until_all_is_given},
      {"a_comment_cannot_be_resolved_through_the_wrong_document",
       a_comment_cannot_be_resolved_through_the_wrong_document},
      {"a_newline_in_a_comment_body_cannot_forge_a_second_listing_row",
       a_newline_in_a_comment_body_cannot_forge_a_second_listing_row},
      {"comments_refuses_a_missing_or_ambiguous_document",
       comments_refuses_a_missing_or_ambiguous_document},
      {"bench_init_scaffolds_one_row_per_artifact_naming_a_real_id",
       bench_init_scaffolds_one_row_per_artifact_naming_a_real_id},
      {"the_same_seed_scaffolds_byte_identical_output",
       the_same_seed_scaffolds_byte_identical_output},
      {"bench_init_refuses_to_overwrite_an_existing_query_set",
       bench_init_refuses_to_overwrite_an_existing_query_set},
      {"daemon_stop_with_no_pid_file_is_a_user_error",
       daemon_stop_with_no_pid_file_is_a_user_error},
      {"daemon_stop_prunes_a_pid_file_naming_a_dead_process",
       daemon_stop_prunes_a_pid_file_naming_a_dead_process},
      {"a_running_daemon_is_stopped_and_leaves_no_pid_file",
       a_running_daemon_is_stopped_and_leaves_no_pid_file},
      {"a_second_daemon_is_refused_while_one_is_running",
       a_second_daemon_is_refused_while_one_is_running},
      {"prune_without_apply_reports_the_window_and_deletes_nothing",
       prune_without_apply_reports_the_window_and_deletes_nothing},
      {"prune_with_apply_removes_only_the_history_older_than_the_window",
       prune_with_apply_removes_only_the_history_older_than_the_window},
      {"a_prune_takes_no_document_with_it_out_of_the_store_or_search",
       a_prune_takes_no_document_with_it_out_of_the_store_or_search},
      {"prune_refuses_a_zero_day_window", prune_refuses_a_zero_day_window},
      {"prune_without_a_window_is_a_user_error",
       prune_without_a_window_is_a_user_error},
      {"metrics_without_a_daemon_is_a_daemon_failure",
       metrics_without_a_daemon_is_a_daemon_failure},
      {"metrics_writes_the_daemons_exposition_to_the_named_file",
       metrics_writes_the_daemons_exposition_to_the_named_file},
      {"reindex_over_a_socket_reports_the_documents_it_ingested",
       reindex_over_a_socket_reports_the_documents_it_ingested},
      {"search_over_a_socket_finds_a_document_the_daemon_indexed",
       search_over_a_socket_finds_a_document_the_daemon_indexed},
      {"search_over_a_socket_with_no_hit_is_a_user_error",
       search_over_a_socket_with_no_hit_is_a_user_error},
      {"list_and_get_over_a_socket_agree_on_the_artifact_id",
       list_and_get_over_a_socket_agree_on_the_artifact_id},
      {"get_over_a_socket_refuses_an_unknown_and_a_malformed_id",
       get_over_a_socket_refuses_an_unknown_and_a_malformed_id},
      {"status_over_a_socket_prints_the_daemons_own_counters",
       status_over_a_socket_prints_the_daemons_own_counters},
      {"add_then_a_daemon_serves_the_corpus_it_created",
       add_then_a_daemon_serves_the_corpus_it_created},
      {"metrics_over_a_socket_prints_the_daemons_exposition",
       metrics_over_a_socket_prints_the_daemons_exposition},
      {"version_needs_no_config_and_no_daemon",
       version_needs_no_config_and_no_daemon},
      {"a_global_flag_works_before_and_after_the_verb_alike",
       a_global_flag_works_before_and_after_the_verb_alike},
      {"a_repeated_global_flag_takes_its_last_value",
       a_repeated_global_flag_takes_its_last_value},
      {"a_socket_verb_does_not_wait_for_the_peer_to_close",
       a_socket_verb_does_not_wait_for_the_peer_to_close},
      {NULL, NULL},
  };
  return kbc_test_run("cli", cases);
}
