#pragma once

#include "pollnet/core.h"

#ifndef _WIN32
#include <fcntl.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <unistd.h>
#endif

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <optional>
#include <span>
#include <string_view>
#include <utility>

namespace pollnet {

namespace detail {

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

inline void set_tcp_quickack(socket_t fd) noexcept {
#if defined(__linux__) && defined(TCP_QUICKACK)
    int qa = 1;
    ::setsockopt(fd, IPPROTO_TCP, TCP_QUICKACK, &qa, sizeof(qa));
#else
    (void)fd;
#endif
}

inline void apply_socket_tuning(socket_t fd, bool is_tcp, int recv_buf_bytes, int send_buf_bytes,
                                int busy_poll_us, bool tcp_quickack) noexcept {
    if (recv_buf_bytes > 0) {
        ::setsockopt(fd, SOL_SOCKET, SO_RCVBUF, reinterpret_cast<sockopt_val_t>(&recv_buf_bytes),
                     sizeof(recv_buf_bytes));
    }
    if (send_buf_bytes > 0) {
        ::setsockopt(fd, SOL_SOCKET, SO_SNDBUF, reinterpret_cast<sockopt_val_t>(&send_buf_bytes),
                     sizeof(send_buf_bytes));
    }
#if defined(__linux__) && defined(SO_BUSY_POLL)
    if (busy_poll_us > 0) {
        ::setsockopt(fd, SOL_SOCKET, SO_BUSY_POLL, &busy_poll_us, sizeof(busy_poll_us));
    }
#else
    (void)busy_poll_us;
#endif
    if (is_tcp && tcp_quickack) {
        set_tcp_quickack(fd);
    }
}

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
    if (ready == 0) return 0;

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
    if (ready == 0) return 0;

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

// 共享 TCP 监听器（供 SocketTcpServer 与 EventLoopTcpServer 复用）
class TcpListener {
   public:
    TcpListener() = default;
    TcpListener(const TcpListener&) = delete;
    TcpListener& operator=(const TcpListener&) = delete;
    TcpListener(TcpListener&&) = delete;
    TcpListener& operator=(TcpListener&&) = delete;

    ~TcpListener() { close("destruct"); }

    [[nodiscard]] bool listen(const TcpServerOptions& opts) noexcept {
        close("reinit");

        if (!ensure_network_init()) {
            saveError("WSAStartup failed", false);
            return false;
        }

        auto ep = Endpoint::from(opts.host, opts.port);
        if (!ep) {
            saveError("invalid host", false);
            return false;
        }

        fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
        if (fd_ == INVALID_SOCKET_FD) {
            saveError("socket error", true);
            return false;
        }

        if (!set_nonblocking(fd_)) {
            close("set nonblock error", true);
            return false;
        }

        int yes = 1;
        if (::setsockopt(fd_, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<sockopt_val_t>(&yes),
                         sizeof(yes)) < 0) {
            close("setsockopt SO_REUSEADDR error", true);
            return false;
        }

        apply_socket_tuning(fd_, true, opts.recv_buf_bytes, opts.send_buf_bytes, opts.busy_poll_us,
                            opts.tcp_quickack);

        if (::bind(fd_, reinterpret_cast<const struct sockaddr*>(&ep->raw), sizeof(ep->raw)) < 0) {
            close("bind error", true);
            return false;
        }

        int backlog = (opts.backlog > 0) ? opts.backlog : 128;
        if (::listen(fd_, backlog) < 0) {
            close("listen error", true);
            return false;
        }
        return true;
    }

    void close(const char* reason, bool check_errno = false, int err_override = 0) noexcept {
        if (fd_ != INVALID_SOCKET_FD) {
            saveError(reason, check_errno, err_override);
            close_socket(fd_);
            fd_ = INVALID_SOCKET_FD;
        }
    }

    [[nodiscard]] socket_t acceptOne(Endpoint& peer_ep) noexcept {
        if (fd_ == INVALID_SOCKET_FD) return INVALID_SOCKET_FD;
        socklen_t addr_len = sizeof(peer_ep.raw);
        for (;;) {
            socket_t client_fd =
                ::accept(fd_, reinterpret_cast<struct sockaddr*>(&peer_ep.raw), &addr_len);
            if (client_fd != INVALID_SOCKET_FD) return client_fd;
            if (!is_eintr(get_last_error())) return INVALID_SOCKET_FD;
            addr_len = sizeof(peer_ep.raw);
        }
    }

    [[nodiscard]] socket_t fd() const noexcept { return fd_; }
    [[nodiscard]] bool isClosed() const noexcept { return fd_ == INVALID_SOCKET_FD; }
    [[nodiscard]] std::string_view lastError() const noexcept { return last_error_; }

    [[nodiscard]] uint16_t localPort() const noexcept {
        if (fd_ == INVALID_SOCKET_FD) return 0;
        struct sockaddr_in addr{};
        socklen_t addrlen = sizeof(addr);
        if (::getsockname(fd_, reinterpret_cast<struct sockaddr*>(&addr), &addrlen) != 0) {
            return 0;
        }
        return ntohs(addr.sin_port);
    }

   private:
    void saveError(const char* msg, bool check_errno = true, int err_override = 0) noexcept {
        format_socket_error(last_error_, sizeof(last_error_), msg, check_errno, err_override);
    }

    socket_t fd_ = INVALID_SOCKET_FD;
    char last_error_[128] = "";
};

}  // namespace detail

// ==========================================
// TCP 后端实现 (Socket Backend - TCP)
// ==========================================

template <typename UserData, size_t RecvBufSize>
class SocketTcpClient;

template <typename UserData, size_t RecvBufSize, size_t MaxConns>
class SocketTcpServer;

template <typename UserData, size_t RecvBufSize, size_t MaxConns>
class EventLoopTcpServer;

template <typename UserData = EmptyUserData, size_t RecvBufSize = 4096>
class SocketTcpConnection : public UserData {
    static_assert(RecvBufSize > 0 &&
                      RecvBufSize <= static_cast<size_t>(std::numeric_limits<int>::max()),
                  "RecvBufSize must be in (0, INT_MAX]");

