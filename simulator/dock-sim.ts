/*
 * EcoRoute Docking Station Simulator (dock-sim.ts)
 *
 * Runs the full docking-version web portal + MQTT publisher
 * entirely on your Mac — no ESP32 needed.
 *
 * Matches the docking-version firmware (v4 dual-fill) exactly:
 *   - 2 sensor slots: left (HC-SR04), right (RCWL-1604)
 *   - Geometry: emptyDistanceCm=40, fullDistanceCm=15 (25cm usable range)
 *   - MQTT payload mirrors firmware: sensors keyed object {left, right}
 *     + aggregation_method, sensor_disagreement, primary/secondary legacy fields
 *   - Simulator patterns: rising, falling, sawtooth, random, hold
 *   - Full web portal: dashboard, config, simulator, MQTT, calibrate
 *
 * Usage:
 *   bun firmware/simulator/dock-sim.ts
 *   bun firmware/simulator/dock-sim.ts --port 8090 --device ECO-DOCK-002
 *   bun firmware/simulator/dock-sim.ts --interval 5
 */

import http from "http";
import mqtt from "mqtt";

const args = process.argv.slice(2);
function getArg(name: string, fallback: string): string {
  const idx = args.indexOf(`--${name}`);
  return idx >= 0 && args[idx + 1] ? args[idx + 1]! : fallback;
}

const PORT            = parseInt(getArg("port", "8091"));
const DEVICE_CODE     = getArg("device", "ECO-DOCK-001");
const BROKER_URL      = process.env.MQTT_BROKER_URL ?? "mqtt://109.123.238.215:1883";
const PUBLISH_INTERVAL_MS = parseInt(getArg("interval", "1")) * 1000;
const FIRMWARE_VERSION    = "4.2.1-sim";
const TOPIC_PREFIX        = "ecoroute/trash_can/";

// ─── Sensor slot types ────────────────────────────────────────────────────────
const SENSOR_TYPE_US  = 0;

type SimPattern = "rising" | "falling" | "sawtooth" | "random" | "hold";

interface SensorSlot {
  tag: string;
  type: number;           // SENSOR_TYPE_US
  trigPin: number;
  echoPin: number;
  fillPercent: number;    // 0–100
  pattern: SimPattern;
  step: number;           // internal counter for sawtooth
}

// ─── Device state (mirrors NVS + runtime globals) ─────────────────────────────
const slots: SensorSlot[] = [
  { tag: "left_fill",   type: SENSOR_TYPE_US,  trigPin:  5, echoPin: 18, fillPercent: 30, pattern: "rising",   step: 0 },
  { tag: "right_fill",  type: SENSOR_TYPE_US,  trigPin: 19, echoPin: 21, fillPercent: 28, pattern: "rising",   step: 0 },
];

const cfg = {
  deviceCode:       DEVICE_CODE,
  wifiSsid:         "SimulatedWiFi",
  emptyDistanceCm:  40.0,   // firmware: binHeightCm=40 (empty bin reading)
  fullDistanceCm:   15.0,   // firmware: cfgFullDistanceCm=15 (full bin reading)
  numSensors:       2,
  mqttBroker:       "109.123.238.215",
  mqttPort:         1883,
  mqttUser:         "ecoroute",
  mqttPass:         "secret",
  mqttTopic:        TOPIC_PREFIX,
  portalUser:       "admin",
  portalPass:       "ecoroute",
};

const runtime = {
  binPresent:      true,
  mqttConnected:   false,
  publishCount:    0,
  lastPublishMs:   0,
  rssi:            -62,
  batteryVoltage:  4.8,
  bootTime:        Date.now(),
};

// ─── Simulation tick ──────────────────────────────────────────────────────────
function tickSlot(s: SensorSlot) {
  switch (s.pattern) {
    case "rising":
      s.fillPercent += 1.2 + Math.random() * 0.8;
      if (s.fillPercent >= 95) s.fillPercent = 5 + Math.random() * 5;
      break;
    case "falling":
      s.fillPercent -= 1.2 + Math.random() * 0.8;
      if (s.fillPercent <= 3) s.fillPercent = 90 + Math.random() * 5;
      break;
    case "sawtooth":
      s.step = (s.step + 1) % 100;
      s.fillPercent = s.step;
      break;
    case "random":
      s.fillPercent += (Math.random() - 0.5) * 8;
      break;
    case "hold":
      break;
  }
  s.fillPercent = Math.max(0, Math.min(100, s.fillPercent));
}

function computeDistanceCm(fill: number): number {
  // Mirrors firmware formula: dist = fullDist + (emptyDist - fullDist) * (1 - fill/100)
  const range = cfg.emptyDistanceCm - cfg.fullDistanceCm; // 25 cm
  return Math.round((cfg.fullDistanceCm + range * (1 - fill / 100)) * 10) / 10;
}

function simulateTick() {
  for (const s of slots.slice(0, cfg.numSensors)) tickSlot(s);
  runtime.rssi = Math.max(-90, Math.min(-40, runtime.rssi + Math.floor(Math.random() * 5) - 2));
  runtime.batteryVoltage = Math.max(3.0, runtime.batteryVoltage - Math.random() * 0.001);
  // randomly toggle bin presence occasionally
  if (Math.random() < 0.03) runtime.binPresent = !runtime.binPresent;
}
setInterval(simulateTick, 5000);

// ─── MQTT publish ──────────────────────────────────────────────────────────────
let mqttClient: mqtt.MqttClient | null = null;

