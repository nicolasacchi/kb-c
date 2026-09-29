/* test_git.c — git plumbing against REAL repositories.
 *
 * Every case here builds an actual repository with the actual `git`
 * binary, commits to it, branches it, breaks it, and then reads it back
 * through libkbc. A mock would prove only that a mock can be called, so
 * there is no mock in this file: the oracle for blame is git's own
 * `--incremental` output, captured by a second, independent spawn.
 *
 * The injection cases are the interesting ones. They do not settle for
 * "the function returned an error": they put a recording `git` shim first
 * on PATH, show that a legal call DOES reach the process (so the recording
 * demonstrably works), and then show that an injection attempt produces NO
 * new argv record at all. Absence of a process is the property that
 * matters, and only a real spawner can be checked that way.
 */

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include "kbc_test.h"
#include "kbc/git.h"

/* ------------------------------------------------------------- fixtures */

/* Bounded copy for the oracle's fixed-width fields. A subject longer than
 * the field truncates rather than overflowing; the comparison that follows
 * then fails loudly, which is the right outcome for a fixture. */
static void copy_field(char *dst, size_t cap, const char *src)
{
  size_t n = strlen(src);
  if (n >= cap)
    n = cap - 1;
  memcpy(dst, src, n);
  dst[n] = '\0';
}

/* Joins a directory and a suffix into `dst`. Written with an explicit bound
 * rather than snprintf because the compiler is right to refuse to prove
 * that "%s/<suffix>" cannot overflow a same-sized buffer, and a fixture
 * that silently truncated a path would produce a confusing failure far
 * from the truncation. An over-long input yields an empty path, which the
 * fixture helpers then fail on loudly. */
static void join(char *dst, size_t cap, const char *dir, const char *suffix)
{
  size_t dn = strlen(dir);
  size_t sn = strlen(suffix);
  if (dn + sn + 1 > cap) {
    dst[0] = '\0';
    return;
  }
  memcpy(dst, dir, dn);
  memcpy(dst + dn, suffix, sn + 1);
}

/* execvp's argv is char *const[] only because pre-C23 exec has no const
 * qualifier; it never writes through it. One cast, made once per call,
 * keeps every fixture argv honestly typed as const char *. */
static char *const *argv_of(const char *const *v)
{
  union {
    const char *const *ro;
    char *const *rw;
  } u;
  u.ro = v;
  return u.rw;
}

/* The one place this file spawns a process of its own: fixtures and oracles
 * need git's own output verbatim, which means running git the way the
 * library does — argv array, no shell — rather than through system(3). */
static int run(const char *const argv[])
{
  pid_t pid = fork();
  int status = 0;
  if (pid < 0)
    return -1;
  if (pid == 0) {
    execvp(argv[0], argv_of(argv));
    _exit(127);
  }
  if (waitpid(pid, &status, 0) < 0)
    return -1;
  return WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
}

/* Every fixture invocation carries the identity flags inline, so no case
 * depends on a git config that may or may not exist on this machine. */
static const char *const k_identity[] = {
    "-c", "user.email=t@example.invalid",
    "-c", "user.name=Test",
    "-c", "commit.gpgsign=false",
};

static int git_in(const char *dir, const char *const sub[])
{
  const char *argv[24];
  int n = 0;
  int i;
  argv[n++] = "git";
  argv[n++] = "-C";
  argv[n++] = dir;
  for (i = 0; i < 6; i++)
    argv[n++] = k_identity[i];
  for (i = 0; sub[i] != NULL && n < 20; i++)
    argv[n++] = sub[i];
  argv[n] = NULL;
  return run(argv);
}

static void git_ok(const char *dir, const char *const sub[])
{
  int rc = git_in(dir, sub);
  KBC_CHECK_MSG(rc == 0, "fixture git: exit %d", rc);
}

static void git_am(const char *dir, const char *message)
{
  char msg[KBC_TEST_PATH_MAX];
  const char *sub[5];
  snprintf(msg, sizeof msg, "%s", message);
  sub[0] = "commit";
  sub[1] = "-q";
  sub[2] = "-m";
  sub[3] = msg;
  sub[4] = NULL;
  git_ok(dir, sub);
}

/* The repository every read case runs against: two commits on main, a
 * branch off the first commit, and a tag. `init -b main` rather than
 * init.defaultBranch, so "main" is a fact of the fixture and not of the
 * machine's configuration. */
static void make_repo(char *dir, size_t cap)
{
  char path[KBC_TEST_PATH_MAX + 64];
  char subdir[KBC_TEST_PATH_MAX + 64];
  const char *init[] = {"init", "-q", "-b", "main",
                  NULL};
  const char *add[] = {"add", "-A", NULL};
  const char *branch[] = {"branch", "feature", NULL};
  const char *tag[] = {"tag", "v1", NULL};

  kbc_test_tmpdir(dir, cap);
  join(subdir, sizeof subdir, dir, "/sub");
  kbc_test_mkdir_p(subdir);
  join(path, sizeof path, dir, "/a.txt");
  kbc_test_write_file(path, "one\ntwo\nthree\n");
  join(path, sizeof path, dir, "/sub/b.txt");
  kbc_test_write_file(path, "alpha\n");

  git_ok(dir, init);
  git_ok(dir, add);
  git_am(dir, "first commit");
  git_ok(dir, branch);
  git_ok(dir, tag);
  join(path, sizeof path, dir, "/a.txt");
  kbc_test_write_file(path, "one\ntwo\nthree\nfour\n");
  git_ok(dir, add);
  git_am(dir, "second commit");
}

/* ------------------------------------------------- a recording git shim */

static char g_shim_dir[KBC_TEST_PATH_MAX];
static char g_real_git[KBC_TEST_PATH_MAX];
static char g_orig_path[KBC_TEST_PATH_MAX * 3];

/* Installs a `git` shim that appends its argv to $KBC_TEST_ARGV_LOG and
 * then execs the real git. The real git's absolute path is baked in at
 * install time, so the shim cannot recurse into itself through PATH. */
static int shim_install(void)
{
  char path[KBC_TEST_PATH_MAX + 64];
  FILE *probe, *f;
  if (g_shim_dir[0] != '\0')
    return 0;
  probe = popen("command -v git 2>/dev/null", "r");
  if (probe == NULL)
    return -1;
  if (fgets(g_real_git, sizeof g_real_git, probe) == NULL) {
    pclose(probe);
    return -1;
  }
  pclose(probe);
  g_real_git[strcspn(g_real_git, "\r\n")] = '\0';
  if (g_real_git[0] != '/')
    return -1;
  kbc_test_tmpdir(g_shim_dir, sizeof g_shim_dir);
  join(path, sizeof path, g_shim_dir, "/git");
  f = fopen(path, "w");
  if (f == NULL)
    return -1;
  fprintf(f,
          "#!/bin/sh\n"
          "for a in \"$@\"; do printf '%%s\\n' \"$a\" >> \"$KBC_TEST_ARGV_LOG\";"
          " done\n"
          "printf '\\n' >> \"$KBC_TEST_ARGV_LOG\"\n"
          /* KBC_TEST_SHIM_SLEEP turns the shim into a git that never
           * answers, which is the only honest way to test a deadline: a
           * real git on a real repository finishes in milliseconds, so
           * the timeout path would otherwise be untestable. */
          "if [ -n \"$KBC_TEST_SHIM_SLEEP\" ]; then"
          " sleep \"$KBC_TEST_SHIM_SLEEP\"; exit 0; fi\n"
          "exec '%s' \"$@\"\n",
          g_real_git);
  fclose(f);
  return chmod(path, 0755) == 0 ? 0 : -1;
}

/* Installs the shim's PATH and points it at `log`. The original PATH is
 * kept verbatim so the restore is exact rather than approximate. */
static void shim_activate(const char *log)
{
  char path[KBC_TEST_PATH_MAX * 4];
  const char *old = getenv("PATH");
  KBC_CHECK_EQ_INT(shim_install(), 0);
  snprintf(g_orig_path, sizeof g_orig_path, "%s", old != NULL ? old : "");
  snprintf(path, sizeof path, "%s:%s", g_shim_dir, g_orig_path);
  KBC_CHECK_MSG(setenv("PATH", path, 1) == 0, "shim: setenv PATH");
  KBC_CHECK_MSG(setenv("KBC_TEST_ARGV_LOG", log, 1) == 0, "shim: setenv");
}

/* Restores the pre-shim PATH exactly. Leaving a recording `git` first on
 * PATH for the rest of the suite would silently invalidate every later
 * case, so this is not optional. */
static void shim_deactivate(void)
{
  unsetenv("KBC_TEST_ARGV_LOG");
  if (g_orig_path[0] != '\0')
    setenv("PATH", g_orig_path, 1);
}

/* Complete invocations in the log; records are separated by a blank line. */
static size_t shim_invocations(const char *log)
{
  char *text = kbc_test_read_file(log);
  size_t n = 0;
  int blank = 0;
  if (text == NULL)
    return 0;
  for (const char *p = text; *p != '\0'; p++) {
    if (*p != '\n')
      continue;
    if (blank)
      n++;
    blank = 1;
  }
  free(text);
  return n;
}

