#include "server.h"
#include "websocket.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cstdio>
#include <cstring>

extern "C" {
#include "llhttp.h"
}

namespace napi_v8 {
    namespace inspector {

        namespace {

            struct ParseState {
                std::string url;
                std::string ws_key;
                bool upgrade = false;
                bool websocket = false;
                // Header tracking
                std::string current_header;
                bool complete = false;
            };

            int OnUrl(llhttp_t *p, const char *at, size_t len) {
                auto *st = static_cast<ParseState *>(p->data);
                st->url.append(at, len);
                return 0;
            }

            int OnHeaderField(llhttp_t *p, const char *at, size_t len) {
                auto *st = static_cast<ParseState *>(p->data);
                st->current_header.assign(at, len);
                for (auto &c: st->current_header) {
                    if (c >= 'A' && c <= 'Z')
                        c |= 0x20;
                }
                return 0;
            }

            int OnHeaderValue(llhttp_t *p, const char *at, size_t len) {
                auto *st = static_cast<ParseState *>(p->data);
                std::string v(at, len);
                if (st->current_header == "upgrade") {
                    for (auto &c: v)
                        if (c >= 'A' && c <= 'Z')
                            c |= 0x20;
                    if (v.find("websocket") != std::string::npos)
                        st->websocket = true;
                } else if (st->current_header == "connection") {
                    for (auto &c: v)
                        if (c >= 'A' && c <= 'Z')
                            c |= 0x20;
                    if (v.find("upgrade") != std::string::npos)
                        st->upgrade = true;
                } else if (st->current_header == "sec-websocket-key") {
                    st->ws_key = v;
                }
                return 0;
            }

            int OnMessageComplete(llhttp_t *p) {
                static_cast<ParseState *>(p->data)->complete = true;
                return 0;
            }

        } // namespace

        InspectorServer::InspectorServer() = default;

        InspectorServer::~InspectorServer() { Stop(); }

        bool InspectorServer::Start(int port, const std::string &context_name) {
            context_name_ = context_name;
            port_ = port;
            listen_fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
            if (listen_fd_ < 0)
                return false;
            int reuse = 1;
            setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

            sockaddr_in addr{};
            addr.sin_family = AF_INET;
            addr.sin_port = htons(port);
            addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

            if (::bind(listen_fd_, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) != 0) {
                perror("[inspector] bind");
                close(listen_fd_);
                listen_fd_ = -1;
                return false;
            }
            if (::listen(listen_fd_, 1) != 0) {
                perror("[inspector] listen");
                close(listen_fd_);
                listen_fd_ = -1;
                return false;
            }
            std::fprintf(stderr, "[inspector] listening on ws://127.0.0.1:%d\n", port);
            accept_thread_ = std::thread([this] { AcceptLoop(); });
            return true;
        }

        void InspectorServer::Stop() {
            if (stop_.exchange(true))
                return;
            if (listen_fd_ >= 0) {
                ::shutdown(listen_fd_, SHUT_RDWR);
                ::close(listen_fd_);
                listen_fd_ = -1;
            }
            if (client_fd_ >= 0) {
                ::shutdown(client_fd_, SHUT_RDWR);
                ::close(client_fd_);
                client_fd_ = -1;
            }
            {
                std::lock_guard<std::mutex> g(out_mu_);
                out_q_.clear();
                out_cv_.notify_all();
            }
            if (accept_thread_.joinable())
                accept_thread_.join();
            if (write_thread_.joinable())
                write_thread_.join();
        }

        void InspectorServer::WaitForConnection() {
            std::unique_lock<std::mutex> g(wait_mu_);
            wait_cv_.wait(g, [this] { return has_client_.load() || stop_.load(); });
        }

        void InspectorServer::Send(const std::string &msg) {
            std::lock_guard<std::mutex> g(out_mu_);
            out_q_.push_back(msg);
            out_cv_.notify_one();
        }

        void InspectorServer::AcceptLoop() {
            while (!stop_.load()) {
                sockaddr_in caddr{};
                socklen_t clen = sizeof(caddr);
                int s = ::accept(listen_fd_, reinterpret_cast<sockaddr *>(&caddr), &clen);
                if (s < 0) {
                    if (stop_.load())
                        return;
                    continue;
                }
#ifdef SO_NOSIGPIPE
                // A debugger client that disconnects mid-session must not raise
                // SIGPIPE and terminate the host process. macOS has no
                // MSG_NOSIGNAL, so suppress it per-socket (Linux uses the send
                // flag below).
                {
                    int on = 1;
                    setsockopt(s, SOL_SOCKET, SO_NOSIGPIPE, &on, sizeof(on));
                }
#endif
                ServeConnection(s);
                ::close(s);
                has_client_.store(false);
            }
        }

        bool InspectorServer::SendHttp(int sock, const std::string &s) {
#ifdef MSG_NOSIGNAL
            const int send_flags = MSG_NOSIGNAL;  // Linux: suppress SIGPIPE per send
#else
            const int send_flags = 0;             // macOS relies on SO_NOSIGPIPE (set on accept)
#endif
            ssize_t off = 0;
            while (off < (ssize_t) s.size()) {
                ssize_t n = ::send(sock, s.data() + off, s.size() - off, send_flags);
                if (n <= 0)
                    return false;
                off += n;
            }
            return true;
        }

