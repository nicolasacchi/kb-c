/* rstore.c — the review-store mirror manager. See include/kbc/rstore.h for
 * the contract and for the secret path, which is the security property this
 * file exists to hold.
 *
 * The shape of the spawner, stated once so the rest reads as consequences:
 *
 *   - one hardened runner, and it is the only place a child process is
 *     created in this file;
 *   - env_clear plus an explicit allowlist, so nothing ambient reaches git
 *     except the proxy/CA passthrough the operator configured;
 *   - `-c` hardening in argv, including a credential.helper reset that
 *     makes an inherited helper impossible;
 *   - the child gets its OWN process group, and the group is SIGKILLed on
 *     every exit path. The exit is observed with waitid(WNOWAIT) so the
 *     leader is still a ZOMBIE — holding its pid, and therefore its group
 *     id — at the moment of the kill, so the signal can never land on a
 *     recycled group;
 *   - a deadline enforced on the whole group, because a hung
 *     git-remote-https must not outlive the call that spawned it;
 *   - bounded capture, drained and discarded past the cap so a chatty child
 *     can neither block on a full pipe nor grow daemon memory.
 */
#define _GNU_SOURCE /* WNOWAIT, pipe2, F_DUPFD_CLOEXEC */

#include "kbc/rstore.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <strings.h>
#include <stdlib.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <string.h>
#include <time.h>
#include <sys/wait.h>
#include <unistd.h>

#include "kbc/json.h"
#include "kbc/log.h"
#include "kbc/mem.h"

/* Propagate a fallible call out of the current function with `err` intact.
 * Every string append in this file goes through one of these; rule 6 is
 * that a -Wunused-result warning is a defect. */
#define RS_TRY(expr)                                                          \
  do {                                                                        \
    kbc_status rs_s_ = (expr);                                                \
    if (rs_s_ != KBC_OK)                                                      \
      return rs_s_;                                                           \
  } while (0)

/* ------------------------------------------------------------------ misc -- */

/* Wipe without the compiler eliding the store. A plain memset over a soon-to-
 * be-freed secret is exactly the store the optimizer is allowed to drop. */
static void wipe(void *p, size_t n) {
  volatile unsigned char *v = (volatile unsigned char *)p;
  while (n-- > 0) {
    *v++ = 0;
  }
}

static bool ci_eq(char a, char b) {
  return (char)tolower((unsigned char)a) == (char)tolower((unsigned char)b);
}

/* Case-insensitive match of the lowercase literal `lit` at s[i..n). */
static bool ci_at(const char *s, size_t n, size_t i, const char *lit) {
  size_t l = strlen(lit);
  if (i + l > n) {
    return false;
  }
  for (size_t k = 0; k < l; k++) {
    if (!ci_eq(s[i + k], lit[k])) {
      return false;
    }
  }
  return true;
}

static bool word_before(const char *s, size_t i) {
  if (i == 0) {
    return true;
  }
  unsigned char c = (unsigned char)s[i - 1];
  return !isalnum(c) && c != '_';
}

static bool is_lower_alpha(char c) {
  return (c >= 'a' && c <= 'z');
}

static bool is_digit_str(const char *s) {
  if (*s == '\0') {
    return false;
  }
  for (const char *p = s; *p != '\0'; p++) {
    if (*p < '0' || *p > '9') {
      return false;
    }
  }
  return true;
}

/* Strict decimal id: no leading zeros, no sign, no empty. git's own ref
 * grammar and this store's ref names both require the canonical spelling;
 * "work-07" and "work-7" are two refs, and only one of them is the member
 * the keep-set means. */
static bool parse_strict_i64(const char *s, int64_t *out) {
  if (!is_digit_str(s) || (s[0] == '0' && s[1] != '\0')) {
    return false;
  }
  if (strlen(s) > 18) {
    return false;
  }
  long long v = 0;
  for (const char *p = s; *p != '\0'; p++) {
    v = v * 10 + (*p - '0');
  }
  if (v < 1) {
    return false;
  }
  *out = (int64_t)v;
  return true;
}

static bool oid_ok(const char *hex) {
  size_t n = strlen(hex);
  if (n != 40 && n != 64) {
    return false;
  }
  for (size_t i = 0; i < n; i++) {
    if (!isxdigit((unsigned char)hex[i]) || isupper((unsigned char)hex[i])) {
      return false;
    }
  }
  return true;
}

/* A full ref name, in the narrower sense the store's own refspecs need: a
 * revspec predicate PLUS git's check-ref-format characters, which are legal
 * in a revspec but not in a ref name. Every name that reaches argv from
 * disk (ls-remote output, for-each-ref output, a bundle's own header) goes
 * through here first. */
static bool ref_name_ok(const char *s) {
  if (s == NULL || *s == '\0') {
    return false;
  }
  if (s[0] == '-') {
    return false;
  }
  size_t n = strlen(s);
  if (n >= KBC_RS_REFNAME_MAX) {
    return false;
  }
  if (strncmp(s, "refs/", 5) != 0 || n <= 5) {
    return false;
  }
  if (strstr(s, "..") != NULL || strstr(s, "@{") != NULL ||
      strstr(s, "//") != NULL) {
    return false;
  }
  for (size_t i = 0; i < n; i++) {
    unsigned char c = (unsigned char)s[i];
    if (c < 0x20 || c == 0x7f || c == ' ' || c == '\t') {
      return false;
    }
    if (strchr("\\:?*[~^", (char)c) != NULL) {
      return false;
    }
  }
  if (strchr(s, '\\') != NULL) {
    return false;
  }
  size_t last = n - 1;
  if (s[last] == '/' || s[last] == '.') {
    return false;
  }
  if (n >= 5 && strcmp(s + n - 5, ".lock") == 0) {
    return false;
  }
  /* No empty or dot-prefixed path component. */
  const char *p = s;
  while (p != NULL && *p != '\0') {
    const char *slash = strchr(p, '/');
    size_t seg = (slash != NULL) ? (size_t)(slash - p) : strlen(p);
    /* A component starting with '-' is option-shaped, and a refspec source
     * that is option-shaped is a way to make git do something else. */
    if (seg == 0 || p[0] == '.' || p[0] == '-') {
      return false;
    }
    p = (slash != NULL) ? slash + 1 : NULL;
  }
  return true;
}

static kbc_status rm_rf(const char *path, kbc_err *err) {
  struct stat st;
  if (lstat(path, &st) != 0) {
    return (errno == ENOENT) ? KBC_OK : kbc_err_set(err, KBC_ERR_IO,
                                                    "lstat %s: %s", path,
                                                    strerror(errno));
  }
  if (!S_ISDIR(st.st_mode)) {
    if (unlink(path) != 0) {
      return kbc_err_set(err, KBC_ERR_IO, "unlink %s: %s", path,
                         strerror(errno));
    }
    return KBC_OK;
  }
  DIR *d = opendir(path);
  if (d == NULL) {
    return kbc_err_set(err, KBC_ERR_IO, "opendir %s: %s", path,
                       strerror(errno));
  }
  kbc_status st_rc = KBC_OK;
  struct dirent *e;
  while ((e = readdir(d)) != NULL) {
    if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0) {
      continue;
    }
    char child[KBC_RS_PATH_MAX];
    if (snprintf(child, sizeof child, "%s/%s", path, e->d_name) >=
        (int)sizeof child) {
      st_rc = kbc_err_set(err, KBC_ERR_IO, "path too long under %s", path);
      break;
    }
    st_rc = rm_rf(child, err);
    if (st_rc != KBC_OK) {
      break;
    }
  }
  closedir(d);
  if (st_rc != KBC_OK) {
    return st_rc;
  }
  if (rmdir(path) != 0) {
    return kbc_err_set(err, KBC_ERR_IO, "rmdir %s: %s", path,
                       strerror(errno));
  }
  return KBC_OK;
}

static kbc_status write_file_sync(const char *path, const char *data, size_t n,
                                  kbc_err *err) {
  int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
  if (fd < 0) {
    return kbc_err_set(err, KBC_ERR_IO, "open %s: %s", path, strerror(errno));
  }
  size_t off = 0;
  while (off < n) {
    ssize_t w = write(fd, data + off, n - off);
    if (w < 0) {
      if (errno == EINTR) {
        continue;
      }
      kbc_status s = kbc_err_set(err, KBC_ERR_IO, "write %s: %s", path,
                                 strerror(errno));
      close(fd);
      return s;
    }
    off += (size_t)w;
  }
  if (fsync(fd) != 0) {
    kbc_status s = kbc_err_set(err, KBC_ERR_IO, "fsync %s: %s", path,
                               strerror(errno));
    close(fd);
    return s;
  }
  close(fd);
  return KBC_OK;
}

/* ----------------------------------------------------------------- class -- */

static const struct {
  kbc_rs_class c;
  const char *slug;
} CLASS_TABLE[] = {
    {KBC_RS_CLASS_VANISHED, "vanished"},
    {KBC_RS_CLASS_OFFLINE, "offline"},
    {KBC_RS_CLASS_TIMEOUT, "timeout"},
    {KBC_RS_CLASS_CREDENTIAL_REJECTED, "credential-rejected"},
    {KBC_RS_CLASS_CREDENTIAL_WRONG_REPO, "credential-wrong-repo"},
    {KBC_RS_CLASS_REPO_NOT_FOUND, "repo-not-found"},
    {KBC_RS_CLASS_AUTH_NO_ACCESS, "auth-no-access"},
    {KBC_RS_CLASS_HOST_KEY_UNKNOWN, "host-key-unknown"},
    {KBC_RS_CLASS_HOST_KEY_MISMATCH, "host-key-mismatch"},
    {KBC_RS_CLASS_AUTH_REQUIRED, "auth-required"},
    {KBC_RS_CLASS_TLS, "tls"},
    {KBC_RS_CLASS_DISK_FULL, "disk-full"},
    {KBC_RS_CLASS_PROTOCOL_REFUSED, "protocol-refused"},
    {KBC_RS_CLASS_URL_REJECTED, "url-rejected"},
    {KBC_RS_CLASS_CREDENTIAL_ACCOUNT_MISMATCH,
     "credential-account-mismatch"},
    {KBC_RS_CLASS_CREDENTIAL_UNAVAILABLE, "credential-unavailable"},
    {KBC_RS_CLASS_NO_CREDENTIALS, "no-credentials"},
    {KBC_RS_CLASS_SPAWN_FAILED, "spawn-failed"},
    {KBC_RS_CLASS_FAILED, "failed"},
};

const char *kbc_rs_class_slug(kbc_rs_class c) {
  for (size_t i = 0; i < sizeof CLASS_TABLE / sizeof CLASS_TABLE[0]; i++) {
    if (CLASS_TABLE[i].c == c) {
      return CLASS_TABLE[i].slug;
    }
  }
  return "failed";
}

kbc_rs_class kbc_rs_class_from_slug(const char *slug) {
  if (slug == NULL) {
    return KBC_RS_CLASS_FAILED;
  }
  for (size_t i = 0; i < sizeof CLASS_TABLE / sizeof CLASS_TABLE[0]; i++) {
    if (strcmp(CLASS_TABLE[i].slug, slug) == 0) {
      return CLASS_TABLE[i].c;
    }
  }
  return KBC_RS_CLASS_FAILED;
}

/* Exact TLS phrases only. A bare "tls"/"ssl" substring would classify a repo
 * literally named `openssl-tls` as a certificate failure. */
static const char *const TLS_PHRASES[] = {
    "ssl certificate",         "ssl: certificate",
    "certificate problem",     "certificate verify failed",
    "certificate has expired", "unable to get local issuer certificate",
    "self-signed certificate", "self signed certificate",
    "server certificate verification failed",
    "ssl_connect",             "ssl connect error",
    "ssl routines",            "gnutls_handshake",
    "gnutls recv error",       "tls handshake",
    "tlsv1 alert",             "schannel:",
    "openssl ssl_read",
};

static bool has(const char *hay, const char *needle) {
  return strstr(hay, needle) != NULL;
}

kbc_rs_class kbc_rs_classify(const char *stderr, size_t n, bool authed) {
  if (stderr == NULL || n == 0) {
    return KBC_RS_CLASS_FAILED;
  }
  /* Quoted URLs and paths are user text: a repository named `openssl-tls`
   * must not classify as `tls`. Replace every quoted span with a
   * placeholder before any phrase is matched. */
  kbc_str clean;
  kbc_str_init(&clean);
  bool clean_ok = true;
  for (size_t i = 0; i < n && clean_ok;) {
    if (stderr[i] != '\'') {
      clean_ok = kbc_str_putc(&clean, (char)tolower((unsigned char)stderr[i])) ==
                 KBC_OK;
      i++;
      continue;
    }
    size_t j = i + 1;
    bool is_urlish = j < n &&
                     (is_lower_alpha(stderr[j]) || stderr[j] == '/' ||
                      stderr[j] == '~');
    if (!is_urlish) {
      clean_ok = kbc_str_putc(&clean, '\'') == KBC_OK;
      i++;
      continue;
    }
    while (j < n && stderr[j] != '\'') {
      j++;
    }
    clean_ok = kbc_str_puts(&clean, "'...'") == KBC_OK;
    i = (j < n) ? j + 1 : n;
  }
  if (!clean_ok) {
    kbc_str_free(&clean);
    return KBC_RS_CLASS_FAILED;
  }
  const char *s = clean.ptr;
  kbc_rs_class cls;
  /* Most specific first: an ssh host-key mismatch ALSO prints "host key
   * verification failed", and a disk-full fetch also prints a generic
   * "fatal:". */
  if (has(s, "remote host identification has changed")) {
    cls = KBC_RS_CLASS_HOST_KEY_MISMATCH;
  } else if (has(s, "no space left on device") || has(s, "disk quota exceeded")) {
    cls = KBC_RS_CLASS_DISK_FULL;
  } else {
    bool tls = false;
    for (size_t i = 0; i < sizeof TLS_PHRASES / sizeof TLS_PHRASES[0]; i++) {
      if (has(s, TLS_PHRASES[i])) {
        tls = true;
        break;
      }
    }
    if (tls) {
      cls = KBC_RS_CLASS_TLS;
    } else if (has(s, "could not resolve host") ||
               has(s, "connection refused") ||
               has(s, "network is unreachable") ||
               has(s, "failed to connect to") ||
               has(s, "temporary failure in name resolution")) {
      cls = KBC_RS_CLASS_OFFLINE;
    } else if (has(s, "transport '") && has(s, "' not allowed")) {
      cls = KBC_RS_CLASS_PROTOCOL_REFUSED;
    } else if (has(s, "host key verification failed") ||
               has(s, "no matching host key") ||
               has(s, "known_hosts")) {
      cls = KBC_RS_CLASS_HOST_KEY_UNKNOWN;
    } else if (has(s, "couldn't find remote ref") ||
               has(s, "could not find remote ref") ||
               has(s, "does not appear to be a git repository")) {
      cls = KBC_RS_CLASS_VANISHED;
    } else if (authed && (has(s, "authentication failed") ||
                         has(s, "invalid username or password") ||
                         has(s, "http basic: access denied") ||
                         has(s, "403 forbidden") ||
                         has(s, "401 unauthorized"))) {
      cls = KBC_RS_CLASS_CREDENTIAL_REJECTED;
    } else if (authed && has(s, "not found")) {
      /* GitHub's answer for "this account cannot see it" — auth-shaped, not
       * a missing repository. */
      cls = KBC_RS_CLASS_AUTH_NO_ACCESS;
    } else if (!authed && (has(s, "could not read username") ||
                           has(s, "terminal prompts disabled") ||
                           has(s, "authentication failed") ||
                           has(s, "permission denied (publickey)") ||
                           has(s, "access denied"))) {
      cls = KBC_RS_CLASS_AUTH_REQUIRED;
    } else if (!authed && has(s, "not found")) {
      cls = KBC_RS_CLASS_REPO_NOT_FOUND;
    } else if (has(s, "shallow")) {
      cls = KBC_RS_CLASS_VANISHED; /* a shallow constraint, kept distinct in
                                      the detail string by the caller */
    } else {
      cls = KBC_RS_CLASS_FAILED;
    }
  }
  kbc_str_free(&clean);
  return cls;
}

/* --------------------------------------------------------------- redact -- */

/* One redaction pass. Each rule rewrites the whole buffer; they are applied
 * in the order below, and the order matters: the header rule runs first
 * because an Authorization header's value is secret whatever its scheme,
 * and the known-literal pass runs before all of them so a secret with no
 * recognizable shape (a GHE or Gitea token) is already gone by the time a
 * shape rule could mangle the text around it. */

static bool tok_run(const char *s, size_t n, size_t *j, const char *extra) {
  size_t start = *j;
  while (*j < n) {
    char c = s[*j];
    if (isalnum((unsigned char)c) || strchr(extra, c) != NULL) {
      (*j)++;
    } else {
      break;
    }
  }
  return *j > start;
}

/* `\b((?:proxy-)?authorization)\s*:[^\r\n]*` -> "<name>: [redacted]" */
static size_t rule_header(const char *s, size_t n, size_t i, kbc_str *out) {
  size_t start = i;
  if (ci_at(s, n, i, "proxy-authorization")) {
    i += strlen("proxy-authorization");
  } else if (ci_at(s, n, i, "authorization") && word_before(s, i)) {
    i += strlen("authorization");
  } else {
    return i;
  }
  if (!word_before(s, start)) {
    return start;
  }
  size_t j = i;
  while (j < n && (s[j] == ' ' || s[j] == '\t')) {
    j++;
  }
  if (j >= n || s[j] != ':') {
    return start;
  }
  RS_TRY(kbc_str_append(out, s + start, j - start));
  RS_TRY(kbc_str_puts(out, KBC_RS_REDACTED));
  while (j < n && s[j] != '\n' && s[j] != '\r') {
    j++;
  }
  return j;
}

/* `\b([a-z][a-z0-9+.\-]*://)[^/@\s'`]+@` -> "<scheme>://[redacted]@" */
static size_t rule_userinfo(const char *s, size_t n, size_t i, kbc_str *out) {
  /* The scheme is case-insensitive, so HTTPS:// is the same shape. */
  if (!word_before(s, i) || !isalpha((unsigned char)s[i])) {
    return i;
  }
  size_t j = i + 1;
  while (j < n && (isalpha((unsigned char)s[j]) || isdigit((unsigned char)s[j]) ||
                   s[j] == '+' || s[j] == '.' || s[j] == '-')) {
    j++;
  }
  if (j + 2 >= n || s[j] != ':' || s[j + 1] != '/' || s[j + 2] != '/') {
    return i;
  }
  size_t at = j + 3;
  size_t k = at;
  while (k < n && s[k] != '/' && s[k] != '@' && s[k] != ' ' &&
         s[k] != '\t' && s[k] != '\n' && s[k] != '\r' && s[k] != '\'' &&
         s[k] != '`' && s[k] != '\\') {
    k++;
  }
  if (k == at || k >= n || s[k] != '@') {
    return i;
  }
  RS_TRY(kbc_str_append(out, s + i, (j + 3) - i));
  RS_TRY(kbc_str_puts(out, KBC_RS_REDACTED "@"));
  return k + 1;
}

/* `<prefix><run of at least min_run token chars>`. The minimum lengths are
 * what keep a 40-hex object id readable: an object id is a secret-shaped
 * string that is NOT a secret, and redacting it would destroy every useful
 * git error. */
static size_t rule_prefixed(const char *s, size_t n, size_t i, kbc_str *out,
                            const char *prefix, size_t min_run,
                            const char *extra) {
  if (!word_before(s, i) || !ci_at(s, n, i, prefix)) {
    return i;
  }
  size_t j = i + strlen(prefix);
  size_t start = j;
  if (!tok_run(s, n, &j, extra) || (j - start) < min_run) {
    return i;
  }
  RS_TRY(kbc_str_puts(out, KBC_RS_REDACTED));
  return j;
}

static size_t rule_gh(const char *s, size_t n, size_t i, kbc_str *out) {
  static const char *const shapes[] = {"ghp_", "gho_", "ghs_", "ghu_", "ghr_"};
  for (size_t k = 0; k < 5; k++) {
    size_t j = rule_prefixed(s, n, i, out, shapes[k], 16, "");
    if (j != i) {
      return j;
    }
  }
  return i;
}

static size_t rule_github_pat(const char *s, size_t n, size_t i,
                              kbc_str *out) {
  return rule_prefixed(s, n, i, out, "github_pat_", 20, "_");
}

static size_t rule_glpat(const char *s, size_t n, size_t i, kbc_str *out) {
  return rule_prefixed(s, n, i, out, "glpat-", 16, "_-");
}

/* `\b(bearer|basic|token)(\s+)[A-Za-z0-9._~+/=\-]{16,}` */
static size_t rule_scheme_cred(const char *s, size_t n, size_t i,
                               kbc_str *out) {
  static const char *const schemes[] = {"bearer", "basic", "token"};
  size_t plen = 0;
  for (size_t k = 0; k < 3; k++) {
    size_t l = strlen(schemes[k]);
    if (l > plen && ci_at(s, n, i, schemes[k])) {
      plen = l;
    }
  }
  if (plen == 0 || !word_before(s, i)) {
    return i;
  }
  size_t j = i + plen;
  size_t ws = j;
  while (j < n && (s[j] == ' ' || s[j] == '\t')) {
    j++;
  }
  if (j == ws) {
    return i;
  }
  size_t start = j;
  if (!tok_run(s, n, &j, "._~+/=-") || (j - start) < 16) {
    return i;
  }
  /* Keep the scheme and its whitespace, drop the credential run: an
   * operator reading `Authorization: [redacted]` learns more than one
   * reading an empty line. */
  RS_TRY(kbc_str_append(out, s + i, start - i));
  RS_TRY(kbc_str_puts(out, KBC_RS_REDACTED));
  return j;
}

/* `(?im)^(\s*password\s*=).*$` — the git credential protocol's own answer
 * format, which is what a helper or a `credential fill` trace prints when
 * something goes wrong. */
static size_t rule_password_line(const char *s, size_t n, size_t i,
                                 kbc_str *out) {
  if (i != 0 && s[i - 1] != '\n') {
    return i;
  }
  size_t j = i;
  while (j < n && (s[j] == ' ' || s[j] == '\t')) {
    j++;
  }
  if (!ci_at(s, n, j, "password")) {
    return i;
  }
  j += strlen("password");
  size_t k = j;
  while (k < n && (s[k] == ' ' || s[k] == '\t')) {
    k++;
  }
  if (k >= n || s[k] != '=') {
    return i;
  }
  RS_TRY(kbc_str_append(out, s + i, (k + 1) - i));
  RS_TRY(kbc_str_puts(out, KBC_RS_REDACTED));
  size_t e = k + 1;
  while (e < n && s[e] != '\n' && s[e] != '\r') {
    e++;
  }
  return e;
}

typedef size_t (*redact_rule)(const char *, size_t, size_t, kbc_str *);

static kbc_status apply_rule(const char *s, size_t n, redact_rule fn,
                             kbc_str *out) {
  kbc_str next;
  kbc_str_init(&next);
  size_t i = 0;
  while (i < n) {
    size_t j = fn(s, n, i, &next);
    if (j == i) {
      kbc_status st = kbc_str_putc(&next, s[i]);
      if (st != KBC_OK) {
        kbc_str_free(&next);
        return st;
      }
      i++;
    } else {
      i = j;
    }
  }
  kbc_str_free(out);
  *out = next;
  /* Keep the invariant that a live kbc_str always has a NUL-terminated
   * buffer: an empty capture is a real answer, and a caller that hands it to
   * strstr must not fault. */
  if (out->ptr == NULL && !kbc_str_reserve(out, 0)) {
    return KBC_ERR_NOMEM;
  }
  return KBC_OK;
}

