/* store.h — SQLite persistence for artifacts, chunks and comments.
 *
 * Concurrency: one connection, one mutex. Every public call takes it
 * internally; a `kbc_store` is safe to share across the httpd's worker threads.
 * WAL + NORMAL synchronous: a crash loses at most the last transaction, never
 * the database. Statements are prepared per call and finalized before return,
 * so there is no cached-statement lifetime to get wrong.
 */
#ifndef KBC_STORE_H
#define KBC_STORE_H

#include "kbc/config.h"
#include "kbc/kbc.h"
#include "kbc/mem.h"
#include "kbc/types.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct kbc_store kbc_store;

/* Opens (creating if needed) the database at cfg->db_path, creating parent
 * directories, and applies migrations up to the schema version. */
kbc_store *kbc_store_open(const kbc_config *cfg, kbc_err *err);
void kbc_store_close(kbc_store *s);
int kbc_store_schema_version(const kbc_store *s);

/* ------------------------------------------------------------ artifacts -- */

kbc_status kbc_store_upsert_artifact(kbc_store *s, const kbc_artifact *a,
                                     kbc_err *err);
/* ARENA record; `with_source` controls whether ->source is populated. */
kbc_status kbc_store_get_artifact(kbc_store *s, kbc_arena *a, const char *id,
                                  bool with_source, kbc_artifact *out,
                                  kbc_err *err);
/* Resolves corpus+relative path to an id, for the watcher path. */
kbc_status kbc_store_get_artifact_by_path(kbc_store *s, kbc_arena *a,
                                          const char *corpus, const char *path,
                                          kbc_artifact *out, kbc_err *err);
kbc_status kbc_store_delete_artifact(kbc_store *s, const char *id,
                                     kbc_err *err);

/* `corpus` NULL = every corpus. `ids_out` is KBC_OWN, NUL-terminated entries;
 * free each with free() and the array with free(). limit is clamped to
 * KBC_MAX_HITS. */
kbc_status kbc_store_list_artifact_ids(kbc_store *s, const char *corpus,
                                       kbc_kind kind, size_t limit,
                                       size_t offset, char ***ids_out,
                                       size_t *n_out, kbc_err *err);
kbc_status kbc_store_count_artifacts(kbc_store *s, const char *corpus,
                                     int64_t *out, kbc_err *err);
kbc_status kbc_store_list_corpora(kbc_store *s, kbc_strlist *out, kbc_err *err);
kbc_status kbc_store_total_bytes(kbc_store *s, int64_t *out, kbc_err *err);
/* Resolves `n` (corpus, path) pairs in ONE round trip, writing the result to
 * `*out`: a KBC_OWN array of EXACTLY n slots, parallel to the input. Each
 * non-NULL slot points at a KBC_ARENA kbc_artifact in `a`; a NULL slot means
 * that pair has no row. The caller free()s the array itself; the artifacts
 * belong to the arena.
 *
 * The parallel shape is deliberate: a search resolving its top-k hits must be
 * able to ask for all of them at once instead of taking the store's lock once
 * per hit, and it must still be able to tell WHICH hit did not resolve. Pairs
 * are de-duplicated internally, so a result set naming the same document twice
 * fetches it once. n == 0 is KBC_OK with *out set to NULL. */
kbc_status kbc_store_get_artifacts_by_path(kbc_store *s, kbc_arena *a,
                                           const char *const *corpora,
                                           const char *const *paths, size_t n,
                                           kbc_artifact ***out, kbc_err *err);

/* ------------------------------------------------------------------ edges -- */

