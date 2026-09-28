/* test_links.c — src/links.c: the four-tier wikilink resolution ladder.
 *
 * Every test below is either a port of a test in the original links.rs test
 * module (same corpus, same targets, same expected resolution, name kept so
 * the two files can be diffed by eye) or a case the original leaves to its
 * caller: the ladder is only as trustworthy as its edges, and the edges —
 * first-wins, the dotted-directory extension, the extension-less document —
 * are exactly where a hashmap build ported to C goes wrong. */

#include "kbc_test.h"
#include "kbc/links.h"

#define DOC(id, rel, title) ((kbc_resolve_doc){(id), (rel), (title)})

static kbc_resolve_index *index_of(const kbc_resolve_doc *docs, size_t n) {
  kbc_err err;
  kbc_err_reset(&err);
  kbc_resolve_index *ix = kbc_resolve_index_new(docs, n, &err);
  KBC_CHECK_MSG(ix != NULL, "index build failed: %s", err.msg);
  return ix;
}

/* Resolves `target` and asserts the WHOLE outcome: the kind, and the match
 * list in the order the candidates were given — the order is part of the
 * contract (kbc_resolution::ids), not an accident of the table.
 *
 * `want` is the expected id list, comma separated, or NULL for no matches.
 * out.ids is KBC_OWN, so it is freed here; the arena is the scratch and dies
 * with the call. */
static void expect(const kbc_resolve_index *ix, const char *target,
                   kbc_resolve_kind kind, const char *want) {
  kbc_arena *a = kbc_arena_new(256);
  KBC_CHECK_NOT_NULL(a);
  kbc_resolution out;
  kbc_err err;
  kbc_err_reset(&err);
  KBC_CHECK_MSG(kbc_resolve_index_resolve(ix, a, target, &out, &err) == KBC_OK,
                "resolve(\"%.40s\"): %s", target, err.msg);
  KBC_CHECK_MSG(out.kind == kind, "resolve(\"%.40s\"): kind %d, want %d", target,
                (int)out.kind, (int)kind);

  size_t n_want = 0;
  for (const char *p = want; p != NULL && *p != '\0'; p++) {
    if (p == want || *p == ',') {
      n_want++;
    }
  }
  KBC_CHECK_MSG(out.ids.len == n_want,
                "resolve(\"%.40s\"): %zu matches, want %zu (%.60s)", target,
                out.ids.len, n_want, want != NULL ? want : "");
  const char *p = want;
  for (size_t i = 0; i < n_want && i < out.ids.len; i++) {
    const char *comma = strchr(p, ',');
    size_t len = comma != NULL ? (size_t)(comma - p) : strlen(p);
    KBC_CHECK_MSG(strlen(out.ids.items[i]) == len &&
                      memcmp(out.ids.items[i], p, len) == 0,
                  "resolve(\"%.40s\") match %zu: got \"%.40s\", want \"%.40s\"",
                  target, i, out.ids.items[i], p);
    p = comma != NULL ? comma + 1 : p + len;
  }
  kbc_strlist_free(&out.ids);
  kbc_arena_free(a);
}

/* ======================= ports of the Rust ladder tests ================== */

KBC_TEST(resolve_by_id) {
  kbc_resolve_doc docs[1] = {DOC("ab12cd34ef56", "ops/x.md", "X")};
  kbc_resolve_index *ix = index_of(docs, 1);
  expect(ix, "ab12cd34ef56", KBC_RESOLVE_ONE, "ab12cd34ef56");
  kbc_resolve_index_free(ix);
}

KBC_TEST(resolve_by_path_with_and_without_ext) {
  kbc_resolve_doc docs[1] = {DOC("i1", "ops/deploy.md", "Deploy")};
  kbc_resolve_index *ix = index_of(docs, 1);
  expect(ix, "ops/deploy.md", KBC_RESOLVE_ONE, "i1");
  expect(ix, "ops/deploy", KBC_RESOLVE_ONE, "i1");
  expect(ix, "/ops/deploy.md", KBC_RESOLVE_ONE, "i1");
  kbc_resolve_index_free(ix);
}

