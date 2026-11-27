#pragma once

// ==========================================
// 跨平台网络底层抽象 (Cross-Platform Abstraction)
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
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#include <cerrno>
#endif

#include <algorithm>
#include <chrono>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <span>
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

[[nodiscard]] inline int64_t now_ms() noexcept {
    using namespace std::chrono;
    return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

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
    while (len > 0 && (sys_msg[len - 1] == '\n' || sys_msg[len - 1] == '\r' || sys_msg[len - 1] == ' ')) {
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

// 统一设置非阻塞模式
[[nodiscard]] inline bool set_nonblocking(socket_t fd) noexcept {
#ifdef _WIN32
    u_long mode = 1;
    return ::ioctlsocket(fd, FIONBIO, &mode) == 0;
#else
    int flags = ::fcntl(fd, F_GETFL, 0);
    if (flags < 0) return false;
    return ::fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0;
#endif
}

#ifdef _WIN32
inline void disable_udp_connreset(socket_t fd) noexcept {
#ifndef SIO_UDP_CONNRESET
#define SIO_UDP_CONNRESET _WSAIOW(IOC_VENDOR, 12)
#endif
    BOOL new_behavior = FALSE;
    DWORD bytes_returned = 0;
    ::WSAIoctl(fd, SIO_UDP_CONNRESET, &new_behavior, sizeof(new_behavior), nullptr, 0,
               &bytes_returned, nullptr, nullptr);
}
#endif

// 非阻塞 connect 状态探测：返回 1 已连接，0 进行中，-1 失败（out_err 写入具体错误码）
[[nodiscard]] inline int check_connect_status(socket_t fd, int& out_err) noexcept {
    out_err = 0;
#ifdef _WIN32
    fd_set wfds;
    fd_set efds;
    FD_ZERO(&wfds);
    FD_ZERO(&efds);
    FD_SET(fd, &wfds);
    FD_SET(fd, &efds);
    timeval tv{0, 0};
    int ready = ::select(0, nullptr, &wfds, &efds, &tv);
    if (ready < 0) {
        out_err = get_last_error();
        return -1;
    }
    if (ready == 0) {
        return 0;
    }
    int so_err = 0;
    int len = static_cast<int>(sizeof(so_err));
    if (::getsockopt(fd, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&so_err), &len) < 0) {
        out_err = get_last_error();
        return -1;
    }
    if (so_err != 0) {
        out_err = so_err;
        return -1;
    }
    if (FD_ISSET(fd, &efds)) {
        out_err = WSAECONNREFUSED;
        return -1;
    }
    return FD_ISSET(fd, &wfds) ? 1 : 0;
#else
    struct pollfd pfd{};
    pfd.fd = fd;
    pfd.events = POLLOUT;
    int ready = ::poll(&pfd, 1, 0);
    if (ready < 0) {
        int err = get_last_error();
        if (err == EINTR) return 0;
        out_err = err;
        return -1;
    }
    if (ready == 0) {
        return 0;
    }
    int so_err = 0;
    socklen_t len = sizeof(so_err);
    if (::getsockopt(fd, SOL_SOCKET, SO_ERROR, &so_err, &len) < 0) {
        out_err = get_last_error();
        return -1;
    }
    if (so_err != 0) {
        out_err = so_err;
        return -1;
    }
    if (pfd.revents & (POLLERR | POLLHUP | POLLNVAL)) {
        out_err = ECONNREFUSED;
        return -1;
    }
    return (pfd.revents & POLLOUT) ? 1 : 0;
#endif
}

}  // namespace detail

// ==========================================
// C++20 概念约束 (Concepts)
// ==========================================

template <typename Conf>
concept TcpConnConfig = requires {
    typename Conf::UserData;
    requires std::convertible_to<decltype(Conf::RecvBufSize), uint32_t>;
    requires std::convertible_to<decltype(Conf::SendTimeoutSec), uint32_t>;
    requires std::convertible_to<decltype(Conf::RecvTimeoutSec), uint32_t>;
    requires(Conf::RecvBufSize > 0);
    requires(Conf::RecvBufSize <= static_cast<uint32_t>(std::numeric_limits<int>::max()));
};

template <typename Conf>
concept TcpClientConfig = TcpConnConfig<Conf> && requires {
    requires std::convertible_to<decltype(Conf::ConnRetrySec), uint32_t>;
    requires std::convertible_to<decltype(Conf::ConnTimeoutSec), uint32_t>;
};

template <typename Conf>
concept TcpServerConfig = TcpConnConfig<Conf> && requires {
    requires std::convertible_to<decltype(Conf::MaxConns), uint32_t>;
    requires(Conf::MaxConns > 0);
};