/* Replace every occurrence of a non-empty literal. No length floor: the
 * caller put the literal in the list BECAUSE it is a secret. */
static kbc_status replace_literal(kbc_str *buf, const char *lit) {
  /* An empty buffer has a NULL ptr: nothing to search, and strstr(NULL) is
   * a crash, not a miss. */
  if (lit == NULL || *lit == '\0' || buf->ptr == NULL || buf->len == 0) {
    return KBC_OK;
  }
  size_t ll = strlen(lit);
  if (strstr(buf->ptr, lit) == NULL) {
    return KBC_OK;
  }
  kbc_str next;
  kbc_str_init(&next);
  size_t consumed = 0;
  kbc_status st = KBC_OK;
  char *hit = strstr(buf->ptr, lit);
  while (hit != NULL) {
    size_t off = (size_t)(hit - buf->ptr);
    st = kbc_str_append(&next, buf->ptr + consumed, off - consumed);
    if (st == KBC_OK) {
      st = kbc_str_puts(&next, KBC_RS_REDACTED);
    }
    if (st != KBC_OK) {
      break;
    }
    consumed = off + ll;
    hit = strstr(buf->ptr + consumed, lit);
  }
  if (st == KBC_OK) {
    st = kbc_str_append(&next, buf->ptr + consumed, buf->len - consumed);
  }
  kbc_str_free(buf);
  *buf = next;
  return st;
}

kbc_status kbc_rs_redact_with(kbc_str *out, const char *s, size_t n,
                              const kbc_rs_secrets *secrets) {
  kbc_str buf;
  kbc_str_init(&buf);
  kbc_status st = kbc_str_append(&buf, s, n);
  if (st != KBC_OK) {
    kbc_str_free(&buf);
    return st;
  }
  if (secrets != NULL) {
    for (size_t i = 0; i < secrets->len; i++) {
      st = replace_literal(&buf, secrets->items[i]);
      if (st != KBC_OK) {
        kbc_str_free(&buf);
        return st;
      }
    }
  }
  static const redact_rule rules[] = {
      rule_header, rule_userinfo,  rule_gh,
      rule_github_pat, rule_glpat, rule_scheme_cred,
      rule_password_line,
  };
  for (size_t i = 0; i < sizeof rules / sizeof rules[0]; i++) {
    st = apply_rule(buf.ptr, buf.len, rules[i], &buf);
    if (st != KBC_OK) {
      kbc_str_free(&buf);
      return st;
    }
  }
  kbc_str_free(out);
  *out = buf;
  return KBC_OK;
}

kbc_status kbc_rs_redact(kbc_str *out, const char *s, size_t n) {
  return kbc_rs_redact_with(out, s, n, NULL);
}

kbc_status kbc_rs_redact_bytes(kbc_str *out, const char *s, size_t n,
                               const kbc_rs_secrets *secrets, size_t max) {
  kbc_status st = kbc_rs_redact_with(out, s, n, secrets);
  if (st != KBC_OK) {
    return st;
  }
  if (out->len <= max) {
    return KBC_OK;
  }
  size_t cut = max;
  /* Back off to a UTF-8 boundary: a cap that splits a sequence would emit
   * bytes that are not a string. */
  while (cut > 0 && ((unsigned char)out->ptr[cut] & 0xc0) == 0x80) {
    cut--;
  }
  out->len = cut;
  out->ptr[cut] = '\0';
  return kbc_str_puts(out, "...[truncated]");
}

/* ------------------------------------------------------------------- url -- */

const char *kbc_rs_proto_str(kbc_rs_proto p) {
  switch (p) {
    case KBC_RS_PROTO_HTTPS:
      return "https";
    case KBC_RS_PROTO_SSH:
      return "ssh";
    case KBC_RS_PROTO_FILE:
      return "file";
  }
  return "https";
}

static bool host_ok(const char *h) {
  size_t n = strlen(h);
  if (n == 0 || n > 253) {
    return false;
  }
  if (!isalnum((unsigned char)h[0])) {
    return false;
  }
  for (size_t i = 0; i < n; i++) {
    if (!isalnum((unsigned char)h[i]) && h[i] != '.' && h[i] != '-') {
      return false;
    }
  }
  return !(h[n - 1] == '.' || strstr(h, "..") != NULL);
}

static bool port_ok(const char *p) {
  size_t n = strlen(p);
  if (n == 0 || n > 5) {
    return false;
  }
  for (size_t i = 0; i < n; i++) {
    if (!isdigit((unsigned char)p[i])) {
      return false;
    }
  }
  long v = strtol(p, NULL, 10);
  return v >= 1 && v <= 65535;
}

/* `host` or `host:port`, with the SCHEME'S DEFAULT PORT folded out. Two
 * spellings of one endpoint must produce one authority, because the
 * credential helper compares the authority as an EXACT string: a kept
 * ":443" would mean a correct remote failing closed as a credential fault.
 * A NON-default port stays — it is a different endpoint. */
static bool authority_ok(const char *a, kbc_rs_proto proto, char *out,
                         size_t cap) {
  const char *colon = strchr(a, ':');
  char host[254];
  if (colon != NULL) {
    size_t hn = (size_t)(colon - a);
    if (hn >= sizeof host) {
      return false;
    }
    memcpy(host, a, hn);
    host[hn] = '\0';
    if (!host_ok(host) || !port_ok(colon + 1)) {
      return false;
    }
    const char *dflt = (proto == KBC_RS_PROTO_HTTPS) ? "443" : "22";
    int n;
    if (strcmp(colon + 1, dflt) == 0) {
      n = snprintf(out, cap, "%s", host);
    } else {
      n = snprintf(out, cap, "%s:%s", host, colon + 1);
    }
    for (size_t i = 0; i < hn; i++) {
      out[i] = (char)tolower((unsigned char)out[i]);
    }
    return n > 0 && (size_t)n < cap;
  }
  if (!host_ok(a)) {
    return false;
  }
  if (snprintf(out, cap, "%s", a) >= (int)cap) {
    return false;
  }
  for (size_t i = 0; out[i] != '\0'; i++) {
    out[i] = (char)tolower((unsigned char)out[i]);
  }
  return true;
}

static bool path_ok(const char *p) {
  size_t n = strlen(p);
  if (n == 0 || n >= 1024) {
    return false;
  }
  char c0 = p[0];
  if (!isalnum((unsigned char)c0) && c0 != '~' && c0 != '_') {
    return false;
  }
  for (size_t i = 0; i < n; i++) {
    char c = p[i];
    if (!(isalnum((unsigned char)c) || c == '.' || c == '_' || c == '-' ||
          c == '/' || c == '~' || c == '+')) {
      return false;
    }
  }
  const char *seg = p;
  while (seg != NULL) {
    const char *slash = strchr(seg, '/');
    size_t len = (slash != NULL) ? (size_t)(slash - seg) : strlen(seg);
    if (len == 2 && seg[0] == '.' && seg[1] == '.') {
      return false;
    }
    seg = (slash != NULL) ? slash + 1 : NULL;
  }
  return true;
}

static bool starts_ci(const char *s, const char *lit) {
  return ci_at(s, strlen(s), 0, lit);
}

/* A copy that REFUSES on truncation instead of silently producing a shorter
 * string. Every caller here composes a field that another field must agree
 * with: a URL's `raw` must rebuild from its own `authority` and `path`, and a
 * credential helper is asked about `host` as an exact string. A silently
 * truncated value is a DIFFERENT one that still looks valid, and "looks valid
 * but is wrong" is the failure this whole module defends against. */
static kbc_status set_exact(char *dst, size_t cap, const char *src,
                            kbc_err *err, const char *what) {
  size_t n = strlen(src);
  if (n >= cap) {
    return kbc_err_set(err, KBC_ERR_INVALID, "%s: %zu bytes does not fit %zu",
                       what, n, cap);
  }
  memcpy(dst, src, n + 1);
  return KBC_OK;
}

/* Same, for a two-piece composition, measured before anything is copied. */
static kbc_status set_exact2(char *dst, size_t cap, const char *a,
                             const char *b, kbc_err *err, const char *what) {
  size_t an = strlen(a);
  size_t bn = strlen(b);
  if (an + bn >= cap) {
    return kbc_err_set(err, KBC_ERR_INVALID, "%s: %zu bytes does not fit %zu",
                       what, an + bn, cap);
  }
  memcpy(dst, a, an);
  memcpy(dst + an, b, bn + 1);
  return KBC_OK;
}
kbc_status kbc_rs_url_parse_remote(const char *s, kbc_rs_url *out,
                                   kbc_err *err) {
  if (s == NULL || *s == '\0') {
    return kbc_err_set(err, KBC_ERR_INVALID, "remote url is empty");
  }
  if (strlen(s) >= KBC_RS_URL_MAX) {
    return kbc_err_set(err, KBC_ERR_INVALID, "remote url is too long");
  }
  for (const char *p = s; *p != '\0'; p++) {
    unsigned char c = (unsigned char)*p;
    if (c < 0x20 || c == 0x7f || c == ' ' || c == '\t') {
      return kbc_err_set(err, KBC_ERR_INVALID,
                         "remote url contains a control or space character");
    }
  }
  if (strstr(s, "::") != NULL) {
    return kbc_err_set(
        err, KBC_ERR_INVALID,
        "remote-helper transports (<name>::<address>) are not allowed");
  }
  if (strchr(s, '%') != NULL) {
    return kbc_err_set(
        err, KBC_ERR_INVALID,
        "remote url carries a percent escape, which is refused because the "
        "literal and the decoded reading name two different projects");
  }
  memset(out, 0, sizeof *out);
  if (starts_ci(s, "file:")) {
    return kbc_err_set(err, KBC_ERR_INVALID,
                       "file:// is not allowed for a remote");
  }
  if (starts_ci(s, "http://")) {
    return kbc_err_set(err, KBC_ERR_INVALID,
                       "plain http:// is not allowed; use https://");
  }
  if (starts_ci(s, "https://") || starts_ci(s, "ssh://")) {
    bool https = starts_ci(s, "https://");
    const char *rest = s + (https ? 8 : 6);
    const char *slash = strchr(rest, '/');
    if (slash == NULL) {
      return kbc_err_set(err, KBC_ERR_INVALID,
                         "remote url has no repository path");
    }
    char auth[264];
    size_t an = (size_t)(slash - rest);
    if (an >= sizeof auth) {
      return kbc_err_set(err, KBC_ERR_INVALID, "remote url authority is too long");
    }
    memcpy(auth, rest, an);
    auth[an] = '\0';
    const char *at = strchr(auth, '@');
    if (at != NULL) {
      if (https) {
        /* A token in a URL is a token in /proc/<pid>/cmdline and in every
         * error git prints. The credential helper is the only route. */
        return kbc_err_set(err, KBC_ERR_INVALID,
                           "https remote must not carry userinfo");
      }
      if ((size_t)(at - auth) != 3 || memcmp(auth, "git", 3) != 0) {
        return kbc_err_set(err, KBC_ERR_INVALID,
                           "ssh remote must use the `git` user");
      }
      memmove(auth, at + 1, strlen(at + 1) + 1);
    } else if (!https) {
      return kbc_err_set(err, KBC_ERR_INVALID,
                         "ssh remote must use the `git` user");
    }
    kbc_rs_proto proto = https ? KBC_RS_PROTO_HTTPS : KBC_RS_PROTO_SSH;
    if (!authority_ok(auth, proto, out->authority, sizeof out->authority)) {
      return kbc_err_set(err, KBC_ERR_INVALID,
                         "remote url has an invalid host or port");
    }
    if (!path_ok(slash + 1)) {
      return kbc_err_set(err, KBC_ERR_INVALID,
                         "remote url has an invalid repository path");
    }
    out->proto = proto;
    kbc_status st = set_exact(out->host, sizeof out->host, out->authority, err,
                              "remote host");
    if (kbc_failed(st)) {
      return st;
    }
    char *c = strchr(out->host, ':');
    if (c != NULL) {
      *c = '\0';
    }
    st = set_exact(out->path, sizeof out->path, slash + 1, err,
                   "remote repository path");
    if (kbc_failed(st)) {
      return st;
    }
    char composed[KBC_RS_URL_MAX];
    int cn = https
                 ? snprintf(composed, sizeof composed, "https://%s/%s",
                            out->authority, out->path)
                 : snprintf(composed, sizeof composed, "ssh://git@%s/%s",
                            out->authority, out->path);
    if (cn < 0 || (size_t)cn >= sizeof composed) {
      return kbc_err_set(err, KBC_ERR_INVALID,
                         "remote url does not fit %zu bytes",
                         sizeof composed);
    }
    st = set_exact(out->raw, sizeof out->raw, composed, err, "remote url");
    if (kbc_failed(st)) {
      return st;
    }
    return KBC_OK;
  }
  if (strstr(s, "://") != NULL) {
    return kbc_err_set(err, KBC_ERR_INVALID,
                       "unsupported remote url form (allowed: https://, "
                       "ssh://git@, git@host:path)");
  }
  if (strncmp(s, "git@", 4) == 0) {
    const char *rest = s + 4;
    const char *colon = strchr(rest, ':');
    if (colon == NULL) {
      return kbc_err_set(err, KBC_ERR_INVALID,
                         "unsupported remote url form (allowed: https://, "
                         "ssh://git@, git@host:path)");
    }
    if (memchr(rest, '/', (size_t)(colon - rest)) != NULL) {
      return kbc_err_set(err, KBC_ERR_INVALID,
                         "scp-form remote url has a slash before its colon");
    }
    char host[254];
    size_t hn = (size_t)(colon - rest);
    if (hn >= sizeof host) {
      return kbc_err_set(err, KBC_ERR_INVALID, "remote url host is too long");
    }
    memcpy(host, rest, hn);
    host[hn] = '\0';
    if (!authority_ok(host, KBC_RS_PROTO_SSH, out->authority,
                      sizeof out->authority)) {
      return kbc_err_set(err, KBC_ERR_INVALID,
                         "remote url has an invalid host or port");
    }
    const char *p = (colon[1] == '/') ? colon + 2 : colon + 1;
    if (!path_ok(p)) {
      return kbc_err_set(err, KBC_ERR_INVALID,
                         "remote url has an invalid repository path");
    }
    out->proto = KBC_RS_PROTO_SSH;
    kbc_status st;
    st = set_exact(out->host, sizeof out->host, out->authority, err,
                   "remote host");
    if (kbc_failed(st)) {
      return st;
    }
    st = set_exact(out->path, sizeof out->path, p, err,
                   "remote repository path");
    if (kbc_failed(st)) {
      return st;
    }
    char composed[KBC_RS_URL_MAX];
    int cn = snprintf(composed, sizeof composed, "git@%s:%s", out->authority,
                      out->path);
    if (cn < 0 || (size_t)cn >= sizeof composed) {
      return kbc_err_set(err, KBC_ERR_INVALID,
                         "remote url does not fit %zu bytes",
                         sizeof composed);
    }
    return set_exact(out->raw, sizeof out->raw, composed, err, "remote url");
  }
  if (strchr(s, '@') != NULL && strchr(s, ':') != NULL) {
    return kbc_err_set(err, KBC_ERR_INVALID,
                       "scp-form remote url must use the `git` user");
  }
  return kbc_err_set(err, KBC_ERR_INVALID,
                     "unsupported remote url form (allowed: https://, "
                     "ssh://git@, git@host:path)");
}

kbc_status kbc_rs_url_local_seed(const char *abs_path, kbc_rs_url *out,
                                 kbc_err *err) {
  if (abs_path == NULL || abs_path[0] != '/') {
    return kbc_err_set(err, KBC_ERR_INVALID,
                       "a local seed source must be an absolute path");
  }
  if (strlen(abs_path) >= KBC_RS_URL_MAX) {
    return kbc_err_set(err, KBC_ERR_INVALID, "local seed path is too long");
  }
  for (const char *p = abs_path; *p != '\0'; p++) {
    unsigned char c = (unsigned char)*p;
    if (c < 0x20 || c == 0x7f) {
      return kbc_err_set(err, KBC_ERR_INVALID,
                         "local seed path contains a control character");
    }
  }
  if (strstr(abs_path, "::") != NULL) {
    return kbc_err_set(
        err, KBC_ERR_INVALID,
        "remote-helper transports (<name>::<address>) are not allowed");
  }
  /* A ".." segment would let a configured path climb out of the root the
   * operator named. Refuse it here rather than at every use. */
  const char *seg = abs_path;
  while (seg != NULL) {
    const char *slash = strchr(seg, '/');
    size_t len = (slash != NULL) ? (size_t)(slash - seg) : strlen(seg);
    if (len == 2 && seg[0] == '.' && seg[1] == '.') {
      return kbc_err_set(err, KBC_ERR_INVALID,
                         "local seed path contains a `..` segment");
    }
    seg = (slash != NULL) ? slash + 1 : NULL;
  }
  memset(out, 0, sizeof *out);
  out->proto = KBC_RS_PROTO_FILE;
  snprintf(out->raw, sizeof out->raw, "%s", abs_path);
  snprintf(out->path, sizeof out->path, "%s", abs_path);
  return KBC_OK;
}

kbc_status kbc_rs_url_https_equivalent(const kbc_rs_url *u, kbc_rs_url *out,
                                       kbc_err *err) {
  if (u->proto == KBC_RS_PROTO_HTTPS) {
    *out = *u;
    return KBC_OK;
  }
  if (u->proto != KBC_RS_PROTO_SSH) {
    return kbc_err_set(err, KBC_ERR_UNSUPPORTED,
                       "a local path has no https form");
  }
  const char *colon = strchr(u->authority, ':');
  if (colon != NULL && strcmp(colon + 1, "22") != 0) {
    /* Bitbucket Server's :7999 has no knowable https twin. Refuse rather
     * than guess a port. */
    return kbc_err_set(err, KBC_ERR_UNSUPPORTED,
                       "an ssh url on a non-default port has no unambiguous "
                       "https form");
  }
  if (u->path[0] == '~') {
    return kbc_err_set(err, KBC_ERR_UNSUPPORTED,
                       "a home-relative ssh path has no unambiguous https "
                       "form");
  }
  char buf[KBC_RS_URL_MAX];
  size_t pl = strlen(u->path);
  const char *suffix =
      (pl >= 4 && strcmp(u->path + pl - 4, ".git") == 0) ? "" : ".git";
  int n = snprintf(buf, sizeof buf, "https://%s/%s%s", u->host, u->path,
                   suffix);
  if (n < 0 || (size_t)n >= sizeof buf) {
    return kbc_err_set(err, KBC_ERR_INVALID,
                       "https form does not fit %zu bytes", sizeof buf);
  }
  return kbc_rs_url_parse_remote(buf, out, err);
}

kbc_status kbc_rs_remote_name_is_valid(const char *name) {
  if (name == NULL) {
    return KBC_ERR_INVALID;
  }
  size_t n = strlen(name);
  if (n == 0 || n > 63) {
    return KBC_ERR_INVALID;
  }
  if (name[0] < 'a' || name[0] > 'z') {
    return KBC_ERR_INVALID;
  }
  for (size_t i = 0; i < n; i++) {
    char c = name[i];
    if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-')) {
      return KBC_ERR_INVALID;
    }
  }
  return KBC_OK;
}

kbc_status kbc_rs_store_key(const kbc_rs_url *u, char *out, size_t cap,
                            kbc_err *err) {
  if (u->proto == KBC_RS_PROTO_FILE) {
    return kbc_err_set(err, KBC_ERR_INVALID,
                       "a local path is not a forge project");
  }
  /* Drop repeated slashes, a trailing slash and a trailing ".git". The
   * path's case is preserved: only github.com's owner/name is
   * case-insensitive, and a guess about any other host would merge two
   * projects. */
  char clean[1024];
  size_t o = 0;
  bool prev_slash = false;
  for (size_t i = 0; u->path[i] != '\0'; i++) {
    char c = u->path[i];
    if (c == '/') {
      if (prev_slash) {
        continue;
      }
      prev_slash = true;
    } else {
      prev_slash = false;
    }
    if (o + 1 >= sizeof clean) {
      return kbc_err_set(err, KBC_ERR_INVALID, "remote path is too long");
    }
    clean[o++] = c;
  }
  while (o > 0 && clean[o - 1] == '/') {
    o--;
  }
  clean[o] = '\0';
  if (o > 4 && strcmp(clean + o - 4, ".git") == 0) {
    o -= 4;
    clean[o] = '\0';
  }
  if (o == 0) {
    return kbc_err_set(err, KBC_ERR_INVALID, "remote url has no path");
  }
  int n = snprintf(out, cap, "%s/%s", u->authority, clean);
  if (n < 0 || (size_t)n >= cap) {
    return kbc_err_set(err, KBC_ERR_INVALID, "store key does not fit");
  }
  if (strcmp(u->host, "github.com") == 0) {
    char *slash = strchr(out, '/');
    for (char *p = out; *p != '\0'; p++) {
      *p = (char)tolower((unsigned char)*p);
    }
    (void)slash;
  }
  return KBC_OK;
}

/* ---------------------------------------------------------------- secret -- */

kbc_status kbc_rs_secret_new(kbc_rs_secret *out, const char *raw,
                             kbc_err *err) {
  memset(out, 0, sizeof *out);
  if (raw == NULL) {
    return kbc_err_set(err, KBC_ERR_INVALID, "credential is empty");
  }
  while (*raw == ' ' || *raw == '\t' || *raw == '\n' || *raw == '\r') {
    raw++;
  }
  size_t n = strlen(raw);
  while (n > 0) {
    char c = raw[n - 1];
    if (c != ' ' && c != '\t' && c != '\n' && c != '\r') {
      break;
    }
    n--;
  }
  if (n == 0) {
    return kbc_err_set(err, KBC_ERR_INVALID, "credential is empty");
  }
  if (n > 1024) {
    return kbc_err_set(err, KBC_ERR_INVALID,
                       "credential is longer than 1024 bytes");
  }
  /* A newline or a space in a token would inject lines into the git
   * credential protocol the helper answers in. */
  for (size_t i = 0; i < n; i++) {
    unsigned char c = (unsigned char)raw[i];
    if (c < 0x21 || c == 0x7f) {
      return kbc_err_set(err, KBC_ERR_INVALID,
                         "credential contains whitespace or a control "
                         "character");
    }
  }
  char *buf = malloc(n + 1);
  if (buf == NULL) {
    return kbc_err_set(err, KBC_ERR_NOMEM, "out of memory for a credential");
  }
  memcpy(buf, raw, n);
  buf[n] = '\0';
  out->bytes = buf;
  out->len = n;
  return KBC_OK;
}

void kbc_rs_secret_wipe(kbc_rs_secret *s) {
  if (s == NULL || s->bytes == NULL) {
    return;
  }
  wipe(s->bytes, s->len);
  free(s->bytes);
  s->bytes = NULL;
  s->len = 0;
}

size_t kbc_rs_secret_len(const kbc_rs_secret *s) {
  return (s == NULL) ? 0 : s->len;
}

bool kbc_rs_secret_eq(const kbc_rs_secret *s, const char *candidate,
                      size_t n) {
  if (s == NULL || s->len != n) {
    return false;
  }
  return kbc_const_time_eq(s->bytes, candidate, n);
}

/* ------------------------------------------------------------------- git -- */

struct kbc_rs_git {
  char git_home[KBC_RS_PATH_MAX];
  char global_config[KBC_RS_PATH_MAX];
  char *git_bin; /* resolved absolute path, or NULL */
  char *path_env; /* sanitized PATH, or NULL */
  kbc_strlist passthrough; /* proxy + CA bundle variables, read at open */
  kbc_strlist safe_dirs;  /* safe.directory entries for local sources */
};

/* Variables that survive into `inherit` mode. Everything else is either a
 * retargeting variable that would silently point the call at another
 * repository, or a debug switch that dumps headers and therefore secrets. */