   public:
    SocketTcpConnection() = default;
    SocketTcpConnection(const SocketTcpConnection&) = delete;
    SocketTcpConnection& operator=(const SocketTcpConnection&) = delete;
    SocketTcpConnection(SocketTcpConnection&&) = delete;
    SocketTcpConnection& operator=(SocketTcpConnection&&) = delete;

    ~SocketTcpConnection() { close("destruct"); }

    [[nodiscard]] std::string_view lastError() const noexcept { return last_error_; }
    [[nodiscard]] bool isConnected() const noexcept { return fd_ != detail::INVALID_SOCKET_FD; }

    [[nodiscard]] std::optional<Endpoint> peer() const noexcept {
        if (fd_ == detail::INVALID_SOCKET_FD) return std::nullopt;
        Endpoint ep;
        socklen_t addr_len = sizeof(ep.raw);
        if (::getpeername(fd_, reinterpret_cast<struct sockaddr*>(&ep.raw), &addr_len) != 0) {
            return std::nullopt;
        }
        return ep;
    }

    void close(const char* reason, bool check_errno = false, int err_override = 0) noexcept {
        if (fd_ != detail::INVALID_SOCKET_FD) {
            saveError(reason, check_errno, err_override);
            detail::close_socket(fd_);
            fd_ = detail::INVALID_SOCKET_FD;
        }
    }

    [[nodiscard]] int writeSome(const void* data, size_t size, bool more = false) noexcept {
        if (fd_ == detail::INVALID_SOCKET_FD) return -1;
        if (size == 0) return 0;
        if (size > static_cast<size_t>(std::numeric_limits<int>::max())) {
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
            if (detail::is_would_block(err)) return 0;
            close("send error", true, err);
            return -1;
        }
        if (ret > 0 && send_timeout_ns_ > 0) {
            send_ts_ = now_ns();
        }
        return ret;
    }

    [[nodiscard]] int writeSome(IoSlice slice, bool more = false) noexcept {
        return writeSome(slice.data, slice.size, more);
    }

    [[nodiscard]] int writeSome(std::span<const IoSlice> slices, bool more = false) noexcept {
        if (fd_ == detail::INVALID_SOCKET_FD) return -1;
        if (slices.empty()) return 0;

        constexpr size_t kMaxIov = 64;
#ifdef _WIN32
        (void)more;
        WSABUF bufs[kMaxIov];
        DWORD buf_cnt = 0;
        size_t total_len = 0;
        for (const auto& s : slices) {
            if (s.size == 0) continue;
            if (!s.data) {
                close("invalid io slice", false);
                return -1;
            }
            if (buf_cnt == kMaxIov) break;
            if (s.size > static_cast<size_t>(std::numeric_limits<int>::max()) - total_len) {
                close("send size overflow", false);
                return -1;
            }
            bufs[buf_cnt].buf = const_cast<CHAR*>(reinterpret_cast<const CHAR*>(s.data));
            bufs[buf_cnt].len = static_cast<ULONG>(s.size);
            total_len += s.size;
            ++buf_cnt;
        }
        if (buf_cnt == 0) return 0;

        DWORD sent_bytes = 0;
        int rc = ::WSASend(fd_, bufs, buf_cnt, &sent_bytes, 0, nullptr, nullptr);
        if (rc == SOCKET_ERROR) {
            int err = detail::get_last_error();
            if (detail::is_would_block(err)) return 0;
            close("WSASend error", true, err);
            return -1;
        }
        if (sent_bytes > 0 && send_timeout_ns_ > 0) {
            send_ts_ = now_ns();
        }
        return static_cast<int>(sent_bytes);
#else
        struct iovec iovs[kMaxIov];
        size_t iov_cnt = 0;
        size_t total_len = 0;
        for (const auto& s : slices) {
            if (s.size == 0) continue;
            if (!s.data) {
                close("invalid io slice", false);
                return -1;
            }
            if (iov_cnt == kMaxIov) break;
            if (s.size > static_cast<size_t>(std::numeric_limits<int>::max()) - total_len) {
                close("send size overflow", false);
                return -1;
            }
            iovs[iov_cnt].iov_base = const_cast<void*>(s.data);
            iovs[iov_cnt].iov_len = s.size;
            total_len += s.size;
            ++iov_cnt;
        }
        if (iov_cnt == 0) return 0;

        struct msghdr msg{};
        msg.msg_iov = iovs;
        msg.msg_iovlen = static_cast<decltype(msg.msg_iovlen)>(iov_cnt);
        int flags = MSG_NOSIGNAL;
        if (more) flags |= MSG_MORE;

        ssize_t ret = 0;
        do {
            ret = ::sendmsg(fd_, &msg, flags);
        } while (ret < 0 && detail::is_eintr(detail::get_last_error()));

        if (ret < 0) {
            int err = detail::get_last_error();
            if (detail::is_would_block(err)) return 0;
            close("sendmsg error", true, err);
            return -1;
        }
        if (ret > 0 && send_timeout_ns_ > 0) {
            send_ts_ = now_ns();
        }
        return static_cast<int>(ret);
#endif
    }

    [[nodiscard]] bool writeNonblock(const void* data, size_t size, bool more = false) noexcept {
        if (size == 0) return isConnected();
        if (size > static_cast<size_t>(std::numeric_limits<int>::max())) {
            close("send size overflow", false);
            return false;
        }
        int sent = writeSome(data, size, more);
        if (sent < 0) return false;
        if (static_cast<size_t>(sent) != size) {
            close("send buffer full", false);
            return false;
        }
        return true;
    }

    [[nodiscard]] bool writeNonblock(IoSlice slice, bool more = false) noexcept {
        return writeNonblock(slice.data, slice.size, more);
    }

