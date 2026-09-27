/* kbcbench-client.c — closed-loop HTTP load client for bench/bench-kbc.sh.
 *
 * The python client in that script is the readable reference implementation and
 * stays the definition of the measurement. It cannot, however, be the thing that
 * finds the server's limit: one interpreter with one GIL saturated before the
 * daemon did, so the number it reported was the harness's. This client is the
 * same measurement without that ceiling — `level` connections, one thread
 * each, non-blocking sockets, and a request path that does the minimum a
 * conforming HTTP/1.1 client must.
 *
 * What it deliberately shares with the python client, so the two are
 * comparable and the numbers stay comparable with what is already published:
 *
 *   - the query set, read from the same file, in file order;
 *   - each connection issues the FULL query set `reps` times, so every query is
 *     sampled equally often at every concurrency level;
 *   - a barrier releases every connection at once, so the measured window
 *     contains no ramp-up;
 *   - the same request line and the same "read the whole body before timing the
 *     next request" rule;
 *   - the same output shapes: a `query_index<TAB>milliseconds` TSV of raw
 *     samples, and key=value lines on stdout. The script computes percentiles
 *     from the TSV with one shared code path, so the percentile algorithm is
 *     literally the same code for both clients.
 *
 * Build: cc -O2 -pthread -o kbcbench-client kbcbench-client.c
 */

#define _POSIX_C_SOURCE 200809L

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

/* Longest single sample, seconds. Generous: the python client allows 60 s per
 * attempt; a C client that gave up sooner would bias p99-max downward. */
#define CLIENT_TIMEOUT_S 60
#define RX_BUF 65536
#define MAX_PATH 4096

typedef struct {
  double ms;
  int qi;
} sample_t;

typedef struct {
  const char *host;
  const char *port;
  const char *path_prefix; /* "/api/search?q=..&mode=..&kb=..&limit=.." per query */
  char **paths;            /* nq prebuilt request paths */
  int nq;
  long reps;
  int conns_opened;
  long errors;
  pthread_barrier_t *barrier;
} cfg_t;

static double now_s(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

static double cpu_s(void) {
  struct timespec ts;
  if (clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &ts) != 0) return 0.0;
  return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

/* Percent-encode everything outside the unreserved set. The query set is plain
 * words today; encoding anyway means a query with a space or a quote cannot
 * change what the server is asked. */
static void url_encode(const char *in, char *out, size_t outsz) {
  static const char hex[] = "0123456789ABCDEF";
  size_t o = 0;
  for (const unsigned char *p = (const unsigned char *)in; *p; p++) {
    int plain = (*p >= 'A' && *p <= 'Z') || (*p >= 'a' && *p <= 'z') ||
                (*p >= '0' && *p <= '9') || *p == '-' || *p == '_' ||
                *p == '.' || *p == '~';
    if (o + (plain ? 1u : 3u) + 1 > outsz) break;
    if (plain) {
      out[o++] = (char)*p;
    } else {
      out[o++] = '%';
      out[o++] = hex[*p >> 4];
      out[o++] = hex[*p & 0x0f];
    }
  }
  out[o] = '\0';
}

static int set_nonblock(int fd) {
  int fl = fcntl(fd, F_GETFL, 0);
  if (fl < 0) return -1;
  return fcntl(fd, F_SETFL, fl | O_NONBLOCK);
}

/* Connect with a bounded wait. Blocking connect() would be simpler, but the
 * socket is non-blocking for the whole session and a non-blocking connect
 * reports its own timeout instead of hanging the whole client on one port. */
static int dial(const char *host, const char *port) {
  char svc[16];
  struct addrinfo hints, *res = NULL, *ai;
  snprintf(svc, sizeof svc, "%s", port);
  memset(&hints, 0, sizeof hints);
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;
  if (getaddrinfo(host, svc, &hints, &res) != 0) return -1;

  int fd = -1;
  for (ai = res; ai; ai = ai->ai_next) {
    fd = socket(ai->ai_family, ai->ai_socktype | SOCK_NONBLOCK, ai->ai_protocol);
    if (fd < 0) continue;
    int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
    if (connect(fd, ai->ai_addr, ai->ai_addrlen) == 0) break;
    if (errno == EINPROGRESS) {
      struct pollfd pfd = {.fd = fd, .events = POLLOUT, .revents = 0};
      int pr = poll(&pfd, 1, (int)CLIENT_TIMEOUT_S * 1000);
      if (pr > 0) {
        int soerr = 0;
        socklen_t sl = sizeof soerr;
        if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &soerr, &sl) == 0 &&
            soerr == 0)
          break;
      }
    }
    close(fd);
    fd = -1;
  }
  freeaddrinfo(res);
  if (fd < 0) return -1;
  (void)set_nonblock(fd);
  return fd;
}

