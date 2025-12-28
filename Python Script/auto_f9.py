#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
自动按键脚本 - 当内存条件满足时按F9
地址A < 地址B * 95% 时触发
"""

import win32pipe
import win32file
import win32gui
import win32con
import win32api
import struct
import time
import threading
from collections import deque

# ==================== 配置 ====================
ADDR_A = 0x15235AC
ADDR_B = 0x15235B0
CHECK_INTERVAL_MS = 20  # 检查间隔20ms
MAX_KEY_QUEUE = 10      # 最大按键队列
TARGET_WINDOW_TITLE = "仙境传说"  # 留空则自动查找，或填入窗口标题关键字

CMD_PIPE_NAME = r'\\.\pipe\PacketsCoreMemPipe'  # READ_MEM commands
RSP_PIPE_NAME = r'\\.\pipe\PacketsCoreRspPipe'

VK_F9 = 0x78  # F9虚拟键码

# ==================== 协议定义 ====================
CMD_MAGIC = 0x32444D43  # 'CMD2'
CMD_VERSION = 2
CMD_READ_MEM = 2

RSP_MAGIC = 0x32505352  # 'RSP2'
RSP_VERSION = 2

MEM_TYPE_U32 = 1

# ==================== 全局变量 ====================
key_queue = deque(maxlen=MAX_KEY_QUEUE)
request_id_counter = 1
responses = {}
resp_lock = threading.Lock()
stop_flag = threading.Event()
target_hwnd = None

# ==================== 内存读取 ====================
def read_u32(cmd_pipe, address, request_id):
    """发送READ_MEM命令读取U32"""
    # CMD2 Header (24字节)
    # [magic(4)][ver(2)][cmd(1)][reserved(1)][flags(4)][sock(8)][payload_len(4)]
    header = struct.pack(
        '<IHBBIQI',
        CMD_MAGIC,      # magic (4)
        CMD_VERSION,    # ver (2)
        CMD_READ_MEM,   # cmd (1)
        0,              # reserved0 (1)
        0,              # flags (4)
        0,              # sock (8, unused for READ_MEM)
        12              # payload_len (4)
    )

    # ReadMemPayload (12字节)
    payload = struct.pack(
        '<IIBHB',
        request_id,     # request_id
        address,        # address
        MEM_TYPE_U32,   # type
        4,              # size
        0               # reserved
    )

    win32file.WriteFile(cmd_pipe, header + payload)

# ==================== 响应接收线程 ====================
def response_listener():
    """后台线程接收READ_MEM响应"""
    global responses

    try:
        rsp_pipe = win32file.CreateFile(
            RSP_PIPE_NAME,
            win32file.GENERIC_READ,
            0,
            None,
            win32file.OPEN_EXISTING,
            0,
            None
        )
    except Exception as e:
        print(f"[ERROR] 无法连接响应管道: {e}")
        return

    print("[INFO] 响应监听线程已启动")

    while not stop_flag.is_set():
        try:
            # 读取RSP2 Header (12字节)
            # [magic(4)][ver(2)][rsp_type(1)][reserved(1)][payload_len(4)]
            hr, header_data = win32file.ReadFile(rsp_pipe, 12)
            if hr != 0 or len(header_data) < 12:
                break

            magic, ver, rsp_type, _, payload_len = struct.unpack('<IHBBI', header_data)

            if magic != RSP_MAGIC or ver != RSP_VERSION:
                continue

            # 读取Payload
            if payload_len > 0:
                hr, payload_data = win32file.ReadFile(rsp_pipe, payload_len)
                if hr != 0 or len(payload_data) < payload_len:
                    break

                # 解析响应
                request_id, status, address, mem_type, size, _ = struct.unpack('<IIIBHB', payload_data[:16])
                raw_bytes = payload_data[16:]

                if status == 0 and mem_type == MEM_TYPE_U32 and len(raw_bytes) >= 4:
                    value = struct.unpack('<I', raw_bytes[:4])[0]
                    with resp_lock:
                        responses[request_id] = value

        except Exception as e:
            if not stop_flag.is_set():
                print(f"[ERROR] 响应线程异常: {e}")
            break

    win32file.CloseHandle(rsp_pipe)
    print("[INFO] 响应监听线程已停止")

# ==================== 窗口查找 ====================
def find_target_window():
    """查找目标窗口句柄"""
    global target_hwnd

    def enum_callback(hwnd, _):
        global target_hwnd
        if not win32gui.IsWindowVisible(hwnd):
            return True
        title = win32gui.GetWindowText(hwnd)
        if not title:
            return True

        # 如果指定了窗口标题，检查是否匹配
        if TARGET_WINDOW_TITLE:
            if TARGET_WINDOW_TITLE.lower() in title.lower():
                target_hwnd = hwnd
                print(f"[INFO] 找到目标窗口: {title} (HWND={hwnd})")
                return False
        # 否则找第一个非系统窗口
        else:
            # 排除一些系统窗口
            if any(x in title for x in ['Program Manager', 'Task Switching', 'MSCTFIME']):
                return True
            target_hwnd = hwnd
            print(f"[INFO] 自动选择窗口: {title} (HWND={hwnd})")
            return False
        return True

    win32gui.EnumWindows(enum_callback, None)
    return target_hwnd is not None

def send_f9_to_window(hwnd):
    """向指定窗口发送F9按键"""
    if not hwnd or not win32gui.IsWindow(hwnd):
        return False

    # 发送WM_KEYDOWN
    win32api.PostMessage(hwnd, win32con.WM_KEYDOWN, VK_F9, 0)
    time.sleep(0.01)  # 短暂延迟
    # 发送WM_KEYUP
    win32api.PostMessage(hwnd, win32con.WM_KEYUP, VK_F9, 0xC0000001)
    return True

# ==================== 按键执行线程 ====================
def key_presser():
    """后台线程执行按键队列"""
    print("[INFO] 按键执行线程已启动")

    while not stop_flag.is_set():
        if key_queue and target_hwnd:
            timestamp = key_queue.popleft()
            if send_f9_to_window(target_hwnd):
                print(f"[KEY] 按下F9 (队列剩余: {len(key_queue)})")
            else:
                print(f"[WARN] 发送F9失败")
        time.sleep(0.01)

    print("[INFO] 按键执行线程已停止")

# ==================== 主循环 ====================
def main():
    global request_id_counter

    print("=" * 50)
    print("自动F9按键脚本 - 持续监控模式")
    print("=" * 50)
    print(f"地址A: 0x{ADDR_A:X}")
    print(f"地址B: 0x{ADDR_B:X}")
    print(f"触发条件: A < B * 0.95")
    print(f"检查间隔: {CHECK_INTERVAL_MS}ms (持续检查)")
    print(f"最大队列: {MAX_KEY_QUEUE}")
    print("=" * 50)
    print()

    # 查找目标窗口
    print("[1/3] 查找目标窗口...")
    if not find_target_window():
        print("[ERROR] 未找到目标窗口！")
        if not TARGET_WINDOW_TITLE:
            print("提示: 可以在脚本开头设置 TARGET_WINDOW_TITLE 指定窗口标题")
        input("\n按回车键退出...")
        return
    print()

    # 连接命令管道
    print("[2/3] 连接命令管道...")
    try:
        cmd_pipe = win32file.CreateFile(
            CMD_PIPE_NAME,
            win32file.GENERIC_WRITE,
            0,
            None,
            win32file.OPEN_EXISTING,
            0,
            None
        )
        print("[OK] 命令管道已连接")
    except Exception as e:
        print(f"[ERROR] 无法连接命令管道: {e}")
        print("请确保DLL已注入到目标进程")
        input("\n按回车键退出...")
        return

    # 启动响应监听线程
    print("[3/3] 启动后台线程...")
    listener_thread = threading.Thread(target=response_listener, daemon=True)
    listener_thread.start()
    time.sleep(0.2)  # 等待连接建立

    # 启动按键执行线程
    key_thread = threading.Thread(target=key_presser, daemon=True)
    key_thread.start()

    print("[OK] 所有线程已启动")
    print()
    print("=" * 50)
    print("开始持续监控内存... (按 Ctrl+C 停止)")
    print("=" * 50)
    print()

    try:
        while True:
            loop_start = time.time()

            # 读取地址A
            request_id_a = request_id_counter
            request_id_counter += 1
            read_u32(cmd_pipe, ADDR_A, request_id_a)

            # 读取地址B
            request_id_b = request_id_counter
            request_id_counter += 1
            read_u32(cmd_pipe, ADDR_B, request_id_b)

            # 等待响应
            timeout = 0.1  # 100ms超时
            start_wait = time.time()
            value_a = None
            value_b = None

            while (time.time() - start_wait) < timeout:
                with resp_lock:
                    if request_id_a in responses:
                        value_a = responses.pop(request_id_a)
                    if request_id_b in responses:
                        value_b = responses.pop(request_id_b)

                if value_a is not None and value_b is not None:
                    break
                time.sleep(0.001)

            # 检查条件
            if value_a is not None and value_b is not None:
                threshold = value_b * 0.95

                print(f"[MEM] A={value_a:10} B={value_b:10} 阈值={threshold:10.1f} ", end='')

                if value_a < threshold:
                    if len(key_queue) < MAX_KEY_QUEUE:
                        key_queue.append(time.time())
                        print("→ 触发! (已加入队列)")
                    else:
                        print("→ 触发但队列已满")
                else:
                    print()
            else:
                print(f"[WARN] 读取超时 (A={value_a}, B={value_b})")

            # 保持20ms间隔
            elapsed = time.time() - loop_start
            sleep_time = (CHECK_INTERVAL_MS / 1000.0) - elapsed
            if sleep_time > 0:
                time.sleep(sleep_time)

    except KeyboardInterrupt:
        print("\n\n[INFO] 用户中断，正在停止...")
    except Exception as e:
        print(f"\n[ERROR] 运行时错误: {e}")
        import traceback
        traceback.print_exc()
    finally:
        stop_flag.set()
        time.sleep(0.2)
        win32file.CloseHandle(cmd_pipe)
        print("[INFO] 已断开连接")
        print()
        input("按回车键退出...")

if __name__ == '__main__':
    try:
        main()
    except Exception as e:
        print(f"\n[FATAL ERROR] {e}")
        import traceback
        traceback.print_exc()
        input("\n按回车键退出...")
