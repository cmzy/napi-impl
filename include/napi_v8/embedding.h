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

// Single "event-loop tick" hook. This embedding has no libuv loop, so the host
// must call this once per event-loop iteration at a safe point (outside GC, when
// calling into JS is OK). It does the per-tick work Node would do automatically:
//   - pumps the V8 foreground task runner (runs FinalizationRegistry cleanup
//     callbacks and other scheduled foreground tasks);
//   - drains the deferred (second-pass) napi finalizer queue (frees napi_wrap'd
//     natives whose JS wrappers were collected).
// Without it, FinalizationRegistry callbacks never fire and every napi_wrap'd
// object is finalized only at env teardown.
NAPI_EXTERN napi_status NAPI_CDECL napi_v8_run_event_loop_tasks(napi_env env);

// Interrupt JavaScript execution currently running on `env`'s isolate.
//
// Requests V8 to terminate whatever JS is executing (v8::Isolate::TerminateExecution):
// a non-catchable termination unwinds the stack, and V8's interrupt flag is checked at
// stack-guard points (function entry, loop back-edges) so even an unbounded loop such as
// `while (true) {}` is interrupted. **Thread-safe**: may be called from a thread other
// than the one running the isolate — the intended use is a host tearing down a worker to
// break it out of a runaway script so the worker thread can be joined (otherwise a
// cooperative-only shutdown hangs forever). After termination fires, the isolate is left
// in the "terminating" state; the host typically proceeds to destroy the env/runtime.
// (If the isolate is idle, this arms the flag so the next JS entry terminates immediately.)
NAPI_EXTERN napi_status NAPI_CDECL napi_v8_terminate_execution(napi_env env);

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // NAPI_V8_EMBEDDING_H_
