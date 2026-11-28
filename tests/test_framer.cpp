#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

#include "pollnet/eventloop.h"
#include "pollnet/socket.h"
#include "test_common.h"

namespace {

using pollnet_test::poll_until;

// ==========================================
// 1. 编译期契约、Rule of Five 与 Concepts 验证
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

// ByteRange 与 SendPart 约束验证
static_assert(pollnet::ByteRange<std::span<const uint8_t>>);
static_assert(pollnet::ByteRange<std::span<const std::byte>>);
static_assert(pollnet::ByteRange<std::string_view>);
static_assert(pollnet::ByteRange<std::string>);
static_assert(pollnet::ByteRange<std::vector<uint8_t>>);
static_assert(pollnet::ByteRange<std::array<uint8_t, 4>>);
static_assert(!pollnet::ByteRange<const char (&)[6]>);
static_assert(!pollnet::ByteRange<std::array<pollnet::IoSlice, 2>>);

static_assert(DefaultConn::kRecvBufSize == 4096);
static_assert(DefaultConn::kSendBufSize == 0);
static_assert(pollnet::SendPart<pollnet::IoSlice>);
static_assert(pollnet::SendPart<std::string_view>);
static_assert(pollnet::SendPart<uint32_t>);
static_assert(!pollnet::SendPart<const char*>);
static_assert(!pollnet::SendPart<const char (&)[6]>);
static_assert(!pollnet::SendPart<std::span<const pollnet::IoSlice>>);

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

bool test_fixed_frame_and_send_parts() {
    struct Tick {
        uint32_t seq;
        uint64_t price;
    };

    static_assert(pollnet::SendPart<Tick>);

    struct MinimalTickHandler {
        void onMessage(DefaultConn&, const Tick&) {}
    };
    using MinTickAdapter = pollnet::FixedFrameHandler<Tick, MinimalTickHandler>;
    static_assert(pollnet::TcpHandler<MinTickAdapter, DefaultConn>);
    static_assert(!pollnet::HasOnConnect<MinTickAdapter, DefaultConn>);
    static_assert(!pollnet::HasOnDisconnect<MinTickAdapter, DefaultConn>);
    static_assert(!pollnet::HasAllowNewConnection<MinTickAdapter>);

    pollnet::TcpServer<> server;
    CHECK(server.listen({.host = "127.0.0.1", .port = 0}));
    pollnet::TcpClient<> client;
    CHECK(client.connect({.host = "127.0.0.1", .port = server.localPort()}));

    struct SrvTickHandler {
        int connected = 0;
        bool send_ok = true;
        std::vector<Tick> rx;
        bool allowNewConnection(const pollnet::Endpoint&) { return true; }
        void onConnect(DefaultConn&) { ++connected; }
        void onMessage(DefaultConn& conn, const Tick& t) {
            rx.push_back(t);
            send_ok = send_ok && conn.sendParts(t);
        }
    } srv_inner;

    struct CliTickHandler {
        int connected = 0;
        std::vector<Tick> echoed;
        void onConnect(DefaultConn&) { ++connected; }
        void onMessage(DefaultConn&, const Tick& t) { echoed.push_back(t); }
    } cli_inner;

    auto srv_h = pollnet::makeFixedFrameHandler<Tick>(srv_inner);
    auto cli_h = pollnet::makeFixedFrameHandler<Tick>(cli_inner);
    static_assert(pollnet::HasOnConnect<decltype(srv_h), DefaultConn>);
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

    return true;
}

bool test_length_prefixed_framer() {
    struct LenHdr {
        uint32_t len_be;
    };

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

    LenHdr bad_hdr{htonl(300u)};
    CHECK(client.sendParts(bad_hdr));
    CHECK(poll_until([&]() {
        server.poll(srv_h);
        return srv_inner.disconnected == 1;
    }));
    CHECK(srv_inner.reason == "message too large");

    return true;
}

bool test_delimiter_framer() {
    struct MinimalDelimHandler {
        void onMessage(DefaultConn&, std::span<const uint8_t>) {}
    };
    using MinDelimAdapter = pollnet::DelimiterFrameHandler<MinimalDelimHandler>;
    static_assert(pollnet::TcpHandler<MinDelimAdapter, DefaultConn>);
    static_assert(!pollnet::HasOnConnect<MinDelimAdapter, DefaultConn>);
    static_assert(!pollnet::HasOnDisconnect<MinDelimAdapter, DefaultConn>);
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
            void onConnect(DefaultConn&) { ++connected; }
            void onMessage(DefaultConn& conn, std::span<const uint8_t> frame) {
                std::string s(reinterpret_cast<const char*>(frame.data()), frame.size());
                frames.push_back(s);
                (void)conn.sendParts(s, std::string_view{"\n"});
            }
        } srv_inner;
        struct CliDelim {
            int connected = 0;
            std::vector<std::string> frames;
            void onConnect(DefaultConn&) { ++connected; }
            void onMessage(DefaultConn&, std::span<const uint8_t> frame) {
                frames.emplace_back(reinterpret_cast<const char*>(frame.data()), frame.size());
            }
        } cli_inner;

        auto srv_h = pollnet::makeDelimiterHandler('\n', srv_inner);
        auto cli_h = pollnet::makeDelimiterHandler('\n', cli_inner);
        static_assert(pollnet::HasOnConnect<decltype(srv_h), DefaultConn>);

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
            void onConnect(DefaultConn&) { ++connected; }
            void onDisconnect(DefaultConn& c) {
                ++disconnected;
                reason = std::string(c.lastError());
            }
            void onMessage(DefaultConn&, std::span<const uint8_t> frame) {
                frames.emplace_back(reinterpret_cast<const char*>(frame.data()), frame.size());
            }
        } srv_inner;
        struct IdleCli {
            int connected = 0;
            void onConnect(DefaultConn&) { ++connected; }
            size_t onData(DefaultConn&, std::span<const uint8_t> d) { return d.size(); }
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
            void onConnect(DefaultConn&) { ++connected; }
            void onDisconnect(DefaultConn& c) {
                ++disconnected;
                reason = std::string(c.lastError());
            }
            void onMessage(DefaultConn&, std::span<const uint8_t>) {}
        } srv_inner;
        struct IdleCli {
            int connected = 0;
            void onConnect(DefaultConn&) { ++connected; }
            size_t onData(DefaultConn&, std::span<const uint8_t> d) { return d.size(); }
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

    return true;
}

}  // namespace

int main() {
    const pollnet_test::TestCase cases[] = {
        {"fixed_frame_and_send_parts", test_fixed_frame_and_send_parts},
        {"length_prefixed_framer", test_length_prefixed_framer},
        {"delimiter_framer", test_delimiter_framer},
    };
    return pollnet_test::run_test_suite("test_framer", cases);
}
