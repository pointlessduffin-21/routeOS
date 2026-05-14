#!/usr/bin/env python3
"""
EcoRoute ESP32 Serial Monitor

Reads the ESP32 serial output (USB), parses telemetry lines,
and optionally logs to a file. Auto-detects the ESP32 port on macOS/Linux.

Usage:
    python3 serial-monitor.py                     # auto-detect port
    python3 serial-monitor.py --port /dev/ttyUSB0 # explicit port
    python3 serial-monitor.py --baud 115200       # custom baud rate
    python3 serial-monitor.py --log telemetry.log # log to file
    python3 serial-monitor.py --json              # output parsed JSON
    python3 serial-monitor.py --list              # list available ports
"""

import argparse
import json
import re
import sys
import time
from datetime import datetime

try:
    import serial
    import serial.tools.list_ports
except ImportError:
    print("ERROR: pyserial not installed. Run: pip3 install pyserial")
    sys.exit(1)


# ── Port detection ────────────────────────────────────────────────────────────

ESP32_VID_PIDS = [
    (0x10C4, 0xEA60),   # CP2102 (common ESP32 devkit)
    (0x1A86, 0x7523),   # CH340 (cheap clones)
    (0x1A86, 0x55D4),   # CH9102
    (0x303A, 0x1001),   # ESP32-S2 native USB
    (0x303A, 0x0002),   # ESP32-S3 native USB
    (0x0403, 0x6001),   # FTDI FT232
    (0x0403, 0x6015),   # FTDI FT231X
]


def list_ports():
    """List all available serial ports with details."""
    ports = serial.tools.list_ports.comports()
    if not ports:
        print("No serial ports found.")
        return []

    print(f"\n{'Port':<25} {'Description':<35} {'VID:PID':<12} {'ESP32?'}")
    print("─" * 80)

    for p in sorted(ports, key=lambda x: x.device):
        vid_pid = f"{p.vid:04X}:{p.pid:04X}" if p.vid else "N/A"
        is_esp = "  ✓" if p.vid and any(
            p.vid == v and p.pid == d for v, d in ESP32_VID_PIDS
        ) else ""
        print(f"{p.device:<25} {(p.description or 'N/A'):<35} {vid_pid:<12}{is_esp}")

    print()
    return ports


def find_esp32_port() -> str | None:
    """Auto-detect ESP32 serial port."""
    ports = serial.tools.list_ports.comports()

    # First pass: match known VID/PID
    for p in ports:
        if p.vid and any(p.vid == v and p.pid == d for v, d in ESP32_VID_PIDS):
            return p.device

    # Second pass: match common names
    for p in ports:
        dev = p.device.lower()
        if any(x in dev for x in ["ttyusb", "ttyacm", "cu.slab", "cu.usbserial", "cu.wchusbserial"]):
            return p.device

    return None


# ── Telemetry parser ──────────────────────────────────────────────────────────

TELEMETRY_PATTERNS = {
    "distance":   re.compile(r"\[(?:sensor|ultrasonic)\]\s+Distance:\s+([\d.-]+)\s*cm"),
    "fill":       re.compile(r"\[(?:sensor|ultrasonic)\]\s+Fill:\s+([\d.-]+)\s*%"),
    "battery_v":  re.compile(r"\[battery\]\s+(?:Voltage:\s+)?([\d.]+)\s*V"),
    "battery_pct": re.compile(r"\((\d+)%\)"),
    "rssi":       re.compile(r"\[wifi\]\s+RSSI:\s+([-\d]+)"),
    "mqtt_status": re.compile(r"\[mqtt\]\s+(?:Status:\s+)?(\w+)"),
    "lid":        re.compile(r"\[ir\]\s+(?:Lid:\s+)?(\w+)"),
    "adaptive":   re.compile(r"\[adaptive\]\s+Next in:\s+(\d+)\s*sec"),
}


def parse_telemetry_line(line: str) -> dict | None:
    """Try to extract structured data from a serial line."""
    result = {}
    for key, pattern in TELEMETRY_PATTERNS.items():
        m = pattern.search(line)
        if m:
            val = m.group(1)
            if key in ("distance", "fill", "battery_v"):
                result[key] = float(val)
            elif key in ("rssi", "battery_pct", "adaptive"):
                result[key] = int(val)
            else:
                result[key] = val
    return result if result else None


# ── Color helpers ─────────────────────────────────────────────────────────────

class C:
    """ANSI color codes."""
    RESET   = "\033[0m"
    BOLD    = "\033[1m"
    DIM     = "\033[2m"
    RED     = "\033[31m"
    GREEN   = "\033[32m"
    YELLOW  = "\033[33m"
    BLUE    = "\033[34m"
    MAGENTA = "\033[35m"
    CYAN    = "\033[36m"
    WHITE   = "\033[37m"
    GRAY    = "\033[90m"


