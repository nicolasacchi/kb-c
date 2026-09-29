/* test_embed.c — the vector lane: the mmap'd vector store and the stdio
 * sidecar.
 *
 * The sidecar half drives a tiny /bin/sh fake through the real fork/exec and
 * the real pipes, so every assertion is about what a caller observes: the
 * status, the health flag, the counters and the floats. The one deadline the
 * tests do not wait out is the 30s read timeout — see embedder_silent_on_embed
 * for what is asserted in its place.
 *
 * Paths are assembled with join_path()/sbuf rather than snprintf: the build
 * runs -Werror=format-truncation, and a 4096-byte root plus a leaf is a
 * warning no matter how carefully the sizes are written.
 */
#include <pthread.h>
#include <sys/wait.h>

#include "kbc/embed.h"
#include "kbc_test.h"

static char g_tmp[KBC_TEST_PATH_MAX];

/* -------------------------------------------------------------- helpers -- */

/* "dir/leaf" into dst. Returns dst, or NULL when it does not fit. */
static char *join_path(char *dst, size_t cap, const char *dir,
                       const char *leaf) {
  size_t dl = strlen(dir);
  size_t ll = strlen(leaf);
  if (dl + 1 + ll + 1 > cap) {
    return NULL;
  }
  memcpy(dst, dir, dl);
  dst[dl] = '/';
  memcpy(dst + dl + 1, leaf, ll + 1);
  return dst;
}

/* A per-case scratch directory under the suite tmpdir. */
static char *case_dir(const char *leaf) {
  static char dir[KBC_TEST_PATH_MAX];
  if (join_path(dir, sizeof dir, g_tmp, leaf) == NULL) {
    kbc_test_fail(__FILE__, __LINE__, "case dir for %s is too long", leaf);
    return NULL;
  }
  kbc_test_rmrf(dir);
  kbc_test_mkdir_p(dir);
  return dir;
}

typedef struct {
  char *p;
  size_t cap;
  size_t len;
} sbuf;

static void sb_add(sbuf *b, const char *s) {
  size_t n = strlen(s);
  if (b->len + n + 1 > b->cap) {
    b->p[b->len] = '\0'; /* truncated script: the test fails, it does not hang */
    return;
  }
  memcpy(b->p + b->len, s, n + 1);
  b->len += n;
}

/* ------------------------------------------------------- vecstore basics -- */

/* Values that survive a float round trip exactly, plus a per-row byte
 * pattern, so a stale or shifted row cannot pass unnoticed. */
static void row_pattern(uint32_t row, float *out, size_t dim) {
  for (size_t i = 0; i < dim; i++) {
    out[i] = (float)row * 1000.0f + (float)i + 0.5f;
  }
}

static void check_row(const float *got, uint32_t row, size_t dim) {
  float want[16];
  KBC_CHECK_NOT_NULL(got);
  if (got == NULL) {
    return;
  }
  row_pattern(row, want, dim);
  KBC_CHECK_MSG(memcmp(got, want, dim * sizeof(float)) == 0,
                "row %u does not match what was written", row);
}

KBC_TEST(vecstore_roundtrip) {
  char *dir = case_dir("vs-roundtrip");
  if (dir == NULL) {
    return;
  }
  char path[KBC_TEST_PATH_MAX];
  KBC_CHECK_NOT_NULL(join_path(path, sizeof path, dir, "store.vec"));

  kbc_err err;
  kbc_err_reset(&err);
  kbc_vecstore *v = kbc_vecstore_new(3, 4, &err);
  KBC_CHECK_NOT_NULL(v);
  if (v == NULL) {
    return;
  }
  KBC_CHECK_EQ_INT(kbc_vecstore_dim(v), 3);
  KBC_CHECK_EQ_INT(kbc_vecstore_count(v), 0);
  KBC_CHECK_NULL(kbc_vecstore_get(v, 0));

  /* A NULL vector is a caller error and must not advance count. */
  kbc_err_reset(&err);
  KBC_CHECK_ERR(kbc_vecstore_set(v, 0, NULL, &err), KBC_ERR_INVALID);
  KBC_CHECK_ERR_MSG(err);
  KBC_CHECK_EQ_INT(kbc_vecstore_count(v), 0);

  for (uint32_t r = 0; r < 3; r++) {
    float vec[3];
    row_pattern(r, vec, 3);
    kbc_err_reset(&err);
    KBC_CHECK_OK(kbc_vecstore_set(v, r, vec, &err));
  }
  KBC_CHECK_EQ_INT(kbc_vecstore_count(v), 3);
  for (uint32_t r = 0; r < 3; r++) {
    check_row(kbc_vecstore_get(v, r), r, 3);
  }
  /* doc_id >= count is not readable, even though capacity covers it. */
  KBC_CHECK_NULL(kbc_vecstore_get(v, 3));
  KBC_CHECK_NULL(kbc_vecstore_get(v, 4));

  kbc_arena *a = kbc_arena_new(256);
  KBC_CHECK_NOT_NULL(a);
  float got[6];
  memset(got, 0, sizeof got);
  const uint32_t ids[2] = {2, 0};
  kbc_err_reset(&err);
  KBC_CHECK_OK(kbc_vecstore_gather(v, ids, 2, a, got, &err));
  check_row(got, 2, 3);
  check_row(got + 3, 0, 3);

  /* A NULL id array with n > 0 is rejected, not dereferenced. */
  kbc_err_reset(&err);
  KBC_CHECK_ERR(kbc_vecstore_gather(v, NULL, 2, a, got, &err), KBC_ERR_INVALID);
  KBC_CHECK_ERR_MSG(err);
  /* n == 0 with no ids is a no-op, and writes nothing. */
  memset(got, 0x5a, sizeof got);
  kbc_err_reset(&err);
  KBC_CHECK_OK(kbc_vecstore_gather(v, NULL, 0, a, got, &err));
  for (size_t i = 0; i < sizeof got; i++) {
    KBC_CHECK_MSG(((const unsigned char *)got)[i] == 0x5a,
                  "gather with n=0 wrote to out[%zu]", i);
  }
  /* An id past count is rejected rather than read out of bounds. */
  const uint32_t bad[1] = {3};
  kbc_err_reset(&err);
  KBC_CHECK_ERR(kbc_vecstore_gather(v, bad, 1, a, got, &err), KBC_ERR_INVALID);
  KBC_CHECK_ERR_MSG(err);
  /* out is caller-owned, so a NULL out is a caller error too. */
  kbc_err_reset(&err);
  KBC_CHECK_ERR(kbc_vecstore_gather(v, ids, 2, a, NULL, &err), KBC_ERR_INVALID);
  KBC_CHECK_ERR_MSG(err);
  kbc_arena_free(a);

  kbc_err_reset(&err);
  KBC_CHECK_OK(kbc_vecstore_save(v, path, &err));
  kbc_vecstore_free(v);

  kbc_err_reset(&err);
  kbc_vecstore *r = kbc_vecstore_open(path, &err);
  KBC_CHECK_NOT_NULL(r);
  if (r == NULL) {
    return;
  }
  KBC_CHECK_EQ_INT(kbc_vecstore_dim(r), 3);
  KBC_CHECK_EQ_INT(kbc_vecstore_count(r), 3);
  for (uint32_t d = 0; d < 3; d++) {
    check_row(kbc_vecstore_get(r, d), d, 3);
  }
  KBC_CHECK_NULL(kbc_vecstore_get(r, 3));
  kbc_vecstore_free(r);
  kbc_test_rmrf(dir);
}

/* The 32-byte on-disk header: magic[8], version, dim, capacity, count, 8
 * reserved bytes. Written by hand, little-endian like the implementation's
 * own struct, so a hostile file can claim anything at all. */
static void write_hostile_vec(const char *path, const char *magic,
                              uint32_t version, uint32_t dim, uint32_t cap,
                              uint32_t count, size_t payload_bytes) {
  unsigned char buf[32];
  memset(buf, 0, sizeof buf);
  memcpy(buf, magic, 8);
  memcpy(buf + 8, &version, sizeof version);
  memcpy(buf + 12, &dim, sizeof dim);
  memcpy(buf + 16, &cap, sizeof cap);
  memcpy(buf + 20, &count, sizeof count);
  FILE *f = fopen(path, "wb");
  KBC_CHECK_NOT_NULL(f);
  if (f == NULL) {
    return;
  }
  KBC_CHECK_EQ_INT(fwrite(buf, 1, sizeof buf, f), (long long)sizeof buf);
  for (size_t i = 0; i < payload_bytes; i++) {
    (void)fputc(0, f);
  }
  KBC_CHECK_OK(fclose(f));
}

/* A file of exactly n bytes, for the cases where even the header is absent or
 * truncated. */
static void write_sized_file(const char *path, size_t n) {
  FILE *f = fopen(path, "wb");
  KBC_CHECK_NOT_NULL(f);
  if (f == NULL) {
    return;
  }
  for (size_t i = 0; i < n; i++) {
    (void)fputc(0, f);
  }
  KBC_CHECK_OK(fclose(f));
}

static void expect_open_rejected(const char *path, const char *what,
                                 const char *msg_substr) {
  kbc_err err;
  kbc_err_reset(&err);
  kbc_vecstore *v = kbc_vecstore_open(path, &err);
  KBC_CHECK_MSG(v == NULL, "%s: open returned a store", what);
  KBC_CHECK_ERR_MSG(err);
  KBC_CHECK_MSG(strstr(err.msg, msg_substr) != NULL,
                "%s: message \"%s\" does not mention \"%s\"", what, err.msg,
                msg_substr);
  kbc_vecstore_free(v);
}

KBC_TEST(vecstore_hostile_files) {
  char *dir = case_dir("vs-hostile");
  if (dir == NULL) {
    return;
  }
  char path[KBC_TEST_PATH_MAX];
  const char *magic = "KBCVEC\x01";

  KBC_CHECK_NOT_NULL(join_path(path, sizeof path, dir, "wrong-magic.vec"));
  write_hostile_vec(path, "NOTVEC\x00", 1, 3, 4, 2, 4 * 3 * 4);
  expect_open_rejected(path, "wrong magic", "magic");

  KBC_CHECK_NOT_NULL(join_path(path, sizeof path, dir, "dim0.vec"));
  write_hostile_vec(path, magic, 1, 0, 4, 2, 4 * 3 * 4);
  expect_open_rejected(path, "dim 0", "dim");

  KBC_CHECK_NOT_NULL(join_path(path, sizeof path, dir, "count-over-cap.vec"));
  write_hostile_vec(path, magic, 1, 3, 4, 9, 4 * 3 * 4);
  expect_open_rejected(path, "count above capacity", "count");

  /* The header claims 100 rows of 4 floats; the file holds two. Opening must
   * fail on the size check rather than map past the end of the file. */
  KBC_CHECK_NOT_NULL(join_path(path, sizeof path, dir, "short.vec"));
  write_hostile_vec(path, magic, 1, 4, 100, 100, 2 * 4 * 4);
  expect_open_rejected(path, "truncated payload", "file holds");

  /* A zero-length file never even reaches the magic check. */
  KBC_CHECK_NOT_NULL(join_path(path, sizeof path, dir, "empty.vec"));
  write_sized_file(path, 0);
  expect_open_rejected(path, "zero-length file", "header");

  /* Half a header is still not a store. */
  KBC_CHECK_NOT_NULL(join_path(path, sizeof path, dir, "half-header.vec"));
  write_sized_file(path, 20);
  expect_open_rejected(path, "truncated header", "header");

  /* A capacity of 0 claims a store that can hold nothing. */
  KBC_CHECK_NOT_NULL(join_path(path, sizeof path, dir, "cap0.vec"));
  write_hostile_vec(path, magic, 1, 3, 0, 0, 0);
  expect_open_rejected(path, "capacity 0", "capacity");

  KBC_CHECK_NOT_NULL(join_path(path, sizeof path, dir, "version.vec"));
  write_hostile_vec(path, magic, 7, 3, 4, 1, 4 * 3 * 4);
  expect_open_rejected(path, "wrong format version", "version");

  kbc_test_rmrf(dir);
}

