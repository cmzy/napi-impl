// gtest conversion of the JSC NAPI backend smoke test (formerly a bespoke
// main()+CHECK harness). Mirrors the V8/Hermes M1 check (run "1+2" == 3) and
// exercises a bit more of the hand-written surface — string round-trip, object
// properties, and a native function called from JS — to confirm the core works.
// Uses only the public embedding API + standard napi_* surface (via the shared
// NapiExtras fixture, which owns platform/runtime/env + a root handle scope).

#include "jsc_gtest_fixture.h"

namespace {

// A native function: returns its first argument doubled (arg0 + arg0).
napi_value double_it(napi_env env, napi_callback_info info) {
  size_t argc = 1;
  napi_value argv[1];
  if (napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr) != napi_ok || argc < 1)
    return nullptr;
  double v = 0;
  napi_get_value_double(env, argv[0], &v);
  napi_value result = nullptr;
  napi_create_double(env, v + v, &result);
  return result;
}

// (1) run "1 + 2" == 3
TEST_F(NapiExtras, SmokeRunScriptAddsIntegers) {
  napi_value src = nullptr, result = nullptr;
  ASSERT_EQ(napi_create_string_utf8(env_, "1 + 2", NAPI_AUTO_LENGTH, &src), napi_ok);
  ASSERT_EQ(napi_run_script(env_, src, &result), napi_ok);
  int32_t v = 0;
  ASSERT_EQ(napi_get_value_int32(env_, result, &v), napi_ok);
  EXPECT_EQ(v, 3);
}

// (2) string round-trip through a script
TEST_F(NapiExtras, SmokeStringRoundTrip) {
  napi_value src = nullptr, result = nullptr;
  ASSERT_EQ(napi_create_string_utf8(env_, "'hello ' + 'world'", NAPI_AUTO_LENGTH, &src), napi_ok);
  ASSERT_EQ(napi_run_script(env_, src, &result), napi_ok);
  char buf[64];
  size_t len = 0;
  ASSERT_EQ(napi_get_value_string_utf8(env_, result, buf, sizeof(buf), &len), napi_ok);
  EXPECT_STREQ(buf, "hello world");
}

// (3) object property set/get
TEST_F(NapiExtras, SmokeObjectPropertySetGet) {
  napi_value obj = nullptr, val = nullptr, got = nullptr;
  ASSERT_EQ(napi_create_object(env_, &obj), napi_ok);
  ASSERT_EQ(napi_create_int32(env_, 42, &val), napi_ok);
  ASSERT_EQ(napi_set_named_property(env_, obj, "x", val), napi_ok);
  ASSERT_EQ(napi_get_named_property(env_, obj, "x", &got), napi_ok);
  int32_t v = 0;
  ASSERT_EQ(napi_get_value_int32(env_, got, &v), napi_ok);
  EXPECT_EQ(v, 42);
}

// (4) native function called from JS: double_it(21) == 42
TEST_F(NapiExtras, SmokeNativeFunctionCalledFromJs) {
  napi_value global = nullptr, fn = nullptr, arg = nullptr, result = nullptr;
  ASSERT_EQ(napi_get_global(env_, &global), napi_ok);
  ASSERT_EQ(napi_create_function(env_, "double_it", NAPI_AUTO_LENGTH, double_it, nullptr, &fn), napi_ok);
  ASSERT_EQ(napi_create_int32(env_, 21, &arg), napi_ok);
  ASSERT_EQ(napi_call_function(env_, global, fn, 1, &arg, &result), napi_ok);
  int32_t v = 0;
  ASSERT_EQ(napi_get_value_int32(env_, result, &v), napi_ok);
  EXPECT_EQ(v, 42);
}

}  // namespace