    [[nodiscard]] bool writeNonblock(std::span<const IoSlice> slices, bool more = false) noexcept {
        constexpr size_t kMaxIov = 64;
        size_t total_bytes = 0;
        for (const auto& s : slices) {
            if (s.size == 0) continue;
            if (!s.data) {
                close("invalid io slice", false);
                return false;
            }
            if (s.size > static_cast<size_t>(std::numeric_limits<int>::max()) - total_bytes) {
                close("send size overflow", false);
                return false;
            }
            total_bytes += s.size;
        }
        if (total_bytes == 0) return isConnected();

        if (slices.size() <= kMaxIov) {
            int sent = writeSome(slices, more);
            if (sent < 0) return false;
            if (static_cast<size_t>(sent) != total_bytes) {
                close("send buffer full", false);
                return false;
            }
            return true;
        }

        size_t offset = 0;
        while (offset < slices.size()) {
            size_t count = std::min(kMaxIov, slices.size() - offset);
            size_t chunk_bytes = 0;
            for (size_t i = 0; i < count; ++i) chunk_bytes += slices[offset + i].size;
            bool chunk_more = more || (offset + count < slices.size());
            int sent = writeSome(slices.subspan(offset, count), chunk_more);
            if (sent < 0) return false;
            if (static_cast<size_t>(sent) != chunk_bytes) {
                close("send buffer full", false);
                return false;
            }
            offset += count;
        }
        return true;
    }

    [[nodiscard]] bool write(const void* data_, size_t size, bool more = false) noexcept {
        if (size == 0) return isConnected();
        const uint8_t* data = static_cast<const uint8_t*>(data_);
        do {
            int sent = writeSome(data, size, more);
            if (sent < 0) return false;
            data += sent;
            size -= static_cast<size_t>(sent);
        } while (size != 0);
        return true;
    }

    [[nodiscard]] bool write(IoSlice slice, bool more = false) noexcept {
        return write(slice.data, slice.size, more);
    }

    [[nodiscard]] bool write(std::span<const IoSlice> slices, bool more = false) noexcept {
        constexpr size_t kMaxIov = 64;
        size_t total_bytes = 0;
        for (const auto& s : slices) {
            if (s.size == 0) continue;
            if (!s.data) {
                close("invalid io slice", false);
                return false;
            }
            total_bytes += s.size;
        }
        if (total_bytes == 0) return isConnected();

        size_t idx = 0;
        size_t slice_off = 0;
        while (idx < slices.size()) {
            std::array<IoSlice, kMaxIov> cur{};
            size_t cnt = 0;
            for (size_t i = idx; i < slices.size() && cnt < kMaxIov; ++i) {
                size_t off = (i == idx) ? slice_off : 0;
                if (slices[i].size <= off) continue;
                cur[cnt++] = IoSlice{static_cast<const uint8_t*>(slices[i].data) + off,
                                     slices[i].size - off};
            }
            if (cnt == 0) break;

            int sent = writeSome(std::span<const IoSlice>{cur.data(), cnt}, more);
            if (sent < 0) return false;
            size_t adv = static_cast<size_t>(sent);
            while (adv > 0 && idx < slices.size()) {
                size_t avail = slices[idx].size - slice_off;
                if (adv < avail) {
                    slice_off += adv;
                    adv = 0;
                } else {
                    adv -= avail;
                    ++idx;
                    slice_off = 0;
                }
            }
            while (idx < slices.size() && slices[idx].size == 0) {
                ++idx;
                slice_off = 0;
            }
        }
        return true;
    }

   protected:
    template <typename, size_t>
    friend class SocketTcpClient;

    template <typename, size_t, size_t>
    friend class SocketTcpServer;

    template <typename, size_t, size_t>
    friend class EventLoopTcpServer;

    template <typename Handler>
    void pollConn(int64_t now, Handler& handler) {
        if (send_timeout_ns_ > 0 && now - send_ts_ >= send_timeout_ns_) {
            detail::dispatch_send_timeout(handler, *this);
            send_ts_ = now;
        }
        bool got_data = read([&](std::span<const uint8_t> buf) {
            return static_cast<size_t>(handler.onData(*this, buf));
        });
        if (recv_timeout_ns_ > 0 && isConnected()) {
            if (!got_data && now - recv_ts_ >= recv_timeout_ns_) {
                detail::dispatch_recv_timeout(handler, *this);
                got_data = true;
            }
            if (got_data) {
                recv_ts_ = now;
            }
        }
    }

