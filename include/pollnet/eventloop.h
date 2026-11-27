#pragma once

#include "pollnet/socket.h"

#ifdef __linux__
#include <sys/epoll.h>
#elif !defined(_WIN32)
#include <poll.h>
#endif

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string_view>
#include <utility>

namespace pollnet {

template <typename UserData = EmptyUserData, size_t RecvBufSize = 4096, size_t MaxConns = 64>
class EventLoopTcpServer {
    static_assert(MaxConns > 0, "MaxConns must be > 0");
    static_assert(MaxConns <= static_cast<size_t>(std::numeric_limits<uint32_t>::max()) - 1,
                  "MaxConns exceeds uint32_t token range");

   public:
    using Conn = SocketTcpConnection<UserData, RecvBufSize>;

    EventLoopTcpServer() noexcept { resetSlots(); }
    EventLoopTcpServer(const EventLoopTcpServer&) = delete;
    EventLoopTcpServer& operator=(const EventLoopTcpServer&) = delete;
    EventLoopTcpServer(EventLoopTcpServer&&) = delete;
    EventLoopTcpServer& operator=(EventLoopTcpServer&&) = delete;

    ~EventLoopTcpServer() { close("destruct"); }

    [[nodiscard]] bool listen(const TcpServerOptions& opts) noexcept {
        closeAllConnections("reinit");
        close("reinit");

        if (!detail::ensure_network_init()) {
            saveError("WSAStartup failed", false);
            return false;
        }

        auto ep = Endpoint::from(opts.host, opts.port);
        if (!ep) {
            saveError("invalid host", false);
            return false;
        }

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

        detail::apply_socket_tuning(listenfd_, true, opts.recv_buf_bytes, opts.send_buf_bytes,
                                    opts.busy_poll_us, opts.tcp_quickack);

        if (::bind(listenfd_, reinterpret_cast<const struct sockaddr*>(&ep->raw), sizeof(ep->raw)) <
            0) {
            close("bind error", true);
            return false;
        }

        int backlog = (opts.backlog > 0) ? opts.backlog : 128;
        if (::listen(listenfd_, backlog) < 0) {
            close("listen error", true);
            return false;
        }

#ifdef __linux__
        epoll_fd_ = ::epoll_create1(EPOLL_CLOEXEC);
        if (epoll_fd_ < 0) {
            close("epoll_create1 error", true);
            return false;
        }
        if (!armListen(true)) {
            close("epoll_ctl listen error", true);
            return false;
        }
#endif

        send_timeout_ns_ = detail::to_timeout_ns(opts.send_timeout);
        recv_timeout_ns_ = detail::to_timeout_ns(opts.recv_timeout);
        tcp_quickack_ = opts.tcp_quickack;
        busy_poll_us_ = opts.busy_poll_us;
        recv_buf_bytes_ = opts.recv_buf_bytes;
        send_buf_bytes_ = opts.send_buf_bytes;
        return true;
    }

    void close(const char* reason, bool check_errno = false, int err_override = 0) noexcept {
        if (listenfd_ != detail::INVALID_SOCKET_FD) {
            saveError(reason, check_errno, err_override);
#ifdef __linux__
            if (epoll_fd_ >= 0 && listen_armed_) {
                ::epoll_ctl(epoll_fd_, EPOLL_CTL_DEL, listenfd_, nullptr);
                listen_armed_ = false;
            }
#endif
            detail::close_socket(listenfd_);
            listenfd_ = detail::INVALID_SOCKET_FD;
        }
#ifdef __linux__
        if (epoll_fd_ >= 0) {
            ::close(epoll_fd_);
            epoll_fd_ = -1;
        }
        listen_armed_ = false;
#endif
    }

    [[nodiscard]] std::string_view lastError() const noexcept { return last_error_; }

    [[nodiscard]] bool isClosed() const noexcept {
        return listenfd_ == detail::INVALID_SOCKET_FD;
    }

    [[nodiscard]] size_t connCount() const noexcept { return active_cnt_; }

    [[nodiscard]] uint16_t localPort() const noexcept {
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
        for (size_t i = 0; i < active_cnt_; ++i) {
            handler(conns_data_[active_slots_[i]]);
        }
    }

    template <typename Handler>
        requires TcpHandler<Handler, Conn>
    void poll(Handler& handler, int timeout_ms = 0) {
        pollImpl(handler, timeout_ms, true, 0);
    }

    template <typename Handler>
        requires TcpHandler<Handler, Conn>
    void poll(Handler& handler, int timeout_ms, int64_t now) {
        pollImpl(handler, timeout_ms, false, now);
    }

