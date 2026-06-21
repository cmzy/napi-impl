// Glue between the V8 inspector and our WebSocket server.
//
// Threading model (see PLAN / inspector.h): the WebSocket transport runs on its
// own thread and only ferries bytes. EVERY V8 inspector call
// (dispatchProtocolMessage, the V8Inspector::Channel callbacks, the pause loop)
// happens on the isolate-owning ("V8") thread. Incoming CDP messages are queued
// by the transport thread and drained on the V8 thread via PumpQueued().

#ifndef SRC_V8_INSPECTOR_BRIDGE_H_
#define SRC_V8_INSPECTOR_BRIDGE_H_

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <deque>
#include <memory>
#include <mutex>
#include <string>

#include "v8-context.h"
#include "v8-inspector.h"
#include "v8-isolate.h"
#include "v8-local-handle.h"

#include "napi_v8/inspector.h" // napi_env + pause/wake handler typedefs

#include "server.h"

namespace napi_v8 {
    namespace inspector {

        class InspectorBridge final : public v8_inspector::V8InspectorClient {
        public:
            InspectorBridge(napi_env env, v8::Isolate *iso, v8::Local<v8::Context> ctx,
                            const std::string &context_name);
            ~InspectorBridge() override;

            bool Start(int port);
            void Stop();
            void WaitForConnection();

            // Drain & dispatch all queued CDP messages on the V8 thread. Non-blocking.
            // Returns the number dispatched; also stored in *out if non-null.
            size_t PumpQueued(size_t *out);

            // Block up to timeout_ms (<0 = until activity) for an incoming message or
            // a pause-state change, then return. Call on the V8 thread.
            void Wait(int timeout_ms);

            bool IsPaused() const { return paused_.load(); }

            void SetPauseHandler(napi_v8_inspector_pause_handler cb, void *data) {
                pause_handler_ = cb;
                pause_data_ = data;
            }
            void SetWakeHandler(napi_v8_inspector_wake_handler cb, void *data) {
                wake_data_.store(data);
                wake_handler_.store(cb);
            }

            // V8InspectorClient overrides ----------------------------------------------
            v8::Local<v8::Context> ensureDefaultContextInGroup(int group_id) override;
            void runMessageLoopOnPause(int contextGroupId) override;
            void quitMessageLoopOnPause() override;

        private:
            // Enqueue a raw CDP message from the transport thread, wake the V8 thread.
            void EnqueueMessage(const std::string &msg);

            // RequestInterrupt trampoline — runs on the V8 thread, drains the queue.
            static void InterruptCb(v8::Isolate *iso, void *data);

            napi_env env_;
            v8::Isolate *isolate_;
            v8::Global<v8::Context> context_;
            std::string context_name_;
            std::unique_ptr<v8_inspector::V8Inspector> inspector_;
            std::shared_ptr<v8_inspector::V8InspectorSession> session_;
            InspectorServer server_;

            // Incoming CDP messages: produced on the transport thread, consumed on the
            // V8 thread. in_cv_ is signalled on enqueue and on pause-state change.
            std::mutex in_mu_;
            std::condition_variable in_cv_;
            std::deque<std::string> in_q_;

            std::atomic<bool> paused_{false};
            std::atomic<bool> interrupt_scheduled_{false};

            // Pause handler: V8 thread only.
            napi_v8_inspector_pause_handler pause_handler_ = nullptr;
            void *pause_data_ = nullptr;
            // Wake handler: read on the transport thread, set on the V8 thread.
            std::atomic<napi_v8_inspector_wake_handler> wake_handler_{nullptr};
            std::atomic<void *> wake_data_{nullptr};

            class ChannelImpl;
            std::unique_ptr<ChannelImpl> channel_;
        };

    } // namespace inspector
} // namespace napi_v8

#endif // SRC_V8_INSPECTOR_BRIDGE_H_