async function connectMqtt() {
  try {
    mqttClient = await mqtt.connectAsync(BROKER_URL, {
      clientId: `ecoroute-dock-sim-${Date.now()}`,
      username: cfg.mqttUser,
      password: cfg.mqttPass,
      clean: true,
    });
    runtime.mqttConnected = true;
    console.log(`[mqtt] Connected to ${BROKER_URL}`);

    mqttClient.on("error", (err) => {
      console.error("[mqtt] Error:", err.message);
      runtime.mqttConnected = false;
    });
    mqttClient.on("close", () => {
      runtime.mqttConnected = false;
    });
  } catch (err: any) {
    console.warn(`[mqtt] Could not connect (${err.message}) — portal still runs without MQTT`);
    runtime.mqttConnected = false;
  }
}

function buildPayload() {
  const activeSensors = slots.slice(0, cfg.numSensors);
  const validSlots = activeSensors.filter(s => s.fillPercent >= 0);
  const avgFill = validSlots.reduce((a, s) => a + s.fillPercent, 0) / (validSlots.length || 1);

  // Dual-fill aggregation — mirrors firmware logic
  const ROLE_KEYS   = ["left", "right"] as const;
  const ROLE_MODELS = ["HC-SR04", "RCWL-1604"] as const;
  const fills  = activeSensors.map(s => s.fillPercent);
  const dists  = activeSensors.map(s => computeDistanceCm(s.fillPercent));
  const fillDiff = activeSensors.length >= 2 ? Math.abs(fills[0]! - fills[1]!) : 0;
  const aggregation_method  = activeSensors.length < 2 ? "single_sensor"
    : fillDiff <= 20 ? "average" : "max_disagreement";
  const sensor_disagreement = aggregation_method === "max_disagreement";

  // Sensors keyed object — matches firmware MQTT shape exactly
  const sensors: Record<string, object> = {};
  for (let i = 0; i < activeSensors.length && i < 2; i++) {
    const s = activeSensors[i]!;
    sensors[ROLE_KEYS[i]!] = {
      tag:         s.tag,
      model:       ROLE_MODELS[i]!,
      distance_cm: dists[i]!,
      fill_percent: Math.round(s.fillPercent * 10) / 10,
      valid:       true,
      trig_gpio:   s.trigPin,
      echo_gpio:   s.echoPin,
    };
  }

  return {
    device_code:        cfg.deviceCode,
    firmware_version:   FIRMWARE_VERSION,
    simulated:          true,
    sim_pattern:        slots[0]!.pattern,
    bin_present:        runtime.binPresent,
    fill_level_percent: Math.round(avgFill * 10) / 10,
    distance_cm:        computeDistanceCm(avgFill),
    battery_voltage:    Math.round(runtime.batteryVoltage * 100) / 100,
    signal_strength:    runtime.rssi,
    uptime_sec:         Math.floor((Date.now() - runtime.bootTime) / 1000),
    publish_count:      runtime.publishCount + 1,
    anomaly_flag:       false,
    // Legacy flat fields (backward compat with v1 backend)
    primary_fill_percent:   Math.round((fills[0] ?? -1) * 10) / 10,
    primary_distance_cm:    dists[0] ?? -1,
    secondary_fill_percent: Math.round((fills[1] ?? -1) * 10) / 10,
    secondary_distance_cm:  dists[1] ?? -1,
    // Dual-fill aggregation metadata
    aggregation_method,
    sensor_disagreement,
    // Per-sensor keyed object (matches firmware exactly)
    sensors,
  };
}

async function publishNow() {
  simulateTick();
  const payload = buildPayload();
  runtime.publishCount++;
  runtime.lastPublishMs = Date.now();

  if (mqttClient?.connected) {
    const topic = `${cfg.mqttTopic}${cfg.deviceCode}`;
    await mqttClient.publishAsync(topic, JSON.stringify(payload), { qos: 1 });
    console.log(`[mqtt] → ${topic}  fill=${payload.fill_level_percent}%  bin=${payload.bin_present ? "DOCKED" : "ABSENT"}  simulated=true`);
  } else {
    console.log(`[sim]  (no MQTT)  fill=${payload.fill_level_percent}%  bin=${payload.bin_present ? "DOCKED" : "ABSENT"}`);
  }
}

if (PUBLISH_INTERVAL_MS > 0) {
  setInterval(publishNow, PUBLISH_INTERVAL_MS);
}

