// Embedding entry for the JSC backend: implements the project-wide embedding C
// ABI (napi_create_platform / napi_create_runtime / napi_create_env + the
// per-tick hook) declared in napi_v8/embedding.h on top of JavaScriptCore's C
// API. Mapping:
//   napi_runtime  -> JSContextGroupRef   (an independent VM / heap, like a V8 isolate)
//   napi_env      -> JSGlobalContextRef  (a global context) + our napi_env__ state
// One env per runtime in this embedding, so libnapi_jsc is a drop-in ABI swap
// for libnapi_v8 / libnapi_hermes: the host uses the identical embedding API +
// standard napi_* surface across engines.

#include <cstdlib>

#include <JavaScriptCore/JavaScript.h>

#include "napi_v8/embedding.h"

#include "js_native_api_jsc.h"

// JSC disables SharedArrayBuffer by default (Spectre mitigation). The napi_v8/
// sab.h extension needs it, so enable the engine option before JSC initializes
// its Options (which it does lazily on first VM creation). A library
// constructor runs at dlopen — earlier than the host's first napi_create_runtime
// — and overwrite=0 leaves a host-set value untouched. If JSC is already
// initialized by the host before we load, the SAB path falls back to an
// ArrayBuffer-backed buffer (see jsc_v8compat.cc).
__attribute__((constructor)) static void napi_jsc_enable_engine_features() {
    setenv("JSC_useSharedArrayBuffer", "1", 0);
}

// ---- opaque embedding types ----------------------------------------------

struct napi_platform__ {
    napi_error_message_handler err_handler = nullptr;
    bool exit_on_unhandled_error = false;
};

struct napi_runtime__ {
    JSContextGroupRef group = nullptr;
    napi_platform platform = nullptr;
    napi_env env = nullptr;  // the single env owned by this runtime (lazily created)
};

// ---- platform -------------------------------------------------------------

napi_status NAPI_CDECL napi_create_platform(int /*argc*/, char** /*argv*/, int /*exec_argc*/, char** /*exec_argv*/,
                                            napi_error_message_handler err_handler, bool exit_on_unhandled_error,
                                            napi_platform* result) {
    // JSC has no global flag parser/platform; argv/exec_argv are accepted for
    // ABI parity and ignored.
    if (result == nullptr)
        return napi_invalid_arg;
    auto* p = new napi_platform__();
    p->err_handler = err_handler;
    p->exit_on_unhandled_error = exit_on_unhandled_error;
    *result = p;
    return napi_ok;
}

napi_status NAPI_CDECL napi_destroy_platform(napi_platform platform) {
    if (platform == nullptr)
        return napi_invalid_arg;
    delete platform;
    return napi_ok;
}

// ---- runtime --------------------------------------------------------------

napi_status NAPI_CDECL napi_create_runtime(napi_platform platform, napi_runtime* result) {
    if (platform == nullptr || result == nullptr)
        return napi_invalid_arg;
    auto* r = new napi_runtime__();
    r->platform = platform;
    r->group = JSContextGroupCreate();
    if (r->group == nullptr) {
        delete r;
        return napi_generic_failure;
    }
    *result = r;
    return napi_ok;
}

napi_status NAPI_CDECL napi_destroy_runtime(napi_runtime runtime) {  // AME-JSC-TEARDOWN-FIX
    if (runtime == nullptr)
        return napi_invalid_arg;
    // AmeCanvas teardown fix: release the runtime's context-group ref *before*
    // deleting the env. The env's JSGlobalContext holds its own (implicit) group
    // ref, so the VM stays alive across this release; napi_jsc_env_delete's
    // JSGlobalContextRelease then drops the *last* group ref, tearing the VM down
    // — and firing any straggler finalizers — while the env is still alive
    // (env_delete drains them before `delete env`). The original order released the
    // group *after* env_delete, so VM teardown fired finalizers that dereferenced
    // the freed env (UAF on env->finalizer_mu -> "mutex lock failed: Invalid
    // argument" abort). See docs/JSC_INTEGRATION.md.
    if (runtime->group != nullptr) {
        JSContextGroupRelease(runtime->group);
        runtime->group = nullptr;
    }
    if (runtime->env != nullptr) {
        napi_jsc_env_delete(runtime->env);  // releases ctx (last group ref -> VM teardown), drains finalizers, frees env
        runtime->env = nullptr;
    }
    delete runtime;
    return napi_ok;
}

// ---- env ------------------------------------------------------------------

