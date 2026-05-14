/*
 * EcoRoute Captive Portal Simulator
 *
 * Serves the exact same web UI as the ESP32 captive portal firmware,
 * but runs locally on Bun/Node with mock sensor data.
 * Lets you test the full portal UI without physical hardware.
 *
 * Usage:
 *   bun firmware/simulator/portal-sim.ts
 *   bun firmware/simulator/portal-sim.ts --port 8080
 *   bun firmware/simulator/portal-sim.ts --device ECO-BIN-002 --ir
 */

const args = process.argv.slice(2);
function getArg(name: string, fallback: string): string {
  const idx = args.indexOf(`--${name}`);
  return idx >= 0 && args[idx + 1] ? args[idx + 1]! : fallback;
}
const PORT = parseInt(getArg("port", "8088"));
const HAS_IR = args.includes("--ir");

// ─── Simulated device state ──────────────────────────────────────────────────

interface DeviceState {
  // Config (persisted in memory, like NVS)
  deviceCode: string;
  wifiSsid: string;
  wifiPass: string;
  mqttBroker: string;
  mqttPort: number;
  mqttUser: string;
  mqttPass: string;
  mqttTopic: string;
  portalUser: string;
  portalPass: string;
  binHeightCm: number;
  sensorOffsetCm: number;

  // Live sensor readings (simulated)
  distanceCm: number;
  fillPercent: number;
  batteryVoltage: number;
  batteryPercent: number;
  rssi: number;
  lidClosed: boolean;
  anomaly: boolean;

  // Status
  mqttConnected: boolean;
  wifiConnected: boolean;
  publishCount: number;
  lastPublishTime: number;
  bootTime: number;
}

const state: DeviceState = {
  deviceCode: getArg("device", "ECO-BIN-001"),
  wifiSsid: "SimulatedWiFi",
  wifiPass: "simpass123",
  mqttBroker: "109.123.238.215",
  mqttPort: 1883,
  mqttUser: "ecoroute",
  mqttPass: "secret",
  mqttTopic: "ecoroute/trash_can/",
  portalUser: "admin",
  portalPass: "ecoroute",
  binHeightCm: 100.0,
  sensorOffsetCm: 2.0,

  distanceCm: 45.0,
  fillPercent: 55.0,
  batteryVoltage: 3.85,
  batteryPercent: 78,
  rssi: -62,
  lidClosed: true,
  anomaly: false,

  mqttConnected: false,
  wifiConnected: true,
  publishCount: 42,
  lastPublishTime: Date.now(),
  bootTime: Date.now(),
};

const FIRMWARE_VERSION = HAS_IR ? "3.0.0-tc2" : "3.0.0";

// ─── Sensor simulation ──────────────────────────────────────────────────────

function simulateSensors() {
  // Drift fill level
  state.fillPercent += (Math.random() - 0.45) * 3;
  state.fillPercent = Math.max(0, Math.min(100, state.fillPercent));

  // Calculate distance from fill
  const effectiveHeight = state.binHeightCm - state.sensorOffsetCm;
  state.distanceCm = effectiveHeight * (1 - state.fillPercent / 100) + state.sensorOffsetCm;
  state.distanceCm = Math.round(state.distanceCm * 10) / 10;

  // Battery slowly drains
  state.batteryVoltage -= Math.random() * 0.001;
  state.batteryVoltage = Math.max(3.0, state.batteryVoltage);
  state.batteryPercent = batteryPercentage(state.batteryVoltage);

  // RSSI wobbles
  state.rssi += Math.floor(Math.random() * 5) - 2;
  state.rssi = Math.max(-90, Math.min(-40, state.rssi));

  // Lid randomly toggles (if IR enabled)
  if (HAS_IR && Math.random() < 0.05) {
    state.lidClosed = !state.lidClosed;
  }

  state.anomaly = false;
}

function batteryPercentage(v: number): number {
  if (v >= 4.2) return 100;
  if (v <= 3.0) return 0;
  if (v >= 3.9) return 75 + Math.round((v - 3.9) / 0.3 * 25);
  if (v >= 3.7) return 50 + Math.round((v - 3.7) / 0.2 * 25);
  if (v >= 3.5) return 25 + Math.round((v - 3.5) / 0.2 * 25);
  if (v >= 3.3) return 10 + Math.round((v - 3.3) / 0.2 * 15);
  return Math.round((v - 3.0) / 0.3 * 10);
}

// Simulate every 5s
setInterval(simulateSensors, 5000);

// ─── HTML helpers (matching firmware exactly) ────────────────────────────────

function fillColor(pct: number): string {
  if (pct >= 90) return "#dc2626";
  if (pct >= 70) return "#d97706";
  if (pct >= 50) return "#ca8a04";
  return "#16a34a";
}

function battClass(v: number): string {
  if (v < 3.0) return "stat-crit";
  if (v < 3.3) return "stat-warn";
  return "stat-ok";
}

function fillBadge(pct: number): string {
  if (pct >= 90) return `<span class='badge badge-danger'>Critical</span>`;
  if (pct >= 70) return `<span class='badge badge-warning'>High</span>`;
  if (pct >= 50) return `<span class='badge badge-warning'>Medium</span>`;
  return `<span class='badge badge-success'>Low</span>`;
}

