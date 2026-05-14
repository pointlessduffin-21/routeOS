/*
 * EcoRoute Lid-Version Simulator (lid-sim.ts)
 *
 * Runs the full lid-version web portal + MQTT publisher entirely on your Mac.
 * No ESP32 needed.
 *
 * Matches lid-version firmware exactly:
 *   - 3x ultrasonic sensors mounted in lid (left / center / right)
 *   - HW-201 IR lid sensor (detects open/closed)
 *   - Fill only updated when lid is CLOSED (lid-open readings discarded)
 *   - MQTT payload: fill_level_percent, left/center/right fills, lid_closed,
 *     sensors{} object, simulated:true
 *
 * Usage:
 *   bun firmware/simulator/lid-sim.ts
 *   bun firmware/simulator/lid-sim.ts --port 8093 --interval 1
 *   bun firmware/simulator/lid-sim.ts --device ECO-BIN-002
 */

import http from "http";
import mqtt from "mqtt";

const args = process.argv.slice(2);
function getArg(name: string, fallback: string): string {
  const idx = args.indexOf(`--${name}`);
  return idx >= 0 && args[idx + 1] ? args[idx + 1]! : fallback;
}

const PORT                = parseInt(getArg("port", "8093"));
const DEVICE_CODE         = getArg("device", "ECO-BIN-001");
const BROKER_URL          = process.env.MQTT_BROKER_URL ?? "mqtt://109.123.238.215:1883";
const PUBLISH_INTERVAL_MS = parseInt(getArg("interval", "1")) * 1000;
const FIRMWARE_VERSION    = "4.0.0-lid-sim";
const TOPIC_PREFIX        = "ecoroute/trash_can/";

type SimPattern = "rising" | "falling" | "sawtooth" | "random" | "hold";
const POSITIONS = ["left", "center", "right"] as const;

interface SensorSlot {
  pos: string;
  tag: string;
  trigPin: number;
  echoPin: number;
  fillPercent: number;
  pattern: SimPattern;
  step: number;
}

// ─── Device state ─────────────────────────────────────────────────────────────
const slots: SensorSlot[] = [
  { pos: "left",   tag: "us1(left)",   trigPin: 32, echoPin: 35, fillPercent: 28, pattern: "rising",   step: 0 },
  { pos: "center", tag: "us2(center)", trigPin: 26, echoPin: 27, fillPercent: 30, pattern: "rising",   step: 0 },
  { pos: "right",  tag: "us3(right)",  trigPin:  5, echoPin: 18, fillPercent: 32, pattern: "sawtooth", step: 0 },
];

const cfg = {
  deviceCode:         DEVICE_CODE,
  wifiSsid:           "SimulatedWiFi",
  binHeightCm:        100.0,
  offsetCm:           2.0,
  numSensors:         3,
  mqttBroker:         "109.123.238.215",
  mqttPort:           1883,
  mqttUser:           "ecoroute",
  mqttPass:           "secret",
  mqttTopic:          TOPIC_PREFIX,
  portalUser:         "admin",
  portalPass:         "ecoroute",
  lidMode:            "both",   // "ir" | "us" | "both"
  lidOpenThreshold:   1.5,      // if US avg > binHeight * this → lid open
};

const runtime = {
  lidClosed:        true,
  lidOpenMs:        0,           // timestamp when lid last opened (0 = closed)
  mqttConnected:    false,
  publishCount:     0,
  lastPublishMs:    0,
  rssi:             -65,
  batteryVoltage:   4.8,
  bootTime:         Date.now(),
  anomaly:          false,
};

// ─── Simulation tick ──────────────────────────────────────────────────────────
function tickSlot(s: SensorSlot) {
  // Only advance fill when lid is closed (mirrors firmware behavior)
  if (!runtime.lidClosed) return;

  switch (s.pattern) {
    case "rising":
      s.fillPercent += 0.8 + Math.random() * 0.6;
      if (s.fillPercent >= 95) s.fillPercent = 5 + Math.random() * 5;
      break;
    case "falling":
      s.fillPercent -= 0.8 + Math.random() * 0.6;
      if (s.fillPercent <= 3) s.fillPercent = 88 + Math.random() * 7;
      break;
    case "sawtooth":
      s.step = (s.step + 1) % 100;
      s.fillPercent = s.step;
      break;
    case "random":
      s.fillPercent += (Math.random() - 0.5) * 6;
      break;
    case "hold":
      break;
  }
  s.fillPercent = Math.max(0, Math.min(100, s.fillPercent));
}

function simulateTick() {
  // Randomly toggle lid 3% chance per tick
  if (Math.random() < 0.03) {
    runtime.lidClosed = !runtime.lidClosed;
    if (!runtime.lidClosed) runtime.lidOpenMs = Date.now();
    console.log(`[sim] Lid ${runtime.lidClosed ? "CLOSED" : "OPENED"}`);
  }
  for (const s of slots.slice(0, cfg.numSensors)) tickSlot(s);
  runtime.rssi = Math.max(-90, Math.min(-40, runtime.rssi + Math.floor(Math.random() * 5) - 2));
  runtime.batteryVoltage = Math.max(3.0, runtime.batteryVoltage - Math.random() * 0.001);
}
setInterval(simulateTick, 2000);

