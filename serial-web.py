#!/usr/bin/env python3
"""
EcoRoute ESP32 Serial → Web UI bridge.

Reads the live serial stream from a TrashCan2 board and serves a local
web dashboard over HTTP + Server-Sent Events so you can view telemetry
in a browser over USB, in parallel with the on-device WiFi portal at
192.168.4.1.

Usage:
    python3 serial-web.py [--port /dev/cu.usbserial-0001] [--http 8765]
"""

import argparse
import json
import os
import queue
import re
import subprocess
import threading
import time
from datetime import datetime
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

import base64
import urllib.request
import urllib.error
import serial
import serial.tools.list_ports

FIRMWARE_DIR = os.path.dirname(os.path.abspath(__file__))
FLASH_DIR_FOR_ENV = {
    "tc2":     os.path.join(FIRMWARE_DIR, "TrashCan2"),
    "esp32dev": FIRMWARE_DIR,
    "webui":    FIRMWARE_DIR,
}


BAUD = 115200
MAX_LOG = 400

PATTERNS = {
    "distance_cm": re.compile(r"avg=([\d.]+)cm"),
    "fill_pct":    re.compile(r"fill=([\d.]+)\s*%"),
    "lid":         re.compile(r"lid=(CLOSED|OPEN)"),
    "rssi":        re.compile(r"rssi=(-?\d+)\s*dBm"),
    "us1":         re.compile(r"us1=([\d.]+)cm"),
    "us2":         re.compile(r"us2=([\d.]+)cm"),
    "us3":         re.compile(r"us3=([\d.]+)cm"),
    "mqtt_ok":     re.compile(r"\[mqtt\]\s+(OK|FAIL)\s+→"),
    "mqtt_state":  re.compile(r"\[mqtt\]\s+(Connecting|Connected|Failed)"),
    "wifi_ip":     re.compile(r"\[wifi\]\s+Connected!\s+IP:\s+([\d.]+)"),
    "ap_portal":   re.compile(r"Portal:\s+(\S+)\s+@\s+([\d.]+)"),
    "device":      re.compile(r"Device:\s+(\S+)"),
    "fw_version":  re.compile(r"EcoRoute Smart Bin\s+(v\S+)"),
    "publish_num": re.compile(r"publish #(\d+)"),
    "next_sec":    re.compile(r"Next in:\s+(\d+)"),
    "us_median":   re.compile(r"\[us\]\s+median:\s+([\d.]+)\s+cm"),
}

CMD_PATTERNS = {
    "cfg_bin_height":     re.compile(r"\[cmd\].*\bbinHeight=([\d.]+)"),
    "cfg_sensor_offset":  re.compile(r"\[cmd\].*\bsensorOffset=([\d.]+)"),
    "cfg_full_dist":      re.compile(r"\[cmd\].*\bfullDist=([\d.]+)"),
    "cfg_ir_inverted":    re.compile(r"\[cmd\].*\birInverted=(\d)"),
    "cfg_lid_mode":       re.compile(r"\[cmd\].*\blidMode=(\d)"),
    "cfg_lid_threshold":  re.compile(r"\[cmd\].*\blidThreshold=([\d.]+)"),
    "cfg_assume_full":    re.compile(r"\[cmd\].*\bassumeFull=(\d)"),
    "cfg_wifi_ssid":      re.compile(r"\[cmd\].*\bwifi='([^']*)'"),
    "cfg_mqtt":           re.compile(r"\[cmd\].*\bmqtt=(\S+):(\d+)"),
    "cfg_ap_mode":        re.compile(r"\[cmd\].*\bapMode=(\d)"),
    "cfg_device":         re.compile(r"\[cmd\].*\bdevice=(\S+)"),
}


