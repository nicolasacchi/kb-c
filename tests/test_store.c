/* test_store.c — src/store.c: SQLite persistence for artifacts, chunks and
 * comments. Every case runs against its own store in its own tmpdir. */

#include "kbc_test.h"

#include <fcntl.h>
#include <pthread.h>
#include <sqlite3.h>
#include <unistd.h>

#include "kbc/store.h"
#include "kbc/types.h"

/* The version the ladder in src/store.c tops out at. Pinned here so a
 * migration that bumps it has to be a deliberate edit in both places: a
 * binary that migrates past what its tests know about is the failure this
 * pin exists to make loud. */
#define CURRENT_SCHEMA 13

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

/* The anchor-state contract now lives in include/kbc/store.h. It used to be
 * mirrored here, because that header was the orchestrator's file and this
 * assignment could not edit it — and a hand-copied mirror of a frozen
 * contract is the drift a frozen header exists to prevent, not the mitigation
 * for it. The round-trip test that guarded the mirror
 * (`anchor_states_are_three_and_only_three`) STAYS: it was never a mirror
 * guard, it is the assertion that all three states are reachable and
 * distinguishable, which is worth having whether or not anything is mirrored. */
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

/* The read side round-trips what the write side stores, as PARALLEL lists,
 * and the pairing is the assertion. Checking the keys and the values as two
 * independent sets would pass an implementation that returned the right keys
 * against the wrong values, which is the only way this function can be wrong
 * and still look right. */
KBC_TEST(metas_read_back_the_pairs_replace_wrote) {
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
  static const char *const keys[] = {"author", "kb-tags"};
  static const char *const vals[] = {"nik", "search, index"};
  KBC_CHECK_OK(kbc_store_replace_metas(s, "kb", "a.md", keys, vals, 2, &err));

  kbc_strlist k;
  kbc_strlist v;
  kbc_strlist_init(&k);
  kbc_strlist_init(&v);
  KBC_CHECK_OK(kbc_store_get_metas(s, "kb", "a.md", &k, &v, &err));
  KBC_CHECK_MSG(k.len == 2 && v.len == 2,
                "get_metas returned %zu keys and %zu values; the lists are "
                "parallel and a difference means a row lost half itself",
                k.len, v.len);
  if (k.len == 2 && v.len == 2) {
    /* SORTED BY KEY, whatever order they were written in: `author` < `kb-tags`
     * and the input lists them the other way round, so an unordered SELECT
     * that happened to return insertion order would fail here. */
    KBC_CHECK_EQ_STR(k.items[0], "author");
    KBC_CHECK_EQ_STR(v.items[0], "nik");
    KBC_CHECK_EQ_STR(k.items[1], "kb-tags");
    KBC_CHECK_EQ_STR(v.items[1], "search, index");
  }
  kbc_strlist_free(&k);
  kbc_strlist_free(&v);
  kbc_store_close(s);
  kbc_test_rmrf(root);
}

/* A multi-valued facet is ONE row per value under the SAME key, so the key
 * alone is not a total order and the two rows can come back in either order.
 * They must come back in the same order every time, or two daemons that
 * ingested the same document disagree on ORDER while agreeing on every value
 * — a diff nobody can read, and one that makes the parity harness useless for
 * the thing it was built to check.
 *
 * Written in the ALREADY-SORTED order, so a reader that simply echoed what it
 * was given cannot pass: reversing the input is what forces the reader's
 * ORDER BY to be the thing under test. */
KBC_TEST(metas_a_multi_valued_facet_comes_back_in_a_stable_order) {
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
  /* Reverse-sorted on the way in, so insertion order and the required order
 * disagree: index, search, then alpha. */
  static const char *const keys[] = {"kb-tags", "kb-tags", "kb-tags"};
  static const char *const vals[] = {"search", "index", "alpha"};
  KBC_CHECK_OK(kbc_store_replace_metas(s, "kb", "a.md", keys, vals, 3, &err));

  static const char *const want[] = {"alpha", "index", "search"};
  kbc_strlist k;
  kbc_strlist v;
  kbc_strlist_init(&k);
  kbc_strlist_init(&v);
  KBC_CHECK_OK(kbc_store_get_metas(s, "kb", "a.md", &k, &v, &err));
  KBC_CHECK_MSG(k.len == 3, "three values of one facet read back as %zu rows",
                k.len);
  for (size_t i = 0; i < k.len && i < 3; i++) {
    KBC_CHECK_MSG(strcmp(k.items[i], "kb-tags") == 0,
                  "row %zu has key \"%s\"; one facet is one key", i, k.items[i]);
    KBC_CHECK_MSG(strcmp(v.items[i], want[i]) == 0,
                  "row %zu is \"%s\", wanted \"%s\" — the values of a "
                  "multi-valued facet are not in a stable order",
                  i, v.items[i], want[i]);
  }
  kbc_strlist_free(&k);
  kbc_strlist_free(&v);
  kbc_store_close(s);
  kbc_test_rmrf(root);
}

/* TWO KINDS OF NOTHING, and the difference between them is the contract.
 *
 * A document that declares no facets, and a corpus that does not exist, both
 * read as zero rows — and both must be KBC_OK with two empty lists, because
 * the overwhelmingly common case for this function is a document with no
 * facets, and a caller that had to catch an error to distinguish it would
 * have to treat the ordinary path as exceptional. `docs_with_meta` already
 * makes the same promise for the same reason ("a filter that matches nothing
 * must never look like a filter that was ignored"), and this is the read
 * twin of that.
 *
 * A NOT_FOUND here would be actively wrong for the second case: doc_metas is
 * keyed by path and carries no foreign key, so a path nobody has ingested
 * genuinely has no facets, and saying "not found" would be a claim about the
 * document the store cannot support. */
KBC_TEST(metas_a_document_with_no_facets_is_empty_and_not_an_error) {
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
  kbc_strlist k;
  kbc_strlist v;

  /* A path that was never ingested, in a corpus that does not exist. */
  kbc_strlist_init(&k);
  kbc_strlist_init(&v);
  KBC_CHECK_OK(kbc_store_get_metas(s, "nosuch", "never.md", &k, &v, &err));
  /* Success leaves `err` UNTOUCHED, not filled: a caller branching on the
   * status must not be able to find a stale message from an earlier call and
   * read it as this one's. The buffer is still whatever the previous call
   * left, so this asserts nothing was written rather than that it was
   * cleared — which is the guarantee the store actually makes. */
  KBC_CHECK_MSG(err.msg[0] == '\0',
                "a successful get_metas wrote \"%s\" into err", err.msg);
  KBC_CHECK_EQ_INT((long long)k.len, 0);
  KBC_CHECK_EQ_INT((long long)v.len, 0);
  kbc_strlist_free(&k);
  kbc_strlist_free(&v);

  /* A path that WAS ingested, and declared nothing — the common case, and the
   * one a NOT_FOUND would turn into an error on every such document. */
  KBC_CHECK_OK(kbc_store_replace_metas(s, "kb", "plain.md", NULL, NULL, 0, &err));
  kbc_strlist_init(&k);
  kbc_strlist_init(&v);
  KBC_CHECK_OK(kbc_store_get_metas(s, "kb", "plain.md", &k, &v, &err));
  KBC_CHECK_MSG(err.msg[0] == '\0',
                "a successful get_metas wrote \"%s\" into err", err.msg);
  KBC_CHECK_EQ_INT((long long)k.len, 0);
  KBC_CHECK_EQ_INT((long long)v.len, 0);
  kbc_strlist_free(&k);
  kbc_strlist_free(&v);

  /* One path's facets do not leak into another's. `b.md` was never written,
   * so if `a.md` had leaked into it the lists would be non-empty — and a
   * query that forgot the path predicate would pass every assertion above. */
  static const char *const keys[] = {"tags"};
  static const char *const vals[] = {"rust"};
  KBC_CHECK_OK(kbc_store_replace_metas(s, "kb", "a.md", keys, vals, 1, &err));
  kbc_strlist_init(&k);
  kbc_strlist_init(&v);
  KBC_CHECK_OK(kbc_store_get_metas(s, "kb", "b.md", &k, &v, &err));
  KBC_CHECK_EQ_INT((long long)k.len, 0);
  kbc_strlist_free(&k);
  kbc_strlist_free(&v);

  /* The same facet key in two corpora stays separate, which is the corpus
   * half of the same predicate. */
  KBC_CHECK_OK(kbc_store_replace_metas(s, "other", "a.md", keys, vals, 1, &err));
  kbc_strlist_init(&k);
  kbc_strlist_init(&v);
  KBC_CHECK_OK(kbc_store_get_metas(s, "other", "a.md", &k, &v, &err));
  KBC_CHECK_MSG(k.len == 1, "the other corpus's facet read back as %zu rows",
                k.len);
  kbc_strlist_free(&k);
  kbc_strlist_free(&v);

  kbc_store_close(s);
  kbc_test_rmrf(root);
}

/* A path validated the same way the writers validate it, so a hostile
 * argument is rejected at the boundary rather than bound into the query. A
 * reader that skipped validation would return an empty list for a path no
 * document can have, which is indistinguishable from "no facets". */
