// V8 fast-call backend (napi/fast_call.h). See FAST_CALL_PLAN.md.
//
// Builds a "fast + slow dual entry" JS function: a FunctionTemplate carrying the
// standard slow napi trampoline (FunctionCallbackWrapper::Invoke) AND a
// v8::CFunction assembled at runtime from the caller's napi_fast_signature. The
// fast metadata (CTypeInfo arrays + CFunctionInfo + CFunction) is owned by a
// FastFnHolder whose lifetime is tied to the function's callback-data External.
//
// V8 14.2 specifics baked in: no per-call fallback (no options.fallback);
// TypedArray/AB are read via ArrayBufferView::GetContents (handle-free).

#include <cstdint>
#include <cstring>
#include <memory>
#include <vector>

#define NAPI_EXPERIMENTAL
#include "js_native_api_v8.h"
#include "js_native_api_v8_impl.h"
#include "napi/js_native_api.h"
#include "napi/node_api.h"

#include "v8-fast-api-calls.h"
#include "v8-memory-span.h"

#include "napi/fast_call.h"

namespace {

    // Minimum scratch for napi_fast_get_buffersource: V8 copies an on-heap typed
    // array (up to typed_array_max_size_in_heap, default 64B) into the caller's
    // scratch via ArrayBufferView::GetContents — a smaller buffer would overflow.
    constexpr size_t kFastBufferSourceScratchMin = 64;

    // napi_fast_type -> v8::CTypeInfo. receiver/jsvalue both map to kV8Value.
    v8::CTypeInfo CTypeOf(napi_fast_type t) {
        using Type = v8::CTypeInfo::Type;
        switch (t) {
            case napi_fast_void:     return v8::CTypeInfo(Type::kVoid);
            case napi_fast_bool:     return v8::CTypeInfo(Type::kBool);
            case napi_fast_int32:    return v8::CTypeInfo(Type::kInt32);
            case napi_fast_uint32:   return v8::CTypeInfo(Type::kUint32);
            case napi_fast_int64:    return v8::CTypeInfo(Type::kInt64);
            case napi_fast_uint64:   return v8::CTypeInfo(Type::kUint64);
            case napi_fast_float32:  return v8::CTypeInfo(Type::kFloat32);
            case napi_fast_float64:  return v8::CTypeInfo(Type::kFloat64);
            case napi_fast_pointer:  return v8::CTypeInfo(Type::kPointer);
            case napi_fast_receiver: return v8::CTypeInfo(Type::kV8Value);
            case napi_fast_jsvalue:  return v8::CTypeInfo(Type::kV8Value);
        }
        return v8::CTypeInfo(Type::kVoid);
    }

    // Owns the per-function fast metadata. CFunctionInfo stores a raw pointer
    // into its CTypeInfo array, and CFunction stores a raw pointer to the
    // CFunctionInfo, so both must outlive the JS function — hence this holder is
    // kept alive by a runtime finalizer on the callback-data External.
    struct FastFnHolder {
        std::vector<std::vector<v8::CTypeInfo>> argInfos;          // [overload][receiver..,(options)]
        std::vector<std::unique_ptr<v8::CFunctionInfo>> infos;     // [overload]
        std::vector<v8::CFunction> cfuncs;                         // [overload]

        static void NAPI_CDECL Delete(napi_env /*env*/, void* data, void* /*hint*/) {
            delete static_cast<FastFnHolder*>(data);
        }
    };