    template <typename Handler>
    bool read(Handler handler) {
        if (fd_ == detail::INVALID_SOCKET_FD) return false;
        if (tail_ >= RecvBufSize) {
            close("recv buf full", false);
            return false;
        }

        int ret = 0;
        do {
            ret = ::recv(fd_, reinterpret_cast<char*>(recvbuf_ + tail_),
                         static_cast<int>(RecvBufSize - tail_), 0);
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
        tail_ += static_cast<size_t>(ret);
        if (tcp_quickack_) {
            detail::set_tcp_quickack(fd_);
        }

        size_t avail = tail_ - head_;
        size_t consumed = handler(std::span<const uint8_t>{recvbuf_ + head_, avail});
        if (!isConnected()) {
            head_ = tail_ = 0;
            return false;
        }
        if (consumed > avail) {
            close("invalid consumed size", false);
            return false;
        }

        size_t remaining = avail - consumed;
        if (remaining == 0) {
            head_ = tail_ = 0;
        } else if (remaining < RecvBufSize) {
            head_ += consumed;
            if ((head_ >= RecvBufSize / 2 || tail_ == RecvBufSize) && head_ > 0 &&
                head_ + remaining <= RecvBufSize) {
                std::memmove(recvbuf_, recvbuf_ + head_, remaining);
                head_ = 0;
                tail_ = remaining;
            } else if (tail_ == RecvBufSize) {
                close("recv buf full", false);
            }
        } else {
            close("recv buf full", false);
        }
        return true;
    }

    bool open(int64_t now, detail::socket_t fd, int64_t send_timeout_ns, int64_t recv_timeout_ns,
              bool tcp_quickack = false, int recv_buf_bytes = 0, int send_buf_bytes = 0,
              int busy_poll_us = 0) noexcept {
        fd_ = fd;
        head_ = tail_ = 0;
        send_ts_ = now;
        recv_ts_ = now;
        send_timeout_ns_ = send_timeout_ns;
        recv_timeout_ns_ = recv_timeout_ns;
        tcp_quickack_ = tcp_quickack;

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

        detail::apply_socket_tuning(fd_, true, recv_buf_bytes, send_buf_bytes, busy_poll_us,
                                    tcp_quickack_);
        return true;
    }

    void saveError(const char* msg, bool check_errno, int err_override = 0) noexcept {
        detail::format_socket_error(last_error_, sizeof(last_error_), msg, check_errno, err_override);
    }

    detail::socket_t fd_ = detail::INVALID_SOCKET_FD;
    size_t head_ = 0;
    size_t tail_ = 0;
    int64_t send_ts_ = 0;
    int64_t recv_ts_ = 0;
    int64_t send_timeout_ns_ = 0;
    int64_t recv_timeout_ns_ = 0;
    bool tcp_quickack_ = false;
    uint8_t recvbuf_[RecvBufSize];
    char last_error_[128] = "";
};

template <typename UserData = EmptyUserData, size_t RecvBufSize = 4096>
class SocketTcpClient {
   public:
    using Conn = SocketTcpConnection<UserData, RecvBufSize>;

    SocketTcpClient() = default;
    SocketTcpClient(const SocketTcpClient&) = delete;
    SocketTcpClient& operator=(const SocketTcpClient&) = delete;
    SocketTcpClient(SocketTcpClient&&) = delete;
    SocketTcpClient& operator=(SocketTcpClient&&) = delete;

    ~SocketTcpClient() { closePendingConnect(); }

    [[nodiscard]] bool connect(const TcpClientOptions& opts) noexcept {
        conn_.close("reinit");
        closePendingConnect();
        initialized_ = false;
        report_disconnect_ = false;
        can_connect_immediately_ = true;
        last_retry_ts_ = 0;
        conn_start_ts_ = 0;
        server_addr_ = {};
        local_port_be_ = 0;
        retry_interval_ns_ = 0;
        connect_timeout_ns_ = 0;
        send_timeout_ns_ = 0;
        recv_timeout_ns_ = 0;
        tcp_quickack_ = false;
        busy_poll_us_ = 0;
        recv_buf_bytes_ = 0;
        send_buf_bytes_ = 0;

        if (!detail::ensure_network_init()) {
            conn_.saveError("WSAStartup failed", false);
            return false;
        }

        auto ep = Endpoint::from(opts.host, opts.port);
        if (!ep) {
            conn_.saveError("invalid host", false);
            return false;
        }

        server_addr_ = ep->raw;
        local_port_be_ = htons(opts.local_port);
        retry_interval_ns_ = detail::to_timeout_ns(opts.retry_interval);
        connect_timeout_ns_ = detail::to_timeout_ns(opts.connect_timeout);
        send_timeout_ns_ = detail::to_timeout_ns(opts.send_timeout);
        recv_timeout_ns_ = detail::to_timeout_ns(opts.recv_timeout);
        tcp_quickack_ = opts.tcp_quickack;
        busy_poll_us_ = opts.busy_poll_us;
        recv_buf_bytes_ = opts.recv_buf_bytes;
        send_buf_bytes_ = opts.send_buf_bytes;
        initialized_ = true;
        return true;
    }

    void allowReconnect() noexcept { can_connect_immediately_ = true; }

    void close(const char* reason) noexcept {
        closePendingConnect();
        conn_.close(reason);
    }

    [[nodiscard]] Conn& conn() noexcept { return conn_; }
    [[nodiscard]] const Conn& conn() const noexcept { return conn_; }

    [[nodiscard]] bool isConnected() const noexcept { return conn_.isConnected(); }
    [[nodiscard]] std::string_view lastError() const noexcept { return conn_.lastError(); }
    [[nodiscard]] std::optional<Endpoint> peer() const noexcept { return conn_.peer(); }

    [[nodiscard]] int writeSome(const void* data, size_t size, bool more = false) noexcept {
        return conn_.writeSome(data, size, more);
    }
    [[nodiscard]] int writeSome(IoSlice slice, bool more = false) noexcept {
        return conn_.writeSome(slice, more);
    }
    [[nodiscard]] int writeSome(std::span<const IoSlice> slices, bool more = false) noexcept {
        return conn_.writeSome(slices, more);
    }

    [[nodiscard]] bool writeNonblock(const void* data, size_t size, bool more = false) noexcept {
        return conn_.writeNonblock(data, size, more);
    }
    [[nodiscard]] bool writeNonblock(IoSlice slice, bool more = false) noexcept {
        return conn_.writeNonblock(slice, more);
    }
    [[nodiscard]] bool writeNonblock(std::span<const IoSlice> slices, bool more = false) noexcept {
        return conn_.writeNonblock(slices, more);
    }

    [[nodiscard]] bool write(const void* data, size_t size, bool more = false) noexcept {
        return conn_.write(data, size, more);
    }
    [[nodiscard]] bool write(IoSlice slice, bool more = false) noexcept {
        return conn_.write(slice, more);
    }
    [[nodiscard]] bool write(std::span<const IoSlice> slices, bool more = false) noexcept {
        return conn_.write(slices, more);
    }

    template <typename Handler>
        requires TcpHandler<Handler, Conn>
    void poll(Handler& handler) {
        int64_t now =
            (!conn_.isConnected() || send_timeout_ns_ > 0 || recv_timeout_ns_ > 0) ? now_ns() : 0;
        poll(handler, now);
    }