KBC_TEST(resolve_by_title_case_insensitive) {
  kbc_resolve_doc docs[1] = {DOC("i1", "n/a.md", "Deploy Checklist")};
  kbc_resolve_index *ix = index_of(docs, 1);
  expect(ix, "deploy checklist", KBC_RESOLVE_ONE, "i1");
  expect(ix, "DEPLOY CHECKLIST", KBC_RESOLVE_ONE, "i1");
  kbc_resolve_index_free(ix);
}

KBC_TEST(resolve_title_ambiguous) {
  kbc_resolve_doc docs[2] = {DOC("i1", "a/x.md", "Notes"), DOC("i2", "b/y.md",
                                                               "notes")};
  kbc_resolve_index *ix = index_of(docs, 2);
  expect(ix, "Notes", KBC_RESOLVE_AMBIGUOUS, "i1,i2");
  expect(ix, "notes", KBC_RESOLVE_AMBIGUOUS, "i1,i2");
  kbc_resolve_index_free(ix);
}

KBC_TEST(resolve_by_basename) {
  kbc_resolve_doc docs[1] = {DOC("i1", "deep/ops/deploy.md", "Some Other "
                                                         "Title")};
  kbc_resolve_index *ix = index_of(docs, 1);
  expect(ix, "deploy", KBC_RESOLVE_ONE, "i1");
  expect(ix, "deploy.md", KBC_RESOLVE_ONE, "i1");
  kbc_resolve_index_free(ix);
}

KBC_TEST(resolve_bare_basename_ambiguous_across_folders) {
  /* Two files share the stem `deploy` in different folders. A bare `[[deploy]]`
   * must be AMBIGUOUS (tier 4 collects), NOT a nondeterministic first-hit,
   * while a folder-qualified target stays unique via tier 2. */
  kbc_resolve_doc docs[2] = {DOC("ops_id", "ops/deploy.md", "Ops deploy"),
                             DOC("infra_id", "infra/deploy.md", "Infra "
                                                              "deploy")};
  kbc_resolve_index *ix = index_of(docs, 2);
  expect(ix, "deploy", KBC_RESOLVE_AMBIGUOUS, "ops_id,infra_id");
  expect(ix, "ops/deploy", KBC_RESOLVE_ONE, "ops_id");
  expect(ix, "infra/deploy.md", KBC_RESOLVE_ONE, "infra_id");
  kbc_resolve_index_free(ix);
}

KBC_TEST(strip_ext_leaves_dotted_directory_intact) {
  /* `a.b/c` (dotted dir, extension-less file) must NOT have its dir-dot
   * stripped, so target `a` cannot false-match the path tier. */
  kbc_resolve_doc docs[1] = {DOC("i1", "a.b/c", "Title")};
  kbc_resolve_index *ix = index_of(docs, 1);
  expect(ix, "a", KBC_RESOLVE_NONE, NULL);
  expect(ix, "b/c", KBC_RESOLVE_NONE, NULL);
  expect(ix, "a.b/c", KBC_RESOLVE_ONE, "i1");
  /* Tier 4 still sees the bare filename, which is the whole path intact. */
  expect(ix, "c", KBC_RESOLVE_ONE, "i1");
  kbc_resolve_index_free(ix);
}

KBC_TEST(resolve_none_for_dangling) {
  kbc_resolve_doc docs[1] = {DOC("i1", "a.md", "A")};
  kbc_resolve_index *ix = index_of(docs, 1);
  expect(ix, "nonexistent", KBC_RESOLVE_NONE, NULL);
  expect(ix, "", KBC_RESOLVE_NONE, NULL);
  kbc_resolve_index_free(ix);
}

KBC_TEST(normalize_strips_anchor_and_slash) {
  /* The normaliser is static, so it is pinned through the ladder: every step
   * of links.rs:182-187 is observable as a target that resolves or not. */
  kbc_resolve_doc docs[1] = {DOC("i1", "ops/x.md", "Title")};
  kbc_resolve_index *ix = index_of(docs, 1);
  expect(ix, "/ops/x.md#section", KBC_RESOLVE_ONE, "i1");
  expect(ix, "  Title #frag ", KBC_RESOLVE_ONE, "i1");
  expect(ix, "Title", KBC_RESOLVE_ONE, "i1");
  expect(ix, "   /ops/x.md   ", KBC_RESOLVE_ONE, "i1");
  /* The fragment split takes the FIRST '#', so a second one is part of the
   * fragment, not of the target. */
  expect(ix, "ops/x.md#a#b", KBC_RESOLVE_ONE, "i1");
  expect(ix, "ops\\x.md", KBC_RESOLVE_ONE, "i1");
  kbc_resolve_index_free(ix);
}

