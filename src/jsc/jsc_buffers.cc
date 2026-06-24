// JSC Node-API backend — ArrayBuffer / TypedArray / DataView, BigInt, type
// tags, and arraybuffer detach-state. These map onto JavaScriptCore's
// JSTypedArray.h C API where one exists, and onto small JS shims (BigInt
// literals, DataView/transfer) where the C API has no direct entry point.

#include "js_native_api_jsc.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include <JavaScriptCore/JSTypedArray.h>

namespace {

struct JSStr {
    JSStringRef s;
    explicit JSStr(const char* u) : s(JSStringCreateWithUTF8CString(u ? u : "")) {}
    ~JSStr() { if (s) JSStringRelease(s); }
    operator JSStringRef() const { return s; }
};

JSObjectRef ToObj(napi_env env, napi_value v) { return JSValueToObject(env->ctx, ToJS(v), nullptr); }

JSValueRef GetProp(napi_env env, JSObjectRef o, const char* name) {
    return JSObjectGetProperty(env->ctx, o, JSStr(name), nullptr);
}

JSObjectRef GlobalCtor(napi_env env, const char* name) {
    JSValueRef v = GetProp(env, JSContextGetGlobalObject(env->ctx), name);
    return (v && JSValueIsObject(env->ctx, v)) ? JSValueToObject(env->ctx, v, nullptr) : nullptr;
}

bool IsArrayBufferVal(napi_env env, napi_value v) {
    return JSValueGetTypedArrayType(env->ctx, ToJS(v), nullptr) == kJSTypedArrayTypeArrayBuffer;
}

// Evaluate a self-contained snippet to a value (used for BigInt literals).
napi_status EvalToValue(napi_env env, const std::string& code, napi_value* result) {
    JSValueRef exc = nullptr;
    JSStringRef s = JSStringCreateWithUTF8CString(code.c_str());
    JSValueRef r = JSEvaluateScript(env->ctx, s, nullptr, nullptr, 0, &exc);
    JSStringRelease(s);
    if (exc != nullptr)
        return napi_jsc_record_exception(env, exc);
    *result = napi_jsc_add_handle(env, r);
    return napi_jsc_clear_error(env);
}

// ArrayBuffer-backing deallocators (JSTypedArrayBytesDeallocator).
void DeallocFree(void* bytes, void*) { std::free(bytes); }

struct AbFinalizer {
    napi_env env;
    napi_finalize cb;
    void* data;
    void* hint;
};
void DeallocFinalize(void* /*bytes*/, void* ctx) {
    auto* f = static_cast<AbFinalizer*>(ctx);
    if (f == nullptr)
        return;
    if (f->cb != nullptr && f->env != nullptr) {
        std::lock_guard<std::mutex> lk(f->env->finalizer_mu);
        f->env->pending_finalizers.push_back({f->cb, f->data, f->hint});
    }
    delete f;
}

JSTypedArrayType ToJSTA(napi_typedarray_type t) {
    switch (t) {
        case napi_int8_array: return kJSTypedArrayTypeInt8Array;
        case napi_uint8_array: return kJSTypedArrayTypeUint8Array;
        case napi_uint8_clamped_array: return kJSTypedArrayTypeUint8ClampedArray;
        case napi_int16_array: return kJSTypedArrayTypeInt16Array;
        case napi_uint16_array: return kJSTypedArrayTypeUint16Array;
        case napi_int32_array: return kJSTypedArrayTypeInt32Array;
        case napi_uint32_array: return kJSTypedArrayTypeUint32Array;
        case napi_float32_array: return kJSTypedArrayTypeFloat32Array;
        case napi_float64_array: return kJSTypedArrayTypeFloat64Array;
        case napi_bigint64_array: return kJSTypedArrayTypeBigInt64Array;
        case napi_biguint64_array: return kJSTypedArrayTypeBigUint64Array;
        default: return kJSTypedArrayTypeNone;  // napi_float16_array: no JSC C-API type
    }
}

bool FromJSTA(JSTypedArrayType t, napi_typedarray_type* out) {
    switch (t) {
        case kJSTypedArrayTypeInt8Array: *out = napi_int8_array; return true;
        case kJSTypedArrayTypeUint8Array: *out = napi_uint8_array; return true;
        case kJSTypedArrayTypeUint8ClampedArray: *out = napi_uint8_clamped_array; return true;
        case kJSTypedArrayTypeInt16Array: *out = napi_int16_array; return true;
        case kJSTypedArrayTypeUint16Array: *out = napi_uint16_array; return true;
        case kJSTypedArrayTypeInt32Array: *out = napi_int32_array; return true;
        case kJSTypedArrayTypeUint32Array: *out = napi_uint32_array; return true;
        case kJSTypedArrayTypeFloat32Array: *out = napi_float32_array; return true;
        case kJSTypedArrayTypeFloat64Array: *out = napi_float64_array; return true;
        case kJSTypedArrayTypeBigInt64Array: *out = napi_bigint64_array; return true;
        case kJSTypedArrayTypeBigUint64Array: *out = napi_biguint64_array; return true;
        default: return false;
    }
}

bool IsBigIntVal(napi_env env, JSValueRef v) {
    if (__builtin_available(macOS 15.0, iOS 18.0, *))
        return JSValueGetType(env->ctx, v) == kJSTypeBigInt;
    return false;  // older OS: no C-API BigInt type tag (deployment target < 15)
}

}  // namespace

