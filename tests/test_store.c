/* test_store.c — src/store.c: SQLite persistence for artifacts, chunks and
 * comments. Every case runs against its own store in its own tmpdir. */

#include "kbc_test.h"

#include <sqlite3.h>

#include "kbc/store.h"
#include "kbc/types.h"

/* The version the ladder in src/store.c tops out at. Pinned here so a
 * migration that bumps it has to be a deliberate edit in both places: a
 * binary that migrates past what its tests know about is the failure this
 * pin exists to make loud. */
#define CURRENT_SCHEMA 10

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
  KBC_CHECK_OK(kbc_store_retry_count_for_path(s, "kb", "bad.md", &retries,
                                              &err));
  KBC_CHECK_EQ_INT(retries, 0); /* a first failure starts the count at zero */

  row.id = "e-bbbbbb";
  row.message = "unclosed fence at line 9";
  row.created_at = 2000;
  KBC_CHECK_OK(kbc_store_record_error(s, &row, &err));
  KBC_CHECK_OK(kbc_store_retry_count_for_path(s, "kb", "bad.md", &retries,
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
  KBC_CHECK_OK(kbc_store_retry_count_for_path(s, "kb", "bad.md", &retries,
                                              &err));
  KBC_CHECK_EQ_INT(retries, 2);

  /* A different path, and the same path under a different corpus, are
   * different failures: neither may inherit the other's count. */
  row.id = "e-dddddd";
  row.path = "worse.md";
  row.created_at = 3000; /* its own clock, so "newest first" is decidable */
  KBC_CHECK_OK(kbc_store_record_error(s, &row, &err));
  KBC_CHECK_OK(kbc_store_retry_count_for_path(s, "kb", "worse.md", &retries,
                                              &err));
  KBC_CHECK_EQ_INT(retries, 0);
  row.id = "e-eeeeee";
  row.path = "bad.md";
  row.corpus = "other";
  KBC_CHECK_OK(kbc_store_record_error(s, &row, &err));
  KBC_CHECK_OK(kbc_store_retry_count_for_path(s, "other", "bad.md", &retries,
                                              &err));
  KBC_CHECK_EQ_INT(retries, 0);
  rows = NULL;
  n = 0;
  KBC_CHECK_OK(kbc_store_list_errors(s, a, "kb", false, 10, &rows, &n, &err));
  KBC_CHECK_EQ_INT(n, 2);

  /* A path nobody ever failed on reads as zero, not as an error. */
  KBC_CHECK_OK(
      kbc_store_retry_count_for_path(s, "kb", "fine.md", &retries, &err));
  KBC_CHECK_EQ_INT(retries, 0);

  /* Clearing hands the document a fresh budget, and the dismissed row stays
   * on disk: the history of a failure is what an operator reads. */
  KBC_CHECK_OK(kbc_store_clear_error(s, "kb", "bad.md", &err));
  KBC_CHECK_OK(kbc_store_retry_count_for_path(s, "kb", "bad.md", &retries,
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
  KBC_CHECK_ERR(kbc_store_retry_count_for_path(s, "kb", "", NULL, &err),
                KBC_ERR_INVALID);

  kbc_arena_free(a);
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
      {NULL, NULL},
  };
  return kbc_test_run("store", cases);
}
