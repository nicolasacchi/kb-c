/* test_store.c — src/store.c: SQLite persistence for artifacts, chunks and
 * comments. Every case runs against its own store in its own tmpdir. */

#include "kbc_test.h"

#include <pthread.h>
#include <sqlite3.h>

#include "kbc/store.h"
#include "kbc/types.h"

/* The version the ladder in src/store.c tops out at. Pinned here so a
 * migration that bumps it has to be a deliberate edit in both places: a
 * binary that migrates past what its tests know about is the failure this
 * pin exists to make loud. */
#define CURRENT_SCHEMA 11

/* include/kbc/store.h is the orchestrator's file; the pending-links contract
 * is proposed there and these are the signatures it will carry. */
kbc_status kbc_store_add_pending_links(kbc_store *s, const char *corpus,
                                       const char *src_path,
                                       const char *const *dst_paths, size_t n,
                                       kbc_err *err);
kbc_status kbc_store_drain_pending(kbc_store *s, const char *corpus,
                                   const char *dst_path, kbc_err *err);
kbc_status kbc_store_delete_pending(kbc_store *s, const char *corpus,
                                    const char *src_path, kbc_err *err);

int64_t kbc_store_pending_count(kbc_store *s, kbc_err *err);

/* Same CONTRACT GAP as the pending-links block above: the facet API the
 * overlay needs, on the terms the header will carry. */
kbc_status kbc_store_replace_metas(kbc_store *s, const char *corpus,
                                   const char *path,
                                   const char *const *keys,
                                   const char *const *values, size_t n,
                                   kbc_err *err);
kbc_status kbc_store_forget_metas(kbc_store *s, const char *corpus,
                                  const char *path, kbc_err *err);
kbc_status kbc_store_docs_with_meta(kbc_store *s, const char *corpus,
                                    const char *key, const char *value,
                                    char ***paths_out, size_t *n_out,
                                    kbc_err *err);

/* ------------------------------------------------------------- helpers --- */

/* kbc_store_open reads exactly one field of the config — db_path — so the
 * tests hand it a stack config and own the string themselves. */
static void cfg_for(kbc_config *cfg, const char *root, const char *db) {
  memset(cfg, 0, sizeof(*cfg));
  size_t n = strlen(root) + strlen(db) + 2u;
  cfg->db_path = malloc(n);
  if (cfg->db_path == NULL) {
    return;
  }
  (void)snprintf(cfg->db_path, n, "%s/%s", root, db);
}

static kbc_store *open_at(const char *root, const char *db, kbc_err *err) {
  kbc_config cfg;
  cfg_for(&cfg, root, db);
  if (cfg.db_path == NULL) {
    (void)kbc_err_set(err, KBC_ERR_NOMEM, "config");
    return NULL;
  }
  kbc_store *s = kbc_store_open(&cfg, err);
  free(cfg.db_path);
  return s;
}

static void fill(kbc_artifact *a, const char *id, const char *corpus,
                 const char *path, kbc_kind kind) {
  memset(a, 0, sizeof(*a));
  a->id = id;
  a->corpus = corpus;
  a->path = path;
  a->title = "Some Title";
  a->kind = kind;
  a->mtime_ns = 1700000000123456789ll;
  a->size_bytes = 4242;
  a->content_hash = 0xdeadbeefu;
  a->heading_count = 7;
  a->summary = "First paragraph of the document.";
}

/* ------------------------------------------------- open / schema version -- */

KBC_TEST(open_creates_file_and_parents_is_idempotent) {
  char root[KBC_TEST_PATH_MAX];
  kbc_test_tmpdir(root, sizeof root);
  char db_path[KBC_TEST_PATH_MAX];
  const char *sub = "/nested/deeper/kb.db";
  size_t rl = strlen(root);
  memcpy(db_path, root, rl);
  memcpy(db_path + rl, sub, strlen(sub) + 1u);
  KBC_CHECK(!kbc_path_exists(db_path));

  kbc_err err;
  kbc_err_reset(&err);
  kbc_store *s = open_at(root, "nested/deeper/kb.db", &err);
  KBC_CHECK_MSG(s != NULL, "open failed: %s", err.msg);
  if (s == NULL) {
    kbc_test_rmrf(root);
    return;
  }
  KBC_CHECK(kbc_path_exists(db_path));
  KBC_CHECK_EQ_INT(kbc_store_schema_version(s), CURRENT_SCHEMA);
  kbc_store_close(s);

  /* Reopening an existing store neither fails nor re-runs the migration. */
  kbc_err_reset(&err);
  s = open_at(root, "nested/deeper/kb.db", &err);
  KBC_CHECK_MSG(s != NULL, "reopen failed: %s", err.msg);
  if (s != NULL) {
    KBC_CHECK_EQ_INT(kbc_store_schema_version(s), CURRENT_SCHEMA);
    kbc_store_close(s);
  }
  kbc_test_rmrf(root);
}

KBC_TEST(schema_version_of_null_store_is_zero) {
  KBC_CHECK_EQ_INT(kbc_store_schema_version(NULL), 0);
  kbc_store_close(NULL); /* must not crash */
}

/* ------------------------------------------------------------ round-trip -- */

KBC_TEST(get_artifact_round_trips_every_field) {
  char root[KBC_TEST_PATH_MAX];
  kbc_test_tmpdir(root, sizeof root);
  kbc_err err;
  kbc_err_reset(&err);
  kbc_store *s = open_at(root, "kb.db", &err);
  KBC_CHECK_MSG(s != NULL, "open: %s", err.msg);
  if (s == NULL) {
    kbc_test_rmrf(root);
    return;
  }

  kbc_artifact in;
  fill(&in, "aaaaaaaaaaaa", "kb", "notes/rfc.md", KBC_KIND_NOTE);
  in.source = "# RFC\n\nBody text of the document, stored verbatim.";
  KBC_CHECK_OK(kbc_store_upsert_artifact(s, &in, &err));

  kbc_arena *a = kbc_arena_new(0);
  kbc_artifact got;
  KBC_CHECK_OK(kbc_store_get_artifact(s, a, "aaaaaaaaaaaa", true, &got, &err));
  KBC_CHECK_EQ_STR(got.id, "aaaaaaaaaaaa");
  KBC_CHECK_EQ_STR(got.corpus, "kb");
  KBC_CHECK_EQ_STR(got.path, "notes/rfc.md");
  KBC_CHECK_EQ_STR(got.title, "Some Title");
  KBC_CHECK_EQ_INT(got.kind, KBC_KIND_NOTE);
  KBC_CHECK_EQ_INT(got.mtime_ns, in.mtime_ns);
  KBC_CHECK_EQ_INT(got.size_bytes, 4242);
  KBC_CHECK_EQ_INT(got.content_hash, (long long)0xdeadbeefu);
  KBC_CHECK_EQ_INT(got.heading_count, 7);
  KBC_CHECK_EQ_STR(got.summary, "First paragraph of the document.");
  KBC_CHECK_NOT_NULL(got.source);
  KBC_CHECK_MSG(strstr(got.source, "stored verbatim") != NULL,
                "source came back as \"%s\"", got.source);
  kbc_arena_free(a);

  /* with_source=false leaves ->source empty even though the row has one. */
  a = kbc_arena_new(0);
  KBC_CHECK_OK(kbc_store_get_artifact(s, a, "aaaaaaaaaaaa", false, &got, &err));
  KBC_CHECK_NULL(got.source);
  KBC_CHECK_EQ_STR(got.title, "Some Title");
  kbc_arena_free(a);

  /* Re-upserting with source = NULL clears the stored source; asking for it
   * again must report "absent", not a stale copy. */
  in.source = NULL;
  KBC_CHECK_OK(kbc_store_upsert_artifact(s, &in, &err));
  a = kbc_arena_new(0);
  KBC_CHECK_OK(kbc_store_get_artifact(s, a, "aaaaaaaaaaaa", true, &got, &err));
  KBC_CHECK_NULL(got.source);
  kbc_arena_free(a);

  kbc_store_close(s);
  kbc_test_rmrf(root);
}

KBC_TEST(get_artifact_by_path_resolves_within_a_corpus) {
  char root[KBC_TEST_PATH_MAX];
  kbc_test_tmpdir(root, sizeof root);
  kbc_err err;
  kbc_store *s = open_at(root, "kb.db", &err);
  KBC_CHECK_MSG(s != NULL, "open: %s", err.msg);
  if (s == NULL) {
    kbc_test_rmrf(root);
    return;
  }

  kbc_artifact a;
  fill(&a, "bbbbbbbbbbbb", "kb", "a/b.md", KBC_KIND_ARTIFACT);
  KBC_CHECK_OK(kbc_store_upsert_artifact(s, &a, &err));
  /* Same relative path, different corpus: the lookup must be corpus-scoped. */
  kbc_artifact other;
  fill(&other, "cccccccccccc", "notes", "a/b.md", KBC_KIND_ARTIFACT);
  KBC_CHECK_OK(kbc_store_upsert_artifact(s, &other, &err));

  kbc_arena *ar = kbc_arena_new(0);
  kbc_artifact got;
  KBC_CHECK_OK(
      kbc_store_get_artifact_by_path(s, ar, "notes", "a/b.md", &got, &err));
  KBC_CHECK_EQ_STR(got.id, "cccccccccccc");
  KBC_CHECK_EQ_STR(got.corpus, "notes");
  /* by_path never returns the source. */
  KBC_CHECK_NULL(got.source);
  kbc_arena_free(ar);

  kbc_store_close(s);
  kbc_test_rmrf(root);
}

/* ------------------------------------------------------------- upserts --- */

KBC_TEST(upsert_updates_in_place_and_rejects_a_foreign_id) {
  char root[KBC_TEST_PATH_MAX];
  kbc_test_tmpdir(root, sizeof root);
  kbc_err err;
  kbc_store *s = open_at(root, "kb.db", &err);
  KBC_CHECK_MSG(s != NULL, "open: %s", err.msg);
  if (s == NULL) {
    kbc_test_rmrf(root);
    return;
  }

  kbc_artifact a;
  fill(&a, "dddddddddddd", "kb", "doc.md", KBC_KIND_ARTIFACT);
  KBC_CHECK_OK(kbc_store_upsert_artifact(s, &a, &err));

  a.title = "Revised Title";
  a.size_bytes = 99;
  a.heading_count = 1;
  a.summary = "Rewritten summary.";
  KBC_CHECK_OK(kbc_store_upsert_artifact(s, &a, &err));

  int64_t n = -1;
  KBC_CHECK_OK(kbc_store_count_artifacts(s, NULL, &n, &err));
  KBC_CHECK_EQ_INT(n, 1); /* updated, not duplicated */

  kbc_arena *ar = kbc_arena_new(0);
  kbc_artifact got;
  KBC_CHECK_OK(kbc_store_get_artifact(s, ar, "dddddddddddd", false, &got, &err));
  KBC_CHECK_EQ_STR(got.title, "Revised Title");
  KBC_CHECK_EQ_INT(got.size_bytes, 99);
  KBC_CHECK_EQ_INT(got.heading_count, 1);
  KBC_CHECK_EQ_STR(got.summary, "Rewritten summary.");
  kbc_arena_free(ar);

  /* A different id claiming the same (corpus, path) is a real conflict, and
   * the stored row must be untouched by the failed attempt. */
  kbc_artifact clash;
  fill(&clash, "eeeeeeeeeeee", "kb", "doc.md", KBC_KIND_ARTIFACT);
  kbc_err_reset(&err);
  KBC_CHECK_ERR(kbc_store_upsert_artifact(s, &clash, &err), KBC_ERR_CONFLICT);
  KBC_CHECK_ERR_MSG(err);
  KBC_CHECK_MSG(strstr(err.msg, "doc.md") != NULL, "msg lacks the path: %s",
                err.msg);
  KBC_CHECK_MSG(strstr(err.msg, "dddddddddddd") != NULL,
                "msg lacks the incumbent id: %s", err.msg);

  KBC_CHECK_OK(kbc_store_count_artifacts(s, NULL, &n, &err));
  KBC_CHECK_EQ_INT(n, 1);
  ar = kbc_arena_new(0);
  kbc_err_reset(&err);
  KBC_CHECK_ERR(kbc_store_get_artifact(s, ar, "eeeeeeeeeeee", false, &got, &err),
                KBC_ERR_NOTFOUND);
  kbc_arena_free(ar);

  kbc_store_close(s);
  kbc_test_rmrf(root);
}

/* -------------------------------------------------------- not-found paths - */

KBC_TEST(unknown_lookups_are_notfound_and_name_the_value) {
  char root[KBC_TEST_PATH_MAX];
  kbc_test_tmpdir(root, sizeof root);
  kbc_err err;
  kbc_store *s = open_at(root, "kb.db", &err);
  KBC_CHECK_MSG(s != NULL, "open: %s", err.msg);
  if (s == NULL) {
    kbc_test_rmrf(root);
    return;
  }
  kbc_arena *ar = kbc_arena_new(0);
  kbc_artifact got;

  kbc_err_reset(&err);
  KBC_CHECK_ERR(
      kbc_store_get_artifact(s, ar, "999999999999", false, &got, &err),
      KBC_ERR_NOTFOUND);
  KBC_CHECK_ERR_MSG(err);
  KBC_CHECK_MSG(strstr(err.msg, "999999999999") != NULL,
                "get_artifact msg lacks the id: %s", err.msg);

  kbc_err_reset(&err);
  KBC_CHECK_ERR(
      kbc_store_get_artifact_by_path(s, ar, "kb", "nope/missing.md", &got, &err),
      KBC_ERR_NOTFOUND);
  KBC_CHECK_ERR_MSG(err);
  KBC_CHECK_MSG(strstr(err.msg, "nope/missing.md") != NULL,
                "get_artifact_by_path msg lacks the path: %s", err.msg);

  kbc_err_reset(&err);
  KBC_CHECK_ERR(kbc_store_delete_artifact(s, "888888888888", &err),
                KBC_ERR_NOTFOUND);
  KBC_CHECK_ERR_MSG(err);
  KBC_CHECK_MSG(strstr(err.msg, "888888888888") != NULL,
                "delete msg lacks the id: %s", err.msg);

  kbc_arena_free(ar);
  kbc_store_close(s);
  kbc_test_rmrf(root);
}

/* ---------------------------------------------------------------- list --- */

KBC_TEST(list_ids_honours_limit_offset_corpus_and_kind) {
  char root[KBC_TEST_PATH_MAX];
  kbc_test_tmpdir(root, sizeof root);
  kbc_err err;
  kbc_store *s = open_at(root, "kb.db", &err);
  KBC_CHECK_MSG(s != NULL, "open: %s", err.msg);
  if (s == NULL) {
    kbc_test_rmrf(root);
    return;
  }

  /* Read order is (corpus, path, id): kb/a..e, then notes/x,y. */
  static const char *const ids[] = {"i0000000001", "i0000000002",
                                    "i0000000003", "i0000000004",
                                    "i0000000005", "i0000000006",
                                    "i0000000007"};
  static const char *const paths[] = {"a.md", "b.md", "c.md", "d.md",
                                      "e.md", "x.md", "y.md"};
  for (size_t i = 0; i < 7; i++) {
    kbc_artifact a;
    fill(&a, ids[i], (i < 5) ? "kb" : "notes", paths[i],
         (i % 2 == 0) ? KBC_KIND_ARTIFACT : KBC_KIND_NOTE);
    KBC_CHECK_OK(kbc_store_upsert_artifact(s, &a, &err));
  }

  char **out = NULL;
  size_t n = 0;
  KBC_CHECK_OK(
      kbc_store_list_artifact_ids(s, NULL, (kbc_kind)-1, 100, 0, &out, &n, &err));
  KBC_CHECK_EQ_INT(n, 7);
  for (size_t i = 0; i < n; i++) {
    KBC_CHECK_MSG(strcmp(out[i], ids[i]) == 0, "full list [%zu] = %s, want %s",
                  i, out[i], ids[i]);
    free(out[i]);
  }
  free(out);

  /* Two adjacent pages of two: together they tile the list, without overlap. */
  char **p0 = NULL, **p1 = NULL;
  size_t n0 = 0, n1 = 0;
  KBC_CHECK_OK(
      kbc_store_list_artifact_ids(s, NULL, (kbc_kind)-1, 2, 0, &p0, &n0, &err));
  KBC_CHECK_OK(
      kbc_store_list_artifact_ids(s, NULL, (kbc_kind)-1, 2, 2, &p1, &n1, &err));
  KBC_CHECK_EQ_INT(n0, 2);
  KBC_CHECK_EQ_INT(n1, 2);
  for (size_t i = 0; i < n0; i++) {
    KBC_CHECK_EQ_STR(p0[i], ids[i]);
    for (size_t j = 0; j < n1; j++)
      KBC_CHECK_MSG(strcmp(p0[i], p1[j]) != 0,
                    "page 0 and page 1 both contain %s", p0[i]);
    free(p0[i]);
  }
  for (size_t i = 0; i < n1; i++) {
    KBC_CHECK_EQ_STR(p1[i], ids[2 + i]);
    free(p1[i]);
  }
  free(p0);
  free(p1);

  /* limit 0 returns nothing, not the world. */
  out = NULL;
  n = 42;
  KBC_CHECK_OK(
      kbc_store_list_artifact_ids(s, NULL, (kbc_kind)-1, 0, 0, &out, &n, &err));
  KBC_CHECK_EQ_INT(n, 0);
  KBC_CHECK_NULL(out);

  /* A corpus filter sees only that corpus. */
  out = NULL;
  n = 0;
  KBC_CHECK_OK(kbc_store_list_artifact_ids(s, "notes", (kbc_kind)-1, 100, 0,
                                           &out, &n, &err));
  KBC_CHECK_EQ_INT(n, 2);
  for (size_t i = 0; i < n; i++) {
    kbc_arena *ar = kbc_arena_new(0);
    kbc_artifact got;
    KBC_CHECK_OK(kbc_store_get_artifact(s, ar, out[i], false, &got, &err));
    KBC_CHECK_EQ_STR(got.corpus, "notes");
    kbc_arena_free(ar);
    free(out[i]);
  }
  free(out);

  /* A kind filter returns only that kind, across corpora. */
  out = NULL;
  n = 0;
  KBC_CHECK_OK(
      kbc_store_list_artifact_ids(s, NULL, KBC_KIND_NOTE, 100, 0, &out, &n,
                                  &err));
  KBC_CHECK_EQ_INT(n, 3); /* ids 2, 4, 6 */
  for (size_t i = 0; i < n; i++) {
    kbc_arena *ar = kbc_arena_new(0);
    kbc_artifact got;
    KBC_CHECK_OK(kbc_store_get_artifact(s, ar, out[i], false, &got, &err));
    KBC_CHECK_MSG(got.kind == KBC_KIND_NOTE, "%s has kind %d", out[i],
                  (int)got.kind);
    kbc_arena_free(ar);
    free(out[i]);
  }
  free(out);

  kbc_store_close(s);
  kbc_test_rmrf(root);
}

/* -------------------------------------------------------------- counts --- */

KBC_TEST(counts_corpora_and_total_bytes) {
  char root[KBC_TEST_PATH_MAX];
  kbc_test_tmpdir(root, sizeof root);
  kbc_err err;
  kbc_store *s = open_at(root, "kb.db", &err);
  KBC_CHECK_MSG(s != NULL, "open: %s", err.msg);
  if (s == NULL) {
    kbc_test_rmrf(root);
    return;
  }

  int64_t total = -1;
  KBC_CHECK_OK(kbc_store_total_bytes(s, &total, &err));
  KBC_CHECK_EQ_INT(total, 0);

  static const char *const ids[] = {"c0000000001", "c0000000002",
                                    "c0000000003", "c0000000004"};
  static const char *const corpora[] = {"zeta", "alpha", "zeta", "mid"};
  static const char *const paths[] = {"a.md", "b.md", "c.md", "d.md"};
  static const int64_t sizes[] = {10, 20, 30, 40};
  for (size_t i = 0; i < 4; i++) {
    kbc_artifact a;
    fill(&a, ids[i], corpora[i], paths[i], KBC_KIND_ARTIFACT);
    a.size_bytes = sizes[i];
    a.source = "some source text";
    KBC_CHECK_OK(kbc_store_upsert_artifact(s, &a, &err));
  }

  int64_t n = 0;
  KBC_CHECK_OK(kbc_store_count_artifacts(s, NULL, &n, &err));
  KBC_CHECK_EQ_INT(n, 4);
  KBC_CHECK_OK(kbc_store_count_artifacts(s, "zeta", &n, &err));
  KBC_CHECK_EQ_INT(n, 2);
  KBC_CHECK_OK(kbc_store_count_artifacts(s, "alpha", &n, &err));
  KBC_CHECK_EQ_INT(n, 1);
  KBC_CHECK_OK(kbc_store_count_artifacts(s, "absent", &n, &err));
  KBC_CHECK_EQ_INT(n, 0);

  KBC_CHECK_OK(kbc_store_total_bytes(s, &total, &err));
  KBC_CHECK_EQ_INT(total, 100);

  kbc_strlist l;
  kbc_strlist_init(&l);
  KBC_CHECK_OK(kbc_store_list_corpora(s, &l, &err));
  KBC_CHECK_EQ_INT(l.len, 3);
  if (l.len == 3) {
    KBC_CHECK_EQ_STR(l.items[0], "alpha");
    KBC_CHECK_EQ_STR(l.items[1], "mid");
    KBC_CHECK_EQ_STR(l.items[2], "zeta"); /* sorted, "zeta" de-duplicated */
  }
  kbc_strlist_free(&l);

  kbc_store_close(s);
  kbc_test_rmrf(root);
}

/* -------------------------------------------------------------- chunks --- */

KBC_TEST(replace_chunks_replaces_and_never_leaves_a_ghost_ord) {
  char root[KBC_TEST_PATH_MAX];
  kbc_test_tmpdir(root, sizeof root);
  kbc_err err;
  kbc_store *s = open_at(root, "kb.db", &err);
  KBC_CHECK_MSG(s != NULL, "open: %s", err.msg);
  if (s == NULL) {
    kbc_test_rmrf(root);
    return;
  }

  kbc_artifact a;
  fill(&a, "d0000000001", "kb", "long.md", KBC_KIND_ARTIFACT);
  KBC_CHECK_OK(kbc_store_upsert_artifact(s, &a, &err));

  kbc_block blocks[8];
  size_t n = 42;
  kbc_arena *ar = kbc_arena_new(0);
  KBC_CHECK_OK(kbc_store_list_chunks(s, ar, "d0000000001", blocks, &n, &err));
  KBC_CHECK_EQ_INT(n, 0);
  kbc_arena_free(ar);

  /* Inserted out of order on purpose: the read path must sort by ord. */
  kbc_chunk_in in[3] = {
      {"d0000000001", 2, "third", 5},
      {"d0000000001", 0, "first", 5},
      {"d0000000001", 1, "second", 6},
  };
  KBC_CHECK_OK(kbc_store_replace_chunks(s, in, 3, &err));

  ar = kbc_arena_new(0);
  KBC_CHECK_OK(kbc_store_list_chunks(s, ar, "d0000000001", blocks, &n, &err));
  KBC_CHECK_EQ_INT(n, 3);
  if (n == 3) {
    KBC_CHECK_EQ_STR(blocks[0].text, "first");
    KBC_CHECK_EQ_INT(blocks[0].text_len, 5);
    KBC_CHECK_EQ_STR(blocks[0].id, "b0");
    KBC_CHECK_EQ_STR(blocks[1].text, "second");
    KBC_CHECK_EQ_INT(blocks[1].text_len, 6);
    KBC_CHECK_EQ_STR(blocks[1].id, "b1");
    KBC_CHECK_EQ_STR(blocks[2].text, "third");
    KBC_CHECK_EQ_STR(blocks[2].id, "b2");
  }
  kbc_arena_free(ar);

  /* The document shrank: ord 1 and 2 must be gone, not merged or shadowed. */
  kbc_chunk_in one[1] = {{"d0000000001", 0, "only", 4}};
  KBC_CHECK_OK(kbc_store_replace_chunks(s, one, 1, &err));

  ar = kbc_arena_new(0);
  KBC_CHECK_OK(kbc_store_list_chunks(s, ar, "d0000000001", blocks, &n, &err));
  KBC_CHECK_EQ_INT(n, 1);
  if (n == 1) {
    KBC_CHECK_EQ_STR(blocks[0].text, "only");
    KBC_CHECK_EQ_STR(blocks[0].id, "b0");
  }
  kbc_arena_free(ar);

  /* An empty batch names no document, so it must be a no-op: it cannot be
   * allowed to wipe every document in the store. */
  KBC_CHECK_OK(kbc_store_replace_chunks(s, NULL, 0, &err));
  ar = kbc_arena_new(0);
  KBC_CHECK_OK(kbc_store_list_chunks(s, ar, "d0000000001", blocks, &n, &err));
  KBC_CHECK_EQ_INT(n, 1);
  if (n == 1) KBC_CHECK_EQ_STR(blocks[0].text, "only");
  kbc_arena_free(ar);

  kbc_store_close(s);
  kbc_test_rmrf(root);
}

