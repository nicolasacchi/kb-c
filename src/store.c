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
#include <time.h>

#include <sqlite3.h>

#include "kbc/log.h"
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

/* --------------------------------------------------------- fk checking --- */

/* `PRAGMA foreign_keys` is a CONNECTION setting, not part of a table's
 * definition, so no CREATE TABLE in the ladder can carry it and none of them
 * does: a file written by a connection that had it off — an older kb-c, a
 * `sqlite3` on a command line, a backup restored halfway, any writer that was
 * not this binary — holds rows the edges were never asked to reject.
 * `foreign_key_check` is a PRAGMA, so no rung of the ladder runs it either,
 * and a volume can therefore sit at the CURRENT version holding orphaned rows
 * and kb-c will call it migrated and current. That state is reachable, and
 * the store tests plant it on purpose.
 *
 * WARN AND CONTINUE. The tempting alternative is to refuse the open, on the
 * argument this file already makes for a volume ahead of the binary: better
 * a hard error than a daemon that fails later, on the first query. That
 * analogy does not survive contact with what an orphan IS, and two things
 * about this schema decide it.
 *
 * 1. THE ORPHANS ARE INERT. Every read of a child row in this file is keyed
 *    by a doc_id that came out of an artifact that exists: the only two
 *    statements that touch `chunks` or `comments` for a caller are the count
 *    and the fetch inside kbc_store_list_chunks and kbc_store_list_comments,
 *    and both bind ?1 to a doc_id the caller already resolved. A chunk row
 *    whose parent was deleted with the pragma off is unreachable, not wrong:
 *    it is invisible to search and to every listing, and it costs bytes and a
 *    little space. The one read that does NOT pre-resolve is
 *    `kbc_store_list_comment_docs`, which takes DISTINCT doc_id straight off
 *    the comments table; httpd.c's anchor pass looks each of those up, gets a
 *    miss, and drops the anchors — which is exactly what a deleted document
 *    already does. `list_index_runs(NULL)` can name a corpus that is not in
 *    `sources`: a real wrong answer, one row in one listing, not a corpus
 *    answered wrongly.
 * 2. REFUSING IS UNRECOVERABLE THROUGH kb-c. There is no repair verb, and
 *    deciding which side of an orphan is wrong is a decision this layer
 *    cannot make — the parent may be gone for good (drop the child) or merely
 *    invisible (restore it) — so a refusal is a daemon that will not start
 *    until the operator leaves the product for `sqlite3` and edits rows by
 *    hand. kb-c would be trading a listed, invisible defect for an outage, on
 *    a volume whose search results are correct.
 *
 * What this does instead is make the defect impossible to miss and impossible
 * to misreport: EVERY violating row is logged, one line each, carrying the
 * child row's own key value. Never a count and never a prefix — a reader that
 * stops at the first calls a three-edge volume a one-edge one, the operator
 * fixes that one, and finds the rest next week.
 *
 * READ-ONLY BY CONSTRUCTION. The pragma walks the child tables and probes the
 * parent index; it writes nothing, and nothing below deletes, updates or
 * repairs. A check that repaired as a side effect would be making the
 * delete-the-child-or-restore-the-parent decision silently, on every open.
 */

/* The child column that names a violating row, per child table. Matched
 * against the table name the pragma reports and never interpolated blindly:
 * a table with no entry here is reported by rowid alone rather than by a
 * statement assembled from a name the schema chose. These are the whole of
 * the foreign keys the ladder creates (v1's chunks and comments onto
 * artifacts, v5's index_runs onto sources, v13's comment_anchors onto
 * comments, v14's attachments onto artifacts and comments, v15's verdicts
 * onto artifacts), so this list and the schema are two views of the same
 * edges.
 *
 * `comment_anchors` is keyed on `comment_id` and NOT on `doc_id` on purpose.
 * `kbc_store_rekey_artifact` rewrites `comments.doc_id` and never
 * `comments.id` (store.c:3963 — the id is minted and a move must not remint
 * it), so an edge on `comment_id` rides through a move untouched while an
 * edge on `doc_id` would have to be added to that transaction and could be
 * forgotten in it.
 *
 * The same argument picks `attachments`' key, and it picks it ONCE: the row
 * has a stable minted id of its own, and it is the only column a rekey never
 * rewrites. `doc_id` moves and `comment_id` does not, so naming either of
 * them would name a value that is a different one after every move.
 *
 * `verdicts` is keyed on `doc_id` alone, and the rekey rewrites it directly —
 * a verdict is about a document and about nothing on it, so it has no second
 * edge to be careful about. */
static const struct {
  const char *table;
  const char *column;
} FK_CHILD_KEYS[] = {
    {"chunks", "doc_id"},
    {"comments", "doc_id"},
    {"index_runs", "corpus"},
    {"comment_anchors", "comment_id"},
    {"attachments", "id"},
    {"verdicts", "doc_id"},
};

/* The child row's own key value, with the COLUMN it came from, so the log
 * line can say `doc_id='gone00000001'` rather than a bare rowid the operator
 * would have to go and look up. False when FK_CHILD_KEYS does not know the
 * table, or when the lookup found no row (a row deleted between the pragma's
 * read and this one) — both fall back to naming the rowid alone, which is
 * still a real identification, just a weaker one. */

static bool fk_child_key(const kbc_store *s, const char *table,
                         sqlite3_int64 rowid, const char **col_out,
                         char *out, size_t cap) {
  if (table == NULL) return false;
  for (size_t i = 0; i < sizeof FK_CHILD_KEYS / sizeof FK_CHILD_KEYS[0]; i++) {
    if (strcmp(table, FK_CHILD_KEYS[i].table) != 0) continue;
    /* Both identifiers come from FK_CHILD_KEYS, never from the pragma's
     * output, so this statement cannot carry anything user-supplied. */
    char sql[128];
    (void)snprintf(sql, sizeof sql, "SELECT %s FROM %s WHERE rowid = ?1;",
                   FK_CHILD_KEYS[i].column, FK_CHILD_KEYS[i].table);
    sqlite3_stmt *k = NULL;
    if (sqlite3_prepare_v2(s->db, sql, -1, &k, NULL) != SQLITE_OK) return false;
    bool got = false;
    if (sqlite3_bind_int64(k, 1, rowid) == SQLITE_OK &&
        sqlite3_step(k) == SQLITE_ROW) {
      const unsigned char *t = sqlite3_column_text(k, 0);
      if (t != NULL) {
        (void)snprintf(out, cap, "%s", (const char *)t);
        *col_out = FK_CHILD_KEYS[i].column;
        got = true;
      }
    }
    (void)sqlite3_finalize(k);
    return got;
  }
  return false;
}

/* Runs the check and reports. Called on EVERY open — see the call site for
 * why that is not only on a version change — and it returns void by design:
 * there is no status here that could turn a violation into a failed open,
 * because the argument above is that a violation is not a reason to fail.
 * A check that cannot RUN is a different thing entirely and is reported as
 * such, because "unchecked" and "checked and clean" must never read the same.
 */
static void report_foreign_key_violations(const kbc_store *s,
                                         const char *db_path) {
  sqlite3_stmt *q = NULL;
  if (sqlite3_prepare_v2(s->db, "PRAGMA foreign_key_check;", -1, &q, NULL) !=
      SQLITE_OK) {
    KBC_LOGE("store: foreign_key_check did not run on %s: %s — this volume's "
             "edges are UNCHECKED, which is not the same as checked and clean",
             db_path, sqlite3_errmsg(s->db));
    return;
  }

  size_t n = 0;
  for (;;) {
    int step = sqlite3_step(q);
    if (step == SQLITE_DONE) break;
    if (step != SQLITE_ROW) {
      /* Stop counting here rather than reporting a total that is a lie: the
       * rows already logged are real, the number is not. */
      KBC_LOGE("store: foreign_key_check stopped after %zu of an unknown "
               "number of rows on %s: %s",
               n, db_path, sqlite3_errmsg(s->db));
      (void)sqlite3_finalize(q);
      return;
    }

    const char *table = (const char *)sqlite3_column_text(q, 0);
    sqlite3_int64 rowid = sqlite3_column_int64(q, 1);
    const char *parent = (const char *)sqlite3_column_text(q, 2);
    int fkid = sqlite3_column_int(q, 3);

    char key[256];
    const char *col = NULL;
    if (fk_child_key(s, table, rowid, &col, key, sizeof key)) {
      KBC_LOGE("store: foreign key violation: %s rowid %lld -> %s#%d: "
               "%s='%s' names a row that is not there",
               table ? table : "?", (long long)rowid, parent ? parent : "?",
               fkid, col, key);
    } else {
      KBC_LOGE("store: foreign key violation: %s rowid %lld -> %s#%d",
               table ? table : "?", (long long)rowid, parent ? parent : "?",
               fkid);
    }
    n++;
  }
  (void)sqlite3_finalize(q);

  if (n > 0) {
    KBC_LOGW("store: %zu foreign key violation(s) in %s. Every one is listed "
             "above. Nothing was repaired and nothing was written: kb-c will "
             "not decide for you whether the child row or the missing parent "
             "is the wrong one.",
             n, db_path);
  }
}

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

/* v11 — the artifact relocate intent log (V0032). Append-only: a row is
 * written BEFORE any mutation with completed_at NULL, and stamped when the
 * storage-side rekey finishes, so a crash between the rename and the rekey
 * leaves a durable signal rather than a half-carried thread.
 *
 * The DDL and both indexes are the migration file's, unchanged — including
 * that completed_at is NULLABLE here and nowhere else in kb-c's schema,
 * because "in flight" is a real state for this table and every other
 * nullable-in-Rust column was folded into a sentinel back at v6
 * (RUN_IN_FLIGHT). The index names are the Rust ones, so a database opened by
 * either program plans the same lookups.
 *
 * The readers landed with it: kbc_store_record_move / _complete_move write
 * the log, kbc_store_moves_lookup / _lookup_path resolve a stale reference,
 * kbc_store_list_incomplete_moves hands the bring-up pass its replay list, and
 * kbc_store_rekey_artifact carries the rows. The table was originally allowed
 * to land ahead of them, on the grounds that a definition with no declaration
 * and no caller is an entry point with no contract to satisfy.
 */
static const char *const SCHEMA_V11 =
    "CREATE TABLE IF NOT EXISTS moves ("
    " id INTEGER PRIMARY KEY AUTOINCREMENT,"
    " old_id TEXT NOT NULL,"
    " new_id TEXT NOT NULL,"
    " old_rel TEXT NOT NULL,"
    " new_rel TEXT NOT NULL,"
    " moved_at INTEGER NOT NULL,"
    " completed_at INTEGER);"
    "CREATE INDEX IF NOT EXISTS idx_moves_old_id ON moves(old_id);"
    "CREATE INDEX IF NOT EXISTS idx_moves_old_rel ON moves(old_rel);";

/* v12 — `abandoned_at`, the third terminal state of a move.
 *
 * `completed_at` alone has two states and the bring-up pass needs three. A
 * rename that never reached the disk still has to LEAVE the replay list — a
 * pass that re-decides it on every boot is a loop with a log line — and
 * stamping it "completed" is not available, because a completed row is a
 * promise about where the document IS and moves_lookup follows it to a
 * destination that does not exist. That is a wrong answer rather than a
 * missing one, and it is reachable today because kbc_app_get_artifact follows
 * the chain. NULL means "not abandoned" and is what every existing row reads
 * as, so the ALTER is a no-op on the data.
 *
 * WHY NOT FOLDED INTO v11, WHICH IS UNRELEASED. The v5..v10 note above folds
 * every unreleased Rust ALTER into the CREATE that introduces its table, and
 * v11 has been in the tree for days, not releases. The fold is still wrong
 * here, for one reason: it is only safe while NO volume anywhere has recorded
 * version 11, and the ladder can never repair it afterwards. migrate_locked
 * skips every version at or below what the volume has recorded, so a volume
 * that already applied v11 would keep a `moves` table with no `abandoned_at`,
 * and the first query naming the column would fail with "no such column" on a
 * schema whose own version number says it is current — silent drift, which is
 * the class the epoch guard exists to make loud. Any build run since v11
 * landed owns such a volume. One ALTER is the cheap side of that trade; the
 * alternative's cost is a class of broken volumes that nothing detects.
 *
 * The only step in this ladder that is not re-runnable on its own: ADD COLUMN
 * fails outright if the column is already there. It still cannot run twice,
 * because it shares a transaction with the row that records version 12 — a
 * failure rolls the ALTER back with the version, and a volume that recorded 12
 * skips the step. Every other step gets that from IF NOT EXISTS; here the
 * transaction is the ONLY thing giving it, which is the point of one
 * transaction per version. */
static const char *const SCHEMA_V12 =
    "ALTER TABLE moves ADD COLUMN abandoned_at INTEGER;";

/* v13 — `comment_anchors`, the PERSISTED anchor verdict.
 *
 * ============================ THE STALE-SET DECISION ====================
 *
 * PERSIST, and REBUILD FROM THE GRAPH ON OPEN. The evidence, the argument
 * against the alternative, and the property that makes this the right answer
 * rather than a plausible one:
 *
 * WHAT THE ORIGINAL ACTUALLY DOES, because the plan's summary of it is
 * wrong. It does NOT recompute its stale set from scratch. `kb-core/src/
 * anchors.rs` is a PERSISTED sidecar — `.anchors-stale.json`, beside the
 * `.review/` directory, keyed on `(artifact_id, comment_id)`, schema
 * versioned, written atomically (tmpfile + rename + parent fsync). Its own
 * header says the file "is safe to lose" and that a v1 sidecar "loads as
 * empty — the set rebuilds on the next reindex". The indexer keeps the
 * loaded set in a `HashMap` (indexer.rs:990) and rewrites the file once per
 * artifact-reindex that changed something.
 *
 * WHY THE ORIGINAL PERSISTS AT ALL. Not for speed. Its comments live in
 * per-artifact JSON review files on disk, OUTSIDE the database
 * (`review_dir`, indexer.rs:3001), so the indexer has no database row to
 * hang a set on and a sidecar is the only place it can go. That reason does
 * not apply to kb-c: `comments` is a TABLE in the SAME database as the
 * artifacts, so the set here costs a table rather than a file, and it is
 * written in the same transaction as the verdict it describes. A set and the
 * graph it is derived from cannot disagree across a crash if they are
 * committed together — which is the entire failure mode the persist-vs-
 * recompute question is about, answered by the storage engine instead of by
 * discipline.
 *
 * WHY NOT RECOMPUTE. Recompute has one real virtue — no state to go stale —
 * and it buys that virtue by destroying the only thing the set EXISTS for.
 * The set is not a cache of the current truth; it is the memory of the
 * PREVIOUS pass, and the previous pass is what makes a TRANSITION
 * observable. `comment.anchor_stale` fires on the edge INTO stale and
 * `comment.anchor_resolved` on the edge OUT of it (indexer.rs:2993-2996); a
 * set recomputed from the current graph has no edge in it, so every pass
 * re-fires `anchor_stale` for every stale anchor and the event volume is
 * bounded by REINDEXES rather than by transitions. That is the exact failure
 * the in-process set was introduced to prevent (anchors.rs:5-8: the v0.5 P4
 * `HashSet` "died on restart, so the first reindex after a restart could
 * never fire `comment.anchor_resolved`"). Recompute is not a cheaper
 * persist; it is that bug, reimplemented.
 *
 * THE PROPERTY THAT HAS TO BE RIGHT. A persisted set can describe a graph
 * that never committed: the pass judged a document, wrote its verdicts, and
 * the process died before the document was re-indexed — or the document was
 * re-indexed and the process died before the pass ran. Either way the rows
 * on disk are a claim about bytes that are no longer the artifact's bytes.
 * So every row carries the `artifacts.content_hash` it was judged against,
 * and `reconcile_anchor_states` — which `kbc_store_open` runs on EVERY open
 * — rewrites any row whose hash no longer matches into UNDECIDABLE. A row
 * that survives an open is a row that describes the graph which EXISTS.
 *
 * WHY A NEW RUNG AND NOT A FOLD INTO v12, WHICH IS UNRELEASED. v12's own
 * comment gives the reason and it applies here unchanged: a fold is safe
 * only while NO volume anywhere has recorded the folded version, because
 * `migrate_locked` skips every version at or below what a volume has. A
 * volume that applied v12 would skip the folded statement and keep a
 * `comment_anchors`-less schema whose own version number says it is current
 * — the silent drift the epoch guard exists to make loud.
 *
 * WHY THIS STEP IS SAFER THAN v12's. It is a CREATE TABLE and a CREATE
 * INDEX, both `IF NOT EXISTS`, so unlike `ALTER TABLE ... ADD COLUMN` it is
 * re-runnable on its own and does not lean on the shared transaction for its
 * idempotence. The FK to `comments(id)` carries the rest: a comment cannot
 * be deleted while its verdict row names it, and `artifacts` deleting a
 * comment cascades to the verdict, so the set is a SUBSET of the live
 * comment set BY CONSTRUCTION rather than by a sweep somebody has to
 * remember to run.
 */
static const char *const SCHEMA_V13 =
    "CREATE TABLE IF NOT EXISTS comment_anchors ("
    " comment_id TEXT PRIMARY KEY REFERENCES comments(id) ON DELETE CASCADE,"
    " doc_id TEXT NOT NULL,"
    /* The CLAIM, copied from `comments.anchor` when the row was written. A
     * copy rather than a join, so the set still describes what was judged
     * after the comment's own text is edited underneath it. */
    " anchor TEXT NOT NULL,"
    /* The heading TEXT behind `resolves_to` at the first judgement, and the
     * thing that makes a slug COLLISION detectable at all. Empty until a
     * pass has resolved the anchor once. */
    " anchor_text TEXT NOT NULL DEFAULT '',"
    /* 0 undecidable, 1 resolved, 2 unresolved. A CHECK, because the three
     * are an invariant of the TABLE and not of every writer that reaches it
     * — the same argument v6 makes for the history `kind`. */
    " state INTEGER NOT NULL CHECK (state IN (0,1,2)),"
    /* The id the claim currently resolves at, which is NOT always
     * `anchor`: a heading that moved is FOLLOWED, and the follow is recorded
     * here rather than by rewriting `comments.anchor`, because a user's
     * anchor is the user's claim and a machine that edits it is the silent
     * re-anchor this whole design exists to prevent. */
    " resolves_to TEXT NOT NULL DEFAULT '',"
    " resolved_ord INTEGER NOT NULL DEFAULT -1,"
    /* The artifact's content_hash at the moment of the judgement, and the
     * input the open-time rebuild compares against. */
    " content_hash INTEGER NOT NULL,"
    " judged_at INTEGER NOT NULL);"
    /* PARTIAL, and for the same reason idx_errors_open is: it serves exactly
     * one query — the stale set, which is everything that is not RESOLVED —
     * and carrying the resolved rows forever to answer a query that never
     * wants them is what turns a lookup into a scan. */
    "CREATE INDEX IF NOT EXISTS idx_comment_anchors_stale"
    " ON comment_anchors(state, doc_id) WHERE state <> 1;";

/* v14 — `attachments`, the blob and its row in ONE place.
 *
 * WHAT THE RUST ACTUALLY SPECIFIES, because the shape here is not a
 * transcription of it. `kb-core/src/attachments.rs` puts the bytes at
 * `<state>/<kb>/.attachments/<artifact_id>/<aid>` under a random
 * `a_<12 hex>` key, and the metadata in a sibling `_manifest.json`. The
 * reason it needs two files and a GC is spelled out in its own header: the
 * comment that OWNS an attachment lives in a per-artifact JSON review file
 * OUTSIDE the database (`review.rs`, `indexer.rs:3001`), so there is no
 * transaction to write a blob and its owning row in together, and the
 * manifest exists to make the un-owned half recoverable.
 *
 * kb-c has no such split: `comments` is a TABLE in the SAME database. So
 * the split buys a grace window, an fsync-and-rename protocol, a
 * `review_lock` critical section and a crash window in which a blob exists
 * with no manifest entry, in exchange for nothing. One row here carries
 * the bytes AND the claim, so "an attachment stored without its row" is not
 * a state the schema can represent.
 *
 * `comment_id` NULL IS A STAGED UPLOAD, and that is the Rust's `adopted:
 * false` (attachments.rs ManifestEntry). A blob is staged when a composer
 * has uploaded it and not yet put it in a comment; the GC reaps a staged row
 * once it is past the grace window, and never reaps an adopted one by age.
 *
 * `content_type` is the SNIFFED type and never the client's claim
 * (`sniff_allowed`, "this sniff IS the upload gate"). The route enforces
 * that; storing the sniffed value is what makes the serve route unable to
 * drift from it, because the serve route reads the column rather than
 * re-deriving the answer.
 *
 * `filename` is a DISPLAY basename and never a path (`sanitize_filename`).
 * The `aid` is the only key, and it is minted rather than derived from the
 * name, so nothing that arrived on a request ever reaches a path.
 *
 * TWO CASCADES, and both are real on this connection: `kbc_store_open` runs
 * `PRAGMA foreign_keys = ON` (store.c:990), so deleting a comment takes its
 * adopted rows and their bytes in the same commit, and deleting an artifact
 * takes its staged ones. The Rust does neither eagerly — it leaves the blob
 * and lets the reference-counted `gc_plan` reap it — but the Rust cannot do
 * it eagerly, for the reason in the first paragraph.
 *
 * A NEW RUNG AND NOT A FOLD INTO v13, for v12's and v13's reason unchanged:
 * `migrate_locked` skips every version at or below what a volume has
 * recorded, so a volume that already applied 13 would keep a schema whose
 * own version number says it is current with no `attachments` table. That is
 * the silent drift the epoch guard exists to make loud. Re-runnable on its
 * own, like v13: both statements are IF NOT EXISTS. */
