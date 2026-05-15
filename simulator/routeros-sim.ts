#!/usr/bin/env bun
/**
 * routeros-sim.ts
 * Simulates a MikroTik RouterOS device following the full Syntag agent flow:
 *   1. Boot → HTTP POST /api/devices (self-registration)
 *   2. MQTT connect → subscribe to plant/#
 *   3. Heartbeat every 30 s  → plant/{zone}/{machine}/status/heartbeat
 *   4. Telemetry every 5 s   → plant/{zone}/{machine}/{signal}
 *   5. SIGINT / SIGTERM      → HTTP PUT /api/devices/:id (status=offline)
 */

import mqtt from "mqtt";

// ─── Configuration ────────────────────────────────────────────────────────────
const HOST         = process.env.BACKEND_HOST  ?? "backend";
const API_PORT     = process.env.API_PORT       ?? "4003";
const BROKER_HOST  = process.env.BROKER_HOST    ?? "mosquitto";
const BROKER_PORT  = process.env.BROKER_PORT    ?? "1883";
const DEVICE_ID    = process.env.DEVICE_ID      ?? "routeros-core-zone-a";
const DEVICE_NAME  = process.env.DEVICE_NAME    ?? "RouterOS Core Switch";
const ZONE         = process.env.DEVICE_ZONE    ?? "Zone-A";
const MACHINE      = process.env.DEVICE_MACHINE ?? "core-switch";
const FIRMWARE     = process.env.FIRMWARE_VER   ?? "RouterOS 7.15.3";

const API_BASE    = `http://${HOST}:${API_PORT}/api`;
const BROKER_URL  = `mqtt://${BROKER_HOST}:${BROKER_PORT}`;

// ─── Runtime state ────────────────────────────────────────────────────────────
let cpuLoad     = 15;
let freeMemKB   = 204800;
let uptimeSecs  = 0;
let rxErrors    = 0;
let txErrors    = 0;
let registered  = false;

// ─── Helpers ──────────────────────────────────────────────────────────────────
function log(tag: string, msg: string) {
  const ts = new Date().toISOString();
  console.log(`[${ts}] [${tag}] ${msg}`);
}

function ts() { return new Date().toISOString(); }

/** Drift simulated metrics to look realistic */
function tickMetrics() {
  uptimeSecs += 5;
  cpuLoad = Math.max(1, Math.min(99, cpuLoad + (Math.random() * 10 - 5)));
  freeMemKB = Math.max(8192, Math.min(256000, freeMemKB + (Math.random() * 2048 - 1024)));
  if (Math.random() < 0.02) rxErrors++;
  if (Math.random() < 0.01) txErrors++;
}

// ─── 1. Self-registration ─────────────────────────────────────────────────────
async function register(): Promise<void> {
  const url  = `${API_BASE}/devices`;
  const body = JSON.stringify({
    id: DEVICE_ID,
    name: DEVICE_NAME,
    zone: ZONE,
    machine: MACHINE,
    firmware: FIRMWARE,
    status: "online",
    configuredMetrics: ["cpu_load", "free_memory", "uptime_seconds", "interface_errors"],
    sensorTypes: ["routeros-system", "routeros-interfaces"],
  });

  try {
    const res = await fetch(url, {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body,
    });
    const data = await res.json() as { data?: { id: string }; error?: string };
    if (res.ok) {
      log("register", `✓ Device registered — id=${data.data?.id ?? DEVICE_ID}`);
      registered = true;
    } else {
      log("register", `✗ HTTP ${res.status}: ${data.error ?? "unknown error"}`);
    }
  } catch (e) {
    log("register", `✗ Connection refused (${url}) — will retry in 10 s`);
    await new Promise((r) => setTimeout(r, 10_000));
    return register();
  }
}

// ─── 6. Offline notice ───────────────────────────────────────────────────────
async function goOffline(): Promise<void> {
  if (!registered) return;
  try {
    await fetch(`${API_BASE}/devices/${DEVICE_ID}`, {
      method: "PUT",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify({ status: "offline" }),
    });
    log("offline", "✓ Status set to offline");
  } catch {
    log("offline", "✗ Could not notify backend (already down?)");
  }
}

// ─── MQTT + publisher loops ───────────────────────────────────────────────────
async function startMqtt(): Promise<void> {
  const client = mqtt.connect(BROKER_URL, {
    clientId: DEVICE_ID,
    keepalive: 60,
    reconnectPeriod: 5000,
    connectTimeout: 10_000,
  });

  client.on("connect", () => {
    log("mqtt", `✓ Connected to ${BROKER_URL}`);

    // ── Heartbeat every 30 s ──────────────────────────────────────────────
    const heartbeatTopic = `plant/${ZONE}/${MACHINE}/status/heartbeat`;
    function publishHeartbeat() {
      const payload = JSON.stringify({ value: true, unit: "", ts: ts(), quality: "good" });
      client.publish(heartbeatTopic, payload, { qos: 0 }, (err) => {
        if (err) log("heartbeat", `✗ publish failed: ${err.message}`);
        else     log("heartbeat", `→ ${heartbeatTopic}`);
      });
    }
    publishHeartbeat();
    setInterval(publishHeartbeat, 30_000);

    // ── Telemetry every 5 s ───────────────────────────────────────────────
    function publishTelemetry() {
      tickMetrics();
      const base = `plant/${ZONE}/${MACHINE}`;

      const signals: Array<[string, unknown, string]> = [
        ["cpu_load",         +cpuLoad.toFixed(1),             "%"],
        ["free_memory",      Math.round(freeMemKB),            "KB"],
        ["uptime_seconds",   uptimeSecs,                       "s"],
        ["interface_errors", { rx_errors: rxErrors, tx_errors: txErrors, ts: ts(), quality: "good" }, ""],
      ];

      for (const [signal, value, unit] of signals) {
        const payload = signal === "interface_errors"
          ? JSON.stringify(value)
          : JSON.stringify({ value, unit, ts: ts(), quality: "good" });

        client.publish(`${base}/${signal}`, payload, { qos: 1 });
      }
      log("telemetry", `cpu=${cpuLoad.toFixed(1)}%  mem=${Math.round(freeMemKB)}KB  up=${uptimeSecs}s  rxErr=${rxErrors}`);
    }
    publishTelemetry();
    setInterval(publishTelemetry, 5_000);
  });

  client.on("error",       (e) => log("mqtt", `✗ ${e.message}`));
  client.on("reconnect",   ()  => log("mqtt", "↺ reconnecting…"));
  client.on("offline",     ()  => log("mqtt", "⚠ broker offline"));

  // Graceful shutdown
  async function shutdown(signal: string) {
    log("shutdown", `${signal} received`);
    await goOffline();
    client.end(true, () => process.exit(0));
  }
  process.on("SIGINT",  () => void shutdown("SIGINT"));
  process.on("SIGTERM", () => void shutdown("SIGTERM"));
}

// ─── Main ─────────────────────────────────────────────────────────────────────
log("boot", `routeOS RouterOS simulator  device=${DEVICE_ID}  zone=${ZONE}  machine=${MACHINE}`);
log("boot", `Backend → ${API_BASE}   Broker → ${BROKER_URL}`);

await register();
await startMqtt();
