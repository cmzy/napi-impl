#include "bridge.h"

#include <chrono>
#include <thread>

#include "v8-isolate.h"
#include "v8-locker.h"

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

        InspectorBridge::InspectorBridge(v8::Isolate *iso, v8::Local<v8::Context> ctx, const std::string &name) :
            isolate_(iso), context_name_(name) {
            context_.Reset(iso, ctx);
            inspector_ = v8_inspector::V8Inspector::create(iso, this);
            channel_ = std::make_unique<ChannelImpl>(this);

            // Register the context with the inspector.
            v8_inspector::V8ContextInfo info(ctx, /*contextGroupId=*/1, Sv(name));
            inspector_->contextCreated(info);
        }

        InspectorBridge::~InspectorBridge() {
            Stop();
            if (inspector_)
                inspector_->contextDestroyed(context_.Get(isolate_));
        }

        bool InspectorBridge::Start(int port) {
            server_.SetOnMessage([this](const std::string &msg) { DispatchProtocolMessage(msg); });
            if (!server_.Start(port, context_name_))
                return false;
            // Connect a session lazily once the client connects: we open the V8
            // session right away so any incoming message has somewhere to land.
            session_ = inspector_->connectShared(
                    /*contextGroupId=*/1, channel_.get(), Sv(std::string()), v8_inspector::V8Inspector::kFullyTrusted,
                    v8_inspector::V8Inspector::kNotWaitingForDebugger);
            return true;
        }

        void InspectorBridge::Stop() {
            server_.Stop();
            session_.reset();
        }

        void InspectorBridge::WaitForConnection() { server_.WaitForConnection(); }

        void InspectorBridge::DispatchProtocolMessage(const std::string &msg) {
            if (!session_)
                return;
            // dispatchProtocolMessage uses the isolate; acquire it from the WS thread.
            v8::Locker locker(isolate_);
            v8::Isolate::Scope iscope(isolate_);
            v8::HandleScope hscope(isolate_);
            v8::Context::Scope cscope(context_.Get(isolate_));
            session_->dispatchProtocolMessage(Sv(msg));
        }

        v8::Local<v8::Context> InspectorBridge::ensureDefaultContextInGroup(int) { return context_.Get(isolate_); }

        void InspectorBridge::runMessageLoopOnPause(int) {
            running_loop_ = true;
            while (running_loop_) {
                // Spin-yield: inspector messages arrive on the WS thread and call
                // DispatchProtocolMessage which then may call quitMessageLoopOnPause.
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
        }

        void InspectorBridge::quitMessageLoopOnPause() { running_loop_ = false; }

    } // namespace inspector
} // namespace napi_v8
