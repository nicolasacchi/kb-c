/* kbc_parity.c — runs the cross-implementation parity suite as a ctest.
 *
 * WHY THIS EXISTS AS A SEPARATE BINARY. Every other test in this directory
 * tests kb-c against kb-c. None of them tests it against the Rust original
 * it is a port of, so "matches the original" has never once been measured
 * in this repo. tools/kbcparity/ is that measurement; this binary is the
 * gate that runs it.
 *
 * TWO JOBS, AND THE SECOND IS NOT DECORATION.
 *
 * 1. It supplies kb-c's own front-matter extraction to the harness. The
 *    kb-c daemon publishes no front matter over HTTP — the metas are store
 *    rows and no route reads them back — so the only way to put kb-c's
 *    extraction of a document's front matter next to the Rust daemon's is
 *    to call the library. This binary links it, parses every vendored
 *    corpus file with kbc_parse(), and writes the metas out as JSON.
 *    Without this the harness reports every front-matter field as a GAP —
 *    a comparison it WAS able to make and did not — and fails the run.
 *
 * 2. It propagates the harness's verdict. It does not soften it, and it
 *    does not summarise it into a pass.
 *
 * WHY A MISSING RUST DAEMON IS A SKIP AND NOT A PASS. The harness exits 77
 * (the automake skip convention) when the comparison could not be performed
 * — no Rust binary, no kb-c binary, a daemon that never came up, a corpus
 * that does not verify. This binary returns that code unchanged and
 * CMakeLists registers 77 as SKIP_RETURN_CODE, so ctest prints
 * "***Skipped" with the harness's own explanation above it. The failure
 * this exists to prevent is a gate that reports green because it did
 * nothing: a skipped gate that looks like a passing one is worse than no
 * gate at all, because it retires the question.
 *
 * USAGE
 *   kbc_parity [TOOLS_DIR]        default: $KBCPARITY_DIR, else the path
 *                                  CMake bakes in
 * Exit: 0 ok / 1 divergence or harness error / 77 skipped-blocked.
 */

#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include "kbc_test.h"

#include "kbc/kbc.h"
#include "kbc/mem.h"
#include "kbc/meta.h"
#include "kbc/parse.h"

#ifndef KBCPARITY_DEFAULT_DIR
#define KBCPARITY_DEFAULT_DIR "tools/kbcparity"
#endif

/* The harness's skip code, restated so a change on either side is a
 * compile-visible disagreement rather than a silent misclassification. */
#define PARITY_SKIP 77

/* The corpus root emit_metas joins against. A file-static rather than a
 * parameter because it is set once per root and only the recursive walk
 * changes it: threading a fifth argument through every frame would be the
 * same value everywhere and one more thing to get wrong at each call. */
static const char *g_corpus = NULL;

/* Forward-declared so emit_metas can read it; defined here because g_corpus
 * is the only state it has. A declaration with no definition is a link error
 * that reads like a missing file, so the two stay adjacent. */
static const char *corpus_dir(void) { return g_corpus != NULL ? g_corpus : ""; }

/* Writes `s` as a JSON string body. The control characters are escaped
 * because a document's own bytes reach here: front matter is attacker-
 * influenced input in this project's threat model, and a raw 0x01 in a
 * value must not be able to close the string and become structure. */
static void json_escape(FILE *f, const char *s) {
  for (const unsigned char *p = (const unsigned char *)s; *p != '\0'; p++) {
    switch (*p) {
    case '"': fputs("\\\"", f); break;
    case '\\': fputs("\\\\", f); break;
    case '\n': fputs("\\n", f); break;
    case '\r': fputs("\\r", f); break;
    case '\t': fputs("\\t", f); break;
    default:
      if (*p < 0x20u) fprintf(f, "\\u%04x", *p);
      else fputc((int)*p, f);
    }
  }
}

/* The extension test the daemons' own ingest gate uses, spelled once here so
 * the front-matter file covers exactly the documents the harness will diff.
 * A .css in the corpus is not a document, and writing a metas row for it
 * would make the harness compare a field on a document neither daemon has. */
static bool indexable(const char *path) {
  const char *dot = strrchr(path, '.');
  if (dot == NULL) return false;
  return strcmp(dot, ".md") == 0 || strcmp(dot, ".markdown") == 0 ||
         strcmp(dot, ".html") == 0 || strcmp(dot, ".htm") == 0;
}

