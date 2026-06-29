// JSC Node-API backend — core surface.
//
// Values, strings, numbers, booleans, handle scopes, typeof/coercion, script
// evaluation, errors/exceptions, env bookkeeping, and the shared JSClass
// callbacks (function/constructor trampolines, external finalizer). Objects,
// functions, classes, wrap/external, references, promises and the remaining
// surface live in jsc_object.cc.

#include "js_native_api_jsc.h"

#include <climits>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <string>

#define NAPI_EXPERIMENTAL
#include "napi/node_api.h"

namespace {

// Callback info passed (as napi_callback_info) into a user napi_callback.
struct CbInfo {
    JSContextRef ctx;
    JSObjectRef this_obj;
    size_t argc;
    const JSValueRef* argv;
    JSObjectRef new_target;  // non-null only for a `new` (constructor) call
    void* data;
};

const char* kErrorMessages[] = {
    nullptr,                            // napi_ok
    "Invalid argument",                 // napi_invalid_arg
    "An object was expected",           // napi_object_expected
    "A string was expected",            // napi_string_expected
    "A string or symbol was expected",  // napi_name_expected (matches Node/V8 wording)
    "A function was expected",          // napi_function_expected
    "A number was expected",            // napi_number_expected
    "A boolean was expected",           // napi_boolean_expected
    "An array was expected",            // napi_array_expected
    "Unknown failure",                  // napi_generic_failure
    "An exception is pending",          // napi_pending_exception
    "The async work item was cancelled",// napi_cancelled
    "napi_escape_handle already called",// napi_escape_called_twice
    "Invalid handle scope usage",       // napi_handle_scope_mismatch
    "Invalid callback scope usage",     // napi_callback_scope_mismatch
    "Thread-safe function queue is full",// napi_queue_full
    "Thread-safe function handle is closing",// napi_closing
    "A bigint was expected",            // napi_bigint_expected
    "A date was expected",              // napi_date_expected
    "An arraybuffer was expected",      // napi_arraybuffer_expected
    "A detachable arraybuffer was expected",// napi_detachable_arraybuffer_expected
    "Main thread would deadlock",       // napi_would_deadlock
    "External buffers are not allowed", // napi_no_external_buffers_allowed
    "Cannot run JavaScript",            // napi_cannot_run_js
};

// JSStringRef RAII.
struct JSStr {
    JSStringRef s;
    explicit JSStr(const char* utf8) : s(JSStringCreateWithUTF8CString(utf8)) {}
    JSStr(const char* utf8, size_t len) {
        std::string z(utf8, len);
        s = JSStringCreateWithUTF8CString(z.c_str());
    }
    ~JSStr() { if (s) JSStringRelease(s); }
    operator JSStringRef() const { return s; }
};

JSValueRef GetMember(JSContextRef ctx, JSObjectRef obj, const char* name) {
    JSStr key(name);
    return JSObjectGetProperty(ctx, obj, key, nullptr);
}

// ---- shared JSClass callbacks ---------------------------------------------

void ExternalFinalize(JSObjectRef obj) {
    auto* st = static_cast<ExternalState*>(JSObjectGetPrivate(obj));
    if (st == nullptr)
        return;
    // The anchor object was collected: empty every weak ref pointing at it
    // FIRST, then defer the user finalizer out of GC (we must not re-enter JS
    // here). Clearing before the deferral is what lets a wrap / add_finalizer
    // finalizer — which runs later, on drain — observe sibling weak refs to the
    // same object as already-empty, regardless of GC finalize order across
    // arches (the bug test_reference exercises on arm64). AME-JSC-REFORDER-FIX
    for (auto& rc : st->ref_controls) {
        rc->alive = false;
        rc->value = nullptr;
    }
    if (st->finalize_cb != nullptr && st->env != nullptr) {
        std::lock_guard<std::mutex> lk(st->env->finalizer_mu);
        st->env->pending_finalizers.push_back({st->finalize_cb, st->data, st->finalize_hint, st->basic});
    }
    delete st;
}

void CallbackFinalize(JSObjectRef obj) {
    delete static_cast<CallbackData*>(JSObjectGetPrivate(obj));
}

// Move any exception the callback left pending into JSC's out-param.
void ForwardPendingException(napi_env env, JSValueRef* exception) {
    if (env->pending_exception == nullptr)
        return;
    if (exception != nullptr)
        *exception = env->pending_exception;
    // Ownership transfers to JSC's exception machinery; balance our protect.
    JSValueUnprotect(env->ctx, env->pending_exception);
    env->pending_exception = nullptr;
}

// AME-JSC-CBSCOPE-FIX: every native callback must run inside its own handle scope.
// napi_get_cb_info (and most napi_* getters) protect each value via
// napi_jsc_add_handle and stash it in the current scope; without a per-callback
// scope those handles accrue in the *root* scope and stay JSValueProtect'd until
// env teardown — so arguments (and anything derived) are never collected and
// finalizers/weak refs never fire. Mirror Node/V8: push a scope on entry, release
// its transient handles on exit, escaping the result to the parent scope so it
// survives. RAII so early returns can't leak the scope.
struct CallbackScope {
    napi_env env;
    napi_handle_scope__* s;
    explicit CallbackScope(napi_env e) : env(e), s(new napi_handle_scope__()) {
        env->scopes.push_back(s);
        // V8 parity: each native callback enters with a clean error slate, so a
        // stale napi_status from an earlier unrelated call doesn't leak into this
        // one (test_general/testNapiStatus relies on the second call seeing napi_ok).
        napi_jsc_clear_error(env);
    }
    // Close the scope, releasing every transient handle it accumulated. The
    // callback's *return* value is deliberately NOT escaped/protected here: once a
    // JSValueRef is handed back to JSC as a callback result, JSC roots it for the
    // duration of the JS call (and thereafter the value's lifetime is governed by
    // JS references — exactly as a V8 napi return value is). Escaping it into the
    // parent scope instead would pin it for the parent's whole lifetime; since the
    // test runner executes the entire script in the root scope, that pinned every
    // napi-returned object until env teardown → such objects were never collected
    // and their finalizers never fired. Callers compute their JSValueRef result
    // before close() and return it immediately (no allocation in between, so no GC
    // can reclaim it before JSC roots it). Tolerates a misbehaving callback that
    // left extra scopes open by unwinding down to ours.
    void close() {
        while (env->scopes.size() > 1 && env->scopes.back() != s) {
            auto* extra = env->scopes.back();
            env->scopes.pop_back();
            for (JSValueRef h : extra->handles)
                JSValueUnprotect(env->ctx, h);
            delete extra;
        }
        env->scopes.pop_back();
        for (JSValueRef h : s->handles)
            JSValueUnprotect(env->ctx, h);
        delete s;
        s = nullptr;
    }
    ~CallbackScope() {
        if (s != nullptr)
            close();
    }
};

JSValueRef FunctionTrampoline(JSContextRef ctx, JSObjectRef function, JSObjectRef thisObject, size_t argc,
                              const JSValueRef argv[], JSValueRef* exception) {
    auto* d = static_cast<CallbackData*>(JSObjectGetPrivate(function));
    if (d == nullptr || d->cb == nullptr)
        return JSValueMakeUndefined(ctx);
    CbInfo info{ctx, thisObject, argc, argv, nullptr, d->data};
    CallbackScope scope(d->env);
    napi_value ret = d->cb(d->env, reinterpret_cast<napi_callback_info>(&info));
    if (d->env->pending_exception != nullptr) {
        scope.close();
        ForwardPendingException(d->env, exception);
        return JSValueMakeUndefined(ctx);
    }
    JSValueRef out = ret != nullptr ? ToJS(ret) : JSValueMakeUndefined(ctx);
    scope.close();
    return out;
}

JSObjectRef ConstructorTrampoline(JSContextRef ctx, JSObjectRef constructor, size_t argc, const JSValueRef argv[],
                                  JSValueRef* exception) {
    auto* d = static_cast<CallbackData*>(JSObjectGetPrivate(constructor));
    JSObjectRef instance = JSObjectMake(ctx, nullptr, nullptr);
    JSValueRef proto = GetMember(ctx, constructor, "prototype");
    if (proto != nullptr && JSValueIsObject(ctx, proto))
        JSObjectSetPrototype(ctx, instance, proto);
    if (d == nullptr || d->cb == nullptr)
        return instance;
    CbInfo info{ctx, instance, argc, argv, constructor, d->data};
    CallbackScope scope(d->env);
    napi_value ret = d->cb(d->env, reinterpret_cast<napi_callback_info>(&info));
    if (d->env->pending_exception != nullptr) {
        scope.close();
        ForwardPendingException(d->env, exception);
        return instance;
    }
    JSObjectRef out = (ret != nullptr && JSValueIsObject(ctx, ToJS(ret))) ? JSValueToObject(ctx, ToJS(ret), nullptr)
                                                                          : instance;
    scope.close();
    return out;
}

// Native init behind a real-JS-function constructor (AME-JSC-NEWTARGET-FIX).
// The JS trampoline (see napi_define_class) calls us as
//   __init(new.target, this, args)
// so argv[0]=new.target, argv[1]=`this` (the instance JSC already created with
// new.target.prototype — correct even for `class Sub extends Ctor`), argv[2]=the
// user args array. We repackage into a constructor CbInfo (this + new_target +
// spread args) and run the napi callback. Making napi classes *real* functions is
// what gives them correct subclassing/new.target, a real `.length`, native
// `instanceof`, and a non-enumerable `prototype.constructor` — none achievable via
// JSC's C-API callAsConstructor object.
JSValueRef ConstructorInitTrampoline(JSContextRef ctx, JSObjectRef function, JSObjectRef /*thisObject*/, size_t argc,
                                     const JSValueRef argv[], JSValueRef* exception) {
    auto* d = static_cast<CallbackData*>(JSObjectGetPrivate(function));
    if (d == nullptr || d->cb == nullptr)
        return JSValueMakeUndefined(ctx);
    JSObjectRef new_target =
        (argc > 0 && JSValueIsObject(ctx, argv[0])) ? JSValueToObject(ctx, argv[0], nullptr) : nullptr;
    JSObjectRef instance =
        (argc > 1 && JSValueIsObject(ctx, argv[1])) ? JSValueToObject(ctx, argv[1], nullptr) : nullptr;
    // Spread the user args array (argv[2]) back into a flat vector for CbInfo.
    std::vector<JSValueRef> user_args;
    if (argc > 2 && JSValueIsObject(ctx, argv[2])) {
        JSObjectRef arr = JSValueToObject(ctx, argv[2], nullptr);
        JSStr len_key("length");
        double n = JSValueToNumber(ctx, JSObjectGetProperty(ctx, arr, len_key, nullptr), nullptr);
        size_t count = (n == n && n > 0) ? static_cast<size_t>(n) : 0;  // NaN-safe
        user_args.reserve(count);
        for (size_t i = 0; i < count; ++i)
            user_args.push_back(JSObjectGetPropertyAtIndex(ctx, arr, static_cast<unsigned>(i), nullptr));
    }
    CbInfo info{ctx, instance, user_args.size(), user_args.empty() ? nullptr : user_args.data(), new_target, d->data};
    CallbackScope scope(d->env);
    napi_value ret = d->cb(d->env, reinterpret_cast<napi_callback_info>(&info));
    if (d->env->pending_exception != nullptr) {
        scope.close();
        ForwardPendingException(d->env, exception);
        return JSValueMakeUndefined(ctx);
    }
    JSValueRef out = ret != nullptr ? ToJS(ret) : JSValueMakeUndefined(ctx);
    scope.close();
    return out;
}

// Invoked when a define_class constructor is called without `new`. Matches
// JSObjectCallAsFunctionCallback (returns JSValueRef); throws a TypeError.
JSValueRef ConstructorAsFunction(JSContextRef ctx, JSObjectRef, JSObjectRef, size_t, const JSValueRef[],
                                 JSValueRef* exception) {  // AME-JSC-CTORTYPEERR-FIX
    if (exception != nullptr) {
        JSValueRef msg = JSValueMakeString(ctx, JSStr("Class constructor cannot be invoked without 'new'"));
        // AmeCanvas fix: calling a constructor without `new` must throw a *TypeError*
        // (Web IDL / ES), not a generic Error. JSObjectMakeError makes an Error, so
        // construct a real TypeError via the global constructor; fall back to Error.
        JSValueRef teVal =
            JSObjectGetProperty(ctx, JSContextGetGlobalObject(ctx), JSStr("TypeError"), nullptr);
        JSObjectRef teCtor =
            (teVal != nullptr && JSValueIsObject(ctx, teVal)) ? JSValueToObject(ctx, teVal, nullptr) : nullptr;
        JSValueRef err = nullptr;
        if (teCtor != nullptr)
            err = JSObjectCallAsConstructor(ctx, teCtor, 1, &msg, nullptr);
        *exception = err != nullptr ? err : JSObjectMakeError(ctx, 1, &msg, nullptr);
    }
    return JSValueMakeUndefined(ctx);
}

}  // namespace