    // Shared builder for both the single and the overloaded entry points.
    // `ovs` holds >=1 already-validated overloads (arg_count>=1, fast_fn!=null).
    napi_status CreateFast(napi_env env, const char* utf8name, size_t length, napi_callback slow_cb,
                           const napi_fast_overload* ovs, size_t n, void* data, napi_value* result) {
        v8::EscapableHandleScope scope(env->isolate);

        // Callback data: a CallbackBundle{env, slow_cb, data} wrapped in an
        // External. Drives the slow trampoline AND backs napi_fast_options_get_data.
        v8::Local<v8::Value> cbdata = v8impl::CallbackBundle::New(env, slow_cb, data);
        RETURN_STATUS_IF_FALSE(env, !cbdata.IsEmpty(), napi_generic_failure);

        FastFnHolder* holder = new FastFnHolder();
        // Tie holder lifetime to cbdata immediately so any early return frees it
        // when cbdata is collected.
        v8impl::ReferenceWithFinalizer::New(env, cbdata, 0, v8impl::ReferenceOwnership::kRuntime,
                                            &FastFnHolder::Delete, holder, nullptr);

        holder->argInfos.reserve(n);
        for (size_t i = 0; i < n; ++i) {
            const napi_fast_signature& s = ovs[i].sig;
            std::vector<v8::CTypeInfo> a;
            a.reserve(s.arg_count + (s.wants_options ? 1u : 0u));
            for (uint32_t k = 0; k < s.arg_count; ++k)
                a.push_back(CTypeOf(s.arg_types[k]));
            if (s.wants_options)
                a.push_back(v8::CTypeInfo(v8::CTypeInfo::kCallbackOptionsType));
            holder->argInfos.push_back(std::move(a));
        }

        holder->infos.reserve(n);
        holder->cfuncs.reserve(n);  // stable .data() for the overloads MemorySpan
        for (size_t i = 0; i < n; ++i) {
            v8::CTypeInfo ret = CTypeOf(ovs[i].sig.return_type);
            holder->infos.push_back(std::make_unique<v8::CFunctionInfo>(
                    ret, static_cast<unsigned int>(holder->argInfos[i].size()), holder->argInfos[i].data(),
                    v8::CFunctionInfo::Int64Representation::kNumber));
            holder->cfuncs.emplace_back(ovs[i].fast_fn, holder->infos.back().get());
        }

        v8::Local<v8::FunctionTemplate> tpl;
        if (n == 1) {
            tpl = v8::FunctionTemplate::New(env->isolate, v8impl::FunctionCallbackWrapper::Invoke, cbdata,
                                            v8::Local<v8::Signature>(), 0, v8::ConstructorBehavior::kThrow,
                                            v8::SideEffectType::kHasSideEffect, &holder->cfuncs[0]);
        } else {
            v8::MemorySpan<const v8::CFunction> span(holder->cfuncs.data(), holder->cfuncs.size());
            tpl = v8::FunctionTemplate::NewWithCFunctionOverloads(
                    env->isolate, v8impl::FunctionCallbackWrapper::Invoke, cbdata, v8::Local<v8::Signature>(), 0,
                    v8::ConstructorBehavior::kThrow, v8::SideEffectType::kHasSideEffect, span);
        }

        v8::Local<v8::Function> fn;
        if (!tpl->GetFunction(env->context()).ToLocal(&fn))
            return napi_set_last_error(env, napi_pending_exception);

        if (utf8name != nullptr) {
            v8::Local<v8::String> name_string;
            int len = (length == NAPI_AUTO_LENGTH) ? -1 : static_cast<int>(length);
            if (!v8::String::NewFromUtf8(env->isolate, utf8name, v8::NewStringType::kInternalized, len)
                         .ToLocal(&name_string))
                return napi_set_last_error(env, napi_generic_failure);
            fn->SetName(name_string);
        }

        *result = v8impl::JsValueFromV8LocalValue(scope.Escape(fn));
        return napi_clear_last_error(env);
    }

} // namespace