KBC_TEST(replace_chunks_is_per_document) {
  char root[KBC_TEST_PATH_MAX];
  kbc_test_tmpdir(root, sizeof root);
  kbc_err err;
  kbc_store *s = open_at(root, "kb.db", &err);
  KBC_CHECK_MSG(s != NULL, "open: %s", err.msg);
  if (s == NULL) {
    kbc_test_rmrf(root);
    return;
  }

  kbc_artifact a, b;
  fill(&a, "e0000000001", "kb", "one.md", KBC_KIND_ARTIFACT);
  fill(&b, "e0000000002", "kb", "two.md", KBC_KIND_ARTIFACT);
  KBC_CHECK_OK(kbc_store_upsert_artifact(s, &a, &err));
  KBC_CHECK_OK(kbc_store_upsert_artifact(s, &b, &err));

  kbc_chunk_in one[1] = {{"e0000000001", 0, "one text", 8}};
  KBC_CHECK_OK(kbc_store_replace_chunks(s, one, 1, &err));
  kbc_chunk_in two[2] = {
      {"e0000000002", 0, "two a", 5},
      {"e0000000002", 1, "two b", 5},
  };
  KBC_CHECK_OK(kbc_store_replace_chunks(s, two, 2, &err));

  kbc_block blocks[8];
  size_t n = 0;
  kbc_arena *ar = kbc_arena_new(0);
  KBC_CHECK_OK(kbc_store_list_chunks(s, ar, "e0000000001", blocks, &n, &err));
  KBC_CHECK_EQ_INT(n, 1);
  if (n == 1) KBC_CHECK_EQ_STR(blocks[0].text, "one text");
  kbc_arena_free(ar);

  ar = kbc_arena_new(0);
  KBC_CHECK_OK(kbc_store_list_chunks(s, ar, "e0000000002", blocks, &n, &err));
  KBC_CHECK_EQ_INT(n, 2);
  if (n == 2) {
    KBC_CHECK_EQ_STR(blocks[0].text, "two a");
    KBC_CHECK_EQ_STR(blocks[1].text, "two b");
  }
  kbc_arena_free(ar);

  /* A duplicate ord inside one batch is rejected, and the failed batch leaves
   * the previously stored chunks in place. */
  kbc_chunk_in dup[2] = {
      {"e0000000001", 0, "x", 1},
      {"e0000000001", 0, "y", 1},
  };
  kbc_err_reset(&err);
  KBC_CHECK_ERR(kbc_store_replace_chunks(s, dup, 2, &err), KBC_ERR_CONFLICT);
  KBC_CHECK_ERR_MSG(err);
  KBC_CHECK_EQ_INT(err.status, KBC_ERR_CONFLICT);
  KBC_CHECK_MSG(strstr(err.msg, "e0000000001") != NULL,
                "msg lacks the doc id: %s", err.msg);
  ar = kbc_arena_new(0);
  KBC_CHECK_OK(kbc_store_list_chunks(s, ar, "e0000000001", blocks, &n, &err));
  KBC_CHECK_EQ_INT(n, 1);
  if (n == 1) KBC_CHECK_EQ_STR(blocks[0].text, "one text");
  kbc_arena_free(ar);

  kbc_store_close(s);
  kbc_test_rmrf(root);
}

/* The failure two apps over one data dir actually produce.
 *
 * `chunks.doc_id` is a foreign key onto `artifacts`, and the two writes that
 * keep a document indexed — the artifact upsert and the chunk replace — are
 * two transactions, because the app layer makes two calls. So a second writer
 * that removes the document in between leaves the first one inserting chunks
 * for a document that is no longer there. That is a FOREIGN KEY failure, and
 * reporting it as "duplicate ord" sends whoever reads the log looking for a
 * race on the ordinal that cannot exist: the delete and the insert are one
 * BEGIN IMMEDIATE, so no other connection can be inside them. */
KBC_TEST(a_chunk_write_for_a_removed_document_names_the_missing_artifact) {
  char root[KBC_TEST_PATH_MAX];
  kbc_test_tmpdir(root, sizeof root);
  kbc_err err;
  kbc_store *a = open_at(root, "kb.db", &err);
  KBC_CHECK_MSG(a != NULL, "open a: %s", err.msg);
  kbc_store *b = a ? open_at(root, "kb.db", &err) : NULL;
  KBC_CHECK_MSG(b != NULL, "open b: %s", err.msg);
  if (a == NULL || b == NULL) {
    kbc_store_close(a);
    kbc_store_close(b);
    kbc_test_rmrf(root);
    return;
  }

  kbc_artifact art;
  fill(&art, "c0000000001", "kb", "gone.md", KBC_KIND_ARTIFACT);
  KBC_CHECK_OK(kbc_store_upsert_artifact(a, &art, &err));
  kbc_chunk_in in[1] = {{"c0000000001", 0, "body", 4}};
  KBC_CHECK_OK(kbc_store_replace_chunks(a, in, 1, &err));

  /* The other store takes the document away — what a delete event, a reconcile
   * sweep or a second daemon over the same directory all end up doing. */
  KBC_CHECK_OK(kbc_store_delete_artifact(b, "c0000000001", &err));

  kbc_err_reset(&err);
  KBC_CHECK_ERR(kbc_store_replace_chunks(a, in, 1, &err), KBC_ERR_CONFLICT);
  KBC_CHECK_ERR_MSG(err);
  KBC_CHECK_MSG(strstr(err.msg, "c0000000001") != NULL,
                "msg does not name the document: %s", err.msg);
  KBC_CHECK_MSG(strstr(err.msg, "artifacts") != NULL,
                "msg does not name the table the foreign key points at: %s",
                err.msg);
  KBC_CHECK_MSG(strstr(err.msg, "duplicate ord") == NULL,
                "a missing artifact is reported as a duplicate ord: %s",
                err.msg);

  /* And it failed whole: no chunk row survives a write that could not commit. */
  kbc_block blocks[4];
  size_t n = 0;
  kbc_arena *ar = kbc_arena_new(0);
  KBC_CHECK_OK(kbc_store_list_chunks(a, ar, "c0000000001", blocks, &n, &err));
  KBC_CHECK_EQ_INT(n, 0);
  kbc_arena_free(ar);

  kbc_store_close(a);
  kbc_store_close(b);
  kbc_test_rmrf(root);
}

/* The same two stores, writing the SAME document at the same time — the shape
 * that was reported as a possible duplicate ord. It is not one, and the reason
 * is worth pinning: replace_chunks holds BEGIN IMMEDIATE across its delete and
 * its insert, so two connections are serialised by sqlite itself and the store
 * mutex never has to cross a connection boundary to keep them apart. A weaker
 * transaction here is the only thing that could produce the collision, and the
 * final row set below is what says whether it happened. */
typedef struct {
  kbc_store *s;
  const char *doc_id;
  int iters;
  int conflicts;
  int other_errors;
} chunk_racer;

static void *race_replace_chunks(void *p) {
  chunk_racer *r = p;
  kbc_chunk_in in[2] = {
      {r->doc_id, 0, "first", 5},
      {r->doc_id, 1, "second", 6},
  };
  for (int i = 0; i < r->iters; i++) {
    kbc_err err;
    kbc_err_reset(&err);
    kbc_status st = kbc_store_replace_chunks(r->s, in, 2, &err);
    if (st == KBC_OK) continue;
    if (st == KBC_ERR_CONFLICT) {
      r->conflicts++;
    } else {
      r->other_errors++;
    }
  }
  return NULL;
}

KBC_TEST(two_stores_sharing_one_db_never_collide_on_a_chunk_ord) {
  char root[KBC_TEST_PATH_MAX];
  kbc_test_tmpdir(root, sizeof root);
  kbc_err err;
  kbc_store *a = open_at(root, "kb.db", &err);
  KBC_CHECK_MSG(a != NULL, "open a: %s", err.msg);
  kbc_store *b = a ? open_at(root, "kb.db", &err) : NULL;
  KBC_CHECK_MSG(b != NULL, "open b: %s", err.msg);
  if (a == NULL || b == NULL) {
    kbc_store_close(a);
    kbc_store_close(b);
    kbc_test_rmrf(root);
    return;
  }

  kbc_artifact art;
  fill(&art, "c0000000002", "kb", "shared.md", KBC_KIND_ARTIFACT);
  KBC_CHECK_OK(kbc_store_upsert_artifact(a, &art, &err));

  chunk_racer racers[2] = {
      {a, "c0000000002", 1000, 0, 0},
      {b, "c0000000002", 1000, 0, 0},
  };
  pthread_t t[2];
  for (int i = 0; i < 2; i++) {
    KBC_CHECK_EQ_INT(pthread_create(&t[i], NULL, race_replace_chunks,
                                    &racers[i]),
                     0);
  }
  for (int i = 0; i < 2; i++) (void)pthread_join(t[i], NULL);
  KBC_CHECK_MSG(racers[0].conflicts == 0 && racers[1].conflicts == 0,
                "two connections collided on a chunk ord (%d, %d)", racers[0].conflicts,
                racers[1].conflicts);
  KBC_CHECK_MSG(racers[0].other_errors == 0 && racers[1].other_errors == 0,
                "a concurrent chunk replace failed (%d, %d)",
                racers[0].other_errors, racers[1].other_errors);

  /* One coherent document, not two stores' rows interleaved: every replace
 * deleted what the previous one wrote, so the ord set is a whole document. */
  kbc_block blocks[4];
  size_t n = 0;
  kbc_arena *ar = kbc_arena_new(0);
  KBC_CHECK_OK(kbc_store_list_chunks(b, ar, "c0000000002", blocks, &n, &err));
  KBC_CHECK_EQ_INT(n, 2);
  if (n == 2) {
    KBC_CHECK_EQ_STR(blocks[0].text, "first");
    KBC_CHECK_EQ_STR(blocks[1].text, "second");
  }
  kbc_arena_free(ar);

  kbc_store_close(a);
  kbc_store_close(b);
  kbc_test_rmrf(root);
}

/* ------------------------------------------------------------ comments --- */

/* Order within a document is (created_at, id) and two comments minted in the
 * same call can share a timestamp, so match by body, not by position. */
static int index_of_body(const kbc_comment *cs, size_t n, const char *body) {
  for (size_t i = 0; i < n; i++)
    if (strcmp(cs[i].body, body) == 0) return (int)i;
  return -1;
}

KBC_TEST(comments_list_resolve_and_foreign_key) {
  char root[KBC_TEST_PATH_MAX];
  kbc_test_tmpdir(root, sizeof root);
  kbc_err err;
  kbc_store *s = open_at(root, "kb.db", &err);
  KBC_CHECK_MSG(s != NULL, "open: %s", err.msg);
  if (s == NULL) {
    kbc_test_rmrf(root);
    return;
  }

  kbc_artifact a;
  fill(&a, "f0000000001", "kb", "c.md", KBC_KIND_ARTIFACT);
  KBC_CHECK_OK(kbc_store_upsert_artifact(s, &a, &err));

  /* A comment on an artifact that does not exist violates the foreign key. */
  kbc_err_reset(&err);
  KBC_CHECK_ERR(kbc_store_add_comment(s, "f0000000009", "b0", "ann", "hi", &err),
                KBC_ERR_CONFLICT);
  KBC_CHECK_ERR_MSG(err);
  /* err.status must agree with the status the caller is handed. */
  KBC_CHECK_EQ_INT(err.status, KBC_ERR_CONFLICT);
  KBC_CHECK_MSG(strstr(err.msg, "f0000000009") != NULL,
                "msg lacks the doc id: %s", err.msg);

  KBC_CHECK_OK(
      kbc_store_add_comment(s, "f0000000001", "section:intro", "ann", "first",
                            &err));
  KBC_CHECK_OK(
      kbc_store_add_comment(s, "f0000000001", "b0", "bo", "second", &err));

  kbc_arena *ar = kbc_arena_new(0);
  kbc_comment *cs = NULL;
  size_t n = 0;
  KBC_CHECK_OK(
      kbc_store_list_comments(s, ar, "f0000000001", 100, &cs, &n, &err));
  KBC_CHECK_EQ_INT(n, 2);
  char id0[KBC_MAX_ID_LEN + 1] = {0};
  if (n == 2) {
    KBC_CHECK(cs[0].id != NULL && kbc_id_is_valid(cs[0].id));
    KBC_CHECK_MSG(strcmp(cs[0].id, cs[1].id) != 0,
                  "two comments share the id %s", cs[0].id);
    KBC_CHECK_MSG(index_of_body(cs, n, "first") >= 0, "no \"first\" comment");
    KBC_CHECK_MSG(index_of_body(cs, n, "second") >= 0, "no \"second\" comment");
    KBC_CHECK_EQ_STR(cs[0].doc_id, "f0000000001");
    KBC_CHECK(!cs[0].resolved);
    KBC_CHECK(!cs[1].resolved);
    KBC_CHECK_NOT_NULL(cs[0].created_at);
    KBC_CHECK(strlen(cs[0].created_at) > 0);
    (void)snprintf(id0, sizeof id0, "%s", cs[0].id);
  }

  /* limit 0 lists nothing. */
  n = 42;
  KBC_CHECK_OK(
      kbc_store_list_comments(s, ar, "f0000000001", 0, &cs, &n, &err));
  KBC_CHECK_EQ_INT(n, 0);
  KBC_CHECK_NULL(cs);
  kbc_arena_free(ar);

  KBC_CHECK_OK(kbc_store_set_comment_resolved(s, id0, true, &err));

  ar = kbc_arena_new(0);
  KBC_CHECK_OK(
      kbc_store_list_comments(s, ar, "f0000000001", 100, &cs, &n, &err));
  KBC_CHECK_EQ_INT(n, 2);
  if (n == 2) {
    KBC_CHECK_MSG(cs[0].resolved, "comment %s not resolved", cs[0].id);
    KBC_CHECK_MSG(!cs[1].resolved, "comment %s resolved by mistake", cs[1].id);
  }
  kbc_arena_free(ar);

  /* And back to unresolved. */
  KBC_CHECK_OK(kbc_store_set_comment_resolved(s, id0, false, &err));
  ar = kbc_arena_new(0);
  KBC_CHECK_OK(
      kbc_store_list_comments(s, ar, "f0000000001", 100, &cs, &n, &err));
  KBC_CHECK_EQ_INT(n, 2);
  if (n == 2) KBC_CHECK(!cs[0].resolved);
  kbc_arena_free(ar);

  kbc_err_reset(&err);
  KBC_CHECK_ERR(kbc_store_set_comment_resolved(s, "000000000000", true, &err),
                KBC_ERR_NOTFOUND);
  KBC_CHECK_ERR_MSG(err);
  KBC_CHECK_MSG(strstr(err.msg, "000000000000") != NULL,
                "msg lacks the comment id: %s", err.msg);

  kbc_store_close(s);
  kbc_test_rmrf(root);
}

/* ------------------------------------------------------------- cascade --- */

KBC_TEST(delete_cascades_to_chunks_and_comments) {
  char root[KBC_TEST_PATH_MAX];
  kbc_test_tmpdir(root, sizeof root);
  kbc_err err;
  kbc_store *s = open_at(root, "kb.db", &err);
  KBC_CHECK_MSG(s != NULL, "open: %s", err.msg);
  if (s == NULL) {
    kbc_test_rmrf(root);
    return;
  }

  kbc_artifact a;
  fill(&a, "99900000001", "kb", "gone.md", KBC_KIND_ARTIFACT);
  KBC_CHECK_OK(kbc_store_upsert_artifact(s, &a, &err));
  kbc_chunk_in ch[2] = {
      {"99900000001", 0, "alpha", 5},
      {"99900000001", 1, "beta", 4},
  };
  KBC_CHECK_OK(kbc_store_replace_chunks(s, ch, 2, &err));
  KBC_CHECK_OK(
      kbc_store_add_comment(s, "99900000001", "b0", "ann", "note", &err));

  kbc_block blocks[4];
  size_t n = 0;
  kbc_arena *ar = kbc_arena_new(0);
  KBC_CHECK_OK(kbc_store_list_chunks(s, ar, "99900000001", blocks, &n, &err));
  KBC_CHECK_EQ_INT(n, 2);
  kbc_arena_free(ar);

  KBC_CHECK_OK(kbc_store_delete_artifact(s, "99900000001", &err));

  kbc_artifact got;
  ar = kbc_arena_new(0);
  kbc_err_reset(&err);
  KBC_CHECK_ERR(kbc_store_get_artifact(s, ar, "99900000001", false, &got, &err),
                KBC_ERR_NOTFOUND);
  kbc_arena_free(ar);

  /* foreign_keys=ON: the children went with the parent, so these reads are
   * empty rather than dangling rows. */
  ar = kbc_arena_new(0);
  KBC_CHECK_OK(kbc_store_list_chunks(s, ar, "99900000001", blocks, &n, &err));
  KBC_CHECK_EQ_INT(n, 0);
  kbc_arena_free(ar);

  kbc_comment *cs = NULL;
  ar = kbc_arena_new(0);
  KBC_CHECK_OK(
      kbc_store_list_comments(s, ar, "99900000001", 100, &cs, &n, &err));
  KBC_CHECK_EQ_INT(n, 0);
  kbc_arena_free(ar);

  /* The slot is reusable: a new artifact may take the freed (corpus, path). */
  kbc_artifact again;
  fill(&again, "a0000000001", "kb", "gone.md", KBC_KIND_ARTIFACT);
  KBC_CHECK_OK(kbc_store_upsert_artifact(s, &again, &err));

  kbc_store_close(s);
  kbc_test_rmrf(root);
}

/* ------------------------------------------------------------ bad input -- */

KBC_TEST(null_and_empty_arguments_are_invalid) {
  char root[KBC_TEST_PATH_MAX];
  kbc_test_tmpdir(root, sizeof root);
  kbc_err err;
  kbc_store *s = open_at(root, "kb.db", &err);
  KBC_CHECK_MSG(s != NULL, "open: %s", err.msg);
  if (s == NULL) {
    kbc_test_rmrf(root);
    return;
  }
  kbc_arena *ar = kbc_arena_new(0);
  kbc_artifact got;
  kbc_artifact in;
  int64_t n = 0;

  kbc_err_reset(&err);
  KBC_CHECK_ERR(kbc_store_get_artifact(s, ar, NULL, false, &got, &err),
                KBC_ERR_INVALID);
  KBC_CHECK_ERR_MSG(err);
  kbc_err_reset(&err);
  KBC_CHECK_ERR(kbc_store_get_artifact(s, ar, "", false, &got, &err),
                KBC_ERR_INVALID);

  kbc_err_reset(&err);
  KBC_CHECK_ERR(
      kbc_store_get_artifact_by_path(s, ar, NULL, "a.md", &got, &err),
      KBC_ERR_INVALID);
  kbc_err_reset(&err);
  KBC_CHECK_ERR(kbc_store_get_artifact_by_path(s, ar, "kb", NULL, &got, &err),
                KBC_ERR_INVALID);
  kbc_err_reset(&err);
  KBC_CHECK_ERR(kbc_store_get_artifact_by_path(s, ar, "kb", "", &got, &err),
                KBC_ERR_INVALID);

  kbc_err_reset(&err);
  KBC_CHECK_ERR(kbc_store_delete_artifact(s, NULL, &err), KBC_ERR_INVALID);

  kbc_err_reset(&err);
  KBC_CHECK_ERR(kbc_store_count_artifacts(s, "", &n, &err), KBC_ERR_INVALID);
  kbc_err_reset(&err);
  KBC_CHECK_ERR(kbc_store_count_artifacts(s, NULL, NULL, &err),
                KBC_ERR_INVALID);

  fill(&in, NULL, "kb", "a.md", KBC_KIND_ARTIFACT);
  kbc_err_reset(&err);
  KBC_CHECK_ERR(kbc_store_upsert_artifact(s, &in, &err), KBC_ERR_INVALID);
  KBC_CHECK_ERR_MSG(err);
  fill(&in, "abababababab", NULL, "a.md", KBC_KIND_ARTIFACT);
  kbc_err_reset(&err);
  KBC_CHECK_ERR(kbc_store_upsert_artifact(s, &in, &err), KBC_ERR_INVALID);
  fill(&in, "abababababab", "kb", "a.md", KBC_KIND_ARTIFACT);
  in.kind = (kbc_kind)KBC_KIND__COUNT;
  kbc_err_reset(&err);
  KBC_CHECK_ERR(kbc_store_upsert_artifact(s, &in, &err), KBC_ERR_INVALID);
  fill(&in, "abababababab", "kb", "a.md", KBC_KIND_ARTIFACT);
  in.size_bytes = -1;
  kbc_err_reset(&err);
  KBC_CHECK_ERR(kbc_store_upsert_artifact(s, &in, &err), KBC_ERR_INVALID);
  fill(&in, "abababababab", "kb", "a.md", KBC_KIND_ARTIFACT);
  in.title = NULL;
  kbc_err_reset(&err);
  KBC_CHECK_ERR(kbc_store_upsert_artifact(s, &in, &err), KBC_ERR_INVALID);

  /* None of the rejected upserts may have landed. */
  kbc_err_reset(&err);
  KBC_CHECK_OK(kbc_store_count_artifacts(s, NULL, &n, &err));
  KBC_CHECK_EQ_INT(n, 0);

  kbc_arena_free(ar);
  kbc_store_close(s);
  kbc_test_rmrf(root);
}

KBC_TEST(open_rejects_an_unusable_db_path) {
  char root[KBC_TEST_PATH_MAX];
  kbc_test_tmpdir(root, sizeof root);
  kbc_err err;

  kbc_err_reset(&err);
  KBC_CHECK_NULL(kbc_store_open(NULL, &err));
  KBC_CHECK_ERR_MSG(err);
  KBC_CHECK_EQ_INT(err.status, KBC_ERR_INVALID);

  kbc_config cfg;
  cfg_for(&cfg, root, "kb.db");
  KBC_CHECK_NOT_NULL(cfg.db_path);
  if (cfg.db_path != NULL) {
    /* A usable path on the other hand opens, and the caller owns it. */
    kbc_err_reset(&err);
    kbc_store *ok = kbc_store_open(&cfg, &err);
    KBC_CHECK_MSG(ok != NULL, "open: %s", err.msg);
    kbc_store_close(ok);

    free(cfg.db_path);
    cfg.db_path = strdup("");
    kbc_err_reset(&err);
    KBC_CHECK_NULL(kbc_store_open(&cfg, &err));
    KBC_CHECK_ERR_MSG(err);
    free(cfg.db_path);
  }
  kbc_test_rmrf(root);
}

/* ------------------------------------------------- batch resolve -------- */

/* 20 KB of source: comfortably past SQLite's page, so it lands in the
 * overflow chain of the record. A read that comes back byte for byte is the
 * evidence that the resolve path stays off it without truncating anything. */
static char *big_source(size_t n) {
  char *p = malloc(n + 1u);
  if (p == NULL) return NULL;
  for (size_t i = 0; i < n; i++) p[i] = (char)('a' + (int)(i % 26u));
  p[n] = '\0';
  return p;
}

/* The batch exists so a search resolves its top-k in one locked round trip,
 * and it reads the row WITHOUT `source` — that column is ~12.5 KB of a ~12.8
 * KB record and nothing on the search path wants it. The batch must therefore
 * return exactly what the per-row read returns, field for field, or the
 * search response changes shape without anybody noticing. */