function computeDistanceCm(fill: number): number {
  const effective = cfg.binHeightCm - cfg.offsetCm;
  return Math.round((effective * (1 - fill / 100) + cfg.offsetCm) * 10) / 10;
}

function avgFill(): number {
  const active = slots.slice(0, cfg.numSensors);
  return active.reduce((a, s) => a + s.fillPercent, 0) / active.length;
}

// ─── MQTT ─────────────────────────────────────────────────────────────────────
let mqttClient: mqtt.MqttClient | null = null;

async function connectMqtt() {
  try {
    mqttClient = await mqtt.connectAsync(BROKER_URL, {
      clientId: `ecoroute-lid-sim-${Date.now()}`,
      username: cfg.mqttUser,
      password: cfg.mqttPass,
      clean: true,
    });
    runtime.mqttConnected = true;
    console.log(`[mqtt] Connected to ${BROKER_URL}`);
    mqttClient.on("error", (e) => { console.error("[mqtt] Error:", e.message); runtime.mqttConnected = false; });
    mqttClient.on("close", () => { runtime.mqttConnected = false; });
  } catch (e: any) {
    console.warn(`[mqtt] Could not connect (${e.message}) — portal still runs`);
  }
}

function buildPayload() {
  const active = slots.slice(0, cfg.numSensors);
  const fill   = avgFill();
  const dist   = computeDistanceCm(fill);

  const payload: Record<string, unknown> = {
    device_code:        cfg.deviceCode,
    firmware_version:   FIRMWARE_VERSION,
    simulated:          true,
    sim_pattern:        slots[0]!.pattern,
    fill_level_percent: Math.round(fill * 10) / 10,
    distance_cm:        dist,
    lid_closed:         runtime.lidClosed,
    anomaly_flag:       runtime.anomaly,
    signal_strength:    runtime.rssi,
    battery_voltage:    Math.round(runtime.batteryVoltage * 100) / 100,
    bin_height_cm:      cfg.binHeightCm,
    uptime_sec:         Math.floor((Date.now() - runtime.bootTime) / 1000),
    publish_count:      runtime.publishCount + 1,
  };

  // Per-position flat fields (matching firmware exactly)
  for (let i = 0; i < cfg.numSensors && i < 3; i++) {
    const pos = POSITIONS[i]!;
    payload[`${pos}_fill_percent`]  = Math.round(slots[i]!.fillPercent * 10) / 10;
    payload[`${pos}_distance_cm`]   = computeDistanceCm(slots[i]!.fillPercent);
  }

  // sensors{} object
  const sensors: Record<string, unknown> = {};
  for (let i = 0; i < cfg.numSensors && i < 3; i++) {
    const pos = POSITIONS[i]!;
    sensors[pos] = {
      distance_cm:  computeDistanceCm(slots[i]!.fillPercent),
      fill_percent: Math.round(slots[i]!.fillPercent * 10) / 10,
      trig_gpio:    slots[i]!.trigPin,
      echo_gpio:    slots[i]!.echoPin,
    };
  }
  payload["sensors"] = sensors;

  return payload;
}

async function publishNow() {
  simulateTick();
  const payload = buildPayload();
  runtime.publishCount++;
  runtime.lastPublishMs = Date.now();

  if (mqttClient?.connected) {
    const topic = `${cfg.mqttTopic}${cfg.deviceCode}`;
    await mqttClient.publishAsync(topic, JSON.stringify(payload), { qos: 1 });
    console.log(`[mqtt] → ${topic}  fill=${payload.fill_level_percent}%  lid=${runtime.lidClosed ? "CLOSED" : "OPEN"}  simulated=true`);
  } else {
    console.log(`[sim]  fill=${payload.fill_level_percent}%  lid=${runtime.lidClosed ? "CLOSED" : "OPEN"}`);
  }
}

if (PUBLISH_INTERVAL_MS > 0) setInterval(publishNow, PUBLISH_INTERVAL_MS);

