// JSC Node-API backend — objects, functions, classes, wrap/external,
// references, promises, symbols, dates, and type predicates. Core values and
// env bookkeeping live in js_native_api_jsc.cc.

#include "js_native_api_jsc.h"

#include <string>

#include <JavaScriptCore/JSTypedArray.h>

#define NAPI_EXPERIMENTAL
#include "napi/node_api.h"

// ---- references / deferred (opaque types) ---------------------------------

// A napi_ref. Two flavours:
//  * weakable (target is an object): collection is tracked via a shared
//    RefControl + a finalize-bearing holder attached to the target. refcount>0
//    adds a JSValueProtect (strong, keeps the target alive); refcount 0 is weak
//    (no protect) so the target can be collected, after which `control->alive`
//    is false and get_reference_value returns empty.
//  * non-weakable (target is a primitive — string/number/symbol/etc.): JSC's C
//    API can't anchor a holder on a primitive, and primitives have no observable
//    collection, so we keep a plain strong protect for the ref's whole life.
struct napi_ref__ {
    napi_env env;
    uint32_t count;
    bool weakable;
    std::shared_ptr<RefControl> control;  // weakable case (shared with the holder)
    JSValueRef strong_value;              // non-weakable case (always protected)
};

struct napi_deferred__ {
    napi_env env;
    JSObjectRef resolve;
    JSObjectRef reject;
};

namespace {

// Mirror of the CbInfo defined in js_native_api_jsc.cc (same layout). Kept in
// sync by hand; both describe the data a napi_callback_info points at.
struct CbInfo {
    JSContextRef ctx;
    JSObjectRef this_obj;
    size_t argc;
    const JSValueRef* argv;
    JSObjectRef new_target;
    void* data;
};

struct JSStr {
    JSStringRef s;
    explicit JSStr(const char* utf8) : s(JSStringCreateWithUTF8CString(utf8 ? utf8 : "")) {}
    JSStr(const char* utf8, size_t len) {
        std::string z(utf8 ? utf8 : "", utf8 ? len : 0);
        s = JSStringCreateWithUTF8CString(z.c_str());
    }
    ~JSStr() { if (s) JSStringRelease(s); }
    operator JSStringRef() const { return s; }
};

JSObjectRef AsObject(napi_env env, napi_value v) {
    return JSValueToObject(env->ctx, ToJS(v), nullptr);
}

JSObjectRef GlobalCtor(napi_env env, const char* name) {
    JSValueRef v = JSObjectGetProperty(env->ctx, JSContextGetGlobalObject(env->ctx), JSStr(name), nullptr);
    if (v == nullptr || !JSValueIsObject(env->ctx, v))
        return nullptr;
    return JSValueToObject(env->ctx, v, nullptr);
}

bool InstanceOfGlobal(napi_env env, JSValueRef v, const char* ctor_name) {
    if (!JSValueIsObject(env->ctx, v))
        return false;
    JSObjectRef ctor = GlobalCtor(env, ctor_name);
    return ctor != nullptr && JSValueIsInstanceOfConstructor(env->ctx, v, ctor, nullptr);
}

JSObjectRef MakeFunction(napi_env env, napi_callback cb, void* data) {  // AME-JSC-FNPROTO-FIX
    JSObjectRef fn = JSObjectMake(env->ctx, env->function_class, new CallbackData{env, cb, data});
    // AmeCanvas fix: napi callbacks are instances of a custom JSClass whose default
    // [[Prototype]] is Object.prototype, so they lack Function.prototype methods
    // (.call/.apply/.bind) even though they're callable. Reparent to
    // Function.prototype so JS that treats them as ordinary functions works
    // (e.g. the WPT harness does `window.addEventListener.bind(window)`).
    if (JSObjectRef funcCtor = GlobalCtor(env, "Function")) {
        JSValueRef proto = JSObjectGetProperty(env->ctx, funcCtor, JSStr("prototype"), nullptr);
        if (proto != nullptr && JSValueIsObject(env->ctx, proto))
            JSObjectSetPrototype(env->ctx, fn, proto);
    }
    return fn;
}

// Install Ctor[Symbol.hasInstance] implementing OrdinaryHasInstance, so
// `x instanceof Ctor` works under JSC. JSC's `instanceof` operator does NOT fall
// back to OrdinaryHasInstance for a callable custom-JSClass constructor: such a
// ctor is `typeof === "function"` with a correct `.prototype` chain, yet because
// it doesn't inherit Function.prototype[@@hasInstance] (its [[Prototype]] is
// Object.prototype) and has no own @@hasInstance, JSC's instanceof returns false.
// Defining an own @@hasInstance forces the spec's instOfHandler path. (Reparenting
// the ctor to Function.prototype instead is wrong: Function.prototype.name is
// non-writable, which then blanks the class's own name.)  AME-JSC-HASINSTANCE-FIX
void InstallHasInstance(JSContextRef ctx, JSObjectRef ctor) {
    JSValueRef symCtor = JSObjectGetProperty(ctx, JSContextGetGlobalObject(ctx), JSStr("Symbol"), nullptr);
    if (!JSValueIsObject(ctx, symCtor))
        return;
    JSValueRef sym = JSObjectGetProperty(ctx, JSValueToObject(ctx, symCtor, nullptr), JSStr("hasInstance"), nullptr);
    if (sym == nullptr || JSValueGetType(ctx, sym) != kJSTypeSymbol)
        return;
    JSStringRef src = JSStringCreateWithUTF8CString(
        "(function(O){if(O===null||(typeof O!=='object'&&typeof O!=='function'))return false;"
        "var P=this.prototype;if(P===null||typeof P!=='object')return false;"
        "var p=Object.getPrototypeOf(O);while(p!==null){if(p===P)return true;p=Object.getPrototypeOf(p);}"
        "return false;})");
    JSValueRef fn = JSEvaluateScript(ctx, src, nullptr, nullptr, 0, nullptr);
    JSStringRelease(src);
    if (!JSValueIsObject(ctx, fn))
        return;
    JSObjectSetPropertyForKey(ctx, ctor, sym, fn,
                             kJSPropertyAttributeReadOnly | kJSPropertyAttributeDontEnum |
                                     kJSPropertyAttributeDontDelete,
                             nullptr);
}

JSPropertyAttributes ToJSAttrs(napi_property_attributes a) {
    JSPropertyAttributes out = kJSPropertyAttributeNone;
    if (!(a & napi_writable)) out |= kJSPropertyAttributeReadOnly;
    if (!(a & napi_enumerable)) out |= kJSPropertyAttributeDontEnum;
    if (!(a & napi_configurable)) out |= kJSPropertyAttributeDontDelete;
    return out;
}

// Define one property/method/accessor on `target` via Object.defineProperty,
// which (unlike the C setters) handles attributes, getters and setters uniformly.
napi_status DefineOne(napi_env env, JSObjectRef target, const napi_property_descriptor* p) {
    JSContextRef ctx = env->ctx;
    if (env->obj_define_property == nullptr)
        return napi_jsc_set_error(env, napi_generic_failure);

    JSValueRef key = p->utf8name != nullptr ? JSValueMakeString(ctx, JSStr(p->utf8name))
                                            : (p->name != nullptr ? ToJS(p->name) : nullptr);
    if (key == nullptr)
        return napi_jsc_set_error(env, napi_name_expected);

    JSObjectRef desc = JSObjectMake(ctx, nullptr, nullptr);
    auto set = [&](const char* k, JSValueRef v) {
        JSObjectSetProperty(ctx, desc, JSStr(k), v, kJSPropertyAttributeNone, nullptr);
    };
    if (p->getter != nullptr || p->setter != nullptr) {
        if (p->getter != nullptr) set("get", MakeFunction(env, p->getter, p->data));
        if (p->setter != nullptr) set("set", MakeFunction(env, p->setter, p->data));
    } else if (p->method != nullptr) {
        set("value", MakeFunction(env, p->method, p->data));
        set("writable", JSValueMakeBoolean(ctx, (p->attributes & napi_writable) != 0));
    } else {
        set("value", p->value != nullptr ? ToJS(p->value) : JSValueMakeUndefined(ctx));
        set("writable", JSValueMakeBoolean(ctx, (p->attributes & napi_writable) != 0));
    }
    set("enumerable", JSValueMakeBoolean(ctx, (p->attributes & napi_enumerable) != 0));
    set("configurable", JSValueMakeBoolean(ctx, (p->attributes & napi_configurable) != 0));

    JSValueRef args[3] = {target, key, desc};
    JSValueRef exc = nullptr;
    JSObjectCallAsFunction(ctx, env->obj_define_property, nullptr, 3, args, &exc);
    if (exc != nullptr)
        return napi_jsc_record_exception(env, exc);
    return napi_ok;
}

}  // namespace