// ---- shared helpers (declared in js_native_api_jsc.h) ---------------------

napi_value napi_jsc_add_handle(napi_env env, JSValueRef v) {
    if (v == nullptr)
        return nullptr;
    JSValueProtect(env->ctx, v);
    env->scopes.back()->handles.push_back(v);
    return ToNapi(v);
}

napi_status napi_jsc_set_error(napi_env env, napi_status status) {
    if (env == nullptr)
        return status;
    env->last_error.error_code = status;
    env->last_error.engine_error_code = 0;
    env->last_error.engine_reserved = nullptr;
    env->last_error.error_message =
        (status >= 0 && status < static_cast<int>(sizeof(kErrorMessages) / sizeof(kErrorMessages[0])))
            ? kErrorMessages[status]
            : nullptr;
    return status;
}

napi_status napi_jsc_clear_error(napi_env env) {
    env->last_error.error_code = napi_ok;
    env->last_error.error_message = nullptr;
    env->last_error.engine_error_code = 0;
    env->last_error.engine_reserved = nullptr;
    return napi_ok;
}

napi_status napi_jsc_record_exception(napi_env env, JSValueRef exc) {
    if (exc == nullptr)
        return napi_ok;
    if (env->pending_exception != nullptr)
        JSValueUnprotect(env->ctx, env->pending_exception);
    JSValueProtect(env->ctx, exc);
    env->pending_exception = exc;
    return napi_jsc_set_error(env, napi_pending_exception);
}

