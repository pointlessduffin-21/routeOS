#!/usr/bin/env bash
# flash.sh — Erase full ESP32 flash (wipes NVS) then reflash firmware
# Usage: ./flash.sh [PORT]
#   PORT defaults to /dev/cu.usbserial-562B0100951

set -e

PORT="${1:-/dev/cu.usbserial-562B0100951}"
ESPTOOL="$HOME/.platformio/packages/tool-esptoolpy/esptool.py"
FIRMWARE_DIR="$(cd "$(dirname "$0")" && pwd)"

echo "========================================"
echo "  EcoRoute ESP32 — Full Erase + Flash"
echo "  Port   : $PORT"
echo "========================================"
echo ""

# Step 1: Erase entire flash (wipes NVS, SPIFFS, everything)
echo "[1/3] Erasing flash (this clears NVS)..."
python3 "$ESPTOOL" --chip esp32 --port "$PORT" --baud 460800 erase_flash
echo "      Done."
echo ""

# Step 2: Build latest firmware
echo "[2/3] Building firmware..."
cd "$FIRMWARE_DIR"
pio run -e dock 2>&1 | tail -5
echo ""

# Step 3: Flash
echo "[3/3] Flashing firmware..."
pio run -e dock --target upload --upload-port "$PORT" 2>&1 | tail -10
echo ""
echo "========================================"
echo "  Done! ESP32 will boot with fresh NVS."
echo "  Triple-sensor triangular array:"
echo "    Slot 0: hc_top   — HC-SR04 #1  TRIG=GPIO32 ECHO=GPIO35 (top, white plate)"
echo "    Slot 1: hc_bot   — HC-SR04 #2  TRIG=GPIO22 ECHO=GPIO23 (bottom, white plate)"
echo "    Slot 2: rcwl_side — RCWL-1604  TRIG=GPIO19 ECHO=GPIO21 (side, right mount)"
echo "  Aggregation: avg_3 / outlier_hi / outlier_lo / median_3"
echo "========================================"
