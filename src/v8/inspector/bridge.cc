#include "bridge.h"

#include <chrono>
#include <mutex>
#include <unordered_set>

#include "v8-isolate.h"

namespace napi_v8 {
    namespace inspector {

        using v8_inspector::StringBuffer;
        using v8_inspector::StringView;

        namespace {

            StringView Sv(const std::string &s) {
                return StringView(reinterpret_cast<const uint8_t *>(s.data()), s.size());
            }

            std::string Sv2str(const StringView &v) {
                if (v.is8Bit())
                    return std::string(reinterpret_cast<const char *>(v.characters8()), v.length());
                // 16-bit -> UTF-8 (best effort, ASCII-friendly).
                std::string out;
                out.reserve(v.length());
                for (size_t i = 0; i < v.length(); ++i) {
                    uint16_t c = v.characters16()[i];
                    if (c < 0x80)
                        out.push_back(static_cast<char>(c));
                    else if (c < 0x800) {
                        out.push_back(static_cast<char>(0xC0 | (c >> 6)));
                        out.push_back(static_cast<char>(0x80 | (c & 0x3F)));
                    } else {
                        out.push_back(static_cast<char>(0xE0 | (c >> 12)));
                        out.push_back(static_cast<char>(0x80 | ((c >> 6) & 0x3F)));
                        out.push_back(static_cast<char>(0x80 | (c & 0x3F)));
                    }
                }
                return out;
            }

            // Liveness registry: a RequestInterrupt scheduled before Stop() can still
            // fire (on the V8 thread) after the bridge is deleted. The interrupt only
            // proceeds if its bridge pointer is still registered here. Construct/destruct
            // and the interrupt callback all run on the V8 thread, so this guards stale
            // pointers across teardown, not concurrent access.
            std::mutex g_live_mu;
            std::unordered_set<InspectorBridge *> g_live;

        } // namespace

        class InspectorBridge::ChannelImpl final : public v8_inspector::V8Inspector::Channel {
        public:
            explicit ChannelImpl(InspectorBridge *br) : br_(br) {}

            void sendResponse(int /*callId*/, std::unique_ptr<StringBuffer> message) override {
                br_->server_.Send(Sv2str(message->string()));
            }
            void sendNotification(std::unique_ptr<StringBuffer> message) override {
                br_->server_.Send(Sv2str(message->string()));
            }
            void flushProtocolNotifications() override {}

        private:
            InspectorBridge *br_;
        };

        InspectorBridge::InspectorBridge(napi_env env, v8::Isolate *iso, v8::Local<v8::Context> ctx,
                                         const std::string &name) :
            env_(env), isolate_(iso), context_name_(name) {
            {
                std::lock_guard<std::mutex> g(g_live_mu);
                g_live.insert(this);
            }
            context_.Reset(iso, ctx);
            inspector_ = v8_inspector::V8Inspector::create(iso, this);
            channel_ = std::make_unique<ChannelImpl>(this);

            // Register the context with the inspector.
            v8_inspector::V8ContextInfo info(ctx, /*contextGroupId=*/1, Sv(name));
            inspector_->contextCreated(info);
        }

        InspectorBridge::~InspectorBridge() {
            {
                std::lock_guard<std::mutex> g(g_live_mu);
                g_live.erase(this);
            }
            Stop();
            if (inspector_) {
                // contextDestroyed materializes the context Local, so a HandleScope
                // must be active. napi_v8_inspector_stop() runs after the host has
                // closed its own handle scope; the isolate is still entered (the env
                // is destroyed later), so open a scope here.
                v8::HandleScope hs(isolate_);
                inspector_->contextDestroyed(context_.Get(isolate_));
            }
        }

        bool InspectorBridge::Start(int port) {
            // Transport thread only queues bytes; it never touches V8.
            server_.SetOnMessage([this](const std::string &msg) { EnqueueMessage(msg); });
            if (!server_.Start(port, context_name_))
                return false;
            // Open the V8 session up front (on the V8 thread) so any incoming message
            // has somewhere to land.
            session_ = inspector_->connectShared(
                    /*contextGroupId=*/1, channel_.get(), Sv(std::string()), v8_inspector::V8Inspector::kFullyTrusted,
                    v8_inspector::V8Inspector::kNotWaitingForDebugger);
            return true;
        }