static const char *const SCHEMA_V14 =
    "CREATE TABLE IF NOT EXISTS attachments ("
    " id TEXT PRIMARY KEY,"
    " doc_id TEXT NOT NULL REFERENCES artifacts(id) ON DELETE CASCADE,"
    /* NULL is a STAGED upload, not an unattached adopted one: a blob is
     * staged until a comment claims it and stays with that comment for as
     * long as the comment lives. */
    " comment_id TEXT REFERENCES comments(id) ON DELETE CASCADE,"
    " filename TEXT NOT NULL,"
    " content_type TEXT NOT NULL,"
    " size_bytes INTEGER NOT NULL,"
    /* The bytes. In the row, so the row and the bytes cannot disagree. */
    " body BLOB NOT NULL,"
    " author TEXT NOT NULL DEFAULT 'you',"
    " created_at TEXT NOT NULL);"
    /* The listing is per document, and a document's staged rows are part of
     * it: a composer that uploaded and has not posted yet still has to be
     * able to see what it uploaded. */
    "CREATE INDEX IF NOT EXISTS idx_attachments_doc ON attachments(doc_id);"
    /* PARTIAL, and for idx_errors_open's reason: the per-comment question is
     * asked only of ADOPTED rows, and carrying the staged ones forever to
     * answer a query that never wants them is what turns a lookup into a
     * scan. */
    "CREATE INDEX IF NOT EXISTS idx_attachments_comment"
    " ON attachments(comment_id) WHERE comment_id IS NOT NULL;";

/* v15 — `verdicts`, the review pass's own answer about a document.
 *
 * THREE STATES and they are not the comment's open/resolved: `VerdictState`
 * is Comment (a working note, no pass/fail signal), Approve and
 * RequestChanges (review.rs, W2.15a). A document can be approved with three
 * comments open, and it can hold an open comment with no verdict at all, so
 * a boolean on the comment cannot carry it and a flag beside the document is
 * the only place it fits.
 *
 * ONE ROW PER DOCUMENT, which is what makes `decided_at` and `decided_by`
 * attributes of the DOCUMENT rather than of a thread: `set_verdict` stamps
 * both server-side and never client-supplied ("never client-supplied,
 * mirroring Comment::created_at/author").
 *
 * The `status-approved` / `status-changes-requested` kb-tag the original
 * mirrors onto the artifact is DERIVED from this row and written in the same
 * transaction, and the derivation only ever runs one way — invariant #12 says
 * the field is "the verdict of record; the tag is derived, never the other
 * way around". A `Comment` verdict writes NO tag: the display shortcut drops
 * any prior `status-*` rather than writing one for a state that carries no
 * pass/fail signal.
 *
 * FK to artifacts with a cascade, unlike the corkboard and the pins: a
 * verdict is a claim ABOUT a document, so there is nothing for it to say once
 * the document is gone. A reading visit is the opposite case and outlives
 * what it was about.
 *
 * A NEW RUNG AND NOT A FOLD INTO v14, for the same reason v14 is not folded
 * into v13. */
static const char *const SCHEMA_V15 =
    "CREATE TABLE IF NOT EXISTS verdicts ("
    " doc_id TEXT PRIMARY KEY REFERENCES artifacts(id) ON DELETE CASCADE,"
    /* 0 comment, 1 approve, 2 request_changes. A CHECK, because the three
     * are an invariant of the TABLE and not of every writer that reaches it
     * — v6's argument for the history `kind`, applied again. */
    " state INTEGER NOT NULL CHECK (state IN (0,1,2)),"
    " decided_at INTEGER NOT NULL,"
    " decided_by TEXT NOT NULL,"
    " note TEXT);";

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
    {9, SCHEMA_V9},   {10, SCHEMA_V10},  {11, SCHEMA_V11},
    {12, SCHEMA_V12}, {13, SCHEMA_V13}, {14, SCHEMA_V14}, {15, SCHEMA_V15},
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
/* Defined with the other list plumbing, below the anchor section that uses
 * it. Declared here so the anchor reader can share the count-then-fetch
 * shape instead of open-coding a second one. */
static kbc_status page_block(kbc_err *err, kbc_store *s, kbc_arena *a,
                             const char *count_sql, const char *corpus,
                             size_t limit, size_t elem_size, void **out,
                             size_t *n_out);

/* The attachment and verdict contract — the constants, the three types and
 * the fourteen functions — now lives in include/kbc/store.h, which is where
 * a caller reads it. It was carried here as a hand-copied block because that
 * header was the orchestrator's file, and it is gone rather than reconciled:
 * a hand-copied mirror of a frozen contract is the drift a frozen header
 * exists to prevent, not the mitigation for it. The reasoning for each of
 * them stays below, with the implementation, because it is about the
 * algorithm and not the signature. */

/* The anchor state machine. Its contract — the three states and why there are
 * three, the persisted stale set, and the six functions — now lives in
 * include/kbc/store.h, which is where a caller reads it. It was carried here
 * as a hand-copied declaration block because the header was the orchestrator's
 * file; that is exactly the drift a frozen header exists to prevent, and it is
 * gone rather than reconciled. The reasoning below stays because it is about
 * the algorithm, not the signature. */

/* The three verdicts, and the reason there are three.
 *
 * A comment's anchor is a CLAIM about a document's headings, and the
 * headings move. Two of the three answers are the ones a boolean has:
 * RESOLVED (the claim still names what it named) and UNRESOLVED (the heading
 * it named is not in the document). The third is the one a boolean cannot
 * hold, and it is the state that makes this a state machine rather than a
 * flag:
 *
 *   UNDECIDABLE — the claim cannot be settled either way, and answering
 *   anyway is how a comment gets silently re-anchored onto a DIFFERENT
 *   heading that happens to share a slug with the one it referred to. The
 *   store can see that "something is at this id now" and that "it is not the
 *   thing that was there"; it cannot see which of the two explanations is
 *   true, and neither "resolved" nor "unresolved" is a true statement about
 *   it. `None` and `False` collapse here, and the collapse is the bug: a
 *   reader that treats "undecidable" as "unresolved" hides a live comment,
 *   and a reader that treats it as "resolved" shows a comment attached to
 *   the wrong heading.
 *
 * Reachable four ways, and each is a separate test:
 *   - the document will not parse, so the caller passes `judgeable = false`;
 *   - the anchor's id is taken over by a heading with DIFFERENT text, and the
 *     original heading is neither at the id nor anywhere else in the
 *     document (the slug collision, which is the failure this exists for);
 *   - the row's verdict was recorded against a `content_hash` the artifact
 *     no longer has, so a crash between the re-index and the pass left a
 *     claim about bytes that are gone;
 *   - the row exists and nothing has judged it yet.
 *
 * UNDECIDABLE is ABSORBING. A pass does not clear it, because a pass is the
 * same computation that could not settle it the first time. What clears it
 * is a human: `kbc_store_set_comment_resolved`, or a re-anchor. That is a
 * deliberate choice against "retry until it agrees" — a store that keeps
 * re-asking a question it has already failed to answer never reaches a fixed
 * point, and a fixed point is the whole point of the exercise. */

/* `section:` is the only prefix kb-c's store accepts on an anchor
 * (store.h:226), and it is stripped rather than searched for: an anchor
 * naming `section:x` and one naming `x` are different claims about a
 * document, and comparing the whole string would collapse them. This is the
 * same rule httpd.c applies before it looks an id up, restated here because
 * the store is where the comparison is made and the two must not drift. */
static const char *anchor_element_id(const char *anchor) {
  static const char kSection[] = "section:";
  if (strncmp(anchor, kSection, sizeof kSection - 1) == 0) {
    return anchor + sizeof kSection - 1;
  }
  return anchor;
}


/* ------------------------------------------------------------ plumbing --- */

static kbc_status sql_fail(kbc_err *err, const kbc_store *s, const char *what,
                           int rc) {
  char *msg = sqlite3_mprintf("%s: %s (rc=%d)", what, sqlite3_errmsg(s->db), rc);
  kbc_status st = kbc_err_set(err, KBC_ERR_SQL, "%s", msg ? msg : what);
  sqlite3_free(msg);
  return st;
}

/* What a bare SQLITE_CONSTRAINT actually was.
 *
 * The connection leaves extended result codes OFF, so a failed step hands back
 * the primary code 19 whatever broke: the unique index, the foreign key, a
 * NOT NULL, a CHECK. Guessing one of those is how "chunk <id>/0: duplicate
 * ord" came to be reported for a write that had nothing to do with ords — the
 * one constraint chunks can hit besides the primary key is the foreign key
 * onto artifacts, and it fires whenever another writer removes the document
 * between the artifact upsert and this call. The extended code is still
 * available on demand, so the diagnosis is read rather than assumed.
 *
 * `fkey` names the table the foreign key points at, and is NULL where the
 * table has no foreign key to blame. Anything this does not recognise falls
 * through to sql_fail, which reports sqlite's own words.
 *
 * The status is left KBC_ERR_CONFLICT on every branch: a caller decides what a
 * lost race means, and this layer only knows what broke. */
