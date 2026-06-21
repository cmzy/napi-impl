// V8 Inspector — exposes Chrome DevTools Protocol over a WebSocket bound to
// a TCP port on localhost. Connect from chrome://inspect or any CDP client.
//
// HTTP/WebSocket handshake is parsed via llhttp (https://llhttp.org).
//
// This API is project-specific; there is no upstream equivalent.

#ifndef NAPI_V8_INSPECTOR_H_
#define NAPI_V8_INSPECTOR_H_

#include "napi/js_native_api.h"
#include "napi/js_native_api_types.h"

#ifdef __cplusplus
extern "C" {
#endif

// Start the inspector listening on `port` (loopback). `context_name` shows up
// in the CDP target list (chrome://inspect). Returns napi_ok once the listener
// is bound; the accept loop runs on its own thread.
NAPI_EXTERN napi_status NAPI_CDECL
napi_v8_inspector_start(napi_env env, int port, const char* context_name);

// Block until the first client connects (handy for tests that want to attach a
// debugger before they begin executing).
NAPI_EXTERN napi_status NAPI_CDECL
napi_v8_inspector_wait_for_connection(napi_env env);

// Shut down the listener and disconnect the current client (if any).
NAPI_EXTERN napi_status NAPI_CDECL napi_v8_inspector_stop(napi_env env);

// ---------------------------------------------------------------------------
// Message loop. The V8 inspector is single-threaded: every CDP message must be
// dispatched on the thread that owns the isolate (the thread you call napi_*
// on). Socket I/O runs on an internal transport thread that only ferries bytes;
// the functions below are what actually drive the inspector on YOUR thread.
//
// Threading contract: call every function in this header EXCEPT
// napi_v8_inspector_set_wake_handler's callback on the isolate-owning thread.
// ---------------------------------------------------------------------------

// Drain and dispatch every CDP message the transport thread has queued since
// the last call, on the calling (isolate-owning) thread. Non-blocking.
// `out_dispatched` (nullable) receives the number of messages processed.
//
// You normally do NOT need to call this: while an inspector is active,
// napi_v8_run_event_loop_tasks() drains the queue for you each tick. It is
// exposed for custom pause handlers (see napi_v8_inspector_set_pause_handler),
// which must pump while the host event-loop tick is not running.
NAPI_EXTERN napi_status NAPI_CDECL
napi_v8_inspector_pump_messages(napi_env env, size_t* out_dispatched);

// True while execution is paused at a breakpoint / `debugger;` / step. Set on
// the isolate-owning thread inside the pause loop.
NAPI_EXTERN bool NAPI_CDECL napi_v8_inspector_is_paused(napi_env env);

// Block up to `timeout_ms` (negative = until activity) until a CDP message
// arrives or the pause state changes, then return so the caller can pump.
// Intended for use inside a custom pause handler to avoid busy-spinning.
NAPI_EXTERN napi_status NAPI_CDECL
napi_v8_inspector_wait(napi_env env, int timeout_ms);

// Handler invoked on the isolate-owning thread when execution pauses. When set,
// the host MUST run a loop that returns only once
// napi_v8_inspector_is_paused(env) == false, each iteration calling
// napi_v8_inspector_wait() then napi_v8_inspector_pump_messages(). When NOT set,
// the library runs an internal blocking pump loop until the client resumes.
typedef void(NAPI_CDECL* napi_v8_inspector_pause_handler)(napi_env env,
                                                          void* data);
NAPI_EXTERN napi_status NAPI_CDECL napi_v8_inspector_set_pause_handler(
    napi_env env, napi_v8_inspector_pause_handler handler, void* data);

// Handler invoked (possibly on the transport thread) when a CDP message
// arrives, so the host can wake a blocked event loop — e.g. write to a
// self-pipe or post to its UI thread. It MUST be cheap, thread-safe, and MUST
// NOT call any napi_* function. Pass handler == NULL to clear.
typedef void(NAPI_CDECL* napi_v8_inspector_wake_handler)(void* data);
NAPI_EXTERN napi_status NAPI_CDECL napi_v8_inspector_set_wake_handler(
    napi_env env, napi_v8_inspector_wake_handler handler, void* data);

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // NAPI_V8_INSPECTOR_H_