function htmlHead(title: string): string {
  return `<!DOCTYPE html><html><head><meta charset='utf-8'>
<meta name='viewport' content='width=device-width,initial-scale=1'>
<title>${title} — EcoRoute</title>
<style>
:root{--primary-500:#4f46e5;--primary-600:#4338ca;--primary-50:#eef2ff;--primary-text:#fff;
--slate-50:#f8fafc;--slate-100:#f1f5f9;--slate-200:#e2e8f0;--slate-400:#94a3b8;--slate-600:#475569;--slate-800:#1e293b;--slate-900:#0f172a;--white:#fff;
--success-bg:#dcfce7;--success-text:#166534;--warning-bg:#fef9c3;--warning-text:#854d0e;--danger-bg:#fee2e2;--danger-text:#991b1b;
--radius-md:6px;--radius-lg:10px}
*{box-sizing:border-box;margin:0;padding:0}
body{font-family:system-ui,-apple-system,sans-serif;background:var(--slate-50);color:var(--slate-900);font-size:14px;line-height:1.5}
.nav{background:var(--white);padding:12px 16px;display:flex;align-items:center;gap:8px;border-bottom:1px solid var(--slate-200);flex-wrap:wrap}
.nav .brand{font-weight:700;font-size:18px;color:var(--primary-500);margin-right:auto}
.nav a{color:var(--slate-600);text-decoration:none;font-size:13px;padding:6px 12px;border-radius:var(--radius-md);transition:all .2s}
.nav a:hover,.nav a.active{color:var(--primary-500);background:var(--primary-50)}
.container{max-width:800px;margin:0 auto;padding:16px}
.card{background:var(--white);border:1px solid var(--slate-200);border-radius:var(--radius-lg);padding:16px;margin-bottom:16px}
.card h2{font-size:16px;font-weight:600;margin-bottom:12px;color:var(--slate-800)}
.hmi-grid{display:grid;grid-template-columns:repeat(auto-fit,minmax(140px,1fr));gap:12px;margin-bottom:16px}
.card-label{font-size:11px;font-weight:600;text-transform:uppercase;letter-spacing:.05em;color:var(--slate-600)}
.card-value{font-size:24px;font-weight:700;color:var(--slate-900);margin-top:2px}
.card-unit{font-size:12px;font-weight:400;color:var(--slate-400);margin-left:2px}
.stat-ok .card-value{color:#16a34a}
.stat-warn .card-value{color:#d97706}
.stat-crit .card-value{color:#dc2626}
.stat-info .card-value{color:var(--primary-500)}
label{display:block;font-size:13px;color:var(--slate-600);margin:10px 0 4px;font-weight:500}
input[type=text],input[type=password],input[type=number]{width:100%;padding:8px 12px;background:var(--white);border:1px solid var(--slate-200);border-radius:var(--radius-md);color:var(--slate-900);font-size:14px;outline:none}
input:focus{border-color:var(--primary-500);box-shadow:0 0 0 3px rgba(79,70,229,.1)}
.btn{display:inline-flex;align-items:center;justify-content:center;padding:8px 16px;border:1px solid transparent;border-radius:var(--radius-md);font-size:14px;font-weight:500;cursor:pointer;gap:6px;transition:all .2s;text-decoration:none}
.btn-primary{background:var(--primary-500);color:var(--primary-text)}
.btn-primary:hover{background:var(--primary-600)}
.btn-outline{background:transparent;border-color:var(--slate-200);color:var(--slate-900)}
.btn-outline:hover{background:var(--slate-50);border-color:var(--slate-400)}
.btn-orange{background:#f97316;color:#fff}
.btn-orange:hover{background:#ea580c}
.btn-red{background:#ef4444;color:#fff}
.btn-red:hover{background:#dc2626}
.btn-blue{background:#3b82f6;color:#fff}
.btn-blue:hover{background:#2563eb}
.row{display:flex;gap:8px;margin-top:12px;flex-wrap:wrap}
.badge{display:inline-flex;align-items:center;padding:2px 8px;border-radius:9999px;font-size:11px;font-weight:600}
.badge-success{background:var(--success-bg);color:var(--success-text)}
.badge-warning{background:var(--warning-bg);color:var(--warning-text)}
.badge-danger{background:var(--danger-bg);color:var(--danger-text)}
.status-dot{display:inline-block;width:8px;height:8px;border-radius:50%;margin-right:6px}
.status-dot.online{background:#22c55e}
.status-dot.offline{background:#ef4444}
.msg{padding:12px 16px;border-radius:var(--radius-md);margin-bottom:12px;font-size:14px}
.msg-ok{background:var(--success-bg);color:var(--success-text);border:1px solid #bbf7d0}
.msg-err{background:var(--danger-bg);color:var(--danger-text);border:1px solid #fecaca}
pre{background:var(--slate-100);padding:12px;border-radius:var(--radius-md);font-size:12px;overflow-x:auto;white-space:pre-wrap;color:var(--slate-600);max-height:300px;overflow-y:auto;border:1px solid var(--slate-200)}
.bar-bg{background:var(--slate-200);border-radius:99px;height:10px;overflow:hidden;margin-top:8px}
.bar-fill{height:100%;border-radius:99px;transition:width .5s}
.mt{margin-top:16px}
.flex{display:flex}.items-center{align-items:center}.justify-between{justify-content:space-between}.gap-2{gap:.5rem}.gap-4{gap:1rem}.mt-4{margin-top:1rem}.mb-4{margin-bottom:1rem}
.text-muted{color:var(--slate-600)}.text-sm{font-size:12px}
.sim-banner{background:#fef3c7;border:1px solid #fde68a;border-radius:var(--radius-md);padding:10px 16px;margin-bottom:16px;font-size:13px;color:#92400e;display:flex;align-items:center;gap:8px}
</style></head><body>`;
}