KBC_TEST(batch_resolve_agrees_with_the_per_row_read) {
  char root[KBC_TEST_PATH_MAX];
  kbc_test_tmpdir(root, sizeof root);
  kbc_err err;
  kbc_store *s = open_at(root, "kb.db", &err);
  KBC_CHECK_MSG(s != NULL, "open: %s", err.msg);
  if (s == NULL) {
    kbc_test_rmrf(root);
    return;
  }

  size_t src_len = 20000u;
  char *src = big_source(src_len);
  KBC_CHECK_NOT_NULL(src);
  if (src == NULL) {
    kbc_store_close(s);
    kbc_test_rmrf(root);
    return;
  }

  kbc_artifact a;
  fill(&a, "111111111111", "kb", "one.md", KBC_KIND_NOTE);
  a.title = "One";
  a.summary = "summary of one";
  a.source = src;
  KBC_CHECK_OK(kbc_store_upsert_artifact(s, &a, &err));
  fill(&a, "222222222222", "kb", "two.md", KBC_KIND_SESSION);
  a.title = "Two";
  a.summary = "summary of two";
  KBC_CHECK_OK(kbc_store_upsert_artifact(s, &a, &err));
  /* Same path in another corpus: a resolve must stay corpus-scoped. */
  fill(&a, "333333333333", "notes", "one.md", KBC_KIND_NOTE);
  a.title = "One (notes)";
  a.summary = "summary of one, in notes";
  KBC_CHECK_OK(kbc_store_upsert_artifact(s, &a, &err));

  const char *corpora[] = {"kb", "notes", "kb", "kb", "kb", "nosuch", "kb"};
  const char *paths[] = {"one.md", "one.md", "two.md", "missing.md", "one.md",
                         "one.md", "one.md"};
  const size_t n = sizeof paths / sizeof paths[0];

  kbc_arena *ar = kbc_arena_new(0);
  kbc_artifact **slots = NULL;
  KBC_CHECK_OK(
      kbc_store_get_artifacts_by_path(s, ar, corpora, paths, n, &slots, &err));
  KBC_CHECK_NOT_NULL(slots);
  KBC_CHECK_MSG(slots != NULL, "batch: %s", err.msg);
  if (slots != NULL) {
    for (size_t i = 0; i < n; i++) {
      const bool should_hit = (i != 3u && i != 5u);
      if (!should_hit) {
        KBC_CHECK_MSG(slots[i] == NULL, "slot %zu: expected no row", i);
        continue;
      }
      KBC_CHECK_MSG(slots[i] != NULL, "slot %zu: expected a row", i);
      if (slots[i] == NULL) continue;

      kbc_arena *one = kbc_arena_new(0);
      kbc_artifact want;
      KBC_CHECK_OK(kbc_store_get_artifact_by_path(s, one, corpora[i], paths[i],
                                                  &want, &err));
      const kbc_artifact *got = slots[i];
      KBC_CHECK_MSG(strcmp(got->id, want.id) == 0, "slot %zu id: %s vs %s", i,
                    got->id, want.id);
      KBC_CHECK_MSG(strcmp(got->corpus, want.corpus) == 0, "slot %zu corpus", i);
      KBC_CHECK_MSG(strcmp(got->path, want.path) == 0, "slot %zu path", i);
      KBC_CHECK_MSG(strcmp(got->title, want.title) == 0, "slot %zu title: %s",
                    i, got->title);
      KBC_CHECK_MSG(strcmp(got->summary, want.summary) == 0,
                    "slot %zu summary: %s", i, got->summary);
      KBC_CHECK_EQ_INT(got->kind, want.kind);
      KBC_CHECK_EQ_INT(got->mtime_ns, want.mtime_ns);
      KBC_CHECK_EQ_INT(got->size_bytes, want.size_bytes);
      KBC_CHECK_EQ_INT(got->content_hash, want.content_hash);
      KBC_CHECK_EQ_INT(got->heading_count, want.heading_count);
      KBC_CHECK_NULL(got->source);
      kbc_arena_free(one);
    }
    /* The duplicate pair is one row fetched once, and both slots name it. */
    KBC_CHECK_NOT_NULL(slots[0]);
    KBC_CHECK_NOT_NULL(slots[4]);
    KBC_CHECK_NOT_NULL(slots[6]);
    if (slots[0] != NULL && slots[4] != NULL) {
      KBC_CHECK_MSG(strcmp(slots[0]->id, slots[4]->id) == 0,
                    "duplicate pair resolved differently");
    }
    free(slots);
  }
  kbc_arena_free(ar);

  /* An unlookupable pair in the middle of the batch. It resolves to no row
   * AND it must not shift which document the pairs after it look up: the
   * de-duplication used to index the caller's arrays by a compacted counter,
   * so a skipped pair made every later pair compare against — and read — the
   * wrong slots. Slot 1 below is the pair that exposed it. */
  const char *shift_corpora[] = {"kb", "", "kb", "kb", NULL};
  const char *shift_paths[] = {"two.md", "ignored.md", "one.md", "two.md", "x"};
  kbc_arena *sa = kbc_arena_new(0);
  kbc_artifact **sslots = NULL;
  KBC_CHECK_OK(kbc_store_get_artifacts_by_path(s, sa, shift_corpora,
                                               shift_paths, 5, &sslots, &err));
  KBC_CHECK_NOT_NULL(sslots);
  if (sslots != NULL) {
    KBC_CHECK_MSG(sslots[0] != NULL, "slot 0 (kb/two.md) went missing");
    KBC_CHECK_MSG(sslots[1] == NULL, "slot 1 (empty corpus) resolved");
    KBC_CHECK_MSG(sslots[2] != NULL, "slot 2 (kb/one.md) went missing");
    KBC_CHECK_MSG(sslots[3] != NULL, "slot 3 (duplicate of slot 0) went missing");
    KBC_CHECK_MSG(sslots[4] == NULL, "slot 4 (NULL corpus) resolved");
    if (sslots[0] != NULL)
      KBC_CHECK_MSG(strcmp(sslots[0]->id, "222222222222") == 0,
                    "slot 0 resolved %s, expected 222222222222", sslots[0]->id);
    if (sslots[2] != NULL)
      KBC_CHECK_MSG(strcmp(sslots[2]->id, "111111111111") == 0,
                    "slot 2 resolved %s, expected 111111111111", sslots[2]->id);
    if (sslots[0] != NULL && sslots[3] != NULL)
      KBC_CHECK_MSG(strcmp(sslots[0]->id, sslots[3]->id) == 0,
                    "the duplicate pair resolved to a different document");
    free(sslots);
  }
  kbc_arena_free(sa);

  /* n == 0 is a successful no-op, not an error and not an empty array. */
  kbc_arena *empty = kbc_arena_new(0);
  kbc_artifact **none = (kbc_artifact **)(void *)ar;
  KBC_CHECK_OK(kbc_store_get_artifacts_by_path(s, empty, corpora, paths, 0,
                                               &none, &err));
  KBC_CHECK_NULL(none);
  kbc_arena_free(empty);

  kbc_store_close(s);
  free(src);
  kbc_test_rmrf(root);
}

/* The wide read is a different call from the resolve and must stay byte for
 * byte exact: `kb get <id> --source` and the artifact route depend on it. */
KBC_TEST(with_source_still_returns_the_whole_text) {
  char root[KBC_TEST_PATH_MAX];
  kbc_test_tmpdir(root, sizeof root);
  kbc_err err;
  kbc_store *s = open_at(root, "kb.db", &err);
  KBC_CHECK_MSG(s != NULL, "open: %s", err.msg);
  if (s == NULL) {
    kbc_test_rmrf(root);
    return;
  }

  size_t src_len = 20000u;
  char *src = big_source(src_len);
  KBC_CHECK_NOT_NULL(src);
  if (src == NULL) {
    kbc_store_close(s);
    kbc_test_rmrf(root);
    return;
  }
  kbc_artifact a;
  fill(&a, "444444444444", "kb", "wide.md", KBC_KIND_ARTIFACT);
  a.source = src;
  KBC_CHECK_OK(kbc_store_upsert_artifact(s, &a, &err));

  /* The batch resolve ran against the same store first: reading the row
   * without `source` must not have damaged the wide read that follows. */
  const char *corpora[] = {"kb"};
  const char *paths[] = {"wide.md"};
  kbc_arena *ar = kbc_arena_new(0);
  kbc_artifact **slots = NULL;
  KBC_CHECK_OK(
      kbc_store_get_artifacts_by_path(s, ar, corpora, paths, 1, &slots, &err));
  KBC_CHECK_NOT_NULL(slots);
  if (slots != NULL) {
    KBC_CHECK_NULL(slots[0]->source);
    free(slots);
  }
  kbc_arena_free(ar);

  kbc_arena *wide = kbc_arena_new(0);
  kbc_artifact got;
  KBC_CHECK_OK(kbc_store_get_artifact(s, wide, "444444444444", true, &got, &err));
  KBC_CHECK_NOT_NULL(got.source);
  if (got.source != NULL) {
    KBC_CHECK_MSG(strlen(got.source) == src_len, "source length %zu != %zu",
                  strlen(got.source), src_len);
    KBC_CHECK_MSG(strcmp(got.source, src) == 0, "source came back changed");
  }
  /* with_source=false on the same row: the ten small columns, no source. */
  kbc_arena_free(wide);
  wide = kbc_arena_new(0);
  KBC_CHECK_OK(kbc_store_get_artifact(s, wide, "444444444444", false, &got, &err));
  KBC_CHECK_NULL(got.source);
  KBC_CHECK_EQ_STR(got.summary, "First paragraph of the document.");
  kbc_arena_free(wide);

  kbc_store_close(s);
  free(src);
  kbc_test_rmrf(root);
}


/* ------------------------------------------------------------------ edges -- */

/* The edges table is REPLACED, never appended: a re-indexed document whose
 * links changed must not leave the previous edges behind, and a document that
 * lost every link must have none. Both are the same guarantee seen from two
 * sides, and both are what a lookup-by-src would get wrong. */
KBC_TEST(edges_replace_twice_leaves_no_ghost) {
  char root[KBC_TEST_PATH_MAX];
  kbc_test_tmpdir(root, sizeof root);
  kbc_err err;
  kbc_err_reset(&err);
  kbc_store *s = open_at(root, "kb.db", &err);
  KBC_CHECK_NOT_NULL(s);

  /* The destinations have to be documents: the in-degree is reported per
   * indexed document, so an edge to a path the store does not know is an edge
   * with nothing to attach a degree to. */
  kbc_artifact a;
  fill(&a, "aaaaaaaaaaaa", "kb", "a.md", KBC_KIND_ARTIFACT);
  KBC_CHECK_OK(kbc_store_upsert_artifact(s, &a, &err));
  fill(&a, "bbbbbbbbbbbb", "kb", "b.md", KBC_KIND_ARTIFACT);
  KBC_CHECK_OK(kbc_store_upsert_artifact(s, &a, &err));
  fill(&a, "cccccccccccc", "kb", "c.md", KBC_KIND_ARTIFACT);
  KBC_CHECK_OK(kbc_store_upsert_artifact(s, &a, &err));

  static const char *const first[] = {"a.md", "b.md"};
  static const char *const second[] = {"c.md"};
  KBC_CHECK_OK(kbc_store_replace_edges(s, "kb", "src.md", first, 2, &err));
  KBC_CHECK_EQ_INT(kbc_store_edge_count(s, "kb", &err), 2);

  KBC_CHECK_OK(kbc_store_replace_edges(s, "kb", "src.md", second, 1, &err));
  KBC_CHECK_EQ_INT(kbc_store_edge_count(s, "kb", &err), 1);
  /* a.md and b.md are gone: the replace is a delete-then-insert inside one
   * transaction, not an upsert that merges. A path with no backlinks and a
   * path that was never indexed both read back as 0. */
  static const char *const want[] = {"a.md", "b.md", "c.md", "ghost.md"};
  uint32_t deg[4] = {9, 9, 9, 9};
  KBC_CHECK_OK(kbc_store_edge_degrees_for(s, "kb", want, 4, deg, &err));
  KBC_CHECK_EQ_INT(deg[0], 0);
  KBC_CHECK_EQ_INT(deg[1], 0);
  KBC_CHECK_EQ_INT(deg[2], 1);
  KBC_CHECK_EQ_INT(deg[3], 0);

  /* replacing with nothing is a delete, not a no-op */
  KBC_CHECK_OK(kbc_store_replace_edges(s, "kb", "src.md", NULL, 0, &err));
  KBC_CHECK_EQ_INT(kbc_store_edge_count(s, "kb", &err), 0);

  kbc_store_close(s);
  kbc_test_rmrf(root);
}

KBC_TEST(edges_delete_is_per_document) {
  char root[KBC_TEST_PATH_MAX];
  kbc_test_tmpdir(root, sizeof root);
  kbc_err err;
  kbc_err_reset(&err);
  kbc_store *s = open_at(root, "kb.db", &err);
  KBC_CHECK_NOT_NULL(s);

  static const char *const dst[] = {"t.md"};
  KBC_CHECK_OK(kbc_store_replace_edges(s, "kb", "one.md", dst, 1, &err));
  KBC_CHECK_OK(kbc_store_replace_edges(s, "kb", "two.md", dst, 1, &err));
  KBC_CHECK_OK(kbc_store_replace_edges(s, "kb", "two.md", dst, 1, &err));
  KBC_CHECK_EQ_INT(kbc_store_edge_count(s, "kb", &err), 2);
  KBC_CHECK_OK(kbc_store_delete_edges(s, "kb", "one.md", &err));
  KBC_CHECK_EQ_INT(kbc_store_edge_count(s, "kb", &err), 1);
  /* deleting a document that has no edges is not an error */
  KBC_CHECK_OK(kbc_store_delete_edges(s, "kb", "one.md", &err));
  KBC_CHECK_EQ_INT(kbc_store_edge_count(s, "kb", &err), 1);
  /* and the edges are per corpus */
  KBC_CHECK_OK(kbc_store_replace_edges(s, "other", "one.md", dst, 1, &err));
  KBC_CHECK_EQ_INT(kbc_store_edge_count(s, "kb", &err), 1);
  KBC_CHECK_EQ_INT(kbc_store_edge_count(s, "other", &err), 1);

  kbc_store_close(s);
  kbc_test_rmrf(root);
}

/* In-degree is BACKLINKS ONLY: a document that links out has in-degree 0, and
 * being linked to twice from two documents counts 2. The caller names the
 * documents it wants by path and gets the degrees back in its own array, in
 * its own order — the store never allocates and never imposes an order. */
KBC_TEST(edges_in_degrees_is_one_aggregate_over_the_graph) {
  char root[KBC_TEST_PATH_MAX];
  kbc_test_tmpdir(root, sizeof root);
  kbc_err err;
  kbc_err_reset(&err);
  kbc_store *s = open_at(root, "kb.db", &err);
  KBC_CHECK_NOT_NULL(s);

  kbc_artifact a;
  fill(&a, "aaaaaaaaaaaa", "kb", "a.md", KBC_KIND_ARTIFACT);
  KBC_CHECK_OK(kbc_store_upsert_artifact(s, &a, &err));
  fill(&a, "bbbbbbbbbbbb", "kb", "b.md", KBC_KIND_ARTIFACT);
  KBC_CHECK_OK(kbc_store_upsert_artifact(s, &a, &err));
  fill(&a, "cccccccccccc", "kb", "c.md", KBC_KIND_ARTIFACT);
  KBC_CHECK_OK(kbc_store_upsert_artifact(s, &a, &err));

  static const char *const to_a[] = {"a.md"};
  static const char *const to_b[] = {"b.md"};
  static const char *const two[] = {"a.md", "b.md"};
  KBC_CHECK_OK(kbc_store_replace_edges(s, "kb", "a.md", to_b, 1, &err));
  KBC_CHECK_OK(kbc_store_replace_edges(s, "kb", "b.md", to_a, 1, &err));
  KBC_CHECK_OK(kbc_store_replace_edges(s, "kb", "c.md", two, 2, &err));

  static const char *const want[] = {"c.md", "a.md", "b.md", "nope.md",
                                     "a.md"};
  uint32_t deg[5] = {9, 9, 9, 9, 9};
  KBC_CHECK_OK(kbc_store_edge_degrees_for(s, "kb", want, 5, deg, &err));
  KBC_CHECK_EQ_INT(deg[0], 0); /* links out, is linked to by nobody */
  KBC_CHECK_EQ_INT(deg[1], 2); /* from b.md and c.md */
  KBC_CHECK_EQ_INT(deg[2], 2); /* from a.md and c.md */
  KBC_CHECK_EQ_INT(deg[3], 0); /* no such document */
  KBC_CHECK_EQ_INT(deg[4], 2); /* a repeated path repeats its degree */

  /* n == 0 touches nothing, and another corpus's graph is not this one's */
  uint32_t untouched = 7;
  KBC_CHECK_OK(kbc_store_edge_degrees_for(s, "kb", NULL, 0, &untouched, &err));
  KBC_CHECK_EQ_INT(untouched, 7);
  KBC_CHECK_OK(kbc_store_edge_degrees_for(s, "other", want, 3, deg, &err));
  KBC_CHECK_EQ_INT(deg[1], 0);

  kbc_store_close(s);
  kbc_test_rmrf(root);
}

/* The backlinks direction, and the property that is a CONTRACT rather than a
 * convenience: nothing linking here is KBC_OK with an empty list. The original
 * serves it as a 200 with [] (links.rs:325), so a store that answered
 * NOTFOUND here would make an ordinary document that nobody happens to link
 * to look like a missing one — a 404 on a document that exists.
 *
 * The mutation this guards: returning KBC_ERR_NOTFOUND on a zero-row answer. */
KBC_TEST(backlinks_of_an_unlinked_document_is_empty_and_ok) {
  char root[KBC_TEST_PATH_MAX];
  kbc_test_tmpdir(root, sizeof root);
  kbc_err err;
  kbc_err_reset(&err);
  kbc_store *s = open_at(root, "kb.db", &err);
  KBC_CHECK_NOT_NULL(s);
  if (s == NULL) {
    kbc_test_rmrf(root);
    return;
  }

  kbc_artifact a;
  fill(&a, "aaaaaaaaaaaa", "kb", "a.md", KBC_KIND_ARTIFACT);
  KBC_CHECK_OK(kbc_store_upsert_artifact(s, &a, &err));

  /* Links OUT and is linked to by nobody: in-degree 0, and the same must be
   * true of a path that was never indexed at all. Neither is an error. */
  static const char *const away[] = {"nowhere.md"};
  KBC_CHECK_OK(kbc_store_replace_edges(s, "kb", "a.md", away, 1, &err));
  kbc_strlist out;
  kbc_strlist_init(&out);
  kbc_status st = kbc_store_list_backlinks(s, "kb", "a.md", &out, &err);
  KBC_CHECK_MSG(st == KBC_OK, "unlinked document reported %s: %s",
                kbc_status_str(st), err.msg);
  KBC_CHECK_EQ_INT((long long)out.len, 0);
  kbc_strlist_free(&out);

  kbc_strlist_init(&out);
  st = kbc_store_list_backlinks(s, "kb", "ghost.md", &out, &err);
  KBC_CHECK_MSG(st == KBC_OK, "unknown path reported %s: %s",
                kbc_status_str(st), err.msg);
  KBC_CHECK_EQ_INT((long long)out.len, 0);
  kbc_strlist_free(&out);

  kbc_store_close(s);
  kbc_test_rmrf(root);
}

/* Which documents link HERE, in the OTHER direction from edge_degrees_for:
 * that one answers "how many" for documents the caller already named, this
 * answers "which", and a backlinks surface cannot be built from a count.
 *
 * Also pins two things a naive query gets wrong. The graph is per CORPUS, so
 * another corpus's edge to the same path is not a backlink to this one's
 * document. And the answer is the set of SOURCES, so a source that links out
 * to two different targets appears once per target it points at, not twice in
 * either list. */
KBC_TEST(backlinks_name_the_sources_and_are_scoped_to_the_corpus) {
  char root[KBC_TEST_PATH_MAX];
  kbc_test_tmpdir(root, sizeof root);
  kbc_err err;
  kbc_err_reset(&err);
  kbc_store *s = open_at(root, "kb.db", &err);
  KBC_CHECK_NOT_NULL(s);
  if (s == NULL) {
    kbc_test_rmrf(root);
    return;
  }

  kbc_artifact a;
  fill(&a, "aaaaaaaaaaaa", "kb", "t.md", KBC_KIND_ARTIFACT);
  KBC_CHECK_OK(kbc_store_upsert_artifact(s, &a, &err));

  static const char *const to_t[] = {"t.md"};
  static const char *const to_t_too[] = {"t.md", "t.md"};
  KBC_CHECK_OK(kbc_store_replace_edges(s, "kb", "one.md", to_t, 1, &err));
  KBC_CHECK_OK(kbc_store_replace_edges(s, "kb", "two.md", to_t, 1, &err));
  /* Another corpus links to the same path: a different graph entirely. */
  KBC_CHECK_OK(kbc_store_replace_edges(s, "other", "three.md", to_t, 1, &err));

  kbc_strlist out;
  kbc_strlist_init(&out);
  KBC_CHECK_OK(kbc_store_list_backlinks(s, "kb", "t.md", &out, &err));
  KBC_CHECK_EQ_INT((long long)out.len, 2);
  KBC_CHECK(out.items != NULL);
  if (out.items != NULL) {
    KBC_CHECK_EQ_STR(out.items[0], "one.md");
    KBC_CHECK_EQ_STR(out.items[1], "two.md");
  }
  kbc_strlist_free(&out);

  KBC_CHECK_OK(kbc_store_list_backlinks(s, "other", "t.md", &out, &err));
  KBC_CHECK_EQ_INT((long long)out.len, 1);
  KBC_CHECK(out.items != NULL);
  if (out.items != NULL) KBC_CHECK_EQ_STR(out.items[0], "three.md");
  kbc_strlist_free(&out);

  /* Deleting one source's edges removes exactly that one backlink, and leaves
   * the other's. This is what proves the answer is read from the edges rather
   * than from the artifact list: deleting an artifact would drop both. */
  KBC_CHECK_OK(kbc_store_delete_edges(s, "kb", "one.md", &err));
  kbc_strlist_init(&out);
  KBC_CHECK_OK(kbc_store_list_backlinks(s, "kb", "t.md", &out, &err));
  KBC_CHECK_EQ_INT((long long)out.len, 1);
  KBC_CHECK(out.items != NULL);
  if (out.items != NULL) KBC_CHECK_EQ_STR(out.items[0], "two.md");
  kbc_strlist_free(&out);

  /* Two.md now links to t.md AND elsewhere: it is one backlink of t.md, not
   * two. The primary key already collapses the duplicate, so this asserts the
   * reader reports sources and not edge rows. */
  KBC_CHECK_OK(kbc_store_replace_edges(s, "kb", "two.md", to_t_too, 2, &err));
  kbc_strlist_init(&out);
  KBC_CHECK_OK(kbc_store_list_backlinks(s, "kb", "t.md", &out, &err));
  KBC_CHECK_EQ_INT((long long)out.len, 1);
  kbc_strlist_free(&out);

  kbc_store_close(s);
  kbc_test_rmrf(root);
}

/* The distinct doc_ids the comments table names — a SET, not a count, and not
 * the corpus's document list.
 *
 * The mutation this guards is the expensive one: enumerating the corpus and
 * issuing one comment query per document. At the 20,000-document corpus that
 * is 40,000 prepared statements per reindex, on the watcher's single-file
 * path, to re-check the two comments that exist. A distinct-doc_id query is
 * proportional to the comments, which is what the original pays. */
KBC_TEST(comment_docs_are_the_distinct_doc_ids_and_not_the_corpus) {
  char root[KBC_TEST_PATH_MAX];
  kbc_test_tmpdir(root, sizeof root);
  kbc_err err;
  kbc_err_reset(&err);
  kbc_store *s = open_at(root, "kb.db", &err);
  KBC_CHECK_NOT_NULL(s);
  if (s == NULL) {
    kbc_test_rmrf(root);
    return;
  }

  /* Three documents indexed, comments on only two of them, and one of those
 * has two comments — so three comment rows, two documents. */
  kbc_artifact a;
  fill(&a, "aaaaaaaaaaaa", "kb", "a.md", KBC_KIND_ARTIFACT);
  KBC_CHECK_OK(kbc_store_upsert_artifact(s, &a, &err));
  fill(&a, "bbbbbbbbbbbb", "kb", "b.md", KBC_KIND_ARTIFACT);
  KBC_CHECK_OK(kbc_store_upsert_artifact(s, &a, &err));
  fill(&a, "cccccccccccc", "kb", "c.md", KBC_KIND_ARTIFACT);
  KBC_CHECK_OK(kbc_store_upsert_artifact(s, &a, &err));

  KBC_CHECK_OK(kbc_store_add_comment(s, "aaaaaaaaaaaa", "h1", "ann",
                                     "first on a", &err));
  KBC_CHECK_OK(kbc_store_add_comment(s, "aaaaaaaaaaaa", "h2", "bob",
                                     "second on a", &err));
  KBC_CHECK_OK(kbc_store_add_comment(s, "cccccccccccc", "h1", "ann",
                                     "only on c", &err));

  kbc_strlist out;
  kbc_strlist_init(&out);
  KBC_CHECK_MSG(kbc_store_list_comment_docs(s, &out, &err) == KBC_OK,
                "list_comment_docs: %s", err.msg);
  /* TWO, not three: b.md has no comments, and a.md is named twice. */
  KBC_CHECK_EQ_INT((long long)out.len, 2);
  KBC_CHECK(out.items != NULL);
  if (out.items != NULL) {
    KBC_CHECK_EQ_STR(out.items[0], "aaaaaaaaaaaa");
    KBC_CHECK_EQ_STR(out.items[1], "cccccccccccc");
  }
  kbc_strlist_free(&out);

  /* Deleting a document cascades its comments away (foreign key, v1), and a
   * document with no comments left must STOP being named — otherwise this is
   * reading the artifact list rather than the comments table. */
  KBC_CHECK_OK(kbc_store_delete_artifact(s, "aaaaaaaaaaaa", &err));
  kbc_strlist_init(&out);
  KBC_CHECK_OK(kbc_store_list_comment_docs(s, &out, &err));
  KBC_CHECK_EQ_INT((long long)out.len, 1);
  KBC_CHECK(out.items != NULL);
  if (out.items != NULL) KBC_CHECK_EQ_STR(out.items[0], "cccccccccccc");
  kbc_strlist_free(&out);

  kbc_store_close(s);
  kbc_test_rmrf(root);
}

