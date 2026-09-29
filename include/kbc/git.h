/* git.h — read-only git object access, blame, and the one working-tree
 * write this daemon is allowed to perform.
 *
 * THE SPLIT. Everything here except kbc_git_switch() and the branch probe
 * it performs is read-only: it opens a repo, resolves revspecs, and reads
 * trees and blobs out of the object database. It NEVER reads the working
 * tree to answer a content question (blame of an uncommitted file is asked
 * for explicitly via kbc_blame_opts.contents), because a dirty working tree
 * must not change what history says. kbc_git_switch() is the boundary: it
 * is the only call in this header whose child process writes to the
 * repository, and everything it does is delegated to `git` itself, so git's
 * own ref/index locking is what makes the write safe. kb-c never opens a
 * file under .git/ for writing.
 *
 * WHY A SUBPROCESS. The Rust original reaches for the gix crate for object
 * reads and shells out to `git` for diff- and blame-shaped work (ADR-4).
 * kb-c has no gix and no libgit2 and adding one would be a dependency the
 * rest of the port does not want, so the whole of this module is one
 * spawn-and-parse layer over the `git` binary: git already implements
 * revspec resolution, delta decoding and blame, and re-implementing any of
 * those in C would be strictly worse.
 *
 * SECURITY. This module runs programs, and a path, ref or revspec in this
 * daemon's threat model is attacker-influenced. Three defences, each
 * independently sufficient, and each independently tested:
 *
 *   1. No shell, ever. Every child is execvp()ed from an argv array; there
 *      is no command string anywhere in this file for a value to be
 *      interpolated into.
 *   2. Every caller-supplied value is validated before it can become an
 *      argv element — kbc_git_revspec_check() and kbc_git_path_check() —
 *      and the internal builder rejects a value that fails validation, so a
 *      call site cannot forget.
 *   3. Defence in depth inside git itself: `--end-of-options` terminates
 *      option parsing before every revspec and `--` terminates it before
 *      every pathspec, so a value that somehow reached argv unvalidated
 *      would still be read as data, not as a flag.
 *
 * A leading `-` is the injection: `--upload-pack=<cmd>` turns a ref name
 * into a command. It is rejected at the type level by both validators.
 *
 * The child environment is the parent's, minus every GIT_* variable that
 * redirects where git reads and writes (GIT_DIR, GIT_WORK_TREE,
 * GIT_INDEX_FILE, GIT_OBJECT_DIRECTORY, GIT_CONFIG*, GIT_EXTERNAL_DIFF,
 * GIT_NAMESPACE, ...) — a request must not be able to inherit an operator's
 * ambient GIT_DIR and answer from the wrong repository — plus
 * GIT_CONFIG_NOSYSTEM=1, GIT_TERMINAL_PROMPT=0, GIT_OPTIONAL_LOCKS=0,
 * GIT_PAGER=cat and LC_ALL=C. GIT_ALTERNATE_OBJECT_DIRECTORIES is the one
 * GIT_* variable that may be set, and only through the explicit
 * kbc_blame_opts.alternates field, never inherited.
 */
#ifndef KBC_GIT_H
#define KBC_GIT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "kbc/kbc.h"
#include "kbc/mem.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------- errors --- */

/* The failure taxonomy. These are NAMED because a caller has to react
 * differently to each: a bad ref is a 400, a detached HEAD is not an
 * error at all for reads, a dirty working tree is a 409, a corrupt
 * repository is a 500 that no retry will fix. Collapsing them into one
 * "git failed" makes every caller guess.
 *
 * The code is retrievable from a filled kbc_err via kbc_git_error(). */
typedef enum {
  KBC_GIT_OK = 0,
  KBC_GIT_E_REJECTED,   /* input failed validation; NO process was started */
  KBC_GIT_E_NOT_A_REPO, /* no .git found at or above the given path */
  KBC_GIT_E_UNBORN,     /* repository has no commits yet */
  KBC_GIT_E_DETACHED_HEAD, /* HEAD names a commit, not a branch */
  KBC_GIT_E_BAD_REF,    /* revspec/ref does not resolve */
  KBC_GIT_E_NOT_FOUND,  /* path is absent from that tree */
  KBC_GIT_E_CORRUPT,    /* git reported a broken object database */
  KBC_GIT_E_DIRTY,      /* working tree has uncommitted changes */
  KBC_GIT_E_INTERNAL,   /* this file's invariant broke — a bug in kb-c */
  KBC_GIT_E_SPAWN,      /* could not exec git at all */
  KBC_GIT_E_FAILED,     /* git exited non-zero, not classifiable */
  KBC_GIT_E_IO,         /* pipe read/write failed, or output hit a cap */
  KBC_GIT_E_TRUNCATED,  /* git's output ended mid-record */
  KBC_GIT_E_TIMEOUT,    /* the child exceeded its deadline and was killed */
  KBC_GIT__COUNT
} kbc_git_ecode;