class Bridge:
    def __init__(self, port, invert_lid=False, invert_fill=False):
        self.port = port
        self.invert_lid = invert_lid
        self.invert_fill = invert_fill
        self.ser = None
        self.running = False
        self.connected = False
        self.paused = False           # true while flashing
        self.flash_running = False
        self.subscribers = []
        self.lock = threading.Lock()
        self.write_lock = threading.Lock()
        self.log = []
        self.state = {
            "device": None,
            "fw_version": None,
            "fill_pct": None,          # displayed (possibly inverted)
            "fill_pct_raw": None,      # raw from firmware
            "distance_cm": None,
            "us1_cm": None,
            "us2_cm": None,
            "us3_cm": None,
            "lid": None,               # displayed (possibly inverted)
            "lid_raw": None,           # raw from firmware
            "rssi": None,
            "wifi_ip": None,
            "mqtt": None,
            "ap_ssid": None,
            "ap_ip": None,
            "publish_count": 0,
            "next_sec": None,
            "last_update": None,
            "boot_time": None,
            "port": port,
            "connected": False,
            "invert_lid": invert_lid,
            "invert_fill": invert_fill,
            "config": {
                "bin_height": None,
                "sensor_offset": None,
                "full_dist": None,
                "ir_inverted": None,
                "lid_mode": None,
                "lid_threshold": None,
                "assume_full": None,
                "wifi_ssid": None,
                "mqtt_broker": None,
                "mqtt_port": None,
                "ap_mode": None,
                "device_code": None,
            },
        }

    def subscribe(self):
        q = queue.Queue(maxsize=500)
        with self.lock:
            self.subscribers.append(q)
        return q

    def unsubscribe(self, q):
        with self.lock:
            if q in self.subscribers:
                self.subscribers.remove(q)

    def broadcast(self, event):
        data = json.dumps(event)
        with self.lock:
            dead = []
            for q in self.subscribers:
                try:
                    q.put_nowait(data)
                except queue.Full:
                    dead.append(q)
            for q in dead:
                self.subscribers.remove(q)

    def open_port(self):
        # Do NOT toggle DTR/RTS on open — would reset the ESP32 on every
        # reconnect. pyserial respects the current line state if we set
        # dsrdtr/rtscts=False and leave DTR alone.
        self.ser = serial.Serial()
        self.ser.port = self.port
        self.ser.baudrate = BAUD
        self.ser.timeout = 0.5
        self.ser.dsrdtr = False
        self.ser.rtscts = False
        self.ser.open()
        self.connected = True
        self.state["connected"] = True
        self.state["boot_time"] = datetime.now().isoformat(timespec="seconds")
        self.broadcast({"type": "status", "msg": f"connected {self.port}"})
        self.broadcast({"type": "state", "state": self.state})

    def run(self):
        self.running = True
        while self.running:
            if self.paused:
                time.sleep(0.2)
                continue
            try:
                if not self.connected:
                    self.open_port()
                raw = self.ser.readline()
                if not raw:
                    continue
                line = raw.decode("utf-8", errors="replace").rstrip()
                if not line:
                    continue
                self.handle_line(line)
            except serial.SerialException as e:
                self.connected = False
                self.state["connected"] = False
                self.broadcast({"type": "status", "msg": f"lost: {e}"})
                try:
                    if self.ser:
                        self.ser.close()
                except Exception:
                    pass
                time.sleep(2)
            except Exception as e:
                self.broadcast({"type": "status", "msg": f"error: {e}"})
                time.sleep(1)

    def send_line(self, text: str) -> bool:
        """Write a line to the ESP32 over serial. Returns True on success."""
        if not self.connected or not self.ser:
            return False
        data = (text.rstrip("\r\n") + "\n").encode("utf-8", errors="replace")
        try:
            with self.write_lock:
                self.ser.write(data)
                self.ser.flush()
            self.handle_line(f"[host] → {text.rstrip()}")
            return True
        except Exception as e:
            self.broadcast({"type": "status", "msg": f"send error: {e}"})
            return False

    def set_invert(self, invert_lid=None, invert_fill=None, persist=False):
        if invert_lid is not None:
            self.invert_lid = bool(invert_lid)
            self.state["invert_lid"] = self.invert_lid
        if invert_fill is not None:
            self.invert_fill = bool(invert_fill)
            self.state["invert_fill"] = self.invert_fill
        # Re-derive displayed values from raw if known
        if self.state.get("fill_pct_raw") is not None:
            raw = self.state["fill_pct_raw"]
            self.state["fill_pct"] = (100.0 - raw) if self.invert_fill else raw
        if self.state.get("lid_raw"):
            raw = self.state["lid_raw"]
            if self.invert_lid:
                self.state["lid"] = "OPEN" if raw == "CLOSED" else "CLOSED"
            else:
                self.state["lid"] = raw
        self.broadcast({"type": "state", "state": self.state})

        persisted = False
        if persist and invert_lid is not None:
            # Translate the *host-side* invert choice into the firmware's
            # NVS-persisted IR inversion. Firmware default is irInverted=true
            # (HIGH = closed). The host "invert lid" means "the firmware's
            # reading is upside down for me" — so flip whatever the firmware
            # currently has by toggling cfgIrInverted.
            # Simpler convention: the button just maps host invert_lid=True
            # → firmware IRINV=0 (NORMAL), invert_lid=False → IRINV=1 (INVERTED).
            v = 0 if self.invert_lid else 1
            persisted = self.send_line(f"SET IRINV {v}")
        return persisted

    def switch_port(self, new_port: str):
        """Disconnect from current port and reconnect to new_port."""
        self.paused = True
        try:
            if self.ser:
                try:
                    self.ser.close()
                except Exception:
                    pass
            self.connected = False
            self.port = new_port
            self.state["connected"] = False
            self.broadcast({"type": "status", "msg": f"switching to {new_port}…"})
        finally:
            self.paused = False

    def flash_firmware(self, env="tc2"):
        """Run `pio run -e <env> -t upload` and stream output as SSE events."""
        if self.flash_running:
            self.broadcast({"type": "flash", "phase": "error",
                            "msg": "flash already running"})
            return
        t = threading.Thread(target=self._flash_impl, args=(env,), daemon=True)
        t.start()

    def _flash_impl(self, env):
        self.flash_running = True
        self.broadcast({"type": "flash", "phase": "start",
                        "msg": f"stopping serial reader…"})
        # Release the port so pio can grab it
        self.paused = True
        try:
            if self.ser and self.ser.is_open:
                self.ser.close()
        except Exception:
            pass
        self.connected = False
        self.state["connected"] = False
        self.broadcast({"type": "state", "state": self.state})
        time.sleep(0.5)

        self.broadcast({"type": "flash", "phase": "running",
                        "msg": f"pio run -e {env} -t upload"})
        # pio from `pip install --user` lands in ~/Library/Python/*/bin on
        # macOS, which isn't on PATH by default. Prepend common locations.
        env_vars = os.environ.copy()
        extra_paths = [
            os.path.expanduser("~/Library/Python/3.9/bin"),
            os.path.expanduser("~/Library/Python/3.10/bin"),
            os.path.expanduser("~/Library/Python/3.11/bin"),
            os.path.expanduser("~/Library/Python/3.12/bin"),
            os.path.expanduser("~/.platformio/penv/bin"),
            "/opt/homebrew/bin",
            "/usr/local/bin",
        ]
        env_vars["PATH"] = ":".join(extra_paths) + ":" + env_vars.get("PATH", "")
        cwd = FLASH_DIR_FOR_ENV.get(env, FIRMWARE_DIR)
        try:
            proc = subprocess.Popen(
                ["pio", "run", "-e", env, "-t", "upload"],
                cwd=cwd,
                stdout=subprocess.PIPE,
                stderr=subprocess.STDOUT,
                text=True,
                bufsize=1,
                env=env_vars,
            )
            assert proc.stdout is not None
            for line in proc.stdout:
                line = line.rstrip()
                if line:
                    self.broadcast({"type": "flash", "phase": "log", "msg": line})
            code = proc.wait()
            if code == 0:
                self.broadcast({"type": "flash", "phase": "done",
                                "msg": "flash succeeded"})
            else:
                self.broadcast({"type": "flash", "phase": "error",
                                "msg": f"pio exited with code {code}"})
        except FileNotFoundError:
            self.broadcast({"type": "flash", "phase": "error",
                            "msg": "pio not found on PATH — install PlatformIO"})
        except Exception as e:
            self.broadcast({"type": "flash", "phase": "error", "msg": str(e)})
        finally:
            self.flash_running = False
            # Give ESP32 time to come out of reset before reopening
            time.sleep(1.0)
            self.paused = False
            self.broadcast({"type": "flash", "phase": "resume",
                            "msg": "resuming serial reader"})

    def push_ota(self, ip: str, port: int = 80, user: str = "admin", password: str = "ecoroute"):
        """POST locally-built firmware.bin to http://<ip>/update via HTTP Basic Auth."""
        bin_path = os.path.join(
            FIRMWARE_DIR, "v3-beta", "TrashCan_3", ".pio", "build", "tc3", "firmware.bin"
        )
        self.broadcast({"type": "flash", "phase": "start", "msg": "WiFi OTA push starting…"})

        if not os.path.exists(bin_path):
            self.broadcast({"type": "flash", "phase": "error",
                            "msg": f"firmware.bin not found — build first: {bin_path}"})
            return

        try:
            with open(bin_path, "rb") as f:
                firmware_data = f.read()
        except OSError as e:
            self.broadcast({"type": "flash", "phase": "error", "msg": f"read error: {e}"})
            return

        self.broadcast({"type": "flash", "phase": "log",
                        "msg": f"Firmware: {len(firmware_data):,} bytes from {bin_path}"})

        # Build multipart/form-data body
        boundary = "----EcoRouteOTABoundary7f3a9"
        part_header = (
            f"--{boundary}\r\n"
            f'Content-Disposition: form-data; name="firmware"; filename="firmware.bin"\r\n'
            f"Content-Type: application/octet-stream\r\n\r\n"
        ).encode("utf-8")
        part_footer = f"\r\n--{boundary}--\r\n".encode("utf-8")
        body = part_header + firmware_data + part_footer

        url = f"http://{ip}:{port}/update"
        self.broadcast({"type": "flash", "phase": "running",
                        "msg": f"POST {url} ({len(body):,} bytes)…"})

        req = urllib.request.Request(url, data=body, method="POST")
        req.add_header("Content-Type", f"multipart/form-data; boundary={boundary}")
        req.add_header("Content-Length", str(len(body)))
        creds = base64.b64encode(f"{user}:{password}".encode()).decode("ascii")
        req.add_header("Authorization", f"Basic {creds}")

        try:
            with urllib.request.urlopen(req, timeout=60) as resp:
                response_body = resp.read().decode("utf-8", errors="replace")
                self.broadcast({"type": "flash", "phase": "done",
                                "msg": f"OTA uploaded — device rebooting. Response: {response_body}"})
        except urllib.error.HTTPError as e:
            err_body = e.read().decode("utf-8", errors="replace")
            self.broadcast({"type": "flash", "phase": "error",
                            "msg": f"HTTP {e.code}: {err_body}"})
        except urllib.error.URLError as e:
            self.broadcast({"type": "flash", "phase": "error",
                            "msg": f"Connection failed: {e.reason}"})
        except Exception as e:
            self.broadcast({"type": "flash", "phase": "error", "msg": str(e)})

    def handle_line(self, line):
        ts = datetime.now().strftime("%H:%M:%S")
        entry = {"ts": ts, "line": line}
        self.log.append(entry)
        if len(self.log) > MAX_LOG:
            self.log = self.log[-MAX_LOG:]
        self.broadcast({"type": "log", "entry": entry})

        changed = False
        for key, pat in PATTERNS.items():
            m = pat.search(line)
            if not m:
                continue
            if key == "distance_cm":
                self.state["distance_cm"] = float(m.group(1))
            elif key == "us1":
                self.state["us1_cm"] = float(m.group(1))
            elif key == "us2":
                self.state["us2_cm"] = float(m.group(1))
            elif key == "us3":
                self.state["us3_cm"] = float(m.group(1))
            elif key == "fill_pct":
                raw = float(m.group(1))
                self.state["fill_pct_raw"] = raw
                self.state["fill_pct"] = (100.0 - raw) if self.invert_fill else raw
            elif key == "us_median":
                self.state["distance_cm"] = float(m.group(1))
            elif key == "lid":
                raw = m.group(1)
                self.state["lid_raw"] = raw
                if self.invert_lid:
                    self.state["lid"] = "OPEN" if raw == "CLOSED" else "CLOSED"
                else:
                    self.state["lid"] = raw
            elif key == "rssi":
                self.state["rssi"] = int(m.group(1))
            elif key == "wifi_ip":
                self.state["wifi_ip"] = m.group(1)
            elif key == "ap_portal":
                self.state["ap_ssid"] = m.group(1)
                self.state["ap_ip"] = m.group(2)
            elif key == "device":
                self.state["device"] = m.group(1)
            elif key == "fw_version":
                self.state["fw_version"] = m.group(1)
            elif key == "mqtt_ok":
                self.state["mqtt"] = m.group(1)
            elif key == "mqtt_state":
                self.state["mqtt"] = m.group(1)
            elif key == "publish_num":
                self.state["publish_count"] = int(m.group(1))
            elif key == "next_sec":
                self.state["next_sec"] = int(m.group(1))
            changed = True

        # Parse [cmd] config responses from STATUS
        cfg = self.state["config"]
        for key, pat in CMD_PATTERNS.items():
            m = pat.search(line)
            if not m:
                continue
            if key == "cfg_bin_height":
                cfg["bin_height"] = float(m.group(1))
            elif key == "cfg_sensor_offset":
                cfg["sensor_offset"] = float(m.group(1))
            elif key == "cfg_full_dist":
                cfg["full_dist"] = float(m.group(1))
            elif key == "cfg_ir_inverted":
                cfg["ir_inverted"] = int(m.group(1))
            elif key == "cfg_lid_mode":
                cfg["lid_mode"] = int(m.group(1))
            elif key == "cfg_lid_threshold":
                cfg["lid_threshold"] = float(m.group(1))
            elif key == "cfg_assume_full":
                cfg["assume_full"] = int(m.group(1))
            elif key == "cfg_wifi_ssid":
                cfg["wifi_ssid"] = m.group(1)
            elif key == "cfg_mqtt":
                cfg["mqtt_broker"] = m.group(1)
                cfg["mqtt_port"] = int(m.group(2))
            elif key == "cfg_ap_mode":
                cfg["ap_mode"] = int(m.group(1))
            elif key == "cfg_device":
                cfg["device_code"] = m.group(1)
            changed = True

        if changed:
            self.state["last_update"] = ts
            self.broadcast({"type": "state", "state": self.state})


