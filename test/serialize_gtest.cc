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

// ---------------------------------------------------------------------------
// ArrayBuffer backing-store transfer (zero-copy): napi_v8_take/adopt/free.

// Take + adopt in the same env: source detaches, adopted AB wraps the SAME bytes
// (pointer identity == no copy) and content is preserved.
TEST_F(NapiExtras, BackingStoreTransferSameEnvZeroCopy) {
    napi_value ab = Run("(() => { const a = new Uint8Array([1,2,3,4,5]); return a.buffer; })()");
    void *src_data = nullptr;
    size_t src_len = 0;
    ASSERT_EQ(napi_get_arraybuffer_info(env_, ab, &src_data, &src_len), napi_ok);
    ASSERT_EQ(src_len, 5u);
    ASSERT_NE(src_data, nullptr);

    napi_v8_backing_store store = nullptr;
    ASSERT_EQ(napi_v8_take_backing_store(env_, ab, &store), napi_ok);
    ASSERT_NE(store, nullptr);

    // Source ArrayBuffer is now detached (transfer semantics).
    bool detached = false;
    ASSERT_EQ(napi_is_detached_arraybuffer(env_, ab, &detached), napi_ok);
    EXPECT_TRUE(detached);

    napi_value ab2 = nullptr;
    ASSERT_EQ(napi_v8_adopt_backing_store(env_, store, &ab2), napi_ok);
    void *dst_data = nullptr;
    size_t dst_len = 0;
    ASSERT_EQ(napi_get_arraybuffer_info(env_, ab2, &dst_data, &dst_len), napi_ok);
    EXPECT_EQ(dst_len, 5u);
    EXPECT_EQ(dst_data, src_data);  // ZERO COPY: same bytes, not a duplicate
    SetGlobal("__ab2", ab2);
    EXPECT_TRUE(Truthy(Run("(() => { const v = new Uint8Array(__ab2); return v[0]===1 && v[4]===5; })()")));
}

// The headline worker use case: take in one isolate, adopt in a DIFFERENT
// runtime/isolate — the store is isolate-independent, so still zero-copy.
TEST_F(NapiExtras, BackingStoreTransferCrossIsolateZeroCopy) {
    napi_value ab = Run("(() => { const a = new Uint32Array([0xdeadbeef, 7, 0]); return a.buffer; })()");
    void *src_data = nullptr;
    size_t src_len = 0;
    ASSERT_EQ(napi_get_arraybuffer_info(env_, ab, &src_data, &src_len), napi_ok);
    ASSERT_EQ(src_len, 12u);

    napi_v8_backing_store store = nullptr;
    ASSERT_EQ(napi_v8_take_backing_store(env_, ab, &store), napi_ok);

    // Adopt into a fresh runtime (its own isolate) — the real cross-thread shape.
    napi_runtime rt2 = nullptr;
    ASSERT_EQ(napi_create_runtime(platform_, &rt2), napi_ok);
    napi_env env2 = nullptr;
    ASSERT_EQ(napi_create_env(rt2, &env2), napi_ok);
    napi_handle_scope scope2 = nullptr;
    ASSERT_EQ(napi_open_handle_scope(env2, &scope2), napi_ok);

    napi_value ab2 = nullptr;
    ASSERT_EQ(napi_v8_adopt_backing_store(env2, store, &ab2), napi_ok);
    void *dst_data = nullptr;
    size_t dst_len = 0;
    ASSERT_EQ(napi_get_arraybuffer_info(env2, ab2, &dst_data, &dst_len), napi_ok);
    EXPECT_EQ(dst_len, 12u);
    EXPECT_EQ(dst_data, src_data);  // zero copy across isolates
    ASSERT_EQ(napi_set_named_property(env2, [&] {
                  napi_value g = nullptr;
                  napi_get_global(env2, &g);
                  return g;
              }(),
                                      "__ab2", ab2),
              napi_ok);
    napi_value chk = nullptr, chk_src = nullptr;
    napi_create_string_utf8(env2, "(new Uint32Array(__ab2))[0] === 0xdeadbeef", NAPI_AUTO_LENGTH, &chk_src);
    ASSERT_EQ(napi_run_script(env2, chk_src, &chk), napi_ok);
    bool ok = false;
    napi_get_value_bool(env2, chk, &ok);
    EXPECT_TRUE(ok);  // bytes intact after cross-isolate adopt

    napi_close_handle_scope(env2, scope2);
    napi_destroy_env(env2);
    napi_destroy_runtime(rt2);
}

