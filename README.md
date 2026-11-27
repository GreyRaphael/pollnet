# pollnet

基于 [MengRao/pollnet](https://github.com/MengRao/pollnet) 改进的跨平台（Linux + Windows）、零堆分配、单头文件 C++20 轮询式网络库。

## 特性

- **C++20 原生支持**：使用 `pollnet` 命名空间、`concepts` 编译期约束、`std::span` 视图接口与 `std::chrono::steady_clock` 单调时钟。
- **跨平台（Linux + Windows）**：统一封装 POSIX Socket 与 Winsock2（支持 MSVC 与 MinGW，自动管理 `WSAStartup`/`WSACleanup`、Windows 非阻塞 `connect` 状态机与组播绑定差异）。
- **零堆分配与资源安全**：连接池与接收缓冲区全部预分配，所有 Socket 类遵循 Rule of Five 禁止误拷贝/移动。

## 目录结构

```text
.
├── include/pollnet/socket.h   # 单头文件核心库
├── examples/                  # TCP / UDP 示例程序
├── tests/test_pollnet.cpp     # 自动化单元测试
├── .github/workflows/ci.yml   # Linux (GCC/Clang) & Windows (MSVC/MinGW) CI
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

## todo

- [x] Add udp examples
- [x] C++20 & cross-platform hardening + unit tests
- [ ] Performance benchmark
- [ ] Eventloop version
