/* main.c — the `kbc` binary.
 *
 * argv parsing, a small blocking HTTP client, and printing. Every decision
 * that is not "which flag did the user type" belongs in libkbc; this file is
 * deliberately free of retrieval, storage and config *policy*.
 *
 * Exit codes are the CLI's contract with a shell:
 *   0  success
 *   1  user error      — bad flag, missing argument, nothing found
 *   2  daemon/IO error — the daemon is unreachable or answered 5xx
 */

/* glibc hides the POSIX surface under -std=c17 (CMAKE_C_EXTENSIONS OFF). */
#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE 1

#include <stdio.h>

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>
#include <sqlite3.h>
#include <sys/wait.h>

#include "kbc/app.h"
#include "kbc/config.h"
#include "kbc/httpd.h"
#include "kbc/index.h"
#include "kbc/json.h"
#include "kbc/kbc.h"
#include "kbc/log.h"
#include "kbc/mem.h"
#include "kbc/parse.h"
#include "kbc/search.h"
#include "kbc/store.h"
#include "kbc/types.h"

#define EXIT_OK 0
#define EXIT_USER 1
#define EXIT_DAEMON 2

/* A corpus name is the Rust's KbName: 1..64 bytes of [a-z0-9_-]. Filesystem-
 * safe and subdomain-safe, and the reason a corpus name can never be a path
 * segment that escapes, nor a tar option once the argv carries `--`. */
#define KBC_MAX_KB_NAME 64u

/* EXIT CODES ARE THIS BINARY'S OWN, and every verb — `backup` and `restore`
 * included — uses the same three. The Rust CLI splits them the other way
 * (0 success, 1 any failure, 2 a usage error), and this port deliberately does
 * NOT reproduce that split:
 *
 *   - the Rust's 2 is clap's built-in default for a usage error, not a
 *     designed contract, and the Rust's 1 is what `Termination` does with a
 *     `Result::Err`. Nothing in the kb repo CHOOSES those numbers; they are
 *     what falls out of clap plus the stdlib.
 *   - reproducing them would make `kbc backup` exit 2 where `kbc search`
 *     exits 1 for the very same class of mistake — a user error — in the SAME
 *     binary. An operator (or script) branching on the exit status would have
 *     to know which verb it ran. That inconsistency inside one program is a
 *     worse defect than diverging from a default nobody reads.
 *   - kb-c is a new port with no installed callers to break.
 *
 * So: 1 = user error / usage, 2 = daemon or IO failure, 0 = success. Classify
 * through the shared helpers — die_user, die_io, fail_status — and never with
 * a per-verb code: a variable holding the code for one verb, written at run
 * time and read by a shared helper, is mutable global state (AGENTS.md rule 5)
 * and it is how the split came back in the first place. */

/* A copy buffer is a working set, not a cache: 64 KiB is the page size the
 * kernel reads anyway, and a bigger one only buys faults. */
#define COPY_BUF_BYTES 65536u
/* Only `tar -tzf` wants tar's stdout, and only to look for one member name in
 * it. A listing past this cap is not a tarball these verbs wrote. */
#define TAR_LIST_MAX (8u * 1024u * 1024u)

/* The routes this client speaks; they must match KBC_ROUTES in src/httpd.c. */

#define R_SEARCH "/api/search"
#define R_ARTIFACTS "/api/artifacts"
#define R_REINDEX "/api/reindex"
#define R_STATS "/api/stats" /* the counters `kbc status` prints */
/* Prometheus text exposition 0.0.4. TOP-LEVEL, not under /api — the
 * original mounts it on its own router (router.rs:1003-1025) so it is
 * neither counted nor rate-limited as an API request, and the placement is
 * load-bearing: under /api it would inherit the bearer. */
#define R_METRICS "/metrics"

/* `kbc prune` retention windows, in days. The original's `[retention]`
 * windows are `Option<u32>` and a set one arms a daily background prune
 * (kb-core/src/config.rs:454-536). A CLI verb is the operator-triggered
 * equivalent, and a day window is the same unit. The ceiling is a
 * deliberate overflow guard, not a policy: 36500 days is a century, past
 * which `days * 86400` is not worth computing in an int and no operator
 * meant it. */
#define PRUNE_MAX_DAYS 36500u

#define HTTP_TIMEOUT_SEC 10
#define HTTP_MAX_RESPONSE (8u * 1024u * 1024u)
#define BENCH_DEFAULT_QUERIES 20u
#define BENCH_DEFAULT_REPEATS 20u
#define TOKEN_BYTES 32u

/* ------------------------------------------------------------------ state -- */

static bool g_json;               /* --json */
static const char *g_daemon_flag; /* --daemon URL */
static const char *g_config_flag; /* --config PATH */
static kbc_config *g_cfg;         /* loaded once, on first use */
static char *g_base_url;          /* resolved daemon base URL */

/* --------------------------------------------------------------- messages -- */

