/* app.c — the composition root.
 *
 * app.h states the invariant this file exists to protect: the index and the
 * store are replaced together, under one write lock, so no reader can observe
 * an index that references documents the store has not committed. Every
 * public entry point here is safe to call from the httpd's worker threads.
 *
 * Locking shape (explicit, never re-entered):
 *
 *   pthread_rwlock_t lock guards EXACTLY {index, vec}.
 *   Public entry points take it; the `_locked` internals do not. The search
 *   resolve callback runs with the read lock already held, so it must never
 *   take it again — a rwlock is not recursive and that would be a hard
 *   self-deadlock.
 *
 * Everything else in kbc_app is either immutable after open (config, store,
 * embedder) or carries its own internal lock (store, embedder, watcher, and
 * the event-bus mutex).
 *
 * The reindex is deliberately two passes over the same manifest. Pass one
 * (walk_dir) decides *which* files are in the index and which of those changed,
 * storing the changed ones; pass two (build_index) reads each file once more
 * to tokenize and embed it. Holding every document's tokens until the end
 * would pin the whole corpus in one arena; a per-file arena in pass two
 * pins one file at a time, which is the memory that actually matters.
 */

/* Feature test macros before any system header. The build compiles with
 * -std=c17 (extensions off), which defines __STRICT_ANSI__ and would
 * otherwise hide opendir/fstatat/rename. */
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE 1
#endif

#include <stdio.h>

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "kbc/app.h"
#include "kbc/json.h"
#include "kbc/log.h"
#include "kbc/mem.h"
#include "kbc/parse.h"
#include "kbc/store.h"
#include "kbc/types.h"

/* The event bus is a fixed table, not a queue: the SSE writer has its own. */
#define KBC_APP_MAX_SUBS 64u
/* Recursion cap for the corpus walk; symlinked directories make this a
 * correctness concern, not merely a stack one. */
#define KBC_APP_MAX_DEPTH 64u
/* Summary length, in bytes, before truncation. */
#define KBC_APP_SUMMARY_MAX 512u
/* The eight owned char* in kbc_config, counted for the deep copy. */
#define KBC_APP_CFG_FIELDS 8u
#define KBC_APP_VEC_FILE "vectors.bin"

typedef struct {
  kbc_event_fn fn;
  void *user;
  uint64_t id;
} kbc_app_sub;

/* One file the index will contain, decided by the walk and consumed by the
 * build pass. Heap: it outlives the per-file arena that parsed it. */
typedef struct {
  size_t corpus_index;
  char *path;  /* KBC_OWN, corpus-relative, '/'-separated */
  char *title; /* KBC_OWN, never NULL once the row is in the manifest */
  char id[KBC_MAX_ID_LEN + 1];
  int64_t mtime_ns;
  int64_t size_bytes;
  bool changed; /* false => the stored row is still current */
} kbc_app_row;

typedef struct {
  kbc_app_row *items;
  size_t len, cap;
  size_t n_changed;
  size_t n_unchanged;
  size_t n_skipped;
} kbc_app_manifest;

struct kbc_app {
  kbc_config *cfg;   /* KBC_OWN, a private copy */
  kbc_store *store;  /* owns its own mutex */
  kbc_index *index;  /* guarded by lock */
  kbc_vecstore *vec; /* guarded by lock; NULL when the lane is off */
  kbc_embedder *embed;
  kbc_watcher *watch;
  char *vec_path; /* KBC_OWN */

  pthread_rwlock_t lock;
  pthread_mutex_t bus_lock;
  kbc_app_sub subs[KBC_APP_MAX_SUBS];
  uint64_t next_sub_id;

  _Atomic int64_t st_indexed;
  _Atomic int64_t st_runs;
  _Atomic int64_t st_searches;
  _Atomic int64_t st_degraded;
  _Atomic int64_t st_last_ns;
  _Atomic int64_t st_last_docs;
  _Atomic int64_t st_last_us;
  _Atomic int64_t st_terms;
  _Atomic int64_t st_docs;
  _Atomic int64_t st_bytes;
};

/* ctx for the search resolver. `ix` is BORROWED and is alive for as long as
 * the caller's read lock is held. */
typedef struct {
  kbc_app *app;
  const kbc_index *ix;
} kbc_app_resolve_ctx;

/* ------------------------------------------------------------------ util */

/* strdup is not in C17 and the build sets __STRICT_ANSI__; roll our own. */
static char *dup_n(const char *s, size_t n) {
  char *p = (char *)malloc(n + 1u);
  if (!p) {
    return NULL;
  }
  if (n > 0) {
    memcpy(p, s, n);
  }
  p[n] = '\0';
  return p;
}

static char *dup_cstr(const char *s) { return dup_n(s, strlen(s)); }

static bool path_has_ext(const char *rel, const char *ext) {
  size_t rl = strlen(rel);
  size_t el = strlen(ext);
  if (rl < el) {
    return false;
  }
  const char *tail = rel + (rl - el);
  for (size_t i = 0; i < el; i++) {
    char a = tail[i];
    if (a >= 'A' && a <= 'Z') {
      a = (char)(a - 'A' + 'a');
    }
    if (a != ext[i]) {
      return false;
    }
  }
  return true;
}

static bool is_indexable(const char *rel) {
  return path_has_ext(rel, ".html") || path_has_ext(rel, ".htm") ||
         path_has_ext(rel, ".md") || path_has_ext(rel, ".markdown");
}

/* The corpus ignore list is "glob-ish substrings" (see watcher.c), so a plain
 * substring test over the relative path and the leaf name is the contract. */
static bool corpus_ignores(const kbc_corpus_cfg *cc, const char *rel,
                           const char *leaf) {
  for (size_t i = 0; i < cc->ignore.len; i++) {
    const char *pat = cc->ignore.items[i];
    if (!pat || pat[0] == '\0') {
      continue;
    }
    if (strstr(rel, pat) != NULL || strstr(leaf, pat) != NULL) {
      return true;
    }
  }
  return false;
}

static void manifest_free(kbc_app_manifest *m) {
  for (size_t i = 0; i < m->len; i++) {
    free(m->items[i].path);
    free(m->items[i].title);
  }
  free(m->items);
  m->items = NULL;
  m->len = m->cap = 0;
}

static kbc_status manifest_push(kbc_app_manifest *m, const kbc_app_row *row,
                                kbc_err *err) {
  if (m->len == m->cap) {
    size_t cap = m->cap ? m->cap * 2u : 64u;
    if (cap < m->cap || cap > (SIZE_MAX / sizeof(*m->items))) {
      return kbc_err_set(err, KBC_ERR_NOMEM, "manifest would exceed %zu rows",
                         SIZE_MAX / sizeof(*m->items));
    }
    kbc_app_row *grown = (kbc_app_row *)realloc(m->items, cap * sizeof(*grown));
    if (!grown) {
      return kbc_err_set(err, KBC_ERR_NOMEM, "realloc for %zu manifest rows",
                         cap);
    }
    m->items = grown;
    m->cap = cap;
  }
  m->items[m->len] = *row;
  m->len++;
  return KBC_OK;
}

static kbc_status join_rel(kbc_str *out, const char *prefix, const char *leaf) {
  if (prefix[0] == '\0') {
    return kbc_str_puts(out, leaf);
  }
  kbc_status s = kbc_str_printf(out, "%s/%s", prefix, leaf);
  if (kbc_failed(s)) {
    return s;
  }
  if (out->len > (size_t)KBC_MAX_PATH_LEN) {
    return kbc_err_set(NULL, KBC_ERR_INVALID, "%s/%s: over the %u byte path cap",
                       prefix, leaf, (unsigned)KBC_MAX_PATH_LEN);
  }
  return KBC_OK;
}

/* --------------------------------------------------------------- summary */

