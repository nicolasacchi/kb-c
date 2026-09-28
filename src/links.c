/* links.c — the four-tier wikilink resolution ladder (links.rs ResolveIndex).
 *
 * A leaf: no I/O, no clock, no global mutable state. The caller builds one
 * index over a candidate set and resolves many raw link targets against it,
 * which is the whole reason the type exists (links.rs:233-239) — resolving
 * per target was an O(docs) rescan that also re-lowercased every candidate
 * per target.
 *
 * The tables are open-addressed rather than a general hash map because the
 * four tiers need exactly three operations: presence, first-wins insert, and
 * append-to-a-list. Every key and every id is COPIED into the index's own
 * arena, so the index outlives the caller's `docs` array.
 *
 * A match list is the key's FIRST id inline plus, only for the rare key that
 * several documents answer to, a lazily allocated overflow list. Almost every
 * key holds exactly one id, and a kbc_strlist per key costs a 64-byte
 * allocation for that 8-byte fact — 96 MB of the index's footprint at 200k
 * candidates, for a case that is the exception, not the rule.
 */

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "kbc/kbc.h"
#include "kbc/links.h"
#include "kbc/mem.h"

/* ------------------------------------------------------------- tables --- */

/* One table shape serves all four tiers: key -> ids in document order. Tiers
 * 3 and 4 append; tier 2 is first-wins and therefore never holds more than
 * one id; tier 1 only needs key PRESENCE, so it stores no id at all and the
 * id the ladder hands back is the matched target itself (links.rs:297-299
 * returns the target, not a stored copy). Keys are (ptr, len) because
 * strip_ext's ext-elided form stops at the dot and is not NUL-terminated
 * there — an empty key is a real key (a document with no extension, a dotfile
 * called `.md`), so a NULL key, not an empty one, marks a free slot. */
typedef struct {
  char *key;      /* KBC_OWN, in the index's arena */
  size_t key_len;
  char *id0;      /* KBC_OWN, in the index's arena; NULL = no id yet */
  kbc_strlist *rest; /* KBC_OWN, NULL until a second document answers */
} tab_slot;

typedef struct {
  tab_slot *slots; /* KBC_OWN */
  size_t cap;
  size_t len;
} tab;

struct kbc_resolve_index {
  kbc_arena *keys; /* KBC_OWN — every key and every id copy */
  tab ids;         /* tier 1: presence only */
  tab paths;       /* tier 2: exact path AND ext-elided path, first-wins */
  tab titles;      /* tier 3: lowercased title */
  tab base;        /* tier 4: lowercased basename AND stem */
};

/* FNV-1a alone mixes the high bits far better than the low ones, and a table
 * indexes with a wide slice of them, so the murmur3 finalizer (three ops) is
 * what keeps the probe chains short. */
static size_t tab_hash(const char *key, size_t len) {
  uint32_t h = kbc_fnv1a32(key, len);
  h ^= h >> 16;
  h *= 0x7feb352du;
  h ^= h >> 15;
  return (size_t)h;
}

/* Index of the slot holding `key`, or of the free slot it belongs in. The
 * hash is 32-bit and the multiply keeps the high half, so the capacity may
 * be ANY number — a power-of-two table would round a 400k-key tier up to
 * 2^20 and hold 2.6 keys' worth of memory for every key it stores. */
static size_t tab_probe(const tab_slot *slots, size_t cap, const char *key,
                        size_t len) {
  size_t i = (size_t)(((uint64_t)tab_hash(key, len) * (uint64_t)cap) >> 32);
  while (slots[i].key != NULL &&
         (slots[i].key_len != len || memcmp(slots[i].key, key, len) != 0)) {
    if (++i == cap) {
      i = 0;
    }
  }
  return i;
}

/* Sizes the table for `want` distinct keys at a 7/8 load factor. Every caller
 * reserves its exact worst case before inserting, so no insert below ever has
 * to grow — and therefore never has to rehash a partially built tier. The
 * spare slots are what make the probe loop above terminate. */
static kbc_status tab_reserve(tab *t, size_t want) {
  if (want > SIZE_MAX / 4u) {
    return KBC_ERR_NOMEM;
  }
  size_t need = want + want / 7u + 8u;
  if (need <= t->cap) {
    return KBC_OK;
  }
  tab_slot *slots = calloc(need, sizeof(*slots));
  if (slots == NULL) {
    return KBC_ERR_NOMEM;
  }
  for (size_t i = 0; i < t->cap; i++) {
    if (t->slots[i].key == NULL) {
      continue;
    }
    slots[tab_probe(slots, need, t->slots[i].key, t->slots[i].key_len)] =
        t->slots[i];
  }
  free(t->slots);
  t->slots = slots;
  t->cap = need;
  return KBC_OK;
}