KBC_TEST(index_reuse_over_a_stable_corpus) {
  /* The edge-record hook builds ONE index per note and resolves every target
   * through it; each answer must be the one-shot form's. The expected
   * outcomes below are what links.rs:563-574 asserts on both sides. */
  kbc_resolve_doc docs[5] = {DOC("ab12cd34ef56", "ops/x.md", "X"),
                             DOC("i1", "ops/deploy.md", "Deploy checklist"),
                             DOC("i2", "infra/deploy.md", "Infra deploy"),
                             DOC("i3", "a/notes.md", "Notes"),
                             DOC("i4", "b/notes.md", "notes")};
  kbc_resolve_index *ix = index_of(docs, 5);
  expect(ix, "ab12cd34ef56", KBC_RESOLVE_ONE, "ab12cd34ef56");
  expect(ix, "ops/deploy.md", KBC_RESOLVE_ONE, "i1");
  expect(ix, "ops/deploy", KBC_RESOLVE_ONE, "i1");
  expect(ix, "deploy checklist", KBC_RESOLVE_ONE, "i1");
  expect(ix, "deploy", KBC_RESOLVE_AMBIGUOUS, "i1,i2");
  expect(ix, "Notes", KBC_RESOLVE_AMBIGUOUS, "i3,i4");
  expect(ix, "notes.md", KBC_RESOLVE_AMBIGUOUS, "i3,i4");
  expect(ix, "dangling", KBC_RESOLVE_NONE, NULL);
  expect(ix, "", KBC_RESOLVE_NONE, NULL);
  /* Reuse must not be stateful: the same target twice is the same answer. */
  expect(ix, "deploy", KBC_RESOLVE_AMBIGUOUS, "i1,i2");
  expect(ix, "deploy", KBC_RESOLVE_AMBIGUOUS, "i1,i2");
  kbc_resolve_index_free(ix);
}

KBC_TEST(path_tier_ext_elided_collision_is_first_doc_wins) {
  /* `ops/deploy.md` elides to `ops/deploy`, colliding with the real
   * extension-less file. Tier 2 is first-hit in doc order (links.rs:268-269
   * or_insert), so swapping the caller's order swaps the answer. */
  kbc_resolve_doc a = DOC("md_doc", "ops/deploy.md", "A");
  kbc_resolve_doc b = DOC("bare_doc", "ops/deploy", "B");
  kbc_resolve_doc md_first[2] = {a, b};
  kbc_resolve_doc bare_first[2] = {b, a};
  kbc_resolve_index *ix = index_of(md_first, 2);
  expect(ix, "ops/deploy", KBC_RESOLVE_ONE, "md_doc");
  kbc_resolve_index_free(ix);
  ix = index_of(bare_first, 2);
  expect(ix, "ops/deploy", KBC_RESOLVE_ONE, "bare_doc");
  kbc_resolve_index_free(ix);
}

KBC_TEST(path_tier_beats_title_tier) {
  /* A target that is both a valid path and someone else's title resolves to
   * the path (more specific), not the title. */
  kbc_resolve_doc docs[2] = {DOC("path_doc", "report.md", "Z"),
                             DOC("title_doc", "other.md", "report.md")};
  kbc_resolve_index *ix = index_of(docs, 2);
  expect(ix, "report.md", KBC_RESOLVE_ONE, "path_doc");
  kbc_resolve_index_free(ix);
}

/* ================== cases the original does not cover =================== */

KBC_TEST(empty_corpus_resolves_nothing) {
  kbc_resolve_doc unused[1] = {DOC("i1", "a.md", "A")};
  kbc_resolve_index *ix = index_of(NULL, 0);
  KBC_CHECK_NOT_NULL(ix);
  /* Every tier, including the id passthrough, on an index with no rows. */
  expect(ix, "ab12cd34ef56", KBC_RESOLVE_NONE, NULL);
  expect(ix, "a.md", KBC_RESOLVE_NONE, NULL);
  expect(ix, "a", KBC_RESOLVE_NONE, NULL);
  expect(ix, "A", KBC_RESOLVE_NONE, NULL);
  expect(ix, "", KBC_RESOLVE_NONE, NULL);
  kbc_resolve_index_free(ix);
  /* An empty array must build the same index as a NULL one. */
  ix = index_of(unused, 0);
  expect(ix, "a.md", KBC_RESOLVE_NONE, NULL);
  kbc_resolve_index_free(ix);
}

