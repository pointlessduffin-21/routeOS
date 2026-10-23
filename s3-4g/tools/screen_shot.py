"""Reconstruct the remote 480x800 screen from RVDP rectangles and save it as a PNG."""
import base64
import io
import os
import socket
import struct
import sys
import time

from PIL import Image

HOST = sys.argv[1] if len(sys.argv) > 1 else "100.70.23.100"
OUT = sys.argv[2] if len(sys.argv) > 2 else "screen.png"
WIDTH, HEIGHT = 480, 800


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


def take(buffer):
    if len(buffer) < 2:
        return None
    opcode = buffer[0] & 0x0F
    length = buffer[1] & 0x7F
    at = 2
    if length == 126:
        if len(buffer) < 4:
            return None
        length = struct.unpack(">H", bytes(buffer[2:4]))[0]
        at = 4
    elif length == 127:
        if len(buffer) < 10:
            return None
        length = struct.unpack(">Q", bytes(buffer[2:10]))[0]
        at = 10
    if len(buffer) < at + length:
        return None
    payload = bytes(buffer[at:at + length])
    del buffer[:at + length]
    return opcode, payload


def main():
    sock = socket.create_connection((HOST, 81), timeout=30)
    key = base64.b64encode(os.urandom(16)).decode()
    sock.sendall(("GET /rvdp HTTP/1.1\r\nHost: %s:81\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
                  "Sec-WebSocket-Key: %s\r\nSec-WebSocket-Version: 13\r\n\r\n" % (HOST, key)).encode())
    reply = b""
    while b"\r\n\r\n" not in reply:
        reply += sock.recv(1)
    print("handshake:", reply.split(b"\r\n")[0].decode(errors="replace"))
    sock.settimeout(5)

    sequence = 0
    hello = bytes([1, 0, 1, 1]) + struct.pack(">I", 3) + struct.pack(">I", 16384) + bytes([1])
    sock.sendall(ws_frame(0x2, packet(1, sequence, hello)))
    sequence += 1

    screen = Image.new("RGB", (WIDTH, HEIGHT), (0, 0, 0))
    started = time.time()
    buffer = bytearray()
    rectangles = 0
    while time.time() - started < 30:
        try:
            chunk = sock.recv(65536)
        except socket.timeout:
            continue
        if not chunk:
            break
        buffer += chunk
        while True:
            got = take(buffer)
            if got is None:
                break
            opcode, data = got
            if opcode == 0x9:
                sock.sendall(ws_frame(0xA, data))
                continue
            if opcode != 0x2 or len(data) < 16 or data[:4] != b"RVDP":
                continue
            kind, body = data[6], data[16:]
            if kind == 4:
                print(f"frame end after {rectangles} rectangles at {time.time() - started:.1f}s")
                screen.save(OUT)
                print("saved", OUT)
                return 0
            if kind != 3 or len(body) < 17:
                continue
            x = struct.unpack(">H", body[4:6])[0]
            y = struct.unpack(">H", body[6:8])[0]
            w = struct.unpack(">H", body[8:10])[0]
            h = struct.unpack(">H", body[10:12])[0]
            encoding = body[12]
            length = struct.unpack(">I", body[13:17])[0]
            payload = body[17:17 + length]
            rectangles += 1
            if encoding == 2:
                band = Image.open(io.BytesIO(payload)).convert("RGB")
                screen.paste(band, (x, y))
            else:
                band = Image.frombytes("RGB", (w, h), payload, "raw", "BGR;16")
                screen.paste(band, (x, y))
    print("no frame end; saving partial")
    screen.save(OUT)
    return 1


if __name__ == "__main__":
    sys.exit(main())