HTML_PAGE = r"""<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="utf-8">
<title>routeOS USB Bridge</title>
<style>
*{box-sizing:border-box;margin:0;padding:0}
:root{
  --bg:#0c1410;--panel:#111a15;--card:#141f19;--border:#1e2d24;--border2:#1e2d24;
  --text:#d4ddd6;--dim:#6b7d70;--muted:#3d5043;
  --accent:#c8e03a;--accent2:#8ba22a;
  --green:#7fa67e;--red:#c97a3c;--blue:#5f8fa8;--cyan:#4fa89a;--purple:#8a7ab8;
  --font:'Courier New',Courier,monospace;
}
html,body{height:100%;background:var(--bg);color:var(--text);font-family:var(--font);font-size:13px}

/* ── Header ── */
header{
  display:flex;align-items:center;gap:14px;padding:10px 20px;
  background:var(--panel);border-bottom:1px solid var(--border);
  position:sticky;top:0;z-index:20;
}
.logo{
  display:flex;align-items:center;justify-content:center;
  width:28px;height:28px;border-radius:6px;
  background:var(--accent);color:var(--bg);
  font-size:11px;font-weight:700;letter-spacing:-.5px;flex-shrink:0;
}
.brand{font-size:15px;font-weight:700;letter-spacing:.03em}
.hdr-conn{margin-left:auto;display:flex;align-items:center;gap:8px;font-size:12px;color:var(--dim)}
.conn-dot{width:7px;height:7px;border-radius:50%;background:var(--muted);flex-shrink:0;transition:background .3s}
.conn-dot.ok{background:var(--accent)}
.hdr-sep{color:var(--muted)}
.port-sel{
  background:var(--card);border:1px solid var(--border2);border-radius:5px;
  color:var(--text);font-family:var(--font);font-size:11px;padding:4px 8px;
  outline:none;cursor:pointer;max-width:200px;
}
.port-sel:focus{border-color:var(--accent)}
.port-sel option{background:var(--panel)}
.btn-refresh{
  background:none;border:1px solid var(--border2);border-radius:5px;
  color:var(--dim);font-family:var(--font);font-size:11px;padding:4px 8px;
  cursor:pointer;letter-spacing:.05em;transition:all .15s;
}
.btn-refresh:hover{border-color:var(--accent);color:var(--accent)}

/* ── Layout ── */
.layout{display:grid;grid-template-columns:360px 1fr;height:calc(100vh - 49px);overflow:hidden}

/* ── Left panel ── */
.left{border-right:1px solid var(--border);overflow-y:auto;display:flex;flex-direction:column}

/* device + stats always visible */
.left-top{padding:14px;display:flex;flex-direction:column;gap:10px;border-bottom:1px solid var(--border)}
.device-card{border:1px solid var(--border2);border-radius:7px;padding:11px 13px;background:var(--card)}
.device-top{display:flex;justify-content:space-between;align-items:flex-start;margin-bottom:3px}
.device-name{font-size:19px;font-weight:700;color:var(--text)}
.fw-badge{font-size:10px;color:var(--accent);letter-spacing:.04em;padding-top:3px}
.device-sub{font-size:11px;color:var(--dim)}

.stat-row{display:grid;grid-template-columns:repeat(3,1fr);gap:7px}
.stat-box{border:1px solid var(--border2);border-radius:6px;padding:8px 10px;background:var(--card)}
.s-label{font-size:9px;letter-spacing:.1em;color:var(--dim);text-transform:uppercase;margin-bottom:3px}
.s-val{font-size:17px;font-weight:700;line-height:1}
.s-unit{font-size:10px;color:var(--dim);margin-left:2px}
.fill-bar-wrap{height:3px;background:var(--border2);border-radius:2px;overflow:hidden;margin-top:5px}
.fill-bar-inner{height:100%;width:0;background:linear-gradient(90deg,var(--green),var(--accent),var(--red));transition:width .4s}

/* ── Collapsible sections ── */
.sections{display:flex;flex-direction:column;flex:1}
.section{border-bottom:1px solid var(--border)}
.sec-hdr{
  display:flex;align-items:center;justify-content:space-between;
  padding:10px 14px;cursor:pointer;user-select:none;
  transition:background .12s;
}
.sec-hdr:hover{background:rgba(255,255,255,.02)}
.sec-title{font-size:10px;letter-spacing:.12em;text-transform:uppercase;color:var(--dim)}
.sec-meta{font-size:11px;color:var(--muted);margin-left:8px}
.sec-chev{color:var(--muted);font-size:10px;transition:transform .2s;line-height:1}
.section.open .sec-chev{transform:rotate(180deg)}
.sec-body{display:none;padding:0 14px 14px;display:flex;flex-direction:column;gap:8px}
.section:not(.open) .sec-body{display:none}

/* ── Fields ── */
.field-row{display:grid;grid-template-columns:1fr 1fr;gap:6px}
.field{display:flex;flex-direction:column;gap:3px}
.field label{font-size:10px;letter-spacing:.08em;color:var(--dim);text-transform:uppercase}
.field input,.field select{
  background:var(--bg);border:1px solid var(--border2);border-radius:5px;
  padding:7px 9px;color:var(--text);font-family:var(--font);font-size:12px;
  outline:none;transition:border-color .15s;width:100%;
}
.field input:focus,.field select:focus{border-color:var(--accent)}
.field input::placeholder{color:var(--muted)}
select option{background:var(--panel)}

/* ── Buttons ── */
.btn-row{display:flex;gap:6px;flex-wrap:wrap}
.btn{
  font-family:var(--font);font-size:11px;letter-spacing:.07em;text-transform:uppercase;
  padding:7px 13px;border-radius:5px;border:1px solid var(--border2);
  cursor:pointer;transition:all .15s;background:var(--card);color:var(--text);
}
.btn:hover{border-color:var(--dim)}
.btn:disabled{opacity:.4;cursor:not-allowed}
.btn-accent{background:var(--accent);color:var(--bg);border-color:var(--accent);font-weight:700}
.btn-accent:hover{background:var(--accent2);border-color:var(--accent2)}
.btn-danger{border-color:var(--red);color:var(--red)}
.btn-danger:hover{background:rgba(201,122,60,.1)}
.btn-sm{padding:5px 10px;font-size:10px}

.sec-msg{font-size:11px;color:var(--accent);min-height:14px}

/* ── Toggles ── */
.toggle-row{display:flex;gap:8px;align-items:center;font-size:11px;color:var(--dim)}
.tpill{
  display:inline-flex;align-items:center;gap:5px;padding:3px 9px;
  border-radius:999px;border:1px solid var(--border2);
  cursor:pointer;user-select:none;transition:all .15s;background:var(--card);
}
.tpill:hover{border-color:var(--dim)}
.tpill.on{border-color:var(--accent);color:var(--accent)}
.tdot{width:5px;height:5px;border-radius:50%;background:var(--muted)}
.tpill.on .tdot{background:var(--accent)}

/* ── Flash pane ── */
.flash-pane{
  display:none;border:1px solid var(--border2);border-radius:5px;
  background:var(--bg);padding:8px 10px;font-size:11px;max-height:140px;overflow-y:auto;
}
.flash-pane.show{display:block}
.ph-start{color:var(--accent)}.ph-running{color:var(--blue)}
.ph-done{color:var(--green)}.ph-error{color:var(--red)}.ph-log{color:var(--dim)}

/* ── Right panel ── */
.right{display:flex;flex-direction:column;overflow:hidden}

/* ── Heatmap ── */
.heatmap-panel{flex-shrink:0;border-bottom:1px solid var(--border);background:var(--panel)}
.heatmap-hdr{
  display:flex;align-items:center;justify-content:space-between;
  padding:8px 16px;cursor:pointer;user-select:none;
}
.heatmap-hdr:hover{background:rgba(255,255,255,.02)}
.heatmap-label{font-size:10px;letter-spacing:.12em;color:var(--dim);text-transform:uppercase}
.heatmap-meta{font-size:11px;color:var(--muted);margin-left:8px}
.heatmap-chev{color:var(--muted);font-size:10px;transition:transform .2s}
.heatmap-panel.open .heatmap-chev{transform:rotate(180deg)}
.heatmap-body{display:none;padding:10px 16px 12px}
.heatmap-panel.open .heatmap-body{display:block}
#heatmap_canvas{width:100%;height:160px;border-radius:6px;border:1px solid var(--border2);display:block;cursor:crosshair}
.heatmap-sensors{display:flex;gap:16px;margin-top:8px;font-size:10px;color:var(--dim)}
.hs-dot{display:inline-block;width:8px;height:8px;border-radius:50%;margin-right:4px;vertical-align:middle}
#heatmap_probe{font-size:11px;color:var(--accent);min-height:14px;margin-top:4px}

.log-header{
  display:flex;align-items:center;justify-content:space-between;
  padding:10px 16px;border-bottom:1px solid var(--border);flex-shrink:0;
}
.log-label{font-size:10px;letter-spacing:.12em;color:var(--dim);text-transform:uppercase}
.live-badge{display:none;align-items:center;gap:5px;font-size:10px;color:var(--accent)}
.live-dot{width:6px;height:6px;border-radius:50%;background:var(--accent);animation:pulse 1.8s ease-in-out infinite}
@keyframes pulse{0%,100%{opacity:1}50%{opacity:.25}}
.log-body{flex:1;overflow-y:auto;padding:10px 14px;font-size:12px;line-height:1.75}
.log-row{display:flex;align-items:baseline}
.log-ts{color:var(--muted);flex-shrink:0;margin-right:10px;font-size:11px}
.log-tag{flex-shrink:0;margin-right:8px}
.log-msg{white-space:pre-wrap;word-break:break-all}

.tag-boot{color:var(--dim)}.tag-wifi{color:var(--blue)}.tag-mqtt{color:var(--purple)}
.tag-sensor,.tag-us,.tag-fill,.tag-result{color:var(--accent)}
.tag-ir,.tag-cal{color:var(--cyan)}
.tag-rate,.tag-next{color:var(--green)}
.tag-err,.tag-error,.tag-warn{color:var(--red)}
.tag-host,.tag-other{color:var(--dim)}

.cmd-bar{display:flex;border-top:1px solid var(--border);flex-shrink:0}
.cmd-input{
  flex:1;background:transparent;border:none;outline:none;
  font-family:var(--font);font-size:13px;color:var(--text);padding:12px 14px;
}
.cmd-input::placeholder{color:var(--muted)}
.cmd-send{
  font-family:var(--font);font-size:11px;letter-spacing:.08em;text-transform:uppercase;
  padding:0 18px;background:var(--card);border:none;border-left:1px solid var(--border);
  color:var(--dim);cursor:pointer;transition:all .15s;
}
.cmd-send:hover{background:var(--panel);color:var(--text)}

::-webkit-scrollbar{width:4px;height:4px}
::-webkit-scrollbar-track{background:transparent}
::-webkit-scrollbar-thumb{background:var(--border2);border-radius:2px}
@media(max-width:800px){.layout{grid-template-columns:1fr;grid-template-rows:auto 1fr}}
</style>
</head>
<body>

<header>
  <div class="logo">ros</div>
  <span class="brand">routeOS USB Bridge</span>
  <div class="hdr-conn">
    <select class="port-sel" id="port_sel" title="Serial port"></select>
    <button class="btn-refresh" id="btn_refresh_ports" title="Refresh port list">⟳</button>
    <span class="hdr-sep">·</span>
    <span class="conn-dot" id="conn_dot"></span>
    <span id="hdr_status">connecting…</span>
  </div>
</header>

<div class="layout">

  <!-- ── LEFT ── -->
  <div class="left">

    <!-- Always-visible: device card + stats -->
    <div class="left-top">
      <div class="device-card">
        <div class="device-top">
          <div class="device-name" id="d_name">—</div>
          <div class="fw-badge" id="d_fw">—</div>
        </div>
        <div class="device-sub" id="d_sub">waiting for device…</div>
      </div>
      <div class="stat-row">
        <div class="stat-box">
          <div class="s-label">Fill</div>
          <div class="s-val" id="s_fill" style="color:var(--green)">—<span class="s-unit">%</span></div>
          <div class="fill-bar-wrap"><div class="fill-bar-inner" id="fillbar"></div></div>
        </div>
        <div class="stat-box">
          <div class="s-label">Lid</div>
          <div class="s-val" id="s_lid" style="color:var(--dim)">—</div>
        </div>
        <div class="stat-box">
          <div class="s-label">MQTT</div>
          <div class="s-val" id="s_mqtt" style="color:var(--dim)">—</div>
        </div>
      </div>
    </div>

    <!-- Collapsible sections -->
    <div class="sections">

      <!-- NETWORK — open by default, has own Save & Reboot -->
      <div class="section open" id="sec_wifi">
        <div class="sec-hdr" onclick="toggle('sec_wifi')">
          <span><span class="sec-title">Network</span><span class="sec-meta" id="wifi_meta"></span></span>
          <span class="sec-chev">▾</span>
        </div>
        <div class="sec-body">
          <div class="field">
            <label>SSID</label>
            <input id="c_wifiSsid" type="text" placeholder="network name">
          </div>
          <div class="field">
            <label>Pass</label>
            <input id="c_wifiPass" type="password" placeholder="password">
          </div>
          <div class="btn-row">
            <button class="btn btn-accent" id="wifi_save_reboot">Save &amp; Reboot</button>
            <button class="btn" id="cfg_reconnect">Reconnect</button>
          </div>
          <div class="sec-msg" id="wifi_msg"></div>
        </div>
      </div>

      <!-- MQTT -->
      <div class="section open" id="sec_mqtt">
        <div class="sec-hdr" onclick="toggle('sec_mqtt')">
          <span><span class="sec-title">MQTT Broker</span><span class="sec-meta" id="mqtt_meta"></span></span>
          <span class="sec-chev">▾</span>
        </div>
        <div class="sec-body">
          <div class="field-row">
            <div class="field">
              <label>Host</label>
              <input id="c_mqttBroker" type="text" placeholder="109.123.238.215">
            </div>
            <div class="field">
              <label>Port</label>
              <input id="c_mqttPort" type="number" placeholder="1883">
            </div>
          </div>
          <div class="btn-row">
            <button class="btn btn-accent" id="mqtt_save">Save MQTT</button>
          </div>
          <div class="sec-msg" id="mqtt_msg"></div>
        </div>
      </div>

      <!-- BIN PARAMETERS -->
      <div class="section" id="sec_bin">
        <div class="sec-hdr" onclick="toggle('sec_bin')">
          <span><span class="sec-title">Bin Parameters</span></span>
          <span class="sec-chev">▾</span>
        </div>
        <div class="sec-body">
          <div class="field-row">
            <div class="field"><label>Height (cm)</label><input id="c_binHeight" type="number" step="0.1" placeholder="100.0"></div>
            <div class="field"><label>Full Dist (cm)</label><input id="c_fullDist" type="number" step="0.1" placeholder="5.0"></div>
          </div>
          <div class="field-row">
            <div class="field"><label>Sensor Offset (cm)</label><input id="c_sensorOffset" type="number" step="0.1" placeholder="0.0"></div>
            <div class="field"><label>Lid Threshold</label><input id="c_lidThreshold" type="number" step="0.1" placeholder="1.5"></div>
          </div>
          <div class="field-row">
            <div class="field"><label>Lid Mode</label>
              <select id="c_lidMode">
                <option value="0">IR Only</option>
                <option value="1">Ultrasonic Only</option>
                <option value="2">Both (IR + US)</option>
              </select>
            </div>
            <div class="field"><label>IR Inverted</label>
              <select id="c_irInv">
                <option value="0">Normal (LOW=closed)</option>
                <option value="1">Inverted (HIGH=closed)</option>
              </select>
            </div>
          </div>
          <div class="btn-row">
            <button class="btn btn-accent" id="bin_save">Save Bin Config</button>
            <button class="btn" id="cfg_cal">Calibrate</button>
          </div>
          <div class="sec-msg" id="bin_msg"></div>
        </div>
      </div>

      <!-- DEVICE -->
      <div class="section" id="sec_device">
        <div class="sec-hdr" onclick="toggle('sec_device')">
          <span><span class="sec-title">Device</span><span class="sec-meta" id="dev_meta"></span></span>
          <span class="sec-chev">▾</span>
        </div>
        <div class="sec-body">
          <div class="field-row">
            <div class="field"><label>AP Mode</label>
              <select id="c_apMode">
                <option value="0">Always</option>
                <option value="1">Hidden</option>
                <option value="2">On-Demand</option>
              </select>
            </div>
            <div class="field"><label>Device Code</label><input id="c_deviceCode" type="text" placeholder="ECO-BIN-003"></div>
          </div>
          <div class="btn-row">
            <button class="btn btn-accent" id="dev_save">Save Device</button>
            <button class="btn" id="cfg_refresh">Status</button>
            <button class="btn btn-danger" id="cfg_reboot">Reboot</button>
          </div>
          <div class="sec-msg" id="dev_msg"></div>
        </div>
      </div>

      <!-- DISPLAY -->
      <div class="section" id="sec_display">
        <div class="sec-hdr" onclick="toggle('sec_display')">
          <span><span class="sec-title">Display</span></span>
          <span class="sec-chev">▾</span>
        </div>
        <div class="sec-body">
          <div class="toggle-row">
            <span class="tpill" id="tog_fill"><span class="tdot"></span>Invert fill</span>
            <span class="tpill" id="tog_lid"><span class="tdot"></span>Invert lid</span>
            <button class="btn btn-sm" id="btn_persist" style="margin-left:auto">Persist IR</button>
          </div>
        </div>
      </div>

      <!-- FIRMWARE -->
      <div class="section" id="sec_flash">
        <div class="sec-hdr" onclick="toggle('sec_flash')">
          <span><span class="sec-title">Firmware</span></span>
          <span class="sec-chev">▾</span>
        </div>
        <div class="sec-body">
          <div style="font-size:11px;color:var(--dim);line-height:1.6">
            Compiles and flashes firmware from the local PlatformIO project.
            Device will reset after flashing.
          </div>
          <div class="btn-row">
            <button class="btn" id="btn_flash">Flash tc3</button>
            <button class="btn" id="btn_flash_tc2">Flash tc2</button>
          </div>
          <div style="margin-top:8px;padding-top:8px;border-top:1px solid var(--border)">
            <div style="font-size:11px;color:var(--dim);margin-bottom:6px">WiFi OTA — push .bin to device over the network</div>
            <div class="field">
              <label>Device IP</label>
              <input id="ota_ip" type="text" placeholder="192.168.x.x" style="width:100%">
            </div>
            <div style="display:grid;grid-template-columns:1fr 1fr;gap:4px;margin-top:4px">
              <input id="ota_user" type="text" placeholder="user (admin)" style="font-size:11px">
              <input id="ota_pass" type="password" placeholder="password" style="font-size:11px">
            </div>
            <button class="btn" id="btn_ota" style="margin-top:6px;width:100%">Push OTA via WiFi</button>
          </div>
          <div style="margin-top:8px;padding-top:8px;border-top:1px solid var(--border)">
            <div style="font-size:11px;color:var(--dim);margin-bottom:6px">Upload .bin → Marketplace (saves to backend/uploads/firmware/)</div>
            <input id="upload_bin_file" type="file" accept=".bin" style="font-size:11px;width:100%;margin-bottom:4px">
            <button class="btn" id="btn_upload_bin" style="width:100%">Upload .bin to Marketplace</button>
            <div id="upload_bin_status" style="font-size:11px;margin-top:4px;color:var(--dim)"></div>
          </div>
          <div class="flash-pane" id="flash_pane"><div id="flash_log"></div></div>
        </div>
      </div>

    </div><!-- /sections -->
  </div><!-- /left -->

  <!-- ── RIGHT ── -->
  <div class="right">

    <!-- Heatmap panel -->
    <div class="heatmap-panel open" id="heatmap_panel">
      <div class="heatmap-hdr" onclick="document.getElementById('heatmap_panel').classList.toggle('open');drawHeatmap()">
        <span><span class="heatmap-label">Fill Heatmap</span><span class="heatmap-meta" id="heatmap_meta">3-sensor IDW</span></span>
        <span class="heatmap-chev">▾</span>
      </div>
      <div class="heatmap-body">
        <canvas id="heatmap_canvas" width="800" height="160"></canvas>
        <div class="heatmap-sensors">
          <span><span class="hs-dot" style="background:#5f8fa8"></span>US1 left</span>
          <span><span class="hs-dot" style="background:#c8e03a"></span>US2 center</span>
          <span><span class="hs-dot" style="background:#8a7ab8"></span>US3 right</span>
        </div>
        <div id="heatmap_probe"></div>
      </div>
    </div>

    <div class="log-header">
      <span class="log-label">Serial Log</span>
      <span class="live-badge" id="live_badge"><span class="live-dot"></span>LIVE</span>
    </div>
    <div class="log-body" id="log"></div>
    <div class="cmd-bar">
      <input class="cmd-input" id="cmd_input" type="text"
        placeholder="STATUS · CAL · REBOOT · SET WIFI ssid pass…"
        autocomplete="off" spellcheck="false">
      <button class="cmd-send" id="cmd_send">Send</button>
    </div>
  </div>

</div>

<script>
const el = id => document.getElementById(id);
const logEl = el('log');
const MAX_LINES = 400;

// ── Accordion ─────────────────────────────────────────────────────────
function toggle(id){
  document.getElementById(id).classList.toggle('open');
}

// ── Log ───────────────────────────────────────────────────────────────
function parseTag(line){
  const m=line.match(/^\[(\w+)]/);
  return m ? {tag:m[1].toLowerCase(), msg:line.slice(m[0].length).trimStart()} : {tag:null,msg:line};
}
function tagCls(tag){
  if(!tag) return 'tag-other';
  return({boot:'tag-boot',wifi:'tag-wifi',ap:'tag-wifi',mqtt:'tag-mqtt',web:'tag-mqtt',
    sensor:'tag-sensor',us:'tag-us',fill:'tag-fill',result:'tag-result',
    ir:'tag-ir',cal:'tag-cal',rate:'tag-rate',next:'tag-next',
    host:'tag-host',err:'tag-err',error:'tag-error',warn:'tag-warn'})[tag]||'tag-other';
}
function appendLog(entry){
  const atBottom=logEl.scrollTop+logEl.clientHeight>=logEl.scrollHeight-30;
  const {tag,msg}=parseTag(entry.line);
  const cls=tagCls(tag);
  const row=document.createElement('div'); row.className='log-row';
  const ts=document.createElement('span'); ts.className='log-ts'; ts.textContent=entry.ts;
  const tg=document.createElement('span'); tg.className='log-tag '+cls; tg.textContent=tag?'['+tag+']':'';
  const mg=document.createElement('span'); mg.className='log-msg '+cls; mg.textContent=msg;
  row.append(ts,tg,mg); logEl.appendChild(row);
  while(logEl.childElementCount>MAX_LINES) logEl.removeChild(logEl.firstChild);
  if(atBottom) logEl.scrollTop=logEl.scrollHeight;
}

// ── State ─────────────────────────────────────────────────────────────
let S={};
function applyState(s){
  S=s;
  el('conn_dot').classList.toggle('ok',!!s.connected);
  el('hdr_status').textContent=s.connected?'Connected':'Disconnected';
  el('hdr_status').style.color=s.connected?'var(--accent)':'var(--dim)';
  // update port selector current value without rebuilding options
  if(el('port_sel') && s.port){ const sel=el('port_sel'); for(let o of sel.options) if(o.value===s.port){ sel.value=s.port; break; } }
  el('live_badge').style.display=s.connected?'flex':'none';

  el('d_name').textContent=s.device||'—';
  el('d_fw').textContent=s.fw_version?'✓ routeOS '+s.fw_version:'—';
  el('d_sub').textContent=[s.wifi_ip?'IP: '+s.wifi_ip:null,s.ap_ssid?'AP: '+s.ap_ssid:null]
    .filter(Boolean).join(' · ')||(s.connected?'connected via USB':'not connected');
  // pre-fill OTA IP when device reports its WiFi address
  if(s.wifi_ip && el('ota_ip') && !el('ota_ip').value) el('ota_ip').value=s.wifi_ip;

  const fill=s.fill_pct;
  el('s_fill').firstChild.textContent=fill==null?'—':fill.toFixed(1);
  el('s_fill').style.color=fill==null?'var(--dim)':fill>=90?'var(--red)':fill>=70?'var(--accent)':'var(--green)';
  el('fillbar').style.width=fill==null?'0':(Math.max(0,Math.min(100,fill))+'%');
  el('s_lid').textContent=s.lid||'—';
  el('s_lid').style.color=s.lid==='CLOSED'?'var(--green)':s.lid==='OPEN'?'var(--red)':'var(--dim)';
  el('s_mqtt').textContent=s.mqtt||'—';
  el('s_mqtt').style.color=(s.mqtt==='OK'||s.mqtt==='Connected')?'var(--green)':s.mqtt?'var(--red)':'var(--dim)';

  el('tog_fill').classList.toggle('on',!!s.invert_fill);
  el('tog_lid').classList.toggle('on',!!s.invert_lid);

  // section meta hints
  if(s.config){
    const cfg=s.config;
    if(cfg.wifi_ssid) el('wifi_meta').textContent=' · '+cfg.wifi_ssid;
    if(cfg.mqtt_broker) el('mqtt_meta').textContent=' · '+cfg.mqtt_broker+':'+(cfg.mqtt_port||1883);
    if(s.device) el('dev_meta').textContent=' · '+s.device;
    populateConfig(cfg);
  }

  // update heatmap meta line with per-sensor distances if available
  const u1=s.us1_cm, u2=s.us2_cm, u3=s.us3_cm;
  if(u1!=null||u2!=null||u3!=null){
    const parts=[];
    if(u1!=null) parts.push('US1 '+u1.toFixed(1)+'cm');
    if(u2!=null) parts.push('US2 '+u2.toFixed(1)+'cm');
    if(u3!=null) parts.push('US3 '+u3.toFixed(1)+'cm');
    el('heatmap_meta').textContent=parts.join(' · ');
  }
  if(document.getElementById('heatmap_panel').classList.contains('open')){
    if(hmAnimFrame) cancelAnimationFrame(hmAnimFrame);
    hmAnimFrame=requestAnimationFrame(drawHeatmap);
  }
}

// ── Config ────────────────────────────────────────────────────────────
const cfgFields={
  binHeight:   {el:'c_binHeight',   cmd:'SET BINHEIGHT',   key:'bin_height'},
  fullDist:    {el:'c_fullDist',    cmd:'SET FULLDIST',    key:'full_dist'},
  sensorOffset:{el:'c_sensorOffset',cmd:'SET SENSOROFFSET',key:'sensor_offset'},
  lidMode:     {el:'c_lidMode',     cmd:'SET LIDMODE',     key:'lid_mode'},
  irInv:       {el:'c_irInv',       cmd:'SET IRINV',       key:'ir_inverted'},
  lidThreshold:{el:'c_lidThreshold',cmd:'SET LIDTHRESHOLD',key:'lid_threshold'},
  apMode:      {el:'c_apMode',      cmd:'SET APMODE',      key:'ap_mode'},
  deviceCode:  {el:'c_deviceCode',  cmd:'SET DEVICE',      key:'device_code'},
};
function populateConfig(cfg){
  for(const [,f] of Object.entries(cfgFields))
    if(f.key&&cfg[f.key]!=null) el(f.el).value=cfg[f.key];
  if(cfg.wifi_ssid) el('c_wifiSsid').value=cfg.wifi_ssid;
  if(cfg.mqtt_broker) el('c_mqttBroker').value=cfg.mqtt_broker;
  if(cfg.mqtt_port)   el('c_mqttPort').value=cfg.mqtt_port;
}

function showMsg(id,txt,ok=true){
  const m=el(id); m.textContent=txt;
  m.style.color=ok?'var(--accent)':'var(--red)';
  setTimeout(()=>{m.textContent=''},3000);
}

async function post(url,body){
  const r=await fetch(url,{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify(body||{})});
  return r.json();
}
const cmd=line=>post('/api/send',{line});

// ── WiFi section ──────────────────────────────────────────────────────
el('wifi_save_reboot').onclick=async()=>{
  const ssid=el('c_wifiSsid').value.trim();
  if(!ssid){showMsg('wifi_msg','Enter an SSID first',false);return;}
  el('wifi_save_reboot').disabled=true; el('wifi_save_reboot').textContent='Saving…';
  await cmd('SET WIFI '+ssid+' '+el('c_wifiPass').value);
  await new Promise(r=>setTimeout(r,300));
  await cmd('REBOOT');
  showMsg('wifi_msg','WiFi saved — rebooting…');
  el('wifi_save_reboot').textContent='Save & Reboot'; el('wifi_save_reboot').disabled=false;
};
el('cfg_reconnect').onclick=()=>{ cmd('RECONNECT'); showMsg('wifi_msg','Reconnect sent'); };

// ── MQTT section ──────────────────────────────────────────────────────
el('mqtt_save').onclick=async()=>{
  const broker=el('c_mqttBroker').value.trim();
  if(!broker){showMsg('mqtt_msg','Enter a broker host first',false);return;}
  await cmd('SET MQTT '+broker+' '+(el('c_mqttPort').value||'1883'));
  showMsg('mqtt_msg','MQTT saved');
};

// ── Bin section ───────────────────────────────────────────────────────
el('bin_save').onclick=async()=>{
  el('bin_save').disabled=true; el('bin_save').textContent='Saving…';
  let n=0;
  for(const k of ['binHeight','fullDist','sensorOffset','lidMode','irInv','lidThreshold']){
    const f=cfgFields[k]; const v=el(f.el).value; if(!v&&v!=='0') continue;
    await cmd(f.cmd+' '+v); n++;
  }
  showMsg('bin_msg',n+' parameter(s) saved');
  el('bin_save').textContent='Save Bin Config'; el('bin_save').disabled=false;
  setTimeout(()=>cmd('STATUS'),400);
};
el('cfg_cal').onclick=async()=>{
  if(!confirm('Auto-calibrate? Bin must be EMPTY.')) return;
  el('cfg_cal').disabled=true;
  await cmd('CAL'); showMsg('bin_msg','Calibrating…');
  setTimeout(()=>{ cmd('STATUS'); el('cfg_cal').disabled=false; },5000);
};

// ── Device section ────────────────────────────────────────────────────
el('dev_save').onclick=async()=>{
  const ap=cfgFields.apMode; const dc=cfgFields.deviceCode;
  await cmd(ap.cmd+' '+el(ap.el).value);
  const code=el(dc.el).value.trim();
  if(code) await cmd(dc.cmd+' '+code);
  showMsg('dev_msg','Device settings saved');
  setTimeout(()=>cmd('STATUS'),400);
};
el('cfg_refresh').onclick=async()=>{ await cmd('STATUS'); showMsg('dev_msg','STATUS sent'); };
el('cfg_reboot').onclick=async()=>{
  if(!confirm('Reboot the ESP32?')) return;
  await cmd('REBOOT'); showMsg('dev_msg','Rebooting…');
};

// ── Display section ───────────────────────────────────────────────────
el('tog_fill').onclick=async()=>post('/api/config',{invert_fill:!S.invert_fill});
el('tog_lid').onclick =async()=>post('/api/config',{invert_lid:!S.invert_lid});
el('btn_persist').onclick=async()=>{
  el('btn_persist').disabled=true;
  const r=await post('/api/config',{invert_lid:S.invert_lid,persist:true});
  el('btn_persist').textContent=r.persisted?'Saved ✓':'Failed';
  setTimeout(()=>{el('btn_persist').textContent='Persist IR';el('btn_persist').disabled=false;},1800);
};

// ── Firmware section ──────────────────────────────────────────────────
function doFlash(env){
  if(!confirm('Flash firmware ('+env+')? ~30-60s, device will reset.')) return;
  el('flash_pane').classList.add('show'); el('flash_log').innerHTML='';
  post('/api/flash',{env});
}
el('btn_flash').onclick=()=>doFlash('tc3');
el('btn_flash_tc2').onclick=()=>doFlash('tc2');

// ── WiFi OTA push ────────────────────────────────────────────────────
(function(){
  const otaIp = el('ota_ip');
  const otaUser = el('ota_user');
  const otaPass = el('ota_pass');
  const btnOta = el('btn_ota');

  if(btnOta) btnOta.addEventListener('click', ()=>{
    const ip = otaIp.value.trim();
    if(!ip){alert('Enter device IP address');return;}
    if(!confirm('Push WiFi OTA to '+ip+'? Device will reboot after flashing.')) return;
    btnOta.disabled=true; btnOta.textContent='Pushing…';
    el('flash_pane').classList.add('show'); el('flash_log').innerHTML='';
    post('/api/ota',{
      ip,
      user: otaUser.value.trim() || 'admin',
      pass: otaPass.value || 'ecoroute'
    }).catch(e=>flashEvent({phase:'error',msg:String(e)}))
      .finally(()=>{btnOta.disabled=false; btnOta.textContent='Push OTA via WiFi';});
  });
})();

// ── Upload .bin to Marketplace ────────────────────────────────────────
(function(){
  const fileInput = el('upload_bin_file');
  const btn = el('btn_upload_bin');
  const status = el('upload_bin_status');
  if(!btn) return;
  btn.addEventListener('click', async ()=>{
    const file = fileInput && fileInput.files && fileInput.files[0];
    if(!file){alert('Select a .bin file first');return;}
    if(!file.name.endsWith('.bin')){alert('Only .bin files accepted');return;}
    btn.disabled=true; btn.textContent='Uploading…'; status.textContent='';
    try{
      const fd=new FormData();
      fd.append('firmware',file,file.name);
      const r=await fetch('/api/upload-bin',{method:'POST',body:fd});
      const j=await r.json();
      if(j.ok){
        status.style.color='#c8e03a';
        status.textContent='✓ Uploaded: '+j.filename+' ('+Math.round(j.size/1024)+' KB)';
      } else {
        status.style.color='#e07070';
        status.textContent='✗ '+j.error;
      }
    }catch(e){
      status.style.color='#e07070';
      status.textContent='✗ Upload failed: '+e;
    }finally{
      btn.disabled=false; btn.textContent='Upload .bin to Marketplace';
    }
  });
})();

function flashEvent(msg){
  const row=document.createElement('div'); row.className='ph-'+msg.phase;
  row.textContent=(msg.phase==='log'?'  ':'['+msg.phase+'] ')+msg.msg;
  el('flash_log').appendChild(row);
  el('flash_pane').scrollTop=el('flash_pane').scrollHeight;
  if(msg.phase==='done'||msg.phase==='error'){
    el('btn_flash').textContent=msg.phase==='done'?'Flash tc3 ✓':'Flash failed';
    setTimeout(()=>{el('btn_flash').textContent='Flash tc3'},3000);
  }
}

// ── Command bar ───────────────────────────────────────────────────────
el('cmd_send').onclick=()=>{ const v=el('cmd_input').value.trim(); if(!v) return; cmd(v); el('cmd_input').value=''; };
el('cmd_input').onkeydown=e=>{ if(e.key==='Enter') el('cmd_send').click(); };

// ── Heatmap ───────────────────────────────────────────────────────────
// Sensor positions in normalized [0,1] coords: US1=left, US2=center-back, US3=right
const SENSOR_POS = [
  {id:'us1_cm',label:'US1',nx:0.18,ny:0.5,  dotColor:'#5f8fa8'},
  {id:'us2_cm',label:'US2',nx:0.5, ny:0.18, dotColor:'#c8e03a'},
  {id:'us3_cm',label:'US3',nx:0.82,ny:0.5,  dotColor:'#8a7ab8'},
];
const IDW_POWER = 4;
let hmAnimFrame = null;

function fillColor(f){ // f in [0,100] → rgba
  // green(0%) → yellow(50%) → orange(75%) → red(100%)
  let r,g,b;
  if(f<=50){const t=f/50; r=Math.round(t*220); g=Math.round(166+t*(200-166)); b=Math.round(58*(1-t));}
  else if(f<=75){const t=(f-50)/25; r=Math.round(220+t*35); g=Math.round(200-t*100); b=0;}
  else{const t=(f-75)/25; r=255; g=Math.round(100*(1-t)); b=0;}
  const a=0.82+0.18*(f/100);
  return `rgba(${r},${g},${b},${a})`;
}

function idwFill(px,py,sensors,binH){
  let wsum=0,fsum=0,exact=null;
  for(const s of sensors){
    if(s.fill==null) continue;
    const dx=px-s.nx, dy=py-s.ny;
    const d2=dx*dx+dy*dy;
    if(d2<1e-9){exact=s.fill;break;}
    const w=1/Math.pow(d2,IDW_POWER/2);
    wsum+=w; fsum+=w*s.fill;
  }
  if(exact!=null) return exact;
  if(wsum===0) return null;
  return fsum/wsum;
}

function drawHeatmap(){
  const canvas=el('heatmap_canvas');
  if(!canvas) return;
  const W=canvas.width, H=canvas.height;
  const ctx=canvas.getContext('2d');
  ctx.clearRect(0,0,W,H);

  // Bin background — rounded rect
  ctx.fillStyle='#111a15';
  ctx.beginPath(); ctx.roundRect(0,0,W,H,8); ctx.fill();
  // Bin outline
  ctx.strokeStyle='#1e2d24'; ctx.lineWidth=1;
  ctx.beginPath(); ctx.roundRect(1,1,W-2,H-2,8); ctx.stroke();

  // Build sensor fill values from state
  const binH = (S.config&&S.config.bin_height)||100;
  const sensors = SENSOR_POS.map(s=>{
    const dist=S[s.id];
    const fill = dist==null ? null : Math.max(0,Math.min(100,(1-dist/binH)*100));
    return {...s, fill};
  });

  const hasData = sensors.some(s=>s.fill!=null);
  if(!hasData){
    ctx.fillStyle='#3d5043'; ctx.font='12px Courier New,monospace';
    ctx.textAlign='center'; ctx.fillText('waiting for sensor data…',W/2,H/2);
    return;
  }

  // IDW pixel pass — draw on offscreen then blit (perf: every 4px)
  const STEP=4;
  for(let px=0;px<W;px+=STEP){
    for(let py=0;py<H;py+=STEP){
      const nx=px/W, ny=py/H;
      const f=idwFill(nx,ny,sensors,binH);
      if(f==null) continue;
      ctx.fillStyle=fillColor(f);
      ctx.fillRect(px,py,STEP,STEP);
    }
  }

  // Bin walls — dark vignette overlay
  const grad=ctx.createRadialGradient(W/2,H/2,Math.min(W,H)*0.3,W/2,H/2,Math.max(W,H)*0.7);
  grad.addColorStop(0,'rgba(0,0,0,0)');
  grad.addColorStop(1,'rgba(12,20,16,0.55)');
  ctx.fillStyle=grad; ctx.fillRect(0,0,W,H);

  // Grid lines (faint)
  ctx.strokeStyle='rgba(255,255,255,0.04)'; ctx.lineWidth=1;
  for(let x=0;x<W;x+=W/5){ctx.beginPath();ctx.moveTo(x,0);ctx.lineTo(x,H);ctx.stroke();}
  for(let y=0;y<H;y+=H/3){ctx.beginPath();ctx.moveTo(0,y);ctx.lineTo(W,y);ctx.stroke();}

  // Sensor dots + labels
  for(const s of sensors){
    const sx=s.nx*W, sy=s.ny*H;
    // glow ring
    ctx.beginPath(); ctx.arc(sx,sy,14,0,Math.PI*2);
    ctx.fillStyle='rgba(0,0,0,0.4)'; ctx.fill();
    // dot
    ctx.beginPath(); ctx.arc(sx,sy,7,0,Math.PI*2);
    ctx.fillStyle=s.dotColor; ctx.fill();
    ctx.strokeStyle='rgba(0,0,0,0.6)'; ctx.lineWidth=1.5; ctx.stroke();
    // label
    ctx.fillStyle='#d4ddd6'; ctx.font='bold 11px Courier New,monospace';
    ctx.textAlign='center';
    const fillTxt=s.fill!=null?s.fill.toFixed(0)+'%':'—';
    ctx.fillText(fillTxt, sx, sy-16);
    ctx.fillStyle='rgba(200,224,58,0.7)'; ctx.font='9px Courier New,monospace';
    ctx.fillText(s.label, sx, sy+20);
  }
}

// Hover probe on canvas
el('heatmap_canvas').addEventListener('mousemove',e=>{
  const canvas=el('heatmap_canvas');
  const rect=canvas.getBoundingClientRect();
  const scaleX=canvas.width/rect.width, scaleY=canvas.height/rect.height;
  const px=(e.clientX-rect.left)*scaleX, py=(e.clientY-rect.top)*scaleY;
  const nx=px/canvas.width, ny=py/canvas.height;
  const binH=(S.config&&S.config.bin_height)||100;
  const sensors=SENSOR_POS.map(s=>{
    const dist=S[s.id];
    const fill=dist==null?null:Math.max(0,Math.min(100,(1-dist/binH)*100));
    return {...s,fill};
  });
  const f=idwFill(nx,ny,sensors,binH);
  el('heatmap_probe').textContent=f!=null?`probe: ${f.toFixed(1)}% fill at (${(nx*100).toFixed(0)}%, ${(ny*100).toFixed(0)}%)` :'';
});
el('heatmap_canvas').addEventListener('mouseleave',()=>{ el('heatmap_probe').textContent=''; });

// ── Port selector ─────────────────────────────────────────────────────
async function loadPorts(){
  try{
    const d=await fetch('/api/devices').then(r=>r.json());
    const sel=el('port_sel');
    const cur=sel.value||d.current;
    sel.innerHTML='';
    if(!d.ports.length){ const o=document.createElement('option'); o.textContent='No ports found'; o.value=''; sel.appendChild(o); return; }
    d.ports.forEach(p=>{ const o=document.createElement('option'); o.value=p.port; o.textContent=p.port+(p.desc&&p.desc!==p.port?' — '+p.desc.slice(0,28):''); sel.appendChild(o); });
    sel.value=cur||d.current||d.ports[0].port;
  }catch(e){ console.warn('loadPorts',e); }
}
async function connectPort(port){
  await fetch('/api/connect',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({port})});
}
el('port_sel').addEventListener('change',e=>connectPort(e.target.value));
el('btn_refresh_ports').addEventListener('click',loadPorts);
loadPorts();

// ── SSE ───────────────────────────────────────────────────────────────
fetch('/api/snapshot').then(r=>r.json()).then(snap=>{ applyState(snap.state); (snap.log||[]).forEach(appendLog); });
const es=new EventSource('/api/stream');
es.onmessage=ev=>{
  const msg=JSON.parse(ev.data);
  if(msg.type==='log') appendLog(msg.entry);
  else if(msg.type==='state') applyState(msg.state);
  else if(msg.type==='flash') flashEvent(msg);
};
es.onerror=()=>{ el('conn_dot').classList.remove('ok'); el('hdr_status').textContent='reconnecting…'; el('live_badge').style.display='none'; };
</script>
</body>
</html>
"""