/* A corpus with no comments at all is an empty list and KBC_OK, for the same
 * reason backlinks is: "there is nothing here" is an answer, not a failure,
 * and a reindex pass that treated it as an error would fail on every clean
 * corpus. */
KBC_TEST(comment_docs_of_a_corpus_with_none_is_empty_and_ok) {
  char root[KBC_TEST_PATH_MAX];
  kbc_test_tmpdir(root, sizeof root);
  kbc_err err;
  kbc_err_reset(&err);
  kbc_store *s = open_at(root, "kb.db", &err);
  KBC_CHECK_NOT_NULL(s);
  if (s == NULL) {
    kbc_test_rmrf(root);
    return;
  }
  kbc_artifact a;
  fill(&a, "aaaaaaaaaaaa", "kb", "a.md", KBC_KIND_ARTIFACT);
  KBC_CHECK_OK(kbc_store_upsert_artifact(s, &a, &err));

  kbc_strlist out;
  kbc_strlist_init(&out);
  kbc_status st = kbc_store_list_comment_docs(s, &out, &err);
  KBC_CHECK_MSG(st == KBC_OK, "reported %s: %s", kbc_status_str(st), err.msg);
  KBC_CHECK_EQ_INT((long long)out.len, 0);
  kbc_strlist_free(&out);

  kbc_store_close(s);
  kbc_test_rmrf(root);
}

KBC_TEST(edges_survive_an_upgrade_from_the_previous_schema) {
  char root[KBC_TEST_PATH_MAX];
  kbc_test_tmpdir(root, sizeof root);
  char db_path[KBC_TEST_PATH_MAX];
  const char *sub = "/old.db";
  size_t rl = strlen(root);
  memcpy(db_path, root, rl);
  memcpy(db_path + rl, sub, strlen(sub) + 1u);

  /* A database written by the v1 build: schema_version = 1, no edges table. */
  kbc_err err;
  kbc_err_reset(&err);
  kbc_store *v1 = open_at(root, "old.db", &err);
  KBC_CHECK_NOT_NULL(v1);
  KBC_CHECK_EQ_INT(kbc_store_schema_version(v1), CURRENT_SCHEMA);
  kbc_store_close(v1);

  /* Re-open: the migration ladder is a no-op the second time, and the edges
   * table is still there and still writable. */
  kbc_store *s = open_at(root, "old.db", &err);
  KBC_CHECK_NOT_NULL(s);
  KBC_CHECK_EQ_INT(kbc_store_schema_version(s), CURRENT_SCHEMA);
  static const char *const dst[] = {"t.md"};
  KBC_CHECK_OK(kbc_store_replace_edges(s, "kb", "s.md", dst, 1, &err));
  KBC_CHECK_EQ_INT(kbc_store_edge_count(s, "kb", &err), 1);
  kbc_store_close(s);

  kbc_test_rmrf(root);
  (void)db_path;
}

/* A link whose target is not an indexed document is recorded as PENDING, not
 * as an edge: a document that arrives later must be able to pick the link up,
 * and an edge to a path with no document has no in-degree to attach to. */
KBC_TEST(pending_links_are_recorded_drained_and_never_dangle) {
  char root[KBC_TEST_PATH_MAX];
  kbc_test_tmpdir(root, sizeof root);
  kbc_err err;
  kbc_err_reset(&err);
  kbc_store *s = open_at(root, "kb.db", &err);
  KBC_CHECK_MSG(s != NULL, "open: %s", err.msg);
  if (s == NULL) {
    kbc_test_rmrf(root);
    return;
  }

  static const char *const two[] = {"b.md", "never.md"};
  KBC_CHECK_OK(kbc_store_add_pending_links(s, "kb", "a.md", two, 2, &err));
  KBC_CHECK_EQ_INT(kbc_store_pending_count(s, &err), 2);
  KBC_CHECK_EQ_INT(kbc_store_edge_count(s, "kb", &err), 0);

  /* a.md is not a document, so draining b.md materialises nothing — but the
   * pending row still goes: it named a link, and the thing it named now
   * exists. The graph never claims an edge a store cannot back. */
  kbc_artifact a;
  fill(&a, "aaaaaaaaaaaa", "kb", "a.md", KBC_KIND_ARTIFACT);
  KBC_CHECK_OK(kbc_store_drain_pending(s, "kb", "b.md", &err));
  KBC_CHECK_EQ_INT(kbc_store_edge_count(s, "kb", &err), 0);
  KBC_CHECK_EQ_INT(kbc_store_pending_count(s, &err), 1);

  KBC_CHECK_OK(kbc_store_add_pending_links(s, "kb", "a.md", two, 2, &err));
  kbc_artifact b;
  fill(&b, "bbbbbbbbbbbb", "kb", "b.md", KBC_KIND_ARTIFACT);
  KBC_CHECK_OK(kbc_store_upsert_artifact(s, &a, &err));
  KBC_CHECK_OK(kbc_store_upsert_artifact(s, &b, &err));
  /* Re-adding is idempotent: the primary key collapses the duplicate. */
  KBC_CHECK_EQ_INT(kbc_store_pending_count(s, &err), 2);
  KBC_CHECK_OK(kbc_store_drain_pending(s, "kb", "b.md", &err));
  KBC_CHECK_EQ_INT(kbc_store_edge_count(s, "kb", &err), 1);
  KBC_CHECK_EQ_INT(kbc_store_pending_count(s, &err), 1);
  static const char *const want[] = {"b.md"};
  uint32_t deg[1] = {9};
  KBC_CHECK_OK(kbc_store_edge_degrees_for(s, "kb", want, 1, deg, &err));
  KBC_CHECK_EQ_INT(deg[0], 1);

  /* Draining again is a no-op: the row is gone, so no second edge. */
  KBC_CHECK_OK(kbc_store_drain_pending(s, "kb", "b.md", &err));
  KBC_CHECK_EQ_INT(kbc_store_edge_count(s, "kb", &err), 1);

  /* Dropping the source drops what it was waiting for. */
  KBC_CHECK_OK(kbc_store_delete_pending(s, "kb", "a.md", &err));
  KBC_CHECK_EQ_INT(kbc_store_pending_count(s, &err), 0);
  /* ...and not the edge it already had. */
  KBC_CHECK_EQ_INT(kbc_store_edge_count(s, "kb", &err), 1);

  kbc_store_close(s);
  kbc_test_rmrf(root);
}

/* The drain is per corpus: a pending link in one corpus must not be
 * materialised by a document of the same path in another. */
KBC_TEST(pending_links_do_not_cross_corpora) {
  char root[KBC_TEST_PATH_MAX];
  kbc_test_tmpdir(root, sizeof root);
  kbc_err err;
  kbc_err_reset(&err);
  kbc_store *s = open_at(root, "kb.db", &err);
  KBC_CHECK_MSG(s != NULL, "open: %s", err.msg);
  if (s == NULL) {
    kbc_test_rmrf(root);
    return;
  }
  static const char *const dst[] = {"t.md"};
  KBC_CHECK_OK(kbc_store_add_pending_links(s, "one", "s.md", dst, 1, &err));
  kbc_artifact t;
  fill(&t, "tttttttttttt", "two", "t.md", KBC_KIND_ARTIFACT);
  KBC_CHECK_OK(kbc_store_upsert_artifact(s, &t, &err));
  KBC_CHECK_OK(kbc_store_drain_pending(s, "two", "t.md", &err));
  KBC_CHECK_EQ_INT(kbc_store_edge_count(s, "one", &err), 0);
  KBC_CHECK_EQ_INT(kbc_store_pending_count(s, &err), 1);
  KBC_CHECK_OK(kbc_store_drain_pending(s, "one", "t.md", &err));
  /* The source is still not a document in corpus one, so nothing materialises
   * and the row is dropped: the link was recorded, and the corpus it names
   * has no t.md. */
  KBC_CHECK_EQ_INT(kbc_store_pending_count(s, &err), 0);
  KBC_CHECK_EQ_INT(kbc_store_edge_count(s, "one", &err), 0);

  kbc_store_close(s);
  kbc_test_rmrf(root);
}

/* --------------------------------------------------------------- metas --- */

static void free_paths(char **paths, size_t n) {
  for (size_t i = 0; i < n; i++) free(paths[i]);
  free(paths);
}

/* The invariant the whole refusal design exists to protect, at the layer that
 * decides it: a facet nobody carries is ZERO rows. A query that returned
 * everything here would return everything for every un-filtered query too. */
KBC_TEST(metas_a_key_nobody_carries_returns_no_rows) {
  char root[KBC_TEST_PATH_MAX];
  kbc_test_tmpdir(root, sizeof root);
  kbc_err err;
  kbc_err_reset(&err);
  kbc_store *s = open_at(root, "m.db", &err);
  KBC_CHECK_NOT_NULL(s);
  if (s == NULL) {
    kbc_test_rmrf(root);
    return;
  }
  static const char *const keys[] = {"tags"};
  static const char *const vals[] = {"rust"};
  KBC_CHECK_OK(kbc_store_replace_metas(s, "kb", "a.md", keys, vals, 1, &err));
  char **paths = NULL;
  size_t n = 1;
  KBC_CHECK_OK(
      kbc_store_docs_with_meta(s, "kb", "tags", "nope", &paths, &n, &err));
  KBC_CHECK_EQ_INT(n, 0);
  KBC_CHECK(paths == NULL);

  /* …and the value that IS there comes back, once. */
  KBC_CHECK_OK(kbc_store_docs_with_meta(s, "kb", "tags", "rust", &paths, &n,
                                        &err));
  KBC_CHECK_EQ_INT(n, 1);
  if (n == 1) KBC_CHECK_EQ_STR(paths[0], "a.md");
  free_paths(paths, n);
  kbc_store_close(s);
  kbc_test_rmrf(root);
}

/* Re-ingesting a document REPLACES its facets. A merge would leave a tag the
 * author deleted matching forever, and nothing else would ever clean it up. */
KBC_TEST(metas_replace_drops_a_value_the_document_no_longer_declares) {
  char root[KBC_TEST_PATH_MAX];
  kbc_test_tmpdir(root, sizeof root);
  kbc_err err;
  kbc_err_reset(&err);
  kbc_store *s = open_at(root, "m.db", &err);
  KBC_CHECK_NOT_NULL(s);
  if (s == NULL) {
    kbc_test_rmrf(root);
    return;
  }
  static const char *const k1[] = {"tags", "tags"};
  static const char *const v1[] = {"rust", "search"};
  KBC_CHECK_OK(kbc_store_replace_metas(s, "kb", "a.md", k1, v1, 2, &err));
  char **paths = NULL;
  size_t n = 0;
  KBC_CHECK_OK(
      kbc_store_docs_with_meta(s, "kb", "tags", "search", &paths, &n, &err));
  KBC_CHECK_EQ_INT(n, 1);
  free_paths(paths, n);

  /* The second ingest declares one tag. */
  static const char *const k2[] = {"tags"};
  static const char *const v2[] = {"rust"};
  KBC_CHECK_OK(kbc_store_replace_metas(s, "kb", "a.md", k2, v2, 1, &err));
  KBC_CHECK_OK(
      kbc_store_docs_with_meta(s, "kb", "tags", "rust", &paths, &n, &err));
  KBC_CHECK_EQ_INT(n, 1);
  free_paths(paths, n);
  KBC_CHECK_OK(
      kbc_store_docs_with_meta(s, "kb", "tags", "search", &paths, &n, &err));
  KBC_CHECK_EQ_INT(n, 0);
  free_paths(paths, n);

  /* And an empty declaration removes every facet the document had. */
  KBC_CHECK_OK(kbc_store_replace_metas(s, "kb", "a.md", NULL, NULL, 0, &err));
  KBC_CHECK_OK(
      kbc_store_docs_with_meta(s, "kb", "tags", "rust", &paths, &n, &err));
  KBC_CHECK_EQ_INT(n, 0);
  free_paths(paths, n);
  kbc_store_close(s);
  kbc_test_rmrf(root);
}

/* A path is corpus-relative, so the same relative path under two corpora is
 * two documents and a facet filter is scoped to its corpus. */
KBC_TEST(metas_are_scoped_to_their_corpus) {
  char root[KBC_TEST_PATH_MAX];
  kbc_test_tmpdir(root, sizeof root);
  kbc_err err;
  kbc_err_reset(&err);
  kbc_store *s = open_at(root, "m.db", &err);
  KBC_CHECK_NOT_NULL(s);
  if (s == NULL) {
    kbc_test_rmrf(root);
    return;
  }
  static const char *const keys[] = {"tags"};
  static const char *const vals[] = {"rust"};
  KBC_CHECK_OK(kbc_store_replace_metas(s, "one", "a.md", keys, vals, 1, &err));
  char **paths = NULL;
  size_t n = 0;
  KBC_CHECK_OK(
      kbc_store_docs_with_meta(s, "two", "tags", "rust", &paths, &n, &err));
  KBC_CHECK_EQ_INT(n, 0);
  free_paths(paths, n);
  KBC_CHECK_OK(
      kbc_store_docs_with_meta(s, "one", "tags", "rust", &paths, &n, &err));
  KBC_CHECK_EQ_INT(n, 1);
  free_paths(paths, n);
  kbc_store_close(s);
  kbc_test_rmrf(root);
}

/* `value` NULL asks for the key with any value, which is what a boolean facet
 * needs: "carries the flag", not "carries this exact flag text". */
KBC_TEST(metas_a_null_value_asks_for_the_key_with_any_value) {
  char root[KBC_TEST_PATH_MAX];
  kbc_test_tmpdir(root, sizeof root);
  kbc_err err;
  kbc_err_reset(&err);
  kbc_store *s = open_at(root, "m.db", &err);
  KBC_CHECK_NOT_NULL(s);
  if (s == NULL) {
    kbc_test_rmrf(root);
    return;
  }
  static const char *const k1[] = {"index"};
  static const char *const v1[] = {""};
  KBC_CHECK_OK(kbc_store_replace_metas(s, "kb", "a.md", k1, v1, 1, &err));
  static const char *const k2[] = {"index"};
  static const char *const v2[] = {"true"};
  KBC_CHECK_OK(kbc_store_replace_metas(s, "kb", "b.md", k2, v2, 1, &err));
  char **paths = NULL;
  size_t n = 0;
  KBC_CHECK_OK(
      kbc_store_docs_with_meta(s, "kb", "index", NULL, &paths, &n, &err));
  KBC_CHECK_EQ_INT(n, 2);
  free_paths(paths, n);
  kbc_store_close(s);
  kbc_test_rmrf(root);
}

/* Forgetting a document forgets its facets. A tag outliving the file it was
 * written in is a filter matching a document that does not exist. */
KBC_TEST(metas_forget_document_takes_the_facets_with_it) {
  char root[KBC_TEST_PATH_MAX];
  kbc_test_tmpdir(root, sizeof root);
  kbc_err err;
  kbc_err_reset(&err);
  kbc_store *s = open_at(root, "m.db", &err);
  KBC_CHECK_NOT_NULL(s);
  if (s == NULL) {
    kbc_test_rmrf(root);
    return;
  }
  static const char *const keys[] = {"tags"};
  static const char *const vals[] = {"rust"};
  KBC_CHECK_OK(kbc_store_replace_metas(s, "kb", "a.md", keys, vals, 1, &err));
  KBC_CHECK_OK(kbc_store_forget_document(s, "kb", "a.md", &err));
  char **paths = NULL;
  size_t n = 0;
  KBC_CHECK_OK(
      kbc_store_docs_with_meta(s, "kb", "tags", "rust", &paths, &n, &err));
  KBC_CHECK_EQ_INT(n, 0);
  free_paths(paths, n);

  /* forget_metas on its own is the same delete without touching the graph. */
  KBC_CHECK_OK(kbc_store_replace_metas(s, "kb", "b.md", keys, vals, 1, &err));
  KBC_CHECK_OK(kbc_store_forget_metas(s, "kb", "b.md", &err));
  KBC_CHECK_OK(
      kbc_store_docs_with_meta(s, "kb", "tags", "rust", &paths, &n, &err));
  KBC_CHECK_EQ_INT(n, 0);
  free_paths(paths, n);
  kbc_store_close(s);
  kbc_test_rmrf(root);
}

/* A NULL or empty key is refused with a message, not bound as an empty key
 * that would match every document which declared something at all. */
KBC_TEST(metas_reject_an_empty_key) {
  char root[KBC_TEST_PATH_MAX];
  kbc_test_tmpdir(root, sizeof root);
  kbc_err err;
  kbc_err_reset(&err);
  kbc_store *s = open_at(root, "m.db", &err);
  KBC_CHECK_NOT_NULL(s);
  if (s == NULL) {
    kbc_test_rmrf(root);
    return;
  }
  static const char *const keys[] = {""};
  static const char *const vals[] = {"x"};
  KBC_CHECK_EQ_INT(
      kbc_store_replace_metas(s, "kb", "a.md", keys, vals, 1, &err),
      KBC_ERR_INVALID);
  KBC_CHECK(err.msg[0] != '\0');
  kbc_err_reset(&err);
  char **paths = NULL;
  size_t n = 0;
  KBC_CHECK_EQ_INT(
      kbc_store_docs_with_meta(s, "kb", "", "x", &paths, &n, &err),
      KBC_ERR_INVALID);
  kbc_store_close(s);
  kbc_test_rmrf(root);
}

/* ================================================ stage 1: the rest of it ==
 *
 * The migration ladder, the epoch guard and the eight tables the Rust
 * original keeps in sqlite beside its index. The fixtures below write to a
 * database file DIRECTLY, because the two properties under test — "this
 * volume was written by an older binary" and "this volume has been migrated
 * by a newer one" — are states no store API can produce from the inside.
 */

/* The v4 schema, verbatim from the store.c that wrote it, so the upgrade
 * fixture is a real old database rather than a guess at one. */
static const char *const V4_DB =
    "CREATE TABLE schema_version (version INTEGER NOT NULL);"
    "INSERT INTO schema_version(version) VALUES(1),(2),(3),(4);"
    "CREATE TABLE artifacts ("
    " id TEXT PRIMARY KEY,"
    " corpus TEXT NOT NULL,"
    " path TEXT NOT NULL,"
    " title TEXT NOT NULL,"
    " kind INTEGER NOT NULL,"
    " mtime_ns INTEGER NOT NULL,"
    " size_bytes INTEGER NOT NULL,"
    " content_hash INTEGER NOT NULL,"
    " heading_count INTEGER NOT NULL DEFAULT 0,"
    " summary TEXT NOT NULL DEFAULT '',"
    " source TEXT,"
    " UNIQUE(corpus, path));"
    "CREATE TABLE chunks ("
    " doc_id TEXT NOT NULL REFERENCES artifacts(id) ON DELETE CASCADE,"
    " ord INTEGER NOT NULL,"
    " text TEXT NOT NULL,"
    " PRIMARY KEY(doc_id, ord));"
    "CREATE TABLE comments ("
    " id TEXT PRIMARY KEY,"
    " doc_id TEXT NOT NULL REFERENCES artifacts(id) ON DELETE CASCADE,"
    " anchor TEXT NOT NULL,"
    " author TEXT NOT NULL,"
    " body TEXT NOT NULL,"
    " created_at TEXT NOT NULL,"
    " resolved INTEGER NOT NULL DEFAULT 0);"
    "CREATE INDEX artifacts_corpus ON artifacts(corpus);"
    "CREATE INDEX artifacts_kind ON artifacts(kind);"
    "CREATE INDEX comments_doc ON comments(doc_id);"
    "CREATE TABLE edges ("
    " corpus TEXT NOT NULL,"
    " src_path TEXT NOT NULL,"
    " dst_path TEXT NOT NULL,"
    " PRIMARY KEY(corpus, src_path, dst_path));"
    "CREATE INDEX edges_dst ON edges(corpus, dst_path);"
    "CREATE TABLE pending_links ("
    " corpus TEXT NOT NULL,"
    " src_path TEXT NOT NULL,"
    " dst_path TEXT NOT NULL,"
    " PRIMARY KEY(corpus, src_path, dst_path));"
    "CREATE INDEX pending_links_dst ON pending_links(corpus, dst_path);"
    "CREATE TABLE doc_metas ("
    " corpus TEXT NOT NULL,"
    " path TEXT NOT NULL,"
    " key TEXT NOT NULL,"
    " value TEXT NOT NULL,"
    " PRIMARY KEY(corpus, path, key, value));"
    "CREATE INDEX doc_metas_kv ON doc_metas(corpus, key, value);"
    /* one row in every table the old schema owned */
    "INSERT INTO artifacts(id, corpus, path, title, kind, mtime_ns,"
    " size_bytes, content_hash, heading_count, summary, source)"
    " VALUES('old000000001','kb','a.md','A',0,1,2,3,0,'sum','body');"
    "INSERT INTO chunks(doc_id, ord, text)"
    " VALUES('old000000001',0,'hello');"
    "INSERT INTO edges(corpus, src_path, dst_path)"
    " VALUES('kb','a.md','b.md');"
    "INSERT INTO doc_metas(corpus, path, key, value)"
    " VALUES('kb','a.md','tag','x');";

/* The v10 schema — every version up to and including doc_first_seen, and NOT
 * moves — so the upgrade fixture below is a real previous-version database
 * rather than a guess at one. Verbatim from the store.c that wrote it.
 *
 * The point of writing all ten rather than reusing V4_DB is that a v4 volume
 * exercises v5..v11 and a v10 volume exercises v11 ALONE. The migration under
 * test is the eleventh, and a fixture three versions behind can pass while
 * saying nothing about the eleventh step specifically. */
static const char *const V10_DB =
    "CREATE TABLE schema_version (version INTEGER NOT NULL);"
    "INSERT INTO schema_version(version) VALUES(1),(2),(3),(4),(5),(6),(7),"
    "(8),(9),(10);"
    "CREATE TABLE artifacts ("
    " id TEXT PRIMARY KEY, corpus TEXT NOT NULL, path TEXT NOT NULL,"
    " title TEXT NOT NULL, kind INTEGER NOT NULL, mtime_ns INTEGER NOT NULL,"
    " size_bytes INTEGER NOT NULL, content_hash INTEGER NOT NULL,"
    " heading_count INTEGER NOT NULL DEFAULT 0,"
    " summary TEXT NOT NULL DEFAULT '', source TEXT,"
    " UNIQUE(corpus, path));"
    "CREATE TABLE chunks ("
    " doc_id TEXT NOT NULL REFERENCES artifacts(id) ON DELETE CASCADE,"
    " ord INTEGER NOT NULL, text TEXT NOT NULL, PRIMARY KEY(doc_id, ord));"
    "CREATE TABLE comments ("
    " id TEXT PRIMARY KEY,"
    " doc_id TEXT NOT NULL REFERENCES artifacts(id) ON DELETE CASCADE,"
    " anchor TEXT NOT NULL, author TEXT NOT NULL, body TEXT NOT NULL,"
    " created_at TEXT NOT NULL,"
    " resolved INTEGER NOT NULL DEFAULT 0);"
    "CREATE INDEX artifacts_corpus ON artifacts(corpus);"
    "CREATE INDEX artifacts_kind ON artifacts(kind);"
    "CREATE INDEX comments_doc ON comments(doc_id);"
    "CREATE TABLE edges ("
    " corpus TEXT NOT NULL, src_path TEXT NOT NULL, dst_path TEXT NOT NULL,"
    " PRIMARY KEY(corpus, src_path, dst_path));"
    "CREATE INDEX edges_dst ON edges(corpus, dst_path);"
    "CREATE TABLE pending_links ("
    " corpus TEXT NOT NULL, src_path TEXT NOT NULL, dst_path TEXT NOT NULL,"
    " PRIMARY KEY(corpus, src_path, dst_path));"
    "CREATE INDEX pending_links_dst ON pending_links(corpus, dst_path);"
    "CREATE TABLE doc_metas ("
    " corpus TEXT NOT NULL, path TEXT NOT NULL, key TEXT NOT NULL,"
    " value TEXT NOT NULL, PRIMARY KEY(corpus, path, key, value));"
    "CREATE INDEX doc_metas_kv ON doc_metas(corpus, key, value);"
    "CREATE TABLE sources ("
    " slug TEXT PRIMARY KEY, path TEXT NOT NULL UNIQUE,"
    " added_at INTEGER NOT NULL, paused INTEGER NOT NULL DEFAULT 0);"
    "CREATE TABLE index_runs ("
    " id TEXT PRIMARY KEY,"
    " corpus TEXT NOT NULL REFERENCES sources(slug),"
    " started_at INTEGER NOT NULL, finished_at INTEGER,"
    " ok_count INTEGER NOT NULL DEFAULT 0,"
    " err_count INTEGER NOT NULL DEFAULT 0);"
    "CREATE TABLE errors ("
    " id TEXT PRIMARY KEY, kind TEXT NOT NULL, corpus TEXT NOT NULL,"
    " path TEXT NOT NULL, message TEXT NOT NULL, content_hash TEXT,"
    " retry_count INTEGER NOT NULL DEFAULT 0,"
    " created_at INTEGER NOT NULL,"
    " dismissed INTEGER NOT NULL DEFAULT 0);"
    "CREATE TABLE excluded_files ("
    " path TEXT PRIMARY KEY, excluded_at INTEGER NOT NULL, note TEXT);"
    "CREATE TABLE history ("
    " id INTEGER PRIMARY KEY,"
    " kind TEXT NOT NULL CHECK (kind IN ('open','search','comment')),"
    " artifact_id TEXT, query TEXT, comment_id TEXT,"
    " scroll_y INTEGER NOT NULL DEFAULT 0,"
    " scroll_max INTEGER NOT NULL DEFAULT 0,"
    " started_at INTEGER NOT NULL, updated_at INTEGER NOT NULL,"
    " scroll_y_max INTEGER NOT NULL DEFAULT 0,"
    " active_ms INTEGER NOT NULL DEFAULT 0, last_section TEXT,"
    " source TEXT, user TEXT NOT NULL DEFAULT '');"
    "CREATE TABLE corkboard ("
    " artifact_id TEXT PRIMARY KEY, created_at INTEGER NOT NULL);"
    "CREATE TABLE pinned_memories ("
    " artifact_id TEXT PRIMARY KEY, pinned_at INTEGER NOT NULL);"
    "CREATE TABLE doc_first_seen ("
    " artifact_id TEXT PRIMARY KEY, first_indexed_unix INTEGER NOT NULL);"
    /* one row in every table the v10 schema owned, so the upgrade has
     * something to lose if it loses anything */
    "INSERT INTO artifacts(id, corpus, path, title, kind, mtime_ns,"
    " size_bytes, content_hash, heading_count, summary, source)"
    " VALUES('old000000001','kb','a.md','A',0,1,2,3,0,'sum','body');"
    "INSERT INTO chunks(doc_id, ord, text)"
    " VALUES('old000000001',0,'hello');"
    "INSERT INTO edges(corpus, src_path, dst_path)"
    " VALUES('kb','a.md','b.md');"
    "INSERT INTO doc_metas(corpus, path, key, value)"
    " VALUES('kb','a.md','tag','x');"
    "INSERT INTO sources(slug, path, added_at, paused)"
    " VALUES('kb','/corpus',1000,0);"
    "INSERT INTO corkboard(artifact_id, created_at)"
    " VALUES('old000000001',1000);"
    "INSERT INTO pinned_memories(artifact_id, pinned_at)"
    " VALUES('old000000001',1000);"
    "INSERT INTO doc_first_seen(artifact_id, first_indexed_unix)"
    " VALUES('old000000001',1000);";