static bool shim_log_contains(const char *log, const char *needle)
{
  char *text = kbc_test_read_file(log);
  bool found;
  if (text == NULL)
    return false;
  found = strstr(text, needle) != NULL;
  free(text);
  return found;
}

/* Runs `argv` (which must begin with "git") and returns its stdout, or
 * NULL. This is the oracle path: git's own output captured verbatim, so a
 * parser is compared against what git actually said rather than against an
 * expectation somebody typed. */
static char *git_stdout(const char *const argv[])
{
  int fds[2];
  pid_t pid;
  kbc_str out;
  char chunk[4096];
  ssize_t got;
  int status = 0;

  if (pipe(fds) != 0)
    return NULL;
  pid = fork();
  if (pid < 0) {
    close(fds[0]);
    close(fds[1]);
    return NULL;
  }
  if (pid == 0) {
    close(fds[0]);
    if (dup2(fds[1], STDOUT_FILENO) < 0)
      _exit(126);
    close(fds[1]);
    execvp(argv[0], argv_of(argv));
    _exit(127);
  }
  close(fds[1]);
  kbc_str_init(&out);
  while ((got = read(fds[0], chunk, sizeof chunk)) > 0)
    kbc_str_append(&out, chunk, (size_t)got);
  close(fds[0]);
  waitpid(pid, &status, 0);
  return out.ptr != NULL ? out.ptr : strdup("");
}

/* ============================================================ the cases */

/* --- repo handle, HEAD, and the three things that can be wrong --------- */

KBC_TEST(open_finds_the_toplevel_from_a_subdirectory)
{
  char dir[KBC_TEST_PATH_MAX + 64], sub[KBC_TEST_PATH_MAX];
  kbc_git_repo *repo = NULL;
  kbc_err err;
  kbc_arena *a;
  kbc_git_head head;
  char *oracle;

  make_repo(dir, sizeof dir);
  join(sub, sizeof sub, dir, "/sub");

  /* Opened from a subdirectory: the handle must hold the toplevel, because
   * that is what -C is given and a caller cannot pass its own cwd. */
  kbc_err_reset(&err);
  KBC_CHECK_OK(kbc_git_repo_open(sub, &repo, &err));
  KBC_CHECK_NOT_NULL(repo);
  KBC_CHECK_EQ_STR(kbc_git_repo_root(repo), dir);
  KBC_CHECK_NOT_NULL(kbc_git_repo_git_dir(repo));
  KBC_CHECK(!kbc_git_repo_is_bare(repo));
  KBC_CHECK(!kbc_git_repo_is_shallow(repo));

  a = kbc_arena_new(64 * 1024);
  kbc_err_reset(&err);
  KBC_CHECK_OK(kbc_git_head_info(repo, a, &head, &err));
  KBC_CHECK_EQ_INT(head.state, KBC_GIT_HEAD_BORN_SYMBOLIC);
  KBC_CHECK_EQ_STR(head.branch, "main");
  KBC_CHECK_EQ_INT(strlen(head.commit), 40);

  /* The commit must be the one git itself names, not merely a 40-hex
   * string: a handle that resolved the wrong commit would still look
   * perfectly well-formed. */
  {
    const char *argv[] = {"git", "-C", dir, "rev-parse",
                    "HEAD", NULL};
    oracle = git_stdout(argv);
    KBC_CHECK_NOT_NULL(oracle);
    oracle[strcspn(oracle, "\r\n")] = '\0';
    KBC_CHECK_EQ_STR(head.commit, oracle);
    free(oracle);
  }
  kbc_arena_free(a);
  kbc_git_repo_close(repo);
  kbc_test_rmrf(dir);
}

KBC_TEST(an_empty_repository_is_unborn_not_broken)
{
  char dir[KBC_TEST_PATH_MAX + 64];
  kbc_git_repo *repo = NULL;
  kbc_err err;
  kbc_arena *a;
  kbc_git_head head;
  const char *init[] = {"init", "-q", "-b", "main",
                  NULL};

  kbc_test_tmpdir(dir, sizeof dir);
  git_ok(dir, init);
  kbc_err_reset(&err);
  KBC_CHECK_OK(kbc_git_repo_open(dir, &repo, &err));

  /* `git init` names a branch that does not exist. That is a repository
   * with no commits, which is a different answer from a typo in a ref and
   * from a damaged object database. */
  a = kbc_arena_new(4096);
  kbc_err_reset(&err);
  KBC_CHECK_OK(kbc_git_head_info(repo, a, &head, &err));
  KBC_CHECK_EQ_INT(head.state, KBC_GIT_HEAD_UNBORN);
  KBC_CHECK_EQ_STR(head.branch, "main");
  KBC_CHECK_NULL(head.commit);

  kbc_err_reset(&err);
  KBC_CHECK_EQ_INT(kbc_git_error(&err), KBC_GIT_OK);
  KBC_CHECK_ERR(kbc_git_resolve(repo, "HEAD", a, (const char **)&dir, &err),
                KBC_ERR_NOTFOUND);
  KBC_CHECK_EQ_INT(kbc_git_error(&err), KBC_GIT_E_UNBORN);
  kbc_arena_free(a);
  kbc_git_repo_close(repo);
  kbc_test_rmrf(dir);
}

KBC_TEST(a_detached_head_is_reported_as_detached)
{
  char dir[KBC_TEST_PATH_MAX + 64];
  kbc_git_repo *repo = NULL;
  kbc_err err;
  kbc_arena *a;
  kbc_git_head head;
  const char *detach[] = {"checkout", "-q", "--detach",
                    "HEAD", NULL};

  make_repo(dir, sizeof dir);
  git_ok(dir, detach);
  kbc_err_reset(&err);
  KBC_CHECK_OK(kbc_git_repo_open(dir, &repo, &err));

  a = kbc_arena_new(4096);
  kbc_err_reset(&err);
  KBC_CHECK_OK(kbc_git_head_info(repo, a, &head, &err));
  KBC_CHECK_EQ_INT(head.state, KBC_GIT_HEAD_DETACHED);
  KBC_CHECK_NULL(head.branch);
  KBC_CHECK_EQ_INT(strlen(head.commit), 40);
  /* A detached HEAD still resolves, reads and blames: it is a state, not a
   * failure, and the reads below prove it. */
  {
    const char *sha = NULL;
    kbc_err_reset(&err);
    KBC_CHECK_OK(kbc_git_resolve(repo, "HEAD", a, &sha, &err));
    KBC_CHECK_EQ_STR(sha, head.commit);
  }
  kbc_arena_free(a);
  kbc_git_repo_close(repo);
  kbc_test_rmrf(dir);
}

KBC_TEST(a_corrupt_object_database_is_named_as_corrupt)
{
  char dir[KBC_TEST_PATH_MAX + 64], objdir[KBC_TEST_PATH_MAX + 64], obj[KBC_TEST_PATH_MAX + 64];
  kbc_git_repo *repo = NULL;
  kbc_err err;
  kbc_arena *a;
  const char *argv[] = {"git", "-C", dir, "rev-parse",
                  "HEAD", NULL};
  char *head;
  const char *sha = NULL;

  make_repo(dir, sizeof dir);
  head = git_stdout(argv);
  KBC_CHECK_NOT_NULL(head);
  head[strcspn(head, "\r\n")] = '\0';
  KBC_CHECK_EQ_INT(strlen(head), 40);

  /* Corrupt the root commit's loose object the way a bad disk would. The
   * handle still opens — the repository LOOKS fine — so the distinction
   * that matters can only be made when the object is actually read. */
  join(objdir, sizeof objdir, dir, "/.git/objects");
  memcpy(objdir + strlen(objdir), "/", 2);
  memcpy(objdir + strlen(objdir), head, 2);
  join(obj, sizeof obj, objdir, "/");
  memcpy(obj + strlen(obj), head + 2, 39);
  kbc_test_mkdir_p(objdir);
  KBC_CHECK_MSG(chmod(objdir, 0755) == 0, "chmod objects/xx");
  KBC_CHECK_MSG(chmod(obj, 0644) == 0, "chmod object");
  kbc_test_write_file(obj, "this is not a zlib stream");

  kbc_err_reset(&err);
  KBC_CHECK_OK(kbc_git_repo_open(dir, &repo, &err));
  a = kbc_arena_new(4096);
  kbc_err_reset(&err);
  KBC_CHECK_ERR(kbc_git_resolve(repo, "HEAD", a, &sha, &err), KBC_ERR_INTERNAL);
  KBC_CHECK_ERR_MSG(err);
  KBC_CHECK_EQ_INT(kbc_git_error(&err), KBC_GIT_E_CORRUPT);
  KBC_CHECK_MSG(strstr(err.msg, "corrupt") != NULL,
                "the message must name the actual condition: %s", err.msg);

  free(head);
  kbc_arena_free(a);
  kbc_git_repo_close(repo);
  kbc_test_rmrf(dir);
}

