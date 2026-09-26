/* store.c — the SQLite persistence layer.
 *
 * THREADING MODEL (the whole of it): one sqlite3 connection guarded by one
 * mutex. Every public entry point takes the mutex for its whole duration and
 * releases it before returning, on every path including the error paths, so
 * two worker threads calling into the same kbc_store never touch the
 * connection at the same time. Copying rows into the caller's arena happens
 * while the mutex is still held, so a returned record never points at memory
 * a later sqlite3_step/sqlite3_finalize would release.
 *
 * SQL discipline: every value that came from a file, an HTTP request or a
 * query string is bound, never formatted into a statement. The only strings
 * assembled here are the fixed filter clauses of the four variants in
 * kbc_store_list_artifact_ids, each a compile-time literal chosen by a
 * boolean — never user text.
 */

#include <stdio.h>

#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

#include <sqlite3.h>

#include "kbc/mem.h"
#include "kbc/store.h"
#include "kbc/types.h"

/* ------------------------------------------------------------- layout ---- */

struct kbc_store {
  sqlite3 *db;
  pthread_mutex_t mu;
  /* Heap-allocated so kbc_store_schema_version can read it through a
   * `const kbc_store *` without casting the const away (-Wcast-qual). */
  atomic_int *version;
};

static const char *const SCHEMA_V1 =
    "CREATE TABLE IF NOT EXISTS artifacts ("
    " id TEXT PRIMARY KEY,"
    " corpus TEXT NOT NULL,"
    " path TEXT NOT NULL,"
    " title TEXT NOT NULL,"
    " kind INTEGER NOT NULL,"
    " mtime_ns INTEGER NOT NULL,"
    " size_bytes INTEGER NOT NULL,"
    " content_hash INTEGER NOT NULL,"
    " heading_count INTEGER NOT NULL DEFAULT 0,"
    " summary TEXT NOT NULL DEFAULT '',"
    " source TEXT,"
    " UNIQUE(corpus, path));"
    "CREATE TABLE IF NOT EXISTS chunks ("
    " doc_id TEXT NOT NULL REFERENCES artifacts(id) ON DELETE CASCADE,"
    " ord INTEGER NOT NULL,"
    " text TEXT NOT NULL,"
    " PRIMARY KEY(doc_id, ord));"
    "CREATE TABLE IF NOT EXISTS comments ("
    " id TEXT PRIMARY KEY,"
    " doc_id TEXT NOT NULL REFERENCES artifacts(id) ON DELETE CASCADE,"
    " anchor TEXT NOT NULL,"
    " author TEXT NOT NULL,"
    " body TEXT NOT NULL,"
    " created_at TEXT NOT NULL,"
    " resolved INTEGER NOT NULL DEFAULT 0);"
    "CREATE INDEX IF NOT EXISTS artifacts_corpus ON artifacts(corpus);"
    "CREATE INDEX IF NOT EXISTS artifacts_kind ON artifacts(kind);"
    "CREATE INDEX IF NOT EXISTS comments_doc ON comments(doc_id);";

/* v2 — the corpus link graph. Backlinks only: one row per outbound link
 * whose target resolves to an indexed document, keyed by the two
 * corpus-relative paths. Path-keyed rather than artifact-id-keyed because a
 * document's id is minted from (corpus, path) and a link is a name in a
 * source file, not a reference to a row: the reindex of a target must not
 * rewrite every edge pointing at it. */
static const char *const SCHEMA_V2 =
    "CREATE TABLE IF NOT EXISTS edges ("
    " corpus TEXT NOT NULL,"
    " src_path TEXT NOT NULL,"
    " dst_path TEXT NOT NULL,"
    " PRIMARY KEY(corpus, src_path, dst_path));"
    "CREATE INDEX IF NOT EXISTS edges_dst ON edges(corpus, dst_path);";

#define SCHEMA_VERSION 2

#define ARTIFACT_COLS                                                          \
  "id, corpus, path, title, kind, mtime_ns, size_bytes, content_hash, "     \
  "heading_count, summary, source"

enum {
  COL_ID = 0,
  COL_CORPUS,
  COL_PATH,
  COL_TITLE,
  COL_KIND,
  COL_MTIME,
  COL_SIZE,
  COL_HASH,
  COL_HEADINGS,
  COL_SUMMARY,
  COL_SOURCE,
  COL_COUNT
};

/* Two shapes of the same row, in the same column order, so one reader
 * (read_artifact) decodes both.
 *
 * `source` averages 12.5 KB against a few hundred bytes for everything else,
 * so a row that carries it is a multi-page record with an overflow chain, and
 * sqlite3_column_text() on it walks that chain — even when the C code goes on
 * to discard the value. Omitting the column from the SELECT is what stops the
 * walk: nothing asks for the overflow pages, and nothing faults them in. The
 * resolve path (a search's top-k, and the watcher's by-path lookup) only ever
 * wants the ten small columns, so it uses SLIM. Only the by-id read that
 * actually returns source to a caller — `kb get <id> --source`, the artifact
 * route — pays for the wide row. */
#define ARTIFACT_SLIM_COLS                                                      \
  "id, corpus, path, title, kind, mtime_ns, size_bytes, content_hash, "         \
  "heading_count, summary"

#define ARTIFACT_SELECT_BY_ID                                                  \
  "SELECT " ARTIFACT_COLS " FROM artifacts WHERE id = ?1;"
#define ARTIFACT_SELECT_BY_ID_SLIM                                             \
  "SELECT " ARTIFACT_SLIM_COLS " FROM artifacts WHERE id = ?1;"
#define ARTIFACT_SELECT_BY_PATH_SLIM                                           \
  "SELECT " ARTIFACT_SLIM_COLS                                                 \
  " FROM artifacts WHERE corpus = ?1 AND path = ?2;"

static kbc_status migrate_locked(kbc_store *s, kbc_err *err);
static kbc_status require_text(kbc_err *err, const char *what, const char *v,
                               size_t max);

/* ------------------------------------------------------------ plumbing --- */

static kbc_status sql_fail(kbc_err *err, const kbc_store *s, const char *what,
                           int rc) {
  char *msg = sqlite3_mprintf("%s: %s (rc=%d)", what, sqlite3_errmsg(s->db), rc);
  kbc_status st = kbc_err_set(err, KBC_ERR_SQL, "%s", msg ? msg : what);
  sqlite3_free(msg);
  return st;
}

static kbc_status bind_text(kbc_err *err, const kbc_store *s, sqlite3_stmt *st,
                            int i, const char *v) {
  /* SQLITE_TRANSIENT: sqlite copies, so a caller buffer that dies before
   * step() cannot dangle. */
  int rc = sqlite3_bind_text(st, i, v, -1, SQLITE_TRANSIENT);
  if (rc != SQLITE_OK) return sql_fail(err, s, "bind text", rc);
  return KBC_OK;
}

static kbc_status bind_i64(kbc_err *err, const kbc_store *s, sqlite3_stmt *st,
                           int i, int64_t v) {
  int rc = sqlite3_bind_int64(st, i, (sqlite3_int64)v);
  if (rc != SQLITE_OK) return sql_fail(err, s, "bind integer", rc);
  return KBC_OK;
}

static kbc_status exec_plain(kbc_err *err, const kbc_store *s,
                             const char *sql) {
  char *emsg = NULL;
  int rc = sqlite3_exec(s->db, sql, NULL, NULL, &emsg);
  if (rc != SQLITE_OK) {
    char *msg = sqlite3_mprintf("%s: %s", sql, emsg ? emsg : "?");
    kbc_status st = kbc_err_set(err, KBC_ERR_SQL, "%s", msg ? msg : sql);
    sqlite3_free(msg);
    sqlite3_free(emsg);
    return st;
  }
  sqlite3_free(emsg);
  return KBC_OK;
}

static kbc_status prepare(kbc_err *err, const kbc_store *s, const char *sql,
                          sqlite3_stmt **out) {
  *out = NULL;
  int rc = sqlite3_prepare_v2(s->db, sql, -1, out, NULL);
  if (rc != SQLITE_OK) return sql_fail(err, s, sql, rc);
  return KBC_OK;
}