const char *kbc_git_ecode_str(kbc_git_ecode e);
/* KBC_GIT_OK when `e` is NULL or empty; KBC_GIT_E_FAILED when the message
 * was not produced by this module (a caller's own kbc_err). */
kbc_git_ecode kbc_git_error(const kbc_err *e);

/* ------------------------------------------------------- validation ---- */

/* The revspec predicate, byte-for-byte the Rust Revspec::parse rule:
 * non-empty, no leading '-', no control or whitespace byte, no "..", no
 * "@{". "@{" is reflog and upstream syntax, which resolves against local
 * state a remote caller must not be able to address. ".." is a RANGE, not
 * an endpoint — kbc_git_range_parse() is how you spell a range.
 *
 * Existence is deliberately NOT checked here: that is the resolver's job,
 * and a ref that does not exist is KBC_GIT_E_BAD_REF, not KBC_GIT_E_REJECTED.
 * The split matters — one is a client error we caused, the other is the
 * client's fault about data we accepted. */
bool kbc_git_revspec_ok(const char *s);
kbc_status kbc_git_revspec_check(const char *s, kbc_err *err);

/* A repo-relative git pathspec: forward-slash, not absolute, no ".."
 * component, no leading '-', no control byte, at most KBC_MAX_PATH_LEN. */
bool kbc_git_path_ok(const char *rel);
kbc_status kbc_git_path_check(const char *rel, kbc_err *err);

/* A validated `<from>..<to>` / `<from>...<to>` range. Endpoints are
 * revspecs, so ".." can only ever be the separator. three_dot selects
 * `...` (symmetric difference). Borrowed pointers into `s`. */
typedef struct {
  /* Points into the string that was parsed, and is NOT NUL-terminated at
   * the endpoint — the separator is still there. The lengths are the
   * usable form, and they are why they exist: a caller that strlen()s
   * `from` gets "main..HEAD". */
  const char *from;
  size_t from_len;
  const char *to;
  size_t to_len;
  bool three_dot;
} kbc_git_range;

kbc_status kbc_git_range_parse(const char *s, kbc_git_range *out,
                               kbc_err *err);

/* `<rev>:<path>`, the object-addressed form git accepts anywhere a
 * revspec does. Both halves are validated: the revspec half by
 * kbc_git_revspec_check, the path half by kbc_git_path_check. Splitting is
 * at the FIRST ':', which is git's own rule. As with kbc_git_range, the
 * two halves are NOT NUL-terminated at the split; the lengths say where
 * each ends, and they are the usable form. */
kbc_status kbc_git_revpath_parse(const char *s, const char **rev,
                                 size_t *rev_len, const char **path,
                                 size_t *path_len, kbc_err *err);

/* ------------------------------------------------------------- repo ---- */

typedef struct kbc_git_repo kbc_git_repo;

/* Opens the repository containing `root`, walking UP the directory tree
 * the way `git` itself does (so a subdirectory of a checkout works, and a
 * linked worktree resolves to its own gitdir). KBC_OWN: release with
 * kbc_git_repo_close().
 *
 * The stored root is the DISCOVERED toplevel, canonicalised — that is what
 * every child process is pointed at with `-C`, so a caller cannot make two
 * handles disagree about which repository they mean. */
kbc_status kbc_git_repo_open(const char *root, kbc_git_repo **out,
                             kbc_err *err);
void kbc_git_repo_close(kbc_git_repo *repo);
const char *kbc_git_repo_root(const kbc_git_repo *repo);    /* BORROWED */
const char *kbc_git_repo_git_dir(const kbc_git_repo *repo);  /* BORROWED */
bool kbc_git_repo_is_shallow(const kbc_git_repo *repo);
bool kbc_git_repo_is_bare(const kbc_git_repo *repo);

/* The three states a repository's HEAD can be in. UNBORN is `git init`
 * with no commit: HEAD names a branch that does not exist yet. It is not
 * an error — it is what every fresh clone of an empty repository is. */
typedef enum {
  KBC_GIT_HEAD_BORN_SYMBOLIC = 0, /* HEAD -> refs/heads/<branch>, which exists */
  KBC_GIT_HEAD_DETACHED,          /* HEAD names a commit directly */
  KBC_GIT_HEAD_UNBORN             /* HEAD -> a branch with no commits yet */
} kbc_git_head_state;

