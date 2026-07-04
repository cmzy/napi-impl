// napi/script_cache.h — engine-neutral compilation-cache surface (project extension).
//
// Runs a JS source string while optionally consuming and/or producing an
// engine-specific compilation cache, so an embedder can skip parse/compile on
// repeated runs of the same script (engine startup, worker prelude, …). Same
// engine-agnostic shape as napi/fast_call.h: the SAME source compiles on every
// backend; real caching on V8 (ScriptCompiler code cache) and Hermes (HBC
// bytecode); JSC and QuickJS fall back to a plain compile+run (no cache).
//
// The cache blob is OPAQUE and ENGINE-SPECIFIC — a V8 blob cannot be consumed by
// Hermes and vice versa. Store and reuse it per engine (an embedder builds with
// one engine at a time). A blob produced by an incompatible engine/version is
// rejected: the engine silently recompiles from source and sets *cache_rejected.
//
// Backends that actually cache define NAPI_HAS_SCRIPT_CACHE=1 (optional gate).

#ifndef NAPI_SCRIPT_CACHE_H_
#define NAPI_SCRIPT_CACHE_H_

#include "napi/js_native_api.h"
#include "napi/js_native_api_types.h"

#ifdef __cplusplus
extern "C" {
#endif

// Compile + run `script` (a JS String value), optionally with a compilation cache.
//
//  script         : a napi_value String — the source. (napi_string_expected otherwise.)
//  origin/origin_len : script name for cache keying / stack traces; may be NULL/0.
//  cached_data/len : a prior cache blob for THIS engine+script, or NULL. On a
//                    mismatch (wrong engine/version/source) the engine silently
//                    recompiles from source; *cache_rejected (if non-NULL) is set true.
//  cache_rejected  : out (nullable) — whether a provided cache was rejected.
//  out_cached_data : out (nullable) — if non-NULL, receives a freshly produced
//                    cache blob of `*out_cached_len` bytes, owned by the caller
//                    (release with napi_free_script_cache). Backends without a
//                    cache leave it NULL/0. Pass NULL to not produce.
//  result          : out (nullable) — the script's completion value.
//
// Returns napi_string_expected if `script` is not a String; napi_pending_exception
// if compilation/execution throws (with the exception set); napi_ok otherwise.
NAPI_EXTERN napi_status NAPI_CDECL
napi_run_script_cached(napi_env env, napi_value script,
                       const char* origin, size_t origin_len,
                       const uint8_t* cached_data, size_t cached_len,
                       bool* cache_rejected,
                       uint8_t** out_cached_data, size_t* out_cached_len,
                       napi_value* result);

// Free a cache blob returned via napi_run_script_cached's out_cached_data. The
// allocator lives inside the library (cross-DLL/CRT correctness); passing NULL is
// a no-op.
NAPI_EXTERN napi_status NAPI_CDECL
napi_free_script_cache(napi_env env, uint8_t* cached_data);

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // NAPI_SCRIPT_CACHE_H_