/* Best-effort ROLLBACK. It must never touch err: the caller is already
 * holding a specific diagnosis, and a rollback that itself fails is not a
 * better one to report. */
static void rollback(const kbc_store *s) {
  char *emsg = NULL;
  (void)sqlite3_exec(s->db, "ROLLBACK;", NULL, NULL, &emsg);
  sqlite3_free(emsg);
}

/* Release st. `prior` is the caller's own status for the statement's work: when
 * it is a failure, the caller's diagnosis is the specific one and stays, and a
 * non-OK return from sqlite3_finalize() (a failed statement hands back its
 * pending error again) must not replace it. Only when the caller has nothing
 * to report does a bad finalize become the reported failure. */
static kbc_status finalize(kbc_err *err, const kbc_store *s, sqlite3_stmt *st,
                           kbc_status prior) {
  int rc = sqlite3_finalize(st);
  if (rc != SQLITE_OK && prior == KBC_OK) return sql_fail(err, s, "finalize", rc);
  return KBC_OK;
}

static void lock(kbc_store *s) { (void)pthread_mutex_lock(&s->mu); }
static void unlock(kbc_store *s) { (void)pthread_mutex_unlock(&s->mu); }

/* strdup is not declared under -std=c17; this is the whole of it. */
static char *dup_str(const char *s) {
  size_t n = strlen(s);
  char *p = malloc(n + 1);
  if (p == NULL) return NULL;
  memcpy(p, s, n + 1);
  return p;
}

/* Column text copied into the arena. sqlite yields const and we only re-point
 * at a const char *, so no qualifier is discarded. */
static const char *col_str(kbc_arena *a, sqlite3_stmt *st, int col) {
  const char *p = (const char *)sqlite3_column_text(st, col);
  return p ? kbc_arena_strdup(a, p) : NULL;
}

static void read_artifact(kbc_arena *a, sqlite3_stmt *st, bool with_source,
                          kbc_artifact *out) {
  memset(out, 0, sizeof(*out));
  out->id = col_str(a, st, COL_ID);
  out->corpus = col_str(a, st, COL_CORPUS);
  out->path = col_str(a, st, COL_PATH);
  out->title = col_str(a, st, COL_TITLE);
  out->kind = (kbc_kind)sqlite3_column_int(st, COL_KIND);
  out->mtime_ns = (int64_t)sqlite3_column_int64(st, COL_MTIME);
  out->size_bytes = (int64_t)sqlite3_column_int64(st, COL_SIZE);
  out->content_hash = (uint32_t)sqlite3_column_int64(st, COL_HASH);
  out->heading_count = (int32_t)sqlite3_column_int(st, COL_HEADINGS);
  out->summary = col_str(a, st, COL_SUMMARY);
  /* The slim statements do not have a `source` column at all. The column
   * count is what says which shape this row is, so the reader is the same
   * function either way. */
  out->source = (with_source && sqlite3_column_count(st) == COL_COUNT)
                    ? col_str(a, st, COL_SOURCE)
                    : NULL;
}

/* ---------------------------------------------------------- validation --- */

static kbc_status require_text(kbc_err *err, const char *what, const char *v,
                               size_t max) {
  if (v == NULL || v[0] == '\0')
    return kbc_err_set(err, KBC_ERR_INVALID, "%s: empty", what);
  size_t n = strlen(v);
  if (n > max)
    return kbc_err_set(err, KBC_ERR_INVALID, "%s: %zu bytes exceeds %zu", what, n,
                       max);
  return KBC_OK;
}

/* Runs a `SELECT <int64 expr>` and writes its single value. `corpus` is bound
 * to ?1 when non-NULL, else the statement must not use ?1. */
static kbc_status count_query(kbc_err *err, const kbc_store *s, const char *sql,
                              const char *corpus, int64_t *out) {
  sqlite3_stmt *st = NULL;
  kbc_status rc = prepare(err, s, sql, &st);
  if (rc == KBC_OK && corpus != NULL) rc = bind_text(err, s, st, 1, corpus);
  if (rc != KBC_OK) {
    (void)finalize(err, s, st, rc);
    return rc;
  }
  int step = sqlite3_step(st);
  if (step == SQLITE_ROW) {
    *out = (int64_t)sqlite3_column_int64(st, 0);
  } else if (step == SQLITE_DONE) {
    rc = kbc_err_set(err, KBC_ERR_INTERNAL, "%s: no row", sql);
  } else {
    rc = sql_fail(err, s, sql, step);
  }
  kbc_status fin = finalize(err, s, st, rc);
  return rc != KBC_OK ? rc : fin;
}

/* --------------------------------------------------------------- open ---- */

kbc_store *kbc_store_open(const kbc_config *cfg, kbc_err *err) {
  if (cfg == NULL || cfg->db_path == NULL) {
    (void)kbc_err_set(err, KBC_ERR_INVALID, "config: db_path is unset");
    return NULL;
  }
  if (cfg->db_path[0] == '\0') {
    (void)kbc_err_set(err, KBC_ERR_INVALID, "db_path: empty");
    return NULL;
  }
  if (strlen(cfg->db_path) > KBC_MAX_PATH_LEN) {
    (void)kbc_err_set(err, KBC_ERR_INVALID, "db_path: longer than %u bytes",
                      (unsigned)KBC_MAX_PATH_LEN);
    return NULL;
  }

  /* sqlite will not create the containing directory for us. */
  const char *slash = strrchr(cfg->db_path, '/');
  if (slash != NULL && slash != cfg->db_path) {
    kbc_str dir;
    kbc_str_init(&dir);
    kbc_status st =
        kbc_str_append(&dir, cfg->db_path, (size_t)(slash - cfg->db_path));
    if (st == KBC_OK) st = kbc_mkdir_p(dir.ptr, err);
    kbc_str_free(&dir);
    if (st != KBC_OK) return NULL;
  }

  sqlite3 *db = NULL;
  int rc = sqlite3_open_v2(cfg->db_path, &db,
                           SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, NULL);
  if (rc != SQLITE_OK) {
    char *msg = sqlite3_mprintf("open %s: %s", cfg->db_path,
                                db ? sqlite3_errmsg(db) : sqlite3_errstr(rc));
    (void)kbc_err_set(err, KBC_ERR_IO, "%s", msg ? msg : cfg->db_path);
    sqlite3_free(msg);
    if (db) (void)sqlite3_close(db);
    return NULL;
  }

  kbc_store *s = calloc(1, sizeof(*s));
  atomic_int *ver = calloc(1, sizeof(*ver));
  if (s == NULL || ver == NULL) {
    (void)kbc_err_set(err, KBC_ERR_NOMEM, "kbc_store: out of memory");
    free(ver);
    free(s);
    (void)sqlite3_close(db);
    return NULL;
  }
  s->db = db;
  s->version = ver;
  atomic_init(ver, 0);
  if (pthread_mutex_init(&s->mu, NULL) != 0) {
    (void)kbc_err_set(err, KBC_ERR_INTERNAL, "kbc_store: mutex init failed");
    free(ver);
    free(s);
    (void)sqlite3_close(db);
    return NULL;
  }

  /* WAL: readers never block the writer. NORMAL: a crash may lose the last
   * transaction but never the database. */
  const char *const pragmas[] = {
      "PRAGMA journal_mode = WAL;", "PRAGMA synchronous = NORMAL;",
      "PRAGMA foreign_keys = ON;",  "PRAGMA busy_timeout = 5000;",
      "PRAGMA temp_store = MEMORY;",
  };
  for (size_t i = 0; i < sizeof(pragmas) / sizeof(pragmas[0]); i++) {
    if (exec_plain(err, s, pragmas[i]) != KBC_OK) {
      kbc_store_close(s);
      return NULL;
    }
  }

  if (migrate_locked(s, err) != KBC_OK) {
    kbc_store_close(s);
    return NULL;
  }
  return s;
}

void kbc_store_close(kbc_store *s) {
  if (s == NULL) return;
  if (s->db != NULL) (void)sqlite3_close(s->db);
  (void)pthread_mutex_destroy(&s->mu);
  free(s->version);
  free(s);
}

