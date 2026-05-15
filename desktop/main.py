#!/usr/bin/env python3
"""
routeOS Desktop Companion
-  Serial monitor  (USB → ESP32 at 115200 baud)
-  Device provisioning  (WiFi / MQTT / device config via HTTP to 192.168.4.1)
-  OTA firmware upload  (POST binary to /api/ota on the device's IP)
-  Sensor marketplace browser  (reads from routeOS backend REST API)

Dependencies:  pip install PyQt6 pyserial requests
"""

import sys, os, threading, time, json
import serial, serial.tools.list_ports
import requests
from pathlib import Path

from PyQt6.QtWidgets import (
    QApplication, QMainWindow, QWidget, QTabWidget,
    QVBoxLayout, QHBoxLayout, QGridLayout, QFormLayout,
    QLabel, QLineEdit, QPushButton, QComboBox, QTextEdit,
    QFileDialog, QProgressBar, QGroupBox, QSpinBox,
    QSplitter, QListWidget, QListWidgetItem, QMessageBox,
    QStatusBar, QCheckBox,
)
from PyQt6.QtCore import Qt, QThread, pyqtSignal, QTimer
from PyQt6.QtGui import QFont, QColor, QTextCursor, QPalette


# ── Palette ──────────────────────────────────────────────────────────────────
DARK_BG   = "#1a1a2e"
PANEL_BG  = "#16213e"
ACCENT    = "#00d4aa"
ACCENT2   = "#0f3460"
TEXT      = "#e0e0e0"
MUTED     = "#888"
ERROR_CLR = "#ff4757"
WARN_CLR  = "#ffa502"
OK_CLR    = "#2ed573"

STYLESHEET = f"""
QMainWindow, QWidget {{ background: {DARK_BG}; color: {TEXT}; font-family: 'Consolas', 'Courier New', monospace; font-size: 13px; }}
QTabWidget::pane {{ border: 1px solid {ACCENT2}; background: {PANEL_BG}; }}
QTabBar::tab {{ background: {ACCENT2}; color: {TEXT}; padding: 8px 18px; border: none; }}
QTabBar::tab:selected {{ background: {ACCENT}; color: #000; font-weight: bold; }}
QPushButton {{ background: {ACCENT2}; color: {ACCENT}; border: 1px solid {ACCENT}; padding: 6px 16px; border-radius: 4px; }}
QPushButton:hover {{ background: {ACCENT}; color: #000; }}
QPushButton:disabled {{ background: #333; color: {MUTED}; border-color: #444; }}
QPushButton#danger {{ background: #3a0010; color: {ERROR_CLR}; border-color: {ERROR_CLR}; }}
QPushButton#danger:hover {{ background: {ERROR_CLR}; color: #fff; }}
QPushButton#ok {{ background: #003a2a; color: {OK_CLR}; border-color: {OK_CLR}; }}
QPushButton#ok:hover {{ background: {OK_CLR}; color: #000; }}
QLineEdit, QSpinBox, QComboBox {{ background: {PANEL_BG}; color: {TEXT}; border: 1px solid {ACCENT2}; padding: 4px 8px; border-radius: 3px; }}
QLineEdit:focus, QSpinBox:focus, QComboBox:focus {{ border-color: {ACCENT}; }}
QTextEdit {{ background: #0d0d1a; color: {ACCENT}; border: 1px solid {ACCENT2}; font-family: monospace; font-size: 12px; }}
QGroupBox {{ border: 1px solid {ACCENT2}; border-radius: 4px; margin-top: 12px; padding-top: 8px; color: {ACCENT}; font-weight: bold; }}
QGroupBox::title {{ subcontrol-origin: margin; left: 10px; }}
QListWidget {{ background: {PANEL_BG}; color: {TEXT}; border: 1px solid {ACCENT2}; }}
QListWidget::item:selected {{ background: {ACCENT2}; color: {ACCENT}; }}
QProgressBar {{ background: {PANEL_BG}; border: 1px solid {ACCENT2}; border-radius: 3px; text-align: center; color: {TEXT}; }}
QProgressBar::chunk {{ background: {ACCENT}; }}
QLabel#heading {{ color: {ACCENT}; font-size: 15px; font-weight: bold; }}
QLabel#muted {{ color: {MUTED}; font-size: 11px; }}
QStatusBar {{ background: {PANEL_BG}; color: {MUTED}; }}
QSplitter::handle {{ background: {ACCENT2}; }}
"""


