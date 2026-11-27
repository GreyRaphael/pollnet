#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <format>
#include <iostream>
#include <span>
#include <string_view>
#include <thread>
#include <vector>

#include "bench_common.h"
#include "pollnet/socket.h"

namespace {

template <bool UseExtClock>
bool run_tcp_ping_pong(const pollnet_bench::BenchConfig& cfg, std::string_view label) {
    using Server = pollnet::TcpServer<pollnet::EmptyUserData, 4096, 4>;
    using Client = pollnet::TcpClient<pollnet::EmptyUserData, 4096>;

    std::atomic<uint16_t> port{0};
    std::atomic<bool> srv_failed{false};
    std::atomic<bool> stop{false};

    std::thread srv_thread([&]() {
        Server server;
        if (!server.listen({.host = "127.0.0.1", .port = 0})) {
            std::cerr << std::format("[v2] server listen failed: {}\n", server.lastError());
            srv_failed.store(true, std::memory_order_release);
            return;
        }
        port.store(server.localPort(), std::memory_order_release);

        struct EchoHandler {
            size_t payload_size;
            size_t onData(Server::Conn& conn, std::span<const uint8_t> data) {
                size_t offset = 0;
                while (data.size() - offset >= payload_size) {
                    [[maybe_unused]] bool ok =
                        conn.writeNonblock(data.subspan(offset, payload_size));
                    offset += payload_size;
                }
                return offset;
            }
        } handler{cfg.payload};

        while (!stop.load(std::memory_order_relaxed)) {
            if constexpr (UseExtClock) {
                server.poll(handler, pollnet_bench::bench_now_ns());
            } else {
                server.poll(handler);
            }
        }
    });

    while (port.load(std::memory_order_acquire) == 0 && !srv_failed.load(std::memory_order_acquire)) {
        std::this_thread::yield();
    }
    if (srv_failed.load(std::memory_order_acquire)) {
        srv_thread.join();
        return false;
    }

    Client client;
    if (!client.connect({.host = "127.0.0.1", .port = port.load(std::memory_order_acquire)})) {
        std::cerr << std::format("[v2] client connect failed: {}\n", client.lastError());
        stop.store(true, std::memory_order_relaxed);
        srv_thread.join();
        return false;
    }

    struct ClientHandler {
        size_t payload_size;
        bool got_reply = false;
        size_t onData(Client::Conn&, std::span<const uint8_t> data) {
            if (data.size() < payload_size) return 0;
            got_reply = true;
            return payload_size;
        }
    } cli_handler{cfg.payload};

    auto poll_client = [&]() {
        if constexpr (UseExtClock) {
            client.poll(cli_handler, pollnet_bench::bench_now_ns());
        } else {
            client.poll(cli_handler);
        }
    };

    while (!client.isConnected()) {
        poll_client();
    }

    std::vector<uint8_t> send_buf(cfg.payload, 0xAB);
    std::vector<int64_t> samples;
    samples.reserve(cfg.iters);

    const size_t total = cfg.warmup + cfg.iters;
    for (size_t i = 0; i < total; ++i) {
        std::memcpy(send_buf.data(), &i, sizeof(uint64_t));
        cli_handler.got_reply = false;
        int64_t t0 = pollnet_bench::bench_now_ns();
        if (!client.writeNonblock(send_buf)) {
            std::cerr << std::format("[v2] writeNonblock failed: {}\n", client.lastError());
            stop.store(true, std::memory_order_relaxed);
            srv_thread.join();
            return false;
        }
        while (!cli_handler.got_reply) {
            poll_client();
            if (!client.isConnected()) {
                std::cerr << std::format("[v2] client disconnected: {}\n", client.lastError());
                stop.store(true, std::memory_order_relaxed);
                srv_thread.join();
                return false;
            }
        }
        int64_t t1 = pollnet_bench::bench_now_ns();
        if (i >= cfg.warmup) {
            samples.push_back(t1 - t0);
        }
    }

    stop.store(true, std::memory_order_relaxed);
    srv_thread.join();

    auto stats = pollnet_bench::compute_latency_stats(samples);
    pollnet_bench::print_latency_result(label, stats, samples.size(), cfg.csv);
    return true;
}

bool run_tcp_throughput(const pollnet_bench::BenchConfig& cfg) {
    using Server = pollnet::TcpServer<pollnet::EmptyUserData, 65536, 4>;
    using Client = pollnet::TcpClient<pollnet::EmptyUserData, 4096>;

    std::atomic<uint16_t> port{0};
    std::atomic<bool> srv_failed{false};
    std::atomic<bool> stop{false};

    std::thread srv_thread([&]() {
        Server server;
        if (!server.listen({.host = "127.0.0.1", .port = 0})) {
            std::cerr << std::format("[v2] tcp_tp server listen failed: {}\n", server.lastError());
            srv_failed.store(true, std::memory_order_release);
            return;
        }
        port.store(server.localPort(), std::memory_order_release);

        struct DrainHandler {
            size_t payload_size;
            size_t target_msgs;
            size_t received = 0;
            bool ack_sent = false;
            size_t onData(Server::Conn& conn, std::span<const uint8_t> data) {
                size_t count = data.size() / payload_size;
                received += count;
                if (!ack_sent && received >= target_msgs) {
                    uint64_t ack = received;
                    [[maybe_unused]] bool ok = conn.writeNonblock(&ack, sizeof(ack));
                    ack_sent = true;
                }
                return count * payload_size;
            }
        } handler{cfg.payload, cfg.throughput_iters};

        while (!stop.load(std::memory_order_relaxed)) {
            server.poll(handler);
        }
    });

    while (port.load(std::memory_order_acquire) == 0 && !srv_failed.load(std::memory_order_acquire)) {
        std::this_thread::yield();
    }
    if (srv_failed.load(std::memory_order_acquire)) {
        srv_thread.join();
        return false;
    }

    Client client;
    if (!client.connect({.host = "127.0.0.1", .port = port.load(std::memory_order_acquire)})) {
        stop.store(true, std::memory_order_relaxed);
        srv_thread.join();
        return false;
    }

    struct AckHandler {
        bool ack_received = false;
        size_t onData(Client::Conn&, std::span<const uint8_t> data) {
            if (data.size() < sizeof(uint64_t)) return 0;
            ack_received = true;
            return sizeof(uint64_t);
        }
    } cli_handler;

    while (!client.isConnected()) {
        client.poll(cli_handler);
    }

    std::vector<uint8_t> send_buf(cfg.payload, 0x55);
    // Warmup messages before pipeline measurement? No: separate pipeline run with fresh count
    int64_t t0 = pollnet_bench::bench_now_ns();
    for (size_t i = 0; i < cfg.throughput_iters; ++i) {
        if (!client.write(send_buf)) {
            std::cerr << std::format("[v2] tcp_tp write failed: {}\n", client.lastError());
            stop.store(true, std::memory_order_relaxed);
            srv_thread.join();
            return false;
        }
    }
    while (!cli_handler.ack_received) {
        client.poll(cli_handler);
        if (!client.isConnected()) {
            std::cerr << std::format("[v2] tcp_tp client closed before ack: {}\n",
                                     client.lastError());
            stop.store(true, std::memory_order_relaxed);
            srv_thread.join();
            return false;
        }
    }
    int64_t t1 = pollnet_bench::bench_now_ns();

    stop.store(true, std::memory_order_relaxed);
    srv_thread.join();

    double elapsed_sec = static_cast<double>(t1 - t0) / 1e9;
    pollnet_bench::print_throughput_result("tcp_throughput", cfg.throughput_iters, cfg.payload,
                                           elapsed_sec, cfg.csv);
    return true;
}

bool run_udp_ping_pong(const pollnet_bench::BenchConfig& cfg) {
    std::atomic<uint16_t> port{0};
    std::atomic<bool> srv_failed{false};
    std::atomic<bool> stop{false};

    std::thread srv_thread([&]() {
        pollnet::UdpReceiver<2048> server;
        if (!server.open({.bind_ip = "127.0.0.1", .bind_port = 0})) {
            std::cerr << std::format("[v2] udp_pp server open failed: {}\n", server.lastError());
            srv_failed.store(true, std::memory_order_release);
            return;
        }
        port.store(server.localPort(), std::memory_order_release);

        while (!stop.load(std::memory_order_relaxed)) {
            [[maybe_unused]] bool got = server.recvFrom(
                [&](std::span<const uint8_t> data, const pollnet::Endpoint& peer) {
                    [[maybe_unused]] bool ok = server.sendTo(data, peer);
                });
        }
    });

    while (port.load(std::memory_order_acquire) == 0 && !srv_failed.load(std::memory_order_acquire)) {
        std::this_thread::yield();
    }
    if (srv_failed.load(std::memory_order_acquire)) {
        srv_thread.join();
        return false;
    }

    pollnet::UdpReceiver<2048> client;
    if (!client.open({.bind_ip = "127.0.0.1", .bind_port = 0})) {
        stop.store(true, std::memory_order_relaxed);
        srv_thread.join();
        return false;
    }

    auto srv_ep = pollnet::Endpoint::loopback(port.load(std::memory_order_acquire));
    std::vector<uint8_t> send_buf(cfg.payload, 0xCD);
    std::vector<int64_t> samples;
    samples.reserve(cfg.iters);

    const size_t total = cfg.warmup + cfg.iters;
    for (size_t i = 0; i < total; ++i) {
        std::memcpy(send_buf.data(), &i, sizeof(uint64_t));
        bool got_reply = false;
        int64_t t0 = pollnet_bench::bench_now_ns();
        if (!client.sendTo(send_buf, srv_ep)) {
            continue;
        }
        while (!got_reply) {
            [[maybe_unused]] bool ok = client.recv([&](std::span<const uint8_t> data) {
                if (data.size() >= sizeof(uint64_t)) {
                    uint64_t seq = 0;
                    std::memcpy(&seq, data.data(), sizeof(uint64_t));
                    if (seq == i) {
                        got_reply = true;
                    }
                }
            });
            if (!got_reply && pollnet_bench::bench_now_ns() - t0 > 50'000'000LL) {
                break;
            }
        }
        if (got_reply && i >= cfg.warmup) {
            int64_t t1 = pollnet_bench::bench_now_ns();
            samples.push_back(t1 - t0);
        }
    }

    stop.store(true, std::memory_order_relaxed);
    srv_thread.join();

    auto stats = pollnet_bench::compute_latency_stats(samples);
    pollnet_bench::print_latency_result("udp_ping_pong", stats, samples.size(), cfg.csv);
    return true;
}

bool run_udp_throughput(const pollnet_bench::BenchConfig& cfg) {
    std::atomic<uint16_t> port{0};
    std::atomic<bool> srv_failed{false};
    std::atomic<bool> sender_done{false};
    std::atomic<size_t> rx_count{0};
    std::atomic<int64_t> last_pkt_ns{0};

    std::thread srv_thread([&]() {
        pollnet::UdpReceiver<2048> server;
        if (!server.open({.bind_ip = "127.0.0.1", .bind_port = 0})) {
            std::cerr << std::format("[v2] udp_tp server open failed: {}\n", server.lastError());
            srv_failed.store(true, std::memory_order_release);
            return;
        }
        port.store(server.localPort(), std::memory_order_release);

        size_t local_rx = 0;
        int64_t local_last = 0;
        int64_t done_ts = 0;
        while (true) {
            bool got = server.recv([&](std::span<const uint8_t>) {
                ++local_rx;
                local_last = pollnet_bench::bench_now_ns();
            });
            if (!got) {
                if (sender_done.load(std::memory_order_acquire)) {
                    int64_t now = pollnet_bench::bench_now_ns();
                    if (done_ts == 0) done_ts = now;
                    if (now - done_ts >= 20'000'000LL || local_rx >= cfg.throughput_iters) {
                        break;
                    }
                }
            } else {
                done_ts = 0;
                if (sender_done.load(std::memory_order_acquire) &&
                    local_rx >= cfg.throughput_iters) {
                    break;
                }
            }
        }
        rx_count.store(local_rx, std::memory_order_release);
        last_pkt_ns.store(local_last, std::memory_order_release);
    });

    while (port.load(std::memory_order_acquire) == 0 && !srv_failed.load(std::memory_order_acquire)) {
        std::this_thread::yield();
    }
    if (srv_failed.load(std::memory_order_acquire)) {
        srv_thread.join();
        return false;
    }

    pollnet::UdpSender sender;
    if (!sender.open({.local_ip = "127.0.0.1",
                      .local_port = 0,
                      .dest_ip = "127.0.0.1",
                      .dest_port = port.load(std::memory_order_acquire)})) {
        sender_done.store(true, std::memory_order_release);
        srv_thread.join();
        return false;
    }

    std::vector<uint8_t> send_buf(cfg.payload, 0x77);
    size_t sent_ok = 0;
    int64_t t0 = pollnet_bench::bench_now_ns();
    for (size_t i = 0; i < cfg.throughput_iters; ++i) {
        std::memcpy(send_buf.data(), &i, sizeof(uint64_t));
        if (sender.send(send_buf)) {
            ++sent_ok;
        }
    }
    int64_t t_send_end = pollnet_bench::bench_now_ns();
    sender_done.store(true, std::memory_order_release);
    srv_thread.join();

    double send_sec = static_cast<double>(t_send_end - t0) / 1e9;
    pollnet_bench::print_throughput_result("udp_tx_throughput", sent_ok, cfg.payload, send_sec,
                                           cfg.csv);

    size_t rx = rx_count.load(std::memory_order_acquire);
    int64_t t_rx_end = last_pkt_ns.load(std::memory_order_acquire);
    double rx_sec = (rx > 0 && t_rx_end > t0) ? static_cast<double>(t_rx_end - t0) / 1e9 : send_sec;
    double loss_pct =
        (cfg.throughput_iters > 0 && rx <= cfg.throughput_iters)
            ? 100.0 * static_cast<double>(cfg.throughput_iters - rx) /
                  static_cast<double>(cfg.throughput_iters)
            : 0.0;
    pollnet_bench::print_throughput_result("udp_rx_throughput", rx, cfg.payload, rx_sec, cfg.csv,
                                           loss_pct);
    return true;
}

}  // namespace

int main(int argc, char** argv) {
    pollnet_bench::BenchConfig cfg;
    if (!pollnet_bench::parse_args(argc, argv, cfg)) {
        return 1;
    }

    if (!cfg.csv) {
        std::cout << std::format(
            "=== pollnet v2.0.0 benchmark (warmup={}, iters={}, tp_iters={}, payload={}B) ===\n",
            cfg.warmup, cfg.iters, cfg.throughput_iters, cfg.payload);
    }

    if (!run_tcp_ping_pong<false>(cfg, "tcp_ping_pong")) return 1;
    if (!run_tcp_ping_pong<true>(cfg, "tcp_ping_pong_ext_clock")) return 1;
    if (!run_tcp_throughput(cfg)) return 1;
    if (!run_udp_ping_pong(cfg)) return 1;
    if (!run_udp_throughput(cfg)) return 1;

    return 0;
}