int kbc_store_schema_version(const kbc_store *s) {
  if (s == NULL || s->version == NULL) return 0;
  /* The version only ever moves forward, under the mutex; it is read here
   * without it, so it is atomic. */
  return atomic_load(s->version);
}

/* ---------------------------------------------------------- migrations --- */

static kbc_status apply_v1(kbc_store *s, kbc_err *err) {
  kbc_status st = exec_plain(err, s, SCHEMA_V1);
  if (st != KBC_OK) return st;
  sqlite3_stmt *ins = NULL;
  st = prepare(err, s,
               "INSERT OR IGNORE INTO schema_version(version) VALUES (?1);",
               &ins);
  if (st != KBC_OK) return st;
  st = bind_i64(err, s, ins, 1, 1); /* step 1, whatever SCHEMA_VERSION is today */
  if (st == KBC_OK) {
    int step = sqlite3_step(ins);
    if (step != SQLITE_DONE)
      st = sql_fail(err, s, "record schema version", step);
  }
  kbc_status fin = finalize(err, s, ins, st);
  return st != KBC_OK ? st : fin;
}

static kbc_status apply_v2(kbc_store *s, kbc_err *err) {
  kbc_status st = exec_plain(err, s, SCHEMA_V2);
  if (st != KBC_OK) return st;
  sqlite3_stmt *ins = NULL;
  st = prepare(err, s,
               "INSERT OR IGNORE INTO schema_version(version) VALUES (?1);",
               &ins);
  if (st != KBC_OK) return st;
  st = bind_i64(err, s, ins, 1, 2);
  if (st == KBC_OK) {
    int step = sqlite3_step(ins);
    if (step != SQLITE_DONE)
      st = sql_fail(err, s, "record schema version", step);
  }
  kbc_status fin = finalize(err, s, ins, st);
  return st != KBC_OK ? st : fin;
}

static kbc_status migrate_locked(kbc_store *s, kbc_err *err) {
  kbc_status st = exec_plain(err, s,
                             "CREATE TABLE IF NOT EXISTS schema_version ("
                             " version INTEGER NOT NULL);");
  if (st != KBC_OK) return st;

  int64_t have = 0;
  st = count_query(err, s,
                   "SELECT IFNULL(MAX(version), 0) FROM schema_version;", NULL,
                   &have);
  if (st != KBC_OK) return st;
  if (have >= SCHEMA_VERSION) {
    atomic_store(s->version, (int)have);
    return KBC_OK;
  }

  /* One transaction for the whole ladder: each step and its version record
   * land together, so a crash mid-migration leaves the previous version
   * intact and the next open re-runs the same steps. Every step is written
   * to be re-runnable (IF NOT EXISTS / OR IGNORE), so a database that
   * already has step 1 only pays for step 2. */
  st = exec_plain(err, s, "BEGIN IMMEDIATE;");
  if (st != KBC_OK) return st;
  if (have < 1) st = apply_v1(s, err);
  if (st == KBC_OK && have < 2) st = apply_v2(s, err);
  if (st == KBC_OK) st = exec_plain(err, s, "COMMIT;");
  if (st != KBC_OK) {
    rollback(s);
    return st;
  }
  atomic_store(s->version, SCHEMA_VERSION);
  return KBC_OK;
}

/* ------------------------------------------------------------ artifacts -- */

kbc_status kbc_store_upsert_artifact(kbc_store *s, const kbc_artifact *a,
                                     kbc_err *err) {
  if (s == NULL || a == NULL)
    return kbc_err_set(err, KBC_ERR_INVALID, "upsert_artifact: null argument");
  kbc_status st = require_text(err, "artifact id", a->id, KBC_MAX_ID_LEN);
  if (st == KBC_OK) st = require_text(err, "artifact corpus", a->corpus, 255);
  if (st == KBC_OK)
    st = require_text(err, "artifact path", a->path, KBC_MAX_PATH_LEN);
  if (st == KBC_OK && a->title == NULL)
    st = kbc_err_set(err, KBC_ERR_INVALID, "artifact title: null");
  if (st == KBC_OK && a->summary == NULL)
    st = kbc_err_set(err, KBC_ERR_INVALID, "artifact summary: null");
  if (st == KBC_OK &&
      ((int)a->kind < 0 || (int)a->kind >= (int)KBC_KIND__COUNT))
    st = kbc_err_set(err, KBC_ERR_INVALID, "artifact kind: %d", (int)a->kind);
  if (st == KBC_OK && a->size_bytes < 0)
    st = kbc_err_set(err, KBC_ERR_INVALID, "artifact size_bytes: %lld",
                     (long long)a->size_bytes);
  if (st == KBC_OK && a->size_bytes > (int64_t)KBC_MAX_ARTIFACT_BYTES)
    st = kbc_err_set(err, KBC_ERR_INVALID, "artifact size_bytes: %lld > %u",
                     (long long)a->size_bytes,
                     (unsigned)KBC_MAX_ARTIFACT_BYTES);
  if (st != KBC_OK) return st;

  lock(s);

  /* The (corpus, path) unique index means a different id already holding this
   * slot is a real conflict, not a silent overwrite. */
  sqlite3_stmt *probe = NULL;
  st = prepare(err, s,
               "SELECT id FROM artifacts WHERE corpus = ?1 AND path = ?2;",
               &probe);
  if (st == KBC_OK) st = bind_text(err, s, probe, 1, a->corpus);
  if (st == KBC_OK) st = bind_text(err, s, probe, 2, a->path);
  if (st == KBC_OK) {
    int step = sqlite3_step(probe);
    if (step == SQLITE_ROW) {
      const char *other = (const char *)sqlite3_column_text(probe, 0);
      if (other == NULL || strcmp(other, a->id) != 0)
        st = kbc_err_set(err, KBC_ERR_CONFLICT,
                         "artifact %s/%s already exists as id %s", a->corpus,
                         a->path, other ? other : "?");
    } else if (step != SQLITE_DONE) {
      st = sql_fail(err, s, "conflict probe: step", step);
    }
  }
  kbc_status fin = finalize(err, s, probe, st);
  if (st == KBC_OK) st = fin;
  if (st != KBC_OK) {
    unlock(s);
    return st;
  }

  sqlite3_stmt *ins = NULL;
  st = prepare(err, s,
               "INSERT INTO artifacts (id, corpus, path, title, kind, mtime_ns,"
               " size_bytes, content_hash, heading_count, summary, source)"
               " VALUES (?1,?2,?3,?4,?5,?6,?7,?8,?9,?10,?11)"
               " ON CONFLICT(id) DO UPDATE SET"
               " corpus=excluded.corpus, path=excluded.path,"
               " title=excluded.title, kind=excluded.kind,"
               " mtime_ns=excluded.mtime_ns, size_bytes=excluded.size_bytes,"
               " content_hash=excluded.content_hash,"
               " heading_count=excluded.heading_count,"
               " summary=excluded.summary, source=excluded.source;",
               &ins);
  if (st == KBC_OK) st = bind_text(err, s, ins, 1, a->id);
  if (st == KBC_OK) st = bind_text(err, s, ins, 2, a->corpus);
  if (st == KBC_OK) st = bind_text(err, s, ins, 3, a->path);
  if (st == KBC_OK) st = bind_text(err, s, ins, 4, a->title);
  if (st == KBC_OK) st = bind_i64(err, s, ins, 5, (int64_t)a->kind);
  if (st == KBC_OK) st = bind_i64(err, s, ins, 6, a->mtime_ns);
  if (st == KBC_OK) st = bind_i64(err, s, ins, 7, a->size_bytes);
  if (st == KBC_OK) st = bind_i64(err, s, ins, 8, (int64_t)a->content_hash);
  if (st == KBC_OK) st = bind_i64(err, s, ins, 9, (int64_t)a->heading_count);
  if (st == KBC_OK) st = bind_text(err, s, ins, 10, a->summary);
  if (st == KBC_OK) {
    int b = (a->source != NULL) ? sqlite3_bind_text(ins, 11, a->source, -1,
                                                    SQLITE_TRANSIENT)
                                : sqlite3_bind_null(ins, 11);
    if (b != SQLITE_OK) st = sql_fail(err, s, "bind source", b);
  }
  if (st == KBC_OK) {
    int step = sqlite3_step(ins);
    if (step == SQLITE_CONSTRAINT)
      st = kbc_err_set(err, KBC_ERR_CONFLICT, "artifact %s: constraint", a->id);
    else if (step != SQLITE_DONE)
      st = sql_fail(err, s, "upsert artifact", step);
  }
  kbc_status fin2 = finalize(err, s, ins, st);
  if (st == KBC_OK) st = fin2;

  unlock(s);
  return st;
}