// ---- objects --------------------------------------------------------------

napi_status NAPI_CDECL napi_create_object(napi_env env, napi_value* result) {
    CHECK_ENV(env);
    CHECK_ARG(env, result);
    *result = napi_jsc_add_handle(env, JSObjectMake(env->ctx, nullptr, nullptr));
    return napi_jsc_clear_error(env);
}

napi_status NAPI_CDECL node_api_create_object_with_properties(napi_env env, napi_value prototype_or_null,
                                                              napi_value* property_names, napi_value* property_values,
                                                              size_t property_count, napi_value* result) {
    NAPI_PREAMBLE(env);
    CHECK_ARG(env, result);
    if (property_count > 0) {
        CHECK_ARG(env, property_names);
        CHECK_ARG(env, property_values);
    }

    JSObjectRef obj = JSObjectMake(env->ctx, nullptr, nullptr);
    // null prototype => null-proto object (like Object.create(null)); otherwise
    // install the given prototype.
    JSObjectSetPrototype(env->ctx, obj,
                         prototype_or_null != nullptr ? ToJS(prototype_or_null) : JSValueMakeNull(env->ctx));

    for (size_t i = 0; i < property_count; i++) {
        JSValueRef key = ToJS(property_names[i]);
        JSType kt = JSValueGetType(env->ctx, key);
        RETURN_STATUS_IF_FALSE(env, kt == kJSTypeString || kt == kJSTypeSymbol, napi_name_expected);
        JSValueRef exc = nullptr;
        JSObjectSetPropertyForKey(env->ctx, obj, key, ToJS(property_values[i]), kJSPropertyAttributeNone, &exc);
        if (exc != nullptr)
            return napi_jsc_record_exception(env, exc);
    }

    *result = napi_jsc_add_handle(env, obj);
    return napi_jsc_clear_error(env);
}

napi_status NAPI_CDECL napi_get_named_property(napi_env env, napi_value object, const char* utf8name,
                                               napi_value* result) {
    NAPI_PREAMBLE(env);
    CHECK_ARG(env, object);
    CHECK_ARG(env, result);
    JSObjectRef obj = AsObject(env, object);
    RETURN_STATUS_IF_FALSE(env, obj != nullptr, napi_object_expected);
    JSValueRef exc = nullptr;
    JSValueRef v = JSObjectGetProperty(env->ctx, obj, JSStr(utf8name), &exc);
    if (exc != nullptr)
        return napi_jsc_record_exception(env, exc);
    *result = napi_jsc_add_handle(env, v);
    return napi_jsc_clear_error(env);
}

napi_status NAPI_CDECL napi_set_named_property(napi_env env, napi_value object, const char* utf8name,
                                               napi_value value) {
    NAPI_PREAMBLE(env);
    CHECK_ARG(env, object);
    CHECK_ARG(env, value);
    JSObjectRef obj = AsObject(env, object);
    RETURN_STATUS_IF_FALSE(env, obj != nullptr, napi_object_expected);
    JSValueRef exc = nullptr;
    JSObjectSetProperty(env->ctx, obj, JSStr(utf8name), ToJS(value), kJSPropertyAttributeNone, &exc);
    if (exc != nullptr)
        return napi_jsc_record_exception(env, exc);
    return napi_jsc_clear_error(env);
}

napi_status NAPI_CDECL napi_has_named_property(napi_env env, napi_value object, const char* utf8name, bool* result) {
    NAPI_PREAMBLE(env);
    CHECK_ARG(env, object);
    CHECK_ARG(env, result);
    JSObjectRef obj = AsObject(env, object);
    RETURN_STATUS_IF_FALSE(env, obj != nullptr, napi_object_expected);
    *result = JSObjectHasProperty(env->ctx, obj, JSStr(utf8name));
    return napi_jsc_clear_error(env);
}

napi_status NAPI_CDECL napi_get_property(napi_env env, napi_value object, napi_value key, napi_value* result) {
    NAPI_PREAMBLE(env);
    CHECK_ARG(env, object);
    CHECK_ARG(env, key);
    CHECK_ARG(env, result);
    JSObjectRef obj = AsObject(env, object);
    RETURN_STATUS_IF_FALSE(env, obj != nullptr, napi_object_expected);
    JSValueRef exc = nullptr;
    JSValueRef v = JSObjectGetPropertyForKey(env->ctx, obj, ToJS(key), &exc);
    if (exc != nullptr)
        return napi_jsc_record_exception(env, exc);
    *result = napi_jsc_add_handle(env, v);
    return napi_jsc_clear_error(env);
}

napi_status NAPI_CDECL napi_set_property(napi_env env, napi_value object, napi_value key, napi_value value) {
    NAPI_PREAMBLE(env);
    CHECK_ARG(env, object);
    CHECK_ARG(env, key);
    CHECK_ARG(env, value);
    JSObjectRef obj = AsObject(env, object);
    RETURN_STATUS_IF_FALSE(env, obj != nullptr, napi_object_expected);
    JSValueRef exc = nullptr;
    JSObjectSetPropertyForKey(env->ctx, obj, ToJS(key), ToJS(value), kJSPropertyAttributeNone, &exc);
    if (exc != nullptr)
        return napi_jsc_record_exception(env, exc);
    return napi_jsc_clear_error(env);
}