KBC_TEST(targets_that_normalise_away_are_dangling) {
  kbc_resolve_doc docs[1] = {DOC("i1", "ops/x.md", "X")};
  kbc_resolve_index *ix = index_of(docs, 1);
  expect(ix, "#", KBC_RESOLVE_NONE, NULL);
  expect(ix, "   ", KBC_RESOLVE_NONE, NULL);
  expect(ix, "///", KBC_RESOLVE_NONE, NULL);
  expect(ix, "\t\n ", KBC_RESOLVE_NONE, NULL);
  expect(ix, "  #  ", KBC_RESOLVE_NONE, NULL);

  /* The same targets on a heap buffer with NO slack after the NUL: a
   * normaliser that reads one byte past the end is a heap overflow, which
   * the ASan lane turns into a failure instead of a silent pass. */
  const char *tight[] = {"#", "   ", "///", "  #  "};
  for (size_t i = 0; i < sizeof(tight) / sizeof(tight[0]); i++) {
    size_t n = strlen(tight[i]);
    char *buf = malloc(n + 1u);
    KBC_CHECK_NOT_NULL(buf);
    memcpy(buf, tight[i], n + 1u);
    expect(ix, buf, KBC_RESOLVE_NONE, NULL);
    free(buf);
  }
  kbc_resolve_index_free(ix);
}

KBC_TEST(target_longer_than_any_key_is_dangling) {
  kbc_resolve_doc docs[2] = {DOC("i1", "ops/deploy.md", "Deploy"),
                             DOC("i2", "a.md", "A")};
  kbc_resolve_index *ix = index_of(docs, 2);

  /* Longer than every key in every table, and far longer than KBC_MAX_PATH_LEN
   * would allow as a stored path: a target is untrusted text, not a path the
   * caller has already validated. */
  size_t n = 4u * KBC_MAX_PATH_LEN;
  char *long_target = malloc(n + 1u);
  KBC_CHECK_NOT_NULL(long_target);
  memset(long_target, 'a', n);
  long_target[n] = '\0';
  expect(ix, long_target, KBC_RESOLVE_NONE, NULL);

  /* The same run but ending in a real key: still nothing, because the ladder
   * compares the WHOLE normalised target, never a suffix of it. */
  memcpy(long_target + n - 6u, "eploy", 5u);
  expect(ix, long_target, KBC_RESOLVE_NONE, NULL);
  free(long_target);
  kbc_resolve_index_free(ix);
}

KBC_TEST(degenerate_documents_do_not_poison_the_ladder) {
  /* Empty title and empty rel_path are legal: they are what a directory entry
   * with no name, or a doc with no front matter, looks like. */
  kbc_resolve_doc docs[3] = {DOC("d1", "", ""), DOC("d2", "a.md", ""),
                             DOC("d3", "", "Real")};
  kbc_resolve_index *ix = index_of(docs, 3);
  expect(ix, "a.md", KBC_RESOLVE_ONE, "d2");
  expect(ix, "a", KBC_RESOLVE_ONE, "d2");
  expect(ix, "real", KBC_RESOLVE_ONE, "d3");
  expect(ix, "x", KBC_RESOLVE_NONE, NULL);
  /* The empty title and the empty path ARE keys in tiers 2-4, and must stay
   * unreachable: a target that normalises to "" is answered before the
   * ladder starts (links.rs:292-294). */
  expect(ix, "", KBC_RESOLVE_NONE, NULL);
  expect(ix, "   ", KBC_RESOLVE_NONE, NULL);
  expect(ix, "/", KBC_RESOLVE_NONE, NULL);
  kbc_resolve_index_free(ix);
}