/* First non-heading block, whitespace-collapsed and truncated. KBC_OWN; never
 * NULL on success, "" when the document has no prose. */
static char *summary_from_blocks(const kbc_blocks *b) {
  for (size_t i = 0; i < b->len; i++) {
    const kbc_block *blk = &b->items[i];
    if (blk->heading_level != 0 || blk->text_len == 0) {
      continue;
    }
    size_t n = blk->text_len;
    if (n > (size_t)KBC_APP_SUMMARY_MAX) {
      n = (size_t)KBC_APP_SUMMARY_MAX;
    }
    char *out = (char *)malloc(n + 1u);
    if (!out) {
      return NULL;
    }
    size_t w = 0;
    bool space = false;
    for (size_t j = 0; j < n; j++) {
      char c = blk->text[j];
      if (c == '\n' || c == '\t' || c == ' ' || c == '\r') {
        space = w > 0;
        continue;
      }
      if (space) {
        out[w++] = ' ';
        space = false;
      }
      out[w++] = c;
    }
    out[w] = '\0';
    return out;
  }
  return dup_cstr("");
}

/* --------------------------------------------------------------- ingest  */

/* The metadata ingest_file persists. KBC_OWN, freed with stored_free. */
typedef struct {
  char *title;
  int32_t heading_count;
  uint32_t content_hash;
} kbc_app_ingested;

static void ingested_free(kbc_app_ingested *g) {
  free(g->title);
  g->title = NULL;
  g->heading_count = 0;
  g->content_hash = 0;
}

static kbc_status read_bounded(const char *path, kbc_str *out, kbc_err *err) {
  kbc_status s = kbc_str_read_file(path, out, err);
  if (kbc_failed(s)) {
    return s;
  }
  if (out->len > (size_t)KBC_MAX_ARTIFACT_BYTES) {
    return kbc_err_set(err, KBC_ERR_IO,
                       "%s: %zu bytes exceeds the %u byte artifact limit", path,
                       out->len, (unsigned)KBC_MAX_ARTIFACT_BYTES);
  }
  return KBC_OK;
}

/* Reads, parses, stores and chunks one file. The id is minted here because
 * the store row and the index must agree on it (types.h: the same
 * (corpus, path, mtime_ns, size) always yields the same 12-hex id). */
static kbc_status ingest_file(kbc_app *app, const char *corpus_name,
                              const char *root, const char *rel,
                              int64_t mtime_ns, int64_t size_bytes,
                              kbc_app_ingested *out, kbc_err *err) {
  memset(out, 0, sizeof(*out));

  kbc_str full;
  kbc_str_init(&full);
  kbc_status s = kbc_str_printf(&full, "%s/%s", root, rel);
  if (kbc_failed(s)) {
    kbc_str_free(&full);
    return kbc_err_set(err, KBC_ERR_NOMEM, "path buffer for %s/%s", root, rel);
  }
  if (full.len > (size_t)KBC_MAX_PATH_LEN) {
    s = kbc_err_set(err, KBC_ERR_INVALID, "%s: path over %u bytes", full.ptr,
                    (unsigned)KBC_MAX_PATH_LEN);
    kbc_str_free(&full);
    return s;
  }

  kbc_arena *fa = kbc_arena_new(64u * 1024u);
  if (!fa) {
    kbc_str_free(&full);
    return kbc_err_set(err, KBC_ERR_NOMEM, "arena for %s", full.ptr);
  }

  kbc_str raw;
  kbc_str_init(&raw);
  s = read_bounded(full.ptr, &raw, err);
  if (kbc_failed(s)) {
    goto fail;
  }

  kbc_parsed *p = kbc_parse(fa, raw.ptr, raw.len, rel, err);
  if (!p) {
    s = kbc_err_set(err, KBC_ERR_PARSE, "parse %s: malformed document",
                    full.ptr);
    goto fail;
  }
  const kbc_blocks *blocks = kbc_parsed_blocks(p);

  char *summary = summary_from_blocks(blocks);
  if (!summary) {
    s = kbc_err_set(err, KBC_ERR_NOMEM, "summary for %s", full.ptr);
    goto fail;
  }

  char id[KBC_MAX_ID_LEN + 1];
  kbc_id_for_artifact(id, corpus_name, rel, mtime_ns, size_bytes);

  int32_t headings = 0;
  for (size_t i = 0; i < blocks->len; i++) {
    if (blocks->items[i].heading_level > 0) {
      headings++;
    }
  }

  kbc_artifact art;
  memset(&art, 0, sizeof(art));
  art.id = id;
  art.corpus = corpus_name;
  art.path = rel;
  art.title = kbc_parsed_title(p);
  art.kind = KBC_KIND_ARTIFACT;
  art.mtime_ns = mtime_ns;
  art.size_bytes = size_bytes;
  art.content_hash = kbc_fnv1a32(raw.ptr, raw.len);
  art.heading_count = headings;
  art.summary = summary;
  /* Copied into arena memory: raw.ptr may carry an embedded NUL, and the
   * stored record is a C string. */
  art.source = kbc_arena_strndup(fa, raw.ptr, raw.len);

  s = kbc_store_upsert_artifact(app->store, &art, err);
  if (kbc_failed(s)) {
    free(summary);
    goto fail;
  }

  /* Chunks are what a comment anchors to; a doc without them is uncommentable,
   * so they are part of the same transaction, not a later backfill. */
  kbc_chunk_in *chunks = NULL;
  if (blocks->len > 0) {
    chunks = (kbc_chunk_in *)calloc(blocks->len, sizeof(*chunks));
    if (!chunks) {
      free(summary);
      s = kbc_err_set(err, KBC_ERR_NOMEM, "%zu chunks for %s", blocks->len,
                      full.ptr);
      goto fail;
    }
    for (size_t i = 0; i < blocks->len; i++) {
      chunks[i].doc_id = art.id;
      chunks[i].ord = (uint32_t)i;
      chunks[i].text = blocks->items[i].text;
      chunks[i].text_len = blocks->items[i].text_len;
    }
  }
  s = kbc_store_replace_chunks(app->store, chunks, blocks->len, err);
  free(chunks);
  if (kbc_failed(s)) {
    free(summary);
    goto fail;
  }

  out->title = dup_cstr(art.title ? art.title : "");
  if (!out->title) {
    free(summary);
    s = kbc_err_set(err, KBC_ERR_NOMEM, "title copy for %s", full.ptr);
    goto fail;
  }
  out->heading_count = headings;
  out->content_hash = art.content_hash;
  free(summary);

  kbc_str_free(&raw);
  kbc_str_free(&full);
  kbc_arena_free(fa);
  return KBC_OK;

fail:
  kbc_str_free(&raw);
  kbc_str_free(&full);
  kbc_arena_free(fa);
  return s;
}

/* ----------------------------------------------------------------- walk  */

/* One directory, recursively. fstatat on the already-open dirfd — never
 * stat-then-open: the name can be swapped between the two, and a corpus is
 * hostile input by the threat model in AGENTS.md. */