static const char *const PASSTHROUGH_VARS[] = {
    "SSL_CERT_FILE", "SSL_CERT_DIR", "HTTPS_PROXY", "https_proxy",
    "HTTP_PROXY",    "http_proxy",  "NO_PROXY",     "no_proxy",
    "ALL_PROXY",     "all_proxy",
};

/* Stripped even in `inherit`: any of these would retarget a store call at
 * another repository, or turn on a prompt / a debug dump. */
static const char *const RETARGETING_VARS[] = {
    "GIT_DIR", "GIT_WORK_TREE", "GIT_INDEX_FILE", "GIT_OBJECT_DIRECTORY",
    "GIT_ALTERNATE_OBJECT_DIRECTORIES", "GIT_COMMON_DIR", "GIT_NAMESPACE",
    "GIT_CONFIG_PARAMETERS", "GIT_CONFIG_COUNT", "GIT_CEILING_DIRECTORIES",
    "GIT_PREFIX", "GIT_ASKPASS", "SSH_ASKPASS", "DISPLAY", "WAYLAND_DISPLAY",
    "GIT_EXEC_PATH", "GIT_CURL_VERBOSE", "GIT_SSL_NO_VERIFY",
};

static const char *const WRITE_TO_REMOTE[] = {"push", "send-pack",
                                              "receive-pack"};

/* argv subcommands this module refuses outright. Not a lint: a refusal at
 * run time, so a future caller cannot route around it by accident. */
static bool is_write_to_remote(const char *sub) {
  for (size_t i = 0; i < sizeof WRITE_TO_REMOTE / sizeof WRITE_TO_REMOTE[0];
       i++) {
    if (strcmp(sub, WRITE_TO_REMOTE[i]) == 0) {
      return true;
    }
  }
  return false;
}

/* A growable, NULL-terminated argv. */
typedef struct {
  char **v;
  size_t n;
  size_t cap;
} argv_builder;

static void argv_init(argv_builder *a) {
  a->v = NULL;
  a->n = 0;
  a->cap = 0;
}

static kbc_status argv_push_owned(argv_builder *a, char *s) {
  if (a->n + 2 > a->cap) {
    size_t cap = (a->cap == 0) ? 16 : a->cap * 2;
    char **v = realloc(a->v, cap * sizeof *v);
    if (v == NULL) {
      free(s);
      return KBC_ERR_NOMEM;
    }
    a->v = v;
    a->cap = cap;
  }
  a->v[a->n++] = s;
  a->v[a->n] = NULL;
  return KBC_OK;
}

static kbc_status argv_push(argv_builder *a, const char *s) {
  return argv_push_owned(a, strdup(s));
}

static void argv_free(argv_builder *a) {
  for (size_t i = 0; i < a->n; i++) {
    free(a->v[i]);
  }
  free(a->v);
  argv_init(a);
}

/* Drop empty and relative PATH entries: a relative entry resolves against
 * the child's cwd, and the child's cwd is store-owned. */
static char *sanitize_path(const char *p) {
  if (p == NULL || *p == '\0') {
    return NULL;
  }
  kbc_str out;
  kbc_str_init(&out);
  const char *seg = p;
  while (*seg != '\0') {
    const char *colon = strchr(seg, ':');
    size_t len = (colon != NULL) ? (size_t)(colon - seg) : strlen(seg);
    if (len > 0 && seg[0] == '/' &&
        (out.len == 0 || kbc_str_putc(&out, ':') == KBC_OK)) {
      if (kbc_str_append(&out, seg, len) != KBC_OK) {
        kbc_str_free(&out);
        return NULL;
      }
    }
    if (colon == NULL) {
      break;
    }
    seg = colon + 1;
  }
  if (out.len == 0) {
    kbc_str_free(&out);
    return NULL;
  }
  return out.ptr; /* ownership transfers; out is not freed */
}

static bool executable_file(const char *p) {
  struct stat st;
  if (stat(p, &st) != 0 || !S_ISREG(st.st_mode)) {
    return false;
  }
  return (st.st_mode & 0111) != 0;
}

static char *resolve_git(const char *path_env) {
  if (path_env == NULL) {
    return NULL;
  }
  const char *seg = path_env;
  while (*seg != '\0') {
    const char *colon = strchr(seg, ':');
    size_t len = (colon != NULL) ? (size_t)(colon - seg) : strlen(seg);
    if (len > 0) {
      char cand[KBC_RS_PATH_MAX];
      if (snprintf(cand, sizeof cand, "%.*s/git", (int)len, seg) <
          (int)sizeof cand &&
          executable_file(cand)) {
        return strdup(cand);
      }
    }
    if (colon == NULL) {
      break;
    }
    seg = colon + 1;
  }
  return NULL;
}

/* A NULL-terminated string vector. kbc_strlist does NOT terminate `items`,
 * and posix_spawn reads envp until it finds a NULL: handing it a kbc_strlist
 * is an out-of-bounds read, which the kernel reports as EFAULT. */
typedef struct {
  char **v;
  size_t n;
  size_t cap;
} strvec;

static void strvec_init(strvec *s) {
  s->v = NULL;
  s->n = 0;
  s->cap = 0;
}

static kbc_status strvec_push_owned(strvec *s, char *p) {
  if (s->n + 2 > s->cap) {
    size_t cap = (s->cap == 0) ? 32 : s->cap * 2;
    char **v = realloc(s->v, cap * sizeof *v);
    if (v == NULL) {
      free(p);
      return KBC_ERR_NOMEM;
    }
    s->v = v;
    s->cap = cap;
  }
  s->v[s->n++] = p;
  s->v[s->n] = NULL;
  return KBC_OK;
}

static kbc_status strvec_push(strvec *s, const char *p) {
  return strvec_push_owned(s, strdup(p));
}

static void strvec_free(strvec *s) {
  for (size_t i = 0; i < s->n; i++) {
    free(s->v[i]);
  }
  free(s->v);
  strvec_init(s);
}

static kbc_status env_set(strvec *env, const char *k, const char *v) {
  char buf[1024];
  int n = snprintf(buf, sizeof buf, "%s=%s", k, v);
  if (n < 0 || (size_t)n >= sizeof buf) {
    return KBC_ERR_INVALID;
  }
  return strvec_push(env, buf);
}

static kbc_status write_global_config(kbc_rs_git *g, kbc_err *err) {
  kbc_str body;
  kbc_str_init(&body);
  kbc_status st = kbc_str_puts(
      &body, "# Written by kb-c (review store). Regenerated; do not edit.\n"
             "[safe]\n");
  for (size_t i = 0; st == KBC_OK && i < g->safe_dirs.len; i++) {
    kbc_str esc;
    kbc_str_init(&esc);
    /* The path is quoted, and a control character in it would inject a
     * config line; kbc_rs_git_allow_local_source refuses those upstream, so
     * all that is left here is to quote. */
    for (const char *p = g->safe_dirs.items[i]; *p != '\0'; p++) {
      if (*p == '\\' || *p == '"') {
        if (kbc_str_putc(&esc, '\\') != KBC_OK) {
          st = KBC_ERR_NOMEM;
          break;
        }
      }
      if (kbc_str_putc(&esc, *p) != KBC_OK) {
        st = KBC_ERR_NOMEM;
        break;
      }
    }
    if (st == KBC_OK) {
      st = kbc_str_printf(&body, "\tdirectory = \"%s\"\n", esc.ptr);
    }
    kbc_str_free(&esc);
  }
  if (st == KBC_OK) {
    /* The temp name is the destination plus a suffix, and a truncated one
     * would be a DIFFERENT file that `rename` then replaces the real config
     * with — so this refuses rather than writing short. */
    char tmp[KBC_RS_PATH_MAX + 8];
    int tn = snprintf(tmp, sizeof tmp, "%s.tmp", g->global_config);
    if (tn < 0 || (size_t)tn >= sizeof tmp) {
      st = kbc_err_set(err, KBC_ERR_INVALID,
                       "global config path does not fit %zu bytes",
                       sizeof tmp);
    } else {
      st = write_file_sync(tmp, body.ptr, body.len, err);
    }
    if (st == KBC_OK) {
      if (rename(tmp, g->global_config) != 0) {
        st = kbc_err_set(err, KBC_ERR_IO, "rename %s: %s", g->global_config,
                         strerror(errno));
      } else {
        chmod(g->global_config, 0600);
      }
    }
  }
  kbc_str_free(&body);
  return st;
}

kbc_status kbc_rs_git_new(const char *git_home, const char *path,
                          kbc_rs_git **out, kbc_err *err) {
  *out = NULL;
  if (git_home == NULL || git_home[0] != '/') {
    return kbc_err_set(err, KBC_ERR_INVALID,
                       "store git home must be an absolute path");
  }
  if (strlen(git_home) >= KBC_RS_PATH_MAX) {
    return kbc_err_set(err, KBC_ERR_INVALID, "store git home is too long");
  }
  kbc_rs_git *g = calloc(1, sizeof *g);
  if (g == NULL) {
    return kbc_err_set(err, KBC_ERR_NOMEM, "out of memory for a git handle");
  }
  kbc_strlist_init(&g->passthrough);
  kbc_strlist_init(&g->safe_dirs);
  snprintf(g->git_home, sizeof g->git_home, "%s", git_home);
  snprintf(g->global_config, sizeof g->global_config, "%s/gitconfig",
           git_home);
  g->path_env = sanitize_path(path);
  g->git_bin = resolve_git(g->path_env);
  for (size_t i = 0; i < sizeof PASSTHROUGH_VARS / sizeof PASSTHROUGH_VARS[0];
       i++) {
    const char *v = getenv(PASSTHROUGH_VARS[i]);
    if (v != NULL && kbc_strlist_push(&g->passthrough, v) != KBC_OK) {
      kbc_rs_git_free(g);
      return kbc_err_set(err, KBC_ERR_NOMEM, "out of memory for passthrough");
    }
  }
  kbc_status st = kbc_mkdir_p(git_home, err);
  if (st == KBC_OK) {
    chmod(git_home, 0700);
    st = write_global_config(g, err);
  }
  if (st != KBC_OK) {
    kbc_rs_git_free(g);
    return st;
  }
  *out = g;
  return KBC_OK;
}

void kbc_rs_git_free(kbc_rs_git *g) {
  if (g == NULL) {
    return;
  }
  free(g->git_bin);
  free(g->path_env);
  kbc_strlist_free(&g->passthrough);
  kbc_strlist_free(&g->safe_dirs);
  free(g);
}

const char *kbc_rs_git_home(const kbc_rs_git *g) {
  return (g == NULL) ? NULL : g->git_home;
}

bool kbc_rs_git_resolved(kbc_rs_git *g, char *out, size_t cap) {
  if (g == NULL || g->git_bin == NULL) {
    return false;
  }
  snprintf(out, cap, "%s", g->git_bin);
  return true;
}

kbc_status kbc_rs_git_allow_local_source(kbc_rs_git *g, const char *abs_path,
                                         kbc_err *err) {
  if (g == NULL) {
    return kbc_err_set(err, KBC_ERR_INVALID, "no git handle");
  }
  if (abs_path == NULL || abs_path[0] != '/') {
    return kbc_err_set(err, KBC_ERR_INVALID,
                       "a local source must be an absolute path");
  }
  for (const char *p = abs_path; *p != '\0'; p++) {
    unsigned char c = (unsigned char)*p;
    if (c < 0x20 || c == 0x7f) {
      return kbc_err_set(err, KBC_ERR_INVALID,
                         "a local source path must carry no control "
                         "characters");
    }
  }
  /* Scoped to exactly the registered path, never "*": safe.directory is a
 * statement that one directory's ownership is acceptable, and a wildcard
 * would make every directory on the host acceptable. */
  if (kbc_strlist_contains(&g->safe_dirs, abs_path)) {
    return KBC_OK;
  }
  kbc_status st = kbc_strlist_push(&g->safe_dirs, abs_path);
  if (st != KBC_OK) {
    return kbc_err_set(err, KBC_ERR_NOMEM, "out of memory for safe.directory");
  }
  return write_global_config(g, err);
}

/* The credential-helper shell snippet. It answers `get` for EXACTLY this
 * credential's protocol and host by relaying the inherited pipe on
 * HELPER_FD, and consumes-and-ignores everything else. The text is composed
 * only from the fd number, the protocol and the authority, all of which
 * passed the alphabet the snippet relies on — so no shell metacharacter can
 * reach it and no secret is in it. */
#define HELPER_FD 3

static bool helper_alphabet_ok(const char *s, bool allow_colon) {
  if (*s == '\0') {
    return false;
  }
  for (const char *p = s; *p != '\0'; p++) {
    bool ok = isalnum((unsigned char)*p) || *p == '.' || *p == '-';
    if (allow_colon && *p == ':') {
      ok = true;
    }
    if (!ok) {
      return false;
    }
  }
  return true;
}

static kbc_status helper_snippet(const kbc_rs_cred *c, kbc_str *out,
                                 kbc_err *err) {
  if (!helper_alphabet_ok(c->host, true) ||
      !helper_alphabet_ok(kbc_rs_proto_str(c->auth == KBC_RS_AUTH_TOKEN
                                               ? KBC_RS_PROTO_HTTPS
                                               : KBC_RS_PROTO_HTTPS),
                          false)) {
    return kbc_err_set(err, KBC_ERR_INTERNAL,
                       "credential scope failed its alphabet check");
  }
  return kbc_str_printf(
      out,
      "f() { test \"$1\" = get || { cat >/dev/null; exit 0; }; p=; h=; "
      "while IFS= read -r l; do [ -z \"$l\" ] && break; case \"$l\" in "
      "protocol=*) p=\"${l#protocol=}\";; host=*) h=\"${l#host=}\";; esac; "
      "done; [ \"$p\" = 'https' ] && [ \"$h\" = '%s' ] || exit 0; "
      "cat <&%d; }; f",
      c->host, HELPER_FD);
}

/* A CLOEXEC pipe pre-loaded with the credential answer. The returned
 * descriptor is duplicated to >= 10 so it can never be HELPER_FD itself:
 * dup2(fd, fd) is a no-op and would NOT clear FD_CLOEXEC, so the child
 * would lose the pipe at exec. */
static kbc_status token_pipe(const kbc_rs_cred *c, int *out_fd, kbc_err *err) {
  *out_fd = -1;
  kbc_str payload;
  kbc_str_init(&payload);
  kbc_status st = kbc_str_printf(&payload, "username=%s\npassword=",
                                 c->username);
  if (st == KBC_OK) {
    st = kbc_str_append(&payload, c->secret.bytes, c->secret.len);
  }
  if (st == KBC_OK) {
    st = kbc_str_putc(&payload, '\n');
  }
  if (st != KBC_OK) {
    kbc_str_free(&payload);
    return kbc_err_set(err, KBC_ERR_NOMEM, "out of memory for a credential");
  }
  if (payload.len >= 4096) {
    /* Well under PIPE_BUF capacity, so the write below can never block. */
    kbc_str_free(&payload);
    return kbc_err_set(err, KBC_ERR_INVALID, "credential payload too large");
  }
  int fds[2];
  if (pipe2(fds, O_CLOEXEC) != 0) {
    kbc_str_free(&payload);
    return kbc_err_set(err, KBC_ERR_IO, "pipe2: %s", strerror(errno));
  }
  size_t off = 0;
  while (off < payload.len) {
    ssize_t w = write(fds[1], payload.ptr + off, payload.len - off);
    if (w < 0) {
      if (errno == EINTR) {
        continue;
      }
      kbc_status e = kbc_err_set(err, KBC_ERR_IO, "credential pipe: %s",
                                 strerror(errno));
      close(fds[0]);
      close(fds[1]);
      kbc_str_free(&payload);
      return e;
    }
    off += (size_t)w;
  }
  kbc_str_free(&payload);
  close(fds[1]); /* the payload is one-shot: git caches it for its run */
  int hi = fcntl(fds[0], F_DUPFD_CLOEXEC, 10);
  close(fds[0]);
  if (hi < 0) {
    return kbc_err_set(err, KBC_ERR_IO, "credential pipe: %s", strerror(errno));
  }
  *out_fd = hi;
  return KBC_OK;
}

static kbc_status build_env(const kbc_rs_git *g, const kbc_rs_call *call,
                            const char *allow_protocol, strvec *env,
                            kbc_err *err) {
  if (call->auth == KBC_RS_AUTH_INHERIT) {
    for (char **e = environ; *e != NULL; e++) {
      const char *eq = strchr(*e, '=');
      if (eq == NULL) {
        continue;
      }
      size_t kn = (size_t)(eq - *e);
      bool strip = false;
      for (size_t i = 0;
           i < sizeof RETARGETING_VARS / sizeof RETARGETING_VARS[0]; i++) {
        if (strlen(RETARGETING_VARS[i]) == kn &&
            strncmp(*e, RETARGETING_VARS[i], kn) == 0) {
          strip = true;
          break;
        }
      }
      if (!strip && kn >= 9 && strncmp(*e, "GIT_TRACE", 9) == 0) {
        strip = true;
      }
      if (!strip) {
        kbc_status st = strvec_push(env, *e);
        if (st != KBC_OK) {
          return kbc_err_set(err, KBC_ERR_NOMEM, "out of memory for env");
        }
      }
    }
    RS_TRY(env_set(env, "SSH_ASKPASS_REQUIRE", "never"));
  } else {
    /* env_clear plus an allowlist. Nothing ambient reaches git except the
     * proxy and CA-bundle variables the operator configured. */
    if (g->path_env != NULL) {
      RS_TRY(env_set(env, "PATH", g->path_env));
    }
    RS_TRY(env_set(env, "HOME", g->git_home));
    RS_TRY(env_set(env, "XDG_CONFIG_HOME", g->git_home));
    RS_TRY(env_set(env, "GIT_CONFIG_NOSYSTEM", "1"));
    RS_TRY(env_set(env, "GIT_CONFIG_GLOBAL", g->global_config));
    RS_TRY(env_set(env, "GIT_ASKPASS", ""));
    RS_TRY(env_set(env, "SSH_ASKPASS", ""));
    RS_TRY(env_set(env, "SSH_ASKPASS_REQUIRE", "never"));
    RS_TRY(env_set(env, "GIT_OPTIONAL_LOCKS", "0"));
    RS_TRY(env_set(env, "GIT_CEILING_DIRECTORIES", g->git_home));
    for (size_t i = 0; i < g->passthrough.len; i++) {
      RS_TRY(strvec_push(env, g->passthrough.items[i]));
    }
  }
  RS_TRY(env_set(env, "GIT_TERMINAL_PROMPT", "0"));
  /* The classifier reads English, and a translated git error would classify
 * as "failed". */
  RS_TRY(env_set(env, "LC_ALL", "C"));
  RS_TRY(env_set(env, "LANG", "C"));
  RS_TRY(env_set(env, "GIT_ALLOW_PROTOCOL", allow_protocol));
  if (call->git_dir != NULL) {
    RS_TRY(env_set(env, "GIT_DIR", call->git_dir));
  }
  return KBC_OK;
}

static const char *protocols_for(kbc_rs_auth_kind k) {
  switch (k) {
    case KBC_RS_AUTH_LOCAL_ONLY:
      return "file";
    case KBC_RS_AUTH_ANONYMOUS:
      return "https";
    case KBC_RS_AUTH_TOKEN:
      return "https";
    case KBC_RS_AUTH_INHERIT:
      return "https:ssh";
  }
  return "file";
}


/* The one place a child process is created.
 *
 * fork+exec rather than posix_spawn for three reasons that matter here: the
 * child needs a working directory (posix_spawn has no portable chdir, and a
 * store call that resolved a relative PATH entry against the DAEMON's cwd
 * would be a store call into whatever directory the daemon happened to be
 * started in); the secret's read end has to become descriptor 3 WITHOUT
 * FD_CLOEXEC, and only the parent can say so portably; and between fork and
 * exec this does nothing but async-signal-safe calls, which is the same bar
 * posix_spawn holds itself to.
 *
 * The child leads its own process group (setpgid(0, 0)) so a deadline can
 * SIGKILL git AND every helper it forked, in one signal.
 */
static kbc_status spawn_child(const char *prog, char *const argv[],
                              char *const envp[], const char *cwd, int in_fd,
                              int out_w, int err_w, int helper_fd,
                              pid_t *out_pid, int *out_errno) {
  *out_pid = -1;
  *out_errno = 0;
  fflush(NULL);
  pid_t pid = fork();
  if (pid < 0) {
    *out_errno = errno;
    return KBC_ERR_IO;
  }
  if (pid == 0) {
    /* Child. Only async-signal-safe calls from here to execve. */
    (void)setpgid(0, 0);
    if (in_fd >= 0) {
      if (dup2(in_fd, STDIN_FILENO) < 0) {
        _exit(126);
      }
    } else {
      int devnull = open("/dev/null", O_RDONLY);
      if (devnull < 0 || dup2(devnull, STDIN_FILENO) < 0) {
        _exit(126);
      }
      if (devnull > STDERR_FILENO) {
        close(devnull);
      }
    }
    if (dup2(out_w, STDOUT_FILENO) < 0 || dup2(err_w, STDERR_FILENO) < 0) {
      _exit(126);
    }
    if (out_w > STDERR_FILENO) {
      close(out_w);
    }
    if (err_w > STDERR_FILENO) {
      close(err_w);
    }
    if (in_fd > STDERR_FILENO) {
      close(in_fd);
    }
    /* The secret reaches the child HERE and nowhere else: this dup is the
     * only thing in the whole system that makes the pipe readable as
     * descriptor 3 of this child, across its exec. The parent's copy is
     * FD_CLOEXEC, and the parent closes it as soon as the fork returns. */
    if (helper_fd >= 0) {
      if (dup2(helper_fd, HELPER_FD) < 0) {
        _exit(126);
      }
      if (helper_fd != HELPER_FD) {
        close(helper_fd);
      }
    }
    if (cwd != NULL && chdir(cwd) != 0) {
      _exit(126);
    }
    execve(prog, argv, envp);
    _exit(127);
  }
  /* The parent also sets the group, because both sides racing to do it is
   * the documented way to make sure it happened. */
  (void)setpgid(pid, pid);
  *out_pid = pid;
  return KBC_OK;
}

typedef struct {
  kbc_str buf;
  size_t cap;
  bool truncated;
} capture_sink;

static void sink_drain(capture_sink *s, int fd) {
  char chunk[8192];
  for (;;) {
    ssize_t r = read(fd, chunk, sizeof chunk);
    if (r > 0) {
      size_t room = (s->cap > s->buf.len) ? s->cap - s->buf.len : 0;
      size_t take = ((size_t)r < room) ? (size_t)r : room;
      /* Past the cap the bytes are READ AND DISCARDED: a child blocked on a
       * full pipe is a hung call, and unbounded capture is unbounded
       * daemon memory. */
      if (take > 0 && kbc_str_append(&s->buf, chunk, take) != KBC_OK) {
        s->truncated = true;
      }
      if (take < (size_t)r) {
        s->truncated = true;
      }
      continue;
    }
    if (r < 0 && errno == EINTR) {
      continue;
    }
    return; /* 0 = EOF, or an error we cannot recover from */
  }
}

void kbc_rs_output_init(kbc_rs_output *o) {
  memset(o, 0, sizeof *o);
  o->exit_code = -1;
  kbc_str_init(&o->stdout);
  kbc_str_init(&o->stderr);
}

void kbc_rs_output_free(kbc_rs_output *o) {
  if (o == NULL) {
    return;
  }
  kbc_str_free(&o->stdout);
  kbc_str_free(&o->stderr);
  memset(o, 0, sizeof *o);
  o->exit_code = -1;
}