function htmlNav(active: string): string {
  const link = (href: string, label: string, id: string) =>
    `<a href='${href}'${active === id ? " class='active'" : ""}>${label}</a>`;
  return `<div class='nav'><span class='brand'>EcoRoute</span>
    ${link("/", "Dashboard", "dash")}
    ${link("/config", "Config", "config")}
    ${link("/mqtt", "MQTT", "mqtt")}
    ${link("/calibrate", "Calibrate", "cal")}
    ${link("/portal", "Security", "portal")}
  </div>`;
}

const SIM_BANNER = `<div class='sim-banner'>&#9888; Simulator Mode — sensor data is simulated, no real hardware connected</div>`;

// ─── Route handlers ──────────────────────────────────────────────────────────

function handleDashboard(): string {
  const upSec = Math.floor((Date.now() - state.bootTime) / 1000);
  let uptime: string;
  if (upSec >= 86400) uptime = `${Math.floor(upSec / 86400)}d ${Math.floor((upSec % 86400) / 3600)}h`;
  else if (upSec >= 3600) uptime = `${Math.floor(upSec / 3600)}h ${Math.floor((upSec % 3600) / 60)}m`;
  else uptime = `${Math.floor(upSec / 60)}m ${upSec % 60}s`;

  let lastPubAgo = "Never";
  if (state.lastPublishTime > state.bootTime) {
    const ago = Math.floor((Date.now() - state.lastPublishTime) / 1000);
    if (ago < 60) lastPubAgo = `${ago}s ago`;
    else if (ago < 3600) lastPubAgo = `${Math.floor(ago / 60)}m ago`;
    else lastPubAgo = `${Math.floor(ago / 3600)}h ago`;
  }

  const fillPct = state.fillPercent >= 0 ? state.fillPercent : 0;
  const fillCol = fillColor(fillPct);
  const mqttDot = state.mqttConnected ? "online" : "offline";
  const wifiDot = state.wifiConnected ? "online" : "offline";

  let h = htmlHead("Dashboard") + htmlNav("dash");
  h += `<div class='container'>${SIM_BANNER}`;

  // Header
  h += `<div class='flex items-center justify-between mb-4'>`;
  h += `<div><h1>${state.deviceCode}</h1><span class='text-sm text-muted'>Firmware v${FIRMWARE_VERSION} &middot; Uptime: ${uptime}</span></div>`;
  h += `<div><span class='status-dot ${mqttDot}'></span><span class='text-sm'>${state.mqttConnected ? "MQTT Connected" : "MQTT Offline"}</span></div>`;
  h += `</div>`;

  // Fill level hero
  h += `<div class='card' style='text-align:center;margin-bottom:16px'>`;
  h += `<div class='flex items-center justify-between'><h2>Fill Level</h2><span id='fill-badge' class='badge ${fillPct >= 90 ? "badge-danger" : fillPct >= 70 ? "badge-warning" : fillPct >= 50 ? "badge-warning" : "badge-success"}'>${fillPct >= 90 ? "Critical" : fillPct >= 70 ? "High" : fillPct >= 50 ? "Medium" : "Low"}</span></div>`;
  h += `<div id='fill-color' style='font-size:48px;font-weight:700;color:${fillCol};margin:12px 0'>`;
  h += `<span id='fill-pct'>${state.fillPercent >= 0 ? state.fillPercent.toFixed(1) : "N/A"}</span><span class='card-unit'>%</span></div>`;
  h += `<span class='text-sm text-muted'>Distance: ${state.distanceCm >= 0 ? state.distanceCm.toFixed(1) + " cm" : "N/A"}</span>`;
  h += `<div class='bar-bg'><div id='fill-bar' class='bar-fill' style='width:${fillPct}%;background:${fillCol}'></div></div>`;
  h += `</div>`;

  // Stats grid
  h += `<div class='hmi-grid'>`;

  if (HAS_IR) {
    const lidDot = state.lidClosed ? "online" : "offline";
    const lidBadge = state.lidClosed
      ? "<span class='badge badge-success'>Closed</span>"
      : "<span class='badge badge-warning'>Open</span>";
    h += `<div class='card'><div class='card-label'>Lid Status</div><div class='card-value' style='font-size:18px'><span class='status-dot ${lidDot}'></span>${state.lidClosed ? "Closed" : "Open"}</div><div class='text-sm'>${lidBadge}</div></div>`;
  }

  h += `<div class='card ${battClass(state.batteryVoltage)}'><div class='card-label'>Battery</div><div class='card-value'>${state.batteryVoltage.toFixed(2)}<span class='card-unit'>V</span></div><div class='text-sm text-muted'>${state.batteryPercent}% remaining</div></div>`;
  h += `<div class='card stat-info'><div class='card-label'>WiFi RSSI</div><div class='card-value'>${state.rssi}<span class='card-unit'>dBm</span></div><div class='text-sm'><span class='status-dot ${wifiDot}'></span>${state.wifiConnected ? "Connected" : "Disconnected"}</div></div>`;
  h += `<div class='card'><div class='card-label'>Publishes</div><div class='card-value'>${state.publishCount}</div><div class='text-sm text-muted'>Total sent</div></div>`;
  h += `<div class='card'><div class='card-label'>Last Publish</div><div class='card-value' style='font-size:18px'>${lastPubAgo}</div><div class='text-sm text-muted'>Next in 60s</div></div>`;
  h += `</div>`;

  // ── Manual Control (fill level slider) ──
  const mqttStatus = state.mqttConnected ? "Connected" : "Disconnected";
  h += `<div class='card' id='sim-ctrl'>`;
  h += `<div class='flex items-center justify-between mb-4'><h2>Manual Fill Control</h2>`;
  h += `<span class='badge ${state.mqttConnected ? "badge-success" : "badge-warning"}'>MQTT ${mqttStatus}</span></div>`;
  h += `<div id='threshold-banner' style='display:none;padding:10px 16px;border-radius:6px;border:1px solid;margin-bottom:12px;font-size:13px;font-weight:600'></div>`;
  h += `<label style='display:flex;justify-content:space-between;font-size:13px;color:var(--slate-600);margin-bottom:6px'>`;
  h += `<span>Fill Level</span>`;
  h += `<span id='slider-value' style='font-weight:700;color:var(--slate-900)'>${fillPct.toFixed(1)}%</span>`;
  h += `</label>`;
  h += `<input type='range' id='fill-slider' min='0' max='100' step='0.5' value='${fillPct.toFixed(1)}'`;
  h += ` style='width:100%;height:8px;cursor:pointer;accent-color:var(--primary-500)'>`;
  h += `<div style='display:flex;justify-content:space-between;font-size:11px;color:var(--slate-400);margin-top:4px'>`;
  h += `<span>0% Empty</span><span style='color:#d97706'>80% Alert</span><span style='color:#dc2626'>95%+ Full</span><span>100%</span></div>`;
  h += `<div class='row' style='margin-top:12px;align-items:center'>`;
  h += `<span id='mqtt-status' style='font-size:12px;transition:color .3s'></span>`;
  h += `<form method='POST' action='/api/force-publish' style='margin-left:auto'><button class='btn btn-outline' type='submit' style='font-size:12px'>Force Publish</button></form>`;
  h += `</div>`;
  h += `<div style='margin-top:12px;padding:10px 14px;background:var(--slate-100);border-radius:6px;font-size:12px;color:var(--slate-600)'>`;
  h += `<strong>How it works:</strong> Drag the slider → telemetry is published to MQTT <code>${state.mqttTopic}${state.deviceCode}</code> → `;
  h += `backend processes it → SSE fires to mobile app → push notification triggers based on your role (admin / dispatcher).`;
  h += `</div></div>`;

  // Actions
  h += `<div class='card'><h2>Quick Actions</h2>`;
  h += `<div class='row'>`;
  h += `<form method='POST' action='/api/read-sensors'><button class='btn btn-outline' type='submit'>Refresh Sensors</button></form>`;
  h += `</div></div>`;

  h += `<script>
(function(){
  function fillColor(p){return p>=90?'#dc2626':p>=70?'#d97706':p>=50?'#ca8a04':'#16a34a';}
  function updateFillDisplay(pct){
    const col=fillColor(pct);
    const pctEl=document.getElementById('fill-pct');
    if(pctEl){pctEl.textContent=pct.toFixed(1);pctEl.style.color=col;}
    const colorEl=document.getElementById('fill-color');
    if(colorEl) colorEl.style.color=col;
    const bar=document.getElementById('fill-bar');
    if(bar){bar.style.width=pct+'%';bar.style.background=col;}
    const badge=document.getElementById('fill-badge');
    if(badge){
      if(pct>=90){badge.className='badge badge-danger';badge.textContent='Critical';}
      else if(pct>=70){badge.className='badge badge-warning';badge.textContent='High';}
      else if(pct>=50){badge.className='badge badge-warning';badge.textContent='Medium';}
      else{badge.className='badge badge-success';badge.textContent='Low';}
    }
    const banner=document.getElementById('threshold-banner');
    if(banner){
      if(pct>=95){
        banner.style.display='block';banner.style.background='#fee2e2';banner.style.color='#991b1b';banner.style.borderColor='#fecaca';
        banner.innerHTML='🔴 <strong>FULL (≥95%)</strong> — Admin: "Bin Full" notification fired &nbsp;|&nbsp; Dispatcher: "Route is Ready" notification fired';
      } else if(pct>=80){
        banner.style.display='block';banner.style.background='#fef3c7';banner.style.color='#92400e';banner.style.borderColor='#fde68a';
        banner.innerHTML='🟠 <strong>Almost Full (≥80%)</strong> — Admin: "Bin Almost Full" notification fired &nbsp;|&nbsp; Dispatcher: bins queued in Route';
      } else {
        banner.style.display='none';
      }
    }
  }
  var slider=document.getElementById('fill-slider');
  var publishTimer=null;
  if(slider){
    slider.addEventListener('input',function(){
      var pct=parseFloat(this.value);
      document.getElementById('slider-value').textContent=pct.toFixed(1)+'%';
      updateFillDisplay(pct);
      clearTimeout(publishTimer);
      publishTimer=setTimeout(async function(){
        var statusEl=document.getElementById('mqtt-status');
        try{
          var res=await fetch('/api/set-fill',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:'fill='+pct});
          var data=await res.json();
          if(statusEl){
            statusEl.textContent=data.published?'✅ Published to MQTT':'⚠ Saved (MQTT offline)';
            statusEl.style.color=data.published?'#16a34a':'#d97706';
            setTimeout(function(){statusEl.textContent='';},3000);
          }
        }catch(e){
          if(statusEl){statusEl.textContent='❌ Publish failed';statusEl.style.color='#dc2626';}
        }
      },300);
    });
    updateFillDisplay(parseFloat(slider.value));
  }
})();
</script>`;
  h += `</div></body></html>`;
  return h;
}