static kbc_status constraint_fail(kbc_err *err, const kbc_store *s,
                                  const char *what, const char *fkey) {
  switch (sqlite3_extended_errcode(s->db)) {
    case SQLITE_CONSTRAINT_PRIMARYKEY:
      return kbc_err_set(err, KBC_ERR_CONFLICT, "%s: a row with this key is "
                                                "already there",
                         what);
    case SQLITE_CONSTRAINT_UNIQUE:
      return kbc_err_set(err, KBC_ERR_CONFLICT,
                         "%s: a row with this value is already there", what);
    case SQLITE_CONSTRAINT_FOREIGNKEY:
      return kbc_err_set(err, KBC_ERR_CONFLICT,
                         "%s: no such row in %s — the row it references is "
                         "gone",
                         what, fkey ? fkey : "the referenced table");
    default:
      return sql_fail(err, s, what, sqlite3_errcode(s->db));
  }
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

  /* AFTER the ladder, and on EVERY open rather than only on a version change:
   * a volume does not have to be migrated to become inconsistent. A writer
   * with `foreign_keys` off can orphan a row at any time, and the version
   * table says nothing about that — it records what RAN, not what is
   * consistent. Gating this on `have < BINARY_EPOCH` would check a volume
   * once, on the open that first migrates it, and never again for the life of
   * the install, which is the state most opens are.
   *
   * On kb-c's own handle, where `foreign_keys` is ON, so the pragma means what
   * it says. It reports and returns: no status can turn a violation into a
   * failed open, and nothing here writes or repairs. */
  report_foreign_key_violations(s, cfg->db_path);

  /* The stale set is rebuilt from the graph on EVERY open, not only on a
   * version change, for the same reason the FK check above is: a volume does
   * not have to be migrated to become inconsistent with its own documents,
   * and "the document was re-indexed" happens on every save rather than on
   * every upgrade. Gating this on `have < BINARY_EPOCH` would leave every
   * install that already migrated permanently describing the graph as it was
   * when it last passed — which is the drift SCHEMA_V13 exists to prevent.
   *
   * It is a REPAIR, and unlike the FK check it writes — so unlike the FK check
   * it is allowed to fail the open. A daemon that started holding verdicts
   * about documents that no longer exist would serve a wrong ANSWER, and
   * "warn and continue" is the wrong posture for a wrong answer; refusing to
   * start is the loud version of the epoch guard, and it is recoverable in a
   * way a silently wrong anchor is not. */
  kbc_err rebuilt_err;
  kbc_err_reset(&rebuilt_err);
  int64_t rebuilt = 0;
  if (kbc_failed(kbc_store_reconcile_anchors(s, &rebuilt, &rebuilt_err))) {
    kbc_store_close(s);
    return NULL;
  }
  if (rebuilt > 0) {
    KBC_LOGI("store: rebuilt %lld anchor verdict(s) against the current"
             " documents in %s",
             (long long)rebuilt, cfg->db_path);
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
   * NOT EXISTS / OR IGNORE) — EXCEPT v12's ADD COLUMN, which fails if the
   * column is already there and is safe only because it shares that
   * transaction: a failed step rolls the ALTER back with the version row, and
   * a volume that recorded 12 skips the step entirely.
   *
   * THE ONE STATE THIS DOES NOT SURVIVE: a volume whose schema_version has
   * been rolled back by hand below what its schema actually is. The ladder
   * trusts that table — it is the only record of what ran — so v12 is re-run
   * against a table that already has the column and the open fails with
   * SQLite's "duplicate column name". That is loud rather than silent, it is
   * corruption the ladder itself cannot produce (one transaction adds the
   * column and records the version together), and repairing it would mean a
   * per-step "is this already applied" probe for a state that only a damaged
   * file or a hand-edited table reaches. */
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
    if (step == SQLITE_CONSTRAINT) {
      /* "constraint" on its own told an operator nothing. The only constraint
       * left after the (corpus, path) probe above is the one another writer
       * can win between that probe and this statement. */
      char what[KBC_MAX_ID_LEN + 16];
      (void)snprintf(what, sizeof what, "artifact %s", a->id);
      st = constraint_fail(err, s, what, NULL);
    } else if (step != SQLITE_DONE) {
      st = sql_fail(err, s, "upsert artifact", step);
    }
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

/* The documents that link HERE: the other direction of the edges table.
 *
 * DISTINCT because a source that links to the same target from two places is
 * ONE backlink, and a backlinks surface that counted paragraphs would be
 * wrong. The primary key already makes (corpus, src_path, dst_path) unique, so
 * this cannot collapse two DIFFERENT sources into one row — the dedup here is
 * about one source appearing once, not about losing sources.
 *
 * `edges_dst` on (corpus, dst_path) is the index that makes this a seek, and
 * a zero-row answer is the ordinary case, not a miss: nothing linking here is
 * KBC_OK with a zero-length list, because the original serves it as a 200 with
 * an empty array (links.rs:325) and a 404 would make an ordinary document
 * look missing. So NOTFOUND is never returned from here. */
kbc_status kbc_store_list_backlinks(kbc_store *s, const char *corpus,
                                    const char *path, kbc_strlist *out,
                                    kbc_err *err) {
  if (s == NULL || out == NULL)
    return kbc_err_set(err, KBC_ERR_INVALID, "list_backlinks: null argument");
  kbc_status rc = require_text(err, "backlinks corpus", corpus, 255);
  if (rc == KBC_OK)
    rc = require_text(err, "backlinks path", path, KBC_MAX_PATH_LEN);
  if (rc != KBC_OK) return rc;

  lock(s);
  sqlite3_stmt *st = NULL;
  rc = prepare(err, s,
               "SELECT DISTINCT src_path FROM edges"
               " WHERE corpus = ?1 AND dst_path = ?2"
               " ORDER BY src_path;",
               &st);
  if (rc == KBC_OK) rc = bind_text(err, s, st, 1, corpus);
  if (rc == KBC_OK) rc = bind_text(err, s, st, 2, path);
  if (rc != KBC_OK) {
    (void)finalize(err, s, st, rc);
    unlock(s);
    return rc;
  }
  for (;;) {
    int step = sqlite3_step(st);
    if (step == SQLITE_DONE) break;
    if (step != SQLITE_ROW) {
      rc = sql_fail(err, s, "list backlinks: step", step);
      break;
    }
    const char *src = (const char *)sqlite3_column_text(st, 0);
    if (src == NULL) {
      rc = kbc_err_set(err, KBC_ERR_INTERNAL, "list backlinks: null src_path");
      break;
    }
    if (out->len >= KBC_MAX_CORPORA) {
      rc = kbc_err_set(err, KBC_ERR_CONFLICT, "backlinks: more than %u",
                       (unsigned)KBC_MAX_CORPORA);
      break;
    }
    rc = kbc_strlist_push(out, src);
    if (rc != KBC_OK) break;
  }
  kbc_status fin = finalize(err, s, st, rc);
  if (rc == KBC_OK) rc = fin;
  unlock(s);
  return rc;
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


/* REPLACE, not merge — EXCEPT for the two keys the verdict owns.
 *
 * A document is re-ingested with the facets it declares NOW, so a tag the
 * author removed stops matching on the next reindex instead of living on as
 * a row nothing will ever clean up. ONE transaction, for the reason
 * replace_edges has: a half-applied facet set is a filter matching a
 * document that never existed.
 *
 * The exception is `kb-tags = status-approved` / `status-changes-requested`,
 * which are NOT the document's to declare. They are the display shortcut the
 * original mirrors off a review verdict (review.rs, invariant #12: "this
 * field stays the verdict of record; the tag is derived, never the other way
 * around"), and `kbc_store_set_verdict` is their only writer. Deleting them
 * here would let every reindex silently un-approve a document — a facet
 * filter that answers "which documents are approved" would go empty while
 * `verdicts` still says the document was approved, which is precisely the
 * "a filter that matches nothing must never look like a filter that was
 * ignored" failure `kbc_store_docs_with_meta` exists to avoid.
 *
 * This is the same discipline `kbc_store_put_source` applies to `paused`:
 * derived state is not an ingest's to undo. The two values are named
 * literally rather than matched with LIKE, because the primary key is
 * (corpus, path, key, value) and this stays a seek. */
/* The `status-*` facet values a derived verdict owns, named once so the
 * writer and the thing that refuses to delete them cannot drift. */
static const char *const VERDICT_TAG_VALUES[2] = {
    "status-approved", "status-changes-requested"};
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
               "DELETE FROM doc_metas WHERE corpus = ?1 AND path = ?2"
               " AND NOT (key = 'kb-tags' AND (value = 'status-approved'"
               " OR value = 'status-changes-requested'));",
               &del);
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

/* The facets a document declares, as two PARALLEL lists. The pairing is the
 * whole contract: index i of `keys` is the key of index i of `values`, which
 * is why the rows are pushed in lockstep and a failed push aborts rather than
 * continuing — a list that drifted out of step would report a key against
 * somebody else's value, and a caller cannot detect that from the shape.
 *
 * ORDER BY key, then value. The key alone is NOT a total order: `kb-tags` is
 * one row per value of a multi-valued facet, so a document declaring
 * `kb-tags="search, index"` has two rows under the same key, and without the
 * tiebreak their order is whatever the index happened to return. Two daemons
 * that ingested the same file would then disagree on ORDER while agreeing on
 * every value — the diff nobody can read.
 *
 * A document with no facets is KBC_OK and two empty lists, NOT
 * KBC_ERR_NOTFOUND. "This document declares no facets" is the answer most
 * documents give, and returning an error for it would make a caller
 * distinguish the ordinary case by catching a failure. Nothing here consults
 * whether the document or the corpus exists either: doc_metas is path-keyed
 * and carries no foreign key, so a path nobody has ingested reads as zero
 * rows, which is the same true statement as "this path declares no facets".
 */
kbc_status kbc_store_get_metas(kbc_store *s, const char *corpus,
                               const char *path, kbc_strlist *keys,
                               kbc_strlist *values, kbc_err *err) {
  if (s == NULL || keys == NULL || values == NULL)
    return kbc_err_set(err, KBC_ERR_INVALID, "get_metas: null argument");
  kbc_status st = require_text(err, "meta corpus", corpus, 255);
  if (st == KBC_OK)
    st = require_text(err, "meta path", path, KBC_MAX_PATH_LEN);
  if (st != KBC_OK) return st;

  lock(s);
  sqlite3_stmt *q = NULL;
  st = prepare(err, s,
               "SELECT key, value FROM doc_metas"
               " WHERE corpus = ?1 AND path = ?2"
               " ORDER BY key ASC, value ASC;",
               &q);
  if (st == KBC_OK) st = bind_text(err, s, q, 1, corpus);
  if (st == KBC_OK) st = bind_text(err, s, q, 2, path);
  for (;;) {
    const int step = (st == KBC_OK) ? sqlite3_step(q) : SQLITE_DONE;
    if (step == SQLITE_DONE) break;
    if (step != SQLITE_ROW) {
      st = sql_fail(err, s, "get metas: step", step);
      break;
    }
    const char *key = (const char *)sqlite3_column_text(q, 0);
    const char *value = (const char *)sqlite3_column_text(q, 1);
    /* Both are NOT NULL in the schema, so a NULL here is a damaged file
     * rather than a state any writer can produce. Refuse it: pushing a NULL
     * is rejected downstream, and reporting a key with no value would break
     * the pairing silently instead of loudly. */
    if (key == NULL || value == NULL) {
      st = kbc_err_set(err, KBC_ERR_INTERNAL,
                       "get metas: null key or value for %s/%s", corpus, path);
      break;
    }
    st = kbc_strlist_push(keys, key);
    if (st != KBC_OK) break;
    st = kbc_strlist_push(values, value);
    if (st != KBC_OK) break;
  }
  kbc_status fin = finalize(err, s, q, st);
  if (st == KBC_OK) st = fin;
  unlock(s);
  return st;
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
      if (step == SQLITE_CONSTRAINT) {
        /* Read BEFORE the reset: reset re-reports the step's error as its own,
         * and the extended code is only the last failed call's. The context
         * names the document because chunks.doc_id IS the artifact id, and
         * "artifacts" is the only table this key can point at. */
        char what[KBC_MAX_ID_LEN + 32];
        (void)snprintf(what, sizeof what, "chunk %s/%u", chunks[i].doc_id,
                       chunks[i].ord);
        rc = constraint_fail(err, s, what, "artifacts");
      } else if (step != SQLITE_DONE) {
        rc = sql_fail(err, s, "insert chunk", step);
      }
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

/* The documents that HAVE comments — the distinct doc_ids the comments table
 * names, which is a set and not a count.
 *
 * NOT filtered by corpus, and that is forced by the schema rather than
 * chosen: comments has carried doc_id and nothing else since v1, so there is
 * no corpus column to filter on and pretending otherwise would mean a scan
 * plus a per-row lookup — the very shape this function exists to remove. The
 * caller that needs the corpus recovers it from the artifact row, which it
 * needs anyway to name the event's `kb` and `source_relative`.
 *
 * `comments_doc` on doc_id makes this a loose index scan over the comment
 * rows, so the cost is proportional to the comments that EXIST and not to the
 * size of the corpus — the property the anchor re-evaluation pass needs. */
kbc_status kbc_store_list_comment_docs(kbc_store *s, kbc_strlist *out,
                                       kbc_err *err) {
  if (s == NULL || out == NULL)
    return kbc_err_set(err, KBC_ERR_INVALID,
                       "list_comment_docs: null argument");

  lock(s);
  sqlite3_stmt *st = NULL;
  kbc_status rc = prepare(err, s,
                           "SELECT DISTINCT doc_id FROM comments"
                           " ORDER BY doc_id;",
                           &st);
  if (rc != KBC_OK) {
    unlock(s);
    return rc;
  }
  for (;;) {
    int step = sqlite3_step(st);
    if (step == SQLITE_DONE) break;
    if (step != SQLITE_ROW) {
      rc = sql_fail(err, s, "list comment docs: step", step);
      break;
    }
    const char *d = (const char *)sqlite3_column_text(st, 0);
    if (d == NULL) {
      rc = kbc_err_set(err, KBC_ERR_INTERNAL,
                       "list comment docs: null doc_id");
      break;
    }
    if (out->len >= KBC_MAX_HITS) {
      rc = kbc_err_set(err, KBC_ERR_CONFLICT,
                       "comment docs: more than %u", (unsigned)KBC_MAX_HITS);
      break;
    }
    rc = kbc_strlist_push(out, d);
    if (rc != KBC_OK) break;
  }
  kbc_status fin = finalize(err, s, st, rc);
  if (rc == KBC_OK) rc = fin;
  unlock(s);
  return rc;
}

/* ========================================================= anchor state ==
 *
 * The three-state machine, the persisted stale set it drives, and the
 * open-time rebuild that keeps the set describing the graph that exists.
 * SCHEMA_V13 carries the persist-vs-recompute decision and the argument for
 * it; this is the code that implements it.
 *
 * WHO OWNS THE DOCUMENT'S HEADINGS. Not this file. The store has `chunks`,
 * whose rows carry an ord and a text and NO slug (store.c:2502 says so, and
 * `kbc_store_list_chunks` synthesises `b<ord>` for the same reason), so the
 * store cannot see a heading slug and must not pretend to. The caller parsed
 * the document and hands the verdict's INPUTS over as `kbc_anchor_heading[]`
 * — the ids the document exposes, in document order, each with the heading
 * text behind it. BORROWED for the call; nothing here retains either field.
 *
 * WHY THE ORDER OF THAT ARRAY IS PART OF THE CONTRACT. `resolved_ord` is a
 * position in it, and a position only means something while the order is the
 * document's. Feeding the same headings in a different order produces a
 * different ord, which is the caller's bug and not a store fallback: there is
 * no order to fall back to, because the store has never seen the document.
 */

/* "Stale" for the EVENT set, and deliberately not "state != RESOLVED is
 * resolved". An UNDECIDABLE anchor is as un-actionable as an unresolved one —
 * a subscriber needs to know about both — and a set that dropped it would be
 * the two-state collapse this section exists to refuse. What the set is NOT
 * for is deciding that a comment is fine: nothing here reads `stale` as
 * `resolved`. */
static bool anchor_is_stale(kbc_anchor_state st) {
  return st != KBC_ANCHOR_RESOLVED;
}

/* Where `id` sits in the caller's heading table, or -1. The ord is the
 * index in the array the caller passed, so "moved" means "the same id is
 * still exposed but at a different position", which is the only sense of
 * "moved" a caller that never told us the old order can have. */
static int64_t anchor_ordinal(const kbc_anchor_heading *h, size_t n,
                              const char *id) {
  for (size_t i = 0; i < n; i++) {
    if (h[i].id != NULL && strcmp(h[i].id, id) == 0) return (int64_t)i;
  }
  return -1;
}

/* The ord of the heading whose text is `text`, or -1 — and -1 for AMBIGUITY
 * as well as for absence, which is the whole point.
 *
 * A slug collision leaves the original heading's text somewhere else in the
 * document exactly ONCE if the heading merely moved, and TWICE or NINE TIMES
 * if the text was duplicated. "Follow the heading" is only a sound
 * conclusion when there is exactly one place it could have gone: with two
 * candidates the store cannot tell which one the comment meant, and picking
 * either is the silent re-anchor. So ambiguity and absence share a return
 * value and the caller turns both into UNDECIDABLE. */
static int64_t anchor_text_ordinal(const kbc_anchor_heading *h, size_t n,
                                   const char *text) {
  if (text == NULL || text[0] == '\0') return -1;
  int64_t found = -1;
  for (size_t i = 0; i < n; i++) {
    const char *t = h[i].text != NULL ? h[i].text : "";
    if (strcmp(t, text) != 0) continue;
    if (found >= 0) return -1; /* two candidates: refuse to choose */
    found = (int64_t)i;
  }
  return found;
}

/* ONE comment's verdict, from the caller's heading table and the row the
 * last pass left behind. `prev_state` is -1 when there is no row yet, which
 * is how "never judged" says so without a fourth state.
 *
 * The order of the tests is the argument, so it is worth stating:
 *
 *  1. NOT JUDGEABLE — the caller says the document has no trustworthy
 *     heading table (it would not parse, or the bytes were not available).
 *     UNDECIDABLE, always. Reporting UNRESOLVED here would flag every
 *     comment on a document that merely failed to parse, which is the one
 *     answer that is wrong in both directions at once.
 *
 *  2. THE ID IS STILL EXPOSED, and either nothing has judged this anchor
 *     before (`prev_state < 0`, so there is no prior claim to contradict)
 *     or the text behind it is the text the comment was made on. RESOLVED.
 *
 *  3. THE ID IS STILL EXPOSED but the text CHANGED. This is the slug
 *     collision: something else now holds the id the comment named. Try to
 *     follow the original heading by its text; if it is uniquely somewhere
 *     else, the comment FOLLOWS it there and stays RESOLVED at the new ord.
 *     If it is not there, or it is there more than once, the claim cannot be
 *     settled — UNDECIDABLE. The two-state machine's answer here is
 *     RESOLVED, and that answer is the bug.
 *
 *  4. THE ID IS GONE. Try to follow by text first — a renamed heading is a
 *     MOVED heading, and the comment follows it. Unique match: RESOLVED at
 *     the new ord. No match, or an ambiguous one: the heading the comment
 *     referred to is not in the document, which is the one case that IS a
 *     clean negative. UNRESOLVED.
 *
 * `resolves_to_out` and `ord_out` receive what a READER should point at,
 * which is the follow target and not `anchor`. The user's claim is never
 * rewritten; a reader that wants the new home asks the store. */
static kbc_anchor_state anchor_judge(const kbc_anchor_heading *h, size_t n,
                                    bool judgeable, const char *claim,
                                    const char *prev_text, int prev_state,
                                    const char **resolves_to_out,
                                    int64_t *ord_out) {
  *resolves_to_out = "";
  *ord_out = -1;
  if (!judgeable) return KBC_ANCHOR_UNDECIDABLE;

  const char *id = anchor_element_id(claim);
  int64_t at = anchor_ordinal(h, n, id);
  if (at >= 0) {
    const char *here = h[at].text != NULL ? h[at].text : "";
    if (prev_state < 0 || prev_text == NULL || prev_text[0] == '\0' ||
        strcmp(prev_text, here) == 0) {
      *resolves_to_out = id;
      *ord_out = at;
      return KBC_ANCHOR_RESOLVED;
    }
    /* The id is taken over by a different heading. The original may still be
     * in the document under a new id; a unique text match is the proof that
     * it moved rather than vanished, and it is the ONLY proof available. */
    int64_t moved = anchor_text_ordinal(h, n, prev_text);
    if (moved >= 0) {
      *resolves_to_out = h[moved].id;
      *ord_out = moved;
      return KBC_ANCHOR_RESOLVED;
    }
    return KBC_ANCHOR_UNDECIDABLE;
  }

  int64_t moved = anchor_text_ordinal(h, n, prev_text);
  if (moved >= 0) {
    *resolves_to_out = h[moved].id;
    *ord_out = moved;
    return KBC_ANCHOR_RESOLVED;
  }
  return KBC_ANCHOR_UNRESOLVED;
}

/* One verdict, one statement. An upsert rather than an insert-or-update pair
 * because the two would disagree about a row that appeared between them, and
 * because a comment is judged once per pass by definition — so "is there a
 * row" is exactly the question the caller should not be answering. */
static kbc_status anchor_write(kbc_err *err, kbc_store *s, const char *cid,
                               const char *doc_id, const char *claim,
                               kbc_anchor_state state, const char *where,
                               int64_t ord, int64_t hash) {
  sqlite3_stmt *q = NULL;
  kbc_status st = prepare(
      err, s,
      "INSERT INTO comment_anchors(comment_id, doc_id, anchor, anchor_text,"
      " state, resolves_to, resolved_ord, content_hash, judged_at)"
      " VALUES(?1,?2,?3,'',?4,?5,?6,?7,?8)"
      " ON CONFLICT(comment_id) DO UPDATE SET"
      "  doc_id=excluded.doc_id, anchor=excluded.anchor,"
      "  state=excluded.state, resolves_to=excluded.resolves_to,"
      "  resolved_ord=excluded.resolved_ord,"
      "  content_hash=excluded.content_hash, judged_at=excluded.judged_at;",
      &q);
  if (st == KBC_OK) st = bind_text(err, s, q, 1, cid);
  if (st == KBC_OK) st = bind_text(err, s, q, 2, doc_id);
  if (st == KBC_OK) st = bind_text(err, s, q, 3, claim);
  if (st == KBC_OK) st = bind_i64(err, s, q, 4, (int64_t)state);
  if (st == KBC_OK) st = bind_text(err, s, q, 5, where);
  if (st == KBC_OK) st = bind_i64(err, s, q, 6, ord);
  if (st == KBC_OK) st = bind_i64(err, s, q, 7, hash);
  if (st == KBC_OK) st = bind_i64(err, s, q, 8, kbc_now_ns() / 1000000000);
  if (st == KBC_OK) {
    const int step = sqlite3_step(q);
    if (step != SQLITE_DONE) st = sql_fail(err, s, "judge anchor", step);
  }
  kbc_status fin = finalize(err, s, q, st);
  return st != KBC_OK ? st : fin;
}

/* Records the heading text behind a resolution, ONCE. The text is the
 * identity the NEXT pass compares against, and it is written only when the
 * row has none: overwriting it with whatever the document says now would
 * make a collision undetectable on the very pass after it happens, which is
 * the one pass where detecting it is the entire job. The `anchor_text = ''`
 * in the predicate is what makes "once" a property of the row rather than a
 * promise in a comment. */
static kbc_status anchor_remember(kbc_err *err, kbc_store *s, const char *cid,
                                  const char *text) {
  sqlite3_stmt *q = NULL;
  kbc_status st =
      prepare(err, s,
              "UPDATE comment_anchors SET anchor_text = ?2"
              " WHERE comment_id = ?1 AND anchor_text = '';",
              &q);
  if (st == KBC_OK) st = bind_text(err, s, q, 1, cid);
  if (st == KBC_OK) st = bind_text(err, s, q, 2, text != NULL ? text : "");
  if (st == KBC_OK) {
    const int step = sqlite3_step(q);
    if (step != SQLITE_DONE) st = sql_fail(err, s, "remember anchor", step);
  }
  kbc_status fin = finalize(err, s, q, st);
  return st != KBC_OK ? st : fin;
}

/* Judges every OPEN comment on one document and writes the verdicts, in ONE
 * transaction, and hands back the edges.
 *
 * ONE TRANSACTION, and that is not tidiness. The set and the graph it
 * describes are committed together, so there is no window in which a crash
 * can leave a verdict that no pass would ever produce — the property
 * SCHEMA_V13 argues for and `kbc_store_reconcile_anchors` then makes
 * checkable on reopen. The original gets the same property the hard way, by
 * writing its sidecar once per artifact and accepting the window in between
 * (indexer.rs:3131-3145).
 *
 * `c.resolved = 0` is `is_open()` (indexer.rs:3035): the original does not
 * judge a resolved comment, and neither does this. A resolved comment keeps
 * whatever verdict it had, which is what makes re-opening it report the
 * state the subscriber already saw rather than a fresh stale event.
 *
 * `content_hash` is read from the ARTIFACT ROW inside the same transaction,
 * never taken from the caller, so a verdict can never claim to describe
 * bytes the store itself does not believe are current. */
kbc_status kbc_store_judge_anchors(kbc_store *s, const char *doc_id,
                                   const kbc_anchor_heading *headings,
                                   size_t n, bool judgeable, kbc_arena *a,
                                   kbc_anchor_judgement **out, size_t *n_out,
                                   kbc_err *err) {
  if (s == NULL || doc_id == NULL || out == NULL || n_out == NULL)
    return kbc_err_set(err, KBC_ERR_INVALID, "judge_anchors: null argument");
  if (headings == NULL && n > 0)
    return kbc_err_set(err, KBC_ERR_INVALID, "judge_anchors: null headings");
  *out = NULL;
  *n_out = 0;
  kbc_status st = require_text(err, "anchor doc id", doc_id, KBC_MAX_ID_LEN);
  if (st != KBC_OK) return st;

  lock(s);
  st = exec_plain(err, s, "BEGIN IMMEDIATE;");
  if (st != KBC_OK) {
    unlock(s);
    return st;
  }

  int64_t hash = 0;
  int64_t at = 0;
  kbc_status rc =
      count_query(err, s, "SELECT content_hash FROM artifacts WHERE id = ?1;",
                  doc_id, &hash);
  if (rc != KBC_OK) goto done;
  rc = count_query(err, s, "SELECT COUNT(*) FROM comments"
                          " WHERE doc_id = ?1 AND resolved = 0;",
                  doc_id, &at);
  if (rc != KBC_OK) goto done;

  size_t total = at > 0 ? (size_t)at : 0u;
  size_t seen = 0;
  kbc_anchor_judgement *judged = NULL;
  if (total > 0) {
    judged = kbc_arena_calloc(a, total, sizeof(*judged));
    if (judged == NULL) {
      rc = kbc_err_set(err, KBC_ERR_NOMEM, "judge anchors: %zu comments",
                       total);
      goto done;
    }
  }

  sqlite3_stmt *q = NULL;
  rc = prepare(err, s,
               "SELECT c.id, c.anchor, a.anchor_text, a.state"
               " FROM comments c"
               " LEFT JOIN comment_anchors a ON a.comment_id = c.id"
               " WHERE c.doc_id = ?1 AND c.resolved = 0"
               " ORDER BY c.id;",
               &q);
  if (rc == KBC_OK) rc = bind_text(err, s, q, 1, doc_id);
  if (rc == KBC_OK) {
    for (;;) {
      const int step = sqlite3_step(q);
      if (step == SQLITE_DONE) break;
      if (step != SQLITE_ROW) {
        rc = sql_fail(err, s, "judge anchors: step", step);
        break;
      }
      const char *cid = (const char *)sqlite3_column_text(q, 0);
      const char *claim = (const char *)sqlite3_column_text(q, 1);
      const char *prev_text = (const char *)sqlite3_column_text(q, 2);
      /* `comments.id` is a NOT NULL PRIMARY KEY and `comments.anchor` is NOT
       * NULL, so neither of these can be NULL on a row this SELECT produced
       * — but the loop below binds both, and a NULL there is a bind failure
       * halfway through a transaction rather than a statement that never
       * ran. Refusing with the column named is the difference between a
       * diagnosable refusal and a generic SQL error. */
      if (cid == NULL || claim == NULL) {
        rc = kbc_err_set(err, KBC_ERR_INTERNAL,
                         "judge anchors: comment row on %s has a null %s",
                         doc_id, cid == NULL ? "id" : "anchor");
        break;
      }
      const int prev_state = sqlite3_column_type(q, 3) == SQLITE_NULL
                                 ? -1
                                 : sqlite3_column_int(q, 3);
      const char *where = "";
      int64_t ord = -1;
      kbc_anchor_state ns =
          anchor_judge(headings, n, judgeable, claim != NULL ? claim : "",
                      prev_text, prev_state, &where, &ord);
      const bool was_stale = prev_state >= 0 && anchor_is_stale(
                                                  (kbc_anchor_state)prev_state);
      rc = anchor_write(err, s, cid, doc_id, claim != NULL ? claim : "", ns,
                        where, ord, hash);
      if (rc != KBC_OK) break;
      /* The remembered text is only written on the FIRST resolution. It is
       * the identity the next pass compares against, and overwriting it with
       * whatever the document says now is how a collision becomes invisible
       * one pass after it happens. */
      /* `ord < n` is not a hope: every RESOLVED path in `anchor_judge` sets
       * it from `anchor_ordinal` or `anchor_text_ordinal`, and both return
       * either -1 or an index they themselves produced by walking `0..n-1`.
       * The `ord >= 0` half is what makes the index expression legal, and
       * the two together are why this needs no range check of its own. */
      if (rc == KBC_OK && ns == KBC_ANCHOR_RESOLVED && ord >= 0 &&
          (prev_text == NULL || prev_text[0] == '\0')) {
        rc = anchor_remember(err, s, cid, headings[ord].text);
      }
      if (rc != KBC_OK) break;
      judged[seen].comment_id = kbc_arena_strdup(a, cid);
      judged[seen].state = ns;
      judged[seen].ord = ord;
      judged[seen].transitioned = anchor_is_stale(ns) != was_stale;
      seen++;
      if (seen == total) break;
    }
  }
  kbc_status fin = finalize(err, s, q, rc);
  if (rc == KBC_OK) rc = fin;

done:
  if (rc == KBC_OK) rc = exec_plain(err, s, "COMMIT;");
  else rollback(s);
  unlock(s);
  if (rc != KBC_OK) return rc;
  *out = judged;
  *n_out = seen;
  return KBC_OK;
}

/* ONE comment's persisted verdict, or NOTFOUND — which is not the same as
 * UNDECIDABLE and must not be read as it.
 *
 * NOTFOUND means no pass has ever judged this comment, which is a question
 * that has not been asked. UNDECIDABLE means it was asked and could not be
 * answered. A caller that maps NOTFOUND onto UNDECIDABLE is asserting that
 * somebody tried; a caller that maps it onto RESOLVED is asserting nobody
 * objected. Both are claims the store cannot make on the caller's behalf, so
 * they are two different returns.
 *
 * The state is read back RAW, not re-checked against the artifact's current
 * `content_hash`. That check is `kbc_store_reconcile_anchors`'s job and it
 * runs on every open, so a row that reaches this function has already been
 * reconciled against the graph that exists. Re-checking here as well would
 * be a second answer to a question with one owner. */
kbc_status kbc_store_get_anchor(kbc_store *s, kbc_arena *a,
                                const char *comment_id, kbc_anchor_row *out,
                                kbc_err *err) {
  if (s == NULL || a == NULL || out == NULL || comment_id == NULL)
    return kbc_err_set(err, KBC_ERR_INVALID, "get_anchor: null argument");
  kbc_status st = require_text(err, "anchor comment id", comment_id,
                               KBC_MAX_ID_LEN);
  if (st != KBC_OK) return st;

  lock(s);
  sqlite3_stmt *q = NULL;
  st = prepare(err, s,
               "SELECT doc_id, anchor, resolves_to, resolved_ord, state"
               " FROM comment_anchors WHERE comment_id = ?1;",
               &q);
  if (st == KBC_OK) st = bind_text(err, s, q, 1, comment_id);
  if (st == KBC_OK) {
    const int step = sqlite3_step(q);
    if (step == SQLITE_ROW) {
      out->doc_id = col_str(a, q, 0);
      out->anchor = col_str(a, q, 1);
      out->resolves_to = col_str(a, q, 2);
      out->resolved_ord = (int64_t)sqlite3_column_int64(q, 3);
      out->state = (kbc_anchor_state)sqlite3_column_int(q, 4);
    } else if (step == SQLITE_DONE) {
      st = kbc_err_set(err, KBC_ERR_NOTFOUND,
                       "anchor for comment %s: never judged", comment_id);
    } else {
      st = sql_fail(err, s, "get anchor: step", step);
    }
  }
  kbc_status fin = finalize(err, s, q, st);
  unlock(s);
  return st != KBC_OK ? st : fin;
}

/* The stale set, as a PAGE.
 *
 * `ORDER BY judged_at DESC, comment_id ASC LIMIT ?1` and the count-then-fetch
 * shape are `kbc_store_list_corkboard`'s exactly, and for the same reason:
 * this is the original's answer to "how many results does this page show"
 * (sqlite.rs:2831 for the corkboard, the same `LIMIT n` over one ordered
 * query here), and a page is a page. `LIMIT n` takes the n most RECENT
 * verdicts, so a comment judged long ago is not among them, and its ABSENCE
 * from this page is not evidence that it is resolved. Anything that needs
 * one specific comment's verdict reads it by id.
 *
 * `doc_id` NULL is every document, which is the shape `GET /anchors/stale`
 * wants and the shape a whole-kb dashboard wants. `stale_only` false asks
 * for everything, which is the shape a caller reconciling its own cache
 * wants; there is no third option, because "the resolved ones" is the
 * complement of the same predicate and a second query would be a second
 * place for the two to disagree. */
kbc_status kbc_store_list_anchors(kbc_store *s, kbc_arena *a, const char *doc_id,
                                  bool stale_only, size_t limit,
                                  kbc_anchor_row **out, size_t *n_out,
                                  kbc_err *err) {
  if (s == NULL || a == NULL || out == NULL || n_out == NULL)
    return kbc_err_set(err, KBC_ERR_INVALID, "list_anchors: null argument");
  if (limit > KBC_MAX_HITS) limit = KBC_MAX_HITS;
  *out = NULL;
  *n_out = 0;
  if (limit == 0) return KBC_OK;
  if (doc_id != NULL && doc_id[0] == '\0')
    return kbc_err_set(err, KBC_ERR_INVALID, "list_anchors: empty doc id");

  /* ONE filter clause for both statements, and it is spelled out in full
   * rather than composed from a `where` and an `and`: composing them puts
   * `AND doc_id = ?1` after a table name whenever `stale_only` is false, and
   * `FROM comment_anchors AND doc_id = ?1` is not a query. The two shapes
   * below are the only two there are, and each is a literal so neither can
   * be assembled wrongly. */
  const char *filter;
  if (stale_only && doc_id != NULL)
    filter = " WHERE state <> 1 AND doc_id = ?1";
  else if (stale_only)
    filter = " WHERE state <> 1";
  else if (doc_id != NULL)
    filter = " WHERE doc_id = ?1";
  else
    filter = "";
  const int limit_ord = doc_id != NULL ? 2 : 1;
  char count_sql[128];
  char list_sql[288];
  (void)snprintf(count_sql, sizeof count_sql,
                 "SELECT COUNT(*) FROM comment_anchors%s;", filter);
  (void)snprintf(list_sql, sizeof list_sql,
                 "SELECT comment_id, doc_id, anchor, resolves_to, resolved_ord,"
                 " state FROM comment_anchors%s"
                 " ORDER BY judged_at DESC, comment_id ASC LIMIT ?%d;",
                 filter, limit_ord);

  lock(s);
  void *block = NULL;
  size_t n = 0;
  kbc_status st =
      page_block(err, s, a, count_sql, doc_id, limit,
                 sizeof(kbc_anchor_row), &block, &n);
  sqlite3_stmt *q = NULL;
  if (st == KBC_OK) st = prepare(err, s, list_sql, &q);
  if (st == KBC_OK && doc_id != NULL) st = bind_text(err, s, q, 1, doc_id);
  if (st == KBC_OK)
    st = bind_i64(err, s, q, doc_id != NULL ? 2 : 1, (int64_t)n);
  kbc_anchor_row *arr = block;
  size_t i = 0;
  while (st == KBC_OK) {
    const int step = sqlite3_step(q);
    if (step == SQLITE_DONE) break;
    if (step != SQLITE_ROW) {
      st = sql_fail(err, s, "list anchors: step", step);
      break;
    }
    if (i >= n) {
      st = kbc_err_set(err, KBC_ERR_INTERNAL, "list anchors: row overflow");
      break;
    }
    arr[i].comment_id = col_str(a, q, 0);
    arr[i].doc_id = col_str(a, q, 1);
    arr[i].anchor = col_str(a, q, 2);
    arr[i].resolves_to = col_str(a, q, 3);
    arr[i].resolved_ord = (int64_t)sqlite3_column_int64(q, 4);
    arr[i].state = (kbc_anchor_state)sqlite3_column_int(q, 5);
    i++;
  }
  kbc_status fin = finalize(err, s, q, st);
  unlock(s);
  if (st != KBC_OK) return st != KBC_OK ? st : fin;
  *out = arr;
  *n_out = i;
  return KBC_OK;
}

/* How big the stale set is, in one statement and no page. This is the shape
 * a health check wants: it must not be able to answer "two" by looking at a
 * page of two, so it counts and does not list. */
int64_t kbc_store_count_stale_anchors(kbc_store *s, kbc_err *err) {
  if (s == NULL) {
    (void)kbc_err_set(err, KBC_ERR_INVALID, "count_stale_anchors: null store");
    return -1;
  }
  lock(s);
  int64_t n = 0;
  kbc_status st = count_query(err, s,
                              "SELECT COUNT(*) FROM comment_anchors"
                              " WHERE state <> 1;",
                              NULL, &n);
  unlock(s);
  return st == KBC_OK ? n : -1;
}

/* Drops every verdict that no longer has a live OPEN comment to describe.
 *
 * The foreign key already does this for a comment deleted through the store
 * — `comment_anchors.comment_id REFERENCES comments(id) ON DELETE CASCADE`,
 * and deleting the artifact cascades to the comment. This exists for the
 * states the FK cannot reach, and both of them are documented elsewhere in
 * this file rather than invented here: a writer with `foreign_keys` off (the
 * header on the FK-checking block says any writer that was not this binary
 * can orphan a row at any time), and a comment RESOLVED after its verdict was
 * written, which the original prunes too — `anchors::prune_if_resolved`,
 * anchors.rs:172-190, drops the key when a comment is resolved and does
 * nothing on an un-resolve, because "reopening must not clear a flag the
 * indexer still owns".
 *
 * So a resolved comment is dropped and an un-resolved one is not: the caller
 * says which by asking for a prune or not asking for one. */
kbc_status kbc_store_prune_anchors(kbc_store *s, int64_t *rows, kbc_err *err) {
  if (s == NULL || rows == NULL)
    return kbc_err_set(err, KBC_ERR_INVALID, "prune_anchors: null argument");
  *rows = 0;
  lock(s);
  sqlite3_stmt *q = NULL;
  kbc_status st = prepare(
      err, s,
      "DELETE FROM comment_anchors WHERE comment_id NOT IN"
      " (SELECT id FROM comments WHERE resolved = 0);",
      &q);
  if (st == KBC_OK) {
    const int step = sqlite3_step(q);
    if (step != SQLITE_DONE) st = sql_fail(err, s, "prune anchors", step);
  }
  kbc_status fin = finalize(err, s, q, st);
  /* Read HERE, under the mutex, and for the reason prune_history gives: the
   * counter is per-CONNECTION, so a statement another thread steps between
   * the unlock and the read replaces the value this function is about to
   * report with that thread's row count. */
  if (st == KBC_OK) *rows = (int64_t)sqlite3_changes(s->db);
  unlock(s);
  if (st != KBC_OK) return st != KBC_OK ? st : fin;
  return fin;
}

/* THE REBUILD FROM THE GRAPH, and the property the whole persist decision
 * rests on: after this, every row in `comment_anchors` describes the graph
 * that EXISTS.
 *
 * Two statements, in one transaction, and each has a reason that is not
 * "tidiness":
 *
 *  1. INVALIDATE. A row whose `content_hash` is not the artifact's current
 *     one is a verdict about bytes that are no longer this document's bytes.
 *     It becomes UNDECIDABLE — not UNRESOLVED, because "the heading is gone"
 *     is a claim about the document and we have not read the document — and
 *     its remembered text is CLEARED, because a text belonging to the old
 *     bytes is exactly what would let the next pass resolve it by following
 *     a heading that no longer exists. This is the crash-between-passes
 *     case: the pass judged a document, the document was re-indexed, and the
 *     process died before the next pass. A two-state machine has nowhere to
 *     put this verdict and so keeps asserting the old one, which is a claim
 *     about a document that never existed in that state.
 *
 *     `artifacts` has no row for a vanished document, so the LEFT JOIN's
 *     NULL invalidates those too — a verdict for a document that is not in
 *     the store is a verdict for nothing.
 *
 *  2. DROP. The rows nothing can describe. The FK covers a comment deleted
 *     through this store; this covers the rest, and the reason is the same
 *     one the FK-checking header gives: the version table records what RAN,
 *     not what is consistent, so a volume can be at the current version and
 *     hold an orphan.
 *
 * IDEMPOTENT, and that is not a nicety: `kbc_store_open` calls this, so a
 * fixed point here is what stops every daemon start from rewriting rows. The
 * second run must report 0 for both halves — a rebuild that is not a fixed
 * point is a rebuild that costs a write on every boot forever, and one that
 * flips a row back and forth is worse than not shipping it.
 *
 * NOT under `foreign_keys = defer`: this is not part of a rekey, it is a
 * repair, and the transaction is what makes the two halves visible together
 * or not at all. */
kbc_status kbc_store_reconcile_anchors(kbc_store *s, int64_t *rows,
                                       kbc_err *err) {
  if (s == NULL || rows == NULL)
    return kbc_err_set(err, KBC_ERR_INVALID, "reconcile_anchors: null argument");
  *rows = 0;
  lock(s);
  kbc_status st = exec_plain(err, s, "BEGIN IMMEDIATE;");
  if (st != KBC_OK) {
    unlock(s);
    return st;
  }
  sqlite3_stmt *q = NULL;
  int64_t touched = 0;
  st = prepare(err, s,
               "UPDATE comment_anchors SET state = 0, anchor_text = '',"
               " resolves_to = '', resolved_ord = -1"
               " WHERE state <> 0"
               " AND content_hash IS NOT"
               " (SELECT content_hash FROM artifacts WHERE id = doc_id);",
               &q);
  if (st == KBC_OK) {
    const int step = sqlite3_step(q);
    if (step != SQLITE_DONE)
      st = sql_fail(err, s, "reconcile anchors: invalidate", step);
  }
  kbc_status fin = finalize(err, s, q, st);
  if (st == KBC_OK) st = fin;
  if (st == KBC_OK) touched = (int64_t)sqlite3_changes(s->db);

  if (st == KBC_OK)
    st = exec_plain(err, s,
                    "DELETE FROM comment_anchors WHERE comment_id NOT IN"
                    " (SELECT id FROM comments WHERE resolved = 0);");
  if (st == KBC_OK) {
    /* Read on THIS connection immediately after the statement, the same
     * reason prune_history reads sqlite3_changes where it does. */
    touched += (int64_t)sqlite3_changes(s->db);
  }

  if (st == KBC_OK) st = exec_plain(err, s, "COMMIT;");
  else rollback(s);
  unlock(s);
  if (st != KBC_OK) return st;
  *rows = touched;
  return KBC_OK;
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
        " created_at = ?2, content_hash = ?5"
        " WHERE corpus = ?3 AND path = ?4 AND dismissed = 0;",
        &q);
    if (prepared != KBC_OK) {
      st = prepared;
    } else {
      st = bind_text(err, s, q, 1, row->message);
      if (st == KBC_OK) st = bind_i64(err, s, q, 2, row->created_at);
      if (st == KBC_OK) st = bind_text(err, s, q, 3, row->corpus);
      if (st == KBC_OK) st = bind_text(err, s, q, 4, row->path);
      /* The stored hash moves with the counter, so the row keeps describing the
       * bytes it is counting failures OF. A NULL records as SQL NULL, so a
       * caller recording without a hash clears the stale one rather than
       * leaving it to answer a question nobody asked again. */
      if (st == KBC_OK) st = bind_text(err, s, q, 5, row->content_hash);
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

/* The quarantine gate, keyed by (corpus, path) and — when the caller names the
 * content — by the hash of the bytes about to be embedded.
 *
 * The two modes answer DIFFERENT questions and conflating them is the bug the
 * hash exists to prevent:
 *
 *   content_hash != NULL — "is this still the document that failed?". A row
 *     whose stored hash differs reads as 0, because it belongs to different
 *     bytes. Editing a document is how an operator fixes one that failed, so
 *     the edited document must get a fresh embedding budget; the original
 *     spells this retry_count_for_path_hash (kb-core/src/indexer.rs:2439).
 *
 *   content_hash == NULL — "how bad is it?". The raw count for the path, which
 *     is what an operator's Errors view and a diagnostic want, and what a
 *     caller with no bytes in hand can still ask.
 *
 * 0 for a path with no open error, which is what a document that has never
 * failed must read as. A DISMISSED row is not counted: clearing an error is
 * what gives a document a fresh budget of attempts (Rust's un-quarantine route,
 * sqlite.rs:413). */
kbc_status kbc_store_retry_count_for_path(kbc_store *s, const char *corpus,
                                          const char *path,
                                          const char *content_hash,
                                          int64_t *out, kbc_err *err) {
  if (s == NULL || out == NULL)
    return kbc_err_set(err, KBC_ERR_INVALID,
                       "retry_count_for_path: null argument");
  kbc_status st = require_text(err, "error corpus", corpus, 255);
  if (st == KBC_OK)
    st = require_text(err, "error path", path, KBC_MAX_PATH_LEN);
  if (st == KBC_OK && content_hash != NULL)
    st = require_text(err, "error content hash", content_hash,
                      KBC_MAX_CONTENT_HASH_LEN);
  if (st != KBC_OK) return st;

  /* Two statement texts rather than one with a bound-and-ignored clause, so
   * neither mode can quietly read the other's rows. `content_hash = ?3` also
   * excludes a row recorded with NO hash: a row that never claimed to be about
   * these bytes is not evidence about them, and counting it would re-gate an
   * edited document on the strength of somebody else's failure. */
  const char *sql =
      content_hash != NULL
          ? "SELECT IFNULL((SELECT retry_count FROM errors WHERE corpus = ?1"
            " AND path = ?2 AND dismissed = 0 AND content_hash = ?3"
            " LIMIT 1), 0);"
          : "SELECT IFNULL((SELECT retry_count FROM errors WHERE corpus = ?1"
            " AND path = ?2 AND dismissed = 0 LIMIT 1), 0);";

  lock(s);
  sqlite3_stmt *q = NULL;
  st = prepare(err, s, sql, &q);
  if (st == KBC_OK) st = bind_text(err, s, q, 1, corpus);
  if (st == KBC_OK) st = bind_text(err, s, q, 2, path);
  if (st == KBC_OK && content_hash != NULL)
    st = bind_text(err, s, q, 3, content_hash);
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

/* Retention — see the declaration in include/kbc/store.h for the contract
 * and the two original citations. The implementation note is the one thing
 * the header cannot say: the DELETE runs under this store's mutex, so it
 * serialises against every other writer on the connection rather than
 * racing one. */
kbc_status kbc_store_prune_history(kbc_store *s, int64_t started_before_unix,
                                   bool apply, int64_t *rows, kbc_err *err) {
  if (s == NULL || rows == NULL)
    return kbc_err_set(err, KBC_ERR_INVALID, "prune_history: null argument");
  *rows = 0;
  /* A cutoff before the epoch prunes NOTHING, and the header says so in
   * those words. The previous shape clamped the cutoff to 0 and then ran
   * `started_at < 0`, which is a different predicate: `started_at` is a
   * caller-supplied INTEGER with no lower bound anywhere on its write path
   * (`kbc_store_add_history` validates the strings and binds the timestamps
   * straight through), so a row at `started_at = -1` is writable and the
   * clamp deleted it. The contract and the code disagreed, and the code was
   * the one that destroyed rows.
   *
   * Returning early rather than substituting 0 is the fix: the dry run and
   * the applied run now take the SAME branch, so the number a dry run
   * reports is by construction the number --apply removes — including when
   * both of them are zero. A cutoff of 0 is a real cutoff and still prunes
   * every row before the epoch; it is a NEGATIVE cutoff that is the no-op.
   * The original's `saturating_sub` lands in the same place for the same
   * reason: there, `now - age` saturates at i64::MIN and matches nothing. */
  if (started_before_unix < 0) return KBC_OK;

  /* One predicate for both modes, so the number a dry run reports is by
   * construction the number --apply removes. A dry run that could disagree
   * with the real thing would be worse than no dry run. */
  const char *sql =
      apply ? "DELETE FROM history WHERE started_at < ?1;"
            : "SELECT COUNT(*) FROM history WHERE started_at < ?1;";
  lock(s);
  sqlite3_stmt *q = NULL;
  kbc_status st = prepare(err, s, sql, &q);
  if (st == KBC_OK) st = bind_i64(err, s, q, 1, started_before_unix);
  int step = SQLITE_ERROR;
  if (st == KBC_OK) {
    step = sqlite3_step(q);
    if (step != SQLITE_DONE && step != SQLITE_ROW) {
      st = sql_fail(err, s, "prune history", step);
    }
  }
  /* Read on THIS connection immediately after the step, which is the only
   * point either value is defined at — sqlite3_changes is a per-connection
   * counter that any statement in between would have overwritten. */
  if (st == KBC_OK) {
    *rows = apply ? (int64_t)sqlite3_changes(s->db)
                  : sqlite3_column_int64(q, 0);
  }
  kbc_status fin = finalize(err, s, q, st);
  unlock(s);
  if (st != KBC_OK) return st != KBC_OK ? st : fin;
  return fin;
}

/* The reclaim half of retention, and separate from the delete so that a
 * caller cannot accidentally couple them. See the declaration for why it is
 * best-effort and must not be given a failure path. */
kbc_status kbc_store_checkpoint(kbc_store *s, kbc_err *err) {
  if (s == NULL)
    return kbc_err_set(err, KBC_ERR_INVALID, "checkpoint: null store");
  /* A scratch `err`, NOT the caller's: a refused or partial checkpoint is
   * reported as KBC_OK by contract, and letting exec_plain write the reason
   * into the caller's err would hand them a filled error alongside a success
   * status — which is the exact shape of the bug the best-effort rule
   * exists to prevent. */
  kbc_err scratch;
  kbc_err_reset(&scratch);
  lock(s);
  /* sqlite3_exec, which steps through every row even with no callback, so
   * the pragma's result row is drained. That matters because the original
   * is explicit about the one-shot hazard next door (sqlite.rs:2408-2412):
   * `incremental_vacuum` frees one page per step, so a single-step exec
   * reclaims a single page however large N is. `wal_checkpoint` is not in
   * that class — one call does the whole checkpoint — but the drain is why
   * exec_plain is the right helper here rather than a bare step. */
  (void)exec_plain(&scratch, s, "PRAGMA wal_checkpoint(TRUNCATE);");
  unlock(s);
  return KBC_OK;
}

/* ================================================================ moves === */

/* The hop bound, and it exists for exactly one reason: the walk must
 * TERMINATE. A chain a -> b -> a is representable in this table (nothing
 * forbids it, and a bug that writes one is precisely the case the bound is
 * for), and an unbounded walk over a cycle hangs a request thread forever.
 * The original bounds it at 64; the same number is used here so the two
 * agree on what a pathological chain looks like. The bound is a guard, not a
 * budget: a legitimate chain is two or three hops long. */
#define MOVE_MAX_HOPS 64

/* Runs one two-parameter UPDATE. `sql` names a statement that takes the old
 * value as ?1 and the new as ?2, so every table's rekey is one call and the
 * binding discipline is stated once instead of nine times. */
static kbc_status rekey_two(kbc_err *err, kbc_store *s, const char *sql,
                            const char *old_v, const char *new_v) {
  sqlite3_stmt *q = NULL;
  kbc_status st = prepare(err, s, sql, &q);
  if (st == KBC_OK) st = bind_text(err, s, q, 1, old_v);
  if (st == KBC_OK) st = bind_text(err, s, q, 2, new_v);
  if (st == KBC_OK) {
    const int step = sqlite3_step(q);
    if (step != SQLITE_DONE) st = sql_fail(err, s, "rekey", step);
  }
  kbc_status fin = finalize(err, s, q, st);
  return (st == KBC_OK) ? fin : st;
}

/* The delete-the-leftover half, whose statements take ONE parameter. It is a
 * separate function rather than a NULL second bind on purpose: binding ?2 to
 * a statement that has no ?2 is SQLITE_RANGE ("column index out of range"),
 * so sharing one helper would mean every DELETE silently failed and the
 * rekey reported a bind error instead of doing its work. */
static kbc_status rekey_del(kbc_err *err, kbc_store *s, const char *sql,
                            const char *old_v) {
  sqlite3_stmt *q = NULL;
  kbc_status st = prepare(err, s, sql, &q);
  if (st == KBC_OK) st = bind_text(err, s, q, 1, old_v);
  if (st == KBC_OK) {
    const int step = sqlite3_step(q);
    if (step != SQLITE_DONE) st = sql_fail(err, s, "rekey delete", step);
  }
  kbc_status fin = finalize(err, s, q, st);
  return (st == KBC_OK) ? fin : st;
}

/* The same two shapes again, for the tables whose key is (corpus, path)
 * rather than an id: the corpus binds as ?1 and the predicate it opens is
 * part of the WHERE, not a value being written.
 *
 * SEPARATE functions rather than an extra parameter on the two above, on
 * purpose. The id-keyed tables have no corpus column at all, so a shared
 * helper would have to take a possibly-NULL corpus and branch on it — and
 * that branch is the exact shape of the bug these exist to make impossible:
 * one statement that rewrites a path in every corpus because the corpus was
 * forgotten. Here the corpus is a required argument, so a call without it
 * does not compile. */
static kbc_status rekey_corp_two(kbc_err *err, kbc_store *s, const char *sql,
                                 const char *corpus, const char *old_v,
                                 const char *new_v) {
  sqlite3_stmt *q = NULL;
  kbc_status st = prepare(err, s, sql, &q);
  if (st == KBC_OK) st = bind_text(err, s, q, 1, corpus);
  if (st == KBC_OK) st = bind_text(err, s, q, 2, old_v);
  if (st == KBC_OK) st = bind_text(err, s, q, 3, new_v);
  if (st == KBC_OK) {
    const int step = sqlite3_step(q);
    if (step != SQLITE_DONE) st = sql_fail(err, s, "rekey", step);
  }
  kbc_status fin = finalize(err, s, q, st);
  return (st == KBC_OK) ? fin : st;
}

static kbc_status rekey_corp_del(kbc_err *err, kbc_store *s, const char *sql,
                                 const char *corpus, const char *old_v) {
  sqlite3_stmt *q = NULL;
  kbc_status st = prepare(err, s, sql, &q);
  if (st == KBC_OK) st = bind_text(err, s, q, 1, corpus);
  if (st == KBC_OK) st = bind_text(err, s, q, 2, old_v);
  if (st == KBC_OK) {
    const int step = sqlite3_step(q);
    if (step != SQLITE_DONE) st = sql_fail(err, s, "rekey delete", step);
  }
  kbc_status fin = finalize(err, s, q, st);
  return (st == KBC_OK) ? fin : st;
}

/* `artifacts` is rekeyed FIRST and the rest after, and the order is not
 * cosmetic. `chunks` and `comments` declare a real FOREIGN KEY onto
 * artifacts(id) and this connection runs `PRAGMA foreign_keys = ON`
 * (src/store.c:607), so with immediate enforcement the two updates are a
 * circular deadlock: the child cannot be rekeyed until the parent row
 * exists under the new id, and the parent cannot be rekeyed until its
 * children have let go. Both statements fail with SQLITE_CONSTRAINT_FOREIGNKEY
 * and the rekey is impossible.
 *
 * `defer_foreign_keys` is what breaks the cycle: it moves every FK check to
 * COMMIT, so both updates apply and the constraint is verified once, at the
 * end, against the finished state. It is per-transaction and auto-resets —
 * a write after this transaction commits is checked normally, which is
 * verified by a test rather than assumed. */
/* The derived-tag projection, defined with the verdict section at the foot
 * of this file. A move needs it because the tag is keyed on (corpus, path)
 * and a move changes the path: without re-projecting, the document at the
 * NEW path carries a verdict and no tag, and the document at the OLD path
 * carries a tag and no document. Forward-declared rather than moved so the
 * verdict section stays in one readable piece. */
static kbc_status verdict_retag(kbc_err *err, kbc_store *s, const char *corpus,
                                const char *path, kbc_verdict_state state);

/* The verdict that SURVIVED a rekey, which is not necessarily the one that
 * was there before: `UPDATE OR IGNORE` means a destination that already had
 * a verdict keeps ITS OWN, and the tag has to be re-derived from the winner
 * rather than copied from the loser. A document with no surviving verdict
 * gets no tag, which is the withdrawal rather than a stale projection. */
static kbc_status verdict_after_move(kbc_err *err, kbc_store *s,
                                     const char *new_id,
                                     kbc_verdict_state *state, bool *found) {
  *found = false;
  sqlite3_stmt *q = NULL;
  kbc_status st = prepare(
      err, s, "SELECT state FROM verdicts WHERE doc_id = ?1;", &q);
  if (st == KBC_OK) st = bind_text(err, s, q, 1, new_id);
  if (st == KBC_OK) {
    const int step = sqlite3_step(q);
    if (step == SQLITE_ROW) {
      *state = (kbc_verdict_state)sqlite3_column_int(q, 0);
      *found = true;
    } else if (step != SQLITE_DONE) {
      st = sql_fail(err, s, "read moved verdict", step);
    }
  }
  kbc_status fin = finalize(err, s, q, st);
  return st != KBC_OK ? st : fin;
}

kbc_status kbc_store_rekey_artifact(kbc_store *s, const char *old_id,
                                    const char *new_id, const char *old_rel,
                                    const char *new_rel, kbc_err *err) {
  if (s == NULL)
    return kbc_err_set(err, KBC_ERR_INVALID, "rekey_artifact: null store");
  kbc_status st = require_text(err, "rekey old_id", old_id, KBC_MAX_ID_LEN);
  if (st == KBC_OK)
    st = require_text(err, "rekey new_id", new_id, KBC_MAX_ID_LEN);
  if (st == KBC_OK)
    st = require_text(err, "rekey old_rel", old_rel, KBC_MAX_PATH_LEN);
  if (st == KBC_OK)
    st = require_text(err, "rekey new_rel", new_rel, KBC_MAX_PATH_LEN);
  if (st == KBC_OK && strcmp(old_id, new_id) == 0)
    return kbc_err_set(err, KBC_ERR_INVALID,
                       "rekey_artifact: old_id and new_id are the same");
  if (st != KBC_OK) return st;

  lock(s);
  st = exec_plain(err, s, "BEGIN IMMEDIATE;");
  if (st != KBC_OK) {
    unlock(s);
    return st;
  }
  /* Must be INSIDE the transaction: the pragma is a no-op outside one. */
  st = exec_plain(err, s, "PRAGMA defer_foreign_keys = ON;");

  /* `chunks`: PK(doc_id, ord), so a destination that already holds the same
   * ordinal is a real collision. OR IGNORE keeps the destination's chunk and
   * the delete drops the leftover — destination state wins, as in the
   * original. */
  if (st == KBC_OK)
    st = rekey_two(err, s,
                   "UPDATE OR IGNORE chunks SET doc_id = ?2 WHERE doc_id = ?1;",
                   old_id, new_id);
  if (st == KBC_OK)
    st = rekey_del(err, s, "DELETE FROM chunks WHERE doc_id = ?1;", old_id);

  /* `corkboard`, `pinned_memories`, `doc_first_seen`: PK on the artifact id,
   * same OR IGNORE + delete-leftover shape. For doc_first_seen the
   * destination's EARLIER timestamp is the one worth keeping, which is
   * exactly what OR IGNORE gives for free. */
  if (st == KBC_OK)
    st = rekey_two(err, s,
                   "UPDATE OR IGNORE corkboard SET artifact_id = ?2"
                   " WHERE artifact_id = ?1;",
                   old_id, new_id);
  if (st == KBC_OK)
    st = rekey_del(err, s, "DELETE FROM corkboard WHERE artifact_id = ?1;",
                   old_id);
  if (st == KBC_OK)
    st = rekey_two(err, s,
                   "UPDATE OR IGNORE pinned_memories SET artifact_id = ?2"
                   " WHERE artifact_id = ?1;",
                   old_id, new_id);
  if (st == KBC_OK)
    st = rekey_del(err, s,
                   "DELETE FROM pinned_memories WHERE artifact_id = ?1;",
                   old_id);
  if (st == KBC_OK)
    st = rekey_two(err, s,
                   "UPDATE OR IGNORE doc_first_seen SET artifact_id = ?2"
                   " WHERE artifact_id = ?1;",
                   old_id, new_id);
  if (st == KBC_OK)
    st = rekey_del(err, s,
                   "DELETE FROM doc_first_seen WHERE artifact_id = ?1;",
                   old_id);

  /* `history`: a reading visit naming the id. The column is NULLABLE and
   * carries no uniqueness, so a plain UPDATE cannot collide and there is
   * nothing to delete afterwards — the same shape the original gives it. The
   * visit follows the document, which is the behaviour store.h's section
   * banner promises: reading history outlives the document, and a moved
   * document is not a removed one. */
  if (st == KBC_OK)
    st = rekey_two(err, s,
                   "UPDATE history SET artifact_id = ?2"
   " WHERE artifact_id IS NOT NULL AND artifact_id = ?1;",
                   old_id, new_id);

  /* `attachments`: PK is the MINTED `id`, which no other row can share and
   * which a move never rewrites, so a plain UPDATE cannot collide and there
   * is nothing to delete afterwards — the same shape `history` gets above and
   * for the same reason.
   *
   * `comment_id` is deliberately NOT rewritten, and that is what keeps an
   * attachment riding through a move: the comment it belongs to keeps its
   * own id (store.c's `comments` UPDATE rewrites `doc_id` and never `id`),
   * so the edge stays valid with no change at all. The corollary is that an
   * attachment whose `comment_id` names a comment of ANOTHER document would
   * be left pointing across a move — which the schema forbids, because the
   * comment_id FK cannot name a comment on a different artifact unless
   * somebody wrote it outside the API, and that is an orphan the open-time
   * `foreign_key_check` reports by name. */
  if (st == KBC_OK)
    st = rekey_two(err, s,
                   "UPDATE attachments SET doc_id = ?2 WHERE doc_id = ?1;",
                   old_id, new_id);

  /* `verdicts`: PK is `doc_id`, so a destination that already carries a
   * verdict IS a real collision and the OR IGNORE + delete-leftover shape
   * applies — the destination's verdict wins, as everywhere else here. The
   * LEFTOVER is not silently dropped, though: the tag the old verdict
   * projected onto the old (corpus, path) has to go with it, or the document
   * at the old path keeps matching `kb-tags:status-approved` after it has
   * stopped existing. The old path is not known at this point in the
   * transaction (the corpus is read further down, before the path half), so
   * the withdrawal is deferred to `verdict_retag` at the call site below,
   * which runs after the corpus read. */
  if (st == KBC_OK)
    st = rekey_two(err, s,
                   "UPDATE OR IGNORE verdicts SET doc_id = ?2"
                   " WHERE doc_id = ?1;",
                   old_id, new_id);
  if (st == KBC_OK)
    st = rekey_del(err, s, "DELETE FROM verdicts WHERE doc_id = ?1;", old_id);


  /* `edges` and `pending_links` are keyed by (corpus, path) — the path alone
   * is not a key — so the path half of the move is what rewrites them. BOTH
   * columns: a document that is itself a link TARGET has its inbound edges
   * named at the old path, and those are exactly the backlinks the move must
   * not lose. The original rekeys both directions for the same reason.
   *
   * AND the corpus, read from the row being moved. A relative path is only
   * unique WITHIN a corpus, so an unfiltered rewrite is not a wider correct
   * rewrite — it is a wrong one, and the delete half makes it destructive.
   * Two corpora holding a same-named document (a config pointing both at
   * overlapping directories produces exactly that) then lose each other's
   * links: the UPDATE renames the other corpus's edge onto a path that does
   * not exist there, and where the renamed row collides on the primary key
   * the OR IGNORE skips it and the unfiltered DELETE then destroys it. Both
   * happened with the function returning ok and nothing reporting an error.
   * A rekey is about ONE document in ONE corpus; scoping it to the corpus
   * that document is in is the rule, not a refinement of it. */
  char corpus[256];
  bool have_corpus = false;
  if (st == KBC_OK) {
    /* Read before the `artifacts` UPDATE below, which is the statement that
     * takes this row's old id away. The path is in the predicate as well as
     * the id so the corpus is read off the row this call is actually about
     * and not off a same-id row at some other path. */
    sqlite3_stmt *q = NULL;
    st = prepare(err, s,
                 "SELECT corpus FROM artifacts WHERE id = ?1 AND path = ?2;",
                 &q);
    if (st == KBC_OK) st = bind_text(err, s, q, 1, old_id);
    if (st == KBC_OK) st = bind_text(err, s, q, 2, old_rel);
    if (st == KBC_OK) {
      const int step = sqlite3_step(q);
      if (step == SQLITE_ROW) {
        const char *c = (const char *)sqlite3_column_text(q, 0);
        const int nb = sqlite3_column_bytes(q, 0);
        /* Bounded, never truncated: `sources` admits a corpus of at most 255
         * bytes, so a longer one means the row was written outside the API
         * and copying it into this buffer would silently rewrite the wrong
         * prefix's edges. */
        if (c == NULL || nb <= 0 || (size_t)nb >= sizeof corpus) {
          st = kbc_err_set(err, KBC_ERR_INTERNAL,
                           "rekey %s: corpus is %d bytes, and a corpus slug is"
                           " at most 255",
                           old_id, nb);
        } else {
          memcpy(corpus, c, (size_t)nb);
          corpus[nb] = '\0';
          have_corpus = true;
        }
      } else if (step == SQLITE_DONE) {
        have_corpus = false;
      } else {
        st = sql_fail(err, s, "read rekey corpus", step);
      }
    }
    kbc_status fin = finalize(err, s, q, st);
    if (st == KBC_OK) st = fin;
  }

  /* No row means this call is not about a document this store holds, and the
   * path half is SKIPPED rather than guessed. Guessing is the bug: an
   * unfiltered rewrite is precisely a guess about which corpus owns the path,
   * and it is the one that destroys rows. Skipping is recoverable — the
   * re-ingest at the new path rebuilds the moved document's own outbound
   * edges from the same bytes — and a deleted edge is not. The id-keyed
   * statements above are left to run; they are no-ops without a parent row,
   * and the trailing `DELETE FROM artifacts` is one too. */
  if (st == KBC_OK && have_corpus) {
    /* The derived tag, withdrawn from the old path and re-projected onto the
     * new one, from the verdict that actually SURVIVED rather than the one
     * that was there before. Without this the move leaves a `status-approved`
     * facet pointing at a path with no document, and the destination has a
     * verdict no filter can see — both halves wrong, in opposite directions,
     * and neither of them is something a later pass would notice. */
    if (st == KBC_OK) {
      kbc_verdict_state state = KBC_VERDICT_COMMENT;
      bool has = false;
      st = verdict_after_move(err, s, new_id, &state, &has);
      if (st == KBC_OK)
        st = verdict_retag(err, s, corpus, old_rel, KBC_VERDICT_COMMENT);
      if (st == KBC_OK && has)
        st = verdict_retag(err, s, corpus, new_rel, state);
    }
    if (st == KBC_OK)
      st = rekey_corp_two(err, s,
                          "UPDATE OR IGNORE edges SET src_path = ?3"
                          " WHERE corpus = ?1 AND src_path = ?2;",
                          corpus, old_rel, new_rel);
    if (st == KBC_OK)
      st = rekey_corp_del(err, s,
                          "DELETE FROM edges WHERE corpus = ?1 AND src_path = "
                          "?2;",
                          corpus, old_rel);
    if (st == KBC_OK)
      st = rekey_corp_two(err, s,
                          "UPDATE OR IGNORE edges SET dst_path = ?3"
                          " WHERE corpus = ?1 AND dst_path = ?2;",
                          corpus, old_rel, new_rel);
    if (st == KBC_OK)
      st = rekey_corp_del(err, s,
                          "DELETE FROM edges WHERE corpus = ?1 AND dst_path = "
                          "?2;",
                          corpus, old_rel);
    if (st == KBC_OK)
      st = rekey_corp_two(err, s,
                          "UPDATE OR IGNORE pending_links SET src_path = ?3"
                          " WHERE corpus = ?1 AND src_path = ?2;",
                          corpus, old_rel, new_rel);
    if (st == KBC_OK)
      st = rekey_corp_del(err, s,
                          "DELETE FROM pending_links WHERE corpus = ?1 AND"
                          " src_path = ?2;",
                          corpus, old_rel);
    if (st == KBC_OK)
      st = rekey_corp_two(err, s,
                          "UPDATE OR IGNORE pending_links SET dst_path = ?3"
                          " WHERE corpus = ?1 AND dst_path = ?2;",
                          corpus, old_rel, new_rel);
    if (st == KBC_OK)
      st = rekey_corp_del(err, s,
                          "DELETE FROM pending_links WHERE corpus = ?1 AND"
                          " dst_path = ?2;",
                          corpus, old_rel);
  }

  /* `comments` — THE EXCEPTION, and the reason this function exists.
   *
   * NOT OR IGNORE + delete-leftover. A comment's `id` is its own PRIMARY KEY
   * and is MINTED, so two different comments never collide on it and a
   * collision here would mean the same comment id at two documents — which
   * deleting "the leftover" would resolve by DESTROYING a user's words. The
   * id and `created_at` are carried across untouched, so a moved comment is
   * the same comment: same id, same timestamp, same anchor, same author,
   * same body, same resolved flag.
   *
   * `created_at` is the field this whole function is about. The only public
   * way to write a comment is kbc_store_add_comment, which MINTS it, so a
   * caller re-adding a carried comment to rebuild it stamped every carried
   * comment "now" — the one user-visible field a move cannot reconstruct from
   * the document's bytes, because a timestamp is not in the bytes. Doing the
   * rekey HERE, in SQL, is what makes the field survive: nothing re-writes
   * the column, so there is nothing to get wrong. */
  if (st == KBC_OK)
    st = rekey_two(err, s,
                   "UPDATE comments SET doc_id = ?2 WHERE doc_id = ?1;",
                   old_id, new_id);

  /* `artifacts` LAST, after every child has let go of old_id — and the order
   * is load-bearing, not tidy. The delete-the-leftover below is an
   * ON DELETE CASCADE parent delete: run while the children still name
   * old_id, it takes their rows WITH it, and a rekey that reported success
   * would have silently deleted the document's chunks and every comment on
   * it. That is the worst possible failure for this function, because the
   * caller is about to re-ingest and rebuild what it can — everything it
   * cannot rebuild is exactly what the cascade destroyed.
   *
   * So the children move first and the parent follows. `OR IGNORE` then
   * delete-the-leftover, like the tables above: a destination that already
   * occupies (corpus, new_rel) WINS on the UNIQUE index, and the orphaned
   * source row goes — now safely, with nothing left to cascade. */
  if (st == KBC_OK) {
    sqlite3_stmt *q = NULL;
    st = prepare(err, s,
                 "UPDATE OR IGNORE artifacts SET id = ?2, path = ?3"
                 " WHERE id = ?1 AND path = ?4;",
                 &q);
    if (st == KBC_OK) st = bind_text(err, s, q, 1, old_id);
    if (st == KBC_OK) st = bind_text(err, s, q, 2, new_id);
    if (st == KBC_OK) st = bind_text(err, s, q, 3, new_rel);
    if (st == KBC_OK) st = bind_text(err, s, q, 4, old_rel);
    if (st == KBC_OK) {
      const int step = sqlite3_step(q);
      if (step != SQLITE_DONE) st = sql_fail(err, s, "rekey artifacts", step);
    }
    kbc_status fin = finalize(err, s, q, st);
    if (st == KBC_OK) st = fin;
  }
  if (st == KBC_OK)
    st = rekey_del(err, s, "DELETE FROM artifacts WHERE id = ?1;", old_id);

  if (st == KBC_OK) st = exec_plain(err, s, "COMMIT;");
  /* One transaction for all of it, so a crash cannot half-carry a thread: a
   * document either arrives at its new id with its comments intact, or it did
   * not move at all. That atomicity is the whole point of this being in the
   * store rather than seven calls under a mutex. */
  if (st != KBC_OK) rollback(s);
  unlock(s);
  return st;
}

/* The intent row, written BEFORE the rename. `completed_at` is left NULL by
 * this insert and only stamped by kbc_store_complete_move, so an interrupted
 * move is a row a bring-up pass can find rather than a document that quietly
 * vanished between two operations nobody could see. */
kbc_status kbc_store_record_move(kbc_store *s, const char *old_id,
                                 const char *new_id, const char *old_rel,
                                 const char *new_rel, int64_t moved_at,
                                 kbc_err *err) {
  if (s == NULL)
    return kbc_err_set(err, KBC_ERR_INVALID, "record_move: null store");
  kbc_status st = require_text(err, "move old_id", old_id, KBC_MAX_ID_LEN);
  if (st == KBC_OK)
    st = require_text(err, "move new_id", new_id, KBC_MAX_ID_LEN);
  if (st == KBC_OK)
    st = require_text(err, "move old_rel", old_rel, KBC_MAX_PATH_LEN);
  if (st == KBC_OK)
    st = require_text(err, "move new_rel", new_rel, KBC_MAX_PATH_LEN);
  if (st != KBC_OK) return st;

  lock(s);
  sqlite3_stmt *q = NULL;
  st = prepare(err, s,
               "INSERT INTO moves(old_id, new_id, old_rel, new_rel, moved_at)"
               " VALUES(?1,?2,?3,?4,?5);",
               &q);
  if (st == KBC_OK) st = bind_text(err, s, q, 1, old_id);
  if (st == KBC_OK) st = bind_text(err, s, q, 2, new_id);
  if (st == KBC_OK) st = bind_text(err, s, q, 3, old_rel);
  if (st == KBC_OK) st = bind_text(err, s, q, 4, new_rel);
  if (st == KBC_OK) st = bind_i64(err, s, q, 5, moved_at);
  if (st == KBC_OK) {
    const int step = sqlite3_step(q);
    if (step != SQLITE_DONE) st = sql_fail(err, s, "record move", step);
  }
  kbc_status fin = finalize(err, s, q, st);
  if (st == KBC_OK) st = fin;
  unlock(s);
  return st;
}

/* Stamps EVERY in-flight row for old_id, not one of them: a document moved
 * twice before the first stamp landed has two intent rows and both describe
 * the same finished move. A single-row UPDATE would leave one of them
 * incomplete forever, and an incomplete row suppresses the watcher's delete
 * guard and shows up in the bring-up replay list for a move that completed
 * long ago.
 *
 * NEVER a row somebody has ABANDONED, and `abandoned_at IS NULL` is the whole
 * guard. An abandoned row is a DECISION that the rename did not happen, and
 * stamping it completed here would turn that decision into the exact wrong
 * answer it exists to prevent: a redirect to a destination the document never
 * reached. A bring-up pass that abandoned a move and a later caller that
 * completes the same id must not be able to disagree about what happened —
 * the row is the record, and only one of them can be right about it. */
kbc_status kbc_store_complete_move(kbc_store *s, const char *old_id,
                                   kbc_err *err) {
  if (s == NULL)
    return kbc_err_set(err, KBC_ERR_INVALID, "complete_move: null store");
  kbc_status st = require_text(err, "move old_id", old_id, KBC_MAX_ID_LEN);
  if (st != KBC_OK) return st;

  lock(s);
  sqlite3_stmt *q = NULL;
  st = prepare(err, s,
               "UPDATE moves SET completed_at = ?2"
               " WHERE old_id = ?1 AND completed_at IS NULL"
               " AND abandoned_at IS NULL;",
               &q);
  if (st == KBC_OK) st = bind_text(err, s, q, 1, old_id);
  if (st == KBC_OK) st = bind_i64(err, s, q, 2, (int64_t)time(NULL));
  if (st == KBC_OK) {
    const int step = sqlite3_step(q);
    if (step != SQLITE_DONE) st = sql_fail(err, s, "complete move", step);
  }
  kbc_status fin = finalize(err, s, q, st);
  if (st == KBC_OK) st = fin;
  /* A move that was never recorded is NOT a failure here. The row is an
   * intent journal, and a caller that completes a move it did not record has
   * still finished the move; reporting NOTFOUND would make a completed rename
   * look broken on a database opened by an older build. */
  unlock(s);
  return st;
}

/* The third terminal state, and the only one that is not a promise about
 * where the document is.
 *
 * `completed_at` alone has two states and the bring-up pass needs three: a
 * rename that never reached the disk still has to leave the replay list, and
 * the only way out of the replay list today is to stamp the row completed,
 * which hands `moves_lookup` a destination that does not exist. The pass
 * would then answer a stale reference with a document that is not there, and
 * `kbc_app_get_artifact` follows that chain, so the wrong answer is reachable
 * rather than theoretical.
 *
 * The predicate is the same shape complete_move's mirror: every IN-FLIGHT,
 * not-yet-abandoned row naming old_id, because a document moved twice before
 * either stamp landed has two intent rows for the same rename. It deliberately
 * will not touch a row that is already completed: an abandoned flag on a
 * finished move would retract a redirect the document actually earned, which
 * is the same class of wrong answer pointed the other way, and this is a
 * bring-up decision about a move that is still in flight.
 *
 * A row that matches nothing is NOT a failure, for the reason
 * `kbc_store_complete_move` gives: this is an intent journal, and the caller
 * has still decided the move is not going to happen. Reporting NOTFOUND would
 * make a second bring-up pass that saw the row before it was stamped look
 * like a broken database. */
kbc_status kbc_store_abandon_move(kbc_store *s, const char *old_id,
                                  kbc_err *err) {
  if (s == NULL)
    return kbc_err_set(err, KBC_ERR_INVALID, "abandon_move: null store");
  kbc_status st = require_text(err, "move old_id", old_id, KBC_MAX_ID_LEN);
  if (st != KBC_OK) return st;

  lock(s);
  sqlite3_stmt *q = NULL;
  st = prepare(err, s,
               "UPDATE moves SET abandoned_at = ?2"
               " WHERE old_id = ?1 AND completed_at IS NULL"
               " AND abandoned_at IS NULL;",
               &q);
  if (st == KBC_OK) st = bind_text(err, s, q, 1, old_id);
  if (st == KBC_OK) st = bind_i64(err, s, q, 2, (int64_t)time(NULL));
  if (st == KBC_OK) {
    const int step = sqlite3_step(q);
    if (step != SQLITE_DONE) st = sql_fail(err, s, "abandon move", step);
  }
  kbc_status fin = finalize(err, s, q, st);
  if (st == KBC_OK) st = fin;
  unlock(s);
  return st;
}

/* Follows a chain of COMPLETED moves and appends the destination to `out`.
 *
 * CHAIN-WALKED, not a single hop, because a path can be renamed more than
 * once: a -> b -> c means a bookmark made before the first rename has to end
 * up at c, and answering "b" would hand the caller a name that was itself
 * renamed away. The original walks the same chain (sqlite.rs:5876).
 *
 * An INCOMPLETE row is skipped rather than followed. Its rename never
 * finished, so its `new_id` is a name the document may never have reached;
 * following it would resolve a bookmark to a document that does not exist,
 * which is worse than reporting the id unchanged.
 *
 * An ABANDONED row is skipped for the same reason, and it is not the same
 * thing. Incomplete is an accident — nobody decided, the process died
 * mid-move — while abandoned is a decision RECORDED IN THE ROW: somebody
 * tried, it did not happen, the id stays where it was. The two states need the
 * same answer (a completed move is a promise about where the document IS; an
 * abandoned one is a promise that it never went anywhere) and they get it from
 * the same clause, which is why `abandoned_at IS NULL` sits next to
 * `completed_at IS NOT NULL` on every hop rather than in a filter upstream: a
 * chain walked hop by hop can pass THROUGH an abandoned row, and only the
 * hop's own predicate can refuse it.
 *
 * A row carrying BOTH stamps is refused by this predicate, and ABANDONED WINS
 * — deliberately, because the two writers above cannot produce the state and
 * the clause is therefore the only thing that decides it. Both stamps name a
 * different destination, so one of them is wrong, and the two answers a caller
 * can get are "this id never moved" and "this id is at a place it is not".
 * Only the first is safe, so the state resolves to it. Keep the clause even
 * though removing it changes nothing the public API can reach: the lookup's
 * predicate is the contract, and a future writer that stamps a row directly
 * must not be able to make this the one place a wrong answer escapes.
 *
 * The walk is bounded twice over, and both bounds are needed. MOVE_MAX_HOPS
 * stops a long chain, and the explicit "have I seen this id" comparison stops
 * a CYCLE — a -> b -> a is representable in the table and an unbounded walk
 * over one never returns, hanging the httpd worker that called it. A cycle
 * stops with the last distinct hop as the answer: a partial redirect is a
 * better answer than no answer, and the caller can detect the truncation
 * because the id it passed in is not in what came back. */
static kbc_status moves_walk(kbc_err *err, kbc_store *s, bool by_path,
                             const char *start, kbc_strlist *out) {
  kbc_status st = KBC_OK;
  /* Sized for a PATH, not an id, because this one function serves both: a
   * path is up to KBC_MAX_PATH_LEN and truncating one would send the walk
   * off to a path that does not exist, which is the one answer a redirect
   * must never give. `require_text` has already bounded the input. */
  char cur[KBC_MAX_PATH_LEN + 1];
  size_t n = strlen(start);
  memcpy(cur, start, n + 1u);

  for (int hop = 0; hop < MOVE_MAX_HOPS; hop++) {
    /* The cycle guard lives at the BOTTOM of this loop, against the value
     * about to be moved to. */
    char next[KBC_MAX_PATH_LEN + 1];
    next[0] = '\0';
    sqlite3_stmt *q = NULL;
    /* Two fixed statements chosen by a bool, never caller text — the only
     * un-bound strings in this file's SQL are compile-time literals. */
    const char *sql =
        by_path ? "SELECT new_rel FROM moves WHERE old_rel = ?1"
                  " AND completed_at IS NOT NULL AND abandoned_at IS NULL"
                  " ORDER BY id DESC LIMIT 1;"
                : "SELECT new_id FROM moves WHERE old_id = ?1"
                  " AND completed_at IS NOT NULL AND abandoned_at IS NULL"
                  " ORDER BY id DESC LIMIT 1;";
    st = prepare(err, s, sql, &q);
    if (st == KBC_OK) st = bind_text(err, s, q, 1, cur);
    if (st == KBC_OK) {
      const int step = sqlite3_step(q);
      if (step == SQLITE_ROW) {
        const char *v = (const char *)sqlite3_column_text(q, 0);
        /* A NULL in a NOT NULL column is the database's problem to report,
         * not a hop to skip quietly. */
        if (v == NULL) {
          st = kbc_err_set(err, KBC_ERR_INTERNAL, "moves: null %s",
                           by_path ? "new_rel" : "new_id");
        } else {
          const size_t vn = strlen(v);
          if (vn > KBC_MAX_PATH_LEN) {
            st = kbc_err_set(err, KBC_ERR_INTERNAL,
                             "moves: a %s in the moves table is %zu bytes",
                             by_path ? "new_rel" : "new_id", vn);
          } else {
            memcpy(next, v, vn + 1u);
          }
        }
      } else if (step != SQLITE_DONE) {
        st = sql_fail(err, s, "moves walk", step);
      }
    }
    kbc_status fin = finalize(err, s, q, st);
    if (st == KBC_OK) st = fin;
    if (st != KBC_OK) return st;

    /* No followed row: the chain ends here, or the only row naming this name
     * is an interrupted or an abandoned move that must not be followed.
     * Either way, stop. */
    if (next[0] == '\0') break;
    /* The CYCLE guard, and it must test the value we are ABOUT TO move TO,
     * not the one we are leaving. Testing the current value against what has
     * already been collected reads hop 1's own destination back on hop 2 and
     * stops every chain at one hop — which is the single-hop bug this whole
     * function exists to avoid, wearing a guard's clothing. */
    if (kbc_strlist_contains(out, next)) break;
    st = kbc_strlist_push(out, next);
    if (st != KBC_OK) return st;
    memcpy(cur, next, strlen(next) + 1u);
  }
  return KBC_OK;
}

/* Where a stale id now lives. `ids` receives the chain's destinations in
 * order, the FINAL one last, and is EMPTY AND KBC_OK when this id never
 * moved: "this document has not been renamed" is the common answer, and a
 * caller wants the absence, not an error. A single hop would be wrong for a
 * document renamed twice, so the walk is the contract and the empty answer
 * is not a failure. */
kbc_status kbc_store_moves_lookup(kbc_store *s, const char *id,
                                  kbc_strlist *ids, kbc_err *err) {
  if (s == NULL || ids == NULL)
    return kbc_err_set(err, KBC_ERR_INVALID, "moves_lookup: null argument");
  kbc_status st = require_text(err, "moves_lookup id", id, KBC_MAX_ID_LEN);
  if (st != KBC_OK) return st;
  lock(s);
  st = moves_walk(err, s, false, id, ids);
  unlock(s);
  return st;
}

/* The same walk keyed by path, for a link written against the old path. */
kbc_status kbc_store_moves_lookup_path(kbc_store *s, const char *rel,
                                       kbc_strlist *rels, kbc_err *err) {
  if (s == NULL || rels == NULL)
    return kbc_err_set(err, KBC_ERR_INVALID, "moves_lookup_path: null "
                       "argument");
  kbc_status st =
      require_text(err, "moves_lookup_path rel", rel, KBC_MAX_PATH_LEN);
  if (st != KBC_OK) return st;
  lock(s);
  st = moves_walk(err, s, true, rel, rels);
  unlock(s);
  return st;
}

/* Moves that started and never finished, for bring-up to converge. Both
 * lists are filled in ONE pass and are PARALLEL: entry i of `old_ids` and
 * entry i of `old_rels` describe the same interrupted move, because a
 * bring-up pass that has to re-query to pair them can pair them wrongly.
 *
 * `completed_at IS NULL AND abandoned_at IS NULL` is the whole predicate — not
 * a timestamp comparison. "Old enough" would be a second, different question
 * (the watcher's 10 s grace), and folding it in here would make a caller
 * unable to ask the first question without the second.
 *
 * The `abandoned_at` clause is why the replay list terminates. An abandoned
 * row HAS left the replay list — that is what abandoning one means — so a
 * bring-up pass that re-listed it would re-decide the same move on every boot,
 * which is a loop with a log line and a permanent warning for an operator who
 * gave up on that rename long ago. Re-listing it would also misdescribe the
 * state: "started and never finished" is the undecidedness an abandoned row no
 * longer has. */
kbc_status kbc_store_list_incomplete_moves(kbc_store *s, kbc_strlist *old_ids,
                                           kbc_strlist *old_rels,
                                           kbc_err *err) {
  if (s == NULL || old_ids == NULL || old_rels == NULL)
    return kbc_err_set(err, KBC_ERR_INVALID,
                       "list_incomplete_moves: null argument");
  lock(s);
  sqlite3_stmt *q = NULL;
  kbc_status st = prepare(err, s,
                         "SELECT old_id, old_rel FROM moves"
                         " WHERE completed_at IS NULL AND abandoned_at IS NULL"
                         " ORDER BY id;",
                         &q);
  for (;;) {
    const int step = (st == KBC_OK) ? sqlite3_step(q) : SQLITE_DONE;
    if (step == SQLITE_DONE) break;
    if (step != SQLITE_ROW) {
      st = sql_fail(err, s, "list incomplete moves", step);
      break;
    }
    const char *id = (const char *)sqlite3_column_text(q, 0);
    const char *rel = (const char *)sqlite3_column_text(q, 1);
    if (id == NULL || rel == NULL) {
      st = kbc_err_set(err, KBC_ERR_INTERNAL,
                       "list incomplete moves: null old_id or old_rel");
      break;
    }
    st = kbc_strlist_push(old_ids, id);
    if (st != KBC_OK) break;
    st = kbc_strlist_push(old_rels, rel);
    if (st != KBC_OK) break;
  }
  kbc_status fin = finalize(err, s, q, st);
  if (st == KBC_OK) st = fin;
  unlock(s);
  return st;
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

/* Most recently anchored first, artifact_id ascending as the tiebreak.
 *
 * A PAGE, and the ORDER BY is what makes it one: `LIMIT n` takes the n most
 * RECENT anchors, so a document anchored early is not among them, and an id
 * missing from this page is not evidence that it is unanchored. Anything that
 * needs one specific document's anchor reads it by id. */
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

/* Most recently pinned first, artifact_id ascending as the tiebreak.
 *
 * A PAGE for the same reason as the corkboard's: `LIMIT n` takes the n most
 * RECENT pins, so a pin set early is not among them and a missing id is not
 * evidence that it is unpinned. */
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

/* ======================================================== attachments ==
 *
 * The blob and its row are ONE ROW. Everything else here follows from that,
 * so the section opens with what it bought and what it cost, because the
 * shape is a deliberate divergence from the original rather than a
 * transcription of it.
 *
 * THE RUST'S MODEL, and why it is a split. `kb-core/src/attachments.rs`
 * stores bytes at `<state>/<kb>/.attachments/<artifact_id>/<aid>` and the
 * metadata in a sibling `_manifest.json`, and its own header says why the
 * manifest is "non-authoritative cache-like state": the comment that OWNS an
 * attachment lives in a per-artifact JSON review file OUTSIDE the database
 * (`review.rs`, `indexer.rs:3001`). With the owner in one file and the bytes
 * in another there is no transaction to join them in, so a reader can find a
 * manifest entry whose blob is not there, a saver can find a blob no entry
 * names, and `gc_plan` plus a 24-hour grace window plus a per-kb
 * `review_lock` plus tmpfile-rename-fsync are what stand in for the
 * transaction the original cannot have.
 *
 * WHAT IT BUYS HERE. `comments` is a TABLE in the SAME database, so the join
 * is available and the whole apparatus retires. One statement carries the
 * bytes and the claim, which makes "an attachment stored without its row"
 * not a state the schema can represent — let alone one a crash window can
 * produce. There is no `aid` that ever reaches a path, so there is no
 * traversal to guard: the Rust needs `is_safe_id` on every attachment id
 * because the id IS half a path, and here the id is a primary key.
 *
 * WHAT IT COSTS, said plainly. Bytes now live in the database file rather
 * than beside it, so they are in the backup (a plus) and in the page cache
 * under a corpus-wide cap the original never had (a minus). The size limit
 * below is what bounds that.
 */

/* The id minting the Rust uses, byte for byte: `a_` + 12 lowercase hex from
 * 6 random bytes (review.rs::new_attachment_id). RANDOM, not derived from
 * the content and not derived from the filename — the original has no
 * content hash on an attachment and no dedup, so two uploads of identical
 * bytes produce two ids and two rows, and that is the documented behaviour
 * rather than a defect to be fixed here. A content-addressed blob would be
 * a different design with a different question attached (who may read the
 * other one), and inventing it would silently deduplicate uploads the
 * original keeps. */
static void mint_attachment_id(char out[KBC_ATTACH_ID_LEN + 1]) {
  /* The randomness comes from the same source `mint_id` uses, because two
   * id minters in one file is two places for a weak fallback to hide. */
  char hex[KBC_MAX_ID_LEN + 1];
  mint_id(hex);
  out[0] = 'a';
  out[1] = '_';
  memcpy(out + 2, hex, KBC_MAX_ID_LEN);
  out[KBC_ATTACH_ID_LEN] = '\0';
}

/* The decoded code point at `*i`, advancing it past the sequence, or 0 with
 * `*i` left where it was when the bytes are not a well-formed UTF-8
 * sequence. Strict in the sense Rust's `str::from_utf8` is: no overlongs, no
 * surrogates, nothing above U+10FFFF, no truncated tail. */
static uint32_t utf8_next(const unsigned char *b, size_t n, size_t *i) {
  const size_t at = *i;
  const unsigned char c = b[at];
  if (c < 0x80u) {
    *i = at + 1;
    return c;
  }
  /* The lead byte carries the length in its top bits and the first payload
   * bits in its bottom ones, so the two agree exactly on the ranges below. */
  size_t need;
  uint32_t cp;
  if (c >= 0xC2u && c <= 0xDFu) {
    need = 2;
    cp = c & 0x1Fu;
  } else if (c >= 0xE0u && c <= 0xEFu) {
    need = 3;
    cp = c & 0x0Fu;
  } else if (c >= 0xF0u && c <= 0xF4u) {
    need = 4;
    cp = c & 0x07u;
  } else {
    return 0; /* a continuation byte in lead position, or C0/C1/F5..FF */
  }
  if (at + need > n) return 0;
  for (size_t k = 1; k < need; k++) {
    const unsigned char t = b[at + k];
    if ((t & 0xC0u) != 0x80u) return 0;
    cp = (cp << 6) | (uint32_t)(t & 0x3Fu);
  }
  /* The three ranges an overlong encoding, a surrogate and an out-of-range
   * scalar would land in. Rust rejects all three, so this does too. */
  if (need == 3 && (cp < 0x800u || (cp >= 0xD800u && cp <= 0xDFFFu)))
    return 0;
  if (need == 4 && (cp < 0x10000u || cp > 0x10FFFFu)) return 0;
  *i = at + need;
  return cp;
}

/* `is_plain_text`, attachments.rs:86-96: valid, non-empty UTF-8 with no
 * control characters other than tab, newline and carriage return.
 *
 * "control character" is Rust's `char::is_control`, which is the Unicode Cc
 * category — so the C0 range, DEL, and U+0080..U+009F (which arrive as the
 * two-byte C2 80..C2 9F). Testing only the C0 range would let a NEL
 * (U+0085) through, and a NEL is exactly the kind of byte that makes a
 * "text" blob render as something else. U+2028 and U+2029 are NOT Cc and
 * are allowed here, matching Rust. */
static bool is_plain_text(const unsigned char *b, size_t n) {
  if (n == 0) return false;
  size_t i = 0;
  while (i < n) {
    const uint32_t cp = utf8_next(b, n, &i);
    if (cp == 0) return false; /* covers U+0000 too, which is a control char */
    if (cp == '\t' || cp == '\n' || cp == '\r') continue;
    if (cp < 0x20u || cp == 0x7Fu || (cp >= 0x80u && cp <= 0x9Fu)) return false;
  }
  return true;
}

/* `sniff_allowed`, attachments.rs:62-82, and it IS the upload gate — "NEVER
 * trusts a client-supplied type". Six accepted shapes and nothing else:
 * PNG, JPEG, GIF87a/89a, WEBP (RIFF....WEBP, so the container AND the form
 * are both checked), %PDF-, and UTF-8 text.
 *
 * SVG, HTML and JS are DELIBERATELY not image types. They are valid UTF-8,
 * so markup sniffs as `text/plain` and the serve route force-downloads it;
 * that is the XSS guard (root invariant #18) and it is why this is a
 * byte-level function and not a lookup of what the client declared.
 *
 * The returned strings are the `&'static str` the serve route emits
 * verbatim, so a stored `content_type` is always one of six and a caller
 * cannot smuggle a header through the column. */
const char *kbc_store_sniff_attachment(const void *bytes, size_t n) {
  const unsigned char *b = bytes;
  if (n >= 8 && memcmp(b, "\x89PNG\r\n\x1a\n", 8) == 0) return "image/png";
  if (n >= 3 && b[0] == 0xFF && b[1] == 0xD8 && b[2] == 0xFF)
    return "image/jpeg";
  if (n >= 6 && (memcmp(b, "GIF87a", 6) == 0 || memcmp(b, "GIF89a", 6) == 0))
    return "image/gif";
  if (n >= 12 && memcmp(b, "RIFF", 4) == 0 && memcmp(b + 8, "WEBP", 4) == 0)
    return "image/webp";
  if (n >= 5 && memcmp(b, "%PDF-", 5) == 0) return "application/pdf";
  return is_plain_text(b, n) ? "text/plain; charset=utf-8" : NULL;
}

/* `is_inline_image`, attachments.rs:101-106. THE keystone of the attachment
 * XSS guard and the reason this is a function of the SNIFFED type rather
 * than of anything a client sent: a browser will run script served inline
 * from the daemon's own origin, and a PDF or an SVG served inline is that
 * script. Everything not on this list is force-downloaded. */
bool kbc_store_attachment_inline(const char *content_type) {
  return strcmp(content_type, "image/png") == 0 ||
         strcmp(content_type, "image/jpeg") == 0 ||
         strcmp(content_type, "image/gif") == 0 ||
         strcmp(content_type, "image/webp") == 0;
}

/* `sanitize_filename`, attachments.rs:113-127, and every clause of it is
 * load-bearing because the value is attacker-influenced (AGENTS.md rule 9):
 *
 *  - rsplit on the last `/` or `\`, so `../../etc/passwd` becomes
 *    `passwd`. The result is never used to build a path here — the key is
 *    the minted aid — but it IS put in a quoted `Content-Disposition`, and a
       slash there is a header the client will believe about a path.
 *  - control characters dropped, which is what keeps a newline out of a
 *    response header;
 *  - `"` becomes `_`, because a quote ends the Content-Disposition
 *    filename and the rest of it becomes attacker-chosen header text;
 *  - capped at 120 CHARACTERS, not 120 bytes, so a cap cannot land inside
 *    a multi-byte sequence and produce a name that is not valid UTF-8;
 *  - empty, `.` and `..` become `file`, because those three are the only
 *    names that name nothing.
 *
 * ARENA, and bounded on both ends: the caller supplies the arena and the
 * 120-character cap makes the result at most 480 bytes. */
const char *kbc_store_sanitize_filename(kbc_arena *a, const char *raw) {
  if (raw == NULL) raw = "";
  const char *base = raw;
  for (const char *p = raw; *p != '\0'; p++) {
    if (*p == '/' || *p == '\\') base = p + 1;
  }
  /* Two passes so the result is trimmed at both ends exactly as
   * `cleaned.trim().take(120).trim()` does: leading whitespace is dropped
   * BEFORE the count, or a name that is 119 visible characters and 400
   * spaces would be counted as mostly spaces and lose its tail. */
  char stack[KBC_ATTACH_MAX_FILENAME_BYTES + 1];
  size_t w = 0;
  for (const unsigned char *p = (const unsigned char *)base; *p != '\0'; p++) {
    if (*p < 0x20u || *p == 0x7Fu) continue; /* a control char, ASCII */
    if (*p == '"') {
      if (w + 1 < sizeof stack) stack[w++] = '_';
      continue;
    }
    if (*p >= 0x80u) {
      /* Counted in CHARACTERS: the whole sequence is appended or the name is
       * left unshortened, because appending half of one produces a string
       * that is not valid UTF-8 and goes into a response header. */
      size_t seqlen = 1;
      if (*p >= 0xC0u) seqlen = *p < 0xE0u ? 2 : (*p < 0xF0u ? 3 : 4);
      if (w + seqlen >= sizeof stack) break;
      memcpy(stack + w, p, seqlen);
      w += seqlen;
      p += seqlen - 1;
      continue;
    }
    if (w + 1 < sizeof stack) stack[w++] = (char)*p;
  }
  stack[w] = '\0';
  char *s = stack;
  while (*s == ' ' || *s == '\t') s++;
  size_t keep = 0;
  uint32_t chars = 0;
  /* The 120-character cap, counted on CODE POINTS and cutting on a
   * boundary, which is the only way the result is still a valid name. */
  const unsigned char *p = (const unsigned char *)s;
  while (p[keep] != '\0') {
    size_t seqlen = 1;
    if (p[keep] >= 0xC0u)
      seqlen = p[keep] < 0xE0u ? 2 : (p[keep] < 0xF0u ? 3 : 4);
    if (chars == 120u) break;
    keep += seqlen;
    chars++;
  }
  size_t end = keep;
  while (end > 0 && (s[end - 1] == ' ' || s[end - 1] == '\t')) end--;
  if (end == 0 || (end == 1 && s[0] == '.') ||
      (end == 2 && s[0] == '.' && s[1] == '.')) {
    return kbc_arena_strdup(a, "file");
  }
  return kbc_arena_strndup(a, s, end);
}

/* How many ADOPTED rows a comment already holds. The per-target cap is
 * checked against this inside the adopting transaction, so a comment cannot
 * cross the cap between the count and the write. */
static int64_t attachment_count_for_comment(kbc_err *err, kbc_store *s,
                                            const char *comment_id) {
  int64_t n = 0;
  kbc_status rc = count_query(
      err, s,
      "SELECT COUNT(*) FROM attachments WHERE comment_id = ?1;", comment_id,
      &n);
  return rc == KBC_OK ? n : -1;
}

/* ONE statement, and the reason there is no transaction to open.
 *
 * The bytes and the row are the same INSERT, so the engine's implicit
 * per-statement transaction already makes "a blob with no row" and "a row
 * with no blob" unrepresentable — the failure mode the invariant "an
 * attachment stored without its row is worse than one refused" is about
 * cannot be reached by a partial write, because there is no partial write.
 * The Rust needs BEGIN-equivalent discipline (fsync, rename, manifest save,
 * rollback of the batch) for exactly the reason this does not.
 *
 * `id_out` is the caller's buffer, sized KBC_ATTACH_ID_LEN + 1, and it is
 * written only on success: a caller that ignores the status cannot act on a
 * half-formed id. */
kbc_status kbc_store_add_attachment(kbc_store *s, const kbc_attachment_in *in,
                                    char id_out[KBC_ATTACH_ID_LEN + 1],
                                    kbc_err *err) {
  if (s == NULL || in == NULL || id_out == NULL)
    return kbc_err_set(err, KBC_ERR_INVALID, "add_attachment: null argument");
  kbc_status st = require_text(err, "attachment doc id", in->doc_id,
                               KBC_MAX_ID_LEN);
  /* An EMPTY comment_id is a STAGED upload, and NULL is the only spelling of
   * that: a caller that means "adopt me onto nothing" and a caller that
   * means "I forgot the comment" are the same request, and treating the
   * empty string as an id to match would be the other reading. */
  if (st == KBC_OK && in->comment_id != NULL && in->comment_id[0] == '\0')
    return kbc_err_set(err, KBC_ERR_INVALID, "attachment comment id: empty");
  if (st == KBC_OK && in->comment_id != NULL)
    st = require_text(err, "attachment comment id", in->comment_id,
                      KBC_MAX_ID_LEN);
  if (st == KBC_OK)
    st = require_text(err, "attachment filename", in->filename,
                      KBC_ATTACH_MAX_FILENAME_BYTES);
  if (st == KBC_OK)
    st = require_text(err, "attachment content type", in->content_type,
                      KBC_ATTACH_MAX_TYPE_BYTES);
  if (st != KBC_OK) return st;
  if (in->body == NULL)
    return kbc_err_set(err, KBC_ERR_INVALID, "attachment body: NULL");
  /* Zero bytes is not an upload. The Rust drops an empty part before it ever
   * reaches the sniffer (`if !buf.is_empty()`), and a zero-length row would
   * be a blob that serves as an empty 200 — an answer about nothing. */
  if (in->body_len == 0)
    return kbc_err_set(err, KBC_ERR_INVALID, "attachment body: 0 bytes");
  if (in->body_len > (size_t)KBC_ATTACH_MAX_BYTES)
    return kbc_err_set(err, KBC_ERR_INVALID,
                       "attachment body: %zu bytes exceeds the %u cap",
                       in->body_len, (unsigned)KBC_ATTACH_MAX_BYTES);
  const char *author = (in->author != NULL && in->author[0] != '\0')
                           ? in->author
                           : "you";
  st = require_text(err, "attachment author", author,
                    KBC_ATTACH_MAX_AUTHOR_BYTES);
  if (st != KBC_OK) return st;

  char id[KBC_ATTACH_ID_LEN + 1];
  mint_attachment_id(id);
  const int64_t now = kbc_now_ns() / 1000000000;

  lock(s);
  /* The per-comment cap, checked HERE as well as in adopt, because a caller
   * may hand a comment_id straight to the insert. The count and the INSERT
   * are separated by nothing another caller can interleave: `lock(s)` is held
   * across both, and one connection behind one mutex is the whole of this
   * store's concurrency (the header's threading model). */
  if (st == KBC_OK && in->comment_id != NULL) {
    const int64_t have = attachment_count_for_comment(err, s, in->comment_id);
    if (have < 0) {
      st = kbc_err_set(err, KBC_ERR_SQL, "attachment count on %s failed",
                       in->comment_id);
    } else if (have >= (int64_t)KBC_ATTACH_MAX_PER_COMMENT) {
      st = kbc_err_set(err, KBC_ERR_CONFLICT,
                       "comment %s already holds %lld attachments, the cap is %u",
                       in->comment_id, (long long)have,
                       (unsigned)KBC_ATTACH_MAX_PER_COMMENT);
    }
  }
  sqlite3_stmt *q = NULL;
  if (st == KBC_OK)
    st = prepare(err, s,
                 "INSERT INTO attachments(id, doc_id, comment_id, filename,"
                 " content_type, size_bytes, body, author, created_at)"
                 " VALUES(?1,?2,?3,?4,?5,?6,?7,?8,?9);",
                 &q);
  if (st == KBC_OK) st = bind_text(err, s, q, 1, id);
  if (st == KBC_OK) st = bind_text(err, s, q, 2, in->doc_id);
  /* NULL, not "": comment_id is what says STAGED, and binding an empty
   * string would make a staged row that no later query can recognise. */
  if (st == KBC_OK && in->comment_id != NULL)
    st = bind_text(err, s, q, 3, in->comment_id);
  if (st == KBC_OK) st = bind_text(err, s, q, 4, in->filename);
  if (st == KBC_OK) st = bind_text(err, s, q, 5, in->content_type);
  if (st == KBC_OK)
    st = bind_i64(err, s, q, 6, (int64_t)in->body_len);
  if (st == KBC_OK) {
    /* SQLITE_TRANSIENT, because `in->body` is the request's arena and the
     * statement must not read from it after the caller has moved on. */
    int rc = sqlite3_bind_blob(q, 7, in->body, (int)in->body_len,
                               SQLITE_TRANSIENT);
    if (rc != SQLITE_OK) st = sql_fail(err, s, "bind attachment body", rc);
  }
  if (st == KBC_OK) st = bind_text(err, s, q, 8, author);
  if (st == KBC_OK) st = bind_i64(err, s, q, 9, now);
  if (st == KBC_OK) {
    const int step = sqlite3_step(q);
    if (step == SQLITE_CONSTRAINT)
      st = constraint_fail(err, s, "attachment on this document", "artifacts");
    else if (step != SQLITE_DONE)
      st = sql_fail(err, s, "add attachment", step);
  }
  kbc_status fin = finalize(err, s, q, st);
  if (st == KBC_OK) st = fin;
  unlock(s);
  if (st != KBC_OK) return st;
  memcpy(id_out, id, sizeof id);
  return KBC_OK;
}

/* One row, decoded. The bytes are NOT selected here: a listing must not walk
 * a 10 MiB overflow chain per row to print a filename, which is the same
 * reason ARTIFACT_SLIM_COLS exists. `kbc_store_read_attachment` is the read
 * that fetches them, and there is exactly one. */
static void read_attachment(kbc_arena *a, sqlite3_stmt *q,
                            kbc_attachment *out) {
  memset(out, 0, sizeof(*out));
  out->id = col_str(a, q, 0);
  out->doc_id = col_str(a, q, 1);
  /* sqlite3_column_text on a NULL column yields NULL, and col_str turns that
   * into NULL, which is exactly how a STAGED row says so. */
  out->comment_id = col_str(a, q, 2);
  out->filename = col_str(a, q, 3);
  out->content_type = col_str(a, q, 4);
  out->size_bytes = (int64_t)sqlite3_column_int64(q, 5);
  out->author = col_str(a, q, 6);
  out->created_at = (int64_t)sqlite3_column_int64(q, 7);
  out->staged = out->comment_id == NULL;
}

#define ATTACHMENT_COLS                                                       \
  "id, doc_id, comment_id, filename, content_type, size_bytes, author, "       \
  "created_at"

kbc_status kbc_store_get_attachment(kbc_store *s, kbc_arena *a, const char *id,
                                    kbc_attachment *out, kbc_err *err) {
  if (s == NULL || a == NULL || out == NULL || id == NULL)
    return kbc_err_set(err, KBC_ERR_INVALID, "get_attachment: null argument");
  kbc_status st = require_text(err, "attachment id", id, KBC_ATTACH_ID_LEN);
  if (st != KBC_OK) return st;

  lock(s);
  sqlite3_stmt *q = NULL;
  char sql[128];
  (void)snprintf(sql, sizeof sql,
                 "SELECT " ATTACHMENT_COLS " FROM attachments WHERE id = ?1;");
  st = prepare(err, s, sql, &q);
  if (st == KBC_OK) st = bind_text(err, s, q, 1, id);
  if (st == KBC_OK) {
    const int step = sqlite3_step(q);
    if (step == SQLITE_ROW) {
      read_attachment(a, q, out);
    } else if (step == SQLITE_DONE) {
      st = kbc_err_set(err, KBC_ERR_NOTFOUND, "attachment %s: not found", id);
    } else {
      st = sql_fail(err, s, "get attachment: step", step);
    }
  }
  kbc_status fin = finalize(err, s, q, st);
  unlock(s);
  return st != KBC_OK ? st : fin;
}

/* The bytes, and the ONLY read that fetches them.
 *
 * Bounded by `size_bytes`, which is the column the row was written with and
 * therefore a number this layer controls rather than one a request supplied.
 * A BLOB whose stored length disagrees with the column would be a corrupt
 * row, and `sqlite3_column_bytes` is the measurement that decides: the copy
 * length is the MINIMUM of the two, so a lying column can under-read but
 * never over-read the blob. */
kbc_status kbc_store_read_attachment(kbc_store *s, const char *id, kbc_str *out,
                                     kbc_err *err) {
  if (s == NULL || out == NULL || id == NULL)
    return kbc_err_set(err, KBC_ERR_INVALID, "read_attachment: null argument");
  kbc_status st = require_text(err, "attachment id", id, KBC_ATTACH_ID_LEN);
  if (st != KBC_OK) return st;

  lock(s);
  sqlite3_stmt *q = NULL;
  st = prepare(err, s,
               "SELECT size_bytes, body FROM attachments WHERE id = ?1;", &q);
  if (st == KBC_OK) st = bind_text(err, s, q, 1, id);
  if (st == KBC_OK) {
    const int step = sqlite3_step(q);
    if (step == SQLITE_ROW) {
      const void *blob = sqlite3_column_blob(q, 1);
      const int got = sqlite3_column_bytes(q, 1);
      const int64_t said = (int64_t)sqlite3_column_int64(q, 0);
      if (blob == NULL || got <= 0) {
        st = kbc_err_set(err, KBC_ERR_INTERNAL,
                         "attachment %s: row says %lld bytes and holds none",
                         id, (long long)said);
      } else {
        size_t n = (size_t)got;
        if (said >= 0 && (int64_t)n > said) n = (size_t)said;
        st = kbc_str_append(out, (const char *)blob, n);
      }
    } else if (step == SQLITE_DONE) {
      st = kbc_err_set(err, KBC_ERR_NOTFOUND, "attachment %s: not found", id);
    } else {
      st = sql_fail(err, s, "read attachment: step", step);
    }
  }
  kbc_status fin = finalize(err, s, q, st);
  unlock(s);
  return st != KBC_OK ? st : fin;
}

/* A PAGE, in the corkboard's shape: count-then-fetch over
 * `ORDER BY created_at DESC, id ASC LIMIT ?n`, which is the ordering every
 * listing in this file uses and for the reason the corkboard's header gives.
 * Absence from a page is not evidence about a row.
 *
 * `doc_id` NULL is every document, which is the shape a "what has this corpus
 * got attached" sweep wants. STAGED rows are included, deliberately: a
 * composer that uploaded and has not posted yet still has to see what it
 * uploaded, and a listing that hid them would make a staged id
 * undiscoverable through the API. */
kbc_status kbc_store_list_attachments(kbc_store *s, kbc_arena *a,
                                      const char *doc_id, size_t limit,
                                      kbc_attachment **out, size_t *n_out,
                                      kbc_err *err) {
  if (s == NULL || a == NULL || out == NULL || n_out == NULL)
    return kbc_err_set(err, KBC_ERR_INVALID,
                       "list_attachments: null argument");
  if (limit > KBC_MAX_HITS) limit = KBC_MAX_HITS;
  *out = NULL;
  *n_out = 0;
  if (limit == 0) return KBC_OK;
  if (doc_id != NULL && doc_id[0] == '\0')
    return kbc_err_set(err, KBC_ERR_INVALID, "list_attachments: empty doc id");

  /* Both statements spelled out in full, for `kbc_store_list_anchors`'s
   * reason: composing a filter from a `where` and an `and` puts
   * `AND doc_id = ?1` after a table name whenever doc_id is NULL, and
   * `FROM attachments AND doc_id = ?1` is not a query. */
  const char *count_sql =
      doc_id != NULL ? "SELECT COUNT(*) FROM attachments WHERE doc_id = ?1;"
                     : "SELECT COUNT(*) FROM attachments;";
  const char *list_sql =
      doc_id != NULL
          ? "SELECT " ATTACHMENT_COLS " FROM attachments WHERE doc_id = ?1"
            " ORDER BY created_at DESC, id ASC LIMIT ?2;"
          : "SELECT " ATTACHMENT_COLS " FROM attachments"
            " ORDER BY created_at DESC, id ASC LIMIT ?1;";

  lock(s);
  void *block = NULL;
  size_t n = 0;
  kbc_status st =
      page_block(err, s, a, count_sql, doc_id, limit, sizeof(kbc_attachment),
                 &block, &n);
  sqlite3_stmt *q = NULL;
  if (st == KBC_OK) st = prepare(err, s, list_sql, &q);
  if (st == KBC_OK && doc_id != NULL) st = bind_text(err, s, q, 1, doc_id);
  if (st == KBC_OK)
    st = bind_i64(err, s, q, doc_id != NULL ? 2 : 1, (int64_t)n);
  kbc_attachment *arr = block;
  size_t i = 0;
  while (st == KBC_OK) {
    const int step = sqlite3_step(q);
    if (step == SQLITE_DONE) break;
    if (step != SQLITE_ROW) {
      st = sql_fail(err, s, "list attachments: step", step);
      break;
    }
    if (i >= n) {
      st = kbc_err_set(err, KBC_ERR_INTERNAL,
                       "list attachments: row overflow");
      break;
    }
    read_attachment(a, q, &arr[i]);
    i++;
  }
  kbc_status fin = finalize(err, s, q, st);
  unlock(s);
  if (st != KBC_OK) return st != KBC_OK ? st : fin;
  *out = arr;
  *n_out = i;
  return KBC_OK;
}

/* Binds a STAGED row to the comment that will own it, in ONE transaction,
 * and refuses the two things the Rust's `adopt_staged` refuses.
 *
 * The transaction is what makes the cap a cap. `adopt_staged` counts the
 * target's existing attachments and then pushes, under a `review_lock` that
 * serialises the same file; here the count and the UPDATE are the same
 * transaction on the same connection, which is the same guarantee without
 * inventing a second lock for a table this file already owns.
 *
 * The `comment_id IS NULL` predicate is what makes adoption ONE-TIME. An id
 * already adopted by a DIFFERENT comment updates zero rows, and zero rows is
 * reported as a CONFLICT naming the id rather than as a success — two
 * comments sharing one blob is a claim about the blob's owner that nobody
 * made. */
kbc_status kbc_store_adopt_attachment(kbc_store *s, const char *id,
                                      const char *comment_id, kbc_err *err) {
  if (s == NULL)
    return kbc_err_set(err, KBC_ERR_INVALID, "adopt_attachment: null store");
  kbc_status st = require_text(err, "attachment id", id, KBC_ATTACH_ID_LEN);
  if (st == KBC_OK)
    st = require_text(err, "adopting comment id", comment_id, KBC_MAX_ID_LEN);
  if (st != KBC_OK) return st;

  lock(s);
  st = exec_plain(err, s, "BEGIN IMMEDIATE;");
  if (st != KBC_OK) {
    unlock(s);
    return st;
  }
  const int64_t have = attachment_count_for_comment(err, s, comment_id);
  if (have < 0) {
    st = kbc_err_set(err, KBC_ERR_SQL, "attachment count on %s failed",
                     comment_id);
  } else if (have >= (int64_t)KBC_ATTACH_MAX_PER_COMMENT) {
    st = kbc_err_set(err, KBC_ERR_CONFLICT,
                     "comment %s already holds %lld attachments, the cap is %u",
                     comment_id, (long long)have,
                     (unsigned)KBC_ATTACH_MAX_PER_COMMENT);
  }
  sqlite3_stmt *q = NULL;
  if (st == KBC_OK)
    st = prepare(err, s,
                 "UPDATE attachments SET comment_id = ?2"
                 " WHERE id = ?1 AND comment_id IS NULL;",
                 &q);
  if (st == KBC_OK) st = bind_text(err, s, q, 1, id);
  if (st == KBC_OK) st = bind_text(err, s, q, 2, comment_id);
  if (st == KBC_OK) {
    const int step = sqlite3_step(q);
    if (step != SQLITE_DONE) st = sql_fail(err, s, "adopt attachment", step);
  }
  kbc_status fin = finalize(err, s, q, st);
  if (st == KBC_OK) st = fin;
  /* Read HERE, under the mutex, for the reason prune_history gives: the
   * counter is per-CONNECTION, so a statement another thread steps between
   * the unlock and the read replaces the value this is about to report. */
  if (st == KBC_OK && sqlite3_changes(s->db) == 0) {
    st = kbc_err_set(err, KBC_ERR_CONFLICT,
                     "attachment %s: not a staged upload, or already adopted",
                     id);
  }
  if (st == KBC_OK) st = exec_plain(err, s, "COMMIT;");
  if (st != KBC_OK) rollback(s);
  unlock(s);
  return st;
}

/* Detach and drop, and the ORDER is the argument.
 *
 * The row goes first, in a transaction, and the caller is told NOTFOUND
 * when there was no row — so "detach an attachment that was never attached"
 * is an answer rather than a silent success. The Rust answers the same way
 * (`remove_comment_attachment` returns NotFound for a missing aid) and then
 * leaves the blob for `gc_manifest`; here the row and the bytes are the same
 * row, so the delete takes both and there is nothing left for a GC to find.
 * The GC below exists for the one case the Rust's GC is really for: a
 * STAGED upload nobody ever adopted. */
kbc_status kbc_store_delete_attachment(kbc_store *s, const char *id,
                                       kbc_err *err) {
  if (s == NULL)
    return kbc_err_set(err, KBC_ERR_INVALID,
                       "delete_attachment: null store");
  kbc_status st = require_text(err, "attachment id", id, KBC_ATTACH_ID_LEN);
  if (st != KBC_OK) return st;

  lock(s);
  sqlite3_stmt *q = NULL;
  st = prepare(err, s, "DELETE FROM attachments WHERE id = ?1;", &q);
  if (st == KBC_OK) st = bind_text(err, s, q, 1, id);
  if (st == KBC_OK) {
    const int step = sqlite3_step(q);
    if (step != SQLITE_DONE) st = sql_fail(err, s, "delete attachment", step);
  }
  kbc_status fin = finalize(err, s, q, st);
  if (st == KBC_OK) st = fin;
  if (st == KBC_OK && sqlite3_changes(s->db) == 0)
    st = kbc_err_set(err, KBC_ERR_NOTFOUND, "attachment %s: not found", id);
  unlock(s);
  return st;
}

/* `gc_plan`'s one arm that survives the port, and it is the arm the whole
 * grace window exists for: a STAGED upload nobody ever adopted
 * (attachments.rs:213-227, "staged + past grace -> delete (abandoned
 * upload)").
 *
 * The other three arms are GONE and each for a reason rather than for
 * convenience:
 *
 *   adopted + referenced  -> keep. There is no code path here.
 *   adopted + unreferenced -> delete now. The original has to reach this
 *     arm by reference-counting, because its bytes outlive the comment that
 *     named them. Here `attachments.comment_id` CASCADES, so an unreferenced
 *     adopted row is not a state the schema admits: the delete happened with
 *     the comment that made it an orphan.
 *   staged + within grace  -> keep. That is the `comment_id IS NULL`
 *     predicate below doing nothing.
 *
 * So this is not a weakened GC, it is the GC with the other three arms
 * discharged by construction, and a caller asking it to reap an adopted
 * attachment is asking for something that cannot be stale.
 *
 * A negative `grace_seconds` reaps NOTHING rather than everything, for
 * `kbc_store_prune_history`'s reason: a maintenance call whose arithmetic
 * went wrong must not become the verb that destroys the most.
 *
 * `created_at` is an INTEGER here rather than the RFC-3339 string the
 * original's manifest carries, and that is what makes the comparison below
 * arithmetic. A TEXT timestamp would make "is this older than the grace
 * window" a string comparison, which is correct exactly as long as every
 * writer agrees on the format — a property nothing enforces. The wire shape
 * is unchanged: the route formats it back to ISO-8601 on the way out. */
kbc_status kbc_store_prune_attachments(kbc_store *s, int64_t now_unix,
                                       int64_t grace_seconds, int64_t *rows,
                                       kbc_err *err) {
  if (s == NULL || rows == NULL)
    return kbc_err_set(err, KBC_ERR_INVALID,
                       "prune_attachments: null argument");
  *rows = 0;
  if (grace_seconds < 0) return KBC_OK;
  lock(s);
  sqlite3_stmt *q = NULL;
  kbc_status st = prepare(
      err, s,
      "DELETE FROM attachments WHERE comment_id IS NULL"
      " AND created_at <= ?1;",
      &q);
  if (st == KBC_OK) st = bind_i64(err, s, q, 1, now_unix - grace_seconds);
  if (st == KBC_OK) {
    const int step = sqlite3_step(q);
    if (step != SQLITE_DONE) st = sql_fail(err, s, "prune attachments", step);
  }
  kbc_status fin = finalize(err, s, q, st);
  if (st == KBC_OK) *rows = (int64_t)sqlite3_changes(s->db);
  unlock(s);
  return st != KBC_OK ? st : fin;
}

/* ------------------------------------------------- deleting a comment --
 *
 * The original's `delete_comment` returns the removed comment so its caller
 * can hand the attachment GC the ids that just lost their owner
 * (comments.rs, via `adopt_staged`/`gc_manifest` being `pub(crate)` "so the
 * create-time adoption in routes::comments (add_comment / add_reply /
 * delete_comment) reuses them"). Here nothing has to be handed anywhere: the
 * schema's own cascade removes the attachment rows and their bytes in this
 * commit, and `comment_anchors` goes with them for the same reason it has
 * since v13.
 *
 * The claim is only true because `kbc_store_open` runs
 * `PRAGMA foreign_keys = ON` (store.c:1122). A writer without it holds
 * orphans, which is the state `report_foreign_key_violations` names on every
 * open and `kbc_store_reconcile_anchors` sweeps — so a volume that reached
 * here by some other writer is repaired on the next open rather than
 * silently believed.
 *
 * ONE transaction and ONE statement. A delete that reported success while
 * leaving the attachments behind would be the half-applied failure rule 10
 * is about, and there is no way to get one here: the engine either applies
 * the cascade or rolls the whole statement back. */
kbc_status kbc_store_delete_comment(kbc_store *s, const char *comment_id,
                                    kbc_err *err) {
  if (s == NULL)
    return kbc_err_set(err, KBC_ERR_INVALID, "delete_comment: null store");
  kbc_status st = require_text(err, "comment id", comment_id, KBC_MAX_ID_LEN);
  if (st != KBC_OK) return st;

  lock(s);
  sqlite3_stmt *q = NULL;
  st = prepare(err, s, "DELETE FROM comments WHERE id = ?1;", &q);
  if (st == KBC_OK) st = bind_text(err, s, q, 1, comment_id);
  if (st == KBC_OK) {
    const int step = sqlite3_step(q);
    if (step != SQLITE_DONE) st = sql_fail(err, s, "delete comment", step);
  }
  kbc_status fin = finalize(err, s, q, st);
  if (st == KBC_OK) st = fin;
  if (st == KBC_OK && sqlite3_changes(s->db) == 0)
    st = kbc_err_set(err, KBC_ERR_NOTFOUND, "comment %s: not found",
                     comment_id);
  unlock(s);
  return st;
}

/* ============================================================= verdicts ==
 *
 * The review pass's own answer about a document, which is a different thing
 * from any comment's open/resolved. The original keeps it in the review
 * file as `ReviewFile.verdict: Option<Verdict>` with a three-state
 * `VerdictState` — Comment, Approve, RequestChanges — and says why the two
 * concepts are separate: a document can be APPROVED with three comments
 * open, and can hold an open comment with no verdict at all.
 */

/* The facet value a verdict projects onto the document, and the one it
 * withdraws. A `COMMENT` verdict projects NEITHER: review.rs is explicit
 * that the display shortcut "drops any prior `status-*` tag rather than
 * writing one for this state", because Comment carries no pass/fail signal
 * and a tag saying otherwise would be a claim the verdict never made.
 * `NULL` here is that withdrawal, and it is a distinct outcome from either
 * tag rather than an absent case. */
static const char *verdict_tag(kbc_verdict_state state) {
  if (state == KBC_VERDICT_APPROVE) return VERDICT_TAG_VALUES[0];
  if (state == KBC_VERDICT_REQUEST_CHANGES) return VERDICT_TAG_VALUES[1];
  return NULL;
}

/* The artifact's (corpus, path), which is what `doc_metas` is keyed on and
 * the only thing the tag mirror can be written against. Read inside the
 * caller's transaction so a document deleted by another writer between the
 * read and the write is a constraint failure rather than a tag on nothing. */
static kbc_status verdict_target(kbc_err *err, kbc_store *s, const char *doc_id,
                                 char *corpus, size_t corpus_cap, char *path,
                                 size_t path_cap, bool *found) {
  *found = false;
  sqlite3_stmt *q = NULL;
  kbc_status st = prepare(
      err, s, "SELECT corpus, path FROM artifacts WHERE id = ?1;", &q);
  if (st == KBC_OK) st = bind_text(err, s, q, 1, doc_id);
  if (st == KBC_OK) {
    const int step = sqlite3_step(q);
    if (step == SQLITE_ROW) {
      const char *c = (const char *)sqlite3_column_text(q, 0);
      const char *p = (const char *)sqlite3_column_text(q, 1);
      const int cb = sqlite3_column_bytes(q, 0);
      const int pb = sqlite3_column_bytes(q, 1);
      /* Bounded, never truncated. `sources` admits a corpus of at most 255
       * bytes and a path at most KBC_MAX_PATH_LEN, so a longer one was
       * written outside the API and copying a prefix would put a tag on a
       * document that is not this one. */
      if (c == NULL || p == NULL || cb <= 0 || (size_t)cb >= corpus_cap ||
          pb <= 0 || (size_t)pb >= path_cap) {
        st = kbc_err_set(err, KBC_ERR_INTERNAL,
                         "verdict on %s: corpus/path is %d/%d bytes, over the"
                         " %zu/%zu this table allows",
                         doc_id, cb, pb, corpus_cap - 1, path_cap - 1);
      } else {
        memcpy(corpus, c, (size_t)cb);
        corpus[cb] = '\0';
        memcpy(path, p, (size_t)pb);
        path[pb] = '\0';
        *found = true;
      }
    } else if (step == SQLITE_DONE) {
      *found = false;
    } else {
      st = sql_fail(err, s, "read verdict target: step", step);
    }
  }
  kbc_status fin = finalize(err, s, q, st);
  return st != KBC_OK ? st : fin;
}

/* Replaces the whole derived `status-*` set for one document: both values
 * dropped, then the one the verdict names inserted. Two statements rather
 * than a conditional, because the interesting case is the one where the
 * verdict CHANGES and the previous tag has to disappear — a document that
 * was approved and is now asking for changes must not keep matching
 * `kb-tags:status-approved`, or the facet filter answers a question about a
 * state the verdict has moved on from. */
static kbc_status verdict_retag(kbc_err *err, kbc_store *s, const char *corpus,
                                const char *path,
                                kbc_verdict_state state) {
  sqlite3_stmt *del = NULL;
  kbc_status st = prepare(
      err, s,
      "DELETE FROM doc_metas WHERE corpus = ?1 AND path = ?2"
      " AND key = 'kb-tags' AND (value = 'status-approved'"
      " OR value = 'status-changes-requested');",
      &del);
  if (st == KBC_OK) st = bind_text(err, s, del, 1, corpus);
  if (st == KBC_OK) st = bind_text(err, s, del, 2, path);
  if (st == KBC_OK) {
    const int step = sqlite3_step(del);
    if (step != SQLITE_DONE) st = sql_fail(err, s, "clear verdict tag", step);
  }
  kbc_status fin = finalize(err, s, del, st);
  if (st == KBC_OK) st = fin;
  const char *tag = verdict_tag(state);
  if (st != KBC_OK || tag == NULL) return st;
  sqlite3_stmt *ins = NULL;
  st = prepare(err, s,
               "INSERT OR IGNORE INTO doc_metas(corpus, path, key, value)"
               " VALUES(?1,?2,'kb-tags',?3);",
               &ins);
  if (st == KBC_OK) st = bind_text(err, s, ins, 1, corpus);
  if (st == KBC_OK) st = bind_text(err, s, ins, 2, path);
  if (st == KBC_OK) st = bind_text(err, s, ins, 3, tag);
  if (st == KBC_OK) {
    const int step = sqlite3_step(ins);
    if (step != SQLITE_DONE) st = sql_fail(err, s, "write verdict tag", step);
  }
  kbc_status fin2 = finalize(err, s, ins, st);
  return st != KBC_OK ? st : fin2;
}

/* The verdict AND its tag, in ONE transaction, which is what makes the tag
 * safe to filter on.
 *
 * The original's invariant #12 says the direction of the derivation, and
 * this is what enforces it in one place: `verdicts` is the verdict of
 * record, `doc_metas` is the projection, and a transaction that wrote one
 * without the other would leave `kb-tags:status-approved` matching a
 * document whose verdict says otherwise. There is no repair pass for that
 * and there does not need to be one, because the two are never apart.
 *
 * `decided_at` is STAMPED HERE and never taken from the caller, for
 * review.rs's reason: "at/by are stamped by set_verdict (never
 * client-supplied), mirroring Comment::created_at/author". A caller-supplied
 * timestamp is a claim about when a human decided, and a client can make it
 * say anything.
 *
 * `decided_by` IS the caller's, because it is attribution rather than
 * fact: it names who, not when, and the original takes it from the request's
 * resolved identity the same way. */
kbc_status kbc_store_set_verdict(kbc_store *s, const char *doc_id,
                                 kbc_verdict_state state,
                                 const char *decided_by, const char *note,
                                 kbc_err *err) {
  if (s == NULL)
    return kbc_err_set(err, KBC_ERR_INVALID, "set_verdict: null store");
  if (state != KBC_VERDICT_COMMENT && state != KBC_VERDICT_APPROVE &&
      state != KBC_VERDICT_REQUEST_CHANGES)
    return kbc_err_set(err, KBC_ERR_INVALID, "verdict state %d: not one of "
                                           "comment|approve|request_changes",
                       (int)state);
  kbc_status st = require_text(err, "verdict doc id", doc_id, KBC_MAX_ID_LEN);
  if (st == KBC_OK)
    st = require_text(err, "verdict decided_by", decided_by,
                      KBC_VERDICT_MAX_BYTES);
  if (st == KBC_OK && note != NULL && strlen(note) > KBC_VERDICT_MAX_NOTE_BYTES)
    return kbc_err_set(err, KBC_ERR_INVALID,
                       "verdict note: over the %u byte cap",
                       (unsigned)KBC_VERDICT_MAX_NOTE_BYTES);
  if (st != KBC_OK) return st;

  char corpus[256];
  char path[KBC_MAX_PATH_LEN + 1];
  bool found = false;
  const int64_t now = kbc_now_ns() / 1000000000;

  lock(s);
  st = exec_plain(err, s, "BEGIN IMMEDIATE;");
  if (st != KBC_OK) {
    unlock(s);
    return st;
  }
  st = verdict_target(err, s, doc_id, corpus, sizeof corpus, path, sizeof path,
                     &found);
  if (st == KBC_OK && !found) {
    st = kbc_err_set(err, KBC_ERR_NOTFOUND, "artifact %s: not indexed", doc_id);
  }
  sqlite3_stmt *q = NULL;
  if (st == KBC_OK)
    st = prepare(err, s,
                 "INSERT INTO verdicts(doc_id, state, decided_at, decided_by,"
                 " note) VALUES(?1,?2,?3,?4,?5)"
                 " ON CONFLICT(doc_id) DO UPDATE SET state=excluded.state,"
                 " decided_at=excluded.decided_at,"
                 " decided_by=excluded.decided_by, note=excluded.note;",
                 &q);
  if (st == KBC_OK) st = bind_text(err, s, q, 1, doc_id);
  if (st == KBC_OK) st = bind_i64(err, s, q, 2, (int64_t)state);
  if (st == KBC_OK) st = bind_i64(err, s, q, 3, now);
  if (st == KBC_OK) st = bind_text(err, s, q, 4, decided_by);
  /* NULL note is "no note", not an empty one, and the two must not be the
   * same row: `note: ""` and an absent note are different claims and the
   * CHECK-free column has no way to tell them apart if the empty string is
   * written. */
  if (st == KBC_OK && note != NULL && note[0] != '\0')
    st = bind_text(err, s, q, 5, note);
  if (st == KBC_OK) {
    const int step = sqlite3_step(q);
    if (step == SQLITE_CONSTRAINT)
      st = constraint_fail(err, s, "verdict on this document", "artifacts");
    else if (step != SQLITE_DONE)
      st = sql_fail(err, s, "set verdict", step);
  }
  kbc_status fin = finalize(err, s, q, st);
  if (st == KBC_OK) st = fin;
  if (st == KBC_OK) st = verdict_retag(err, s, corpus, path, state);
  if (st == KBC_OK) st = exec_plain(err, s, "COMMIT;");
  if (st != KBC_OK) rollback(s);
  unlock(s);
  return st;
}

kbc_status kbc_store_get_verdict(kbc_store *s, kbc_arena *a, const char *doc_id,
                                 kbc_verdict *out, kbc_err *err) {
  if (s == NULL || a == NULL || out == NULL || doc_id == NULL)
    return kbc_err_set(err, KBC_ERR_INVALID, "get_verdict: null argument");
  kbc_status st = require_text(err, "verdict doc id", doc_id, KBC_MAX_ID_LEN);
  if (st != KBC_OK) return st;

  lock(s);
  sqlite3_stmt *q = NULL;
  st = prepare(err, s,
               "SELECT state, decided_at, decided_by, note FROM verdicts"
               " WHERE doc_id = ?1;",
               &q);
  if (st == KBC_OK) st = bind_text(err, s, q, 1, doc_id);
  if (st == KBC_OK) {
    const int step = sqlite3_step(q);
    if (step == SQLITE_ROW) {
      memset(out, 0, sizeof(*out));
      /* The CHECK constraint is what makes this cast sound; a value the
       * table cannot hold cannot arrive here. */
      out->state = (kbc_verdict_state)sqlite3_column_int(q, 0);
      out->decided_at = (int64_t)sqlite3_column_int64(q, 1);
      out->decided_by = col_str(a, q, 2);
      out->note = col_str(a, q, 3);
    } else if (step == SQLITE_DONE) {
      /* NOTFOUND is "nobody has reviewed this", which is a DIFFERENT answer
       * from a COMMENT verdict — a working note with no pass/fail signal.
       * Collapsing them would let a client report "under review" for a
       * document nobody has looked at. */
      st = kbc_err_set(err, KBC_ERR_NOTFOUND, "verdict for %s: none set",
                       doc_id);
    } else {
      st = sql_fail(err, s, "get verdict: step", step);
    }
  }
  kbc_status fin = finalize(err, s, q, st);
  unlock(s);
  return st != KBC_OK ? st : fin;
}

kbc_status kbc_store_clear_verdict(kbc_store *s, const char *doc_id,
                                   kbc_err *err) {
  if (s == NULL)
    return kbc_err_set(err, KBC_ERR_INVALID, "clear_verdict: null store");
  kbc_status st = require_text(err, "verdict doc id", doc_id, KBC_MAX_ID_LEN);
  if (st != KBC_OK) return st;

  char corpus[256];
  char path[KBC_MAX_PATH_LEN + 1];
  bool found = false;

  lock(s);
  st = exec_plain(err, s, "BEGIN IMMEDIATE;");
  if (st != KBC_OK) {
    unlock(s);
    return st;
  }
  /* The document is looked up even on the clearing path, because the tag has
   * to be withdrawn from the same (corpus, path) the verdict was projected
   * onto. Clearing only the row would leave the tag behind and a filter on
   * `kb-tags:status-approved` would keep matching a document that has no
   * verdict at all — a facet answer with nothing behind it. */
  st = verdict_target(err, s, doc_id, corpus, sizeof corpus, path, sizeof path,
                     &found);
  sqlite3_stmt *q = NULL;
  if (st == KBC_OK)
    st = prepare(err, s, "DELETE FROM verdicts WHERE doc_id = ?1;", &q);
  if (st == KBC_OK) st = bind_text(err, s, q, 1, doc_id);
  if (st == KBC_OK) {
    const int step = sqlite3_step(q);
    if (step != SQLITE_DONE) st = sql_fail(err, s, "clear verdict", step);
  }
  kbc_status fin = finalize(err, s, q, st);
  if (st == KBC_OK) st = fin;
  if (st == KBC_OK && sqlite3_changes(s->db) == 0) {
    st = kbc_err_set(err, KBC_ERR_NOTFOUND, "verdict for %s: none set", doc_id);
  }
  /* Only reached when a row really was there, and `found` then has to be
   * true: a verdict row cascades with its artifact, so a verdict without a
   * document is a row this connection's own cascade should have removed. */
  if (st == KBC_OK && found)
    st = verdict_retag(err, s, corpus, path, KBC_VERDICT_COMMENT);
  if (st == KBC_OK) st = exec_plain(err, s, "COMMIT;");
  if (st != KBC_OK) rollback(s);
  unlock(s);
  return st;
}