static void say_err(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void say_err(const char *fmt, ...) {
  if (g_json) {
    return; /* --json promises an empty stderr */
  }
  va_list ap;
  va_start(ap, fmt);
  fputs("kbc: ", stderr);
  vfprintf(stderr, fmt, ap);
  fputc('\n', stderr);
  va_end(ap);
}

/* The one JSON object --json promises. Every exit path funnels through here
 * so the shape is identical whether the run succeeded or not. `error` stays
 * the machine-readable status; `reason` is the human text naming the value
 * that failed, and is omitted when there is nothing to add. */
static void json_error2(const char *error, const char *reason) {
  kbc_str s;
  kbc_str_init(&s);
  (void)kbc_str_puts(&s, "{\"ok\":false,\"error\":");
  (void)kbc_str_append_json_string(&s, error, strlen(error));
  if (reason != NULL && reason[0] != '\0') {
    (void)kbc_str_puts(&s, ",\"reason\":");
    (void)kbc_str_append_json_string(&s, reason, strlen(reason));
  }
  (void)kbc_str_puts(&s, "}");
  fwrite(s.ptr, 1, s.len, stdout);
  fputc('\n', stdout);
  kbc_str_free(&s);
}

static void json_error(const char *msg) { json_error2(msg, NULL); }

static void json_errorf(const char *fmt, ...)
    __attribute__((format(printf, 1, 2)));
static void json_errorf(const char *fmt, ...) {
  char buf[KBC_ERR_MSG_MAX];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(buf, sizeof buf, fmt, ap);
  va_end(ap);
  json_error(buf);
}

/* A missing thing the user named is a user error; anything the daemon or the
 * filesystem refused is an IO error. `e` is only trusted when it describes
 * this very status: a caller that synthesises a status from an earlier
 * `kbc_err` would otherwise print a stale reason. */
static int fail_status(kbc_status s, const kbc_err *e, const char *what,
                       const char *hint) __attribute__((noreturn));

static int die_user(const char *fmt, ...)
    __attribute__((format(printf, 1, 2), noreturn));
static int die_user(const char *fmt, ...) {
  char buf[KBC_ERR_MSG_MAX];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(buf, sizeof buf, fmt, ap);
  va_end(ap);
  json_error(buf);
  say_err("%s", buf);
  exit(EXIT_USER); /* unconditional: see the exit-code contract above */
}

/* A printed line, with the operator-supplied parts escaped. The repo's one
 * escaping helper is kbc_json_escape; it covers every byte below 0x20 —
 * ESC included, so a corpus name cannot drive the operator's terminal or forge
 * a second line of output. */
static void put_shown(kbc_str *s, const char *v) {
  if (kbc_failed(kbc_json_escape(s, v, strlen(v)))) {
    die_user("%s", "out of memory escaping a value for output");
  }
}

/* Appends into a line being built. Every failure here is OOM-or-nothing, and a
 * truncated line is worse than a dead process, so this exits rather than
 * casting the status away. */
static void line_puts(kbc_str *s, const char *v) {
  if (kbc_failed(kbc_str_puts(s, v))) {
    die_user("%s", "out of memory building an output line");
  }
}

/* One stdout line, newline included, buffer released. */
static void emit_line(kbc_str *s) {
  fwrite(s->ptr, 1, s->len, stdout);
  fputc('\n', stdout);
  kbc_str_free(s);
}

static int die_io(const char *fmt, ...)
    __attribute__((format(printf, 1, 2), noreturn));
static int die_io(const char *fmt, ...) {
  char buf[KBC_ERR_MSG_MAX];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(buf, sizeof buf, fmt, ap);
  va_end(ap);
  json_error(buf);
  say_err("%s", buf);
  exit(EXIT_DAEMON);
}

/* A missing thing the user named is a user error; anything the daemon or the
 * filesystem refused is an IO error. */
static int status_exit(kbc_status s, const kbc_err *e, const char *what) {
  if (s == KBC_OK) {
    return EXIT_OK;
  }
  fail_status(s, e, what, NULL);
}

static int fail_status(kbc_status s, const kbc_err *e, const char *what,
                       const char *hint) {
  const char *reason =
      (e != NULL && e->status == s && e->msg[0] != '\0') ? e->msg : NULL;
  char error[KBC_ERR_MSG_MAX];
  snprintf(error, sizeof error, "%s: %s", what, kbc_status_str(s));
  /* `what` may be a path, the reason a full errno string: the human line is
   * the one place the two are shown together, so it gets room for both. */
  char human[2 * KBC_ERR_MSG_MAX + 64];
  if (reason != NULL) {
    snprintf(human, sizeof human, "%s: %s%s%s", what, reason,
             hint != NULL ? " " : "", hint != NULL ? hint : "");
  } else if (hint != NULL) {
    snprintf(human, sizeof human, "%s: %s %s", what, kbc_status_str(s), hint);
  } else {
    snprintf(human, sizeof human, "%s", error);
  }
  json_error2(error, reason);
  say_err("%s", human);
  exit((s == KBC_ERR_INVALID || s == KBC_ERR_NOTFOUND || s == KBC_ERR_PARSE)
           ? EXIT_USER
           : EXIT_DAEMON);
}

/* ------------------------------------------------------------- path utils -- */

static char *xstrdup(const char *s) {
  char *p = strdup(s);
  if (p == NULL) {
    die_io("%s", "out of memory");
  }
  return p;
}

static char *path_join(const char *a, const char *b) {
  size_t la = strlen(a);
  while (la > 1 && a[la - 1] == '/') {
    la--;
  }
  size_t lb = strlen(b);
  char *out = malloc(la + lb + 2);
  if (out == NULL) {
    die_io("%s", "out of memory");
  }
  memcpy(out, a, la);
  size_t at = la;
  if (at == 0 || out[at - 1] != '/') {
    out[at++] = '/';
  }
  memcpy(out + at, b, lb);
  out[at + lb] = '\0';
  return out;
}

static const char *env_or_null(const char *name) {
  const char *v = getenv(name);
  return (v != NULL && v[0] != '\0') ? v : NULL;
}

/* `~/...` is expanded here too: a quoted argument or a cron invocation never
 * sees the shell's tilde expansion. */
static char *expand_tilde(const char *p) {
  if (p[0] != '~') {
    return xstrdup(p);
  }
  if (p[1] != '\0' && p[1] != '/') {
    return xstrdup(p); /* ~user is not this binary's business */
  }
  const char *home = env_or_null("HOME");
  if (home == NULL) {
    return xstrdup(p);
  }
  return path_join(home, p[1] == '/' ? p + 2 : "");
}

static char *dir_of(const char *file) {
  const char *slash = strrchr(file, '/');
  if (slash == NULL) {
    return xstrdup(".");
  }
  if (slash == file) {
    return xstrdup("/");
  }
  size_t n = (size_t)(slash - file);
  char *out = malloc(n + 1);
  if (out == NULL) {
    die_io("%s", "out of memory");
  }
  memcpy(out, file, n);
  out[n] = '\0';
  return out;
}

static char *config_dir(void) {
  const char *xdg = env_or_null("XDG_CONFIG_HOME");
  if (xdg != NULL) {
    return path_join(xdg, "kb");
  }
  const char *home = env_or_null("HOME");
  if (home == NULL) {
    return NULL;
  }
  return path_join(home, ".config/kb");
}

/* --config > $KBC_CONFIG_PATH > $XDG_CONFIG_HOME/kb/kb.toml >
 * ~/.config/kb/kb.toml. NULL means "no config file"; the defaults still load. */
static char *resolve_config_path(void) {
  if (g_config_flag != NULL) {
    return xstrdup(g_config_flag);
  }
  const char *env = env_or_null("KBC_CONFIG_PATH");
  if (env != NULL) {
    return xstrdup(env);
  }
  char *dir = config_dir();
  if (dir == NULL) {
    return NULL;
  }
  char *p = path_join(dir, "kb.toml");
  free(dir);
  return p;
}

/* --------------------------------------------------------------- config ---- */

static kbc_config *load_config(void) {
  if (g_cfg != NULL) {
    return g_cfg;
  }
  kbc_err e;
  kbc_err_reset(&e);
  g_cfg = kbc_config_defaults();
  if (g_cfg == NULL) {
    die_io("%s", "out of memory building the default config");
  }
  char *path = resolve_config_path();
  if (path == NULL) {
    return g_cfg;
  }
  kbc_status s = kbc_config_load_file(g_cfg, path, &e);
  free(path);
  if (kbc_failed(s)) {
    exit(status_exit(s, &e, "config"));
  }
  return g_cfg;
}

static void free_config(void) {
  if (g_cfg != NULL) {
    kbc_config_free(g_cfg);
    g_cfg = NULL;
  }
  free(g_base_url);
  g_base_url = NULL;
}

/* ------------------------------------------------- url encoding / parsing -- */

/* Unreserved per RFC 3986 stays; everything else — including ' ', '&', '=',
 * '#', '%', '+' and every byte outside 0x21..0x7E — is %XX. A query holding an
 * unencoded '&' would truncate the request, which is the whole point. */
static void url_encode(kbc_str *out, const char *s) {
  static const char hex[] = "0123456789ABCDEF";
  for (const unsigned char *p = (const unsigned char *)s; *p != '\0'; p++) {
    unsigned char c = *p;
    bool plain = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                 (c >= '0' && c <= '9') || c == '-' || c == '.' || c == '_' ||
                 c == '~';
    if (plain) {
      (void)kbc_str_putc(out, (char)c);
    } else {
      char esc[3] = { '%', hex[c >> 4], hex[c & 0x0Fu] };
      (void)kbc_str_append(out, esc, 3);
    }
  }
}

typedef struct {
  char *host;
  char *port;
  char *prefix; /* path prefix of the base URL, "" or "/something" */
} kbc_url;

static void url_free(kbc_url *u) {
  free(u->host);
  free(u->port);
  free(u->prefix);
  u->host = NULL;
  u->port = NULL;
  u->prefix = NULL;
}

static bool url_parse(const char *url, kbc_url *out, kbc_err *err) {
  memset(out, 0, sizeof *out);
  if (strncmp(url, "http://", 7) != 0) {
    kbc_err_set(err, KBC_ERR_INVALID, "daemon url %s: only http:// is supported", url);
    return false;
  }
  const char *rest = url + 7;
  const char *slash = strchr(rest, '/');
  size_t hlen = slash != NULL ? (size_t)(slash - rest) : strlen(rest);
  if (hlen == 0) {
    kbc_err_set(err, KBC_ERR_INVALID, "daemon url %s: no host", url);
    return false;
  }
  /* An IPv6 literal is [::1]:4317 — only a colon after the closing bracket
   * separates the port. */
  const char *port_at = NULL;
  if (rest[0] == '[') {
    const char *close = memchr(rest, ']', hlen);
    if (close == NULL) {
      kbc_err_set(err, KBC_ERR_INVALID, "daemon url %s: unterminated [", url);
      return false;
    }
    if (close + 1 < rest + hlen && close[1] == ':') {
      port_at = close + 1;
    }
  } else {
    const char *colon = memchr(rest, ':', hlen);
    if (colon != NULL) {
      port_at = colon;
    }
  }
  if (port_at != NULL) {
    out->host = strndup(rest, (size_t)(port_at - rest));
    out->port = xstrdup(port_at + 1);
    for (const char *p = out->port; *p != '\0'; p++) {
      if (!isdigit((unsigned char)*p)) {
        kbc_err_set(err, KBC_ERR_INVALID, "daemon url %s: port %s is not a number",
                    url, out->port);
        url_free(out);
        return false;
      }
    }
  } else {
    out->host = strndup(rest, hlen);
    out->port = xstrdup("80");
  }
  if (out->host == NULL) {
    url_free(out);
    kbc_err_set(err, KBC_ERR_NOMEM, "daemon url %s: out of memory", url);
    return false;
  }
  if (slash != NULL) {
    size_t plen = strlen(slash);
    while (plen > 1 && slash[plen - 1] == '/') {
      plen--;
    }
    out->prefix = strndup(slash, plen);
    if (out->prefix == NULL) {
      url_free(out);
      kbc_err_set(err, KBC_ERR_NOMEM, "daemon url %s: out of memory", url);
      return false;
    }
  } else {
    out->prefix = xstrdup("");
  }
  return true;
}

/* ~/.config/kb/daemons.toml's [daemon] entry: one key, endpoint or url.
 * A file we cannot read falls through to the loopback default rather than
 * failing the command — a stale fleet file must not break `kbc status`. */
static char *daemons_toml_endpoint(void) {
  char *dir = config_dir();
  if (dir == NULL) {
    return NULL;
  }
  char *path = path_join(dir, "daemons.toml");
  free(dir);
  kbc_err e;
  kbc_err_reset(&e);
  kbc_str body;
  kbc_str_init(&body);
  char *out = NULL;
  if (kbc_failed(kbc_str_read_file(path, &body, &e))) {
    goto done;
  }
  {
    bool in_daemon = false;
    size_t pos = 0;
    while (pos < body.len) {
      size_t end = pos;
      while (end < body.len && body.ptr[end] != '\n') {
        end++;
      }
      const char *line = body.ptr + pos;
      size_t llen = end - pos;
      if (llen > 0 && line[llen - 1] == '\r') {
        llen--;
      }
      if (llen > 0 && line[0] == '[') {
        in_daemon = (llen == 8 && strncmp(line, "[daemon]", 8) == 0);
      } else if (in_daemon) {
        const char *eq = memchr(line, '=', llen);
        if (eq != NULL) {
          size_t klen = (size_t)(eq - line);
          while (klen > 0 && isspace((unsigned char)line[klen - 1])) {
            klen--;
          }
          bool is_key = (klen == 8 && strncmp(line, "endpoint", 8) == 0) ||
                        (klen == 3 && strncmp(line, "url", 3) == 0);
          if (is_key) {
            const char *v = eq + 1;
            const char *vend = line + llen;
            while (v < vend && isspace((unsigned char)*v)) {
              v++;
            }
            if (v < vend && *v == '"') {
              v++;
              const char *stop = v;
              while (stop < vend && *stop != '"') {
                stop++;
              }
              out = strndup(v, (size_t)(stop - v));
            }
            goto done;
          }
        }
      }
      pos = end + 1;
    }
  }
done:
  kbc_str_free(&body);
  free(path);
  return out;
}

static const char *resolve_base_url(void) {
  if (g_base_url == NULL) {
    if (g_daemon_flag != NULL) {
      g_base_url = xstrdup(g_daemon_flag);
    } else {
      const char *env = env_or_null("KBC_DAEMON_URL");
      if (env != NULL) {
        g_base_url = xstrdup(env);
      } else {
        char *from_file = daemons_toml_endpoint();
        if (from_file != NULL && from_file[0] != '\0') {
          g_base_url = from_file;
        } else {
          free(from_file);
          const kbc_config *cfg = load_config();
          char buf[64];
          snprintf(buf, sizeof buf, "http://127.0.0.1:%d", cfg->port);
          g_base_url = xstrdup(buf);
        }
      }
    }
    size_t n = strlen(g_base_url);
    while (n > 0 && g_base_url[n - 1] == '/') {
      g_base_url[--n] = '\0';
    }
  }
  return g_base_url;
}

/* The daemon's token, for the CLI's OWN outbound requests. The rule itself
 * lives in kbc_config_load_token: this is the same function the daemon path
 * calls, so the CLI and the daemon cannot disagree about what the token is.
 * `cfg` is non-const because resolving stores into it, which also means the
 * token file is read once per process, not once per request. */
static char *read_token(kbc_config *cfg) {
  kbc_err e;
  kbc_err_reset(&e);
  if (kbc_failed(kbc_config_load_token(cfg, &e))) {
    /* Not fatal here: the CLI is a client. A daemon on a non-loopback bind
     * will have refused to start over the same condition, and this request
     * will come back 401/403, which says the same thing more usefully. */
    return NULL;
  }
  if (cfg->token == NULL || cfg->token[0] == '\0') {
    return NULL;
  }
  return xstrdup(cfg->token);
}

/* ---------------------------------------------------------- http client ---- */

static int connect_to(const kbc_url *u, kbc_err *err) {
  struct addrinfo hints;
  memset(&hints, 0, sizeof hints);
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;
  struct addrinfo *res = NULL;
  int rc = getaddrinfo(u->host, u->port, &hints, &res);
  if (rc != 0) {
    kbc_err_set(err, KBC_ERR_IO, "resolve %s:%s: %s", u->host, u->port,
                gai_strerror(rc));
    return -1;
  }
  int fd = -1;
  int saved = 0;
  for (struct addrinfo *ai = res; ai != NULL; ai = ai->ai_next) {
    fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
    if (fd < 0) {
      continue;
    }
    struct timeval tv = { HTTP_TIMEOUT_SEC, 0 };
    (void)setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
    (void)setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    int one = 1;
    (void)setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
    if (connect(fd, ai->ai_addr, ai->ai_addrlen) == 0) {
      break;
    }
    saved = errno;
    close(fd);
    fd = -1;
  }
  freeaddrinfo(res);
  if (fd < 0) {
    kbc_err_set(err, KBC_ERR_IO, "connect %s:%s: %s", u->host, u->port,
                strerror(saved != 0 ? saved : errno));
    return -1;
  }
  return fd;
}

static kbc_status write_all(int fd, const char *buf, size_t n, kbc_err *err) {
  size_t off = 0;
  while (off < n) {
    ssize_t w = send(fd, buf + off, n - off, 0);
    if (w < 0) {
      if (errno == EINTR) {
        continue;
      }
      return kbc_err_set(err, KBC_ERR_IO, "send %zu bytes: %s", n - off,
                         strerror(errno));
    }
    off += (size_t)w;
  }
  return KBC_OK;
}

/* One recv into `out`. `*eof` is set when the peer closed the socket, which
 * is how a response that declares no length ends. A recv timeout is not a
 * framing signal and must not be reported as one: a socket that timed out
 * with bytes already buffered would otherwise spin this loop forever, so the
 * timeout is reported as eof and the caller decides whether what it has is
 * enough. */
static kbc_status recv_more(int fd, kbc_str *out, bool *eof, kbc_err *err) {
  *eof = false;
  char buf[8192];
  ssize_t r = recv(fd, buf, sizeof buf, 0);
  if (r < 0) {
    if (errno == EINTR) {
      return KBC_OK; /* no bytes, no eof: the caller loops */
    }
    if (out->len > 0) {
      *eof = true;
      return KBC_OK; /* whatever arrived before the timeout still parses */
    }
    return kbc_err_set(err, KBC_ERR_IO, "recv: %s", strerror(errno));
  }
  if (r == 0) {
    *eof = true;
    return KBC_OK;
  }
  if (out->len + (size_t)r > HTTP_MAX_RESPONSE) {
    return kbc_err_set(err, KBC_ERR_IO, "response over %u bytes",
                       HTTP_MAX_RESPONSE);
  }
  if (kbc_failed(kbc_str_append(out, buf, (size_t)r))) {
    return kbc_err_set(err, KBC_ERR_NOMEM, "response buffer: out of memory");
  }
  return KBC_OK;
}

static const char *find_crlfcrlf(const char *p, size_t n) {
  if (n < 4) {
    return NULL;
  }
  for (size_t i = 0; i + 4 <= n; i++) {
    if (p[i] == '\r' && p[i + 1] == '\n' && p[i + 2] == '\r' && p[i + 3] == '\n') {
      return p + i;
    }
  }
  return NULL;
}

static const char *header_value(const char *headers, size_t hlen,
                                const char *name, size_t *vlen) {
  size_t nlen = strlen(name);
  size_t pos = 0;
  while (pos < hlen) {
    size_t end = pos;
    while (end < hlen && headers[end] != '\n') {
      end++;
    }
    size_t llen = end - pos;
    if (llen > 0 && headers[pos + llen - 1] == '\r') {
      llen--;
    }
    if (llen > nlen && headers[pos + nlen] == ':' &&
        strncasecmp(headers + pos, name, nlen) == 0) {
      size_t vs = pos + nlen + 1;
      while (vs < pos + llen && isspace((unsigned char)headers[vs])) {
        vs++;
      }
      size_t ve = pos + llen;
      while (ve > vs && isspace((unsigned char)headers[ve - 1])) {
        ve--;
      }
      *vlen = ve - vs;
      return headers + vs;
    }
    pos = end + 1;
  }
  return NULL;
}

static kbc_status parse_status_line(const char *line, size_t len, int *status,
                                    kbc_err *err) {
  if (len < 12 || strncmp(line, "HTTP/1.", 7) != 0 || line[8] != ' ') {
    return kbc_err_set(err, KBC_ERR_PARSE, "malformed status line: %.*s",
                       (int)(len < 40 ? len : 40), line);
  }
  size_t i = 9;
  int code = 0;
  size_t digits = 0;
  while (i < len && isdigit((unsigned char)line[i])) {
    code = code * 10 + (line[i] - '0');
    i++;
    digits++;
  }
  if (digits != 3) {
    return kbc_err_set(err, KBC_ERR_PARSE, "no status code in: %.*s",
                       (int)(len < 40 ? len : 40), line);
  }
  *status = code;
  return KBC_OK;
}

/* One request, one response. `path` already carries its query string. */
static kbc_status http_do(const char *method, const char *path, const char *body,
                          size_t body_len, int *status, kbc_str *resp,
                          kbc_err *err) {
  kbc_config *cfg = load_config();
  kbc_url u;
  if (!url_parse(resolve_base_url(), &u, err)) {
    return KBC_ERR_INVALID;
  }
  char *token = read_token(cfg);
  size_t body_n = body != NULL ? body_len : 0u;

  kbc_str req;
  kbc_str_init(&req);
  kbc_status s = kbc_str_printf(&req,
                               "%s %s%s HTTP/1.1\r\nHost: %s:%s\r\n"
                               "User-Agent: " KBC_PROJECT "/" KBC_VERSION
                               "\r\nAccept: application/json\r\n",
                               method, u.prefix, path, u.host, u.port);
  if (!kbc_failed(s) && token != NULL) {
    s = kbc_str_puts(&req, "Authorization: Bearer ");
    if (!kbc_failed(s)) {
      s = kbc_str_puts(&req, token);
    }
    if (!kbc_failed(s)) {
      s = kbc_str_puts(&req, "\r\n");
    }
  }
  if (!kbc_failed(s)) {
    s = kbc_str_printf(&req, "Connection: close\r\nContent-Length: %zu\r\n",
                       body_n);
  }
  if (!kbc_failed(s) && body_n > 0) {
    s = kbc_str_puts(&req, "Content-Type: application/json\r\n");
  }
  if (!kbc_failed(s)) {
    s = kbc_str_puts(&req, "\r\n");
  }
  if (!kbc_failed(s) && body_n > 0) {
    s = kbc_str_append(&req, body, body_n);
  }
  free(token);
  if (kbc_failed(s)) {
    kbc_str_free(&req);
    url_free(&u);
    return kbc_err_set(err, KBC_ERR_NOMEM, "request buffer: out of memory");
  }

  int fd = connect_to(&u, err);
  if (fd < 0) {
    kbc_str_free(&req);
    url_free(&u);
    return KBC_ERR_IO;
  }
  s = write_all(fd, req.ptr, req.len, err);
  kbc_str_free(&req);
  url_free(&u);
  if (kbc_failed(s)) {
    close(fd);
    return s;
  }
  kbc_str_init(resp);
  /* The header block first, then exactly as many body bytes as it declares.
   * Reading to EOF instead is what made every socket verb take the full
   * HTTP_TIMEOUT_SEC: the daemon answers `Connection: keep-alive` even to a
   * `Connection: close` request, so the peer never closes and the only thing
   * that ends the wait is the recv timeout. Content-Length is the framing
   * the response actually declares, so it is what bounds the read. */
  bool eof = false;
  size_t hlen = 0;
  while (find_crlfcrlf(resp->ptr, resp->len) == NULL && !eof) {
    s = recv_more(fd, resp, &eof, err);
    if (kbc_failed(s)) {
      close(fd);
      kbc_str_free(resp);
      return s;
    }
  }
  const char *sep = find_crlfcrlf(resp->ptr, resp->len);
  if (sep == NULL) {
    close(fd);
    kbc_str_free(resp);
    return kbc_err_set(err, KBC_ERR_PARSE,
                       "truncated response: no header block");
  }
  hlen = (size_t)(sep - resp->ptr);
  size_t line_end = 0;
  while (line_end < hlen && resp->ptr[line_end] != '\n') {
    line_end++;
  }
  s = parse_status_line(resp->ptr, line_end, status, err);
  if (kbc_failed(s)) {
    close(fd);
    kbc_str_free(resp);
    return s;
  }
  size_t have = resp->len - (hlen + 4);
  size_t vlen = 0;
  const char *cl = header_value(resp->ptr, hlen, "content-length", &vlen);
  if (cl != NULL) {
    char num[24];
    size_t n = vlen < sizeof num - 1 ? vlen : sizeof num - 1;
    memcpy(num, cl, n);
    num[n] = '\0';
    char *endp = NULL;
    errno = 0;
    unsigned long long want = strtoull(num, &endp, 10);
    if (endp == num || errno != 0) {
      close(fd);
      kbc_str_free(resp);
      return kbc_err_set(err, KBC_ERR_PARSE, "bad Content-Length: %s", num);
    }
    if (want > HTTP_MAX_RESPONSE) {
      close(fd);
      kbc_str_free(resp);
      return kbc_err_set(err, KBC_ERR_IO, "Content-Length %llu over %u bytes",
                         want, HTTP_MAX_RESPONSE);
    }
    /* Only now is the declared length known, so only now can the body be
     * read to exactly that many bytes. A response with no Content-Length has
     * no other terminator than the peer closing, which eof reports. */
    while (have < (size_t)want && !eof) {
      s = recv_more(fd, resp, &eof, err);
      if (kbc_failed(s)) {
        close(fd);
        kbc_str_free(resp);
        return s;
      }
      have = resp->len - (hlen + 4);
    }
    if (have > (size_t)want) {
      have = (size_t)want;
    }
  }
  close(fd);
  /* Recomputed HERE, not above: every recv_more that grew the buffer may have
   * realloc'd it, so a body pointer taken BEFORE the read loop dangles by
   * the time the memmove below uses it. The offset is fixed; the address is
   * not. */
  const char *body_ptr = resp->ptr + hlen + 4;
  memmove(resp->ptr, body_ptr, have);
  resp->len = have;
  resp->ptr[have] = '\0';
  return KBC_OK;
}

/* Wraps http_do: a 401 gets the message that names the fix, everything else
 * is returned to the caller with the body and status. `eout` receives the
 * error, so a caller can tell a connect failure from a status it must
 * report; pass NULL to keep the error private. */
static kbc_status http_call(const char *method, const char *path,
                            const char *body, size_t body_len, int *status,
                            kbc_str *resp, kbc_err *eout) {
  kbc_err local;
  kbc_err_reset(&local);
  kbc_err *e = eout != NULL ? eout : &local;
  kbc_status s = http_do(method, path, body, body_len, status, resp, e);
  if (kbc_failed(s)) {
    return s;
  }
  if (*status == 401) {
    kbc_str_free(resp);
    return kbc_err_set(e, KBC_ERR_INVALID,
                       "the daemon wants a token: run `kbc token generate`, "
                       "restart the daemon, then retry");
  }
  return KBC_OK;
}

/* Reports a failed daemon call in the right exit class. */
static int call_failed(kbc_status s, const kbc_err *e, const char *what)
    __attribute__((noreturn));
static int call_failed(kbc_status s, const kbc_err *e, const char *what) {
  const char *hint = (s == KBC_ERR_IO && e != NULL &&
                      strncmp(e->msg, "connect ", 8) == 0)
                         ? "(start one with `kbc daemon`)"
                         : NULL;
  fail_status(s, e, what, hint);
}

/* ------------------------------------------------- local (no daemon) ----- */

/* Which verbs fall back to a local, in-process app when nothing is
 * listening: search, reindex, get and list. Each is a read over the same
 * store and index the daemon would use -- reindex re-ingests into that same
 * store -- and each renders through the same JSON the routes emit, so the
 * two paths are indistinguishable except for the one stderr line.
 * `add` does not fall back: it writes the config file itself, which is the
 * user's intent rather than a query over the store. `status` does not: it
 * reports the counters of a running daemon, which is the point of asking. */

/* "Unreachable" is a transport failure and never a status code. A 401 or a
 * 500 means the daemon IS there and is answering, so its answer is the one
 * the user gets; only a resolve/connect/send/recv failure means nobody is
 * home. http_call already turns 401 into a KBC_ERR_INVALID, so it can never
 * reach the fallback. */
static bool daemon_unreachable(kbc_status s, const kbc_err *e) {
  if (s != KBC_ERR_IO || e == NULL) {
    return false;
  }
  static const char *const pre[] = { "resolve ", "connect ", "send ", "recv ",
                                      NULL };
  for (size_t i = 0; pre[i] != NULL; i++) {
    if (strncmp(e->msg, pre[i], strlen(pre[i])) == 0) {
      return true;
    }
  }
  return false;
}

/* The one line that names the path taken. --json promises an empty stderr,
 * so there the fallback is silent and the JSON is the only output. */
static void say_local(const char *verb) {
  if (!g_json) {
    fprintf(stderr, "%s: no daemon at %s; running locally\n", verb,
            resolve_base_url());
  }
}

/* ---- the JSON the routes emit, built here so both paths render alike ---- */

/* The routes spell an absent artifact_id/summary as null and an absent
 * corpus/path/title as ""; the local path must spell them the same way or
 * the two bodies stop being interchangeable. */
static kbc_status put_json_nullable(kbc_str *out, const char *s) {
  return s != NULL ? kbc_str_append_json_string(out, s, strlen(s))
                   : kbc_str_puts(out, "null");
}

static kbc_status put_json_text(kbc_str *out, const char *s) {
  return kbc_str_append_json_string(out, s != NULL ? s : "",
                                    s != NULL ? strlen(s) : 0);
}

static kbc_status search_row_json(kbc_str *out, const kbc_result_row *row) {
  kbc_status st = kbc_str_printf(
      out,
      "{\"doc_id\":%lu,\"score\":%.6f,\"keyword_score\":%.6f,"
      "\"vector_score\":%.6f,\"keyword_rank\":%ld,\"vector_rank\":%ld,"
      "\"artifact_id\":",
      (unsigned long)row->doc_id, row->score, row->keyword_score,
      row->vector_score, (long)row->keyword_rank, (long)row->vector_rank);
  if (kbc_failed(st)) {
    return st;
  }
  st = put_json_nullable(out, row->artifact_id);
  if (kbc_failed(st)) {
    return st;
  }
  if ((st = kbc_str_puts(out, ",\"corpus\":"), kbc_failed(st))) {
    return st;
  }
  if ((st = put_json_text(out, row->corpus), kbc_failed(st))) {
    return st;
  }
  if ((st = kbc_str_puts(out, ",\"path\":"), kbc_failed(st))) {
    return st;
  }
  if ((st = put_json_text(out, row->path), kbc_failed(st))) {
    return st;
  }
  if ((st = kbc_str_puts(out, ",\"title\":"), kbc_failed(st))) {
    return st;
  }
  if ((st = put_json_text(out, row->title), kbc_failed(st))) {
    return st;
  }
  if ((st = kbc_str_puts(out, ",\"summary\":"), kbc_failed(st))) {
    return st;
  }
  if ((st = put_json_nullable(out, row->summary), kbc_failed(st))) {
    return st;
  }
  return kbc_str_putc(out, '}');
}

static kbc_status artifact_json(kbc_str *out, const kbc_artifact *art,
                                bool with_source) {
  kbc_status st = kbc_str_puts(out, "{\"id\":");
  if (kbc_failed(st)) {
    return st;
  }
  st = kbc_str_append_json_string(out, art->id, strlen(art->id));
  if (kbc_failed(st)) {
    return st;
  }
  st = kbc_str_printf(out,
                      ",\"kind\":\"%s\",\"mtime_ns\":%lld,"
                      "\"size_bytes\":%lld,\"content_hash\":%lu,"
                      "\"heading_count\":%d,\"corpus\":",
                      kbc_kind_str(art->kind), (long long)art->mtime_ns,
                      (long long)art->size_bytes,
                      (unsigned long)art->content_hash,
                      (int)art->heading_count);
  if (kbc_failed(st)) {
    return st;
  }
  st = kbc_str_append_json_string(out, art->corpus, strlen(art->corpus));
  if (kbc_failed(st)) {
    return st;
  }
  if ((st = kbc_str_puts(out, ",\"path\":"), kbc_failed(st))) {
    return st;
  }
  if ((st = kbc_str_append_json_string(out, art->path, strlen(art->path)),
      kbc_failed(st))) {
    return st;
  }
  if ((st = kbc_str_puts(out, ",\"title\":"), kbc_failed(st))) {
    return st;
  }
  if ((st = put_json_text(out, art->title), kbc_failed(st))) {
    return st;
  }
  if ((st = kbc_str_puts(out, ",\"summary\":"), kbc_failed(st))) {
    return st;
  }
  if ((st = put_json_text(out, art->summary), kbc_failed(st))) {
    return st;
  }
  if (!with_source) {
    return kbc_str_putc(out, '}');
  }
  if ((st = kbc_str_puts(out, ",\"source\":"), kbc_failed(st))) {
    return st;
  }
  if ((st = put_json_nullable(out, art->source), kbc_failed(st))) {
    return st;
  }
  return kbc_str_putc(out, '}');
}

/* The local app: the same config the HTTP path would have used -- the same
 * --config, token_path, data_dir, corpora -- and therefore the same store and
 * index, so a daemon started later sees exactly what a local run wrote. */
static kbc_app *local_open(kbc_err *err) {
  kbc_err_reset(err);
  /* Over HTTP the daemon owns the log and the CLI's stderr stays empty; a
   * local app would otherwise print its own INFO lines into --json's empty
   * stderr. Errors still print, as they do on every other path. */
  if (g_json) {
    kbc_log_set_level(KBC_LOG_ERROR);
  }
  return kbc_app_open(load_config(), err);
}

static kbc_status local_search_json(kbc_str *out, const kbc_query *kq,
                                    kbc_err *err) {
  kbc_app *app = local_open(err);
  if (app == NULL) {
    return KBC_ERR_IO;
  }
  kbc_arena *a = kbc_arena_new(64u * 1024u);
  kbc_search_result res;
  memset(&res, 0, sizeof res);
  kbc_status st = a != NULL ? KBC_OK
                            : kbc_err_set(err, KBC_ERR_NOMEM,
                                          "search: out of memory");
  if (!kbc_failed(st)) {
    st = kbc_app_search(app, a, kq, &res, err);
  }
  size_t emitted = 0;
  if (!kbc_failed(st)) {
    st = kbc_str_puts(out, "{\"results\":[");
  }
  for (size_t i = 0; !kbc_failed(st) && i < res.len && emitted < kq->limit;
       i++) {
    if (emitted > 0) {
      st = kbc_str_putc(out, ',');
      if (kbc_failed(st)) {
        break;
      }
    }
    st = search_row_json(out, &res.rows[i]);
    if (kbc_failed(st)) {
      break;
    }
    emitted++;
  }
  if (!kbc_failed(st)) {
    st = kbc_str_printf(
        out,
        "],\"took_us\":%lld,\"candidates\":%zu,\"degraded\":%s,\"limit\":%zu,"
        "\"offset\":0,\"count\":%zu}",
        (long long)res.took_us, res.candidates, res.degraded ? "true" : "false",
        kq->limit, emitted);
  }
  kbc_arena_free(a);
  kbc_app_close(app);
  return st;
}

/* The counters come from the app itself, exactly as route_reindex reads
 * them, so the printed line is the same line. `kb` is not a filter here
 * either: the route ignores it, so the local path ignores it too. */
static kbc_status local_reindex_json(kbc_str *out, kbc_err *err) {
  kbc_app *app = local_open(err);
  if (app == NULL) {
    return KBC_ERR_IO;
  }
  int64_t t0 = kbc_now_ns();
  kbc_status st = kbc_app_reindex(app, err);
  int64_t took_us = (kbc_now_ns() - t0) / 1000;
  kbc_app_stats stats;
  memset(&stats, 0, sizeof stats);
  if (!kbc_failed(st)) {
    kbc_err local;
    kbc_err_reset(&local);
    (void)kbc_app_stats_get(app, &stats, &local);
  }
  if (!kbc_failed(st)) {
    st = kbc_str_printf(out, "{\"docs\":%lld,\"took_us\":%lld}",
                        (long long)stats.last_reindex_docs,
                        (long long)took_us);
  }
  kbc_app_close(app);
  return st;
}

static kbc_status local_get_json(kbc_str *out, const char *id, bool with_source,
                                 kbc_err *err) {
  kbc_app *app = local_open(err);
  if (app == NULL) {
    return KBC_ERR_IO;
  }
  kbc_arena *a = kbc_arena_new(64u * 1024u);
  kbc_artifact art;
  memset(&art, 0, sizeof art);
  kbc_status st = a != NULL ? KBC_OK : kbc_err_set(err, KBC_ERR_NOMEM,
                                                 "get: out of memory");
  if (!kbc_failed(st)) {
    st = kbc_app_get_artifact(app, a, id, with_source, &art, err);
  }
  if (!kbc_failed(st)) {
    st = artifact_json(out, &art, with_source);
  }
  kbc_arena_free(a);
  kbc_app_close(app);
  return st;
}

static kbc_status local_list_json(kbc_str *out, const char *kb, size_t limit,
                                  kbc_err *err) {
  kbc_app *app = local_open(err);
  if (app == NULL) {
    return KBC_ERR_IO;
  }
  kbc_arena *a = kbc_arena_new(64u * 1024u);
  kbc_artifact *rows = NULL;
  size_t n = 0;
  kbc_status st = a != NULL ? KBC_OK : kbc_err_set(err, KBC_ERR_NOMEM,
                                                 "list: out of memory");
  if (!kbc_failed(st)) {
    st = kbc_app_list_artifacts(app, a, kb, KBC_KIND__COUNT, limit, 0, &rows,
                                &n, err);
  }
  if (!kbc_failed(st)) {
    st = kbc_str_puts(out, "{\"artifacts\":[");
  }
  for (size_t i = 0; !kbc_failed(st) && i < n; i++) {
    if (i > 0) {
      st = kbc_str_putc(out, ',');
      if (kbc_failed(st)) {
        break;
      }
    }
    st = artifact_json(out, &rows[i], false);
  }
  if (!kbc_failed(st)) {
    st = kbc_str_printf(out, "],\"total\":%zu,\"limit\":%zu,\"offset\":0}", n,
                        limit);
  }
  kbc_arena_free(a);
  kbc_app_close(app);
  return st;
}

/* --------------------------------------------------------------- output ---- */

/* --json means "one JSON object on stdout and nothing else", so every verb
 * hands the daemon's body straight through. */
static void print_body(const kbc_str *resp) {
  fwrite(resp->ptr, 1, resp->len, stdout);
  fputc('\n', stdout);
}

/* Falls back to the raw body when the shape is not one we recognise: a CLI
 * that prints nothing because a field moved is worse than one that prints
 * what the daemon actually said. */
static kbc_arena *resp_arena(const kbc_str *resp, kbc_json **out,
                             const char *what) {
  kbc_arena *a = kbc_arena_new(64u * 1024u);
  if (a == NULL) {
    die_io("%s: out of memory", what);
  }
  kbc_err e;
  kbc_err_reset(&e);
  *out = kbc_json_parse(a, resp->ptr, resp->len, &e);
  if (*out == NULL) {
    kbc_arena_free(a);
    die_io("%s: %s", what, e.msg[0] != '\0' ? e.msg : "response was not JSON");
  }
  return a;
}

static const char *row_str(const kbc_json *row, const char *const *keys) {
  for (size_t i = 0; keys[i] != NULL; i++) {
    const char *v = kbc_json_str(row, keys[i], NULL);
    if (v != NULL) {
      return v;
    }
  }
  return NULL;
}

static const char *const K_ID_KEYS[] = { "id", "artifact_id", NULL };
static const char *const K_PATH_KEYS[] = { "path", "rel_path", NULL };
static const char *const K_TITLE_KEYS[] = { "title", "name", NULL };
static const char *const K_SCORE_KEYS[] = { "score", "fused_score", NULL };

/* The array of rows, whichever of the four names the route used. */
static const kbc_json *rows_of(const kbc_json *root) {
  static const char *const keys[] = { "hits", "results", "artifacts", "items",
                                      NULL };
  for (size_t i = 0; keys[i] != NULL; i++) {
    const kbc_json *v = kbc_json_get(root, keys[i]);
    if (v != NULL && kbc_json_is(v, KBC_JSON_ARR)) {
      return v;
    }
  }
  if (kbc_json_is(root, KBC_JSON_ARR)) {
    return root;
  }
  return NULL;
}

static void print_rows(const kbc_json *root) {
  const kbc_json *rows = rows_of(root);
  if (rows == NULL) {
    return;
  }
  size_t n = kbc_json_len(rows);
  for (size_t i = 0; i < n; i++) {
    const kbc_json *row = kbc_json_at(rows, i);
    if (!kbc_json_is(row, KBC_JSON_OBJ)) {
      continue;
    }
    const char *id = row_str(row, K_ID_KEYS);
    const char *path = row_str(row, K_PATH_KEYS);
    const char *title = row_str(row, K_TITLE_KEYS);
    double score = 0.0;
    for (size_t k = 0; K_SCORE_KEYS[k] != NULL; k++) {
      const kbc_json *sv = kbc_json_get(row, K_SCORE_KEYS[k]);
      if (sv != NULL && kbc_json_is(sv, KBC_JSON_NUM)) {
        score = sv->u.num;
        break;
      }
    }
    printf("%2zu. %s\n", i + 1, title != NULL ? title : "(untitled)");
    if (id != NULL) {
      printf("    id:   %s\n", id);
    }
    if (path != NULL) {
      printf("    path: %s\n", path);
    }
    printf("    score: %.4f\n", score);
  }
}

/* ---------------------------------------------------------- arg parsing --- */

typedef struct {
  const char *name;
  bool takes_value;
} flag_def;

/* Everything a verb can be told, in one place: the flags are validated and
 * collected in a single pass, so ordering never surprises a verb. */
typedef struct {
  const char *kb;
  const char *mode;
  const char *bind;
  const char *corpus;
  const char *id;
  const char *query;
  const char *body;
  const char *anchor;
  const char *author;
  const char *path;
  const char *output;
  const char *out;
  long port;
  size_t limit;
  size_t queries;
  size_t repeats;
  size_t n;
  unsigned long long seed;
  bool has_limit;
  bool has_queries;
  bool has_repeats;
  bool has_bind;
  bool has_port;
  bool has_kb;
  bool has_mode;
  bool has_n;
  bool has_seed;
  bool source;
  bool all;
  bool force;
  bool foreground;
  bool apply;
  /* `prune`: the retention window in days. The original's `[retention]`
   * windows are `Option<u32>` and a set one arms a daily background sweep
   * (kb-core/src/config.rs:454-536), so a day window is its own unit and a
   * MISSING window is its own answer. */
  size_t days;
  bool has_days;
  const char *positional[4];
  int npos;
} opts;

static size_t parse_bounded(const char *s, const char *flag, size_t lo,
                            size_t hi) {
  char *endp = NULL;
  errno = 0;
  unsigned long v = strtoul(s, &endp, 10);
  if (endp == s || *endp != '\0' || errno != 0 || v < lo || v > hi) {
    die_user("%s: %s is not a number in %zu..%zu", flag, s, lo, hi);
  }
  return (size_t)v;
}

static void parse_verb(const flag_def *defs, int argc, char **argv, int start,
                       opts *o) {
  memset(o, 0, sizeof *o);
  bool end_of_options = false; /* set by `--`; see below */
  for (int k = start; k < argc; k++) {
    const char *s = argv[k];
    /* `--` ends option parsing: everything after it is a positional, whatever
     * it looks like. A corpus name may legitimately begin with `-` (`-notes`
     * is a valid KbName), and without this there would be no way to NAME one
     * on the command line at all. Standard POSIX, and the only place a
     * leading-dash value can be passed unambiguously. */
    if (end_of_options) {
      if (o->npos < (int)(sizeof o->positional / sizeof o->positional[0])) {
        o->positional[o->npos++] = s;
      } else {
        die_user("unexpected argument %s", s);
      }
      continue;
    }
    if (strcmp(s, "--") == 0) {
      end_of_options = true;
      continue;
    }
    if (s[0] != '-' || s[1] == '\0') {
      if (o->npos < (int)(sizeof o->positional / sizeof o->positional[0])) {
        o->positional[o->npos++] = s;
      } else {
        die_user("unexpected argument %s", s);
      }
      continue;
    }
    /* Global flags: accepted before or after the verb, in any position among
     * the verb's own flags. Repeated, the last one wins. */
    if (strcmp(s, "--json") == 0) {
      g_json = true;
      continue;
    }
    if (strcmp(s, "--daemon") == 0 || strcmp(s, "--config") == 0) {
      if (k + 1 >= argc) {
        die_user("%s needs an argument", s);
      }
      const char *value = argv[++k];
      if (s[2] == 'd') {
        g_daemon_flag = value;
      } else {
        g_config_flag = value;
      }
      continue;
    }
    if (s[1] != '-') {
      die_user("unknown flag %s (kbc takes long flags only)", s);
    }
    const flag_def *d = NULL;
    for (size_t j = 0; defs[j].name != NULL; j++) {
      if (strcmp(s, defs[j].name) == 0) {
        d = &defs[j];
        break;
      }
    }
    if (d == NULL) {
      die_user("unknown flag %s", s);
    }
    const char *value = NULL;
    if (d->takes_value) {
      if (k + 1 >= argc) {
        die_user("%s needs an argument", s);
      }
      value = argv[++k];
    }
    if (strcmp(s, "--kb") == 0) {
      o->kb = value;
      o->has_kb = true;
    } else if (strcmp(s, "--mode") == 0) {
      o->mode = value;
      o->has_mode = true;
    } else if (strcmp(s, "--bind") == 0) {
      o->bind = value;
      o->has_bind = true;
    } else if (strcmp(s, "--corpus") == 0) {
      o->corpus = value;
    } else if (strcmp(s, "--artifact-id") == 0) {
      o->id = value;
    } else if (strcmp(s, "--limit") == 0) {
      o->limit = parse_bounded(value, "--limit", 1, KBC_MAX_HITS);
      o->has_limit = true;
    } else if (strcmp(s, "--queries") == 0) {
      o->queries = parse_bounded(value, "--queries", 1, 100000);
      o->has_queries = true;
    } else if (strcmp(s, "--repeat") == 0) {
      o->repeats = parse_bounded(value, "--repeat", 1, 100000);
      o->has_repeats = true;
    } else if (strcmp(s, "--port") == 0) {
      size_t p = parse_bounded(value, "--port", 1, 65535);
      o->port = (long)p;
      o->has_port = true;
    } else if (strcmp(s, "--source") == 0) {
      o->source = true;
    } else if (strcmp(s, "--out") == 0) {
      o->out = value;
    } else if (strcmp(s, "--all") == 0) {
      o->all = true;
    } else if (strcmp(s, "--force") == 0) {
      o->force = true;
    } else if (strcmp(s, "--body") == 0) {
      o->body = value;
    } else if (strcmp(s, "--anchor") == 0) {
      o->anchor = value;
    } else if (strcmp(s, "--author") == 0) {
      o->author = value;
    } else if (strcmp(s, "--path") == 0) {
      o->path = value;
    } else if (strcmp(s, "--output") == 0) {
      o->output = value;
    } else if (strcmp(s, "--n") == 0) {
      o->n = parse_bounded(value, "--n", 1, 1000000);
      o->has_n = true;
    } else if (strcmp(s, "--seed") == 0) {
      /* Base 0: the Rust's own default is written `0xb33f` and the README
       * tells operators to pass it that way, so a decimal-only parse would
       * reject the value the documentation hands them. */
      char *endp = NULL;
      errno = 0;
      unsigned long long v = strtoull(value, &endp, 0);
      if (endp == value || *endp != '\0' || errno != 0) {
        die_user("--seed: %s is not a number", value);
      }
      o->seed = v;
      o->has_seed = true;
    } else if (strcmp(s, "--foreground") == 0) {
      o->foreground = true;
    } else if (strcmp(s, "--apply") == 0) {
      o->apply = true;
    } else if (strcmp(s, "--days") == 0) {
      /* Lower bound 1, not 0, and that is the original's rule rather than a
       * nicety: `[retention] history_days = 0` is a HARD validation error
       * (config.rs:1932-1947) because a zero-day window sets the cutoff to
       * `now` and would delete every row on the next tick. A CLI cannot
       * express "off" as 0, so it refuses it here and the verb keeps the
       * original's meaning for the one value that means the opposite. */
      o->days = parse_bounded(value, "--days", 1, PRUNE_MAX_DAYS);
      o->has_days = true;
    }
  }
}

static const char *positional(const opts *o, int index, const char *what) {
  if (index >= o->npos) {
    die_user("%s needs an argument", what);
  }
  return o->positional[index];
}

/* --------------------------------------------------------------- verbs ----- */

static const flag_def FLAGS_SEARCH[] = { { "--kb", true },
                                         { "--mode", true },
                                         { "--limit", true },
                                         { NULL, false } };
static const flag_def FLAGS_GET[] = { { "--source", false }, { NULL, false } };
static const flag_def FLAGS_KB[] = { { "--kb", true }, { NULL, false } };
static const flag_def FLAGS_LIST[] = { { "--kb", true },
                                       { "--limit", true },
                                       { NULL, false } };
static const flag_def FLAGS_NONE[] = { { NULL, false } };
static const flag_def FLAGS_DAEMON[] = { { "--bind", true },
                                         { "--port", true },
                                         { "--foreground", false },
                                         { NULL, false } };
static const flag_def FLAGS_BENCH[] = { { "--queries", true },
                                        { "--repeat", true },
                                        { "--corpus", true },
                                        { NULL, false } };
static const flag_def FLAGS_BACKUP[] = { { "--out", true },
                                         { "--all", false },
                                         { NULL, false } };
static const flag_def FLAGS_RESTORE[] = { { "--kb", true },
                                           { "--force", false },
                                           { NULL, false } };
/* `comments` — the Rust's flag surface, minus what has no kb-c
 * counterpart. `--kb` narrows to one corpus; `--path`/`--artifact-id` name
 * the document (either, never both); `--limit` caps the listing the way the
 * Rust's `?limit=` does. `--daemon` is absent because the daemon's own
 * route table has no review endpoints: this port's comments are rows in
 * the store, not a daemon-side review file, so the CLI reads the store
 * directly and there is no endpoint to name. */
static const flag_def FLAGS_COMMENTS[] = { { "--kb", true },
                                           { "--path", true },
                                           { "--artifact-id", true },
                                           { "--limit", true },
                                           { "--body", true },
                                           { "--anchor", true },
                                           { "--author", true },
                                           { "--all", false },
                                           { NULL, false } };
/* `--kb` is the corpus whose NAME goes into each id: kb-c mints an id from
 * (corpus, path) together (src/ids.c), so without the name there is no id to
 * write and the scaffold would be unusable against a kb-c store. */
static const flag_def FLAGS_BENCH_INIT[] = { { "--kb", true },
                                             { "--corpus", true },
                                             { "--output", true },
                                             { "--n", true },
                                             { "--seed", true },
                                             { NULL, false } };

/* `prune` — the retention window and the affirmative. `--days` names the
 * window in the original's own unit (a day count), and `--apply` is the
 * only thing that makes it delete. There is no third flag because there is
 * no third choice: the original's `reading_sections_days` has no kb-c
 * counterpart (kb-c folded that state into the history row's own columns —
 * `last_section`, `scroll_y_max`, `active_ms` — so a section window and a
 * history window would prune the SAME rows here), and a flag that aliases
 * another flag is a flag that can disagree with it. */
static const flag_def FLAGS_PRUNE[] = { { "--days", true },
                                        { "--apply", false },
                                        { NULL, false } };
/* `metrics` — `--out` writes the scrape to a file instead of stdout. The
 * Rust's `kb metrics` has no such flag (it only prints), so this is the one
 * place the verb is larger than the original: a scrape you cannot save is
 * not an export, and PORT_PLAN stage 6 asks for an export. */
static const flag_def FLAGS_METRICS[] = { { "--out", true },
                                          { NULL, false } };

static int cmd_search(int argc, char **argv, int start) {
  opts o;
  parse_verb(FLAGS_SEARCH, argc, argv, start, &o);
  const char *q = positional(&o, 0, "search <query>");
  if (q[0] == '\0') {
    die_user("search <query>: the query is empty");
  }
  if (strlen(q) > KBC_MAX_QUERY_LEN) {
    die_user("search: the query is %zu bytes, the limit is %u", strlen(q),
             KBC_MAX_QUERY_LEN);
  }
  kbc_search_mode m = KBC_MODE_HYBRID;
  if (o.has_mode && !kbc_search_mode_parse(o.mode, &m)) {
    die_user("--mode: %s is not one of hybrid, keyword, semantic", o.mode);
  }
  size_t limit = o.has_limit ? o.limit : 20;

  kbc_str path;
  kbc_str_init(&path);
  (void)kbc_str_printf(&path, "%s?q=", R_SEARCH);
  url_encode(&path, q);
  if (o.has_kb) {
    (void)kbc_str_puts(&path, "&kb=");
    url_encode(&path, o.kb);
  }
  (void)kbc_str_printf(&path, "&mode=%s&limit=%zu", kbc_search_mode_str(m), limit);

  int status = 0;
  kbc_str resp;
  kbc_err e;
  kbc_err_reset(&e);
  kbc_status s = http_call("GET", path.ptr, NULL, 0, &status, &resp, &e);
  kbc_str_free(&path);
  if (kbc_failed(s)) {
    if (!daemon_unreachable(s, &e)) {
      return call_failed(s, &e, "search");
    }
    kbc_query kq;
    memset(&kq, 0, sizeof kq);
    kq.q = q;
    kq.corpus = o.has_kb ? o.kb : NULL;
    kq.kind = KBC_KIND__COUNT;
    kq.mode = m;
    kq.limit = limit;
    kq.rrf_k = load_config()->rrf_k;
    say_local("search");
    kbc_str_init(&resp);
    kbc_err_reset(&e);
    s = local_search_json(&resp, &kq, &e);
    if (kbc_failed(s)) {
      kbc_str_free(&resp);
      return call_failed(s, &e, "search");
    }
  } else if (status == 404) {
    kbc_str_free(&resp);
    die_user("search: no such corpus %s", o.has_kb ? o.kb : "(any)");
  } else if (status < 200 || status >= 300) {
    kbc_str_free(&resp);
    die_io("search: HTTP %d", status);
  }
  if (g_json) {
    print_body(&resp);
    kbc_str_free(&resp);
    return EXIT_OK;
  }
  kbc_json *root = NULL;
  kbc_arena *a = resp_arena(&resp, &root, "search");
  const kbc_json *rows = rows_of(root);
  if (rows == NULL) {
    print_body(&resp);
  } else if (kbc_json_len(rows) == 0) {
    kbc_arena_free(a);
    kbc_str_free(&resp);
    die_user("no results for %s", q);
  } else {
    print_rows(root);
  }
  kbc_arena_free(a);
  kbc_str_free(&resp);
  return EXIT_OK;
}

static int cmd_get(int argc, char **argv, int start) {
  opts o;
  parse_verb(FLAGS_GET, argc, argv, start, &o);
  const char *id = positional(&o, 0, "get <id>");
  if (!kbc_id_is_valid(id)) {
    die_user("get <id>: %s is not a 12-hex artifact id", id);
  }
  kbc_str path;
  kbc_str_init(&path);
  (void)kbc_str_printf(&path, "%s/%s", R_ARTIFACTS, id);
  if (o.source) {
    (void)kbc_str_puts(&path, "?source=1");
  }
  int status = 0;
  kbc_str resp;
  kbc_err e;
  kbc_err_reset(&e);
  kbc_status s = http_call("GET", path.ptr, NULL, 0, &status, &resp, &e);
  kbc_str_free(&path);
  if (kbc_failed(s)) {
    if (!daemon_unreachable(s, &e)) {
      return call_failed(s, &e, "get");
    }
    say_local("get");
    kbc_str_init(&resp);
    kbc_err_reset(&e);
    s = local_get_json(&resp, id, o.source, &e);
    if (s == KBC_ERR_NOTFOUND) {
      kbc_str_free(&resp);
      die_user("no artifact %s", id);
    }
    if (kbc_failed(s)) {
      kbc_str_free(&resp);
      return call_failed(s, &e, "get");
    }
  } else if (status == 404) {
    kbc_str_free(&resp);
    die_user("no artifact %s", id);
  } else if (status < 200 || status >= 300) {
    kbc_str_free(&resp);
    die_io("get: HTTP %d", status);
  }
  if (g_json) {
    print_body(&resp);
    kbc_str_free(&resp);
    return EXIT_OK;
  }
  kbc_json *root = NULL;
  kbc_arena *a = resp_arena(&resp, &root, "get");
  if (kbc_json_is(root, KBC_JSON_OBJ)) {
    const kbc_json *art = kbc_json_get(root, "artifact");
    if (!kbc_json_is(art, KBC_JSON_OBJ)) {
      art = root;
    }
    const char *title = kbc_json_str(art, "title", NULL);
    const char *pathv = kbc_json_str(art, "path", NULL);
    const char *corpus = kbc_json_str(art, "corpus", NULL);
    const char *summary = kbc_json_str(art, "summary", NULL);
    const char *source = kbc_json_str(art, "source", NULL);
    if (title != NULL) {
      printf("%s\n", title);
    }
    printf("id: %s\n", id);
    if (corpus != NULL) {
      printf("kb: %s\n", corpus);
    }
    if (pathv != NULL) {
      printf("path: %s\n", pathv);
    }
    if (summary != NULL && summary[0] != '\0') {
      printf("\n%s\n", summary);
    }
    if (source != NULL) {
      printf("\n---\n%s\n", source);
    }
  } else {
    print_body(&resp);
  }
  kbc_arena_free(a);
  kbc_str_free(&resp);
  return EXIT_OK;
}

static int cmd_list(int argc, char **argv, int start) {
  opts o;
  parse_verb(FLAGS_LIST, argc, argv, start, &o);
  size_t limit = o.has_limit ? o.limit : 20;
  kbc_str path;
  kbc_str_init(&path);
  (void)kbc_str_printf(&path, "%s?limit=%zu", R_ARTIFACTS, limit);
  if (o.has_kb) {
    (void)kbc_str_puts(&path, "&kb=");
    url_encode(&path, o.kb);
  }
  int status = 0;
  kbc_str resp;
  kbc_err e;
  kbc_err_reset(&e);
  kbc_status s = http_call("GET", path.ptr, NULL, 0, &status, &resp, &e);
  kbc_str_free(&path);
  if (kbc_failed(s)) {
    if (!daemon_unreachable(s, &e)) {
      return call_failed(s, &e, "list");
    }
    say_local("list");
    kbc_str_init(&resp);
    kbc_err_reset(&e);
    if (kbc_failed(s = local_list_json(&resp, o.has_kb ? o.kb : NULL, limit,
                                       &e))) {
      kbc_str_free(&resp);
      return call_failed(s, &e, "list");
    }
  } else if (status == 404) {
    kbc_str_free(&resp);
    die_user("list: no such corpus %s", o.has_kb ? o.kb : "(any)");
  } else if (status < 200 || status >= 300) {
    kbc_str_free(&resp);
    die_io("list: HTTP %d", status);
  }
  if (g_json) {
    print_body(&resp);
    kbc_str_free(&resp);
    return EXIT_OK;
  }
  kbc_json *root = NULL;
  kbc_arena *a = resp_arena(&resp, &root, "list");
  const kbc_json *rows = rows_of(root);
  if (rows == NULL) {
    print_body(&resp);
  } else if (kbc_json_len(rows) == 0) {
    kbc_arena_free(a);
    kbc_str_free(&resp);
    die_user("no artifacts in %s", o.has_kb ? o.kb : "any corpus");
  } else {
    print_rows(root);
  }
  kbc_arena_free(a);
  kbc_str_free(&resp);
  return EXIT_OK;
}

static int cmd_reindex(int argc, char **argv, int start) {
  opts o;
  parse_verb(FLAGS_KB, argc, argv, start, &o);
  kbc_str body;
  kbc_str_init(&body);
  if (o.has_kb) {
    (void)kbc_str_puts(&body, "{\"kb\":");
    (void)kbc_str_append_json_string(&body, o.kb, strlen(o.kb));
    (void)kbc_str_puts(&body, "}");
  }
  int status = 0;
  kbc_str resp;
  kbc_err e;
  kbc_err_reset(&e);
  kbc_status s =
      http_call("POST", R_REINDEX, body.ptr, body.len, &status, &resp, &e);
  kbc_str_free(&body);
  if (kbc_failed(s)) {
    if (!daemon_unreachable(s, &e)) {
      return call_failed(s, &e, "reindex");
    }
    /* The local app writes to the configured store and index, so a daemon
     * started later sees exactly what this run indexed. */
    say_local("reindex");
    kbc_str_init(&resp);
    kbc_err_reset(&e);
    if (kbc_failed(s = local_reindex_json(&resp, &e))) {
      kbc_str_free(&resp);
      return call_failed(s, &e, "reindex");
    }
  } else if (status < 200 || status >= 300) {
    kbc_str_free(&resp);
    die_io("reindex: HTTP %d", status);
  }
  if (g_json) {
    print_body(&resp);
    kbc_str_free(&resp);
    return EXIT_OK;
  }
  kbc_json *root = NULL;
  kbc_arena *a = resp_arena(&resp, &root, "reindex");
  const kbc_json *dv = kbc_json_get(root, "docs");
  if (dv == NULL) {
    dv = kbc_json_get(root, "documents");
  }
  if (dv != NULL && kbc_json_is(dv, KBC_JSON_NUM)) {
    double took = kbc_json_num(root, "took_us", 0.0);
    /* stderr, not stdout: the original splits this verb's streams so that
     * `kb reindex > log` captures machine-readable output and leaves the
     * human line out of it (reindex.rs:52-53 in the Rust). The two verbs
     * whose output an operator pipes -- this one and `metrics` -- both keep
     * their payload on stdout, so a redirect stays parseable. */
    fprintf(stderr, "reindexed %.0f documents in %.0f us%s%s\n", dv->u.num,
            took, o.has_kb ? " in " : "", o.has_kb ? o.kb : "");
  } else if (status == 202) {
    /* The daemon accepted the work rather than running it inline. */
    fprintf(stderr, "reindex accepted%s%s; run `kbc status` for the counters\n",
            o.has_kb ? " for " : "", o.has_kb ? o.kb : "");
  } else {
    print_body(&resp);
  }
  kbc_arena_free(a);
  kbc_str_free(&resp);
  return EXIT_OK;
}

static int cmd_status(int argc, char **argv, int start) {
  opts o;
  parse_verb(FLAGS_NONE, argc, argv, start, &o);
  int status = 0;
  kbc_str resp;
  kbc_err e;
  kbc_err_reset(&e);
  kbc_status s = http_call("GET", R_STATS, NULL, 0, &status, &resp, &e);
  if (kbc_failed(s)) {
    return call_failed(s, &e, "status");
  }
  if (status < 200 || status >= 300) {
    kbc_str_free(&resp);
    die_io("status: HTTP %d", status);
  }
  if (g_json) {
    print_body(&resp);
    kbc_str_free(&resp);
    return EXIT_OK;
  }
  kbc_json *root = NULL;
  kbc_arena *a = resp_arena(&resp, &root, "status");
  static const char *const counters[] = {
    "artifacts_indexed", "reindex_runs",    "searches_served",
    "searches_degraded", "last_reindex_ns", "last_reindex_docs",
    "last_reindex_us",   "index_terms",     "index_docs",
    "db_bytes",          NULL
  };
  bool any = false;
  for (size_t i = 0; counters[i] != NULL; i++) {
    const kbc_json *v = kbc_json_get(root, counters[i]);
    if (v != NULL && kbc_json_is(v, KBC_JSON_NUM)) {
      printf("%-19s %.0f\n", counters[i], v->u.num);
      any = true;
    }
  }
  if (!any) {
    print_body(&resp);
  } else {
    printf("%-19s %s\n", "daemon", resolve_base_url());
  }
  kbc_arena_free(a);
  kbc_str_free(&resp);
  return EXIT_OK;
}

static int cmd_add(int argc, char **argv, int start) {
  opts o;
  parse_verb(FLAGS_KB, argc, argv, start, &o);
  const char *dir = positional(&o, 0, "add <dir>");
  const char *kb = o.has_kb ? o.kb : "default";
  if (kb[0] == '\0') {
    die_user("--kb: the corpus name is empty");
  }
  char *expanded = expand_tilde(dir);
  char resolved[KBC_MAX_PATH_LEN + 1];
  if (realpath(expanded, resolved) == NULL) {
    die_user("source path not found: %s: %s", expanded, strerror(errno));
  }
  struct stat st;
  if (stat(resolved, &st) != 0) {
    die_user("stat %s: %s", resolved, strerror(errno));
  }
  if (!S_ISDIR(st.st_mode)) {
    die_user("source path must be a directory: %s", resolved);
  }

  kbc_config *cfg = load_config();
  bool updated = kbc_config_corpus(cfg, kb) != NULL;
  if (updated) {
    kbc_corpus_cfg *cur = &cfg->corpora[0];
    for (size_t i = 0; i < cfg->ncorpora; i++) {
      if (cfg->corpora[i].name != NULL && strcmp(cfg->corpora[i].name, kb) == 0) {
        cur = &cfg->corpora[i];
        break;
      }
    }
    free(cur->path);
    cur->path = xstrdup(resolved);
  } else {
    if (cfg->ncorpora >= KBC_MAX_CORPORA) {
      die_user("already at the %u-corpus limit", KBC_MAX_CORPORA);
    }
    kbc_corpus_cfg *grown = realloc(cfg->corpora,
                                    (cfg->ncorpora + 1) * sizeof *grown);
    if (grown == NULL) {
      die_io("%s", "out of memory");
    }
    cfg->corpora = grown;
    kbc_corpus_cfg *slot = &cfg->corpora[cfg->ncorpora];
    memset(slot, 0, sizeof *slot);
    slot->name = xstrdup(kb);
    slot->path = xstrdup(resolved);
    kbc_strlist_init(&slot->ignore);
    cfg->ncorpora++;
  }

  char *cfg_path = resolve_config_path();
  if (cfg_path == NULL) {
    die_user("no config path: pass --config PATH or set HOME");
  }
  kbc_err e;
  kbc_err_reset(&e);
  kbc_str toml;
  kbc_str_init(&toml);
  kbc_status s = kbc_config_dump(cfg, &toml, &e);
  if (kbc_failed(s)) {
    exit(status_exit(s, &e, "config"));
  }
  s = kbc_str_write_file_atomic(cfg_path, toml.ptr, toml.len, &e);
  if (kbc_failed(s)) {
    exit(status_exit(s, &e, cfg_path));
  }
  kbc_str_free(&toml);
  free(cfg_path);
  free(expanded);

  /* Refresh: the corpus is registered, so index it now rather than making
   * the operator run a second command to see the documents. */
  kbc_app *app = kbc_app_open(cfg, &e);
  if (app == NULL) {
    /* The status kbc_app_open actually failed with, not a blanket IO: a
     * corrupt index is a PARSE, and fail_status only prints the reason when it
     * matches the status it is given — so a hardcoded KBC_ERR_IO threw away the
     * one line that says WHICH file is bad and WHY, leaving the operator with
     * "open the store: io". */
    exit(status_exit(e.status ? e.status : KBC_ERR_IO, &e, "open the store"));
  }
  s = kbc_app_reindex(app, &e);
  if (kbc_failed(s)) {
    kbc_app_close(app);
    exit(status_exit(s, &e, "reindex"));
  }
  kbc_app_stats stats;
  if (kbc_failed(kbc_app_stats_get(app, &stats, &e))) {
    memset(&stats, 0, sizeof stats);
  }
  kbc_app_close(app);

  if (g_json) {
    kbc_str out;
    kbc_str_init(&out);
    (void)kbc_str_puts(&out, "{\"ok\":true,\"kb\":");
    (void)kbc_str_append_json_string(&out, kb, strlen(kb));
    (void)kbc_str_puts(&out, ",\"path\":");
    (void)kbc_str_append_json_string(&out, resolved, strlen(resolved));
    (void)kbc_str_printf(&out, ",\"documents\":%lld",
                         (long long)stats.artifacts_indexed);
    (void)kbc_str_puts(&out, "}");
    print_body(&out);
    kbc_str_free(&out);
    return EXIT_OK;
  }
  printf("%s corpus %s -> %s (%lld documents)\n",
         updated ? "updated" : "added", kb, resolved,
         (long long)stats.artifacts_indexed);
  return EXIT_OK;
}

static int cmd_config_show(int argc, char **argv, int start) {
  opts o;
  parse_verb(FLAGS_NONE, argc, argv, start, &o);
  const kbc_config *cfg = load_config();
  kbc_str toml;
  kbc_str_init(&toml);
  kbc_err e;
  kbc_err_reset(&e);
  kbc_status s = kbc_config_dump(cfg, &toml, &e);
  if (kbc_failed(s)) {
    exit(status_exit(s, &e, "config"));
  }
  if (!g_json) {
    print_body(&toml);
    kbc_str_free(&toml);
    return EXIT_OK;
  }
  const char *cfg_path = cfg->config_path != NULL ? cfg->config_path : "";
  const char *data_dir = cfg->data_dir != NULL ? cfg->data_dir : "";
  const char *token_path = cfg->token_path != NULL ? cfg->token_path : "";
  kbc_str out;
  kbc_str_init(&out);
  (void)kbc_str_puts(&out, "{\"ok\":true,\"config_path\":");
  (void)kbc_str_append_json_string(&out, cfg_path, strlen(cfg_path));
  (void)kbc_str_puts(&out, ",\"data_dir\":");
  (void)kbc_str_append_json_string(&out, data_dir, strlen(data_dir));
  (void)kbc_str_puts(&out, ",\"bind_addr\":");
  (void)kbc_str_append_json_string(&out, cfg->bind_addr, strlen(cfg->bind_addr));
  (void)kbc_str_printf(&out, ",\"port\":%d", cfg->port);
  (void)kbc_str_puts(&out, ",\"token_path\":");
  (void)kbc_str_append_json_string(&out, token_path, strlen(token_path));
  (void)kbc_str_puts(&out, ",\"corpora\":[");
  for (size_t i = 0; i < cfg->ncorpora; i++) {
    if (i > 0) {
      (void)kbc_str_putc(&out, ',');
    }
    (void)kbc_str_puts(&out, "{\"name\":");
    (void)kbc_str_append_json_string(&out, cfg->corpora[i].name,
                                     strlen(cfg->corpora[i].name));
    (void)kbc_str_puts(&out, ",\"path\":");
    (void)kbc_str_append_json_string(&out, cfg->corpora[i].path,
                                     strlen(cfg->corpora[i].path));
    (void)kbc_str_putc(&out, '}');
  }
  (void)kbc_str_puts(&out, "]}");
  print_body(&out);
  kbc_str_free(&out);
  kbc_str_free(&toml);
  return EXIT_OK;
}

static int cmd_token_generate(int argc, char **argv, int start) {
  opts o;
  parse_verb(FLAGS_NONE, argc, argv, start, &o);
  const kbc_config *cfg = load_config();
  if (cfg->token_path == NULL) {
    die_user("no token path configured: pass --config PATH or set HOME");
  }
  if (kbc_path_exists(cfg->token_path)) {
    die_user("token file already exists at %s", cfg->token_path);
  }
  unsigned char raw[TOKEN_BYTES];
  int fd = open("/dev/urandom", O_RDONLY);
  if (fd < 0) {
    die_io("open /dev/urandom: %s", strerror(errno));
  }
  size_t got = 0;
  while (got < sizeof raw) {
    ssize_t r = read(fd, raw + got, sizeof raw - got);
    if (r < 0) {
      if (errno == EINTR) {
        continue;
      }
      int saved = errno;
      close(fd);
      die_io("read /dev/urandom: %s", strerror(saved));
    }
    if (r == 0) {
      break;
    }
    got += (size_t)r;
  }
  close(fd);
  if (got != sizeof raw) {
    die_io("read /dev/urandom: short read (%zu of %zu bytes)", got,
           sizeof raw);
  }

  kbc_str b64;
  kbc_str_init(&b64);
  kbc_err e;
  kbc_err_reset(&e);
  if (kbc_failed(kbc_base64_encode(&b64, raw, sizeof raw))) {
    die_io("base64: out of memory");
  }
  /* url-safe and unpadded: the value goes into an HTTP header unquoted. */
  for (size_t i = 0; i < b64.len; i++) {
    if (b64.ptr[i] == '+') {
      b64.ptr[i] = '-';
    } else if (b64.ptr[i] == '/') {
      b64.ptr[i] = '_';
    } else if (b64.ptr[i] == '=') {
      b64.len = i;
      break;
    }
  }
  kbc_str line;
  kbc_str_init(&line);
  (void)kbc_str_append(&line, b64.ptr, b64.len);
  (void)kbc_str_putc(&line, '\n');
  kbc_str_free(&b64);

  char *dir = dir_of(cfg->token_path);
  if (dir != NULL) {
    (void)kbc_mkdir_p(dir, &e);
    free(dir);
  }
  /* O_EXCL: two `kbc token generate` racing must not both "win". */
  int tfd = open(cfg->token_path, O_WRONLY | O_CREAT | O_EXCL, 0600);
  if (tfd < 0) {
    die_io("open %s: %s", cfg->token_path, strerror(errno));
  }
  size_t off = 0;
  while (off < line.len) {
    ssize_t w = write(tfd, line.ptr + off, line.len - off);
    if (w < 0) {
      if (errno == EINTR) {
        continue;
      }
      int saved = errno;
      close(tfd);
      die_io("write %s: %s", cfg->token_path, strerror(saved));
    }
    off += (size_t)w;
  }
  kbc_str_free(&line);
  if (fsync(tfd) != 0) {
    int saved = errno;
    close(tfd);
    die_io("write %s: %s", cfg->token_path, strerror(saved));
  }
  if (close(tfd) != 0) {
    die_io("close %s: %s", cfg->token_path, strerror(errno));
  }
  if (g_json) {
    kbc_str out;
    kbc_str_init(&out);
    (void)kbc_str_puts(&out, "{\"ok\":true,\"token_path\":");
    (void)kbc_str_append_json_string(&out, cfg->token_path,
                                     strlen(cfg->token_path));
    (void)kbc_str_puts(&out, "}");
    print_body(&out);
    kbc_str_free(&out);
    return EXIT_OK;
  }
  printf("wrote %s (mode 0600)\n", cfg->token_path);
  return EXIT_OK;
}

static int cmd_version(void) {
  if (g_json) {
    printf("{\"ok\":true,\"name\":\"%s\",\"version\":\"%s\"}\n", KBC_PROJECT,
           KBC_VERSION);
    return EXIT_OK;
  }
  printf("%s %s\n", KBC_PROJECT, KBC_VERSION);
  return EXIT_OK;
}

/* ----------------------------------------------------------------- prune ---
 *
 * `kbc prune [--days N] [--apply]` — the operator-triggered retention sweep.
 *
 * WHAT THE ORIGINAL PRUNES, because the name invites a wrong answer.
 * `Db::retention_prune` (kb-core/src/storage/sqlite.rs:2361) deletes rows
 * from THREE tables, and every one of them is *history*:
 *
 *   DELETE FROM reading_sections WHERE last_at < cutoff
 *   DELETE FROM reading_sections
 *          WHERE visit_id IN (SELECT id FROM history WHERE started_at < ?)
 *   DELETE FROM history WHERE started_at < cutoff
 *
 * plus `memory_recalls` rows whose artifact_id is a served-* id, sharing the
 * history cutoff. It NEVER deletes a document. Its own doc comment says so
 * (sqlite.rs:2357): it "deliberately does NOT manage the R2-cascade tables
 * (sessions/edges/…); those are pruned per-artifact on delete, not by age
 * (and `edges` has no timestamp to prune on)". The config comment says it
 * again (config.rs:470): edges "has NO timestamp column, so it CANNOT be
 * time-pruned and is deliberately absent here".
 *
 * So the store-vs-index question this verb was designed around does not
 * arise: there is no document to remove, and therefore no store that can
 * disagree with the index about one. `kbc_app_delete_path` is the ONE
 * removal in this port and retention deliberately does not call it — wiring
 * it in would invent a corpus-deleting feature the original does not have,
 * and it is the single most dangerous line anyone could add to this file.
 *
 * THE THRESHOLD IS THE ORIGINAL'S. `[retention] history_days` is an
 * `Option<u32>` in days; `None` means keep forever, so an absent window is a
 * no-op rather than a default, and `--days 0` is refused for the reason
 * config.rs:1932 gives. A day count is the unit and the unit is unchanged.
 *
 * WHY A CLI FLAG AND NOT `[retention]`: kb-c's config is frozen (kbc_config
 * has no retention field) and the original's sweep is a background daemon
 * task, which a one-shot binary has no way to schedule. The flag is the
 * operator-triggered equivalent of the same window.
 *
 * WHY DRY RUN BY DEFAULT. The original's sweep has no dry run because it
 * cannot be run by hand — it is armed by config and fires on a timer, so
 * "delete on sight" was never reachable. A CLI verb puts the delete on the
 * command line, where a typo is one keypress away, so the safety moves to
 * the invocation: without `--apply` this reports and changes nothing. That
 * is a deliberate divergence and the reason is stated rather than assumed.
 */


static int cmd_prune(int argc, char **argv, int start) {
  opts o;
  parse_verb(FLAGS_PRUNE, argc, argv, start, &o);
  if (o.npos != 0) {
    die_user("prune takes no positional argument: kbc prune --days N%s",
             o.apply ? " --apply" : "");
  }
  /* An absent window is the original's `None` — keep forever — so there is
   * nothing to do. It is a user error rather than a silent success because
   * the operator asked a verb named `prune` to prune, and exiting 0 having
   * removed nothing is the reading that hides a typo in the flag name. */
  if (!o.has_days) {
    die_user("prune needs --days N (an unset retention window means keep "
             "forever, so there is nothing to prune)");
  }

  const kbc_config *cfg = load_config();
  if (cfg->db_path == NULL || cfg->db_path[0] == '\0') {
    die_user("no database configured: pass --config PATH or set HOME");
  }
  if (!kbc_path_exists(cfg->db_path)) {
    die_user("prune: no store at %s — run `kbc add <dir> --kb NAME` first",
             cfg->db_path);
  }

  /* The cutoff, exactly as the original computes it: `now - window`, with
   * the window in seconds. `days` is already capped at PRUNE_MAX_DAYS, so
   * the product cannot overflow int64 — the cap is load-bearing here and not
   * only a tidiness bound. */
  int64_t now = (int64_t)time(NULL);
  int64_t window = (int64_t)o.days * 86400;
  int64_t cutoff = now - window;
  if (cutoff < 0) {
    /* A window longer than the epoch itself: nothing that exists is older
     * than a date before 1970, so the answer is zero rather than a wrapped
     * negative cutoff that would match every row. The original's
     * `saturating_sub` lands in the same place for the same reason. */
    cutoff = 0;
  }

  /* The store is the ONLY writer, so the DELETE goes through it and under its
   * mutex. An earlier draft of this verb opened its own sqlite3 connection
   * and ran the statement here, on the reasoning that `backup` already does
   * something similar for VACUUM INTO. That reasoning does not hold now that
   * `kbc_store_prune_history` exists: a second connection to the same file is
   * a second writer outside the mutex, and a retention pass is exactly the
   * operation where racing another writer matters. The vacuum_into precedent
   * is a read plus a whole-file copy; this is a row delete.
   *
   * A live daemon holding the same db is not a conflict: the store runs WAL
   * with a 5s busy_timeout (src/store.c:552), so the two serialise. Same
   * posture `comments` takes, and the reason it can. */
  kbc_err e;
  kbc_err_reset(&e);
  kbc_store *store = kbc_store_open(cfg, &e);
  if (store == NULL) {
    fail_status(e.status != KBC_OK ? e.status : KBC_ERR_IO, &e, "prune",
                NULL);
  }
  int64_t rows = 0;
  kbc_status st = kbc_store_prune_history(store, cutoff, o.apply, &rows, &e);
  if (kbc_failed(st)) {
    kbc_store_close(store);
    fail_status(st, &e, "prune", NULL);
  }
  /* Reclaim the freed pages, best-effort, exactly as the original does
   * (sqlite.rs:2426: `let _ = ... wal_checkpoint(TRUNCATE)`). Under WAL the
   * file does not shrink until something checkpoints, so without this a
   * long-lived database keeps the file size of a corpus that shrank — the
   * database stops growing but the disk is never given back. The status is
   * discarded deliberately and the reason is in store.h: a failed reclaim
   * costs disk, and rolling the delete back to make the reclaim succeed
   * would cost the operator rows.
   *
   * AFTER the delete, and never inside it: the delete is already committed,
   * so the worst case here is a large file, not lost history. */
  if (o.apply) {
    (void)kbc_store_checkpoint(store, &e);
  }
  kbc_store_close(store);

  if (g_json) {
    kbc_str out;
    kbc_str_init(&out);
    (void)kbc_str_printf(&out,
                         "{\"ok\":true,\"apply\":%s,\"days\":%zu,"
                         "\"cutoff\":%lld,\"rows\":%lld}\n",
                         o.apply ? "true" : "false", o.days,
                         (long long)cutoff, (long long)rows);
    emit_line(&out);
  } else if (o.apply) {
    printf("pruned %lld history row%s older than %zu day%s\n",
           (long long)rows, rows == 1 ? "" : "s", o.days,
           o.days == 1 ? "" : "s");
  } else if (rows == 0) {
    printf("no history rows older than %zu day%s; nothing to prune\n", o.days,
           o.days == 1 ? "" : "s");
  } else {
    printf("would prune %lld history row%s older than %zu day%s\n",
           (long long)rows, rows == 1 ? "" : "s", o.days,
           o.days == 1 ? "" : "s");
    printf("  re-run with --apply to delete them\n");
  }
  return EXIT_OK;
}

/* --------------------------------------------------------------- metrics ---
 *
 * `kbc metrics [--out PATH]` — print (or save) the daemon's Prometheus text
 * exposition.
 *
 * WHAT THE ORIGINAL IS, because PORT_PLAN's phrase "metrics export" does not
 * settle it. `kb metrics` (kb-cli/src/commands/metrics.rs:1-2) is "print the
 * daemon's `GET /api/metrics` snapshot" — a CLIENT. It fetches, it
 * pretty-prints, and it writes to stdout. There is no push to a collector,
 * no offline dump, and no scrape-to-file anywhere in it. So this is not a
 * network feature: it is one GET against a route kb-c already serves, and
 * the endpoint being already built is what makes it nearly free.
 *
 * The one divergence is `--out`, and it is the one PORT_PLAN's word
 * "export" asks for: the Rust's verb cannot save its scrape, so `kb metrics
 * > file` is the only way to keep one, and a shell redirect is not something
 * a verb can be tested through.
 *
 * NO LOCAL FALLBACK, unlike `search`/`reindex`/`get`/`list`. Every counter
 * this endpoint renders lives in the running daemon's `metrics_reg`; an
 * in-process app has no daemon, so a fallback would print a full exposition
 * of zeroes that reads exactly like a healthy idle daemon. `kbc status` sets
 * the precedent and its reason: no fallback, exit 2 when no daemon answers.
 */
/* Defined in the backup section below, which is where the tar/destination
 * path rules already live. Declared rather than moved: `--out` on a metrics
 * scrape is the same "a path the user typed" problem, and it must go through
 * the same bounded check, not a second one written to suit its caller. */
static char *bounded_path(const char *what, const char *p);

static int cmd_metrics(int argc, char **argv, int start) {
  opts o;
  parse_verb(FLAGS_METRICS, argc, argv, start, &o);
  if (o.npos != 0) {
    die_user("metrics takes no positional argument");
  }
  int status = 0;
  kbc_str resp;
  kbc_err e;
  kbc_err_reset(&e);
  kbc_status s = http_call("GET", R_METRICS, NULL, 0, &status, &resp, &e);
  if (kbc_failed(s)) {
    return call_failed(s, &e, "metrics");
  }
  if (status < 200 || status >= 300) {
    kbc_str_free(&resp);
    die_io("metrics: HTTP %d", status);
  }
  if (o.out != NULL) {
    /* The same shape `restore` uses for a path the user typed: expand
     * `~`, then bound-check. Assembled with an explicit length check rather
     * than a snprintf into a fixed buffer, because `-Werror=format-
     * truncation` is on and a truncated path is a DIFFERENT path — it would
     * write the scrape somewhere the operator did not name. */
    char *expanded = expand_tilde(o.out);
    char *path = bounded_path("metrics --out", expanded);
    free(expanded);
    kbc_err_reset(&e);
    if (kbc_failed(kbc_str_write_file_atomic(path, resp.ptr, resp.len, &e))) {
      int rc = fail_status(e.status != KBC_OK ? e.status : KBC_ERR_IO, &e,
                           "metrics --out", NULL);
      free(path);
      kbc_str_free(&resp);
      return rc;
    }
    if (!g_json) {
      printf("wrote %zu bytes of metrics to %s\n", resp.len, path);
    }
    free(path);
    kbc_str_free(&resp);
    return EXIT_OK;
  }
  /* `--json` is deliberately NOT honoured. The body is Prometheus text
   * exposition, not JSON, and wrapping it in a JSON string would give a
   * scraper something it cannot parse and an operator something that is not
   * what `curl /metrics` returned. */
  print_body(&resp);
  kbc_str_free(&resp);
  return EXIT_OK;
}

/* ----------------------------------------------------------------- bench --- */

static int cmp_u64(const void *a, const void *b) {
  uint64_t x = *(const uint64_t *)a;
  uint64_t y = *(const uint64_t *)b;
  return x < y ? -1 : (x > y ? 1 : 0);
}

static int cmp_term(const void *a, const void *b) {
  return strcmp(*(const char *const *)a, *(const char *const *)b);
}

/* Nearest-rank percentile over an ascending sample. */
static uint64_t percentile(const uint64_t *sorted, size_t n, double p) {
  if (n == 0) {
    return 0;
  }
  double rank = p * (double)n;
  size_t idx = (size_t)rank;
  if ((double)idx < rank) {
    idx++;
  }
  if (idx == 0) {
    idx = 1;
  }
  if (idx > n) {
    idx = n;
  }
  return sorted[idx - 1];
}

static int cmd_bench(int argc, char **argv, int start) {
  opts o;
  parse_verb(FLAGS_BENCH, argc, argv, start, &o);
  size_t queries = o.has_queries ? o.queries : BENCH_DEFAULT_QUERIES;
  size_t repeats = o.has_repeats ? o.repeats : BENCH_DEFAULT_REPEATS;
  const kbc_config *base = load_config();
  const char *corpus = o.corpus;
  if (corpus == NULL) {
    if (base->ncorpora == 0) {
      die_user("no corpus configured: pass --corpus DIR or run `kbc add <dir> --kb NAME`");
    }
    corpus = base->corpora[0].path;
  }
  struct stat cst;
  if (stat(corpus, &cst) != 0) {
    die_user("corpus %s: %s", corpus, strerror(errno));
  }
  if (!S_ISDIR(cst.st_mode)) {
    die_user("corpus %s is not a directory", corpus);
  }

  /* A private data dir: the bench must not touch the daemon's db or index. */
  char tmpl[] = "/tmp/kbc-bench-XXXXXX";
  char *tmpdir = mkdtemp(tmpl);
  if (tmpdir == NULL) {
    die_io("mkdtemp /tmp/kbc-bench-XXXXXX: %s", strerror(errno));
  }
  kbc_config *cfg = kbc_config_defaults();
  if (cfg == NULL) {
    rmdir(tmpdir);
    die_io("%s", "out of memory");
  }
  char *dbp = path_join(tmpdir, "bench.db");
  char *ixp = path_join(tmpdir, "bench.idx");
  free(cfg->db_path);
  free(cfg->index_path);
  cfg->db_path = dbp;   /* owned by cfg from here; unlinked via these pointers */
  cfg->index_path = ixp;
  cfg->ncorpora = 0;
  free(cfg->corpora);
  cfg->corpora = calloc(1, sizeof *cfg->corpora);
  if (cfg->corpora == NULL) {
    rmdir(tmpdir);
    die_io("%s", "out of memory");
  }
  cfg->corpora[0].name = xstrdup("bench");
  cfg->corpora[0].path = xstrdup(corpus);
  kbc_strlist_init(&cfg->corpora[0].ignore);
  cfg->ncorpora = 1;
  free(cfg->token);
  cfg->token = NULL;
  free(cfg->embedder_cmd);
  cfg->embedder_cmd = NULL; /* no sidecar: the bench measures the keyword lane */

  kbc_err e;
  kbc_err_reset(&e);
  int rc = EXIT_OK;
  int64_t ingest_us = 0;
  kbc_app_stats stats;
  memset(&stats, 0, sizeof stats);
  kbc_app *app = kbc_app_open(cfg, &e);
  if (app == NULL) {
    rc = status_exit(e.status ? e.status : KBC_ERR_IO, &e,
                     "bench: open the store");
    goto cleanup;
  }
  {
    int64_t t0 = kbc_now_ns();
    kbc_status s = kbc_app_reindex(app, &e);
    ingest_us = (kbc_now_ns() - t0) / 1000;
    if (kbc_failed(s)) {
      rc = status_exit(s, &e, "bench: ingest");
      kbc_app_close(app);
      goto cleanup;
    }
  }
  if (kbc_failed(kbc_app_stats_get(app, &stats, &e))) {
    rc = status_exit(KBC_ERR_IO, &e, "bench: stats");
    kbc_app_close(app);
    goto cleanup;
  }

  /* The query set is the index's own sorted term list, so the numbers are
   * reproducible from the header we print and need no label file. */
  kbc_index *built = kbc_index_open(cfg->index_path, &e);
  if (built == NULL) {
    rc = status_exit(KBC_ERR_IO, &e, "bench: open the index");
    kbc_app_close(app);
    goto cleanup;
  }
  kbc_arena *ta = kbc_arena_new(64u * 1024u);
  if (ta == NULL) {
    kbc_index_free(built);
    kbc_app_close(app);
    rc = die_io("%s", "out of memory");
    goto cleanup;
  }
  kbc_strlist terms;
  kbc_strlist_init(&terms);
  if (kbc_failed(kbc_index_expand_prefix(built, ta, "", &terms, &e))) {
    kbc_strlist_free(&terms);
    kbc_arena_free(ta);
    kbc_index_free(built);
    kbc_app_close(app);
    rc = status_exit(KBC_ERR_IO, &e, "bench: terms");
    goto cleanup;
  }
  kbc_index_free(built);
  if (terms.len == 0) {
    kbc_strlist_free(&terms);
    kbc_arena_free(ta);
    kbc_app_close(app);
    rc = die_user("bench: %s has no indexable terms", corpus);
    goto cleanup;
  }
  qsort(terms.items, terms.len, sizeof *terms.items, cmp_term);
  if (queries > terms.len) {
    queries = terms.len;
  }

  size_t samples = queries * repeats;
  uint64_t *lat = malloc(samples * sizeof *lat);
  if (lat == NULL) {
    kbc_strlist_free(&terms);
    kbc_arena_free(ta);
    kbc_app_close(app);
    rc = die_io("%s", "out of memory");
    goto cleanup;
  }
  size_t got = 0;
  int64_t hits_total = 0;
  for (size_t qi = 0; qi < queries; qi++) {
    for (size_t ri = 0; ri < repeats; ri++) {
      kbc_arena *qa = kbc_arena_new(32u * 1024u);
      if (qa == NULL) {
        break;
      }
      kbc_query q;
      memset(&q, 0, sizeof q);
      q.q = terms.items[qi];
      q.mode = KBC_MODE_KEYWORD;
      q.limit = 10;
      q.rrf_k = cfg->rrf_k;
      q.bm25_k1 = cfg->bm25_k1;
      q.bm25_b = cfg->bm25_b;
      kbc_search_result res;
      memset(&res, 0, sizeof res);
      int64_t t0 = kbc_now_ns();
      kbc_status s = kbc_app_search(app, qa, &q, &res, &e);
      int64_t dt = kbc_now_ns() - t0;
      if (kbc_failed(s)) {
        kbc_arena_free(qa);
        free(lat);
        kbc_strlist_free(&terms);
        kbc_arena_free(ta);
        kbc_app_close(app);
        rc = status_exit(s, &e, "bench: query");
        goto cleanup;
      }
      hits_total += (int64_t)res.len;
      lat[got++] = (uint64_t)(dt / 1000);
      kbc_arena_free(qa);
    }
  }
  kbc_strlist_free(&terms);
  kbc_arena_free(ta);
  kbc_app_close(app);
  if (got == 0) {
    free(lat);
    rc = die_io("bench: no query completed");
    goto cleanup;
  }
  qsort(lat, got, sizeof *lat, cmp_u64);
  uint64_t min = lat[0];
  uint64_t p50 = percentile(lat, got, 0.50);
  uint64_t p95 = percentile(lat, got, 0.95);
  uint64_t p99 = percentile(lat, got, 0.99);

  if (g_json) {
    kbc_str out;
    kbc_str_init(&out);
    (void)kbc_str_puts(&out, "{\"ok\":true,\"corpus\":");
    (void)kbc_str_append_json_string(&out, corpus, strlen(corpus));
    (void)kbc_str_printf(
        &out,
        ",\"documents\":%lld,\"terms\":%lld,\"queries\":%zu,\"repeats\":%zu,"
        "\"samples\":%zu,\"hits\":%lld,\"ingest_us\":%lld,\"min_us\":%llu,"
        "\"p50_us\":%llu,\"p95_us\":%llu,\"p99_us\":%llu}",
        (long long)stats.index_docs, (long long)stats.index_terms, queries,
        repeats, got, (long long)hits_total, (long long)ingest_us,
        (unsigned long long)min, (unsigned long long)p50,
        (unsigned long long)p95, (unsigned long long)p99);
    (void)kbc_str_putc(&out, '}');
    print_body(&out);
    kbc_str_free(&out);
  } else {
    printf("corpus:    %s\n", corpus);
    printf("documents: %lld\n", (long long)stats.index_docs);
    printf("terms:     %lld\n", (long long)stats.index_terms);
    printf("queries:   %zu (repeats: %zu, samples: %zu)\n", queries, repeats, got);
    printf("ingest:    %lld us\n", (long long)ingest_us);
    printf("min:       %llu us\n", (unsigned long long)min);
    printf("p50:       %llu us\n", (unsigned long long)p50);
    printf("p95:       %llu us\n", (unsigned long long)p95);
    printf("p99:       %llu us\n", (unsigned long long)p99);
  }
  free(lat);

cleanup:
  /* The app is closed on every path that reaches here, so the files are
   * ours to remove. sqlite's WAL sidecars go with them, or rmdir fails. */
  unlink(dbp);
  unlink(ixp);
  {
    static const char *const suffix[] = { "-wal", "-shm", NULL };
    size_t dbn = strlen(dbp);
    for (size_t i = 0; suffix[i] != NULL; i++) {
      char *side = malloc(dbn + strlen(suffix[i]) + 1);
      if (side != NULL) {
        memcpy(side, dbp, dbn);
        memcpy(side + dbn, suffix[i], strlen(suffix[i]) + 1);
        unlink(side);
        free(side);
      }
    }
  }
  kbc_config_free(cfg); /* frees db_path and index_path with it */
  rmdir(tmpdir);
  return rc;
}

/* --------------------------------------------------------------- daemon ---- */

static volatile sig_atomic_t g_stop;
/* The pid-file lifecycle, implemented with `daemon stop` far below because
 * that is where its contract is written down. Declared here so `cmd_daemon`
 * can run the check BEFORE it forks: a double start has to fail the command
 * the operator ran, and a check made only in the child would be reported by
 * a process that already exited 0. */
static char *daemon_pid_path(void);
static long read_daemon_pid(const char *path);
static bool pid_is_alive(long pid);
static void daemon_write_pidfile(const char *path);

static void on_stop_signal(int sig) {
  (void)sig;
  g_stop = 1; /* the only async-signal-safe thing done here */
}

static void install_signal_handlers(void) {
  struct sigaction sa;
  memset(&sa, 0, sizeof sa);
  sa.sa_handler = on_stop_signal;
  sigemptyset(&sa.sa_mask);
  sa.sa_flags = 0; /* deliberately no SA_RESTART: nanosleep must return EINTR */
  if (sigaction(SIGINT, &sa, NULL) != 0 || sigaction(SIGTERM, &sa, NULL) != 0) {
    die_io("sigaction: %s", strerror(errno));
  }
  struct sigaction ign;
  memset(&ign, 0, sizeof ign);
  ign.sa_handler = SIG_IGN;
  sigemptyset(&ign.sa_mask);
  if (sigaction(SIGPIPE, &ign, NULL) != 0) {
    die_io("sigaction(SIGPIPE): %s", strerror(errno));
  }
}

static int cmd_daemon(int argc, char **argv, int start) {
  opts o;
  parse_verb(FLAGS_DAEMON, argc, argv, start, &o);
  kbc_config *cfg = load_config();
  if (o.has_bind) {
    free(cfg->bind_addr);
    cfg->bind_addr = xstrdup(o.bind);
  }
  if (o.has_port) {
    cfg->port = (int)o.port;
  }
  kbc_err e;
  kbc_err_reset(&e);
  /* Resolve the token BEFORE validate and before the bind guard: both read
   * cfg->token, and a configured-but-unreadable token file must be a startup
   * failure rather than a silently unauthenticated daemon on a public bind. */
  kbc_status s = kbc_config_load_token(cfg, &e);
  if (kbc_failed(s)) {
    exit(status_exit(s, &e, "token"));
  }
  s = kbc_config_validate(cfg, &e);
  if (kbc_failed(s)) {
    exit(status_exit(s, &e, "config"));
  }
  char why[KBC_ERR_MSG_MAX];
  if (!kbc_config_bind_is_safe(cfg, why, sizeof why)) {
    die_user("%s", why[0] != '\0' ? why : "refusing an unsafe bind");
  }

  kbc_log_init(cfg->log_level, cfg->json_logs);

  /* The pid file is named before the fork so the parent can run the
   * double-start check and the child can own the file. The check happens in
   * the PARENT deliberately: `kbc daemon` returns 0 the moment it forks, so
   * a check made only in the child would be reported by a process that had
   * already told the operator it succeeded. */
  char *pid_path = daemon_pid_path();
  long recorded = read_daemon_pid(pid_path);
  if (recorded != 0 && recorded != (long)getpid() && pid_is_alive(recorded)) {
    kbc_str msg;
    kbc_str_init(&msg);
    line_puts(&msg, "another kb daemon is already running (pid ");
    {
      char pidbuf[24];
      (void)snprintf(pidbuf, sizeof pidbuf, "%ld", recorded);
      line_puts(&msg, pidbuf);
    }
    line_puts(&msg, ", file: ");
    put_shown(&msg, pid_path);
    line_puts(&msg, "); use `kbc daemon stop` to terminate it first");
    die_user("%s", msg.ptr);
  }

  if (!o.foreground) {
    pid_t pid = fork();
    if (pid < 0) {
      die_io("fork: %s", strerror(errno));
    }
    if (pid > 0) {
      /* The parent exits here and the child needs `pid_path`, so this is the
       * parent's only chance to free it. `die_*` above exit without reaching
       * this, which is a bounded one-shot leak on a path that ends the
       * process, but the ordinary success path is not that and should not
       * leak either — LeakSanitizer is right about this one. */
      free(pid_path);
      return EXIT_OK;
    }
    if (setsid() < 0) {
      KBC_LOGE("setsid: %s", strerror(errno));
    }
    int devnull = open("/dev/null", O_RDWR);
    if (devnull >= 0) {
      (void)dup2(devnull, STDIN_FILENO);
      (void)dup2(devnull, STDOUT_FILENO);
      /* stderr is deliberately left attached: the daemon's log is the only
       * way an operator sees it, and a log line is never stdout. */
      if (devnull > STDERR_FILENO) {
        close(devnull);
      }
    }
  }
  install_signal_handlers();

  kbc_app *app = kbc_app_open(cfg, &e);
  if (app == NULL) {
    exit(status_exit(e.status ? e.status : KBC_ERR_IO, &e, "open the store"));
  }
  if (kbc_failed(kbc_app_start_watcher(app, &e))) {
    kbc_app_close(app);
    exit(status_exit(KBC_ERR_IO, &e, "watcher"));
  }
  kbc_httpd *h = kbc_httpd_start(app, cfg, &e);
  if (h == NULL) {
    kbc_app_stop_watcher(app);
    kbc_app_close(app);
    exit(status_exit(KBC_ERR_IO, &e, "httpd"));
  }
  /* AFTER the bind, so a daemon that failed to start leaves no pid file
   * claiming a process that is not there. */
  daemon_write_pidfile(pid_path);
  kbc_app_stats stats;
  if (kbc_failed(kbc_app_stats_get(app, &stats, &e))) {
    memset(&stats, 0, sizeof stats);
  }
  /* Greppable startup line: address, corpora, index shape, db size. */
  KBC_LOGI("listening on %s:%d corpora=%zu artifacts=%lld index_docs=%lld "
           "index_terms=%lld db_bytes=%lld",
           cfg->bind_addr, kbc_httpd_port(h), cfg->ncorpora,
           (long long)stats.artifacts_indexed, (long long)stats.index_docs,
           (long long)stats.index_terms, (long long)stats.db_bytes);
  /* Whether the bearer tier is live. Never the value: this line goes to the
   * operator's terminal and into whatever collects the daemon's stderr. */
  KBC_LOGI("bearer token %s (source: %s)",
           (cfg->token != NULL && cfg->token[0] != '\0') ? "active" : "not configured",
           (cfg->token != NULL && cfg->token[0] != '\0') ? cfg->token_path
                                                          : "none");
  if (o.foreground && !g_json) {
    printf("kbc daemon listening on http://%s:%d\n", cfg->bind_addr,
           kbc_httpd_port(h));
    fflush(stdout);
  }

  while (g_stop == 0) {
    struct timespec ts = { 0, 200 * 1000 * 1000 }; /* 200ms */
    (void)nanosleep(&ts, NULL);
  }
  KBC_LOGI("shutting down");
  /* Remove the pid file on the clean path, so `kbc daemon stop` finding one
   * means a daemon really is running. A crash leaves it behind and the next
   * start prunes it — that is the whole reason the staleness check exists. */
  (void)unlink(pid_path);
  kbc_httpd_stop(h);
  kbc_app_stop_watcher(app);
  kbc_app_close(app);
  return EXIT_OK;
}

/* --------------------------------------------------------------- backup ----
 *
 * `kbc backup <kb> [--out PATH] [--all]` and `kbc restore <tarball> --kb NAME
 * [--force]`, ported from kb-cli/src/commands/{backup,restore}.rs.
 *
 * WHY VACUUM INTO AND NOT A FILE COPY. Tar-ing the live `index.db` while the
 * daemon writes captures a torn, half-applied transaction. `VACUUM INTO` reads
 * a transactionally-consistent view under WAL and writes a fresh, standalone,
 * defragmented database — no `-wal`/`-shm` sidecars to reconcile, and it needs
 * no cooperation from the running daemon.
 *
 * THE DESTINATION IS A BOUND PARAMETER, not a formatted literal. The Rust
 * builds `"VACUUM INTO '<dest>'"` by doubling apostrophes (backup.rs:40) and
 * justifies it with "VACUUM INTO predates bound parameters on some sqlite
 * builds". kb-c links sqlite 3.27+, where the argument is an ordinary
 * expression that takes a bound parameter, and rule 9 says all SQL goes
 * through bound parameters. The formatting is dropped deliberately: a corpus
 * name or state path is operator input, and a string-formatted path into SQL
 * is the injection surface the escaping was only papering over.
 *
 * THE TARBALL LAYOUT — the archive root is the corpus name, and inside it:
 *
 *     <kb>/index.db      the sqlite snapshot (VACUUM INTO)
 *     <kb>/lance/        the vector store, when the daemon has one
 *     <kb>/.review/      per-artifact review JSON, when present
 *     slates/            the DAEMON-WIDE sibling, when the daemon has one
 *
 * `slates/` rides ALONGSIDE `<kb>/` and not inside it, because a slate keys on
 * a PROJECT and is daemon-wide state. `restore` puts it back at the same level
 * (the archive extracts into the kb-state PARENT, which IS the state dir).
 */

/* The Rust's KbName::new: 1..64 bytes of [a-z0-9_-]. A corpus name is a
 * filesystem path segment AND a tar member name AND a daemon name, so it is
 * validated once here rather than trusted at each use. */
static bool kb_name_ok(const char *kb) {
  size_t n = strlen(kb);
  if (n == 0 || n > KBC_MAX_KB_NAME) {
    return false;
  }
  for (size_t i = 0; i < n; i++) {
    char c = kb[i];
    if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' ||
          c == '-')) {
      return false;
    }
  }
  return true;
}

