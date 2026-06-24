// Weak-reference / finalizer GC test for the JSC backend.
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
// scope that closes immediately, so it never pins the target into the long-lived
// root scope.

#include <cstdio>

#include "js_native_api_jsc.h"   // internal: napi_env__ layout + JSGarbageCollect
#include "napi_v8/embedding.h"

#define CHECK(expr)                                                                                                    \
    do {                                                                                                              \
        napi_status _s = (expr);                                                                                      \
        if (_s != napi_ok) {                                                                                          \
            std::fprintf(stderr, "FAIL: %s -> status %d\n", #expr, (int)_s);                                         \
            return 1;                                                                                                 \
        }                                                                                                             \
    } while (0)

#define EXPECT(cond, msg)                                                                                              \
    do {                                                                                                              \
        if (!(cond)) {                                                                                                \
            std::fprintf(stderr, "FAIL: %s\n", msg);                                                                  \
            return 1;                                                                                                 \
        }                                                                                                             \
    } while (0)

static int g_finalized = 0;
static void on_finalize(napi_env, void* /*data*/, void* /*hint*/) { g_finalized += 1; }
static void on_error(const char* msg) { std::fprintf(stderr, "[engine error] %s\n", msg ? msg : "(null)"); }

// Overwrite the stack region a prior retrieval may have left a target pointer
// in, so JSC's conservative stack scan can't falsely keep it alive.
__attribute__((noinline)) static void clobber_stack() {
    volatile long buf[4096];
    for (int i = 0; i < 4096; ++i)
        buf[i] = i;
    (void)buf;
}

// Force collection: a few full GCs with allocation pressure in between, so the
// wrapped object and its (second-level) holder are both swept. Allocations run
// in a scope that closes so they don't pin into the root scope.
static void force_gc(napi_env env) {
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
static bool ref_is_empty(napi_env env, napi_ref ref) {
    napi_handle_scope s = nullptr;
    napi_open_handle_scope(env, &s);
    napi_value v = reinterpret_cast<napi_value>(0x1);  // sentinel
    napi_get_reference_value(env, ref, &v);
    napi_close_handle_scope(env, s);
    return v == nullptr;
}

static int g_payload = 7;

// Allocate + wrap a fresh object; return only the (weak) ref. The object lives
// solely in this popped frame, so after return it is collectible.
__attribute__((noinline)) static napi_ref make_wrapped(napi_env env) {
    napi_handle_scope scope = nullptr;
    napi_open_handle_scope(env, &scope);
    napi_value obj = nullptr;
    napi_create_object(env, &obj);
    napi_ref ref = nullptr;
    napi_wrap(env, obj, &g_payload, on_finalize, nullptr, &ref);
    napi_close_handle_scope(env, scope);
    return ref;
}

__attribute__((noinline)) static napi_ref make_ref(napi_env env, uint32_t count) {
    napi_handle_scope scope = nullptr;
    napi_open_handle_scope(env, &scope);
    napi_value obj = nullptr;
    napi_create_object(env, &obj);
    napi_ref ref = nullptr;
    napi_create_reference(env, obj, count, &ref);
    napi_close_handle_scope(env, scope);
    return ref;
}

int main() {
    napi_platform platform = nullptr;
    napi_runtime runtime = nullptr;
    napi_env env = nullptr;
    CHECK(napi_create_platform(0, nullptr, 0, nullptr, on_error, false, &platform));
    CHECK(napi_create_runtime(platform, &runtime));
    CHECK(napi_create_env(runtime, &env));

    // --- A. wrap finalizer + weak ref ------------------------------------
    // Note: we must NOT retrieve the ref before forcing GC — doing so would
    // leave the target pointer in a live stack frame and JSC's conservative
    // collector would refuse to collect it. The finalizer not having run yet is
    // our (non-pinning) liveness proxy.
    napi_ref wrap_ref = make_wrapped(env);
    EXPECT(g_finalized == 0, "A: finalizer must not run before collection");
    force_gc(env);
    EXPECT(g_finalized == 1, "A: wrap finalizer should have run exactly once after GC");
    EXPECT(ref_is_empty(env, wrap_ref), "A: weak wrap ref should read empty after collection");
    CHECK(napi_delete_reference(env, wrap_ref));

    // --- B. strong reference pins across GC, then releases ----------------
    napi_ref strong = make_ref(env, 1);  // refcount 1 => strong
    force_gc(env);
    EXPECT(!ref_is_empty(env, strong), "B: refcount-1 reference must survive GC");
    {
        uint32_t cnt = 99;
        CHECK(napi_reference_unref(env, strong, &cnt));  // 1 -> 0, now weak
        EXPECT(cnt == 0, "B: unref should report count 0");
    }
    force_gc(env);
    EXPECT(ref_is_empty(env, strong), "B: after unref to 0 the target should be collected");
    CHECK(napi_delete_reference(env, strong));

    // --- C. create_reference(value, 0) is weak ----------------------------
    napi_ref weak = make_ref(env, 0);  // refcount 0 => weak
    force_gc(env);
    EXPECT(ref_is_empty(env, weak), "C: weak reference should read empty after collection");
    CHECK(napi_delete_reference(env, weak));

    CHECK(napi_destroy_env(env));
    CHECK(napi_destroy_runtime(runtime));
    CHECK(napi_destroy_platform(platform));

    std::puts("WEAKREF PASS");
    return 0;
}
