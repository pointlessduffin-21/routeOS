#!/usr/bin/env python3
"""
routeOS Desktop Companion  v2.0
  · Serial Monitor         -- live ESP32 serial console
  · Provisioning           -- WiFi / MQTT / device config over HTTP
  · OTA Firmware Update    -- upload .bin to device
  · Sensor Catalog         -- browse 22+ sensors with live pin configurator

Dependencies: pip install PyQt6 pyserial requests
"""

import sys, os, threading, time, json, re as _re, subprocess
import serial, serial.tools.list_ports
import requests
from pathlib import Path

from PyQt6.QtWidgets import (
    QApplication, QMainWindow, QWidget, QStackedWidget,
    QVBoxLayout, QHBoxLayout, QFormLayout,
    QLabel, QLineEdit, QPushButton, QComboBox, QTextEdit,
    QFileDialog, QProgressBar, QGroupBox, QSpinBox, QDoubleSpinBox,
    QSplitter, QListWidget, QListWidgetItem, QMessageBox,
    QStatusBar, QCheckBox, QScrollArea, QFrame, QSizePolicy,
    QAbstractItemView,
)
from PyQt6.QtCore import Qt, QThread, pyqtSignal
from PyQt6.QtGui import QFont, QColor, QTextCursor


# -- Palette -------------------------------------------------------------------
BG       = "#0c0c14"
SURF     = "#12121e"
SURF2    = "#1a1a2a"
SURF3    = "#20202e"
BORDER   = "#2a2a3e"
ACCENT   = "#7c6af7"
ACCENT_B = "#5b4fe0"
ACCENT_T = "#a89ff8"
SUCCESS  = "#22c55e"
WARNING  = "#f59e0b"
ERROR_C  = "#ef4444"
TEXT     = "#e2e8f0"
TEXT_M   = "#8892a4"
TEXT_D   = "#4a5568"
TERM_BG  = "#08080f"

UI_FONT = "'Inter', 'Segoe UI', 'SF Pro Display', sans-serif"
MONO    = "'JetBrains Mono', 'Fira Code', 'Cascadia Code', 'Consolas', monospace"

SS = f"""
QMainWindow, QWidget {{
    background: {BG};
    color: {TEXT};
    font-family: {UI_FONT};
    font-size: 13px;
}}
QScrollArea, QScrollArea > QWidget > QWidget {{ background: transparent; }}
QScrollBar:vertical {{
    background: {SURF};  width: 6px;  border-radius: 3px;
}}
QScrollBar::handle:vertical {{
    background: {BORDER}; border-radius: 3px; min-height: 24px;
}}
QScrollBar::add-line:vertical, QScrollBar::sub-line:vertical {{ height: 0; }}
QFrame#card {{
    background: {SURF};
    border: 1px solid {BORDER};
    border-radius: 12px;
}}
QGroupBox {{
    background: {SURF};
    border: 1px solid {BORDER};
    border-radius: 10px;
    margin-top: 18px;
    padding: 14px 12px 10px 12px;
    color: {TEXT_M};
    font-size: 11px;
    font-weight: 600;
    letter-spacing: 0.08em;
    text-transform: uppercase;
}}
QGroupBox::title {{
    subcontrol-origin: margin;
    left: 12px;
    top: 2px;
    color: {TEXT_M};
}}
QLineEdit, QSpinBox, QDoubleSpinBox, QComboBox {{
    background: {SURF2};
    color: {TEXT};
    border: 1px solid {BORDER};
    border-radius: 8px;
    padding: 7px 11px;
    font-size: 13px;
    selection-background-color: {ACCENT_B};
}}
QLineEdit:focus, QSpinBox:focus, QDoubleSpinBox:focus, QComboBox:focus {{
    border-color: {ACCENT};
    background: {SURF3};
}}
QLineEdit[readOnly="true"] {{ color: {TEXT_M}; background: {SURF}; }}
QComboBox::drop-down {{ border: none; padding-right: 8px; }}
QComboBox QAbstractItemView {{
    background: {SURF2};
    border: 1px solid {BORDER};
    border-radius: 8px;
    selection-background-color: {SURF3};
    outline: none;
}}
QSpinBox::up-button, QSpinBox::down-button,
QDoubleSpinBox::up-button, QDoubleSpinBox::down-button {{
    background: {SURF3}; border: none; width: 18px; border-radius: 4px;
}}
QPushButton {{
    background: {SURF2};
    color: {TEXT};
    border: 1px solid {BORDER};
    border-radius: 8px;
    padding: 7px 16px;
    font-size: 13px;
    font-weight: 500;
}}
QPushButton:hover  {{ background: {SURF3}; border-color: {ACCENT}; color: {ACCENT_T}; }}
QPushButton:pressed {{ background: {SURF}; }}
QPushButton:disabled {{ color: {TEXT_D}; border-color: {SURF3}; }}
QPushButton#primary {{
    background: {ACCENT_B}; color: #fff; border: none; font-weight: 600;
}}
QPushButton#primary:hover  {{ background: {ACCENT}; }}
QPushButton#danger  {{ background: #2a0a0a; color: {ERROR_C}; border-color: #5a1a1a; }}
QPushButton#danger:hover   {{ background: {ERROR_C}; color: #fff; }}
QPushButton#success {{ background: #0a2a14; color: {SUCCESS}; border-color: #1a5a2a; }}
QPushButton#success:hover  {{ background: {SUCCESS}; color: #000; }}
QPushButton#ghost   {{
    background: transparent; border: none; color: {TEXT_M}; padding: 6px 10px;
}}
QPushButton#ghost:hover {{ background: {SURF2}; color: {TEXT}; border-radius: 6px; }}
QPushButton#nav {{
    background: transparent;
    border: none;
    border-radius: 10px;
    color: {TEXT_M};
    text-align: left;
    padding: 10px 14px;
    font-size: 13px;
    font-weight: 500;
}}
QPushButton#nav:hover {{ background: {SURF2}; color: {TEXT}; }}
QPushButton#nav[active="true"] {{
    background: {SURF2};
    color: {ACCENT_T};
    border: 1px solid {BORDER};
}}
QTextEdit {{
    background: {TERM_BG};
    color: #cdd9e5;
    border: 1px solid {BORDER};
    border-radius: 10px;
    font-family: {MONO};
    font-size: 12px;
    selection-background-color: {ACCENT_B};
}}
QListWidget {{
    background: {SURF};
    color: {TEXT};
    border: 1px solid {BORDER};
    border-radius: 10px;
    outline: none;
    padding: 4px;
}}
QListWidget::item {{
    padding: 9px 12px;
    border-radius: 7px;
    margin: 1px 2px;
}}
QListWidget::item:hover    {{ background: {SURF2}; }}
QListWidget::item:selected {{ background: {SURF3}; color: {ACCENT_T}; }}
QProgressBar {{
    background: {SURF2};
    border: 1px solid {BORDER};
    border-radius: 6px;
    height: 8px;
    text-align: center;
    color: transparent;
}}
QProgressBar::chunk {{
    background: qlineargradient(x1:0,y1:0,x2:1,y2:0, stop:0 {ACCENT_B}, stop:1 {ACCENT});
    border-radius: 6px;
}}
QSplitter::handle {{ background: {BORDER}; margin: 2px; border-radius: 2px; }}
QCheckBox {{ spacing: 8px; color: {TEXT_M}; }}
QCheckBox::indicator {{
    width: 16px; height: 16px;
    border: 1px solid {BORDER};
    border-radius: 4px;
    background: {SURF2};
}}
QCheckBox::indicator:checked {{ background: {ACCENT_B}; border-color: {ACCENT_B}; }}
QStatusBar {{
    background: {SURF};
    border-top: 1px solid {BORDER};
    color: {TEXT_D};
    font-size: 11px;
}}
"""