# ─────────────────────────────────────────────────────────────────────────────
# Serial reader thread
# ─────────────────────────────────────────────────────────────────────────────
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
                    self.error.emit(str(e))
                    break
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


# ─────────────────────────────────────────────────────────────────────────────
# OTA upload thread
# ─────────────────────────────────────────────────────────────────────────────
class OtaThread(QThread):
    progress = pyqtSignal(int)          # 0-100
    status   = pyqtSignal(str)          # status text
    done     = pyqtSignal(bool, str)    # (success, message)

    def __init__(self, device_ip: str, firmware_path: str):
        super().__init__()
        self.device_ip     = device_ip
        self.firmware_path = firmware_path

    def run(self):
        url  = f"http://{self.device_ip}/api/ota"
        path = Path(self.firmware_path)
        size = path.stat().st_size
        self.status.emit(f"Uploading {path.name}  ({size:,} bytes)  →  {url}")

        try:
            with open(path, "rb") as f:
                data = f.read()

            class ProgressReader:
                def __init__(self, data: bytes, cb):
                    self._data  = data
                    self._pos   = 0
                    self._cb    = cb
                    self.len    = len(data)

                def read(self, n=-1):
                    chunk = self._data[self._pos:self._pos + (n if n > 0 else self.len)]
                    self._pos += len(chunk)
                    pct = int(self._pos * 100 / self.len) if self.len else 100
                    self._cb(pct)
                    return chunk

            reader = ProgressReader(data, self.progress.emit)

            resp = requests.post(
                url,
                data=reader,
                headers={"Content-Type": "application/octet-stream",
                         "Content-Length": str(size)},
                timeout=120,
            )
            if resp.status_code == 200:
                self.done.emit(True, "OTA complete — device is rebooting")
            else:
                self.done.emit(False, f"HTTP {resp.status_code}: {resp.text}")
        except Exception as e:
            self.done.emit(False, str(e))


