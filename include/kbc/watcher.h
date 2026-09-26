/* watcher.h — inotify-backed change detection with debounce.
 *
 * One thread per kbc_watcher. Watches every configured corpus root
 * recursively (watches are added per directory, so a directory that did not
 * exist at start is picked up on the next rescan). Events are coalesced per
 * (corpus, path) for cfg->watcher_debounce_ms, then published to the bus; the
 * callback runs ON THE WATCHER THREAD and must return promptly.
 */
#ifndef KBC_WATCHER_H
#define KBC_WATCHER_H

#include "kbc/config.h"
#include "kbc/kbc.h"

#ifdef __cplusplus
extern "C" {
#endif

/* (user, type, json_payload) — type is e.g. "index.updated", json is a
 * complete object like {"artifact_id":"...","path":"..."}. Both BORROWED and
 * only valid for the duration of the call. */
typedef void (*kbc_event_fn)(void *user, const char *type,
                             const char *json_payload);

typedef struct kbc_watcher kbc_watcher;

/* Starts the thread. The watcher copies what it needs out of `cfg`, so `cfg`
 * may be freed on return. */
kbc_watcher *kbc_watcher_start(const kbc_config *cfg, kbc_event_fn fn,
                               void *user, kbc_err *err);

/* Joins the thread, drains, frees. Safe on NULL. */
void kbc_watcher_stop(kbc_watcher *w);

bool kbc_watcher_is_running(const kbc_watcher *w);
/* Counters for /api/metrics. */
void kbc_watcher_counts(const kbc_watcher *w, int64_t *events_seen,
                        int64_t *reindex_triggered);

#ifdef __cplusplus
}
#endif

#endif /* KBC_WATCHER_H */