void napi_jsc_fatal_in_finalizer() {
    // Matches Node's message (test_fatal_finalize asserts /Finalizer is calling a
    // function that may affect GC state/ on the aborted child's stderr).
    std::fprintf(stderr,
                 "FATAL ERROR: Finalizer is calling a function that may affect GC state.\n"
                 "The finalizers stored by `napi_add_finalizer` should only call basic Node-API "
                 "methods. Use `node_api_post_finalizer` from inside of the finalizer to work "
                 "around this issue.\n");
    std::fflush(stderr);
    std::abort();
}

void napi_jsc_drain_finalizers(napi_env env) {
    for (;;) {
        PendingFinalizer f;
        {
            std::lock_guard<std::mutex> lk(env->finalizer_mu);
            if (env->pending_finalizers.empty())
                break;
            f = env->pending_finalizers.front();
            env->pending_finalizers.pop_front();
        }
        if (f.cb == nullptr)
            continue;
        // A basic (napi_wrap / napi_add_finalizer) finalizer runs with the GC-guard
        // set: any non-basic Node-API it calls hits NAPI_PREAMBLE's fatal check. A
        // regular (napi_create_external) finalizer may call JS, so it runs unguarded.
        env->in_gc_finalizer = f.basic;
        f.cb(env, f.data, f.hint);
        env->in_gc_finalizer = false;
        // AME-JSC-FINALIZER-UNCAUGHT-FIX: a finalizer that runs JS may throw. Node
        // routes such an error to process 'uncaughtException' (and, with no handler,
        // reports "Error during Finalize"). Surface it through the host convention
        // globalThis.__emitUncaughtException(err) if present, else the env's string
        // uncaught hook; then clear so the remaining finalizers still run.
        if (env->pending_exception != nullptr) {
            JSValueRef err = env->pending_exception;
            env->pending_exception = nullptr;  // take ownership of the protect
            JSObjectRef global = JSContextGetGlobalObject(env->ctx);
            JSValueRef emit = JSObjectGetProperty(env->ctx, global, JSStr("__emitUncaughtException"), nullptr);
            bool surfaced = false;
            if (emit != nullptr && JSValueIsObject(env->ctx, emit)) {
                JSObjectRef emitFn = JSValueToObject(env->ctx, emit, nullptr);
                if (emitFn != nullptr && JSObjectIsFunction(env->ctx, emitFn)) {
                    JSValueRef a = err;
                    JSObjectCallAsFunction(env->ctx, emitFn, nullptr, 1, &a, nullptr);
                    surfaced = true;
                }
            }
            if (!surfaced && env->uncaught != nullptr) {
                JSStringRef s = JSValueToStringCopy(env->ctx, err, nullptr);
                if (s != nullptr) {
                    size_t n = JSStringGetMaximumUTF8CStringSize(s);
                    std::string buf(n, '\0');
                    JSStringGetUTF8CString(s, buf.data(), n);
                    env->uncaught(buf.c_str());
                    JSStringRelease(s);
                }
            }
            JSValueUnprotect(env->ctx, err);
            napi_jsc_clear_error(env);
        }
    }
}

// ---- env lifecycle --------------------------------------------------------

napi_env napi_jsc_env_new(JSGlobalContextRef ctx, void (*uncaught)(const char*)) {
    auto* env = new napi_env__();
    env->ctx = JSGlobalContextRetain(ctx);
    env->uncaught = uncaught;

    JSClassDefinition ext = kJSClassDefinitionEmpty;
    ext.className = "NapiExternal";
    ext.finalize = ExternalFinalize;
    env->external_class = JSClassCreate(&ext);

    JSClassDefinition fn = kJSClassDefinitionEmpty;
    fn.className = "NapiFunction";
    fn.callAsFunction = FunctionTrampoline;
    fn.finalize = CallbackFinalize;
    env->function_class = JSClassCreate(&fn);

    JSClassDefinition ctor = kJSClassDefinitionEmpty;
    ctor.className = "NapiClass";
    ctor.callAsConstructor = ConstructorTrampoline;
    ctor.callAsFunction = ConstructorAsFunction;
    ctor.finalize = CallbackFinalize;
    env->constructor_class = JSClassCreate(&ctor);

    JSClassDefinition ctor_init = kJSClassDefinitionEmpty;  // AME-JSC-NEWTARGET-FIX
    ctor_init.className = "NapiClassInit";
    ctor_init.callAsFunction = ConstructorInitTrampoline;
    ctor_init.finalize = CallbackFinalize;
    env->ctor_init_class = JSClassCreate(&ctor_init);

    env->scopes.push_back(new napi_handle_scope__());  // root scope

    JSObjectRef global = JSContextGetGlobalObject(ctx);
    auto cache_fn = [&](JSObjectRef holder, const char* name) -> JSObjectRef {
        JSValueRef v = GetMember(ctx, holder, name);
        if (v == nullptr || !JSValueIsObject(ctx, v))
            return nullptr;
        JSObjectRef o = JSValueToObject(ctx, v, nullptr);
        if (o != nullptr)
            JSValueProtect(ctx, o);
        return o;
    };
    JSObjectRef object_ctor = nullptr;
    if (JSValueRef ov = GetMember(ctx, global, "Object"); ov && JSValueIsObject(ctx, ov))
        object_ctor = JSValueToObject(ctx, ov, nullptr);
    if (object_ctor != nullptr) {
        env->obj_define_property = cache_fn(object_ctor, "defineProperty");
        env->obj_freeze = cache_fn(object_ctor, "freeze");
        env->obj_seal = cache_fn(object_ctor, "seal");
        if (JSValueRef pv = GetMember(ctx, object_ctor, "prototype"); pv && JSValueIsObject(ctx, pv)) {
            JSObjectRef proto = JSValueToObject(ctx, pv, nullptr);
            env->obj_has_own = cache_fn(proto, "hasOwnProperty");
        }
    }
    if (JSValueRef sv = GetMember(ctx, global, "Symbol"); sv && JSValueIsObject(ctx, sv)) {
        JSObjectRef symbol_ctor = JSValueToObject(ctx, sv, nullptr);
        env->symbol_for = cache_fn(symbol_ctor, "for");
    }
    env->wrap_key = JSValueMakeSymbol(ctx, JSStr("napi.wrap"));
    if (env->wrap_key != nullptr)
        JSValueProtect(ctx, env->wrap_key);
    env->type_tag_key = JSValueMakeSymbol(ctx, JSStr("napi.typeTag"));
    if (env->type_tag_key != nullptr)
        JSValueProtect(ctx, env->type_tag_key);

    // SharedArrayBuffer constructor (present only if the engine enabled SAB; see
    // the JSC_useSharedArrayBuffer constructor in napi_jsc_engine.cc) + the tag
    // symbol for fallback ArrayBuffer-backed shared buffers.
    env->shared_arraybuffer_ctor = cache_fn(global, "SharedArrayBuffer");
    env->sab_tag = JSValueMakeSymbol(ctx, JSStr("napi.sab"));
    if (env->sab_tag != nullptr)
        JSValueProtect(ctx, env->sab_tag);

    return env;
}

