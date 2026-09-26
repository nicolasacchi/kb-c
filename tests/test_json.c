/* test_json.c — src/json.c: strict parser, arena tree, serializers. */
#include "kbc_test.h"
#include "kbc/json.h"

#include <math.h>
#include <stdlib.h>

/* Parse `text` into a fresh arena owned by the caller; asserts success. */
static kbc_json *parse_ok(kbc_arena **a, const char *text, kbc_err *err) {
  *a = kbc_arena_new(4096);
  KBC_CHECK_NOT_NULL(*a);
  kbc_json *v = kbc_json_parse(*a, text, strlen(text), err);
  KBC_CHECK_MSG(v != NULL, "parse of %.40s failed: %s", text, err->msg);
  return v;
}

/* Asserts rejection, a KBC_ERR_PARSE status, and that the message names a byte
 * offset (every parse error must be actionable, not just "bad json"). */
static void expect_reject(const char *text) {
  kbc_arena *a = kbc_arena_new(1024);
  kbc_err err;
  kbc_err_reset(&err);
  kbc_json *v = kbc_json_parse(a, text, strlen(text), &err);
  KBC_CHECK_MSG(v == NULL, "expected %.40s to be rejected, got a value", text);
  KBC_CHECK_EQ_INT(err.status, KBC_ERR_PARSE);
  KBC_CHECK_ERR_MSG(err);
  KBC_CHECK_MSG(strstr(err.msg, "byte") != NULL,
                "error for \"%.40s\" names no byte offset: %s", text, err.msg);
  kbc_arena_free(a);
}

static void dump_to(kbc_str *out, const kbc_json *v, bool pretty) {
  kbc_str_init(out);
  kbc_err err;
  kbc_err_reset(&err);
  KBC_CHECK_OK(kbc_json_dump(v, out, pretty, &err));
}

/* ------------------------------------------------------------ round trip -- */

KBC_TEST(json_roundtrip_all_types) {
  static const char *src =
      "{\"nul\":null,\"t\":true,\"f\":false,\"i\":42,\"neg\":-17,"
      "\"flt\":3.5,\"exp\":1.5e3,\"negzero\":-0.0,\"s\":\"a\\nb\","
      "\"arr\":[1,[2,3],{\"deep\":[true,null]}],\"empty_arr\":[],"
      "\"empty_obj\":{}}";
  kbc_err err;
  kbc_err_reset(&err);
  kbc_arena *a = NULL;
  kbc_json *v = parse_ok(&a, src, &err);
  if (!v) { kbc_arena_free(a); return; }

  KBC_CHECK_EQ_INT(kbc_json_type_of(v), KBC_JSON_OBJ);
  KBC_CHECK_EQ_INT(kbc_json_len(v), 12);
  KBC_CHECK(kbc_json_is(kbc_json_get(v, "nul"), KBC_JSON_NULL));
  KBC_CHECK(kbc_json_is(kbc_json_get(v, "t"), KBC_JSON_BOOL));
  KBC_CHECK(kbc_json_bool(v, "t", false));
  KBC_CHECK(!kbc_json_bool(v, "f", true));
  KBC_CHECK_EQ_DBL(kbc_json_num(v, "i", 0), 42.0, 0);
  KBC_CHECK_EQ_DBL(kbc_json_num(v, "neg", 0), -17.0, 0);
  KBC_CHECK_EQ_DBL(kbc_json_num(v, "flt", 0), 3.5, 1e-12);
  KBC_CHECK_EQ_DBL(kbc_json_num(v, "exp", 0), 1500.0, 1e-9);
  KBC_CHECK_EQ_STR(kbc_json_str(v, "s", "?"), "a\nb");
  KBC_CHECK_EQ_INT(kbc_json_len(kbc_json_get(v, "empty_arr")), 0);
  KBC_CHECK_EQ_INT(kbc_json_len(kbc_json_get(v, "empty_obj")), 0);

  /* negative zero must survive as -0.0, not collapse to +0.0 */
  kbc_json *nz = kbc_json_get(v, "negzero");
  KBC_CHECK_NOT_NULL(nz);
  KBC_CHECK(signbit(nz->u.num));

  kbc_str out;
  dump_to(&out, v, false);
  KBC_CHECK(out.ptr != NULL && out.len > 0);

  /* Round trip: the dump must re-parse to the same structure. */
  kbc_arena *a2 = kbc_arena_new(4096);
  kbc_err e2;
  kbc_err_reset(&e2);
  kbc_json *v2 = kbc_json_parse(a2, out.ptr, out.len, &e2);
  KBC_CHECK_MSG(v2 != NULL, "dump did not re-parse: %s", e2.msg);
  if (v2) {
    KBC_CHECK_EQ_INT(kbc_json_len(v2), kbc_json_len(v));
    KBC_CHECK_EQ_DBL(kbc_json_num(v2, "exp", 0), 1500.0, 1e-9);
    KBC_CHECK_EQ_DBL(kbc_json_num(v2, "flt", 0), 3.5, 1e-12);
    KBC_CHECK_EQ_STR(kbc_json_str(v2, "s", "?"), "a\nb");
    KBC_CHECK(kbc_json_bool(v2, "f", true) == false);
    const kbc_json *arr = kbc_json_get(v2, "arr");
    KBC_CHECK_EQ_INT(kbc_json_len(arr), 3);
    KBC_CHECK_EQ_INT(kbc_json_len(kbc_json_at(arr, 1)), 2);
    KBC_CHECK_EQ_DBL(kbc_json_at(kbc_json_at(arr, 1), 1)->u.num, 3.0, 0);
    const kbc_json *deep = kbc_json_get(kbc_json_at(arr, 2), "deep");
    KBC_CHECK_EQ_INT(kbc_json_len(deep), 2);
    KBC_CHECK(kbc_json_is(kbc_json_at(deep, 0), KBC_JSON_BOOL));
  }
  kbc_arena_free(a2);
  kbc_str_free(&out);
  kbc_arena_free(a);
}