// ===========================================================================
// ArrayBuffer
// ===========================================================================

napi_status NAPI_CDECL napi_create_arraybuffer(napi_env env, size_t byte_length, void** data, napi_value* result) {
    NAPI_PREAMBLE(env);
    CHECK_ARG(env, result);
    void* mem = std::calloc(1, byte_length != 0 ? byte_length : 1);
    if (mem == nullptr)
        return napi_jsc_set_error(env, napi_generic_failure);
    JSValueRef exc = nullptr;
    JSObjectRef ab = JSObjectMakeArrayBufferWithBytesNoCopy(env->ctx, mem, byte_length, DeallocFree, nullptr, &exc);
    if (ab == nullptr || exc != nullptr) {
        std::free(mem);
        return napi_jsc_record_exception(env, exc);
    }
    if (data != nullptr)
        *data = mem;
    *result = napi_jsc_add_handle(env, ab);
    return napi_jsc_clear_error(env);
}

napi_status NAPI_CDECL napi_create_external_arraybuffer(napi_env env, void* external_data, size_t byte_length,
                                                        napi_finalize finalize_cb, void* finalize_hint,
                                                        napi_value* result) {
    NAPI_PREAMBLE(env);
    CHECK_ARG(env, result);
    AbFinalizer* f = finalize_cb != nullptr ? new AbFinalizer{env, finalize_cb, external_data, finalize_hint} : nullptr;
    JSValueRef exc = nullptr;
    // No finalizer => null deallocator: JSC won't free the caller's buffer.
    JSObjectRef ab = JSObjectMakeArrayBufferWithBytesNoCopy(env->ctx, external_data, byte_length,
                                                            f != nullptr ? DeallocFinalize : nullptr, f, &exc);
    if (ab == nullptr || exc != nullptr) {
        delete f;
        return napi_jsc_record_exception(env, exc);
    }
    *result = napi_jsc_add_handle(env, ab);
    return napi_jsc_clear_error(env);
}

napi_status NAPI_CDECL napi_get_arraybuffer_info(napi_env env, napi_value arraybuffer, void** data,
                                                 size_t* byte_length) {
    NAPI_PREAMBLE(env);
    CHECK_ARG(env, arraybuffer);
    RETURN_STATUS_IF_FALSE(env, IsArrayBufferVal(env, arraybuffer), napi_arraybuffer_expected);
    JSObjectRef o = ToObj(env, arraybuffer);
    JSValueRef exc = nullptr;
    if (data != nullptr)
        *data = JSObjectGetArrayBufferBytesPtr(env->ctx, o, &exc);
    if (byte_length != nullptr)
        *byte_length = JSObjectGetArrayBufferByteLength(env->ctx, o, &exc);
    if (exc != nullptr)
        return napi_jsc_record_exception(env, exc);
    return napi_jsc_clear_error(env);
}