# ─────────────────────────────────────────────────────────────────────────────
# Tab 1 — Serial Monitor
# ─────────────────────────────────────────────────────────────────────────────
class SerialTab(QWidget):
    def __init__(self):
        super().__init__()
        self._thread: SerialThread | None = None
        self._build()

    def _build(self):
        root = QVBoxLayout(self)
        root.setSpacing(8)

        # ── toolbar ──
        bar = QHBoxLayout()
        self.port_combo = QComboBox(); self.port_combo.setMinimumWidth(160)
        self.baud_combo = QComboBox()
        for b in ["9600","19200","38400","57600","115200","230400","460800","921600"]:
            self.baud_combo.addItem(b)
        self.baud_combo.setCurrentText("115200")

        self.refresh_btn = QPushButton("⟳ Ports")
        self.connect_btn = QPushButton("Connect")
        self.connect_btn.setObjectName("ok")
        self.clear_btn   = QPushButton("Clear")

        bar.addWidget(QLabel("Port:"))
        bar.addWidget(self.port_combo)
        bar.addWidget(QLabel("Baud:"))
        bar.addWidget(self.baud_combo)
        bar.addWidget(self.refresh_btn)
        bar.addWidget(self.connect_btn)
        bar.addWidget(self.clear_btn)
        bar.addStretch()
        root.addLayout(bar)

        # ── log ──
        self.log = QTextEdit(); self.log.setReadOnly(True)
        self.log.setFont(QFont("Courier New", 11))
        root.addWidget(self.log, 1)

        # ── send bar ──
        send_bar = QHBoxLayout()
        self.send_edit = QLineEdit(); self.send_edit.setPlaceholderText("Send command…")
        self.send_btn  = QPushButton("Send"); self.send_btn.setObjectName("ok")
        self.nl_check  = QCheckBox("\\n"); self.nl_check.setChecked(True)
        send_bar.addWidget(self.send_edit, 1)
        send_bar.addWidget(self.nl_check)
        send_bar.addWidget(self.send_btn)
        root.addLayout(send_bar)

        # ── signals ──
        self.refresh_btn.clicked.connect(self._refresh_ports)
        self.connect_btn.clicked.connect(self._toggle_connect)
        self.clear_btn.clicked.connect(self.log.clear)
        self.send_btn.clicked.connect(self._send)
        self.send_edit.returnPressed.connect(self._send)

        self._refresh_ports()

    def _refresh_ports(self):
        self.port_combo.clear()
        for p in serial.tools.list_ports.comports():
            self.port_combo.addItem(f"{p.device}  {p.description}", p.device)

    def _toggle_connect(self):
        if self._thread and self._thread.isRunning():
            self._thread.stop()
            self._thread.wait()
            self._thread = None
            self.connect_btn.setText("Connect")
            self.connect_btn.setObjectName("ok")
            self.connect_btn.setStyle(self.connect_btn.style())
            self._append("[disconnected]", MUTED)
        else:
            port = self.port_combo.currentData() or self.port_combo.currentText().split()[0]
            baud = int(self.baud_combo.currentText())
            self._thread = SerialThread(port, baud)
            self._thread.line_received.connect(self._on_line)
            self._thread.error.connect(lambda e: self._append(f"[error] {e}", ERROR_CLR))
            self._thread.start()
            self.connect_btn.setText("Disconnect")
            self.connect_btn.setObjectName("danger")
            self.connect_btn.setStyle(self.connect_btn.style())
            self._append(f"[connected] {port} @ {baud}", OK_CLR)

    def _on_line(self, line: str):
        color = ERROR_CLR if "[error]" in line.lower() or "[fail]" in line.lower() \
               else WARN_CLR  if "[warn]"  in line.lower() \
               else OK_CLR    if "[ok]"    in line.lower() or "[boot]" in line.lower() \
               else ACCENT
        self._append(line, color)

    def _append(self, text: str, color: str = TEXT):
        ts = time.strftime("%H:%M:%S")
        self.log.setTextColor(QColor(MUTED))
        self.log.insertPlainText(f"{ts}  ")
        self.log.setTextColor(QColor(color))
        self.log.insertPlainText(text + "\n")
        self.log.moveCursor(QTextCursor.MoveOperation.End)

    def _send(self):
        if not (self._thread and self._thread.isRunning()): return
        text = self.send_edit.text()
        if not text: return
        data = (text + ("\n" if self.nl_check.isChecked() else "")).encode()
        self._thread.write(data)
        self._append(f">> {text}", WARN_CLR)
        self.send_edit.clear()

    def active_thread(self) -> SerialThread | None:
        return self._thread if (self._thread and self._thread.isRunning()) else None