/* True only when the leader has exited AND is still waitable (WNOWAIT), so
 * its pid — and therefore its group id — cannot have been recycled when the
 * group is killed. */
static bool exited_unreaped(pid_t pid) {
  for (;;) {
    siginfo_t info;
    memset(&info, 0, sizeof info);
    int rc = waitid(P_PID, (id_t)pid, &info, WEXITED | WNOHANG | WNOWAIT);
    if (rc == 0) {
      return info.si_pid != 0;
    }
    if (errno != EINTR) {
      return false;
    }
  }
}

static int64_t mono_ms(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (int64_t)ts.tv_sec * 1000 + (int64_t)ts.tv_nsec / 1000000;
}

kbc_status kbc_rs_git_run(kbc_rs_git *g, const kbc_rs_call *call,
                          kbc_rs_output *out, kbc_err *err) {
  kbc_rs_output_init(out);
  if (g == NULL || call == NULL || call->argv == NULL || call->argc == 0) {
    return kbc_err_set(err, KBC_ERR_INVALID, "git call has no argv");
  }
  const char *op = (call->op != NULL) ? call->op : call->argv[0];
  if (is_write_to_remote(call->argv[0])) {
    return kbc_err_set(err, KBC_ERR_INVALID,
                       "the review store never writes to a remote (`%s`)",
                       call->argv[0]);
  }
  if (g->git_bin == NULL) {
    return kbc_err_set(err, KBC_ERR_NOTFOUND,
                       "no `git` on the configured PATH for store call `%s`",
                       op);
  }
  if (call->auth == KBC_RS_AUTH_TOKEN &&
      (call->cred == NULL || call->cred->secret.bytes == NULL)) {
    return kbc_err_set(err, KBC_ERR_INVALID,
                       "store call `%s` asked for a token with none in hand",
                       op);
  }

  int helper_fd = -1;
  if (call->auth == KBC_RS_AUTH_TOKEN) {
    kbc_status st = token_pipe(call->cred, &helper_fd, err);
    if (st != KBC_OK) {
      return st;
    }
  }

  const char *allow = protocols_for(call->auth);
  strvec env;
  strvec_init(&env);
  argv_builder av;
  argv_init(&av);
  int out_pipe[2] = {-1, -1};
  int err_pipe[2] = {-1, -1};
  int in_pipe[2] = {-1, -1};
  int in_w = -1; /* the parent's copy of the stdin WRITE end */
  kbc_status st = KBC_OK;
  kbc_rs_class cls = KBC_RS_CLASS_FAILED;
  const char *fail = NULL;
  char failmsg[256] = {0};

  st = argv_push(&av, g->git_bin);
  static const char *const harden_common[] = {
      "core.hooksPath=/dev/null", "core.askPass=",
      "core.fsmonitor=false",   "protocol.allow=never",
      "gc.auto=0",              "maintenance.auto=false",
  };
  for (size_t i = 0; st == KBC_OK && i < 6; i++) {
    st = argv_push(&av, "-c");
    if (st == KBC_OK) {
      st = argv_push(&av, harden_common[i]);
    }
  }
  {
    const char *protos[3];
    size_t np = 0;
    const char *p = allow;
    while (*p != '\0' && np < 3) {
      protos[np++] = p;
      const char *colon = strchr(p, ':');
      if (colon == NULL) {
        break;
      }
      p = colon + 1;
    }
    for (size_t i = 0; st == KBC_OK && i < np; i++) {
      char buf[64];
      snprintf(buf, sizeof buf, "protocol.%s.allow=always", protos[i]);
      st = argv_push(&av, "-c");
      if (st == KBC_OK) {
        st = argv_push(&av, buf);
      }
    }
  }
  if (st == KBC_OK && call->auth != KBC_RS_AUTH_INHERIT) {
    /* Reset every inherited credential.helper BEFORE anything else: an
     * ambient helper would answer for a host this store has no credential
     * for, and would answer with the operator's own identity. */
    st = argv_push(&av, "-c");
    if (st == KBC_OK) {
      st = argv_push(&av, "credential.helper=");
    }
  }
  if (st == KBC_OK && call->auth == KBC_RS_AUTH_TOKEN) {
    kbc_str snip;
    kbc_str_init(&snip);
    kbc_status hs = helper_snippet(call->cred, &snip, err);
    if (hs == KBC_OK) {
      kbc_str line;
      kbc_str_init(&line);
      hs = kbc_str_printf(&line, "credential.helper=!%s", snip.ptr);
      if (hs == KBC_OK) {
        hs = argv_push(&av, "-c");
      }
      if (hs == KBC_OK) {
        hs = argv_push(&av, line.ptr);
      }
      if (hs == KBC_OK) {
        hs = argv_push(&av, "-c");
      }
      if (hs == KBC_OK) {
        hs = argv_push(&av, "credential.useHttpPath=false");
      }
      kbc_str_free(&line);
    }
    kbc_str_free(&snip);
    st = hs;
  }
  for (size_t i = 0; st == KBC_OK && i < call->argc; i++) {
    st = argv_push(&av, call->argv[i]);
  }
  if (st == KBC_OK) {
    st = build_env(g, call, allow, &env, err);
  }
  if (st == KBC_OK && pipe(out_pipe) != 0) {
    st = kbc_err_set(err, KBC_ERR_IO, "pipe: %s", strerror(errno));
  }
  if (st == KBC_OK && pipe(err_pipe) != 0) {
    st = kbc_err_set(err, KBC_ERR_IO, "pipe: %s", strerror(errno));
  }
  if (st == KBC_OK && call->stdin_bytes != NULL) {
    if (pipe(in_pipe) != 0) {
      st = kbc_err_set(err, KBC_ERR_IO, "pipe: %s", strerror(errno));
    } else {
      int flags = fcntl(in_pipe[0], F_GETFL);
      fcntl(in_pipe[0], F_SETFL, flags | O_NONBLOCK);
      /* The child gets the READ end; the parent writes to the WRITE end. */
      in_w = fcntl(in_pipe[1], F_DUPFD_CLOEXEC, 10);
      close(in_pipe[1]);
      in_pipe[1] = -1;
      if (in_w < 0) {
        st = kbc_err_set(err, KBC_ERR_IO, "dup stdin: %s", strerror(errno));
      }
    }
  }

  pid_t pid = -1;
  if (st == KBC_OK) {
    /* The child's cwd is the store-owned git_home, so a relative PATH entry
     * could never resolve to something under the daemon's own directory. */
    int saved = 0;
    st = spawn_child(g->git_bin, av.v, env.v, g->git_home, in_pipe[0], out_pipe[1],
                     err_pipe[1], helper_fd, &pid, &saved);
    if (st != KBC_OK) {
      cls = KBC_RS_CLASS_SPAWN_FAILED;
      snprintf(failmsg, sizeof failmsg, "spawn git: %s", strerror(saved));
      fail = failmsg;
    }
  }
  /* From here every exit is the same: drop the parent's copies, kill the
   * group, reap the leader. Nothing leaks and nothing outlives the call. */
  /* Close the parent's copies of the WRITE ends and nothing else. The READ
   * ends are the whole point of the exercise: the poll loop below reads from
   * them, and closing them here made every `poll` return POLLNVAL, so every
   * child byte was discarded and every caller saw empty stdout and empty
   * stderr. That is silent, it is invisible, and it looks exactly like a git
   * that decided to say nothing.
   *
   * `in_pipe` is different again: the child holds the READ end and the parent
   * holds `in_w` (closed just below), so BOTH halves of in_pipe go now. */
  if (out_pipe[1] >= 0) {
    close(out_pipe[1]);
  }
  if (err_pipe[1] >= 0) {
    close(err_pipe[1]);
  }
  for (int i = 0; i < 2; i++) {
    if (in_pipe[i] >= 0) {
      close(in_pipe[i]);
    }
  }
  if (in_w >= 0) {
    close(in_w);
  }
  if (helper_fd >= 0) {
    close(helper_fd);
  }
  if (st != KBC_OK || pid < 0) {
    argv_free(&av);
    strvec_free(&env);
    if (fail != NULL) {
      return kbc_err_set(err, KBC_ERR_NOTFOUND, "store git `%s`: %s", op,
                         failmsg);
    }
    if (st == KBC_ERR_NOMEM) {
      return kbc_err_set(err, KBC_ERR_NOMEM, "out of memory for store git");
    }
    return st;
  }

  fcntl(out_pipe[0], F_SETFL, O_NONBLOCK);
  fcntl(err_pipe[0], F_SETFL, O_NONBLOCK);
  size_t stdin_off = 0;
  size_t stdin_len = (call->stdin_bytes != NULL) ? call->stdin_len : 0;
  bool stdin_open = (in_w >= 0);
  capture_sink so = {{0, 0, 0}, (call->stdout_cap != 0) ? call->stdout_cap
                                                        : KBC_RS_STDOUT_CAP,
                     false};
  capture_sink se = {{0, 0, 0}, KBC_RS_STDERR_CAP, false};
  kbc_str_init(&so.buf);
  kbc_str_init(&se.buf);
  int64_t deadline =
      mono_ms() + (int64_t)(call->timeout_s != 0 ? call->timeout_s : 60u) * 1000;
  bool timed_out = false;
  bool reaped = false;
  while (!reaped) {
    struct pollfd pfd[2];
    pfd[0].fd = out_pipe[0];
    pfd[1].fd = err_pipe[0];
    pfd[0].events = pfd[1].events = POLLIN;
    pfd[0].revents = pfd[1].revents = 0;
    int pr = poll(pfd, 2, 50);
    if (pr > 0) {
      if ((pfd[0].revents & (POLLIN | POLLHUP)) != 0) {
        sink_drain(&so, out_pipe[0]);
      }
      if ((pfd[1].revents & (POLLIN | POLLHUP)) != 0) {
        sink_drain(&se, err_pipe[0]);
      }
    }
    if (stdin_open && stdin_off < stdin_len) {
      ssize_t w = write(in_w, call->stdin_bytes + stdin_off, stdin_len - stdin_off);
      if (w > 0) {
        stdin_off += (size_t)w;
      } else if (w < 0 && errno != EAGAIN && errno != EINTR) {
        stdin_open = false; /* the child closed stdin: its decision, not ours */
      }
    }
    if (exited_unreaped(pid)) {
      /* Kill the group on EVERY exit path, so no helper or remote-helper the
       * child forked can outlive the call. The leader is still a zombie, so
       * the pgid is still this child's. */
      kill(-pid, SIGKILL);
      int status = 0;
      waitpid(pid, &status, 0);
      reaped = true;
      out->exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
      break;
    }
    if (mono_ms() >= deadline) {
      KBC_LOGW("store git call `%s` timed out after %us; process group killed",
               op, call->timeout_s);
      kill(-pid, SIGKILL);
      int status = 0;
      waitpid(pid, &status, 0);
      timed_out = true;
      reaped = true;
      break;
    }
  }
  /* A bounded final drain: only a process that escaped the group (setsid)
   * can still hold a pipe, and it must not hold the call open. */
  int64_t grace = mono_ms() + 2000;
  for (;;) {
    struct pollfd pfd[2];
    pfd[0].fd = out_pipe[0];
    pfd[1].fd = err_pipe[0];
    pfd[0].events = pfd[1].events = POLLIN;
    pfd[0].revents = pfd[1].revents = 0;
    if (poll(pfd, 2, 50) <= 0) {
      if (mono_ms() >= grace) {
        break;
      }
      continue;
    }
    if ((pfd[0].revents & (POLLIN | POLLHUP)) != 0) {
      sink_drain(&so, out_pipe[0]);
    }
    if ((pfd[1].revents & (POLLIN | POLLHUP)) != 0) {
      sink_drain(&se, err_pipe[0]);
    }
    if (mono_ms() >= grace) {
      break;
    }
  }
  close(out_pipe[0]);
  close(err_pipe[0]);
  argv_free(&av);
  strvec_free(&env);

  out->stdout = so.buf;
  out->stdout_truncated = so.truncated;
  out->timed_out = timed_out;

  /* stderr is redacted here, once, before it can reach a log line, an error
   * string, or a caller. The known literal of THIS call is in the list, so a
   * token with no recognizable shape is still gone. */
  const char *lit = (call->cred != NULL) ? call->cred->secret.bytes : NULL;
  const char *lits[1] = {lit};
  kbc_rs_secrets secrets = {lits, (lit != NULL) ? 1u : 0u};
  kbc_status rst = kbc_rs_redact_bytes(&out->stderr, se.buf.ptr, se.buf.len,
                                       &secrets, KBC_RS_STDERR_CAP);
  kbc_str_free(&se.buf);
  if (rst != KBC_OK) {
    kbc_rs_output_free(out);
    return kbc_err_set(err, KBC_ERR_NOMEM, "out of memory for stderr");
  }
  if (se.truncated) {
    kbc_str_puts(&out->stderr, "\n...[stderr truncated]");
  }
  if (timed_out) {
    kbc_rs_output_free(out);
    return kbc_err_set(err, KBC_ERR_TIMEOUT,
                       "store git `%s` timed out after %us; process group "
                       "killed (class %s): %s",
                       op, call->timeout_s, kbc_rs_class_slug(KBC_RS_CLASS_TIMEOUT),
                       out->stderr.ptr);
  }
  int exit_code = out->exit_code;
  if (out->exit_code != 0 && !call->allow_nonzero) {
    cls = kbc_rs_classify(out->stderr.ptr, out->stderr.len,
                          call->auth == KBC_RS_AUTH_TOKEN ||
                              call->auth == KBC_RS_AUTH_INHERIT);
    char detail[KBC_RS_DETAIL_MAX + 128];
    kbc_str cap;
    kbc_str_init(&cap);
    kbc_rs_redact_bytes(&cap, out->stderr.ptr, out->stderr.len, &secrets,
                        KBC_RS_DETAIL_MAX);
    snprintf(detail, sizeof detail, "%s", cap.ptr);
    kbc_str_free(&cap);
    kbc_status rc = kbc_err_set(err, KBC_ERR_IO,
                                "store git `%s` failed (exit=%d, %s): %s", op,
                                exit_code, kbc_rs_class_slug(cls), detail);
    kbc_rs_output_free(out);
    return rc;
  }
  return KBC_OK;
}

/* The high-level operations. Every one composes its argv from validated
 * atoms only, and none of them can be talked into quoting a shell: there is
 * no shell anywhere between a config value and a git argument. */

static kbc_status run_plain(kbc_rs_git *g, const char *op, const char *const *argv,
                            size_t argc, const char *git_dir, bool allow_nonzero,
                            kbc_rs_output *out, kbc_err *err) {
  kbc_rs_call call;
  memset(&call, 0, sizeof call);
  call.op = op;
  call.argv = argv;
  call.argc = argc;
  call.git_dir = git_dir;
  call.auth = KBC_RS_AUTH_LOCAL_ONLY;
  call.timeout_s = 60;
  call.stdout_cap = 4u * 1024u * 1024u;
  call.allow_nonzero = allow_nonzero;
  return kbc_rs_git_run(g, &call, out, err);
}

kbc_status kbc_rs_git_init_bare(kbc_rs_git *g, const char *abs_dir,
                                kbc_err *err) {
  if (abs_dir == NULL || abs_dir[0] != '/') {
    return kbc_err_set(err, KBC_ERR_INVALID,
                       "a store directory must be an absolute path");
  }
  const char *argv[] = {"init", "--bare", "--quiet", "--template=", abs_dir};
  kbc_rs_output out;
  kbc_status st = run_plain(g, "init", argv, 5, NULL, false, &out, err);
  kbc_rs_output_free(&out);
  return st;
}

kbc_status kbc_rs_git_config_remote(kbc_rs_git *g, const char *git_dir,
                                    const char *remote_name,
                                    const kbc_rs_url *url, kbc_err *err) {
  if (kbc_rs_remote_name_is_valid(remote_name) != KBC_OK) {
    return kbc_err_set(err, KBC_ERR_INVALID, "invalid remote name");
  }
  if (url == NULL) {
    return kbc_err_set(err, KBC_ERR_INVALID, "a remote needs a url");
  }
  /* The key is composed from a validated RemoteName and a static suffix, so
   * it can never carry a value git would read as an option. */
  char key[128];
  const char *suffixes[] = {"url", "pushurl", "tagOpt"};
  const char *values[3];
  char pushurl[64];
  snprintf(pushurl, sizeof pushurl, "kbcode-no-push://refused");
  values[0] = url->raw;
  values[1] = pushurl;
  values[2] = "--no-tags";
  for (size_t i = 0; i < 3; i++) {
    snprintf(key, sizeof key, "remote.%s.%s", remote_name, suffixes[i]);
    const char *argv[] = {"config", "--replace-all", key, values[i]};
    kbc_rs_output out;
    kbc_status st = run_plain(g, "config", argv, 4, git_dir, false, &out, err);
    kbc_rs_output_free(&out);
    if (st != KBC_OK) {
      return st;
    }
  }
  /* No `fetch` line: every store fetch names its refspecs explicitly, and a
   * wildcard refspec would write refs the store never classified. */
  snprintf(key, sizeof key, "remote.%s.fetch", remote_name);
  const char *argv[] = {"config", "--unset-all", key};
  kbc_rs_output out;
  kbc_status st = run_plain(g, "config", argv, 3, git_dir, true, &out, err);
  kbc_rs_output_free(&out);
  return st;
}

/* One line "<oid> <refname>". */
static bool split_oid_ref(const char *line, char *oid, size_t oid_cap,
                          char *ref, size_t ref_cap) {
  const char *sp = strchr(line, ' ');
  if (sp == NULL) {
    return false;
  }
  size_t ol = (size_t)(sp - line);
  if (ol == 0 || ol + 1 >= oid_cap) {
    return false;
  }
  if (snprintf(ref, ref_cap, "%s", sp + 1) >= (int)ref_cap) {
    return false;
  }
  memcpy(oid, line, ol);
  oid[ol] = '\0';
  return true;
}

kbc_status kbc_rs_git_list_refs(kbc_rs_git *g, const char *git_dir,
                                const char *const *prefixes,
                                kbc_strlist *out, kbc_err *err) {
  const char *argv[16];
  size_t argc = 0;
  argv[argc++] = "for-each-ref";
  argv[argc++] = "--format=%(objectname) %(refname)";
  if (prefixes != NULL) {
    for (size_t i = 0; prefixes[i] != NULL && argc < 14; i++) {
      /* A pattern that git could read as an option would make this whole
       * call a way to run a different subcommand. */
      if (strncmp(prefixes[i], "refs/", 5) != 0) {
        return kbc_err_set(err, KBC_ERR_INVALID,
                           "a ref pattern must start with refs/");
      }
      argv[argc++] = prefixes[i];
    }
  }
  argv[argc] = NULL;
  kbc_rs_output cap;
  kbc_status st = run_plain(g, "for-each-ref", argv, argc, git_dir, false, &cap,
                            err);
  if (st != KBC_OK) {
    kbc_rs_output_free(&cap);
    return st;
  }
  const char *p = cap.stdout.ptr;
  while (p != NULL && *p != '\0') {
    const char *nl = strchr(p, '\n');
    size_t len = (nl != NULL) ? (size_t)(nl - p) : strlen(p);
    char line[KBC_RS_REFNAME_MAX + KBC_RS_OID_MAX + 2];
    if (len < sizeof line) {
      memcpy(line, p, len);
      line[len] = '\0';
      char oid[KBC_RS_OID_MAX];
      char ref[KBC_RS_REFNAME_MAX];
      if (split_oid_ref(line, oid, sizeof oid, ref, sizeof ref) &&
          oid_ok(oid) && ref_name_ok(ref)) {
        st = kbc_strlist_push(out, line);
        if (st != KBC_OK) {
          break;
        }
      }
    }
    if (nl == NULL) {
      break;
 }
    p = nl + 1;
  }
  kbc_rs_output_free(&cap);
  if (st != KBC_OK) {
    return kbc_err_set(err, KBC_ERR_NOMEM, "out of memory for a ref list");
  }
  return KBC_OK;
}

kbc_status kbc_rs_git_update_refs(kbc_rs_git *g, const char *git_dir,
                                  const char *tx, size_t tx_len,
                                  kbc_err *err) {
  if (tx == NULL || tx_len == 0) {
    return KBC_OK;
  }
  const char *argv[] = {"update-ref", "--stdin"};
  kbc_rs_call call;
  memset(&call, 0, sizeof call);
  call.op = "update-ref";
  call.argv = argv;
  /* 2, not 3. The array has two elements and `argc` said three, so the
   * dispatcher read one past the end and handed git a garbage third argument
   * — which surfaced as `usage: git update-ref ... -m <rea`, an argv element
   * that was never in this source file. */
  call.argc = 2;
  call.git_dir = git_dir;
  call.auth = KBC_RS_AUTH_LOCAL_ONLY;
  call.timeout_s = 120;
  call.stdin_bytes = tx;
  call.stdin_len = tx_len;
  kbc_rs_output out;
  kbc_status st = kbc_rs_git_run(g, &call, &out, err);
  kbc_rs_output_free(&out);
  return st;
}

/* ------------------------------------------------------------------ cred -- */

const char *kbc_rs_profile_slug(kbc_rs_profile p) {
  switch (p) {
    case KBC_RS_PROFILE_GH_CLI:
      return "gh-cli";
    case KBC_RS_PROFILE_DEPLOY_KEY:
      return "deploy-key";
    case KBC_RS_PROFILE_TOKEN_FILE:
      return "token";
    case KBC_RS_PROFILE_ANONYMOUS:
      return "anonymous";
    case KBC_RS_PROFILE_INHERIT:
      return "inherit";
    case KBC_RS_PROFILE_NONE:
      return "none";
  }
  return "none";
}

const char *kbc_rs_cred_pin_str(kbc_rs_cred_pin p) {
  switch (p) {
    case KBC_RS_PIN_AUTO:
      return "auto";
    case KBC_RS_PIN_GH_CLI:
      return "gh-cli";
    case KBC_RS_PIN_DEPLOY_KEY:
      return "deploy-key";
    case KBC_RS_PIN_TOKEN:
      return "token";
    case KBC_RS_PIN_ANONYMOUS:
      return "anonymous";
    case KBC_RS_PIN_INHERIT:
      return "inherit";
    case KBC_RS_PIN_NONE:
      return "none";
  }
  return "auto";
}

kbc_rs_cred_pin kbc_rs_cred_pin_parse(const char *s, bool *unknown) {
  if (unknown != NULL) {
    *unknown = false;
  }
  if (s == NULL) {
    return KBC_RS_PIN_AUTO;
  }
  while (*s == ' ' || *s == '\t') {
    s++;
  }
  for (const char *p = s; *p != '\0'; p++) {
    if (*p >= 'A' && *p <= 'Z') {
      continue;
    }
    break;
  }
  static const struct {
    const char *name;
    kbc_rs_cred_pin pin;
  } table[] = {
      {"", KBC_RS_PIN_AUTO},           {"auto", KBC_RS_PIN_AUTO},
      {"gh-cli", KBC_RS_PIN_GH_CLI},    {"deploy-key", KBC_RS_PIN_DEPLOY_KEY},
      {"token", KBC_RS_PIN_TOKEN},      {"anonymous", KBC_RS_PIN_ANONYMOUS},
      {"inherit", KBC_RS_PIN_INHERIT},  {"none", KBC_RS_PIN_NONE},
  };
  for (size_t i = 0; i < sizeof table / sizeof table[0]; i++) {
    size_t n = strlen(table[i].name);
    if (strlen(s) != n) {
      continue;
 }
    bool eq = true;
    for (size_t k = 0; k < n; k++) {
      if (tolower((unsigned char)s[k]) != table[i].name[k]) {
 eq = false;
        break;
      }
    }
    if (eq) {
      return table[i].pin;
    }
  }
  if (unknown != NULL) {
    *unknown = true;
  }
  return KBC_RS_PIN_AUTO;
}