typedef struct {
  kbc_git_head_state state;
  const char *branch; /* ARENA: short branch name; NULL unless BORN_SYMBOLIC */
  const char *commit; /* ARENA: 40-hex; NULL when UNBORN */
} kbc_git_head;

kbc_status kbc_git_head_info(const kbc_git_repo *repo, kbc_arena *a,
                             kbc_git_head *out, kbc_err *err);

/* ------------------------------------------------------------- refs ---- */

typedef enum {
  KBC_GIT_REF_BRANCH = 0,
  KBC_GIT_REF_TAG,
  KBC_GIT_REF_REMOTE
} kbc_git_ref_kind;

typedef struct {
  kbc_git_ref_kind kind;
  const char *full_name;  /* ARENA: refs/heads/main */
  const char *short_name; /* ARENA: main, or origin/main for a remote */
  const char *object;     /* ARENA: 40-hex object the ref points at */
  const char *remote;     /* ARENA: origin; NULL for a local branch or tag */
} kbc_git_ref;

/* KBC_ARENA: `items` dies with `a`; never free it individually. */
typedef struct {
  kbc_git_ref *items;
  size_t len, cap;
} kbc_git_refs;

/* Local branches and tags, optionally with remote-tracking branches.
 * Remote `<remote>/HEAD` symrefs are excluded: they are a pointer to a
 * default branch, not a branch an operator can check out. */
kbc_status kbc_git_list_refs(const kbc_git_repo *repo, bool include_remote,
                             kbc_arena *a, kbc_git_refs *out, kbc_err *err);

/* The branch a bare-ish "what would a clone land on" question wants:
 * refs/remotes/origin/HEAD's target when it is set, else HEAD's own
 * branch. NULL on an unborn or detached HEAD. */
kbc_status kbc_git_default_branch(const kbc_git_repo *repo, kbc_arena *a,
                                  const char **out, kbc_err *err);

/* --------------------------------------------------------- resolving --- */

/* `rev` to a commit id. KBC_GIT_E_BAD_REF when it does not name one;
 * KBC_GIT_E_REJECTED when it failed validation and no process was run. */
kbc_status kbc_git_resolve(const kbc_git_repo *repo, const char *rev,
                           kbc_arena *a, const char **sha, kbc_err *err);

/* ----------------------------------------------------------- commits --- */

typedef struct {
  const char *oid;            /* ARENA: 40-hex */
  const char *tree;           /* ARENA: 40-hex */
  const char *author;         /* ARENA */
  const char *author_mail;    /* ARENA, angle brackets stripped */
  int64_t author_time;        /* unix seconds */
  const char *committer;      /* ARENA */
  const char *committer_mail; /* ARENA */
  int64_t committer_time;     /* unix seconds */
  const char *subject;        /* ARENA: first line of the message */
  size_t parent_count;        /* 0 for a root commit */
  const char **parents;       /* ARENA array of ARENA 40-hex, in order */
} kbc_git_commit;

kbc_status kbc_git_commit_info(const kbc_git_repo *repo, const char *rev,
                               kbc_arena *a, kbc_git_commit *out,
                               kbc_err *err);

/* -------------------------------------------------------------- tree --- */

typedef enum {
  KBC_GIT_ENTRY_BLOB = 0,
  KBC_GIT_ENTRY_TREE,
  KBC_GIT_ENTRY_COMMIT, /* submodule: a gitlink, not content */
  KBC_GIT_ENTRY_SYMLINK
} kbc_git_entry_kind;

typedef struct {
  kbc_git_entry_kind kind;
  uint32_t mode;    /* raw git mode bits, e.g. 0100644 */
  const char *path; /* ARENA: repo-relative, '/' separated */
  const char *oid;  /* ARENA: 40-hex */
  uint64_t size;    /* blob bytes; UINT64_MAX for a tree or gitlink */
} kbc_git_entry;

/* KBC_ARENA. */
typedef struct {
  kbc_git_entry *items;
  size_t len, cap;
} kbc_git_entries;

/* Non-recursive listing of one directory in `rev`. An empty `path` lists
 * the top level. A path that names a blob rather than a directory lists
 * that single entry. KBC_GIT_E_NOT_FOUND when the path is absent. */
kbc_status kbc_git_tree(const kbc_git_repo *repo, const char *rev,
                        const char *path, kbc_arena *a, kbc_git_entries *out,
                        kbc_err *err);

/* -------------------------------------------------------------- blob --- */