KBC_TEST(json_number_forms) {
  kbc_arena *a = kbc_arena_new(1024);
  kbc_err err;
  kbc_err_reset(&err);
  static const char *src = "[0,-0,1,-1,0.5,-0.25,1e2,1E2,1e+2,1e-2,123456789]";
  kbc_json *v = kbc_json_parse(a, src, strlen(src), &err);
  KBC_CHECK_MSG(v != NULL, "%s", err.msg);
  KBC_CHECK_EQ_INT(kbc_json_len(v), 11);
  if (!v) { kbc_arena_free(a); return; }
  KBC_CHECK_EQ_DBL(kbc_json_at(v, 0)->u.num, 0.0, 0);
  KBC_CHECK(signbit(kbc_json_at(v, 1)->u.num));
  KBC_CHECK_EQ_DBL(kbc_json_at(v, 2)->u.num, 1.0, 0);
  KBC_CHECK_EQ_DBL(kbc_json_at(v, 3)->u.num, -1.0, 0);
  KBC_CHECK_EQ_DBL(kbc_json_at(v, 4)->u.num, 0.5, 0);
  KBC_CHECK_EQ_DBL(kbc_json_at(v, 5)->u.num, -0.25, 0);
  KBC_CHECK_EQ_DBL(kbc_json_at(v, 6)->u.num, 100.0, 0);
  KBC_CHECK_EQ_DBL(kbc_json_at(v, 7)->u.num, 100.0, 0);
  KBC_CHECK_EQ_DBL(kbc_json_at(v, 8)->u.num, 100.0, 0);
  KBC_CHECK_EQ_DBL(kbc_json_at(v, 9)->u.num, 0.01, 1e-12);
  KBC_CHECK_EQ_DBL(kbc_json_at(v, 10)->u.num, 123456789.0, 0);
  /* 2^53 boundary: exactly representable, no silent rounding */
  kbc_json *big = kbc_json_parse(a, "9007199254740993", 16, &err);
  KBC_CHECK_MSG(big != NULL, "%s", err.msg);
  if (big) KBC_CHECK_EQ_DBL(big->u.num, 9007199254740992.0, 0);
  kbc_arena_free(a);
}

/* --------------------------------------------------------------- escapes -- */