KBC_TEST(vecstore_growth) {
  char *dir = case_dir("vs-grow");
  if (dir == NULL) {
    return;
  }
  char path[KBC_TEST_PATH_MAX];
  KBC_CHECK_NOT_NULL(join_path(path, sizeof path, dir, "grown.vec"));

  kbc_err err;
  kbc_err_reset(&err);
  kbc_vecstore *v = kbc_vecstore_new(2, 2, &err);
  KBC_CHECK_NOT_NULL(v);
  if (v == NULL) {
    return;
  }
  for (uint32_t r = 0; r < 2; r++) {
    float vec[2];
    row_pattern(r, vec, 2);
    kbc_err_reset(&err);
    KBC_CHECK_OK(kbc_vecstore_set(v, r, vec, &err));
  }
  /* doc_id 5 is far past the initial capacity of 2. */
  float far[2];
  row_pattern(5, far, 2);
  kbc_err_reset(&err);
  KBC_CHECK_OK(kbc_vecstore_set(v, 5, far, &err));
  KBC_CHECK_EQ_INT(kbc_vecstore_count(v), 6);
  /* Every earlier row survived the grow bit-for-bit. */
  for (uint32_t r = 0; r < 2; r++) {
    check_row(kbc_vecstore_get(v, r), r, 2);
  }
  check_row(kbc_vecstore_get(v, 5), 5, 2);
  KBC_CHECK_NULL(kbc_vecstore_get(v, 6));

  kbc_err_reset(&err);
  KBC_CHECK_OK(kbc_vecstore_save(v, path, &err));
  kbc_vecstore_free(v);

  kbc_err_reset(&err);
  kbc_vecstore *r = kbc_vecstore_open(path, &err);
  KBC_CHECK_NOT_NULL(r);
  if (r == NULL) {
    return;
  }
  KBC_CHECK_EQ_INT(kbc_vecstore_dim(r), 2);
  KBC_CHECK_EQ_INT(kbc_vecstore_count(r), 6);
  for (uint32_t d = 0; d < 2; d++) {
    check_row(kbc_vecstore_get(r, d), d, 2);
  }
  check_row(kbc_vecstore_get(r, 5), 5, 2);
  kbc_vecstore_free(r);
  kbc_test_rmrf(dir);
}

/* -------------------------------------------------------------- sidecar -- */

static void make_fake(const char *path, const char *body) {
  static char script[8192];
  sbuf b = {script, sizeof script, 0};
  sb_add(&b, "#!/bin/sh\n");
  sb_add(&b, body);
  kbc_test_write_file(path, script);
  KBC_CHECK_MSG(chmod(path, 0755) == 0, "chmod %s: %s", path, strerror(errno));
}

static const char kGoodBody[] =
    "while IFS= read -r line; do\n"
    "  case \"$line\" in\n"
    "  *health*) printf '%s\\n' '{\"ok\":true,\"dim\":3,\"model\":\"fake\"}' "
    ";;\n"
    "  *embed*) printf '%s\\n' '{\"ok\":true,\"dim\":3,\"vectors\":"
    "[[1.5,-2.25,3],[4,5.5,6]]}' ;;\n"
    "  esac\n"
    "done\n";

/* No child of ours may outlive the test: after a stop, waitpid must find
 * nothing left to reap. */
static void check_no_zombie(const char *what) {
  int status = 0;
  errno = 0;
  pid_t r = waitpid(-1, &status, WNOHANG);
  KBC_CHECK_MSG(r == -1 && errno == ECHILD,
                "%s: waitpid(-1, WNOHANG) returned %d (errno %s), expected -1 "
                "with ECHILD",
                what, (int)r, strerror(errno));
}

KBC_TEST(embedder_health_and_embed) {
  char *dir = case_dir("embed-good");
  if (dir == NULL) {
    return;
  }
  char script[KBC_TEST_PATH_MAX];
  KBC_CHECK_NOT_NULL(join_path(script, sizeof script, dir, "fake.sh"));
  make_fake(script, kGoodBody);

  const char *argv[2] = {script, NULL};
  kbc_err err;
  kbc_err_reset(&err);
  kbc_embedder *e = kbc_embedder_start(argv, &err);
  KBC_CHECK_NOT_NULL(e);
  if (e == NULL) {
    return;
  }
  /* start() handshakes, so the sidecar is trusted and its dim is known before
   * the first embed. (It used to be the restart() below that established
   * both; that is what left the model unknown for the life of the object.) */
  KBC_CHECK_MSG(kbc_embedder_healthy(e), "a sidecar that handshook is not healthy");
  KBC_CHECK_EQ_INT(kbc_embedder_dim(e), 3);
  kbc_err_reset(&err);
  KBC_CHECK_OK(kbc_embedder_restart(e, &err));
  KBC_CHECK_EQ_INT(kbc_embedder_dim(e), 3);
  KBC_CHECK_MSG(kbc_embedder_healthy(e), "unhealthy after a good handshake");

  kbc_arena *a = kbc_arena_new(256);
  KBC_CHECK_NOT_NULL(a);
  const char *texts[2] = {"alpha", "beta"};
  float *out = NULL;
  kbc_err_reset(&err);
  KBC_CHECK_OK(kbc_embedder_embed(e, a, texts, 2, 0, &out, &err));
  KBC_CHECK_NOT_NULL(out);
  if (out != NULL) {
    /* Exactly the floats the fake replied with, in row-major order. */
    const float want[6] = {1.5f, -2.25f, 3.0f, 4.0f, 5.5f, 6.0f};
    for (size_t i = 0; i < 6; i++) {
      KBC_CHECK_MSG(out[i] == want[i], "float %zu: got %g, want %g", i,
                    (double)out[i], (double)want[i]);
    }
  }
  int64_t requests = -1;
  kbc_embedder_counts(e, &requests, NULL);
  /* A matching dim_hint is accepted and spends exactly one more request. */
  float *hinted = NULL;
  kbc_err_reset(&err);
  KBC_CHECK_OK(kbc_embedder_embed(e, a, texts, 2, 3, &hinted, &err));
  KBC_CHECK_NOT_NULL(hinted);
  int64_t after = -1;
  kbc_embedder_counts(e, &after, NULL);
  KBC_CHECK_EQ_INT(after, requests + 1);
  /* A contradicting dim_hint is a caller error: no request, no sidecar
   * traffic, and the embedder stays trusted. */
  float *bad_hint = NULL;
  kbc_err_reset(&err);
  KBC_CHECK_ERR(kbc_embedder_embed(e, a, texts, 2, 9, &bad_hint, &err),
                KBC_ERR_INVALID);
  KBC_CHECK_ERR_MSG(err);
  KBC_CHECK_NULL(bad_hint);
  int64_t after_bad = -1;
  kbc_embedder_counts(e, &after_bad, NULL);
  KBC_CHECK_EQ_INT(after_bad, after);
  /* A NULL text is rejected before a byte reaches the pipe. */
  const char *with_null[2] = {"alpha", NULL};
  float *null_out = NULL;
  kbc_err_reset(&err);
  KBC_CHECK_ERR(kbc_embedder_embed(e, a, with_null, 2, 0, &null_out, &err),
                KBC_ERR_INVALID);
  KBC_CHECK_NULL(null_out);
  int64_t after_null = -1;
  kbc_embedder_counts(e, &after_null, NULL);
  KBC_CHECK_EQ_INT(after_null, after);
  /* n == 0 is a no-op that still clears *out. */
  float poison[1] = {7.0f};
  float *zero_out = poison;
  kbc_err_reset(&err);
  KBC_CHECK_OK(kbc_embedder_embed(e, a, NULL, 0, 0, &zero_out, &err));
  KBC_CHECK_NULL(zero_out);
  KBC_CHECK_MSG(kbc_embedder_healthy(e), "the sidecar is no longer healthy");

  kbc_arena_free(a);
  kbc_embedder_stop(e);
  check_no_zombie("embedder_health_and_embed");
  kbc_test_rmrf(dir);
}

/* Drives one deliberately misbehaving fake and pins what a caller sees: the
 * embed fails, *out stays NULL, the sidecar is no longer trusted, and the
 * failure is counted. */
static void expect_reply_rejected(const char *what, const char *body,
                                  kbc_status want) {
  char *dir = case_dir(what);
  if (dir == NULL) {
    return;
  }
  char script[KBC_TEST_PATH_MAX];
  KBC_CHECK_NOT_NULL(join_path(script, sizeof script, dir, "fake.sh"));
  make_fake(script, body);

  const char *argv[2] = {script, NULL};
  kbc_err err;
  kbc_err_reset(&err);
  kbc_embedder *e = kbc_embedder_start(argv, &err);
  if (e == NULL) {
    kbc_test_rmrf(dir);
    return;
  }
  kbc_arena *a = kbc_arena_new(256);
  KBC_CHECK_NOT_NULL(a);
  const char *texts[2] = {"alpha", "beta"};
  float poison[1] = {7.0f};
  float *out = poison;
  kbc_err_reset(&err);
  KBC_CHECK_ERR(kbc_embedder_embed(e, a, texts, 2, 0, &out, &err), want);
  KBC_CHECK_ERR_MSG(err);
  KBC_CHECK_MSG(out == NULL, "%s: *out was populated on failure", what);
  KBC_CHECK_MSG(!kbc_embedder_healthy(e), "%s: sidecar still marked healthy",
                what);
  int64_t requests = -1;
  int64_t failures = -1;
  kbc_embedder_counts(e, &requests, &failures);
  KBC_CHECK_EQ_INT(requests, 1);
  KBC_CHECK_EQ_INT(failures, 1);
  /* The child is killed on a protocol error, so a second embed reports the
   * sidecar as gone rather than reusing a desynchronised stream. */
  float *second = poison;
  kbc_err_reset(&err);
  KBC_CHECK_ERR(kbc_embedder_embed(e, a, texts, 2, 0, &second, &err),
                KBC_ERR_IO);
  KBC_CHECK_ERR_MSG(err);
  KBC_CHECK_NULL(second);
  int64_t failures2 = -1;
  kbc_embedder_counts(e, NULL, &failures2);
  KBC_CHECK_EQ_INT(failures2, 2);
  KBC_CHECK_MSG(!kbc_embedder_healthy(e), "%s: healthy after a second failure",
                what);

  kbc_arena_free(a);
  kbc_embedder_stop(e);
  check_no_zombie(what);
  kbc_test_rmrf(dir);
}

KBC_TEST(embedder_short_vectors) {
  expect_reply_rejected(
      "embedder-short-vectors",
      "while IFS= read -r line; do\n"
      "  case \"$line\" in\n"
      "  *health*) printf '%s\\n' '{\"ok\":true,\"dim\":3}' ;;\n"
      "  *embed*) printf '%s\\n' '{\"ok\":true,\"dim\":3,\"vectors\":"
      "[[1.5,-2.25,3]]}' ;;\n"
      "  esac\n"
      "done\n",
      KBC_ERR_PARSE);
}

KBC_TEST(embedder_wrong_vector_length) {
  expect_reply_rejected(
      "embedder-wrong-length",
      "while IFS= read -r line; do\n"
      "  case \"$line\" in\n"
      "  *health*) printf '%s\\n' '{\"ok\":true,\"dim\":3}' ;;\n"
      "  *embed*) printf '%s\\n' '{\"ok\":true,\"dim\":3,\"vectors\":"
      "[[1.5,-2.25],[4,5.5,6]]}' ;;\n"
      "  esac\n"
      "done\n",
      KBC_ERR_PARSE);
}

KBC_TEST(embedder_not_ok) {
  expect_reply_rejected("embedder-not-ok",
                        "while IFS= read -r line; do\n"
                        "  case \"$line\" in\n"
                        "  *health*) printf '%s\\n' '{\"ok\":true,"
                        "\"dim\":3}' ;;\n"
                        "  *embed*) printf '%s\\n' '{\"ok\":false,"
                        "\"error\":\"no model\"}' ;;\n"
                        "  esac\n"
                        "done\n",
                        KBC_ERR_PARSE);
}

KBC_TEST(embedder_non_object_reply) {
  expect_reply_rejected("embedder-non-object",
                        "while IFS= read -r line; do\n"
                        "  case \"$line\" in\n"
                        "  *health*) printf '%s\\n' '{\"ok\":true,"
                        "\"dim\":3}' ;;\n"
                        "  *embed*) printf '%s\\n' '[1.5,-2.25,3]' ;;\n"
                        "  esac\n"
                        "done\n",
                        KBC_ERR_PARSE);
}

/* A sidecar that dies mid-conversation: spawn() cannot tell that exec failed,
 * so the first exchange is the thing that notices. */
