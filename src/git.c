/* git.c — a spawn-and-parse layer over the `git` binary.
 *
 * Nothing here reimplements git. Revspec resolution, tree walking, delta
 * decoding and blame all belong to the binary; this file builds argv
 * arrays, runs one child at a time, and parses the documented output
 * formats back out. See include/kbc/git.h for the security contract —
 * short version: execvp with an argv array, never a shell; every
 * caller-supplied value validated before it can become an argv element; and
 * `--end-of-options` plus `--` inside git so a value that somehow slipped
 * through is still read as data.
 *
 * The child environment is snapshotted at repo-open time (see build_env)
 * because it must be scrubbed once, not once per process spawn, and
 * because a handle is cheap to open per request and must not observe an
 * ambient GIT_DIR that changes underneath it.
 */

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "kbc/git.h"

/* environ is POSIX but not ISO C, so -std=c17 hides the declaration. */
extern char **environ;

/* The longest argv any command here builds: blame with every option is
 * git + 3 + "blame" + 8 + "--" + path. 48 is double that; overflowing it
 * is a bug in this file, not in a caller. */
#define GIT_ARGV_MAX 48

/* git's diagnostics are capped so one chatty failure cannot eat a request
 * budget. 64 KiB is far more than any message in this file has ever
 * produced, and the cap is a backstop, not a normal limit. */
#define GIT_STDERR_CAP (64u * 1024u)

/* One line of `git blame --incremental`. Real lines are a sha, four
 * numbers and a filename; 1 MiB only exists so a git bug cannot make this
 * process allocate without bound. */
#define GIT_MAX_LINE (1u * 1024u * 1024u)

#define GIT_READ_CHUNK 65536u

/* ================================================================ errors */

/* A kbc_err carries a status and a message and nothing else, and the frozen
 * contract says a failure is reported through one. But "git failed" is not
 * an answer a caller can act on, and the kbc_status vocabulary is too
 * coarse to carry the taxonomy this module needs — a bad ref and a
 * missing path and an unborn HEAD are all KBC_ERR_NOTFOUND there.
 *
 * So the ecode travels as a stable prefix on the message and
 * kbc_git_error() reads it back. The prefix is part of the contract, not a
 * rendering detail: it is what makes "each failure mode is distinctly
 * named" true all the way out to the caller. */
static const char *const k_ecode_names[KBC_GIT__COUNT] = {
    "ok",
    "rejected",
    "not-a-repo",
    "unborn",
    "detached-head",
    "bad-ref",
    "not-found",
    "corrupt",
    "dirty",
    "internal",
    "spawn",
    "git-failed",
    "io",
    "truncated",
    "timeout",
};

const char *kbc_git_ecode_str(kbc_git_ecode e)
{
  if ((int)e < 0 || (size_t)e >= KBC_GIT__COUNT)
    return "unknown";
  return k_ecode_names[e];
}

/* Which kbc_status each failure mode maps to. A caller that only has the
 * status still gets "not found" vs "conflict" vs "internal" right; only the
 * distinctions WITHIN not-found need kbc_git_error(). */
static kbc_status ecode_status(kbc_git_ecode e)
{
  switch (e) {
  case KBC_GIT_OK:
    return KBC_OK;
  case KBC_GIT_E_REJECTED:
  case KBC_GIT_E_NOT_A_REPO:
  case KBC_GIT_E_DETACHED_HEAD:
    return KBC_ERR_INVALID;
  case KBC_GIT_E_UNBORN:
  case KBC_GIT_E_BAD_REF:
  case KBC_GIT_E_NOT_FOUND:
    return KBC_ERR_NOTFOUND;
  case KBC_GIT_E_CORRUPT:
  case KBC_GIT_E_INTERNAL:
    return KBC_ERR_INTERNAL;
  case KBC_GIT_E_DIRTY:
    return KBC_ERR_CONFLICT;
  case KBC_GIT_E_TRUNCATED:
    return KBC_ERR_PARSE;
  case KBC_GIT_E_TIMEOUT:
    return KBC_ERR_TIMEOUT;
  case KBC_GIT_E_SPAWN:
  case KBC_GIT_E_FAILED:
  case KBC_GIT_E_IO:
  default:
    return KBC_ERR_IO;
  }
}

kbc_git_ecode kbc_git_error(const kbc_err *e)
{
  size_t i;
  if (e == NULL || e->msg[0] == '\0')
    return KBC_GIT_OK;
  for (i = 1; i < KBC_GIT__COUNT; i++) {
    const char *name = k_ecode_names[i];
    size_t n = strlen(name);
    if (strncmp(e->msg, name, n) == 0 && e->msg[n] == ':' && e->msg[n + 1] == ' ')
      return (kbc_git_ecode)i;
  }
  /* Not one of ours: the caller filled this itself, so the most conservative
   * reading is "git failed", never a specific mode we did not observe. */
  return KBC_GIT_E_FAILED;
}

static kbc_status fail(kbc_err *err, kbc_git_ecode ec, const char *fmt, ...)
{
  char body[KBC_ERR_MSG_MAX - 32];
  va_list ap;
  va_start(ap, fmt);
  if (vsnprintf(body, sizeof body, fmt, ap) < 0)
    body[0] = '\0';
  va_end(ap);
  if (err != NULL) {
    err->status = ecode_status(ec);
    /* Truncation here would eat the ": " the decoder looks for, so the
     * prefix is written first and the body can only ever lose its tail. */
    snprintf(err->msg, sizeof err->msg, "%s: %s", k_ecode_names[ec], body);
  }
  return ecode_status(ec);
}

/* ============================================================ validation */

/* A byte git must never see in a revspec or a path. ASCII control
 * characters and DEL, plus the C1 range: the Rust original rejects
 * char::is_control(), and U+0080..U+009F are Cc in Unicode, so excluding
 * them keeps the two implementations' answers identical for the same
 * input. Anything above that is ordinary text and is allowed. */
static bool byte_is_control(unsigned char c)
{
  return c < 0x20u || c == 0x7fu || (c >= 0x80u && c <= 0x9fu);
}

static bool revspec_ok_n(const char *s, size_t n)
{
  size_t i;
  if (s == NULL || n == 0 || s[0] == '-')
    return false;
  for (i = 0; i < n; i++) {
    if (byte_is_control((unsigned char)s[i]))
      return false;
    /* isspace() under the C locale is exactly space, \t \n \v \f \r, which
     * is also what the Rust predicate rejects. A whitespace byte inside a
     * revspec is never meaningful and is how an argument gets split. */
    if (isspace((unsigned char)s[i]))
      return false;
  }
  /* ".." is a RANGE, not an endpoint — kbc_git_range_parse is the only way
   * to spell one, so an endpoint that contains it is a mistake or an
   * attempt to smuggle the range separator past the validator. */
  if (n >= 2) {
    for (i = 0; i + 1 < n; i++)
      if (s[i] == '.' && s[i + 1] == '.')
        return false;
  }
  /* "@{" is reflog and upstream syntax. It resolves against local state a
   * remote caller must not be able to address. */
  for (i = 0; i + 1 < n; i++)
    if (s[i] == '@' && s[i + 1] == '{')
      return false;
  return true;
}

bool kbc_git_revspec_ok(const char *s)
{
  return revspec_ok_n(s, s != NULL ? strlen(s) : 0u);
}

kbc_status kbc_git_revspec_check(const char *s, kbc_err *err)
{
  size_t n = s != NULL ? strlen(s) : 0u;
  if (revspec_ok_n(s, n))
    return KBC_OK;
  /* The offending value goes in the message verbatim up to a point: an
   * operator reading the log needs to see what was actually rejected.
   * Control bytes are printed as-is because they are the whole reason. */
  return fail(err, KBC_GIT_E_REJECTED, "revspec \"%.*s\" rejected",
              (int)(n > 120u ? 120u : n), s != NULL ? s : "");
}

static bool path_ok_n(const char *rel, size_t n)
{
  size_t i, start = 0;
  if (rel == NULL || n == 0 || n > KBC_MAX_PATH_LEN)
    return false;
  /* Absolute escapes the repository, and a leading '-' is the flag
   * vector even after "--", because git still expands a pathspec's own
   * leading characters. */
  if (rel[0] == '/' || rel[0] == '-')
    return false;
  /* Glob metacharacters are NOT rejected: the child runs with
   * GIT_LITERAL_PATHSPECS=1, so git reads every pathspec as the exact
   * filename it names and '*' means a file whose name contains a star.
   * A backslash is likewise a legal byte in a Linux filename and is left
   * alone — narrowing the domain here would make real files unreadable. */
  for (i = 0; i < n; i++) {
    if (byte_is_control((unsigned char)rel[i]))
      return false;
  }
  /* Component-wise: ".." anywhere is an escape, and an empty component
   * ("a//b", or a trailing slash) is not a path git can name. */
  for (i = 0; i <= n; i++) {
    if (i == n || rel[i] == '/') {
      size_t clen = i - start;
      if (clen == 0)
        return false;
      if (clen == 2 && rel[start] == '.' && rel[start + 1] == '.')
        return false;
      start = i + 1;
    }
  }
  return true;
}

bool kbc_git_path_ok(const char *rel)
{
  return path_ok_n(rel, rel != NULL ? strlen(rel) : 0u);
}

kbc_status kbc_git_path_check(const char *rel, kbc_err *err)
{
  size_t n = rel != NULL ? strlen(rel) : 0u;
  if (path_ok_n(rel, n))
    return KBC_OK;
  return fail(err, KBC_GIT_E_REJECTED, "path \"%.*s\" rejected",
              (int)(n > 120u ? 120u : n), rel != NULL ? rel : "");
}

kbc_status kbc_git_range_parse(const char *s, kbc_git_range *out,
                               kbc_err *err)
{
  size_t n = s != NULL ? strlen(s) : 0u;
  size_t i, at, dots;
  if (out == NULL)
    return fail(err, KBC_GIT_E_REJECTED, "range: no output");
  if (n == 0)
    return fail(err, KBC_GIT_E_REJECTED, "range: empty");
  /* First '.' run decides: it must be exactly two or three dots. A single
   * dot is not a range at all (v1.2 is a legal tag name), and four is
   * never anything. */
  for (at = 0; at < n && s[at] != '.'; at++) {
  }
  if (at >= n)
    return fail(err, KBC_GIT_E_REJECTED, "range \"%s\": no \"..\" separator",
                s);
  for (dots = 0; at + dots < n && s[at + dots] == '.'; dots++) {
  }
  if (dots != 2 && dots != 3)
    return fail(err, KBC_GIT_E_REJECTED,
                "range \"%s\": separator is %zu dots, want 2 or 3", s, dots);
  if (at == 0 || at + dots >= n)
    return fail(err, KBC_GIT_E_REJECTED, "range \"%s\": empty endpoint", s);
  /* Endpoints are validated exactly as a standalone revspec is, so the
   * only way ".." reaches git is as the separator this function inserted. */
  if (!revspec_ok_n(s, at))
    return fail(err, KBC_GIT_E_REJECTED, "range \"%s\": left endpoint", s);
  if (!revspec_ok_n(s + at + dots, n - at - dots))
    return fail(err, KBC_GIT_E_REJECTED, "range \"%s\": right endpoint", s);
  /* A second separator would make the range ambiguous; reject rather than
   * guess which endpoint the caller meant. */
  for (i = at + dots; i < n; i++) {
    if (s[i] == '.' && i + 1 < n && s[i + 1] == '.')
      return fail(err, KBC_GIT_E_REJECTED, "range \"%s\": two separators",
                  s);
  }
  out->from = s;
  out->from_len = at;
  out->to = s + at + dots;
  out->to_len = n - at - dots;
  out->three_dot = (dots == 3);
  return KBC_OK;
}

kbc_status kbc_git_revpath_parse(const char *s, const char **rev,
                                 size_t *rev_len, const char **path,
                                 size_t *path_len, kbc_err *err)
{
  const char *colon;
  if (rev == NULL || rev_len == NULL || path == NULL || path_len == NULL)
    return fail(err, KBC_GIT_E_REJECTED, "revpath: no output");
  colon = s != NULL ? strchr(s, ':') : NULL;
  if (colon == NULL || colon == s || colon[1] == '\0')
    return fail(err, KBC_GIT_E_REJECTED,
                "\"%s\" is not <rev>:<path>", s != NULL ? s : "");
  /* git splits at the FIRST colon, so that is where we split: a path with
   * a colon in it cannot be addressed this way, which is git's rule and
   * not this file's. */
  if (!revspec_ok_n(s, (size_t)(colon - s)))
    return fail(err, KBC_GIT_E_REJECTED, "\"%s\": bad revspec half", s);
  if (!path_ok_n(colon + 1, strlen(colon + 1)))
    return fail(err, KBC_GIT_E_REJECTED, "\"%s\": bad path half", s);
  *rev = s;
  *rev_len = (size_t)(colon - s);
  *path = colon + 1;
  *path_len = strlen(colon + 1);
  return KBC_OK;
}

/* ================================================================= argv */

/* execvp's signature predates const: it takes char *const argv[] and
 * writes nothing through it. This is the single place that cast is made,
 * immediately before the exec, so no other code in the file has to lie
 * about a string's constness. */
