/* embed.h — the vector lane: a subprocess sidecar plus a flat vector store.
 *
 * The daemon never links an inference runtime. kb's Rust build already keeps
 * ONNX Runtime out of the daemon and inside `kb-embedder`, a child process
 * spoken to over stdio; kb-c keeps that seam and speaks the same shape of
 * protocol (newline-delimited JSON), so the same sidecar binary serves both.
 *
 * Wire format (one JSON object per line, both directions):
 *   -> {"op":"health"}
 *   <- {"ok":true,"dim":384,"model":"...","ready":true}
 *   -> {"op":"embed","texts":["a","b"]}
 *   <- {"ok":true,"dim":384,"vectors":[[0.1,...],[0.2,...]]}
 *   -> {"op":"shutdown"}
 * The reader tolerates `embeddings` as an alias for `vectors`, and infers the
 * dimension from the first vector when `dim` is absent. A malformed line, a
 * non-object, or a short vector array is a protocol error: the sidecar is
 * killed and marked unhealthy rather than half-trusted.
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

#ifdef __cplusplus
}
#endif

#endif /* KBC_EMBED_H */
