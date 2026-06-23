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
    std::unique_ptr<v8::ArrayBuffer::Allocator> allocator;
};

// Process-singleton V8 platform pointer, captured at napi_create_platform, used
// by napi_v8_run_event_loop_tasks to pump the foreground task runner.
static v8::Platform *g_v8_platform = nullptr;

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

    v8::Local<v8::Private> GetPrivateKey(v8::Local<v8::Context> ctx, PrivateKeyKind kind) {
        // V8 14.x removed Context::GetIsolate(); use the active isolate (callers
        // are inside an Isolate::Scope at this point).
        v8::Isolate *iso = v8::Isolate::GetCurrent();
        (void) ctx;
        // Cache the two Privates per isolate: v8::Private::ForApi does a string
        // intern + private-symbol registry lookup, and this is on the hot
        // napi_wrap/napi_unwrap/type_tag path. A V8 isolate is single-threaded and
        // bound to one thread, so a thread_local cache keyed by the isolate is
        // correct (the isolate check handles the rare multi-isolate-per-thread
        // case). v8::Eternal lives for the isolate's lifetime.
        static thread_local v8::Isolate *cached_iso = nullptr;
        static thread_local v8::Eternal<v8::Private> cached_wrapper;
        static thread_local v8::Eternal<v8::Private> cached_type_tag;
        if (cached_iso != iso) {
            cached_iso = iso;
            auto mk = [iso](const char *name) {
                v8::Local<v8::String> s =
                        v8::String::NewFromUtf8(iso, name, v8::NewStringType::kInternalized).ToLocalChecked();
                return v8::Private::ForApi(iso, s);
            };
            cached_wrapper.Set(iso, mk("node:napi_v8::wrapper"));
            cached_type_tag.Set(iso, mk("node:napi_v8::type_tag"));
        }
        return (kind == PrivateKeyKind::wrapper) ? cached_wrapper.Get(iso) : cached_type_tag.Get(iso);
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
    p->platform = v8::platform::NewDefaultPlatform();
    g_v8_platform = p->platform.get(); // for napi_v8_run_event_loop_tasks
    v8::V8::InitializePlatform(p->platform.get());
    v8::V8::Initialize();
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
    v8::V8::Dispose();
    v8::V8::DisposePlatform();
    delete platform;
    return napi_ok;
}

napi_status NAPI_CDECL napi_create_runtime(napi_platform platform, napi_runtime *result) {
    if (platform == nullptr || result == nullptr)
        return napi_invalid_arg;
    auto *r = new napi_runtime__();
    r->allocator.reset(v8::ArrayBuffer::Allocator::NewDefaultAllocator());
    r->create_params.array_buffer_allocator = r->allocator.get();
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
    if (runtime->isolate != nullptr)
        runtime->isolate->Dispose();
    delete runtime;
    return napi_ok;
}

napi_status NAPI_CDECL napi_create_env(napi_runtime runtime, napi_env *result) {
    if (runtime == nullptr || result == nullptr)
        return napi_invalid_arg;
    v8::Isolate *isolate = runtime->isolate;
    isolate->Enter();
    v8::HandleScope hs(isolate);
    v8::Local<v8::Context> ctx = v8::Context::New(isolate);
    if (ctx.IsEmpty()) {
        isolate->Exit();
        return napi_generic_failure;
    }
    // Enter the context so napi_get_global etc. resolve to ours.
    ctx->Enter();
    auto *env = new EmbedEnv(ctx, NAPI_VERSION);
    *result = env;
    return napi_ok;
}

napi_status NAPI_CDECL napi_destroy_env(napi_env env) {
    if (env == nullptr)
        return napi_invalid_arg;
    v8::Isolate *iso = env->isolate;
    {
        v8::HandleScope hs(iso);
        env->context()->Exit();
    }
    env->Unref(); // matches initial refs=1 from napi_env__ ctor; triggers DeleteMe
    iso->Exit();
    return napi_ok;
}

napi_status NAPI_CDECL napi_v8_run_event_loop_tasks(napi_env env) {
    if (env == nullptr)
        return napi_invalid_arg;
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

} // extern "C"
