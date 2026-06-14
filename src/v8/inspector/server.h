// TCP server that accepts a single client, performs the HTTP/1.1
// WebSocket upgrade handshake (parsed via llhttp), then ferries CDP
// messages between the client and the V8 inspector bridge.

#ifndef SRC_V8_INSPECTOR_SERVER_H_
#define SRC_V8_INSPECTOR_SERVER_H_

#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <thread>

namespace napi_v8 {
    namespace inspector {

        class InspectorServer {
        public:
            using OnMessage = std::function<void(const std::string &)>;

            InspectorServer();
            ~InspectorServer();

            // Bind a listening socket on `port` (loopback). Returns false on error.
            bool Start(int port, const std::string &context_name);

            // Stop the listener, close any client, join threads.
            void Stop();

            // Block until the first client connects.
            void WaitForConnection();

            // Set a callback to handle CDP messages received from the client.
            // Called on the inspector thread.
            void SetOnMessage(OnMessage cb) { on_message_ = std::move(cb); }

            // Send a CDP message frame to the connected client (thread-safe).
            void Send(const std::string &msg);

            bool HasClient() const { return has_client_.load(); }

        private:
            void AcceptLoop();
            bool ServeConnection(int sock);
            bool SendHttp(int sock, const std::string &s);
            bool RecvLine(int sock, std::string *out, std::string *leftover);
            bool HandshakeAndUpgrade(int sock, std::string *leftover);
            void ClientReadLoop(int sock, std::string leftover);
            void ClientWriteLoop(int sock);

            int listen_fd_ = -1;
            int client_fd_ = -1;
            std::atomic<bool> stop_{false};
            std::atomic<bool> has_client_{false};
            std::string context_name_;

            std::thread accept_thread_;
            std::thread write_thread_;

            std::mutex out_mu_;
            std::condition_variable out_cv_;
            std::deque<std::string> out_q_;

            std::mutex wait_mu_;
            std::condition_variable wait_cv_;

            OnMessage on_message_;
        };

    } // namespace inspector
} // namespace napi_v8

#endif // SRC_V8_INSPECTOR_SERVER_H_
