/* test_mem.c — src/mem.c and src/kbc.c: arenas, growable strings, string
 * lists, file I/O, base64/hex, UTF-8 and folding, and the error carrier.
 *
 * The interesting properties are the ones a caller can break silently:
 * alignment and accounting in the arena, the NUL at ptr[len] of a kbc_str,
 * strictness of the decoders, and the exact byte offsets utf8_validate
 * reports. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "kbc/mem.h"
#include "kbc_test.h"

static char g_tmp[KBC_TEST_PATH_MAX];

/* Manual join: the tmpdir is bounded, but -Wformat-truncation cannot see
 * that the result still fits, and the test wants the whole path. */
static void tmp_path(char *buf, size_t cap, const char *name) {
  size_t dl = strlen(g_tmp);
  size_t nl = strlen(name);
  KBC_CHECK_MSG(dl + 1u + nl + 1u <= cap, "tmp path does not fit");
  memcpy(buf, g_tmp, dl);
  buf[dl] = '/';
  memcpy(buf + dl + 1u, name, nl + 1u);
}

/* ================================ arena ================================= */

KBC_TEST(arena_hands_back_every_allocation) {
  kbc_arena *a = kbc_arena_new(64);
  KBC_CHECK_NOT_NULL(a);

  unsigned char *p = kbc_arena_calloc(a, 16, 4);
  KBC_CHECK_NOT_NULL(p);
  for (size_t i = 0; i < 64; i++) {
    if (p[i] != 0) {
      kbc_test_fail(__FILE__, __LINE__, "calloc byte %zu is %u, want 0", i,
                    p[i]);
      break;
    }
  }

  char *d = kbc_arena_strdup(a, "hello");
  KBC_CHECK_NOT_NULL(d);
  KBC_CHECK_EQ_STR(d, "hello");

  /* strndup stops at n even when the source is longer, and keeps embedded
   * NULs when n says so. */
  char *n3 = kbc_arena_strndup(a, "hello", 3);
  KBC_CHECK_EQ_STR(n3, "hel");
  const char with_nul[] = {'a', '\0', 'b'};
  char *ne = kbc_arena_strndup(a, with_nul, 3);
  KBC_CHECK_NOT_NULL(ne);
  KBC_CHECK(memcmp(ne, with_nul, 3) == 0);
  KBC_CHECK_EQ_INT(ne[3], 0);

  char *pf = kbc_arena_printf(a, "%s-%d-%s", "x", 42, "y");
  KBC_CHECK_EQ_STR(pf, "x-42-y");
  KBC_CHECK_NOT_NULL(kbc_arena_strdup(a, ""));

  /* Everything above is distinct memory, not one aliased slot. */
  KBC_CHECK((void *)p != (void *)d);
  KBC_CHECK((void *)d != (void *)n3);
  KBC_CHECK((void *)n3 != (void *)pf);

  kbc_arena_free(a);
}

KBC_TEST(arena_allocations_are_8_byte_aligned) {
  kbc_arena *a = kbc_arena_new(4096);
  KBC_CHECK_NOT_NULL(a);
  /* Sizes 1..9 exercise the alignment padding between consecutive
   * allocations as well as each size itself. */
  for (size_t n = 1; n <= 9; n++) {
    void *p = kbc_arena_alloc(a, n);
    KBC_CHECK_NOT_NULL(p);
    KBC_CHECK_MSG(((uintptr_t)p % 8u) == 0,
                  "alloc(%zu) returned %p, not 8-byte aligned", n, p);
    memset(p, 0xA5, n); /* must not disturb the neighbours */
  }
  kbc_arena_free(a);
}

KBC_TEST(arena_zero_byte_allocations_are_distinct) {
  kbc_arena *a = kbc_arena_new(64);
  void *p1 = kbc_arena_alloc(a, 0);
  void *p2 = kbc_arena_alloc(a, 0);
  void *p3 = kbc_arena_calloc(a, 0, 0);
  KBC_CHECK_NOT_NULL(p1);
  KBC_CHECK_NOT_NULL(p2);
  KBC_CHECK_NOT_NULL(p3);
  /* Callers use these as identity keys, so they must never collide. */
  KBC_CHECK(p1 != p2);
  KBC_CHECK(p1 != p3);
  KBC_CHECK(p2 != p3);
  kbc_arena_free(a);
}

KBC_TEST(arena_bytes_counts_padded_hand_outs) {
  kbc_arena *a = kbc_arena_new(4096);
  KBC_CHECK_EQ_INT(kbc_arena_bytes(a), 0);

  KBC_CHECK_NOT_NULL(kbc_arena_alloc(a, 1));
  /* Nothing is padded ahead of the first hand-out in a block. */
  KBC_CHECK_EQ_INT(kbc_arena_bytes(a), 1);

  KBC_CHECK_NOT_NULL(kbc_arena_alloc(a, 9));
  KBC_CHECK_EQ_INT(kbc_arena_bytes(a), 8 + 9);

  /* A request bigger than the first block gets its own block, and the total
   * spans every block. */
  KBC_CHECK_NOT_NULL(kbc_arena_alloc(a, 20000));
  KBC_CHECK_EQ_INT(kbc_arena_bytes(a), 8 + 9 + 20000);

  kbc_arena_free(a);
}

KBC_TEST(arena_reset_drops_later_blocks_and_keeps_the_first) {
  kbc_arena *a = kbc_arena_new(64);
  unsigned char *first = kbc_arena_alloc(a, 32);
  KBC_CHECK_NOT_NULL(first);
  memset(first, 0xFF, 32);
  KBC_CHECK_NOT_NULL(kbc_arena_alloc(a, 100000)); /* forces a second block */
  KBC_CHECK(kbc_arena_bytes(a) > 32);

  kbc_arena_reset(a);
  /* Everything after the first block is gone, so nothing it handed out can
   * still be reachable. */
  KBC_CHECK_EQ_INT(kbc_arena_bytes(a), 0);

  /* The first block survives: the same address comes back, writable. */
  unsigned char *again = kbc_arena_alloc(a, 32);
  KBC_CHECK_MSG(again == first, "reset did not keep the first block: %p != %p",
                (void *)again, (void *)first);
  memset(again, 0x5A, 32);
  KBC_CHECK_EQ_INT(first[0], 0x5A);
  kbc_arena_free(a);
}