napi_status NAPI_CDECL napi_create_fast_function(napi_env env, const char* utf8name, size_t length,
                                                 napi_callback slow_cb, const napi_fast_signature* sig,
                                                 const void* fast_fn, void* data, napi_value* result) {
    NAPI_PREAMBLE(env);
    CHECK_ARG(env, result);
    CHECK_ARG(env, slow_cb);

    // No fast path requested (or no receiver slot) -> behave exactly like
    // napi_create_function. arg_types must start with the receiver (arg 0).
    if (sig == nullptr || fast_fn == nullptr || sig->arg_count == 0 || sig->arg_types == nullptr)
        return napi_create_function(env, utf8name, length, slow_cb, data, result);

    napi_fast_overload ov{*sig, fast_fn};
    STATUS_CALL(CreateFast(env, utf8name, length, slow_cb, &ov, 1, data, result));
    return GET_RETURN_STATUS(env);
}

napi_status NAPI_CDECL napi_create_fast_function_overloads(napi_env env, const char* utf8name, size_t length,
                                                           napi_callback slow_cb,
                                                           const napi_fast_overload* overloads,
                                                           size_t overload_count, void* data, napi_value* result) {
    NAPI_PREAMBLE(env);
    CHECK_ARG(env, result);
    CHECK_ARG(env, slow_cb);

    // Validate: every overload must be a real fast variant, else fall back to slow.
    bool usable = overload_count > 0 && overloads != nullptr;
    for (size_t i = 0; usable && i < overload_count; ++i) {
        const napi_fast_signature& s = overloads[i].sig;
        if (overloads[i].fast_fn == nullptr || s.arg_count == 0 || s.arg_types == nullptr)
            usable = false;
    }
    if (!usable)
        return napi_create_function(env, utf8name, length, slow_cb, data, result);

    STATUS_CALL(CreateFast(env, utf8name, length, slow_cb, overloads, overload_count, data, result));
    return GET_RETURN_STATUS(env);
}

napi_status NAPI_CDECL napi_define_fast_accessor(napi_env env, napi_value object, napi_value name, napi_value getter,
                                                 napi_value setter, napi_property_attributes attributes) {
    NAPI_PREAMBLE(env);
    CHECK_ARG(env, object);
    CHECK_ARG(env, name);
    RETURN_STATUS_IF_FALSE(env, getter != nullptr || setter != nullptr, napi_invalid_arg);

    v8::Local<v8::Value> objv = v8impl::V8LocalValueFromJsValue(object);
    RETURN_STATUS_IF_FALSE(env, objv->IsObject(), napi_object_expected);
    v8::Local<v8::Object> obj = objv.As<v8::Object>();

    v8::Local<v8::Value> namev = v8impl::V8LocalValueFromJsValue(name);
    RETURN_STATUS_IF_FALSE(env, namev->IsName(), napi_name_expected);
    v8::Local<v8::Name> key = namev.As<v8::Name>();

    v8::Local<v8::Function> get_fn;  // empty => read-disabled
    v8::Local<v8::Function> set_fn;  // empty => write-disabled
    if (getter != nullptr) {
        v8::Local<v8::Value> gv = v8impl::V8LocalValueFromJsValue(getter);
        RETURN_STATUS_IF_FALSE(env, gv->IsFunction(), napi_function_expected);
        get_fn = gv.As<v8::Function>();
    }
    if (setter != nullptr) {
        v8::Local<v8::Value> sv = v8impl::V8LocalValueFromJsValue(setter);
        RETURN_STATUS_IF_FALSE(env, sv->IsFunction(), napi_function_expected);
        set_fn = sv.As<v8::Function>();
    }

    // Accessors carry no "writable" bit; only enumerable/configurable apply.
    int attr = v8::None;
    if ((attributes & napi_enumerable) == 0)
        attr |= v8::DontEnum;
    if ((attributes & napi_configurable) == 0)
        attr |= v8::DontDelete;

    // When get_fn/set_fn were built by napi_create_fast_function they carry a
    // v8::CFunction; installing them as an accessor lets V8 reach the fast C
    // entry on optimized, monomorphic property access (slow trampoline backs
    // every other case).
    obj->SetAccessorProperty(key, get_fn, set_fn, static_cast<v8::PropertyAttribute>(attr));
    return GET_RETURN_STATUS(env);
}