/* a2 is the by-path variant's path and is NULL for the by-id variant, so a
 * miss must name the value that was actually looked up. */
static kbc_status get_artifact_locked(kbc_store *s, kbc_arena *a,
                                      const char *sql, const char *a1,
                                      const char *a2, bool with_source,
                                      kbc_artifact *out, kbc_err *err) {
  sqlite3_stmt *st = NULL;
  kbc_status rc = prepare(err, s, sql, &st);
  if (rc == KBC_OK) rc = bind_text(err, s, st, 1, a1);
  if (rc == KBC_OK && a2 != NULL) rc = bind_text(err, s, st, 2, a2);
  if (rc != KBC_OK) {
    (void)finalize(err, s, st, rc);
    return rc;
  }
  int step = sqlite3_step(st);
  if (step == SQLITE_ROW) {
    read_artifact(a, st, with_source, out);
  } else if (step == SQLITE_DONE) {
    rc = (a2 != NULL)
             ? kbc_err_set(err, KBC_ERR_NOTFOUND, "artifact %s/%s: not found", a1,
                           a2)
             : kbc_err_set(err, KBC_ERR_NOTFOUND, "artifact %s: not found", a1);
  } else {
    rc = sql_fail(err, s, "get artifact: step", step);
  }
  kbc_status fin = finalize(err, s, st, rc);
  return rc != KBC_OK ? rc : fin;
}

kbc_status kbc_store_get_artifact(kbc_store *s, kbc_arena *a, const char *id,
                                  bool with_source, kbc_artifact *out,
                                  kbc_err *err) {
  if (s == NULL || a == NULL || out == NULL)
    return kbc_err_set(err, KBC_ERR_INVALID, "get_artifact: null argument");
  kbc_status st = require_text(err, "artifact id", id, KBC_MAX_ID_LEN);
  if (st != KBC_OK) return st;

  /* The wide statement is only for a caller that keeps `source`; without it
   * nothing ever asks sqlite for the overflow chain. */
  lock(s);
  st = get_artifact_locked(s, a,
                           with_source ? ARTIFACT_SELECT_BY_ID
                                       : ARTIFACT_SELECT_BY_ID_SLIM,
                           id, NULL, with_source, out, err);
  unlock(s);
  return st;
}

kbc_status kbc_store_get_artifact_by_path(kbc_store *s, kbc_arena *a,
                                          const char *corpus,
                                          const char *path,
                                          kbc_artifact *out, kbc_err *err) {
  if (s == NULL || a == NULL || out == NULL)
    return kbc_err_set(err, KBC_ERR_INVALID,
                       "get_artifact_by_path: null argument");
  kbc_status st = require_text(err, "corpus", corpus, 255);
  if (st == KBC_OK) st = require_text(err, "path", path, KBC_MAX_PATH_LEN);
  if (st != KBC_OK) return st;

  lock(s);
  st = get_artifact_locked(s, a, ARTIFACT_SELECT_BY_PATH_SLIM, corpus, path,
                           false, out, err);
  unlock(s);
  return st;
}

kbc_status kbc_store_get_artifacts_by_path(kbc_store *s, kbc_arena *a,
                                           const char *const *corpora,
                                           const char *const *paths, size_t n,
                                           kbc_artifact ***out, kbc_err *err) {
  if (s == NULL || a == NULL || out == NULL ||
      (n > 0 && (corpora == NULL || paths == NULL)))
    return kbc_err_set(err, KBC_ERR_INVALID,
                       "get_artifacts_by_path: null argument");
  *out = NULL;
  if (n == 0) return KBC_OK;

  /* The array is KBC_OWN and has EXACTLY the caller's n slots — a caller
   * indexing slot i of its own request has to find it there. The cap applies
   * to the SQL work only: past KBC_MAX_HITS pairs a slot stays NULL, which
   * reads as "no row" rather than as a shifted row. */
  kbc_artifact **slots = calloc(n, sizeof(*slots));
  kbc_artifact *arts = kbc_arena_calloc(a, n, sizeof(*arts));
  size_t *owner = malloc(n * sizeof(*owner)); /* slot -> distinct pair */
  const size_t np = n > (size_t)KBC_MAX_HITS ? (size_t)KBC_MAX_HITS : n;
  /* The distinct pairs, compacted. They need their own arrays because a
   * distinct pair's index is NOT its input slot: a pair that cannot be looked
   * up is skipped, and every later pair shifts down. Reading the input arrays
   * at a distinct index therefore looks up the wrong document as soon as one
   * pair is unlookupable. */
  const char **dc = malloc(np * sizeof(*dc));
  const char **dp = malloc(np * sizeof(*dp));
  if (slots == NULL || arts == NULL || owner == NULL || dc == NULL ||
      dp == NULL) {
    free(slots);
    free(owner);
    free(dc);
    free(dp);
    return kbc_err_set(err, KBC_ERR_NOMEM, "get_artifacts_by_path: %zu pairs",
                       n);
  }
  *out = slots;


  /* A pair that cannot be looked up (NULL, empty, over the cap) resolves to
   * no row. It is a slot the caller must be able to see the absence of, not a
   * reason to fail the batch and with it every other pair. */
  size_t distinct = 0;
  for (size_t i = 0; i < np; i++) {
    owner[i] = SIZE_MAX;
    if (kbc_failed(require_text(NULL, "corpus", corpora[i], 255)) ||
        kbc_failed(require_text(NULL, "path", paths[i], KBC_MAX_PATH_LEN))) {
      continue;
    }
    size_t seen = distinct;
    for (size_t j = 0; j < seen; j++) {
      if (strcmp(dc[j], corpora[i]) == 0 && strcmp(dp[j], paths[i]) == 0) {
        owner[i] = j;
        break;
      }
    }
    if (owner[i] == SIZE_MAX) {
      dc[distinct] = corpora[i];
      dp[distinct] = paths[i];
      owner[i] = distinct++;
    }
  }
  for (size_t i = np; i < n; i++) owner[i] = SIZE_MAX;
  if (distinct == 0) {
    free(slots);
    free(owner);
    free(dc);
    free(dp);
    *out = NULL;
    return kbc_err_set(err, KBC_ERR_INVALID,
                       "get_artifacts_by_path: no lookupable pair in %zu", n);
  }

  /* One lock, one statement, one step per DISTINCT pair.
   *
   * The mutex is the reason this exists: a search resolving its top k used to
   * take the store's lock k times, so every hit of every query serialised on
   * one connection. What the statement looks like is a separate question, and
   * it was measured: binding all n pairs as one `(?a AND ?b) OR (?c AND ?d)`
   * chain makes sqlite3_prepare_v2 re-parse and re-plan an n-term OR on every
   * query (193 us for 10 pairs here, against 168 us for the same ten lookups
   * through one reset-and-rebind statement). So the pairs go in as bound
   * parameters to a single prepared statement, reset between pairs, which
   * keeps the index seek per pair and pays the prepare once.
   *
   * The statement is the SLIM one. `source` is 12.5 KB of a ~12.8 KB row, so
   * selecting it makes every one of these steps decode a record whose tail
   * lives on overflow pages — a walk the resolver never wanted, since a search
   * hit reports an id and a summary and nothing else. Measured on the
   * 1,114-document benchmark database with bench/resolve-mb.c, 50 pairs, 15
   * interleaved trials: 5.5-6.6 us per lookup wide against 3.5-4.1 us slim.
   */
  lock(s);
  sqlite3_stmt *sel = NULL;
  kbc_status st = prepare(err, s, ARTIFACT_SELECT_BY_PATH_SLIM, &sel);
  for (size_t k = 0; st == KBC_OK && k < distinct; k++) {
    st = bind_text(err, s, sel, 1, dc[k]);
    if (st == KBC_OK) st = bind_text(err, s, sel, 2, dp[k]);
    if (st != KBC_OK) break;
    /* (corpus, path) is UNIQUE, so this step yields at most one row. */
    int step = sqlite3_step(sel);
    if (step == SQLITE_ROW) {
      read_artifact(a, sel, false, &arts[k]);
    } else if (step != SQLITE_DONE) {
      st = sql_fail(err, s, "get artifacts by path: step", step);
    }
    (void)sqlite3_reset(sel);
  }
  kbc_status fin = finalize(err, s, sel, st);
  if (st == KBC_OK) st = fin;
  unlock(s);
  if (kbc_failed(st)) {
    free(slots);
    free(owner);
    free(dc);
    free(dp);
    *out = NULL;
    return st;
  }

  for (size_t i = 0; i < n; i++) {
    /* A duplicate pair shares one arena record: identical strings, and the
     * caller only reads them. */
    slots[i] = (owner[i] != SIZE_MAX && arts[owner[i]].id != NULL)
                   ? &arts[owner[i]]
                   : NULL;
  }
  free(owner);
  free(dc);
  free(dp);
  return KBC_OK;
}