void napi_jsc_env_delete(napi_env env) {
    if (env == nullptr)
        return;

    napi_jsc_drain_finalizers(env);
    if (env->instance_data_finalize != nullptr)
        env->instance_data_finalize(env, env->instance_data, env->instance_data_hint);

    auto unprotect = [&](JSValueRef v) { if (v) JSValueUnprotect(env->ctx, v); };
    unprotect(env->obj_define_property);
    unprotect(env->obj_has_own);
    unprotect(env->obj_freeze);
    unprotect(env->obj_seal);
    unprotect(env->symbol_for);
    unprotect(env->wrap_key);
    unprotect(env->type_tag_key);
    unprotect(env->shared_arraybuffer_ctor);
    unprotect(env->sab_tag);
    unprotect(env->ctor_factory);
    unprotect(env->fn_factory);
    unprotect(env->pending_exception);

    for (auto* scope : env->scopes) {
        for (JSValueRef h : scope->handles)
            JSValueUnprotect(env->ctx, h);
        delete scope;
    }
    env->scopes.clear();

    if (env->external_class) JSClassRelease(env->external_class);
    if (env->function_class) JSClassRelease(env->function_class);
    if (env->constructor_class) JSClassRelease(env->constructor_class);
    if (env->ctor_init_class) JSClassRelease(env->ctor_init_class);

    JSGlobalContextRelease(env->ctx);   // fires remaining finalizes -> enqueue
    napi_jsc_drain_finalizers(env);     // best-effort (ctx gone; callbacks free memory)

    delete env;
}

// ---- error / exception surface --------------------------------------------

napi_status NAPI_CDECL napi_get_last_error_info(napi_env env, const napi_extended_error_info** result) {
    CHECK_ENV(env);
    CHECK_ARG(env, result);
    *result = &env->last_error;
    return napi_ok;
}

napi_status NAPI_CDECL napi_is_exception_pending(napi_env env, bool* result) {
    CHECK_ENV(env);
    CHECK_ARG(env, result);
    *result = env->pending_exception != nullptr;
    return napi_jsc_clear_error(env);
}

napi_status NAPI_CDECL napi_get_and_clear_last_exception(napi_env env, napi_value* result) {
    CHECK_ENV(env);
    CHECK_ARG(env, result);
    if (env->pending_exception == nullptr) {
        *result = napi_jsc_add_handle(env, JSValueMakeUndefined(env->ctx));
        return napi_jsc_clear_error(env);
    }
    *result = napi_jsc_add_handle(env, env->pending_exception);
    JSValueUnprotect(env->ctx, env->pending_exception);
    env->pending_exception = nullptr;
    return napi_jsc_clear_error(env);
}

napi_status NAPI_CDECL napi_throw(napi_env env, napi_value error) {
    CHECK_ENV(env);
    CHECK_ARG(env, error);
    napi_jsc_clear_error(env);
    if (env->pending_exception != nullptr)
        JSValueUnprotect(env->ctx, env->pending_exception);
    env->pending_exception = ToJS(error);
    JSValueProtect(env->ctx, env->pending_exception);
    return napi_ok;
}

namespace {
napi_status ThrowNamed(napi_env env, const char* code, const char* msg, const char* ctor_name) {
    JSContextRef ctx = env->ctx;
    JSValueRef m = JSValueMakeString(ctx, JSStr(msg));
    JSValueRef exc = nullptr;
    JSObjectRef err = nullptr;
    if (ctor_name == nullptr) {
        err = JSObjectMakeError(ctx, 1, &m, &exc);
    } else {
        JSObjectRef ctor = nullptr;
        if (JSValueRef cv = GetMember(ctx, JSContextGetGlobalObject(ctx), ctor_name);
            cv && JSValueIsObject(ctx, cv))
            ctor = JSValueToObject(ctx, cv, nullptr);
        if (ctor != nullptr)
            err = JSObjectCallAsConstructor(ctx, ctor, 1, &m, &exc);
        if (err == nullptr)
            err = JSObjectMakeError(ctx, 1, &m, &exc);
    }
    if (err == nullptr)
        return napi_jsc_set_error(env, napi_generic_failure);
    if (code != nullptr) {
        JSStr ck("code");
        JSObjectSetProperty(ctx, err, ck, JSValueMakeString(ctx, JSStr(code)), kJSPropertyAttributeNone, nullptr);
    }
    if (env->pending_exception != nullptr)
        JSValueUnprotect(ctx, env->pending_exception);
    env->pending_exception = err;
    JSValueProtect(ctx, env->pending_exception);
    return napi_ok;
}
}  // namespace

napi_status NAPI_CDECL napi_throw_error(napi_env env, const char* code, const char* msg) {
    CHECK_ENV(env);
    return ThrowNamed(env, code, msg, nullptr);
}
napi_status NAPI_CDECL napi_throw_type_error(napi_env env, const char* code, const char* msg) {
    CHECK_ENV(env);
    return ThrowNamed(env, code, msg, "TypeError");
}
napi_status NAPI_CDECL napi_throw_range_error(napi_env env, const char* code, const char* msg) {
    CHECK_ENV(env);
    return ThrowNamed(env, code, msg, "RangeError");
}
napi_status NAPI_CDECL node_api_throw_syntax_error(napi_env env, const char* code, const char* msg) {
    CHECK_ENV(env);
    return ThrowNamed(env, code, msg, "SyntaxError");
}

namespace {
napi_status CreateError(napi_env env, napi_value code, napi_value msg, const char* ctor_name, napi_value* result) {
    NAPI_PREAMBLE(env);
    CHECK_ARG(env, msg);
    CHECK_ARG(env, result);
    RETURN_STATUS_IF_FALSE(env, JSValueIsString(env->ctx, ToJS(msg)), napi_string_expected);
    JSContextRef ctx = env->ctx;
    JSValueRef m = ToJS(msg);
    JSValueRef exc = nullptr;
    JSObjectRef err = nullptr;
    if (ctor_name == nullptr) {
        err = JSObjectMakeError(ctx, 1, &m, &exc);
    } else {
        JSObjectRef ctor = nullptr;
        if (JSValueRef cv = GetMember(ctx, JSContextGetGlobalObject(ctx), ctor_name);
            cv && JSValueIsObject(ctx, cv))
            ctor = JSValueToObject(ctx, cv, nullptr);
        if (ctor != nullptr)
            err = JSObjectCallAsConstructor(ctx, ctor, 1, &m, &exc);
    }
    if (exc != nullptr)
        return napi_jsc_record_exception(env, exc);
    if (err == nullptr)
        return napi_jsc_set_error(env, napi_generic_failure);
    if (code != nullptr) {
        JSStr ck("code");
        JSObjectSetProperty(ctx, err, ck, ToJS(code), kJSPropertyAttributeNone, nullptr);
    }
    *result = napi_jsc_add_handle(env, err);
    return napi_ok;
}
}  // namespace

