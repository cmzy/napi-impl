// Shared googletest fixture for the JSC standalone-test conversions.
//
// Mirrors the NapiExtras fixture originally written inline in
// node_api_extras_gtest.cc: it drives the active backend through the public
// embedding API (napi_create_platform -> runtime -> env, plus a root handle
// scope) in SetUp/TearDown, and exposes a handful of small JS-eval helpers
// (Run/Global/Str/I32/Truthy/...). Each converted JSC test (.cc) is built as a
// standalone executable that includes this header, so there is no cross-TU ODR
// concern with the anonymous namespace below.

#ifndef NAPI_TEST_JSC_GTEST_FIXTURE_H_
#define NAPI_TEST_JSC_GTEST_FIXTURE_H_

#include <gtest/gtest.h>

#include "napi/js_native_api.h"
#include "napi_v8/embedding.h"

namespace {

class NapiExtras : public ::testing::Test {
 protected:
  napi_platform platform_ = nullptr;
  napi_runtime runtime_ = nullptr;
  napi_env env_ = nullptr;
  napi_handle_scope scope_ = nullptr;

  void SetUp() override {
    ASSERT_EQ(napi_create_platform(0, nullptr, 0, nullptr, nullptr, false, &platform_), napi_ok);
    ASSERT_EQ(napi_create_runtime(platform_, &runtime_), napi_ok);
    ASSERT_EQ(napi_create_env(runtime_, &env_), napi_ok);
    ASSERT_EQ(napi_open_handle_scope(env_, &scope_), napi_ok);
  }
  void TearDown() override {
    if (scope_) napi_close_handle_scope(env_, scope_);
    if (env_) napi_destroy_env(env_);
    if (runtime_) napi_destroy_runtime(runtime_);
    if (platform_) napi_destroy_platform(platform_);
  }

  napi_value Run(const char* code) {
    napi_value src = nullptr, res = nullptr;
    EXPECT_EQ(napi_create_string_utf8(env_, code, NAPI_AUTO_LENGTH, &src), napi_ok);
    EXPECT_EQ(napi_run_script(env_, src, &res), napi_ok);
    return res;
  }
  napi_value Global() {
    napi_value g = nullptr;
    EXPECT_EQ(napi_get_global(env_, &g), napi_ok);
    return g;
  }
  void SetGlobal(const char* name, napi_value v) { EXPECT_EQ(napi_set_named_property(env_, Global(), name, v), napi_ok); }
  napi_value Undefined() {
    napi_value v = nullptr;
    EXPECT_EQ(napi_get_undefined(env_, &v), napi_ok);
    return v;
  }
  napi_value Int(int32_t n) {
    napi_value v = nullptr;
    EXPECT_EQ(napi_create_int32(env_, n, &v), napi_ok);
    return v;
  }
  napi_value Str(const char* s) {
    napi_value v = nullptr;
    EXPECT_EQ(napi_create_string_utf8(env_, s, NAPI_AUTO_LENGTH, &v), napi_ok);
    return v;
  }
  int32_t I32(napi_value v) {
    int32_t out = 0;
    EXPECT_EQ(napi_get_value_int32(env_, v, &out), napi_ok);
    return out;
  }
  bool Truthy(napi_value v) {
    bool out = false;
    EXPECT_EQ(napi_get_value_bool(env_, v, &out), napi_ok);
    return out;
  }
};

}  // namespace

#endif  // NAPI_TEST_JSC_GTEST_FIXTURE_H_