// ─── HTML helpers ─────────────────────────────────────────────────────────────
const CSS = `
:root{
  --bg:#0d1117;--surface:#161b22;--surface-2:#21262d;--surface-3:#30363d;
  --border:#30363d;--text:#e6edf3;--text-2:#8b949e;--text-3:#656d76;
  --primary:#2da44e;--primary-dark:#238636;
  --info:#388bfd;--warn:#d29922;--danger:#f85149;--ok:#2da44e;
  --r-sm:6px;--r-md:8px;--r-lg:12px;
}
*{box-sizing:border-box;margin:0;padding:0}
body{font-family:system-ui,-apple-system,sans-serif;background:var(--bg);color:var(--text);font-size:14px;line-height:1.5;min-height:100vh}
.nav{background:var(--surface);border-bottom:1px solid var(--border);padding:10px 16px;display:flex;align-items:center;gap:4px;flex-wrap:wrap}
.nav .brand{font-weight:700;color:var(--primary);font-size:16px;margin-right:auto}
.nav a{color:var(--text-2);text-decoration:none;padding:6px 10px;border-radius:var(--r-sm);font-size:13px;transition:.15s}
.nav a:hover,.nav a.active{color:var(--text);background:var(--surface-2)}
.container{max-width:860px;margin:0 auto;padding:16px}
.card{background:var(--surface);border:1px solid var(--border);border-radius:var(--r-lg);padding:16px;margin-bottom:16px}
.card h2{font-size:15px;font-weight:600;margin-bottom:12px}
.grid{display:grid;grid-template-columns:repeat(auto-fit,minmax(150px,1fr));gap:10px;margin-bottom:14px}
.stat{background:var(--surface-2);border:1px solid var(--border);border-radius:var(--r-md);padding:12px}
.stat-label{font-size:11px;font-weight:600;text-transform:uppercase;letter-spacing:.05em;color:var(--text-3)}
.stat-value{font-size:22px;font-weight:700;margin-top:2px}
.stat-unit{font-size:12px;font-weight:400;color:var(--text-2);margin-left:2px}
.stat-meta{font-size:12px;color:var(--text-2);margin-top:2px}
.stat.ok .stat-value{color:var(--ok)}.stat.warn .stat-value{color:var(--warn)}.stat.danger .stat-value{color:var(--danger)}.stat.info .stat-value{color:var(--info)}
label{display:block;font-size:13px;color:var(--text-2);margin:10px 0 4px;font-weight:500}
input,select{width:100%;padding:7px 10px;background:var(--surface-2);border:1px solid var(--border);border-radius:var(--r-sm);color:var(--text);font-size:14px;outline:none}
input:focus,select:focus{border-color:var(--primary)}
.btn{display:inline-flex;align-items:center;padding:7px 14px;border:1px solid transparent;border-radius:var(--r-sm);font-size:13px;font-weight:500;cursor:pointer;text-decoration:none;transition:.15s}
.btn-primary{background:var(--primary);color:#fff;border-color:var(--primary-dark)}.btn-primary:hover{background:var(--primary-dark)}
.btn-outline{background:transparent;border-color:var(--border);color:var(--text)}.btn-outline:hover{background:var(--surface-2)}
.btn-red{background:#b91c1c;color:#fff}.btn-blue{background:#1d4ed8;color:#fff}.btn-orange{background:#c2410c;color:#fff}.btn-purple{background:#7c3aed;color:#fff}
.row{display:flex;gap:8px;margin-top:12px;flex-wrap:wrap}
.badge{display:inline-flex;align-items:center;padding:2px 8px;border-radius:9999px;font-size:11px;font-weight:600}
.badge-success{background:#0d2818;color:#3fb950;border:1px solid #238636}
.badge-warning{background:#2d1e00;color:#d29922;border:1px solid #9e6a03}
.badge-danger{background:#2d0f0f;color:#f85149;border:1px solid #b91c1c}
.badge-info{background:#0d1f3c;color:#79c0ff;border:1px solid #1f6feb}
.dot{display:inline-block;width:8px;height:8px;border-radius:50%;vertical-align:middle;margin-right:4px}
.dot.online{background:#3fb950}.dot.offline{background:#f85149}
.msg{padding:10px 14px;border-radius:var(--r-sm);margin-bottom:12px;font-size:13px}
.msg-ok{background:#0d2818;color:#3fb950;border:1px solid #238636}
.msg-err{background:#2d0f0f;color:#f85149;border:1px solid #b91c1c}
.fill-hero{text-align:center;padding:16px 0}
.fill-num{font-size:64px;font-weight:800;line-height:1}
.fill-pct{font-size:28px;font-weight:600;color:var(--text-2)}
.bar-bg{width:100%;background:var(--surface-2);border-radius:9999px;height:8px;margin-top:8px;overflow:hidden}
.bar-fill{height:100%;border-radius:9999px;transition:width .3s ease}
.text-sm{font-size:13px}.text-muted{color:var(--text-2)}.mt{margin-top:12px}
pre{background:var(--surface-2);border:1px solid var(--border);border-radius:var(--r-sm);padding:12px;font-size:12px;font-family:monospace;white-space:pre-wrap;color:var(--text-2)}
table{width:100%;border-collapse:collapse}
th,td{padding:8px 10px;text-align:left;border-bottom:1px solid var(--border);font-size:13px}
th{color:var(--text-2);font-weight:600;font-size:11px;text-transform:uppercase;letter-spacing:.05em}
.sim-banner{background:#1c1038;border:1px solid #7c3aed;border-radius:var(--r-md);padding:10px 14px;margin-bottom:14px;color:#c4b5fd;font-size:13px}
.lid-badge-closed{background:#0d2818;color:#3fb950;border:1px solid #238636;padding:4px 12px;border-radius:9999px;font-size:12px;font-weight:700}
.lid-badge-open{background:#2d1e00;color:#d29922;border:1px solid #9e6a03;padding:4px 12px;border-radius:9999px;font-size:12px;font-weight:700}
`;

function htmlHead(title: string): string {
  return `<!DOCTYPE html><html lang="en"><head>
<meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>${title} — routeOS Lid Sim</title>
<style>${CSS}</style></head><body>`;
}

