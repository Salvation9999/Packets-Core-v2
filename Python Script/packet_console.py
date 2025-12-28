import struct
import socket
import threading
import time
import win32file

CAP_PIPE = r"\\.\pipe\PacketsCorePipe"      # PKT2 stream (DLL -> Python)
CMD_PIPE = r"\\.\pipe\PacketsCoreSendPipe"  # CMD2 stream for TCP_SEND (Python -> DLL)

# PKT2 capture header (32 bytes)
CAP_MAGIC = b"PKT2"
CAP_VER = 2
CAP_HDR_FMT = "<4sHBBIQIHIH"  # magic, ver, typ, af, payload_len, sock, lip, lport, rip, rport
CAP_HDR_SIZE = struct.calcsize(CAP_HDR_FMT)

# CMD2 send header (24 bytes)
CMD_MAGIC = b"CMD2"
CMD_VER = 2
CMD_TCP_SEND = 1
CMD_HDR_FMT = "<4sHBBIQI"  # magic, ver, cmd, reserved, flags, sock(uint64), payload_len


def read_exact(handle, n: int) -> bytes:
    buf = b""
    while len(buf) < n:
        _, chunk = win32file.ReadFile(handle, n - len(buf))
        if not chunk:
            raise EOFError("pipe closed")
        buf += chunk
    return buf


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


def ipv4_to_str(ip_net_order_u32: int) -> str:
    return socket.inet_ntoa(struct.pack("!I", ip_net_order_u32))


def parse_hex(s: str) -> bytes:
    s = s.strip().replace(" ", "").replace("\n", "").replace("\t", "")
    if s.startswith("0x") or s.startswith("0X"):
        s = s[2:]
    if len(s) % 2 != 0:
        raise ValueError("hex string length must be even")
    return bytes.fromhex(s)


class SharedState:
    def __init__(self):
        self._lock = threading.Lock()
        self.last_sock = None
        self.flags = 0

    def set_sock(self, socket_handle: int):
        with self._lock:
            self.last_sock = socket_handle

    def get_sock(self):
        with self._lock:
            return self.last_sock

    def set_flags(self, flags: int):
        with self._lock:
            self.flags = flags

    def get_flags(self):
        with self._lock:
            return self.flags


def capture_loop(state: SharedState, stop_evt: threading.Event):
    capture_pipe = open_pipe_with_retry(CAP_PIPE, win32file.GENERIC_READ)
    print("[capture] connected")

    while not stop_evt.is_set():
        hdr = read_exact(capture_pipe, CAP_HDR_SIZE)
        magic, ver, packet_type, address_family, payload_len, socket_handle, local_ip, local_port, remote_ip, remote_port = struct.unpack(CAP_HDR_FMT, hdr)
        if magic != CAP_MAGIC or ver != CAP_VER:
            continue

        payload = read_exact(capture_pipe, payload_len) if payload_len else b""
        state.set_sock(socket_handle)

        tag = "SEND" if packet_type == 1 else "RECV" if packet_type == 2 else f"TYPE_{packet_type}"
        l = f"{ipv4_to_str(local_ip)}:{local_port}" if local_ip != 0 and local_port != 0 else "-"
        r = f"{ipv4_to_str(remote_ip)}:{remote_port}" if remote_ip != 0 and remote_port != 0 else "-"

        print(f"[{tag}] {payload_len} bytes sock=0x{socket_handle:X} {l} -> {r}")
        if payload:
            print(" ".join(f"{b:02X}" for b in payload))


def send_cmd(command_pipe, socket_handle: int, flags: int, payload: bytes):
    hdr = struct.pack(CMD_HDR_FMT, CMD_MAGIC, CMD_VER, CMD_TCP_SEND, 0, flags, socket_handle, len(payload))
    win32file.WriteFile(command_pipe, hdr)
    if payload:
        win32file.WriteFile(command_pipe, payload)


def main():
    state = SharedState()
    stop_evt = threading.Event()

    t = threading.Thread(target=capture_loop, args=(state, stop_evt), daemon=True)
    t.start()

    command_pipe = open_pipe_with_retry(CMD_PIPE, win32file.GENERIC_WRITE)
    print("[cmd] connected")

    print("Enter HEX to send. Commands: /sock, /flags <n>, /quit")
    while True:
        line = input("> ").strip()
        if not line:
            continue
        if line.lower() in ("/q", "/quit", "quit", "exit"):
            break
        if line.lower() == "/sock":
            socket_handle = state.get_sock()
            print("sock=" + (f"0x{socket_handle:X}" if socket_handle is not None else "<none yet>"))
            continue
        if line.lower().startswith("/flags"):
            parts = line.split(maxsplit=1)
            if len(parts) != 2:
                print("usage: /flags 0")
                continue
            state.set_flags(int(parts[1], 0))
            print(f"flags={state.get_flags()}")
            continue

        socket_handle = state.get_sock()
        if socket_handle is None:
            print("no sock yet (wait for any SEND/RECV first)")
            continue

        try:
            payload = parse_hex(line)
        except Exception as e:
            print(f"bad hex: {e}")
            continue

        flags = state.get_flags()
        send_cmd(command_pipe, socket_handle, flags, payload)
        print(f"sent {len(payload)} bytes via sock=0x{socket_handle:X} flags={flags}")

    stop_evt.set()


if __name__ == "__main__":
    main()
