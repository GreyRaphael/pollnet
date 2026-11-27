/**
 * @file udp_multicast.cpp
 * @brief pollnet::UdpSocket 示例：UDP 组播加入（IP_ADD_MEMBERSHIP）与批量收发（sendBatch / recvFromBatch）。
 *
 * 运行方式：
 *   1. 启动组播接收端：
 *      ./udp_multicast recv [group_ip=239.255.0.1] [port=12345] [iface_ip=0.0.0.0]
 *   2. 启动组播发送端：
 *      ./udp_multicast send [group_ip=239.255.0.1] [port=12345] [batch_size=4]
 *
 * 演示核心特性：
 * 1. 通过 UdpOptions 配置组播组地址（multicast_group_ip）、网卡 IP（multicast_interface_ip）与 TTL；
 * 2. 接收端使用 recvFromBatch<16> 一次批量收取最多 16 个报文（Linux 走 recvmmsg，Windows 自动回退 recvfrom）；
 * 3. 发送端使用预分配缓冲 + IoSlice 数组调用 sendBatch 批量发送（Linux 走 sendmmsg，Windows 自动回退 send）。
 */

#include <array>
#include <atomic>
#include <charconv>
#include <csignal>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <format>
#include <iostream>
#include <span>
#include <string>
#include <string_view>

#include "pollnet/socket.h"

