#pragma once

#include <cstdint>
#include <condition_variable>
#include <mutex>
#include <queue>
#include <vector>

// ---------------- PacketItem ----------------
// 用于 Hook（生产者）与 IPC 发送线程（消费者）之间传递的网络数据包
// 设计目标：
// - 结构稳定、可扩展
// - 尽量避免平台相关类型直接暴露（如 SOCKET）
// - 仅支持 IPv4（v2 版本约束）

struct PacketItem
{
    uint8_t type = 0;           // 1=SEND, 2=RECV
    uint8_t addr_flags = 0;     // bit0 = 本地地址有效, bit1 = 远端地址有效
    uint16_t reserved0 = 0;     // 预留字段，用于结构体对齐或未来扩展，当前必须置 0

    uintptr_t sock = 0;         // 套接字值，原始 SOCKET 被强制转换为 uintptr_t 存储，便于在不同模块 / 语言之间传递

    uint32_t local_ipv4 = 0;    // 本地 IPv4 地址，网络字节序（等同于 sockaddr_in.sin_addr.s_addr）
    uint16_t local_port = 0;    // 本地端口，主机字节序
    uint16_t reserved1 = 0;     // 预留字段（对齐 / 扩展）

    uint32_t remote_ipv4 = 0;   // 远端 IPv4 地址，网络字节序
    uint16_t remote_port = 0;   // 远端端口，主机字节序
    uint16_t reserved2 = 0;     // 预留字段（对齐 / 扩展）

    std::vector<uint8_t> data;  // 数据载荷，原始网络包数据（不做解析，保持透明）
};

// ---------------- Global Queue ----------------
// 全局包队列
// Hook 线程作为生产者 push PacketItem
// 发送 / IPC 线程作为消费者 pop PacketItem

extern std::mutex g_mutex; // 互斥锁，保护 g_queue 与 g_stop，防止多线程数据竞争

extern std::condition_variable g_cv; // 条件变量，用于在队列为空时阻塞消费者线程，当有新数据入队或系统退出时被唤醒

extern std::queue<PacketItem> g_queue; // 包队列，多生产者 / 单或多消费者模型

// ---------------- Stop Control ----------------
// 全局停止标志
// 当置为 true 时：
// - 所有阻塞在队列上的线程应尽快退出
// - 不再尝试继续消费数据
extern bool g_stop;

// ---------------- API ----------------

// 唤醒所有等待在 g_cv 上的线程
// 常用于：
// 1. push 新数据后通知消费者
// 2. 程序退出 / DLL 卸载时，唤醒阻塞线程以便安全结束
void PacketQueueNotifyAll();

// 从队列中阻塞式取出一个 PacketItem（带超时）
//
// 参数：
//   out        - 成功时返回的包数据
//   timeoutMs - 最大等待时间（毫秒）
//
// 返回值：
//   true  - 成功取到数据
//   false - 超时 / g_stop 被设置 / 队列为空
bool PacketQueueTryPop(PacketItem& out, uint32_t timeoutMs);
