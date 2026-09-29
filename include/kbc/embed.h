/* embed.h — the vector lane: a subprocess sidecar plus a flat vector store.
 *
 * The daemon never links an inference runtime. kb's Rust build already keeps
 * ONNX Runtime out of the daemon and inside `kb-embedder`, a child process
 * spoken to over stdio; kb-c keeps that seam and speaks the SAME protocol, so
 * the same sidecar binary serves both.
 *
 * The protocol is newline-delimited JSON, and it is NOT request/response by
 * default — the sidecar announces itself first. Verified against the real
 * /home/nik/.local/bin/kb-embedder; the shapes below are what it actually
 * accepts, established by the sidecar's own parse errors.
 *
 *   <- {"kind":"ready","model":"bge-small-en-v1.5","dim":384}   UNSOLICITED,
 *                                                                   on connect
 *   -> {"kind":"embed","req_id":N,"texts":["a","b"]}             req_id is
 *   <- {"kind":"embed_ok","req_id":N,"vectors":[[..],[..]]}        MANDATORY
 *   <- {"kind":"error","req_id":N,"msg":"..."}                   on failure
 *   -> {"kind":"shutdown"}
 *
 * Three consequences, each of which is a bug that was shipped once:
 *  1. There is no "health" op. The sidecar replies "unknown variant `health`".
 *     Readiness is the unsolicited `ready` line; kbc_embedder must absorb it
 *     wherever it arrives, including in the middle of waiting for a reply.
 *  2. req_id is mandatory and must be echoed. A reply carrying someone else's
 *     req_id is a protocol error, not an answer.
 *  3. The sidecar reads stdin as UTF-8 and EXITS on a line that is not. So no
 *     byte >= 0x80 may ever reach the wire: text is escaped locally to \u00XX
 *     rather than passed through as raw UTF-8. A corpus with 8-bit bytes
 *     otherwise kills the sidecar mid-ingest and silently ends the vector lane.
 *
 * `kbc_embedder_start` takes the sidecar's argv (not a shell string): the
 * binary path, then --model and --cache, are all needed.
 */
#ifndef KBC_EMBED_H
#define KBC_EMBED_H
#include "kbc/json.h"
#include "kbc/kbc.h"
#include "kbc/mem.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------ sidecar ---- */

typedef struct kbc_embedder kbc_embedder;

/* Spawns `argv0` with the given extra argv (NULL-terminated, may be NULL).
 * stdin/stdout are pipes; stderr is inherited so the sidecar's own logs land
 * in the daemon's log. */
kbc_embedder *kbc_embedder_start(const char *const *argv, kbc_err *err);
/* Closes stdin, waits up to 2s, then SIGKILLs. Frees. Safe on NULL. */
void kbc_embedder_stop(kbc_embedder *e);

bool kbc_embedder_healthy(const kbc_embedder *e);
size_t kbc_embedder_dim(const kbc_embedder *e); /* 0 until the first response */
/* Restarts a crashed sidecar and re-runs the health handshake. */
kbc_status kbc_embedder_restart(kbc_embedder *e, kbc_err *err);
void kbc_embedder_counts(const kbc_embedder *e, int64_t *requests,
                         int64_t *failures);

/* Embeds n texts. `out` receives n*dim floats, row-major, ARENA-scoped.
 * `dim_hint` (0 = unknown) is a consistency check against the sidecar's own
 * dimension. KBC_ERR_TIMEOUT if the sidecar does not answer within 30s;
 * KBC_ERR_PARSE on a malformed reply; KBC_ERR_IO when the pipe dies. Each of
 * those three kills and reaps the child and marks it unhealthy — a
 * desynchronised stream cannot be reused, so a timeout is not a soft miss. */
kbc_status kbc_embedder_embed(kbc_embedder *e, kbc_arena *a,
                              const char *const *texts, size_t n,
                              size_t dim_hint, float **out, kbc_err *err);

/* ---------------------------------------------------------- vector store - */

typedef struct kbc_vecstore kbc_vecstore;

/* Creates (or opens) a store of fixed dimension. mmap'd whole; a store of N
 * docs at dim D costs N*D*4 bytes of address space and no heap. */
kbc_vecstore *kbc_vecstore_new(size_t dim, uint32_t capacity, kbc_err *err);
kbc_vecstore *kbc_vecstore_open(const char *path, kbc_err *err);
void kbc_vecstore_free(kbc_vecstore *v);

size_t kbc_vecstore_dim(const kbc_vecstore *v);
uint32_t kbc_vecstore_count(const kbc_vecstore *v);
/* Grows the file if needed. `vec` must hold exactly kbc_vecstore_dim(v)
 * floats; a NULL pointer is KBC_ERR_INVALID. A grow invalidates every
 * pointer previously returned by kbc_vecstore_get. */
