// gtest conversion of the weak-reference / finalizer GC test for the JSC backend
// (formerly a bespoke main()+CHECK/EXPECT harness).
//
// White-box: includes the backend's internal header to reach env->ctx so it can
// force collection via JSGarbageCollect (there is no public "collect now" in the
// embedding API). Verifies the three weak-ref behaviours:
//   A. napi_wrap finalizer runs after the wrapped object is collected, and the
//      returned (weak) reference then reads back empty.
//   B. a strong reference (refcount 1) keeps its target alive across GC; after
//      napi_reference_unref the target becomes collectible again.
//   C. napi_create_reference(value, 0) is weak: empties after collection.
//
// JSC's collector is *conservative* over the C stack, so a target is only
// collectible once no JSValueRef to it remains in a live stack frame. Two rules
// keep this deterministic: (1) allocate each target in a `noinline` helper that
// returns only the napi_ref — the target pointer dies with that popped frame,
// below the stack pointer GC scans; (2) every retrieval runs in its own handle
// scope that closes immediately, so it never pins the target into a long-lived
// scope.

#include <cstdint>

#include "js_native_api_jsc.h"  // internal: napi_env__ layout + JSGarbageCollect

#include "napi_gtest_fixture.h"

namespace {

int g_finalized = 0;
void on_finalize(napi_env, void* /*data*/, void* /*hint*/) { g_finalized += 1; }

// Overwrite the stack region a prior retrieval may have left a target pointer
// in, so JSC's conservative stack scan can't falsely keep it alive.
__attribute__((noinline)) void clobber_stack() {
  volatile long buf[4096];
  for (int i = 0; i < 4096; ++i)
    buf[i] = i;
  (void)buf;
}

// Force collection: a few full GCs with allocation pressure in between, so the
// wrapped object and its (second-level) holder are both swept. Allocations run
// in a scope that closes so they don't pin into the root scope.
void force_gc(napi_env env) {
  clobber_stack();
  for (int i = 0; i < 6; ++i) {
    napi_handle_scope s = nullptr;
    napi_open_handle_scope(env, &s);
    napi_value src = nullptr, res = nullptr;
    if (napi_create_string_utf8(env, "(()=>{let a=[];for(let i=0;i<20000;i++)a.push({});return a.length})()",
                                NAPI_AUTO_LENGTH, &src) == napi_ok)
      napi_run_script(env, src, &res);
    napi_close_handle_scope(env, s);
    JSGarbageCollect(env->ctx);
  }
  napi_v8_run_event_loop_tasks(env);  // drain deferred finalizers
}

// Read a reference inside its own (immediately closed) scope so the retrieved
// value is never pinned into the root scope.
bool ref_is_empty(napi_env env, napi_ref ref) {
  napi_handle_scope s = nullptr;
  napi_open_handle_scope(env, &s);
  napi_value v = reinterpret_cast<napi_value>(0x1);  // sentinel
  napi_get_reference_value(env, ref, &v);
  napi_close_handle_scope(env, s);
  return v == nullptr;
}

int g_payload = 7;

// Allocate + wrap a fresh object; return only the (weak) ref. The object lives
// solely in this popped frame, so after return it is collectible.
__attribute__((noinline)) napi_ref make_wrapped(napi_env env) {
  napi_handle_scope scope = nullptr;
  napi_open_handle_scope(env, &scope);
  napi_value obj = nullptr;
  napi_create_object(env, &obj);
  napi_ref ref = nullptr;
  napi_wrap(env, obj, &g_payload, on_finalize, nullptr, &ref);
  napi_close_handle_scope(env, scope);
  return ref;
}

__attribute__((noinline)) napi_ref make_ref(napi_env env, uint32_t count) {
  napi_handle_scope scope = nullptr;
  napi_open_handle_scope(env, &scope);
  napi_value obj = nullptr;
  napi_create_object(env, &obj);
  napi_ref ref = nullptr;
  napi_create_reference(env, obj, count, &ref);
  napi_close_handle_scope(env, scope);
  return ref;
}

// Like make_ref but the target is Object.freeze'd (non-extensible) before the
// reference is taken — the weak-ref holder cannot be attached to it. Models a
// frozen [SameObject] attribute (e.g. WebCrypto CryptoKey.algorithm) held only
// by a napi_ref.
__attribute__((noinline)) napi_ref make_ref_frozen(napi_env env, uint32_t count) {
  napi_handle_scope scope = nullptr;
  napi_open_handle_scope(env, &scope);
  napi_value obj = nullptr;
  napi_create_object(env, &obj);
  napi_object_freeze(env, obj);
  napi_ref ref = nullptr;
  napi_create_reference(env, obj, count, &ref);
  napi_close_handle_scope(env, scope);
  return ref;
}

// Attach a finalizer via napi_add_finalizer (not napi_wrap); object lives only
// in this popped frame so it is collectible after return.
__attribute__((noinline)) napi_ref make_with_finalizer(napi_env env) {
  napi_handle_scope scope = nullptr;
  napi_open_handle_scope(env, &scope);
  napi_value obj = nullptr;
  napi_create_object(env, &obj);
  napi_ref ref = nullptr;
  napi_add_finalizer(env, obj, &g_payload, on_finalize, nullptr, &ref);
  napi_close_handle_scope(env, scope);
  return ref;
}

// --- A. wrap finalizer + weak ref ----------------------------------------
// Note: we must NOT retrieve the ref before forcing GC — doing so would leave
// the target pointer in a live stack frame and JSC's conservative collector
// would refuse to collect it. The finalizer not having run yet is our
// (non-pinning) liveness proxy.
TEST_F(NapiExtras, WrapFinalizerAndWeakRef) {
  g_finalized = 0;
  napi_ref wrap_ref = make_wrapped(env_);
  EXPECT_TRUE(g_finalized == 0) << "A: finalizer must not run before collection";
  force_gc(env_);
  EXPECT_TRUE(g_finalized == 1) << "A: wrap finalizer should have run exactly once after GC";
  EXPECT_TRUE(ref_is_empty(env_, wrap_ref)) << "A: weak wrap ref should read empty after collection";
  ASSERT_EQ(napi_delete_reference(env_, wrap_ref), napi_ok);
}

// --- B2. strong reference to a FROZEN object survives GC ------------------
// Regression for AME-JSC-FROZEN-REF-FIX: a refcount-1 ref to a non-extensible
// (Object.freeze) target must survive GC just like any other object. The weak-ref
// holder can't be attached to a frozen target — JSC's [[DefineOwnProperty]]
// silently rejects it *without* setting an exception. If AttachHolder wrongly
// reports success, the holder is orphaned (unreferenced → GC'd → its finalizer
// clears the RefControl), so the strong ref reads back NULL despite JSValueProtect.
// Surfaced by WebCrypto: CryptoKey.algorithm is a frozen [SameObject] object held
// only by a napi_ref; long-lived keys read .algorithm == null after GC pressure.
TEST_F(NapiExtras, StrongReferenceToFrozenObjectSurvivesGC) {
  napi_ref strong = make_ref_frozen(env_, 1);  // refcount 1 => strong, target frozen
  force_gc(env_);
  EXPECT_TRUE(!ref_is_empty(env_, strong)) << "B2: refcount-1 ref to a frozen object must survive GC";
  ASSERT_EQ(napi_delete_reference(env_, strong), napi_ok);
}

// --- B. strong reference pins across GC, then releases -------------------
TEST_F(NapiExtras, StrongReferencePinsThenReleases) {
  napi_ref strong = make_ref(env_, 1);  // refcount 1 => strong
  force_gc(env_);
  EXPECT_TRUE(!ref_is_empty(env_, strong)) << "B: refcount-1 reference must survive GC";
  {
    uint32_t cnt = 99;
    ASSERT_EQ(napi_reference_unref(env_, strong, &cnt), napi_ok);  // 1 -> 0, now weak
    EXPECT_TRUE(cnt == 0) << "B: unref should report count 0";
  }
  force_gc(env_);
  EXPECT_TRUE(ref_is_empty(env_, strong)) << "B: after unref to 0 the target should be collected";
  ASSERT_EQ(napi_delete_reference(env_, strong), napi_ok);
}

// --- C. create_reference(value, 0) is weak -------------------------------
TEST_F(NapiExtras, WeakReferenceEmptiesAfterGc) {
  napi_ref weak = make_ref(env_, 0);  // refcount 0 => weak
  force_gc(env_);
  EXPECT_TRUE(ref_is_empty(env_, weak)) << "C: weak reference should read empty after collection";
  ASSERT_EQ(napi_delete_reference(env_, weak), napi_ok);
}

// --- D. napi_add_finalizer runs once after the object is collected --------
TEST_F(NapiExtras, AddFinalizerRunsAfterGc) {
  g_finalized = 0;
  napi_ref ref = make_with_finalizer(env_);
  EXPECT_TRUE(g_finalized == 0) << "D: add_finalizer must not run before collection";
  force_gc(env_);
  EXPECT_TRUE(g_finalized == 1) << "D: add_finalizer should have run exactly once after GC";
  ASSERT_EQ(napi_delete_reference(env_, ref), napi_ok);
}

}  // namespace
