/**
 * @file framer_demo.cpp
 * @brief pollnet 三种编译期零开销拆帧器综合示例（定长结构体 / 长度头 / 分隔符）。
 *
 * 运行方式：
 *   ./framer_demo            # 依次运行全部 3 种拆帧演示（默认）
 *   ./framer_demo fixed      # 仅运行 FixedFrameHandler（定长结构体拆帧）
 *   ./framer_demo length     # 仅运行 LengthPrefixedHandler（定长包头+变长包体拆帧）
 *   ./framer_demo delimiter  # 仅运行 DelimiterFrameHandler（分隔符拆帧）
 *
 * 演示特性：
 * 1. 定长结构体拆帧 (`makeFixedFrameHandler<TickMsg>`)：粘包 + 半包自动拼接；
 * 2. 长度头拆帧 (`makeLengthPrefixedHandler<MsgHeader>`)：结合 `lowLatency` 预设、
 *    `SendBufSize = 65536` 静态发送缓冲与 `sendParts` 可变参聚集写；
 * 3. 分隔符拆帧 (`makeDelimiterHandler("\r\n", ...)`)：单字节/多字节跨包边界切帧；
 * 4. 所有拆帧器均通过 `HandlerForwarder` 自动透传可选生命周期回调 (`onConnect`/`onDisconnect`)。
 */

#include <chrono>
#include <cstdint>
#include <format>
#include <iostream>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "pollnet/socket.h"