/* The corpus link graph. One row per outbound link whose target resolves to an
 * indexed document; a target that does not resolve is simply not an edge, so
 * the table is a subset of what the parser found, never a superset.
 *
 * In-degree is BACKLINKS ONLY. The Rust daemon deliberately ignores
 * out-degree when boosting (fusion.rs:214, fed from storage.edge_counts()), and
 * this table exists to compute that same number.
 *
 * CONVERGENCE. Edges are owned by the SOURCE and are rewritten only when the
 * SOURCE is re-ingested, so a document that arrives after a document linking to
 * it would otherwise stay at in-degree 0 forever. They do not, because a target
 * that did not exist at write time is recorded in `pending_links` (schema v3) and
 * drained when the target arrives. A full reindex drains for EVERY document the
 * walk saw, including unchanged ones, which is what makes the graph independent
 * of the order documents were visited. `kbc_store_forget_document` demotes a
 * removed document's inbound edges to pending rows rather than dropping them, so
 * a document that comes back finds its backlinks. The property is asserted, not
 * argued: tests/test_app.c::the_link_graph_does_not_depend_on_the_ingest_order
 * ingests the same two-document corpus in both orders and requires an identical
 * edge set and identical in-degrees.
 *
 * What is still true, and why the boost defaults to off: the graph is complete,
 * but the boost's WEIGHT has never been benchmarked. graph_boost = 0.0 is the
 * default because an unmeasured ranking weight should not change anyone's
 * results; see DECISIONS.md ADR-004. */
kbc_status kbc_store_replace_edges(kbc_store *s, const char *corpus,
                                   const char *src_path,
                                   const char *const *dst_paths, size_t n,
                                   kbc_err *err);
/* Drops every edge leaving (corpus, src_path). Called when a document's content
 * changes or it disappears, so a re-link cannot leave a stale edge behind. */
kbc_status kbc_store_delete_edges(kbc_store *s, const char *corpus,
                                  const char *src_path, kbc_err *err);
/* The in-degree (backlink count) of each of `n` NAMED documents, in one
 * statement, written into the caller's `in_deg` array in the order given. A
 * document with no backlinks is 0; a document that does not exist is also 0,
 * so a caller never has to distinguish them. n == 0 is KBC_OK and touches
 * nothing.
 *
 * The caller names documents by PATH because the store is the only layer that
 * knows paths: an index doc id means nothing here, and the index deliberately
 * does not carry the graph. This is a single aggregate over the table, not a
 * lookup per document. */
kbc_status kbc_store_edge_degrees_for(kbc_store *s, const char *corpus,
                                      const char *const *paths, size_t n,
                                      uint32_t *in_deg, kbc_err *err);
int64_t kbc_store_edge_count(kbc_store *s, const char *corpus, kbc_err *err);

/* The documents that link HERE, which is the other direction of the same
 * table. `edge_degrees_for` answers "how many" for documents the caller
 * already named; this answers "which", and a backlinks surface cannot be built
 * from a count.
 *
 * `out` receives KBC_OWN corpus-relative source paths, de-duplicated, and the
 * caller frees it with kbc_strlist_free. NOTHING LINKING HERE IS A ZERO-LENGTH
 * LIST AND KBC_OK, never KBC_ERR_NOTFOUND: the original is explicit that
 * "nothing links here" is a 200 with an empty array, and turning it into a
 * 404 would make an ordinary document look missing.
 *
 * One query. A round trip per backlink would be the same defect the edge
 * write has already been bitten by. */
kbc_status kbc_store_list_backlinks(kbc_store *s, const char *corpus,
                                    const char *path, kbc_strlist *out,
                                    kbc_err *err);

/* ------------------------------------------------------- pending links -- */

/* A link whose target did not exist when the source was ingested. This is what
 * makes the graph order-independent: a document arriving after a document that
 * links to it finds the link waiting instead of losing it.
 *
 * The drain is order-independent because of WHERE it is called, not what it
 * does: a full reindex drains for EVERY document the walk saw, including
 * unchanged ones, so an unchanged source still completes the graph. */
kbc_status kbc_store_add_pending_links(kbc_store *s, const char *corpus,
                                       const char *src_path,
                                       const char *const *dst_paths, size_t n,
                                       kbc_err *err);
/* Materialises every pending link pointing at dst_path whose source is still a
 * document, then deletes those rows. ONE transaction: an edge added with the
 * row kept would be inserted forever, and a row deleted without its edge loses
 * the link permanently. */