napi_status NAPI_CDECL napi_has_property(napi_env env, napi_value object, napi_value key, bool* result) {
    NAPI_PREAMBLE(env);
    CHECK_ARG(env, object);
    CHECK_ARG(env, key);
    CHECK_ARG(env, result);
    JSObjectRef obj = AsObject(env, object);
    RETURN_STATUS_IF_FALSE(env, obj != nullptr, napi_object_expected);
    JSValueRef exc = nullptr;
    *result = JSObjectHasPropertyForKey(env->ctx, obj, ToJS(key), &exc);
    if (exc != nullptr)
        return napi_jsc_record_exception(env, exc);
    return napi_jsc_clear_error(env);
}

napi_status NAPI_CDECL napi_delete_property(napi_env env, napi_value object, napi_value key, bool* result) {
    NAPI_PREAMBLE(env);
    CHECK_ARG(env, object);
    CHECK_ARG(env, key);
    JSObjectRef obj = AsObject(env, object);
    RETURN_STATUS_IF_FALSE(env, obj != nullptr, napi_object_expected);
    JSValueRef exc = nullptr;
    bool ok = JSObjectDeletePropertyForKey(env->ctx, obj, ToJS(key), &exc);
    if (exc != nullptr)
        return napi_jsc_record_exception(env, exc);
    if (result != nullptr)
        *result = ok;
    return napi_jsc_clear_error(env);
}

napi_status NAPI_CDECL napi_has_own_property(napi_env env, napi_value object, napi_value key, bool* result) {
    NAPI_PREAMBLE(env);
    CHECK_ARG(env, object);
    CHECK_ARG(env, key);
    CHECK_ARG(env, result);
    JSObjectRef obj = AsObject(env, object);
    RETURN_STATUS_IF_FALSE(env, obj != nullptr, napi_object_expected);
    RETURN_STATUS_IF_FALSE(env, env->obj_has_own != nullptr, napi_generic_failure);
    JSValueRef arg = ToJS(key);
    JSValueRef exc = nullptr;
    JSValueRef r = JSObjectCallAsFunction(env->ctx, env->obj_has_own, obj, 1, &arg, &exc);
    if (exc != nullptr)
        return napi_jsc_record_exception(env, exc);
    *result = r != nullptr && JSValueToBoolean(env->ctx, r);
    return napi_jsc_clear_error(env);
}

napi_status NAPI_CDECL napi_get_element(napi_env env, napi_value object, uint32_t index, napi_value* result) {
    NAPI_PREAMBLE(env);
    CHECK_ARG(env, object);
    CHECK_ARG(env, result);
    JSObjectRef obj = AsObject(env, object);
    RETURN_STATUS_IF_FALSE(env, obj != nullptr, napi_object_expected);
    JSValueRef exc = nullptr;
    JSValueRef v = JSObjectGetPropertyAtIndex(env->ctx, obj, index, &exc);
    if (exc != nullptr)
        return napi_jsc_record_exception(env, exc);
    *result = napi_jsc_add_handle(env, v);
    return napi_jsc_clear_error(env);
}

napi_status NAPI_CDECL napi_set_element(napi_env env, napi_value object, uint32_t index, napi_value value) {
    NAPI_PREAMBLE(env);
    CHECK_ARG(env, object);
    CHECK_ARG(env, value);
    JSObjectRef obj = AsObject(env, object);
    RETURN_STATUS_IF_FALSE(env, obj != nullptr, napi_object_expected);
    JSValueRef exc = nullptr;
    JSObjectSetPropertyAtIndex(env->ctx, obj, index, ToJS(value), &exc);
    if (exc != nullptr)
        return napi_jsc_record_exception(env, exc);
    return napi_jsc_clear_error(env);
}

napi_status NAPI_CDECL napi_has_element(napi_env env, napi_value object, uint32_t index, bool* result) {
    NAPI_PREAMBLE(env);
    CHECK_ARG(env, object);
    CHECK_ARG(env, result);
    JSObjectRef obj = AsObject(env, object);
    RETURN_STATUS_IF_FALSE(env, obj != nullptr, napi_object_expected);
    JSValueRef key = JSValueMakeNumber(env->ctx, index);
    JSValueRef exc = nullptr;
    *result = JSObjectHasPropertyForKey(env->ctx, obj, key, &exc);
    if (exc != nullptr)
        return napi_jsc_record_exception(env, exc);
    return napi_jsc_clear_error(env);
}

napi_status NAPI_CDECL napi_delete_element(napi_env env, napi_value object, uint32_t index, bool* result) {
    NAPI_PREAMBLE(env);
    CHECK_ARG(env, object);
    JSObjectRef obj = AsObject(env, object);
    RETURN_STATUS_IF_FALSE(env, obj != nullptr, napi_object_expected);
    JSValueRef key = JSValueMakeNumber(env->ctx, index);
    JSValueRef exc = nullptr;
    bool ok = JSObjectDeletePropertyForKey(env->ctx, obj, key, &exc);
    if (exc != nullptr)
        return napi_jsc_record_exception(env, exc);
    if (result != nullptr)
        *result = ok;
    return napi_jsc_clear_error(env);
}

napi_status NAPI_CDECL napi_get_property_names(napi_env env, napi_value object, napi_value* result) {
    NAPI_PREAMBLE(env);
    CHECK_ARG(env, object);
    CHECK_ARG(env, result);
    JSObjectRef obj = AsObject(env, object);
    RETURN_STATUS_IF_FALSE(env, obj != nullptr, napi_object_expected);
    JSPropertyNameArrayRef names = JSObjectCopyPropertyNames(env->ctx, obj);
    size_t count = JSPropertyNameArrayGetCount(names);
    std::vector<JSValueRef> elems(count);
    for (size_t i = 0; i < count; ++i) {
        JSStringRef name = JSPropertyNameArrayGetNameAtIndex(names, i);
        elems[i] = JSValueMakeString(env->ctx, name);
    }
    JSObjectRef arr = JSObjectMakeArray(env->ctx, count, count ? elems.data() : nullptr, nullptr);
    JSPropertyNameArrayRelease(names);
    *result = napi_jsc_add_handle(env, arr);
    return napi_jsc_clear_error(env);
}