// Regression: the SOURCE isolate is destroyed BEFORE the destination. A zero-copy
// backing store records the allocator that created it and frees the bytes through
// that allocator's virtual Free() in ~BackingStore. With a PER-RUNTIME allocator,
// destroying the source runtime frees that allocator while the destination still
// owns the transferred backing; the destination's later teardown (ArrayBufferSweeper
// -> ~BackingStore) then frees through the freed allocator -> SEGV. A process-shared
// allocator keeps the pointer valid across isolate lifetimes. This is the exact
// worker->parent postMessage-transfer shape at engine shutdown (the worker isolate
// is reaped first, the main isolate is torn down after while still holding the
// transferred ArrayBuffer). Pre-fix this test SEGVs in the final destroy; post-fix
// it completes cleanly.
TEST_F(NapiExtras, BackingStoreTransferSurvivesSourceIsolateDestroyedFirst) {
    // Source runtime/isolate: create an ArrayBuffer and take its backing store.
    napi_runtime rt_src = nullptr;
    ASSERT_EQ(napi_create_runtime(platform_, &rt_src), napi_ok);
    napi_env env_src = nullptr;
    ASSERT_EQ(napi_create_env(rt_src, &env_src), napi_ok);
    napi_handle_scope scope_src = nullptr;
    ASSERT_EQ(napi_open_handle_scope(env_src, &scope_src), napi_ok);
    napi_value ab_src = nullptr;
    {
        napi_value src = nullptr;
        napi_create_string_utf8(env_src, "(() => { const a = new Uint8Array([9,8,7,6]); return a.buffer; })()",
                                NAPI_AUTO_LENGTH, &src);
        ASSERT_EQ(napi_run_script(env_src, src, &ab_src), napi_ok);
    }
    napi_v8_backing_store store = nullptr;
    ASSERT_EQ(napi_v8_take_backing_store(env_src, ab_src, &store), napi_ok);

    // Destination runtime/isolate adopts the backing (cross-isolate, zero copy).
    napi_runtime rt_dst = nullptr;
    ASSERT_EQ(napi_create_runtime(platform_, &rt_dst), napi_ok);
    napi_env env_dst = nullptr;
    ASSERT_EQ(napi_create_env(rt_dst, &env_dst), napi_ok);
    napi_handle_scope scope_dst = nullptr;
    ASSERT_EQ(napi_open_handle_scope(env_dst, &scope_dst), napi_ok);
    napi_value ab_dst = nullptr;
    ASSERT_EQ(napi_v8_adopt_backing_store(env_dst, store, &ab_dst), napi_ok);

    // Destroy the SOURCE first — frees a per-runtime allocator in the buggy build.
    napi_close_handle_scope(env_src, scope_src);
    napi_destroy_env(env_src);
    napi_destroy_runtime(rt_src);

    // The adopted buffer is still valid and readable in the destination.
    void *data = nullptr;
    size_t len = 0;
    ASSERT_EQ(napi_get_arraybuffer_info(env_dst, ab_dst, &data, &len), napi_ok);
    EXPECT_EQ(len, 4u);
    EXPECT_EQ(static_cast<uint8_t *>(data)[0], 9u);

    // Destroy the destination — its ArrayBufferSweeper frees the transferred backing
    // through the recorded allocator. Buggy: SEGV (source allocator gone). Fixed: clean.
    napi_close_handle_scope(env_dst, scope_dst);
    napi_destroy_env(env_dst);
    napi_destroy_runtime(rt_dst);
}