/* An operator-supplied path, bounded. Refused over KBC_MAX_PATH_LEN rather
 * than truncated: a truncated path is a DIFFERENT path, and silently writing
 * the backup somewhere the operator did not name is worse than refusing. The
 * offending value is ESCAPED into the message — a path carrying a newline
 * would otherwise forge a second line of stderr. */
static char *bounded_path(const char *what, const char *p) {
  if (strlen(p) >= KBC_MAX_PATH_LEN) {
    kbc_str line;
    kbc_str_init(&line);
    put_shown(&line, what);
    put_shown(&line, ": ");
    put_shown(&line, p);
    line_puts(&line, " is longer than 4096 bytes");
    char buf[KBC_ERR_MSG_MAX];
    size_t n = line.len < sizeof buf - 1u ? line.len : sizeof buf - 1u;
    memcpy(buf, line.ptr, n);
    buf[n] = '\0';
    kbc_str_free(&line);
    die_user("%s", buf);
  }
  return xstrdup(p);
}


static char *join2(const char *a, const char *b) {
  char *j = path_join(a, b);
  if (strlen(j) >= KBC_MAX_PATH_LEN) {
    die_user("path %s is longer than %u bytes", j, KBC_MAX_PATH_LEN);
  }
  return j;
}

/* There is deliberately no per-verb error function here. Every failure in
 * these two verbs goes through die_user, die_io or fail_status, so the exit
 * class is decided in ONE place and cannot drift between verbs — see the
 * exit-code contract at the top of this file. */