napi_status NAPI_CDECL napi_create_error(napi_env env, napi_value code, napi_value msg, napi_value* result) {
    return CreateError(env, code, msg, nullptr, result);
}
napi_status NAPI_CDECL napi_create_type_error(napi_env env, napi_value code, napi_value msg, napi_value* result) {
    return CreateError(env, code, msg, "TypeError", result);
}
napi_status NAPI_CDECL napi_create_range_error(napi_env env, napi_value code, napi_value msg, napi_value* result) {
    return CreateError(env, code, msg, "RangeError", result);
}
napi_status NAPI_CDECL node_api_create_syntax_error(napi_env env, napi_value code, napi_value msg,
                                                    napi_value* result) {
    return CreateError(env, code, msg, "SyntaxError", result);
}

napi_status NAPI_CDECL napi_is_error(napi_env env, napi_value value, bool* result) {
    CHECK_ENV(env);
    CHECK_ARG(env, value);
    CHECK_ARG(env, result);
    JSContextRef ctx = env->ctx;
    bool is = false;
    if (JSValueIsObject(ctx, ToJS(value))) {
        if (JSValueRef ev = GetMember(ctx, JSContextGetGlobalObject(ctx), "Error");
            ev && JSValueIsObject(ctx, ev)) {
            JSObjectRef err_ctor = JSValueToObject(ctx, ev, nullptr);
            is = JSValueIsInstanceOfConstructor(ctx, ToJS(value), err_ctor, nullptr);
        }
    }
    *result = is;
    return napi_jsc_clear_error(env);
}

// ---- simple value getters -------------------------------------------------

napi_status NAPI_CDECL napi_get_undefined(napi_env env, napi_value* result) {
    CHECK_ENV(env);
    CHECK_ARG(env, result);
    *result = napi_jsc_add_handle(env, JSValueMakeUndefined(env->ctx));
    return napi_jsc_clear_error(env);
}

napi_status NAPI_CDECL napi_get_null(napi_env env, napi_value* result) {
    CHECK_ENV(env);
    CHECK_ARG(env, result);
    *result = napi_jsc_add_handle(env, JSValueMakeNull(env->ctx));
    return napi_jsc_clear_error(env);
}

napi_status NAPI_CDECL napi_get_global(napi_env env, napi_value* result) {
    CHECK_ENV(env);
    CHECK_ARG(env, result);
    *result = napi_jsc_add_handle(env, JSContextGetGlobalObject(env->ctx));
    return napi_jsc_clear_error(env);
}

napi_status NAPI_CDECL napi_get_boolean(napi_env env, bool value, napi_value* result) {
    CHECK_ENV(env);
    CHECK_ARG(env, result);
    *result = napi_jsc_add_handle(env, JSValueMakeBoolean(env->ctx, value));
    return napi_jsc_clear_error(env);
}

// ---- number constructors --------------------------------------------------

napi_status NAPI_CDECL napi_create_double(napi_env env, double value, napi_value* result) {
    CHECK_ENV(env);
    CHECK_ARG(env, result);
    *result = napi_jsc_add_handle(env, JSValueMakeNumber(env->ctx, value));
    return napi_jsc_clear_error(env);
}
napi_status NAPI_CDECL napi_create_int32(napi_env env, int32_t value, napi_value* result) {
    return napi_create_double(env, static_cast<double>(value), result);
}
napi_status NAPI_CDECL napi_create_uint32(napi_env env, uint32_t value, napi_value* result) {
    return napi_create_double(env, static_cast<double>(value), result);
}
napi_status NAPI_CDECL napi_create_int64(napi_env env, int64_t value, napi_value* result) {
    return napi_create_double(env, static_cast<double>(value), result);
}

// ---- number getters -------------------------------------------------------

napi_status NAPI_CDECL napi_get_value_double(napi_env env, napi_value value, double* result) {
    CHECK_ENV(env);
    CHECK_ARG(env, value);
    CHECK_ARG(env, result);
    RETURN_STATUS_IF_FALSE(env, JSValueIsNumber(env->ctx, ToJS(value)), napi_number_expected);
    *result = JSValueToNumber(env->ctx, ToJS(value), nullptr);
    return napi_jsc_clear_error(env);
}

namespace {
// ECMAScript ToInt32 / ToUint32 on an already-numeric double.
int64_t ToInteger(double d) {  // AME-JSC-INT64-CLAMP-FIX
    // napi_get_value_int64 semantics (matches Node's napi + V8 NumberToInt64):
    // non-finite (NaN/±Inf) → 0; finite but out of int64 range → clamp to
    // INT64_MAX / INT64_MIN (NOT a raw cast — casting an out-of-range double to
    // int64 is UB and on x86 yields INT64_MIN for *both* overflow directions,
    // so positive overflow wrongly came back negative). In-range → truncate.
    if (!std::isfinite(d))
        return 0;
    if (d >= 9223372036854775808.0)  // 2^63, first double past INT64_MAX
        return std::numeric_limits<int64_t>::max();
    if (d <= -9223372036854775808.0)  // -2^63 == INT64_MIN
        return std::numeric_limits<int64_t>::min();
    return static_cast<int64_t>(d < 0 ? std::ceil(d) : std::floor(d));
}
uint32_t ToUint32(double d) {
    if (std::isnan(d) || std::isinf(d))
        return 0;
    double m = std::fmod(std::trunc(d), 4294967296.0);
    if (m < 0)
        m += 4294967296.0;
    return static_cast<uint32_t>(m);
}
}  // namespace

napi_status NAPI_CDECL napi_get_value_int32(napi_env env, napi_value value, int32_t* result) {
    CHECK_ENV(env);
    CHECK_ARG(env, value);
    CHECK_ARG(env, result);
    RETURN_STATUS_IF_FALSE(env, JSValueIsNumber(env->ctx, ToJS(value)), napi_number_expected);
    *result = static_cast<int32_t>(ToUint32(JSValueToNumber(env->ctx, ToJS(value), nullptr)));
    return napi_jsc_clear_error(env);
}
napi_status NAPI_CDECL napi_get_value_uint32(napi_env env, napi_value value, uint32_t* result) {
    CHECK_ENV(env);
    CHECK_ARG(env, value);
    CHECK_ARG(env, result);
    RETURN_STATUS_IF_FALSE(env, JSValueIsNumber(env->ctx, ToJS(value)), napi_number_expected);
    *result = ToUint32(JSValueToNumber(env->ctx, ToJS(value), nullptr));
    return napi_jsc_clear_error(env);
}
napi_status NAPI_CDECL napi_get_value_int64(napi_env env, napi_value value, int64_t* result) {
    CHECK_ENV(env);
    CHECK_ARG(env, value);
    CHECK_ARG(env, result);
    RETURN_STATUS_IF_FALSE(env, JSValueIsNumber(env->ctx, ToJS(value)), napi_number_expected);
    *result = ToInteger(JSValueToNumber(env->ctx, ToJS(value), nullptr));
    return napi_jsc_clear_error(env);
}