# -- Helpers -------------------------------------------------------------------
_CFG_DIR      = Path.home() / ".routeos"
_PIN_CFG_FILE = _CFG_DIR / "sensor_pins.json"

def _load_pin_configs() -> dict:
    try:
        if _PIN_CFG_FILE.exists():
            return json.loads(_PIN_CFG_FILE.read_text())
    except Exception:
        pass
    return {}

def _save_pin_configs(configs: dict) -> None:
    try:
        _CFG_DIR.mkdir(parents=True, exist_ok=True)
        _PIN_CFG_FILE.write_text(json.dumps(configs, indent=2))
    except Exception as e:
        print(f"[pin_cfg] {e}")

def _snake(name: str) -> str:
    s = _re.sub(r"([a-z0-9])([A-Z])", r"\1_\2", name)
    return _re.sub(r"([A-Z]+)([A-Z][a-z])", r"\1_\2", s).upper()

def h_sep() -> QFrame:
    f = QFrame()
    f.setFrameShape(QFrame.Shape.HLine)
    f.setStyleSheet(f"color: {BORDER};")
    return f

def lbl(text: str, muted=False, size: int = 0) -> QLabel:
    l = QLabel(text)
    style = f"color: {TEXT_M if muted else TEXT};"
    if size:
        style += f" font-size: {size}px;"
    l.setStyleSheet(style)
    return l


# -- Background threads --------------------------------------------------------
class SerialThread(QThread):
    line_received = pyqtSignal(str)
    error         = pyqtSignal(str)

    def __init__(self, port: str, baud: int = 115200):
        super().__init__()
        self.port   = port
        self.baud   = baud
        self._stop  = threading.Event()
        self.ser: serial.Serial | None = None

    def run(self):
        try:
            self.ser = serial.Serial(self.port, self.baud, timeout=1)
            while not self._stop.is_set():
                try:
                    raw = self.ser.readline()
                    if raw:
                        line = raw.decode("utf-8", errors="replace").rstrip()
                        if line:
                            self.line_received.emit(line)
                except serial.SerialException as e:
                    self.error.emit(str(e)); break
        except Exception as e:
            self.error.emit(str(e))

    def write(self, data: bytes):
        if self.ser and self.ser.is_open:
            self.ser.write(data)

    def stop(self):
        self._stop.set()
        if self.ser:
            try: self.ser.close()
            except: pass


class OtaThread(QThread):
    progress = pyqtSignal(int)
    status   = pyqtSignal(str)
    done     = pyqtSignal(bool, str)

    def __init__(self, device_ip: str, firmware_path: str):
        super().__init__()
        self.device_ip     = device_ip
        self.firmware_path = firmware_path

    def run(self):
        url  = f"http://{self.device_ip}/api/ota"
        path = Path(self.firmware_path)
        size = path.stat().st_size
        self.status.emit(f"Uploading {path.name} ({size:,} bytes)")
        try:
            data = path.read_bytes()

            class ProgressReader:
                def __init__(self_, cb):
                    self_._pos = 0
                    self_._cb  = cb
                def read(self_, n=-1):
                    chunk = data[self_._pos: self_._pos + (n if n > 0 else len(data))]
                    self_._pos += len(chunk)
                    self_._cb(int(self_._pos * 100 / size) if size else 100)
                    return chunk

            resp = requests.post(
                url, data=ProgressReader(self.progress.emit),
                headers={"Content-Type": "application/octet-stream",
                         "Content-Length": str(size)},
                timeout=120,
            )
            if resp.status_code == 200:
                self.done.emit(True, "OTA complete -- device rebooting")
            else:
                self.done.emit(False, f"HTTP {resp.status_code}: {resp.text}")
        except Exception as e:
            self.done.emit(False, str(e))


# -- Shared log widget ---------------------------------------------------------
class LogView(QTextEdit):
    def __init__(self):
        super().__init__()
        self.setReadOnly(True)
        self.setFont(QFont("JetBrains Mono", 11))

    def append_line(self, text: str, color: str = TEXT):
        ts = time.strftime("%H:%M:%S")
        self.setTextColor(QColor(TEXT_D))
        self.insertPlainText(f"{ts}  ")
        self.setTextColor(QColor(color))
        self.insertPlainText(text + "\n")
        self.moveCursor(QTextCursor.MoveOperation.End)