static char *exec_arg(const char *s)
{
  union {
    const char *ro;
    char *rw;
  } u;
  u.ro = s;
  return u.rw;
}

typedef struct {
  char *v[GIT_ARGV_MAX];
  size_t n;
} git_argv;

static void argv_init(git_argv *a, const char *root)
{
  /* -C before the subcommand: that is what makes every path in this file
   * relative to one repository regardless of the daemon's cwd, which is
   * not something the daemon controls. --no-pager because a pager would
   * turn a pipe into a terminal and hang the request. */
  a->v[0] = exec_arg("git");
  a->v[1] = exec_arg("--no-pager");
  a->v[2] = exec_arg("-C");
  a->v[3] = exec_arg(root);
  a->n = 4;
  a->v[4] = NULL;
}

static kbc_status argv_add(git_argv *a, const char *s)
{
  if (a->n + 1 >= GIT_ARGV_MAX)
    return fail(NULL, KBC_GIT_E_INTERNAL, "argv overflow at %zu", a->n);
  a->v[a->n++] = exec_arg(s);
  /* execvp reads until it finds NULL; without this terminator it walks off
   * the end of the array into whatever is next in memory. */
  a->v[a->n] = NULL;
  return KBC_OK;
}

static kbc_status argv_add_rev(git_argv *a, const char *s, kbc_err *err)
{
  /* --end-of-options tells git that nothing after this point is an option,
   * which is git's own half of the defence. It is redundant with the
   * validator and that is the point: the validator is the rule, this is
   * what makes a future mistake in the rule non-exploitable. */
  kbc_status st = kbc_git_revspec_check(s, err);
  if (st != KBC_OK)
    return st;
  st = argv_add(a, "--end-of-options");
  if (st != KBC_OK)
    return st;
  return argv_add(a, s);
}

static kbc_status argv_add_path(git_argv *a, const char *s, kbc_err *err)
{
  kbc_status st = kbc_git_path_check(s, err);
  if (st != KBC_OK)
    return st;
  /* -- is the pathspec terminator. After it git cannot re-read the value
   * as a revspec, which is the other half of the injection: a path is the
   * one value that reaches an option-looking position in several of these
   * commands. */
  st = argv_add(a, "--");
  if (st != KBC_OK)
    return st;
  return argv_add(a, s);
}

/* ======================================================== child process */

typedef struct kbc_git_repo {
  char *root;    /* KBC_OWN: canonical toplevel, or the gitdir if bare */
  char *git_dir; /* KBC_OWN */
  char **env;    /* KBC_OWN: NULL-terminated sanitized environ */
  size_t env_len;
  bool shallow;
  bool bare;
} kbc_git_repo;

/* Forced into every child. GIT_LITERAL_PATHSPECS is the load-bearing one:
 * without it git treats '*' and '[' in a pathspec as globs, so a path from
 * a request could select files the caller never named. The rest keep git
 * from blocking on a terminal, from rewriting .git/index on a read, and
 * from emitting messages in a language this file's classifier cannot read.
 * LC_ALL=C is why the classifier below matches English words. */
static const char *const k_forced_env[] = {
    "GIT_CONFIG_NOSYSTEM=1",
    "GIT_TERMINAL_PROMPT=0",
    "GIT_OPTIONAL_LOCKS=0",
    "GIT_LITERAL_PATHSPECS=1",
    "GIT_PAGER=cat",
    "LC_ALL=C",
    "LANG=C",
};

/* Every GIT_* variable is dropped from the inherited environment.
 *
 * The daemon's own environment is not attacker-controlled, but a GIT_DIR
 * or GIT_OBJECT_DIRECTORY inherited from however the daemon was started
 * would silently answer a request from a different repository than the one
 * the handle names — a request must not be able to redirect where this
 * module reads. The one GIT_* variable a caller may influence,
 * GIT_ALTERNATE_OBJECT_DIRECTORIES, is added back explicitly and only for
 * the single invocation that asked for it, never inherited. */
static char **build_env(size_t *out_len)
{
  size_t pass = 0, forced = sizeof k_forced_env / sizeof k_forced_env[0];
  size_t n = 0, i;
  char **env;
  for (i = 0; environ != NULL && environ[i] != NULL; i++) {
    if (strncmp(environ[i], "GIT_", 4) != 0)
      pass++;
  }
  if (pass > SIZE_MAX / sizeof(char *) - forced - 1)
    return NULL;
  env = calloc(pass + forced + 1, sizeof *env);
  if (env == NULL)
    return NULL;
  for (i = 0; environ != NULL && environ[i] != NULL; i++) {
    if (strncmp(environ[i], "GIT_", 4) != 0)
      env[n++] = environ[i];
  }
  for (i = 0; i < forced; i++)
    env[n++] = exec_arg(k_forced_env[i]);
  env[n] = NULL;
  *out_len = n;
  return env;
}

static int64_t now_ms(void)
{
  struct timespec ts;
  if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0)
    return 0;
  return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* pipe(2) plus FD_CLOEXEC. pipe2() would be one call, but it is declared
 * only under _GNU_SOURCE and this build is -std=c17 with _POSIX_C_SOURCE,
 * so the two-syscall form is the portable one here. The close-on-exec is
 * not hygiene: it is what keeps a descriptor for a child's stdout from
 * surviving into the next child, which is how a process ends up waiting on
 * a pipe nobody will ever write to. */
static bool make_pipe(int fds[2])
{
  int fl;
  if (pipe(fds) != 0)
    return false;
  fl = fcntl(fds[0], F_GETFD);
  if (fl < 0 || fcntl(fds[0], F_SETFD, fl | FD_CLOEXEC) != 0) {
    close(fds[0]);
    close(fds[1]);
    return false;
  }
  fl = fcntl(fds[1], F_GETFD);
  if (fl < 0 || fcntl(fds[1], F_SETFD, fl | FD_CLOEXEC) != 0) {
    close(fds[0]);
    close(fds[1]);
    return false;
  }
  return true;
}

/* Writing to a child that died between fork and here raises SIGPIPE, whose
 * default action would kill the daemon. Blocking it on THIS thread for the
 * duration of the write, then restoring, is the whole fix: it is thread-
 * local, so it changes no process-wide disposition and cannot be raced by
 * another request. */
static ssize_t write_nosigpipe(int fd, const char *data, size_t n)
{
  sigset_t block, prev;
  ssize_t w;
  int rc, saved;
  sigemptyset(&block);
  sigaddset(&block, SIGPIPE);
  rc = pthread_sigmask(SIG_BLOCK, &block, &prev);
  if (rc != 0) {
    errno = rc;
    return -1;
  }
  w = write(fd, data, n);
  saved = errno;
  rc = pthread_sigmask(SIG_SETMASK, &prev, NULL);
  if (w < 0) {
    errno = saved;
    return -1;
  }
  if (rc != 0) {
    errno = rc;
    return -1;
  }
  return w;
}

static void close_fd(int *fd)
{
  if (*fd >= 0) {
    close(*fd);
    *fd = -1;
  }
}

static bool buf_contains(const char *hay, size_t n, const char *needle)
{
  size_t m = strlen(needle);
  size_t i;
  if (m == 0 || m > n)
    return false;
  for (i = 0; i + m <= n; i++)
    if (memcmp(hay + i, needle, m) == 0)
      return true;
  return false;
}

/* Turn git's exit status and stderr into one of the named failures.
 *
 * This is a heuristic over English diagnostic text and it is honest about
 * that: an unrecognised message is KBC_GIT_E_FAILED, never a guess at
 * something more specific. The order matters — a broken object database
 * also prints "bad object", and calling that a bad ref would send an
 * operator looking for a typo instead of at the disk.
 *
 * The strings are pinned by tests/test_git.c against a real repository
 * that has actually been broken in each of these ways. */
static kbc_git_ecode classify(int code, const char *err, size_t err_len)
{
  static const char *const corrupt[] = {
      "is corrupt",       "inflate:",         "unable to unpack",
      "unable to parse",  "index file smaller",
  };
  static const char *const not_a_repo[] = {
      "not a git repository",
      "cannot change to",
      "this operation must be run in a work tree",
  };
  static const char *const unborn[] = {
      "Needed a single revision",
      "ambiguous argument 'HEAD'",
  };
  static const char *const bad_ref[] = {
      "unknown revision",     "ambiguous argument", "bad revision",
      "no such ref",          "invalid reference",   "not a valid object name",
      "reference is not a tree", "could not resolve", "bad object",
      "unknown revision or path",
  };
  static const char *const not_found[] = {
      "no such path", "does not exist in", "exists on disk, but not in",
  };
  size_t i;
  for (i = 0; i < sizeof corrupt / sizeof corrupt[0]; i++)
    if (buf_contains(err, err_len, corrupt[i]))
      return KBC_GIT_E_CORRUPT;
  for (i = 0; i < sizeof not_a_repo / sizeof not_a_repo[0]; i++)
    if (buf_contains(err, err_len, not_a_repo[i]))
      return KBC_GIT_E_NOT_A_REPO;
  for (i = 0; i < sizeof unborn / sizeof unborn[0]; i++)
    if (buf_contains(err, err_len, unborn[i]))
      return KBC_GIT_E_UNBORN;
  for (i = 0; i < sizeof not_found / sizeof not_found[0]; i++)
    if (buf_contains(err, err_len, not_found[i]))
      return KBC_GIT_E_NOT_FOUND;
  for (i = 0; i < sizeof bad_ref / sizeof bad_ref[0]; i++)
    if (buf_contains(err, err_len, bad_ref[i]))
      return KBC_GIT_E_BAD_REF;
  /* No diagnostic at all is its own case: a usage error or a bare exit.
   * code==0 never reaches here. */
  (void)code;
  return KBC_GIT_E_FAILED;
}

/* ================================================================ sink */

typedef struct blame_parser blame_parser;

static kbc_status blame_parser_feed(blame_parser *bp, const char *data,
                                    size_t n, kbc_err *err);

/* Where a child's stdout and stderr go. Exactly one of `out` and `bp` is
 * live: capture for everything except blame, which is parsed as the bytes
 * arrive because a file's blame can be larger than the request's memory. */
typedef struct {
  kbc_str *out;
  size_t out_cap;
  size_t out_seen;
  kbc_str *err;
  blame_parser *bp;
  bool overflow;
} git_sink;

static kbc_status sink_stdout(git_sink *s, const char *data, size_t n,
                              kbc_err *err)
{
  if (s->overflow)
    return KBC_OK; /* keep draining so the child is never blocked on us */
  if (s->bp != NULL)
    return blame_parser_feed(s->bp, data, n, err);
  if (s->out != NULL) {
    if (s->out->len + n > s->out_cap) {
      s->overflow = true;
      return KBC_OK;
    }
    return kbc_str_append(s->out, data, n);
  }
  return KBC_OK;
}

static kbc_status sink_stderr(git_sink *s, const char *data, size_t n,
                              kbc_err *err)
{
  (void)err; /* stderr is diagnostic: it is read for classification only */
  if (s->err == NULL || s->err->len >= GIT_STDERR_CAP)
    return KBC_OK;
  if (s->err->len + n > GIT_STDERR_CAP)
    n = GIT_STDERR_CAP - s->err->len;
  return kbc_str_append(s->err, data, n);
}

/* ============================================================ run_git */

typedef struct {
  git_sink *sink;
  int code;      /* child's exit status, or -1 if it never ran */
  int signal;    /* signal that killed it, or 0 */
  kbc_git_ecode ec; /* KBC_GIT_OK when the child succeeded */
} run_result;

/* Runs one git command to completion and reaps it.
 *
 * Everything the loop needs to be correct lives in this one function, and
 * there is no other way to start a process from this file: the two pipes,
 * the concurrent stdin pump, the deadline and the reap. The invariants it
 * maintains on every path out — both pipes closed, the child waited for,
 * no SIGPIPE escaping — are what make "no leaked descriptors or zombies
 * across many calls" true rather than a hope. */