napi_status NAPI_CDECL napi_get_all_property_names(napi_env env, napi_value object, napi_key_collection_mode mode,
                                                   napi_key_filter filter, napi_key_conversion conversion,
                                                   napi_value* result) {
    NAPI_PREAMBLE(env);
    CHECK_ARG(env, object);
    CHECK_ARG(env, result);
    JSObjectRef obj = AsObject(env, object);
    RETURN_STATUS_IF_FALSE(env, obj != nullptr, napi_object_expected);
    // The C API has no descriptor-aware key enumeration, so honor mode/filter/
    // conversion through a JS helper using Reflect.ownKeys + descriptors.
    static const char* kHelper =
        "(function(o,own,wr,en,cf,ss,sy,n2s){"
        "var out=[],seen=new Set(),c=o;"
        "do{var ks=Reflect.ownKeys(c);"
        "for(var i=0;i<ks.length;i++){var k=ks[i];var sym=(typeof k==='symbol');"
        "if(sym){if(sy)continue;}else{if(ss)continue;}"
        "var d=Object.getOwnPropertyDescriptor(c,k);if(!d)continue;"
        "if(wr&&!('writable' in d?d.writable:true))continue;"
        "if(en&&!d.enumerable)continue;"
        "if(cf&&!d.configurable)continue;"
        "var id=sym?k:String(k);if(seen.has(id))continue;seen.add(id);"
        "out.push((n2s&&!sym)?String(k):k);}"
        "if(own)break;c=Object.getPrototypeOf(c);}while(c);"
        "return out;})";
    JSValueRef exc = nullptr;
    JSStringRef src = JSStringCreateWithUTF8CString(kHelper);
    JSValueRef fnv = JSEvaluateScript(env->ctx, src, nullptr, nullptr, 0, &exc);
    JSStringRelease(src);
    if (exc != nullptr)
        return napi_jsc_record_exception(env, exc);
    JSObjectRef fn = JSValueToObject(env->ctx, fnv, nullptr);
    auto b = [&](bool v) { return JSValueMakeBoolean(env->ctx, v); };
    JSValueRef args[8] = {
        ToJS(object),
        b(mode == napi_key_own_only),
        b((filter & napi_key_writable) != 0),
        b((filter & napi_key_enumerable) != 0),
        b((filter & napi_key_configurable) != 0),
        b((filter & napi_key_skip_strings) != 0),
        b((filter & napi_key_skip_symbols) != 0),
        b(conversion == napi_key_numbers_to_strings),
    };
    JSValueRef arr = JSObjectCallAsFunction(env->ctx, fn, nullptr, 8, args, &exc);
    if (exc != nullptr)
        return napi_jsc_record_exception(env, exc);
    *result = napi_jsc_add_handle(env, arr);
    return napi_jsc_clear_error(env);
}

napi_status NAPI_CDECL napi_get_prototype(napi_env env, napi_value object, napi_value* result) {
    NAPI_PREAMBLE(env);
    CHECK_ARG(env, object);
    CHECK_ARG(env, result);
    JSObjectRef obj = AsObject(env, object);
    RETURN_STATUS_IF_FALSE(env, obj != nullptr, napi_object_expected);
    *result = napi_jsc_add_handle(env, JSObjectGetPrototype(env->ctx, obj));
    return napi_jsc_clear_error(env);
}

napi_status NAPI_CDECL node_api_set_prototype(napi_env env, napi_value object, napi_value value) {
    NAPI_PREAMBLE(env);
    CHECK_ARG(env, object);
    CHECK_ARG(env, value);
    JSObjectRef obj = AsObject(env, object);
    RETURN_STATUS_IF_FALSE(env, obj != nullptr, napi_object_expected);
    // JSObjectSetPrototype has no error channel; it silently no-ops on a
    // non-extensible object, which matches the "best effort" of the C API.
    JSObjectSetPrototype(env->ctx, obj, ToJS(value));
    return napi_jsc_clear_error(env);
}

namespace {
napi_status FreezeOrSeal(napi_env env, napi_value object, JSObjectRef fn) {
    NAPI_PREAMBLE(env);
    CHECK_ARG(env, object);
    RETURN_STATUS_IF_FALSE(env, fn != nullptr, napi_generic_failure);
    JSValueRef arg = ToJS(object);
    JSValueRef exc = nullptr;
    JSObjectCallAsFunction(env->ctx, fn, nullptr, 1, &arg, &exc);
    if (exc != nullptr)
        return napi_jsc_record_exception(env, exc);
    return napi_jsc_clear_error(env);
}
}  // namespace

napi_status NAPI_CDECL napi_object_freeze(napi_env env, napi_value object) {
    return FreezeOrSeal(env, object, env ? env->obj_freeze : nullptr);
}
napi_status NAPI_CDECL napi_object_seal(napi_env env, napi_value object) {
    return FreezeOrSeal(env, object, env ? env->obj_seal : nullptr);
}

// ---- arrays ---------------------------------------------------------------

napi_status NAPI_CDECL napi_create_array(napi_env env, napi_value* result) {
    CHECK_ENV(env);
    CHECK_ARG(env, result);
    *result = napi_jsc_add_handle(env, JSObjectMakeArray(env->ctx, 0, nullptr, nullptr));
    return napi_jsc_clear_error(env);
}

napi_status NAPI_CDECL napi_create_array_with_length(napi_env env, size_t length, napi_value* result) {
    CHECK_ENV(env);
    CHECK_ARG(env, result);
    JSObjectRef arr = JSObjectMakeArray(env->ctx, 0, nullptr, nullptr);
    JSObjectSetProperty(env->ctx, arr, JSStr("length"),
                        JSValueMakeNumber(env->ctx, static_cast<double>(length)), kJSPropertyAttributeNone, nullptr);
    *result = napi_jsc_add_handle(env, arr);
    return napi_jsc_clear_error(env);
}

napi_status NAPI_CDECL napi_is_array(napi_env env, napi_value value, bool* result) {
    CHECK_ENV(env);
    CHECK_ARG(env, value);
    CHECK_ARG(env, result);
    *result = JSValueIsArray(env->ctx, ToJS(value));
    return napi_jsc_clear_error(env);
}

napi_status NAPI_CDECL napi_get_array_length(napi_env env, napi_value value, uint32_t* result) {
    NAPI_PREAMBLE(env);
    CHECK_ARG(env, value);
    CHECK_ARG(env, result);
    JSObjectRef obj = AsObject(env, value);
    RETURN_STATUS_IF_FALSE(env, obj != nullptr, napi_array_expected);
    JSValueRef len = JSObjectGetProperty(env->ctx, obj, JSStr("length"), nullptr);
    *result = static_cast<uint32_t>(JSValueToNumber(env->ctx, len, nullptr));
    return napi_jsc_clear_error(env);
}

// ---- functions ------------------------------------------------------------

napi_status NAPI_CDECL napi_create_function(napi_env env, const char* utf8name, size_t length, napi_callback cb,
                                            void* data, napi_value* result) {
    CHECK_ENV(env);
    CHECK_ARG(env, cb);
    CHECK_ARG(env, result);
    JSObjectRef fn = MakeFunction(env, cb, data);
    if (utf8name != nullptr && length > 0) {
        std::string name = (length == NAPI_AUTO_LENGTH) ? std::string(utf8name) : std::string(utf8name, length);
        JSObjectSetProperty(env->ctx, fn, JSStr("name"), JSValueMakeString(env->ctx, JSStr(name.c_str())),
                            kJSPropertyAttributeReadOnly | kJSPropertyAttributeDontEnum, nullptr);
    }
    *result = napi_jsc_add_handle(env, fn);
    return napi_jsc_clear_error(env);
}

napi_status NAPI_CDECL napi_call_function(napi_env env, napi_value recv, napi_value func, size_t argc,
                                          const napi_value* argv, napi_value* result) {
    NAPI_PREAMBLE(env);
    CHECK_ARG(env, func);
    JSObjectRef fn = AsObject(env, func);
    RETURN_STATUS_IF_FALSE(env, fn != nullptr && JSObjectIsFunction(env->ctx, fn), napi_function_expected);
    JSObjectRef thiz = recv != nullptr && JSValueIsObject(env->ctx, ToJS(recv)) ? AsObject(env, recv) : nullptr;
    std::vector<JSValueRef> args(argc);
    for (size_t i = 0; i < argc; ++i)
        args[i] = ToJS(argv[i]);
    JSValueRef exc = nullptr;
    JSValueRef r = JSObjectCallAsFunction(env->ctx, fn, thiz, argc, argc ? args.data() : nullptr, &exc);
    if (exc != nullptr)
        return napi_jsc_record_exception(env, exc);
    if (result != nullptr)
        *result = napi_jsc_add_handle(env, r);
    return napi_jsc_clear_error(env);
}

