/* ids.c — domain enumerations, hit-list maintenance and id minting.
 *
 * The id mint is the load-bearing part: it is what makes a reindex
 * idempotent, so it must depend only on fields that are already part of the
 * record and never on wall-clock time, addresses or iteration order. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "kbc/kbc.h"
#include "kbc/mem.h"
#include "kbc/types.h"

const char *kbc_kind_str(kbc_kind k) {
  switch (k) {
  case KBC_KIND_ARTIFACT:
    return "artifact";
  case KBC_KIND_NOTE:
    return "note";
  case KBC_KIND_MEMORY:
    return "memory";
  case KBC_KIND_SESSION:
    return "session";
  case KBC_KIND__COUNT:
    break;
  }
  return "unknown";
}

kbc_status kbc_kind_from_str(const char *s, kbc_kind *out, kbc_err *err) {
  if (s == NULL || out == NULL) {
    return kbc_err_set(err, KBC_ERR_INVALID,
                       "kbc_kind_from_str: null argument");
  }
  static const struct {
    const char *name;
    kbc_kind kind;
  } table[] = {
      {"artifact", KBC_KIND_ARTIFACT}, {"note", KBC_KIND_NOTE},
      {"memory", KBC_KIND_MEMORY},     {"session", KBC_KIND_SESSION},
  };
  for (size_t i = 0; i < sizeof(table) / sizeof(table[0]); i++) {
    if (strcmp(s, table[i].name) == 0) {
      *out = table[i].kind;
      return KBC_OK;
    }
  }
  /* KBC_KIND__COUNT is a count, not a name: it is deliberately not here. */
  return kbc_err_set(err, KBC_ERR_INVALID, "unknown kind %s", s);
}

/* ================================ hits ================================= */

void kbc_hits_init(kbc_hits *h) {
  h->items = NULL;
  h->len = 0;
  h->cap = 0;
}

void kbc_hits_free(kbc_hits *h) {
  if (h == NULL) {
    return;
  }
  free(h->items);
  h->items = NULL;
  h->len = 0;
  h->cap = 0;
}

kbc_status kbc_hits_push(kbc_hits *h, uint32_t doc, double score) {
  if (h == NULL) {
    return KBC_ERR_INVALID;
  }
  if (h->len == h->cap) {
    size_t cap = h->cap == 0 ? 16u : h->cap * 2u;
    if (cap > KBC_MAX_HITS) {
      return KBC_ERR_INVALID;
    }
    if (cap > SIZE_MAX / sizeof(kbc_hit)) {
      return KBC_ERR_NOMEM;
    }
    kbc_hit *items = realloc(h->items, cap * sizeof(*items));
    if (items == NULL) {
      return KBC_ERR_NOMEM;
    }
    h->items = items;
    h->cap = cap;
  }
  h->items[h->len].doc = doc;
  h->items[h->len].score = score;
  h->items[h->len].rank = 0;
  h->items[h->len].vector_score = 0.0;
  h->len++;
  return KBC_OK;
}

static bool hit_le(const kbc_hit *a, const kbc_hit *b) {
  /* score desc, then doc asc. Strict weak ordering on a total order, so the
   * merge below is well defined. */
  if (a->score > b->score) {
    return true;
  }
  if (a->score < b->score) {
    return false;
  }
  return a->doc < b->doc;
}

/* Bottom-up merge sort. A stable sort keeps equal (score, doc) pairs in the
 * order the lane produced them, which qsort would not guarantee. */
static void hits_merge_sort(kbc_hit *items, kbc_hit *scratch, size_t n) {
  for (size_t width = 1; width < n; width *= 2) {
    for (size_t lo = 0; lo < n; lo += 2u * width) {
      size_t mid = lo + width;
      size_t hi = lo + 2u * width;
      if (mid > n) {
        mid = n;
      }
      if (hi > n) {
        hi = n;
      }
      size_t i = lo;
      size_t j = mid;
      size_t k = lo;
      while (i < mid && j < hi) {
        scratch[k++] = hit_le(&items[j], &items[i]) ? items[j++] : items[i++];
      }
      while (i < mid) {
        scratch[k++] = items[i++];
      }
      while (j < hi) {
        scratch[k++] = items[j++];
      }
      memcpy(items + lo, scratch + lo, (hi - lo) * sizeof(*items));
    }
    if (width > n / 2u) {
      break;
    }
  }
}