    template <typename Handler>
        requires TcpHandler<Handler, Conn>
    void poll(Handler& handler, int64_t now) {
        if (!initialized_) return;
        if (!conn_.isConnected()) {
            if (report_disconnect_) {
                detail::dispatch_disconnect(handler, conn_);
                report_disconnect_ = false;
            }
            int ret = stepConnect(now);
            if (ret <= 0) {
                if (ret < 0) detail::dispatch_connect_failed(handler);
                return;
            }
            report_disconnect_ = true;
            detail::dispatch_connect(handler, conn_);
        }
        conn_.pollConn(now, handler);
    }

   private:
    void closePendingConnect() noexcept {
        if (conn_fd_ != detail::INVALID_SOCKET_FD) {
            detail::close_socket(conn_fd_);
            conn_fd_ = detail::INVALID_SOCKET_FD;
        }
    }

    int stepConnect(int64_t now) noexcept {
        if (conn_fd_ == detail::INVALID_SOCKET_FD) {
            if (!can_connect_immediately_) {
                if (retry_interval_ns_ <= 0 || now - last_retry_ts_ < retry_interval_ns_) {
                    return 0;
                }
            }
            can_connect_immediately_ = false;
            last_retry_ts_ = now;

            detail::socket_t fd = ::socket(AF_INET, SOCK_STREAM, 0);
            if (fd == detail::INVALID_SOCKET_FD) {
                conn_.saveError("socket error", true);
                return -1;
            }
            if (local_port_be_ != 0) {
                struct sockaddr_in local_addr{};
                local_addr.sin_family = AF_INET;
                local_addr.sin_addr.s_addr = htonl(INADDR_ANY);
                local_addr.sin_port = local_port_be_;
                if (::bind(fd, reinterpret_cast<struct sockaddr*>(&local_addr), sizeof(local_addr)) <
                    0) {
                    conn_.saveError("bind error", true);
                    detail::close_socket(fd);
                    return -1;
                }
            }

            if (!detail::set_nonblocking(fd)) {
                conn_.saveError("set nonblock error", true);
                detail::close_socket(fd);
                return -1;
            }
            detail::apply_socket_tuning(fd, true, recv_buf_bytes_, send_buf_bytes_, busy_poll_us_,
                                        tcp_quickack_);

            int ret =
                ::connect(fd, reinterpret_cast<struct sockaddr*>(&server_addr_), sizeof(server_addr_));
            if (ret == 0) {
                return conn_.open(now, fd, send_timeout_ns_, recv_timeout_ns_, tcp_quickack_, 0, 0,
                                  busy_poll_us_)
                           ? 1
                           : -1;
            }

            int err = detail::get_last_error();
            if (detail::is_connect_in_progress(err)) {
                conn_fd_ = fd;
                conn_start_ts_ = now;
                return 0;
            }

            conn_.saveError("connect error", true, err);
            detail::close_socket(fd);
            return -1;
        }

        int conn_err = 0;
        int status = detail::check_connect_status(conn_fd_, conn_err);
        if (status > 0) {
            detail::socket_t fd = conn_fd_;
            conn_fd_ = detail::INVALID_SOCKET_FD;
            return conn_.open(now, fd, send_timeout_ns_, recv_timeout_ns_, tcp_quickack_, 0, 0,
                              busy_poll_us_)
                       ? 1
                       : -1;
        }
        if (status < 0) {
            conn_.saveError("connect error", true, conn_err);
            closePendingConnect();
            return -1;
        }
        if (connect_timeout_ns_ > 0 && now - conn_start_ts_ >= connect_timeout_ns_) {
            conn_.saveError("connect expired", false);
            closePendingConnect();
            return -1;
        }
        return 0;
    }

    Conn conn_;
    bool initialized_ = false;
    bool report_disconnect_ = false;
    bool can_connect_immediately_ = true;
    detail::socket_t conn_fd_ = detail::INVALID_SOCKET_FD;
    int64_t last_retry_ts_ = 0;
    int64_t conn_start_ts_ = 0;
    int64_t retry_interval_ns_ = 0;
    int64_t connect_timeout_ns_ = 0;
    int64_t send_timeout_ns_ = 0;
    int64_t recv_timeout_ns_ = 0;
    struct sockaddr_in server_addr_{};
    uint16_t local_port_be_ = 0;
    bool tcp_quickack_ = false;
    int busy_poll_us_ = 0;
    int recv_buf_bytes_ = 0;
    int send_buf_bytes_ = 0;
};

template <typename UserData = EmptyUserData, size_t RecvBufSize = 4096, size_t MaxConns = 64>
class SocketTcpServer {
    static_assert(MaxConns > 0, "MaxConns must be > 0");

   public:
    using Conn = SocketTcpConnection<UserData, RecvBufSize>;

    SocketTcpServer() noexcept {
        for (size_t i = 0; i < MaxConns; i++) conns_[i] = conns_data_ + i;
    }
    SocketTcpServer(const SocketTcpServer&) = delete;
    SocketTcpServer& operator=(const SocketTcpServer&) = delete;
    SocketTcpServer(SocketTcpServer&&) = delete;
    SocketTcpServer& operator=(SocketTcpServer&&) = delete;

    ~SocketTcpServer() { close("destruct"); }

    [[nodiscard]] bool listen(const TcpServerOptions& opts) noexcept {
        for (size_t i = 0; i < conns_cnt_; i++) {
            conns_[i]->close("reinit");
        }
        conns_cnt_ = 0;
        if (!listener_.listen(opts)) {
            return false;
        }
        send_timeout_ns_ = detail::to_timeout_ns(opts.send_timeout);
        recv_timeout_ns_ = detail::to_timeout_ns(opts.recv_timeout);
        accept_poll_interval_ = opts.accept_poll_interval;
        accept_poll_counter_ = 0;
        tcp_quickack_ = opts.tcp_quickack;
        busy_poll_us_ = opts.busy_poll_us;
        recv_buf_bytes_ = opts.recv_buf_bytes;
        send_buf_bytes_ = opts.send_buf_bytes;
        return true;
    }

