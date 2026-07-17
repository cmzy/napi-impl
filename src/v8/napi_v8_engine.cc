// Embedding entry: napi_create_platform / napi_create_runtime / napi_create_env.
// Implements a concrete subclass of the Node.js-derived napi_env__ that can
// run standalone (no Node Environment, no libuv).

#include "js_native_api_v8.h"

#include "v8-array-buffer.h"
#include "v8-initialization.h"
#include "v8-isolate.h"
#include "v8-local-handle.h"
#include "v8-platform.h"

#include "libplatform/libplatform.h"

extern "C" {
#include "napi_v8/embedding.h"
}

#include <cstdlib>
#include <memory>

// ---- napi_platform / napi_runtime opaque types ----------------------------

struct napi_platform__ {
    std::unique_ptr<v8::Platform> platform;
};

struct napi_runtime__ {
    v8::Isolate *isolate = nullptr;
    v8::Isolate::CreateParams create_params;
};

// Process-shared ArrayBuffer allocator. A zero-copy transferred ArrayBuffer's V8
// BackingStore records the allocator that created it and calls that allocator's
// virtual Free() on destruction. Because backing stores are transferred ACROSS
// isolates (e.g. a worker isolate postMessage-transfers an ArrayBuffer to the main
// isolate), a per-runtime allocator dangles once the SOURCE runtime is destroyed
// while a DESTINATION runtime still owns the transferred backing: the destination's
// teardown (ArrayBufferSweeper → ~BackingStore) then frees through the freed
// allocator → SEGV. A single process-shared, stateless (malloc/free) allocator
// outlives every isolate, so the recorded allocator pointer is always valid. Set via
// CreateParams::array_buffer_allocator_shared so every isolate holds a ref to the one
// instance. Thread-safe one-time init (function-local static); never torn down
// (stateless, process-lifetime — same discipline as the singleton platform above).
static std::shared_ptr<v8::ArrayBuffer::Allocator> SharedArrayBufferAllocator() {
    static std::shared_ptr<v8::ArrayBuffer::Allocator> g_alloc(
            v8::ArrayBuffer::Allocator::NewDefaultAllocator());
    return g_alloc;
}

// Process-singleton V8 platform. V8::Initialize/InitializePlatform are
// once-per-process (a second call aborts), so napi_create_platform is made
// idempotent: the platform is owned globally and refcounted across
// create/destroy pairs, letting a host create it more than once (each extra
// call hands back a non-owning handle sharing the same platform). g_v8_platform
// is the raw pointer napi_v8_run_event_loop_tasks pumps.
static std::unique_ptr<v8::Platform> g_owned_platform;
static v8::Platform *g_v8_platform = nullptr;
static int g_platform_refcount = 0;

// ---- Concrete env subclass ------------------------------------------------

namespace {

    class EmbedEnv : public napi_env__ {
    public:
        EmbedEnv(v8::Local<v8::Context> ctx, int32_t module_api_version) : napi_env__(ctx, module_api_version) {}

        // No host queue: invoke finalizers synchronously. Finalizers run between
        // event-loop turns or at env teardown, where there is no ambient
        // HandleScope and the context has been exited — yet a finalizer may
        // allocate V8 handles or call into JS (napi_get_global,
        // napi_call_function, ...). Establish a HandleScope and re-enter the
        // context here; without the scope, e.g. napi_get_global fatals with
        // "Cannot create a handle without a HandleScope". Route through
        // CallIntoModule so a thrown finalizer exception is captured and surfaced
        // as an uncaught exception instead of silently poisoning the next napi
        // call (matches Node's --force-node-api-uncaught-exceptions-policy and the
        // Hermes backend's unhandled-error path).
        void CallFinalizer(napi_finalize cb, void *data, void *hint) override {
            if (cb == nullptr)
                return;
            // enter-once was removed: a finalizer (from the host tick or env
            // teardown) may run with no isolate entered — enter it here.
            v8::Isolate::Scope isolate_scope(isolate);
            v8::HandleScope handle_scope(isolate);
            v8::Local<v8::Context> ctx = context();
            v8::Context::Scope context_scope(ctx);
            CallIntoModule([&](napi_env env) { cb(env, data, hint); },
                           [this](napi_env /*env*/, v8::Local<v8::Value> error) { EmitUncaughtException(error); });
        }

