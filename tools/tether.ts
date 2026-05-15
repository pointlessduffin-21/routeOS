#!/usr/bin/env bun
/**
 * tether.ts — Live MQTT subscriber that prints a colour-coded terminal table
 * of all plant/{zone}/{machine}/{signal} messages as they arrive.
 *
 * Usage:  bun tether.ts [broker-url]
 *         BROKER=mqtt://192.168.1.100:1883 bun tether.ts
 */

import mqtt from "mqtt";

const BROKER = process.env.BROKER ?? process.argv[2] ?? "mqtt://localhost:1883";
const TOPIC  = "plant/#";
const WIDTH  = process.stdout.columns || 120;

// ANSI helpers
const C = {
  reset:  "\x1b[0m",
  green:  "\x1b[32m",
  yellow: "\x1b[33m",
  cyan:   "\x1b[36m",
  gray:   "\x1b[90m",
  bold:   "\x1b[1m",
  red:    "\x1b[31m",
};

function header() {
  const title = `  routeOS · Live MQTT Tether — ${BROKER}  `;
  console.clear();
  console.log(C.bold + C.green + "─".repeat(WIDTH) + C.reset);
  console.log(C.bold + C.green + title.padEnd(WIDTH) + C.reset);
  console.log(C.bold + C.green + "─".repeat(WIDTH) + C.reset);
  console.log(
    C.bold +
    " TIMESTAMP".padEnd(28) +
    "TOPIC".padEnd(55) +
    "VALUE".padEnd(30) +
    "QUALITY" +
    C.reset
  );
  console.log(C.gray + "─".repeat(WIDTH) + C.reset);
}

let lineCount = 0;
let lastTopic = "";

header();

const client = mqtt.connect(BROKER, { clientId: "routeos-tether", keepalive: 30 });

client.on("connect", () => {
  console.log(C.green + `[tether] Connected → subscribed to ${TOPIC}` + C.reset + "\n");
  client.subscribe(TOPIC);
});

client.on("message", (topic: string, buf: Buffer) => {
  const raw = buf.toString();
  let value = raw;
  let quality = "";

  try {
    const obj = JSON.parse(raw) as Record<string, unknown>;
    if ("value" in obj) {
      const v = obj.value;
      value = typeof v === "object" ? JSON.stringify(v) : String(v);
      if ("unit" in obj && obj.unit) value += ` ${obj.unit}`;
    } else {
      value = JSON.stringify(obj);
    }
    quality = typeof obj.quality === "string" ? obj.quality : "";
  } catch {
    // not JSON — show raw
  }

  const ts    = new Date().toISOString().replace("T", " ").slice(0, 23);
  const color = quality === "good" ? C.green : quality ? C.yellow : C.cyan;
  const tColor = topic === lastTopic ? C.gray : C.cyan;
  lastTopic = topic;

  const row =
    C.gray  + ` ${ts}  ` + C.reset +
    tColor  + topic.padEnd(53) + "  " + C.reset +
    color   + value.slice(0, 28).padEnd(30) + C.reset +
    C.gray  + quality + C.reset;

  console.log(row);
  lineCount++;

  // Re-draw header every 40 lines for readability
  if (lineCount % 40 === 0) header();
});

client.on("error",     (e: Error)  => console.error(C.red + `[tether] Error: ${e.message}` + C.reset));
client.on("reconnect", ()          => console.log(C.yellow + "[tether] Reconnecting…" + C.reset));
client.on("close",     ()          => console.log(C.gray   + "[tether] Connection closed" + C.reset));

process.on("SIGINT", () => {
  client.end();
  console.log("\n[tether] Disconnected. Bye.");
  process.exit(0);
});
