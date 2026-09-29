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

/* The store, BORROWED and valid until kbc_app_close. NULL on a NULL app.
 *
 * The store is the ONLY writer — see the enrichment registry below — and the
 * hook contract says a hook writes through it rather than reaching SQL. This
 * exists so a READER that needs the store reads it the same way a writer does,
 * rather than growing a second path into the tables. At the moment that reader
 * is the anchor re-evaluation the httpd does when a document is reindexed:
 * a comment's anchor goes stale or resolves as the document's headings move,
 * and answering that needs the comments and the edge graph, which live here
 * and nowhere else. */
kbc_store *kbc_app_store(kbc_app *app);

/* --------------------------------------------------------- capture & mv -- */

/* A capture turns uploaded bytes into a corpus file with provenance stamped
 * into its front matter, and it must produce a file BYTE-IDENTICAL to the
 * original's — that byte-diff is the acceptance gate, which is why the write
 * ORDER below is part of the contract rather than an implementation detail.
 *
 * The seven keys are stamped in this order: `kb-category`, `kb-tags`,
 * `kb-capture-original`, `kb-capture-url`, `kb-capture-at`, `kb-session`,
 * `kb-expires-at` (capture.rs:517, 518, 523, 529, 531, 541, 545).
 *
 * But that is only the order of FIRST INSERTION, and the distinction is the
 * whole subtlety. Each key goes through a line-oriented setter
 * (markdown.rs:327) and `edit_keys_region` (:384-413) decides placement: a key
 * the document ALREADY carries is edited in place, and one it does not is
 * appended after the last line. So a capture onto a document that already has
 * a category keeps the author's position for it and grows the other six after
 * it. A key that appears twice collapses to one. With no front matter at all a
 * fresh block is prepended; a leading `---` with no closing fence is not front
 * matter (markdown.rs:359-360), which is the same rule `kbc_fm_fence` in
 * parse.h already encodes — use that, not a second predicate.
 *
 * `title` steers the OUTPUT FILENAME and is never stamped into the document.
 * The response title is the indexer's own `title.or(h1).or(stem)` chain, which
 * `kbc_app` already runs, so there is deliberately no title on the result: a
 * second implementation would only be a second thing to drift.
 *
 * `url` is INERT. The daemon never dereferences it — that is the project's
 * SSRF ruling, not an omission — and there is a test that points a capture at a
 * real listener and asserts nothing connects. */
typedef struct {
  const char *corpus;  /* configured corpus name */
  const char *capture_dir; /* corpus-relative; NULL -> the default */
  const char *from;    /* provenance: cli | spa | share; NULL -> no from: */
  const char *title;   /* steers the FILENAME. Never stamped. */
  const char *url;     /* provenance only. NEVER dereferenced. */
  const char *original_filename; /* the multipart filename; provenance only */
  const char *category; /* NULL or empty -> "capture" */
  const char *session_id; /* NULL -> no kb-session stamp */
  const char *const *tags; /* free text; each slugified */
  size_t n_tags;
  bool has_expires_at;
  int64_t expires_at;
  const char *body; /* the uploaded bytes; len travels with it */
  size_t body_len;
  const char *ext; /* dot-less output extension; NULL -> "md" */
  /* > 0 pins the capture's clock second, which is what makes the written file
   * reproducible in a test (capture.rs:405 FrozenNow, same purpose). <= 0 is the
   * production path: the wall clock. */
  int64_t now_unix;
} kbc_capture_input;

/* Caller-allocated, so there is nothing to free and no ownership question. */
typedef struct {
  char id[KBC_MAX_ID_LEN + 1];    /* what the watcher will assign on ingest */
  char path[KBC_MAX_PATH_LEN + 1]; /* corpus-relative, '/' separated */
  size_t bytes;                   /* the bytes actually written */
} kbc_capture_result;

/* A URL "capture" is a text stub. The shared text becomes a paragraph and the
 * URL is stored beside it. Nothing is fetched, ever. */
typedef struct {
  const char *corpus;
  const char *capture_dir;
  const char *from;
  const char *title; /* empty -> "Untitled capture" */
  const char *url;   /* inert; NEVER dereferenced */
  const char *text;  /* the shared text, as a paragraph */
  const char *const *tags;
  size_t n_tags;
  int64_t now_unix;
} kbc_capture_url_input;

/* One part of a multipart body. `name` is the form field, so a caller matches
 on the field rather than on position. */
typedef struct {
  const char *name;     /* KBC_ARENA, the form field name */
  const char *filename; /* KBC_ARENA or NULL when the part had none */
  const char *data;     /* KBC_ARENA, may contain NULs */
  size_t len;
} kbc_multipart_part;

/* Decodes a multipart body. Takes the content type and the RAW bytes, so the
 * caller needs no other dependency on the app to use it. `out` is the caller's
 * array of `out_cap`.
 *
 * `*n_out` is a property of the BODY: it is ALWAYS the number of parts found,
 * on every path including failure, and the scan runs to the end of the body
 * rather than stopping at the caller's array. So it exceeds `out_cap` exactly
 * when the array was too small — that is the signal, not a hazard.
 *
 * The STATUS is a property of the CALL, and it says which of two things went
 * wrong. A MALFORMED body outranks an overflow on purpose: resizing does not
 * fix a truncated upload, so a caller told only "your array was too small"
 * would sit in a resize loop forever. The two failure modes are therefore
 * distinguishable without the caller having to get anything right twice.
 *
 * Nothing is ever written past `out_cap`, and `out[0..*n_out)` is readable on
 * every return — so a caller that ignores the status cannot read an unfilled
 * slot, and a caller that reads the count can size correctly and retry.
 *
 * Part strings are KBC_ARENA and die with `a`. `data` may contain NULs, which
 * is why `len` travels beside it. */
kbc_status kbc_multipart_parse(const char *content_type, const char *body,
                               size_t body_len, kbc_multipart_part *out,
                               size_t out_cap, size_t *n_out, kbc_arena *a,
                               kbc_err *err);

kbc_status kbc_app_capture(kbc_app *app, const char *corpus,
                           const kbc_capture_input *in, kbc_capture_result *out,
                           kbc_err *err);
kbc_status kbc_app_capture_url_stub(kbc_app *app, const char *corpus,
                                    const kbc_capture_url_input *in,
                                    kbc_capture_result *out, kbc_err *err);

/* A rename, which is a removal plus a creation unless something carries
 * across. What is REBUILT from the bytes: the artifact row, chunks, doc_metas
 * and outbound edges. What is CARRIED, because nothing can rebuild it: the
 * comments with their resolved flag, the corkboard entry and the pin WITH
 * THEIR ORIGINAL TIMESTAMPS, and the `doc_first_seen` anchor — that one before
 * the re-ingest, because `kbc_store_first_seen` is INSERT OR IGNORE and a
 * re-ingest would otherwise claim "now".
 *
 * The original also holds a delete-suppression guard for the watcher's
 * debounce window. kb-c does not need it and has no table for it: the watcher's
 * delete resolves against the STORE ROW for the path, and this holds
 * `reindex_mu` across the rename and the rekey, so the window never opens. A
 * crash mid-move leaves a stale row, which the reconcile pass already sweeps. */
kbc_status kbc_app_move_path(kbc_app *app, const char *corpus,
                             const char *old_rel, const char *new_rel,
                             kbc_err *err);

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