KBC_TEST(json_escapes) {
  kbc_arena *a = kbc_arena_new(1024);
  kbc_err err;
  kbc_err_reset(&err);
  const char *src = "\"\\n\\t\\\"\\\\\\/\\u0041\\b\\f\\r\"";
  kbc_json *v = kbc_json_parse(a, src, strlen(src), &err);
  KBC_CHECK_MSG(v != NULL, "%s", err.msg);
  if (v) {
    const char *want = "\n\t\"\\/" "A" "\b\f\r";
    KBC_CHECK_EQ_INT(v->u.str.len, strlen(want));
    KBC_CHECK_EQ_STR(v->u.str.ptr, want);
  }
  kbc_arena_free(a);
}

KBC_TEST(json_surrogate_pair) {
  kbc_arena *a = kbc_arena_new(1024);
  kbc_err err;
  kbc_err_reset(&err);
  const char *src = "\"\\ud83d\\ude00\"";
  kbc_json *v = kbc_json_parse(a, src, strlen(src), &err);
  KBC_CHECK_MSG(v != NULL, "%s", err.msg);
  if (v) {
    /* U+1F600 -> F0 9F 98 80 */
    static const unsigned char want[] = {0xF0, 0x9F, 0x98, 0x80};
    KBC_CHECK_EQ_INT(v->u.str.len, 4);
    KBC_CHECK_MSG(memcmp(v->u.str.ptr, want, 4) == 0,
                  "got %02x %02x %02x %02x", (unsigned char)v->u.str.ptr[0],
                  (unsigned char)v->u.str.ptr[1], (unsigned char)v->u.str.ptr[2],
                  (unsigned char)v->u.str.ptr[3]);
  }
  kbc_arena_free(a);
}

KBC_TEST(json_unpaired_surrogate_rejected) {
  expect_reject("\"\\ud83d\"");      /* high with no low */
  expect_reject("\"\\ud83dx\"");     /* high not followed by \u */
  expect_reject("\"\\ud83d\\u0041\""); /* high followed by a non-low escape */
  expect_reject("\"\\udc00\"");      /* lone low */
  expect_reject("\"\\u12\"");        /* short hex */
  expect_reject("\"\\uZZZZ\"");      /* non-hex */
  expect_reject("\"\\q\"");          /* unknown escape */
}

KBC_TEST(json_raw_utf8_survives) {
  kbc_arena *a = kbc_arena_new(1024);
  kbc_err err;
  kbc_err_reset(&err);
  /* raw 2-byte, 3-byte and 4-byte sequences, no escapes at all */
  const char *src = "\"\xC3\xA9\xE2\x9C\x93\xF0\x9F\x98\x80\"";
  kbc_json *v = kbc_json_parse(a, src, strlen(src), &err);
  KBC_CHECK_MSG(v != NULL, "%s", err.msg);
  if (!v) { kbc_arena_free(a); return; }
  KBC_CHECK_EQ_INT(v->u.str.len, 2 + 3 + 4);
  KBC_CHECK(memcmp(v->u.str.ptr, src + 1, 9) == 0);
  kbc_str out;
  dump_to(&out, v, false);
  /* dump re-emits the bytes verbatim, inside quotes */
  KBC_CHECK_EQ_INT(out.len, 11);
  KBC_CHECK(memcmp(out.ptr, src, 11) == 0);
  kbc_str_free(&out);
  kbc_arena_free(a);
}

/* -------------------------------------------------------------- rejects -- */

KBC_TEST(json_rejects) {
  expect_reject("");
  expect_reject("   ");
  expect_reject("null trailing");     /* trailing garbage after the value */
  expect_reject("[1,2,]");            /* trailing comma in array */
  expect_reject("{\"a\":1,}");         /* trailing comma in object */
  expect_reject("'single'");          /* single-quoted string */
  expect_reject("{'a':1}");           /* single-quoted key */
  expect_reject("NaN");
  expect_reject("Infinity");
  expect_reject("-Infinity");
  expect_reject("[NaN]");
  expect_reject("+1");                /* leading plus */
  expect_reject("01");                /* leading zero */
  expect_reject("[01]");
  expect_reject("1.");                /* no fraction digits */
  expect_reject(".1");
  expect_reject("1e");                /* no exponent digits */
  expect_reject("0x1f");
  expect_reject("{\"a\" 1}");         /* missing colon */
  expect_reject("{\"a\":}");          /* missing value */
  expect_reject("\"unterminated");    /* unterminated string */
  expect_reject("[\"a\"");            /* unclosed bracket */
  expect_reject("{\"a\":1");          /* unclosed brace */
  expect_reject("{a:1}");             /* non-numeric (unquoted) key */
  expect_reject("tru");               /* truncated literal */
  expect_reject("nulll");
  expect_reject("1e400");             /* out of range for a double */
}

