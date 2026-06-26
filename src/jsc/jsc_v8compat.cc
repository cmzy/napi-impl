// JSC adaptation of the V8-specific embedding extensions — the Inspector
// (napi_v8/inspector.h) and SharedArrayBuffer (napi_v8/sab.h) surfaces. These
// keep the napi_v8_* names (like napi_v8_run_event_loop_tasks) so a consumer
// written against the extension headers links any libnapi_<engine>.
//
// Inspector: V8's path runs a host-driven Chrome DevTools Protocol loop over a
// TCP WebSocket. JSC has no such public surface — instead it exposes
// JSGlobalContextSetInspectable (macOS 13.3+/iOS 16.4+), which makes the context
// debuggable through the *system* RemoteInspector transport (attach via Safari's
// Develop menu, WebKit Inspector Protocol — not chrome://inspect). So start/stop
// are real (they toggle inspectability + set the display name); the CDP message
// pump / pause-state / handler calls have no host-driven equivalent and are
// honest no-ops (JSC drives its own transport thread).
//
// SharedArrayBuffer: JSC gates SAB off by default; we enable it at load (see the
// constructor in napi_jsc_engine.cc) and create real SharedArrayBuffer objects
// via the global constructor, reading their backing store with
// JSObjectGetArrayBufferBytesPtr. If SAB is unavailable we fall back to an
// ArrayBuffer over our own allocation (real zero-copy C<->JS within the process,
// tagged so is_shared_arraybuffer still reports it as shared).

// node_api_create/is_sharedarraybuffer are experimental core node-api calls.
#define NAPI_EXPERIMENTAL

#include <cstddef>
#include <cstdlib>

#include <JavaScriptCore/JavaScript.h>

#include "js_native_api_jsc.h"
#include "napi_v8/inspector.h"

namespace {
JSObjectRef ToObj(napi_env env, napi_value v) {
    return JSValueToObject(env->ctx, ToJS(v), nullptr);
}
void sab_dealloc(void* bytes, void*) { std::free(bytes); }
}  // namespace

// ===========================================================================
// Inspector (napi_v8/inspector.h)
// ===========================================================================

napi_status NAPI_CDECL napi_v8_inspector_start(napi_env env, int /*port*/, const char* context_name) {
    CHECK_ENV(env);
    // JSC has no TCP port: inspection runs over the system RemoteInspector
    // transport, so `port` is not applicable and is ignored.
    if (context_name != nullptr) {
        JSStringRef name = JSStringCreateWithUTF8CString(context_name);
        JSGlobalContextSetName(env->ctx, name);
        JSStringRelease(name);
    }
    if (__builtin_available(macOS 13.3, iOS 16.4, *)) {
        JSGlobalContextSetInspectable(env->ctx, true);
        return napi_jsc_clear_error(env);
    }
    // Older OS: no public inspectability toggle.
    return napi_jsc_set_error(env, napi_generic_failure);
}

napi_status NAPI_CDECL napi_v8_inspector_stop(napi_env env) {
    CHECK_ENV(env);
    if (__builtin_available(macOS 13.3, iOS 16.4, *)) {
        JSGlobalContextSetInspectable(env->ctx, false);
    }
    return napi_jsc_clear_error(env);
}

napi_status NAPI_CDECL napi_v8_inspector_wait_for_connection(napi_env env) {
    CHECK_ENV(env);
    // JSC exposes no "a client attached" signal through the public C API; a
    // debugger may attach at any time via Safari. Nothing to block on.
    return napi_jsc_clear_error(env);
}

napi_status NAPI_CDECL napi_v8_inspector_pump_messages(napi_env env, size_t* out_dispatched) {
    CHECK_ENV(env);
    // JSC's RemoteInspector runs its own transport thread; there is no host queue
    // to drain (unlike V8, where CDP messages must be dispatched on this thread).
    if (out_dispatched != nullptr)
        *out_dispatched = 0;
    return napi_jsc_clear_error(env);
}

bool NAPI_CDECL napi_v8_inspector_is_paused(napi_env env) {
    // No public pause-state query in JSC's C API.
    (void)env;
    return false;
}

napi_status NAPI_CDECL napi_v8_inspector_wait(napi_env env, int /*timeout_ms*/) {
    CHECK_ENV(env);
    return napi_jsc_clear_error(env);  // nothing for the host to wait on
}

napi_status NAPI_CDECL napi_v8_inspector_set_pause_handler(napi_env env, napi_v8_inspector_pause_handler handler,
                                                           void* data) {
    CHECK_ENV(env);
    // Stored for fidelity; JSC drives pause/resume through its own transport, so
    // we never invoke this.
    env->insp_pause_handler = reinterpret_cast<void*>(handler);
    env->insp_pause_data = data;
    return napi_jsc_clear_error(env);
}

napi_status NAPI_CDECL napi_v8_inspector_set_wake_handler(napi_env env, napi_v8_inspector_wake_handler handler,
                                                          void* data) {
    CHECK_ENV(env);
    env->insp_wake_handler = reinterpret_cast<void*>(handler);
    env->insp_wake_data = data;
    return napi_jsc_clear_error(env);
}