kbc_status kbc_store_delete_artifact(kbc_store *s, const char *id,
                                     kbc_err *err) {
  if (s == NULL)
    return kbc_err_set(err, KBC_ERR_INVALID, "delete_artifact: null store");
  kbc_status st = require_text(err, "artifact id", id, KBC_MAX_ID_LEN);
  if (st != KBC_OK) return st;

  lock(s);
  /* foreign_keys=ON, so the chunks and comments rows go with the artifact. */
  sqlite3_stmt *del = NULL;
  st = prepare(err, s, "DELETE FROM artifacts WHERE id = ?1;", &del);
  if (st == KBC_OK) st = bind_text(err, s, del, 1, id);
  if (st == KBC_OK) {
    int step = sqlite3_step(del);
    if (step != SQLITE_DONE) st = sql_fail(err, s, "delete artifact", step);
  }
  kbc_status fin = finalize(err, s, del, st);
  if (st == KBC_OK) st = fin;
  if (st == KBC_OK && sqlite3_changes(s->db) == 0)
    st = kbc_err_set(err, KBC_ERR_NOTFOUND, "artifact %s: not found", id);
  unlock(s);
  return st;
}

kbc_status kbc_store_list_artifact_ids(kbc_store *s, const char *corpus,
                                       kbc_kind kind, size_t limit,
                                       size_t offset, char ***ids_out,
                                       size_t *n_out, kbc_err *err) {
  if (s == NULL || ids_out == NULL || n_out == NULL)
    return kbc_err_set(err, KBC_ERR_INVALID,
                       "list_artifact_ids: null argument");
  if (corpus != NULL) {
    kbc_status st = require_text(err, "corpus", corpus, 255);
    if (st != KBC_OK) return st;
  }
  *ids_out = NULL;
  *n_out = 0;
  if (limit > KBC_MAX_HITS) limit = KBC_MAX_HITS;
  if (limit == 0) return KBC_OK;
  if (offset > (size_t)INT64_MAX)
    return kbc_err_set(err, KBC_ERR_INVALID, "offset: %zu too large", offset);

  const bool filter_kind = (int)kind >= 0 && (int)kind < (int)KBC_KIND__COUNT;
  /* Four fixed shapes: a NULL filter is a different statement, never
   * `WHERE corpus = NULL`. Every string here is a compile-time literal. */
  const char *sql;
  if (corpus != NULL && filter_kind)
    sql = "SELECT id FROM artifacts WHERE corpus = ?1 AND kind = ?2"
          " ORDER BY corpus, path, id LIMIT ?3 OFFSET ?4;";
  else if (corpus != NULL)
    sql = "SELECT id FROM artifacts WHERE corpus = ?1"
          " ORDER BY corpus, path, id LIMIT ?2 OFFSET ?3;";
  else if (filter_kind)
    sql = "SELECT id FROM artifacts WHERE kind = ?1"
          " ORDER BY corpus, path, id LIMIT ?2 OFFSET ?3;";
  else
    sql = "SELECT id FROM artifacts ORDER BY corpus, path, id"
          " LIMIT ?1 OFFSET ?2;";

  int first = 1;
  lock(s);
  sqlite3_stmt *st = NULL;
  kbc_status rc = prepare(err, s, sql, &st);
  if (rc == KBC_OK && corpus != NULL) {
    rc = bind_text(err, s, st, first, corpus);
    first++;
  }
  if (rc == KBC_OK && filter_kind) {
    rc = bind_i64(err, s, st, first, (int64_t)kind);
    first++;
  }
  if (rc == KBC_OK) rc = bind_i64(err, s, st, first, (int64_t)limit);
  if (rc == KBC_OK) rc = bind_i64(err, s, st, first + 1, (int64_t)offset);
  if (rc != KBC_OK) {
    (void)finalize(err, s, st, rc);
    unlock(s);
    return rc;
  }

  /* Grow a power-of-two array bounded by `limit`. The strings are malloc'd
   * and handed to the caller (KBC_OWN), not arena memory: the caller frees
   * each entry, then the array. */
  size_t cap = 8, len = 0;
  char **arr = NULL;
  for (;;) {
    int step = sqlite3_step(st);
    if (step == SQLITE_DONE) break;
    if (step != SQLITE_ROW) {
      rc = sql_fail(err, s, "list ids: step", step);
      break;
    }
    if (arr == NULL || len == cap) {
      size_t ncap = (cap * 2 > limit) ? limit : cap * 2;
      char **narr = realloc(arr, ncap * sizeof(*arr));
      if (narr == NULL) {
        rc = kbc_err_set(err, KBC_ERR_NOMEM, "list ids: %zu entries", ncap);
        break;
      }
      arr = narr;
      cap = ncap;
    }
    const char *id = (const char *)sqlite3_column_text(st, 0);
    char *copy = (id != NULL) ? dup_str(id) : NULL;
    if (copy == NULL) {
      rc = kbc_err_set(err, id != NULL ? KBC_ERR_NOMEM : KBC_ERR_INTERNAL,
                       "list ids: %s", id != NULL ? "out of memory" : "null id");
      break;
    }
    arr[len++] = copy;
  }
  kbc_status fin = finalize(err, s, st, rc);
  if (rc == KBC_OK) rc = fin;
  unlock(s);

  if (rc != KBC_OK) {
    for (size_t i = 0; i < len; i++) free(arr[i]);
    free(arr);
    return rc;
  }
  *ids_out = arr;
  *n_out = len;
  return KBC_OK;
}

kbc_status kbc_store_count_artifacts(kbc_store *s, const char *corpus,
                                     int64_t *out, kbc_err *err) {
  if (s == NULL || out == NULL)
    return kbc_err_set(err, KBC_ERR_INVALID,
                       "count_artifacts: null argument");
  if (corpus != NULL) {
    kbc_status st = require_text(err, "corpus", corpus, 255);
    if (st != KBC_OK) return st;
  }
  lock(s);
  kbc_status rc = count_query(
      err, s,
      corpus != NULL ? "SELECT COUNT(*) FROM artifacts WHERE corpus = ?1;"
                     : "SELECT COUNT(*) FROM artifacts;",
      corpus, out);
  unlock(s);
  return rc;
}

