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

/* search.h gives the searcher no way to learn the document embeddings, so
 * search.c exports this setter and records the header gap there. Declared
 * here because search.h is frozen; when the header grows it, this
 * declaration goes away with it. */
kbc_status kbc_searcher_set_vecstore(kbc_searcher *s, const kbc_vecstore *vs,
                                     kbc_err *err);
#define KBC_APP_MAX_SUBS 64u
/* Recursion cap for the corpus walk; symlinked directories make this a
 * correctness concern, not merely a stack one. */
#define KBC_APP_MAX_DEPTH 64u
/* Summary length, in bytes, before truncation. */
#define KBC_APP_SUMMARY_MAX 512u
/* The eight owned char* in kbc_config, counted for the deep copy. */
#define KBC_APP_CFG_FIELDS 8u
#define KBC_APP_VEC_FILE "vectors.bin"
/* search.c's `since:` value grammar, which validates the atom there and is
 * applied here; see the CONTRACT GAP note there for the header line the
 * orchestrator should add. */
kbc_status kbc_since_value_ns(const char *value, int64_t *ns, kbc_err *err);
/* The link graph's write, and the per-source view it takes. Defined next to
 * store_forget_path; declared here because reindex_locked is the caller that
 * owns the whole document set. */
typedef struct {
  const char *corpus;
  const char *src;
  char **dst; /* owned targets; the array is ours to free */
  size_t n;
} app_edge_src;

static void app_edge_srcs_free(app_edge_src *v, size_t n);
static kbc_status store_write_links(kbc_app *app, app_edge_src *v, size_t nsrc,
                                    kbc_err *err);

/* Links recorded per document. A document with more outbound links than this
 * is a generated page, not curation; the cap keeps one file from turning a
 * reindex into a graph write the size of the corpus. */
#define APP_MAX_EDGES_PER_DOC 4096u

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

/* The metadata ingest_file persists. KBC_OWN, freed with ingested_free.
 *
 * `link_paths` are the document's outbound link targets, already resolved
 * against its own directory but NOT yet checked against the store: a target
 * that has not been ingested yet is not a document, and whether it is one
 * cannot be known until the whole pass has run. They are therefore carried
 * out of the parse and written once, after the last document is in. */
typedef struct {
  char *title;
  int32_t heading_count;
  uint32_t content_hash;
  char **link_paths; /* KBC_OWN, corpus-relative */
  size_t n_links;
} kbc_app_ingested;