static kbc_status run_git(const kbc_git_repo *repo, const git_argv *av,
                           const char *alt_objects, const void *in,
                           size_t in_len, git_sink *sink, uint32_t timeout_ms,
                           run_result *res, kbc_err *err)
{
  int outfd[2] = {-1, -1}, errfd[2] = {-1, -1}, infd[2] = {-1, -1};
  char **env = repo->env;
  char **alt_var = NULL;
  char *alt_env = NULL;
  size_t in_off = 0;
  int64_t deadline = timeout_ms > 0 ? now_ms() + (int64_t)timeout_ms : 0;
  bool out_open, err_open, in_open, killed = false, timed_out = false;
  struct pollfd pfd[3];
  char buf[GIT_READ_CHUNK];
  pid_t pid;
  int wstatus = 0;
  kbc_status st = KBC_OK;

  res->code = -1;
  res->signal = 0;
  res->ec = KBC_GIT_OK;

  if (alt_objects != NULL) {
    static const char pfx[] = "GIT_ALTERNATE_OBJECT_DIRECTORIES=";
    size_t need = sizeof pfx + strlen(alt_objects);
    alt_var = calloc(repo->env_len + 2, sizeof *alt_var);
    alt_env = malloc(need);
    if (alt_var == NULL || alt_env == NULL) {
      free(alt_var);
      free(alt_env);
      return fail(err, KBC_GIT_E_IO, "git: no memory for alternates env");
    }
    memcpy(alt_env, pfx, sizeof pfx - 1);
    memcpy(alt_env + sizeof pfx - 1, alt_objects, strlen(alt_objects) + 1);
    memcpy(alt_var, repo->env, repo->env_len * sizeof *alt_var);
    alt_var[repo->env_len] = alt_env;
    alt_var[repo->env_len + 1] = NULL;
    env = alt_var;
  }

  if (!make_pipe(outfd)) {
    st = fail(err, KBC_GIT_E_IO, "git: pipe for stdout: %s", strerror(errno));
    goto done;
  }
  if (!make_pipe(errfd)) {
    st = fail(err, KBC_GIT_E_IO, "git: pipe for stderr: %s", strerror(errno));
    goto done;
  }
  if (in_len > 0 && !make_pipe(infd)) {
    st = fail(err, KBC_GIT_E_IO, "git: pipe for stdin: %s", strerror(errno));
    goto done;
  }

  pid = fork();
  if (pid < 0) {
    st = fail(err, KBC_GIT_E_SPAWN, "git: fork: %s", strerror(errno));
    goto done;
  }
  if (pid == 0) {
    /* Child. Only async-signal-safe calls from here to execvp. */
    int devnull;
    if (in_len > 0) {
      if (dup2(infd[0], STDIN_FILENO) < 0)
        _exit(126);
      close(infd[0]);
      close(infd[1]);
    } else {
      /* git must never inherit the daemon's stdin: a subcommand that
       * decides to prompt would consume a request body. */
      devnull = open("/dev/null", O_RDONLY);
      if (devnull < 0 || dup2(devnull, STDIN_FILENO) < 0)
        _exit(126);
      if (devnull > STDERR_FILENO)
        close(devnull);
    }
    if (dup2(outfd[1], STDOUT_FILENO) < 0 || dup2(errfd[1], STDERR_FILENO) < 0)
      _exit(126);
    close(outfd[0]);
    close(outfd[1]);
    close(errfd[0]);
    close(errfd[1]);
    /* The sanitized environment goes in by assigning `environ` rather than
     * by execvpe, which glibc does not provide: a plain store to a global
     * is async-signal-safe and execvp reads it on the next line. */
    environ = env;
    execvp("git", av->v);
    _exit(127); /* exec failed; 127 is the shell convention for that */
  }

  /* Parent. The write ends exist in the child now; keeping them open here
   * would mean a reader that never sees EOF. */
  close_fd(&outfd[1]);
  close_fd(&errfd[1]);
  close_fd(&infd[0]);
  out_open = true;
  err_open = true;
  in_open = in_len > 0;

  pfd[0].fd = outfd[0];
  pfd[0].events = POLLIN;
  pfd[1].fd = errfd[0];
  pfd[1].events = POLLIN;
  pfd[2].fd = infd[1];
  pfd[2].events = POLLOUT;
  for (;;) {
    int nf = 0, timeout = -1, n, idx_in = -1, idx_out = -1, idx_err = -1;
    ssize_t got;
    if (out_open)
      idx_out = nf++;
    if (err_open)
      idx_err = nf++;
    if (in_open)
      idx_in = nf++;
    if (nf == 0)
      break;
    if (idx_out >= 0)
      pfd[idx_out].revents = 0;
    if (idx_err >= 0)
      pfd[idx_err].revents = 0;
    if (idx_in >= 0)
      pfd[idx_in].revents = 0;
    if (deadline > 0) {
      int64_t left = deadline - now_ms();
      if (left <= 0) {
        timed_out = true;
        break;
      }
      timeout = left > 1000 ? 1000 : (int)left;
    }
    n = poll(pfd, (nfds_t)nf, timeout);
    if (n < 0) {
      if (errno == EINTR)
        continue;
      st = fail(err, KBC_GIT_E_IO, "git: poll: %s", strerror(errno));
      break;
    }
    if (n == 0) {
      /* Only the 1000 ms tick can land here; the real deadline is the
       * `left <= 0` branch above. Re-loop to re-check it. */
      continue;
    }
    /* stdin first: the child may be blocked writing its payload back at
     * us, and every byte we accept here is a byte of deadlock it does not
     * get to. One loop drives both directions, which is why this file does
     * not need the writer thread the Rust original spawns. */
    if (idx_in >= 0 && pfd[idx_in].revents != 0) {
      got = write_nosigpipe(infd[1], (const char *)in + in_off, in_len - in_off);
      if (got > 0) {
        in_off += (size_t)got;
        if (in_off >= in_len) {
          close_fd(&infd[1]);
          in_open = false;
        }
      } else if (got < 0 && (errno == EAGAIN || errno == EINTR)) {
        /* retry */
      } else {
        /* EPIPE here means git is already gone or never wanted the
         * payload. Stop writing and let the read side discover why. */
        close_fd(&infd[1]);
        in_open = false;
      }
    }
    if (idx_out >= 0 && pfd[idx_out].revents != 0) {
      got = read(outfd[0], buf, sizeof buf);
      if (got > 0) {
        st = sink_stdout(sink, buf, (size_t)got, err);
        if (st != KBC_OK)
          break;
      } else if (got == 0) {
        out_open = false;
      } else if (errno != EAGAIN && errno != EINTR) {
        st = fail(err, KBC_GIT_E_IO, "git: read stdout: %s", strerror(errno));
        break;
      }
    }
    if (idx_err >= 0 && pfd[idx_err].revents != 0) {
      got = read(errfd[0], buf, sizeof buf);
      if (got > 0) {
        st = sink_stderr(sink, buf, (size_t)got, err);
        if (st != KBC_OK)
          break;
      } else if (got == 0) {
        err_open = false;
      } else if (errno != EAGAIN && errno != EINTR) {
        st = fail(err, KBC_GIT_E_IO, "git: read stderr: %s", strerror(errno));
        break;
      }
    }
  }

  if (in_open) {
    /* Loop ended with the payload undelivered — either the deadline or a
     * sink error. Git will see EOF the moment the pipe closes. */
    close_fd(&infd[1]);
    in_open = false;
  }

  if (st != KBC_OK || timed_out || sink->overflow) {
    /* We already have our answer; the child cannot be allowed to sit in a
     * pipe we will never drain again. SIGKILL because a child blocked in
     * write() cannot be asked politely. */
    kill(pid, SIGKILL);
    killed = true;
  }
  close_fd(&outfd[0]);
  close_fd(&errfd[0]);

  for (;;) {
    pid_t w = waitpid(pid, &wstatus, 0);
    if (w == pid)
      break;
    if (w < 0 && errno == EINTR)
      continue;
    if (w < 0)
      st = fail(err, KBC_GIT_E_IO, "git: waitpid: %s", strerror(errno));
    break;
  }

  {
    const char *sub = av->n > 4 ? av->v[4] : "(no subcommand)";
    const char *emsg = sink->err != NULL ? sink->err->ptr : "";
    size_t elen = sink->err != NULL ? sink->err->len : 0u;
    if (timed_out) {
      st = fail(err, KBC_GIT_E_TIMEOUT, "git %s: killed after %u ms", sub,
                timeout_ms);
    } else if (st != KBC_OK) {
      /* already filled in by whoever failed */
    } else if (sink->overflow) {
      st = fail(err, KBC_GIT_E_IO, "git %s: output exceeded %zu bytes", sub,
                sink->out_cap);
    } else if (WIFSIGNALED(wstatus)) {
      res->signal = WTERMSIG(wstatus);
      st = fail(err, KBC_GIT_E_IO, "git %s: killed by signal %d%s", sub,
                res->signal, killed ? " (we killed it)" : "");
    } else {
      res->code = WEXITSTATUS(wstatus);
      if (res->code == 0) {
        res->ec = KBC_GIT_OK;
        st = KBC_OK;
      } else if (res->code == 127) {
        st = fail(err, KBC_GIT_E_SPAWN, "git: cannot exec git (not on PATH?)");
      } else {
        res->ec = classify(res->code, emsg, elen);
        st = fail(err, res->ec, "git %s: exit %d: %.*s", sub, res->code,
                  (int)(elen > 200u ? 200u : elen), emsg);
      }
    }
  }

done:
  close_fd(&outfd[0]);
  close_fd(&outfd[1]);
  close_fd(&errfd[0]);
  close_fd(&errfd[1]);
  close_fd(&infd[0]);
  close_fd(&infd[1]);
  free(alt_var);
  free(alt_env);
  return st;
}

/* Capture a whole run. `out` may be NULL for a probe whose only answer is
 * its exit status (--quiet forms). Everything else in this file goes
 * through here or through run_git directly; there is no third way to
 * start a process. */
static kbc_status capture(const kbc_git_repo *repo, const git_argv *av,
                          const void *in, size_t in_len, size_t cap,
                          kbc_str *out, run_result *res, kbc_err *err)
{
  kbc_str diag;
  git_sink sink;
  kbc_status st;
  kbc_str_init(&diag);
  memset(&sink, 0, sizeof sink);
  sink.out = out;
  sink.out_cap = cap;
  sink.err = &diag;
  st = run_git(repo, av, NULL, in, in_len, &sink, 0, res, err);
  kbc_str_free(&diag);
  return st;
}

/* ============================================================== parsing */

/* Trailing newline(s) then leading blanks. The input length is taken by
 * value and the result written through a pointer: a caller that forgets to
 * seed the length would otherwise silently trim a garbage number of bytes
 * off the end of a heap buffer. */
static const char *str_trim(const char *s, size_t len, size_t *out_len)
{
  while (len > 0 && (s[len - 1] == '\n' || s[len - 1] == '\r'))
    len--;
  while (len > 0 && (*s == ' ' || *s == '\t')) {
    s++;
    len--;
  }
  *out_len = len;
  return s;
}

static bool parse_u32(const char *s, size_t n, uint32_t *out)
{
  uint64_t v = 0;
  size_t i;
  if (n == 0 || n > 10)
    return false;
  for (i = 0; i < n; i++) {
    if (s[i] < '0' || s[i] > '9')
      return false;
    v = v * 10u + (uint64_t)(s[i] - '0');
  }
  if (v > 0xffffffffu)
    return false;
  *out = (uint32_t)v;
  return true;
}

static bool parse_i64(const char *s, size_t n, int64_t *out)
{
  int64_t v = 0;
  size_t i = 0;
  bool neg = false;
  if (n == 0)
    return false;
  if (s[0] == '-') {
    neg = true;
    i = 1;
    if (n == 1)
      return false;
  }
  for (; i < n; i++) {
    if (s[i] < '0' || s[i] > '9')
      return false;
    if (v > (INT64_MAX - (s[i] - '0')) / 10)
      return false;
    v = v * 10 + (s[i] - '0');
  }
  *out = neg ? -v : v;
  return true;
}

static bool is_hex(const char *s, size_t n)
{
  size_t i;
  for (i = 0; i < n; i++) {
    char c = s[i];
    if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') ||
          (c >= 'A' && c <= 'F')))
      return false;
  }
  return n > 0;
}

/* Split on a single NUL. git's --format and -z output both use this, and
 * both guarantee the field count, so a short record is a protocol
 * violation rather than an empty field. */
static size_t split_nul(char *s, size_t n, char **fields, size_t want)
{
  size_t count = 0, start = 0, i;
  for (i = 0; i <= n; i++) {
    if (i == n || s[i] == '\0') {
      if (count >= want)
        return 0;
      fields[count++] = s + start;
      start = i + 1;
      if (count == want && i < n)
        return 0; /* there was more than we asked for */
      if (count == want)
        return count;
    }
  }
  return count == want ? count : 0;
}

static kbc_status grow_arena_array(void **items, size_t *cap, size_t want,
                                   size_t elem, kbc_arena *a)
{
  size_t ncap = *cap != 0 ? *cap : 8;
  void *fresh;
  if (want <= *cap)
    return KBC_OK;
  while (ncap < want) {
    if (ncap > SIZE_MAX / 2)
      return KBC_ERR_NOMEM;
    ncap *= 2;
  }
  /* The arena cannot realloc, so growing copies and leaves the old block to
   * die with the arena. Same pattern as parse.c and app.c: doubling means
   * the copies total under 2x the final size, which is the price of an
   * arena and is paid once per request. */
  fresh = kbc_arena_calloc(a, ncap, elem);
  if (fresh == NULL)
    return KBC_ERR_NOMEM;
  if (*items != NULL && *cap > 0)
    memcpy(fresh, *items, *cap * elem);
  *items = fresh;
  *cap = ncap;
  return KBC_OK;
}

/* ================================================================= repo */

