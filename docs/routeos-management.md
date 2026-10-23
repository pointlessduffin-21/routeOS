# routeOS management (Core 1.1)

routeOS Core is the part of the firmware that [Termz](https://github.com/pointlessduffin-21/cubeterm)
talks to. Beyond the capability report and drivers (see [routeos-packages.md](routeos-packages.md)), Core 1.1
gives a board an **event log**, **Wi-Fi management that undoes itself**, and **over-the-air firmware updates
with a safety net**. This page is the reference for those, and says plainly what is protected and what is not.

All routes below need the management token (`Authorization: Bearer <token>`, printed on the serial console)
and exist only in firmware built with `-DROUTEOS_MANAGE=1`. The transport is plain HTTP, so use them over
Tailscale, a VPN, or a LAN you control; never expose the port to the internet.

## Routes

| Route | Purpose |
| --- | --- |
| `GET /api/logs?since=N` | The last 48 events, newest after `N`. |
| `GET /api/wifi` | Connection, signal, saved network, why it last dropped, state of a change in progress. |
| `POST /api/wifi/scan`, `GET /api/wifi/scan` | Start a scan; read it (`running: true` until done). |
| `PUT /api/wifi` | `{"ssid","password"}`: try another network. |
| `GET /api/ota` | Running firmware, the other slot, update progress, the boot guard, the last event. |
| `POST /api/ota/begin` | `{"size": bytes}`: start receiving an image into the idle slot. |
| `POST /api/ota/chunk` | One piece of the image, `multipart/form-data`, with `X-Ota-Offset: <bytes already held>`. |
| `POST /api/ota/finish` | Verify the whole image and make it the next boot. |
| `POST /api/ota/abort` | Forget a half-received image. |
| `POST /api/ota/rollback` | Boot the other slot instead. |
| `GET /api/capabilities` | Public. Its `features` list (`system`, `logs`, `wifi`, `ota`) says which of the above a board offers. |

## Event log

`routeos::log('I' | 'W' | 'E', "text %d", ...)` writes to a 48-line ring buffer in RAM (92 characters a line) and to
the serial console. It is gone after a restart. `GET /api/logs` returns `{"boot_id","uptime_s","next","dropped",
"lines":[{"n","t","l","m"}]}`: ask for lines after the last `n` you saw. `boot_id` is random per start, so a reader
that sees it change knows the numbering began again. Core logs boots, Wi-Fi events, driver changes, updates and
rollbacks; the firmware adds its own (the display viewer connecting). **Never log a secret**: tokens and
passwords are kept out of it, and the tests check that.

## Wi-Fi

* **Where the network comes from.** A network saved through the API is tried first, then the one compiled into the
  firmware (`wifi_secrets.h`). If the one in use has been down for 30 s and the other exists, the device tries the
  other. A saved password lives in the device's flash (NVS), unencrypted, like any ESP32 Wi-Fi setting.
* **A change cannot strand the device.** `PUT /api/wifi` answers at once, then the device leaves its network and
  tries the new one for 20 s. It is **saved only if it joins**. If it does not, the previous network is restored
  (it waits up to 25 s for it) and `GET /api/wifi` reports `result.state: "reverted"` with a reason in words
  (`4-way handshake timeout (wrong password?)`, `no access point with that name was found`, ...). Changes are
  refused while another change, a scan or a firmware update is running.
* **It may get a new address.** If the new network gives the board another IP, the console can no longer reach it
  at the old one. Termz says so, with the board's MAC address so it can be found in a router's device list.
* **Joins the strongest access point** of a network, not the first one it hears.
* **What it cannot do:** 5 GHz networks (the ESP32 has no 5 GHz radio) and WPA2-Enterprise (needs a username).
  The scan lists enterprise networks but marks them unsupported.

## Firmware updates

### What an image must be

An ordinary ESP32 application image (`firmware.bin` from PlatformIO or Arduino) that also carries the routeOS
marker. Put this once in the firmware's main file, with string literals:

```cpp
ROUTEOS_IMAGE("routeos-orbit-hmi", "1.1.1");
```

It stamps `ROUTEOS-IMAGE|1|<name>|<version>|core=<core version>|ota=1|END` into the image. Linking fails without it,
on purpose. The marker tells Termz and the device that the file is routeOS firmware whose Core has the update guard
below. `routeos::begin()` holds a reference to it so the linker cannot discard it. The sample firmware reads its
name and version from `FW_NAME` and `FW_VERSION`, so a build can override them: `-DFW_VERSION=\"1.2.0\"`.

### The protocol

Termz sends the image in pieces so that a weak link costs a piece, not the whole transfer:

1. `POST /api/ota/begin {"size": N}`: the device checks the size against its idle slot and free memory, and
   answers `chunk_max` (16 KB).
2. `POST /api/ota/chunk` for each piece, `multipart/form-data` with the bytes in a file part and the offset in
   `X-Ota-Offset`. The device writes what arrives as it arrives. A piece at the wrong offset is refused with a
   409 that says how many bytes it holds, so a sender always knows where to resume. A piece cut off half way keeps
   the whole 1436-byte slices that arrived; ask `GET /api/ota` for `update.received` and carry on from there.
3. `POST /api/ota/finish`: the device checks that everything arrived, that the image carries the marker and says
   `ota=1`, and then asks ESP-IDF to activate it, which verifies the image's own checksum and appended SHA-256
   before it switches the boot slot. Only then does it reply and restart.

The running firmware is **never touched** before step 3 succeeds. The first 16 bytes of an image are written last,
so a half-written slot is not bootable. A failed or abandoned update leaves the device exactly as it was, except
that the idle slot (the previous firmware) is gone: `other.verified` reports whether it holds a good image.

### The safety net

A newly installed image is *on trial*. It must reach the network and **stay up for 30 s** to be confirmed.

| What happens | Result |
| --- | --- |
| It crashes or restarts three times in a row | On the fourth start the previous firmware is booted again. |
| It starts but does not reach the network within two minutes (a hang) | The previous firmware is booted again. |
| It stays up 30 s on the network | Confirmed. `last.kind` becomes `confirmed`. |
| You ask to go back (`POST /api/ota/rollback`) | The other slot boots. The image switched to is on trial too, so switching to a broken image undoes itself. |

`GET /api/ota` reports it all: `guard.pending`, `guard.starts`, and `last.kind` (`installed`, `confirmed`,
`rolled-back`, `switched`, `failed`) with a sentence in `last.text`.

**What this does not cover.** The guard is code in the *new* image's Core. If a new image crashes before
`routeos::begin()` runs (in a static initialiser, or while the chip starts up), nothing can roll it back and the
board needs a USB reflash. Nor can it help if both slots are damaged, or if the bootloader is. USB flashing
always works. The sample firmware's image uses 88% of its 1.25 MB slot; a larger image needs a different
partition scheme, which also needs a USB flash.

### Why pieces are multipart

The first draft read raw request bodies. The Arduino web server's raw reader asks the socket for a whole 1436-byte
buffer even when fewer bytes remain, and busy-waits for the rest; a body of an awkward length therefore froze the
network task until the watchdog rebooted the board, and it did so before the token was checked. Multipart is the
path Arduino's own OTA example uses, and the chunk route is registered so that anything *not* multipart takes the
ordinary, safe path.

## A web server one odd client cannot stop

Two weaknesses in the Arduino `WebServer` library were found on the lab board and are fixed by `routeos::Server`,
a drop-in replacement the firmware uses instead of `WebServer`:

* After serving a request the library leaves a **5 s** read timeout on its client object (its assignment operator
  does not reset it), and its readers busy-wait for that long. 5 s is the task watchdog's limit, so **eight bytes
  of a request line from any machine on the network rebooted the board**, with no token. `routeos::Server` resets
  the timeout to 1 s before each request; the worst wait for an odd request is now about a second.
* Its multipart reader waits for ever on a client that stops sending. A watchdog closes the connection of any
  single request stuck for 30 s, and a quicker one (8 s without a byte) cuts a stalled firmware piece.

What remains: the server is single-threaded, so a client that keeps sending odd requests can still keep it busy
(for about a second each, or 30 s for a stalled one). It does not restart the device.

## Memory, and a board that restarts rather than sits dead (Core 1.1.1)

On the lab board, after about four hours, the display stopped answering. Nothing had crashed: the CPU ran and the serial
console printed, but the board did not even answer ping. Its heap showed 63 KB free with the **largest free block 2 KB**.
Wi-Fi and TCP need blocks of about 1.6 KB and more, so the network stack was starved and never recovered. The same state
was then reproduced on purpose (a test build that shreds the heap, then a display viewer connects): with Core 1.1.0 the
network died for good, every time.

What explains it, measured on that board:

* Only **8-bit DRAM** can be used for network buffers, and `ESP.getFreeHeap()` counts about 50 KB more (IRAM) that it
  cannot. Real headroom was 25 KB. `/health`'s `heap` is the larger figure; `largest_block` is the one that matters.
* A display viewer opening sends a 65 KB frame in a burst, and on a weak link that briefly took 11 to 17 KB of the 25 KB.
  On a heap already fragmented by hours of uptime, nothing was left.
* Core 1.1.0 itself had made it worse: four extra tasks created after Wi-Fi started (each a stack and a control block, and a
  long-lived block in the middle of the heap), and a 32 KB LVGL pool that the UI barely uses.

What 1.1.1 changes:

| Change | Effect |
| --- | --- |
| One housekeeping task (`ros-watch`) replaces the web-request, upload and Wi-Fi tasks, and starts before the network | fewer, earlier allocations |
| The driver sampler task starts with the first installed driver | nothing held on a board with no drivers |
| LVGL pool 32 KB to 24 KB (the UI leaves about 8 KB of it free) | 8 KB back |
| **Self-check**: if the largest block stays under 3 KB (or 8-bit free under 6 KB) for 10 s, the board logs it, saves the reason and restarts | a starved network now costs about 15 s, not a power cycle |
| The display viewer gets a packet only when its socket has room (`select`), and is dropped after 15 s of nothing getting through | a slow link no longer stops HTTP while a display is open |
| Wi-Fi modem sleep off | ping median 118 ms with 10% loss became 11 ms with 1 loss in 40 (two runs minutes apart, so the link also varied) |

The restart reason is shown after the board comes back, in the log: `the last run was restarted by the self-check:
largest block 2676 B, 21 KB free, up 0 min`. A new image that does this on every start is rolled back by the update guard
like any other that keeps restarting. The serial status line now also prints `free8` (8-bit free) and `rssi`.

Not solved by any of this: the lab board often sits at about -80 dBm (it sometimes reports -90 and drops its association),
and the display's 65 KB first frame still takes a second or more over such a link. A better antenna position or access
point is the real fix for that.

## What was tested, on the lab board (ESP32-D0WD-V3, 4 MB, no PSRAM)

| Test | Result |
| --- | --- |
| Wi-Fi: five invalid requests, malformed JSON, no token; a scan; a change with the **wrong password** (the board came back by itself in 25 s, same address, nothing saved, reason reported); a change with the right one (saved, password never returned or logged) | all passed |
| Firmware: defences against bad input, a cut piece that resumed from exactly what had been written, a socket left silent (the board recovered by itself in 9 s with no restart), a damaged image, an image without the marker and one that cannot guard its start (each refused by the device itself) | 36 of 36 passed |
| A real update 1.1.0 to 1.1.1-test (17 s to send at 66 KB/s over a weak link, confirmed after 30 s), then going back | passed |
| An image that **crashes on every start** | three starts, then the previous firmware returned by itself within 37 s |
| An image that **hangs** and never reaches the network | the previous firmware returned at the two-minute mark (145 s after the push) |
| Half a request line, socket held open | before: the board rebooted. After: worst wait 1.05 s, no restart |
| A stalled multipart request to an ordinary route | cut at 31 s, no restart, logged |
| Heap shredded to a 2 KB largest block, then a display viewer connects (Core 1.1.0) | the network died and never came back, every time |
| The same test on the Core 1.1.1 work-in-progress build (merged tasks, self-check) | warned, restarted itself after 10 s, logged why, back on the network |
| Ten display sessions in a row on Core 1.1.0, six on the 1.1.1 work-in-progress build, 2,258 `/health` requests in five minutes | heap flat, no false self-check restart |

The 1.1.1 results above are from work-in-progress builds. The final 1.1.1 image (24 KB LVGL pool, test routes removed)
has been compiled but **not yet flashed or exercised**.

Not tested: a power cut in the middle of an update (the design leaves the running slot untouched, but nobody has
pulled the plug), installing an image built by the Arduino IDE (Termz's image check accepts it, but it was not
run on the board), 320x240 displays, a long soak, and any real sensor.

## Building and checking

`pio run -e virtual-hmi-control` builds the sample firmware with management on. `tools/export_arduino.py hmi
--control` exports the same firmware for the Arduino IDE (Core is copied under `src/routeos_core/`). The Termz
repository's `tests/test_firmware.py`, `tests/test_routeos_firmware.py` and `tests/browser_modules_check.py` test
the other end against a simulated board, including every failure above.