static void ingested_free(kbc_app_ingested *g) {
  free(g->title);
  g->title = NULL;
  g->heading_count = 0;
  g->content_hash = 0;
  for (size_t i = 0; i < g->n_links; i++) free(g->link_paths[i]);
  free(g->link_paths);
  g->link_paths = NULL;
  g->n_links = 0;
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
 * (corpus, path) always yields the same 12-hex id, so an edited file updates
 * its row in place instead of colliding with the UNIQUE(corpus, path) one). */
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
  kbc_id_for_artifact(id, corpus_name, rel);

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

  /* The outbound link targets, carried out to be written once the pass has
   * stored every document — see kbc_app_ingested. */
  {
    const kbc_links *links = kbc_parsed_links(p);
    size_t nl = links->len;
    if (nl > APP_MAX_EDGES_PER_DOC) {
      KBC_LOGW("%s/%s: %zu links, only the first %u recorded", corpus_name, rel,
               nl, (unsigned)APP_MAX_EDGES_PER_DOC);
      nl = APP_MAX_EDGES_PER_DOC;
    }
    if (nl > 0) {
      out->link_paths = calloc(nl, sizeof(*out->link_paths));
      if (out->link_paths == NULL) {
        free(summary);
        s = kbc_err_set(err, KBC_ERR_NOMEM, "%zu link targets of %s/%s", nl,
                        corpus_name, rel);
        goto fail;
      }
      for (size_t i = 0; i < nl; i++) {
        out->link_paths[i] = dup_cstr(links->items[i].target);
        if (out->link_paths[i] == NULL) {
          free(summary);
          s = kbc_err_set(err, KBC_ERR_NOMEM, "link target copy for %s/%s",
                          corpus_name, rel);
          goto fail;
        }
        out->n_links++;
      }
    }
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
    kbc_id_for_artifact(row.id, cc->name, row.path);
    kbc_str_free(&rel);

    /* The unchanged check is one indexed lookup against the stored row, which
     * already carries the previous mtime and size. (mtime_ns, size) is the
     * change-detection fast path, never part of the name: an unedited file
     * costs one stat and one lookup, and no read of its bytes. */
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
    if (kbc_failed(q)) {
      row.changed = true;
    } else {
      row.changed = !(prev.mtime_ns == row.mtime_ns &&
                      prev.size_bytes == row.size_bytes);
    }
    row.title = dup_cstr(prev.title ? prev.title : "");
    kbc_arena_free(qa);
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

/* Splits the configured `[embedder] command` into an argv. The sidecar is not
 * a bare binary — `kb-embedder --model NAME --cache DIR` is the only way to
 * say which model to load — so the config value is a command LINE, not a
 * path. Splitting happens here rather than in config.c so the frozen
 * `char *embedder_cmd` contract keeps its meaning ("the command to run") while
 * what is exec'd stays honest about the arguments.
 *
 * Whitespace separates; '...' and "..." group; a backslash escapes the next
 * character inside quotes. No expansion of any kind: nothing here reaches a
 * shell, the argv is exec'd directly. `buf` is a mutable copy the caller owns
 * and frees — the words point into it, and kbc_embedder_start copies them
 * before returning, so the config value itself is never modified (it is
 * written back verbatim when the config is saved). */
static kbc_status split_command(char *buf, const char **argv, size_t max,
                                 kbc_err *err) {
  size_t n = 0;
  char *p = buf;
  while (*p != '\0') {
    while (*p == ' ' || *p == '\t' || *p == '\n') {
      p++;
    }
    if (*p == '\0') {
      break;
    }
    if (n + 1 >= max) {
      return kbc_err_set(err, KBC_ERR_INVALID,
                         "embedder command has more than %zu words", max - 1);
    }
    argv[n++] = p;
    char quote = '\0';
    while (*p != '\0') {
      if (quote != '\0') {
        if (*p == quote) {
          quote = '\0';
          p++;
          continue;
        }
        if (*p == '\\' && p[1] != '\0' && quote == '"') {
          p++;
        }
        p++;
        continue;
      }
      if (*p == '\'' || *p == '"') {
        quote = *p++;
        continue;
      }
      if (*p == ' ' || *p == '\t' || *p == '\n') {
        break;
      }
      p++;
    }
    if (quote != '\0') {
      return kbc_err_set(err, KBC_ERR_INVALID,
                         "embedder command has an unterminated %c quote",
                         quote);
    }
    if (*p != '\0') {
      *p++ = '\0';
    }
  }
  argv[n] = NULL;
  if (n == 0) {
    return kbc_err_set(err, KBC_ERR_INVALID,
                       "embedder command holds no executable");
  }
  return KBC_OK;
}

/* The text a document is indexed by: the title twice (the BM25 length-norm
 * convention, and the caller's decision per index.h), then the extracted
 * prose. Raw markup would only contribute terms nobody searches for. Shared by
 * the full build and the one-document update — two copies of this rule would
 * be two chances for the keyword lane to disagree with itself. */
static kbc_status searchable_text(const kbc_blocks *blocks, const char *title,
                                  kbc_str *out, kbc_err *err) {
  size_t b;
  for (b = 0; b < blocks->len + 2u; b++) {
    if (b < 2u) {
      if (kbc_failed(kbc_str_puts(out, title)) ||
          kbc_failed(kbc_str_putc(out, ' '))) {
        return kbc_err_set(err, KBC_ERR_NOMEM, "searchable text for \"%s\"",
                           title);
      }
      continue;
    }
    if (kbc_failed(kbc_str_putc(out, ' ')) ||
        kbc_failed(kbc_str_append(out, blocks->items[b - 2u].text,
                                  blocks->items[b - 2u].text_len))) {
      return kbc_err_set(err, KBC_ERR_NOMEM, "searchable text for \"%s\"",
                         title);
    }
  }
  return KBC_OK;
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

  /* The sidecar reports its dimension only once it has answered something,
   * and nothing has asked it anything yet at open time, so the store is
   * created on the first successful embed in the loop below rather than here.
   * `vec_tried` makes that a one-shot: a store that cannot be created is not
   * retried per document, and a healthy sidecar that never answers never
   * allocates one. */
  kbc_vecstore *vec = NULL;
  bool vec_tried = false;

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
    bool have_source = false;
    if (!row->changed) {
      /* The whole point of the (mtime_ns, size) fast path: an unedited file
       * must not have its bytes read. Its indexed text is whatever the last
       * committed store row holds, so re-reading the file here would smuggle
       * unindexed edits into the index behind the walk's back. */
      kbc_artifact prev;
      memset(&prev, 0, sizeof(prev));
      kbc_err local;
      kbc_err_reset(&local);
      kbc_status g = kbc_store_get_artifact(app->store, fa, row->id, true,
                                            &prev, &local);
      if (!kbc_failed(g) && prev.source) {
        s = kbc_str_append(&raw, prev.source, strlen(prev.source));
        if (kbc_failed(s)) {
          s = kbc_err_set(err, KBC_ERR_NOMEM, "stored source for %s",
                          row->path);
          kbc_str_free(&raw);
          kbc_arena_free(fa);
          kbc_str_free(&full);
          goto fail;
        }
        have_source = true;
      } else {
        /* No committed row behind the skip: the store is the authority and it
         * has nothing, so fall back to the file rather than index an empty
         * document. */
        KBC_LOGW("%s: no stored source for an unchanged file (%s), reading it",
                 row->path, local.msg);
      }
    }
    if (!have_source) {
      s = read_bounded(full.ptr, &raw, err);
    }
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
    s = searchable_text(kbc_parsed_blocks(p), row->title, &text, err);
    if (kbc_failed(s)) {
      goto text_fail;
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

    /* Embedded before the store decision: at this point nobody may know the
     * dimension yet, and a store sized for the wrong one is worse than none. */
    size_t dim = 0;
    float *v = embed_one(app, fa, text.ptr, &dim);
    if (v != NULL && vec == NULL && !vec_tried && dim > 0 && m->len > 0) {
      vec_tried = true;
      kbc_err local;
      kbc_err_reset(&local);
      vec = kbc_vecstore_new(dim, (uint32_t)m->len, &local);
      if (!vec) {
        KBC_LOGW("vector store: %s, keyword lane only", local.msg);
      }
    }
    if (v && vec) {
      if (dim == kbc_vecstore_dim(vec)) {
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
      } else {
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
    /* searchable_text already named the document; keep its message. */
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

/* -------------------------------------------------------- orphan sweep */

/* The corpus on disk is the authority, and a full walk is the moment that
 * authority is re-established. A store row whose (corpus, path) this walk did
 * not see belongs to a file that has since been renamed or deleted on disk;
 * leaving it behind is a permanent orphan, because no later reindex will ever
 * see the path either. Deleting it here also keeps the row count in step with
 * the corpus: a rename is a delete plus an insert, never a silent second row.
 *
 * The sweep is scoped to the corpora in this config and matches on the exact
 * (corpus, path) pair, so a row of a different corpus — or the same path under
 * a different corpus — is never touched. */

/* Sorted set of "corpus\x1fpath" keys. 0x1f cannot occur in either part, so
 * the join is unambiguous. Sorted + bsearch rather than a hash table: one
 * comparison function, no per-entry allocation beyond the key itself. */
typedef struct {
  char **keys; /* KBC_OWN, each a strdup'd key */
  size_t n, cap;
} path_set;

static void path_set_free(path_set *ps) {
  for (size_t i = 0; i < ps->n; i++) {
    free(ps->keys[i]);
  }
  free(ps->keys);
  ps->keys = NULL;
  ps->n = 0;
  ps->cap = 0;
}

static int path_key_cmp(const void *a, const void *b) {
  return strcmp(*(char *const *)a, *(char *const *)b);
}

static kbc_status path_set_push(path_set *ps, const char *key, kbc_err *err) {
  if (ps->n == ps->cap) {
    size_t cap = ps->cap ? ps->cap * 2u : 64u;
    if (cap < ps->cap || cap > SIZE_MAX / sizeof(*ps->keys)) {
      return kbc_err_set(err, KBC_ERR_NOMEM, "path set would exceed %zu keys",
                         SIZE_MAX / sizeof(*ps->keys));
    }
    char **grown = (char **)realloc(ps->keys, cap * sizeof(*grown));
    if (!grown) {
      return kbc_err_set(err, KBC_ERR_NOMEM, "realloc for %zu path keys", cap);
    }
    ps->keys = grown;
    ps->cap = cap;
  }
  char *copy = dup_cstr(key);
  if (!copy) {
    return kbc_err_set(err, KBC_ERR_NOMEM, "path key copy for \"%s\"", key);
  }
  ps->keys[ps->n++] = copy;
  return KBC_OK;
}

static bool path_set_has(const path_set *ps, const char *key) {
  if (ps->n == 0) {
    return false;
  }
  return bsearch(&key, ps->keys, ps->n, sizeof(*ps->keys), path_key_cmp) !=
         NULL;
}

/* One store row, one arena: the artifact's `path` is carved out of `qa`, so
 * the arena has to outlive every use of it — the probe key, the delete
 * warning and the log line all read `prev.path`. Keeping the arena inside
 * this function makes that lifetime the block it belongs to. */
static kbc_status sweep_one_id(kbc_app *app, const kbc_corpus_cfg *cc,
                               const char *id, const path_set *seen,
                               kbc_str *probe, size_t *removed, kbc_err *err) {
  kbc_arena *qa = kbc_arena_new(4096u);
  if (!qa) {
    return kbc_err_set(err, KBC_ERR_NOMEM, "arena for %s", id);
  }
  kbc_artifact prev;
  memset(&prev, 0, sizeof(prev));
  kbc_err local;
  kbc_err_reset(&local);
  kbc_status q =
      kbc_store_get_artifact(app->store, qa, id, false, &prev, &local);
  if (kbc_failed(q)) {
    /* Deleted between the id list and the read: nothing to orphan. */
    KBC_LOGW("sweep: artifact %s vanished mid-sweep: %s", id, local.msg);
    kbc_arena_free(qa);
    return KBC_OK;
  }

  kbc_status s = KBC_OK;
  kbc_str_clear(probe);
  s = kbc_str_printf(probe, "%s\x1f%s", cc->name,
                     prev.path ? prev.path : "");
  if (kbc_failed(s)) {
    s = kbc_err_set(err, KBC_ERR_NOMEM, "sweep key for %s", id);
  } else if (path_set_has(seen, probe->ptr)) {
    s = KBC_OK;
  } else {
    kbc_status d = kbc_store_delete_artifact(app->store, id, err);
    if (kbc_failed(d)) {
      /* One stuck row must not cost the whole rescan: the row stays
       * visible and the next reindex tries again. */
      KBC_LOGW("sweep: cannot drop %s/%s: %s", cc->name,
               prev.path ? prev.path : "?", err->msg);
      kbc_err_reset(err);
      s = KBC_OK;
    } else {
      (*removed)++;
      KBC_LOGI("sweep: %s/%s is no longer in the corpus, dropped", cc->name,
               prev.path ? prev.path : "?");
    }
  }
  kbc_arena_free(qa);
  return s;
}

/* Deletes the store rows for paths the walk did not see. `removed` is the
 * caller's log counter, not a manifest stat. */
static kbc_status sweep_orphans(kbc_app *app, const kbc_app_manifest *m,
                                size_t *removed, kbc_err *err) {
  path_set seen;
  memset(&seen, 0, sizeof(seen));
  kbc_str key;
  kbc_str_init(&key);

  kbc_status s = KBC_OK;
  for (size_t i = 0; i < m->len && !kbc_failed(s); i++) {
    kbc_str_clear(&key);
    s = kbc_str_printf(&key, "%s\x1f%s",
                       app->cfg->corpora[m->items[i].corpus_index].name,
                       m->items[i].path);
    if (kbc_failed(s)) {
      s = kbc_err_set(err, KBC_ERR_NOMEM, "seen-path key for %s",
                      m->items[i].path);
      break;
    }
    s = path_set_push(&seen, key.ptr, err);
  }
  if (!kbc_failed(s) && seen.n > 1) {
    qsort(seen.keys, seen.n, sizeof(*seen.keys), path_key_cmp);
  }
  kbc_str_free(&key);
  if (kbc_failed(s)) {
    path_set_free(&seen);
    return s;
  }

  for (size_t i = 0; i < app->cfg->ncorpora && !kbc_failed(s); i++) {
    const kbc_corpus_cfg *cc = &app->cfg->corpora[i];

    /* Every id for this corpus, collected before any delete: the paging
     * offset would skip rows once deleted rows shrink the result set. */
    kbc_strlist ids;
    kbc_strlist_init(&ids);
    for (size_t offset = 0; !kbc_failed(s);) {
      char **page = NULL;
      size_t np = 0;
      s = kbc_store_list_artifact_ids(app->store, cc->name, KBC_KIND_ARTIFACT,
                                     KBC_MAX_HITS, offset, &page, &np, err);
      if (kbc_failed(s)) {
        break;
      }
      for (size_t j = 0; j < np; j++) {
        kbc_status ps = kbc_strlist_push(&ids, page[j]);
        free(page[j]);
        if (kbc_failed(ps)) {
          s = kbc_err_set(err, KBC_ERR_NOMEM, "%zu stored ids for %s", np,
                          cc->name);
        }
      }
      free(page);
      if (kbc_failed(s) || np == 0) {
        break;
      }
      offset += np;
    }
    if (kbc_failed(s)) {
      kbc_strlist_free(&ids);
      break;
    }

    kbc_str probe;
    kbc_str_init(&probe);
    for (size_t j = 0; j < ids.len; j++) {
      s = sweep_one_id(app, cc, ids.items[j], &seen, &probe, removed, err);
      if (kbc_failed(s)) {
        break;
      }
    }
    kbc_str_free(&probe);
    kbc_strlist_free(&ids);
  }

  path_set_free(&seen);
  return s;
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

  /* The link graph is written after the loop, not inside it: a link target
   * that has not been ingested yet is not a document, and a document that
   * links to a sibling further down the walk must still produce an edge. */
  app_edge_src *edge_srcs = calloc(m.len > 0 ? m.len : 1, sizeof(*edge_srcs));
  size_t n_edge_srcs = 0;
  if (edge_srcs == NULL) {
    manifest_free(&m);
    return kbc_err_set(err, KBC_ERR_NOMEM, "%zu link sources", m.len);
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
        app_edge_srcs_free(edge_srcs, n_edge_srcs);
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
    /* EVERY changed document joins the write, links or not: a document that
     * lost its last link has to reach replace_edges with an empty target list,
     * which is what deletes the edges it used to have. */
    edge_srcs[n_edge_srcs].corpus = cc->name;
    edge_srcs[n_edge_srcs].src = row->path;
    edge_srcs[n_edge_srcs].dst = g.link_paths;
    edge_srcs[n_edge_srcs].n = g.n_links;
    n_edge_srcs++;
    g.link_paths = NULL;
    g.n_links = 0;
    ingested_free(&g);
  }

  if (!kbc_failed(s = store_write_links(app, edge_srcs, n_edge_srcs, err))) {
    /* nothing */
  }
  for (size_t i = 0; i < n_edge_srcs; i++) {
    for (size_t j = 0; j < edge_srcs[i].n; j++) free(edge_srcs[i].dst[j]);
    free(edge_srcs[i].dst);
  }
  app_edge_srcs_free(edge_srcs, n_edge_srcs);
  if (kbc_failed(s)) {
    manifest_free(&m);
    return s;
  }

  /* The walk is the authority on what the corpus contains, so the rows it did
   * not see go now — before the build, so the published index and the store
   * agree on the same document set. */
  size_t n_removed = 0;
  if (kbc_failed(s = sweep_orphans(app, &m, &n_removed, err))) {
    manifest_free(&m);
    return s;
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
           "unchanged, %zu skipped, %zu removed, %" PRId64 " embedded) in %"
           PRId64 " us",
           docs, terms, n_changed, n_unchanged, n_skipped, n_removed, embedded,
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
  c->graph_boost = src->graph_boost;
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
    const size_t clen = strlen(app->cfg->embedder_cmd);
    char *cbuf = malloc(clen + 1);
    if (cbuf == NULL) {
      (void)kbc_err_set(err, KBC_ERR_NOMEM, "embedder command buffer");
      goto fail;
    }
    memcpy(cbuf, app->cfg->embedder_cmd, clen + 1);
    const char *argv[64];
    kbc_status cst =
        split_command(cbuf, argv, sizeof argv / sizeof argv[0], err);
    app->embed = kbc_failed(cst) ? NULL : kbc_embedder_start(argv, err);
    free(cbuf);
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

/* Drops the store row for one path. A path that is not there is not a failure:
 * the caller is bringing the index in line with the filesystem, and the
 * filesystem has already said the document does not exist. */
static kbc_status store_forget_path(kbc_app *app, const char *corpus,
                                    const char *rel_path, kbc_err *err) {
  kbc_arena *qa = kbc_arena_new(4096u);
  if (qa == NULL) {
    return kbc_err_set(err, KBC_ERR_NOMEM, "arena for %s/%s", corpus, rel_path);
  }
  kbc_artifact prev;
  memset(&prev, 0, sizeof prev);
  kbc_status s = KBC_OK;
  kbc_err local;
  kbc_err_reset(&local);
  if (kbc_failed(kbc_store_get_artifact_by_path(app->store, qa, corpus, rel_path,
                                                &prev, &local))) {
    kbc_arena_free(qa);
    /* Not a row, but the edges it left are still ours to drop: a document
     * that was re-ingested without its store row is not a document. */
    return kbc_store_delete_edges(app->store, corpus, rel_path, err);
  }
  /* The edges leaving a document go with it. Backlinks INTO it are kept: a
   * document that is unlinked today may be linked again the moment it is
   * re-ingested, and dropping them would make every other document's
   * in-degree depend on the order files happened to be scanned in. */
  s = kbc_store_delete_edges(app->store, corpus, rel_path, err);
  if (kbc_failed(s)) {
    kbc_arena_free(qa);
    return s;
  }
  s = kbc_store_delete_artifact(app->store, prev.id, err);
  kbc_arena_free(qa);
  return s;
}

/* The link graph, kept in step with the documents.
 *
 * Every re-ingest REPLACES the edges leaving that path, and a removal drops
 * them, so a file whose links changed cannot leave the previous edges behind.
 * A target that is not an indexed document is not an edge at all (store.h:
 * the table is a subset of what the parser found, never a superset), which is
 * why this runs after the whole pass has stored its documents: whether a
 * target is a document is not knowable while the pass is still half done, and
 * an order-dependent graph is a graph that flips on the next reindex.
 *
 * The targets of every source are resolved against the store in ONE batch;
 * each source's own replace is then a single transaction. */
static void app_edge_srcs_free(app_edge_src *v, size_t n) {
  free(v);
  (void)n;
}

static kbc_status store_write_links(kbc_app *app, app_edge_src *v, size_t nsrc,
                                    kbc_err *err) {
  size_t total = 0;
  for (size_t i = 0; i < nsrc; i++) total += v[i].n;
  /* total == 0 is NOT a no-op: every source in this batch has lost its last
   * link, and the write that deletes those edges is exactly the point. Only
   * the batch RESOLVE needs targets, so that is what is skipped. */
  if (total == 0) {
    for (size_t i = 0; i < nsrc; i++) {
      kbc_status s = kbc_store_replace_edges(app->store, v[i].corpus, v[i].src,
                                             NULL, 0, err);
      if (kbc_failed(s)) return s;
    }
    KBC_LOGI("graph: %zu documents, 0 of 0 link targets recorded", nsrc);
    return KBC_OK;
  }

  kbc_arena *ea = kbc_arena_new(64u * 1024u);
  if (ea == NULL)
    return kbc_err_set(err, KBC_ERR_NOMEM, "link arena for %zu targets", total);
  const char **corpora = kbc_arena_calloc(ea, total, sizeof(*corpora));
  const char **paths = kbc_arena_calloc(ea, total, sizeof(*paths));
  if (corpora == NULL || paths == NULL) {
    kbc_arena_free(ea);
    return kbc_err_set(err, KBC_ERR_NOMEM, "link batch of %zu targets", total);
  }
  size_t at = 0;
  for (size_t i = 0; i < nsrc; i++) {
    for (size_t j = 0; j < v[i].n; j++) {
      corpora[at] = v[i].corpus;
      paths[at] = v[i].dst[j];
      at++;
    }
  }

  kbc_artifact **arts = NULL;
  kbc_err local;
  kbc_err_reset(&local);
  kbc_status s = kbc_store_get_artifacts_by_path(app->store, ea, corpora, paths,
                                                 total, &arts, &local);
  if (kbc_failed(s)) {
    kbc_arena_free(ea);
    return kbc_err_set(err, s, "resolve %zu link targets: %s", total,
                       local.msg[0] ? local.msg : "store failed");
  }

  const char **kept = calloc(total, sizeof(*kept));
  if (kept == NULL) {
    free(arts);
    kbc_arena_free(ea);
    return kbc_err_set(err, KBC_ERR_NOMEM, "%zu resolved links", total);
  }
  at = 0;
  size_t recorded = 0;
  s = KBC_OK;
  for (size_t i = 0; i < nsrc && s == KBC_OK; i++) {
    size_t nk = 0;
    for (size_t j = 0; j < v[i].n; j++) {
      /* The resolved path is the artifact's own, not the link's spelling:
       * `../x/y.md` and `x/y.md` are one edge, and the primary key says so. */
      if (arts[at + j] != NULL) kept[nk++] = arts[at + j]->path;
    }
    s = kbc_store_replace_edges(app->store, v[i].corpus, v[i].src, kept, nk,
                                err);
    recorded += nk;
    at += v[i].n;
  }
  free(kept);
  free(arts);
  kbc_arena_free(ea);
  if (s == KBC_OK) {
    /* Three numbers, and they are not the same number: the parser found
     * `total` link targets, `recorded` of them resolved to an indexed
     * document, and the graph holds `edges` rows — fewer than `recorded`
     * because a source linking one document twice has one edge. Only the last
     * is the table itself. */
    const int64_t edges = kbc_store_edge_count(app->store, v[0].corpus, NULL);
    KBC_LOGI("graph: %zu documents, %zu of %zu link targets resolved, %lld "
             "edges in the graph",
             nsrc, recorded, total, (long long)edges);
  }
  return s;
}

/* One document, in place: read it, tokenize it, rewrite only its postings.
 *
 * The mutation runs under the write lock — a reader must never see a
 * half-rewritten postings array — and the result is saved and promoted before
 * the lock is released, so a crash mid-update leaves the previous generation
 * on disk and the daemon comes back up on it. The store row is committed
 * before any of this (reindex_one), so app.h's invariant holds throughout: the
 * index never references a document the store has not committed.
 *
 * `vanished` means the file is not on disk, which is a removal rather than an
 * update.
 *
 * `rel_path` is CORPUS-RELATIVE — the same shape the watcher publishes and the
 * store and index are keyed on. The corpus root is joined once, into `full`,
 * and never prepended to the key.
 */
static kbc_status index_touch_one(kbc_app *app, const kbc_corpus_cfg *cc,
                                  const char *rel_path, bool vanished,
                                  kbc_err *err) {
  const int64_t t0 = kbc_now_ns();
  kbc_arena *fa = kbc_arena_new(64u * 1024u);
  if (fa == NULL) {
    return kbc_err_set(err, KBC_ERR_NOMEM, "arena for %s/%s", cc->name,
                       rel_path);
  }
  kbc_str full, text, raw;
  kbc_str_init(&full);
  kbc_str_init(&text);
  kbc_str_init(&raw);
  kbc_tokens toks;
  memset(&toks, 0, sizeof toks);
  const char *title = "";
  float *v = NULL;
  size_t dim = 0;
  kbc_status s;
  uint32_t doc_id, target = UINT32_MAX, docs_before = 0;
  bool dropped_vec = false;

  s = kbc_str_printf(&full, "%s/%s", cc->path, rel_path);
  if (kbc_failed(s)) {
    s = kbc_err_set(err, KBC_ERR_NOMEM, "path buffer for %s/%s", cc->name,
                    rel_path);
    goto out;
  }
  if (!vanished) {
    s = read_bounded(full.ptr, &raw, err);
    if (kbc_failed(s)) {
      goto out;
    }
    kbc_parsed *p = kbc_parse(fa, raw.ptr, raw.len, rel_path, err);
    if (p == NULL) {
      s = kbc_err_set(err, KBC_ERR_PARSE, "parse %s: malformed document",
                      full.ptr);
      goto out;
    }
    const char *parsed_title = kbc_parsed_title(p);
    if (parsed_title != NULL) {
      title = parsed_title;
    }
    s = searchable_text(kbc_parsed_blocks(p), title, &text, err);
    if (kbc_failed(s)) {
      goto out;
    }
    s = kbc_tokenize(fa, text.ptr, text.len, &toks, err);
    if (kbc_failed(s)) {
      goto out;
    }
    /* Embedded before the write lock, because the embedder is a subprocess
     * round trip and the lock is the one thing searches queue behind. */
    v = embed_one(app, fa, text.ptr, &dim);
  }

  pthread_rwlock_wrlock(&app->lock);
  doc_id = kbc_index_id_of(app->index, cc->name, rel_path);
  docs_before = kbc_index_doc_count(app->index);
  if (vanished) {
    if (doc_id != UINT32_MAX) {
      s = kbc_index_remove_doc(app->index, doc_id, err);
      if (kbc_failed(s)) {
        pthread_rwlock_unlock(&app->lock);
        goto out;
      }
      /* A removal renumbers every document above it, so the vector rows keyed
       * on those ids now describe the wrong documents. The store has no
       * per-row delete, and a vector lane that returns another document's
       * neighbours is worse than no vector lane: drop it, loudly, and let the
       * next full reindex rebuild it. Removing the LAST document renumbers
       * nothing, so that case keeps its lane. */
      if (app->vec != NULL && doc_id + 1u < docs_before) {
        kbc_vecstore_free(app->vec);
        app->vec = NULL;
        dropped_vec = true;
      }
    }
  } else {
    target = (doc_id == UINT32_MAX) ? docs_before : doc_id;
    s = kbc_index_update_doc(app->index, doc_id, cc->name, rel_path, title,
                             KBC_KIND_ARTIFACT, &toks, err);
    if (kbc_failed(s)) {
      pthread_rwlock_unlock(&app->lock);
      goto out;
    }
    /* A replace keeps every doc id, and an append uses one past the old end,
     * so `target` is the row this text belongs to. */
    if (v != NULL && app->vec != NULL) {
      if (dim != kbc_vecstore_dim(app->vec)) {
        KBC_LOGW("embedder returned dim %zu, store is %zu; vector lane off", dim,
                 kbc_vecstore_dim(app->vec));
      } else {
        kbc_err local;
        kbc_err_reset(&local);
        if (kbc_failed(kbc_vecstore_set(app->vec, target, v, &local))) {
          KBC_LOGW("vector row %u for %s: %s, dropping the vector lane", target,
                   rel_path, local.msg);
          kbc_vecstore_free(app->vec);
          app->vec = NULL;
          dropped_vec = true;
        }
      }
    }
  }

  kbc_str build_path;
  kbc_str_init(&build_path);
  s = kbc_str_printf(&build_path, "%s.build", app->cfg->index_path);
  if (!kbc_failed(s)) {
    s = kbc_index_save(app->index, build_path.ptr, err);
  }
  if (!kbc_failed(s)) {
    s = promote_file(app->cfg->index_path, err);
  }
  kbc_str_free(&build_path);
  if (kbc_failed(s)) {
    pthread_rwlock_unlock(&app->lock);
    goto out;
  }
  const int64_t docs = (int64_t)kbc_index_doc_count(app->index);
  atomic_store_explicit(&app->st_indexed, docs, memory_order_relaxed);
  atomic_store_explicit(&app->st_docs, docs, memory_order_relaxed);
  atomic_store_explicit(&app->st_terms, (int64_t)kbc_index_term_count(app->index),
                        memory_order_relaxed);
  atomic_store_explicit(&app->st_last_ns, kbc_now_ns(), memory_order_relaxed);
  atomic_store_explicit(&app->st_last_docs, docs, memory_order_relaxed);
  /* st_runs counts FULL scans, and this is not one; last_reindex_* is "when the
 * index last changed", which this is. */
  atomic_store_explicit(&app->st_last_us, (kbc_now_ns() - t0) / 1000,
                        memory_order_relaxed);
  pthread_rwlock_unlock(&app->lock);

  kbc_str payload;
  kbc_str_init(&payload);
  if (!kbc_failed(kbc_str_printf(&payload, "{\"docs\":%" PRId64 "}", docs))) {
    kbc_app_publish(app, "index.updated", payload.ptr);
  }
  kbc_str_free(&payload);
  KBC_LOGI("reindex: %s/%s %s, %" PRId64 " docs in %" PRId64 " us%s", cc->name,
           rel_path, vanished ? "removed" : "updated", docs,
           (kbc_now_ns() - t0) / 1000,
           dropped_vec ? ", vector lane off" : "");

out:
  kbc_str_free(&text);
  kbc_str_free(&raw);
  kbc_str_free(&full);
  kbc_arena_free(fa);
  return s;
}

/* Bring one path up to date, without touching any other. The filesystem is the
 * authority, exactly as it is for a full scan: a delete event for a file that
 * is still there re-ingests it rather than dropping it, and a save event for a
 * file that has since been deleted drops it. */
static kbc_status reindex_one(kbc_app *app, const char *corpus,
                              const char *rel_path, kbc_err *err) {
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
  /* No index to update in place (first start, before any reindex): a full scan
   * is not a fallback here, it is the only thing that can work. */
  if (app->index == NULL) {
    return kbc_app_reindex(app, err);
  }

  /* The filesystem is the authority. A path that is not there is a removal:
   * the store row goes first, so the index is never the side that still knows
   * about a document the store has dropped. */
  struct stat sb;
  kbc_str full;
  kbc_str_init(&full);
  kbc_status s = kbc_str_printf(&full, "%s/%s", cc->path, rel_path);
  bool vanished = kbc_failed(s) || stat(full.ptr, &sb) != 0;

  if (vanished) {
    kbc_str_free(&full);
    if (!kbc_failed(store_forget_path(app, corpus, rel_path, err))) {
      s = index_touch_one(app, cc, rel_path, true, err);
    }
    goto done;
  }

  kbc_app_ingested g;
  s = ingest_file(app, cc->name, cc->path, rel_path,
                  (int64_t)sb.st_mtim.tv_sec * 1000000000LL +
                      (int64_t)sb.st_mtim.tv_nsec,
                  (int64_t)sb.st_size, &g, err);
  kbc_str_free(&full);
  if (kbc_failed(s)) {
    if (s != KBC_ERR_NOTFOUND) {
      return s;
    }
    /* Vanished between the stat and the read. */
    KBC_LOGW("%s/%s vanished mid-reindex, dropping it from the index", corpus,
             rel_path);
    kbc_err_reset(err);
    s = store_forget_path(app, corpus, rel_path, err);
    if (kbc_failed(s)) {
      return s;
    }
    s = index_touch_one(app, cc, rel_path, true, err);
    goto done;
  }
  /* The single-document path writes its own edges here: the rest of the
   * corpus is already in the store, so a target that is a document resolves
   * now. A target that is not yet indexed records no edge, and the next full
   * scan — which every watcher event is followed by — records it. */
  if (g.n_links > 0) {
    app_edge_src one;
    one.corpus = corpus;
    one.src = rel_path;
    one.dst = g.link_paths;
    one.n = g.n_links;
    s = store_write_links(app, &one, 1, err);
  } else {
    s = kbc_store_delete_edges(app->store, corpus, rel_path, err);
  }
  if (kbc_failed(s)) {
    ingested_free(&g);
    return s;
  }
  ingested_free(&g);
  s = index_touch_one(app, cc, rel_path, false, err);

done:
  if (kbc_failed(s)) {
    /* The store row is committed and the index is not, so this file's
     * keywords are stale until something else reindexes it. A watcher does
     * not re-fire for a file it has already reported, so a full scan now is
     * what keeps the invariant app.h exists for — the rare path, and the only
     * one that cannot leave the daemon a reindex behind. */
    KBC_LOGW("reindex %s/%s: %s; falling back to a full scan", corpus, rel_path,
             err->msg);
    kbc_err_reset(err);
    return kbc_app_reindex(app, err);
   }
  return KBC_OK;
 }

kbc_status kbc_app_reindex_file(kbc_app *app, const char *corpus,
                                const char *rel_path, kbc_err *err) {
  return reindex_one(app, corpus, rel_path, err);
}

kbc_status kbc_app_reindex_remove(kbc_app *app, const char *corpus,
                                  const char *rel_path, kbc_err *err) {
  return reindex_one(app, corpus, rel_path, err);
}


/* ---------------------------------------------------------------- search */
/* Resolves every surviving row in ONE store round trip.
 *
 * The searcher asked the store once per row, and the store guards one
 * connection with one mutex: a query returning k hits took that mutex k
 * times, and the convoy behind it — not the scoring, which is thread-local by
 * design — is what capped query throughput under concurrency. The index may
 * legitimately be one reindex ahead of the store, so a row the batch does not
 * resolve is dropped with a debug line and the search still succeeds. */
static kbc_status resolve_rows(kbc_app *app, kbc_arena *a,
                               kbc_search_result *out, int64_t since_ns,
                               kbc_err *err) {
  if (out->len == 0) return KBC_OK;

  const char **corpora = kbc_arena_calloc(a, out->len, sizeof(*corpora));
  const char **paths = kbc_arena_calloc(a, out->len, sizeof(*paths));
  if (corpora == NULL || paths == NULL)
    return kbc_err_set(err, KBC_ERR_NOMEM, "search: %zu hit pairs", out->len);
  for (size_t i = 0; i < out->len; i++) {
    /* A row with no corpus or no path cannot name a store row. The empty
     * string is the batch's own "unlookupable" marker: that slot comes back
     * NULL, exactly as a row with no store record does. */
    corpora[i] = out->rows[i].corpus != NULL ? out->rows[i].corpus : "";
    paths[i] = out->rows[i].path != NULL ? out->rows[i].path : "";
  }

  kbc_artifact **arts = NULL;
  kbc_err local;
  kbc_err_reset(&local);
  kbc_status s =
      kbc_store_get_artifacts_by_path(app->store, a, corpora, paths, out->len,
                                      &arts, &local);
  if (kbc_failed(s)) {
    free(arts);
    return kbc_err_set(err, s, "resolve %zu search hits: %s", out->len,
                       local.msg[0] ? local.msg : "store failed");
  }

  size_t kept = 0;
  for (size_t i = 0; i < out->len; i++) {
    kbc_result_row *r = &out->rows[i];
    if (arts[i] == NULL) {
      KBC_LOGD("search: doc %u not in store, dropping", r->doc_id);
      continue;
    }
    /* `since:` is a mtime predicate and the mtime lives here, one layer above
     * the index. It shares the unresolved-row path on purpose: a row that
     * cannot be resolved is dropped either way, so a since: filter changes
     * WHICH rows survive, never WHETHER an unresolvable one does. */
    if (since_ns > 0 && arts[i]->mtime_ns < since_ns) {
      KBC_LOGD("search: %s older than since:, dropping", arts[i]->path);
      continue;
    }
    r->artifact_id = arts[i]->id;
    r->summary = arts[i]->summary;
    if (kept != i) out->rows[kept] = *r;
    kept++;
  }
  out->len = kept;
  free(arts);
  return KBC_OK;
}

/* `since:<value>` out of the raw query string, in the same token shape the
 * grammar reads (query.rs:580): words split on whitespace, '(', ')', and
 * '"'; a ':' is its own token between a key and its value. The grammar
 * validates the value and drops the atom; the THRESHOLD is applied here,
 * because the mtime is the store's and the store is this layer. Two atoms
 * AND together, so the strictest wins — the same fold search.c does.
 *
 * `NOT since:` is skipped, matching the grammar: there is no upper bound to
 * negate, so the original warns and ignores it, and filtering on an atom the
 * grammar dropped would be two different queries under one name. */
typedef struct {
  const char *p;
  size_t len;
  bool quoted;
  bool colon;
} app_tok;

/* One token, or false at the end of the string. `quoted` carries the `"…"`
 * form, whose contents are a value even when they contain a space. */
static bool app_next_tok(const char *q, size_t len, size_t *i, app_tok *t) {
  while (*i < len) {
    const char c = q[*i];
    if (c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '(' ||
        c == ')') {
      (*i)++;
      continue;
    }
    if (c == ':') {
      (*i)++;
      t->p = q + *i - 1;
      t->len = 1;
      t->quoted = false;
      t->colon = true;
      return true;
    }
    if (c == '"') {
      const size_t s = ++(*i);
      while (*i < len && q[*i] != '"') (*i)++;
      t->p = q + s;
      t->len = *i - s;
      t->quoted = true;
      t->colon = false;
      if (*i < len) (*i)++;
      return true;
    }
    const size_t s = *i;
    while (*i < len && q[*i] != ' ' && q[*i] != '\t' && q[*i] != '\n' &&
           q[*i] != '\r' && q[*i] != '(' && q[*i] != ')' && q[*i] != ':') {
      (*i)++;
    }
    t->p = q + s;
    t->len = *i - s;
    t->quoted = false;
    t->colon = false;
    return true;
  }
  return false;
}

static kbc_status query_since_ns(const char *q, int64_t *ns, kbc_err *err) {
  *ns = 0;
  const size_t len = strlen(q);
  size_t i = 0;
  /* Four tokens of look-behind: `NOT` `since` `:` `value` is the longest
   * shape that matters, and the negation is the one thing that must be seen
   * before the atom is acted on. */
  app_tok ring[5];
  memset(ring, 0, sizeof(ring));
  size_t filled = 0;
  app_tok t;
  while (app_next_tok(q, len, &i, &t)) {
    const size_t slot = filled % 5;
    /* ring[slot-1] is the most recent previous token, so the pattern reads
     * backwards: `:` , `since` , whatever came before it. */
    const app_tok *p1 = &ring[(slot + 2) % 5]; /* the token before `since` */
    const app_tok *p2 = &ring[(slot + 3) % 5]; /* `since` */
    const app_tok *p3 = &ring[(slot + 4) % 5]; /* `:` */
    const bool shape = filled >= 3 && !p1->quoted && !p2->quoted && p2->len == 5 &&
                       strncasecmp(p2->p, "since", 5) == 0 && p3->colon &&
                       !t.colon && t.len > 0;
    const bool negated = shape && !p1->quoted && p1->len > 0 &&
                         (strncasecmp(p1->p, "not", 3) == 0 ||
                          strncasecmp(p1->p, "!", 1) == 0);
    if (shape && !negated) {
      char value[KBC_MAX_QUERY_LEN + 1];
      if (t.len >= sizeof(value))
        return kbc_err_set(err, KBC_ERR_INVALID, "since: value is too long");
      memcpy(value, t.p, t.len);
      value[t.len] = '\0';
      int64_t one = 0;
      kbc_status st = kbc_since_value_ns(value, &one, err);
      if (st != KBC_OK) return st;
      if (one > *ns) *ns = one;
    }
    ring[slot] = t;
    filled++;
  }
  return KBC_OK;
}

/* The corpus link graph, in the searcher's doc-id space.
 *
 * The store names documents by PATH and cannot name them by index doc id, so
 * the walk starts on the index side: the doc table is a flat array, and one
 * pass over it gives every path this corpus has, in doc-id order. Those paths
 * go to the store in ONE statement, and the degrees come back parallel to the
 * array — no positional join against the corpus's artifact order, and no
 * per-document store lookup. Only documents with a non-zero in-degree are
 * carried across; the rest cannot change an ordering.
 *
 * KBC_OWN out: free with free(). *ids_out / *deg_out are NULL when the corpus
 * has no linked document, which is the "no graph" case and is not an error. */
static kbc_status graph_in_degrees(kbc_app *app, const char *corpus,
                                    uint32_t **ids_out, uint32_t **deg_out,
                                    size_t *n_out, kbc_err *err) {
  *ids_out = NULL;
  *deg_out = NULL;
  *n_out = 0;
  const uint32_t n_docs = kbc_index_doc_count(app->index);
  size_t m = 0;
  for (uint32_t d = 0; d < n_docs; d++) {
    const kbc_doc_meta *meta = kbc_index_doc(app->index, d);
    if (meta != NULL && meta->corpus != NULL && meta->path != NULL &&
        strcmp(meta->corpus, corpus) == 0)
      m++;
  }
  if (m == 0) return KBC_OK;

  const char **paths = calloc(m, sizeof(*paths));
  uint32_t *doc_ids = calloc(m, sizeof(*doc_ids));
  uint32_t *deg = calloc(m, sizeof(*deg));
  if (paths == NULL || doc_ids == NULL || deg == NULL) {
    free(paths);
    free(doc_ids);
    free(deg);
    return kbc_err_set(err, KBC_ERR_NOMEM, "edge in-degrees: %zu documents", m);
  }
  size_t k = 0;
  for (uint32_t d = 0; d < n_docs; d++) {
    const kbc_doc_meta *meta = kbc_index_doc(app->index, d);
    if (meta != NULL && meta->corpus != NULL && meta->path != NULL &&
        strcmp(meta->corpus, corpus) == 0) {
      paths[k] = meta->path;
      doc_ids[k] = d;
      k++;
    }
  }

  kbc_status s = kbc_store_edge_degrees_for(app->store, corpus, paths, m, deg,
                                            err);
  free(paths);
  if (kbc_failed(s)) {
    free(doc_ids);
    free(deg);
    return s;
  }

  size_t kept = 0;
  for (size_t i = 0; i < m; i++) {
    if (deg[i] == 0) continue;
    doc_ids[kept] = doc_ids[i];
    deg[kept] = deg[i];
    kept++;
  }
  /* doc_ids was built by walking the index upward, so it is already ascending
   * and the searcher's binary search over it is valid as it stands. */
  if (kept == 0) {
    free(doc_ids);
    free(deg);
    return KBC_OK;
  }
  *ids_out = doc_ids;
  *deg_out = deg;
  *n_out = kept;
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

  /* `since:` is resolved here, not in the index: the mtime is the store's.
   * An unparsable value is an error, never a silently ignored filter. */
  int64_t since_ns = 0;
  kbc_status pre = query_since_ns(q->q, &since_ns, err);
  if (kbc_failed(pre)) return pre;
  if (q->since_ns > since_ns) since_ns = q->since_ns;
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

  /* The searcher is handed no resolver: the rows come back with their corpus
   * and path already copied into the caller's arena, which is everything the
   * batch needs. Resolving here, after the index lock is released, is what
   * lets one query take the store's lock once. */

  kbc_err local;
  kbc_err_reset(&local);
  kbc_searcher *s = kbc_searcher_new(app->index, NULL, NULL, &local);
  if (!s) {
    if (ea) {
      kbc_arena_free(ea);
    }
    pthread_rwlock_unlock(&app->lock);
    return kbc_err_set(err, local.status ? local.status : KBC_ERR_INTERNAL,
                       "kbc_searcher_new: %s",
                       local.msg[0] ? local.msg : "failed");
  }

  /* The searcher scores the vector lane against the document embeddings, and
   * search.h has no way to learn them; search.c exports this setter and
   * documents the gap. Without this call the lane never runs and every
   * semantic query degrades to an empty result. app->vec is BORROWED for the
   * search, and the read lock held here is what keeps it alive. */
  (void)kbc_searcher_set_vecstore(s, app->vec, &local);

  /* The effective query: the caller's, plus the graph the store knows about.
   * The weight comes from the config, read once per search, and 0.0 means the
   * graph is off — so nothing is fetched and every score is byte-identical to
   * a build with no edge table. (0, 4] is enforced by the config layer. */
  const double weight = app->cfg != NULL ? app->cfg->graph_boost : 0.0;
  kbc_query eq = *q;
  uint32_t *deg_ids = NULL;
  uint32_t *deg_vals = NULL;
  size_t deg_len = 0;
  if (weight > 0.0 && q->corpus != NULL) {
    kbc_err gerr;
    kbc_err_reset(&gerr);
    if (kbc_failed(graph_in_degrees(app, q->corpus, &deg_ids, &deg_vals,
                                    &deg_len, &gerr))) {
      KBC_LOGW("search: graph boost off for this query: %s", gerr.msg);
      deg_ids = NULL;
      deg_vals = NULL;
      deg_len = 0;
    } else {
      eq.graph_boost_weight = weight;
      eq.in_deg_doc_ids = deg_ids;
      eq.in_deg = deg_vals;
      eq.in_deg_len = deg_len;
      KBC_LOGD("search: graph boost weight %.3f over %zu linked documents",
               weight, deg_len);
    }
  }

  kbc_status rc_st = kbc_search_run(s, a, &eq, vec, vec_len, out, err);
  kbc_searcher_free(s);
  free(deg_ids);
  free(deg_vals);
  if (ea) {
    kbc_arena_free(ea);
  }
  pthread_rwlock_unlock(&app->lock);

  if (kbc_failed(rc_st)) {
    return rc_st;
  }

  kbc_status rs = resolve_rows(app, a, out, since_ns, err);
  if (kbc_failed(rs)) {
    return rs;
  }

  out->took_us = (kbc_now_ns() - t0) / 1000;
  /* The searcher knows what was asked for and what ran; degraded means a lane
   * the caller wanted could not run, which is not the same as "this build has
   * no embedder". A mode=keyword query never wanted the vector lane. */
  if (out->degraded) {
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
  /* The event contract: `corpus` names the corpus, `path` is CORPUS-RELATIVE.
   * Everything below — the store, the index and the touch/remove path — is
   * keyed on exactly that, so the root is joined here and nowhere else. An
   * absolute path in the event would key every lookup on a path that does not
   * exist, and an edit would read as a removal. */
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
