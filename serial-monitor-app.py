#!/usr/bin/env python3
"""
EcoRoute ESP32 Serial Monitor — Desktop App (Pygame)

A visual serial monitor for EcoRoute ESP32 smart bins.
Displays live sensor readings, parsed telemetry, and raw serial output
in a dark-themed dashboard.

Usage:
    python3 serial-monitor-app.py
"""

import pygame
import serial
import serial.tools.list_ports
import threading
import queue
import re
import time
import json
from datetime import datetime

# ═══════════════════════════════════════════════════════════════════════════════
#  CONSTANTS
# ═══════════════════════════════════════════════════════════════════════════════

APP_TITLE = "EcoRoute Serial Monitor"
WINDOW_W, WINDOW_H = 1100, 720
FPS = 30
BAUD_RATE = 115200
MAX_LOG_LINES = 200

# ESP32 known USB VID/PIDs
ESP32_VID_PIDS = [
    (0x10C4, 0xEA60), (0x1A86, 0x7523), (0x1A86, 0x55D4),
    (0x303A, 0x1001), (0x303A, 0x0002), (0x0403, 0x6001), (0x0403, 0x6015),
]

# ═══════════════════════════════════════════════════════════════════════════════
#  COLORS (Slate dark theme)
# ═══════════════════════════════════════════════════════════════════════════════

BG           = (15, 23, 42)      # slate-900
SURFACE      = (30, 41, 59)      # slate-800
SURFACE2     = (51, 65, 85)      # slate-700
BORDER       = (71, 85, 105)     # slate-600
TEXT         = (226, 232, 240)    # slate-200
TEXT_DIM     = (148, 163, 184)    # slate-400
TEXT_MUTED   = (100, 116, 139)   # slate-500

GREEN        = (34, 197, 94)     # green-500
GREEN_BG     = (5, 46, 22)       # green-950
RED          = (239, 68, 68)     # red-500
RED_BG       = (69, 10, 10)      # red-950
ORANGE       = (249, 115, 22)    # orange-500
ORANGE_BG    = (67, 20, 7)
YELLOW       = (234, 179, 8)     # yellow-500
BLUE         = (59, 130, 246)    # blue-500
BLUE_BG      = (23, 37, 84)
CYAN         = (6, 182, 212)     # cyan-500
MAGENTA      = (168, 85, 247)    # purple-500
INDIGO       = (79, 70, 229)     # indigo-500
WHITE        = (248, 250, 252)   # slate-50

# ═══════════════════════════════════════════════════════════════════════════════
#  TELEMETRY PARSER
# ═══════════════════════════════════════════════════════════════════════════════

PATTERNS = {
    "distance":  re.compile(r"\[(?:sensor|ultrasonic)\]\s+Distance:\s+([\d.-]+)"),
    "fill":      re.compile(r"\[(?:sensor|ultrasonic)\]\s+Fill:\s+([\d.-]+)"),
    "battery_v": re.compile(r"\[battery[^\]]*\]\s+(?:Voltage:\s+)?([\d.]+)\s*V"),
    "battery_p": re.compile(r"\((\d+)%\)"),
    "rssi":      re.compile(r"\[wifi[^\]]*\]\s+RSSI:\s+([-\d]+)"),
    "mqtt":      re.compile(r"\[mqtt\]\s+(?:Status:\s+)?(OK|FAIL|Connected|Failed)"),
    "lid":       re.compile(r"\[ir[^\]]*\]\s+(?:Lid:\s+)?(CLOSED|OPEN|closed|open)", re.IGNORECASE),
    "next_sec":  re.compile(r"\[adaptive\]\s+Next in:\s+(\d+)"),
}

def tag_color(line: str):
    """Return color for a log line based on its tag."""
    l = line.lower()
    if "[wifi]" in l or "[self-test]" in l or "[ap]" in l:  return BLUE
    if "[mqtt]" in l or "[web]" in l:    return MAGENTA
    if "[sensor]" in l or "[ultrasonic]" in l: return GREEN
    if "[battery]" in l or "[batt]" in l: return YELLOW
    if "[ir]" in l:                       return CYAN
    if "[adaptive]" in l or "[config]" in l: return TEXT_DIM
    if "[cal]" in l:                      return CYAN
    if "error" in l or "failed" in l or "critical" in l: return RED
    if "warning" in l or "warn" in l:     return ORANGE
    if line.startswith("═") or line.startswith("╔") or line.startswith("╚"): return CYAN
    if line.startswith("─"):              return SURFACE2
    return TEXT_DIM


