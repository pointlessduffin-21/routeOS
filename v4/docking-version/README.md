# routeOS v4 — Docking-Station Variant

**Firmware version:** `4.0.0`  
**Default device code:** `ECO-BIN-DOCK-1`  
**Target board:** ESP32-WROOM-32U (30-pin devkit, U.FL external antenna)

## Concept

The ESP32 and sensors are mounted to a fixed PVC dock frame. A removable inner-liner bin slides into the dock. Sensor 0 measures fill level from above; sensor 1 (RCWL-1604 millimeter-wave radar) detects whether the collection vehicle has pulled up alongside the station and auto-posts a `service_event` to the backend.

```
          ┌──────────────────────────────────┐
          │         PVC DOCK FRAME           │
          │  ┌────────────┐  ┌────────────┐  │
          │  │  HC-SR04   │  │ RCWL-1604  │  │
          │  │ (fill, top)│  │ (dock, fwd)│  │
          │  └─────┬──────┘  └─────┬──────┘  │
          │        │               │          │
          │  ┌─────▼───────────────▼──────┐   │
          │  │      ESP32-WROOM-32U       │   │
          │  └────────────────────────────┘   │
          │                                   │
          │  ╔═══════════════════════════╗    │
          │  ║  inner liner (removable)  ║    │
          │  ╚═══════════════════════════╝    │
          └──────────────────────────────────┘
```

---

## Wiring

### Sensor 0 — HC-SR04 (fill level, mounted above bin opening)

> ⚠️ **HC-SR04 ECHO outputs 5 V logic. A voltage divider is REQUIRED to protect the ESP32 (3.3 V tolerant).**

| HC-SR04 pin | Connects to         | Notes                              |
|-------------|---------------------|------------------------------------|
| VCC         | Vin (5 V)           | Use the 5 V USB rail, not 3V3      |
| GND         | GND                 |                                    |
| TRIG        | GPIO **32**         | Direct 3.3 V output — OK as-is     |
| ECHO        | GPIO **35** ← divider | See resistor network below       |

**Voltage divider for ECHO line:**

```
HC-SR04 ECHO (5V) ──── 1kΩ ──── GPIO 35 ──── 2kΩ ──── GND
```

Use 1 kΩ and 2 kΩ resistors (or 2.2 kΩ if 2 kΩ is unavailable — close enough).

---

### Sensor 1 — RCWL-1604 (dock / vehicle proximity)

3.3 V native logic — no level shifting needed.

| RCWL-1604 pin | Connects to   |
|---------------|---------------|
| VCC           | 3V3           |
| GND           | GND           |
| TRIG          | GPIO **19**   |
| ECHO          | GPIO **21**   |

---

### Status LED & Buttons

| Component         | GPIO | Direction   | Notes                         |
|-------------------|------|-------------|-------------------------------|
| Status LED        | 2    | OUTPUT      | Onboard blue LED on most devkits |
| Calibration button| 0    | INPUT_PULLUP | BOOT button; hold 3 s to enter calibration |

---

## Full pin summary

| GPIO | Function          | Direction | Voltage |
|------|-------------------|-----------|---------|
| 32   | HC-SR04 TRIG      | OUT       | 3.3 V   |
| 35   | HC-SR04 ECHO      | IN        | 3.3 V ← via divider |
| 19   | RCWL-1604 TRIG    | OUT       | 3.3 V   |
| 21   | RCWL-1604 ECHO    | IN        | 3.3 V   |
| 2    | Status LED        | OUT       | 3.3 V   |
| 0    | Cal/Boot button   | IN        | INPUT_PULLUP |

---

## Parts list

| Component             | Qty | Notes                             |
|-----------------------|-----|-----------------------------------|
| ESP32-WROOM-32U       | 1   | U.FL variant for external antenna |
| HC-SR04               | 1   | 5 V, ~4 m range                   |
| RCWL-1604             | 1   | 3.3 V mmWave radar, ~1.5 m range  |
| 1 kΩ resistor         | 1   | ECHO voltage divider (top leg)    |
| 2 kΩ resistor         | 1   | ECHO voltage divider (bottom leg) |
| 5 V power supply      | 1   | USB wall adapter or TP4056+18650  |
| U.FL external antenna | 1   | Better range inside bin housing   |

---

## Power

- Nominal: 5 V USB
- Field deployment: solar panel → TP4056 Li-ion charger → 18650 → Vin

---

## Flash

```bash
# Install PlatformIO CLI, then:
cd firmware/v4/docking-version
pio run -e dock -t upload
```

Simulator build (no hardware required):

```bash
pio run -e dock-sim -t upload
```

---

## Telemetry fields (MQTT payload)

```json
{
  "device_code": "ECO-BIN-DOCK-1",
  "fill_level_percent": 42.0,
  "distance_cm": 20.3,
  "bin_present": true,
  "sensors": {
    "primary":   { "distance_cm": 20.3, "fill_pct": 42.0, "ok": true },
    "secondary": { "distance_cm": 18.7, "fill_pct": 46.6, "ok": true }
  },
  "rssi": -58,
  "uptime_sec": 3600,
  "firmware": "4.0.0"
}
```