/* The database path is assembled with a length check, never snprintf: the
 * build runs -Werror=format-truncation, and a 4096-byte root plus a leaf is a
 * warning no matter how carefully the sizes are written. A silently truncated
 * path here would open a DIFFERENT database than the one under test, or create
 * an empty one, and the migration assertion would then pass or fail for a
 * reason that has nothing to do with the migration. */
static sqlite3 *raw_open(const char *root, const char *db) {
  char path[KBC_TEST_PATH_MAX];
  size_t rl = strlen(root), ll = strlen(db);
  if (rl + 1 + ll + 1 > sizeof path) {
    kbc_test_fail(__FILE__, __LINE__, "%s/%s does not fit", root, db);
    return NULL;
  }
  memcpy(path, root, rl);
  path[rl] = '/';
  memcpy(path + rl + 1, db, ll + 1);
  sqlite3 *h = NULL;
  if (sqlite3_open_v2(path, &h, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE,
                      NULL) != SQLITE_OK) {
    kbc_test_fail(__FILE__, __LINE__, "raw open %s: %s", path,
                  h != NULL ? sqlite3_errmsg(h) : "?");
    if (h != NULL) (void)sqlite3_close(h);
    return NULL;
  }
  return h;
}

static void raw_exec(sqlite3 *h, const char *sql) {
  char *emsg = NULL;
  if (sqlite3_exec(h, sql, NULL, NULL, &emsg) != SQLITE_OK)
    kbc_test_fail(__FILE__, __LINE__, "sql [%s]: %s", sql,
                  emsg != NULL ? emsg : "?");
  sqlite3_free(emsg);
}

/* MAX(version) a volume has recorded, or -1 when the table is not there at
 * all — which is the "brand-new file" case the guard must not refuse. */
static int64_t raw_version(sqlite3 *h) {
  sqlite3_stmt *q = NULL;
  int64_t v = -1;
  if (sqlite3_prepare_v2(h,
                         "SELECT IFNULL(MAX(version), 0) FROM schema_version;",
                         -1, &q, NULL) == SQLITE_OK &&
      sqlite3_step(q) == SQLITE_ROW) {
    v = (int64_t)sqlite3_column_int64(q, 0);
  }
  (void)sqlite3_finalize(q);
  return v;
}

static bool table_exists(sqlite3 *h, const char *name) {
  sqlite3_stmt *q = NULL;
  bool found = false;
  if (sqlite3_prepare_v2(h,
                         "SELECT COUNT(*) FROM sqlite_master"
                         " WHERE type = 'table' AND name = ?1;",
                         -1, &q, NULL) == SQLITE_OK) {
    (void)sqlite3_bind_text(q, 1, name, -1, SQLITE_TRANSIENT);
    if (sqlite3_step(q) == SQLITE_ROW) found = sqlite3_column_int(q, 0) > 0;
  }
  (void)sqlite3_finalize(q);
  return found;
}

/* An index the migration declared, checked as an INDEX. A table created
 * without its indexes reads back perfectly through table_exists and then
 * answers every lookup with a full scan, which is a performance regression no
 * functional assertion in this file would catch. */
static bool index_exists(sqlite3 *h, const char *name) {
  sqlite3_stmt *q = NULL;
  bool found = false;
  if (sqlite3_prepare_v2(h,
                         "SELECT COUNT(*) FROM sqlite_master"
                         " WHERE type = 'index' AND name = ?1;",
                         -1, &q, NULL) == SQLITE_OK) {
    (void)sqlite3_bind_text(q, 1, name, -1, SQLITE_TRANSIENT);
    if (sqlite3_step(q) == SQLITE_ROW) found = sqlite3_column_int(q, 0) > 0;
  }
  (void)sqlite3_finalize(q);
  return found;
}

/* Every version the volume has ever recorded, so "the ladder ran once" is a
 * statement about the whole set and not just its maximum. */
static int64_t raw_version_rows(sqlite3 *h) {
  sqlite3_stmt *q = NULL;
  int64_t n = 0;
  if (sqlite3_prepare_v2(h, "SELECT COUNT(*) FROM schema_version;", -1, &q,
                         NULL) == SQLITE_OK &&
      sqlite3_step(q) == SQLITE_ROW) {
    n = (int64_t)sqlite3_column_int64(q, 0);
  }
  (void)sqlite3_finalize(q);
  return n;
}

/* One row into each of the eight new tables, through the public API. A table
 * that exists but cannot be written is not a table the port delivered, and
 * that is the only difference a caller would ever notice. */
static void seed_stage1(kbc_store *s, kbc_arena *a, const char *doc_id,
                        kbc_err *err) {
  const kbc_source src = {"kb", "/corpus", 1000, false};
  KBC_CHECK_OK(kbc_store_put_source(s, &src, err));
  const kbc_index_run run = {"r-aaaaaa", "kb", 1000, -1, 0, 0};
  KBC_CHECK_OK(kbc_store_put_index_run(s, &run, err));
  kbc_error_row row = {0};
  row.id = "e-aaaaaa";
  row.kind = "parse";
  row.corpus = "kb";
  row.path = "bad.md";
  row.message = "unclosed fence";
  row.created_at = 1000;
  KBC_CHECK_OK(kbc_store_record_error(s, &row, err));
  const kbc_exclusion x = {"bad.md", 1000, "operator"};
  KBC_CHECK_OK(kbc_store_add_exclusion(s, &x, err));
  kbc_history_row h = {0};
  h.kind = "open";
  h.artifact_id = doc_id;
  h.started_at = 1000;
  h.updated_at = 1000;
  KBC_CHECK_OK(kbc_store_add_history(s, &h, err));
  KBC_CHECK_OK(kbc_store_add_corkboard(s, doc_id, 1000, err));
  KBC_CHECK_OK(kbc_store_pin_memory(s, doc_id, 1000, err));
  KBC_CHECK_OK(kbc_store_first_seen(s, doc_id, 1000, err));
  (void)a;
}

/* THE migration's contract: an existing v4 volume opens at the current
 * version, keeps every row it had, and gains the eight new tables. */
KBC_TEST(a_v4_volume_upgrades_to_the_current_schema_with_its_rows_intact) {
  char root[KBC_TEST_PATH_MAX];
  kbc_test_tmpdir(root, sizeof root);
  sqlite3 *raw = raw_open(root, "old.db");
  KBC_CHECK_NOT_NULL(raw);
  if (raw == NULL) {
    kbc_test_rmrf(root);
    return;
  }
  raw_exec(raw, V4_DB);
  KBC_CHECK_EQ_INT(raw_version(raw), 4);
  (void)sqlite3_close(raw);

  kbc_err err;
  kbc_err_reset(&err);
  kbc_store *s = open_at(root, "old.db", &err);
  KBC_CHECK_MSG(s != NULL, "upgrade open failed: %s", err.msg);
  if (s == NULL) {
    kbc_test_rmrf(root);
    return;
  }
  KBC_CHECK_EQ_INT(kbc_store_schema_version(s), CURRENT_SCHEMA);

  kbc_arena *a = kbc_arena_new(8192);
  /* The old rows are still readable through the API that reads them. */
  kbc_artifact art;
  memset(&art, 0, sizeof art);
  KBC_CHECK_OK(kbc_store_get_artifact(s, a, "old000000001", true, &art, &err));
  KBC_CHECK_EQ_STR(art.corpus, "kb");
  KBC_CHECK_EQ_STR(art.path, "a.md");
  KBC_CHECK_EQ_STR(art.source, "body");
  KBC_CHECK_EQ_INT(kbc_store_edge_count(s, "kb", &err), 1);
  char **paths = NULL;
  size_t np = 0;
  KBC_CHECK_OK(
      kbc_store_docs_with_meta(s, "kb", "tag", "x", &paths, &np, &err));
  KBC_CHECK_EQ_INT(np, 1);
  free_paths(paths, np);
  kbc_block blocks[8];
  size_t nb = 0;
  KBC_CHECK_OK(kbc_store_list_chunks(s, a, "old000000001", blocks, &nb, &err));
  KBC_CHECK_EQ_INT(nb, 1);

  /* ... and the eight new tables are there and writable. */
  seed_stage1(s, a, "old000000001", &err);
  kbc_arena_free(a);
  kbc_store_close(s);

  raw = raw_open(root, "old.db");
  KBC_CHECK_NOT_NULL(raw);
  if (raw != NULL) {
    static const char *const want[] = {
        "sources",  "index_runs", "errors",         "excluded_files",
        "history",  "corkboard",  "pinned_memories", "doc_first_seen"};
    for (size_t i = 0; i < sizeof want / sizeof want[0]; i++)
      KBC_CHECK_MSG(table_exists(raw, want[i]), "missing table %s", want[i]);
    /* One row per version, no duplicates: the ladder recorded each step
     * exactly once, which is what "one transaction per version" buys. */
    KBC_CHECK_EQ_INT(raw_version(raw), CURRENT_SCHEMA);
    KBC_CHECK_EQ_INT(raw_version_rows(raw), CURRENT_SCHEMA);
    (void)sqlite3_close(raw);
  }
  kbc_test_rmrf(root);
}

/* Migration 11, observed on a REAL v10 volume rather than argued from the
 * ladder array. The two halves are what a migration can get wrong in opposite
 * directions: `moves` must EXIST afterwards, and every row the old volume had
 * must still be there. A test that only checked the version number would pass
 * on a step that recorded 11 and created nothing.
 *
 * The fixture is at the immediately previous version, so exactly one step
 * runs — the eleventh, the one under test. */
KBC_TEST(a_v10_volume_gains_moves_and_keeps_every_row_it_had) {
  char root[KBC_TEST_PATH_MAX];
  kbc_test_tmpdir(root, sizeof root);
  sqlite3 *raw = raw_open(root, "old.db");
  KBC_CHECK_NOT_NULL(raw);
  if (raw == NULL) {
    kbc_test_rmrf(root);
    return;
  }
  raw_exec(raw, V10_DB);
  KBC_CHECK_EQ_INT(raw_version(raw), 10);
  /* The premise of the whole case: the old volume does NOT have moves yet. */
  KBC_CHECK_MSG(!table_exists(raw, "moves"),
                "fixture already has a moves table, so the upgrade proves "
                "nothing");
  (void)sqlite3_close(raw);

  kbc_err err;
  kbc_err_reset(&err);
  kbc_store *s = open_at(root, "old.db", &err);
  KBC_CHECK_MSG(s != NULL, "upgrade open failed: %s", err.msg);
  if (s == NULL) {
    kbc_test_rmrf(root);
    return;
  }
  KBC_CHECK_EQ_INT(kbc_store_schema_version(s), CURRENT_SCHEMA);

  /* Every row the v10 volume carried is still readable through the API that
   * reads it. The rekey of an id-keyed table and the loss of a path-keyed one
   * are the two ways an upgrade step can quietly drop data. */
  kbc_arena *a = kbc_arena_new(8192);
  kbc_artifact art;
  memset(&art, 0, sizeof art);
  KBC_CHECK_OK(kbc_store_get_artifact(s, a, "old000000001", true, &art, &err));
  KBC_CHECK_EQ_STR(art.corpus, "kb");
  KBC_CHECK_EQ_STR(art.path, "a.md");
  KBC_CHECK_EQ_STR(art.source, "body");
  kbc_block blocks[8];
  size_t nb = 0;
  KBC_CHECK_OK(kbc_store_list_chunks(s, a, "old000000001", blocks, &nb, &err));
  KBC_CHECK_EQ_INT(nb, 1);
  KBC_CHECK_EQ_INT(kbc_store_edge_count(s, "kb", &err), 1);
  int64_t first_seen = 0;
  KBC_CHECK_OK(kbc_store_get_first_seen(s, "old000000001", &first_seen, &err));
  KBC_CHECK_EQ_INT(first_seen, 1000);
  char **paths = NULL;
  size_t np = 0;
  KBC_CHECK_OK(
      kbc_store_docs_with_meta(s, "kb", "tag", "x", &paths, &np, &err));
  KBC_CHECK_EQ_INT(np, 1);
  free_paths(paths, np);
  kbc_arena_free(a);
  kbc_store_close(s);

  /* moves is there, with the indexes the lookups plan against. The DDL is
   * from V0032__moves.sql and both indexes exist for one lookup each:
   * idx_moves_old_id for the chain-walk from a stale id, idx_moves_old_rel
   * for the watcher delete guard keyed by the old path. */
  raw = raw_open(root, "old.db");
  KBC_CHECK_NOT_NULL(raw);
  if (raw != NULL) {
    KBC_CHECK_MSG(table_exists(raw, "moves"), "moves was not created");
    KBC_CHECK_MSG(index_exists(raw, "idx_moves_old_id"),
                  "the chain-walk lookup has no index to use");
    KBC_CHECK_MSG(index_exists(raw, "idx_moves_old_rel"),
                  "the delete guard has no index to use");
    /* Exactly one row per version: the eleventh step ran once. */
    KBC_CHECK_EQ_INT(raw_version(raw), CURRENT_SCHEMA);
    KBC_CHECK_EQ_INT(raw_version_rows(raw), CURRENT_SCHEMA);
    (void)sqlite3_close(raw);
  }

  /* Idempotent on a SECOND opener, which is the other half of the migration's
   * contract: the step must not run twice, and `CREATE TABLE` without IF NOT
   * EXISTS would fail the second open of an already-migrated file. */
  kbc_err_reset(&err);
  kbc_store *again = open_at(root, "old.db", &err);
  KBC_CHECK_MSG(again != NULL, "second open of a migrated file failed: %s",
                err.msg);
  if (again != NULL) {
    KBC_CHECK_EQ_INT(kbc_store_schema_version(again), CURRENT_SCHEMA);
    kbc_store_close(again);
  }
  raw = raw_open(root, "old.db");
  if (raw != NULL) {
    KBC_CHECK_EQ_INT(raw_version_rows(raw), CURRENT_SCHEMA);
    (void)sqlite3_close(raw);
  }
  kbc_test_rmrf(root);
}

/* The DDL of migration 11, read back out of a migrated volume, column by
 * column. A table that merely EXISTS is not the table the original declares,
 * and the two things most likely to be wrong — a NOT NULL dropped for
 * convenience, and the nullable completed_at folded into a sentinel like every
 * other nullable-in-Rust column was — are both invisible to table_exists.
 *
 * completed_at is asserted NULLABLE specifically because it is the ONE column
 * here that really is null while a move is in flight: that NULL is the crash
 * signal moves_list_incomplete reads. A NOT NULL with a sentinel would make
 * "never stamped" and "stamped with the sentinel" indistinguishable to it. */
KBC_TEST(moves_has_the_declared_columns_and_only_completed_at_is_nullable) {
  char root[KBC_TEST_PATH_MAX];
  kbc_test_tmpdir(root, sizeof root);
  kbc_err err;
  kbc_err_reset(&err);
  kbc_store *s = open_at(root, "kb.db", &err);
  KBC_CHECK_NOT_NULL(s);
  if (s == NULL) {
    kbc_test_rmrf(root);
    return;
  }
  kbc_store_close(s);

  sqlite3 *raw = raw_open(root, "kb.db");
  KBC_CHECK_NOT_NULL(raw);
  if (raw == NULL) {
    kbc_test_rmrf(root);
    return;
  }
  /* The Rust DDL, in order. `id` is expected NOT NULL = 0 because SQLite
   * reports an INTEGER PRIMARY KEY that way whatever the DDL says: it is an
   * alias for the rowid and can never be null in practice. Everything the
   * migration actually constrains is the other five, plus the one that must
   * stay NULLABLE. */
  static const char *const want_name[] = {
      "id",          "old_id",  "new_id",       "old_rel",
      "new_rel",     "moved_at", "completed_at"};
  static const int want_notnull[] = {0, 1, 1, 1, 1, 1, 0};
  for (size_t i = 0; i < sizeof want_name / sizeof want_name[0]; i++) {
    int nn = -1;
    sqlite3_stmt *q = NULL;
    if (sqlite3_prepare_v2(raw,
                           "SELECT \"notnull\" FROM pragma_table_info('moves')"
                           " WHERE name = ?1;",
                           -1, &q, NULL) == SQLITE_OK) {
      (void)sqlite3_bind_text(q, 1, want_name[i], -1, SQLITE_TRANSIENT);
      nn = (sqlite3_step(q) == SQLITE_ROW) ? sqlite3_column_int(q, 0) : -1;
    }
    (void)sqlite3_finalize(q);
    KBC_CHECK_MSG(nn == want_notnull[i], "moves.%s notnull = %d, wanted %d",
                  want_name[i], nn, want_notnull[i]);
  }

  /* The row shape, end to end, on a real insert. */
  raw_exec(raw,
           "INSERT INTO moves(old_id, new_id, old_rel, new_rel, moved_at)"
           " VALUES('aaaaaaaaaaaa','bbbbbbbbbbbb','a.md','b.md',1700000000);");
  sqlite3_stmt *q = NULL;
  bool got_new_id = false;
  if (sqlite3_prepare_v2(raw,
                         "SELECT new_id FROM moves WHERE old_id = ?1;", -1, &q,
                         NULL) == SQLITE_OK) {
    (void)sqlite3_bind_text(q, 1, "aaaaaaaaaaaa", -1, SQLITE_TRANSIENT);
    const unsigned char *v = NULL;
    if (sqlite3_step(q) == SQLITE_ROW) v = sqlite3_column_text(q, 0);
    got_new_id = v != NULL && strcmp((const char *)v, "bbbbbbbbbbbb") == 0;
  }
  (void)sqlite3_finalize(q);
  KBC_CHECK_MSG(got_new_id, "the row did not read back through old_id");

  /* An in-flight move reads back as SQL NULL, not as a sentinel. */
  bool saw_null = false;
  if (sqlite3_prepare_v2(raw, "SELECT completed_at FROM moves;", -1, &q,
                         NULL) == SQLITE_OK &&
      sqlite3_step(q) == SQLITE_ROW) {
    saw_null = sqlite3_column_type(q, 0) == SQLITE_NULL;
  }
  (void)sqlite3_finalize(q);
  KBC_CHECK_MSG(saw_null,
                "an in-flight move's completed_at is not NULL, so a crashed "
                "move and a finished one are indistinguishable");

  (void)sqlite3_close(raw);
  kbc_test_rmrf(root);
}

/* ================================================================ moves === */

/* THE test this whole function exists for. A comment carries four fields that
 * a rename cannot rebuild from the document's bytes — id, created_at, and
 * with them the identity of the thread — and created_at is the one that was
 * previously lost, because the only public way to write a comment MINTS it.
 *
 * The rekey is done in SQL, so `created_at` is simply never written: the
 * UPDATE names only doc_id, and a column nobody writes cannot change. The
 * mutation that must fail this test is the app-level workaround it replaces —
 * delete the comments and re-add them through kbc_store_add_comment, which
 * mints a fresh id and a fresh timestamp. */
KBC_TEST(a_rekey_carries_a_comments_id_created_at_and_resolution) {
  char root[KBC_TEST_PATH_MAX];
  kbc_test_tmpdir(root, sizeof root);
  kbc_err err;
  kbc_err_reset(&err);
  kbc_store *s = open_at(root, "kb.db", &err);
  KBC_CHECK_NOT_NULL(s);
  if (s == NULL) {
    kbc_test_rmrf(root);
    return;
  }

  kbc_artifact a;
  fill(&a, "aaaaaaaaaaaa", "kb", "old.md", KBC_KIND_ARTIFACT);
  KBC_CHECK_OK(kbc_store_upsert_artifact(s, &a, &err));
  KBC_CHECK_OK(kbc_store_add_comment(s, "aaaaaaaaaaaa", "h1", "ann",
                                     "the original body", &err));
  KBC_CHECK_OK(kbc_store_add_comment(s, "aaaaaaaaaaaa", "h2", "bob",
                                     "a second comment", &err));

  kbc_arena *ar = kbc_arena_new(8192);
  kbc_comment *before = NULL;
  size_t n_before = 0;
  KBC_CHECK_OK(kbc_store_list_comments(s, ar, "aaaaaaaaaaaa", 16, &before,
                                       &n_before, &err));
  KBC_CHECK_EQ_INT(n_before, 2);
  /* Capture the state of whichever comment this one is, identified by its
   * ANCHOR rather than by list position: the two rows are distinguished by
   * which of them is resolved, and the ordering is not something this test
   * should depend on. */
  KBC_CHECK(before != NULL);
  char id0[KBC_MAX_ID_LEN + 1] = {0};
  char at0[64] = {0};
  char anchor0[32] = {0};
  if (before != NULL) {
    KBC_CHECK_OK(kbc_store_set_comment_resolved(s, before[0].id, true, &err));
    const size_t li = strlen(before[0].id);
    if (li <= KBC_MAX_ID_LEN) memcpy(id0, before[0].id, li + 1u);
    const size_t la = strlen(before[0].created_at);
    if (la < sizeof at0) memcpy(at0, before[0].created_at, la + 1u);
    const size_t lan = strlen(before[0].anchor);
    if (lan < sizeof anchor0) memcpy(anchor0, before[0].anchor, lan + 1u);
  }

  /* The destination exists, because a rekey onto a path with no artifact row
   * would be re-derivable by re-ingest and would not exercise the carry. */
  kbc_artifact b;
  fill(&b, "bbbbbbbbbbbb", "kb", "new.md", KBC_KIND_ARTIFACT);
  KBC_CHECK_OK(kbc_store_upsert_artifact(s, &b, &err));

  KBC_CHECK_OK(kbc_store_rekey_artifact(s, "aaaaaaaaaaaa", "bbbbbbbbbbbb",
                                        "old.md", "new.md", &err));

  /* The comments are on the new id, and the resolved one is still the SAME
   * comment: same id, same created_at, same anchor, still resolved. Located
   * by anchor, so the assertion is about identity and not about list order. */
  kbc_comment *after = NULL;
  size_t n_after = 0;
  KBC_CHECK_OK(kbc_store_list_comments(s, ar, "bbbbbbbbbbbb", 16, &after,
                                       &n_after, &err));
  KBC_CHECK_EQ_INT(n_after, 2);
  KBC_CHECK(after != NULL);
  if (after != NULL) {
    const kbc_comment *carried = NULL;
    for (size_t i = 0; i < n_after; i++) {
      if (strcmp(after[i].anchor, anchor0) == 0) carried = &after[i];
    }
    KBC_CHECK_MSG(carried != NULL,
                  "the comment anchored at \"%s\" is gone after the rekey",
                  anchor0);
    if (carried != NULL) {
      KBC_CHECK_EQ_STR(carried->id, id0);
      KBC_CHECK_EQ_STR(carried->created_at, at0);
      KBC_CHECK_MSG(carried->resolved,
                    "the resolved flag did not survive the rekey, so a "
                    "user's decision about a thread was lost by a rename");
    }
  }
  /* And none are left behind on the dead id. */
  kbc_comment *orphan = NULL;
  size_t n_orphan = 0;
  KBC_CHECK_OK(
      kbc_store_list_comments(s, ar, "aaaaaaaaaaaa", 16, &orphan, &n_orphan,
                              &err));
  KBC_CHECK_EQ_INT(n_orphan, 0);

  kbc_arena_free(ar);
  kbc_store_close(s);
  kbc_test_rmrf(root);
}

