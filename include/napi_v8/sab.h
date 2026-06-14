// napi_v8/sab.h — SharedArrayBuffer extensions (project-specific).
//
// Node-API itself has no SAB-aware surface (Node treats SAB as a plain Object
// and napi_is_arraybuffer returns false for it). For embedders that need
// zero-copy SAB across the C ABI we expose three explicit calls backed by
// v8::SharedArrayBuffer.
//
// All functions live in the napi_v8_* namespace to avoid colliding with any
// future upstream additions.

#ifndef NAPI_V8_SAB_H_
#define NAPI_V8_SAB_H_

#include "napi/js_native_api.h"
#include "napi/js_native_api_types.h"

#ifdef __cplusplus
extern "C" {
#endif

// Allocate a new zero-initialized SharedArrayBuffer of `byte_length` bytes.
// If `data` is non-NULL, receives a pointer to the SAB's backing memory.
NAPI_EXTERN napi_status NAPI_CDECL
napi_v8_create_shared_arraybuffer(napi_env env,
                                  size_t byte_length,
                                  void** data,
                                  napi_value* result);

// True iff `value` is a SharedArrayBuffer.
NAPI_EXTERN napi_status NAPI_CDECL
napi_v8_is_shared_arraybuffer(napi_env env, napi_value value, bool* result);

// Read the backing pointer / size of a SharedArrayBuffer. Returns
// napi_arraybuffer_expected if `sab` is not a SharedArrayBuffer.
// Either `data` or `byte_length` may be NULL.
NAPI_EXTERN napi_status NAPI_CDECL
napi_v8_get_shared_arraybuffer_info(napi_env env,
                                    napi_value sab,
                                    void** data,
                                    size_t* byte_length);

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // NAPI_V8_SAB_H_
