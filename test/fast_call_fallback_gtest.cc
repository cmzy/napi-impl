// gtest for the engine-neutral fast-call FALLBACK (src/common/fast_call_fallback.cc):
// the slow-equivalent surface non-V8 backends expose for napi/fast_call.h. Built
// for JSC (NAPI_HAS_FAST_CALL is undefined on the CMake track), so the symbols
// resolve to the fallback impls — napi_create_fast_function* degrade to
// napi_create_function, napi_fast_wrap == napi_wrap, and the fast-only read
// helpers are inert (NULL/false). Raises the otherwise-untested fallback file.

#include "napi/fast_call.h"
#include "napi_gtest_fixture.h"

namespace {

// A standard slow napi callback: returns arg0 + arg1.
napi_value AddSlow(napi_env env, napi_callback_info info) {
  size_t argc = 2;
  napi_value argv[2];
  napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
  double a = 0, b = 0;
  napi_get_value_double(env, argv[0], &a);
  napi_get_value_double(env, argv[1], &b);
  napi_value r = nullptr;
  napi_create_double(env, a + b, &r);
  return r;
}

// napi_create_fast_function degrades to napi_create_function (slow_cb only).
TEST_F(NapiExtras, FallbackCreateFastFunction) {
  // sig / fast_fn provided but ignored on the fallback path.
  const napi_fast_type args[] = {napi_fast_receiver, napi_fast_float64, napi_fast_float64};
  const napi_fast_signature sig{napi_fast_float64, 3, args, false};
  napi_value fn = nullptr;
  ASSERT_EQ(napi_create_fast_function(env_, "add", NAPI_AUTO_LENGTH, AddSlow, &sig,
                                      reinterpret_cast<const void*>(&AddSlow), nullptr, &fn),
            napi_ok);
  SetGlobal("add", fn);
  EXPECT_EQ(I32(Run("add(2, 3)")), 5);

  // slow-only form (sig == nullptr) takes the same fallback path.
  napi_value fn2 = nullptr;
  ASSERT_EQ(napi_create_fast_function(env_, "add2", NAPI_AUTO_LENGTH, AddSlow, nullptr, nullptr, nullptr, &fn2),
            napi_ok);
  SetGlobal("add2", fn2);
  EXPECT_EQ(I32(Run("add2(4, 5)")), 9);
}

// napi_create_fast_function_overloads likewise degrades to a single slow_cb.
TEST_F(NapiExtras, FallbackCreateFastFunctionOverloads) {
  const napi_fast_type a2[] = {napi_fast_receiver, napi_fast_float64};
  const napi_fast_type a3[] = {napi_fast_receiver, napi_fast_float64, napi_fast_float64};
  const napi_fast_overload ovs[] = {
      {{napi_fast_float64, 2, a2, false}, reinterpret_cast<const void*>(&AddSlow)},
      {{napi_fast_float64, 3, a3, false}, reinterpret_cast<const void*>(&AddSlow)},
  };
  napi_value fn = nullptr;
  ASSERT_EQ(napi_create_fast_function_overloads(env_, "ov", NAPI_AUTO_LENGTH, AddSlow, ovs, 2, nullptr, &fn), napi_ok);
  SetGlobal("ov", fn);
  EXPECT_EQ(I32(Run("ov(7, 8)")), 15);
}

// napi_fast_wrap == napi_wrap: the native pointer round-trips via napi_unwrap.
TEST_F(NapiExtras, FallbackFastWrapUnwrap) {
  static int sentinel = 0x1234;
  static const int kTag = 0;
  napi_value obj = nullptr;
  ASSERT_EQ(napi_create_object(env_, &obj), napi_ok);
  ASSERT_EQ(napi_fast_wrap(env_, obj, &sentinel, &kTag, nullptr, nullptr, nullptr), napi_ok);
  void* out = nullptr;
  ASSERT_EQ(napi_unwrap(env_, obj, &out), napi_ok);
  EXPECT_EQ(out, &sentinel);
}

// The fast-only read helpers are inert on the fallback (no fast path exists).
TEST_F(NapiExtras, FallbackFastReadHelpersAreInert) {
  static const int kTag = 0;
  int dummy = 0;
  auto recv = reinterpret_cast<napi_fast_recv>(&dummy);
  auto val = reinterpret_cast<napi_fast_value>(&dummy);
  auto opts = reinterpret_cast<napi_fast_options>(&dummy);

  EXPECT_EQ(napi_fast_unwrap(recv, &kTag), nullptr);
  EXPECT_EQ(napi_fast_value_unwrap(val, &kTag), nullptr);
  EXPECT_FALSE(napi_fast_value_is_nullish(val));

  uint8_t scratch[64];
  void* data = nullptr;
  size_t len = 0;
  napi_fast_bs_type elem = napi_fast_bs_unknown;
  EXPECT_FALSE(napi_fast_get_buffersource(val, scratch, sizeof scratch, &data, &len, &elem));
  EXPECT_EQ(napi_fast_options_get_data(opts), nullptr);
}

}  // namespace
