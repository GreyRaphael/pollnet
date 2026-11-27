# pollnet

基于 [MengRao/pollnet](https://github.com/MengRao/pollnet) 重构的跨平台（Linux + Windows）、零堆分配、模块化 C++20 轮询式网络库。

## 特性

- **模块化分层设计**：核心契约与跨平台抽象位于 `core.h`，标准轮询 Socket 位于 `socket.h`，并预留 `eventloop.h` / `tcpdirect.h` / `efvi.h` 多后端扩展接口。
- **现代 C++20 API**：指定初始化器 `Options` 结构体、`std::chrono::milliseconds` 亚秒级超时、`Endpoint` 值类型、`std::span` 零拷贝视图与编译期 `if constexpr` 可选回调（仅 `onData` 必选）。
- **跨平台（Linux + Windows）**：统一封装 POSIX Socket 与 Winsock2（支持 MSVC 与 MinGW，自动管理 `WSAStartup`/`WSACleanup`、Windows 非阻塞 `connect` 状态机与组播绑定差异）。
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
│   ├── core.h                     # 后端无关核心：Endpoint、Options、Concepts 与可选回调分发
│   └── socket.h                   # Linux + Windows 标准非阻塞轮询 Socket 后端
├── examples/                      # TCP / UDP 示例程序
├── tests/test_pollnet.cpp         # 自动化单元测试
├── benchmarks/
│   ├── bench_common.h             # Benchmark 公共计时与统计工具
│   ├── bench_v1.cpp               # v1.1.0 基准程序
│   ├── bench_v2.cpp               # v2.0.0 基准程序 (CMake target: pollnet_bench)
│   └── compare_v1_v2.sh           # 一键同口径编译对比脚本
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

## 性能基准与对比 (`v1.1.0` vs `v2.0.0`)

单独运行当前版本 Benchmark（建议 Release 编译）：

```bash
cmake -S . -B build-rel -DCMAKE_BUILD_TYPE=Release
cmake --build build-rel --target pollnet_bench -j
./build-rel/pollnet_bench --warmup 2000 --iters 50000 --throughput-iters 200000 --payload 64
```

一键提取 `v1.1.0` 头文件并使用相同编译参数对比 `v1.1.0` 与 `v2.0.0`：

```bash
./benchmarks/compare_v1_v2.sh --warmup 2000 --iters 30000 --throughput-iters 200000 --payload 64
```

## todo

- [x] Add udp examples
- [x] C++20 & cross-platform hardening + unit tests
- [x] Modular C++20 v2.0 API refactoring (`core.h` + `socket.h`)
- [x] Performance benchmark (`v1.1.0` vs `v2.0.0`)
- [ ] Eventloop version (`include/pollnet/eventloop.h`)