kbc_status kbc_store_list_corpora(kbc_store *s, kbc_strlist *out,
                                  kbc_err *err) {
  if (s == NULL || out == NULL)
    return kbc_err_set(err, KBC_ERR_INVALID, "list_corpora: null argument");

  lock(s);
  sqlite3_stmt *st = NULL;
  kbc_status rc = prepare(err, s,
                          "SELECT DISTINCT corpus FROM artifacts"
                          " ORDER BY corpus;",
                          &st);
  if (rc != KBC_OK) {
    unlock(s);
    return rc;
  }
  for (;;) {
    int step = sqlite3_step(st);
    if (step == SQLITE_DONE) break;
    if (step != SQLITE_ROW) {
      rc = sql_fail(err, s, "list corpora: step", step);
      break;
    }
    const char *c = (const char *)sqlite3_column_text(st, 0);
    if (c == NULL) {
      rc = kbc_err_set(err, KBC_ERR_INTERNAL, "list corpora: null corpus");
      break;
    }
    if (out->len >= KBC_MAX_CORPORA) {
      rc = kbc_err_set(err, KBC_ERR_CONFLICT, "corpora: more than %u",
                       (unsigned)KBC_MAX_CORPORA);
      break;
    }
    rc = kbc_strlist_push(out, c); /* copies into the caller's list */
    if (rc != KBC_OK) break;
  }
  kbc_status fin = finalize(err, s, st, rc);
  if (rc == KBC_OK) rc = fin;
  unlock(s);
  return rc;
}

kbc_status kbc_store_total_bytes(kbc_store *s, int64_t *out, kbc_err *err) {
  if (s == NULL || out == NULL)
    return kbc_err_set(err, KBC_ERR_INVALID, "total_bytes: null argument");
  lock(s);
  kbc_status rc =
      count_query(err, s,
                  "SELECT IFNULL(SUM(size_bytes), 0) FROM artifacts;", NULL,
                  out);
  unlock(s);
  return rc;
}


/* ------------------------------------------------------------------ edges -- */

kbc_status kbc_store_delete_edges(kbc_store *s, const char *corpus,
                                   const char *src_path, kbc_err *err) {
  if (s == NULL)
    return kbc_err_set(err, KBC_ERR_INVALID, "delete_edges: null store");
  kbc_status st = require_text(err, "edge corpus", corpus, 255);
  if (st == KBC_OK)
    st = require_text(err, "edge src_path", src_path, KBC_MAX_PATH_LEN);
  if (st != KBC_OK) return st;

  lock(s);
  sqlite3_stmt *st1 = NULL;
  st = prepare(err, s,
               "DELETE FROM edges WHERE corpus = ?1 AND src_path = ?2;",
               &st1);
  if (st == KBC_OK) st = bind_text(err, s, st1, 1, corpus);
  if (st == KBC_OK) st = bind_text(err, s, st1, 2, src_path);
  if (st == KBC_OK) {
    int step = sqlite3_step(st1);
    if (step != SQLITE_DONE) st = sql_fail(err, s, "delete edges: step", step);
  }
  kbc_status fin = finalize(err, s, st1, st);
  unlock(s);
  return st != KBC_OK ? st : fin;
}

kbc_status kbc_store_replace_edges(kbc_store *s, const char *corpus,
                                   const char *src_path,
                                   const char *const *dst_paths, size_t n,
                                   kbc_err *err) {
  if (s == NULL)
    return kbc_err_set(err, KBC_ERR_INVALID, "replace_edges: null store");
  if (n > 0 && dst_paths == NULL)
    return kbc_err_set(err, KBC_ERR_INVALID, "replace_edges: null dst_paths");
  kbc_status st = require_text(err, "edge corpus", corpus, 255);
  if (st == KBC_OK)
    st = require_text(err, "edge src_path", src_path, KBC_MAX_PATH_LEN);
  for (size_t i = 0; st == KBC_OK && i < n; i++) {
    st = require_text(err, "edge dst_path", dst_paths[i], KBC_MAX_PATH_LEN);
  }
  if (st != KBC_OK) return st;

  lock(s);
  /* REPLACE, in ONE transaction: the delete and every insert commit or roll
  * back together, so a re-indexed document whose links changed cannot leave
  * the old edges behind on a failure, and a duplicate target in the batch
  * collapses on the primary key rather than failing the whole write. */
  st = exec_plain(err, s, "BEGIN IMMEDIATE;");
  if (st != KBC_OK) {
    unlock(s);
    return st;
  }
  sqlite3_stmt *del = NULL;
  st = prepare(err, s,
               "DELETE FROM edges WHERE corpus = ?1 AND src_path = ?2;",
               &del);
  if (st == KBC_OK) st = bind_text(err, s, del, 1, corpus);
  if (st == KBC_OK) st = bind_text(err, s, del, 2, src_path);
  if (st == KBC_OK) {
    int step = sqlite3_step(del);
    if (step != SQLITE_DONE) st = sql_fail(err, s, "delete edges: step", step);
  }
  kbc_status fin = finalize(err, s, del, st);
  if (st == KBC_OK) st = fin;

  sqlite3_stmt *ins = NULL;
  if (st == KBC_OK) {
    st = prepare(err, s,
                 "INSERT OR IGNORE INTO edges(corpus, src_path, dst_path) "
                 "VALUES(?1,?2,?3);",
                 &ins);
    for (size_t i = 0; st == KBC_OK && i < n; i++) {
      st = bind_text(err, s, ins, 1, corpus);
      if (st == KBC_OK) st = bind_text(err, s, ins, 2, src_path);
      if (st == KBC_OK) st = bind_text(err, s, ins, 3, dst_paths[i]);
      if (st == KBC_OK) {
        int step = sqlite3_step(ins);
        if (step != SQLITE_DONE)
          st = sql_fail(err, s, "insert edge", step);
        (void)sqlite3_reset(ins);
      }
    }
    kbc_status fin2 = finalize(err, s, ins, st);
    if (st == KBC_OK) st = fin2;
  }

  if (st == KBC_OK) st = exec_plain(err, s, "COMMIT;");
  if (st != KBC_OK) rollback(s);
  unlock(s);
  return st;
}

kbc_status kbc_store_edge_degrees_for(kbc_store *s, const char *corpus,
                                      const char *const *paths, size_t n,
                                      uint32_t *in_deg, kbc_err *err) {
  if (s == NULL || (n > 0 && (paths == NULL || in_deg == NULL)))
    return kbc_err_set(err, KBC_ERR_INVALID, "edge_degrees_for: null argument");
  kbc_status st = require_text(err, "edge corpus", corpus, 255);
  if (st != KBC_OK) return st;
  if (n == 0) return KBC_OK;
  for (size_t i = 0; i < n; i++) {
    if (paths[i] == NULL)
      return kbc_err_set(err, KBC_ERR_INVALID, "edge_degrees_for: path %zu is "
                         "NULL", i);
    in_deg[i] = 0;
  }

  /* The documents are named by path because the store cannot know an index
   * doc id, and they travel as a JSON array rather than as n bound parameters
   * because n is the size of a corpus, not of a page: a VALUES list would cap
   * the caller at SQLITE_MAX_VARIABLE_NUMBER rows per query. */
  kbc_str js;
  kbc_str_init(&js);
  st = kbc_str_putc(&js, '[');
  for (size_t i = 0; st == KBC_OK && i < n; i++) {
    if (i > 0) st = kbc_str_putc(&js, ',');
    if (st == KBC_OK)
      st = kbc_str_append_json_string(&js, paths[i], strlen(paths[i]));
  }
  if (st == KBC_OK) st = kbc_str_putc(&js, ']');
  if (st != KBC_OK) {
    kbc_str_free(&js);
    return kbc_err_set(err, st, "edge_degrees_for: %zu paths", n);
  }

  lock(s);
  /* ONE statement for the whole batch: a correlated aggregate per requested
   * path, so a document with no backlinks and a document that does not exist
   * both come back as 0 and the caller never has to tell them apart. */
  sqlite3_stmt *q = NULL;
  st = prepare(err, s,
               "SELECT (SELECT COUNT(*) FROM edges e"
               "         WHERE e.corpus = ?1 AND e.dst_path = j.value)"
               "  FROM json_each(?2) j;",
               &q);
  if (st == KBC_OK) st = bind_text(err, s, q, 1, corpus);
  if (st == KBC_OK) {
    int rc = sqlite3_bind_text(q, 2, js.ptr, (int)js.len, SQLITE_TRANSIENT);
    if (rc != SQLITE_OK) st = sql_fail(err, s, "bind paths", rc);
  }
  size_t i = 0;
  while (st == KBC_OK) {
    int step = sqlite3_step(q);
    if (step == SQLITE_DONE) break;
    if (step != SQLITE_ROW) {
      st = sql_fail(err, s, "edge degrees: step", step);
      break;
    }
    /* json_each yields exactly the caller's rows, in array order, duplicates
     * included, so slot i is the degree of paths[i]. */
    if (i < n) in_deg[i] = (uint32_t)sqlite3_column_int64(q, 0);
    i++;
  }
  kbc_status fin = finalize(err, s, q, st);
  if (st == KBC_OK) st = fin;
  unlock(s);
  kbc_str_free(&js);
  return st;
}

