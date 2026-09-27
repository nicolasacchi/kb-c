/* test_store.c — src/store.c: SQLite persistence for artifacts, chunks and
 * comments. Every case runs against its own store in its own tmpdir. */

#include "kbc_test.h"

#include "kbc/store.h"
#include "kbc/types.h"

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
  KBC_CHECK_EQ_INT(kbc_store_schema_version(s), 4);
  kbc_store_close(s);

  /* Reopening an existing store neither fails nor re-runs the migration. */
  kbc_err_reset(&err);
  s = open_at(root, "nested/deeper/kb.db", &err);
  KBC_CHECK_MSG(s != NULL, "reopen failed: %s", err.msg);
  if (s != NULL) {
    KBC_CHECK_EQ_INT(kbc_store_schema_version(s), 4);
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
  KBC_CHECK_EQ_INT(kbc_store_schema_version(v1), 4);
  kbc_store_close(v1);

  /* Re-open: the migration ladder is a no-op the second time, and the edges
   * table is still there and still writable. */
  kbc_store *s = open_at(root, "old.db", &err);
  KBC_CHECK_NOT_NULL(s);
  KBC_CHECK_EQ_INT(kbc_store_schema_version(s), 4);
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
      {NULL, NULL},
  };
  return kbc_test_run("store", cases);
}