kbc_status kbc_vecstore_set(kbc_vecstore *v, uint32_t doc_id, const float *vec,
                            kbc_err *err);
/* NULL when doc_id >= count. */
const float *kbc_vecstore_get(const kbc_vecstore *v, uint32_t doc_id);
kbc_status kbc_vecstore_save(kbc_vecstore *v, const char *path, kbc_err *err);
/* Copies the listed rows into `out` (n*dim floats, row-major, caller-sized). */
kbc_status kbc_vecstore_gather(const kbc_vecstore *v, const uint32_t *ids,
                               size_t n, kbc_arena *a, float *out,
                               kbc_err *err);

/* --------------------------------------------------------- query cache -- */

/* Caches one embedding per (model, query). The key is those two fields and
 * NOTHING else — no corpus, no mode, no limit, no top-k, no dim and no
 * embedder identity. The original is emphatic about it (embed_cache.rs:320-322)
 * and the reason is the failure it prevents: two distinct models can share a
 * dim, and serving one's vector under the other's key returns silently wrong
 * results with no dim error left to catch it.
 *
 * The key is the RAW query — no trim, no case fold, no whitespace collapse and
 * no hash — so two queries differing only past the embedder's 32 KiB input cap
 * share an entry and the first to populate wins. That is the original's
 * behaviour and it is reproduced rather than quietly "fixed".
 *
 * A caller-owned object, not a global: AGENTS.md rule 5 forbids mutable
 * file-scope state, and one instance shared by the daemon IS the original's
 * process-wide behaviour without the global. Sharing it across embedders
 * running different models is safe precisely because the model is in the key;
 * a caller wanting isolation passes a separate cache.
 *
 * Look-up is a LINEAR SCAN with tuple equality, not a hash, and the reason is
 * measured rather than assumed: at capacity 1024 a random hit costs about
 * 19 us, and contention is flat from 1 to 8 threads, against the 50-100 ms
 * sidecar round trip this cache exists to avoid — 0.02-0.04% of the work it
 * replaces. If embedding ever gets a hundred times faster the trade flips and
 * a hash wins; that is recorded here so the decision gets revisited rather
 * than inherited. */
typedef struct kbc_query_cache kbc_query_cache;

typedef struct {
  float *vec;  /* KBC_ARENA, `dim` floats; NULL on a failed embed */
  size_t dim;
  uint64_t embed_ms; /* wall time of the EMBEDDING STEP, and exactly 0 on a
                      * hit, because on a hit there is no embedding step to
                      * time. Timing the whole call would make this field say
                      * nothing about whether inference happened. */
  bool cache_hit;
} kbc_query_outcome;

/* `capacity` is the eviction ceiling and is never exceeded, not even
 * transiently. 0 is KBC_ERR_INVALID: a cache with no room is not a cache. */
kbc_query_cache *kbc_query_cache_new(size_t capacity, kbc_err *err);
void kbc_query_cache_free(kbc_query_cache *c);
size_t kbc_query_cache_len(const kbc_query_cache *c);
size_t kbc_query_cache_capacity(const kbc_query_cache *c);

/* `get` MUTATES the LRU order — a hit moves the entry to most-recently-used,
 * so eviction is least-recently-*used*. That is not obvious from the signature
 * and it is load-bearing.
 *
 * `*vec` is a KBC_ARENA copy in `a`, valid as long as `a`, and NOT a pointer
 * into the cache: a later `put` of the same key may replace the entry, and a
 * caller still holding the vector would otherwise be reading freed memory. */
bool kbc_query_cache_get(kbc_query_cache *c, const char *model,
                         const char *query, size_t query_len, kbc_arena *a,
                         const float **vec, size_t *dim);

/* Replacing an existing key does not grow the cache. */
kbc_status kbc_query_cache_put(kbc_query_cache *c, const char *model,
                               const char *query, size_t query_len,
                               const float *vec, size_t dim, kbc_err *err);

/* The model the sidecar ACTUALLY loaded, captured off the wire at handshake
 * time and never reconstructed from argv — argv is the operator's request,
 * the wire is what the model is. An over-long name is KBC_ERR_PARSE rather
 * than a truncation, because a clipped name collides with every other clipped
 * name, which is the failure the name is in the key to prevent. */
kbc_status kbc_embedder_model(kbc_embedder *e, char *out, size_t cap,
                              kbc_err *err);

/* The cached path: a hit answers without touching the sidecar, a miss embeds,
 * puts, and reports the `embed_ms` it took. `c` may be NULL, which embeds
 * unconditionally — so a caller with no cache is not a special case. */
kbc_status kbc_embed_query(kbc_embedder *e, kbc_query_cache *c, kbc_arena *a,
                           const char *query, kbc_query_outcome *out,
                           kbc_err *err);


#ifdef __cplusplus
}
#endif

#endif /* KBC_EMBED_H */