/* `YYYYMMDD-HHMMSS` in UTC — the Rust's chrono stamp. Seconds, not finer: the
 * shape is what makes `<kb>-<stamp>.tar.gz` recognisable, and two backups of
 * the same corpus in the same second are the same file by design. */
static char *utc_stamp(void) {
  time_t now = time(NULL);
  struct tm tm;
  if (now == (time_t)-1 || gmtime_r(&now, &tm) == NULL) {
    die_io("utc_stamp: %s", strerror(errno));
  }
  char buf[32];
  if (strftime(buf, sizeof buf, "%Y%m%d-%H%M%S", &tm) == 0) {
    die_io("utc_stamp: the clock produced no timestamp");
  }
  return xstrdup(buf);
}

/* Copies one regular file, byte for byte, creating the destination. Callers
 * pass paths they have already bounded; nothing here is operator text. */
static kbc_status copy_file(const char *src, const char *dst, kbc_err *err) {
  int in = open(src, O_RDONLY);
  if (in < 0) {
    return kbc_err_set(err, KBC_ERR_IO, "open %s: %s", src, strerror(errno));
  }
  int out = open(dst, O_WRONLY | O_CREAT | O_TRUNC, 0600);
  if (out < 0) {
    int e = errno;
    (void)close(in);
    return kbc_err_set(err, KBC_ERR_IO, "create %s: %s", dst, strerror(e));
  }
  kbc_status st = KBC_OK;
  char *buf = malloc(COPY_BUF_BYTES);
  if (buf == NULL) {
    (void)close(out);
    (void)close(in);
    return kbc_err_set(err, KBC_ERR_NOMEM, "out of memory copying %s", src);
  }
  for (;;) {
    ssize_t n = read(in, buf, COPY_BUF_BYTES);
    if (n < 0) {
      if (errno == EINTR) {
        continue;
      }
      st = kbc_err_set(err, KBC_ERR_IO, "read %s: %s", src, strerror(errno));
      break;
    }
    if (n == 0) {
      break;
    }
    ssize_t at = 0;
    while (at < n) {
      ssize_t w = write(out, buf + at, (size_t)(n - at));
      if (w < 0) {
        if (errno == EINTR) {
          continue;
        }
        st = kbc_err_set(err, KBC_ERR_IO, "write %s: %s", dst,
                         strerror(errno));
        goto done;
      }
      at += w;
    }
  }
done:
  free(buf);
  if (st == KBC_OK && fsync(out) != 0) {
    st = kbc_err_set(err, KBC_ERR_IO, "fsync %s: %s", dst, strerror(errno));
  }
  if (close(out) != 0 && st == KBC_OK) {
    st = kbc_err_set(err, KBC_ERR_IO, "close %s: %s", dst, strerror(errno));
  }
  if (close(in) != 0 && st == KBC_OK) {
    st = kbc_err_set(err, KBC_ERR_IO, "close %s: %s", src, strerror(errno));
  }
  return st;
}

