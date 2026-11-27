/**
 * @file udp_client.cpp
 * @brief pollnet::UdpSocket 示例：绑定本地端口与默认对端地址，封装并发送自定义 UDP 数据报。
 *
 * 演示核心特性：
 * 1. 通过 UdpOptions 同时指定本地绑定地址与默认远端地址（dest_ip/dest_port）；
 * 2. 栈上预分配发送缓冲区，零堆分配组装网络字节序包头与消息体；
 * 3. 调用 client.send(...) 向默认目标发送数据包（如需发往不同目标，可使用 client.sendTo(..., dst)）。
 */

#include <array>
#include <cstdint>
#include <cstring>
#include <format>
#include <iostream>
#include <ranges>
#include <span>
#include <string>

#include "pollnet/socket.h"

// 与 udp_server.cpp 约定一致的 8 字节大端包头
struct MsgHeader {
    uint32_t type;
    uint32_t seq;
};

int main() {
    // 配置 dest_ip / dest_port 后，open() 会在底层对 UDP socket 执行 connect() 绑定默认对端，
    // 之后可直接调用 client.send() / client.recv() 而无需每次传入目标地址。
    pollnet::UdpSocket<> client;
    if (!client.open({.bind_ip = "127.0.0.1",
                      .bind_port = 4321,
                      .dest_ip = "127.0.0.1",
                      .dest_port = 1234})) {
        std::cout << std::format("open error: {}\n", client.lastError());
        return 1;
    }

    // 以太网 MTU 1500 扣除 IP(20B) + UDP(8B) 头后最大不分片有效载荷为 1472 字节
    constexpr size_t kMaxUdpPayload = 1472;
    std::array<uint8_t, kMaxUdpPayload> send_buf{};

    // 连续构造并发送 7 个示例数据包（seq = 5..11）
    for (uint32_t i = 5; i < 12; ++i) {
        auto msg = std::format("hello{}", i);
        size_t body_len = msg.size();
        if (sizeof(MsgHeader) + body_len > kMaxUdpPayload) {
            std::cout << "too big\n";
            break;
        }

        // 将包头字段转为网络字节序（大端），并与字符串包体依次写入发送缓冲区
        MsgHeader header{htonl(1), htonl(i)};
        std::memcpy(send_buf.data(), &header, sizeof(MsgHeader));
        std::memcpy(send_buf.data() + sizeof(MsgHeader), msg.data(), body_len);

        // 调试输出：十六进制打印实际发送的总字节序列
        size_t total_len = sizeof(MsgHeader) + body_len;
        for (uint8_t b : send_buf | std::views::take(total_len)) {
            std::cout << std::format("{:02x} ", b);
        }
        std::cout << '\n';

        // 发送已构造好的 UDP 数据报；高并发批量发包场景也可使用 client.sendBatch(...)（Linux 下走 sendmmsg）
        if (!client.send(std::span<const uint8_t>{send_buf.data(), total_len})) {
            std::cout << std::format("send failed: {}\n", client.lastError());
        }
    }

    return 0;
}
