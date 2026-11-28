#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <thread>
#include <type_traits>
#include <vector>

#include "pollnet/socket.h"
#include "test_common.h"

namespace {

using pollnet_test::poll_until;

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

bool test_udp_invalid_options_and_reinit() {
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

bool test_udp_struct_helpers() {
    struct Tick {
        uint32_t seq;
        uint64_t price;
    };

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

    return true;
}

}  // namespace

int main() {
    const pollnet_test::TestCase cases[] = {
        {"endpoint", test_endpoint},
        {"udp_invalid_options_and_reinit", test_udp_invalid_options_and_reinit},
        {"pin_thread_and_low_latency_presets", test_pin_thread_and_low_latency_presets},
        {"udp_loopback_and_batch", test_udp_loopback_and_batch},
        {"udp_struct_helpers", test_udp_struct_helpers},
    };
    return pollnet_test::run_test_suite("test_udp", cases);
}
