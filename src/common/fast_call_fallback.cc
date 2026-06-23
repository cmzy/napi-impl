// Engine-agnostic fallback for napi/fast_call.h on backends WITHOUT V8 fast
// calls (Hermes / JSC / QuickJS). See FAST_CALL_PLAN.md §6.
//
// This file lives in the shared `common` source list, which the V8 (GN) build
// also compiles. To avoid duplicate symbols with the real implementation
// (src/v8/fast_call.cc), the ENTIRE translation unit is empty when
// NAPI_HAS_FAST_CALL is defined — i.e. it only emits these symbols on the
// non-V8 (CMake) track, whose napi_flags does not define that macro.
//
// Behaviour is observably slow-path-equivalent: napi_create_fast_function*
// ignores the fast descriptor and builds a plain napi_create_function;
// napi_fast_wrap == napi_wrap; the fast-only read helpers are never invoked on
// these engines (no fast path) and return benign values.

#if !defined(NAPI_HAS_FAST_CALL)

#define NAPI_EXPERIMENTAL
#include "napi/js_native_api.h"
#include "napi/node_api.h"

#include "napi/fast_call.h"

napi_status NAPI_CDECL napi_create_fast_function(napi_env env, const char* utf8name, size_t length,
                                                 napi_callback slow_cb, const napi_fast_signature* sig,
                                                 const void* fast_fn, void* data, napi_value* result) {
    (void)sig;
    (void)fast_fn;
    return napi_create_function(env, utf8name, length, slow_cb, data, result);
}

napi_status NAPI_CDECL napi_create_fast_function_overloads(napi_env env, const char* utf8name, size_t length,
                                                           napi_callback slow_cb,
                                                           const napi_fast_overload* overloads,
                                                           size_t overload_count, void* data, napi_value* result) {
    (void)overloads;
    (void)overload_count;
    return napi_create_function(env, utf8name, length, slow_cb, data, result);
}

napi_status NAPI_CDECL napi_fast_wrap(napi_env env, napi_value js_object, void* native, napi_finalize finalize_cb,
                                      void* finalize_hint, napi_ref* result) {
    return napi_wrap(env, js_object, native, reinterpret_cast<node_api_basic_finalize>(finalize_cb), finalize_hint,
                     result);
}

void* NAPI_CDECL napi_fast_unwrap(napi_fast_recv recv) {
    (void)recv;
    return nullptr;
}

void* NAPI_CDECL napi_fast_value_unwrap(napi_fast_value value) {
    (void)value;
    return nullptr;
}

bool NAPI_CDECL napi_fast_value_is_nullish(napi_fast_value value) {
    (void)value;
    return false;
}

bool NAPI_CDECL napi_fast_get_buffersource(napi_fast_value value, void* scratch, size_t scratch_len, void** out_data,
                                           size_t* out_byte_length, napi_fast_bs_type* out_elem) {
    (void)value;
    (void)scratch;
    (void)scratch_len;
    (void)out_data;
    (void)out_byte_length;
    (void)out_elem;
    return false;
}

void* NAPI_CDECL napi_fast_options_get_data(napi_fast_options opts) {
    (void)opts;
    return nullptr;
}

#endif // !NAPI_HAS_FAST_CALL