class Handler(BaseHTTPRequestHandler):
    bridge: "Bridge" = None  # set by main

    def log_message(self, fmt, *args):
        return  # quiet

    def _json(self, obj, code=200):
        body = json.dumps(obj).encode("utf-8")
        self.send_response(code)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def _read_json(self):
        length = int(self.headers.get("Content-Length") or 0)
        if length == 0:
            return {}
        try:
            return json.loads(self.rfile.read(length).decode("utf-8"))
        except Exception:
            return {}

    def do_POST(self):
        if self.path == "/api/config":
            body = self._read_json()
            persisted = self.bridge.set_invert(
                invert_lid=body.get("invert_lid"),
                invert_fill=body.get("invert_fill"),
                persist=bool(body.get("persist")),
            )
            self._json({"ok": True, "persisted": persisted,
                        "state": self.bridge.state})
            return
        if self.path == "/api/send":
            body = self._read_json()
            line = (body or {}).get("line", "")
            ok = self.bridge.send_line(line) if line else False
            self._json({"ok": ok})
            return
        if self.path == "/api/flash":
            body = self._read_json()
            env = (body or {}).get("env", "tc2")
            self.bridge.flash_firmware(env=env)
            self._json({"ok": True, "env": env}, code=202)
            return
        if self.path == "/api/connect":
            body = self._read_json()
            port = (body or {}).get("port", "")
            if not port:
                self._json({"ok": False, "error": "missing port"}, code=400)
                return
            self.bridge.switch_port(port)
            self._json({"ok": True, "port": port})
            return
        if self.path == "/api/ota":
            body = self._read_json()
            ip = (body or {}).get("ip", "").strip()
            if not ip:
                self._json({"ok": False, "error": "ip required"}, code=400)
                return
            user = (body or {}).get("user", "admin")
            password = (body or {}).get("pass", "ecoroute")
            t = threading.Thread(
                target=self.bridge.push_ota,
                args=(ip, 80, user, password),
                daemon=True,
            )
            t.start()
            self._json({"ok": True, "msg": f"OTA push started to {ip}"})
            return

        if self.path == "/api/upload-bin":
            # Multipart upload — save .bin to uploads/firmware/ relative to this script
            import email
            import email.message
            ctype = self.headers.get("Content-Type", "")
            if "multipart/form-data" not in ctype:
                self._json({"ok": False, "error": "multipart/form-data required"}, code=400)
                return
            length = int(self.headers.get("Content-Length", 0))
            if length <= 0 or length > 4 * 1024 * 1024:
                self._json({"ok": False, "error": "invalid or missing Content-Length (max 4 MB)"}, code=400)
                return
            # Build a fake HTTP message so email.message can parse the multipart body
            body_bytes = self.rfile.read(length)
            msg_bytes = b"Content-Type: " + ctype.encode() + b"\r\n\r\n" + body_bytes
            msg = email.message_from_bytes(msg_bytes)
            filename = None
            data = None
            for part in msg.walk():
                cd = part.get("Content-Disposition", "")
                if "filename" in cd:
                    # Extract filename safely — no path traversal
                    raw_name = part.get_filename() or "upload.bin"
                    safe_name = os.path.basename(raw_name.replace("\\", "/"))
                    if not safe_name.endswith(".bin"):
                        self._json({"ok": False, "error": "only .bin files accepted"}, code=400)
                        return
                    filename = safe_name
                    data = part.get_payload(decode=True)
                    break
            if filename is None or not data:
                self._json({"ok": False, "error": "no .bin file found in upload"}, code=400)
                return
            upload_dir = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "backend", "uploads", "firmware")
            os.makedirs(upload_dir, exist_ok=True)
            dest = os.path.join(upload_dir, filename)
            with open(dest, "wb") as fh:
                fh.write(data)
            self.bridge.broadcast({"type": "upload-bin", "filename": filename, "size": len(data)})
            self._json({"ok": True, "filename": filename, "size": len(data)})
            return

        self.send_error(404)

    def do_GET(self):
        if self.path == "/" or self.path == "/index.html":
            body = HTML_PAGE.encode("utf-8")
            self.send_response(200)
            self.send_header("Content-Type", "text/html; charset=utf-8")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)
            return

        if self.path == "/api/snapshot":
            body = json.dumps({
                "state": self.bridge.state,
                "log": self.bridge.log[-200:],
            }).encode("utf-8")
            self.send_response(200)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)
            return

        if self.path == "/api/devices":
            ports = [
                {"port": p.device, "desc": p.description}
                for p in serial.tools.list_ports.comports()
            ]
            self._json({"ports": ports, "current": self.bridge.port})
            return
        if self.path == "/api/stream":
            self.send_response(200)
            self.send_header("Content-Type", "text/event-stream")
            self.send_header("Cache-Control", "no-cache")
            self.send_header("Connection", "keep-alive")
            self.end_headers()
            q = self.bridge.subscribe()
            try:
                # Push current state immediately
                self.wfile.write(f"data: {json.dumps({'type':'state','state':self.bridge.state})}\n\n".encode())
                self.wfile.flush()
                while True:
                    try:
                        data = q.get(timeout=15)
                        self.wfile.write(f"data: {data}\n\n".encode())
                        self.wfile.flush()
                    except queue.Empty:
                        self.wfile.write(b": ping\n\n")
                        self.wfile.flush()
            except (BrokenPipeError, ConnectionResetError):
                pass
            finally:
                self.bridge.unsubscribe(q)
            return

        self.send_error(404)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", default="/dev/cu.usbserial-0001")
    ap.add_argument("--http", type=int, default=8765)
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--invert-lid", action="store_true",
                    help="Reverse IR lid readings (default: off)")
    ap.add_argument("--invert-fill", action="store_true",
                    help="Reverse fill %% to 100-fill (default: off)")
    args = ap.parse_args()

    bridge = Bridge(args.port,
                    invert_lid=args.invert_lid,
                    invert_fill=args.invert_fill)
    t = threading.Thread(target=bridge.run, daemon=True)
    t.start()

    Handler.bridge = bridge
    httpd = ThreadingHTTPServer((args.host, args.http), Handler)
    print(f"[serial-web] serial={args.port} baud={BAUD}")
    print(f"[serial-web] http://{args.host}:{args.http}")
    try:
        httpd.serve_forever()
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    main()
