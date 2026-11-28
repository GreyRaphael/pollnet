#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "pollnet/eventloop.h"
#include "pollnet/socket.h"
#include "test_common.h"

bool odr_helper_smoke();

namespace {

using pollnet_test::poll_until;

std::vector<uint8_t> make_frame(std::string_view payload) {
    uint32_t len = static_cast<uint32_t>(payload.size());
    std::vector<uint8_t> out(sizeof(uint32_t) + payload.size());
    std::memcpy(out.data(), &len, sizeof(uint32_t));
    std::memcpy(out.data() + sizeof(uint32_t), payload.data(), payload.size());
    return out;
}

bool test_tcp_invalid_options_and_reinit() {
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

    return true;
}

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

bool test_static_send_buffer_backpressure() {
    constexpr size_t kChunkBytes = 256;
    constexpr size_t kSendBufCap = 4096;

    auto make_chunk = [&](uint32_t seq) {
        std::array<uint8_t, kChunkBytes> buf{};
        buf.fill(static_cast<uint8_t>((seq & 0xFFu) ^ 0x5Au));
        std::memcpy(buf.data(), &seq, sizeof(seq));
        return buf;
    };

    auto verify_chunks = [&](const std::vector<uint8_t>& bytes, size_t expected_cnt) -> bool {
        if (bytes.size() != expected_cnt * kChunkBytes) return false;
        for (size_t i = 0; i < expected_cnt; ++i) {
            uint32_t seq = 0;
            std::memcpy(&seq, bytes.data() + i * kChunkBytes, sizeof(seq));
            if (seq != static_cast<uint32_t>(i)) return false;
            uint8_t pat = static_cast<uint8_t>((seq & 0xFFu) ^ 0x5Au);
            for (size_t k = sizeof(uint32_t); k < kChunkBytes; ++k) {
                if (bytes[i * kChunkBytes + k] != pat) return false;
            }
        }
        return true;
    };

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
        CHECK(client.isConnected());
        CHECK(client.pendingSendBytes() > 0 && client.freeSendBytes() < kChunkBytes);

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

bool test_odr_multi_tu_link() {
    CHECK(odr_helper_smoke());
    return true;
}

}  // namespace

int main() {
    const pollnet_test::TestCase cases[] = {
        {"tcp_invalid_options_and_reinit", test_tcp_invalid_options_and_reinit},
        {"tcp_minimal_and_full_handler", test_tcp_minimal_and_full_handler},
        {"tcp_tuning_and_vector_write", test_tcp_tuning_and_vector_write},
        {"tcp_framing_and_compaction", test_tcp_framing_and_compaction},
        {"tcp_bad_consumed_and_buf_full", test_tcp_bad_consumed_and_buf_full},
        {"tcp_connect_fail_and_allow_reconnect", test_tcp_connect_fail_and_allow_reconnect},
        {"tcp_subsecond_timeouts_and_custom_clock", test_tcp_subsecond_timeouts_and_custom_clock},
        {"tcp_max_conns_and_slot_reuse", test_tcp_max_conns_and_slot_reuse},
        {"eventloop_backend", test_eventloop_backend},
        {"static_send_buffer_backpressure", test_static_send_buffer_backpressure},
        {"odr_multi_tu_link", test_odr_multi_tu_link},
    };
    return pollnet_test::run_test_suite("test_tcp", cases);
}