/* Find "\r\n\r\n" in [buf, buf+n). Returns the offset just past it, or 0. */
static size_t header_end(const char *buf, size_t n) {
  if (n < 4) return 0;
  for (size_t i = 0; i + 4 <= n; i++) {
    if (buf[i] == '\r' && buf[i + 1] == '\n' && buf[i + 2] == '\r' &&
        buf[i + 3] == '\n')
      return i + 4;
  }
  return 0;
}

/* Content-Length out of a complete header block. -1 when absent, -2 on
 * something this client will not silently mis-frame (chunked, or a value that
 * overflows). */
static long content_length(const char *hdr, size_t n) {
  static const char kName[] = "content-length:";
  const char *end = hdr + n;
  for (const char *p = hdr; p + sizeof kName - 1 <= end; p++) {
    if (p != hdr && p[-1] != '\n') continue;
    if (strncasecmp(p, kName, sizeof kName - 1) != 0) continue;
    const char *v = p + sizeof kName - 1;
    while (v < end && (*v == ' ' || *v == '\t')) v++;
    if (v >= end || *v < '0' || *v > '9') return -2;
    unsigned long long acc = 0;
    for (; v < end && *v >= '0' && *v <= '9'; v++) {
      acc = acc * 10ull + (unsigned long long)(*v - '0');
      if (acc > 1ull << 40) return -2;
    }
    return (long)acc;
  }
  return -1;
}

/* One request/response exchange on a connected, non-blocking socket. Returns 0
 * on a complete 200 response, -1 if the connection is unusable and the caller
 * should redial. A 200 check matters: a 503 from the busy path is a completed
 * exchange of the wrong thing, and counting it as a hit would flatter the server.
 */
static int exchange(int fd, const char *path, double deadline) {
  char req[MAX_PATH + 256];
  int rl = snprintf(req, sizeof req,
                    "GET %s HTTP/1.1\r\nHost: bench\r\n"
                    "Accept: */*\r\nConnection: keep-alive\r\n\r\n",
                    path);
  if (rl <= 0 || (size_t)rl >= sizeof req) return -1;

  size_t sent = 0;
  while (sent < (size_t)rl) {
    ssize_t w = send(fd, req + sent, (size_t)rl - sent, MSG_NOSIGNAL);
    if (w > 0) {
      sent += (size_t)w;
      continue;
    }
    if (w < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
      if (now_s() > deadline) return -1;
      struct pollfd p = {.fd = fd, .events = POLLOUT, .revents = 0};
      poll(&p, 1, 200);
      continue;
    }
    return -1;
  }

  char buf[RX_BUF];
  size_t have = 0, he = 0;
  long clen = -1;
  for (;;) {
    if (he) {
      if (clen < 0) {
        clen = content_length(buf, he);
        if (clen == -2) return -1; /* not framed as this client expects */
      }
      if (clen >= 0 && have >= he + (size_t)clen) {
        /* Status line check: "HTTP/1.1 200 ..." */
        if (have < 12 || memcmp(buf, "HTTP/1.", 7) != 0) return -1;
        return (strncmp(buf + 9, "200", 3) == 0) ? 0 : -1;
      }
    }
    if (now_s() > deadline) return -1;
    struct pollfd p = {.fd = fd, .events = POLLIN, .revents = 0};
    int pr = poll(&p, 1, 200);
    if (pr < 0) {
      if (errno == EINTR) continue;
      return -1;
    }
    if (pr == 0) continue;
    ssize_t r = recv(fd, buf + have, sizeof buf - have, 0);
    if (r > 0) {
      /* A pipelined or unsolicited extra response byte would desynchronise the
       * next exchange; refuse rather than measure someone else's bytes. */
      if (have + (size_t)r >= sizeof buf) return -1;
      have += (size_t)r;
      if (!he) he = header_end(buf, have);
      continue;
    }
    if (r == 0) return -1; /* peer closed keep-alive */
    if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) continue;
    return -1;
  }
}

