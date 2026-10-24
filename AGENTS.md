# Working on routeOS — read this before you touch firmware

Two ESP-IDF projects live here: `s3-4g/` (Waveshare ESP32-S3-A-SIM7670X-4G, cellular + Tailscale + RVDP) and
`p4/hmi/` (Guition JC8012P4A1, 800×1280 panel, RVDP). Both have a known-good configuration. Reproduce it; do not
rediscover it.

## Hard rules

1. **Never fabricate credentials, configuration values, pins or APNs.** Every value that matters is already committed
   in `sdkconfig.defaults`, or belongs in a gitignored secrets file with an `.example` beside it.
   Missing credentials are *expected*: both firmwares build and boot without them.
2. **Never regenerate `sdkconfig` from scratch** (`idf.py set-target`, deleting it without defaults, `menuconfig`
   round-trips). Both projects reproduce their tested `sdkconfig` from the committed `sdkconfig.defaults` byte for
   byte. If you must delete `sdkconfig`, the next `idf.py reconfigure` rebuilds the same one.
3. **Never flash a board that is not the one you were asked to flash.** Check the port first:
   - P4: `USB VID:PID=303A:1001`, USB-Serial-JTAG, console on the same port. **Opening this port can reset the board.**
   - S3-4G: **CH343 `VID:PID=1A86:55D3`**, not the native USB. Opening it with DTR/RTS asserted resets the board.
4. **Do not rewrite a working file because it looks odd.** Every non-default value in `sdkconfig.defaults` and every
   comment in `p4/hmi/main/rvdp_server.cpp` records a failure that was measured on hardware. Read the comment.
5. **Do not claim success without the output.** "It builds" means the binary size line; "it works" means `/health`
   answering and a frame arriving. Paste the actual output.

## Build and flash

```powershell
# ESP-IDF 5.5.2, Windows
$env:PATH = [Environment]::GetEnvironmentVariable('PATH','Machine') + ';' + [Environment]::GetEnvironmentVariable('PATH','User')
. C:\esp\esp-idf\export.ps1

# S3-4G (cellular board)
idf.py -C s3-4g build
idf.py -C s3-4g -p COM<n> -b 460800 flash monitor

# P4 (800x1280 panel)
idf.py -C p4/hmi build
idf.py -C p4/hmi -p COM<n> -b 460800 flash monitor
```

Full details, expected boot logs and a troubleshooting table: `s3-4g/README.md`.

## Secrets (gitignored, by design)

| Project | File | Content | Example |
|---|---|---|---|
| s3-4g | `sdkconfig.credentials` | `CONFIG_ML_TAILSCALE_AUTH_KEY`, node name | `sdkconfig.credentials.example` |
| s3-4g | `main/wifi_secrets.h` | `HMI_WIFI_SSID`, `HMI_WIFI_PASSWORD` | `main/wifi_secrets.h.example` |
| p4 | `main/wifi_secrets.h` | `HMI_WIFI_SSID`, `HMI_WIFI_PASSWORD` | `main/wifi_secrets.h.example` |

Without them the builds still succeed: the S3 runs cellular-only without Tailscale, the P4 runs without Wi-Fi.

## RVDP transport invariants (do not undo)

In `p4/hmi/main/rvdp_server.cpp` and `s3-4g/main/rvdp_server.cpp`:

- **A full TCP socket is backpressure, not a dead viewer.** Frames are skipped and coalesced; the session stays up.
  Dropping the viewer because it cannot be written to is what made the P4 unreachable after a few hours.
- **The receive handler must never block on the send mutex** (`sendSmallTry`). Blocking it stops the whole HTTP
  server, which is indistinguishable from "the device died".
- **One viewer owns the display.** Ownership is released the instant its socket closes.
- Memory is two fixed buffers: the frame shadow (PSRAM, allocated once) and one packet buffer. Nothing is allocated
  per frame. If you add a per-frame allocation, you have broken the design.