int64_t kbc_store_edge_count(kbc_store *s, const char *corpus, kbc_err *err) {
  if (s == NULL)
    return kbc_err_set(err, KBC_ERR_INVALID, "edge_count: null store");
  kbc_status st = require_text(err, "edge corpus", corpus, 255);
  if (st != KBC_OK) return st;
  int64_t out = 0;
  lock(s);
  st = count_query(err, s,
                  "SELECT COUNT(*) FROM edges WHERE corpus = ?1;", corpus,
                  &out);
  unlock(s);
  return kbc_failed(st) ? st : out;
}
/* --------------------------------------------------------------- chunks -- */

kbc_status kbc_store_replace_chunks(kbc_store *s, const kbc_chunk_in *chunks,
                                    size_t n, kbc_err *err) {
  if (s == NULL)
    return kbc_err_set(err, KBC_ERR_INVALID, "replace_chunks: null store");
  if (n > 0 && chunks == NULL)
    return kbc_err_set(err, KBC_ERR_INVALID, "replace_chunks: null chunks");
  for (size_t i = 0; i < n; i++) {
    if (chunks[i].doc_id == NULL || chunks[i].text == NULL)
      return kbc_err_set(err, KBC_ERR_INVALID,
                         "chunk %zu: null doc_id or text", i);
    if (chunks[i].text_len > (size_t)INT32_MAX)
      return kbc_err_set(err, KBC_ERR_INVALID, "chunk %zu: %zu bytes", i,
                         chunks[i].text_len);
  }

  lock(s);
  kbc_status rc = exec_plain(err, s, "BEGIN IMMEDIATE;");
  if (rc != KBC_OK) {
    unlock(s);
    return rc;
  }

  /* Replace, not merge: every doc_id in the batch loses its old chunks, so a
   * document that shrank cannot leave stale ord rows behind. Consecutive
   * rows of the same doc are deleted once each. */
  sqlite3_stmt *del = NULL;
  rc = prepare(err, s, "DELETE FROM chunks WHERE doc_id = ?1;", &del);
  for (size_t i = 0; rc == KBC_OK && i < n; i++) {
    if (i == 0) {
      rc = bind_text(err, s, del, 1, chunks[i].doc_id);
      continue;
    }
    if (strcmp(chunks[i].doc_id, chunks[i - 1].doc_id) != 0) {
      int step = sqlite3_step(del);
      if (step != SQLITE_DONE) {
        rc = sql_fail(err, s, "delete chunks", step);
        break;
      }
      (void)sqlite3_reset(del);
      rc = bind_text(err, s, del, 1, chunks[i].doc_id);
    }
  }
  if (rc == KBC_OK && n > 0) {
    int step = sqlite3_step(del);
    if (step != SQLITE_DONE) rc = sql_fail(err, s, "delete chunks", step);
  }
  kbc_status fin = finalize(err, s, del, rc);
  if (rc == KBC_OK) rc = fin;

  sqlite3_stmt *ins = NULL;
  if (rc == KBC_OK)
    rc = prepare(err, s,
                 "INSERT INTO chunks(doc_id, ord, text) VALUES(?1,?2,?3);",
                 &ins);
  for (size_t i = 0; rc == KBC_OK && i < n; i++) {
    rc = bind_text(err, s, ins, 1, chunks[i].doc_id);
    if (rc == KBC_OK) rc = bind_i64(err, s, ins, 2, (int64_t)chunks[i].ord);
    if (rc == KBC_OK) {
      int b = sqlite3_bind_text(ins, 3, chunks[i].text,
                                (int)chunks[i].text_len, SQLITE_TRANSIENT);
      if (b != SQLITE_OK) rc = sql_fail(err, s, "bind chunk text", b);
    }
    if (rc == KBC_OK) {
      int step = sqlite3_step(ins);
      if (step == SQLITE_CONSTRAINT)
        rc = kbc_err_set(err, KBC_ERR_CONFLICT, "chunk %s/%u: duplicate ord",
                         chunks[i].doc_id, chunks[i].ord);
      else if (step != SQLITE_DONE)
        rc = sql_fail(err, s, "insert chunk", step);
      (void)sqlite3_reset(ins);
    }
  }
  kbc_status fin2 = finalize(err, s, ins, rc);
  if (rc == KBC_OK) rc = fin2;

  if (rc == KBC_OK) rc = exec_plain(err, s, "COMMIT;");
  if (rc != KBC_OK) rollback(s);
  unlock(s);
  return rc;
}

kbc_status kbc_store_list_chunks(kbc_store *s, kbc_arena *a, const char *doc_id,
                                 kbc_block *out, size_t *n_out,
                                 kbc_err *err) {
  if (s == NULL || a == NULL || out == NULL || n_out == NULL)
    return kbc_err_set(err, KBC_ERR_INVALID, "list_chunks: null argument");
  kbc_status st = require_text(err, "doc_id", doc_id, KBC_MAX_ID_LEN);
  if (st != KBC_OK) return st;

  *n_out = 0;
  lock(s);
  /* Count first, then fetch: the arena block is sized exactly once and the
   * two reads happen under the same lock, so they cannot disagree. */
  int64_t n64 = 0;
  st = count_query(err, s, "SELECT COUNT(*) FROM chunks WHERE doc_id = ?1;",
                   doc_id, &n64);
  if (st != KBC_OK) {
    unlock(s);
    return st;
  }
  if (n64 <= 0) {
    unlock(s);
    return KBC_OK;
  }
  size_t n = (size_t)n64;
  kbc_block *blocks = kbc_arena_alloc(a, n * sizeof(*blocks));
  if (blocks == NULL) {
    unlock(s);
    return kbc_err_set(err, KBC_ERR_NOMEM, "list chunks: %zu blocks", n);
  }
  memset(blocks, 0, n * sizeof(*blocks));

  sqlite3_stmt *stq = NULL;
  st = prepare(err, s,
               "SELECT ord, text FROM chunks WHERE doc_id = ?1 ORDER BY ord;",
               &stq);
  if (st == KBC_OK) st = bind_text(err, s, stq, 1, doc_id);
  if (st != KBC_OK) {
    (void)finalize(err, s, stq, st);
    unlock(s);
    return st;
  }
  size_t i = 0;
  for (;;) {
    int step = sqlite3_step(stq);
    if (step == SQLITE_DONE) break;
    if (step != SQLITE_ROW) {
      st = sql_fail(err, s, "list chunks: step", step);
      break;
    }
    if (i >= n) {
      st = kbc_err_set(err, KBC_ERR_INTERNAL, "list chunks: row overflow");
      break;
    }
    const char *txt = (const char *)sqlite3_column_text(stq, 1);
    int len = sqlite3_column_bytes(stq, 1);
    if (len < 0) len = 0;
    /* Chunks carry no slug column; the anchor is derived from the ord so a
     * comment can still address a chunk positionally. */
    blocks[i].id = kbc_arena_printf(a, "b%d", sqlite3_column_int(stq, 0));
    blocks[i].text = kbc_arena_strndup(a, txt ? txt : "", (size_t)len);
    blocks[i].text_len = (size_t)len;
    blocks[i].heading_level = 0;
    blocks[i].offset = 0;
    i++;
  }
  kbc_status fin = finalize(err, s, stq, st);
  if (st == KBC_OK) st = fin;
  if (st == KBC_OK && i != n)
    st = kbc_err_set(err, KBC_ERR_INTERNAL, "list chunks: %zu of %zu rows", i,
                     n);
  unlock(s);
  if (st != KBC_OK) return st;

  memcpy(out, blocks, n * sizeof(*blocks));
  *n_out = n;
  return KBC_OK;
}

