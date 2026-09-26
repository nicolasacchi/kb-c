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