/* One document's kb-* front matter, straight out of the parse the ingest
 * path itself uses. This is deliberately kbc_parse + kbc_parsed_metas and
 * nothing else: a second, hand-rolled front-matter reader here would be a
 * third opinion on what the document says, and the harness's whole claim is
 * that the two daemons' readers are the ones being compared.
 *
 * A multi-valued key arrives as ONE ENTRY PER ELEMENT (meta.h: a
 * comma-separated `kb-tags` is two rows in the store so the overlay can
 * match a single value). JSON has no such shape — an object may not repeat a
 * key — so the elements are joined back with commas here, which is the form
 * the Rust side publishes (`tags_csv`, indexer.rs:2763, joined with ",") and
 * the form the harness compares (`FM_FIELDS` + `fm_keys` in tools/kbcparity/
 * kbcparity, which joins the row's tag list the same way).

 * The keys are emitted the way kb-c's own reader names them: `kb-` stripped
 * and the rest lowercased (parse.c's front_matter()). The harness's
 * FM_FIELDS table maps each one onto the rust row field carrying the same
 * answer — category -> kb_category, tags -> tags, created -> created_unix,
 * status -> kb_status, severity -> kb_severity — so this file and that
 * table are two halves of one naming scheme, not two schemes.
 *
 * Emitting one object key per ELEMENT instead is not a formatting quirk; it
 * is a wrong report. `json.load` keeps the LAST of a repeated key, so a
 * document declaring `kb-tags: ladder, parity` reached the harness as
 * `parity` alone and every tag comparison for it was a false divergence
 * against a daemon that had published all of them. */
static void emit_metas(FILE *f, const char *rel) {
  char abs[KBC_TEST_PATH_MAX];
  int n = snprintf(abs, sizeof abs, "%s/%s", corpus_dir(), rel);
  if (n < 0 || (size_t)n >= sizeof abs) {
    fprintf(stderr, "  kbc_parity: path too long: %s\n", rel);
    return;
  }
  char *src = kbc_test_read_file(abs);
  if (src == NULL) return;
  size_t len = strlen(src);
  kbc_arena *a = kbc_arena_new(64u * 1024u);
  if (a == NULL) {
    free(src);
    return;
  }
  kbc_err err;
  kbc_err_reset(&err);
  kbc_parsed *p = kbc_parse(a, src, len, rel, &err);
  fputs("  \"", f);
  json_escape(f, rel);
  fputs("\": {", f);
  if (p != NULL) {
    const kbc_metas *m = kbc_parsed_metas(p);
    bool first = true;
    for (size_t i = 0; i < m->len; i++) {
      /* Every element of the key already emitted at `i` is folded into this
       * one, so the key is written once and the scan resumes past the run. */
      size_t j = i + 1;
      while (j < m->len && strcmp(m->items[j].key, m->items[i].key) == 0) j++;
      if (!first) fputs(", ", f);
      first = false;
      fputs("\"", f);
      json_escape(f, m->items[i].key);
      fputs("\": \"", f);
      json_escape(f, m->items[i].value);
      for (size_t k = i + 1; k < j; k++) {
        fputs(",", f);
        json_escape(f, m->items[k].value);
      }
      fputs("\"", f);
      i = j - 1;
    }
  }
  fputs("}", f);
  kbc_arena_free(a);
  free(src);
}

static void walk(FILE *f, const char *root, const char *rel, int *first) {
  char dir[KBC_TEST_PATH_MAX];
  int n = snprintf(dir, sizeof dir, "%s%s%s", root, rel[0] ? "/" : "", rel);
  if (n < 0 || (size_t)n >= sizeof dir) return;
  DIR *d = opendir(dir);
  if (d == NULL) return;
  struct dirent *e;
  while ((e = readdir(d)) != NULL) {
    if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0) continue;
    char child[KBC_TEST_PATH_MAX];
    int m = snprintf(child, sizeof child, "%s%s%s", rel,
                     rel[0] ? "/" : "", e->d_name);
    if (m < 0 || (size_t)m >= sizeof child) continue;
    struct stat st;
    char probe[KBC_TEST_PATH_MAX];
    int p = snprintf(probe, sizeof probe, "%s/%s", root, child);
    if (p < 0 || (size_t)p >= sizeof probe) continue;
    if (stat(probe, &st) != 0) continue;
    if (S_ISDIR(st.st_mode)) {
      walk(f, root, child, first);
    } else if (indexable(child)) {
      if (!*first) fputs(",\n", f);
      *first = 0;
      emit_metas(f, child);
    }
  }
  closedir(d);
}


/* The corpus the harness will diff is corpus/ PLUS the ladder fixtures
 * merged on top, and a document that only exists in the fixtures still has
 * front matter worth comparing. Both roots are walked so the JSON covers
 * every document the harness will ask about. */