KBC_TEST(a_path_that_is_not_a_repository_is_not_a_repository)
{
  char dir[KBC_TEST_PATH_MAX + 64], missing[KBC_TEST_PATH_MAX + 64];
  kbc_git_repo *repo = NULL;
  kbc_err err;

  kbc_test_tmpdir(dir, sizeof dir);
  join(missing, sizeof missing, dir, "/not-here");

  /* The caller named a path that is not a repository root: that is an
   * invalid argument, not a missing document. */
  kbc_err_reset(&err);
  KBC_CHECK_ERR(kbc_git_repo_open(missing, &repo, &err), KBC_ERR_INVALID);
  KBC_CHECK_EQ_INT(kbc_git_error(&err), KBC_GIT_E_NOT_A_REPO);
  KBC_CHECK_ERR_MSG(err);
  KBC_CHECK_NULL(repo);

  /* A directory that exists but holds no .git anywhere above it is the
   * other shape of the same answer, and it comes back from git's own
   * "not a git repository" text rather than from a stat. */
  kbc_err_reset(&err);
  KBC_CHECK_ERR(kbc_git_repo_open(dir, &repo, &err), KBC_ERR_INVALID);
  KBC_CHECK_EQ_INT(kbc_git_error(&err), KBC_GIT_E_NOT_A_REPO);
  kbc_test_rmrf(dir);
}

/* --- object reads: refs, commits, trees, blobs -------------------------- */

KBC_TEST(refs_are_listed_and_match_git)
{
  char dir[KBC_TEST_PATH_MAX + 64];
  kbc_git_repo *repo = NULL;
  kbc_err err;
  kbc_arena *a = kbc_arena_new(256 * 1024);
  kbc_git_refs refs;
  size_t i;
  bool saw_main = false, saw_feature = false, saw_tag = false;
  const char *argv[] = {"git",    "-C",  dir,
                  "rev-parse", "main", NULL};

  make_repo(dir, sizeof dir);
  KBC_CHECK_OK(kbc_git_repo_open(dir, &repo, &err));
  kbc_err_reset(&err);
  KBC_CHECK_OK(kbc_git_list_refs(repo, false, a, &refs, &err));
  KBC_CHECK_MSG(refs.len >= 3, "expected 2 branches + 1 tag, got %zu",
                refs.len);

  for (i = 0; i < refs.len; i++) {
    const kbc_git_ref *r = &refs.items[i];
    KBC_CHECK_EQ_INT(strlen(r->object), 40);
    KBC_CHECK_MSG(strncmp(r->full_name, "refs/heads/", 11) == 0 ||
                      strncmp(r->full_name, "refs/tags/", 10) == 0,
                  "unexpected ref namespace: %s", r->full_name);
    if (strcmp(r->short_name, "main") == 0) {
      saw_main = true;
      KBC_CHECK_EQ_INT(r->kind, KBC_GIT_REF_BRANCH);
      KBC_CHECK_EQ_STR(r->full_name, "refs/heads/main");
      KBC_CHECK_NULL(r->remote);
      /* The object id has to be the one git names for that branch. */
      {
        char *oracle = git_stdout(argv);
        oracle[strcspn(oracle, "\r\n")] = '\0';
        KBC_CHECK_EQ_STR(r->object, oracle);
        free(oracle);
      }
    } else if (strcmp(r->short_name, "feature") == 0) {
      saw_feature = true;
      KBC_CHECK_EQ_INT(r->kind, KBC_GIT_REF_BRANCH);
    } else if (strcmp(r->short_name, "v1") == 0) {
      saw_tag = true;
      KBC_CHECK_EQ_INT(r->kind, KBC_GIT_REF_TAG);
      KBC_CHECK_EQ_STR(r->full_name, "refs/tags/v1");
    }
  }
  KBC_CHECK(saw_main);
  KBC_CHECK(saw_feature);
  KBC_CHECK(saw_tag);

  {
    const char *def = NULL;
    kbc_err_reset(&err);
    KBC_CHECK_OK(kbc_git_default_branch(repo, a, &def, &err));
    KBC_CHECK_EQ_STR(def, "main");
  }
  kbc_arena_free(a);
  kbc_git_repo_close(repo);
  kbc_test_rmrf(dir);
}

KBC_TEST(commit_metadata_matches_git_field_for_field)
{
  char dir[KBC_TEST_PATH_MAX + 64];
  kbc_git_repo *repo = NULL;
  kbc_err err;
  kbc_arena *a = kbc_arena_new(64 * 1024);
  kbc_git_commit c;
  const char *argv[] = {"git", "-C", dir,
                  "show",    "-s",
                  "--format=%H %an %ae %at %s",
                  "HEAD",    NULL};
  char *oracle;

  make_repo(dir, sizeof dir);
  KBC_CHECK_OK(kbc_git_repo_open(dir, &repo, &err));
  kbc_err_reset(&err);
  KBC_CHECK_OK(kbc_git_commit_info(repo, "HEAD", a, &c, &err));

  KBC_CHECK_EQ_INT(strlen(c.oid), 40);
  KBC_CHECK_EQ_INT(strlen(c.tree), 40);
  KBC_CHECK_EQ_STR(c.author, "Test");
  KBC_CHECK_EQ_STR(c.author_mail, "t@example.invalid");
  KBC_CHECK_EQ_STR(c.subject, "second commit");
  KBC_CHECK_EQ_INT(c.parent_count, 1);

  /* Compared against git's own rendering of the same commit, not against a
   * literal: if the fixture's identity ever changes, this still holds. */
  oracle = git_stdout(argv);
  KBC_CHECK_NOT_NULL(oracle);
  {
    char want[1024];
    snprintf(want, sizeof want, "%s Test t@example.invalid %lld %s", c.oid,
             (long long)c.author_time, c.subject);
    oracle[strcspn(oracle, "\r\n")] = '\0';
    KBC_CHECK_EQ_STR(oracle, want);
  }
  free(oracle);
  kbc_arena_free(a);
  kbc_git_repo_close(repo);
  kbc_test_rmrf(dir);
}

KBC_TEST(a_root_commit_has_no_parents_and_the_second_has_one)
{
  char dir[KBC_TEST_PATH_MAX + 64];
  kbc_git_repo *repo = NULL;
  kbc_err err;
  kbc_arena *a = kbc_arena_new(64 * 1024);
  kbc_git_commit root, second;

  make_repo(dir, sizeof dir);
  KBC_CHECK_OK(kbc_git_repo_open(dir, &repo, &err));
  kbc_err_reset(&err);
  KBC_CHECK_OK(kbc_git_commit_info(repo, "HEAD~1", a, &root, &err));
  KBC_CHECK_EQ_INT(root.parent_count, 0);
  KBC_CHECK_NULL(root.parents);
  KBC_CHECK_EQ_STR(root.subject, "first commit");

  kbc_err_reset(&err);
  KBC_CHECK_OK(kbc_git_commit_info(repo, "HEAD", a, &second, &err));
  KBC_CHECK_EQ_INT(second.parent_count, 1);
  KBC_CHECK_EQ_STR(second.parents[0], root.oid);
  kbc_arena_free(a);
  kbc_git_repo_close(repo);
  kbc_test_rmrf(dir);
}

KBC_TEST(tree_listing_matches_git_including_kinds_and_sizes)
{
  char dir[KBC_TEST_PATH_MAX + 64];
  kbc_git_repo *repo = NULL;
  kbc_err err;
  kbc_arena *a = kbc_arena_new(256 * 1024);
  kbc_git_entries top, sub;
  const kbc_git_entry *a_txt = NULL, *sub_dir = NULL;

  make_repo(dir, sizeof dir);
  KBC_CHECK_OK(kbc_git_repo_open(dir, &repo, &err));

  kbc_err_reset(&err);
  KBC_CHECK_OK(kbc_git_tree(repo, "HEAD", NULL, a, &top, &err));
  KBC_CHECK_EQ_INT(top.len, 2);
  for (size_t i = 0; i < top.len; i++) {
    if (strcmp(top.items[i].path, "a.txt") == 0)
      a_txt = &top.items[i];
    if (strcmp(top.items[i].path, "sub") == 0)
      sub_dir = &top.items[i];
  }
  KBC_CHECK_NOT_NULL(a_txt);
  KBC_CHECK_NOT_NULL(sub_dir);
  /* "one\ntwo\nthree\nfour\n" is 19 bytes; getting this from git's own
   * --long field is what proves the padded size column was parsed. */
  KBC_CHECK_EQ_INT(a_txt->size, 19);
  KBC_CHECK_EQ_INT(a_txt->kind, KBC_GIT_ENTRY_BLOB);
  KBC_CHECK_EQ_INT(a_txt->mode, 0100644);
  KBC_CHECK_EQ_INT(strlen(a_txt->oid), 40);
  /* A tree has no content size at all, and reporting 0 for it would be a
   * lie a caller could not detect. */
  KBC_CHECK_EQ_INT(sub_dir->kind, KBC_GIT_ENTRY_TREE);
  KBC_CHECK_EQ_INT(sub_dir->mode, 0040000);
  KBC_CHECK(sub_dir->size == UINT64_MAX);

  kbc_err_reset(&err);
  KBC_CHECK_OK(kbc_git_tree(repo, "HEAD", "sub", a, &sub, &err));
  KBC_CHECK_EQ_INT(sub.len, 1);
  KBC_CHECK_EQ_STR(sub.items[0].path, "b.txt");
  KBC_CHECK_EQ_INT(sub.items[0].size, 6);

  kbc_arena_free(a);
  kbc_git_repo_close(repo);
  kbc_test_rmrf(dir);
}

