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

struct MsgHeader {
    uint32_t body_len;
};

std::string upper_and_double(std::string_view sv) {
    std::string out;
    out.reserve(sv.size() * 2);  // 一次分配到位

    // 第一次：转换并存入
    std::ranges::transform(sv, std::back_inserter(out), [](unsigned char c) {
        return static_cast<char>(std::toupper(c));
    });

    // 第二次：直接把前半部分拷贝到后半部分，无需再调用 toupper
    out.append(out);

    return out;
}

class MyServer : public TcpServer {
   private:
    /* data */
   public:
    MyServer(/* args */) {}
    ~MyServer() = default;

    void onTcpConnected(TcpServer::Conn& conn) {
        conn.getPeername(conn.addr);
        std::println("new connection from {}:{}, total={}", inet_ntoa(conn.addr.sin_addr), ntohs(conn.addr.sin_port), getConnCnt());
        /*
        server.foreachConn([&](TcpServer::Conn& conn) {
          cout << "current connection from: " << inet_ntoa(conn.addr.sin_addr) << ":" << ntohs(conn.addr.sin_port)
               << endl;
        });
        */
    }
    void onSendTimeout(TcpServer::Conn& conn) {
        std::println("onSendTimeout should not be called as SendTimeoutSec=0");
        exit(1);
    }
    uint32_t onTcpData(TcpServer::Conn& conn, const uint8_t* data, uint32_t size) {
        while (size >= sizeof(MsgHeader)) {
            // handle header
            const auto* req_header = reinterpret_cast<const MsgHeader*>(data);
            uint32_t req_body_len = req_header->body_len;
            uint32_t total_len = sizeof(MsgHeader) + req_body_len;

            if (size < total_len) {
                break;
            }

            // handle body
            std::string_view req_body{reinterpret_cast<const char*>(data) + sizeof(MsgHeader), req_body_len};

            std::println("Recv Body [len: {}]: {}", req_body_len, req_body);
            auto result = upper_and_double(req_body);
            MsgHeader rsp_header;
            rsp_header.body_len = result.size();
            conn.write(&rsp_header, sizeof(MsgHeader));
            conn.write(result.data(), result.size());

            data += total_len;
            size -= total_len;
        }
        return size;
    }
    void onRecvTimeout(TcpServer::Conn& conn) {
        std::println("onRecvTimeout");
        conn.close("timeout");
    }
    void onTcpDisconnect(TcpServer::Conn& conn) {
        std::println("clientdisconnected: {}:{}, reason={}, total={}", inet_ntoa(conn.addr.sin_addr), ntohs(conn.addr.sin_port), conn.getLastError(), getConnCnt());
    }
};

int main(int argc, char const* argv[]) {
    MyServer server;
    if (!server.init("", "127.0.0.1", 1234)) {
        std::println("init failed: {}", server.getLastError());
        return 1;
    }
    while (true) {
        server.poll(server);
    }

    return 0;
}