function handleConfig(msg = ""): string {
  let h = htmlHead("Configuration") + htmlNav("config");
  h += `<div class='container'>${SIM_BANNER}${msg}`;

  h += `<form method='POST' action='/api/save-config'>`;
  h += `<div class='card'><h2>Device</h2>`;
  h += `<label>Device Code</label><input type='text' name='deviceCode' value='${state.deviceCode}'>`;
  h += `</div>`;

  h += `<div class='card'><h2>WiFi Station</h2>`;
  h += `<label>SSID</label><input type='text' name='wifiSsid' value='${state.wifiSsid}'>`;
  h += `<label>Password</label><input type='password' name='wifiPass' value='${state.wifiPass}'>`;
  h += `</div>`;

  h += `<div class='card'><h2>Sensor</h2>`;
  h += `<label>Bin Height (cm)</label><input type='number' name='binHeight' value='${state.binHeightCm.toFixed(1)}' step='0.1'>`;
  h += `<label>Sensor Offset (cm)</label><input type='number' name='sensorOffset' value='${state.sensorOffsetCm.toFixed(1)}' step='0.1'>`;
  h += `</div>`;

  h += `<button class='btn btn-primary' type='submit'>Save Configuration</button>`;
  h += `</form>`;

  h += `<form method='POST' action='/api/factory-reset' class='mt'>`;
  h += `<button class='btn btn-red' type='submit' onclick="return confirm('Reset ALL settings?')">Factory Reset</button>`;
  h += `</form>`;

  h += `</div></body></html>`;
  return h;
}