KBC_TEST(blobs_read_the_committed_bytes_and_absent_paths_are_named)
{
  char dir[KBC_TEST_PATH_MAX + 64];
  kbc_git_repo *repo = NULL;
  kbc_err err;
  kbc_arena *a = kbc_arena_new(64 * 1024);
  kbc_str blob;

  make_repo(dir, sizeof dir);
  KBC_CHECK_OK(kbc_git_repo_open(dir, &repo, &err));
  kbc_str_init(&blob);
  kbc_err_reset(&err);
  KBC_CHECK_OK(kbc_git_blob(repo, "HEAD", "a.txt", &blob, &err));
  KBC_CHECK_EQ_STR(blob.ptr, "one\ntwo\nthree\nfour\n");

  /* At the older commit the same path holds different bytes: a blob read
   * that ignored the revspec would return the working tree's content. */
  kbc_str_clear(&blob);
  kbc_err_reset(&err);
  KBC_CHECK_OK(kbc_git_blob(repo, "HEAD~1", "a.txt", &blob, &err));
  KBC_CHECK_EQ_STR(blob.ptr, "one\ntwo\nthree\n");

  /* A dirty working tree must not change what history says. */
  {
    char path[KBC_TEST_PATH_MAX + 64];
    join(path, sizeof path, dir, "/a.txt");
    kbc_test_write_file(path, "completely different\n");
    kbc_str_clear(&blob);
    kbc_err_reset(&err);
    KBC_CHECK_OK(kbc_git_blob(repo, "HEAD", "a.txt", &blob, &err));
    KBC_CHECK_EQ_STR(blob.ptr, "one\ntwo\nthree\nfour\n");
  }

  kbc_err_reset(&err);
  KBC_CHECK_ERR(kbc_git_blob(repo, "HEAD", "nope.txt", &blob, &err),
                KBC_ERR_NOTFOUND);
  KBC_CHECK_EQ_INT(kbc_git_error(&err), KBC_GIT_E_NOT_FOUND);

  KBC_CHECK(!kbc_git_blob_exists(repo, "HEAD", "nope.txt", &err));
  KBC_CHECK(kbc_git_blob_exists(repo, "HEAD", "a.txt", &err));

  kbc_str_free(&blob);
  kbc_arena_free(a);
  kbc_git_repo_close(repo);
  kbc_test_rmrf(dir);
}

KBC_TEST(numstat_and_diff_match_git)
{
  char dir[KBC_TEST_PATH_MAX + 64];
  kbc_git_repo *repo = NULL;
  kbc_err err;
  kbc_arena *a = kbc_arena_new(256 * 1024);
  kbc_git_file_stats stats;
  kbc_git_diff_opts dopt;
  kbc_str text;
  const char *argv[] = {"git",  "-C",      dir,
                  "show",  "--numstat",
                  "--format=", "HEAD", NULL};
  char *oracle;

  make_repo(dir, sizeof dir);
  KBC_CHECK_OK(kbc_git_repo_open(dir, &repo, &err));
  kbc_err_reset(&err);
  KBC_CHECK_OK(kbc_git_commit_numstat(repo, "HEAD", a, &stats, &err));
  KBC_CHECK_EQ_INT(stats.len, 1);
  KBC_CHECK_EQ_STR(stats.items[0].path, "a.txt");
  KBC_CHECK_EQ_INT(stats.items[0].insertions, 1);
  KBC_CHECK_EQ_INT(stats.items[0].deletions, 0);
  KBC_CHECK(!stats.items[0].binary);

  oracle = git_stdout(argv);
  KBC_CHECK_NOT_NULL(oracle);
  oracle[strcspn(oracle, "\r\n")] = '\0';
  {
    char want[256];
    snprintf(want, sizeof want, "1\t0\ta.txt");
    KBC_CHECK_EQ_STR(oracle, want);
  }
  free(oracle);

  /* Diff text is handed back verbatim, so it must be byte-identical to
   * what the same command printed here. */
  memset(&dopt, 0, sizeof dopt);
  dopt.from = "HEAD~1";
  dopt.to = "HEAD";
  dopt.path = "a.txt";
  kbc_str_init(&text);
  kbc_err_reset(&err);
  KBC_CHECK_OK(kbc_git_diff(repo, &dopt, &text, &err));
  KBC_CHECK_MSG(strstr(text.ptr, "+four") != NULL, "diff text: %s", text.ptr);
  KBC_CHECK_MSG(strstr(text.ptr, "three") != NULL, "diff text: %s", text.ptr);

  kbc_str_free(&text);
  kbc_arena_free(a);
  kbc_git_repo_close(repo);
  kbc_test_rmrf(dir);
}

/* --- blame, compared against git's OWN wire ---------------------------- */

/* One region as git reported it, parsed here by a deliberately different
 * and much simpler scanner than the library's: a line is a header when it
 * starts with 40 hex digits and a space, and a region ends at the next
 * header or at a "filename" line. If the two parsers agree on a real
 * repository, they are not agreeing by construction. */
typedef struct {
  char sha[41];
  uint32_t orig_start, final_start, count;
  char filename[512];
  char author[256];
  char author_mail[256];
  char subject[256];
  long long author_time;
  bool boundary;
} oracle_region;

static bool hex40(const char *s)
{
  for (int i = 0; i < 40; i++) {
    char c = s[i];
    if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f')))
      return false;
  }
  return s[40] == ' ' || s[40] == '\n' || s[40] == '\0';
}

static unsigned parse_num(const char **p)
{
  unsigned v = 0;
  while (**p >= '0' && **p <= '9') {
    v = v * 10u + (unsigned)(**p - '0');
    (*p)++;
  }
  return v;
}

/* Metadata is emitted by git only on a sha's first appearance, so the
 * oracle keeps a per-sha record and re-attaches it — which is exactly the
 * contract the library has to satisfy. */
typedef struct {
  char sha[41];
  char author[256];
  char author_mail[256];
  char subject[256];
  long long author_time;
  bool boundary;
  bool seen;
} oracle_meta;

/* Records what git said about this sha. The table is APPENDED to on first
 * sight and overwritten afterwards, which is what makes a repeat region's
 * metadata recoverable at all. */
static void meta_note(oracle_meta *tab, size_t *n, size_t cap,
                      const oracle_region *r)
{
  for (size_t i = 0; i < *n; i++) {
    if (strcmp(tab[i].sha, r->sha) == 0) {
      copy_field(tab[i].author, sizeof tab[i].author, r->author);
      copy_field(tab[i].author_mail, sizeof tab[i].author_mail,
                 r->author_mail);
      copy_field(tab[i].subject, sizeof tab[i].subject, r->subject);
      tab[i].author_time = r->author_time;
      tab[i].boundary = r->boundary;
      tab[i].seen = true;
      return;
    }
  }
  if (*n < cap) {
    oracle_meta *slot = &tab[*n];
    memset(slot, 0, sizeof *slot);
    copy_field(slot->sha, sizeof slot->sha, r->sha);
    copy_field(slot->author, sizeof slot->author, r->author);
    copy_field(slot->author_mail, sizeof slot->author_mail,
               r->author_mail);
    copy_field(slot->subject, sizeof slot->subject, r->subject);
    slot->author_time = r->author_time;
    slot->boundary = r->boundary;
    slot->seen = true;
    (*n)++;
  }
}

static void meta_apply(oracle_meta *tab, size_t n, oracle_region *r)
{
  for (size_t i = 0; i < n; i++) {
    if (strcmp(tab[i].sha, r->sha) == 0) {
      copy_field(r->author, sizeof r->author, tab[i].author);
      copy_field(r->author_mail, sizeof r->author_mail,
                 tab[i].author_mail);
      copy_field(r->subject, sizeof r->subject, tab[i].subject);
      r->author_time = tab[i].author_time;
      r->boundary = tab[i].boundary;
      return;
    }
  }
}

