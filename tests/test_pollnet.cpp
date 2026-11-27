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

#include "pollnet/eventloop.h"
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

static_assert(!std::is_copy_constructible_v<pollnet::EventLoopTcpServer<>>);
static_assert(!std::is_copy_assignable_v<pollnet::EventLoopTcpServer<>>);
static_assert(!std::is_move_constructible_v<pollnet::EventLoopTcpServer<>>);
static_assert(!std::is_move_assignable_v<pollnet::EventLoopTcpServer<>>);

static_assert(!std::is_copy_constructible_v<pollnet::UdpSocket<>>);
static_assert(!std::is_copy_assignable_v<pollnet::UdpSocket<>>);
static_assert(!std::is_move_constructible_v<pollnet::UdpSocket<>>);
static_assert(!std::is_move_assignable_v<pollnet::UdpSocket<>>);

// ByteRange 与 IoSlice 约束验证
static_assert(pollnet::ByteRange<std::span<const uint8_t>>);
static_assert(pollnet::ByteRange<std::span<const std::byte>>);
static_assert(pollnet::ByteRange<std::string_view>);
static_assert(pollnet::ByteRange<std::string>);
static_assert(pollnet::ByteRange<std::vector<uint8_t>>);
static_assert(pollnet::ByteRange<std::array<uint8_t, 4>>);
static_assert(!pollnet::ByteRange<const char (&)[6]>);
static_assert(!pollnet::ByteRange<std::array<pollnet::IoSlice, 2>>);

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
    CHECK(server.localPort() != 0);
    CHECK(server.listen({.host = "127.0.0.1", .port = 0}));
    CHECK(server.localPort() != 0);
    CHECK(!server.isClosed());

    pollnet::UdpSocket<> sock;
    CHECK(!sock.open({.bind_ip = "bad"}));
    CHECK(!sock.lastError().empty());
    CHECK(!sock.open({.bind_ip = "127.0.0.1", .dest_ip = "bad"}));
    CHECK(!sock.open({.bind_ip = "127.0.0.1", .multicast_group_ip = "bad"}));
    CHECK(!sock.open({.bind_ip = "127.0.0.1",
                      .multicast_group_ip = "239.0.0.1",
                      .multicast_interface_ip = "bad"}));
    CHECK(sock.isClosed());
    CHECK(sock.localPort() == 0);
    CHECK(sock.open({.bind_ip = "127.0.0.1", .bind_port = 0}));
    CHECK(sock.isOpen());
    CHECK(sock.localPort() != 0);
    CHECK(sock.open({.bind_ip = "127.0.0.1",
                     .bind_port = 0,
                     .dest_ip = "127.0.0.1",
                     .dest_port = 1234}));
    CHECK(sock.isOpen());

    return true;
}

// ==========================================
// 4. 极简 onData-only Handler + 完整 Echo/span/allowNewConnection
// ==========================================