napi_status NAPI_CDECL napi_detach_arraybuffer(napi_env env, napi_value arraybuffer) {
    NAPI_PREAMBLE(env);
    CHECK_ARG(env, arraybuffer);
    RETURN_STATUS_IF_FALSE(env, IsArrayBufferVal(env, arraybuffer), napi_arraybuffer_expected);
    // JSC has no C-API detach; ArrayBuffer.prototype.transfer() detaches the
    // source. Caveat: once native code has fetched the buffer's bytes pointer
    // (JSObjectGetArrayBufferBytesPtr, e.g. via napi_get_arraybuffer_info) JSC
    // pins the backing store and transfer() can no longer detach it (it returns
    // a copy and leaves the source attached) — detaching would dangle the
    // outstanding native pointer. We therefore verify the detach took effect and
    // report napi_detachable_arraybuffer_expected if it did not.
    JSObjectRef o = ToObj(env, arraybuffer);
    JSValueRef tv = GetProp(env, o, "transfer");
    RETURN_STATUS_IF_FALSE(env, tv != nullptr && JSValueIsObject(env->ctx, tv), napi_detachable_arraybuffer_expected);
    JSObjectRef tfn = JSValueToObject(env->ctx, tv, nullptr);
    JSValueRef exc = nullptr;
    JSObjectCallAsFunction(env->ctx, tfn, o, 0, nullptr, &exc);
    if (exc != nullptr)
        return napi_jsc_record_exception(env, exc);
    JSValueRef d = GetProp(env, o, "detached");
    RETURN_STATUS_IF_FALSE(env, d != nullptr && JSValueIsBoolean(env->ctx, d) && JSValueToBoolean(env->ctx, d),
                           napi_detachable_arraybuffer_expected);
    return napi_jsc_clear_error(env);
}

napi_status NAPI_CDECL napi_is_detached_arraybuffer(napi_env env, napi_value value, bool* result) {
    CHECK_ENV(env);
    CHECK_ARG(env, value);
    CHECK_ARG(env, result);
    // ArrayBuffer.prototype.detached is a boolean accessor; undefined elsewhere.
    *result = false;
    if (JSValueIsObject(env->ctx, ToJS(value))) {
        JSValueRef d = GetProp(env, ToObj(env, value), "detached");
        *result = d != nullptr && JSValueIsBoolean(env->ctx, d) && JSValueToBoolean(env->ctx, d);
    }
    return napi_jsc_clear_error(env);
}

// ===========================================================================
// TypedArray
// ===========================================================================

napi_status NAPI_CDECL napi_create_typedarray(napi_env env, napi_typedarray_type type, size_t length,
                                              napi_value arraybuffer, size_t byte_offset, napi_value* result) {
    NAPI_PREAMBLE(env);
    CHECK_ARG(env, arraybuffer);
    CHECK_ARG(env, result);
    JSTypedArrayType jt = ToJSTA(type);
    RETURN_STATUS_IF_FALSE(env, jt != kJSTypedArrayTypeNone, napi_invalid_arg);  // e.g. float16: unsupported
    RETURN_STATUS_IF_FALSE(env, IsArrayBufferVal(env, arraybuffer), napi_arraybuffer_expected);
    JSObjectRef buf = ToObj(env, arraybuffer);
    JSValueRef exc = nullptr;
    JSObjectRef ta = JSObjectMakeTypedArrayWithArrayBufferAndOffset(env->ctx, jt, buf, byte_offset, length, &exc);
    if (exc != nullptr)
        return napi_jsc_record_exception(env, exc);
    *result = napi_jsc_add_handle(env, ta);
    return napi_jsc_clear_error(env);
}

napi_status NAPI_CDECL napi_get_typedarray_info(napi_env env, napi_value typedarray, napi_typedarray_type* type,
                                                size_t* length, void** data, napi_value* arraybuffer,
                                                size_t* byte_offset) {
    NAPI_PREAMBLE(env);
    CHECK_ARG(env, typedarray);
    JSValueRef exc = nullptr;
    JSTypedArrayType jt = JSValueGetTypedArrayType(env->ctx, ToJS(typedarray), &exc);
    napi_typedarray_type nt;
    RETURN_STATUS_IF_FALSE(env, FromJSTA(jt, &nt), napi_invalid_arg);
    JSObjectRef o = ToObj(env, typedarray);
    if (type != nullptr)
        *type = nt;
    if (length != nullptr)
        *length = JSObjectGetTypedArrayLength(env->ctx, o, &exc);
    if (data != nullptr)
        *data = JSObjectGetTypedArrayBytesPtr(env->ctx, o, &exc);
    if (byte_offset != nullptr)
        *byte_offset = JSObjectGetTypedArrayByteOffset(env->ctx, o, &exc);
    if (arraybuffer != nullptr) {
        JSObjectRef buf = JSObjectGetTypedArrayBuffer(env->ctx, o, &exc);
        *arraybuffer = napi_jsc_add_handle(env, buf);
    }
    if (exc != nullptr)
        return napi_jsc_record_exception(env, exc);
    return napi_jsc_clear_error(env);
}