template <typename H, typename Conn>
concept TcpClientHandler =
    requires(H& h, Conn& conn, const uint8_t* data, uint32_t size) {
        h.onTcpConnectFailed();
        h.onTcpConnected(conn);
        h.onTcpDisconnect(conn);
        h.onSendTimeout(conn);
        h.onRecvTimeout(conn);
        { h.onTcpData(conn, data, size) } -> std::convertible_to<uint32_t>;
    };

template <typename H, typename Conn>
concept TcpServerHandler =
    requires(H& h, Conn& conn, const uint8_t* data, uint32_t size) {
        h.onTcpConnected(conn);
        h.onTcpDisconnect(conn);
        h.onSendTimeout(conn);
        h.onRecvTimeout(conn);
        { h.onTcpData(conn, data, size) } -> std::convertible_to<uint32_t>;
    };

template <typename H>
concept UdpReadHandler = std::invocable<H, const uint8_t*, uint32_t>;

template <typename H>
concept UdpRecvFromHandler = std::invocable<H, const uint8_t*, uint32_t, const sockaddr_in&>;

// ==========================================
// 业务逻辑实现 (Business Logic)
// ==========================================

template <TcpServerConfig Conf>
class SocketTcpServer;

template <TcpConnConfig Conf>
class SocketTcpConnection : public Conf::UserData {
   public:
    SocketTcpConnection() = default;
    SocketTcpConnection(const SocketTcpConnection&) = delete;
    SocketTcpConnection& operator=(const SocketTcpConnection&) = delete;
    SocketTcpConnection(SocketTcpConnection&&) = delete;
    SocketTcpConnection& operator=(SocketTcpConnection&&) = delete;

    ~SocketTcpConnection() { close("destruct"); }

    [[nodiscard]] const char* getLastError() const noexcept { return last_error_; }

    [[nodiscard]] bool isConnected() const noexcept { return fd_ != detail::INVALID_SOCKET_FD; }

    [[nodiscard]] bool getPeername(struct sockaddr_in& addr) const noexcept {
        if (fd_ == detail::INVALID_SOCKET_FD) return false;
        socklen_t addr_len = sizeof(addr);
        return ::getpeername(fd_, reinterpret_cast<struct sockaddr*>(&addr), &addr_len) == 0;
    }

    void close(const char* reason, bool check_errno = false, int err_override = 0) noexcept {
        if (fd_ != detail::INVALID_SOCKET_FD) {
            saveError(reason, check_errno, err_override);
            detail::close_socket(fd_);
            fd_ = detail::INVALID_SOCKET_FD;
        }
    }

    [[nodiscard]] int writeSome(const void* data, uint32_t size, bool more = false) noexcept {
        if (fd_ == detail::INVALID_SOCKET_FD) return -1;
        if (size == 0) return 0;
        if (size > static_cast<uint32_t>(std::numeric_limits<int>::max())) {
            close("send size overflow", false);
            return -1;
        }

        int flags = 0;
#ifndef _WIN32
        flags |= MSG_NOSIGNAL;
        if (more) flags |= MSG_MORE;
#else
        (void)more;
#endif
        int ret = 0;
        do {
            ret = ::send(fd_, reinterpret_cast<const char*>(data), static_cast<int>(size), flags);
        } while (ret < 0 && detail::is_eintr(detail::get_last_error()));

        if (ret < 0) {
            int err = detail::get_last_error();
            if (detail::is_would_block(err)) {
                return 0;
            }
            close("send error", true, err);
            return -1;
        }
        if constexpr (Conf::SendTimeoutSec > 0) {
            if (ret > 0) {
                send_ts_ = detail::now_ms();
            }
        }
        return ret;
    }

    [[nodiscard]] int writeSome(std::span<const std::byte> data, bool more = false) noexcept {
        if (data.size() > static_cast<size_t>(std::numeric_limits<int>::max())) {
            close("send size overflow", false);
            return -1;
        }
        return writeSome(data.data(), static_cast<uint32_t>(data.size()), more);
    }

    [[nodiscard]] int writeSome(std::span<const uint8_t> data, bool more = false) noexcept {
        return writeSome(std::as_bytes(data), more);
    }

    [[nodiscard]] bool write(const void* data_, uint32_t size, bool more = false) noexcept {
        if (size == 0) return isConnected();
        const uint8_t* data = static_cast<const uint8_t*>(data_);
        do {
            int sent = writeSome(data, size, more);
            if (sent < 0) return false;
            data += sent;
            size -= static_cast<uint32_t>(sent);
        } while (size != 0);
        return true;
    }