namespace {

// 轮询驱动辅助函数：在超时预算内驱动客户端与服务端步进，直到条件满足
template <typename Pred>
bool poll_until(Pred&& pred, int timeout_ms = 3000) {
    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    while (std::chrono::steady_clock::now() < deadline) {
        if (pred()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return pred();
}

// ==========================================
// 模式 1：FixedFrameHandler（定长结构体拆帧）
// ==========================================

bool demo_fixed_frame() {
    std::cout << "=== [1/3] FixedFrameHandler (定长结构体拆帧) ===\n";

    // 定长二进制协议结构体（POD / TriviallyCopyable）
    struct TickMsg {
        uint32_t seq;
        int64_t price_x100;
    };

    pollnet::TcpServer<> server;
    if (!server.listen({.host = "127.0.0.1", .port = 0})) {
        std::cerr << std::format("server listen failed: {}\n", server.lastError());
        return false;
    }

    pollnet::TcpClient<> client;
    if (!client.connect({.host = "127.0.0.1", .port = server.localPort()})) {
        std::cerr << std::format("client connect failed: {}\n", client.lastError());
        return false;
    }

    // 业务 Handler 无需编写 `onData` 字节流循环，只需实现 `onMessage(conn, const TickMsg&)`
    struct ServerMsgHandler {
        int connected = 0;
        void onConnect(pollnet::TcpConnection<>&) { ++connected; }
        void onMessage(pollnet::TcpConnection<>& conn, const TickMsg& msg) {
            std::cout << std::format("[fixed][server] recv seq={}, price_x100={}\n", msg.seq,
                                     msg.price_x100);
            // 直接把修改后的结构体作为 SendPart 回发，零手动拼包
            TickMsg ack{msg.seq, msg.price_x100 + 1};
            (void)conn.sendParts(ack);
        }
    } srv_inner;

    struct ClientMsgHandler {
        int connected = 0;
        std::vector<TickMsg> replies;
        void onConnect(pollnet::TcpConnection<>&) { ++connected; }
        void onMessage(pollnet::TcpConnection<>&, const TickMsg& msg) {
            std::cout << std::format("[fixed][client] ack seq={}, price_x100={}\n", msg.seq,
                                     msg.price_x100);
            replies.push_back(msg);
        }
    } cli_inner;

    auto srv_framed = pollnet::makeFixedFrameHandler<TickMsg>(srv_inner);
    auto cli_framed = pollnet::makeFixedFrameHandler<TickMsg>(cli_inner);

    if (!poll_until([&]() {
            server.poll(srv_framed);
            client.poll(cli_framed);
            return srv_inner.connected == 1 && cli_inner.connected == 1;
        })) {
        std::cerr << "fixed: connect timeout\n";
        return false;
    }

    // 一次聚集写粘包发送 2 个结构体 + 刻意拆成两段的第 3 个半包结构体
    TickMsg t1{1, 10050};
    TickMsg t2{2, 10120};
    TickMsg t3{3, 10200};
    if (!client.sendParts(t1, t2) || !client.writeNonblock(&t3, 4)) {
        std::cerr << "fixed: send error\n";
        return false;
    }

    if (!poll_until([&]() {
            server.poll(srv_framed);
            client.poll(cli_framed);
            return cli_inner.replies.size() == 2;
        })) {
        std::cerr << "fixed: first batch timeout\n";
        return false;
    }

    // 补全第 3 帧的剩余字节，验证自动半包拼接
    if (!client.writeNonblock(reinterpret_cast<const uint8_t*>(&t3) + 4, sizeof(TickMsg) - 4)) {
        std::cerr << "fixed: send tail error\n";
        return false;
    }

    if (!poll_until([&]() {
            server.poll(srv_framed);
            client.poll(cli_framed);
            return cli_inner.replies.size() == 3;
        })) {
        std::cerr << "fixed: half-packet timeout\n";
        return false;
    }

    return true;
}

// ==========================================
// 模式 2：LengthPrefixedHandler（定长包头 + 变长包体拆帧）
// ==========================================

bool demo_length_prefixed() {
    std::cout << "=== [2/3] LengthPrefixedHandler (长度头拆帧 + 静态发送缓冲) ===\n";

    // 自定义包头：2 字节类型 + 4 字节网络字节序长度
    struct MsgHeader {
        uint16_t msg_type;
        uint32_t body_len_be;
    };

    // 第 4 模板参数传入 65536：开启 64KiB 静态环形发送缓冲（写满时无损入队，并在 poll 中自动续发）
    using BufferedServer = pollnet::TcpServer<pollnet::EmptyUserData, 4096, 16, 65536>;
    using BufferedClient = pollnet::TcpClient<pollnet::EmptyUserData, 4096, 65536>;

    BufferedServer server;
    if (!server.listen(pollnet::TcpServerOptions::lowLatency("127.0.0.1", 0))) {
        std::cerr << std::format("length server listen failed: {}\n", server.lastError());
        return false;
    }

    BufferedClient client;
    if (!client.connect(pollnet::TcpClientOptions::lowLatency("127.0.0.1", server.localPort()))) {
        std::cerr << std::format("length client connect failed: {}\n", client.lastError());
        return false;
    }

    auto extract_len = [](const MsgHeader& h) { return ntohl(h.body_len_be); };

    // 服务端采用 3 参 onMessage(conn, hdr, body)：同时拿到包头元数据与包体视图
    struct ServerMsgHandler {
        int connected = 0;
        void onConnect(BufferedServer::Conn&) { ++connected; }
        void onMessage(BufferedServer::Conn& conn, const MsgHeader& hdr,
                       std::span<const uint8_t> body) {
            std::string_view text{reinterpret_cast<const char*>(body.data()), body.size()};
            std::cout << std::format("[length][server] type={}, len={}, body={}\n",
                                     ntohs(hdr.msg_type), body.size(), text);
            // 直接把包头结构体 + 包体视图聚集写回发
            (void)conn.sendParts(hdr, body);
        }
    } srv_inner;

    // 客户端采用 2 参 onMessage(conn, body)：若只关心包体，可省略 hdr 参数
    struct ClientMsgHandler {
        int connected = 0;
        std::vector<std::string> replies;
        void onConnect(BufferedClient::Conn&) { ++connected; }
        void onMessage(BufferedClient::Conn&, std::span<const uint8_t> body) {
            std::string text{reinterpret_cast<const char*>(body.data()), body.size()};
            std::cout << std::format("[length][client] echoed={}\n", text);
            replies.push_back(std::move(text));
        }
    } cli_inner;

    // 默认第 3 参 max_body_len = 0，框架会自动钳位为 Conn::kRecvBufSize - sizeof(MsgHeader)
    auto srv_framed = pollnet::makeLengthPrefixedHandler<MsgHeader>(extract_len, srv_inner);
    auto cli_framed = pollnet::makeLengthPrefixedHandler<MsgHeader>(extract_len, cli_inner);

    if (!poll_until([&]() {
            server.poll(srv_framed);
            client.poll(cli_framed);
            return srv_inner.connected == 1 && cli_inner.connected == 1;
        })) {
        std::cerr << "length: connect timeout\n";
        return false;
    }

    const std::string_view p1 = "ORDER_NEW";
    const std::string_view p2 = "ORDER_CANCEL";
    MsgHeader h1{htons(101), htonl(static_cast<uint32_t>(p1.size()))};
    MsgHeader h2{htons(102), htonl(static_cast<uint32_t>(p2.size()))};

    // 通过 sendParts 一次系统调用发出两条完整的 [Header | Body] 帧
    if (!client.sendParts(h1, p1, h2, p2)) {
        std::cerr << "length: sendParts error\n";
        return false;
    }

    if (!poll_until([&]() {
            server.poll(srv_framed);
            client.poll(cli_framed);
            return cli_inner.replies.size() == 2;
        })) {
        std::cerr << "length: reply timeout\n";
        return false;
    }

    return cli_inner.replies[0] == p1 && cli_inner.replies[1] == p2;
}

// ==========================================
// 模式 3：DelimiterFrameHandler（分隔符拆帧）
// ==========================================

bool demo_delimiter_frame() {
    std::cout << "=== [3/3] DelimiterFrameHandler (多字节分隔符 \\r\\n 拆帧) ===\n";

    pollnet::TcpServer<> server;
    if (!server.listen({.host = "127.0.0.1", .port = 0})) {
        std::cerr << std::format("delimiter server listen failed: {}\n", server.lastError());
        return false;
    }

    pollnet::TcpClient<> client;
    if (!client.connect({.host = "127.0.0.1", .port = server.localPort()})) {
        std::cerr << std::format("delimiter client connect failed: {}\n", client.lastError());
        return false;
    }

    struct ServerMsgHandler {
        int connected = 0;
        void onConnect(pollnet::TcpConnection<>&) { ++connected; }
        void onMessage(pollnet::TcpConnection<>& conn, std::span<const uint8_t> frame) {
            std::string_view line{reinterpret_cast<const char*>(frame.data()), frame.size()};
            std::cout << std::format("[delim][server] recv line='{}'\n", line);
            // 回发原始行内容 + 分隔符 "\r\n"
            (void)conn.sendParts(line, std::string_view{"\r\n"});
        }
    } srv_inner;

    struct ClientMsgHandler {
        int connected = 0;
        std::vector<std::string> lines;
        void onConnect(pollnet::TcpConnection<>&) { ++connected; }
        void onMessage(pollnet::TcpConnection<>&, std::span<const uint8_t> frame) {
            std::string line{reinterpret_cast<const char*>(frame.data()), frame.size()};
            std::cout << std::format("[delim][client] echoed line='{}'\n", line);
            lines.push_back(std::move(line));
        }
    } cli_inner;

    // 传入 "\r\n"（也支持单字节如 '\n' 或 FIX 的 '\x01'）；默认剥离分隔符（include_delimiter = false）
    auto srv_framed = pollnet::makeDelimiterHandler("\r\n", srv_inner);
    auto cli_framed = pollnet::makeDelimiterHandler("\r\n", cli_inner);

    if (!poll_until([&]() {
            server.poll(srv_framed);
            client.poll(cli_framed);
            return srv_inner.connected == 1 && cli_inner.connected == 1;
        })) {
        std::cerr << "delimiter: connect timeout\n";
        return false;
    }

    // 刻意把 "\r\n" 跨包切分：先发 "PING\r"，再发 "\nHELLO\r\n"
    if (!client.writeNonblock(std::string_view{"PING\r"})) {
        std::cerr << "delimiter: send first half error\n";
        return false;
    }
    for (int i = 0; i < 10; ++i) {
        server.poll(srv_framed);
        client.poll(cli_framed);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    if (!client.writeNonblock(std::string_view{"\nHELLO\r\n"})) {
        std::cerr << "delimiter: send second half error\n";
        return false;
    }

    if (!poll_until([&]() {
            server.poll(srv_framed);
            client.poll(cli_framed);
            return cli_inner.lines.size() == 2;
        })) {
        std::cerr << "delimiter: reply timeout\n";
        return false;
    }

    return cli_inner.lines[0] == "PING" && cli_inner.lines[1] == "HELLO";
}

}  // namespace

int main(int argc, char** argv) {
    std::string_view mode = (argc >= 2) ? std::string_view{argv[1]} : "all";

    if (mode == "all") {
        if (!demo_fixed_frame() || !demo_length_prefixed() || !demo_delimiter_frame()) {
            return 1;
        }
        std::cout << "All 3 framer demos passed!\n";
        return 0;
    }
    if (mode == "fixed") return demo_fixed_frame() ? 0 : 1;
    if (mode == "length") return demo_length_prefixed() ? 0 : 1;
    if (mode == "delimiter") return demo_delimiter_frame() ? 0 : 1;

    std::cerr << std::format("usage: {} [all|fixed|length|delimiter]\n", argv[0]);
    return 1;
}