    private:
        // Surface an unhandled finalizer exception to globalThis.__emitUncaughtException
        // (the host bridge for Node's process 'uncaughtException'). The thrown value
        // is already captured in `error_value`, so clear the pending exception first
        // so we can call back into JS. If no dispatcher is installed, it throws, or
        // V8 is terminating, the exception is dropped after clearing — never left
        // pending to corrupt the next napi call.
        void EmitUncaughtException(v8::Local<v8::Value> error_value) {
            napi_value error = v8impl::JsValueFromV8LocalValue(error_value);
            napi_value pending = nullptr;
            napi_get_and_clear_last_exception(this, &pending);
            if (terminatedOrTerminating())
                return;
            napi_value global = nullptr, emit = nullptr;
            napi_valuetype t = napi_undefined;
            if (napi_get_global(this, &global) == napi_ok &&
                napi_get_named_property(this, global, "__emitUncaughtException", &emit) == napi_ok &&
                napi_typeof(this, emit, &t) == napi_ok && t == napi_function) {
                napi_value undef = nullptr, res = nullptr, argv[1] = {error};
                napi_get_undefined(this, &undef);
                if (napi_call_function(this, undef, emit, 1, argv, &res) != napi_ok)
                    napi_get_and_clear_last_exception(this, &pending); // dispatcher threw; drop
            }
        }
    };

    // ---- Per-context private key cache ----------------------------------------

} // namespace

namespace napi_v8_priv {

    // Set by the optional inspector module; see js_native_api_v8_internals.h.
    void (*g_inspector_tick_hook)(napi_env__ *env) = nullptr;

    namespace {
        // Per-thread cache of the two Privates used on the hot
        // napi_wrap/napi_unwrap/type_tag path (v8::Private::ForApi does a string
        // intern + private-symbol registry lookup). A V8 isolate is single-threaded
        // and bound to one thread, so a thread_local cache keyed by the live isolate
        // is correct *while that isolate is alive*.
        //
        // CAUTION: a v8::Isolate* address is REUSED by the allocator after the
        // previous isolate is disposed. Keying the cache on the raw pointer alone is
        // therefore unsafe across dispose: a new isolate that reuses a cached
        // address would be a false cache hit, returning v8::Eternal Privates that
        // belong to the destroyed isolate (dangling handles) → crash inside
        // HasPrivate/SetPrivate on the next wrap. The cache is invalidated on isolate
        // teardown via ForgetIsolatePrivateKeys() (called from napi_destroy_runtime),
        // which clears cached_iso so the next call rebuilds for the new isolate.
        // Lifted to namespace scope (was function-static) so the teardown hook can
        // reach it.
        thread_local v8::Isolate *g_pk_cached_iso = nullptr;
        thread_local v8::Eternal<v8::Private> g_pk_cached_wrapper;
        thread_local v8::Eternal<v8::Private> g_pk_cached_type_tag;
    } // namespace

    v8::Local<v8::Private> GetPrivateKey(v8::Local<v8::Context> ctx, PrivateKeyKind kind) {
        // V8 14.x removed Context::GetIsolate(); use the active isolate (callers
        // are inside an Isolate::Scope at this point).
        v8::Isolate *iso = v8::Isolate::GetCurrent();
        (void) ctx;
        if (g_pk_cached_iso != iso) {
            g_pk_cached_iso = iso;
            auto mk = [iso](const char *name) {
                v8::Local<v8::String> s =
                        v8::String::NewFromUtf8(iso, name, v8::NewStringType::kInternalized).ToLocalChecked();
                return v8::Private::ForApi(iso, s);
            };
            g_pk_cached_wrapper.Set(iso, mk("node:napi_v8::wrapper"));
            g_pk_cached_type_tag.Set(iso, mk("node:napi_v8::type_tag"));
        }
        return (kind == PrivateKeyKind::wrapper) ? g_pk_cached_wrapper.Get(iso) : g_pk_cached_type_tag.Get(iso);
    }

    void ForgetIsolatePrivateKeys(v8::Isolate *iso) {
        // Runs on the isolate's owning thread just before Isolate::Dispose(), so it
        // operates on the correct thread_local cache. Clearing the cached pointer
        // forces the next GetPrivateKey() to rebuild fresh Privates for whatever
        // isolate next occupies this thread — even one that reuses this address. The
        // stale Eternals are never read again: every lookup goes through the
        // cached_iso guard first, which now misses.
        if (g_pk_cached_iso == iso)
            g_pk_cached_iso = nullptr;
    }

} // namespace napi_v8_priv

// ---- Embedding C API ------------------------------------------------------

