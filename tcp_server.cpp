#include <atomic>
#include <csignal>
#include <cstdint>
#include <print>
#include <string_view>

#include "socket.h"

struct ServerConf {
    static const uint32_t RecvBufSize = 4096;
    static const uint32_t MaxConns = 10;
    static const uint32_t SendTimeoutSec = 0;
    static const uint32_t RecvTimeoutSec = 10;
    struct UserData {
        struct sockaddr_in addr;
    };
};

using TcpServer = SocketTcpServer<ServerConf>;

// 使用 atomic 确保信号处理函数与主循环之间的内存可见性
std::atomic<bool> running{true};

// 信号处理回调
void signal_handler(int sig) {
    if (sig == SIGINT || sig == SIGTERM) {
        running = false;
    }
}

TcpServer server;

std::string to_upper(std::string_view sv) {
    std::string out;
    out.reserve(sv.size());

    // 使用 C++20 ranges 将转换后的字符直接放入 string
    std::ranges::transform(sv, std::back_inserter(out), [](unsigned char c) {
        return std::toupper(c);
    });

    return out;
}

int main(int argc, char **argv) {
    std::signal(SIGINT, signal_handler);
    std::signal(SIGTERM, signal_handler);

    if (!server.init("", "127.0.0.1", 1234)) {
        std::println("init failed: {}", server.getLastError());
        return 1;
    }

    while (running) {
        struct
        {
            void onTcpConnected(TcpServer::Conn &conn) {
                conn.getPeername(conn.addr);
                std::println("new connection from {}:{}, total={}", inet_ntoa(conn.addr.sin_addr), ntohs(conn.addr.sin_port), server.getConnCnt());
                /*
                server.foreachConn([&](TcpServer::Conn& conn) {
                  cout << "current connection from: " << inet_ntoa(conn.addr.sin_addr) << ":" << ntohs(conn.addr.sin_port)
                       << endl;
                });
                */
            }
            void onSendTimeout(TcpServer::Conn &conn) {
                std::println("onSendTimeout should not be called as SendTimeoutSec=0");
                exit(1);
            }
            uint32_t onTcpData(TcpServer::Conn &conn, const uint8_t *data, uint32_t size) {
                std::string_view sv{reinterpret_cast<const char *>(data), size};
                std::println("Received {} bytes from server: {}", size, sv);
                auto result = to_upper(sv);

                conn.write(result.data(), result.size());
                return 0;
            }
            void onRecvTimeout(TcpServer::Conn &conn) {
                std::println("onRecvTimeout");
                conn.close("timeout");
            }
            void onTcpDisconnect(TcpServer::Conn &conn) {
                std::println("clientdisconnected: {}:{}, reason={}, total={}", inet_ntoa(conn.addr.sin_addr), ntohs(conn.addr.sin_port), conn.getLastError(), server.getConnCnt());
            }
        } handler;
        server.poll(handler);
    }

    return 0;
}