typedef struct {
  const cfg_t *cfg;
  int id;
  sample_t *samples;
  long nsamples;
  long errors;
  long conns_opened;
  int failed_connect;
} worker_t;

static void *worker_main(void *arg) {
  worker_t *w = (worker_t *)arg;
  const cfg_t *c = w->cfg;
  long cap = c->reps * c->nq;
  sample_t *s = (sample_t *)malloc((size_t)cap * sizeof *s);
  w->samples = s;
  if (!s) {
    /* The barrier is released even on the failure path: a worker that returns
     * without arriving would hang every other worker and the main thread
     * instead of failing the run. */
    w->failed_connect = 1;
    pthread_barrier_wait(c->barrier);
    return NULL;
  }
  w->samples = s;

  int fd = dial(c->host, c->port);
  if (fd < 0) {
    w->failed_connect = 1;
    pthread_barrier_wait(c->barrier);
    return NULL;
  }
  w->conns_opened = 1;

  /* Every connection is connected and blocked here before any request is sent,
   * so the measured window contains no ramp-up. */
  pthread_barrier_wait(c->barrier);

  long n = 0;
  int dead = 0;
  for (long rep = 0; rep < c->reps && !dead; rep++) {
    for (int qi = 0; qi < c->nq; qi++) {
      double t0 = now_s();
      int ok = exchange(fd, c->paths[qi], t0 + CLIENT_TIMEOUT_S);
      if (ok != 0) {
        /* One redial, exactly like the python client: a daemon that closed an
         * idle keep-alive is not an error, but a socket that stays broken is. */
        close(fd);
        fd = dial(c->host, c->port);
        w->conns_opened++;
        if (fd >= 0)
          ok = exchange(fd, c->paths[qi], now_s() + CLIENT_TIMEOUT_S);
        if (ok != 0) {
          w->errors++;
          close(fd);
          fd = dial(c->host, c->port);
          w->conns_opened++;
          if (fd < 0) {
            /* The server is gone. Stop this connection instead of re-dialling:
             * the samples from here on would be dial latency, not query
             * latency, and would drag every percentile with them. */
            dead = 1;
          }
        }
      }
      s[n].qi = qi;
      s[n].ms = (now_s() - t0) * 1000.0;
      n++;
    }
  }
  if (fd >= 0) close(fd);
  w->nsamples = n;
  return NULL;
}

static int cmp_double(const void *a, const void *b) {
  double x = *(const double *)a, y = *(const double *)b;
  return (x > y) - (x < y);
}

