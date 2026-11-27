#pragma once

// ==========================================
// 跨平台网络核心层 (Cross-Platform Core)
// ==========================================
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#include <cerrno>
#endif

#include <chrono>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <format>
#include <limits>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>

namespace pollnet {

namespace detail {

#ifdef _WIN32
using socket_t = SOCKET;
inline constexpr socket_t INVALID_SOCKET_FD = INVALID_SOCKET;
using sockopt_val_t = const char*;

[[nodiscard]] inline int get_last_error() noexcept { return ::WSAGetLastError(); }
[[nodiscard]] inline bool is_would_block(int err) noexcept { return err == WSAEWOULDBLOCK; }
[[nodiscard]] inline bool is_connect_in_progress(int err) noexcept {
    return err == WSAEWOULDBLOCK || err == WSAEINPROGRESS;
}
[[nodiscard]] inline bool is_eintr(int err) noexcept { return err == WSAEINTR; }
inline void close_socket(socket_t s) noexcept { ::closesocket(s); }
#else
using socket_t = int;
inline constexpr socket_t INVALID_SOCKET_FD = -1;
using sockopt_val_t = const void*;

[[nodiscard]] inline int get_last_error() noexcept { return errno; }
[[nodiscard]] inline bool is_would_block(int err) noexcept {
    return err == EAGAIN || err == EWOULDBLOCK;
}
[[nodiscard]] inline bool is_connect_in_progress(int err) noexcept {
    return err == EINPROGRESS || err == EINTR;
}
[[nodiscard]] inline bool is_eintr(int err) noexcept { return err == EINTR; }
inline void close_socket(socket_t s) noexcept { ::close(s); }
#endif

// C++20 线程安全的全局 WSA 初始化与自动清理助手
[[nodiscard]] inline bool ensure_network_init() noexcept {
#ifdef _WIN32
    struct WsaGuard {
        bool ok = false;
        WsaGuard() noexcept {
            WSADATA wsaData{};
            ok = (::WSAStartup(MAKEWORD(2, 2), &wsaData) == 0);
        }
        ~WsaGuard() {
            if (ok) {
                ::WSACleanup();
            }
        }
    };
    static const WsaGuard guard;
    return guard.ok;
#else
    return true;
#endif
}

#ifndef _WIN32
[[nodiscard]] inline const char* strerror_result(int rc, char* buf) noexcept {
    return (rc == 0) ? buf : "unknown error";
}

[[nodiscard]] inline const char* strerror_result(const char* res, char*) noexcept {
    return res ? res : "unknown error";
}
#endif

// 线程安全的跨平台错误格式化
inline void format_socket_error(char* buf, size_t buf_size, const char* msg, bool check_errno,
                                int err_override = 0) noexcept {
    if (buf_size == 0) return;
    const char* safe_msg = msg ? msg : "";
    if (!check_errno) {
        std::snprintf(buf, buf_size, "%s", safe_msg);
        return;
    }

    int err = (err_override != 0) ? err_override : get_last_error();
#ifdef _WIN32
    char sys_msg[128]{};
    DWORD len = ::FormatMessageA(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                                 nullptr, static_cast<DWORD>(err),
                                 MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT), sys_msg,
                                 static_cast<DWORD>(sizeof(sys_msg)), nullptr);
    while (len > 0 &&
           (sys_msg[len - 1] == '\n' || sys_msg[len - 1] == '\r' || sys_msg[len - 1] == ' ')) {
        sys_msg[--len] = '\0';
    }
    if (len > 0) {
        std::snprintf(buf, buf_size, "%s: %s (WSA_ERR:%d)", safe_msg, sys_msg, err);
    } else {
        std::snprintf(buf, buf_size, "%s (WSA_ERR:%d)", safe_msg, err);
    }
#else
    char sys_msg[128]{};
    const char* err_str = strerror_result(::strerror_r(err, sys_msg, sizeof(sys_msg)), sys_msg);
    std::snprintf(buf, buf_size, "%s: %s", safe_msg, err_str);
#endif
}

[[nodiscard]] constexpr int64_t to_timeout_ns(std::chrono::milliseconds d) noexcept {
    if (d.count() <= 0) return 0;
    constexpr int64_t kMaxMs = std::numeric_limits<int64_t>::max() / 1'000'000LL;
    if (d.count() >= kMaxMs) return std::numeric_limits<int64_t>::max();
    return d.count() * 1'000'000LL;
}

}  // namespace detail

// ==========================================
// 单调时钟与网络端点抽象 (Clock & Endpoint)
// ==========================================

