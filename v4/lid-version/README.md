# routeOS v4 — Lid-Sensor Variant

**Firmware version:** `4.0.0-lid`  
**Default device code:** `ECO-BIN-LID-1`  
**Target board:** ESP32-WROOM-32 (30-pin devkit, PCB antenna)

## Concept

Three RCWL-9610A ultrasonic sensors are arranged in a triangle under the bin lid. They fire in sequence (120 ms apart) to prevent echo crosstalk. Fill level is the average of all three readings. An HW-201 IR proximity module detects whether the lid is open or closed — when the lid opens the device suppresses fill reporting (reading is invalid while lid is up).

```
              LID (top view)
         ┌─────────────────────┐
         │   ●  us1(left)      │  GPIO 5 / 18
         │                     │
         │      ●  us2(center) │  GPIO 19 / 21
         │                     │
         │   ●  us3(right)     │  GPIO 22 / 23
         │         ◈ HW-201 IR │  GPIO 4
         └─────────────────────┘
                    │
                 wires run
                down to ESP32
                in bin body
```

---

## Wiring

### Sensors 0–2 — RCWL-9610A (ultrasonic fill level)

All three sensors run at **3.3 V native** — no voltage divider needed.

| Sensor          | Tag          | TRIG GPIO | ECHO GPIO | VCC  |
|-----------------|--------------|-----------|-----------|------|
| RCWL-9610A #1   | `us1(left)`  | **5**     | **18**    | 3V3  |
| RCWL-9610A #2   | `us2(center)`| **19**    | **21**    | 3V3  |
| RCWL-9610A #3   | `us3(right)` | **22**    | **23**    | 3V3  |

Each sensor: `VCC → 3V3`, `GND → GND`, `TRIG → GPIO(n)`, `ECHO → GPIO(n)`.

---

### IR Lid Detector — HW-201

| HW-201 pin | Connects to | Notes                        |
|------------|-------------|------------------------------|
| VCC        | 3V3         |                              |
| GND        | GND         |                              |
| OUT        | GPIO **4**  | LOW = object detected = lid **closed** |

The firmware reads 5 samples in quick succession and uses a majority vote to debounce. You can invert the polarity via the `irInverted` NVS config key if your module wires differently.

**Lid modes** (set via web portal or NVS `lidMode` key):

| Mode | Value | Behaviour |
|------|-------|-----------|
| `IR_ONLY`  | 0 | Trust IR sensor exclusively |
| `US_ONLY`  | 1 | Use ultrasonic distance threshold |
| `BOTH`     | 2 | IR primary; US overrides if they disagree *(default)* |

---

### Status LED & Buttons

| Component          | GPIO | Direction    | Notes                         |
|--------------------|------|--------------|-------------------------------|
| Status LED         | 2    | OUTPUT       | Onboard LED                   |
| Calibration button | 0    | INPUT_PULLUP | BOOT button; hold 3 s to calibrate |

---

## Full pin summary

| GPIO | Function              | Direction | Voltage |
|------|-----------------------|-----------|---------|
| 4    | HW-201 OUT (lid IR)   | IN        | 3.3 V   |
| 5    | RCWL-9610A #1 TRIG    | OUT       | 3.3 V   |
| 18   | RCWL-9610A #1 ECHO    | IN        | 3.3 V   |
| 19   | RCWL-9610A #2 TRIG    | OUT       | 3.3 V   |
| 21   | RCWL-9610A #2 ECHO    | IN        | 3.3 V   |
| 22   | RCWL-9610A #3 TRIG    | OUT       | 3.3 V   |
| 23   | RCWL-9610A #3 ECHO    | IN        | 3.3 V   |
| 2    | Status LED            | OUT       | 3.3 V   |
| 0    | Cal/Boot button       | IN        | INPUT_PULLUP |

---

## Parts list

| Component             | Qty | Notes                                  |
|-----------------------|-----|----------------------------------------|
| ESP32-WROOM-32        | 1   | PCB antenna, fits inside bin body      |
| RCWL-9610A            | 3   | 3.3 V ultrasonic, ~4 m range           |
| HW-201 IR module      | 1   | Active-low output, 3.3 V supply        |
| 5 V power supply      | 1   | USB wall adapter or TP4056+18650       |

No voltage dividers needed — all sensors are 3.3 V native.

---

## Default configuration

| Key              | Default   | Description                             |
|------------------|-----------|-----------------------------------------|
| `binHeight`      | 100 cm    | Distance from sensor to empty bin floor |
| `lidMode`        | 2 (BOTH)  | IR + ultrasonic sensor fusion           |
| `irInverted`     | false     | Invert HW-201 polarity                  |
| `lidOpenThr`     | 50 cm     | US distance above which lid = open      |
| `numSensors`     | 3         | All three RCWL-9610A active             |
| `sensorInterval` | 5000 ms   | Idle telemetry interval                 |

---

## Power

- Nominal: 5 V USB
- Field deployment: solar panel → TP4056 Li-ion charger → 18650 → Vin

---

## Flash

```bash
# Install PlatformIO CLI, then:
cd firmware/v4/lid-version
pio run -e lid -t upload
```

Simulator build (no hardware required):

```bash
pio run -e lid-sim -t upload
```

---

## Telemetry fields (MQTT payload)

```json
{
  "device_code": "ECO-BIN-LID-1",
  "fill_level_percent": 63.0,
  "distance_cm": 37.0,
  "lid_closed": true,
  "sensors": {
    "left":   { "distance_cm": 36.8, "fill_pct": 63.2, "ok": true },
    "center": { "distance_cm": 37.1, "fill_pct": 62.9, "ok": true },
    "right":  { "distance_cm": 37.1, "fill_pct": 62.9, "ok": true }
  },
  "rssi": -55,
  "uptime_sec": 1800,
  "firmware": "4.0.0-lid"
}
```