kbc_status kbc_store_drain_pending(kbc_store *s, const char *corpus,
                                   const char *dst_path, kbc_err *err);
kbc_status kbc_store_delete_pending(kbc_store *s, const char *corpus,
                                    const char *src_path, kbc_err *err);
int64_t kbc_store_pending_count(kbc_store *s, kbc_err *err);
/* Forgets a document entirely: drops the edges leaving it AND the pending rows
 * leaving it, and DEMOTES its inbound edges to pending rows rather than
 * dropping them, so a document that comes back finds its backlinks waiting.
 * ONE transaction. Without the demotion, removing a document leaves a dangling
 * edge pointing at something that is no longer indexed. */
kbc_status kbc_store_forget_document(kbc_store *s, const char *corpus,
                                      const char *path, kbc_err *err);


/* --------------------------------------------------------- doc metadata -- */

/* Filterable facets, replaced whole on every ingest so a value the document no
 * longer declares stops matching immediately. The (corpus, key, value) index
 * is what makes "every document with this tag" one query rather than a scan. */
kbc_status kbc_store_replace_metas(kbc_store *s, const char *corpus,
                                   const char *path,
                                   const char *const *keys,
                                   const char *const *values, size_t n,
                                   kbc_err *err);
kbc_status kbc_store_forget_metas(kbc_store *s, const char *corpus,
                                  const char *path, kbc_err *err);
/* Documents carrying (key, value), as KBC_OWN path strings the caller frees.
 * `value == NULL` asks for the key with ANY value. ONE query. An empty result
 * is a ZERO-LENGTH array — a filter that matches nothing must never look like
 * a filter that was ignored. */
kbc_status kbc_store_docs_with_meta(kbc_store *s, const char *corpus,
                                    const char *key, const char *value,
                                    char ***paths_out, size_t *n_out,
                                    kbc_err *err);

/* ---------------------------------------------------------------- chunks -- */

typedef struct {
  const char *doc_id; /* BORROWED */
  uint32_t ord;
  const char *text;   /* BORROWED */
  size_t text_len;
} kbc_chunk_in;

kbc_status kbc_store_replace_chunks(kbc_store *s, const kbc_chunk_in *chunks,
                                    size_t n, kbc_err *err);
kbc_status kbc_store_list_chunks(kbc_store *s, kbc_arena *a, const char *doc_id,
                                 kbc_block *out, size_t *n_out, kbc_err *err);

/* -------------------------------------------------------------- comments -- */

typedef struct {
  const char *id;      /* ARENA */
  const char *doc_id;  /* ARENA */
  const char *anchor;  /* ARENA, element id or "section:<id>" */
  const char *author;  /* ARENA */
  const char *body;    /* ARENA */
  const char *created_at; /* ARENA, ISO-8601 */
  bool resolved;
} kbc_comment;

kbc_status kbc_store_add_comment(kbc_store *s, const char *doc_id,
                                 const char *anchor, const char *author,
                                 const char *body, kbc_err *err);
kbc_status kbc_store_list_comments(kbc_store *s, kbc_arena *a,
                                   const char *doc_id, size_t limit,
                                   kbc_comment **out, size_t *n_out,
                                   kbc_err *err);
kbc_status kbc_store_set_comment_resolved(kbc_store *s, const char *comment_id,
                                         bool resolved, kbc_err *err);

/* The documents that HAVE comments — the distinct doc_ids the table names.
 * `list_comments` is keyed by doc_id and has no "all comments" form, so a
 * caller that needs to walk every commented document otherwise enumerates the
 * corpus and issues one comment query per DOCUMENT. At the 20,000-document
 * corpus in BENCHMARKS.md that is 40,000 prepared statements per reindex, on
 * the watcher's single-file path, to re-check the two or three comments the
 * corpus actually has. With this the pass is O(documents-with-comments), which
 * is what the original pays: it walks the ONE reindexed document's own comment
 * list (indexer.rs:3035), not the corpus's.
 *
 * `out` receives KBC_OWN ids and the caller frees it with kbc_strlist_free.
 * One query, and the same posture as kbc_store_list_backlinks.
 *
 * NOT filtered by corpus, because the comments table carries doc_id only
 * (schema v1) and has no corpus column to filter on. A caller that needs the
 * corpus recovers it from the artifact row — which it needs anyway, since the
 * anchor event's `kb` and `source_relative` both come from there. */
