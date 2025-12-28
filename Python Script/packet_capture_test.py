import struct
import socket
import win32file

PIPE_NAME = r"\\.\pipe\PacketsCorePipe"

MAGIC = b"PKT2"
HDR_FMT = "<4sHBBIQIHIH"  # 32 bytes
HDR_SIZE = struct.calcsize(HDR_FMT)


def read_exact(handle, n: int) -> bytes:
    buf = b""
    while len(buf) < n:
        _, chunk = win32file.ReadFile(handle, n - len(buf))
        if not chunk:
            raise EOFError("pipe closed")
        buf += chunk
    return buf


def ipv4_to_str(ip_net_order_u32: int) -> str:
    return socket.inet_ntoa(struct.pack("!I", ip_net_order_u32))


def main():
    print("Waiting for DLL pipe ...")

    handle = win32file.CreateFile(
        PIPE_NAME,
        win32file.GENERIC_READ,
        0,
        None,
        win32file.OPEN_EXISTING,
        0,
        None,
    )

    print("Connected!")

    while True:
        hdr = read_exact(handle, HDR_SIZE)
        magic, ver, packet_type, address_family, payload_len, socket_handle, local_ip, local_port, remote_ip, remote_port = struct.unpack(HDR_FMT, hdr)

        if magic != MAGIC:
            raise RuntimeError(f"bad magic: {magic!r}")
        if ver != 2:
            raise RuntimeError(f"unexpected ver: {ver}")

        payload = read_exact(handle, payload_len) if payload_len else b""

        tag = "SEND" if packet_type == 1 else "RECV" if packet_type == 2 else f"TYPE_{packet_type}"
        l = f"{ipv4_to_str(local_ip)}:{local_port}" if local_ip != 0 and local_port != 0 else "-"
        r = f"{ipv4_to_str(remote_ip)}:{remote_port}" if remote_ip != 0 and remote_port != 0 else "-"

        print(f"[{tag}] {payload_len} bytes sock=0x{socket_handle:X} {l} -> {r}")
        if payload:
            print(" ".join(f"{b:02X}" for b in payload))


if __name__ == "__main__":
    main()