KBC_TEST(first_wins_is_the_callers_order_contract) {
  /* First-wins covers two collisions: the ext-elided key an earlier document
   * claims, and two documents with the SAME exact path. Neither may be
   * overwritten by a later one, or the id a link resolves to flips on the
   * next reindex. */
  kbc_resolve_doc a = DOC("md_doc", "ops/deploy.md", "A");
  kbc_resolve_doc b = DOC("bare_doc", "ops/deploy", "B");
  kbc_resolve_doc c = DOC("dup_doc", "ops/deploy.md", "C");
  kbc_resolve_doc first[3] = {a, b, c};
  kbc_resolve_doc last[3] = {c, b, a};

  kbc_resolve_index *ix = index_of(first, 3);
  expect(ix, "ops/deploy", KBC_RESOLVE_ONE, "md_doc");
  expect(ix, "ops/deploy.md", KBC_RESOLVE_ONE, "md_doc");
  /* The loser's own id is still reachable by its title: first-wins silences
   * the PATH tier, not the document. */
  expect(ix, "b", KBC_RESOLVE_ONE, "bare_doc");
  expect(ix, "c", KBC_RESOLVE_ONE, "dup_doc");
  kbc_resolve_index_free(ix);

  ix = index_of(last, 3);
  expect(ix, "ops/deploy.md", KBC_RESOLVE_ONE, "dup_doc");
  expect(ix, "ops/deploy", KBC_RESOLVE_ONE, "dup_doc");
  kbc_resolve_index_free(ix);
}

KBC_TEST(extensionless_document_is_one_by_basename) {
  /* `ops/deploy` has no extension, so its stem IS its basename. Inserting it
   * into tier 4 twice would make every extension-less target AMBIGUOUS with
   * ITSELF — a two-element list holding the same id twice, which is not what
   * the original's single push produces. */
  kbc_resolve_doc docs[1] = {DOC("bare_id", "ops/deploy", "Bare")};
  kbc_resolve_index *ix = index_of(docs, 1);
  expect(ix, "ops/deploy", KBC_RESOLVE_ONE, "bare_id");
  expect(ix, "deploy", KBC_RESOLVE_ONE, "bare_id");
  kbc_resolve_index_free(ix);

  /* With a real sibling the same target IS ambiguous — in document order, and
   * once per document. */
  kbc_resolve_doc two[2] = {DOC("md_id", "ops/deploy.md", "MD"),
                            DOC("bare_id", "ops/deploy", "Bare")};
  ix = index_of(two, 2);
  expect(ix, "deploy", KBC_RESOLVE_AMBIGUOUS, "md_id,bare_id");
  /* First-wins again, now inside tier 4's corpus: the .md document claims
   * "ops/deploy" with its ext-elided key BEFORE the extension-less document
   * claims it as its real path, so the bare document is reachable by
   * basename but not by its own path. That is the caller's ordering, not a
   * bug — and it is why the order of `docs` is a contract. */
  expect(ix, "ops/deploy", KBC_RESOLVE_ONE, "md_id");
  expect(ix, "ops/deploy.md", KBC_RESOLVE_ONE, "md_id");
  kbc_resolve_index_free(ix);
}

KBC_TEST(id_tier_rejects_uppercase_and_wrong_length) {
  kbc_resolve_doc docs[1] = {DOC("ab12cd34ef56", "ops/x.md", "X")};
  kbc_resolve_index *ix = index_of(docs, 1);
  expect(ix, "ab12cd34ef56", KBC_RESOLVE_ONE, "ab12cd34ef56");
  expect(ix, "  ab12cd34ef56  ", KBC_RESOLVE_ONE, "ab12cd34ef56");
  /* links.rs:199-203 — hexdigit AND NOT uppercase, so an uppercase id is
   * not an id and falls through to a tier that has no key for it. */
  expect(ix, "AB12CD34EF56", KBC_RESOLVE_NONE, NULL);
  expect(ix, "ab12cd34efg6", KBC_RESOLVE_NONE, NULL);
  expect(ix, "ab12cd34ef5", KBC_RESOLVE_NONE, NULL);
  expect(ix, "ab12cd34ef567", KBC_RESOLVE_NONE, NULL);
  expect(ix, "ab12cd34ef5 ", KBC_RESOLVE_NONE, NULL);
  kbc_resolve_index_free(ix);

  /* The guard is on the TARGET, but it has to be tested with documents whose
   * OWN ids are off-shape: a length rule that accepted 11 or 13 bytes would
   * hand back an id this indexer could never have minted. */
  kbc_resolve_doc odd[3] = {DOC("ab12cd34ef5", "ops/short.md", "Short"),
                            DOC("ab12cd34ef567", "ops/odd.md", "Odd"),
                            DOC("ab12cd34efg6", "ops/g.md", "G")};
  ix = index_of(odd, 3);
  expect(ix, "ab12cd34ef5", KBC_RESOLVE_NONE, NULL);
  expect(ix, "ab12cd34ef567", KBC_RESOLVE_NONE, NULL);
  expect(ix, "ab12cd34efg6", KBC_RESOLVE_NONE, NULL);
  /* All three stay reachable by path and title, as they must. */
  expect(ix, "ops/odd", KBC_RESOLVE_ONE, "ab12cd34ef567");
  expect(ix, "odd", KBC_RESOLVE_ONE, "ab12cd34ef567");
  kbc_resolve_index_free(ix);
}