kbc_status kbc_store_list_comment_docs(kbc_store *s, kbc_strlist *out,
                                       kbc_err *err);

/* ------------------------------------------------ stage 1: the rest of it --
 *
 * The Rust original's per-kb SQLite schema is a forward-only migration list
 * of 42 embedded .sql files (kb-core/migrations/, run from Db::open at
 * storage/sqlite.rs:198). kb-c does not take all 42: INVENTORY.md is the scope
 * contract and every OUT-OF-SCOPE row there carries its reason. These are the
 * tables kb-c claims. Their DDL is the Rust DDL, unchanged except for the
 * table and column names kb-c already uses (`corpus` for `source_slug`).
 *
 * `identity_backfill_done` is deliberately NOT ported. It is a once-only
 * marker for `Db::identity_backfill`, which backfills per-user read/unread
 * overrides for list entries. kb-c has no list entries and no per-user
 * override state in its scope, so the marker would gate a pass that has
 * nothing to do — a table with no writer and no reader.
 */

/* ---------------------------------------------------------------- sources -- */

/* One row per indexed root. `slug` is kb-c's corpus name and is the primary
 * key, matching Rust's SourceSlug; `paused` is 0/1, not a bool, because it is
 * an INTEGER column there and a caller may read it back verbatim. */
typedef struct {
  const char *corpus;  /* ARENA */
  const char *path;    /* ARENA */
  int64_t added_at;    /* unix seconds */
  bool paused;
} kbc_source;

/* Inserts or updates by corpus: an existing row keeps its `added_at`, because
 * that is when the source was first seen, not when it was last written. */
kbc_status kbc_store_put_source(kbc_store *s, const kbc_source *src,
                                kbc_err *err);
kbc_status kbc_store_get_source(kbc_store *s, kbc_arena *a, const char *corpus,
                                kbc_source *out, kbc_err *err);
kbc_status kbc_store_list_sources(kbc_store *s, kbc_arena *a, size_t limit,
                                  kbc_source **out, size_t *n_out,
                                  kbc_err *err);
kbc_status kbc_store_set_source_paused(kbc_store *s, const char *corpus,
                                       bool paused, kbc_err *err);

/* ------------------------------------------------------------- index runs -- */

/* One row per reindex pass. `finished_at` is negative while the run is still
 * in flight, which is the Rust convention (a NULL column) expressed in a
 * field that cannot be NULL; a caller reading it must test for that. */
typedef struct {
  const char *id;      /* ARENA, "r-" + 6 base32 */
  const char *corpus;  /* ARENA */
  int64_t started_at;
  int64_t finished_at; /* < 0 while in flight */
  int64_t ok_count;
  int64_t err_count;
} kbc_index_run;

kbc_status kbc_store_put_index_run(kbc_store *s, const kbc_index_run *run,
                                   kbc_err *err);
kbc_status kbc_store_finish_index_run(kbc_store *s, const char *id,
                                      int64_t finished_at, int64_t ok_count,
                                      int64_t err_count, kbc_err *err);
kbc_status kbc_store_list_index_runs(kbc_store *s, kbc_arena *a,
                                     const char *corpus, size_t limit,
                                     kbc_index_run **out, size_t *n_out,
                                     kbc_err *err);

/* ----------------------------------------------------------------- errors -- */