KBC_TEST(json_rejects_control_byte_in_string) {
  kbc_arena *a = kbc_arena_new(256);
  kbc_err err;
  kbc_err_reset(&err);
  const char src[] = {'"', 'a', 0x01, 'b', '"'};
  kbc_json *v = kbc_json_parse(a, src, sizeof src, &err);
  KBC_CHECK_NULL(v);
  KBC_CHECK_EQ_INT(err.status, KBC_ERR_PARSE);
  KBC_CHECK_ERR_MSG(err);
  kbc_arena_free(a);
}

KBC_TEST(json_rejects_null_arguments) {
  kbc_err err;
  kbc_err_reset(&err);
  KBC_CHECK_NULL(kbc_json_parse(NULL, "1", 1, &err));
  KBC_CHECK_EQ_INT(err.status, KBC_ERR_INVALID);
  kbc_err_reset(&err);
  kbc_arena *a0 = kbc_arena_new(64);
  KBC_CHECK_NULL(kbc_json_parse(a0, NULL, 4, &err));
  kbc_arena_free(a0);
  kbc_arena_free(NULL); /* must be a no-op, not a crash */
  /* len 0 with NULL text is an empty document, i.e. a parse error, not a crash */
  kbc_arena *a = kbc_arena_new(64);
  kbc_err_reset(&err);
  KBC_CHECK_NULL(kbc_json_parse(a, NULL, 0, &err));
  KBC_CHECK_EQ_INT(err.status, KBC_ERR_PARSE);
  kbc_arena_free(a);
}

/* --------------------------------------------------------- duplicate key -- */

KBC_TEST(json_duplicate_key_last_wins) {
  kbc_arena *a = kbc_arena_new(1024);
  kbc_err err;
  kbc_err_reset(&err);
  const char *src = "{\"k\":1,\"other\":2,\"k\":3,\"k\":\"four\"}";
  kbc_json *v = kbc_json_parse(a, src, strlen(src), &err);
  KBC_CHECK_MSG(v != NULL, "%s", err.msg);
  if (!v) { kbc_arena_free(a); return; }
  /* duplicates collapse: the first key position keeps the last value */
  KBC_CHECK_EQ_INT(kbc_json_len(v), 2);
  KBC_CHECK_EQ_STR(kbc_json_str(v, "k", "?"), "four");
  KBC_CHECK_EQ_DBL(kbc_json_num(v, "k", -1), -1.0, 0); /* now a string */
  KBC_CHECK_EQ_DBL(kbc_json_num(v, "other", -1), 2.0, 0);
  kbc_str out;
  dump_to(&out, v, false);
  KBC_CHECK_EQ_STR(out.ptr, "{\"k\":\"four\",\"other\":2}");
  kbc_str_free(&out);
  kbc_arena_free(a);
}

/* ----------------------------------------------------------------- depth -- */

static char *nest_brackets(size_t n) {
  char *s = malloc(n * 2 + 1);
  for (size_t i = 0; i < n; i++) s[i] = '[';
  for (size_t i = 0; i < n; i++) s[n + i] = ']';
  s[n * 2] = '\0';
  return s;
}