KBC_TEST(arena_calloc_zeroes_recycled_memory) {
  kbc_arena *a = kbc_arena_new(64);
  unsigned char *dirty = kbc_arena_calloc(a, 16, 4);
  KBC_CHECK_NOT_NULL(dirty);
  memset(dirty, 0xCC, 64);
  kbc_arena_reset(a);

  /* Recycled first block, now full of 0xCC: calloc must zero it again
   * rather than hand back the stale pattern. */
  unsigned char *fresh = kbc_arena_calloc(a, 16, 4);
  KBC_CHECK_EQ_INT(fresh[0], 0);
  KBC_CHECK_EQ_INT(fresh[31], 0);
  KBC_CHECK_EQ_INT(fresh[63], 0);
  kbc_arena_free(a);
}

KBC_TEST(arena_rejects_null_and_overflowing_requests) {
  KBC_CHECK_NULL(kbc_arena_alloc(NULL, 8));
  KBC_CHECK_NULL(kbc_arena_strdup(NULL, "x"));
  KBC_CHECK_EQ_INT(kbc_arena_bytes(NULL), 0);
  kbc_arena_free(NULL); /* must not crash */

  kbc_arena *a = kbc_arena_new(64);
  KBC_CHECK_NULL(kbc_arena_strdup(a, NULL));
  KBC_CHECK_NULL(kbc_arena_printf(a, NULL));
  KBC_CHECK_NULL(kbc_arena_strndup(a, NULL, 3));
  /* count * size must not wrap: SIZE_MAX * 2 would truncate to a small
   * allocation and hand back a pointer that looks valid. */
  KBC_CHECK_NULL(kbc_arena_calloc(a, SIZE_MAX, 2));
  KBC_CHECK_NULL(kbc_arena_strndup(a, "x", SIZE_MAX));
  kbc_arena_free(a);
}

/* ================================ kbc_str =============================== */

KBC_TEST(str_appends_keep_len_and_nul_in_step) {
  kbc_str s;
  kbc_str_init(&s);
  KBC_CHECK_EQ_INT(s.len, 0);
  KBC_CHECK_NULL(s.ptr);

  KBC_CHECK_OK(kbc_str_puts(&s, "abc"));
  KBC_CHECK_OK(kbc_str_putc(&s, 'd'));
  KBC_CHECK_OK(kbc_str_append(&s, "ef", 2));
  KBC_CHECK_EQ_INT(s.len, 6);
  KBC_CHECK_EQ_STR(s.ptr, "abcdef");
  /* The terminator follows len, not the capacity. */
  KBC_CHECK_EQ_INT(s.ptr[s.len], 0);
  KBC_CHECK(s.cap > s.len);

  KBC_CHECK_OK(kbc_str_printf(&s, "|%d|%s|", 7, "g"));
  KBC_CHECK_EQ_INT(s.len, 11);
  KBC_CHECK_EQ_STR(s.ptr, "abcdef|7|g|");
  KBC_CHECK_EQ_INT(s.ptr[s.len], 0);
  kbc_str_free(&s);
}

KBC_TEST(str_append_preserves_embedded_nuls) {
  kbc_str s;
  kbc_str_init(&s);
  const char raw[] = {'a', '\0', 'b', '\0', 'c'};
  KBC_CHECK_OK(kbc_str_append(&s, raw, sizeof raw));
  KBC_CHECK_EQ_INT(s.len, 5);
  KBC_CHECK(memcmp(s.ptr, raw, sizeof raw) == 0);
  KBC_CHECK_EQ_INT(s.ptr[5], 0);

  /* Appending after an embedded NUL must not stop at it. */
  KBC_CHECK_OK(kbc_str_puts(&s, "Z"));
  KBC_CHECK_EQ_INT(s.len, 6);
  KBC_CHECK_EQ_INT(s.ptr[5], 'Z');
  kbc_str_free(&s);
}

KBC_TEST(str_rejects_null_data_but_allows_null_zero_len) {
  kbc_str s;
  kbc_str_init(&s);
  KBC_CHECK_OK(kbc_str_append(&s, NULL, 0)); /* no-op, still fine */
  KBC_CHECK_EQ_INT(s.len, 0);
  KBC_CHECK_ERR(kbc_str_append(&s, NULL, 3), KBC_ERR_INVALID);
  KBC_CHECK_ERR(kbc_str_puts(&s, NULL), KBC_ERR_INVALID);
  KBC_CHECK_ERR(kbc_str_printf(&s, NULL), KBC_ERR_INVALID);
  KBC_CHECK_ERR(kbc_str_append(NULL, "x", 1), KBC_ERR_INVALID);
  /* None of the rejected calls may have appended a byte. */
  KBC_CHECK_EQ_INT(s.len, 0);
  kbc_str_free(&s);
}

KBC_TEST(str_clear_keeps_capacity) {
  kbc_str s;
  kbc_str_init(&s);
  KBC_CHECK_OK(kbc_str_append(&s, "0123456789", 10));
  size_t cap = s.cap;
  kbc_str_clear(&s);
  KBC_CHECK_EQ_INT(s.len, 0);
  KBC_CHECK_EQ_INT(s.cap, cap);
  KBC_CHECK_NOT_NULL(s.ptr);
  KBC_CHECK_EQ_INT(s.ptr[0], 0);

  /* The kept buffer is still usable and terminates at the new len. */
  KBC_CHECK_OK(kbc_str_puts(&s, "ab"));
  KBC_CHECK_EQ_STR(s.ptr, "ab");
  KBC_CHECK_EQ_INT(s.ptr[s.len], 0);
  kbc_str_free(&s);
  KBC_CHECK_NULL(s.ptr);
  KBC_CHECK_EQ_INT(s.cap, 0);
}

