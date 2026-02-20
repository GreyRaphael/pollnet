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

using socket_t = SOCKET;
constexpr socket_t INVALID_SOCKET_FD = INVALID_SOCKET;
using sockopt_val_t = const char*;

inline int get_last_error() { return WSAGetLastError(); }
inline bool is_would_block(int err) { return err == WSAEWOULDBLOCK; }
inline bool is_in_progress(int err) { return err == WSAEWOULDBLOCK || err == WSAEINPROGRESS || err == WSAEALREADY; }
inline bool is_isconn(int err) { return err == WSAEISCONN; }
inline void close_socket(socket_t s) { closesocket(s); }
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <linux/if_packet.h>
#include <net/ethernet.h>
#include <net/if.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#include <cerrno>

using socket_t = int;
constexpr socket_t INVALID_SOCKET_FD = -1;
using sockopt_val_t = const void*;

inline int get_last_error() { return errno; }
inline bool is_would_block(int err) { return err == EAGAIN || err == EWOULDBLOCK; }
inline bool is_in_progress(int err) { return err == EINPROGRESS || err == EALREADY; }
inline bool is_isconn(int err) { return err == EISCONN; }
inline void close_socket(socket_t s) { ::close(s); }
#endif

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <limits>

// C++20 线程安全的全局 WSA 初始化助手
inline void ensure_network_init() {
#ifdef _WIN32
    static const bool initialized = []() {
        WSADATA wsaData;
        return WSAStartup(MAKEWORD(2, 2), &wsaData) == 0;
    }();
    (void)initialized;
#endif
}

// 统一设置非阻塞模式
inline bool set_nonblocking(socket_t fd) {
#ifdef _WIN32
    u_long mode = 1;
    return ioctlsocket(fd, FIONBIO, &mode) == 0;
#else
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0) return false;
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0;
#endif
}

// ==========================================
// 业务逻辑实现 (Business Logic)
// ==========================================

template <typename Conf>
class SocketTcpConnection : public Conf::UserData {
   public:
    ~SocketTcpConnection() { close("destruct"); }

    const char* getLastError() { return last_error_; }

    bool isConnected() { return fd_ != INVALID_SOCKET_FD; }

    bool getPeername(struct sockaddr_in& addr) {
        socklen_t addr_len = sizeof(addr);
        return ::getpeername(fd_, (struct sockaddr*)&addr, &addr_len) == 0;
    }

    void close(const char* reason, bool check_errno = false) {
        if (fd_ != INVALID_SOCKET_FD) {
            saveError(reason, check_errno);
            close_socket(fd_);
            fd_ = INVALID_SOCKET_FD;
        }
    }

    int writeSome(const void* data, uint32_t size, bool more = false) {
        int flags = 0;
#ifndef _WIN32
        flags |= MSG_NOSIGNAL;
        if (more) flags |= MSG_MORE;
#endif
        // Windows 的 send 需要 const char*
        int ret = ::send(fd_, reinterpret_cast<const char*>(data), size, flags);
        if (ret < 0) {
            if (is_would_block(get_last_error()))
                ret = 0;
            else
                close("send error", true);
        }
        if (Conf::SendTimeoutSec) send_ts_ = time(0);
        return ret;
    }

    bool write(const void* data_, uint32_t size, bool more = false) {
        const uint8_t* data = static_cast<const uint8_t*>(data_);
        do {
            int sent = writeSome(data, size, more);
            if (sent < 0) return false;
            data += sent;
            size -= sent;
        } while (size != 0);
        return true;
    }

    bool writeNonblock(const void* data, uint32_t size, bool more = false) {
        if (writeSome(data, size, more) != static_cast<int>(size)) {
            close("send error", true);
            return false;
        }
        return true;
    }

   protected:
    template <typename ServerConf>
    friend class SocketTcpServer;

    template <typename Handler>
    void pollConn(int64_t now, Handler& handler) {
        if (Conf::SendTimeoutSec && now >= send_ts_ + Conf::SendTimeoutSec) {
            handler.onSendTimeout(*this);
            send_ts_ = now;
        }
        bool got_data = read(
            [&](const uint8_t* data, uint32_t size) { return handler.onTcpData(*this, data, size); });
        if (Conf::RecvTimeoutSec) {
            if (!got_data && now >= expire_ts_) {
                handler.onRecvTimeout(*this);
                got_data = true;
            }
            if (got_data) expire_ts_ = now + Conf::RecvTimeoutSec;
        }
    }