function handleMqtt(msg = ""): string {
  let h = htmlHead("MQTT Settings") + htmlNav("mqtt");
  h += `<div class='container'>${SIM_BANNER}${msg}`;

  h += `<form method='POST' action='/api/save-mqtt'>`;
  h += `<div class='card'><h2>MQTT Broker</h2>`;
  h += `<label>Broker Host</label><input type='text' name='mqttBroker' value='${state.mqttBroker}'>`;
  h += `<label>Port</label><input type='number' name='mqttPort' value='${state.mqttPort}'>`;
  h += `<label>Username</label><input type='text' name='mqttUser' value='${state.mqttUser}'>`;
  h += `<label>Password</label><input type='password' name='mqttPass' value='${state.mqttPass}'>`;
  h += `<label>Topic Prefix</label><input type='text' name='mqttTopic' value='${state.mqttTopic}'>`;
  h += `<div class='text-sm text-muted mt-4'>Full topic: ${state.mqttTopic}${state.deviceCode}</div>`;
  h += `</div>`;
  h += `<div class='row'>`;
  h += `<button class='btn btn-primary' type='submit'>Save MQTT</button>`;
  h += `</div></form>`;

  h += `<div class='row'>`;
  h += `<form method='POST' action='/api/test-mqtt'><button class='btn btn-blue' type='submit'>Test Connection</button></form>`;
  h += `<form method='POST' action='/api/force-publish'><button class='btn btn-orange' type='submit'>Force Publish</button></form>`;
  h += `</div>`;

  h += `</div></body></html>`;
  return h;
}

