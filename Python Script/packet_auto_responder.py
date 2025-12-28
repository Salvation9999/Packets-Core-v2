import struct
import time
import win32file

CAP_PIPE = r"\\.\pipe\PacketsCorePipe"      # PKT2 v2 stream (DLL -> Python)
CMD_PIPE = r"\\.\pipe\PacketsCoreSendPipe"  # CMD2 v2 stream for TCP_SEND (Python -> DLL)

# PKT2 v2 header (32 bytes)
CAP_MAGIC = b"PKT2"
CAP_VER = 2
CAP_HDR_FMT = "<4sHBBIQIHIH"
CAP_HDR_SIZE = struct.calcsize(CAP_HDR_FMT)

# CMD2 v2 header (24 bytes)
CMD_MAGIC = b"CMD2"
CMD_VER = 2
CMD_TCP_SEND = 1
CMD_HDR_FMT = "<4sHBBIQI"  # magic, ver, cmd, reserved, flags, sock(uint64), payload_len

MATCH_HEX = "37 04 00 00 00 00 02"
REPLY_HEX = "37 04 00 00 00 00 03"

MATCH_BYTES = bytes.fromhex(MATCH_HEX.replace(" ", ""))
REPLY_BYTES = bytes.fromhex(REPLY_HEX.replace(" ", ""))


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


def send_cmd(command_pipe, socket_handle: int, flags: int, payload: bytes):
    hdr = struct.pack(CMD_HDR_FMT, CMD_MAGIC, CMD_VER, CMD_TCP_SEND, 0, flags, socket_handle, len(payload))
    win32file.WriteFile(command_pipe, hdr)
    if payload:
        win32file.WriteFile(command_pipe, payload)


def main():
    print("[logic] connecting pipes...")
    capture_pipe = open_pipe_with_retry(CAP_PIPE, win32file.GENERIC_READ, timeout_sec=30.0)
    command_pipe = open_pipe_with_retry(CMD_PIPE, win32file.GENERIC_WRITE, timeout_sec=30.0)
    print("[logic] connected")

    # simple per-sock debounce to avoid spamming if packets repeat rapidly
    last_sent_ts_by_sock = {}
    debounce_sec = 0.2

    while True:
        hdr = read_exact(capture_pipe, CAP_HDR_SIZE)
        magic, ver, packet_type, address_family, payload_len, socket_handle, local_ip, local_port, remote_ip, remote_port = struct.unpack(CAP_HDR_FMT, hdr)
        if magic != CAP_MAGIC or ver != CAP_VER:
            # desync; read nothing else
            continue

        payload = read_exact(capture_pipe, payload_len) if payload_len else b""

        if payload == MATCH_BYTES:
            now = time.time()
            last = last_sent_ts_by_sock.get(socket_handle, 0.0)
            if now - last >= debounce_sec:
                send_cmd(command_pipe, socket_handle, 0, REPLY_BYTES)
                last_sent_ts_by_sock[socket_handle] = now
                print(f"[logic] matched on sock=0x{socket_handle:X}, auto-sent {len(REPLY_BYTES)} bytes")


if __name__ == "__main__":
    main()
