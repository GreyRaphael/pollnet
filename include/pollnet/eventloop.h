#pragma once

#include "pollnet/socket.h"

#ifdef __linux__
#include <sys/epoll.h>
#elif !defined(_WIN32)
#include <poll.h>
#endif

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string_view>

namespace pollnet {

#ifdef __linux__
namespace detail {
template <size_t MaxConns, bool Enable>
struct WriteInterestState {
    [[nodiscard]] constexpr bool get(size_t) const noexcept { return false; }
    constexpr void set(size_t, bool) noexcept {}
    constexpr void reset() noexcept {}
};

template <size_t MaxConns>
struct WriteInterestState<MaxConns, true> {
    [[nodiscard]] bool get(size_t slot) const noexcept { return armed_[slot]; }
    void set(size_t slot, bool v) noexcept { armed_[slot] = v; }
    void reset() noexcept { armed_.fill(false); }
    std::array<bool, MaxConns> armed_{};
};
}  // namespace detail
#endif

template <typename UserData = EmptyUserData, size_t RecvBufSize = 4096, size_t MaxConns = 64,
          size_t SendBufSize = 0>
class EventLoopTcpServer {
    static_assert(MaxConns > 0, "MaxConns must be > 0");
    static_assert(MaxConns <= static_cast<size_t>(std::numeric_limits<uint32_t>::max()) - 1,
                  "MaxConns exceeds uint32_t token range");

   public:
    using Conn = SocketTcpConnection<UserData, RecvBufSize, SendBufSize>;

    EventLoopTcpServer() noexcept { resetSlots(); }
    EventLoopTcpServer(const EventLoopTcpServer&) = delete;
    EventLoopTcpServer& operator=(const EventLoopTcpServer&) = delete;
    EventLoopTcpServer(EventLoopTcpServer&&) = delete;
    EventLoopTcpServer& operator=(EventLoopTcpServer&&) = delete;

    ~EventLoopTcpServer() { close("destruct"); }

    [[nodiscard]] bool listen(const TcpServerOptions& opts) noexcept {
        closeAllConnections("reinit");
        close("reinit");

        if (!listener_.listen(opts)) {
            return false;
        }

#ifdef __linux__
        epoll_fd_ = ::epoll_create1(EPOLL_CLOEXEC);
        if (epoll_fd_ < 0) {
            listener_.close("epoll_create1 error", true);
            return false;
        }
        if (!armListen(true)) {
            listener_.close("epoll_ctl listen error", true);
            ::close(epoll_fd_);
            epoll_fd_ = -1;
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
#ifdef __linux__
        if (epoll_fd_ >= 0 && listen_armed_ && !listener_.isClosed()) {
            ::epoll_ctl(epoll_fd_, EPOLL_CTL_DEL, listener_.fd(), nullptr);
            listen_armed_ = false;
        }
#endif
        listener_.close(reason, check_errno, err_override);
#ifdef __linux__
        if (epoll_fd_ >= 0) {
            ::close(epoll_fd_);
            epoll_fd_ = -1;
        }
        listen_armed_ = false;
#endif
    }

    [[nodiscard]] std::string_view lastError() const noexcept { return listener_.lastError(); }
    [[nodiscard]] bool isClosed() const noexcept { return listener_.isClosed(); }
    [[nodiscard]] size_t connCount() const noexcept { return active_cnt_; }
    [[nodiscard]] uint16_t localPort() const noexcept { return listener_.localPort(); }

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
        if (listener_.isClosed() && active_cnt_ == 0) return;

        if constexpr (SendBufSize > 0) {
            flushPendingWrites(handler);
            if (listener_.isClosed() && active_cnt_ == 0) return;
        }

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
            bool can_write = (ev_mask & EPOLLOUT) != 0;
            bool can_read = (ev_mask & ~uint32_t{EPOLLOUT}) != 0;
            bool has_err = (ev_mask & EPOLLERR) != 0;
            handleActiveSlot(handler, static_cast<size_t>(token), can_read, can_write, has_err, now);
        }
#else
#ifdef _WIN32
        using pollfd_t = WSAPOLLFD;
        constexpr short kPollRead = POLLRDNORM;
        constexpr short kPollWrite = POLLWRNORM;
#else
        using pollfd_t = struct pollfd;
        constexpr short kPollRead = POLLIN;
        constexpr short kPollWrite = POLLOUT;
#endif
        std::array<pollfd_t, MaxConns + 1> pfds{};
        std::array<uint32_t, MaxConns + 1> tokens{};
        size_t nfds = 0;

        if (!listener_.isClosed() && free_top_ > 0) {
            pfds[nfds].fd = listener_.fd();
            pfds[nfds].events = kPollRead;
            pfds[nfds].revents = 0;
            tokens[nfds] = kListenToken;
            ++nfds;
        }
        for (size_t i = 0; i < active_cnt_; ++i) {
            size_t slot = active_slots_[i];
            short ev = kPollRead;
            if constexpr (SendBufSize > 0) {
                if (conns_data_[slot].pendingSendBytes() > 0) {
                    ev |= kPollWrite;
                }
            }
            pfds[nfds].fd = conns_data_[slot].fd_;
            pfds[nfds].events = ev;
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
                bool can_write = (rev & kPollWrite) != 0;
                bool can_read = (rev & static_cast<short>(~kPollWrite)) != 0;
                bool has_err = (rev & POLLERR) != 0;
                handleActiveSlot(handler, static_cast<size_t>(token), can_read, can_write, has_err,
                                 now);
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
#ifdef __linux__
        write_armed_.reset();
#endif
    }