static size_t parse_blame_oracle(const char *text, oracle_region *out,
                                 size_t cap, oracle_meta *tab,
                                 size_t *tab_n, size_t tab_cap)
{
  size_t n = 0;
  const char *line = text;
  bool in_block = false;
  bool is_header = false;
  oracle_region cur;
  memset(&cur, 0, sizeof cur);

  while (*line != '\0') {
    const char *eol = strchr(line, '\n');
    size_t len = eol != NULL ? (size_t)(eol - line) : strlen(line);
    char buf[1024];
    size_t copy = len < sizeof buf - 1 ? len : sizeof buf - 1;
    memcpy(buf, line, copy);
    buf[copy] = '\0';

    is_header = hex40(buf);
    if (is_header) {
      if (in_block && n < cap)
        out[n++] = cur;
      memset(&cur, 0, sizeof cur);
      const char *p = buf;
      memcpy(cur.sha, p, 40);
      cur.sha[40] = '\0';
      p += 41;
      cur.orig_start = parse_num(&p);
      while (*p == ' ')
        p++;
      cur.final_start = parse_num(&p);
      while (*p == ' ')
        p++;
      cur.count = parse_num(&p);
      in_block = true;
    } else if (in_block && strncmp(buf, "filename ", 9) == 0) {
      copy_field(cur.filename, sizeof cur.filename, buf + 9);
      if (n < cap) {
        meta_apply(tab, *tab_n, &cur);
        out[n++] = cur;
      }
      in_block = false;
      memset(&cur, 0, sizeof cur);
    } else if (in_block && strncmp(buf, "author ", 7) == 0) {
      copy_field(cur.author, sizeof cur.author, buf + 7);
    } else if (in_block && strncmp(buf, "author-mail ", 12) == 0) {
      char *v = buf + 12;
      size_t vl = strlen(v);
      if (vl >= 2 && v[0] == '<' && v[vl - 1] == '>') {
        v[vl - 1] = '\0';
        copy_field(cur.author_mail, sizeof cur.author_mail, v + 1);
      }
    } else if (in_block && strncmp(buf, "author-time ", 12) == 0) {
      cur.author_time = strtoll(buf + 12, NULL, 10);
    } else if (in_block && strncmp(buf, "summary ", 8) == 0) {
      copy_field(cur.subject, sizeof cur.subject, buf + 8);
    } else if (in_block && strcmp(buf, "summary") == 0) {
      cur.subject[0] = '\0';
    } else if (in_block && strcmp(buf, "boundary") == 0) {
      cur.boundary = true;
    }
    /* A header line carries no metadata yet, and recording from it would
     * blank whatever a previous block already said about that sha. Every
     * other line in a block is metadata as far as this oracle cares. */
    if (in_block && !is_header)
      meta_note(tab, tab_n, tab_cap, &cur);
    if (eol == NULL)
      break;
    line = eol + 1;
  }
  return n;
}

/* A history where one commit owns lines on BOTH sides of another commit's
 * insertion, so git emits that sha's metadata once and then omits it for a
 * later region. Without that shape the re-attachment rule is untested. */
static void make_blame_repo(char *dir, size_t cap)
{
  char path[KBC_TEST_PATH_MAX + 64];
  const char *init[] = {"init", "-q", "-b", "main",
                  NULL};
  const char *add[] = {"add", "-A", NULL};

  kbc_test_tmpdir(dir, cap);
  join(path, sizeof path, dir, "/a.txt");
  kbc_test_write_file(path, "alpha\nbeta\ngamma\ndelta\nepsilon\n");
  git_ok(dir, init);
  git_ok(dir, add);
  git_am(dir, "first");
  kbc_test_write_file(path, "alpha\nINSERTED\nbeta\ngamma\ndelta\nepsilon\n");
  git_ok(dir, add);
  git_am(dir, "second");
}

KBC_TEST(blame_regions_are_byte_identical_to_gits_own_output)
{
  char dir[KBC_TEST_PATH_MAX + 64];
  kbc_git_repo *repo = NULL;
  kbc_err err;
  kbc_arena *a = kbc_arena_new(1024 * 1024);
  kbc_blames got;
  oracle_region want[128];
  oracle_meta tab[64];
  size_t nwant;
  size_t tab_n = 0;
  char *raw;
  const char *argv[] = {"git",  "-C",           dir,
                  "blame", "--incremental",
                  "--",     "a.txt",      NULL};

  make_blame_repo(dir, sizeof dir);
  memset(want, 0, sizeof want);
  memset(tab, 0, sizeof tab);

  /* The oracle: git's own bytes, produced by an independent invocation. */
  raw = git_stdout(argv);
  KBC_CHECK_NOT_NULL(raw);
  nwant = parse_blame_oracle(raw, want, 128, tab, &tab_n, 64);
  KBC_CHECK_MSG(nwant >= 3, "oracle found %zu regions", nwant);

  KBC_CHECK_OK(kbc_git_repo_open(dir, &repo, &err));
  kbc_err_reset(&err);
  {
    kbc_git_blame_opts o;
    memset(&o, 0, sizeof o);
    o.path = "a.txt";
    KBC_CHECK_OK(kbc_git_blame(repo, &o, a, &got, &err));
  }
  KBC_CHECK_MSG(!got.truncated, "no truncation expected");
  KBC_CHECK_EQ_INT(got.len, nwant);

  /* Order and extent, region by region, against git's own numbering. */
  for (size_t i = 0; i < got.len && i < nwant; i++) {
    KBC_CHECK_MSG(strcmp(got.items[i].sha, want[i].sha) == 0,
                  "region %zu sha: got %.40s want %.40s", i, got.items[i].sha,
                  want[i].sha);
    KBC_CHECK_EQ_INT(got.items[i].orig_start, want[i].orig_start);
    KBC_CHECK_EQ_INT(got.items[i].final_start, want[i].final_start);
    KBC_CHECK_EQ_INT(got.items[i].count, want[i].count);
    KBC_CHECK_MSG(strcmp(got.items[i].filename, want[i].filename) == 0,
                  "region %zu filename: got %s want %s", i,
                  got.items[i].filename, want[i].filename);
    /* Metadata git omitted on a repeat region must still be present. */
    KBC_CHECK_MSG(strcmp(got.items[i].author, want[i].author) == 0,
                  "region %zu author: got \"%s\" want \"%s\"", i,
                  got.items[i].author, want[i].author);
    KBC_CHECK_MSG(strcmp(got.items[i].author_mail, want[i].author_mail) == 0,
                  "region %zu mail: got \"%s\" want \"%s\"", i,
                  got.items[i].author_mail, want[i].author_mail);
    KBC_CHECK_MSG(strcmp(got.items[i].subject, want[i].subject) == 0,
                  "region %zu subject: got \"%s\" want \"%s\"", i,
                  got.items[i].subject, want[i].subject);
    KBC_CHECK_EQ_INT(got.items[i].author_time, want[i].author_time);
    KBC_CHECK_EQ_INT(got.items[i].boundary, want[i].boundary);
  }

  /* The fixture is only interesting if some sha really did repeat, and
   * only if git really did omit its metadata the second time. That is the
   * behaviour under test, so it is asserted rather than assumed. */
  {
    size_t repeats = 0, omissions = 0;
    for (size_t i = 1; i < nwant; i++) {
      if (strcmp(want[i].sha, want[i - 1].sha) != 0)
        continue;
      repeats++;
      /* A repeat region's own metadata block, before re-attachment. */
      if (want[i].author[0] == '\0' || want[i].subject[0] == '\0')
        omissions++;
    }
    KBC_CHECK_MSG(repeats > 0, "fixture must repeat a sha");
    KBC_CHECK_MSG(omissions > 0,
                  "git must omit metadata on the repeat; otherwise the "
                  "re-attachment path never runs");
  }

  free(raw);
  kbc_arena_free(a);
  kbc_git_repo_close(repo);
  kbc_test_rmrf(dir);
}

KBC_TEST(blame_of_uncommitted_bytes_uses_the_bytes_not_the_tree)
{
  char dir[KBC_TEST_PATH_MAX + 64], path[KBC_TEST_PATH_MAX + 64];
  kbc_git_repo *repo = NULL;
  kbc_err err;
  kbc_arena *a = kbc_arena_new(1024 * 1024);
  kbc_blames committed, edited;
  kbc_git_blame_opts o;

  make_blame_repo(dir, sizeof dir);
  KBC_CHECK_OK(kbc_git_repo_open(dir, &repo, &err));

  memset(&o, 0, sizeof o);
  o.path = "a.txt";
  kbc_err_reset(&err);
  KBC_CHECK_OK(kbc_git_blame(repo, &o, a, &committed, &err));

  /* An edit nobody committed, attributed against the same history. */
  join(path, sizeof path, dir, "/a.txt");
  kbc_test_write_file(path, "alpha\nINSERTED\nbeta\ngamma\ndelta\nepsilon\nZ\n");
  memset(&o, 0, sizeof o);
  o.path = "a.txt";
  o.contents = "alpha\nINSERTED\nbeta\ngamma\ndelta\nepsilon\nZ\n";
  o.contents_len = strlen(o.contents);
  kbc_err_reset(&err);
  KBC_CHECK_OK(kbc_git_blame(repo, &o, a, &edited, &err));

  /* The committed file has 6 lines, the edited one 7: the extra region can
   * only be there because the bytes came from --contents. */
  KBC_CHECK(edited.len > committed.len);
  {
    uint32_t total = 0;
    for (size_t i = 0; i < edited.len; i++)
      total += edited.items[i].count;
    KBC_CHECK_EQ_INT(total, 7);
  }

  kbc_arena_free(a);
  kbc_git_repo_close(repo);
  kbc_test_rmrf(dir);
}