napi_status NAPI_CDECL napi_fast_wrap(napi_env env, napi_value js_object, void* native, const void* type_tag,
                                      napi_finalize finalize_cb, void* finalize_hint, napi_ref* result) {
    CHECK_ENV(env);
    // CHECK_ENV (not CHECK_ENV_NOT_IN_GC) so it does not self-scope via the macro;
    // v8impl::Wrap + the type-tag Private read the current isolate/context, so
    // enter them here (needed for same-thread multi-runtime).
    v8impl::CallScope __ame_call_scope(env);
    // native and type_tag are stored as V8 aligned pointers (the low bit is
    // reserved). Reject an unaligned input cleanly instead of letting V8 raise a
    // fatal "Unaligned pointer" — e.g. a `static const char` tag sentinel, which
    // is byte-aligned. Use a >=2-aligned tag (an int/pointer or alignas(2)).
    RETURN_STATUS_IF_FALSE(env, (reinterpret_cast<uintptr_t>(native) & 1u) == 0, napi_invalid_arg);
    RETURN_STATUS_IF_FALSE(env, (reinterpret_cast<uintptr_t>(type_tag) & 1u) == 0, napi_invalid_arg);

    // Standard wrap (private property + finalizer/ref) so napi_unwrap keeps working.
    napi_status status = v8impl::Wrap(env, js_object, native, finalize_cb, finalize_hint, result);
    if (status != napi_ok)
        return status;

    // Mirror into the fast-readable internal fields: 0 = native ptr, 1 = type tag
    // (so the unwrap helpers can reject a wrong-class receiver), when reserved.
    v8::Local<v8::Value> v = v8impl::V8LocalValueFromJsValue(js_object);
    if (v->IsObject()) {
        v8::Local<v8::Object> obj = v.As<v8::Object>();
        int fields = obj->InternalFieldCount();
        if (fields >= 1)
            obj->SetAlignedPointerInInternalField(0, native, v8::kEmbedderDataTypeTagDefault);
        if (fields >= 2)
            obj->SetAlignedPointerInInternalField(1, const_cast<void*>(type_tag), v8::kEmbedderDataTypeTagDefault);
    }
    return napi_clear_last_error(env);
}

//=== Fast-call-only helpers (run inside a fast callback: no env, no alloc) ====

namespace {
    // Read native ptr (field 0) iff `v` is an object carrying the fast slot AND
    // (when expected_tag != NULL) its type tag (field 1) matches — guarding both
    // non-object receivers and cross-class type confusion. fast-safe.
    void* UnwrapNative(v8::Local<v8::Value> v, const void* expected_tag) {
        if (v.IsEmpty() || !v->IsObject())
            return nullptr;
        v8::Local<v8::Object> obj = v.As<v8::Object>();
        int fields = obj->InternalFieldCount();
        if (fields < 1)
            return nullptr;
        if (expected_tag != nullptr) {
            if (fields < 2 ||
                obj->GetAlignedPointerFromInternalField(1, v8::kEmbedderDataTypeTagDefault) != expected_tag)
                return nullptr;
        }
        return obj->GetAlignedPointerFromInternalField(0, v8::kEmbedderDataTypeTagDefault);
    }
} // namespace

void* NAPI_CDECL napi_fast_unwrap(napi_fast_recv recv, const void* expected_type_tag) {
    v8::Local<v8::Value> v;
    static_assert(sizeof(v) == sizeof(recv), "Local must be pointer-sized");
    std::memcpy(static_cast<void*>(&v), &recv, sizeof(v));
    return UnwrapNative(v, expected_type_tag);
}