kbc_status kbc_git_repo_open(const char *root, kbc_git_repo **out,
                             kbc_err *err)
{
  kbc_git_repo *r;
  git_argv av;
  kbc_str buf;
  run_result res;
  kbc_status st;
  char *lines[3];
  size_t line_lens[3];
  char *toplevel = NULL;

  if (out == NULL)
    return fail(err, KBC_GIT_E_REJECTED, "repo open: no output");
  *out = NULL;
  if (root == NULL || root[0] == '\0')
    return fail(err, KBC_GIT_E_REJECTED, "repo open: empty path");
  if (!kbc_path_exists(root))
    return fail(err, KBC_GIT_E_NOT_A_REPO, "no such directory: %s", root);

  r = calloc(1, sizeof *r);
  if (r == NULL)
    return fail(err, KBC_GIT_E_IO, "repo open: out of memory");
  r->env = build_env(&r->env_len);
  if (r->env == NULL) {
    free(r);
    return fail(err, KBC_GIT_E_IO, "repo open: cannot snapshot environment");
  }

  argv_init(&av, root);
  if (argv_add(&av, "rev-parse") != KBC_OK ||
      argv_add(&av, "--absolute-git-dir") != KBC_OK ||
      argv_add(&av, "--is-bare-repository") != KBC_OK ||
      argv_add(&av, "--is-shallow-repository") != KBC_OK) {
    kbc_git_repo_close(r);
    return fail(err, KBC_GIT_E_INTERNAL, "repo open: argv build");
  }
  kbc_str_init(&buf);
  st = capture(r, &av, NULL, 0, 64u * 1024u, &buf, &res, err);
  if (st != KBC_OK) {
    kbc_str_free(&buf);
    kbc_git_repo_close(r);
    return st;
  }
  /* Exactly three lines, in the order the options were given. */
  {
    size_t n = buf.len, start = 0, i;
    int got = 0;
    for (i = 0; i <= n && got < 3; i++) {
      if (i == n || buf.ptr[i] == '\n') {
        lines[got] = buf.ptr + start;
        line_lens[got] = i - start;
        got++;
        start = i + 1;
      }
    }
    if (got != 3) {
      kbc_str_free(&buf);
      kbc_git_repo_close(r);
      return fail(err, KBC_GIT_E_INTERNAL, "repo open: %s: bad rev-parse",
                  root);
    }
  }
  if (line_lens[0] == 0) {
    kbc_str_free(&buf);
    kbc_git_repo_close(r);
    return fail(err, KBC_GIT_E_NOT_A_REPO, "no git directory at %s", root);
  }
  r->git_dir = strndup(lines[0], line_lens[0]);
  r->bare = line_lens[1] == 4 && memcmp(lines[1], "true", 4) == 0;
  r->shallow = line_lens[2] == 5 && memcmp(lines[2], "true", 5) == 0;
  if (r->git_dir == NULL) {
    kbc_str_free(&buf);
    kbc_git_repo_close(r);
    return fail(err, KBC_GIT_E_IO, "repo open: out of memory");
  }

  /* --show-toplevel fails in a bare repo ("this operation must be run in a
   * work tree"), which is the only reason the bare case is asked about
   * separately: for a bare repository the gitdir IS the root. */
  if (r->bare) {
    toplevel = strdup(r->git_dir);
  } else {
    argv_init(&av, root);
    if (argv_add(&av, "rev-parse") == KBC_OK &&
        argv_add(&av, "--show-toplevel") == KBC_OK) {
      kbc_str_clear(&buf);
      st = capture(r, &av, NULL, 0, 64u * 1024u, &buf, &res, err);
      if (st != KBC_OK) {
        kbc_str_free(&buf);
        kbc_git_repo_close(r);
        return st;
      }
      {
        size_t n;
        const char *t = str_trim(buf.ptr, buf.len, &n);
        if (n == 0) {
          kbc_str_free(&buf);
          kbc_git_repo_close(r);
          return fail(err, KBC_GIT_E_NOT_A_REPO, "no work tree at %s", root);
        }
        toplevel = strndup(t, n);
      }
    }
  }
  kbc_str_free(&buf);
  if (toplevel == NULL) {
    kbc_git_repo_close(r);
    return fail(err, KBC_GIT_E_IO, "repo open: out of memory");
  }
  r->root = toplevel;
  *out = r;
  return KBC_OK;
}

void kbc_git_repo_close(kbc_git_repo *repo)
{
  if (repo == NULL)
    return;
  free(repo->root);
  free(repo->git_dir);
  /* Only the vector is ours. Its entries point into the parent's environ
   * and at static strings in k_forced_env, both of which outlive any
   * handle and must not be freed. */
  free(repo->env);
  free(repo);
}

const char *kbc_git_repo_root(const kbc_git_repo *repo)
{
  return repo != NULL ? repo->root : NULL;
}

const char *kbc_git_repo_git_dir(const kbc_git_repo *repo)
{
  return repo != NULL ? repo->git_dir : NULL;
}

bool kbc_git_repo_is_shallow(const kbc_git_repo *repo)
{
  return repo != NULL && repo->shallow;
}

bool kbc_git_repo_is_bare(const kbc_git_repo *repo)
{
  return repo != NULL && repo->bare;
}

/* ============================================================= resolve */

static kbc_status resolve_sha(const kbc_git_repo *repo, const char *rev,
                               kbc_arena *a, const char **sha, kbc_err *err)
{
  git_argv av;
  kbc_str buf;
  run_result res;
  kbc_status st;
  size_t n;
  const char *t;
  char *deferred_free = NULL;

  argv_init(&av, repo->root);
  st = kbc_git_revspec_check(rev, err);
  if (st != KBC_OK)
    return st;
  if (argv_add(&av, "rev-parse") != KBC_OK ||
      argv_add(&av, "--verify") != KBC_OK ||
      argv_add(&av, "--end-of-options") != KBC_OK)
    return fail(err, KBC_GIT_E_INTERNAL, "resolve: argv build");
  /* "<rev>^{commit}" is ONE argv element, never two: git treats the peel
   * suffix as part of the object name, and passing "^{commit}" separately
   * makes git read it as a second revision. The suffix peels an annotated
   * tag down to its commit, which is what every caller here wants. */
  {
    size_t need = strlen(rev) + sizeof "^{commit}";
    char *spec = malloc(need);
    if (spec == NULL)
      return kbc_err_set(err, KBC_ERR_NOMEM, "resolve: %zu bytes", need);
    snprintf(spec, need, "%s^{commit}", rev);
    if (argv_add(&av, spec) != KBC_OK) {
      free(spec);
      return fail(err, KBC_GIT_E_INTERNAL, "resolve: argv build");
    }
    /* argv holds the pointer until the child has been exec'd, so the
     * free has to follow the run rather than precede it. */
    deferred_free = spec;
  }

  kbc_str_init(&buf);
  /* --quiet is deliberately NOT used: git's "ambiguous argument" /
   * "Needed a single revision" text on stderr is what tells an unborn
   * repository apart from a typo, and suppressing it would collapse both
   * into one undiagnosable failure. */
  st = capture(repo, &av, NULL, 0, 4096u, &buf, &res, err);
  free(deferred_free);
  if (st != KBC_OK) {
    kbc_str_free(&buf);
    return st;
  }
  t = str_trim(buf.ptr, buf.len, &n);
  if (n != 40 || !is_hex(t, n)) {
    kbc_str_free(&buf);
    return fail(err, KBC_GIT_E_INTERNAL, "resolve %s: not a commit id", rev);
  }
  *sha = kbc_arena_strndup(a, t, n);
  kbc_str_free(&buf);
  return *sha != NULL ? KBC_OK : kbc_err_set(err, KBC_ERR_NOMEM, "resolve: arena");
}

kbc_status kbc_git_resolve(const kbc_git_repo *repo, const char *rev,
                           kbc_arena *a, const char **sha, kbc_err *err)
{
  if (repo == NULL || sha == NULL)
    return fail(err, KBC_GIT_E_REJECTED, "resolve: no repo");
  if (a == NULL)
    return fail(err, KBC_GIT_E_REJECTED, "resolve: no arena");
  *sha = NULL;
  return resolve_sha(repo, rev, a, sha, err);
}

/* ================================================================= head */

kbc_status kbc_git_head_info(const kbc_git_repo *repo, kbc_arena *a,
                             kbc_git_head *out, kbc_err *err)
{
  git_argv av;
  kbc_str buf;
  run_result res;
  kbc_status st;
  size_t n;
  const char *t;
  char *full = NULL;
  static const char heads[] = "refs/heads/";

  if (repo == NULL || out == NULL || a == NULL)
    return fail(err, KBC_GIT_E_REJECTED, "head: missing argument");
  memset(out, 0, sizeof *out);

  /* symbolic-ref -q: exit 0 prints the ref HEAD points at, exit 1 means
   * detached, anything else is a broken repository. */
  argv_init(&av, repo->root);
  kbc_str_init(&buf);
  if (argv_add(&av, "symbolic-ref") != KBC_OK ||
      argv_add(&av, "-q") != KBC_OK ||
      argv_add(&av, "HEAD") != KBC_OK) {
    kbc_str_free(&buf);
    return fail(err, KBC_GIT_E_INTERNAL, "head: argv build");
  }
  st = capture(repo, &av, NULL, 0, 4096u, &buf, &res, err);
  if (st != KBC_OK) {
    kbc_str_free(&buf);
    if (kbc_git_error(err) == KBC_GIT_E_FAILED) {
      /* exit 1 with no diagnostic is exactly what -q means for a detached
       * HEAD, so anything else is reported as the repository problem it
       * is and the ambiguous case is resolved by asking git to peel HEAD. */
      kbc_err_reset(err);
      st = resolve_sha(repo, "HEAD", a, &out->commit, err);
      if (st != KBC_OK)
        return fail(err, KBC_GIT_E_CORRUPT,
                    "detached HEAD that does not resolve: %s", repo->root);
      out->state = KBC_GIT_HEAD_DETACHED;
      return KBC_OK;
    }
    return st;
  }
  t = str_trim(buf.ptr, buf.len, &n);
  if (n <= sizeof heads - 1 || memcmp(t, heads, sizeof heads - 1) != 0) {
    kbc_str_free(&buf);
    return fail(err, KBC_GIT_E_INTERNAL, "head: unexpected ref %.*s",
                (int)(n > 80u ? 80u : n), t);
  }
  out->branch = kbc_arena_strndup(a, t + sizeof heads - 1,
                                  n - (sizeof heads - 1));
  /* Freed only after the copy: `t` points into buf. */
  kbc_str_free(&buf);
  if (out->branch == NULL)
    return kbc_err_set(err, KBC_ERR_NOMEM, "head: arena");

  /* Symbolic is not the same as born: a freshly `git init`ed repository
   * names a branch that does not exist yet, and every read against it has
   * to fail as "no commits" rather than as "bad ref". */
  {
    full = malloc(sizeof heads + strlen(out->branch));
    if (full == NULL)
      return kbc_err_set(err, KBC_ERR_NOMEM, "head: refs/heads/%s", out->branch);
    memcpy(full, heads, sizeof heads - 1);
    memcpy(full + sizeof heads - 1, out->branch, strlen(out->branch) + 1);
    argv_init(&av, repo->root);
    if (argv_add(&av, "show-ref") != KBC_OK ||
        argv_add(&av, "--verify") != KBC_OK ||
        argv_add(&av, "--quiet") != KBC_OK ||
        argv_add(&av, "--end-of-options") != KBC_OK ||
        argv_add(&av, full) != KBC_OK) {
      free(full);
      return fail(err, KBC_GIT_E_INTERNAL, "head: argv build");
    }
  }
  kbc_str_init(&buf);
  st = capture(repo, &av, NULL, 0, 4096u, &buf, &res, err);
  /* The argv still points at `full`, so the free has to follow the run. */
  free(full);
  kbc_str_free(&buf);

  if (st != KBC_OK) {
    kbc_err_reset(err);
    out->state = KBC_GIT_HEAD_UNBORN;
    return KBC_OK;
  }
  out->state = KBC_GIT_HEAD_BORN_SYMBOLIC;
  return resolve_sha(repo, "HEAD", a, &out->commit, err);
}

/* ================================================================= refs */

/* %(refname) and %(objectname) separated by NUL, records by LF. A ref name
 * cannot contain LF or NUL — git's own rules forbid every control byte in
 * a ref name — so this framing is unambiguous without the -z form
 * for-each-ref does not have. */