KBC_TEST(json_depth_limit) {
  kbc_arena *a = kbc_arena_new(64 * 1024);
  /* 64 levels: accepted (this is the documented cap, not off-by-one-strict) */
  char *ok = nest_brackets(64);
  kbc_err err;
  kbc_err_reset(&err);
  kbc_json *v = kbc_json_parse(a, ok, strlen(ok), &err);
  KBC_CHECK_MSG(v != NULL, "64 levels rejected: %s", err.msg);
  kbc_str out;
  kbc_str_init(&out);
  if (v) {
    KBC_CHECK_OK(kbc_json_dump(v, &out, false, &err));
    KBC_CHECK_EQ_INT(out.len, 128);
  }
  kbc_str_free(&out);
  free(ok);

  /* 66 levels: rejected with an offset, and no stack exhaustion. The top-level
   * value parses at depth 0, so the cap admits 64 nested levels below it. */
  char *deep = nest_brackets(66);
  kbc_err_reset(&err);
  kbc_json *d = kbc_json_parse(a, deep, strlen(deep), &err);
  KBC_CHECK_NULL(d);
  KBC_CHECK_EQ_INT(err.status, KBC_ERR_PARSE);
  KBC_CHECK(strstr(err.msg, "deep") != NULL);
  free(deep);

  /* a pathological 100k-deep document must be refused, not crash the process */
  char *vdeep = nest_brackets(100000);
  kbc_err_reset(&err);
  KBC_CHECK_NULL(kbc_json_parse(a, vdeep, strlen(vdeep), &err));
  KBC_CHECK_EQ_INT(err.status, KBC_ERR_PARSE);
  free(vdeep);
  kbc_arena_free(a);
}

/* -------------------------------------------------------------- builders -- */

KBC_TEST(json_obj_set_overwrite_does_not_grow) {
  kbc_arena *a = kbc_arena_new(1024);
  kbc_json *o = kbc_json_new_obj(a);
  KBC_CHECK_OK(kbc_json_obj_set(a, o, "k", kbc_json_new_num(a, 1)));
  size_t len1 = kbc_json_len(o);
  size_t cap1 = o->u.obj.cap;
  KBC_CHECK_OK(kbc_json_obj_set(a, o, "k", kbc_json_new_num(a, 2)));
  KBC_CHECK_EQ_INT(kbc_json_len(o), len1);
  KBC_CHECK_EQ_INT(o->u.obj.cap, cap1);
  KBC_CHECK_EQ_DBL(kbc_json_num(o, "k", -1), 2.0, 0);
  /* a different key does grow len, and cap grows only when len outruns it */
  for (int i = 0; i < 32; i++) {
    char key[16];
    snprintf(key, sizeof key, "k%d", i);
    KBC_CHECK_OK(kbc_json_obj_set(a, o, key, kbc_json_new_num(a, i)));
  }
  KBC_CHECK_EQ_INT(kbc_json_len(o), 33);
  KBC_CHECK(o->u.obj.cap >= 33);
  /* every key is individually retrievable, none shadowed by the overwrite */
  for (int i = 0; i < 32; i++) {
    char key[16];
    snprintf(key, sizeof key, "k%d", i);
    KBC_CHECK_EQ_DBL(kbc_json_num(o, key, -99), (double)i, 0);
  }
  /* the overwritten value is still the one stored under "k" */
  KBC_CHECK_EQ_DBL(kbc_json_num(o, "k", -99), 2.0, 0);
  kbc_arena_free(a);
}

KBC_TEST(json_arr_push_and_nesting) {
  kbc_arena *a = kbc_arena_new(1024);
  kbc_json *root = kbc_json_new_obj(a);
  kbc_json *arr = kbc_json_new_arr(a);
  kbc_json *inner = kbc_json_new_arr(a);
  kbc_json *leaf = kbc_json_new_obj(a);
  KBC_CHECK_OK(kbc_json_arr_push(a, inner, kbc_json_new_str(a, "deep")));
  KBC_CHECK_OK(kbc_json_obj_set(a, leaf, "x", kbc_json_new_num(a, 1)));
  KBC_CHECK_OK(kbc_json_arr_push(a, inner, leaf));
  KBC_CHECK_OK(kbc_json_arr_push(a, arr, inner));
  KBC_CHECK_OK(kbc_json_arr_push(a, arr, kbc_json_new_null(a)));
  KBC_CHECK_OK(kbc_json_obj_set(a, root, "list", arr));
  KBC_CHECK_EQ_INT(kbc_json_len(arr), 2);
  KBC_CHECK_EQ_INT(kbc_json_at(arr, 0)->type, KBC_JSON_ARR);
  KBC_CHECK(kbc_json_is(kbc_json_at(arr, 1), KBC_JSON_NULL));
  kbc_err err;
  kbc_str out;
  dump_to(&out, root, false);
  KBC_CHECK_EQ_STR(out.ptr, "{\"list\":[[\"deep\",{\"x\":1}],null]}");
  kbc_err_reset(&err);
  kbc_arena *a2 = kbc_arena_new(1024);
  kbc_json *back = kbc_json_parse(a2, out.ptr, out.len, &err);
  KBC_CHECK_MSG(back != NULL, "%s", err.msg);
  if (back) KBC_CHECK_EQ_INT(kbc_json_len(kbc_json_at(kbc_json_at(kbc_json_get(back, "list"), 0), 1)), 1);
  kbc_arena_free(a2);
  kbc_str_free(&out);
  kbc_arena_free(a);
}

