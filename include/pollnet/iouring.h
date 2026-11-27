#pragma once

#ifdef __linux__

#include "pollnet/socket.h"

#include <linux/io_uring.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <string_view>
#include <utility>

namespace pollnet {

namespace detail {

class IoUring {
   public:
    IoUring() = default;
    IoUring(const IoUring&) = delete;
    IoUring& operator=(const IoUring&) = delete;
    IoUring(IoUring&&) = delete;
    IoUring& operator=(IoUring&&) = delete;

    ~IoUring() { destroy(); }

    [[nodiscard]] bool setup(uint32_t entries, uint32_t flags = 0) noexcept {
        destroy();

        struct io_uring_params p{};
        p.flags = flags;
        int fd = static_cast<int>(::syscall(__NR_io_uring_setup, entries, &p));
        if (fd < 0) {
            return false;
        }
        ring_fd_ = fd;
        features_ = p.features;
        sq_entries_ = p.sq_entries;

        sq_ring_sz_ = p.sq_off.array + p.sq_entries * sizeof(uint32_t);
        cq_ring_sz_ = p.cq_off.cqes + p.cq_entries * sizeof(struct io_uring_cqe);
        if (features_ & IORING_FEAT_SINGLE_MMAP) {
            sq_ring_sz_ = std::max(sq_ring_sz_, cq_ring_sz_);
            cq_ring_sz_ = sq_ring_sz_;
        }

        void* sq_ptr = ::mmap(nullptr, sq_ring_sz_, PROT_READ | PROT_WRITE,
                              MAP_SHARED | MAP_POPULATE, ring_fd_, IORING_OFF_SQ_RING);
        if (sq_ptr == MAP_FAILED) {
            destroy();
            return false;
        }
        sq_ring_ptr_ = sq_ptr;

        if (features_ & IORING_FEAT_SINGLE_MMAP) {
            cq_ring_ptr_ = sq_ring_ptr_;
        } else {
            void* cq_ptr = ::mmap(nullptr, cq_ring_sz_, PROT_READ | PROT_WRITE,
                                  MAP_SHARED | MAP_POPULATE, ring_fd_, IORING_OFF_CQ_RING);
            if (cq_ptr == MAP_FAILED) {
                destroy();
                return false;
            }
            cq_ring_ptr_ = cq_ptr;
        }

        sqes_sz_ = static_cast<size_t>(p.sq_entries) * sizeof(struct io_uring_sqe);
        void* sqes_map = ::mmap(nullptr, sqes_sz_, PROT_READ | PROT_WRITE,
                                MAP_SHARED | MAP_POPULATE, ring_fd_, IORING_OFF_SQES);
        if (sqes_map == MAP_FAILED) {
            destroy();
            return false;
        }
        sqes_ptr_ = static_cast<struct io_uring_sqe*>(sqes_map);

        auto* sq_base = static_cast<uint8_t*>(sq_ring_ptr_);
        sq_head_ = reinterpret_cast<uint32_t*>(sq_base + p.sq_off.head);
        sq_tail_ = reinterpret_cast<uint32_t*>(sq_base + p.sq_off.tail);
        sq_mask_ = *reinterpret_cast<uint32_t*>(sq_base + p.sq_off.ring_mask);
        sq_array_ = reinterpret_cast<uint32_t*>(sq_base + p.sq_off.array);
        local_sq_tail_ = *sq_tail_;
        pending_sqes_ = 0;

        auto* cq_base = static_cast<uint8_t*>(cq_ring_ptr_);
        cq_head_ = reinterpret_cast<uint32_t*>(cq_base + p.cq_off.head);
        cq_tail_ = reinterpret_cast<uint32_t*>(cq_base + p.cq_off.tail);
        cq_mask_ = *reinterpret_cast<uint32_t*>(cq_base + p.cq_off.ring_mask);
        cqes_ = reinterpret_cast<struct io_uring_cqe*>(cq_base + p.cq_off.cqes);
        return true;
    }