void* NAPI_CDECL napi_fast_value_unwrap(napi_fast_value value, const void* expected_type_tag) {
    v8::Local<v8::Value> v;
    std::memcpy(static_cast<void*>(&v), &value, sizeof(v));
    return UnwrapNative(v, expected_type_tag);
}

bool NAPI_CDECL napi_fast_value_is_nullish(napi_fast_value value) {
    v8::Local<v8::Value> v;
    std::memcpy(static_cast<void*>(&v), &value, sizeof(v));
    return v.IsEmpty() || v->IsNullOrUndefined();
}

namespace {
    napi_fast_bs_type ElemKind(v8::Local<v8::Value> v) {
        if (v->IsFloat32Array())      return napi_fast_bs_f32;
        if (v->IsFloat64Array())      return napi_fast_bs_f64;
        if (v->IsInt32Array())        return napi_fast_bs_i32;
        if (v->IsUint32Array())       return napi_fast_bs_u32;
        if (v->IsInt16Array())        return napi_fast_bs_i16;
        if (v->IsUint16Array())       return napi_fast_bs_u16;
        if (v->IsInt8Array())         return napi_fast_bs_i8;
        if (v->IsUint8ClampedArray()) return napi_fast_bs_u8c;
        if (v->IsUint8Array())        return napi_fast_bs_u8;
        if (v->IsBigInt64Array())     return napi_fast_bs_i64;
        if (v->IsBigUint64Array())    return napi_fast_bs_u64;
        return napi_fast_bs_unknown;
    }
} // namespace

bool NAPI_CDECL napi_fast_get_buffersource(napi_fast_value value, void* scratch, size_t scratch_len,
                                           void** out_data, size_t* out_byte_length, napi_fast_bs_type* out_elem) {
    v8::Local<v8::Value> v;
    std::memcpy(static_cast<void*>(&v), &value, sizeof(v));
    if (v.IsEmpty())
        return false;

    if (v->IsArrayBufferView()) {
        // GetContents copies an on-heap typed array into `scratch`; an undersized
        // scratch would overflow. Off-heap arrays are returned zero-copy, but V8
        // still needs a valid storage span. Require the documented minimum.
        if (scratch == nullptr || scratch_len < kFastBufferSourceScratchMin)
            return false;
        v8::Local<v8::ArrayBufferView> view = v.As<v8::ArrayBufferView>();
        v8::MemorySpan<uint8_t> storage(static_cast<uint8_t*>(scratch), scratch_len);
        v8::MemorySpan<uint8_t> span = view->GetContents(storage);
        if (out_data) *out_data = span.data();
        if (out_byte_length) *out_byte_length = span.size();
        if (out_elem) *out_elem = ElemKind(v);
        return true;
    }
    if (v->IsArrayBuffer()) {
        v8::Local<v8::ArrayBuffer> ab = v.As<v8::ArrayBuffer>();
        if (out_data) *out_data = ab->Data();
        if (out_byte_length) *out_byte_length = ab->ByteLength();
        if (out_elem) *out_elem = napi_fast_bs_arraybuffer;
        return true;
    }
    if (v->IsSharedArrayBuffer()) {
        v8::Local<v8::SharedArrayBuffer> ab = v.As<v8::SharedArrayBuffer>();
        if (out_data) *out_data = ab->Data();
        if (out_byte_length) *out_byte_length = ab->ByteLength();
        if (out_elem) *out_elem = napi_fast_bs_arraybuffer;
        return true;
    }
    return false;
}

void* NAPI_CDECL napi_fast_options_get_data(napi_fast_options opts) {
    if (opts == nullptr)
        return nullptr;
    auto* o = reinterpret_cast<v8::FastApiCallbackOptions*>(opts);
    v8::Local<v8::Value> d = o->data;
    if (d.IsEmpty() || !d->IsExternal())
        return nullptr;
    auto* bundle = static_cast<v8impl::CallbackBundle*>(d.As<v8::External>()->Value());
    return bundle ? bundle->cb_data : nullptr;
}
