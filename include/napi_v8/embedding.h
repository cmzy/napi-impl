// napi_v8/embedding.h
//
// Embedding API for napi-impl's V8 backend. Lets the host create a V8 platform,
// isolate ("runtime"), and a napi_env that wraps a context. After this you use
// the standard Node-API surface from "napi/js_native_api.h".
//
// This API is project-specific (no equivalent header exists in upstream
// Node.js or node-api-headers). Other backends (hermes/jsc/quickjs) will
// expose their own engine-specific embedding entry points; the standard
// napi_* surface stays identical across engines.

#ifndef NAPI_V8_EMBEDDING_H_
#define NAPI_V8_EMBEDDING_H_

#include "napi/js_native_api.h"
#include "napi/js_native_api_types.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct napi_platform__* napi_platform;
typedef struct napi_runtime__* napi_runtime;

typedef void(NAPI_CDECL* napi_error_message_handler)(const char* message);

NAPI_EXTERN napi_status NAPI_CDECL
napi_create_platform(int argc,
                     char** argv,
                     int exec_argc,
                     char** exec_argv,
                     napi_error_message_handler err_handler,
                     bool exit_on_unhandled_error,
                     napi_platform* result);

NAPI_EXTERN napi_status NAPI_CDECL napi_destroy_platform(napi_platform platform);

NAPI_EXTERN napi_status NAPI_CDECL napi_create_runtime(napi_platform platform,
                                                       napi_runtime* result);

NAPI_EXTERN napi_status NAPI_CDECL napi_destroy_runtime(napi_runtime runtime);

NAPI_EXTERN napi_status NAPI_CDECL napi_create_env(napi_runtime runtime,
                                                   napi_env* result);

NAPI_EXTERN napi_status NAPI_CDECL napi_destroy_env(napi_env env);

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // NAPI_V8_EMBEDDING_H_
