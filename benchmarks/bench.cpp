#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <format>
#include <iostream>
#include <memory>
#include <span>
#include <string_view>
#include <thread>
#include <vector>

#include "bench_common.h"
#include "pollnet/eventloop.h"
#include "pollnet/socket.h"

namespace {

template <template <typename, size_t, size_t> class ServerT, bool UseExtClock = false,
          bool PassTimeoutArg = false>
bool run_tcp_ping_pong_server(const pollnet_bench::BenchConfig& cfg, std::string_view label,
                              bool quickack = false) {
    using Server = ServerT<pollnet::EmptyUserData, 4096, 4>;
    using Client = pollnet::TcpClient<pollnet::EmptyUserData, 4096>;

    std::atomic<uint16_t> port{0};
    std::atomic<bool> srv_failed{false};
    std::atomic<bool> stop{false};

    std::thread srv_thread([&]() {
        pollnet_bench::pin_current_thread(cfg.server_cpu, "server");
        Server server;
        if (!server.listen({.host = "127.0.0.1", .port = 0, .tcp_quickack = quickack})) {
            std::cerr << std::format("[bench] {} listen failed: {}\n", label, server.lastError());
            srv_failed.store(true, std::memory_order_release);
            return;
        }
        port.store(server.localPort(), std::memory_order_release);

        struct EchoHandler {
            size_t payload_size;
            size_t onData(typename Server::Conn& conn, std::span<const uint8_t> data) {
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
            if constexpr (PassTimeoutArg) {
                server.poll(handler, 0);
            } else if constexpr (UseExtClock) {
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
    if (!client.connect({.host = "127.0.0.1",
                         .port = port.load(std::memory_order_acquire),
                         .tcp_quickack = quickack})) {
        std::cerr << std::format("[bench] {} client connect failed: {}\n", label,
                                 client.lastError());
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
            std::cerr << std::format("[bench] {} writeNonblock failed: {}\n", label,
                                     client.lastError());
            stop.store(true, std::memory_order_relaxed);
            srv_thread.join();
            return false;
        }
        while (!cli_handler.got_reply) {
            poll_client();
            if (!client.isConnected()) {
                std::cerr << std::format("[bench] {} client disconnected: {}\n", label,
                                         client.lastError());
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

template <template <typename, size_t, size_t> class ServerT, bool PassTimeoutArg>
bool run_tcp_64conn_ping_pong(const pollnet_bench::BenchConfig& cfg, std::string_view label) {
    constexpr size_t kNumConns = 64;
    using Server = ServerT<pollnet::EmptyUserData, 4096, kNumConns>;
    using Client = pollnet::TcpClient<pollnet::EmptyUserData, 4096>;

    std::atomic<uint16_t> port{0};
    std::atomic<bool> srv_failed{false};
    std::atomic<bool> stop{false};

    std::thread srv_thread([&]() {
        pollnet_bench::pin_current_thread(cfg.server_cpu, "server");
        Server server;
        if (!server.listen({.host = "127.0.0.1", .port = 0})) {
            std::cerr << std::format("[bench] {} listen failed: {}\n", label, server.lastError());
            srv_failed.store(true, std::memory_order_release);
            return;
        }
        port.store(server.localPort(), std::memory_order_release);

        struct EchoHandler {
            size_t payload_size;
            size_t onData(typename Server::Conn& conn, std::span<const uint8_t> data) {
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
            if constexpr (PassTimeoutArg) {
                server.poll(handler, 0);
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

    uint16_t srv_port = port.load(std::memory_order_acquire);
    std::vector<std::unique_ptr<Client>> clients(kNumConns);
    for (size_t i = 0; i < kNumConns; ++i) {
        clients[i] = std::make_unique<Client>();
        if (!clients[i]->connect({.host = "127.0.0.1", .port = srv_port})) {
            stop.store(true, std::memory_order_relaxed);
            srv_thread.join();
            return false;
        }
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

    while (true) {
        size_t connected_cnt = 0;
        for (auto& c : clients) {
            if (!c->isConnected()) {
                c->poll(cli_handler);
            }
            if (c->isConnected()) ++connected_cnt;
        }
        if (connected_cnt == kNumConns) break;
    }

    const size_t warmup = std::min<size_t>(cfg.warmup, 1000);
    const size_t iters = std::min<size_t>(cfg.iters, 20000);
    std::vector<uint8_t> send_buf(cfg.payload, 0xAB);
    std::vector<int64_t> samples;
    samples.reserve(iters);

    Client& active = *clients[0];
    for (size_t i = 0; i < warmup + iters; ++i) {
        std::memcpy(send_buf.data(), &i, sizeof(uint64_t));
        cli_handler.got_reply = false;
        int64_t t0 = pollnet_bench::bench_now_ns();
        if (!active.writeNonblock(send_buf)) {
            stop.store(true, std::memory_order_relaxed);
            srv_thread.join();
            return false;
        }
        while (!cli_handler.got_reply) {
            active.poll(cli_handler);
            if (!active.isConnected()) {
                stop.store(true, std::memory_order_relaxed);
                srv_thread.join();
                return false;
            }
        }
        int64_t t1 = pollnet_bench::bench_now_ns();
        if (i >= warmup) {
            samples.push_back(t1 - t0);
        }
    }

    stop.store(true, std::memory_order_relaxed);
    srv_thread.join();

    auto stats = pollnet_bench::compute_latency_stats(samples);
    pollnet_bench::print_latency_result(label, stats, samples.size(), cfg.csv);
    return true;
}

template <bool UseWritev>
bool run_tcp_throughput(const pollnet_bench::BenchConfig& cfg, std::string_view label) {
    using Server = pollnet::SocketTcpServer<pollnet::EmptyUserData, 65536, 4>;
    using Client = pollnet::TcpClient<pollnet::EmptyUserData, 4096>;
    constexpr size_t kWritevBatch = 16;
    const size_t target_msgs =
        UseWritev ? (cfg.throughput_iters / kWritevBatch) * kWritevBatch : cfg.throughput_iters;

    std::atomic<uint16_t> port{0};
    std::atomic<bool> srv_failed{false};
    std::atomic<bool> stop{false};

    std::thread srv_thread([&]() {
        pollnet_bench::pin_current_thread(cfg.server_cpu, "server");
        Server server;
        if (!server.listen({.host = "127.0.0.1", .port = 0})) {
            std::cerr << std::format("[bench] {} listen failed: {}\n", label, server.lastError());
            srv_failed.store(true, std::memory_order_release);
            return;
        }
        port.store(server.localPort(), std::memory_order_release);

        struct DrainHandler {
            size_t payload_size;
            size_t expected_msgs;
            size_t received = 0;
            bool ack_sent = false;
            size_t onData(Server::Conn& conn, std::span<const uint8_t> data) {
                size_t count = data.size() / payload_size;
                received += count;
                if (!ack_sent && received >= expected_msgs) {
                    uint64_t ack = received;
                    [[maybe_unused]] bool ok = conn.writeNonblock(&ack, sizeof(ack));
                    ack_sent = true;
                }
                return count * payload_size;
            }
        } handler{cfg.payload, target_msgs};

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
    uint64_t hdr_val = 42;
    std::vector<uint8_t> body_buf(cfg.payload - sizeof(hdr_val), 0x66);
    std::array<pollnet::IoSlice, kWritevBatch * 2> batch_slices{};
    for (size_t i = 0; i < kWritevBatch; ++i) {
        batch_slices[i * 2] = pollnet::IoSlice{&hdr_val, sizeof(hdr_val)};
        batch_slices[i * 2 + 1] = pollnet::IoSlice{body_buf};
    }

    int64_t t0 = pollnet_bench::bench_now_ns();
    if constexpr (UseWritev) {
        for (size_t i = 0; i < target_msgs; i += kWritevBatch) {
            if (!client.write(batch_slices)) {
                std::cerr << std::format("[bench] {} write failed: {}\n", label,
                                         client.lastError());
                stop.store(true, std::memory_order_relaxed);
                srv_thread.join();
                return false;
            }
        }
    } else {
        for (size_t i = 0; i < target_msgs; ++i) {
            if (!client.write(send_buf)) {
                std::cerr << std::format("[bench] {} write failed: {}\n", label,
                                         client.lastError());
                stop.store(true, std::memory_order_relaxed);
                srv_thread.join();
                return false;
            }
        }
    }
    while (!cli_handler.ack_received) {
        client.poll(cli_handler);
        if (!client.isConnected()) {
            std::cerr << std::format("[bench] {} client closed before ack: {}\n", label,
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
    pollnet_bench::print_throughput_result(label, target_msgs, cfg.payload, elapsed_sec, cfg.csv);
    return true;
}

bool run_udp_ping_pong(const pollnet_bench::BenchConfig& cfg) {
    std::atomic<uint16_t> port{0};
    std::atomic<bool> srv_failed{false};
    std::atomic<bool> stop{false};

    std::thread srv_thread([&]() {
        pollnet_bench::pin_current_thread(cfg.server_cpu, "server");
        pollnet::UdpSocket<2048> server;
        if (!server.open({.bind_ip = "127.0.0.1", .bind_port = 0})) {
            std::cerr << std::format("[bench] udp_pp server open failed: {}\n",
                                     server.lastError());
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

    pollnet::UdpSocket<2048> client;
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

template <bool UseBatch>
bool run_udp_throughput(const pollnet_bench::BenchConfig& cfg, std::string_view tx_label,
                        std::string_view rx_label) {
    constexpr size_t kBatchSize = 16;
    const size_t target_msgs =
        UseBatch ? (cfg.throughput_iters / kBatchSize) * kBatchSize : cfg.throughput_iters;

    std::atomic<uint16_t> port{0};
    std::atomic<bool> srv_failed{false};
    std::atomic<bool> sender_done{false};
    std::atomic<size_t> rx_count{0};
    std::atomic<int64_t> last_pkt_ns{0};

    std::thread srv_thread([&]() {
        pollnet_bench::pin_current_thread(cfg.server_cpu, "server");
        pollnet::UdpSocket<2048> server;
        if (!server.open({.bind_ip = "127.0.0.1", .bind_port = 0})) {
            std::cerr << std::format("[bench] {} server open failed: {}\n", rx_label,
                                     server.lastError());
            srv_failed.store(true, std::memory_order_release);
            return;
        }
        port.store(server.localPort(), std::memory_order_release);

        size_t local_rx = 0;
        int64_t local_last = 0;
        int64_t done_ts = 0;
        while (true) {
            bool got_any = false;
            if constexpr (UseBatch) {
                size_t n = server.recvBatch<kBatchSize>([&](std::span<const uint8_t>) {
                    ++local_rx;
                });
                if (n > 0) {
                    got_any = true;
                    local_last = pollnet_bench::bench_now_ns();
                }
            } else {
                got_any = server.recv([&](std::span<const uint8_t>) {
                    ++local_rx;
                    local_last = pollnet_bench::bench_now_ns();
                });
            }

            if (!got_any) {
                if (sender_done.load(std::memory_order_acquire)) {
                    int64_t now = pollnet_bench::bench_now_ns();
                    if (done_ts == 0) done_ts = now;
                    if (now - done_ts >= 20'000'000LL || local_rx >= target_msgs) {
                        break;
                    }
                }
            } else {
                done_ts = 0;
                if (sender_done.load(std::memory_order_acquire) && local_rx >= target_msgs) {
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

    pollnet::UdpSocket<2048> sender;
    if (!sender.open({.bind_ip = "127.0.0.1",
                      .bind_port = 0,
                      .dest_ip = "127.0.0.1",
                      .dest_port = port.load(std::memory_order_acquire)})) {
        sender_done.store(true, std::memory_order_release);
        srv_thread.join();
        return false;
    }

    std::vector<uint8_t> send_buf(cfg.payload, 0x77);
    std::vector<uint8_t> batch_storage(kBatchSize * cfg.payload, 0x77);
    std::array<pollnet::IoSlice, kBatchSize> batch_slices{};
    for (size_t i = 0; i < kBatchSize; ++i) {
        batch_slices[i] = pollnet::IoSlice{batch_storage.data() + i * cfg.payload, cfg.payload};
    }

    size_t sent_ok = 0;
    int64_t t0 = pollnet_bench::bench_now_ns();
    if constexpr (UseBatch) {
        for (size_t i = 0; i < target_msgs; i += kBatchSize) {
            sent_ok += sender.sendBatch(batch_slices);
        }
    } else {
        for (size_t i = 0; i < target_msgs; ++i) {
            std::memcpy(send_buf.data(), &i, sizeof(uint64_t));
            if (sender.send(send_buf)) {
                ++sent_ok;
            }
        }
    }
    int64_t t_send_end = pollnet_bench::bench_now_ns();
    sender_done.store(true, std::memory_order_release);
    srv_thread.join();

    double send_sec = static_cast<double>(t_send_end - t0) / 1e9;
    pollnet_bench::print_throughput_result(tx_label, sent_ok, cfg.payload, send_sec, cfg.csv);

    size_t rx = rx_count.load(std::memory_order_acquire);
    int64_t t_rx_end = last_pkt_ns.load(std::memory_order_acquire);
    double rx_sec = (rx > 0 && t_rx_end > t0) ? static_cast<double>(t_rx_end - t0) / 1e9 : send_sec;
    double loss_pct =
        (target_msgs > 0 && rx <= target_msgs)
            ? 100.0 * static_cast<double>(target_msgs - rx) / static_cast<double>(target_msgs)
            : 0.0;
    pollnet_bench::print_throughput_result(rx_label, rx, cfg.payload, rx_sec, cfg.csv, loss_pct);
    return true;
}

}  // namespace

int main(int argc, char** argv) {
    pollnet_bench::BenchConfig cfg;
    if (!pollnet_bench::parse_args(argc, argv, cfg)) {
        return 1;
    }

    pollnet_bench::pin_current_thread(cfg.client_cpu, "client");

    if (!cfg.csv) {
        std::cout << std::format(
            "=== pollnet v2.0.2 benchmark (warmup={}, iters={}, tp_iters={}, payload={}B, "
            "server_cpu={}, client_cpu={}) ===\n",
            cfg.warmup, cfg.iters, cfg.throughput_iters, cfg.payload, cfg.server_cpu,
            cfg.client_cpu);
    }

    if (!run_tcp_ping_pong_server<pollnet::SocketTcpServer, false, false>(cfg, "tcp_ping_pong"))
        return 1;
    if (!run_tcp_ping_pong_server<pollnet::SocketTcpServer, true, false>(
            cfg, "tcp_ping_pong_ext_clock"))
        return 1;
    if (!run_tcp_ping_pong_server<pollnet::EventLoopTcpServer, false, true>(
            cfg, "tcp_ping_pong_epoll"))
        return 1;
    if (!run_tcp_throughput<false>(cfg, "tcp_throughput")) return 1;
    if (!run_tcp_throughput<true>(cfg, "tcp_writev_throughput")) return 1;
    if (!run_udp_ping_pong(cfg)) return 1;
    if (!run_udp_throughput<false>(cfg, "udp_tx_throughput", "udp_rx_throughput")) return 1;
    if (!run_udp_throughput<true>(cfg, "udp_batch_tx_throughput", "udp_batch_rx_throughput"))
        return 1;
    if (!run_tcp_64conn_ping_pong<pollnet::SocketTcpServer, false>(cfg, "tcp_64conn_busy_poll"))
        return 1;
    if (!run_tcp_64conn_ping_pong<pollnet::EventLoopTcpServer, true>(cfg, "tcp_64conn_epoll"))
        return 1;

    return 0;
}
