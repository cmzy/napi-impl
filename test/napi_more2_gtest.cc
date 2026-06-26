// Deeper core coverage: get_new_target, property keys, adjust_external_memory,
// property-descriptor attributes (the attrs mapping), exception forwarding via
// napi_call_function, string length-query/truncation, arraybuffer-with-data, and
// napi_get_cb_info with more args than the caller's buffer.

#define NAPI_VERSION 10
#define NAPI_EXPERIMENTAL
#include "napi_gtest_fixture.h"

namespace {

bool g_was_ctor = false;
napi_value probe_ctor(napi_env env, napi_callback_info info) {
  napi_value nt = nullptr;
  napi_get_new_target(env, info, &nt);
  g_was_ctor = (nt != nullptr);
  napi_value undef = nullptr;
  napi_get_undefined(env, &undef);
  return undef;
}

napi_value throwing_fn(napi_env env, napi_callback_info info) {
  (void)info;
  napi_throw_error(env, "ERR", "boom");
  return nullptr;
}

// get_cb_info called with a 1-slot argv but more actual args: argc is rewritten
// to the real count (covers the "extra args" copy path).
napi_value cbinfo_probe(napi_env env, napi_callback_info info) {
  size_t argc = 1;
  napi_value argv[1];
  napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
  napi_value r = nullptr;
  napi_create_uint32(env, static_cast<uint32_t>(argc), &r);
  return r;
}

TEST_F(NapiExtras, GetNewTarget) {
  // `new` path: napi_define_class makes a constructable function (plain
  // napi_create_function functions are not constructors).
  napi_value cls = nullptr;
  ASSERT_EQ(napi_define_class(env_, "Cls", NAPI_AUTO_LENGTH, probe_ctor, nullptr, 0, nullptr, &cls), napi_ok);
  SetGlobal("Cls", cls);
  g_was_ctor = false;
  Run("new Cls()");
  EXPECT_TRUE(g_was_ctor) << "new.target is set under `new`";

  // plain-call path: a regular function called without `new` -> null new.target.
  napi_value fn = nullptr;
  ASSERT_EQ(napi_create_function(env_, "fn", NAPI_AUTO_LENGTH, probe_ctor, nullptr, &fn), napi_ok);
  SetGlobal("fn", fn);
  g_was_ctor = true;
  Run("fn()");
  EXPECT_FALSE(g_was_ctor) << "new.target is null under a plain call";
}

TEST_F(NapiExtras, PropertyKeys) {
  napi_value k = nullptr;
  ASSERT_EQ(node_api_create_property_key_utf8(env_, "pk", NAPI_AUTO_LENGTH, &k), napi_ok);
  napi_value obj = nullptr;
  ASSERT_EQ(napi_create_object(env_, &obj), napi_ok);
  ASSERT_EQ(napi_set_property(env_, obj, k, Int(9)), napi_ok);
  napi_value got = nullptr;
  ASSERT_EQ(napi_get_property(env_, obj, k, &got), napi_ok);
  EXPECT_EQ(I32(got), 9);

  napi_value k2 = nullptr;
  ASSERT_EQ(node_api_create_property_key_utf16(env_, u"pk2", NAPI_AUTO_LENGTH, &k2), napi_ok);
  napi_valuetype t;
  ASSERT_EQ(napi_typeof(env_, k2, &t), napi_ok);
  EXPECT_EQ(t, napi_string);
}

TEST_F(NapiExtras, AdjustExternalMemory) {
  int64_t adjusted = -1;
  ASSERT_EQ(napi_adjust_external_memory(env_, 4096, &adjusted), napi_ok);
  EXPECT_GE(adjusted, 0);
  ASSERT_EQ(napi_adjust_external_memory(env_, -4096, &adjusted), napi_ok);
}

TEST_F(NapiExtras, DefinePropertiesWithAttributes) {
  napi_value obj = nullptr;
  ASSERT_EQ(napi_create_object(env_, &obj), napi_ok);
  napi_property_descriptor descs[2] = {};
  descs[0].utf8name = "ro";
  descs[0].value = Int(1);
  descs[0].attributes = napi_enumerable;  // not writable, not configurable
  descs[1].utf8name = "rw";
  descs[1].value = Int(2);
  descs[1].attributes =
      static_cast<napi_property_attributes>(napi_writable | napi_enumerable | napi_configurable);
  ASSERT_EQ(napi_define_properties(env_, obj, 2, descs), napi_ok);
  SetGlobal("__o", obj);
  EXPECT_EQ(I32(Run("__o.ro")), 1);
  EXPECT_EQ(I32(Run("__o.rw")), 2);
  EXPECT_TRUE(Truthy(Run("Object.getOwnPropertyDescriptor(__o,'ro').writable === false")));
  EXPECT_TRUE(Truthy(Run("Object.getOwnPropertyDescriptor(__o,'rw').configurable === true")));
}

TEST_F(NapiExtras, CallFunctionForwardsException) {
  napi_value fn = nullptr;
  ASSERT_EQ(napi_create_function(env_, "boom", NAPI_AUTO_LENGTH, throwing_fn, nullptr, &fn), napi_ok);
  napi_value ret = nullptr;
  EXPECT_EQ(napi_call_function(env_, Global(), fn, 0, nullptr, &ret), napi_pending_exception);
  bool pending = false;
  ASSERT_EQ(napi_is_exception_pending(env_, &pending), napi_ok);
  EXPECT_TRUE(pending);
  napi_value e = nullptr;
  ASSERT_EQ(napi_get_and_clear_last_exception(env_, &e), napi_ok);
}

TEST_F(NapiExtras, StringUtf8LengthAndTruncate) {
  napi_value s = Str("hello");
  size_t len = 0;
  ASSERT_EQ(napi_get_value_string_utf8(env_, s, nullptr, 0, &len), napi_ok);  // length query
  EXPECT_EQ(len, 5u);
  char buf[3] = {0};
  size_t copied = 0;
  ASSERT_EQ(napi_get_value_string_utf8(env_, s, buf, sizeof(buf), &copied), napi_ok);  // truncates
  EXPECT_EQ(copied, 2u);
  EXPECT_STREQ(buf, "he");
}

TEST_F(NapiExtras, CreateArrayBufferWithData) {
  void* data = nullptr;
  napi_value ab = nullptr;
  ASSERT_EQ(napi_create_arraybuffer(env_, 16, &data, &ab), napi_ok);
  ASSERT_NE(data, nullptr);
  static_cast<unsigned char*>(data)[0] = 0x7E;
  SetGlobal("__ab", ab);
  EXPECT_EQ(I32(Run("new Uint8Array(__ab)[0]")), 0x7E);
}

TEST_F(NapiExtras, CbInfoExtraArgs) {
  napi_value fn = nullptr;
  ASSERT_EQ(napi_create_function(env_, "probe", NAPI_AUTO_LENGTH, cbinfo_probe, nullptr, &fn), napi_ok);
  SetGlobal("probe", fn);
  EXPECT_EQ(I32(Run("probe(1,2,3,4)")), 4);  // get_cb_info reports the real argc
}

}  // namespace
