# routeOS S3-4G — from a bare machine to a working board

This is the whole procedure, starting at a computer with nothing installed. Follow it in order. Every value here was
measured on hardware; nothing is a guess. If a step's output does not match what is written, stop and read
[Troubleshooting](#troubleshooting) instead of changing source code.

Target: Waveshare **ESP32-S3-A-SIM7670X-4G V2.0** running routeOS over Wi-Fi or LTE, with Tailscale and RVDP
(the remote display) up.

---

## 1. What you need

**Hardware**

| Item | Detail |
|---|---|
| Board | Waveshare ESP32-S3-A-SIM7670X-4G V2.0 (ESP32-S3, 16 MB flash, 2 MB quad PSRAM) |
| Cable | USB-C **data** cable to the board's USB port (a charge-only cable looks exactly like a dead board) |
| SIM | LTE data SIM, APN set in `sdkconfig.defaults` (`internet.globe.com.ph` by default) |
| Antennas | LTE and Wi-Fi/Bluetooth pigtails fitted — the modem will not register without the LTE one |
| Switch | **DIP switch 3 ("4G") must be ON.** With it off, GPIO17/18 never reach the modem |

**Software**

| Item | Detail |
|---|---|
| ESP-IDF | **v5.5.2** (pinned; another version will regenerate the configuration differently) |
| Python | 3.9+ for the IDF tools, plus `pyserial` for the helper scripts in `tools/` |
| Git | to clone ESP-IDF |
| Network | once, to fetch the LVGL component (~170 MB unpacked). See [Offline builds](#7-offline-builds) |

---

## 2. Install ESP-IDF v5.5.2 (skip if `idf.py --version` already says v5.5.2)

### Windows (PowerShell)

```powershell
# 1. Install Git for Windows and Python 3.11+ first (winget):
winget install --id Git.Git -e
winget install --id Python.Python.3.11 -e

# 2. Clone the exact IDF version, with submodules (the first clone takes a while):
cd C:\esp
git clone -b v5.5.2 --recursive https://github.com/espressif/esp-idf.git esp-idf

# 3. Install the toolchain for the ESP32-S3 only (smaller and faster than "all"):
cd C:\esp\esp-idf
.\install.ps1 esp32s3
```

Every new shell needs the environment before any `idf.py` command:

```powershell
$env:PATH = [Environment]::GetEnvironmentVariable('PATH','Machine') + ';' + [Environment]::GetEnvironmentVariable('PATH','User')
. C:\esp\esp-idf\export.ps1
idf.py --version        # must print: ESP-IDF v5.5.2
```

### macOS / Linux

```sh
# Debian/Ubuntu prerequisites (macOS: brew install cmake ninja dfu-util python3)
sudo apt-get install -y git wget flex bison gperf python3 python3-pip python3-venv \
  cmake ninja-build ccache libffi-dev libssl-dev dfu-util libusb-1.0-0

mkdir -p ~/esp && cd ~/esp
git clone -b v5.5.2 --recursive https://github.com/espressif/esp-idf.git esp-idf
cd esp-idf
./install.sh esp32s3
. ./export.sh
idf.py --version        # must print: ESP-IDF v5.5.2
```

On Linux, also add yourself to the serial group once, then log out and back in:

```sh
sudo usermod -aG dialout "$USER"
```

---

## 3. Unpack this package

```powershell
# Windows
Expand-Archive -Path "$env:USERPROFILE\Downloads\routeos-s3-4g-2026-10-25-final.zip" -DestinationPath C:\work -Force
cd C:\work\routeos-s3-4g
```

```sh
# macOS / Linux
mkdir -p ~/work && cd ~/work
unzip ~/Downloads/routeos-s3-4g-2026-10-25-final.zip
cd routeos-s3-4g
```

The layout is:

```
routeos-s3-4g/
  README.md                 <- this file
  AGENTS.md                 <- rules for anyone (or any agent) touching this project
  s3-4g/                    <- the project
    sdkconfig               <- THE configuration that was tested; do not regenerate it
    sdkconfig.defaults      <- complete defaults; regenerates sdkconfig byte for byte
    sdkconfig.credentials   <- Tailscale auth key + node name        (secret)
    main/wifi_secrets.h     <- Wi-Fi SSID + password                 (secret)
    main/, components/, partitions.csv, dependencies.lock, tools/
```

The two secret files are **included in this package** and are **not** in the git repository. If you copy the project
from GitHub instead, create them from the `.example` files — the build works without them (cellular only, no
Tailscale) but the board will behave differently from the one that was tested.

---

## 4. Build

```powershell
# Windows: environment first, every new shell
$env:PATH = [Environment]::GetEnvironmentVariable('PATH','Machine') + ';' + [Environment]::GetEnvironmentVariable('PATH','User')
. C:\esp\esp-idf\export.ps1

cd C:\work\routeos-s3-4g
idf.py -C s3-4g build
```

```sh
# macOS / Linux
. ~/esp/esp-idf/export.sh
cd ~/work/routeos-s3-4g
idf.py -C s3-4g build
```

Expected, near the end:

```
routeos_s3_4g.bin binary size 0x... bytes. Smallest app partition is 0x... bytes.
Project build complete.
```

The first build also fetches LVGL into `s3-4g/managed_components` (see [Offline builds](#7-offline-builds)). A build
that fails on `lvgl.h` is a component-fetch failure, not a source problem.

**Do not run `idf.py set-target`, `idf.py menuconfig` or delete `sdkconfig` to "fix" a build.** The defaults in this
package regenerate the tested configuration exactly; anything else is a different firmware.

---

## 5. Flash

Find the port. The board talks through a **CH343 USB bridge** (`VID:PID 1A86:55D3`) — not the ESP32-S3 native USB.

```powershell
# Windows: the CH343 shows as COMx
python -m serial.tools.list_ports -v
```

```sh
# Linux
ls -l /dev/ttyUSB* /dev/ttyACM*
# macOS
ls /dev/cu.*
```

Then:

```powershell
idf.py -C s3-4g -p COM7 -b 460800 flash monitor
```

```sh
idf.py -C s3-4g -p /dev/ttyUSB0 -b 460800 flash monitor
```

Exit the monitor with `Ctrl+]`.

On Windows the CH343 needs the WCH driver (usually installed by Windows Update; otherwise "USB-SERIAL CH343" appears
with a warning triangle in Device Manager).

---

## 6. What a good boot looks like

In this order, on the console:

```
I (xxx) routeos: boot: <name> <version> (core ...), reset: power on, free memory NNN KB
I (xxx) ml_net_switch: Wi-Fi first, 6000 ms budget
I (xxx) routeos: Wi-Fi connected, 192.168.NNN.NNN          (or: no Wi-Fi, falling back to cellular)
I (xxx) routeos: modem power on (GPIO21)
I (xxx) ml_cellular: modem A7670E-FASE, IMEI 86xxxxxxxxxxxxx
I (xxx) ml_cellular: PPP CONNECT via 'ATD*99***1#' (pass 1)
I (xxx) ml_cellular: PPP got IP: 100.NN.NN.NN
I (xxx) routeos: tailscale up, node routeos-4g
I (xxx) rvdp: display: rvdp on ws://:81/rvdp, 480x800
```

Verify, from another machine on the same network or on the tailnet:

```powershell
curl http://<board-ip>/health
```

Must answer JSON containing `"width":480,"height":800` and a `"page"` field. Then check the display transport:

```powershell
python tools\screen_shot.py <board-ip> out.png   # writes one frame; prints "frame end after N rectangles"
```

If `/health` answers but `/rvdp` never sends a frame, the problem is in the WebSocket handler, not the network.
If neither answers but the board pings, the HTTP server is wedged — see Troubleshooting.

---

## 7. Offline builds

`dependencies.lock` pins LVGL by hash, but the component itself is not in the git repository. Two ways to build
without internet:

1. **Recommended:** on any machine that has built once, copy `s3-4g/managed_components/` into the same place in the
   offline copy. The component manager then sees the dependency satisfied and skips the download.
2. Keep a copy of the whole built project (including `managed_components/`) next to the sources. `build/` itself can
   always be deleted.

---

## 8. Reproducibility guarantees

- `sdkconfig.defaults` regenerates `sdkconfig` **byte for byte**. Verified on 2026-10-25: deleting `sdkconfig` and
  running `idf.py reconfigure` produced a file with **zero** differences from the one that ran on the board.
- `sdkconfig.credentials` is optional: `CMakeLists.txt` appends it only if it exists, and the firmware logs
  "no Tailscale auth key" and runs cellular-only without it.
- `main/wifi_secrets.h` is optional: `main.cpp` includes it through `__has_include`.
- The exact source state that was tested is tagged `known-good-2026-10-25` in the repository.

---

## 9. Troubleshooting

| Symptom | Cause | Fix |
|---|---|---|
| `idf.py: command not found` | environment not exported in this shell | run `export.ps1` / `export.sh` again (every new shell) |
| `ESP-IDF v5.x` not `v5.5.2` | wrong checkout | `cd esp-idf; git checkout v5.5.2; git submodule update --init --recursive` |
| `fatal error: lvgl.h: No such file` | component not fetched | allow network for one build, or copy `managed_components/` from a built copy |
| `fatal error: wifi_secrets.h: No such file` | very old tree, before the `__has_include` guard | use this package |
| `Could not open port ... Permission denied` (Linux) | missing group | `sudo usermod -aG dialout $USER`, then log out and in |
| Port never appears | charge-only cable, or missing CH343 driver | different cable; install WCH CH343 driver |
| esptool cannot sync / board disappears at LTE attach | USB power dip when the modem transmits | powered USB hub, shorter cable, replug and retry |
| UART shows `ROK` instead of `OK` | one corrupted byte at 115200 while the modem powers up | retry; the driver tolerates it |
| `+CME ERROR: operation not allowed` when dialling | bearer already held by auto-dial | `AT+DIALMODE=0` before dialling (driver does this) |
| PPP connects, no internet | wrong APN | set `CONFIG_ML_CELLULAR_APN` in `sdkconfig.defaults` |
| Tailscale never comes up | no auth key | put `CONFIG_ML_TAILSCALE_AUTH_KEY` in `sdkconfig.credentials` |
| Inbound connections to the node fail | WireGuard peer table full | keep `CONFIG_ML_MAX_PEERS=16` and `CONFIG_ML_PRIORITY_PEER_IP` |
| App does not fit in the partition | flash size option lost | `CONFIG_ESPTOOLPY_FLASHSIZE_16MB=y` must be in `sdkconfig` |
| A board that pings but whose HTTP server never answers | see the note below | power-cycle; do not reflash "to fix it" blindly |

Notes that come from measurement:

- **5 GHz Wi-Fi is invisible to this board.** The S3 radio scans 2.4 GHz only. If a scan shows nothing, check the band
  before blaming the firmware.
- **A wedged HTTP server is not a network failure.** If `ping` answers but `/health` resets the connection, the board
  is alive and its web server is stuck; a power cycle restores it. The P4 firmware now reboots itself in that state;
  the S3-4G does not yet.
- **Do not open the serial port with DTR/RTS asserted.** On this board that resets it, which looks exactly like a
  crash. The helper scripts in `tools/` set both lines false before opening.

---

## 10. Do not

- Do not commit `sdkconfig.credentials` or `main/wifi_secrets.h`.
- Do not regenerate `sdkconfig` with `set-target` unless you intend to re-derive it from `sdkconfig.defaults`.
- Do not "clean up" `sdkconfig.defaults`. Every non-default value there is load-bearing: peer table size, PSRAM mode,
  APN, pins, PPP options, WebSocket support.
- Do not rewrite `rvdp_server.cpp` from scratch. Its comments record hardware failures that cost hours; the invariants
  are listed in `AGENTS.md`.
- Do not claim success without the outputs: the binary size line for a build, `/health` JSON for a boot, and a real
  frame for the display.
