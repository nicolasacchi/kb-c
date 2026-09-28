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

#include <sys/wait.h>

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
 * usually the thing under test. */
static int run_kbc(const char *home, const char *config_path,
                   char *const argv[], kbc_str *out) {
  int fds[2] = {-1, -1};
  if (out != NULL && pipe(fds) != 0) {
    return -1;
  }
  pid_t pid = fork();
  if (pid < 0) {
    return -1;
  }
  if (pid == 0) {
    if (out != NULL) {
      (void)close(fds[0]);
      (void)dup2(fds[1], STDOUT_FILENO);
      (void)dup2(fds[1], STDERR_FILENO);
      (void)close(fds[1]);
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
  if (out != NULL) {
    (void)close(fds[1]);
    char buf[4096];
    for (;;) {
      ssize_t n = read(fds[0], buf, sizeof buf);
      if (n <= 0) {
        break;
      }
      (void)kbc_str_append(out, buf, (size_t)n);
    }
    (void)close(fds[0]);
  }
  int status = 0;
  while (waitpid(pid, &status, 0) < 0) {
    if (errno != EINTR) {
      return -1;
    }
  }
  return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
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
static bool world_up(world *w, const char *corpus) {
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
      {NULL, NULL},
  };
  return kbc_test_run("cli", cases);
}