napi_status NAPI_CDECL napi_new_instance(napi_env env, napi_value constructor, size_t argc, const napi_value* argv,
                                         napi_value* result) {
    NAPI_PREAMBLE(env);
    CHECK_ARG(env, constructor);
    CHECK_ARG(env, result);
    JSObjectRef ctor = AsObject(env, constructor);
    RETURN_STATUS_IF_FALSE(env, ctor != nullptr && JSObjectIsConstructor(env->ctx, ctor), napi_function_expected);
    std::vector<JSValueRef> args(argc);
    for (size_t i = 0; i < argc; ++i)
        args[i] = ToJS(argv[i]);
    JSValueRef exc = nullptr;
    JSObjectRef r = JSObjectCallAsConstructor(env->ctx, ctor, argc, argc ? args.data() : nullptr, &exc);
    if (exc != nullptr)
        return napi_jsc_record_exception(env, exc);
    *result = napi_jsc_add_handle(env, r);
    return napi_jsc_clear_error(env);
}

napi_status NAPI_CDECL napi_get_cb_info(napi_env env, napi_callback_info cbinfo, size_t* argc, napi_value* argv,
                                        napi_value* this_arg, void** data) {
    CHECK_ENV(env);
    CHECK_ARG(env, cbinfo);
    auto* info = reinterpret_cast<CbInfo*>(cbinfo);
    if (argv != nullptr && argc != nullptr) {
        size_t want = *argc;
        for (size_t i = 0; i < want; ++i)
            argv[i] = napi_jsc_add_handle(env, i < info->argc ? info->argv[i] : JSValueMakeUndefined(env->ctx));
    }
    if (argc != nullptr)
        *argc = info->argc;
    if (this_arg != nullptr)
        *this_arg = napi_jsc_add_handle(env, info->this_obj != nullptr ? static_cast<JSValueRef>(info->this_obj)
                                                                       : JSValueMakeUndefined(env->ctx));
    if (data != nullptr)
        *data = info->data;
    return napi_jsc_clear_error(env);
}

napi_status NAPI_CDECL napi_get_new_target(napi_env env, napi_callback_info cbinfo, napi_value* result) {
    CHECK_ENV(env);
    CHECK_ARG(env, cbinfo);
    CHECK_ARG(env, result);
    auto* info = reinterpret_cast<CbInfo*>(cbinfo);
    *result = info->new_target != nullptr ? napi_jsc_add_handle(env, info->new_target) : nullptr;
    return napi_jsc_clear_error(env);
}

napi_status NAPI_CDECL napi_instanceof(napi_env env, napi_value object, napi_value constructor, bool* result) {
    NAPI_PREAMBLE(env);
    CHECK_ARG(env, object);
    CHECK_ARG(env, constructor);
    CHECK_ARG(env, result);
    JSObjectRef ctor = AsObject(env, constructor);
    RETURN_STATUS_IF_FALSE(env, ctor != nullptr, napi_function_expected);
    JSValueRef exc = nullptr;
    *result = JSValueIsInstanceOfConstructor(env->ctx, ToJS(object), ctor, &exc);
    if (exc != nullptr)
        return napi_jsc_record_exception(env, exc);
    return napi_jsc_clear_error(env);
}

napi_status NAPI_CDECL napi_define_class(napi_env env, const char* utf8name, size_t length, napi_callback ctor_cb,
                                         void* data, size_t prop_count, const napi_property_descriptor* props,
                                         napi_value* result) {
    NAPI_PREAMBLE(env);
    CHECK_ARG(env, ctor_cb);
    CHECK_ARG(env, result);
    JSContextRef ctx = env->ctx;

    JSObjectRef ctor = JSObjectMake(ctx, env->constructor_class, new CallbackData{env, ctor_cb, data});
    JSObjectRef proto = JSObjectMake(ctx, nullptr, nullptr);
    JSObjectSetProperty(ctx, ctor, JSStr("prototype"), proto,
                        kJSPropertyAttributeReadOnly | kJSPropertyAttributeDontEnum | kJSPropertyAttributeDontDelete,
                        nullptr);
    // AmeCanvas fix: define `constructor` non-enumerable (writable+configurable),
    // per the ES spec / matching V8. JSObjectSetProperty's kJSPropertyAttributeDontEnum
    // is NOT honored here under JSC — the property comes out enumerable, leaks into
    // `for..in`, and trips conformance that enumerates an interface's own functions
    // (e.g. WebGL offscreencanvas/methods). Use the Object.defineProperty path
    // (same as DefineOne for methods), which applies attributes reliably.
    // AME-JSC-CTORENUM-FIX
    {
        napi_property_descriptor cd = {};
        cd.utf8name = "constructor";
        cd.value = napi_jsc_add_handle(env, ctor);
        cd.attributes = static_cast<napi_property_attributes>(napi_writable | napi_configurable);
        DefineOne(env, proto, &cd);
    }
    if (utf8name != nullptr) {
        std::string name = (length == NAPI_AUTO_LENGTH) ? std::string(utf8name) : std::string(utf8name, length);
        JSObjectSetProperty(ctx, ctor, JSStr("name"), JSValueMakeString(ctx, JSStr(name.c_str())),
                            kJSPropertyAttributeReadOnly | kJSPropertyAttributeDontEnum, nullptr);
    }
    InstallHasInstance(ctx, ctor);  // make `x instanceof Ctor` work under JSC (AME-JSC-HASINSTANCE-FIX)

    for (size_t i = 0; i < prop_count; ++i) {
        const napi_property_descriptor* p = &props[i];
        JSObjectRef target = (p->attributes & napi_static) ? ctor : proto;
        napi_status s = DefineOne(env, target, p);
        if (s != napi_ok)
            return s;
    }

    *result = napi_jsc_add_handle(env, ctor);
    return napi_jsc_clear_error(env);
}

napi_status NAPI_CDECL napi_define_properties(napi_env env, napi_value object, size_t property_count,
                                              const napi_property_descriptor* properties) {
    NAPI_PREAMBLE(env);
    CHECK_ARG(env, object);
    JSObjectRef obj = AsObject(env, object);
    RETURN_STATUS_IF_FALSE(env, obj != nullptr, napi_object_expected);
    for (size_t i = 0; i < property_count; ++i) {
        napi_status s = DefineOne(env, obj, &properties[i]);
        if (s != napi_ok)
            return s;
    }
    return napi_jsc_clear_error(env);
}

// ---- external / wrap ------------------------------------------------------