static kbc_status walk_dir(kbc_app *app, const kbc_corpus_cfg *cc,
                           size_t corpus_index, const char *rel_prefix,
                           unsigned depth, kbc_app_manifest *m, kbc_err *err) {
  kbc_str dirpath;
  kbc_str_init(&dirpath);
  kbc_status s = kbc_str_printf(&dirpath, "%s/%s", cc->path, rel_prefix);
  if (kbc_failed(s)) {
    kbc_str_free(&dirpath);
    return kbc_err_set(err, KBC_ERR_NOMEM, "dir path for %s", cc->path);
  }

  DIR *d = opendir(dirpath.ptr);
  if (!d) {
    int saved = errno;
    s = kbc_err_set(err, KBC_ERR_IO, "opendir %s: %s", dirpath.ptr,
                    strerror(saved));
    kbc_str_free(&dirpath);
    return s;
  }
  const int dfd = dirfd(d);
  if (dfd < 0) {
    closedir(d);
    kbc_str_free(&dirpath);
    return kbc_err_set(err, KBC_ERR_INTERNAL, "dirfd %s: no descriptor",
                      dirpath.ptr);
  }

  struct dirent *ent;
  errno = 0;
  while ((ent = readdir(d)) != NULL) {
    if (ent->d_name[0] == '.') {
      continue; /* "." ".." and every dotfile */
    }

    kbc_str rel;
    kbc_str_init(&rel);
    if (kbc_failed(join_rel(&rel, rel_prefix, ent->d_name))) {
      s = kbc_err_set(err, KBC_ERR_INVALID, "%s/%s: path over the %u byte cap",
                      cc->path, rel_prefix, (unsigned)KBC_MAX_PATH_LEN);
      kbc_str_free(&rel);
      closedir(d);
      kbc_str_free(&dirpath);
      return s;
    }

    struct stat st;
    if (fstatat(dfd, ent->d_name, &st, 0) != 0) {
      /* Vanished between readdir and fstatat: not an error, the next reindex
       * will not see it either. */
      kbc_str_free(&rel);
      continue;
    }

    if (S_ISDIR(st.st_mode)) {
      if (depth + 1u >= KBC_APP_MAX_DEPTH) {
        KBC_LOGW("%s: nesting over %u levels, not descending", rel.ptr,
                 (unsigned)KBC_APP_MAX_DEPTH);
        kbc_str_free(&rel);
        continue;
      }
      if (corpus_ignores(cc, rel.ptr, ent->d_name)) {
        kbc_str_free(&rel);
        continue;
      }
      s = walk_dir(app, cc, corpus_index, rel.ptr, depth + 1u, m, err);
      kbc_str_free(&rel);
      if (kbc_failed(s)) {
        closedir(d);
        kbc_str_free(&dirpath);
        return s;
      }
      continue;
    }

    if (!S_ISREG(st.st_mode) || !is_indexable(rel.ptr) ||
        corpus_ignores(cc, rel.ptr, ent->d_name)) {
      kbc_str_free(&rel);
      continue;
    }
    if (st.st_size <= 0 ||
        (uintmax_t)st.st_size > (uintmax_t)KBC_MAX_ARTIFACT_BYTES) {
      KBC_LOGW("%s/%s: %ju bytes is out of range (1..%u), skipped", cc->path,
               rel.ptr, (uintmax_t)st.st_size,
               (unsigned)KBC_MAX_ARTIFACT_BYTES);
      m->n_skipped++;
      kbc_str_free(&rel);
      continue;
    }

    kbc_app_row row;
    memset(&row, 0, sizeof(row));
    row.corpus_index = corpus_index;
    row.path = dup_cstr(rel.ptr);
    if (!row.path) {
      s = kbc_err_set(err, KBC_ERR_NOMEM, "manifest row for %s", rel.ptr);
      kbc_str_free(&rel);
      closedir(d);
      kbc_str_free(&dirpath);
      return s;
    }
    row.mtime_ns = (int64_t)st.st_mtim.tv_sec * 1000000000LL +
                   (int64_t)st.st_mtim.tv_nsec;
    row.size_bytes = (int64_t)st.st_size;
    kbc_id_for_artifact(row.id, cc->name, row.path, row.mtime_ns,
                        row.size_bytes);
    kbc_str_free(&rel);

    /* The unchanged check is one indexed lookup against the stored row, which
     * already carries the previous mtime and size. This is the whole point of
     * the (mtime_ns, size) mint: an unedited file costs one stat and one
     * primary-key read, and no read of its bytes. */
    kbc_arena *qa = kbc_arena_new(4096u);
    if (!qa) {
      s = kbc_err_set(err, KBC_ERR_NOMEM, "arena for %s", row.path);
      free(row.path);
      closedir(d);
      kbc_str_free(&dirpath);
      return s;
    }
    kbc_artifact prev;
    memset(&prev, 0, sizeof(prev));
    kbc_err local;
    kbc_err_reset(&local);
    kbc_status q = kbc_store_get_artifact_by_path(app->store, qa, cc->name,
                                                  row.path, &prev, &local);
    kbc_arena_free(qa);
    if (kbc_failed(q)) {
      row.changed = true;
    } else {
      row.changed = !(prev.mtime_ns == row.mtime_ns &&
                      prev.size_bytes == row.size_bytes);
    }
    row.title = dup_cstr(prev.title ? prev.title : "");
    if (!row.title) {
      row.title = dup_cstr("");
    }
    if (!row.title) {
      s = kbc_err_set(err, KBC_ERR_NOMEM, "title copy for %s", row.path);
      free(row.path);
      closedir(d);
      kbc_str_free(&dirpath);
      return s;
    }

    if (row.changed) {
      m->n_changed++;
    } else {
      m->n_unchanged++;
    }
    s = manifest_push(m, &row, err);
    if (kbc_failed(s)) {
      free(row.path);
      free(row.title);
      closedir(d);
      kbc_str_free(&dirpath);
      return s;
    }
  }
  const int readdir_errno = errno;

  closedir(d);
  kbc_str_free(&dirpath);
  if (readdir_errno != 0) {
    return kbc_err_set(err, KBC_ERR_IO, "readdir %s/%s: %s", cc->path,
                       rel_prefix, strerror(readdir_errno));
  }
  return KBC_OK;
}

/* ------------------------------------------------------------- embedding */

/* Embeds one text. KBC_ARENA copy on success, NULL when the sidecar is
 * unusable. A dead sidecar degrades the search lane; it is never a reason to
 * fail a reindex. */
static float *embed_one(kbc_app *app, kbc_arena *a, const char *text,
                        size_t *dim_out) {
  *dim_out = 0;
  if (!app->embed || !kbc_embedder_healthy(app->embed)) {
    return NULL;
  }
  const char *texts[1];
  texts[0] = text;
  float *out = NULL;
  kbc_err local;
  kbc_err_reset(&local);
  kbc_status s = kbc_embedder_embed(app->embed, a, texts, 1u,
                                    kbc_embedder_dim(app->embed), &out, &local);
  if (kbc_failed(s) || !out) {
    KBC_LOGW("embedder: %s, vector lane degraded", local.msg);
    return NULL;
  }
  *dim_out = kbc_embedder_dim(app->embed);
  return *dim_out ? out : NULL;
}

/* ------------------------------------------------------------- the build */

/* Pass two: turn the manifest into a sealed index, embedding as it goes.
 * Doc ids are the manifest positions, so a vecstore row and an index doc id
 * are the same number by construction. */