# ─────────────────────────────────────────────────────────────────────────────
# Tab 2 — Provisioning
# ─────────────────────────────────────────────────────────────────────────────
class ProvisionTab(QWidget):
    log_signal = pyqtSignal(str, str)   # text, color

    def __init__(self):
        super().__init__()
        self.log_signal.connect(self._append)
        self._build()

    def _build(self):
        root = QHBoxLayout(self)
        left = QVBoxLayout()
        root.addLayout(left, 1)

        # ── Target ──
        tgt = QGroupBox("Target Device")
        tg  = QFormLayout(tgt)
        self.ip_edit = QLineEdit("192.168.4.1")
        self.ip_edit.setPlaceholderText("Device IP (default: 192.168.4.1 in AP mode)")
        tg.addRow("Device IP:", self.ip_edit)
        left.addWidget(tgt)

        # ── WiFi ──
        wf = QGroupBox("WiFi")
        wl = QFormLayout(wf)
        self.ssid_edit = QLineEdit()
        self.pass_edit = QLineEdit(); self.pass_edit.setEchoMode(QLineEdit.EchoMode.Password)
        wl.addRow("SSID:", self.ssid_edit)
        wl.addRow("Password:", self.pass_edit)
        left.addWidget(wf)

        # ── MQTT ──
        mq = QGroupBox("MQTT Broker")
        ml = QFormLayout(mq)
        self.mqtt_host  = QLineEdit()
        self.mqtt_port  = QSpinBox(); self.mqtt_port.setRange(1, 65535); self.mqtt_port.setValue(1883)
        self.mqtt_user  = QLineEdit()
        self.mqtt_pass  = QLineEdit(); self.mqtt_pass.setEchoMode(QLineEdit.EchoMode.Password)
        self.mqtt_topic = QLineEdit()
        ml.addRow("Broker host:", self.mqtt_host)
        ml.addRow("Port:",        self.mqtt_port)
        ml.addRow("Username:",    self.mqtt_user)
        ml.addRow("Password:",    self.mqtt_pass)
        ml.addRow("Topic:",       self.mqtt_topic)
        left.addWidget(mq)

        # ── Device ──
        dv = QGroupBox("Device Identity")
        dl = QFormLayout(dv)
        self.dev_code   = QLineEdit()
        self.bin_height = QSpinBox(); self.bin_height.setRange(10, 500); self.bin_height.setValue(60)
        dl.addRow("Device code:", self.dev_code)
        dl.addRow("Container height (cm):", self.bin_height)
        left.addWidget(dv)

        # ── Buttons ──
        btns = QHBoxLayout()
        self.fetch_btn   = QPushButton("⬇ Fetch Current Config")
        self.apply_btn   = QPushButton("✓ Apply Config"); self.apply_btn.setObjectName("ok")
        self.reboot_btn  = QPushButton("⟳ Reboot"); self.reboot_btn.setObjectName("danger")
        self.reset_btn   = QPushButton("✕ Factory Reset"); self.reset_btn.setObjectName("danger")
        btns.addWidget(self.fetch_btn)
        btns.addWidget(self.apply_btn)
        btns.addWidget(self.reboot_btn)
        btns.addWidget(self.reset_btn)
        left.addLayout(btns)
        left.addStretch()

        # ── Log ──
        self.log = QTextEdit(); self.log.setReadOnly(True); self.log.setMaximumHeight(220)
        root.addWidget(self.log, 1)

        # ── Signals ──
        self.fetch_btn.clicked.connect(self._fetch)
        self.apply_btn.clicked.connect(self._apply)
        self.reboot_btn.clicked.connect(self._reboot)
        self.reset_btn.clicked.connect(self._factory_reset)

    def _url(self, path: str) -> str:
        return f"http://{self.ip_edit.text().strip()}{path}"

    def _fetch(self):
        def _do():
            try:
                r = requests.get(self._url("/api/status"), timeout=5)
                d = r.json()
                self.log_signal.emit(f"Status: {json.dumps(d, indent=2)}", ACCENT)
                cfg = d.get("config", {})
                if cfg.get("ssid"):       self.ssid_edit.setText(cfg["ssid"])
                if cfg.get("mqtt_host"):  self.mqtt_host.setText(cfg["mqtt_host"])
                if cfg.get("mqtt_port"):  self.mqtt_port.setValue(int(cfg["mqtt_port"]))
                if cfg.get("mqtt_topic"): self.mqtt_topic.setText(cfg["mqtt_topic"])
                if cfg.get("device_code"):self.dev_code.setText(cfg["device_code"])
            except Exception as e:
                self.log_signal.emit(f"Error: {e}", ERROR_CLR)
        threading.Thread(target=_do, daemon=True).start()

    def _apply(self):
        def _do():
            ip = self.ip_edit.text().strip()
            # 1. WiFi
            try:
                r = requests.post(self._url("/api/wifi"),
                    json={"ssid": self.ssid_edit.text(), "password": self.pass_edit.text()},
                    timeout=5)
                self.log_signal.emit(f"WiFi → {r.status_code}", OK_CLR if r.ok else ERROR_CLR)
            except Exception as e:
                self.log_signal.emit(f"WiFi error: {e}", ERROR_CLR)
            # 2. MQTT
            try:
                r = requests.post(self._url("/api/mqtt"),
                    json={"host": self.mqtt_host.text(), "port": self.mqtt_port.value(),
                          "user": self.mqtt_user.text(), "password": self.mqtt_pass.text(),
                          "topic": self.mqtt_topic.text()},
                    timeout=5)
                self.log_signal.emit(f"MQTT → {r.status_code}", OK_CLR if r.ok else ERROR_CLR)
            except Exception as e:
                self.log_signal.emit(f"MQTT error: {e}", ERROR_CLR)
            # 3. Device
            try:
                r = requests.post(self._url("/api/device"),
                    json={"device_code": self.dev_code.text(),
                          "bin_height_cm": self.bin_height.value()},
                    timeout=5)
                self.log_signal.emit(f"Device → {r.status_code}", OK_CLR if r.ok else ERROR_CLR)
            except Exception as e:
                self.log_signal.emit(f"Device error: {e}", ERROR_CLR)
            self.log_signal.emit("Done — use Reboot to apply changes", WARN_CLR)
        threading.Thread(target=_do, daemon=True).start()

    def _reboot(self):
        def _do():
            try:
                r = requests.post(self._url("/api/reboot"), timeout=5)
                self.log_signal.emit(f"Reboot → {r.status_code}", OK_CLR if r.ok else ERROR_CLR)
            except Exception as e:
                self.log_signal.emit(f"Reboot error: {e}", ERROR_CLR)
        threading.Thread(target=_do, daemon=True).start()

    def _factory_reset(self):
        if QMessageBox.question(self, "Factory Reset",
                "This will erase all config on the device. Are you sure?") != QMessageBox.StandardButton.Yes:
            return
        def _do():
            try:
                r = requests.post(self._url("/api/factory-reset"), timeout=5)
                self.log_signal.emit(f"Factory reset → {r.status_code}", OK_CLR if r.ok else ERROR_CLR)
            except Exception as e:
                self.log_signal.emit(f"Error: {e}", ERROR_CLR)
        threading.Thread(target=_do, daemon=True).start()

    def _append(self, text: str, color: str = TEXT):
        ts = time.strftime("%H:%M:%S")
        self.log.setTextColor(QColor(MUTED))
        self.log.insertPlainText(f"{ts}  ")
        self.log.setTextColor(QColor(color))
        self.log.insertPlainText(text + "\n")
        self.log.moveCursor(QTextCursor.MoveOperation.End)