function handleCalibrate(msg = ""): string {
  let h = htmlHead("Calibration") + htmlNav("cal");
  h += `<div class='container'>${SIM_BANNER}${msg}`;

  h += `<div class='card'><h2>Current Calibration</h2>`;
  h += `<div class='hmi-grid'>`;
  h += `<div class='card'><div class='card-label'>Bin Height</div><div class='card-value'>${state.binHeightCm.toFixed(1)}<span class='card-unit'>cm</span></div></div>`;
  h += `<div class='card'><div class='card-label'>Sensor Offset</div><div class='card-value'>${state.sensorOffsetCm.toFixed(1)}<span class='card-unit'>cm</span></div></div>`;
  h += `<div class='card stat-info'><div class='card-label'>Distance</div><div class='card-value'>${state.distanceCm >= 0 ? state.distanceCm.toFixed(1) : "N/A"}<span class='card-unit'>cm</span></div></div>`;
  h += `<div class='card'><div class='card-label'>Fill</div><div class='card-value'>${state.fillPercent >= 0 ? state.fillPercent.toFixed(1) + "%" : "N/A"}</div></div>`;
  h += `</div></div>`;

  h += `<div class='card'><h2>Auto Calibrate</h2>`;
  h += `<p class='text-sm text-muted mb-4'>Place sensor at top of EMPTY bin, then calibrate. Takes 10 readings.</p>`;
  h += `<form method='POST' action='/api/calibrate'><button class='btn btn-primary' type='submit'>Start Calibration</button></form></div>`;

  h += `<div class='card'><h2>Manual Calibrate</h2>`;
  h += `<form method='POST' action='/api/save-config'>`;
  h += `<label>Bin Height (cm)</label><input type='number' name='binHeight' value='${state.binHeightCm.toFixed(1)}' step='0.1'>`;
  h += `<label>Sensor Offset (cm)</label><input type='number' name='sensorOffset' value='${state.sensorOffsetCm.toFixed(1)}' step='0.1'>`;
  h += `<input type='hidden' name='deviceCode' value='${state.deviceCode}'>`;
  h += `<input type='hidden' name='wifiSsid' value='${state.wifiSsid}'>`;
  h += `<input type='hidden' name='wifiPass' value='${state.wifiPass}'>`;
  h += `<button class='btn btn-blue mt' type='submit'>Save Manual Values</button>`;
  h += `</form></div>`;

  h += `</div></body></html>`;
  return h;
}

function handlePortalSecurity(msg = ""): string {
  let h = htmlHead("Portal Security") + htmlNav("portal");
  h += `<div class='container'>${SIM_BANNER}${msg}`;

  h += `<form method='POST' action='/api/save-portal'>`;
  h += `<div class='card'><h2>Portal Login Credentials</h2>`;
  h += `<p class='text-sm text-muted mb-4'>Change the username and password for this portal.</p>`;
  h += `<label>Username</label><input type='text' name='portalUser' value='${state.portalUser}'>`;
  h += `<label>New Password</label><input type='password' name='portalPass' placeholder='Enter new password'>`;
  h += `<label>Confirm Password</label><input type='password' name='portalPassConfirm' placeholder='Confirm new password'>`;
  h += `</div>`;
  h += `<button class='btn btn-primary' type='submit'>Update Credentials</button>`;
  h += `</form>`;

  h += `<div class='card mt'><h2>Access Point Info</h2>`;
  h += `<div class='hmi-grid'>`;
  h += `<div class='card stat-info'><div class='card-label'>AP SSID</div><div class='card-value' style='font-size:16px'>EcoRoute-${state.deviceCode}</div></div>`;
  h += `<div class='card'><div class='card-label'>Portal URL</div><div class='card-value' style='font-size:16px'>localhost:${PORT}</div></div>`;
  h += `</div></div>`;

  h += `</div></body></html>`;
  return h;
}

// ─── MQTT client (publishes telemetry when fill is set manually) ─────────────

import mqtt from "mqtt";
import http from "node:http";
import { URL } from "node:url";

const MQTT_BROKER = process.env.MQTT_BROKER_URL ?? "mqtt://109.123.238.215:1883";

let mqttClient: mqtt.MqttClient | null = null;

async function connectMqtt() {
  try {
    mqttClient = await mqtt.connectAsync(MQTT_BROKER, {
      clientId: `ecoroute-portal-sim-${Date.now()}`,
      clean: true,
      connectTimeout: 5000,
    });
    state.mqttConnected = true;
    console.log(`[mqtt] Connected to ${MQTT_BROKER}`);
    mqttClient.on("disconnect", () => { state.mqttConnected = false; });
    mqttClient.on("close", () => { state.mqttConnected = false; });
    mqttClient.on("reconnect", () => { state.mqttConnected = mqttClient?.connected ?? false; });
  } catch (e: unknown) {
    const msg = e instanceof Error ? e.message : String(e);
    state.mqttConnected = false;
    console.warn(`[mqtt] Could not connect (${msg}) — telemetry publish disabled`);
  }
}

async function publishTelemetry() {
  if (!mqttClient?.connected) return;
  const distanceCm = (state.binHeightCm - state.sensorOffsetCm) * (1 - state.fillPercent / 100) + state.sensorOffsetCm;
  const payload = {
    device_code: state.deviceCode,
    fill_level_percent: Math.round(state.fillPercent * 10) / 10,
    distance_cm: Math.round(distanceCm * 10) / 10,
    battery_voltage: Math.round(state.batteryVoltage * 100) / 100,
    signal_strength: state.rssi,
    anomaly_flag: false,
    firmware_version: FIRMWARE_VERSION,
  };
  const topic = `${state.mqttTopic}${state.deviceCode}`;
  try {
    await mqttClient.publishAsync(topic, JSON.stringify(payload), { qos: 1 });
    state.publishCount++;
    state.lastPublishTime = Date.now();
    console.log(`[mqtt] Published fill=${payload.fill_level_percent}% → ${topic}`);
  } catch (e: unknown) {
    const msg = e instanceof Error ? e.message : String(e);
    console.warn(`[mqtt] Publish failed: ${msg}`);
  }
}