static kbc_status build_index(kbc_app *app, kbc_app_manifest *m,
                              kbc_index **ix_out, kbc_vecstore **vec_out,
                              int64_t *embedded, kbc_err *err) {
  *ix_out = NULL;
  *vec_out = NULL;
  *embedded = 0;

  kbc_index *ix = kbc_index_new();
  if (!ix) {
    return kbc_err_set(err, KBC_ERR_NOMEM, "kbc_index_new");
  }
  kbc_status s = kbc_index_begin_build(ix, err);
  if (kbc_failed(s)) {
    kbc_index_free(ix);
    return s;
  }

  kbc_vecstore *vec = NULL;
  size_t vdim = 0;
  if (app->embed && kbc_embedder_healthy(app->embed)) {
    vdim = kbc_embedder_dim(app->embed);
    if (vdim > 0 && m->len > 0) {
      kbc_err local;
      kbc_err_reset(&local);
      vec = kbc_vecstore_new(vdim, (uint32_t)m->len, &local);
      if (!vec) {
        KBC_LOGW("vector store: %s, keyword lane only", local.msg);
      }
    }
  }

  int64_t n_embedded = 0;
  for (size_t i = 0; i < m->len; i++) {
    kbc_app_row *row = &m->items[i];
    const kbc_corpus_cfg *cc = &app->cfg->corpora[row->corpus_index];

    kbc_str full;
    kbc_str_init(&full);
    if (kbc_failed(kbc_str_printf(&full, "%s/%s", cc->path, row->path))) {
      s = kbc_err_set(err, KBC_ERR_NOMEM, "path buffer for %s", row->path);
      kbc_str_free(&full);
      goto fail;
    }

    kbc_arena *fa = kbc_arena_new(64u * 1024u);
    if (!fa) {
      s = kbc_err_set(err, KBC_ERR_NOMEM, "arena for %s", full.ptr);
      kbc_str_free(&full);
      goto fail;
    }

    kbc_str raw;
    kbc_str_init(&raw);
    s = read_bounded(full.ptr, &raw, err);
    if (kbc_failed(s)) {
      kbc_str_free(&raw);
      kbc_arena_free(fa);
      if (s != KBC_ERR_NOTFOUND) {
        kbc_str_free(&full);
        goto fail;
      }
      /* The file went away between the walk and this pass. Dropping the row
       * would punch a hole in the doc ids (index.h requires them dense), so
       * the doc is staged as a tombstone: zero tokens, still addressable by
       * path, matches nothing. */
      KBC_LOGW("%s vanished mid-reindex, staged as a tombstone", full.ptr);
      kbc_tokens empty;
      memset(&empty, 0, sizeof(empty));
      s = kbc_index_add_doc(ix, (uint32_t)i, cc->name, row->path, row->title,
                            KBC_KIND_ARTIFACT, &empty, err);
      kbc_str_free(&full);
      if (kbc_failed(s)) {
        goto fail;
      }
      continue;
    }

    kbc_parsed *p = kbc_parse(fa, raw.ptr, raw.len, row->path, err);
    if (!p) {
      s = kbc_err_set(err, KBC_ERR_PARSE, "parse %s: malformed document",
                      full.ptr);
      kbc_str_free(&raw);
      kbc_arena_free(fa);
      kbc_str_free(&full);
      goto fail;
    }
    if (row->title[0] == '\0') {
      const char *parsed_title = kbc_parsed_title(p);
      if (parsed_title && parsed_title[0] != '\0') {
        char *t = dup_cstr(parsed_title);
        if (!t) {
          s = kbc_err_set(err, KBC_ERR_NOMEM, "title for %s", full.ptr);
          kbc_str_free(&raw);
          kbc_arena_free(fa);
          kbc_str_free(&full);
          goto fail;
        }
        free(row->title);
        row->title = t;
      }
    }

    /* Searchable text: the title twice (the BM25 length-norm convention, and
     * the caller's decision per index.h), then the extracted prose. Raw markup
     * would only contribute terms nobody searches for. */
    kbc_str text;
    kbc_str_init(&text);
    const kbc_blocks *blocks = kbc_parsed_blocks(p);
    for (size_t b = 0; b < blocks->len + 2u; b++) {
      if (b < 2u) {
        if (kbc_failed(kbc_str_puts(&text, row->title)) ||
            kbc_failed(kbc_str_putc(&text, ' '))) {
          goto text_fail;
        }
        continue;
      }
      if (kbc_failed(kbc_str_putc(&text, ' ')) ||
          kbc_failed(kbc_str_append(&text, blocks->items[b - 2u].text,
                                    blocks->items[b - 2u].text_len))) {
        goto text_fail;
      }
    }

    kbc_tokens toks;
    memset(&toks, 0, sizeof(toks));
    s = kbc_tokenize(fa, text.ptr, text.len, &toks, err);
    if (kbc_failed(s)) {
      kbc_str_free(&text);
      kbc_str_free(&raw);
      kbc_arena_free(fa);
      kbc_str_free(&full);
      goto fail;
    }

    s = kbc_index_add_doc(ix, (uint32_t)i, cc->name, row->path, row->title,
                          KBC_KIND_ARTIFACT, &toks, err);
    if (kbc_failed(s)) {
      kbc_str_free(&text);
      kbc_str_free(&raw);
      kbc_arena_free(fa);
      kbc_str_free(&full);
      goto fail;
    }

    if (vec) {
      size_t dim = 0;
      float *v = embed_one(app, fa, text.ptr, &dim);
      if (v && dim == kbc_vecstore_dim(vec)) {
        kbc_err local;
        kbc_err_reset(&local);
        if (kbc_failed(kbc_vecstore_set(vec, (uint32_t)i, v, &local))) {
          KBC_LOGW("vector row %zu for %s: %s, dropping the vector lane", i,
                   full.ptr, local.msg);
          kbc_vecstore_free(vec);
          vec = NULL;
        } else {
          n_embedded++;
        }
      } else if (v) {
        /* Dimension drift between the sidecar and the store means the two
         * halves of the lane no longer agree; the keyword lane is still
         * correct, so degrade rather than publish mismatched rows. */
        KBC_LOGW("embedder returned dim %zu, store is %zu: vector lane off",
                 dim, kbc_vecstore_dim(vec));
        kbc_vecstore_free(vec);
        vec = NULL;
      }
    }

    kbc_str_free(&text);
    kbc_str_free(&raw);
    kbc_arena_free(fa);
    kbc_str_free(&full);
    continue;

  text_fail:
    s = kbc_err_set(err, KBC_ERR_NOMEM, "searchable text for %s", row->path);
    kbc_str_free(&text);
    kbc_str_free(&raw);
    kbc_arena_free(fa);
    kbc_str_free(&full);
    goto fail;
  }

  s = kbc_index_end_build(ix, err);
  if (kbc_failed(s)) {
    goto fail;
  }
  *ix_out = ix;
  *vec_out = vec;
  *embedded = n_embedded;
  return KBC_OK;

fail:
  kbc_index_free(ix);
  if (vec) {
    kbc_vecstore_free(vec);
  }
  return s;
}

/* Renames `<path>.build` over `path`. Used after kbc_index_save has already
 * fsync'd the build file, so the promotion itself is the atomic step and a
 * failure before it leaves the previous generation intact. */
static kbc_status promote_file(const char *path, kbc_err *err) {
  kbc_str tmp;
  kbc_str_init(&tmp);
  kbc_status s = kbc_str_printf(&tmp, "%s.build", path);
  if (kbc_failed(s)) {
    kbc_str_free(&tmp);
    return kbc_err_set(err, KBC_ERR_NOMEM, "temp path for %s", path);
  }
  if (rename(tmp.ptr, path) != 0) {
    int saved = errno;
    (void)unlink(tmp.ptr);
    s = kbc_err_set(err, KBC_ERR_IO, "rename %s to %s: %s", tmp.ptr, path,
                    strerror(saved));
    kbc_str_free(&tmp);
    return s;
  }
  kbc_str_free(&tmp);
  return KBC_OK;
}

/* Walk -> ingest -> build -> save -> promote -> swap. On any failure before
 * the swap the old index keeps serving: the daemon never publishes a
 * half-built index, and the store keeps the rows it already committed. */