        void InspectorBridge::Stop() {
            server_.Stop();
            session_.reset();
            // Release any V8-thread waiter (Wait / internal pause loop).
            {
                std::lock_guard<std::mutex> g(in_mu_);
                paused_.store(false);
            }
            in_cv_.notify_all();
        }

        void InspectorBridge::WaitForConnection() { server_.WaitForConnection(); }

        // ---- Transport thread -> V8 thread plumbing -------------------------------

        void InspectorBridge::EnqueueMessage(const std::string &msg) {
            {
                std::lock_guard<std::mutex> g(in_mu_);
                in_q_.push_back(msg);
            }
            in_cv_.notify_all();
            // Wake a host event loop that may be blocked on I/O.
            if (auto wake = wake_handler_.load())
                wake(wake_data_.load());
            // Nudge the V8 thread so long-running JS yields to a newly-set breakpoint
            // or pause request. Coalesce to a single pending interrupt.
            if (!interrupt_scheduled_.exchange(true))
                isolate_->RequestInterrupt(&InterruptCb, this);
        }

        void InspectorBridge::InterruptCb(v8::Isolate * /*iso*/, void *data) {
            // Runs on the V8 thread. Validate the bridge is still alive (a stale
            // interrupt may fire after Stop()/delete).
            {
                std::lock_guard<std::mutex> g(g_live_mu);
                if (g_live.find(static_cast<InspectorBridge *>(data)) == g_live.end())
                    return;
            }
            auto *self = static_cast<InspectorBridge *>(data);
            self->interrupt_scheduled_.store(false);
            self->PumpQueued(nullptr);
        }

        // ---- V8 thread: dispatch & message loop -----------------------------------

        size_t InspectorBridge::PumpQueued(size_t *out) {
            size_t n = 0;
            for (;;) {
                std::string msg;
                {
                    std::lock_guard<std::mutex> g(in_mu_);
                    if (in_q_.empty())
                        break;
                    msg = std::move(in_q_.front());
                    in_q_.pop_front();
                }
                // We are the isolate-owning thread: no v8::Locker, just scopes.
                if (session_) {
                    v8::HandleScope hscope(isolate_);
                    v8::Context::Scope cscope(context_.Get(isolate_));
                    session_->dispatchProtocolMessage(Sv(msg));
                }
                ++n;
            }
            if (out)
                *out = n;
            return n;
        }

        void InspectorBridge::Wait(int timeout_ms) {
            std::unique_lock<std::mutex> g(in_mu_);
            if (!in_q_.empty())
                return;
            auto ready = [this] { return !in_q_.empty() || !paused_.load(); };
            if (timeout_ms < 0)
                in_cv_.wait(g, ready);
            else
                in_cv_.wait_for(g, std::chrono::milliseconds(timeout_ms), ready);
        }

        v8::Local<v8::Context> InspectorBridge::ensureDefaultContextInGroup(int) { return context_.Get(isolate_); }

        void InspectorBridge::runMessageLoopOnPause(int) {
            paused_.store(true);
            if (pause_handler_ != nullptr) {
                // Host owns the nested loop; it must return only once IsPaused()==false.
                pause_handler_(env_, pause_data_);
            } else {
                // Internal default: block-and-pump until the client resumes. We pump
                // only inspector messages here (not the general foreground task runner)
                // so paused execution does not run unrelated JS — matching Node.js.
                while (paused_.load()) {
                    PumpQueued(nullptr);
                    if (!paused_.load())
                        break;
                    std::unique_lock<std::mutex> g(in_mu_);
                    in_cv_.wait_for(g, std::chrono::milliseconds(20),
                                    [this] { return !in_q_.empty() || !paused_.load(); });
                }
            }
            paused_.store(false);
        }

        void InspectorBridge::quitMessageLoopOnPause() {
            paused_.store(false);
            in_cv_.notify_all();
        }

    } // namespace inspector
} // namespace napi_v8
