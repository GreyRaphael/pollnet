#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstring>
#include <format>
#include <iostream>
#include <iterator>
#include <span>
#include <string>
#include <string_view>

#include "pollnet/socket.h"

namespace {
std::atomic<bool> g_running{true};
void handle_signal(int) { g_running.store(false, std::memory_order_relaxed); }

struct ConnData {
    pollnet::Endpoint peer_addr{};
};

struct MsgHeader {
    uint32_t body_len;
};

std::string upper_and_double(std::string_view sv) {
    std::string out;
    out.reserve(sv.size() * 2);

    std::ranges::transform(sv, std::back_inserter(out), [](unsigned char c) {
        return static_cast<char>(std::toupper(c));
    });

    out.append(out);
    return out;
}
}  // namespace

using TcpServer = pollnet::TcpServer<ConnData, 4096, 10>;

class EchoHandler {
   public:
    explicit EchoHandler(TcpServer& server) : server_(server) {}

    void onConnect(TcpServer::Conn& conn) {
        conn.peer_addr = conn.peer().value_or(pollnet::Endpoint{});
        std::cout << std::format("new connection from {}, total={}\n",
                                 conn.peer_addr.to_string(), server_.connCount());
    }

    size_t onData(TcpServer::Conn& conn, std::span<const uint8_t> data) {
        size_t offset = 0;
        while (data.size() - offset >= sizeof(MsgHeader)) {
            MsgHeader req_header{};
            std::memcpy(&req_header, data.data() + offset, sizeof(MsgHeader));
            uint32_t req_body_len = ntohl(req_header.body_len);
            if (req_body_len > 4096 - sizeof(MsgHeader)) {
                conn.close("message too large");
                return data.size();
            }

            if (data.size() - offset - sizeof(MsgHeader) < req_body_len) {
                break;
            }

            std::string_view req_body{
                reinterpret_cast<const char*>(data.data() + offset + sizeof(MsgHeader)),
                req_body_len};

            std::cout << std::format("Recv Body [len: {}]: {}\n", req_body_len, req_body);
            auto result = upper_and_double(req_body);

            MsgHeader rsp_header{};
            rsp_header.body_len = htonl(static_cast<uint32_t>(result.size()));

            std::string packet;
            packet.resize(sizeof(MsgHeader) + result.size());
            std::memcpy(packet.data(), &rsp_header, sizeof(MsgHeader));
            std::memcpy(packet.data() + sizeof(MsgHeader), result.data(), result.size());
            if (!conn.writeNonblock(packet.data(), packet.size())) {
                return data.size();
            }

            offset += sizeof(MsgHeader) + req_body_len;
        }
        return offset;
    }

    void onRecvTimeout(TcpServer::Conn& conn) {
        std::cout << "onRecvTimeout\n";
        conn.close("timeout");
    }

    void onDisconnect(TcpServer::Conn& conn) {
        std::cout << std::format("client disconnected: {}, reason={}, total={}\n",
                                 conn.peer_addr.to_string(), conn.lastError(),
                                 server_.connCount());
    }

   private:
    TcpServer& server_;
};

int main() {
    using namespace std::chrono_literals;
    std::signal(SIGINT, handle_signal);

    TcpServer server;
    if (!server.listen({.host = "127.0.0.1", .port = 1234, .recv_timeout = 10s})) {
        std::cout << std::format("listen failed: {}\n", server.lastError());
        return 1;
    }

    EchoHandler handler(server);
    while (g_running.load(std::memory_order_relaxed)) {
        server.poll(handler);
    }

    return 0;
}