    void destroy() noexcept {
        if (sqes_ptr_ != nullptr) {
            ::munmap(sqes_ptr_, sqes_sz_);
            sqes_ptr_ = nullptr;
            sqes_sz_ = 0;
        }
        if (cq_ring_ptr_ != nullptr && cq_ring_ptr_ != sq_ring_ptr_) {
            ::munmap(cq_ring_ptr_, cq_ring_sz_);
        }
        cq_ring_ptr_ = nullptr;
        cq_ring_sz_ = 0;
        if (sq_ring_ptr_ != nullptr) {
            ::munmap(sq_ring_ptr_, sq_ring_sz_);
            sq_ring_ptr_ = nullptr;
            sq_ring_sz_ = 0;
        }
        if (ring_fd_ >= 0) {
            ::close(ring_fd_);
            ring_fd_ = -1;
        }
        sq_head_ = sq_tail_ = sq_array_ = cq_head_ = cq_tail_ = nullptr;
        cqes_ = nullptr;
        sq_entries_ = sq_mask_ = cq_mask_ = local_sq_tail_ = pending_sqes_ = features_ = 0;
    }

    [[nodiscard]] bool isReady() const noexcept { return ring_fd_ >= 0; }
    [[nodiscard]] uint32_t pendingSqes() const noexcept { return pending_sqes_; }

    [[nodiscard]] struct io_uring_sqe* getSqe() noexcept {
        if (ring_fd_ < 0) return nullptr;
        uint32_t head = __atomic_load_n(sq_head_, __ATOMIC_ACQUIRE);
        if (local_sq_tail_ - head >= sq_entries_) {
            return nullptr;
        }
        uint32_t idx = local_sq_tail_ & sq_mask_;
        sq_array_[idx] = idx;
        struct io_uring_sqe* sqe = &sqes_ptr_[idx];
        std::memset(sqe, 0, sizeof(*sqe));
        ++local_sq_tail_;
        ++pending_sqes_;
        return sqe;
    }

    [[nodiscard]] bool hasCqe() const noexcept {
        if (ring_fd_ < 0) return false;
        uint32_t head = *cq_head_;
        uint32_t tail = __atomic_load_n(cq_tail_, __ATOMIC_ACQUIRE);
        return head != tail;
    }

    [[nodiscard]] const struct io_uring_cqe* peekCqe() const noexcept {
        if (ring_fd_ < 0) return nullptr;
        uint32_t head = *cq_head_;
        uint32_t tail = __atomic_load_n(cq_tail_, __ATOMIC_ACQUIRE);
        if (head == tail) return nullptr;
        return &cqes_[head & cq_mask_];
    }

    void advanceCqe(uint32_t count = 1) noexcept {
        uint32_t next_head = *cq_head_ + count;
        __atomic_store_n(cq_head_, next_head, __ATOMIC_RELEASE);
    }

    void submit(int timeout_ms = 0) noexcept {
        if (ring_fd_ < 0) return;

        uint32_t to_submit = pending_sqes_;
        if (to_submit > 0) {
            __atomic_store_n(sq_tail_, local_sq_tail_, __ATOMIC_RELEASE);
            pending_sqes_ = 0;
        }

        const bool want_wait = (timeout_ms > 0 && !hasCqe());
        if (to_submit == 0 && !want_wait) {
            return;
        }

        if (want_wait && (features_ & IORING_FEAT_EXT_ARG)) {
            struct __kernel_timespec ts{};
            ts.tv_sec = timeout_ms / 1000;
            ts.tv_nsec = static_cast<long long>(timeout_ms % 1000) * 1'000'000LL;
            struct io_uring_getevents_arg arg{};
            arg.ts = reinterpret_cast<uint64_t>(&ts);

            long rc = 0;
            do {
                rc = ::syscall(__NR_io_uring_enter, ring_fd_, to_submit, 1U,
                               IORING_ENTER_GETEVENTS | IORING_ENTER_EXT_ARG,
                               static_cast<const void*>(&arg), sizeof(arg));
                if (rc >= 0) break;
                if (errno != EINTR) break;
            } while (true);
        } else {
            uint32_t min_complete = want_wait ? 1U : 0U;
            uint32_t enter_flags = want_wait ? IORING_ENTER_GETEVENTS : 0U;
            long rc = 0;
            do {
                rc = ::syscall(__NR_io_uring_enter, ring_fd_, to_submit, min_complete, enter_flags,
                               static_cast<const void*>(nullptr), size_t{0});
                if (rc >= 0) break;
                if (errno != EINTR) break;
            } while (true);
        }
    }

   private:
    int ring_fd_ = -1;
    uint32_t features_ = 0;
    uint32_t sq_entries_ = 0;
    uint32_t sq_mask_ = 0;
    uint32_t cq_mask_ = 0;
    uint32_t local_sq_tail_ = 0;
    uint32_t pending_sqes_ = 0;

