/* app.h — the composition root: config + store + index + embedder + watcher.
 *
 * Everything the daemon and the CLI need hangs off one kbc_app. The invariant
 * it exists to protect: the index and the store are replaced together, under
 * one write lock, so no reader can observe an index that references documents
 * the store has not committed (or the reverse).
 */
#ifndef KBC_APP_H
#define KBC_APP_H

#include "kbc/config.h"
#include "kbc/embed.h"
#include "kbc/index.h"
#include "kbc/kbc.h"
#include "kbc/search.h"
#include "kbc/store.h"
#include "kbc/watcher.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct kbc_app kbc_app;

kbc_app *kbc_app_open(const kbc_config *cfg, kbc_err *err);
void kbc_app_close(kbc_app *app);

const kbc_config *kbc_app_config(const kbc_app *app);

/* Full scan: walks every configured corpus, hashes, skips unchanged files,
 * ingests the rest, rebuilds the index and saves it. Emits watcher events.
 * KBC_ERR_IO if a corpus root is unreadable. */
kbc_status kbc_app_reindex(kbc_app *app, kbc_err *err);
/* One file: re-read, re-index, save. `corpus` + `rel_path`. */
kbc_status kbc_app_reindex_file(kbc_app *app, const char *corpus,
                                const char *rel_path, kbc_err *err);
kbc_status kbc_app_reindex_remove(kbc_app *app, const char *corpus,
                                  const char *rel_path, kbc_err *err);

/* ARENA result. Takes the read lock internally. */
kbc_status kbc_app_search(kbc_app *app, kbc_arena *a, const kbc_query *q,
                          kbc_search_result *out, kbc_err *err);
kbc_status kbc_app_get_artifact(kbc_app *app, kbc_arena *a, const char *id,
                                bool with_source, kbc_artifact *out,
                                kbc_err *err);
kbc_status kbc_app_list_artifacts(kbc_app *app, kbc_arena *a, const char *corpus,
                                  kbc_kind kind, size_t limit, size_t offset,
                                  kbc_artifact **out, size_t *n_out,
                                  kbc_err *err);

/* Starts the inotify watcher thread. Idempotent. */
kbc_status kbc_app_start_watcher(kbc_app *app, kbc_err *err);
void kbc_app_stop_watcher(kbc_app *app);

/* Subscribes to the event bus (SSE). Returns a subscription id; the callback
 * runs on the watcher/httpd thread that published the event and must not block
 * or call back into kbc_app. `user` must stay valid until unsubscribed. */
uint64_t kbc_app_subscribe(kbc_app *app, kbc_event_fn fn, void *user);
void kbc_app_unsubscribe(kbc_app *app, uint64_t id);
void kbc_app_publish(kbc_app *app, const char *type, const char *json_payload);

/* Counters for /api/metrics and `kbc status`. */
/* Counters for GET /api/stats and `kbc status`. */
typedef struct {
  int64_t artifacts_indexed;
  int64_t reindex_runs;
  int64_t searches_served;
  int64_t searches_degraded;
  int64_t last_reindex_ns;
  int64_t last_reindex_docs;
  int64_t last_reindex_us;
  int64_t index_terms;
  int64_t index_docs;
  int64_t db_bytes;
} kbc_app_stats;

kbc_status kbc_app_stats_get(kbc_app *app, kbc_app_stats *out, kbc_err *err);

/* Test seam: replace the index with one the caller built (used by the
 * integration tests to assert ranking without an on-disk corpus). */
void kbc_app_set_index(kbc_app *app, kbc_index *ix);

#ifdef __cplusplus
}
#endif

#endif /* KBC_APP_H */
