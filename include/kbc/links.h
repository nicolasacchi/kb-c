/* links.h — the four-tier wikilink resolution ladder.
 *
 * This is the piece stage 2 of PORT_PLAN.md calls out as not ported: kb-c
 * normalises a link target (parse.c's link_resolve, which matches
 * links.rs:182 normalize_target) but resolves it by EXACT PATH against the
 * store, so `[[Some Title]]` and a bare `[[deploy]]` find nothing even when
 * exactly one document answers to that name. The graph is therefore smaller
 * than the corpus states.
 *
 * Shape follows the original: a leaf with no I/O, no clock and no map
 * iteration. A caller builds one index over a candidate set and resolves many
 * targets against it. Building it per target is the O(docs) scan the original
 * removed, and the whole reason ResolveIndex exists (links.rs:233-239).
 *
 * The ladder, in order (links.rs:205-223):
 *
 *   1. id      — the target is a 12-hex lowercase id of some document.
 *   2. path    — the target equals a document's source-relative path, exact or
 *                with the FINAL extension elided. Case-sensitive: the
 *                filesystem's rule, not a user's.
 *   3. title   — case-insensitive exact title.
 *   4. basename— case-insensitive bare filename, with or without extension.
 *
 * Tiers 1-2 are inherently unique, so they cannot be ambiguous. Tiers 3-4 can
 * match several documents, and then the answer is AMBIGUOUS, not "the first
 * one": a bare basename shared by `ops/deploy.md` and `infra/deploy.md` is
 * not an unambiguous edge, and picking one by index order would be
 * nondeterministic and would flip on the next reindex. That is the whole point
 * of the Ambiguous outcome and the reason the original has it.
 */
#ifndef KBC_LINKS_H
#define KBC_LINKS_H

#include "kbc/kbc.h"
#include "kbc/mem.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct kbc_resolve_index kbc_resolve_index;

/* One candidate: the three fields the ladder reads, and nothing else. The
 * original calls this DocLite (links.rs:41-46) for the same reason. */
typedef struct {
  const char *id;       /* BORROWED, 12 lowercase hex */
  const char *rel_path; /* BORROWED, source-relative, '/' separated */
  const char *title;    /* BORROWED, may be "" */
} kbc_resolve_doc;

typedef enum {
  KBC_RESOLVE_NONE = 0, /* nothing matched — a dangling link, never an error */
  KBC_RESOLVE_ONE,      /* exactly one document matched; ids.len == 1 */
  KBC_RESOLVE_AMBIGUOUS /* several share a title or basename; ids.len > 1 */
} kbc_resolve_kind;

typedef struct {
  kbc_resolve_kind kind;
  /* The matches, in the order the candidates were given. ONE has exactly one,
   * AMBIGUOUS has at least two and NOTHING else, NONE has none. Entries are
   * KBC_OWN and the caller frees them with kbc_strlist_free — NOT arena-owned.
   * The original sorts them only where it reports them (graph_report.rs:222-230),
   * so the order here is document order, not sorted. */
  kbc_strlist ids;
} kbc_resolution;

/* Builds the ladder's four lookup tables over `docs` (links.rs:258-287).
 *
 * Duplicates are resolved the way the original's linear `.find()` resolved
 * them, and the rule is not obvious: a key already claimed by an EARLIER
 * document is never overwritten, so when `ops/deploy.md` and `ops/deploy`
 * collide on the extension-elided form, the first document in `docs` order
 * wins. The caller must therefore pass documents in a stable order, or the
 * id a path resolves to changes between runs.
 *
 * `docs` is BORROWED for the call only — every key is copied, so the index
 * outlives the array. NULL on allocation failure, with err filled. */
kbc_resolve_index *kbc_resolve_index_new(const kbc_resolve_doc *docs, size_t n,
                                         kbc_err *err);
void kbc_resolve_index_free(kbc_resolve_index *ix);

/* The ladder (links.rs:290-330). `target` is the RAW link target, exactly as
 * it was written; this normalises it (trim, drop a `#fragment`, strip a
 * leading '/', fold '\\' to '/') before matching, so a caller must NOT
 * pre-normalise or a corpus-local target that parse.c already resolved
 * against its own directory will be normalised twice.
 *
 * `a` is SCRATCH — the normalised target and its lowercased form, both dead
 * by the next call. The ids in `out` are NOT arena-owned; see the field's
 * comment, and free them with kbc_strlist_free. Returns KBC_OK for all three
 * outcomes, including a dangling link: a
 * target that names nothing is a normal state of a corpus, not a failure. */
kbc_status kbc_resolve_index_resolve(const kbc_resolve_index *ix,
                                     kbc_arena *a, const char *target,
                                     kbc_resolution *out, kbc_err *err);

#ifdef __cplusplus
}
#endif

#endif /* KBC_LINKS_H */
