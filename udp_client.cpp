#include <cstdint>
#include <cstring>
#include <format>
#include <print>
#include <ranges>
#include <string>

#include "socket.h"

struct MsgHeader {
    uint32_t type;
    uint32_t seq;
};

int main(int argc, char const* argv[]) {
    SocketUdpSender client;
    if (!client.init("", "127.0.0.1", 4321, "127.0.0.1", 1234)) {
        std::println("init error:{}", client.getLastError());
        exit(1);
    }

    // 以太网的 MTU 是 1500 字节
    static thread_local char send_buf[1500];
    for (uint32_t i = 5; i < 12; ++i) {
        // prepare body
        auto msg = std::format("hello{}", i);
        auto body_len = msg.size();
        // prepare header
        MsgHeader header{1, i};
        if (sizeof(MsgHeader) + body_len > 1500) {
            std::println("too big");
            break;
        }

        // send
        std::memcpy(send_buf, &header, sizeof(MsgHeader));
        std::memcpy(send_buf + sizeof(MsgHeader), msg.data(), body_len);
        // std::println("send [len={}], data={:02d}", sizeof(MsgHeader) + body_len, send_buf);
        for (unsigned char b : send_buf | std::views::take(sizeof(MsgHeader) + body_len)) {  // take(10) just to limit output
            std::print("{:02x} ", b);
        }
        std::println("");

        client.write(send_buf, sizeof(MsgHeader) + body_len);
    }
}