namespace {
// Attach a finalize-bearing external_class holder to `anchor` under `key`,
// carrying native `data`/`finalize_cb`/`hint` and an optional weak-ref
// `control`. The holder is GC-collected together with `anchor` (its only strong
// referrer), at which point ExternalFinalize fires. Returns false and yields the
// exception if the property can't be set (e.g. a frozen/non-extensible target).
bool AttachHolder(napi_env env, JSObjectRef anchor, JSValueRef key, void* data, napi_finalize finalize_cb,
                  void* hint, std::shared_ptr<RefControl> control, JSValueRef* exc_out) {
    auto* st = new ExternalState{env, data, finalize_cb, hint, std::move(control)};
    JSObjectRef holder = JSObjectMake(env->ctx, env->external_class, st);
    JSValueRef exc = nullptr;
    JSObjectSetPropertyForKey(env->ctx, anchor, key, holder,
                              kJSPropertyAttributeDontEnum | kJSPropertyAttributeDontDelete, &exc);
    if (exc != nullptr) {
        if (exc_out != nullptr)
            *exc_out = exc;
        // The holder is now unreferenced; it will be GC'd and its st freed.
        return false;
    }
    return true;
}

// Build a napi_ref for `v` with `count`. Object targets become weakable (a
// holder under a fresh hidden symbol tracks their collection); primitives — and
// objects we can't anchor a holder on — fall back to a plain strong protect.
napi_ref MakeReference(napi_env env, JSValueRef v, uint32_t count) {
    if (JSValueIsObject(env->ctx, v)) {
        JSObjectRef obj = JSValueToObject(env->ctx, v, nullptr);
        auto control = std::make_shared<RefControl>(RefControl{env, v, true});
        JSValueRef key = JSValueMakeSymbol(env->ctx, JSStr("napi.ref"));
        if (AttachHolder(env, obj, key, nullptr, nullptr, nullptr, control, nullptr)) {
            if (count > 0)
                JSValueProtect(env->ctx, v);  // strong while refcount > 0
            return new napi_ref__{env, count, true, control, nullptr};
        }
    }
    JSValueProtect(env->ctx, v);
    return new napi_ref__{env, count, false, nullptr, v};
}
}  // namespace

napi_status NAPI_CDECL napi_create_external(napi_env env, void* data, napi_finalize finalize_cb, void* finalize_hint,
                                            napi_value* result) {
    CHECK_ENV(env);
    CHECK_ARG(env, result);
    auto* st = new ExternalState{env, data, finalize_cb, finalize_hint};
    JSObjectRef holder = JSObjectMake(env->ctx, env->external_class, st);
    *result = napi_jsc_add_handle(env, holder);
    return napi_jsc_clear_error(env);
}

napi_status NAPI_CDECL napi_get_value_external(napi_env env, napi_value value, void** result) {
    CHECK_ENV(env);
    CHECK_ARG(env, value);
    CHECK_ARG(env, result);
    RETURN_STATUS_IF_FALSE(env, JSValueIsObjectOfClass(env->ctx, ToJS(value), env->external_class),
                           napi_invalid_arg);
    auto* st = static_cast<ExternalState*>(JSObjectGetPrivate(AsObject(env, value)));
    *result = st != nullptr ? st->data : nullptr;
    return napi_jsc_clear_error(env);
}

napi_status NAPI_CDECL napi_wrap(napi_env env, napi_value js_object, void* native_object, napi_finalize finalize_cb,
                                 void* finalize_hint, napi_ref* result) {
    NAPI_PREAMBLE(env);
    CHECK_ARG(env, js_object);
    CHECK_ARG(env, native_object);
    JSObjectRef obj = AsObject(env, js_object);
    RETURN_STATUS_IF_FALSE(env, obj != nullptr, napi_object_expected);
    // If the caller wants a reference back, the returned (weak) ref shares the
    // wrap holder's control block — one holder serves both the user finalizer
    // and the ref's collection tracking (mirrors Node's single Reference).
    std::shared_ptr<RefControl> control;
    if (result != nullptr)
        control = std::make_shared<RefControl>(RefControl{env, ToJS(js_object), true});
    JSValueRef exc = nullptr;
    if (!AttachHolder(env, obj, env->wrap_key, native_object, finalize_cb, finalize_hint, control, &exc))
        return napi_jsc_record_exception(env, exc);
    if (result != nullptr)
        *result = new napi_ref__{env, 0, true, control, nullptr};  // weak (refcount 0)
    return napi_jsc_clear_error(env);
}

namespace {
ExternalState* WrapHolder(napi_env env, napi_value js_object) {
    JSObjectRef obj = AsObject(env, js_object);
    if (obj == nullptr)
        return nullptr;
    JSValueRef hv = JSObjectGetPropertyForKey(env->ctx, obj, env->wrap_key, nullptr);
    if (hv == nullptr || !JSValueIsObjectOfClass(env->ctx, hv, env->external_class))
        return nullptr;
    return static_cast<ExternalState*>(JSObjectGetPrivate(JSValueToObject(env->ctx, hv, nullptr)));
}
}  // namespace

napi_status NAPI_CDECL napi_unwrap(napi_env env, napi_value js_object, void** result) {
    NAPI_PREAMBLE(env);
    CHECK_ARG(env, js_object);
    CHECK_ARG(env, result);
    ExternalState* st = WrapHolder(env, js_object);
    RETURN_STATUS_IF_FALSE(env, st != nullptr, napi_invalid_arg);
    *result = st->data;
    return napi_jsc_clear_error(env);
}

napi_status NAPI_CDECL napi_remove_wrap(napi_env env, napi_value js_object, void** result) {
    NAPI_PREAMBLE(env);
    CHECK_ARG(env, js_object);
    ExternalState* st = WrapHolder(env, js_object);
    RETURN_STATUS_IF_FALSE(env, st != nullptr, napi_invalid_arg);
    if (result != nullptr)
        *result = st->data;
    st->finalize_cb = nullptr;  // detach: the user reclaims ownership, no finalizer
    st->data = nullptr;
    // Keep the holder attached when it also backs a weak ref, so that ref keeps
    // tracking the (still-live) object; otherwise drop the hidden wrap property.
    if (!st->ref_control) {
        JSObjectRef obj = AsObject(env, js_object);
        JSObjectDeletePropertyForKey(env->ctx, obj, env->wrap_key, nullptr);
    }
    return napi_jsc_clear_error(env);
}

napi_status NAPI_CDECL napi_add_finalizer(napi_env env, napi_value js_object, void* finalize_data,
                                          napi_finalize finalize_cb, void* finalize_hint, napi_ref* result) {
    NAPI_PREAMBLE(env);
    CHECK_ARG(env, js_object);
    CHECK_ARG(env, finalize_cb);
    JSObjectRef obj = AsObject(env, js_object);
    RETURN_STATUS_IF_FALSE(env, obj != nullptr, napi_object_expected);
    std::shared_ptr<RefControl> control;
    if (result != nullptr)
        control = std::make_shared<RefControl>(RefControl{env, ToJS(js_object), true});
    // A fresh hidden symbol per finalizer so multiple can coexist on one object.
    JSValueRef key = JSValueMakeSymbol(env->ctx, JSStr("napi.finalizer"));
    JSValueRef exc = nullptr;
    if (!AttachHolder(env, obj, key, finalize_data, finalize_cb, finalize_hint, control, &exc))
        return napi_jsc_record_exception(env, exc);
    if (result != nullptr)
        *result = new napi_ref__{env, 0, true, control, nullptr};  // weak (refcount 0)
    return napi_jsc_clear_error(env);
}

