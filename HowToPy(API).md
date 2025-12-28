# HowToPy(API) — 在 Python 中正确依赖 Packets-Core-v2.dll

本文面向 co-workers：**你不需要理解 DLL 内部实现**，只需要按本文方式通过命名管道与 DLL 通信。

> 前提：DLL 已被注入目标进程并已运行（注入后会自动启动 core 线程）。

## 1. 你需要安装什么

- Windows
- Python 3.x
- 依赖：`pywin32`

```bash
pip install pywin32
```

> 本仓库示例脚本都基于 `win32file` 来读写 named pipe。

## 2. 你需要知道的 4 条管道

| 方向 | 管道 | 说明 |
|---|---|---|
| DLL -> Python | `\\.\pipe\PacketsCorePipe` | 抓包流（PKT2 v2 帧） |
| Python -> DLL | `\\.\pipe\PacketsCoreSendPipe` | TCP_SEND 命令（CMD2 v2） |
| Python -> DLL | `\\.\pipe\PacketsCoreMemPipe` | READ_MEM 命令（CMD2 v2 + payload） |
| DLL -> Python | `\\.\pipe\PacketsCoreRspPipe` | READ_MEM 响应（RSP2 v2 帧） |

**多客户端说明**

- `PacketsCorePipe` 与 `PacketsCoreRspPipe` 是广播：你这边连上去只是“订阅者”。
- `PacketsCoreSendPipe` 与 `PacketsCoreMemPipe` 支持多个 client：每个连接会被 DLL 起一个读线程。

## 3. 协议速查（直接可用）

### 3.1 PKT2 v2（抓包）

- header: 32 bytes

```py
CAP_MAGIC = b"PKT2"
CAP_VER = 2
CAP_HDR_FMT = "<4sHBBIQIHIH"  # 32 bytes
# magic, ver, typ, af, payload_len, sock, lip, lport, rip, rport
```

字段注意：

- `local_ip` / `remote_ip`：IPv4（network order），转字符串用：

```py
socket.inet_ntoa(struct.pack("!I", ip_u32))
```

- `local_port` / `remote_port`：**host order**。
  - 说明：C++ 源码里某些注释写“network byte order”，但实际 Hook 处对 `sin_port` 做了 `ntohs()`，因此你从管道解出来的端口值就是常见的十进制端口号（主机序）。

### 3.2 CMD2 v2（发包 / 读内存命令）

- header: 24 bytes

```py
CMD_MAGIC = b"CMD2"
CMD_VER = 2
CMD_HDR_FMT = "<4sHBBIQI"  # 24 bytes
# magic, ver, cmd, reserved, flags, sock(uint64), payload_len
```

- `cmd=1`：TCP_SEND，payload 为要发送的 bytes。
- `cmd=2`：READ_MEM，payload 为 12 bytes：

```py
# request_id(4) address(4) type(1) size(2) reserved(1)
READ_MEM_PAYLOAD_FMT = "<IIBHB"  # 12 bytes
```

### 3.3 RSP2 v2（读内存响应）

- frame header: 12 bytes

```py
# RSP2 magic 在 C++ 里是 uint32 写入（0x32505352），在线上就是 4 个 ASCII 字节 'RSP2'。
# 你可以选择：
# - 当作 uint32 解析（与仓库 read_memory.py 一致）
# - 或当作 4s 解析（与 PKT2/CMD2 的写法一致）
RSP_MAGIC_U32 = 0x32505352  # 'RSP2'
RSP_VER = 2
RSP_HDR_FMT = "<IHBBI"  # 12 bytes: magic(u32), ver(u16), rsp_type(u8), reserved(u8), payload_len(u32)
```

- READ_MEM 响应 payload：固定头 16 bytes + data

```py
RSP_READ_MEM_PAYLOAD_HDR_FMT = "<IIIBHB"  # 16 bytes
# request_id, status, address, mem_type, size, reserved
```

## 4. 推荐的 Python 代码结构（不要照抄示例的“临时脚本风格”）

### 4.1 基础工具：可靠读满 N 字节

命名管道是 byte stream，`ReadFile` 可能返回短读。

```py
import win32file

def read_exact(handle, n: int) -> bytes:
    buf = b""
    while len(buf) < n:
        _, chunk = win32file.ReadFile(handle, n - len(buf))
        if not chunk:
            raise EOFError("pipe closed")
        buf += chunk
    return buf
```