# -- Status dot ----------------------------------------------------------------
class StatusDot(QLabel):
    def __init__(self, color: str = TEXT_D):
        super().__init__("●")
        self.set_color(color)

    def set_color(self, color: str):
        self.setStyleSheet(f"font-size: 10px; color: {color};")


# =============================================================================
# Page 1 -- Serial Monitor
# =============================================================================
class SerialPage(QWidget):
    _sig = pyqtSignal(str, str)

    def __init__(self):
        super().__init__()
        self._thread: SerialThread | None = None
        self._sig.connect(lambda t, c: self.log.append_line(t, c))
        self._build()

    def _build(self):
        root = QVBoxLayout(self)
        root.setContentsMargins(24, 20, 24, 20)
        root.setSpacing(14)

        # header
        hdr = QHBoxLayout()
        title = QLabel("Serial Monitor")
        title.setStyleSheet(f"font-size: 20px; font-weight: 700; color: {TEXT};")
        self.dot    = StatusDot()
        self.status = lbl("disconnected", muted=True)
        hdr.addWidget(title)
        hdr.addSpacing(10)
        hdr.addWidget(self.dot)
        hdr.addWidget(self.status)
        hdr.addStretch()
        root.addLayout(hdr)

        # toolbar
        bar_w = QFrame()
        bar_w.setObjectName("card")
        bar_w.setStyleSheet(
            f"QFrame#card {{ background: {SURF}; border: 1px solid {BORDER}; "
            f"border-radius: 10px; padding: 6px 14px; }}"
        )
        tc = QHBoxLayout(bar_w)
        tc.setSpacing(8)
        tc.setContentsMargins(8, 6, 8, 6)
        self.port_combo = QComboBox()
        self.port_combo.setMinimumWidth(220)
        self.baud_combo = QComboBox()
        for b in ["9600","19200","38400","57600","115200","230400","460800","921600"]:
            self.baud_combo.addItem(b)
        self.baud_combo.setCurrentText("115200")
        self.baud_combo.setFixedWidth(110)
        self.refresh_btn = QPushButton("⟳")
        self.refresh_btn.setObjectName("ghost")
        self.refresh_btn.setFixedWidth(36)
        self.connect_btn = QPushButton("Connect")
        self.connect_btn.setObjectName("primary")
        self.connect_btn.setFixedWidth(100)
        self.clear_btn = QPushButton("Clear")
        self.clear_btn.setObjectName("ghost")
        tc.addWidget(lbl("Port", muted=True))
        tc.addWidget(self.port_combo, 1)
        tc.addWidget(lbl("Baud", muted=True))
        tc.addWidget(self.baud_combo)
        tc.addWidget(self.refresh_btn)
        tc.addStretch()
        tc.addWidget(self.clear_btn)
        tc.addWidget(self.connect_btn)
        root.addWidget(bar_w)

        # log
        self.log = LogView()
        root.addWidget(self.log, 1)

        # send bar
        sb = QHBoxLayout()
        sb.setSpacing(8)
        self.send_edit = QLineEdit()
        self.send_edit.setPlaceholderText("Send command...  (Enter to send)")
        self.send_edit.setStyleSheet(
            f"background: {SURF2}; border: 1px solid {BORDER}; border-radius: 8px; "
            f"padding: 8px 12px; font-family: {MONO}; font-size: 12px;"
        )
        self.nl_check = QCheckBox("\\n")
        self.nl_check.setChecked(True)
        self.send_btn = QPushButton("Send  ->")
        self.send_btn.setObjectName("primary")
        self.send_btn.setFixedWidth(90)
        sb.addWidget(self.send_edit, 1)
        sb.addWidget(self.nl_check)
        sb.addWidget(self.send_btn)
        root.addLayout(sb)

        self.refresh_btn.clicked.connect(self._refresh_ports)
        self.connect_btn.clicked.connect(self._toggle_connect)
        self.clear_btn.clicked.connect(self.log.clear)
        self.send_btn.clicked.connect(self._send)
        self.send_edit.returnPressed.connect(self._send)
        self._refresh_ports()

    def _refresh_ports(self):
        self.port_combo.clear()
        for p in serial.tools.list_ports.comports():
            self.port_combo.addItem(p.device, p.device)

    def _toggle_connect(self):
        if self._thread and self._thread.isRunning():
            self._thread.stop(); self._thread.wait(); self._thread = None
            self.connect_btn.setText("Connect")
            self.connect_btn.setObjectName("primary")
            self.connect_btn.setStyle(self.connect_btn.style())
            self.dot.set_color(TEXT_D)
            self.status.setText("disconnected")
            self._sig.emit("[disconnected]", TEXT_M)
        else:
            port = self.port_combo.currentData() or ""
            if not port:
                self._sig.emit("No port selected", ERROR_C); return
            baud = int(self.baud_combo.currentText())
            self._thread = SerialThread(port, baud)
            self._thread.line_received.connect(self._on_line)
            self._thread.error.connect(lambda e: self._sig.emit(f"[error] {e}", ERROR_C))
            self._thread.start()
            self.connect_btn.setText("Disconnect")
            self.connect_btn.setObjectName("danger")
            self.connect_btn.setStyle(self.connect_btn.style())
            self.dot.set_color(SUCCESS)
            self.status.setText(f"{port} @ {baud}")
            self._sig.emit(f"[connected] {port} @ {baud}", SUCCESS)

    def _on_line(self, line: str):
        lo = line.lower()
        c = (ERROR_C  if any(x in lo for x in ("[error]","[fail]","[e]"))
             else WARNING if any(x in lo for x in ("[warn]","[w]"))
             else SUCCESS  if any(x in lo for x in ("[ok]","[boot]","[i]","[info]"))
             else TEXT)
        self._sig.emit(line, c)

    def _send(self):
        if not (self._thread and self._thread.isRunning()): return
        text = self.send_edit.text()
        if not text: return
        self._thread.write((text + ("\n" if self.nl_check.isChecked() else "")).encode())
        self._sig.emit(f">> {text}", ACCENT_T)
        self.send_edit.clear()


