#!/usr/bin/env bun
/**
 * monitor.ts — Polls the routeOS backend and renders a live fleet status table.
 *
 * Usage:  bun monitor.ts [backend-url]
 *         BACKEND=http://192.168.1.100:4003 bun monitor.ts
 */

import type { Device } from "../backend/src/types";

const BACKEND     = process.env.BACKEND ?? process.argv[2] ?? "http://localhost:4003";
const POLL_MS     = Number(process.env.POLL_MS ?? "3000");
const WIDTH       = process.stdout.columns || 140;

const C = {
  reset:  "\x1b[0m",
  green:  "\x1b[32m",
  yellow: "\x1b[33m",
  red:    "\x1b[31m",
  cyan:   "\x1b[36m",
  gray:   "\x1b[90m",
  bold:   "\x1b[1m",
};

function statusColor(s: string) {
  if (s === "online")   return C.green;
  if (s === "degraded") return C.yellow;
  return C.red;
}

function timeSince(iso: string | null): string {
  if (!iso) return "never";
  const d = Date.now() - new Date(iso).getTime();
  if (d < 60_000)   return `${Math.round(d / 1000)}s ago`;
  if (d < 3_600_000) return `${Math.round(d / 60_000)}m ago`;
  return `${Math.round(d / 3_600_000)}h ago`;
}

function render(devices: Device[]) {
  console.clear();
  const now = new Date().toISOString().replace("T", " ").slice(0, 19);
  const title = `  routeOS Fleet Monitor  ·  ${BACKEND}  ·  ${now}  (${devices.length} devices)  `;
  console.log(C.bold + C.cyan + "─".repeat(WIDTH) + C.reset);
  console.log(C.bold + C.cyan + title.padEnd(WIDTH) + C.reset);
  console.log(C.bold + C.cyan + "─".repeat(WIDTH) + C.reset);

  if (devices.length === 0) {
    console.log(C.gray + "\n  No devices registered yet.\n" + C.reset);
    return;
  }

  const cols = [
    ["STATUS", 10], ["DEVICE ID", 28], ["NAME", 22], ["ZONE", 12],
    ["MACHINE", 16], ["FIRMWARE", 16], ["LAST SEEN", 12],
  ] as const;

  console.log(
    C.bold +
    cols.map(([h, w]) => h.padEnd(w as number)).join("") +
    C.reset
  );
  console.log(C.gray + "─".repeat(WIDTH) + C.reset);

  for (const d of devices) {
    const sc = statusColor(d.status);
    console.log(
      sc      + d.status.padEnd(10)           + C.reset +
      C.cyan  + d.id.slice(0, 26).padEnd(28)  + C.reset +
               d.name.slice(0, 20).padEnd(22) +
      C.gray  + d.zone.padEnd(12)             + C.reset +
               d.machine.padEnd(16)           +
      C.gray  + d.firmware.slice(0, 14).padEnd(16) + C.reset +
               timeSince(d.lastSeen).padEnd(12)
    );
  }
  console.log(C.gray + "─".repeat(WIDTH) + C.reset);
  console.log(C.gray + `  Refreshing every ${POLL_MS / 1000}s  ·  Ctrl-C to exit` + C.reset);
}

async function poll() {
  try {
    const res  = await fetch(`${BACKEND}/api/devices`);
    if (!res.ok) throw new Error(`HTTP ${res.status}`);
    const json = await res.json() as { data: Device[] };
    render(json.data ?? []);
  } catch (e) {
    const err = e instanceof Error ? e.message : String(e);
    console.clear();
    console.log(C.red + `[monitor] Cannot reach backend at ${BACKEND}: ${err}` + C.reset);
    console.log(C.gray + "Retrying…" + C.reset);
  }
}

console.log(`[monitor] Starting — polling ${BACKEND} every ${POLL_MS}ms`);
await poll();
const timer = setInterval(poll, POLL_MS);

process.on("SIGINT", () => {
  clearInterval(timer);
  console.log("\n[monitor] Bye.");
  process.exit(0);
});