    [[nodiscard]] bool write(std::span<const std::byte> data, bool more = false) noexcept {
        if (data.size() > static_cast<size_t>(std::numeric_limits<int>::max())) {
            close("send size overflow", false);
            return false;
        }
        return write(data.data(), static_cast<uint32_t>(data.size()), more);
    }

    [[nodiscard]] bool write(std::span<const uint8_t> data, bool more = false) noexcept {
        return write(std::as_bytes(data), more);
    }

    [[nodiscard]] bool writeNonblock(const void* data, uint32_t size, bool more = false) noexcept {
        if (size == 0) return isConnected();
        int sent = writeSome(data, size, more);
        if (sent < 0) return false;
        if (sent != static_cast<int>(size)) {
            close("send buffer full", false);
            return false;
        }
        return true;
    }

    [[nodiscard]] bool writeNonblock(std::span<const std::byte> data, bool more = false) noexcept {
        if (data.size() > static_cast<size_t>(std::numeric_limits<int>::max())) {
            close("send size overflow", false);
            return false;
        }
        return writeNonblock(data.data(), static_cast<uint32_t>(data.size()), more);
    }

    [[nodiscard]] bool writeNonblock(std::span<const uint8_t> data, bool more = false) noexcept {
        return writeNonblock(std::as_bytes(data), more);
    }

   protected:
    template <TcpServerConfig ServerConf>
    friend class SocketTcpServer;

    template <typename Handler>
    void pollConn(int64_t now, Handler& handler) {
        if constexpr (Conf::SendTimeoutSec > 0) {
            constexpr int64_t kSendTimeoutMs = static_cast<int64_t>(Conf::SendTimeoutSec) * 1000LL;
            if (now >= send_ts_ + kSendTimeoutMs) {
                handler.onSendTimeout(*this);
                send_ts_ = now;
            }
        }
        bool got_data = read(
            [&](const uint8_t* data, uint32_t size) { return handler.onTcpData(*this, data, size); });
        if constexpr (Conf::RecvTimeoutSec > 0) {
            constexpr int64_t kRecvTimeoutMs = static_cast<int64_t>(Conf::RecvTimeoutSec) * 1000LL;
            if (isConnected()) {
                if (!got_data && now >= expire_ts_) {
                    handler.onRecvTimeout(*this);
                    got_data = true;
                }
                if (got_data) expire_ts_ = now + kRecvTimeoutMs;
            }
        }
    }

    template <typename Handler>
        requires std::invocable<Handler&, const uint8_t*, uint32_t> &&
                 std::convertible_to<std::invoke_result_t<Handler&, const uint8_t*, uint32_t>, uint32_t>
    bool read(Handler handler) {
        if (fd_ == detail::INVALID_SOCKET_FD) return false;
        if (tail_ >= Conf::RecvBufSize) {
            close("recv buf full", false);
            return false;
        }

        int ret = 0;
        do {
            ret = ::recv(fd_, reinterpret_cast<char*>(recvbuf_ + tail_),
                         static_cast<int>(Conf::RecvBufSize - tail_), 0);
        } while (ret < 0 && detail::is_eintr(detail::get_last_error()));

        if (ret <= 0) {
            if (ret < 0) {
                int err = detail::get_last_error();
                if (detail::is_would_block(err)) return false;
                close("read error", true, err);
            } else {
                close("remote close");
            }
            return false;
        }
        tail_ += static_cast<uint32_t>(ret);

        uint32_t avail = tail_ - head_;
        uint32_t remaining = static_cast<uint32_t>(handler(recvbuf_ + head_, avail));
        if (!isConnected()) {
            head_ = tail_ = 0;
            return false;
        }
        if (remaining > avail) {
            close("invalid remaining size", false);
            return false;
        }

        if (remaining == 0) {
            head_ = tail_ = 0;
        } else {
            head_ = tail_ - remaining;
            if (head_ >= Conf::RecvBufSize / 2 || (tail_ == Conf::RecvBufSize && head_ > 0)) {
                std::memmove(recvbuf_, recvbuf_ + head_, remaining);
                head_ = 0;
                tail_ = remaining;
            } else if (tail_ == Conf::RecvBufSize) {
                close("recv buf full", false);
            }
        }
        return true;
    }

    bool open(int64_t now, detail::socket_t fd) noexcept {
        fd_ = fd;
        head_ = tail_ = 0;
        send_ts_ = now;
        expire_ts_ = now + static_cast<int64_t>(Conf::RecvTimeoutSec) * 1000LL;

        if (!detail::set_nonblocking(fd_)) {
            close("set nonblock error", true);
            return false;
        }

        int yes = 1;
        if (::setsockopt(fd_, IPPROTO_TCP, TCP_NODELAY,
                         reinterpret_cast<detail::sockopt_val_t>(&yes), sizeof(yes)) < 0) {
            close("setsockopt TCP_NODELAY error", true);
            return false;
        }

        return true;
    }