# ═══════════════════════════════════════════════════════════════════════════════
#  SERIAL THREAD
# ═══════════════════════════════════════════════════════════════════════════════

class SerialThread:
    def __init__(self):
        self.port = None
        self.ser = None
        self.running = False
        self.thread = None
        self.line_queue = queue.Queue()
        self.connected = False
        self.error_msg = ""

    def scan_ports(self):
        """Return list of (device, description, is_esp32) tuples."""
        ports = serial.tools.list_ports.comports()
        result = []
        for p in sorted(ports, key=lambda x: x.device):
            is_esp = bool(p.vid and any(p.vid == v and p.pid == d for v, d in ESP32_VID_PIDS))
            result.append((p.device, p.description or "Unknown", is_esp))
        return result

    def auto_detect(self):
        """Find first ESP32 port."""
        for dev, desc, is_esp in self.scan_ports():
            if is_esp:
                return dev
        return None

    def connect(self, port: str):
        """Connect to a serial port in a background thread."""
        self.disconnect()
        self.port = port
        self.running = True
        self.error_msg = ""
        self.thread = threading.Thread(target=self._read_loop, daemon=True)
        self.thread.start()

    def disconnect(self):
        self.running = False
        if self.thread:
            self.thread.join(timeout=2)
            self.thread = None
        if self.ser and self.ser.is_open:
            self.ser.close()
        self.ser = None
        self.connected = False

    def _read_loop(self):
        try:
            self.ser = serial.Serial(self.port, BAUD_RATE, timeout=0.5)
            self.connected = True
            self.line_queue.put(("__STATUS__", f"Connected to {self.port}"))

            while self.running:
                try:
                    raw = self.ser.readline()
                    if raw:
                        line = raw.decode("utf-8", errors="replace").rstrip()
                        if line:
                            self.line_queue.put(("LINE", line))
                except serial.SerialException:
                    self.connected = False
                    self.line_queue.put(("__STATUS__", "Connection lost — retrying..."))
                    time.sleep(2)
                    try:
                        self.ser = serial.Serial(self.port, BAUD_RATE, timeout=0.5)
                        self.connected = True
                        self.line_queue.put(("__STATUS__", f"Reconnected to {self.port}"))
                    except Exception:
                        pass
        except Exception as e:
            self.error_msg = str(e)
            self.connected = False
            self.line_queue.put(("__STATUS__", f"Error: {e}"))


# ═══════════════════════════════════════════════════════════════════════════════
#  APP
# ═══════════════════════════════════════════════════════════════════════════════

