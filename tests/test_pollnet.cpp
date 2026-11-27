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

#define CHECK(cond)                                                                      \
    do {                                                                                 \
        if (!(cond)) {                                                                   \
            std::cerr << std::format("[FAIL] {}:{} ({})\n", __FILE__, __LINE__, #cond);  \
            ++g_failures;                                                                \
            return false;                                                                \
        }                                                                                \
    } while (false)

// ==========================================
// 1. 编译期契约与 C++20 Concept 验证
// ==========================================

struct DefaultConf {
    static constexpr uint32_t RecvBufSize = 4096;
    static constexpr uint32_t MaxConns = 8;
    static constexpr uint32_t ConnRetrySec = 1;
    static constexpr uint32_t ConnTimeoutSec = 5;
    static constexpr uint32_t SendTimeoutSec = 0;
    static constexpr uint32_t RecvTimeoutSec = 0;
    struct UserData {};
};

struct ZeroBufConf {
    static constexpr uint32_t RecvBufSize = 0;
    static constexpr uint32_t SendTimeoutSec = 0;
    static constexpr uint32_t RecvTimeoutSec = 0;
    struct UserData {};
};

struct MissingMaxConnsConf {
    static constexpr uint32_t RecvBufSize = 1024;
    static constexpr uint32_t SendTimeoutSec = 0;
    static constexpr uint32_t RecvTimeoutSec = 0;
    struct UserData {};
};

struct ValidClientHandler {
    void onTcpConnectFailed() {}
    void onTcpConnected(pollnet::SocketTcpConnection<DefaultConf>&) {}
    void onTcpDisconnect(pollnet::SocketTcpConnection<DefaultConf>&) {}
    void onSendTimeout(pollnet::SocketTcpConnection<DefaultConf>&) {}
    void onRecvTimeout(pollnet::SocketTcpConnection<DefaultConf>&) {}
    uint32_t onTcpData(pollnet::SocketTcpConnection<DefaultConf>&, const uint8_t*, uint32_t) {
        return 0;
    }
};

struct InvalidClientHandler {
    void onTcpConnectFailed() {}
    void onTcpConnected(pollnet::SocketTcpConnection<DefaultConf>&) {}
    void onTcpDisconnect(pollnet::SocketTcpConnection<DefaultConf>&) {}
    void onSendTimeout(pollnet::SocketTcpConnection<DefaultConf>&) {}
    void onRecvTimeout(pollnet::SocketTcpConnection<DefaultConf>&) {}
    void onTcpData(pollnet::SocketTcpConnection<DefaultConf>&, const uint8_t*, uint32_t) {}
};

// Rule of Five: 全部不可拷贝、不可移动
static_assert(!std::is_copy_constructible_v<pollnet::SocketTcpConnection<DefaultConf>>);
static_assert(!std::is_copy_assignable_v<pollnet::SocketTcpConnection<DefaultConf>>);
static_assert(!std::is_move_constructible_v<pollnet::SocketTcpConnection<DefaultConf>>);
static_assert(!std::is_move_assignable_v<pollnet::SocketTcpConnection<DefaultConf>>);

static_assert(!std::is_copy_constructible_v<pollnet::SocketTcpClient<DefaultConf>>);
static_assert(!std::is_copy_assignable_v<pollnet::SocketTcpClient<DefaultConf>>);
static_assert(!std::is_move_constructible_v<pollnet::SocketTcpClient<DefaultConf>>);
static_assert(!std::is_move_assignable_v<pollnet::SocketTcpClient<DefaultConf>>);

static_assert(!std::is_copy_constructible_v<pollnet::SocketTcpServer<DefaultConf>>);
static_assert(!std::is_copy_assignable_v<pollnet::SocketTcpServer<DefaultConf>>);
static_assert(!std::is_move_constructible_v<pollnet::SocketTcpServer<DefaultConf>>);
static_assert(!std::is_move_assignable_v<pollnet::SocketTcpServer<DefaultConf>>);

static_assert(!std::is_copy_constructible_v<pollnet::SocketUdpReceiver<>>);
static_assert(!std::is_copy_assignable_v<pollnet::SocketUdpReceiver<>>);
static_assert(!std::is_move_constructible_v<pollnet::SocketUdpReceiver<>>);
static_assert(!std::is_move_assignable_v<pollnet::SocketUdpReceiver<>>);

static_assert(!std::is_copy_constructible_v<pollnet::SocketUdpSender>);
static_assert(!std::is_copy_assignable_v<pollnet::SocketUdpSender>);
static_assert(!std::is_move_constructible_v<pollnet::SocketUdpSender>);
static_assert(!std::is_move_assignable_v<pollnet::SocketUdpSender>);

// C++20 Concept 约束校验
static_assert(pollnet::TcpConnConfig<DefaultConf>);
static_assert(pollnet::TcpClientConfig<DefaultConf>);
static_assert(pollnet::TcpServerConfig<DefaultConf>);
static_assert(!pollnet::TcpConnConfig<ZeroBufConf>);
static_assert(!pollnet::TcpServerConfig<MissingMaxConnsConf>);
static_assert(pollnet::TcpClientHandler<ValidClientHandler, pollnet::SocketTcpConnection<DefaultConf>>);
static_assert(!pollnet::TcpClientHandler<InvalidClientHandler, pollnet::SocketTcpConnection<DefaultConf>>);
static_assert(pollnet::TcpServerHandler<ValidClientHandler, pollnet::SocketTcpConnection<DefaultConf>>);
static_assert(!pollnet::TcpServerHandler<InvalidClientHandler, pollnet::SocketTcpConnection<DefaultConf>>);
static_assert(pollnet::UdpReadHandler<decltype([](const uint8_t*, uint32_t) {})>);
static_assert(pollnet::UdpRecvFromHandler<decltype([](const uint8_t*, uint32_t, const sockaddr_in&) {})>);

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
// 2. 参数校验与重复 init 测试
// ==========================================

bool test_invalid_init_and_reinit() {
    pollnet::SocketTcpClient<DefaultConf> client;
    CHECK(!client.init("", nullptr, 1234));
    CHECK(std::strlen(client.getLastError()) > 0);
    CHECK(!client.init("", "", 1234));
    CHECK(!client.init("", "999.999.999.999", 1234));
    CHECK(client.init("", "127.0.0.1", 1234));
    CHECK(client.init("", "127.0.0.1", 2345));

    pollnet::SocketTcpServer<DefaultConf> server;
    CHECK(!server.init("", nullptr, 0));
    CHECK(std::strlen(server.getLastError()) > 0);
    CHECK(!server.init("", "invalid_ip", 0));
    CHECK(server.isClosed());
    CHECK(server.getLocalPort() == 0);

    CHECK(server.init("", "127.0.0.1", 0));
    uint16_t port1 = server.getLocalPort();
    CHECK(port1 != 0);
    CHECK(server.init("", "127.0.0.1", 0));
    uint16_t port2 = server.getLocalPort();
    CHECK(port2 != 0);
    CHECK(!server.isClosed());

    pollnet::SocketUdpReceiver<> rx;
    CHECK(!rx.init("", nullptr, 0));
    CHECK(!rx.init("", "bad_ip", 0));
    CHECK(!rx.init("", "127.0.0.1", 0, "not_an_ip"));
    CHECK(rx.isClosed());
    CHECK(rx.getLocalPort() == 0);
    CHECK(rx.init("", "127.0.0.1", 0));
    CHECK(rx.getLocalPort() != 0);
    CHECK(rx.init("", "127.0.0.1", 0, nullptr));
    CHECK(rx.getLocalPort() != 0);

    pollnet::SocketUdpSender tx;
    CHECK(!tx.init("", nullptr, 0, "127.0.0.1", rx.getLocalPort()));
    CHECK(!tx.init("", "127.0.0.1", 0, nullptr, rx.getLocalPort()));
    CHECK(!tx.init("", "bad", 0, "127.0.0.1", rx.getLocalPort()));
    CHECK(!tx.init("", "127.0.0.1", 0, "bad", rx.getLocalPort()));
    CHECK(tx.isClosed());
    CHECK(tx.init("", "127.0.0.1", 0, "127.0.0.1", rx.getLocalPort()));
    CHECK(tx.getLocalPort() != 0);
    CHECK(tx.init("", "127.0.0.1", 0, "127.0.0.1", rx.getLocalPort()));
    CHECK(!tx.isClosed());

    return true;
}

// ==========================================
// 3. TCP 基本收发、getPeername、foreachConn 与 span
// ==========================================

struct BasicConf {
    static constexpr uint32_t RecvBufSize = 4096;
    static constexpr uint32_t MaxConns = 4;
    static constexpr uint32_t ConnRetrySec = 1;
    static constexpr uint32_t ConnTimeoutSec = 5;
    static constexpr uint32_t SendTimeoutSec = 0;
    static constexpr uint32_t RecvTimeoutSec = 0;
    struct UserData {
        uint64_t tag = 0;
    };
};

bool test_tcp_basic_echo_and_span() {
    pollnet::SocketTcpServer<BasicConf> server;
    CHECK(server.init("", "127.0.0.1", 0));
    uint16_t port = server.getLocalPort();
    CHECK(port != 0);

    pollnet::SocketTcpClient<BasicConf> client;
    CHECK(client.init("", "127.0.0.1", port));

    struct ServerHandler {
        int connected = 0;
        int disconnected = 0;
        std::string recv_all;

        void onTcpConnected(pollnet::SocketTcpConnection<BasicConf>& conn) {
            ++connected;
            conn.tag = 42;
            sockaddr_in peer{};
            if (conn.getPeername(peer)) {
                conn.tag += 1;
            }
        }
        void onTcpDisconnect(pollnet::SocketTcpConnection<BasicConf>&) { ++disconnected; }
        void onSendTimeout(pollnet::SocketTcpConnection<BasicConf>&) {}
        void onRecvTimeout(pollnet::SocketTcpConnection<BasicConf>&) {}
        uint32_t onTcpData(pollnet::SocketTcpConnection<BasicConf>& conn, const uint8_t* data,
                           uint32_t size) {
            recv_all.append(reinterpret_cast<const char*>(data), size);
            [[maybe_unused]] bool ok = conn.writeNonblock(std::span<const uint8_t>{data, size});
            return 0;
        }
    } srv_handler;

    struct ClientHandler {
        int connected = 0;
        int disconnected = 0;
        int connect_failed = 0;
        std::string recv_all;

        void onTcpConnectFailed() { ++connect_failed; }
        void onTcpConnected(pollnet::SocketTcpConnection<BasicConf>&) { ++connected; }
        void onTcpDisconnect(pollnet::SocketTcpConnection<BasicConf>&) { ++disconnected; }
        void onSendTimeout(pollnet::SocketTcpConnection<BasicConf>&) {}
        void onRecvTimeout(pollnet::SocketTcpConnection<BasicConf>&) {}
        uint32_t onTcpData(pollnet::SocketTcpConnection<BasicConf>&, const uint8_t* data,
                           uint32_t size) {
            recv_all.append(reinterpret_cast<const char*>(data), size);
            return 0;
        }
    } cli_handler;

    CHECK(poll_until([&]() {
        server.poll(srv_handler);
        client.poll(cli_handler);
        return srv_handler.connected == 1 && cli_handler.connected == 1;
    }));

    CHECK(server.getConnCnt() == 1);
    uint64_t seen_tag = 0;
    server.foreachConn([&](pollnet::SocketTcpConnection<BasicConf>& conn) { seen_tag = conn.tag; });
    CHECK(seen_tag == 43);

    sockaddr_in srv_peer{};
    CHECK(client.getPeername(srv_peer));
    CHECK(ntohs(srv_peer.sin_port) == port);

    const std::string msg1 = "hello-ptr|";
    const std::string msg2 = "hello-u8span|";
    const std::string msg3 = "hello-bytespan";

    CHECK(client.write(msg1.data(), static_cast<uint32_t>(msg1.size())));
    std::vector<uint8_t> u8_buf(msg2.begin(), msg2.end());
    CHECK(client.write(std::span<const uint8_t>{u8_buf}));
    std::vector<std::byte> byte_buf;
    for (char c : msg3) {
        byte_buf.push_back(static_cast<std::byte>(c));
    }
    CHECK(client.writeNonblock(std::span<const std::byte>{byte_buf}));

    const std::string expected = msg1 + msg2 + msg3;
    CHECK(poll_until([&]() {
        server.poll(srv_handler);
        client.poll(cli_handler);
        return cli_handler.recv_all == expected;
    }));
    CHECK(srv_handler.recv_all == expected);

    client.close("test done");
    CHECK(poll_until([&]() {
        server.poll(srv_handler);
        client.poll(cli_handler);
        return srv_handler.disconnected == 1;
    }));
    CHECK(server.getConnCnt() == 0);

    return true;
}

// ==========================================
// 4. TCP 粘包、半包与头部空闲时 memmove 压缩
// ==========================================

struct SmallBufConf {
    static constexpr uint32_t RecvBufSize = 64;
    static constexpr uint32_t MaxConns = 4;
    static constexpr uint32_t ConnRetrySec = 1;
    static constexpr uint32_t ConnTimeoutSec = 5;
    static constexpr uint32_t SendTimeoutSec = 0;
    static constexpr uint32_t RecvTimeoutSec = 0;
    struct UserData {};
};

std::vector<uint8_t> make_frame(std::string_view payload) {
    uint32_t len = static_cast<uint32_t>(payload.size());
    std::vector<uint8_t> out(sizeof(uint32_t) + payload.size());
    std::memcpy(out.data(), &len, sizeof(uint32_t));
    std::memcpy(out.data() + sizeof(uint32_t), payload.data(), payload.size());
    return out;
}

bool test_tcp_framing_and_compaction() {
    pollnet::SocketTcpServer<SmallBufConf> server;
    CHECK(server.init("", "127.0.0.1", 0));
    uint16_t port = server.getLocalPort();

    pollnet::SocketTcpClient<SmallBufConf> client;
    CHECK(client.init("", "127.0.0.1", port));

    struct FrameServerHandler {
        int connected = 0;
        int disconnected = 0;
        std::vector<std::string> frames;

        void onTcpConnected(pollnet::SocketTcpConnection<SmallBufConf>&) { ++connected; }
        void onTcpDisconnect(pollnet::SocketTcpConnection<SmallBufConf>&) { ++disconnected; }
        void onSendTimeout(pollnet::SocketTcpConnection<SmallBufConf>&) {}
        void onRecvTimeout(pollnet::SocketTcpConnection<SmallBufConf>&) {}
        uint32_t onTcpData(pollnet::SocketTcpConnection<SmallBufConf>&, const uint8_t* data,
                           uint32_t size) {
            while (size >= sizeof(uint32_t)) {
                uint32_t len = 0;
                std::memcpy(&len, data, sizeof(uint32_t));
                if (size - sizeof(uint32_t) < len) {
                    break;
                }
                frames.emplace_back(reinterpret_cast<const char*>(data + sizeof(uint32_t)), len);
                uint32_t total = static_cast<uint32_t>(sizeof(uint32_t)) + len;
                data += total;
                size -= total;
            }
            return size;
        }
    } srv_handler;

    struct IdleClientHandler {
        int connected = 0;
        void onTcpConnectFailed() {}
        void onTcpConnected(pollnet::SocketTcpConnection<SmallBufConf>&) { ++connected; }
        void onTcpDisconnect(pollnet::SocketTcpConnection<SmallBufConf>&) {}
        void onSendTimeout(pollnet::SocketTcpConnection<SmallBufConf>&) {}
        void onRecvTimeout(pollnet::SocketTcpConnection<SmallBufConf>&) {}
        uint32_t onTcpData(pollnet::SocketTcpConnection<SmallBufConf>&, const uint8_t*, uint32_t) {
            return 0;
        }
    } cli_handler;

    CHECK(poll_until([&]() {
        server.poll(srv_handler);
        client.poll(cli_handler);
        return srv_handler.connected == 1 && cli_handler.connected == 1;
    }));

    // 4a. 粘包测试：一次发送 3 个帧
    std::vector<uint8_t> coalesced;
    for (std::string_view s : {"alpha", "beta", "gamma"}) {
        auto f = make_frame(s);
        coalesced.insert(coalesced.end(), f.begin(), f.end());
    }
    CHECK(client.write(std::span<const uint8_t>{coalesced}));
    CHECK(poll_until([&]() {
        server.poll(srv_handler);
        return srv_handler.frames.size() == 3;
    }));
    CHECK(srv_handler.frames[0] == "alpha");
    CHECK(srv_handler.frames[1] == "beta");
    CHECK(srv_handler.frames[2] == "gamma");

    // 4b. 半包测试：逐字节发送一个帧
    auto frag = make_frame("split-frame-ok");
    for (uint8_t b : frag) {
        CHECK(client.write(&b, 1));
        server.poll(srv_handler);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    CHECK(poll_until([&]() {
        server.poll(srv_handler);
        return srv_handler.frames.size() == 4;
    }));
    CHECK(srv_handler.frames[3] == "split-frame-ok");

    // 4c. 精确验证 tail_ == RecvBufSize(64) 且 0 < head_(10) < RecvBufSize/2(32) 时触发 memmove 压缩而不误断连
    auto f10 = make_frame("123456");  // 4 + 6 = 10 字节
    std::string payload56(56, 'Z');
    auto f60 = make_frame(payload56);  // 4 + 56 = 60 字节
    CHECK(f10.size() == 10 && f60.size() == 60);

    std::vector<uint8_t> batch;
    batch.insert(batch.end(), f10.begin(), f10.end());
    batch.insert(batch.end(), f60.begin(), f60.begin() + 54);  // 10 + 54 = 64 字节填满缓冲区
    CHECK(batch.size() == 64);
    CHECK(client.write(std::span<const uint8_t>{batch}));

    CHECK(poll_until([&]() {
        server.poll(srv_handler);
        return srv_handler.frames.size() == 5;
    }));
    // 继续轮询几个周期确保剩余字节读完并触发 memmove
    for (int i = 0; i < 10; ++i) {
        server.poll(srv_handler);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    CHECK(srv_handler.disconnected == 0);
    CHECK(server.getConnCnt() == 1);

    // 发送 f60 剩余的 6 字节，验证完整收包
    CHECK(client.write(f60.data() + 54, 6));
    CHECK(poll_until([&]() {
        server.poll(srv_handler);
        return srv_handler.frames.size() == 6;
    }));
    CHECK(srv_handler.frames[4] == "123456");
    CHECK(srv_handler.frames[5] == payload56);

    return true;
}

// ==========================================
// 5. 异常 remaining 返回值 & 单包撑满 RecvBufSize
// ==========================================

bool test_tcp_bad_remaining_and_buf_full() {
    // 5a. remaining > avail
    {
        pollnet::SocketTcpServer<SmallBufConf> server;
        CHECK(server.init("", "127.0.0.1", 0));
        pollnet::SocketTcpClient<SmallBufConf> client;
        CHECK(client.init("", "127.0.0.1", server.getLocalPort()));

        struct BadRemainingHandler {
            int connected = 0;
            int disconnected = 0;
            std::string reason;
            void onTcpConnected(pollnet::SocketTcpConnection<SmallBufConf>&) { ++connected; }
            void onTcpDisconnect(pollnet::SocketTcpConnection<SmallBufConf>& conn) {
                ++disconnected;
                reason = conn.getLastError();
            }
            void onSendTimeout(pollnet::SocketTcpConnection<SmallBufConf>&) {}
            void onRecvTimeout(pollnet::SocketTcpConnection<SmallBufConf>&) {}
            uint32_t onTcpData(pollnet::SocketTcpConnection<SmallBufConf>&, const uint8_t*,
                               uint32_t size) {
                return size + 1;  // 故意返回非法剩余长度
            }
        } srv_handler;

        struct CliHandler {
            int connected = 0;
            void onTcpConnectFailed() {}
            void onTcpConnected(pollnet::SocketTcpConnection<SmallBufConf>&) { ++connected; }
            void onTcpDisconnect(pollnet::SocketTcpConnection<SmallBufConf>&) {}
            void onSendTimeout(pollnet::SocketTcpConnection<SmallBufConf>&) {}
            void onRecvTimeout(pollnet::SocketTcpConnection<SmallBufConf>&) {}
            uint32_t onTcpData(pollnet::SocketTcpConnection<SmallBufConf>&, const uint8_t*,
                               uint32_t) {
                return 0;
            }
        } cli_handler;

        CHECK(poll_until([&]() {
            server.poll(srv_handler);
            client.poll(cli_handler);
            return srv_handler.connected == 1 && cli_handler.connected == 1;
        }));

        CHECK(client.write("abcd", 4));
        CHECK(poll_until([&]() {
            server.poll(srv_handler);
            return srv_handler.disconnected == 1;
        }));
        CHECK(server.getConnCnt() == 0);
        CHECK(srv_handler.reason == "invalid remaining size");
    }

    // 5b. 整整 64 字节无法消费触发 recv buf full
    {
        pollnet::SocketTcpServer<SmallBufConf> server;
        CHECK(server.init("", "127.0.0.1", 0));
        pollnet::SocketTcpClient<SmallBufConf> client;
        CHECK(client.init("", "127.0.0.1", server.getLocalPort()));

        struct FullBufHandler {
            int connected = 0;
            int disconnected = 0;
            std::string reason;
            void onTcpConnected(pollnet::SocketTcpConnection<SmallBufConf>&) { ++connected; }
            void onTcpDisconnect(pollnet::SocketTcpConnection<SmallBufConf>& conn) {
                ++disconnected;
                reason = conn.getLastError();
            }
            void onSendTimeout(pollnet::SocketTcpConnection<SmallBufConf>&) {}
            void onRecvTimeout(pollnet::SocketTcpConnection<SmallBufConf>&) {}
            uint32_t onTcpData(pollnet::SocketTcpConnection<SmallBufConf>&, const uint8_t*,
                               uint32_t size) {
                return size;  // 始终不消费
            }
        } srv_handler;

        struct CliHandler {
            int connected = 0;
            void onTcpConnectFailed() {}
            void onTcpConnected(pollnet::SocketTcpConnection<SmallBufConf>&) { ++connected; }
            void onTcpDisconnect(pollnet::SocketTcpConnection<SmallBufConf>&) {}
            void onSendTimeout(pollnet::SocketTcpConnection<SmallBufConf>&) {}
            void onRecvTimeout(pollnet::SocketTcpConnection<SmallBufConf>&) {}
            uint32_t onTcpData(pollnet::SocketTcpConnection<SmallBufConf>&, const uint8_t*,
                               uint32_t) {
                return 0;
            }
        } cli_handler;

        CHECK(poll_until([&]() {
            server.poll(srv_handler);
            client.poll(cli_handler);
            return srv_handler.connected == 1 && cli_handler.connected == 1;
        }));

        std::array<uint8_t, 64> fill{};
        fill.fill(0x5A);
        CHECK(client.write(std::span<const uint8_t>{fill}));
        CHECK(poll_until([&]() {
            server.poll(srv_handler);
            return srv_handler.disconnected == 1;
        }));
        CHECK(server.getConnCnt() == 0);
        CHECK(srv_handler.reason == "recv buf full");
    }

    return true;
}

// ==========================================
// 6. 未监听端口快速失败 + allowReconnect 恢复
// ==========================================

struct LongRetryConf {
    static constexpr uint32_t RecvBufSize = 1024;
    static constexpr uint32_t MaxConns = 4;
    static constexpr uint32_t ConnRetrySec = 10;
    static constexpr uint32_t ConnTimeoutSec = 30;
    static constexpr uint32_t SendTimeoutSec = 0;
    static constexpr uint32_t RecvTimeoutSec = 0;
    struct UserData {};
};

bool test_tcp_connect_fail_and_allow_reconnect() {
    pollnet::SocketTcpServer<LongRetryConf> server;
    CHECK(server.init("", "127.0.0.1", 0));
    uint16_t port = server.getLocalPort();
    server.close("reserve port released");

    pollnet::SocketTcpClient<LongRetryConf> client;
    CHECK(client.init("", "127.0.0.1", port));

    struct CliHandler {
        int failed = 0;
        int connected = 0;
        void onTcpConnectFailed() { ++failed; }
        void onTcpConnected(pollnet::SocketTcpConnection<LongRetryConf>&) { ++connected; }
        void onTcpDisconnect(pollnet::SocketTcpConnection<LongRetryConf>&) {}
        void onSendTimeout(pollnet::SocketTcpConnection<LongRetryConf>&) {}
        void onRecvTimeout(pollnet::SocketTcpConnection<LongRetryConf>&) {}
        uint32_t onTcpData(pollnet::SocketTcpConnection<LongRetryConf>&, const uint8_t*,
                           uint32_t) {
            return 0;
        }
    } cli_handler;

    auto start = std::chrono::steady_clock::now();
    CHECK(poll_until([&]() {
        client.poll(cli_handler);
        return cli_handler.failed >= 1;
    }));
    auto elapsed_ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start)
            .count();
    CHECK(elapsed_ms < 2000);
    CHECK(std::strlen(client.getLastError()) > 0);

    // 重新启动服务端，调用 allowReconnect() 应立即重连成功而无需等 10s
    CHECK(server.init("", "127.0.0.1", port));
    client.allowReconnect();

    struct SrvHandler {
        int connected = 0;
        void onTcpConnected(pollnet::SocketTcpConnection<LongRetryConf>&) { ++connected; }
        void onTcpDisconnect(pollnet::SocketTcpConnection<LongRetryConf>&) {}
        void onSendTimeout(pollnet::SocketTcpConnection<LongRetryConf>&) {}
        void onRecvTimeout(pollnet::SocketTcpConnection<LongRetryConf>&) {}
        uint32_t onTcpData(pollnet::SocketTcpConnection<LongRetryConf>&, const uint8_t*,
                           uint32_t) {
            return 0;
        }
    } srv_handler;

    CHECK(poll_until([&]() {
        server.poll(srv_handler);
        client.poll(cli_handler);
        return cli_handler.connected == 1 && srv_handler.connected == 1;
    }));

    return true;
}

// ==========================================
// 7. SendTimeoutSec / RecvTimeoutSec 超时回调
// ==========================================

struct TimeoutConf {
    static constexpr uint32_t RecvBufSize = 1024;
    static constexpr uint32_t MaxConns = 4;
    static constexpr uint32_t ConnRetrySec = 1;
    static constexpr uint32_t ConnTimeoutSec = 5;
    static constexpr uint32_t SendTimeoutSec = 1;
    static constexpr uint32_t RecvTimeoutSec = 1;
    struct UserData {};
};

bool test_tcp_timeouts() {
    pollnet::SocketTcpServer<TimeoutConf> server;
    CHECK(server.init("", "127.0.0.1", 0));

    pollnet::SocketTcpClient<TimeoutConf> client;
    CHECK(client.init("", "127.0.0.1", server.getLocalPort()));

    struct SrvHandler {
        int connected = 0;
        int recv_timeouts = 0;
        int disconnected = 0;
        void onTcpConnected(pollnet::SocketTcpConnection<TimeoutConf>&) { ++connected; }
        void onTcpDisconnect(pollnet::SocketTcpConnection<TimeoutConf>&) { ++disconnected; }
        void onSendTimeout(pollnet::SocketTcpConnection<TimeoutConf>&) {}
        void onRecvTimeout(pollnet::SocketTcpConnection<TimeoutConf>& conn) {
            ++recv_timeouts;
            conn.close("recv timeout");
        }
        uint32_t onTcpData(pollnet::SocketTcpConnection<TimeoutConf>&, const uint8_t*, uint32_t) {
            return 0;
        }
    } srv_handler;

    struct CliHandler {
        int connected = 0;
        int send_timeouts = 0;
        int disconnected = 0;
        void onTcpConnectFailed() {}
        void onTcpConnected(pollnet::SocketTcpConnection<TimeoutConf>&) { ++connected; }
        void onTcpDisconnect(pollnet::SocketTcpConnection<TimeoutConf>&) { ++disconnected; }
        void onSendTimeout(pollnet::SocketTcpConnection<TimeoutConf>&) { ++send_timeouts; }
        void onRecvTimeout(pollnet::SocketTcpConnection<TimeoutConf>&) {}
        uint32_t onTcpData(pollnet::SocketTcpConnection<TimeoutConf>&, const uint8_t*, uint32_t) {
            return 0;
        }
    } cli_handler;

    CHECK(poll_until([&]() {
        server.poll(srv_handler);
        client.poll(cli_handler);
        return srv_handler.connected == 1 && cli_handler.connected == 1;
    }));

    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(2500);
    while (std::chrono::steady_clock::now() < deadline) {
        server.poll(srv_handler);
        client.poll(cli_handler);
        if (cli_handler.send_timeouts >= 1 && srv_handler.recv_timeouts >= 1 &&
            srv_handler.disconnected >= 1 && cli_handler.disconnected >= 1) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }

    CHECK(cli_handler.send_timeouts >= 1);
    CHECK(srv_handler.recv_timeouts >= 1);
    CHECK(srv_handler.disconnected == 1);
    CHECK(cli_handler.disconnected == 1);

    return true;
}

// ==========================================
// 8. MaxConns 上限与断开后槽位复用
// ==========================================

struct TwoConnConf {
    static constexpr uint32_t RecvBufSize = 1024;
    static constexpr uint32_t MaxConns = 2;
    static constexpr uint32_t ConnRetrySec = 1;
    static constexpr uint32_t ConnTimeoutSec = 5;
    static constexpr uint32_t SendTimeoutSec = 0;
    static constexpr uint32_t RecvTimeoutSec = 0;
    struct UserData {};
};

bool test_tcp_max_conns_and_slot_reuse() {
    pollnet::SocketTcpServer<TwoConnConf> server;
    CHECK(server.init("", "127.0.0.1", 0));
    uint16_t port = server.getLocalPort();

    struct SrvHandler {
        int connected = 0;
        int disconnected = 0;
        std::string last_data;
        void onTcpConnected(pollnet::SocketTcpConnection<TwoConnConf>&) { ++connected; }
        void onTcpDisconnect(pollnet::SocketTcpConnection<TwoConnConf>&) { ++disconnected; }
        void onSendTimeout(pollnet::SocketTcpConnection<TwoConnConf>&) {}
        void onRecvTimeout(pollnet::SocketTcpConnection<TwoConnConf>&) {}
        uint32_t onTcpData(pollnet::SocketTcpConnection<TwoConnConf>&, const uint8_t* data,
                           uint32_t size) {
            last_data.assign(reinterpret_cast<const char*>(data), size);
            return 0;
        }
    } srv_handler;

    struct CliHandler {
        int connected = 0;
        void onTcpConnectFailed() {}
        void onTcpConnected(pollnet::SocketTcpConnection<TwoConnConf>&) { ++connected; }
        void onTcpDisconnect(pollnet::SocketTcpConnection<TwoConnConf>&) {}
        void onSendTimeout(pollnet::SocketTcpConnection<TwoConnConf>&) {}
        void onRecvTimeout(pollnet::SocketTcpConnection<TwoConnConf>&) {}
        uint32_t onTcpData(pollnet::SocketTcpConnection<TwoConnConf>&, const uint8_t*, uint32_t) {
            return 0;
        }
    };

    pollnet::SocketTcpClient<TwoConnConf> c1, c2, c3;
    CliHandler h1, h2, h3;
    CHECK(c1.init("", "127.0.0.1", port));
    CHECK(c2.init("", "127.0.0.1", port));

    CHECK(poll_until([&]() {
        server.poll(srv_handler);
        c1.poll(h1);
        c2.poll(h2);
        return srv_handler.connected == 2 && h1.connected == 1 && h2.connected == 1;
    }));
    CHECK(server.getConnCnt() == 2);

    // 第 3 个客户端发起连接，服务端在满槽时不得接纳
    CHECK(c3.init("", "127.0.0.1", port));
    for (int i = 0; i < 30; ++i) {
        server.poll(srv_handler);
        c3.poll(h3);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    CHECK(server.getConnCnt() == 2);
    CHECK(srv_handler.connected == 2);

    // 断开 c1 释放槽位，服务端应接纳 c3 并能正常收发
    c1.close("release slot");
    CHECK(poll_until([&]() {
        server.poll(srv_handler);
        c3.poll(h3);
        return srv_handler.disconnected == 1 && srv_handler.connected == 3 && h3.connected == 1;
    }));
    CHECK(server.getConnCnt() == 2);

    CHECK(c3.write("slot-reused", 11));
    CHECK(poll_until([&]() {
        server.poll(srv_handler);
        return srv_handler.last_data == "slot-reused";
    }));

    return true;
}

// ==========================================
// 9. UDP 回环单播收发测试
// ==========================================

bool test_udp_loopback() {
    pollnet::SocketUdpReceiver<1500> rx1;
    pollnet::SocketUdpReceiver<1500> rx2;
    CHECK(rx1.init("", "127.0.0.1", 0));
    CHECK(rx2.init("", "127.0.0.1", 0));

    uint16_t rx1_port = rx1.getLocalPort();
    uint16_t rx2_port = rx2.getLocalPort();
    CHECK(rx1_port != 0 && rx2_port != 0);

    pollnet::SocketUdpSender tx;
    CHECK(tx.init("", "127.0.0.1", 0, "127.0.0.1", rx1_port));
    uint16_t tx_port = tx.getLocalPort();
    CHECK(tx_port != 0);

    // 9a. Sender(raw) -> Receiver::recvfrom
    const std::string ping = "udp-ping";
    CHECK(tx.write(ping.data(), static_cast<uint32_t>(ping.size())));

    std::string rx1_msg;
    sockaddr_in seen_src{};
    CHECK(poll_until([&]() {
        return rx1.recvfrom([&](const uint8_t* data, uint32_t size, const sockaddr_in& src) {
            rx1_msg.assign(reinterpret_cast<const char*>(data), size);
            seen_src = src;
        });
    }));
    CHECK(rx1_msg == ping);
    CHECK(ntohs(seen_src.sin_port) == tx_port);

    // 9b. Sender(std::span<const std::byte>) -> Receiver::read
    const std::string ping2 = "udp-span-byte";
    std::vector<std::byte> byte_payload;
    for (char c : ping2) {
        byte_payload.push_back(static_cast<std::byte>(c));
    }
    CHECK(tx.write(std::span<const std::byte>{byte_payload}));

    std::string rx1_msg2;
    CHECK(poll_until([&]() {
        return rx1.read([&](const uint8_t* data, uint32_t size) {
            rx1_msg2.assign(reinterpret_cast<const char*>(data), size);
        });
    }));
    CHECK(rx1_msg2 == ping2);

    // 9c. Receiver::sendto(std::span<const uint8_t>) -> rx2::read
    sockaddr_in dst_rx2{};
    dst_rx2.sin_family = AF_INET;
    dst_rx2.sin_port = htons(rx2_port);
    CHECK(::inet_pton(AF_INET, "127.0.0.1", &dst_rx2.sin_addr) == 1);

    const std::string pong = "udp-pong-span";
    std::vector<uint8_t> pong_bytes(pong.begin(), pong.end());
    CHECK(rx1.sendto(std::span<const uint8_t>{pong_bytes}, dst_rx2));

    std::string rx2_msg;
    CHECK(poll_until([&]() {
        return rx2.read([&](const uint8_t* data, uint32_t size) {
            rx2_msg.assign(reinterpret_cast<const char*>(data), size);
        });
    }));
    CHECK(rx2_msg == pong);

    return true;
}

}  // namespace

int main() {
    struct TestCase {
        const char* name;
        bool (*fn)();
    };

    const TestCase cases[] = {
        {"invalid_init_and_reinit", test_invalid_init_and_reinit},
        {"tcp_basic_echo_and_span", test_tcp_basic_echo_and_span},
        {"tcp_framing_and_compaction", test_tcp_framing_and_compaction},
        {"tcp_bad_remaining_and_buf_full", test_tcp_bad_remaining_and_buf_full},
        {"tcp_connect_fail_and_allow_reconnect", test_tcp_connect_fail_and_allow_reconnect},
        {"tcp_timeouts", test_tcp_timeouts},
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