    void saveError(const char* msg, bool check_errno, int err_override = 0) noexcept {
        detail::format_socket_error(last_error_, sizeof(last_error_), msg, check_errno, err_override);
    }

    detail::socket_t fd_ = detail::INVALID_SOCKET_FD;
    int64_t send_ts_ = 0;
    int64_t expire_ts_ = 0;
    uint32_t head_ = 0;
    uint32_t tail_ = 0;
    uint8_t recvbuf_[Conf::RecvBufSize]{};
    char last_error_[128] = "";
};

template <TcpClientConfig Conf>
class SocketTcpClient : public SocketTcpConnection<Conf> {
   public:
    using Conn = SocketTcpConnection<Conf>;

    SocketTcpClient() = default;
    SocketTcpClient(const SocketTcpClient&) = delete;
    SocketTcpClient& operator=(const SocketTcpClient&) = delete;
    SocketTcpClient(SocketTcpClient&&) = delete;
    SocketTcpClient& operator=(SocketTcpClient&&) = delete;

    ~SocketTcpClient() { closePendingConnect(); }

    [[nodiscard]] bool init([[maybe_unused]] const char* interface_ip, const char* server_ip,
                            uint16_t server_port, uint16_t local_port = 0) noexcept {
        this->close("reinit");
        closePendingConnect();
        report_disconnect_ = false;
        next_conn_ts_ = std::numeric_limits<int64_t>::min();
        conn_expire_ts_ = 0;
        server_addr_ = {};
        local_port_be_ = 0;

        if (!detail::ensure_network_init()) {
            Conn::saveError("WSAStartup failed", false);
            return false;
        }
        if (!server_ip) {
            Conn::saveError("invalid server_ip", false);
            return false;
        }

        server_addr_.sin_family = AF_INET;
        if (::inet_pton(AF_INET, server_ip, &(server_addr_.sin_addr)) != 1) {
            Conn::saveError("invalid server_ip", false);
            return false;
        }
        server_addr_.sin_port = htons(server_port);
        local_port_be_ = htons(local_port);
        return true;
    }

    void allowReconnect() noexcept { next_conn_ts_ = std::numeric_limits<int64_t>::min(); }

    template <TcpClientHandler<Conn> Handler>
    void poll(Handler& handler) {
        int64_t now = detail::now_ms();
        if (!this->isConnected()) {
            if (report_disconnect_) {
                handler.onTcpDisconnect(*this);
                report_disconnect_ = false;
            }
            int ret = connect(now);
            if (ret <= 0) {
                if (ret < 0) handler.onTcpConnectFailed();
                return;
            }
            report_disconnect_ = true;
            handler.onTcpConnected(*this);
        }
        this->pollConn(now, handler);
    }

   private:
    void closePendingConnect() noexcept {
        if (conn_fd_ != detail::INVALID_SOCKET_FD) {
            detail::close_socket(conn_fd_);
            conn_fd_ = detail::INVALID_SOCKET_FD;
        }
    }

    int connect(int64_t now) noexcept {
        if (conn_fd_ == detail::INVALID_SOCKET_FD) {
            if (now < next_conn_ts_) return 0;
            if constexpr (Conf::ConnRetrySec > 0) {
                next_conn_ts_ = now + static_cast<int64_t>(Conf::ConnRetrySec) * 1000LL;
            } else {
                next_conn_ts_ = std::numeric_limits<int64_t>::max();
            }

            detail::socket_t fd = ::socket(AF_INET, SOCK_STREAM, 0);
            if (fd == detail::INVALID_SOCKET_FD) {
                Conn::saveError("socket error", true);
                return -1;
            }
            if (local_port_be_) {
                struct sockaddr_in local_addr{};
                local_addr.sin_family = AF_INET;
                local_addr.sin_addr.s_addr = INADDR_ANY;
                local_addr.sin_port = local_port_be_;
                if (::bind(fd, reinterpret_cast<struct sockaddr*>(&local_addr), sizeof(local_addr)) < 0) {
                    Conn::saveError("bind error", true);
                    detail::close_socket(fd);
                    return -1;
                }
            }

            if (!detail::set_nonblocking(fd)) {
                Conn::saveError("set nonblock error", true);
                detail::close_socket(fd);
                return -1;
            }

            int ret = ::connect(fd, reinterpret_cast<struct sockaddr*>(&server_addr_), sizeof(server_addr_));
            if (ret == 0) {
                return Conn::open(now, fd) ? 1 : -1;
            }

            int err = detail::get_last_error();
            if (detail::is_connect_in_progress(err)) {
                conn_fd_ = fd;
                if constexpr (Conf::ConnTimeoutSec > 0) {
                    conn_expire_ts_ = now + static_cast<int64_t>(Conf::ConnTimeoutSec) * 1000LL;
                } else {
                    conn_expire_ts_ = std::numeric_limits<int64_t>::max();
                }
                return 0;
            }

            Conn::saveError("connect error", true, err);
            detail::close_socket(fd);
            return -1;
        }

        int conn_err = 0;
        int status = detail::check_connect_status(conn_fd_, conn_err);
        if (status > 0) {
            detail::socket_t fd = conn_fd_;
            conn_fd_ = detail::INVALID_SOCKET_FD;
            return Conn::open(now, fd) ? 1 : -1;
        }
        if (status < 0) {
            Conn::saveError("connect error", true, conn_err);
            closePendingConnect();
            return -1;
        }
        if (now >= conn_expire_ts_) {
            Conn::saveError("connect expired", false);
            closePendingConnect();
            return -1;
        }
        return 0;
    }

