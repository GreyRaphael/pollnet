# pollnet

基于 [MengRao/pollnet](https://github.com/MengRao/pollnet) 重构的跨平台（Linux + Windows）、零堆分配、极简模块化 C++20 高性能网络库。

## 特性

- **精简三层模块化设计（Linux + Windows 全平台一致）**：
  - [core.h](include/pollnet/core.h)：后端无关核心（`Endpoint`、`ByteRange`/`IoSlice`、`Options`、`Concepts`、零开销可选回调分发）。
  - [socket.h](include/pollnet/socket.h)：标准轮询后端（`TcpClient` / `TcpServer` / 统一 `UdpSocket`；支持 `accept` 节流、向量写 `IoSlice` 批写、Linux `sendmmsg`/`recvmmsg` UDP 批量收发与低延迟内核调优参数）。
  - [eventloop.h](include/pollnet/eventloop.h)：跨平台事件循环后端 `EventLoopTcpServer`（Linux `epoll` / Windows `WSAPoll` / 通用 POSIX `poll`），在多连接场景实现 $O(\text{active})$ 派发。
- **现代 C++20 API**：指定初始化器 `Options` 结构体、`std::chrono::milliseconds` 亚秒级超时、`std::span`/`ByteRange` 零拷贝视图与编译期 `if constexpr` 可选回调（仅 `onData` 必选）。
- **零堆分配与资源安全**：连接池与接收缓冲区全部预分配，所有 Socket 类遵循 Rule of Five 禁止误拷贝/移动。

## 快速示例

```cpp
#include <iostream>
#include <span>
#include "pollnet/socket.h"

struct EchoHandler {
    size_t onData(pollnet::TcpConnection<>& conn, std::span<const uint8_t> data) {
        (void)conn.writeNonblock(data);
        return data.size();  // 返回已消费字节数
    }
};

int main() {
    using namespace std::chrono_literals;
    pollnet::TcpServer<> server;
    if (!server.listen({.host = "127.0.0.1", .port = 1234, .recv_timeout = 10s})) {
        std::cerr << server.lastError() << '\n';
        return 1;
    }
    EchoHandler handler;
    while (true) server.poll(handler);
}
```

## 目录结构

```text
.
├── include/pollnet/
│   ├── core.h                     # 后端无关核心：Endpoint、IoSlice、Options、Concepts
│   ├── socket.h                   # Linux + Windows 轮询后端（TCP + 统一 UdpSocket）
│   └── eventloop.h                # 跨平台 EventLoop 后端（Linux epoll / Windows WSAPoll）
├── examples/                      # TCP / UDP 示例程序
├── tests/test_pollnet.cpp         # 自动化单元测试
├── benchmarks/
│   ├── bench_common.h             # Benchmark 公共计时、绑核与统计工具
│   └── bench.cpp                  # 性能基准程序 (CMake target: pollnet_bench)
├── .github/workflows/ci.yml       # Linux (GCC/Clang) & Windows (MSVC/MinGW) CI
├── .github/workflows/release.yml
└── CMakeLists.txt
```

## 构建与测试

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build -j
ctest --test-dir build --output-on-failure
```

Windows (MSVC):

```powershell
cmake -S . -B build
cmake --build build --config Debug -j
ctest --test-dir build -C Debug --output-on-failure
```

## 性能基准（支持跨平台 CPU 绑核）

```bash
cmake -S . -B build-rel -DCMAKE_BUILD_TYPE=Release
cmake --build build-rel --target pollnet_bench -j
./build-rel/pollnet_bench --warmup 5000 --iters 50000 --throughput-iters 200000 --payload 64 --server-cpu 2 --client-cpu 4
```

## todo

- [x] Add udp examples
- [x] C++20 & cross-platform hardening + unit tests
- [x] Modular C++20 v2.0 API refactoring (`core.h` + `socket.h`)
- [x] Performance benchmark (`pollnet_bench`)
- [x] Eventloop version (`include/pollnet/eventloop.h`)