static kbc_status list_refs_kind(const kbc_git_repo *repo, const char *prefix,
                                 kbc_git_ref_kind kind, kbc_arena *a,
                                 kbc_git_refs *out, kbc_err *err)
{
  git_argv av;
  kbc_str buf;
  run_result res;
  kbc_status st;
  size_t plen = strlen(prefix);
  size_t i, start = 0;

  argv_init(&av, repo->root);
  if (argv_add(&av, "for-each-ref") != KBC_OK ||
      argv_add(&av, "--format=%(refname)%00%(objectname)") != KBC_OK ||
      argv_add(&av, "--end-of-options") != KBC_OK ||
      argv_add(&av, prefix) != KBC_OK) {
    return fail(err, KBC_GIT_E_INTERNAL, "refs: argv build");
  }
  kbc_str_init(&buf);
  st = capture(repo, &av, NULL, 0, 4u * 1024u * 1024u, &buf, &res, err);
  if (st != KBC_OK) {
    kbc_str_free(&buf);
    return st;
  }
  for (i = 0; i <= buf.len; i++) {
    char *rec, *nul;
    size_t reclen, namelen;
    kbc_git_ref ref;
    if (i != buf.len && buf.ptr[i] != '\n')
      continue;
    rec = buf.ptr + start;
    reclen = i - start;
    start = i + 1;
    if (reclen == 0)
      continue;
    nul = memchr(rec, '\0', reclen);
    if (nul == NULL)
      continue; /* not our record shape; skipping beats guessing */
    *nul = '\0';
    namelen = (size_t)(nul - rec);
    if (namelen <= plen)
      continue;
    /* refs/remotes/<remote>/HEAD is a symref naming a default branch, not a
     * branch anyone can check out. git also refuses to create a local
     * branch called HEAD, so this never hides a real branch. */
    if (namelen > plen && memcmp(rec + namelen - 5, "/HEAD", 5) == 0)
      continue;
    memset(&ref, 0, sizeof ref);
    ref.kind = kind;
    ref.full_name = kbc_arena_strndup(a, rec, namelen);
    ref.object = kbc_arena_strndup(a, nul + 1,
                               reclen - (size_t)(nul + 1 - rec));
    ref.short_name = kbc_arena_strndup(a, rec + plen, namelen - plen);
    ref.remote = NULL;
    if (kind == KBC_GIT_REF_REMOTE) {
      const char *slash = memchr(rec + plen, '/', namelen - plen);
      if (slash == NULL)
        ref.remote = NULL;
      else
        ref.remote = kbc_arena_strndup(a, rec + plen,
                                       (size_t)(slash - (rec + plen)));
    }
    if (ref.full_name == NULL || ref.object == NULL ||
        ref.short_name == NULL) {
      kbc_str_free(&buf);
      return kbc_err_set(err, KBC_ERR_NOMEM, "refs: arena");
    }
    st = grow_arena_array((void **)&out->items, &out->cap, out->len + 1,
                          sizeof *out->items, a);
    if (st != KBC_OK) {
      kbc_str_free(&buf);
      return kbc_err_set(err, KBC_ERR_NOMEM, "refs: %zu entries", out->len + 1);
    }
    out->items[out->len++] = ref;
  }
  kbc_str_free(&buf);
  return KBC_OK;
}

kbc_status kbc_git_list_refs(const kbc_git_repo *repo, bool include_remote,
                             kbc_arena *a, kbc_git_refs *out, kbc_err *err)
{
  kbc_status st;
  if (repo == NULL || a == NULL || out == NULL)
    return fail(err, KBC_GIT_E_REJECTED, "refs: missing argument");
  memset(out, 0, sizeof *out);
  st = list_refs_kind(repo, "refs/heads/", KBC_GIT_REF_BRANCH, a, out, err);
  if (st != KBC_OK)
    return st;
  st = list_refs_kind(repo, "refs/tags/", KBC_GIT_REF_TAG, a, out, err);
  if (st != KBC_OK || !include_remote)
    return st;
  return list_refs_kind(repo, "refs/remotes/", KBC_GIT_REF_REMOTE, a, out,
                        err);
}

kbc_status kbc_git_default_branch(const kbc_git_repo *repo, kbc_arena *a,
                                  const char **out, kbc_err *err)
{
  kbc_git_head head;
  kbc_status st;
  if (repo == NULL || a == NULL || out == NULL)
    return fail(err, KBC_GIT_E_REJECTED, "default branch: missing argument");
  *out = NULL;
  st = kbc_git_head_info(repo, a, &head, err);
  if (st != KBC_OK)
    return st;
  if (head.state == KBC_GIT_HEAD_BORN_SYMBOLIC)
    *out = head.branch;
  return KBC_OK;
}

/* =============================================================== commit */

kbc_status kbc_git_commit_info(const kbc_git_repo *repo, const char *rev,
                               kbc_arena *a, kbc_git_commit *out,
                               kbc_err *err)
{
  /* Ten NUL-separated fields. The format string is this file's own, which
   * is what makes the parse unambiguous: %s is free text and would collide
   * with any other separator, so it is fenced by %x00 on both sides. */
  static const char fmt[] =
      "--format=%H%x00%T%x00%an%x00%ae%x00%at%x00%cn%x00%ce%x00%ct%x00%s%x00%P";
  git_argv av;
  kbc_str buf;
  run_result res;
  kbc_status st;
  char *f[10];
  char *scratch;
  size_t i, n, nparents;

  if (repo == NULL || rev == NULL || a == NULL || out == NULL)
    return fail(err, KBC_GIT_E_REJECTED, "commit: missing argument");
  memset(out, 0, sizeof *out);

  argv_init(&av, repo->root);
  st = kbc_git_revspec_check(rev, err);
  if (st != KBC_OK)
    return st;
  if (argv_add(&av, "show") != KBC_OK || argv_add(&av, "-s") != KBC_OK ||
      /* A signed commit would otherwise print its signature verification
       * output ahead of the format, and this parse has no room for it. */
      argv_add(&av, "--no-show-signature") != KBC_OK ||
      argv_add(&av, fmt) != KBC_OK || argv_add(&av, "--end-of-options") !=
                                      KBC_OK ||
      argv_add(&av, rev) != KBC_OK) {
    return fail(err, KBC_GIT_E_INTERNAL, "commit: argv build");
  }
  kbc_str_init(&buf);
  st = capture(repo, &av, NULL, 0, 1024u * 1024u, &buf, &res, err);
  if (st != KBC_OK) {
    kbc_str_free(&buf);
    return st;
  }
  /* git terminates every record with exactly one newline, and the last
   * field (%P) has no NUL after it, so that newline would otherwise end up
   * inside the parent list. Strip it before the field pointers are taken. */
  n = buf.len;
  if (n > 0 && buf.ptr[n - 1] == '\n')
    n--;
  /* split_nul writes field pointers into the buffer, so the copy it walks
   * has to live in the arena, not in the kbc_str that is about to die. */
  scratch = kbc_arena_strndup(a, buf.ptr, n);
  kbc_str_free(&buf);
  if (scratch == NULL)
    return kbc_err_set(err, KBC_ERR_NOMEM, "commit: arena");
  if (split_nul(scratch, n, f, 10) != 10)
    return fail(err, KBC_GIT_E_TRUNCATED, "commit %s: malformed record", rev);

  out->oid = kbc_arena_strdup(a, f[0]);
  out->tree = kbc_arena_strdup(a, f[1]);
  out->author = kbc_arena_strdup(a, f[2]);
  out->author_mail = kbc_arena_strdup(a, f[3]);
  out->committer = kbc_arena_strdup(a, f[5]);
  out->committer_mail = kbc_arena_strdup(a, f[6]);
  out->subject = kbc_arena_strdup(a, f[8]);
  if (out->oid == NULL || out->tree == NULL || out->author == NULL ||
      out->author_mail == NULL || out->committer == NULL ||
      out->committer_mail == NULL || out->subject == NULL)
    return kbc_err_set(err, KBC_ERR_NOMEM, "commit %s: arena", rev);
  if (!parse_i64(f[4], strlen(f[4]), &out->author_time) ||
      !parse_i64(f[7], strlen(f[7]), &out->committer_time))
    return fail(err, KBC_GIT_E_TRUNCATED, "commit %s: bad timestamp", rev);

  /* %P is a space-separated parent list and is empty for a root commit. */
  nparents = 0;
  for (const char *p = f[9]; *p != '\0';) {
    const char *q = strchr(p, ' ');
    nparents++;
    p = q != NULL ? q + 1 : p + strlen(p);
  }
  out->parents = nparents > 0 ? kbc_arena_calloc(a, nparents, sizeof(char *))
                              : NULL;
  if (nparents > 0 && out->parents == NULL)
    return kbc_err_set(err, KBC_ERR_NOMEM, "commit %s: %zu parents", rev, nparents);
  i = 0;
  for (const char *p = f[9]; *p != '\0';) {
    const char *q = strchr(p, ' ');
    size_t len = q != NULL ? (size_t)(q - p) : strlen(p);
    out->parents[i] = kbc_arena_strndup(a, p, len);
    if (out->parents[i] == NULL)
      return kbc_err_set(err, KBC_ERR_NOMEM, "commit %s: arena", rev);
    out->parent_count++;
    i++;
    p = q != NULL ? q + 1 : p + len;
  }
  return KBC_OK;
}

/* ================================================================= tree */

kbc_status kbc_git_tree(const kbc_git_repo *repo, const char *rev,
                        const char *path, kbc_arena *a, kbc_git_entries *out,
                        kbc_err *err)
{
  git_argv av;
  kbc_str buf;
  run_result res;
  kbc_status st;
  size_t i, start = 0;
  char *tree_argv_owns = NULL;

  if (repo == NULL || rev == NULL || a == NULL || out == NULL)
    return fail(err, KBC_GIT_E_REJECTED, "tree: missing argument");
  memset(out, 0, sizeof *out);

  argv_init(&av, repo->root);
  st = kbc_git_revspec_check(rev, err);
  if (st != KBC_OK)
    return st;
  if (argv_add(&av, "ls-tree") != KBC_OK || argv_add(&av, "-z") != KBC_OK ||
      /* --long is what puts a size on every blob; without it a caller
       * cannot tell a 4 GiB file from a 4-byte one without reading it. */
      argv_add(&av, "--long") != KBC_OK ||
      argv_add(&av, "--end-of-options") != KBC_OK)
    return fail(err, KBC_GIT_E_INTERNAL, "tree: argv build");
  /* An empty path means the top level. A non-empty one is REPLACED by the
   * peeled form "<rev>:<path>", never added alongside `rev`: ls-tree takes
   * one tree and then pathspecs, so passing both would ask for entries
   * under HEAD matching the pathspec "HEAD:sub" — that is, nothing.
   *
   * Peeling rather than pathspec-ing is also what makes "sub" mean what a
   * caller means by it: a pathspec names the ENTRY, so ls-tree would report
   * the tree "sub" itself rather than what is inside it. A bare "--" with
   * nothing after it is worse still: an empty pathspec, which git answers
   * with an empty listing rather than with the root. */
  if (path != NULL && path[0] != '\0') {
    size_t need;
    char *spec;
    st = kbc_git_path_check(path, err);
    if (st != KBC_OK)
      return st;
    need = strlen(rev) + strlen(path) + 2;
    spec = malloc(need);
    if (spec == NULL)
      return kbc_err_set(err, KBC_ERR_NOMEM, "tree: %zu bytes", need);
    snprintf(spec, need, "%s:%s", rev, path);
    if (argv_add(&av, spec) != KBC_OK) {
      free(spec);
      return fail(err, KBC_GIT_E_INTERNAL, "tree: argv build");
    }
    /* The argv keeps the pointer until the child has exec'd. */
    tree_argv_owns = spec;
  } else if (argv_add(&av, rev) != KBC_OK) {
    return fail(err, KBC_GIT_E_INTERNAL, "tree: argv build");
  }
  kbc_str_init(&buf);
  st = capture(repo, &av, NULL, 0, 8u * 1024u * 1024u, &buf, &res, err);
  free(tree_argv_owns);
  if (st != KBC_OK) {
    kbc_str_free(&buf);
    return st;
  }
  /* Each record: "<mode> SP <type> SP <oid> SP<padded size> TAB <path>" and
   * a NUL terminator. Splitting on the TAB first is what makes a path with
   * a space in it parse correctly. */
  for (i = 0; i < buf.len; i++) {
    char *rec = buf.ptr + start;
    size_t reclen = i - start;
    char *sp_type, *sz, *oidend, *sizestart, *tab;
    kbc_git_entry e;
    if (buf.ptr[i] != '\0')
      continue; /* only a NUL ends a record; a path may contain anything */
    start = i + 1;
    /* "<mode> SP <type> SP <oid> SP<pad><size> TAB <path>". */
    sp_type = memchr(rec, ' ', reclen);
    if (sp_type == NULL)
      continue;
    sz = memchr(sp_type + 1, ' ', reclen - (size_t)(sp_type + 1 - rec));
    if (sz == NULL)
      continue;
    /* "mode SP type SP oid SP<pad>size TAB path": the oid ends at the next
     * space, and the size is right-aligned and padded after it (git emits
     * "      2" for a 2-byte blob and "      -" for a tree). Splitting on
     * the third space instead silently reports every tree and gitlink as
     * a zero-byte object. */
    oidend = sz + 1;
    while (oidend < rec + reclen && *oidend != ' ')
      oidend++;
    sizestart = oidend;
    while (sizestart < rec + reclen && *sizestart == ' ')
      sizestart++;
    tab = memchr(sizestart, '\t', reclen - (size_t)(sizestart - rec));
    /* An empty size token is the only malformed shape here. */
    if (tab == NULL || tab == sizestart || oidend >= rec + reclen)
      continue;
    memset(&e, 0, sizeof e);
    e.path = kbc_arena_strndup(a, tab + 1,
                              reclen - (size_t)(tab + 1 - rec));
    e.oid = kbc_arena_strndup(a, sz + 1, (size_t)(oidend - sz - 1));
    if (e.path == NULL || e.oid == NULL) {
      kbc_str_free(&buf);
      return kbc_err_set(err, KBC_ERR_NOMEM, "tree: arena");
    }
    e.mode = (uint32_t)strtoul(rec, NULL, 8);
    /* The type word git prints is redundant with the mode, and the mode is
     * the authority because it is what git enforces on checkout. */
    if (e.mode == 0040000u)
      e.kind = KBC_GIT_ENTRY_TREE;
    else if (e.mode == 0160000u)
      e.kind = KBC_GIT_ENTRY_COMMIT;
    else if (e.mode == 0120000u)
      e.kind = KBC_GIT_ENTRY_SYMLINK;
    else
      e.kind = KBC_GIT_ENTRY_BLOB;
    /* git prints "-" for a tree or a gitlink: it has no content size. */
    if (tab - sizestart == 1 && sizestart[0] == '-') {
      e.size = UINT64_MAX;
    } else {
      char sizetxt[24];
      size_t slen = (size_t)(tab - sizestart);
      if (slen >= sizeof sizetxt)
        slen = sizeof sizetxt - 1;
      memcpy(sizetxt, sizestart, slen);
      sizetxt[slen] = '\0';
      e.size = strtoull(sizetxt, NULL, 10);
    }
    st = grow_arena_array((void **)&out->items, &out->cap, out->len + 1,
                          sizeof *out->items, a);
    if (st != KBC_OK) {
      kbc_str_free(&buf);
      return kbc_err_set(err, KBC_ERR_NOMEM, "tree: %zu entries", out->len + 1);
    }
    out->items[out->len++] = e;
  }
  kbc_str_free(&buf);
  /* An empty tree for a valid rev means the path simply is not there. git
   * says so itself ("no such path"), but only when it is asked to peel the
   * path; for a plain ls-tree it says nothing at all. */
  if (out->len == 0 && path != NULL && path[0] != '\0')
    return fail(err, KBC_GIT_E_NOT_FOUND, "no such path '%s' in %s", path,
                rev);
  return KBC_OK;
}