int main(int argc, char **argv) {
  if (argc < 10) {
    fprintf(stderr,
            "usage: %s HOST PORT MODE KB LIMIT CONNS REPS QUERYFILE OUTTSV\n",
            argv[0]);
    return 2;
  }
  const char *host = argv[1], *port = argv[2], *mode = argv[3], *kb = argv[4];
  const char *limit = argv[5];
  int level = atoi(argv[6]);
  long reps = atol(argv[7]);
  const char *qfile = argv[8], *out = argv[9];
  if (level < 1 || reps < 1) {
    fprintf(stderr, "conns and reps must be >= 1\n");
    return 2;
  }

  /* --- query set: same file, same order, comments and blanks skipped ------ */
  FILE *qf = fopen(qfile, "r");
  if (!qf) {
    fprintf(stderr, "cannot read %s: %s\n", qfile, strerror(errno));
    return 2;
  }
  char **queries = NULL;
  int nq = 0, qcap = 0;
  char line[4096];
  while (fgets(line, sizeof line, qf)) {
    size_t L = strlen(line);
    while (L && (line[L - 1] == '\n' || line[L - 1] == '\r')) line[--L] = '\0';
    if (L == 0 || line[0] == '#') continue;
    if (nq == qcap) {
      qcap = qcap ? qcap * 2 : 16;
      char **t = (char **)realloc(queries, (size_t)qcap * sizeof *t);
      if (!t) { fclose(qf); return 2; }
      queries = t;
    }
    queries[nq] = strdup(line);
    if (!queries[nq]) { fclose(qf); return 2; }
    nq++;
  }
  fclose(qf);
  if (nq == 0) {
    fprintf(stderr, "no queries in %s\n", qfile);
    return 2;
  }

  char **paths = (char **)calloc((size_t)nq, sizeof *paths);
  if (!paths) return 2;
  for (int i = 0; i < nq; i++) {
    char enc[3072];
    url_encode(queries[i], enc, sizeof enc);
    paths[i] = (char *)malloc(MAX_PATH);
    if (!paths[i]) return 2;
    snprintf(paths[i], MAX_PATH, "/api/search?q=%s&mode=%s&kb=%s&limit=%s", enc,
             mode, kb, limit);
  }

  cfg_t cfg;
  memset(&cfg, 0, sizeof cfg);
  cfg.host = host;
  cfg.port = port;
  cfg.paths = paths;
  cfg.nq = nq;
  cfg.reps = reps;
  cfg.path_prefix = "/api/search";

  pthread_barrier_t barrier;
  if (pthread_barrier_init(&barrier, NULL, (unsigned)level + 1u) != 0) {
    fprintf(stderr, "pthread_barrier_init failed\n");
    return 2;
  }
  cfg.barrier = &barrier;

  pthread_t *th = (pthread_t *)calloc((size_t)level, sizeof *th);
  worker_t *ws = (worker_t *)calloc((size_t)level, sizeof *ws);
  if (!th || !ws) return 2;

  /* The main thread is the level+1-th party, so it releases the barrier only
   * once every worker is connected and waiting. */
  for (int i = 0; i < level; i++) {
    ws[i].cfg = &cfg;
    ws[i].id = i;
    if (pthread_create(&th[i], NULL, worker_main, &ws[i]) != 0) {
      fprintf(stderr, "pthread_create failed at %d\n", i);
      return 2;
    }
  }

  double cpu0 = cpu_s();
  pthread_barrier_wait(&barrier);
  double wall0 = now_s();
  for (int i = 0; i < level; i++) pthread_join(th[i], NULL);
  double wall1 = now_s();
  double cpu_total = cpu_s() - cpu0;

  long total = 0, errors = 0, conns = 0, failed = 0;
  for (int i = 0; i < level; i++) {
    total += ws[i].nsamples;
    errors += ws[i].errors;
    conns += ws[i].conns_opened;
    failed += ws[i].failed_connect ? 1 : 0;
  }

  FILE *of = fopen(out, "w");
  if (!of) {
    fprintf(stderr, "cannot write %s: %s\n", out, strerror(errno));
    return 2;
  }
  static char obuf[1 << 20];
  setvbuf(of, obuf, _IOFBF, sizeof obuf);
  double *ms = (double *)malloc((size_t)(total ? total : 1) * sizeof *ms);
  if (!ms) return 2;
  long m = 0;
  for (int i = 0; i < level; i++) {
    for (long k = 0; k < ws[i].nsamples; k++) {
      fprintf(of, "%d\t%.4f\n", ws[i].samples[k].qi, ws[i].samples[k].ms);
      ms[m++] = ws[i].samples[k].ms;
    }
    free(ws[i].samples);
  }
  fflush(of);
  fclose(of);

  double dur = wall1 - wall0;
  /* The script computes percentiles from the TSV, so the client does not print
   * them; it prints only what cannot be recovered from the samples. */
  printf("REQUESTS=%ld\n", total);
  printf("CLIENTS=%d\n", level);
  printf("WALL_S=%.4f\n", dur);
  printf("RPS=%.2f\n", dur > 0 ? (double)total / dur : 0.0);
  printf("CLIENT_CPU_S=%.4f\n", cpu_total);
  printf("CLIENT_CPU_FRAC=%.4f\n", dur > 0 ? cpu_total / dur : 0.0);
  printf("CONNS_OPENED=%ld\n", conns);
  printf("ERRORS=%ld\n", errors);
  printf("CONNECT_FAILURES=%ld\n", failed);
  qsort(ms, (size_t)m, sizeof *ms, cmp_double);
  (void)m;
  return failed ? 3 : 0;
}