static kbc_status reindex_locked(kbc_app *app, kbc_err *err) {
  const int64_t t0 = kbc_now_ns();

  kbc_app_manifest m;
  memset(&m, 0, sizeof(m));
  kbc_status s = KBC_OK;
  for (size_t i = 0; i < app->cfg->ncorpora; i++) {
    if (kbc_failed(s = walk_dir(app, &app->cfg->corpora[i], i, "", 0u, &m,
                                err))) {
      manifest_free(&m);
      return s;
    }
  }
  if (m.len > (size_t)UINT32_MAX) {
    size_t n = m.len;
    manifest_free(&m);
    return kbc_err_set(err, KBC_ERR_INVALID,
                       "%zu documents exceeds the %u doc id ceiling", n,
                       (unsigned)UINT32_MAX);
  }

  for (size_t i = 0; i < m.len; i++) {
    kbc_app_row *row = &m.items[i];
    if (!row->changed) {
      continue;
    }
    const kbc_corpus_cfg *cc = &app->cfg->corpora[row->corpus_index];
    kbc_app_ingested g;
    if (kbc_failed(s = ingest_file(app, cc->name, cc->path, row->path,
                                   row->mtime_ns, row->size_bytes, &g, err))) {
      if (s != KBC_ERR_NOTFOUND) {
        manifest_free(&m);
        return s;
      }
      /* Deleted between the walk and the ingest. The build pass will stage it
       * as a tombstone, so the doc ids stay dense and the index is correct
       * without a second full walk. */
      KBC_LOGW("%s/%s vanished mid-reindex, keeping the row as a tombstone",
               cc->name, row->path);
      kbc_err_reset(err);
      continue;
    }
    if (g.title && g.title[0] != '\0') {
      free(row->title);
      row->title = g.title;
      g.title = NULL;
    }
    ingested_free(&g);
  }

  kbc_index *ix = NULL;
  kbc_vecstore *vec = NULL;
  int64_t embedded = 0;
  if (kbc_failed(s = build_index(app, &m, &ix, &vec, &embedded, err))) {
    manifest_free(&m);
    return s;
  }

  kbc_str build_path;
  kbc_str_init(&build_path);
  s = kbc_str_printf(&build_path, "%s.build", app->cfg->index_path);
  if (!kbc_failed(s)) {
    s = kbc_index_save(ix, build_path.ptr, err);
  }
  if (!kbc_failed(s)) {
    s = promote_file(app->cfg->index_path, err);
  }
  kbc_str_free(&build_path);
  if (kbc_failed(s)) {
    manifest_free(&m);
    kbc_index_free(ix);
    if (vec) {
      kbc_vecstore_free(vec);
    }
    return s;
  }

  if (vec) {
    kbc_err local;
    kbc_err_reset(&local);
    if (kbc_failed(kbc_vecstore_save(vec, app->vec_path, &local))) {
      KBC_LOGW("vector store save: %s, keyword lane only", local.msg);
      kbc_vecstore_free(vec);
      vec = NULL;
    }
  }

  const int64_t t1 = kbc_now_ns();
  const int64_t docs = (int64_t)kbc_index_doc_count(ix);
  const int64_t terms = (int64_t)kbc_index_term_count(ix);
  const size_t n_changed = m.n_changed;
  const size_t n_unchanged = m.n_unchanged;
  const size_t n_skipped = m.n_skipped;

  /* The one place the live index changes. Both pointers move inside one write
   * lock, so a reader sees the new index only together with its vectors. */
  pthread_rwlock_wrlock(&app->lock);
  kbc_index *old_ix = app->index;
  kbc_vecstore *old_vec = app->vec;
  app->index = ix;
  app->vec = vec;
  if (old_ix) {
    kbc_index_free(old_ix);
  }
  if (old_vec) {
    kbc_vecstore_free(old_vec);
  }
  pthread_rwlock_unlock(&app->lock);

  atomic_store_explicit(&app->st_indexed, docs, memory_order_relaxed);
  atomic_store_explicit(&app->st_terms, terms, memory_order_relaxed);
  atomic_store_explicit(&app->st_docs, docs, memory_order_relaxed);
  atomic_store_explicit(&app->st_last_ns, t1, memory_order_relaxed);
  atomic_store_explicit(&app->st_last_docs, docs, memory_order_relaxed);
  atomic_store_explicit(&app->st_last_us, (t1 - t0) / 1000,
                        memory_order_relaxed);
  atomic_fetch_add_explicit(&app->st_runs, 1, memory_order_relaxed);

  manifest_free(&m);

  KBC_LOGI("reindex: %" PRId64 " docs, %" PRId64 " terms (%zu changed, %zu "
           "unchanged, %zu skipped, %" PRId64 " embedded) in %" PRId64 " us",
           docs, terms, n_changed, n_unchanged, n_skipped, embedded,
           (t1 - t0) / 1000);
  return KBC_OK;
}

/* ------------------------------------------------------------------ open */

static kbc_status config_clone(kbc_config **dst, const kbc_config *src,
                               kbc_err *err) {
  kbc_config *c = (kbc_config *)calloc(1, sizeof(*c));
  if (!c) {
    return kbc_err_set(err, KBC_ERR_NOMEM, "kbc_config copy");
  }
  c->port = src->port;
  c->bm25_k1 = src->bm25_k1;
  c->bm25_b = src->bm25_b;
  c->rrf_k = src->rrf_k;
  c->chunk_max_bytes = src->chunk_max_bytes;
  c->search_max_hits = src->search_max_hits;
  c->watcher_debounce_ms = src->watcher_debounce_ms;
  c->http_workers = src->http_workers;
  c->json_logs = src->json_logs;
  c->log_level = src->log_level;

  /* The eight owned strings, in header order. */
  char **slots[KBC_APP_CFG_FIELDS] = {
      &c->config_path, &c->data_dir, &c->db_path,      &c->index_path,
      &c->bind_addr,   &c->token_path, &c->token,      &c->embedder_cmd};
  const char *srcs[KBC_APP_CFG_FIELDS] = {
      src->config_path, src->data_dir, src->db_path,    src->index_path,
      src->bind_addr,   src->token_path, src->token,    src->embedder_cmd};

  for (size_t i = 0; i < KBC_APP_CFG_FIELDS; i++) {
    if (!srcs[i]) {
      continue;
    }
    *slots[i] = dup_cstr(srcs[i]);
    if (!*slots[i]) {
      kbc_config_free(c);
      return kbc_err_set(err, KBC_ERR_NOMEM, "copy of config field %zu", i);
    }
  }

  c->ncorpora = src->ncorpora;
  if (src->ncorpora > 0) {
    c->corpora = (kbc_corpus_cfg *)calloc(src->ncorpora, sizeof(*c->corpora));
    if (!c->corpora) {
      kbc_config_free(c);
      return kbc_err_set(err, KBC_ERR_NOMEM, "%zu corpora", src->ncorpora);
    }
    for (size_t i = 0; i < src->ncorpora; i++) {
      c->corpora[i].name = dup_cstr(src->corpora[i].name);
      c->corpora[i].path = dup_cstr(src->corpora[i].path);
      kbc_strlist_init(&c->corpora[i].ignore);
      for (size_t j = 0; j < src->corpora[i].ignore.len; j++) {
        if (kbc_failed(kbc_strlist_push(&c->corpora[i].ignore,
                                       src->corpora[i].ignore.items[j]))) {
          kbc_config_free(c);
          return kbc_err_set(err, KBC_ERR_NOMEM, "corpus %zu ignore %zu", i, j);
        }
      }
      if (!c->corpora[i].name || !c->corpora[i].path) {
        kbc_config_free(c);
        return kbc_err_set(err, KBC_ERR_NOMEM, "corpus %zu", i);
      }
    }
  }
  *dst = c;
  return KBC_OK;
}

static kbc_index *empty_index(kbc_err *err) {
  kbc_index *ix = kbc_index_new();
  if (!ix) {
    (void)kbc_err_set(err, KBC_ERR_NOMEM, "kbc_index_new");
    return NULL;
  }
  if (kbc_failed(kbc_index_begin_build(ix, err)) ||
      kbc_failed(kbc_index_end_build(ix, err))) {
    kbc_index_free(ix);
    return NULL;
  }
  return ix;
}