/* One row per ingest failure, keyed by (corpus, path). `retry_count` is what
 * the quarantine gate reads: a document whose retries reach
 * QUARANTINE_THRESHOLD stops being embedded and stays keyword-searchable.
 * `dismissed` is 0/1, and the "open errors" query is a partial index over
 * exactly `dismissed = 0`.
 *
 * `content_hash` is the hash of the file's bytes at the time of the failure,
 * and it is the reason an EDITED document gets a fresh embedding budget: the
 * gate is keyed by (path, content_hash), not by path alone, because editing a
 * document is how an operator fixes one that failed. The original names this
 * `retry_count_for_path_hash`; the C port keeps the hash because the
 * behaviour is the behaviour. */
typedef struct {
  const char *id;      /* ARENA, "e-" + 6 base32 */
  const char *kind;    /* ARENA: parse | io | sqlite | lance | embed */
  const char *corpus;  /* ARENA */
  const char *path;    /* ARENA, source-relative, forward-slash */
  const char *message; /* ARENA */
  const char *content_hash; /* ARENA or NULL */
  int64_t retry_count;
  int64_t created_at;
  bool dismissed;
} kbc_error_row;

/* Records a failure, keyed by (corpus, path): a second failure for the same
 * path increments `retry_count` rather than inserting a duplicate row. */
kbc_status kbc_store_record_error(kbc_store *s, const kbc_error_row *row,
                                  kbc_err *err);
/* The quarantine gate. Returns 0 when there is no row for the path, and a
 * count of 0 when the row was recorded against DIFFERENT content — an edited
 * document is a fresh attempt, not a continuing failure. `content_hash` is the
 * hash of the bytes about to be embedded; pass NULL to read the count for the
 * path alone, which is the "how bad is it" question rather than the "is this
 * document still the one that failed" question.
 *
 * The original's name for this is `retry_count_for_path_hash`
 * (kb-core/src/indexer.rs:2439) and the hash is load-bearing: without it a
 * document that fails, is edited to fix the failure, and is re-indexed never
 * leaves quarantine, and an operator's only remedy is to clear the error by
 * hand. */
kbc_status kbc_store_retry_count_for_path(kbc_store *s, const char *corpus,
                                          const char *path,
                                          const char *content_hash,
                                          int64_t *out, kbc_err *err);
/* No production caller yet, and deliberately so. The errors table IS written —
 * by the quarantine gate, on every embed failure — but the surface that would
 * LIST open errors or clear one is an operator route that is not built, so the
 * rows accumulate and nothing in production reads them. They are not dead
 * weight: `retry_count` is load-bearing for the gate, and these rows are the
 * input the route will read. Add a caller when the route exists; do not add a
 * synthetic one before it.
 *
 * In particular there is deliberately no "clear on a successful reindex" step.
 * The gate reads this row on the next pass, so clearing it there un-gates the
 * document, it takes a real embed, fails, returns at count 1, and a permanently
 * failing document oscillates in and out of quarantine forever. Clearing is
 * operator-only, and that is the whole point of it. */
kbc_status kbc_store_clear_error(kbc_store *s, const char *corpus,
                                 const char *path, kbc_err *err);
kbc_status kbc_store_list_errors(kbc_store *s, kbc_arena *a, const char *corpus,
                                 bool open_only, size_t limit,
                                 kbc_error_row **out, size_t *n_out,
                                 kbc_err *err);

/* -------------------------------------------------------- excluded files --
 *
 * SCHEMA ONLY, no production writer. `excluded_files` records durable operator
 * intent, and the surface that writes it — an exclusions route, so an operator
 * can keep one file out of the index — is not built. Nothing in kb-c creates
 * an exclusion row, so the three functions below have no caller. That is a
 * deliberate state and not an oversight: the writers are declared and
 * implemented so that porting the exclusions surface is a route rather than a
 * schema migration, and the ingest gate that would READ them is named in
 * kbc_store_list_exclusions. What would not be defensible is inventing a
 * writer now — an exclusion nobody sets is a feature nobody asked for, and a
 * table with a writer nobody calls is the dead weight this port keeps trying
 * to avoid. */

/* Operator intent, keyed by the source-relative forward-slash path — the same
 * string the artifact id is hashed from, so an exclusion cannot be attached
 * to the wrong document by a path-normalisation difference. NOT the quarantine
 * pattern: this is durable operator intent, quarantine is failure-driven. */
