// napi/fast_call.h — engine-neutral fast-call surface (project-specific).
//
// Lets an embedder expose a native method as a V8 "fast call" (v8::CFunction)
// while staying engine-agnostic: the SAME source compiles and runs on backends
// without fast calls (Hermes/JSC/QuickJS), where these symbols degrade to the
// ordinary slow path. This header carries NO v8 types — the fast callback's
// receiver / object args / options are opaque pointer-sized handles, read via
// the fast-safe helpers below, so the caller never #includes a v8 header.
//
// Design & rationale: see FAST_CALL_PLAN.md. Key facts baked in here:
//   * V8 14.2 has no per-call fallback (FastApiCallbackOptions has no `fallback`
//     field). "Is this method fast?" is a REGISTRATION-time decision: pass
//     sig=NULL/fast_fn=NULL for slow-only. A fast callback must be total for the
//     inputs its signature admits (it cannot bail to slow mid-call).
//   * TypedArray/ArrayBuffer are not first-class fast arg types in this V8; pass
//     them as napi_fast_value and read bytes via napi_fast_get_buffersource.
//
// All symbols are exported by every backend (non-V8 = slow-equivalent fallback),
// so switching engines needs no source change. Backends that actually take a
// fast path define NAPI_HAS_FAST_CALL=1 (compile-time gate is optional).

#ifndef NAPI_FAST_CALL_H_
#define NAPI_FAST_CALL_H_

#include "napi/js_native_api.h"
#include "napi/js_native_api_types.h"

