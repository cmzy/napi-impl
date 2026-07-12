// Embedding entry for the JSC backend: implements the project-wide embedding C
// ABI (napi_create_platform / napi_create_runtime / napi_create_env + the
// per-tick hook) declared in napi_v8/embedding.h on top of JavaScriptCore's C
// API. Mapping:
//   napi_runtime  -> JSContextGroupRef   (an independent VM / heap, like a V8 isolate)
//   napi_env      -> JSGlobalContextRef  (a global context) + our napi_env__ state
// One env per runtime in this embedding, so libnapi_jsc is a drop-in ABI swap
// for libnapi_v8 / libnapi_hermes: the host uses the identical embedding API +
// standard napi_* surface across engines.

#include <atomic>
#include <cstdlib>

#include <JavaScriptCore/JavaScript.h>

#include "napi_v8/embedding.h"

#include "js_native_api_jsc.h"

// JSContextGroupSetExecutionTimeLimit lives in JavaScriptCore's private header
// (JSContextRefPrivate.h), not shipped with the system framework — but the symbol
// is exported by JavaScriptCore.framework. Forward-declare the stable private
// prototype (long-standing WebKit runaway-script API) and link against it.
extern "C" {
typedef bool (*JSShouldTerminateCallback)(JSContextRef ctx, void *context);
void JSContextGroupSetExecutionTimeLimit(JSContextGroupRef group, double limit,
                                         JSShouldTerminateCallback callback, void *context);
}

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
    // napi_v8_terminate_execution flag. JSC has no lock-free "terminate now" C API:
    // JSContextGroupSetExecutionTimeLimit acquires the JSLock, which a running (esp.
    // runaway `while(true){}`) script holds → calling it on demand from another thread
    // deadlocks. Instead we arm the watchdog ONCE at runtime creation (JSLock free then)
    // with a small poll interval + a callback that reads this atomic flag; terminate
    // just sets the flag (no JSLock), and the already-armed watchdog fires within the
    // interval and terminates. Checked at VMTraps points (loop back-edges).
    std::atomic<bool> terminate_requested{false};
};

namespace {
    // Watchdog poll interval: a script running longer than this hits the callback at the
    // next VMTraps checkpoint. Small enough for responsive teardown (~50 ms), large enough
    // that sub-interval scripts (the norm) never trigger it → steady-state cost is a
    // background timer only.
    constexpr double kJscWatchdogPollSeconds = 0.05;

    // Watchdog callback (invoked by JSC's watchdog at an execution checkpoint once the
    // poll interval elapses): terminate iff the host requested it. **JSC fires the callback
    // only ONCE per arming** (it does NOT auto-re-arm on a false return — verified by
    // instrumentation: without the re-arm below the callback fires exactly once, so a
    // terminate flag set afterwards is never observed and a runaway loop hangs forever).
    // So re-arm here before returning false, to keep polling the flag for the script's
    // lifetime. This runs on the JS thread holding the JSLock; the SetExecutionTimeLimit's
    // JSLockHolder is a recursive same-thread acquire (no deadlock — unlike an on-demand
    // cross-thread call, which is exactly the deadlock this whole design avoids).
    bool JscTerminateCheck(JSContextRef, void *context) {
        auto *r = static_cast<napi_runtime>(context);
        if (r == nullptr)
            return false;
        if (r->terminate_requested.load(std::memory_order_acquire))
            return true;
        JSContextGroupSetExecutionTimeLimit(r->group, kJscWatchdogPollSeconds, JscTerminateCheck, r);
        return false;
    }
} // namespace

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
    // Arm the watchdog now (no script running → the JSLock this acquires is free, no
    // deadlock). 50 ms poll: a script running longer than this invokes JscTerminateCheck
    // at the next VMTraps checkpoint; it returns false (re-arm) until a host terminate
    // request flips the flag, then terminates within ~50 ms. Scripts shorter than 50 ms
    // never trigger it, so steady-state overhead is a background timer only.
    JSContextGroupSetExecutionTimeLimit(r->group, kJscWatchdogPollSeconds, JscTerminateCheck, r);
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

napi_status NAPI_CDECL napi_v8_terminate_execution(napi_env env) {
    if (env == nullptr || env->embed_owner == nullptr)
        return napi_invalid_arg;
    auto *owner = static_cast<napi_runtime>(env->embed_owner);
    // Set the flag only — NO JSLock (the runaway script holds it; acquiring it here,
    // as JSContextGroupSetExecutionTimeLimit does, would deadlock). The watchdog armed
    // at napi_create_runtime is already running; its next checkpoint reads the flag and
    // terminates the script. Thread-safe: called from the teardown thread.
    owner->terminate_requested.store(true, std::memory_order_release);
    return napi_ok;
}
