/* test_store.c — src/store.c: SQLite persistence for artifacts, chunks and
 * comments. Every case runs against its own store in its own tmpdir. */

#include "kbc_test.h"

#include "kbc/store.h"
#include "kbc/types.h"

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
  KBC_CHECK_EQ_INT(kbc_store_schema_version(s), 1);
  kbc_store_close(s);

  /* Reopening an existing store neither fails nor re-runs the migration. */
  kbc_err_reset(&err);
  s = open_at(root, "nested/deeper/kb.db", &err);
  KBC_CHECK_MSG(s != NULL, "reopen failed: %s", err.msg);
  if (s != NULL) {
    KBC_CHECK_EQ_INT(kbc_store_schema_version(s), 1);
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
      {NULL, NULL},
  };
  return kbc_test_run("store", cases);
}
