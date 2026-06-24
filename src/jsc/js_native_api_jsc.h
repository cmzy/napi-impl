// Internal definitions for the JSC Node-API backend.
//
// Unlike the Hermes backend (which links Hermes' own bundled Node-API), JSC
// ships no Node-API, so this backend hand-writes the napi_* surface on top of
// JavaScriptCore's stable C API (JSValueRef / JSObjectRef / JSStringRef), the
// same shape as the V8 port but against JSC instead of v8.h.
//
// Value model: a napi_value IS a JSValueRef (reinterpret-cast). Every value we
// hand out is JSValueProtect'd and recorded in the current handle scope; closing
// the scope unprotects them. A root scope, opened at env creation, backs values
// the embedding host creates outside any explicit scope.

#ifndef SRC_JSC_JS_NATIVE_API_JSC_H_
#define SRC_JSC_JS_NATIVE_API_JSC_H_

// Expose the full Node-API declaration surface (incl. NAPI_VERSION >= 10 and
// experimental node_api_* entry points) so our definitions of those symbols
// inherit NAPI_EXTERN's default visibility and get exported. This is the
// compile-time decl level only; the runtime version we report is env->version
// (9) via napi_get_version. Must precede the napi header includes below.
#ifndef NAPI_VERSION
#define NAPI_VERSION 2147483647  // == NAPI_VERSION_EXPERIMENTAL
#endif
#ifndef NAPI_EXPERIMENTAL
#define NAPI_EXPERIMENTAL
#endif

#include <cstdint>
#include <cstring>
#include <deque>
#include <mutex>
#include <vector>

#include <JavaScriptCore/JavaScript.h>

#include "napi/js_native_api.h"
#include "napi/js_native_api_types.h"

// ---- handle scopes --------------------------------------------------------

struct napi_handle_scope__ {
    bool escapable = false;
    bool escaped = false;
    std::vector<JSValueRef> handles;  // each JSValueProtect'd; unprotected on close
};

// ---- native holders (external values, wrapped natives) --------------------

// Private data of an `external_class` object: a JSC object that carries a native
// pointer + optional finalizer. Used both for napi_create_external and as the
// hidden holder attached to napi_wrap'd objects.
struct ExternalState {
    napi_env env = nullptr;
    void* data = nullptr;          // native pointer (external value / wrapped native)
    napi_finalize finalize_cb = nullptr;
    void* finalize_hint = nullptr;
};

// Private data of a `function_class` / `constructor_class` object: routes a JSC
// call back into a user napi_callback.
struct CallbackData {
    napi_env env = nullptr;
    napi_callback cb = nullptr;
    void* data = nullptr;
};

// A finalizer deferred out of GC (JSC finalize callbacks must not re-enter JS),
// drained on the per-tick hook and at env teardown.
struct PendingFinalizer {
    napi_finalize cb;
    void* data;
    void* hint;
};

// ---- env ------------------------------------------------------------------

struct napi_env__ {
    JSGlobalContextRef ctx = nullptr;  // retained for the env's lifetime

    std::vector<napi_handle_scope__*> scopes;  // scopes[0] is the root scope

    JSValueRef pending_exception = nullptr;  // protected while set
    napi_extended_error_info last_error{};

    void* instance_data = nullptr;
    napi_finalize instance_data_finalize = nullptr;
    void* instance_data_hint = nullptr;

    // Shared JSClasses (context-group independent; created once per env).
    JSClassRef external_class = nullptr;
    JSClassRef function_class = nullptr;
    JSClassRef constructor_class = nullptr;

    // Cached globals (protected) used to express operations the C API lacks.
    JSObjectRef obj_define_property = nullptr;   // Object.defineProperty
    JSObjectRef obj_has_own = nullptr;           // Object.prototype.hasOwnProperty
    JSObjectRef obj_freeze = nullptr;            // Object.freeze
    JSObjectRef obj_seal = nullptr;              // Object.seal
    JSObjectRef symbol_for = nullptr;            // Symbol.for
    JSValueRef wrap_key = nullptr;               // hidden Symbol keying napi_wrap holders

    std::mutex finalizer_mu;
    std::deque<PendingFinalizer> pending_finalizers;

    int version = 9;  // reported by napi_get_version (node-api-headers v1.9.0)

    // Surfaces an unhandled JS error (e.g. thrown from a finalizer) to the host.
    void (*uncaught)(const char* msg) = nullptr;

    // Back-link to the embedding's napi_runtime (set by napi_jsc_engine.cc) so
    // either teardown order (destroy_env then destroy_runtime, or vice versa)
    // is safe. Opaque to the napi surface.
    void* embed_owner = nullptr;
};

// ---- shared helpers (defined in js_native_api_jsc.cc) ---------------------

inline JSValueRef ToJS(napi_value v) {
    return reinterpret_cast<JSValueRef>(v);
}
inline napi_value ToNapi(JSValueRef v) {
    return reinterpret_cast<napi_value>(const_cast<OpaqueJSValue*>(v));
}

// Protect `v`, record it in the current handle scope, return it as a napi_value.
napi_value napi_jsc_add_handle(napi_env env, JSValueRef v);

// last-error / exception bookkeeping.
napi_status napi_jsc_set_error(napi_env env, napi_status status);
napi_status napi_jsc_clear_error(napi_env env);
// Record `exc` (from a JSC exception out-param) as the pending exception and
// return napi_pending_exception. `exc` may be null (returns napi_ok then).
napi_status napi_jsc_record_exception(napi_env env, JSValueRef exc);

// Run all deferred finalizers (called from the per-tick hook and at teardown).
void napi_jsc_drain_finalizers(napi_env env);

// env lifecycle, used by the embedding adapter (napi_jsc_engine.cc).
napi_env napi_jsc_env_new(JSGlobalContextRef ctx, void (*uncaught)(const char*));
void napi_jsc_env_delete(napi_env env);

// ---- argument-checking macros ---------------------------------------------

#define CHECK_ENV(env)                                                                                                  \
    do {                                                                                                               \
        if ((env) == nullptr)                                                                                          \
            return napi_invalid_arg;                                                                                   \
    } while (0)

#define CHECK_ARG(env, arg)                                                                                            \
    do {                                                                                                               \
        if ((arg) == nullptr)                                                                                          \
            return napi_jsc_set_error((env), napi_invalid_arg);                                                       \
    } while (0)

#define RETURN_STATUS_IF_FALSE(env, cond, status)                                                                      \
    do {                                                                                                               \
        if (!(cond))                                                                                                   \
            return napi_jsc_set_error((env), (status));                                                               \
    } while (0)

// Entry guard for functions that touch JS state: clear stale error, refuse to
// proceed while a JS exception is pending.
#define NAPI_PREAMBLE(env)                                                                                             \
    do {                                                                                                               \
        CHECK_ENV(env);                                                                                                \
        napi_jsc_clear_error(env);                                                                                     \
        if ((env)->pending_exception != nullptr)                                                                       \
            return napi_jsc_set_error((env), napi_pending_exception);                                                  \
    } while (0)

#endif  // SRC_JSC_JS_NATIVE_API_JSC_H_
