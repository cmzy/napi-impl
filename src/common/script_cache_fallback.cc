// Engine-agnostic fallback for napi/script_cache.h on backends WITHOUT a real
// compilation cache. Mirrors fast_call_fallback.cc: this TU lives in the shared
// `common` source list (also compiled by the V8/GN build), so the ENTIRE file is
// empty when NAPI_HAS_SCRIPT_CACHE is defined — the real impl then lives in the
// backend (src/v8/script_cache.cc; a Hermes HBC impl opts in the same way). On
// backends that do NOT define the macro (JSC / QuickJS) it emits the default:
// compile + run the source normally, ignore any provided cache, and produce none.

#if !defined(NAPI_HAS_SCRIPT_CACHE)

#define NAPI_EXPERIMENTAL
#include "napi/js_native_api.h"
#include "napi/node_api.h"

#include "napi/script_cache.h"

#include <cstdlib>  // std::free

napi_status NAPI_CDECL napi_run_script_cached(napi_env env, napi_value script, const char* origin, size_t origin_len,
                                              const uint8_t* cached_data, size_t cached_len, bool* cache_rejected,
                                              uint8_t** out_cached_data, size_t* out_cached_len, napi_value* result) {
    (void)origin;
    (void)origin_len;
    (void)cached_len;
    // No cache on this engine: a provided blob is "rejected" (we recompile from
    // source), and none is produced.
    if (cache_rejected != nullptr)
        *cache_rejected = (cached_data != nullptr);
    if (out_cached_data != nullptr) {
        *out_cached_data = nullptr;
        if (out_cached_len != nullptr)
            *out_cached_len = 0;
    }
    // napi_run_script requires a non-null result out-param; use a local if the
    // caller does not want the value.
    napi_value local = nullptr;
    napi_status s = napi_run_script(env, script, result != nullptr ? result : &local);
    return s;
}

napi_status NAPI_CDECL napi_free_script_cache(napi_env env, uint8_t* cached_data) {
    (void)env;
    // This backend never produces a blob, but honour the contract (free if given).
    if (cached_data != nullptr)
        std::free(cached_data);
    return napi_ok;
}

#endif  // !NAPI_HAS_SCRIPT_CACHE