// ===========================================================================
// DataView (no C-API constructor; use the JS DataView constructor)
// ===========================================================================

napi_status NAPI_CDECL napi_create_dataview(napi_env env, size_t length, napi_value arraybuffer, size_t byte_offset,
                                            napi_value* result) {
    NAPI_PREAMBLE(env);
    CHECK_ARG(env, arraybuffer);
    CHECK_ARG(env, result);
    RETURN_STATUS_IF_FALSE(env, IsArrayBufferVal(env, arraybuffer), napi_arraybuffer_expected);
    JSObjectRef ctor = GlobalCtor(env, "DataView");
    RETURN_STATUS_IF_FALSE(env, ctor != nullptr, napi_generic_failure);
    JSValueRef args[3] = {ToJS(arraybuffer), JSValueMakeNumber(env->ctx, static_cast<double>(byte_offset)),
                          JSValueMakeNumber(env->ctx, static_cast<double>(length))};
    JSValueRef exc = nullptr;
    JSObjectRef dv = JSObjectCallAsConstructor(env->ctx, ctor, 3, args, &exc);
    if (exc != nullptr)
        return napi_jsc_record_exception(env, exc);
    *result = napi_jsc_add_handle(env, dv);
    return napi_jsc_clear_error(env);
}

napi_status NAPI_CDECL napi_get_dataview_info(napi_env env, napi_value dataview, size_t* byte_length, void** data,
                                              napi_value* arraybuffer, size_t* byte_offset) {
    NAPI_PREAMBLE(env);
    CHECK_ARG(env, dataview);
    JSObjectRef ctor = GlobalCtor(env, "DataView");
    RETURN_STATUS_IF_FALSE(
        env, ctor != nullptr && JSValueIsInstanceOfConstructor(env->ctx, ToJS(dataview), ctor, nullptr),
        napi_invalid_arg);
    JSObjectRef o = ToObj(env, dataview);
    size_t off = static_cast<size_t>(JSValueToNumber(env->ctx, GetProp(env, o, "byteOffset"), nullptr));
    size_t len = static_cast<size_t>(JSValueToNumber(env->ctx, GetProp(env, o, "byteLength"), nullptr));
    if (byte_length != nullptr)
        *byte_length = len;
    if (byte_offset != nullptr)
        *byte_offset = off;
    JSValueRef bufv = GetProp(env, o, "buffer");
    JSObjectRef buf = JSValueToObject(env->ctx, bufv, nullptr);
    if (arraybuffer != nullptr)
        *arraybuffer = napi_jsc_add_handle(env, buf);
    if (data != nullptr) {
        JSValueRef exc = nullptr;
        void* base = JSObjectGetArrayBufferBytesPtr(env->ctx, buf, &exc);
        *data = base != nullptr ? static_cast<char*>(base) + off : nullptr;
    }
    return napi_jsc_clear_error(env);
}

// ===========================================================================
// BigInt (no C-API surface; BigInt literals + asIntN/asUintN via JS)
// ===========================================================================

napi_status NAPI_CDECL napi_create_bigint_int64(napi_env env, int64_t value, napi_value* result) {
    NAPI_PREAMBLE(env);
    CHECK_ARG(env, result);
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%lldn", static_cast<long long>(value));
    return EvalToValue(env, buf, result);
}

napi_status NAPI_CDECL napi_create_bigint_uint64(napi_env env, uint64_t value, napi_value* result) {
    NAPI_PREAMBLE(env);
    CHECK_ARG(env, result);
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%llun", static_cast<unsigned long long>(value));
    return EvalToValue(env, buf, result);
}