# ─────────────────────────────────────────────────────────────────────────────
# Tab 3 — OTA Firmware Updater
# ─────────────────────────────────────────────────────────────────────────────
class OtaTab(QWidget):
    def __init__(self):
        super().__init__()
        self._thread: OtaThread | None = None
        self._build()

    def _build(self):
        root = QVBoxLayout(self)
        root.setSpacing(12)

        # ── heading ──
        h = QLabel("OTA Firmware Update")
        h.setObjectName("heading"); root.addWidget(h)
        root.addWidget(QLabel("Upload a compiled .bin firmware to the ESP32 over HTTP.\n"
                              "Device must be on the same WiFi network and powered on."))

        # ── form ──
        form = QGroupBox("Update settings")
        fl   = QFormLayout(form)

        self.ip_edit = QLineEdit()
        self.ip_edit.setPlaceholderText("e.g. 192.168.1.45  (find via serial log or router DHCP)")

        self.fw_edit = QLineEdit()
        self.fw_edit.setReadOnly(True)
        self.fw_edit.setPlaceholderText("Path to .bin firmware file…")

        browse_btn = QPushButton("Browse…")
        browse_btn.clicked.connect(self._browse)

        fw_row = QHBoxLayout()
        fw_row.addWidget(self.fw_edit, 1)
        fw_row.addWidget(browse_btn)
        fw_widget = QWidget(); fw_widget.setLayout(fw_row)

        fl.addRow("Device IP:", self.ip_edit)
        fl.addRow("Firmware:", fw_widget)
        root.addWidget(form)

        # ── hint ──
        hint = QLabel("Tip — compile with PlatformIO:\n"
                      "  pio run -e tc2\n"
                      "Binary will be at:  .pio/build/tc2/firmware.bin")
        hint.setObjectName("muted"); root.addWidget(hint)

        # ── progress ──
        self.progress = QProgressBar(); self.progress.setValue(0)
        root.addWidget(self.progress)

        self.status_label = QLabel("Ready"); self.status_label.setObjectName("muted")
        root.addWidget(self.status_label)

        # ── flash button ──
        self.flash_btn = QPushButton("⬆  Flash Firmware"); self.flash_btn.setObjectName("ok")
        self.flash_btn.setMinimumHeight(40)
        self.flash_btn.clicked.connect(self._flash)
        root.addWidget(self.flash_btn)

        # ── build shortcut ──
        build_box = QGroupBox("Quick build (calls PlatformIO)")
        bl = QHBoxLayout(build_box)
        self.env_edit = QLineEdit("tc2")
        self.build_btn = QPushButton("Build firmware.bin")
        bl.addWidget(QLabel("Env:")); bl.addWidget(self.env_edit); bl.addWidget(self.build_btn)
        root.addWidget(build_box)
        self.build_btn.clicked.connect(self._build_fw)

        self.build_log = QTextEdit(); self.build_log.setReadOnly(True); self.build_log.setMaximumHeight(180)
        root.addWidget(self.build_log)

        root.addStretch()

    def _browse(self):
        path, _ = QFileDialog.getOpenFileName(self, "Select firmware .bin", "", "Binary (*.bin)")
        if path:
            self.fw_edit.setText(path)

    def _flash(self):
        ip   = self.ip_edit.text().strip()
        path = self.fw_edit.text().strip()
        if not ip or not path:
            QMessageBox.warning(self, "Missing info", "Enter device IP and select a .bin file.")
            return
        self.flash_btn.setEnabled(False)
        self.progress.setValue(0)

        self._thread = OtaThread(ip, path)
        self._thread.progress.connect(self.progress.setValue)
        self._thread.status.connect(lambda s: self.status_label.setText(s))
        self._thread.done.connect(self._on_done)
        self._thread.start()

    def _on_done(self, success: bool, msg: str):
        self.flash_btn.setEnabled(True)
        self.status_label.setText(msg)
        if success:
            self.progress.setValue(100)
            QMessageBox.information(self, "OTA Complete", msg)
        else:
            QMessageBox.critical(self, "OTA Failed", msg)

    def _build_fw(self):
        import subprocess
        env = self.env_edit.text().strip() or "tc2"
        project_root = str(Path(__file__).parent.parent)
        self.build_log.clear()
        self.build_log.setTextColor(QColor(ACCENT))
        self.build_log.insertPlainText(f"Running: pio run -e {env}\n")

        def _do():
            proc = subprocess.Popen(
                ["/opt/homebrew/bin/pio", "run", "-e", env],
                cwd=project_root,
                stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                text=True,
            )
            for line in proc.stdout:
                self.build_log.setTextColor(QColor(WARN_CLR if "warning" in line.lower()
                                            else ERROR_CLR if "error" in line.lower()
                                            else ACCENT))
                self.build_log.insertPlainText(line)
                self.build_log.moveCursor(QTextCursor.MoveOperation.End)
            proc.wait()
            if proc.returncode == 0:
                bin_path = Path(project_root) / ".pio" / "build" / env / "firmware.bin"
                if bin_path.exists():
                    self.fw_edit.setText(str(bin_path))
                self.build_log.setTextColor(QColor(OK_CLR))
                self.build_log.insertPlainText("\n✓ Build success\n")
            else:
                self.build_log.setTextColor(QColor(ERROR_CLR))
                self.build_log.insertPlainText(f"\n✗ Build failed (exit {proc.returncode})\n")
        threading.Thread(target=_do, daemon=True).start()


