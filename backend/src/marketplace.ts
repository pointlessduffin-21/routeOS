import { readdirSync, readFileSync } from "fs";
import { join } from "path";
import type { SensorType } from "./types";

// Resolve path: env var (Docker) or relative to backend/src/ (local dev)
const CATALOG_PATH = process.env.CATALOG_PATH ?? join(import.meta.dir, "../../marketplace/sensors");

const catalog = new Map<string, SensorType>();

export function loadCatalog(): void {
  try {
    const files = readdirSync(CATALOG_PATH).filter((f) => f.endsWith(".json"));
    for (const file of files) {
      try {
        const raw = readFileSync(join(CATALOG_PATH, file), "utf-8");
        const sensor = JSON.parse(raw) as SensorType;
        catalog.set(sensor.id, sensor);
      } catch (e) {
        console.warn(`[marketplace] Failed to load ${file}:`, e);
      }
    }
    console.log(`[marketplace] Loaded ${catalog.size} sensor types from ${CATALOG_PATH}`);
  } catch {
    console.warn(`[marketplace] Catalog directory not found at ${CATALOG_PATH} — marketplace will be empty`);
  }
}

export function getAllSensors(): SensorType[] {
  return Array.from(catalog.values()).sort((a, b) => a.name.localeCompare(b.name));
}

export function getSensorById(id: string): SensorType | undefined {
  return catalog.get(id);
}

export function getCategories(): Array<{ name: string; count: number }> {
  const counts = new Map<string, number>();
  for (const s of catalog.values()) {
    counts.set(s.category, (counts.get(s.category) ?? 0) + 1);
  }
  return Array.from(counts.entries())
    .map(([name, count]) => ({ name, count }))
    .sort((a, b) => a.name.localeCompare(b.name));
}

export function searchSensors(query: string): SensorType[] {
  const q = query.toLowerCase();
  return getAllSensors().filter(
    (s) =>
      s.id.includes(q) ||
      s.name.toLowerCase().includes(q) ||
      s.category.toLowerCase().includes(q) ||
      s.description.toLowerCase().includes(q) ||
      s.tags.some((t) => t.toLowerCase().includes(q))
  );
}