/* Every artifact-referencing row moves, and the parent row's id AND path move
 * with it. The path half is not cosmetic: an artifact id is minted from
 * (corpus, path), so leaving the path behind would leave a row whose id does
 * not match its own path — the invariant the whole id scheme rests on. */
KBC_TEST(a_rekey_moves_the_artifact_row_and_every_table_that_names_it) {
  char root[KBC_TEST_PATH_MAX];
  kbc_test_tmpdir(root, sizeof root);
  kbc_err err;
  kbc_err_reset(&err);
  kbc_store *s = open_at(root, "kb.db", &err);
  KBC_CHECK_NOT_NULL(s);
  if (s == NULL) {
    kbc_test_rmrf(root);
    return;
  }

  kbc_artifact a;
  fill(&a, "aaaaaaaaaaaa", "kb", "old.md", KBC_KIND_ARTIFACT);
  KBC_CHECK_OK(kbc_store_upsert_artifact(s, &a, &err));
  kbc_chunk_in ck = {"aaaaaaaaaaaa", 0, "the chunk text", 14};
  KBC_CHECK_OK(kbc_store_replace_chunks(s, &ck, 1, &err));
  KBC_CHECK_OK(kbc_store_add_corkboard(s, "aaaaaaaaaaaa", 1000, &err));
  KBC_CHECK_OK(kbc_store_pin_memory(s, "aaaaaaaaaaaa", 1000, &err));
  KBC_CHECK_OK(kbc_store_first_seen(s, "aaaaaaaaaaaa", 1000, &err));
  kbc_history_row h = {0};
  h.kind = "open";
  h.artifact_id = "aaaaaaaaaaaa";
  h.started_at = 1000;
  h.updated_at = 1000;
  KBC_CHECK_OK(kbc_store_add_history(s, &h, &err));
  static const char *const out[] = {"other.md"};
  KBC_CHECK_OK(kbc_store_replace_edges(s, "kb", "old.md", out, 1, &err));
  static const char *const pend[] = {"later.md"};
  KBC_CHECK_OK(kbc_store_add_pending_links(s, "kb", "old.md", pend, 1, &err));
  /* A link INTO the document, naming the old path, written before the move
   * so the rekey is what has to carry it. */
  static const char *const into[] = {"old.md"};
  KBC_CHECK_OK(kbc_store_replace_edges(s, "kb", "linker.md", into, 1, &err));

  KBC_CHECK_OK(kbc_store_rekey_artifact(s, "aaaaaaaaaaaa", "bbbbbbbbbbbb",
                                        "old.md", "new.md", &err));

  /* The parent row, id and path together. */
  kbc_arena *ar = kbc_arena_new(8192);
  kbc_artifact got;
  memset(&got, 0, sizeof got);
  KBC_CHECK_OK(kbc_store_get_artifact(s, ar, "bbbbbbbbbbbb", false, &got, &err));
  KBC_CHECK_EQ_STR(got.path, "new.md");
  KBC_CHECK_EQ_STR(got.corpus, "kb");

  kbc_block blocks[8];
  size_t nb = 0;
  KBC_CHECK_OK(kbc_store_list_chunks(s, ar, "bbbbbbbbbbbb", blocks, &nb, &err));
  KBC_CHECK_MSG(nb == 1, "the chunk did not follow the rekey (nb=%zu)", nb);
  if (nb == 1) KBC_CHECK_EQ_STR(blocks[0].text, "the chunk text");

  int64_t first_seen = 0;
  KBC_CHECK_OK(kbc_store_get_first_seen(s, "bbbbbbbbbbbb", &first_seen, &err));
  KBC_CHECK_MSG(first_seen == 1000,
                "the first-indexed anchor moved to \"now\" (%lld), so a "
                "\"created\" sort would date the document to its rename",
                (long long)first_seen);

  kbc_corkboard_row *cork = NULL;
  size_t nc = 0;
  KBC_CHECK_OK(kbc_store_list_corkboard(s, ar, 16, &cork, &nc, &err));
  KBC_CHECK_EQ_INT(nc, 1);
  KBC_CHECK(cork != NULL);
  if (cork != NULL) {
    KBC_CHECK_EQ_STR(cork[0].artifact_id, "bbbbbbbbbbbb");
    KBC_CHECK_MSG(cork[0].created_at == 1000, "the anchor lost its timestamp");
  }
  kbc_pin_row *pins = NULL;
  size_t np = 0;
  KBC_CHECK_OK(kbc_store_list_pins(s, ar, 16, &pins, &np, &err));
  KBC_CHECK_EQ_INT(np, 1);
  KBC_CHECK(pins != NULL);
  if (pins != NULL) KBC_CHECK_EQ_STR(pins[0].artifact_id, "bbbbbbbbbbbb");

  kbc_history_row *hist = NULL;
  size_t nh = 0;
  KBC_CHECK_OK(kbc_store_list_history(s, ar, NULL, 16, &hist, &nh, &err));
  KBC_CHECK_MSG(nh == 1, "the reading visit was lost by a rename (nh=%zu)",
                nh);
  KBC_CHECK(hist != NULL);
  if (hist != NULL)
    KBC_CHECK_EQ_STR(hist[0].artifact_id, "bbbbbbbbbbbb");

  /* The graph, both directions.
   *
   * `linker.md -> old.md` was written BEFORE the rekey, so it is an inbound
   * edge naming the old path: a move must carry it to the new one, or every
   * bookmarked link INTO the document breaks the moment it is renamed. */
  kbc_strlist back;
  kbc_strlist_init(&back);
  KBC_CHECK_OK(kbc_store_list_backlinks(s, "kb", "new.md", &back, &err));
  KBC_CHECK_MSG(back.len == 1 && strcmp(back.items[0], "linker.md") == 0,
                "an inbound link to the old path did not follow the move "
                "(len=%zu)",
                back.len);
  kbc_strlist_free(&back);

  /* The moved document's OWN outbound edge, which is owned by its src_path
   * and so moves with the document. */
  kbc_strlist srcs;
  kbc_strlist_init(&srcs);
  KBC_CHECK_OK(kbc_store_list_backlinks(s, "kb", "other.md", &srcs, &err));
  KBC_CHECK_MSG(srcs.len == 1 && strcmp(srcs.items[0], "new.md") == 0,
                "the moved document's own outbound edge stayed on the old "
                "path");
  kbc_strlist_free(&srcs);
  KBC_CHECK_MSG(kbc_store_pending_count(s, &err) == 1,
                "the pending link was dropped by the rekey");

  kbc_arena_free(ar);
  kbc_store_close(s);
  kbc_test_rmrf(root);
}

/* A stale id follows a CHAIN, not a single hop. This is the property that
 * makes the redirect work for a document renamed twice: a bookmark made
 * before the first rename has to end up at the third name, and answering the
 * second would hand the caller a name that was itself renamed away.
 *
 * The mutation that must fail this: a single-hop lookup (LIMIT 1 with no
 * follow-up), which would return "b" here. */
KBC_TEST(a_stale_id_follows_a_chain_of_renames_to_its_final_home) {
  char root[KBC_TEST_PATH_MAX];
  kbc_test_tmpdir(root, sizeof root);
  kbc_err err;
  kbc_err_reset(&err);
  kbc_store *s = open_at(root, "kb.db", &err);
  KBC_CHECK_NOT_NULL(s);
  if (s == NULL) {
    kbc_test_rmrf(root);
    return;
  }

  /* a -> b -> c, every hop completed. */
  KBC_CHECK_OK(kbc_store_record_move(s, "aaaaaaaaaaaa", "bbbbbbbbbbbb",
                                     "a.md", "b.md", 1000, &err));
  KBC_CHECK_OK(kbc_store_complete_move(s, "aaaaaaaaaaaa", &err));
  KBC_CHECK_OK(kbc_store_record_move(s, "bbbbbbbbbbbb", "cccccccccccc",
                                     "b.md", "c.md", 2000, &err));
  KBC_CHECK_OK(kbc_store_complete_move(s, "bbbbbbbbbbbb", &err));

  kbc_strlist ids;
  kbc_strlist_init(&ids);
  KBC_CHECK_OK(kbc_store_moves_lookup(s, "aaaaaaaaaaaa", &ids, &err));
  KBC_CHECK_MSG(ids.len == 2,
                "a two-hop chain returned %zu hops; a stale id must reach its "
                "FINAL home, not the next name along",
                ids.len);
  if (ids.len == 2) {
    KBC_CHECK_EQ_STR(ids.items[0], "bbbbbbbbbbbb");
    KBC_CHECK_MSG(strcmp(ids.items[1], "cccccccccccc") == 0,
                  "the last hop is not the final id");
  }
  kbc_strlist_free(&ids);

  /* The middle id also resolves, to the last hop only. */
  kbc_strlist_init(&ids);
  KBC_CHECK_OK(kbc_store_moves_lookup(s, "bbbbbbbbbbbb", &ids, &err));
  KBC_CHECK_EQ_INT((long long)ids.len, 1);
  if (ids.len == 1) KBC_CHECK_EQ_STR(ids.items[0], "cccccccccccc");
  kbc_strlist_free(&ids);

  /* An id that never moved is an EMPTY LIST AND KBC_OK, not an error: "this
   * document was never renamed" is the common answer and the overwhelmingly
   * more useful one. */
  kbc_strlist_init(&ids);
  kbc_status st = kbc_store_moves_lookup(s, "dddddddddddd", &ids, &err);
  KBC_CHECK_MSG(st == KBC_OK, "an unmoved id reported %s: %s",
                kbc_status_str(st), err.msg);
  KBC_CHECK_EQ_INT((long long)ids.len, 0);
  kbc_strlist_free(&ids);

  /* And the same walk keyed by path, because a link is a name in a file. */
  kbc_strlist rels;
  kbc_strlist_init(&rels);
  KBC_CHECK_OK(kbc_store_moves_lookup_path(s, "a.md", &rels, &err));
  KBC_CHECK_EQ_INT((long long)rels.len, 2);
  if (rels.len == 2) {
    KBC_CHECK_EQ_STR(rels.items[0], "b.md");
    KBC_CHECK_EQ_STR(rels.items[1], "c.md");
  }
  kbc_strlist_free(&rels);

  kbc_store_close(s);
  kbc_test_rmrf(root);
}

/* An INTERRUPTED move is not a redirect. Its rename never completed, so its
 * new_id is a name the document may never have reached; following it would
 * resolve a bookmark to a document that does not exist, which is a worse
 * answer than leaving the id alone.
 *
 * It must also show up in the incomplete list, because that is the bring-up
 * pass's only way to learn the rename is outstanding. */
KBC_TEST(an_interrupted_move_is_listed_for_replay_and_never_followed) {
  char root[KBC_TEST_PATH_MAX];
  kbc_test_tmpdir(root, sizeof root);
  kbc_err err;
  kbc_err_reset(&err);
  kbc_store *s = open_at(root, "kb.db", &err);
  KBC_CHECK_NOT_NULL(s);
  if (s == NULL) {
    kbc_test_rmrf(root);
    return;
  }

  /* One completed move and one that never finished. */
  KBC_CHECK_OK(kbc_store_record_move(s, "aaaaaaaaaaaa", "bbbbbbbbbbbb",
                                     "a.md", "b.md", 1000, &err));
  KBC_CHECK_OK(kbc_store_complete_move(s, "aaaaaaaaaaaa", &err));
  KBC_CHECK_OK(kbc_store_record_move(s, "cccccccccccc", "dddddddddddd",
                                     "c.md", "d.md", 2000, &err));

  /* The interrupted one does not redirect. */
  kbc_strlist ids;
  kbc_strlist_init(&ids);
  KBC_CHECK_OK(kbc_store_moves_lookup(s, "cccccccccccc", &ids, &err));
  KBC_CHECK_MSG(ids.len == 0,
                "an interrupted move was followed to %s — a name the rename "
                "never reached",
                ids.len > 0 ? ids.items[0] : "?");
  kbc_strlist_free(&ids);

  /* Both lists are parallel: entry i pairs with entry i. */
  kbc_strlist old_ids, old_rels;
  kbc_strlist_init(&old_ids);
  kbc_strlist_init(&old_rels);
  KBC_CHECK_OK(kbc_store_list_incomplete_moves(s, &old_ids, &old_rels, &err));
  KBC_CHECK_MSG(old_ids.len == 1, "the interrupted move was not listed (%zu)",
                old_ids.len);
  KBC_CHECK_EQ_INT((long long)old_ids.len, (long long)old_rels.len);
  if (old_ids.len == 1 && old_rels.len == 1) {
    KBC_CHECK_EQ_STR(old_ids.items[0], "cccccccccccc");
    KBC_CHECK_EQ_STR(old_rels.items[0], "c.md");
  }
  kbc_strlist_free(&old_ids);
  kbc_strlist_free(&old_rels);

 /* Once the rename is finished the row leaves the replay list. */
  KBC_CHECK_OK(kbc_store_complete_move(s, "cccccccccccc", &err));
  kbc_strlist_init(&old_ids);
  kbc_strlist_init(&old_rels);
  KBC_CHECK_OK(kbc_store_list_incomplete_moves(s, &old_ids, &old_rels, &err));
  KBC_CHECK_MSG(old_ids.len == 0,
                "a completed move is still queued for replay");
  kbc_strlist_free(&old_ids);
  kbc_strlist_free(&old_rels);

  /* And it redirects once it is complete. */
  kbc_strlist_init(&ids);
  KBC_CHECK_OK(kbc_store_moves_lookup(s, "cccccccccccc", &ids, &err));
  KBC_CHECK_EQ_INT((long long)ids.len, 1);
  kbc_strlist_free(&ids);

  kbc_store_close(s);
  kbc_test_rmrf(root);
}

/* The walk must TERMINATE on a cycle, and a cycle is representable: nothing
 * in the schema forbids a -> b -> a, and a buggy writer is exactly the case
 * the bound exists for. Without the guard this is an httpd worker that never
 * answers and a bring-up pass that never finishes.
 *
 * The rows are written through raw SQL because the public API records a move
 * in one direction at a time and the test needs both directions of a loop in
 * one volume. */
KBC_TEST(a_cyclic_move_chain_terminates_instead_of_hanging) {
  char root[KBC_TEST_PATH_MAX];
  kbc_test_tmpdir(root, sizeof root);
  kbc_err err;
  kbc_err_reset(&err);
  kbc_store *s = open_at(root, "kb.db", &err);
  KBC_CHECK_NOT_NULL(s);
  if (s == NULL) {
    kbc_test_rmrf(root);
    return;
  }
  kbc_store_close(s);

  /* a -> b -> a, both completed. A single-hop lookup would return "b" and
 * stop; an unguarded walk would go round forever. */
  sqlite3 *raw = raw_open(root, "kb.db");
  KBC_CHECK_NOT_NULL(raw);
  if (raw == NULL) {
    kbc_test_rmrf(root);
    return;
  }
  raw_exec(raw,
           "INSERT INTO moves(old_id, new_id, old_rel, new_rel, moved_at,"
           " completed_at) VALUES('aaaaaaaaaaaa','bbbbbbbbbbbb','a.md','b.md',"
           "1,10);"
           "INSERT INTO moves(old_id, new_id, old_rel, new_rel, moved_at,"
           " completed_at) VALUES('bbbbbbbbbbbb','aaaaaaaaaaaa','b.md','a.md',"
           "2,20);");
  (void)sqlite3_close(raw);

  s = open_at(root, "kb.db", &err);
  KBC_CHECK_NOT_NULL(s);
  if (s == NULL) {
    kbc_test_rmrf(root);
    return;
  }
  kbc_strlist ids;
  kbc_strlist_init(&ids);
  kbc_status st = kbc_store_moves_lookup(s, "aaaaaaaaaaaa", &ids, &err);
  /* The point of the case is that this RETURNS. */
  KBC_CHECK_MSG(st == KBC_OK, "the walk did not terminate: %s",
                kbc_status_str(st));
  KBC_CHECK_MSG(ids.len <= 4, "the walk did not stop; it reported %zu hops",
                ids.len);
  kbc_strlist_free(&ids);

  kbc_store_close(s);
  kbc_test_rmrf(root);
}

/* ONE TRANSACTION PER VERSION, observed rather than argued. A `history`
 * table of a shape the v6 step cannot index makes the ladder die on version 6
 * and not before it: the recorded version must then be 5 — the last version
 * that fully committed — with v5's tables on disk and v7's absent. A ladder
 * that wrapped every step in one transaction would report 4 and leave
 * nothing, and a ladder that recorded the version before running the step
 * would report 6 over a schema that does not have it. */
KBC_TEST(a_failing_migration_keeps_the_last_committed_version) {
  char root[KBC_TEST_PATH_MAX];
  kbc_test_tmpdir(root, sizeof root);
  sqlite3 *raw = raw_open(root, "stuck.db");
  KBC_CHECK_NOT_NULL(raw);
  if (raw == NULL) {
    kbc_test_rmrf(root);
    return;
  }
  raw_exec(raw, V4_DB);
  raw_exec(raw, "CREATE TABLE history (id INTEGER PRIMARY KEY);");
  (void)sqlite3_close(raw);

  kbc_err err;
  kbc_err_reset(&err);
  kbc_store *s = open_at(root, "stuck.db", &err);
  KBC_CHECK_MSG(s == NULL, "a migration that fails must not hand back a store");
  KBC_CHECK_ERR_MSG(err);
  kbc_store_close(s);

  raw = raw_open(root, "stuck.db");
  KBC_CHECK_NOT_NULL(raw);
  if (raw != NULL) {
    KBC_CHECK_EQ_INT(raw_version(raw), 5);
    KBC_CHECK_EQ_INT(raw_version_rows(raw), 5);
    KBC_CHECK_MSG(table_exists(raw, "sources"),
                  "version 5 committed and must have survived");
    KBC_CHECK_MSG(!table_exists(raw, "corkboard"),
                  "version 7 never ran and must not be recorded");
    (void)sqlite3_close(raw);
  }
  kbc_test_rmrf(root);
}

/* The epoch guard. The refusal is for a volume AHEAD of this binary and for
 * nothing else: equal and behind are the two states every ordinary open is
 * in, and a guard that refused them would brick every valid volume. */
KBC_TEST(the_epoch_guard_refuses_a_volume_ahead_and_nothing_else) {
  char root[KBC_TEST_PATH_MAX];
  kbc_test_tmpdir(root, sizeof root);
  kbc_err err;
  kbc_err_reset(&err);
  kbc_store *s = open_at(root, "ahead.db", &err);
  KBC_CHECK_NOT_NULL(s);
  if (s == NULL) {
    kbc_test_rmrf(root);
    return;
  }
  kbc_artifact art;
  fill(&art, "aaaaaaaaaaaa", "kb", "a.md", KBC_KIND_ARTIFACT);
  KBC_CHECK_OK(kbc_store_upsert_artifact(s, &art, &err));
  kbc_store_close(s);

  /* A volume a newer binary already migrated: the recorded version is
   * AHEAD of everything this binary knows how to produce. */
  sqlite3 *raw = raw_open(root, "ahead.db");
  KBC_CHECK_NOT_NULL(raw);
  if (raw == NULL) {
    kbc_test_rmrf(root);
    return;
  }
  raw_exec(raw, "DELETE FROM schema_version;");
  {
    char sql[128];
    (void)snprintf(sql, sizeof sql,
                   "INSERT INTO schema_version(version) VALUES(%d);",
                   CURRENT_SCHEMA + 1);
    raw_exec(raw, sql);
  }
  (void)sqlite3_close(raw);

  kbc_err_reset(&err);
  s = open_at(root, "ahead.db", &err);
  KBC_CHECK_MSG(s == NULL, "a volume ahead must be refused, not opened");
  KBC_CHECK_EQ_INT(err.status, KBC_ERR_CONFLICT);
  KBC_CHECK_ERR_MSG(err);
  /* Both epochs are named: an operator has to be able to tell which binary to
   * deploy without reading the source. */
  {
    char want[64];
    (void)snprintf(want, sizeof want, "V%d", CURRENT_SCHEMA + 1);
    KBC_CHECK_MSG(strstr(err.msg, want) != NULL, "no volume epoch in: %s",
                  err.msg);
    (void)snprintf(want, sizeof want, "V%d", CURRENT_SCHEMA);
    KBC_CHECK_MSG(strstr(err.msg, want) != NULL, "no binary epoch in: %s",
                  err.msg);
  }

  /* A refused open must not have touched the volume: not the recorded
   * version, not the rows. A guard that ran the migrations first would leave
   * the version rewritten. */
  raw = raw_open(root, "ahead.db");
  KBC_CHECK_NOT_NULL(raw);
  if (raw != NULL) {
    KBC_CHECK_EQ_INT(raw_version(raw), CURRENT_SCHEMA + 1);
    sqlite3_stmt *q = NULL;
    int64_t rows = 0;
    if (sqlite3_prepare_v2(raw, "SELECT COUNT(*) FROM artifacts;", -1, &q,
                           NULL) == SQLITE_OK &&
        sqlite3_step(q) == SQLITE_ROW) {
      rows = (int64_t)sqlite3_column_int64(q, 0);
    }
    (void)sqlite3_finalize(q);
    KBC_CHECK_EQ_INT(rows, 1);
    (void)sqlite3_close(raw);
  }

  /* Behind: an open that has to migrate forward, which is the normal case. */
  raw = raw_open(root, "ahead.db");
  if (raw != NULL) {
    raw_exec(raw, "DELETE FROM schema_version;");
    {
      char sql[128];
      (void)snprintf(sql, sizeof sql,
                     "INSERT INTO schema_version(version) VALUES(%d);",
                     CURRENT_SCHEMA - 1);
      raw_exec(raw, sql);
    }
    (void)sqlite3_close(raw);
  }
  kbc_err_reset(&err);
  s = open_at(root, "ahead.db", &err);
  KBC_CHECK_MSG(s != NULL, "a volume behind must open: %s", err.msg);
  if (s != NULL) {
    KBC_CHECK_EQ_INT(kbc_store_schema_version(s), CURRENT_SCHEMA);
    kbc_arena *a = kbc_arena_new(4096);
    kbc_artifact again;
    memset(&again, 0, sizeof again);
    KBC_CHECK_OK(
        kbc_store_get_artifact(s, a, "aaaaaaaaaaaa", false, &again, &err));
    KBC_CHECK_EQ_STR(again.path, "a.md");
    kbc_arena_free(a);
    kbc_store_close(s);
  }

  /* Equal: nothing to do, and still an open. */
  kbc_err_reset(&err);
  s = open_at(root, "ahead.db", &err);
  KBC_CHECK_MSG(s != NULL, "a volume at the binary's epoch must open: %s",
                err.msg);
  if (s != NULL) {
    KBC_CHECK_EQ_INT(kbc_store_schema_version(s), CURRENT_SCHEMA);
    kbc_store_close(s);
  }
  kbc_test_rmrf(root);
}

/* `added_at` is when the source was FIRST seen. An update that moved it would
 * make every re-adoption of a corpus look like a fresh source, and a pause
 * that a re-ingest could undo would not be a pause. */
