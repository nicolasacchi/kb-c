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
#include "kbc/links.h"
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

/* Counters for GET /api/stats, /api/metrics and `kbc status`. */
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
  /* The query-embedding cache. A cache nobody can read is a cache nobody can
   * tell is working, and the hit ratio is the only evidence the thing does
   * anything at all.
   *
   * `query_cache_drops` counts query embeds that MISSED and then FAILED, so
   * nothing was written back. It is deliberately not an invalidation count:
   * there is no whole-cache invalidation, because there does not need to be
   * one. The cache key carries the model, a restart that fails to handshake
   * leaves the model unknown and the next embed refuses rather than keying
   * under a stale name, and a restart that succeeds with a different model is
   * a legitimate miss on a different key.
   *
   * The three together account for every search: hits + misses + drops is the
   * number of queries run, and that identity is what the concurrency test
   * asserts. A counter set that does not sum to the work done is a counter
   * set nobody can reason from. */
  int64_t query_cache_hits;
  int64_t query_cache_misses;
  int64_t query_cache_drops;
  int64_t query_cache_entries;
} kbc_app_stats;

kbc_status kbc_app_stats_get(kbc_app *app, kbc_app_stats *out, kbc_err *err);

/* ------------------------------------------------- the enrichment registry */

/* PORT_PLAN stage 1 unit 5, and SMALLER than the plan's wording. The original
 * registers eight hooks (kb-core/src/enrich.rs:118-133) and six of them serve
 * features kb-c does not have — a sessions corpus, two memory ledgers,
 * memory-link seeding, code-reference extraction, artifact snapshots and
 * reading-list anchors. Porting them would be a table of empty registrations,
 * so they are named-and-absent with a reason each, not present-and-dead.
 *
 * What IS kb-c's is the DISCIPLINE, and it is the part with teeth
 * (enrich.rs:14-24): a hook is BEST-EFFORT. Its failure is logged and stepped
 * over, it must NEVER fail the index, and it must never leave the store half
 * written. A pre-filter runs first and cheaply, so a hook that cannot apply
 * costs nothing. Run order IS registration order — the plan says so, and a
 * batch whose hooks ran in an arbitrary order would be a batch whose result
 * depended on a sort.
 *
 * A BATCH, not a document, and this is the one place kb-c cannot follow the
 * original. The original enriches per document; kb-c writes the link graph
 * after the whole walk, because a document linking to a sibling further down
 * must still produce an edge, and an edge written per document would make the
 * graph depend on walk order. The convergence property in store.h is what
 * makes that safe, and this unit is shaped to match it. */
typedef struct kbc_enrich_ctx kbc_enrich_ctx;

/* One document to enrich: its corpus, its path, and the link targets the
 * parser found, still unresolved. The original dedups on a normalised target
 * (enrich.rs:1088-1094) and that has no counterpart here, because the edges
 * table's own primary key collapses duplicates. */
typedef struct {
  const char *corpus;   /* BORROWED; every source in a batch names one corpus,
                         * because a corpus is the ladder's whole world */
  const char *src;      /* BORROWED, corpus-relative, '/' separated */
  char *const *targets; /* BORROWED, owned by the caller */
  size_t n_targets;
} kbc_enrich_source;

/* Cheap pre-filter. False means the hook is not run at all. */
typedef bool (*kbc_enrich_prefilter_fn)(const kbc_enrich_ctx *ctx, void *user);
typedef kbc_status (*kbc_enrich_fn)(const kbc_enrich_ctx *ctx, void *user,
                                    kbc_err *err);

/* The registry's size, not a policy: the original's is a fixed eight. */
#define KBC_ENRICH_MAX_HOOKS 16u

/* What a hook may write through. The store is the ONLY writer — the original
 * says hooks write through the storage handle and never SQL directly
 * (enrich.rs:20-24) — and every other field is BORROWED and dies with the
 * batch, so a hook that keeps one past kbc_enrich_run holds a dangling
 * pointer. `candidates` is what the resolution ladder may name, and an edge
 * written to anything outside it is an edge the graph cannot later verify. */
struct kbc_enrich_ctx {
  kbc_store *store;
  const char *corpus;
  const kbc_enrich_source *sources; /* walk order */
  size_t n_sources;
  const kbc_resolve_doc *candidates;
  size_t n_candidates;
};

/* Runs `n` hooks over `ctx` in the order given. The arrays are parallel
 * rather than a struct table so a caller never has to redeclare the hook
 * layout to use it — mirroring a struct asserts its layout, not its
 * behaviour. They are packed into the daemon's own table and run by the same
 * loop, so this entry point and the daemon's path are ONE implementation.
 *
 * Returns KBC_OK even when a hook fails: that failure is logged under the
 * hook's name and the run continues. `err` is filled only for a failure of
 * the RUN itself, never on a hook's behalf — a caller seeing a failure here
 * would reasonably conclude the index failed, and it did not. */
kbc_status kbc_enrich_run(size_t n, const char *const *names,
                          kbc_enrich_prefilter_fn pre[], kbc_enrich_fn fns[],
                          void *const *users, const kbc_enrich_ctx *ctx,
                          kbc_err *err);

/* Test seam: replace the index with one the caller built (used by the
 * integration tests to assert ranking without an on-disk corpus). */
void kbc_app_set_index(kbc_app *app, kbc_index *ix);

#ifdef __cplusplus
}
#endif

#endif /* KBC_APP_H */