napi_status NAPI_CDECL napi_get_value_bool(napi_env env, napi_value value, bool* result) {
    CHECK_ENV(env);
    CHECK_ARG(env, value);
    CHECK_ARG(env, result);
    RETURN_STATUS_IF_FALSE(env, JSValueIsBoolean(env->ctx, ToJS(value)), napi_boolean_expected);
    *result = JSValueToBoolean(env->ctx, ToJS(value));
    return napi_jsc_clear_error(env);
}

// ---- strings --------------------------------------------------------------

napi_status NAPI_CDECL napi_create_string_utf8(napi_env env, const char* str, size_t length, napi_value* result) {  // AME-JSC-STR8-FIX
    CHECK_ENV(env);
    CHECK_ARG(env, result);
    if (length > 0)  // V8 parity: NULL str only allowed with length 0 (AUTO_LENGTH=SIZE_MAX>0 → required)
        CHECK_ARG(env, str);
    // V8 parity: an explicit byte length above INT_MAX is rejected (else the decode
    // loop reads far past the buffer → OOB crash). test_string TestLargeUtf8.
    RETURN_STATUS_IF_FALSE(env, length == NAPI_AUTO_LENGTH || length <= static_cast<size_t>(INT_MAX),
                           napi_invalid_arg);
    const char* p = str ? str : "";
    size_t n = (length == NAPI_AUTO_LENGTH) ? std::strlen(p) : length;
    // AmeCanvas fix: decode exactly `n` UTF-8 bytes to UTF-16 (U+FFFD for invalid
    // sequences) and build via JSStringCreateWithCharacters. The original
    // JSStringCreateWithUTF8CString path is NUL-terminated, so it truncated strings
    // at an embedded `\0` (e.g. URLSearchParams "%00" → "b\0c" became "b") and
    // lenient-mangled invalid UTF-8 / lone surrogates. (Also fixes the earlier
    // explicit-length temporary-UAF; node-addon-api String::New(const char*) passes
    // strlen, so this path is hit for every eval'd script.) See docs/JSC_INTEGRATION.md.
    const unsigned char* b = reinterpret_cast<const unsigned char*>(p);
    std::vector<JSChar> u16;
    u16.reserve(n);
    auto emit = [&](uint32_t cp) {
        if (cp <= 0xFFFF) {
            u16.push_back(static_cast<JSChar>(cp));
        } else {
            cp -= 0x10000;
            u16.push_back(static_cast<JSChar>(0xD800 + (cp >> 10)));
            u16.push_back(static_cast<JSChar>(0xDC00 + (cp & 0x3FF)));
        }
    };
    for (size_t i = 0; i < n;) {
        unsigned char c = b[i];
        if (c < 0x80) {
            emit(c);
            ++i;
            continue;
        }
        int extra;
        uint32_t acc, lo;
        if ((c & 0xE0) == 0xC0) { extra = 1; acc = c & 0x1F; lo = 0x80; }
        else if ((c & 0xF0) == 0xE0) { extra = 2; acc = c & 0x0F; lo = 0x800; }
        else if ((c & 0xF8) == 0xF0) { extra = 3; acc = c & 0x07; lo = 0x10000; }
        else { emit(0xFFFD); ++i; continue; }  // invalid lead byte
        if (i + static_cast<size_t>(extra) >= n) { emit(0xFFFD); ++i; continue; }  // truncated
        bool ok = true;
        for (int k = 1; k <= extra; ++k) {
            unsigned char cc = b[i + k];
            if ((cc & 0xC0) != 0x80) { ok = false; break; }
            acc = (acc << 6) | (cc & 0x3F);
        }
        if (!ok) { emit(0xFFFD); ++i; continue; }  // bad continuation
        if (acc < lo || acc > 0x10FFFF || (acc >= 0xD800 && acc <= 0xDFFF)) { emit(0xFFFD); ++i; continue; }  // overlong/range/surrogate
        emit(acc);
        i += static_cast<size_t>(extra) + 1;
    }
    static const JSChar kEmpty = 0;
    JSStringRef s = JSStringCreateWithCharacters(u16.empty() ? &kEmpty : u16.data(), u16.size());
    JSValueRef v = JSValueMakeString(env->ctx, s);
    JSStringRelease(s);
    *result = napi_jsc_add_handle(env, v);
    return napi_jsc_clear_error(env);
}

napi_status NAPI_CDECL napi_create_string_utf16(napi_env env, const char16_t* str, size_t length,
                                                napi_value* result) {
    CHECK_ENV(env);
    CHECK_ARG(env, result);
    if (length > 0)  // V8 parity: NULL str only allowed with length 0
        CHECK_ARG(env, str);
    RETURN_STATUS_IF_FALSE(env, length == NAPI_AUTO_LENGTH || length <= static_cast<size_t>(INT_MAX),
                           napi_invalid_arg);  // V8 parity: reject oversize length (TestLargeUtf16)
    size_t n = (length == NAPI_AUTO_LENGTH) ? [&] {
        size_t i = 0;
        if (str) while (str[i] != 0) ++i;
        return i;
    }() : length;
    JSStringRef s = JSStringCreateWithCharacters(reinterpret_cast<const JSChar*>(str), n);
    JSValueRef v = JSValueMakeString(env->ctx, s);
    JSStringRelease(s);
    *result = napi_jsc_add_handle(env, v);
    return napi_jsc_clear_error(env);
}

napi_status NAPI_CDECL napi_create_string_latin1(napi_env env, const char* str, size_t length, napi_value* result) {
    CHECK_ENV(env);
    CHECK_ARG(env, result);
    if (length > 0)  // V8 parity: NULL str only allowed with length 0
        CHECK_ARG(env, str);
    RETURN_STATUS_IF_FALSE(env, length == NAPI_AUTO_LENGTH || length <= static_cast<size_t>(INT_MAX),
                           napi_invalid_arg);  // V8 parity: reject oversize length (TestLargeLatin1)
    size_t n = (length == NAPI_AUTO_LENGTH) ? (str ? std::strlen(str) : 0) : length;
    std::vector<JSChar> u16(n);
    for (size_t i = 0; i < n; ++i)
        u16[i] = static_cast<unsigned char>(str[i]);
    JSStringRef s = JSStringCreateWithCharacters(u16.data(), n);
    JSValueRef v = JSValueMakeString(env->ctx, s);
    JSStringRelease(s);
    *result = napi_jsc_add_handle(env, v);
    return napi_jsc_clear_error(env);
}

