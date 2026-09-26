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
 * CONVERGENCE — read this before trusting an in-degree. Edges are owned by the
 * SOURCE and are rewritten only when the SOURCE is re-ingested, never when a
 * target appears. A document ingested after a document that already links to it
 * therefore stays at in-degree 0 until its sources are re-ingested. This is
 * neither rare nor self-healing: `kbc daemon` does not reindex at startup, and a
 * full `kbc reindex` does NOT repair it either, because unchanged documents
 * contribute no edge writes. In practice the graph is complete only from a
 * from-scratch index, or after the linking sources are themselves touched.
 * kbc_config.graph_boost defaults to 0.0 — the graph is OFF — for exactly this
 * reason. Closing the gap properly needs a pending-links table drained at the
 * end of each ingest pass; that is PORT_PLAN stage 1, not a subtlety of this
 * function. */
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

#ifdef __cplusplus
}
#endif

#endif /* KBC_STORE_H */
