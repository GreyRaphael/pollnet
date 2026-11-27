/**
 * @file tcp_client.cpp
 * @brief pollnet::TcpClient 示例：非阻塞建连、自动重连与基于 IoSlice 的请求-响应客户端。
 *
 * 演示核心特性：
 * 1. 非阻塞 connect 与失败自动重连（retry_interval）状态机；
 * 2. 通过 `conn.write(iov)` 向量写一次性发送连续多帧请求（演示服务端粘包处理）；
 * 3. 按照自定义定长包头解析服务端回显响应。
 */

#include <array>
#include <atomic>
#include <chrono>
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
// 全局运行标志，用于捕获 SIGINT（Ctrl+C）后优雅退出事件轮询循环
std::atomic<bool> g_running{true};
void handle_signal(int) { g_running.store(false, std::memory_order_relaxed); }

// 与 tcp_server.cpp 约定一致的协议包头：4 字节大端包体长度
struct MsgHeader {
    uint32_t body_len;
};
}  // namespace

// 使用默认模板参数：无额外 UserData，单连接接收缓冲区为 4096 字节
using TcpClient = pollnet::TcpClient<>;

// 客户端事件处理器（仅 onData 为必选，连接生命周期回调按需实现）
class ClientHandler {
   public:
    explicit ClientHandler(TcpClient& client) : client_(client) {}

    // 建连失败（例如服务端未启动、被拒绝连接或超过 connect_timeout）时触发
    void onConnectFailed() {
        std::cout << std::format("connect error: {}\n", client_.lastError());
    }

    // 与服务端 TCP 连接建立成功时触发
    void onConnect(TcpClient::Conn& conn) {
        std::cout << std::format("connected! first={}\n", is_first_);
        // 仅在首次建连成功时发送测试请求，避免重连后重复发包
        if (is_first_) {
            std::string msg{"hello"};
            MsgHeader header{};
            header.body_len = htonl(static_cast<uint32_t>(msg.size()));

            // 构造 4 个 IoSlice 分片，在一次系统调用中连续发送 2 条 "[MsgHeader | hello]" 消息，
            // 用于直观验证服务端在单次 recv 中的粘包解析能力
            std::array<pollnet::IoSlice, 4> iov{{
                {&header, sizeof(MsgHeader)},
                {msg},
                {&header, sizeof(MsgHeader)},
                {msg},
            }};
            // conn.write(...) 会在遭遇 EAGAIN/WSAEWOULDBLOCK 时自旋重试直到全部写完（或连接出错关闭）
            if (!conn.write(iov)) {
                std::cout << std::format("write error: {}\n", conn.lastError());
            }
        }
        is_first_ = false;
    }

    // 连接断开时触发（若配置了 retry_interval > 0，后续 poll() 会按间隔自动发起重连）
    void onDisconnect([[maybe_unused]] TcpClient::Conn& conn) {
        std::cout << "disconnected!\n";
    }

    // 收到服务端数据时触发，返回值表示本轮已消费的字节数
    size_t onData(TcpClient::Conn& conn, std::span<const uint8_t> data) {
        size_t offset = 0;

        // 与服务端对称的 [MsgHeader | body] 粘包/半包解析循环
        while (data.size() - offset >= sizeof(MsgHeader)) {
            MsgHeader header{};
            std::memcpy(&header, data.data() + offset, sizeof(MsgHeader));
            uint32_t body_len = ntohl(header.body_len);
            if (body_len > 4096 - sizeof(MsgHeader)) {
                conn.close("message too large");
                return data.size();
            }

            // 包体尚未收完整，跳出等待后续数据到达
            if (data.size() - offset - sizeof(MsgHeader) < body_len) {
                break;
            }

            std::string_view body{
                reinterpret_cast<const char*>(data.data() + offset + sizeof(MsgHeader)),
                body_len};

            std::cout << std::format("Recv Body [len: {}]: {}\n", body_len, body);
            offset += sizeof(MsgHeader) + body_len;
        }
        return offset;
    }

   private:
    TcpClient& client_;
    bool is_first_ = true;
};

int main() {
    using namespace std::chrono_literals;
    std::signal(SIGINT, handle_signal);

    TcpClient client;
    // connect() 仅校验参数并初始化非阻塞客户端配置，实际建连与重连由后续 poll() 驱动：
    //   retry_interval  = 3s  : 建连失败或断线后每隔 3 秒自动重连一次
    //   connect_timeout = 30s : 单次 TCP 握手超时时间
    if (!client.connect({.host = "127.0.0.1",
                         .port = 1234,
                         .retry_interval = 3s,
                         .connect_timeout = 30s})) {
        std::cout << std::format("connect init error: {}\n", client.lastError());
        return 1;
    }

    ClientHandler handler(client);
    // 非阻塞轮询主循环：驱动建连状态机、重连计时、数据收发与回调分发
    while (g_running.load(std::memory_order_relaxed)) {
        client.poll(handler);
    }

    return 0;
}
