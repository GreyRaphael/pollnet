#include <print>
#include <string_view>

#include "socket.h"

struct MsgHeader {
    uint32_t type;
    uint32_t seq;
};

int main(int argc, char const* argv[]) {
    SocketUdpReceiver<> server;
    if (!server.init("", "127.0.0.1", 1234)) {
        std::println("init failed: {}", server.getLastError());
        return 1;
    }

    while (true) {
        server.recvfrom([](const uint8_t* data, uint32_t size, auto addr) {
            // 1. 直接打印收到的原始数据 dat
            for (auto b : std::span(data, size)) {
                std::print("{:02x} ", b);
            }
            std::println("from [{}:{}]", inet_ntoa(addr.sin_addr), ntohs(addr.sin_port));

            // 2. 解析时也直接使用 data
            auto req_header = reinterpret_cast<const MsgHeader*>(data);

            // 注意类型转换，data 是 uint8_t*，计算偏移时很安全
            const char* body_ptr = reinterpret_cast<const char*>(data + sizeof(MsgHeader));
            auto body_len = size - sizeof(MsgHeader);

            auto body = std::string_view(body_ptr, body_len);
            std::println("recv {}, type={}, seq={}", body, req_header->type, req_header->seq);
        });
    }
}