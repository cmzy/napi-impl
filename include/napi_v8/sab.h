// napi_v8/sab.h — SharedArrayBuffer (migration note; no declarations).
//
// The SharedArrayBuffer surface is now part of core node-api (experimental);
// there are no `napi_v8_*` SAB functions anymore. From "napi/js_native_api.h"
// (define NAPI_EXPERIMENTAL):
//   node_api_create_sharedarraybuffer(env, byte_length, data, result)
//   node_api_create_external_sharedarraybuffer(env, data, len, finalize, hint, result)
//   node_api_is_sharedarraybuffer(env, value, result)
//
// To read a SAB's backing pointer/size, napi_get_arraybuffer_info also accepts a
// SharedArrayBuffer in this implementation — a non-standard, lenient superset:
// upstream Node returns napi_invalid_arg for a SAB there. (napi_is_arraybuffer
// still reports false for a SAB, matching upstream.) This replaces the former
// napi_v8_get_shared_arraybuffer_info.
//
// The header is retained (declaration-free) so the packaged umbrella headers and
// the extension-convention layout stay stable.

#ifndef NAPI_V8_SAB_H_
#define NAPI_V8_SAB_H_

#endif  // NAPI_V8_SAB_H_
