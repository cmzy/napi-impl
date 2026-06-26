// gtest for the object/property/lifecycle napi_* surface the conformance suite
// leaves uncovered on the JSC backend: the property + element family, promises,
// dates, instances/instanceof, references, externals, finalizers, freeze/seal,
// and the error throw/create helpers. Engine-neutral (shared fixture).

// node_api_*_syntax_error are gated on NAPI_VERSION >= 9 in js_native_api.h.
#define NAPI_VERSION 10
#include "napi_gtest_fixture.h"

namespace {

// ---- property family (by napi_value key) -----------------------------------
TEST_F(NapiExtras, PropertyByKey) {
  napi_value obj = nullptr;
  ASSERT_EQ(napi_create_object(env_, &obj), napi_ok);
  napi_value key = Str("k");
  ASSERT_EQ(napi_set_property(env_, obj, key, Int(42)), napi_ok);
  bool has = false;
  ASSERT_EQ(napi_has_property(env_, obj, key, &has), napi_ok);
  EXPECT_TRUE(has);
  ASSERT_EQ(napi_has_own_property(env_, obj, key, &has), napi_ok);
  EXPECT_TRUE(has);
  napi_value got = nullptr;
  ASSERT_EQ(napi_get_property(env_, obj, key, &got), napi_ok);
  EXPECT_EQ(I32(got), 42);
  ASSERT_EQ(napi_delete_property(env_, obj, key, &has), napi_ok);
  EXPECT_TRUE(has);
  ASSERT_EQ(napi_has_property(env_, obj, key, &has), napi_ok);
  EXPECT_FALSE(has);

  napi_value names = nullptr;
  ASSERT_EQ(napi_set_property(env_, obj, Str("a"), Int(1)), napi_ok);
  ASSERT_EQ(napi_get_property_names(env_, obj, &names), napi_ok);
  uint32_t n = 0;
  ASSERT_EQ(napi_get_array_length(env_, names, &n), napi_ok);
  EXPECT_EQ(n, 1u);
}

// ---- element family (by index) ---------------------------------------------
TEST_F(NapiExtras, ElementFamily) {
  napi_value arr = Run("[]");
  ASSERT_EQ(napi_set_element(env_, arr, 0, Int(10)), napi_ok);
  bool has = false;
  ASSERT_EQ(napi_has_element(env_, arr, 0, &has), napi_ok);
  EXPECT_TRUE(has);
  napi_value got = nullptr;
  ASSERT_EQ(napi_get_element(env_, arr, 0, &got), napi_ok);
  EXPECT_EQ(I32(got), 10);
  ASSERT_EQ(napi_delete_element(env_, arr, 0, &has), napi_ok);
  EXPECT_TRUE(has);
}

// ---- instanceof + new_instance ---------------------------------------------
TEST_F(NapiExtras, InstanceofAndNewInstance) {
  napi_value ctor = Run("(function Foo(){ this.x = 1; })");
  napi_value inst = nullptr;
  ASSERT_EQ(napi_new_instance(env_, ctor, 0, nullptr, &inst), napi_ok);
  bool is = false;
  ASSERT_EQ(napi_instanceof(env_, inst, ctor, &is), napi_ok);
  EXPECT_TRUE(is);
  napi_value x = nullptr;
  ASSERT_EQ(napi_get_named_property(env_, inst, "x", &x), napi_ok);
  EXPECT_EQ(I32(x), 1);
}

// ---- promises --------------------------------------------------------------
TEST_F(NapiExtras, PromiseResolveAndReject) {
  napi_deferred d = nullptr;
  napi_value p = nullptr;
  ASSERT_EQ(napi_create_promise(env_, &d, &p), napi_ok);
  bool is = false;
  ASSERT_EQ(napi_is_promise(env_, p, &is), napi_ok);
  EXPECT_TRUE(is);
  ASSERT_EQ(napi_resolve_deferred(env_, d, Int(5)), napi_ok);

  // reject path (SettleDeferred's other branch)
  napi_deferred d2 = nullptr;
  napi_value p2 = nullptr;
  ASSERT_EQ(napi_create_promise(env_, &d2, &p2), napi_ok);
  ASSERT_EQ(napi_reject_deferred(env_, d2, Str("nope")), napi_ok);
}

// ---- dates -----------------------------------------------------------------
TEST_F(NapiExtras, Dates) {
  napi_value d = nullptr;
  ASSERT_EQ(napi_create_date(env_, 1000.0, &d), napi_ok);
  bool is = false;
  ASSERT_EQ(napi_is_date(env_, d, &is), napi_ok);
  EXPECT_TRUE(is);
  double ms = 0;
  ASSERT_EQ(napi_get_date_value(env_, d, &ms), napi_ok);
  EXPECT_EQ(ms, 1000.0);
}

// ---- references ------------------------------------------------------------
TEST_F(NapiExtras, References) {
  napi_value obj = Run("({ v: 3 })");
  napi_ref ref = nullptr;
  ASSERT_EQ(napi_create_reference(env_, obj, 1, &ref), napi_ok);
  uint32_t cnt = 0;
  ASSERT_EQ(napi_reference_ref(env_, ref, &cnt), napi_ok);
  EXPECT_EQ(cnt, 2u);
  ASSERT_EQ(napi_reference_unref(env_, ref, &cnt), napi_ok);
  EXPECT_EQ(cnt, 1u);
  napi_value got = nullptr;
  ASSERT_EQ(napi_get_reference_value(env_, ref, &got), napi_ok);
  napi_value v = nullptr;
  ASSERT_EQ(napi_get_named_property(env_, got, "v", &v), napi_ok);
  EXPECT_EQ(I32(v), 3);
  ASSERT_EQ(napi_delete_reference(env_, ref), napi_ok);
}

// ---- external --------------------------------------------------------------
TEST_F(NapiExtras, External) {
  static int payload = 0xBEEF;
  napi_value ext = nullptr;
  ASSERT_EQ(napi_create_external(env_, &payload, nullptr, nullptr, &ext), napi_ok);
  napi_valuetype t;
  ASSERT_EQ(napi_typeof(env_, ext, &t), napi_ok);
  EXPECT_EQ(t, napi_external);
  void* out = nullptr;
  ASSERT_EQ(napi_get_value_external(env_, ext, &out), napi_ok);
  EXPECT_EQ(out, &payload);
}

// ---- wrap / remove_wrap -----------------------------------------------------
// (napi_add_finalizer + its env-teardown drain is covered by jsc_weakref, which
// forces GC first; registering one here and tearing the env down immediately
// trips the finalizer mutex.)
TEST_F(NapiExtras, WrapRemoveWrap) {
  static int native = 11;
  napi_value obj = nullptr;
  ASSERT_EQ(napi_create_object(env_, &obj), napi_ok);
  ASSERT_EQ(napi_wrap(env_, obj, &native, nullptr, nullptr, nullptr), napi_ok);
  void* got = nullptr;
  ASSERT_EQ(napi_unwrap(env_, obj, &got), napi_ok);
  EXPECT_EQ(got, &native);
  void* removed = nullptr;
  ASSERT_EQ(napi_remove_wrap(env_, obj, &removed), napi_ok);
  EXPECT_EQ(removed, &native);
}

// ---- freeze / seal ---------------------------------------------------------
TEST_F(NapiExtras, FreezeAndSeal) {
  napi_value f = Run("({ a: 1 })");
  ASSERT_EQ(napi_object_freeze(env_, f), napi_ok);
  SetGlobal("__f", f);
  EXPECT_TRUE(Truthy(Run("Object.isFrozen(__f)")));

  napi_value s = Run("({ b: 2 })");
  ASSERT_EQ(napi_object_seal(env_, s), napi_ok);
  SetGlobal("__s", s);
  EXPECT_TRUE(Truthy(Run("Object.isSealed(__s)")));
}

// ---- error throw / create helpers (drives ThrowNamed + CreateError) --------
TEST_F(NapiExtras, ErrorHelpers) {
  auto clear = [&] {
    napi_value e = nullptr;
    napi_get_and_clear_last_exception(env_, &e);
  };
  EXPECT_EQ(napi_throw_error(env_, "E1", "msg"), napi_ok);
  clear();
  EXPECT_EQ(napi_throw_type_error(env_, "E2", "type"), napi_ok);
  clear();
  EXPECT_EQ(napi_throw_range_error(env_, "E3", "range"), napi_ok);
  clear();
  EXPECT_EQ(node_api_throw_syntax_error(env_, "E4", "syntax"), napi_ok);
  clear();

  napi_value err = nullptr;
  bool is = false;
  ASSERT_EQ(napi_create_type_error(env_, nullptr, Str("t"), &err), napi_ok);
  ASSERT_EQ(napi_is_error(env_, err, &is), napi_ok);
  EXPECT_TRUE(is);
  ASSERT_EQ(napi_create_range_error(env_, nullptr, Str("r"), &err), napi_ok);
  ASSERT_EQ(napi_is_error(env_, err, &is), napi_ok);
  EXPECT_TRUE(is);
  ASSERT_EQ(node_api_create_syntax_error(env_, nullptr, Str("s"), &err), napi_ok);
  ASSERT_EQ(napi_is_error(env_, err, &is), napi_ok);
  EXPECT_TRUE(is);
}

}  // namespace
