/**
 * @file eventloop_tcp_server.cpp
 * @brief pollnet::EventLoopTcpServer 示例：跨平台事件循环多连接 TCP 回显服务端。
 *
 * 与标准 `TcpServer`（逐连接轮询）相比，EventLoopTcpServer 在底层使用：
 * - Linux   : epoll (仅处理活跃连接，O(active) 派发，适合数十到上千并发连接)
 * - Windows : WSAPoll
 * - 其他POSIX: poll
 *
 * 演示核心特性：
 * 1. 可选准入控制回调 `allowNewConnection(const Endpoint&)`（按需拦截非法/限流连接）；
 * 2. 通过 `UserData`（ConnData）零堆分配附带连接统计状态；
 * 3. 基于 `consumed` 返回值的零拷贝粘包/半包拆包 + `IoSlice` 向量写（兼容 tcp_client 示例协议）；
 * 4. 内核低延迟调优选项（tcp_quickack / recv_buf_bytes / send_buf_bytes）与 `poll(handler, timeout_ms)`。
 */

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <charconv>
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

#include "pollnet/eventloop.h"

namespace {
// 全局运行标志，用于捕获 SIGINT（Ctrl+C）后优雅退出事件循环
std::atomic<bool> g_running{true};
void handle_signal(int) { g_running.store(false, std::memory_order_relaxed); }

// 每条连接附带的自定义用户数据（由 EventLoopTcpServer::Conn 公有继承，零额外分配）
struct ConnData {
    pollnet::Endpoint peer_addr{};
    uint64_t msg_count = 0;
};

// 与 examples/tcp_client.cpp 约定一致的协议包头：4 字节网络字节序（大端）包体长度
struct MsgHeader {
    uint32_t body_len;
};

// 示例业务逻辑：将请求内容转为大写并重复拼接后回显
std::string upper_and_double(std::string_view sv) {
    std::string out;
    out.reserve(sv.size() * 2);

    std::ranges::transform(sv, std::back_inserter(out), [](unsigned char c) {
        return static_cast<char>(std::toupper(c));
    });

    out.append(out);
    return out;
}

// 轻量无异常整数解析（用于解析可选命令行参数 timeout_ms）
bool parse_int(const char* s, int& out) noexcept {
    if (!s || *s == '\0') return false;
    std::string_view sv{s};
    auto [ptr, ec] = std::from_chars(sv.data(), sv.data() + sv.size(), out);
    return ec == std::errc{} && ptr == sv.data() + sv.size();
}
}  // namespace

// 模板参数说明：
//   ConnData : 连接附带的自定义用户数据
//   4096     : 单连接预分配接收缓冲区大小（字节）
//   128      : 预分配连接池最大并发连接数（EventLoop 模式下大量空闲连接几乎不消耗额外 CPU）
using EventLoopServer = pollnet::EventLoopTcpServer<ConnData, 4096, 128>;

// 事件循环回调处理器（仅 onData 为必选，其余回调由框架通过 Concepts 在编译期按需分发）
class EventLoopEchoHandler {
   public:
    explicit EventLoopEchoHandler(EventLoopServer& server) : server_(server) {}

    // 可选准入回调：accept 到新连接但尚未加入连接池前触发，返回 false 将直接关闭该 socket
    bool allowNewConnection(const pollnet::Endpoint& peer) {
        // 示例默认全部放行；实际业务中可在此校验 IP 黑白名单或限流
        (void)peer;
        return true;
    }

    // 新连接建立完成时触发（此时 server_.connCount() 已计入当前连接）
    void onConnect(EventLoopServer::Conn& conn) {
        conn.peer_addr = conn.peer().value_or(pollnet::Endpoint{});
        conn.msg_count = 0;
        std::cout << std::format("[eventloop] new connection from {}, active={}\n",
                                 conn.peer_addr.to_string(), server_.connCount());
    }