extern "C" {

napi_status NAPI_CDECL napi_create_platform(int argc, char **argv, int exec_argc, char **exec_argv,
                                            napi_error_message_handler err_handler, bool exit_on_unhandled_error,
                                            napi_platform *result) {
    if (result == nullptr)
        return napi_invalid_arg;
    auto *p = new napi_platform__();
    if (g_platform_refcount == 0) {
        // First platform for this process: init V8 exactly once.
        // Baseline flags (kept for back-compat): expose gc() for tests, staged harmony.
        const char kFlags[] = "--expose-gc --harmony";
        v8::V8::SetFlagsFromString(kFlags, sizeof(kFlags) - 1);
        // Honor host-supplied V8 flags passed via argv (e.g. iOS --jitless, heap
        // limits). argv[0] is the program name; SetFlagsFromCommandLine parses
        // argv[1..]. Must run before V8::Initialize (it does). keep_flags=false so
        // recognized flags are consumed; unknown ones are left in argv (ignored).
        if (argc > 0 && argv != nullptr) {
            int ac = argc;
            v8::V8::SetFlagsFromCommandLine(&ac, argv, /*remove_flags=*/false);
        }
        g_owned_platform = v8::platform::NewDefaultPlatform();
        g_v8_platform = g_owned_platform.get(); // for napi_v8_run_event_loop_tasks
        v8::V8::InitializePlatform(g_owned_platform.get());
        v8::V8::Initialize();
    }
    // else: reuse the already-initialized process platform; `p` is a non-owning
    // handle. Extra argv/flags are ignored (V8 is already up).
    ++g_platform_refcount;
    (void) exec_argc;
    (void) exec_argv;
    (void) err_handler;
    (void) exit_on_unhandled_error;
    *result = p;
    return napi_ok;
}

napi_status NAPI_CDECL napi_destroy_platform(napi_platform platform) {
    if (platform == nullptr)
        return napi_invalid_arg;
    // Tear down V8 only when the last platform handle is destroyed.
    if (g_platform_refcount > 0 && --g_platform_refcount == 0) {
        v8::V8::Dispose();
        v8::V8::DisposePlatform();
        g_owned_platform.reset();
        g_v8_platform = nullptr;
    }
    delete platform;
    return napi_ok;
}

napi_status NAPI_CDECL napi_create_runtime(napi_platform platform, napi_runtime *result) {
    if (platform == nullptr || result == nullptr)
        return napi_invalid_arg;
    auto *r = new napi_runtime__();
    // Process-shared allocator (not per-runtime) so cross-isolate transferred backing
    // stores never free through a dead source-isolate allocator (see comment above).
    r->create_params.array_buffer_allocator_shared = SharedArrayBufferAllocator();
    r->isolate = v8::Isolate::New(r->create_params);
    if (r->isolate == nullptr) {
        delete r;
        return napi_generic_failure;
    }
    *result = r;
    return napi_ok;
}

napi_status NAPI_CDECL napi_destroy_runtime(napi_runtime runtime) {
    if (runtime == nullptr)
        return napi_invalid_arg;
    if (runtime->isolate != nullptr) {
        // Invalidate the per-thread Private cache before the isolate's address can be
        // reused by a future isolate (else a reused address is a false cache hit on a
        // dangling Private → crash in the next napi_wrap). Same thread as Dispose().
        napi_v8_priv::ForgetIsolatePrivateKeys(runtime->isolate);
        runtime->isolate->Dispose();
    }
    delete runtime;
    return napi_ok;
}

napi_status NAPI_CDECL napi_create_env(napi_runtime runtime, napi_env *result) {
    if (runtime == nullptr || result == nullptr)
        return napi_invalid_arg;
    v8::Isolate *isolate = runtime->isolate;
    // B 档: scope the isolate/context only for context creation + env
    // construction. The isolate is NOT left entered — every napi entry re-enters
    // its own isolate+context per call (v8impl::CallScope), which is what lets
    // multiple runtimes share one thread and be torn down in any order. (Formerly
    // this did a persistent isolate->Enter()+ctx->Enter() left until destroy.)
    v8::Isolate::Scope isolate_scope(isolate);
    v8::HandleScope hs(isolate);
    v8::Local<v8::Context> ctx = v8::Context::New(isolate);
    if (ctx.IsEmpty())
        return napi_generic_failure;
    // Context entered only while the napi_env__ ctor runs (it reads
    // Isolate::GetCurrent(), valid inside isolate_scope).
    v8::Context::Scope ctx_scope(ctx);
    auto *env = new EmbedEnv(ctx, NAPI_VERSION);
    *result = env;
    return napi_ok;
}

napi_status NAPI_CDECL napi_destroy_env(napi_env env) {
    if (env == nullptr)
        return napi_invalid_arg;
    v8::Isolate *iso = env->isolate;
    // Unref() -> DeleteMe() runs finalizers; each self-scopes its own context via
    // CallFinalizer. Enter the isolate + a handle scope for teardown, but do NOT
    // hold a Context::Scope across Unref(): env->context() aliases the env's own
    // Global<Context> (PersistentToLocal reinterpret), which delete-this destroys
    // — a Context::Scope outliving the env would then Exit() a freed handle
    // ("Cannot exit non-entered context"). No isolate/context Exit is owed
    // (enter-once was removed, so the env was never left entered).
    v8::Isolate::Scope isolate_scope(iso);
    v8::HandleScope hs(iso);
    env->Unref(); // matches initial refs=1 from napi_env__ ctor; triggers DeleteMe
    return napi_ok;
}

napi_status NAPI_CDECL napi_v8_run_event_loop_tasks(napi_env env) {
    if (env == nullptr)
        return napi_invalid_arg;
    // enter-once was removed, so the host may call this with no isolate entered.
    // Self-scope: enter the isolate + a handle scope + the context for the pump
    // (PumpMessageLoop passes the isolate explicitly, but finalizers and the
    // inspector tick hook create handles / call into JS).
    v8::Isolate::Scope isolate_scope(env->isolate);
    v8::HandleScope hs(env->isolate);
    v8::Context::Scope ctx_scope(env->context());
    // Single host-driven "event-loop tick" hook. This bare embedding has no
    // libuv loop, so the host (AmeCanvas event loop) must call this at a safe
    // point — isolate + context entered, a handle scope open, not inside GC.
    // It performs the work Node would do per event-loop iteration:
    //   1. Pump the V8 foreground task runner — runs FinalizationRegistry
    //      cleanup callbacks and any other foreground tasks V8 scheduled
    //      (without this they never fire).
    //   2. Drain the deferred (second-pass) napi finalizer queue — frees
    //      napi_wrap'd natives whose JS wrappers were collected (without this
    //      they leak until env teardown).
    // (Microtasks stay on V8's kAuto policy and need no explicit pump here.)
    if (g_v8_platform != nullptr) {
        // kDoNotWait: run all ready tasks, then return without blocking.
        while (v8::platform::PumpMessageLoop(g_v8_platform, env->isolate,
                                             v8::platform::MessageLoopBehavior::kDoNotWait)) {
        }
    }
    env->DrainFinalizerQueue();
    //   3. If an inspector is active for this env, drain queued CDP messages and
    //      dispatch them on this (the V8) thread. No-op when the inspector module
    //      is not linked or no inspector was started.
    if (napi_v8_priv::g_inspector_tick_hook != nullptr)
        napi_v8_priv::g_inspector_tick_hook(env);
    return napi_ok;
}

napi_status NAPI_CDECL napi_v8_terminate_execution(napi_env env) {
    if (env == nullptr || env->isolate == nullptr)
        return napi_invalid_arg;
    // v8::Isolate::TerminateExecution is thread-safe by contract: it sets the isolate's
    // interrupt/terminate flag, checked at stack-guard points (function entry + loop
    // back-edges), so JS running on ANOTHER thread — including `while (true) {}` — is
    // unwound with a non-catchable termination. Do NOT enter the isolate here (it may be
    // busy on its own thread; entering from a second thread would deadlock/abort). Just
    // arm the flag.
    env->isolate->TerminateExecution();
    return napi_ok;
}

napi_status NAPI_CDECL napi_v8_cancel_terminate_execution(napi_env env) {
    if (env == nullptr || env->isolate == nullptr)
        return napi_invalid_arg;
    // Clears a pending TerminateExecution so the isolate is reusable for subsequent calls.
    // Unlike terminate_execution (cross-thread by contract), this MUST be called on the
    // isolate's OWN thread — the intended use is: after a runaway JS has been terminated and
    // has unwound out to the C++ embedder boundary (e.g. RunFrame returns), the owning thread
    // calls this to re-enable the isolate so the next frame/task runs. No-op if not currently
    // terminating, so callers may invoke it unconditionally after each drive.
    env->isolate->CancelTerminateExecution();
    return napi_ok;
}

napi_status NAPI_CDECL napi_v8_is_execution_terminating(napi_env env, bool* result) {
    if (env == nullptr || env->isolate == nullptr || result == nullptr)
        return napi_invalid_arg;
    // Whether a termination is currently in progress on this isolate (true between an armed
    // TerminateExecution and the point the owning thread clears it via cancel). Call on the
    // owning thread. Lets the embedder distinguish "JS finished normally" from "JS was killed
    // by the watchdog" (→ report to host) after a drive returns.
    *result = env->isolate->IsExecutionTerminating();
    return napi_ok;
}

} // extern "C"