function htmlNav(active: string): string {
  const link = (href: string, label: string, key: string) =>
    `<a href="${href}" class="${active === key ? "active" : ""}">${label}</a>`;
  return `<nav class="nav">
    <span class="brand">🪣 routeOS Lid Sim</span>
    ${link("/", "Dashboard", "home")}
    ${link("/config", "Config", "config")}
    ${link("/simulator", "Simulator", "sim")}
    ${link("/calibrate", "Calibrate", "cal")}
    ${link("/mqtt", "MQTT", "mqtt")}
    <span style="margin-left:auto;font-size:12px;color:var(--text-3)">${cfg.deviceCode}</span>
  </nav>`;
}

const SIM_BANNER = `<div class="sim-banner">
  <strong style="color:#a78bfa">⚗️ SIMULATOR ACTIVE</strong> — 3x ultrasonic in lid (left/center/right) + HW-201 IR lid detect.
  MQTT payloads include <code>simulated:true</code>. Open <a href="/simulator" style="color:#c4b5fd">/simulator</a> to control.
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

// ─── Pages ────────────────────────────────────────────────────────────────────
function handleDashboard(): string {
  const fill  = avgFill();
  const dist  = computeDistanceCm(fill);
  const uptime = Math.floor((Date.now() - runtime.bootTime) / 1000);
  const uptimeStr = `${Math.floor(uptime / 3600)}h ${Math.floor((uptime % 3600) / 60)}m`;

  let h = htmlHead("Dashboard") + htmlNav("home");
  h += `<div class="container">${SIM_BANNER}`;

  // Lid status banner
  h += `<div class="card" style="border-color:${runtime.lidClosed ? "#238636" : "#9e6a03"}">
    <div style="display:flex;align-items:center;justify-content:space-between">
      <h2>Lid Status</h2>
      <span class="${runtime.lidClosed ? "lid-badge-closed" : "lid-badge-open"}">${runtime.lidClosed ? "🔒 CLOSED" : "🔓 OPEN"}</span>
    </div>
    <p class="text-sm text-muted" style="margin-top:6px">
      ${runtime.lidClosed
        ? "Lid is closed — fill readings active."
        : `Lid is open — fill readings paused. Opened ${Math.round((Date.now() - runtime.lidOpenMs) / 1000)}s ago.`}
    </p>
    <div class="row">
      <form method="POST" action="/api/toggle-lid">
        <button class="btn ${runtime.lidClosed ? "btn-orange" : "btn-primary"}" type="submit">
          ${runtime.lidClosed ? "Open Lid" : "Close Lid"}
        </button>
      </form>
    </div>
  </div>`;

  // Per-sensor cards
  h += `<div class="card"><h2>Ultrasonic Sensors (Lid-Mounted)</h2><div class="grid">`;
  for (let i = 0; i < cfg.numSensors; i++) {
    const s = slots[i]!;
    const d = computeDistanceCm(s.fillPercent);
    h += `<div class="stat">
      <div class="stat-label">${s.pos.toUpperCase()} — ${s.tag}</div>
      <div class="stat-value" style="color:${fillColor(s.fillPercent)}">${s.fillPercent.toFixed(1)}<span class="stat-unit">%</span></div>
      <div class="stat-meta">${d.toFixed(1)} cm · TRIG ${s.trigPin}/ECHO ${s.echoPin} · ${s.pattern}</div>
      <div class="bar-bg" style="margin-top:6px"><div class="bar-fill" style="width:${s.fillPercent.toFixed(0)}%;background:${fillColor(s.fillPercent)}"></div></div>
    </div>`;
  }
  h += `</div></div>`;

  // Combined fill hero
  h += `<div class="card">
    <div style="display:flex;align-items:center;justify-content:space-between;margin-bottom:8px">
      <h2>Average Fill Level</h2>${fillBadge(fill)}
    </div>
    <div class="fill-hero"><span class="fill-num" style="color:${fillColor(fill)}">${fill.toFixed(1)}</span><span class="fill-pct">%</span></div>
    <div class="bar-bg"><div class="bar-fill" style="width:${fill.toFixed(0)}%;background:${fillColor(fill)}"></div></div>
    <div class="text-sm text-muted" style="margin-top:8px">avg ${dist.toFixed(1)} cm · bin height ${cfg.binHeightCm} cm</div>
  </div>`;

  // Status grid
  h += `<div class="grid">
    <div class="stat info"><div class="stat-label">Battery</div>
      <div class="stat-value">${runtime.batteryVoltage.toFixed(2)}<span class="stat-unit">V</span></div></div>
    <div class="stat"><div class="stat-label">RSSI</div>
      <div class="stat-value">${runtime.rssi}<span class="stat-unit">dBm</span></div></div>
    <div class="stat ${runtime.mqttConnected ? "ok" : "warn"}">
      <div class="stat-label">MQTT</div>
      <div class="stat-value">${runtime.mqttConnected ? "OK" : "OFF"}</div>
      <div class="stat-meta"><span class="dot ${runtime.mqttConnected ? "online" : "offline"}"></span>${runtime.publishCount} published</div>
    </div>
    <div class="stat"><div class="stat-label">Uptime</div>
      <div class="stat-value" style="font-size:18px">${uptimeStr}</div></div>
    <div class="stat"><div class="stat-label">Last Publish</div>
      <div class="stat-value" style="font-size:16px">${runtime.lastPublishMs ? new Date(runtime.lastPublishMs).toLocaleTimeString() : "—"}</div></div>
  </div>`;

  // Actions
  h += `<div class="card"><h2>Quick Actions</h2><div class="row">
    <form method="POST" action="/api/force-publish"><button class="btn btn-primary">Force Publish</button></form>
    <form method="POST" action="/api/read-sensors"><button class="btn btn-outline">Refresh Readings</button></form>
    <form method="POST" action="/api/reboot"><button class="btn btn-red">Reboot (sim)</button></form>
  </div></div>`;

  h += `</div></body></html>`;
  return h;
}

function handleSimulator(msg = ""): string {
  const patterns: SimPattern[] = ["rising", "falling", "sawtooth", "random", "hold"];
  let h = htmlHead("Simulator") + htmlNav("sim");
  h += `<div class="container">${msg}${SIM_BANNER}`;

  // Per-slot controls
  h += `<div class="card"><h2>Per-Sensor Pattern Control</h2>
    <table><thead><tr><th>Slot</th><th>Position</th><th>Tag</th><th>Fill %</th><th>Pattern</th><th>Set</th></tr></thead><tbody>`;
  for (let i = 0; i < cfg.numSensors; i++) {
    const s = slots[i]!;
    h += `<tr><td>${i}</td><td><strong>${s.pos}</strong></td><td>${s.tag}</td>
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

  // Lid control
  h += `<div class="card"><h2>Lid Control (HW-201 IR Sim)</h2>
    <p class="text-sm text-muted" style="margin-bottom:10px">
      Lid status: <span class="${runtime.lidClosed ? "lid-badge-closed" : "lid-badge-open"}">${runtime.lidClosed ? "🔒 CLOSED" : "🔓 OPEN"}</span>
      &nbsp;— fill advances only when lid is CLOSED.
    </p>
    <div class="row">
      <form method="POST" action="/api/toggle-lid">
        <button class="btn ${runtime.lidClosed ? "btn-orange" : "btn-primary"}" type="submit">
          ${runtime.lidClosed ? "Open Lid" : "Close Lid"}
        </button>
      </form>
    </div>
  </div>`;

  // Presets
  h += `<div class="card"><h2>Apply Preset to All Sensors</h2>
    <div class="row">
      ${patterns.map(p => `<form method="POST" action="/api/apply-preset"><input type="hidden" name="pattern" value="${p}"><button class="btn btn-outline" type="submit">${p}</button></form>`).join("")}
    </div>
  </div>`;

  // Fill override with real-time sliders
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
        <span style="font-weight:600;font-size:13px">${s.pos} — ${s.tag}</span>
        <span style="display:flex;align-items:center;gap:8px">
          ${badge}
          <span id="lbl-lid-${i}" style="font-size:20px;font-weight:800;color:${color}">${pct}%</span>
        </span>
      </div>
      <div style="position:relative;height:28px;display:flex;align-items:center">
        <div style="position:absolute;left:${80}%;top:-16px;font-size:10px;color:var(--warn);pointer-events:none">80%</div>
        <div style="position:absolute;left:${80}%;top:0;bottom:0;width:2px;background:var(--warn);opacity:0.5;pointer-events:none"></div>
        <div style="position:absolute;left:${95}%;top:0;bottom:0;width:2px;background:var(--danger);opacity:0.5;pointer-events:none"></div>
        <input type="range" id="slider-lid-${i}" min="0" max="100" step="0.5" value="${pct}"
          style="width:100%;accent-color:${color};cursor:pointer"
          oninput="lidSliderInput(${i}, this.value)">
      </div>
      <div style="display:flex;justify-content:space-between;font-size:10px;color:var(--text-3);margin-top:2px">
        <span>0%</span><span style="color:var(--warn)">80% threshold</span><span>100%</span>
      </div>
    </div>`;
  }
  h += `</div>`;

  // Payload preview
  h += `<div class="card"><h2>Current MQTT Payload Preview</h2>
    <pre id="payload-preview">${JSON.stringify(buildPayload(), null, 2)}</pre>
    <div class="row">
      <form method="POST" action="/api/force-publish"><button class="btn btn-primary">Publish Now</button></form>
    </div>
  </div>`;

  h += `</div>