### 4.2 连接管道要带重试

注入后 DLL 的 pipe accept 线程需要时间起来；生产代码建议重试 30s。

```py
import time
import win32file

def open_pipe_with_retry(pipe_name: str, access: int, timeout_sec: float = 30.0):
    deadline = time.time() + timeout_sec
    last_err = None
    while time.time() < deadline:
        try:
            return win32file.CreateFile(
                pipe_name,
                access,
                0,
                None,
                win32file.OPEN_EXISTING,
                0,
                None,
            )
        except Exception as e:
            last_err = e
            time.sleep(0.1)
    raise last_err
```

## 5. 常用用法

### 5.1 只抓包（订阅 PKT2）

最小可用示例：参考仓库 `Python Script/packet_capture_test.py`。

你只需要：

1) `CreateFile(\\.\pipe\PacketsCorePipe, GENERIC_READ)`
2) 循环：读 32 字节 header -> 按 `payload_len` 读 payload

### 5.2 边抓包边发包（使用“最后一个 sock”）

推荐做法：

- 起一个 capture 线程持续更新 `last_sock`
- 主线程向 `PacketsCoreSendPipe` 写 CMD2(TCP_SEND)

仓库参考：`Python Script/packet_console.py`

关键点：**sock 必须来自抓包流中出现的 socket 值**。

### 5.3 读内存（READ_MEM + RSP2）

仓库参考：`Python Script/read_memory.py`。

建议把读内存封装为：

- 一个“写命令”的 cmd handle（写入 `PacketsCoreMemPipe`）
- 一个“响应监听线程”（读取 `PacketsCoreRspPipe`，按 `request_id` 分发）

注意事项：

- address 为 `uint32`，更适合 32-bit 目标进程。
- `STR_Z`：`size` 表示“最大扫描长度”（1..1024），如果未遇到 `\0` 且达到 size，返回 `STATUS_STRING_TOO_LONG`。

## 6. 状态码/读取类型

读取类型（请求中的 `type`）：

- `1` = U32（返回 4 bytes）
- `2` = U64（返回 8 bytes）
- `3` = BYTES（返回 size bytes）
- `4` = STR_Z（返回 0 结尾字符串内容，不含 `\0`）
- `5` = STR_FIXED（返回 size bytes）

状态码（响应中的 `status`）：

- `0` OK
- `1` INVALID_ADDRESS
- `2` INVALID_TYPE
- `3` INVALID_SIZE
- `4` STRING_TOO_LONG
- `99` UNKNOWN_ERROR（目前实现主要返回 0/1/2/3/4）

## 7. 常见坑（务必读）

1. **pipe 连上了但一直没数据**
   - 目标进程可能没走 `send/recv`（比如 `WSASend/WSARecv`）。
   - 如果你只想获取某连接的数据，需要自己在 Python 侧按 `sock`/IP/port 过滤。

2. **慢客户端导致“看起来丢包”**
   - DLL 对每个客户端队列限额约 4MB，积压会丢旧帧。
   - 解决：你这边要么更快消费，要么在本地落盘/队列化。

3. **不要假设抓包与发包一一对应**
   - TCP 是流；一次 `send()` 不等于一条“业务消息”。
   - 同理 `recv()` 也可能拆包/粘包。

4. **sock 值不是全局唯一**
   - 连接关闭后 sock 数值可能被复用。
   - 建议在 Python 侧用 `(sock, remote_ip, remote_port)` 联合判断。

## 8. 建议的团队实践

- 把协议常量与 `struct.pack/unpack` 封装在一个小模块里（例如 `packets_core_api.py`）。
- 对外只暴露：
  - `subscribe_packets(callback)`
  - `tcp_send(sock, payload, flags=0)`
  - `read_mem(address, type, size=0) -> (status, data)`

---

## 附：仓库内可直接运行的脚本

路径：`Packets-Core-v2/Python Script/`

- `packet_capture_test.py`：只抓包
- `packet_console.py`：交互式抓包 + 发包
- `tcp_packet_sender.py`：命令行发包（可自动取最后一个 sock）
- `read_memory.py`：交互式读内存
- `packet_auto_responder.py`：示例自动回应 （坐下自动封包站起）
- `auto_f9.py`：示例F9自动刷血