napi_status NAPI_CDECL napi_get_value_string_utf8(napi_env env, napi_value value, char* buf, size_t bufsize,
                                                  size_t* result) {  // AME-JSC-STR8READ-FIX
    CHECK_ENV(env);
    CHECK_ARG(env, value);
    RETURN_STATUS_IF_FALSE(env, JSValueIsString(env->ctx, ToJS(value)), napi_string_expected);
    JSStringRef s = JSValueToStringCopy(env->ctx, ToJS(value), nullptr);
    size_t len = JSStringGetLength(s);
    const JSChar* chars = JSStringGetCharactersPtr(s);
    // AmeCanvas fix: encode UTF-16 → UTF-8 mapping unpaired surrogates to U+FFFD
    // (WHATWG / matches V8). JSStringGetUTF8CString drops/garbles lone surrogates,
    // breaking round-trips of strings with unpaired surrogates (e.g. URLSearchParams
    // "\ud800x" should read back as "�x"). Length-based, so embedded NUL is safe.
    std::string out;
    out.reserve(len + 8);
    auto put = [&](uint32_t cp) {
        if (cp < 0x80) {
            out.push_back(static_cast<char>(cp));
        } else if (cp < 0x800) {
            out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        } else if (cp < 0x10000) {
            out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        } else {
            out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        }
    };
    for (size_t i = 0; i < len; ++i) {
        uint32_t u = chars[i];
        if (u >= 0xD800 && u <= 0xDBFF) {  // high surrogate
            if (i + 1 < len && chars[i + 1] >= 0xDC00 && chars[i + 1] <= 0xDFFF) {
                uint32_t lo = chars[++i];
                put(0x10000 + ((u - 0xD800) << 10) + (lo - 0xDC00));
            } else {
                put(0xFFFD);  // unpaired high
            }
        } else if (u >= 0xDC00 && u <= 0xDFFF) {  // unpaired low
            put(0xFFFD);
        } else {
            put(u);
        }
    }
    JSStringRelease(s);
    if (buf == nullptr) {
        CHECK_ARG(env, result);
        *result = out.size();
    } else {
        size_t copy = (bufsize == 0) ? 0 : std::min(out.size(), bufsize - 1);
        // AME-JSC-UTF8-TRUNC-FIX: never split a multi-byte UTF-8 sequence. If the
        // cut lands inside a character (the first excluded byte is a continuation
        // byte 0x80–0xBF), back off to the preceding character boundary so the
        // result is whole code points — matches V8's insufficient-buffer truncation
        // (test_string testUnicodeCases TestUtf8Insufficient).
        while (copy > 0 && copy < out.size() && (static_cast<unsigned char>(out[copy]) & 0xC0) == 0x80)
            --copy;
        if (copy)
            std::memcpy(buf, out.data(), copy);
        if (bufsize > 0)
            buf[copy] = 0;
        if (result != nullptr)
            *result = copy;
    }
    return napi_jsc_clear_error(env);
}

napi_status NAPI_CDECL napi_get_value_string_utf16(napi_env env, napi_value value, char16_t* buf, size_t bufsize,
                                                   size_t* result) {
    CHECK_ENV(env);
    CHECK_ARG(env, value);
    RETURN_STATUS_IF_FALSE(env, JSValueIsString(env->ctx, ToJS(value)), napi_string_expected);
    JSStringRef s = JSValueToStringCopy(env->ctx, ToJS(value), nullptr);
    size_t len = JSStringGetLength(s);
    const JSChar* chars = JSStringGetCharactersPtr(s);
    if (buf == nullptr) {
        CHECK_ARG(env, result);
        *result = len;
    } else {
        size_t copy = (bufsize == 0) ? 0 : std::min(len, bufsize - 1);
        for (size_t i = 0; i < copy; ++i)
            buf[i] = chars[i];
        if (bufsize > 0)
            buf[copy] = 0;
        if (result != nullptr)
            *result = copy;
    }
    JSStringRelease(s);
    return napi_jsc_clear_error(env);
}

napi_status NAPI_CDECL napi_get_value_string_latin1(napi_env env, napi_value value, char* buf, size_t bufsize,
                                                    size_t* result) {
    CHECK_ENV(env);
    CHECK_ARG(env, value);
    RETURN_STATUS_IF_FALSE(env, JSValueIsString(env->ctx, ToJS(value)), napi_string_expected);
    JSStringRef s = JSValueToStringCopy(env->ctx, ToJS(value), nullptr);
    size_t len = JSStringGetLength(s);
    const JSChar* chars = JSStringGetCharactersPtr(s);
    if (buf == nullptr) {
        CHECK_ARG(env, result);
        *result = len;
    } else {
        size_t copy = (bufsize == 0) ? 0 : std::min(len, bufsize - 1);
        for (size_t i = 0; i < copy; ++i)
            buf[i] = static_cast<char>(chars[i] & 0xFF);
        if (bufsize > 0)
            buf[copy] = 0;
        if (result != nullptr)
            *result = copy;
    }
    JSStringRelease(s);
    return napi_jsc_clear_error(env);
}

// ---- typeof / coercion / equality -----------------------------------------

napi_status NAPI_CDECL napi_typeof(napi_env env, napi_value value, napi_valuetype* result) {
    CHECK_ENV(env);
    CHECK_ARG(env, value);
    CHECK_ARG(env, result);
    JSContextRef ctx = env->ctx;
    JSValueRef v = ToJS(value);
    switch (JSValueGetType(ctx, v)) {
        case kJSTypeUndefined: *result = napi_undefined; break;
        case kJSTypeNull: *result = napi_null; break;
        case kJSTypeBoolean: *result = napi_boolean; break;
        case kJSTypeNumber: *result = napi_number; break;
        case kJSTypeString: *result = napi_string; break;
        case kJSTypeSymbol: *result = napi_symbol; break;
        // AmeCanvas fix: BigInt was missing — it fell through to the object branch and
        // typeof'd as napi_object, so WebIDL union/sequence conversions accepted BigInt
        // where they must throw TypeError (e.g. roundRect(0n), new Blob(7n)).
        // kJSTypeBigInt is macOS 15+/iOS 18+; harmless elsewhere (never returned). AME-JSC-BIGINT-TYPEOF-FIX
        case kJSTypeBigInt: *result = napi_bigint; break;
        case kJSTypeObject:
        default: {
            JSObjectRef obj = JSValueToObject(ctx, v, nullptr);
            if (obj != nullptr && JSValueIsObjectOfClass(ctx, v, env->external_class))
                *result = napi_external;
            else if (obj != nullptr && JSObjectIsFunction(ctx, obj))
                *result = napi_function;
            else
                *result = napi_object;
            break;
        }
    }
    return napi_jsc_clear_error(env);
}