# ─────────────────────────────────────────────────────────────────────────────
# Tab 4 — Sensor Marketplace
# ─────────────────────────────────────────────────────────────────────────────
class MarketplaceTab(QWidget):
    def __init__(self):
        super().__init__()
        self._sensors: list[dict] = []
        self._build()

    def _build(self):
        root = QHBoxLayout(self)

        # ── Left: list ──
        left = QVBoxLayout()
        bar  = QHBoxLayout()
        self.backend_edit = QLineEdit("http://localhost:4003")
        self.fetch_btn    = QPushButton("⬇ Load Catalog")
        self.search_edit  = QLineEdit(); self.search_edit.setPlaceholderText("Search sensors…")
        bar.addWidget(self.backend_edit, 1); bar.addWidget(self.fetch_btn)
        left.addLayout(bar)
        left.addWidget(self.search_edit)
        self.list_widget  = QListWidget()
        left.addWidget(self.list_widget, 1)

        # ── Right: detail ──
        self.detail = QTextEdit(); self.detail.setReadOnly(True)
        self.detail.setFont(QFont("Courier New", 12))

        splitter = QSplitter(Qt.Orientation.Horizontal)
        lw = QWidget(); lw.setLayout(left)
        splitter.addWidget(lw)
        splitter.addWidget(self.detail)
        splitter.setSizes([280, 620])
        root.addWidget(splitter)

        self.fetch_btn.clicked.connect(self._fetch)
        self.list_widget.currentRowChanged.connect(self._show_detail)
        self.search_edit.textChanged.connect(self._filter)

    def _fetch(self):
        url = f"{self.backend_edit.text().rstrip('/')}/api/marketplace"
        def _do():
            try:
                r = requests.get(url, timeout=5)
                data = r.json().get("data", [])
                self._sensors = data
                self.list_widget.clear()
                for s in data:
                    item = QListWidgetItem(f"  {s['name']}")
                    item.setToolTip(s["category"])
                    self.list_widget.addItem(item)
                self.detail.setTextColor(QColor(OK_CLR))
                self.detail.setText(f"✓  Loaded {len(data)} sensors from {url}")
            except Exception as e:
                self.detail.setTextColor(QColor(ERROR_CLR))
                self.detail.setText(f"Error: {e}\n\nMake sure the backend is running:\n  docker compose up -d")
        threading.Thread(target=_do, daemon=True).start()

    def _show_detail(self, row: int):
        if row < 0 or row >= len(self._sensors): return
        s = self._sensors[row]
        lines = [
            f"{'─'*60}",
            f"  {s['name']}",
            f"  id: {s['id']}   category: {s['category']}",
            f"  interface: {s['interface']}",
            f"  {s['manufacturer']}",
            f"{'─'*60}",
            f"\n  {s['description']}\n",
            "  SIGNALS:",
        ]
        for sig in s.get("signals", []):
            rng = f"  [{sig.get('min','?')}–{sig.get('max','?')}]" \
                  if sig.get("min") is not None else ""
            alm = []
            if sig.get("alarmHigh") is not None: alm.append(f"⬆{sig['alarmHigh']}")
            if sig.get("alarmLow")  is not None: alm.append(f"⬇{sig['alarmLow']}")
            lines.append(f"    {sig['name']:<24} {sig['unit']:<8}{rng}  {' '.join(alm)}  — {sig['description']}")

        if s.get("gpioMap"):
            lines += ["", "  GPIO:", "    " + "  ".join(f"{k}→{v}" for k,v in s["gpioMap"].items())]
        lines += ["", f"  MQTT topic pattern:", f"    {s['mqttTopicPattern']}"]
        if s.get("firmwareHint"):
            lines += ["", "  Firmware hint:", *[f"    {l}" for l in s["firmwareHint"].split(". ")]]
        if s.get("routerosHint"):
            lines += ["", "  RouterOS hint:", *[f"    {l}" for l in s["routerosHint"].split(". ")]]
        if s.get("datasheet"):
            lines += ["", f"  Datasheet: {s['datasheet']}"]
        if s.get("tags"):
            lines += ["", f"  tags: {', '.join(s['tags'])}"]

        self.detail.setTextColor(QColor(ACCENT))
        self.detail.setText("\n".join(lines))

    def _filter(self, text: str):
        q = text.lower()
        for i in range(self.list_widget.count()):
            item = self.list_widget.item(i)
            if i < len(self._sensors):
                s = self._sensors[i]
                match = (q in s["id"] or q in s["name"].lower() or
                         q in s["category"].lower() or
                         any(q in t for t in s.get("tags", [])))
                item.setHidden(not match if q else False)