KBC_TEST(a_source_keeps_the_moment_it_was_first_seen) {
  char root[KBC_TEST_PATH_MAX];
  kbc_test_tmpdir(root, sizeof root);
  kbc_err err;
  kbc_err_reset(&err);
  kbc_store *s = open_at(root, "src.db", &err);
  KBC_CHECK_NOT_NULL(s);
  if (s == NULL) {
    kbc_test_rmrf(root);
    return;
  }
  kbc_arena *a = kbc_arena_new(8192);

  const kbc_source first = {"kb", "/corpus/one", 1000, false};
  KBC_CHECK_OK(kbc_store_put_source(s, &first, &err));
  kbc_source got;
  memset(&got, 0, sizeof got);
  KBC_CHECK_OK(kbc_store_get_source(s, a, "kb", &got, &err));
  KBC_CHECK_EQ_STR(got.path, "/corpus/one");
  KBC_CHECK_EQ_INT(got.added_at, 1000);
  KBC_CHECK_EQ_INT(got.paused, 0);

  KBC_CHECK_OK(kbc_store_set_source_paused(s, "kb", true, &err));
  const kbc_source again = {"kb", "/corpus/moved", 9999, false};
  KBC_CHECK_OK(kbc_store_put_source(s, &again, &err));
  KBC_CHECK_OK(kbc_store_get_source(s, a, "kb", &got, &err));
  KBC_CHECK_EQ_STR(got.path, "/corpus/moved"); /* the path does move */
  KBC_CHECK_EQ_INT(got.added_at, 1000);         /* the moment does not */
  KBC_CHECK_EQ_INT(got.paused, 1);              /* and a pause survives */

  const kbc_source other = {"other", "/elsewhere", 2000, false};
  KBC_CHECK_OK(kbc_store_put_source(s, &other, &err));
  kbc_source *rows = NULL;
  size_t n = 0;
  KBC_CHECK_OK(kbc_store_list_sources(s, a, 10, &rows, &n, &err));
  KBC_CHECK_EQ_INT(n, 2);
  if (n == 2) {
    KBC_CHECK_EQ_STR(rows[0].corpus, "other"); /* newest added first */
    KBC_CHECK_EQ_STR(rows[1].corpus, "kb");
  }
  rows = NULL;
  n = 0;
  KBC_CHECK_OK(kbc_store_list_sources(s, a, 1, &rows, &n, &err));
  KBC_CHECK_EQ_INT(n, 1);
  KBC_CHECK_OK(kbc_store_list_sources(s, a, 0, &rows, &n, &err));
  KBC_CHECK_EQ_INT(n, 0);
  KBC_CHECK_NULL(rows);

  /* A slug that is not there says so, by name. */
  kbc_err_reset(&err);
  KBC_CHECK_ERR(kbc_store_get_source(s, a, "nope", &got, &err),
                KBC_ERR_NOTFOUND);
  KBC_CHECK_ERR_MSG(err);
  KBC_CHECK_ERR(kbc_store_set_source_paused(s, "nope", true, &err),
                KBC_ERR_NOTFOUND);
  /* Two corpora cannot claim one directory: `path` is UNIQUE upstream, and
   * "/corpus/one" is free again only because `kb` moved off it. */
  const kbc_source clash = {"third", "/corpus/moved", 3000, false};
  KBC_CHECK_ERR(kbc_store_put_source(s, &clash, &err), KBC_ERR_CONFLICT);
  KBC_CHECK_ERR_MSG(err);

  kbc_arena_free(a);
  kbc_store_close(s);
  kbc_test_rmrf(root);
}

/* An in-flight run has to be readable AS in flight, and finished as finished:
 * the whole point of the column is that a caller can tell the two apart. */
KBC_TEST(an_index_run_reads_back_as_in_flight_until_it_is_finished) {
  char root[KBC_TEST_PATH_MAX];
  kbc_test_tmpdir(root, sizeof root);
  kbc_err err;
  kbc_err_reset(&err);
  kbc_store *s = open_at(root, "run.db", &err);
  KBC_CHECK_NOT_NULL(s);
  if (s == NULL) {
    kbc_test_rmrf(root);
    return;
  }
  kbc_arena *a = kbc_arena_new(8192);
  const kbc_source src = {"kb", "/corpus", 1000, false};
  KBC_CHECK_OK(kbc_store_put_source(s, &src, &err));

  const kbc_index_run run = {"r-aaaaaa", "kb", 2000, -1, 0, 0};
  KBC_CHECK_OK(kbc_store_put_index_run(s, &run, &err));
  kbc_index_run *rows = NULL;
  size_t n = 0;
  KBC_CHECK_OK(kbc_store_list_index_runs(s, a, "kb", 10, &rows, &n, &err));
  KBC_CHECK_EQ_INT(n, 1);
  if (n == 1) {
    KBC_CHECK_EQ_STR(rows[0].id, "r-aaaaaa");
    KBC_CHECK_MSG(rows[0].finished_at < 0, "an unfinished run must read back "
                                          "negative, got %lld",
                  (long long)rows[0].finished_at);
  }

  KBC_CHECK_OK(kbc_store_finish_index_run(s, "r-aaaaaa", 2500, 7, 2, &err));
  rows = NULL;
  n = 0;
  KBC_CHECK_OK(kbc_store_list_index_runs(s, a, "kb", 10, &rows, &n, &err));
  KBC_CHECK_EQ_INT(n, 1);
  if (n == 1) {
    KBC_CHECK_EQ_INT(rows[0].finished_at, 2500);
    KBC_CHECK_EQ_INT(rows[0].ok_count, 7);
    KBC_CHECK_EQ_INT(rows[0].err_count, 2);
  }

  /* A second pass, newer, still running — and passing a different negative
   * must not mint a second in-flight value for a reader to miss. */
  const kbc_index_run later = {"r-bbbbbb", "kb", 3000, -99, 0, 0};
  KBC_CHECK_OK(kbc_store_put_index_run(s, &later, &err));
  rows = NULL;
  n = 0;
  KBC_CHECK_OK(kbc_store_list_index_runs(s, a, "kb", 10, &rows, &n, &err));
  KBC_CHECK_EQ_INT(n, 2);
  if (n == 2) {
    KBC_CHECK_EQ_STR(rows[0].id, "r-bbbbbb"); /* newest pass first */
    KBC_CHECK_EQ_INT(rows[0].finished_at, -1);
    KBC_CHECK_EQ_STR(rows[1].id, "r-aaaaaa");
  }
  rows = NULL;
  n = 0;
  KBC_CHECK_OK(kbc_store_list_index_runs(s, a, NULL, 10, &rows, &n, &err));
  KBC_CHECK_EQ_INT(n, 2); /* NULL corpus is every source */
  rows = NULL;
  n = 0;
  KBC_CHECK_OK(kbc_store_list_index_runs(s, a, "elsewhere", 10, &rows, &n,
                                         &err));
  KBC_CHECK_EQ_INT(n, 0);

  /* A finished_at that is negative would make a finished run indistinguishable
   * from a running one. */
  kbc_err_reset(&err);
  KBC_CHECK_ERR(kbc_store_finish_index_run(s, "r-bbbbbb", -1, 1, 0, &err),
                KBC_ERR_INVALID);
  KBC_CHECK_ERR_MSG(err);
  KBC_CHECK_ERR(kbc_store_finish_index_run(s, "r-zzzzzz", 1, 0, 0, &err),
                KBC_ERR_NOTFOUND);
  /* A run is always about a source that exists: the foreign key says so. */
  const kbc_index_run orphan = {"r-ccccc", "nosuch", 1, -1, 0, 0};
  KBC_CHECK_ERR(kbc_store_put_index_run(s, &orphan, &err), KBC_ERR_CONFLICT);
  KBC_CHECK_ERR_MSG(err);

  kbc_arena_free(a);
  kbc_store_close(s);
  kbc_test_rmrf(root);
}

/* The quarantine gate's input. A second failure of the same path must MOVE a
 * counter, not add a row: a store that duplicated would let a document that
 * fails forever be embedded forever. */
KBC_TEST(a_repeated_failure_counts_retries_instead_of_duplicating_the_row) {
  char root[KBC_TEST_PATH_MAX];
  kbc_test_tmpdir(root, sizeof root);
  kbc_err err;
  kbc_err_reset(&err);
  kbc_store *s = open_at(root, "err.db", &err);
  KBC_CHECK_NOT_NULL(s);
  if (s == NULL) {
    kbc_test_rmrf(root);
    return;
  }
  kbc_arena *a = kbc_arena_new(8192);
  kbc_error_row row = {0};
  row.id = "e-aaaaaa";
  row.kind = "parse";
  row.corpus = "kb";
  row.path = "bad.md";
  row.message = "unclosed fence";
  row.created_at = 1000;
  KBC_CHECK_OK(kbc_store_record_error(s, &row, &err));

  int64_t retries = -1;
  KBC_CHECK_OK(kbc_store_retry_count_for_path(s, "kb", "bad.md", NULL, &retries,
                                              &err));
  KBC_CHECK_EQ_INT(retries, 0); /* a first failure starts the count at zero */

  row.id = "e-bbbbbb";
  row.message = "unclosed fence at line 9";
  row.created_at = 2000;
  KBC_CHECK_OK(kbc_store_record_error(s, &row, &err));
  KBC_CHECK_OK(kbc_store_retry_count_for_path(s, "kb", "bad.md", NULL, &retries,
                                              &err));
  KBC_CHECK_EQ_INT(retries, 1);

  /* ONE row, carrying the newer message and the newer time. */
  kbc_error_row *rows = NULL;
  size_t n = 0;
  KBC_CHECK_OK(kbc_store_list_errors(s, a, "kb", false, 10, &rows, &n, &err));
  KBC_CHECK_EQ_INT(n, 1);
  if (n == 1) {
    KBC_CHECK_EQ_STR(rows[0].message, "unclosed fence at line 9");
    KBC_CHECK_EQ_INT(rows[0].created_at, 2000);
    KBC_CHECK_EQ_INT(rows[0].retry_count, 1);
  }

  row.id = "e-cccccc";
  KBC_CHECK_OK(kbc_store_record_error(s, &row, &err));
  KBC_CHECK_OK(kbc_store_retry_count_for_path(s, "kb", "bad.md", NULL, &retries,
                                              &err));
  KBC_CHECK_EQ_INT(retries, 2);

  /* A different path, and the same path under a different corpus, are
   * different failures: neither may inherit the other's count. */
  row.id = "e-dddddd";
  row.path = "worse.md";
  row.created_at = 3000; /* its own clock, so "newest first" is decidable */
  KBC_CHECK_OK(kbc_store_record_error(s, &row, &err));
  KBC_CHECK_OK(kbc_store_retry_count_for_path(s, "kb", "worse.md", NULL, &retries,
                                              &err));
  KBC_CHECK_EQ_INT(retries, 0);
  row.id = "e-eeeeee";
  row.path = "bad.md";
  row.corpus = "other";
  KBC_CHECK_OK(kbc_store_record_error(s, &row, &err));
  KBC_CHECK_OK(kbc_store_retry_count_for_path(s, "other", "bad.md", NULL, &retries,
                                              &err));
  KBC_CHECK_EQ_INT(retries, 0);
  rows = NULL;
  n = 0;
  KBC_CHECK_OK(kbc_store_list_errors(s, a, "kb", false, 10, &rows, &n, &err));
  KBC_CHECK_EQ_INT(n, 2);

  /* A path nobody ever failed on reads as zero, not as an error. */
  KBC_CHECK_OK(
      kbc_store_retry_count_for_path(s, "kb", "fine.md", NULL, &retries,
                                              &err));
  KBC_CHECK_EQ_INT(retries, 0);

  /* Clearing hands the document a fresh budget, and the dismissed row stays
   * on disk: the history of a failure is what an operator reads. */
  KBC_CHECK_OK(kbc_store_clear_error(s, "kb", "bad.md", &err));
  KBC_CHECK_OK(kbc_store_retry_count_for_path(s, "kb", "bad.md", NULL, &retries,
                                              &err));
  KBC_CHECK_EQ_INT(retries, 0);
  rows = NULL;
  n = 0;
  KBC_CHECK_OK(kbc_store_list_errors(s, a, "kb", false, 10, &rows, &n, &err));
  KBC_CHECK_EQ_INT(n, 2);
  if (n == 2) {
    /* Newest first: the failure recorded at 3000 leads, the dismissed one
     * from 2000 follows with its count intact and its own history kept. */
    KBC_CHECK_EQ_STR(rows[0].path, "worse.md");
    KBC_CHECK_EQ_INT(rows[0].retry_count, 0);
    KBC_CHECK_EQ_INT(rows[0].dismissed, 0);
    KBC_CHECK_EQ_STR(rows[1].path, "bad.md");
    KBC_CHECK_EQ_INT(rows[1].retry_count, 2);
    KBC_CHECK_EQ_INT(rows[1].dismissed, 1);
  }
  kbc_err_reset(&err);
  KBC_CHECK_ERR(kbc_store_clear_error(s, "kb", "bad.md", &err),
                KBC_ERR_NOTFOUND);
  KBC_CHECK_ERR_MSG(err);

  kbc_arena_free(a);
  kbc_store_close(s);
  kbc_test_rmrf(root);
}

/* The open-errors surface is the query idx_errors_open exists to serve: only
 * the rows nobody dismissed, and the newest failure first. */
KBC_TEST(the_open_errors_query_returns_only_rows_nobody_dismissed) {
  char root[KBC_TEST_PATH_MAX];
  kbc_test_tmpdir(root, sizeof root);
  kbc_err err;
  kbc_err_reset(&err);
  kbc_store *s = open_at(root, "open.db", &err);
  KBC_CHECK_NOT_NULL(s);
  if (s == NULL) {
    kbc_test_rmrf(root);
    return;
  }
  kbc_arena *a = kbc_arena_new(8192);
  static const char *const paths[] = {"a.md", "b.md", "c.md"};
  for (size_t i = 0; i < 3; i++) {
    kbc_error_row row = {0};
    row.id = i == 0 ? "e-aaaaaa" : (i == 1 ? "e-bbbbbb" : "e-cccccc");
    row.kind = "io";
    row.corpus = "kb";
    row.path = paths[i];
    row.message = "unreadable";
    row.created_at = 1000 + (int64_t)i * 100;
    KBC_CHECK_OK(kbc_store_record_error(s, &row, &err));
  }
  KBC_CHECK_OK(kbc_store_clear_error(s, "kb", "b.md", &err));

  kbc_error_row *open_rows = NULL;
  size_t n_open = 0;
  KBC_CHECK_OK(
      kbc_store_list_errors(s, a, "kb", true, 10, &open_rows, &n_open, &err));
  KBC_CHECK_EQ_INT(n_open, 2);
  if (n_open == 2) {
    KBC_CHECK_EQ_STR(open_rows[0].path, "c.md"); /* newest first */
    KBC_CHECK_EQ_STR(open_rows[1].path, "a.md");
    KBC_CHECK_EQ_INT(open_rows[0].dismissed, 0);
    KBC_CHECK_EQ_INT(open_rows[1].dismissed, 0);
  }

  kbc_error_row *all_rows = NULL;
  size_t n_all = 0;
  KBC_CHECK_OK(
      kbc_store_list_errors(s, a, "kb", false, 10, &all_rows, &n_all, &err));
  KBC_CHECK_EQ_INT(n_all, 3);
  if (n_all == 3) {
    KBC_CHECK_EQ_STR(all_rows[0].path, "c.md");
    KBC_CHECK_EQ_STR(all_rows[1].path, "b.md");
    KBC_CHECK_EQ_INT(all_rows[1].dismissed, 1); /* still on disk, just closed */
    KBC_CHECK_EQ_STR(all_rows[2].path, "a.md");
  }
  kbc_error_row *none = NULL;
  size_t n_none = 0;
  KBC_CHECK_OK(
      kbc_store_list_errors(s, a, "nowhere", true, 10, &none, &n_none, &err));
  KBC_CHECK_EQ_INT(n_none, 0);
  kbc_arena_free(a);
  kbc_store_close(s);

  /* The index is PARTIAL, and both halves of that are observable: the query
   * is served by it, and the dismissed row is not IN it. A full-text plan, or
   * an index holding all three rows, would mean the DDL lost the property it
   * was ported for. */
  sqlite3 *raw = raw_open(root, "open.db");
  KBC_CHECK_NOT_NULL(raw);
  if (raw != NULL) {
    sqlite3_stmt *q = NULL;
    bool served = false;
    if (sqlite3_prepare_v2(raw,
                           "EXPLAIN QUERY PLAN SELECT id FROM errors"
                           " WHERE corpus = ?1 AND dismissed = 0;",
                           -1, &q, NULL) == SQLITE_OK) {
      (void)sqlite3_bind_text(q, 1, "kb", -1, SQLITE_TRANSIENT);
      while (sqlite3_step(q) == SQLITE_ROW) {
        const unsigned char *detail = sqlite3_column_text(q, 3);
        if (detail != NULL &&
            strstr((const char *)detail, "idx_errors_open") != NULL) {
          served = true;
        }
      }
    }
    (void)sqlite3_finalize(q);
    KBC_CHECK_MSG(served, "the open-errors query did not use idx_errors_open");

    /* dbstat reports one row per b-tree PAGE, so the entries are the summed
     * cells. Two of the three failures are undismissed, so a partial index
     * over them holds two entries and a full one holds three. */
    q = NULL;
    int64_t indexed = -1;
    if (sqlite3_prepare_v2(raw,
                           "SELECT IFNULL(SUM(ncell), 0) FROM dbstat"
                           " WHERE name = 'idx_errors_open';",
                           -1, &q, NULL) == SQLITE_OK &&
        sqlite3_step(q) == SQLITE_ROW) {
      indexed = (int64_t)sqlite3_column_int64(q, 0);
    }
    (void)sqlite3_finalize(q);
    KBC_CHECK_MSG(indexed == 2,
                  "idx_errors_open holds %lld entries; a partial index over "
                  "the undismissed rows holds 2, a full one holds 3",
                  (long long)indexed);
    (void)sqlite3_close(raw);
  }
  kbc_test_rmrf(root);
}

/* The whole point of doc_first_seen: a reindex must not move it. A store that
 * updated the row would leave a "created" sort with nothing to sort by. */
KBC_TEST(first_seen_survives_a_reindex) {
  char root[KBC_TEST_PATH_MAX];
  kbc_test_tmpdir(root, sizeof root);
  kbc_err err;
  kbc_err_reset(&err);
  kbc_store *s = open_at(root, "seen.db", &err);
  KBC_CHECK_NOT_NULL(s);
  if (s == NULL) {
    kbc_test_rmrf(root);
    return;
  }
  kbc_artifact art;
  fill(&art, "bbbbbbbbbbbb", "kb", "a.md", KBC_KIND_ARTIFACT);
  KBC_CHECK_OK(kbc_store_upsert_artifact(s, &art, &err));
  KBC_CHECK_OK(kbc_store_first_seen(s, art.id, 1000, &err));

  int64_t first = 0;
  KBC_CHECK_OK(kbc_store_get_first_seen(s, art.id, &first, &err));
  KBC_CHECK_EQ_INT(first, 1000);

  /* The reindex: the document is written again, with a new mtime, and the
   * indexer stamps first-seen again from the clock it now believes. */
  art.mtime_ns = 1800000000000000000ll;
  art.content_hash = 0x0badc0deu;
  KBC_CHECK_OK(kbc_store_upsert_artifact(s, &art, &err));
  KBC_CHECK_OK(kbc_store_first_seen(s, art.id, 2000, &err));
  KBC_CHECK_OK(kbc_store_get_first_seen(s, art.id, &first, &err));
  KBC_CHECK_EQ_INT(first, 1000);

  kbc_err_reset(&err);
  KBC_CHECK_ERR(kbc_store_get_first_seen(s, "cccccccccccc", &first, &err),
                KBC_ERR_NOTFOUND);
  KBC_CHECK_ERR_MSG(err);
  kbc_store_close(s);
  kbc_test_rmrf(root);
}

/* One polymorphic table, one CHECK. A kind outside the three is refused by
 * the table itself, and the refusal must not leave a row behind. */
KBC_TEST(history_holds_three_kinds_and_refuses_a_fourth) {
  char root[KBC_TEST_PATH_MAX];
  kbc_test_tmpdir(root, sizeof root);
  kbc_err err;
  kbc_err_reset(&err);
  kbc_store *s = open_at(root, "hist.db", &err);
  KBC_CHECK_NOT_NULL(s);
  if (s == NULL) {
    kbc_test_rmrf(root);
    return;
  }
  kbc_arena *a = kbc_arena_new(8192);

  kbc_history_row open_row = {0};
  open_row.kind = "open";
  open_row.artifact_id = "bbbbbbbbbbbb";
  open_row.scroll_y = 400;
  open_row.scroll_max = 900;
  open_row.scroll_y_max = 850;
  open_row.active_ms = 61000;
  open_row.last_section = "h-2";
  open_row.source = "web";
  open_row.user = "ann";
  open_row.started_at = 1000;
  open_row.updated_at = 1100;
  KBC_CHECK_OK(kbc_store_add_history(s, &open_row, &err));

  kbc_history_row search = {0};
  search.kind = "search";
  search.query = "sqlite partial index";
  search.user = "ann";
  search.started_at = 2000;
  search.updated_at = 2000;
  KBC_CHECK_OK(kbc_store_add_history(s, &search, &err));

  kbc_history_row comment = {0};
  comment.kind = "comment";
  comment.artifact_id = "bbbbbbbbbbbb";
  comment.comment_id = "cccccccccccc";
  comment.user = "bob";
  comment.started_at = 3000;
  comment.updated_at = 3000;
  KBC_CHECK_OK(kbc_store_add_history(s, &comment, &err));

  kbc_history_row *rows = NULL;
  size_t n = 0;
  KBC_CHECK_OK(kbc_store_list_history(s, a, NULL, 10, &rows, &n, &err));
  KBC_CHECK_EQ_INT(n, 3);
  if (n == 3) {
    KBC_CHECK_EQ_STR(rows[0].kind, "comment"); /* most recent first */
    KBC_CHECK_EQ_STR(rows[0].user, "bob");
    KBC_CHECK_EQ_STR(rows[0].comment_id, "cccccccccccc");
    KBC_CHECK_NULL(rows[0].query);
    KBC_CHECK_EQ_STR(rows[1].kind, "search");
    KBC_CHECK_EQ_STR(rows[1].query, "sqlite partial index");
    KBC_CHECK_NULL(rows[1].artifact_id);
    KBC_CHECK_EQ_STR(rows[2].kind, "open");
    KBC_CHECK_EQ_STR(rows[2].artifact_id, "bbbbbbbbbbbb");
    KBC_CHECK_EQ_INT(rows[2].scroll_y, 400);
    KBC_CHECK_EQ_INT(rows[2].scroll_y_max, 850);
    KBC_CHECK_EQ_INT(rows[2].active_ms, 61000);
    KBC_CHECK_EQ_STR(rows[2].last_section, "h-2");
    KBC_CHECK_EQ_STR(rows[2].source, "web");
    KBC_CHECK_MSG(rows[2].id > 0, "a stored row must come back with its id");
  }
  rows = NULL;
  n = 0;
  KBC_CHECK_OK(kbc_store_list_history(s, a, "ann", 10, &rows, &n, &err));
  KBC_CHECK_EQ_INT(n, 2);
  rows = NULL;
  n = 0;
  KBC_CHECK_OK(kbc_store_list_history(s, a, "carol", 10, &rows, &n, &err));
  KBC_CHECK_EQ_INT(n, 0);

  /* A fourth kind is not a new row type, it is a bug in the caller. */
  kbc_history_row bogus = {0};
  bogus.kind = "delete";
  bogus.started_at = 4000;
  bogus.updated_at = 4000;
  kbc_err_reset(&err);
  KBC_CHECK_ERR(kbc_store_add_history(s, &bogus, &err), KBC_ERR_INVALID);
  KBC_CHECK_ERR_MSG(err);
  KBC_CHECK_MSG(strstr(err.msg, "delete") != NULL,
                "the message must name the refused kind: %s", err.msg);
  rows = NULL;
  n = 0;
  KBC_CHECK_OK(kbc_store_list_history(s, a, NULL, 10, &rows, &n, &err));
  KBC_CHECK_EQ_INT(n, 3); /* the refused row is not on disk */

  kbc_arena_free(a);
  kbc_store_close(s);
  kbc_test_rmrf(root);
}

/* Why these three tables exist at all: a document removal must leave a user's
 * anchors, pins and reading history alone. Rust's cascade stops at the
 * document, and a timeline that loses its entries when a file is reorged out
 * is not a timeline. */