KBC_TEST(embedder_child_death) {
  char *dir = case_dir("embed-dead");
  if (dir == NULL) {
    return;
  }
  char missing[KBC_TEST_PATH_MAX];
  KBC_CHECK_NOT_NULL(join_path(missing, sizeof missing, dir, "not-a-program"));

  const char *argv[2] = {missing, NULL};
  kbc_err err;
  kbc_err_reset(&err);
  kbc_embedder *e = kbc_embedder_start(argv, &err);
  KBC_CHECK_NOT_NULL(e);
  if (e == NULL) {
    return;
  }
  kbc_arena *a = kbc_arena_new(256);
  const char *texts[1] = {"alpha"};
  float *out = NULL;
  kbc_err_reset(&err);
  KBC_CHECK_MSG(kbc_embedder_embed(e, a, texts, 1, 0, &out, &err) == KBC_ERR_IO,
                "a sidecar that cannot exec must report IO, got %s: %s",
                kbc_status_str(err.status), err.msg);
  KBC_CHECK_ERR_MSG(err);
  KBC_CHECK_NULL(out);
  KBC_CHECK_MSG(!kbc_embedder_healthy(e),
                "a sidecar that exited is still marked healthy");
  kbc_arena_free(a);
  kbc_embedder_stop(e);
  check_no_zombie("embedder_child_death");
  kbc_test_rmrf(dir);
}

/* The unhealthy -> healthy transition. The fake answers with junk until a
 * marker file appears, so the first embed kills the child and the restart
 * (with its handshake) brings a trusted sidecar back. */
KBC_TEST(embedder_restart_recovers) {
  char *dir = case_dir("embed-restart");
  if (dir == NULL) {
    return;
  }
  char script[KBC_TEST_PATH_MAX];
  char marker[KBC_TEST_PATH_MAX];
  KBC_CHECK_NOT_NULL(join_path(script, sizeof script, dir, "flaky.sh"));
  KBC_CHECK_NOT_NULL(join_path(marker, sizeof marker, dir, "good"));

  static char body[8192];
  sbuf b = {body, sizeof body, 0};
  sb_add(&b, "M=");
  sb_add(&b, "'");
  sb_add(&b, marker);
  sb_add(&b, "'\n");
  sb_add(&b,
         "while IFS= read -r line; do\n"
         "  case \"$line\" in\n"
         "  *health*) printf '%s\\n' '{\"ok\":true,\"dim\":3}' ;;\n"
         "  *embed*)\n"
         "    if [ -f \"$M\" ]; then\n"
         "      printf '%s\\n' '{\"ok\":true,\"dim\":3,\"vectors\":"
         "[[1.5,-2.25,3]]}'\n"
         "    else\n"
         "      printf '%s\\n' 'not json at all'\n"
         "    fi ;;\n"
         "  esac\n"
         "done\n");
  make_fake(script, body);

  const char *argv[2] = {script, NULL};
  kbc_err err;
  kbc_err_reset(&err);
  kbc_embedder *e = kbc_embedder_start(argv, &err);
  if (e == NULL) {
    return;
  }
  kbc_arena *a = kbc_arena_new(256);
  const char *texts[1] = {"alpha"};
  float *out = NULL;
  kbc_err_reset(&err);
  KBC_CHECK_ERR(kbc_embedder_embed(e, a, texts, 1, 0, &out, &err),
                KBC_ERR_PARSE);
  KBC_CHECK_MSG(!kbc_embedder_healthy(e), "unhealthy after a junk reply");

  /* Still broken before the marker appears: restart re-runs the handshake,
   * which this fake answers, so the embedder is trusted again — and is
   * untrusted once more the moment the sidecar lies. */
  kbc_err_reset(&err);
  KBC_CHECK_OK(kbc_embedder_restart(e, &err));
  KBC_CHECK_MSG(kbc_embedder_healthy(e), "unhealthy after a good restart");
  KBC_CHECK_EQ_INT(kbc_embedder_dim(e), 3);
  float *still_bad = NULL;
  kbc_err_reset(&err);
  KBC_CHECK_ERR(kbc_embedder_embed(e, a, texts, 1, 0, &still_bad, &err),
                KBC_ERR_PARSE);
  KBC_CHECK_NULL(still_bad);

  kbc_test_write_file(marker, "now good\n");
  kbc_err_reset(&err);
  KBC_CHECK_OK(kbc_embedder_restart(e, &err));
  float *out2 = NULL;
  kbc_err_reset(&err);
  KBC_CHECK_OK(kbc_embedder_embed(e, a, texts, 1, 3, &out2, &err));
  KBC_CHECK_NOT_NULL(out2);
  if (out2 != NULL) {
    KBC_CHECK_EQ_DBL(out2[1], -2.25, 0.0);
  }
  KBC_CHECK_MSG(kbc_embedder_healthy(e), "unhealthy after a good embed");
  int64_t failures = -1;
  kbc_embedder_counts(e, NULL, &failures);
  KBC_CHECK_EQ_INT(failures, 2);

  kbc_arena_free(a);
  kbc_embedder_stop(e);
  check_no_zombie("embedder_restart_recovers");
  kbc_test_rmrf(dir);
}

/* A sidecar that answers health but stays silent on embed. Waiting out the
 * 30s read timeout would dominate the suite, so what is asserted here is what
 * the timeout path must NOT break: the silent sidecar is still trusted after
 * a restart, and stopping it reaps it. The KBC_ERR_TIMEOUT itself belongs to
 * the deadline lane, not to this binary. */
KBC_TEST(embedder_silent_on_embed) {
  char *dir = case_dir("embed-silent");
  if (dir == NULL) {
    return;
  }
  char script[KBC_TEST_PATH_MAX];
  KBC_CHECK_NOT_NULL(join_path(script, sizeof script, dir, "silent.sh"));
  make_fake(script, "while IFS= read -r line; do\n"
                    "  case \"$line\" in\n"
                    "  *health*) printf '%s\\n' '{\"ok\":true,"
                    "\"dim\":3}' ;;\n"
                    "  esac\n"
                    "done\n");

  const char *argv[2] = {script, NULL};
  kbc_err err;
  kbc_err_reset(&err);
  kbc_embedder *e = kbc_embedder_start(argv, &err);
  if (e == NULL) {
    return;
  }
  kbc_err_reset(&err);
  KBC_CHECK_OK(kbc_embedder_restart(e, &err));
  KBC_CHECK_EQ_INT(kbc_embedder_dim(e), 3);
  KBC_CHECK_MSG(kbc_embedder_healthy(e),
                "a silent-on-embed sidecar is unhealthy after a handshake");
  kbc_embedder_stop(e);
  check_no_zombie("embedder_silent_on_embed");
  kbc_test_rmrf(dir);
}

KBC_TEST(embedder_start_rejects_bad_argv) {
  kbc_err err;
  kbc_err_reset(&err);
  const char *empty[1] = {NULL};
  KBC_CHECK_NULL(kbc_embedder_start(empty, &err));
  KBC_CHECK_ERR_MSG(err);
  kbc_err_reset(&err);
  KBC_CHECK_NULL(kbc_embedder_start(NULL, &err));
  KBC_CHECK_ERR_MSG(err);
  KBC_CHECK_MSG(!kbc_embedder_healthy(NULL), "NULL embedder looks healthy");
  KBC_CHECK_EQ_INT(kbc_embedder_dim(NULL), 0);
  kbc_embedder_stop(NULL); /* must be safe */
}

/* A sidecar that answers a request with TWO lines in one write, the way the
 * real kb-embedder does when it pushes its unsolicited `ready` and replies in
 * the same breath. One read() returns both; the client must return the first
 * and KEEP the second.
 *
 * This is the test the old single-line fakes could never have found: with
 * only one line per reply, the tail past the newline is always empty and the
 * bug is invisible. Here it is the whole point. */
static void make_burst_fake(const char *path) {
  static char script[8192];
  sbuf b = {script, sizeof script, 0};
  sb_add(&b, "#!/bin/sh\n");
  sb_add(&b, "while IFS= read -r line; do\n");
  sb_add(&b, "  case \"$line\" in\n");
  sb_add(&b, "  *health*) printf '%s\\n' '{\"ok\":true,\"dim\":3,"
             "\"model\":\"burst-model\"}' ;;\n");
  sb_add(&b, "  *embed*) printf '%s\\n%s\\n' "
             "'{\"kind\":\"ready\",\"dim\":3,\"model\":\"burst-model\"}' "
             "'{\"ok\":true,\"dim\":3,\"vectors\":[[1.5,-2.25,3]]}' ;;\n");
  sb_add(&b, "  esac\n");
  sb_add(&b, "done\n");
  kbc_test_write_file(path, script);
  KBC_CHECK_MSG(chmod(path, 0755) == 0, "chmod %s: %s", path, strerror(errno));
}

/* Every embed must succeed. A lost second line means the client waits out
 * the full 30 s deadline, returns KBC_ERR_TIMEOUT and reaps a child that
 * answered correctly — and only intermittently, because it depends on how the
 * kernel happened to split the write. So the loop runs many times: one pass
 * proves nothing about a race. */
KBC_TEST(embedder_does_not_lose_a_line_that_shared_a_read) {
  char *dir = case_dir("embed-burst");
  if (dir == NULL) {
    return;
  }
  char script[KBC_TEST_PATH_MAX];
  KBC_CHECK_NOT_NULL(join_path(script, sizeof script, dir, "burst.sh"));
  make_burst_fake(script);
  const char *argv[2] = {script, NULL};
  kbc_err err;
  kbc_err_reset(&err);
  kbc_embedder *e = kbc_embedder_start(argv, &err);
  KBC_CHECK_NOT_NULL(e);
  if (e == NULL) {
    kbc_test_rmrf(dir);
    return;
  }
  kbc_arena *a = kbc_arena_new(4096);
  KBC_CHECK_NOT_NULL(a);
  const char *texts[1] = {"alpha"};
  for (int i = 0; i < 40; i++) {
    float *out = NULL;
    kbc_err_reset(&err);
    kbc_status st = kbc_embedder_embed(e, a, texts, 1, 0, &out, &err);
    KBC_CHECK_MSG(st == KBC_OK,
                  "embed %d failed (%s: %s): a line that shared a read with "
                  "another was discarded, so the client waited out its "
                  "deadline for a reply it had already read",
                  i, kbc_status_str(st), err.msg);
    if (st == KBC_OK) {
      KBC_CHECK_NOT_NULL(out);
      if (out != NULL) {
        KBC_CHECK_EQ_DBL(out[0], 1.5, 0.0);
      }
    }
  }
  KBC_CHECK_MSG(kbc_embedder_healthy(e),
                "the sidecar was reaped after a lost line; it answered every "
                "request");

  kbc_arena_free(a);
  kbc_embedder_stop(e);
  check_no_zombie("embedder_does_not_lose_a_line_that_shared_a_read");
  kbc_test_rmrf(dir);
}

/* The same burst on the very first exchange, which is where the production
 * sidecar actually does it: the unsolicited `ready` and the answer to our
 * probe arrive together, before we have ever sent anything. */
KBC_TEST(embedder_handshake_survives_a_burst_on_the_first_read) {
  char *dir = case_dir("embed-burst-handshake");
  if (dir == NULL) {
    return;
  }
  char script[KBC_TEST_PATH_MAX];
  KBC_CHECK_NOT_NULL(join_path(script, sizeof script, dir, "burst.sh"));
  /* This one answers health with a ready AND a second line, so start's
   * handshake has to keep the one it did not want. */
  make_fake(script, "while IFS= read -r line; do\n"
                    "  case \"$line\" in\n"
                    "  *health*) printf '%s\\n%s\\n' "
                    "'{\"kind\":\"ready\",\"dim\":3,\"model\":\"burst-model\"}' "
                    "'{\"ok\":true,\"dim\":3}' ;;\n"
                    "  *embed*) printf '%s\\n' '{\"ok\":true,\"dim\":3,"
                    "\"vectors\":[[1.5,-2.25,3]]}' ;;\n"
                    "  esac\n"
                    "done\n");
  const char *argv[2] = {script, NULL};
  kbc_err err;
  kbc_err_reset(&err);
  kbc_embedder *e = kbc_embedder_start(argv, &err);
  KBC_CHECK_NOT_NULL(e);
  if (e == NULL) {
    kbc_test_rmrf(dir);
    return;
  }
  /* start() handshakes, so the model is known and the sidecar is trusted. */
  char model[160];
  kbc_err_reset(&err);
  KBC_CHECK_OK(kbc_embedder_model(e, model, sizeof model, &err));
  KBC_CHECK_EQ_STR(model, "burst-model");
  KBC_CHECK_EQ_INT(kbc_embedder_dim(e), 3);
  KBC_CHECK_MSG(kbc_embedder_healthy(e), "unhealthy after a burst handshake");

  /* The drained line's tail is still buffered, so the FIRST embed is not
   * confused by it. Whatever it is, the exchange must succeed and land the
   * right floats. */
  kbc_arena *a = kbc_arena_new(1024);
  const char *texts[1] = {"alpha"};
  float *out = NULL;
  kbc_err_reset(&err);
  KBC_CHECK_OK(kbc_embedder_embed(e, a, texts, 1, 0, &out, &err));
  if (out != NULL) {
    KBC_CHECK_EQ_DBL(out[0], 1.5, 0.0);
  }

  kbc_arena_free(a);
  kbc_embedder_stop(e);
  check_no_zombie("embedder_handshake_survives_a_burst_on_the_first_read");
  kbc_test_rmrf(dir);
}