napi_status NAPI_CDECL napi_coerce_to_bool(napi_env env, napi_value value, napi_value* result) {
    NAPI_PREAMBLE(env);
    CHECK_ARG(env, value);
    CHECK_ARG(env, result);
    *result = napi_jsc_add_handle(env, JSValueMakeBoolean(env->ctx, JSValueToBoolean(env->ctx, ToJS(value))));
    return napi_jsc_clear_error(env);
}
napi_status NAPI_CDECL napi_coerce_to_number(napi_env env, napi_value value, napi_value* result) {  // AME-JSC-BIGINT-TONUMBER-FIX
    NAPI_PREAMBLE(env);
    CHECK_ARG(env, value);
    CHECK_ARG(env, result);
    // AmeCanvas fix: ES ToNumber(BigInt) throws TypeError, but JSC's JSValueToNumber
    // is lenient on BigInt (returns NaN, no exception), so WebIDL `double` conversions
    // silently accepted BigInt (e.g. roundRect(0n) didn't throw). Reject explicitly.
    if (JSValueGetType(env->ctx, ToJS(value)) == kJSTypeBigInt) {
        napi_throw_type_error(env, nullptr, "Cannot convert a BigInt value to a number");
        return napi_jsc_set_error(env, napi_pending_exception);
    }
    JSValueRef exc = nullptr;
    double d = JSValueToNumber(env->ctx, ToJS(value), &exc);
    if (exc != nullptr)
        return napi_jsc_record_exception(env, exc);
    *result = napi_jsc_add_handle(env, JSValueMakeNumber(env->ctx, d));
    return napi_jsc_clear_error(env);
}
napi_status NAPI_CDECL napi_coerce_to_string(napi_env env, napi_value value, napi_value* result) {
    NAPI_PREAMBLE(env);
    CHECK_ARG(env, value);
    CHECK_ARG(env, result);
    JSValueRef exc = nullptr;
    JSStringRef s = JSValueToStringCopy(env->ctx, ToJS(value), &exc);
    if (exc != nullptr)
        return napi_jsc_record_exception(env, exc);
    JSValueRef v = JSValueMakeString(env->ctx, s);
    JSStringRelease(s);
    *result = napi_jsc_add_handle(env, v);
    return napi_jsc_clear_error(env);
}
napi_status NAPI_CDECL napi_coerce_to_object(napi_env env, napi_value value, napi_value* result) {
    NAPI_PREAMBLE(env);
    CHECK_ARG(env, value);
    CHECK_ARG(env, result);
    JSValueRef exc = nullptr;
    JSObjectRef o = JSValueToObject(env->ctx, ToJS(value), &exc);
    if (exc != nullptr)
        return napi_jsc_record_exception(env, exc);
    *result = napi_jsc_add_handle(env, o);
    return napi_jsc_clear_error(env);
}

napi_status NAPI_CDECL napi_strict_equals(napi_env env, napi_value lhs, napi_value rhs, bool* result) {
    CHECK_ENV(env);
    CHECK_ARG(env, lhs);
    CHECK_ARG(env, rhs);
    CHECK_ARG(env, result);
    *result = JSValueIsStrictEqual(env->ctx, ToJS(lhs), ToJS(rhs));
    return napi_jsc_clear_error(env);
}

// ---- script ---------------------------------------------------------------

napi_status NAPI_CDECL napi_run_script(napi_env env, napi_value script, napi_value* result) {
    NAPI_PREAMBLE(env);
    CHECK_ARG(env, script);
    CHECK_ARG(env, result);
    RETURN_STATUS_IF_FALSE(env, JSValueIsString(env->ctx, ToJS(script)), napi_string_expected);
    JSStringRef src = JSValueToStringCopy(env->ctx, ToJS(script), nullptr);
    JSValueRef exc = nullptr;
    JSValueRef r = JSEvaluateScript(env->ctx, src, nullptr, nullptr, 0, &exc);
    JSStringRelease(src);
    if (exc != nullptr)
        return napi_jsc_record_exception(env, exc);
    *result = napi_jsc_add_handle(env, r);
    return napi_jsc_clear_error(env);
}

// ---- handle scopes --------------------------------------------------------

napi_status NAPI_CDECL napi_open_handle_scope(napi_env env, napi_handle_scope* result) {
    CHECK_ENV(env);
    CHECK_ARG(env, result);
    auto* scope = new napi_handle_scope__();
    env->scopes.push_back(scope);
    *result = reinterpret_cast<napi_handle_scope>(scope);
    return napi_jsc_clear_error(env);
}

napi_status NAPI_CDECL napi_close_handle_scope(napi_env env, napi_handle_scope scope) {
    CHECK_ENV(env);
    CHECK_ARG(env, scope);
    RETURN_STATUS_IF_FALSE(env, env->scopes.size() > 1, napi_handle_scope_mismatch);
    auto* s = reinterpret_cast<napi_handle_scope__*>(scope);
    RETURN_STATUS_IF_FALSE(env, env->scopes.back() == s, napi_handle_scope_mismatch);
    for (JSValueRef h : s->handles)
        JSValueUnprotect(env->ctx, h);
    env->scopes.pop_back();
    delete s;
    return napi_jsc_clear_error(env);
}

napi_status NAPI_CDECL napi_open_escapable_handle_scope(napi_env env, napi_escapable_handle_scope* result) {
    CHECK_ENV(env);
    CHECK_ARG(env, result);
    auto* scope = new napi_handle_scope__();
    scope->escapable = true;
    env->scopes.push_back(scope);
    *result = reinterpret_cast<napi_escapable_handle_scope>(scope);
    return napi_jsc_clear_error(env);
}

napi_status NAPI_CDECL napi_close_escapable_handle_scope(napi_env env, napi_escapable_handle_scope scope) {
    return napi_close_handle_scope(env, reinterpret_cast<napi_handle_scope>(scope));
}

napi_status NAPI_CDECL napi_escape_handle(napi_env env, napi_escapable_handle_scope scope, napi_value escapee,
                                          napi_value* result) {
    CHECK_ENV(env);
    CHECK_ARG(env, scope);
    CHECK_ARG(env, escapee);
    CHECK_ARG(env, result);
    auto* s = reinterpret_cast<napi_handle_scope__*>(scope);
    RETURN_STATUS_IF_FALSE(env, s->escapable && !s->escaped, napi_escape_called_twice);
    RETURN_STATUS_IF_FALSE(env, env->scopes.size() >= 2 && env->scopes.back() == s, napi_handle_scope_mismatch);
    s->escaped = true;
    napi_handle_scope__* parent = env->scopes[env->scopes.size() - 2];
    JSValueRef v = ToJS(escapee);
    JSValueProtect(env->ctx, v);
    parent->handles.push_back(v);
    *result = ToNapi(v);
    return napi_jsc_clear_error(env);
}

// ---- misc -----------------------------------------------------------------

napi_status NAPI_CDECL napi_get_version(napi_env env, uint32_t* result) {
    CHECK_ENV(env);
    CHECK_ARG(env, result);
    *result = static_cast<uint32_t>(env->version);
    return napi_jsc_clear_error(env);
}

napi_status NAPI_CDECL napi_get_instance_data(napi_env env, void** data) {
    CHECK_ENV(env);
    CHECK_ARG(env, data);
    *data = env->instance_data;
    return napi_jsc_clear_error(env);
}

napi_status NAPI_CDECL napi_set_instance_data(napi_env env, void* data, napi_finalize finalize_cb,
                                              void* finalize_hint) {
    CHECK_ENV(env);
    env->instance_data = data;
    env->instance_data_finalize = finalize_cb;
    env->instance_data_hint = finalize_hint;
    return napi_jsc_clear_error(env);
}

napi_status NAPI_CDECL napi_adjust_external_memory(napi_env env, int64_t change, int64_t* result) {
    CHECK_ENV(env);
    // JSC's C API exposes no external-memory accounting hook; accept and report
    // the delta as the running total is unavailable. (J2: thread through.)
    if (result != nullptr)
        *result = change;
    return napi_jsc_clear_error(env);
}
