"""Short RVDP probe: handshake, then print every packet for a while.

Answers one question the soak could not: is the link alive when no frames arrive?
The screen is static most of the time, so the only liveness signal is PING/PONG.
"""
import base64
import os
import socket
import struct
import sys
import time

HOST = sys.argv[1] if len(sys.argv) > 1 else "192.168.254.125"
SECONDS = float(sys.argv[2]) if len(sys.argv) > 2 else 90.0

NAMES = {1: "HELLO", 2: "DISPLAY_INFO", 3: "RECT", 4: "FRAME_END", 5: "RESYNC", 6: "POINTER",
         7: "KEY", 8: "PING", 9: "PONG", 10: "ERROR", 11: "CONTROL_STATE"}


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


def packet(kind, sequence, payload, minor=0):
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


def note(text):
    print(f"{time.strftime('%H:%M:%S')} {text}", flush=True)


def main():
    sock = socket.create_connection((HOST, 81), timeout=15)
    key = base64.b64encode(os.urandom(16)).decode()
    sock.sendall(("GET /rvdp HTTP/1.1\r\nHost: h\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
                  "Sec-WebSocket-Key: %s\r\nSec-WebSocket-Version: 13\r\n\r\n" % key).encode())
    head = b""
    while b"\r\n\r\n" not in head:
        head += sock.recv(1)
    note(head.split(b"\r\n")[0].decode(errors="replace"))
    sock.settimeout(0.5)

    opened = time.time()
    deadline = opened + SECONDS
    sequence = 0
    peerMinor = 0

    hello = bytes([1, 0, 1, 1]) + struct.pack(">I", 3) + struct.pack(">I", 16384) + bytes([1])
    sock.sendall(ws_frame(0x2, packet(1, sequence, hello, minor=0)))
    sequence += 1

    buffer = bytearray()
    pending = {}
    last_rx = opened
    last_ping = opened
    counts = {}
    frames = 0
    total = 0

    try:
        while time.time() < deadline:
            now = time.time()
            if now - last_ping >= 5:
                nonce = int(now * 1000) & 0xFFFFFFFF
                pending[nonce] = now
                sock.sendall(ws_frame(0x2, packet(8, sequence, struct.pack(">I", nonce), minor=peerMinor)))
                sequence += 1
                last_ping = now
            try:
                chunk = sock.recv(65536)
            except socket.timeout:
                continue
            if not chunk:
                note(f"socket closed by the board after {now - opened:.1f}s, {total} bytes")
                break
            last_rx = now
            total += len(chunk)
            buffer += chunk
            while True:
                got = take(buffer)
                if got is None:
                    break
                opcode, data = got
                if opcode == 0x9:
                    sock.sendall(ws_frame(0xA, data))
                    continue
                if opcode == 0x8:
                    note("close frame from the board")
                    return
                if opcode != 0x2 or len(data) < 16 or data[:4] != b"RVDP":
                    continue
                kind = data[6]
                payload = data[16:]
                counts[kind] = counts.get(kind, 0) + 1
                if kind == 2:
                    peerMinor = data[5]
                    w, h = struct.unpack(">HH", payload[:4])
                    note(f"DISPLAY_INFO {w}x{h} minor={peerMinor} session={struct.unpack('>I', payload[6:10])[0]}")
                elif kind == 4:
                    frames += 1
                    note(f"FRAME_END {frames} rects={struct.unpack('>H', payload[4:6])[0]} full={payload[6]} t={now - opened:.1f}s")
                elif kind == 9:
                    nonce = struct.unpack(">I", payload[:4])[0]
                    sent = pending.pop(nonce, None)
                    rtt = (now - sent) * 1000 if sent else -1
                    note(f"PONG rtt={rtt:.0f}ms")
                elif kind == 10:
                    note(f"ERROR {struct.unpack('>H', payload[:2])[0]} {payload[2:].decode(errors='replace')}")
                elif kind == 11:
                    note(f"CONTROL_STATE granted={payload[0]}")
    finally:
        note(f"counts {({NAMES.get(k, k): v for k, v in counts.items()})}, frames {frames}, {total / 1024:.0f} KB, last rx {time.time() - last_rx:.1f}s ago")
        sock.close()


if __name__ == "__main__":
    main()
