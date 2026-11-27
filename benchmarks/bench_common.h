#pragma once

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#elif defined(__linux__)
#include <pthread.h>
#include <sched.h>
#endif

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <format>
#include <iostream>
#include <numeric>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace pollnet_bench {

struct BenchConfig {
    size_t warmup = 2000;
    size_t iters = 50000;
    size_t throughput_iters = 200000;
    size_t payload = 64;
    int server_cpu = -1;
    int client_cpu = -1;
    bool csv = false;
};

[[nodiscard]] inline int64_t bench_now_ns() noexcept {
    using namespace std::chrono;
    return duration_cast<nanoseconds>(steady_clock::now().time_since_epoch()).count();
}

inline bool pin_current_thread(int cpu_id, std::string_view role = "") noexcept {
    if (cpu_id < 0) return true;
#ifdef _WIN32
    if (cpu_id >= static_cast<int>(sizeof(DWORD_PTR) * 8)) {
        std::cerr << std::format("[warn] invalid cpu_id {} for {}\n", cpu_id, role);
        return false;
    }
    DWORD_PTR mask = (DWORD_PTR{1} << cpu_id);
    if (::SetThreadAffinityMask(::GetCurrentThread(), mask) == 0) {
        std::cerr << std::format("[warn] SetThreadAffinityMask({}) failed for {} (err={})\n",
                                 cpu_id, role, ::GetLastError());
        return false;
    }
    return true;
#elif defined(__linux__)
    if (cpu_id >= CPU_SETSIZE) {
        std::cerr << std::format("[warn] cpu_id {} >= CPU_SETSIZE for {}\n", cpu_id, role);
        return false;
    }
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(cpu_id, &cpuset);
    int rc = ::pthread_setaffinity_np(::pthread_self(), sizeof(cpu_set_t), &cpuset);
    if (rc != 0) {
        std::cerr << std::format("[warn] pthread_setaffinity_np({}) failed for {} ({})\n",
                                 cpu_id, role, std::generic_category().message(rc));
        return false;
    }
    return true;
#else
    (void)role;
    return false;
#endif
}

inline void print_usage(const char* prog) {
    std::cout << std::format(
        "Usage: {} [--warmup N] [--iters N] [--throughput-iters N] [--payload BYTES] "
        "[--server-cpu ID] [--client-cpu ID] [--csv]\n",
        prog);
}

[[nodiscard]] inline bool parse_size_arg(const char* str, size_t& out) {
    if (!str || str[0] == '\0') return false;
    char* end = nullptr;
    unsigned long long val = std::strtoull(str, &end, 10);
    if (end == str || *end != '\0') return false;
    out = static_cast<size_t>(val);
    return true;
}

[[nodiscard]] inline bool parse_int_arg(const char* str, int& out) {
    if (!str || str[0] == '\0') return false;
    char* end = nullptr;
    long val = std::strtol(str, &end, 10);
    if (end == str || *end != '\0') return false;
    out = static_cast<int>(val);
    return true;
}

[[nodiscard]] inline bool parse_args(int argc, char** argv, BenchConfig& cfg) {
    for (int i = 1; i < argc; ++i) {
        std::string_view arg = argv[i];
        if (arg == "--csv") {
            cfg.csv = true;
        } else if (arg == "--help" || arg == "-h") {
            print_usage(argv[0]);
            std::exit(0);
        } else if (arg == "--warmup" && i + 1 < argc) {
            if (!parse_size_arg(argv[++i], cfg.warmup)) return false;
        } else if (arg == "--iters" && i + 1 < argc) {
            if (!parse_size_arg(argv[++i], cfg.iters) || cfg.iters == 0) return false;
        } else if (arg == "--throughput-iters" && i + 1 < argc) {
            if (!parse_size_arg(argv[++i], cfg.throughput_iters) || cfg.throughput_iters == 0) {
                return false;
            }
        } else if (arg == "--payload" && i + 1 < argc) {
            if (!parse_size_arg(argv[++i], cfg.payload)) return false;
        } else if (arg == "--server-cpu" && i + 1 < argc) {
            if (!parse_int_arg(argv[++i], cfg.server_cpu)) return false;
        } else if (arg == "--client-cpu" && i + 1 < argc) {
            if (!parse_int_arg(argv[++i], cfg.client_cpu)) return false;
        } else {
            std::cerr << std::format("Unknown or incomplete option: {}\n", arg);
            print_usage(argv[0]);
            return false;
        }
    }
    cfg.payload = std::clamp<size_t>(cfg.payload, 16, 1400);
    return true;
}