    // 收到数据时触发，返回值 `consumed` 表示本轮实际处理的字节数
    size_t onData(EventLoopServer::Conn& conn, std::span<const uint8_t> data) {
        size_t offset = 0;

        // 粘包循环：按 [MsgHeader | body] 逐帧解析
        while (data.size() - offset >= sizeof(MsgHeader)) {
            MsgHeader req_header{};
            std::memcpy(&req_header, data.data() + offset, sizeof(MsgHeader));
            uint32_t req_body_len = ntohl(req_header.body_len);

            if (req_body_len > 4096 - sizeof(MsgHeader)) {
                conn.close("message too large");
                return data.size();
            }

            // 半包保护：数据未收全时跳出循环，未消费部分会自动保留等待下次可读事件
            if (data.size() - offset - sizeof(MsgHeader) < req_body_len) {
                break;
            }

            std::string_view req_body{
                reinterpret_cast<const char*>(data.data() + offset + sizeof(MsgHeader)),
                req_body_len};

            ++conn.msg_count;
            std::cout << std::format("[eventloop] recv #{} [len={}]: {}\n", conn.msg_count,
                                     req_body_len, req_body);

            auto result = upper_and_double(req_body);
            MsgHeader rsp_header{};
            rsp_header.body_len = htonl(static_cast<uint32_t>(result.size()));

            // IoSlice 向量写：零拼包拷贝一次发出包头与包体
            std::array<pollnet::IoSlice, 2> iov{{{&rsp_header, sizeof(MsgHeader)}, {result}}};
            if (!conn.writeNonblock(iov)) {
                return data.size();
            }

            offset += sizeof(MsgHeader) + req_body_len;
        }

        return offset;
    }

    // 超过 recv_timeout 未收到数据时触发
    void onRecvTimeout(EventLoopServer::Conn& conn) {
        std::cout << std::format("[eventloop] recv timeout from {}\n",
                                 conn.peer_addr.to_string());
        conn.close("timeout");
    }

    // 连接断开后触发（此时 server_.connCount() 已扣除当前连接）
    void onDisconnect(EventLoopServer::Conn& conn) {
        std::cout << std::format("[eventloop] disconnected {}, msgs={}, reason={}, active={}\n",
                                 conn.peer_addr.to_string(), conn.msg_count, conn.lastError(),
                                 server_.connCount());
    }

   private:
    EventLoopServer& server_;
};

int main(int argc, char** argv) {
    using namespace std::chrono_literals;
    std::signal(SIGINT, handle_signal);

    // 可通过命令行传入 poll 超时时间（单位毫秒）：
    //   0 : 非阻塞立即返回（适合绑核超低延迟忙轮询场景）
    //  >0 : 无 I/O 事件时最多阻塞等待 timeout_ms 毫秒以让出 CPU（默认 1ms）
    int poll_timeout_ms = 1;
    if (argc >= 2 && (!parse_int(argv[1], poll_timeout_ms) || poll_timeout_ms < 0)) {
        std::cerr << std::format("usage: {} [poll_timeout_ms >= 0]\n", argv[0]);
        return 1;
    }

    EventLoopServer server;
    // 配置监听端点、10 秒空闲超时及内核调优参数；可直接搭配 examples/tcp_client 进行联调
    if (!server.listen({.host = "127.0.0.1",
                        .port = 1234,
                        .backlog = 128,
                        .recv_timeout = 10s,
                        .tcp_quickack = true,
                        .recv_buf_bytes = 256 * 1024,
                        .send_buf_bytes = 256 * 1024})) {
        std::cerr << std::format("listen failed: {}\n", server.lastError());
        return 1;
    }

    std::cout << std::format("EventLoopTcpServer listening on 127.0.0.1:{} (poll_timeout={}ms)\n",
                             server.localPort(), poll_timeout_ms);

    EventLoopEchoHandler handler(server);
    while (g_running.load(std::memory_order_relaxed)) {
        server.poll(handler, poll_timeout_ms);
    }

    return 0;
}