<script>
function lidSliderInput(slot, value) {
  const pct = parseFloat(value);
  const color = pct >= 95 ? "#f85149" : pct >= 80 ? "#c2410c" : pct >= 60 ? "#d97706" : "#2da44e";
  const lbl = document.getElementById("lbl-lid-"+slot);
  if (lbl) { lbl.textContent = Math.round(pct)+"%"; lbl.style.color = color; }
  const slider = document.getElementById("slider-lid-"+slot);
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
  h += `<div class="container">${msg}<form method="POST" action="/api/save-config">`;
  h += `<div class="card"><h2>Device Identity</h2>
    <label>Device Code</label><input type="text" name="deviceCode" value="${cfg.deviceCode}">
    <label>WiFi SSID</label><input type="text" name="wifiSsid" value="${cfg.wifiSsid}">
    <label>WiFi Password</label><input type="password" name="wifiPass" placeholder="(leave blank to keep)">
  </div>`;

  h += `<div class="card"><h2>Sensor Configuration (3x Ultrasonic in Lid)</h2>
    <p class="text-sm text-muted" style="margin-bottom:10px">All three sensors are ultrasonic, mounted inside the bin lid facing down.</p>
    <label>Active Sensor Count</label>
    <select name="numSensors">${[1,2,3,4].map(n => `<option value="${n}"${cfg.numSensors === n ? " selected" : ""}>${n}</option>`).join("")}</select>`;
  for (let i = 0; i < 3; i++) {
    const s = slots[i] ?? { pos: `slot${i}`, tag: "", trigPin: 0, echoPin: 0, fillPercent: 0, pattern: "hold" as SimPattern, step: 0 };
    const dim = i >= cfg.numSensors ? ";opacity:.4" : "";
    h += `<div style="margin-top:10px;padding:10px;background:var(--surface-2);border-radius:var(--r-sm)${dim}">
      <div style="font-weight:600;margin-bottom:6px">Slot ${i} — ${s.pos.toUpperCase()}</div>
      <div style="display:grid;grid-template-columns:80px 80px 1fr;gap:8px">
        <div><label style="margin:0 0 4px">TRIG</label><input type="number" name="trig${i}" value="${s.trigPin}" min="0" max="39"></div>
        <div><label style="margin:0 0 4px">ECHO</label><input type="number" name="echo${i}" value="${s.echoPin}" min="0" max="39"></div>
        <div><label style="margin:0 0 4px">Label</label><input type="text" name="tag${i}" value="${s.tag}" maxlength="31"></div>
      </div></div>`;
  }
  h += `</div>`;

  h += `<div class="card"><h2>Lid Sensor (HW-201 IR)</h2>
    <label>Lid Mode</label>
    <select name="lidMode">
      <option value="ir"${cfg.lidMode === "ir" ? " selected" : ""}>IR only</option>
      <option value="us"${cfg.lidMode === "us" ? " selected" : ""}>Ultrasonic only</option>
      <option value="both"${cfg.lidMode === "both" ? " selected" : ""}>Both (IR + US)</option>
    </select>
    <label>US Lid-Open Threshold (× bin height)</label>
    <input type="number" name="lidOpenThr" value="${cfg.lidOpenThreshold}" step="0.1" min="1.0" max="3.0">
    <div class="text-sm text-muted" style="margin-top:4px">If avg distance > binHeight × threshold → lid detected open.</div>
  </div>`;

  h += `<div class="card"><h2>Bin Dimensions</h2>
    <label>Bin Height (cm)</label><input type="number" name="binHeight" value="${cfg.binHeightCm}" step="0.1">
    <label>Sensor Offset (cm)</label><input type="number" name="sensorOffset" value="${cfg.offsetCm}" step="0.1">
  </div>`;

  h += `<button class="btn btn-primary" type="submit">Save Config</button></form>`;
  h += `<form method="POST" action="/api/factory-reset" class="mt">
    <button class="btn btn-red" type="submit" onclick="return confirm('Reset all settings?')">Factory Reset</button>
  </form></div></body></html>`;
  return h;
}