KBC_TEST(backslash_fold_happens_after_the_slash_strip) {
  /* Order inside the normaliser is load-bearing: a run of backslashes is
   * NOT a run of slashes at the time the leading slashes are stripped, so it
   * folds to a DOUBLED slash and matches nothing. */
  kbc_resolve_doc docs[1] = {DOC("i1", "ops/deploy.md", "Deploy")};
  kbc_resolve_index *ix = index_of(docs, 1);
  expect(ix, "ops\\deploy.md", KBC_RESOLVE_ONE, "i1");
  expect(ix, "\\\\ops\\deploy.md", KBC_RESOLVE_NONE, NULL);
  /* A stored path is source-relative: an absolute-looking target has had
   * its leading slash stripped and must not match the relative key. */
  expect(ix, "/\\ops/deploy.md", KBC_RESOLVE_NONE, NULL);
  kbc_resolve_index_free(ix);
}

KBC_TEST(title_tier_beats_basename_tier) {
  /* Tiers 3 and 4 share one lookup key, so a title hit must stop the walk:
   * here tier 4 would answer AMBIGUOUS(md1,md2) and tier 3 answers ONE. */
  kbc_resolve_doc docs[3] = {DOC("title_doc", "other.md", "deploy"),
                             DOC("md1", "ops/deploy.md", "Ops"),
                             DOC("md2", "infra/deploy.md", "Infra")};
  kbc_resolve_index *ix = index_of(docs, 3);
  expect(ix, "deploy", KBC_RESOLVE_ONE, "title_doc");
  kbc_resolve_index_free(ix);
}

KBC_TEST(index_outlives_the_docs_array) {
  /* links.h: `docs` is BORROWED for the build call only — every key is
   * copied, so the index does not read the caller's array again. The
   * strings are freed before the first resolve, so an implementation that
   * borrowed them is a use-after-free the ASan lane catches. */
  char *id0 = strdup("aaaaaaaaaaaa");
  char *rel0 = strdup("ops/x.md");
  char *title0 = strdup("X");
  char *id1 = strdup("bbbbbbbbbbbb");
  char *rel1 = strdup("ops/deploy.md");
  char *title1 = strdup("Deploy");
  KBC_CHECK_NOT_NULL(id0);
  KBC_CHECK_NOT_NULL(rel1);
  kbc_resolve_doc docs[2] = {{id0, rel0, title0}, {id1, rel1, title1}};
  kbc_resolve_index *ix = index_of(docs, 2);
  free(id0);
  free(rel0);
  free(title0);
  free(id1);
  free(rel1);
  free(title1);
  expect(ix, "ops/deploy", KBC_RESOLVE_ONE, "bbbbbbbbbbbb");
  expect(ix, "deploy", KBC_RESOLVE_ONE, "bbbbbbbbbbbb");
  expect(ix, "x", KBC_RESOLVE_ONE, "aaaaaaaaaaaa");
  kbc_resolve_index_free(ix);
}

