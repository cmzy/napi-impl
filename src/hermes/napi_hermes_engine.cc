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

#include <atomic>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <unordered_map>

#include "hermes/BCGen/HBC/HBC.h"        // hermes::hbc::CompileFlags
#include "hermes/Public/RuntimeConfig.h" // hermes::vm::RuntimeConfig
#include "hermes/VM/Runtime.h"           // hermes::vm::Runtime, ExecutionStatus
#include "hermes_node_api.h"             // hermes::node_api::{getOrCreateNodeApiEnvironment, openNodeApiScope, closeNodeApiScope, Task, TaskRunner}

#include "napi_v8/embedding.h"           // our cross-engine embedding C ABI

namespace {

// Node-API version this embedding advertises (returned by napi_get_version).
// Matches the node-api-headers we vendor (v1.9.0 / Node 22) and the upstream
// js-native-api suite's expectation. Note 8<->9 cross no Hermes behavior gate
// (those are at apiVersion >= 10 and == NAPI_VERSION_EXPERIMENTAL), so this only
// affects the reported version, not API semantics.
constexpr int32_t kNodeApiVersion = 9;

// Hermes' Node-API defers second-pass (post-GC) finalizers by posting a task to
// the env's TaskRunner — and NodeApiEnvironment::enqueueFinalizer dereferences
// it unconditionally, so a null runner segfaults the first time a napi_wrap'd
// external is collected. We supply a minimal runner that queues posted tasks
// and runs them when the host pumps the event loop (napi_v8_run_event_loop_tasks).
class EmbedTaskRunner final : public hermes::node_api::TaskRunner {
public:
    void post(std::unique_ptr<hermes::node_api::Task> task) noexcept override {
        // During runtime teardown, ~vm::Runtime finalizes externals and posts
        // their second-pass finalizers here. A queued task captures a counted
        // ref to the node-api env, so it would outlive vm::Runtime; when our
        // task runner is later destroyed, dropping that ref runs the env's
        // deleteMe() against an already-freed runtime — a use-after-free. Run
        // teardown tasks inline instead (this mirrors Hermes' own
        // isShuttingDown_ path, which finalizes immediately).
        if (shutting_down_.load(std::memory_order_acquire)) {
            task->invoke();
            return;
        }
        std::lock_guard<std::mutex> lk(mu_);
        queue_.push_back(std::move(task));
    }

    // Run all queued tasks, including any they enqueue transitively.
    void drain() noexcept {
        for (;;) {
            std::unique_ptr<hermes::node_api::Task> task;
            {
                std::lock_guard<std::mutex> lk(mu_);
                if (queue_.empty())
                    break;
                task = std::move(queue_.front());
                queue_.pop_front();
            }
            task->invoke();
        }
    }

    // After this, post() runs tasks inline (called just before vm::Runtime is
    // destroyed). One-way latch.
    void beginShutdown() noexcept {
        shutting_down_.store(true, std::memory_order_release);
    }

private:
    std::mutex mu_;
    std::deque<std::unique_ptr<hermes::node_api::Task>> queue_;
    std::atomic<bool> shutting_down_{false};
};

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
    std::shared_ptr<EmbedTaskRunner> task_runner;
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
    r->task_runner = std::make_shared<EmbedTaskRunner>();
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
    if (runtime->task_runner) {
        runtime->task_runner->drain();         // flush already-queued finalizers
        runtime->task_runner->beginShutdown(); // teardown-posted finalizers run inline
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
                [plat](napi_env env, napi_value error) {
                    // An unhandled JS error (e.g. thrown from a napi finalizer)
                    // reaches here with the exception still pending on the env, so
                    // we must clear it before we can call back into JS (the value
                    // is already captured as `error`). Surface it to a host
                    // dispatcher if installed — globalThis.__emitUncaughtException,
                    // matching Node's process 'uncaughtException' — else fall back
                    // to the platform string handler.
                    if (env != nullptr && error != nullptr) {
                        napi_value pending = nullptr;
                        napi_get_and_clear_last_exception(env, &pending);
                        napi_value global = nullptr, emit = nullptr;
                        napi_valuetype t = napi_undefined;
                        if (napi_get_global(env, &global) == napi_ok &&
                            napi_get_named_property(env, global,
                                                    "__emitUncaughtException", &emit) == napi_ok &&
                            napi_typeof(env, emit, &t) == napi_ok && t == napi_function) {
                            napi_value undef = nullptr, res = nullptr, argv[1] = {error};
                            napi_get_undefined(env, &undef);
                            if (napi_call_function(env, undef, emit, 1, argv, &res) == napi_ok)
                                return;
                            // The dispatcher itself threw; clear and fall through.
                            napi_get_and_clear_last_exception(env, &pending);
                        }
                    }
                    if (plat != nullptr && plat->err_handler != nullptr)
                        plat->err_handler("Unhandled JavaScript error");
                };
        hermes::vm::CallResult<napi_env> envRes = hermes::node_api::getOrCreateNodeApiEnvironment(
                *runtime->runtime,
                compileFlags,
                runtime->task_runner,
                unhandled,
                kNodeApiVersion);
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
    if (owner != nullptr && owner->runtime) {
        owner->runtime->drainJobs(); // run pending microtasks (Promise jobs)
        if (owner->task_runner)
            owner->task_runner->drain(); // run deferred second-pass finalizers
    }
    return napi_ok;
}