// A large buffer proves the win is real (no O(n) copy): still pointer-identical.
TEST_F(NapiExtras, BackingStoreTransferLargeIsZeroCopy) {
    napi_value ab = Run("new ArrayBuffer(4 * 1024 * 1024)");  // 4 MiB
    void *src_data = nullptr;
    size_t src_len = 0;
    ASSERT_EQ(napi_get_arraybuffer_info(env_, ab, &src_data, &src_len), napi_ok);
    ASSERT_EQ(src_len, 4u * 1024 * 1024);

    napi_v8_backing_store store = nullptr;
    ASSERT_EQ(napi_v8_take_backing_store(env_, ab, &store), napi_ok);
    napi_value ab2 = nullptr;
    ASSERT_EQ(napi_v8_adopt_backing_store(env_, store, &ab2), napi_ok);
    void *dst_data = nullptr;
    size_t dst_len = 0;
    ASSERT_EQ(napi_get_arraybuffer_info(env_, ab2, &dst_data, &dst_len), napi_ok);
    EXPECT_EQ(dst_data, src_data);
    EXPECT_EQ(dst_len, src_len);
}

TEST_F(NapiExtras, TakeBackingStoreRejectsNonArrayBuffer) {
    napi_v8_backing_store store = nullptr;
    EXPECT_EQ(napi_v8_take_backing_store(env_, Run("({})"), &store), napi_arraybuffer_expected);
    EXPECT_EQ(store, nullptr);
    EXPECT_EQ(napi_v8_take_backing_store(env_, Run("new Uint8Array(4)"), &store), napi_arraybuffer_expected);
}

TEST_F(NapiExtras, TakeBackingStoreRejectsSharedArrayBuffer) {
    // SAB is shared, not transferred -> rejected as not an ArrayBuffer.
    if (!Truthy(Run("typeof SharedArrayBuffer === 'function'")))
        GTEST_SKIP() << "SharedArrayBuffer not exposed in this build";
    napi_v8_backing_store store = nullptr;
    EXPECT_EQ(napi_v8_take_backing_store(env_, Run("new SharedArrayBuffer(8)"), &store), napi_arraybuffer_expected);
    EXPECT_EQ(store, nullptr);
}

TEST_F(NapiExtras, TakeBackingStoreRejectsAlreadyDetached) {
    napi_value ab = Run("new ArrayBuffer(8)");
    napi_v8_backing_store s1 = nullptr;
    ASSERT_EQ(napi_v8_take_backing_store(env_, ab, &s1), napi_ok);
    // Second take on the now-detached source is rejected.
    napi_v8_backing_store s2 = nullptr;
    EXPECT_EQ(napi_v8_take_backing_store(env_, ab, &s2), napi_detachable_arraybuffer_expected);
    EXPECT_EQ(s2, nullptr);
    napi_v8_free_backing_store(s1);
}

TEST_F(NapiExtras, TakeBackingStoreArgValidation) {
    napi_v8_backing_store store = nullptr;
    EXPECT_EQ(napi_v8_take_backing_store(env_, nullptr, &store), napi_invalid_arg);
    EXPECT_EQ(napi_v8_take_backing_store(env_, Run("new ArrayBuffer(4)"), nullptr), napi_invalid_arg);
}

TEST_F(NapiExtras, AdoptBackingStoreRejectsNull) {
    napi_value out = nullptr;
    EXPECT_EQ(napi_v8_adopt_backing_store(env_, nullptr, &out), napi_invalid_arg);
}

TEST_F(NapiExtras, FreeBackingStoreWithoutAdoptReleases) {
    // Take then free (never adopted): exercises the release path (no leak under ASan).
    napi_v8_backing_store store = nullptr;
    ASSERT_EQ(napi_v8_take_backing_store(env_, Run("new ArrayBuffer(1024)"), &store), napi_ok);
    EXPECT_EQ(napi_v8_free_backing_store(store), napi_ok);
    EXPECT_EQ(napi_v8_free_backing_store(nullptr), napi_ok);  // NULL is a no-op
}

}  // namespace
