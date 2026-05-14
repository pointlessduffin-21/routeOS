#!/usr/bin/env bash
# EcoRoute routeOS USB Serial Bridge
# Usage: ./start-bridge.sh [port] [http-port]
#   port       Serial port (default: auto-detect first CP210x/CH340)
#   http-port  HTTP port   (default: 8765)

set -e
cd "$(dirname "$0")"

# Auto-detect port if not specified
PORT="${1:-}"
if [ -z "$PORT" ]; then
  PORT=$(python3 -c "
import serial.tools.list_ports
ports = list(serial.tools.list_ports.comports())
for p in ports:
    desc = (p.description or '').lower()
    if any(x in desc for x in ['cp210', 'ch340', 'usb serial', 'uart']):
        print(p.device); exit()
if ports:
    print(ports[0].device)
" 2>/dev/null || true)
fi

HTTP_PORT="${2:-8765}"

if [ -z "$PORT" ]; then
  echo "[bridge] No serial port found. Available ports:"
  python3 -m serial.tools.list_ports
  echo ""
  echo "Usage: $0 /dev/cu.usbserial-XXXX [http-port]"
  exit 1
fi

# Check deps
if ! python3 -c "import serial" 2>/dev/null; then
  echo "[bridge] Installing pyserial…"
  pip3 install pyserial
fi

echo "[bridge] Port: $PORT"
echo "[bridge] Open: http://127.0.0.1:$HTTP_PORT"
echo "[bridge] Press Ctrl+C to stop"
echo ""

python3 serial-web.py --port "$PORT" --http "$HTTP_PORT"