// Connect to MQTT broker on startup (non-blocking)
connectMqtt();

function parseFormBody(body: string): Record<string, string> {
  const params: Record<string, string> = {};
  for (const pair of body.split("&")) {
    const [k, v] = pair.split("=");
    if (k && v !== undefined) params[decodeURIComponent(k)] = decodeURIComponent(v.replace(/\+/g, " "));
  }
  return params;
}

function readBody(req: http.IncomingMessage): Promise<string> {
  return new Promise((resolve) => {
    let body = "";
    req.on("data", (chunk: Buffer) => { body += chunk.toString(); });
    req.on("end", () => resolve(body));
  });
}

function sendHtml(res: http.ServerResponse, html: string, status = 200) {
  res.writeHead(status, { "Content-Type": "text/html; charset=utf-8" });
  res.end(html);
}

function sendJson(res: http.ServerResponse, data: object) {
  res.writeHead(200, { "Content-Type": "application/json" });
  res.end(JSON.stringify(data));
}

function redirect(res: http.ServerResponse, location: string) {
  res.writeHead(302, { Location: location });
  res.end();
}

function requireAuth(req: http.IncomingMessage, res: http.ServerResponse): boolean {
  const auth = req.headers.authorization;
  if (auth) {
    const encoded = auth.split(" ")[1];
    if (encoded) {
      const decoded = Buffer.from(encoded, "base64").toString();
      const [user, pass] = decoded.split(":");
      if (user === state.portalUser && pass === state.portalPass) return true;
    }
  }
  res.writeHead(401, { "WWW-Authenticate": 'Basic realm="EcoRoute Smart Bin"' });
  res.end("Unauthorized");
  return false;
}