static const tab_slot *tab_find(const tab *t, const char *key, size_t len) {
  if (t->cap == 0) {
    return NULL;
  }
  size_t i = tab_probe(t->slots, t->cap, key, len);
  return t->slots[i].key != NULL ? &t->slots[i] : NULL;
}

/* Takes ownership of `key` (an arena copy) so the index outlives `docs`. An
 * `id` of NULL stores the key alone, which is tier 1's whole job. `first_wins`
 * mirrors Rust's or_insert (links.rs:268-269): a key an EARLIER document
 * already claimed is never overwritten, which is exactly the linear
 * `.find()`'s first-hit rule, and is what makes `ops/deploy.md` beat
 * `ops/deploy` when the .md document comes first in the caller's array. */
static kbc_status tab_put(tab *t, char *key, size_t len, char *id,
                          bool first_wins) {
  size_t i = tab_probe(t->slots, t->cap, key, len);
  if (t->slots[i].key != NULL) {
    /* Tier 1 is a SET: Rust's HashSet::insert is idempotent, so a corpus that
     * lists one id twice must build, not fail. A first-wins insert never
     * overwrites; only tiers 3 and 4 append. */
    if (id == NULL || first_wins) {
      return KBC_OK;
    }
    tab_slot *s = &t->slots[i];
    if (s->rest == NULL) {
      s->rest = calloc(1, sizeof(*s->rest));
      if (s->rest == NULL) {
        return KBC_ERR_NOMEM;
      }
      kbc_strlist_init(s->rest);
    }
    /* Appends never touch slot 0, so the document order the caller passed is
     * the order the ids come back in. */
    return kbc_strlist_push(s->rest, id);
  }
  t->slots[i].key = key;
  t->slots[i].key_len = len;
  t->slots[i].id0 = id;
  t->slots[i].rest = NULL;
  t->len++;
  return KBC_OK;
}

static void tab_free(tab *t) {
  for (size_t i = 0; i < t->cap; i++) {
    kbc_strlist_free(t->slots[i].rest);
    free(t->slots[i].rest);
  }
  free(t->slots);
  t->slots = NULL;
  t->cap = 0;
  t->len = 0;
}

/* --------------------------------------------------------- key strings --- */

static char *key_copy(kbc_arena *a, const char *key, size_t len) {
  return kbc_arena_strndup(a, key, len);
}

/* Rust's to_lowercase is Unicode-aware; these keys are compared byte for byte
 * (see tab_probe), so this folds A-Z and leaves every other byte alone, UTF-8
 * continuation bytes included. Folding only part of a multi-byte sequence
 * would corrupt the key; the divergence from the original is that a non-ASCII
 * uppercase letter does not fold, which cannot make two distinct corpus keys
 * collide. */
static char *lower_dup(kbc_arena *a, const char *s, size_t n, size_t *out_len) {
  char *d = kbc_arena_strndup(a, s, n);
  if (d == NULL) {
    return NULL;
  }
  for (size_t i = 0; i < n; i++) {
    if (d[i] >= 'A' && d[i] <= 'Z') {
      d[i] = (char)(d[i] + ('a' - 'A'));
    }
  }
  *out_len = n;
  return d;
}

/* links.rs:189 — everything after the LAST '/', the whole string when there
 * is none. A trailing slash therefore yields "", exactly as rsplit does. */
static const char *basename(const char *rel, size_t *out_len) {
  const char *slash = strrchr(rel, '/');
  if (slash == NULL) {
    *out_len = strlen(rel);
    return rel;
  }
  const char *b = slash + 1;
  *out_len = strlen(b);
  return b;
}

/* links.rs:336-345 — the extension comes off the FINAL SEGMENT only. The old
 * rsplit_once('.') split on a dotted DIRECTORY's dot, which made `a.b/c`
 * claim the path-tier key `a`; splitting on the last slash first is the fix
 * links.rs:527 pins. A dotfile with no slash, `.hidden`, elides to "", and a
 * dotfile in a folder, `a.b/.md`, elides to `a.b/` — both faithful, and the
 * empty one is unreachable as a tier-2 key because a target normalising to ""
 * is answered before the ladder starts. */