# =============================================================================
# Page 2 -- Provisioning
# =============================================================================
class ProvisionPage(QWidget):
    _sig = pyqtSignal(str, str)

    def __init__(self):
        super().__init__()
        self._sig.connect(lambda t, c: self.log.append_line(t, c))
        self._build()

    def _build(self):
        root = QVBoxLayout(self)
        root.setContentsMargins(24, 20, 24, 20)
        root.setSpacing(14)

        title = QLabel("Device Provisioning")
        title.setStyleSheet(f"font-size: 20px; font-weight: 700; color: {TEXT};")
        root.addWidget(title)

        body = QHBoxLayout(); body.setSpacing(16)
        root.addLayout(body, 1)

        forms = QVBoxLayout(); forms.setSpacing(12)
        body.addLayout(forms, 1)

        tgt = QGroupBox("Target Device")
        tg  = QFormLayout(tgt); tg.setSpacing(10)
        self.ip_edit = QLineEdit("192.168.4.1")
        self.ip_edit.setPlaceholderText("192.168.4.1 (ESP32 AP mode)")
        tg.addRow("Device IP", self.ip_edit)
        forms.addWidget(tgt)

        wf = QGroupBox("WiFi Network")
        wl = QFormLayout(wf); wl.setSpacing(10)
        self.ssid_edit = QLineEdit(); self.ssid_edit.setPlaceholderText("WiFi SSID")
        self.pass_edit = QLineEdit(); self.pass_edit.setEchoMode(QLineEdit.EchoMode.Password)
        wl.addRow("SSID", self.ssid_edit); wl.addRow("Password", self.pass_edit)
        forms.addWidget(wf)

        mq = QGroupBox("MQTT Broker")
        ml = QFormLayout(mq); ml.setSpacing(10)
        self.mqtt_host  = QLineEdit(); self.mqtt_host.setPlaceholderText("e.g. 192.168.1.100")
        self.mqtt_port  = QSpinBox(); self.mqtt_port.setRange(1, 65535); self.mqtt_port.setValue(1883)
        self.mqtt_user  = QLineEdit()
        self.mqtt_pass  = QLineEdit(); self.mqtt_pass.setEchoMode(QLineEdit.EchoMode.Password)
        self.mqtt_topic = QLineEdit(); self.mqtt_topic.setPlaceholderText("plant/zone/device")
        ml.addRow("Host", self.mqtt_host); ml.addRow("Port", self.mqtt_port)
        ml.addRow("Username", self.mqtt_user); ml.addRow("Password", self.mqtt_pass)
        ml.addRow("Topic", self.mqtt_topic)
        forms.addWidget(mq)

        dv = QGroupBox("Device Identity")
        dl = QFormLayout(dv); dl.setSpacing(10)
        self.dev_code   = QLineEdit(); self.dev_code.setPlaceholderText("e.g. ECO-BIN-1001")
        self.bin_height = QSpinBox()
        self.bin_height.setRange(10, 500); self.bin_height.setValue(60); self.bin_height.setSuffix(" cm")
        dl.addRow("Device Code", self.dev_code); dl.addRow("Container Height", self.bin_height)
        forms.addWidget(dv)

        ab = QHBoxLayout(); ab.setSpacing(8)
        self.fetch_btn  = QPushButton("Fetch Config")
        self.apply_btn  = QPushButton("Apply All"); self.apply_btn.setObjectName("primary")
        self.reboot_btn = QPushButton("Reboot")
        self.reset_btn  = QPushButton("Factory Reset"); self.reset_btn.setObjectName("danger")
        for b in (self.fetch_btn, self.apply_btn, self.reboot_btn, self.reset_btn):
            ab.addWidget(b)
        forms.addLayout(ab); forms.addStretch()

        log_v = QVBoxLayout()
        log_v.addWidget(lbl("Activity Log", muted=True))
        self.log = LogView()
        log_v.addWidget(self.log, 1)
        body.addLayout(log_v, 1)

        self.fetch_btn.clicked.connect(self._fetch)
        self.apply_btn.clicked.connect(self._apply)
        self.reboot_btn.clicked.connect(self._reboot)
        self.reset_btn.clicked.connect(self._factory_reset)

    def _url(self, p): return f"http://{self.ip_edit.text().strip()}{p}"

    def _fetch(self):
        def _do():
            try:
                r = requests.get(self._url("/api/status"), timeout=5); d = r.json()
                self._sig.emit("Config fetched:", SUCCESS)
                self._sig.emit(json.dumps(d, indent=2), TEXT_M)
                cfg = d.get("config", {})
                if cfg.get("ssid"):       self.ssid_edit.setText(cfg["ssid"])
                if cfg.get("mqtt_host"):  self.mqtt_host.setText(cfg["mqtt_host"])
                if cfg.get("mqtt_port"):  self.mqtt_port.setValue(int(cfg["mqtt_port"]))
                if cfg.get("mqtt_topic"): self.mqtt_topic.setText(cfg["mqtt_topic"])
                if cfg.get("device_code"):self.dev_code.setText(cfg["device_code"])
            except Exception as e:
                self._sig.emit(f"Error: {e}", ERROR_C)
        threading.Thread(target=_do, daemon=True).start()

    def _apply(self):
        ip = self.ip_edit.text().strip()
        def _do():
            for name, path, payload in [
                ("WiFi",   "/api/wifi",   {"ssid": self.ssid_edit.text(), "password": self.pass_edit.text()}),
                ("MQTT",   "/api/mqtt",   {"host": self.mqtt_host.text(), "port": self.mqtt_port.value(),
                                           "user": self.mqtt_user.text(), "password": self.mqtt_pass.text(),
                                           "topic": self.mqtt_topic.text()}),
                ("Device", "/api/device", {"device_code": self.dev_code.text(),
                                           "bin_height_cm": self.bin_height.value()}),
            ]:
                try:
                    r = requests.post(f"http://{ip}{path}", json=payload, timeout=5)
                    self._sig.emit(f"{name} -> HTTP {r.status_code}", SUCCESS if r.ok else ERROR_C)
                except Exception as e:
                    self._sig.emit(f"{name} error: {e}", ERROR_C)
            self._sig.emit("Done -- reboot to apply changes", WARNING)
        threading.Thread(target=_do, daemon=True).start()

    def _reboot(self):
        def _do():
            try:
                r = requests.post(self._url("/api/reboot"), timeout=5)
                self._sig.emit(f"Reboot -> {r.status_code}", SUCCESS if r.ok else ERROR_C)
            except Exception as e:
                self._sig.emit(f"Error: {e}", ERROR_C)
        threading.Thread(target=_do, daemon=True).start()

    def _factory_reset(self):
        if QMessageBox.question(self, "Factory Reset",
                "Erase all config on this device?") != QMessageBox.StandardButton.Yes:
            return
        def _do():
            try:
                r = requests.post(self._url("/api/factory-reset"), timeout=5)
                self._sig.emit(f"Factory reset -> {r.status_code}", SUCCESS if r.ok else ERROR_C)
            except Exception as e:
                self._sig.emit(f"Error: {e}", ERROR_C)
        threading.Thread(target=_do, daemon=True).start()


