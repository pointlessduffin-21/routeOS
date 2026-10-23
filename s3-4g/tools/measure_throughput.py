"""Measure how fast the board actually pushes pixels, using the firmware's own byte counter."""
import base64
import json
import os
import socket
import struct
import sys
import threading
import time
import urllib.request

HOST = sys.argv[1] if len(sys.argv) > 1 else "192.168.254.125"


def health():
    with urllib.request.urlopen(f"http://{HOST}/health", timeout=8) as response:
        return json.load(response)


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


def hold_viewer(stop):
    sock = socket.create_connection((HOST, 81), timeout=15)
    key = base64.b64encode(os.urandom(16)).decode()
    sock.sendall(("GET /rvdp HTTP/1.1\r\nHost: h\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
                  "Sec-WebSocket-Key: %s\r\nSec-WebSocket-Version: 13\r\n\r\n" % key).encode())
    head = b""
    while b"\r\n\r\n" not in head:
        head += sock.recv(1)
    hello = bytes([1, 0, 1, 1]) + struct.pack(">I", 3) + struct.pack(">I", 16384) + bytes([1])
    sock.sendall(ws_frame(0x2, packet(1, 0, hello)))
    sock.settimeout(1.0)
    sequence = 1
    while not stop.is_set():
        try:
            chunk = sock.recv(65536)
        except socket.timeout:
            sock.sendall(ws_frame(0x2, packet(8, sequence, struct.pack(">I", int(time.time()) & 0xFFFFFFFF))))
            sequence += 1
            continue
        if not chunk:
            break
    sock.close()


stop = threading.Event()
worker = threading.Thread(target=hold_viewer, args=(stop,), daemon=True)
worker.start()

previous = health()["bytes"]
start = time.time()
for _ in range(14):
    time.sleep(1)
    sample = health()
    sent = sample["bytes"] - previous
    previous = sample["bytes"]
    print(f"{time.time() - start:5.1f}s  +{sent:7d} B  {sent / 1024:6.1f} KB  queue {sample['queue']}")
stop.set()
worker.join(timeout=5)