KBC_TEST(str_reserve_prevents_growth_within_the_reserved_span) {
  kbc_str s;
  kbc_str_init(&s);
  KBC_CHECK(kbc_str_reserve(&s, 100));
  KBC_CHECK_NOT_NULL(s.ptr);
  KBC_CHECK(s.cap >= 101); /* room for 100 bytes plus the NUL */
  size_t cap = s.cap;

  char buf[100];
  memset(buf, 'z', sizeof buf);
  KBC_CHECK_OK(kbc_str_append(&s, buf, sizeof buf));
  KBC_CHECK_EQ_INT(s.len, 100);
  /* Exactly the reserved span: the append must not have reallocated. */
  KBC_CHECK_EQ_INT(s.cap, cap);
  KBC_CHECK_EQ_INT(s.ptr[100], 0);
  KBC_CHECK(!kbc_str_reserve(NULL, 1));
  kbc_str_free(&s);
}

KBC_TEST(str_json_escapes_quotes_slashes_controls_and_del) {
  kbc_str s;
  kbc_str_init(&s);
  KBC_CHECK_OK(kbc_str_append_json_string(&s, "a\"b\\c", 5));
  KBC_CHECK_EQ_STR(s.ptr, "\"a\\\"b\\\\c\"");

  kbc_str_clear(&s);
  const char ctrl[] = {'a', 0x01, 0x7f};
  KBC_CHECK_OK(kbc_str_append_json_string(&s, ctrl, sizeof ctrl));
  KBC_CHECK_EQ_STR(s.ptr, "\"a\\u0001\\u007f\"");

  kbc_str_clear(&s);
  KBC_CHECK_OK(kbc_str_append_json_string(&s, "", 0));
  KBC_CHECK_EQ_STR(s.ptr, "\"\"");
  kbc_str_free(&s);
}

KBC_TEST(str_json_passes_valid_utf8_through_untouched) {
  kbc_str s;
  kbc_str_init(&s);
  const char text[] = "Caf\xc3\xa9 \xf0\x9f\x98\x80";
  size_t n = sizeof text - 1;
  KBC_CHECK_OK(kbc_str_append_json_string(&s, text, n));
  /* Quotes on the outside, the UTF-8 bytes untouched on the inside. */
  KBC_CHECK_EQ_INT(s.len, n + 2);
  KBC_CHECK_EQ_INT(s.ptr[0], '"');
  KBC_CHECK_EQ_INT(s.ptr[n + 1], '"');
  KBC_CHECK(memcmp(s.ptr + 1, text, n) == 0);

  /* A malformed byte is escaped so the result stays parseable. */
  kbc_str_clear(&s);
  const char bad[] = {'\xc3'}; /* truncated two-byte sequence */
  KBC_CHECK_OK(kbc_str_append_json_string(&s, bad, 1));
  KBC_CHECK_EQ_STR(s.ptr, "\"\\u00c3\"");
  kbc_str_free(&s);
}

KBC_TEST(str_json_appends_after_existing_content) {
  kbc_str s;
  kbc_str_init(&s);
  KBC_CHECK_OK(kbc_str_puts(&s, "key="));
  KBC_CHECK_OK(kbc_str_append_json_string(&s, "v", 1));
  KBC_CHECK_EQ_STR(s.ptr, "key=\"v\"");
  KBC_CHECK_ERR(kbc_str_append_json_string(&s, NULL, 2), KBC_ERR_INVALID);
  KBC_CHECK_ERR(kbc_str_append_json_string(NULL, "v", 1), KBC_ERR_INVALID);
  kbc_str_free(&s);
}

/* ============================== kbc_strlist ============================= */

KBC_TEST(strlist_push_copies_its_argument) {
  kbc_strlist l;
  kbc_strlist_init(&l);
  char *heap = strdup("borrowed");
  KBC_CHECK_NOT_NULL(heap);
  KBC_CHECK_OK(kbc_strlist_push(&l, heap));
  /* The caller's buffer goes away immediately; the list must be immune. */
  free(heap);
  KBC_CHECK_EQ_INT(l.len, 1);
  KBC_CHECK(kbc_strlist_contains(&l, "borrowed"));

  KBC_CHECK_ERR(kbc_strlist_push(&l, NULL), KBC_ERR_INVALID);
  KBC_CHECK_ERR(kbc_strlist_push(NULL, "x"), KBC_ERR_INVALID);
  KBC_CHECK_EQ_INT(l.len, 1);
  kbc_strlist_free(&l);
}

KBC_TEST(strlist_push_owned_takes_the_pointer_and_frees_it_once) {
  kbc_strlist l;
  kbc_strlist_init(&l);
  char *owned = strdup("owned");
  KBC_CHECK_OK(kbc_strlist_push_owned(&l, owned));
  /* No copy: the list adopts the caller's allocation, so it must be the very
   * same pointer. Freeing the list below must free it exactly once. */
  KBC_CHECK(l.items[0] == owned);
  KBC_CHECK_EQ_INT(l.len, 1);

  KBC_CHECK_ERR(kbc_strlist_push_owned(&l, NULL), KBC_ERR_INVALID);
  KBC_CHECK_EQ_INT(l.len, 1);
  kbc_strlist_free(&l);
  KBC_CHECK_EQ_INT(l.len, 0);
  KBC_CHECK_NULL(l.items);
}

KBC_TEST(strlist_contains_and_sort_track_the_items) {
  kbc_strlist l;
  kbc_strlist_init(&l);
  KBC_CHECK(!kbc_strlist_contains(&l, "a")); /* empty */
  KBC_CHECK(!kbc_strlist_contains(NULL, "a"));

  /* More pushes than the initial capacity, to cross the growth path. */
  const char *words[] = {"pear", "apple", "fig", "date", "cherry", "apple"};
  for (size_t i = 0; i < sizeof words / sizeof words[0]; i++) {
    KBC_CHECK_OK(kbc_strlist_push(&l, words[i]));
  }
  KBC_CHECK_EQ_INT(l.len, 6);
  KBC_CHECK(kbc_strlist_contains(&l, "fig"));
  KBC_CHECK(kbc_strlist_contains(&l, "apple")); /* the duplicate, too */
  KBC_CHECK(!kbc_strlist_contains(&l, "kiwi"));

  kbc_strlist_sort(&l);
  KBC_CHECK_EQ_INT(l.len, 6);
  KBC_CHECK_EQ_STR(l.items[0], "apple");
  KBC_CHECK_EQ_STR(l.items[1], "apple");
  KBC_CHECK_EQ_STR(l.items[2], "cherry");
  KBC_CHECK_EQ_STR(l.items[3], "date");
  KBC_CHECK_EQ_STR(l.items[4], "fig");
  KBC_CHECK_EQ_STR(l.items[5], "pear");

  kbc_strlist_sort(&l); /* idempotent */
  KBC_CHECK_EQ_STR(l.items[5], "pear");
  kbc_strlist_sort(NULL);
  kbc_strlist_free(&l);
  kbc_strlist_free(NULL);
}