static int write_front_matter(const char *corpus, const char *fixtures,
                              const char *out) {
  g_corpus = corpus;
  FILE *f = fopen(out, "wb");
  if (f == NULL) {
    fprintf(stderr, "  kbc_parity: cannot write %s: %s\n", out,
            strerror(errno));
    return -1;
  }
  int first = 1;
  fputs("{\n", f);
  /* walk() owns the separator via *first, including across the two roots — a
   * comma emitted here as well was a second comma after the first fixture
   * root had already written, which is a JSON syntax error rather than a
   * parseable-but-wrong document. The two roots are one object. */
  walk(f, corpus, "", &first);
  g_corpus = fixtures;
  walk(f, fixtures, "", &first);
  fputs("\n}\n", f);
  fclose(f);
  return 0;
}

static int run_harness(const char *tools, const char *json_path) {
  char harness[KBC_TEST_PATH_MAX];
  int n = snprintf(harness, sizeof harness, "%s/kbcparity", tools);
  if (n < 0 || (size_t)n >= sizeof harness) return 1;
  if (access(harness, X_OK) != 0) {
    fprintf(stderr,
            "  kbc_parity: %s is not executable — the parity suite cannot "
            "run\n", harness);
    return PARITY_SKIP;
  }
  /* execv takes char *const argv[] and the project builds with -Wcast-qual,
   * so the usual `(char *)"literal"` is not available. Mutable arrays are the
   * honest form: they live in the frame the child inherits across fork(), and
   * the harness never writes through them. */
  char arg_run[] = "run";
  char arg_fm[] = "--front-matter";
  char json[KBC_TEST_PATH_MAX];
  json[0] = '\0';
  if (json_path != NULL) {
    int jn = snprintf(json, sizeof json, "%s", json_path);
    if (jn < 0 || (size_t)jn >= sizeof json) {
      fprintf(stderr, "  kbc_parity: front-matter path too long\n");
      return 1;
    }
  }
  char *args[] = {harness, arg_run, arg_fm, json, NULL};
  fflush(stdout);
  fflush(stderr);
  pid_t pid = fork();
  if (pid < 0) {
    fprintf(stderr, "  kbc_parity: fork failed: %s\n", strerror(errno));
    return 1;
  }
  if (pid == 0) {
    execv(harness, args);
    fprintf(stderr, "  kbc_parity: exec %s failed: %s\n", harness,
            strerror(errno));
    _exit(127);
  }
  int status = 0;
  if (waitpid(pid, &status, 0) < 0) {
    fprintf(stderr, "  kbc_parity: waitpid failed: %s\n", strerror(errno));
    return 1;
  }
  if (WIFSIGNALED(status)) {
    fprintf(stderr, "  kbc_parity: harness died on signal %d\n",
            WTERMSIG(status));
    return 1;
  }
  return WEXITSTATUS(status);
}

int main(int argc, char **argv) {
  const char *tools = argc > 1 ? argv[1] : getenv("KBCPARITY_DIR");
  if (tools == NULL || tools[0] == '\0') tools = KBCPARITY_DEFAULT_DIR;

  char corpus[KBC_TEST_PATH_MAX];
  char fixtures[KBC_TEST_PATH_MAX];
  int a = snprintf(corpus, sizeof corpus, "%s/corpus", tools);
  int b = snprintf(fixtures, sizeof fixtures, "%s/fixtures/ladder", tools);
  if (a < 0 || b < 0 || (size_t)a >= sizeof corpus ||
      (size_t)b >= sizeof fixtures) {
    fprintf(stderr, "  kbc_parity: tools path too long: %s\n", tools);
    return 1;
  }

  char tmp[KBC_TEST_PATH_MAX];
  kbc_test_tmpdir(tmp, sizeof tmp);
  char json_path[KBC_TEST_PATH_MAX];
  int c = snprintf(json_path, sizeof json_path, "%s/front-matter.json", tmp);
  if (c < 0 || (size_t)c >= sizeof json_path) return 1;

  printf("== kbc parity ==\n");
  printf("tools:    %s\n", tools);
  printf("corpus:   %s\n", corpus);
  printf("fixtures: %s\n", fixtures);

  if (write_front_matter(corpus, fixtures, json_path) != 0) {
    kbc_test_rmrf(tmp);
    return 1;
  }
  printf("kb-c front matter: %s\n", json_path);

  int rc = run_harness(tools, json_path);
  kbc_test_rmrf(tmp);

  if (rc == PARITY_SKIP) {
    printf("kbc parity: SKIPPED-BLOCKED (exit %d) — the harness printed why "
           "above. This is NOT a pass.\n", PARITY_SKIP);
    return PARITY_SKIP;
  }
  if (rc != 0) {
    printf("kbc parity: FAILED (harness exit %d) — see the divergence report "
           "above.\n", rc);
  } else {
    printf("kbc parity: ok\n");
  }
  return rc;
}