napi_status NAPI_CDECL napi_create_bigint_words(napi_env env, int sign_bit, size_t word_count, const uint64_t* words,
                                                napi_value* result) {
    NAPI_PREAMBLE(env);
    CHECK_ARG(env, result);
    if (word_count == 0 || words == nullptr)
        return EvalToValue(env, "0n", result);
    // Magnitude as a hex literal, most-significant word first; a leading unary
    // minus handles the sign. `-0x..n` is a valid BigInt expression.
    std::string code = sign_bit ? "-0x" : "0x";
    char chunk[20];
    std::snprintf(chunk, sizeof(chunk), "%llx", static_cast<unsigned long long>(words[word_count - 1]));
    code += chunk;
    for (size_t i = word_count - 1; i-- > 0;) {
        std::snprintf(chunk, sizeof(chunk), "%016llx", static_cast<unsigned long long>(words[i]));
        code += chunk;
    }
    code += "n";
    return EvalToValue(env, code, result);
}

namespace {
// Call BigInt.asIntN/asUintN(64, value) and return the wrapped bigint + whether
// the original was unchanged (lossless).
napi_status BigIntWrapped(napi_env env, napi_value value, const char* fn_name, JSValueRef* wrapped, bool* lossless) {
    JSObjectRef bigint = GlobalCtor(env, "BigInt");
    RETURN_STATUS_IF_FALSE(env, bigint != nullptr, napi_generic_failure);
    JSValueRef fnv = GetProp(env, bigint, fn_name);
    RETURN_STATUS_IF_FALSE(env, fnv != nullptr && JSValueIsObject(env->ctx, fnv), napi_generic_failure);
    JSObjectRef fn = JSValueToObject(env->ctx, fnv, nullptr);
    JSValueRef args[2] = {JSValueMakeNumber(env->ctx, 64), ToJS(value)};
    JSValueRef exc = nullptr;
    *wrapped = JSObjectCallAsFunction(env->ctx, fn, bigint, 2, args, &exc);
    if (exc != nullptr)
        return napi_jsc_record_exception(env, exc);
    if (lossless != nullptr)
        *lossless = JSValueIsStrictEqual(env->ctx, ToJS(value), *wrapped);
    return napi_ok;
}
}  // namespace

napi_status NAPI_CDECL napi_get_value_bigint_int64(napi_env env, napi_value value, int64_t* result, bool* lossless) {
    NAPI_PREAMBLE(env);
    CHECK_ARG(env, value);
    CHECK_ARG(env, result);
    RETURN_STATUS_IF_FALSE(env, IsBigIntVal(env, ToJS(value)), napi_bigint_expected);
    JSValueRef wrapped = nullptr;
    napi_status s = BigIntWrapped(env, value, "asIntN", &wrapped, lossless);
    if (s != napi_ok)
        return s;
    JSStringRef str = JSValueToStringCopy(env->ctx, wrapped, nullptr);
    char buf[32];
    JSStringGetUTF8CString(str, buf, sizeof(buf));
    JSStringRelease(str);
    *result = std::strtoll(buf, nullptr, 10);
    return napi_jsc_clear_error(env);
}

napi_status NAPI_CDECL napi_get_value_bigint_uint64(napi_env env, napi_value value, uint64_t* result,
                                                    bool* lossless) {
    NAPI_PREAMBLE(env);
    CHECK_ARG(env, value);
    CHECK_ARG(env, result);
    RETURN_STATUS_IF_FALSE(env, IsBigIntVal(env, ToJS(value)), napi_bigint_expected);
    JSValueRef wrapped = nullptr;
    napi_status s = BigIntWrapped(env, value, "asUintN", &wrapped, lossless);
    if (s != napi_ok)
        return s;
    JSStringRef str = JSValueToStringCopy(env->ctx, wrapped, nullptr);
    char buf[32];
    JSStringGetUTF8CString(str, buf, sizeof(buf));
    JSStringRelease(str);
    *result = std::strtoull(buf, nullptr, 10);
    return napi_jsc_clear_error(env);
}

