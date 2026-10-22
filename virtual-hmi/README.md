# routeOS Virtual HMI / RVDP lab prototype

The ESP32 runs a **real EEZ Studio-exported LVGL 8.4 UI** without a physical
display. A browser or iShareScreen reconstructs its RGB565 tile stream. Existing
production `src/` and root PlatformIO environments are untouched.

## Hardware screenshots

![Browser connected to the physical ESP32 and displaying its EEZ-generated LVGL UI](../docs/screenshots/hmi-browser-hardware.png)

*Real ESP32 output in the browser: Orbit calculator with a 20-button keypad, history and scratchpad.*

![iShareScreen Liquid Glass RVDP viewer receiving pixels from the physical ESP32](../docs/screenshots/hmi-native-hardware.png)

*Real device output in the native viewer; Qt offscreen window capture. These are
hardware screenshots, not the host simulator's test pattern.*

## EEZ editing and regeneration

Open `eez/routeos-hmi.eez-project` in **EEZ Studio 0.29.0**. This is a native LVGL
project, Flow disabled, LVGL **8.4.0**, 240×135. Use Build (F7). It exports into
`src/eez_generated/`. The checked-in C/headers are actual EEZ build output, not
hand-written substitutes. Keep the generated object identifiers used by `src/ui_bindings.cpp`, or adapt
those bindings when changing the project.

Calculator evaluation, history, navigation and keyboard bindings live outside
generated files. Firmware calls `ui_init()` after display/input registration and `ui_tick()`
from its single UI owner loop. The display transport has no EEZ-specific logic.
Flow support can be evaluated later; it adds a runtime and must pass a new heap
budget. LVGL 9 exports require a port of the display/input adapter.

For command-line regeneration:

```powershell
& 'path/to/EEZ Studio.exe' --build-project "$PWD/virtual-hmi/eez/routeos-hmi.eez-project"
```

EEZ build must report no errors. Do not edit generated files to fix application
behavior. The project originated from the official EEZ LVGL template at
https://github.com/eez-open/eez-project-templates/blob/master/templates/v0.23.0/LVGL-8.3.eez-project
and explicitly targets 8.4.0. EEZ sources and portable release files used to
generate it are kept only in ignored `build/`.

## Firmware build and network

From the repository root (install PlatformIO in a virtual environment first):

```powershell
.venv/Scripts/python.exe -m platformio run --project-dir virtual-hmi -e virtual-hmi
```

`virtual-hmi` is view-only. For an isolated interactive lab demo use
`-e virtual-hmi-control`; viewers must independently request pointer and keyboard permission.
No authentication/authorization suitable for customer deployment is included.

Create ignored `virtual-hmi/include/wifi_secrets.h` locally:

```c
#define HMI_WIFI_SSID "your-isolated-development-network"
#define HMI_WIFI_PASSWORD "your-network-password"
```

This file and compiled firmware contain credentials; never commit or distribute
them. Without the file the board starts a temporary WPA2 AP with a random password
printed only on serial. It does not read, clear, or modify production NVS settings.

Before flashing, save the original firmware (the backup contains sensitive NVS):

```powershell
.venv/Scripts/python.exe virtual-hmi/backup_flash.py --port COM4
.venv/Scripts/python.exe -m platformio run --project-dir virtual-hmi -e virtual-hmi-control -t upload --upload-port COM4
.venv/Scripts/python.exe -m platformio device monitor --port COM4 --baud 115200
```

The backup tool is intentionally for this verified **4-MiB** original ESP32. It
checks chunk integrity, has ROM fallback, and never writes flash. Keep the final
binary and its SHA256 together. To restore after disconnecting every serial
monitor, use esptool `write-flash 0 build/hmi-backup/original-COM4-20261022.bin`.
Do not erase flash or change eFuses. Restore only to the same unencrypted lab board.

## View

Serial prints the acquired IP. Browser: `http://DEVICE_IP/hmi`. iShareScreen source:

```powershell
cd ../iShareScreen
.venv/Scripts/python.exe -m isharescreen.frontend.desktop.standard_app --host ws://DEVICE_IP:81/rvdp --style glass
```

Choose routeOS (RVDP), optionally request pointer + keyboard control, and Connect. Initial
connection and reconnect require a complete screen. The same native window
provides fit, zoom, refresh and reconnect; unsupported clipboard and Mac-specific actions are disabled. Saved entries contain only the endpoint.
Fresh verified native binaries are in `iShareScreen/dist/orbit/`. Older running
copies elsewhere in `dist/` may predate RVDP; do not replace an executable while
it is in use. Rebuild to a separate folder with `packaging/build.ps1 -DistDir dist/orbit`.

One viewer at a time. Close the browser before opening a native viewer. Pressed
input releases on disconnect or after two seconds without a pointer event.

## Orbit controls

- Calculator: 20 touch buttons, live result, precedence, parentheses, percent,
  powers (`^`), scientific notation and error handling. Enter or `=` saves a result.
- History: three recent calculations; tap a row to reuse its result; Clear empties it.
- Notes: a 128-character scratchpad with LVGL's on-screen QWERTY keyboard.
- Click the viewer screen to focus physical keyboard input. Enter calculates on
  Calculator and inserts a newline in Notes. Tab/PageDown cycles pages, PageUp
  goes back, Escape returns to Calculator (or clears it). Backspace, Delete,
  Left/Right and Home/End edit text. Printable ASCII is supported.
- Expressions are limited to 64 characters. History and notes are held in RAM
  and reset on reboot. RVDP 1.1 enables keyboard input; 1.0 viewers retain pointer
  support. One viewer can connect at a time.

![Calculator history rendered on the ESP32](../docs/screenshots/orbit-history-browser.png)

![Scratchpad and LVGL keyboard rendered on the ESP32](../docs/screenshots/orbit-notes-browser.png)

## Simulator and tests

Install `virtual-hmi/requirements-host.txt` (and Playwright for browser tests), or
use iShareScreen's existing Python environment for the simulator:

```powershell
../iShareScreen/.venv/Scripts/python.exe virtual-hmi/simulator.py --control
# Browser: http://127.0.0.1:8095/hmi
# Native endpoint: ws://127.0.0.1:8095/rvdp
node --test virtual-hmi/tests/wire.test.cjs
.venv/Scripts/python.exe virtual-hmi/tests/browser_smoke.py
```

The deterministic simulator is a protocol test pattern, not LVGL/EEZ emulation.
Native protocol and real-window tests live in iShareScreen `tests/test_rvdp.py`.
Golden vectors and the exact wire contract are under `protocol/`. Firmware has no
full framebuffer: LVGL writes a 7,680-byte strip, copies to a fixed ten-slot queue,
then reuses the strip. A full nine-strip snapshot plus its end marker must fit.

`/health` and serial report heap, minimum heap, largest allocation, PSRAM, queue
depth/drops, batches, bytes, UI execution time, and stack watermarks. Soak tests,
320×240, and authenticated TLS are separate measurement gates. See
`docs/virtual-hmi.md` for actual results; a build passing is not hardware proof.