void kbc_hits_sort_desc(kbc_hits *h) {
  if (h == NULL || h->len < 2) {
    return;
  }
  kbc_hit *scratch = malloc(h->len * sizeof(*scratch));
  if (scratch == NULL) {
    /* Cannot sort without scratch memory; the caller's order is preserved
     * rather than half-permuted. */
    return;
  }
  hits_merge_sort(h->items, scratch, h->len);
  free(scratch);

  for (size_t i = 0; i < h->len; i++) {
    h->items[i].rank = (uint32_t)i;
  }
}

void kbc_hits_truncate(kbc_hits *h, size_t n) {
  if (h == NULL) {
    return;
  }
  if (n < h->len) {
    h->len = n;
  }
}

/* ================================= ids ================================= */

static uint64_t fnv1a64_update(uint64_t h, const void *data, size_t n) {
  const unsigned char *p = data;
  for (size_t i = 0; i < n; i++) {
    h ^= (uint64_t)p[i];
    h *= 0x100000001b3ull;
  }
  return h;
}

void kbc_id_for_artifact(char out[KBC_MAX_ID_LEN + 1], const char *corpus,
                         const char *path, int64_t mtime_ns,
                         int64_t size_bytes) {
  if (out == NULL) {
    return;
  }
  out[0] = '\0';
  if (corpus == NULL) {
    corpus = "";
  }
  if (path == NULL) {
    path = "";
  }

  /* 0x1f is the ASCII unit separator: it cannot occur in a corpus name or a
   * corpus-relative path, so ("kb", "a/b") and ("k", "b/a") cannot collide
   * into the same hash. The integers go in little-endian byte order, which is
   * fixed by the standard rather than by the machine's byte order. */
  const uint64_t basis = 0xcbf29ce484222325ull;
  uint64_t h = basis;
  h = fnv1a64_update(h, corpus, strlen(corpus));
  h = fnv1a64_update(h, "\x1f", 1);
  h = fnv1a64_update(h, path, strlen(path));
  h = fnv1a64_update(h, "\x1f", 1);

  uint64_t mt = (uint64_t)mtime_ns;
  unsigned char mt_le[8];
  uint64_t sz = (uint64_t)size_bytes;
  unsigned char sz_le[8];
  for (size_t i = 0; i < 8; i++) {
    mt_le[i] = (unsigned char)((mt >> (8u * i)) & 0xffu);
    sz_le[i] = (unsigned char)((sz >> (8u * i)) & 0xffu);
  }
  h = fnv1a64_update(h, mt_le, sizeof(mt_le));
  h = fnv1a64_update(h, "\x1f", 1);
  h = fnv1a64_update(h, sz_le, sizeof(sz_le));

  /* First 48 bits, hex, lowercase: 12 chars plus the NUL. */
  uint64_t top = h >> 16;
  static const char digits[] = "0123456789abcdef";
  for (size_t i = 0; i < KBC_MAX_ID_LEN; i++) {
    unsigned shift = (unsigned)((KBC_MAX_ID_LEN - 1u - i) * 4u);
    out[i] = digits[(top >> shift) & 0xfu];
  }
  out[KBC_MAX_ID_LEN] = '\0';
}

bool kbc_id_is_valid(const char *id) {
  if (id == NULL) {
    return false;
  }
  for (size_t i = 0; i < KBC_MAX_ID_LEN; i++) {
    char c = id[i];
    bool ok = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
    if (!ok) {
      return false;
    }
  }
  return id[KBC_MAX_ID_LEN] == '\0';
}

char *kbc_title_from_path(kbc_arena *a, const char *rel_path,
                          const char *fallback) {
  if (a == NULL) {
    return NULL;
  }
  const char *fallback_safe = fallback != NULL ? fallback : "";

  if (rel_path == NULL) {
    return kbc_arena_strdup(a, fallback_safe);
  }

  const char *base = rel_path;
  for (const char *p = rel_path; *p != '\0'; p++) {
    if (*p == '/') {
      base = p + 1;
    }
  }

  size_t len = strlen(base);
  /* Strip the final extension only: "notes/.config" must not become "" and
   * "archive.tar.gz" keeps "archive.tar". */
  const char *dot = NULL;
  for (size_t i = 0; i < len; i++) {
    if (base[i] == '.') {
      dot = base + i;
    }
  }
  if (dot != NULL && dot != base) {
    len = (size_t)(dot - base);
  }

  char *title = kbc_arena_alloc(a, len + 1u);
  if (title == NULL) {
    return NULL;
  }
  for (size_t i = 0; i < len; i++) {
    char c = base[i];
    title[i] = (c == '-' || c == '_') ? ' ' : c;
  }
  title[len] = '\0';

  if (title[0] == '\0') {
    return kbc_arena_strdup(a, fallback_safe);
  }
  return title;
}