// globalThis.gc() -> JSGarbageCollect: parity with the V8 backend, whose
// napi_create_platform passes "--expose-gc" so V8 installs globalThis.gc
// unconditionally (napi_v8_engine.cc). JSC has no such flag, so we install the
// same global here. Lets embedders/tests force a synchronous full collection so
// GC-driven finalizers (napi_wrap / external / ArrayBuffer deallocators) run
// deterministically instead of at the engine's lazy discretion — the host's
// leak/lifecycle tests rely on `if (globalThis.gc) gc()`. Diagnostic-only and
// harmless in production, same rationale as V8's always-on --expose-gc.
// JSSynchronousGarbageCollectForDebugging forces a FULL synchronous collection
// (JSC SPI, exported by the system JavaScriptCore.framework; the public
// JSGarbageCollect is advisory/eden-biased and does not reliably reclaim +
// enqueue finalizers for objects freed this turn — the host's leak tests need
// deterministic reclamation). Declared extern here since it lives in a private
// header (JSContextRefPrivate.h) not always in the SDK; the symbol is present.
extern "C" void JSSynchronousGarbageCollectForDebugging(JSContextRef);
static JSValueRef napi_jsc_gc_callback(JSContextRef ctx, JSObjectRef, JSObjectRef, size_t, const JSValueRef[],
                                       JSValueRef*) {
    JSSynchronousGarbageCollectForDebugging(ctx);
    return JSValueMakeUndefined(ctx);
}
static void napi_jsc_expose_gc(JSGlobalContextRef ctx) {
    JSObjectRef global = JSContextGetGlobalObject(ctx);
    JSStringRef name = JSStringCreateWithUTF8CString("gc");
    JSObjectRef fn = JSObjectMakeFunctionWithCallback(ctx, name, napi_jsc_gc_callback);
    JSObjectSetProperty(ctx, global, name, fn, kJSPropertyAttributeDontEnum, nullptr);
    JSStringRelease(name);
}

napi_status NAPI_CDECL napi_create_env(napi_runtime runtime, napi_env* result) {
    if (runtime == nullptr || result == nullptr)
        return napi_invalid_arg;
    if (runtime->env == nullptr) {
        JSGlobalContextRef ctx = JSGlobalContextCreateInGroup(runtime->group, nullptr);
        if (ctx == nullptr)
            return napi_generic_failure;
        napi_error_message_handler uncaught =
            runtime->platform != nullptr ? runtime->platform->err_handler : nullptr;
        napi_env env = napi_jsc_env_new(ctx, uncaught);
        JSGlobalContextRelease(ctx);  // env_new retained its own reference
        if (env == nullptr)
            return napi_generic_failure;
        napi_jsc_expose_gc(env->ctx);  // globalThis.gc()（镜像 V8 --expose-gc）
        env->embed_owner = runtime;
        runtime->env = env;
    }
    *result = runtime->env;
    return napi_ok;
}

napi_status NAPI_CDECL napi_destroy_env(napi_env env) {  // AME-JSC-TEARDOWN-FIX
    if (env == nullptr)
        return napi_invalid_arg;
    auto* owner = static_cast<napi_runtime>(env->embed_owner);
    if (owner != nullptr) {
        if (owner->env == env)
            owner->env = nullptr;
        // Drop the runtime's context-group ref *before* env_delete, exactly as
        // napi_destroy_runtime does. Otherwise env_delete's JSGlobalContextRelease
        // is NOT the last group ref (the runtime still holds one) → the VM does not
        // tear down → the straggler wrap/external finalizers don't fire until the
        // *later* napi_destroy_runtime releases the group, by which point `delete env`
        // has already run → those finalizers deref a freed env (UAF on
        // finalizer_mu → "mutex lock failed: Invalid argument" abort). Releasing the
        // group here makes env_delete drop the last ref, so the VM tears down and
        // drains finalizers while the env is still alive. Whichever of
        // destroy_env / destroy_runtime runs first releases the group; the other
        // sees a null group and skips. See docs/JSC_INTEGRATION.md.
        if (owner->group != nullptr) {
            JSContextGroupRelease(owner->group);
            owner->group = nullptr;
        }
    }
    napi_jsc_env_delete(env);
    return napi_ok;
}

// ---- per-tick hook --------------------------------------------------------

napi_status NAPI_CDECL napi_v8_run_event_loop_tasks(napi_env env) {
    if (env == nullptr)
        return napi_invalid_arg;
    // JSC drains its microtask (Promise job) queue automatically at the end of
    // each JSEvaluateScript / native callback, so there is nothing to pump here;
    // we only run the deferred (post-GC) napi finalizers the wrap/external
    // holders enqueued.
    napi_jsc_drain_finalizers(env);
    return napi_ok;
}
