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

/* The routes this client speaks; they must match KBC_ROUTES in src/httpd.c. */
#define R_SEARCH "/api/search"
#define R_ARTIFACTS "/api/artifacts"
#define R_REINDEX "/api/reindex"
#define R_STATS "/api/stats" /* the counters `kbc status` prints */

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
  exit(EXIT_USER);
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

/* The daemon's token: the config value when it carries one, else the first
 * line of the configured token file. No token file is not an error — a
 * loopback daemon runs without one. */
static char *read_token(const kbc_config *cfg) {
  if (cfg->token != NULL && cfg->token[0] != '\0') {
    return xstrdup(cfg->token);
  }
  if (cfg->token_path == NULL) {
    return NULL;
  }
  kbc_err e;
  kbc_err_reset(&e);
  kbc_str body;
  kbc_str_init(&body);
  if (kbc_failed(kbc_str_read_file(cfg->token_path, &body, &e))) {
    kbc_str_free(&body);
    return NULL;
  }
  size_t start = 0;
  while (start < body.len &&
         (body.ptr[start] == '\n' || body.ptr[start] == '\r')) {
    start++;
  }
  size_t end = start;
  while (end < body.len && body.ptr[end] != '\n' && body.ptr[end] != '\r') {
    end++;
  }
  char *tok = strndup(body.ptr + start, end - start);
  kbc_str_free(&body);
  if (tok != NULL && tok[0] == '\0') {
    free(tok);
    tok = NULL;
  }
  return tok;
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

static kbc_status read_all(int fd, kbc_str *out, kbc_err *err) {
  char buf[8192];
  for (;;) {
    ssize_t r = recv(fd, buf, sizeof buf, 0);
    if (r < 0) {
      if (errno == EINTR) {
        continue;
      }
      if (out->len > 0) {
        return KBC_OK; /* whatever arrived before the timeout still parses */
      }
      return kbc_err_set(err, KBC_ERR_IO, "recv: %s", strerror(errno));
    }
    if (r == 0) {
      return KBC_OK;
    }
    if (out->len + (size_t)r > HTTP_MAX_RESPONSE) {
      return kbc_err_set(err, KBC_ERR_IO, "response over %u bytes",
                         HTTP_MAX_RESPONSE);
    }
    if (kbc_failed(kbc_str_append(out, buf, (size_t)r))) {
      return kbc_err_set(err, KBC_ERR_NOMEM, "response buffer: out of memory");
    }
  }
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
  const kbc_config *cfg = load_config();
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
  s = read_all(fd, resp, err);
  close(fd);
  if (kbc_failed(s)) {
    return s;
  }

  const char *sep = find_crlfcrlf(resp->ptr, resp->len);
  if (sep == NULL) {
    return kbc_err_set(err, KBC_ERR_PARSE,
                       "truncated response: no header block");
  }
  size_t hlen = (size_t)(sep - resp->ptr);
  size_t line_end = 0;
  while (line_end < hlen && resp->ptr[line_end] != '\n') {
    line_end++;
  }
  s = parse_status_line(resp->ptr, line_end, status, err);
  if (kbc_failed(s)) {
    return s;
  }
  const char *body_ptr = resp->ptr + hlen + 4;
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
      return kbc_err_set(err, KBC_ERR_PARSE, "bad Content-Length: %s", num);
    }
    if (want > HTTP_MAX_RESPONSE) {
      return kbc_err_set(err, KBC_ERR_IO, "Content-Length %llu over %u bytes",
                         want, HTTP_MAX_RESPONSE);
    }
    if (have > (size_t)want) {
      have = (size_t)want;
    }
  }
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
  long port;
  size_t limit;
  size_t queries;
  size_t repeats;
  bool has_limit;
  bool has_queries;
  bool has_repeats;
  bool has_bind;
  bool has_port;
  bool has_kb;
  bool has_mode;
  bool source;
  bool foreground;
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
  for (int k = start; k < argc; k++) {
    const char *s = argv[k];
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
    } else if (strcmp(s, "--foreground") == 0) {
      o->foreground = true;
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
    printf("reindexed %.0f documents in %.0f us%s%s\n", dv->u.num, took,
           o.has_kb ? " in " : "", o.has_kb ? o.kb : "");
  } else if (status == 202) {
    /* The daemon accepted the work rather than running it inline. */
    printf("reindex accepted%s%s; run `kbc status` for the counters\n",
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
    exit(status_exit(KBC_ERR_IO, &e, "open the store"));
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
    rc = status_exit(KBC_ERR_IO, &e, "bench: open the store");
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
  kbc_status s = kbc_config_validate(cfg, &e);
  if (kbc_failed(s)) {
    exit(status_exit(s, &e, "config"));
  }
  char why[KBC_ERR_MSG_MAX];
  if (!kbc_config_bind_is_safe(cfg, why, sizeof why)) {
    die_user("%s", why[0] != '\0' ? why : "refusing an unsafe bind");
  }

  kbc_log_init(cfg->log_level, cfg->json_logs);

  if (!o.foreground) {
    pid_t pid = fork();
    if (pid < 0) {
      die_io("fork: %s", strerror(errno));
    }
    if (pid > 0) {
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
    exit(status_exit(KBC_ERR_IO, &e, "open the store"));
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
  kbc_httpd_stop(h);
  kbc_app_stop_watcher(app);
  kbc_app_close(app);
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
          "  add <dir> --kb NAME\n"
          "  search <query> [--kb NAME] [--mode hybrid|keyword|semantic]"
          " [--limit N]\n"
          "  get <id> [--source]\n"
          "  reindex [--kb NAME]\n"
          "  list [--kb NAME] [--limit N]\n"
          "  status\n"
          "  bench [--queries N] [--repeat N] [--corpus DIR]\n"
          "  config show\n"
          "  token generate\n"
          "  version\n");
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