   private:
    template <typename Handler>
    void pollImpl(Handler& handler, int timeout_ms, bool use_sys_clock, int64_t now) {
        if (isClosed() && active_cnt_ == 0) return;

#ifdef __linux__
        if (epoll_fd_ < 0) return;
        constexpr size_t kMaxEvents = (MaxConns + 1 < 128) ? (MaxConns + 1) : 128;
        std::array<struct epoll_event, kMaxEvents> events{};
        int n = 0;
        do {
            n = ::epoll_wait(epoll_fd_, events.data(), static_cast<int>(kMaxEvents), timeout_ms);
        } while (n < 0 && detail::is_eintr(detail::get_last_error()));

        if (use_sys_clock && (send_timeout_ns_ > 0 || recv_timeout_ns_ > 0)) {
            now = now_ns();
        }

        for (int i = 0; i < n; ++i) {
            uint32_t token = events[static_cast<size_t>(i)].data.u32;
            uint32_t ev_mask = events[static_cast<size_t>(i)].events;

            if (token == kListenToken) {
                acceptReady(handler, now);
                continue;
            }

            size_t slot = static_cast<size_t>(token);
            if (slot >= MaxConns || !slot_active_[slot]) continue;

            Conn& c = conns_data_[slot];
            bool got_data = c.read([&](std::span<const uint8_t> buf) {
                return static_cast<size_t>(handler.onData(c, buf));
            });
            if (got_data && recv_timeout_ns_ > 0) {
                c.recv_ts_ = now;
            }
            if (c.isConnected() && (ev_mask & EPOLLERR)) {
                c.close("epoll error", false);
            }
            if (!c.isConnected()) {
                releaseSlot(slot);
                detail::dispatch_disconnect(handler, c);
            }
        }
#else
#ifdef _WIN32
        using pollfd_t = WSAPOLLFD;
        constexpr short kPollRead = POLLRDNORM;
#else
        using pollfd_t = struct pollfd;
        constexpr short kPollRead = POLLIN;
#endif
        std::array<pollfd_t, MaxConns + 1> pfds{};
        std::array<uint32_t, MaxConns + 1> tokens{};
        size_t nfds = 0;

        if (!isClosed() && free_top_ > 0) {
            pfds[nfds].fd = listenfd_;
            pfds[nfds].events = kPollRead;
            pfds[nfds].revents = 0;
            tokens[nfds] = kListenToken;
            ++nfds;
        }
        for (size_t i = 0; i < active_cnt_; ++i) {
            size_t slot = active_slots_[i];
            pfds[nfds].fd = conns_data_[slot].fd_;
            pfds[nfds].events = kPollRead;
            pfds[nfds].revents = 0;
            tokens[nfds] = static_cast<uint32_t>(slot);
            ++nfds;
        }
        if (nfds == 0) return;

        int n = 0;
        do {
#ifdef _WIN32
            n = ::WSAPoll(pfds.data(), static_cast<ULONG>(nfds), timeout_ms);
#else
            n = ::poll(pfds.data(), static_cast<nfds_t>(nfds), timeout_ms);
#endif
        } while (n < 0 && detail::is_eintr(detail::get_last_error()));

        if (use_sys_clock && (send_timeout_ns_ > 0 || recv_timeout_ns_ > 0)) {
            now = now_ns();
        }

        if (n > 0) {
            for (size_t i = 0; i < nfds; ++i) {
                short rev = pfds[i].revents;
                if (rev == 0) continue;
                uint32_t token = tokens[i];
                if (token == kListenToken) {
                    acceptReady(handler, now);
                    continue;
                }
                size_t slot = static_cast<size_t>(token);
                if (slot >= MaxConns || !slot_active_[slot]) continue;

                Conn& c = conns_data_[slot];
                bool got_data = c.read([&](std::span<const uint8_t> buf) {
                    return static_cast<size_t>(handler.onData(c, buf));
                });
                if (got_data && recv_timeout_ns_ > 0) {
                    c.recv_ts_ = now;
                }
                if (c.isConnected() && (rev & POLLERR)) {
                    c.close("poll error", false);
                }
                if (!c.isConnected()) {
                    releaseSlot(slot);
                    detail::dispatch_disconnect(handler, c);
                }
            }
        }
#endif

        if (send_timeout_ns_ > 0 || recv_timeout_ns_ > 0) {
            checkTimeouts(handler, now);
        }
    }

    static constexpr uint32_t kListenToken = std::numeric_limits<uint32_t>::max();

    void resetSlots() noexcept {
        active_cnt_ = 0;
        free_top_ = MaxConns;
        for (size_t i = 0; i < MaxConns; ++i) {
            free_slots_[i] = MaxConns - 1 - i;
            active_pos_[i] = 0;
            active_slots_[i] = 0;
            slot_active_[i] = false;
        }
    }

