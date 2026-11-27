#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <format>
#include <iostream>
#include <thread>
#include <vector>

#include "bench_common.h"
#include "pollnet/socket.h"

namespace {

struct PingPongConf {
    static constexpr uint32_t RecvBufSize = 4096;
    static constexpr uint32_t MaxConns = 4;
    static constexpr uint32_t ConnRetrySec = 1;
    static constexpr uint32_t ConnTimeoutSec = 5;
    static constexpr uint32_t SendTimeoutSec = 0;
    static constexpr uint32_t RecvTimeoutSec = 0;
    struct UserData {};
};

struct ThroughputServerConf {
    static constexpr uint32_t RecvBufSize = 65536;
    static constexpr uint32_t MaxConns = 4;
    static constexpr uint32_t ConnRetrySec = 1;
    static constexpr uint32_t ConnTimeoutSec = 5;
    static constexpr uint32_t SendTimeoutSec = 0;
    static constexpr uint32_t RecvTimeoutSec = 0;
    struct UserData {};
};

bool run_tcp_ping_pong(const pollnet_bench::BenchConfig& cfg) {
    using Server = pollnet::SocketTcpServer<PingPongConf>;
    using Client = pollnet::SocketTcpClient<PingPongConf>;

    std::atomic<uint16_t> port{0};
    std::atomic<bool> srv_failed{false};
    std::atomic<bool> stop{false};

    std::thread srv_thread([&]() {
        pollnet_bench::pin_current_thread(cfg.server_cpu, "server");
        Server server;
        if (!server.init("", "127.0.0.1", 0)) {
            std::cerr << std::format("[v1] server init failed: {}\n", server.getLastError());
            srv_failed.store(true, std::memory_order_release);
            return;
        }
        port.store(server.getLocalPort(), std::memory_order_release);

        struct EchoHandler {
            uint32_t payload_size;
            void onTcpConnected(Server::Conn&) {}
            void onTcpDisconnect(Server::Conn&) {}
            void onSendTimeout(Server::Conn&) {}
            void onRecvTimeout(Server::Conn&) {}
            uint32_t onTcpData(Server::Conn& conn, const uint8_t* data, uint32_t size) {
                while (size >= payload_size) {
                    [[maybe_unused]] bool ok = conn.writeNonblock(data, payload_size);
                    data += payload_size;
                    size -= payload_size;
                }
                return size;
            }
        } handler{static_cast<uint32_t>(cfg.payload)};

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
    if (!client.init("", "127.0.0.1", port.load(std::memory_order_acquire))) {
        std::cerr << std::format("[v1] client init failed: {}\n", client.getLastError());
        stop.store(true, std::memory_order_relaxed);
        srv_thread.join();
        return false;
    }

    struct ClientHandler {
        uint32_t payload_size;
        bool got_reply = false;
        void onTcpConnectFailed() {}
        void onTcpConnected(Client::Conn&) {}
        void onTcpDisconnect(Client::Conn&) {}
        void onSendTimeout(Client::Conn&) {}
        void onRecvTimeout(Client::Conn&) {}
        uint32_t onTcpData(Client::Conn&, const uint8_t*, uint32_t size) {
            if (size < payload_size) return size;
            got_reply = true;
            return size - payload_size;
        }
    } cli_handler{static_cast<uint32_t>(cfg.payload)};

    while (!client.isConnected()) {
        client.poll(cli_handler);
    }

    std::vector<uint8_t> send_buf(cfg.payload, 0xAB);
    std::vector<int64_t> samples;
    samples.reserve(cfg.iters);

    const size_t total = cfg.warmup + cfg.iters;
    for (size_t i = 0; i < total; ++i) {
        std::memcpy(send_buf.data(), &i, sizeof(uint64_t));
        cli_handler.got_reply = false;
        int64_t t0 = pollnet_bench::bench_now_ns();
        if (!client.writeNonblock(send_buf.data(), static_cast<uint32_t>(send_buf.size()))) {
            std::cerr << std::format("[v1] writeNonblock failed: {}\n", client.getLastError());
            stop.store(true, std::memory_order_relaxed);
            srv_thread.join();
            return false;
        }
        while (!cli_handler.got_reply) {
            client.poll(cli_handler);
            if (!client.isConnected()) {
                std::cerr << std::format("[v1] client disconnected: {}\n", client.getLastError());
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
    pollnet_bench::print_latency_result("tcp_ping_pong", stats, samples.size(), cfg.csv);
    return true;
}

bool run_tcp_throughput(const pollnet_bench::BenchConfig& cfg) {
    using Server = pollnet::SocketTcpServer<ThroughputServerConf>;
    using Client = pollnet::SocketTcpClient<PingPongConf>;

    std::atomic<uint16_t> port{0};
    std::atomic<bool> srv_failed{false};
    std::atomic<bool> stop{false};

    std::thread srv_thread([&]() {
        pollnet_bench::pin_current_thread(cfg.server_cpu, "server");
        Server server;
        if (!server.init("", "127.0.0.1", 0)) {
            std::cerr << std::format("[v1] tcp_tp server init failed: {}\n", server.getLastError());
            srv_failed.store(true, std::memory_order_release);
            return;
        }
        port.store(server.getLocalPort(), std::memory_order_release);

        struct DrainHandler {
            uint32_t payload_size;
            size_t target_msgs;
            size_t received = 0;
            bool ack_sent = false;
            void onTcpConnected(Server::Conn&) {}
            void onTcpDisconnect(Server::Conn&) {}
            void onSendTimeout(Server::Conn&) {}
            void onRecvTimeout(Server::Conn&) {}
            uint32_t onTcpData(Server::Conn& conn, const uint8_t*, uint32_t size) {
                uint32_t count = size / payload_size;
                received += count;
                if (!ack_sent && received >= target_msgs) {
                    uint64_t ack = received;
                    [[maybe_unused]] bool ok = conn.writeNonblock(&ack, sizeof(ack));
                    ack_sent = true;
                }
                return size - count * payload_size;
            }
        } handler{static_cast<uint32_t>(cfg.payload), cfg.throughput_iters};

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
    if (!client.init("", "127.0.0.1", port.load(std::memory_order_acquire))) {
        stop.store(true, std::memory_order_relaxed);
        srv_thread.join();
        return false;
    }

    struct AckHandler {
        bool ack_received = false;
        void onTcpConnectFailed() {}
        void onTcpConnected(Client::Conn&) {}
        void onTcpDisconnect(Client::Conn&) {}
        void onSendTimeout(Client::Conn&) {}
        void onRecvTimeout(Client::Conn&) {}
        uint32_t onTcpData(Client::Conn&, const uint8_t*, uint32_t size) {
            if (size < sizeof(uint64_t)) return size;
            ack_received = true;
            return size - static_cast<uint32_t>(sizeof(uint64_t));
        }
    } cli_handler;

    while (!client.isConnected()) {
        client.poll(cli_handler);
    }

    std::vector<uint8_t> send_buf(cfg.payload, 0x55);
    int64_t t0 = pollnet_bench::bench_now_ns();
    for (size_t i = 0; i < cfg.throughput_iters; ++i) {
        if (!client.write(send_buf.data(), static_cast<uint32_t>(send_buf.size()))) {
            std::cerr << std::format("[v1] tcp_tp write failed: {}\n", client.getLastError());
            stop.store(true, std::memory_order_relaxed);
            srv_thread.join();
            return false;
        }
    }
    while (!cli_handler.ack_received) {
        client.poll(cli_handler);
        if (!client.isConnected()) {
            std::cerr << std::format("[v1] tcp_tp client closed before ack: {}\n",
                                     client.getLastError());
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
        pollnet_bench::pin_current_thread(cfg.server_cpu, "server");
        pollnet::SocketUdpReceiver<2048> server;
        if (!server.init("", "127.0.0.1", 0)) {
            std::cerr << std::format("[v1] udp_pp server init failed: {}\n", server.getLastError());
            srv_failed.store(true, std::memory_order_release);
            return;
        }
        port.store(server.getLocalPort(), std::memory_order_release);

        while (!stop.load(std::memory_order_relaxed)) {
            [[maybe_unused]] bool got = server.recvfrom(
                [&](const uint8_t* data, uint32_t size, const sockaddr_in& peer) {
                    [[maybe_unused]] bool ok = server.sendto(data, size, peer);
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

    pollnet::SocketUdpReceiver<2048> client;
    if (!client.init("", "127.0.0.1", 0)) {
        stop.store(true, std::memory_order_relaxed);
        srv_thread.join();
        return false;
    }

    sockaddr_in srv_addr{};
    srv_addr.sin_family = AF_INET;
    srv_addr.sin_port = htons(port.load(std::memory_order_acquire));
    if (::inet_pton(AF_INET, "127.0.0.1", &srv_addr.sin_addr) != 1) {
        stop.store(true, std::memory_order_relaxed);
        srv_thread.join();
        return false;
    }

    std::vector<uint8_t> send_buf(cfg.payload, 0xCD);
    std::vector<int64_t> samples;
    samples.reserve(cfg.iters);

    const size_t total = cfg.warmup + cfg.iters;
    for (size_t i = 0; i < total; ++i) {
        std::memcpy(send_buf.data(), &i, sizeof(uint64_t));
        bool got_reply = false;
        int64_t t0 = pollnet_bench::bench_now_ns();
        if (!client.sendto(send_buf.data(), static_cast<uint32_t>(send_buf.size()), srv_addr)) {
            continue;
        }
        while (!got_reply) {
            [[maybe_unused]] bool ok = client.read([&](const uint8_t* data, uint32_t size) {
                if (size >= sizeof(uint64_t)) {
                    uint64_t seq = 0;
                    std::memcpy(&seq, data, sizeof(uint64_t));
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
        pollnet_bench::pin_current_thread(cfg.server_cpu, "server");
        pollnet::SocketUdpReceiver<2048> server;
        if (!server.init("", "127.0.0.1", 0)) {
            std::cerr << std::format("[v1] udp_tp server init failed: {}\n", server.getLastError());
            srv_failed.store(true, std::memory_order_release);
            return;
        }
        port.store(server.getLocalPort(), std::memory_order_release);

        size_t local_rx = 0;
        int64_t local_last = 0;
        int64_t done_ts = 0;
        while (true) {
            bool got = server.read([&](const uint8_t*, uint32_t) {
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

    pollnet::SocketUdpSender sender;
    if (!sender.init("", "127.0.0.1", 0, "127.0.0.1", port.load(std::memory_order_acquire))) {
        sender_done.store(true, std::memory_order_release);
        srv_thread.join();
        return false;
    }

    std::vector<uint8_t> send_buf(cfg.payload, 0x77);
    size_t sent_ok = 0;
    int64_t t0 = pollnet_bench::bench_now_ns();
    for (size_t i = 0; i < cfg.throughput_iters; ++i) {
        std::memcpy(send_buf.data(), &i, sizeof(uint64_t));
        if (sender.write(send_buf.data(), static_cast<uint32_t>(send_buf.size()))) {
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

    pollnet_bench::pin_current_thread(cfg.client_cpu, "client");

    if (!cfg.csv) {
        std::cout << std::format(
            "=== pollnet v1.1.0 benchmark (warmup={}, iters={}, tp_iters={}, payload={}B, "
            "server_cpu={}, client_cpu={}) ===\n",
            cfg.warmup, cfg.iters, cfg.throughput_iters, cfg.payload, cfg.server_cpu,
            cfg.client_cpu);
    }

    if (!run_tcp_ping_pong(cfg)) return 1;
    if (!run_tcp_throughput(cfg)) return 1;
    if (!run_udp_ping_pong(cfg)) return 1;
    if (!run_udp_throughput(cfg)) return 1;

    return 0;
}
