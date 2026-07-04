// napi_v8/serialize.h — structured-clone serialization via V8's ValueSerializer.
//
// Exposes V8's native structured-clone wire format (the same one postMessage /
// structuredClone use internally) as three C entry points, so an embedder can
// serialize a JS value to a portable byte buffer and reconstruct it in the same
// OR a different env/isolate (worker postMessage — the wire format is
// isolate-independent).
//
// Scope (v1, plain — no serializer Delegate): V8 built-in types only —
// primitives, String, BigInt, Boolean/Number/Date/RegExp wrappers, Array,
// Object, Map, Set, Error, ArrayBuffer and TypedArray/DataView (copied by value),
// and cyclic graphs. Values V8 cannot clone on its own — functions, symbols,
// SharedArrayBuffer, and host/API objects (anything requiring a Delegate) — throw
// a DataClone exception (the call returns napi_pending_exception). A
// Delegate-based variant (host objects + ArrayBuffer transfer + SAB) is a future
// addition and would be a SEPARATE symbol; this v1 API is unaffected by it.

#ifndef NAPI_V8_SERIALIZE_H_
#define NAPI_V8_SERIALIZE_H_

#include "napi/js_native_api.h"
#include "napi/js_native_api_types.h"

#ifdef __cplusplus
extern "C" {
#endif

// Serialize `value` into a freshly-allocated byte buffer (V8 structured-clone
// wire format). On napi_ok, `*data` receives a buffer of `*length` bytes owned by
// the caller — it MUST be released with napi_v8_free_serialized_data (do not
// free() it yourself: the allocator lives inside the napi_v8 library, and freeing
// across a DLL/CRT boundary corrupts the heap, notably on Windows). On any
// un-cloneable value a DataClone exception is thrown and the call returns
// napi_pending_exception with `*data` / `*length` left untouched.
NAPI_EXTERN napi_status NAPI_CDECL
napi_v8_serialize(napi_env env, napi_value value,
                  uint8_t** data, size_t* length);

// Free a buffer returned by napi_v8_serialize. Passing NULL is a no-op.
NAPI_EXTERN napi_status NAPI_CDECL
napi_v8_free_serialized_data(napi_env env, uint8_t* data);

// Deserialize `length` bytes (produced by napi_v8_serialize, possibly in another
// env/isolate) into a JS value in `env`. On malformed/truncated input returns
// napi_invalid_arg (no value produced); a throwing path during deserialization
// surfaces as napi_pending_exception. The bytes are not retained — the caller
// still owns `data`.
NAPI_EXTERN napi_status NAPI_CDECL
napi_v8_deserialize(napi_env env, const uint8_t* data, size_t length,
                    napi_value* result);

// ---------------------------------------------------------------------------
// ArrayBuffer backing-store transfer (zero-copy)
//
// The structured-clone `transfer` half: move an ArrayBuffer's bytes from one
// env/isolate to another WITHOUT copying (postMessage/worker transfer). Backed by
// V8's isolate-independent BackingStore (shared_ptr) — the same mechanism Chrome
// and Node's worker_threads use. Unlike napi_v8_serialize (which copies
// ArrayBuffers by value), this hands off ownership of the raw bytes.
//
// NOTE (V8-only): this is a V8-specific capability. JavaScriptCore pins a buffer
// once its bytes pointer is read (so transfer() degrades to a copy) and Hermes
// ties ArrayBuffer memory to its runtime — neither can move a backing store to
// another isolate/runtime. Embedders targeting those engines keep copying.

// Opaque owned backing store, moved out of an ArrayBuffer. Adopt it into a new
// ArrayBuffer (same or another env/isolate — the store is isolate-independent)
// with napi_v8_adopt_backing_store, or release it with napi_v8_free_backing_store.
typedef struct napi_v8_backing_store__* napi_v8_backing_store;

// Take ownership of `arraybuffer`'s backing store WITHOUT copying and DETACH the
// source ArrayBuffer (transfer semantics — the source becomes zero-length/detached
// in its env; any TypedArray views over it become empty). On napi_ok, `*out`
// receives an owned handle. A non-ArrayBuffer — including a SharedArrayBuffer,
// which is shared rather than transferred — returns napi_arraybuffer_expected; a
// non-detachable ArrayBuffer (already detached, or WASM memory) returns
// napi_detachable_arraybuffer_expected.
NAPI_EXTERN napi_status NAPI_CDECL
napi_v8_take_backing_store(napi_env env, napi_value arraybuffer,
                           napi_v8_backing_store* out);

// Create a new ArrayBuffer in `env` that adopts `store` (zero copy — same bytes).
// On napi_ok the store is CONSUMED (do not free it afterwards); on error the
// caller still owns `store` and must free it. Passing a NULL store returns
// napi_invalid_arg.
NAPI_EXTERN napi_status NAPI_CDECL
napi_v8_adopt_backing_store(napi_env env, napi_v8_backing_store store,
                            napi_value* out_arraybuffer);

// Release a backing store that was NOT consumed by napi_v8_adopt_backing_store
// (frees the bytes when this is the last reference). Passing NULL is a no-op.
// env-less: a backing store is isolate-independent, so this needs no env.
NAPI_EXTERN napi_status NAPI_CDECL
napi_v8_free_backing_store(napi_v8_backing_store store);

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // NAPI_V8_SERIALIZE_H_
