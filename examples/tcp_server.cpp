/**
 * @file tcp_server.cpp
 * @brief pollnet::TcpServer 示例：自定义定长包头（Length-Prefixed）TCP 回显服务端。
 *
 * 演示核心特性：
 * 1. 通过 UserData（ConnData）在连接对象上直接内联附带业务状态，无需额外哈希表查找；
 * 2. 基于返回值 `consumed` 实现零拷贝半包/粘包边界解析；
 * 3. 使用 pollnet::IoSlice 分散/聚集写（底层 sendmsg/WSASend）避免额外拼包拷贝；
 * 4. 亚秒级接收超时检测（onRecvTimeout）与优雅退出。
 */

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
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

#include "pollnet/socket.h"

namespace {
// 全局运行标志，用于捕获 SIGINT（Ctrl+C）后优雅退出事件轮询循环
std::atomic<bool> g_running{true};
void handle_signal(int) { g_running.store(false, std::memory_order_relaxed); }

// 每条连接附带的自定义用户数据（UserData）：
// 会被 TcpServer::Conn 直接公有继承，零堆分配且可直接通过 conn.peer_addr 访问。
struct ConnData {
    pollnet::Endpoint peer_addr{};
};

// 自定义应用层协议包头：4 字节网络字节序（大端）包体长度，紧随对应长度的包体。
struct MsgHeader {
    uint32_t body_len;
};

// 示例业务逻辑：将请求字符串转为大写并重复拼接一次后返回。
std::string upper_and_double(std::string_view sv) {
    std::string out;
    out.reserve(sv.size() * 2);

    std::ranges::transform(sv, std::back_inserter(out), [](unsigned char c) {
        return static_cast<char>(std::toupper(c));
    });

    out.append(out);
    return out;
}
}  // namespace

// 模板参数说明：
//   ConnData : 连接对象继承的自定义用户数据
//   4096     : 单连接预分配接收缓冲区大小（字节），同时也是单帧最大允许接收长度
//   10       : 预分配连接池最大并发连接数（零堆分配，超出后暂时不再 accept）
using TcpServer = pollnet::TcpServer<ConnData, 4096, 10>;

// 服务端事件处理器：
// 编译期采用 Concepts + `if constexpr` 分发回调，仅 `onData` 为必选，其余生命周期回调均按需实现。
class EchoHandler {
   public:
    explicit EchoHandler(TcpServer& server) : server_(server) {}

    // 新连接建立成功时触发（此时 server_.connCount() 已计入当前连接）
    void onConnect(TcpServer::Conn& conn) {
        conn.peer_addr = conn.peer().value_or(pollnet::Endpoint{});
        std::cout << std::format("new connection from {}, total={}\n",
                                 conn.peer_addr.to_string(), server_.connCount());
    }

    // 收到数据时触发：
    // 参数 `data` 为连接接收缓冲区内当前累积的全部未消费字节视图；
    // 返回值 `consumed` 告诉框架本次共处理了多少字节，未处理的半包数据会自动保留在缓冲区中等待下次读取。
    size_t onData(TcpServer::Conn& conn, std::span<const uint8_t> data) {
        size_t offset = 0;

        // 粘包循环：只要剩余未处理数据不少于定长包头，就持续尝试拆帧
        while (data.size() - offset >= sizeof(MsgHeader)) {
            // 使用 memcpy 读取协议头（避免直接强转指针导致未对齐内存访问 UB），并转为主机字节序
            MsgHeader req_header{};
            std::memcpy(&req_header, data.data() + offset, sizeof(MsgHeader));
            uint32_t req_body_len = ntohl(req_header.body_len);

            // 长度合法性校验：单条完整消息不能超过连接接收缓冲区上限，防止恶意长度包导致永远等不到完整帧
            if (req_body_len > 4096 - sizeof(MsgHeader)) {
                conn.close("message too large");
                return data.size();
            }

            // 半包检查：包体尚未收齐则跳出循环，保留从 `offset` 开始的数据到下一次 poll
            if (data.size() - offset - sizeof(MsgHeader) < req_body_len) {
                break;
            }

            // 零拷贝构造当前帧包体的 string_view 视图
            std::string_view req_body{
                reinterpret_cast<const char*>(data.data() + offset + sizeof(MsgHeader)),
                req_body_len};

            std::cout << std::format("Recv Body [len: {}]: {}\n", req_body_len, req_body);
            auto result = upper_and_double(req_body);

            // 构造响应头（大端长度）与响应体，使用 IoSlice 向量写一次发送，避免内存拼包拷贝
            MsgHeader rsp_header{};
            rsp_header.body_len = htonl(static_cast<uint32_t>(result.size()));

            std::array<pollnet::IoSlice, 2> iov{{{&rsp_header, sizeof(MsgHeader)}, {result}}};
            // writeNonblock：若内核发送缓冲区已满导致无法一次性非阻塞发完，内部会自动 close 连接并返回 false
            if (!conn.writeNonblock(iov)) {
                return data.size();
            }

            offset += sizeof(MsgHeader) + req_body_len;
        }

        // 返回本轮实际消费的总字节数，框架会自动将后续半包数据前移或复位缓冲区游标
        return offset;
    }

    // 超过 TcpServerOptions::recv_timeout 未收到任何数据时触发
    void onRecvTimeout(TcpServer::Conn& conn) {
        std::cout << "onRecvTimeout\n";
        conn.close("timeout");
    }

    // 连接断开后触发（此时 server_.connCount() 已扣除当前连接）；可通过 conn.lastError() 查看断开原因
    void onDisconnect(TcpServer::Conn& conn) {
        std::cout << std::format("client disconnected: {}, reason={}, total={}\n",
                                 conn.peer_addr.to_string(), conn.lastError(),
                                 server_.connCount());
    }

   private:
    TcpServer& server_;
};

int main() {
    using namespace std::chrono_literals;
    std::signal(SIGINT, handle_signal);

    // 若后续连接数较多（如数十到上千并发），可包含 "pollnet/eventloop.h" 并替换为 EventLoopTcpServer
    TcpServer server;
    // 使用 C++20 指定初始化器配置监听端点与 10 秒接收空闲超时
    if (!server.listen({.host = "127.0.0.1", .port = 1234, .recv_timeout = 10s})) {
        std::cout << std::format("listen failed: {}\n", server.lastError());
        return 1;
    }

    EchoHandler handler(server);
    // 单线程非阻塞轮询主循环：驱动 accept、连接超时检查、数据收发与回调派发
    while (g_running.load(std::memory_order_relaxed)) {
        server.poll(handler);
    }

    return 0;
}
