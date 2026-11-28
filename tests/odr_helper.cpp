#include <cstdint>
#include <span>

#include "pollnet/eventloop.h"
#include "pollnet/socket.h"

namespace {

struct OdrTick {
    uint32_t seq;
    uint64_t ts;
};

struct OdrHandler {
    size_t onData(auto&, std::span<const uint8_t> data) { return data.size(); }
};

struct OdrFramingHandler {
    void onMessage(pollnet::TcpConnection<>&, const OdrTick&) {}
};

}  // namespace

// 在独立翻译单元中实例化 pollnet 全核心模板与内联符号，用于多 TU 链接 ODR 校验
bool odr_helper_smoke() {
    auto ep = pollnet::Endpoint::from("127.0.0.1", 12345);
    if (!ep.has_value() || ep->port() != 12345) return false;

    pollnet::TcpClient<> client;
    pollnet::TcpServer<pollnet::EmptyUserData, 4096, 4> server;
    pollnet::EventLoopTcpServer<pollnet::EmptyUserData, 4096, 4, 1024> ev_server;
    pollnet::UdpSocket<1500> udp;

    OdrHandler h;
    OdrFramingHandler fh;
    auto framed = pollnet::makeFixedFrameHandler<OdrTick>(fh);
    client.poll(h);
    server.poll(framed);
    ev_server.poll(h, 0);
    (void)udp.isClosed();
    return client.freeSendBytes() == 0;
}