def colorize_line(line: str) -> str:
    """Apply color to known log prefixes."""
    ts = f"{C.GRAY}{datetime.now().strftime('%H:%M:%S')}{C.RESET} "

    if line.startswith("═") or line.startswith("╔") or line.startswith("╚"):
        return ts + C.CYAN + line + C.RESET
    if line.startswith("─"):
        return ts + C.GRAY + line + C.RESET

    # Color by tag
    tag_colors = {
        "[wifi]":      C.BLUE,
        "[mqtt]":      C.MAGENTA,
        "[sensor]":    C.GREEN,
        "[ultrasonic]": C.GREEN,
        "[battery]":   C.YELLOW,
        "[ir]":        C.CYAN,
        "[adaptive]":  C.GRAY,
        "[cal]":       C.CYAN,
        "[self-test]": C.BLUE,
        "[config]":    C.GRAY,
        "[ap]":        C.BLUE,
        "[web]":       C.MAGENTA,
        "[batt]":      C.YELLOW,
    }

    for tag, color in tag_colors.items():
        if tag in line:
            return ts + color + line + C.RESET

    # Highlight errors/warnings
    lower = line.lower()
    if "error" in lower or "failed" in lower or "critical" in lower:
        return ts + C.RED + line + C.RESET
    if "warning" in lower or "warn" in lower:
        return ts + C.YELLOW + line + C.RESET

    return ts + line


# ── Main ──────────────────────────────────────────────────────────────────────

def main():
    parser = argparse.ArgumentParser(
        description="EcoRoute ESP32 Serial Monitor",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="""
Examples:
  python3 serial-monitor.py                          # auto-detect + monitor
  python3 serial-monitor.py --list                   # list ports
  python3 serial-monitor.py --port /dev/cu.usbserial-0001
  python3 serial-monitor.py --json                   # parsed JSON output
  python3 serial-monitor.py --log readings.log       # log to file
        """,
    )
    parser.add_argument("--port", "-p", help="Serial port (auto-detect if omitted)")
    parser.add_argument("--baud", "-b", type=int, default=115200, help="Baud rate (default: 115200)")
    parser.add_argument("--list", "-l", action="store_true", help="List available serial ports")
    parser.add_argument("--log", help="Log output to file")
    parser.add_argument("--json", "-j", action="store_true", help="Output parsed telemetry as JSON")
    parser.add_argument("--raw", action="store_true", help="Raw output (no colors, no timestamps)")
    parser.add_argument("--reconnect", action="store_true", default=True, help="Auto-reconnect on disconnect")

    args = parser.parse_args()

    if args.list:
        list_ports()
        return

    # Resolve port
    port = args.port
    if not port:
        print(f"{C.CYAN}Scanning for ESP32 devices...{C.RESET}")
        port = find_esp32_port()
        if not port:
            print(f"{C.RED}No ESP32 found. Available ports:{C.RESET}")
            list_ports()
            print(f"Specify manually with: python3 serial-monitor.py --port /dev/ttyXXX")
            sys.exit(1)
        print(f"{C.GREEN}Found ESP32 at: {port}{C.RESET}")

    # Open log file
    log_file = None
    if args.log:
        log_file = open(args.log, "a", encoding="utf-8")
        print(f"{C.GRAY}Logging to: {args.log}{C.RESET}")

    # Banner
    print()
    print(f"{C.CYAN}╔══════════════════════════════════════════════════╗{C.RESET}")
    print(f"{C.CYAN}║     EcoRoute ESP32 Serial Monitor               ║{C.RESET}")
    print(f"{C.CYAN}╚══════════════════════════════════════════════════╝{C.RESET}")
    print()
    print(f"  Port:      {C.BOLD}{port}{C.RESET}")
    print(f"  Baud:      {args.baud}")
    print(f"  Mode:      {'JSON' if args.json else 'Raw' if args.raw else 'Colored'}")
    if args.log:
        print(f"  Log file:  {args.log}")
    print()
    print(f"  {C.DIM}Press Ctrl+C to stop.{C.RESET}")
    print()

    while True:
        try:
            ser = serial.Serial(port, args.baud, timeout=1)
            print(f"{C.GREEN}Connected to {port} @ {args.baud} baud{C.RESET}")
            print(f"{C.GRAY}{'─' * 60}{C.RESET}")

            while True:
                try:
                    raw = ser.readline()
                    if not raw:
                        continue

                    try:
                        line = raw.decode("utf-8", errors="replace").rstrip()
                    except Exception:
                        line = raw.decode("ascii", errors="replace").rstrip()

                    if not line:
                        continue

                    # Log to file
                    if log_file:
                        ts = datetime.now().isoformat()
                        log_file.write(f"[{ts}] {line}\n")
                        log_file.flush()

                    # JSON mode: parse and print structured data
                    if args.json:
                        parsed = parse_telemetry_line(line)
                        if parsed:
                            parsed["_timestamp"] = datetime.now().isoformat()
                            parsed["_raw"] = line
                            print(json.dumps(parsed))
                        continue

                    # Raw mode
                    if args.raw:
                        print(line)
                        continue

                    # Colored mode (default)
                    print(colorize_line(line))

                except serial.SerialException:
                    print(f"\n{C.RED}Serial connection lost!{C.RESET}")
                    ser.close()
                    break

        except serial.SerialException as e:
            print(f"{C.RED}Cannot open {port}: {e}{C.RESET}")
            if not args.reconnect:
                sys.exit(1)
            print(f"{C.YELLOW}Reconnecting in 3s...{C.RESET}")
            time.sleep(3)

        except KeyboardInterrupt:
            print(f"\n{C.CYAN}Monitor stopped.{C.RESET}")
            if log_file:
                log_file.close()
            try:
                ser.close()
            except Exception:
                pass
            break


if __name__ == "__main__":
    main()