/* ------------------------------------------------------------- comments -- */

/* 12 lowercase hex, so kbc_id_is_valid accepts a minted comment id. */
static void mint_id(char out[KBC_MAX_ID_LEN + 1]) {
  static atomic_ullong counter;
  uint64_t v = 0;
  FILE *f = fopen("/dev/urandom", "rb");
  if (f != NULL) {
    size_t got = fread(&v, 1, sizeof(v), f);
    (void)fclose(f);
    if (got != sizeof(v)) v = 0;
  }
  if (v == 0) {
    /* Fallback, not a security property: ids only need to be unique. */
    v = (uint64_t)kbc_now_ns() * 0x9E3779B97F4A7C15ull;
    v ^= atomic_fetch_add(&counter, 1) * 0xBF58476D1CE4E5B9ull;
  }
  static const char hex[] = "0123456789abcdef";
  for (unsigned i = 0; i < (unsigned)KBC_MAX_ID_LEN; i++) {
    out[i] = hex[v & 0xF];
    v >>= 4;
  }
  out[KBC_MAX_ID_LEN] = '\0';
}

kbc_status kbc_store_add_comment(kbc_store *s, const char *doc_id,
                                 const char *anchor, const char *author,
                                 const char *body, kbc_err *err) {
  if (s == NULL)
    return kbc_err_set(err, KBC_ERR_INVALID, "add_comment: null store");
  kbc_status st = require_text(err, "doc_id", doc_id, KBC_MAX_ID_LEN);
  if (st == KBC_OK)
    st = require_text(err, "anchor", anchor, KBC_MAX_ID_LEN * 4);
  if (st == KBC_OK) st = require_text(err, "author", author, 255);
  if (st == KBC_OK) st = require_text(err, "body", body, KBC_MAX_ARTIFACT_BYTES);
  if (st != KBC_OK) return st;

  char id[KBC_MAX_ID_LEN + 1];
  mint_id(id);
  char now[32];
  (void)kbc_now_iso8601(now, sizeof(now));

  lock(s);
  sqlite3_stmt *ins = NULL;
  st = prepare(err, s,
               "INSERT INTO comments(id, doc_id, anchor, author, body,"
               " created_at, resolved) VALUES(?1,?2,?3,?4,?5,?6,0);",
               &ins);
  if (st == KBC_OK) st = bind_text(err, s, ins, 1, id);
  if (st == KBC_OK) st = bind_text(err, s, ins, 2, doc_id);
  if (st == KBC_OK) st = bind_text(err, s, ins, 3, anchor);
  if (st == KBC_OK) st = bind_text(err, s, ins, 4, author);
  if (st == KBC_OK) st = bind_text(err, s, ins, 5, body);
  if (st == KBC_OK) st = bind_text(err, s, ins, 6, now);
  if (st == KBC_OK) {
    int step = sqlite3_step(ins);
    if (step == SQLITE_CONSTRAINT)
      st = kbc_err_set(err, KBC_ERR_CONFLICT,
                       "comment %s on %s: no such artifact or duplicate id",
                       id, doc_id);
    else if (step != SQLITE_DONE)
      st = sql_fail(err, s, "add comment", step);
  }
  kbc_status fin = finalize(err, s, ins, st);
  if (st == KBC_OK) st = fin;
  unlock(s);
  return st;
}

kbc_status kbc_store_list_comments(kbc_store *s, kbc_arena *a,
                                   const char *doc_id, size_t limit,
                                   kbc_comment **out, size_t *n_out,
                                   kbc_err *err) {
  if (s == NULL || a == NULL || out == NULL || n_out == NULL)
    return kbc_err_set(err, KBC_ERR_INVALID, "list_comments: null argument");
  kbc_status st = require_text(err, "doc_id", doc_id, KBC_MAX_ID_LEN);
  if (st != KBC_OK) return st;
  if (limit > KBC_MAX_HITS) limit = KBC_MAX_HITS;

  *out = NULL;
  *n_out = 0;
  if (limit == 0) return KBC_OK;

  lock(s);
  int64_t n64 = 0;
  st = count_query(err, s, "SELECT COUNT(*) FROM comments WHERE doc_id = ?1;",
                   doc_id, &n64);
  if (st != KBC_OK) {
    unlock(s);
    return st;
  }
  size_t n = (n64 > (int64_t)limit) ? limit : (size_t)(n64 > 0 ? n64 : 0);
  if (n == 0) {
    unlock(s);
    return KBC_OK;
  }

  kbc_comment *arr = kbc_arena_alloc(a, n * sizeof(*arr));
  if (arr == NULL) {
    unlock(s);
    return kbc_err_set(err, KBC_ERR_NOMEM, "list comments: %zu rows", n);
  }
  memset(arr, 0, n * sizeof(*arr));

  sqlite3_stmt *stq = NULL;
  st = prepare(err, s,
               "SELECT id, doc_id, anchor, author, body, created_at, resolved"
               " FROM comments WHERE doc_id = ?1 ORDER BY created_at, id"
               " LIMIT ?2;",
               &stq);
  if (st == KBC_OK) st = bind_text(err, s, stq, 1, doc_id);
  if (st == KBC_OK) st = bind_i64(err, s, stq, 2, (int64_t)n);
  if (st != KBC_OK) {
    (void)finalize(err, s, stq, st);
    unlock(s);
    return st;
  }
  size_t i = 0;
  for (;;) {
    int step = sqlite3_step(stq);
    if (step == SQLITE_DONE) break;
    if (step != SQLITE_ROW) {
      st = sql_fail(err, s, "list comments: step", step);
      break;
    }
    if (i >= n) {
      st = kbc_err_set(err, KBC_ERR_INTERNAL, "list comments: row overflow");
      break;
    }
    arr[i].id = col_str(a, stq, 0);
    arr[i].doc_id = col_str(a, stq, 1);
    arr[i].anchor = col_str(a, stq, 2);
    arr[i].author = col_str(a, stq, 3);
    arr[i].body = col_str(a, stq, 4);
    arr[i].created_at = col_str(a, stq, 5);
    arr[i].resolved = sqlite3_column_int(stq, 6) != 0;
    i++;
  }
  kbc_status fin = finalize(err, s, stq, st);
  if (st == KBC_OK) st = fin;
  unlock(s);
  if (st != KBC_OK) return st;

  *out = arr;
  *n_out = i;
  return KBC_OK;
}

kbc_status kbc_store_set_comment_resolved(kbc_store *s, const char *comment_id,
                                          bool resolved, kbc_err *err) {
  if (s == NULL)
    return kbc_err_set(err, KBC_ERR_INVALID,
                       "set_comment_resolved: null store");
  kbc_status st = require_text(err, "comment id", comment_id, KBC_MAX_ID_LEN);
  if (st != KBC_OK) return st;

  lock(s);
  sqlite3_stmt *up = NULL;
  st = prepare(err, s, "UPDATE comments SET resolved = ?1 WHERE id = ?2;",
               &up);
  if (st == KBC_OK) st = bind_i64(err, s, up, 1, resolved ? 1 : 0);
  if (st == KBC_OK) st = bind_text(err, s, up, 2, comment_id);
  if (st == KBC_OK) {
    int step = sqlite3_step(up);
    if (step != SQLITE_DONE) st = sql_fail(err, s, "resolve comment", step);
  }
  kbc_status fin = finalize(err, s, up, st);
  if (st == KBC_OK) st = fin;
  if (st == KBC_OK && sqlite3_changes(s->db) == 0)
    st = kbc_err_set(err, KBC_ERR_NOTFOUND, "comment %s: not found", comment_id);
  unlock(s);
  return st;
}