// ─── HTML helpers (dark theme matching docking firmware) ──────────────────────
const CSS = `
:root{
  --bg:#0d1117;--surface:#161b22;--surface-2:#21262d;--surface-3:#30363d;
  --border:#30363d;--text:#e6edf3;--text-2:#8b949e;--text-3:#656d76;
  --primary:#2da44e;--primary-dark:#238636;--primary-text:#fff;
  --info:#388bfd;--warn:#d29922;--danger:#f85149;
  --ok:#2da44e;--r-sm:6px;--r-md:8px;--r-lg:12px;
}
*{box-sizing:border-box;margin:0;padding:0}
body{font-family:system-ui,-apple-system,sans-serif;background:var(--bg);color:var(--text);font-size:14px;line-height:1.5;min-height:100vh}
.nav{background:var(--surface);border-bottom:1px solid var(--border);padding:10px 16px;display:flex;align-items:center;gap:4px;flex-wrap:wrap}
.nav .brand{font-weight:700;color:var(--primary);font-size:16px;margin-right:auto;white-space:nowrap}
.nav a{color:var(--text-2);text-decoration:none;padding:6px 10px;border-radius:var(--r-sm);font-size:13px;transition:.15s}
.nav a:hover,.nav a.active{color:var(--text);background:var(--surface-2)}
.container{max-width:860px;margin:0 auto;padding:16px}
.card{background:var(--surface);border:1px solid var(--border);border-radius:var(--r-lg);padding:16px;margin-bottom:16px}
.card h2{font-size:15px;font-weight:600;margin-bottom:12px;color:var(--text)}
.grid{display:grid;grid-template-columns:repeat(auto-fit,minmax(150px,1fr));gap:10px;margin-bottom:14px}
.stat{background:var(--surface-2);border:1px solid var(--border);border-radius:var(--r-md);padding:12px}
.stat-label{font-size:11px;font-weight:600;text-transform:uppercase;letter-spacing:.05em;color:var(--text-3)}
.stat-value{font-size:22px;font-weight:700;margin-top:2px}
.stat-unit{font-size:12px;font-weight:400;color:var(--text-2);margin-left:2px}
.stat-meta{font-size:12px;color:var(--text-2);margin-top:2px}
.stat.ok .stat-value,.stat.success .stat-value{color:var(--ok)}
.stat.warn .stat-value{color:var(--warn)}
.stat.danger .stat-value{color:var(--danger)}
.stat.info .stat-value{color:var(--info)}
label{display:block;font-size:13px;color:var(--text-2);margin:10px 0 4px;font-weight:500}
input,select{width:100%;padding:7px 10px;background:var(--surface-2);border:1px solid var(--border);border-radius:var(--r-sm);color:var(--text);font-size:14px;outline:none}
input:focus,select:focus{border-color:var(--primary);box-shadow:0 0 0 3px rgba(45,164,78,.15)}
.btn{display:inline-flex;align-items:center;padding:7px 14px;border:1px solid transparent;border-radius:var(--r-sm);font-size:13px;font-weight:500;cursor:pointer;text-decoration:none;transition:.15s}
.btn-primary{background:var(--primary);color:#fff;border-color:var(--primary-dark)}
.btn-primary:hover{background:var(--primary-dark)}
.btn-outline{background:transparent;border-color:var(--border);color:var(--text)}
.btn-outline:hover{background:var(--surface-2)}
.btn-red{background:#b91c1c;color:#fff}
.btn-red:hover{background:#991b1b}
.btn-blue{background:#1d4ed8;color:#fff}
.btn-blue:hover{background:#1e3a8a}
.btn-purple{background:#7c3aed;color:#fff}
.btn-purple:hover{background:#6d28d9}
.btn-orange{background:#c2410c;color:#fff}
.btn-orange:hover{background:#9a3412}
.row{display:flex;gap:8px;margin-top:12px;flex-wrap:wrap}
.badge{display:inline-flex;align-items:center;padding:2px 8px;border-radius:9999px;font-size:11px;font-weight:600}
.badge-success{background:#0d2818;color:#3fb950;border:1px solid #238636}
.badge-warning{background:#2d1e00;color:#d29922;border:1px solid #9e6a03}
.badge-danger{background:#2d0f0f;color:#f85149;border:1px solid #b91c1c}
.badge-info{background:#0d1f3c;color:#79c0ff;border:1px solid #1f6feb}
.dot{display:inline-block;width:8px;height:8px;border-radius:50%;vertical-align:middle;margin-right:4px}
.dot.online{background:#3fb950}
.dot.offline{background:#f85149}
.msg{padding:10px 14px;border-radius:var(--r-sm);margin-bottom:12px;font-size:13px}
.msg-ok{background:#0d2818;color:#3fb950;border:1px solid #238636}
.msg-err{background:#2d0f0f;color:#f85149;border:1px solid #b91c1c}
.msg-warn{background:#2d1e00;color:#d29922;border:1px solid #9e6a03}
.fill-hero{text-align:center;padding:20px 0}
.fill-num{font-size:64px;font-weight:800;line-height:1}
.fill-pct{font-size:28px;font-weight:600;color:var(--text-2)}
.bar-bg{width:100%;background:var(--surface-2);border-radius:9999px;height:8px;margin-top:8px;overflow:hidden}
.bar-fill{height:100%;border-radius:9999px;transition:width .4s ease}
.text-sm{font-size:13px}
.text-muted{color:var(--text-2)}
.mt{margin-top:12px}
pre{background:var(--surface-2);border:1px solid var(--border);border-radius:var(--r-sm);padding:12px;font-size:12px;font-family:monospace;white-space:pre-wrap;overflow-x:auto;color:var(--text-2)}
table{width:100%;border-collapse:collapse}
th,td{padding:8px 10px;text-align:left;border-bottom:1px solid var(--border);font-size:13px}
th{color:var(--text-2);font-weight:600;font-size:11px;text-transform:uppercase;letter-spacing:.05em}
.sim-banner{background:#1c1038;border:1px solid #7c3aed;border-radius:var(--r-md);padding:10px 14px;margin-bottom:14px;color:#c4b5fd;font-size:13px}
`;

function htmlHead(title: string): string {
  return `<!DOCTYPE html><html lang="en"><head>
<meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>${title} — routeOS Dock Sim</title>
<style>${CSS}</style></head><body>`;
}

function htmlNav(active: string): string {
  const link = (href: string, label: string, key: string) =>
    `<a href="${href}" class="${active === key ? "active" : ""}">${label}</a>`;
  return `<nav class="nav">
    <span class="brand">⚗️ routeOS Dock Sim</span>
    ${link("/", "Dashboard", "home")}
    ${link("/config", "Config", "config")}
    ${link("/simulator", "Simulator", "sim")}
    ${link("/calibrate", "Calibrate", "cal")}
    ${link("/mqtt", "MQTT", "mqtt")}
    <span style="margin-left:auto;font-size:12px;color:var(--text-3)">${cfg.deviceCode}</span>
  </nav>`;
}

const SIM_BANNER = `<div class="sim-banner">
  <strong style="color:#a78bfa">⚗️ SIMULATOR ACTIVE</strong> — All sensor readings are synthetic.
  MQTT payloads include <code>simulated:true</code>. Open <a href="/simulator" style="color:#c4b5fd">/simulator</a> to control patterns.
</div>`;

function fillColor(pct: number): string {
  if (pct >= 90) return "var(--danger)";
  if (pct >= 70) return "var(--warn)";
  if (pct >= 50) return "#d97706";
  return "var(--ok)";
}

