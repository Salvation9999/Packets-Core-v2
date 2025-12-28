import argparse
import struct
import threading
import time
import win32file

CMD_PIPE = r"\\.\pipe\PacketsCoreSendPipe"  # TCP_SEND commands
CAP_PIPE = r"\\.\pipe\PacketsCorePipe"  # PKT2 stream (multi-client broadcast)

# CMD2 v2 header: <4s H B B I Q I
CMD_HDR_FMT = "<4sHBBIQI"  # 24 bytes
CMD_MAGIC = b"CMD2"
CMD_VER = 2
CMD_TCP_SEND = 1

# PKT2 capture header: <4s H B B I Q I H I H
CAP_HDR_FMT = "<4sHBBIQIHIH"  # 32 bytes
CAP_MAGIC = b"PKT2"
CAP_VER = 2
CAP_HDR_SIZE = struct.calcsize(CAP_HDR_FMT)


def read_exact(handle, n: int) -> bytes:
    buf = b""
    while len(buf) < n:
        _, chunk = win32file.ReadFile(handle, n - len(buf))
        if not chunk:
            raise EOFError("pipe closed")
        buf += chunk
    return buf


def open_pipe_with_retry(pipe_name: str, access: int, timeout_sec: float = 5.0):
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


def parse_hex(s: str) -> bytes:
    s = s.strip().replace(" ", "").replace("\n", "").replace("\t", "")
    if s.startswith("0x") or s.startswith("0X"):
        s = s[2:]
    if len(s) % 2 != 0:
        raise ValueError("hex string length must be even")
    return bytes.fromhex(s)


class SockTracker:
    def __init__(self):
        self._lock = threading.Lock()
        self._last_sock = None

    def set(self, socket_handle: int):
        with self._lock:
            self._last_sock = socket_handle

    def get(self):
        with self._lock:
            return self._last_sock


def capture_thread_main(tracker: SockTracker, stop_evt: threading.Event):
    capture_pipe = open_pipe_with_retry(CAP_PIPE, win32file.GENERIC_READ, timeout_sec=30.0)
    while not stop_evt.is_set():
        hdr = read_exact(capture_pipe, CAP_HDR_SIZE)
        magic, ver, packet_type, address_family, payload_len, socket_handle, local_ip, local_port, remote_ip, remote_port = struct.unpack(CAP_HDR_FMT, hdr)
        if magic != CAP_MAGIC or ver != CAP_VER:
            continue
        tracker.set(socket_handle)
        if payload_len:
            _ = read_exact(capture_pipe, payload_len)


def send_cmd(command_pipe, socket_handle: int, flags: int, payload: bytes):
    hdr = struct.pack(CMD_HDR_FMT, CMD_MAGIC, CMD_VER, CMD_TCP_SEND, 0, flags, socket_handle, len(payload))
    win32file.WriteFile(command_pipe, hdr)
    if payload:
        win32file.WriteFile(command_pipe, payload)


def main():
    ap = argparse.ArgumentParser(description="Send TCP payload via PacketsCore CMD2 pipe")
    ap.add_argument("--sock", default=None, help="socket value (decimal or 0x...). If omitted, auto-use last sock from capture stream")
    ap.add_argument("--flags", default="0", help="send() flags (default 0)")
    ap.add_argument("--hex", default=None, help="payload hex string (optional; if omitted, enter interactive mode)")
    args = ap.parse_args()

    flags = int(args.flags, 0)

    # cmd pipe (write)
    command_pipe = open_pipe_with_retry(CMD_PIPE, win32file.GENERIC_WRITE, timeout_sec=30.0)

    # sock source
    tracker = SockTracker()
    stop_evt = threading.Event()
    t = None

    if args.sock is not None:
        socket_handle_fixed = int(args.sock, 0)
    else:
        t = threading.Thread(target=capture_thread_main, args=(tracker, stop_evt), daemon=True)
        t.start()
        socket_handle_fixed = None

    def resolve_sock():
        if socket_handle_fixed is not None:
            return socket_handle_fixed
        while True:
            v = tracker.get()
            if v is not None:
                return v
            time.sleep(0.05)

    try:
        if args.hex is not None:
            payload = parse_hex(args.hex)
            send_cmd(command_pipe, resolve_sock(), flags, payload)
            return

        print("Enter HEX per line. Empty line to quit.")
        while True:
            line = input("> ").strip()
            if not line:
                break
            payload = parse_hex(line)
            socket_handle = resolve_sock()
            send_cmd(command_pipe, socket_handle, flags, payload)
            print(f"sent {len(payload)} bytes via sock=0x{socket_handle:X}")
    finally:
        stop_evt.set()


if __name__ == "__main__":
    main()