// ---- references -----------------------------------------------------------

napi_status NAPI_CDECL napi_create_reference(napi_env env, napi_value value, uint32_t initial_refcount,
                                             napi_ref* result) {
    CHECK_ENV(env);
    CHECK_ARG(env, value);
    CHECK_ARG(env, result);
    *result = MakeReference(env, ToJS(value), initial_refcount);
    return napi_jsc_clear_error(env);
}

napi_status NAPI_CDECL napi_delete_reference(napi_env env, napi_ref ref) {
    CHECK_ENV(env);
    CHECK_ARG(env, ref);
    if (ref->weakable) {
        if (ref->count > 0 && ref->control && ref->control->alive)
            JSValueUnprotect(env->ctx, ref->control->value);  // release our strong hold
    } else {
        JSValueUnprotect(env->ctx, ref->strong_value);
    }
    delete ref;
    return napi_jsc_clear_error(env);
}

napi_status NAPI_CDECL napi_reference_ref(napi_env env, napi_ref ref, uint32_t* result) {
    CHECK_ENV(env);
    CHECK_ARG(env, ref);
    ref->count += 1;
    // 0 -> 1 re-strengthens a weak ref (no-op if the target is already gone).
    if (ref->weakable && ref->count == 1 && ref->control && ref->control->alive)
        JSValueProtect(env->ctx, ref->control->value);
    if (result != nullptr)
        *result = ref->count;
    return napi_jsc_clear_error(env);
}

napi_status NAPI_CDECL napi_reference_unref(napi_env env, napi_ref ref, uint32_t* result) {
    CHECK_ENV(env);
    CHECK_ARG(env, ref);
    RETURN_STATUS_IF_FALSE(env, ref->count > 0, napi_generic_failure);
    ref->count -= 1;
    // 1 -> 0 weakens: drop the protect so the target becomes collectible.
    if (ref->weakable && ref->count == 0 && ref->control && ref->control->alive)
        JSValueUnprotect(env->ctx, ref->control->value);
    if (result != nullptr)
        *result = ref->count;
    return napi_jsc_clear_error(env);
}

napi_status NAPI_CDECL napi_get_reference_value(napi_env env, napi_ref ref, napi_value* result) {
    CHECK_ENV(env);
    CHECK_ARG(env, ref);
    CHECK_ARG(env, result);
    if (ref->weakable) {
        // After the target is collected the ref is empty: napi_value NULL.
        *result = (ref->control && ref->control->alive) ? napi_jsc_add_handle(env, ref->control->value) : nullptr;
    } else {
        *result = napi_jsc_add_handle(env, ref->strong_value);
    }
    return napi_jsc_clear_error(env);
}

// ---- symbols --------------------------------------------------------------

napi_status NAPI_CDECL napi_create_symbol(napi_env env, napi_value description, napi_value* result) {
    CHECK_ENV(env);
    CHECK_ARG(env, result);
    JSStringRef desc = nullptr;
    if (description != nullptr && JSValueIsString(env->ctx, ToJS(description)))
        desc = JSValueToStringCopy(env->ctx, ToJS(description), nullptr);
    JSValueRef sym = JSValueMakeSymbol(env->ctx, desc);
    if (desc != nullptr)
        JSStringRelease(desc);
    *result = napi_jsc_add_handle(env, sym);
    return napi_jsc_clear_error(env);
}

napi_status NAPI_CDECL node_api_symbol_for(napi_env env, const char* utf8description, size_t length,
                                           napi_value* result) {
    CHECK_ENV(env);
    CHECK_ARG(env, result);
    RETURN_STATUS_IF_FALSE(env, env->symbol_for != nullptr, napi_generic_failure);
    std::string d = (length == NAPI_AUTO_LENGTH) ? std::string(utf8description ? utf8description : "")
                                                 : std::string(utf8description, length);
    JSValueRef arg = JSValueMakeString(env->ctx, JSStr(d.c_str()));
    JSValueRef exc = nullptr;
    JSValueRef sym = JSObjectCallAsFunction(env->ctx, env->symbol_for, nullptr, 1, &arg, &exc);
    if (exc != nullptr)
        return napi_jsc_record_exception(env, exc);
    *result = napi_jsc_add_handle(env, sym);
    return napi_jsc_clear_error(env);
}

// ---- property keys (interned strings; a plain string suffices for JSC) -----

napi_status NAPI_CDECL node_api_create_property_key_utf8(napi_env env, const char* str, size_t length,
                                                         napi_value* result) {
    return napi_create_string_utf8(env, str, length, result);
}
napi_status NAPI_CDECL node_api_create_property_key_utf16(napi_env env, const char16_t* str, size_t length,
                                                          napi_value* result) {
    return napi_create_string_utf16(env, str, length, result);
}
napi_status NAPI_CDECL node_api_create_property_key_latin1(napi_env env, const char* str, size_t length,
                                                           napi_value* result) {
    return napi_create_string_latin1(env, str, length, result);
}

// ---- external strings -----------------------------------------------------
//
// JSC's C API always copies string bytes into its own heap, so we cannot keep
// the caller's buffer live as an external. We copy, report copied=true, and
// (since the caller may rely on the engine to release the buffer) defer the
// finalizer to the next tick so it runs once the copy is safely owned by JSC.

namespace {
napi_status ExternalString(napi_env env, napi_status make_status, void* buf, node_api_basic_finalize finalize_cb,
                           void* finalize_hint, bool* copied) {
    if (make_status != napi_ok)
        return make_status;
    if (copied != nullptr)
        *copied = true;
    if (finalize_cb != nullptr) {
        std::lock_guard<std::mutex> lk(env->finalizer_mu);
        env->pending_finalizers.push_back({finalize_cb, buf, finalize_hint});
    }
    return napi_jsc_clear_error(env);
}
}  // namespace

napi_status NAPI_CDECL node_api_create_external_string_latin1(napi_env env, char* str, size_t length,
                                                             node_api_basic_finalize finalize_callback,
                                                             void* finalize_hint, napi_value* result, bool* copied) {
    CHECK_ENV(env);
    return ExternalString(env, napi_create_string_latin1(env, str, length, result), str, finalize_callback,
                          finalize_hint, copied);
}

napi_status NAPI_CDECL node_api_create_external_string_utf16(napi_env env, char16_t* str, size_t length,
                                                            node_api_basic_finalize finalize_callback,
                                                            void* finalize_hint, napi_value* result, bool* copied) {
    CHECK_ENV(env);
    return ExternalString(env, napi_create_string_utf16(env, str, length, result), str, finalize_callback,
                          finalize_hint, copied);
}

// ---- dates ----------------------------------------------------------------