/* A restart must not splice the dead child's buffered bytes into the new
 * child's stream. The marker flips the second generation to a sidecar with a
 * DIFFERENT dim, so a stale line from generation one shows up as a shape
 * mismatch rather than silently plausible floats. */
KBC_TEST(embedder_restart_discards_the_previous_childs_residual) {
  char *dir = case_dir("embed-residual-restart");
  if (dir == NULL) {
    return;
  }
  char script[KBC_TEST_PATH_MAX];
  char marker[KBC_TEST_PATH_MAX];
  KBC_CHECK_NOT_NULL(join_path(script, sizeof script, dir, "flip.sh"));
  KBC_CHECK_NOT_NULL(join_path(marker, sizeof marker, dir, "second"));
  static char body[8192];
  sbuf b = {body, sizeof body, 0};
  sb_add(&b, "M='");
  sb_add(&b, marker);
  sb_add(&b, "'\n");
  sb_add(&b, "while IFS= read -r line; do\n");
  sb_add(&b, "  case \"$line\" in\n");
  sb_add(&b, "  *health*)\n");
  sb_add(&b, "    if [ -f \"$M\" ]; then\n");
  sb_add(&b, "      printf '%s\\n' '{\"ok\":true,\"dim\":2,\"model\":"
             "\"model-two\"}'\n");
  sb_add(&b, "    else\n");
  sb_add(&b, "      printf '%s\\n' '{\"ok\":true,\"dim\":3,\"model\":"
             "\"model-one\"}'\n");
  sb_add(&b, "    fi ;;\n");
  sb_add(&b, "  *embed*)\n");
  sb_add(&b, "    if [ -f \"$M\" ]; then\n");
  sb_add(&b, "      printf '%s\\n' '{\"ok\":true,\"dim\":2,\"vectors\":"
             "[[7,8]]}'\n");
  sb_add(&b, "    else\n");
  /* Two lines: the first is the reply we want, the second must be kept and
   * then DISCARDED with the child, not replayed to generation two. */
  sb_add(&b, "      printf '%s\\n%s\\n' '{\"ok\":true,\"dim\":3,\"vectors\":"
             "[[1.5,-2.25,3]]}' '{\"ok\":true,\"dim\":2,\"vectors\":[[7,8]]}'\n");
  sb_add(&b, "    fi ;;\n");
  sb_add(&b, "  esac\n");
  sb_add(&b, "done\n");
  make_fake(script, body);

  const char *argv[2] = {script, NULL};
  kbc_err err;
  kbc_err_reset(&err);
  kbc_embedder *e = kbc_embedder_start(argv, &err);
  KBC_CHECK_NOT_NULL(e);
  if (e == NULL) {
    kbc_test_rmrf(dir);
    return;
  }
  char model[160];
  kbc_err_reset(&err);
  KBC_CHECK_OK(kbc_embedder_model(e, model, sizeof model, &err));
  KBC_CHECK_EQ_STR(model, "model-one");
  kbc_arena *a = kbc_arena_new(1024);
  const char *texts[1] = {"alpha"};
  float *out = NULL;
  kbc_err_reset(&err);
  KBC_CHECK_OK(kbc_embedder_embed(e, a, texts, 1, 0, &out, &err));
  /* Generation one left a line buffered that nobody asked for. */
  kbc_test_write_file(marker, "second generation\n");
  kbc_err_reset(&err);
  KBC_CHECK_OK(kbc_embedder_restart(e, &err));
  kbc_err_reset(&err);
  KBC_CHECK_OK(kbc_embedder_model(e, model, sizeof model, &err));
  KBC_CHECK_EQ_STR(model, "model-two");
  KBC_CHECK_EQ_INT(kbc_embedder_dim(e), 2);
  out = NULL;
  kbc_err_reset(&err);
  KBC_CHECK_OK(kbc_embedder_embed(e, a, texts, 1, 0, &out, &err));
  if (out != NULL) {
    /* Generation two's own floats. A spliced stale line would fail the
     * dim_hint check or hand back generation one's three-wide vector. */
    KBC_CHECK_EQ_DBL(out[0], 7.0, 0.0);
    KBC_CHECK_EQ_DBL(out[1], 8.0, 0.0);
  }

  kbc_arena_free(a);
  kbc_embedder_stop(e);
  check_no_zombie("embedder_restart_discards_the_previous_childs_residual");
  kbc_test_rmrf(dir);
}

/* start() completes a handshake before returning, so a caller that has just
 * constructed an embedder already knows the model and can trust the sidecar.
 * Without it the model is unknown for the life of the object and the query
 * lane refuses to key anything. */
KBC_TEST(embedder_start_handshakes_and_knows_the_model) {
  char *dir = case_dir("embed-start-hs");
  if (dir == NULL) {
    return;
  }
  char script[KBC_TEST_PATH_MAX];
  KBC_CHECK_NOT_NULL(join_path(script, sizeof script, dir, "good.sh"));
  make_fake(script, "while IFS= read -r line; do\n"
                    "  case \"$line\" in\n"
                    "  *health*) printf '%s\\n' '{\"ok\":true,\"dim\":3,"
                    "\"model\":\"startup-model\"}' ;;\n"
                    "  *embed*) printf '%s\\n' '{\"ok\":true,\"dim\":3,"
                    "\"vectors\":[[1.5,-2.25,3]]}' ;;\n"
                    "  esac\n"
                    "done\n");
  const char *argv[2] = {script, NULL};
  kbc_err err;
  kbc_err_reset(&err);
  kbc_embedder *e = kbc_embedder_start(argv, &err);
  KBC_CHECK_NOT_NULL(e);
  if (e == NULL) {
    kbc_test_rmrf(dir);
    return;
  }
  /* No restart() call: everything below must already be true. */
  char model[160];
  kbc_err_reset(&err);
  KBC_CHECK_OK(kbc_embedder_model(e, model, sizeof model, &err));
  KBC_CHECK_EQ_STR(model, "startup-model");
  KBC_CHECK_EQ_INT(kbc_embedder_dim(e), 3);
  KBC_CHECK_MSG(kbc_embedder_healthy(e),
                "a sidecar that handshook during start is not healthy");
  /* The handshake cost exactly one request, and a first embed works without
   * any further setup. */
  kbc_arena *a = kbc_arena_new(512);
  const char *texts[1] = {"alpha"};
  float *out = NULL;
  kbc_err_reset(&err);
  KBC_CHECK_OK(kbc_embedder_embed(e, a, texts, 1, 0, &out, &err));
  if (out != NULL) {
    KBC_CHECK_EQ_DBL(out[0], 1.5, 0.0);
  }
  kbc_arena_free(a);
  kbc_embedder_stop(e);
  check_no_zombie("embedder_start_handshakes_and_knows_the_model");
  kbc_test_rmrf(dir);
}

/* A sidecar that will not handshake is a DEGRADED start, not a failed one:
 * PORT_PLAN's contract is "no vector lane without a healthy sidecar", and
 * kbc_app degrades the vector lane everywhere else, so one broken sidecar must
 * not stop the daemon serving keyword search. The embedder comes back
 * untrusted with no model, and restart is the recovery path. */
KBC_TEST(embedder_start_degrades_when_no_handshake) {
  char *dir = case_dir("embed-start-degraded");
  if (dir == NULL) {
    return;
  }
  char script[KBC_TEST_PATH_MAX];
  KBC_CHECK_NOT_NULL(join_path(script, sizeof script, dir, "junk.sh"));
  make_fake(script, "while IFS= read -r line; do\n"
                    "  case \"$line\" in\n"
                    "  *health*) printf '%s\\n' 'not json at all' ;;\n"
                    "  esac\n"
                    "done\n");
  const char *argv[2] = {script, NULL};
  kbc_err err;
  kbc_err_reset(&err);
  kbc_embedder *e = kbc_embedder_start(argv, &err);
  KBC_CHECK_MSG(e != NULL,
                "a sidecar that will not handshake failed the whole start; it "
                "should degrade to an untrusted embedder");
  if (e == NULL) {
    kbc_test_rmrf(dir);
    return;
  }
  KBC_CHECK_MSG(!kbc_embedder_healthy(e),
                "an embedder whose handshake failed reports healthy");
  char model[160];
  memset(model, 'x', sizeof model);
  kbc_err_reset(&err);
  KBC_CHECK_ERR(kbc_embedder_model(e, model, sizeof model, &err),
                KBC_ERR_NOTFOUND);
  KBC_CHECK_MSG(model[0] == '\0',
                "a degraded start left \"%s\" as the model", model);
  /* The child that failed its handshake was reaped, so stop() finds nothing
 * left to wait for. */
  kbc_embedder_stop(e);
  check_no_zombie("embedder_start_degrades_when_no_handshake");
  kbc_test_rmrf(dir);
}

/* How many descriptors this process holds open. Read from /proc rather than
 * guessed, so the assertion is about the process and not about a model of it.
 * Returns -1 if /proc is unavailable, which the caller treats as "cannot
 * check" rather than as a failure. */
static int open_fd_count(void) {
  DIR *d = opendir("/proc/self/fd");
  if (d == NULL) {
    return -1;
  }
  int n = 0;
  struct dirent *ent;
  while ((ent = readdir(d)) != NULL) {
    if (ent->d_name[0] != '.') {
      n++;
    }
  }
  (void)closedir(d);
  return n;
}

/* Every start/stop pair must release both pipe ends. A leaked write end on the
 * child's stdin does not merely waste a descriptor: it keeps the pipe open, so
 * closing our copy never delivers EOF, the child never exits on its own, and
 * every stop burns the whole 2 s reap grace before the SIGKILL fallback. That
 * was most of this suite's wall time — about 2 s per embedder — and it grows
 * without bound in a long-lived daemon that restarts its sidecar.
 *
 * Counting fds rather than timing the stop: the descriptor count is exact and
 * the test cannot flake on a loaded machine. */
KBC_TEST(embedder_start_stop_does_not_leak_descriptors) {
  char *dir = case_dir("embed-fdleak");
  if (dir == NULL) {
    return;
  }
  char script[KBC_TEST_PATH_MAX];
  KBC_CHECK_NOT_NULL(join_path(script, sizeof script, dir, "good.sh"));
  make_fake(script, "while IFS= read -r line; do\n"
                    "  case \"$line\" in\n"
                    "  *health*) printf '%s\\n' '{\"ok\":true,\"dim\":3,"
                    "\"model\":\"leak-model\"}' ;;\n"
                    "  *embed*) printf '%s\\n' '{\"ok\":true,\"dim\":3,"
                    "\"vectors\":[[1.5,-2.25,3]]}' ;;\n"
                    "  esac\n"
                    "done\n");
  const char *argv[2] = {script, NULL};
  kbc_err err;

  /* Warm up once: the first cycle can legitimately differ (the shell's own
   * fds, lazy allocations) and we are measuring the steady state. */
  kbc_err_reset(&err);
  kbc_embedder *warm = kbc_embedder_start(argv, &err);
  KBC_CHECK_NOT_NULL(warm);
  if (warm == NULL) {
    kbc_test_rmrf(dir);
    return;
  }
  kbc_embedder_stop(warm);
  int before = open_fd_count();
  if (before < 0) {
    kbc_test_rmrf(dir); /* no /proc: nothing to assert against */
    return;
  }

  enum { kCycles = 12 };
  for (int i = 0; i < kCycles; i++) {
    kbc_err_reset(&err);
    kbc_embedder *e = kbc_embedder_start(argv, &err);
    KBC_CHECK_NOT_NULL(e);
    if (e == NULL) {
      break;
    }
    kbc_embedder_stop(e);
  }
  int after = open_fd_count();
  KBC_CHECK_MSG(after <= before,
                "%d start/stop cycles grew the descriptor count from %d to "
                "%d; the pipe ends are not being closed",
                kCycles, before, after);
  /* Restarts are the unbounded case: a long-lived daemon that keeps its
   * sidecar alive would leak on every one of them. */
  kbc_err_reset(&err);
  kbc_embedder *e = kbc_embedder_start(argv, &err);
  KBC_CHECK_NOT_NULL(e);
  if (e != NULL) {
    for (int i = 0; i < kCycles; i++) {
      kbc_err_reset(&err);
      KBC_CHECK_OK(kbc_embedder_restart(e, &err));
    }
    kbc_embedder_stop(e);
  }
  int after_restarts = open_fd_count();
  KBC_CHECK_MSG(after_restarts <= before,
                "%d restarts grew the descriptor count from %d to %d",
                kCycles, before, after_restarts);

  kbc_test_rmrf(dir);
}

