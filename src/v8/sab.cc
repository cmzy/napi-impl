// SharedArrayBuffer entry points (core node-api, experimental):
// node_api_create_sharedarraybuffer / _is_sharedarraybuffer /
// _create_external_sharedarraybuffer. Reading a SAB's backing pointer/size is
// done via napi_get_arraybuffer_info, which this implementation extends to
// accept a SAB (see src/v8/arraybuffer.cc).

#define NAPI_EXPERIMENTAL
#include "napi/js_native_api.h"
#include "js_native_api_v8.h"
#include "js_native_api_v8_impl.h"

#include <memory>

#include "v8-array-buffer.h"

napi_status NAPI_CDECL node_api_create_sharedarraybuffer(napi_env env,
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

napi_status NAPI_CDECL node_api_is_sharedarraybuffer(napi_env env,
                                                    napi_value value,
                                                    bool *result) {
    CHECK_ENV_NOT_IN_GC(env);
    CHECK_ARG(env, value);
    CHECK_ARG(env, result);

    *result = v8impl::V8LocalValueFromJsValue(value)->IsSharedArrayBuffer();
    return napi_clear_last_error(env);
}

napi_status NAPI_CDECL node_api_create_external_sharedarraybuffer(napi_env env, void *external_data,
                                                                  size_t byte_length,
                                                                  node_api_noenv_finalize finalize_cb,
                                                                  void *finalize_hint, napi_value *result) {
    // Wrap caller-owned memory as a SharedArrayBuffer; the finalizer (no env, per
    // node_api_noenv_finalize) runs from V8's BackingStore deleter on free.
    NAPI_PREAMBLE(env);
    CHECK_ARG(env, result);

    struct FinalizerBox {
        node_api_noenv_finalize cb;
        void *hint;
    };
    FinalizerBox *box = (finalize_cb != nullptr) ? new FinalizerBox{finalize_cb, finalize_hint} : nullptr;

    auto deleter = +[](void *data, size_t /*len*/, void *deleter_data) {
        if (auto *b = static_cast<FinalizerBox *>(deleter_data)) {
            b->cb(data, b->hint);
            delete b;
        }
    };
    std::unique_ptr<v8::BackingStore> store =
            v8::SharedArrayBuffer::NewBackingStore(external_data, byte_length, deleter, box);
    v8::Local<v8::SharedArrayBuffer> sab = v8::SharedArrayBuffer::New(env->isolate, std::move(store));
    if (sab.IsEmpty()) {
        delete box;
        return napi_set_last_error(env, napi_generic_failure);
    }
    *result = v8impl::JsValueFromV8LocalValue(sab);
    return GET_RETURN_STATUS(env);
}
