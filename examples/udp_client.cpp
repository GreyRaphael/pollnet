#include <array>
#include <cstdint>
#include <cstring>
#include <format>
#include <iostream>
#include <ranges>
#include <span>
#include <string>

#include "pollnet/socket.h"

struct MsgHeader {
    uint32_t type;
    uint32_t seq;
};

int main() {
    pollnet::SocketUdpSender client;
    if (!client.init("", "127.0.0.1", 4321, "127.0.0.1", 1234)) {
        std::cout << std::format("init error: {}\n", client.getLastError());
        return 1;
    }

    // 以太网 MTU 1500 扣除 IP(20B) + UDP(8B) 头后最大不分片有效载荷为 1472 字节
    constexpr size_t kMaxUdpPayload = 1472;
    std::array<uint8_t, kMaxUdpPayload> send_buf{};

    for (uint32_t i = 5; i < 12; ++i) {
        auto msg = std::format("hello{}", i);
        size_t body_len = msg.size();
        if (sizeof(MsgHeader) + body_len > kMaxUdpPayload) {
            std::cout << "too big\n";
            break;
        }

        MsgHeader header{htonl(1), htonl(i)};
        std::memcpy(send_buf.data(), &header, sizeof(MsgHeader));
        std::memcpy(send_buf.data() + sizeof(MsgHeader), msg.data(), body_len);

        size_t total_len = sizeof(MsgHeader) + body_len;
        for (uint8_t b : send_buf | std::views::take(total_len)) {
            std::cout << std::format("{:02x} ", b);
        }
        std::cout << '\n';

        if (!client.write(std::span<const uint8_t>{send_buf.data(), total_len})) {
            std::cout << std::format("send failed: {}\n", client.getLastError());
        }
    }

    return 0;
}