/* Removes a file or a whole tree. Used for the staging dir and for --force's
 * wipe, so it must not follow a symlink out of the tree it was pointed at:
 * lstat decides, and a symlink is unlinked rather than descended into. */
static kbc_status rm_rf(const char *path, kbc_err *err) {
  struct stat sb;
  if (lstat(path, &sb) != 0) {
    if (errno == ENOENT) {
      return KBC_OK;
    }
    return kbc_err_set(err, KBC_ERR_IO, "stat %s: %s", path, strerror(errno));
  }
  if (!S_ISDIR(sb.st_mode)) {
    if (unlink(path) != 0 && errno != ENOENT) {
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
  kbc_status st = KBC_OK;
  struct dirent *de;
  errno = 0;
  while (st == KBC_OK && (de = readdir(d)) != NULL) {
    if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0) {
      continue;
    }
    char *child = join2(path, de->d_name);
    st = rm_rf(child, err);
    free(child);
    errno = 0;
  }
  if (st == KBC_OK && closedir(d) != 0) {
    st = kbc_err_set(err, KBC_ERR_IO, "closedir %s: %s", path,
                     strerror(errno));
  } else if (st != KBC_OK) {
    (void)closedir(d);
  }
  if (st == KBC_OK && rmdir(path) != 0 && errno != ENOENT) {
    st = kbc_err_set(err, KBC_ERR_IO, "rmdir %s: %s", path, strerror(errno));
  }
  return st;
}

/* Recursive copy, the Rust's copy_dir. Depth-bounded because a state tree can
 * be deep and the process stack is not: a corpus that somehow produced a
 * thousand nested directories must fail loudly, not smash the stack. */
#define COPY_MAX_DEPTH 64u

static kbc_status copy_dir(const char *src, const char *dst, unsigned depth,
                           kbc_err *err) {
  if (depth > COPY_MAX_DEPTH) {
    return kbc_err_set(err, KBC_ERR_IO, "%s nests deeper than %u levels", src,
                       COPY_MAX_DEPTH);
  }
  if (kbc_failed(kbc_mkdir_p(dst, err))) {
    return err->status;
  }
  DIR *d = opendir(src);
  if (d == NULL) {
    return kbc_err_set(err, KBC_ERR_IO, "opendir %s: %s", src, strerror(errno));
  }
  kbc_status st = KBC_OK;
  struct dirent *de;
  errno = 0;
  while (st == KBC_OK && (de = readdir(d)) != NULL) {
    if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0) {
      continue;
    }
    char *s = join2(src, de->d_name);
    char *t = join2(dst, de->d_name);
    struct stat sb;
    if (lstat(s, &sb) != 0) {
      st = kbc_err_set(err, KBC_ERR_IO, "stat %s: %s", s, strerror(errno));
    } else if (S_ISDIR(sb.st_mode)) {
      st = copy_dir(s, t, depth + 1u, err);
    } else if (S_ISREG(sb.st_mode)) {
      st = copy_file(s, t, err);
    }
    /* Anything else — a socket, a fifo, a symlink — is SKIPPED, matching the
     * Rust's WalkDir, which yields symlinks as entries and copies neither. */
    free(s);
    free(t);
    errno = 0;
  }
  if (st == KBC_OK && closedir(d) != 0) {
    st = kbc_err_set(err, KBC_ERR_IO, "closedir %s: %s", src, strerror(errno));
  } else if (st != KBC_OK) {
    (void)closedir(d);
  }
  return st;
}

