// SharedArrayBuffer extension entry points (napi_v8_*).

#define NAPI_EXPERIMENTAL
#include "napi/js_native_api.h"
#include "js_native_api_v8.h"
#include "js_native_api_v8_impl.h"

#include "v8-array-buffer.h"

extern "C" {
#include "napi_v8/sab.h"
}

napi_status NAPI_CDECL napi_v8_create_shared_arraybuffer(napi_env env,
                                                        size_t byte_length,
                                                        void **data,
                                                        napi_value *result) {
    NAPI_PREAMBLE(env);
    CHECK_ARG(env, result);

    v8::Local<v8::SharedArrayBuffer> sab =
        v8::SharedArrayBuffer::New(env->isolate, byte_length);
    if (sab.IsEmpty())
        return napi_set_last_error(env, napi_generic_failure);

    if (data != nullptr)
        *data = sab->Data();
    *result = v8impl::JsValueFromV8LocalValue(sab);
    return GET_RETURN_STATUS(env);
}

napi_status NAPI_CDECL napi_v8_is_shared_arraybuffer(napi_env env,
                                                    napi_value value,
                                                    bool *result) {
    CHECK_ENV_NOT_IN_GC(env);
    CHECK_ARG(env, value);
    CHECK_ARG(env, result);

    *result = v8impl::V8LocalValueFromJsValue(value)->IsSharedArrayBuffer();
    return napi_clear_last_error(env);
}

napi_status NAPI_CDECL napi_v8_get_shared_arraybuffer_info(napi_env env,
                                                          napi_value sab,
                                                          void **data,
                                                          size_t *byte_length) {
    CHECK_ENV_NOT_IN_GC(env);
    CHECK_ARG(env, sab);

    v8::Local<v8::Value> v = v8impl::V8LocalValueFromJsValue(sab);
    RETURN_STATUS_IF_FALSE(env, v->IsSharedArrayBuffer(),
                           napi_arraybuffer_expected);

    v8::Local<v8::SharedArrayBuffer> as = v.As<v8::SharedArrayBuffer>();
    if (data != nullptr)
        *data = as->Data();
    if (byte_length != nullptr)
        *byte_length = as->ByteLength();
    return napi_clear_last_error(env);
}
