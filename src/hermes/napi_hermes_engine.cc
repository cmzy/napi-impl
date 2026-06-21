// Embedding entry for the Hermes backend: implements the project-wide
// embedding C ABI (napi_create_platform / napi_create_runtime /
// napi_create_env + the per-tick hook) declared in napi_v8/embedding.h, on top
// of Hermes's Node-API (hermes::node_api::getOrCreateNodeApiEnvironment) and the
// Hermes VM (hermes::vm::Runtime).
//
// Deliberately does NOT use Hermes JSI (API/hermes) or the Chrome inspector
// (API/hermes_shared/inspector) — the latter does not build outside Windows and
// debugging is out of scope. We talk to the VM and Node-API layers directly so
// the resulting libnapi_hermes is a drop-in ABI swap for libnapi_v8: the host
// uses the identical embedding API + standard napi_* surface across engines.

#include <functional>
#include <memory>
#include <mutex>
#include <unordered_map>

#include "hermes/BCGen/HBC/HBC.h"        // hermes::hbc::CompileFlags
#include "hermes/Public/RuntimeConfig.h" // hermes::vm::RuntimeConfig
#include "hermes/VM/Runtime.h"           // hermes::vm::Runtime, ExecutionStatus
#include "hermes_node_api.h"             // hermes::node_api::{getOrCreateNodeApiEnvironment, openNodeApiScope, closeNodeApiScope}

#include "napi_v8/embedding.h"           // our cross-engine embedding C ABI

namespace {

// The embedding API has no per-env handle to carry engine state, so map the
// opaque napi_env back to the runtime that owns it (and the scope we keep open
// for the env's lifetime). One env per runtime in this embedding.
struct EnvState {
    napi_runtime runtime = nullptr;
    void *scope = nullptr;
};

std::mutex g_envs_mu;
std::unordered_map<napi_env, EnvState> &EnvMap() {
    static std::unordered_map<napi_env, EnvState> m;
    return m;
}

} // namespace

// ---- opaque embedding types ----------------------------------------------

struct napi_platform__ {
    napi_error_message_handler err_handler = nullptr;
    bool exit_on_unhandled_error = false;
};

struct napi_runtime__ {
    std::shared_ptr<hermes::vm::Runtime> runtime;
    napi_platform platform = nullptr;
    napi_env env = nullptr; // root node-api env (owned by the VM runtime)
    void *env_scope = nullptr;
};

// ---- platform -------------------------------------------------------------

napi_status NAPI_CDECL napi_create_platform(int /*argc*/,
                                            char ** /*argv*/,
                                            int /*exec_argc*/,
                                            char ** /*exec_argv*/,
                                            napi_error_message_handler err_handler,
                                            bool exit_on_unhandled_error,
                                            napi_platform *result) {
    // Hermes has no global platform/flag parser comparable to V8; argv/exec_argv
    // are accepted for ABI parity and ignored.
    if (result == nullptr)
        return napi_invalid_arg;
    auto *p = new napi_platform__();
    p->err_handler = err_handler;
    p->exit_on_unhandled_error = exit_on_unhandled_error;
    *result = p;
    return napi_ok;
}

napi_status NAPI_CDECL napi_destroy_platform(napi_platform platform) {
    if (platform == nullptr)
        return napi_invalid_arg;
    delete platform;
    return napi_ok;
}

// ---- runtime --------------------------------------------------------------

napi_status NAPI_CDECL napi_create_runtime(napi_platform platform, napi_runtime *result) {
    if (platform == nullptr || result == nullptr)
        return napi_invalid_arg;
    auto config = hermes::vm::RuntimeConfig::Builder().build();
    auto *r = new napi_runtime__();
    r->platform = platform;
    r->runtime = hermes::vm::Runtime::create(config);
    if (!r->runtime) {
        delete r;
        return napi_generic_failure;
    }
    *result = r;
    return napi_ok;
}

napi_status NAPI_CDECL napi_destroy_runtime(napi_runtime runtime) {
    if (runtime == nullptr)
        return napi_invalid_arg;
    if (runtime->env != nullptr) {
        if (runtime->env_scope != nullptr)
            hermes::node_api::closeNodeApiScope(runtime->env, runtime->env_scope);
        {
            std::lock_guard<std::mutex> lk(g_envs_mu);
            EnvMap().erase(runtime->env);
        }
    }
    runtime->runtime.reset(); // tears down the VM and its node-api env
    delete runtime;
    return napi_ok;
}

// ---- env ------------------------------------------------------------------

napi_status NAPI_CDECL napi_create_env(napi_runtime runtime, napi_env *result) {
    if (runtime == nullptr || result == nullptr)
        return napi_invalid_arg;
    if (runtime->env == nullptr) {
        hermes::hbc::CompileFlags compileFlags; // defaults (lazy compilation)
        napi_platform plat = runtime->platform;
        std::function<void(napi_env, napi_value)> unhandled =
                [plat](napi_env /*env*/, napi_value /*error*/) {
                    if (plat != nullptr && plat->err_handler != nullptr)
                        plat->err_handler("Unhandled JavaScript error");
                };
        hermes::vm::CallResult<napi_env> envRes = hermes::node_api::getOrCreateNodeApiEnvironment(
                *runtime->runtime,
                compileFlags,
                /*taskRunner*/ nullptr,
                unhandled,
                NAPI_VERSION);
        if (envRes.getStatus() == hermes::vm::ExecutionStatus::EXCEPTION)
            return napi_generic_failure;
        runtime->env = *envRes;
        napi_status s = hermes::node_api::openNodeApiScope(runtime->env, &runtime->env_scope);
        if (s != napi_ok)
            return s;
        std::lock_guard<std::mutex> lk(g_envs_mu);
        EnvMap()[runtime->env] = EnvState{runtime, runtime->env_scope};
    }
    *result = runtime->env;
    return napi_ok;
}

napi_status NAPI_CDECL napi_destroy_env(napi_env env) {
    if (env == nullptr)
        return napi_invalid_arg;
    napi_runtime owner = nullptr;
    void *scope = nullptr;
    {
        std::lock_guard<std::mutex> lk(g_envs_mu);
        auto &m = EnvMap();
        auto it = m.find(env);
        if (it == m.end())
            return napi_invalid_arg;
        owner = it->second.runtime;
        scope = it->second.scope;
        m.erase(it);
    }
    if (scope != nullptr)
        hermes::node_api::closeNodeApiScope(env, scope);
    if (owner != nullptr) {
        owner->env = nullptr;
        owner->env_scope = nullptr;
    }
    return napi_ok;
}

// ---- per-tick hook --------------------------------------------------------

napi_status NAPI_CDECL napi_v8_run_event_loop_tasks(napi_env env) {
    if (env == nullptr)
        return napi_invalid_arg;
    napi_runtime owner = nullptr;
    {
        std::lock_guard<std::mutex> lk(g_envs_mu);
        auto it = EnvMap().find(env);
        if (it != EnvMap().end())
            owner = it->second.runtime;
    }
    if (owner != nullptr && owner->runtime)
        owner->runtime->drainJobs(); // run pending microtasks (Promise jobs)
    return napi_ok;
}
