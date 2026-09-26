/* resolve-mb.c — the single-process microbenchmark for the search resolve.
 *
 * A query's hits are resolved by looking up N (corpus, path) pairs, one
 * sqlite3_step each, through one reset-and-rebind statement — the exact shape
 * kbc_store_get_artifacts_by_path() uses. That is what this measures, and
 * nothing else: no HTTP, no scoring, no JSON.
 *
 * It exists because the end-to-end numbers conflate the resolve with
 * everything around it. A/B a database here and the per-hit cost is a direct
 * subtraction; A/B the daemon and it is not.
 *
 * Build:  cc -O2 -o /tmp/resolve-mb bench/resolve-mb.c -lsqlite3
 * Usage:  resolve-mb <db> [pairs] [trials]
 * Prints one line per variant per trial plus a per-variant min/median, because
 * this host is shared: the MINIMUM over trials is the stable estimator and the
 * median is the honest one, and a claim that only the median supports is not
 * worth making.
 */
#define _POSIX_C_SOURCE 200809L
#include <sqlite3.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static double now(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

static int cmp_d(const void *a, const void *b) {
  double x = *(const double *)a, y = *(const double *)b;
  return x < y ? -1 : (x > y);
}

typedef struct {
  char *corpus;
  char *path;
} pair;

#define MAXV 8
static const char *V_NAME[MAXV];
static const char *V_SQL[MAXV];
static int nvar = 0;

static void variant(const char *name, const char *sql) {
  V_NAME[nvar] = name;
  V_SQL[nvar] = sql;
  nvar++;
}

/* Returns the per-lookup microseconds, or a negative count when the schema
 * cannot answer the variant at all (a database whose `source` has already
 * been moved out) — a skip, not a zero. */
static double trial(sqlite3 *db, const char *dbfile, int v, pair *ps, int np, int reps) {
  sqlite3_stmt *st = NULL;
  if (sqlite3_prepare_v2(db, V_SQL[v], -1, &st, NULL) != SQLITE_OK) {
    if (v == 0) printf("  (`source` is not a column of artifacts in %s)\n", dbfile);
    sqlite3_finalize(st);
    return -1.0;
  }
  double t0 = now();
  for (int r = 0; r < reps; r++)
    for (int i = 0; i < np; i++) {
      sqlite3_reset(st);
      sqlite3_bind_text(st, 1, ps[i].corpus, -1, SQLITE_TRANSIENT);
      sqlite3_bind_text(st, 2, ps[i].path, -1, SQLITE_TRANSIENT);
      if (sqlite3_step(st) == SQLITE_ROW) {
        /* Touch every selected column the way read_artifact() does, so the
         * decode cost is in the measurement and not optimised away. */
        for (int c = 0; c < sqlite3_column_count(st); c++)
          (void)sqlite3_column_text(st, c);
      }
    }
  double dt = now() - t0;
  sqlite3_finalize(st);
  return dt * 1e6 / ((double)reps * np);
}

int main(int argc, char **argv) {
  if (argc < 2) {
    fprintf(stderr, "usage: %s <db> [pairs] [trials]\n", argv[0]);
    return 2;
  }
  int np = argc > 2 ? atoi(argv[2]) : 10;
  int trials = argc > 3 ? atoi(argv[3]) : 15;
  int reps = np >= 50 ? 20 : 200;

  sqlite3 *db = NULL;
  if (sqlite3_open_v2(argv[1], &db, SQLITE_OPEN_READONLY, NULL) != SQLITE_OK) {
    fprintf(stderr, "open: %s\n", sqlite3_errmsg(db));
    return 1;
  }

  pair *ps = calloc((size_t)np, sizeof(*ps));
  sqlite3_stmt *q = NULL;
  if (sqlite3_prepare_v2(db,
                         "SELECT corpus, path FROM artifacts ORDER BY id LIMIT ?1;",
                         -1, &q, NULL) != SQLITE_OK) {
    fprintf(stderr, "pair query: %s\n", sqlite3_errmsg(db));
    return 1;
  }
  sqlite3_bind_int(q, 1, np);
  for (int i = 0; i < np && sqlite3_step(q) == SQLITE_ROW; i++) {
    ps[i].corpus = strdup((const char *)sqlite3_column_text(q, 0));
    ps[i].path = strdup((const char *)sqlite3_column_text(q, 1));
  }
  sqlite3_finalize(q);
  if (ps[0].corpus == NULL) {
    fprintf(stderr, "no rows to resolve\n");
    return 1;
  }

  variant("wide (source in the row)",
          "SELECT id, corpus, path, title, kind, mtime_ns, size_bytes,"
          " content_hash, heading_count, summary, source FROM artifacts"
          " WHERE corpus = ?1 AND path = ?2;");
  variant("slim (source not selected)",
          "SELECT id, corpus, path, title, kind, mtime_ns, size_bytes,"
          " content_hash, heading_count, summary FROM artifacts"
          " WHERE corpus = ?1 AND path = ?2;");
  variant("slim, no corpus/path echo",
          "SELECT id, title, summary FROM artifacts"
          " WHERE corpus = ?1 AND path = ?2;");

  double *res = calloc((size_t)trials * (size_t)nvar, sizeof(double));
  /* A warm-up pass per variant, discarded: the first read of a page costs a
   * copy into the page cache, and the daemon's steady state is warm. */
  for (int v = 0; v < nvar; v++) trial(db, argv[1], v, ps, np, 2);
  for (int t = 0; t < trials; t++)
    for (int v = 0; v < nvar; v++)
      res[t * nvar + v] = trial(db, argv[1], v, ps, np, reps);

  printf("db=%s pairs=%d trials=%d reps=%d\n", argv[1], np, trials, reps);
  for (int v = 0; v < nvar; v++) {
    double *row = malloc((size_t)trials * sizeof(double));
    int m = 0;
    for (int t = 0; t < trials; t++)
      if (res[t * nvar + v] >= 0.0) row[m++] = res[t * nvar + v];
    if (m == 0) {
      printf("  %-28s not available in this schema\n", V_NAME[v]);
      free(row);
      continue;
    }
    qsort(row, (size_t)m, sizeof(double), cmp_d);
    printf("  %-28s min %6.2f us  p50 %6.2f us  per batch(%.0f us)\n", V_NAME[v],
           row[0], row[m / 2], row[m / 2] * np);
    free(row);
  }
  for (int v = 0; v < nvar; v++) {
    printf("  %-28s", V_NAME[v]);
    for (int t = 0; t < trials; t++)
      if (res[t * nvar + v] >= 0.0) printf(" %.2f", res[t * nvar + v]);
    printf("\n");
  }
  return 0;
}