    template <typename Handler>
    bool read(Handler handler) {
        // 使用 recv 替代 read，支持跨平台
        int ret = ::recv(fd_, reinterpret_cast<char*>(recvbuf_ + tail_), Conf::RecvBufSize - tail_, 0);
        if (ret <= 0) {
            if (ret < 0 && is_would_block(get_last_error())) return false;
            if (ret < 0)
                close("read error", true);
            else
                close("remote close");
            return false;
        }
        tail_ += ret;

        uint32_t remaining = handler(recvbuf_ + head_, tail_ - head_);
        if (remaining == 0) {
            head_ = tail_ = 0;
        } else {
            head_ = tail_ - remaining;
            if (head_ >= Conf::RecvBufSize / 2) {
                memmove(recvbuf_, recvbuf_ + head_, remaining);  // C++ 中 memmove 比 memcpy 安全
                head_ = 0;
                tail_ = remaining;
            } else if (tail_ == Conf::RecvBufSize) {
                close("recv buf full");
            }
        }
        return true;
    }

    bool open(int64_t now, socket_t fd) {
        fd_ = fd;
        head_ = tail_ = 0;
        send_ts_ = now;
        expire_ts_ = now + Conf::RecvTimeoutSec;

        if (!set_nonblocking(fd_)) {
            close("fcntl/ioctlsocket O_NONBLOCK error", true);
            return false;
        }

        int yes = 1;
        if (setsockopt(fd_, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<sockopt_val_t>(&yes), sizeof(yes)) < 0) {
            close("setsockopt TCP_NODELAY error", true);
            return false;
        }

        return true;
    }

    void saveError(const char* msg, bool check_errno) {
        if (check_errno) {
            int err = get_last_error();
#ifdef _WIN32
            snprintf(last_error_, sizeof(last_error_), "%s (WSA_ERR:%d)", msg, err);
#else
            snprintf(last_error_, sizeof(last_error_), "%s %s", msg, strerror(err));
#endif
        } else {
            snprintf(last_error_, sizeof(last_error_), "%s", msg);
        }
    }

    socket_t fd_ = INVALID_SOCKET_FD;
    int64_t send_ts_ = 0;
    int64_t expire_ts_ = 0;
    uint32_t head_ = 0;
    uint32_t tail_ = 0;
    uint8_t recvbuf_[Conf::RecvBufSize];
    char last_error_[64] = "";
};

template <typename Conf>
class SocketTcpClient : public SocketTcpConnection<Conf> {
   public:
    using Conn = SocketTcpConnection<Conf>;

    bool init(const char* interface_ip, const char* server_ip, uint16_t server_port, uint16_t local_port = 0) {
        ensure_network_init();

        server_addr_.sin_family = AF_INET;
        inet_pton(AF_INET, server_ip, &(server_addr_.sin_addr));
        server_addr_.sin_port = htons(server_port);
        memset(&(server_addr_.sin_zero), 0, 8);  // bzero 非标准，改用 memset
        local_port_be_ = htons(local_port);
        return true;
    }

    void allowReconnect() { next_conn_ts_ = 0; }

    template <typename Handler>
    void poll(Handler& handler) {
        int64_t now = time(0);
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
    int connect(int64_t now) {
        if (conn_fd_ == INVALID_SOCKET_FD) {
            if (now < next_conn_ts_) return 0;
            if (Conf::ConnRetrySec)
                next_conn_ts_ = now + Conf::ConnRetrySec;
            else
                next_conn_ts_ = std::numeric_limits<int64_t>::max();

            socket_t fd = socket(AF_INET, SOCK_STREAM, 0);
            if (fd == INVALID_SOCKET_FD) {
                Conn::saveError("socket error", true);
                return -1;
            }
            if (local_port_be_) {
                struct sockaddr_in local_addr;
                local_addr.sin_family = AF_INET;
                local_addr.sin_addr.s_addr = INADDR_ANY;
                local_addr.sin_port = local_port_be_;
                if (::bind(fd, (struct sockaddr*)&local_addr, sizeof(local_addr)) < 0) {
                    Conn::saveError("bind error", true);
                    close_socket(fd);
                    return -1;
                }
            }

            if (!set_nonblocking(fd)) {
                Conn::saveError("set nonblock error", true);
                close_socket(fd);
                return -1;
            }
            conn_fd_ = fd;

            if (Conf::ConnTimeoutSec)
                conn_expire_ts_ = now + Conf::ConnTimeoutSec;
            else
                conn_expire_ts_ = std::numeric_limits<int64_t>::max();
        }

        int ret = ::connect(conn_fd_, (struct sockaddr*)&server_addr_, sizeof(server_addr_));
        int err = get_last_error();

        if (ret == 0 || is_isconn(err)) {
            if (!Conn::open(now, conn_fd_)) {
                conn_fd_ = INVALID_SOCKET_FD;
                return -1;
            }
            conn_fd_ = INVALID_SOCKET_FD;
            return 1;
        }

        if (is_in_progress(err) && now < conn_expire_ts_) {
            return 0;
        }

        if (now < conn_expire_ts_)
            Conn::saveError("connect error", true);
        else
            Conn::saveError("connect expired", false);

        close_socket(conn_fd_);
        conn_fd_ = INVALID_SOCKET_FD;
        return -1;
    }

    bool report_disconnect_ = false;
    socket_t conn_fd_ = INVALID_SOCKET_FD;
    int64_t next_conn_ts_ = 0;
    int64_t conn_expire_ts_ = 0;
    struct sockaddr_in server_addr_;
    uint16_t local_port_be_;
};

template <typename Conf>
class SocketTcpServer {
   public:
    using Conn = SocketTcpConnection<Conf>;