/* --------------------------------------------------------- query cache -- */

/* A fake sidecar that names its model and returns a dim-3 vector, so a test
 * can tell WHICH sidecar answered rather than only that something did. The
 * announced model is what the cache key is built from. */
static void make_model_fake(const char *path, const char *model) {
  static char script[8192];
  sbuf b = {script, sizeof script, 0};
  sb_add(&b, "#!/bin/sh\n");
  sb_add(&b, "while IFS= read -r line; do\n");
  sb_add(&b, "  case \"$line\" in\n");
  sb_add(&b, "  *health*) printf '%s\\n' '{\"ok\":true,\"dim\":3,\"model\":\"");
  sb_add(&b, model);
  sb_add(&b, "\"}' ;;\n");
  sb_add(&b, "  *embed*) printf '%s\\n' '{\"ok\":true,\"dim\":3,\"vectors\":"
             "[[1.5,-2.25,3]]}' ;;\n");
  sb_add(&b, "  esac\n");
  sb_add(&b, "done\n");
  kbc_test_write_file(path, script);
  KBC_CHECK_MSG(chmod(path, 0755) == 0, "chmod %s: %s", path, strerror(errno));
}

/* Starts a fake and completes its handshake, which is where the model name
 * is learned. */
static kbc_embedder *start_model_fake(const char *dir, const char *leaf,
                                      const char *model) {
  static char script[KBC_TEST_PATH_MAX];
  KBC_CHECK_NOT_NULL(join_path(script, sizeof script, dir, leaf));
  make_model_fake(script, model);
  const char *argv[2] = {script, NULL};
  kbc_err err;
  kbc_err_reset(&err);
  kbc_embedder *e = kbc_embedder_start(argv, &err);
  KBC_CHECK_MSG(e != NULL, "start %s: %s", leaf, err.msg);
  if (e == NULL) {
    return NULL;
  }
  kbc_err_reset(&err);
  KBC_CHECK_OK(kbc_embedder_restart(e, &err));
  return e;
}

/* THE key test. Two models of the SAME dim must not share an entry: a
 * dim-keyed cache serves one model's vector for the other's query, and
 * nothing anywhere reports an error. */
KBC_TEST(query_cache_two_models_same_dim_do_not_share) {
  char *dir = case_dir("qc-two-models");
  if (dir == NULL) {
    return;
  }
  kbc_embedder *a = start_model_fake(dir, "a.sh", "bge-base-en-v1.5");
  kbc_embedder *b =
      start_model_fake(dir, "b.sh", "jina-embeddings-v2-base-code");
  kbc_err err;
  kbc_err_reset(&err);
  kbc_query_cache *c = kbc_query_cache_new(8, &err);
  kbc_arena *ar = kbc_arena_new(1024);
  KBC_CHECK_NOT_NULL(c);
  KBC_CHECK_NOT_NULL(ar);
  if (a == NULL || b == NULL || c == NULL || ar == NULL) {
    kbc_embedder_stop(a);
    kbc_embedder_stop(b);
    kbc_query_cache_free(c);
    kbc_arena_free(ar);
    kbc_test_rmrf(dir);
    return;
  }
  KBC_CHECK_EQ_INT(kbc_embedder_dim(a), 3);
  KBC_CHECK_EQ_INT(kbc_embedder_dim(b), 3);
  char ma[160];
  char mb[160];
  kbc_err_reset(&err);
  KBC_CHECK_OK(kbc_embedder_model(a, ma, sizeof ma, &err));
  KBC_CHECK_OK(kbc_embedder_model(b, mb, sizeof mb, &err));
  KBC_CHECK_EQ_STR(ma, "bge-base-en-v1.5");
  KBC_CHECK_EQ_STR(mb, "jina-embeddings-v2-base-code");

  kbc_query_outcome oa;
  kbc_query_outcome ob;
  kbc_err_reset(&err);
  KBC_CHECK_OK(kbc_embed_query(a, c, ar, "shared query", &oa, &err));
  KBC_CHECK_MSG(!oa.cache_hit, "the first query cannot be a cache hit");
  kbc_err_reset(&err);
  KBC_CHECK_OK(kbc_embed_query(b, c, ar, "shared query", &ob, &err));
  /* Same text, same dim, different model: a MISS. A hit here means the key
   * carries the dim rather than the model name. */
  KBC_CHECK_MSG(!ob.cache_hit,
                "a same-dim different-model query was served from the cache");

  /* And both entries coexist, so replaying A is a hit on A's own entry. */
  kbc_query_outcome again;
  kbc_err_reset(&err);
  KBC_CHECK_OK(kbc_embed_query(a, c, ar, "shared query", &again, &err));
  KBC_CHECK_MSG(again.cache_hit, "model A's own replay missed its entry");
  KBC_CHECK_EQ_INT(kbc_query_cache_len(c), 2);

  /* A model nobody announced shares nothing, at the same dim. */
  kbc_arena *peek = kbc_arena_new(256);
  KBC_CHECK_NOT_NULL(peek);
  const float *va = NULL;
  const float *vb = NULL;
  const float *vn = NULL;
  size_t dim = 0;
  KBC_CHECK(kbc_query_cache_get(c, "bge-base-en-v1.5", "shared query", 12, peek,
                                &va, &dim));
  KBC_CHECK(kbc_query_cache_get(c, "jina-embeddings-v2-base-code",
                                "shared query", 12, peek, &vb, &dim));
  KBC_CHECK(!kbc_query_cache_get(c, "some-other-model", "shared query", 12,
                                 peek, &vn, &dim));
  KBC_CHECK_NULL(vn);

  kbc_arena_free(peek);
  kbc_arena_free(ar);
  kbc_query_cache_free(c);
  kbc_embedder_stop(a);
  kbc_embedder_stop(b);
  check_no_zombie("query_cache_two_models_same_dim_do_not_share");
  kbc_test_rmrf(dir);
}

/* A hit does not call the sidecar and reports embed_ms 0, which is what makes
 * the field readable: 0 means no embedding step ran. */
KBC_TEST(query_cache_hit_skips_sidecar_and_reports_zero_ms) {
  char *dir = case_dir("qc-hit");
  if (dir == NULL) {
    return;
  }
  kbc_embedder *e = start_model_fake(dir, "one.sh", "fake-model");
  if (e == NULL) {
    kbc_test_rmrf(dir);
    return;
  }
  kbc_err err;
  kbc_err_reset(&err);
  kbc_query_cache *c = kbc_query_cache_new(8, &err);
  kbc_arena *ar = kbc_arena_new(1024);
  KBC_CHECK_NOT_NULL(c);
  KBC_CHECK_NOT_NULL(ar);
  if (c == NULL || ar == NULL) {
    kbc_query_cache_free(c);
    kbc_arena_free(ar);
    kbc_embedder_stop(e);
    kbc_test_rmrf(dir);
    return;
  }

  kbc_query_outcome miss;
  kbc_err_reset(&err);
  KBC_CHECK_OK(kbc_embed_query(e, c, ar, "how do I reindex?", &miss, &err));
  KBC_CHECK_MSG(!miss.cache_hit, "the first query reported a cache hit");
  KBC_CHECK_NOT_NULL(miss.vec);
  if (miss.vec != NULL) {
    KBC_CHECK_EQ_DBL(miss.vec[0], 1.5, 0.0);
    KBC_CHECK_EQ_DBL(miss.vec[1], -2.25, 0.0);
    KBC_CHECK_EQ_DBL(miss.vec[2], 3.0, 0.0);
  }
  KBC_CHECK_EQ_INT(miss.dim, 3);
  int64_t after_miss = -1;
  kbc_embedder_counts(e, &after_miss, NULL);
  KBC_CHECK_MSG(after_miss > 0, "the miss never reached the sidecar");

  kbc_query_outcome hit;
  kbc_err_reset(&err);
  KBC_CHECK_OK(kbc_embed_query(e, c, ar, "how do I reindex?", &hit, &err));
  KBC_CHECK_MSG(hit.cache_hit, "the replayed query missed the cache");
  KBC_CHECK_MSG(hit.embed_ms == 0,
                "a cache hit reported embed_ms %llu; it is 0 because no "
                "embedding step ran",
                (unsigned long long)hit.embed_ms);
  KBC_CHECK_EQ_INT(hit.dim, 3);
  KBC_CHECK_NOT_NULL(hit.vec);
  if (hit.vec != NULL) {
    KBC_CHECK_EQ_DBL(hit.vec[0], 1.5, 0.0);
    KBC_CHECK_EQ_DBL(hit.vec[1], -2.25, 0.0);
  }
  /* The proof the sidecar was not called: the request counter is unchanged.
   * A hit that quietly re-embedded would move it. */
  int64_t after_hit = -1;
  kbc_embedder_counts(e, &after_hit, NULL);
  KBC_CHECK_MSG(after_hit == after_miss,
                "a cache hit spent %lld extra sidecar request(s)",
                (long long)(after_hit - after_miss));

  /* A different query under the same model is a separate entry and does
   * reach the sidecar. */
  kbc_query_outcome other;
  kbc_err_reset(&err);
  KBC_CHECK_OK(
      kbc_embed_query(e, c, ar, "how do I rebuild the index?", &other, &err));
  KBC_CHECK_MSG(!other.cache_hit, "a different query hit the first entry");
  int64_t after_other = -1;
  kbc_embedder_counts(e, &after_other, NULL);
  KBC_CHECK_EQ_INT(after_other, after_miss + 1);
  KBC_CHECK_EQ_INT(kbc_query_cache_len(c), 2);

  kbc_arena_free(ar);
  kbc_query_cache_free(c);
  kbc_embedder_stop(e);
  check_no_zombie("query_cache_hit_skips_sidecar_and_reports_zero_ms");
  kbc_test_rmrf(dir);
}

/* A get TOUCHES: it moves the entry to the front, so the next put evicts a
 * DIFFERENT entry than it would have without the touch. Without the move the
 * cache is a FIFO and "aaa survived" below fails. */
KBC_TEST(query_cache_get_touches_lru_order) {
  kbc_err err;
  kbc_err_reset(&err);
  kbc_query_cache *c = kbc_query_cache_new(3, &err);
  KBC_CHECK_NOT_NULL(c);
  if (c == NULL) {
    return;
  }
  const float v[3] = {1.0f, 2.0f, 3.0f};
  kbc_err_reset(&err);
  KBC_CHECK_OK(kbc_query_cache_put(c, "m", "aaa", 3, v, 3, &err));
  KBC_CHECK_OK(kbc_query_cache_put(c, "m", "bbb", 3, v, 3, &err));
  KBC_CHECK_OK(kbc_query_cache_put(c, "m", "ccc", 3, v, 3, &err));
  KBC_CHECK_EQ_INT(kbc_query_cache_len(c), 3);

  /* Touch the oldest. LRU order becomes ccc, aaa, bbb — bbb is the victim.
   * Without the touch it would be aaa. */
  kbc_arena *a1 = kbc_arena_new(256);
  KBC_CHECK_NOT_NULL(a1);
  const float *got = NULL;
  size_t dim = 0;
  KBC_CHECK(kbc_query_cache_get(c, "m", "aaa", 3, a1, &got, &dim));
  KBC_CHECK_NOT_NULL(got);
  KBC_CHECK_EQ_INT(dim, 3);

  kbc_err_reset(&err);
  KBC_CHECK_OK(kbc_query_cache_put(c, "m", "ddd", 3, v, 3, &err));
  KBC_CHECK_EQ_INT(kbc_query_cache_len(c), 3);

  kbc_arena *a2 = kbc_arena_new(256);
  KBC_CHECK_NOT_NULL(a2);
  const float *out = NULL;
  size_t d2 = 0;
  KBC_CHECK_MSG(kbc_query_cache_get(c, "m", "aaa", 3, a2, &out, &d2),
                "the touched entry was evicted; get is not touching");
  KBC_CHECK_MSG(!kbc_query_cache_get(c, "m", "bbb", 3, a2, &out, &d2),
                "the least-recently-used entry survived an overflowing put");
  KBC_CHECK(kbc_query_cache_get(c, "m", "ccc", 3, a2, &out, &d2));
  KBC_CHECK(kbc_query_cache_get(c, "m", "ddd", 3, a2, &out, &d2));

  kbc_arena_free(a1);
  kbc_arena_free(a2);
  kbc_query_cache_free(c);
}