[[nodiscard]] inline int64_t now_ns() noexcept {
    using namespace std::chrono;
    return duration_cast<nanoseconds>(steady_clock::now().time_since_epoch()).count();
}

struct Endpoint {
    struct sockaddr_in raw{};

    constexpr Endpoint() noexcept = default;
    explicit constexpr Endpoint(const struct sockaddr_in& addr) noexcept : raw(addr) {}

    [[nodiscard]] static std::optional<Endpoint> from(std::string_view ip_str,
                                                      uint16_t port_num) noexcept {
        if (ip_str.empty() || ip_str.size() >= INET_ADDRSTRLEN ||
            ip_str.find('\0') != std::string_view::npos) {
            return std::nullopt;
        }
        char buf[INET_ADDRSTRLEN]{};
        std::memcpy(buf, ip_str.data(), ip_str.size());

        Endpoint ep;
        ep.raw.sin_family = AF_INET;
        ep.raw.sin_port = htons(port_num);
        if (::inet_pton(AF_INET, buf, &ep.raw.sin_addr) != 1) {
            return std::nullopt;
        }
        return ep;
    }

    [[nodiscard]] static Endpoint any(uint16_t port_num = 0) noexcept {
        Endpoint ep;
        ep.raw.sin_family = AF_INET;
        ep.raw.sin_addr.s_addr = htonl(INADDR_ANY);
        ep.raw.sin_port = htons(port_num);
        return ep;
    }

    [[nodiscard]] static Endpoint loopback(uint16_t port_num = 0) noexcept {
        Endpoint ep;
        ep.raw.sin_family = AF_INET;
        ep.raw.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        ep.raw.sin_port = htons(port_num);
        return ep;
    }

    [[nodiscard]] uint16_t port() const noexcept { return ntohs(raw.sin_port); }

    [[nodiscard]] std::string ip() const {
        char buf[INET_ADDRSTRLEN]{};
        if (::inet_ntop(AF_INET, &raw.sin_addr, buf, sizeof(buf)) == nullptr) {
            return "<invalid>";
        }
        return std::string(buf);
    }

    [[nodiscard]] std::string to_string() const { return std::format("{}:{}", ip(), port()); }

    [[nodiscard]] bool is_valid() const noexcept { return raw.sin_family == AF_INET; }

    [[nodiscard]] friend bool operator==(const Endpoint& a, const Endpoint& b) noexcept {
        return a.raw.sin_family == b.raw.sin_family && a.raw.sin_port == b.raw.sin_port &&
               a.raw.sin_addr.s_addr == b.raw.sin_addr.s_addr;
    }
};

// ==========================================
// 后端无关配置项与视图 (Options & IoSlice)
// ==========================================

struct EmptyUserData {};

template <typename R>
concept ByteRange = std::ranges::contiguous_range<R> && std::ranges::sized_range<R> &&
                    !std::is_array_v<std::remove_cvref_t<R>> &&
                    (std::same_as<std::remove_cv_t<std::ranges::range_value_t<R>>, uint8_t> ||
                     std::same_as<std::remove_cv_t<std::ranges::range_value_t<R>>, std::byte> ||
                     std::same_as<std::remove_cv_t<std::ranges::range_value_t<R>>, char>);

struct IoSlice {
    const void* data = nullptr;
    size_t size = 0;

    constexpr IoSlice() noexcept = default;
    constexpr IoSlice(const void* ptr, size_t len) noexcept : data(ptr), size(len) {}

    template <ByteRange R>
        requires(!std::same_as<std::remove_cvref_t<R>, IoSlice>)
    IoSlice(const R& range) noexcept
        : data(std::ranges::data(range)), size(std::ranges::size(range)) {}
};

struct TcpClientOptions {
    std::string_view host{};
    uint16_t port = 0;
    uint16_t local_port = 0;
    std::string_view interface{};
    std::chrono::milliseconds retry_interval{3000};
    std::chrono::milliseconds connect_timeout{5000};
    std::chrono::milliseconds send_timeout{0};
    std::chrono::milliseconds recv_timeout{0};
    bool tcp_quickack = false;
    int busy_poll_us = 0;
    int recv_buf_bytes = 0;
    int send_buf_bytes = 0;
};

struct TcpServerOptions {
    std::string_view host{"0.0.0.0"};
    uint16_t port = 0;
    std::string_view interface{};
    int backlog = 128;
    std::chrono::milliseconds send_timeout{0};
    std::chrono::milliseconds recv_timeout{0};
    uint32_t accept_poll_interval = 64;
    bool tcp_quickack = false;
    int busy_poll_us = 0;
    int recv_buf_bytes = 0;
    int send_buf_bytes = 0;
};