    bool init(const char* interface_ip, const char* server_ip, uint16_t server_port) {
        ensure_network_init();

        for (uint32_t i = 0; i < Conf::MaxConns; i++) conns_[i] = conns_data_ + i;
        listenfd_ = socket(AF_INET, SOCK_STREAM, 0);
        if (listenfd_ == INVALID_SOCKET_FD) {
            saveError("socket error");
            return false;
        }

        if (!set_nonblocking(listenfd_)) {
            close("set nonblock error");
            return false;
        }

        int yes = 1;
        if (setsockopt(listenfd_, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<sockopt_val_t>(&yes), sizeof(yes)) < 0) {
            close("setsockopt SO_REUSEADDR error");
            return false;
        }

        struct sockaddr_in local_addr;
        local_addr.sin_family = AF_INET;
        inet_pton(AF_INET, server_ip, &(local_addr.sin_addr));
        local_addr.sin_port = htons(server_port);
        memset(&(local_addr.sin_zero), 0, 8);

        if (::bind(listenfd_, (struct sockaddr*)&local_addr, sizeof(local_addr)) < 0) {
            close("bind error");
            return false;
        }
        if (listen(listenfd_, 5) < 0) {
            close("listen error");
            return false;
        }

        return true;
    }

    void close(const char* reason) {
        if (listenfd_ != INVALID_SOCKET_FD) {
            saveError(reason);
            close_socket(listenfd_);
            listenfd_ = INVALID_SOCKET_FD;
        }
    }

    const char* getLastError() { return last_error_; }

    ~SocketTcpServer() { close("destruct"); }

    bool isClosed() { return listenfd_ == INVALID_SOCKET_FD; }

    uint32_t getConnCnt() { return conns_cnt_; }

    template <typename Handler>
    void foreachConn(Handler handler) {
        for (uint32_t i = 0; i < conns_cnt_; i++) {
            Conn& conn = *conns_[i];
            handler(conn);
        }
    }

    template <typename Handler>
    void poll(Handler& handler) {
        int64_t now = time(0);
        if (conns_cnt_ < Conf::MaxConns) {
            Conn& conn = *conns_[conns_cnt_];
            struct sockaddr_in clientaddr;
            socklen_t addr_len = sizeof(clientaddr);
            socket_t fd = ::accept(listenfd_, (struct sockaddr*)&(clientaddr), &addr_len);
            if (fd != INVALID_SOCKET_FD && conn.open(now, fd)) {
                conns_cnt_++;
                handler.onTcpConnected(conn);
            }
        }
        for (uint32_t i = 0; i < conns_cnt_;) {
            Conn& conn = *conns_[i];
            conn.pollConn(now, handler);
            if (conn.isConnected())
                i++;
            else {
                std::swap(conns_[i], conns_[--conns_cnt_]);
                handler.onTcpDisconnect(conn);
            }
        }
    }

   private:
    void saveError(const char* msg) {
        int err = get_last_error();
#ifdef _WIN32
        snprintf(last_error_, sizeof(last_error_), "%s (WSA_ERR:%d)", msg, err);
#else
        snprintf(last_error_, sizeof(last_error_), "%s %s", msg, strerror(err));
#endif
    }

    socket_t listenfd_ = INVALID_SOCKET_FD;
    uint32_t conns_cnt_ = 0;
    Conn* conns_[Conf::MaxConns];
    Conn conns_data_[Conf::MaxConns];
    char last_error_[64] = "";
};