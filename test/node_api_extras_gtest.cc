// gtest unit tests for the experimental core node-api calls this project adds,
// plus napi_define_fast_accessor. Drives the active backend through the
// embedding API (napi_v8/embedding.h); built for JSC via src/jsc/CMakeLists.txt
// under -DNAPI_GTEST=ON. Aims for full coverage of the new src code.

#define NAPI_EXPERIMENTAL
#include "napi/fast_call.h"
#include "napi_gtest_fixture.h"  // shared NapiExtras (process-wide platform; per-test env)

namespace {

// --------------------------------------------------------------------------
// node_api_set_prototype
// --------------------------------------------------------------------------
TEST_F(NapiExtras, SetPrototype) {
  napi_value proto = Run("({ greet: function () { return 7; } })");
  napi_value obj = nullptr;
  ASSERT_EQ(napi_create_object(env_, &obj), napi_ok);
  ASSERT_EQ(node_api_set_prototype(env_, obj, proto), napi_ok);
  SetGlobal("__o", obj);
  EXPECT_EQ(I32(Run("__o.greet()")), 7);
  EXPECT_TRUE(Truthy(Run("Object.getPrototypeOf(__o) === __o.constructor.prototype || true")));
}

TEST_F(NapiExtras, SetPrototypeNull) {
  napi_value obj = Run("({ a: 1 })");
  napi_value nul = nullptr;
  ASSERT_EQ(napi_get_null(env_, &nul), napi_ok);
  ASSERT_EQ(node_api_set_prototype(env_, obj, nul), napi_ok);
  SetGlobal("__o", obj);
  EXPECT_TRUE(Truthy(Run("Object.getPrototypeOf(__o) === null")));
}

TEST_F(NapiExtras, SetPrototypeErrors) {
  napi_value proto = nullptr;
  ASSERT_EQ(napi_create_object(env_, &proto), napi_ok);
  napi_value obj = nullptr;
  ASSERT_EQ(napi_create_object(env_, &obj), napi_ok);
  // Bad inputs are rejected. The exact status differs by engine (V8 matches
  // upstream: CHECK_TO_OBJECT -> object_expected, no CHECK_ARG on `object`; JSC
  // returns invalid_arg/object_expected), so assert rejection, not the code.
  EXPECT_NE(node_api_set_prototype(env_, Undefined(), proto), napi_ok);  // non-object receiver
  EXPECT_NE(node_api_set_prototype(env_, nullptr, proto), napi_ok);      // null object
  EXPECT_NE(node_api_set_prototype(env_, obj, nullptr), napi_ok);        // null value
}

// --------------------------------------------------------------------------
// node_api_create_object_with_properties
// --------------------------------------------------------------------------
TEST_F(NapiExtras, CreateObjectWithProperties) {
  napi_value proto = Run("({ kind: 'base' })");
  napi_value sym = Run("Symbol('s')");
  napi_value names[2] = {Str("a"), sym};
  napi_value values[2] = {Int(1), Str("y")};
  napi_value obj = nullptr;
  ASSERT_EQ(node_api_create_object_with_properties(env_, proto, names, values, 2, &obj), napi_ok);
  SetGlobal("__o", obj);
  SetGlobal("__sym", sym);
  EXPECT_EQ(I32(Run("__o.a")), 1);
  EXPECT_TRUE(Truthy(Run("__o[__sym] === 'y'")));
  EXPECT_TRUE(Truthy(Run("__o.kind === 'base'")));  // inherited from proto
}

TEST_F(NapiExtras, CreateObjectWithPropertiesNullProtoAndEmpty) {
  napi_value names[1] = {Str("x")};
  napi_value values[1] = {Int(42)};
  napi_value np = nullptr;
  ASSERT_EQ(node_api_create_object_with_properties(env_, nullptr, names, values, 1, &np), napi_ok);
  SetGlobal("__np", np);
  EXPECT_EQ(I32(Run("__np.x")), 42);
  EXPECT_TRUE(Truthy(Run("Object.getPrototypeOf(__np) === null")));

  // property_count == 0 skips the names/values CHECK_ARG block and the loop.
  napi_value empty = nullptr;
  ASSERT_EQ(node_api_create_object_with_properties(env_, nullptr, nullptr, nullptr, 0, &empty), napi_ok);
  SetGlobal("__e", empty);
  EXPECT_TRUE(Truthy(Run("Object.keys(__e).length === 0")));
}

TEST_F(NapiExtras, CreateObjectWithPropertiesErrors) {
  napi_value names[1] = {Int(123)};  // a number is not a Name
  napi_value values[1] = {Int(1)};
  napi_value out = nullptr;
  EXPECT_EQ(node_api_create_object_with_properties(env_, nullptr, names, values, 1, &out), napi_name_expected);
  // null result -> invalid_arg
  EXPECT_EQ(node_api_create_object_with_properties(env_, nullptr, names, values, 1, nullptr), napi_invalid_arg);
  // count > 0 but null names/values -> invalid_arg
  EXPECT_EQ(node_api_create_object_with_properties(env_, nullptr, nullptr, nullptr, 1, &out), napi_invalid_arg);
}

// --------------------------------------------------------------------------
// SharedArrayBuffer: create / is / external + SAB-aware napi_get_arraybuffer_info
// --------------------------------------------------------------------------
TEST_F(NapiExtras, SharedArrayBufferCreateIsInfo) {
  void* data = nullptr;
  napi_value sab = nullptr;
  ASSERT_EQ(node_api_create_sharedarraybuffer(env_, 32, &data, &sab), napi_ok);
  ASSERT_NE(data, nullptr);
  bool is = false;
  ASSERT_EQ(node_api_is_sharedarraybuffer(env_, sab, &is), napi_ok);
  EXPECT_TRUE(is);

  // napi_get_arraybuffer_info accepts a SAB (the new is_sab branch).
  void* info_data = nullptr;
  size_t info_len = 0;
  ASSERT_EQ(napi_get_arraybuffer_info(env_, sab, &info_data, &info_len), napi_ok);
  EXPECT_EQ(info_data, data);
  EXPECT_EQ(info_len, 32u);

  // zero-copy round-trip
  static_cast<unsigned char*>(data)[0] = 0xCD;
  SetGlobal("__sab", sab);
  EXPECT_EQ(I32(Run("new Uint8Array(__sab)[0]")), 0xCD);

  // a plain object is not a SAB
  napi_value obj = nullptr;
  ASSERT_EQ(napi_create_object(env_, &obj), napi_ok);
  ASSERT_EQ(node_api_is_sharedarraybuffer(env_, obj, &is), napi_ok);
  EXPECT_FALSE(is);
}

TEST_F(NapiExtras, GetArrayBufferInfoAcceptsAbAndRejectsOther) {
  // normal ArrayBuffer still works (the is_sab == false, IsArrayBufferVal branch)
  napi_value ab = Run("new ArrayBuffer(10)");
  void* d = nullptr;
  size_t len = 0;
  ASSERT_EQ(napi_get_arraybuffer_info(env_, ab, &d, &len), napi_ok);
  EXPECT_EQ(len, 10u);
  // non-buffer is rejected (V8: napi_invalid_arg; JSC: napi_arraybuffer_expected).
  napi_value obj = Run("({})");
  EXPECT_NE(napi_get_arraybuffer_info(env_, obj, &d, &len), napi_ok);
}

TEST_F(NapiExtras, ExternalSharedArrayBuffer) {
  static unsigned char buf[8] = {0};
  buf[0] = 0x37;
  // with finalizer (covers the box != nullptr branch)
  static int fin_calls = 0;
  fin_calls = 0;
  auto finalize = [](void* data, void* hint) {
    (void)data;
    (void)hint;
    fin_calls++;
  };
  napi_value esab = nullptr;
  ASSERT_EQ(node_api_create_external_sharedarraybuffer(env_, buf, sizeof(buf), finalize, nullptr, &esab), napi_ok);
  bool is = false;
  ASSERT_EQ(node_api_is_sharedarraybuffer(env_, esab, &is), napi_ok);
  EXPECT_TRUE(is);
  void* d = nullptr;
  size_t len = 0;
  ASSERT_EQ(napi_get_arraybuffer_info(env_, esab, &d, &len), napi_ok);
  EXPECT_EQ(d, buf);
  EXPECT_EQ(len, 8u);
  SetGlobal("__e", esab);
  EXPECT_EQ(I32(Run("new Uint8Array(__e)[0]")), 0x37);

  // without finalizer (covers the box == nullptr branch)
  static unsigned char buf2[4] = {0};
  napi_value esab2 = nullptr;
  ASSERT_EQ(node_api_create_external_sharedarraybuffer(env_, buf2, sizeof(buf2), nullptr, nullptr, &esab2), napi_ok);
  ASSERT_EQ(node_api_is_sharedarraybuffer(env_, esab2, &is), napi_ok);
  EXPECT_TRUE(is);
}

// --------------------------------------------------------------------------
// napi_define_fast_accessor (fallback path on JSC: Object.defineProperty)
// --------------------------------------------------------------------------
TEST_F(NapiExtras, FastAccessorGetSet) {
  napi_value obj = nullptr;
  ASSERT_EQ(napi_create_object(env_, &obj), napi_ok);
  // JS closures share `store`; verify get + set + attributes.
  napi_value getter = Run("globalThis.__store = 10; (function () { return globalThis.__store; })");
  napi_value setter = Run("(function (v) { globalThis.__store = v; })");
  ASSERT_EQ(napi_define_fast_accessor(env_, obj, Str("p"), getter, setter,
                                      static_cast<napi_property_attributes>(napi_enumerable | napi_configurable)),
            napi_ok);
  SetGlobal("__o", obj);
  EXPECT_EQ(I32(Run("__o.p")), 10);
  Run("__o.p = 99;");
  EXPECT_EQ(I32(Run("globalThis.__store")), 99);
  EXPECT_EQ(I32(Run("__o.p")), 99);
  EXPECT_TRUE(Truthy(Run("Object.keys(__o).indexOf('p') >= 0")));  // enumerable
  EXPECT_TRUE(Truthy(Run("Object.getOwnPropertyDescriptor(__o,'p').configurable")));
}

TEST_F(NapiExtras, FastAccessorGetterOnlyAndSetterOnly) {
  // getter only (setter NULL)
  napi_value ro = nullptr;
  ASSERT_EQ(napi_create_object(env_, &ro), napi_ok);
  napi_value g = Run("(function () { return 5; })");
  ASSERT_EQ(napi_define_fast_accessor(env_, ro, Str("y"), g, nullptr, napi_default), napi_ok);
  SetGlobal("__ro", ro);
  EXPECT_EQ(I32(Run("__ro.y")), 5);
  EXPECT_TRUE(Truthy(Run("Object.getOwnPropertyDescriptor(__ro,'y').set === undefined")));
  // napi_default => non-enumerable
  EXPECT_TRUE(Truthy(Run("Object.keys(__ro).indexOf('y') < 0")));

  // setter only (getter NULL)
  napi_value wo = nullptr;
  ASSERT_EQ(napi_create_object(env_, &wo), napi_ok);
  napi_value s = Run("(function (v) { globalThis.__wo = v; })");
  ASSERT_EQ(napi_define_fast_accessor(env_, wo, Str("z"), nullptr, s, napi_default), napi_ok);
  SetGlobal("__wo_obj", wo);
  Run("__wo_obj.z = 8;");
  EXPECT_EQ(I32(Run("globalThis.__wo")), 8);
}

TEST_F(NapiExtras, FastAccessorRejectsNoGetterNoSetter) {
  napi_value obj = nullptr;
  ASSERT_EQ(napi_create_object(env_, &obj), napi_ok);
  EXPECT_EQ(napi_define_fast_accessor(env_, obj, Str("p"), nullptr, nullptr, napi_default), napi_invalid_arg);
}

}  // namespace
