#include <atomic>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <format>
#include <iostream>
#include <string>
#include <string_view>

#include "pollnet/socket.h"

namespace {
std::atomic<bool> g_running{true};
void handle_signal(int) { g_running.store(false, std::memory_order_relaxed); }
}  // namespace

struct ClientConf {
    static const uint32_t RecvBufSize = 4096;
    static const uint32_t ConnRetrySec = 3;
    static const uint32_t ConnTimeoutSec = 30;
    static const uint32_t SendTimeoutSec = 0;
    static const uint32_t RecvTimeoutSec = 0;
    struct UserData {};
};

using TcpClient = pollnet::SocketTcpClient<ClientConf>;

struct MsgHeader {
    uint32_t body_len;
};

class MyClient : public TcpClient {
   private:
    bool is_first = true;

   public:
    MyClient() = default;
    ~MyClient() = default;

    void onTcpConnectFailed() {
        std::cout << std::format("connect error: {}\n", this->getLastError());
    }

    void onTcpConnected([[maybe_unused]] TcpClient::Conn& conn) {
        std::cout << std::format("connected! first={}\n", is_first);
        if (is_first) {
            std::string msg{"hello"};
            MsgHeader header{};
            header.body_len = htonl(static_cast<uint32_t>(msg.size()));

            std::string packet;
            packet.resize(sizeof(MsgHeader) + msg.size());
            std::memcpy(packet.data(), &header, sizeof(MsgHeader));
            std::memcpy(packet.data() + sizeof(MsgHeader), msg.data(), msg.size());

            if (!this->write(packet.data(), static_cast<uint32_t>(packet.size())) ||
                !this->write(packet.data(), static_cast<uint32_t>(packet.size()))) {
                std::cout << std::format("write error: {}\n", this->getLastError());
            }
        }
        is_first = false;
    }

    void onTcpDisconnect([[maybe_unused]] TcpClient::Conn& conn) {
        std::cout << "disconnected!\n";
    }

    void onSendTimeout([[maybe_unused]] TcpClient::Conn& conn) {
        std::cout << "send timeout\n";
    }

    void onRecvTimeout(TcpClient::Conn& conn) {
        std::cout << "recv timeout\n";
        conn.close("onRecvTimeout");
    }

    uint32_t onTcpData(TcpClient::Conn& conn, const uint8_t* data, uint32_t size) {
        while (size >= sizeof(MsgHeader)) {
            MsgHeader header{};
            std::memcpy(&header, data, sizeof(MsgHeader));
            uint32_t body_len = ntohl(header.body_len);
            if (body_len > ClientConf::RecvBufSize - sizeof(MsgHeader)) {
                conn.close("message too large");
                return 0;
            }

            if (size - sizeof(MsgHeader) < body_len) {
                break;
            }

            std::string_view body{
                reinterpret_cast<const char*>(data + sizeof(MsgHeader)),
                body_len};

            std::cout << std::format("Recv Body [len: {}]: {}\n", body_len, body);

            uint32_t total_len = static_cast<uint32_t>(sizeof(MsgHeader)) + body_len;
            data += total_len;
            size -= total_len;
        }
        return size;
    }
};

int main() {
    std::signal(SIGINT, handle_signal);

    MyClient client;
    if (!client.init("", "127.0.0.1", 1234)) {
        std::cout << std::format("init error: {}\n", client.getLastError());
        return 1;
    }

    while (g_running.load(std::memory_order_relaxed)) {
        client.poll(client);
    }
    return 0;
}