KBC_TEST(json_builder_rejects_bad_arguments) {
  kbc_arena *a = kbc_arena_new(256);
  kbc_json *o = kbc_json_new_obj(a);
  kbc_json *arr = kbc_json_new_arr(a);
  KBC_CHECK_EQ_INT(kbc_json_obj_set(NULL, o, "k", kbc_json_new_null(a)),
                   KBC_ERR_INVALID);
  KBC_CHECK_EQ_INT(kbc_json_obj_set(a, NULL, "k", kbc_json_new_null(a)),
                   KBC_ERR_INVALID);
  KBC_CHECK_EQ_INT(kbc_json_obj_set(a, o, NULL, kbc_json_new_null(a)),
                   KBC_ERR_INVALID);
  KBC_CHECK_EQ_INT(kbc_json_obj_set(a, o, "k", NULL), KBC_ERR_INVALID);
  KBC_CHECK_EQ_INT(kbc_json_obj_set(a, arr, "k", kbc_json_new_null(a)),
                   KBC_ERR_INVALID); /* not an object */
  KBC_CHECK_EQ_INT(kbc_json_arr_push(NULL, arr, kbc_json_new_null(a)),
                   KBC_ERR_INVALID);
  KBC_CHECK_EQ_INT(kbc_json_arr_push(a, NULL, kbc_json_new_null(a)),
                   KBC_ERR_INVALID);
  KBC_CHECK_EQ_INT(kbc_json_arr_push(a, arr, NULL), KBC_ERR_INVALID);
  KBC_CHECK_EQ_INT(kbc_json_arr_push(a, o, kbc_json_new_null(a)),
                   KBC_ERR_INVALID); /* not an array */
  kbc_arena_free(a);
}

KBC_TEST(json_null_accessors_are_safe) {
  KBC_CHECK_NULL(kbc_json_get(NULL, "k"));
  KBC_CHECK_NULL(kbc_json_at(NULL, 0));
  KBC_CHECK_EQ_INT(kbc_json_len(NULL), 0);
  KBC_CHECK_EQ_INT(kbc_json_type_of(NULL), KBC_JSON_NULL);
  KBC_CHECK(!kbc_json_is(NULL, KBC_JSON_NULL));
  KBC_CHECK_EQ_STR(kbc_json_str(NULL, "k", "dflt"), "dflt");
  KBC_CHECK_EQ_DBL(kbc_json_num(NULL, "k", -1.5), -1.5, 0);
  KBC_CHECK_EQ_INT(kbc_json_i64(NULL, "k", -7), -7);
  KBC_CHECK(kbc_json_bool(NULL, "k", true) == true);
  kbc_json *none = NULL;
  KBC_CHECK_EQ_INT(kbc_json_str(none, "k", NULL) == NULL, 1);
  /* a non-object receiver yields the default, never a wild read */
  kbc_arena *a = kbc_arena_new(256);
  kbc_json *arr = kbc_json_new_arr(a);
  KBC_CHECK_NULL(kbc_json_get(arr, "k"));
  KBC_CHECK_EQ_STR(kbc_json_str(arr, "k", "d"), "d");
  KBC_CHECK_NULL(kbc_json_at(arr, 99));
  KBC_CHECK_NULL(kbc_json_at(arr, 0));
  KBC_CHECK_EQ_INT(kbc_json_len(kbc_json_new_str(a, "scalar")), 0);
  kbc_arena_free(a);
}

