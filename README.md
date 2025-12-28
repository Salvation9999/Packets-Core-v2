# Packets-Core-v2

> Windows DLL：注入到目标进程后 Hook `ws2_32.dll` / `wsock32.dll` 的 `send/recv`，并通过命名管道把抓到的 TCP 数据包流式广播给外部（通常是 Python）。同时提供通过命名管道触发的“原套接字 send()”以及“读取目标进程内存(32-bit address)”能力。

## 目录

- [项目提供什么](#项目提供什么)
- [整体架构](#整体架构)
- [快速开始](#快速开始)
  - [1) 编译 DLL](#1-编译-dll)
  - [2) 注入 DLL](#2-注入-dll)
  - [3) 用 Python 验证抓包](#3-用-python-验证抓包)
- [IPC：命名管道一览](#ipc命名管道一览)
- [协议概览（v2）](#协议概览v2)
  - [PKT2：抓包广播（DLL -> Client）](#pkt2抓包广播dll---client)
  - [CMD2：发送命令（Client -> DLL）](#cmd2发送命令client---dll)
  - [RSP2：内存读响应广播（DLL -> Client）](#rsp2内存读响应广播dll---client)
- [运行/稳定性注意事项](#运行稳定性注意事项)
- [FAQ / 排错](#faq--排错)

---

## 项目提供什么

1. **抓包**：在目标进程内拦截所有 `send()` 与 `recv()` 的 payload（不做协议解析，仅透传字节）。
2. **广播输出**：抓到的包以二进制帧（PKT2 v2）写入命名管道 `\\.\pipe\PacketsCorePipe`，支持多个客户端同时连接（每个客户端有独立缓冲队列）。
3. **外部触发 send()**：外部进程可通过命名管道 `\\.\pipe\PacketsCoreSendPipe` 发送 CMD2 命令，让 DLL 在目标进程里对“指定 socket 值”调用 `send()`。
4. **读取目标进程内存（x86 地址）**：外部进程可通过 `\\.\pipe\PacketsCoreMemPipe` 发送 READ_MEM 命令；DLL 在目标进程内安全读取内存后，通过 `\\.\pipe\PacketsCoreRspPipe` 以 RSP2 广播回读结果。

> 重要：当前实现没有“状态查询 API”。外部只能假设 DLL 注入成功后其后台线程已经跑起来。

## 整体架构

- `Dllmain.cpp`
  - `DLL_PROCESS_ATTACH` 时创建后台线程 `CoreWorker`，在线程内调用 `CoreStart()`（避免在 Loader Lock 中做复杂逻辑）。
  - `DLL_PROCESS_DETACH` **不会**调用 `CoreStop()`；仅设置 `g_stop = true` 并唤醒等待线程。

- `CoreEngine.cpp`
  - `CoreStart()` 会依次启动：
    1) 抓包管道 sender（PKT2，`PacketsCorePipe`）
    2) 发送命令 receiver（CMD2 TCP_SEND，`PacketsCoreSendPipe`）
    3) 内存命令 receiver（CMD2 READ_MEM，`PacketsCoreMemPipe`）
    4) 响应管道 sender（RSP2，`PacketsCoreRspPipe`）
    5) 网络 Hook（MinHook，`send/recv`）以及 watchdog

- `NetworkHook.cpp`
  - Hook `ws2_32.dll` 与 `wsock32.dll` 的 `send/recv`。
  - 将拦截到的数据 push 到全局 `PacketQueue`。
  - watchdog 每 50ms 检查函数 prologue 是否仍为 `JMP rel32 (0xE9)`，如被还原则尝试重新 enable hook。

- `PacketCaptureSender.cpp`
  - 多 accept 线程（4）接受多个客户端连接。
  - dispatch 线程从 `PacketQueue` pop 数据并广播到所有 client（每个 client 有 writer 线程 + 4MB 队列，队列超限会丢旧帧）。

- `CommandPipeReceiver.cpp` / `MemoryCommandReceiver.cpp`
  - 多 accept 线程（4）+ 每连接一个 client 线程读取 CMD2 帧。
  - `TCP_SEND`：直接对指定 sock 调用 `send(sock, payload, len, flags)`。
  - `READ_MEM`：调用 `ReadMemorySafe()` 读内存后 push 到 `RspQueue`。

- `ResponsePipeSender.cpp`
  - 与抓包 sender 类似的多客户端广播机制，广播 RSP2（读内存响应）。

## 快速开始

### 1) 编译 DLL

- 打开 `Packets-Core-v2/Packets-Core-v2.sln`
- 选择 `Release|x86` 编译。
  - Hook 部分支持 x86/x64。
  - **内存读取模块使用 `uint32 address`**（见 `MemoryReaderX86.*`），因此 READ_MEM 逻辑更贴近“32-bit 目标进程”。

### 2) 注入 DLL

请暂时使用processHacker或任意注入器。

### 3) 用 Python 验证抓包

见：`Packets-Core-v2/Python Script/packet_capture_test.py`

1. 先注入 DLL 到目标进程
2. 在你的 Python 环境里安装 pywin32
3. 运行脚本，它会连接 `\\.\pipe\PacketsCorePipe` 并持续打印抓到的包。

更完整的交互示例：`packet_console.py`（边抓包边向最后一个 sock 发送 hex payload）。

## IPC：命名管道一览

| 方向 | 管道名 | 用途 | 多客户端 | 备注 |
|---|---|---|---|---|
| DLL -> Client | `\\.\pipe\PacketsCorePipe` | PKT2 抓包广播 | 是 | 每 client 4MB 队列，慢客户端会丢旧帧 |
| Client -> DLL | `\\.\pipe\PacketsCoreSendPipe` | CMD2：TCP_SEND | 是 | 每连接一个读线程 |
| Client -> DLL | `\\.\pipe\PacketsCoreMemPipe` | CMD2：READ_MEM | 是 | 读取结果发往 RSP2 |
| DLL -> Client | `\\.\pipe\PacketsCoreRspPipe` | RSP2：READ_MEM 响应广播 | 是 | 同样 4MB 队列策略 |

## 协议概览（v2）

所有多字节整数均为 **little-endian**（因为 C++ 直接写 packed struct；Python 示例均使用 `<`）。

### PKT2：抓包广播（DLL -> Client）

- magic: ASCII `'PKT2'`，数值 `0x32544B50`
- ver: `2`
- header 固定 32 字节，然后紧跟 payload

C 结构（`#pragma pack(push,1)`）：

```c
struct FrameHeaderV2 {
  uint32_t magic;       // 'PKT2'
  uint16_t ver;         // 2
  uint8_t  type;        // 1=SEND, 2=RECV
  uint8_t  af;          // 4=IPv4
  uint32_t payload_len; // bytes
  uint64_t sock;        // SOCKET 值（直接透传）
  uint32_t local_ip;    // IPv4，network order（与 inet_ntoa/pack("!I") 对应）
  uint16_t local_port;  // host order（代码里使用 ntohs）
  uint32_t remote_ip;   // network order
  uint16_t remote_port; // host order
};
```

Python 参考（仓库示例）：`packet_capture_test.py` / `packet_console.py`

```py
CAP_HDR_FMT = "<4sHBBIQIHIH"  # 32 bytes
```

### CMD2：发送命令（Client -> DLL）

共用 header（24 字节）：

```c
struct CmdHeaderV2 {
  uint32_t magic;       // 'CMD2' = 0x32444D43
  uint16_t ver;         // 2
  uint8_t  cmd;         // 1=TCP_SEND, 2=READ_MEM
  uint8_t  reserved0;   // 0
  uint32_t flags;       // send() flags (仅 TCP_SEND 有意义)
  uint64_t sock;        // SOCKET 值（仅 TCP_SEND 有意义；READ_MEM 时通常为 0）
  uint32_t payload_len; // payload bytes
};
```

- `cmd=1 (TCP_SEND)`：payload 就是要发出的原始 bytes。
- `cmd=2 (READ_MEM)`：payload 为固定 12 字节：

```c
struct ReadMemPayload {
  uint32_t request_id;
  uint32_t address;  // uint32
  uint8_t  type;     // 1..5
  uint16_t size;     // 对 BYTES/STR* 类型有效 (1..1024)
  uint8_t  reserved;
};
```

Python 参考：`tcp_packet_sender.py`、`read_memory.py`

### RSP2：内存读响应广播（DLL -> Client）

header 固定 12 字节：

```c
struct RspFrameHeaderV2 {
  uint32_t magic;       // 'RSP2' = 0x32505352
  uint16_t ver;         // 2
  uint8_t  rsp_type;    // 1=READ_MEM
  uint8_t  reserved0;   // 0
  uint32_t payload_len; // bytes
};
```

payload 固定头 16 字节 + data：

```c
struct RspPayloadV2 {
  uint32_t request_id;
  uint32_t status;   // 0=OK, 1=INVALID_ADDRESS, 2=INVALID_TYPE, 3=INVALID_SIZE, 4=STRING_TOO_LONG, 99=UNKNOWN_ERROR
  uint32_t address;
  uint8_t  type;     // 与请求一致
  uint16_t size;     // 与请求一致
  uint8_t  reserved1;
};
// 紧跟 item.data (bytes)
```

状态码与读取类型请见 `MemoryReaderX86.h`。

## 运行/稳定性注意事项

- **Loader Lock**：核心逻辑在后台线程启动，避免 DllMain 阻塞。
- **缺少状态查询/停止导出**：当前仅通过全局 `g_stop` 停止后台线程；没有库级 API 给外部调用。
- **watchdog 行为明显**：50ms 周期检查/重写 hook，可能带来明显特征与开销。
- **慢客户端丢包**：抓包与响应广播对每个 client 限制队列大小（4MB）；超限会丢弃旧帧。
- **READ_MEM 地址为 32-bit**：对 64-bit 目标进程读取 64-bit 指针链需要额外约束/适配（当前协议/实现未覆盖）。

## FAQ / 排错

1. **Python 连接管道失败（FileNotFoundError / ERROR_FILE_NOT_FOUND）**
   - DLL 还没注入或还没启动到 `CoreStart()`。
   - 目标进程权限不足：尝试以管理员运行注入器/脚本。

2. **抓包无数据**
   - 目标进程可能不是走 `send/recv`（例如使用 `WSASend/WSARecv` 或自定义 IO）。
   - 目标进程可能使用非 IPv4，当前 `FillSocketAddrsIPv4` 只填 IPv4。

3. **TCP_SEND 发送失败/无效果**
   - 你必须使用抓包流中出现的 `sock` 值（`SOCKET` 句柄的数值）。
   - 如果 socket 已关闭/被复用，发送可能落在错误连接上。

4. **READ_MEM 返回 INVALID_ADDRESS**
   - 地址不可读或越界。
   - 使用了错误的位宽（例如把 64-bit 指针截断为 uint32）。

---

## 相关文档

- `HowToPy(API).md`：面向 co-workers 的 Python 接入指南（推荐阅读）
- `Packets-Core-v2/Python Script/*`：示例脚本
