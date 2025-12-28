// 精简 Windows 头，减少宏/符号污染
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

// memo:
// 1. CreateThread 本身仍然是“有风险但可接受”。微软文档：DllMain 里不推荐 CreateThread，实际上不这么做好像不行。
// 2. PROCESS_DETACH 不调用 CoreStop，卸载时 Core 线程可能仍在跑一小段时间，清理完全依赖外部调用方。前提是：要有外部控制流程。
// 3. g_workerThread 不 Join = 不确定退出时机。稳定，不死锁，不保证顺序。

#include "CoreEngine.h"  // Core 业务逻辑头文件
#include "PacketQueue.h" // 封包队列，用于线程间通信 / 唤醒

static HANDLE g_workerThread = nullptr; // 工作线程句柄，全局保存，便于卸载时关闭


// 后台工作线程入口函数 （注意：这里才是“安全上下文”，可以做任何耗时/复杂操作）
static DWORD WINAPI CoreWorker(LPVOID)
{
    // 在独立线程中启动核心逻辑
    // 不能在 DllMain 里做这些事情（会卡 Loader Lock）
    CoreStart();
    return 0;
}

// DLL 主入口
BOOL APIENTRY DllMain(HMODULE hModule, DWORD ul_reason_for_call, LPVOID)
{
    switch (ul_reason_for_call)
    {
    case DLL_PROCESS_ATTACH:

        // 禁止线程创建/销毁时反复回调 DllMain（减少干扰，也避免意外死锁）
        DisableThreadLibraryCalls(hModule);

        // 创建后台工作线程 （注意：这里只是 CreateThread，本身不做任何业务）
        g_workerThread = CreateThread(
            nullptr,        // 默认安全属性
            0,              // 默认栈大小
            CoreWorker,     // 线程入口
            nullptr,        // 参数
            0,              // 立即运行
            nullptr         // 不关心线程 ID
        );
        break;

    //DLL卸载阶段 （仍然处在 Loader Lock 下，绝不能做阻塞 / 同步 / 网络 / IO 等操作）
    case DLL_PROCESS_DETACH:

        // 通知后台线程停止（软信号）
        g_stop = true;

        // 唤醒所有可能在等待队列上的线程
        // 防止线程永远卡在 wait 状态
        PacketQueueNotifyAll();

        if (g_workerThread)
        {
            // 这里只是关闭句柄，不等待线程退出
            // WaitForSingleObject 在这里是危险操作
            CloseHandle(g_workerThread);
            g_workerThread = nullptr;
        }
        // 如果需要“确定性退出”（等线程干净退出）：
        // 正确做法是：
        // 1. 导出一个 Stop 接口
        // 2. 在注入器 / 外部控制进程里调用 CoreStop()
        // 3. 等线程退出后再 FreeLibrary
        break;

    default:
        break;
    }
    return TRUE;
}