static const char *strip_ext(const char *rel, size_t *out_len) {
  const char *slash = strrchr(rel, '/');
  const char *from = slash != NULL ? slash + 1 : rel;
  const char *end = rel + strlen(rel);
  for (const char *p = end; p > from; p--) {
    if (p[-1] == '.') {
      *out_len = (size_t)(p - 1 - rel);
      return rel;
    }
  }
  *out_len = (size_t)(end - rel);
  return rel;
}

/* links.rs:194-197 — the basename's extension removed, which is strip_ext on
 * a string that contains no separator. */
static const char *stem(const char *rel, size_t *out_len) {
  size_t base_len;
  const char *b = basename(rel, &base_len);
  return strip_ext(b, out_len);
}

/* links.rs:199-203 — 12 bytes, ASCII hex, and NOT an ASCII uppercase letter,
 * which leaves exactly [0-9a-f]. The length is a BYTE length, so a 6-character
 * non-ASCII id is not an id either. */
static bool is_id_shape(const char *t, size_t len) {
  if (len != KBC_MAX_ID_LEN) {
    return false;
  }
  for (size_t i = 0; i < len; i++) {
    char c = t[i];
    bool hex = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
    if (!hex) {
      return false;
    }
  }
  return true;
}

/* Rust's char::is_whitespace is Unicode White_Space; this is the ASCII
 * subset of it, which is the only part a corpus path or title reaches. */
static bool is_space(char c) {
  return c == ' ' || c == '\t' || c == '\n' || c == '\v' || c == '\f' ||
         c == '\r';
}

/* links.rs:182-187. The order is load-bearing: trim, cut at the FIRST '#',
 * trim AGAIN (`Title #frag ` collapses to `Title`), strip a RUN of leading
 * slashes, and only then fold backslashes. Folding first would make
 * `\ops\x` normalise to `ops/x`, which the original does not do. */
static kbc_status normalize(const char *target, kbc_arena *a, const char **out,
                            size_t *out_len, kbc_err *err) {
  const char *beg = target;
  const char *end = target + strlen(target);
  while (beg < end && is_space(*beg)) {
    beg++;
  }
  while (end > beg && is_space(end[-1])) {
    end--;
  }
  const char *hash = memchr(beg, '#', (size_t)(end - beg));
  if (hash != NULL) {
    end = hash;
  }
  while (end > beg && is_space(end[-1])) {
    end--;
  }
  while (beg < end && *beg == '/') {
    beg++;
  }
  size_t n = (size_t)(end - beg);
  char *buf = kbc_arena_strndup(a, beg, n);
  if (buf == NULL) {
    return kbc_err_set(err, KBC_ERR_NOMEM, "links: normalise a %zu-byte target",
                       n);
  }
  for (size_t i = 0; i < n; i++) {
    if (buf[i] == '\\') {
      buf[i] = '/';
    }
  }
  *out = buf;
  *out_len = n;
  return KBC_OK;
}

/* ================================ index ================================ */

void kbc_resolve_index_free(kbc_resolve_index *ix) {
  if (ix == NULL) {
    return;
  }
  tab_free(&ix->ids);
  tab_free(&ix->paths);
  tab_free(&ix->titles);
  tab_free(&ix->base);
  kbc_arena_free(ix->keys);
  free(ix);
}

/* One candidate into all four tables, in the order the original inserts them
 * (links.rs:266-280). The id is copied ONCE and shared by every tier: it is
 * the same string in all four, and tier 1's key IS the id. */