/* Capacity is a ceiling that is never exceeded, and re-putting a live key
 * REPLACES rather than appends. The ceiling is asserted after EVERY put: an
 * implementation that appends and trims on the next call passes a check made
 * only at the end. */
KBC_TEST(query_cache_capacity_ceiling_and_reput) {
  kbc_err err;
  kbc_err_reset(&err);
  kbc_query_cache *c = kbc_query_cache_new(4, &err);
  KBC_CHECK_NOT_NULL(c);
  if (c == NULL) {
    return;
  }
  KBC_CHECK_EQ_INT(kbc_query_cache_capacity(c), 4);
  const float v[3] = {1.0f, 2.0f, 3.0f};
  char key[32];

  for (int i = 0; i < 40; i++) {
    KBC_CHECK_EQ_INT(snprintf(key, sizeof key, "q%02d", i), 3);
    kbc_err_reset(&err);
    KBC_CHECK_OK(kbc_query_cache_put(c, "m", key, strlen(key), v, 3, &err));
    KBC_CHECK_MSG(kbc_query_cache_len(c) <= 4,
                  "after %d puts into a capacity of 4 the cache holds %zu", i,
                  kbc_query_cache_len(c));
  }
  KBC_CHECK_EQ_INT(kbc_query_cache_len(c), 4);

  kbc_arena *a = kbc_arena_new(256);
  KBC_CHECK_NOT_NULL(a);
  const float *got = NULL;
  size_t dim = 0;
  for (int i = 36; i < 40; i++) {
    KBC_CHECK_EQ_INT(snprintf(key, sizeof key, "q%02d", i), 3);
    KBC_CHECK_MSG(kbc_query_cache_get(c, "m", key, strlen(key), a, &got, &dim),
                  "recently inserted key %s was evicted", key);
  }
  KBC_CHECK(!kbc_query_cache_get(c, "m", "q00", 3, a, &got, &dim));
  KBC_CHECK(!kbc_query_cache_get(c, "m", "q35", 3, a, &got, &dim));

  kbc_err_reset(&err);
  KBC_CHECK_OK(kbc_query_cache_put(c, "m", "q39", 3, v, 3, &err));
  KBC_CHECK_EQ_INT(kbc_query_cache_len(c), 4);
  const float replacement[3] = {9.0f, 8.0f, 7.0f};
  kbc_err_reset(&err);
  KBC_CHECK_OK(kbc_query_cache_put(c, "m", "q39", 3, replacement, 3, &err));
  KBC_CHECK_MSG(kbc_query_cache_len(c) == 4,
                "re-putting a live key grew the cache to %zu",
                kbc_query_cache_len(c));
  KBC_CHECK(kbc_query_cache_get(c, "m", "q39", 3, a, &got, &dim));
  if (got != NULL) {
    KBC_CHECK_EQ_DBL(got[0], 9.0, 0.0);
    KBC_CHECK_EQ_DBL(got[2], 7.0, 0.0);
  }

  for (int i = 0; i < 200; i++) {
    kbc_err_reset(&err);
    KBC_CHECK_OK(kbc_query_cache_put(c, "m", "q38", 3, v, 3, &err));
  }
  KBC_CHECK_EQ_INT(kbc_query_cache_len(c), 4);

  kbc_arena_free(a);
  kbc_query_cache_free(c);
}

/* The key is the RAW query: no trim, no case folding, no whitespace
 * collapse, and length travels with the bytes so an embedded NUL is data
 * rather than a terminator. */
KBC_TEST(query_cache_key_is_the_raw_query) {
  kbc_err err;
  kbc_err_reset(&err);
  kbc_query_cache *c = kbc_query_cache_new(16, &err);
  KBC_CHECK_NOT_NULL(c);
  if (c == NULL) {
    return;
  }
  const float v[3] = {1.0f, 2.0f, 3.0f};
  const char *variants[] = {"query", " query", "query ", "QUERY", "Query",
                            "qu ery"};
  const size_t nvariants = sizeof variants / sizeof variants[0];
  for (size_t i = 0; i < nvariants; i++) {
    kbc_err_reset(&err);
    KBC_CHECK_OK(kbc_query_cache_put(c, "m", variants[i], strlen(variants[i]),
                                      v, 3, &err));
  }
  KBC_CHECK_MSG(kbc_query_cache_len(c) == nvariants,
                "the cache holds %zu of %zu raw-query variants; the key is "
                "being normalised",
                kbc_query_cache_len(c), nvariants);
  kbc_arena *a = kbc_arena_new(256);
  KBC_CHECK_NOT_NULL(a);
  const float *got = NULL;
  size_t dim = 0;
  for (size_t i = 0; i < nvariants; i++) {
    KBC_CHECK_MSG(kbc_query_cache_get(c, "m", variants[i], strlen(variants[i]),
                                      a, &got, &dim),
                  "variant \"%s\" did not get its own entry", variants[i]);
  }
  /* The key is (len, bytes): a query whose tail follows a NUL is a DIFFERENT
   * entry from its own prefix. Truncating at the NUL would let the two
   * collide and serve one vector for two different queries. */
  const char embedded[8] = {'a', 'b', '\0', 'c', 'd', 'e', 'f', '\0'};
  kbc_err_reset(&err);
  KBC_CHECK_OK(kbc_query_cache_put(c, "m", embedded, 8, v, 3, &err));
  KBC_CHECK(kbc_query_cache_get(c, "m", embedded, 8, a, &got, &dim));
  KBC_CHECK_MSG(!kbc_query_cache_get(c, "m", embedded, 2, a, &got, &dim),
                "a NUL-truncated key collided with the full-length key");
  KBC_CHECK_MSG(kbc_query_cache_len(c) == nvariants + 1,
                "the cache holds %zu entries, expected the %zu variants plus "
                "the embedded-NUL key",
                kbc_query_cache_len(c), nvariants);

  kbc_arena_free(a);
  kbc_query_cache_free(c);
}

/* Re-putting a live key REPLACES it. With room to spare, a put that appended
 * instead of removing the old entry would grow the cache by one per call, and
 * the capacity ceiling would not hide it. */
KBC_TEST(query_cache_reput_does_not_grow) {
  kbc_err err;
  kbc_err_reset(&err);
  kbc_query_cache *c = kbc_query_cache_new(64, &err);
  KBC_CHECK_NOT_NULL(c);
  if (c == NULL) {
    return;
  }
  const float v[3] = {1.0f, 2.0f, 3.0f};
  kbc_err_reset(&err);
  KBC_CHECK_OK(kbc_query_cache_put(c, "m", "same", 4, v, 3, &err));
  KBC_CHECK_EQ_INT(kbc_query_cache_len(c), 1);
  for (int i = 0; i < 20; i++) {
    kbc_err_reset(&err);
    KBC_CHECK_OK(kbc_query_cache_put(c, "m", "same", 4, v, 3, &err));
    KBC_CHECK_MSG(kbc_query_cache_len(c) == 1,
                  "re-putting one key %d times left %zu entries; a put must "
                  "replace, not append",
                  i + 1, kbc_query_cache_len(c));
  }
  kbc_query_cache_free(c);
}

/* A miss's embed_ms is a real duration and a hit's is 0. The fake sleeps on
 * the embed branch, so the miss is tens of milliseconds: without that a miss
 * could round to 0 ms and the hit's 0 would prove nothing. */
KBC_TEST(query_cache_embed_ms_is_zero_only_on_a_hit) {
  char *dir = case_dir("qc-ms");
  if (dir == NULL) {
    return;
  }
  char script[KBC_TEST_PATH_MAX];
  KBC_CHECK_NOT_NULL(join_path(script, sizeof script, dir, "slow.sh"));
  make_fake(script, "while IFS= read -r line; do\n"
                    "  case \"$line\" in\n"
                    "  *health*) printf '%s\\n' '{\"ok\":true,\"dim\":3,"
                    "\"model\":\"slow-model\"}' ;;\n"
                    "  *embed*) sleep 0.05; printf '%s\\n' '{\"ok\":true,"
                    "\"dim\":3,\"vectors\":[[1.5,-2.25,3]]}' ;;\n"
                    "  esac\n"
                    "done\n");
  const char *argv[2] = {script, NULL};
  kbc_err err;
  kbc_err_reset(&err);
  kbc_embedder *e = kbc_embedder_start(argv, &err);
  KBC_CHECK_NOT_NULL(e);
  if (e == NULL) {
    kbc_test_rmrf(dir);
    return;
  }
  kbc_err_reset(&err);
  KBC_CHECK_OK(kbc_embedder_restart(e, &err));
  kbc_err_reset(&err);
  kbc_query_cache *c = kbc_query_cache_new(8, &err);
  kbc_arena *a = kbc_arena_new(256);
  KBC_CHECK_NOT_NULL(c);
  KBC_CHECK_NOT_NULL(a);
  if (c == NULL || a == NULL) {
    kbc_query_cache_free(c);
    kbc_arena_free(a);
    kbc_embedder_stop(e);
    kbc_test_rmrf(dir);
    return;
  }

  kbc_query_outcome miss;
  kbc_query_outcome hit;
  kbc_err_reset(&err);
  KBC_CHECK_OK(kbc_embed_query(e, c, a, "a slow query", &miss, &err));
  KBC_CHECK_MSG(!miss.cache_hit, "the first query was a hit");
  KBC_CHECK_MSG(miss.embed_ms > 0,
                "a miss that spent ~50 ms in the sidecar reported embed_ms 0");

  kbc_err_reset(&err);
  KBC_CHECK_OK(kbc_embed_query(e, c, a, "a slow query", &hit, &err));
  KBC_CHECK_MSG(hit.cache_hit, "the replayed query missed the cache");
  /* The sharp form: same query, same model, 0 ms because the embedding step
   * did not run. Timing the whole call here would make the field say nothing
   * about whether inference happened. */
  KBC_CHECK_MSG(hit.embed_ms == 0,
                "a cache hit reported embed_ms %llu; it must be 0",
                (unsigned long long)hit.embed_ms);

  /* A different query is a fresh miss and pays the sleep again, so the 0 on
   * the hit is the cache and not a stuck counter. */
  kbc_query_outcome other;
  kbc_err_reset(&err);
  KBC_CHECK_OK(kbc_embed_query(e, c, a, "another slow query", &other, &err));
  KBC_CHECK_MSG(!other.cache_hit, "a different query hit the cache");
  KBC_CHECK_MSG(other.embed_ms > 0, "the second miss reported embed_ms 0");

  kbc_arena_free(a);
  kbc_query_cache_free(c);
  kbc_embedder_stop(e);
  check_no_zombie("query_cache_embed_ms_is_zero_only_on_a_hit");
  kbc_test_rmrf(dir);
}

/* The vector handed to a caller is the CALLER's: it is an arena copy, so it
 * survives a later put that replaces the same key. A cache that returned its
 * own buffer would show the second put's floats here. */
KBC_TEST(query_cache_hit_vector_is_an_independent_copy) {
  kbc_err err;
  kbc_err_reset(&err);
  kbc_query_cache *c = kbc_query_cache_new(4, &err);
  KBC_CHECK_NOT_NULL(c);
  if (c == NULL) {
    return;
  }
  const float first[3] = {1.0f, 2.0f, 3.0f};
  kbc_err_reset(&err);
  KBC_CHECK_OK(kbc_query_cache_put(c, "m", "q", 1, first, 3, &err));

  kbc_arena *a = kbc_arena_new(256);
  KBC_CHECK_NOT_NULL(a);
  const float *held = NULL;
  size_t dim = 0;
  KBC_CHECK(kbc_query_cache_get(c, "m", "q", 1, a, &held, &dim));
  KBC_CHECK_NOT_NULL(held);
  if (held == NULL) {
    kbc_arena_free(a);
    kbc_query_cache_free(c);
    return;
  }
  KBC_CHECK_EQ_DBL(held[0], 1.0, 0.0);

  const float second[3] = {42.0f, 43.0f, 44.0f};
  kbc_err_reset(&err);
  KBC_CHECK_OK(kbc_query_cache_put(c, "m", "q", 1, second, 3, &err));
  KBC_CHECK_EQ_INT(kbc_query_cache_len(c), 1);
  KBC_CHECK_MSG(held[0] == 1.0f && held[2] == 3.0f,
                "the previously returned vector changed under the caller: it "
                "aliased the cache's buffer (%g, %g)",
                (double)held[0], (double)held[2]);

  const float *fresh = NULL;
  size_t fdim = 0;
  KBC_CHECK(kbc_query_cache_get(c, "m", "q", 1, a, &fresh, &fdim));
  if (fresh != NULL) {
    KBC_CHECK_EQ_DBL(fresh[0], 42.0, 0.0);
  }

  kbc_arena_free(a);
  kbc_query_cache_free(c);
}

