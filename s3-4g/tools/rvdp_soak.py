"""Multi-hour RVDP reliability soak for the ESP32-P4 display.

One viewer slot, one session, hours long. It tests both things a viewer needs: the picture keeps coming, and a tap on
the picture still does something.

  session survived?     the WebSocket stays open and PING/PONG keeps answering
  screen still renders? a RESYNC every five minutes must produce a full repaint, timed
  taps still work?      a tap on the keypad must change the screen, so a repaint must follow within seconds
  memory holding up?    /api/capabilities heap free and largest block, sampled every minute
  radio holding up?     /api/system RSSI and channel, when the management token is supplied
  board still alive?    uptime_s must keep climbing; a drop to zero is a reboot

The tap coordinates are the calculator keypad on the 800x1280 panel: "5" and "AC", in the keypad matrix laid out by
layout.cpp at (32,446) size (736,790), five columns and five rows with 14 px gaps. Nothing the soak presses is
destructive - digits and clear only rewrite the expression - and the soak never leaves that page.

Usage: rvdp_soak.py [host] [hours] [logfile] [token]
"""
import base64
import json
import os
import socket
import struct
import sys
import time
import urllib.request

HOST = sys.argv[1] if len(sys.argv) > 1 else "192.168.254.125"
HOURS = float(sys.argv[2]) if len(sys.argv) > 2 else 4.0
LOG_PATH = sys.argv[3] if len(sys.argv) > 3 else "soak.log"
TOKEN = sys.argv[4] if len(sys.argv) > 4 else "ec9b7a701ecba6c02a09448c646eb053"

WS_PORT = 81
HTTP_PORT = 80
PING_EVERY = float(os.environ.get("SOAK_PING", 3.0))         # keeps lastRx fresh, so the board never calls us stale
HEALTH_EVERY = float(os.environ.get("SOAK_HEALTH", 60.0))
REPAINT_EVERY = float(os.environ.get("SOAK_REPAINT", 300.0))
INPUT_EVERY = float(os.environ.get("SOAK_INPUT", 300.0))
STALL_LIMIT = 15.0      # under the board's own 20 s idle timeout, so a stall is logged before the board acts on it
REPAINT_LIMIT = 90.0    # a full screen must arrive well inside this
TAP_LIMIT = 25.0        # a tap that changes the page must be visible well inside this; it takes seconds, not ms
RECONNECT_PAUSE = 5.0
BUSY_PAUSE = 25.0       # the board frees a silent viewer only after its own 20 s idle timeout

KEYPAD = {"left": 32, "top": 446, "width": 736, "height": 790, "gap": 14, "columns": 5, "rows": 5}


def key_center(column, row):
    cell_w = (KEYPAD["width"] - KEYPAD["gap"] * (KEYPAD["columns"] - 1)) / KEYPAD["columns"]
    cell_h = (KEYPAD["height"] - KEYPAD["gap"] * (KEYPAD["rows"] - 1)) / KEYPAD["rows"]
    x = KEYPAD["left"] + column * (cell_w + KEYPAD["gap"]) + cell_w / 2
    y = KEYPAD["top"] + row * (cell_h + KEYPAD["gap"]) + cell_h / 2
    return int(x), int(y)


DIGIT = key_center(1, 2)      # "5", used once to focus the expression field so the panel runs under load
CLEAR = key_center(0, 0)      # "AC"