/* `true` when `path` is a directory with no entries. The Rust's
 * `read_dir(&kb_state)?.next().is_some()`, inverted: an EXISTING BUT EMPTY
 * state dir is a legitimate restore target and must not need --force. */
static bool dir_is_empty(const char *path) {
  DIR *d = opendir(path);
  if (d == NULL) {
    return true;
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

/* An execvp argv, built without ever casting away const.
 *
 * execvp takes `char *const argv[]`, and -Wcast-qual rightly refuses to make
 * one out of a `const char *`. A cast would also misdescribe the contract:
 * these slots are read by the child, never written. So the builder strdups
 * each argument, owns it, and the whole vector is freed in one call. */
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

/* Always NULL-terminates, so `a.v` is directly usable as the execvp argv. An
 * allocation failure here is fatal by design: there is no argv to run without
 * it, and a half-built one would be worse than none. The callers push a
 * compile-time-fixed number of arguments (7 or 8), so this vector never grows
 * without bound and the doubling cannot overflow. */
static void argv_push(argv_vec *a, const char *arg) {
  if (a->n + 2u > a->cap) {
    size_t cap = a->cap == 0 ? 8u : a->cap * 2u;
    char **grown = realloc(a->v, cap * sizeof *grown);
    if (grown == NULL) {
      argv_free(a);
      die_io("out of memory building a `tar` command line");
    }
    a->v = grown;
    a->cap = cap;
  }
  char *copy = strdup(arg);
  if (copy == NULL) {
    argv_free(a);
    die_io("out of memory building a `tar` command line");
  }
  a->v[a->n++] = copy;
  a->v[a->n] = NULL;
}

/* Runs `argv[0]` with `argv`, no shell, and returns its exit status; -1 when
 * it could not be spawned at all. Nothing here is ever formatted into a
 * command string: a corpus name is operator input and a path is too, and a
 * shell would make both an injection surface (rule 9). tar is an EXTERNAL
 * process in the original too, and it must stay one — this verb never shells
 * out to `kbc` itself. */
static int run_argv(char *const argv[], kbc_str *out) {
  int fds[2] = {-1, -1};
  if (out != NULL && pipe(fds) != 0) {
    say_err("pipe: %s", strerror(errno));
    return -1;
  }
  pid_t pid = fork();
  if (pid < 0) {
    if (out != NULL) {
      (void)close(fds[0]);
      (void)close(fds[1]);
    }
    say_err("fork: %s", strerror(errno));
    return -1;
  }
  if (pid == 0) {
    if (out != NULL) {
      (void)close(fds[0]);
      if (dup2(fds[1], STDOUT_FILENO) < 0) {
        _exit(127);
      }
      (void)close(fds[1]);
    }
    execvp(argv[0], argv);
    _exit(127);
  }
  if (out != NULL) {
    (void)close(fds[1]);
    char buf[COPY_BUF_BYTES];
    for (;;) {
      ssize_t n = read(fds[0], buf, sizeof buf);
      if (n < 0) {
        if (errno == EINTR) {
          continue;
        }
        break;
      }
      if (n == 0) {
        break;
      }
      if (out->len + (size_t)n > TAR_LIST_MAX) {
        say_err("`%s` wrote more than %u bytes of listing", argv[0],
                TAR_LIST_MAX);
        (void)close(fds[0]);
        (void)waitpid(pid, NULL, 0);
        return -1;
      }
      if (kbc_failed(kbc_str_append(out, buf, (size_t)n))) {
        say_err("out of memory reading `%s` output", argv[0]);
        (void)close(fds[0]);
        (void)waitpid(pid, NULL, 0);
        return -1;
      }
    }
    (void)close(fds[0]);
  }
  int status = 0;
  while (waitpid(pid, &status, 0) < 0) {
    if (errno != EINTR) {
      say_err("waitpid: %s", strerror(errno));
      return -1;
    }
  }
  if (WIFEXITED(status)) {
    return WEXITSTATUS(status);
  }
  if (WIFSIGNALED(status)) {
    say_err("`%s` was killed by signal %d", argv[0], WTERMSIG(status));
  }
  return -1;
}

/* tar's exit code, or a message naming the spawn failure. The Rust reports
 * "tar invocation failed (is `tar` installed?)" for a spawn error, which is the
 * common case and the one the hint is for. */
static int run_tar(char *const argv[], kbc_str *out) {
  int rc = run_argv(argv, out);
  if (rc == 127) {
    say_err("tar invocation failed (is `tar` installed?)");
  }
  return rc;
}

/* A transactionally-consistent copy of the sqlite database at `src` to `dest`
 * via VACUUM INTO.
 *
 * Opened READ-ONLY, unlike the Rust's Connection::open: that one carries
 * SQLITE_OPEN_CREATE, so a mistyped source path silently CREATES an empty
 * database and the backup "succeeds" with nothing in it. Read-only fails
 * loudly instead. VACUUM INTO works on a read-only connection — it only
 * writes the destination.
 *
 * `dest` must not already exist: sqlite refuses to overwrite, and refusing is
 * what keeps a re-run from quietly swapping one snapshot for another. */
static kbc_status vacuum_into(const char *src, const char *dest, kbc_err *err) {
  sqlite3 *db = NULL;
  int rc = sqlite3_open_v2(src, &db, SQLITE_OPEN_READONLY, NULL);
  if (rc != SQLITE_OK) {
    kbc_status st = kbc_err_set(err, KBC_ERR_IO, "open %s: %s", src,
                                db != NULL ? sqlite3_errmsg(db) : "no sqlite");
    if (db != NULL) {
      (void)sqlite3_close(db);
    }
    return st;
  }
  sqlite3_stmt *q = NULL;
  rc = sqlite3_prepare_v2(db, "VACUUM INTO ?1", -1, &q, NULL);
  if (rc != SQLITE_OK) {
    kbc_status st = kbc_err_set(err, KBC_ERR_SQL, "prepare VACUUM INTO: %s",
                                sqlite3_errmsg(db));
    (void)sqlite3_close(db);
    return st;
  }
  rc = sqlite3_bind_text(q, 1, dest, -1, SQLITE_TRANSIENT);
  if (rc != SQLITE_OK) {
    kbc_status st = kbc_err_set(err, KBC_ERR_SQL, "bind VACUUM INTO dest: %s",
                                sqlite3_errmsg(db));
    (void)sqlite3_finalize(q);
    (void)sqlite3_close(db);
    return st;
  }
  rc = sqlite3_step(q);
  kbc_status st = KBC_OK;
  if (rc != SQLITE_DONE) {
    st = kbc_err_set(err, KBC_ERR_SQL, "VACUUM INTO %s: %s", dest,
                     sqlite3_errmsg(db));
  }
  (void)sqlite3_finalize(q);
  (void)sqlite3_close(db);
  return st;
}

/* `true` when the archive carries the daemon-wide `slates/` member. Pre-SL2
 * tarballs do not, and asking tar to extract a member that is not there is an
 * error — so the LISTING is the gate, not a swallowed extraction failure. */
static bool archive_has_slates(const char *tarball) {
  argv_vec a = {NULL, 0, 0};
  argv_push(&a, "tar");
  argv_push(&a, "-tzf");
  argv_push(&a, tarball);
  kbc_str listing;
  kbc_str_init(&listing);
  int rc = run_tar(a.v, &listing);
  argv_free(&a);
  if (rc != 0) {
    kbc_str_free(&listing);
    die_io("tar -tzf %s failed with exit code %d", tarball, rc);
  }
  bool found = false;
  for (size_t i = 0; i < listing.len && !found; i++) {
    size_t start = i;
    while (i < listing.len && listing.ptr[i] != '\n') {
      i++;
    }
    size_t n = i - start;
    if (n > 0 && listing.ptr[i - 1] == '/') {
      n--;
    }
    if (n >= 6u && memcmp(listing.ptr + start, "slates", 6u) == 0 &&
        (n == 6u || listing.ptr[start + 6u] == '/')) {
      found = true;
    }
  }
  kbc_str_free(&listing);
  return found;
}

/* Packs `staging` with tar, by member name.
 *
 * The `--` BEFORE THE MEMBER LIST is not decoration. tar reads a leading `-`
 * as an option, and a corpus name may legitimately start with one (`-notes`
 * is a legal KbName). The Rust CLI omits it (commands/backup.rs:408) while
 * its own daemon writer emits it (backup.rs:318); the CLI is the bug, and an
 * operator-supplied corpus name must never be read as a tar option. */
static int tar_tree(const char *staging, const char *out, const char *kb,
                    bool include_slates) {
  argv_vec a = {NULL, 0, 0};
  argv_push(&a, "tar");
  argv_push(&a, "-czf");
  argv_push(&a, out);
  argv_push(&a, "-C");
  argv_push(&a, staging);
  argv_push(&a, "--");
  argv_push(&a, kb);
  if (include_slates) {
    argv_push(&a, "slates");
  }
  int rc = run_tar(a.v, NULL);
  argv_free(&a);
  return rc;
}

/* Extracts ONE member (`kb` or `slates`) of `tarball` into `dest`. The `--`
 * applies here for the same reason it does in tar_tree. */
static int tar_extract(const char *tarball, const char *dest,
                       const char *member) {
  argv_vec a = {NULL, 0, 0};
  argv_push(&a, "tar");
  argv_push(&a, "-xzf");
  argv_push(&a, tarball);
  argv_push(&a, "-C");
  argv_push(&a, dest);
  argv_push(&a, "--");
  argv_push(&a, member);
  int rc = run_tar(a.v, NULL);
  argv_free(&a);
  return rc;
}

/* `<data_dir>/exports/` — where the default tarball name and the staging dir
 * live. The Rust's `<state>/exports/`; kb-c's per-daemon state directory IS
 * `data_dir`, so this is the same level. */
static char *exports_dir(const kbc_config *cfg) {
  return join2(cfg->data_dir, "exports");
}

/* Builds the consistent snapshot tree at `staging/<kb>/` and tars it.
 *
 * A FAILED BACKUP LEAVES NOTHING BEHIND: the caller removes the staging tree on
 * both the success and the error path, and a tar that exits non-zero has its
 * partial output file removed here. A half-written tarball that looks like a
 * backup is the one artefact worse than no backup at all. */
static kbc_status stage_and_tar(const kbc_config *cfg, const char *kb,
                                const char *staging, const char *out,
                                kbc_err *err) {
  char *staged_kb = join2(staging, kb);
  char *staged_db = join2(staged_kb, "index.db");
  char *staged_lance = join2(staged_kb, "lance");
  char *staged_review = join2(staged_kb, ".review");
  char *staged_slates = join2(staging, "slates");
  char *src_lance = join2(cfg->data_dir, "lance");
  char *src_review = join2(cfg->data_dir, ".review");
  char *src_slates = join2(cfg->data_dir, "slates");

  kbc_status st = kbc_mkdir_p(staged_kb, err);
  /* 1. sqlite — transactionally consistent even while the daemon writes. */
  if (st == KBC_OK) {
    st = vacuum_into(cfg->db_path, staged_db, err);
  }
  /* 2. lance — the vector store, copied verbatim when the daemon has one. */
  if (st == KBC_OK && kbc_path_exists(src_lance)) {
    st = copy_dir(src_lance, staged_lance, 0u, err);
  }
  /* 3. review — each review JSON file lands via atomic rename, so every file
   *    is already internally consistent and a plain copy is enough. */
  if (st == KBC_OK && kbc_path_exists(src_review)) {
    st = copy_dir(src_review, staged_review, 0u, err);
  }
  /* 3b. slates — the daemon-wide clause. See the layout comment. */
  bool include_slates = st == KBC_OK && kbc_path_exists(src_slates);
  if (include_slates) {
    st = copy_dir(src_slates, staged_slates, 0u, err);
  }
  if (st == KBC_OK) {
    int rc = tar_tree(staging, out, kb, include_slates);
    if (rc != 0) {
      (void)unlink(out); /* the partial tarball, gone before anyone sees it */
      st = kbc_err_set(err, KBC_ERR_IO, "tar exited %d", rc);
    }
  }
  free(staged_kb);
  free(staged_db);
  free(staged_lance);
  free(staged_review);
  free(staged_slates);
  free(src_lance);
  free(src_review);
  free(src_slates);
  return st;
}

/* `true` when `kb` is a corpus this daemon is configured to index.
 *
 * kb-c's corpus REGISTRY is the config's [[corpus]] list, not a row in the
 * store: `kbc add` writes kb.toml, and the store's `sources` table stays empty
 * on a real deployment. So this asks the config, and the store-existence check
 * above is what covers "configured but never indexed".
 *
 * Checking the config rather than the store matters for the SNAPSHOT too: the
 * tarball is the whole volume, so backing up a corpus that does not exist
 * would produce a perfectly valid tarball of somebody else's data under a name
 * that was never indexed — a mistake the operator would only discover when the
 * restore did not contain what they expected. */
static bool corpus_is_configured(const kbc_config *cfg, const char *kb) {
  return kbc_config_corpus(cfg, kb) != NULL;
}

/* One corpus's snapshot. `out_arg` NULL means the default export name. */

static kbc_status snapshot_one(const kbc_config *cfg, const char *kb,
                               const char *out_arg, kbc_err *err) {
  /* Two questions, in this order, because they have different remedies.
   *
   * Is the corpus CONFIGURED? kb-c keeps one sqlite volume per daemon, not
   * one directory per corpus the way the Rust does, so "the state dir exists"
   * is the wrong question: the volume exists as soon as ANY corpus has been
   * indexed. The config's [[corpus]] list is the registry, and it is what the
   * snapshot is NAMED after.
   *
   * Does the volume EXIST? sqlite's Connection::open CREATES a missing file,
   * so a state dir that was never indexed would otherwise be snapshotted as a
   * brand-new empty database. Refusing first is what stops a backup from
   * inventing the thing it backs up. */
  if (!corpus_is_configured(cfg, kb)) {
    return kbc_err_set(err, KBC_ERR_NOTFOUND,
                       "kb %s is not a configured corpus; run "
                       "`kbc add <dir> --kb %s` first",
                       kb, kb);
  }
  if (!kbc_path_exists(cfg->db_path)) {
    return kbc_err_set(err, KBC_ERR_NOTFOUND,
                       "kb %s has no state at %s; index something first", kb,
                       cfg->db_path);
  }
  char *exports = exports_dir(cfg);
  kbc_status st = kbc_mkdir_p(exports, err);
  char *out = NULL;
  char *staging = NULL;
  if (st == KBC_OK) {
    char *stamp = utc_stamp();
    if (out_arg != NULL) {
      char *expanded = expand_tilde(out_arg);
      out = bounded_path("--out", expanded);
      free(expanded);
    } else {
      char name[KBC_MAX_KB_NAME + 32u];
      (void)snprintf(name, sizeof name, "%s-%s.tar.gz", kb, stamp);
      out = join2(exports, name);
    }
    char *dir = dir_of(out);
    st = kbc_mkdir_p(dir, err);
    free(dir);
    /* `.staging-<kb>-<pid>-<stamp>`: the pid keeps two concurrent backups of
     * the same corpus in the same second off each other's staging tree. */
    char sname[KBC_MAX_KB_NAME + 64u];
    (void)snprintf(sname, sizeof sname, ".staging-%s-%ld-%s", kb,
                   (long)getpid(), stamp);
    staging = join2(exports, sname);
    free(stamp);
  }
  /* Stage a consistent snapshot under exports/, tar it, then clean up the
   * staging dir unconditionally — success or error. */
  if (st == KBC_OK) {
    st = stage_and_tar(cfg, kb, staging, out, err);
  }
  if (staging != NULL) {
    /* The cleanup runs whether the snapshot succeeded or not, and a cleanup
     * that itself fails is reported ONLY when there is no earlier, more
     * useful failure to report: the operator needs the snapshot's reason. */
    kbc_err local;
    kbc_err_reset(&local);
    kbc_status cs = rm_rf(staging, &local);
    if (st == KBC_OK && kbc_failed(cs)) {
      st = local.status;
      (void)kbc_err_set(err, st, "removing the staging dir %s: %s", staging,
                        local.msg);
    }
  }
  if (st != KBC_OK) {
    free(exports);
    free(out);
    free(staging);
    return st;
  }
  free(exports);
  free(staging);
  if (g_json) {
    kbc_str line;
    kbc_str_init(&line);
    line_puts(&line, "{\"ok\":true,\"kb\":");
    put_shown(&line, kb);
    line_puts(&line, ",\"tarball\":");
    put_shown(&line, out);
    line_puts(&line, "}");
    emit_line(&line);
  } else {
    kbc_str line;
    kbc_str_init(&line);
    line_puts(&line, "backup: ");
    put_shown(&line, cfg->data_dir);
    line_puts(&line, " -> ");
    put_shown(&line, out);
    emit_line(&line);
  }
  free(out);
  return KBC_OK;
}


/* `--all` — one tarball per corpus this daemon knows, each at the default
 * export path. The corpus LIST comes from `GET /api/kbs`, but every snapshot
 * is this machine's own state dir, so a remote daemon is refused before the
 * list: listing one host's corpora and tarring another's state would file this
 * machine's data under the remote's names. Loopback only, which is the same
 * bound the config's own bind guard uses.
 *
 * EVERY listed corpus is attempted, including after a failure: a partial sweep
 * that stopped at the first error would leave the operator guessing which
 * corpora are covered. Failures are collected and named together at the end —
 * a later success does not cancel an earlier failure, and reporting success
 * when any corpus failed is the one outcome the command must never produce. */
static int backup_all(const kbc_config *cfg) {
  const char *base = resolve_base_url();
  kbc_url u;
  kbc_err e;
  kbc_err_reset(&e);
  if (!url_parse(base, &u, &e)) {
    die_user("backup --all: %s", e.msg);
  }
  bool loopback = strcmp(u.host, "127.0.0.1") == 0 ||
                  strcmp(u.host, "localhost") == 0 || strcmp(u.host, "::1") == 0;
  if (!loopback) {
    char *host = u.host;
    die_user("backup --all: refusing daemon %s — the corpus list would come "
             "from another host while the snapshot is this machine's state",
             host);
  }
  url_free(&u);

  int status = 0;
  kbc_str resp;
  kbc_err_reset(&e);
  if (kbc_failed(http_call("GET", "/api/kbs", NULL, 0, &status, &resp, &e))) {
    /* call_failed, like every other verb: a transport failure is the daemon
     * class (exit 2), a 401 is the user class — the same split `kbc status`
     * makes, which is the whole point of using the shared helper. */
    call_failed(e.status, &e, "backup --all: listing corpora");
  }
  if (status < 200 || status >= 300) {
    kbc_str_free(&resp);
    die_io("backup --all: HTTP %d listing corpora", status);
  }
  kbc_json *root = NULL;
  kbc_arena *a = resp_arena(&resp, &root, "backup --all");
  const kbc_json *kbs = kbc_json_get(root, "kbs");
  if (kbs == NULL || !kbc_json_is(kbs, KBC_JSON_ARR)) {
    kbc_arena_free(a);
    kbc_str_free(&resp);
    die_io("backup --all: the daemon's answer carried no corpus list");
  }
  size_t n = kbc_json_len(kbs);
  if (n == 0) {
    kbc_arena_free(a);
    kbc_str_free(&resp);
    die_user("backup --all: the daemon lists no corpora; nothing to back up");
  }
  kbc_str failed_names;
  kbc_str_init(&failed_names);
  size_t nfailed = 0;
  int worst = EXIT_USER;
  for (size_t i = 0; i < n; i++) {
    const kbc_json *row = kbc_json_at(kbs, i);
    const char *name =
        kbc_json_is(row, KBC_JSON_OBJ) ? kbc_json_str(row, "name", NULL) : NULL;
    /* A row with no usable name is a FAILURE, not a skip: dropping it would
     * silently leave that corpus out of the backup set and still report
     * success. */
    if (name == NULL || !kb_name_ok(name)) {
      say_err("backup: kb FAILED: the daemon listed a row with no usable name");
      if (nfailed++ > 0) {
        (void)kbc_str_putc(&failed_names, ',');
      }
      (void)kbc_str_puts(&failed_names, "(unnamed)");
      worst = EXIT_DAEMON; /* the daemon's own answer is what is wrong */
      continue;
    }
    kbc_err local;
    kbc_err_reset(&local);
    if (kbc_failed(snapshot_one(cfg, name, NULL, &local))) {
      /* Built and escaped rather than passed to say_err as a format: `name`
       * came off the wire, and a corpus name is operator input even when it
       * arrived by HTTP. */
      kbc_str why;
      kbc_str_init(&why);
      line_puts(&why, "backup: kb ");
      put_shown(&why, name);
      line_puts(&why, " FAILED: ");
      put_shown(&why, local.msg[0] != '\0' ? local.msg
                                          : kbc_status_str(local.status));
      emit_line(&why);
      if (nfailed++ > 0) {
        (void)kbc_str_putc(&failed_names, ',');
      }
      (void)kbc_str_puts(&failed_names, name);
      /* The aggregate keeps the WORST class seen: a sweep that hit a daemon
       * or filesystem failure must not report the same code as one that only
       * found corpora the user has not set up — the same rule fail_status
       * applies to a single failure, applied to the sweep. */
      if (local.status != KBC_ERR_INVALID && local.status != KBC_ERR_NOTFOUND &&
          local.status != KBC_ERR_PARSE) {
        worst = EXIT_DAEMON;
      }
    }
  }
  kbc_arena_free(a);
  kbc_str_free(&resp);
  if (nfailed > 0) {
    /* Sized for the worst case of two 64-bit counts, so -Wformat-truncation
     * can see the write fits: this line is never truncated, and a truncated
     * one would name the wrong number of corpora. */
    char counts[96];
    (void)snprintf(counts, sizeof counts, "backup: %zu of %zu corpora failed: ",
                   nfailed, n);
    kbc_str line;
    kbc_str_init(&line);
    line_puts(&line, counts);
    put_shown(&line, failed_names.ptr);
    emit_line(&line);
    kbc_str_free(&failed_names);
    return worst;
  }
  kbc_str_free(&failed_names);
  return EXIT_OK;
}

static int cmd_backup(int argc, char **argv, int start) {
  /* No exit code is set up here: every failure below reaches exit() through
   * die_user, die_io or fail_status, which is how every other verb does it. */
  opts o;
  parse_verb(FLAGS_BACKUP, argc, argv, start, &o);
  if (o.all && o.out != NULL) {
    die_user("--out is not valid with --all (one path cannot hold every "
             "tarball)");
  }
  if (o.all && o.npos > 0) {
    die_user("--all takes no corpus name");
  }
  kbc_config *cfg = load_config();
  if (o.all) {
    return backup_all(cfg);
  }
  const char *kb = positional(&o, 0, "backup <kb>");
  if (!kb_name_ok(kb)) {
    die_user("invalid kb %s: a corpus name must be 1-%u bytes of [a-z0-9_-]",
             kb, KBC_MAX_KB_NAME);
  }
  /* The --out bound is enforced where the path is produced, in snapshot_one;
   * checking it here too would be a second rule to keep in step. */
  kbc_err e;
  kbc_err_reset(&e);
  kbc_status s = snapshot_one(cfg, kb, o.out, &e);
  if (kbc_failed(s)) {
    /* fail_status, not a verb-specific reporter: a corpus the user has not
     * configured is a user error (1); a filesystem or daemon failure is 2. */
    fail_status(s, &e, kb, NULL);
  }
  return EXIT_OK;
}

/* The Rust's `kb restore <tarball> --kb <name> [--force]`. The daemon for this
 * kb must be stopped first: it holds index.db open.
 *
 * NO VERSION CHECK HERE, and that is deliberate. The Rust does not check in
 * restore either — a too-new tarball restores "successfully" here and is
 * REFUSED at the next kbc_store_open, by refuse_if_volume_ahead, which runs
 * BEFORE migrations and would otherwise have to undo them. Adding a check in
 * restore would silently move the failure to a different command, and would
 * reject a tarball this binary might still be able to read after a downgrade. */
static int cmd_restore(int argc, char **argv, int start) {
  opts o;
  parse_verb(FLAGS_RESTORE, argc, argv, start, &o);
  const char *tarball_arg = positional(&o, 0, "restore <tarball>");
  if (!o.has_kb) {
    die_user("restore needs --kb <name>");
  }
  if (!kb_name_ok(o.kb)) {
    die_user("invalid kb %s: a corpus name must be 1-%u bytes of [a-z0-9_-]",
             o.kb, KBC_MAX_KB_NAME);
  }
  char *expanded = expand_tilde(tarball_arg);
  char *tarball = bounded_path("tarball", expanded);
  free(expanded);
  if (!kbc_path_exists(tarball)) {
    /* A tarball the user named and that is not there is a user error, the
     * same class `kbc get`'s missing artifact is. */
    die_user("restore: tarball %s not found", tarball);
  }

  kbc_config *cfg = load_config();
  /* The tarball root is `<kb>/…`, so extracting into the state dir recreates
   * `<state>/<kb>/` — the parent IS the state dir. */
  char *kb_state = join2(cfg->data_dir, o.kb);
  if (kbc_path_exists(kb_state) && !dir_is_empty(kb_state) && !o.force) {
    kbc_str line;
    kbc_str_init(&line);
    line_puts(&line, "restore: kb ");
    put_shown(&line, o.kb);
    line_puts(&line, " already has state at ");
    put_shown(&line, kb_state);
    line_puts(&line,
              " - refusing to overwrite. Stop the daemon for this kb, then "
              "pass --force to replace it.");
    emit_line(&line);
    free(kb_state);
    free(tarball);
    return EXIT_USER;
  }
  if (o.force && kbc_path_exists(kb_state)) {
    kbc_err e;
    kbc_err_reset(&e);
    if (kbc_failed(rm_rf(kb_state, &e))) {
      fail_status(e.status, &e, "restore: clearing existing state", NULL);
    }
  }
  if (kbc_failed(kbc_mkdir_p(cfg->data_dir, NULL))) {
    die_io("restore: %s is not a usable state directory", cfg->data_dir);
  }

  /* The kb's own tree, always. Named explicitly rather than extracting the
   * whole archive, so the slates member below is a SEPARATE, guarded
   * decision. */
  int rc = tar_extract(tarball, cfg->data_dir, o.kb);
  if (rc != 0) {
    free(kb_state);
    free(tarball);
    die_io("restore: tar exited %d extracting %s/", rc, o.kb);
  }

  /* SLATES — never clobbers. --force is scoped to "replace THIS kb's state",
   * and slates are daemon-wide: honouring --force here would destroy every
   * other project's live coordination board on a one-kb restore. An existing
   * slates/ is left alone and the skip is printed with its remedy. */
  char *slates_dir = join2(cfg->data_dir, "slates");
  if (archive_has_slates(tarball)) {
    if (kbc_path_exists(slates_dir)) {
      kbc_str line;
      kbc_str_init(&line);
      line_puts(&line, "restore: SKIPPING the tarball's daemon-wide slates/ - ");
      put_shown(&line, slates_dir);
      line_puts(&line, " already exists. Slates are not per-kb, so --force "
                       "(which replaces only kb ");
      put_shown(&line, o.kb);
      line_puts(&line, ") does not cover them; move that directory aside and "
                       "re-run to restore them.");
      emit_line(&line);
    } else {
      int src = tar_extract(tarball, cfg->data_dir, "slates");
      if (src != 0) {
        free(slates_dir);
        free(kb_state);
        free(tarball);
        die_io("restore: tar exited %d extracting slates/", src);
      }
      kbc_str line;
      kbc_str_init(&line);
      line_puts(&line, "restore: slates/ -> ");
      put_shown(&line, slates_dir);
      emit_line(&line);
    }
  }

  /* Validate: the archive must have produced this kb's index.db. Catches a
 * wrong tarball, or a --kb that does not match the archive's root dir. */
  char *index_db = join2(kb_state, "index.db");
  if (!kbc_path_exists(index_db)) {
    kbc_str line;
    kbc_str_init(&line);
    line_puts(&line, "restore produced no index.db at ");
    put_shown(&line, kb_state);
    line_puts(&line, " - wrong tarball, or its root dir doesn't match --kb ");
    put_shown(&line, o.kb);
    line_puts(&line, "?");
    emit_line(&line);
    free(index_db);
    free(slates_dir);
    free(kb_state);
    free(tarball);
    return EXIT_USER;
  }

  if (g_json) {
    kbc_str line;
    kbc_str_init(&line);
    line_puts(&line, "{\"ok\":true,\"kb\":");
    put_shown(&line, o.kb);
    line_puts(&line, ",\"tarball\":");
    put_shown(&line, tarball);
    line_puts(&line, ",\"state\":");
    put_shown(&line, kb_state);
    line_puts(&line, "}");
    emit_line(&line);
  } else {
    kbc_str line;
    kbc_str_init(&line);
    line_puts(&line, "restore: ");
    put_shown(&line, tarball);
    line_puts(&line, " -> ");
    put_shown(&line, kb_state);
    emit_line(&line);
  }
  free(index_db);
  free(slates_dir);
  free(kb_state);
  free(tarball);
  return EXIT_OK;
}

/* kb-c's indexable set, matching is_indexable() in src/app.c. Duplicated
 * rather than shared because that helper is file-local to app.c and this
 * file may not reach into app internals; the two lists must stay in step,
 * and this comment is where a reader looks when they stop matching. */
static bool is_bench_indexable(const char *rel) {
  size_t n = strlen(rel);
  static const char *const exts[] = { ".html", ".htm", ".md", ".markdown",
                                      NULL };
  for (size_t i = 0; exts[i] != NULL; i++) {
    size_t el = strlen(exts[i]);
    if (n <= el) {
      continue;
    }
    size_t k = 0;
    bool same = true;
    for (size_t j = n - el; j < n; j++, k++) {
      char c = rel[j];
      if (c >= 'A' && c <= 'Z') {
        c = (char)(c - 'A' + 'a');
      }
      if (c != exts[i][k]) {
        same = false;
        break;
      }
    }
    if (same) {
      return true;
    }
  }
  return false;
}


/* -------------------------------------------------------------- comments ---
 *
 * `kbc comments list|add|resolve|unresolve` — the four subcommands the Rust's
 * 1,683-line `comments.rs` reduces to, once the store's actual surface is
 * what has to be served.
 *
 * WHY THIS IS NOT AN HTTP CLIENT. Every verb in the Rust talks to the daemon
 * (`GET /api/kb/{kb}/reviews`, `POST .../resolve`, and a dozen more), because
 * in the Rust a comment is a field inside a per-artifact `.review` JSON
 * file that only the daemon reads and writes. kb-c has no review file and no
 * review routes — `KBC_ROUTES` in src/httpd.c lists thirteen endpoints, and
 * not one of them is a review endpoint — and `src/httpd.c` is not this file.
 * What kb-c does have is `comments` as a TABLE (src/store.c: the Rust's own
 * DDL, unchanged) behind three frozen functions: add, list-by-doc,
 * set-resolved.
 * So this verb opens the store and calls those three. A CLI that dialled a
 * route the daemon does not serve would return 404 on every invocation,
 * which is not a port, it is a broken one.
 *
 * The consequence, stated rather than hidden: the Rust's review-file verbs —
 * `export`, `apply`, `import`, `verdict`, `keep`, `reanchor`, `edit`,
 * `delete`, `inbox`, the attachment pair, and `watch` — have no counterpart
 * here and are not built. Each needs state kb-c does not store (a review
 * file, a proposal queue, a reply tree, an attachment blob, or an SSE
 * comments.updated event the daemon never publishes). They are missing
 * because the surface is missing, not because the parsing was hard.
 *
 * A live daemon holding the same db is not a conflict: the store runs WAL
 * with a 5s busy_timeout (src/store.c:552), so a reader and this writer
 * serialise rather than deadlock. This is the same posture `backup` takes
 * when it VACUUMs INTO a db the daemon has open.
 */

/* The store, opened the way `backup` opens one: from the resolved config, so
 * `--config` and $HOME both reach it. */
static kbc_store *open_comment_store(void) {
  kbc_err e;
  kbc_err_reset(&e);
  kbc_store *s = kbc_store_open(load_config(), &e);
  if (s == NULL) {
    fail_status(e.status != KBC_OK ? e.status : KBC_ERR_IO, &e, "comments",
                NULL);
  }
  return s;
}

/* The document a comment hangs off, from `--artifact-id` or `--path`.
 *
 * `--path` is resolved through `kbc_store_get_artifact_by_path`, the frozen
 * (corpus, relative path) -> row lookup the watcher itself uses. The Rust
 * resolves `--path` over HTTP through `/lookup`, which can answer
 * `ambiguous` with a candidate list; there is no such ambiguity here,
 * because (corpus, path) is UNIQUE in the store — one path is one row. A
 * miss is therefore always NOTFOUND, never a choice, and the Rust's
 * "matched N artifacts, pick one" branch has nothing to say here.
 */
static char *comment_doc_id(kbc_store *s, const opts *o) {
  if (o->id != NULL && o->path != NULL) {
    die_user("use either --artifact-id or --path, not both");
  }
  if (o->id == NULL && o->path == NULL) {
    die_user("comments needs a document: pass --artifact-id ID or "
             "--path FILE");
  }
  if (o->id != NULL) {
    if (!kbc_id_is_valid(o->id)) {
      die_user("--artifact-id %s: an id is %u lowercase hex chars", o->id,
               KBC_MAX_ID_LEN);
    }
    return xstrdup(o->id);
  }
  kbc_arena *a = kbc_arena_new(4096);
  if (a == NULL) {
    die_io("out of memory");
  }
  const kbc_config *cfg = load_config();
  const char *kb = o->has_kb ? o->kb : NULL;
  if (kb == NULL) {
    if (cfg->ncorpora != 1) {
      kbc_arena_free(a);
      die_user("--path needs --kb: %zu corpora are configured",
               cfg->ncorpora);
    }
    kb = cfg->corpora[0].name;
  }
  if (!kb_name_ok(kb)) {
    kbc_arena_free(a);
    die_user("invalid kb %s: a corpus name must be 1-%u bytes of [a-z0-9_-]",
             kb, KBC_MAX_KB_NAME);
  }
  kbc_artifact art;
  memset(&art, 0, sizeof art);
  kbc_err e;
  kbc_err_reset(&e);
  kbc_status st = kbc_store_get_artifact_by_path(s, a, kb, o->path, &art, &e);
  if (kbc_failed(st)) {
    kbc_arena_free(a);
    fail_status(st, &e, "comments", NULL);
  }
  /* art.id is ARENA-owned and dies with `a`, so the id is copied out. */
  char *id = xstrdup(art.id);
  kbc_arena_free(a);
  return id;
}

/* One comment as the JSON the listing emits. Field names follow the Rust's
 * review row (`id`, `artifact_id`, `author`, `anchor`, `body`,
 * `created_at`, `status`) so a consumer written against the Rust's `--json`
 * keeps working; `status` is spelled from kb-c's `resolved` bit. */
static void comment_json(kbc_str *out, const kbc_comment *c,
                         const char *doc_id) {
  (void)kbc_str_puts(out, "{\"id\":");
  (void)kbc_str_append_json_string(out, c->id, strlen(c->id));
  (void)kbc_str_puts(out, ",\"artifact_id\":");
  (void)kbc_str_append_json_string(out, doc_id, strlen(doc_id));
  (void)kbc_str_puts(out, ",\"author\":");
  (void)kbc_str_append_json_string(out, c->author, strlen(c->author));
  (void)kbc_str_puts(out, ",\"anchor\":");
  (void)kbc_str_append_json_string(out, c->anchor, strlen(c->anchor));
  (void)kbc_str_puts(out, ",\"body\":");
  (void)kbc_str_append_json_string(out, c->body, strlen(c->body));
  (void)kbc_str_puts(out, ",\"created_at\":");
  (void)kbc_str_append_json_string(out, c->created_at, strlen(c->created_at));
  (void)kbc_str_printf(out, ",\"status\":\"%s\"}",
                       c->resolved ? "resolved" : "open");
}

/* One table cell: the value ESCAPED, truncated to `max` bytes (0 = no
 * limit), then left-padded to `width` columns.
 *
 * Escaping is not decoration. A comment body and an author are operator
 * input, and the listing prints them raw into a terminal: without
 * kbc_json_escape a body carrying ESC could repaint the operator's screen
 * and one carrying a newline would forge an extra table row — a comment
 * turning into a fake comment id in the operator's own listing. Rule 9.
 *
 * The truncation is on the ESCAPED text, not the raw, so a cut can never
 * land inside an escape sequence and leave half of it on the line. */
static void cell(kbc_str *out, const char *s, size_t max, size_t width) {
  kbc_str esc;
  kbc_str_init(&esc);
  if (kbc_failed(kbc_json_escape(&esc, s, strlen(s)))) {
    kbc_str_free(&esc);
    die_user("%s", "out of memory escaping a value for output");
  }
  size_t n = esc.len;
  bool cut = max > 0 && n > max;
  if (cut) {
    n = max;
  }
  (void)kbc_str_append(out, esc.ptr, n);
  kbc_str_free(&esc);
  if (cut) {
    (void)kbc_str_puts(out, "...");
    n += 3;
  }
  /* Pad to the column, but never TRUNCATE to it: a value wider than its
   * column shifts the rest of the row right, which is ugly and harmless,
   * whereas cutting would lose data the operator cannot otherwise see. */
  for (size_t p = n; p < width; p++) {
    (void)kbc_str_putc(out, ' ');
  }
  if (width > 0) {
    (void)kbc_str_putc(out, ' ');
  }
}

/* The Rust's `kb comments list` table: KB, ARTIFACT, STATUS, AUTHOR, ANCHOR,
 * BODY. kb-c has no KB column to print — a comment's row carries a doc id,
 * and the doc's corpus is one `get_artifact` away, not in the comment — so
 * the column is dropped rather than filled with a constant.
 *
 * The empty case prints `(no open comments)` / `(no comments)` exactly as
 * the Rust does, which is the one line a script can grep for. */
static int comments_list(int argc, char **argv, int start) {
  opts o;
  parse_verb(FLAGS_COMMENTS, argc, argv, start, &o);
  kbc_store *s = open_comment_store();
  char *doc = comment_doc_id(s, &o);
  size_t limit = o.has_limit ? o.limit : 50;

  kbc_arena *a = kbc_arena_new(8192);
  if (a == NULL) {
    free(doc);
    kbc_store_close(s);
    die_io("out of memory");
  }
  kbc_comment *rows = NULL;
  size_t n = 0;
  kbc_err e;
  kbc_err_reset(&e);
  kbc_status st = kbc_store_list_comments(s, a, doc, limit, &rows, &n, &e);
  if (kbc_failed(st)) {
    kbc_arena_free(a);
    free(doc);
    kbc_store_close(s);
    fail_status(st, &e, "comments list", NULL);
  }

  /* `--all` includes resolved comments. Without it an already-addressed
   * comment drops off the list, which is the Rust's `status` filter: the
 * default is the open ones, `--all` is every one. */
  size_t shown = 0;
  for (size_t i = 0; i < n; i++) {
    if (!o.all && rows[i].resolved) {
      continue;
    }
    shown++;
  }
  if (g_json) {
    kbc_str out;
    kbc_str_init(&out);
    (void)kbc_str_puts(&out, "{\"ok\":true,\"artifact_id\":");
    (void)kbc_str_append_json_string(&out, doc, strlen(doc));
    (void)kbc_str_puts(&out, ",\"comments\":[");
    size_t at = 0;
    for (size_t i = 0; i < n; i++) {
      if (!o.all && rows[i].resolved) {
        continue;
      }
      if (at++ > 0) {
        (void)kbc_str_putc(&out, ',');
      }
      comment_json(&out, &rows[i], doc);
    }
    (void)kbc_str_puts(&out, "]}");
    emit_line(&out);
  } else if (shown == 0) {
    printf("(no %scomments)\n", o.all ? "" : "open ");
  } else {
    printf("%-14s %-9s %-8s %-24s %s\n", "COMMENT", "STATUS", "AUTHOR",
           "ANCHOR", "BODY");
    for (size_t i = 0; i < n; i++) {
      if (!o.all && rows[i].resolved) {
        continue;
      }
      kbc_str line;
      kbc_str_init(&line);
      cell(&line, rows[i].id, 0, 14);
      cell(&line, rows[i].resolved ? "resolved" : "open", 0, 9);
      cell(&line, rows[i].author, 0, 8);
      cell(&line, rows[i].anchor, 24, 24);
      cell(&line, rows[i].body, 60, 0);
      emit_line(&line);
    }
  }
  kbc_arena_free(a);
  free(doc);
  kbc_store_close(s);
  return EXIT_OK;
}

/* `kbc comments add --artifact-id ID|--path FILE --body TEXT
 *   [--anchor A] [--author X]`
 *
 * Defaults are the Rust's: `--anchor file` and `--author claude`
 * (CommentsAction::Add). The Rust's other add-time flags — `--page`,
 * `--choice-json`, `--attach` — write columns and tables the store's
 * `comments` DDL does not have, so they are not accepted rather than
 * accepted and dropped.
 *
 * The store mints the comment id itself and does not hand it back, so the
 * confirmation reports the count, not an id the caller cannot verify. */
static int comments_add(int argc, char **argv, int start) {
  opts o;
  parse_verb(FLAGS_COMMENTS, argc, argv, start, &o);
  if (o.body == NULL) {
    die_user("comments add needs --body TEXT");
  }
  if (o.body[0] == '\0') {
    die_user("comments add: --body is empty");
  }
  if (o.npos > 0) {
    die_user("comments add takes no positional argument (the document is "
             "--artifact-id or --path)");
  }
  const char *anchor = o.anchor != NULL ? o.anchor : "file";
  const char *author = o.author != NULL ? o.author : "claude";
  if (anchor[0] == '\0') {
    die_user("comments add: --anchor is empty");
  }
  if (author[0] == '\0') {
    die_user("comments add: --author is empty");
  }
  kbc_store *s = open_comment_store();
  char *doc = comment_doc_id(s, &o);
  kbc_err e;
  kbc_err_reset(&e);
  kbc_status st = kbc_store_add_comment(s, doc, anchor, author, o.body, &e);
  if (kbc_failed(st)) {
    free(doc);
    kbc_store_close(s);
    fail_status(st, &e, "comments add", NULL);
  }
  kbc_str line;
  kbc_str_init(&line);
  if (g_json) {
    line_puts(&line, "{\"ok\":true,\"artifact_id\":");
    put_shown(&line, doc);
    line_puts(&line, ",\"added\":1}");
  } else {
    line_puts(&line, "added 1 comment to ");
    put_shown(&line, doc);
  }
  emit_line(&line);
  free(doc);
  kbc_store_close(s);
  return EXIT_OK;
}

/* `kbc comments resolve|unresolve --artifact-id ID|--path FILE
 *   <comment_id>`
 *
 * The Rust takes the comment id as a trailing positional after kb and
 * artifact_id, and offers `--all` to flip every comment on the document in
 * one daemon round trip. There is no round trip here and no bulk call in the
 * frozen store, so `--all` is not offered: pretending one UPDATE per
 * comment is the atomic `resolve_all` endpoint would be a lie about what
 * happened if the process died halfway.
 *
 * The comment id is a positional, so the document comes from the flags —
 * which is the same shape the Rust's own `Reply`/`Edit`/`Delete`/
 * `Reanchor` subcommands use for exactly this reason ("comment_id is the
 * sole positional (it is required, so it can't trail the optional
 * kb/artifact positionals without ambiguity)"). */
static int comments_set_resolved(int argc, char **argv, int start,
                                 bool resolved) {
  opts o;
  parse_verb(FLAGS_COMMENTS, argc, argv, start, &o);
  const char *verb = resolved ? "resolve" : "unresolve";
  if (o.npos == 0) {
    die_user("comments %s needs a <comment_id>", verb);
  }
  if (o.npos > 1) {
    die_user("comments %s takes one <comment_id>", verb);
  }
  const char *cid = o.positional[0];
  if (!kbc_id_is_valid(cid)) {
    die_user("<comment_id> %s: an id is %u lowercase hex chars", cid,
             KBC_MAX_ID_LEN);
  }
  if (o.body != NULL) {
    die_user("comments %s takes no --body", verb);
  }
  kbc_store *s = open_comment_store();
  char *doc = comment_doc_id(s, &o);
  kbc_err e;
  kbc_err_reset(&e);
  /* The document is resolved and checked first so `resolve` on a comment of
   * some OTHER document reports the mismatch rather than silently flipping
   * a row the caller did not name. */
  kbc_arena *a = kbc_arena_new(4096);
  if (a == NULL) {
    free(doc);
    kbc_store_close(s);
    die_io("out of memory");
 }
  kbc_comment *rows = NULL;
  size_t n = 0;
  kbc_err_reset(&e);
  kbc_status st = kbc_store_list_comments(s, a, doc, KBC_MAX_HITS, &rows, &n, &e);
  if (kbc_failed(st)) {
    kbc_arena_free(a);
    free(doc);
    kbc_store_close(s);
    fail_status(st, &e, verb, NULL);
  }
  bool found = false;
  for (size_t i = 0; i < n; i++) {
    if (strcmp(rows[i].id, cid) == 0) {
      found = true;
      break;
    }
  }
  kbc_arena_free(a);
  if (!found) {
    /* NOTFOUND, so fail_status classifies this as the user error it is: the
     * caller named a comment that is not on the document they named. The
     * message is assembled through a kbc_str because both operands are
     * operator-supplied and must be escaped before they reach a terminal. */
    kbc_str msg;
    kbc_str_init(&msg);
    line_puts(&msg, "comment ");
    put_shown(&msg, cid);
    line_puts(&msg, " is not on document ");
    put_shown(&msg, doc);
    kbc_err nf;
    kbc_err_reset(&nf);
    kbc_err_set(&nf, KBC_ERR_NOTFOUND, "%s", msg.ptr);
    kbc_str_free(&msg);
    free(doc);
    kbc_store_close(s);
    fail_status(KBC_ERR_NOTFOUND, &nf, verb, NULL);
  }
  kbc_err_reset(&e);
  st = kbc_store_set_comment_resolved(s, cid, resolved, &e);
  if (kbc_failed(st)) {
    free(doc);
    kbc_store_close(s);
    fail_status(st, &e, verb, NULL);
  }
  kbc_str line;
  kbc_str_init(&line);
  if (g_json) {
    line_puts(&line, "{\"ok\":true,\"comment_id\":");
    put_shown(&line, cid);
    line_puts(&line, ",\"status\":");
    put_shown(&line, resolved ? "resolved" : "open");
    line_puts(&line, "}");
  } else {
    line_puts(&line, resolved ? "resolved " : "unresolved ");
    put_shown(&line, cid);
  }
  emit_line(&line);
  free(doc);
  kbc_store_close(s);
  return EXIT_OK;
}

static int cmd_comments(int argc, char **argv, int start, const char *sub) {
  if (sub == NULL) {
    die_user("comments needs a subcommand: list, add, resolve, unresolve");
  }
  if (strcmp(sub, "list") == 0) {
    return comments_list(argc, argv, start);
  }
  if (strcmp(sub, "add") == 0) {
    return comments_add(argc, argv, start);
  }
  if (strcmp(sub, "resolve") == 0) {
    return comments_set_resolved(argc, argv, start, true);
  }
  if (strcmp(sub, "unresolve") == 0) {
    return comments_set_resolved(argc, argv, start, false);
  }
  die_user("unknown comments subcommand %s (list, add, resolve, unresolve)",
           sub);
}

/* ----------------------------------------------------------- daemon stop ---
 *
 * `kbc daemon stop` — terminate a running `kbc daemon`.
 *
 * THE PID FILE IS THE WHOLE CONTRACT, and kb-c's `kbc daemon` did not have
 * one until this verb needed it, so `cmd_daemon` above now writes and
 * removes it around the same lifecycle the Rust's does (daemon.rs: the
 * `run()` doc comment). The Rust's rules, kept because each one is a
 * failure somebody already hit:
 *
 *   - the file is written AFTER the listener binds, so a daemon that failed
 *     to bind leaves no pid file claiming a process that is not there;
 *   - a start refuses when the file names a LIVE pid other than our own, and
 *     prunes it when that pid is dead (a crash leaves the file behind);
 *   - the own-pid case counts as stale. In a container the daemon is pid 1,
 *     so after a hard kill the next boot is ALSO pid 1 and `kill -0 1`
 *     succeeds — without this the daemon would refuse to start forever;
 *   - a clean exit removes the file, so `stop` finding one means a daemon is
 *     genuinely running.
 *
 * `stop` sends SIGTERM and polls for up to 10 s, printing the Rust's two
 * progress lines. It never sends SIGKILL: a daemon draining in-flight
 * requests is doing the right thing, and escalating under it would turn a
 * slow shutdown into data loss. The timeout message names the pid so the
 * operator can escalate themselves, exactly as the Rust's does.
 *
 * Exit classes, this binary's convention and not the Rust's blanket 1:
 *   0  the daemon exited
 *   1  no pid file, an unreadable one, a stale one, or a pid that is not a
 *      comment-shaped id — the user asked to stop something that is not
 *      running, which is a usage error, not a daemon failure
 *   2  the signal could not be sent, or the daemon ignored SIGTERM for 10 s
 */
static char *daemon_pid_path(void) {
  const kbc_config *cfg = load_config();
  if (cfg->data_dir == NULL || cfg->data_dir[0] == '\0') {
    die_user("no data_dir configured: pass --config PATH or set HOME");
  }
  return join2(cfg->data_dir, "kb-daemon.pid");
}

/* `kill -0`, which reports existence without delivering anything. EPERM
 * means the process is there and merely not ours to signal — still alive,
 * and treating it as dead would let two daemons believe they own the store. */
static bool pid_is_alive(long pid) {
  if (kill((pid_t)pid, 0) == 0) {
    return true;
  }
  return errno == EPERM;
}

/* The recorded pid, or 0 when the file is absent, unreadable, or holds
 * something that is not a bare positive decimal — the three ways a pid file
 * is stale, since nothing else ever writes this one. */
static long read_daemon_pid(const char *path) {
  kbc_str s;
  kbc_str_init(&s);
  kbc_err e;
  kbc_err_reset(&e);
  if (kbc_failed(kbc_str_read_file(path, &s, &e))) {
    kbc_str_free(&s);
    return 0;
  }
  long pid = 0;
  for (size_t i = 0; i < s.len; i++) {
    unsigned char c = (unsigned char)s.ptr[i];
    if (c == '\n' || c == '\r' || c == ' ' || c == '\t') {
      continue;
    }
    if (c < '0' || c > '9') {
      pid = 0;
      break;
    }
    pid = pid * 10 + (long)(c - '0');
    if (pid > 0x7fffffffL) {
      pid = 0;
      break;
    }
  }
  kbc_str_free(&s);
  return pid > 0 ? pid : 0;
}

/* Called by cmd_daemon once the listener is up. See the banner above for
 * why each branch exists. */
static void daemon_write_pidfile(const char *path) {
  long existing = read_daemon_pid(path);
  long ours = (long)getpid();
  if (existing != 0 && existing != ours && pid_is_alive(existing)) {
    die_user("another kb daemon is already running (pid %ld, file: %s); "
             "use `kbc daemon stop` to terminate it first",
             existing, path);
  }
  char buf[32];
  int n = snprintf(buf, sizeof buf, "%ld\n", ours);
  if (n < 0 || (size_t)n >= sizeof buf) {
    die_io("the pid does not fit in %zu bytes", sizeof buf);
  }
  kbc_err e;
  kbc_err_reset(&e);
  if (kbc_failed(kbc_str_write_file_atomic(path, buf, (size_t)n, &e))) {
    fail_status(e.status != KBC_OK ? e.status : KBC_ERR_IO, &e, "pid file",
                NULL);
  }
}

static int cmd_daemon_stop(void) {
  char *path = daemon_pid_path();
  long pid = read_daemon_pid(path);
  /* `die_user`/`die_io` exit the process, so nothing below frees `path` on
   * these two paths: freeing first and then naming it in the message is a
   * use-after-free that happens to read freed heap. */
  if (pid == 0) {
    die_user("no kb-daemon.pid at %s (daemon not running, or pid file "
             "missing)",
             path);
  }
  if (!pid_is_alive(pid)) {
    /* Prune it, as the Rust does, so the next start does not have to
     * rediscover the same corpse — and report it as the user error it is. */
    (void)unlink(path);
    die_user("pid %ld is dead; removed the stale file at %s", pid, path);
  }
  if (kill((pid_t)pid, SIGTERM) != 0) {
    int saved = errno;
    free(path);
    die_io("send SIGTERM to pid %ld: %s", pid, strerror(saved));
  }
  if (!g_json) {
    fprintf(stderr, "sent SIGTERM to kb daemon pid %ld; waiting for exit\n",
            pid);
  }
  /* 10 s, polled every 100 ms — the Rust's deadline and interval. A daemon
   * that drains in-flight requests and then exits lands here in well under a
   * second; the full ten is only reached by one that is genuinely stuck. */
  for (int waited = 0; waited < 100; waited++) {
    if (!pid_is_alive(pid)) {
      (void)unlink(path);
      if (g_json) {
        kbc_str line;
        kbc_str_init(&line);
        line_puts(&line, "{\"ok\":true,\"pid\":");
        (void)kbc_str_printf(&line, "%ld}", pid);
        emit_line(&line);
      } else {
        printf("kb daemon pid %ld exited cleanly\n", pid);
      }
      free(path);
      return EXIT_OK;
    }
    struct timespec ts = { 0, 100 * 1000 * 1000 };
    (void)nanosleep(&ts, NULL);
  }
  free(path);
  die_io("kb daemon pid %ld did not exit within 10 s — still alive after "
         "SIGTERM. Investigate (its graceful-shutdown drain may be stuck on "
         "in-flight requests) or send SIGKILL manually: kill -9 %ld",
         pid, pid);
}

/* ------------------------------------------------------------ bench init ---
 *
 * `kbc bench init --corpus DIR --output PATH [--n 40] [--seed 0xb33f]`
 *
 * Scaffolds a `queries.jsonl` labelled-query file by sampling N artifacts
 * from a corpus directory, one JSON object per line, each with an empty
 * `query` for the operator to fill in and a `relevant` array pre-seeded
 * with the sampled file's artifact id. This is the Rust's
 * BenchAction::Init, and it is a MEASUREMENT tool, not a product one: its
 * only consumer is `bench run`, which is the recall@k bake-off harness the
 * plan's R1 gate is built on. It is built here because the plan names it as
 * not-done and it is the first half of the quality gate — but it is built
 * for that reason and no other.
 *
 * Two things differ from the Rust, both forced by kb-c:
 *
 *   - the id. The Rust mints it with `ArtifactId::from_path(rel)` — a bare
 *     SHA-256 of the relative path. kb-c's id is `kbc_id_for_artifact`,
 *     which hashes (corpus, path) together precisely so ("kb","a/b") and
 *     ("k","b/a") cannot collide (src/ids.c). A scaffold carrying Rust
 *     ids would name rows that do not exist in a kb-c store, so the id
 *     written here is kb-c's. `--kb NAME` says which corpus the paths are
 * *     relative to, because without it there is no id to write.
 *   - the sampled extension set. The Rust samples `.html`/`.htm`; kb-c
 *     indexes `.html`, `.htm`, `.md` and `.markdown` (is_indexable in
 *     src/app.c) and a markdown corpus is the common case here, so
 *     sampling HTML alone would find nothing to scaffold.
 *
 * The sampler is SplitMix64 + a partial Fisher-Yates, ported exactly: same
 * seed, same scaffold, so a labeller and a collaborator start from one set.
 */
static void splitmix64_next(unsigned long long *state,
                           unsigned long long *out) {
  *state += 0x9e3779b97f4a7c15ull;
  unsigned long long z = *state;
  z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ull;
  z = (z ^ (z >> 27)) * 0x94d049bb133111ebull;
  *out = z ^ (z >> 31);
}

/* Recursive collect of indexable files, as corpus-relative paths. Depth is
 * bounded: a corpus is user data and a symlink loop or a pathological tree
 * must not turn a scaffolder into a hang. */
static void collect_indexable(const char *root, const char *rel,
                              kbc_strlist *out, unsigned depth) {
  if (depth > 32) {
    return;
  }
  char *dir = rel[0] != '\0' ? path_join(root, rel) : xstrdup(root);
  DIR *d = opendir(dir);
  if (d == NULL) {
    free(dir);
    return;
  }
  kbc_strlist names;
  kbc_strlist_init(&names);
  struct dirent *de;
  while ((de = readdir(d)) != NULL) {
    if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0) {
      continue;
    }
    (void)kbc_strlist_push(&names, de->d_name);
  }
  (void)closedir(d);
  /* Sorted, because readdir order is filesystem order and the sample must
   * not drift with it. */
  kbc_strlist_sort(&names);
  for (size_t i = 0; i < names.len; i++) {
    char *child_rel = rel[0] != '\0' ? path_join(rel, names.items[i])
                                     : xstrdup(names.items[i]);
    char *child_abs = path_join(dir, names.items[i]);
    struct stat st;
    /* lstat, not stat: a symlink is not followed. A corpus is untrusted
     * input (rule 9) and a link out of the tree would sample files the
     * corpus does not contain — and on a link cycle it would not
     * return. */
    if (lstat(child_abs, &st) == 0) {
      if (S_ISDIR(st.st_mode)) {
        collect_indexable(root, child_rel, out, depth + 1);
      } else if (S_ISREG(st.st_mode) && is_bench_indexable(child_rel)) {
        (void)kbc_strlist_push(out, child_rel);
      }
    }
    free(child_abs);
    free(child_rel);
  }
  kbc_strlist_free(&names);
  free(dir);
}

static int cmd_bench_init(int argc, char **argv, int start) {
  opts o;
  parse_verb(FLAGS_BENCH_INIT, argc, argv, start, &o);
  const kbc_config *cfg = load_config();
  const char *kb = o.has_kb ? o.kb : NULL;
  const char *corpus = o.corpus;
  if (kb == NULL && corpus == NULL) {
    die_user("bench init needs --kb NAME or --corpus DIR");
  }
  if (kb == NULL) {
    if (cfg->ncorpora != 1) {
      die_user("bench init needs --kb NAME: %zu corpora are configured",
               cfg->ncorpora);
    }
    kb = cfg->corpora[0].name;
  }
  if (!kb_name_ok(kb)) {
    die_user("invalid kb %s: a corpus name must be 1-%u bytes of [a-z0-9_-]",
             kb, KBC_MAX_KB_NAME);
  }
  if (corpus == NULL) {
    const kbc_corpus_cfg *cc = kbc_config_corpus(cfg, kb);
    if (cc == NULL) {
      die_user("no corpus named %s in the config", kb);
    }
    corpus = cc->path;
  }
  if (o.output == NULL) {
    die_user("bench init needs --output PATH");
  }
  if (kbc_path_exists(o.output)) {
    die_user("refusing to overwrite %s; delete it first if you really want to "
             "re-scaffold (labelled query sets are easy to lose)",
             o.output);
  }
  char *expanded = expand_tilde(o.output);
  char *out_path = bounded_path("bench init --output", expanded);
  free(expanded);

  struct stat cst;
  if (stat(corpus, &cst) != 0) {
    free(out_path);
    die_user("corpus dir does not exist: %s", corpus);
  }
  if (!S_ISDIR(cst.st_mode)) {
    free(out_path);
    die_user("corpus path is not a directory: %s", corpus);
  }

  kbc_strlist rels;
  kbc_strlist_init(&rels);
  collect_indexable(corpus, "", &rels, 0);
  if (rels.len == 0) {
    kbc_strlist_free(&rels);
    free(out_path);
    die_user("no .html/.htm/.md/.markdown files under %s; bench init needs at "
             "least one artifact",
             corpus);
  }

  /* Partial Fisher-Yates over the sorted list: shuffle only as far as `n`
   * and take the prefix. O(n) regardless of corpus size, and identical for
   * a given seed because `rels` is sorted and the PRNG is the Rust's. */
  size_t want = o.has_n ? o.n : 40;
  if (want > rels.len) {
    want = rels.len;
  }
  unsigned long long state = o.has_seed ? o.seed : 0xb33full;
  for (size_t i = 0; i < want; i++) {
    unsigned long long r = 0;
    splitmix64_next(&state, &r);
    size_t pick = i + (size_t)(r % (unsigned long long)(rels.len - i));
    char *tmp = rels.items[i];
    rels.items[i] = rels.items[pick];
    rels.items[pick] = tmp;
  }

  kbc_str buf;
  kbc_str_init(&buf);
  for (size_t i = 0; i < want; i++) {
    char id[KBC_MAX_ID_LEN + 1];
    kbc_id_for_artifact(id, kb, rels.items[i]);
    (void)kbc_str_puts(&buf, "{\"query\":\"\",\"relevant\":[");
    (void)kbc_str_append_json_string(&buf, id, strlen(id));
    (void)kbc_str_puts(&buf, "],\"notes\":");
    kbc_str note;
    kbc_str_init(&note);
    (void)kbc_str_puts(&note, "scaffold: ");
    (void)kbc_str_append(&note, rels.items[i], strlen(rels.items[i]));
    (void)kbc_str_append_json_string(&buf, note.ptr, note.len);
    kbc_str_free(&note);
    /* One object per line: the jsonl contract the Rust's serialiser keeps
     * by using to_string rather than to_string_pretty. */
    (void)kbc_str_puts(&buf, "}\n");
  }
  kbc_err e;
  kbc_err_reset(&e);
  kbc_status st = kbc_str_write_file_atomic(out_path, buf.ptr, buf.len, &e);
  kbc_str_free(&buf);
  if (kbc_failed(st)) {
    kbc_strlist_free(&rels);
    free(out_path);
    fail_status(st, &e, "bench init", NULL);
  }
  if (!g_json) {
    printf("scaffolded %zu query rows from %zu artifacts in %s\n", want,
           rels.len, corpus);
    printf("  -> %s\n", out_path);
    printf("  next: fill in each line's empty `query` field with the phrase "
           "to evaluate.\n");
  }
  kbc_strlist_free(&rels);
  free(out_path);
  return EXIT_OK;
}

/* ------------------------------------------------------------------ help --- */

static void usage(FILE *out) {
  fprintf(out,
          "usage: kbc [--json] [--daemon URL] [--config PATH] <verb> [args]\n"
          "\n"
          "The global flags above may also be given after the verb, among its\n"
          "own flags: `kbc reindex --kb notes` and `kbc reindex --config\n"
          "kb.toml` both work. A global repeated in one command line takes\n"
          "its last value.\n"
          "verbs:\n"
          "  daemon [--bind ADDR] [--port N] [--foreground]\n"
          "  daemon stop             (SIGTERM the running daemon, wait 10s)\n"
          "  add <dir> --kb NAME\n"
          "  search <query> [--kb NAME] [--mode hybrid|keyword|semantic]"
          " [--limit N]\n"
          "  get <id> [--source]\n"
          "  reindex [--kb NAME]\n"
          "  list [--kb NAME] [--limit N]\n"
          "  status\n"
          "  comments list (--artifact-id ID | --path FILE) [--all]"
          " [--limit N]\n"
          "  comments add (--artifact-id ID | --path FILE) --body TEXT"
          " [--anchor A] [--author X]\n"
          "  comments resolve <comment_id> (--artifact-id ID | --path"
          " FILE)\n"
          "  comments unresolve <comment_id> (--artifact-id ID | --path"
          " FILE)\n"
          "  bench [--queries N] [--repeat N] [--corpus DIR]\n"
          "  bench init (--kb NAME | --corpus DIR) --output PATH [--n N]"
          " [--seed S]\n"
          "  backup <kb> [--out PATH]\n"
          "  backup --all            (one tarball per corpus; no --out)\n"
          "  restore <tarball> --kb NAME [--force]\n"
          "  prune --days N [--apply]  (retention; a DRY RUN without --apply)\n"
          "  metrics [--out PATH]      (the daemon's Prometheus exposition)\n"
          "  config show\n"
          "  token generate\n"
          "  version\n"
          "\n"
          "prune removes READING HISTORY rows older than the window, exactly\n"
          "as the original's retention sweep does. It never removes a\n"
          "document: the original's `retention_prune` deliberately leaves the\n"
          "artifact tables alone, so there is no corpus for it to lose.\n"
          "\n"
          "comments verbs read the store directly: kb-c has no review-file\n"
          "routes, so its comments are table rows, not daemon-side state.\n"
          "That is why they take no --daemon.\n");
}

/* ------------------------------------------------------------------ main --- */

int main(int argc, char **argv) {
  int i = 1;
  for (; i < argc; i++) {
    const char *s = argv[i];
    if (strcmp(s, "--json") == 0) {
      g_json = true;
      continue;
    }
    if (strcmp(s, "--daemon") == 0) {
      if (i + 1 >= argc) {
        die_user("--daemon needs an argument");
      }
      g_daemon_flag = argv[++i];
      continue;
    }
    if (strcmp(s, "--config") == 0) {
      if (i + 1 >= argc) {
        die_user("--config needs an argument");
      }
      g_config_flag = argv[++i];
      continue;
    }
    if (strcmp(s, "--help") == 0 || strcmp(s, "-h") == 0) {
      usage(stdout);
      free_config();
      return EXIT_OK;
    }
    if (strcmp(s, "--version") == 0 || strcmp(s, "-V") == 0) {
      int rc = cmd_version();
      free_config();
      return rc;
    }
    break;
  }
  if (i >= argc) {
    if (g_json) {
      json_error("no verb given");
      free_config();
      return EXIT_USER;
    }
    usage(stderr);
    free_config();
    return EXIT_USER;
  }

  const char *verb = argv[i];
  int start = i + 1;
  /* The first non-flag token is the subcommand, so `config --json show`
   * reads the same as `config show --json`. */
  const char *sub = NULL;
  for (int k = start; k < argc; k++) {
    if (argv[k][0] != '-') {
      sub = argv[k];
      break;
    }
  }
  if (strcmp(verb, "config") == 0) {
    if (sub == NULL || strcmp(sub, "show") != 0) {
      die_user("config needs a subcommand: show");
    }
    int rc = cmd_config_show(argc, argv, start);
    free_config();
    return rc;
  }
  if (strcmp(verb, "token") == 0) {
    if (sub == NULL || strcmp(sub, "generate") != 0) {
      die_user("token needs a subcommand: generate");
    }
    int rc = cmd_token_generate(argc, argv, start);
    free_config();
    return rc;
  }
  /* A verb's SUBCOMMAND, found by skipping only the three global flags (and
   * their values). The looser "first token not starting with -" rule this
   * used above is wrong for a verb that has both subcommands and
   * value-taking flags: in `kbc daemon --bind 10.0.0.1` the address is not a
   * subcommand, and treating it as one would make the verb unreachable. */
  const char *subcmd = NULL;
  int subcmd_at = -1;
  for (int k = start; k < argc; k++) {
    const char *t = argv[k];
    if (strcmp(t, "--json") == 0) {
      continue;
    }
    if (strcmp(t, "--daemon") == 0 || strcmp(t, "--config") == 0) {
      k++;
      continue;
    }
    if (t[0] == '-') {
      break; /* a verb flag: everything after belongs to that verb */
    }
    subcmd = t;
    subcmd_at = k;
    break;
  }
  if (strcmp(verb, "comments") == 0) {
    int rc = cmd_comments(argc, argv, subcmd_at < 0 ? start : subcmd_at + 1,
                          subcmd);
    free_config();
    return rc;
  }
  if (strcmp(verb, "bench") == 0 && subcmd != NULL &&
      strcmp(subcmd, "init") == 0) {
    int rc = cmd_bench_init(argc, argv, subcmd_at + 1);
    free_config();
    return rc;
  }
  if (strcmp(verb, "daemon") == 0 && subcmd != NULL &&
      strcmp(subcmd, "stop") == 0) {
    if (subcmd_at + 1 < argc) {
      die_user("daemon stop takes no arguments");
    }
    int rc = cmd_daemon_stop();
    free_config();
    return rc;
  }
  /* A verb that HAS subcommands and was given one it does not have is a
   * usage error, not a silently-ignored token. `kbc bench discover` is the
   * live example: discover is a labelling aid over `kbc search`, which this
   * binary already has, so it is not ported and saying so beats running a
   * search the operator did not ask for. */
  if (subcmd != NULL && strcmp(verb, "bench") == 0) {
    die_user("unknown bench subcommand %s (init)", subcmd);
  }

  int rc;
  if (strcmp(verb, "search") == 0) {
    rc = cmd_search(argc, argv, start);
  } else if (strcmp(verb, "get") == 0) {
    rc = cmd_get(argc, argv, start);
  } else if (strcmp(verb, "list") == 0) {
    rc = cmd_list(argc, argv, start);
  } else if (strcmp(verb, "reindex") == 0) {
    rc = cmd_reindex(argc, argv, start);
  } else if (strcmp(verb, "status") == 0) {
    rc = cmd_status(argc, argv, start);
  } else if (strcmp(verb, "add") == 0) {
    rc = cmd_add(argc, argv, start);
  } else if (strcmp(verb, "bench") == 0) {
    rc = cmd_bench(argc, argv, start);
  } else if (strcmp(verb, "daemon") == 0) {
    rc = cmd_daemon(argc, argv, start);
  } else if (strcmp(verb, "backup") == 0) {
    rc = cmd_backup(argc, argv, start);
  } else if (strcmp(verb, "restore") == 0) {
    rc = cmd_restore(argc, argv, start);
  } else if (strcmp(verb, "prune") == 0) {
    rc = cmd_prune(argc, argv, start);
  } else if (strcmp(verb, "metrics") == 0) {
    rc = cmd_metrics(argc, argv, start);
  } else if (strcmp(verb, "version") == 0) {
    rc = cmd_version();
  } else if (strcmp(verb, "help") == 0) {
    usage(stdout);
    rc = EXIT_OK;
  } else {
    if (g_json) {
      json_errorf("unknown verb %s", verb);
      free_config();
      return EXIT_USER;
    }
    usage(stderr);
    die_user("unknown verb %s", verb);
  }
  free_config();
  return rc;
}