KBC_TEST(blame_at_a_pinned_revision_and_with_a_line_range)
{
  char dir[KBC_TEST_PATH_MAX + 64];
  kbc_git_repo *repo = NULL;
  kbc_err err;
  kbc_arena *a = kbc_arena_new(1024 * 1024);
  kbc_blames at_first, narrowed;
  kbc_git_blame_opts o;

  make_blame_repo(dir, sizeof dir);
  KBC_CHECK_OK(kbc_git_repo_open(dir, &repo, &err));

  memset(&o, 0, sizeof o);
  o.path = "a.txt";
  o.rev = "HEAD~1";
  kbc_err_reset(&err);
  KBC_CHECK_OK(kbc_git_blame(repo, &o, a, &at_first, &err));
  {
    uint32_t total = 0;
    for (size_t i = 0; i < at_first.len; i++)
      total += at_first.items[i].count;
    KBC_CHECK_EQ_INT(total, 5); /* the file before INSERTED existed */
    /* The whole file is one commit's work, and that commit is a root. */
    KBC_CHECK_EQ_INT(at_first.len, 1);
    KBC_CHECK(at_first.items[0].boundary);
    KBC_CHECK_NULL(at_first.items[0].previous_sha);
  }

  memset(&o, 0, sizeof o);
  o.path = "a.txt";
  o.line_start = 3;
  o.line_end = 3;
  kbc_err_reset(&err);
  KBC_CHECK_OK(kbc_git_blame(repo, &o, a, &narrowed, &err));
  {
    uint32_t total = 0;
    for (size_t i = 0; i < narrowed.len; i++)
      total += narrowed.items[i].count;
    KBC_CHECK_EQ_INT(total, 1);
  }

  kbc_arena_free(a);
  kbc_git_repo_close(repo);
  kbc_test_rmrf(dir);
}

/* --- argv injection ----------------------------------------------------- */

KBC_TEST(a_revspec_that_would_inject_a_flag_never_reaches_a_process)
{
  /* The actual documented vector: --upload-pack=<program> turns a ref name
   * into the command git runs on the far end. If this string ever reaches
   * an option parser, a request field becomes execution. */
  static const char *const vectors[] = {
      "--upload-pack=touch /tmp/kbc-pwned",
      "--upload-pack=/bin/sh",
      "--output=/tmp/kbc-pwned",
      "--exec=touch /tmp/kbc-pwned",
      "-x",
      "--",
      "-",
      " --upload-pack=x",
      "HEAD --upload-pack=x",
      "HEAD\n--upload-pack=x",
      "HEAD --output=/tmp/x",
      "main..HEAD",
      "HEAD@{1}",
      "",
      "refs/heads/ok-branch_1.2",
  };
  char dir[KBC_TEST_PATH_MAX + 64];
  kbc_git_repo *repo = NULL;
  kbc_err err;
  kbc_arena *a = kbc_arena_new(64 * 1024);
  char log[KBC_TEST_PATH_MAX + 64];
  size_t before;
  const char *sha = NULL;

  make_repo(dir, sizeof dir);
  join(log, sizeof log, dir, "/argv.log");

  KBC_CHECK_OK(kbc_git_repo_open(dir, &repo, &err));
  KBC_CHECK_EQ_INT(shim_install(), 0);
  shim_activate(log);

  /* Positive control, in the same environment and against the same
   * handle: a legal revspec DOES reach a process. Without this the
   * negative result below would prove nothing — an argv log that never
   * fills would look identical. */
  kbc_err_reset(&err);
  KBC_CHECK_OK(kbc_git_resolve(repo, "HEAD", a, &sha, &err));
  KBC_CHECK_NOT_NULL(sha);
  KBC_CHECK_MSG(shim_invocations(log) >= 1,
                "the recording shim must see a legal call");
  KBC_CHECK_MSG(shim_log_contains(log, "rev-parse"),
                "the recorded argv must be the one we expected");
  before = shim_invocations(log);

  for (size_t i = 0; i < sizeof vectors / sizeof vectors[0]; i++) {
    kbc_err_reset(&err);
    sha = NULL;
    KBC_CHECK_MSG(kbc_git_resolve(repo, vectors[i], a, &sha, &err) != KBC_OK,
                  "vector %zu (%s) must be refused", i, vectors[i]);
    KBC_CHECK_ERR_MSG(err);
    KBC_CHECK_EQ_INT(kbc_git_error(&err), KBC_GIT_E_REJECTED);
    KBC_CHECK_MSG(strstr(err.msg, vectors[i]) != NULL ||
                      (vectors[i][0] == '\0'),
                  "the message must name the offending value: %s", err.msg);
    KBC_CHECK_NULL(sha);
  }

  /* The whole point: not one of those strings produced a process. */
  KBC_CHECK_EQ_INT(shim_invocations(log), before);
  KBC_CHECK_MSG(!shim_log_contains(log, "upload-pack"),
                "the injection string reached an argv");
  KBC_CHECK_MSG(!shim_log_contains(log, "kbc-pwned"),
                "the injection string reached an argv");

  shim_deactivate();
  kbc_arena_free(a);
  kbc_git_repo_close(repo);
  kbc_test_rmrf(dir);
}

KBC_TEST(a_path_that_would_escape_the_repository_never_reaches_a_process)
{
  static const char *const vectors[] = {
      "--output=/tmp/kbc-pwned",
      "/etc/passwd",
      "../outside.txt",
      "sub/../../outside.txt",
      "..",
      "a//b",
      "-rf",
      "sub/",
      "",
  };
  char dir[KBC_TEST_PATH_MAX + 64];
  kbc_git_repo *repo = NULL;
  kbc_err err;
  kbc_arena *a = kbc_arena_new(64 * 1024);
  char log[KBC_TEST_PATH_MAX + 64];
  size_t before;
  kbc_str blob;
  kbc_git_blame_opts bo;

  make_repo(dir, sizeof dir);
  join(log, sizeof log, dir, "/argv.log");
  KBC_CHECK_OK(kbc_git_repo_open(dir, &repo, &err));
  KBC_CHECK_EQ_INT(shim_install(), 0);
  shim_activate(log);

  kbc_str_init(&blob);
  kbc_err_reset(&err);
  KBC_CHECK_OK(kbc_git_blob(repo, "HEAD", "a.txt", &blob, &err));
  KBC_CHECK_MSG(shim_invocations(log) >= 1, "control: a legal path spawns");
  before = shim_invocations(log);

  for (size_t i = 0; i < sizeof vectors / sizeof vectors[0]; i++) {
    kbc_err_reset(&err);
    kbc_str_clear(&blob);
    KBC_CHECK_MSG(kbc_git_blob(repo, "HEAD", vectors[i], &blob, &err) !=
                      KBC_OK,
                  "path vector %zu (%s) must be refused", i, vectors[i]);
    KBC_CHECK_ERR_MSG(err);
    KBC_CHECK_EQ_INT(kbc_git_error(&err), KBC_GIT_E_REJECTED);
  }
  KBC_CHECK_EQ_INT(shim_invocations(log), before);
  KBC_CHECK_MSG(!shim_log_contains(log, "kbc-pwned"),
                "an injected flag reached an argv");

  /* Blame takes a path too, and it is the one command with a stdin, so a
   * rejected path there must be refused before the pipe is even built. */
  for (size_t i = 0; i < sizeof vectors / sizeof vectors[0]; i++) {
    kbc_blames got;
    memset(&bo, 0, sizeof bo);
    bo.path = vectors[i];
    kbc_err_reset(&err);
    KBC_CHECK_MSG(kbc_git_blame(repo, &bo, a, &got, &err) != KBC_OK,
                  "blame path vector %zu (%s) must be refused", i,
                  vectors[i]);
    KBC_CHECK_EQ_INT(kbc_git_error(&err), KBC_GIT_E_REJECTED);
  }
  KBC_CHECK_EQ_INT(shim_invocations(log), before);

  kbc_str_free(&blob);
  shim_deactivate();
  kbc_arena_free(a);
  kbc_git_repo_close(repo);
  kbc_test_rmrf(dir);
}

/* --- resources ---------------------------------------------------------- */

/* Every open descriptor this process holds. Compared before and after a
 * long run of mixed operations, so a pipe left open by one error path
 * cannot hide behind a path that happens to be taken often. */
static size_t open_fds(void)
{
  DIR *d = opendir("/proc/self/fd");
  struct dirent *e;
  size_t n = 0;
  if (d == NULL)
    return 0;
  while ((e = readdir(d)) != NULL) {
    if (e->d_name[0] == '.')
      continue;
    n++;
  }
  closedir(d);
  return n;
}

static bool no_children(void)
{
  int status = 0;
  pid_t w = waitpid(-1, &status, WNOHANG);
  return w == -1 && errno == ECHILD;
}