/* ================================ files ================================= */

KBC_TEST(read_file_reports_a_missing_path_by_name) {
  char path[KBC_TEST_PATH_MAX];
  tmp_path(path, sizeof path, "does-not-exist");
  kbc_str out;
  kbc_str_init(&out);
  kbc_err err;
  kbc_err_reset(&err);

  kbc_status st = kbc_str_read_file(path, &out, &err);
  KBC_CHECK_ERR(st, KBC_ERR_IO);
  KBC_CHECK_EQ_INT(err.status, KBC_ERR_IO);
  KBC_CHECK_ERR_MSG(err);
  KBC_CHECK_MSG(strstr(err.msg, path) != NULL, "message %s omits the path",
                err.msg);
  KBC_CHECK_MSG(strstr(err.msg, "open") != NULL, "message %s omits the op",
                err.msg);
  KBC_CHECK_EQ_INT(out.len, 0); /* a failed read appends nothing */

  KBC_CHECK_ERR(kbc_str_read_file(NULL, &out, &err), KBC_ERR_INVALID);
  KBC_CHECK_ERR(kbc_str_read_file(path, NULL, &err), KBC_ERR_INVALID);
  kbc_str_free(&out);
}

KBC_TEST(read_file_refuses_a_directory) {
  kbc_str out;
  kbc_str_init(&out);
  kbc_err err;
  kbc_err_reset(&err);
  kbc_status st = kbc_str_read_file(g_tmp, &out, &err);
  KBC_CHECK_ERR(st, KBC_ERR_INVALID);
  KBC_CHECK_ERR_MSG(err);
  KBC_CHECK(strstr(err.msg, g_tmp) != NULL);
  KBC_CHECK_EQ_INT(out.len, 0);
  kbc_str_free(&out);
}

KBC_TEST(write_then_read_round_trips_bytes_exactly) {
  char path[KBC_TEST_PATH_MAX];
  tmp_path(path, sizeof path, "round.bin");

  /* Larger than the 64 KiB read buffer, with NULs at both ends and exactly
   * on the buffer boundary. */
  enum { N = 200000 };
  char *payload = malloc(N);
  KBC_CHECK_NOT_NULL(payload);
  for (int i = 0; i < N; i++) {
    payload[i] = (char)((i * 31 + 7) & 0xff);
  }
  payload[0] = '\0';
  payload[1000] = '\0';
  payload[65536] = '\0';
  payload[N - 1] = '\0';

  kbc_err err;
  kbc_err_reset(&err);
  KBC_CHECK_OK(kbc_str_write_file_atomic(path, payload, N, &err));
  KBC_CHECK_EQ_INT(err.msg[0], 0); /* success leaves no stale message */
  KBC_CHECK(kbc_path_exists(path));

  kbc_str out;
  kbc_str_init(&out);
  kbc_err_reset(&err);
  KBC_CHECK_OK(kbc_str_read_file(path, &out, &err));
  KBC_CHECK_EQ_INT(out.len, N);
  KBC_CHECK_MSG(memcmp(out.ptr, payload, N) == 0, "round trip differs");
  kbc_str_free(&out);

  /* An overwrite replaces the contents outright. */
  KBC_CHECK_OK(kbc_str_write_file_atomic(path, "ab", 2, &err));
  kbc_str_init(&out);
  KBC_CHECK_OK(kbc_str_read_file(path, &out, &err));
  KBC_CHECK_EQ_INT(out.len, 2);
  KBC_CHECK_EQ_STR(out.ptr, "ab");
  kbc_str_free(&out);

  KBC_CHECK_ERR(kbc_str_write_file_atomic(NULL, "x", 1, &err),
                KBC_ERR_INVALID);
  KBC_CHECK_ERR(kbc_str_write_file_atomic(path, NULL, 4, &err),
                KBC_ERR_INVALID);
  free(payload);
}

KBC_TEST(failed_atomic_write_leaves_no_temp_file_behind) {
  char sub[KBC_TEST_PATH_MAX];
  char inner[KBC_TEST_PATH_MAX];
  tmp_path(sub, sizeof sub, "target");
  memcpy(inner, sub, strlen(sub) + 1u);
  strcat(inner, "/inner");
  kbc_test_mkdir_p(inner);

  /* The destination is a directory, so the final rename cannot succeed. */
  kbc_err err;
  kbc_err_reset(&err);
  kbc_status st = kbc_str_write_file_atomic(sub, "payload", 7, &err);
  KBC_CHECK_ERR(st, KBC_ERR_IO);
  KBC_CHECK_ERR_MSG(err);

  /* No "<name>.tmp.<pid>" may survive the failure. */
  DIR *d = opendir(sub);
  KBC_CHECK_NOT_NULL(d);
  if (d != NULL) {
    struct dirent *de;
    while ((de = readdir(d)) != NULL) {
      KBC_CHECK_MSG(strncmp(de->d_name, "target.tmp.", 10) != 0,
                    "temp file %s left behind", de->d_name);
    }
    closedir(d);
  }
}

/* =============================== encodings ============================== */