function fillBadge(pct: number): string {
  if (pct >= 90) return "<span class='badge badge-danger'>Critical</span>";
  if (pct >= 70) return "<span class='badge badge-warning'>High</span>";
  if (pct >= 50) return "<span class='badge badge-warning'>Medium</span>";
  return "<span class='badge badge-success'>Low</span>";
}

// ─── Page handlers ────────────────────────────────────────────────────────────

function handleDashboard(): string {
  const active = slots.slice(0, cfg.numSensors);
  const avgFill = active.reduce((a, s) => a + s.fillPercent, 0) / active.length;
  const avgDist = computeDistanceCm(avgFill);
  const uptime = Math.floor((Date.now() - runtime.bootTime) / 1000);
  const uptimeStr = `${Math.floor(uptime / 3600)}h ${Math.floor((uptime % 3600) / 60)}m`;

  let h = htmlHead("Dashboard") + htmlNav("home");
  h += `<div class="container">${SIM_BANNER}`;

  // per-sensor cards
  h += `<div class="card"><h2>Sensor Slots</h2><div class="grid">`;
  for (let i = 0; i < cfg.numSensors; i++) {
    const s = slots[i]!;
    const typeLabel = "Ultrasonic";
    const dist = computeDistanceCm(s.fillPercent);
    h += `<div class="stat"><div class="stat-label">Slot ${i} — ${s.tag}</div>
      <div class="stat-value" style="color:${fillColor(s.fillPercent)}">${s.fillPercent.toFixed(1)}<span class="stat-unit">%</span></div>
      <div class="stat-meta">${dist.toFixed(1)} cm · ${typeLabel} · ${s.pattern}</div>
      <div class="bar-bg" style="margin-top:6px"><div class="bar-fill" style="width:${s.fillPercent.toFixed(0)}%;background:${fillColor(s.fillPercent)}"></div></div>
    </div>`;
  }
  h += `</div></div>`;

  // combined hero
  h += `<div class="card">
    <div style="display:flex;align-items:center;justify-content:space-between;margin-bottom:8px">
      <h2>Combined Fill Level</h2>${fillBadge(avgFill)}
    </div>
    <div class="fill-hero"><span class="fill-num" style="color:${fillColor(avgFill)}">${avgFill.toFixed(1)}</span><span class="fill-pct">%</span></div>
    <div class="bar-bg"><div class="bar-fill" style="width:${avgFill.toFixed(0)}%;background:${fillColor(avgFill)}"></div></div>
    <div class="text-sm text-muted" style="margin-top:8px">avg ${avgDist.toFixed(1)} cm across ${cfg.numSensors} slots</div>
  </div>`;

  // status grid
  h += `<div class="grid">
    <div class="stat ${runtime.binPresent ? "ok" : "warn"}">
      <div class="stat-label">Bin Docked</div>
      <div class="stat-value">${runtime.binPresent ? "YES" : "NO"}</div>
      <div class="stat-meta">${runtime.binPresent ? "Collection bin present" : "No bin detected"}</div>
    </div>
    <div class="stat info"><div class="stat-label">Battery</div>
      <div class="stat-value">${runtime.batteryVoltage.toFixed(2)}<span class="stat-unit">V</span></div>
    </div>
    <div class="stat"><div class="stat-label">RSSI</div>
      <div class="stat-value">${runtime.rssi}<span class="stat-unit">dBm</span></div>
    </div>
    <div class="stat ${runtime.mqttConnected ? "ok" : "warn"}">
      <div class="stat-label">MQTT</div>
      <div class="stat-value">${runtime.mqttConnected ? "OK" : "OFF"}</div>
      <div class="stat-meta"><span class="dot ${runtime.mqttConnected ? "online" : "offline"}"></span>${runtime.publishCount} published</div>
    </div>
    <div class="stat"><div class="stat-label">Uptime</div>
      <div class="stat-value" style="font-size:18px">${uptimeStr}</div>
    </div>
    <div class="stat"><div class="stat-label">Last Publish</div>
      <div class="stat-value" style="font-size:16px">${runtime.lastPublishMs ? new Date(runtime.lastPublishMs).toLocaleTimeString() : "—"}</div>
    </div>
  </div>`;

  // quick actions
  h += `<div class="card"><h2>Quick Actions</h2><div class="row">
    <form method="POST" action="/api/force-publish"><button class="btn btn-primary" type="submit">Force Publish</button></form>
    <form method="POST" action="/api/read-sensors"><button class="btn btn-outline" type="submit">Refresh Readings</button></form>
    <form method="POST" action="/api/toggle-bin"><button class="btn btn-outline" type="submit">${runtime.binPresent ? "Remove Bin" : "Dock Bin"}</button></form>
    <form method="POST" action="/api/reboot"><button class="btn btn-red" type="submit">Reboot (sim)</button></form>
  </div></div>`;

  h += `</div></body></html>`;
  return h;
}

