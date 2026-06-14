// Glue between the V8 inspector and our WebSocket server.

#ifndef SRC_V8_INSPECTOR_BRIDGE_H_
#define SRC_V8_INSPECTOR_BRIDGE_H_

#include <memory>
#include <string>

#include "v8-inspector.h"
#include "v8-isolate.h"
#include "v8-local-handle.h"
#include "v8-context.h"

#include "server.h"

namespace napi_v8 {
namespace inspector {

class InspectorBridge final : public v8_inspector::V8InspectorClient {
 public:
  InspectorBridge(v8::Isolate* iso, v8::Local<v8::Context> ctx,
                  const std::string& context_name);
  ~InspectorBridge() override;

  bool Start(int port);
  void Stop();
  void WaitForConnection();

  // Hand a raw CDP message off to V8. Must be called on the V8 thread.
  void DispatchProtocolMessage(const std::string& msg);

  // V8InspectorClient overrides ----------------------------------------------
  v8::Local<v8::Context> ensureDefaultContextInGroup(int group_id) override;
  void runMessageLoopOnPause(int contextGroupId) override;
  void quitMessageLoopOnPause() override;

 private:
  v8::Isolate* isolate_;
  v8::Global<v8::Context> context_;
  std::string context_name_;
  std::unique_ptr<v8_inspector::V8Inspector> inspector_;
  std::shared_ptr<v8_inspector::V8InspectorSession> session_;
  InspectorServer server_;
  bool running_loop_ = false;

  class ChannelImpl;
  std::unique_ptr<ChannelImpl> channel_;
};

}  // namespace inspector
}  // namespace napi_v8

#endif  // SRC_V8_INSPECTOR_BRIDGE_H_