    void* sq_ring_ptr_ = nullptr;
    size_t sq_ring_sz_ = 0;
    void* cq_ring_ptr_ = nullptr;
    size_t cq_ring_sz_ = 0;
    struct io_uring_sqe* sqes_ptr_ = nullptr;
    size_t sqes_sz_ = 0;

    uint32_t* sq_head_ = nullptr;
    uint32_t* sq_tail_ = nullptr;
    uint32_t* sq_array_ = nullptr;
    uint32_t* cq_head_ = nullptr;
    uint32_t* cq_tail_ = nullptr;
    struct io_uring_cqe* cqes_ = nullptr;
};

[[nodiscard]] constexpr uint32_t next_pow2_u32(uint32_t v) noexcept {
    if (v <= 1U) return 1U;
    --v;
    v |= v >> 1U;
    v |= v >> 2U;
    v |= v >> 4U;
    v |= v >> 8U;
    v |= v >> 16U;
    return v + 1U;
}

}  // namespace detail

template <typename UserData = EmptyUserData, size_t RecvBufSize = 4096, size_t MaxConns = 64>
class IoUringTcpServer {
    static_assert(MaxConns > 0, "MaxConns must be > 0");
    static_assert(MaxConns <= static_cast<size_t>(std::numeric_limits<uint32_t>::max()) - 1,
                  "MaxConns exceeds uint32_t slot range");

   public:
    using Conn = SocketTcpConnection<UserData, RecvBufSize>;

    IoUringTcpServer() noexcept { resetSlots(); }
    IoUringTcpServer(const IoUringTcpServer&) = delete;
    IoUringTcpServer& operator=(const IoUringTcpServer&) = delete;
    IoUringTcpServer(IoUringTcpServer&&) = delete;
    IoUringTcpServer& operator=(IoUringTcpServer&&) = delete;

    ~IoUringTcpServer() { close("destruct"); }

    [[nodiscard]] bool listen(const TcpServerOptions& opts) noexcept {
        closeAllConnections("reinit");
        close("reinit");

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
        if (::setsockopt(listenfd_, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes)) < 0) {
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

        uint32_t ring_entries =
            detail::next_pow2_u32(std::max<uint32_t>(64U, static_cast<uint32_t>(MaxConns * 2 + 16)));
        if (!ring_.setup(ring_entries)) {
            close("io_uring_setup error", true);
            return false;
        }

        send_timeout_ns_ = detail::to_timeout_ns(opts.send_timeout);
        recv_timeout_ns_ = detail::to_timeout_ns(opts.recv_timeout);
        tcp_quickack_ = opts.tcp_quickack;
        busy_poll_us_ = opts.busy_poll_us;
        recv_buf_bytes_ = opts.recv_buf_bytes;
        send_buf_bytes_ = opts.send_buf_bytes;

        ++accept_gen_;
        accept_armed_ = false;
        prepAccept();
        ring_.submit(0);
        return true;
    }