bool test_tcp_minimal_and_full_handler() {
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
// 5. 内核调优参数与统一 IoSlice 向量写测试
// ==========================================

bool test_tcp_tuning_and_vector_write() {
    pollnet::TcpServer<> server;
    CHECK(server.listen({.host = "127.0.0.1",
                         .port = 0,
                         .accept_poll_interval = 8,
                         .tcp_quickack = true,
                         .busy_poll_us = 25,
                         .recv_buf_bytes = 65536,
                         .send_buf_bytes = 65536}));

    pollnet::TcpClient<> client;
    CHECK(client.connect({.host = "127.0.0.1",
                          .port = server.localPort(),
                          .tcp_quickack = true,
                          .busy_poll_us = 25,
                          .recv_buf_bytes = 65536,
                          .send_buf_bytes = 65536}));

    struct SrvHandler {
        std::string recv_all;
        size_t onData(pollnet::TcpConnection<>& conn, std::span<const uint8_t> data) {
            recv_all.append(reinterpret_cast<const char*>(data.data()), data.size());
            const std::string_view p1 = "ack:";
            std::array<pollnet::IoSlice, 2> iov{{{p1}, {data}}};
            [[maybe_unused]] bool ok = conn.writeNonblock(iov);
            return data.size();
        }
    } srv_h;

    struct CliHandler {
        std::string recv_all;
        size_t onData(pollnet::TcpConnection<>&, std::span<const uint8_t> data) {
            recv_all.append(reinterpret_cast<const char*>(data.data()), data.size());
            return data.size();
        }
    } cli_h;

    CHECK(poll_until([&]() {
        server.poll(srv_h);
        client.poll(cli_h);
        return client.isConnected() && server.connCount() == 1;
    }));

    const std::string s1 = "vec-";
    const std::array<uint8_t, 3> s2{'i', 'o', '-'};
    const std::array<std::byte, 2> s3{std::byte{'o'}, std::byte{'k'}};
    std::array<pollnet::IoSlice, 5> slices{{
        {nullptr, 0},
        {s1},
        {s2},
        {nullptr, 0},
        {s3},
    }};
    CHECK(client.writeNonblock(slices));

    std::vector<pollnet::IoSlice> many_slices;
    const std::string unit = "X";
    for (int i = 0; i < 70; ++i) {
        many_slices.emplace_back(unit);
    }
    CHECK(client.write(many_slices));

    const std::string expected_srv = "vec-io-ok" + std::string(70, 'X');
    CHECK(poll_until([&]() {
        server.poll(srv_h);
        client.poll(cli_h);
        return srv_h.recv_all == expected_srv && cli_h.recv_all.size() >= expected_srv.size();
    }));

    return true;
}

// ==========================================
// 6. TCP 粘包、半包与 memmove 压缩恢复
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

    std::vector<uint8_t> coalesced;
    for (std::string_view s : {"alpha", "beta", "gamma"}) {
        auto f = make_frame(s);
        coalesced.insert(coalesced.end(), f.begin(), f.end());
    }
    CHECK(client.write(coalesced));
    CHECK(poll_until([&]() {
        server.poll(srv_h);
        return srv_h.frames.size() == 3;
    }));
    CHECK(srv_h.frames[0] == "alpha");
    CHECK(srv_h.frames[1] == "beta");
    CHECK(srv_h.frames[2] == "gamma");

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

    auto f10 = make_frame("123456");
    std::string payload56(56, 'Z');
    auto f60 = make_frame(payload56);
    std::vector<uint8_t> batch;
    batch.insert(batch.end(), f10.begin(), f10.end());
    batch.insert(batch.end(), f60.begin(), f60.begin() + 54);
    CHECK(batch.size() == 64);
    CHECK(client.write(batch));

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
// 7. 非法 consumed 与单包填满 RecvBufSize 防护
// ==========================================

bool test_tcp_bad_consumed_and_buf_full() {
    using SmallServer = pollnet::TcpServer<pollnet::EmptyUserData, 64, 4>;
    using SmallClient = pollnet::TcpClient<pollnet::EmptyUserData, 64>;

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
        CHECK(client.write(fill));
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
// 8. 未监听端口快速失败 + allowReconnect 恢复
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

#ifdef _WIN32
    // Windows TCP 协议栈在未监听回环端口上会重传 SYN，约 2~2.5s 后才上报 WSAECONNREFUSED
    constexpr int kConnectFailBudgetMs = 8000;
#else
    constexpr int kConnectFailBudgetMs = 2000;
#endif

    auto start = std::chrono::steady_clock::now();
    CHECK(poll_until(
        [&]() {
            client.poll(cli_h);
            return cli_h.failed >= 1;
        },
        kConnectFailBudgetMs));
    auto elapsed_ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start)
            .count();
    CHECK(elapsed_ms < kConnectFailBudgetMs);
    CHECK(!client.lastError().empty());
    CHECK(client.lastError() != "connect expired");

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
// 9. 亚秒级运行时超时 + 外部时间戳注入测试
// ==========================================

bool test_tcp_subsecond_timeouts_and_custom_clock() {
    using namespace std::chrono_literals;

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
// 10. MaxConns 上限与断开后槽位复用
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
// 11. EventLoopTcpServer 测试
// ==========================================

bool test_eventloop_backend() {
    using namespace std::chrono_literals;
    using Srv = pollnet::EventLoopTcpServer<pollnet::EmptyUserData, 64, 2>;
    using Cli = pollnet::TcpClient<pollnet::EmptyUserData, 64>;

    {
        Srv server;
        CHECK(server.listen({.host = "127.0.0.1", .port = 0}));
        uint16_t port = server.localPort();
        CHECK(port != 0);

        struct BackendHandler {
            bool allow = false;
            int rejected = 0;
            int connected = 0;
            int disconnected = 0;
            std::vector<std::string> frames;

            bool allowNewConnection(const pollnet::Endpoint&) {
                if (!allow) {
                    ++rejected;
                    return false;
                }
                return true;
            }
            void onConnect(Srv::Conn&) { ++connected; }
            void onDisconnect(Srv::Conn&) { ++disconnected; }
            size_t onData(Srv::Conn& conn, std::span<const uint8_t> data) {
                size_t offset = 0;
                while (data.size() - offset >= sizeof(uint32_t)) {
                    uint32_t len = 0;
                    std::memcpy(&len, data.data() + offset, sizeof(uint32_t));
                    if (data.size() - offset - sizeof(uint32_t) < len) break;
                    frames.emplace_back(
                        reinterpret_cast<const char*>(data.data() + offset + sizeof(uint32_t)),
                        len);
                    auto sub = data.subspan(offset, sizeof(uint32_t) + len);
                    [[maybe_unused]] bool ok = conn.writeNonblock(sub);
                    offset += sizeof(uint32_t) + len;
                }
                return offset;
            }
        } srv_h;

        struct CliHandler {
            int connected = 0;
            int disconnected = 0;
            int failed = 0;
            std::string recv_all;
            void onConnectFailed() { ++failed; }
            void onConnect(Cli::Conn&) { ++connected; }
            void onDisconnect(Cli::Conn&) { ++disconnected; }
            size_t onData(Cli::Conn&, std::span<const uint8_t> data) {
                recv_all.append(reinterpret_cast<const char*>(data.data()), data.size());
                return data.size();
            }
        };

        Cli c_rej;
        CliHandler h_rej;
        CHECK(c_rej.connect({.host = "127.0.0.1", .port = port}));
        CHECK(poll_until([&]() {
            server.poll(srv_h, 0);
            c_rej.poll(h_rej);
            return srv_h.rejected == 1 && (h_rej.disconnected >= 1 || h_rej.failed >= 1);
        }));
        CHECK(server.connCount() == 0);

        srv_h.allow = true;
        Cli c1, c2, c3;
        CliHandler h1, h2, h3;
        CHECK(c1.connect({.host = "127.0.0.1", .port = port}));
        CHECK(c2.connect({.host = "127.0.0.1", .port = port}));

        CHECK(poll_until([&]() {
            server.poll(srv_h, 0);
            c1.poll(h1);
            c2.poll(h2);
            return srv_h.connected == 2 && h1.connected == 1 && h2.connected == 1;
        }));
        CHECK(server.connCount() == 2);

        size_t iter_cnt = 0;
        server.foreachConn([&](Srv::Conn&) { ++iter_cnt; });
        CHECK(iter_cnt == 2);

        auto f1 = make_frame("ev1");
        auto f2 = make_frame("ev2");
        CHECK(c1.write(f1.data(), 2));
        server.poll(srv_h, 0);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
        CHECK(c1.write(f1.data() + 2, f1.size() - 2));
        CHECK(c1.write(f2));
        CHECK(poll_until([&]() {
            server.poll(srv_h, 0);
            c1.poll(h1);
            return srv_h.frames.size() == 2 && h1.recv_all.size() == f1.size() + f2.size();
        }));
        CHECK(srv_h.frames[0] == "ev1");
        CHECK(srv_h.frames[1] == "ev2");

        CHECK(c3.connect({.host = "127.0.0.1", .port = port}));
        for (int i = 0; i < 30; ++i) {
            server.poll(srv_h, 0);
            c3.poll(h3);
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        CHECK(server.connCount() == 2);
        CHECK(srv_h.connected == 2);

        c1.close("release");
        CHECK(poll_until([&]() {
            server.poll(srv_h, 0);
            c3.poll(h3);
            return srv_h.disconnected == 1 && srv_h.connected == 3 && h3.connected == 1;
        }));
        CHECK(server.connCount() == 2);

        auto f3 = make_frame("c3-ok");
        CHECK(c3.write(f3));
        CHECK(poll_until([&]() {
            server.poll(srv_h, 0);
            c3.poll(h3);
            return srv_h.frames.size() == 3 && h3.recv_all.size() == f3.size();
        }));
        CHECK(srv_h.frames[2] == "c3-ok");
    }

    {
        Srv server;
        CHECK(server.listen({.host = "127.0.0.1", .port = 0, .recv_timeout = 120ms}));
        uint16_t port = server.localPort();

        Cli client;
        CHECK(client.connect({.host = "127.0.0.1", .port = port}));

        struct TimeoutSrvHandler {
            int connected = 0;
            int recv_timeouts = 0;
            int disconnected = 0;
            void onConnect(Srv::Conn&) { ++connected; }
            void onDisconnect(Srv::Conn&) { ++disconnected; }
            void onRecvTimeout(Srv::Conn& c) {
                ++recv_timeouts;
                c.close("timeout");
            }
            size_t onData(Srv::Conn&, std::span<const uint8_t> data) { return data.size(); }
        } srv_h;

        struct IdleCli {
            int connected = 0;
            int disconnected = 0;
            void onConnect(Cli::Conn&) { ++connected; }
            void onDisconnect(Cli::Conn&) { ++disconnected; }
            size_t onData(Cli::Conn&, std::span<const uint8_t> data) { return data.size(); }
        } cli_h;

        CHECK(poll_until([&]() {
            server.poll(srv_h, 0);
            client.poll(cli_h);
            return srv_h.connected == 1 && cli_h.connected == 1;
        }));

        CHECK(poll_until([&]() {
            server.poll(srv_h, 5);
            client.poll(cli_h);
            return srv_h.recv_timeouts >= 1 && srv_h.disconnected == 1 && cli_h.disconnected == 1;
        }));
        CHECK(server.connCount() == 0);

        Cli client2;
        IdleCli cli_h2;
        CHECK(client2.connect({.host = "127.0.0.1", .port = port}));
        CHECK(poll_until([&]() {
            server.poll(srv_h, 5);
            client2.poll(cli_h2);
            return srv_h.connected == 2 && cli_h2.connected == 1;
        }));
    }

    return true;
}

// ==========================================
// 12. 统一 UdpSocket 单包与批量收发测试
// ==========================================

bool test_udp_loopback_and_batch() {
    pollnet::UdpSocket<1500> rx1;
    pollnet::UdpSocket<1500> rx2;
    CHECK(rx1.open({.bind_ip = "127.0.0.1", .bind_port = 0}));
    CHECK(rx2.open({.bind_ip = "127.0.0.1", .bind_port = 0}));

    uint16_t rx1_port = rx1.localPort();
    uint16_t rx2_port = rx2.localPort();
    CHECK(rx1_port != 0 && rx2_port != 0);

    pollnet::UdpSocket<1500> tx;
    CHECK(tx.open({.bind_ip = "127.0.0.1",
                   .bind_port = 0,
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
    CHECK(tx.send(byte_payload));

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
    CHECK(rx1.sendTo(pong_bytes, dst_rx2));

    std::string rx2_msg;
    CHECK(poll_until([&]() {
        return rx2.recv([&](std::span<const uint8_t> data) {
            rx2_msg.assign(reinterpret_cast<const char*>(data.data()), data.size());
        });
    }));
    CHECK(rx2_msg == pong);

    const std::array<std::string, 4> batch_msgs = {"b0", "b1", "b2", "b3"};
    std::array<pollnet::IoSlice, 4> batch_slices{
        batch_msgs[0], batch_msgs[1], batch_msgs[2], batch_msgs[3]};
    CHECK(tx.sendBatch(batch_slices) == 4);

    std::vector<std::string> got_batch;
    CHECK(poll_until([&]() {
        [[maybe_unused]] size_t n = rx1.recvBatch<8>([&](std::span<const uint8_t> data) {
            got_batch.emplace_back(reinterpret_cast<const char*>(data.data()), data.size());
        });
        return got_batch.size() == 4;
    }));
    CHECK(got_batch[0] == "b0" && got_batch[3] == "b3");

    CHECK(rx1.sendToBatch(batch_slices, dst_rx2) == 4);
    std::vector<std::string> got_batch2;
    CHECK(poll_until([&]() {
        [[maybe_unused]] size_t n = rx2.recvFromBatch<8>(
            [&](std::span<const uint8_t> data, const pollnet::Endpoint& peer) {
                if (peer.port() == rx1_port) {
                    got_batch2.emplace_back(reinterpret_cast<const char*>(data.data()),
                                            data.size());
                }
            });
        return got_batch2.size() == 4;
    }));
    CHECK(got_batch2[1] == "b1" && got_batch2[2] == "b2");

    return true;
}

// ==========================================
// 13. 线程绑核与低延迟配置预设测试
// ==========================================

bool test_pin_thread_and_low_latency_presets() {
    using namespace std::chrono_literals;

    static_assert(std::is_aggregate_v<pollnet::TcpClientOptions>);
    static_assert(std::is_aggregate_v<pollnet::TcpServerOptions>);
    static_assert(std::is_aggregate_v<pollnet::UdpOptions>);

    auto cli_opts = pollnet::TcpClientOptions::lowLatency("127.0.0.1", 9001);
    CHECK(cli_opts.host == "127.0.0.1" && cli_opts.port == 9001);
    CHECK(cli_opts.retry_interval == 1000ms && cli_opts.connect_timeout == 3000ms);
    CHECK(cli_opts.tcp_quickack && cli_opts.busy_poll_us == 50);
    CHECK(cli_opts.recv_buf_bytes == 256 * 1024 && cli_opts.send_buf_bytes == 256 * 1024);

    auto srv_opts = pollnet::TcpServerOptions::lowLatency("127.0.0.1", 9002);
    CHECK(srv_opts.host == "127.0.0.1" && srv_opts.port == 9002);
    CHECK(srv_opts.tcp_quickack && srv_opts.busy_poll_us == 50);
    CHECK(srv_opts.recv_buf_bytes == 256 * 1024 && srv_opts.send_buf_bytes == 256 * 1024);

    auto udp_rx = pollnet::UdpOptions::lowLatencyRecv("0.0.0.0", 9003, "239.255.0.1", "0.0.0.0");
    CHECK(udp_rx.bind_port == 9003 && udp_rx.multicast_group_ip == "239.255.0.1");
    CHECK(udp_rx.busy_poll_us == 50 && udp_rx.recv_buf_bytes == 1024 * 1024);

    auto udp_tx = pollnet::UdpOptions::lowLatencySend("127.0.0.1", 9003, 9004);
    CHECK(udp_tx.bind_port == 9004 && udp_tx.dest_ip == "127.0.0.1" && udp_tx.dest_port == 9003);
    CHECK(udp_tx.multicast_ttl == 1 && udp_tx.send_buf_bytes == 256 * 1024);

    CHECK(!pollnet::pinThread(-1));
    CHECK(!pollnet::pinThread(100000));
#ifdef __linux__
    int cur_cpu = ::sched_getcpu();
    if (cur_cpu >= 0) {
        CHECK(pollnet::pinThread(cur_cpu));
    }
#elif defined(_WIN32)
    CHECK(pollnet::pinThread(0));
#endif

    return true;
}

// ==========================================
// 14. 编译期拆帧器、sendParts 与 UDP 结构体收发测试
// ==========================================

bool test_framers_and_send_parts_and_udp_struct() {
    struct Tick {
        uint32_t seq;
        uint64_t price;
    };
    struct LenHdr {
        uint32_t len_be;
    };

    static_assert(pollnet::TcpConnection<>::kRecvBufSize == 4096);
    static_assert(pollnet::TcpConnection<>::kSendBufSize == 0);
    static_assert(pollnet::SendPart<pollnet::IoSlice>);
    static_assert(pollnet::SendPart<std::string_view>);
    static_assert(pollnet::SendPart<Tick>);
    static_assert(!pollnet::SendPart<const char*>);
    static_assert(!pollnet::SendPart<const char (&)[6]>);
    static_assert(!pollnet::SendPart<std::span<const pollnet::IoSlice>>);

    struct MinimalTickHandler {
        std::vector<Tick> ticks;
        void onMessage(pollnet::TcpConnection<>& conn, const Tick& t) {
            ticks.push_back(t);
            (void)conn.sendParts(t);
        }
    };
    using MinTickAdapter = pollnet::FixedFrameHandler<Tick, MinimalTickHandler>;
    static_assert(pollnet::TcpHandler<MinTickAdapter, pollnet::TcpConnection<>>);
    static_assert(!pollnet::HasOnConnect<MinTickAdapter, pollnet::TcpConnection<>>);
    static_assert(!pollnet::HasOnDisconnect<MinTickAdapter, pollnet::TcpConnection<>>);
    static_assert(!pollnet::HasAllowNewConnection<MinTickAdapter>);

    // 14a. FixedFrameHandler + sendParts（验证粘包 + 半包）
    {
        pollnet::TcpServer<> server;
        CHECK(server.listen({.host = "127.0.0.1", .port = 0}));
        pollnet::TcpClient<> client;
        CHECK(client.connect({.host = "127.0.0.1", .port = server.localPort()}));

        struct SrvTickHandler {
            int connected = 0;
            bool send_ok = true;
            std::vector<Tick> rx;
            bool allowNewConnection(const pollnet::Endpoint&) { return true; }
            void onConnect(pollnet::TcpConnection<>&) { ++connected; }
            void onMessage(pollnet::TcpConnection<>& conn, const Tick& t) {
                rx.push_back(t);
                send_ok = send_ok && conn.sendParts(t);
            }
        } srv_inner;

        struct CliTickHandler {
            int connected = 0;
            std::vector<Tick> echoed;
            void onConnect(pollnet::TcpConnection<>&) { ++connected; }
            void onMessage(pollnet::TcpConnection<>&, const Tick& t) { echoed.push_back(t); }
        } cli_inner;

        auto srv_h = pollnet::makeFixedFrameHandler<Tick>(srv_inner);
        auto cli_h = pollnet::makeFixedFrameHandler<Tick>(cli_inner);
        static_assert(pollnet::HasOnConnect<decltype(srv_h), pollnet::TcpConnection<>>);
        static_assert(pollnet::HasAllowNewConnection<decltype(srv_h)>);

        CHECK(poll_until([&]() {
            server.poll(srv_h);
            client.poll(cli_h);
            return srv_inner.connected == 1 && cli_inner.connected == 1;
        }));

        Tick t1{1, 1001}, t2{2, 2002}, t3{3, 3003};
        CHECK(client.sendParts(t1, t2));
        CHECK(client.writeNonblock(&t3, 4));
        CHECK(poll_until([&]() {
            server.poll(srv_h);
            client.poll(cli_h);
            return srv_inner.rx.size() == 2 && cli_inner.echoed.size() == 2;
        }));
        CHECK(client.writeNonblock(reinterpret_cast<const uint8_t*>(&t3) + 4, sizeof(Tick) - 4));
        CHECK(poll_until([&]() {
            server.poll(srv_h);
            client.poll(cli_h);
            return srv_inner.rx.size() == 3 && cli_inner.echoed.size() == 3;
        }));
        CHECK(srv_inner.send_ok);
        CHECK(cli_inner.echoed[0].seq == 1 && cli_inner.echoed[1].price == 2002 &&
              cli_inner.echoed[2].seq == 3);
    }

    // 14b. LengthPrefixedHandler（两种 onMessage 签名 + 超大包防护）
    {
        pollnet::TcpServer<pollnet::EmptyUserData, 256, 4> server;
        CHECK(server.listen({.host = "127.0.0.1", .port = 0}));
        pollnet::TcpClient<pollnet::EmptyUserData, 256> client;
        CHECK(client.connect({.host = "127.0.0.1", .port = server.localPort()}));

        auto extract_len = [](const LenHdr& h) { return ntohl(h.len_be); };
        struct SrvLenHandler {
            int connected = 0;
            int disconnected = 0;
            bool send_ok = true;
            std::string reason;
            std::vector<std::string> rx;
            void onConnect(pollnet::TcpConnection<pollnet::EmptyUserData, 256>&) { ++connected; }
            void onDisconnect(pollnet::TcpConnection<pollnet::EmptyUserData, 256>& c) {
                ++disconnected;
                reason = std::string(c.lastError());
            }
            void onMessage(pollnet::TcpConnection<pollnet::EmptyUserData, 256>& conn,
                           const LenHdr& hdr, std::span<const uint8_t> body) {
                rx.emplace_back(reinterpret_cast<const char*>(body.data()), body.size());
                send_ok = send_ok && conn.sendParts(hdr, body);
            }
        } srv_inner;

        struct CliLenHandler {
            int connected = 0;
            std::vector<std::string> echoed;
            void onConnect(pollnet::TcpConnection<pollnet::EmptyUserData, 256>&) { ++connected; }
            void onMessage(pollnet::TcpConnection<pollnet::EmptyUserData, 256>&,
                           std::span<const uint8_t> body) {
                echoed.emplace_back(reinterpret_cast<const char*>(body.data()), body.size());
            }
        } cli_inner;

        auto srv_h = pollnet::makeLengthPrefixedHandler<LenHdr>(extract_len, srv_inner);
        auto cli_h = pollnet::makeLengthPrefixedHandler<LenHdr>(extract_len, cli_inner);

        CHECK(poll_until([&]() {
            server.poll(srv_h);
            client.poll(cli_h);
            return srv_inner.connected == 1 && cli_inner.connected == 1;
        }));

        const std::string_view m1 = "alpha";
        const std::string_view m2 = "beta-msg";
        LenHdr h1{htonl(static_cast<uint32_t>(m1.size()))};
        LenHdr h2{htonl(static_cast<uint32_t>(m2.size()))};
        CHECK(client.sendParts(h1, m1, h2, m2));

        CHECK(poll_until([&]() {
            server.poll(srv_h);
            client.poll(cli_h);
            return srv_inner.rx.size() == 2 && cli_inner.echoed.size() == 2;
        }));
        CHECK(srv_inner.send_ok);
        CHECK(cli_inner.echoed[0] == "alpha" && cli_inner.echoed[1] == "beta-msg");

        LenHdr bad_hdr{htonl(300)};
        CHECK(client.sendParts(bad_hdr));
        CHECK(poll_until([&]() {
            server.poll(srv_h);
            return srv_inner.disconnected == 1;
        }));
        CHECK(srv_inner.reason == "message too large");
    }

    // 14c. UdpSocket sendStruct / sendStructTo / recvStruct
    {
        pollnet::UdpSocket<1500> rx1;
        pollnet::UdpSocket<1500> rx2;
        CHECK(rx1.open({.bind_ip = "127.0.0.1", .bind_port = 0}));
        CHECK(rx2.open({.bind_ip = "127.0.0.1", .bind_port = 0}));

        pollnet::UdpSocket<1500> tx;
        CHECK(tx.open({.bind_ip = "127.0.0.1",
                       .bind_port = 0,
                       .dest_ip = "127.0.0.1",
                       .dest_port = rx1.localPort()}));

        Tick pkt1{77, 123456};
        CHECK(tx.sendStruct(pkt1));

        Tick got1{};
        pollnet::Endpoint from_ep;
        CHECK(poll_until([&]() {
            return rx1.recvStruct<Tick>([&](const Tick& t, const pollnet::Endpoint& peer) {
                got1 = t;
                from_ep = peer;
            });
        }));
        CHECK(got1.seq == 77 && got1.price == 123456 && from_ep.port() == tx.localPort());

        // 发送尺寸不匹配的数据报，recvStruct 应忽略并返回 false
        CHECK(tx.send("tiny", 4));
        for (int i = 0; i < 5; ++i) {
            bool parsed = rx1.recvStruct<Tick>([&](const Tick&) {});
            CHECK(!parsed);
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }

        Tick pkt2{88, 654321};
        CHECK(rx1.sendStructTo(pkt2, pollnet::Endpoint::loopback(rx2.localPort())));
        Tick got2{};
        CHECK(poll_until([&]() {
            return rx2.recvStruct<Tick>([&](const Tick& t) { got2 = t; });
        }));
        CHECK(got2.seq == 88 && got2.price == 654321);
    }

    // 14d. DelimiterFrameHandler（单字节/多字节跨包分隔符、include_delimiter、超限及非法分隔符）
    {
        struct MinimalDelimHandler {
            void onMessage(pollnet::TcpConnection<>&, std::span<const uint8_t>) {}
        };
        using MinDelimAdapter = pollnet::DelimiterFrameHandler<MinimalDelimHandler>;
        static_assert(pollnet::TcpHandler<MinDelimAdapter, pollnet::TcpConnection<>>);
        static_assert(!pollnet::HasOnConnect<MinDelimAdapter, pollnet::TcpConnection<>>);
        static_assert(!pollnet::HasOnDisconnect<MinDelimAdapter, pollnet::TcpConnection<>>);
        static_assert(!pollnet::HasAllowNewConnection<MinDelimAdapter>);

        // 单字节 '\n'：粘包、空帧与半包
        {
            pollnet::TcpServer<> server;
            CHECK(server.listen({.host = "127.0.0.1", .port = 0}));
            pollnet::TcpClient<> client;
            CHECK(client.connect({.host = "127.0.0.1", .port = server.localPort()}));

            struct SrvDelim {
                int connected = 0;
                std::vector<std::string> frames;
                void onConnect(pollnet::TcpConnection<>&) { ++connected; }
                void onMessage(pollnet::TcpConnection<>& conn, std::span<const uint8_t> frame) {
                    std::string s(reinterpret_cast<const char*>(frame.data()), frame.size());
                    frames.push_back(s);
                    (void)conn.sendParts(s, std::string_view{"\n"});
                }
            } srv_inner;
            struct CliDelim {
                int connected = 0;
                std::vector<std::string> frames;
                void onConnect(pollnet::TcpConnection<>&) { ++connected; }
                void onMessage(pollnet::TcpConnection<>&, std::span<const uint8_t> frame) {
                    frames.emplace_back(reinterpret_cast<const char*>(frame.data()), frame.size());
                }
            } cli_inner;

            auto srv_h = pollnet::makeDelimiterHandler('\n', srv_inner);
            auto cli_h = pollnet::makeDelimiterHandler('\n', cli_inner);
            static_assert(pollnet::HasOnConnect<decltype(srv_h), pollnet::TcpConnection<>>);

            CHECK(poll_until([&]() {
                server.poll(srv_h);
                client.poll(cli_h);
                return srv_inner.connected == 1 && cli_inner.connected == 1;
            }));

            CHECK(client.writeNonblock(std::string_view{"a\n\nbc"}));
            CHECK(poll_until([&]() {
                server.poll(srv_h);
                client.poll(cli_h);
                return srv_inner.frames.size() == 2 && cli_inner.frames.size() == 2;
            }));
            CHECK(client.writeNonblock(std::string_view{"\n"}));
            CHECK(poll_until([&]() {
                server.poll(srv_h);
                client.poll(cli_h);
                return srv_inner.frames.size() == 3 && cli_inner.frames.size() == 3;
            }));
            CHECK(cli_inner.frames[0] == "a" && cli_inner.frames[1].empty() &&
                  cli_inner.frames[2] == "bc");
        }

        // 多字节 "\r\n" + include_delimiter = true + max_frame_len = 8 超限校验
        {
            pollnet::TcpServer<> server;
            CHECK(server.listen({.host = "127.0.0.1", .port = 0}));
            pollnet::TcpClient<> client;
            CHECK(client.connect({.host = "127.0.0.1", .port = server.localPort()}));

            struct SrvMulti {
                int connected = 0;
                int disconnected = 0;
                std::string reason;
                std::vector<std::string> frames;
                void onConnect(pollnet::TcpConnection<>&) { ++connected; }
                void onDisconnect(pollnet::TcpConnection<>& c) {
                    ++disconnected;
                    reason = std::string(c.lastError());
                }
                void onMessage(pollnet::TcpConnection<>&, std::span<const uint8_t> frame) {
                    frames.emplace_back(reinterpret_cast<const char*>(frame.data()), frame.size());
                }
            } srv_inner;
            struct IdleCli {
                int connected = 0;
                void onConnect(pollnet::TcpConnection<>&) { ++connected; }
                size_t onData(pollnet::TcpConnection<>&, std::span<const uint8_t> d) {
                    return d.size();
                }
            } cli_h;

            auto srv_h = pollnet::makeDelimiterHandler("\r\n", srv_inner, 8, true);
            CHECK(poll_until([&]() {
                server.poll(srv_h);
                client.poll(cli_h);
                return srv_inner.connected == 1 && cli_h.connected == 1;
            }));

            CHECK(client.writeNonblock(std::string_view{"hi\r"}));
            CHECK(poll_until([&]() {
                server.poll(srv_h);
                return server.connCount() == 1 && srv_inner.frames.empty();
            }));
            CHECK(client.writeNonblock(std::string_view{"\n"}));
            CHECK(poll_until([&]() {
                server.poll(srv_h);
                return srv_inner.frames.size() == 1;
            }));
            CHECK(srv_inner.frames[0] == "hi\r\n");

            CHECK(client.writeNonblock(std::string_view{"12345678"}));
            CHECK(poll_until([&]() {
                server.poll(srv_h);
                return srv_inner.disconnected == 1;
            }));
            CHECK(srv_inner.reason == "frame too large");
        }

        // 空分隔符应立即关闭连接并报错
        {
            pollnet::TcpServer<> server;
            CHECK(server.listen({.host = "127.0.0.1", .port = 0}));
            pollnet::TcpClient<> client;
            CHECK(client.connect({.host = "127.0.0.1", .port = server.localPort()}));

            struct BadDelimSrv {
                int connected = 0;
                int disconnected = 0;
                std::string reason;
                void onConnect(pollnet::TcpConnection<>&) { ++connected; }
                void onDisconnect(pollnet::TcpConnection<>& c) {
                    ++disconnected;
                    reason = std::string(c.lastError());
                }
                void onMessage(pollnet::TcpConnection<>&, std::span<const uint8_t>) {}
            } srv_inner;
            struct IdleCli {
                int connected = 0;
                void onConnect(pollnet::TcpConnection<>&) { ++connected; }
                size_t onData(pollnet::TcpConnection<>&, std::span<const uint8_t> d) {
                    return d.size();
                }
            } cli_h;

            auto srv_h = pollnet::makeDelimiterHandler("", srv_inner);
            CHECK(poll_until([&]() {
                server.poll(srv_h);
                client.poll(cli_h);
                return srv_inner.connected == 1 && cli_h.connected == 1;
            }));

            CHECK(client.writeNonblock(std::string_view{"x"}));
            CHECK(poll_until([&]() {
                server.poll(srv_h);
                return srv_inner.disconnected == 1;
            }));
            CHECK(srv_inner.reason == "invalid delimiter");
        }
    }

    return true;
}

// ==========================================
// 15. 静态发送缓冲排队与无损背压测试
// ==========================================

bool test_static_send_buffer_backpressure() {
    constexpr size_t kChunkBytes = 256;
    constexpr size_t kSendBufCap = 4096;

    auto make_chunk = [&](uint32_t seq) {
        std::array<uint8_t, kChunkBytes> buf{};
        buf.fill(static_cast<uint8_t>((seq & 0xFF) ^ 0x5A));
        std::memcpy(buf.data(), &seq, sizeof(seq));
        return buf;
    };

    auto verify_chunks = [&](const std::vector<uint8_t>& bytes, size_t expected_cnt) -> bool {
        if (bytes.size() != expected_cnt * kChunkBytes) return false;
        for (size_t i = 0; i < expected_cnt; ++i) {
            uint32_t seq = 0;
            std::memcpy(&seq, bytes.data() + i * kChunkBytes, sizeof(seq));
            if (seq != static_cast<uint32_t>(i)) return false;
            uint8_t pat = static_cast<uint8_t>((seq & 0xFF) ^ 0x5A);
            for (size_t k = sizeof(uint32_t); k < kChunkBytes; ++k) {
                if (bytes[i * kChunkBytes + k] != pat) return false;
            }
        }
        return true;
    };

    // 15a. SocketTcpClient (SendBufSize=4096) 队列积压、无损背压与恢复续发
    {
        pollnet::TcpServer<pollnet::EmptyUserData, 4096, 4> server;
        CHECK(server.listen(
            {.host = "127.0.0.1", .port = 0, .recv_buf_bytes = 4096, .send_buf_bytes = 4096}));

        pollnet::TcpClient<pollnet::EmptyUserData, 4096, kSendBufCap> client;
        CHECK(client.connect({.host = "127.0.0.1",
                              .port = server.localPort(),
                              .recv_buf_bytes = 4096,
                              .send_buf_bytes = 4096}));

        struct SrvRx {
            int connected = 0;
            std::vector<uint8_t> bytes;
            void onConnect(pollnet::TcpConnection<>&) { ++connected; }
            size_t onData(pollnet::TcpConnection<>&, std::span<const uint8_t> data) {
                bytes.insert(bytes.end(), data.begin(), data.end());
                return data.size();
            }
        } srv_h;

        struct CliRx {
            int connected = 0;
            void onConnect(pollnet::TcpConnection<pollnet::EmptyUserData, 4096, kSendBufCap>&) {
                ++connected;
            }
            size_t onData(pollnet::TcpConnection<pollnet::EmptyUserData, 4096, kSendBufCap>&,
                          std::span<const uint8_t> data) {
                return data.size();
            }
        } cli_h;

        CHECK(poll_until([&]() {
            server.poll(srv_h);
            client.poll(cli_h);
            return srv_h.connected == 1 && cli_h.connected == 1;
        }));
        CHECK(client.pendingSendBytes() == 0 && client.freeSendBytes() == kSendBufCap);

        // 停止 server.poll 制造对端消费停滞，直到触发静态发送缓冲满背压
        size_t accepted = 0;
        bool hit_backpressure = false;
        for (size_t i = 0; i < 32768; ++i) {
            auto chunk = make_chunk(static_cast<uint32_t>(i));
            if (client.writeNonblock(chunk)) {
                ++accepted;
            } else {
                hit_backpressure = true;
                break;
            }
        }
        CHECK(hit_backpressure);
        CHECK(client.isConnected());  // 无损背压：不能因发送缓冲满强行断开连接
        CHECK(client.pendingSendBytes() > 0 && client.freeSendBytes() < kChunkBytes);

        // 恢复消费并将排空后继续追加第二批（验证环形缓冲回绕）
        CHECK(poll_until(
            [&]() {
                server.poll(srv_h);
                client.poll(cli_h);
                return client.pendingSendBytes() == 0 &&
                       srv_h.bytes.size() == accepted * kChunkBytes;
            },
            5000));

        for (size_t j = 0; j < 24; ++j) {
            auto chunk = make_chunk(static_cast<uint32_t>(accepted));
            CHECK(client.writeNonblock(chunk));
            ++accepted;
        }
        CHECK(poll_until(
            [&]() {
                server.poll(srv_h);
                client.poll(cli_h);
                return srv_h.bytes.size() == accepted * kChunkBytes;
            },
            5000));
        CHECK(verify_chunks(srv_h.bytes, accepted));
        CHECK(!client.writeNonblock(static_cast<const void*>(nullptr), 16));
        CHECK(!client.isConnected());
        CHECK(client.lastError() == "invalid send buffer");
    }

    // 15b. EventLoopTcpServer (SendBufSize=4096) 可写事件续发与无损背压
    {
        using EvServer = pollnet::EventLoopTcpServer<pollnet::EmptyUserData, 4096, 4, kSendBufCap>;
        EvServer server;
        CHECK(server.listen(
            {.host = "127.0.0.1", .port = 0, .recv_buf_bytes = 4096, .send_buf_bytes = 4096}));

        pollnet::TcpClient<> client;
        CHECK(client.connect({.host = "127.0.0.1",
                              .port = server.localPort(),
                              .recv_buf_bytes = 4096,
                              .send_buf_bytes = 4096}));

        struct SrvHandler {
            int connected = 0;
            EvServer::Conn* conn = nullptr;
            void onConnect(EvServer::Conn& c) {
                ++connected;
                conn = &c;
            }
            size_t onData(EvServer::Conn&, std::span<const uint8_t> data) { return data.size(); }
        } srv_h;

        struct CliRx {
            int connected = 0;
            std::vector<uint8_t> bytes;
            void onConnect(pollnet::TcpConnection<>&) { ++connected; }
            size_t onData(pollnet::TcpConnection<>&, std::span<const uint8_t> data) {
                bytes.insert(bytes.end(), data.begin(), data.end());
                return data.size();
            }
        } cli_h;

        CHECK(poll_until([&]() {
            server.poll(srv_h, 0);
            client.poll(cli_h);
            return srv_h.connected == 1 && cli_h.connected == 1 && srv_h.conn != nullptr;
        }));

        size_t accepted = 0;
        bool hit_backpressure = false;
        for (size_t i = 0; i < 32768; ++i) {
            auto chunk = make_chunk(static_cast<uint32_t>(i));
            if (srv_h.conn->writeNonblock(chunk)) {
                ++accepted;
            } else {
                hit_backpressure = true;
                break;
            }
        }
        CHECK(hit_backpressure);
        CHECK(srv_h.conn->isConnected());
        CHECK(srv_h.conn->pendingSendBytes() > 0);

        // 对端尚未读时先驱动一次 EventLoop，确保 EPOLLOUT/POLLWRNORM 注册稳定无误关
        server.poll(srv_h, 0);
        CHECK(srv_h.conn->isConnected());

        CHECK(poll_until(
            [&]() {
                server.poll(srv_h, 5);
                client.poll(cli_h);
                return srv_h.conn->pendingSendBytes() == 0 &&
                       cli_h.bytes.size() == accepted * kChunkBytes;
            },
            5000));
        CHECK(verify_chunks(cli_h.bytes, accepted));
    }

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
        {"tcp_tuning_and_vector_write", test_tcp_tuning_and_vector_write},
        {"tcp_framing_and_compaction", test_tcp_framing_and_compaction},
        {"tcp_bad_consumed_and_buf_full", test_tcp_bad_consumed_and_buf_full},
        {"tcp_connect_fail_and_allow_reconnect", test_tcp_connect_fail_and_allow_reconnect},
        {"tcp_subsecond_timeouts_and_custom_clock", test_tcp_subsecond_timeouts_and_custom_clock},
        {"tcp_max_conns_and_slot_reuse", test_tcp_max_conns_and_slot_reuse},
        {"eventloop_backend", test_eventloop_backend},
        {"udp_loopback_and_batch", test_udp_loopback_and_batch},
        {"pin_thread_and_low_latency_presets", test_pin_thread_and_low_latency_presets},
        {"framers_and_send_parts_and_udp_struct", test_framers_and_send_parts_and_udp_struct},
        {"static_send_buffer_backpressure", test_static_send_buffer_backpressure},
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
