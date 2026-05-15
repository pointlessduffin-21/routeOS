#!/usr/bin/env bash
# Build routeOS Desktop Companion as a macOS .app
# Usage: ./build.sh
# Output: dist/routeOS.app

set -e
cd "$(dirname "$0")"

echo "==> Installing Python dependencies..."
pip3 install -r requirements.txt --break-system-packages -q

echo "==> Installing PyInstaller..."
pip3 install pyinstaller --break-system-packages -q

echo "==> Building routeOS.app..."
pyinstaller routeOS.spec \
  --distpath dist \
  --workpath build \
  --noconfirm \
  --clean

APP="dist/routeOS.app"
if [ -d "$APP" ]; then
    SIZE=$(du -sh "$APP" | cut -f1)
    echo ""
    echo "✓  Built: $APP  ($SIZE)"
    echo ""
    echo "To open:  open $APP"
    echo "To copy to /Applications:  cp -r $APP /Applications/"
else
    echo "✗  Build failed — check output above"
    exit 1
fi