# Header navigation buttons (nav() in layout.cpp: y 22, height 64). Tapping these changes the whole screen, so a
# repaint after one is unambiguous - unlike the keypad, whose cursor blinks and keeps the panel redrawing anyway.
HISTORY_BUTTON = (434 + 190 // 2, 22 + 32)     # calculator page: HISTORY at x 434, width 190
CALC_BUTTON = (424 + 130 // 2, 22 + 32)        # history page: CALC at x 424, width 130
AREA_BIG = 300000       # a page switch paints most of the 800x1280 screen; a cursor or a button covers a few KB


def note(text, handle=None):
    line = f"{time.strftime('%Y-%m-%d %H:%M:%S')} {text}"
    print(line, flush=True)
    if handle:
        handle.write(line + "\n")
        handle.flush()


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


def fetch(path):
    request = urllib.request.Request(f"http://{HOST}:{HTTP_PORT}{path}")
    if TOKEN:
        request.add_header("Authorization", "Bearer " + TOKEN)
    with urllib.request.urlopen(request, timeout=8) as response:
        return json.loads(response.read().decode())


class Health:
    """One sample of everything the board will say about itself."""

    def __init__(self):
        self.uptime = None
        self.reset = None
        self.reboots = 0
        self.free = []
        self.largest = []
        self.rvdp = None

    def sample(self, handle, csv):
        caps = system = None
        try:
            caps = fetch("/api/capabilities")
        except Exception as exc:                                  # noqa: BLE001 - a failure is only a missing sample
            note(f"health: capabilities unavailable ({exc})", handle)
        try:
            system = fetch("/api/system")
        except Exception:                                          # noqa: BLE001
            system = None
        if not caps:
            return
        memory = caps.get("memory", {})
        free = memory.get("heap_free_kb")
        largest = memory.get("largest_block_kb")
        uptime = caps.get("uptime_s")
        reset = caps.get("reset_reason")
        rssi = channel = ""
        if system and "wifi" in system:
            rssi = system["wifi"].get("rssi", "")
            channel = system["wifi"].get("channel", "")
        if free is not None:
            self.free.append(free)
        if largest is not None:
            self.largest.append(largest)
        if self.uptime is not None and uptime is not None and uptime < self.uptime - 60:
            self.reboots += 1
            note(f"BOARD REBOOTED: uptime fell from {self.uptime}s to {uptime}s, reset reason {reset}", handle)
        self.uptime = uptime
        self.reset = reset
        # The board's own view of the transport: how many viewers it lost, how often the sender wedged, how often a
        # write failed. A soak that only sees its own side cannot tell "the board dropped me" from "the link died".
        wedges = send_fails = viewer_drops = queue = ""
        try:
            local = fetch("/health")
            wedges = local.get("wedges", "")
            send_fails = local.get("send_fails", "")
            viewer_drops = local.get("viewer_drops", "")
            queue = local.get("queue", "")
            self.rvdp = local
        except Exception:                                          # noqa: BLE001
            pass
        csv.write(f"{int(time.time())},{free},{largest},{uptime},{rssi},{channel},"
                  f"{wedges},{send_fails},{viewer_drops},{queue}\n")
        csv.flush()


def board_logs(handle, wanted=12):
    """The board keeps its own log in RAM; after a drop it is the only account of what happened."""
    try:
        payload = fetch("/api/logs")
    except Exception as exc:                                      # noqa: BLE001
        note(f"    board log unavailable ({exc})", handle)
        return
    entries = [e for e in payload.get("lines", []) if e.get("l") in ("W", "E")]
    note(f"    board log (uptime {payload.get('uptime_s', '?')}s), last {min(wanted, len(entries))} warnings/errors:",
         handle)
    for entry in entries[-wanted:]:
        note(f"      [{entry.get('l')}] {entry.get('m')}", handle)


def session(deadline, state, handle):
    """One WebSocket session. Returns (ending, opened, frames) where ending says why it closed."""
    sock = socket.create_connection((HOST, WS_PORT), timeout=20)
    key = base64.b64encode(os.urandom(16)).decode()
    sock.sendall(("GET /rvdp HTTP/1.1\r\nHost: %s\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
                  "Sec-WebSocket-Key: %s\r\nSec-WebSocket-Version: 13\r\n\r\n" % (HOST, key)).encode())
    head = b""
    while b"\r\n\r\n" not in head:
        chunk = sock.recv(1)
        if not chunk:
            raise ConnectionError("closed during the handshake")
        head += chunk
    if b"101" not in head.split(b"\r\n")[0]:
        raise ConnectionError(head.split(b"\r\n")[0].decode(errors="replace"))
    sock.settimeout(0.25)

    opened = time.time()
    sequence = state["sequence"]
    peer_minor = 0
    granted = False
    buffer = bytearray()
    last_ping = last_rx = opened
    last_health = 0.0
    next_resync = opened + REPAINT_EVERY
    next_input = opened + INPUT_EVERY * 0.5       # the first tap test comes early, so a broken one shows up fast
    queue = [("tap history", HISTORY_BUTTON[0], HISTORY_BUTTON[1]),
             ("tap calc", CALC_BUTTON[0], CALC_BUTTON[1])]
    pending = None                                # (label, t0) of the repaint we are waiting for
    taps = state["taps"]
    frames = 0
    seen_info = False
    nonces = {}

    def send(kind, payload):
        nonlocal sequence
        sock.sendall(ws_frame(0x2, packet(kind, sequence, payload, peer_minor)))
        sequence += 1

    def tap(x, y):
        """A real click: move, press, release. The panel releases a press left alone for two seconds."""
        send(6, struct.pack(">HHBB", x, y, 0, 0))
        send(6, struct.pack(">HHBB", x, y, 1, 1))
        time.sleep(0.05)
        send(6, struct.pack(">HHBB", x, y, 0, 2))

    send(1, bytes([1, 0, 1, 1]) + struct.pack(">I", 3) + struct.pack(">I", 16384) + bytes([1]))
    focus_sent = False

    ending = "deadline"
    while time.time() < deadline:
        now = time.time()

        if now - last_ping >= PING_EVERY:
            nonce = int(now * 1000) & 0xFFFFFFFF
            nonces[nonce] = now
            try:
                send(8, struct.pack(">I", nonce))
            except OSError:
                ending = "send failed"
                break
            last_ping = now
            if now - last_rx > STALL_LIMIT:
                ending = f"silent for {now - last_rx:.0f}s"
                break

        if granted and not focus_sent:
            # One tap on the expression field, so the panel is in the state a real user leaves it in: a focused
            # text field, a blinking cursor, and no idle time between frames. Not measured - it is setup.
            try:
                tap(*DIGIT)
            except OSError:
                ending = "send failed"
                break
            focus_sent = True
            note("tapped the expression field, so the panel now redraws continuously", handle)

        if pending is None and granted and queue:
            label, x, y = queue.pop(0)
            try:
                tap(x, y)
            except OSError:
                ending = "send failed"
                break
            pending = (f"{label} at {x},{y}", time.time())
            if not queue:
                next_input = now + INPUT_EVERY

        if pending is None and granted and now >= next_input and not queue:
            # A page switch there and back: two whole-screen changes nobody can mistake for a cursor blink.
            queue.extend([("tap history", HISTORY_BUTTON[0], HISTORY_BUTTON[1]),
                          ("tap calc", CALC_BUTTON[0], CALC_BUTTON[1])])

        if pending is None and now >= next_resync:
            try:
                send(5, bytes([2]))
            except OSError:
                ending = "send failed"
                break
            pending = ("resync", now)
            next_resync = now + REPAINT_EVERY

        if now - last_health >= HEALTH_EVERY:
            state["health"].sample(handle, state["csv"])
            last_health = now

        try:
            chunk = sock.recv(65536)
        except socket.timeout:
            if pending and time.time() - pending[1] > (TAP_LIMIT if "tap" in pending[0] else REPAINT_LIMIT):
                waited = time.time() - pending[1]
                note(f"{pending[0]}: NO REPAINT after {waited:.0f}s", handle)
                if "tap" in pending[0]:
                    taps["missing"] += 1
                pending = None
            continue
        except OSError as exc:
            ending = f"socket error {exc}"
            break
        if not chunk:
            ending = "closed by the board" if seen_info else "refused (display busy)"
            break
        last_rx = now
        state["bytes"] += len(chunk)
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
                ending = "close frame"
                break
            if opcode != 0x2 or len(data) < 16 or data[:4] != b"RVDP":
                continue
            kind = data[6]
            payload = data[16:]
            if kind == 2:
                seen_info = True
                peer_minor = data[5]
                width, height = struct.unpack(">HH", payload[:4])
                session_id = struct.unpack(">I", payload[6:10])[0]
                note(f"session open: {width}x{height}, minor {peer_minor}, device session {session_id}", handle)
            elif kind == 3:
                _, _, w, h = struct.unpack(">HHHH", payload[4:12])
                state["batch_area"] += w * h
            elif kind == 4:
                frames += 1
                state["frames"] += 1
                rects = struct.unpack(">H", payload[4:6])[0]
                full = payload[6]
                area = state["batch_area"]
                state["batch_area"] = 0
                if pending:
                    label, t0 = pending
                    elapsed = time.time() - t0
                    if label == "resync":
                        # Only a snapshot answers a resync; anything else is the panel's own housekeeping.
                        if full:
                            state["repaints"].append(elapsed)
                            state["rects"].append(rects)
                            note(f"full repaint {len(state['repaints'])}: {rects} rectangles in {elapsed:.1f}s", handle)
                            pending = None
                    elif area < AREA_BIG:
                        # A page switch is not always one full snapshot: a blinking cursor can start a batch first,
                        # so the switch arrives in pieces. Judge by how much of the screen the frame painted.
                        taps["partial"] += 1
                    else:
                        taps["answered"] += 1
                        taps["latency"].append(elapsed)
                        taps["area"].append(area)
                        note(f"{label}: new page in {elapsed:.1f}s ({rects} rectangles, {area} px)", handle)
                        pending = None
                elif full:
                    note(f"unsolicited full repaint: {rects} rectangles", handle)
            elif kind == 9:
                nonce = struct.unpack(">I", payload[:4])[0]
                sent = nonces.pop(nonce, None)
                if sent is not None:
                    state["rtt"].append((time.time() - sent) * 1000)
            elif kind == 10:
                code = struct.unpack(">H", payload[:2])[0]
                note(f"device error {code}: {payload[2:].decode(errors='replace')}", handle)
            elif kind == 11:
                granted = bool(payload[0])
                note(f"control {'granted' if granted else 'refused'}", handle)
        if ending == "close frame":
            break

    state["sequence"] = sequence
    try:
        sock.close()
    except OSError:
        pass
    return ending, opened, frames


def percentile(values, fraction):
    if not values:
        return 0.0
    ordered = sorted(values)
    index = min(len(ordered) - 1, int(fraction * (len(ordered) - 1)))
    return ordered[index]


def main():
    # One soak at a time. Two clients on a board that allows one viewer fight over the display, and the numbers from
    # that are worthless. Binding a local port is a lock that Windows releases the moment this process dies.
    lock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    try:
        lock.bind(("127.0.0.1", 45999))
        lock.listen(1)
    except OSError:
        print("another soak is already running on this machine; not starting a second one", flush=True)
        return 2

    started = time.time()
    deadline = started + HOURS * 3600
    handle = open(LOG_PATH, "a", encoding="utf-8", buffering=1)
    csv = open(os.path.splitext(LOG_PATH)[0] + "-health.csv", "a", encoding="utf-8", buffering=1)
    csv.write("# unix,heap_free_kb,largest_block_kb,uptime_s,rssi,channel,wedges,send_fails,viewer_drops,queue\n")

    state = {"sequence": 0, "bytes": 0, "frames": 0, "rtt": [], "repaints": [], "rects": [],
             "health": Health(), "csv": csv, "batch_area": 0,
             "taps": {"answered": 0, "missing": 0, "partial": 0, "latency": [], "area": []}}
    sessions = drops = 0
    longest = 0.0
    next_hour = 1
    refused = 0

    note(f"=== soak start: {HOST}:{WS_PORT}, {HOURS:.1f} h, log {LOG_PATH}", handle)
    note(f"    taps: history {HISTORY_BUTTON}, calc {CALC_BUTTON}, keypad digit {DIGIT}", handle)
    while time.time() < deadline:
        sessions += 1
        try:
            ending, opened, frames = session(deadline, state, handle)
        except (OSError, ConnectionError, ValueError) as exc:
            ending, opened, frames = f"connect failed: {exc}", time.time(), 0
        held = time.time() - opened
        longest = max(longest, held)
        busy = "busy" in ending or "refused" in ending
        unreachable = "connect failed" in ending
        refused = refused + 1 if unreachable else 0
        if ending == "deadline":
            note(f"session {sessions}: ran {held / 60:.1f} min, {frames} frames", handle)
        elif refused == 1 or (refused and refused % 20 == 0):
            # A board that is alive but not serving gets one line, then a reminder every twenty attempts, instead of
            # sixteen hundred identical lines that bury the moment things went wrong.
            note(f"session {sessions}: board not serving ({ending}) - attempt {refused} to reconnect", handle)
            board_logs(handle, wanted=6)
        elif refused:
            pass
        else:
            drops += 1
            note(f"session {sessions}: ENDED after {held / 60:.1f} min ({ending}), {frames} frames", handle)
            board_logs(handle)
        state["sequence"] = 0

        if time.time() - started > next_hour * 3600:
            next_hour += 1
            health = state["health"]
            taps = state["taps"]
            note(f"--- after {(time.time() - started) / 3600:.1f} h: {sessions} sessions, {drops} failures, "
                 f"{state['frames']} frames, {state['bytes'] / 1048576:.1f} MB, "
                 f"taps {taps['answered']} ok / {taps['missing']} missed, "
                 f"heap {health.free[-1] if health.free else '?'} KB, reboots {health.reboots} ---", handle)

        if time.time() < deadline:
            pause = BUSY_PAUSE if busy else RECONNECT_PAUSE
            if refused:
                pause = min(60.0, 5.0 * refused)      # back off while the board is not serving, so we do not spin
            time.sleep(pause)

    health = state["health"]
    taps = state["taps"]
    hours = (time.time() - started) / 3600
    note(f"=== soak end: {hours:.1f} h, {sessions} sessions, {drops} failures, longest {longest / 60:.1f} min, "
         f"{state['frames']} frames, {state['bytes'] / 1048576:.1f} MB, reboots {health.reboots}", handle)
    if state["repaints"]:
        note(f"    full repaint: {len(state['repaints'])} samples, "
             f"mean {sum(state['repaints']) / len(state['repaints']):.1f}s, "
             f"worst {max(state['repaints']):.1f}s", handle)
    note(f"    taps: {taps['answered']} changed the page, {taps['missing']} with no page change, "
         f"{taps['partial']} partial redraws", handle)
    if taps["latency"]:
        note(f"    tap to repaint: mean {sum(taps['latency']) / len(taps['latency']):.1f}s, "
             f"p95 {percentile(taps['latency'], 0.95):.1f}s, worst {max(taps['latency']):.1f}s", handle)
    if state["rtt"]:
        note(f"    ping: {len(state['rtt'])} samples, mean {sum(state['rtt']) / len(state['rtt']):.0f}ms, "
             f"p95 {percentile(state['rtt'], 0.95):.0f}ms, worst {max(state['rtt']):.0f}ms", handle)
    if health.free:
        note(f"    heap free: start {health.free[0]} KB, end {health.free[-1]} KB, low {min(health.free)} KB, "
             f"largest block low {min(health.largest)} KB", handle)
    note(f"    reset reason last seen: {health.reset}", handle)
    handle.close()
    csv.close()


if __name__ == "__main__":
    main()