    bool report_disconnect_ = false;
    detail::socket_t conn_fd_ = detail::INVALID_SOCKET_FD;
    int64_t next_conn_ts_ = std::numeric_limits<int64_t>::min();
    int64_t conn_expire_ts_ = 0;
    struct sockaddr_in server_addr_{};
    uint16_t local_port_be_ = 0;
};

template <TcpServerConfig Conf>
class SocketTcpServer {
   public:
    using Conn = SocketTcpConnection<Conf>;

    SocketTcpServer() noexcept {
        for (uint32_t i = 0; i < Conf::MaxConns; i++) conns_[i] = conns_data_ + i;
    }
    SocketTcpServer(const SocketTcpServer&) = delete;
    SocketTcpServer& operator=(const SocketTcpServer&) = delete;
    SocketTcpServer(SocketTcpServer&&) = delete;
    SocketTcpServer& operator=(SocketTcpServer&&) = delete;

    ~SocketTcpServer() { close("destruct"); }

    [[nodiscard]] bool init([[maybe_unused]] const char* interface_ip, const char* server_ip,
                            uint16_t server_port) noexcept {
        for (uint32_t i = 0; i < conns_cnt_; i++) {
            conns_[i]->close("reinit");
        }
        conns_cnt_ = 0;
        close("reinit");

        if (!detail::ensure_network_init()) {
            saveError("WSAStartup failed", false);
            return false;
        }
        if (!server_ip) {
            saveError("invalid server_ip", false);
            return false;
        }

        struct sockaddr_in local_addr{};
        local_addr.sin_family = AF_INET;
        if (::inet_pton(AF_INET, server_ip, &(local_addr.sin_addr)) != 1) {
            saveError("invalid server_ip", false);
            return false;
        }
        local_addr.sin_port = htons(server_port);

        listenfd_ = ::socket(AF_INET, SOCK_STREAM, 0);
        if (listenfd_ == detail::INVALID_SOCKET_FD) {
            saveError("socket error", true);
            return false;
        }

        if (!detail::set_nonblocking(listenfd_)) {
            close("set nonblock error", true);
            return false;
        }

        int yes = 1;
        if (::setsockopt(listenfd_, SOL_SOCKET, SO_REUSEADDR,
                         reinterpret_cast<detail::sockopt_val_t>(&yes), sizeof(yes)) < 0) {
            close("setsockopt SO_REUSEADDR error", true);
            return false;
        }

        if (::bind(listenfd_, reinterpret_cast<struct sockaddr*>(&local_addr), sizeof(local_addr)) < 0) {
            close("bind error", true);
            return false;
        }
        if (::listen(listenfd_, 128) < 0) {
            close("listen error", true);
            return false;
        }

        return true;
    }

    void close(const char* reason, bool check_errno = false, int err_override = 0) noexcept {
        if (listenfd_ != detail::INVALID_SOCKET_FD) {
            saveError(reason, check_errno, err_override);
            detail::close_socket(listenfd_);
            listenfd_ = detail::INVALID_SOCKET_FD;
        }
    }

    [[nodiscard]] const char* getLastError() const noexcept { return last_error_; }

    [[nodiscard]] bool isClosed() const noexcept { return listenfd_ == detail::INVALID_SOCKET_FD; }

    [[nodiscard]] uint32_t getConnCnt() const noexcept { return conns_cnt_; }

    [[nodiscard]] uint16_t getLocalPort() const noexcept {
        if (listenfd_ == detail::INVALID_SOCKET_FD) return 0;
        struct sockaddr_in addr{};
        socklen_t addrlen = sizeof(addr);
        if (::getsockname(listenfd_, reinterpret_cast<struct sockaddr*>(&addr), &addrlen) != 0) {
            return 0;
        }
        return ntohs(addr.sin_port);
    }