namespace {
// 全局运行标志，用于捕获 SIGINT（Ctrl+C）后优雅退出接收循环
std::atomic<bool> g_running{true};
void handle_signal(int) { g_running.store(false, std::memory_order_relaxed); }

// 8 字节大端自定义协议包头，紧跟变长消息体
struct MsgHeader {
    uint32_t type;
    uint32_t seq;
};

// 无异常整数解析，确保端口号与批量参数范围安全
bool parse_int(std::string_view sv, int& out) noexcept {
    if (sv.empty()) return false;
    auto [ptr, ec] = std::from_chars(sv.data(), sv.data() + sv.size(), out);
    return ec == std::errc{} && ptr == sv.data() + sv.size();
}

void print_usage(const char* prog) {
    std::cerr << std::format(
        "usage:\n"
        "  {} recv [group_ip=239.255.0.1] [port=12345] [iface_ip=0.0.0.0]\n"
        "  {} send [group_ip=239.255.0.1] [port=12345] [batch_size=4]\n",
        prog, prog);
}

int run_recv(std::string_view group_ip, uint16_t port, std::string_view iface_ip) {
    std::signal(SIGINT, handle_signal);

    pollnet::UdpSocket<> sock;
    // 打开组播接收套接字：
    //   bind_ip                : 绑定 "0.0.0.0" 以兼容 Linux 与 Windows 组播接收限制
    //   multicast_group_ip     : 通过 IP_ADD_MEMBERSHIP 加入该 D 类组播组
    //   multicast_interface_ip : 加入组播所使用的本地网卡 IP（"0.0.0.0" 表示系统默认路由网卡）
    //   recv_buf_bytes         : 适当放大内核 SO_RCVBUF，降低突发批量组播丢包概率
    if (!sock.open({.bind_ip = "0.0.0.0",
                    .bind_port = port,
                    .multicast_group_ip = group_ip,
                    .multicast_interface_ip = iface_ip,
                    .recv_buf_bytes = 256 * 1024})) {
        std::cerr << std::format("multicast recv open failed: {}\n", sock.lastError());
        return 1;
    }

    std::cout << std::format(
        "UDP multicast receiver joined {} on port {} (iface={}), press Ctrl+C to stop...\n",
        group_ip, port, iface_ip);

    size_t total_packets = 0;
    while (g_running.load(std::memory_order_relaxed)) {
        // 批量非阻塞收包（单次最多收 16 个数据报，零堆分配）
        size_t n = sock.recvFromBatch<16>(
            [&](std::span<const uint8_t> data, const pollnet::Endpoint& peer) {
                if (data.size() < sizeof(MsgHeader)) {
                    std::cout << std::format("[mcast recv] drop short packet ({}B) from {}\n",
                                             data.size(), peer.to_string());
                    return;
                }

                MsgHeader hdr{};
                std::memcpy(&hdr, data.data(), sizeof(MsgHeader));
                uint32_t type = ntohl(hdr.type);
                uint32_t seq = ntohl(hdr.seq);

                std::string_view body{
                    reinterpret_cast<const char*>(data.data() + sizeof(MsgHeader)),
                    data.size() - sizeof(MsgHeader)};

                ++total_packets;
                std::cout << std::format("[mcast recv #{}] from={} type={} seq={} body={}\n",
                                         total_packets, peer.to_string(), type, seq, body);
            });
        (void)n;
    }

    std::cout << std::format("stopped, total received packets={}\n", total_packets);
    return 0;
}

int run_send(std::string_view group_ip, uint16_t port, size_t batch_size) {
    pollnet::UdpSocket<> sock;
    // 打开组播发送套接字：
    //   dest_ip / dest_port : 关联默认组播目标地址，随后可直接使用 sendBatch 发送切片列表
    //   multicast_ttl = 1   : 限制组播报文仅在当前局域网子网内传播
    if (!sock.open({.bind_ip = "0.0.0.0",
                    .bind_port = 0,
                    .dest_ip = group_ip,
                    .dest_port = port,
                    .multicast_ttl = 1,
                    .send_buf_bytes = 256 * 1024})) {
        std::cerr << std::format("multicast send open failed: {}\n", sock.lastError());
        return 1;
    }

    constexpr size_t kMaxBatch = 16;
    constexpr size_t kMaxPacketBytes = 256;
    std::array<std::array<uint8_t, kMaxPacketBytes>, kMaxBatch> pkt_bufs{};
    std::array<pollnet::IoSlice, kMaxBatch> slices{};

    for (size_t i = 0; i < batch_size; ++i) {
        auto body = std::format("mcast-hello-{}", i + 1);
        MsgHeader hdr{htonl(1), htonl(static_cast<uint32_t>(i + 1))};
        size_t total_len = sizeof(MsgHeader) + body.size();

        std::memcpy(pkt_bufs[i].data(), &hdr, sizeof(MsgHeader));
        std::memcpy(pkt_bufs[i].data() + sizeof(MsgHeader), body.data(), body.size());
        slices[i] = pollnet::IoSlice{pkt_bufs[i].data(), total_len};
    }

    // 一次提交全部切片批量发出；在 Linux 下走 sendmmsg 系统调用，减少用户态/内核态切换次数
    size_t sent = sock.sendBatch(std::span<const pollnet::IoSlice>{slices.data(), batch_size});
    if (sent == 0) {
        std::cerr << "sendBatch sent 0 packets (would block or network route unreachable)\n";
        return 1;
    }

    std::cout << std::format("sent {}/{} multicast packets to {}:{}\n", sent, batch_size, group_ip,
                             port);
    return (sent == batch_size) ? 0 : 1;
}
}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        print_usage(argv[0]);
        return 1;
    }

    std::string_view mode{argv[1]};
    std::string_view group_ip = (argc >= 3) ? std::string_view{argv[2]} : "239.255.0.1";

    int port_val = 12345;
    if (argc >= 4 && (!parse_int(argv[3], port_val) || port_val <= 0 || port_val > 65535)) {
        std::cerr << "invalid port (expected 1..65535)\n";
        return 1;
    }
    uint16_t port = static_cast<uint16_t>(port_val);

    if (mode == "recv") {
        std::string_view iface_ip = (argc >= 5) ? std::string_view{argv[4]} : "0.0.0.0";
        return run_recv(group_ip, port, iface_ip);
    }

    if (mode == "send") {
        int batch_val = 4;
        if (argc >= 5 && (!parse_int(argv[4], batch_val) || batch_val <= 0 || batch_val > 16)) {
            std::cerr << "invalid batch_size (expected 1..16)\n";
            return 1;
        }
        return run_send(group_ip, port, static_cast<size_t>(batch_val));
    }

    print_usage(argv[0]);
    return 1;
}
