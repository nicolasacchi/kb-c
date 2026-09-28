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
 * chosen rather than bound are the fixed filter clauses of the four variants
 * in kbc_store_list_artifact_ids and the four in kbc_store_list_errors, each
 * a compile-time literal picked by a boolean — never user text.
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

/* v3 — pending links: the link targets a document named that were not an
 * indexed document when the source was written. An edge is owned by its
 * source, so a document that appears AFTER the documents that link to it
 * would otherwise stay at in-degree 0 forever: nothing ever rewrites those
 * sources' edges. This table is the only record that the link exists, and it
 * is what makes the graph independent of the order documents were visited
 * in — kbc_store_drain_pending materialises the edge the moment the target
 * lands. Path-keyed like edges, for the same reason. */
static const char *const SCHEMA_V3 =
    "CREATE TABLE IF NOT EXISTS pending_links ("
    " corpus TEXT NOT NULL,"
    " src_path TEXT NOT NULL,"
    " dst_path TEXT NOT NULL,"
    " PRIMARY KEY(corpus, src_path, dst_path));"
    "CREATE INDEX IF NOT EXISTS pending_links_dst"
    " ON pending_links(corpus, dst_path);";

/* v4 — per-document metadata, the facets the query overlay filters on. One
 * row per (corpus, path, key, value), and one entry per VALUE of a
 * multi-valued key, so `kb-tags="search, index"` is two rows and matching
 * one tag is an index probe rather than a string scan. The (corpus, key,
 * value) index is the query the overlay actually runs; (corpus, path) is
 * what makes replace-on-ingest a single DELETE.
 *
 * Path-keyed like edges and pending_links, for the same reason: a document's
 * id is minted from (corpus, path) but a facet is a name in a source file,
 * and a re-ingest must be able to REPLACE the document's facets without
 * knowing anything else about it. */
static const char *const SCHEMA_V4 =
    "CREATE TABLE IF NOT EXISTS doc_metas ("
    " corpus TEXT NOT NULL,"
    " path TEXT NOT NULL,"
    " key TEXT NOT NULL,"
    " value TEXT NOT NULL,"
    " PRIMARY KEY(corpus, path, key, value));"
    "CREATE INDEX IF NOT EXISTS doc_metas_kv"
    " ON doc_metas(corpus, key, value);";

/* v5..v10 — the rest of the Rust original's per-kb schema that kb-c claims.
 * The DDL is the Rust DDL (kb-core/migrations/), unchanged except for the one
 * rename kb-c already makes: `source_slug` is `corpus` everywhere, because
 * kb-c calls a source root a corpus. A Rust migration that only ALTERed a
 * table is folded into the CREATE that introduces it — these tables have
 * never existed in any kb-c schema, so there is nothing to ALTER — and the
 * indexes are the FINAL upstream form, because an index is only useful if it
 * matches the query it serves.
 *
 * v5 — sources, index runs, ingest errors (V0001). */
static const char *const SCHEMA_V5 =
    "CREATE TABLE IF NOT EXISTS sources ("
    " slug TEXT PRIMARY KEY,"
    " path TEXT NOT NULL UNIQUE,"
    " added_at INTEGER NOT NULL,"
    " paused INTEGER NOT NULL DEFAULT 0);"
    "CREATE TABLE IF NOT EXISTS index_runs ("
    " id TEXT PRIMARY KEY,"
    " corpus TEXT NOT NULL REFERENCES sources(slug),"
    " started_at INTEGER NOT NULL,"
    " finished_at INTEGER,"
    " ok_count INTEGER NOT NULL DEFAULT 0,"
    " err_count INTEGER NOT NULL DEFAULT 0);"
    "CREATE INDEX IF NOT EXISTS idx_runs_source ON index_runs(corpus);"
    "CREATE INDEX IF NOT EXISTS idx_runs_started ON index_runs(started_at);"
    "CREATE TABLE IF NOT EXISTS errors ("
    " id TEXT PRIMARY KEY,"
    " kind TEXT NOT NULL,"
    " corpus TEXT NOT NULL,"
    " path TEXT NOT NULL,"
    " message TEXT NOT NULL,"
    " content_hash TEXT,"
    " retry_count INTEGER NOT NULL DEFAULT 0,"
    " created_at INTEGER NOT NULL,"
    " dismissed INTEGER NOT NULL DEFAULT 0);"
    "CREATE INDEX IF NOT EXISTS idx_errors_source ON errors(corpus);"
    "CREATE INDEX IF NOT EXISTS idx_errors_path ON errors(path);"
    /* PARTIAL, and deliberately so: dismissed = 0 is the only predicate the
     * open-errors surface ever asks for, so a full index would carry every
     * resolved row forever to answer a query that never wants it. */
    "CREATE INDEX IF NOT EXISTS idx_errors_open"
    " ON errors(dismissed) WHERE dismissed = 0;";

/* v6 — the reading/search/comment history (V0003 + V0007 + V0012 + V0014 +
 * V0022 + V0034). One polymorphic table so a timeline is one ORDER BY rather
 * than a UNION of three; the CHECK is what keeps "one of three kinds" an
 * invariant of the table rather than of every caller.
 *
 * Both later indexes are PARTIAL for the same reason the errors one is: each
 * exists for exactly one query, and restricting the index to the rows that
 * query can match is what turns a full scan into a seek. */
static const char *const SCHEMA_V6 =
    "CREATE TABLE IF NOT EXISTS history ("
    " id INTEGER PRIMARY KEY,"
    " kind TEXT NOT NULL CHECK (kind IN ('open','search','comment')),"
    " artifact_id TEXT,"
    " query TEXT,"
    " comment_id TEXT,"
    " scroll_y INTEGER NOT NULL DEFAULT 0,"
    " scroll_max INTEGER NOT NULL DEFAULT 0,"
    " started_at INTEGER NOT NULL,"
    " updated_at INTEGER NOT NULL,"
    " scroll_y_max INTEGER NOT NULL DEFAULT 0,"
    " active_ms INTEGER NOT NULL DEFAULT 0,"
    " last_section TEXT,"
    " source TEXT,"
    " user TEXT NOT NULL DEFAULT '');"
    "CREATE INDEX IF NOT EXISTS idx_history_started"
    " ON history(started_at DESC);"
    "CREATE INDEX IF NOT EXISTS idx_history_artifact_open"
    " ON history(artifact_id, started_at DESC)"
    " WHERE artifact_id IS NOT NULL AND kind = 'open';"
    "CREATE INDEX IF NOT EXISTS idx_history_search"
    " ON history(query, started_at DESC) WHERE kind = 'search';";

/* v7 — the corkboard, the artifacts a user has anchored (V0005). */
static const char *const SCHEMA_V7 =
    "CREATE TABLE IF NOT EXISTS corkboard ("
    " artifact_id TEXT PRIMARY KEY,"
    " created_at INTEGER NOT NULL);"
    "CREATE INDEX IF NOT EXISTS idx_corkboard_created"
    " ON corkboard(created_at DESC);";

/* v8 — pinned memories, which bypass the recall decay floor (V0006). Same
 * shape as the corkboard and for the same reason. */
static const char *const SCHEMA_V8 =
    "CREATE TABLE IF NOT EXISTS pinned_memories ("
    " artifact_id TEXT PRIMARY KEY,"
    " pinned_at INTEGER NOT NULL);"
    "CREATE INDEX IF NOT EXISTS idx_pinned_memories_at"
    " ON pinned_memories(pinned_at DESC);";

/* v9 — per-file exclusion, durable operator intent keyed by the
 * source-relative path (V0023). */
static const char *const SCHEMA_V9 =
    "CREATE TABLE IF NOT EXISTS excluded_files ("
    " path TEXT PRIMARY KEY,"
    " excluded_at INTEGER NOT NULL,"
    " note TEXT);";

/* v10 — the stable first-indexed anchor per artifact (V0033). No index: the
 * only query it serves is a primary-key probe, and it outlives the artifact
 * (there is no foreign key, on purpose). */
static const char *const SCHEMA_V10 =
    "CREATE TABLE IF NOT EXISTS doc_first_seen ("
    " artifact_id TEXT PRIMARY KEY,"
    " first_indexed_unix INTEGER NOT NULL);";

/* The ladder, in the shape refinery's Runner has it: an ordered list of
 * (version, sql), applied FORWARD-ONLY, one transaction per version. Rust
 * reads its binary epoch from the runner rather than from a second constant
 * so the two can never drift (sibling.rs:79); BINARY_EPOCH is that read,
 * and there is deliberately no SCHEMA_VERSION constant to fall out of step
 * with the last entry. */
typedef struct {
  int version;
  const char *sql;
} kbc_migration;

static const kbc_migration MIGRATIONS[] = {
    {1, SCHEMA_V1},   {2, SCHEMA_V2},   {3, SCHEMA_V3},   {4, SCHEMA_V4},
    {5, SCHEMA_V5},   {6, SCHEMA_V6},   {7, SCHEMA_V7},   {8, SCHEMA_V8},
    {9, SCHEMA_V9},   {10, SCHEMA_V10},
};

