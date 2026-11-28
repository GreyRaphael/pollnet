#pragma once

#include <chrono>
#include <format>
#include <iostream>
#include <span>
#include <thread>

namespace pollnet_test {

inline int g_failures = 0;

#define CHECK(cond)                                                                     \
    do {                                                                                \
        if (!(cond)) {                                                                  \
            std::cerr << std::format("[FAIL] {}:{} ({})\n", __FILE__, __LINE__, #cond); \
            ++pollnet_test::g_failures;                                                 \
            return false;                                                               \
        }                                                                               \
    } while (false)

struct TestCase {
    const char* name;
    bool (*fn)();
};

template <typename Pred>
inline bool poll_until(Pred&& pred, int timeout_ms = 2000) {
    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    while (std::chrono::steady_clock::now() < deadline) {
        if (pred()) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return pred();
}

inline int run_test_suite(const char* suite_name, std::span<const TestCase> cases) {
    for (const auto& tc : cases) {
        bool ok = tc.fn();
        std::cout << std::format("[{}] {}\n", ok ? "PASS" : "FAIL", tc.name);
    }

    if (g_failures != 0) {
        std::cerr << std::format("[{}] Total failures: {}\n", suite_name, g_failures);
        return 1;
    }

    std::cout << std::format("[{}] All tests passed!\n", suite_name);
    return 0;
}

}  // namespace pollnet_test
