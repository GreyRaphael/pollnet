#include <algorithm>
#include <atomic>
#include <cctype>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <format>
#include <iostream>
#include <iterator>
#include <string>
#include <string_view>

#include "pollnet/socket.h"

namespace {
std::atomic<bool> g_running{true};
void handle_signal(int) { g_running.store(false, std::memory_order_relaxed); }

std::string addr_to_string(const sockaddr_in& addr) {
    char buf[INET_ADDRSTRLEN]{};
    if (::inet_ntop(AF_INET, &addr.sin_addr, buf, sizeof(buf)) == nullptr) {
        return "<invalid>";
    }
    return buf;
}
}  // namespace

struct ServerConf {
    static const uint32_t RecvBufSize = 4096;
    static const uint32_t MaxConns = 10;
    static const uint32_t SendTimeoutSec = 0;
    static const uint32_t RecvTimeoutSec = 10;
    struct UserData {
        struct sockaddr_in addr {};
    };
};

using TcpServer = pollnet::SocketTcpServer<ServerConf>;

struct MsgHeader {
    uint32_t body_len;
};

std::string upper_and_double(std::string_view sv) {
    std::string out;
    out.reserve(sv.size() * 2);

    std::ranges::transform(sv, std::back_inserter(out), [](unsigned char c) {
        return static_cast<char>(std::toupper(c));
    });

    out.append(out);
    return out;
}

class MyServer : public TcpServer {
   public:
    MyServer() = default;
    ~MyServer() = default;

    void onTcpConnected(TcpServer::Conn& conn) {
        if (!conn.getPeername(conn.addr)) {
            conn.addr = {};
        }
        std::cout << std::format("new connection from {}:{}, total={}\n",
                                 addr_to_string(conn.addr), ntohs(conn.addr.sin_port), getConnCnt());
    }

    void onSendTimeout([[maybe_unused]] TcpServer::Conn& conn) {
        std::cout << "onSendTimeout should not be called as SendTimeoutSec=0\n";
        std::exit(1);
    }

    uint32_t onTcpData(TcpServer::Conn& conn, const uint8_t* data, uint32_t size) {
        while (size >= sizeof(MsgHeader)) {
            MsgHeader req_header{};
            std::memcpy(&req_header, data, sizeof(MsgHeader));
            uint32_t req_body_len = ntohl(req_header.body_len);
            if (req_body_len > ServerConf::RecvBufSize - sizeof(MsgHeader)) {
                conn.close("message too large");
                return 0;
            }

            if (size - sizeof(MsgHeader) < req_body_len) {
                break;
            }

            std::string_view req_body{
                reinterpret_cast<const char*>(data + sizeof(MsgHeader)),
                req_body_len};

            std::cout << std::format("Recv Body [len: {}]: {}\n", req_body_len, req_body);
            auto result = upper_and_double(req_body);

            MsgHeader rsp_header{};
            rsp_header.body_len = htonl(static_cast<uint32_t>(result.size()));

            std::string packet;
            packet.resize(sizeof(MsgHeader) + result.size());
            std::memcpy(packet.data(), &rsp_header, sizeof(MsgHeader));
            std::memcpy(packet.data() + sizeof(MsgHeader), result.data(), result.size());
            if (!conn.writeNonblock(packet.data(), static_cast<uint32_t>(packet.size()))) {
                return 0;
            }

            uint32_t total_len = static_cast<uint32_t>(sizeof(MsgHeader)) + req_body_len;
            data += total_len;
            size -= total_len;
        }
        return size;
    }

    void onRecvTimeout(TcpServer::Conn& conn) {
        std::cout << "onRecvTimeout\n";
        conn.close("timeout");
    }

    void onTcpDisconnect(TcpServer::Conn& conn) {
        std::cout << std::format("client disconnected: {}:{}, reason={}, total={}\n",
                                 addr_to_string(conn.addr), ntohs(conn.addr.sin_port),
                                 conn.getLastError(), getConnCnt());
    }
};

int main() {
    std::signal(SIGINT, handle_signal);

    MyServer server;
    if (!server.init("", "127.0.0.1", 1234)) {
        std::cout << std::format("init failed: {}\n", server.getLastError());
        return 1;
    }

    while (g_running.load(std::memory_order_relaxed)) {
        server.poll(server);
    }

    return 0;
}
