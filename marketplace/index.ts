#!/usr/bin/env bun
/**
 * index.ts — routeOS Sensor Marketplace CLI Browser
 * Reads sensor JSON catalog from ./sensors/ and presents a TUI.
 *
 * Usage:  bun index.ts [search-term]
 *         bun index.ts temperature
 */

import { readdirSync, readFileSync } from "fs";
import { join } from "path";

type SignalType = "float" | "integer" | "boolean" | "string";
interface SensorSignal { name: string; unit: string; type: SignalType; min?: number; max?: number; description: string; alarmHigh?: number; alarmLow?: number; }
interface SensorType { id: string; name: string; category: string; description: string; manufacturer: string; interface: string; icon: string; tags: string[]; signals: SensorSignal[]; mqttTopicPattern: string; gpioMap: Record<string, string> | null; firmwareHint: string | null; routerosHint: string | null; datasheet: string | null; }

const C = {
  reset: "\x1b[0m", bold: "\x1b[1m", dim: "\x1b[2m",
  green: "\x1b[32m", cyan: "\x1b[36m", yellow: "\x1b[33m",
  magenta: "\x1b[35m", gray: "\x1b[90m", blue: "\x1b[34m",
};

const SENSOR_DIR = join(import.meta.dir, "sensors");

function loadAll(): SensorType[] {
  try {
    return readdirSync(SENSOR_DIR)
      .filter((f) => f.endsWith(".json"))
      .map((f) => JSON.parse(readFileSync(join(SENSOR_DIR, f), "utf-8")) as SensorType)
      .sort((a, b) => a.category.localeCompare(b.category) || a.name.localeCompare(b.name));
  } catch {
    console.error(`Cannot read sensor catalog at ${SENSOR_DIR}`);
    return [];
  }
}

function printSensorCard(s: SensorType) {
  const W = process.stdout.columns || 100;
  console.log("\n" + C.bold + C.cyan + "┌─ " + s.name + " " + "─".repeat(Math.max(0, W - s.name.length - 4)) + C.reset);
  console.log(C.gray + "│" + C.reset + ` ${C.dim}id:${C.reset} ${s.id}   ${C.dim}category:${C.reset} ${s.category}   ${C.dim}interface:${C.reset} ${s.interface}`);
  console.log(C.gray + "│" + C.reset + ` ${C.dim}manufacturer:${C.reset} ${s.manufacturer}`);
  console.log(C.gray + "│" + C.reset + ` ${s.description}`);
  console.log(C.gray + "│" + C.reset);
  console.log(C.gray + "│" + C.reset + ` ${C.bold}Signals:${C.reset}`);
  for (const sig of s.signals) {
    const range = sig.min !== undefined && sig.max !== undefined ? ` [${sig.min}–${sig.max}]` : "";
    const alarms = [
      sig.alarmHigh !== undefined ? `⬆${sig.alarmHigh}` : null,
      sig.alarmLow  !== undefined ? `⬇${sig.alarmLow}`  : null,
    ].filter(Boolean).join(" ");
    console.log(
      C.gray + "│" + C.reset +
      `   ${C.green}${sig.name}${C.reset}  ${C.dim}${sig.unit}${range}${C.reset}` +
      (alarms ? `  ${C.yellow}${alarms}${C.reset}` : "") +
      `  — ${sig.description}`
    );
  }

  if (s.gpioMap) {
    console.log(C.gray + "│" + C.reset + ` ${C.bold}GPIO:${C.reset} ` + Object.entries(s.gpioMap).map(([k, v]) => `${k}→${v}`).join("  "));
  }
  console.log(C.gray + "│" + C.reset + ` ${C.bold}MQTT topic:${C.reset} ${C.magenta}${s.mqttTopicPattern}${C.reset}`);
  if (s.firmwareHint)  console.log(C.gray + "│" + C.reset + ` ${C.bold}Firmware:${C.reset} ${s.firmwareHint}`);
  if (s.routerosHint)  console.log(C.gray + "│" + C.reset + ` ${C.bold}RouterOS:${C.reset} ${s.routerosHint}`);
  if (s.tags.length)   console.log(C.gray + "│" + C.reset + ` ${C.dim}tags: ${s.tags.join(", ")}${C.reset}`);
  if (s.datasheet)     console.log(C.gray + "│" + C.reset + ` ${C.blue}datasheet: ${s.datasheet}${C.reset}`);
  console.log(C.bold + C.cyan + "└" + "─".repeat(Math.max(0, W - 1)) + C.reset);
}

function printIndex(sensors: SensorType[]) {
  const W = process.stdout.columns || 100;
  console.log("\n" + C.bold + C.green + "  routeOS Sensor Marketplace" + C.reset);
  console.log(C.gray + "  " + "─".repeat(W - 2) + C.reset);

  const byCategory = new Map<string, SensorType[]>();
  for (const s of sensors) {
    const arr = byCategory.get(s.category) ?? [];
    arr.push(s);
    byCategory.set(s.category, arr);
  }

  for (const [cat, items] of [...byCategory.entries()].sort()) {
    console.log(`\n  ${C.bold}${C.cyan}${cat}${C.reset}`);
    for (const s of items) {
      const signals = s.signals.map((x) => x.name).join(", ");
      console.log(
        `    ${C.green}${s.id.padEnd(24)}${C.reset}` +
        `${s.name.padEnd(30)}` +
        `${C.gray}${signals.slice(0, 50)}${C.reset}`
      );
    }
  }
  console.log(C.gray + "\n  " + `${sensors.length} sensors total  ·  bun index.ts <id|term> for details` + C.reset + "\n");
}

const query = process.argv[2]?.toLowerCase().trim();
const all   = loadAll();

if (!query) {
  printIndex(all);
} else {
  // Try exact id match first, then search
  const exact = all.find((s) => s.id === query);
  if (exact) {
    printSensorCard(exact);
  } else {
    const results = all.filter((s) =>
      s.id.includes(query) ||
      s.name.toLowerCase().includes(query) ||
      s.category.toLowerCase().includes(query) ||
      s.description.toLowerCase().includes(query) ||
      s.tags.some((t) => t.includes(query))
    );
    if (results.length === 0) {
      console.log(`\n${C.yellow}No sensors matching "${query}"${C.reset}\n`);
    } else if (results.length === 1) {
      printSensorCard(results[0]);
    } else {
      console.log(`\n${C.bold}${results.length} results for "${query}":${C.reset}`);
      for (const s of results) printSensorCard(s);
    }
  }
}
