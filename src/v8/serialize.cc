// Structured-clone serialization via V8's ValueSerializer / ValueDeserializer.
// See include/napi_v8/serialize.h for the API contract. v1 = plain (no Delegate):
// V8 built-in types only; functions / symbols / SharedArrayBuffer / host objects
// throw a DataClone exception (surfaced as napi_pending_exception).

#include "napi_v8/serialize.h"

#include "js_native_api_v8.h"
#include "js_native_api_v8_impl.h"

#include <cstdlib>  // ::free
#include <memory>   // std::shared_ptr (backing-store transfer)
#include <utility>  // std::pair, std::move

#include "v8-array-buffer.h"  // v8::ArrayBuffer / v8::BackingStore (transfer)
#include "v8-context.h"
#include "v8-value-serializer.h"

napi_status NAPI_CDECL napi_v8_serialize(napi_env env, napi_value value,
                                         uint8_t **data, size_t *length) {
    NAPI_PREAMBLE(env);
    CHECK_ARG(env, value);
    CHECK_ARG(env, data);
    CHECK_ARG(env, length);

    *data = nullptr;
    *length = 0;

    // Local handles are transient (the result is bytes, not a JS value), so a
    // plain scope contains them — nothing escapes to the caller's scope.
    v8::HandleScope handle_scope(env->isolate);
    v8::Local<v8::Context> context = env->context();
    v8::Context::Scope context_scope(context);

    v8::Local<v8::Value> v8_value = v8impl::V8LocalValueFromJsValue(value);

    // Default delegate (single-arg ctor): host objects / SAB / functions /
    // symbols throw a DataClone exception, caught by NAPI_PREAMBLE's TryCatch.
    v8::ValueSerializer serializer(env->isolate);
    serializer.WriteHeader();
    if (serializer.WriteValue(context, v8_value).FromMaybe(false)) {
        std::pair<uint8_t *, size_t> buf = serializer.Release();
        *data = buf.first;
        *length = buf.second;
    }
    // On WriteValue failure the DataClone exception is pending -> GET_RETURN_STATUS
    // returns napi_pending_exception and *data / *length stay null/0.
    return GET_RETURN_STATUS(env);
}

napi_status NAPI_CDECL napi_v8_free_serialized_data(napi_env env, uint8_t *data) {
    CHECK_ENV_NOT_IN_GC(env);
    // V8's default ValueSerializer delegate allocates the Release() buffer with the
    // malloc family, so free() pairs with it. Freeing inside the library (rather
    // than letting the caller free()) keeps allocation/free on one heap — required
    // for cross-DLL/CRT correctness on Windows.
    if (data != nullptr)
        ::free(data);
    return napi_clear_last_error(env);
}

napi_status NAPI_CDECL napi_v8_deserialize(napi_env env, const uint8_t *data,
                                           size_t length, napi_value *result) {
    NAPI_PREAMBLE(env);
    CHECK_ARG(env, data);
    CHECK_ARG(env, result);

    *result = nullptr;

    // The reconstructed value graph must outlive this call, so escape its root to
    // the caller's scope; intermediate handles stay contained here.
    v8::EscapableHandleScope handle_scope(env->isolate);
    v8::Local<v8::Context> context = env->context();
    v8::Context::Scope context_scope(context);

    v8::ValueDeserializer deserializer(env->isolate, data, length);
    v8::Local<v8::Value> v8_result;
    {
        // Malformed / truncated / wrong-version bytes make V8 throw an internal
        // "unable to deserialize" error. Plain deserialization (no Delegate) runs
        // no user JS, so any throw here is V8's bad-data signal — contain it and
        // surface a clean napi_invalid_arg rather than leaking a pending exception.
        v8::TryCatch inner(env->isolate);
        bool ok = deserializer.ReadHeader(context).FromMaybe(false) &&
                  deserializer.ReadValue(context).ToLocal(&v8_result);
        if (!ok) {
            inner.Reset();
            return napi_set_last_error(env, napi_invalid_arg);
        }
    }
    *result = v8impl::JsValueFromV8LocalValue(handle_scope.Escape(v8_result));
    return GET_RETURN_STATUS(env);
}

// ---------------------------------------------------------------------------
// ArrayBuffer backing-store transfer (zero-copy) — see serialize.h.
// Backed by V8's isolate-independent BackingStore (shared_ptr): the source AB is
// detached but the store survives (our ref keeps it alive), then adopted into a
// new AB in the same or another isolate — no byte copy.

struct napi_v8_backing_store__ {
    std::shared_ptr<v8::BackingStore> store;
};

napi_status NAPI_CDECL napi_v8_take_backing_store(napi_env env, napi_value arraybuffer,
                                                  napi_v8_backing_store *out) {
    CHECK_ENV_NOT_IN_GC(env);
    CHECK_ARG(env, arraybuffer);
    CHECK_ARG(env, out);
    *out = nullptr;

    v8::Local<v8::Value> value = v8impl::V8LocalValueFromJsValue(arraybuffer);
    // A SharedArrayBuffer is not an ArrayBuffer (IsArrayBuffer==false) and is
    // shared, not transferred — it falls out here as napi_arraybuffer_expected.
    RETURN_STATUS_IF_FALSE(env, value->IsArrayBuffer(), napi_arraybuffer_expected);

    v8::Local<v8::ArrayBuffer> arr = value.As<v8::ArrayBuffer>();
    // Reject non-detachable (WASM memory) AND already-detached buffers: the latter
    // has no bytes to take, yet V8's IsDetachable() can still report true for it.
    RETURN_STATUS_IF_FALSE(env, arr->IsDetachable() && !arr->WasDetached(),
                           napi_detachable_arraybuffer_expected);

    // Grab a strong ref to the backing store BEFORE detaching: Detach() drops the
    // AB's own ref, but ours keeps the bytes alive for adoption elsewhere.
    std::shared_ptr<v8::BackingStore> bs = arr->GetBackingStore();
    if (arr->Detach(v8::Local<v8::Value>()).IsNothing())
        return napi_set_last_error(env, napi_detachable_arraybuffer_expected);

    *out = new napi_v8_backing_store__{std::move(bs)};
    return napi_clear_last_error(env);
}

napi_status NAPI_CDECL napi_v8_adopt_backing_store(napi_env env, napi_v8_backing_store store,
                                                   napi_value *out_arraybuffer) {
    NAPI_PREAMBLE(env);
    CHECK_ARG(env, out_arraybuffer);
    RETURN_STATUS_IF_FALSE(env, store != nullptr && store->store, napi_invalid_arg);

    // V8 takes its own ref to the shared backing store (zero copy — same bytes).
    v8::Local<v8::ArrayBuffer> ab = v8::ArrayBuffer::New(env->isolate, store->store);
    *out_arraybuffer = v8impl::JsValueFromV8LocalValue(ab);
    delete store;  // consumed: releases our ref; V8 now owns a ref, bytes stay alive
    return GET_RETURN_STATUS(env);
}

napi_status NAPI_CDECL napi_v8_free_backing_store(napi_v8_backing_store store) {
    // env-less: a backing store is isolate-independent. delete of NULL is a no-op;
    // otherwise the shared_ptr releases, freeing the bytes when it was the last ref.
    delete store;
    return napi_ok;
}