typedef struct {
  const char *path;        /* ARENA, source-relative forward-slash */
  int64_t excluded_at;     /* unix seconds */
  const char *note;        /* ARENA or NULL */
} kbc_exclusion;

kbc_status kbc_store_add_exclusion(kbc_store *s, const kbc_exclusion *x,
                                   kbc_err *err);
kbc_status kbc_store_remove_exclusion(kbc_store *s, const char *path,
                                      kbc_err *err);
/* Loads every exclusion at daemon bring-up into the caller's ingest gate. A
 * caller that has no gate yet may ignore them safely: nothing reads the table
 * directly, by design, so a half-built daemon degrades to indexing everything
 * rather than to a query that returns the wrong answer. */
kbc_status kbc_store_list_exclusions(kbc_store *s, kbc_arena *a, size_t limit,
                                     kbc_exclusion **out, size_t *n_out,
                                     kbc_err *err);

/* ----------------------------------------------------------- doc history -- */

/* The corkboard (a user's "anchored" artifacts), pinned memories, and the
 * reading/search history. These three exist for ONE reason in this port: a
 * document removal must leave them alone. Rust's CASCADE_STEPS
 * (storage/sqlite.rs:6392) drops artifacts, chunks, comments, edges and
 * friends, and stops there; a user's reading history outliving the document
 * they read is the behaviour to preserve, and it is only assertable if the
 * tables exist.
 *
 * NO PRODUCTION WRITER, for the same reason and with the same force as the
 * exclusions table above. The surfaces that create these rows are the SPA's
 * timeline, the anchor route and the memory pin — stage 3 and stage 4, and not
 * built. So kb-c writes no history row, no corkboard entry and no pin. They
 * are the one place in this port where an EMPTY table is the correct and the
 * TESTED state: `tests/test_app.c` proves a removal leaves them alone, and it
 * could not prove that if the tables did not exist. A writer added before its
 * surface exists would be a feature nobody asked for. */
typedef struct {
  const char *artifact_id; /* ARENA */
  int64_t created_at;      /* unix seconds */
} kbc_corkboard_row;

typedef struct {
  const char *artifact_id; /* ARENA */
  int64_t pinned_at;       /* unix seconds */
} kbc_pin_row;

/* A history row is one of three kinds, discriminated exactly as Rust does
 * with a CHECK-constrained `kind`: "open" (a reading visit, non-null
 * artifact_id), "search" (non-null query), "comment" (non-null comment_id).
 * The kind-specific fields are NULL for the other two. */
typedef struct {
  int64_t id;             /* rowid, 0 on write */
  const char *kind;       /* ARENA: open | search | comment */
  const char *artifact_id; /* ARENA or NULL */
  const char *query;      /* ARENA or NULL */
  const char *comment_id; /* ARENA or NULL */
  const char *source;     /* ARENA or NULL */
  const char *user;       /* ARENA or NULL */
  int64_t scroll_y;
  int64_t scroll_max;
  int64_t scroll_y_max;
  int64_t active_ms;
  const char *last_section; /* ARENA or NULL */
  int64_t started_at;
  int64_t updated_at;
} kbc_history_row;

kbc_status kbc_store_add_history(kbc_store *s, const kbc_history_row *row,
                                 kbc_err *err);
/* Most recent first, which is the order every history surface uses. */
kbc_status kbc_store_list_history(kbc_store *s, kbc_arena *a, const char *user,
                                  size_t limit, kbc_history_row **out,
                                  size_t *n_out, kbc_err *err);