kbc_app *kbc_app_open(const kbc_config *cfg, kbc_err *err) {
  if (!cfg) {
    (void)kbc_err_set(err, KBC_ERR_INVALID, "kbc_app_open: cfg is NULL");
    return NULL;
  }
  if (!cfg->data_dir || !cfg->db_path || !cfg->index_path) {
    (void)kbc_err_set(err, KBC_ERR_INVALID,
                      "kbc_app_open: data_dir, db_path and index_path are "
                      "all required");
    return NULL;
  }
  if (cfg->ncorpora > (size_t)KBC_MAX_CORPORA) {
    (void)kbc_err_set(err, KBC_ERR_INVALID, "%zu corpora exceeds the %u limit",
                      cfg->ncorpora, (unsigned)KBC_MAX_CORPORA);
    return NULL;
  }

  kbc_app *app = (kbc_app *)calloc(1, sizeof(*app));
  if (!app) {
    (void)kbc_err_set(err, KBC_ERR_NOMEM, "kbc_app");
    return NULL;
  }
  if (pthread_rwlock_init(&app->lock, NULL) != 0) {
    free(app);
    (void)kbc_err_set(err, KBC_ERR_INTERNAL, "pthread_rwlock_init failed");
    return NULL;
  }
  if (pthread_mutex_init(&app->bus_lock, NULL) != 0) {
    pthread_rwlock_destroy(&app->lock);
    free(app);
    (void)kbc_err_set(err, KBC_ERR_INTERNAL, "pthread_mutex_init failed");
    return NULL;
  }
  app->next_sub_id = 1u;

  if (kbc_failed(config_clone(&app->cfg, cfg, err))) {
    goto fail;
  }

  kbc_str vp;
  kbc_str_init(&vp);
  kbc_status s = kbc_str_printf(&vp, "%s/%s", app->cfg->data_dir,
                                KBC_APP_VEC_FILE);
  if (kbc_failed(s) || (app->vec_path = dup_cstr(vp.ptr)) == NULL) {
    kbc_str_free(&vp);
    (void)kbc_err_set(err, KBC_ERR_NOMEM, "vector store path under %s",
                      app->cfg->data_dir);
    goto fail;
  }
  kbc_str_free(&vp);

  if (kbc_failed(kbc_mkdir_p(app->cfg->data_dir, err))) {
    goto fail;
  }

  app->store = kbc_store_open(app->cfg, err);
  if (!app->store) {
    goto fail;
  }

  /* A missing index is normal — first start, or before the first reindex. A
   * corrupt one is not: there are bytes on disk we cannot read, and quietly
   * starting empty would make "never indexed" and "written by another build"
   * indistinguishable to the user. index.h already returns KBC_ERR_PARSE with
   * the reason; pass it through. */
  if (kbc_path_exists(app->cfg->index_path)) {
    kbc_err local;
    kbc_err_reset(&local);
    app->index = kbc_index_open(app->cfg->index_path, &local);
    if (!app->index) {
      (void)kbc_err_set(err, local.status ? local.status : KBC_ERR_PARSE,
                        "open index %s: %s", app->cfg->index_path,
                        local.msg[0] ? local.msg : "unreadable");
      goto fail;
    }
  } else {
    app->index = empty_index(err);
    if (!app->index) {
      goto fail;
    }
  }

  if (kbc_path_exists(app->vec_path)) {
    kbc_err local;
    kbc_err_reset(&local);
    app->vec = kbc_vecstore_open(app->vec_path, &local);
    if (!app->vec) {
      KBC_LOGW("vector store %s unusable (%s), vector lane off", app->vec_path,
               local.msg);
    }
  }

  if (app->cfg->embedder_cmd && app->cfg->embedder_cmd[0] != '\0') {
    const char *argv[2];
    argv[0] = app->cfg->embedder_cmd;
    argv[1] = NULL;
    app->embed = kbc_embedder_start(argv, err);
    if (!app->embed) {
      goto fail;
    }
  }

  const int64_t docs = (int64_t)kbc_index_doc_count(app->index);
  atomic_store_explicit(&app->st_indexed, docs, memory_order_relaxed);
  atomic_store_explicit(&app->st_docs, docs, memory_order_relaxed);
  atomic_store_explicit(&app->st_terms,
                        (int64_t)kbc_index_term_count(app->index),
                        memory_order_relaxed);
  return app;

fail:
  kbc_app_close(app);
  return NULL;
}

void kbc_app_close(kbc_app *app) {
  if (!app) {
    return;
  }
  /* The watcher thread publishes onto the bus and calls back into the store;
   * it must be joined before either goes away. */
  kbc_app_stop_watcher(app);
  if (app->embed) {
    kbc_embedder_stop(app->embed);
    app->embed = NULL;
  }
  if (app->vec) {
    kbc_vecstore_free(app->vec);
  }
  if (app->index) {
    kbc_index_free(app->index);
  }
  if (app->store) {
    kbc_store_close(app->store);
  }
  free(app->vec_path);
  kbc_config_free(app->cfg);
  pthread_mutex_destroy(&app->bus_lock);
  pthread_rwlock_destroy(&app->lock);
  free(app);
}

const kbc_config *kbc_app_config(const kbc_app *app) {
  return app ? app->cfg : NULL;
}

/* --------------------------------------------------------------- reindex */

kbc_status kbc_app_reindex(kbc_app *app, kbc_err *err) {
  if (!app) {
    return kbc_err_set(err, KBC_ERR_INVALID, "kbc_app_reindex: app is NULL");
  }
  if (app->cfg->ncorpora == 0) {
    return kbc_err_set(err, KBC_ERR_INVALID, "no corpora configured");
  }
  kbc_status s = reindex_locked(app, err);
  if (kbc_failed(s)) {
    return s;
  }

  const int64_t docs =
      atomic_load_explicit(&app->st_docs, memory_order_relaxed);
  kbc_str payload;
  kbc_str_init(&payload);
  if (!kbc_failed(kbc_str_printf(&payload, "{\"docs\":%" PRId64 "}", docs))) {
    kbc_app_publish(app, "index.updated", payload.ptr);
  }
  kbc_str_free(&payload);
  return KBC_OK;
}

/* Bring one path up to date. The index is immutable once built (index.h: "a
 * running daemon never mutates a live index"), and index.h offers no
 * add-to-open operation, so a single-file update has no correct form cheaper
 * than a full rebuild. The rebuild is also the only form that cannot leave the
 * index a reindex behind, which is the invariant app.h exists to protect. */
static kbc_status reindex_one(kbc_app *app, const char *corpus,
                              const char *rel_path, bool removed, kbc_err *err) {
  if (!app) {
    return kbc_err_set(err, KBC_ERR_INVALID, "reindex: app is NULL");
  }
  if (!corpus || !rel_path || rel_path[0] == '\0') {
    return kbc_err_set(err, KBC_ERR_INVALID,
                       "reindex: corpus and path are both required");
  }
  if (strlen(rel_path) > (size_t)KBC_MAX_PATH_LEN) {
    return kbc_err_set(err, KBC_ERR_INVALID, "%s/%s: path over the %u byte cap",
                       corpus, rel_path, (unsigned)KBC_MAX_PATH_LEN);
  }
  /* A path from the watcher or a request body reaches the filesystem; '..' is
   * refused before it does. */
  if (strstr(rel_path, "..") != NULL) {
    return kbc_err_set(err, KBC_ERR_INVALID, "%s/%s: '..' is not allowed",
                       corpus, rel_path);
  }
  const kbc_corpus_cfg *cc = kbc_config_corpus(app->cfg, corpus);
  if (!cc) {
    return kbc_err_set(err, KBC_ERR_NOTFOUND, "corpus %s is not configured",
                       corpus);
  }

  if (removed) {
    kbc_arena *qa = kbc_arena_new(4096u);
    if (!qa) {
      return kbc_err_set(err, KBC_ERR_NOMEM, "arena for %s/%s", corpus,
                         rel_path);
    }
    kbc_artifact prev;
    memset(&prev, 0, sizeof(prev));
    kbc_status q =
        kbc_store_get_artifact_by_path(app->store, qa, corpus, rel_path, &prev,
                                       err);
    if (!kbc_failed(q)) {
      kbc_status d = kbc_store_delete_artifact(app->store, prev.id, err);
      if (kbc_failed(d)) {
        kbc_arena_free(qa);
        return d;
      }
    } else {
      /* Already gone: the rebuild below drops the doc either way, so this is
       * not a failure the caller needs to see. */
      kbc_err_reset(err);
    }
    kbc_arena_free(qa);
  }

  return kbc_app_reindex(app, err);
}