function handleSimulator(msg = ""): string {
  const active = slots.slice(0, cfg.numSensors);
  const patterns: SimPattern[] = ["rising", "falling", "sawtooth", "random", "hold"];

  let h = htmlHead("Simulator") + htmlNav("sim");
  h += `<div class="container">${msg}${SIM_BANNER}`;

  // per-slot pattern controls
  h += `<div class="card"><h2>Per-Slot Pattern Control</h2>`;
  h += `<table><thead><tr><th>Slot</th><th>Tag</th><th>Type</th><th>Fill %</th><th>Pattern</th><th>Set</th></tr></thead><tbody>`;
  for (let i = 0; i < cfg.numSensors; i++) {
    const s = slots[i]!;
    h += `<tr><td>${i}</td><td>${s.tag}</td><td>US</td>
      <td><strong style="color:${fillColor(s.fillPercent)}">${s.fillPercent.toFixed(1)}%</strong></td>
      <td>${s.pattern}</td>
      <td><form method="POST" action="/api/set-pattern" style="display:flex;gap:4px">
        <input type="hidden" name="slot" value="${i}">
        <select name="pattern" style="width:auto;padding:4px 6px">
          ${patterns.map(p => `<option value="${p}"${s.pattern === p ? " selected" : ""}>${p}</option>`).join("")}
        </select>
        <button class="btn btn-purple" type="submit">Set</button>
      </form></td></tr>`;
  }
  h += `</tbody></table></div>`;

  // apply-to-all presets
  h += `<div class="card"><h2>Apply Preset to All Slots</h2>
    <p class="text-sm text-muted" style="margin-bottom:10px">Sets the same pattern on all ${cfg.numSensors} active slots instantly.</p>
    <div class="row">
      ${patterns.map(p => `<form method="POST" action="/api/apply-preset"><input type="hidden" name="pattern" value="${p}"><button class="btn btn-outline" type="submit">${p}</button></form>`).join("")}
    </div>
  </div>`;

    // real-time fill sliders
  h += `<div class="card"><h2>⚡ Real-Time Fill Level Sliders</h2>
    <p class="text-sm text-muted" style="margin-bottom:14px">Drag a slider to instantly override fill level and trigger MQTT publish. Sets pattern to <strong>hold</strong>.</p>`;
  for (let i = 0; i < cfg.numSensors; i++) {
    const s = slots[i]!;
    const pct = Math.round(s.fillPercent);
    const color = pct >= 95 ? "#f85149" : pct >= 80 ? "#c2410c" : pct >= 60 ? "#d97706" : "#2da44e";
    const badge = pct >= 95 ? `<span class="badge badge-danger">🔴 FULL</span>` :
                  pct >= 80 ? `<span class="badge badge-warning">🟠 ALMOST FULL</span>` : "";
    h += `<div style="margin-bottom:18px">
      <div style="display:flex;align-items:center;justify-content:space-between;margin-bottom:6px">
        <span style="font-weight:600;font-size:13px">${s.tag}</span>
        <span style="display:flex;align-items:center;gap:8px">
          ${badge}
          <span id="lbl-dock-${i}" style="font-size:20px;font-weight:800;color:${color}">${pct}%</span>
        </span>
      </div>
      <div style="position:relative;height:28px;display:flex;align-items:center">
        <div style="position:absolute;left:${80}%;top:-16px;font-size:10px;color:var(--warn);pointer-events:none">80%</div>
        <div style="position:absolute;left:${80}%;top:0;bottom:0;width:2px;background:var(--warn);opacity:0.5;pointer-events:none"></div>
        <div style="position:absolute;left:${95}%;top:0;bottom:0;width:2px;background:var(--danger);opacity:0.5;pointer-events:none"></div>
        <input type="range" id="slider-dock-${i}" min="0" max="100" step="0.5" value="${pct}"
          style="width:100%;accent-color:${color};cursor:pointer"
          oninput="dockSliderInput(${i}, this.value)">
      </div>
      <div style="display:flex;justify-content:space-between;font-size:10px;color:var(--text-3);margin-top:2px">
        <span>0%</span><span style="color:var(--warn)">80% threshold</span><span>100%</span>
      </div>
    </div>`;
  }
  h += `</div>`;

  // current MQTT payload preview
  const payload = buildPayload();
  h += `<div class="card"><h2>Current MQTT Payload Preview</h2>
    <pre id="payload-preview">${JSON.stringify(payload, null, 2)}</pre>
    <div class="row">
      <form method="POST" action="/api/force-publish"><button class="btn btn-primary" type="submit">Publish Now</button></form>
    </div>
  </div>`;

  h += `</div>
<script>
function dockSliderInput(slot, value) {
  const pct = parseFloat(value);
  const color = pct >= 95 ? "#f85149" : pct >= 80 ? "#c2410c" : pct >= 60 ? "#d97706" : "#2da44e";
  const lbl = document.getElementById("lbl-dock-"+slot);
  if (lbl) { lbl.textContent = Math.round(pct)+"%"; lbl.style.color = color; }
  const slider = document.getElementById("slider-dock-"+slot);
  if (slider) slider.style.accentColor = color;

  fetch("/api/set-fill-json", {
    method: "POST",
    headers: {"Content-Type":"application/json"},
    body: JSON.stringify({slot, fill: pct})
  }).then(r => r.json()).then(d => {
    const pre = document.getElementById("payload-preview");
    if (pre && d.payload) pre.textContent = JSON.stringify(d.payload, null, 2);
  });
}
</script>
</body></html>`;
  return h;
}