KBC_TEST(base64_encode_pads_by_remainder) {
  kbc_str s;
  kbc_str_init(&s);
  KBC_CHECK_OK(kbc_base64_encode(&s, "f", 1));
  KBC_CHECK_EQ_STR(s.ptr, "Zg==");
  KBC_CHECK_OK(kbc_base64_encode(&s, "fo", 2));
  KBC_CHECK_EQ_STR(s.ptr, "Zg==Zm8=");
  kbc_str_clear(&s);
  KBC_CHECK_OK(kbc_base64_encode(&s, "foo", 3));
  KBC_CHECK_EQ_STR(s.ptr, "Zm9v");
  kbc_str_clear(&s);
  KBC_CHECK_OK(kbc_base64_encode(&s, "foob", 4));
  KBC_CHECK_EQ_STR(s.ptr, "Zm9vYg==");
  kbc_str_clear(&s);
  KBC_CHECK_OK(kbc_base64_encode(&s, "fooba", 5));
  KBC_CHECK_EQ_STR(s.ptr, "Zm9vYmE=");
  /* All the 6-bit values, including the +/- alphabet tail. */
  kbc_str_clear(&s);
  const unsigned char all[] = {0xfb, 0xff, 0xbf};
  KBC_CHECK_OK(kbc_base64_encode(&s, all, sizeof all));
  KBC_CHECK_EQ_STR(s.ptr, "+/+/");
  kbc_str_free(&s);
}

KBC_TEST(base64_round_trips_every_remainder) {
  static const size_t lens[] = {0, 1, 2, 3, 4, 5};
  for (size_t li = 0; li < sizeof lens / sizeof lens[0]; li++) {
    size_t n = lens[li];
    char data[8];
    for (size_t i = 0; i < n; i++) {
      data[i] = (char)(0xA0 + i); /* high bit set: no NUL surprises */
    }

    kbc_str enc;
    kbc_str_init(&enc);
    KBC_CHECK_OK(kbc_base64_encode(&enc, n ? data : NULL, n));
    if (n == 0) {
      KBC_CHECK_EQ_INT(enc.len, 0);
    } else {
      KBC_CHECK_EQ_INT(enc.len, ((n + 2) / 3) * 4);
    }

    kbc_arena *a = kbc_arena_new(64);
    const char *dec = NULL;
    size_t dec_len = 0;
    kbc_err err;
    kbc_err_reset(&err);
    /* An empty encode leaves ptr NULL, so feed the decoder a real empty
     * string; a NULL input is rejected before length is even looked at. */
    KBC_CHECK_OK(
        kbc_base64_decode(a, enc.ptr != NULL ? enc.ptr : "", enc.len, &dec,
                          &dec_len, &err));
    KBC_CHECK_MSG(dec_len == n, "len %zu round-tripped to %zu", n, dec_len);
    KBC_CHECK_NOT_NULL(dec);
    KBC_CHECK(n == 0 || memcmp(dec, data, n) == 0);

    /* A rejected decode must not hand back a buffer. */
    dec = (const char *)"sentinel";
    dec_len = 99;
    kbc_err_reset(&err);
    KBC_CHECK_ERR(kbc_base64_decode(a, "!!!!", 4, &dec, &dec_len, &err),
                  KBC_ERR_INVALID);
    KBC_CHECK_NULL(dec);
    KBC_CHECK_EQ_INT(dec_len, 0);
    KBC_CHECK_ERR_MSG(err);

    kbc_arena_free(a);
    kbc_str_free(&enc);
  }
}

KBC_TEST(base64_decode_is_strict) {
  kbc_arena *a = kbc_arena_new(64);
  const char *out = NULL;
  size_t out_len = 0;
  kbc_err err;

  /* Alphabet: '_' is the URL-safe variant, not the standard one. */
  kbc_err_reset(&err);
  KBC_CHECK_ERR(kbc_base64_decode(a, "Zm9v_Zm9v", 8, &out, &out_len, &err),
                KBC_ERR_INVALID);
  KBC_CHECK_ERR_MSG(err);
  /* Same length, wrong alphabet. */
  kbc_err_reset(&err);
  KBC_CHECK_ERR(kbc_base64_decode(a, "Zm9*", 4, &out, &out_len, &err),
                KBC_ERR_INVALID);
  /* Length not a multiple of 4. */
  kbc_err_reset(&err);
  KBC_CHECK_ERR(kbc_base64_decode(a, "Zm9vY", 5, &out, &out_len, &err),
                KBC_ERR_INVALID);
  /* Padding in the first slot, and more than two pad bytes. */
  kbc_err_reset(&err);
  KBC_CHECK_ERR(kbc_base64_decode(a, "=m9v", 4, &out, &out_len, &err),
                KBC_ERR_INVALID);
  kbc_err_reset(&err);
  KBC_CHECK_ERR(kbc_base64_decode(a, "A===", 4, &out, &out_len, &err),
                KBC_ERR_INVALID);
  /* Padding that is not at the end of the last quantum. */
  kbc_err_reset(&err);
  KBC_CHECK_ERR(kbc_base64_decode(a, "Zg==Zg==", 8, &out, &out_len, &err),
                KBC_ERR_INVALID);
  /* Embedded whitespace: a line break is not the decoder's job to skip. */
  kbc_err_reset(&err);
  KBC_CHECK_ERR(kbc_base64_decode(a, "Zm9v\r\n", 6, &out, &out_len, &err),
                KBC_ERR_INVALID);
  kbc_err_reset(&err);
  KBC_CHECK_ERR(kbc_base64_decode(a, "Zm9 v", 5, &out, &out_len, &err),
                KBC_ERR_INVALID);

  kbc_err_reset(&err);
  KBC_CHECK_ERR(kbc_base64_decode(NULL, "Zm9v", 4, &out, &out_len, &err),
                KBC_ERR_INVALID);
  KBC_CHECK_ERR(kbc_base64_decode(a, NULL, 4, &out, &out_len, &err),
                KBC_ERR_INVALID);
  KBC_CHECK_ERR(kbc_base64_decode(a, "Zm9v", 4, NULL, &out_len, &err),
                KBC_ERR_INVALID);
  kbc_arena_free(a);
}