    void close(const char* reason, bool check_errno = false, int err_override = 0) noexcept {
        listener_.close(reason, check_errno, err_override);
    }

    [[nodiscard]] std::string_view lastError() const noexcept { return listener_.lastError(); }
    [[nodiscard]] bool isClosed() const noexcept { return listener_.isClosed(); }
    [[nodiscard]] size_t connCount() const noexcept { return conns_cnt_; }
    [[nodiscard]] uint16_t localPort() const noexcept { return listener_.localPort(); }

    template <typename Handler>
        requires std::invocable<Handler&, Conn&>
    void foreachConn(Handler handler) {
        for (size_t i = 0; i < conns_cnt_; i++) {
            handler(*conns_[i]);
        }
    }

    template <typename Handler>
        requires TcpHandler<Handler, Conn>
    void poll(Handler& handler) {
        int64_t now = (send_timeout_ns_ > 0 || recv_timeout_ns_ > 0) ? now_ns() : 0;
        poll(handler, now);
    }

    template <typename Handler>
        requires TcpHandler<Handler, Conn>
    void poll(Handler& handler, int64_t now) {
        bool check_accept = false;
        if (!listener_.isClosed() && conns_cnt_ < MaxConns) {
            if (conns_cnt_ == 0 || accept_poll_interval_ <= 1) {
                check_accept = true;
            } else if (++accept_poll_counter_ >= accept_poll_interval_) {
                accept_poll_counter_ = 0;
                check_accept = true;
            }
        }
        while (check_accept && !listener_.isClosed() && conns_cnt_ < MaxConns) {
            Endpoint peer_ep;
            detail::socket_t fd = listener_.acceptOne(peer_ep);
            if (fd == detail::INVALID_SOCKET_FD) break;

            if (!detail::dispatch_allow_new_connection(handler, peer_ep)) {
                detail::close_socket(fd);
                continue;
            }
            Conn& c = *conns_[conns_cnt_];
            if (c.open(now, fd, send_timeout_ns_, recv_timeout_ns_, tcp_quickack_, recv_buf_bytes_,
                       send_buf_bytes_, busy_poll_us_)) {
                conns_cnt_++;
                detail::dispatch_connect(handler, c);
            }
        }
        for (size_t i = 0; i < conns_cnt_;) {
            Conn& c = *conns_[i];
            c.pollConn(now, handler);
            if (c.isConnected()) {
                i++;
            } else {
                std::swap(conns_[i], conns_[--conns_cnt_]);
                accept_poll_counter_ = accept_poll_interval_;
                detail::dispatch_disconnect(handler, c);
            }
        }
    }

   private:
    detail::TcpListener listener_;
    size_t conns_cnt_ = 0;
    uint32_t accept_poll_interval_ = 64;
    uint32_t accept_poll_counter_ = 0;
    int64_t send_timeout_ns_ = 0;
    int64_t recv_timeout_ns_ = 0;
    bool tcp_quickack_ = false;
    int busy_poll_us_ = 0;
    int recv_buf_bytes_ = 0;
    int send_buf_bytes_ = 0;
    Conn* conns_[MaxConns]{};
    Conn conns_data_[MaxConns];
};

// ==========================================
// 统一 UDP 后端实现 (Socket Backend - UDP)
// ==========================================

template <size_t RecvBufSize = 1500>
class SocketUdpSocket {
    static_assert(RecvBufSize > 0 &&
                      RecvBufSize <= static_cast<size_t>(std::numeric_limits<int>::max()),
                  "RecvBufSize must be in (0, INT_MAX]");

   public:
    SocketUdpSocket() = default;
    SocketUdpSocket(const SocketUdpSocket&) = delete;
    SocketUdpSocket& operator=(const SocketUdpSocket&) = delete;
    SocketUdpSocket(SocketUdpSocket&&) = delete;
    SocketUdpSocket& operator=(SocketUdpSocket&&) = delete;

    ~SocketUdpSocket() { close("destruct"); }

