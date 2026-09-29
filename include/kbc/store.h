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