function handleMqtt(msg = ""): string {
  let h = htmlHead("MQTT") + htmlNav("mqtt");
  h += `<div class="container">${msg}
    <form method="POST" action="/api/save-mqtt">
      <div class="card"><h2>MQTT Broker</h2>
        <label>Host</label><input type="text" name="mqttBroker" value="${cfg.mqttBroker}">
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
      <form method="POST" action="/api/test-mqtt"><button class="btn btn-blue">Test Connection</button></form>
      <form method="POST" action="/api/force-publish"><button class="btn btn-orange">Force Publish</button></form>
    </div>
    <div class="card mt"><h2>Status</h2>
      <div class="grid">
        <div class="stat ${runtime.mqttConnected ? "ok" : "warn"}">
          <div class="stat-label">Connection</div>
          <div class="stat-value">${runtime.mqttConnected ? "Connected" : "Disconnected"}</div>
          <div class="stat-meta">${cfg.mqttBroker}:${cfg.mqttPort}</div>
        </div>
        <div class="stat info">
          <div class="stat-label">Published</div>
          <div class="stat-value">${runtime.publishCount}</div>
        </div>
      </div>
    </div>
  </div></body></html>`;
  return h;
}

function handleCalibrate(msg = ""): string {
  let h = htmlHead("Calibrate") + htmlNav("cal");
  h += `<div class="container">${msg}
    <div class="card"><h2>Current Calibration</h2>
      <div class="grid">
        <div class="stat info"><div class="stat-label">Bin Height</div><div class="stat-value">${cfg.binHeightCm.toFixed(1)}<span class="stat-unit">cm</span></div></div>
        <div class="stat"><div class="stat-label">Sensor Offset</div><div class="stat-value">${cfg.offsetCm.toFixed(1)}<span class="stat-unit">cm</span></div></div>
      </div>
    </div>
    <div class="card"><h2>Run Calibration (simulated)</h2>
      <p class="text-sm text-muted" style="margin-bottom:10px">Lid must be CLOSED for calibration. Simulates 10 readings per sensor.</p>
      <form method="POST" action="/api/calibrate">
        <label>Target: Empty Bin Height (cm)</label>
        <input type="number" name="targetHeight" value="${cfg.binHeightCm}" step="0.1">
        <div class="row"><button class="btn btn-primary" type="submit">Run Calibration</button></div>
      </form>
    </div>
  </div></body></html>`;
  return h;
}