KBC_TEST(json_typed_getters_type_mismatch) {
  kbc_arena *a = kbc_arena_new(512);
  kbc_err err;
  kbc_err_reset(&err);
  const char *src = "{\"n\":\"str\",\"b\":true,\"num\":3.9,"
                    "\"big\":1e30,\"small\":-1e30,\"obj\":{}}";
  kbc_json *v = kbc_json_parse(a, src, strlen(src), &err);
  KBC_CHECK_MSG(v != NULL, "%s", err.msg);
  if (!v) { kbc_arena_free(a); return; }
  KBC_CHECK_EQ_STR(kbc_json_str(v, "num", "d"), "d"); /* number, not string */
  KBC_CHECK_EQ_STR(kbc_json_str(v, "missing", "d"), "d");
  KBC_CHECK_EQ_DBL(kbc_json_num(v, "n", 9.0), 9.0, 0);
  KBC_CHECK(kbc_json_bool(v, "num", true) == true);
  KBC_CHECK_EQ_INT(kbc_json_i64(v, "num", -1), 3);       /* truncates */
  KBC_CHECK_EQ_INT(kbc_json_i64(v, "n", -1), -1);
  /* out of int64 range saturates instead of invoking UB */
  KBC_CHECK_EQ_INT(kbc_json_i64(v, "big", 0), INT64_MAX);
  KBC_CHECK_EQ_INT(kbc_json_i64(v, "small", 0), INT64_MIN);
  kbc_arena_free(a);
}

/* ----------------------------------------------------------------- dump -- */

KBC_TEST(json_dump_nonfinite_is_null) {
  kbc_arena *a = kbc_arena_new(256);
  kbc_err err;
  kbc_err_reset(&err);
  kbc_json *o = kbc_json_new_obj(a);
  kbc_json *nan = kbc_json_new_num(a, (double)NAN);
  kbc_json *inf = kbc_json_new_num(a, (double)INFINITY);
  kbc_json *ninf = kbc_json_new_num(a, -(double)INFINITY);
  KBC_CHECK_OK(kbc_json_obj_set(a, o, "nan", nan));
  KBC_CHECK_OK(kbc_json_obj_set(a, o, "inf", inf));
  KBC_CHECK_OK(kbc_json_obj_set(a, o, "ninf", ninf));
  kbc_str out;
  dump_to(&out, o, false);
  KBC_CHECK_EQ_STR(out.ptr, "{\"nan\":null,\"inf\":null,\"ninf\":null}");
  /* and the output must itself be valid JSON */
  kbc_arena *a2 = kbc_arena_new(256);
  kbc_err e2;
  kbc_err_reset(&e2);
  KBC_CHECK_NOT_NULL(kbc_json_parse(a2, out.ptr, out.len, &e2));
  kbc_arena_free(a2);
  kbc_str_free(&out);
  kbc_arena_free(a);
}

KBC_TEST(json_dump_escapes_control_bytes) {
  kbc_arena *a = kbc_arena_new(256);
  kbc_err err;
  kbc_err_reset(&err);
  const char raw[] = {'a', 0x01, 'b', 0x1f, 'c', 0x7f, 0};
  kbc_json *s = kbc_json_new_strn(a, raw, 6);
  kbc_str out;
  dump_to(&out, s, false);
  KBC_CHECK_EQ_STR(out.ptr, "\"a\\u0001b\\u001fc\x7f\"");
  /* DEL and above are legal raw JSON bytes, not escaped */
  KBC_CHECK(strstr(out.ptr, "c") != NULL);
  kbc_str_free(&out);

  /* an embedded NUL survives as bytes, and dumps as \u0000 */
  kbc_json *z = kbc_json_new_strn(a, "x\0y", 3);
  kbc_str o2;
  dump_to(&o2, z, false);
  KBC_CHECK_EQ_STR(o2.ptr, "\"x\\u0000y\"");
  KBC_CHECK_EQ_INT(z->u.str.len, 3);
  kbc_str_free(&o2);

  /* a short \uXXXX is not valid JSON: escaping must pad to four digits */
  KBC_CHECK_OK(kbc_json_escape(&out, "\x01", 1));
  KBC_CHECK_EQ_STR(out.ptr, "\\u0001");
  kbc_str_free(&out);
  kbc_arena_free(a);
}