# ─────────────────────────────────────────────────────────────────────────────
# Main Window
# ─────────────────────────────────────────────────────────────────────────────
class MainWindow(QMainWindow):
    def __init__(self):
        super().__init__()
        self.setWindowTitle("routeOS Desktop Companion")
        self.resize(1200, 780)

        tabs = QTabWidget()
        self.serial_tab = SerialTab()
        self.prov_tab   = ProvisionTab()
        self.ota_tab    = OtaTab()
        self.mkt_tab    = MarketplaceTab()

        tabs.addTab(self.serial_tab, "📟  Serial Monitor")
        tabs.addTab(self.prov_tab,   "⚙  Provisioning")
        tabs.addTab(self.ota_tab,    "⬆  OTA Update")
        tabs.addTab(self.mkt_tab,    "🔌  Sensor Catalog")

        self.setCentralWidget(tabs)

        bar = QStatusBar()
        self.status_lbl = QLabel("routeOS Desktop Companion  ·  disconnected")
        bar.addWidget(self.status_lbl)
        self.setStatusBar(bar)


# ─────────────────────────────────────────────────────────────────────────────
def main():
    app = QApplication(sys.argv)
    app.setStyleSheet(STYLESHEET)
    win = MainWindow()
    win.show()
    sys.exit(app.exec())


if __name__ == "__main__":
    main()
