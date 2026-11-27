#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <format>
#include <iostream>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <type_traits>
#include <vector>

#include "pollnet/socket.h"

namespace {

int g_failures = 0;

#define CHECK(cond)                                                                     \
    do {                                                                                \
        if (!(cond)) {                                                                  \
            std::cerr << std::format("[FAIL] {}:{} ({})\n", __FILE__, __LINE__, #cond); \
            ++g_failures;                                                               \
            return false;                                                               \
        }                                                                               \
    } while (false)

// ==========================================
// 1. 编译期契约、Rule of Five 与 Concepts
// ==========================================

using DefaultConn = pollnet::TcpConnection<>;

struct MinimalHandler {
    size_t onData(DefaultConn&, std::span<const uint8_t> data) { return data.size(); }
};

struct FullHandler {
    void onConnect(DefaultConn&) {}
    void onDisconnect(DefaultConn&) {}
    void onConnectFailed() {}
    void onSendTimeout(DefaultConn&) {}
    void onRecvTimeout(DefaultConn&) {}
    bool allowNewConnection(const pollnet::Endpoint&) { return true; }
    size_t onData(DefaultConn&, std::span<const uint8_t> data) { return data.size(); }
};

struct InvalidReturnHandler {
    void onData(DefaultConn&, std::span<const uint8_t>) {}
};

// Rule of Five
static_assert(!std::is_copy_constructible_v<pollnet::TcpConnection<>>);
static_assert(!std::is_copy_assignable_v<pollnet::TcpConnection<>>);
static_assert(!std::is_move_constructible_v<pollnet::TcpConnection<>>);
static_assert(!std::is_move_assignable_v<pollnet::TcpConnection<>>);

static_assert(!std::is_copy_constructible_v<pollnet::TcpClient<>>);
static_assert(!std::is_copy_assignable_v<pollnet::TcpClient<>>);
static_assert(!std::is_move_constructible_v<pollnet::TcpClient<>>);
static_assert(!std::is_move_assignable_v<pollnet::TcpClient<>>);

static_assert(!std::is_copy_constructible_v<pollnet::TcpServer<>>);
static_assert(!std::is_copy_assignable_v<pollnet::TcpServer<>>);
static_assert(!std::is_move_constructible_v<pollnet::TcpServer<>>);
static_assert(!std::is_move_assignable_v<pollnet::TcpServer<>>);

static_assert(!std::is_copy_constructible_v<pollnet::UdpSocket<>>);
static_assert(!std::is_copy_assignable_v<pollnet::UdpSocket<>>);
static_assert(!std::is_move_constructible_v<pollnet::UdpSocket<>>);
static_assert(!std::is_move_assignable_v<pollnet::UdpSocket<>>);

static_assert(!std::is_copy_constructible_v<pollnet::UdpReceiver<>>);
static_assert(!std::is_copy_assignable_v<pollnet::UdpReceiver<>>);
static_assert(!std::is_move_constructible_v<pollnet::UdpReceiver<>>);
static_assert(!std::is_move_assignable_v<pollnet::UdpReceiver<>>);

static_assert(!std::is_copy_constructible_v<pollnet::UdpSender>);
static_assert(!std::is_copy_assignable_v<pollnet::UdpSender>);
static_assert(!std::is_move_constructible_v<pollnet::UdpSender>);
static_assert(!std::is_move_assignable_v<pollnet::UdpSender>);

// 最小跨后端契约与可选回调 Concept 验证
static_assert(pollnet::TcpConnectionLike<DefaultConn>);
static_assert(pollnet::TcpConnectionLike<pollnet::TcpClient<>>);
static_assert(pollnet::TcpHandler<MinimalHandler, DefaultConn>);
static_assert(!pollnet::HasOnConnect<MinimalHandler, DefaultConn>);
static_assert(!pollnet::HasOnDisconnect<MinimalHandler, DefaultConn>);
static_assert(!pollnet::HasOnConnectFailed<MinimalHandler>);
static_assert(!pollnet::HasOnSendTimeout<MinimalHandler, DefaultConn>);
static_assert(!pollnet::HasOnRecvTimeout<MinimalHandler, DefaultConn>);
static_assert(!pollnet::HasAllowNewConnection<MinimalHandler>);

static_assert(pollnet::TcpHandler<FullHandler, DefaultConn>);
static_assert(pollnet::HasOnConnect<FullHandler, DefaultConn>);
static_assert(pollnet::HasOnDisconnect<FullHandler, DefaultConn>);
static_assert(pollnet::HasOnConnectFailed<FullHandler>);
static_assert(pollnet::HasOnSendTimeout<FullHandler, DefaultConn>);
static_assert(pollnet::HasOnRecvTimeout<FullHandler, DefaultConn>);
static_assert(pollnet::HasAllowNewConnection<FullHandler>);

static_assert(!pollnet::TcpHandler<InvalidReturnHandler, DefaultConn>);
static_assert(pollnet::UdpDataHandler<decltype([](std::span<const uint8_t>) {})>);
static_assert(pollnet::UdpPacketHandler<decltype([](std::span<const uint8_t>,
                                                    const pollnet::Endpoint&) {})>);

template <typename Pred>
bool poll_until(Pred&& pred, int timeout_ms = 2000) {
    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    while (std::chrono::steady_clock::now() < deadline) {
        if (pred()) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return pred();
}

// ==========================================
// 2. Endpoint 值类型测试
// ==========================================

bool test_endpoint() {
    auto ep = pollnet::Endpoint::from("127.0.0.1", 8080);
    CHECK(ep.has_value());
    CHECK(ep->is_valid());
    CHECK(ep->port() == 8080);
    CHECK(ep->ip() == "127.0.0.1");
    CHECK(ep->to_string() == "127.0.0.1:8080");
    CHECK(*ep == pollnet::Endpoint::loopback(8080));

    auto any_ep = pollnet::Endpoint::any(1234);
    CHECK(any_ep.is_valid());
    CHECK(any_ep.ip() == "0.0.0.0");
    CHECK(any_ep.port() == 1234);
    CHECK(!(*ep == any_ep));

    CHECK(!pollnet::Endpoint::from("", 8080).has_value());
    CHECK(!pollnet::Endpoint::from("bad_ip", 8080).has_value());
    CHECK(!pollnet::Endpoint::from("999.999.999.999", 8080).has_value());
    const std::string embedded_nul("127.0.0.1\0x", 11);
    CHECK(!pollnet::Endpoint::from(embedded_nul, 8080).has_value());

    return true;
}

// ==========================================
// 3. 参数校验与重复 init 幂等测试
// ==========================================

bool test_invalid_options_and_reinit() {
    pollnet::TcpClient<> client;
    CHECK(!client.connect({.host = "", .port = 1234}));
    CHECK(!client.lastError().empty());
    CHECK(!client.connect({.host = "999.999.999.999", .port = 1234}));
    CHECK(client.connect({.host = "127.0.0.1", .port = 1234}));
    CHECK(client.connect({.host = "127.0.0.1", .port = 2345}));

    pollnet::TcpServer<> server;
    CHECK(!server.listen({.host = "invalid_ip", .port = 0}));
    CHECK(!server.lastError().empty());
    CHECK(server.isClosed());
    CHECK(server.localPort() == 0);

    CHECK(server.listen({.host = "127.0.0.1", .port = 0}));
    uint16_t port1 = server.localPort();
    CHECK(port1 != 0);
    CHECK(server.listen({.host = "127.0.0.1", .port = 0}));
    CHECK(server.localPort() != 0);
    CHECK(!server.isClosed());

    pollnet::UdpReceiver<> rx;
    CHECK(!rx.open({.bind_ip = "bad_ip", .bind_port = 0}));
    CHECK(!rx.open({.bind_ip = "127.0.0.1", .bind_port = 0, .multicast_interface_ip = "bad"}));
    CHECK(rx.isClosed());
    CHECK(rx.localPort() == 0);
    CHECK(rx.open({.bind_ip = "127.0.0.1", .bind_port = 0}));
    CHECK(rx.localPort() != 0);
    CHECK(rx.open({.bind_ip = "127.0.0.1", .bind_port = 0}));
    CHECK(!rx.isClosed());

    pollnet::UdpSender tx;
    CHECK(!tx.open({.local_ip = "127.0.0.1", .dest_ip = "", .dest_port = rx.localPort()}));
    CHECK(!tx.open({.local_ip = "bad", .dest_ip = "127.0.0.1", .dest_port = rx.localPort()}));
    CHECK(!tx.open({.local_ip = "127.0.0.1", .dest_ip = "bad", .dest_port = rx.localPort()}));
    CHECK(tx.isClosed());
    CHECK(tx.open({.local_ip = "127.0.0.1", .dest_ip = "127.0.0.1", .dest_port = rx.localPort()}));
    CHECK(tx.localPort() != 0);
    CHECK(tx.open({.local_ip = "127.0.0.1", .dest_ip = "127.0.0.1", .dest_port = rx.localPort()}));
    CHECK(!tx.isClosed());

    pollnet::UdpSocket<> sock;
    CHECK(!sock.open({.bind_ip = "bad"}));
    CHECK(!sock.open({.bind_ip = "127.0.0.1", .dest_ip = "bad"}));
    CHECK(!sock.open({.bind_ip = "127.0.0.1", .multicast_group_ip = "bad"}));
    CHECK(!sock.open({.bind_ip = "127.0.0.1",
                      .multicast_group_ip = "239.0.0.1",
                      .multicast_interface_ip = "bad"}));
    CHECK(sock.isClosed());
    CHECK(sock.open({.bind_ip = "127.0.0.1", .bind_port = 0}));
    CHECK(sock.isOpen());
    CHECK(sock.localPort() != 0);

    return true;
}

// ==========================================
// 4. 极简 onData-only Handler + 完整 Echo/span/allowNewConnection
// ==========================================

bool test_tcp_minimal_and_full_handler() {
    // 4a. 仅实现 onData 的极简 Handler
    {
        pollnet::TcpServer<> server;
        CHECK(server.listen({.host = "127.0.0.1", .port = 0}));
        pollnet::TcpClient<> client;
        CHECK(client.connect({.host = "127.0.0.1", .port = server.localPort()}));

        struct MinimalEcho {
            size_t onData(pollnet::TcpConnection<>& conn, std::span<const uint8_t> data) {
                [[maybe_unused]] bool ok = conn.writeNonblock(data);
                return data.size();
            }
        } srv_h;

        struct MinimalRecv {
            std::string received;
            size_t onData(pollnet::TcpConnection<>&, std::span<const uint8_t> data) {
                received.append(reinterpret_cast<const char*>(data.data()), data.size());
                return data.size();
            }
        } cli_h;

        CHECK(poll_until([&]() {
            server.poll(srv_h);
            client.poll(cli_h);
            return client.isConnected() && server.connCount() == 1;
        }));

        CHECK(client.write("ping", 4));
        CHECK(poll_until([&]() {
            server.poll(srv_h);
            client.poll(cli_h);
            return cli_h.received == "ping";
        }));
    }

    // 4b. allowNewConnection 过滤 + UserData + peer + foreachConn + span
    struct TagData {
        uint64_t tag = 0;
    };
    using TagServer = pollnet::TcpServer<TagData, 4096, 4>;
    using TagClient = pollnet::TcpClient<TagData, 4096>;

    TagServer server;
    CHECK(server.listen({.host = "127.0.0.1", .port = 0}));
    uint16_t port = server.localPort();

    struct FullSrvHandler {
        bool allow = false;
        int rejected = 0;
        int connected = 0;
        int disconnected = 0;
        std::string recv_all;

        bool allowNewConnection(const pollnet::Endpoint& ep) {
            if (!allow) {
                ++rejected;
                return false;
            }
            return ep.is_valid();
        }
        void onConnect(TagServer::Conn& conn) {
            ++connected;
            conn.tag = 42;
            if (conn.peer().has_value()) {
                conn.tag += 1;
            }
        }
        void onDisconnect(TagServer::Conn&) { ++disconnected; }
        size_t onData(TagServer::Conn& conn, std::span<const uint8_t> data) {
            recv_all.append(reinterpret_cast<const char*>(data.data()), data.size());
            [[maybe_unused]] bool ok = conn.writeNonblock(data);
            return data.size();
        }
    } srv_h;

    struct FullCliHandler {
        int connected = 0;
        int disconnected = 0;
        int failed = 0;
        std::string recv_all;

        void onConnectFailed() { ++failed; }
        void onConnect(TagClient::Conn&) { ++connected; }
        void onDisconnect(TagClient::Conn&) { ++disconnected; }
        size_t onData(TagClient::Conn&, std::span<const uint8_t> data) {
            recv_all.append(reinterpret_cast<const char*>(data.data()), data.size());
            return data.size();
        }
    };

    // 先验证 allowNewConnection 返回 false 拒绝连接且不占槽位
    TagClient rejected_client;
    FullCliHandler rejected_h;
    CHECK(rejected_client.connect({.host = "127.0.0.1", .port = port}));
    CHECK(poll_until([&]() {
        server.poll(srv_h);
        rejected_client.poll(rejected_h);
        return srv_h.rejected == 1 && (rejected_h.disconnected >= 1 || rejected_h.failed >= 1);
    }));
    CHECK(server.connCount() == 0);
    CHECK(srv_h.connected == 0);

    // 放行新连接
    srv_h.allow = true;
    TagClient client;
    FullCliHandler cli_h;
    CHECK(client.connect({.host = "127.0.0.1", .port = port}));
    CHECK(poll_until([&]() {
        server.poll(srv_h);
        client.poll(cli_h);
        return srv_h.connected == 1 && cli_h.connected == 1;
    }));

    uint64_t seen_tag = 0;
    server.foreachConn([&](TagServer::Conn& c) { seen_tag = c.tag; });
    CHECK(seen_tag == 43);

    auto srv_peer = client.peer();
    CHECK(srv_peer.has_value());
    CHECK(srv_peer->port() == port);

    const std::string msg1 = "hello-ptr|";
    const std::string msg2 = "hello-u8span|";
    const std::string msg3 = "hello-bytespan";

    CHECK(client.write(msg1.data(), msg1.size()));
    std::vector<uint8_t> u8_buf(msg2.begin(), msg2.end());
    CHECK(client.write(std::span<const uint8_t>{u8_buf}));
    std::vector<std::byte> byte_buf;
    for (char c : msg3) {
        byte_buf.push_back(static_cast<std::byte>(c));
    }
    CHECK(client.writeNonblock(std::span<const std::byte>{byte_buf}));

    const std::string expected = msg1 + msg2 + msg3;
    CHECK(poll_until([&]() {
        server.poll(srv_h);
        client.poll(cli_h);
        return cli_h.recv_all == expected;
    }));
    CHECK(srv_h.recv_all == expected);

    client.close("test done");
    CHECK(poll_until([&]() {
        server.poll(srv_h);
        client.poll(cli_h);
        return srv_h.disconnected == 1;
    }));
    CHECK(server.connCount() == 0);

    return true;
}

// ==========================================
// 5. TCP 粘包、半包与 memmove 压缩恢复
// ==========================================

std::vector<uint8_t> make_frame(std::string_view payload) {
    uint32_t len = static_cast<uint32_t>(payload.size());
    std::vector<uint8_t> out(sizeof(uint32_t) + payload.size());
    std::memcpy(out.data(), &len, sizeof(uint32_t));
    std::memcpy(out.data() + sizeof(uint32_t), payload.data(), payload.size());
    return out;
}

bool test_tcp_framing_and_compaction() {
    using SmallServer = pollnet::TcpServer<pollnet::EmptyUserData, 64, 4>;
    using SmallClient = pollnet::TcpClient<pollnet::EmptyUserData, 64>;

    SmallServer server;
    CHECK(server.listen({.host = "127.0.0.1", .port = 0}));
    SmallClient client;
    CHECK(client.connect({.host = "127.0.0.1", .port = server.localPort()}));

    struct FrameSrvHandler {
        int connected = 0;
        int disconnected = 0;
        std::vector<std::string> frames;

        void onConnect(SmallServer::Conn&) { ++connected; }
        void onDisconnect(SmallServer::Conn&) { ++disconnected; }
        size_t onData(SmallServer::Conn&, std::span<const uint8_t> data) {
            size_t offset = 0;
            while (data.size() - offset >= sizeof(uint32_t)) {
                uint32_t len = 0;
                std::memcpy(&len, data.data() + offset, sizeof(uint32_t));
                if (data.size() - offset - sizeof(uint32_t) < len) {
                    break;
                }
                frames.emplace_back(
                    reinterpret_cast<const char*>(data.data() + offset + sizeof(uint32_t)), len);
                offset += sizeof(uint32_t) + len;
            }
            return offset;
        }
    } srv_h;

    struct IdleCliHandler {
        int connected = 0;
        void onConnect(SmallClient::Conn&) { ++connected; }
        size_t onData(SmallClient::Conn&, std::span<const uint8_t> data) { return data.size(); }
    } cli_h;

    CHECK(poll_until([&]() {
        server.poll(srv_h);
        client.poll(cli_h);
        return srv_h.connected == 1 && cli_h.connected == 1;
    }));

    // 5a. 粘包测试
    std::vector<uint8_t> coalesced;
    for (std::string_view s : {"alpha", "beta", "gamma"}) {
        auto f = make_frame(s);
        coalesced.insert(coalesced.end(), f.begin(), f.end());
    }
    CHECK(client.write(std::span<const uint8_t>{coalesced}));
    CHECK(poll_until([&]() {
        server.poll(srv_h);
        return srv_h.frames.size() == 3;
    }));
    CHECK(srv_h.frames[0] == "alpha");
    CHECK(srv_h.frames[1] == "beta");
    CHECK(srv_h.frames[2] == "gamma");

    // 5b. 逐字节半包测试
    auto frag = make_frame("split-frame-ok");
    for (uint8_t b : frag) {
        CHECK(client.write(&b, 1));
        server.poll(srv_h);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    CHECK(poll_until([&]() {
        server.poll(srv_h);
        return srv_h.frames.size() == 4;
    }));
    CHECK(srv_h.frames[3] == "split-frame-ok");

    // 5c. tail_ == 64 且 0 < head_(10) < 32 时 memmove 压缩恢复
    auto f10 = make_frame("123456");  // 10 字节
    std::string payload56(56, 'Z');
    auto f60 = make_frame(payload56);  // 60 字节
    std::vector<uint8_t> batch;
    batch.insert(batch.end(), f10.begin(), f10.end());
    batch.insert(batch.end(), f60.begin(), f60.begin() + 54);
    CHECK(batch.size() == 64);
    CHECK(client.write(std::span<const uint8_t>{batch}));

    CHECK(poll_until([&]() {
        server.poll(srv_h);
        return srv_h.frames.size() == 5;
    }));
    for (int i = 0; i < 10; ++i) {
        server.poll(srv_h);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    CHECK(srv_h.disconnected == 0);
    CHECK(server.connCount() == 1);

    CHECK(client.write(f60.data() + 54, 6));
    CHECK(poll_until([&]() {
        server.poll(srv_h);
        return srv_h.frames.size() == 6;
    }));
    CHECK(srv_h.frames[4] == "123456");
    CHECK(srv_h.frames[5] == payload56);

    return true;
}

// ==========================================
// 6. 非法 consumed 与单包填满 RecvBufSize 防护
// ==========================================

bool test_tcp_bad_consumed_and_buf_full() {
    using SmallServer = pollnet::TcpServer<pollnet::EmptyUserData, 64, 4>;
    using SmallClient = pollnet::TcpClient<pollnet::EmptyUserData, 64>;

    // 6a. consumed > avail
    {
        SmallServer server;
        CHECK(server.listen({.host = "127.0.0.1", .port = 0}));
        SmallClient client;
        CHECK(client.connect({.host = "127.0.0.1", .port = server.localPort()}));

        struct BadConsumedHandler {
            int connected = 0;
            int disconnected = 0;
            std::string reason;
            void onConnect(SmallServer::Conn&) { ++connected; }
            void onDisconnect(SmallServer::Conn& c) {
                ++disconnected;
                reason = std::string(c.lastError());
            }
            size_t onData(SmallServer::Conn&, std::span<const uint8_t> data) {
                return data.size() + 1;
            }
        } srv_h;

        struct CliHandler {
            int connected = 0;
            void onConnect(SmallClient::Conn&) { ++connected; }
            size_t onData(SmallClient::Conn&, std::span<const uint8_t> data) { return data.size(); }
        } cli_h;

        CHECK(poll_until([&]() {
            server.poll(srv_h);
            client.poll(cli_h);
            return srv_h.connected == 1 && cli_h.connected == 1;
        }));

        CHECK(client.write("abcd", 4));
        CHECK(poll_until([&]() {
            server.poll(srv_h);
            return srv_h.disconnected == 1;
        }));
        CHECK(server.connCount() == 0);
        CHECK(srv_h.reason == "invalid consumed size");
    }

    // 6b. 单包占满 64 字节且不消费触发 recv buf full
    {
        SmallServer server;
        CHECK(server.listen({.host = "127.0.0.1", .port = 0}));
        SmallClient client;
        CHECK(client.connect({.host = "127.0.0.1", .port = server.localPort()}));

        struct FullBufHandler {
            int connected = 0;
            int disconnected = 0;
            std::string reason;
            void onConnect(SmallServer::Conn&) { ++connected; }
            void onDisconnect(SmallServer::Conn& c) {
                ++disconnected;
                reason = std::string(c.lastError());
            }
            size_t onData(SmallServer::Conn&, std::span<const uint8_t>) { return 0; }
        } srv_h;

        struct CliHandler {
            int connected = 0;
            void onConnect(SmallClient::Conn&) { ++connected; }
            size_t onData(SmallClient::Conn&, std::span<const uint8_t> data) { return data.size(); }
        } cli_h;

        CHECK(poll_until([&]() {
            server.poll(srv_h);
            client.poll(cli_h);
            return srv_h.connected == 1 && cli_h.connected == 1;
        }));

        std::array<uint8_t, 64> fill{};
        fill.fill(0x5A);
        CHECK(client.write(std::span<const uint8_t>{fill}));
        CHECK(poll_until([&]() {
            server.poll(srv_h);
            return srv_h.disconnected == 1;
        }));
        CHECK(server.connCount() == 0);
        CHECK(srv_h.reason == "recv buf full");
    }

    return true;
}

// ==========================================
// 7. 未监听端口快速失败 + allowReconnect 恢复
// ==========================================

bool test_tcp_connect_fail_and_allow_reconnect() {
    using namespace std::chrono_literals;

    pollnet::TcpServer<> server;
    CHECK(server.listen({.host = "127.0.0.1", .port = 0}));
    uint16_t port = server.localPort();
    server.close("release port");

    pollnet::TcpClient<> client;
    CHECK(client.connect({.host = "127.0.0.1",
                          .port = port,
                          .retry_interval = 10s,
                          .connect_timeout = 30s}));

    struct CliHandler {
        int failed = 0;
        int connected = 0;
        void onConnectFailed() { ++failed; }
        void onConnect(pollnet::TcpConnection<>&) { ++connected; }
        size_t onData(pollnet::TcpConnection<>&, std::span<const uint8_t> data) {
            return data.size();
        }
    } cli_h;

    auto start = std::chrono::steady_clock::now();
    CHECK(poll_until([&]() {
        client.poll(cli_h);
        return cli_h.failed >= 1;
    }));
    auto elapsed_ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start)
            .count();
    CHECK(elapsed_ms < 2000);
    CHECK(!client.lastError().empty());

    CHECK(server.listen({.host = "127.0.0.1", .port = port}));
    client.allowReconnect();

    struct SrvHandler {
        int connected = 0;
        void onConnect(pollnet::TcpConnection<>&) { ++connected; }
        size_t onData(pollnet::TcpConnection<>&, std::span<const uint8_t> data) {
            return data.size();
        }
    } srv_h;

    CHECK(poll_until([&]() {
        server.poll(srv_h);
        client.poll(cli_h);
        return cli_h.connected == 1 && srv_h.connected == 1;
    }));

    return true;
}

// ==========================================
// 8. 亚秒级运行时超时 + 外部时间戳注入测试
// ==========================================

bool test_tcp_subsecond_timeouts_and_custom_clock() {
    using namespace std::chrono_literals;

    // 8a. 真实时钟 150ms 亚秒级超时
    {
        pollnet::TcpServer<> server;
        CHECK(server.listen({.host = "127.0.0.1", .port = 0, .recv_timeout = 150ms}));

        pollnet::TcpClient<> client;
        CHECK(client.connect({.host = "127.0.0.1",
                              .port = server.localPort(),
                              .send_timeout = 150ms}));

        struct SrvHandler {
            int connected = 0;
            int recv_timeouts = 0;
            int disconnected = 0;
            void onConnect(pollnet::TcpConnection<>&) { ++connected; }
            void onDisconnect(pollnet::TcpConnection<>&) { ++disconnected; }
            void onRecvTimeout(pollnet::TcpConnection<>& c) {
                ++recv_timeouts;
                c.close("recv timeout");
            }
            size_t onData(pollnet::TcpConnection<>&, std::span<const uint8_t> data) {
                return data.size();
            }
        } srv_h;

        struct CliHandler {
            int connected = 0;
            int send_timeouts = 0;
            int disconnected = 0;
            void onConnect(pollnet::TcpConnection<>&) { ++connected; }
            void onDisconnect(pollnet::TcpConnection<>&) { ++disconnected; }
            void onSendTimeout(pollnet::TcpConnection<>&) { ++send_timeouts; }
            size_t onData(pollnet::TcpConnection<>&, std::span<const uint8_t> data) {
                return data.size();
            }
        } cli_h;

        CHECK(poll_until([&]() {
            server.poll(srv_h);
            client.poll(cli_h);
            return srv_h.connected == 1 && cli_h.connected == 1;
        }));

        CHECK(poll_until([&]() {
            server.poll(srv_h);
            client.poll(cli_h);
            return cli_h.send_timeouts >= 1 && srv_h.recv_timeouts >= 1 &&
                   srv_h.disconnected == 1 && cli_h.disconnected == 1;
        }));
    }

    // 8b. 外部注入时间戳 poll(handler, now_ns)
    {
        pollnet::TcpServer<> server;
        CHECK(server.listen({.host = "127.0.0.1", .port = 0, .recv_timeout = 150ms}));

        pollnet::TcpClient<> client;
        CHECK(client.connect({.host = "127.0.0.1",
                              .port = server.localPort(),
                              .send_timeout = 150ms}));

        struct ClockSrvHandler {
            int connected = 0;
            int recv_timeouts = 0;
            void onConnect(pollnet::TcpConnection<>&) { ++connected; }
            void onRecvTimeout(pollnet::TcpConnection<>& c) {
                ++recv_timeouts;
                c.close("clock recv timeout");
            }
            size_t onData(pollnet::TcpConnection<>&, std::span<const uint8_t> data) {
                return data.size();
            }
        } srv_h;

        struct ClockCliHandler {
            int connected = 0;
            int send_timeouts = 0;
            void onConnect(pollnet::TcpConnection<>&) { ++connected; }
            void onSendTimeout(pollnet::TcpConnection<>&) { ++send_timeouts; }
            size_t onData(pollnet::TcpConnection<>&, std::span<const uint8_t> data) {
                return data.size();
            }
        } cli_h;

        constexpr int64_t kBaseNs = 1'000'000'000LL;
        CHECK(poll_until([&]() {
            server.poll(srv_h, kBaseNs);
            client.poll(cli_h, kBaseNs);
            return srv_h.connected == 1 && cli_h.connected == 1;
        }));

        server.poll(srv_h, kBaseNs + 100'000'000LL);
        client.poll(cli_h, kBaseNs + 100'000'000LL);
        CHECK(srv_h.recv_timeouts == 0);
        CHECK(cli_h.send_timeouts == 0);

        server.poll(srv_h, kBaseNs + 200'000'000LL);
        client.poll(cli_h, kBaseNs + 200'000'000LL);
        CHECK(srv_h.recv_timeouts == 1);
        CHECK(cli_h.send_timeouts == 1);
    }

    return true;
}

// ==========================================
// 9. MaxConns 上限与断开后槽位复用
// ==========================================

bool test_tcp_max_conns_and_slot_reuse() {
    using TwoServer = pollnet::TcpServer<pollnet::EmptyUserData, 1024, 2>;
    using TwoClient = pollnet::TcpClient<pollnet::EmptyUserData, 1024>;

    TwoServer server;
    CHECK(server.listen({.host = "127.0.0.1", .port = 0}));
    uint16_t port = server.localPort();

    struct SrvHandler {
        int connected = 0;
        int disconnected = 0;
        std::string last_data;
        void onConnect(TwoServer::Conn&) { ++connected; }
        void onDisconnect(TwoServer::Conn&) { ++disconnected; }
        size_t onData(TwoServer::Conn&, std::span<const uint8_t> data) {
            last_data.assign(reinterpret_cast<const char*>(data.data()), data.size());
            return data.size();
        }
    } srv_h;

    struct CliHandler {
        int connected = 0;
        void onConnect(TwoClient::Conn&) { ++connected; }
        size_t onData(TwoClient::Conn&, std::span<const uint8_t> data) { return data.size(); }
    };

    TwoClient c1, c2, c3;
    CliHandler h1, h2, h3;
    CHECK(c1.connect({.host = "127.0.0.1", .port = port}));
    CHECK(c2.connect({.host = "127.0.0.1", .port = port}));

    CHECK(poll_until([&]() {
        server.poll(srv_h);
        c1.poll(h1);
        c2.poll(h2);
        return srv_h.connected == 2 && h1.connected == 1 && h2.connected == 1;
    }));
    CHECK(server.connCount() == 2);

    CHECK(c3.connect({.host = "127.0.0.1", .port = port}));
    for (int i = 0; i < 30; ++i) {
        server.poll(srv_h);
        c3.poll(h3);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    CHECK(server.connCount() == 2);
    CHECK(srv_h.connected == 2);

    c1.close("release slot");
    CHECK(poll_until([&]() {
        server.poll(srv_h);
        c3.poll(h3);
        return srv_h.disconnected == 1 && srv_h.connected == 3 && h3.connected == 1;
    }));
    CHECK(server.connCount() == 2);

    CHECK(c3.write("slot-reused", 11));
    CHECK(poll_until([&]() {
        server.poll(srv_h);
        return srv_h.last_data == "slot-reused";
    }));

    return true;
}

// ==========================================
// 10. UDP Receiver/Sender 与统一 UdpSocket 回环测试
// ==========================================

bool test_udp_loopback() {
    // 10a. UdpReceiver + UdpSender
    pollnet::UdpReceiver<1500> rx1;
    pollnet::UdpReceiver<1500> rx2;
    CHECK(rx1.open({.bind_ip = "127.0.0.1", .bind_port = 0}));
    CHECK(rx2.open({.bind_ip = "127.0.0.1", .bind_port = 0}));

    uint16_t rx1_port = rx1.localPort();
    uint16_t rx2_port = rx2.localPort();
    CHECK(rx1_port != 0 && rx2_port != 0);

    pollnet::UdpSender tx;
    CHECK(tx.open({.local_ip = "127.0.0.1",
                   .local_port = 0,
                   .dest_ip = "127.0.0.1",
                   .dest_port = rx1_port}));
    uint16_t tx_port = tx.localPort();
    CHECK(tx_port != 0);

    const std::string ping = "udp-ping";
    CHECK(tx.send(ping.data(), ping.size()));

    std::string rx1_msg;
    pollnet::Endpoint seen_peer;
    CHECK(poll_until([&]() {
        return rx1.recvFrom([&](std::span<const uint8_t> data, const pollnet::Endpoint& peer) {
            rx1_msg.assign(reinterpret_cast<const char*>(data.data()), data.size());
            seen_peer = peer;
        });
    }));
    CHECK(rx1_msg == ping);
    CHECK(seen_peer.port() == tx_port);
    CHECK(seen_peer.ip() == "127.0.0.1");

    const std::string ping2 = "udp-span-byte";
    std::vector<std::byte> byte_payload;
    for (char c : ping2) {
        byte_payload.push_back(static_cast<std::byte>(c));
    }
    CHECK(tx.send(std::span<const std::byte>{byte_payload}));

    std::string rx1_msg2;
    CHECK(poll_until([&]() {
        return rx1.recv([&](std::span<const uint8_t> data) {
            rx1_msg2.assign(reinterpret_cast<const char*>(data.data()), data.size());
        });
    }));
    CHECK(rx1_msg2 == ping2);

    auto dst_rx2 = pollnet::Endpoint::loopback(rx2_port);
    const std::string pong = "udp-pong-span";
    std::vector<uint8_t> pong_bytes(pong.begin(), pong.end());
    CHECK(rx1.sendTo(std::span<const uint8_t>{pong_bytes}, dst_rx2));

    std::string rx2_msg;
    CHECK(poll_until([&]() {
        return rx2.recv([&](std::span<const uint8_t> data) {
            rx2_msg.assign(reinterpret_cast<const char*>(data.data()), data.size());
        });
    }));
    CHECK(rx2_msg == pong);

    // 10b. 统一 UdpSocket（双向收发 + connect/send + sendTo/recvFrom）
    pollnet::UdpSocket<1500> u1;
    pollnet::UdpSocket<1500> u2;
    CHECK(u1.open({.bind_ip = "127.0.0.1", .bind_port = 0}));
    CHECK(u2.open({.bind_ip = "127.0.0.1",
                   .bind_port = 0,
                   .dest_ip = "127.0.0.1",
                   .dest_port = u1.localPort()}));

    const std::string u2_to_u1 = "unified-send";
    CHECK(u2.send(u2_to_u1.data(), u2_to_u1.size()));

    std::string u1_got;
    pollnet::Endpoint u2_ep;
    CHECK(poll_until([&]() {
        return u1.recvFrom([&](std::span<const uint8_t> data, const pollnet::Endpoint& peer) {
            u1_got.assign(reinterpret_cast<const char*>(data.data()), data.size());
            u2_ep = peer;
        });
    }));
    CHECK(u1_got == u2_to_u1);
    CHECK(u2_ep.port() == u2.localPort());

    const std::string u1_to_u2 = "unified-sendto";
    CHECK(u1.sendTo(u1_to_u2.data(), u1_to_u2.size(), u2_ep));
    std::string u2_got;
    CHECK(poll_until([&]() {
        return u2.recv([&](std::span<const uint8_t> data) {
            u2_got.assign(reinterpret_cast<const char*>(data.data()), data.size());
        });
    }));
    CHECK(u2_got == u1_to_u2);

    return true;
}

}  // namespace

int main() {
    struct TestCase {
        const char* name;
        bool (*fn)();
    };

    const TestCase cases[] = {
        {"endpoint", test_endpoint},
        {"invalid_options_and_reinit", test_invalid_options_and_reinit},
        {"tcp_minimal_and_full_handler", test_tcp_minimal_and_full_handler},
        {"tcp_framing_and_compaction", test_tcp_framing_and_compaction},
        {"tcp_bad_consumed_and_buf_full", test_tcp_bad_consumed_and_buf_full},
        {"tcp_connect_fail_and_allow_reconnect", test_tcp_connect_fail_and_allow_reconnect},
        {"tcp_subsecond_timeouts_and_custom_clock", test_tcp_subsecond_timeouts_and_custom_clock},
        {"tcp_max_conns_and_slot_reuse", test_tcp_max_conns_and_slot_reuse},
        {"udp_loopback", test_udp_loopback},
    };

    for (const auto& tc : cases) {
        bool ok = tc.fn();
        std::cout << std::format("[{}] {}\n", ok ? "PASS" : "FAIL", tc.name);
    }

    if (g_failures != 0) {
        std::cerr << std::format("Total failures: {}\n", g_failures);
        return 1;
    }

    std::cout << "All pollnet tests passed!\n";
    return 0;
}
