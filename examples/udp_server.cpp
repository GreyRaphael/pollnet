#include <atomic>
#include <csignal>
#include <cstdint>
#include <cstring>
#include <format>
#include <iostream>
#include <span>
#include <string_view>

#include "pollnet/socket.h"

namespace {
std::atomic<bool> g_running{true};
void handle_signal(int) { g_running.store(false, std::memory_order_relaxed); }

struct MsgHeader {
    uint32_t type;
    uint32_t seq;
};
}  // namespace

int main() {
    std::signal(SIGINT, handle_signal);

    pollnet::UdpSocket<> server;
    if (!server.open({.bind_ip = "127.0.0.1", .bind_port = 1234})) {
        std::cout << std::format("open failed: {}\n", server.lastError());
        return 1;
    }

    while (g_running.load(std::memory_order_relaxed)) {
        [[maybe_unused]] bool received =
            server.recvFrom([](std::span<const uint8_t> data, const pollnet::Endpoint& peer) {
                if (data.size() < sizeof(MsgHeader)) {
                    std::cout << std::format("drop short packet (size={}) from [{}]\n",
                                             data.size(), peer.to_string());
                    return;
                }

                for (uint8_t b : data) {
                    std::cout << std::format("{:02x} ", b);
                }
                std::cout << std::format("from [{}]\n", peer.to_string());

                MsgHeader req_header{};
                std::memcpy(&req_header, data.data(), sizeof(MsgHeader));
                uint32_t type = ntohl(req_header.type);
                uint32_t seq = ntohl(req_header.seq);

                const char* body_ptr = reinterpret_cast<const char*>(data.data() + sizeof(MsgHeader));
                size_t body_len = data.size() - sizeof(MsgHeader);
                std::string_view body(body_ptr, body_len);

                std::cout << std::format("recv {}, type={}, seq={}\n", body, type, seq);
            });
    }

    return 0;
}
