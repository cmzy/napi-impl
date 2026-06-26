// Further core-surface coverage: has_named_property, get_prototype, the array
// helpers, external strings, and node_api_post_finalizer — all common calls the
// conformance suite + earlier gtests happen to miss on this backend.

#define NAPI_VERSION 10        // node_api_create_external_string_* are >= 10
#define NAPI_EXPERIMENTAL      // node_api_post_finalizer
#include "napi_gtest_fixture.h"

namespace {

TEST_F(NapiExtras, HasNamedProperty) {
  napi_value obj = Run("({ k: 1 })");
  bool has = false;
  ASSERT_EQ(napi_has_named_property(env_, obj, "k", &has), napi_ok);
  EXPECT_TRUE(has);
  ASSERT_EQ(napi_has_named_property(env_, obj, "nope", &has), napi_ok);
  EXPECT_FALSE(has);
}

TEST_F(NapiExtras, GetPrototype) {
  napi_value arr = Run("[]");
  napi_value proto = nullptr;
  ASSERT_EQ(napi_get_prototype(env_, arr, &proto), napi_ok);
  SetGlobal("__p", proto);
  EXPECT_TRUE(Truthy(Run("__p === Array.prototype")));
}

TEST_F(NapiExtras, Arrays) {
  napi_value a = nullptr;
  ASSERT_EQ(napi_create_array(env_, &a), napi_ok);
  bool is = false;
  ASSERT_EQ(napi_is_array(env_, a, &is), napi_ok);
  EXPECT_TRUE(is);

  napi_value a2 = nullptr;
  ASSERT_EQ(napi_create_array_with_length(env_, 5, &a2), napi_ok);
  uint32_t len = 0;
  ASSERT_EQ(napi_get_array_length(env_, a2, &len), napi_ok);
  EXPECT_EQ(len, 5u);

  ASSERT_EQ(napi_is_array(env_, Run("({})"), &is), napi_ok);
  EXPECT_FALSE(is);
}

TEST_F(NapiExtras, ExternalStrings) {
  static char latin1[] = "external-latin1";
  napi_value s = nullptr;
  bool copied = false;
  ASSERT_EQ(node_api_create_external_string_latin1(env_, latin1, NAPI_AUTO_LENGTH, nullptr, nullptr, &s, &copied),
            napi_ok);
  char buf[32] = {0};
  size_t n = 0;
  ASSERT_EQ(napi_get_value_string_utf8(env_, s, buf, sizeof(buf), &n), napi_ok);
  EXPECT_STREQ(buf, "external-latin1");

  static char16_t u16[] = u"external-utf16";
  napi_value s2 = nullptr;
  ASSERT_EQ(node_api_create_external_string_utf16(env_, u16, NAPI_AUTO_LENGTH, nullptr, nullptr, &s2, &copied), napi_ok);
  napi_valuetype t;
  ASSERT_EQ(napi_typeof(env_, s2, &t), napi_ok);
  EXPECT_EQ(t, napi_string);
}

TEST_F(NapiExtras, PostFinalizer) {
  static int ran = 0;
  ran = 0;
  ASSERT_EQ(node_api_post_finalizer(
                env_, [](napi_env, void* data, void*) { ++*static_cast<int*>(data); }, &ran, nullptr),
            napi_ok);
  ASSERT_EQ(napi_v8_run_event_loop_tasks(env_), napi_ok);  // drains the deferred finalizer queue
  EXPECT_EQ(ran, 1);
}

}  // namespace