    template <typename Handler>
        requires std::invocable<Handler&, Conn&>
    void foreachConn(Handler handler) {
        for (uint32_t i = 0; i < conns_cnt_; i++) {
            Conn& conn = *conns_[i];
            handler(conn);
        }
    }

    template <TcpServerHandler<Conn> Handler>
    void poll(Handler& handler) {
        int64_t now = detail::now_ms();
        while (!isClosed() && conns_cnt_ < Conf::MaxConns) {
            struct sockaddr_in clientaddr{};
            socklen_t addr_len = sizeof(clientaddr);
            detail::socket_t fd =
                ::accept(listenfd_, reinterpret_cast<struct sockaddr*>(&clientaddr), &addr_len);
            if (fd == detail::INVALID_SOCKET_FD) {
                if (detail::is_eintr(detail::get_last_error())) continue;
                break;
            }
            Conn& conn = *conns_[conns_cnt_];
            if (conn.open(now, fd)) {
                conns_cnt_++;
                handler.onTcpConnected(conn);
            }
        }
        for (uint32_t i = 0; i < conns_cnt_;) {
            Conn& conn = *conns_[i];
            conn.pollConn(now, handler);
            if (conn.isConnected()) {
                i++;
            } else {
                std::swap(conns_[i], conns_[--conns_cnt_]);
                handler.onTcpDisconnect(conn);
            }
        }
    }

   private:
    void saveError(const char* msg, bool check_errno = true, int err_override = 0) noexcept {
        detail::format_socket_error(last_error_, sizeof(last_error_), msg, check_errno, err_override);
    }

    detail::socket_t listenfd_ = detail::INVALID_SOCKET_FD;
    uint32_t conns_cnt_ = 0;
    Conn* conns_[Conf::MaxConns]{};
    Conn conns_data_[Conf::MaxConns];
    char last_error_[128] = "";
};

template <uint32_t RecvBufSize = 1500>
class SocketUdpReceiver {
    static_assert(RecvBufSize > 0 &&
                      RecvBufSize <= static_cast<uint32_t>(std::numeric_limits<int>::max()),
                  "RecvBufSize must be in (0, INT_MAX]");

   public:
    SocketUdpReceiver() = default;
    SocketUdpReceiver(const SocketUdpReceiver&) = delete;
    SocketUdpReceiver& operator=(const SocketUdpReceiver&) = delete;
    SocketUdpReceiver(SocketUdpReceiver&&) = delete;
    SocketUdpReceiver& operator=(SocketUdpReceiver&&) = delete;

    ~SocketUdpReceiver() { close("destruct"); }

    [[nodiscard]] bool init([[maybe_unused]] const char* interface_ip, const char* dest_ip,
                            uint16_t dest_port, const char* subscribe_ip = "") noexcept {
        close("reinit");

        if (!detail::ensure_network_init()) {
            saveError("WSAStartup failed", false);
            return false;
        }
        if (!dest_ip) {
            saveError("invalid dest_ip", false);
            return false;
        }

        struct in_addr dest_in_addr{};
        if (::inet_pton(AF_INET, dest_ip, &dest_in_addr) != 1) {
            saveError("invalid dest_ip", false);
            return false;
        }

        bool enable_multicast = (subscribe_ip != nullptr && subscribe_ip[0] != '\0');
        struct in_addr sub_in_addr{};
        if (enable_multicast && ::inet_pton(AF_INET, subscribe_ip, &sub_in_addr) != 1) {
            saveError("invalid subscribe_ip", false);
            return false;
        }

        if ((fd_ = ::socket(AF_INET, SOCK_DGRAM, 0)) == detail::INVALID_SOCKET_FD) {
            saveError("socket error", true);
            return false;
        }

        if (!detail::set_nonblocking(fd_)) {
            close("set nonblock error", true);
            return false;
        }

#ifdef _WIN32
        detail::disable_udp_connreset(fd_);
#endif

        int optval = 1;
        if (::setsockopt(fd_, SOL_SOCKET, SO_REUSEADDR,
                         reinterpret_cast<detail::sockopt_val_t>(&optval), sizeof(int)) < 0) {
            close("setsockopt SO_REUSEADDR error", true);
            return false;
        }

        struct sockaddr_in servaddr{};
        servaddr.sin_family = AF_INET;
        servaddr.sin_port = htons(dest_port);
#ifdef _WIN32
        // Windows 下不允许将 UDP socket 直接 bind 到组播地址，需绑定 INADDR_ANY
        servaddr.sin_addr.s_addr = enable_multicast ? INADDR_ANY : dest_in_addr.s_addr;
#else
        servaddr.sin_addr = dest_in_addr;
#endif

        if (::bind(fd_, reinterpret_cast<const struct sockaddr*>(&servaddr), sizeof(servaddr)) < 0) {
            close("bind failed", true);
            return false;
        }

        if (enable_multicast) {
            struct ip_mreq group{};
            group.imr_interface = sub_in_addr;
            group.imr_multiaddr = dest_in_addr;

            if (::setsockopt(fd_, IPPROTO_IP, IP_ADD_MEMBERSHIP,
                             reinterpret_cast<detail::sockopt_val_t>(&group), sizeof(group)) < 0) {
                close("setsockopt IP_ADD_MEMBERSHIP failed", true);
                return false;
            }
        }

        return true;
    }

