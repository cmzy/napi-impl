// gtest for common core napi_* surface the upstream conformance suite happens
// not to exercise on this backend: utf16/latin1 strings, value coercions, error
// create/throw/typeof, strict-equals, integer getters, escapable handle scopes,
// and version / last-error / instance-data. Engine-neutral (shared fixture).

#include <cstring>
#include <string>

#include "napi_gtest_fixture.h"

namespace {

// ---- strings: utf16 + latin1 round-trips ----------------------------------
TEST_F(NapiExtras, StringUtf16RoundTrip) {
  const char16_t* in = u"héllo";  // hello with an accented e
  napi_value s = nullptr;
  ASSERT_EQ(napi_create_string_utf16(env_, in, NAPI_AUTO_LENGTH, &s), napi_ok);
  napi_valuetype t;
  ASSERT_EQ(napi_typeof(env_, s, &t), napi_ok);
  EXPECT_EQ(t, napi_string);

  size_t len = 0;
  ASSERT_EQ(napi_get_value_string_utf16(env_, s, nullptr, 0, &len), napi_ok);  // length query
  EXPECT_EQ(len, 5u);
  char16_t buf[16] = {0};
  size_t copied = 0;
  ASSERT_EQ(napi_get_value_string_utf16(env_, s, buf, 16, &copied), napi_ok);
  EXPECT_EQ(copied, 5u);
  EXPECT_EQ(std::u16string(buf), std::u16string(in));
}

TEST_F(NapiExtras, StringLatin1RoundTrip) {
  const char* in = "caf\xe9";  // "café" in latin1 (0xE9 = é)
  napi_value s = nullptr;
  ASSERT_EQ(napi_create_string_latin1(env_, in, NAPI_AUTO_LENGTH, &s), napi_ok);
  size_t len = 0;
  ASSERT_EQ(napi_get_value_string_latin1(env_, s, nullptr, 0, &len), napi_ok);
  EXPECT_EQ(len, 4u);
  char buf[16] = {0};
  size_t copied = 0;
  ASSERT_EQ(napi_get_value_string_latin1(env_, s, buf, 16, &copied), napi_ok);
  EXPECT_EQ(copied, 4u);
  EXPECT_STREQ(buf, in);
}

// ---- coercions -------------------------------------------------------------
TEST_F(NapiExtras, Coercions) {
  napi_value num = nullptr, str = nullptr, b = nullptr, obj = nullptr, out = nullptr;

  // coerce string "42" -> number 42
  ASSERT_EQ(napi_coerce_to_number(env_, Str("42"), &num), napi_ok);
  EXPECT_EQ(I32(num), 42);

  // coerce number 42 -> string "42"
  ASSERT_EQ(napi_coerce_to_string(env_, Int(42), &str), napi_ok);
  char buf[8] = {0};
  size_t n = 0;
  ASSERT_EQ(napi_get_value_string_utf8(env_, str, buf, sizeof(buf), &n), napi_ok);
  EXPECT_STREQ(buf, "42");

  // coerce 0 -> false, 1 -> true
  ASSERT_EQ(napi_coerce_to_bool(env_, Int(0), &b), napi_ok);
  EXPECT_FALSE(Truthy(b));
  ASSERT_EQ(napi_coerce_to_bool(env_, Int(1), &b), napi_ok);
  EXPECT_TRUE(Truthy(b));

  // coerce number -> Number object
  ASSERT_EQ(napi_coerce_to_object(env_, Int(7), &obj), napi_ok);
  napi_valuetype t;
  ASSERT_EQ(napi_typeof(env_, obj, &t), napi_ok);
  EXPECT_EQ(t, napi_object);
  (void)out;
}

// ---- errors: create / is_error / throw / pending / clear -------------------
TEST_F(NapiExtras, ErrorCreateThrowClear) {
  napi_value err = nullptr;
  ASSERT_EQ(napi_create_error(env_, nullptr, Str("boom"), &err), napi_ok);
  bool is = false;
  ASSERT_EQ(napi_is_error(env_, err, &is), napi_ok);
  EXPECT_TRUE(is);
  ASSERT_EQ(napi_is_error(env_, Int(1), &is), napi_ok);
  EXPECT_FALSE(is);

  ASSERT_EQ(napi_throw(env_, err), napi_ok);
  bool pending = false;
  ASSERT_EQ(napi_is_exception_pending(env_, &pending), napi_ok);
  EXPECT_TRUE(pending);
  napi_value caught = nullptr;
  ASSERT_EQ(napi_get_and_clear_last_exception(env_, &caught), napi_ok);
  ASSERT_EQ(napi_is_exception_pending(env_, &pending), napi_ok);
  EXPECT_FALSE(pending);
}

// ---- typeof across the value kinds ----------------------------------------
TEST_F(NapiExtras, Typeof) {
  napi_valuetype t;
  napi_value undef = nullptr, nul = nullptr, fn = nullptr;
  ASSERT_EQ(napi_get_undefined(env_, &undef), napi_ok);
  ASSERT_EQ(napi_get_null(env_, &nul), napi_ok);
  fn = Run("(function () {})");

  ASSERT_EQ(napi_typeof(env_, undef, &t), napi_ok);
  EXPECT_EQ(t, napi_undefined);
  ASSERT_EQ(napi_typeof(env_, nul, &t), napi_ok);
  EXPECT_EQ(t, napi_null);
  ASSERT_EQ(napi_typeof(env_, Int(1), &t), napi_ok);
  EXPECT_EQ(t, napi_number);
  ASSERT_EQ(napi_typeof(env_, Str("x"), &t), napi_ok);
  EXPECT_EQ(t, napi_string);
  ASSERT_EQ(napi_typeof(env_, fn, &t), napi_ok);
  EXPECT_EQ(t, napi_function);
  ASSERT_EQ(napi_typeof(env_, Run("({})"), &t), napi_ok);
  EXPECT_EQ(t, napi_object);
  ASSERT_EQ(napi_typeof(env_, Run("true"), &t), napi_ok);
  EXPECT_EQ(t, napi_boolean);
}

// ---- strict equals ---------------------------------------------------------
TEST_F(NapiExtras, StrictEquals) {
  napi_value a = Str("x");
  bool eq = false;
  ASSERT_EQ(napi_strict_equals(env_, a, a, &eq), napi_ok);
  EXPECT_TRUE(eq);
  ASSERT_EQ(napi_strict_equals(env_, Int(1), Int(2), &eq), napi_ok);
  EXPECT_FALSE(eq);
}

// ---- integer getters -------------------------------------------------------
TEST_F(NapiExtras, IntegerGetters) {
  uint32_t u = 0;
  ASSERT_EQ(napi_get_value_uint32(env_, Run("4000000000"), &u), napi_ok);
  EXPECT_EQ(u, 4000000000u);
  int64_t i = 0;
  ASSERT_EQ(napi_get_value_int64(env_, Run("9007199254740991"), &i), napi_ok);
  EXPECT_EQ(i, 9007199254740991LL);
}

// ---- escapable handle scope ------------------------------------------------
TEST_F(NapiExtras, EscapableHandleScope) {
  napi_escapable_handle_scope scope = nullptr;
  ASSERT_EQ(napi_open_escapable_handle_scope(env_, &scope), napi_ok);
  napi_value inner = nullptr;
  ASSERT_EQ(napi_create_int32(env_, 99, &inner), napi_ok);
  napi_value escaped = nullptr;
  ASSERT_EQ(napi_escape_handle(env_, scope, inner, &escaped), napi_ok);
  ASSERT_EQ(napi_close_escapable_handle_scope(env_, scope), napi_ok);
  EXPECT_EQ(I32(escaped), 99);
}

// ---- version / last-error / instance-data ----------------------------------
TEST_F(NapiExtras, VersionLastErrorInstanceData) {
  uint32_t ver = 0;
  ASSERT_EQ(napi_get_version(env_, &ver), napi_ok);
  EXPECT_GE(ver, 8u);

  const napi_extended_error_info* info = nullptr;
  ASSERT_EQ(napi_get_last_error_info(env_, &info), napi_ok);
  ASSERT_NE(info, nullptr);

  int marker = 7;
  ASSERT_EQ(napi_set_instance_data(env_, &marker, nullptr, nullptr), napi_ok);
  void* got = nullptr;
  ASSERT_EQ(napi_get_instance_data(env_, &got), napi_ok);
  EXPECT_EQ(got, &marker);
}

}  // namespace