napi_status NAPI_CDECL napi_create_date(napi_env env, double time, napi_value* result) {
    CHECK_ENV(env);
    CHECK_ARG(env, result);
    JSValueRef arg = JSValueMakeNumber(env->ctx, time);
    JSValueRef exc = nullptr;
    JSObjectRef d = JSObjectMakeDate(env->ctx, 1, &arg, &exc);
    if (exc != nullptr)
        return napi_jsc_record_exception(env, exc);
    *result = napi_jsc_add_handle(env, d);
    return napi_jsc_clear_error(env);
}

napi_status NAPI_CDECL napi_is_date(napi_env env, napi_value value, bool* result) {
    CHECK_ENV(env);
    CHECK_ARG(env, value);
    CHECK_ARG(env, result);
    *result = JSValueIsDate(env->ctx, ToJS(value));
    return napi_jsc_clear_error(env);
}

napi_status NAPI_CDECL napi_get_date_value(napi_env env, napi_value value, double* result) {
    NAPI_PREAMBLE(env);
    CHECK_ARG(env, value);
    CHECK_ARG(env, result);
    RETURN_STATUS_IF_FALSE(env, JSValueIsDate(env->ctx, ToJS(value)), napi_date_expected);
    JSObjectRef d = AsObject(env, value);
    JSValueRef vof = JSObjectGetProperty(env->ctx, d, JSStr("valueOf"), nullptr);
    JSObjectRef vof_fn = vof && JSValueIsObject(env->ctx, vof) ? JSValueToObject(env->ctx, vof, nullptr) : nullptr;
    RETURN_STATUS_IF_FALSE(env, vof_fn != nullptr, napi_generic_failure);
    JSValueRef exc = nullptr;
    JSValueRef ms = JSObjectCallAsFunction(env->ctx, vof_fn, d, 0, nullptr, &exc);
    if (exc != nullptr)
        return napi_jsc_record_exception(env, exc);
    *result = JSValueToNumber(env->ctx, ms, nullptr);
    return napi_jsc_clear_error(env);
}

// ---- promises -------------------------------------------------------------

napi_status NAPI_CDECL napi_create_promise(napi_env env, napi_deferred* deferred, napi_value* promise) {
    NAPI_PREAMBLE(env);
    CHECK_ARG(env, deferred);
    CHECK_ARG(env, promise);
    JSObjectRef resolve = nullptr, reject = nullptr;
    JSValueRef exc = nullptr;
    JSObjectRef p = JSObjectMakeDeferredPromise(env->ctx, &resolve, &reject, &exc);
    if (exc != nullptr)
        return napi_jsc_record_exception(env, exc);
    JSValueProtect(env->ctx, resolve);
    JSValueProtect(env->ctx, reject);
    *deferred = new napi_deferred__{env, resolve, reject};
    *promise = napi_jsc_add_handle(env, p);
    return napi_jsc_clear_error(env);
}

namespace {
napi_status SettleDeferred(napi_env env, napi_deferred deferred, napi_value resolution, bool resolve) {
    NAPI_PREAMBLE(env);
    CHECK_ARG(env, deferred);
    JSObjectRef fn = resolve ? deferred->resolve : deferred->reject;
    JSValueRef arg = resolution != nullptr ? ToJS(resolution) : JSValueMakeUndefined(env->ctx);
    JSValueRef exc = nullptr;
    JSObjectCallAsFunction(env->ctx, fn, nullptr, 1, &arg, &exc);
    JSValueUnprotect(env->ctx, deferred->resolve);
    JSValueUnprotect(env->ctx, deferred->reject);
    delete deferred;
    if (exc != nullptr)
        return napi_jsc_record_exception(env, exc);
    return napi_jsc_clear_error(env);
}
}  // namespace

napi_status NAPI_CDECL napi_resolve_deferred(napi_env env, napi_deferred deferred, napi_value resolution) {
    return SettleDeferred(env, deferred, resolution, true);
}
napi_status NAPI_CDECL napi_reject_deferred(napi_env env, napi_deferred deferred, napi_value rejection) {
    return SettleDeferred(env, deferred, rejection, false);
}
napi_status NAPI_CDECL napi_is_promise(napi_env env, napi_value value, bool* is_promise) {
    CHECK_ENV(env);
    CHECK_ARG(env, value);
    CHECK_ARG(env, is_promise);
    *is_promise = InstanceOfGlobal(env, ToJS(value), "Promise");
    return napi_jsc_clear_error(env);
}

// ---- type predicates ------------------------------------------------------

napi_status NAPI_CDECL napi_is_arraybuffer(napi_env env, napi_value value, bool* result) {
    CHECK_ENV(env);
    CHECK_ARG(env, value);
    CHECK_ARG(env, result);
    JSValueRef exc = nullptr;
    *result = JSValueGetTypedArrayType(env->ctx, ToJS(value), &exc) == kJSTypedArrayTypeArrayBuffer;
    return napi_jsc_clear_error(env);
}

napi_status NAPI_CDECL napi_is_typedarray(napi_env env, napi_value value, bool* result) {
    CHECK_ENV(env);
    CHECK_ARG(env, value);
    CHECK_ARG(env, result);
    JSValueRef exc = nullptr;
    JSTypedArrayType t = JSValueGetTypedArrayType(env->ctx, ToJS(value), &exc);
    *result = t != kJSTypedArrayTypeNone && t != kJSTypedArrayTypeArrayBuffer;
    if (!*result) {
        // AmeCanvas fix: JSC's C typed-array API doesn't recognize Float16Array. AME-JSC-FLOAT16-FIX
        JSObjectRef f16 = GlobalCtor(env, "Float16Array");
        if (f16 != nullptr && JSValueIsInstanceOfConstructor(env->ctx, ToJS(value), f16, nullptr))
            *result = true;
    }
    return napi_jsc_clear_error(env);
}

napi_status NAPI_CDECL napi_is_dataview(napi_env env, napi_value value, bool* result) {
    CHECK_ENV(env);
    CHECK_ARG(env, value);
    CHECK_ARG(env, result);
    *result = InstanceOfGlobal(env, ToJS(value), "DataView");
    return napi_jsc_clear_error(env);
}

// napi_is_detached_arraybuffer, the ArrayBuffer/TypedArray/DataView family,
// BigInt, and type tags are implemented in jsc_buffers.cc.

// node_api_post_finalizer is declared in napi/js_native_api.h under
// NAPI_EXPERIMENTAL, but js_native_api_jsc.h pulls that header in before this
// TU defines NAPI_EXPERIMENTAL, so the experimental decl is guarded out here.
// Re-declare with explicit C linkage so the exported symbol stays unmangled.
extern "C" NAPI_EXTERN napi_status NAPI_CDECL node_api_post_finalizer(node_api_basic_env, napi_finalize, void*,
                                                                      void*);

napi_status NAPI_CDECL node_api_post_finalizer(node_api_basic_env basic_env, napi_finalize finalize_cb,
                                               void* finalize_data, void* finalize_hint) {
    auto env = const_cast<napi_env>(basic_env);
    CHECK_ENV(env);
    if (finalize_cb != nullptr) {
        std::lock_guard<std::mutex> lk(env->finalizer_mu);
        env->pending_finalizers.push_back({finalize_cb, finalize_data, finalize_hint});
    }
    return napi_ok;
}