static kbc_status index_doc(kbc_resolve_index *ix, const kbc_resolve_doc *d) {
  kbc_arena *a = ix->keys;
  /* A caller that forgot a field gets the empty string, not a crash: the
   * degenerate document is a real state, and every tier treats "" as a key
   * no non-empty target can match. */
  const char *id = d->id != NULL ? d->id : "";
  const char *rel = d->rel_path != NULL ? d->rel_path : "";
  const char *title = d->title != NULL ? d->title : "";
  kbc_status st;
  size_t len;
  char *key;

  char *id_copy = key_copy(a, id, strlen(id));
  if (id_copy == NULL) {
    return KBC_ERR_NOMEM;
  }
  st = tab_put(&ix->ids, id_copy, strlen(id), NULL, false);
  if (st != KBC_OK) {
    return st;
  }

  key = key_copy(a, rel, strlen(rel));
  if (key == NULL) {
    return KBC_ERR_NOMEM;
  }
  st = tab_put(&ix->paths, key, strlen(rel), id_copy, true);
  if (st != KBC_OK) {
    return st;
  }
  /* strip_ext writes `len`, so its result and the length must be read in
   * separate statements: passing both to one call would leave the length at
   * the mercy of the unspecified order of argument evaluation. */
  const char *elided = strip_ext(rel, &len);
  key = key_copy(a, elided, len);
  if (key == NULL) {
    return KBC_ERR_NOMEM;
  }
  st = tab_put(&ix->paths, key, len, id_copy, true);
  if (st != KBC_OK) {
    return st;
  }

  key = lower_dup(a, title, strlen(title), &len);
  if (key == NULL) {
    return KBC_ERR_NOMEM;
  }
  st = tab_put(&ix->titles, key, len, id_copy, false);
  if (st != KBC_OK) {
    return st;
  }

  size_t base_len, stem_len;
  const char *base = basename(rel, &base_len);
  const char *stemmed = stem(rel, &stem_len);
  key = lower_dup(a, base, base_len, &len);
  if (key == NULL) {
    return KBC_ERR_NOMEM;
  }
  st = tab_put(&ix->base, key, len, id_copy, false);
  if (st != KBC_OK) {
    return st;
  }
  /* A document whose stem equals its basename has no extension and is
   * inserted ONCE. Inserting it twice would make every extension-less
   * target Ambiguous with ITSELF — the one bug here no other tier catches. */
  if (stem_len == base_len && memcmp(stemmed, base, base_len) == 0) {
    return KBC_OK;
  }
  key = lower_dup(a, stemmed, stem_len, &len);
  if (key == NULL) {
    return KBC_ERR_NOMEM;
  }
  return tab_put(&ix->base, key, len, id_copy, false);
}

kbc_resolve_index *kbc_resolve_index_new(const kbc_resolve_doc *docs, size_t n,
                                         kbc_err *err) {
  if (docs == NULL && n > 0) {
    kbc_err_set(err, KBC_ERR_INVALID, "links: docs is NULL with n=%zu", n);
    return NULL;
  }
  if (n > SIZE_MAX / 16u) {
    kbc_err_set(err, KBC_ERR_NOMEM,
                "links: %zu candidates is past the tables' addressable range",
                n);
    return NULL;
  }
  kbc_resolve_index *ix = calloc(1, sizeof(*ix));
  if (ix == NULL) {
    kbc_err_set(err, KBC_ERR_NOMEM, "links: resolve index");
    return NULL;
  }
  /* One block sized for a real corpus; the arena grows for anything larger.
   * Every key and id dies with it, so a whole index is one free. */
  ix->keys = kbc_arena_new(n == 0 ? 4096u : 64u * 1024u);
  if (ix->keys == NULL) {
    free(ix);
    kbc_err_set(err, KBC_ERR_NOMEM, "links: key arena for %zu candidates", n);
    return NULL;
  }

  kbc_status st = KBC_OK;
  if (n > 0) {
    /* Worst case distinct keys per table: one id and one title each, two for
     * paths (exact + ext-elided) and two for basenames (basename + stem). */
    if ((st = tab_reserve(&ix->ids, n)) == KBC_OK) {
      st = tab_reserve(&ix->paths, n * 2u);
    }
    if (st == KBC_OK) {
      st = tab_reserve(&ix->titles, n);
    }
    if (st == KBC_OK) {
      st = tab_reserve(&ix->base, n * 2u);
    }
    for (size_t i = 0; st == KBC_OK && i < n; i++) {
      st = index_doc(ix, &docs[i]);
    }
  }
  if (st != KBC_OK) {
    kbc_resolve_index_free(ix);
    kbc_err_set(err, st, "links: resolve index over %zu candidates", n);
    return NULL;
  }
  return ix;
}

/* =============================== resolve =============================== */

/* How many documents answer to this key. A slot with no id is not a match:
 * the original's `match len { 1 => .., n if n > 1 => .., _ => {} }`
 * (links.rs:311-319) falls through to the next tier on an empty list, and an
 * empty list must never be reported as an ambiguous zero. */
static size_t tab_ids(const tab_slot *s) {
  if (s->id0 == NULL) {
    return 0;
  }
  return 1u + (s->rest != NULL ? s->rest->len : 0u);
}

/* The kind is DERIVED from the count, never passed in, so no caller can label
 * a two-element list ONE. The ids are copied out of the index: the caller
 * frees them with kbc_strlist_free (kbc_resolution::ids is KBC_OWN). */