function handleConfig(msg = ""): string {
  let h = htmlHead("Config") + htmlNav("config");
  h += `<div class="container">${msg}`;
  h += `<form method="POST" action="/api/save-config">`;

  // device
  h += `<div class="card"><h2>Device Identity</h2>
    <label>Device Code</label><input type="text" name="deviceCode" value="${cfg.deviceCode}" maxlength="32">
    <label>WiFi SSID</label><input type="text" name="wifiSsid" value="${cfg.wifiSsid}">
    <label>WiFi Password</label><input type="password" name="wifiPass" placeholder="(leave blank to keep)">
  </div>`;

  // sensor slots
  h += `<div class="card"><h2>Sensor Slots</h2>
    <label>Active Sensor Count</label>
    <select name="numSensors">${[1,2].map(n => `<option value="${n}"${cfg.numSensors === n ? " selected" : ""}>${n}</option>`).join("")}</select>`;

  for (let i = 0; i < 2; i++) {
    const s = slots[i] ?? { tag: "", type: SENSOR_TYPE_US, trigPin: 0, echoPin: 0, fillPercent: 0, pattern: "hold" as SimPattern, step: 0 };
    const dim = i >= cfg.numSensors ? ";opacity:.4" : "";
    h += `<div style="margin-top:12px;padding:12px;background:var(--surface-2);border-radius:var(--r-sm)${dim}">
      <div style="font-weight:600;margin-bottom:8px">Slot ${i}${i === 0 ? " <span class='badge badge-success'>primary</span>" : ""}</div>
      <div style="display:grid;grid-template-columns:110px 80px 80px 1fr;gap:8px">
        <div><label style="margin:0 0 4px">Type</label>
          <select name="type${i}">
            <option value="0" selected>Ultrasonic</option>
          </select></div>
        <div><label style="margin:0 0 4px">TRIG</label><input type="number" name="trig${i}" value="${s.trigPin}" min="0" max="39"></div>
        <div><label style="margin:0 0 4px">ECHO</label><input type="number" name="echo${i}" value="${s.echoPin}" min="0" max="39"></div>
        <div><label style="margin:0 0 4px">Label</label><input type="text" name="tag${i}" value="${s.tag}" maxlength="31"></div>
      </div></div>`;
  }
  h += `</div>`;

  h += `<div class="card"><h2>Bin Dimensions</h2>
    <label>Bin Height (cm)</label><input type="number" name="binHeight" value="${cfg.binHeightCm}" step="0.1">
    <label>Sensor Offset (cm)</label><input type="number" name="sensorOffset" value="${cfg.offsetCm}" step="0.1">
  </div>`;

  h += `<button class="btn btn-primary" type="submit">Save Configuration</button>`;
  h += `</form>`;

  h += `<form method="POST" action="/api/factory-reset" class="mt">
    <button class="btn btn-red" type="submit" onclick="return confirm('Reset all settings?')">Factory Reset</button>
  </form>`;

  h += `</div></body></html>`;
  return h;
}

function handleMqtt(msg = ""): string {
  let h = htmlHead("MQTT") + htmlNav("mqtt");
  h += `<div class="container">${msg}`;
  h += `<form method="POST" action="/api/save-mqtt">
    <div class="card"><h2>MQTT Broker</h2>
      <label>Broker Host</label><input type="text" name="mqttBroker" value="${cfg.mqttBroker}">
      <label>Port</label><input type="number" name="mqttPort" value="${cfg.mqttPort}">
      <label>Username</label><input type="text" name="mqttUser" value="${cfg.mqttUser}">
      <label>Password</label><input type="password" name="mqttPass" placeholder="(leave blank to keep)">
      <label>Topic Prefix</label><input type="text" name="mqttTopic" value="${cfg.mqttTopic}">
      <div class="text-sm text-muted" style="margin-top:4px">Full topic: ${cfg.mqttTopic}${cfg.deviceCode}</div>
    </div>
    <div class="row">
      <button class="btn btn-primary" type="submit">Save MQTT</button>
    </div>
  </form>
  <div class="row">
    <form method="POST" action="/api/test-mqtt"><button class="btn btn-blue" type="submit">Test Connection</button></form>
    <form method="POST" action="/api/force-publish"><button class="btn btn-orange" type="submit">Force Publish</button></form>
  </div>
  <div class="card mt"><h2>Connection Status</h2>
    <div class="grid">
      <div class="stat ${runtime.mqttConnected ? "ok" : "warn"}">
        <div class="stat-label">Status</div>
        <div class="stat-value">${runtime.mqttConnected ? "Connected" : "Disconnected"}</div>
        <div class="stat-meta">${cfg.mqttBroker}:${cfg.mqttPort}</div>
      </div>
      <div class="stat info">
        <div class="stat-label">Published</div>
        <div class="stat-value">${runtime.publishCount}</div>
        <div class="stat-meta">total messages</div>
      </div>
    </div>
  </div>
  </div></body></html>`;
  return h;
}

function handleCalibrate(msg = ""): string {
  let h = htmlHead("Calibrate") + htmlNav("cal");
  h += `<div class="container">${msg}`;
  h += `<div class="card"><h2>Current Calibration</h2>
    <div class="grid">
      <div class="stat info"><div class="stat-label">Bin Height</div><div class="stat-value">${cfg.binHeightCm.toFixed(1)}<span class="stat-unit">cm</span></div></div>
      <div class="stat"><div class="stat-label">Sensor Offset</div><div class="stat-value">${cfg.offsetCm.toFixed(1)}<span class="stat-unit">cm</span></div></div>
      <div class="stat"><div class="stat-label">Full Distance</div><div class="stat-value">${cfg.fullDistCm.toFixed(1)}<span class="stat-unit">cm</span></div></div>
    </div>
  </div>`;

  h += `<div class="card"><h2>Run Calibration (simulated)</h2>
    <p class="text-sm text-muted" style="margin-bottom:10px">Runs 10 synthetic readings and averages them.</p>
    <form method="POST" action="/api/calibrate">
      <label>Target: Empty Bin Height (cm)</label><input type="number" name="targetHeight" value="${cfg.binHeightCm}" step="0.1">
      <div class="row"><button class="btn btn-primary" type="submit">Run Calibration</button></div>
    </form>
  </div>`;

  h += `</div></body></html>`;
  return h;
}

function handleStatus(): object {
  const active = slots.slice(0, cfg.numSensors);
  const avgFill = active.reduce((a, s) => a + s.fillPercent, 0) / active.length;
  return {
    device_code:        cfg.deviceCode,
    firmware_version:   FIRMWARE_VERSION,
    simulated:          true,
    bin_present:        runtime.binPresent,
    fill_level_percent: Math.round(avgFill * 10) / 10,
    distance_cm:        computeDistanceCm(avgFill),
    bin_height_cm:      cfg.binHeightCm,
    battery_voltage:    runtime.batteryVoltage,
    signal_strength:    runtime.rssi,
    mqtt_connected:     runtime.mqttConnected,
    wifi_connected:     true,
    publish_count:      runtime.publishCount,
    uptime_sec:         Math.floor((Date.now() - runtime.bootTime) / 1000),
    num_sensors:        cfg.numSensors,
    sensors: active.map((s, i) => ({
      slot: i, tag: s.tag, type: "ultrasonic",
      fill_percent: Math.round(s.fillPercent * 10) / 10,
      distance_cm: computeDistanceCm(s.fillPercent),
      pattern: s.pattern,
    })),
  };
}