/* A blob is read into caller memory and can be arbitrarily large, so it
 * is bounded. 8 MiB is above every source file this daemon will ever hold
 * and far below anything that would matter as an OOM. */
#define KBC_GIT_MAX_BLOB_BYTES (8u * 1024u * 1024u)

/* Bytes of `<rev>:<path>` appended to `out` (not cleared). Revspec and
 * path are both validated; `--` and `--end-of-options` are inserted
 * regardless. KBC_GIT_E_NOT_FOUND / KBC_GIT_E_BAD_REF as appropriate,
 * KBC_GIT_E_IO if the blob exceeds KBC_GIT_MAX_BLOB_BYTES. */
kbc_status kbc_git_blob(const kbc_git_repo *repo, const char *rev,
                        const char *path, kbc_str *out, kbc_err *err);

bool kbc_git_blob_exists(const kbc_git_repo *repo, const char *rev,
                         const char *path, kbc_err *err);

/* -------------------------------------------------------------- diff --- */

typedef struct {
  const char *from;   /* NULL = the empty tree (i.e. --root) */
  const char *to;     /* NULL = the working tree */
  const char *path;   /* NULL = every path */
  int context;        /* 0 selects git's default of 3 */
} kbc_git_diff_opts;

/* Unified diff text, git's own renderer, `--no-color -U<n>` so the output
 * is stable enough to compare and to show. KBC_GIT_E_IO if it exceeds
 * KBC_GIT_MAX_BLOB_BYTES. */
kbc_status kbc_git_diff(const kbc_git_repo *repo, const kbc_git_diff_opts *o,
                        kbc_str *out, kbc_err *err);

/* Two object-addressed endpoints, for comparing one file's history across
 * renames: `<from_sha>:<from_path>` vs `<to_sha>:<to_path>`. Each of the
 * four arguments is validated; a colon is added by this function, never by
 * the caller. */
kbc_status kbc_git_diff_objects(const kbc_git_repo *repo, const char *from_sha,
                                const char *from_path, const char *to_sha,
                                const char *to_path, kbc_str *out,
                                kbc_err *err);

/* ------------------------------------------------------------ blame ---- */

/* One attributed line-group, exactly as `git blame --incremental`
 * describes it. A region covers `count` consecutive lines starting at
 * `final_start` in the requested version and `orig_start` in the blamed
 * commit's version.
 *
 * The metadata fields are a property of the COMMIT, not of the line group.
 * git emits them only the first time a given sha appears in a run, so
 * kbc_git_blame() re-attaches them to every later region citing that sha —
 * every region here is complete even though the wire was not. */
typedef struct {
  char sha[41];
  uint32_t orig_start;
  uint32_t final_start;
  uint32_t count;
  const char *author;         /* ARENA */
  const char *author_mail;    /* ARENA, angle brackets stripped */
  int64_t author_time;        /* unix seconds */
  const char *subject;        /* ARENA */
  const char *previous_sha;   /* ARENA or NULL; NULL exactly when boundary */
  const char *previous_filename; /* ARENA or NULL */
  const char *filename;       /* ARENA: the path THIS COMMIT knew the file by */
  bool boundary;              /* root commit, or a shallow/--since edge */
} kbc_blame_region;

/* KBC_ARENA. `truncated` is set when region_cap stopped regions from
 * being kept; the wire is still drained to completion so the child is
 * never killed for producing more than we asked to keep. */
typedef struct {
  kbc_blame_region *items;
  size_t len, cap;
  bool truncated;
} kbc_blames;

typedef struct {
  const char *rev;              /* NULL lets git blame the checked-out HEAD */
  const char *path;             /* REQUIRED, validated */
  const char *contents;         /* NULL: blame what git reads. Else blame
                                 * these exact bytes (--contents -), which
                                 * is how an uncommitted edit is attributed
                                 * without consulting the working tree. */
  size_t contents_len;
  const char *ignore_revs_file; /* NULL, or an operator-chosen config path */
  const char *alternates;       /* NULL, or a read-only GIT_ALTERNATE_OBJECT_
                                 * DIRECTORIES. This is the only GIT_*
                                 * variable a caller may set, and it is set
                                 * on this invocation alone. */
  uint32_t line_start, line_end;/* 0,0 = the whole file */
  uint32_t region_cap;          /* 0 = KBC_GIT_MAX_BLAME_REGIONS */
  uint32_t timeout_ms;          /* 0 = no deadline */
} kbc_git_blame_opts;

#define KBC_GIT_MAX_BLAME_REGIONS 200000u