/* Retention. Deletes HISTORY rows older than the cutoff — and nothing else.
 *
 * It does NOT delete documents, and that is the whole shape of it. The
 * original is explicit that it "does NOT manage the R2-cascade tables
 * (sessions/edges/…); those are pruned per-artifact on delete, not by age"
 * (sqlite.rs:2357), and that `edges` "has NO timestamp column, so it CANNOT be
 * time-pruned" (config.rs:470). A retention pass that removed documents by age
 * would be a corpus-deleting feature the original does not have, and it would
 * have to cascade into the index and the graph to stay coherent — which is how
 * a maintenance job becomes the most dangerous verb in a CLI.
 *
 * `apply` false is a dry run: `*rows` reports what WOULD go and nothing is
 * deleted. The original's background task has no dry-run at all, because it is
 * a background job; a foreground command is a different thing, and a
 * maintenance verb whose default destroys is a verb somebody runs by accident.
 *
 * `*rows` is the number removed, or the number that would be removed. A
 * negative cutoff prunes nothing rather than everything. */
kbc_status kbc_store_prune_history(kbc_store *s, int64_t started_before_unix,
                                   bool apply, int64_t *rows, kbc_err *err);

/* Reclaims the space a delete freed, by checkpointing the WAL in TRUNCATE
 * mode. The counterpart of `kbc_store_prune_history`, and the reason that
 * function does not do it itself: under WAL the freed pages stay invisible
 * to the OS until something checkpoints, so without this a long-lived
 * database keeps the file size of a corpus that shrank.
 *
 * BEST-EFFORT, and that is a contract rather than a caveat: the original
 * runs it as `let _ = ... wal_checkpoint(TRUNCATE)` (sqlite.rs:2426), a
 * deliberate discard, because a failed reclaim costs disk and rolling back
 * the delete to make the reclaim succeed would cost the operator rows. Do
 * NOT wire a failure path onto this. It returns KBC_OK unless the STORE
 * itself is unusable; a refused or partial checkpoint is reported as
 * success, because the pages are not lost either way — SQLite reuses them
 * internally and the file shrinks at the next checkpoint that does run.
 *
 * TRUNCATE degrades to a partial checkpoint when a reader holds an older
 * snapshot, which is why "did not shrink the file" is a possible outcome
 * rather than a failure.
 *
 * `incremental_vacuum` is deliberately absent, and is the original's own
 * omission in kb-c's case: the original gates it on an `auto_vacuum =
 * INCREMENTAL` database (config.rs:506-515), and kb-c's store sets only
 * `journal_mode` and `synchronous` (src/store.c:574) and never
 * `auto_vacuum`. Issuing it here would be a silent no-op. */
kbc_status kbc_store_checkpoint(kbc_store *s, kbc_err *err);

/* ------------------------------------------------------------------ moves --
 *
 * A rename that loses its old id is a broken bookmark, so the original records
 * the move and redirects a stale reference to its new home
 * (sqlite.rs:5876 `moves_lookup`, :5928 `moves_suppresses_delete`, :5953
 * `moves_list_incomplete`). Three things depend on it and none of them can be
 * rebuilt from the bytes: a comment anchored to a document that moved, a
 * bookmark to its old id, and a link written against its old path.
 *
 * The table is at migration 11. It is here, and useless, until the functions
 * below exist — which is the shape of this port's recurring gap and the reason
 * it is written down rather than left to be discovered as a bug report. */

/* Re-keys every artifact-referencing row from one document to another, in ONE
 * transaction: `artifacts(id, path)`, `chunks`, `comments`, `corkboard`,
 * `pinned_memories`, `doc_first_seen`, `history`, `edges(src_path, dst_path)`
 * and `pending_links(src_path)`.
 *
 * Everything is `UPDATE OR IGNORE` and then the leftovers are deleted, so the
 * DESTINATION wins on a collision. THE EXCEPTION IS `comments`, and the
 * exception is the whole reason this function exists: a comment row has its own
 * `id` and `created_at`, and the only public way to write one MINTS BOTH. So an
 * ordinary rekey would rewrite the doc_id and leave every carried comment
 * stamped "now" — the one user-visible field a move cannot rebuild from the
 * document's bytes. A caller doing this today has to re-derive the state by
 * matching (anchor, author, body) and keeping a `claimed` bitmap, which is a
 * workaround for a missing capability rather than a design. */
kbc_status kbc_store_rekey_artifact(kbc_store *s, const char *old_id,
                                    const char *new_id, const char *old_rel,
                                    const char *new_rel, kbc_err *err);