    [[nodiscard]] uint16_t getLocalPort() const noexcept {
        if (fd_ == detail::INVALID_SOCKET_FD) return 0;
        struct sockaddr_in addr{};
        socklen_t addrlen = sizeof(addr);
        if (::getsockname(fd_, reinterpret_cast<struct sockaddr*>(&addr), &addrlen) != 0) {
            return 0;
        }
        return ntohs(addr.sin_port);
    }

    [[nodiscard]] const char* getLastError() const noexcept { return last_error_; }

    [[nodiscard]] bool isClosed() const noexcept { return fd_ == detail::INVALID_SOCKET_FD; }

    void close(const char* reason, bool check_errno = false, int err_override = 0) noexcept {
        if (fd_ != detail::INVALID_SOCKET_FD) {
            saveError(reason, check_errno, err_override);
            detail::close_socket(fd_);
            fd_ = detail::INVALID_SOCKET_FD;
        }
    }

    template <UdpReadHandler Handler>
    [[nodiscard]] bool read(Handler handler) {
        if (fd_ == detail::INVALID_SOCKET_FD) return false;
        int n = 0;
        do {
            n = ::recv(fd_, reinterpret_cast<char*>(buf_), static_cast<int>(RecvBufSize), 0);
        } while (n < 0 && detail::is_eintr(detail::get_last_error()));

        if (n > 0) {
            handler(buf_, static_cast<uint32_t>(n));
            return true;
        }
        return false;
    }

    template <UdpRecvFromHandler Handler>
    [[nodiscard]] bool recvfrom(Handler handler) {
        if (fd_ == detail::INVALID_SOCKET_FD) return false;
        struct sockaddr_in src_addr{};
        socklen_t addrlen = sizeof(src_addr);
        int n = 0;
        do {
            n = ::recvfrom(fd_, reinterpret_cast<char*>(buf_), static_cast<int>(RecvBufSize), 0,
                           reinterpret_cast<struct sockaddr*>(&src_addr), &addrlen);
        } while (n < 0 && detail::is_eintr(detail::get_last_error()));

        if (n > 0) {
            handler(buf_, static_cast<uint32_t>(n), src_addr);
            return true;
        }
        return false;
    }

    [[nodiscard]] bool sendto(const void* data, uint32_t size,
                              const sockaddr_in& dst_addr) noexcept {
        if (fd_ == detail::INVALID_SOCKET_FD) return false;
        if (size > static_cast<uint32_t>(std::numeric_limits<int>::max())) return false;
        int ret = 0;
        do {
            ret = ::sendto(fd_, reinterpret_cast<const char*>(data), static_cast<int>(size), 0,
                           reinterpret_cast<const struct sockaddr*>(&dst_addr), sizeof(dst_addr));
        } while (ret < 0 && detail::is_eintr(detail::get_last_error()));
        return ret == static_cast<int>(size);
    }

    [[nodiscard]] bool sendto(std::span<const std::byte> data,
                              const sockaddr_in& dst_addr) noexcept {
        if (data.size() > static_cast<size_t>(std::numeric_limits<int>::max())) return false;
        return sendto(data.data(), static_cast<uint32_t>(data.size()), dst_addr);
    }

    [[nodiscard]] bool sendto(std::span<const uint8_t> data,
                              const sockaddr_in& dst_addr) noexcept {
        return sendto(std::as_bytes(data), dst_addr);
    }

   private:
    void saveError(const char* msg, bool check_errno = true, int err_override = 0) noexcept {
        detail::format_socket_error(last_error_, sizeof(last_error_), msg, check_errno, err_override);
    }

    detail::socket_t fd_ = detail::INVALID_SOCKET_FD;
    uint8_t buf_[RecvBufSize]{};
    char last_error_[128] = "";
};

