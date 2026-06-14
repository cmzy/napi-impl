// Public C entry points declared in napi_v8/inspector.h, backed by
// InspectorBridge.

#define NAPI_EXPERIMENTAL
#include "js_native_api_v8.h"
#include "napi/js_native_api.h"

#if !defined(_WIN32)
#include "bridge.h"
#endif

#include <map>
#include <mutex>

extern "C" {
#include "napi_v8/inspector.h"
}

#if defined(_WIN32)

// Windows: POSIX socket inspector not ported; expose stubs that report failure.
extern "C" napi_status NAPI_CDECL napi_v8_inspector_start(napi_env, int, const char *) {
    return napi_generic_failure;
}
extern "C" napi_status NAPI_CDECL napi_v8_inspector_wait_for_connection(napi_env) {
    return napi_generic_failure;
}
extern "C" napi_status NAPI_CDECL napi_v8_inspector_stop(napi_env) { return napi_ok; }

#else

namespace {

    std::mutex g_mu;
    std::map<napi_env, napi_v8::inspector::InspectorBridge *> g_bridges;

    napi_v8::inspector::InspectorBridge *GetOrCreate(napi_env env, const char *name) {
        std::lock_guard<std::mutex> g(g_mu);
        auto it = g_bridges.find(env);
        if (it != g_bridges.end())
            return it->second;
        auto *br = new napi_v8::inspector::InspectorBridge(env->isolate, env->context(),
                                                           name ? std::string(name) : std::string("napi_v8"));
        g_bridges[env] = br;
        return br;
    }

} // namespace

extern "C" napi_status NAPI_CDECL napi_v8_inspector_start(napi_env env, int port, const char *context_name) {
    if (env == nullptr || port <= 0 || port > 65535)
        return napi_invalid_arg;
    auto *br = GetOrCreate(env, context_name);
    return br->Start(port) ? napi_ok : napi_generic_failure;
}

extern "C" napi_status NAPI_CDECL napi_v8_inspector_wait_for_connection(napi_env env) {
    if (env == nullptr)
        return napi_invalid_arg;
    std::lock_guard<std::mutex> g(g_mu);
    auto it = g_bridges.find(env);
    if (it == g_bridges.end())
        return napi_invalid_arg;
    it->second->WaitForConnection();
    return napi_ok;
}

extern "C" napi_status NAPI_CDECL napi_v8_inspector_stop(napi_env env) {
    if (env == nullptr)
        return napi_invalid_arg;
    napi_v8::inspector::InspectorBridge *br = nullptr;
    {
        std::lock_guard<std::mutex> g(g_mu);
        auto it = g_bridges.find(env);
        if (it == g_bridges.end())
            return napi_ok;
        br = it->second;
        g_bridges.erase(it);
    }
    br->Stop();
    delete br;
    return napi_ok;
}

#endif  // !defined(_WIN32)