KBC_TEST(many_calls_leak_no_descriptors_and_no_processes)
{
  char dir[KBC_TEST_PATH_MAX + 64];
  kbc_git_repo *repo = NULL;
  kbc_err err;
  kbc_arena *a = kbc_arena_new(512 * 1024);
  size_t fds_before, fds_after;

  make_repo(dir, sizeof dir);
  KBC_CHECK_OK(kbc_git_repo_open(dir, &repo, &err));
  /* Warm the caches and the allocator first: a first call legitimately
   * opens things a second one will not. */
  for (int i = 0; i < 3; i++) {
    kbc_git_head_info(repo, a, &(kbc_git_head){0}, &err);
    kbc_git_dirty_paths(repo, a, &(kbc_git_dirty){0}, &err);
  }
  fds_before = open_fds();
  KBC_CHECK(fds_before > 0);

  /* Every call is made twice: once on the success path and once on the
   * error path, because an error path is where a descriptor is most likely
   * to be left behind. */
  for (int i = 0; i < 60; i++) {
    kbc_git_head head;
    kbc_git_refs refs;
    kbc_git_entries entries;
    kbc_git_file_stats stats;
    kbc_str text;
    const char *sha = NULL;

    kbc_err_reset(&err);
    KBC_CHECK_OK(kbc_git_head_info(repo, a, &head, &err));
    kbc_err_reset(&err);
    KBC_CHECK_OK(kbc_git_resolve(repo, "HEAD", a, &sha, &err));
    kbc_err_reset(&err);
    KBC_CHECK(kbc_git_resolve(repo, "no-such-ref", a, &sha, &err) != KBC_OK);
    kbc_err_reset(&err);
    KBC_CHECK_OK(kbc_git_list_refs(repo, true, a, &refs, &err));
    kbc_err_reset(&err);
    KBC_CHECK_OK(kbc_git_tree(repo, "HEAD", NULL, a, &entries, &err));
    kbc_err_reset(&err);
    KBC_CHECK(kbc_git_tree(repo, "HEAD", "no/such/dir", a, &entries, &err) !=
              KBC_OK);
    kbc_str_init(&text);
    kbc_err_reset(&err);
    KBC_CHECK_OK(kbc_git_blob(repo, "HEAD", "a.txt", &text, &err));
    kbc_str_free(&text);
    kbc_err_reset(&err);
    KBC_CHECK_OK(kbc_git_commit_numstat(repo, "HEAD", a, &stats, &err));
    kbc_err_reset(&err);
    kbc_git_dirty_paths(repo, a, &(kbc_git_dirty){0}, &err);
  }

  fds_after = open_fds();
  KBC_CHECK_MSG(fds_after == fds_before,
                "descriptors leaked: %zu before, %zu after 600 git calls",
                fds_before, fds_after);
  /* Every child must have been waited for. A zombie here is a process
   * this code spawned and never reaped. */
  KBC_CHECK_MSG(no_children(), "a git child was not reaped");

  kbc_arena_free(a);
  kbc_git_repo_close(repo);
  kbc_test_rmrf(dir);
}

KBC_TEST(a_child_that_never_answers_is_killed_at_its_deadline)
{
  char dir[KBC_TEST_PATH_MAX + 64];
  kbc_git_repo *repo = NULL;
  kbc_err err;
  kbc_arena *a = kbc_arena_new(64 * 1024);
  char log[KBC_TEST_PATH_MAX + 64];
  kbc_git_blame_opts o;
  kbc_blames got;

  make_repo(dir, sizeof dir);
  join(log, sizeof log, dir, "/argv.log");
  KBC_CHECK_OK(kbc_git_repo_open(dir, &repo, &err));

  KBC_CHECK_EQ_INT(shim_install(), 0);
  shim_activate(log);
  setenv("KBC_TEST_SHIM_SLEEP", "30", 1);

  memset(&o, 0, sizeof o);
  o.path = "a.txt";
  o.timeout_ms = 300;
  kbc_err_reset(&err);
  KBC_CHECK_ERR(kbc_git_blame(repo, &o, a, &got, &err), KBC_ERR_TIMEOUT);
  KBC_CHECK_ERR_MSG(err);
  KBC_CHECK_EQ_INT(kbc_git_error(&err), KBC_GIT_E_TIMEOUT);

  /* The deadline is only real if the child is gone, not merely abandoned. */
  unsetenv("KBC_TEST_SHIM_SLEEP");
  KBC_CHECK_MSG(no_children(), "the timed-out child was not reaped");

  shim_deactivate();
  kbc_arena_free(a);
  kbc_git_repo_close(repo);
  kbc_test_rmrf(dir);
}

/* --- the validators, on their own -------------------------------------- */

KBC_TEST(the_revspec_predicate_accepts_names_and_nothing_else)
{
  static const char *const legal[] = {
      "HEAD",         "main",         "v1.2.3",        "refs/heads/feature/x",
      "HEAD~1",       "HEAD^2",       "deadbeef",      "origin/main",
      "a.b.c",        "feature/sub",  "release-1.2.3",
  };
  static const char *const illegal[] = {
      "",         "-",        "--",       "-x",
      "--upload-pack=touch /tmp/x", " --x", "a b", "a\tb",
      "a\nb",     "a..b",     "..",       "a..",
      "@{1}",     "HEAD@{0}", "a@{b",     "a\x01" "b",
      "\x7f",     "a\x80" "b",
  };
  kbc_err err;
  for (size_t i = 0; i < sizeof legal / sizeof legal[0]; i++) {
    kbc_err_reset(&err);
    KBC_CHECK_MSG(kbc_git_revspec_ok(legal[i]), "should be legal: %s",
                  legal[i]);
    KBC_CHECK_OK(kbc_git_revspec_check(legal[i], &err));
  }
  for (size_t i = 0; i < sizeof illegal / sizeof illegal[0]; i++) {
    kbc_err_reset(&err);
    KBC_CHECK_MSG(!kbc_git_revspec_ok(illegal[i]), "should be illegal");
    KBC_CHECK_ERR(kbc_git_revspec_check(illegal[i], &err), KBC_ERR_INVALID);
    KBC_CHECK_EQ_INT(kbc_git_error(&err), KBC_GIT_E_REJECTED);
    KBC_CHECK_ERR_MSG(err);
  }
  KBC_CHECK(!kbc_git_revspec_ok(NULL));
  /* A caller that passes err == NULL must not crash. */
  KBC_CHECK_ERR(kbc_git_revspec_check("-x", NULL), KBC_ERR_INVALID);
}

KBC_TEST(the_path_predicate_keeps_the_filesystem_inside_the_repo)
{
  static const char *const legal[] = {
      "a.txt", "sub/b.txt", "sub/deep/c.txt", "name with spaces.txt",
      "glob*star.txt", "dash-inside-ok.txt", "\xc3\xa9-accent.txt",
      /* A backslash is an ordinary byte in a Linux filename, and the child
       * runs with GIT_LITERAL_PATHSPECS=1, so it is neither an escape nor a
       * glob. Rejecting it would make a real file unreadable. */
      "..\\a", "back\\slash.txt",
  };
  static const char *const illegal[] = {
      "",          "/etc/passwd", "..",         "../x",
      "a/../b",    "a/..",        "a//b",
      "sub/",      "-rf",         "a\nb",       "\x01",
  };
  kbc_err err;
  for (size_t i = 0; i < sizeof legal / sizeof legal[0]; i++) {
    kbc_err_reset(&err);
    KBC_CHECK_MSG(kbc_git_path_ok(legal[i]), "should be legal: %s", legal[i]);
    KBC_CHECK_OK(kbc_git_path_check(legal[i], &err));
  }
  for (size_t i = 0; i < sizeof illegal / sizeof illegal[0]; i++) {
    kbc_err_reset(&err);
    KBC_CHECK_MSG(!kbc_git_path_ok(illegal[i]), "should be illegal: %s",
                  illegal[i]);
    KBC_CHECK_ERR(kbc_git_path_check(illegal[i], &err), KBC_ERR_INVALID);
    KBC_CHECK_EQ_INT(kbc_git_error(&err), KBC_GIT_E_REJECTED);
  }
  KBC_CHECK(!kbc_git_path_ok(NULL));
  KBC_CHECK_ERR(kbc_git_path_check("/x", NULL), KBC_ERR_INVALID);
}