/* Hostile input is refused, not absorbed: a zero capacity, a zero-width
 * vector, a NULL vector, and a query past the key ceiling. A truncated key
 * would collide with every other query sharing the prefix, which is the
 * failure the model name in the key exists to prevent. */
KBC_TEST(query_cache_rejects_bad_arguments) {
  kbc_err err;
  kbc_err_reset(&err);
  KBC_CHECK_NULL(kbc_query_cache_new(0, &err));
  KBC_CHECK_ERR_MSG(err);
  KBC_CHECK_EQ_INT(kbc_query_cache_len(NULL), 0);
  KBC_CHECK_EQ_INT(kbc_query_cache_capacity(NULL), 0);
  kbc_query_cache_free(NULL); /* must be safe */

  kbc_err_reset(&err);
  kbc_query_cache *c = kbc_query_cache_new(2, &err);
  KBC_CHECK_NOT_NULL(c);
  if (c == NULL) {
    return;
  }
  const float v[3] = {1.0f, 2.0f, 3.0f};
  kbc_err_reset(&err);
  KBC_CHECK_ERR(kbc_query_cache_put(c, "m", "q", 1, v, 0, &err),
                KBC_ERR_INVALID);
  KBC_CHECK_ERR_MSG(err);
  kbc_err_reset(&err);
  KBC_CHECK_ERR(kbc_query_cache_put(c, "m", "q", 1, NULL, 3, &err),
                KBC_ERR_INVALID);
  kbc_err_reset(&err);
  KBC_CHECK_ERR(kbc_query_cache_put(NULL, "m", "q", 1, v, 3, &err),
                KBC_ERR_INVALID);
  /* A rejected put leaves nothing behind. */
  KBC_CHECK_EQ_INT(kbc_query_cache_len(c), 0);

  kbc_arena *a = kbc_arena_new(256);
  KBC_CHECK_NOT_NULL(a);
  kbc_query_outcome o;
  kbc_err_reset(&err);
  KBC_CHECK_ERR(kbc_embed_query(NULL, c, a, "q", &o, &err), KBC_ERR_INVALID);
  KBC_CHECK_ERR_MSG(err);

  kbc_arena_free(a);
  kbc_query_cache_free(c);
}

/* An empty query is refused at the query seam and never reaches the
 * sidecar: a zero-length text is not a search, and caching it would let it
 * evict a real query. */
KBC_TEST(query_cache_rejects_empty_query) {
  char *dir = case_dir("qc-empty");
  if (dir == NULL) {
    return;
  }
  kbc_embedder *e = start_model_fake(dir, "one.sh", "fake-model");
  if (e == NULL) {
    kbc_test_rmrf(dir);
    return;
  }
  kbc_err err;
  kbc_err_reset(&err);
  kbc_query_cache *c = kbc_query_cache_new(4, &err);
  kbc_arena *a = kbc_arena_new(256);
  KBC_CHECK_NOT_NULL(c);
  KBC_CHECK_NOT_NULL(a);
  kbc_query_outcome o;
  memset(&o, 0xff, sizeof o);
  kbc_err_reset(&err);
  KBC_CHECK_ERR(kbc_embed_query(e, c, a, "", &o, &err), KBC_ERR_INVALID);
  KBC_CHECK_ERR_MSG(err);
  KBC_CHECK_EQ_INT(kbc_query_cache_len(c), 0);
  int64_t requests = -1;
  kbc_embedder_counts(e, &requests, NULL);
  KBC_CHECK_MSG(requests == 0, "an empty query spent %lld sidecar request(s)",
                (long long)requests);

  kbc_arena_free(a);
  kbc_query_cache_free(c);
  kbc_embedder_stop(e);
  check_no_zombie("query_cache_rejects_empty_query");
  kbc_test_rmrf(dir);
}

/* A sidecar that names no model still gets a cache, keyed on the empty name.
 * Refusing to cache would trade a slow query for a lane that re-embeds
 * forever, which is the wrong trade. */
KBC_TEST(query_cache_works_without_an_announced_model) {
  char *dir = case_dir("qc-nomodel");
  if (dir == NULL) {
    return;
  }
  char script[KBC_TEST_PATH_MAX];
  KBC_CHECK_NOT_NULL(join_path(script, sizeof script, dir, "anon.sh"));
  make_fake(script, "while IFS= read -r line; do\n"
                    "  case \"$line\" in\n"
                    "  *health*) printf '%s\\n' '{\"ok\":true,\"dim\":3}' ;;\n"
                    "  *embed*) printf '%s\\n' '{\"ok\":true,\"dim\":3,"
                    "\"vectors\":[[1.5,-2.25,3]]}' ;;\n"
                    "  esac\n"
                    "done\n");
  const char *argv[2] = {script, NULL};
  kbc_err err;
  kbc_err_reset(&err);
  kbc_embedder *e = kbc_embedder_start(argv, &err);
  KBC_CHECK_NOT_NULL(e);
  if (e == NULL) {
    kbc_test_rmrf(dir);
    return;
  }
  kbc_err_reset(&err);
  KBC_CHECK_OK(kbc_embedder_restart(e, &err));
  /* A sidecar that HANDSHOOK and named no model is not an error: the empty
   * name is a legitimate key component. This is the case that is distinct
   * from never having handshook at all, which the next test pins. */
  char model[160];
  kbc_err_reset(&err);
  KBC_CHECK_OK(kbc_embedder_model(e, model, sizeof model, &err));
  KBC_CHECK_EQ_STR(model, "");

  kbc_err_reset(&err);
  kbc_query_cache *c = kbc_query_cache_new(4, &err);
  kbc_arena *a = kbc_arena_new(256);
  KBC_CHECK_NOT_NULL(c);
  KBC_CHECK_NOT_NULL(a);
  kbc_query_outcome miss;
  kbc_query_outcome hit;
  kbc_err_reset(&err);
  KBC_CHECK_OK(kbc_embed_query(e, c, a, "who goes there", &miss, &err));
  kbc_err_reset(&err);
  KBC_CHECK_OK(kbc_embed_query(e, c, a, "who goes there", &hit, &err));
  KBC_CHECK_MSG(!miss.cache_hit, "the first query was a hit");
  KBC_CHECK_MSG(hit.cache_hit,
                "an unnamed model is not cached, so every query re-embeds");
  KBC_CHECK_MSG(hit.embed_ms == 0, "the hit reported a non-zero embed_ms");
  int64_t requests = -1;
  kbc_embedder_counts(e, &requests, NULL);
  KBC_CHECK_EQ_INT(requests, 1);

  kbc_arena_free(a);
  kbc_query_cache_free(c);
  kbc_embedder_stop(e);
  check_no_zombie("query_cache_works_without_an_announced_model");
  kbc_test_rmrf(dir);
}

/* An embedder that has NOT handshook has no model, and saying so is a status
 * rather than a silent empty string: a caller that cannot tell "unknown yet"
 * from "the sidecar named nothing" will cache vectors under a key it never
 * chose, which is the failure the model name in the key exists to prevent.
 * Every failure also leaves `out` empty rather than partially filled, since a
 * half-copied name is a valid C string that is not the model's name. */
KBC_TEST(embedder_model_is_notfound_before_a_handshake) {
  char *dir = case_dir("qc-model-unknown");
  if (dir == NULL) {
    return;
  }
  /* One fake that answers a good handshake until a marker appears, so the
   * SAME embedder can be walked from "handshook, model known" to
   * "restart failed, model unknown" without swapping children underneath. */
  char script[KBC_TEST_PATH_MAX];
  char marker[KBC_TEST_PATH_MAX];
  KBC_CHECK_NOT_NULL(join_path(script, sizeof script, dir, "flaky.sh"));
  KBC_CHECK_NOT_NULL(join_path(marker, sizeof marker, dir, "broken"));
  static char body[8192];
  sbuf b = {body, sizeof body, 0};
  sb_add(&b, "M='");
  sb_add(&b, marker);
  sb_add(&b, "'\n");
  sb_add(&b,
         "while IFS= read -r line; do\n"
         "  case \"$line\" in\n"
         "  *health*)\n"
         "    if [ -f \"$M\" ]; then printf '%s\\n' 'not json at all';\n"
         "    else printf '%s\\n' '{\"ok\":true,\"dim\":3,"
         "\"model\":\"some-model\"}'; fi ;;\n"
         "  *embed*) printf '%s\\n' '{\"ok\":true,\"dim\":3,\"vectors\":"
         "[[1.5,-2.25,3]]}' ;;\n"
         "  esac\n"
         "done\n");
  make_fake(script, body);
  const char *argv[2] = {script, NULL};
  kbc_err err;
  kbc_err_reset(&err);
  kbc_embedder *e = kbc_embedder_start(argv, &err);
  KBC_CHECK_NOT_NULL(e);
  if (e == NULL) {
    kbc_test_rmrf(dir);
    return;
  }

  /* start() handshakes, so the name is already known here — a fresh start is
   * NOT the "unknown" case any more, which is the point of the D2 fix. */
  char out[64];
  kbc_err_reset(&err);
  KBC_CHECK_OK(kbc_embedder_model(e, out, sizeof out, &err));
  KBC_CHECK_EQ_STR(out, "some-model");

  /* Now break it and restart. This is the ONLY way to reach "unknown", and
   * that is right: a live child exists only while some child has handshook.
   * The model must NOT still be the old child's — it is gone, and its name is
   * not a fact about whatever loads next. Serving it would key the new
   * sidecar's vectors under the old model's name, which is precisely the
   * collision the key exists to prevent. */
  kbc_test_write_file(marker, "broken now\n");
  kbc_err_reset(&err);
  KBC_CHECK_ERR(kbc_embedder_restart(e, &err), KBC_ERR_PARSE);
  memset(out, 'x', sizeof out);
  kbc_err_reset(&err);
  KBC_CHECK_ERR(kbc_embedder_model(e, out, sizeof out, &err), KBC_ERR_NOTFOUND);
  KBC_CHECK_MSG(out[0] == '\0',
                "a failed restart left the old model readable as \"%s\"", out);

  /* Bad arguments are refused rather than dereferenced, and a NULL embedder
   * is safe to ask. */
  kbc_err_reset(&err);
  KBC_CHECK_ERR(kbc_embedder_model(NULL, out, sizeof out, &err),
                KBC_ERR_INVALID);
  KBC_CHECK_ERR_MSG(err);
  kbc_err_reset(&err);
  KBC_CHECK_ERR(kbc_embedder_model(e, NULL, 8, &err), KBC_ERR_INVALID);
  kbc_err_reset(&err);
  KBC_CHECK_ERR(kbc_embedder_model(e, out, 0, &err), KBC_ERR_INVALID);
  /* err == NULL must be safe on every one of those paths. */
  (void)kbc_embedder_model(NULL, out, sizeof out, NULL);
  (void)kbc_embedder_model(e, out, 0, NULL);

  kbc_embedder_stop(e);
  check_no_zombie("embedder_model_is_notfound_before_a_handshake");
  kbc_test_rmrf(dir);
}

/* A buffer too small for the model is refused. Clipping it instead would hand
 * back a valid C string that is not the model's name — the same silent key
 * collision the name in the key prevents, reached from the other direction. */
KBC_TEST(embedder_model_refuses_a_short_buffer) {
  char *dir = case_dir("qc-model-short");
  if (dir == NULL) {
    return;
  }
  kbc_embedder *e = start_model_fake(dir, "one.sh", "jina-embeddings-v2-base");
  if (e == NULL) {
    kbc_test_rmrf(dir);
    return;
  }
  char small[8];
  kbc_err err;
  kbc_err_reset(&err);
  KBC_CHECK_ERR(kbc_embedder_model(e, small, sizeof small, &err),
                KBC_ERR_INVALID);
  KBC_CHECK_ERR_MSG(err);
  KBC_CHECK_MSG(small[0] == '\0',
                "a refused short read left \"%s\" behind", small);
  /* A buffer that fits exactly still works: n+1 bytes, no off-by-one. */
  char exact[28];
  kbc_err_reset(&err);
  KBC_CHECK_OK(kbc_embedder_model(e, exact, sizeof exact, &err));
  KBC_CHECK_EQ_STR(exact, "jina-embeddings-v2-base");

  kbc_embedder_stop(e);
  check_no_zombie("embedder_model_refuses_a_short_buffer");
  kbc_test_rmrf(dir);
}

