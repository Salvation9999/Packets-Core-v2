// 精简 Windows 头，减少宏/符号污染
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

// memo:
// 1. IsPrologueHooked的检测方式极度特征化，我假设 Hook 一定是 inline JMP，完全绑定 MinHook 的实现细节，对非 JMP 型 Hook 无效。
// 2. Watchdog 50ms 噪声过大，行为特征很明显，可能容易被抓。
// 3. CreateThread + 永久循环 = 易枚举对象（用户态线程、明确循环、明确栈行为），容易被抓。
// 4. 临界区初始化不是原子保护 : if (!g_csInit) InitializeCriticalSection(...)，目前问题不大。

#include <winsock2.h>       // SOCKET / send / recv
#include <ws2tcpip.h>       // TCP/IP 扩展

#include <MinHook.h>        // Hook 框架

#include <cstring>

#include "NetworkHook.h"        // Hook 对外接口声明
#include "PacketQueue.h"    // 封包队列（生产者：hook，消费者：IPC）

#pragma comment(lib, "ws2_32.lib")

static void FillSocketAddrsIPv4(PacketItem& item, SOCKET s)
{
    // local
    sockaddr_in laddr{};
    int laddrLen = sizeof(laddr);
    if (getsockname(s, (sockaddr*)&laddr, &laddrLen) == 0 && laddr.sin_family == AF_INET)
    {
        item.addr_flags |= 0x01;
        item.local_ipv4 = laddr.sin_addr.s_addr; // network order
        item.local_port = ntohs(laddr.sin_port);
    }

    // remote
    sockaddr_in raddr{};
    int raddrLen = sizeof(raddr);
    if (getpeername(s, (sockaddr*)&raddr, &raddrLen) == 0 && raddr.sin_family == AF_INET)
    {
        item.addr_flags |= 0x02;
        item.remote_ipv4 = raddr.sin_addr.s_addr; // network order
        item.remote_port = ntohs(raddr.sin_port);
    }
}

// 原始 send / recv 函数指针类型
typedef int (WINAPI* Send_t)(SOCKET, const char*, int, int);
typedef int (WINAPI* Recv_t)(SOCKET, char*, int, int);

// 保存原始函数地址（MinHook trampoline）
static Send_t oSend = nullptr;
static Recv_t oRecv = nullptr;

// 需要持续保持 Hook 的目标地址
// 同时覆盖 ws2_32 和 wsock32（兼容老程序/静态引用）
static void* g_pSend_ws2 = nullptr;
static void* g_pRecv_ws2 = nullptr;
static void* g_pSend_wsock32 = nullptr;
static void* g_pRecv_wsock32 = nullptr;

// Hook 看门狗线程句柄，防止HOOK失败
static HANDLE g_watchdogThread = nullptr;

// Hook 管理用的临界区（防止并发重装）
static CRITICAL_SECTION g_hookAdminCs;
static bool g_csInit = false;

// send Hook：抓发包
int WINAPI hkSend(SOCKET s, const char* buf, int len, int flags)
{
    if (buf && len > 0)
    {
        PacketItem item;
        item.type = 1; // 1 = send
        item.sock = (uintptr_t)s;
        FillSocketAddrsIPv4(item, s);
        item.data.assign(buf, buf + len);

        {
            // 多生产者并发保护
            std::lock_guard<std::mutex> lock(g_mutex);
            g_queue.push(std::move(item));
        }

        // 唤醒消费者线程（命名管道 sender）
        g_cv.notify_one();
    }

    // 调用原始 send，保证功能不破坏
    return oSend(s, buf, len, flags);
}

// recv Hook： 抓收包
int WINAPI hkRecv(SOCKET s, char* buf, int len, int flags)
{
    // 先调用原始 recv，保证 buf 已填充
    int ret = oRecv(s, buf, len, flags);

    if (ret > 0 && buf)
    {
        PacketItem item;
        item.type = 2;
        item.sock = (uintptr_t)s;
        FillSocketAddrsIPv4(item, s);
        item.data.assign(buf, buf + ret);

        {
            std::lock_guard<std::mutex> lock(g_mutex);
            g_queue.push(std::move(item));
        }
        g_cv.notify_one();
    }

    return ret;
}

// 对指定模块安装 Hook
static bool HookModule(const char* name)
{
    if (!name || !*name)
        return false;

    // 获取模块基址（仅对已加载模块生效）
    HMODULE mod = GetModuleHandleA(name);
    if (!mod)
        return false;

    // 获取导出的 send / recv 地址
    void* pSend = GetProcAddress(mod, "send");
    void* pRecv = GetProcAddress(mod, "recv");

    if (!pSend || !pRecv)
        return false;

    // 记录真实入口地址，供 watchdog 检测/重装
    if (_stricmp(name, "ws2_32.dll") == 0)
    {
        g_pSend_ws2 = pSend;
        g_pRecv_ws2 = pRecv;
    }
    else if (_stricmp(name, "wsock32.dll") == 0)
    {
        g_pSend_wsock32 = pSend;
        g_pRecv_wsock32 = pRecv;
    }

    // 创建 Hook
    // 即使已存在也不视为失败（watchdog 可能重复 Enable）
    MH_CreateHook(pSend, hkSend, (void**)&oSend);
    MH_CreateHook(pRecv, hkRecv, (void**)&oRecv);

    return true;
}

