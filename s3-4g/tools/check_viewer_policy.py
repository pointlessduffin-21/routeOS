"""Check the viewer policy: the first viewer keeps the screen, the second is told it is busy."""
import base64
import os
import socket
import struct
import sys
import time

HOST = sys.argv[1] if len(sys.argv) > 1 else "192.168.254.125"


def connect():
    sock = socket.create_connection((HOST, 81), timeout=10)
    key = base64.b64encode(os.urandom(16)).decode()
    sock.sendall(("GET /rvdp HTTP/1.1\r\nHost: h\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
                  "Sec-WebSocket-Key: %s\r\nSec-WebSocket-Version: 13\r\n\r\n" % key).encode())
    head = b""
    while b"\r\n\r\n" not in head:
        head += sock.recv(1)
    return sock, head.split(b"\r\n")[0].decode(errors="replace")


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


hello = bytes([1, 0, 1, 1]) + struct.pack(">I", 3) + struct.pack(">I", 16384) + bytes([1])

first, first_head = connect()
first.sendall(ws_frame(0x2, packet(1, 0, hello)))
print("viewer A:", first_head)

second, second_head = connect()
second.sendall(ws_frame(0x2, packet(1, 0, hello)))
print("viewer B:", second_head)

second.settimeout(4)
try:
    reply = second.recv(200)
except socket.timeout:
    reply = b"(silence)"
kind = reply[6] if len(reply) > 6 else None
print("viewer B got packet type", kind, "(9=ERROR expected)")

first.settimeout(3)
try:
    data = first.recv(4096)
    print("viewer A still receiving:", len(data), "bytes")
except socket.timeout:
    print("viewer A got nothing in 3 s")

first.close()
second.close()