/* `c == NULL` embeds unconditionally — the header's promise that a caller
 * with no cache is not a special case. Every call misses, every call reaches
 * the sidecar, and the floats are still right. */
KBC_TEST(query_cache_null_cache_embeds_every_time) {
  char *dir = case_dir("qc-nocache");
  if (dir == NULL) {
    return;
  }
  kbc_embedder *e = start_model_fake(dir, "one.sh", "fake-model");
  if (e == NULL) {
    kbc_test_rmrf(dir);
    return;
  }
  kbc_arena *a = kbc_arena_new(512);
  KBC_CHECK_NOT_NULL(a);
  kbc_err err;
  kbc_query_outcome first;
  kbc_query_outcome second;
  kbc_err_reset(&err);
  KBC_CHECK_OK(kbc_embed_query(e, NULL, a, "same text", &first, &err));
  kbc_err_reset(&err);
  KBC_CHECK_OK(kbc_embed_query(e, NULL, a, "same text", &second, &err));
  KBC_CHECK_MSG(!first.cache_hit && !second.cache_hit,
                "a NULL cache reported a hit");
  if (first.vec != NULL && second.vec != NULL) {
    KBC_CHECK_EQ_DBL(first.vec[0], 1.5, 0.0);
    KBC_CHECK_EQ_DBL(second.vec[0], 1.5, 0.0);
  }
  int64_t requests = -1;
  kbc_embedder_counts(e, &requests, NULL);
  KBC_CHECK_EQ_INT(requests, 2);

  kbc_arena_free(a);
  kbc_embedder_stop(e);
  check_no_zombie("query_cache_null_cache_embeds_every_time");
  kbc_test_rmrf(dir);
}

/* An embedder with no model cannot key a cache entry, and asking for one is an
 * error rather than a query silently stored under a model nobody chose.
 *
 * The only way to be in that state is a handshake that failed, so this walks
 * a live embedder into it with a marker rather than starting one there. */
KBC_TEST(query_cache_refuses_to_cache_without_a_known_model) {
  char *dir = case_dir("qc-query-unknown");
  if (dir == NULL) {
    return;
  }
  char script[KBC_TEST_PATH_MAX];
  char marker[KBC_TEST_PATH_MAX];
  KBC_CHECK_NOT_NULL(join_path(script, sizeof script, dir, "flip.sh"));
  KBC_CHECK_NOT_NULL(join_path(marker, sizeof marker, dir, "broken"));
  static char body[8192];
  sbuf b = {body, sizeof body, 0};
  sb_add(&b, "M='");
  sb_add(&b, marker);
  sb_add(&b, "'\n");
  sb_add(&b, "while IFS= read -r line; do\n");
  sb_add(&b, "  case \"$line\" in\n");
  sb_add(&b, "  *health*)\n");
  sb_add(&b, "    if [ -f \"$M\" ]; then printf '%s\\n' 'not json at all';\n");
  sb_add(&b, "    else printf '%s\\n' '{\"ok\":true,\"dim\":3,"
             "\"model\":\"known-model\"}'; fi ;;\n");
  sb_add(&b, "  *embed*) printf '%s\\n' '{\"ok\":true,\"dim\":3,\"vectors\":"
             "[[1.5,-2.25,3]]}' ;;\n");
  sb_add(&b, "  esac\n");
  sb_add(&b, "done\n");
  make_fake(script, body);
  const char *argv[2] = {script, NULL};
  kbc_err err;
  kbc_err_reset(&err);
  kbc_embedder *e = kbc_embedder_start(argv, &err);
  KBC_CHECK_NOT_NULL(e);
  kbc_err_reset(&err);
  kbc_query_cache *c = kbc_query_cache_new(4, &err);
  kbc_arena *a = kbc_arena_new(256);
  KBC_CHECK_NOT_NULL(c);
  KBC_CHECK_NOT_NULL(a);
  if (e == NULL || c == NULL || a == NULL) {
    kbc_embedder_stop(e);
    kbc_query_cache_free(c);
    kbc_arena_free(a);
    kbc_test_rmrf(dir);
    return;
  }
  /* Sanity: with a model, the same query caches. */
  kbc_query_outcome warm;
  kbc_err_reset(&err);
  KBC_CHECK_OK(kbc_embed_query(e, c, a, "a query", &warm, &err));
  KBC_CHECK_MSG(!warm.cache_hit, "the first query was a hit");
  KBC_CHECK_EQ_INT(kbc_query_cache_len(c), 1);

  /* Break the sidecar, so the restart fails its handshake and the model goes
   * unknown. The cache still holds the old entry; the query lane must refuse
   * rather than serve it or store a new vector under an empty model. */
  kbc_test_write_file(marker, "broken now\n");
  kbc_err_reset(&err);
  KBC_CHECK_ERR(kbc_embedder_restart(e, &err), KBC_ERR_PARSE);
  kbc_query_outcome o;
  memset(&o, 0xff, sizeof o);
  kbc_err_reset(&err);
  KBC_CHECK_ERR(kbc_embed_query(e, c, a, "a query", &o, &err), KBC_ERR_NOTFOUND);
  KBC_CHECK_ERR_MSG(err);
  KBC_CHECK_MSG(o.vec == NULL, "a refused query handed back a vector");
  KBC_CHECK_MSG(!o.cache_hit, "a refused query reported a cache hit");
  KBC_CHECK_MSG(kbc_query_cache_len(c) == 1,
                "a refused query changed the cache to %zu entries",
                kbc_query_cache_len(c));

  kbc_arena_free(a);
  kbc_query_cache_free(c);
  kbc_embedder_stop(e);
  check_no_zombie("query_cache_refuses_to_cache_without_a_known_model");
  kbc_test_rmrf(dir);
}

/* Four threads hammering the same eight keys: every get must see a whole
 * vector, never a mix of two keys' patterns, and the ceiling must hold
 * throughout. Under TSan this is also the race detector for the
 * scan-and-mutate lock. */
static void *cache_worker(void *arg) {
  kbc_query_cache *c = arg;
  kbc_arena *a = kbc_arena_new(4096);
  if (a == NULL) {
    return NULL;
  }
  for (int i = 0; i < 400; i++) {
    for (int k = 0; k < 8; k++) {
      /* Built by hand, not snprintf: the loop bound makes the width obvious
       * and the build runs -Werror=format-truncation, which cannot see the
       * bound through the int. "k0".."k7" is three bytes in a four-byte slot. */
      char key[4] = {'k', (char)('0' + k), '\0', '\0'};
      const float *got = NULL;
      size_t dim = 0;
      if (!kbc_query_cache_get(c, "m", key, strlen(key), a, &got, &dim)) {
        float v[3] = {(float)(k + 1), (float)(k + 1), (float)(k + 1)};
        kbc_err scratch;
        kbc_err_reset(&scratch);
        (void)kbc_query_cache_put(c, "m", key, strlen(key), v, 3, &scratch);
      }
    }
    if (kbc_query_cache_len(c) > 64) {
      break; /* the ceiling was breached; the assertion below reports it */
    }
  }
  kbc_arena_free(a);
  return NULL;
}

KBC_TEST(query_cache_is_thread_safe) {
  kbc_err err;
  kbc_err_reset(&err);
  kbc_query_cache *c = kbc_query_cache_new(64, &err);
  KBC_CHECK_NOT_NULL(c);
  if (c == NULL) {
    return;
  }
  pthread_t th[4];
  for (int t = 0; t < 4; t++) {
    KBC_CHECK_EQ_INT(pthread_create(&th[t], NULL, cache_worker, c), 0);
  }
  for (int t = 0; t < 4; t++) {
    KBC_CHECK_EQ_INT(pthread_join(th[t], NULL), 0);
  }
  KBC_CHECK_MSG(kbc_query_cache_len(c) == 8,
                "after concurrent access the cache holds %zu entries, want 8",
                kbc_query_cache_len(c));
  kbc_arena *a = kbc_arena_new(1024);
  KBC_CHECK_NOT_NULL(a);
  const float *got = NULL;
  size_t dim = 0;
  for (int k = 0; k < 8; k++) {
    char key[8];
    KBC_CHECK_EQ_INT(snprintf(key, sizeof key, "k%d", k), 2);
    KBC_CHECK_MSG(kbc_query_cache_get(c, "m", key, strlen(key), a, &got, &dim),
                  "key %s is missing after concurrent access", key);
    if (got != NULL) {
      /* A torn read shows up as a mix of two keys' patterns. */
      float want = (float)(k + 1);
      KBC_CHECK_MSG(got[0] == want && got[1] == want && got[2] == want,
                    "key %s came back torn: %g %g %g", key, (double)got[0],
                    (double)got[1], (double)got[2]);
    }
  }
  KBC_CHECK_MSG(kbc_query_cache_len(c) <= 64, "the ceiling was exceeded");

  kbc_arena_free(a);
  kbc_query_cache_free(c);
}

int main(void) {
  kbc_test_tmpdir(g_tmp, sizeof g_tmp);
  int rc = kbc_test_run(
      "embed",
      (kbc_test_case[]){{"vecstore_roundtrip", vecstore_roundtrip},
                       {"vecstore_hostile_files", vecstore_hostile_files},
                       {"vecstore_growth", vecstore_growth},
                       {"embedder_health_and_embed", embedder_health_and_embed},
                       {"embedder_short_vectors", embedder_short_vectors},
                       {"embedder_wrong_vector_length",
                        embedder_wrong_vector_length},
                       {"embedder_not_ok", embedder_not_ok},
                       {"embedder_non_object_reply", embedder_non_object_reply},
                       {"embedder_child_death", embedder_child_death},
                       {"embedder_restart_recovers", embedder_restart_recovers},
                       {"embedder_silent_on_embed", embedder_silent_on_embed},
                       {"embedder_start_rejects_bad_argv",
                        embedder_start_rejects_bad_argv},
                       {"embedder_does_not_lose_a_line_that_shared_a_read",
                        embedder_does_not_lose_a_line_that_shared_a_read},
                       {"embedder_handshake_survives_a_burst_on_the_first_read",
                        embedder_handshake_survives_a_burst_on_the_first_read},
                       {"embedder_restart_discards_the_previous_childs_residual",
                        embedder_restart_discards_the_previous_childs_residual},
                       {"embedder_start_handshakes_and_knows_the_model",
                        embedder_start_handshakes_and_knows_the_model},
                       {"embedder_start_degrades_when_no_handshake",
                        embedder_start_degrades_when_no_handshake},
                       {"embedder_start_stop_does_not_leak_descriptors",
                        embedder_start_stop_does_not_leak_descriptors},
                       {"query_cache_two_models_same_dim_do_not_share",
                        query_cache_two_models_same_dim_do_not_share},
                       {"query_cache_hit_skips_sidecar_and_reports_zero_ms",
                        query_cache_hit_skips_sidecar_and_reports_zero_ms},
                       {"query_cache_get_touches_lru_order",
                        query_cache_get_touches_lru_order},
                       {"query_cache_capacity_ceiling_and_reput",
                        query_cache_capacity_ceiling_and_reput},
                       {"query_cache_key_is_the_raw_query",
                        query_cache_key_is_the_raw_query},
                       {"query_cache_hit_vector_is_an_independent_copy",
                        query_cache_hit_vector_is_an_independent_copy},
                       {"query_cache_rejects_bad_arguments",
                        query_cache_rejects_bad_arguments},
                       {"query_cache_rejects_empty_query",
                        query_cache_rejects_empty_query},
                       {"query_cache_works_without_an_announced_model",
                        query_cache_works_without_an_announced_model},
                       {"query_cache_is_thread_safe",
                        query_cache_is_thread_safe},
                       {"embedder_model_is_notfound_before_a_handshake",
                        embedder_model_is_notfound_before_a_handshake},
                       {"embedder_model_refuses_a_short_buffer",
                        embedder_model_refuses_a_short_buffer},
                       {"query_cache_null_cache_embeds_every_time",
                        query_cache_null_cache_embeds_every_time},
                       {"query_cache_refuses_to_cache_without_a_known_model",
                        query_cache_refuses_to_cache_without_a_known_model},
                       {"query_cache_reput_does_not_grow",
                        query_cache_reput_does_not_grow},
                       {"query_cache_embed_ms_is_zero_only_on_a_hit",
                        query_cache_embed_ms_is_zero_only_on_a_hit},
                       {NULL, NULL}});
  kbc_test_rmrf(g_tmp);
  return rc;
}