function handleStatus(): object {
  const fill = avgFill();
  return {
    device_code:        cfg.deviceCode,
    firmware_version:   FIRMWARE_VERSION,
    simulated:          true,
    fill_level_percent: Math.round(fill * 10) / 10,
    distance_cm:        computeDistanceCm(fill),
    lid_closed:         runtime.lidClosed,
    anomaly_flag:       runtime.anomaly,
    signal_strength:    runtime.rssi,
    battery_voltage:    runtime.batteryVoltage,
    mqtt_connected:     runtime.mqttConnected,
    wifi_connected:     true,
    publish_count:      runtime.publishCount,
    uptime_sec:         Math.floor((Date.now() - runtime.bootTime) / 1000),
    bin_height_cm:      cfg.binHeightCm,
    num_sensors:        cfg.numSensors,
    sensors: Object.fromEntries(slots.slice(0, cfg.numSensors).map(s => [
      s.pos, {
        fill_percent: Math.round(s.fillPercent * 10) / 10,
        distance_cm:  computeDistanceCm(s.fillPercent),
        pattern:      s.pattern,
        trig_gpio:    s.trigPin,
        echo_gpio:    s.echoPin,
      }
    ])),
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
  return new Promise(resolve => { let b = ""; req.on("data", c => b += c); req.on("end", () => resolve(b)); });
}
function sendHtml(res: http.ServerResponse, html: string) {
  res.writeHead(200, { "Content-Type": "text/html; charset=utf-8" }); res.end(html);
}
function sendJson(res: http.ServerResponse, data: object) {
  res.writeHead(200, { "Content-Type": "application/json" }); res.end(JSON.stringify(data));
}
function redirect(res: http.ServerResponse, loc: string) {
  res.writeHead(302, { Location: loc }); res.end();
}
function requireAuth(req: http.IncomingMessage, res: http.ServerResponse): boolean {
  const auth = req.headers.authorization;
  if (auth) {
    const [, enc] = auth.split(" ");
    if (enc) {
      const [user, pass] = Buffer.from(enc, "base64").toString().split(":");
      if (user === cfg.portalUser && pass === cfg.portalPass) return true;
    }
  }
  res.writeHead(401, { "WWW-Authenticate": `Basic realm="routeOS Lid Sim"` });
  res.end("Unauthorized");
  return false;
}

const server = http.createServer(async (req, res) => {
  if (!requireAuth(req, res)) return;
  const url    = new URL(req.url || "/", `http://localhost:${PORT}`);
  const path   = url.pathname;
  const method = req.method || "GET";

  if (method === "GET") {
    if (path === "/" || path === "/index.html") return sendHtml(res, handleDashboard());
    if (path === "/config")    return sendHtml(res, handleConfig(url.searchParams.has("saved") ? "<div class='msg msg-ok'>Config saved!</div>" : ""));
    if (path === "/simulator") return sendHtml(res, handleSimulator(url.searchParams.has("saved") ? "<div class='msg msg-ok'>Simulator updated!</div>" : ""));
    if (path === "/mqtt") {
      const msg = url.searchParams.has("saved") ? "<div class='msg msg-ok'>MQTT saved!</div>"
                : url.searchParams.has("tested") ? "<div class='msg msg-ok'>Test OK (simulated)</div>" : "";
      return sendHtml(res, handleMqtt(msg));
    }
    if (path === "/calibrate") return sendHtml(res, handleCalibrate(url.searchParams.has("done") ? "<div class='msg msg-ok'>Calibration saved!</div>" : ""));
    if (path === "/api/status")  return sendJson(res, handleStatus());
    if (path === "/api/payload") return sendJson(res, buildPayload());
    return redirect(res, "/");
  }

  if (method === "POST") {
    const body = await readBody(req);
    const form = parseFormBody(body);
    const ref  = (req.headers.referer as string) || "/";

    if (path === "/api/force-publish") { await publishNow(); return redirect(res, ref); }
    if (path === "/api/read-sensors")  { simulateTick(); return redirect(res, "/"); }
    if (path === "/api/reboot")        { runtime.bootTime = Date.now(); runtime.publishCount = 0; return redirect(res, "/"); }

    if (path === "/api/toggle-lid") {
      runtime.lidClosed = !runtime.lidClosed;
      if (!runtime.lidClosed) runtime.lidOpenMs = Date.now();
      console.log(`[sim] Lid ${runtime.lidClosed ? "CLOSED" : "OPENED"}`);
      return redirect(res, ref);
    }

    if (path === "/api/set-pattern") {
      const i = parseInt(form.slot ?? "0");
      const p = (form.pattern ?? "rising") as SimPattern;
      if (i >= 0 && i < slots.length) { slots[i]!.pattern = p; console.log(`[sim] slot ${i} → ${p}`); }
      return redirect(res, "/simulator?saved=1");
    }

    if (path === "/api/apply-preset") {
      const p = (form.pattern ?? "rising") as SimPattern;
      for (let i = 0; i < cfg.numSensors; i++) slots[i]!.pattern = p;
      console.log(`[sim] all slots → ${p}`);
      return redirect(res, "/simulator?saved=1");
    }

    if (path === "/api/set-fill") {
      const i    = parseInt(form.slot ?? "-1");
      const fill = Math.max(0, Math.min(100, parseFloat(form.fill ?? "50")));
      if (i === -1) {
        for (let j = 0; j < cfg.numSensors; j++) { slots[j]!.fillPercent = fill; slots[j]!.pattern = "hold"; }
      } else if (i >= 0 && i < slots.length) {
        slots[i]!.fillPercent = fill; slots[i]!.pattern = "hold";
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
        for (let j = 0; j < cfg.numSensors; j++) { slots[j]!.fillPercent = fill; slots[j]!.pattern = "hold"; }
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
      if (form.lidMode)    cfg.lidMode     = form.lidMode;
      if (form.lidOpenThr) cfg.lidOpenThreshold = parseFloat(form.lidOpenThr);
      for (let i = 0; i < 3; i++) {
        if (!slots[i]) continue;
        if (form[`trig${i}`]) slots[i]!.trigPin = parseInt(form[`trig${i}`]!);
        if (form[`echo${i}`]) slots[i]!.echoPin = parseInt(form[`echo${i}`]!);
        if (form[`tag${i}`])  slots[i]!.tag = form[`tag${i}`]!;
      }
      return redirect(res, "/config?saved=1");
    }

    if (path === "/api/save-mqtt") {
      if (form.mqttBroker) cfg.mqttBroker = form.mqttBroker;
      if (form.mqttPort)   cfg.mqttPort   = parseInt(form.mqttPort);
      if (form.mqttUser)   cfg.mqttUser   = form.mqttUser;
      if (form.mqttPass?.length > 0) cfg.mqttPass = form.mqttPass;
      if (form.mqttTopic)  cfg.mqttTopic  = form.mqttTopic;
      return redirect(res, "/mqtt?saved=1");
    }

    if (path === "/api/test-mqtt") return redirect(res, "/mqtt?tested=1");

    if (path === "/api/calibrate") {
      const target = parseFloat(form.targetHeight ?? String(cfg.binHeightCm));
      let log = `Calibration — ${cfg.numSensors} sensors (lid closed)\n${"─".repeat(44)}\n`;
      for (let i = 0; i < cfg.numSensors; i++) {
        let sum = 0;
        for (let j = 0; j < 10; j++) {
          const d = target + (Math.random() * 4 - 2);
          log += `[${slots[i]!.pos}] Reading ${j+1}: ${d.toFixed(1)} cm\n`;
          sum += d;
        }
        const avg = sum / 10;
        log += `[${slots[i]!.pos}] → avg ${avg.toFixed(1)} cm\n\n`;
      }
      cfg.binHeightCm = Math.round(target * 10) / 10;
      log += `Bin height set to ${cfg.binHeightCm.toFixed(1)} cm`;
      let h = htmlHead("Calibration Result") + htmlNav("cal");
      h += `<div class="container"><div class="msg msg-ok">Calibration complete!</div>
        <div class="card"><h2>Result</h2><pre>${log}</pre></div>
        <a href="/calibrate?done=1" class="btn btn-primary">Back</a>
      </div></body></html>`;
      return sendHtml(res, h);
    }

    if (path === "/api/factory-reset") {
      cfg.deviceCode = DEVICE_CODE; cfg.binHeightCm = 100; cfg.offsetCm = 2;
      cfg.numSensors = 3; cfg.lidMode = "both"; cfg.lidOpenThreshold = 1.5;
      cfg.mqttBroker = "109.123.238.215"; cfg.mqttPort = 1883;
      cfg.mqttUser = "ecoroute"; cfg.mqttPass = "secret"; cfg.mqttTopic = TOPIC_PREFIX;
      slots[0] = { pos: "left",   tag: "us1(left)",   trigPin: 32, echoPin: 35, fillPercent: 28, pattern: "rising", step: 0 };
      slots[1] = { pos: "center", tag: "us2(center)", trigPin: 26, echoPin: 27, fillPercent: 30, pattern: "rising", step: 0 };
      slots[2] = { pos: "right",  tag: "us3(right)",  trigPin:  5, echoPin: 18, fillPercent: 32, pattern: "rising", step: 0 };
      let h = htmlHead("Factory Reset") + htmlNav("config");
      h += `<div class="container"><div class="msg msg-ok">Factory reset complete.</div>
        <a href="/" class="btn btn-primary">Dashboard</a></div></body></html>`;
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
║     routeOS Lid-Version Simulator                    ║
╚══════════════════════════════════════════════════════╝

  Portal:    http://localhost:${PORT}
  Login:     ${cfg.portalUser} / ${cfg.portalPass}
  Device:    ${cfg.deviceCode}
  Sensors:   3x ultrasonic (left/center/right) + HW-201 IR lid
  MQTT:      ${BROKER_URL}  (topic: ${cfg.mqttTopic}${cfg.deviceCode})
  Publish:   every ${PUBLISH_INTERVAL_MS / 1000}s

  Press Ctrl+C to stop.
`);
  await connectMqtt();
  await publishNow();
});