// ===========================================================================
// SharedArrayBuffer (core node-api, experimental): node_api_create_/is_/
// create_external_sharedarraybuffer. Reading a SAB's backing pointer/size goes
// through napi_get_arraybuffer_info, which the JSC backend extends to accept a
// SAB (see src/jsc/jsc_buffers.cc).
// ===========================================================================

napi_status NAPI_CDECL node_api_create_sharedarraybuffer(napi_env env, size_t byte_length, void** data,
                                                         napi_value* result) {
    NAPI_PREAMBLE(env);
    CHECK_ARG(env, result);
    JSContextRef ctx = env->ctx;
    JSObjectRef buf = nullptr;

    if (env->shared_arraybuffer_ctor != nullptr) {
        // Real SharedArrayBuffer via `new SharedArrayBuffer(byte_length)`.
        JSValueRef arg = JSValueMakeNumber(ctx, static_cast<double>(byte_length));
        JSValueRef exc = nullptr;
        buf = JSObjectCallAsConstructor(ctx, env->shared_arraybuffer_ctor, 1, &arg, &exc);
        if (exc != nullptr)
            return napi_jsc_record_exception(env, exc);
    }
    if (buf == nullptr) {
        // Fallback: an ArrayBuffer over our own zeroed allocation. Real zero-copy
        // C<->JS within the process; not a cross-agent SharedArrayBuffer.
        void* mem = std::calloc(1, byte_length != 0 ? byte_length : 1);
        if (mem == nullptr)
            return napi_jsc_set_error(env, napi_generic_failure);
        JSValueRef exc = nullptr;
        buf = JSObjectMakeArrayBufferWithBytesNoCopy(ctx, mem, byte_length, sab_dealloc, nullptr, &exc);
        if (buf == nullptr || exc != nullptr) {
            std::free(mem);
            return napi_jsc_record_exception(env, exc);
        }
        // Tag it so node_api_is_sharedarraybuffer recognizes the fallback.
        JSObjectSetPropertyForKey(ctx, buf, env->sab_tag, JSValueMakeBoolean(ctx, true),
                                  kJSPropertyAttributeDontEnum | kJSPropertyAttributeDontDelete, nullptr);
    }

    if (data != nullptr) {
        JSValueRef exc = nullptr;
        *data = JSObjectGetArrayBufferBytesPtr(ctx, buf, &exc);
    }
    *result = napi_jsc_add_handle(env, buf);
    return napi_jsc_clear_error(env);
}

napi_status NAPI_CDECL node_api_is_sharedarraybuffer(napi_env env, napi_value value, bool* result) {
    CHECK_ENV(env);
    CHECK_ARG(env, value);
    CHECK_ARG(env, result);
    JSContextRef ctx = env->ctx;
    bool is = false;
    if (env->shared_arraybuffer_ctor != nullptr)
        is = JSValueIsInstanceOfConstructor(ctx, ToJS(value), env->shared_arraybuffer_ctor, nullptr);
    if (!is && JSValueIsObject(ctx, ToJS(value)) && env->sab_tag != nullptr) {
        JSValueRef tag = JSObjectGetPropertyForKey(ctx, ToObj(env, value), env->sab_tag, nullptr);
        is = tag != nullptr && JSValueToBoolean(ctx, tag);
    }
    *result = is;
    return napi_jsc_clear_error(env);
}

napi_status NAPI_CDECL node_api_create_external_sharedarraybuffer(napi_env env, void* external_data, size_t byte_length,
                                                                  node_api_noenv_finalize finalize_cb,
                                                                  void* finalize_hint, napi_value* result) {
    NAPI_PREAMBLE(env);
    CHECK_ARG(env, result);
    JSContextRef ctx = env->ctx;
    // JSC has no SAB-over-external-memory constructor; wrap the caller's memory
    // as a no-copy ArrayBuffer and tag it as shared (same scheme as the create
    // fallback). Real C<->JS zero-copy over the buffer; not cross-agent sharing.
    struct FinalizerBox {
        node_api_noenv_finalize cb;
        void* hint;
    };
    FinalizerBox* box = (finalize_cb != nullptr) ? new FinalizerBox{finalize_cb, finalize_hint} : nullptr;
    auto dealloc = +[](void* bytes, void* deallocator_ctx) {
        if (auto* b = static_cast<FinalizerBox*>(deallocator_ctx)) {
            b->cb(bytes, b->hint);
            delete b;
        }
    };

    JSValueRef exc = nullptr;
    JSObjectRef buf = JSObjectMakeArrayBufferWithBytesNoCopy(ctx, external_data, byte_length, dealloc, box, &exc);
    if (buf == nullptr || exc != nullptr) {
        delete box;
        return napi_jsc_record_exception(env, exc);
    }
    JSObjectSetPropertyForKey(ctx, buf, env->sab_tag, JSValueMakeBoolean(ctx, true),
                              kJSPropertyAttributeDontEnum | kJSPropertyAttributeDontDelete, nullptr);
    *result = napi_jsc_add_handle(env, buf);
    return napi_jsc_clear_error(env);
}
