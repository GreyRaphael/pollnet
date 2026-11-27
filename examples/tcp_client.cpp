#include <array>
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstring>
#include <format>
#include <iostream>
#include <span>
#include <string>
#include <string_view>

#include "pollnet/socket.h"

namespace {
std::atomic<bool> g_running{true};
void handle_signal(int) { g_running.store(false, std::memory_order_relaxed); }

struct MsgHeader {
    uint32_t body_len;
};
}  // namespace

using TcpClient = pollnet::TcpClient<>;

class ClientHandler {
   public:
    explicit ClientHandler(TcpClient& client) : client_(client) {}

    void onConnectFailed() {
        std::cout << std::format("connect error: {}\n", client_.lastError());
    }

    void onConnect(TcpClient::Conn& conn) {
        std::cout << std::format("connected! first={}\n", is_first_);
        if (is_first_) {
            std::string msg{"hello"};
            MsgHeader header{};
            header.body_len = htonl(static_cast<uint32_t>(msg.size()));

            std::array<pollnet::IoSlice, 4> iov{{
                {&header, sizeof(MsgHeader)},
                {msg},
                {&header, sizeof(MsgHeader)},
                {msg},
            }};
            if (!conn.writev(iov)) {
                std::cout << std::format("writev error: {}\n", conn.lastError());
            }
        }
        is_first_ = false;
    }

    void onDisconnect([[maybe_unused]] TcpClient::Conn& conn) {
        std::cout << "disconnected!\n";
    }

    size_t onData(TcpClient::Conn& conn, std::span<const uint8_t> data) {
        size_t offset = 0;
        while (data.size() - offset >= sizeof(MsgHeader)) {
            MsgHeader header{};
            std::memcpy(&header, data.data() + offset, sizeof(MsgHeader));
            uint32_t body_len = ntohl(header.body_len);
            if (body_len > 4096 - sizeof(MsgHeader)) {
                conn.close("message too large");
                return data.size();
            }

            if (data.size() - offset - sizeof(MsgHeader) < body_len) {
                break;
            }

            std::string_view body{
                reinterpret_cast<const char*>(data.data() + offset + sizeof(MsgHeader)),
                body_len};

            std::cout << std::format("Recv Body [len: {}]: {}\n", body_len, body);
            offset += sizeof(MsgHeader) + body_len;
        }
        return offset;
    }

   private:
    TcpClient& client_;
    bool is_first_ = true;
};

int main() {
    using namespace std::chrono_literals;
    std::signal(SIGINT, handle_signal);

    TcpClient client;
    if (!client.connect({.host = "127.0.0.1",
                         .port = 1234,
                         .retry_interval = 3s,
                         .connect_timeout = 30s})) {
        std::cout << std::format("connect init error: {}\n", client.lastError());
        return 1;
    }

    ClientHandler handler(client);
    while (g_running.load(std::memory_order_relaxed)) {
        client.poll(handler);
    }

    return 0;
}