KBC_TEST(resolve_rejects_null_arguments_and_tolerates_null_err) {
  kbc_resolve_doc docs[1] = {DOC("i1", "a.md", "A")};
  kbc_resolve_index *ix = index_of(docs, 1);
  kbc_arena *a = kbc_arena_new(256);
  KBC_CHECK_NOT_NULL(a);
  kbc_resolution out;
  kbc_err err;
  kbc_err_reset(&err);

  KBC_CHECK_ERR(kbc_resolve_index_resolve(NULL, a, "a.md", &out, &err),
                KBC_ERR_INVALID);
  KBC_CHECK_ERR_MSG(err);
  KBC_CHECK_ERR(kbc_resolve_index_resolve(ix, a, "a.md", NULL, &err),
                KBC_ERR_INVALID);
  KBC_CHECK_ERR_MSG(err);
  KBC_CHECK_ERR(kbc_resolve_index_resolve(ix, NULL, "a.md", &out, &err),
                KBC_ERR_INVALID);
  KBC_CHECK_ERR_MSG(err);
  KBC_CHECK_ERR(kbc_resolve_index_resolve(ix, a, NULL, &out, &err),
                KBC_ERR_INVALID);
  /* A NULL arena is a caller bug, not a dangling link, and err == NULL must
   * be safe on every path — the failure one included. */
  KBC_CHECK_ERR(kbc_resolve_index_resolve(ix, NULL, "a.md", &out, NULL),
                KBC_ERR_INVALID);
  KBC_CHECK_OK(kbc_resolve_index_resolve(ix, a, "a.md", &out, NULL));
  KBC_CHECK_EQ_INT(out.kind, KBC_RESOLVE_ONE);
  kbc_strlist_free(&out.ids);
  kbc_arena_free(a);
  kbc_resolve_index_free(ix);

  kbc_err_reset(&err);
  KBC_CHECK_NULL(kbc_resolve_index_new(NULL, 1, &err));
  KBC_CHECK_ERR(err.status, KBC_ERR_INVALID);
  KBC_CHECK_ERR_MSG(err);
  kbc_resolve_index_free(NULL);
}

int main(void) {
  static const kbc_test_case cases[] = {
      {"resolve_by_id", resolve_by_id},
      {"resolve_by_path_with_and_without_ext",
       resolve_by_path_with_and_without_ext},
      {"resolve_by_title_case_insensitive", resolve_by_title_case_insensitive},
      {"resolve_title_ambiguous", resolve_title_ambiguous},
      {"resolve_by_basename", resolve_by_basename},
      {"resolve_bare_basename_ambiguous_across_folders",
       resolve_bare_basename_ambiguous_across_folders},
      {"strip_ext_leaves_dotted_directory_intact",
       strip_ext_leaves_dotted_directory_intact},
      {"resolve_none_for_dangling", resolve_none_for_dangling},
      {"normalize_strips_anchor_and_slash", normalize_strips_anchor_and_slash},
      {"index_reuse_over_a_stable_corpus", index_reuse_over_a_stable_corpus},
      {"path_tier_ext_elided_collision_is_first_doc_wins",
       path_tier_ext_elided_collision_is_first_doc_wins},
      {"path_tier_beats_title_tier", path_tier_beats_title_tier},
      {"empty_corpus_resolves_nothing", empty_corpus_resolves_nothing},
      {"targets_that_normalise_away_are_dangling",
       targets_that_normalise_away_are_dangling},
      {"target_longer_than_any_key_is_dangling",
       target_longer_than_any_key_is_dangling},
      {"degenerate_documents_do_not_poison_the_ladder",
       degenerate_documents_do_not_poison_the_ladder},
      {"first_wins_is_the_callers_order_contract",
       first_wins_is_the_callers_order_contract},
      {"extensionless_document_is_one_by_basename",
       extensionless_document_is_one_by_basename},
      {"id_tier_rejects_uppercase_and_wrong_length",
       id_tier_rejects_uppercase_and_wrong_length},
      {"backslash_fold_happens_after_the_slash_strip",
       backslash_fold_happens_after_the_slash_strip},
      {"title_tier_beats_basename_tier", title_tier_beats_basename_tier},
      {"index_outlives_the_docs_array", index_outlives_the_docs_array},
      {"resolve_rejects_null_arguments_and_tolerates_null_err",
       resolve_rejects_null_arguments_and_tolerates_null_err},
      {NULL, NULL},
  };
  return kbc_test_run("links", cases);
}