class SocketUdpSender {
   public:
    SocketUdpSender() = default;
    SocketUdpSender(const SocketUdpSender&) = delete;
    SocketUdpSender& operator=(const SocketUdpSender&) = delete;
    SocketUdpSender(SocketUdpSender&&) = delete;
    SocketUdpSender& operator=(SocketUdpSender&&) = delete;

    ~SocketUdpSender() { close("destruct"); }

    [[nodiscard]] bool init([[maybe_unused]] const char* interface_ip, const char* local_ip,
                            uint16_t local_port, const char* dest_ip,
                            uint16_t dest_port) noexcept {
        close("reinit");

        if (!detail::ensure_network_init()) {
            saveError("WSAStartup failed", false);
            return false;
        }
        if (!local_ip) {
            saveError("invalid local_ip", false);
            return false;
        }
        if (!dest_ip) {
            saveError("invalid dest_ip", false);
            return false;
        }

        struct sockaddr_in localaddr{};
        localaddr.sin_family = AF_INET;
        localaddr.sin_port = htons(local_port);
        if (::inet_pton(AF_INET, local_ip, &(localaddr.sin_addr)) != 1) {
            saveError("invalid local_ip", false);
            return false;
        }

        struct sockaddr_in destaddr{};
        destaddr.sin_family = AF_INET;
        destaddr.sin_port = htons(dest_port);
        if (::inet_pton(AF_INET, dest_ip, &(destaddr.sin_addr)) != 1) {
            saveError("invalid dest_ip", false);
            return false;
        }

        if ((fd_ = ::socket(AF_INET, SOCK_DGRAM, 0)) == detail::INVALID_SOCKET_FD) {
            saveError("socket error", true);
            return false;
        }

        if (!detail::set_nonblocking(fd_)) {
            close("set nonblock error", true);
            return false;
        }

#ifdef _WIN32
        detail::disable_udp_connreset(fd_);
#endif

        if (::bind(fd_, reinterpret_cast<const struct sockaddr*>(&localaddr), sizeof(localaddr)) < 0) {
            close("bind failed", true);
            return false;
        }

        if (::connect(fd_, reinterpret_cast<struct sockaddr*>(&destaddr), sizeof(destaddr)) < 0) {
            close("connect error", true);
            return false;
        }

        int ttl = 64;
        if (::setsockopt(fd_, IPPROTO_IP, IP_MULTICAST_TTL,
                         reinterpret_cast<detail::sockopt_val_t>(&ttl), sizeof(ttl)) < 0) {
            close("setsockopt IP_MULTICAST_TTL failed", true);
            return false;
        }

        return true;
    }

    [[nodiscard]] uint16_t getLocalPort() const noexcept {
        if (fd_ == detail::INVALID_SOCKET_FD) return 0;
        struct sockaddr_in addr{};
        socklen_t addrlen = sizeof(addr);
        if (::getsockname(fd_, reinterpret_cast<struct sockaddr*>(&addr), &addrlen) != 0) {
            return 0;
        }
        return ntohs(addr.sin_port);
    }

    [[nodiscard]] const char* getLastError() const noexcept { return last_error_; }

    [[nodiscard]] bool isClosed() const noexcept { return fd_ == detail::INVALID_SOCKET_FD; }

    void close(const char* reason, bool check_errno = false, int err_override = 0) noexcept {
        if (fd_ != detail::INVALID_SOCKET_FD) {
            saveError(reason, check_errno, err_override);
            detail::close_socket(fd_);
            fd_ = detail::INVALID_SOCKET_FD;
        }
    }

    [[nodiscard]] bool write(const void* data, uint32_t size) noexcept {
        if (fd_ == detail::INVALID_SOCKET_FD) return false;
        if (size > static_cast<uint32_t>(std::numeric_limits<int>::max())) return false;
        int ret = 0;
        do {
            ret = ::send(fd_, reinterpret_cast<const char*>(data), static_cast<int>(size), 0);
        } while (ret < 0 && detail::is_eintr(detail::get_last_error()));
        return ret == static_cast<int>(size);
    }

    [[nodiscard]] bool write(std::span<const std::byte> data) noexcept {
        if (data.size() > static_cast<size_t>(std::numeric_limits<int>::max())) return false;
        return write(data.data(), static_cast<uint32_t>(data.size()));
    }

    [[nodiscard]] bool write(std::span<const uint8_t> data) noexcept {
        return write(std::as_bytes(data));
    }

   private:
    void saveError(const char* msg, bool check_errno = true, int err_override = 0) noexcept {
        detail::format_socket_error(last_error_, sizeof(last_error_), msg, check_errno, err_override);
    }

    detail::socket_t fd_ = detail::INVALID_SOCKET_FD;
    char last_error_[128] = "";
};

}  // namespace pollnet
