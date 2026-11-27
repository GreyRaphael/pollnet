/**
 * @file udp_server.cpp
 * @brief pollnet::UdpSocket 示例：绑定本地端口并轮询接收带自定义包头的 UDP 数据报。
 *
 * 演示核心特性：
 * 1. 使用统一的 pollnet::UdpSocket 打开本地非阻塞 UDP 套接字；
 * 2. 通过 recvFrom 零堆分配回调读取数据包字节视图（std::span）及对端地址（Endpoint）；
 * 3. 安全解析自定义网络字节序包头与零拷贝提取包体视图。
 */

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
// 全局运行标志，用于捕获 SIGINT（Ctrl+C）后优雅退出收包循环
std::atomic<bool> g_running{true};
void handle_signal(int) { g_running.store(false, std::memory_order_relaxed); }

// 与 udp_client.cpp 约定一致的 8 字节大端包头
struct MsgHeader {
    uint32_t type;
    uint32_t seq;
};
}  // namespace

int main() {
    std::signal(SIGINT, handle_signal);

    // 默认模板参数 RecvBufSize = 1500（对应标准以太网 MTU）；
    // 开启 SO_REUSEADDR 与非阻塞模式，并可通过 UdpOptions 配置 SO_RCVBUF/busy_poll_us 或加入组播组。
    pollnet::UdpSocket<> server;
    if (!server.open({.bind_ip = "127.0.0.1", .bind_port = 1234})) {
        std::cout << std::format("open failed: {}\n", server.lastError());
        return 1;
    }

    while (g_running.load(std::memory_order_relaxed)) {
        // 非阻塞单包接收并附带对端 Endpoint；当前内核接收队列无数据时立即返回 false。
        // 若需要更高吞吐，可改用 server.recvFromBatch([](data, peer) { ... }) 在 Linux 下走 recvmmsg 批量收包。
        [[maybe_unused]] bool received =
            server.recvFrom([](std::span<const uint8_t> data, const pollnet::Endpoint& peer) {
                // UDP 按数据报边界递交：若收到的完整报文长度小于包头，直接丢弃异常短包
                if (data.size() < sizeof(MsgHeader)) {
                    std::cout << std::format("drop short packet (size={}) from [{}]\n",
                                             data.size(), peer.to_string());
                    return;
                }

                // 调试输出：以十六进制打印完整原始报文与发送方 IP:Port
                for (uint8_t b : data) {
                    std::cout << std::format("{:02x} ", b);
                }
                std::cout << std::format("from [{}]\n", peer.to_string());

                // 使用 memcpy 防止未对齐读取，并将字段从网络字节序（大端）转为主机字节序
                MsgHeader req_header{};
                std::memcpy(&req_header, data.data(), sizeof(MsgHeader));
                uint32_t type = ntohl(req_header.type);
                uint32_t seq = ntohl(req_header.seq);

                // 零拷贝提取包头之后的消息文本视图
                const char* body_ptr = reinterpret_cast<const char*>(data.data() + sizeof(MsgHeader));
                size_t body_len = data.size() - sizeof(MsgHeader);
                std::string_view body(body_ptr, body_len);

                std::cout << std::format("recv {}, type={}, seq={}\n", body, type, seq);
            });
    }

    return 0;
}
