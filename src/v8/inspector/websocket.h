// Minimal RFC 6455 WebSocket framing for the inspector. Server-side only:
// we expect client frames to be masked and we send server frames unmasked.

#ifndef SRC_V8_INSPECTOR_WEBSOCKET_H_
#define SRC_V8_INSPECTOR_WEBSOCKET_H_

#include <cstdint>
#include <string>
#include <vector>

namespace napi_v8 {
    namespace inspector {

        // Compute Sec-WebSocket-Accept = base64(SHA1(key + GUID)).
        std::string AcceptKey(const std::string &client_key);

        enum class WsOp : uint8_t {
            kContinuation = 0x0,
            kText = 0x1,
            kBinary = 0x2,
            kClose = 0x8,
            kPing = 0x9,
            kPong = 0xA,
        };

        struct WsFrame {
            bool fin = true;
            WsOp op = WsOp::kText;
            std::string payload;
        };

        // Streaming decoder. Push bytes via Feed(); poll via TryDecode(out).
        class WsDecoder {
        public:
            void Feed(const char *data, size_t len);
            // Returns true and fills `out` with one complete frame. False if more data
            // needed. Returns -1 on protocol error (caller should close).
            int TryDecode(WsFrame *out);

        private:
            std::vector<char> buf_;
        };

        // Build a server-to-client frame (no masking). Appends to `out`.
        void EncodeFrame(const WsFrame &f, std::string *out);

    } // namespace inspector
} // namespace napi_v8

#endif // SRC_V8_INSPECTOR_WEBSOCKET_H_