/* Records a move, before the rename, so a crash in between leaves an
 * incomplete row that startup can converge rather than a lost document. */
kbc_status kbc_store_record_move(kbc_store *s, const char *old_id,
                                 const char *new_id, const char *old_rel,
                                 const char *new_rel, int64_t moved_at,
                                 kbc_err *err);
kbc_status kbc_store_complete_move(kbc_store *s, const char *old_id,
                                   kbc_err *err);

/* Where a stale reference now lives. CHAIN-WALKED, not a single hop: a path
 * can be renamed more than once, and the original bounds the walk at 64 hops
 * and guards the cycle because a cycle must TERMINATE. A row whose
 * `completed_at` is unset is an interrupted move, not a redirect — a caller
 * that follows one would resolve a document to a name the rename never
 * reached. No row is KBC_ERR_NOTFOUND's business: "this id never moved" is a
 * normal answer and a caller wants the absence, not an error. */
kbc_status kbc_store_moves_lookup(kbc_store *s, const char *id,
                                  kbc_strlist *ids, kbc_err *err);
kbc_status kbc_store_moves_lookup_path(kbc_store *s, const char *rel,
                                       kbc_strlist *rels, kbc_err *err);
/* Moves that started and never finished, so bring-up can converge them. */
kbc_status kbc_store_list_incomplete_moves(kbc_store *s, kbc_strlist *old_ids,
                                           kbc_strlist *old_rels, kbc_err *err);

/* Marks a move as ABANDONED rather than completed.
 *
 * `completed_at` alone has two states and the convergence pass needs three. A
 * rename that never reached the disk still has to leave the replay list — a
 * pass that re-decides it every boot is a loop with a log line — and stamping
 * it "completed" makes `moves_lookup` redirect a stale id to a destination
 * that does not exist. That is a wrong answer, not a missing one, and it is
 * reachable today because `kbc_app_get_artifact` follows the chain.
 *
 * So a third terminal state, and it is explicitly not a redirect: the id stays
 * where it was, and the row is a record that somebody tried and it did not
 * happen. It also fixes the standing warning — a permanent one on every boot
 * for a move the operator has long since given up on.
 *
 * Distinct from `kbc_store_complete_move` in the strongest way available: a
 * completed move is a promise about where the document IS. */
kbc_status kbc_store_abandon_move(kbc_store *s, const char *old_id,
                                  kbc_err *err);

kbc_status kbc_store_add_corkboard(kbc_store *s, const char *artifact_id,
                                   int64_t created_at, kbc_err *err);
kbc_status kbc_store_remove_corkboard(kbc_store *s, const char *artifact_id,
                                      kbc_err *err);
kbc_status kbc_store_list_corkboard(kbc_store *s, kbc_arena *a, size_t limit,
                                    kbc_corkboard_row **out, size_t *n_out,
                                    kbc_err *err);

kbc_status kbc_store_pin_memory(kbc_store *s, const char *artifact_id,
                                int64_t pinned_at, kbc_err *err);
kbc_status kbc_store_unpin_memory(kbc_store *s, const char *artifact_id,
                                  kbc_err *err);
kbc_status kbc_store_list_pins(kbc_store *s, kbc_arena *a, size_t limit,
                               kbc_pin_row **out, size_t *n_out, kbc_err *err);

/* --------------------------------------------------------- first indexed -- */

/* Insert-or-ignore. The point is that a reindex must NOT refresh it: a
 * "created" sort needs an anchor that survives both a reindex and a file
 * copy, and mtime drifts on edit while btime does not survive a copy. */
kbc_status kbc_store_first_seen(kbc_store *s, const char *artifact_id,
                                int64_t first_indexed_unix, kbc_err *err);
kbc_status kbc_store_get_first_seen(kbc_store *s, const char *artifact_id,
                                    int64_t *out, kbc_err *err);


#ifdef __cplusplus
}
#endif

#endif /* KBC_STORE_H */