/* ================================================================= blob */

kbc_status kbc_git_blob(const kbc_git_repo *repo, const char *rev,
                        const char *path, kbc_str *out, kbc_err *err)
{
  git_argv av;
  run_result res;
  kbc_status st;
  size_t plen;
  char *spec;

  if (repo == NULL || rev == NULL || path == NULL || out == NULL)
    return fail(err, KBC_GIT_E_REJECTED, "blob: missing argument");
  st = kbc_git_revspec_check(rev, err);
  if (st != KBC_OK)
    return st;
  st = kbc_git_path_check(path, err);
  if (st != KBC_OK)
    return st;
  /* The colon is added here and never arrives pre-joined from a caller,
   * so there is exactly one place where a revspec becomes an object name. */
  plen = strlen(rev) + strlen(path) + 2;
  spec = malloc(plen);
  if (spec == NULL)
    return kbc_err_set(err, KBC_ERR_NOMEM, "blob: %zu bytes", plen);
  snprintf(spec, plen, "%s:%s", rev, path);
  argv_init(&av, repo->root);
  st = argv_add(&av, "cat-file");
  if (st == KBC_OK)
    st = argv_add(&av, "blob");
  if (st == KBC_OK)
    st = argv_add(&av, "--end-of-options");
  if (st == KBC_OK)
    st = argv_add(&av, spec);
  if (st != KBC_OK) {
    free(spec);
    return fail(err, KBC_GIT_E_INTERNAL, "blob: argv build");
  }
  st = capture(repo, &av, NULL, 0, KBC_GIT_MAX_BLOB_BYTES, out, &res, err);
  /* The argv still points at `spec` while the child runs. */
  free(spec);
  return st;
}

bool kbc_git_blob_exists(const kbc_git_repo *repo, const char *rev,
                         const char *path, kbc_err *err)
{
  kbc_err scratch;
  kbc_str throwaway;
  kbc_status st;
  kbc_err_reset(&scratch);
  kbc_str_init(&throwaway);
  st = kbc_git_blob(repo, rev, path, &throwaway, &scratch);
  kbc_str_free(&throwaway);
  if (st == KBC_OK)
    return true;
  /* Only "absent" is false. A repository that is broken, or a revspec that
   * was rejected, must not be reported as "the file does not exist" — the
   * caller is about to cache that answer. */
  if (kbc_git_error(&scratch) == KBC_GIT_E_NOT_FOUND)
    return false;
  if (err != NULL)
    *err = scratch;
  return false;
}

/* ================================================================= diff */

kbc_status kbc_git_diff(const kbc_git_repo *repo, const kbc_git_diff_opts *o,
                        kbc_str *out, kbc_err *err)
{
  git_argv av;
  run_result res;
  kbc_status st;
  char uopt[32];
  int ctx;

  if (repo == NULL || o == NULL || out == NULL)
    return fail(err, KBC_GIT_E_REJECTED, "diff: missing argument");
  if (o->from == NULL && o->to != NULL)
    return fail(err, KBC_GIT_E_REJECTED,
                "diff: a range needs its older end (from)");
  ctx = o->context > 0 ? o->context : 3;
  if (ctx > 1000)
    return fail(err, KBC_GIT_E_REJECTED, "diff: context %d out of range", ctx);
  if (o->from != NULL) {
    st = kbc_git_revspec_check(o->from, err);
    if (st != KBC_OK)
      return st;
  }
  if (o->to != NULL) {
    st = kbc_git_revspec_check(o->to, err);
    if (st != KBC_OK)
      return st;
  }
  snprintf(uopt, sizeof uopt, "-U%d", ctx);
  argv_init(&av, repo->root);
  if (argv_add(&av, "diff") != KBC_OK ||
      argv_add(&av, "--no-color") != KBC_OK ||
      argv_add(&av, uopt) != KBC_OK)
    return fail(err, KBC_GIT_E_INTERNAL, "diff: argv build");
  if (o->from != NULL || o->to != NULL) {
    /* One --end-of-options covers every rev that follows it. */
    if (argv_add(&av, "--end-of-options") != KBC_OK)
      return fail(err, KBC_GIT_E_INTERNAL, "diff: argv build");
    if (o->from != NULL && argv_add(&av, o->from) != KBC_OK)
      return fail(err, KBC_GIT_E_INTERNAL, "diff: argv build");
    if (o->to != NULL && argv_add(&av, o->to) != KBC_OK)
      return fail(err, KBC_GIT_E_INTERNAL, "diff: argv build");
  }
  if (o->path != NULL && o->path[0] != '\0') {
    st = argv_add_path(&av, o->path, err);
    if (st != KBC_OK)
      return st;
  }
  return capture(repo, &av, NULL, 0, KBC_GIT_MAX_BLOB_BYTES, out, &res, err);
}

kbc_status kbc_git_diff_objects(const kbc_git_repo *repo, const char *from_sha,
                                const char *from_path, const char *to_sha,
                                const char *to_path, kbc_str *out,
                                kbc_err *err)
{
  git_argv av;
  run_result res;
  kbc_status st;
  char *specs[2];
  size_t i;

  if (repo == NULL || from_sha == NULL || from_path == NULL || to_sha == NULL ||
      to_path == NULL || out == NULL)
    return fail(err, KBC_GIT_E_REJECTED, "diff objects: missing argument");
  st = kbc_git_revspec_check(from_sha, err);
  if (st == KBC_OK)
    st = kbc_git_path_check(from_path, err);
  if (st == KBC_OK)
    st = kbc_git_revspec_check(to_sha, err);
  if (st == KBC_OK)
    st = kbc_git_path_check(to_path, err);
  if (st != KBC_OK)
    return st;
  for (i = 0; i < 2; i++) {
    size_t need = strlen(i == 0 ? from_sha : to_sha) +
                  strlen(i == 0 ? from_path : to_path) + 2;
    specs[i] = malloc(need);
    if (specs[i] == NULL) {
      if (i == 1)
        free(specs[0]);
      return kbc_err_set(err, KBC_ERR_NOMEM, "diff objects: %zu bytes", need);
    }
    snprintf(specs[i], need, "%s:%s", i == 0 ? from_sha : to_sha,
             i == 0 ? from_path : to_path);
  }
  argv_init(&av, repo->root);
  if (argv_add(&av, "diff") != KBC_OK ||
      argv_add(&av, "--no-color") != KBC_OK ||
      argv_add(&av, "-U3") != KBC_OK ||
      argv_add(&av, "--end-of-options") != KBC_OK ||
      argv_add(&av, specs[0]) != KBC_OK || argv_add(&av, specs[1]) != KBC_OK) {
    free(specs[0]);
    free(specs[1]);
    return fail(err, KBC_GIT_E_INTERNAL, "diff objects: argv build");
  }
  st = capture(repo, &av, NULL, 0, KBC_GIT_MAX_BLOB_BYTES, out, &res, err);
  /* Both argv entries are still live while the child runs. */
  free(specs[0]);
  free(specs[1]);
  return st;
}

/* ================================================================= blame */

/* Per-commit metadata, cached for the life of one parse run.
 *
 * git emits author/author-mail/author-time/summary/previous/boundary only
 * the FIRST time a sha appears in a run and never again for a later region
 * citing that same sha. Every one of those is a property of the commit, not
 * of the line group, so the parser keeps them keyed by sha and re-attaches
 * them. Without that cache every region after the first for a commit would
 * come back empty, and a blame view would be wrong in a way no caller could
 * detect. */
typedef struct {
  char sha[41];
  const char *author;
  const char *author_mail;
  int64_t author_time;
  const char *subject;
  const char *previous_sha;
  const char *previous_filename;
  bool boundary;
} blame_meta;

/* Open-addressed, power-of-two, sha[0] == '\0' marks an empty slot. The
 * number of distinct commits touching one file is small but unbounded, and
 * a linear scan would make the parse quadratic on exactly the files this
 * daemon exists to explain. */
typedef struct {
  blame_meta *slots;
  size_t cap;
  size_t used;
} blame_cache;

typedef struct {
  char sha[41];
  uint32_t orig_start, final_start, count;
  const char *author;
  const char *author_mail;
  int64_t author_time;
  const char *subject;
  const char *previous_sha;
  const char *previous_filename;
  bool boundary;
  bool fresh;
} blame_partial;

struct blame_parser {
  kbc_arena *a;
  const kbc_git_blame_opts *o;
  kbc_blames *out;
  kbc_str line;
  blame_partial part;
  bool have_part;
  blame_cache cache;
  size_t cap; /* 0 = unlimited */
};

static size_t sha_slot(const blame_parser *bp, const char *sha)
{
  uint32_t h = kbc_fnv1a32(sha, 40);
  return (size_t)h & (bp->cache.cap - 1u);
}

static blame_meta *cache_find(blame_parser *bp, const char *sha)
{
  size_t i;
  if (bp->cache.slots == NULL)
    return NULL;
  for (i = sha_slot(bp, sha);; i = (i + 1) & (bp->cache.cap - 1u)) {
    if (bp->cache.slots[i].sha[0] == '\0')
      return NULL;
    if (memcmp(bp->cache.slots[i].sha, sha, 40) == 0)
      return &bp->cache.slots[i];
  }
}

static kbc_status cache_put(blame_parser *bp, const char *sha,
                             const blame_meta *m, kbc_err *err)
{
  size_t i;
  if (bp->cache.slots == NULL) {
    bp->cache.cap = 64;
    bp->cache.slots = kbc_arena_calloc(bp->a, bp->cache.cap,
                                       sizeof *bp->cache.slots);
    if (bp->cache.slots == NULL)
      return kbc_err_set(err, KBC_ERR_NOMEM, "blame: cache");
  }
  /* Grow at 70%: linear probing degrades sharply past that, and a blame of
   * a long-lived file touches hundreds of commits. */
  if ((bp->cache.used + 1) * 10 >= bp->cache.cap * 7) {
    blame_cache old = bp->cache;
    size_t j;
    bp->cache.cap = old.cap * 2;
    bp->cache.slots = kbc_arena_calloc(bp->a, bp->cache.cap,
                                       sizeof *bp->cache.slots);
    if (bp->cache.slots == NULL) {
      bp->cache = old;
      return kbc_err_set(err, KBC_ERR_NOMEM, "blame: cache grow");
    }
    for (j = 0; j < old.cap; j++) {
      if (old.slots[j].sha[0] != '\0') {
        size_t k = sha_slot(bp, old.slots[j].sha);
        while (bp->cache.slots[k].sha[0] != '\0')
          k = (k + 1) & (bp->cache.cap - 1u);
        bp->cache.slots[k] = old.slots[j];
      }
    }
  }
  for (i = sha_slot(bp, sha); bp->cache.slots[i].sha[0] != '\0';
       i = (i + 1) & (bp->cache.cap - 1u)) {
    if (memcmp(bp->cache.slots[i].sha, sha, 40) == 0) {
      bp->cache.slots[i] = *m;
      return KBC_OK;
    }
  }
  memcpy(bp->cache.slots[i].sha, sha, 40);
  bp->cache.slots[i].sha[40] = '\0';
  {
    blame_meta *slot = &bp->cache.slots[i];
    slot->author = m->author;
    slot->author_mail = m->author_mail;
    slot->author_time = m->author_time;
    slot->subject = m->subject;
    slot->previous_sha = m->previous_sha;
    slot->previous_filename = m->previous_filename;
    slot->boundary = m->boundary;
  }
  bp->cache.used++;
  return KBC_OK;
}

/* One "<sha> <orig> <final> <count>" line. git never emits a fifth field,
 * and a record with one is not a record this parser knows how to read. */