struct UdpOptions {
    std::string_view bind_ip{"0.0.0.0"};
    uint16_t bind_port = 0;
    std::string_view dest_ip{};
    uint16_t dest_port = 0;
    std::string_view multicast_group_ip{};
    std::string_view multicast_interface_ip{};
    std::string_view interface{};
    int multicast_ttl = 64;
    int busy_poll_us = 0;
    int recv_buf_bytes = 0;
    int send_buf_bytes = 0;
};

// ==========================================
// C++20 概念契约与可选回调分发 (Concepts)
// ==========================================

template <typename Conn>
concept TcpConnectionLike = requires(Conn& conn, const Conn& cconn, IoSlice slice,
                                     std::span<const IoSlice> slices, const char* reason) {
    { cconn.isConnected() } -> std::convertible_to<bool>;
    { cconn.lastError() } -> std::convertible_to<std::string_view>;
    { cconn.peer() } -> std::same_as<std::optional<Endpoint>>;
    { conn.writeSome(slice) } -> std::convertible_to<int>;
    { conn.writeSome(slices) } -> std::convertible_to<int>;
    { conn.writeNonblock(slice) } -> std::convertible_to<bool>;
    { conn.writeNonblock(slices) } -> std::convertible_to<bool>;
    conn.close(reason);
};

template <typename H, typename Conn>
concept HasOnConnect = requires(H& h, Conn& c) { h.onConnect(c); };

template <typename H, typename Conn>
concept HasOnDisconnect = requires(H& h, Conn& c) { h.onDisconnect(c); };

template <typename H>
concept HasOnConnectFailed = requires(H& h) { h.onConnectFailed(); };

template <typename H, typename Conn>
concept HasOnSendTimeout = requires(H& h, Conn& c) { h.onSendTimeout(c); };

template <typename H, typename Conn>
concept HasOnRecvTimeout = requires(H& h, Conn& c) { h.onRecvTimeout(c); };

template <typename H, typename Conn>
concept HasOnData = requires(H& h, Conn& c, std::span<const uint8_t> data) {
    { h.onData(c, data) } -> std::convertible_to<size_t>;
};

template <typename H>
concept HasAllowNewConnection = requires(H& h, const Endpoint& ep) {
    { h.allowNewConnection(ep) } -> std::convertible_to<bool>;
};

template <typename H, typename Conn>
concept TcpHandler = HasOnData<H, Conn>;

template <typename H>
concept UdpDataHandler = std::invocable<H&, std::span<const uint8_t>>;

template <typename H>
concept UdpPacketHandler = std::invocable<H&, std::span<const uint8_t>, const Endpoint&>;

namespace detail {

template <typename H, typename Conn>
inline void dispatch_connect(H& h, Conn& conn) {
    if constexpr (HasOnConnect<H, Conn>) {
        h.onConnect(conn);
    } else {
        (void)h;
        (void)conn;
    }
}

template <typename H, typename Conn>
inline void dispatch_disconnect(H& h, Conn& conn) {
    if constexpr (HasOnDisconnect<H, Conn>) {
        h.onDisconnect(conn);
    } else {
        (void)h;
        (void)conn;
    }
}

template <typename H>
inline void dispatch_connect_failed(H& h) {
    if constexpr (HasOnConnectFailed<H>) {
        h.onConnectFailed();
    } else {
        (void)h;
    }
}

template <typename H, typename Conn>
inline void dispatch_send_timeout(H& h, Conn& conn) {
    if constexpr (HasOnSendTimeout<H, Conn>) {
        h.onSendTimeout(conn);
    } else {
        (void)h;
        (void)conn;
    }
}

template <typename H, typename Conn>
inline void dispatch_recv_timeout(H& h, Conn& conn) {
    if constexpr (HasOnRecvTimeout<H, Conn>) {
        h.onRecvTimeout(conn);
    } else {
        (void)h;
        (void)conn;
    }
}

template <typename H>
[[nodiscard]] inline bool dispatch_allow_new_connection(H& h, const Endpoint& ep) {
    if constexpr (HasAllowNewConnection<H>) {
        return static_cast<bool>(h.allowNewConnection(ep));
    } else {
        (void)h;
        (void)ep;
        return true;
    }
}

}  // namespace detail

}  // namespace pollnet
