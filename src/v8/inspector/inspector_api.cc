// Public C entry points declared in napi_v8/inspector.h, backed by
// InspectorBridge.

#define NAPI_EXPERIMENTAL
#include "js_native_api_v8.h"
#include "napi/js_native_api.h"

#if !defined(_WIN32)
#include "bridge.h"
#endif

#include <cstddef>
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
extern "C" napi_status NAPI_CDECL napi_v8_inspector_pump_messages(napi_env, size_t *out_dispatched) {
    if (out_dispatched != nullptr)
        *out_dispatched = 0;
    return napi_generic_failure;
}
extern "C" bool NAPI_CDECL napi_v8_inspector_is_paused(napi_env) { return false; }
extern "C" napi_status NAPI_CDECL napi_v8_inspector_wait(napi_env, int) { return napi_generic_failure; }
extern "C" napi_status NAPI_CDECL napi_v8_inspector_set_pause_handler(napi_env, napi_v8_inspector_pause_handler,
                                                                      void *) {
    return napi_generic_failure;
}
extern "C" napi_status NAPI_CDECL napi_v8_inspector_set_wake_handler(napi_env, napi_v8_inspector_wake_handler, void *) {
    return napi_generic_failure;
}

#else

namespace {

    std::mutex g_mu;
    std::map<napi_env, napi_v8::inspector::InspectorBridge *> g_bridges;

    // Engine tick hook (registered via napi_v8_priv::g_inspector_tick_hook): drain
    // this env's queued CDP messages on the V8 thread each event-loop tick. Look up
    // the bridge under the lock, then release BEFORE pumping — dispatch may run a
    // nested pause loop and we must not hold g_mu across it.
    void InspectorTickHook(napi_env env) {
        napi_v8::inspector::InspectorBridge *br = nullptr;
        {
            std::lock_guard<std::mutex> g(g_mu);
            auto it = g_bridges.find(env);
            if (it != g_bridges.end())
                br = it->second;
        }
        if (br != nullptr)
            br->PumpQueued(nullptr);
    }

    napi_v8::inspector::InspectorBridge *GetOrCreate(napi_env env, const char *name) {
        std::lock_guard<std::mutex> g(g_mu);
        auto it = g_bridges.find(env);
        if (it != g_bridges.end())
            return it->second;
        auto *br = new napi_v8::inspector::InspectorBridge(env, env->isolate, env->context(),
                                                           name ? std::string(name) : std::string("napi_v8"));
        g_bridges[env] = br;
        // Fold inspector draining into the host's event-loop tick.
        napi_v8_priv::g_inspector_tick_hook = &InspectorTickHook;
        return br;
    }

    napi_v8::inspector::InspectorBridge *Lookup(napi_env env) {
        std::lock_guard<std::mutex> g(g_mu);
        auto it = g_bridges.find(env);
        return it == g_bridges.end() ? nullptr : it->second;
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
    auto *br = Lookup(env);
    if (br == nullptr)
        return napi_invalid_arg;
    br->WaitForConnection();
    return napi_ok;
}

extern "C" napi_status NAPI_CDECL napi_v8_inspector_pump_messages(napi_env env, size_t *out_dispatched) {
    if (out_dispatched != nullptr)
        *out_dispatched = 0;
    if (env == nullptr)
        return napi_invalid_arg;
    auto *br = Lookup(env);
    if (br == nullptr)
        return napi_invalid_arg;
    br->PumpQueued(out_dispatched);
    return napi_ok;
}

extern "C" bool NAPI_CDECL napi_v8_inspector_is_paused(napi_env env) {
    if (env == nullptr)
        return false;
    auto *br = Lookup(env);
    return br != nullptr && br->IsPaused();
}

extern "C" napi_status NAPI_CDECL napi_v8_inspector_wait(napi_env env, int timeout_ms) {
    if (env == nullptr)
        return napi_invalid_arg;
    auto *br = Lookup(env);
    if (br == nullptr)
        return napi_invalid_arg;
    br->Wait(timeout_ms);
    return napi_ok;
}

extern "C" napi_status NAPI_CDECL napi_v8_inspector_set_pause_handler(napi_env env,
                                                                      napi_v8_inspector_pause_handler handler,
                                                                      void *data) {
    if (env == nullptr)
        return napi_invalid_arg;
    auto *br = Lookup(env);
    if (br == nullptr)
        return napi_invalid_arg;
    br->SetPauseHandler(handler, data);
    return napi_ok;
}

extern "C" napi_status NAPI_CDECL napi_v8_inspector_set_wake_handler(napi_env env,
                                                                     napi_v8_inspector_wake_handler handler,
                                                                     void *data) {
    if (env == nullptr)
        return napi_invalid_arg;
    auto *br = Lookup(env);
    if (br == nullptr)
        return napi_invalid_arg;
    br->SetWakeHandler(handler, data);
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
        if (g_bridges.empty())
            napi_v8_priv::g_inspector_tick_hook = nullptr;
    }
    br->Stop();
    delete br;
    return napi_ok;
}

#endif  // !defined(_WIN32)