KBC_TEST(hex_round_trips_and_rejects_odd_or_invalid_input) {
  kbc_str s;
  kbc_str_init(&s);
  const unsigned char data[] = {0x00, 0x0f, 0xa5, 0xff};
  KBC_CHECK_OK(kbc_hex_encode(&s, data, sizeof data));
  KBC_CHECK_EQ_STR(s.ptr, "000fa5ff");
  KBC_CHECK_EQ_INT(s.len, 8);

  unsigned char back[8];
  size_t n = 0;
  KBC_CHECK(kbc_hex_decode(s.ptr, s.len, back, sizeof back, &n));
  KBC_CHECK_EQ_INT(n, sizeof data);
  KBC_CHECK(memcmp(back, data, sizeof data) == 0);

  /* Uppercase hex is accepted and yields the same bytes. */
  KBC_CHECK(kbc_hex_decode("000FA5FF", 8, back, sizeof back, &n));
  KBC_CHECK(memcmp(back, data, sizeof data) == 0);

  /* Odd length, a non-hex digit, and an output buffer that is too small. */
  n = 99;
  KBC_CHECK(!kbc_hex_decode("abc", 3, back, sizeof back, &n));
  KBC_CHECK_EQ_INT(n, 0);
  KBC_CHECK(!kbc_hex_decode("zz", 2, back, sizeof back, &n));
  KBC_CHECK(!kbc_hex_decode("00 11", 5, back, sizeof back, &n));
  KBC_CHECK(!kbc_hex_decode("0001", 4, back, 1, &n));
  KBC_CHECK(!kbc_hex_decode(NULL, 2, back, sizeof back, &n));
  KBC_CHECK(!kbc_hex_decode("00", 2, NULL, 8, &n));
  kbc_str_free(&s);
}

/* ================================= utf8 ================================= */

KBC_TEST(utf8_len_reads_the_lead_byte) {
  KBC_CHECK_EQ_INT(kbc_utf8_len('A'), 1);
  KBC_CHECK_EQ_INT(kbc_utf8_len(0x7f), 1);
  KBC_CHECK_EQ_INT(kbc_utf8_len(0xc3), 2);
  KBC_CHECK_EQ_INT(kbc_utf8_len(0xe2), 3);
  KBC_CHECK_EQ_INT(kbc_utf8_len(0xf0), 4);
  KBC_CHECK_EQ_INT(kbc_utf8_len(0xf4), 4);
  /* Overlong two-byte leads. */
  KBC_CHECK_EQ_INT(kbc_utf8_len(0xc0), 0);
  KBC_CHECK_EQ_INT(kbc_utf8_len(0xc1), 0);
  /* Past U+10FFFF. */
  KBC_CHECK_EQ_INT(kbc_utf8_len(0xf5), 0);
  KBC_CHECK_EQ_INT(kbc_utf8_len(0xf8), 0);
  KBC_CHECK_EQ_INT(kbc_utf8_len(0xfe), 0);
  /* A bare continuation byte is not a lead. */
  KBC_CHECK_EQ_INT(kbc_utf8_len(0x80), 0);
  KBC_CHECK_EQ_INT(kbc_utf8_len(0xbf), 0);
}

KBC_TEST(utf8_validate_accepts_well_formed_text) {
  const char text[] = "a\xc3\xa9\xe2\x82\xac\xf0\x9f\x98\x80";
  size_t bad = 12345;
  KBC_CHECK(kbc_utf8_validate(text, sizeof text - 1, &bad));
  /* On success the out-param is the end of the buffer, not a stale value. */
  KBC_CHECK_EQ_INT(bad, sizeof text - 1);
  KBC_CHECK(kbc_utf8_validate("", 0, &bad));
  KBC_CHECK_EQ_INT(bad, 0);
}

KBC_TEST(utf8_validate_reports_the_offset_of_the_first_bad_sequence) {
  struct {
    const char *bytes;
    size_t n;
    size_t want;
  } cases[] = {
      {"abc\x80", 4, 3},          /* stray continuation */
      {"ab\xff", 3, 2},           /* impossible lead */
      {"\xc0\xaf", 2, 0},         /* overlong '/' */
      {"\xe0\x80\xaf", 3, 0},     /* overlong, 3-byte form */
      {"\xed\xa0\x80", 3, 0},     /* U+D800 surrogate */
      {"\xf5\x80\x80\x80", 4, 0}, /* past U+10FFFF */
      {"ok\xc3", 3, 2},           /* truncated 2-byte */
      {"ok\xf0\x9f\x98", 5, 2},   /* truncated 4-byte */
      {"\xe2\x28\xa1", 3, 0},     /* bad continuation */
  };
  for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
    size_t bad = 9999;
    KBC_CHECK_MSG(!kbc_utf8_validate(cases[i].bytes, cases[i].n, &bad),
                  "case %zu accepted", i);
    KBC_CHECK_MSG(bad == cases[i].want, "case %zu: offset %zu, want %zu", i,
                  bad, cases[i].want);
  }
  size_t bad = 7;
  KBC_CHECK(!kbc_utf8_validate(NULL, 4, &bad));
  KBC_CHECK_EQ_INT(bad, 0);
}

KBC_TEST(utf8_count_counts_codepoints_not_bytes) {
  const char text[] = "a\xc3\xa9\xe2\x82\xac\xf0\x9f\x98\x80";
  /* 4 codepoints in 10 bytes. */
  KBC_CHECK_EQ_INT(kbc_utf8_count(text, sizeof text - 1), 4);
  KBC_CHECK_EQ_INT(kbc_utf8_count("", 0), 0);
  /* A truncated final sequence must be counted, not dropped, and must not
   * stall the cursor: every byte still advances it. */
  KBC_CHECK_EQ_INT(kbc_utf8_count("a\xc3", 2), 2);
  KBC_CHECK_EQ_INT(kbc_utf8_count("\xe2\x82", 2), 2);
  KBC_CHECK_EQ_INT(kbc_utf8_count("\x80\x80", 2), 2);
  KBC_CHECK_EQ_INT(kbc_utf8_count(NULL, 4), 0);
}

/* ================================== fold ================================ */

KBC_TEST(fold_lowercases_ascii_exactly) {
  char out[32];
  KBC_CHECK_EQ_INT(kbc_fold_utf8("ABC", 3, out), 1);
  KBC_CHECK_EQ_STR(out, "a");
  KBC_CHECK_EQ_INT(kbc_fold_utf8("z", 1, out), 1);
  KBC_CHECK_EQ_STR(out, "z");
  /* A digit is already lowercase: returned unchanged, one unit. */
  KBC_CHECK_EQ_INT(kbc_fold_utf8("5-", 2, out), 1);
  KBC_CHECK_EQ_STR(out, "5");
  KBC_CHECK_EQ_INT(kbc_fold_utf8("", 0, out), 0);
  KBC_CHECK_EQ_STR(out, "");
  KBC_CHECK_EQ_INT(kbc_fold_utf8(NULL, 4, out), 0);
  KBC_CHECK_EQ_STR(out, "");
  KBC_CHECK_EQ_INT(kbc_fold_utf8("A", 1, NULL), 0);
}