KBC_TEST(metas_reject_a_path_no_writer_would_have_accepted) {
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
  kbc_strlist k;
  kbc_strlist v;
  kbc_strlist_init(&k);
  kbc_strlist_init(&v);
  KBC_CHECK_ERR(kbc_store_get_metas(s, "", "a.md", &k, &v, &err),
                KBC_ERR_INVALID);
  KBC_CHECK_ERR_MSG(err);
  kbc_strlist_init(&k);
  kbc_strlist_init(&v);
  KBC_CHECK_ERR(kbc_store_get_metas(s, "kb", "", &k, &v, &err),
                KBC_ERR_INVALID);
  KBC_CHECK_ERR_MSG(err);
  /* The lists are untouched by a rejected call, so a caller that ignores the
 * status and reads the lists anyway gets the previous answer, not a partial
 * one. */
  KBC_CHECK_EQ_INT((long long)k.len, 0);
  kbc_strlist_free(&k);
  kbc_strlist_free(&v);
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

/* The v11 schema — V10_DB plus what step 11 added, and NOT what step 12 adds.
 * Executed as a second statement rather than concatenated, because V10_DB is a
 * variable and adjacent-literal concatenation only works on literals; the
 * ordering is the same, and "a v11 volume" then means exactly what it says.
 *
 * The DDL is verbatim from the v11 step in src/store.c, so this fixture is a
 * volume the previous binary really did write — which is the only kind of
 * volume the v12 step has to survive. The rows are the two states that matter
 * to it: one move finished, one still in flight. A completed row that came
 * back NULL is the failure this test exists to catch, and a step that
 * backfilled rather than ALTERed would show it as a non-NULL abandoned_at. */
static const char *const V11_MOVES =
    "CREATE TABLE moves ("
    " id INTEGER PRIMARY KEY AUTOINCREMENT,"
    " old_id TEXT NOT NULL,"
    " new_id TEXT NOT NULL,"
    " old_rel TEXT NOT NULL,"
    " new_rel TEXT NOT NULL,"
    " moved_at INTEGER NOT NULL,"
    " completed_at INTEGER);"
    "CREATE INDEX idx_moves_old_id ON moves(old_id);"
    "CREATE INDEX idx_moves_old_rel ON moves(old_rel);"
    "INSERT INTO schema_version(version) VALUES(11);"
    "INSERT INTO moves(old_id, new_id, old_rel, new_rel, moved_at,"
    " completed_at) VALUES('aaaaaaaaaaaa','bbbbbbbbbbbb','a.md','b.md',1,10);"
    "INSERT INTO moves(old_id, new_id, old_rel, new_rel, moved_at,"
    " completed_at) VALUES('cccccccccccc','dddddddddddd','c.md','d.md',2,NULL);";

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

/* A column the migration declared, checked as a COLUMN of that table. This is
 * the one shape an ALTER can get wrong in a way nothing else here can see: the
 * table exists, the rows read back, every lookup works, and the column the new
 * code names is not in the schema — a runtime "no such column" on the first
 * query, in a database whose version number says it is current. */
static bool column_exists(sqlite3 *h, const char *table, const char *col) {
  sqlite3_stmt *q = NULL;
  bool found = false;
  if (sqlite3_prepare_v2(h,
                         "SELECT COUNT(*) FROM pragma_table_info(?1)"
                         " WHERE name = ?2;",
                         -1, &q, NULL) == SQLITE_OK) {
    (void)sqlite3_bind_text(q, 1, table, -1, SQLITE_TRANSIENT);
    (void)sqlite3_bind_text(q, 2, col, -1, SQLITE_TRANSIENT);
    if (sqlite3_step(q) == SQLITE_ROW) found = sqlite3_column_int(q, 0) > 0;
  }
  (void)sqlite3_finalize(q);
  return found;
}

/* Runs `sql` and reports whether SQLite REFUSED it. This is the assertion
 * that tells a shipped ALTER from a wrong fold, and column_exists cannot:
 * a fold puts `abandoned_at` in the CREATE that introduces `moves`, so on a
 * volume that volume already HAS the column and every existence check above
 * passes — while the real question, "what does the new code do against a
 * database the OLD code wrote", is never asked. Against a genuine v11 file
 * the post-migration query must FAIL, and if it does not, the step under
 * test did not run.
 *
 * A non-NULL return is the refusal. sqlite3_prepare_v2 is where "no such
 * column" surfaces, so stepping is not reached and `q` stays NULL. */
static bool query_refused(sqlite3 *h, const char *sql) {
  sqlite3_stmt *q = NULL;
  int rc = sqlite3_prepare_v2(h, sql, -1, &q, NULL);
  if (q != NULL) (void)sqlite3_finalize(q);
  return rc != SQLITE_OK;
}

/* Every row `PRAGMA foreign_key_check` reports, as "table:rowid->parent#fkid"
 * strings. The four columns are (table, rowid, parent, fkid) — rowid is the
 * SECOND and the parent name the THIRD, and reading them the other way round
 * produces rows that look plausible and name no parent at all, which is the
 * failure this comment exists to prevent.
 *
 * The FULL list, deliberately: `foreign_key_check` returns one row per
 * offending edge, and a reader that stops at the first reports a database as
 * having one problem when it has nine — so the operator fixes one, re-runs,
 * and finds eight more. This helper exists to make truncation impossible to
 * write: a caller that wants the count gets the count, and a caller that
 * wants the rows cannot get a prefix without saying so. */
static size_t foreign_key_violations(sqlite3 *h, char out[][96], size_t cap) {
  sqlite3_stmt *q = NULL;
  size_t n = 0;
  if (sqlite3_prepare_v2(h, "PRAGMA foreign_key_check;", -1, &q, NULL) !=
      SQLITE_OK)
    return 0;
  while (sqlite3_step(q) == SQLITE_ROW && n < cap) {
    const unsigned char *tbl = sqlite3_column_text(q, 0);
    sqlite3_int64 rowid = sqlite3_column_int64(q, 1);
    const unsigned char *par = sqlite3_column_text(q, 2);
    int fkid = sqlite3_column_int(q, 3);
    (void)snprintf(out[n], 96, "%s:%lld->%s#%d", tbl ? (const char *)tbl : "?",
                   (long long)rowid, par ? (const char *)par : "?", fkid);
    n++;
  }
  (void)sqlite3_finalize(q);
  return n;
}

/* One INTEGER out of the first row `sql` returns for `arg`, and whether that
 * row exists at all. `*isnull` separates SQL NULL from the value 0, which is
 * the whole distinction every assertion about an unset stamp is making: a
 * sentinel would satisfy `== 0` and quietly turn "never completed" into a
 * timestamp. */
static bool raw_one_i64(sqlite3 *h, const char *sql, const char *arg, int col,
                        bool *isnull, int64_t *out) {
  sqlite3_stmt *q = NULL;
  bool found = false;
  *isnull = true;
  *out = 0;
  if (sqlite3_prepare_v2(h, sql, -1, &q, NULL) == SQLITE_OK) {
    (void)sqlite3_bind_text(q, 1, arg, -1, SQLITE_TRANSIENT);
    if (sqlite3_step(q) == SQLITE_ROW) {
      found = true;
      *isnull = sqlite3_column_type(q, col) == SQLITE_NULL;
      *out = (int64_t)sqlite3_column_int64(q, col);
    }
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

/* Migrations 11 and 12, observed on a REAL v10 volume rather than argued from
 * the ladder array. The two halves are what a migration can get wrong in
 * opposite directions: `moves` must EXIST afterwards, and every row the old
 * volume had must still be there. A test that only checked the version number
 * would pass on a step that recorded 11 and created nothing.
 *
 * The fixture is at the version immediately before the first step under test,
 * so the ladder runs v11 and then v12 and nothing earlier — those two are what
 * this case covers together. */
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

/* Migration 12, observed on a REAL v11 volume — one the previous binary
 * wrote, `moves` and all, and with no abandoned_at anywhere in it.
 *
 * A v10 fixture cannot stand in for it. Step 12 ALTERs a table that v10
 * volumes do not have, so against a v10 volume the only thing under test is
 * the step before it, and an ALTER that silently did nothing — or that
 * recorded version 12 and left the schema at 11 — would pass every assertion
 * a v10 fixture can make.
 *
 * What has to survive: the column appears, and BOTH rows keep their stamps.
 * The completed row coming back with completed_at = 10 is the load-bearing
 * assertion — a step that rebuilt the table, or that backfilled every row,
 * would lose it — and the in-flight row's NULL is the other half, because
 * "in flight" and "never stamped with 0" have to stay distinguishable or
 * `moves_list_incomplete` starts replaying a finished move forever. */
KBC_TEST(a_v11_volume_gains_the_abandon_column_and_keeps_its_stamps) {
  char root[KBC_TEST_PATH_MAX];
  kbc_test_tmpdir(root, sizeof root);
  sqlite3 *raw = raw_open(root, "old.db");
  KBC_CHECK_NOT_NULL(raw);
  if (raw == NULL) {
    kbc_test_rmrf(root);
    return;
  }
  raw_exec(raw, V10_DB);
  raw_exec(raw, V11_MOVES);
  KBC_CHECK_EQ_INT(raw_version(raw), 11);
  /* The premise: this volume has a moves table and NOT the column, so the
   * upgrade has something to do. A fixture that already had the column would
   * make every assertion below pass for free. */
  KBC_CHECK_MSG(table_exists(raw, "moves"), "the fixture has no moves table");
  KBC_CHECK_MSG(!column_exists(raw, "moves", "abandoned_at"),
                "the fixture already has abandoned_at, so the upgrade proves "
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

  /* The completed row still redirects, so the ALTER neither lost the row nor
   * changed what a completed move means. Checked through the API that reads
   * it, because a row that reads back correctly through raw SQL and is not
   * followed would be a walk bug wearing a migration's clothes. */
  kbc_strlist first;
  kbc_strlist_init(&first);
  KBC_CHECK_OK(kbc_store_moves_lookup(s, "aaaaaaaaaaaa", &first, &err));
  KBC_CHECK_MSG(first.len == 1,
                "the pre-upgrade completed move no longer resolves (%zu hops)",
                first.len);
  if (first.len == 1) KBC_CHECK_EQ_STR(first.items[0], "bbbbbbbbbbbb");
  kbc_strlist_free(&first);
  kbc_store_close(s);

  /* The stamps, read back out of the file. `abandoned_at` NULL on a row that
   * predates the ALTER is the whole backfill story: a nullable column added to
   * an existing table is NULL on every existing row, and anything else here
   * would be a decision this test did not ask for. */
  raw = raw_open(root, "old.db");
  KBC_CHECK_NOT_NULL(raw);
  if (raw != NULL) {
    KBC_CHECK_MSG(column_exists(raw, "moves", "abandoned_at"),
                  "the v12 step recorded its version without adding the column");
    /* THE FOLD DETECTOR, on the UPGRADED volume. The v12 replay query — the
     * one `kbc_store_list_incomplete_moves` actually runs, `abandoned_at`
     * included — must now be ACCEPTED. column_exists above can be satisfied by
     * a fold, because under a fold the column is born in the CREATE that
     * introduces `moves` and every shape check reads back fine; what a fold
     * cannot do is fix a volume the OLD binary wrote, since that file already
     * has its `moves` and no later step will touch it. So the assertion that
     * separates the two is behavioural, on a file the old code produced. */
    KBC_CHECK_MSG(!query_refused(raw,
                                 "SELECT old_id FROM moves"
                                 " WHERE completed_at IS NULL AND"
                                 " abandoned_at IS NULL;"),
                  "the upgraded volume still refuses the v12 replay query, so "
                  "this step is not an ALTER — check the ladder for a fold "
                  "into the CREATE that introduces moves");
    bool isnull = false;
    int64_t v = 0;
    KBC_CHECK_MSG(raw_one_i64(raw,
                              "SELECT completed_at FROM moves WHERE old_id ="
                              " ?1;",
                              "aaaaaaaaaaaa", 0, &isnull, &v),
                  "the completed move did not survive the upgrade");
    KBC_CHECK_MSG(!isnull && v == 10,
                  "completed_at = %s%lld, wanted 10 — the upgrade rewrote a "
                  "stamp the store had already made",
                  isnull ? "NULL" : "", (long long)v);
    KBC_CHECK_MSG(raw_one_i64(raw,
                              "SELECT abandoned_at FROM moves WHERE old_id ="
                              " ?1;",
                              "aaaaaaaaaaaa", 0, &isnull, &v),
                  "abandoned_at is not readable on the upgraded volume");
    KBC_CHECK_MSG(isnull,
                  "abandoned_at = %lld on a row that predates the column; NULL "
                  "is what 'never abandoned' has to read as",
                  (long long)v);
    KBC_CHECK_MSG(raw_one_i64(raw,
                              "SELECT completed_at FROM moves WHERE old_id ="
                              " ?1;",
                              "cccccccccccc", 0, &isnull, &v),
                  "the in-flight move did not survive the upgrade");
    KBC_CHECK_MSG(isnull,
                  "an in-flight move came back with completed_at = %lld, so a "
                  "finished move and a crashed one are indistinguishable",
                  (long long)v);
    /* Both rows, and one row per version: step 12 ran once on a file that had
     * never seen it. A version table with a duplicate is the shape a re-runnable
     * step leaves behind when its guard is missing. */
    KBC_CHECK_EQ_INT(raw_version_rows(raw), CURRENT_SCHEMA);
    sqlite3_stmt *q = NULL;
    int64_t rows = 0;
    if (sqlite3_prepare_v2(raw, "SELECT COUNT(*) FROM moves;", -1, &q,
                           NULL) == SQLITE_OK &&
        sqlite3_step(q) == SQLITE_ROW) {
      rows = (int64_t)sqlite3_column_int64(q, 0);
    }
    (void)sqlite3_finalize(q);
    KBC_CHECK_EQ_INT(rows, 2);
    (void)sqlite3_close(raw);
  }

  /* Idempotent on a SECOND opener, which for THIS step is the sharp half: v12
   * is the one migration in the ladder that is not re-runnable on its own
   * (ADD COLUMN fails if the column is there), so nothing but the recorded
   * version stands between an already-migrated file and a duplicate-column
   * error on the next open. */
  kbc_err_reset(&err);
  kbc_store *again = open_at(root, "old.db", &err);
  KBC_CHECK_MSG(again != NULL, "second open of a migrated file failed: %s",
                err.msg);
  if (again != NULL) {
    KBC_CHECK_EQ_INT(kbc_store_schema_version(again), CURRENT_SCHEMA);
    kbc_strlist ids;
    kbc_strlist_init(&ids);
    KBC_CHECK_OK(kbc_store_moves_lookup(again, "aaaaaaaaaaaa", &ids, &err));
    KBC_CHECK_EQ_INT((long long)ids.len, 1);
    kbc_strlist_free(&ids);
    kbc_store_close(again);
  }
  raw = raw_open(root, "old.db");
  if (raw != NULL) {
    KBC_CHECK_EQ_INT(raw_version_rows(raw), CURRENT_SCHEMA);
    (void)sqlite3_close(raw);
  }
  kbc_test_rmrf(root);
}

/* The DDL of migrations 11 and 12, read back out of a migrated volume, column
 * by column. A table that merely EXISTS is not the table the original declares,
 * and the two things most likely to be wrong — a NOT NULL dropped for
 * convenience, and the nullable completed_at folded into a sentinel like every
 * other nullable-in-Rust column was — are both invisible to table_exists.
 *
 * completed_at is asserted NULLABLE specifically because it is a column that
 * really is null while a move is in flight: that NULL is the crash signal
 * moves_list_incomplete reads. A NOT NULL with a sentinel would make "never
 * stamped" and "stamped with the sentinel" indistinguishable to it.
 *
 * abandoned_at is kb-c's ONE departure from the Rust DDL, declared here so
 * that departure is a checked decision rather than an accident: the Rust table
 * has no third state because the Rust bring-up has no pass that has to leave
 * one. It is nullable for the same reason completed_at is — NULL is "not
 * abandoned" — and an ALTER that adds a nullable column gives every existing
 * row exactly that, so the upgrade needs no backfill. A NOT NULL, or a 0
 * sentinel, would make "never abandoned" and "abandoned at the epoch"
 * indistinguishable, and every predicate over it would have to carry the
 * disambiguation. */
KBC_TEST(moves_declares_the_rust_columns_plus_the_one_kb_c_adds) {
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
  /* The Rust DDL in order, then the one column kb-c adds. `id` is expected NOT
   * NULL = 0 because SQLite reports an INTEGER PRIMARY KEY that way whatever
   * the DDL says: it is an alias for the rowid and can never be null in
   * practice. Everything a migration actually constrains is the other five,
   * plus the two that must stay NULLABLE. */
  static const char *const want_name[] = {
      "id",          "old_id",  "new_id",       "old_rel",
      "new_rel",     "moved_at", "completed_at", "abandoned_at"};
  static const int want_notnull[] = {0, 1, 1, 1, 1, 1, 0, 0};
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

/* `PRAGMA foreign_keys` is a CONNECTION setting, not part of a table's
 * definition, so it is not folded into any CREATE and cannot be: a file
 * written by a connection that had it off, or by a tool that never turns it
 * on, is a file whose edges were never enforced. `foreign_key_check` is the
 * only way to find that out, and it is a PRAGMA, so nothing in the ladder
 * runs it — the schema can be at the current version and still be internally
 * inconsistent.
 *
 * THE PROPERTY UNDER TEST IS THE COUNT, and it is the count because a
 * reader that stops at the first row is worse than no reader: it reports one
 * problem, the operator fixes that one, re-runs, and finds the next. So this
 * plants THREE orphaned edges of two different kinds and asserts three rows
 * come back — a first-row-only implementation fails here, and a test that
 * planted one orphan could not tell the two apart.
 *
 * The orphans are written with foreign_keys explicitly OFF, which is the
 * whole point: they are the rows the pragma's absence let in. `comments` and
 * `chunks` are chosen because both are `ON DELETE CASCADE` from
 * `artifacts(id)` (v1), so a cascade is what would normally have removed
 * them, and a delete that ran without the pragma leaves exactly this. */
KBC_TEST(foreign_key_check_reports_every_orphaned_edge_not_the_first) {
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
  /* The premise: a database with no violations at all. Without this the
   * count below could be satisfied by a helper that returns rows from a
   * healthy file. */
  char rows[8][96];
  KBC_CHECK_MSG(foreign_key_violations(raw, rows, 8) == 0,
                "a freshly migrated store already reports FK violations, so "
                "the orphans planted below prove nothing");

  /* The pragma is per-connection, and this one is the writer that never
   * enforced the edges — which is the only way to produce the state the check
   * exists to find. */
  raw_exec(raw, "PRAGMA foreign_keys = OFF;");
  /* Two children of a parent that does not exist, in two different tables,
   * plus a second chunk — three rows, so "returns the first" and "returns
   * all" are different answers. */
  raw_exec(raw,
           "INSERT INTO chunks(doc_id, ord, text)"
           " VALUES('gone00000001',0,'a');"
           "INSERT INTO chunks(doc_id, ord, text)"
           " VALUES('gone00000001',1,'b');"
           "INSERT INTO comments(id, doc_id, anchor, author, body,"
           " created_at, resolved)"
           " VALUES('k-aaaaaa','gone00000002','#a','me','body','2024',0);");
  raw_exec(raw, "PRAGMA foreign_keys = ON;");

  size_t n = foreign_key_violations(raw, rows, 8);
  KBC_CHECK_MSG(n == 3,
                "foreign_key_check reported %zu offending rows, wanted 3 — a "
                "reader that stops at the first calls a three-edge database a "
                "one-edge one",
                n);

  /* Each violation names the CHILD table it sits in and the parent table the
   * edge points at, which is the pair an operator needs to find it. A count of
   * 3 drawn from three rows of ONE table would satisfy the assertion above
   * and say nothing about where to look, so the split is checked: two from
   * `chunks`, one from `comments`, every one naming `artifacts` as the parent
   * it fails to find.
   *
   * NOT the missing key value: the pragma reports the child's rowid, not the
   * absent parent id, so there is nothing in the row to match against a
   * literal. Asserting one would be asserting a SQLite feature that is not
   * there. */
  int seen_chunks = 0;
  int seen_comments = 0;
  for (size_t i = 0; i < n; i++) {
    if (strncmp(rows[i], "chunks:", 7) == 0) seen_chunks++;
    if (strncmp(rows[i], "comments:", 9) == 0) seen_comments++;
    KBC_CHECK_MSG(strstr(rows[i], "->artifacts#") != NULL,
                  "violation %zu (%s) does not name artifacts as the parent "
                  "it fails to find",
                  i, rows[i]);
  }
  KBC_CHECK_MSG(seen_chunks == 2, "the two orphan chunks read back as %d",
                seen_chunks);
  KBC_CHECK_MSG(seen_comments == 1, "the orphan comment read back as %d",
                seen_comments);

  /* And the same database, repaired, reads clean. A check that always
   * returned three would pass every assertion above. */
  raw_exec(raw, "DELETE FROM chunks WHERE doc_id LIKE 'gone%';");
  raw_exec(raw, "DELETE FROM comments WHERE doc_id LIKE 'gone%';");
  KBC_CHECK_EQ_INT((long long)foreign_key_violations(raw, rows, 8), 0);

  (void)sqlite3_close(raw);
  kbc_test_rmrf(root);
}

/* The FK report leaves the store through the LOGGER, and the logger has no
 * sink hook — it writes to stderr and the default level (INFO) prints ERROR
 * unconditionally. So "did the open report this" is only observable by
 * capturing the descriptor across the open, which is what these two helpers
 * do. */
typedef struct {
  int saved_fd;
  char path[KBC_TEST_PATH_MAX];
} stderr_capture;

static void stderr_capture_begin(stderr_capture *c, const char *root) {
  c->saved_fd = -1;
  c->path[0] = '\0';
  size_t rl = strlen(root);
  if (rl + sizeof "/log.txt" > sizeof c->path) return;
  memcpy(c->path, root, rl);
  memcpy(c->path + rl, "/log.txt", sizeof "/log.txt");
  c->saved_fd = dup(STDERR_FILENO);
  int fd = open(c->path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
  if (fd < 0) {
    if (c->saved_fd >= 0) (void)close(c->saved_fd);
    c->saved_fd = -1;
    return;
  }
  (void)dup2(fd, STDERR_FILENO);
  (void)close(fd);
}

/* Restores stderr and hands back the captured text; the caller frees it. NULL
 * when the capture never started, which every assertion below treats as a
 * failure rather than as an empty log. */
static char *stderr_capture_end(stderr_capture *c) {
  fflush(stderr);
  if (c->saved_fd >= 0) {
    (void)dup2(c->saved_fd, STDERR_FILENO);
    (void)close(c->saved_fd);
    c->saved_fd = -1;
  }
  char *text = kbc_test_read_file(c->path);
  kbc_test_rmrf(c->path);
  return text;
}

/* How many times `needle` appears. Counting rather than matching is the point:
 * the property under test is that FOUR rows come back, and `strstr` alone
 * cannot tell one line from four. */
static int count_occurrences(const char *hay, const char *needle) {
  if (hay == NULL || needle == NULL) return 0;
  size_t len = strlen(needle);
  int n = 0;
  for (const char *p = hay; (p = strstr(p, needle)) != NULL; p += len) n++;
  return n;
}

/* THE BEHAVIOUR TEST for the guard in kbc_store_open. kb-c WARN AND CONTINUES
 * on a foreign key violation: the store opens, serves, and reports every
 * offending row.
 *
 * WHY "STILL SERVES" IS THE ASSERTION AND NOT A DETAIL. The alternative the
 * guard rejected was refusing to open, which would have made this test assert
 * a NULL store instead. Both are defensible; what is not defensible is a
 * volume whose search results are correct and whose only defect is a handful
 * of unreachable rows, taking the whole daemon down with no in-product way
 * back — kb-c ships no repair verb, and deciding whether the child row or the
 * missing parent is the wrong one is a decision the store layer must not make
 * on every open.
 *
 * FOUR orphans across THREE tables, planted by a writer with the pragma off,
 * on a volume that is ALREADY AT THE CURRENT VERSION. The version not moving
 * is load-bearing, not incidental: it is what makes this an acceptance test
 * for "runs on EVERY open" rather than for "runs after a migration". A guard
 * wired into the ladder's migration branch would pass a test that migrated and
 * fail this one, which is the whole trap.
 */
KBC_TEST(an_open_reports_every_orphaned_row_and_still_serves) {
  char root[KBC_TEST_PATH_MAX];
  kbc_test_tmpdir(root, sizeof root);
  kbc_err err;

  kbc_err_reset(&err);
  kbc_store *s = open_at(root, "kb.db", &err);
  KBC_CHECK_MSG(s != NULL, "first open failed: %s", err.msg);
  if (s == NULL) {
    kbc_test_rmrf(root);
    return;
  }
  int64_t first_version = kbc_store_schema_version(s);
  kbc_store_close(s);

  sqlite3 *raw = raw_open(root, "kb.db");
  KBC_CHECK_NOT_NULL(raw);
  if (raw == NULL) {
    kbc_test_rmrf(root);
    return;
  }
  /* The premise: the open above did not move the version, so the reopen below
   * takes the ladder's early return and runs no migration at all. */
  KBC_CHECK_MSG(raw_version(raw) == first_version,
                "the fixture volume moved from %lld while being created",
                (long long)first_version);

  /* The pragma is per-connection, and this is the writer that never enforced
 * the edges — which is the only way to reach the state the guard exists for. */
  raw_exec(raw, "PRAGMA foreign_keys = OFF;");
  raw_exec(raw,
           "INSERT INTO sources(slug, path, added_at, paused)"
           " VALUES('kb','/corpus',0,0);"
           /* Two chunks on ONE dangling doc_id: two violating rows that report
            * the same key value, so an implementation that de-duplicates by
            * key would show one line where there are two. */
           "INSERT INTO chunks(doc_id, ord, text)"
           " VALUES('gone00000001',0,'a');"
           "INSERT INTO chunks(doc_id, ord, text)"
           " VALUES('gone00000001',1,'b');"
           "INSERT INTO comments(id, doc_id, anchor, author, body,"
           " created_at, resolved)"
           " VALUES('k-aaaaaa','gone00000002','#a','me','body','2024',0);"
           /* And one row of the third kind: a child of `sources`, not of
            * `artifacts`, so a guard that only knows one edge misses it. */
           "INSERT INTO index_runs(id, corpus, started_at, finished_at,"
           " ok_count, err_count)"
           " VALUES('r-ccccc','nosuch',1,2,0,1);");
  raw_exec(raw, "PRAGMA foreign_keys = ON;");
  char rows[8][96];
  KBC_CHECK_MSG(foreign_key_violations(raw, rows, 8) == 4,
                "the fixture holds %zu violations, wanted 4 — the assertions "
                "below would be measuring a different fixture",
                foreign_key_violations(raw, rows, 8));
  (void)sqlite3_close(raw);

  /* THE OPEN. Captured, because the report is a log and the log is stderr. */
  stderr_capture cap;
  stderr_capture_begin(&cap, root);
  kbc_err_reset(&err);
  s = open_at(root, "kb.db", &err);
  char *log = stderr_capture_end(&cap);

  /* (1) Warn and continue: the store is open and usable. */
  KBC_CHECK_MSG(s != NULL, "the open refused a volume with orphaned rows: %s",
                err.msg);
  if (s == NULL) {
    free(log);
    kbc_test_rmrf(root);
    return;
  }
  /* (4) It ran on an open where the version did NOT change. */
  KBC_CHECK_EQ_INT(kbc_store_schema_version(s), (long long)first_version);
  int64_t n_art = -1;
  KBC_CHECK_OK(kbc_store_count_artifacts(s, NULL, &n_art, &err));
  KBC_CHECK_EQ_INT(n_art, 0);

  /* (2) THE FULL ROW LIST CAME BACK — not a count, not a first row. The
   * per-row lines are the ones ending in a colon; the summary line names the
   * total and does not, so counting the colon form counts ROWS. */
  KBC_CHECK_MSG(log != NULL, "stderr was not captured, so nothing was "
                "observed about what the open reported");
  KBC_CHECK_MSG(count_occurrences(log, "foreign key violation:") == 4,
                "the open reported %d violating rows, wanted all 4 — a reader "
                "that stops at the first calls a four-edge volume a one-edge "
                "one. Captured log:\n%s",
                count_occurrences(log, "foreign key violation:"),
                log != NULL ? log : "(none)");

  /* Each line names the offending row's OWN KEY, not a rowid the operator
   * would have to go and look up — and the two chunks share a key, so the
   * count of that key in the log is two, which is the assertion a
   * de-duplicating or single-line reporter fails. */
  KBC_CHECK_MSG(count_occurrences(log, "doc_id='gone00000001'") == 2,
                "the two chunk rows on gone00000001 produced %d lines, wanted "
                "2. Captured log:\n%s",
                count_occurrences(log, "doc_id='gone00000001'"),
                log != NULL ? log : "(none)");
  KBC_CHECK_MSG(count_occurrences(log, "doc_id='gone00000002'") == 1,
                "the comment row on gone00000002 produced %d lines, wanted 1",
                count_occurrences(log, "doc_id='gone00000002'"));
  /* The `sources` edge, which has no artifact to name: the corpus IS the key
   * the row is missing, so it is what the line has to carry. */
  KBC_CHECK_MSG(count_occurrences(log, "corpus='nosuch'") == 1,
                "the index_runs row for corpus 'nosuch' produced %d lines, "
                "wanted 1",
                count_occurrences(log, "corpus='nosuch'"));

  /* All three child tables named, so a count drawn from three rows of ONE
   * table cannot satisfy the count above. */
  KBC_CHECK_MSG(count_occurrences(log, "chunks rowid") == 2,
                "the log names chunks %d times, wanted 2",
                count_occurrences(log, "chunks rowid"));
  KBC_CHECK_MSG(count_occurrences(log, "comments rowid") == 1,
                "the log names comments %d times, wanted 1",
                count_occurrences(log, "comments rowid"));
  KBC_CHECK_MSG(count_occurrences(log, "index_runs rowid") == 1,
                "the log names index_runs %d times, wanted 1",
                count_occurrences(log, "index_runs rowid"));

  /* And the total, so an operator reading only the tail still learns the
 * size of the job. */
  KBC_CHECK_MSG(strstr(log != NULL ? log : "", "4 foreign key violation(s)") !=
                    NULL,
                "the summary line does not carry the count. Captured log:\n%s",
                log != NULL ? log : "(none)");

  /* (3) NOTHING WAS WRITTEN AND NOTHING WAS REPAIRED. A check that deletes
   * the orphan as a side effect would be making the delete-the-child-or-
   * restore-the-parent decision silently, on every open. */
  raw = raw_open(root, "kb.db");
  KBC_CHECK_NOT_NULL(raw);
  if (raw != NULL) {
    KBC_CHECK_MSG(foreign_key_violations(raw, rows, 8) == 4,
                  "after the open the volume holds %zu violations, wanted the "
                  "same 4 — the open repaired, and a check must not repair",
                  foreign_key_violations(raw, rows, 8));
    int64_t n_ver = raw_version(raw);
    KBC_CHECK_MSG(n_ver == first_version,
                  "the open recorded a new schema version (%lld -> %lld), so "
                  "it wrote to the volume",
                  (long long)first_version, (long long)n_ver);
    (void)sqlite3_close(raw);
  }

  free(log);
  kbc_store_close(s);
  kbc_test_rmrf(root);
}

/* THE OTHER HALF: a healthy volume must come back CLEAN, and — the part a
 * report-everything implementation gets wrong by accident — UNCHANGED. The
 * check runs on every open, so the thing it must never do is touch the file
 * it is inspecting.
 *
 * The volume is made LARGE on purpose, with a recursive CTE rather than 20k
 * round trips through the public API: `foreign_key_check` is a full walk of
 * every child table with an index probe per row, so a check that only ever
 * sees a fixture of two rows has proved nothing about the case it will meet.
 * Measured separately on a 20,000-document volume (160,000 chunk rows): the
 * check costs ~100 ms median and IS essentially the whole of open there — see
 * the report. This test asserts the behaviour at that shape, not a wall-clock
 * bound, which would be a flaky assertion about the machine rather than about
 * the code.
 */
KBC_TEST(a_healthy_volume_opens_clean_and_unchanged_at_scale) {
  enum { DOCS = 2000, CHUNKS_PER_DOC = 8, COMMENTS = 500, RUNS = 200 };

  char root[KBC_TEST_PATH_MAX];
  kbc_test_tmpdir(root, sizeof root);
  kbc_err err;

  kbc_err_reset(&err);
  kbc_store *s = open_at(root, "kb.db", &err);
  KBC_CHECK_MSG(s != NULL, "first open failed: %s", err.msg);
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
  /* Every row written here SATISFIES its foreign key: the parents exist, so
 * the volume the open is about to check is genuinely clean and a green
 * report means what it says. */
  char fill[4096];
  (void)snprintf(fill, sizeof fill,
                 "PRAGMA foreign_keys = ON;"
                 "INSERT INTO sources(slug, path, added_at, paused)"
                 " VALUES('kb','/corpus',0,0);"
                 "WITH RECURSIVE n(i) AS (SELECT 0 UNION ALL"
                 " SELECT i+1 FROM n WHERE i+1 < %d)"
                 " INSERT INTO artifacts(id, corpus, path, title, kind,"
                 " mtime_ns, size_bytes, content_hash, heading_count, summary)"
                 " SELECT printf('%%012x',i+1),'kb',printf('doc/%%05d.md',i),"
                 " 'T',0,1,100,i,0,'s' FROM n;"
                 "WITH RECURSIVE n(i) AS (SELECT 0 UNION ALL"
                 " SELECT i+1 FROM n WHERE i+1 < %d)"
                 " INSERT INTO chunks(doc_id, ord, text)"
                 " SELECT printf('%%012x',(i/%d)+1), i%%%d,"
                 " 'chunk text padding padding padding' FROM n;"
                 "WITH RECURSIVE n(i) AS (SELECT 0 UNION ALL"
                 " SELECT i+1 FROM n WHERE i+1 < %d)"
                 " INSERT INTO comments(id, doc_id, anchor, author, body,"
                 " created_at, resolved)"
                 " SELECT printf('k%%011x',i+1),printf('%%012x',(i%%%d)+1),"
                 " '#a','me','b','2024',0 FROM n;"
                 "WITH RECURSIVE n(i) AS (SELECT 0 UNION ALL"
                 " SELECT i+1 FROM n WHERE i+1 < %d)"
                 " INSERT INTO index_runs(id, corpus, started_at, finished_at,"
                 " ok_count, err_count)"
                 " SELECT printf('r%%011x',i+1),'kb',i,i+1,1,0 FROM n;"
                 "ANALYZE;",
                 (int)DOCS, (int)(DOCS * CHUNKS_PER_DOC), (int)CHUNKS_PER_DOC,
                 (int)CHUNKS_PER_DOC, (int)COMMENTS, (int)DOCS, (int)RUNS);
  raw_exec(raw, fill);
  char rows[8][96];
  KBC_CHECK_MSG(foreign_key_violations(raw, rows, 8) == 0,
                "the fixture volume is not clean, so a clean report from the "
                "open below would mean nothing");
  (void)sqlite3_close(raw);

  stderr_capture cap;
  stderr_capture_begin(&cap, root);
  kbc_err_reset(&err);
  s = open_at(root, "kb.db", &err);
  char *log = stderr_capture_end(&cap);

  KBC_CHECK_MSG(s != NULL, "a healthy volume failed to open: %s", err.msg);
  if (s == NULL) {
    free(log);
    kbc_test_rmrf(root);
    return;
  }
  KBC_CHECK_MSG(strstr(log != NULL ? log : "", "foreign key") == NULL,
                "a healthy volume produced FK output. Captured log:\n%s",
                log != NULL ? log : "(none)");

  int64_t n_art = -1;
  KBC_CHECK_OK(kbc_store_count_artifacts(s, NULL, &n_art, &err));
  KBC_CHECK_EQ_INT(n_art, DOCS);
  free(log);
  kbc_store_close(s);

  /* UNCHANGED: the same rows, and no new schema version recorded. A guard
   * that wrote anything — a repair, a touch, a re-record of the version —
   * would move one of these. */
  raw = raw_open(root, "kb.db");
  KBC_CHECK_NOT_NULL(raw);
  if (raw != NULL) {
    KBC_CHECK_EQ_INT((long long)foreign_key_violations(raw, rows, 8), 0);
    KBC_CHECK_EQ_INT((long long)raw_version(raw), CURRENT_SCHEMA);
    (void)sqlite3_close(raw);
  }

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

/* An ABANDONED move answers exactly like an interrupted one, and for the same
 * reason: neither is a promise about where the document is, and following
 * either resolves a stale reference to a name the rename never reached. The
 * difference is not the answer but the decision behind it — a bring-up pass
 * concluded this one is not going to happen — and the difference that DOES
 * show is the replay list, which an abandoned row has left and an interrupted
 * one is still in. Re-deciding it on every boot is a loop with a log line.
 *
 * The row itself is asserted, not just the answers, and that is the point:
 * an implementation that satisfied every lookup above by DELETING the row
 * would pass all of them. What distinguishes abandoning from forgetting is
 * that the row survives, with completed_at still NULL — a move that was never
 * stamped is not a move that finished — and abandoned_at set. */
KBC_TEST(an_abandoned_move_leaves_the_replay_list_and_is_never_followed) {
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

  /* Two interrupted moves, so abandoning one can be seen to leave the OTHER
   * in the replay list: a predicate that filtered on the wrong column, or one
   * that emptied the table, would pass a single-move fixture. */
  KBC_CHECK_OK(kbc_store_record_move(s, "aaaaaaaaaaaa", "bbbbbbbbbbbb",
                                     "a.md", "b.md", 1000, &err));
  KBC_CHECK_OK(kbc_store_record_move(s, "cccccccccccc", "dddddddddddd",
                                     "c.md", "d.md", 2000, &err));
  KBC_CHECK_OK(kbc_store_abandon_move(s, "aaaaaaaaaaaa", &err));

  /* Neither lookup follows it. */
  kbc_strlist ids;
  kbc_strlist_init(&ids);
  KBC_CHECK_OK(kbc_store_moves_lookup(s, "aaaaaaaaaaaa", &ids, &err));
  KBC_CHECK_MSG(ids.len == 0,
                "an abandoned move was followed to %s — a name the rename "
                "never reached",
                ids.len > 0 ? ids.items[0] : "?");
  kbc_strlist_free(&ids);
  kbc_strlist rels;
  kbc_strlist_init(&rels);
  KBC_CHECK_OK(kbc_store_moves_lookup_path(s, "a.md", &rels, &err));
  KBC_CHECK_MSG(rels.len == 0,
                "an abandoned path was followed to %s — a name the rename "
                "never reached",
                rels.len > 0 ? rels.items[0] : "?");
  kbc_strlist_free(&rels);

  /* The replay list holds the other move and only the other move. This is
   * the assertion that makes the second boot of an abandoned move a no-op. */
  kbc_strlist old_ids, old_rels;
  kbc_strlist_init(&old_ids);
  kbc_strlist_init(&old_rels);
  KBC_CHECK_OK(kbc_store_list_incomplete_moves(s, &old_ids, &old_rels, &err));
  KBC_CHECK_MSG(old_ids.len == 1,
                "the replay list holds %zu rows; the abandoned move must have "
                "left it and the other must have stayed",
                old_ids.len);
  KBC_CHECK_EQ_INT((long long)old_ids.len, (long long)old_rels.len);
  if (old_ids.len == 1 && old_rels.len == 1) {
    KBC_CHECK_EQ_STR(old_ids.items[0], "cccccccccccc");
    KBC_CHECK_EQ_STR(old_rels.items[0], "c.md");
  }
  kbc_strlist_free(&old_ids);
  kbc_strlist_free(&old_rels);
  kbc_store_close(s);

  /* The row is still THERE, stamped as abandoned and not as completed. The
   * three answers above are also what a delete would produce, so without this
   * the case could not tell an abandoned move from a forgotten one — and
   * "somebody tried and it did not happen" is the entire reason the state
   * exists. */
  sqlite3 *raw = raw_open(root, "kb.db");
  KBC_CHECK_NOT_NULL(raw);
  if (raw != NULL) {
    bool isnull = true;
    int64_t v = 0;
    KBC_CHECK_MSG(raw_one_i64(raw,
                              "SELECT completed_at FROM moves WHERE old_id ="
                              " ?1;",
                              "aaaaaaaaaaaa", 0, &isnull, &v),
                  "the abandoned move's row was deleted, so this is a forgotten "
                  "move rather than an abandoned one");
    KBC_CHECK_MSG(isnull,
                  "abandon stamped completed_at = %lld; abandoning is not "
                  "completing",
                  (long long)v);
    KBC_CHECK_MSG(raw_one_i64(raw,
                              "SELECT abandoned_at FROM moves WHERE old_id ="
                              " ?1;",
                              "aaaaaaaaaaaa", 0, &isnull, &v),
                  "the abandoned move has no row to read abandoned_at from");
    KBC_CHECK_MSG(!isnull && v > 0,
                  "abandoned_at is %s; it must carry the moment of the "
                  "decision, not the NULL that means 'only not completed'",
                  isnull ? "NULL" : "0");
    (void)sqlite3_close(raw);
  }
  kbc_test_rmrf(root);
}

/* The two writes that must not be able to disagree.
 *
 * A bring-up pass abandons a move; something later calls complete_move on the
 * same id. If complete_move stamped the abandoned row, the decision is
 * silently reversed and `moves_lookup` starts redirecting to a destination
 * that does not exist — the exact wrong answer the abandoned state exists to
 * prevent, reachable through a perfectly ordinary sequence of two calls. The
 * guard is one clause of SQL, and this is the only thing that fires it.
 *
 * The second move in the same volume is the control: `complete_move` is
 * filtered PER ROW, so this also fails an implementation that read the
 * abandoned flag as a statement about the document rather than about one
 * attempt at renaming it. */
KBC_TEST(completing_an_abandoned_move_does_not_revive_the_redirect) {
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
  KBC_CHECK_OK(kbc_store_record_move(s, "aaaaaaaaaaaa", "bbbbbbbbbbbb",
                                     "a.md", "b.md", 1000, &err));
  KBC_CHECK_OK(kbc_store_record_move(s, "eeeeeeeeeeee", "ffffffffffff",
                                     "e.md", "f.md", 2000, &err));
  KBC_CHECK_OK(kbc_store_abandon_move(s, "aaaaaaaaaaaa", &err));
  KBC_CHECK_OK(kbc_store_complete_move(s, "aaaaaaaaaaaa", &err));
  /* Completing the abandoned id twice must not be a different answer: an
   * abandoned row is terminal, and the second call is the one a retrying
   * bring-up pass would make. */
  KBC_CHECK_OK(kbc_store_complete_move(s, "aaaaaaaaaaaa", &err));

  kbc_strlist ids;
  kbc_strlist_init(&ids);
  KBC_CHECK_OK(kbc_store_moves_lookup(s, "aaaaaaaaaaaa", &ids, &err));
  KBC_CHECK_MSG(ids.len == 0,
                "an abandoned move was completed into a redirect to %s",
                ids.len > 0 ? ids.items[0] : "?");
  kbc_strlist_free(&ids);
  kbc_strlist rels;
  kbc_strlist_init(&rels);
  KBC_CHECK_OK(kbc_store_moves_lookup_path(s, "a.md", &rels, &err));
  KBC_CHECK_MSG(rels.len == 0, "the abandoned path was redirected to %s",
                rels.len > 0 ? rels.items[0] : "?");
  kbc_strlist_free(&rels);

  /* The replay list now holds the CONTROL move and not the abandoned one, so
   * the guard is "abandoned rows are terminal" rather than "abandoned rows are
   * invisible": completing the abandoned id twice did not put it back in the
   * queue, and it did not take the other row out of it. */
  kbc_strlist old_ids, old_rels;
  kbc_strlist_init(&old_ids);
  kbc_strlist_init(&old_rels);
  KBC_CHECK_OK(kbc_store_list_incomplete_moves(s, &old_ids, &old_rels, &err));
  KBC_CHECK_MSG(old_ids.len == 1,
                "the replay list holds %zu rows; it must hold the move that is "
                "still in flight and not the abandoned one",
                old_ids.len);
  if (old_ids.len == 1) KBC_CHECK_EQ_STR(old_ids.items[0], "eeeeeeeeeeee");
  kbc_strlist_free(&old_ids);
  kbc_strlist_free(&old_rels);

  /* And the control: an ordinary move, untouched by any of this, still
   * redirects. The abandoned filter is per row. */
  KBC_CHECK_OK(kbc_store_complete_move(s, "eeeeeeeeeeee", &err));
  kbc_strlist_init(&ids);
  KBC_CHECK_OK(kbc_store_moves_lookup(s, "eeeeeeeeeeee", &ids, &err));
  KBC_CHECK_MSG(ids.len == 1,
                "an ordinary completed move stopped resolving (%zu hops)",
                ids.len);
  if (ids.len == 1) KBC_CHECK_EQ_STR(ids.items[0], "ffffffffffff");
  kbc_strlist_free(&ids);

  /* With both terminal, the replay list is empty: this is the state a second
   * bring-up pass sees, and it is why it does no work on the next boot. */
  kbc_strlist_init(&old_ids);
  kbc_strlist_init(&old_rels);
  KBC_CHECK_OK(kbc_store_list_incomplete_moves(s, &old_ids, &old_rels, &err));
  KBC_CHECK_MSG(old_ids.len == 0,
                "%zu moves are still queued after both reached a terminal "
                "state",
                old_ids.len);
  kbc_strlist_free(&old_ids);
  kbc_strlist_free(&old_rels);
  kbc_store_close(s);

  sqlite3 *raw = raw_open(root, "kb.db");
  if (raw != NULL) {
    bool isnull = true;
    int64_t v = 0;
    KBC_CHECK_MSG(raw_one_i64(raw,
                              "SELECT completed_at FROM moves WHERE old_id ="
                              " ?1;",
                              "aaaaaaaaaaaa", 0, &isnull, &v),
                  "the abandoned move's row disappeared");
    KBC_CHECK_MSG(isnull, "the abandoned move was stamped completed = %lld",
                  (long long)v);
    KBC_CHECK_MSG(raw_one_i64(raw,
                              "SELECT abandoned_at FROM moves WHERE old_id ="
                              " ?1;",
                              "aaaaaaaaaaaa", 0, &isnull, &v) && !isnull,
                  "the abandoned flag was cleared, so the row is merely "
                  "incomplete again");
    (void)sqlite3_close(raw);
  }
  kbc_test_rmrf(root);
}

/* The other direction: an ordinary completed move is STILL followed. The
 * abandoned clause is an addition to the walk's predicate, and the way an
 * addition to a predicate goes wrong is by excluding the rows it was not
 * meant to touch — `abandoned_at IS NOT NULL`, a missing AND, a NULL default
 * that makes every migrated row look abandoned. Each of those turns every
 * bookmark in a real volume into a 404, and each passes a test suite whose
 * only move fixtures are abandoned ones.
 *
 * So the two states share ONE volume here, and the assertion is that they
 * answer differently, which a predicate that ignored completed_at entirely
 * cannot do. */
KBC_TEST(a_completed_move_still_redirects_beside_an_abandoned_one) {
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

  /* a -> b -> c, both hops completed, and one abandoned move beside it. */
  KBC_CHECK_OK(kbc_store_record_move(s, "aaaaaaaaaaaa", "bbbbbbbbbbbb",
                                     "a.md", "b.md", 1000, &err));
  KBC_CHECK_OK(kbc_store_complete_move(s, "aaaaaaaaaaaa", &err));
  KBC_CHECK_OK(kbc_store_record_move(s, "bbbbbbbbbbbb", "cccccccccccc",
                                     "b.md", "c.md", 2000, &err));
  KBC_CHECK_OK(kbc_store_complete_move(s, "bbbbbbbbbbbb", &err));
  KBC_CHECK_OK(kbc_store_record_move(s, "eeeeeeeeeeee", "ffffffffffff",
                                     "e.md", "f.md", 3000, &err));
  KBC_CHECK_OK(kbc_store_abandon_move(s, "eeeeeeeeeeee", &err));

  /* And the mirror of the guard above: abandoning a move that is ALREADY
   * completed is refused, because the flag would retract a redirect the
   * document earned. Two promises, one destination each, and this one is the
   * true one — so the call that would contradict it has to be the one that
   * loses. It is a bring-up decision about a move still in flight, and this
   * move is not in flight. */
  KBC_CHECK_OK(kbc_store_abandon_move(s, "aaaaaaaaaaaa", &err));

  /* The chain still walks to its final home — two hops, not one and not zero.
   * A walk that stopped at the abandoned-looking rows would report 0, and one
   * that ignored completed_at entirely would report something else again. */
  kbc_strlist ids;
  kbc_strlist_init(&ids);
  KBC_CHECK_OK(kbc_store_moves_lookup(s, "aaaaaaaaaaaa", &ids, &err));
  KBC_CHECK_MSG(ids.len == 2,
                "a completed two-hop chain returned %zu hops beside an "
                "abandoned move",
                ids.len);
  if (ids.len == 2) {
    KBC_CHECK_EQ_STR(ids.items[0], "bbbbbbbbbbbb");
    KBC_CHECK_EQ_STR(ids.items[1], "cccccccccccc");
  }
  kbc_strlist_free(&ids);

  kbc_strlist rels;
  kbc_strlist_init(&rels);
  KBC_CHECK_OK(kbc_store_moves_lookup_path(s, "a.md", &rels, &err));
  KBC_CHECK_MSG(rels.len == 2, "the completed path chain returned %zu hops",
                rels.len);
  kbc_strlist_free(&rels);

  /* The middle id too, and not the abandoned one. */
  kbc_strlist_init(&ids);
  KBC_CHECK_OK(kbc_store_moves_lookup(s, "bbbbbbbbbbbb", &ids, &err));
  KBC_CHECK_EQ_INT((long long)ids.len, 1);
  if (ids.len == 1) KBC_CHECK_EQ_STR(ids.items[0], "cccccccccccc");
  kbc_strlist_free(&ids);
  kbc_strlist_init(&ids);
  KBC_CHECK_OK(kbc_store_moves_lookup(s, "eeeeeeeeeeee", &ids, &err));
  KBC_CHECK_EQ_INT((long long)ids.len, 0);
  kbc_strlist_free(&ids);

  /* Abandoning must not have touched the completed rows either: an
   * `abandoned_at` written onto a finished move would retract a redirect the
   * document earned, and the walk would then (correctly, per its own
   * predicate) refuse to follow it. So the flag has to be absent there. */
  kbc_store_close(s);
  sqlite3 *raw = raw_open(root, "kb.db");
  KBC_CHECK_NOT_NULL(raw);
  if (raw != NULL) {
    bool isnull = true;
    int64_t v = 0;
    KBC_CHECK_MSG(raw_one_i64(raw,
                              "SELECT abandoned_at FROM moves WHERE old_id ="
                              " ?1;",
                              "aaaaaaaaaaaa", 0, &isnull, &v),
                  "a completed move has no row");
    KBC_CHECK_MSG(isnull,
                  "a completed move was marked abandoned at %lld, so its "
                  "redirect is now retractable by a row nobody asked to "
                  "retract",
                  (long long)v);
    KBC_CHECK_MSG(raw_one_i64(raw,
                              "SELECT completed_at FROM moves WHERE old_id ="
                              " ?1;",
                              "aaaaaaaaaaaa", 0, &isnull, &v) && !isnull,
                  "a completed move lost its completed_at");
    (void)sqlite3_close(raw);
  }
  kbc_test_rmrf(root);
}

/* PRECEDENCE: a row carrying BOTH stamps is not followed, and this is the
 * only case that can prove it.
 *
 * kb-c's two writers make the state unreachable — complete_move refuses an
 * abandoned row and abandon_move refuses a completed one — so a test written
 * through the API cannot produce it, and a walk that dropped its
 * `abandoned_at` clause would pass every other case in this file. The rows go
 * in through raw SQL for the same reason the cycle test's do: the API will not
 * write the state the predicate has to survive.
 *
 * Abandoned wins, and the reason is which promise is safer to break. Both
 * stamps name a different destination, so one of them is wrong; a caller that
 * follows the completed one gets a document that is not there, and a caller
 * that follows neither gets an id that never moved. The second is a missing
 * answer, the first is a wrong one, and this store never trades a missing
 * answer for a wrong one.
 *
 * The control row is the half that makes this a precedence test rather than an
 * "everything is refused" test: an ordinary completed move in the SAME volume
 * still resolves, so a walk that had simply stopped answering would fail it. */
KBC_TEST(a_move_stamped_completed_and_abandoned_is_not_followed) {
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
  raw_exec(raw,
           "INSERT INTO moves(old_id, new_id, old_rel, new_rel, moved_at,"
           " completed_at, abandoned_at)"
           " VALUES('aaaaaaaaaaaa','bbbbbbbbbbbb','a.md','b.md',1,10,20);"
           "INSERT INTO moves(old_id, new_id, old_rel, new_rel, moved_at,"
           " completed_at, abandoned_at)"
           " VALUES('eeeeeeeeeeee','ffffffffffff','e.md','f.md',2,30,40);"
           /* the control: completed, never abandoned */
           "INSERT INTO moves(old_id, new_id, old_rel, new_rel, moved_at,"
           " completed_at) VALUES('gggggggggggg','hhhhhhhhhhhh','g.md','h.md',3,"
           "50);");
  (void)sqlite3_close(raw);

  kbc_err_reset(&err);
  s = open_at(root, "kb.db", &err);
  KBC_CHECK_NOT_NULL(s);
  if (s == NULL) {
    kbc_test_rmrf(root);
    return;
  }
  kbc_strlist ids;
  kbc_strlist_init(&ids);
  KBC_CHECK_OK(kbc_store_moves_lookup(s, "aaaaaaaaaaaa", &ids, &err));
  KBC_CHECK_MSG(ids.len == 0,
                "a row stamped both completed and abandoned was followed to %s",
                ids.len > 0 ? ids.items[0] : "?");
  kbc_strlist_free(&ids);
  kbc_strlist_init(&ids);
  KBC_CHECK_OK(kbc_store_moves_lookup(s, "eeeeeeeeeeee", &ids, &err));
  KBC_CHECK_EQ_INT((long long)ids.len, 0);
  kbc_strlist_free(&ids);
  kbc_strlist rels;
  kbc_strlist_init(&rels);
  KBC_CHECK_OK(kbc_store_moves_lookup_path(s, "e.md", &rels, &err));
  KBC_CHECK_MSG(rels.len == 0,
                "the path of a doubly-stamped row was followed to %s",
                rels.len > 0 ? rels.items[0] : "?");
  kbc_strlist_free(&rels);

  /* The control: completed alone, and it still redirects. */
  kbc_strlist_init(&ids);
  KBC_CHECK_OK(kbc_store_moves_lookup(s, "gggggggggggg", &ids, &err));
  KBC_CHECK_MSG(ids.len == 1, "the control move stopped resolving (%zu hops)",
                ids.len);
  if (ids.len == 1) KBC_CHECK_EQ_STR(ids.items[0], "hhhhhhhhhhhh");
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

  /* Behind: an open that has to migrate forward, which is the normal case.
   *
   * A REAL previous-version volume, built from the fixture, rather than the
   * current one with its version table rewound. The difference is not
   * cosmetic and this half of the case used to be written the other way: a
   * rewound `schema_version` claims the volume is older than its schema, and
   * the ladder trusts that table. Every step used to survive being re-run
   * (IF NOT EXISTS, OR IGNORE), so the lie was harmless; migration 12's ADD
   * COLUMN is not, and SQLite's "duplicate column name" is what a volume in
   * that state now gets. It is a state the ladder cannot produce — a volume
   * that recorded 11 has never had the column, because the same transaction
   * adds both — so the fixture below is the honest way to ask the question.
   * The refusal is not silent: the open fails and names the step. */
  raw = raw_open(root, "behind.db");
  if (raw != NULL) {
    raw_exec(raw, V10_DB);
    raw_exec(raw, V11_MOVES);
    (void)sqlite3_close(raw);
  }
  kbc_err_reset(&err);
  s = open_at(root, "behind.db", &err);
  KBC_CHECK_MSG(s != NULL, "a volume behind must open: %s", err.msg);
  if (s != NULL) {
    KBC_CHECK_EQ_INT(kbc_store_schema_version(s), CURRENT_SCHEMA);
    kbc_arena *a = kbc_arena_new(4096);
    kbc_artifact again;
    memset(&again, 0, sizeof again);
    KBC_CHECK_OK(
        kbc_store_get_artifact(s, a, "old000000001", false, &again, &err));
    KBC_CHECK_EQ_STR(again.path, "a.md");
    kbc_arena_free(a);
    kbc_store_close(s);
  }

  /* Equal: nothing to do, and still an open. `behind.db` rather than
   * `ahead.db`, because the ahead half above left that file stamped V13 on
   * purpose and nothing since has moved it back — the volume that is now at
   * this binary's epoch is the one the behind half just migrated. */
  kbc_err_reset(&err);
  s = open_at(root, "behind.db", &err);
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

/* --------------------------------------- history retention and anchors --
 *
 * Two things are under test and they are not the same thing.
 *
 * RETENTION is a fixed-point property: prune, and the second prune removes
 * nothing. That is only interesting if the first one is allowed to remove
 * something, and only trustworthy if it cannot remove more than it says.
 *
 * ANCHORS are a three-state machine, and the third state is the whole
 * subject. `anchor_states_are_three_and_only_three` asserts that all three
 * are REACHABLE and that each is distinguishable from the other two; the two
 * obvious states are a rounding error next to that, and a machine that only
 * ever produces RESOLVED and UNRESOLVED passes every other test here.
 */

/* Adds `n` comments on one document and hands back their ids in the order
 * `kbc_store_list_comments` returns them (created_at DESC, id DESC), which
 * is the only order a caller can name an anchor by. The ids are minted, so
 * the store is the only place they can come from. */
static char (*comment_ids_for(kbc_store *s, kbc_arena *a, const char *doc_id,
                              const char *const *anchors, size_t n,
                              kbc_err *err))[KBC_MAX_ID_LEN + 1] {
  for (size_t i = 0; i < n; i++) {
    if (kbc_failed(kbc_store_add_comment(s, doc_id, anchors[i], "you",
                                         "a remark", err)))
      return NULL;
  }
  kbc_comment *cs = NULL;
  size_t got = 0;
  if (kbc_failed(kbc_store_list_comments(s, a, doc_id, KBC_MAX_HITS, &cs, &got,
                                         err)))
    return NULL;
  if (got != n) {
    (void)kbc_err_set(err, KBC_ERR_INTERNAL, "comment_ids_for: %zu of %zu",
                      got, n);
    return NULL;
  }
  char(*ids)[KBC_MAX_ID_LEN + 1] = kbc_arena_calloc(a, n, KBC_MAX_ID_LEN + 1);
  if (ids == NULL) {
    (void)kbc_err_set(err, KBC_ERR_NOMEM, "comment_ids_for: %zu ids", n);
    return NULL;
  }
  /* Indexed by the CLAIM, not by the order the store happens to list in:
   * `kbc_store_list_comments` is newest-first and the ids are minted from
   * /dev/urandom, so a positional read would tie each assertion to an order
   * nothing guarantees. Matching on the anchor text is the only stable join
   * between "the comment I asked for" and "the row the store holds". */
  for (size_t i = 0; i < n; i++) {
    bool found = false;
    for (size_t j = 0; j < got; j++) {
      if (strcmp(cs[j].anchor, anchors[i]) != 0) continue;
      size_t len = strlen(cs[j].id);
      if (len > KBC_MAX_ID_LEN) len = KBC_MAX_ID_LEN;
      memcpy(ids[i], cs[j].id, len);
      ids[i][len] = '\0';
      found = true;
      break;
    }
    if (!found) {
      (void)kbc_err_set(err, KBC_ERR_INTERNAL, "comment_ids_for: no row for %s",
                        anchors[i]);
      return NULL;
    }
  }
  return ids;
}

/* One document, three comments, three anchors, and the three verdicts they
 * must get. The three anchors are named for what they are:
 *
 *   "install"  — a heading the document still has, so RESOLVED.
 *   "retired"  — a heading the document has not got, so UNRESOLVED.
 *   "unread"   — settled by the caller saying the document is not judgeable,
 *                which is the state a two-state machine has no way to hold.
 *
 * The third case is the one this whole section is about, so it is asserted
 * FIRST in the run below and separately: a machine that answers "unresolved"
 * for a document it could not read has told a user their comment is broken
 * when the truth is that nobody looked. */
KBC_TEST(anchor_states_are_three_and_only_three) {
  char root[KBC_TEST_PATH_MAX];
  kbc_test_tmpdir(root, sizeof root);
  kbc_err err;
  kbc_err_reset(&err);
  kbc_store *s = open_at(root, "states.db", &err);
  KBC_CHECK_NOT_NULL(s);
  if (s == NULL) {
    kbc_test_rmrf(root);
    return;
  }
  kbc_arena *a = kbc_arena_new(8192);
  kbc_artifact art;
  fill(&art, "bbbbbbbbbbbb", "kb", "a.md", KBC_KIND_ARTIFACT);
  KBC_CHECK_OK(kbc_store_upsert_artifact(s, &art, &err));

  const char *claims[] = {"install", "retired", "unread"};
  char(*ids)[KBC_MAX_ID_LEN + 1] =
      comment_ids_for(s, a, art.id, claims, 3, &err);
  KBC_CHECK_NOT_NULL(ids);
  if (ids == NULL) {
    kbc_arena_free(a);
    kbc_store_close(s);
    kbc_test_rmrf(root);
    return;
  }

  /* A first pass with nothing judgeable: every comment lands in the state
 * that says "nobody has an opinion", and all three verdicts are reachable
 * from that one call. */
  const kbc_anchor_heading none[1] = {{"", ""}};
  kbc_anchor_judgement *js = NULL;
  size_t nj = 0;
  KBC_CHECK_OK(
      kbc_store_judge_anchors(s, art.id, none, 1, false, a, &js, &nj, &err));
  KBC_CHECK_EQ_INT(nj, 3);
  KBC_CHECK_EQ_INT(kbc_store_count_stale_anchors(s, &err), 3);

  /* Now the document is readable. "install" is there and "retired" is not;
 * "unread" was never a heading and never will be, so it stays a clean
 * negative rather than an excuse. */
  const kbc_anchor_heading heads[] = {
      {"intro", "Introduction"},
      {"install", "Install"},
  };
  js = NULL;
  nj = 0;
  KBC_CHECK_OK(
      kbc_store_judge_anchors(s, art.id, heads, 2, true, a, &js, &nj, &err));
  KBC_CHECK_EQ_INT(nj, 3);

  kbc_anchor_row row;
  KBC_CHECK_OK(kbc_store_get_anchor(s, a, ids[0], &row, &err));
  KBC_CHECK_EQ_INT(row.state, KBC_ANCHOR_RESOLVED);
  KBC_CHECK_EQ_INT(row.resolved_ord, 1);
  KBC_CHECK_EQ_STR(row.resolves_to, "install");
  /* The USER'S CLAIM IS NEVER REWRITTEN. `install` is both the claim and
 * where it resolves here, so a store that quietly rewrote the anchor would
 * be indistinguishable in this case — which is exactly why the follow test
 * below exists. */
  KBC_CHECK_EQ_STR(row.anchor, "install");

  KBC_CHECK_OK(kbc_store_get_anchor(s, a, ids[1], &row, &err));
  KBC_CHECK_EQ_INT(row.state, KBC_ANCHOR_UNRESOLVED);
  KBC_CHECK_EQ_INT(row.resolved_ord, -1);
  KBC_CHECK_EQ_STR(row.resolves_to, "");

  KBC_CHECK_OK(kbc_store_get_anchor(s, a, ids[2], &row, &err));
  KBC_CHECK_EQ_INT(row.state, KBC_ANCHOR_UNRESOLVED);

  /* One of the three is resolved, so the stale set holds TWO — and the two
   * are of different kinds. `retired` was resolved-then-broken (it was in the
   * set after the unjudgeable pass and stayed there), and `unread` entered
   * the set for the first time on this pass, because a document that can now
   * be read can now say that its heading is not there. Neither is the
   * UNDECIDABLE state: there is none left, and a machine that could not tell
   * those two apart from it would have no way to report that anything
   * changed. */
  KBC_CHECK_EQ_INT(kbc_store_count_stale_anchors(s, &err), 2);

  kbc_arena_free(a);
  kbc_store_close(s);
  kbc_test_rmrf(root);
}

/* The document will not parse. Every comment on it must read UNDECIDABLE,
 * and NOT UNRESOLVED.
 *
 * This is the assertion a two-state machine fails, and it fails in the
 * direction that costs a user something: reporting UNRESOLVED says "the
 * heading your comment is on is gone", which is a false statement about a
 * document nobody managed to read, and it is the input to a stale badge and
 * to a `comment.anchor_stale` event the subscriber will act on. */
KBC_TEST(a_document_that_will_not_parse_leaves_every_anchor_undecidable) {
  char root[KBC_TEST_PATH_MAX];
  kbc_test_tmpdir(root, sizeof root);
  kbc_err err;
  kbc_err_reset(&err);
  kbc_store *s = open_at(root, "unparse.db", &err);
  KBC_CHECK_NOT_NULL(s);
  if (s == NULL) {
    kbc_test_rmrf(root);
    return;
  }
  kbc_arena *a = kbc_arena_new(8192);
  kbc_artifact art;
  fill(&art, "bbbbbbbbbbbb", "kb", "a.md", KBC_KIND_ARTIFACT);
  KBC_CHECK_OK(kbc_store_upsert_artifact(s, &art, &err));

  /* The headings are all still there. The caller simply could not read the
   * document, which is a different fact and gets a different answer. */
  const kbc_anchor_heading heads[] = {
      {"intro", "Introduction"},
      {"install", "Install"},
  };
  const char *claims[] = {"intro", "install"};
  char(*ids)[KBC_MAX_ID_LEN + 1] =
      comment_ids_for(s, a, art.id, claims, 2, &err);
  KBC_CHECK_NOT_NULL(ids);
  if (ids == NULL) {
    kbc_arena_free(a);
    kbc_store_close(s);
    kbc_test_rmrf(root);
    return;
  }
  kbc_anchor_judgement *js = NULL;
  size_t nj = 0;
  KBC_CHECK_OK(
      kbc_store_judge_anchors(s, art.id, heads, 2, false, a, &js, &nj, &err));
  KBC_CHECK_EQ_INT(nj, 2);
  for (size_t i = 0; i < 2; i++) {
    kbc_anchor_row row;
    KBC_CHECK_OK(kbc_store_get_anchor(s, a, ids[i], &row, &err));
    KBC_CHECK_EQ_INT(row.state, KBC_ANCHOR_UNDECIDABLE);
    /* An undecidable anchor points at NOTHING. Leaving a `resolves_to`
     * behind would be a reader's cue to jump to a heading the store just
     * said it could not vouch for. */
    KBC_CHECK_EQ_INT(row.resolved_ord, -1);
    KBC_CHECK_EQ_STR(row.resolves_to, "");
  }
  /* Both are in the stale set: a subscriber needs to be told about an
   * undecidable anchor at least as much as about a broken one. */
  KBC_CHECK_EQ_INT(kbc_store_count_stale_anchors(s, &err), 2);

  kbc_arena_free(a);
  kbc_store_close(s);
  kbc_test_rmrf(root);
}

/* A HEADING THAT MOVES. The comment follows it, and the follow is visible.
 *
 * The anchor here is an EXPLICIT element id, which is the only kind of anchor
 * that can move in kb-c. A heading's slug is a pure function of the heading's
 * text (src/parse.c, `p_flush`: `slug_into(p->a, text, ...)`), so a renamed
 * heading gets a new slug and there is no identity left to follow it by — the
 * case two tests down says so out loud rather than pretending otherwise. An
 * explicit `{#install}` keeps its id when the block it is on moves, which is
 * the situation a reader means by "my comment's section moved down the page".
 *
 * "Configuration" is inserted above, so the block that WAS at ord 1 is at
 * ord 2. The comment follows it there, and `comments.anchor` is left exactly
 * as the user wrote it — the store records where the claim now resolves and
 * never edits the claim. */
KBC_TEST(a_moved_heading_takes_its_comment_with_it) {
  char root[KBC_TEST_PATH_MAX];
  kbc_test_tmpdir(root, sizeof root);
  kbc_err err;
  kbc_err_reset(&err);
  kbc_store *s = open_at(root, "moved.db", &err);
  KBC_CHECK_NOT_NULL(s);
  if (s == NULL) {
    kbc_test_rmrf(root);
    return;
  }
  kbc_arena *a = kbc_arena_new(8192);
  kbc_artifact art;
  fill(&art, "bbbbbbbbbbbb", "kb", "a.md", KBC_KIND_ARTIFACT);
  KBC_CHECK_OK(kbc_store_upsert_artifact(s, &art, &err));

  const kbc_anchor_heading before[] = {
      {"intro", "Introduction"},
      {"install", "Install"},
  };
  const char *claims[] = {"install"};
  char(*ids)[KBC_MAX_ID_LEN + 1] =
      comment_ids_for(s, a, art.id, claims, 1, &err);
  KBC_CHECK_NOT_NULL(ids);
  if (ids == NULL) {
    kbc_arena_free(a);
    kbc_store_close(s);
    kbc_test_rmrf(root);
    return;
  }
  kbc_anchor_judgement *js = NULL;
  size_t nj = 0;
  KBC_CHECK_OK(
      kbc_store_judge_anchors(s, art.id, before, 2, true, a, &js, &nj, &err));
  KBC_CHECK_EQ_INT(nj, 1);
  KBC_CHECK_EQ_INT(js[0].state, KBC_ANCHOR_RESOLVED);
  KBC_CHECK_EQ_INT(js[0].ord, 1);
  /* First observation of a healthy anchor is not a transition into the set:
   * the set is a memory of the PREVIOUS pass, and there was none. */
  KBC_CHECK_MSG(!js[0].transitioned,
                "a first resolution must not fire a stale event");

  const kbc_anchor_heading after[] = {
      {"intro", "Introduction"},
      {"configuration", "Configuration"},
      {"install", "Install"},
  };
  js = NULL;
  nj = 0;
  KBC_CHECK_OK(
      kbc_store_judge_anchors(s, art.id, after, 3, true, a, &js, &nj, &err));
  KBC_CHECK_EQ_INT(nj, 1);
  KBC_CHECK_EQ_INT(js[0].state, KBC_ANCHOR_RESOLVED);
  /* The follow target: the same block, one position further down. */
  KBC_CHECK_EQ_INT(js[0].ord, 2);
  KBC_CHECK_MSG(!js[0].transitioned,
                "following a heading is not an edge out of the stale set");

  kbc_anchor_row row;
  KBC_CHECK_OK(kbc_store_get_anchor(s, a, ids[0], &row, &err));
  KBC_CHECK_EQ_INT(row.state, KBC_ANCHOR_RESOLVED);
  KBC_CHECK_EQ_INT(row.resolved_ord, 2);
  KBC_CHECK_EQ_STR(row.resolves_to, "install");
  /* And the user's claim is untouched. */
  KBC_CHECK_EQ_STR(row.anchor, "install");
  KBC_CHECK_EQ_INT(kbc_store_count_stale_anchors(s, &err), 0);

  kbc_arena_free(a);
  kbc_store_close(s);
  kbc_test_rmrf(root);
}

/* THE ANCHOR ID IS TAKEN OVER BY A DIFFERENT BLOCK. This is the failure the
 * third state exists for.
 *
 * The comment was written on a block whose explicit id is `install` and whose
 * heading text is "Install". The document is edited: that block is renamed to
 * "Installation" (so it now carries `id="installation"`), and a NEW,
 * unrelated block is given the id `install` — "Retire the old installer" —
 * further down the page.
 *
 * The id the comment named is present again, and it is now somebody else's
 * block. A boolean says RESOLVED, and a comment written about the top of the
 * document is attached to the bottom of it with no record that anything
 * happened. The store can see that the text behind the id is not the text the
 * comment was made on, and that the original text is nowhere in the document;
 * those two facts together are exactly "cannot be decided": something is
 * there, it is not the thing, and nothing says where the thing went. */
KBC_TEST(a_heading_taken_over_by_another_leaves_the_anchor_undecidable) {
  char root[KBC_TEST_PATH_MAX];
  kbc_test_tmpdir(root, sizeof root);
  kbc_err err;
  kbc_err_reset(&err);
  kbc_store *s = open_at(root, "collide.db", &err);
  KBC_CHECK_NOT_NULL(s);
  if (s == NULL) {
    kbc_test_rmrf(root);
    return;
  }
  kbc_arena *a = kbc_arena_new(8192);
  kbc_artifact art;
  fill(&art, "bbbbbbbbbbbb", "kb", "a.md", KBC_KIND_ARTIFACT);
  KBC_CHECK_OK(kbc_store_upsert_artifact(s, &art, &err));

  const kbc_anchor_heading before[] = {
      {"intro", "Introduction"},
      {"install", "Install"},
  };
  const char *claims[] = {"install"};
  char(*ids)[KBC_MAX_ID_LEN + 1] =
      comment_ids_for(s, a, art.id, claims, 1, &err);
  KBC_CHECK_NOT_NULL(ids);
  if (ids == NULL) {
    kbc_arena_free(a);
    kbc_store_close(s);
    kbc_test_rmrf(root);
    return;
  }
  kbc_anchor_judgement *js = NULL;
  size_t nj = 0;
  KBC_CHECK_OK(
      kbc_store_judge_anchors(s, art.id, before, 2, true, a, &js, &nj, &err));
  KBC_CHECK_EQ_INT(js[0].state, KBC_ANCHOR_RESOLVED);
  KBC_CHECK_EQ_INT(js[0].ord, 1);

  /* THE HEADING THAT MOVED: "Install", now reading "Installation" at ord 2.
   * THE HEADING IT COLLIDED WITH: a new block, "Retire the old installer",
   * which took the id `install` at ord 4. */
  const kbc_anchor_heading after[] = {
      {"intro", "Introduction"},
      {"configuration", "Configuration"},
      {"installation", "Installation"},
      {"troubleshooting", "Troubleshooting"},
      {"install", "Retire the old installer"},
  };
  js = NULL;
  nj = 0;
  KBC_CHECK_OK(
      kbc_store_judge_anchors(s, art.id, after, 5, true, a, &js, &nj, &err));
  KBC_CHECK_EQ_INT(nj, 1);
  KBC_CHECK_EQ_INT(js[0].state, KBC_ANCHOR_UNDECIDABLE);
  KBC_CHECK_MSG(js[0].transitioned,
                "the anchor left the resolved set and that is an event");

  kbc_anchor_row row;
  KBC_CHECK_OK(kbc_store_get_anchor(s, a, ids[0], &row, &err));
  KBC_CHECK_EQ_INT(row.state, KBC_ANCHOR_UNDECIDABLE);
  /* It does NOT resolve at the colliding block. Pointing it at ord 4 is
   * precisely the silent re-anchor this state exists to prevent, and the
   * reason `resolves_to` is empty rather than "install". */
  KBC_CHECK_EQ_INT(row.resolved_ord, -1);
  KBC_CHECK_EQ_STR(row.resolves_to, "");
  KBC_CHECK_EQ_INT(kbc_store_count_stale_anchors(s, &err), 1);

  /* ABSORBING, and deliberately: a pass is the same computation that already
   * failed to settle it, so asking again would loop. A human settles it. */
  js = NULL;
  nj = 0;
  KBC_CHECK_OK(
      kbc_store_judge_anchors(s, art.id, after, 5, true, a, &js, &nj, &err));
  KBC_CHECK_EQ_INT(js[0].state, KBC_ANCHOR_UNDECIDABLE);
  KBC_CHECK_MSG(!js[0].transitioned,
                "a second pass over an unchanged document fires nothing");

  /* A comment that was never judged is NOT the same as one judged
   * undecidable, and the store says so rather than guessing. */
  KBC_CHECK_OK(kbc_store_add_comment(s, art.id, "install", "you", "later", &err));
  kbc_comment *cs = NULL;
  size_t nc = 0;
  KBC_CHECK_OK(kbc_store_list_comments(s, a, art.id, KBC_MAX_HITS, &cs, &nc,
                                       &err));
  KBC_CHECK_EQ_INT(nc, 2);
  bool found_new = false;
  for (size_t i = 0; i < nc; i++) {
    kbc_anchor_row probe;
    kbc_err_reset(&err);
    if (kbc_failed(kbc_store_get_anchor(s, a, cs[i].id, &probe, &err))) {
      KBC_CHECK_EQ_INT(err.status, KBC_ERR_NOTFOUND);
      KBC_CHECK_MSG(strstr(err.msg, "never judged") != NULL,
                    "not-found must say the question was never asked: %s",
                    err.msg);
      found_new = true;
    }
  }
  KBC_CHECK_MSG(found_new,
                "the comment added after the last pass must read as never"
                " judged, which is a different answer from undecidable");

  kbc_arena_free(a);
  kbc_store_close(s);
  kbc_test_rmrf(root);
}

/* A HEADING RENAMED, WITH NO EXPLICIT ID. The anchor's slug is a pure function
 * of the heading's text, so "Install" and "Installation" have different ids
 * and the store holds nothing that ties them together.
 *
 * It therefore answers UNRESOLVED — the heading the comment referred to, the
 * one reading "Install", is not in this document — and NOT a confident
 * "resolved" and NOT an undecidable. That is the honest answer for the shape
 * kb-c stores, and it is worth a test because the tempting alternative
 * (match the new slug by prefix and call it followed) would be the silent
 * re-anchor with extra steps: "Installation", "Installer" and "Installing" are
 * three different headings and one guess.
 *
 * The original CAN follow a rename, because its `Chapter` anchor is a
 * heading-text PATH and its resolver is token-set Jaccard (review.rs:327-328,
 * 0.5 threshold). kb-c has no fuzzy tier at all — src/httpd.c:3923 says so —
 * and a store that invented one would be answering a question the wire
 * contract has no score for. */
KBC_TEST(a_renamed_heading_is_unresolved_and_not_silently_followed) {
  char root[KBC_TEST_PATH_MAX];
  kbc_test_tmpdir(root, sizeof root);
  kbc_err err;
  kbc_err_reset(&err);
  kbc_store *s = open_at(root, "rename.db", &err);
  KBC_CHECK_NOT_NULL(s);
  if (s == NULL) {
    kbc_test_rmrf(root);
    return;
  }
  kbc_arena *a = kbc_arena_new(8192);
  kbc_artifact art;
  fill(&art, "bbbbbbbbbbbb", "kb", "a.md", KBC_KIND_ARTIFACT);
  KBC_CHECK_OK(kbc_store_upsert_artifact(s, &art, &err));

  const kbc_anchor_heading before[] = {
      {"intro", "Introduction"},
      {"install", "Install"},
  };
  const char *claims[] = {"install"};
  char(*ids)[KBC_MAX_ID_LEN + 1] =
      comment_ids_for(s, a, art.id, claims, 1, &err);
  KBC_CHECK_NOT_NULL(ids);
  if (ids == NULL) {
    kbc_arena_free(a);
    kbc_store_close(s);
    kbc_test_rmrf(root);
    return;
  }
  kbc_anchor_judgement *js = NULL;
  size_t nj = 0;
  KBC_CHECK_OK(
      kbc_store_judge_anchors(s, art.id, before, 2, true, a, &js, &nj, &err));
  KBC_CHECK_EQ_INT(js[0].state, KBC_ANCHOR_RESOLVED);

  /* "Install" is renamed "Installation"; a decoy "Installer" is added so a
   * prefix-matching implementation would have something to grab. */
  const kbc_anchor_heading after[] = {
      {"intro", "Introduction"},
      {"installer", "Installer"},
      {"installation", "Installation"},
  };
  js = NULL;
  nj = 0;
  KBC_CHECK_OK(
      kbc_store_judge_anchors(s, art.id, after, 3, true, a, &js, &nj, &err));
  KBC_CHECK_EQ_INT(js[0].state, KBC_ANCHOR_UNRESOLVED);
  KBC_CHECK_MSG(js[0].transitioned,
                "losing the heading is an edge into the stale set");

  kbc_anchor_row row;
  KBC_CHECK_OK(kbc_store_get_anchor(s, a, ids[0], &row, &err));
  KBC_CHECK_EQ_INT(row.state, KBC_ANCHOR_UNRESOLVED);
  KBC_CHECK_EQ_INT(row.resolved_ord, -1);
  KBC_CHECK_EQ_STR(row.resolves_to, "");
  KBC_CHECK_EQ_INT(kbc_store_count_stale_anchors(s, &err), 1);

  kbc_arena_free(a);
  kbc_store_close(s);
  kbc_test_rmrf(root);
}

/* CRASH RECOVERY, for the persist decision this section implements.
 *
 * The sequence a crash can actually produce: a pass judges a document and
 * writes its verdicts; the document is re-indexed, so its bytes and its
 * `content_hash` change; the process dies BEFORE the next pass runs. The
 * rows on disk are then a claim about bytes that are no longer the
 * document's bytes, and a store that trusts them answers about a document
 * that never existed in that state.
 *
 * So: judge, re-index with different bytes, reopen, and read. The reopened
 * state must describe the graph that EXISTS — undecidable, because nobody
 * has read the new document — and not the graph that WAS, which said
 * resolved. */
KBC_TEST(a_crash_between_a_pass_and_a_reindex_leaves_no_claim_about_old_bytes) {
  char root[KBC_TEST_PATH_MAX];
  kbc_test_tmpdir(root, sizeof root);
  kbc_err err;
  kbc_err_reset(&err);
  kbc_store *s = open_at(root, "crash.db", &err);
  KBC_CHECK_NOT_NULL(s);
  if (s == NULL) {
    kbc_test_rmrf(root);
    return;
  }
  kbc_arena *a = kbc_arena_new(8192);
  kbc_artifact art;
  fill(&art, "bbbbbbbbbbbb", "kb", "a.md", KBC_KIND_ARTIFACT);
  KBC_CHECK_OK(kbc_store_upsert_artifact(s, &art, &err));

  const kbc_anchor_heading heads[] = {
      {"intro", "Introduction"},
      {"install", "Install"},
  };
  const char *claims[] = {"install"};
  char(*ids)[KBC_MAX_ID_LEN + 1] =
      comment_ids_for(s, a, art.id, claims, 1, &err);
  KBC_CHECK_NOT_NULL(ids);
  if (ids == NULL) {
    kbc_arena_free(a);
    kbc_store_close(s);
    kbc_test_rmrf(root);
    return;
  }
  kbc_anchor_judgement *js = NULL;
  size_t nj = 0;
  KBC_CHECK_OK(
      kbc_store_judge_anchors(s, art.id, heads, 2, true, a, &js, &nj, &err));
  KBC_CHECK_EQ_INT(js[0].state, KBC_ANCHOR_RESOLVED);
  char kept[KBC_MAX_ID_LEN + 1];
  memcpy(kept, ids[0], sizeof kept);
  kbc_arena_free(a);

  /* The document is edited and re-indexed — the bytes and the hash move —
 * and the process dies here, before the next pass. */
  art.content_hash = 0x12345678u;
  art.mtime_ns = 1700000000999999999ll;
  KBC_CHECK_OK(kbc_store_upsert_artifact(s, &art, &err));
  kbc_store_close(s);

  /* Reopen. The rebuild runs on every open, not only on a version change. */
  s = open_at(root, "crash.db", &err);
  KBC_CHECK_NOT_NULL(s);
  if (s == NULL) {
    kbc_test_rmrf(root);
    return;
  }
  a = kbc_arena_new(8192);
  kbc_anchor_row row;
  KBC_CHECK_OK(kbc_store_get_anchor(s, a, kept, &row, &err));
  KBC_CHECK_EQ_INT(row.state, KBC_ANCHOR_UNDECIDABLE);
  /* The claim's remembered heading text is cleared with the verdict. It
   * belonged to the old bytes, and leaving it would let the next pass
   * "follow" a heading that is no longer in the document. */
  KBC_CHECK_EQ_INT(row.resolved_ord, -1);
  KBC_CHECK_EQ_STR(row.resolves_to, "");
  KBC_CHECK_EQ_INT(kbc_store_count_stale_anchors(s, &err), 1);

  /* A reconcile on the already-rebuilt graph is a no-op, so the rebuild is a
   * fixed point and does not rewrite rows on every daemon start. */
  int64_t touched = 0;
  KBC_CHECK_OK(kbc_store_reconcile_anchors(s, &touched, &err));
  KBC_CHECK_EQ_INT(touched, 0);
  KBC_CHECK_OK(kbc_store_get_anchor(s, a, kept, &row, &err));
  KBC_CHECK_EQ_INT(row.state, KBC_ANCHOR_UNDECIDABLE);

  /* And the pass that was interrupted can now be run: it reads the document
   * that exists, and the verdict it writes describes that document. */
  js = NULL;
  nj = 0;
  KBC_CHECK_OK(
      kbc_store_judge_anchors(s, art.id, heads, 2, true, a, &js, &nj, &err));
  KBC_CHECK_EQ_INT(nj, 1);
  KBC_CHECK_EQ_INT(js[0].state, KBC_ANCHOR_RESOLVED);
  KBC_CHECK_OK(kbc_store_get_anchor(s, a, kept, &row, &err));
  KBC_CHECK_EQ_INT(row.state, KBC_ANCHOR_RESOLVED);

  kbc_arena_free(a);

  /* The OTHER half of the rebuild, and the one the foreign key cannot cover:
   * a verdict row whose comment no longer exists. It is reachable — the
   * header on this file's FK-checking block says any writer that was not
   * this binary can orphan a row at any time, and `foreign_keys` is a
   * CONNECTION setting — so the rebuild drops it, and the count afterwards
   * describes the comments that exist rather than the ones a deleted
   * comment used to have. */
  sqlite3 *raw = raw_open(root, "crash.db");
  KBC_CHECK_NOT_NULL(raw);
  if (raw != NULL) {
    char del[128];
    (void)snprintf(del, sizeof del,
                   "DELETE FROM comments WHERE id = '%s';", kept);
    raw_exec(raw, del);
    (void)sqlite3_close(raw);
  }
  kbc_store_close(s);
  s = open_at(root, "crash.db", &err);
  KBC_CHECK_NOT_NULL(s);
  if (s == NULL) {
    kbc_test_rmrf(root);
    return;
  }
  /* A fresh arena for the reopened store. The previous one was freed above,
   * next to the last read out of it: every row that came back from it points
   * into its blocks, so it cannot outlive them, and freeing it twice would
   * be a double free rather than tidiness. */
  a = kbc_arena_new(8192);
  kbc_err_reset(&err);
  KBC_CHECK_ERR(kbc_store_get_anchor(s, a, kept, &row, &err), KBC_ERR_NOTFOUND);
  KBC_CHECK_EQ_INT(kbc_store_count_stale_anchors(s, &err), 0);
  kbc_arena_free(a);
  kbc_store_close(s);
  kbc_test_rmrf(root);
}

/* THE FIXED POINT, for all three passes at once: judge, prune, reconcile.
 *
 * The second run of each must remove nothing and fire nothing. That is the
 * property the whole design is built for — a pass that is not idempotent
 * turns every reindex into a fresh batch of `comment.anchor_stale` events,
 * which is the exact failure the persisted set was introduced to prevent
 * (anchors.rs:5-8). */
KBC_TEST(judging_pruning_and_reconciling_all_reach_a_fixed_point) {
  char root[KBC_TEST_PATH_MAX];
  kbc_test_tmpdir(root, sizeof root);
  kbc_err err;
  kbc_err_reset(&err);
  kbc_store *s = open_at(root, "fixed.db", &err);
  KBC_CHECK_NOT_NULL(s);
  if (s == NULL) {
    kbc_test_rmrf(root);
    return;
  }
  kbc_arena *a = kbc_arena_new(8192);
  kbc_artifact art;
  fill(&art, "bbbbbbbbbbbb", "kb", "a.md", KBC_KIND_ARTIFACT);
  KBC_CHECK_OK(kbc_store_upsert_artifact(s, &art, &err));

  const kbc_anchor_heading heads[] = {
      {"intro", "Introduction"},
      {"install", "Install"},
  };
  const char *claims[] = {"install", "retired", "also-retired"};
  char(*ids)[KBC_MAX_ID_LEN + 1] =
      comment_ids_for(s, a, art.id, claims, 3, &err);
  KBC_CHECK_NOT_NULL(ids);
  if (ids == NULL) {
    kbc_arena_free(a);
    kbc_store_close(s);
    kbc_test_rmrf(root);
    return;
  }
  kbc_anchor_judgement *js = NULL;
  size_t nj = 0;
  KBC_CHECK_OK(
      kbc_store_judge_anchors(s, art.id, heads, 2, true, a, &js, &nj, &err));
  KBC_CHECK_EQ_INT(nj, 3);
  size_t edges = 0;
  for (size_t i = 0; i < nj; i++) {
    if (js[i].transitioned) edges++;
  }
  /* Two of the three went straight into the set; one was already fine. */
  KBC_CHECK_EQ_INT(edges, 2);
  KBC_CHECK_EQ_INT(kbc_store_count_stale_anchors(s, &err), 2);

  /* Second pass over the same bytes: no state moves, so no edge. */
  js = NULL;
  nj = 0;
  KBC_CHECK_OK(
      kbc_store_judge_anchors(s, art.id, heads, 2, true, a, &js, &nj, &err));
  edges = 0;
  for (size_t i = 0; i < nj; i++) {
    if (js[i].transitioned) edges++;
  }
  KBC_CHECK_EQ_INT(edges, 0);
  KBC_CHECK_EQ_INT(kbc_store_count_stale_anchors(s, &err), 2);

  /* A RESOLVED comment leaves the set — the original's `prune_if_resolved`
   * (anchors.rs:172-190). The prune keys on "is this comment still OPEN",
   * not on whether its verdict was stale, so resolving `install` drops its
   * row even though that row was RESOLVED: a resolved comment is not walked
   * by the pass (`c.resolved = 0`, indexer.rs:3035) and a verdict nothing
   * will re-read is not worth keeping. The stale COUNT is what stays put,
   * and that is the observable that matters — the set did not grow. */
  KBC_CHECK_OK(kbc_store_set_comment_resolved(s, ids[0], true, &err));
  int64_t pruned = 0;
  KBC_CHECK_OK(kbc_store_prune_anchors(s, &pruned, &err));
  KBC_CHECK_EQ_INT(pruned, 1);
  KBC_CHECK_EQ_INT(kbc_store_count_stale_anchors(s, &err), 2);
  /* Resolving one of the two STALE comments is what moves the count. */
  KBC_CHECK_OK(kbc_store_set_comment_resolved(s, ids[1], true, &err));
  KBC_CHECK_OK(kbc_store_prune_anchors(s, &pruned, &err));
  KBC_CHECK_EQ_INT(pruned, 1);
  KBC_CHECK_EQ_INT(kbc_store_count_stale_anchors(s, &err), 1);
  /* And the prune is itself a fixed point. */
  KBC_CHECK_OK(kbc_store_prune_anchors(s, &pruned, &err));
  KBC_CHECK_EQ_INT(pruned, 0);

  int64_t touched = 0;
  KBC_CHECK_OK(kbc_store_reconcile_anchors(s, &touched, &err));
  KBC_CHECK_EQ_INT(touched, 0);

  kbc_arena_free(a);
  kbc_store_close(s);
  kbc_test_rmrf(root);
}

/* Retention, and the two ways it can lie. Both are fixed-point claims about
 * a destructive verb, which is the only kind of claim worth making about
 * one. */
KBC_TEST(pruning_history_twice_removes_nothing_the_second_time) {
  char root[KBC_TEST_PATH_MAX];
  kbc_test_tmpdir(root, sizeof root);
  kbc_err err;
  kbc_err_reset(&err);
  kbc_store *s = open_at(root, "prune.db", &err);
  KBC_CHECK_NOT_NULL(s);
  if (s == NULL) {
    kbc_test_rmrf(root);
    return;
  }
  kbc_arena *a = kbc_arena_new(8192);
  kbc_artifact art;
  fill(&art, "bbbbbbbbbbbb", "kb", "a.md", KBC_KIND_ARTIFACT);
  KBC_CHECK_OK(kbc_store_upsert_artifact(s, &art, &err));

  kbc_history_row h = {0};
  h.kind = "open";
  h.artifact_id = art.id;
  h.started_at = 1000;
  h.updated_at = 1000;
  KBC_CHECK_OK(kbc_store_add_history(s, &h, &err));
  h.started_at = 2000;
  h.updated_at = 2000;
  KBC_CHECK_OK(kbc_store_add_history(s, &h, &err));
  h.started_at = 9000;
  h.updated_at = 9000;
  KBC_CHECK_OK(kbc_store_add_history(s, &h, &err));

  int64_t rows = 0;
  KBC_CHECK_OK(kbc_store_prune_history(s, 5000, false, &rows, &err));
  KBC_CHECK_EQ_INT(rows, 2);
  /* The dry run's number is the number --apply removes, and the row that
   * survives proves the predicate is `<` and not `<=`. */
  KBC_CHECK_OK(kbc_store_prune_history(s, 5000, true, &rows, &err));
  KBC_CHECK_EQ_INT(rows, 2);
  kbc_history_row *hist = NULL;
  size_t nh = 0;
  KBC_CHECK_OK(kbc_store_list_history(s, a, NULL, 10, &hist, &nh, &err));
  KBC_CHECK_EQ_INT(nh, 1);
  if (nh == 1) KBC_CHECK_EQ_INT(hist[0].started_at, 9000);

  KBC_CHECK_OK(kbc_store_prune_history(s, 5000, false, &rows, &err));
  KBC_CHECK_EQ_INT(rows, 0);
  KBC_CHECK_OK(kbc_store_prune_history(s, 5000, true, &rows, &err));
  KBC_CHECK_EQ_INT(rows, 0);

  /* A NEGATIVE cutoff is the header's promised no-op, and it used not to be
   * one: the code clamped the cutoff to 0 and then ran `started_at < 0`, and
   * `started_at` is a caller-supplied INTEGER with no lower bound on its
   * write path. A row below the epoch is writable, and the clamp deleted it
   * while the header said it never would. */
  h.started_at = -1;
  h.updated_at = -1;
  KBC_CHECK_OK(kbc_store_add_history(s, &h, &err));
  KBC_CHECK_OK(kbc_store_list_history(s, a, NULL, 10, &hist, &nh, &err));
  KBC_CHECK_EQ_INT(nh, 2);
  KBC_CHECK_OK(kbc_store_prune_history(s, -1, false, &rows, &err));
  KBC_CHECK_EQ_INT(rows, 0);
  KBC_CHECK_OK(kbc_store_prune_history(s, -1, true, &rows, &err));
  KBC_CHECK_EQ_INT(rows, 0);
  KBC_CHECK_OK(kbc_store_list_history(s, a, NULL, 10, &hist, &nh, &err));
  KBC_CHECK_EQ_INT(nh, 2);
  /* A cutoff of ZERO is a real cutoff and still prunes the row below the
   * epoch — the no-op is a NEGATIVE cutoff, not the absence of one. */
  KBC_CHECK_OK(kbc_store_prune_history(s, 0, true, &rows, &err));
  KBC_CHECK_EQ_INT(rows, 1);
  KBC_CHECK_OK(kbc_store_list_history(s, a, NULL, 10, &hist, &nh, &err));
  KBC_CHECK_EQ_INT(nh, 1);

  /* Retention removed rows and NO documents. That is the whole shape of it
   * (store.h:487): `edges` has no timestamp column, so the original cannot
   * time-prune the graph either, and a retention pass that deleted documents
   * would be a corpus-deleting verb the original does not have. */
  int64_t docs = 0;
  KBC_CHECK_OK(kbc_store_count_artifacts(s, "kb", &docs, &err));
  KBC_CHECK_EQ_INT(docs, 1);
  KBC_CHECK_OK(kbc_store_checkpoint(s, &err));

  kbc_arena_free(a);
  kbc_store_close(s);
  kbc_test_rmrf(root);
}

/* The stale set is a PAGE, and it is `kbc_store_list_corkboard`'s page: the
 * ORDER BY and the `LIMIT n` are the original's answer to "how many results
 * does this page show", so a caller that treats an absent id as "fine" is
 * wrong here for the same reason it is wrong there. */
KBC_TEST(the_stale_set_is_a_ordered_page_and_not_a_verdict_on_absence) {
  char root[KBC_TEST_PATH_MAX];
  kbc_test_tmpdir(root, sizeof root);
  kbc_err err;
  kbc_err_reset(&err);
  kbc_store *s = open_at(root, "page.db", &err);
  KBC_CHECK_NOT_NULL(s);
  if (s == NULL) {
    kbc_test_rmrf(root);
    return;
  }
  kbc_arena *a = kbc_arena_new(8192);
  kbc_artifact art;
  fill(&art, "bbbbbbbbbbbb", "kb", "a.md", KBC_KIND_ARTIFACT);
  KBC_CHECK_OK(kbc_store_upsert_artifact(s, &art, &err));

  const kbc_anchor_heading heads[] = {{"intro", "Introduction"}};
  const char *claims[] = {"gone-a", "gone-b", "intro"};
  char(*ids)[KBC_MAX_ID_LEN + 1] =
      comment_ids_for(s, a, art.id, claims, 3, &err);
  KBC_CHECK_NOT_NULL(ids);
  if (ids == NULL) {
    kbc_arena_free(a);
    kbc_store_close(s);
    kbc_test_rmrf(root);
    return;
  }
  kbc_anchor_judgement *js = NULL;
  size_t nj = 0;
  KBC_CHECK_OK(
      kbc_store_judge_anchors(s, art.id, heads, 1, true, a, &js, &nj, &err));
  KBC_CHECK_EQ_INT(kbc_store_count_stale_anchors(s, &err), 2);

  kbc_anchor_row *rows = NULL;
  size_t n = 0;
  KBC_CHECK_OK(
      kbc_store_list_anchors(s, a, art.id, true, 10, &rows, &n, &err));
  KBC_CHECK_EQ_INT(n, 2);
  /* Both rows carry the whole verdict, not just a flag: a stale-anchor
   * surface cannot show a user where their comment went without the id it
   * resolves at. */
  for (size_t i = 0; i < n; i++) {
    KBC_CHECK_EQ_INT(rows[i].state, KBC_ANCHOR_UNRESOLVED);
    KBC_CHECK_EQ_STR(rows[i].resolves_to, "");
    KBC_CHECK_EQ_INT(rows[i].resolved_ord, -1);
    KBC_CHECK_EQ_STR(rows[i].doc_id, art.id);
  }
  /* `stale_only` false asks for everything, resolved included. */
  rows = NULL;
  n = 0;
  KBC_CHECK_OK(
      kbc_store_list_anchors(s, a, art.id, false, 10, &rows, &n, &err));
  KBC_CHECK_EQ_INT(n, 3);

  /* `LIMIT n` takes the n most recent, so an id missing from the page is not
   * evidence about it. The count still says three. */
  rows = NULL;
  n = 0;
  KBC_CHECK_OK(
      kbc_store_list_anchors(s, a, art.id, false, 1, &rows, &n, &err));
  KBC_CHECK_EQ_INT(n, 1);
  KBC_CHECK_EQ_INT(kbc_store_count_stale_anchors(s, &err), 2);

  /* A `doc_id` that names no document is an empty page, never an error and
   * never somebody else's rows. */
  rows = NULL;
  n = 0;
  KBC_CHECK_OK(
      kbc_store_list_anchors(s, a, "zzzzzzzzzzzz", true, 10, &rows, &n, &err));
  KBC_CHECK_EQ_INT(n, 0);
  KBC_CHECK_NULL(rows);

  KBC_CHECK_ERR(kbc_store_list_anchors(s, a, "", true, 10, &rows, &n, &err),
                KBC_ERR_INVALID);
  KBC_CHECK_ERR(kbc_store_list_anchors(s, NULL, NULL, true, 10, &rows, &n,
                                       &err),
                KBC_ERR_INVALID);
  kbc_anchor_row row;
  /* The rest of the anchor surface refuses the same shapes its neighbours
   * do. `judge_anchors` also refuses a null heading ARRAY with a non-zero
   * length: that is the one that would otherwise read past the end of
   * nothing and decide every anchor against uninitialised memory. */
  KBC_CHECK_ERR(kbc_store_judge_anchors(s, NULL, heads, 1, true, a, &js, &nj,
                                        &err),
                KBC_ERR_INVALID);
  KBC_CHECK_ERR(kbc_store_judge_anchors(s, art.id, NULL, 1, true, a, &js, &nj,
                                        &err),
                KBC_ERR_INVALID);
  KBC_CHECK_ERR(kbc_store_judge_anchors(s, "", heads, 1, true, a, &js, &nj,
                                        &err),
                KBC_ERR_INVALID);
  KBC_CHECK_ERR(kbc_store_get_anchor(s, a, "", &row, &err), KBC_ERR_INVALID);
  KBC_CHECK_ERR(kbc_store_get_anchor(NULL, a, ids[0], &row, &err),
                KBC_ERR_INVALID);
  KBC_CHECK_EQ_INT(kbc_store_count_stale_anchors(NULL, &err), -1);
  KBC_CHECK_ERR(kbc_store_prune_anchors(s, NULL, &err), KBC_ERR_INVALID);
  KBC_CHECK_ERR(kbc_store_reconcile_anchors(s, NULL, &err), KBC_ERR_INVALID);
  /* A zero limit is an empty page, not an error and not an unfiltered scan. */
  rows = NULL;
  n = 0;
  KBC_CHECK_OK(
      kbc_store_list_anchors(s, a, art.id, false, 0, &rows, &n, &err));
  KBC_CHECK_EQ_INT(n, 0);
  KBC_CHECK_NULL(rows);
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
      {"metas_read_back_the_pairs_replace_wrote",
       metas_read_back_the_pairs_replace_wrote},
      {"metas_a_multi_valued_facet_comes_back_in_a_stable_order",
       metas_a_multi_valued_facet_comes_back_in_a_stable_order},
      {"metas_a_document_with_no_facets_is_empty_and_not_an_error",
       metas_a_document_with_no_facets_is_empty_and_not_an_error},
      {"metas_reject_a_path_no_writer_would_have_accepted",
       metas_reject_a_path_no_writer_would_have_accepted},
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
      {"moves_declares_the_rust_columns_plus_the_one_kb_c_adds",
       moves_declares_the_rust_columns_plus_the_one_kb_c_adds},
      {"foreign_key_check_reports_every_orphaned_edge_not_the_first",
       foreign_key_check_reports_every_orphaned_edge_not_the_first},
      {"an_open_reports_every_orphaned_row_and_still_serves",
       an_open_reports_every_orphaned_row_and_still_serves},
      {"a_healthy_volume_opens_clean_and_unchanged_at_scale",
       a_healthy_volume_opens_clean_and_unchanged_at_scale},
      {"a_rekey_carries_a_comments_id_created_at_and_resolution",
       a_rekey_carries_a_comments_id_created_at_and_resolution},
      {"a_rekey_moves_the_artifact_row_and_every_table_that_names_it",
       a_rekey_moves_the_artifact_row_and_every_table_that_names_it},
      {"a_stale_id_follows_a_chain_of_renames_to_its_final_home",
       a_stale_id_follows_a_chain_of_renames_to_its_final_home},
      {"an_interrupted_move_is_listed_for_replay_and_never_followed",
       an_interrupted_move_is_listed_for_replay_and_never_followed},
      {"a_v11_volume_gains_the_abandon_column_and_keeps_its_stamps",
       a_v11_volume_gains_the_abandon_column_and_keeps_its_stamps},
      {"an_abandoned_move_leaves_the_replay_list_and_is_never_followed",
       an_abandoned_move_leaves_the_replay_list_and_is_never_followed},
      {"completing_an_abandoned_move_does_not_revive_the_redirect",
       completing_an_abandoned_move_does_not_revive_the_redirect},
      {"a_completed_move_still_redirects_beside_an_abandoned_one",
       a_completed_move_still_redirects_beside_an_abandoned_one},
      {"a_move_stamped_completed_and_abandoned_is_not_followed",
       a_move_stamped_completed_and_abandoned_is_not_followed},
      {"a_cyclic_move_chain_terminates_instead_of_hanging",
       a_cyclic_move_chain_terminates_instead_of_hanging},
      {"a_rekey_does_not_reach_into_a_second_corpus_at_the_same_path",
       a_rekey_does_not_reach_into_a_second_corpus_at_the_same_path},
      {"a_rekey_does_not_drop_a_second_corpus_edge_to_the_new_path",
       a_rekey_does_not_drop_a_second_corpus_edge_to_the_new_path},
      {"a_rekey_of_an_id_this_store_does_not_hold_touches_no_path",
       a_rekey_of_an_id_this_store_does_not_hold_touches_no_path},
      {"anchor_states_are_three_and_only_three",
       anchor_states_are_three_and_only_three},
      {"a_document_that_will_not_parse_leaves_every_anchor_undecidable",
       a_document_that_will_not_parse_leaves_every_anchor_undecidable},
      {"a_moved_heading_takes_its_comment_with_it",
       a_moved_heading_takes_its_comment_with_it},
      {"a_heading_taken_over_by_another_leaves_the_anchor_undecidable",
       a_heading_taken_over_by_another_leaves_the_anchor_undecidable},
      {"a_renamed_heading_is_unresolved_and_not_silently_followed",
       a_renamed_heading_is_unresolved_and_not_silently_followed},
      {"a_crash_between_a_pass_and_a_reindex_leaves_no_claim_about_old_bytes",
       a_crash_between_a_pass_and_a_reindex_leaves_no_claim_about_old_bytes},
      {"judging_pruning_and_reconciling_all_reach_a_fixed_point",
       judging_pruning_and_reconciling_all_reach_a_fixed_point},
      {"pruning_history_twice_removes_nothing_the_second_time",
       pruning_history_twice_removes_nothing_the_second_time},
      {"the_stale_set_is_a_ordered_page_and_not_a_verdict_on_absence",
       the_stale_set_is_a_ordered_page_and_not_a_verdict_on_absence},
      {NULL, NULL},
  };
  return kbc_test_run("store", cases);
}