// ─── HTTP server ──────────────────────────────────────────────────────────────

function parseFormBody(body: string): Record<string, string> {
  const params: Record<string, string> = {};
  for (const part of body.split("&")) {
    const [k, v] = part.split("=");
    if (k && v !== undefined) params[decodeURIComponent(k)] = decodeURIComponent(v.replace(/\+/g, " "));
  }
  return params;
}

function readBody(req: http.IncomingMessage): Promise<string> {
  return new Promise((resolve) => {
    let body = "";
    req.on("data", (c: Buffer) => { body += c.toString(); });
    req.on("end", () => resolve(body));
  });
}

function sendHtml(res: http.ServerResponse, html: string) {
  res.writeHead(200, { "Content-Type": "text/html; charset=utf-8" });
  res.end(html);
}

function sendJson(res: http.ServerResponse, data: object) {
  res.writeHead(200, { "Content-Type": "application/json" });
  res.end(JSON.stringify(data));
}

function redirect(res: http.ServerResponse, loc: string) {
  res.writeHead(302, { Location: loc });
  res.end();
}

function requireAuth(req: http.IncomingMessage, res: http.ServerResponse): boolean {
  const auth = req.headers.authorization;
  if (auth) {
    const [, encoded] = auth.split(" ");
    if (encoded) {
      const [user, pass] = Buffer.from(encoded, "base64").toString().split(":");
      if (user === cfg.portalUser && pass === cfg.portalPass) return true;
    }
  }
  res.writeHead(401, { "WWW-Authenticate": `Basic realm="routeOS Dock Sim"` });
  res.end("Unauthorized");
  return false;
}

