#include <format>
#include <print>
#include <string>

#include "socket.h"

struct ClientConf {
    static const uint32_t RecvBufSize = 4096;
    static const uint32_t ConnRetrySec = 3;
    static const uint32_t ConnTimeoutSec = 30;
    static const uint32_t SendTimeoutSec = 0;
    static const uint32_t RecvTimeoutSec = 0;
    struct UserData {
    };
};

using TcpClient = SocketTcpClient<ClientConf>;

struct MsgHeader {
    uint32_t body_len;
};

class MyClient : public TcpClient {
   private:
    bool is_first = true;

   public:
    MyClient(/* args */) {}
    ~MyClient() = default;

    void onTcpConnectFailed() { std::println("connect error:{}", this->getLastError()); }
    void onTcpConnected(TcpClient::Conn &conn) {
        std::println("connected! first={}", is_first);
        if (is_first) {
            MsgHeader header;
            std::string msg{"hello"};
            header.body_len = msg.size();
            this->write(&header, sizeof(MsgHeader));
            this->write(msg.data(), header.body_len);
            this->write(&header, sizeof(MsgHeader));
            this->write(msg.data(), header.body_len);
        }
        is_first = false;
    }
    void onTcpDisconnect(TcpClient::Conn &conn) {
        std::println("disconnected!");
    }
    void onSendTimeout(TcpClient::Conn &conn) { std::println("send timeout"); }
    void onRecvTimeout(TcpClient::Conn &conn) {
        std::println("recv timeout");
        conn.close("onRecvTimeout");
    }
    uint32_t onTcpData(TcpClient::Conn &conn, const uint8_t *data, uint32_t size) {
        while (size >= sizeof(MsgHeader)) {
            // 1. 解析 Header
            const auto *header = reinterpret_cast<const MsgHeader *>(data);
            uint32_t body_len = header->body_len;
            uint32_t total_len = sizeof(MsgHeader) + body_len;

            // 2. 检查缓冲区是否有完整的 Body
            if (size < total_len) {
                // 数据不足一个完整包，跳出循环，返回剩余字节数
                // pollnet 会保留这些字节并在下次数据到达时拼接到 data 开头
                break;
            }

            // 3. 处理 Body (使用 string_view 避免拷贝)
            std::string_view body{
                reinterpret_cast<const char *>(data) + sizeof(MsgHeader),
                body_len};

            std::println("Recv Body [len: {}]: {}", body_len, body);

            // 4. 移动指针，准备处理下一个包
            data += total_len;
            size -= total_len;
        }
        // 返回值告知 pollnet 缓冲区还剩多少字节没处理（即半包部分）
        return size;
    }
};

int main(int argc, char const *argv[]) {
    MyClient client;
    if (!client.init("", "127.0.0.1", 1234)) {
        std::println("init error:{}", client.getLastError());
        exit(1);
    }
    while (true) {
        client.poll(client);
    }
    return 0;
}