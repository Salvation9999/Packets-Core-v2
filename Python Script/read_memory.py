#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
Memory Reader - 通过DLL读取目标进程内存
"""

import struct
import win32file
import win32pipe
import threading
import queue
import time

# 管道名称
CMD_PIPE_NAME = r'\\.\pipe\PacketsCoreMemPipe'  # READ_MEM commands
RSP_PIPE_NAME = r'\\.\pipe\PacketsCoreRspPipe'

# 协议常量
CMD_MAGIC = 0x32444D43  # 'CMD2'
CMD_VERSION = 2
CMD_READ_MEM = 2

RSP_MAGIC = 0x32505352  # 'RSP2'
RSP_VERSION = 2
RSP_TYPE_READ_MEM = 1

# 内存读取类型
READ_TYPE_U32 = 1
READ_TYPE_U64 = 2
READ_TYPE_BYTES = 3
READ_TYPE_STR_Z = 4
READ_TYPE_STR_FIXED = 5

# 状态码
STATUS_OK = 0
STATUS_INVALID_ADDRESS = 1
STATUS_INVALID_TYPE = 2
STATUS_INVALID_SIZE = 3
STATUS_STRING_TOO_LONG = 4

STATUS_NAMES = {
    STATUS_OK: "成功",
    STATUS_INVALID_ADDRESS: "非法地址",
    STATUS_INVALID_TYPE: "无效类型",
    STATUS_INVALID_SIZE: "无效大小",
    STATUS_STRING_TOO_LONG: "字符串过长"
}


class MemoryReader:
    def __init__(self):
        self.cmd_pipe = None
        self.rsp_pipe = None
        self.request_id_counter = 1
        self.pending_requests = {}  # request_id -> queue
        self.rsp_thread = None
        self.running = False

    def connect(self):
        """连接到DLL的命名管道"""
        try:
            # 连接命令管道
            self.cmd_pipe = win32file.CreateFile(
                CMD_PIPE_NAME,
                win32file.GENERIC_WRITE,
                0,
                None,
                win32file.OPEN_EXISTING,
                0,
                None
            )
            print(f"✓ 已连接到命令管道: {CMD_PIPE_NAME}")

            # 连接响应管道
            self.rsp_pipe = win32file.CreateFile(
                RSP_PIPE_NAME,
                win32file.GENERIC_READ,
                0,
                None,
                win32file.OPEN_EXISTING,
                0,
                None
            )
            print(f"✓ 已连接到响应管道: {RSP_PIPE_NAME}")

            # 启动响应接收线程
            self.running = True
            self.rsp_thread = threading.Thread(target=self._rsp_listener, daemon=True)
            self.rsp_thread.start()

            return True
        except Exception as e:
            print(f"✗ 连接失败: {e}")
            print("  请确保DLL已注入到目标进程")
            return False

    def _rsp_listener(self):
        """后台线程：持续接收响应"""
        while self.running:
            try:
                # 读取响应帧头 (12字节)
                header_data = self._read_exact(self.rsp_pipe, 12)
                if not header_data:
                    break

                magic, ver, rsp_type, reserved, payload_len = struct.unpack('<IHBBI', header_data)

                if magic != RSP_MAGIC or ver != RSP_VERSION:
                    continue

                # 读取payload
                payload_data = self._read_exact(self.rsp_pipe, payload_len)
                if not payload_data:
                    break

                # 解析响应
                if rsp_type == RSP_TYPE_READ_MEM:
                    self._handle_read_mem_response(payload_data)

            except Exception as e:
                if self.running:
                    print(f"响应接收错误: {e}")
                break

    def _read_exact(self, pipe, size):
        """精确读取指定字节数"""
        data = b''
        while len(data) < size:
            try:
                result, chunk = win32file.ReadFile(pipe, size - len(data))
                if not chunk:
                    return None
                data += chunk
            except:
                return None
        return data

    def _handle_read_mem_response(self, payload_data):
        """处理读内存响应"""
        # 解析payload头部 (16字节)
        request_id, status, address, mem_type, size, reserved = struct.unpack('<IIIBHB', payload_data[:16])
        raw_bytes = payload_data[16:]

        # 找到等待的请求
        if request_id in self.pending_requests:
            result_queue = self.pending_requests[request_id]
            result_queue.put({
                'request_id': request_id,
                'status': status,
                'address': address,
                'type': mem_type,
                'size': size,
                'data': raw_bytes
            })

    def read_memory(self, address, mem_type, size=0):
        """
        读取内存
        :param address: 内存地址 (uint32)
        :param mem_type: 读取类型 (1-5)
        :param size: 大小 (对于BYTES/STR类型)
        :return: (status, data) 元组
        """
        request_id = self.request_id_counter
        self.request_id_counter += 1

        # 创建响应队列
        result_queue = queue.Queue()
        self.pending_requests[request_id] = result_queue

        try:
            # 构建CMD2请求
            # Header: magic(4) + ver(2) + cmd(1) + reserved(1) + flags(4) + sock(8) + payload_len(4) = 24字节
            # Payload: request_id(4) + address(4) + type(1) + size(2) + reserved(1) = 12字节
            payload = struct.pack('<IIBHB', request_id, address, mem_type, size, 0)
            header = struct.pack('<IHBBIQI', CMD_MAGIC, CMD_VERSION, CMD_READ_MEM, 0, 0, 0, len(payload))

            # 发送请求
            win32file.WriteFile(self.cmd_pipe, header + payload)

            # 等待响应 (5秒超时)
            try:
                response = result_queue.get(timeout=5.0)
                return response['status'], response['data']
            except queue.Empty:
                return -1, None

        finally:
            # 清理
            if request_id in self.pending_requests:
                del self.pending_requests[request_id]

    def close(self):
        """关闭连接"""
        self.running = False
        if self.cmd_pipe:
            win32file.CloseHandle(self.cmd_pipe)
        if self.rsp_pipe:
            win32file.CloseHandle(self.rsp_pipe)


def format_data(data, mem_type):
    """格式化显示数据"""
    if not data:
        return "无数据"

    if mem_type == READ_TYPE_U32:
        if len(data) == 4:
            value = struct.unpack('<I', data)[0]
            return f"0x{value:08X} ({value})"
    elif mem_type == READ_TYPE_U64:
        if len(data) == 8:
            value = struct.unpack('<Q', data)[0]
            return f"0x{value:016X} ({value})"
    elif mem_type in [READ_TYPE_BYTES, READ_TYPE_STR_Z, READ_TYPE_STR_FIXED]:
        # 尝试作为字符串显示
        hex_str = data.hex().upper()
        hex_formatted = ' '.join(hex_str[i:i+2] for i in range(0, len(hex_str), 2))

        result = f"Hex: {hex_formatted}\n"

        # 尝试UTF-8解码
        try:
            text = data.decode('utf-8')
            result += f"UTF-8: {repr(text)}"
        except:
            pass

        # 尝试GBK解码
        try:
            text = data.decode('gbk')
            result += f"\nGBK: {repr(text)}"
        except:
            pass

        return result

    return data.hex().upper()


def main():
    print("=" * 60)
    print("  内存读取工具 - Memory Reader")
    print("=" * 60)
    print()

    reader = MemoryReader()

    if not reader.connect():
        return

    print()
    print("连接成功！输入 'quit' 或 'exit' 退出")
    print("-" * 60)

    try:
        while True:
            print()
            # 输入地址
            addr_input = input("请输入内存地址 (十六进制，如 0x12345678): ").strip()
            if addr_input.lower() in ['quit', 'exit', 'q']:
                break

            try:
                address = int(addr_input, 16)
            except:
                print("✗ 无效的地址格式")
                continue

            # 选择类型
            print("\n请选择读取类型:")
            print("  1 - U32 (4字节整数)")
            print("  2 - U64 (8字节整数)")
            print("  3 - BYTES (自定义字节数组)")
            print("  4 - STR_Z (0结尾字符串)")
            print("  5 - STR_FIXED (固定长度字符串)")

            type_input = input("类型 (1-5): ").strip()
            try:
                mem_type = int(type_input)
                if mem_type < 1 or mem_type > 5:
                    print("✗ 无效的类型")
                    continue
            except:
                print("✗ 无效的类型")
                continue

            # 如果需要，输入大小
            size = 0
            if mem_type in [READ_TYPE_BYTES, READ_TYPE_STR_Z, READ_TYPE_STR_FIXED]:
                size_input = input(f"请输入{'最大扫描' if mem_type == READ_TYPE_STR_Z else '读取'}长度 (1-1024): ").strip()
                try:
                    size = int(size_input)
                    if size < 1 or size > 1024:
                        print("✗ 长度必须在1-1024之间")
                        continue
                except:
                    print("✗ 无效的长度")
                    continue

            # 发送读取请求
            print(f"\n正在读取 0x{address:08X}...")
            status, data = reader.read_memory(address, mem_type, size)

            # 显示结果
            print("-" * 60)
            if status == -1:
                print("✗ 超时：未收到响应")
            elif status != STATUS_OK:
                print(f"✗ 读取失败: {STATUS_NAMES.get(status, f'未知错误({status})')}")
            else:
                print("✓ 读取成功!")
                print(f"\n结果:")
                print(format_data(data, mem_type))
            print("-" * 60)

    except KeyboardInterrupt:
        print("\n\n中断")
    finally:
        reader.close()
        print("\n已断开连接")


if __name__ == '__main__':
    main()