const server = http.createServer(async (req, res) => {
  if (!requireAuth(req, res)) return;

  const url  = new URL(req.url || "/", `http://localhost:${PORT}`);
  const path = url.pathname;
  const method = req.method || "GET";

  if (method === "GET") {
    if (path === "/" || path === "/index.html") return sendHtml(res, handleDashboard());
    if (path === "/config")    return sendHtml(res, handleConfig(url.searchParams.has("saved") ? "<div class='msg msg-ok'>Config saved!</div>" : ""));
    if (path === "/simulator") return sendHtml(res, handleSimulator(url.searchParams.has("saved") ? "<div class='msg msg-ok'>Simulator updated!</div>" : ""));
    if (path === "/mqtt") {
      const msg = url.searchParams.has("saved") ? "<div class='msg msg-ok'>MQTT saved!</div>"
                : url.searchParams.has("tested") ? "<div class='msg msg-ok'>MQTT test OK (simulated)</div>" : "";
      return sendHtml(res, handleMqtt(msg));
    }
    if (path === "/calibrate") return sendHtml(res, handleCalibrate(url.searchParams.has("done") ? "<div class='msg msg-ok'>Calibration saved!</div>" : ""));
    if (path === "/api/status") return sendJson(res, handleStatus());
    if (path === "/api/payload") return sendJson(res, buildPayload());
    return redirect(res, "/");
  }

  if (method === "POST") {
    const body = await readBody(req);
    const form = parseFormBody(body);
    const ref  = (req.headers.referer as string) || "/";

    if (path === "/api/force-publish") {
      await publishNow();
      return redirect(res, ref);
    }

    if (path === "/api/read-sensors") {
      simulateTick();
      return redirect(res, "/");
    }

    if (path === "/api/toggle-bin") {
      runtime.binPresent = !runtime.binPresent;
      console.log(`[sim] Bin ${runtime.binPresent ? "DOCKED" : "REMOVED"}`);
      return redirect(res, "/");
    }

    if (path === "/api/reboot") {
      runtime.bootTime = Date.now();
      runtime.publishCount = 0;
      console.log("[sim] Rebooted (state reset)");
      return redirect(res, "/");
    }

    if (path === "/api/set-pattern") {
      const slotIdx = parseInt(form.slot ?? "0");
      const pattern = (form.pattern ?? "rising") as SimPattern;
      if (slotIdx >= 0 && slotIdx < slots.length) {
        slots[slotIdx]!.pattern = pattern;
        console.log(`[sim] Slot ${slotIdx} pattern → ${pattern}`);
      }
      return redirect(res, "/simulator?saved=1");
    }

    if (path === "/api/apply-preset") {
      const pattern = (form.pattern ?? "rising") as SimPattern;
      for (let i = 0; i < cfg.numSensors; i++) slots[i]!.pattern = pattern;
      console.log(`[sim] All slots → ${pattern}`);
      return redirect(res, "/simulator?saved=1");
    }

    if (path === "/api/set-fill") {
      const slotIdx = parseInt(form.slot ?? "-1");
      const fill    = Math.max(0, Math.min(100, parseFloat(form.fill ?? "50")));
      if (slotIdx === -1) {
        for (let i = 0; i < cfg.numSensors; i++) { slots[i]!.fillPercent = fill; slots[i]!.pattern = "hold"; }
        console.log(`[sim] All slots fill → ${fill}% (hold)`);
      } else if (slotIdx >= 0 && slotIdx < slots.length) {
        slots[slotIdx]!.fillPercent = fill;
        slots[slotIdx]!.pattern = "hold";
        console.log(`[sim] Slot ${slotIdx} fill → ${fill}% (hold)`);
      }
      return redirect(res, "/simulator?saved=1");
    }

    // JSON endpoint for real-time slider AJAX calls
    if (path === "/api/set-fill-json") {
      let jsonBody: { slot?: number; fill?: number } = {};
      try { jsonBody = JSON.parse(body); } catch { /* ignore */ }
      const slotIdx = jsonBody.slot ?? -1;
      const fill    = Math.max(0, Math.min(100, jsonBody.fill ?? 50));
      if (slotIdx === -1) {
        for (let i = 0; i < cfg.numSensors; i++) { slots[i]!.fillPercent = fill; slots[i]!.pattern = "hold"; }
      } else if (slotIdx >= 0 && slotIdx < slots.length) {
        slots[slotIdx]!.fillPercent = fill;
        slots[slotIdx]!.pattern = "hold";
      }
      await publishNow();
      res.writeHead(200, { "Content-Type": "application/json" });
      res.end(JSON.stringify({ ok: true, payload: buildPayload() }));
      return;
    }

    if (path === "/api/save-config") {
      if (form.deviceCode) cfg.deviceCode = form.deviceCode;
      if (form.wifiSsid)   cfg.wifiSsid   = form.wifiSsid;
      if (form.binHeight)  cfg.binHeightCm = parseFloat(form.binHeight);
      if (form.sensorOffset) cfg.offsetCm  = parseFloat(form.sensorOffset);
      if (form.numSensors) cfg.numSensors  = parseInt(form.numSensors);
      for (let i = 0; i < 4; i++) {
        if (!slots[i]) continue;
        if (form[`type${i}`] !== undefined) slots[i]!.type = parseInt(form[`type${i}`]!);
        if (form[`trig${i}`] !== undefined) slots[i]!.trigPin = parseInt(form[`trig${i}`]!);
        if (form[`echo${i}`] !== undefined) slots[i]!.echoPin = parseInt(form[`echo${i}`]!);
        if (form[`tag${i}`])  slots[i]!.tag = form[`tag${i}`]!;
      }
      console.log(`[sim] Config saved — device=${cfg.deviceCode} numSensors=${cfg.numSensors}`);
      return redirect(res, "/config?saved=1");
    }

    if (path === "/api/save-mqtt") {
      if (form.mqttBroker) cfg.mqttBroker = form.mqttBroker;
      if (form.mqttPort)   cfg.mqttPort   = parseInt(form.mqttPort);
      if (form.mqttUser)   cfg.mqttUser   = form.mqttUser;
      if (form.mqttPass && form.mqttPass.length > 0) cfg.mqttPass = form.mqttPass;
      if (form.mqttTopic)  cfg.mqttTopic  = form.mqttTopic;
      return redirect(res, "/mqtt?saved=1");
    }

    if (path === "/api/test-mqtt") {
      return redirect(res, "/mqtt?tested=1");
    }

    if (path === "/api/calibrate") {
      const targetHeight = parseFloat(form.targetHeight ?? String(cfg.binHeightCm));
      let log = `Calibration (simulated)\n${"─".repeat(44)}\n`;
      let sum = 0;
      for (let i = 0; i < 10; i++) {
        const d = targetHeight + (Math.random() * 4 - 2);
        log += `Reading ${i + 1}: ${d.toFixed(1)} cm\n`;
        sum += d;
      }
      cfg.binHeightCm = Math.round((sum / 10) * 10) / 10;
      log += `\nResult: ${cfg.binHeightCm.toFixed(1)} cm`;
      console.log(`[cal] height → ${cfg.binHeightCm.toFixed(1)} cm`);
      let h = htmlHead("Calibration Result") + htmlNav("cal");
      h += `<div class="container"><div class="msg msg-ok">Calibration complete!</div>
        <div class="card"><h2>Result</h2><pre>${log}</pre></div>
        <a href="/calibrate?done=1" class="btn btn-primary">Back</a>
      </div></body></html>`;
      return sendHtml(res, h);
    }

    if (path === "/api/factory-reset") {
      cfg.deviceCode = DEVICE_CODE; cfg.binHeightCm = 100; cfg.offsetCm = 2; cfg.numSensors = 2;
      cfg.mqttBroker = "109.123.238.215"; cfg.mqttPort = 1883;
      cfg.mqttUser = "ecoroute"; cfg.mqttPass = "secret";
      cfg.mqttTopic = TOPIC_PREFIX;
      slots[0] = { tag: "HC-SR04(fill)",   type: SENSOR_TYPE_US,  trigPin: 32, echoPin: 35, fillPercent: 30, pattern: "rising",   step: 0 };
      slots[1] = { tag: "RCWL-1604(dock)", type: SENSOR_TYPE_US,  trigPin:  5, echoPin: 21, fillPercent: 28, pattern: "rising",   step: 0 };
      console.log("[sim] Factory reset");
      let h = htmlHead("Factory Reset") + htmlNav("config");
      h += `<div class="container"><div class="msg msg-ok">Factory reset complete.</div>
        <a href="/" class="btn btn-primary">Dashboard</a>
      </div></body></html>`;
      return sendHtml(res, h);
    }

    return redirect(res, "/");
  }

  res.writeHead(404); res.end("Not found");
});

// ─── Boot ──────────────────────────────────────────────────────────────────────
server.listen(PORT, async () => {
  console.log(`
╔══════════════════════════════════════════════════════╗
║     routeOS Docking Station Simulator                ║
╚══════════════════════════════════════════════════════╝

  Portal:    http://localhost:${PORT}
  Login:     ${cfg.portalUser} / ${cfg.portalPass}
  Device:    ${cfg.deviceCode}
  Sensors:   ${cfg.numSensors} slots (US · US)
  MQTT:      ${BROKER_URL}  (topic: ${cfg.mqttTopic}${cfg.deviceCode})
  Publish:   every ${PUBLISH_INTERVAL_MS / 1000}s

  Press Ctrl+C to stop.
`);
  await connectMqtt();
  await publishNow();
});