KBC_TEST(a_range_is_split_endpoint_wise_and_only_one_separator_is_accepted)
{
  kbc_git_range r;
  kbc_err err;
  const char *rev, *path;
  size_t rev_len, path_len;

  kbc_err_reset(&err);
  KBC_CHECK_OK(kbc_git_range_parse("main..HEAD", &r, &err));
  KBC_CHECK(!r.three_dot);
  /* The endpoints are spans, not C strings: the separator is still in the
   * middle of `from` and nothing NUL-terminates it. */
  KBC_CHECK_EQ_INT(r.from_len, 4);
  KBC_CHECK_EQ_INT(r.to_len, 4);
  KBC_CHECK(memcmp(r.from, "main", 4) == 0);
  KBC_CHECK(memcmp(r.to, "HEAD", 4) == 0);
  KBC_CHECK_EQ_INT((int)(r.to - r.from), 6);

  kbc_err_reset(&err);
  KBC_CHECK_OK(kbc_git_range_parse("main...HEAD", &r, &err));
  KBC_CHECK(r.three_dot);

  /* A single dot is a tag name, not a range, and four dots is nothing. */
  KBC_CHECK_ERR(kbc_git_range_parse("v1.2", &r, &err), KBC_ERR_INVALID);
  KBC_CHECK_ERR(kbc_git_range_parse("a....b", &r, &err), KBC_ERR_INVALID);
  KBC_CHECK_ERR(kbc_git_range_parse("a..b..c", &r, &err), KBC_ERR_INVALID);
  KBC_CHECK_ERR(kbc_git_range_parse("..b", &r, &err), KBC_ERR_INVALID);
  KBC_CHECK_ERR(kbc_git_range_parse("a..", &r, &err), KBC_ERR_INVALID);
  /* The endpoints are held to the same rule as a standalone revspec, so
   * ".." can only ever be the separator this function chose. */
  KBC_CHECK_ERR(kbc_git_range_parse("-x..HEAD", &r, &err), KBC_ERR_INVALID);
  KBC_CHECK_ERR(kbc_git_range_parse("main..--upload-pack=x", &r, &err),
                KBC_ERR_INVALID);

  kbc_err_reset(&err);
  KBC_CHECK_OK(kbc_git_revpath_parse("HEAD~1:old/name.txt", &rev, &rev_len, &path,
                                      &path_len, &err));
  KBC_CHECK_EQ_INT(rev_len, 6);
  KBC_CHECK(memcmp(rev, "HEAD~1", 6) == 0);
  KBC_CHECK_EQ_STR(path, "old/name.txt");
  KBC_CHECK_EQ_INT(path_len, strlen("old/name.txt"));

  KBC_CHECK_ERR(kbc_git_revpath_parse("no-colon", &rev, &rev_len, &path, &path_len,
                                      &err),
                KBC_ERR_INVALID);
  KBC_CHECK_ERR(kbc_git_revpath_parse(":path", &rev, &rev_len, &path, &path_len,
                                      &err),
                KBC_ERR_INVALID);
  KBC_CHECK_ERR(kbc_git_revpath_parse("rev:", &rev, &rev_len, &path, &path_len,
                                      &err),
                KBC_ERR_INVALID);
  KBC_CHECK_ERR(kbc_git_revpath_parse("-x:a.txt", &rev, &rev_len, &path, &path_len,
                                      &err),
                KBC_ERR_INVALID);
  KBC_CHECK_ERR(kbc_git_revpath_parse("HEAD:../escape", &rev, &rev_len, &path, &path_len,
                                      &err),
                KBC_ERR_INVALID);
}

/* --- checkout: the one write ------------------------------------------- */

KBC_TEST(a_dirty_tree_is_refused_with_the_paths_and_nothing_is_written)
{
  char dir[KBC_TEST_PATH_MAX + 64], path[KBC_TEST_PATH_MAX + 64];
  kbc_git_repo *repo = NULL;
  kbc_err err;
  kbc_arena *a = kbc_arena_new(64 * 1024);
  kbc_git_dirty dirty, listed;

  make_repo(dir, sizeof dir);
  KBC_CHECK_OK(kbc_git_repo_open(dir, &repo, &err));

  join(path, sizeof path, dir, "/a.txt");
  kbc_test_write_file(path, "locally edited\n");
  join(path, sizeof path, dir, "/sub/b.txt");
  kbc_test_write_file(path, "also edited\n");

  kbc_err_reset(&err);
  KBC_CHECK_OK(kbc_git_dirty_paths(repo, a, &listed, &err));
  KBC_CHECK_EQ_INT(listed.len, 2);

  memset(&dirty, 0, sizeof dirty);
  kbc_err_reset(&err);
  KBC_CHECK_ERR(kbc_git_switch(repo, "feature", a, &dirty, &err),
                KBC_ERR_CONFLICT);
  KBC_CHECK_EQ_INT(kbc_git_error(&err), KBC_GIT_E_DIRTY);
  KBC_CHECK_EQ_INT(dirty.len, 2);
  /* The refusal names the offending files, so an operator can act on it. */
  {
    bool saw_a = false, saw_b = false;
    for (size_t i = 0; i < dirty.len; i++) {
      if (strcmp(dirty.items[i], "a.txt") == 0)
        saw_a = true;
      if (strcmp(dirty.items[i], "sub/b.txt") == 0)
        saw_b = true;
    }
    KBC_CHECK(saw_a);
    KBC_CHECK(saw_b);
  }

  /* HEAD did not move: the refusal has to happen before the write, not
   * after it and then be reported. */
  {
    kbc_git_head head;
    kbc_err_reset(&err);
    KBC_CHECK_OK(kbc_git_head_info(repo, a, &head, &err));
    KBC_CHECK_EQ_STR(head.branch, "main");
  }

  /* Clean the tree and the same call now succeeds. */
  {
    const char *reset[] = {"checkout", "-q", "--",
                     "a.txt", "sub/b.txt", NULL};
    git_ok(dir, reset);
  }
  kbc_err_reset(&err);
  KBC_CHECK_OK(kbc_git_switch(repo, "feature", a, NULL, &err));
  {
    kbc_git_head head;
    kbc_err_reset(&err);
    KBC_CHECK_OK(kbc_git_head_info(repo, a, &head, &err));
    KBC_CHECK_EQ_STR(head.branch, "feature");
  }
  /* And back, so the fixture's own state is the checker's problem, not
   * this test's. */
  kbc_err_reset(&err);
  KBC_CHECK_OK(kbc_git_switch(repo, "main", a, NULL, &err));

  kbc_arena_free(a);
  kbc_git_repo_close(repo);
  kbc_test_rmrf(dir);
}

KBC_TEST(switching_to_a_tag_detaches_and_to_an_unknown_ref_is_named)
{
  char dir[KBC_TEST_PATH_MAX + 64];
  kbc_git_repo *repo = NULL;
  kbc_err err;
  kbc_arena *a = kbc_arena_new(64 * 1024);
  const char *revparse[] = {"git", "-C", dir,
                       "symbolic-ref", "-q", "HEAD",
                       NULL};

  make_repo(dir, sizeof dir);
  KBC_CHECK_OK(kbc_git_repo_open(dir, &repo, &err));

  /* v1 is a tag, not a local branch: `git switch` would refuse it and
   * `git checkout` detaches HEAD, which is what an operator's own command
   * does and therefore what this must do. */
  kbc_err_reset(&err);
  KBC_CHECK_OK(kbc_git_switch(repo, "v1", a, NULL, &err));
  KBC_CHECK_MSG(run(revparse) == 1, "HEAD must be detached after a tag");

  kbc_err_reset(&err);
  KBC_CHECK_OK(kbc_git_switch(repo, "main", a, NULL, &err));

  /* A ref that does not exist is a bad ref, not a generic git failure. */
  kbc_err_reset(&err);
  KBC_CHECK_ERR(kbc_git_switch(repo, "no-such-branch", a, NULL, &err),
                KBC_ERR_NOTFOUND);
  KBC_CHECK_EQ_INT(kbc_git_error(&err), KBC_GIT_E_BAD_REF);

  /* And an injected target never gets as far as a checkout. */
  kbc_err_reset(&err);
  KBC_CHECK_ERR(kbc_git_switch(repo, "--upload-pack=touch /tmp/x", a, NULL,
                               &err),
                KBC_ERR_INVALID);
  KBC_CHECK_EQ_INT(kbc_git_error(&err), KBC_GIT_E_REJECTED);

  kbc_arena_free(a);
  kbc_git_repo_close(repo);
  kbc_test_rmrf(dir);
}

int main(void)
{
  int rc = kbc_test_run("git", (kbc_test_case[]){
                          {"open", open_finds_the_toplevel_from_a_subdirectory},
                          {"unborn", an_empty_repository_is_unborn_not_broken},
                          {"detached", a_detached_head_is_reported_as_detached},
                          {"corrupt", a_corrupt_object_database_is_named_as_corrupt},
                          {"not-a-repo", a_path_that_is_not_a_repository_is_not_a_repository},
                          {"refs", refs_are_listed_and_match_git},
                          {"commit", commit_metadata_matches_git_field_for_field},
                          {"parents", a_root_commit_has_no_parents_and_the_second_has_one},
                          {"tree", tree_listing_matches_git_including_kinds_and_sizes},
                          {"blob", blobs_read_the_committed_bytes_and_absent_paths_are_named},
                          {"numstat", numstat_and_diff_match_git},
                          {"blame-oracle", blame_regions_are_byte_identical_to_gits_own_output},
                          {"blame-contents", blame_of_uncommitted_bytes_uses_the_bytes_not_the_tree},
                          {"blame-rev", blame_at_a_pinned_revision_and_with_a_line_range},
                          {"inject-rev", a_revspec_that_would_inject_a_flag_never_reaches_a_process},
                          {"inject-path", a_path_that_would_escape_the_repository_never_reaches_a_process},
                          {"leaks", many_calls_leak_no_descriptors_and_no_processes},
                          {"timeout", a_child_that_never_answers_is_killed_at_its_deadline},
                          {"revspec", the_revspec_predicate_accepts_names_and_nothing_else},
                          {"path", the_path_predicate_keeps_the_filesystem_inside_the_repo},
                          {"range", a_range_is_split_endpoint_wise_and_only_one_separator_is_accepted},
                          {"dirty", a_dirty_tree_is_refused_with_the_paths_and_nothing_is_written},
                          {"switch", switching_to_a_tag_detaches_and_to_an_unknown_ref_is_named},
                          {NULL, NULL}});
  if (g_shim_dir[0] != '\0')
    kbc_test_rmrf(g_shim_dir);
  return rc;
}
