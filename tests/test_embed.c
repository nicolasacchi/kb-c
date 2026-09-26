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
  KBC_CHECK_MSG(kbc_embedder_healthy(e), "a spawned sidecar is not healthy");
  /* No handshake has run yet, so the dimension is not known. */
  KBC_CHECK_EQ_INT(kbc_embedder_dim(e), 0);

  /* restart() runs the health handshake; the dim comes from it. */
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
  KBC_CHECK_ERR(kbc_embedder_embed(e, a, texts, 1, 0, &out, &err), KBC_ERR_IO);
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
  KBC_CHECK_EQ_INT(kbc_embedder_dim(e), 0);

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
                       {NULL, NULL}});
  kbc_test_rmrf(g_tmp);
  return rc;
}