KBC_TEST(fold_strips_latin_diacritics) {
  char out[32];
  /* The contract is one UTF-8 sequence per call, so the fold of "Café" is
   * the fold of its first sequence: "C" -> "c". */
  KBC_CHECK_EQ_INT(kbc_fold_utf8("Caf\xc3\xa9", 5, out), 1);
  KBC_CHECK_EQ_STR(out, "c");
  KBC_CHECK_EQ_INT(kbc_fold_utf8("\xc3\xa9", 2, out), 1); /* é */
  KBC_CHECK_EQ_STR(out, "e");
  KBC_CHECK_EQ_INT(kbc_fold_utf8("\xc3\x89", 2, out), 1); /* É */
  KBC_CHECK_EQ_STR(out, "e");
  KBC_CHECK_EQ_INT(kbc_fold_utf8("\xc5\xbf", 2, out), 1); /* ſ */
  KBC_CHECK_EQ_STR(out, "s");
  KBC_CHECK_EQ_INT(kbc_fold_utf8("\xc5\xbd", 2, out), 1); /* Ž */
  KBC_CHECK_EQ_STR(out, "z");
  /* A symbol with no ASCII equivalent folds to nothing, not to a byte. */
  KBC_CHECK_EQ_INT(kbc_fold_utf8("\xc3\x97", 2, out), 0); /* U+00D7 */
  KBC_CHECK_EQ_STR(out, "");
}

KBC_TEST(fold_digraph_stays_inside_the_out_buffer) {
  char out[32];
  memset(out, '@', sizeof out);
  /* U+00DF sharp-s decomposes to "ss": two bytes out of one two-byte
   * sequence, and it must not run past the 32-byte buffer. */
  KBC_CHECK_EQ_INT(kbc_fold_utf8("\xc3\x9f", 2, out), 2);
  KBC_CHECK_EQ_STR(out, "ss");
  KBC_CHECK_EQ_INT(out[31], '@'); /* nothing written past the result */
}

KBC_TEST(fold_passes_through_what_it_cannot_fold) {
  char out[32];
  /* A 4-byte emoji is outside the Latin table: copied unchanged. */
  KBC_CHECK_EQ_INT(kbc_fold_utf8("\xf0\x9f\x98\x80", 4, out), 4);
  KBC_CHECK_EQ_STR(out, "\xf0\x9f\x98\x80");
  /* A two-byte Greek letter too. */
  KBC_CHECK_EQ_INT(kbc_fold_utf8("\xce\xb1", 2, out), 2);
  KBC_CHECK_EQ_STR(out, "\xce\xb1");
}

KBC_TEST(fold_never_splits_a_sequence) {
  char out[32];
  /* A lone continuation byte is copied as-is, one byte, never merged with
   * the byte after it. */
  KBC_CHECK_EQ_INT(kbc_fold_utf8("\x80x", 2, out), 1);
  KBC_CHECK_EQ_STR(out, "\x80");
  /* A lead with no continuation bytes left: exactly the lead is copied. */
  KBC_CHECK_EQ_INT(kbc_fold_utf8("\xe2", 1, out), 1);
  KBC_CHECK_EQ_STR(out, "\xe2");
  /* A lead whose continuations are broken keeps all its bytes. */
  KBC_CHECK_EQ_INT(kbc_fold_utf8("\xe2(\xa1", 3, out), 3);
  KBC_CHECK_EQ_STR(out, "\xe2(\xa1");
}

/* ================================ kbc_err =============================== */

KBC_TEST(err_set_truncates_a_long_expansion) {
  kbc_err e;
  kbc_err_reset(&e);
  char *big = malloc(4096);
  KBC_CHECK_NOT_NULL(big);
  memset(big, 'z', 4095);
  big[4095] = '\0';

  kbc_status st = kbc_err_set(&e, KBC_ERR_IO, "open %s: %s", big, big);
  KBC_CHECK_ERR(st, KBC_ERR_IO);
  KBC_CHECK_EQ_INT(e.status, KBC_ERR_IO);
  /* Truncated to the cap minus the NUL, and still a valid C string. */
  KBC_CHECK_EQ_INT(strlen(e.msg), KBC_ERR_MSG_MAX - 1);
  KBC_CHECK(memcmp(e.msg + 5, big, KBC_ERR_MSG_MAX - 6) == 0);
  KBC_CHECK(!kbc_failed(KBC_OK));
  KBC_CHECK(kbc_failed(KBC_ERR_IO));

  kbc_err_reset(&e);
  KBC_CHECK_EQ_INT(e.status, KBC_OK);
  KBC_CHECK_EQ_INT(e.msg[0], 0);
  kbc_err_reset(NULL);
  free(big);
}

KBC_TEST(err_set_tolerates_a_null_carrier_and_a_null_format) {
  /* Dropping the error is the documented "may be NULL" path: the status
   * still travels. */
  KBC_CHECK_ERR(kbc_err_set(NULL, KBC_ERR_NOTFOUND, "missing %s", "x"),
                KBC_ERR_NOTFOUND);
  kbc_err e;
  kbc_err_reset(&e);
  /* A NULL format leaves the status, which is the part callers branch on. */
  KBC_CHECK_ERR(kbc_err_set(&e, KBC_ERR_PARSE, NULL), KBC_ERR_PARSE);
  KBC_CHECK_EQ_INT(e.status, KBC_ERR_PARSE);
  KBC_CHECK_EQ_INT(e.msg[0], 0);
}