/* Runs `git blame --incremental` and parses the wire protocol. The child
 * is streamed: stdout is consumed line by line while it runs, so a file
 * larger than memory still works, and stdin (`--contents`) is pumped
 * concurrently with stdout by one poll() loop, because writing a payload
 * larger than the pipe buffer while not reading stdout deadlocks git.
 * KBC_GIT_E_TRUNCATED if the stream ends mid-region, KBC_GIT_E_TIMEOUT if
 * the child outlived `timeout_ms` (it is killed and reaped). */
kbc_status kbc_git_blame(const kbc_git_repo *repo,
                         const kbc_git_blame_opts *o, kbc_arena *a,
                         kbc_blames *out, kbc_err *err);

/* The `git log -L` line history behind one line range. */
typedef struct {
  const char *sha;       /* ARENA */
  int64_t author_time;   /* unix seconds */
  const char *subject;   /* ARENA */
  uint32_t line;         /* the line number this event concerns */
} kbc_blame_event;

/* KBC_ARENA. `max_entries` bounds the subprocess itself (-n), not a
 * post-hoc truncation, so a file with a million revisions costs one git
 * run of `max_entries` depth. */
typedef struct {
  kbc_blame_event *items;
  size_t len, cap;
} kbc_blame_events;

kbc_status kbc_git_blame_timeline(const kbc_git_repo *repo, const char *path,
                                  uint32_t line, uint32_t max_entries,
                                  kbc_arena *a, kbc_blame_events *out,
                                  kbc_err *err);

/* ------------------------------------------------------------ numstat -- */

typedef struct {
  const char *path;      /* ARENA, repo-relative */
  uint32_t insertions;
  uint32_t deletions;
  bool binary;          /* git reported "-\t-": line counts do not apply */
} kbc_git_file_stat;

/* KBC_ARENA. */
typedef struct {
  kbc_git_file_stat *items;
  size_t len, cap;
} kbc_git_file_stats;

/* Per-file line deltas of one commit, from `git show --numstat`. `sha` is
 * expected to be a full id this daemon resolved itself; it is validated
 * like any other revspec regardless. A merge commit is diffed against its
 * first parent, which is git's own default for `git show`. */
kbc_status kbc_git_commit_numstat(const kbc_git_repo *repo, const char *sha,
                                  kbc_arena *a, kbc_git_file_stats *out,
                                  kbc_err *err);

/* The full patch text of one file in one commit. */
kbc_status kbc_git_commit_patch(const kbc_git_repo *repo, const char *sha,
                                const char *path, kbc_str *out,
                                kbc_err *err);

/* ----------------------------------------------------------- checkout -- */

/* KBC_ARENA. `items` are repo-relative paths exactly as `git status
 * --porcelain` reports them, minus the two-character status prefix. */
typedef struct {
  const char **items;
  size_t len, cap;
} kbc_git_dirty;

kbc_status kbc_git_dirty_paths(const kbc_git_repo *repo, kbc_arena *a,
                               kbc_git_dirty *out, kbc_err *err);

/* THE WRITE. Moves HEAD and the working tree, and is the only call here
 * that does. `target` is a revspec-validated branch name, tag, remote
 * branch or object id — NOT a path and NOT an option.
 *
 * Sequence, and it is not atomic as a whole:
 *   1. `git status --porcelain -z`; if anything is dirty, NOTHING is
 *      written, `dirty` is filled, and KBC_GIT_E_DIRTY is returned.
 *   2. `git show-ref --verify --quiet refs/heads/<target>` decides the verb:
 *      `git switch <target>` for a branch that exists locally (so HEAD
 *      stays symbolic and the checkout is an ordinary fast-forward), and
 *      `git checkout <target>` for anything else, which is what an operator
 *      typing at a terminal would get and what detaches HEAD for a tag, a
 *      remote branch or a raw object id.
 *   3. the child does the work, under git's own ref and index locking.
 *
 * There is no rollback and there cannot be one: git, not kb-c, holds the
 * locks. The window between step 1 and step 3 is a genuine TOCTOU — a tree
 * dirtied by another process in between is git's problem, and this is why
 * step 1 sets GIT_OPTIONAL_LOCKS=0 rather than fighting for the index.
 * Callers must treat KBC_GIT_E_DIRTY as advisory, not as a guarantee. */
kbc_status kbc_git_switch(const kbc_git_repo *repo, const char *target,
                          kbc_arena *a, kbc_git_dirty *dirty, kbc_err *err);

#ifdef __cplusplus
}
#endif

#endif /* KBC_GIT_H */
