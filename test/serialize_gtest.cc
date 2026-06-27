// Tests for the napi_v8 structured-clone serialization API
// (napi_v8/serialize.h): napi_v8_serialize / _deserialize / _free_serialized_data.
// V8 ValueSerializer-backed, plain (no Delegate) — built-in types only.

#include "napi_gtest_fixture.h"

#include "napi_v8/serialize.h"

namespace {

// Round-trip `value` through serialize -> deserialize in the same env; returns the
// reconstructed clone. Asserts the buffer contract along the way.
napi_value RoundTrip(napi_env env, napi_value value) {
    uint8_t *data = nullptr;
    size_t length = 0;
    EXPECT_EQ(napi_v8_serialize(env, value, &data, &length), napi_ok);
    EXPECT_NE(data, nullptr);
    EXPECT_GT(length, 0u);
    napi_value clone = nullptr;
    EXPECT_EQ(napi_v8_deserialize(env, data, length, &clone), napi_ok);
    EXPECT_EQ(napi_v8_free_serialized_data(env, data), napi_ok);
    return clone;
}

TEST_F(NapiExtras, SerializeRoundTripsPrimitivesAndContainers) {
    napi_value original = Run(
        "({a:1, b:[1,2,3], c:'hi', d:new Map([['k',9]]), e:new Set([1,2]),"
        " f:new Date(1000), g:123n, h:true, i:null})");
    napi_value clone = RoundTrip(env_, original);
    SetGlobal("__clone", clone);
    EXPECT_TRUE(Truthy(Run(
        "__clone.a===1 && __clone.b[2]===3 && __clone.c==='hi' &&"
        " __clone.d.get('k')===9 && __clone.e.has(2) && __clone.f.getTime()===1000 &&"
        " __clone.g===123n && __clone.h===true && __clone.i===null")));
}

TEST_F(NapiExtras, SerializeProducesDeepIndependentClone) {
    napi_value original = Run("({arr:[1,2,3]})");
    napi_value clone = RoundTrip(env_, original);
    SetGlobal("__orig", original);
    SetGlobal("__clone", clone);
    Run("__orig.arr[0] = 99");  // mutate the source after cloning
    EXPECT_TRUE(Truthy(Run("__clone.arr[0] === 1")));  // clone is unaffected
}

TEST_F(NapiExtras, SerializeRoundTripsTypedArrayBytes) {
    napi_value original = Run("new Uint8Array([10, 20, 30])");
    napi_value clone = RoundTrip(env_, original);
    SetGlobal("__c", clone);
    EXPECT_TRUE(Truthy(Run(
        "__c instanceof Uint8Array && __c.length===3 && __c[0]===10 && __c[2]===30")));
}

TEST_F(NapiExtras, SerializeRoundTripsCyclicGraph) {
    napi_value original = Run("(() => { const o = {x:5}; o.self = o; return o; })()");
    napi_value clone = RoundTrip(env_, original);
    SetGlobal("__c", clone);
    // Structured clone preserves the self-reference (identity within the graph).
    EXPECT_TRUE(Truthy(Run("__c.self === __c && __c.x === 5")));
}

TEST_F(NapiExtras, SerializeUncloneableValueThrowsDataClone) {
    napi_value fn = Run("(function f(){})");
    uint8_t *data = reinterpret_cast<uint8_t *>(0x1);  // poison: must be left untouched
    size_t length = 7;
    EXPECT_EQ(napi_v8_serialize(env_, fn, &data, &length), napi_pending_exception);
    EXPECT_EQ(data, nullptr);
    EXPECT_EQ(length, 0u);
    // Clear the pending exception so the fixture's teardown / later tests are clean.
    napi_value ex = nullptr;
    EXPECT_EQ(napi_get_and_clear_last_exception(env_, &ex), napi_ok);
}

TEST_F(NapiExtras, FreeNullIsNoOp) {
    EXPECT_EQ(napi_v8_free_serialized_data(env_, nullptr), napi_ok);
}

TEST_F(NapiExtras, ArgValidationRejectsNullPointers) {
    napi_value v = Run("({a:1})");
    uint8_t *data = nullptr;
    size_t length = 0;
    // serialize: a null out-pointer is rejected before any work.
    EXPECT_EQ(napi_v8_serialize(env_, v, nullptr, &length), napi_invalid_arg);
    EXPECT_EQ(napi_v8_serialize(env_, v, &data, nullptr), napi_invalid_arg);
    // deserialize: null bytes / null result are rejected.
    const uint8_t dummy[1] = {0};
    napi_value out = nullptr;
    EXPECT_EQ(napi_v8_deserialize(env_, nullptr, 0, &out), napi_invalid_arg);
    EXPECT_EQ(napi_v8_deserialize(env_, dummy, sizeof(dummy), nullptr), napi_invalid_arg);
}

TEST_F(NapiExtras, DeserializeRejectsMalformedBytes) {
    const uint8_t garbage[] = {0x00, 0x01, 0x02, 0x03};
    napi_value result = reinterpret_cast<napi_value>(0x1);  // poison
    EXPECT_EQ(napi_v8_deserialize(env_, garbage, sizeof(garbage), &result),
              napi_invalid_arg);
    EXPECT_EQ(result, nullptr);
}

// The headline use case: serialize in one env, reconstruct in a DIFFERENT env on
// the same runtime (the worker-postMessage shape; the wire format is portable).
TEST_F(NapiExtras, DeserializeInAnotherEnv) {
    napi_value original = Run("({n:42, s:'x'})");
    uint8_t *data = nullptr;
    size_t length = 0;
    ASSERT_EQ(napi_v8_serialize(env_, original, &data, &length), napi_ok);

    napi_env env2 = nullptr;
    ASSERT_EQ(napi_create_env(runtime_, &env2), napi_ok);
    napi_handle_scope scope2 = nullptr;
    ASSERT_EQ(napi_open_handle_scope(env2, &scope2), napi_ok);

    napi_value clone = nullptr;
    EXPECT_EQ(napi_v8_deserialize(env2, data, length, &clone), napi_ok);
    EXPECT_EQ(napi_v8_free_serialized_data(env_, data), napi_ok);

    napi_value n = nullptr;
    ASSERT_EQ(napi_get_named_property(env2, clone, "n", &n), napi_ok);
    int32_t n_val = 0;
    EXPECT_EQ(napi_get_value_int32(env2, n, &n_val), napi_ok);
    EXPECT_EQ(n_val, 42);

    napi_close_handle_scope(env2, scope2);
    napi_destroy_env(env2);
}

}  // namespace