    void closeAllConnections(const char* reason) noexcept {
        for (size_t i = active_cnt_; i > 0; --i) {
            conns_data_[active_slots_[i - 1]].close(reason);
        }
        resetSlots();
    }

#ifdef __linux__
    [[nodiscard]] bool armListen(bool enable) noexcept {
        if (epoll_fd_ < 0 || listener_.isClosed()) return false;
        if (enable == listen_armed_) return true;
        if (enable) {
            struct epoll_event ev{};
            ev.events = EPOLLIN;
            ev.data.u32 = kListenToken;
            if (::epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, listener_.fd(), &ev) < 0) return false;
            listen_armed_ = true;
        } else {
            ::epoll_ctl(epoll_fd_, EPOLL_CTL_DEL, listener_.fd(), nullptr);
            listen_armed_ = false;
        }
        return true;
    }

    [[nodiscard]] bool updateWriteInterest(size_t slot, bool want_write) noexcept {
        if (epoll_fd_ < 0 || slot >= MaxConns || !slot_active_[slot]) return true;
        if (write_armed_.get(slot) == want_write) return true;
        struct epoll_event ev{};
        ev.events = want_write ? (EPOLLIN | EPOLLOUT) : EPOLLIN;
        ev.data.u32 = static_cast<uint32_t>(slot);
        if (::epoll_ctl(epoll_fd_, EPOLL_CTL_MOD, conns_data_[slot].fd_, &ev) < 0) {
            return false;
        }
        write_armed_.set(slot, want_write);
        return true;
    }
#endif

    template <typename Handler>
    void flushPendingWrites(Handler& handler) {
        for (size_t i = active_cnt_; i > 0; --i) {
            size_t slot = active_slots_[i - 1];
            Conn& c = conns_data_[slot];
            if (c.pendingSendBytes() > 0) {
                (void)c.flushSendBuffer();
                if (!c.isConnected()) {
                    releaseSlot(slot);
                    detail::dispatch_disconnect(handler, c);
                    continue;
                }
            }
#ifdef __linux__
            if (!updateWriteInterest(slot, c.pendingSendBytes() > 0)) {
                c.close("epoll_ctl mod error", true);
                releaseSlot(slot);
                detail::dispatch_disconnect(handler, c);
            }
#endif
        }
    }

    template <typename Handler>
    void handleActiveSlot(Handler& handler, size_t slot, bool can_read, bool can_write, bool has_err,
                          int64_t now) {
        if (slot >= MaxConns || !slot_active_[slot]) return;
        Conn& c = conns_data_[slot];
        if constexpr (SendBufSize > 0) {
            if (can_write && c.pendingSendBytes() > 0) {
                (void)c.flushSendBuffer();
#ifdef __linux__
                if (c.isConnected() && c.pendingSendBytes() == 0) {
                    (void)updateWriteInterest(slot, false);
                }
#endif
            }
        } else {
            (void)can_write;
        }
        if (c.isConnected() && can_read) {
            bool got_data = c.read([&](std::span<const uint8_t> buf) {
                return static_cast<size_t>(handler.onData(c, buf));
            });
            if (got_data && recv_timeout_ns_ > 0) {
                c.recv_ts_ = now;
            }
        }
        if (c.isConnected() && has_err) {
            c.close("eventloop error", false);
        }
        if (!c.isConnected()) {
            releaseSlot(slot);
            detail::dispatch_disconnect(handler, c);
        }
    }

    template <typename Handler>
    void acceptReady(Handler& handler, int64_t now) {
        while (!listener_.isClosed() && free_top_ > 0) {
            Endpoint peer_ep;
            detail::socket_t fd = listener_.acceptOne(peer_ep);
            if (fd == detail::INVALID_SOCKET_FD) break;

            if (!detail::dispatch_allow_new_connection(handler, peer_ep)) {
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
#ifdef __linux__
            write_armed_.set(slot, false);
#endif
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
#ifdef __linux__
        write_armed_.set(slot, false);
#endif
        size_t pos = active_pos_[slot];
        size_t last_slot = active_slots_[--active_cnt_];
        if (pos != active_cnt_) {
            active_slots_[pos] = last_slot;
            active_pos_[last_slot] = pos;
        }
        free_slots_[free_top_++] = slot;
#ifdef __linux__
        if (!listener_.isClosed() && !listen_armed_) {
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

    detail::TcpListener listener_;
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
#ifdef __linux__
    [[no_unique_address]] detail::WriteInterestState<MaxConns, (SendBufSize > 0)> write_armed_;
#endif
    Conn conns_data_[MaxConns];
};

}  // namespace pollnet