#ifdef __cplusplus
extern "C" {
#endif

// --- Opaque handles seen inside a fast callback ----------------------------
// ABI-compatible with the engine's native representation; treat as tokens and
// pass to the helpers below. On V8: napi_fast_recv≡Local<Object>,
// napi_fast_value≡Local<Value>, napi_fast_options≡FastApiCallbackOptions&.
typedef struct napi_fast_recv__* napi_fast_recv;
typedef struct napi_fast_value__* napi_fast_value;
typedef struct napi_fast_options__* napi_fast_options;

// --- Fast arg/return type tags (map 1:1 to V8 CTypeInfo::Type) --------------
// Object / TypedArray / generic JS value all use napi_fast_value (=kV8Value);
// the distinction is purely which helper the caller invokes on it.
typedef enum {
    napi_fast_void = 0,
    napi_fast_bool,
    napi_fast_int32,
    napi_fast_uint32,
    napi_fast_int64,   // IDL (unsigned) long long truncation semantics
    napi_fast_uint64,
    napi_fast_float32,
    napi_fast_float64,
    napi_fast_pointer,   // raw void* (advanced)
    napi_fast_receiver,  // JS `this`; conventionally arg 0 (maps to kV8Value)
    napi_fast_jsvalue,   // object / TypedArray / ArrayBuffer / generic (kV8Value)
} napi_fast_type;

// One fast signature. arg_types[0] is conventionally the receiver. The trailing
// options arg is NOT listed here — set wants_options to request it.
typedef struct {
    napi_fast_type return_type;
    uint32_t arg_count;             // includes receiver, excludes options
    const napi_fast_type* arg_types;  // length == arg_count
    bool wants_options;             // true => fast cb gets a trailing napi_fast_options
} napi_fast_signature;

// One overload variant (resolved by V8 on argument count only).
typedef struct {
    napi_fast_signature sig;
    const void* fast_fn;
} napi_fast_overload;

// Element kind reported by napi_fast_get_buffersource.
typedef enum {
    napi_fast_bs_unknown = 0,
    napi_fast_bs_i8,
    napi_fast_bs_u8,
    napi_fast_bs_u8c,
    napi_fast_bs_i16,
    napi_fast_bs_u16,
    napi_fast_bs_i32,
    napi_fast_bs_u32,
    napi_fast_bs_f32,
    napi_fast_bs_f64,
    napi_fast_bs_i64,
    napi_fast_bs_u64,
    napi_fast_bs_arraybuffer,
} napi_fast_bs_type;

// --- Registration -----------------------------------------------------------

// Create a JS function exposing a fast C entry plus a slow napi fallback.
//   slow_cb : REQUIRED. The standard napi callback — sole path on non-fast
//             engines and the path V8 takes whenever it does not (or cannot)
//             use the fast path. Its ctx is recovered from the `this_arg` of
//             napi_get_cb_info via napi_unwrap (same pointer the fast path reads).
//   sig     : fast signature; NULL => slow-only.
//   fast_fn : pointer to the caller's fast C function whose signature matches
//             `sig` (see FAST_CALL_PLAN.md §5); NULL => slow-only.
//   data    : bound data; slow_cb reads it via napi_get_cb_info, the fast fn via
//             napi_fast_options_get_data (only if sig.wants_options).
// When sig/fast_fn is NULL or the backend has no fast support, the result is
// observably equivalent to napi_create_function(env,name,len,slow_cb,data,...).
NAPI_EXTERN napi_status NAPI_CDECL napi_create_fast_function(napi_env env,
                                                            const char* utf8name,
                                                            size_t length,
                                                            napi_callback slow_cb,
                                                            const napi_fast_signature* sig,
                                                            const void* fast_fn,
                                                            void* data,
                                                            napi_value* result);

// As above, but with multiple fast overloads (V8 resolves by argument count).
// A single slow_cb backs all of them. overload_count==0 => slow-only.
NAPI_EXTERN napi_status NAPI_CDECL napi_create_fast_function_overloads(napi_env env,
                                                                      const char* utf8name,
                                                                      size_t length,
                                                                      napi_callback slow_cb,
                                                                      const napi_fast_overload* overloads,
                                                                      size_t overload_count,
                                                                      void* data,
                                                                      napi_value* result);

// --- Wrap / unwrap (fast-safe native pointer storage) -----------------------

// Like napi_wrap, but ALSO stores `native` in a fast-readable slot (V8 internal
// field 0) so napi_fast_unwrap can retrieve it inside a fast callback. The JS
// object must be an instance created with a reserved internal field (every
// napi_define_class instance reserves one). napi_unwrap keeps working (slow
// path). If the object has no internal field, only the slow wrap is recorded
// and napi_fast_unwrap will return NULL. On non-fast engines: identical to
// napi_wrap.
// `native` must be at least 2-byte aligned (V8 stores it as an aligned pointer
// and uses the low bit); pointers from new/malloc always satisfy this.
NAPI_EXTERN napi_status NAPI_CDECL napi_fast_wrap(napi_env env,
                                                 napi_value js_object,
                                                 void* native,
                                                 napi_finalize finalize_cb,
                                                 void* finalize_hint,
                                                 napi_ref* result);

// Recover the native ctx from the receiver inside a fast callback (O(1) read of
// internal field 0). Returns NULL if the receiver carries no fast slot.
NAPI_EXTERN void* NAPI_CDECL napi_fast_unwrap(napi_fast_recv recv);

// Recover the native pointer behind an object argument inside a fast callback.
// Returns NULL for null/undefined or a value with no fast slot.
NAPI_EXTERN void* NAPI_CDECL napi_fast_value_unwrap(napi_fast_value v);

// True iff the value is null or undefined (fast-safe).
NAPI_EXTERN bool NAPI_CDECL napi_fast_value_is_nullish(napi_fast_value v);

// Expose the bytes of a TypedArray / ArrayBuffer / DataView to a fast callback.
// Off-heap backing stores are returned zero-copy; small on-heap typed arrays are
// copied into the caller-provided `scratch`. For a TypedArray/DataView, `scratch`
// MUST be non-NULL and at least 64 bytes (V8's typed_array_max_size_in_heap) —
// V8 copies the whole on-heap view into it, so a smaller buffer would overflow;
// an undersized/NULL scratch makes this return false. out_elem (may be NULL)
// receives the element kind. Returns false if `v` is not a buffer source.
// Fast-safe (no JS, no allocation that triggers GC).
NAPI_EXTERN bool NAPI_CDECL napi_fast_get_buffersource(napi_fast_value v,
                                                      void* scratch,
                                                      size_t scratch_len,
                                                      void** out_data,
                                                      size_t* out_byte_length,
                                                      napi_fast_bs_type* out_elem);

// --- Options (only delivered when sig.wants_options) ------------------------
// Returns the `data` bound at napi_create_fast_function. Fast-safe.
// Note: V8 14.2 has no per-call fallback (no set_fallback is offered).
NAPI_EXTERN void* NAPI_CDECL napi_fast_options_get_data(napi_fast_options opts);

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // NAPI_FAST_CALL_H_
