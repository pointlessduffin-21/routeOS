"""Check that closing a viewer frees the display immediately, so the next one starts at once."""
import base64
import os
import socket
import struct
import sys
import time

HOST = sys.argv[1] if len(sys.argv) > 1 else "192.168.254.125"


def ws_frame(opcode, payload):
    mask = os.urandom(4)
    n = len(payload)
    head = bytes([0x80 | opcode])
    if n < 126:
        head += bytes([0x80 | n])
    elif n < 65536:
        head += bytes([0x80 | 126]) + struct.pack(">H", n)
    else:
        head += bytes([0x80 | 127]) + struct.pack(">Q", n)
    return head + mask + bytes(b ^ mask[i % 4] for i, b in enumerate(payload))


def packet(kind, sequence, payload, minor=1):
    return b"RVDP" + bytes([1, minor, kind, 0]) + struct.pack(">I", sequence) + struct.pack(">I", len(payload)) + payload


def open_viewer():
    sock = socket.create_connection((HOST, 81), timeout=10)
    key = base64.b64encode(os.urandom(16)).decode()
    sock.sendall(("GET /rvdp HTTP/1.1\r\nHost: h\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
                  "Sec-WebSocket-Key: %s\r\nSec-WebSocket-Version: 13\r\n\r\n" % key).encode())
    head = b""
    while b"\r\n\r\n" not in head:
        head += sock.recv(1)
    hello = bytes([1, 0, 1, 1]) + struct.pack(">I", 3) + struct.pack(">I", 16384) + bytes([1])
    sock.sendall(ws_frame(0x2, packet(1, 0, hello)))
    return sock


def wait_frame_end(sock, limit=40):
    """Return seconds until a FRAME_END arrives, or None."""
    started = time.time()
    sock.settimeout(1.0)
    buffer = bytearray()
    while time.time() - started < limit:
        try:
            chunk = sock.recv(65536)
        except socket.timeout:
            continue
        if not chunk:
            return None
        buffer += chunk
        at = 0
        while at + 2 <= len(buffer):
            length = buffer[at + 1] & 0x7F
            offset = 2
            if length == 126:
                if at + 4 > len(buffer):
                    break
                length = struct.unpack(">H", bytes(buffer[at + 2:at + 4]))[0]
                offset = 4
            elif length == 127:
                if at + 10 > len(buffer):
                    break
                length = struct.unpack(">Q", bytes(buffer[at + 2:at + 10]))[0]
                offset = 10
            if at + offset + length > len(buffer):
                break
            frame = bytes(buffer[at + offset:at + offset + length])
            at += offset + length
            if len(frame) > 16 and frame[6] == 4:
                return time.time() - started
        del buffer[:at]
    return None


first = open_viewer()
took = wait_frame_end(first)
print(f"viewer A: first frame in {took:.1f}s" if took else "viewer A: no frame")
first.close()
print("closed A, connecting B immediately")

second = open_viewer()
took = wait_frame_end(second)
print(f"viewer B: first frame in {took:.1f}s" if took else "viewer B: refused or no frame")
second.close()