// 确保临界区只初始化一次
static void EnsureAdminCs()
{
    if (!g_csInit)
    {
        InitializeCriticalSection(&g_hookAdminCs);
        g_csInit = true;
    }
}

// 判断目标函数入口是否仍被 Hook
static bool IsPrologueHooked(void* target)
{
    if (!target)
        return false;

    // MinHook x86/x64 默认是 JMP rel32（0xE9）
    __try
    {
        const volatile unsigned char* p = (const volatile unsigned char*)target;
        return p[0] == 0xE9;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        // 防止目标页被保护导致异常
        return false;
    }
}

// 在不抛异常的前提下强制重写 Hook
static void ReenableAllHooks_NoThrow()
{
    // 必须在 g_hookAdminCs 保护下调用
    // Disable + Enable 可以覆盖被还原的 prologue
    if (g_pSend_ws2)
    {
        MH_DisableHook(g_pSend_ws2);
        MH_EnableHook(g_pSend_ws2);
    }
    if (g_pRecv_ws2)
    {
        MH_DisableHook(g_pRecv_ws2);
        MH_EnableHook(g_pRecv_ws2);
    }
    if (g_pSend_wsock32)
    {
        MH_DisableHook(g_pSend_wsock32);
        MH_EnableHook(g_pSend_wsock32);
    }
    if (g_pRecv_wsock32)
    {
        MH_DisableHook(g_pRecv_wsock32);
        MH_EnableHook(g_pRecv_wsock32);
    }
}

// Hook 看门狗线程
static DWORD WINAPI WatchdogThreadMain(LPVOID)
{
    // 初始检查间隔 50ms (激进策略，可以改）
    // 行为特征很明显，可能容易被抓吗？未测试不清楚）
    // 目前不修改逻辑或只修改数值，后期考虑“降噪”
    const DWORD kIntervalMs = 50;
    DWORD backoffMs = kIntervalMs;

    while (!g_stop)
    {
        // 检查所有已记录的 Hook 入口是否仍存在
        const bool ws2SendOk = IsPrologueHooked(g_pSend_ws2);
        const bool ws2RecvOk = IsPrologueHooked(g_pRecv_ws2);
        const bool w32SendOk = IsPrologueHooked(g_pSend_wsock32);
        const bool w32RecvOk = IsPrologueHooked(g_pRecv_wsock32);

        const bool anyMissing =
            (g_pSend_ws2 && !ws2SendOk) || (g_pRecv_ws2 && !ws2RecvOk) ||
            (g_pSend_wsock32 && !w32SendOk) || (g_pRecv_wsock32 && !w32RecvOk);

        if (anyMissing)
        {
            // 有 Hook 被还原，进入管理区重装
            EnsureAdminCs();
            EnterCriticalSection(&g_hookAdminCs);
            ReenableAllHooks_NoThrow();
            LeaveCriticalSection(&g_hookAdminCs);

            // 指数退避，避免与保护机制硬刚
            DWORD next = backoffMs + kIntervalMs;
            backoffMs = (next > 1000) ? 1000 : next;
        }
        else
        {
            // 一切正常，恢复快速检查
            backoffMs = kIntervalMs;
        }

        Sleep(backoffMs);
    }

    return 0;
}

// 启动 Hook 看门狗
bool StartNetHookWatchdog()
{
    if (g_watchdogThread)
        return true;

    EnsureAdminCs();
    g_watchdogThread = CreateThread(nullptr, 0, WatchdogThreadMain, nullptr, 0, nullptr);
    return g_watchdogThread != nullptr;
}

// 停止 Hook 看门狗
void StopNetHookWatchdog()
{
    if (!g_watchdogThread)
        return;

    // 不无限等待，避免卡在 Loader Lock
    WaitForSingleObject(g_watchdogThread, 500);
    CloseHandle(g_watchdogThread);
    g_watchdogThread = nullptr;
}

// 初始化网络 Hook
bool InitNetHook()
{
    EnsureAdminCs();

    // 初始化 MinHook
    if (MH_Initialize() != MH_OK)
        return false;

    // 等待 ws2_32.dll 被加载（延迟加载目标）
    while (!GetModuleHandleA("ws2_32.dll"))
        Sleep(50);

    bool ok = false;
    ok |= HookModule("ws2_32.dll");
    ok |= HookModule("wsock32.dll");

    if (!ok)
        return false;

    // 启用所有已创建 Hook
    if (MH_EnableHook(MH_ALL_HOOKS) != MH_OK)
        return false;

    return true;
}

// 卸载网络 Hook
void UninitNetHook()
{
    // Stop watchdog before uninitializing MinHook.
    StopNetHookWatchdog();

    MH_DisableHook(MH_ALL_HOOKS);
    MH_Uninitialize();
}