const server = http.createServer(async (req, res) => {
  if (!requireAuth(req, res)) return;

  const url = new URL(req.url || "/", `http://localhost:${PORT}`);
  const path = url.pathname;
  const method = req.method || "GET";

  // GET pages
  if (method === "GET") {
    if (path === "/" || path === "/index.html") return sendHtml(res, handleDashboard());
    if (path === "/config") return sendHtml(res, handleConfig(url.searchParams.has("saved") ? "<div class='msg msg-ok'>Configuration saved!</div>" : ""));
    if (path === "/mqtt") {
      let msg = "";
      if (url.searchParams.has("saved")) msg = "<div class='msg msg-ok'>MQTT settings saved!</div>";
      if (url.searchParams.has("tested")) msg = "<div class='msg msg-ok'>MQTT connection test successful! (simulated)</div>";
      return sendHtml(res, handleMqtt(msg));
    }
    if (path === "/calibrate") return sendHtml(res, handleCalibrate(url.searchParams.has("result") ? "<div class='msg msg-ok'>Calibration complete!</div>" : ""));
    if (path === "/portal") return sendHtml(res, handlePortalSecurity(url.searchParams.has("saved") ? "<div class='msg msg-ok'>Portal credentials updated!</div>" : ""));

    if (path === "/api/status") {
      return sendJson(res, {
        device_code: state.deviceCode,
        firmware: FIRMWARE_VERSION,
        fill_level_percent: state.fillPercent,
        distance_cm: state.distanceCm,
        battery_voltage: state.batteryVoltage,
        battery_percent: state.batteryPercent,
        rssi: state.rssi,
        lid_closed: HAS_IR ? state.lidClosed : null,
        mqtt_connected: state.mqttConnected,
        wifi_connected: state.wifiConnected,
        uptime_sec: Math.floor((Date.now() - state.bootTime) / 1000),
        publish_count: state.publishCount,
        bin_height_cm: state.binHeightCm,
        sensor_offset_cm: state.sensorOffsetCm,
      });
    }
  }

  // POST actions
  if (method === "POST") {
    const body = await readBody(req);
    const form = parseFormBody(body);
    const referer = (req.headers.referer as string) || "/";

    if (path === "/api/set-fill") {
      const fill = parseFloat(form.fill ?? body);
      if (!isNaN(fill)) {
        state.fillPercent = Math.max(0, Math.min(100, fill));
        const effective = state.binHeightCm - state.sensorOffsetCm;
        state.distanceCm = Math.round((effective * (1 - state.fillPercent / 100) + state.sensorOffsetCm) * 10) / 10;
        state.anomaly = false;
        console.log(`[sim] Manual fill set — fill=${state.fillPercent.toFixed(1)}% dist=${state.distanceCm}cm`);
        await publishTelemetry();
      }
      return sendJson(res, {
        fill_level_percent: state.fillPercent,
        distance_cm: state.distanceCm,
        published: mqttClient?.connected ?? false,
      });
    }

    if (path === "/api/force-publish") {
      simulateSensors();
      state.publishCount++;
      state.lastPublishTime = Date.now();
      await publishTelemetry();
      console.log(`[sim] Force publish — fill=${state.fillPercent.toFixed(1)}% batt=${state.batteryVoltage.toFixed(2)}V`);
      return redirect(res, referer);
    }

    if (path === "/api/read-sensors") {
      simulateSensors();
      console.log(`[sim] Sensor refresh — fill=${state.fillPercent.toFixed(1)}%`);
      return redirect(res, "/");
    }

    if (path === "/api/save-config") {
      if (form.deviceCode) state.deviceCode = form.deviceCode;
      if (form.wifiSsid) state.wifiSsid = form.wifiSsid;
      if (form.wifiPass && form.wifiPass.length > 0) state.wifiPass = form.wifiPass;
      if (form.binHeight) state.binHeightCm = parseFloat(form.binHeight);
      if (form.sensorOffset) state.sensorOffsetCm = parseFloat(form.sensorOffset);
      console.log(`[sim] Config saved — device=${state.deviceCode} height=${state.binHeightCm}`);
      return redirect(res, "/config?saved=1");
    }

    if (path === "/api/save-mqtt") {
      if (form.mqttBroker) state.mqttBroker = form.mqttBroker;
      if (form.mqttPort) state.mqttPort = parseInt(form.mqttPort);
      if (form.mqttUser) state.mqttUser = form.mqttUser;
      if (form.mqttPass && form.mqttPass.length > 0) state.mqttPass = form.mqttPass;
      if (form.mqttTopic) state.mqttTopic = form.mqttTopic;
      console.log(`[sim] MQTT config saved — broker=${state.mqttBroker}:${state.mqttPort}`);
      return redirect(res, "/mqtt?saved=1");
    }

    if (path === "/api/test-mqtt") {
      console.log(`[sim] MQTT test — would connect to ${state.mqttBroker}:${state.mqttPort}`);
      return redirect(res, "/mqtt?tested=1");
    }

    if (path === "/api/calibrate") {
      let log = "";
      let sum = 0;
      for (let i = 0; i < 10; i++) {
        const d = state.binHeightCm + (Math.random() * 4 - 2);
        log += `Reading ${i + 1}: ${d.toFixed(1)} cm\n`;
        sum += d;
      }
      state.binHeightCm = Math.round((sum / 10 - state.sensorOffsetCm) * 10) / 10;
      log += `\nCalibration complete!\nBin height: ${state.binHeightCm.toFixed(1)} cm\nSensor offset: ${state.sensorOffsetCm.toFixed(1)} cm`;
      console.log(`[sim] Calibration — height=${state.binHeightCm}`);

      let h = htmlHead("Calibration Result") + htmlNav("cal");
      h += `<div class='container'>${SIM_BANNER}`;
      h += `<div class='card'><h2>Calibration Result</h2><pre>${log}</pre></div>`;
      h += `<a href='/calibrate?result=1' class='btn btn-primary'>Back to Calibration</a>`;
      h += `</div></body></html>`;
      return sendHtml(res, h);
    }

    if (path === "/api/save-portal") {
      const newPass = form.portalPass || "";
      const confirm = form.portalPassConfirm || "";

      if (newPass.length > 0 && newPass !== confirm) {
        let h = htmlHead("Error") + htmlNav("portal");
        h += `<div class='container'><div class='msg msg-err'>Passwords do not match!</div>`;
        h += `<a href='/portal' class='btn btn-blue'>Go Back</a></div></body></html>`;
        return sendHtml(res, h);
      }

      if (form.portalUser && form.portalUser.length > 0) state.portalUser = form.portalUser;
      if (newPass.length > 0) state.portalPass = newPass;
      console.log(`[sim] Portal credentials updated — user=${state.portalUser}`);
      return redirect(res, "/portal?saved=1");
    }

    if (path === "/api/factory-reset") {
      Object.assign(state, {
        deviceCode: "ECO-BIN-001", wifiSsid: "", wifiPass: "",
        mqttBroker: "109.123.238.215", mqttPort: 1883, mqttUser: "ecoroute", mqttPass: "secret",
        mqttTopic: "ecoroute/trash_can/", portalUser: "admin", portalPass: "ecoroute",
        binHeightCm: 100.0, sensorOffsetCm: 2.0,
      });
      console.log("[sim] Factory reset");
      let h = htmlHead("Factory Reset") + htmlNav("config");
      h += `<div class='container'><div class='msg msg-ok'>Factory reset complete. Settings restored to defaults.</div>`;
      h += `<a href='/' class='btn btn-primary'>Go to Dashboard</a></div></body></html>`;
      return sendHtml(res, h);
    }
  }

  // Catchall
  redirect(res, "/");
});

server.listen(PORT, () => {
  console.log(`
╔══════════════════════════════════════════════════╗
║     EcoRoute Captive Portal Simulator            ║
╚══════════════════════════════════════════════════╝

  URL:       http://localhost:${PORT}
  Login:     ${state.portalUser} / ${state.portalPass}
  Device:    ${state.deviceCode}
  IR Sensor: ${HAS_IR ? "Yes (HW-201)" : "No"}
  Firmware:  v${FIRMWARE_VERSION}

  Open http://localhost:${PORT} in your browser.
  Press Ctrl+C to stop.
`);
});