    void close(const char* reason, bool check_errno = false, int err_override = 0) noexcept {
        if (listenfd_ != detail::INVALID_SOCKET_FD) {
            saveError(reason, check_errno, err_override);
            detail::close_socket(listenfd_);
            listenfd_ = detail::INVALID_SOCKET_FD;
        }
        accept_armed_ = false;
        ring_.destroy();
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
    static constexpr uint64_t kTagMask = 0xFF00000000000000ULL;
    static constexpr uint64_t kTagAccept = 0xAA00000000000000ULL;
    static constexpr uint64_t kTagCancel = 0xCC00000000000000ULL;

    [[nodiscard]] uint64_t makeRecvToken(size_t slot) const noexcept {
        return (static_cast<uint64_t>(gen_[slot]) << 32) | static_cast<uint32_t>(slot);
    }

    [[nodiscard]] uint64_t makeAcceptToken() const noexcept {
        return kTagAccept | static_cast<uint64_t>(accept_gen_);
    }

    void resetSlots() noexcept {
        active_cnt_ = 0;
        free_top_ = MaxConns;
        for (size_t i = 0; i < MaxConns; ++i) {
            free_slots_[i] = MaxConns - 1 - i;
            active_pos_[i] = 0;
            active_slots_[i] = 0;
            slot_active_[i] = false;
            slot_closing_[i] = false;
            recv_armed_[i] = false;
            gen_[i] = 1;
        }
    }

    void closeAllConnections(const char* reason) noexcept {
        for (size_t i = active_cnt_; i > 0; --i) {
            size_t slot = active_slots_[i - 1];
            conns_data_[slot].close(reason);
        }
        resetSlots();
    }

    void prepAccept() noexcept {
        if (isClosed() || accept_armed_ || free_top_ == 0 || !ring_.isReady()) return;
        struct io_uring_sqe* sqe = ring_.getSqe();
        if (sqe == nullptr) return;
        accept_addr_ = {};
        accept_addr_len_ = sizeof(accept_addr_);
        sqe->opcode = IORING_OP_ACCEPT;
        sqe->fd = listenfd_;
        sqe->addr = reinterpret_cast<uint64_t>(&accept_addr_);
        sqe->addr2 = reinterpret_cast<uint64_t>(&accept_addr_len_);
        sqe->accept_flags = SOCK_NONBLOCK | SOCK_CLOEXEC;
        sqe->user_data = makeAcceptToken();
        accept_armed_ = true;
    }

    void prepRecv(size_t slot) noexcept {
        if (!slot_active_[slot] || recv_armed_[slot] || !ring_.isReady()) return;
        Conn& c = conns_data_[slot];
        if (!c.isConnected()) return;
        if (c.tail_ >= RecvBufSize) {
            c.close("recv buf full", false);
            return;
        }
        struct io_uring_sqe* sqe = ring_.getSqe();
        if (sqe == nullptr) return;
        sqe->opcode = IORING_OP_RECV;
        sqe->fd = c.fd_;
        sqe->addr = reinterpret_cast<uint64_t>(c.recvbuf_ + c.tail_);
        sqe->len = static_cast<__u32>(RecvBufSize - c.tail_);
        sqe->user_data = makeRecvToken(slot);
        recv_armed_[slot] = true;
    }

    void prepCancelRecv(size_t slot) noexcept {
        if (!ring_.isReady()) return;
        struct io_uring_sqe* sqe = ring_.getSqe();
        if (sqe == nullptr) return;
        sqe->opcode = IORING_OP_ASYNC_CANCEL;
        sqe->addr = makeRecvToken(slot);
        sqe->user_data = kTagCancel;
    }

    template <typename Handler>
    void pollImpl(Handler& handler, int timeout_ms, bool use_sys_clock, int64_t now) {
        if ((isClosed() && active_cnt_ == 0) || !ring_.isReady()) return;

        if (!ring_.hasCqe()) {
            ring_.submit(timeout_ms);
        } else if (ring_.pendingSqes() > 0) {
            ring_.submit(0);
        }

        if (use_sys_clock && (send_timeout_ns_ > 0 || recv_timeout_ns_ > 0)) {
            now = now_ns();
        }

        while (const struct io_uring_cqe* cqe = ring_.peekCqe()) {
            const uint64_t udata = cqe->user_data;
            const int32_t res = cqe->res;
            ring_.advanceCqe(1);

            if ((udata & kTagMask) == kTagCancel) {
                continue;
            }
            if ((udata & kTagMask) == kTagAccept) {
                handleAcceptCqe(handler, udata, res, now);
                continue;
            }
            handleRecvCqe(handler, udata, res, now);
        }

        if (!accept_armed_ && !isClosed() && free_top_ > 0) {
            prepAccept();
        }
        if (ring_.pendingSqes() > 0) {
            ring_.submit(0);
        }

        if (send_timeout_ns_ > 0 || recv_timeout_ns_ > 0) {
            checkTimeouts(handler, now);
            if (ring_.pendingSqes() > 0) {
                ring_.submit(0);
            }
        }
    }

    template <typename Handler>
    void handleAcceptCqe(Handler& handler, uint64_t udata, int32_t res, int64_t now) {
        uint32_t token_gen = static_cast<uint32_t>(udata & 0xFFFFFFFFULL);
        if (token_gen != accept_gen_) {
            if (res >= 0) ::close(res);
            return;
        }
        accept_armed_ = false;

        if (res >= 0) {
            int fd = res;
            if (isClosed() || free_top_ == 0 ||
                !detail::dispatch_allow_new_connection(handler, Endpoint(accept_addr_))) {
                ::close(fd);
            } else {
                size_t slot = free_slots_[free_top_ - 1];
                Conn& c = conns_data_[slot];
                if (c.open(now, fd, send_timeout_ns_, recv_timeout_ns_, tcp_quickack_,
                           recv_buf_bytes_, send_buf_bytes_, busy_poll_us_)) {
                    --free_top_;
                    slot_active_[slot] = true;
                    slot_closing_[slot] = false;
                    recv_armed_[slot] = false;
                    ++gen_[slot];
                    active_pos_[slot] = active_cnt_;
                    active_slots_[active_cnt_++] = slot;
                    detail::dispatch_connect(handler, c);
                    if (c.isConnected()) {
                        prepRecv(slot);
                    } else {
                        releaseSlot(slot);
                        detail::dispatch_disconnect(handler, c);
                    }
                }
            }
        }

        if (!isClosed() && free_top_ > 0 && (res >= 0 || res != -ECANCELED)) {
            prepAccept();
        }
    }

    template <typename Handler>
    void handleRecvCqe(Handler& handler, uint64_t udata, int32_t res, int64_t now) {
        size_t slot = static_cast<size_t>(udata & 0xFFFFFFFFULL);
        uint32_t token_gen = static_cast<uint32_t>(udata >> 32);
        if (slot >= MaxConns) return;

        if (slot_closing_[slot] && token_gen == gen_[slot]) {
            recv_armed_[slot] = false;
            slot_closing_[slot] = false;
            ++gen_[slot];
            free_slots_[free_top_++] = slot;
            if (!accept_armed_ && !isClosed()) {
                prepAccept();
            }
            return;
        }

        if (!slot_active_[slot] || token_gen != gen_[slot]) {
            return;
        }
        recv_armed_[slot] = false;

        Conn& c = conns_data_[slot];
        if (!c.isConnected()) {
            releaseSlot(slot);
            detail::dispatch_disconnect(handler, c);
            return;
        }

        if (res > 0) {
            c.tail_ += static_cast<size_t>(res);
            if (c.tcp_quickack_) {
                detail::set_tcp_quickack(c.fd_);
            }
            size_t avail = c.tail_ - c.head_;
            size_t consumed =
                static_cast<size_t>(handler.onData(c, std::span<const uint8_t>{c.recvbuf_ + c.head_, avail}));
            if (!c.isConnected()) {
                c.head_ = c.tail_ = 0;
            } else if (consumed > avail) {
                c.close("invalid consumed size", false);
            } else {
                size_t remaining = avail - consumed;
                if (remaining == 0) {
                    c.head_ = c.tail_ = 0;
                } else if (remaining < RecvBufSize) {
                    c.head_ += consumed;
                    if ((c.head_ >= RecvBufSize / 2 || c.tail_ == RecvBufSize) && c.head_ > 0 &&
                        c.head_ + remaining <= RecvBufSize) {
                        std::memmove(c.recvbuf_, c.recvbuf_ + c.head_, remaining);
                        c.head_ = 0;
                        c.tail_ = remaining;
                    } else if (c.tail_ == RecvBufSize) {
                        c.close("recv buf full", false);
                    }
                } else {
                    c.close("recv buf full", false);
                }
            }
            if (c.isConnected() && recv_timeout_ns_ > 0) {
                c.recv_ts_ = now;
            }
        } else if (res == 0) {
            c.close("remote close");
        } else {
            int err = -res;
            if (err != EAGAIN && err != EWOULDBLOCK && err != EINTR) {
                c.close("io_uring recv error", true, err);
            }
        }

        if (c.isConnected()) {
            prepRecv(slot);
        } else {
            releaseSlot(slot);
            detail::dispatch_disconnect(handler, c);
        }
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
        if (recv_armed_[slot]) {
            slot_closing_[slot] = true;
            prepCancelRecv(slot);
        } else {
            ++gen_[slot];
            free_slots_[free_top_++] = slot;
            if (!accept_armed_ && !isClosed()) {
                prepAccept();
            }
        }
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

    detail::IoUring ring_;
    detail::socket_t listenfd_ = detail::INVALID_SOCKET_FD;
    bool accept_armed_ = false;
    uint32_t accept_gen_ = 0;
    struct sockaddr_in accept_addr_{};
    socklen_t accept_addr_len_ = sizeof(accept_addr_);

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
    bool slot_closing_[MaxConns]{};
    bool recv_armed_[MaxConns]{};
    uint32_t gen_[MaxConns]{};
    Conn conns_data_[MaxConns];
    char last_error_[128] = "";
};

}  // namespace pollnet

#endif  // __linux__
