/* meta.h — the filterable document metadata the query overlay needs.
 *
 * A document's facets come from its own markup: `<meta name="kb-tags"
 * content="a, b">` and friends. They are extracted at ingest, stored per
 * document, and evaluated at query time as `tags:`, `cap:`/`caps:` and `index:`
 * atoms — the keys the Rust overlay understands (crates/kb-core/src/query.rs
 * `apply_atom`).
 *
 * A key the document does not carry matches nothing, which is why a filter on
 * an absent facet returns zero rows rather than everything. The inverse — a
 * filter silently ignored — is the failure this design exists to prevent.
 */
#ifndef KBC_META_H
#define KBC_META_H

#include "kbc/kbc.h"
#include "kbc/mem.h"

#ifdef __cplusplus
extern "C" {
#endif

/* The reserved facet keys, as the markup spells them. `tags` and `caps` are
 * multi-valued (a comma-separated content attribute); `index` is a boolean
 * flag. Everything else in a kb-* meta is carried but not filterable today. */
#define KBC_META_TAGS "tags"
#define KBC_META_CAPS "caps"
#define KBC_META_INDEX "index"

/* One key/value pair. ARENA. */
typedef struct {
  const char *key;   /* lowercased, KBC_ARENA */
  const char *value; /* KBC_ARENA, whitespace-trimmed; for a multi-valued key,
                      * one entry per comma-separated element */
} kbc_meta;

typedef struct {
  kbc_meta *items; /* KBC_ARENA */
  size_t len, cap;
} kbc_metas;

#endif /* KBC_META_H */
