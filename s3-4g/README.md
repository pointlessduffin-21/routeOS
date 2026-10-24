# routeOS S3-4G — buildable, reproducible copy

The S3-4G project **as it actually builds and runs**, documented so a fresh clone reproduces the firmware with no
guessing.

Three inputs are deliberately kept out of git and must exist locally before the board behaves as it did in testing:

| File | Content | Get it from |
|---|---|---|
| `sdkconfig` | the exact tested configuration (3006 lines) | regenerated automatically from `sdkconfig.defaults` — verified byte-identical |
| `sdkconfig.credentials` | `CONFIG_ML_TAILSCALE_AUTH_KEY`, node name | copy `sdkconfig.credentials.example` and fill in |
| `main/wifi_secrets.h` | Wi-Fi SSID and password | copy `main/wifi_secrets.h.example` and fill in |

`sdkconfig.defaults` regenerates `sdkconfig` **byte for byte** (verified: zero differences against the file that ran
on the board). Neither secret is required to build: without them the firmware compiles, boots, runs cellular-only and
logs that it is not joining the tailnet.

Everything else here is ordinary source: `main/`, `components/microlink`, `components/wireguard_lwip`,
`partitions.csv`, `dependencies.lock`, and the diagnostic tools in `tools/`.

## Board

Waveshare **ESP32-S3-A-SIM7670X-4G V2.0**.

- ESP32-S3, 16 MB flash, 2 MB PSRAM (quad, in-package), connected via a **CH343** USB bridge (VID 1A86, PID 55D3).
- **DIP switch 3 ("4G") must be ON**, or GPIO17/18 never reach the modem.
- Modem power is a MOSFET on **GPIO21**, pulsed low for 400 ms at boot.
- Modem UART: **ESP TX = GPIO18 → modem RX**, **ESP RX = GPIO17 ← modem TX**. 115200.
- Modem: SIM7670X / A7670E-FASE. APN `internet.globe.com.ph`.
- Display: 480×800, RVDP over WebSocket on **port 81**, management HTTP on **port 80**.

## Prerequisites

- **ESP-IDF 5.5.2** (5.5.x should work; this was verified on 5.5.2). Other major versions will change the config.
- The `microlink` and `wireguard_lwip` components are in-tree — nothing to install by hand.
- `managed_components/` (LVGL) is **not** included: the component manager fetches it from `dependencies.lock` on the
  first build. Needs network access once.

### Windows (PowerShell)

```powershell
$env:PATH = [Environment]::GetEnvironmentVariable('PATH','Machine') + ';' + [Environment]::GetEnvironmentVariable('PATH','User')
. C:\esp\esp-idf\export.ps1
```

### macOS / Linux

```sh
. ~/esp/esp-idf/export.sh
```

## Build and flash

Find the port first — on Windows it is the CH343, not the ESP32-S3 native USB:

```powershell
python -m serial.tools.list_ports -v      # look for VID:PID=1A86:55D3
```

```sh
idf.py -C s3-4g build
idf.py -C s3-4g -p <PORT> -b 460800 flash monitor
```

Expected build output (do not accept anything wildly different):

```
routeos_s3_4g.bin binary size 0x... bytes. Smallest app partition is 0x... bytes.
Project build complete.
```

Note the partition table is `partitions.csv`, offset `0x8000`, 16 MB flash. If the size check complains that the app
does not fit, the flash size option was lost — `CONFIG_ESPTOOLPY_FLASHSIZE_16MB=y` must be present in `sdkconfig`.

## What a good boot looks like

On the console, in order:

```
I (xxx) routeos: boot: ...
I (xxx) ml_net_switch: trying Wi-Fi first (6 s)
W (xxx) routeos: no Wi-Fi ... (or: Wi-Fi connected, <ip>)
I (xxx) routeos: modem power on (GPIO21)
I (xxx) ml_cellular: AT+DIAG... / A7670E-FASE, IMEI ...
I (xxx) ml_cellular: PPP CONNECT via 'ATD*99***1#' (pass 1)
I (xxx) ml_cellular: PPP got IP: 100.x.x.x
I (xxx) routeos: tailscale up, node routeos-4g
I (xxx) rvdp: display: rvdp on ws://:81/rvdp, 480x800
```

The ICMP/PPP IP is carrier-assigned and changes; the **tailnet** address is the stable one.

Verify from a machine on the tailnet:

```powershell
curl http://100.70.23.100/health
```

It must answer with JSON containing `"width":480,"height":800,...`. If `/health` answers but `/rvdp` does not, the
problem is the WebSocket handler, not the network.

## Verified before this copy was made

- `sdkconfig.defaults` + `sdkconfig.credentials` regenerate `sdkconfig` with **zero** differences from the file that
  ran on the board.
- `idf.py reconfigure` and a full build complete on the Nitro 5 with this exact tree.

Not verified on this copy: a flash of the physical board (it was not connected when the copy was made). The binary
that ran last was built from commit `010ab33` plus this configuration.

## If something goes wrong

| Symptom | Cause | Fix |
|---|---|---|
| `fatal error: wifi_secrets.h: No such file or directory` | old tree, before the `__has_include` guard | use this copy, or add the guard |
| esptool cannot sync, board drops off the bus at LTE attach | USB power/EMI on this board | powered hub, shorter cable, retry; the CH343 sometimes needs a replug |
| UART shows `ROK` instead of `OK` | one corrupted leading byte at 115200 while the modem powers up | retry the AT command; the driver already tolerates it |
| `+CME ERROR: operation not allowed` on dial | bearer already taken | `AT+DIALMODE=0` before dialling (the driver does this) |
| Tailscale never comes up | auth key missing | `sdkconfig.credentials` must exist and hold `CONFIG_ML_TAILSCALE_AUTH_KEY` |
| Inbound connections to the node fail | peer table full | `CONFIG_ML_MAX_PEERS=16` and `CONFIG_ML_PRIORITY_PEER_IP` are both in `sdkconfig.defaults`; keep them |
| App does not fit | flash size option lost | `CONFIG_ESPTOOLPY_FLASHSIZE_16MB=y` |

## Do not

- Do not regenerate `sdkconfig` with `idf.py set-target` unless you intend to re-derive it; it will drop the
  credentials fragment.
- Do not "clean up" `sdkconfig.defaults`: every non-default value there is load-bearing. Changing
  `CONFIG_ML_*` (peers, buffers, pins, APN) changes behaviour that was measured.
- Do not commit `sdkconfig.credentials` or `main/wifi_secrets.h`.