void kbc_rs_cred_init(kbc_rs_cred *c) {
  memset(c, 0, sizeof *c);
}

void kbc_rs_cred_free(kbc_rs_cred *c) {
  if (c == NULL) {
    return;
  }
  kbc_rs_secret_wipe(&c->secret);
  memset(c, 0, sizeof *c);
}

bool kbc_rs_cred_has_secret(const kbc_rs_cred *c) {
  return c != NULL && c->secret.bytes != NULL;
}

kbc_status kbc_rs_read_token_file(const char *path, const char *username,
                                  const kbc_rs_url *url, kbc_rs_secret *out,
                                  kbc_rs_class *cls, kbc_err *err) {
  *cls = KBC_RS_CLASS_CREDENTIAL_UNAVAILABLE;
  if (url == NULL || url->proto == KBC_RS_PROTO_FILE) {
    *cls = KBC_RS_CLASS_URL_REJECTED;
    return kbc_err_set(err, KBC_ERR_INVALID,
                       "a token needs an https-capable remote");
  }
  /* open -> fstat the SAME descriptor. A stat-then-open would let the file
   * be swapped between the check and the read, and a symlink would be
   * followed to a file that passes every test below. */
  int fd = open(path, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
  if (fd < 0) {
    return kbc_err_set(err, KBC_ERR_NOTFOUND, "token_file %s: %s", path,
                       strerror(errno));
  }
  struct stat st;
  if (fstat(fd, &st) != 0) {
    close(fd);
    return kbc_err_set(err, KBC_ERR_IO, "token_file %s: %s", path,
                       strerror(errno));
  }
  if (!S_ISREG(st.st_mode)) {
    close(fd);
    return kbc_err_set(err, KBC_ERR_INVALID, "token_file %s is not a file",
                       path);
  }
  if (st.st_uid != geteuid()) {
    close(fd);
    return kbc_err_set(err, KBC_ERR_INVALID,
                       "token_file %s is not owned by the daemon user", path);
  }
  if ((st.st_mode & 0077) != 0) {
    close(fd);
    return kbc_err_set(err, KBC_ERR_INVALID,
                       "token_file %s is not owner-only (0600 or 0400)", path);
  }
  char buf[4097];
  size_t n = 0;
  for (;;) {
    ssize_t r = read(fd, buf + n, sizeof buf - 1 - n);
    if (r < 0) {
      if (errno == EINTR) {
        continue;
      }
      kbc_status s = kbc_err_set(err, KBC_ERR_IO, "token_file %s: %s", path,
                                 strerror(errno));
      wipe(buf, sizeof buf);
      close(fd);
      return s;
    }
    if (r == 0) {
      break;
    }
    n += (size_t)r;
    if (n >= sizeof buf - 1) {
      wipe(buf, sizeof buf);
      close(fd);
      return kbc_err_set(err, KBC_ERR_INVALID,
                         "token_file %s is larger than 4096 bytes", path);
    }
  }
  close(fd);
  buf[n] = '\0';
  kbc_status s = kbc_rs_secret_new(out, buf, err);
  wipe(buf, sizeof buf);
  if (s != KBC_OK) {
    /* Present, readable, owner-only, and REFUSED by validation: the operator
     * configured a credential and it is broken. The ladder must stop here
     * rather than fall through to another identity. */
    *cls = KBC_RS_CLASS_CREDENTIAL_REJECTED;
    return s;
  }
  *cls = KBC_RS_CLASS_CREDENTIAL_UNAVAILABLE;
  (void)username;
  return KBC_OK;
}

static void skip_push(kbc_rs_cred *out, kbc_rs_profile rung, kbc_rs_class cls,
                      const char *reason) {
  if (out->n_skipped >= KBC_RS_MAX_SKIPPED) {
    return;
  }
  kbc_rs_skipped *s = &out->skipped[out->n_skipped++];
  s->rung = rung;
  s->cls = cls;
  snprintf(s->reason, sizeof s->reason, "%s", reason);
}

static bool cred_binds(const kbc_rs_cred_cfg *cfg) {
  /* Either a pinned gh login or a recorded one. With either, the store is
   * BOUND to that account and any failure to produce it stops the ladder:
   * falling through would swap the identity the store fetches as. */
  return (cfg->gh_user != NULL && *cfg->gh_user != '\0') ||
         (cfg->recorded_account != NULL && *cfg->recorded_account != '\0');
}

static kbc_status finish_cred(kbc_rs_cred *out, kbc_rs_profile profile,
                              kbc_rs_auth_kind auth, const char *reason) {
  out->profile = profile;
  out->auth = auth;
  snprintf(out->reason, sizeof out->reason, "%s", reason);
  return KBC_OK;
}

kbc_status kbc_rs_cred_resolve(const kbc_rs_cred_cfg *cfg, const kbc_rs_url *url,
                               const kbc_rs_ladder_probes *probes,
                               kbc_rs_cred *out, kbc_err *err) {
  kbc_rs_cred_init(out);
  if (cfg == NULL || url == NULL || probes == NULL) {
    return kbc_err_set(err, KBC_ERR_INVALID, "credential resolve needs a config, a url and probes");
  }
  kbc_rs_url https;
  bool have_https =
      kbc_rs_url_https_equivalent(url, &https, NULL) == KBC_OK;
  const char *username = (cfg->token_username != NULL &&
                          *cfg->token_username != '\0')
                             ? cfg->token_username
                             : "x-access-token";
  kbc_rs_gh_login gh;
  memset(&gh, 0, sizeof gh);
  kbc_rs_class cls = KBC_RS_CLASS_FAILED;
  bool bound = cred_binds(cfg);

  /* 1. explicit pin: that rung, or an error. Never a fall-through. */
  switch (cfg->pin) {
    case KBC_RS_PIN_GH_CLI: {
      if (!have_https) {
        return kbc_err_set(err, KBC_ERR_UNSUPPORTED,
                           "gh-cli needs an https-capable remote");
 }
      kbc_status st = probes->gh_cli(probes->user, &https, cfg->gh_user,
                                     cfg->recorded_account, &gh, &cls, err);
      if (st != KBC_OK) {
        return st;
 }
      snprintf(out->host, sizeof out->host, "%s", https.authority);
      snprintf(out->username, sizeof out->username, "%s", username);
      snprintf(out->account, sizeof out->account, "%s", gh.account);
      out->broader_than_needed = gh.broader_than_needed;
      out->secret = gh.secret;
      memset(&gh, 0, sizeof gh);
      return finish_cred(out, KBC_RS_PROFILE_GH_CLI, KBC_RS_AUTH_TOKEN,
                         "pinned gh-cli");
    }
    case KBC_RS_PIN_DEPLOY_KEY:
      return kbc_err_set(err, KBC_ERR_UNSUPPORTED,
                         "credential = deploy-key is not available in this build");
    case KBC_RS_PIN_TOKEN: {
      if (cfg->token_file == NULL) {
        return kbc_err_set(err, KBC_ERR_INVALID,
                           "credential = token needs token_file");
      }
      if (!have_https) {
    return kbc_err_set(err, KBC_ERR_UNSUPPORTED,
   "token needs an https-capable remote");
      }
      kbc_rs_secret tok;
      kbc_status st = probes->token_file(probes->user, cfg->token_file,
                                         username, &https, &tok, &cls, err);
      if (st != KBC_OK) {
        return st;
      }
      snprintf(out->host, sizeof out->host, "%s", https.authority);
      snprintf(out->username, sizeof out->username, "%s", username);
      out->secret = tok;
      return finish_cred(out, KBC_RS_PROFILE_TOKEN_FILE, KBC_RS_AUTH_TOKEN,
                         "pinned token_file");
    }
    case KBC_RS_PIN_ANONYMOUS:
      if (!have_https) {
        return kbc_err_set(err, KBC_ERR_UNSUPPORTED,
                           "anonymous needs an https-capable remote");
      }
      kbc_status st = probes->anonymous(probes->user, &https, &cls, err);
      if (st != KBC_OK) {
        return st;
      }
      return finish_cred(out, KBC_RS_PROFILE_ANONYMOUS, KBC_RS_AUTH_ANONYMOUS,
                         "pinned anonymous (probed)");
    case KBC_RS_PIN_INHERIT:
      if (!cfg->allow_inherited_credentials) {
        return kbc_err_set(
            err, KBC_ERR_INVALID,
            "credential = inherit but allow_inherited_credentials = false");
      }
      return finish_cred(out, KBC_RS_PROFILE_INHERIT, KBC_RS_AUTH_INHERIT,
                         "pinned inherit (ambient environment)");
    case KBC_RS_PIN_NONE:
      return finish_cred(out, KBC_RS_PROFILE_NONE, KBC_RS_AUTH_LOCAL_ONLY,
                         "pinned none");
    case KBC_RS_PIN_AUTO:
      break;
  }

  /* 2. gh-cli. */
  if (have_https) {
    kbc_status st = probes->gh_cli(probes->user, &https, cfg->gh_user,
                                   cfg->recorded_account, &gh, &cls, err);
    if (st == KBC_OK) {
      snprintf(out->host, sizeof out->host, "%s", https.authority);
      snprintf(out->username, sizeof out->username, "%s", username);
      snprintf(out->account, sizeof out->account, "%s", gh.account);
      out->broader_than_needed = gh.broader_than_needed;
      out->secret = gh.secret;
      memset(&gh, 0, sizeof gh);
      return finish_cred(out, KBC_RS_PROFILE_GH_CLI, KBC_RS_AUTH_TOKEN,
                         "gh-cli");
    }
    kbc_rs_secret_wipe(&gh.secret);
    memset(&gh, 0, sizeof gh);
    /* An account mismatch is a refusal, not a rung that does not apply. */
    if (cls == KBC_RS_CLASS_CREDENTIAL_ACCOUNT_MISMATCH || bound) {
      return st;
    }
    kbc_str why;
    kbc_str_init(&why);
    kbc_str_puts(&why, kbc_rs_class_slug(cls));
    kbc_str_puts(&why, ": ");
    kbc_str_puts(&why, (err != NULL) ? err->msg : "gh-cli unavailable");
    skip_push(out, KBC_RS_PROFILE_GH_CLI, cls, why.ptr);
    kbc_str_free(&why);
  } else if (bound) {
    return kbc_err_set(err, KBC_ERR_UNSUPPORTED,
                       "a gh account is pinned or recorded but the remote "
                       "has no https form");
  } else {
    skip_push(out, KBC_RS_PROFILE_GH_CLI, KBC_RS_CLASS_URL_REJECTED,
              "gh-cli needs an https-capable remote");
  }

  /* 3. deploy key — not in this build. Recorded, never silently absent. */
  skip_push(out, KBC_RS_PROFILE_DEPLOY_KEY, KBC_RS_CLASS_CREDENTIAL_UNAVAILABLE,
            "deploy keys are not available in this build");

  /* 4. token_file. A file that could not be OPENED is a rung that does not
   * apply; one that was readable and then REFUSED stops the ladder, because
   * falling through would fetch as a different identity. */
  if (cfg->token_file != NULL && have_https) {
    kbc_rs_secret tok;
    memset(&tok, 0, sizeof tok);
    kbc_err probe_err;
    kbc_err_reset(&probe_err);
    kbc_status st = probes->token_file(probes->user, cfg->token_file, username,
                                       &https, &tok, &cls, &probe_err);
    if (st == KBC_OK) {
      snprintf(out->host, sizeof out->host, "%s", https.authority);
      snprintf(out->username, sizeof out->username, "%s", username);
      out->secret = tok;
      return finish_cred(out, KBC_RS_PROFILE_TOKEN_FILE, KBC_RS_AUTH_TOKEN,
                         "token_file");
    }
    kbc_rs_secret_wipe(&tok);
    if (cls == KBC_RS_CLASS_CREDENTIAL_REJECTED) {
      return kbc_err_set(err, KBC_ERR_INVALID, "%s", probe_err.msg);
    }
    skip_push(out, KBC_RS_PROFILE_TOKEN_FILE, cls, probe_err.msg);
  }

  /* 5. anonymous, probed. */
  if (have_https) {
    kbc_err probe_err;
    kbc_err_reset(&probe_err);
    kbc_status st = probes->anonymous(probes->user, &https, &cls, &probe_err);
    if (st == KBC_OK) {
      return finish_cred(out, KBC_RS_PROFILE_ANONYMOUS, KBC_RS_AUTH_ANONYMOUS,
                         "anonymous https (ls-remote probe succeeded)");
    }
    skip_push(out, KBC_RS_PROFILE_ANONYMOUS, cls, probe_err.msg);
  } else {
    skip_push(out, KBC_RS_PROFILE_ANONYMOUS, KBC_RS_CLASS_URL_REJECTED,
              "anonymous needs an https-capable remote");
  }

  /* 6. inherit — only when the operator said so, and amber forever. */
  if (cfg->allow_inherited_credentials) {
    return finish_cred(out, KBC_RS_PROFILE_INHERIT, KBC_RS_AUTH_INHERIT,
                       "inherit (legacy ambient environment)");
  }
  skip_push(out, KBC_RS_PROFILE_INHERIT, KBC_RS_CLASS_NO_CREDENTIALS,
            "allow_inherited_credentials = false");

  /* 7. none. */
  return finish_cred(out, KBC_RS_PROFILE_NONE, KBC_RS_AUTH_LOCAL_ONLY,
                     "no credential rung applies");
}

/* The live probes. `gh` runs with a scrubbed environment: it needs HOME and
 * the XDG dirs to reach its keyring, and it must NOT see GH_TOKEN or
 * GITHUB_TOKEN, because those override the keyring and would make the
 * account check below meaningless. */
static const char *const GH_ENV_PASSTHROUGH[] = {
    "PATH", "HOME", "XDG_CONFIG_HOME", "XDG_DATA_HOME", "XDG_STATE_HOME",
    "XDG_CACHE_HOME", "XDG_RUNTIME_DIR", "DBUS_SESSION_BUS_ADDRESS",
    "GH_CONFIG_DIR", "HTTPS_PROXY", "https_proxy", "HTTP_PROXY", "http_proxy",
    "NO_PROXY", "no_proxy", "SSL_CERT_FILE", "SSL_CERT_DIR",
};

static kbc_status run_scrubbed(const char *prog, const char *const *argv,
                                size_t argc, unsigned timeout_s,
                                kbc_rs_output *out, kbc_err *err) {
  kbc_rs_output_init(out);
  argv_builder av;
  argv_init(&av);
  strvec env;
  strvec_init(&env);
  int op[2] = {-1, -1};
  int ep[2] = {-1, -1};
  kbc_status st = argv_push(&av, prog);
  for (size_t i = 0; st == KBC_OK && i < argc; i++) {
    st = argv_push(&av, argv[i]);
  }
  for (size_t i = 0;
       st == KBC_OK && i < sizeof GH_ENV_PASSTHROUGH / sizeof GH_ENV_PASSTHROUGH[0];
       i++) {
    const char *v = getenv(GH_ENV_PASSTHROUGH[i]);
    if (v != NULL) {
      st = env_set(&env, GH_ENV_PASSTHROUGH[i], v);
    }
  }
  static const char *const fixed[] = {
      "GH_PROMPT_DISABLED=1",   "GH_NO_UPDATE_NOTIFIER=1",
      "GH_NO_EXTENSION_UPDATE_NOTIFIER=1", "GH_SPINNER_DISABLED=1",
      "NO_COLOR=1",             "CLICOLOR=0",
      "LC_ALL=C",               "LANG=C",
  };
  for (size_t i = 0; st == KBC_OK && i < sizeof fixed / sizeof fixed[0]; i++) {
    st = strvec_push(&env, fixed[i]);
  }
  if (st == KBC_OK && pipe(op) != 0) {
    st = kbc_err_set(err, KBC_ERR_IO, "pipe: %s", strerror(errno));
  }
  if (st == KBC_OK && pipe(ep) != 0) {
    st = kbc_err_set(err, KBC_ERR_IO, "pipe: %s", strerror(errno));
  }
  pid_t pid = -1;
  if (st == KBC_OK) {
    int saved = 0;
    st = spawn_child(prog, av.v, env.v, NULL, -1, op[1], ep[1], -1, &pid, &saved);
    if (st != KBC_OK) {
      st = kbc_err_set(err, KBC_ERR_NOTFOUND, "spawn %s: %s", prog,
                       strerror(saved));
    }
  }
  for (int i = 0; i < 2; i++) {
    if (op[i] >= 0) {
      close(op[i]);
    }
    if (ep[i] >= 0) {
      close(ep[i]);
    }
  }
  argv_free(&av);
  strvec_free(&env);
  if (st != KBC_OK) {
    kbc_rs_output_free(out);
    return st;
  }
  fcntl(op[0], F_SETFL, O_NONBLOCK);
  fcntl(ep[0], F_SETFL, O_NONBLOCK);
  capture_sink so = {{0, 0, 0}, 1u << 20, false};
  capture_sink se = {{0, 0, 0}, 16u * 1024u, false};
  kbc_str_init(&so.buf);
  kbc_str_init(&se.buf);
  int64_t deadline = mono_ms() + (int64_t)timeout_s * 1000;
  bool timed_out = false;
  for (;;) {
    struct pollfd pfd[2] = {{op[0], POLLIN, 0}, {ep[0], POLLIN, 0}};
    if (poll(pfd, 2, 50) > 0) {
      if ((pfd[0].revents & (POLLIN | POLLHUP)) != 0) {
        sink_drain(&so, op[0]);
      }
      if ((pfd[1].revents & (POLLIN | POLLHUP)) != 0) {
        sink_drain(&se, ep[0]);
      }
    }
    if (exited_unreaped(pid)) {
 kill(-pid, SIGKILL);
      int status = 0;
      waitpid(pid, &status, 0);
   out->exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
      break;
    }
    if (mono_ms() >= deadline) {
      kill(-pid, SIGKILL);
      int status = 0;
      waitpid(pid, &status, 0);
  timed_out = true;
      break;
    }
  }
  close(op[0]);
  close(ep[0]);
  out->stdout = so.buf;
  out->stderr = se.buf;
  out->timed_out = timed_out;
  if (timed_out) {
    kbc_rs_output_free(out);
    return kbc_err_set(err, KBC_ERR_TIMEOUT, "%s timed out", prog);
  }
  return KBC_OK;
}

static bool gh_login_ok(const char *u) {
  size_t n = strlen(u);
  if (n == 0 || n > 39 || !isalnum((unsigned char)u[0])) {
    return false;
  }
  for (size_t i = 0; i < n; i++) {
    if (!isalnum((unsigned char)u[i]) && u[i] != '-') {
      return false;
    }
  }
  return true;
}

static bool gh_host_ok(const char *h) {
  size_t n = strlen(h);
  if (n == 0 || !isalnum((unsigned char)h[0])) {
    return false;
  }
  for (size_t i = 0; i < n; i++) {
    if (!isalnum((unsigned char)h[i]) && h[i] != '.' && h[i] != '-') {
      return false;
    }
  }
  return true;
}

static bool json_str_field(kbc_arena *a, const kbc_json *obj, const char *key,
                           char *out, size_t cap) {
  const char *v = kbc_json_str(obj, key, NULL);
  if (v == NULL || snprintf(out, cap, "%s", v) >= (int)cap) {
    return false;
  }
  (void)a;
  return true;
}

static kbc_status probe_gh(void *user, const kbc_rs_url *url,
                           const char *pinned, const char *recorded,
                           kbc_rs_gh_login *out, kbc_rs_class *cls,
                           kbc_err *err) {
  (void)user;
  const char *gh_prog = getenv("KBC_RS_GH_PROGRAM");
  if (gh_prog == NULL || *gh_prog == '\0') {
    gh_prog = "gh";
 }
  if (!gh_host_ok(url->host)) {
    *cls = KBC_RS_CLASS_CREDENTIAL_UNAVAILABLE;
    return kbc_err_set(err, KBC_ERR_INVALID, "bad gh hostname");
  }
  const char *status_argv[] = {"auth", "status", "--hostname", url->host,
   "--json", "hosts"};
  kbc_rs_output cap;
  kbc_status st = run_scrubbed(gh_prog, status_argv, 6, 20, &cap, err);
  if (st != KBC_OK) {
    *cls = KBC_RS_CLASS_CREDENTIAL_UNAVAILABLE;
    return st;
  }
  if (cap.timed_out) {
    /* A deadline that fired says nothing about WHICH account gh holds, so it
     * is not read as a statement about the credential. */
    kbc_rs_output_free(&cap);
    *cls = KBC_RS_CLASS_TIMEOUT;
    return kbc_err_set(err, KBC_ERR_TIMEOUT, "gh timed out (keyring locked?)");
  }
  kbc_str detail;
  kbc_str_init(&detail);
  kbc_rs_redact_bytes(&detail, cap.stderr.ptr, cap.stderr.len, NULL, 512);
  if (cap.exit_code != 0) {
    char *low = detail.ptr;
    for (char *p = low; *p != '\0'; p++) {
      *p = (char)tolower((unsigned char)*p);
  }
    bool not_logged_in = strstr(low, "not logged in") != NULL ||
                         strstr(low, "no oauth token") != NULL;
    kbc_str_free(&detail);
    kbc_rs_output_free(&cap);
    *cls = KBC_RS_CLASS_CREDENTIAL_UNAVAILABLE;
    if (not_logged_in) {
      return kbc_err_set(err, KBC_ERR_NOTFOUND, "gh is not logged in to %s",
           url->host);
    }
    return kbc_err_set(err, KBC_ERR_IO, "gh auth status failed");
  }
  kbc_arena *a = kbc_arena_new(64 * 1024);
  if (a == NULL) {
    kbc_str_free(&detail);
    kbc_rs_output_free(&cap);
    *cls = KBC_RS_CLASS_FAILED;
    return kbc_err_set(err, KBC_ERR_NOMEM, "out of memory for gh status");
  }
  kbc_err perr;
  kbc_err_reset(&perr);
  kbc_json *root = kbc_json_parse(a, cap.stdout.ptr, cap.stdout.len, &perr);
  kbc_str_free(&detail);
  /* gh printed an account listing, not a token — but the capture is wiped
   * anyway so a future gh that prints one cannot leave it in our heap. */
  wipe(cap.stdout.ptr, cap.stdout.len);
  kbc_rs_output_free(&cap);
  if (root == NULL) {
    kbc_arena_free(a);
    *cls = KBC_RS_CLASS_CREDENTIAL_UNAVAILABLE;
    return kbc_err_set(err, KBC_ERR_PARSE, "gh auth status: unparseable json");
  }
  const kbc_json *hosts = kbc_json_get(root, "hosts");
  const kbc_json *arr =
      (kbc_json_type_of(hosts) == KBC_JSON_OBJ) ? kbc_json_get(hosts, url->host)
             : NULL;
  char chosen[KBC_RS_ACCOUNT_MAX] = {0};
  char active[KBC_RS_ACCOUNT_MAX] = {0};
  bool have_active = false;
  if (kbc_json_type_of(arr) == KBC_JSON_ARR) {
    for (size_t i = 0; i < kbc_json_len(arr); i++) {
      const kbc_json *acct = kbc_json_at(arr, i);
      char login[KBC_RS_ACCOUNT_MAX];
      if (!json_str_field(a, acct, "login", login, sizeof login) ||
     !gh_login_ok(login)) {
        continue;
      }
      if (kbc_json_bool(acct, "active", false)) {
        snprintf(active, sizeof active, "%s", login);
        have_active = true;
      }
      if (pinned != NULL && *pinned != '\0') {
        if (strcasecmp(login, pinned) == 0) {
          snprintf(chosen, sizeof chosen, "%s", login);
        }
      } else if (recorded != NULL && *recorded != '\0') {
     if (strcasecmp(login, recorded) == 0) {
          snprintf(chosen, sizeof chosen, "%s", login);
   }
      } else if (have_active && strcmp(active, login) == 0) {
        snprintf(chosen, sizeof chosen, "%s", login);
      }
    }
  }
  if (chosen[0] == '\0') {
    kbc_arena_free(a);
    *cls = (pinned != NULL && *pinned != '\0') ||
                   (recorded != NULL && *recorded != '\0')
      ? KBC_RS_CLASS_CREDENTIAL_ACCOUNT_MISMATCH
               : KBC_RS_CLASS_CREDENTIAL_UNAVAILABLE;
    if (*cls == KBC_RS_CLASS_CREDENTIAL_ACCOUNT_MISMATCH) {
      return kbc_err_set(err, KBC_ERR_CONFLICT,
        "credential-account-mismatch on %s: no gh account answers",
  url->host);
    }
    return kbc_err_set(err, KBC_ERR_NOTFOUND, "gh is not logged in to %s",
         url->host);
  }
  if (pinned == NULL && recorded != NULL && *recorded != '\0' && have_active &&
      strcasecmp(active, chosen) != 0) {
    kbc_arena_free(a);
    *cls = KBC_RS_CLASS_CREDENTIAL_ACCOUNT_MISMATCH;
    return kbc_err_set(err, KBC_ERR_CONFLICT,
        "credential-account-mismatch on %s: expected `%s` but `%s` is active",
        url->host, recorded, active);
  }
  /* Read the token bound to the account that was just checked, so there is
   * no switch-in-between window between "who is logged in" and "whose token
   * do we take". */
  const char *tok_argv[] = {"auth", "token", "--hostname", url->host, "--user",
    chosen};
  kbc_arena_free(a);
  kbc_rs_output tok;
  kbc_err terr;
  kbc_err_reset(&terr);
  kbc_status ts = run_scrubbed(gh_prog, tok_argv, 5, 20, &tok, &terr);
  if (ts != KBC_OK) {
    *cls = (ts == KBC_ERR_TIMEOUT) ? KBC_RS_CLASS_TIMEOUT
               : KBC_RS_CLASS_CREDENTIAL_UNAVAILABLE;
    return kbc_err_set(err, ts, "gh auth token: %s", terr.msg);
  }
  if (tok.exit_code != 0) {
    kbc_str why;
    kbc_str_init(&why);
    kbc_rs_redact_bytes(&why, tok.stderr.ptr, tok.stderr.len, NULL, 256);
    kbc_status s = kbc_err_set(err, KBC_ERR_IO, "gh auth token: %s", why.ptr);
    kbc_str_free(&why);
    kbc_rs_output_free(&tok);
    *cls = KBC_RS_CLASS_CREDENTIAL_UNAVAILABLE;
    return s;
  }
  kbc_status s = kbc_rs_secret_new(&out->secret, tok.stdout.ptr, err);
  /* The token is now in the SecretToken; the capture that carried it is
   * wiped before it is freed. */
  wipe(tok.stdout.ptr, tok.stdout.len);
  kbc_rs_output_free(&tok);
  if (s != KBC_OK) {
    *cls = KBC_RS_CLASS_CREDENTIAL_REJECTED;
    return s;
  }
  snprintf(out->account, sizeof out->account, "%s", chosen);
  *cls = KBC_RS_CLASS_CREDENTIAL_UNAVAILABLE;
  return KBC_OK;
}

static kbc_status probe_anonymous(void *user, const kbc_rs_url *url,
                                  kbc_rs_class *cls, kbc_err *err);

static kbc_status probe_token_file(void *user, const char *path,
                                   const char *username, const kbc_rs_url *url,
                                   kbc_rs_secret *out, kbc_rs_class *cls,
                                   kbc_err *err) {
  (void)user;
  return kbc_rs_read_token_file(path, username, url, out, cls, err);
}

kbc_status kbc_rs_ladder_probes_live(const char *gh_prog, void *user,
    kbc_rs_ladder_probes *out) {
  (void)gh_prog;
  out->gh_cli = probe_gh;
  out->token_file = probe_token_file;
  out->anonymous = probe_anonymous;
  out->user = user;
  return KBC_OK;
}

static kbc_status probe_anonymous(void *user, const kbc_rs_url *url,
                                  kbc_rs_class *cls, kbc_err *err) {
  kbc_rs_git *g = (kbc_rs_git *)user;
  if (url->proto != KBC_RS_PROTO_HTTPS) {
    *cls = KBC_RS_CLASS_URL_REJECTED;
    return kbc_err_set(err, KBC_ERR_UNSUPPORTED,
                       "anonymous needs an https remote");
  }
  const char *argv[] = {"ls-remote", url->raw, "HEAD"};
  kbc_rs_call call;
  memset(&call, 0, sizeof call);
  call.op = "ls-remote";
  call.argv = argv;
  call.argc = 3;
  call.auth = KBC_RS_AUTH_ANONYMOUS;
  call.timeout_s = 15;
  call.stdout_cap = 64u * 1024u;
  kbc_rs_output out;
  kbc_status st = kbc_rs_git_run(g, &call, &out, err);
  kbc_rs_output_free(&out);
  if (st == KBC_OK) {
    *cls = KBC_RS_CLASS_CREDENTIAL_UNAVAILABLE;
    return KBC_OK;
  }
  *cls = kbc_rs_classify(err != NULL ? err->msg : "", err != NULL ? strlen(err->msg) : 0, false);
  return st;
}


/* -------------------------------------------------------------- manifest -- */

kbc_status kbc_rs_manifest_write(const char *dir, const char *uuid,
                                 const char *store_key, int64_t created_at,
                                 kbc_err *err) {
  if (dir == NULL || uuid == NULL || store_key == NULL) {
    return kbc_err_set(err, KBC_ERR_INVALID, "manifest needs a dir, a uuid and a key");
  }
  kbc_arena *a = kbc_arena_new(8 * 1024);
  if (a == NULL) {
    return kbc_err_set(err, KBC_ERR_NOMEM, "out of memory for a manifest");
  }
  kbc_json *o = kbc_json_new_obj(a);
  kbc_status st = kbc_json_obj_set(a, o, "schema",
                                   kbc_json_new_str(a, KBC_RS_MANIFEST_SCHEMA));
  if (st == KBC_OK) {
    st = kbc_json_obj_set(a, o, "format", kbc_json_new_num(a, 1));
  }
  if (st == KBC_OK) {
    st = kbc_json_obj_set(a, o, "uuid", kbc_json_new_str(a, uuid));
  }
  if (st == KBC_OK) {
    st = kbc_json_obj_set(a, o, "store_key", kbc_json_new_str(a, store_key));
  }
  if (st == KBC_OK) {
    st = kbc_json_obj_set(a, o, "created_at",
      kbc_json_new_num(a, (double)created_at));
  }
  kbc_str body;
  kbc_str_init(&body);
  if (st == KBC_OK) {
    st = kbc_json_dump(o, &body, true, err);
  }
  if (st == KBC_OK) {
    st = kbc_str_putc(&body, '\n');
  }
  if (st == KBC_OK) {
    char path[KBC_RS_PATH_MAX];
    snprintf(path, sizeof path, "%s/%s", dir, KBC_RS_MANIFEST_NAME);
    st = write_file_sync(path, body.ptr, body.len, err);
  }
  kbc_str_free(&body);
  kbc_arena_free(a);
  return st;
}

kbc_status kbc_rs_manifest_check(const char *dir, const char *uuid,
                                 const char *store_key,
                                 kbc_rs_manifest *out, kbc_err *err) {
  memset(out, 0, sizeof *out);
  if (dir == NULL) {
    return kbc_err_set(err, KBC_ERR_INVALID, "manifest needs a directory");
  }
  struct stat st;
  if (stat(dir, &st) != 0 || !S_ISDIR(st.st_mode)) {
    return kbc_err_set(err, KBC_ERR_NOTFOUND,
                       "store directory %s is missing", dir);
  }
  char path[KBC_RS_PATH_MAX];
  snprintf(path, sizeof path, "%s/%s", dir, KBC_RS_MANIFEST_NAME);
  if (!kbc_path_exists(path)) {
    return kbc_err_set(err, KBC_ERR_NOTFOUND, "store manifest is missing in %s",
                       dir);
  }
  kbc_str body;
  kbc_str_init(&body);
  kbc_status rc = kbc_str_read_file(path, &body, err);
  if (rc != KBC_OK) {
    kbc_str_free(&body);
    return rc;
  }
  kbc_arena *a = kbc_arena_new(16 * 1024);
  if (a == NULL) {
    kbc_str_free(&body);
    return kbc_err_set(err, KBC_ERR_NOMEM, "out of memory for a manifest");
  }
  kbc_err perr;
  kbc_err_reset(&perr);
  kbc_json *o = kbc_json_parse(a, body.ptr, body.len, &perr);
  kbc_str_free(&body);
  if (o == NULL || kbc_json_type_of(o) != KBC_JSON_OBJ) {
    kbc_arena_free(a);
    return kbc_err_set(err, KBC_ERR_PARSE, "store manifest is unreadable");
  }
  snprintf(out->schema, sizeof out->schema, "%s", kbc_json_str(o, "schema", ""));
  out->format = (uint32_t)kbc_json_num(o, "format", 0);
  snprintf(out->uuid, sizeof out->uuid, "%s", kbc_json_str(o, "uuid", ""));
  snprintf(out->store_key, sizeof out->store_key, "%s",
           kbc_json_str(o, "store_key", ""));
  out->created_at = kbc_json_i64(o, "created_at", 0);
  kbc_arena_free(a);
  /* A manifest that disagrees with the row that owns the directory is a
   * restore pointing at the wrong store, or a copied directory. It is never
   * silently adopted. */
  if (strcmp(out->schema, KBC_RS_MANIFEST_SCHEMA) != 0) {
    return kbc_err_set(err, KBC_ERR_CONFLICT,
                       "store manifest schema mismatch (found \"%s\")",
                 out->schema);
  }
  if (uuid != NULL && strcmp(out->uuid, uuid) != 0) {
    return kbc_err_set(err, KBC_ERR_CONFLICT,
 "store manifest uuid mismatch (directory holds \"%s\", row says \"%s\")",
           out->uuid, uuid);
  }
  if (store_key != NULL && strcmp(out->store_key, store_key) != 0) {
    return kbc_err_set(err, KBC_ERR_CONFLICT,
                       "store manifest store_key mismatch (directory holds "
      "\"%s\", row says \"%s\")",
        out->store_key, store_key);
  }
  return KBC_OK;
}

/* ------------------------------------------------------------------ lock -- */

struct kbc_rs_lock {
  int fd;
  pid_t holder;
  char path[KBC_RS_PATH_MAX];
};

/* The holder record is what makes a refusal actionable: "store X is locked"
 * is a question, "store X is locked by pid 4711 since ..." is an answer. */
static void lock_write_holder(kbc_rs_lock *l, const char *uuid) {
  char body[256];
  int n = snprintf(body, sizeof body,
          "kb-review-store-lock/1\nuuid=%s\npid=%ld\nstarted=%lld\n", uuid,
  (long)l->holder, (long long)time(NULL));
  if (n <= 0) {
    return;
  }
  if (ftruncate(l->fd, 0) != 0) {
    return;
  }
  ssize_t w = write(l->fd, body, (size_t)n);
  (void)w;
  (void)fsync(l->fd);
}

kbc_status kbc_rs_lock_acquire(const char *root, const char *uuid,
   kbc_rs_lock **out, kbc_err *err) {
  *out = NULL;
  if (root == NULL || root[0] != '/' || uuid == NULL) {
    return kbc_err_set(err, KBC_ERR_INVALID,
           "a store lock needs an absolute root and a uuid");
  }
  kbc_status rc = kbc_mkdir_p(root, err);
  if (rc != KBC_OK) {
    return rc;
  }
  char path[KBC_RS_PATH_MAX];
  if (snprintf(path, sizeof path, "%s/%s.lock", root, uuid) >= (int)sizeof path) {
 return kbc_err_set(err, KBC_ERR_INVALID, "store lock path is too long");
  }
  int fd = open(path, O_RDWR | O_CREAT | O_CLOEXEC, 0600);
  if (fd < 0) {
    return kbc_err_set(err, KBC_ERR_IO, "open %s: %s", path, strerror(errno));
  }
  if (flock(fd, LOCK_EX | LOCK_NB) != 0) {
    int saved = errno;
    /* Read the holder BEFORE giving up, so the refusal can name it. */
    kbc_str body;
    kbc_str_init(&body);
    if (kbc_str_read_file(path, &body, NULL) == KBC_OK) {
      char *pid = strstr(body.ptr, "pid=");
      if (pid != NULL) {
        pid += 4;
        long v = strtol(pid, NULL, 10);
        kbc_err_set(err, KBC_ERR_CONFLICT,
            "store %s is already locked by pid %ld (%s); refusing rather than "
    "racing the holder",
       uuid, v, path);
      }
    }
    kbc_str_free(&body);
    close(fd);
    if (err == NULL || err->msg[0] == '\0') {
      return kbc_err_set(NULL, KBC_ERR_CONFLICT,
          "store %s is already locked (%s); refusing rather than racing "
    "the holder",
   uuid, path);
    }
    (void)saved;
    return KBC_ERR_CONFLICT;
  }
  kbc_rs_lock *l = calloc(1, sizeof *l);
  if (l == NULL) {
    close(fd);
    return kbc_err_set(err, KBC_ERR_NOMEM, "out of memory for a store lock");
  }
  l->fd = fd;
  l->holder = getpid();
  snprintf(l->path, sizeof l->path, "%s", path);
  lock_write_holder(l, uuid);
  *out = l;
  return KBC_OK;
}

void kbc_rs_lock_release(kbc_rs_lock *l) {
  if (l == NULL) {
    return;
  }
  /* The lock is advisory and lives on the open file description, so closing
   * is the whole release: there is no state elsewhere to unwind. */
  close(l->fd);
  free(l);
}

pid_t kbc_rs_lock_holder(const kbc_rs_lock *l) {
  return (l == NULL) ? -1 : l->holder;
}

/* ------------------------------------------------------------------ seed -- */

/* kb's own store config, appended to a freshly initialised bare repo. Every
 * line is a decision about what a store may do on its own: no hooks, no
 * automatic gc, no automatic maintenance, and unpackLimit=1 so a fetch always
 * lands as a pack instead of a spray of loose objects. */
static kbc_status write_store_config(const char *dir, const char *uuid,
                                     kbc_err *err) {
  kbc_str body;
  kbc_str_init(&body);
  kbc_str_puts(&body,
        "[core]\n\tlogAllRefUpdates = always\n\thooksPath = /dev/null\n"
        "[gc]\n\tauto = 0\n"
        "[maintenance]\n\tauto = false\n"
        "[fetch]\n\tprune = false\n\tunpackLimit = 1\n"
               "[protocol]\n\tversion = 2\n"
      "[uploadpack]\n\tallowAnySHA1InWant = true\n");
  kbc_status st = kbc_str_printf(&body, "[kbcode]\n\tstoreVersion = 1\n"
            "\tstoreUuid = %s\n", uuid);
  char path[KBC_RS_PATH_MAX];
  if (st == KBC_OK) {
    st = set_exact2(path, sizeof path, dir, "/config", err, "store config path");
    if (st == KBC_OK) {
      st = kbc_str_append(&body, "", 0);
    }
  }
  if (st == KBC_OK) {
    /* Append: `git init` already wrote a [core] section we must not lose. */
    int fd = open(path, O_WRONLY | O_APPEND | O_CLOEXEC, 0600);
    if (fd < 0) {
      st = kbc_err_set(err, KBC_ERR_IO, "open %s: %s", path, strerror(errno));
    } else {
      size_t off = 0;
      while (off < body.len) {
        ssize_t w = write(fd, body.ptr + off, body.len - off);
        if (w < 0) {
   if (errno == EINTR) {
       continue;
   }
   st = kbc_err_set(err, KBC_ERR_IO, "write %s: %s", path, strerror(errno));
          break;
      }
        off += (size_t)w;
      }
      if (st == KBC_OK && fsync(fd) != 0) {
        st = kbc_err_set(err, KBC_ERR_IO, "fsync %s: %s", path,
             strerror(errno));
      }
      close(fd);
    }
  }
  if (st == KBC_OK) {
    st = set_exact2(path, sizeof path, dir, "/HEAD", err, "store HEAD path");
    if (st == KBC_OK) {
      st = write_file_sync(path, "ref: refs/kbc/none\n", 19, err);
    }
  }
  kbc_str_free(&body);
  return st;
}

/* A refspec for a head or a tag of the base remote, or false when the name
 * is not one. Every name here came off the wire, so it is validated before
 * it can become an argv atom. */
static bool base_refspec(const char *refname, char *out, size_t cap) {
  const char *tail = NULL;
  const char *dst_prefix = NULL;
  if (strncmp(refname, "refs/heads/", 11) == 0) {
    tail = refname + 11;
    dst_prefix = "refs/remotes/base/";
  } else if (strncmp(refname, "refs/tags/", 10) == 0) {
    tail = refname + 10;
    dst_prefix = "refs/remotes/base/tag/";
  } else {
    return false;
  }
  char full[KBC_RS_REFNAME_MAX];
  int n = snprintf(full, sizeof full, "refs/remotes/%s%s",
        strncmp(refname, "refs/heads/", 11) == 0 ? "base/" : "base/tag/", tail);
  if (n <= 0 || (size_t)n >= sizeof full || !ref_name_ok(full)) {
    return false;
  }
  (void)dst_prefix;
  int m = snprintf(out, cap, "+%s:%s", refname, full);
  return m > 0 && (size_t)m < cap;
}

kbc_status kbc_rs_seed(kbc_rs_git *g, const kbc_rs_seed_plan *plan,
        kbc_rs_seed_report *out, kbc_err *err) {
  memset(out, 0, sizeof *out);
  if (g == NULL || plan == NULL || plan->root == NULL || plan->uuid == NULL ||
      plan->store_key == NULL) {
    return kbc_err_set(err, KBC_ERR_INVALID, "a seed needs a plan");
  }
  int64_t started = kbc_now_ns();
  kbc_status st = kbc_mkdir_p(plan->root, err);
  if (st != KBC_OK) {
    return st;
  }
  char final_dir[KBC_RS_PATH_MAX];
  char tmp[KBC_RS_PATH_MAX];
  if (snprintf(final_dir, sizeof final_dir, "%s/%s.git", plan->root,
               plan->uuid) >= (int)sizeof final_dir ||
      snprintf(tmp, sizeof tmp, "%s/.seed-%s.tmp", plan->root, plan->uuid) >=
          (int)sizeof tmp) {
    return kbc_err_set(err, KBC_ERR_INVALID, "store paths are too long");
  }
  if (kbc_path_exists(final_dir)) {
    return kbc_err_set(err, KBC_ERR_CONFLICT,
        "store directory %s already exists", final_dir);
  }
  /* The lock is taken HERE, by the seeder, for the whole call. Two seeds
   * writing one manifest is how a mirror ends up describing a repo that does
   * not exist, so the second one is refused rather than queued. */
  kbc_rs_lock *lock = NULL;
  st = kbc_rs_lock_acquire(plan->root, plan->uuid, &lock, err);
  if (st != KBC_OK) {
    return st;
  }
  /* From here every failure removes the .tmp: a half-mirror must never be
   * left under the final name, and a leftover .tmp is the only thing the next
   * attempt is allowed to find. */
  st = rm_rf(tmp, err);
  if (st == KBC_OK) {
    st = kbc_rs_git_init_bare(g, tmp, err);
  }
  if (st == KBC_OK) {
    st = write_store_config(tmp, plan->uuid, err);
  }
  if (st == KBC_OK && plan->base != NULL) {
    st = kbc_rs_git_config_remote(g, tmp, "base", plan->base, err);
  }

  kbc_rs_auth_kind auth = KBC_RS_AUTH_LOCAL_ONLY;
  bool may_fetch = false;
  if (plan->base == NULL) {
    out->base_state = KBC_RS_BASE_OFFLINE;
    snprintf(out->base_detail, sizeof out->base_detail, "no base remote");
  } else if (plan->cred != NULL &&
 plan->cred->profile == KBC_RS_PROFILE_NONE) {
    out->base_state = KBC_RS_BASE_NO_CREDENTIAL;
    snprintf(out->base_detail, sizeof out->base_detail,
      "credential profile is none; cached refs only");
  } else if (plan->cred != NULL) {
    auth = plan->cred->auth;
    may_fetch = true;
  } else if (plan->base->proto == KBC_RS_PROTO_FILE) {
    /* A local mirror has no credential to resolve; the file transport is
     * the whole of its authentication. */
    auth = KBC_RS_AUTH_LOCAL_ONLY;
    may_fetch = true;
  } else {
    out->base_state = KBC_RS_BASE_OFFLINE;
    snprintf(out->base_detail, sizeof out->base_detail,
       "no credential was resolved; seeding locally only");
  }

  if (st == KBC_OK && may_fetch) {
    /* A token is only ever offered to its own protocol and host. */
    if (plan->cred != NULL && kbc_rs_cred_has_secret(plan->cred) &&
        strcmp(plan->base->authority, plan->cred->host) != 0) {
      st = kbc_err_set(err, KBC_ERR_INVALID,
         "the resolved credential is scoped to a different host than the base "
  "remote");
    }
  }
  if (st == KBC_OK && may_fetch) {
    const char *argv[] = {"ls-remote", "--heads", "--tags", plan->base->raw};
    kbc_rs_call probe;
    memset(&probe, 0, sizeof probe);
    probe.op = "ls-remote";
    probe.argv = argv;
    probe.argc = 4;
    probe.auth = auth;
    probe.cred = plan->cred;
    probe.timeout_s = (plan->timeout_s != 0) ? plan->timeout_s / 4 + 15 : 60;
    probe.stdout_cap = 8u * 1024u * 1024u;
    kbc_rs_output listing;
    st = kbc_rs_git_run(g, &probe, &listing, err);
    if (st == KBC_OK) {
      kbc_strlist names;
      kbc_strlist_init(&names);
      const char *p = listing.stdout.ptr;
      while (p != NULL && *p != '\0') {
      const char *nl = strchr(p, '\n');
    size_t len = (nl != NULL) ? (size_t)(nl - p) : strlen(p);
        char line[KBC_RS_REFNAME_MAX + KBC_RS_OID_MAX + 2];
        if (len < sizeof line) {
       memcpy(line, p, len);
          line[len] = '\0';
char oid[KBC_RS_OID_MAX];
          char ref[KBC_RS_REFNAME_MAX];
          if (split_oid_ref(line, oid, sizeof oid, ref, sizeof ref) &&
              oid_ok(oid) && ref_name_ok(ref)) {
            char spec[KBC_RS_REFNAME_MAX * 2 + 8];
       if (base_refspec(ref, spec, sizeof spec)) {
              st = kbc_strlist_push(&names, spec);
            } else {
      out->refs_skipped++;
    }
        } else {
            out->refs_skipped++;
          }
        } else {
          out->refs_skipped++;
        }
   if (st != KBC_OK) {
        break;
        }
 if (nl == NULL) {
  break;
        }
  p = nl + 1;
      }
      kbc_rs_output_free(&listing);
      if (st != KBC_OK) {
        kbc_strlist_free(&names);
        st = kbc_err_set(err, KBC_ERR_NOMEM, "out of memory for refspecs");
      } else {
        kbc_status fs = KBC_OK;
        if (names.len > 0) {
          const char **fetch_argv = calloc(names.len + 8, sizeof *fetch_argv);
          if (fetch_argv == NULL) {
            st = kbc_err_set(err, KBC_ERR_NOMEM, "out of memory for a seed");
          } else {
            static const char *const head[] = {
                "fetch",       "--no-tags",  "--no-write-fetch-head",
                "--no-auto-gc", "--no-auto-maintenance", "--quiet",
                "base"};
            kbc_rs_call fetch;
            memset(&fetch, 0, sizeof fetch);
            size_t na = 0;
            for (size_t i = 0; i < sizeof head / sizeof head[0]; i++) {
              fetch_argv[na++] = head[i];
            }
            for (size_t i = 0; i < names.len; i++) {
              fetch_argv[na++] = names.items[i];
            }
            fetch_argv[na] = NULL;
            fetch.op = "fetch";
            fetch.argv = fetch_argv;
            fetch.argc = na;
            fetch.git_dir = tmp;
            fetch.auth = auth;
            fetch.cred = plan->cred;
            fetch.timeout_s = (plan->timeout_s != 0)
                                  ? plan->timeout_s
                                  : KBC_RS_SEED_TIMEOUT_S;
            kbc_rs_output fo;
            fs = kbc_rs_git_run(g, &fetch, &fo, err);
            if (fs == KBC_OK) {
              out->base_state = KBC_RS_BASE_FETCHED;
              out->refs_imported = names.len;
            } else {
              out->base_state = KBC_RS_BASE_FAILED;
              snprintf(out->base_detail, sizeof out->base_detail, "%s",
                       err != NULL ? err->msg : "base fetch failed");
            }
            kbc_rs_output_free(&fo);
            free(fetch_argv);
            st = fs;
          }
        } else {
          out->base_state = KBC_RS_BASE_FETCHED;
          snprintf(out->base_detail, sizeof out->base_detail,
                   "the remote has no heads or tags");
        }
        kbc_strlist_free(&names);
      }
    } else {
      out->base_state = KBC_RS_BASE_FAILED;
      snprintf(out->base_detail, sizeof out->base_detail, "%s",
               err != NULL ? err->msg : "ls-remote failed");
      /* A base fetch that cannot happen is OFFLINE-DEGRADABLE, not fatal: the
       * mirror is still worth having and the next sync retries. */
      st = KBC_OK;
    }
  }
  if (st == KBC_OK) {
    st = kbc_rs_manifest_write(tmp, plan->uuid, plan->store_key,
                               (int64_t)(started / 1000000000), err);
  }
  if (st == KBC_OK) {
    if (rename(tmp, final_dir) != 0) {
      st = kbc_err_set(err, KBC_ERR_IO, "rename %s to %s: %s", tmp, final_dir,
                       strerror(errno));
    }
  }
  if (st != KBC_OK) {
    rm_rf(tmp, NULL);
    kbc_rs_lock_release(lock);
    return st;
  }
  snprintf(out->git_dir, sizeof out->git_dir, "%s", final_dir);
  out->elapsed_ms = (kbc_now_ns() - started) / 1000000;
  kbc_rs_lock_release(lock);
  return KBC_OK;
}

/* ----------------------------------------------------------- base ladder -- */

const char *kbc_rs_base_source_slug(kbc_rs_base_source s) {
  switch (s) {
    case KBC_RS_BASE_EXPLICIT:
      return "explicit";
    case KBC_RS_BASE_CONFIG:
      return "config";
    case KBC_RS_BASE_MEMBER:
      return "member";
    case KBC_RS_BASE_PR_SLUG:
      return "pr-slug";
    case KBC_RS_BASE_GH_RESOLVED:
      return "gh-resolved";
    case KBC_RS_BASE_SINGLE:
      return "single";
    case KBC_RS_BASE_LOCAL:
      return "local";
    case KBC_RS_BASE_AMBIGUOUS:
      return "ambiguous";
  }
  return "ambiguous";
}

/* Classify one remote. Returns false for a remote that is simply not a
 * forge remote (a local path) — that is nothing to report. A remote that is
 * REFUSED as unsafe is reported through `reason`, because dropping it
 * silently is how a repo with a real forge remote ends up keyed as local. */
static bool remote_key(const kbc_rs_remote_info *r, char *key, size_t cap,
                       const char **reason) {
  *reason = NULL;
  if (r->url == NULL || *r->url == '\0') {
    *reason = "the remote has no url";
    return false;
  }
  /* A local path is not a forge project: that is nothing to report, and a
   * repo whose only remote is one legitimately gets a `local:` store. */
  if (r->url[0] == '/' || r->url[0] == '.') {
    return false;
  }
  kbc_rs_url u;
  kbc_err err;
  kbc_err_reset(&err);
  if (kbc_rs_url_parse_remote(r->url, &u, &err) != KBC_OK) {
    *reason = err.msg;
    return false;
  }
  if (kbc_rs_store_key(&u, key, cap, &err) != KBC_OK) {
    *reason = err.msg;
    return false;
  }
  return true;
}

kbc_status kbc_rs_base_ladder_run(const kbc_rs_base_input *in,
                                  kbc_rs_base_ladder *out, kbc_err *err) {
  memset(out, 0, sizeof *out);
  if (in == NULL) {
    return kbc_err_set(err, KBC_ERR_INVALID, "the base ladder needs an input");
  }
  /* Rungs 1 and 2 are operator statements and run before anything derived
   * from the clone: an operator who wrote a base url means it, even if some
   * other remote would also match a store. */
  const char *statements[2] = {in->explicit_url, in->config_url};
  kbc_rs_base_source sources[2] = {KBC_RS_BASE_EXPLICIT, KBC_RS_BASE_CONFIG};
  for (size_t i = 0; i < 2; i++) {
    if (statements[i] == NULL || *statements[i] == '\0') {
      continue;
    }
    kbc_rs_url u;
    if (kbc_rs_url_parse_remote(statements[i], &u, err) != KBC_OK) {
      return KBC_ERR_INVALID;
  }
    if (kbc_rs_store_key(&u, out->store_key, sizeof out->store_key, err) !=
        KBC_OK) {
      return KBC_ERR_INVALID;
    }
    out->source = sources[i];
    snprintf(out->remote, sizeof out->remote, "%s", "(operator)");
    return KBC_OK;
  }
  /* Membership: a remote that normalizes to an EXISTING store joins it. */
  for (size_t i = 0; i < in->n_existing; i++) {
    for (size_t j = 0; j < in->n_remotes; j++) {
      char key[KBC_RS_KEY_MAX];
      const char *why = NULL;
      if (!remote_key(&in->remotes[j], key, sizeof key, &why)) {
        continue;
 }
      if (in->existing[i].member_store_key != NULL &&
          strcmp(in->existing[i].member_store_key, key) == 0) {
        snprintf(out->store_key, sizeof out->store_key, "%s",
   in->existing[i].store_key);
        snprintf(out->remote, sizeof out->remote, "%s", in->remotes[j].name);
        out->source = KBC_RS_BASE_MEMBER;
        return KBC_OK;
      }
    }
  }
  /* pr-slug, matched against the repo's OWN remotes so the host comes from a
   * real remote and never from a guess. */
  if (in->pr_slug != NULL && *in->pr_slug != '\0') {
    for (size_t j = 0; j < in->n_remotes; j++) {
      char key[KBC_RS_KEY_MAX];
      const char *why = NULL;
      if (!remote_key(&in->remotes[j], key, sizeof key, &why)) {
        continue;
      }
      const char *slash = strchr(key, '/');
      if (slash != NULL && strcmp(slash + 1, in->pr_slug) == 0) {
        snprintf(out->store_key, sizeof out->store_key, "%s", key);
        snprintf(out->remote, sizeof out->remote, "%s", in->remotes[j].name);
        out->source = KBC_RS_BASE_PR_SLUG;
        return KBC_OK;
      }
    }
  }
  /* gh-resolved, then the single forge remote. Two remotes naming the SAME
   * project are not ambiguous: they agree, and the first one names it. */
  char keys[2][KBC_RS_KEY_MAX];
  char names[2][64];
  for (size_t pass = 0; pass < 2; pass++) {
    size_t hits = 0;
    for (size_t j = 0; j < in->n_remotes; j++) {
      bool marked = (in->remotes[j].gh_resolved != NULL &&
                     strcmp(in->remotes[j].gh_resolved, "base") == 0);
      if ((pass == 0) != marked) {
        continue;
      }
      char key[KBC_RS_KEY_MAX];
      const char *why = NULL;
      if (!remote_key(&in->remotes[j], key, sizeof key, &why)) {
        /* A refused remote is never silently dropped: "this remote is not a
         * forge URL" and "this remote is not one kb will fetch from" are
         * different answers for the operator. */
        if (why != NULL) {
          out->refused_count++;
          if (out->refused_reason[0] == '\0') {
            snprintf(out->refused_reason, sizeof out->refused_reason, "%s",
                     why);
          }
        }
        continue;
      }
      bool dup = false;
      for (size_t h = 0; h < hits; h++) {
        dup = dup || strcmp(keys[h], key) == 0;
      }
      if (dup) {
        continue;
      }
      if (hits < 2) {
        snprintf(keys[hits], KBC_RS_KEY_MAX, "%s", key);
        snprintf(names[hits], sizeof names[hits], "%s", in->remotes[j].name);
      }
      hits++;
    }
    if (hits == 1) {
      snprintf(out->store_key, sizeof out->store_key, "%s", keys[0]);
      snprintf(out->remote, sizeof out->remote, "%s", names[0]);
      out->source = (pass == 0) ? KBC_RS_BASE_GH_RESOLVED : KBC_RS_BASE_SINGLE;
      return KBC_OK;
    }
    if (hits > 1) {
      out->source = KBC_RS_BASE_AMBIGUOUS;
      return kbc_err_set(err, KBC_ERR_CONFLICT,
                         "base-url-ambiguous: %zu distinct forge projects and "
                         "no statement picks one",
                         hits);
    }
  }
  /* Nothing classified. A refusal is louder than a local store: the operator
   * wrote something this ladder will not use, and that must be said. */
  if (out->refused_count > 0) {
    out->source = KBC_RS_BASE_AMBIGUOUS;
    return kbc_err_set(err, KBC_ERR_CONFLICT,
        "remote-url-refused: no forge remote, and %d remote(s) were refused "
  "(first: %s)",
             out->refused_count, out->refused_reason);
  }
  out->source = KBC_RS_BASE_LOCAL;
  return KBC_OK;
}

/* -------------------------------------------------------------------- gc -- */

static bool contains_i64(const int64_t *v, size_t n, int64_t x) {
  for (size_t i = 0; i < n; i++) {
    if (v[i] == x) {
      return true;
}
  }
  return false;
}

kbc_rs_ref_class kbc_rs_gc_classify_ref(const char *refname,
                      const kbc_rs_gc_keep *keep) {
  kbc_rs_ref_class c;
  memset(&c, 0, sizeof c);
  c.kind = KBC_RS_REF_UNCLASSIFIED;
  if (refname == NULL) {
    return c;
  }
  const int64_t *reviews = (keep != NULL) ? keep->review_ids : NULL;
  size_t n_reviews = (keep != NULL) ? keep->n_review_ids : 0;
  const int64_t *prs = (keep != NULL) ? keep->open_pr_numbers : NULL;
  size_t n_prs = (keep != NULL) ? keep->n_open_pr_numbers : 0;
  const int64_t *members = (keep != NULL) ? keep->member_ids : NULL;
  size_t n_members = (keep != NULL) ? keep->n_member_ids : 0;

  if (strncmp(refname, "refs/kbc/review/", 16) == 0) {
    const char *rest = refname + 16;
    const char *slash = strchr(rest, '/');
    if (slash == NULL) {
      return c;
 }
    char idbuf[24];
    size_t idlen = (size_t)(slash - rest);
    if (idlen == 0 || idlen >= sizeof idbuf) {
      return c;
    }
    memcpy(idbuf, rest, idlen);
    idbuf[idlen] = '\0';
    int64_t id = 0;
    if (!parse_strict_i64(idbuf, &id)) {
      return c;
 }
    const char *ps = slash + 1;
    if (strncmp(ps, "ps", 2) != 0) {
      return c;
    }
    ps += 2;
    if (strncmp(ps, "ps", 2) == 0) {
      return c;
    }
    const char *dash = strstr(ps, "-base");
    char num[24];
    size_t nl = (dash != NULL) ? (size_t)(dash - ps) : strlen(ps);
    if (nl == 0 || nl >= sizeof num || strchr(ps, '/') != NULL) {
      return c;
    }
    memcpy(num, ps, nl);
    num[nl] = '\0';
    int64_t psn = 0;
    if (!parse_strict_i64(num, &psn)) {
      return c;
    }
    if (dash != NULL) {
      if (strcmp(dash, "-base") != 0) {
        return c;
      }
      c.kind = KBC_RS_REF_PATCHSET_BASE;
    } else {
      c.kind = KBC_RS_REF_PATCHSET;
    }
    c.id = id;
    c.bound = contains_i64(reviews, n_reviews, id);
    return c;
  }
  static const struct {
    const char *prefix;
    kbc_rs_ref_kind kind;
  } simple[] = {
      {"refs/kbc/pr/", KBC_RS_REF_PR},
      {"refs/kbc/prm/", KBC_RS_REF_PRM},
  };
  for (size_t i = 0; i < 2; i++) {
    size_t pl = strlen(simple[i].prefix);
    if (strncmp(refname, simple[i].prefix, pl) != 0) {
  continue;
    }
    const char *num = refname + pl;
    if (strchr(num, '/') != NULL) {
      return c;
    }
    int64_t v = 0;
    if (!parse_strict_i64(num, &v)) {
      return c;
    }
    c.kind = simple[i].kind;
    c.id = v;
    c.bound = contains_i64(prs, n_prs, v);
    return c;
  }
  if (strncmp(refname, "refs/kbc/hint/", 14) == 0) {
    const char *rest = refname + 14;
    const char *slash = strchr(rest, '/');
    if (slash == NULL || slash == rest) {
      return c;
    }
    char idbuf[24];
    size_t idlen = (size_t)(slash - rest);
    if (idlen >= sizeof idbuf) {
      return c;
    }
    memcpy(idbuf, rest, idlen);
    idbuf[idlen] = '\0';
    int64_t id = 0;
    if (!parse_strict_i64(idbuf, &id)) {
      return c;
    }
    c.kind = KBC_RS_REF_HINT;
    c.id = id;
    c.bound = contains_i64(members, n_members, id);
    return c;
  }
  if (strncmp(refname, "refs/remotes/work-", 18) == 0) {
    const char *rest = refname + 18;
    const char *slash = strchr(rest, '/');
    if (slash == NULL || slash == rest) {
      return c;
    }
    char idbuf[24];
    size_t idlen = (size_t)(slash - rest);
    if (idlen >= sizeof idbuf) {
      return c;
    }
    memcpy(idbuf, rest, idlen);
    idbuf[idlen] = '\0';
    int64_t id = 0;
    if (!parse_strict_i64(idbuf, &id)) {
      return c;
    }
    const char *branch = slash + 1;
    char full[KBC_RS_REFNAME_MAX];
    int n = snprintf(full, sizeof full, "refs/heads/%s", branch);
    if (n <= 0 || (size_t)n >= sizeof full || !ref_name_ok(full)) {
      return c;
    }
    c.kind = KBC_RS_REF_WORK;
    c.id = id;
    c.branch = branch; /* BORROWED from refname */
    c.bound = contains_i64(members, n_members, id);
    return c;
  }
  /* refs/remotes/base/ is the credentialed base fetch's own namespace and
   * is never a candidate; anything else this scan was not asked to classify
   * is skipped entirely rather than guessed at. */
  return c;
}

void kbc_rs_gc_report_init(kbc_rs_gc_report *r) {
  memset(r, 0, sizeof *r);
}

void kbc_rs_gc_report_free(kbc_rs_gc_report *r) {
  if (r == NULL) {
    return;
  }
  free(r->cands);
  memset(r, 0, sizeof *r);
}

static const char *const GC_PREFIXES[] = {"refs/kbc/", "refs/remotes/work-",
      NULL};

kbc_status kbc_rs_gc_run(kbc_rs_git *g, const char *git_dir,
      const kbc_rs_gc_keep *keep, const char *bundle_dir,
 const char *guard_path, const char *uuid, bool apply,
 kbc_rs_gc_report *out, kbc_err *err) {
  kbc_rs_gc_report_init(out);
  if (g == NULL || git_dir == NULL) {
    return kbc_err_set(err, KBC_ERR_INVALID, "a gc pass needs a git and a store");
  }
  if (apply && guard_path != NULL && uuid != NULL) {
    kbc_rs_guard guard;
    kbc_status gs = kbc_rs_guard_read(guard_path, &guard, err);
    if (gs != KBC_OK) {
 return gs;
    }
    if (kbc_rs_guard_blocks(&guard, uuid)) {
      return kbc_err_set(err, KBC_ERR_CONFLICT,
           "the restore guard is flagged (%s) and store %s has not been "
  "acknowledged; refusing to apply a gc",
    guard.reason, uuid);
    }
  }
  kbc_strlist refs;
  kbc_strlist_init(&refs);
  kbc_status st = kbc_rs_git_list_refs(g, git_dir, GC_PREFIXES, &refs, err);
  if (st != KBC_OK) {
    kbc_strlist_free(&refs);
    return st;
  }
  out->n_scanned = refs.len;
  for (size_t i = 0; i < refs.len; i++) {
    char oid[KBC_RS_OID_MAX];
    char ref[KBC_RS_REFNAME_MAX];
    if (!split_oid_ref(refs.items[i], oid, sizeof oid, ref, sizeof ref)) {
      continue;
    }
    kbc_rs_ref_class c = kbc_rs_gc_classify_ref(ref, keep);
    if (c.kind == KBC_RS_REF_UNCLASSIFIED) {
      out->n_unclassified++;
      continue;
 }
    if (c.bound) {
      out->n_bound++;
      continue;
    }
    out->n_orphan++;
    kbc_rs_gc_candidate *grown =
        realloc(out->cands, (out->n + 1) * sizeof *grown);
    if (grown == NULL) {
      kbc_strlist_free(&refs);
      kbc_rs_gc_report_free(out);
      return kbc_err_set(err, KBC_ERR_NOMEM, "out of memory for gc candidates");
    }
    out->cands = grown;
    snprintf(out->cands[out->n].refname,
             sizeof out->cands[out->n].refname, "%s", ref);
    snprintf(out->cands[out->n].old_oid, sizeof out->cands[out->n].old_oid,
       "%s", oid);
    out->n++;
  }
  kbc_strlist_free(&refs);
  if (!apply || out->n == 0) {
    return KBC_OK;
  }
  if (bundle_dir == NULL) {
    return kbc_err_set(err, KBC_ERR_INVALID,
          "a gc apply needs a bundle directory: no ref is ever deleted by an "
      "apply whose pre-apply bundle did not cover it");
  }
  kbc_status mk = kbc_mkdir_p(bundle_dir, err);
  if (mk != KBC_OK) {
    return mk;
  }
  char dest[KBC_RS_PATH_MAX];
  snprintf(dest, sizeof dest, "%s/store-%s-%lld.bundle", bundle_dir,
    (uuid != NULL) ? uuid : "unknown", (long long)time(NULL));
  st = kbc_rs_backup_write_covering(g, git_dir, dest, out->cands, out->n, err);
  if (st != KBC_OK) {
    return st;
  }
  snprintf(out->bundle, sizeof out->bundle, "%s", dest);
  out->covered_by_bundle = out->n;
  /* One transaction, each delete guarded by the old value this pass
   * observed: git refuses the WHOLE transaction if any guard went stale, so a
   * fetch racing the gc can never be clobbered. */
  kbc_str tx;
  kbc_str_init(&tx);
  for (size_t i = 0; i < out->n; i++) {
    kbc_str_printf(&tx, "delete %s %s\n", out->cands[i].refname,
       out->cands[i].old_oid);
  }
  st = kbc_rs_git_update_refs(g, git_dir, tx.ptr, tx.len, err);
  kbc_str_free(&tx);
  if (st == KBC_OK) {
    out->applied = true;
  }
  return st;
}

kbc_status kbc_rs_gc_daily(kbc_rs_git *g, const char *git_dir, size_t *swept,
   kbc_err *err) {
  if (swept != NULL) {
    *swept = 0;
  }
  const char *argv[] = {"maintenance", "run", "--no-quiet",
 "--task=commit-graph"};
  kbc_rs_output out;
  kbc_status st = run_plain(g, "maintenance", argv, 3, git_dir, false, &out,
         err);
  kbc_rs_output_free(&out);
  return st;
}

kbc_status kbc_rs_gc_weekly(kbc_rs_git *g, const char *git_dir, kbc_err *err) {
  const char *argv[] = {"repack", "--geometric=2", "-d", "--write-midx"};
  kbc_rs_output out;
  kbc_status st = run_plain(g, "repack", argv, 4, git_dir, false, &out, err);
  kbc_rs_output_free(&out);
  return st;
}

kbc_status kbc_rs_gc_monthly(kbc_rs_git *g, const char *git_dir,
       bool allow_expire, kbc_err *err) {
  const char *argv[6];
  size_t n = 0;
  argv[n++] = "repack";
  argv[n++] = "--cruft";
  argv[n++] = "-d";
  char exp[64];
  /* --cruft implies a whole-repo repack, so it can NOT be combined with
   * --geometric: real git refuses that combination outright. */
  snprintf(exp, sizeof exp, "--cruft-expiration=%s",
           allow_expire ? "2.weeks.ago" : "never");
  argv[n++] = exp;
  argv[n] = NULL;
  kbc_rs_output out;
  kbc_status st = run_plain(g, "repack", argv, n, git_dir, false, &out, err);
  kbc_rs_output_free(&out);
  if (st != KBC_OK) {
    return st;
  }
  const char *reflog[] = {"reflog", "expire", "--expire=14.days", "--all"};
  st = run_plain(g, "reflog", reflog, 4, git_dir, false, &out, err);
  kbc_rs_output_free(&out);
  return st;
}

/* --------------------------------------------------------------- backups -- */

static kbc_status bundle_heads(kbc_rs_git *g, const char *git_dir,
                               const kbc_rs_gc_candidate *cover, size_t n_cover,
                               const char *dest, kbc_err *err) {
  kbc_strlist heads;
  kbc_strlist_init(&heads);
  kbc_status st = kbc_rs_git_list_refs(g, git_dir,
             (const char *const[]){"refs/kbc/", NULL}, &heads, err);
  if (st != KBC_OK) {
    kbc_strlist_free(&heads);
    return st;
  }
  for (size_t i = 0; i < n_cover; i++) {
    if (!kbc_strlist_contains(&heads, cover[i].refname)) {
      st = kbc_strlist_push(&heads, cover[i].refname);
      if (st != KBC_OK) {
        break;
      }
    }
  }
  if (st != KBC_OK) {
    kbc_strlist_free(&heads);
    return kbc_err_set(err, KBC_ERR_NOMEM, "out of memory for bundle heads");
  }
  if (heads.len == 0) {
    kbc_strlist_free(&heads);
    return kbc_err_set(err, KBC_ERR_NOTFOUND,
            "the store has no refs/kbc refs to bundle");
  }
  kbc_strlist excl;
  kbc_strlist_init(&excl);
  st = kbc_rs_git_list_refs(g, git_dir,
   (const char *const[]){"refs/remotes/base/", NULL}, &excl, err);
  if (st != KBC_OK) {
    kbc_strlist_free(&heads);
    kbc_strlist_free(&excl);
    return st;
  }
  size_t argc = 0;
  const char **argv = calloc(heads.len + excl.len + 8, sizeof *argv);
  if (argv == NULL) {
    kbc_strlist_free(&heads);
    kbc_strlist_free(&excl);
    return kbc_err_set(err, KBC_ERR_NOMEM, "out of memory for a bundle");
  }
#define BUNDLE_PUSH(s)                             \
  do {                                             \
    if (argv != NULL) {                            \
      argv[argc++] = (s);                          \
    }                                              \
  } while (0)
  BUNDLE_PUSH("bundle");
  BUNDLE_PUSH("create");
  BUNDLE_PUSH("--quiet");
  BUNDLE_PUSH("--end-of-options");
  BUNDLE_PUSH(dest);
  for (size_t i = 0; i < heads.len; i++) {
    BUNDLE_PUSH(heads.items[i]);
  }
  /* refs/remotes/base/ are EXCLUSIONS, never heads: those objects are
   * re-fetchable from the base remote, so a bundle need not carry them. */
  for (size_t i = 0; i < excl.len; i++) {
    char neg[KBC_RS_REFNAME_MAX + 2];
    snprintf(neg, sizeof neg, "^%s", excl.items[i]);
    BUNDLE_PUSH(neg);
  }
#undef BUNDLE_PUSH
  kbc_rs_call call;
  memset(&call, 0, sizeof call);
  call.op = "bundle-create";
  call.argv = argv;
  call.argc = argc;
  call.git_dir = git_dir;
  call.auth = KBC_RS_AUTH_LOCAL_ONLY;
  call.timeout_s = 1800;
  kbc_rs_output out;
  st = kbc_rs_git_run(g, &call, &out, err);
  kbc_rs_output_free(&out);
  free(argv);
  if (st == KBC_OK) {
    /* The manifest records every ref name and the oid it pointed at, even
     * for a ref whose tip needed no new objects because base already carries
     * it — so a restore can be checked against what the bundle claimed. */
    kbc_str man;
    kbc_str_init(&man);
    for (size_t i = 0; i < heads.len; i++) {
      char oid[KBC_RS_OID_MAX];
      char ref[KBC_RS_REFNAME_MAX];
      if (split_oid_ref(heads.items[i], oid, sizeof oid, ref, sizeof ref)) {
        kbc_str_printf(&man, "%s\t%s\n", oid, ref);
      }
    }
    char mpath[KBC_RS_PATH_MAX];
    snprintf(mpath, sizeof mpath, "%s.refs", dest);
    st = write_file_sync(mpath, man.ptr, man.len, err);
    kbc_str_free(&man);
  }
  kbc_strlist_free(&heads);
  kbc_strlist_free(&excl);
  return st;
}

kbc_status kbc_rs_backup_write(kbc_rs_git *g, const char *git_dir,
   const char *dest, kbc_err *err) {
  if (dest == NULL || dest[0] != '/') {
    return kbc_err_set(err, KBC_ERR_INVALID, "a bundle needs an absolute dest");
  }
  return bundle_heads(g, git_dir, NULL, 0, dest, err);
}

kbc_status kbc_rs_backup_write_covering(kbc_rs_git *g, const char *git_dir,
          const char *dest,
          const kbc_rs_gc_candidate *cover, size_t n_cover,
          kbc_err *err) {
  if (dest == NULL || dest[0] != '/') {
    return kbc_err_set(err, KBC_ERR_INVALID, "a bundle needs an absolute dest");
  }
  return bundle_heads(g, git_dir, cover, n_cover, dest, err);
}

kbc_status kbc_rs_backup_prune(const char *backups_dir, const char *uuid,
 size_t keep, kbc_err *err) {
  DIR *d = opendir(backups_dir);
  if (d == NULL) {
    return (errno == ENOENT)
           ? KBC_OK
 : kbc_err_set(err, KBC_ERR_IO, "opendir %s: %s", backups_dir,
    strerror(errno));
  }
  char prefix[KBC_RS_UUID_MAX + 8];
  snprintf(prefix, sizeof prefix, "store-%s-", uuid);
  int64_t *stamps = calloc(256, sizeof *stamps);
  char (*paths)[KBC_RS_PATH_MAX] = calloc(256, sizeof *paths);
  static char scratch_path[KBC_RS_PATH_MAX];
  size_t n = 0;
  if (stamps != NULL && paths != NULL) {
    struct dirent *e;
    while ((e = readdir(d)) != NULL && n < 256) {
      const char *rest = strstr(e->d_name, prefix);
      if (rest == NULL || rest != e->d_name) {
 continue;
      }
      const char *ts = e->d_name + strlen(prefix);
      const char *dot = strstr(ts, ".bundle");
      if (dot == NULL || dot[7] != '\0') {
        continue;
      }
      char num[32];
      size_t tl = (size_t)(dot - ts);
      if (tl == 0 || tl >= sizeof num) {
 continue;
      }
      memcpy(num, ts, tl);
      num[tl] = '\0';
      stamps[n] = (int64_t)strtoll(num, NULL, 10);
      snprintf(paths[n], KBC_RS_PATH_MAX, "%s/%s", backups_dir, e->d_name);
      n++;
    }
    /* Newest first, so `keep` is the newest `keep`. The candidate is copied
     * out before the shift: shifting memcpys over the element being placed
     * is how a sort quietly becomes its own inverse. */
    for (size_t i = 1; i < n; i++) {
      int64_t v = stamps[i];
      char (*path_v)[KBC_RS_PATH_MAX] = &paths[i];
      memcpy(scratch_path, paths[i], sizeof paths[i]);
      size_t j = i;
      while (j > 0 && stamps[j - 1] < v) {
        stamps[j] = stamps[j - 1];
        memcpy(paths[j], paths[j - 1], sizeof paths[j]);
        j--;
      }
      stamps[j] = v;
      memcpy(paths[j], scratch_path, sizeof paths[j]);
      (void)path_v;
    }
    for (size_t i = keep; i < n; i++) {
      char side[KBC_RS_PATH_MAX];
      snprintf(side, sizeof side, "%s.refs", paths[i]);
      (void)unlink(side);
      if (unlink(paths[i]) != 0 && errno != ENOENT) {
        KBC_LOGW("could not remove pruned backup bundle %s: %s", paths[i],
   strerror(errno));
      }
    }
  }
  free(stamps);
  free(paths);
  closedir(d);
  return KBC_OK;
}

kbc_status kbc_rs_bundle_list_heads(kbc_rs_git *g, const char *bundle,
            kbc_strlist *out, kbc_err *err) {
  if (bundle == NULL || bundle[0] != '/') {
    return kbc_err_set(err, KBC_ERR_INVALID,
      "a bundle must be an absolute path");
  }
  const char *argv[] = {"bundle", "list-heads", bundle};
  kbc_rs_output cap;
  kbc_status st = run_plain(g, "bundle-list-heads", argv, 3, NULL, false, &cap,
       err);
  if (st != KBC_OK) {
    kbc_rs_output_free(&cap);
    return st;
  }
  const char *p = cap.stdout.ptr;
  while (p != NULL && *p != '\0') {
    const char *nl = strchr(p, '\n');
    size_t len = (nl != NULL) ? (size_t)(nl - p) : strlen(p);
    char line[KBC_RS_REFNAME_MAX + KBC_RS_OID_MAX + 2];
    if (len < sizeof line) {
      memcpy(line, p, len);
      line[len] = '\0';
      char oid[KBC_RS_OID_MAX];
      char ref[KBC_RS_REFNAME_MAX];
      if (split_oid_ref(line, oid, sizeof oid, ref, sizeof ref) && oid_ok(oid) &&
     ref_name_ok(ref)) {
        st = kbc_strlist_push(out, line);
        if (st != KBC_OK) {
          break;
        }
      }
    }
    if (nl == NULL) {
      break;
    }
    p = nl + 1;
  }
  kbc_rs_output_free(&cap);
  if (st != KBC_OK) {
    return kbc_err_set(err, KBC_ERR_NOMEM, "out of memory for bundle heads");
  }
  return KBC_OK;
}

void kbc_rs_restore_report_init(kbc_rs_restore_report *r) {
  memset(r, 0, sizeof *r);
}

void kbc_rs_restore_report_free(kbc_rs_restore_report *r) {
  if (r == NULL) {
    return;
  }
  free(r->collisions);
  memset(r, 0, sizeof *r);
}

kbc_status kbc_rs_restore(kbc_rs_git *g, const char *bundle,
   const char *git_dir, bool force, kbc_rs_restore_report *out,
      kbc_err *err) {
  kbc_rs_restore_report_init(out);
  if (g == NULL || bundle == NULL || git_dir == NULL) {
    return kbc_err_set(err, KBC_ERR_INVALID,
      "a restore needs a git, a bundle and a destination");
  }
  if (git_dir[0] != '/') {
    return kbc_err_set(err, KBC_ERR_INVALID,
     "a restore destination must be an absolute path");
  }
  snprintf(out->git_dir, sizeof out->git_dir, "%s", git_dir);
  kbc_strlist heads;
  kbc_strlist_init(&heads);
  kbc_status st = kbc_rs_bundle_list_heads(g, bundle, &heads, err);
  if (st != KBC_OK) {
    kbc_strlist_free(&heads);
    return st;
  }
  if (heads.len == 0) {
    kbc_strlist_free(&heads);
    return kbc_err_set(err, KBC_ERR_NOTFOUND,
      "the bundle %s carries no ref this store can restore", bundle);
  }
  if (!kbc_path_exists(git_dir)) {
    st = kbc_rs_git_init_bare(g, git_dir, err);
    if (st != KBC_OK) {
      kbc_strlist_free(&heads);
      return st;
    }
  }
  /* THE GUARD. The bundle's OWN header is the source of truth for what a
   * restore would write, and it is read before anything is touched. */
  kbc_strlist have;
  kbc_strlist_init(&have);
  st = kbc_rs_git_list_refs(g, git_dir, NULL, &have, err);
  if (st != KBC_OK) {
    kbc_strlist_free(&heads);
    kbc_strlist_free(&have);
    return st;
  }
  for (size_t i = 0; i < heads.len; i++) {
    char oid[KBC_RS_OID_MAX];
    char ref[KBC_RS_REFNAME_MAX];
    if (!split_oid_ref(heads.items[i], oid, sizeof oid, ref, sizeof ref)) {
      continue;
    }
    for (size_t j = 0; j < have.len; j++) {
      char have_oid[KBC_RS_OID_MAX];
      char have_ref[KBC_RS_REFNAME_MAX];
      if (!split_oid_ref(have.items[j], have_oid, sizeof have_oid, have_ref,
           sizeof have_ref)) {
        continue;
      }
      /* A ref already holding the SAME object is not a collision: it is the
       * round-trip's success case, and re-fetching it is a no-op. */
      if (strcmp(have_ref, ref) == 0 && strcmp(have_oid, oid) != 0) {
        kbc_rs_restore_collision *grown =
            realloc(out->collisions, (out->n + 1) * sizeof *grown);
        if (grown == NULL) {
          kbc_strlist_free(&heads);
          kbc_strlist_free(&have);
          kbc_rs_restore_report_free(out);
          return kbc_err_set(err, KBC_ERR_NOMEM, "out of memory for a restore");
        }
        out->collisions = grown;
        snprintf(out->collisions[out->n].refname,
                 sizeof out->collisions[out->n].refname, "%s", ref);
      snprintf(out->collisions[out->n].old_oid,
       sizeof out->collisions[out->n].old_oid, "%s", have_oid);
        snprintf(out->collisions[out->n].new_oid,
        sizeof out->collisions[out->n].new_oid, "%s", oid);
        out->n++;
      }
    }
  }
  kbc_strlist_free(&have);
  if (out->n > 0 && !force) {
    kbc_str what;
    kbc_str_init(&what);
    for (size_t i = 0; i < out->n && i < 3; i++) {
      kbc_str_printf(&what, "%s%s at %s would become %s", (i > 0) ? ", " : "",
        out->collisions[i].refname, out->collisions[i].old_oid,
        out->collisions[i].new_oid);
    }
    if (out->n > 3) {
      kbc_str_printf(&what, ", and %zu more", out->n - 3);
    }
    kbc_err_set(err, KBC_ERR_CONFLICT,
"refusing to restore %s: it would overwrite %zu existing ref(s): %s",
bundle, out->n, what.ptr);
    kbc_str_free(&what);
    kbc_strlist_free(&heads);
    kbc_rs_restore_report_free(out);
    return KBC_ERR_CONFLICT;
  }
  /* Fetch BY the refspecs, named one by one from the bundle's own header.
   * `git fetch <bundle>` with no refspec would write only FETCH_HEAD and
   * restore nothing. */
  const char **fargv = calloc(heads.len + 8, sizeof *fargv);
  if (fargv == NULL) {
    kbc_strlist_free(&heads);
    kbc_rs_restore_report_free(out);
    return kbc_err_set(err, KBC_ERR_NOMEM, "out of memory for a restore");
  }
  static const char *const rflags[] = {
      "fetch",       "--no-tags",   "--no-write-fetch-head",
      "--no-auto-gc", "--no-auto-maintenance", "--quiet"};
  size_t na = 0;
  for (size_t i = 0; i < sizeof rflags / sizeof rflags[0]; i++) {
    fargv[na++] = rflags[i];
  }
  size_t first_spec = na;
  for (size_t i = 0; i < heads.len; i++) {
    char oid[KBC_RS_OID_MAX];
    char ref[KBC_RS_REFNAME_MAX];
    if (!split_oid_ref(heads.items[i], oid, sizeof oid, ref, sizeof ref)) {
      continue;
    }
    /* By NAME, not by object id: a bundle advertises its own ref names, and
     * fetching a bare oid from it would need an allowAnySHA1InWant the bundle
     * side does not offer. */
    /* "+ref:ref" is '+' + ref + ':' + ref + NUL = 2*len(ref) + 3 bytes. The
     * old `strlen(ref) + 3` was half of that, so every restore with a real ref
     * name wrote past the end of its own allocation. */
    size_t rn = strlen(ref);
    size_t need = 2u * rn + 3u;
    char *spec = malloc(need);
    if (spec == NULL) {
      st = kbc_err_set(err, KBC_ERR_NOMEM, "out of memory for a restore");
      break;
    }
    snprintf(spec, need, "+%s:%s", ref, ref);
    fargv[na++] = spec;
  }
  fargv[na] = NULL;
  kbc_rs_call call;
  memset(&call, 0, sizeof call);
  call.op = "restore";
  call.argv = fargv;
  call.argc = na;
  call.git_dir = git_dir;
  call.auth = KBC_RS_AUTH_LOCAL_ONLY;
  call.timeout_s = 1800;
  if (st == KBC_OK) {
    kbc_rs_output cap;
    st = kbc_rs_git_run(g, &call, &cap, err);
    kbc_rs_output_free(&cap);
  }
  for (size_t i = first_spec; i < na; i++) {
    free((void *)(uintptr_t)fargv[i]);
  }
  free(fargv);
  kbc_strlist_free(&heads);
  if (st != KBC_OK) {
    kbc_rs_restore_report_free(out);
    return st;
  }
  /* Verify: every head the bundle claimed is now present at that object. A
   * restore that reports success without checking is a claim, not a fact. */
  kbc_strlist want;
  kbc_strlist got;
  kbc_strlist_init(&want);
  kbc_strlist_init(&got);
  st = kbc_rs_bundle_list_heads(g, bundle, &want, err);
  if (st == KBC_OK) {
    st = kbc_rs_git_list_refs(g, git_dir, NULL, &got, err);
  }
  if (st == KBC_OK) {
    for (size_t i = 0; i < want.len; i++) {
      char oid[KBC_RS_OID_MAX];
      char ref[KBC_RS_REFNAME_MAX];
      if (!split_oid_ref(want.items[i], oid, sizeof oid, ref, sizeof ref)) {
        continue;
      }
      bool found = false;
      for (size_t j = 0; j < got.len; j++) {
        char h_oid[KBC_RS_OID_MAX];
        char h_ref[KBC_RS_REFNAME_MAX];
        if (split_oid_ref(got.items[j], h_oid, sizeof h_oid, h_ref,
                          sizeof h_ref) &&
            strcmp(h_ref, ref) == 0 && strcmp(h_oid, oid) == 0) {
          found = true;
          break;
        }
      }
      if (!found) {
        st = kbc_err_set(err, KBC_ERR_IO,
                         "restore did not recreate %s at %s", ref, oid);
        break;
      }
      out->n_restored++;
    }
  }
  kbc_strlist_free(&want);
  kbc_strlist_free(&got);
  if (st != KBC_OK) {
    kbc_rs_restore_report_free(out);
  }
  return st;
}

/* --------------------------------------------------------- restore guard -- */

void kbc_rs_guard_init(kbc_rs_guard *g) {
  memset(g, 0, sizeof *g);
  snprintf(g->schema, sizeof g->schema, "%s", KBC_RS_GUARD_SCHEMA);
}

static kbc_status guard_write(const char *path, const kbc_rs_guard *g,
          kbc_err *err) {
  kbc_arena *a = kbc_arena_new(8 * 1024);
  if (a == NULL) {
    return kbc_err_set(err, KBC_ERR_NOMEM, "out of memory for the guard");
  }
  kbc_json *o = kbc_json_new_obj(a);
  kbc_status st = kbc_json_obj_set(a, o, "schema",
        kbc_json_new_str(a, KBC_RS_GUARD_SCHEMA));
  if (st == KBC_OK) {
    st = kbc_json_obj_set(a, o, "high_water_epoch",
       kbc_json_new_num(a, (double)g->high_water_epoch));
  }
  if (st == KBC_OK) {
    st = kbc_json_obj_set(a, o, "flagged", kbc_json_new_bool(a, g->flagged));
  }
  if (st == KBC_OK) {
    st = kbc_json_obj_set(a, o, "flagged_at",
kbc_json_new_num(a, (double)g->flagged_at));
  }
  if (st == KBC_OK) {
    st = kbc_json_obj_set(a, o, "flagged_reason",
kbc_json_new_str(a, g->reason));
  }
  if (st == KBC_OK) {
    kbc_json *arr = kbc_json_new_arr(a);
    for (size_t i = 0; st == KBC_OK && i < g->n_ack; i++) {
      st = kbc_json_arr_push(a, arr, kbc_json_new_str(a, g->acknowledged[i]));
    }
    if (st == KBC_OK) {
      st = kbc_json_obj_set(a, o, "acknowledged_stores", arr);
    }
  }
  kbc_str body;
  kbc_str_init(&body);
  if (st == KBC_OK) {
    st = kbc_json_dump(o, &body, true, err);
  }
  if (st == KBC_OK) {
    st = kbc_str_write_file_atomic(path, body.ptr, body.len, err);
  }
  kbc_str_free(&body);
  kbc_arena_free(a);
  return st;
}

kbc_status kbc_rs_guard_read(const char *path, kbc_rs_guard *out, kbc_err *err) {
  kbc_rs_guard_init(out);
  if (path == NULL) {
    return kbc_err_set(err, KBC_ERR_INVALID, "the guard needs a path");
  }
  kbc_str body;
  kbc_str_init(&body);
  kbc_status st;
  if (!kbc_path_exists(path)) {
    /* A MISSING file is the honest "nothing has ever run here" first boot. */
    kbc_str_free(&body);
    return KBC_OK;
  }
  st = kbc_str_read_file(path, &body, err);
  if (st != KBC_OK) {
    kbc_str_free(&body);
    /* A file that EXISTS and cannot be read is the opposite: fail CLOSED. */
    out->flagged = true;
    snprintf(out->reason, sizeof out->reason,
             "the restore-guard sentinel is unreadable (%s)", st == KBC_ERR_IO
             ? "io error"
         : "not a readable file");
    return KBC_OK;
  }
  kbc_arena *a = kbc_arena_new(16 * 1024);
  kbc_err perr;
  kbc_err_reset(&perr);
  kbc_json *o = (a != NULL) ? kbc_json_parse(a, body.ptr, body.len, &perr) : NULL;
  kbc_str_free(&body);
  if (o == NULL || kbc_json_type_of(o) != KBC_JSON_OBJ) {
    kbc_arena_free(a);
    out->flagged = true;
    snprintf(out->reason, sizeof out->reason,
       "the restore-guard sentinel is corrupt; treating it as FLAGGED");
    return KBC_OK;
  }
  snprintf(out->schema, sizeof out->schema, "%s", kbc_json_str(o, "schema", ""));
  out->high_water_epoch = kbc_json_i64(o, "high_water_epoch", 0);
  out->has_epoch = kbc_json_get(o, "high_water_epoch") != NULL;
  out->flagged = kbc_json_bool(o, "flagged", false);
  out->flagged_at = kbc_json_i64(o, "flagged_at", 0);
  snprintf(out->reason, sizeof out->reason, "%s",
    kbc_json_str(o, "flagged_reason", ""));
  const kbc_json *ack = kbc_json_get(o, "acknowledged_stores");
  if (kbc_json_type_of(ack) == KBC_JSON_ARR) {
    for (size_t i = 0; i < kbc_json_len(ack) && out->n_ack < KBC_RS_GUARD_MAX_ACK;
         i++) {
      const kbc_json *v = kbc_json_at(ack, i);
      if (kbc_json_is(v, KBC_JSON_STR) && v->u.str.len < KBC_RS_UUID_MAX) {
        snprintf(out->acknowledged[out->n_ack], KBC_RS_UUID_MAX, "%.*s",
      (int)v->u.str.len, v->u.str.ptr);
        out->n_ack++;
      }
    }
  }
  kbc_arena_free(a);
  return KBC_OK;
}

kbc_status kbc_rs_guard_observe_epoch(const char *path, bool has_epoch,
             int64_t epoch, int64_t now, kbc_rs_guard *out, kbc_err *err) {
  kbc_status st = kbc_rs_guard_read(path, out, err);
  if (st != KBC_OK) {
    return st;
  }
  out->just_flagged = false;
  if (has_epoch) {
    if (out->has_epoch && epoch < out->high_water_epoch) {
      /* The volume's epoch went BACKWARD. The only known cause is a restore
     * of an older snapshot, and a restored database plus a store full of
 * *newer* refs is exactly the state a gc apply would destroy. */
      if (!out->flagged) {
        out->flagged = true;
        out->flagged_at = now;
        snprintf(out->reason, sizeof out->reason,
      "volume epoch regressed (%lld < previously observed %lld)",
      (long long)epoch, (long long)out->high_water_epoch);
        out->n_ack = 0;
        out->just_flagged = true;
      }
    } else if (!out->has_epoch || epoch > out->high_water_epoch) {
      out->has_epoch = true;
      out->high_water_epoch = epoch;
    }
  }
  return guard_write(path, out, err);
}

kbc_status kbc_rs_guard_acknowledge(const char *path, const char *uuid,
          int64_t now, kbc_err *err) {
  kbc_rs_guard g;
  kbc_status st = kbc_rs_guard_read(path, &g, err);
  if (st != KBC_OK) {
    return st;
  }
  if (!g.flagged) {
    return KBC_OK;
  }
  for (size_t i = 0; i < g.n_ack; i++) {
    if (strcmp(g.acknowledged[i], uuid) == 0) {
      return KBC_OK;
    }
  }
  if (g.n_ack >= KBC_RS_GUARD_MAX_ACK) {
    return kbc_err_set(err, KBC_ERR_CONFLICT,
         "the restore guard already holds %zu acknowledgements", g.n_ack);
  }
  /* Per-STORE: acknowledging one store never clears the flag for another
   * store nobody has looked at yet. */
  snprintf(g.acknowledged[g.n_ack], KBC_RS_UUID_MAX, "%s", uuid);
  g.n_ack++;
  (void)now;
  return guard_write(path, &g, err);
}

bool kbc_rs_guard_blocks(const kbc_rs_guard *g, const char *uuid) {
  if (g == NULL || !g->flagged) {
    return false;
  }
  for (size_t i = 0; i < g->n_ack; i++) {
    if (uuid != NULL && strcmp(g->acknowledged[i], uuid) == 0) {
      return false;
    }
  }
  return true;
}
