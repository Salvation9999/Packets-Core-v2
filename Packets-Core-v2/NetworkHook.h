#pragma once

bool InitNetHook(); // 初始化网络 Hook
// 负责：
// - 初始化 MinHook
// - 等待 ws2_32.dll 加载
// - 对 send / recv 安装 Hook
// - 启用所有 Hook

void UninitNetHook(); // 反初始化网络 Hook
// 负责：
// - 停止 Hook 看门狗
// - 禁用所有 Hook
// - 反初始化 MinHook
// 注意：应在安全上下文中调用（非 DllMain / 非 Loader Lock）

bool StartNetHookWatchdog(); // Hook 看门狗：用于在目标进程还原函数前几字节时，强制重新写入 Hook
// 适用场景：
// - 目标程序自修复 ws2_32 / wsock32 函数入口
// - Hook 被周期性擦除
// 设计说明：
// - 检查间隔固定为 50ms
// - 属于“主动对抗型”机制

void StopNetHookWatchdog(); // 停止 Hook 看门狗
// 行为：
// - 请求 watchdog 线程退出
// - 等待有限时间并关闭线程句柄
// 注意：
// - 不保证 watchdog 已完成一次完整循环
// - 目的是避免卸载阶段阻塞或死锁

