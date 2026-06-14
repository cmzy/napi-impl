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

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // NAPI_V8_INSPECTOR_H_