#define MIGRATION_COUNT (sizeof(MIGRATIONS) / sizeof(MIGRATIONS[0]))
#define BINARY_EPOCH MIGRATIONS[MIGRATION_COUNT - 1].version


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
static kbc_status volume_epoch(kbc_err *err, const kbc_store *s, int64_t *out);
static kbc_status refuse_if_volume_ahead(kbc_err *err, const char *db_path,
                                         int64_t volume);
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

  /* busy_timeout first, and alone: it is a connection setting, not a write to
   * the file, and without it the guard's read below would fail outright on a
   * volume another process is opening at the same moment. */
  if (exec_plain(err, s, "PRAGMA busy_timeout = 5000;") != KBC_OK) {
    kbc_store_close(s);
    return NULL;
  }

  /* Before the pragmas that touch the file, before anything at all: a volume
   * this binary cannot read must not be written to even by a journal-mode
   * change. The read is a plain SELECT against a table name that is a literal
   * here, so there is nothing user-influenced in it. */
  int64_t volume = 0;
  if (volume_epoch(err, s, &volume) != KBC_OK) {
    kbc_store_close(s);
    return NULL;
  }
  if (refuse_if_volume_ahead(err, cfg->db_path, volume) != KBC_OK) {
    kbc_store_close(s);
    return NULL;
  }

  /* WAL: readers never block the writer. NORMAL: a crash may lose the last
   * transaction but never the database. */
  const char *const pragmas[] = {
      "PRAGMA journal_mode = WAL;", "PRAGMA synchronous = NORMAL;",
      "PRAGMA foreign_keys = ON;",  "PRAGMA temp_store = MEMORY;",
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

/* The version a volume has reached, or 0 for a volume that has never been
 * migrated — a brand-new file, or one whose bookkeeping table does not exist
 * yet. That case is Rust's `None` (sibling.rs:95) and is never a refusal, so
 * it is reported as 0 rather than as a state of its own. */
static kbc_status volume_epoch(kbc_err *err, const kbc_store *s,
                               int64_t *out) {
  int64_t exists = 0;
  kbc_status st = count_query(
      err, s,
      "SELECT COUNT(*) FROM sqlite_master"
      " WHERE type = 'table' AND name = 'schema_version';",
      NULL, &exists);
  if (st != KBC_OK) return st;
  if (exists == 0) {
    *out = 0;
    return KBC_OK;
  }
  return count_query(err, s,
                     "SELECT IFNULL(MAX(version), 0) FROM schema_version;", NULL,
                     out);
}

/* The boot guard, ported from Rust's refuse_if_volume_ahead
 * (kb-core/src/sibling.rs:119). The condition is `volume > binary` and
 * nothing else: a volume at or behind the binary is what every ordinary open
 * looks like, and only a volume AHEAD has been forward-migrated by a newer
 * binary that this one cannot read. The direction is written out here rather
 * than folded into the caller's comparison because getting it backwards
 * refuses every valid volume.
 *
 * A hard error naming both epochs, never a warning: refinery only ever
 * migrates forward, so the alternative is a green health check on a daemon
 * that fails on the first query touching a column it has never heard of —
 * the 13.5 h outage that made Rust write this. Both epochs lead the message
 * because kbc_err's buffer is 256 bytes and a long db_path would otherwise
 * truncate them off the end. */
static kbc_status refuse_if_volume_ahead(kbc_err *err, const char *db_path,
                                         int64_t volume) {
  if (volume <= (int64_t)BINARY_EPOCH) return KBC_OK;
  return kbc_err_set(err, KBC_ERR_CONFLICT,
                     "refusing to open %s: schema epoch V%lld on disk is NEWER "
                     "than this binary's V%d - deploy a binary >= epoch V%lld "
                     "or restore the backup matching epoch V%d",
                     db_path, (long long)volume, BINARY_EPOCH,
                     (long long)volume, BINARY_EPOCH);
}

/* Records one version INSIDE the transaction that applies it, so the
 * recorded version can never claim a step that did not commit. */
static kbc_status record_version(kbc_err *err, const kbc_store *s,
                                 int version) {
  sqlite3_stmt *ins = NULL;
  kbc_status st = prepare(err, s,
                           "INSERT OR IGNORE INTO schema_version(version) "
                           "VALUES (?1);",
                           &ins);
  if (st == KBC_OK) st = bind_i64(err, s, ins, 1, version);
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
  st = volume_epoch(err, s, &have);
  if (st != KBC_OK) return st;
  if (have >= (int64_t)BINARY_EPOCH) {
    atomic_store(s->version, (int)have);
    return KBC_OK;
  }

  /* ONE TRANSACTION PER VERSION. That granularity IS the property: a version
   * that fails leaves the recorded version at the last one that fully
   * committed, so the next open re-runs exactly the version that failed and
   * none of the ones before it. Every step is written to be re-runnable (IF
   * NOT EXISTS / OR IGNORE), so a database that already carries a step only
   * pays for the ones after it. */
  for (size_t i = 0; i < MIGRATION_COUNT; i++) {
    if ((int64_t)MIGRATIONS[i].version <= have) continue;
    st = exec_plain(err, s, "BEGIN IMMEDIATE;");
    if (st == KBC_OK) st = exec_plain(err, s, MIGRATIONS[i].sql);
    if (st == KBC_OK) st = record_version(err, s, MIGRATIONS[i].version);
    if (st == KBC_OK) st = exec_plain(err, s, "COMMIT;");
    if (st != KBC_OK) {
      rollback(s);
      return st;
    }
  }
  atomic_store(s->version, BINARY_EPOCH);
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

/* ------------------------------------------------------------- pending --- */

/* Records the link targets a document named that were not indexed documents.
 * OR IGNORE, in ONE transaction: a target named twice collapses on the
 * primary key, and a failure leaves the previous pending set rather than half
 * of a new one. */
kbc_status kbc_store_add_pending_links(kbc_store *s, const char *corpus,
                                       const char *src_path,
                                       const char *const *dst_paths, size_t n,
                                       kbc_err *err) {
  if (s == NULL)
    return kbc_err_set(err, KBC_ERR_INVALID, "add_pending_links: null store");
  if (n > 0 && dst_paths == NULL)
    return kbc_err_set(err, KBC_ERR_INVALID, "add_pending_links: null "
                       "dst_paths");
  kbc_status st = require_text(err, "pending corpus", corpus, 255);
  if (st == KBC_OK)
    st = require_text(err, "pending src_path", src_path, KBC_MAX_PATH_LEN);
  for (size_t i = 0; st == KBC_OK && i < n; i++) {
    st = require_text(err, "pending dst_path", dst_paths[i], KBC_MAX_PATH_LEN);
  }
  if (st != KBC_OK) return st;
  if (n == 0) return KBC_OK;

  lock(s);
  st = exec_plain(err, s, "BEGIN IMMEDIATE;");
  if (st == KBC_OK) {
    sqlite3_stmt *ins = NULL;
    st = prepare(err, s,
                 "INSERT OR IGNORE INTO pending_links(corpus, src_path, "
                 "dst_path) VALUES(?1,?2,?3);",
                 &ins);
    for (size_t i = 0; st == KBC_OK && i < n; i++) {
      st = bind_text(err, s, ins, 1, corpus);
      if (st == KBC_OK) st = bind_text(err, s, ins, 2, src_path);
      if (st == KBC_OK) st = bind_text(err, s, ins, 3, dst_paths[i]);
      if (st == KBC_OK) {
        int step = sqlite3_step(ins);
        if (step != SQLITE_DONE)
          st = sql_fail(err, s, "insert pending link", step);
        (void)sqlite3_reset(ins);
      }
    }
    kbc_status fin = finalize(err, s, ins, st);
    if (st == KBC_OK) st = fin;
  }
  if (st == KBC_OK) st = exec_plain(err, s, "COMMIT;");
  if (st != KBC_OK) rollback(s);
  unlock(s);
  return st;
}

/* Removes a document from the graph, as a removal must.
 *
 * A document that goes takes its OUTBOUND edges and its outbound pending links
 * with it: it is no longer a document, so it can neither make a link nor be
 * the source of one. Its INBOUND edges go too — an edge to a path with no
 * document is not an edge — but each one is demoted to a pending link first,
 * because the link the source wrote is still true and the target may come back.
 * Dropping those rows instead is how a corpus that deletes and re-adds a hub
 * silently loses every backlink to it.
 *
 * Pending links POINTING at the path are left alone: they are the record of a
 * link waiting for a target that has not arrived, and this document is a
 * target that has just left. The next drain finds nothing to do and the next
 * arrival of the document materialises them.
 *
 * ONE transaction: a half-forgotten document would leave the graph claiming a
 * link that nobody wrote, or dropping one that somebody did. */
kbc_status kbc_store_forget_document(kbc_store *s, const char *corpus,
                                      const char *path, kbc_err *err) {
  if (s == NULL)
    return kbc_err_set(err, KBC_ERR_INVALID, "forget_document: null store");
  kbc_status st = require_text(err, "pending corpus", corpus, 255);
  if (st == KBC_OK)
    st = require_text(err, "document path", path, KBC_MAX_PATH_LEN);
  if (st != KBC_OK) return st;

  lock(s);
  st = exec_plain(err, s, "BEGIN IMMEDIATE;");
  if (st == KBC_OK) {
    static const char *const SQLS[] = {
        /* The demotion runs first: once the edges are gone there is nothing
         * left to say which of them pointed here. */
        "INSERT OR IGNORE INTO pending_links(corpus, src_path, dst_path) "
        "SELECT corpus, src_path, dst_path FROM edges"
        " WHERE corpus = ?1 AND dst_path = ?2;",
        "DELETE FROM edges WHERE corpus = ?1 AND (src_path = ?2 OR dst_path = "
        "?2);",
        "DELETE FROM pending_links WHERE corpus = ?1 AND src_path = ?2;",
        /* A facet is a name the document wrote into itself, so a document
         * that is gone has no facets: leaving them would keep it matching
         * `tag:` after its bytes were deleted. */
        "DELETE FROM doc_metas WHERE corpus = ?1 AND path = ?2;",
    };
    for (size_t i = 0; st == KBC_OK && i < sizeof(SQLS) / sizeof(SQLS[0]);
         i++) {
      sqlite3_stmt *q = NULL;
      st = prepare(err, s, SQLS[i], &q);
      if (st == KBC_OK) st = bind_text(err, s, q, 1, corpus);
      if (st == KBC_OK) st = bind_text(err, s, q, 2, path);
      if (st == KBC_OK) {
        int step = sqlite3_step(q);
        if (step != SQLITE_DONE)
          st = sql_fail(err, s, "forget document: step", step);
      }
      kbc_status fin = finalize(err, s, q, st);
      if (st == KBC_OK) st = fin;
    }
  }
  if (st == KBC_OK) st = exec_plain(err, s, "COMMIT;");
  if (st != KBC_OK) rollback(s);
  unlock(s);
  return st;
}


/* These three are declared in include/kbc/store.h, beside the edge contract.
 * Declared here on the same terms as the header would carry so -Wmissing-
 * prototypes stays green and app.c can reach them:
 *   kbc_status kbc_store_replace_metas(kbc_store *s, const char *corpus,
 *                                      const char *path,
 *                                      const char *const *keys,
 *                                      const char *const *values, size_t n,
 *                                      kbc_err *err);
 *   kbc_status kbc_store_forget_metas(kbc_store *s, const char *corpus,
 *                                     const char *path, kbc_err *err);
 *   kbc_status kbc_store_docs_with_meta(kbc_store *s, const char *corpus,
 *                                       const char *key, const char *value,
 *                                       char ***paths_out, size_t *n_out,
 *                                       kbc_err *err); */
kbc_status kbc_store_replace_metas(kbc_store *s, const char *corpus,
                                   const char *path,
                                   const char *const *keys,
                                   const char *const *values, size_t n,
                                   kbc_err *err);
kbc_status kbc_store_forget_metas(kbc_store *s, const char *corpus,
                                  const char *path, kbc_err *err);
kbc_status kbc_store_docs_with_meta(kbc_store *s, const char *corpus,
                                    const char *key, const char *value,
                                    char ***paths_out, size_t *n_out,
                                    kbc_err *err);

/* --------------------------------------------------------------- metas --- */


/* REPLACE, not merge. A document is re-ingested with the facets it declares
 * NOW, so a tag the author removed stops matching on the next reindex
 * instead of living on as a row nothing will ever clean up. ONE transaction,
 * for the reason replace_edges has: a half-applied facet set is a filter
 * matching a document that never existed. */
kbc_status kbc_store_replace_metas(kbc_store *s, const char *corpus,
                                   const char *path,
                                   const char *const *keys,
                                   const char *const *values, size_t n,
                                   kbc_err *err) {
  if (s == NULL)
    return kbc_err_set(err, KBC_ERR_INVALID, "replace_metas: null store");
  if (n > 0 && (keys == NULL || values == NULL))
    return kbc_err_set(err, KBC_ERR_INVALID, "replace_metas: null facet list");
  kbc_status st = require_text(err, "meta corpus", corpus, 255);
  if (st == KBC_OK)
    st = require_text(err, "meta path", path, KBC_MAX_PATH_LEN);
  for (size_t i = 0; st == KBC_OK && i < n; i++) {
    /* A facet key is a bare lowercase name; the value is author text and
     * only has to fit the artifact it came from. */
    if (keys[i] == NULL || keys[i][0] == '\0')
      st = kbc_err_set(err, KBC_ERR_INVALID, "meta key %zu is empty", i);
    else if (strlen(keys[i]) > 64)
      st = kbc_err_set(err, KBC_ERR_INVALID,
                       "meta key \"%s\" is over the 64 byte cap", keys[i]);
    else if (values[i] == NULL)
      st = kbc_err_set(err, KBC_ERR_INVALID, "meta value %zu is NULL", i);
    else if (strlen(values[i]) > KBC_MAX_PATH_LEN)
      st = kbc_err_set(err, KBC_ERR_INVALID,
                       "meta value for \"%s\" is over the %u byte cap",
                       keys[i], (unsigned)KBC_MAX_PATH_LEN);
  }
  if (st != KBC_OK) return st;

  lock(s);
  st = exec_plain(err, s, "BEGIN IMMEDIATE;");
  if (st != KBC_OK) {
    unlock(s);
    return st;
  }
  sqlite3_stmt *del = NULL;
  st = prepare(err, s,
               "DELETE FROM doc_metas WHERE corpus = ?1 AND path = ?2;", &del);
  if (st == KBC_OK) st = bind_text(err, s, del, 1, corpus);
  if (st == KBC_OK) st = bind_text(err, s, del, 2, path);
  if (st == KBC_OK) {
    int step = sqlite3_step(del);
    if (step != SQLITE_DONE) st = sql_fail(err, s, "delete metas: step", step);
  }
  kbc_status fin = finalize(err, s, del, st);
  if (st == KBC_OK) st = fin;

  sqlite3_stmt *ins = NULL;
  if (st == KBC_OK) {
    st = prepare(err, s,
                 "INSERT OR IGNORE INTO doc_metas(corpus, path, key, value) "
                 "VALUES(?1,?2,?3,?4);",
                 &ins);
    for (size_t i = 0; st == KBC_OK && i < n; i++) {
      st = bind_text(err, s, ins, 1, corpus);
      if (st == KBC_OK) st = bind_text(err, s, ins, 2, path);
      if (st == KBC_OK) st = bind_text(err, s, ins, 3, keys[i]);
      if (st == KBC_OK) st = bind_text(err, s, ins, 4, values[i]);
      if (st == KBC_OK) {
        int step = sqlite3_step(ins);
        if (step != SQLITE_DONE) st = sql_fail(err, s, "insert meta", step);
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

kbc_status kbc_store_forget_metas(kbc_store *s, const char *corpus,
                                  const char *path, kbc_err *err) {
  if (s == NULL)
    return kbc_err_set(err, KBC_ERR_INVALID, "forget_metas: null store");
  kbc_status st = require_text(err, "meta corpus", corpus, 255);
  if (st == KBC_OK)
    st = require_text(err, "meta path", path, KBC_MAX_PATH_LEN);
  if (st != KBC_OK) return st;
  lock(s);
  sqlite3_stmt *q = NULL;
  st = prepare(err, s, "DELETE FROM doc_metas WHERE corpus = ?1 AND path = ?2;",
               &q);
  if (st == KBC_OK) st = bind_text(err, s, q, 1, corpus);
  if (st == KBC_OK) st = bind_text(err, s, q, 2, path);
  if (st == KBC_OK) {
    int step = sqlite3_step(q);
    if (step != SQLITE_DONE) st = sql_fail(err, s, "forget metas: step", step);
  }
  kbc_status fin = finalize(err, s, q, st);
  unlock(s);
  return st != KBC_OK ? st : fin;
}

/* The documents carrying (key, value), as KBC_OWN paths. ONE indexed query,
 * and an EMPTY result is zero rows — never "no filter". That is the whole
 * point of the table existing: a filter that matches nothing must say so.
 *
 * `value` NULL asks for the documents carrying `key` with ANY value, which is
 * what a boolean facet (`index`) needs. */
kbc_status kbc_store_docs_with_meta(kbc_store *s, const char *corpus,
                                    const char *key, const char *value,
                                    char ***paths_out, size_t *n_out,
                                    kbc_err *err) {
  if (s == NULL || paths_out == NULL || n_out == NULL)
    return kbc_err_set(err, KBC_ERR_INVALID, "docs_with_meta: null argument");
  *paths_out = NULL;
  *n_out = 0;
  kbc_status st = require_text(err, "meta corpus", corpus, 255);
  if (st == KBC_OK && (key == NULL || key[0] == '\0'))
    st = kbc_err_set(err, KBC_ERR_INVALID, "docs_with_meta: empty meta key");
  if (st != KBC_OK) return st;

  lock(s);
  sqlite3_stmt *q = NULL;
  st = prepare(err, s,
               value != NULL
                   ? "SELECT DISTINCT path FROM doc_metas"
                     " WHERE corpus = ?1 AND key = ?2 AND value = ?3;"
                   : "SELECT DISTINCT path FROM doc_metas"
                     " WHERE corpus = ?1 AND key = ?2;",
               &q);
  if (st == KBC_OK) st = bind_text(err, s, q, 1, corpus);
  if (st == KBC_OK) st = bind_text(err, s, q, 2, key);
  if (st == KBC_OK && value != NULL) st = bind_text(err, s, q, 3, value);
  char **paths = NULL;
  size_t n = 0;
  size_t cap = 0;
  while (st == KBC_OK) {
    const int step = sqlite3_step(q);
    if (step == SQLITE_DONE) break;
    if (step != SQLITE_ROW) {
      st = sql_fail(err, s, "docs_with_meta: step", step);
      break;
    }
    if (n == cap) {
      const size_t ncap = cap == 0 ? 32 : cap * 2;
      char **grown = (char **)realloc(paths, ncap * sizeof(*grown));
      if (grown == NULL) {
        st = kbc_err_set(err, KBC_ERR_NOMEM, "docs_with_meta: %zu paths", ncap);
        break;
      }
      paths = grown;
      cap = ncap;
    }
    const unsigned char *text = sqlite3_column_text(q, 0);
    paths[n] = text != NULL ? strdup((const char *)text) : NULL;
    if (paths[n] == NULL) {
      st = kbc_err_set(err, KBC_ERR_NOMEM, "docs_with_meta: path copy");
      break;
    }
    n++;
  }
  kbc_status fin = finalize(err, s, q, st);
  unlock(s);
  if (st != KBC_OK) {
    for (size_t i = 0; i < n; i++) free(paths[i]);
    free(paths);
    return st != KBC_OK ? st : fin;
  }
  *paths_out = paths;
  *n_out = n;
  return KBC_OK;
}

/* Materialises every pending link pointing AT `dst_path` whose source is still
 * an indexed document, then deletes those pending rows. ONE transaction, and
 * the order is the point: a graph that gained the edge but kept the pending
 * row would be re-inserted on every later drain, and a graph that deleted the
 * row without the edge would lose the link forever.
 *
 * A source that is no longer a document contributes nothing and its pending
 * row still goes: the link was recorded, and the only thing it named is this
 * target, which now exists. A source re-ingested later re-records it.
 *
 * The source list is collected before the writes rather than stepped through
 * while they run: the same connection is doing the insert and the delete, and
 * a statement being stepped is the one whose rows the writes would disturb. */
kbc_status kbc_store_drain_pending(kbc_store *s, const char *corpus,
                                   const char *dst_path, kbc_err *err) {
  if (s == NULL)
    return kbc_err_set(err, KBC_ERR_INVALID, "drain_pending: null store");
  kbc_status st = require_text(err, "pending corpus", corpus, 255);
  if (st == KBC_OK)
    st = require_text(err, "pending dst_path", dst_path, KBC_MAX_PATH_LEN);
  if (st != KBC_OK) return st;

  lock(s);
  char **srcs = NULL;
  size_t n_srcs = 0;
  st = exec_plain(err, s, "BEGIN IMMEDIATE;");
  if (st == KBC_OK) {
    sqlite3_stmt *q = NULL;
    st = prepare(err, s,
                 "SELECT src_path FROM pending_links"
                 " WHERE corpus = ?1 AND dst_path = ?2;",
                 &q);
    if (st == KBC_OK) st = bind_text(err, s, q, 1, corpus);
    if (st == KBC_OK) st = bind_text(err, s, q, 2, dst_path);
    while (st == KBC_OK) {
      int step = sqlite3_step(q);
      if (step == SQLITE_DONE) break;
      if (step != SQLITE_ROW) {
        st = sql_fail(err, s, "read pending links: step", step);
        break;
      }
      char **grown = realloc(srcs, (n_srcs + 1u) * sizeof(*srcs));
      if (grown == NULL) {
        st = kbc_err_set(err, KBC_ERR_NOMEM, "drain %zu pending links",
                        n_srcs + 1u);
        break;
      }
      srcs = grown;
      const char *p = (const char *)sqlite3_column_text(q, 0);
      srcs[n_srcs] = dup_str(p ? p : "");
      if (srcs[n_srcs] == NULL) {
        st = kbc_err_set(err, KBC_ERR_NOMEM, "drain %zu pending links",
                        n_srcs + 1u);
        break;
      }
      n_srcs++;
    }
    kbc_status fin = finalize(err, s, q, st);
    if (st == KBC_OK) st = fin;
  }

  if (st == KBC_OK) {
    for (size_t i = 0; st == KBC_OK && i < n_srcs; i++) {
      sqlite3_stmt *edge = NULL;
      /* The source must still be a document: an edge is a link between two
       * indexed documents, and a deleted source has no place to hang one. */
      st = prepare(err, s,
                   "INSERT OR IGNORE INTO edges(corpus, src_path, dst_path) "
                   "SELECT ?1, p.path, ?2 FROM artifacts p"
                   " WHERE p.corpus = ?1 AND p.path = ?3;",
                   &edge);
      if (st == KBC_OK) st = bind_text(err, s, edge, 1, corpus);
      if (st == KBC_OK) st = bind_text(err, s, edge, 2, dst_path);
      if (st == KBC_OK) st = bind_text(err, s, edge, 3, srcs[i]);
      if (st == KBC_OK) {
        int step = sqlite3_step(edge);
        if (step != SQLITE_DONE)
          st = sql_fail(err, s, "drain pending link: step", step);
      }
      kbc_status fin = finalize(err, s, edge, st);
      if (st == KBC_OK) st = fin;
    }
  }

  if (st == KBC_OK) {
    sqlite3_stmt *del = NULL;
    st = prepare(err, s,
                 "DELETE FROM pending_links WHERE corpus = ?1 AND dst_path = "
                 "?2;",
                 &del);
    if (st == KBC_OK) st = bind_text(err, s, del, 1, corpus);
    if (st == KBC_OK) st = bind_text(err, s, del, 2, dst_path);
    if (st == KBC_OK) {
      int step = sqlite3_step(del);
      if (step != SQLITE_DONE)
        st = sql_fail(err, s, "delete drained links: step", step);
    }
    kbc_status fin = finalize(err, s, del, st);
    if (st == KBC_OK) st = fin;
  }

  for (size_t i = 0; i < n_srcs; i++) free(srcs[i]);
  free(srcs);
  if (st == KBC_OK) st = exec_plain(err, s, "COMMIT;");
  if (st != KBC_OK) rollback(s);
  unlock(s);
  return st;
}

/* Drops every pending link LEAVING a source. Called with its edges when a
 * document is removed: the pending rows name links that document made, and
 * nothing else records that it made them. */
kbc_status kbc_store_delete_pending(kbc_store *s, const char *corpus,
                                    const char *src_path, kbc_err *err) {
  if (s == NULL)
    return kbc_err_set(err, KBC_ERR_INVALID, "delete_pending: null store");
  kbc_status st = require_text(err, "pending corpus", corpus, 255);
  if (st == KBC_OK)
    st = require_text(err, "pending src_path", src_path, KBC_MAX_PATH_LEN);
  if (st != KBC_OK) return st;

  lock(s);
  sqlite3_stmt *st1 = NULL;
  st = prepare(err, s,
               "DELETE FROM pending_links WHERE corpus = ?1 AND src_path = "
               "?2;",
               &st1);
  if (st == KBC_OK) st = bind_text(err, s, st1, 1, corpus);
  if (st == KBC_OK) st = bind_text(err, s, st1, 2, src_path);
  if (st == KBC_OK) {
    int step = sqlite3_step(st1);
    if (step != SQLITE_DONE) st = sql_fail(err, s, "delete pending: step", step);
  }
  kbc_status fin = finalize(err, s, st1, st);
  unlock(s);
  return st != KBC_OK ? st : fin;
}

int64_t kbc_store_pending_count(kbc_store *s, kbc_err *err) {
  if (s == NULL)
    return kbc_err_set(err, KBC_ERR_INVALID, "pending_count: null store");
  int64_t out = 0;
  lock(s);
  kbc_status st = count_query(err, s, "SELECT COUNT(*) FROM pending_links;",
                              NULL, &out);
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

/* ===================================================== stage 1: the rest ==
 *
 * The eight tables the Rust original keeps in sqlite beside its columnar
 * index (see the section banner in store.h for the scope contract). Two
 * properties hold across all of them and are worth stating once here:
 *
 *  - They OUTLIVE the artifact. None of these tables has a foreign key to
 *    `artifacts`, because Rust's cascade stops at artifacts, chunks,
 *    comments and edges (sqlite.rs:6392) and a user's reading history
 *    outliving the document they read is the behaviour being preserved.
 *  - Every list is ordered most-recent-first, and the tiebreak after the
 *    timestamp is stated at each query: a burst of rows inside one second
 *    must not come back in rowid order, which is insertion order and changes
 *    with the order the writer happened to use.
 */

/* `finished_at` for a run that has not finished. Rust stores SQL NULL there;
 * a C row struct cannot be NULL, so the store writes ONE sentinel and a
 * reader tests for a negative value. Every write path goes through
 * finish_value() so a caller cannot mint a second in-flight value that some
 * other reader's `== RUN_IN_FLIGHT` comparison would miss. */
#define RUN_IN_FLIGHT ((int64_t)-1)

static int64_t finish_value(int64_t finished_at) {
  return finished_at < 0 ? RUN_IN_FLIGHT : finished_at;
}

/* Bounds for the free text these tables carry. A corpus slug and a path reuse
 * the limits the rest of the file already applies; a message, a note, a query
 * and a section id have no natural length, and every one of them can carry
 * bytes taken out of a corpus file (AGENTS.md rule 9), so each gets a
 * ceiling here rather than an unbounded column. */
enum {
  KBC_MAX_ERROR_KIND_LEN = 64,
  KBC_MAX_ERROR_MESSAGE_LEN = 65536,
  KBC_MAX_CONTENT_HASH_LEN = 128,
  KBC_MAX_EXCLUSION_NOTE_LEN = 4096,
  KBC_MAX_HISTORY_QUERY_LEN = 4096,
  KBC_MAX_HISTORY_SECTION_LEN = 512,
  KBC_MAX_HISTORY_SOURCE_LEN = 64,
  KBC_MAX_HISTORY_USER_LEN = 255
};

/* The two steps every list_* below shares: how many rows the page can hold,
 * and an exactly-sized, zeroed arena block to decode them into. `limit` has
 * already been clamped to KBC_MAX_HITS by the caller, which is what makes the
 * count*elem_size multiply below provably in range. `count_sql` references
 * ?1 only when `corpus` is non-NULL, per count_query's contract. */
static kbc_status page_block(kbc_err *err, kbc_store *s, kbc_arena *a,
                             const char *count_sql, const char *corpus,
                             size_t limit, size_t elem_size, void **out,
                             size_t *n_out) {
  *out = NULL;
  *n_out = 0;
  int64_t n64 = 0;
  kbc_status st = count_query(err, s, count_sql, corpus, &n64);
  if (st != KBC_OK) return st;
  if (n64 < 0) n64 = 0;
  const size_t n = (n64 > (int64_t)limit) ? limit : (size_t)n64;
  if (n == 0) return KBC_OK;
  void *block = kbc_arena_calloc(a, n, elem_size);
  if (block == NULL)
    return kbc_err_set(err, KBC_ERR_NOMEM, "list: %zu rows of %zu bytes", n,
                       elem_size);
  *out = block;
  *n_out = n;
  return KBC_OK;
}

/* -------------------------------------------------------------- sources -- */

/* `added_at` and `paused` are deliberately absent from the UPDATE set.
 * `added_at` is when the source was FIRST seen, not when the row was last
 * written, so a re-ingest must not move it; `paused` is operator intent that
 * an ingest has no business undoing (Rust sets only `path`,
 * sqlite.rs:217, and pause is otherwise lost on every restart). */
kbc_status kbc_store_put_source(kbc_store *s, const kbc_source *src,
                                kbc_err *err) {
  if (s == NULL || src == NULL)
    return kbc_err_set(err, KBC_ERR_INVALID, "put_source: null argument");
  kbc_status st = require_text(err, "source corpus", src->corpus, 255);
  if (st == KBC_OK)
    st = require_text(err, "source path", src->path, KBC_MAX_PATH_LEN);
  if (st != KBC_OK) return st;

  lock(s);
  sqlite3_stmt *q = NULL;
  st = prepare(err, s,
               "INSERT INTO sources(slug, path, added_at, paused)"
               " VALUES(?1,?2,?3,?4)"
               " ON CONFLICT(slug) DO UPDATE SET path = excluded.path;",
               &q);
  if (st == KBC_OK) st = bind_text(err, s, q, 1, src->corpus);
  if (st == KBC_OK) st = bind_text(err, s, q, 2, src->path);
  if (st == KBC_OK) st = bind_i64(err, s, q, 3, src->added_at);
  if (st == KBC_OK) st = bind_i64(err, s, q, 4, src->paused ? 1 : 0);
  if (st == KBC_OK) {
    const int step = sqlite3_step(q);
    /* The only constraint this can hit is `path` already naming another
     * source — a caller that has aliased two corpora onto one directory. */
    if (step == SQLITE_CONSTRAINT)
      st = kbc_err_set(err, KBC_ERR_CONFLICT,
                       "put_source %s: %s is already another source's path",
                       src->corpus, src->path);
    else if (step != SQLITE_DONE)
      st = sql_fail(err, s, "put source", step);
  }
  kbc_status fin = finalize(err, s, q, st);
  if (st == KBC_OK) st = fin;
  unlock(s);
  return st;
}

static void read_source(kbc_arena *a, sqlite3_stmt *st, kbc_source *out) {
  out->corpus = col_str(a, st, 0);
  out->path = col_str(a, st, 1);
  out->added_at = (int64_t)sqlite3_column_int64(st, 2);
  out->paused = sqlite3_column_int(st, 3) != 0;
}

kbc_status kbc_store_get_source(kbc_store *s, kbc_arena *a, const char *corpus,
                                kbc_source *out, kbc_err *err) {
  if (s == NULL || a == NULL || out == NULL)
    return kbc_err_set(err, KBC_ERR_INVALID, "get_source: null argument");
  kbc_status st = require_text(err, "source corpus", corpus, 255);
  if (st != KBC_OK) return st;

  lock(s);
  sqlite3_stmt *q = NULL;
  st = prepare(err, s,
               "SELECT slug, path, added_at, paused FROM sources"
               " WHERE slug = ?1;",
               &q);
  if (st == KBC_OK) st = bind_text(err, s, q, 1, corpus);
  if (st != KBC_OK) {
    (void)finalize(err, s, q, st);
    unlock(s);
    return st;
  }
  const int step = sqlite3_step(q);
  if (step == SQLITE_ROW) {
    read_source(a, q, out);
  } else if (step == SQLITE_DONE) {
    st = kbc_err_set(err, KBC_ERR_NOTFOUND, "source %s: not found", corpus);
  } else {
    st = sql_fail(err, s, "get source", step);
  }
  kbc_status fin = finalize(err, s, q, st);
  unlock(s);
  return st != KBC_OK ? st : fin;
}

/* Most recently added first, corpus as the tiebreak so two sources adopted in
 * the same second do not come back in insertion order. */
kbc_status kbc_store_list_sources(kbc_store *s, kbc_arena *a, size_t limit,
                                  kbc_source **out, size_t *n_out,
                                  kbc_err *err) {
  if (s == NULL || a == NULL || out == NULL || n_out == NULL)
    return kbc_err_set(err, KBC_ERR_INVALID, "list_sources: null argument");
  if (limit > KBC_MAX_HITS) limit = KBC_MAX_HITS;
  *out = NULL;
  *n_out = 0;
  if (limit == 0) return KBC_OK;

  lock(s);
  void *block = NULL;
  size_t n = 0;
  kbc_status st = page_block(err, s, a, "SELECT COUNT(*) FROM sources;", NULL,
                             limit, sizeof(kbc_source), &block, &n);
  sqlite3_stmt *q = NULL;
  if (st == KBC_OK)
    st = prepare(err, s,
                 "SELECT slug, path, added_at, paused FROM sources"
                 " ORDER BY added_at DESC, slug ASC LIMIT ?1;",
                 &q);
  if (st == KBC_OK) st = bind_i64(err, s, q, 1, (int64_t)n);
  kbc_source *arr = block;
  size_t i = 0;
  while (st == KBC_OK) {
    const int step = sqlite3_step(q);
    if (step == SQLITE_DONE) break;
    if (step != SQLITE_ROW) {
      st = sql_fail(err, s, "list sources: step", step);
      break;
    }
    if (i >= n) {
      st = kbc_err_set(err, KBC_ERR_INTERNAL, "list sources: row overflow");
      break;
    }
    read_source(a, q, &arr[i]);
    i++;
  }
  kbc_status fin = finalize(err, s, q, st);
  unlock(s);
  if (st != KBC_OK) return st != KBC_OK ? st : fin;
  *out = arr;
  *n_out = i;
  return KBC_OK;
}

kbc_status kbc_store_set_source_paused(kbc_store *s, const char *corpus,
                                       bool paused, kbc_err *err) {
  if (s == NULL)
    return kbc_err_set(err, KBC_ERR_INVALID, "set_source_paused: null store");
  kbc_status st = require_text(err, "source corpus", corpus, 255);
  if (st != KBC_OK) return st;

  lock(s);
  sqlite3_stmt *q = NULL;
  st = prepare(err, s, "UPDATE sources SET paused = ?1 WHERE slug = ?2;", &q);
  if (st == KBC_OK) st = bind_i64(err, s, q, 1, paused ? 1 : 0);
  if (st == KBC_OK) st = bind_text(err, s, q, 2, corpus);
  if (st == KBC_OK) {
    const int step = sqlite3_step(q);
    if (step != SQLITE_DONE)
      st = sql_fail(err, s, "pause source", step);
  }
  kbc_status fin = finalize(err, s, q, st);
  if (st == KBC_OK && sqlite3_changes(s->db) == 0)
    st = kbc_err_set(err, KBC_ERR_NOTFOUND, "source %s: not found", corpus);
  unlock(s);
  return st != KBC_OK ? st : fin;
}

/* ----------------------------------------------------------- index runs -- */

static void read_index_run(kbc_arena *a, sqlite3_stmt *st, kbc_index_run *out) {
  out->id = col_str(a, st, 0);
  out->corpus = col_str(a, st, 1);
  out->started_at = (int64_t)sqlite3_column_int64(st, 2);
  out->finished_at = (int64_t)sqlite3_column_int64(st, 3);
  out->ok_count = (int64_t)sqlite3_column_int64(st, 4);
  out->err_count = (int64_t)sqlite3_column_int64(st, 5);
}

kbc_status kbc_store_put_index_run(kbc_store *s, const kbc_index_run *run,
                                   kbc_err *err) {
  if (s == NULL || run == NULL)
    return kbc_err_set(err, KBC_ERR_INVALID, "put_index_run: null argument");
  kbc_status st = require_text(err, "run id", run->id, KBC_MAX_ID_LEN);
  if (st == KBC_OK) st = require_text(err, "run corpus", run->corpus, 255);
  if (st != KBC_OK) return st;

  lock(s);
  sqlite3_stmt *q = NULL;
  st = prepare(err, s,
               "INSERT INTO index_runs(id, corpus, started_at, finished_at,"
               " ok_count, err_count) VALUES(?1,?2,?3,?4,?5,?6);",
               &q);
  if (st == KBC_OK) st = bind_text(err, s, q, 1, run->id);
  if (st == KBC_OK) st = bind_text(err, s, q, 2, run->corpus);
  if (st == KBC_OK) st = bind_i64(err, s, q, 3, run->started_at);
  if (st == KBC_OK)
    st = bind_i64(err, s, q, 4, finish_value(run->finished_at));
  if (st == KBC_OK) st = bind_i64(err, s, q, 5, run->ok_count);
  if (st == KBC_OK) st = bind_i64(err, s, q, 6, run->err_count);
  if (st == KBC_OK) {
    const int step = sqlite3_step(q);
    /* The foreign key is the only constraint here that a caller can reach:
     * a run is always about a source that exists, because the source row is
     * what the run is FOR. */
    if (step == SQLITE_CONSTRAINT)
      st = kbc_err_set(err, KBC_ERR_CONFLICT,
                       "index run %s: no source named %s (or duplicate run id)",
                       run->id, run->corpus);
    else if (step != SQLITE_DONE)
      st = sql_fail(err, s, "put index run", step);
  }
  kbc_status fin = finalize(err, s, q, st);
  if (st == KBC_OK) st = fin;
  unlock(s);
  return st;
}

kbc_status kbc_store_finish_index_run(kbc_store *s, const char *id,
                                      int64_t finished_at, int64_t ok_count,
                                      int64_t err_count, kbc_err *err) {
  if (s == NULL)
    return kbc_err_set(err, KBC_ERR_INVALID, "finish_index_run: null store");
  kbc_status st = require_text(err, "run id", id, KBC_MAX_ID_LEN);
  if (st != KBC_OK) return st;
  /* A finished run that reads back as in flight would be indistinguishable
   * from one that never finished, which is the one distinction this column
   * exists to carry. */
  if (finished_at < 0)
    return kbc_err_set(err, KBC_ERR_INVALID,
                       "finish_index_run %s: finished_at %lld is negative",
                       id, (long long)finished_at);

  lock(s);
  sqlite3_stmt *q = NULL;
  st = prepare(err, s,
               "UPDATE index_runs SET finished_at = ?1, ok_count = ?2,"
               " err_count = ?3 WHERE id = ?4;",
               &q);
  if (st == KBC_OK) st = bind_i64(err, s, q, 1, finished_at);
  if (st == KBC_OK) st = bind_i64(err, s, q, 2, ok_count);
  if (st == KBC_OK) st = bind_i64(err, s, q, 3, err_count);
  if (st == KBC_OK) st = bind_text(err, s, q, 4, id);
  if (st == KBC_OK) {
    const int step = sqlite3_step(q);
    if (step != SQLITE_DONE)
      st = sql_fail(err, s, "finish index run", step);
  }
  kbc_status fin = finalize(err, s, q, st);
  if (st == KBC_OK && sqlite3_changes(s->db) == 0)
    st = kbc_err_set(err, KBC_ERR_NOTFOUND, "index run %s: not found", id);
  unlock(s);
  return st != KBC_OK ? st : fin;
}

/* Newest pass first, id as the tiebreak so two passes started in the same
 * second do not ride insertion order. `corpus` NULL asks for every source. */
kbc_status kbc_store_list_index_runs(kbc_store *s, kbc_arena *a,
                                     const char *corpus, size_t limit,
                                     kbc_index_run **out, size_t *n_out,
                                     kbc_err *err) {
  if (s == NULL || a == NULL || out == NULL || n_out == NULL)
    return kbc_err_set(err, KBC_ERR_INVALID, "list_index_runs: null argument");
  if (corpus != NULL && corpus[0] == '\0')
    return kbc_err_set(err, KBC_ERR_INVALID, "list_index_runs: empty corpus");
  if (limit > KBC_MAX_HITS) limit = KBC_MAX_HITS;
  *out = NULL;
  *n_out = 0;
  if (limit == 0) return KBC_OK;

  lock(s);
  void *block = NULL;
  size_t n = 0;
  kbc_status st =
      page_block(err, s, a,
                 corpus != NULL
                     ? "SELECT COUNT(*) FROM index_runs WHERE corpus = ?1;"
                     : "SELECT COUNT(*) FROM index_runs;",
                 corpus, limit, sizeof(kbc_index_run), &block, &n);
  sqlite3_stmt *q = NULL;
  if (st == KBC_OK)
    st = prepare(err, s,
                 corpus != NULL
                     ? "SELECT id, corpus, started_at, finished_at, ok_count,"
                       " err_count FROM index_runs WHERE corpus = ?1"
                       " ORDER BY started_at DESC, id ASC LIMIT ?2;"
                     : "SELECT id, corpus, started_at, finished_at, ok_count,"
                       " err_count FROM index_runs"
                       " ORDER BY started_at DESC, id ASC LIMIT ?1;",
                 &q);
  if (st == KBC_OK && corpus != NULL) st = bind_text(err, s, q, 1, corpus);
  if (st == KBC_OK)
    st = bind_i64(err, s, q, corpus != NULL ? 2 : 1, (int64_t)n);
  kbc_index_run *arr = block;
  size_t i = 0;
  while (st == KBC_OK) {
    const int step = sqlite3_step(q);
    if (step == SQLITE_DONE) break;
    if (step != SQLITE_ROW) {
      st = sql_fail(err, s, "list index runs: step", step);
      break;
    }
    if (i >= n) {
      st = kbc_err_set(err, KBC_ERR_INTERNAL, "list index runs: row overflow");
      break;
    }
    read_index_run(a, q, &arr[i]);
    i++;
  }
  kbc_status fin = finalize(err, s, q, st);
  unlock(s);
  if (st != KBC_OK) return st != KBC_OK ? st : fin;
  *out = arr;
  *n_out = i;
  return KBC_OK;
}

/* --------------------------------------------------------------- errors -- */

static void read_error_row(kbc_arena *a, sqlite3_stmt *st,
                           kbc_error_row *out) {
  out->id = col_str(a, st, 0);
  out->kind = col_str(a, st, 1);
  out->corpus = col_str(a, st, 2);
  out->path = col_str(a, st, 3);
  out->message = col_str(a, st, 4);
  out->content_hash = col_str(a, st, 5);
  out->retry_count = (int64_t)sqlite3_column_int64(st, 6);
  out->created_at = (int64_t)sqlite3_column_int64(st, 7);
  out->dismissed = sqlite3_column_int(st, 8) != 0;
}

/* One row per failing path. A second failure of the SAME (corpus, path)
 * increments `retry_count` and refreshes the message and the timestamp; it
 * does not insert a duplicate. That counter is the quarantine gate's only
 * input (store.h), so a store that duplicated instead of counting would let a
 * document that fails forever be embedded forever.
 *
 * `UPDATE` then `INSERT` in ONE transaction, the shape Rust uses
 * (sqlite.rs:363): the two statements cannot both run without the
 * transaction, because a crash between them would lose the increment and
 * start the count again.
 *
 * A row that has been dismissed is not matched by the UPDATE — a cleared
 * error stays cleared until the file's content hash moves it — so a
 * re-exclusion is an INSERT and gets a fresh id. */
kbc_status kbc_store_record_error(kbc_store *s, const kbc_error_row *row,
                                  kbc_err *err) {
  if (s == NULL || row == NULL)
    return kbc_err_set(err, KBC_ERR_INVALID, "record_error: null argument");
  kbc_status st = require_text(err, "error id", row->id, KBC_MAX_ID_LEN);
  if (st == KBC_OK) st = require_text(err, "error kind", row->kind,
                                      KBC_MAX_ERROR_KIND_LEN);
  if (st == KBC_OK) st = require_text(err, "error corpus", row->corpus, 255);
  if (st == KBC_OK)
    st = require_text(err, "error path", row->path, KBC_MAX_PATH_LEN);
  if (st == KBC_OK)
    st = require_text(err, "error message", row->message,
                      KBC_MAX_ERROR_MESSAGE_LEN);
  if (st == KBC_OK && row->content_hash != NULL)
    st = require_text(err, "error content hash", row->content_hash,
                      KBC_MAX_CONTENT_HASH_LEN);
  if (st != KBC_OK) return st;

  lock(s);
  st = exec_plain(err, s, "BEGIN IMMEDIATE;");
  sqlite3_stmt *q = NULL;
  if (st == KBC_OK) {
    const kbc_status prepared = prepare(
        err, s,
        "UPDATE errors SET retry_count = retry_count + 1, message = ?1,"
        " created_at = ?2 WHERE corpus = ?3 AND path = ?4 AND dismissed = 0;",
        &q);
    if (prepared != KBC_OK) {
      st = prepared;
    } else {
      st = bind_text(err, s, q, 1, row->message);
      if (st == KBC_OK) st = bind_i64(err, s, q, 2, row->created_at);
      if (st == KBC_OK) st = bind_text(err, s, q, 3, row->corpus);
      if (st == KBC_OK) st = bind_text(err, s, q, 4, row->path);
      if (st == KBC_OK) {
        const int step = sqlite3_step(q);
        if (step != SQLITE_DONE)
          st = sql_fail(err, s, "bump retry count", step);
      }
      /* finalize() reports OK for a statement whose own work already failed,
       * so the caller's diagnosis has to survive it. */
      const kbc_status fin = finalize(err, s, q, st);
      if (st == KBC_OK) st = fin;
    }
  }
  /* sqlite3_finalize has run, so the write is complete and the change count
   * is this statement's — read it before the next statement resets it. */
  if (st == KBC_OK && sqlite3_changes(s->db) == 0) {
    const kbc_status prepared = prepare(
        err, s,
        "INSERT INTO errors(id, kind, corpus, path, message, content_hash,"
        " retry_count, created_at, dismissed)"
        " VALUES(?1,?2,?3,?4,?5,?6,0,?7,0);",
        &q);
    if (prepared != KBC_OK) {
      st = prepared;
    } else {
      st = bind_text(err, s, q, 1, row->id);
      if (st == KBC_OK) st = bind_text(err, s, q, 2, row->kind);
      if (st == KBC_OK) st = bind_text(err, s, q, 3, row->corpus);
      if (st == KBC_OK) st = bind_text(err, s, q, 4, row->path);
      if (st == KBC_OK) st = bind_text(err, s, q, 5, row->message);
      if (st == KBC_OK && row->content_hash != NULL)
        st = bind_text(err, s, q, 6, row->content_hash);
      if (st == KBC_OK) st = bind_i64(err, s, q, 7, row->created_at);
      if (st == KBC_OK) {
        const int step = sqlite3_step(q);
        if (step == SQLITE_CONSTRAINT)
          st = kbc_err_set(err, KBC_ERR_CONFLICT,
                           "record_error %s: duplicate error id", row->id);
        else if (step != SQLITE_DONE)
          st = sql_fail(err, s, "record error", step);
      }
      const kbc_status fin = finalize(err, s, q, st);
      if (st == KBC_OK) st = fin;
    }
  }
  if (st != KBC_OK) {
    rollback(s);
    unlock(s);
    return st;
  }
  st = exec_plain(err, s, "COMMIT;");
  unlock(s);
  return st;
}

/* The quarantine gate. 0 for a path with no open error, which is what a
 * document that has never failed must read as. A DISMISSED row is not
 * counted: clearing an error is what gives a document a fresh budget of
 * attempts (Rust's un-quarantine route, sqlite.rs:413). */
kbc_status kbc_store_retry_count_for_path(kbc_store *s, const char *corpus,
                                          const char *path, int64_t *out,
                                          kbc_err *err) {
  if (s == NULL || out == NULL)
    return kbc_err_set(err, KBC_ERR_INVALID,
                       "retry_count_for_path: null argument");
  kbc_status st = require_text(err, "error corpus", corpus, 255);
  if (st == KBC_OK)
    st = require_text(err, "error path", path, KBC_MAX_PATH_LEN);
  if (st != KBC_OK) return st;

  lock(s);
  sqlite3_stmt *q = NULL;
  st = prepare(err, s,
               "SELECT IFNULL((SELECT retry_count FROM errors"
               " WHERE corpus = ?1 AND path = ?2 AND dismissed = 0"
               " LIMIT 1), 0);",
               &q);
  if (st == KBC_OK) st = bind_text(err, s, q, 1, corpus);
  if (st == KBC_OK) st = bind_text(err, s, q, 2, path);
  if (st != KBC_OK) {
    (void)finalize(err, s, q, st);
    unlock(s);
    return st;
  }
  const int step = sqlite3_step(q);
  if (step == SQLITE_ROW) {
    *out = (int64_t)sqlite3_column_int64(q, 0);
  } else if (step == SQLITE_DONE) {
    st = kbc_err_set(err, KBC_ERR_INTERNAL, "retry count: no row");
  } else {
    st = sql_fail(err, s, "retry count", step);
  }
  kbc_status fin = finalize(err, s, q, st);
  unlock(s);
  return st != KBC_OK ? st : fin;
}

/* Clears every open error for a path — the "the file changed, the parse
 * error may no longer apply" path and the un-quarantine route. Dismissed, not
 * deleted: the history of a failure is what tells an operator why a document
 * is being treated differently. NOTFOUND when there was nothing open, so a
 * caller that believes it cleared something is told it did not. */
kbc_status kbc_store_clear_error(kbc_store *s, const char *corpus,
                                 const char *path, kbc_err *err) {
  if (s == NULL)
    return kbc_err_set(err, KBC_ERR_INVALID, "clear_error: null store");
  kbc_status st = require_text(err, "error corpus", corpus, 255);
  if (st == KBC_OK)
    st = require_text(err, "error path", path, KBC_MAX_PATH_LEN);
  if (st != KBC_OK) return st;

  lock(s);
  sqlite3_stmt *q = NULL;
  st = prepare(err, s,
               "UPDATE errors SET dismissed = 1"
               " WHERE corpus = ?1 AND path = ?2 AND dismissed = 0;",
               &q);
  if (st == KBC_OK) st = bind_text(err, s, q, 1, corpus);
  if (st == KBC_OK) st = bind_text(err, s, q, 2, path);
  if (st == KBC_OK) {
    const int step = sqlite3_step(q);
    if (step != SQLITE_DONE) st = sql_fail(err, s, "clear error", step);
  }
  kbc_status fin = finalize(err, s, q, st);
  if (st == KBC_OK && sqlite3_changes(s->db) == 0)
    st = kbc_err_set(err, KBC_ERR_NOTFOUND, "error %s/%s: nothing open",
                     corpus, path);
  unlock(s);
  return st != KBC_OK ? st : fin;
}

/* Newest failure first, id ascending as the tiebreak: a burst of failures in
 * one second must come back in a deterministic order, and Rust's is exactly
 * this (sqlite.rs:436). `open_only` is the query idx_errors_open exists to
 * serve, and `corpus` NULL asks for every source. */
kbc_status kbc_store_list_errors(kbc_store *s, kbc_arena *a, const char *corpus,
                                 bool open_only, size_t limit,
                                 kbc_error_row **out, size_t *n_out,
                                 kbc_err *err) {
  if (s == NULL || a == NULL || out == NULL || n_out == NULL)
    return kbc_err_set(err, KBC_ERR_INVALID, "list_errors: null argument");
  if (corpus != NULL && corpus[0] == '\0')
    return kbc_err_set(err, KBC_ERR_INVALID, "list_errors: empty corpus");
  if (limit > KBC_MAX_HITS) limit = KBC_MAX_HITS;
  *out = NULL;
  *n_out = 0;
  if (limit == 0) return KBC_OK;

  /* Four fixed statements, each a compile-time literal chosen by two
   * booleans — the same shape kbc_store_list_artifact_ids uses. No value
   * ever reaches the SQL text; the corpus and the page size are bound. */
  static const char *const COUNTS[4] = {
      "SELECT COUNT(*) FROM errors;",
      "SELECT COUNT(*) FROM errors WHERE dismissed = 0;",
      "SELECT COUNT(*) FROM errors WHERE corpus = ?1;",
      "SELECT COUNT(*) FROM errors WHERE corpus = ?1 AND dismissed = 0;"};
  static const char *const PAGES[4] = {
      "SELECT id, kind, corpus, path, message, content_hash, retry_count,"
      " created_at, dismissed FROM errors ORDER BY created_at DESC, id ASC"
      " LIMIT ?1;",
      "SELECT id, kind, corpus, path, message, content_hash, retry_count,"
      " created_at, dismissed FROM errors WHERE dismissed = 0"
      " ORDER BY created_at DESC, id ASC LIMIT ?1;",
      "SELECT id, kind, corpus, path, message, content_hash, retry_count,"
      " created_at, dismissed FROM errors WHERE corpus = ?1"
      " ORDER BY created_at DESC, id ASC LIMIT ?2;",
      "SELECT id, kind, corpus, path, message, content_hash, retry_count,"
      " created_at, dismissed FROM errors"
      " WHERE corpus = ?1 AND dismissed = 0"
      " ORDER BY created_at DESC, id ASC LIMIT ?2;"};
  const size_t which = (corpus != NULL ? 2u : 0u) + (open_only ? 1u : 0u);

  lock(s);
  void *block = NULL;
  size_t n = 0;
  kbc_status st = page_block(err, s, a, COUNTS[which], corpus, limit,
                             sizeof(kbc_error_row), &block, &n);
  sqlite3_stmt *q = NULL;
  if (st == KBC_OK) st = prepare(err, s, PAGES[which], &q);
  if (st == KBC_OK && corpus != NULL) st = bind_text(err, s, q, 1, corpus);
  if (st == KBC_OK)
    st = bind_i64(err, s, q, corpus != NULL ? 2 : 1, (int64_t)n);
  kbc_error_row *arr = block;
  size_t i = 0;
  while (st == KBC_OK) {
    const int step = sqlite3_step(q);
    if (step == SQLITE_DONE) break;
    if (step != SQLITE_ROW) {
      st = sql_fail(err, s, "list errors: step", step);
      break;
    }
    if (i >= n) {
      st = kbc_err_set(err, KBC_ERR_INTERNAL, "list errors: row overflow");
      break;
    }
    read_error_row(a, q, &arr[i]);
    i++;
  }
  kbc_status fin = finalize(err, s, q, st);
  unlock(s);
  if (st != KBC_OK) return st != KBC_OK ? st : fin;
  *out = arr;
  *n_out = i;
  return KBC_OK;
}

/* ------------------------------------------------------ excluded files -- */

/* Durable operator intent, so re-adding an already-excluded path is a no-op
 * that keeps the ORIGINAL excluded_at and note: the first decision is the
 * one the operator made, and a bring-up that re-asserted it would push the
 * path down the "newest exclusions" list every restart (Rust's add_exclusion,
 * sqlite.rs:266). */
kbc_status kbc_store_add_exclusion(kbc_store *s, const kbc_exclusion *x,
                                   kbc_err *err) {
  if (s == NULL || x == NULL)
    return kbc_err_set(err, KBC_ERR_INVALID, "add_exclusion: null argument");
  kbc_status st = require_text(err, "excluded path", x->path, KBC_MAX_PATH_LEN);
  if (st == KBC_OK && x->note != NULL)
    st = require_text(err, "exclusion note", x->note,
                      KBC_MAX_EXCLUSION_NOTE_LEN);
  if (st != KBC_OK) return st;

  lock(s);
  sqlite3_stmt *q = NULL;
  st = prepare(err, s,
               "INSERT INTO excluded_files(path, excluded_at, note)"
               " VALUES(?1,?2,?3) ON CONFLICT(path) DO NOTHING;",
               &q);
  if (st == KBC_OK) st = bind_text(err, s, q, 1, x->path);
  if (st == KBC_OK) st = bind_i64(err, s, q, 2, x->excluded_at);
  if (st == KBC_OK && x->note != NULL)
    st = bind_text(err, s, q, 3, x->note);
  if (st == KBC_OK) {
    const int step = sqlite3_step(q);
    if (step != SQLITE_DONE) st = sql_fail(err, s, "add exclusion", step);
  }
  kbc_status fin = finalize(err, s, q, st);
  if (st == KBC_OK) st = fin;
  unlock(s);
  return st;
}

kbc_status kbc_store_remove_exclusion(kbc_store *s, const char *path,
                                      kbc_err *err) {
  if (s == NULL)
    return kbc_err_set(err, KBC_ERR_INVALID, "remove_exclusion: null store");
  kbc_status st = require_text(err, "excluded path", path, KBC_MAX_PATH_LEN);
  if (st != KBC_OK) return st;

  lock(s);
  sqlite3_stmt *q = NULL;
  st = prepare(err, s, "DELETE FROM excluded_files WHERE path = ?1;", &q);
  if (st == KBC_OK) st = bind_text(err, s, q, 1, path);
  if (st == KBC_OK) {
    const int step = sqlite3_step(q);
    if (step != SQLITE_DONE) st = sql_fail(err, s, "remove exclusion", step);
  }
  kbc_status fin = finalize(err, s, q, st);
  if (st == KBC_OK && sqlite3_changes(s->db) == 0)
    st = kbc_err_set(err, KBC_ERR_NOTFOUND, "exclusion %s: not found", path);
  unlock(s);
  return st != KBC_OK ? st : fin;
}

/* Newest exclusion first, path ascending as the tiebreak so a bulk exclude
 * lands in a stable order. Read once at bring-up into the caller's ingest
 * gate: nothing consults this table per document, which is why a limit here
 * costs the caller nothing — a caller that needs the whole set asks for
 * KBC_MAX_HITS. */
kbc_status kbc_store_list_exclusions(kbc_store *s, kbc_arena *a, size_t limit,
                                     kbc_exclusion **out, size_t *n_out,
                                     kbc_err *err) {
  if (s == NULL || a == NULL || out == NULL || n_out == NULL)
    return kbc_err_set(err, KBC_ERR_INVALID, "list_exclusions: null argument");
  if (limit > KBC_MAX_HITS) limit = KBC_MAX_HITS;
  *out = NULL;
  *n_out = 0;
  if (limit == 0) return KBC_OK;

  lock(s);
  void *block = NULL;
  size_t n = 0;
  kbc_status st = page_block(err, s, a,
                             "SELECT COUNT(*) FROM excluded_files;", NULL,
                             limit, sizeof(kbc_exclusion), &block, &n);
  sqlite3_stmt *q = NULL;
  if (st == KBC_OK)
    st = prepare(err, s,
                 "SELECT path, excluded_at, note FROM excluded_files"
                 " ORDER BY excluded_at DESC, path ASC LIMIT ?1;",
                 &q);
  if (st == KBC_OK) st = bind_i64(err, s, q, 1, (int64_t)n);
  kbc_exclusion *arr = block;
  size_t i = 0;
  while (st == KBC_OK) {
    const int step = sqlite3_step(q);
    if (step == SQLITE_DONE) break;
    if (step != SQLITE_ROW) {
      st = sql_fail(err, s, "list exclusions: step", step);
      break;
    }
    if (i >= n) {
      st = kbc_err_set(err, KBC_ERR_INTERNAL, "list exclusions: row overflow");
      break;
    }
    arr[i].path = col_str(a, q, 0);
    arr[i].excluded_at = (int64_t)sqlite3_column_int64(q, 1);
    arr[i].note = col_str(a, q, 2);
    i++;
  }
  kbc_status fin = finalize(err, s, q, st);
  unlock(s);
  if (st != KBC_OK) return st != KBC_OK ? st : fin;
  *out = arr;
  *n_out = i;
  return KBC_OK;
}

/* --------------------------------------------------------- doc history -- */

/* The kind is NOT validated in C: the CHECK on the table is the invariant,
 * and a second copy of the list would be a second thing to forget when a
 * fourth kind is added. A rejected kind comes back as KBC_ERR_INVALID with
 * the offending value in the message, which is what the caller needs.
 *
 * `user` is stored as '' when the caller passes NULL: the column is NOT NULL
 * upstream, where '' is exactly the "pre-multi-user row" marker the identity
 * backfill looks for. `id` is ignored on the way in — it is the rowid, and
 * the row's own value comes back from a list. */
kbc_status kbc_store_add_history(kbc_store *s, const kbc_history_row *row,
                                 kbc_err *err) {
  if (s == NULL || row == NULL)
    return kbc_err_set(err, KBC_ERR_INVALID, "add_history: null argument");
  kbc_status st = require_text(err, "history kind", row->kind, 32);
  if (st == KBC_OK && row->artifact_id != NULL)
    st = require_text(err, "history artifact id", row->artifact_id,
                      KBC_MAX_ID_LEN);
  if (st == KBC_OK && row->query != NULL)
    st = require_text(err, "history query", row->query,
                      KBC_MAX_HISTORY_QUERY_LEN);
  if (st == KBC_OK && row->comment_id != NULL)
    st = require_text(err, "history comment id", row->comment_id,
                      KBC_MAX_ID_LEN);
  if (st == KBC_OK && row->last_section != NULL)
    st = require_text(err, "history section", row->last_section,
                      KBC_MAX_HISTORY_SECTION_LEN);
  if (st == KBC_OK && row->source != NULL)
    st = require_text(err, "history source", row->source,
                      KBC_MAX_HISTORY_SOURCE_LEN);
  if (st == KBC_OK && row->user != NULL)
    st = require_text(err, "history user", row->user, KBC_MAX_HISTORY_USER_LEN);
  if (st != KBC_OK) return st;

  lock(s);
  sqlite3_stmt *q = NULL;
  st = prepare(err, s,
               "INSERT INTO history(kind, artifact_id, query, comment_id,"
               " scroll_y, scroll_max, scroll_y_max, active_ms, last_section,"
               " source, user, started_at, updated_at)"
               " VALUES(?1,?2,?3,?4,?5,?6,?7,?8,?9,?10,?11,?12,?13);",
               &q);
  if (st == KBC_OK) st = bind_text(err, s, q, 1, row->kind);
  if (st == KBC_OK && row->artifact_id != NULL)
    st = bind_text(err, s, q, 2, row->artifact_id);
  if (st == KBC_OK && row->query != NULL)
    st = bind_text(err, s, q, 3, row->query);
  if (st == KBC_OK && row->comment_id != NULL)
    st = bind_text(err, s, q, 4, row->comment_id);
  if (st == KBC_OK) st = bind_i64(err, s, q, 5, row->scroll_y);
  if (st == KBC_OK) st = bind_i64(err, s, q, 6, row->scroll_max);
  if (st == KBC_OK) st = bind_i64(err, s, q, 7, row->scroll_y_max);
  if (st == KBC_OK) st = bind_i64(err, s, q, 8, row->active_ms);
  if (st == KBC_OK && row->last_section != NULL)
    st = bind_text(err, s, q, 9, row->last_section);
  if (st == KBC_OK && row->source != NULL)
    st = bind_text(err, s, q, 10, row->source);
  if (st == KBC_OK)
    st = bind_text(err, s, q, 11, row->user != NULL ? row->user : "");
  if (st == KBC_OK) st = bind_i64(err, s, q, 12, row->started_at);
  if (st == KBC_OK) st = bind_i64(err, s, q, 13, row->updated_at);
  if (st == KBC_OK) {
    const int step = sqlite3_step(q);
    if (step == SQLITE_CONSTRAINT)
      st = kbc_err_set(err, KBC_ERR_INVALID,
                       "add_history: kind \"%s\" is not one of"
                       " open, search, comment",
                       row->kind);
    else if (step != SQLITE_DONE)
      st = sql_fail(err, s, "add history", step);
  }
  kbc_status fin = finalize(err, s, q, st);
  if (st == KBC_OK) st = fin;
  unlock(s);
  return st;
}

/* Newest first — the order every history surface uses — with the rowid
 * DESCENDING as the tiebreak so two events inside one second come back in
 * the order they happened. `user` NULL asks for every user. */
kbc_status kbc_store_list_history(kbc_store *s, kbc_arena *a, const char *user,
                                  size_t limit, kbc_history_row **out,
                                  size_t *n_out, kbc_err *err) {
  if (s == NULL || a == NULL || out == NULL || n_out == NULL)
    return kbc_err_set(err, KBC_ERR_INVALID, "list_history: null argument");
  if (user != NULL && user[0] == '\0')
    return kbc_err_set(err, KBC_ERR_INVALID, "list_history: empty user");
  if (limit > KBC_MAX_HITS) limit = KBC_MAX_HITS;
  *out = NULL;
  *n_out = 0;
  if (limit == 0) return KBC_OK;

  lock(s);
  void *block = NULL;
  size_t n = 0;
  kbc_status st =
      page_block(err, s, a,
                 user != NULL ? "SELECT COUNT(*) FROM history WHERE user = ?1;"
                              : "SELECT COUNT(*) FROM history;",
                 user, limit, sizeof(kbc_history_row), &block, &n);
  sqlite3_stmt *q = NULL;
  if (st == KBC_OK)
    st = prepare(err, s,
                 user != NULL
                     ? "SELECT id, kind, artifact_id, query, comment_id,"
                       " source, user, scroll_y, scroll_max, scroll_y_max,"
                       " active_ms, last_section, started_at, updated_at"
                       " FROM history WHERE user = ?1"
                       " ORDER BY started_at DESC, id DESC LIMIT ?2;"
                     : "SELECT id, kind, artifact_id, query, comment_id,"
                       " source, user, scroll_y, scroll_max, scroll_y_max,"
                       " active_ms, last_section, started_at, updated_at"
                       " FROM history ORDER BY started_at DESC, id DESC"
                       " LIMIT ?1;",
                 &q);
  if (st == KBC_OK && user != NULL) st = bind_text(err, s, q, 1, user);
  if (st == KBC_OK) st = bind_i64(err, s, q, user != NULL ? 2 : 1, (int64_t)n);
  kbc_history_row *arr = block;
  size_t i = 0;
  while (st == KBC_OK) {
    const int step = sqlite3_step(q);
    if (step == SQLITE_DONE) break;
    if (step != SQLITE_ROW) {
      st = sql_fail(err, s, "list history: step", step);
      break;
    }
    if (i >= n) {
      st = kbc_err_set(err, KBC_ERR_INTERNAL, "list history: row overflow");
      break;
    }
    kbc_history_row *r = &arr[i];
    r->id = (int64_t)sqlite3_column_int64(q, 0);
    r->kind = col_str(a, q, 1);
    r->artifact_id = col_str(a, q, 2);
    r->query = col_str(a, q, 3);
    r->comment_id = col_str(a, q, 4);
    r->source = col_str(a, q, 5);
    r->user = col_str(a, q, 6);
    r->scroll_y = (int64_t)sqlite3_column_int64(q, 7);
    r->scroll_max = (int64_t)sqlite3_column_int64(q, 8);
    r->scroll_y_max = (int64_t)sqlite3_column_int64(q, 9);
    r->active_ms = (int64_t)sqlite3_column_int64(q, 10);
    r->last_section = col_str(a, q, 11);
    r->started_at = (int64_t)sqlite3_column_int64(q, 12);
    r->updated_at = (int64_t)sqlite3_column_int64(q, 13);
    i++;
  }
  kbc_status fin = finalize(err, s, q, st);
  unlock(s);
  if (st != KBC_OK) return st != KBC_OK ? st : fin;
  *out = arr;
  *n_out = i;
  return KBC_OK;
}

/* ----------------------------------------------------------- corkboard -- */

/* Anchoring twice is a no-op that keeps the ORIGINAL created_at: the corkboard
 * is "when did you anchor this", and a re-anchor must not make an old
 * document look new (Rust's corkboard_add, sqlite.rs:2831). */
kbc_status kbc_store_add_corkboard(kbc_store *s, const char *artifact_id,
                                   int64_t created_at, kbc_err *err) {
  if (s == NULL)
    return kbc_err_set(err, KBC_ERR_INVALID, "add_corkboard: null store");
  kbc_status st = require_text(err, "artifact id", artifact_id, KBC_MAX_ID_LEN);
  if (st != KBC_OK) return st;

  lock(s);
  sqlite3_stmt *q = NULL;
  st = prepare(err, s,
               "INSERT INTO corkboard(artifact_id, created_at) VALUES(?1,?2)"
               " ON CONFLICT(artifact_id) DO NOTHING;",
               &q);
  if (st == KBC_OK) st = bind_text(err, s, q, 1, artifact_id);
  if (st == KBC_OK) st = bind_i64(err, s, q, 2, created_at);
  if (st == KBC_OK) {
    const int step = sqlite3_step(q);
    if (step != SQLITE_DONE) st = sql_fail(err, s, "anchor", step);
  }
  kbc_status fin = finalize(err, s, q, st);
  if (st == KBC_OK) st = fin;
  unlock(s);
  return st;
}

kbc_status kbc_store_remove_corkboard(kbc_store *s, const char *artifact_id,
                                      kbc_err *err) {
  if (s == NULL)
    return kbc_err_set(err, KBC_ERR_INVALID, "remove_corkboard: null store");
  kbc_status st = require_text(err, "artifact id", artifact_id, KBC_MAX_ID_LEN);
  if (st != KBC_OK) return st;

  lock(s);
  sqlite3_stmt *q = NULL;
  st = prepare(err, s, "DELETE FROM corkboard WHERE artifact_id = ?1;", &q);
  if (st == KBC_OK) st = bind_text(err, s, q, 1, artifact_id);
  if (st == KBC_OK) {
    const int step = sqlite3_step(q);
    if (step != SQLITE_DONE) st = sql_fail(err, s, "unanchor", step);
  }
  kbc_status fin = finalize(err, s, q, st);
  if (st == KBC_OK && sqlite3_changes(s->db) == 0)
    st = kbc_err_set(err, KBC_ERR_NOTFOUND, "corkboard %s: not anchored",
                     artifact_id);
  unlock(s);
  return st != KBC_OK ? st : fin;
}

/* Most recently anchored first, artifact_id ascending as the tiebreak. */
kbc_status kbc_store_list_corkboard(kbc_store *s, kbc_arena *a, size_t limit,
                                    kbc_corkboard_row **out, size_t *n_out,
                                    kbc_err *err) {
  if (s == NULL || a == NULL || out == NULL || n_out == NULL)
    return kbc_err_set(err, KBC_ERR_INVALID, "list_corkboard: null argument");
  if (limit > KBC_MAX_HITS) limit = KBC_MAX_HITS;
  *out = NULL;
  *n_out = 0;
  if (limit == 0) return KBC_OK;

  lock(s);
  void *block = NULL;
  size_t n = 0;
  kbc_status st = page_block(err, s, a, "SELECT COUNT(*) FROM corkboard;", NULL,
                             limit, sizeof(kbc_corkboard_row), &block, &n);
  sqlite3_stmt *q = NULL;
  if (st == KBC_OK)
    st = prepare(err, s,
                 "SELECT artifact_id, created_at FROM corkboard"
                 " ORDER BY created_at DESC, artifact_id ASC LIMIT ?1;",
                 &q);
  if (st == KBC_OK) st = bind_i64(err, s, q, 1, (int64_t)n);
  kbc_corkboard_row *arr = block;
  size_t i = 0;
  while (st == KBC_OK) {
    const int step = sqlite3_step(q);
    if (step == SQLITE_DONE) break;
    if (step != SQLITE_ROW) {
      st = sql_fail(err, s, "list corkboard: step", step);
      break;
    }
    if (i >= n) {
      st = kbc_err_set(err, KBC_ERR_INTERNAL, "list corkboard: row overflow");
      break;
    }
    arr[i].artifact_id = col_str(a, q, 0);
    arr[i].created_at = (int64_t)sqlite3_column_int64(q, 1);
    i++;
  }
  kbc_status fin = finalize(err, s, q, st);
  unlock(s);
  if (st != KBC_OK) return st != KBC_OK ? st : fin;
  *out = arr;
  *n_out = i;
  return KBC_OK;
}

/* Same shape as the corkboard and the same reason: a pin that is set twice
 * keeps the time it was first pinned. */
kbc_status kbc_store_pin_memory(kbc_store *s, const char *artifact_id,
                                int64_t pinned_at, kbc_err *err) {
  if (s == NULL)
    return kbc_err_set(err, KBC_ERR_INVALID, "pin_memory: null store");
  kbc_status st = require_text(err, "artifact id", artifact_id, KBC_MAX_ID_LEN);
  if (st != KBC_OK) return st;

  lock(s);
  sqlite3_stmt *q = NULL;
  st = prepare(err, s,
               "INSERT INTO pinned_memories(artifact_id, pinned_at)"
               " VALUES(?1,?2) ON CONFLICT(artifact_id) DO NOTHING;",
               &q);
  if (st == KBC_OK) st = bind_text(err, s, q, 1, artifact_id);
  if (st == KBC_OK) st = bind_i64(err, s, q, 2, pinned_at);
  if (st == KBC_OK) {
    const int step = sqlite3_step(q);
    if (step != SQLITE_DONE) st = sql_fail(err, s, "pin memory", step);
  }
  kbc_status fin = finalize(err, s, q, st);
  if (st == KBC_OK) st = fin;
  unlock(s);
  return st;
}

kbc_status kbc_store_unpin_memory(kbc_store *s, const char *artifact_id,
                                  kbc_err *err) {
  if (s == NULL)
    return kbc_err_set(err, KBC_ERR_INVALID, "unpin_memory: null store");
  kbc_status st = require_text(err, "artifact id", artifact_id, KBC_MAX_ID_LEN);
  if (st != KBC_OK) return st;

  lock(s);
  sqlite3_stmt *q = NULL;
  st = prepare(err, s, "DELETE FROM pinned_memories WHERE artifact_id = ?1;",
               &q);
  if (st == KBC_OK) st = bind_text(err, s, q, 1, artifact_id);
  if (st == KBC_OK) {
    const int step = sqlite3_step(q);
    if (step != SQLITE_DONE) st = sql_fail(err, s, "unpin memory", step);
  }
  kbc_status fin = finalize(err, s, q, st);
  if (st == KBC_OK && sqlite3_changes(s->db) == 0)
    st = kbc_err_set(err, KBC_ERR_NOTFOUND, "pinned memory %s: not pinned",
                     artifact_id);
  unlock(s);
  return st != KBC_OK ? st : fin;
}

/* Most recently pinned first, artifact_id ascending as the tiebreak. */
kbc_status kbc_store_list_pins(kbc_store *s, kbc_arena *a, size_t limit,
                               kbc_pin_row **out, size_t *n_out, kbc_err *err) {
  if (s == NULL || a == NULL || out == NULL || n_out == NULL)
    return kbc_err_set(err, KBC_ERR_INVALID, "list_pins: null argument");
  if (limit > KBC_MAX_HITS) limit = KBC_MAX_HITS;
  *out = NULL;
  *n_out = 0;
  if (limit == 0) return KBC_OK;

  lock(s);
  void *block = NULL;
  size_t n = 0;
  kbc_status st = page_block(err, s, a,
                             "SELECT COUNT(*) FROM pinned_memories;", NULL,
                             limit, sizeof(kbc_pin_row), &block, &n);
  sqlite3_stmt *q = NULL;
  if (st == KBC_OK)
    st = prepare(err, s,
                 "SELECT artifact_id, pinned_at FROM pinned_memories"
                 " ORDER BY pinned_at DESC, artifact_id ASC LIMIT ?1;",
                 &q);
  if (st == KBC_OK) st = bind_i64(err, s, q, 1, (int64_t)n);
  kbc_pin_row *arr = block;
  size_t i = 0;
  while (st == KBC_OK) {
    const int step = sqlite3_step(q);
    if (step == SQLITE_DONE) break;
    if (step != SQLITE_ROW) {
      st = sql_fail(err, s, "list pins: step", step);
      break;
    }
    if (i >= n) {
      st = kbc_err_set(err, KBC_ERR_INTERNAL, "list pins: row overflow");
      break;
    }
    arr[i].artifact_id = col_str(a, q, 0);
    arr[i].pinned_at = (int64_t)sqlite3_column_int64(q, 1);
    i++;
  }
  kbc_status fin = finalize(err, s, q, st);
  unlock(s);
  if (st != KBC_OK) return st != KBC_OK ? st : fin;
  *out = arr;
  *n_out = i;
  return KBC_OK;
}

/* -------------------------------------------------------- first indexed -- */

/* INSERT OR IGNORE, and that is the entire behaviour. A reindex MUST NOT move
 * this timestamp: it is the anchor a "created" sort needs, and it has to
 * survive both a reindex and a file copy — mtime drifts on edit, btime does
 * not survive a copy, and indexed_at refreshes every pass. An UPDATE here
 * would silently turn the table into a no-op table. */
kbc_status kbc_store_first_seen(kbc_store *s, const char *artifact_id,
                                int64_t first_indexed_unix, kbc_err *err) {
  if (s == NULL)
    return kbc_err_set(err, KBC_ERR_INVALID, "first_seen: null store");
  kbc_status st = require_text(err, "artifact id", artifact_id, KBC_MAX_ID_LEN);
  if (st != KBC_OK) return st;

  lock(s);
  sqlite3_stmt *q = NULL;
  st = prepare(err, s,
               "INSERT OR IGNORE INTO doc_first_seen(artifact_id,"
               " first_indexed_unix) VALUES(?1,?2);",
               &q);
  if (st == KBC_OK) st = bind_text(err, s, q, 1, artifact_id);
  if (st == KBC_OK) st = bind_i64(err, s, q, 2, first_indexed_unix);
  if (st == KBC_OK) {
    const int step = sqlite3_step(q);
    if (step != SQLITE_DONE) st = sql_fail(err, s, "record first seen", step);
  }
  kbc_status fin = finalize(err, s, q, st);
  if (st == KBC_OK) st = fin;
  unlock(s);
  return st;
}

kbc_status kbc_store_get_first_seen(kbc_store *s, const char *artifact_id,
                                    int64_t *out, kbc_err *err) {
  if (s == NULL || out == NULL)
    return kbc_err_set(err, KBC_ERR_INVALID, "get_first_seen: null argument");
  kbc_status st = require_text(err, "artifact id", artifact_id, KBC_MAX_ID_LEN);
  if (st != KBC_OK) return st;

  lock(s);
  sqlite3_stmt *q = NULL;
  st = prepare(err, s,
               "SELECT first_indexed_unix FROM doc_first_seen"
               " WHERE artifact_id = ?1;",
               &q);
  if (st == KBC_OK) st = bind_text(err, s, q, 1, artifact_id);
  if (st != KBC_OK) {
    (void)finalize(err, s, q, st);
    unlock(s);
    return st;
  }
  const int step = sqlite3_step(q);
  if (step == SQLITE_ROW) {
    *out = (int64_t)sqlite3_column_int64(q, 0);
  } else if (step == SQLITE_DONE) {
    st = kbc_err_set(err, KBC_ERR_NOTFOUND, "artifact %s: never indexed",
                     artifact_id);
  } else {
    st = sql_fail(err, s, "get first seen", step);
  }
  kbc_status fin = finalize(err, s, q, st);
  unlock(s);
  return st != KBC_OK ? st : fin;
}