        bool InspectorServer::ServeConnection(int sock) {
            // Buffer for HTTP request parsing.
            std::string raw;
            char tmp[4096];
            llhttp_t parser;
            llhttp_settings_t settings;
            ParseState state;

            llhttp_settings_init(&settings);
            settings.on_url = OnUrl;
            settings.on_header_field = OnHeaderField;
            settings.on_header_value = OnHeaderValue;
            settings.on_message_complete = OnMessageComplete;
            llhttp_init(&parser, HTTP_REQUEST, &settings);
            parser.data = &state;

            while (!state.complete && !stop_.load()) {
                ssize_t n = ::recv(sock, tmp, sizeof(tmp), 0);
                if (n <= 0)
                    return false;
                raw.append(tmp, n);
                enum llhttp_errno err = llhttp_execute(&parser, tmp, n);
                if (err != HPE_OK && err != HPE_PAUSED_UPGRADE)
                    return false;
            }

            // Route /json/version, /json, and /json/list (very minimal CDP discovery).
            if (state.url == "/json" || state.url == "/json/list") {
                std::string ws_url = "ws://127.0.0.1:" + std::to_string(port_) + "/napi-v8";
                std::string body = "[{\"description\":\"napi_v8\","
                                   "\"id\":\"napi-v8\","
                                   "\"title\":\"" +
                                   context_name_ +
                                   "\","
                                   "\"type\":\"node\","
                                   "\"webSocketDebuggerUrl\":\"" +
                                   ws_url +
                                   "\","
                                   "\"devtoolsFrontendUrl\":\"\"}]";
                char hdr[256];
                int hl = std::snprintf(hdr, sizeof(hdr),
                                       "HTTP/1.1 200 OK\r\n"
                                       "Content-Type: application/json; charset=UTF-8\r\n"
                                       "Content-Length: %zu\r\nConnection: close\r\n\r\n",
                                       body.size());
                std::string resp(hdr, hl);
                resp += body;
                SendHttp(sock, resp);
                return true;
            }
            if (state.url == "/json/version") {
                std::string body = "{\"Browser\":\"napi_v8/1.0\","
                                   "\"Protocol-Version\":\"1.3\"}";
                char hdr[256];
                int hl = std::snprintf(hdr, sizeof(hdr),
                                       "HTTP/1.1 200 OK\r\n"
                                       "Content-Type: application/json; charset=UTF-8\r\n"
                                       "Content-Length: %zu\r\nConnection: close\r\n\r\n",
                                       body.size());
                SendHttp(sock, std::string(hdr, hl) + body);
                return true;
            }

            // Else: WebSocket upgrade.
            if (!state.upgrade || !state.websocket || state.ws_key.empty()) {
                const char *m = "HTTP/1.1 400 Bad Request\r\nContent-Length: 0\r\n\r\n";
                SendHttp(sock, m);
                return false;
            }

            std::string accept = AcceptKey(state.ws_key);
            std::string resp = "HTTP/1.1 101 Switching Protocols\r\n"
                               "Upgrade: websocket\r\n"
                               "Connection: Upgrade\r\n"
                               "Sec-WebSocket-Accept: " +
                               accept + "\r\n\r\n";
            if (!SendHttp(sock, resp))
                return false;

            client_fd_ = sock;
            has_client_.store(true);
            {
                std::lock_guard<std::mutex> g(wait_mu_);
                wait_cv_.notify_all();
            }
            write_thread_ = std::thread([this, sock] { ClientWriteLoop(sock); });
            ClientReadLoop(sock, std::string());
            if (write_thread_.joinable())
                write_thread_.join();
            client_fd_ = -1;
            return true;
        }

        void InspectorServer::ClientReadLoop(int sock, std::string leftover) {
            WsDecoder dec;
            if (!leftover.empty())
                dec.Feed(leftover.data(), leftover.size());
            char tmp[4096];
            WsFrame frame;
            std::string assembled;
            while (!stop_.load()) {
                ssize_t n = ::recv(sock, tmp, sizeof(tmp), 0);
                if (n <= 0)
                    break;
                dec.Feed(tmp, n);
                while (dec.TryDecode(&frame) == 1) {
                    if (frame.op == WsOp::kClose)
                        return;
                    if (frame.op == WsOp::kPing) {
                        WsFrame pong{true, WsOp::kPong, frame.payload};
                        std::string buf;
                        EncodeFrame(pong, &buf);
                        SendHttp(sock, buf);
                        continue;
                    }
                    if (frame.op == WsOp::kText || frame.op == WsOp::kBinary || frame.op == WsOp::kContinuation) {
                        assembled += frame.payload;
                        if (frame.fin) {
                            if (on_message_)
                                on_message_(assembled);
                            assembled.clear();
                        }
                    }
                }
            }
        }

        void InspectorServer::ClientWriteLoop(int sock) {
            while (!stop_.load()) {
                std::string msg;
                {
                    std::unique_lock<std::mutex> g(out_mu_);
                    out_cv_.wait(g, [this] { return !out_q_.empty() || stop_.load(); });
                    if (stop_.load() && out_q_.empty())
                        return;
                    msg = std::move(out_q_.front());
                    out_q_.pop_front();
                }
                WsFrame f{true, WsOp::kText, std::move(msg)};
                std::string raw;
                EncodeFrame(f, &raw);
                if (!SendHttp(sock, raw))
                    return;
            }
        }

    } // namespace inspector
} // namespace napi_v8
