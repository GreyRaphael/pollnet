#include <atomic>
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

std::string addr_to_string(const sockaddr_in& addr) {
    char buf[INET_ADDRSTRLEN]{};
    if (::inet_ntop(AF_INET, &addr.sin_addr, buf, sizeof(buf)) == nullptr) {
        return "<invalid>";
    }
    return buf;
}
}  // namespace

struct MsgHeader {
    uint32_t type;
    uint32_t seq;
};

int main() {
    std::signal(SIGINT, handle_signal);

    pollnet::SocketUdpReceiver<> server;
    if (!server.init("", "127.0.0.1", 1234)) {
        std::cout << std::format("init failed: {}\n", server.getLastError());
        return 1;
    }

    while (g_running.load(std::memory_order_relaxed)) {
        [[maybe_unused]] bool received =
            server.recvfrom([](const uint8_t* data, uint32_t size, const sockaddr_in& addr) {
                if (size < sizeof(MsgHeader)) {
                    std::cout << std::format("drop short packet (size={}) from [{}:{}]\n",
                                             size, addr_to_string(addr), ntohs(addr.sin_port));
                    return;
                }

                for (auto b : std::span(data, size)) {
                    std::cout << std::format("{:02x} ", b);
                }
                std::cout << std::format("from [{}:{}]\n", addr_to_string(addr), ntohs(addr.sin_port));

                MsgHeader req_header{};
                std::memcpy(&req_header, data, sizeof(MsgHeader));
                uint32_t type = ntohl(req_header.type);
                uint32_t seq = ntohl(req_header.seq);

                const char* body_ptr = reinterpret_cast<const char*>(data + sizeof(MsgHeader));
                size_t body_len = size - sizeof(MsgHeader);
                std::string_view body(body_ptr, body_len);

                std::cout << std::format("recv {}, type={}, seq={}\n", body, type, seq);
            });
    }

    return 0;
}
