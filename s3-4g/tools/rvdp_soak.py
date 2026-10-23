"""Hold an RVDP session open and prove it survives.

Connects like a viewer, keeps the heartbeat going, and records every disconnect with how long it lasted. The point is
the summary at the end: one line per reconnect, plus totals, so "it stayed up for an hour" is a measurement instead of
an opinion.
"""
import base64
import os
import socket
import struct
import sys
import time

HOST = sys.argv[1] if len(sys.argv) > 1 else "192.168.254.125"
MINUTES = float(sys.argv[2]) if len(sys.argv) > 2 else 60.0
LOG = sys.argv[3] if len(sys.argv) > 3 else "soak.log"

PING_EVERY = 5.0          # the device drops a viewer after 20 s of silence
SILENCE_LIMIT = 60.0      # a gap this long counts as a broken session, not a slow frame


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


class Log:
    def __init__(self, path):
        self.handle = open(path, "a", encoding="utf-8", buffering=1)

    def write(self, text):
        stamp = time.strftime("%Y-%m-%d %H:%M:%S")
        line = f"{stamp} {text}"
        print(line, flush=True)
        self.handle.write(line + "\n")


def session(log, deadline, stats):
    sock = socket.create_connection((HOST, 81), timeout=20)
    key = base64.b64encode(os.urandom(16)).decode()
    sock.sendall(("GET /rvdp HTTP/1.1\r\nHost: %s:81\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
                  "Sec-WebSocket-Key: %s\r\nSec-WebSocket-Version: 13\r\n\r\n" % (HOST, key)).encode())
    reply = b""
    while b"\r\n\r\n" not in reply:
        reply += sock.recv(1)
    if b"101" not in reply.split(b"\r\n")[0]:
        raise ConnectionError("handshake refused: " + reply.split(b"\r\n")[0].decode("utf-8", "replace"))
    sock.settimeout(1.0)

    sequence = 0
    hello = bytes([1, 0, 1, 1]) + struct.pack(">I", 3) + struct.pack(">I", 16384) + bytes([1])
    sock.sendall(ws_frame(0x2, packet(1, sequence, hello)))
    sequence += 1

    buffer = bytearray()
    last_ping = last_rx = time.time()
    rects = frame_rects = 0
    log.write(f"session open (attempt {stats['connects']})")

    while time.time() < deadline:
        now = time.time()
        if now - last_ping >= PING_EVERY:
            sock.sendall(ws_frame(0x2, packet(8, sequence, struct.pack(">I", int(now) & 0xFFFFFFFF))))
            sequence += 1
            last_ping = now
        try:
            chunk = sock.recv(65536)
        except socket.timeout:
            if time.time() - last_rx > SILENCE_LIMIT:
                raise TimeoutError(f"nothing from the device for {SILENCE_LIMIT:.0f}s")
            continue
        if not chunk:
            raise ConnectionError("device closed the session")
        last_rx = time.time()
        stats["bytes"] += len(chunk)
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
            kind = data[6]
            if kind == 8:                      # PING from the device
                body = data[16:20]
                sock.sendall(ws_frame(0x2, packet(9, sequence, body)))
                sequence += 1
                continue
            if kind == 3:
                rects += 1
                frame_rects += 1
                stats["rects"] += 1
                continue
            if kind == 4:
                body = data[16:]
                if len(body) >= 7:
                    count = struct.unpack(">H", body[4:6])[0]
                    if count != frame_rects:
                        log.write(f"FRAME_END mismatch: says {count}, saw {frame_rects}")
                    stats["frames"] += 1
                frame_rects = 0
                continue
    return "deadline"


def main():
    log = Log(LOG)
    stats = {"connects": 0, "drops": 0, "bytes": 0, "rects": 0, "frames": 0}
    deadline = time.time() + MINUTES * 60
    started = time.time()
    log.write(f"soak start: {HOST}:81 for {MINUTES:.0f} min")
    while time.time() < deadline:
        stats["connects"] += 1
        opened = time.time()
        try:
            session(log, deadline, stats)
        except (OSError, ConnectionError, TimeoutError, ValueError) as exc:
            stats["drops"] += 1
            held = time.time() - opened
            log.write(f"session ended after {held / 60:.1f} min: {type(exc).__name__}: {exc}")
            if time.time() < deadline:
                time.sleep(5)
    elapsed = time.time() - started
    log.write(f"soak end: {elapsed / 60:.1f} min, connects={stats['connects']} drops={stats['drops']} "
              f"frames={stats['frames']} rects={stats['rects']} bytes={stats['bytes']}")
    return 0 if stats["drops"] == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
