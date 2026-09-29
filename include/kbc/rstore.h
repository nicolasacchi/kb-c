/* rstore.h — the kb-owned review-store mirror manager.
 *
 * NOT a review feature. This is a third-party git mirror manager: it clones
 * OTHER people's repositories, holds credentials for them, runs scheduled
 * housekeeping and GC, and writes backup bundles. Everything it touches is
 * outside kb-c's own tree.
 *
 * Layering, and why the order is the security property:
 *
 *   url       attacker-influenced remote strings -> validated argv atoms
 *   redact    the ONE path from raw subprocess bytes to a loggable string
 *   cred      the credential ladder; the secret never leaves memory
 *   git       the hardened spawner; the ONE place a secret can enter a
 *             child process
 *   manifest  the directory <-> identity tie, and the per-store flock
 *   seed      seed-by-fetch into a bare mirror
 *   gc        keep-set classification and the guarded ref-delete
 *   backup    bundle write / prune / restore, and the restore guard
 *
 * THE SECRET PATH, stated once so no reader has to re-derive it. A token
 * never reaches a child argv, a child environment, a log line, an error
 * string, or disk. It is written into a CLOEXEC pipe; the read end is
 * dup'ed onto descriptor 3 INSIDE the spawned child only (posix_spawn file
 * actions), so the token exists in the daemon's memory, the pipe buffer,
 * and git's memory, and nowhere else. git is told about it by a fixed
 * `credential.helper=!` shell snippet whose text is composed only from the
 * fd number and the credential's already-validated protocol and authority;
 * the snippet relays descriptor 3 to git's own credential plumbing. argv is
 * world-readable through /proc/<pid>/cmdline on most systems, which is why
 * the "-c credential.helper" route exists at all: a helper that READS the
 * secret is safe, a helper that CONTAINS it is not.
 *
 * Every fallible function takes `kbc_err *` (NULL-safe) and returns
 * kbc_status. Ownership: kbc_rs_secret, kbc_rs_cred, kbc_rs_lock and the
 * heap members of kbc_rs_gc_report / kbc_rs_restore_report are KBC_OWN and
 * have an explicit _free. Everything else in this header is value or
 * BORROWED.
 */
#ifndef KBC_RSTORE_H
#define KBC_RSTORE_H

#include <sys/types.h> /* pid_t, for the lock holder */

#include "kbc/kbc.h"
#include "kbc/mem.h" /* kbc_str, kbc_strlist */