class App:
    def __init__(self):
        pygame.init()
        self.screen = pygame.display.set_mode((WINDOW_W, WINDOW_H), pygame.RESIZABLE)
        pygame.display.set_caption(APP_TITLE)
        self.clock = pygame.time.Clock()

        # Fonts
        self.font_sm = pygame.font.SysFont("Menlo,Consolas,monospace", 12)
        self.font_md = pygame.font.SysFont("SF Pro Display,Helvetica Neue,Arial", 14)
        self.font_lg = pygame.font.SysFont("SF Pro Display,Helvetica Neue,Arial", 18, bold=True)
        self.font_xl = pygame.font.SysFont("SF Pro Display,Helvetica Neue,Arial", 36, bold=True)
        self.font_mono = pygame.font.SysFont("Menlo,Consolas,monospace", 13)

        # State
        self.serial_thread = SerialThread()
        self.log_lines = []           # (timestamp, line, color)
        self.log_scroll = 0
        self.telemetry = {
            "fill": None, "distance": None,
            "battery_v": None, "battery_p": None,
            "rssi": None, "mqtt": None,
            "lid": None, "next_sec": None,
        }
        self.status_msg = "Not connected"
        self.status_color = TEXT_DIM
        self.publish_count = 0
        self.last_update = None

        # UI state
        self.view = "monitor"     # "monitor" or "ports"
        self.ports_list = []
        self.selected_port = -1
        self.hover_port = -1
        self.hover_btn = None
        self.btn_rects = {}

        # Initial scan
        self.refresh_ports()

    def refresh_ports(self):
        self.ports_list = self.serial_thread.scan_ports()

    def set_status(self, msg, color=TEXT_DIM):
        self.status_msg = msg
        self.status_color = color

    # ── Event processing ────────────────────────────────────────────────────

    def handle_events(self):
        w, h = self.screen.get_size()
        mx, my = pygame.mouse.get_pos()
        self.hover_btn = None
        self.hover_port = -1

        for event in pygame.event.get():
            if event.type == pygame.QUIT:
                return False

            if event.type == pygame.VIDEORESIZE:
                self.screen = pygame.display.set_mode((event.w, event.h), pygame.RESIZABLE)

            if event.type == pygame.MOUSEWHEEL:
                # Scroll log
                if self.view == "monitor":
                    self.log_scroll -= event.y * 3
                    self.log_scroll = max(0, min(self.log_scroll, max(0, len(self.log_lines) - 20)))

            if event.type == pygame.MOUSEBUTTONDOWN and event.button == 1:
                # Check button clicks
                for name, rect in self.btn_rects.items():
                    if rect.collidepoint(mx, my):
                        self.on_button(name)

                # Port selection
                if self.view == "ports":
                    port_y_start = 140
                    for i, (dev, desc, is_esp) in enumerate(self.ports_list):
                        row_rect = pygame.Rect(20, port_y_start + i * 50, w - 40, 45)
                        if row_rect.collidepoint(mx, my):
                            self.selected_port = i

            if event.type == pygame.KEYDOWN:
                if event.key == pygame.K_ESCAPE:
                    if self.view == "ports":
                        self.view = "monitor"
                    else:
                        return False

        # Track hover on ports
        if self.view == "ports":
            port_y_start = 140
            for i in range(len(self.ports_list)):
                row_rect = pygame.Rect(20, port_y_start + i * 50, w - 40, 45)
                if row_rect.collidepoint(mx, my):
                    self.hover_port = i

        # Track hover on buttons
        for name, rect in self.btn_rects.items():
            if rect.collidepoint(mx, my):
                self.hover_btn = name

        return True

    def on_button(self, name):
        if name == "connect":
            if self.serial_thread.connected:
                self.serial_thread.disconnect()
                self.set_status("Disconnected", ORANGE)
            else:
                self.view = "ports"
                self.refresh_ports()

        elif name == "refresh_ports":
            self.refresh_ports()

        elif name == "connect_selected":
            if 0 <= self.selected_port < len(self.ports_list):
                dev = self.ports_list[self.selected_port][0]
                self.serial_thread.connect(dev)
                self.set_status(f"Connecting to {dev}...", YELLOW)
                self.view = "monitor"

        elif name == "auto_detect":
            port = self.serial_thread.auto_detect()
            if port:
                self.serial_thread.connect(port)
                self.set_status(f"Auto-detected {port}", GREEN)
                self.view = "monitor"
            else:
                self.set_status("No ESP32 found", RED)

        elif name == "back":
            self.view = "monitor"

        elif name == "clear_log":
            self.log_lines.clear()
            self.log_scroll = 0

    # ── Serial data processing ──────────────────────────────────────────────

    def process_serial(self):
        while not self.serial_thread.line_queue.empty():
            kind, data = self.serial_thread.line_queue.get_nowait()

            if kind == "__STATUS__":
                color = GREEN if "Connected" in data or "Reconnected" in data else RED if "Error" in data else YELLOW
                self.set_status(data, color)
                self.log_lines.append((datetime.now().strftime("%H:%M:%S"), f"[SYS] {data}", color))
                continue

            # Parse telemetry
            for key, pat in PATTERNS.items():
                m = pat.search(data)
                if m:
                    val = m.group(1)
                    if key in ("distance", "fill", "battery_v"):
                        self.telemetry[key] = float(val)
                    elif key in ("rssi", "battery_p", "next_sec"):
                        self.telemetry[key] = int(val)
                    else:
                        self.telemetry[key] = val

            if "[mqtt]" in data and ("OK" in data or "Payload" in data):
                self.publish_count += 1
                self.last_update = datetime.now()

            # Add to log
            ts = datetime.now().strftime("%H:%M:%S")
            color = tag_color(data)
            self.log_lines.append((ts, data, color))

            if len(self.log_lines) > MAX_LOG_LINES:
                self.log_lines = self.log_lines[-MAX_LOG_LINES:]

            # Auto-scroll to bottom
            self.log_scroll = max(0, len(self.log_lines) - 20)

        # Update connection status display
        if self.serial_thread.connected:
            self.status_color = GREEN

    # ── Drawing ─────────────────────────────────────────────────────────────

    def draw(self):
        self.btn_rects.clear()
        w, h = self.screen.get_size()
        self.screen.fill(BG)

        if self.view == "ports":
            self.draw_ports_view(w, h)
        else:
            self.draw_monitor_view(w, h)

        pygame.display.flip()

    def draw_button(self, x, y, w, h, label, name, color=INDIGO, text_color=WHITE):
        rect = pygame.Rect(x, y, w, h)
        self.btn_rects[name] = rect
        c = tuple(min(255, v + 30) for v in color) if self.hover_btn == name else color
        pygame.draw.rect(self.screen, c, rect, border_radius=6)
        txt = self.font_md.render(label, True, text_color)
        self.screen.blit(txt, (x + (w - txt.get_width()) // 2, y + (h - txt.get_height()) // 2))

    def draw_card(self, x, y, w, h):
        rect = pygame.Rect(x, y, w, h)
        pygame.draw.rect(self.screen, SURFACE, rect, border_radius=10)
        pygame.draw.rect(self.screen, BORDER, rect, width=1, border_radius=10)
        return rect

    def draw_stat_card(self, x, y, w, h, label, value, unit="", val_color=WHITE):
        self.draw_card(x, y, w, h)
        lbl = self.font_sm.render(label.upper(), True, TEXT_MUTED)
        self.screen.blit(lbl, (x + 12, y + 10))
        val_str = str(value) if value is not None else "—"
        val_surf = self.font_xl.render(val_str, True, val_color)
        # Scale down if too wide
        if val_surf.get_width() > w - 24:
            val_surf = self.font_lg.render(val_str, True, val_color)
        self.screen.blit(val_surf, (x + 12, y + 28))
        if unit:
            u = self.font_sm.render(unit, True, TEXT_DIM)
            self.screen.blit(u, (x + 14 + val_surf.get_width(), y + 48))

    def draw_monitor_view(self, w, h):
        # ── Top bar ──────────────────────────────────────────────────────
        pygame.draw.rect(self.screen, SURFACE, (0, 0, w, 50))
        pygame.draw.line(self.screen, BORDER, (0, 50), (w, 50))

        title = self.font_lg.render("EcoRoute Serial Monitor", True, INDIGO)
        self.screen.blit(title, (16, 14))

        # Status dot + text
        dot_color = GREEN if self.serial_thread.connected else RED
        pygame.draw.circle(self.screen, dot_color, (w - 200, 25), 5)
        st = self.font_md.render(self.status_msg[:30], True, self.status_color)
        self.screen.blit(st, (w - 190, 17))

        # Connect/Disconnect button
        btn_label = "Disconnect" if self.serial_thread.connected else "Connect"
        btn_color = RED if self.serial_thread.connected else GREEN
        self.draw_button(w - 430, 10, 110, 30, btn_label, "connect", btn_color)

        # Clear log button
        self.draw_button(w - 310, 10, 90, 30, "Clear Log", "clear_log", SURFACE2)

        # ── Dashboard cards ──────────────────────────────────────────────
        card_y = 60
        card_h = 90
        pad = 10
        n_cards = 6
        card_w = (w - pad * (n_cards + 1)) // n_cards

        fill = self.telemetry["fill"]
        fill_str = f"{fill:.1f}" if fill is not None else "—"
        fill_col = RED if fill and fill >= 90 else ORANGE if fill and fill >= 70 else YELLOW if fill and fill >= 50 else GREEN
        self.draw_stat_card(pad, card_y, card_w, card_h, "Fill Level", fill_str, "%", fill_col)

        dist = self.telemetry["distance"]
        dist_str = f"{dist:.1f}" if dist is not None else "—"
        self.draw_stat_card(pad + (card_w + pad), card_y, card_w, card_h, "Distance", dist_str, "cm", CYAN)

        bv = self.telemetry["battery_v"]
        bp = self.telemetry["battery_p"]
        bv_str = f"{bv:.2f}" if bv is not None else "—"
        batt_col = RED if bv and bv < 3.0 else ORANGE if bv and bv < 3.3 else GREEN
        batt_unit = f"V ({bp}%)" if bp is not None else "V"
        self.draw_stat_card(pad + (card_w + pad) * 2, card_y, card_w, card_h, "Battery", bv_str, batt_unit, batt_col)

        rssi = self.telemetry["rssi"]
        rssi_str = str(rssi) if rssi is not None else "—"
        rssi_col = GREEN if rssi and rssi > -60 else YELLOW if rssi and rssi > -75 else RED
        self.draw_stat_card(pad + (card_w + pad) * 3, card_y, card_w, card_h, "WiFi RSSI", rssi_str, "dBm", rssi_col)

        mqtt_val = self.telemetry["mqtt"]
        mqtt_str = mqtt_val if mqtt_val else "—"
        mqtt_col = GREEN if mqtt_val in ("OK", "Connected") else RED if mqtt_val else TEXT_DIM
        self.draw_stat_card(pad + (card_w + pad) * 4, card_y, card_w, card_h, "MQTT", mqtt_str, "", mqtt_col)

        lid = self.telemetry["lid"]
        lid_str = lid.capitalize() if lid else "N/A"
        lid_col = GREEN if lid and lid.upper() == "CLOSED" else ORANGE if lid else TEXT_DIM
        self.draw_stat_card(pad + (card_w + pad) * 5, card_y, card_w, card_h, "Lid", lid_str, "", lid_col)

        # ── Fill bar ─────────────────────────────────────────────────────
        bar_y = card_y + card_h + 8
        bar_h = 8
        pygame.draw.rect(self.screen, SURFACE2, (pad, bar_y, w - pad * 2, bar_h), border_radius=4)
        if fill is not None and fill >= 0:
            fill_w = int((w - pad * 2) * fill / 100)
            pygame.draw.rect(self.screen, fill_col, (pad, bar_y, fill_w, bar_h), border_radius=4)

        # ── Info row ─────────────────────────────────────────────────────
        info_y = bar_y + bar_h + 6
        pub_txt = f"Publishes: {self.publish_count}"
        last_txt = f"Last: {self.last_update.strftime('%H:%M:%S')}" if self.last_update else "Last: —"
        next_s = self.telemetry["next_sec"]
        next_txt = f"Next in: {next_s}s" if next_s else "Next: —"
        port_txt = f"Port: {self.serial_thread.port}" if self.serial_thread.port else "Port: —"

        for i, txt in enumerate([port_txt, pub_txt, last_txt, next_txt]):
            s = self.font_sm.render(txt, True, TEXT_MUTED)
            self.screen.blit(s, (pad + i * (w // 4), info_y))

        # ── Log section ──────────────────────────────────────────────────
        log_top = info_y + 22
        log_rect = pygame.Rect(pad, log_top, w - pad * 2, h - log_top - 10)
        pygame.draw.rect(self.screen, SURFACE, log_rect, border_radius=8)
        pygame.draw.rect(self.screen, BORDER, log_rect, width=1, border_radius=8)

        # Log header
        hdr = self.font_sm.render("SERIAL OUTPUT", True, TEXT_MUTED)
        self.screen.blit(hdr, (pad + 12, log_top + 6))
        count_txt = self.font_sm.render(f"{len(self.log_lines)} lines", True, TEXT_MUTED)
        self.screen.blit(count_txt, (log_rect.right - count_txt.get_width() - 12, log_top + 6))

        # Log lines (clipped)
        clip_rect = pygame.Rect(log_rect.x + 4, log_top + 24, log_rect.w - 8, log_rect.h - 30)
        self.screen.set_clip(clip_rect)

        line_h = 17
        visible_lines = clip_rect.h // line_h
        start = self.log_scroll
        end = min(start + visible_lines, len(self.log_lines))

        for i in range(start, end):
            ts, text, color = self.log_lines[i]
            y = clip_rect.y + (i - start) * line_h

            # Timestamp
            ts_surf = self.font_mono.render(ts, True, TEXT_MUTED)
            self.screen.blit(ts_surf, (clip_rect.x + 4, y))

            # Line text (truncate if needed)
            max_chars = (clip_rect.w - 80) // 8
            display_text = text[:max_chars]
            line_surf = self.font_mono.render(display_text, True, color)
            self.screen.blit(line_surf, (clip_rect.x + 76, y))

        self.screen.set_clip(None)

        # Scrollbar
        if len(self.log_lines) > visible_lines:
            sb_h = max(20, int(clip_rect.h * visible_lines / len(self.log_lines)))
            sb_y = clip_rect.y + int((clip_rect.h - sb_h) * self.log_scroll / max(1, len(self.log_lines) - visible_lines))
            pygame.draw.rect(self.screen, SURFACE2, (log_rect.right - 8, sb_y, 4, sb_h), border_radius=2)

    def draw_ports_view(self, w, h):
        # Top bar
        pygame.draw.rect(self.screen, SURFACE, (0, 0, w, 50))
        pygame.draw.line(self.screen, BORDER, (0, 50), (w, 50))

        title = self.font_lg.render("Select Serial Port", True, INDIGO)
        self.screen.blit(title, (16, 14))

        self.draw_button(w - 230, 10, 70, 30, "Back", "back", SURFACE2)
        self.draw_button(w - 150, 10, 80, 30, "Refresh", "refresh_ports", BLUE)

        # Instructions
        sub = self.font_md.render("Plug in your ESP32 via USB, then select its port below.", True, TEXT_DIM)
        self.screen.blit(sub, (20, 65))

        # Auto-detect button
        self.draw_button(20, 95, 160, 32, "Auto-Detect ESP32", "auto_detect", GREEN)

        # Port list
        y_start = 140
        if not self.ports_list:
            no_ports = self.font_md.render("No serial ports found. Plug in your ESP32 and click Refresh.", True, TEXT_MUTED)
            self.screen.blit(no_ports, (20, y_start + 20))
        else:
            for i, (dev, desc, is_esp) in enumerate(self.ports_list):
                row_rect = pygame.Rect(20, y_start + i * 50, w - 40, 45)
                bg = BLUE_BG if i == self.selected_port else SURFACE if i == self.hover_port else BG
                pygame.draw.rect(self.screen, bg, row_rect, border_radius=8)
                pygame.draw.rect(self.screen, BORDER if i == self.selected_port else SURFACE2, row_rect, width=1, border_radius=8)

                # Device name
                dev_surf = self.font_mono.render(dev, True, WHITE if i == self.selected_port else TEXT)
                self.screen.blit(dev_surf, (row_rect.x + 12, row_rect.y + 6))

                # Description
                desc_surf = self.font_sm.render(desc, True, TEXT_DIM)
                self.screen.blit(desc_surf, (row_rect.x + 12, row_rect.y + 26))

                # ESP32 badge
                if is_esp:
                    badge_x = row_rect.right - 80
                    badge_rect = pygame.Rect(badge_x, row_rect.y + 12, 65, 22)
                    pygame.draw.rect(self.screen, GREEN_BG, badge_rect, border_radius=11)
                    esp_txt = self.font_sm.render("ESP32", True, GREEN)
                    self.screen.blit(esp_txt, (badge_x + 14, row_rect.y + 15))

        # Connect button (bottom)
        if self.selected_port >= 0:
            sel_dev = self.ports_list[self.selected_port][0] if self.selected_port < len(self.ports_list) else ""
            self.draw_button(20, h - 60, w - 40, 40, f"Connect to {sel_dev}", "connect_selected", INDIGO)

    # ── Main loop ───────────────────────────────────────────────────────────

    def run(self):
        running = True
        while running:
            running = self.handle_events()
            self.process_serial()
            self.draw()
            self.clock.tick(FPS)

        self.serial_thread.disconnect()
        pygame.quit()


# ═══════════════════════════════════════════════════════════════════════════════
#  ENTRY POINT
# ═══════════════════════════════════════════════════════════════════════════════

if __name__ == "__main__":
    app = App()
    app.run()