kbc_status kbc_app_reindex_file(kbc_app *app, const char *corpus,
                                const char *rel_path, kbc_err *err) {
  return reindex_one(app, corpus, rel_path, false, err);
}

kbc_status kbc_app_reindex_remove(kbc_app *app, const char *corpus,
                                  const char *rel_path, kbc_err *err) {
  return reindex_one(app, corpus, rel_path, true, err);
}

/* ---------------------------------------------------------------- search */

static kbc_status resolve_locked(void *ctx, kbc_arena *a, uint32_t doc_id,
                                 const char **artifact_id,
                                 const char **summary) {
  kbc_app_resolve_ctx *rc = (kbc_app_resolve_ctx *)ctx;
  const kbc_doc_meta *meta = kbc_index_doc(rc->ix, doc_id);
  if (!meta) {
    return kbc_err_set(NULL, KBC_ERR_NOTFOUND, "doc %u is out of range", doc_id);
  }
  kbc_artifact art;
  memset(&art, 0, sizeof(art));
  kbc_err local;
  kbc_err_reset(&local);
  kbc_status s = kbc_store_get_artifact_by_path(rc->app->store, a, meta->corpus,
                                                meta->path, &art, &local);
  if (kbc_failed(s)) {
    /* search.h: KBC_ERR_NOTFOUND drops the row. The index may legitimately be
     * one reindex ahead of the store, and failing the whole search over it
     * would be worse than returning one fewer hit. */
    return kbc_err_set(NULL, KBC_ERR_NOTFOUND, "%s/%s is not in the store",
                       meta->corpus, meta->path);
  }
  *artifact_id = art.id;
  *summary = art.summary;
  return KBC_OK;
}

kbc_status kbc_app_search(kbc_app *app, kbc_arena *a, const kbc_query *q,
                          kbc_search_result *out, kbc_err *err) {
  if (!app || !a || !q || !out) {
    return kbc_err_set(err, KBC_ERR_INVALID, "kbc_app_search: NULL argument");
  }
  if (!q->q || q->q[0] == '\0') {
    return kbc_err_set(err, KBC_ERR_INVALID, "query text is empty");
  }
  if (strlen(q->q) > (size_t)KBC_MAX_QUERY_LEN) {
    return kbc_err_set(err, KBC_ERR_INVALID, "query of %zu bytes exceeds %u",
                       strlen(q->q), (unsigned)KBC_MAX_QUERY_LEN);
  }
  memset(out, 0, sizeof(*out));

  const int64_t t0 = kbc_now_ns();

  pthread_rwlock_rdlock(&app->lock);

  kbc_arena *ea = NULL;
  float *vec = NULL;
  size_t vec_len = 0;
  if (app->embed && kbc_embedder_healthy(app->embed)) {
    /* Embedding is a subprocess round trip. It runs under the READ lock: a
     * slow sidecar must not stall the swap a concurrent reindex is waiting to
     * make, and the embedder serializes its own pipe internally. */
    ea = kbc_arena_new(16u * 1024u);
    if (ea) {
      vec = embed_one(app, ea, q->q, &vec_len);
    }
  }

  kbc_app_resolve_ctx rc;
  rc.app = app;
  rc.ix = app->index;

  kbc_err local;
  kbc_err_reset(&local);
  kbc_searcher *s = kbc_searcher_new(app->index, resolve_locked, &rc, &local);
  if (!s) {
    if (ea) {
      kbc_arena_free(ea);
    }
    pthread_rwlock_unlock(&app->lock);
    return kbc_err_set(err, local.status ? local.status : KBC_ERR_INTERNAL,
                       "kbc_searcher_new: %s",
                       local.msg[0] ? local.msg : "failed");
  }

  kbc_status rc_st = kbc_search_run(s, a, q, vec, vec_len, out, err);
  kbc_searcher_free(s);
  if (ea) {
    kbc_arena_free(ea);
  }
  pthread_rwlock_unlock(&app->lock);

  if (kbc_failed(rc_st)) {
    return rc_st;
  }

  out->took_us = (kbc_now_ns() - t0) / 1000;
  if (!vec) {
    out->degraded = true;
    out->vector_ran = false;
    atomic_fetch_add_explicit(&app->st_degraded, 1, memory_order_relaxed);
  }
  atomic_fetch_add_explicit(&app->st_searches, 1, memory_order_relaxed);
  return KBC_OK;
}

kbc_status kbc_app_get_artifact(kbc_app *app, kbc_arena *a, const char *id,
                                bool with_source, kbc_artifact *out,
                                kbc_err *err) {
  if (!app || !a || !id || !out) {
    return kbc_err_set(err, KBC_ERR_INVALID, "kbc_app_get_artifact: NULL "
                                            "argument");
  }
  if (!kbc_id_is_valid(id)) {
    return kbc_err_set(err, KBC_ERR_INVALID, "%s: not a %u-hex artifact id", id,
                       (unsigned)KBC_MAX_ID_LEN);
  }
  return kbc_store_get_artifact(app->store, a, id, with_source, out, err);
}

kbc_status kbc_app_list_artifacts(kbc_app *app, kbc_arena *a, const char *corpus,
                                  kbc_kind kind, size_t limit, size_t offset,
                                  kbc_artifact **out, size_t *n_out,
                                  kbc_err *err) {
  if (!app || !a || !out || !n_out) {
    return kbc_err_set(err, KBC_ERR_INVALID, "kbc_app_list_artifacts: NULL "
                                            "argument");
  }
  *out = NULL;
  *n_out = 0;

  char **ids = NULL;
  size_t n = 0;
  kbc_status s =
      kbc_store_list_artifact_ids(app->store, corpus, kind, limit, offset, &ids,
                                  &n, err);
  if (kbc_failed(s)) {
    return s;
  }
  if (n == 0) {
    free(ids);
    return KBC_OK;
  }

  kbc_artifact *rows = (kbc_artifact *)kbc_arena_calloc(a, n, sizeof(*rows));
  if (!rows) {
    for (size_t i = 0; i < n; i++) {
      free(ids[i]);
    }
    free(ids);
    return kbc_err_set(err, KBC_ERR_NOMEM, "%zu artifact rows", n);
  }
  size_t kept = 0;
  for (size_t i = 0; i < n; i++) {
    kbc_err local;
    kbc_err_reset(&local);
    if (kbc_failed(kbc_store_get_artifact(app->store, a, ids[i], false,
                                          &rows[kept], &local))) {
      /* Deleted between the id list and the read: skip it, keep the page
       * dense rather than reporting a row with no record behind it. */
      KBC_LOGW("list: artifact %s vanished mid-page: %s", ids[i], local.msg);
      continue;
    }
    kept++;
  }
  for (size_t i = 0; i < n; i++) {
    free(ids[i]);
  }
  free(ids);
  *out = rows;
  *n_out = kept;
  return KBC_OK;
}

/* --------------------------------------------------------------- watcher */