static kbc_status blame_header(blame_parser *bp, const char *line, size_t n,
                               kbc_err *err)
{
  const char *tok[4];
  size_t tokl[4];
  size_t count = 0, start = 0, i;

  memset(&bp->part, 0, sizeof bp->part);
  /* Exactly four space-separated fields. A fifth, an empty one, or a
   * trailing space is not this protocol and guessing at it would attribute
   * lines to the wrong commit — the one failure mode blame must not have. */
  for (i = 0; i <= n; i++) {
    if (i != n && line[i] != ' ')
      continue;
    if (i == start || count == 4)
      goto bad;
    tok[count] = line + start;
    tokl[count] = i - start;
    count++;
    start = i + 1;
  }
  if (count != 4)
    goto bad;
  if (tokl[0] != 40 || !is_hex(tok[0], 40))
    goto bad;
  memcpy(bp->part.sha, tok[0], 40);
  bp->part.sha[40] = '\0';
  if (!parse_u32(tok[1], tokl[1], &bp->part.orig_start) ||
      !parse_u32(tok[2], tokl[2], &bp->part.final_start) ||
      !parse_u32(tok[3], tokl[3], &bp->part.count))
    goto bad;
  return KBC_OK;

bad:
  return fail(err, KBC_GIT_E_TRUNCATED, "blame: malformed header \"%.*s\"",
              (int)(n > 60u ? 60u : n), line);
}

/* One metadata line. Unknown keys — author-tz, committer*, and whatever a
 * future git adds — are ignored on purpose: the protocol is documented as
 * extensible, and a parser that errored on an unknown key would break on a
 * git upgrade rather than on bad input. */
static void blame_kv(blame_parser *bp, const char *line, size_t n)
{
  blame_partial *p = &bp->part;
  const char *v;
  size_t vn;

  if (n == 8 && memcmp(line, "boundary", 8) == 0) {
    p->boundary = true;
    p->fresh = true;
    return;
  }
  if (n >= 7 && memcmp(line, "author ", 7) == 0) {
    v = line + 7;
    vn = n - 7;
  } else if (n >= 12 && memcmp(line, "author-mail ", 12) == 0) {
    v = line + 12;
    vn = n - 12;
    /* git fences the address in angle brackets; the callers want the
     * address, and both ends are trimmed, matching what the Rust parser
     * does with trim_matches('<' | '>'). */
    while (vn > 0 && v[0] == '<')
      v++, vn--;
    while (vn > 0 && v[vn - 1] == '>')
      vn--;
    p->author_mail = kbc_arena_strndup(bp->a, v, vn);
    p->fresh = true;
    return;
  } else if (n >= 12 && memcmp(line, "author-time ", 12) == 0) {
    int64_t t = 0;
    v = line + 12;
    vn = n - 12;
    if (!parse_i64(v, vn, &t))
      t = 0;
    p->author_time = t;
    p->fresh = true;
    return;
  } else if (n >= 8 && memcmp(line, "summary ", 8) == 0) {
    p->subject = kbc_arena_strndup(bp->a, line + 8, n - 8);
    p->fresh = true;
    return;
  } else if (n == 7 && memcmp(line, "summary", 7) == 0) {
    p->subject = kbc_arena_strdup(bp->a, "");
    p->fresh = true;
    return;
  } else if (n > 9 && memcmp(line, "previous ", 9) == 0) {
    const char *rest = line + 9;
    size_t rn = n - 9;
    const char *sp = memchr(rest, ' ', rn);
    if (sp != NULL) {
      p->previous_sha = kbc_arena_strndup(bp->a, rest, (size_t)(sp - rest));
      p->previous_filename = kbc_arena_strdup(bp->a, sp + 1);
      p->fresh = true;
    }
    return;
  } else {
    p->fresh = true; /* an unknown key still means "metadata block" */
    return;
  }
  p->author = kbc_arena_strndup(bp->a, v, vn);
  p->fresh = true;
}

/* A "filename" line terminates a region. That is the one rule this parser
 * relies on for "the block is complete", and it is why an output stream
 * that ends mid-block is detectable rather than silently short. */
static kbc_status blame_finalize(blame_parser *bp, const char *name, size_t n,
                                 kbc_err *err)
{
  blame_meta meta;
  const blame_meta *cached = NULL;
  kbc_blame_region *r;
  kbc_status st;

  memset(&meta, 0, sizeof meta);
  if (bp->part.fresh) {
    meta.author = bp->part.author;
    meta.author_mail = bp->part.author_mail;
    meta.author_time = bp->part.author_time;
    meta.subject = bp->part.subject;
    meta.previous_sha = bp->part.previous_sha;
    meta.previous_filename = bp->part.previous_filename;
    meta.boundary = bp->part.boundary;
    st = cache_put(bp, bp->part.sha, &meta, err);
    if (st != KBC_OK)
      return st;
    cached = &meta;
  } else {
    cached = cache_find(bp, bp->part.sha);
  }
  if (bp->cap != 0 && bp->out->len >= bp->cap) {
    /* Keep parsing and keep draining: the child is still running, and
     * killing it would turn "we kept what you asked for" into "blame
     * failed". The caller is told the answer is partial. */
    bp->out->truncated = true;
    return KBC_OK;
  }
  st = grow_arena_array((void **)&bp->out->items, &bp->out->cap,
                        bp->out->len + 1, sizeof *bp->out->items, bp->a);
  if (st != KBC_OK)
    return kbc_err_set(err, KBC_ERR_NOMEM, "blame: %zu regions", bp->out->len + 1);
  r = &bp->out->items[bp->out->len];
  memset(r, 0, sizeof *r);
  memcpy(r->sha, bp->part.sha, 41);
  r->orig_start = bp->part.orig_start;
  r->final_start = bp->part.final_start;
  r->count = bp->part.count;
  if (cached != NULL) {
    r->author = cached->author;
    r->author_mail = cached->author_mail;
    r->author_time = cached->author_time;
    r->subject = cached->subject;
    r->previous_sha = cached->previous_sha;
    r->previous_filename = cached->previous_filename;
    r->boundary = cached->boundary;
  }
  r->filename = kbc_arena_strndup(bp->a, name, n);
  if (r->filename == NULL)
    return kbc_err_set(err, KBC_ERR_NOMEM, "blame: arena");
  bp->out->len++;
  return KBC_OK;
}

static kbc_status blame_line(blame_parser *bp, const char *line, size_t n,
                             kbc_err *err)
{
  static const char fn[] = "filename ";
  if (!bp->have_part) {
    kbc_status st = blame_header(bp, line, n, err);
    bp->have_part = true;
    return st;
  }
  if (n > sizeof fn - 1 && memcmp(line, fn, sizeof fn - 1) == 0) {
    kbc_status st = blame_finalize(bp, line + sizeof fn - 1,
                                   n - (sizeof fn - 1), err);
    /* The filename line ENDS the block: leaving the parser mid-block makes
     * the next header line be read as metadata, and every region after the
     * first comes back with a zero sha. */
    bp->have_part = false;
    return st;
  }
  blame_kv(bp, line, n);
  return KBC_OK;
}

/* Called by the sink with whatever bytes arrived. Splitting on '\n' here
 * rather than in the child reader is what lets blame stream: regions are
 * handed to the caller while git is still producing them. */
static kbc_status blame_parser_feed(blame_parser *bp, const char *data,
                                    size_t n, kbc_err *err)
{
  const char *p = data, *end = data + n;
  while (p < end) {
    const char *nl = memchr(p, '\n', (size_t)(end - p));
    size_t take = nl != NULL ? (size_t)(nl - p) : (size_t)(end - p);
    kbc_status st;
    if (bp->line.len + take > GIT_MAX_LINE)
      return fail(err, KBC_GIT_E_IO, "blame: line over %u bytes",
                  GIT_MAX_LINE);
    st = kbc_str_append(&bp->line, p, take);
    if (st != KBC_OK)
      return kbc_err_set(err, KBC_ERR_NOMEM, "blame: line buffer");
    p += take;
    if (nl != NULL) {
      p++;
      st = blame_line(bp, bp->line.ptr, bp->line.len, err);
      kbc_str_clear(&bp->line);
      if (st != KBC_OK)
        return st;
    }
  }
  return KBC_OK;
}

kbc_status kbc_git_blame(const kbc_git_repo *repo,
                         const kbc_git_blame_opts *o, kbc_arena *a,
                         kbc_blames *out, kbc_err *err)
{
  git_argv av;
  git_sink sink;
  kbc_str diag;
  run_result res;
  blame_parser bp;
  kbc_status st;
  bool has_contents;

  if (repo == NULL || o == NULL || a == NULL || out == NULL)
    return fail(err, KBC_GIT_E_REJECTED, "blame: missing argument");
  memset(out, 0, sizeof *out);
  st = kbc_git_path_check(o->path, err);
  if (st != KBC_OK)
    return st;
  if (o->rev != NULL) {
    st = kbc_git_revspec_check(o->rev, err);
    if (st != KBC_OK)
      return st;
  }
  if (o->line_start != 0 && o->line_end != 0 &&
      o->line_end < o->line_start)
    return fail(err, KBC_GIT_E_REJECTED, "blame: line range %u-%u is backwards",
                o->line_start, o->line_end);

  argv_init(&av, repo->root);
  if (argv_add(&av, "blame") != KBC_OK || argv_add(&av, "--incremental") !=
                                           KBC_OK)
    return fail(err, KBC_GIT_E_INTERNAL, "blame: argv build");
  if (o->ignore_revs_file != NULL) {
    if (argv_add(&av, "--ignore-revs-file") != KBC_OK ||
        argv_add(&av, o->ignore_revs_file) != KBC_OK)
      return fail(err, KBC_GIT_E_INTERNAL, "blame: argv build");
  }
  if (o->line_start != 0 || o->line_end != 0) {
    char range[32];
    snprintf(range, sizeof range, "%u,%u", o->line_start, o->line_end);
    if (argv_add(&av, "-L") != KBC_OK || argv_add(&av, range) != KBC_OK)
      return fail(err, KBC_GIT_E_INTERNAL, "blame: argv build");
  }
  has_contents = o->contents != NULL;
  if (has_contents && (argv_add(&av, "--contents") != KBC_OK ||
                       argv_add(&av, "-") != KBC_OK))
    return fail(err, KBC_GIT_E_INTERNAL, "blame: argv build");
  /* No --end-of-options here: git blame's own parser reads the first
   * positional as the revision and everything after "--" as the path, and
   * an --end-of-options in between makes it treat the path as a second
   * revision ("fatal: bad revision"). The revspec validator above and the
   * "--" added by argv_add_path are what keep this invocation safe, and
   * tests/test_git.c proves a leading-dash revspec spawns nothing. */
  if (o->rev != NULL) {
    st = kbc_git_revspec_check(o->rev, err);
    if (st != KBC_OK)
      return st;
    if (argv_add(&av, o->rev) != KBC_OK)
      return fail(err, KBC_GIT_E_INTERNAL, "blame: argv build");
  }
  st = argv_add_path(&av, o->path, err);
  if (st != KBC_OK)
    return st;

  memset(&bp, 0, sizeof bp);
  bp.a = a;
  bp.o = o;
  bp.out = out;
  bp.cap = o->region_cap != 0 ? o->region_cap : KBC_GIT_MAX_BLAME_REGIONS;
  kbc_str_init(&bp.line);
  kbc_str_init(&diag);
  memset(&sink, 0, sizeof sink);
  sink.bp = &bp;
  sink.err = &diag;
  st = run_git(repo, &av, o->alternates, o->contents, o->contents_len, &sink,
               o->timeout_ms, &res, err);
  kbc_str_free(&diag);
  kbc_str_free(&bp.line);
  if (st != KBC_OK)
    return st;
  /* git ends every record with a filename line, so a stream that ends with
   * a half-built region means the output was cut short. Returning the
   * partial answer would attribute the last region's lines to a commit
   * whose metadata never arrived. */
  if (bp.have_part)
    return fail(err, KBC_GIT_E_TRUNCATED,
                "blame: output ended mid-region");
  return KBC_OK;
}

/* ============================================================= timeline */

