#include "PacketQueue.h"

#include <chrono>

// 全局互斥锁
// 保护 g_queue 与 g_stop，防止多线程并发访问导致数据竞争
std::mutex g_mutex;

// 条件变量
// 用于在队列为空时阻塞消费者线程，
// 当有新数据入队或需要停止时被唤醒
std::condition_variable g_cv;

// 数据队列
// 生产者向其中 push PacketItem，消费者从中 pop
std::queue<PacketItem> g_queue;

// 停止标志
// 为 true 时表示队列即将销毁或系统准备退出，
// 所有等待中的线程应尽快返回
bool g_stop = false;

// 唤醒所有等待在条件变量上的线程
void PacketQueueNotifyAll()
{
    g_cv.notify_all();
}

// 尝试从队列中取出一个 PacketItem
// 参数：
//   out        - 成功时返回的包数据
//   timeoutMs - 等待超时时间（毫秒）
//
// 返回值：
//   true  - 成功取到数据
//   false - 超时 / 队列被停止 / 队列为空
bool PacketQueueTryPop(PacketItem& out, uint32_t timeoutMs)
{
    // 获取互斥锁，进入临界区
    std::unique_lock<std::mutex> lock(g_mutex);

    if (!g_cv.wait_for(lock, std::chrono::milliseconds(timeoutMs), [] {
            return g_stop || !g_queue.empty();
        }))
    {
        return false; // 等待超时
    }

    if (g_stop) // 如果已经被要求停止，不再继续取数据
        return false;

    if (g_queue.empty())  // 理论上不会发生（谓词已判断），但作为防御性检查
        return false;

    // 取出队首元素
    // 使用 std::move 避免不必要的拷贝
    out = std::move(g_queue.front());
    g_queue.pop();
    return true;
}
