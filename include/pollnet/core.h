#pragma once

// ==========================================
// 跨平台网络核心层 (Cross-Platform Core)
// ==========================================
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#pragma comment(lib, "ws2_32.lib")
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#ifdef __linux__
#include <sched.h>
#endif
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#include <cerrno>
#endif

#include <algorithm>
#include <array>
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
#include <utility>

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
// 单调时钟、线程绑核与网络端点抽象
// ==========================================

[[nodiscard]] inline int64_t now_ns() noexcept {
    using namespace std::chrono;
    return duration_cast<nanoseconds>(steady_clock::now().time_since_epoch()).count();
}

// 将当前线程绑定到指定 CPU 核心；失败或平台不支持时返回 false
inline bool pinThread(int cpu_id) noexcept {
    if (cpu_id < 0) return false;
#ifdef _WIN32
    if (cpu_id >= static_cast<int>(sizeof(DWORD_PTR) * 8)) return false;
    return ::SetThreadAffinityMask(::GetCurrentThread(), DWORD_PTR{1} << cpu_id) != 0;
#elif defined(__linux__)
    if (cpu_id >= CPU_SETSIZE) return false;
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(cpu_id, &cpuset);
    return ::sched_setaffinity(0, sizeof(cpu_set_t), &cpuset) == 0;
#else
    return false;
#endif
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

template <typename T>
concept TriviallyCopyablePart = requires {
    typename std::remove_cvref_t<T>;
} && !std::same_as<std::remove_cvref_t<T>, IoSlice> && !ByteRange<std::remove_cvref_t<T>> &&
    !std::ranges::range<std::remove_cvref_t<T>> && !std::is_pointer_v<std::remove_cvref_t<T>> &&
    !std::is_array_v<std::remove_cvref_t<T>> &&
    std::is_trivially_copyable_v<std::remove_cvref_t<T>>;

template <typename T>
concept SendPart = std::same_as<std::remove_cvref_t<T>, IoSlice> ||
                   ByteRange<std::remove_cvref_t<T>> || TriviallyCopyablePart<T>;

struct TcpClientOptions {
    std::string_view host{};
    uint16_t port = 0;
    uint16_t local_port = 0;
    std::string_view iface{};
    std::chrono::milliseconds retry_interval{3000};
    std::chrono::milliseconds connect_timeout{5000};
    std::chrono::milliseconds send_timeout{0};
    std::chrono::milliseconds recv_timeout{0};
    bool tcp_quickack = false;
    int busy_poll_us = 0;
    int recv_buf_bytes = 0;
    int send_buf_bytes = 0;

    [[nodiscard]] static constexpr TcpClientOptions lowLatency(std::string_view target_host,
                                                               uint16_t target_port) noexcept {
        TcpClientOptions opts{};
        opts.host = target_host;
        opts.port = target_port;
        opts.retry_interval = std::chrono::milliseconds{1000};
        opts.connect_timeout = std::chrono::milliseconds{3000};
        opts.tcp_quickack = true;
        opts.busy_poll_us = 50;
        opts.recv_buf_bytes = 256 * 1024;
        opts.send_buf_bytes = 256 * 1024;
        return opts;
    }
};

struct TcpServerOptions {
    std::string_view host{"0.0.0.0"};
    uint16_t port = 0;
    std::string_view iface{};
    int backlog = 128;
    std::chrono::milliseconds send_timeout{0};
    std::chrono::milliseconds recv_timeout{0};
    uint32_t accept_poll_interval = 64;
    bool tcp_quickack = false;
    int busy_poll_us = 0;
    int recv_buf_bytes = 0;
    int send_buf_bytes = 0;

    [[nodiscard]] static constexpr TcpServerOptions lowLatency(std::string_view bind_host,
                                                               uint16_t listen_port) noexcept {
        TcpServerOptions opts{};
        opts.host = bind_host;
        opts.port = listen_port;
        opts.backlog = 128;
        opts.accept_poll_interval = 64;
        opts.tcp_quickack = true;
        opts.busy_poll_us = 50;
        opts.recv_buf_bytes = 256 * 1024;
        opts.send_buf_bytes = 256 * 1024;
        return opts;
    }
};

struct UdpOptions {
    std::string_view bind_ip{"0.0.0.0"};
    uint16_t bind_port = 0;
    std::string_view dest_ip{};
    uint16_t dest_port = 0;
    std::string_view multicast_group_ip{};
    std::string_view multicast_interface_ip{};
    std::string_view iface{};
    int multicast_ttl = 64;
    int busy_poll_us = 0;
    int recv_buf_bytes = 0;
    int send_buf_bytes = 0;

    [[nodiscard]] static constexpr UdpOptions lowLatencyRecv(
        std::string_view bind_addr, uint16_t port, std::string_view mcast_group = {},
        std::string_view mcast_iface = {}) noexcept {
        UdpOptions opts{};
        opts.bind_ip = bind_addr;
        opts.bind_port = port;
        opts.multicast_group_ip = mcast_group;
        opts.multicast_interface_ip = mcast_iface;
        opts.busy_poll_us = 50;
        opts.recv_buf_bytes = 1024 * 1024;
        return opts;
    }

    [[nodiscard]] static constexpr UdpOptions lowLatencySend(
        std::string_view dest_addr, uint16_t port, uint16_t local_bind_port = 0) noexcept {
        UdpOptions opts{};
        opts.bind_ip = "0.0.0.0";
        opts.bind_port = local_bind_port;
        opts.dest_ip = dest_addr;
        opts.dest_port = port;
        opts.multicast_ttl = 1;
        opts.send_buf_bytes = 256 * 1024;
        return opts;
    }
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

template <SendPart T>
[[nodiscard]] inline IoSlice make_io_slice(const T& part) noexcept {
    using CleanT = std::remove_cvref_t<T>;
    if constexpr (std::same_as<CleanT, IoSlice>) {
        return part;
    } else if constexpr (ByteRange<CleanT>) {
        return IoSlice{part};
    } else {
        return IoSlice{&part, sizeof(CleanT)};
    }
}

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

// 共享可选生命周期回调转发基类，避免在各个拆帧适配器中重复转发代码
template <typename Handler>
class HandlerForwarder {
   public:
    explicit HandlerForwarder(Handler& handler) noexcept : handler_(handler) {}

    template <typename Conn>
        requires HasOnConnect<Handler, Conn>
    void onConnect(Conn& conn) {
        handler_.onConnect(conn);
    }

    template <typename Conn>
        requires HasOnDisconnect<Handler, Conn>
    void onDisconnect(Conn& conn) {
        handler_.onDisconnect(conn);
    }

    void onConnectFailed()
        requires HasOnConnectFailed<Handler>
    {
        handler_.onConnectFailed();
    }

    template <typename Conn>
        requires HasOnSendTimeout<Handler, Conn>
    void onSendTimeout(Conn& conn) {
        handler_.onSendTimeout(conn);
    }

    template <typename Conn>
        requires HasOnRecvTimeout<Handler, Conn>
    void onRecvTimeout(Conn& conn) {
        handler_.onRecvTimeout(conn);
    }

    bool allowNewConnection(const Endpoint& ep)
        requires HasAllowNewConnection<Handler>
    {
        return static_cast<bool>(handler_.allowNewConnection(ep));
    }

   protected:
    Handler& handler_;
};

}  // namespace detail

// ==========================================
// 零开销编译期拆帧适配器 (Framers)
// ==========================================

template <typename MsgT, typename Handler>
class FixedFrameHandler : public detail::HandlerForwarder<Handler> {
    static_assert(std::is_trivially_copyable_v<MsgT> && sizeof(MsgT) > 0,
                  "MsgT must be non-empty trivially copyable");

   public:
    explicit FixedFrameHandler(Handler& handler) noexcept
        : detail::HandlerForwarder<Handler>(handler) {}

    template <typename Conn>
        requires requires(Handler& h, Conn& c, const MsgT& msg) { h.onMessage(c, msg); }
    size_t onData(Conn& conn, std::span<const uint8_t> data) {
        size_t offset = 0;
        while (data.size() - offset >= sizeof(MsgT)) {
            MsgT msg{};
            std::memcpy(&msg, data.data() + offset, sizeof(MsgT));
            this->handler_.onMessage(conn, msg);
            if (!conn.isConnected()) {
                return data.size();
            }
            offset += sizeof(MsgT);
        }
        return offset;
    }
};

template <typename MsgT, typename Handler>
[[nodiscard]] inline FixedFrameHandler<MsgT, Handler> makeFixedFrameHandler(Handler& handler) noexcept {
    return FixedFrameHandler<MsgT, Handler>{handler};
}

template <typename HeaderT, typename LenFn, typename Handler>
class LengthPrefixedHandler : public detail::HandlerForwarder<Handler> {
    static_assert(std::is_trivially_copyable_v<HeaderT> && sizeof(HeaderT) > 0,
                  "HeaderT must be non-empty trivially copyable");
    static_assert(std::invocable<LenFn&, const HeaderT&>,
                  "LenFn must be invocable with const HeaderT&");

    using LenRet = std::remove_cvref_t<std::invoke_result_t<LenFn&, const HeaderT&>>;
    static_assert(std::integral<LenRet>, "LenFn must return an integral body length");

   public:
    LengthPrefixedHandler(LenFn len_fn, Handler& handler, size_t max_body_len = 0) noexcept(
        std::is_nothrow_move_constructible_v<LenFn>)
        : detail::HandlerForwarder<Handler>(handler),
          len_fn_(std::move(len_fn)),
          max_body_len_(max_body_len) {}

    template <typename Conn>
        requires(
            requires(Handler& h, Conn& c, const HeaderT& hdr, std::span<const uint8_t> body) {
                h.onMessage(c, hdr, body);
            } ||
            requires(Handler& h, Conn& c, std::span<const uint8_t> body) {
                h.onMessage(c, body);
            })
    size_t onData(Conn& conn, std::span<const uint8_t> data) {
        size_t max_body = max_body_len_;
        if constexpr (requires { Conn::kRecvBufSize; }) {
            if constexpr (Conn::kRecvBufSize > sizeof(HeaderT)) {
                constexpr size_t kCap = Conn::kRecvBufSize - sizeof(HeaderT);
                max_body = (max_body == 0 || max_body > kCap) ? kCap : max_body;
            } else {
                max_body = 0;
            }
        } else if (max_body == 0) {
            max_body = std::numeric_limits<size_t>::max() - sizeof(HeaderT);
        }

        size_t offset = 0;
        while (data.size() - offset >= sizeof(HeaderT)) {
            HeaderT hdr{};
            std::memcpy(&hdr, data.data() + offset, sizeof(HeaderT));

            LenRet raw_len = len_fn_(hdr);
            if constexpr (std::signed_integral<LenRet>) {
                if (raw_len < 0) {
                    conn.close("invalid message length", false);
                    return data.size();
                }
            }
            size_t body_len = static_cast<size_t>(raw_len);
            if (body_len > max_body) {
                conn.close("message too large", false);
                return data.size();
            }

            if (data.size() - offset - sizeof(HeaderT) < body_len) {
                break;
            }

            std::span<const uint8_t> body{data.data() + offset + sizeof(HeaderT), body_len};
            if constexpr (requires(Handler& h, Conn& c, const HeaderT& hd,
                                   std::span<const uint8_t> b) { h.onMessage(c, hd, b); }) {
                this->handler_.onMessage(conn, hdr, body);
            } else {
                this->handler_.onMessage(conn, body);
            }
            if (!conn.isConnected()) {
                return data.size();
            }
            offset += sizeof(HeaderT) + body_len;
        }
        return offset;
    }

   private:
    LenFn len_fn_;
    size_t max_body_len_ = 0;
};

template <typename HeaderT, typename LenFn, typename Handler>
[[nodiscard]] inline LengthPrefixedHandler<HeaderT, LenFn, Handler> makeLengthPrefixedHandler(
    LenFn len_fn, Handler& handler, size_t max_body_len = 0)
    noexcept(std::is_nothrow_move_constructible_v<LenFn>) {
    return LengthPrefixedHandler<HeaderT, LenFn, Handler>{std::move(len_fn), handler, max_body_len};
}

template <typename Handler>
class DelimiterFrameHandler : public detail::HandlerForwarder<Handler> {
   public:
    static constexpr size_t kMaxDelimLen = 16;

    DelimiterFrameHandler(char delim, Handler& handler, size_t max_frame_len = 0,
                          bool include_delimiter = false) noexcept
        : detail::HandlerForwarder<Handler>(handler),
          delim_len_(1),
          max_frame_len_(max_frame_len),
          include_delimiter_(include_delimiter) {
        delim_[0] = static_cast<uint8_t>(delim);
    }

    DelimiterFrameHandler(std::string_view delim, Handler& handler, size_t max_frame_len = 0,
                          bool include_delimiter = false) noexcept
        : detail::HandlerForwarder<Handler>(handler),
          max_frame_len_(max_frame_len),
          include_delimiter_(include_delimiter) {
        if (!delim.empty() && delim.size() <= kMaxDelimLen) {
            delim_len_ = delim.size();
            std::memcpy(delim_, delim.data(), delim_len_);
        }
    }

    template <typename Conn>
        requires requires(Handler& h, Conn& c, std::span<const uint8_t> frame) {
            h.onMessage(c, frame);
        }
    size_t onData(Conn& conn, std::span<const uint8_t> data) {
        if (delim_len_ == 0) {
            conn.close("invalid delimiter", false);
            return data.size();
        }

        size_t max_frame = max_frame_len_;
        if constexpr (requires { Conn::kRecvBufSize; }) {
            if (max_frame == 0 || max_frame > Conn::kRecvBufSize) {
                max_frame = Conn::kRecvBufSize;
            }
        } else if (max_frame == 0) {
            max_frame = std::numeric_limits<size_t>::max();
        }

        size_t offset = 0;
        while (data.size() - offset >= delim_len_) {
            size_t rem = data.size() - offset;
            size_t pos = rem;

            if (delim_len_ == 1) {
                const void* hit = std::memchr(data.data() + offset, delim_[0], rem);
                if (hit != nullptr) {
                    pos = static_cast<size_t>(static_cast<const uint8_t*>(hit) -
                                              (data.data() + offset));
                }
            } else {
                std::string_view hay{reinterpret_cast<const char*>(data.data() + offset), rem};
                std::string_view needle{reinterpret_cast<const char*>(delim_), delim_len_};
                size_t found = hay.find(needle);
                if (found != std::string_view::npos) {
                    pos = found;
                }
            }

            if (pos == rem) {
                size_t max_unmatched =
                    include_delimiter_
                        ? (max_frame > 0 ? max_frame - 1 : 0)
                        : (max_frame <= std::numeric_limits<size_t>::max() - (delim_len_ - 1)
                               ? max_frame + (delim_len_ - 1)
                               : std::numeric_limits<size_t>::max());
                if (rem > max_unmatched) {
                    conn.close("frame too large", false);
                    return data.size();
                }
                break;
            }

            size_t frame_total = pos + delim_len_;
            size_t emit_len = include_delimiter_ ? frame_total : pos;
            if (emit_len > max_frame) {
                conn.close("frame too large", false);
                return data.size();
            }

            this->handler_.onMessage(conn,
                                     std::span<const uint8_t>{data.data() + offset, emit_len});
            if (!conn.isConnected()) {
                return data.size();
            }
            offset += frame_total;
        }
        return offset;
    }

   private:
    uint8_t delim_[kMaxDelimLen]{};
    size_t delim_len_ = 0;
    size_t max_frame_len_ = 0;
    bool include_delimiter_ = false;
};

template <typename Handler>
[[nodiscard]] inline DelimiterFrameHandler<Handler> makeDelimiterHandler(
    char delim, Handler& handler, size_t max_frame_len = 0,
    bool include_delimiter = false) noexcept {
    return DelimiterFrameHandler<Handler>{delim, handler, max_frame_len, include_delimiter};
}

template <typename Handler>
[[nodiscard]] inline DelimiterFrameHandler<Handler> makeDelimiterHandler(
    std::string_view delim, Handler& handler, size_t max_frame_len = 0,
    bool include_delimiter = false) noexcept {
    return DelimiterFrameHandler<Handler>{delim, handler, max_frame_len, include_delimiter};
}

}  // namespace pollnet