struct LatencyStats {
    int64_t min_ns = 0;
    double avg_ns = 0.0;
    int64_t p50_ns = 0;
    int64_t p90_ns = 0;
    int64_t p99_ns = 0;
    int64_t p999_ns = 0;
    int64_t max_ns = 0;
};

[[nodiscard]] inline LatencyStats compute_latency_stats(std::vector<int64_t>& samples) {
    LatencyStats st{};
    if (samples.empty()) return st;

    std::sort(samples.begin(), samples.end());
    auto quantile = [&](double q) -> int64_t {
        size_t idx = static_cast<size_t>(static_cast<double>(samples.size() - 1) * q);
        if (idx >= samples.size()) idx = samples.size() - 1;
        return samples[idx];
    };

    double sum = std::accumulate(samples.begin(), samples.end(), 0.0);
    st.min_ns = samples.front();
    st.avg_ns = sum / static_cast<double>(samples.size());
    st.p50_ns = quantile(0.50);
    st.p90_ns = quantile(0.90);
    st.p99_ns = quantile(0.99);
    st.p999_ns = quantile(0.999);
    st.max_ns = samples.back();
    return st;
}

inline void print_latency_result(std::string_view name, const LatencyStats& st, size_t count,
                                 bool csv) {
    if (csv) {
        std::cout << std::format("RESULT|{}|min|{}|ns\n", name, st.min_ns);
        std::cout << std::format("RESULT|{}|avg|{:.1f}|ns\n", name, st.avg_ns);
        std::cout << std::format("RESULT|{}|p50|{}|ns\n", name, st.p50_ns);
        std::cout << std::format("RESULT|{}|p90|{}|ns\n", name, st.p90_ns);
        std::cout << std::format("RESULT|{}|p99|{}|ns\n", name, st.p99_ns);
        std::cout << std::format("RESULT|{}|p999|{}|ns\n", name, st.p999_ns);
        std::cout << std::format("RESULT|{}|max|{}|ns\n", name, st.max_ns);
        std::cout << std::format("RESULT|{}|samples|{}|cnt\n", name, count);
    } else {
        std::cout << std::format(
            "{:<26} samples={:<7} min={:<6} avg={:<8.1f} p50={:<6} p90={:<6} p99={:<7} "
            "p99.9={:<7} max={} ns\n",
            name, count, st.min_ns, st.avg_ns, st.p50_ns, st.p90_ns, st.p99_ns, st.p999_ns,
            st.max_ns);
    }
}

inline void print_throughput_result(std::string_view name, size_t msgs, size_t payload,
                                      double elapsed_sec, bool csv, double loss_pct = -1.0) {
    double safe_sec = (elapsed_sec > 1e-9) ? elapsed_sec : 1e-9;
    double msg_per_sec = static_cast<double>(msgs) / safe_sec;
    double mb_per_sec =
        (static_cast<double>(msgs) * static_cast<double>(payload)) / (safe_sec * 1024.0 * 1024.0);

    if (csv) {
        std::cout << std::format("RESULT|{}|msg_per_sec|{:.0f}|msg/s\n", name, msg_per_sec);
        std::cout << std::format("RESULT|{}|mb_per_sec|{:.2f}|MB/s\n", name, mb_per_sec);
        if (loss_pct >= 0.0) {
            std::cout << std::format("RESULT|{}|loss_pct|{:.2f}|%\n", name, loss_pct);
        }
    } else {
        if (loss_pct >= 0.0) {
            std::cout << std::format(
                "{:<26} msgs={:<7} payload={}B rate={:.0f} msg/s ({:.2f} MB/s) loss={:.2f}%\n",
                name, msgs, payload, msg_per_sec, mb_per_sec, loss_pct);
        } else {
            std::cout << std::format(
                "{:<26} msgs={:<7} payload={}B rate={:.0f} msg/s ({:.2f} MB/s)\n", name, msgs,
                payload, msg_per_sec, mb_per_sec);
        }
    }
}

}  // namespace pollnet_bench