kbc_status kbc_git_blame_timeline(const kbc_git_repo *repo, const char *path,
                                  uint32_t line, uint32_t max_entries,
                                  kbc_arena *a, kbc_blame_events *out,
                                  kbc_err *err)
{
  /* SOH-delimited rather than space-delimited: %s is free text and would
   * otherwise be indistinguishable from a field boundary. */
  static const char fmt[] = "--format=%x01%H%x01%at%x01%s";
  git_argv av;
  kbc_str buf;
  run_result res;
  kbc_status st;
  char spec[64];
  char *scratch;
  size_t n, i;

  if (repo == NULL || path == NULL || a == NULL || out == NULL)
    return fail(err, KBC_GIT_E_REJECTED, "timeline: missing argument");
  memset(out, 0, sizeof *out);
  st = kbc_git_path_check(path, err);
  if (st != KBC_OK)
    return st;
  if (max_entries == 0 || max_entries > 100000u)
    return fail(err, KBC_GIT_E_REJECTED, "timeline: max_entries %u", max_entries);
  /* The -L argument is "<rev>:<path>" and is a positional, so the value
   * git reads can never be told apart from the path without the path being
   * validated first — which it was, above. */
  if (strlen(path) + 16 >= sizeof spec)
    return fail(err, KBC_GIT_E_REJECTED, "timeline: path too long");
  snprintf(spec, sizeof spec, "%u,%u:%s", line, line, path);
  argv_init(&av, repo->root);
  if (argv_add(&av, "log") != KBC_OK || argv_add(&av, "-L") != KBC_OK ||
      argv_add(&av, spec) != KBC_OK || argv_add(&av, fmt) != KBC_OK ||
      argv_add(&av, "-n") != KBC_OK) {
    return fail(err, KBC_GIT_E_INTERNAL, "timeline: argv build");
  }
  {
    char num[16];
    snprintf(num, sizeof num, "%u", max_entries);
    if (argv_add(&av, num) != KBC_OK)
      return fail(err, KBC_GIT_E_INTERNAL, "timeline: argv build");
  }
  kbc_str_init(&buf);
  st = capture(repo, &av, NULL, 0, 4u * 1024u * 1024u, &buf, &res, err);
  if (st != KBC_OK) {
    kbc_str_free(&buf);
    return st;
  }
  n = buf.len;
  scratch = kbc_arena_strndup(a, buf.ptr, n);
  kbc_str_free(&buf);
  if (scratch == NULL)
    return kbc_err_set(err, KBC_ERR_NOMEM, "timeline: arena");
  /* `git log -L` interleaves the commit header with the diff hunks it
   * prints for each commit, so records are pulled out by their SOH fence
   * and everything between them is the diff and is ignored. */
  for (i = 0; i < n; i++) {
    const char *f[3];
    size_t fl[3];
    size_t got = 0, j, fstart;
    kbc_blame_event ev;
    if (scratch[i] != '\x01')
      continue;
    fstart = i + 1;
    for (j = fstart; j <= n; j++) {
      if (j == n || scratch[j] == '\x01' || scratch[j] == '\n') {
        if (got < 3) {
          f[got] = scratch + fstart;
          fl[got] = j - fstart;
          got++;
        }
        fstart = j + 1;
        if (got == 3 || j == n)
          break;
      }
    }
    if (got != 3)
      break;
    memset(&ev, 0, sizeof ev);
    ev.sha = kbc_arena_strndup(a, f[0], fl[0]);
    ev.subject = kbc_arena_strndup(a, f[2], fl[2]);
    ev.line = line;
    if (ev.sha == NULL || ev.subject == NULL)
      return kbc_err_set(err, KBC_ERR_NOMEM, "timeline: arena");
    if (!parse_i64(f[1], fl[1], &ev.author_time))
      ev.author_time = 0;
    st = grow_arena_array((void **)&out->items, &out->cap, out->len + 1,
                          sizeof *out->items, a);
    if (st != KBC_OK)
      return kbc_err_set(err, KBC_ERR_NOMEM, "timeline: %zu events", out->len + 1);
    out->items[out->len++] = ev;
    i = j;
  }
  return KBC_OK;
}

/* ============================================================== numstat */

kbc_status kbc_git_commit_numstat(const kbc_git_repo *repo, const char *sha,
                                  kbc_arena *a, kbc_git_file_stats *out,
                                  kbc_err *err)
{
  git_argv av;
  kbc_str buf;
  run_result res;
  kbc_status st;
  char *scratch;
  size_t n, i, start = 0;

  if (repo == NULL || sha == NULL || a == NULL || out == NULL)
    return fail(err, KBC_GIT_E_REJECTED, "numstat: missing argument");
  memset(out, 0, sizeof *out);

  argv_init(&av, repo->root);
  /* --format= with nothing after it suppresses the commit header, so the
   * only thing on stdout is the numstat table. */
  if (argv_add(&av, "show") != KBC_OK ||
      argv_add(&av, "--numstat") != KBC_OK ||
      argv_add(&av, "--format=") != KBC_OK ||
      argv_add(&av, "--end-of-options") != KBC_OK ||
      argv_add(&av, sha) != KBC_OK)
    return fail(err, KBC_GIT_E_INTERNAL, "numstat: argv build");
  kbc_str_init(&buf);
  st = capture(repo, &av, NULL, 0, 16u * 1024u * 1024u, &buf, &res, err);
  if (st != KBC_OK) {
    kbc_str_free(&buf);
    return st;
  }
  n = buf.len;
  scratch = kbc_arena_strndup(a, buf.ptr, n);
  kbc_str_free(&buf);
  if (scratch == NULL)
    return kbc_err_set(err, KBC_ERR_NOMEM, "numstat: arena");
  /* "<ins> TAB <del> TAB <path>", one per line. A rename prints one line
   * naming both sides, which this deliberately does not try to split: the
   * caller gets what git reported and nothing is inferred from it. */
  for (i = 0; i < n; i++) {
    char *rec, *t1, *t2;
    size_t reclen;
    kbc_git_file_stat fs;
    if (scratch[i] != '\n')
      continue;
    rec = scratch + start;
    reclen = i - start;
    start = i + 1;
    if (reclen == 0)
      continue;
    t1 = memchr(rec, '\t', reclen);
    if (t1 == NULL)
      continue;
    t2 = memchr(t1 + 1, '\t', reclen - (size_t)(t1 + 1 - rec));
    if (t2 == NULL)
      continue;
    memset(&fs, 0, sizeof fs);
    fs.path = kbc_arena_strndup(a, t2 + 1,
                                reclen - (size_t)(t2 + 1 - rec));
    if (fs.path == NULL)
      return kbc_err_set(err, KBC_ERR_NOMEM, "numstat: arena");
    /* git prints "-\t-" for a binary file: there are no line counts to
     * report, and 0/0 would be indistinguishable from a real empty diff. */
    if ((size_t)(t1 - rec) == 1 && rec[0] == '-') {
      fs.binary = true;
    } else {
      uint32_t ins = 0, del = 0;
      if (!parse_u32(rec, (size_t)(t1 - rec), &ins) ||
          !parse_u32(t1 + 1, (size_t)(t2 - t1 - 1), &del))
        return fail(err, KBC_GIT_E_TRUNCATED,
                    "numstat %s: bad counts in \"%.60s\"", sha, rec);
      fs.insertions = ins;
      fs.deletions = del;
    }
    st = grow_arena_array((void **)&out->items, &out->cap, out->len + 1,
                          sizeof *out->items, a);
    if (st != KBC_OK)
      return kbc_err_set(err, KBC_ERR_NOMEM, "numstat: %zu files", out->len + 1);
    out->items[out->len++] = fs;
  }
  (void)n;
  return KBC_OK;
}

kbc_status kbc_git_commit_patch(const kbc_git_repo *repo, const char *sha,
                                const char *path, kbc_str *out, kbc_err *err)
{
  git_argv av;
  run_result res;
  kbc_status st;

  if (repo == NULL || sha == NULL || path == NULL || out == NULL)
    return fail(err, KBC_GIT_E_REJECTED, "patch: missing argument");
  argv_init(&av, repo->root);
  st = kbc_git_revspec_check(sha, err);
  if (st != KBC_OK)
    return st;
  if (argv_add(&av, "show") != KBC_OK ||
      argv_add(&av, "--format=") != KBC_OK ||
      argv_add(&av, "--no-color") != KBC_OK ||
      argv_add(&av, "--end-of-options") != KBC_OK ||
      argv_add(&av, sha) != KBC_OK)
    return fail(err, KBC_GIT_E_INTERNAL, "patch: argv build");
  st = argv_add_path(&av, path, err);
  if (st != KBC_OK)
    return st;
  return capture(repo, &av, NULL, 0, KBC_GIT_MAX_BLOB_BYTES, out, &res, err);
}

/* ============================================================= checkout */

kbc_status kbc_git_dirty_paths(const kbc_git_repo *repo, kbc_arena *a,
                               kbc_git_dirty *out, kbc_err *err)
{
  git_argv av;
  kbc_str buf;
  run_result res;
  kbc_status st;
  char *scratch;
  size_t n, i, start = 0;

  if (repo == NULL || a == NULL || out == NULL)
    return fail(err, KBC_GIT_E_REJECTED, "status: missing argument");
  memset(out, 0, sizeof *out);
  argv_init(&av, repo->root);
  /* -z so a path containing a space, a quote or a newline survives intact;
   * porcelain without it quotes and escapes, which is a format this file
   * would then have to un-parse. */
  if (argv_add(&av, "status") != KBC_OK ||
      argv_add(&av, "--porcelain") != KBC_OK || argv_add(&av, "-z") != KBC_OK) {
    return fail(err, KBC_GIT_E_INTERNAL, "status: argv build");
  }
  kbc_str_init(&buf);
  st = capture(repo, &av, NULL, 0, 8u * 1024u * 1024u, &buf, &res, err);
  if (st != KBC_OK) {
    kbc_str_free(&buf);
    return st;
  }
  n = buf.len;
  scratch = kbc_arena_strndup(a, buf.ptr, n);
  kbc_str_free(&buf);
  if (scratch == NULL)
    return kbc_err_set(err, KBC_ERR_NOMEM, "status: arena");
  /* "XY <path>" and, for a rename or a copy, a second NUL-terminated
   * record holding the source path. The two-character status prefix is
   * dropped: callers want the paths, and the status is recoverable from
   * `git status` itself. */
  for (i = 0; i < n; i++) {
    const char *path;
    char *rec = scratch + start;
    size_t reclen = i - start;
    start = i + 1;
    if (reclen < 4)
      continue;
    path = kbc_arena_strndup(a, rec + 3, reclen - 3);
    if (path == NULL)
      return kbc_err_set(err, KBC_ERR_NOMEM, "status: arena");
    st = grow_arena_array((void **)&out->items, &out->cap, out->len + 1,
                          sizeof *out->items, a);
    if (st != KBC_OK)
      return kbc_err_set(err, KBC_ERR_NOMEM, "status: %zu paths", out->len + 1);
    out->items[out->len++] = path;
    /* A rename or a copy is reported as "XY new\0old\0". Only the
     * destination exists in the working tree, so the source record is
     * consumed and dropped rather than listed as a path that is not
     * there. */
    if (rec[0] == 'R' || rec[0] == 'C') {
      size_t j;
      for (j = i + 1; j < n && scratch[j] != '\0'; j++) {
      }
      i = j;
    }
  }
  return KBC_OK;
}

kbc_status kbc_git_switch(const kbc_git_repo *repo, const char *target,
                          kbc_arena *a, kbc_git_dirty *dirty, kbc_err *err)
{
  kbc_git_dirty paths;
  git_argv av;
  run_result res;
  kbc_status st;
  bool is_branch;
  char *full;

  if (repo == NULL || target == NULL || a == NULL)
    return fail(err, KBC_GIT_E_REJECTED, "switch: missing argument");
  if (dirty != NULL)
    memset(dirty, 0, sizeof *dirty);
  st = kbc_git_revspec_check(target, err);
  if (st != KBC_OK)
    return st;

  st = kbc_git_dirty_paths(repo, a, &paths, err);
  if (st != KBC_OK)
    return st;
  if (paths.len > 0) {
    if (dirty != NULL)
      *dirty = paths;
    return fail(err, KBC_GIT_E_DIRTY, "working tree is dirty (%zu path(s))",
                paths.len);
  }

  /* `git switch` refuses a tag, a remote branch or a raw object id, while
   * `git checkout` has always detached HEAD for them. Probing for a local
   * branch first is what makes the common case keep HEAD symbolic and the
   * uncommon case behave exactly as an operator's own command would. */
  full = malloc(strlen(target) + 12);
  if (full == NULL)
    return kbc_err_set(err, KBC_ERR_NOMEM, "switch: refs/heads/%s", target);
  snprintf(full, strlen(target) + 12, "refs/heads/%s", target);
  argv_init(&av, repo->root);
  st = argv_add(&av, "show-ref");
  if (st == KBC_OK)
    st = argv_add(&av, "--verify");
  if (st == KBC_OK)
    st = argv_add(&av, "--quiet");
  if (st == KBC_OK)
    st = argv_add(&av, "--end-of-options");
  if (st == KBC_OK)
    st = argv_add(&av, full);
  if (st != KBC_OK) {
    free(full);
    return fail(err, KBC_GIT_E_INTERNAL, "switch: argv build");
  }
  st = capture(repo, &av, NULL, 0, 4096u, NULL, &res, err);
  /* The argv still points at `full` while the child runs. */
  free(full);
  if (st != KBC_OK) {
    /* show-ref exits 1 for "no such ref", which is the ordinary answer to
     * "is this a local branch". Anything else is a real failure. */
    if (res.code != 1)
      return st;
    kbc_err_reset(err);
  }
  is_branch = (st == KBC_OK);

  argv_init(&av, repo->root);
  st = argv_add(&av, is_branch ? "switch" : "checkout");
  if (st == KBC_OK)
    st = argv_add_rev(&av, target, err);
  if (st != KBC_OK)
    return st;
  st = capture(repo, &av, NULL, 0, 4096u, NULL, &res, err);
  if (st != KBC_OK)
    return st;
  return KBC_OK;
}