KBC_TEST(json_dump_pretty_reparses) {
  kbc_arena *a = kbc_arena_new(1024);
  kbc_err err;
  kbc_err_reset(&err);
  const char *src = "{\"a\":[1,2,{}],\"b\":{\"c\":[]},\"d\":\"s\"}";
  kbc_json *v = kbc_json_parse(a, src, strlen(src), &err);
  KBC_CHECK_MSG(v != NULL, "%s", err.msg);
  if (!v) { kbc_arena_free(a); return; }
  kbc_str pretty;
  dump_to(&pretty, v, true);
  kbc_str compact;
  dump_to(&compact, v, false);
  KBC_CHECK(pretty.len > compact.len);
  KBC_CHECK(strchr(pretty.ptr, '\n') != NULL);
  KBC_CHECK(strstr(pretty.ptr, "  \"a\"") != NULL); /* two-space indent */
  KBC_CHECK(strstr(pretty.ptr, ": ") != NULL);       /* space after colon */

  kbc_arena *a2 = kbc_arena_new(1024);
  kbc_err e2;
  kbc_err_reset(&e2);
  kbc_json *back = kbc_json_parse(a2, pretty.ptr, pretty.len, &e2);
  KBC_CHECK_MSG(back != NULL, "pretty dump did not re-parse: %s", e2.msg);
  if (back) {
    kbc_str again;
    dump_to(&again, back, false);
    KBC_CHECK_EQ_STR(again.ptr, compact.ptr); /* identical tree */
    kbc_str_free(&again);
  }
  kbc_arena_free(a2);
  kbc_str_free(&pretty);
  kbc_str_free(&compact);
  kbc_arena_free(a);
}

KBC_TEST(json_dump_appends_and_null_value) {
  kbc_str out;
  kbc_str_init(&out);
  kbc_err err;
  kbc_err_reset(&err);
  KBC_CHECK_OK(kbc_str_puts(&out, "PREFIX:"));
  KBC_CHECK_OK(kbc_json_dump(NULL, &out, false, &err));
  kbc_str_free(&out);
  kbc_str_init(&out);
  kbc_err_reset(&err);
  KBC_CHECK_EQ_INT(kbc_json_dump(NULL, NULL, false, &err), KBC_ERR_INVALID);
  KBC_CHECK_ERR_MSG(err);
  kbc_str_free(&out);
  kbc_str_init(&out);
  KBC_CHECK_EQ_INT(kbc_json_escape(NULL, "x", 1), KBC_ERR_INVALID);
  kbc_str_free(&out);
  kbc_str_init(&out);
  KBC_CHECK_OK(kbc_json_escape(&out, NULL, 0)); /* NULL string == empty */
  KBC_CHECK_EQ_INT(out.len, 0);
  kbc_str_free(&out);
}

int main(void) {
  static const kbc_test_case cases[] = {
      {"roundtrip_all_types", json_roundtrip_all_types},
      {"number_forms", json_number_forms},
      {"escapes", json_escapes},
      {"surrogate_pair", json_surrogate_pair},
      {"unpaired_surrogate_rejected", json_unpaired_surrogate_rejected},
      {"raw_utf8_survives", json_raw_utf8_survives},
      {"rejects", json_rejects},
      {"rejects_control_byte_in_string", json_rejects_control_byte_in_string},
      {"rejects_null_arguments", json_rejects_null_arguments},
      {"duplicate_key_last_wins", json_duplicate_key_last_wins},
      {"depth_limit", json_depth_limit},
      {"obj_set_overwrite_does_not_grow", json_obj_set_overwrite_does_not_grow},
      {"arr_push_and_nesting", json_arr_push_and_nesting},
      {"builder_rejects_bad_arguments", json_builder_rejects_bad_arguments},
      {"null_accessors_are_safe", json_null_accessors_are_safe},
      {"typed_getters_type_mismatch", json_typed_getters_type_mismatch},
      {"dump_nonfinite_is_null", json_dump_nonfinite_is_null},
      {"dump_escapes_control_bytes", json_dump_escapes_control_bytes},
      {"dump_pretty_reparses", json_dump_pretty_reparses},
      {"dump_appends_and_null_value", json_dump_appends_and_null_value},
      {NULL, NULL},
  };
  return kbc_test_run("json", cases);
}
