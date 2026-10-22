# routeOS packages

routeOS devices are managed from [Termz](https://github.com/pointlessduffin-21/cubeterm).
Termz asks a board what it is, judges a catalog of packages against that answer,
and installs the ones that fit. This page is the reference for the package
format, the device API behind it, and what is and is not checked.

**Status.** The package format, the Termz catalog and install flow, and the
Core firmware compile and are tested against a *simulated* device
(`cubeterm/tests/fake_routeos.py`). The Core firmware has **not yet been run on a
physical board**, so every catalog entry says "Not yet verified on hardware"
until someone does that and records the board in its `status.tested_on`.

## Three kinds of package

| Kind | What it is | Installs |
| --- | --- | --- |
| `driver` | A sensor described as data: one of four sampling primitives plus a scale. No code. | At run time, over Wi-Fi. No reflash. |
| `plugin` | A bundle of drivers installed (and removed) together. | At run time. |
| `module` | A feature compiled into a firmware image (RVDP, Tailscale). | By firmware image only. |

A module cannot be hot-loaded: it is native code, and loading code a network
peer sends would let anyone who reaches the device run anything on it. Termz
therefore *reports* whether a board could take a module (`needs-image`) and
refuses to pretend it can deliver one. Building and delivering images is future
work; today a module is chosen when the firmware is built.

## What a board reports

`GET /api/capabilities` is public and read-only. It carries nothing secret:

```json
{ "schema": 1,
  "device":   { "chip": "ESP32-D0WD-V3", "family": "esp32", "revision": 3, "cores": 2,
                "cpu_mhz": 240, "flash_mb": 4, "psram_kb": 0, "sdk": "v4.4.7", "id": "c4977c" },
  "memory":   { "heap_total_kb": 250, "heap_free_kb": 108, "heap_min_kb": 80, "largest_block_kb": 34 },
  "partitions": { "app_kb": 1280, "ota": true },
  "firmware": { "name": "routeos-orbit-hmi", "version": "1.0.0" },
  "modules":  [ { "id": "core", "version": "1.0.0" }, { "id": "rvdp", "version": "1.1" } ],
  "drivers":  { "supported": true, "installed": 0, "max": 8, "max_spec_bytes": 380 },
  "pins":     { "allowed": [4, 5, 13, ...], "input_only": [34, 35, 36, 39], "adc": [32, 33, 34, 35, 36, 39],
                "i2c": { "sda": 21, "scl": 22 } },
  "manage":   { "enabled": true, "token_set": true },
  "uptime_s": 120, "reset_reason": "power-on" }
```

Termz re-validates every field and keeps only the ones it understands, with the
types it expects, so a confused or hostile device cannot inject markup or odd
values into the UI.

## Compatibility verdicts

Termz compares a manifest's `requires` with the report and gives each package one
status. The reasons are written for the technician (for example "Needs 2 MB of
PSRAM; this board has none.").

| Status | Meaning |
| --- | --- |
| `installed` | The module is in this firmware, or the driver (every driver of a plugin) is on the device. |
| `available` | It fits and can be installed now. |
| `needs-image` | A module that fits this board but is not in its firmware. |
| `blocked` | It cannot work here: wrong chip family, too little flash or PSRAM, a required module missing, a conflicting module present, too little free heap, or the driver limit reached. |
| `unknown` | The board did not report its hardware (older firmware, or unreachable). |

Heap checks keep **40 KB** free for Wi-Fi, TLS and the web server, so a package is
blocked when its `cost.heap_kb` plus that reserve exceeds what is free. The
cost figures in the catalog are estimates unless the notes say they were measured.

## Package manifest (schema 1)

A manifest is a JSON object of at most 6,000 bytes. Unknown fields are rejected.

```json
{
  "schema": 1,
  "id": "hcsr04-ultrasonic",
  "kind": "driver",
  "name": "HC-SR04 ultrasonic distance",
  "version": "1.0.0",
  "category": "Distance",
  "description": "Fill level or proximity.",
  "tags": ["ultrasonic", "distance"],
  "requires": { "chips": ["esp32"], "min_flash_mb": 4, "modules": ["core>=1.0"] },
  "cost": { "heap_kb": 1 },
  "install": { "method": "runtime" },
  "driver": {
    "type": "pulse_echo",
    "pins": { "trig": 5, "echo": 18 },
    "interval_ms": 10000, "samples": 5,
    "signal": { "name": "distance_cm", "unit": "cm", "scale": 0.01715, "offset": 0 }
  },
  "status": { "tested_on": [], "notes": "Not yet verified on hardware." },
  "datasheet": "https://example.com/datasheet.pdf"
}
```

| Field | Rules |
| --- | --- |
| `id` | 2 to 32 characters: `a-z`, `0-9`, `-`. Unique in the catalog; your own package cannot reuse a built-in id. |
| `kind` | `driver`, `plugin` or `module` (a module cannot be added by a user). |
| `version` | `1`, `1.0` or `1.0.0`, optional `-suffix`. |
| `requires.chips` | Families from `esp32`, `esp32s2`, `esp32s3`, `esp32c3`, `esp32c6`. |
| `requires.min_flash_mb`, `min_psram_kb`, `min_free_heap_kb` | Whole numbers. |
| `requires.modules` | Ids, optionally `core>=1.0`. A driver normally needs `core`. |
| `requires.conflicts` | Ids that must not be present. |
| `cost` | `heap_kb`, `psram_kb`. |
| `includes` | A plugin only: 1 to 8 distinct driver ids. |
| `config.fields` | A module's settings, at most 8: `string`, `secret` or `integer`. Describes fields only; it holds no values. |
| `status` | `tested_on` (at most 5 boards) and `notes`. Be honest here. |

## Driver types

A driver is validated by Termz when it is saved and **again by the device** when
it is installed; the firmware trusts nothing it is sent.

| `type` | Reads | Pins | Notes |
| --- | --- | --- | --- |
| `adc` | An analog voltage | `pin`: an ADC1 pin (32, 33, 34, 35, 36, 39) | Averages `samples` readings (1 to 16). ADC2 pins are not allowed: Wi-Fi uses them. |
| `digital` | A logic level | `pin` | Majority of `samples` reads (default 3); reads 1 or 0. `pull`: `none`, `up`, `down` (not on GPIO 34 to 39). `invert`: true for active-low parts. |
| `pulse_echo` | Echo time of a ranger such as the HC-SR04 | `trig` (output-capable), `echo` | `samples` 1 to 7, median taken. A missing echo is reported as a bad reading, never as zero. |
| `i2c_reg` | A register on the I2C bus | none: the bus is fixed on GPIO 21 (SDA) and 22 (SCL) | `address` 3 to 119, `register` 0 to 255, `bytes` 1 to 4, `endian`, `signed`, `shift` 0 to 24. |

Every driver has `interval_ms` (1,000 to 3,600,000, default 10,000) and a
`signal` that turns the raw value into a reading: `name` (`a-z0-9_`), `unit`
(8 printable ASCII characters at most), and `value = raw * scale + offset`.

Limits on the device: **8 drivers**, **380 bytes** of JSON each, and no two
drivers may claim the same GPIO (two I2C drivers share the bus). Drivers are saved
to flash (NVS) and reloaded at boot; that path has been compiled but not yet
exercised on a board. The classic ESP32 pin map is defined;
other chips report `drivers.supported: false` until theirs is added.

When a technician installs a catalog driver, Termz lets them change only the
pins, `interval_ms`, `samples`, `pull`, `invert` and I2C `address`. Everything else
(type, scale, signal) is the package's.

## Device API (routeOS Core)

| Route | Auth | Purpose |
| --- | --- | --- |
| `GET /api/capabilities` | none | The report above. |
| `GET /api/drivers` | Bearer token | Installed driver specs. |
| `GET /api/drivers/readings` | Bearer token | Latest value, quality (`good`, `bad`, `pending`), age and error count per driver. |
| `PUT /api/drivers/{id}` | Bearer token | Install or replace a driver. The body's `id` must match the path. |
| `DELETE /api/drivers/{id}` | Bearer token | Remove a driver. |

The management routes exist only in firmware built with `-DROUTEOS_MANAGE=1`
(the PlatformIO `virtual-hmi-control` environment, or `--control` with the Arduino
exporter). A view-only build answers `manage.enabled: false` and refuses them.

## Security model

- **The token.** On first network start the device creates a random management
  token, keeps it in flash (NVS), and prints it on the serial console. It is
  compared in constant time. There is no default token and no way to read it over
  the network: whoever can see the serial console, which means physical access,
  can get it.
- **Saving it in Termz.** Paste it into the host's *Management token* field. Termz
  stores it encrypted like any other host credential and never sends it back to the
  browser.
- **No code arrives over the network.** A driver is data interpreted by fixed
  firmware. The worst a bad driver can do is read pins the allowlist permits.
- **Transport.** The device speaks plain HTTP. The token and drivers cross the
  network in clear text, so reach the device only over a trusted link: Tailscale,
  a VPN, or a LAN you control. Do not expose its port to the internet.
- **Termz never signs you out because of a device.** A device rejecting a token is
  reported as an error on that host, not as a Termz session failure.

## Adding your own package

In Termz, open a routeOS host, choose **Modules**, then **Add your own package**
and paste a `driver` or `plugin` manifest. It is validated, kept in *your*
catalog only (other users never see it), and installed exactly like a built-in
one. A plugin may include drivers from the built-in catalog or your own.

To contribute a package to the shared catalog, add its JSON to
`marketplace/packages/` in this repository (and the copy in Termz's
`cubeterm/catalog/`), and set `status.tested_on` only after running it on that
board.

## Tailscale, honestly

The Tailscale module is catalogued with `requires.min_psram_kb: 2048`. The
upstream component, [Csontikka/esphome-tailscale](https://github.com/Csontikka/esphome-tailscale)
(built on microlink), states a classic ESP32 without PSRAM is not expected to
work, and no routeOS image with it has been built yet. Termz shows it as
**Not compatible** on a board like the 4 MB lab ESP32-D0WD-V3 and as **Needs a
firmware image** on a board that has the PSRAM. For a board without PSRAM, reach
it through a Tailscale subnet router on the same network instead.