KBC_TEST(status_str_is_distinct_and_non_empty_for_every_status) {
  for (int i = 0; i < (int)KBC_STATUS__COUNT; i++) {
    const char *s = kbc_status_str((kbc_status)i);
    KBC_CHECK_MSG(s != NULL && s[0] != '\0', "status %d has no name", i);
    for (int j = 0; j < i; j++) {
      KBC_CHECK_MSG(strcmp(s, kbc_status_str((kbc_status)j)) != 0,
                    "statuses %d and %d share the name \"%s\"", j, i, s);
    }
  }
  /* Out of range is a real answer, and not one of the names. */
  KBC_CHECK_EQ_STR(kbc_status_str(KBC_STATUS__COUNT), "unknown");
  KBC_CHECK_EQ_STR(kbc_status_str((kbc_status)-3), "unknown");
  KBC_CHECK(!kbc_failed(KBC_OK));
  KBC_CHECK(kbc_failed(KBC_ERR_CONFLICT));
}

KBC_TEST(const_time_eq_compares_content_over_the_given_span) {
  KBC_CHECK(kbc_const_time_eq("token", "token", 5));
  KBC_CHECK(kbc_const_time_eq("", "", 0));
  KBC_CHECK(kbc_const_time_eq(NULL, NULL, 0));

  /* The span n is the only length either side has, so a pair that shares a
   * prefix is equal over exactly the bytes that match and not equal once the
   * span reaches the differing tail. */
  char a5[5] = {'t', 'o', 'k', 'e', 'n'};
  char b5[5] = {'t', 'o', 'k', 'e', 'X'};
  KBC_CHECK(kbc_const_time_eq(a5, b5, 3));
  KBC_CHECK(kbc_const_time_eq(a5, b5, 4));
  KBC_CHECK(!kbc_const_time_eq(a5, b5, 5));

  /* Same prefix, different tail — and a difference at the very last byte
   * must still be detected. */
  char a[64], b[64];
  memset(a, 'A', sizeof a);
  memcpy(b, a, sizeof b);
  KBC_CHECK(kbc_const_time_eq(a, b, sizeof a));
  b[63] = 'B';
  KBC_CHECK(!kbc_const_time_eq(a, b, sizeof a));
  b[63] = 'A';
  b[0] = 'B';
  KBC_CHECK(!kbc_const_time_eq(a, b, sizeof a));

  /* One NUL in the middle of a span is a difference, not a short compare. */
  const char with_nul[] = {'a', '\0', 'b'};
  const char without[] = {'a', 'x', 'b'};
  KBC_CHECK(!kbc_const_time_eq(with_nul, without, 3));
  KBC_CHECK(!kbc_const_time_eq(without, NULL, 3));
  KBC_CHECK(!kbc_const_time_eq(NULL, without, 3));
}

/* ================================== main ================================ */

int main(void) {
  kbc_test_tmpdir(g_tmp, sizeof g_tmp);
  int rc = kbc_test_run("mem", (kbc_test_case[]){
      {"arena_hands_back_every_allocation", arena_hands_back_every_allocation},
      {"arena_allocations_are_8_byte_aligned",
       arena_allocations_are_8_byte_aligned},
      {"arena_zero_byte_allocations_are_distinct",
       arena_zero_byte_allocations_are_distinct},
      {"arena_bytes_counts_padded_hand_outs", arena_bytes_counts_padded_hand_outs},
      {"arena_reset_keeps_first_block",
       arena_reset_drops_later_blocks_and_keeps_the_first},
      {"arena_calloc_zeroes_recycled_memory",
       arena_calloc_zeroes_recycled_memory},
      {"arena_rejects_null_and_overflowing",
       arena_rejects_null_and_overflowing_requests},
      {"str_appends_keep_len_and_nul_in_step",
       str_appends_keep_len_and_nul_in_step},
      {"str_append_preserves_embedded_nuls",
       str_append_preserves_embedded_nuls},
      {"str_rejects_null_data",
       str_rejects_null_data_but_allows_null_zero_len},
      {"str_clear_keeps_capacity", str_clear_keeps_capacity},
      {"str_reserve_prevents_growth",
       str_reserve_prevents_growth_within_the_reserved_span},
      {"str_json_escapes",
       str_json_escapes_quotes_slashes_controls_and_del},
      {"str_json_passes_valid_utf8_through",
       str_json_passes_valid_utf8_through_untouched},
      {"str_json_appends", str_json_appends_after_existing_content},
      {"strlist_push_copies", strlist_push_copies_its_argument},
      {"strlist_push_owned",
       strlist_push_owned_takes_the_pointer_and_frees_it_once},
      {"strlist_contains_and_sort", strlist_contains_and_sort_track_the_items},
      {"read_file_reports_a_missing_path", read_file_reports_a_missing_path_by_name},
      {"read_file_refuses_a_directory", read_file_refuses_a_directory},
      {"write_then_read_round_trips_bytes",
       write_then_read_round_trips_bytes_exactly},
      {"failed_write_leaves_no_temp_file",
       failed_atomic_write_leaves_no_temp_file_behind},
      {"base64_encode_pads_by_remainder", base64_encode_pads_by_remainder},
      {"base64_round_trips_every_remainder",
       base64_round_trips_every_remainder},
      {"base64_decode_is_strict", base64_decode_is_strict},
      {"hex_round_trips", hex_round_trips_and_rejects_odd_or_invalid_input},
      {"utf8_len_reads_the_lead_byte", utf8_len_reads_the_lead_byte},
      {"utf8_validate_accepts", utf8_validate_accepts_well_formed_text},
      {"utf8_validate_reports_offset",
       utf8_validate_reports_the_offset_of_the_first_bad_sequence},
      {"utf8_count_counts_codepoints", utf8_count_counts_codepoints_not_bytes},
      {"fold_lowercases_ascii", fold_lowercases_ascii_exactly},
      {"fold_strips_latin_diacritics", fold_strips_latin_diacritics},
      {"fold_digraph_stays_inside_out", fold_digraph_stays_inside_the_out_buffer},
      {"fold_passes_through", fold_passes_through_what_it_cannot_fold},
      {"fold_never_splits_a_sequence", fold_never_splits_a_sequence},
      {"err_set_truncates", err_set_truncates_a_long_expansion},
      {"err_set_tolerates_null",
       err_set_tolerates_a_null_carrier_and_a_null_format},
      {"status_str_is_distinct",
       status_str_is_distinct_and_non_empty_for_every_status},
      {"const_time_eq", const_time_eq_compares_content_over_the_given_span},
      {NULL, NULL}});
  kbc_test_rmrf(g_tmp);
  return rc;
}