/* Runs on the watcher thread. It hands off to a rebuild, so a burst of
 * inotify events has to be debounced upstream — watcher.c coalesces per
 * (corpus, path) for cfg->watcher_debounce_ms, which is what keeps this from
 * being a rebuild per write. */
static void watcher_cb(void *user, const char *type, const char *json_payload) {
  kbc_app *app = (kbc_app *)user;
  kbc_arena *a = kbc_arena_new(8192u);
  if (!a) {
    return;
  }
  kbc_err local;
  kbc_err_reset(&local);
  kbc_json *j = kbc_json_parse(a, json_payload, strlen(json_payload), &local);
  if (!j || !kbc_json_is(j, KBC_JSON_OBJ)) {
    KBC_LOGW("watcher event %s: unparsable payload: %s", type, local.msg);
    kbc_arena_free(a);
    return;
  }
  const char *corpus = kbc_json_str(j, "corpus", "");
  const char *path = kbc_json_str(j, "path", "");
  if (corpus[0] == '\0' || path[0] == '\0') {
    KBC_LOGW("watcher event %s: no corpus/path in the payload", type);
    kbc_arena_free(a);
    return;
  }

  /* The payload's own flag is advisory; the filesystem is the authority. */
  bool removed = kbc_json_bool(j, "removed", false);
  const kbc_corpus_cfg *cc = kbc_config_corpus(app->cfg, corpus);
  if (cc) {
    kbc_str full;
    kbc_str_init(&full);
    if (!kbc_failed(kbc_str_printf(&full, "%s/%s", cc->path, path))) {
      removed = removed || !kbc_path_exists(full.ptr);
    }
    kbc_str_free(&full);
  }

  kbc_err ignored;
  kbc_err_reset(&ignored);
  kbc_status s = removed ? kbc_app_reindex_remove(app, corpus, path, &ignored)
                         : kbc_app_reindex_file(app, corpus, path, &ignored);
  if (kbc_failed(s)) {
    KBC_LOGW("watcher %s %s/%s: %s", type, corpus, path, ignored.msg);
  }
  kbc_arena_free(a);
}

kbc_status kbc_app_start_watcher(kbc_app *app, kbc_err *err) {
  if (!app) {
    return kbc_err_set(err, KBC_ERR_INVALID, "kbc_app_start_watcher: app is "
                                            "NULL");
  }
  if (app->watch) {
    return KBC_OK; /* idempotent */
  }
  kbc_err local;
  kbc_err_reset(&local);
  app->watch = kbc_watcher_start(app->cfg, watcher_cb, app, &local);
  if (!app->watch) {
    return kbc_err_set(err, local.status ? local.status : KBC_ERR_IO,
                       "kbc_watcher_start: %s",
                       local.msg[0] ? local.msg : "failed");
  }
  return KBC_OK;
}

void kbc_app_stop_watcher(kbc_app *app) {
  if (!app || !app->watch) {
    return;
  }
  kbc_watcher_stop(app->watch);
  app->watch = NULL;
}

/* ------------------------------------------------------------------- bus */

uint64_t kbc_app_subscribe(kbc_app *app, kbc_event_fn fn, void *user) {
  if (!app || !fn) {
    return 0;
  }
  uint64_t id = 0;
  pthread_mutex_lock(&app->bus_lock);
  for (size_t i = 0; i < KBC_APP_MAX_SUBS; i++) {
    if (app->subs[i].fn != NULL) {
      continue;
    }
    id = app->next_sub_id++;
    app->subs[i].fn = fn;
    app->subs[i].user = user;
    app->subs[i].id = id;
    break;
  }
  pthread_mutex_unlock(&app->bus_lock);
  if (id == 0) {
    KBC_LOGW("event bus: all %u subscription slots are in use",
             (unsigned)KBC_APP_MAX_SUBS);
  }
  return id;
}

void kbc_app_unsubscribe(kbc_app *app, uint64_t id) {
  if (!app || id == 0) {
    return;
  }
  pthread_mutex_lock(&app->bus_lock);
  for (size_t i = 0; i < KBC_APP_MAX_SUBS; i++) {
    if (app->subs[i].fn != NULL && app->subs[i].id == id) {
      memset(&app->subs[i], 0, sizeof(app->subs[i]));
      break;
    }
  }
  pthread_mutex_unlock(&app->bus_lock);
}

void kbc_app_publish(kbc_app *app, const char *type, const char *json_payload) {
  if (!app || !type || !json_payload) {
    return;
  }
  /* Snapshot the subscriber list and drop the lock before calling. A
   * callback is forbidden from re-entering kbc_app, but the bus must not
   * deadlock if one subscribes or unsubscribes while it is being called. */
  kbc_app_sub snap[KBC_APP_MAX_SUBS];
  size_t n = 0;
  pthread_mutex_lock(&app->bus_lock);
  for (size_t i = 0; i < KBC_APP_MAX_SUBS && n < KBC_APP_MAX_SUBS; i++) {
    if (app->subs[i].fn != NULL) {
      snap[n] = app->subs[i];
      n++;
    }
  }
  pthread_mutex_unlock(&app->bus_lock);

  for (size_t i = 0; i < n; i++) {
    snap[i].fn(snap[i].user, type, json_payload);
  }
}

/* ----------------------------------------------------------------- stats */

kbc_status kbc_app_stats_get(kbc_app *app, kbc_app_stats *out, kbc_err *err) {
  if (!app || !out) {
    return kbc_err_set(err, KBC_ERR_INVALID, "kbc_app_stats_get: NULL "
                                            "argument");
  }
  memset(out, 0, sizeof(*out));
  out->artifacts_indexed =
      atomic_load_explicit(&app->st_indexed, memory_order_relaxed);
  out->reindex_runs = atomic_load_explicit(&app->st_runs, memory_order_relaxed);
  out->searches_served =
      atomic_load_explicit(&app->st_searches, memory_order_relaxed);
  out->searches_degraded =
      atomic_load_explicit(&app->st_degraded, memory_order_relaxed);
  out->last_reindex_ns =
      atomic_load_explicit(&app->st_last_ns, memory_order_relaxed);
  out->last_reindex_docs =
      atomic_load_explicit(&app->st_last_docs, memory_order_relaxed);
  out->last_reindex_us =
      atomic_load_explicit(&app->st_last_us, memory_order_relaxed);
  out->index_terms = atomic_load_explicit(&app->st_terms, memory_order_relaxed);
  out->index_docs = atomic_load_explicit(&app->st_docs, memory_order_relaxed);

  int64_t bytes = 0;
  kbc_err local;
  kbc_err_reset(&local);
  if (!kbc_failed(kbc_store_total_bytes(app->store, &bytes, &local))) {
    atomic_store_explicit(&app->st_bytes, bytes, memory_order_relaxed);
  }
  out->db_bytes = atomic_load_explicit(&app->st_bytes, memory_order_relaxed);
  return KBC_OK;
}

void kbc_app_set_index(kbc_app *app, kbc_index *ix) {
  if (!app) {
    kbc_index_free(ix);
    return;
  }
  if (!ix) {
    return;
  }
  /* Takes ownership of `ix`; the previous one dies here, under the lock, so
   * no reader is still inside it. */
  pthread_rwlock_wrlock(&app->lock);
  kbc_index *old = app->index;
  app->index = ix;
  atomic_store_explicit(&app->st_indexed, (int64_t)kbc_index_doc_count(ix),
                        memory_order_relaxed);
  atomic_store_explicit(&app->st_docs, (int64_t)kbc_index_doc_count(ix),
                        memory_order_relaxed);
  atomic_store_explicit(&app->st_terms, (int64_t)kbc_index_term_count(ix),
                        memory_order_relaxed);
  if (old) {
    kbc_index_free(old);
  }
  pthread_rwlock_unlock(&app->lock);
}