# =============================================================================
# Page 3 -- OTA Firmware Update
# =============================================================================
class OtaPage(QWidget):
    def __init__(self):
        super().__init__()
        self._thread: OtaThread | None = None
        self._build()

    def _build(self):
        root = QVBoxLayout(self)
        root.setContentsMargins(24, 20, 24, 20)
        root.setSpacing(14)

        title = QLabel("OTA Firmware Update")
        title.setStyleSheet(f"font-size: 20px; font-weight: 700; color: {TEXT};")
        root.addWidget(title)
        root.addWidget(lbl("Upload a compiled .bin to the ESP32 over HTTP.", muted=True))

        body = QHBoxLayout(); body.setSpacing(20)
        root.addLayout(body, 1)

        # left
        left = QVBoxLayout(); left.setSpacing(12)
        body.addLayout(left, 1)

        cfg = QGroupBox("Upload Settings")
        fl  = QFormLayout(cfg); fl.setSpacing(10)
        self.ip_edit = QLineEdit(); self.ip_edit.setPlaceholderText("e.g. 192.168.1.45")

        fw_row = QHBoxLayout(); fw_row.setSpacing(6)
        self.fw_edit = QLineEdit(); self.fw_edit.setReadOnly(True)
        self.fw_edit.setPlaceholderText("Select .bin firmware file...")
        browse = QPushButton("Browse..."); browse.clicked.connect(self._browse)
        fw_row.addWidget(self.fw_edit, 1); fw_row.addWidget(browse)
        fw_w = QWidget(); fw_w.setLayout(fw_row)

        fl.addRow("Device IP", self.ip_edit); fl.addRow("Firmware", fw_w)
        left.addWidget(cfg)

        prog_grp = QGroupBox("Progress")
        pg = QVBoxLayout(prog_grp); pg.setSpacing(8)
        self.progress   = QProgressBar(); self.progress.setValue(0)
        self.status_lbl = lbl("Ready to flash", muted=True)
        pg.addWidget(self.progress); pg.addWidget(self.status_lbl)
        left.addWidget(prog_grp)

        self.flash_btn = QPushButton("Flash Firmware")
        self.flash_btn.setObjectName("primary")
        self.flash_btn.setMinimumHeight(42)
        left.addWidget(self.flash_btn)
        left.addStretch()

        # right
        right = QVBoxLayout(); right.setSpacing(12)
        body.addLayout(right, 1)

        bld = QGroupBox("PlatformIO Build")
        bl  = QVBoxLayout(bld); bl.setSpacing(8)
        env_row = QHBoxLayout()
        env_row.addWidget(lbl("Env", muted=True))
        self.env_edit  = QLineEdit("tc2"); self.env_edit.setFixedWidth(80)
        self.build_btn = QPushButton("Build"); self.build_btn.setObjectName("primary")
        env_row.addWidget(self.env_edit); env_row.addWidget(self.build_btn); env_row.addStretch()
        bl.addLayout(env_row)
        self.build_log = LogView()
        bl.addWidget(self.build_log, 1)
        right.addWidget(bld, 1)
        right.addWidget(lbl(".pio/build/<env>/firmware.bin", muted=True))

        self.flash_btn.clicked.connect(self._flash)
        self.build_btn.clicked.connect(self._build_fw)

    def _browse(self):
        path, _ = QFileDialog.getOpenFileName(self, "Select firmware .bin", "", "Binary (*.bin)")
        if path: self.fw_edit.setText(path)

    def _flash(self):
        ip = self.ip_edit.text().strip(); path = self.fw_edit.text().strip()
        if not ip or not path:
            QMessageBox.warning(self, "Missing info", "Enter device IP and select a .bin file.")
            return
        self.flash_btn.setEnabled(False); self.progress.setValue(0)
        self._thread = OtaThread(ip, path)
        self._thread.progress.connect(self.progress.setValue)
        self._thread.status.connect(self.status_lbl.setText)
        self._thread.done.connect(self._on_done)
        self._thread.start()

    def _on_done(self, success: bool, msg: str):
        self.flash_btn.setEnabled(True)
        self.status_lbl.setText(msg)
        self.status_lbl.setStyleSheet(f"color: {SUCCESS if success else ERROR_C};")
        if success:
            self.progress.setValue(100)
            QMessageBox.information(self, "OTA Complete", msg)
        else:
            QMessageBox.critical(self, "OTA Failed", msg)

    def _build_fw(self):
        env  = self.env_edit.text().strip() or "tc2"
        root = str(Path(__file__).parent.parent)
        self.build_log.clear()
        self.build_log.append_line(f"pio run -e {env}", TEXT_M)
        def _do():
            proc = subprocess.Popen(["pio", "run", "-e", env], cwd=root,
                                    stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
            for line in proc.stdout:
                lo = line.lower()
                c = ERROR_C if "error" in lo else WARNING if "warning" in lo else TEXT_M
                self.build_log.append_line(line.rstrip(), c)
            proc.wait()
            if proc.returncode == 0:
                bin_path = Path(root) / ".pio" / "build" / env / "firmware.bin"
                if bin_path.exists(): self.fw_edit.setText(str(bin_path))
                self.build_log.append_line("Build success", SUCCESS)
            else:
                self.build_log.append_line(f"Build failed (exit {proc.returncode})", ERROR_C)
        threading.Thread(target=_do, daemon=True).start()


# =============================================================================
# Pin Configuration Panel
# =============================================================================
class PinConfigPanel(QWidget):
    config_saved = pyqtSignal(str, dict)

    def __init__(self):
        super().__init__()
        self._sensor: dict | None = None
        self._fields: dict        = {}
        self._configs             = _load_pin_configs()
        self._build()

    def _build(self):
        root = QVBoxLayout(self)
        root.setContentsMargins(0, 0, 0, 0)
        root.setSpacing(10)

        self._title = QLabel("Select a sensor to configure pins")
        self._title.setStyleSheet(
            f"font-size: 14px; font-weight: 600; color: {TEXT}; padding: 2px 0;"
        )
        root.addWidget(self._title)

        scroll = QScrollArea()
        scroll.setWidgetResizable(True)
        scroll.setFrameShape(QFrame.Shape.NoFrame)
        scroll.setHorizontalScrollBarPolicy(Qt.ScrollBarPolicy.ScrollBarAlwaysOff)
        self._form_w = QWidget()
        self._form   = QFormLayout(self._form_w)
        self._form.setSpacing(10)
        self._form.setLabelAlignment(Qt.AlignmentFlag.AlignRight | Qt.AlignmentFlag.AlignVCenter)
        self._form.setFieldGrowthPolicy(QFormLayout.FieldGrowthPolicy.ExpandingFieldsGrow)
        scroll.setWidget(self._form_w)
        root.addWidget(scroll, 1)

        acts = QHBoxLayout(); acts.setSpacing(8)
        self._btn_reset = QPushButton("Reset Defaults")
        self._btn_cpp   = QPushButton("Copy C++")
        self._btn_save  = QPushButton("Save"); self._btn_save.setObjectName("success")
        for b in (self._btn_reset, self._btn_cpp, self._btn_save):
            b.setEnabled(False); acts.addWidget(b)
        acts.addStretch()
        root.addLayout(acts)

        dev = QHBoxLayout(); dev.setSpacing(8)
        dev.addWidget(lbl("Device IP", muted=True))
        self._ip = QLineEdit("192.168.4.1"); self._ip.setFixedWidth(145)
        self._btn_send = QPushButton("Send to Device"); self._btn_send.setEnabled(False)
        dev.addWidget(self._ip); dev.addWidget(self._btn_send); dev.addStretch()
        root.addLayout(dev)

        self._status = QLabel("")
        self._status.setStyleSheet(f"font-size: 11px; color: {TEXT_M};")
        root.addWidget(self._status)

        self._btn_reset.clicked.connect(self._reset)
        self._btn_cpp.clicked.connect(self._copy_cpp)
        self._btn_save.clicked.connect(self._save)
        self._btn_send.clicked.connect(self._send)

    def load(self, sensor: dict):
        self._sensor = sensor
        schema       = sensor.get("configSchema", {})
        sensor_id    = sensor["id"]
        saved        = self._configs.get(sensor_id, {})
        self._title.setText(sensor["name"])

        while self._form.rowCount() > 0:
            self._form.removeRow(0)
        self._fields.clear()

        if not schema:
            self._form.addRow(lbl("No configurable pins for this sensor.", muted=True))
            for b in (self._btn_reset, self._btn_cpp, self._btn_save, self._btn_send):
                b.setEnabled(False)
            return

        for key, spec in schema.items():
            t       = spec.get("type", "string")
            default = spec.get("default", "")
            current = saved.get(key, default)
            tip     = spec.get("description", "")
            is_pin  = "pin" in key.lower()

            if t == "integer":
                w = QSpinBox()
                w.setRange(0, 39 if is_pin else 9999)
                if is_pin: w.setSuffix("  (GPIO)")
                w.setToolTip(tip or ("ESP32 GPIO pin number" if is_pin else ""))
                try:    w.setValue(int(current))
                except: w.setValue(int(default) if default != "" else 0)
            elif t == "number":
                w = QDoubleSpinBox()
                w.setRange(0.0, 99999.0); w.setDecimals(2)
                w.setToolTip(tip)
                try:    w.setValue(float(current))
                except: w.setValue(float(default) if default != "" else 0.0)
            else:
                w = QLineEdit()
                w.setText(str(current) if current != "" else str(default))
                w.setToolTip(tip)

            human     = _snake(key).replace("_", " ").title()
            customised = key in saved
            label = QLabel(human + ("  *" if customised else ""))
            label.setStyleSheet(
                f"color: {ACCENT_T if customised else TEXT_M}; font-size: 12px;"
            )
            if tip: label.setToolTip(tip)
            self._form.addRow(label, w)
            self._fields[key] = (w, spec)

        gmap = sensor.get("gpioMap", {})
        if gmap:
            hint = lbl("Defaults: " + "  ".join(f"{k}->{v}" for k, v in gmap.items()), muted=True)
            hint.setStyleSheet(f"color: {TEXT_D}; font-size: 11px;")
            self._form.addRow("", hint)

        for b in (self._btn_reset, self._btn_cpp, self._btn_save, self._btn_send):
            b.setEnabled(True)
        self._set_status("")

    def _current(self) -> dict:
        out = {}
        for k, (w, _) in self._fields.items():
            if isinstance(w, QSpinBox):         out[k] = w.value()
            elif isinstance(w, QDoubleSpinBox): out[k] = round(w.value(), 4)
            else:                               out[k] = w.text()
        return out

    def _reset(self):
        for k, (w, spec) in self._fields.items():
            d = spec.get("default", "")
            if isinstance(w, QSpinBox):
                try:    w.setValue(int(d))
                except: w.setValue(0)
            elif isinstance(w, QDoubleSpinBox):
                try:    w.setValue(float(d))
                except: w.setValue(0.0)
            else: w.setText(str(d) if d != "" else "")
        self._set_status("Reset to defaults -- click Save to persist")

    def _save(self):
        if not self._sensor: return
        sid = self._sensor["id"]
        cfg = self._current()
        self._configs[sid] = cfg
        _save_pin_configs(self._configs)
        self.config_saved.emit(sid, cfg)
        self.load(self._sensor)
        self._set_status(f"Saved to {_PIN_CFG_FILE}", SUCCESS)

    def _copy_cpp(self):
        if not self._sensor: return
        prefix = self._sensor["id"].upper().replace("-", "_")
        lines  = [f"// {self._sensor['id']} -- routeOS pin config"]
        for k, v in self._current().items():
            lines.append(f"#define {prefix}_{_snake(k):<36} {v}")
        QApplication.clipboard().setText("\n".join(lines))
        self._set_status("C++ snippet copied to clipboard", SUCCESS)

    def _send(self):
        if not self._sensor: return
        payload = {"sensorId": self._sensor["id"], "config": self._current()}
        url     = f"http://{self._ip.text().strip()}/api/sensors/config"
        self._set_status(f"Sending to {url}...")
        def _do():
            try:
                r = requests.post(url, json=payload, timeout=5)
                msg   = f"Device accepted ({r.status_code})" if r.ok else f"Error {r.status_code}: {r.text[:60]}"
                color = SUCCESS if r.ok else ERROR_C
            except Exception as e:
                msg, color = f"Error: {e}", ERROR_C
            self._set_status(msg, color)
        threading.Thread(target=_do, daemon=True).start()

    def _set_status(self, msg: str, color: str = TEXT_M):
        self._status.setStyleSheet(f"font-size: 11px; color: {color};")
        self._status.setText(msg)


# =============================================================================
# Page 4 -- Sensor Catalog
# =============================================================================
class SensorPage(QWidget):
    def __init__(self):
        super().__init__()
        self._sensors: list[dict] = []
        self._build()

    def _build(self):
        root = QVBoxLayout(self)
        root.setContentsMargins(24, 20, 24, 20)
        root.setSpacing(14)

        hdr = QHBoxLayout()
        title = QLabel("Sensor Catalog")
        title.setStyleSheet(f"font-size: 20px; font-weight: 700; color: {TEXT};")
        hdr.addWidget(title)
        hdr.addStretch()
        self.backend_edit = QLineEdit("http://localhost:4003")
        self.backend_edit.setFixedWidth(220)
        self.fetch_btn = QPushButton("Load Catalog"); self.fetch_btn.setObjectName("primary")
        hdr.addWidget(self.backend_edit); hdr.addWidget(self.fetch_btn)
        root.addLayout(hdr)

        self.search = QLineEdit()
        self.search.setPlaceholderText("Search by name, category, tag...")
        self.search.setStyleSheet(
            f"background: {SURF2}; border: 1px solid {BORDER}; border-radius: 8px; padding: 8px 12px;"
        )
        root.addWidget(self.search)

        split = QSplitter(Qt.Orientation.Horizontal)
        split.setChildrenCollapsible(False)

        self.list_w = QListWidget(); self.list_w.setMinimumWidth(220)
        split.addWidget(self.list_w)

        vsplit = QSplitter(Qt.Orientation.Vertical)
        vsplit.setChildrenCollapsible(False)

        # detail
        det = QFrame(); det.setObjectName("card")
        dl  = QVBoxLayout(det); dl.setContentsMargins(14, 12, 14, 12)
        dl.addWidget(lbl("Overview", muted=True, size=11))
        self.detail = QTextEdit(); self.detail.setReadOnly(True)
        self.detail.setFont(QFont("JetBrains Mono", 11))
        self.detail.setStyleSheet(
            f"background: {SURF}; border: none; font-family: {MONO}; "
            f"font-size: 11px; color: {TEXT_M};"
        )
        dl.addWidget(self.detail, 1)
        vsplit.addWidget(det)

        # pin config
        pin_f = QFrame(); pin_f.setObjectName("card")
        pl    = QVBoxLayout(pin_f); pl.setContentsMargins(16, 14, 16, 14); pl.setSpacing(10)

        ph = QHBoxLayout()
        ph_title = QLabel("Pin Configuration")
        ph_title.setStyleSheet(
            f"font-size: 13px; font-weight: 600; color: {ACCENT_T}; letter-spacing: 0.02em;"
        )
        ph_sub = lbl("* = customised", muted=True, size=11)
        ph.addWidget(ph_title); ph.addStretch(); ph.addWidget(ph_sub)
        pl.addLayout(ph)
        pl.addWidget(h_sep())

        self.pin_cfg = PinConfigPanel()
        pl.addWidget(self.pin_cfg, 1)
        vsplit.addWidget(pin_f)

        vsplit.setSizes([260, 400])
        split.addWidget(vsplit)
        split.setSizes([240, 900])
        root.addWidget(split, 1)

        self.fetch_btn.clicked.connect(self._fetch)
        self.list_w.currentRowChanged.connect(self._show)
        self.search.textChanged.connect(self._filter)

    def _fetch(self):
        url = f"{self.backend_edit.text().rstrip('/')}/api/marketplace"
        def _do():
            try:
                r    = requests.get(url, timeout=5)
                data = r.json().get("data", [])
                self._sensors = data
                self.list_w.clear()
                for s in data:
                    self.list_w.addItem(QListWidgetItem(s["name"]))
                self.detail.setTextColor(QColor(SUCCESS))
                self.detail.setText(f"Loaded {len(data)} sensors from {url}\n\nSelect a sensor from the list.")
            except Exception as e:
                self.detail.setTextColor(QColor(ERROR_C))
                self.detail.setText(f"Error: {e}\n\ndocker compose up -d")
        threading.Thread(target=_do, daemon=True).start()

    def _show(self, row: int):
        if row < 0 or row >= len(self._sensors): return
        s = self._sensors[row]
        SEP = "-" * 56
        lines = [SEP, f"  {s['name']}",
                 f"  {s['id']}  *  {s['category']}  *  {s.get('manufacturer','')}",
                 f"  Interface: {s.get('interface','')}",
                 SEP, "", f"  {s['description']}", "", "  SIGNALS"]
        for sig in s.get("signals", []):
            rng = f"[{sig.get('min','?')} - {sig.get('max','?')}]" if sig.get("min") is not None else ""
            alm = "  ".join(filter(None, [
                f"high:{sig['alarmHigh']}" if sig.get("alarmHigh") is not None else "",
                f"low:{sig['alarmLow']}"  if sig.get("alarmLow")  is not None else "",
            ]))
            lines.append(f"  {sig['name']:<26}{sig.get('unit',''):<8}  {rng:<18}  {alm}")
        if s.get("gpioMap"):
            lines += ["", "  GPIO DEFAULTS",
                      "  " + "  ".join(f"{k}->{v}" for k, v in s["gpioMap"].items())]
        lines += ["", "  MQTT TOPIC", f"  {s.get('mqttTopicPattern','')}"]
        if s.get("datasheet"):
            lines += ["", f"  Datasheet: {s['datasheet']}"]
        if s.get("tags"):
            lines += ["", "  Tags: " + "  /  ".join(s["tags"])]
        self.detail.setTextColor(QColor(TEXT_M))
        self.detail.setText("\n".join(lines))
        self.pin_cfg.load(s)

    def _filter(self, q: str):
        q = q.lower()
        for i in range(self.list_w.count()):
            item = self.list_w.item(i)
            if i < len(self._sensors):
                s  = self._sensors[i]
                ok = (not q or q in s["id"] or q in s["name"].lower()
                      or q in s.get("category","").lower()
                      or any(q in t for t in s.get("tags", [])))
                item.setHidden(not ok)


# =============================================================================
# Sidebar nav button
# =============================================================================
class NavButton(QPushButton):
    def __init__(self, icon: str, label: str):
        super().__init__(f"  {icon}   {label}")
        self.setObjectName("nav")
        self.setCursor(Qt.CursorShape.PointingHandCursor)
        self.setMinimumHeight(42)
        self.setSizePolicy(QSizePolicy.Policy.Expanding, QSizePolicy.Policy.Fixed)

    def set_active(self, active: bool):
        self.setProperty("active", "true" if active else "false")
        self.setStyle(self.style())


# =============================================================================
# Main Window
# =============================================================================
class MainWindow(QMainWindow):
    def __init__(self):
        super().__init__()
        self.setWindowTitle("routeOS")
        self.resize(1280, 820)
        self.setMinimumSize(900, 600)
        self._build()

    def _build(self):
        root = QWidget()
        self.setCentralWidget(root)
        outer = QHBoxLayout(root)
        outer.setContentsMargins(0, 0, 0, 0)
        outer.setSpacing(0)

        # Sidebar
        sidebar = QWidget()
        sidebar.setFixedWidth(204)
        sidebar.setStyleSheet(
            f"background: {SURF}; border-right: 1px solid {BORDER};"
        )
        sl = QVBoxLayout(sidebar)
        sl.setContentsMargins(12, 24, 12, 20)
        sl.setSpacing(4)

        logo = QLabel("routeOS")
        logo.setStyleSheet(
            f"font-size: 19px; font-weight: 800; color: {ACCENT_T}; "
            f"letter-spacing: -0.02em; padding: 0 4px 2px 4px;"
        )
        sub = QLabel("Desktop Companion")
        sub.setStyleSheet(
            f"font-size: 10px; color: {TEXT_D}; "
            f"padding: 0 4px 18px 4px; letter-spacing: 0.05em;"
        )
        sl.addWidget(logo)
        sl.addWidget(sub)

        sep = QFrame()
        sep.setFrameShape(QFrame.Shape.HLine)
        sep.setStyleSheet(f"color: {BORDER}; margin-bottom: 6px;")
        sl.addWidget(sep)

        nav_items = [
            ("📟", "Serial Monitor",   0),
            ("⚙",  "Provisioning",     1),
            ("⬆",  "OTA Update",       2),
            ("🔌", "Sensors",          3),
        ]
        self._nav_btns: list[NavButton] = []
        for icon, label, idx in nav_items:
            btn = NavButton(icon, label)
            btn.clicked.connect(lambda _, i=idx: self._switch(i))
            self._nav_btns.append(btn)
            sl.addWidget(btn)

        sl.addStretch()
        ver = QLabel("v2.0.0")
        ver.setStyleSheet(f"color: {TEXT_D}; font-size: 10px; padding: 4px;")
        sl.addWidget(ver)

        outer.addWidget(sidebar)

        # Content
        self._stack = QStackedWidget()
        self._stack.setStyleSheet(f"background: {BG};")
        self._pages = [SerialPage(), ProvisionPage(), OtaPage(), SensorPage()]
        for p in self._pages:
            self._stack.addWidget(p)
        outer.addWidget(self._stack, 1)

        bar = QStatusBar()
        bar.addWidget(QLabel("routeOS Desktop  *  v2.0.0"))
        self.setStatusBar(bar)

        self._switch(0)

    def _switch(self, index: int):
        self._stack.setCurrentIndex(index)
        for i, btn in enumerate(self._nav_btns):
            btn.set_active(i == index)


def main():
    app = QApplication(sys.argv)
    app.setStyleSheet(SS)
    app.setAttribute(Qt.ApplicationAttribute.AA_UseHighDpiPixmaps, True)
    win = MainWindow()
    win.show()
    sys.exit(app.exec())


if __name__ == "__main__":
    main()