static kbc_status resolution_set(kbc_resolution *out, const tab_slot *s,
                                 kbc_err *err) {
  size_t n = tab_ids(s);
  out->kind = n == 1 ? KBC_RESOLVE_ONE : KBC_RESOLVE_AMBIGUOUS;
  kbc_status st = kbc_strlist_push(&out->ids, s->id0);
  if (st == KBC_OK && s->rest != NULL) {
    for (size_t i = 0; i < s->rest->len; i++) {
      st = kbc_strlist_push(&out->ids, s->rest->items[i]);
      if (st != KBC_OK) {
        break;
      }
    }
  }
  if (st != KBC_OK) {
    kbc_strlist_free(&out->ids);
    return kbc_err_set(err, st, "links: copy %zu match ids", n);
  }
  return KBC_OK;
}

/* The ladder (links.rs:290-330). Ownership split, which links.h states
 * twice and differently: `a` owns the SCRATCH — the normalised target and
 * its lowercased form, both dead by the next call — and `out->ids` holds
 * KBC_OWN heap copies the caller frees with kbc_strlist_free. That is the
 * safe reading of the two comments: kbc_strlist_free calls free() on every
 * item, so ids parked in the caller's arena would be freed twice, and a
 * caller that freed only the arena would leak them instead. */
kbc_status kbc_resolve_index_resolve(const kbc_resolve_index *ix, kbc_arena *a,
                                     const char *target, kbc_resolution *out,
                                     kbc_err *err) {
  if (ix == NULL) {
    return kbc_err_set(err, KBC_ERR_INVALID, "links resolve: index is NULL");
  }
  if (out == NULL) {
    return kbc_err_set(err, KBC_ERR_INVALID, "links resolve: out is NULL");
  }
  if (a == NULL) {
    return kbc_err_set(err, KBC_ERR_INVALID, "links resolve: arena is NULL");
  }
  if (target == NULL) {
    return kbc_err_set(err, KBC_ERR_INVALID, "links resolve: target is NULL");
  }
  out->kind = KBC_RESOLVE_NONE;
  kbc_strlist_init(&out->ids);

  /* Initialised, and not only to silence -Wmaybe-uninitialized: normalize
   * returns WITHOUT writing them on its allocation-failure path, and the
   * check below is what stops that path being read. The initialisers make the
   * safety local rather than dependent on the compiler following an
   * out-parameter through an early return. */
  const char *norm = NULL;
  size_t norm_len = 0;
  kbc_status st = normalize(target, a, &norm, &norm_len, err);
  if (st != KBC_OK) {
    return st;
  }
  /* A target that normalises away names nothing. Dangling links are a normal
   * state of a corpus, so this is KBC_OK, not a failure (links.rs:292-294). */
  if (norm_len == 0) {
    return KBC_OK;
  }

  /* 1. id — only a 12-hex lowercase target is even a candidate. The id the
   * ladder hands back IS the matched target (links.rs:298), so it is copied
   * out of the scratch rather than out of a table. */
  if (is_id_shape(norm, norm_len) &&
      tab_find(&ix->ids, norm, norm_len) != NULL) {
    out->kind = KBC_RESOLVE_ONE;
    st = kbc_strlist_push(&out->ids, norm);
    if (st != KBC_OK) {
      kbc_strlist_free(&out->ids);
      return kbc_err_set(err, st, "links: copy the id \"%.64s\"", norm);
    }
    return KBC_OK;
  }

  /* 2. source-relative path, exact or ext-elided. Both forms keep the
   * folder, so a hit here is unique — the invariant that lets this tier
   * return ONE at all. */
  const tab_slot *hit = tab_find(&ix->paths, norm, norm_len);
  if (hit != NULL) {
    if (tab_ids(hit) != 1) {
      return kbc_err_set(err, KBC_ERR_INTERNAL,
                         "links: first-wins path key \"%.64s\" holds %zu ids",
                         norm, tab_ids(hit));
    }
    return resolution_set(out, hit, err);
  }

  /* 3 and 4 share one lookup key: the target lowercased. */
  size_t lower_len;
  char *lower = lower_dup(a, norm, norm_len, &lower_len);
  if (lower == NULL) {
    return kbc_err_set(err, KBC_ERR_NOMEM, "links: lowercase \"%.64s\"", norm);
  }
  hit = tab_find(&ix->titles, lower, lower_len);
  if (hit == NULL || tab_ids(hit) == 0) {
    hit = tab_find(&ix->base, lower, lower_len);
  }
  if (hit == NULL || tab_ids(hit) == 0) {
    return KBC_OK;
  }
  return resolution_set(out, hit, err);
}
