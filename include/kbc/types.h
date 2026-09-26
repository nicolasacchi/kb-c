/* types.h — the domain records every layer passes around, plus id minting.
 *
 * kbc_artifact and friends are ARENA records: every const char* inside points
 * into the kbc_arena the record was read with. There is no artifact_free().
 */
#ifndef KBC_TYPES_H
#define KBC_TYPES_H

#include "kbc/kbc.h"
#include "kbc/mem.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
  KBC_KIND_ARTIFACT = 0, /* a rendered HTML/Markdown deliverable */
  KBC_KIND_NOTE = 1,
  KBC_KIND_MEMORY = 2, /* curated fact, ranked by salience + decay */
  KBC_KIND_SESSION = 3, /* captured agent transcript digest */
  KBC_KIND__COUNT
} kbc_kind;

const char *kbc_kind_str(kbc_kind k);
/* KBC_ERR_INVALID (with err filled) for an unknown name. */
kbc_status kbc_kind_from_str(const char *s, kbc_kind *out, kbc_err *err);

/* One indexed document. ARENA: valid until the arena is reset/freed. */
typedef struct {
  const char *id;     /* 12 lowercase hex, minted by kbc_id_for_artifact */
  const char *corpus; /* corpus (kb) name */
  const char *path;   /* corpus-relative, '/'-separated, no leading slash */
  const char *title;
  kbc_kind kind;
  int64_t mtime_ns;
  int64_t size_bytes;
  uint32_t content_hash; /* kbc_fnv1a32 of the raw bytes; NOT a security hash */
  int32_t heading_count;
  const char *summary; /* first paragraph, NUL-terminated, may be "" */
  const char *source;  /* full raw text, may be NULL when not requested */
} kbc_artifact;

/* A block of extracted prose with the stable anchor id a comment attaches to.
 * ARENA. */
typedef struct {
  const char *id;   /* slug: heading slug, else "b<N>" */
  const char *text; /* NUL-terminated, newlines collapsed to spaces */
  size_t text_len;
  int32_t heading_level; /* 0 = not a heading, 1..6 = h1..h6 */
  uint32_t offset;       /* byte offset of the block in the source */
} kbc_block;

typedef struct {
  kbc_block *items; /* KBC_ARENA */
  size_t len, cap;
} kbc_blocks;

typedef struct kbc_parsed kbc_parsed; /* opaque; see parse.h */

/* Search hit. Score is in whatever lane produced it; rank is the 0-based
 * position after that lane's own sort, and is what the fusion consumes. */
typedef struct {
  uint32_t doc;
  double score;
  uint32_t rank;
  double vector_score; /* 0 when the vector lane did not run */
} kbc_hit;

typedef struct {
  kbc_hit *items; /* KBC_OWN — kbc_hits_free */
  size_t len, cap;
} kbc_hits;

void kbc_hits_init(kbc_hits *h);
void kbc_hits_free(kbc_hits *h);
kbc_status kbc_hits_push(kbc_hits *h, uint32_t doc, double score);
void kbc_hits_sort_desc(kbc_hits *h); /* by score desc, then doc asc — stable */
void kbc_hits_truncate(kbc_hits *h, size_t n);

/* FNV-1a 64 over the fields, hex-encoded to exactly 12 chars (48 bits). Stable
 * across runs and machines: the same (corpus, path, mtime_ns, size) always
 * yields the same id, which is what makes reindex idempotent. */
void kbc_id_for_artifact(char out[KBC_MAX_ID_LEN + 1], const char *corpus,
                         const char *path, int64_t mtime_ns, int64_t size_bytes);
bool kbc_id_is_valid(const char *id);

/* "notes/2026/rfc.md" -> "rfc" when the file has no title, else the caller's
 * fallback. ARENA copy. */
char *kbc_title_from_path(kbc_arena *a, const char *rel_path,
                          const char *fallback);

#ifdef __cplusplus
}
#endif

#endif /* KBC_TYPES_H */