    void closeAllConnections(const char* reason) noexcept {
        for (size_t i = active_cnt_; i > 0; --i) {
            size_t slot = active_slots_[i - 1];
            conns_data_[slot].close(reason);
        }
        resetSlots();
    }

#ifdef __linux__
    [[nodiscard]] bool armListen(bool enable) noexcept {
        if (epoll_fd_ < 0 || isClosed()) return false;
        if (enable == listen_armed_) return true;
        if (enable) {
            struct epoll_event ev{};
            ev.events = EPOLLIN;
            ev.data.u32 = kListenToken;
            if (::epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, listenfd_, &ev) < 0) {
                return false;
            }
            listen_armed_ = true;
        } else {
            ::epoll_ctl(epoll_fd_, EPOLL_CTL_DEL, listenfd_, nullptr);
            listen_armed_ = false;
        }
        return true;
    }
#endif

    template <typename Handler>
    void acceptReady(Handler& handler, int64_t now) {
        while (!isClosed() && free_top_ > 0) {
            struct sockaddr_in clientaddr{};
            socklen_t addr_len = sizeof(clientaddr);
            detail::socket_t fd =
                ::accept(listenfd_, reinterpret_cast<struct sockaddr*>(&clientaddr), &addr_len);
            if (fd == detail::INVALID_SOCKET_FD) {
                if (detail::is_eintr(detail::get_last_error())) continue;
                break;
            }
            if (!detail::dispatch_allow_new_connection(handler, Endpoint(clientaddr))) {
                detail::close_socket(fd);
                continue;
            }

            size_t slot = free_slots_[free_top_ - 1];
            Conn& c = conns_data_[slot];
            if (!c.open(now, fd, send_timeout_ns_, recv_timeout_ns_, tcp_quickack_, recv_buf_bytes_,
                        send_buf_bytes_, busy_poll_us_)) {
                continue;
            }

#ifdef __linux__
            struct epoll_event ev{};
            ev.events = EPOLLIN;
            ev.data.u32 = static_cast<uint32_t>(slot);
            if (::epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, c.fd_, &ev) < 0) {
                c.close("epoll_ctl add error", true);
                continue;
            }
#endif

            --free_top_;
            slot_active_[slot] = true;
            active_pos_[slot] = active_cnt_;
            active_slots_[active_cnt_++] = slot;
            detail::dispatch_connect(handler, c);
        }
#ifdef __linux__
        if (free_top_ == 0) {
            (void)armListen(false);
        }
#endif
    }

    void releaseSlot(size_t slot) noexcept {
        if (!slot_active_[slot]) return;
        slot_active_[slot] = false;
        size_t pos = active_pos_[slot];
        size_t last_slot = active_slots_[--active_cnt_];
        if (pos != active_cnt_) {
            active_slots_[pos] = last_slot;
            active_pos_[last_slot] = pos;
        }
        free_slots_[free_top_++] = slot;
#ifdef __linux__
        if (!isClosed() && !listen_armed_) {
            (void)armListen(true);
        }
#endif
    }

    template <typename Handler>
    void checkTimeouts(Handler& handler, int64_t now) {
        for (size_t i = active_cnt_; i > 0; --i) {
            size_t slot = active_slots_[i - 1];
            Conn& c = conns_data_[slot];
            if (send_timeout_ns_ > 0 && now - c.send_ts_ >= send_timeout_ns_) {
                detail::dispatch_send_timeout(handler, c);
                c.send_ts_ = now;
            }
            if (recv_timeout_ns_ > 0 && c.isConnected() && now - c.recv_ts_ >= recv_timeout_ns_) {
                detail::dispatch_recv_timeout(handler, c);
                c.recv_ts_ = now;
            }
            if (!c.isConnected()) {
                releaseSlot(slot);
                detail::dispatch_disconnect(handler, c);
            }
        }
    }

    void saveError(const char* msg, bool check_errno = true, int err_override = 0) noexcept {
        detail::format_socket_error(last_error_, sizeof(last_error_), msg, check_errno, err_override);
    }

    detail::socket_t listenfd_ = detail::INVALID_SOCKET_FD;
#ifdef __linux__
    int epoll_fd_ = -1;
    bool listen_armed_ = false;
#endif
    size_t active_cnt_ = 0;
    size_t free_top_ = 0;
    int64_t send_timeout_ns_ = 0;
    int64_t recv_timeout_ns_ = 0;
    bool tcp_quickack_ = false;
    int busy_poll_us_ = 0;
    int recv_buf_bytes_ = 0;
    int send_buf_bytes_ = 0;
    size_t active_slots_[MaxConns]{};
    size_t active_pos_[MaxConns]{};
    size_t free_slots_[MaxConns]{};
    bool slot_active_[MaxConns]{};
    Conn conns_data_[MaxConns];
    char last_error_[128] = "";
};

}  // namespace pollnet