napi_status NAPI_CDECL napi_get_value_bigint_words(napi_env env, napi_value value, int* sign_bit, size_t* word_count,
                                                   uint64_t* words) {
    NAPI_PREAMBLE(env);
    CHECK_ARG(env, value);
    CHECK_ARG(env, word_count);
    RETURN_STATUS_IF_FALSE(env, IsBigIntVal(env, ToJS(value)), napi_bigint_expected);
    // value.toString(16) yields the signed hex magnitude ("-ff" / "ff" / "0").
    JSObjectRef boxed = ToObj(env, value);
    JSValueRef tsv = GetProp(env, boxed, "toString");
    JSObjectRef ts = JSValueToObject(env->ctx, tsv, nullptr);
    JSValueRef radix = JSValueMakeNumber(env->ctx, 16);
    JSValueRef exc = nullptr;
    JSValueRef hexv = JSObjectCallAsFunction(env->ctx, ts, boxed, 1, &radix, &exc);
    if (exc != nullptr)
        return napi_jsc_record_exception(env, exc);
    JSStringRef hs = JSValueToStringCopy(env->ctx, hexv, nullptr);
    std::vector<char> hb(JSStringGetMaximumUTF8CStringSize(hs));
    JSStringGetUTF8CString(hs, hb.data(), hb.size());
    JSStringRelease(hs);
    std::string hex(hb.data());

    bool neg = !hex.empty() && hex[0] == '-';
    if (neg)
        hex.erase(0, 1);
    if (hex == "0")
        hex.clear();
    size_t needed = (hex.size() + 15) / 16;
    if (sign_bit != nullptr)
        *sign_bit = neg ? 1 : 0;
    if (words != nullptr) {
        size_t cap = *word_count;
        for (size_t w = 0; w < needed && w < cap; ++w) {
            size_t end = hex.size() - 16 * w;
            size_t start = end >= 16 ? end - 16 : 0;
            words[w] = std::strtoull(hex.substr(start, end - start).c_str(), nullptr, 16);
        }
    }
    *word_count = needed;
    return napi_jsc_clear_error(env);
}

// ===========================================================================
// Type tags — stored under a hidden per-env symbol as a 32-hex-digit string.
// ===========================================================================

napi_status NAPI_CDECL napi_type_tag_object(napi_env env, napi_value value, const napi_type_tag* type_tag) {
    NAPI_PREAMBLE(env);
    CHECK_ARG(env, value);
    CHECK_ARG(env, type_tag);
    JSObjectRef o = ToObj(env, value);
    RETURN_STATUS_IF_FALSE(env, o != nullptr, napi_object_expected);
    // Tagging twice is an error (matches the spec).
    JSValueRef existing = JSObjectGetPropertyForKey(env->ctx, o, env->type_tag_key, nullptr);
    RETURN_STATUS_IF_FALSE(env, existing == nullptr || !JSValueIsString(env->ctx, existing), napi_invalid_arg);
    char buf[33];
    std::snprintf(buf, sizeof(buf), "%016llx%016llx", static_cast<unsigned long long>(type_tag->lower),
                  static_cast<unsigned long long>(type_tag->upper));
    JSValueRef tagv = JSValueMakeString(env->ctx, JSStr(buf));
    JSValueRef exc = nullptr;
    JSObjectSetPropertyForKey(env->ctx, o, env->type_tag_key, tagv,
                              kJSPropertyAttributeDontEnum | kJSPropertyAttributeDontDelete, &exc);
    if (exc != nullptr)
        return napi_jsc_record_exception(env, exc);
    return napi_jsc_clear_error(env);
}

napi_status NAPI_CDECL napi_check_object_type_tag(napi_env env, napi_value value, const napi_type_tag* type_tag,
                                                  bool* result) {
    NAPI_PREAMBLE(env);
    CHECK_ARG(env, value);
    CHECK_ARG(env, type_tag);
    CHECK_ARG(env, result);
    *result = false;
    JSObjectRef o = ToObj(env, value);
    if (o == nullptr)
        return napi_jsc_clear_error(env);
    JSValueRef tagv = JSObjectGetPropertyForKey(env->ctx, o, env->type_tag_key, nullptr);
    if (tagv == nullptr || !JSValueIsString(env->ctx, tagv))
        return napi_jsc_clear_error(env);
    JSStringRef s = JSValueToStringCopy(env->ctx, tagv, nullptr);
    char buf[64];
    JSStringGetUTF8CString(s, buf, sizeof(buf));
    JSStringRelease(s);
    char want[33];
    std::snprintf(want, sizeof(want), "%016llx%016llx", static_cast<unsigned long long>(type_tag->lower),
                  static_cast<unsigned long long>(type_tag->upper));
    *result = std::strcmp(buf, want) == 0;
    return napi_jsc_clear_error(env);
}