    [[nodiscard]] bool open(const UdpOptions& opts) noexcept {
        close("reinit");

        if (!detail::ensure_network_init()) {
            saveError("WSAStartup failed", false);
            return false;
        }

        auto bind_ep = Endpoint::from(opts.bind_ip, opts.bind_port);
        if (!bind_ep) {
            saveError("invalid bind_ip", false);
            return false;
        }

        const bool has_dest = !opts.dest_ip.empty();
        std::optional<Endpoint> dest_ep;
        if (has_dest) {
            dest_ep = Endpoint::from(opts.dest_ip, opts.dest_port);
            if (!dest_ep) {
                saveError("invalid dest_ip", false);
                return false;
            }
        }

        const bool has_mcast = !opts.multicast_group_ip.empty();
        std::optional<Endpoint> mcast_group_ep;
        std::optional<Endpoint> mcast_iface_ep;
        if (has_mcast) {
            mcast_group_ep = Endpoint::from(opts.multicast_group_ip, 0);
            if (!mcast_group_ep) {
                saveError("invalid multicast_group_ip", false);
                return false;
            }
            if (!opts.multicast_interface_ip.empty()) {
                mcast_iface_ep = Endpoint::from(opts.multicast_interface_ip, 0);
                if (!mcast_iface_ep) {
                    saveError("invalid multicast_interface_ip", false);
                    return false;
                }
            } else {
                mcast_iface_ep = Endpoint::any(0);
            }
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
        detail::apply_socket_tuning(fd_, false, opts.recv_buf_bytes, opts.send_buf_bytes,
                                    opts.busy_poll_us, false);

        if (!has_dest || has_mcast) {
            int optval = 1;
            if (::setsockopt(fd_, SOL_SOCKET, SO_REUSEADDR,
                             reinterpret_cast<detail::sockopt_val_t>(&optval), sizeof(int)) < 0) {
                close("setsockopt SO_REUSEADDR error", true);
                return false;
            }
        }

        struct sockaddr_in servaddr = bind_ep->raw;
#ifdef _WIN32
        // Windows 下不允许将 UDP socket 直接 bind 到组播地址，需绑定 INADDR_ANY
        if (has_mcast) {
            servaddr.sin_addr.s_addr = htonl(INADDR_ANY);
        }
#endif
        if (::bind(fd_, reinterpret_cast<const struct sockaddr*>(&servaddr), sizeof(servaddr)) < 0) {
            close("bind failed", true);
            return false;
        }

        if (has_mcast) {
            struct ip_mreq group{};
            group.imr_interface = mcast_iface_ep->raw.sin_addr;
            group.imr_multiaddr = mcast_group_ep->raw.sin_addr;
            if (::setsockopt(fd_, IPPROTO_IP, IP_ADD_MEMBERSHIP,
                             reinterpret_cast<detail::sockopt_val_t>(&group), sizeof(group)) < 0) {
                close("setsockopt IP_ADD_MEMBERSHIP failed", true);
                return false;
            }
        }

        if (has_dest) {
            if (::connect(fd_, reinterpret_cast<const struct sockaddr*>(&dest_ep->raw),
                          sizeof(dest_ep->raw)) < 0) {
                close("connect error", true);
                return false;
            }
            if (opts.multicast_ttl > 0) {
                int ttl = opts.multicast_ttl;
                if (::setsockopt(fd_, IPPROTO_IP, IP_MULTICAST_TTL,
                                 reinterpret_cast<detail::sockopt_val_t>(&ttl), sizeof(ttl)) < 0) {
                    close("setsockopt IP_MULTICAST_TTL failed", true);
                    return false;
                }
            }
        }

        return true;
    }

    void close(const char* reason, bool check_errno = false, int err_override = 0) noexcept {
        if (fd_ != detail::INVALID_SOCKET_FD) {
            saveError(reason, check_errno, err_override);
            detail::close_socket(fd_);
            fd_ = detail::INVALID_SOCKET_FD;
        }
    }

    [[nodiscard]] bool isOpen() const noexcept { return fd_ != detail::INVALID_SOCKET_FD; }
    [[nodiscard]] bool isClosed() const noexcept { return fd_ == detail::INVALID_SOCKET_FD; }
    [[nodiscard]] std::string_view lastError() const noexcept { return last_error_; }

    [[nodiscard]] uint16_t localPort() const noexcept {
        if (fd_ == detail::INVALID_SOCKET_FD) return 0;
        struct sockaddr_in addr{};
        socklen_t addrlen = sizeof(addr);
        if (::getsockname(fd_, reinterpret_cast<struct sockaddr*>(&addr), &addrlen) != 0) {
            return 0;
        }
        return ntohs(addr.sin_port);
    }

    [[nodiscard]] bool send(const void* data, size_t size) noexcept {
        if (fd_ == detail::INVALID_SOCKET_FD) return false;
        if (size > static_cast<size_t>(std::numeric_limits<int>::max())) return false;
        int ret = 0;
        do {
            ret = ::send(fd_, reinterpret_cast<const char*>(data), static_cast<int>(size), 0);
        } while (ret < 0 && detail::is_eintr(detail::get_last_error()));
        return ret == static_cast<int>(size);
    }

    [[nodiscard]] bool send(IoSlice slice) noexcept { return send(slice.data, slice.size); }

    [[nodiscard]] size_t sendBatch(std::span<const IoSlice> packets) noexcept {
        return sendBatchImpl(packets, nullptr);
    }

    [[nodiscard]] bool sendTo(const void* data, size_t size, const Endpoint& dst) noexcept {
        if (fd_ == detail::INVALID_SOCKET_FD) return false;
        if (size > static_cast<size_t>(std::numeric_limits<int>::max())) return false;
        int ret = 0;
        do {
            ret = ::sendto(fd_, reinterpret_cast<const char*>(data), static_cast<int>(size), 0,
                           reinterpret_cast<const struct sockaddr*>(&dst.raw), sizeof(dst.raw));
        } while (ret < 0 && detail::is_eintr(detail::get_last_error()));
        return ret == static_cast<int>(size);
    }

    [[nodiscard]] bool sendTo(IoSlice slice, const Endpoint& dst) noexcept {
        return sendTo(slice.data, slice.size, dst);
    }

    [[nodiscard]] size_t sendToBatch(std::span<const IoSlice> packets,
                                     const Endpoint& dst) noexcept {
        return sendBatchImpl(packets, &dst);
    }

    template <UdpDataHandler Handler>
    [[nodiscard]] bool recv(Handler handler) {
        if (fd_ == detail::INVALID_SOCKET_FD) return false;
        int n = 0;
        do {
            n = ::recv(fd_, reinterpret_cast<char*>(buf_), static_cast<int>(RecvBufSize), 0);
        } while (n < 0 && detail::is_eintr(detail::get_last_error()));

        if (n > 0) {
            handler(std::span<const uint8_t>{buf_, static_cast<size_t>(n)});
            return true;
        }
        return false;
    }

    template <UdpPacketHandler Handler>
    [[nodiscard]] bool recvFrom(Handler handler) {
        if (fd_ == detail::INVALID_SOCKET_FD) return false;
        Endpoint peer_ep;
        socklen_t addrlen = sizeof(peer_ep.raw);
        int n = 0;
        do {
            n = ::recvfrom(fd_, reinterpret_cast<char*>(buf_), static_cast<int>(RecvBufSize), 0,
                           reinterpret_cast<struct sockaddr*>(&peer_ep.raw), &addrlen);
        } while (n < 0 && detail::is_eintr(detail::get_last_error()));

        if (n > 0) {
            handler(std::span<const uint8_t>{buf_, static_cast<size_t>(n)}, peer_ep);
            return true;
        }
        return false;
    }

    template <size_t MaxBatch = 16, UdpDataHandler Handler>
    [[nodiscard]] size_t recvBatch(Handler handler) {
        return recvBatchImpl<MaxBatch, false>(handler);
    }

    template <size_t MaxBatch = 16, UdpPacketHandler Handler>
    [[nodiscard]] size_t recvFromBatch(Handler handler) {
        return recvBatchImpl<MaxBatch, true>(handler);
    }

   private:
    [[nodiscard]] size_t sendBatchImpl(std::span<const IoSlice> packets,
                                       const Endpoint* dst) noexcept {
        if (fd_ == detail::INVALID_SOCKET_FD || packets.empty()) return 0;
#ifdef __linux__
        constexpr size_t kBatchSize = 64;
        size_t total_sent = 0;
        size_t offset = 0;
        while (offset < packets.size()) {
            size_t count = std::min(kBatchSize, packets.size() - offset);
            struct mmsghdr msgs[kBatchSize]{};
            struct iovec iovs[kBatchSize]{};
            for (size_t i = 0; i < count; ++i) {
                const auto& pkt = packets[offset + i];
                iovs[i].iov_base = const_cast<void*>(pkt.data);
                iovs[i].iov_len = pkt.size;
                msgs[i].msg_hdr.msg_iov = &iovs[i];
                msgs[i].msg_hdr.msg_iovlen = 1;
                if (dst != nullptr) {
                    msgs[i].msg_hdr.msg_name = const_cast<sockaddr_in*>(&dst->raw);
                    msgs[i].msg_hdr.msg_namelen = sizeof(dst->raw);
                }
            }
            int ret = 0;
            do {
                ret = ::sendmmsg(fd_, msgs, static_cast<unsigned int>(count), MSG_DONTWAIT);
            } while (ret < 0 && detail::is_eintr(detail::get_last_error()));
            if (ret <= 0) break;
            total_sent += static_cast<size_t>(ret);
            if (static_cast<size_t>(ret) < count) break;
            offset += count;
        }
        return total_sent;
#else
        size_t total_sent = 0;
        for (const auto& pkt : packets) {
            bool ok = (dst != nullptr) ? sendTo(pkt.data, pkt.size, *dst)
                                       : send(pkt.data, pkt.size);
            if (!ok) break;
            ++total_sent;
        }
        return total_sent;
#endif
    }

    template <size_t MaxBatch, bool WantPeer, typename Handler>
    [[nodiscard]] size_t recvBatchImpl(Handler& handler) {
        static_assert(MaxBatch > 0 && MaxBatch <= 64, "MaxBatch must be in [1, 64]");
        static_assert(MaxBatch * RecvBufSize <= 256 * 1024, "Batch stack buffer exceeds 256 KiB");
        if (fd_ == detail::INVALID_SOCKET_FD) return 0;

#ifdef __linux__
        alignas(16) uint8_t batch_buf[MaxBatch][RecvBufSize];
        struct mmsghdr msgs[MaxBatch]{};
        struct iovec iovs[MaxBatch]{};
        Endpoint peers[MaxBatch]{};

        for (size_t i = 0; i < MaxBatch; ++i) {
            iovs[i].iov_base = batch_buf[i];
            iovs[i].iov_len = RecvBufSize;
            msgs[i].msg_hdr.msg_iov = &iovs[i];
            msgs[i].msg_hdr.msg_iovlen = 1;
            if constexpr (WantPeer) {
                msgs[i].msg_hdr.msg_name = &peers[i].raw;
                msgs[i].msg_hdr.msg_namelen = sizeof(peers[i].raw);
            }
        }

        int n = 0;
        do {
            n = ::recvmmsg(fd_, msgs, static_cast<unsigned int>(MaxBatch), MSG_DONTWAIT, nullptr);
        } while (n < 0 && detail::is_eintr(detail::get_last_error()));

        if (n <= 0) return 0;

        for (int i = 0; i < n; ++i) {
            size_t len = static_cast<size_t>(msgs[i].msg_len);
            if (len > RecvBufSize) len = RecvBufSize;
            if constexpr (WantPeer) {
                handler(std::span<const uint8_t>{batch_buf[i], len}, peers[i]);
            } else {
                handler(std::span<const uint8_t>{batch_buf[i], len});
            }
        }
        return static_cast<size_t>(n);
#else
        size_t count = 0;
        for (; count < MaxBatch; ++count) {
            bool ok = false;
            if constexpr (WantPeer) {
                ok = recvFrom(handler);
            } else {
                ok = recv(handler);
            }
            if (!ok) break;
        }
        return count;
#endif
    }

    void saveError(const char* msg, bool check_errno = true, int err_override = 0) noexcept {
        detail::format_socket_error(last_error_, sizeof(last_error_), msg, check_errno, err_override);
    }

    detail::socket_t fd_ = detail::INVALID_SOCKET_FD;
    uint8_t buf_[RecvBufSize];
    char last_error_[128] = "";
};

// ==========================================
// 默认便捷别名 (Default Aliases)
// ==========================================

template <typename UserData = EmptyUserData, size_t RecvBufSize = 4096>
using TcpConnection = SocketTcpConnection<UserData, RecvBufSize>;

template <typename UserData = EmptyUserData, size_t RecvBufSize = 4096>
using TcpClient = SocketTcpClient<UserData, RecvBufSize>;

template <typename UserData = EmptyUserData, size_t RecvBufSize = 4096, size_t MaxConns = 64>
using TcpServer = SocketTcpServer<UserData, RecvBufSize, MaxConns>;

template <size_t RecvBufSize = 1500>
using UdpSocket = SocketUdpSocket<RecvBufSize>;

}  // namespace pollnet