#ifdef __cplusplus
extern "C" {
#endif

#define KBC_RS_PATH_MAX 4096
#define KBC_RS_REFNAME_MAX 256
#define KBC_RS_OID_MAX 65 /* 64 hex + NUL; git is moving to sha256 */
#define KBC_RS_KEY_MAX 320 /* "host[:port]/owner/name" */
#define KBC_RS_UUID_MAX 40
#define KBC_RS_URL_MAX 1024
#define KBC_RS_DETAIL_MAX 256

/* Bound on the process capture buffers. A chatty child is drained and
 * discarded past these, so it can neither block on a full pipe nor grow
 * daemon memory without limit. */
#define KBC_RS_STDERR_CAP (64u * 1024u)
#define KBC_RS_STDOUT_CAP (16u * 1024u * 1024u)
#define KBC_RS_MAX_ARGS 4096
#define KBC_RS_MAX_ENV 128
#define KBC_RS_MAX_COVER 4096

/* What a redacted span becomes. Exposed so a caller can assert on it. */
#define KBC_RS_REDACTED "[redacted]"

/* ---------------------------------------------------------------- redact -- */

/* An explicit list of secret literals the calling operation held in hand.
 * A literal is replaced whatever its length: the caller put it in this list
 * BECAUSE it is a secret, and a one-byte secret is still a secret. The
 * length floor that keeps a 40-hex object id readable lives on the shape
 * rules inside kbc_rs_redact, not here. */
typedef struct {
  const char *const *items;
  size_t len;
} kbc_rs_secrets;

/* Shape rules only. `n` bytes at `s` (may contain NULs; they are neither
 * matched nor trusted to terminate anything). Appends to `out`. */
kbc_status kbc_rs_redact(kbc_str *out, const char *s, size_t n);

/* Shape rules AND every literal in `secrets`. */
kbc_status kbc_rs_redact_with(kbc_str *out, const char *s, size_t n,
                              const kbc_rs_secrets *secrets);

/* The ONE path from raw subprocess bytes to a storable/loggable string:
 * redact, then cap at `max` bytes on a UTF-8 boundary. */
kbc_status kbc_rs_redact_bytes(kbc_str *out, const char *s, size_t n,
                               const kbc_rs_secrets *secrets, size_t max);

/* ------------------------------------------------------------------- url -- */

typedef enum {
  KBC_RS_PROTO_HTTPS = 0,
  KBC_RS_PROTO_SSH = 1,
  KBC_RS_PROTO_FILE = 2 /* a LOCAL seed source only; never a remote */
} kbc_rs_proto;

const char *kbc_rs_proto_str(kbc_rs_proto p);

/* A remote URL that passed the allowlist. Invariant: `raw` is what goes in
 * argv, and nothing in it can begin with '-' — every accepted form starts
 * with a fixed scheme, "git@", or '/'. That is why no separate leading-dash
 * check exists here. */
typedef struct {
  kbc_rs_proto proto;
  char raw[KBC_RS_URL_MAX];
  char authority[264]; /* host[:port], default port folded out */
  char host[254];
  char path[1024];
} kbc_rs_url;

/* A NETWORK remote. https://host[:port]/path (never with userinfo — a token
 * in a URL is a token in /proc), or ssh://git@host[:port]/path, or scp form
 * git@host:path. Rejects file://, plain http://, <transport>::<address>,
 * control characters, whitespace, IPv6 literals, and any percent escape in
 * the path. A rejected URL is never echoed back: the message names the RULE
 * it broke, not the value. */
kbc_status kbc_rs_url_parse_remote(const char *s, kbc_rs_url *out,
                                   kbc_err *err);

/* A LOCAL seed source (a member clone), by absolute path. Only ever used
 * with the file-only protocol allowance. */
kbc_status kbc_rs_url_local_seed(const char *abs_path, kbc_rs_url *out,
                                 kbc_err *err);

/* The https form of an ssh URL on the same host, derived only when the
 * mapping is unambiguous (no non-default port, no home-relative path).
 * KBC_ERR_UNSUPPORTED otherwise — the caller must not guess. */
kbc_status kbc_rs_url_https_equivalent(const kbc_rs_url *u, kbc_rs_url *out,
                                       kbc_err *err);

/* A remote NAME in a store config ("base", "work-<id>"). [a-z][a-z0-9-]*. */
kbc_status kbc_rs_remote_name_is_valid(const char *name);

/* store_key = "host[:port]/owner/name", host lower-cased, default port
 * dropped, userinfo never included, trailing ".git" dropped. Refuses any
 * percent escape. `local:<uuid>` is minted by the caller, not here. */
kbc_status kbc_rs_store_key(const kbc_rs_url *u, char *out, size_t cap,
                            kbc_err *err);

/* ------------------------------------------------------------------ cred -- */

/* Typed failure classes with stable slugs. The slug is what a caller
 * persists and what a UI matches on; renaming one is a wire break. */
typedef enum {
  KBC_RS_CLASS_VANISHED = 0,
  KBC_RS_CLASS_OFFLINE,
  KBC_RS_CLASS_TIMEOUT,
  KBC_RS_CLASS_CREDENTIAL_REJECTED,
  KBC_RS_CLASS_CREDENTIAL_WRONG_REPO,
  KBC_RS_CLASS_REPO_NOT_FOUND,
  KBC_RS_CLASS_AUTH_NO_ACCESS,
  KBC_RS_CLASS_HOST_KEY_UNKNOWN,
  KBC_RS_CLASS_HOST_KEY_MISMATCH,
  KBC_RS_CLASS_AUTH_REQUIRED,
  KBC_RS_CLASS_TLS,
  KBC_RS_CLASS_DISK_FULL,
  KBC_RS_CLASS_PROTOCOL_REFUSED,
  KBC_RS_CLASS_URL_REJECTED,
  KBC_RS_CLASS_CREDENTIAL_ACCOUNT_MISMATCH,
  KBC_RS_CLASS_CREDENTIAL_UNAVAILABLE,
  KBC_RS_CLASS_NO_CREDENTIALS,
  KBC_RS_CLASS_SPAWN_FAILED,
  KBC_RS_CLASS_FAILED
} kbc_rs_class;

const char *kbc_rs_class_slug(kbc_rs_class c);
kbc_rs_class kbc_rs_class_from_slug(const char *slug);

/* Classify redacted git stderr. `authed` says whether the failing call
 * carried a credential, because the same stderr means different things with
 * and without one. */
kbc_rs_class kbc_rs_classify(const char *stderr, size_t n, bool authed);

/* A secret in memory only: wiped on free, never format-able. There is no
 * accessor that returns a bare `const char *`; the only consumer inside
 * this library is the credential pipe, and the only public rendering is
 * kbc_rs_secret_len / kbc_rs_secret_eq (constant-time). */
typedef struct {
  char *bytes; /* KBC_OWN, NUL-terminated, wiped */
  size_t len;
} kbc_rs_secret;

/* Rejects empty, >1024 bytes, or any whitespace/control character (a
 * newline would inject lines into the git credential protocol). Length is
 * NOT otherwise bounded: a short token is a credential the operator is told
 * about, not one the ladder silently steps over. */
kbc_status kbc_rs_secret_new(kbc_rs_secret *out, const char *raw,
                             kbc_err *err);
void kbc_rs_secret_wipe(kbc_rs_secret *s);
size_t kbc_rs_secret_len(const kbc_rs_secret *s);
/* Constant-time. `candidate` need not be NUL-terminated at n bytes. */
bool kbc_rs_secret_eq(const kbc_rs_secret *s, const char *candidate,
                      size_t n);

/* `[[review.repos]] credential` — the PIN. A pinned rung that fails is an
 * ERROR, never a fall-through. */
typedef enum {
  KBC_RS_PIN_AUTO = 0,
  KBC_RS_PIN_GH_CLI,
  KBC_RS_PIN_DEPLOY_KEY, /* Phase 2: always refused, never a rung */
  KBC_RS_PIN_TOKEN,
  KBC_RS_PIN_ANONYMOUS,
  KBC_RS_PIN_INHERIT,
  KBC_RS_PIN_NONE
} kbc_rs_cred_pin;

/* Tolerant: an unknown value falls back to auto and returns true in
 * `*unknown`, so a typo warns instead of stopping the daemon. */
kbc_rs_cred_pin kbc_rs_cred_pin_parse(const char *s, bool *unknown);
const char *kbc_rs_cred_pin_str(kbc_rs_cred_pin p);

typedef enum {
  KBC_RS_PROFILE_GH_CLI = 0,
  KBC_RS_PROFILE_DEPLOY_KEY,
  KBC_RS_PROFILE_TOKEN_FILE,
  KBC_RS_PROFILE_ANONYMOUS,
  KBC_RS_PROFILE_INHERIT,
  KBC_RS_PROFILE_NONE
} kbc_rs_profile;

const char *kbc_rs_profile_slug(kbc_rs_profile p);

/* How a call authenticates, and therefore which transports it may use. */
typedef enum {
  KBC_RS_AUTH_LOCAL_ONLY = 0, /* file only, no credential of any kind */
  KBC_RS_AUTH_ANONYMOUS,      /* https, no credential */
  KBC_RS_AUTH_TOKEN,          /* https, token through the pipe helper */
  KBC_RS_AUTH_INHERIT         /* the ambient environment (legacy, amber) */
} kbc_rs_auth_kind;

typedef struct {
  kbc_rs_cred_pin pin;
  const char *gh_user;          /* BORROWED, may be NULL */
  const char *token_file;       /* absolute, ~-expanded by the caller */
  const char *token_username;   /* defaults to x-access-token */
  bool allow_inherited_credentials;
  const char *recorded_account; /* the store's persisted cred_account */
} kbc_rs_cred_cfg;

#define KBC_RS_MAX_SKIPPED 8
#define KBC_RS_ACCOUNT_MAX 40

typedef struct {
  kbc_rs_profile rung;
  kbc_rs_class cls;
  char reason[KBC_RS_DETAIL_MAX];
} kbc_rs_skipped;

/* The resolved fetch credential. KBC_OWN (wipe it with _free). */
typedef struct {
  kbc_rs_profile profile;
  kbc_rs_auth_kind auth;
  char host[KBC_RS_URL_MAX];  /* the scope's authority; a token is never
                                 offered to any other protocol+authority */
  char username[64];
  char account[KBC_RS_ACCOUNT_MAX]; /* the gh login, "" otherwise */
  kbc_rs_secret secret;            /* zeroed; empty for non-token rungs */
  bool broader_than_needed;        /* gh scopes broader than fetch+GET */
  char reason[KBC_RS_DETAIL_MAX];
  kbc_rs_skipped skipped[KBC_RS_MAX_SKIPPED];
  size_t n_skipped;
} kbc_rs_cred;

void kbc_rs_cred_init(kbc_rs_cred *c);
void kbc_rs_cred_free(kbc_rs_cred *c);
/* True for the two rungs that actually carry a secret. */
bool kbc_rs_cred_has_secret(const kbc_rs_cred *c);

/* What a gh-cli read produces: the account (never the token) plus the
 * token itself. Filled by the probes, consumed by the ladder. */
typedef struct {
  char account[KBC_RS_ACCOUNT_MAX];
  bool broader_than_needed;
  kbc_rs_secret secret;
} kbc_rs_gh_login;

/* The side-effecting probes the ladder needs, behind a vtable so the
 * ORDER and the fall-through rules are testable with no gh and no network.
 * Every callback returns a kbc_status AND sets *out->cls, because the
 * class is what decides skip-vs-stop and it is not derivable from the
 * status. */
typedef struct {
  /* Returns KBC_ERR_NOTFOUND when gh is absent or logged out (a rung that
   * does not apply), KBC_ERR_CONFLICT for an account mismatch, and
   * KBC_ERR_INVALID for a credential that was PRESENT and is broken. */
  kbc_status (*gh_cli)(void *user, const kbc_rs_url *url, const char *pinned,
                       const char *recorded, kbc_rs_gh_login *out,
                       kbc_rs_class *cls, kbc_err *err);
  /* Reads an owner-only (0600/0400) token file. Same status discipline. */
  kbc_status (*token_file)(void *user, const char *path, const char *username,
                           const kbc_rs_url *url, kbc_rs_secret *out,
                           kbc_rs_class *cls, kbc_err *err);
  /* A scrubbed `ls-remote <url> HEAD`. */
  kbc_status (*anonymous)(void *user, const kbc_rs_url *url, kbc_rs_class *cls,
                          kbc_err *err);
  void *user;
} kbc_rs_ladder_probes;

/* Walk the ladder. Stops (rather than falling through) on an account
 * mismatch and on a token file that was present and readable but is
 * broken, because falling through would silently swap the identity the
 * store fetches as. */
kbc_status kbc_rs_cred_resolve(const kbc_rs_cred_cfg *cfg, const kbc_rs_url *url,
                               const kbc_rs_ladder_probes *probes,
                               kbc_rs_cred *out, kbc_err *err);

/* The real probes: `gh` run with a scrubbed environment, a token file, and
 * a scrubbed `ls-remote`. `gh_prog` is the binary to run; "gh" resolves it
 * from PATH. `git` may be NULL for a token-file-only deployment. */
kbc_status kbc_rs_ladder_probes_live(const char *gh_prog, void *user,
                                     kbc_rs_ladder_probes *out);

/* `git_dir` = the member clone's absolute common dir. Reads the file the
 * same-descriptor way (open O_NOFOLLOW|O_CLOEXEC, then fstat), and refuses
 * anything not owned by us or not owner-only. */
kbc_status kbc_rs_read_token_file(const char *path, const char *username,
                                  const kbc_rs_url *url, kbc_rs_secret *out,
                                  kbc_rs_class *cls, kbc_err *err);

/* ------------------------------------------------------------------- git -- */

/* git_home must be ABSOLUTE; it is created 0700 and is the HOME /
 * XDG_CONFIG_HOME of every scrubbed call. The only file kb keeps there is
 * its own gitconfig. `path` is the daemon's PATH; empty and relative
 * entries are dropped (a relative entry resolves against the child's cwd).
 * If `path` is NULL, PATH is omitted entirely and `resolved_git` returns
 * false — a spawn then fails loudly rather than running a bare "git". */
typedef struct kbc_rs_git kbc_rs_git;

kbc_status kbc_rs_git_new(const char *git_home, const char *path,
                          kbc_rs_git **out, kbc_err *err);
void kbc_rs_git_free(kbc_rs_git *g);
const char *kbc_rs_git_home(const kbc_rs_git *g);
/* The absolute path of the git a scrubbed call will execute. */
bool kbc_rs_git_resolved(kbc_rs_git *g, char *out, size_t cap);
/* Trust a local source repository owned by another uid. Adds exactly this
 * path (never "*") to safe.directory in kb's own global config. */
kbc_status kbc_rs_git_allow_local_source(kbc_rs_git *g, const char *abs_path,
                                         kbc_err *err);

/* One invocation. Every field is validated by the type or the parser that
 * produced it; `kbc_rs_git_run` adds the `-c` hardening, the environment,
 * the deadline, the process group, and the redaction. */
typedef struct {
  const char *op;              /* names the call in errors and logs */
  const char *const *argv;     /* NUL-terminated; argv[0] is the subcommand */
  size_t argc;
  const char *git_dir;         /* sets GIT_DIR; NULL unsets it */
  kbc_rs_auth_kind auth;
  const kbc_rs_cred *cred;     /* required for KBC_RS_AUTH_TOKEN */
  const char *stdin_bytes;     /* may contain NULs */
  size_t stdin_len;
  size_t stdout_cap;
  unsigned timeout_s;
  bool allow_nonzero;
} kbc_rs_call;

typedef struct {
  int exit_code; /* -1 when the child was killed or never reported one */
  bool timed_out;
  bool stdout_truncated;
  bool stderr_truncated;
  kbc_str stdout; /* raw bytes; call kbc_rs_redact_bytes before logging */
  kbc_str stderr; /* ALREADY redacted with this call's own secret */
} kbc_rs_output;

void kbc_rs_output_init(kbc_rs_output *o);
void kbc_rs_output_free(kbc_rs_output *o);

/* Runs one call. A `push`/`send-pack`/`receive-pack` argv is REFUSED here,
 * not merely discouraged. On failure `err` carries op, class slug and the
 * redacted, capped stderr; the token cannot be in it. */
kbc_status kbc_rs_git_run(kbc_rs_git *g, const kbc_rs_call *call,
                          kbc_rs_output *out, kbc_err *err);

/* The high-level operations. Each is argv-composed from validated atoms
 * only; none of them can be talked into quoting a shell. */
kbc_status kbc_rs_git_init_bare(kbc_rs_git *g, const char *abs_dir,
                                kbc_err *err);
kbc_status kbc_rs_git_config_remote(kbc_rs_git *g, const char *git_dir,
                                    const char *remote_name,
                                    const kbc_rs_url *url, kbc_err *err);
/* `git for-each-ref` over `prefixes` (NULL-terminated, may be empty).
 * Each out[i] is "<oid> <refname>". */
kbc_status kbc_rs_git_list_refs(kbc_rs_git *g, const char *git_dir,
                                const char *const *prefixes,
                                kbc_strlist *out, kbc_err *err);
/* One `update-ref --stdin` transaction from `tx`. Used by GC's apply and by
 * the ref-recreate path; the caller composes the transaction from refs it
 * validated. */
kbc_status kbc_rs_git_update_refs(kbc_rs_git *g, const char *git_dir,
                                  const char *tx, size_t tx_len,
                                  kbc_err *err);

/* -------------------------------------------------------------- manifest -- */

#define KBC_RS_MANIFEST_NAME "kb-code-store.json"
#define KBC_RS_MANIFEST_SCHEMA "kb-code-store/1"

typedef struct {
  char schema[32];
  uint32_t format;
  char uuid[KBC_RS_UUID_MAX];
  char store_key[KBC_RS_KEY_MAX];
  int64_t created_at; /* unix seconds */
} kbc_rs_manifest;

kbc_status kbc_rs_manifest_write(const char *dir, const char *uuid,
                                 const char *store_key, int64_t created_at,
                                 kbc_err *err);
/* KBC_ERR_NOTFOUND when the directory or the file is missing,
 * KBC_ERR_CONFLICT when schema/uuid/store_key disagree, KBC_ERR_PARSE when
 * the file is unreadable or malformed. The manifest carries no secret and
 * no path. */
kbc_status kbc_rs_manifest_check(const char *dir, const char *uuid,
                                 const char *store_key,
                                 kbc_rs_manifest *out, kbc_err *err);

/* An exclusive, non-blocking flock(2) on <root>/<uuid>.lock, held until
 * released. A second holder is REFUSED, not queued: two seeds writing one
 * manifest is how a mirror ends up describing a repo that does not exist.
 * On refusal `err` names the holder pid and the lock path. */
typedef struct kbc_rs_lock kbc_rs_lock;

kbc_status kbc_rs_lock_acquire(const char *root, const char *uuid,
                               kbc_rs_lock **out, kbc_err *err);
void kbc_rs_lock_release(kbc_rs_lock *l);
pid_t kbc_rs_lock_holder(const kbc_rs_lock *l);

/* ------------------------------------------------------------------ seed -- */

typedef struct {
  const char *root;         /* absolute; created if missing */
  const char *uuid;
  const char *store_key;
  const kbc_rs_url *base;   /* the remote to mirror; NULL = offline seed */
  const kbc_rs_cred *cred;  /* NULL or profile none = no base fetch */
  unsigned timeout_s;       /* 0 = KBC_RS_SEED_TIMEOUT_S */
  /* Refs under these prefixes are enumerated from the remote and fetched
   * one by one. No wildcard refspec is ever built. NULL-terminated; may be
   * NULL, meaning the store's own default pair. */
  const char *const *ref_prefixes;
} kbc_rs_seed_plan;

#define KBC_RS_SEED_TIMEOUT_S 1800u

typedef enum {
  KBC_RS_BASE_OFFLINE = 0, /* no base remote configured */
  KBC_RS_BASE_FETCHED,
  KBC_RS_BASE_NO_CREDENTIAL,
  KBC_RS_BASE_FAILED
} kbc_rs_base_state;

typedef struct {
  char git_dir[KBC_RS_PATH_MAX];
  size_t refs_imported;
  size_t refs_skipped;
  kbc_rs_base_state base_state;
  char base_detail[KBC_RS_DETAIL_MAX];
  int64_t elapsed_ms;
} kbc_rs_seed_report;

/* Holds the store lock for the whole call: the caller does NOT take it.
 * Clones into <root>/.seed-<uuid>.tmp, writes the manifest, and renames to
 * <root>/<uuid>.git. ANY failure before the rename removes the .tmp
 * immediately, so a failure can never leave a half-mirror under the final
 * name. */
kbc_status kbc_rs_seed(kbc_rs_git *g, const kbc_rs_seed_plan *plan,
                       kbc_rs_seed_report *out, kbc_err *err);

/* ----------------------------------------------------------- base ladder -- */

/* `host/owner/name` of an existing store, used by the membership rung. */
typedef struct {
  const char *store_key;
  const char *member_store_key; /* NULL for a store with no members yet */
} kbc_rs_known_store;

typedef struct {
  const char *name;       /* remote name */
  const char *url;        /* as written; may carry userinfo, never echoed */
  const char *gh_resolved;/* "base" when gh repo set-default chose it */
} kbc_rs_remote_info;

typedef struct {
  const char *explicit_url;   /* operator's store set-base-url */
  const char *config_url;     /* [[review.repos]] base_url */
  const char *pr_slug;        /* "owner/name" recorded from a PR binding */
  const kbc_rs_remote_info *remotes;
  size_t n_remotes;
  const kbc_rs_known_store *existing;
  size_t n_existing;
} kbc_rs_base_input;

typedef enum {
  KBC_RS_BASE_EXPLICIT = 0,
  KBC_RS_BASE_CONFIG,
  KBC_RS_BASE_MEMBER,
  KBC_RS_BASE_PR_SLUG,
  KBC_RS_BASE_GH_RESOLVED,
  KBC_RS_BASE_SINGLE,
  KBC_RS_BASE_LOCAL, /* no forge remote at all */
  KBC_RS_BASE_AMBIGUOUS
} kbc_rs_base_source;

typedef struct {
  kbc_rs_base_source source;
  char store_key[KBC_RS_KEY_MAX];
  char remote[64];
  /* A remote REFUSED as unsafe is never silently dropped. When another
   * remote answered, the store is keyed from that one and the refusal still
   * rides out here; when none did, the ladder refuses with it. */
  char refused_reason[KBC_RS_DETAIL_MAX];
  int refused_count;
} kbc_rs_base_ladder;

/* Pure: no git, no network, no I/O. KBC_ERR_CONFLICT for base-url-ambiguous
 * and for remote-url-refused. */
kbc_status kbc_rs_base_ladder_run(const kbc_rs_base_input *in,
                                  kbc_rs_base_ladder *out, kbc_err *err);
const char *kbc_rs_base_source_slug(kbc_rs_base_source s);

/* -------------------------------------------------------------------- gc -- */

/* The DB half of the keep-set, supplied by the caller. kb-c has no
 * review_stores table, so the boundary is this struct: whoever knows which
 * reviews exist and which repos are registered members says so, and the GC
 * never guesses. An EMPTY review_ids set therefore means "delete every
 * refs/kbc/review ref", which is why apply requires a bundle first. */
typedef struct {
  const int64_t *review_ids;
  size_t n_review_ids;
  const int64_t *open_pr_numbers;
  size_t n_open_pr_numbers;
  const int64_t *member_ids;
  size_t n_member_ids;
} kbc_rs_gc_keep;

typedef struct {
  char refname[KBC_RS_REFNAME_MAX];
  char old_oid[KBC_RS_OID_MAX];
} kbc_rs_gc_candidate;

typedef enum {
  KBC_RS_REF_UNCLASSIFIED = 0,
  KBC_RS_REF_PATCHSET,
  KBC_RS_REF_PATCHSET_BASE,
  KBC_RS_REF_PR,
  KBC_RS_REF_PRM,
  KBC_RS_REF_HINT,
  KBC_RS_REF_WORK
} kbc_rs_ref_kind;

typedef struct {
  kbc_rs_ref_kind kind;
  int64_t id;      /* review id for patchset/pr/prm, member id for hint/work */
  bool bound;      /* the keep-set says it is live */
  const char *branch; /* for kind == WORK, else NULL */
} kbc_rs_ref_class;

/* Pure: classify one refname. A name this cannot parse is UNCLASSIFIED and
 * can therefore never become a delete candidate. */
kbc_rs_ref_class kbc_rs_gc_classify_ref(const char *refname,
                                        const kbc_rs_gc_keep *keep);

typedef struct {
  kbc_rs_gc_candidate *cands; /* KBC_OWN */
  size_t n;
  size_t n_scanned, n_bound, n_orphan, n_unclassified;
  size_t covered_by_bundle; /* 0 unless apply ran */
  char bundle[KBC_RS_PATH_MAX];
  bool applied;
} kbc_rs_gc_report;

void kbc_rs_gc_report_init(kbc_rs_gc_report *r);
void kbc_rs_gc_report_free(kbc_rs_gc_report *r);

/* One store-wide pass. `apply == false` is a pure dry run: it classifies and
 * fills `out` and touches nothing. `apply == true` writes a pre-apply
 * bundle covering EVERY candidate, then one old-value-guarded
 * `update-ref --stdin` transaction. `guard_path` (may be NULL) is the
 * restore-guard sentinel: a flagged store is refused unless `uuid` is in
 * its acknowledged set, and the refusal names the flag reason. */
kbc_status kbc_rs_gc_run(kbc_rs_git *g, const char *git_dir,
                         const kbc_rs_gc_keep *keep, const char *bundle_dir,
                         const char *guard_path, const char *uuid, bool apply,
                         kbc_rs_gc_report *out, kbc_err *err);

/* The scheduled cadences. Each is one hardened git call with its own
 * deadline; none of them is reachable from a seeded store without a caller. */
kbc_status kbc_rs_gc_daily(kbc_rs_git *g, const char *git_dir, size_t *swept,
                           kbc_err *err);
kbc_status kbc_rs_gc_weekly(kbc_rs_git *g, const char *git_dir, kbc_err *err);
/* `allow_expire` is the DATA-LOSS decision and is deliberately NOT taken
 * here: a packed object keeps its PACK's mtime, so an expiring cruft repack
 * run soon after a GC apply can prune objects that same apply orphaned. The
 * caller computes it from the cooldown below. */
kbc_status kbc_rs_gc_monthly(kbc_rs_git *g, const char *git_dir,
                             bool allow_expire, kbc_err *err);
#define KBC_RS_CRUFT_COOLDOWN_SECS (14LL * 24LL * 3600LL)
/* Routine backup shape: every ref under refs/kbc/ as a head, every ref under
 * refs/remotes/base/ as an exclusion (those objects are re-fetchable, so a
 * bundle need not carry them). KBC_ERR_NOTFOUND when the store has no
 * refs/kbc refs at all — git refuses an empty bundle and an empty bundle is
 * not a useful backup. A "<dest>.refs" manifest of "<oid>\t<refname>" is
 * always written beside it, so a restore can be checked against what the
 * bundle claimed to carry. */
kbc_status kbc_rs_backup_write(kbc_rs_git *g, const char *git_dir,
                               const char *dest, kbc_err *err);
/* The wider shape: every refs/kbc head PLUS every (oid, refname) in
 * `cover` — the refs the apply is about to delete, whatever namespace they
 * are in. The invariant this maintains: no ref is ever deleted by an apply
 * whose pre-apply bundle did not cover it. */
kbc_status kbc_rs_backup_write_covering(kbc_rs_git *g, const char *git_dir,
                                        const char *dest,
                                        const kbc_rs_gc_candidate *cover,
                                        size_t n_cover, kbc_err *err);
/* Keep the newest `keep` store-<uuid>-*.bundle files, removing the rest. */
kbc_status kbc_rs_backup_prune(const char *backups_dir, const char *uuid,
                               size_t keep, kbc_err *err);
/* The refs a bundle claims to carry (`git bundle list-heads`). */
kbc_status kbc_rs_bundle_list_heads(kbc_rs_git *g, const char *bundle,
                                    kbc_strlist *out, kbc_err *err);

typedef struct {
  char refname[KBC_RS_REFNAME_MAX];
  char old_oid[KBC_RS_OID_MAX];
  char new_oid[KBC_RS_OID_MAX];
} kbc_rs_restore_collision;

typedef struct {
  kbc_rs_restore_collision *collisions; /* KBC_OWN */
  size_t n;
  size_t n_restored;
  char git_dir[KBC_RS_PATH_MAX];
} kbc_rs_restore_report;

void kbc_rs_restore_report_init(kbc_rs_restore_report *r);
void kbc_rs_restore_report_free(kbc_rs_restore_report *r);

/* Restore a bundle into a bare mirror at `git_dir`.
 *
 * THE GUARD: before anything is written, the bundle's own heads are read
 * and compared against the refs already at `git_dir`. If any ref the
 * restore would write already exists AT A DIFFERENT OBJECT, the restore is
 * REFUSED — KBC_ERR_CONFLICT — and the error names every ref and both
 * object ids, because "your restore was refused" without the list of what
 * it would have overwritten is an unusable answer. Refs that already hold
 * the SAME object are not collisions; they are the round-trip's success
 * case. Pass force = true to apply anyway, which is then a deliberate
 * clobber the caller has asked for. */
kbc_status kbc_rs_restore(kbc_rs_git *g, const char *bundle,
                          const char *git_dir, bool force,
                          kbc_rs_restore_report *out, kbc_err *err);

/* --------------------------------------------------------- restore guard -- */

/* A sentinel beside the daemon's state file, NOT under the store root: a
 * restore of the database volume alone can never also roll this file back,
 * and that asymmetry is what the epoch detector depends on. It fails CLOSED
 * — a corrupt or unreadable-but-present sentinel reads as FLAGGED, never as
 * "never flagged". */
#define KBC_RS_GUARD_SCHEMA "kbc-restore-guard/1"
#define KBC_RS_GUARD_FILE "review-store-restore-guard.json"
#define KBC_RS_GUARD_MAX_ACK 8

typedef struct {
  char schema[40];
  bool has_epoch;
  int64_t high_water_epoch;
  bool flagged;
  int64_t flagged_at;
  char reason[KBC_RS_DETAIL_MAX];
  char acknowledged[KBC_RS_GUARD_MAX_ACK][KBC_RS_UUID_MAX];
  size_t n_ack;
  bool just_flagged; /* true only on the call that JUST set it */
} kbc_rs_guard;

void kbc_rs_guard_init(kbc_rs_guard *g);
kbc_status kbc_rs_guard_read(const char *path, kbc_rs_guard *out,
                             kbc_err *err);
kbc_status kbc_rs_guard_write(const char *path, const kbc_rs_guard *g,
                              kbc_err *err);
/* Compare the volume's epoch at this boot against the highest ever
 * observed. A regression is the only known cause of which a restore is the
 * only known explanation, so it flags; a higher epoch bumps the mark and
 * never lowers it. A NEW flagging event clears the acknowledgements. */
kbc_status kbc_rs_guard_observe_epoch(const char *path, bool has_epoch,
                                      int64_t epoch, int64_t now,
                                      kbc_rs_guard *out, kbc_err *err);
kbc_status kbc_rs_guard_acknowledge(const char *path, const char *uuid,
                                    int64_t now, kbc_err *err);
bool kbc_rs_guard_blocks(const kbc_rs_guard *g, const char *uuid);

#ifdef __cplusplus
}
#endif

#endif /* KBC_RSTORE_H */