KBC_TEST(anchors_pins_and_history_outlive_the_document_they_name) {
  char root[KBC_TEST_PATH_MAX];
  kbc_test_tmpdir(root, sizeof root);
  kbc_err err;
  kbc_err_reset(&err);
  kbc_store *s = open_at(root, "outlive.db", &err);
  KBC_CHECK_NOT_NULL(s);
  if (s == NULL) {
    kbc_test_rmrf(root);
    return;
  }
  kbc_arena *a = kbc_arena_new(8192);
  kbc_artifact art;
  fill(&art, "bbbbbbbbbbbb", "kb", "a.md", KBC_KIND_ARTIFACT);
  KBC_CHECK_OK(kbc_store_upsert_artifact(s, &art, &err));

  KBC_CHECK_OK(kbc_store_add_corkboard(s, art.id, 1000, &err));
  KBC_CHECK_OK(kbc_store_add_corkboard(s, art.id, 9999, &err));
  KBC_CHECK_OK(kbc_store_pin_memory(s, art.id, 2000, &err));
  KBC_CHECK_OK(kbc_store_pin_memory(s, art.id, 9999, &err));
  KBC_CHECK_OK(kbc_store_first_seen(s, art.id, 3000, &err));
  const kbc_exclusion x = {"a.md", 4000, "operator note"};
  KBC_CHECK_OK(kbc_store_add_exclusion(s, &x, &err));
  const kbc_exclusion again = {"a.md", 9999, "a different note"};
  KBC_CHECK_OK(kbc_store_add_exclusion(s, &again, &err));
  kbc_history_row h = {0};
  h.kind = "open";
  h.artifact_id = art.id;
  h.started_at = 5000;
  h.updated_at = 5000;
  KBC_CHECK_OK(kbc_store_add_history(s, &h, &err));

  /* Re-anchoring and re-excluding keep the FIRST decision's time: the
   * timestamp is when the operator acted, not when the row was last written. */
  kbc_corkboard_row *board = NULL;
  size_t nb = 0;
  KBC_CHECK_OK(kbc_store_list_corkboard(s, a, 10, &board, &nb, &err));
  KBC_CHECK_EQ_INT(nb, 1);
  if (nb == 1) {
    KBC_CHECK_EQ_INT(board[0].created_at, 1000);
    KBC_CHECK_EQ_STR(board[0].artifact_id, art.id);
  }
  kbc_pin_row *pins = NULL;
  size_t np = 0;
  KBC_CHECK_OK(kbc_store_list_pins(s, a, 10, &pins, &np, &err));
  KBC_CHECK_EQ_INT(np, 1);
  if (np == 1) KBC_CHECK_EQ_INT(pins[0].pinned_at, 2000);
  kbc_exclusion *xs = NULL;
  size_t nx = 0;
  KBC_CHECK_OK(kbc_store_list_exclusions(s, a, 10, &xs, &nx, &err));
  KBC_CHECK_EQ_INT(nx, 1);
  if (nx == 1) {
    KBC_CHECK_EQ_INT(xs[0].excluded_at, 4000);
    KBC_CHECK_EQ_STR(xs[0].note, "operator note");
  }

  /* The document goes. Everything above stays. */
  KBC_CHECK_OK(kbc_store_delete_artifact(s, art.id, &err));
  board = NULL;
  nb = 0;
  KBC_CHECK_OK(kbc_store_list_corkboard(s, a, 10, &board, &nb, &err));
  KBC_CHECK_EQ_INT(nb, 1);
  pins = NULL;
  np = 0;
  KBC_CHECK_OK(kbc_store_list_pins(s, a, 10, &pins, &np, &err));
  KBC_CHECK_EQ_INT(np, 1);
  kbc_history_row *hist = NULL;
  size_t nh = 0;
  KBC_CHECK_OK(kbc_store_list_history(s, a, NULL, 10, &hist, &nh, &err));
  KBC_CHECK_EQ_INT(nh, 1);
  int64_t first = 0;
  KBC_CHECK_OK(kbc_store_get_first_seen(s, art.id, &first, &err));
  KBC_CHECK_EQ_INT(first, 3000);

  KBC_CHECK_OK(kbc_store_remove_corkboard(s, art.id, &err));
  KBC_CHECK_OK(kbc_store_unpin_memory(s, art.id, &err));
  KBC_CHECK_OK(kbc_store_remove_exclusion(s, "a.md", &err));
  kbc_err_reset(&err);
  KBC_CHECK_ERR(kbc_store_remove_corkboard(s, art.id, &err), KBC_ERR_NOTFOUND);
  KBC_CHECK_ERR(kbc_store_unpin_memory(s, art.id, &err), KBC_ERR_NOTFOUND);
  KBC_CHECK_ERR(kbc_store_remove_exclusion(s, "a.md", &err), KBC_ERR_NOTFOUND);
  KBC_CHECK_ERR_MSG(err);
  xs = NULL;
  nx = 0;
  KBC_CHECK_OK(kbc_store_list_exclusions(s, a, 10, &xs, &nx, &err));
  KBC_CHECK_EQ_INT(nx, 0);

  kbc_arena_free(a);
  kbc_store_close(s);
  kbc_test_rmrf(root);
}

/* Every one of these values can come out of a corpus file, a config file or a
 * query string, so each has to be bounded and named when it is not. */
KBC_TEST(stage1_writes_reject_empty_and_oversized_values) {
  char root[KBC_TEST_PATH_MAX];
  kbc_test_tmpdir(root, sizeof root);
  kbc_err err;
  kbc_err_reset(&err);
  kbc_store *s = open_at(root, "bound.db", &err);
  KBC_CHECK_NOT_NULL(s);
  if (s == NULL) {
    kbc_test_rmrf(root);
    return;
  }
  kbc_arena *a = kbc_arena_new(8192);

  const kbc_source no_corpus = {"", "/corpus", 1, false};
  KBC_CHECK_ERR(kbc_store_put_source(s, &no_corpus, &err), KBC_ERR_INVALID);
  const kbc_source no_path = {"kb", "", 1, false};
  KBC_CHECK_ERR(kbc_store_put_source(s, &no_path, &err), KBC_ERR_INVALID);
  KBC_CHECK_ERR(kbc_store_put_source(s, NULL, &err), KBC_ERR_INVALID);

  kbc_error_row row = {0};
  row.id = "e-aaaaaa";
  row.kind = "";
  row.corpus = "kb";
  row.path = "a.md";
  row.message = "boom";
  KBC_CHECK_ERR(kbc_store_record_error(s, &row, &err), KBC_ERR_INVALID);
  row.kind = "parse";
  row.message = "";
  KBC_CHECK_ERR(kbc_store_record_error(s, &row, &err), KBC_ERR_INVALID);
  /* A message is bytes out of a file: it gets a ceiling, and the ceiling is
   * enforced rather than trusted. */
  const size_t huge = 70000;
  char *big = malloc(huge + 1);
  KBC_CHECK_NOT_NULL(big);
  if (big != NULL) {
    memset(big, 'x', huge);
    big[huge] = '\0';
    row.message = big;
    KBC_CHECK_ERR(kbc_store_record_error(s, &row, &err), KBC_ERR_INVALID);
    KBC_CHECK_ERR_MSG(err);
    free(big);
  }
  /* Nothing that was refused was written. */
  kbc_error_row *rows = NULL;
  size_t n = 0;
  KBC_CHECK_OK(kbc_store_list_errors(s, a, "kb", false, 10, &rows, &n, &err));
  KBC_CHECK_EQ_INT(n, 0);

  kbc_history_row h = {0};
  h.kind = "";
  KBC_CHECK_ERR(kbc_store_add_history(s, &h, &err), KBC_ERR_INVALID);
  KBC_CHECK_ERR(kbc_store_first_seen(s, "", 1, &err), KBC_ERR_INVALID);
  /* 13 characters, one past KBC_MAX_ID_LEN. */
  KBC_CHECK_ERR(kbc_store_pin_memory(s, "0123456789abc", 1, &err),
                KBC_ERR_INVALID);
  KBC_CHECK_ERR(kbc_store_pin_memory(s, NULL, 1, &err), KBC_ERR_INVALID);
  KBC_CHECK_ERR(kbc_store_retry_count_for_path(s, "kb", "", NULL, NULL, &err),
                KBC_ERR_INVALID);

  kbc_arena_free(a);
  kbc_store_close(s);
  kbc_test_rmrf(root);
}

/* ------------------------------------- a rekey stays inside its own corpus -- */

/* Two corpora, each with a document at the SAME relative path, each linked to
 * by its own linker. That shape is not exotic: a config pointing two corpora
 * at overlapping directories produces it, and `edges` is keyed
 * (corpus, src_path, dst_path) precisely because a path is only unique within
 * a corpus. A rekey carries one document's links, so it has to name the
 * corpus — an unfiltered rewrite is not a wider correct rewrite, it is a
 * rename of somebody else's edge onto a path that does not exist for them. */
static kbc_store *two_corpora_at(char *root, const char *db, kbc_err *err) {
  kbc_store *s = open_at(root, db, err);
  if (s == NULL) return NULL;
  const kbc_source kb = {"kb", "/tmp/kbc-x-kb", 1, false};
  const kbc_source two = {"two", "/tmp/kbc-x-two", 1, false};
  KBC_CHECK_OK(kbc_store_put_source(s, &kb, err));
  KBC_CHECK_OK(kbc_store_put_source(s, &two, err));
  kbc_artifact art;
  fill(&art, "aaaaaaaaaaaa", "kb", "old.md", KBC_KIND_ARTIFACT);
  KBC_CHECK_OK(kbc_store_upsert_artifact(s, &art, err));
  fill(&art, "bbbbbbbbbbbb", "two", "old.md", KBC_KIND_ARTIFACT);
  KBC_CHECK_OK(kbc_store_upsert_artifact(s, &art, err));
  return s;
}

/* How many sources link HERE, as a set of names — the surface a reader sees,
 * so a corruption here is not a schema curiosity. */
static bool links_here(kbc_store *s, const char *corpus, const char *path,
                       const char *want, kbc_err *err) {
  kbc_strlist back;
  kbc_strlist_init(&back);
  kbc_store_list_backlinks(s, corpus, path, &back, err);
  bool ok = back.len == 1 && strcmp(back.items[0], want) == 0;
  kbc_strlist_free(&back);
  return ok;
}

KBC_TEST(a_rekey_does_not_reach_into_a_second_corpus_at_the_same_path) {
  char root[KBC_TEST_PATH_MAX];
  kbc_test_tmpdir(root, sizeof root);
  kbc_err err;
  kbc_err_reset(&err);
  kbc_store *s = two_corpora_at(root, "scope.db", &err);
  KBC_CHECK_NOT_NULL(s);
  if (s == NULL) {
    kbc_test_rmrf(root);
    return;
  }
  static const char *const into[] = {"old.md"};
  KBC_CHECK_OK(kbc_store_replace_edges(s, "kb", "kb-linker.md", into, 1, &err));
  KBC_CHECK_OK(
      kbc_store_replace_edges(s, "two", "two-linker.md", into, 1, &err));
  static const char *const pend[] = {"old.md"};
  KBC_CHECK_OK(kbc_store_add_pending_links(s, "kb", "kb-waiter.md", pend, 1, &err));
  KBC_CHECK_OK(
      kbc_store_add_pending_links(s, "two", "two-waiter.md", pend, 1, &err));
  KBC_CHECK_MSG(links_here(s, "kb", "old.md", "kb-linker.md", &err),
                "the fixture is wrong: kb's own backlink is not at old.md");
  KBC_CHECK_MSG(links_here(s, "two", "old.md", "two-linker.md", &err),
                "the fixture is wrong: two's own backlink is not at old.md");

  KBC_CHECK_OK(
      kbc_store_rekey_artifact(s, "aaaaaaaaaaaa", "cccccccccccc", "old.md",
                               "new.md", &err));

  /* The moved corpus: the whole point of the rekey, and the behaviour every
   * earlier rekey test already covers. Asserted here too so a change that
   * over-corrects — scoping so hard the move stops carrying anything — fails
   * in the same test that would have let the bleed through. */
  KBC_CHECK_MSG(links_here(s, "kb", "new.md", "kb-linker.md", &err),
                "the rekey did not carry the moved document's own inbound "
                "link to the new path");
  /* The corpus that was not renamed: untouched, and `new.md` in it is a path
   * with no document. */
  KBC_CHECK_MSG(links_here(s, "two", "old.md", "two-linker.md", &err),
                "rekeying in corpus kb moved corpus two's backlink to the new "
                "path, where no document by that name exists");
  kbc_strlist stray;
  kbc_strlist_init(&stray);
  kbc_store_list_backlinks(s, "two", "new.md", &stray, &err);
  KBC_CHECK_MSG(stray.len == 0,
                "corpus two gained %zu backlink(s) to new.md from a rekey it "
                "was not part of",
                stray.len);
  kbc_strlist_free(&stray);

  /* pending_links is keyed the same way and was rewritten by the same six
   * statements, so it is asserted through the table rather than through a
   * reader: there is no public listing for a pending link's target, and
   * draining one is destructive. */
  sqlite3 *raw = NULL;
  char dbp[KBC_TEST_PATH_MAX];
  const size_t rl = strlen(root);
  KBC_CHECK_MSG(rl + 16u < sizeof dbp, "tmpdir path too long");
  memcpy(dbp, root, rl);
  memcpy(dbp + rl, "/scope.db", 10);
  if (sqlite3_open_v2(dbp, &raw, SQLITE_OPEN_READONLY, NULL) == SQLITE_OK) {
    sqlite3_stmt *q = NULL;
    bool two_pending_intact = false;
    if (sqlite3_prepare_v2(raw,
                           "SELECT COUNT(*) FROM pending_links"
                           " WHERE corpus = 'two' AND src_path = 'two-waiter.md'"
                           " AND dst_path = 'old.md';",
                           -1, &q, NULL) == SQLITE_OK &&
        sqlite3_step(q) == SQLITE_ROW) {
      two_pending_intact = sqlite3_column_int(q, 0) == 1;
    }
    (void)sqlite3_finalize(q);
    (void)sqlite3_close(raw);
    KBC_CHECK_MSG(two_pending_intact,
                  "corpus two's pending link to old.md was rewritten by a "
                  "rekey in corpus kb");
  } else {
    KBC_CHECK_MSG(false, "could not reopen the store's database read-only");
  }

  kbc_store_close(s);
  kbc_test_rmrf(root);
}

/* The same fixture with one difference that turns a wrong rename into a
 * DESTROYED row, and it is the half that matters: `two-both.md` links to both
 * `old.md` and `new.md`. Unfiltered, the rekey renames its `old.md` edge onto
 * `new.md`, the primary key (two, two-both.md, new.md) collides with the edge
 * that was already there, OR IGNORE skips the write, and the unfiltered
 * `DELETE FROM edges WHERE dst_path = 'old.md'` then removes the row anyway.
 * One statement's guard causes the next statement to destroy what it could
 * not write. The function returns ok and nothing reports the loss — which is
 * why this is its own test rather than another assertion above: a bleed is
 * visible, a dropped edge is not. */
KBC_TEST(a_rekey_does_not_drop_a_second_corpus_edge_to_the_new_path) {
  char root[KBC_TEST_PATH_MAX];
  kbc_test_tmpdir(root, sizeof root);
  kbc_err err;
  kbc_err_reset(&err);
  kbc_store *s = two_corpora_at(root, "drop.db", &err);
  KBC_CHECK_NOT_NULL(s);
  if (s == NULL) {
    kbc_test_rmrf(root);
    return;
  }
  static const char *const both[] = {"old.md", "new.md"};
  KBC_CHECK_OK(
      kbc_store_replace_edges(s, "two", "two-both.md", both, 2, &err));
  static const char *const into[] = {"old.md"};
  KBC_CHECK_OK(kbc_store_replace_edges(s, "kb", "kb-linker.md", into, 1, &err));

  KBC_CHECK_OK(
      kbc_store_rekey_artifact(s, "aaaaaaaaaaaa", "cccccccccccc", "old.md",
                               "new.md", &err));

  /* two-both.md still links to old.md, and it still links to new.md: both
   * rows are still there. Counting one is not enough — the bleed alone
   * produces a count of one, with the wrong row left. */
  int64_t at_old = -1, at_new = -1;
  kbc_strlist back;
  kbc_strlist_init(&back);
  kbc_err_reset(&err);
  kbc_store_list_backlinks(s, "two", "old.md", &back, &err);
  at_old = (int64_t)back.len;
  bool two_both_at_old =
      back.len == 1 && strcmp(back.items[0], "two-both.md") == 0;
  kbc_strlist_free(&back);
  kbc_strlist_init(&back);
  kbc_store_list_backlinks(s, "two", "new.md", &back, &err);
  at_new = (int64_t)back.len;
  bool two_both_at_new =
      back.len == 1 && strcmp(back.items[0], "two-both.md") == 0;
  kbc_strlist_free(&back);
  KBC_CHECK_MSG(two_both_at_old,
                "corpus two's edge two-both.md -> old.md was DROPPED by a "
                "rekey in corpus kb, and the rekey reported success "
                "(old.md backlinks=%lld, new.md backlinks=%lld)",
                (long long)at_old, (long long)at_new);
  KBC_CHECK_MSG(two_both_at_new,
                "corpus two's pre-existing edge two-both.md -> new.md did not "
                "survive (new.md backlinks=%lld)",
                (long long)at_new);

  kbc_store_close(s);
  kbc_test_rmrf(root);
}

/* The empty-SELECT branch, which is the other half of the fix and the one with
 * a judgement call in it: an id the store does not hold has no corpus, so the
 * path half is skipped. Guessing one — the unfiltered behaviour — is what
 * destroyed rows; skipping leaves the other corpus's links where they are.
 * The observable is that a same-named path in a real corpus survives a rekey
 * of an id this store never held. */
KBC_TEST(a_rekey_of_an_id_this_store_does_not_hold_touches_no_path) {
  char root[KBC_TEST_PATH_MAX];
  kbc_test_tmpdir(root, sizeof root);
  kbc_err err;
  kbc_err_reset(&err);
  kbc_store *s = two_corpora_at(root, "ghost.db", &err);
  KBC_CHECK_NOT_NULL(s);
  if (s == NULL) {
    kbc_test_rmrf(root);
    return;
  }
  static const char *const into[] = {"old.md"};
  KBC_CHECK_OK(
      kbc_store_replace_edges(s, "two", "two-linker.md", into, 1, &err));

  /* An id and a path that name nothing here, in a store that DOES hold
   * `two/old.md`. The call is not an error: the id-keyed statements are
   * no-ops without a parent row, and the trailing delete is one too. */
  KBC_CHECK_OK(
      kbc_store_rekey_artifact(s, "dddddddddddd", "eeeeeeeeeeee", "old.md",
                               "renamed.md", &err));
  KBC_CHECK_MSG(links_here(s, "two", "old.md", "two-linker.md", &err),
                "a rekey for an id the store does not hold still rewrote a "
                "path in a corpus it never looked up");
  kbc_strlist stray;
  kbc_strlist_init(&stray);
  kbc_store_list_backlinks(s, "two", "renamed.md", &stray, &err);
  KBC_CHECK_MSG(stray.len == 0,
                "the guess produced %zu backlink(s) to a path it invented",
                stray.len);
  kbc_strlist_free(&stray);

  kbc_store_close(s);
  kbc_test_rmrf(root);
}

int main(void) {
  
static const kbc_test_case cases[] = {
      {"open_creates_file_and_parents_is_idempotent",
       open_creates_file_and_parents_is_idempotent},
      {"schema_version_of_null_store_is_zero",
       schema_version_of_null_store_is_zero},
      {"get_artifact_round_trips_every_field",
       get_artifact_round_trips_every_field},
      {"get_artifact_by_path_resolves_within_a_corpus",
       get_artifact_by_path_resolves_within_a_corpus},
      {"upsert_updates_in_place_and_rejects_a_foreign_id",
       upsert_updates_in_place_and_rejects_a_foreign_id},
      {"unknown_lookups_are_notfound_and_name_the_value",
       unknown_lookups_are_notfound_and_name_the_value},
      {"list_ids_honours_limit_offset_corpus_and_kind",
       list_ids_honours_limit_offset_corpus_and_kind},
      {"counts_corpora_and_total_bytes", counts_corpora_and_total_bytes},
      {"replace_chunks_replaces_and_never_leaves_a_ghost_ord",
       replace_chunks_replaces_and_never_leaves_a_ghost_ord},
      {"replace_chunks_is_per_document", replace_chunks_is_per_document},
      {"a_chunk_write_for_a_removed_document_names_the_missing_artifact",
       a_chunk_write_for_a_removed_document_names_the_missing_artifact},
      {"two_stores_sharing_one_db_never_collide_on_a_chunk_ord",
       two_stores_sharing_one_db_never_collide_on_a_chunk_ord},
      {"comments_list_resolve_and_foreign_key",
       comments_list_resolve_and_foreign_key},
      {"delete_cascades_to_chunks_and_comments",
       delete_cascades_to_chunks_and_comments},
      {"null_and_empty_arguments_are_invalid",
       null_and_empty_arguments_are_invalid},
      {"open_rejects_an_unusable_db_path", open_rejects_an_unusable_db_path},
      {"batch_resolve_agrees_with_the_per_row_read",
       batch_resolve_agrees_with_the_per_row_read},
      {"with_source_still_returns_the_whole_text",
       with_source_still_returns_the_whole_text},
      {"edges_replace_twice_leaves_no_ghost", edges_replace_twice_leaves_no_ghost},
      {"edges_delete_is_per_document", edges_delete_is_per_document},
      {"edges_in_degrees_is_one_aggregate_over_the_graph",
       edges_in_degrees_is_one_aggregate_over_the_graph},
      {"edges_survive_an_upgrade_from_the_previous_schema",
       edges_survive_an_upgrade_from_the_previous_schema},
      {"pending_links_are_recorded_drained_and_never_dangle",
       pending_links_are_recorded_drained_and_never_dangle},
      {"pending_links_do_not_cross_corpora", pending_links_do_not_cross_corpora},
      {"metas_a_key_nobody_carries_returns_no_rows",
       metas_a_key_nobody_carries_returns_no_rows},
      {"metas_replace_drops_a_value_the_document_no_longer_declares",
       metas_replace_drops_a_value_the_document_no_longer_declares},
      {"metas_are_scoped_to_their_corpus", metas_are_scoped_to_their_corpus},
      {"metas_a_null_value_asks_for_the_key_with_any_value",
       metas_a_null_value_asks_for_the_key_with_any_value},
      {"metas_forget_document_takes_the_facets_with_it",
       metas_forget_document_takes_the_facets_with_it},
      {"metas_reject_an_empty_key", metas_reject_an_empty_key},
      {"a_v4_volume_upgrades_to_the_current_schema_with_its_rows_intact",
       a_v4_volume_upgrades_to_the_current_schema_with_its_rows_intact},
      {"the_epoch_guard_refuses_a_volume_ahead_and_nothing_else",
       the_epoch_guard_refuses_a_volume_ahead_and_nothing_else},
      {"a_source_keeps_the_moment_it_was_first_seen",
       a_source_keeps_the_moment_it_was_first_seen},
      {"an_index_run_reads_back_as_in_flight_until_it_is_finished",
       an_index_run_reads_back_as_in_flight_until_it_is_finished},
      {"a_repeated_failure_counts_retries_instead_of_duplicating_the_row",
       a_repeated_failure_counts_retries_instead_of_duplicating_the_row},
      {"the_open_errors_query_returns_only_rows_nobody_dismissed",
       the_open_errors_query_returns_only_rows_nobody_dismissed},
      {"first_seen_survives_a_reindex", first_seen_survives_a_reindex},
      {"history_holds_three_kinds_and_refuses_a_fourth",
       history_holds_three_kinds_and_refuses_a_fourth},
      {"anchors_pins_and_history_outlive_the_document_they_name",
       anchors_pins_and_history_outlive_the_document_they_name},
      {"stage1_writes_reject_empty_and_oversized_values",
       stage1_writes_reject_empty_and_oversized_values},
      {"a_failing_migration_keeps_the_last_committed_version",
       a_failing_migration_keeps_the_last_committed_version},
      {"backlinks_of_an_unlinked_document_is_empty_and_ok",
       backlinks_of_an_unlinked_document_is_empty_and_ok},
      {"backlinks_name_the_sources_and_are_scoped_to_the_corpus",
       backlinks_name_the_sources_and_are_scoped_to_the_corpus},
      {"comment_docs_are_the_distinct_doc_ids_and_not_the_corpus",
       comment_docs_are_the_distinct_doc_ids_and_not_the_corpus},
      {"comment_docs_of_a_corpus_with_none_is_empty_and_ok",
       comment_docs_of_a_corpus_with_none_is_empty_and_ok},
      {"a_v10_volume_gains_moves_and_keeps_every_row_it_had",
       a_v10_volume_gains_moves_and_keeps_every_row_it_had},
      {"moves_has_the_declared_columns_and_only_completed_at_is_nullable",
       moves_has_the_declared_columns_and_only_completed_at_is_nullable},
      {"a_rekey_carries_a_comments_id_created_at_and_resolution",
       a_rekey_carries_a_comments_id_created_at_and_resolution},
      {"a_rekey_moves_the_artifact_row_and_every_table_that_names_it",
       a_rekey_moves_the_artifact_row_and_every_table_that_names_it},
      {"a_stale_id_follows_a_chain_of_renames_to_its_final_home",
       a_stale_id_follows_a_chain_of_renames_to_its_final_home},
      {"an_interrupted_move_is_listed_for_replay_and_never_followed",
       an_interrupted_move_is_listed_for_replay_and_never_followed},
      {"a_cyclic_move_chain_terminates_instead_of_hanging",
       a_cyclic_move_chain_terminates_instead_of_hanging},
      {"a_rekey_does_not_reach_into_a_second_corpus_at_the_same_path",
       a_rekey_does_not_reach_into_a_second_corpus_at_the_same_path},
      {"a_rekey_does_not_drop_a_second_corpus_edge_to_the_new_path",
       a_rekey_does_not_drop_a_second_corpus_edge_to_the_new_path},
      {"a_rekey_of_an_id_this_store_does_not_hold_touches_no_path",
       a_rekey_of_an_id_this_store_does_not_hold_touches_no_path},
      {NULL, NULL},
  };
  return kbc_test_run("store", cases);
}
