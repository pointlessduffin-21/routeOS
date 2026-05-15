import { Database } from "bun:sqlite";
import { join } from "path";

const DB_PATH = process.env.DB_PATH ?? join(import.meta.dir, "../../routeos.db");

export const db = new Database(DB_PATH, { create: true });

db.exec(`
  PRAGMA journal_mode = WAL;
  PRAGMA foreign_keys = ON;

  CREATE TABLE IF NOT EXISTS devices (
    id               TEXT PRIMARY KEY,
    name             TEXT NOT NULL,
    zone             TEXT NOT NULL DEFAULT '',
    machine          TEXT NOT NULL DEFAULT '',
    firmware         TEXT NOT NULL DEFAULT '',
    status           TEXT NOT NULL DEFAULT 'offline',
    configured_metrics TEXT NOT NULL DEFAULT '[]',
    sensor_types     TEXT NOT NULL DEFAULT '[]',
    ip_address       TEXT,
    last_seen        TEXT,
    registered_at    TEXT NOT NULL DEFAULT (strftime('%Y-%m-%dT%H:%M:%SZ','now')),
    updated_at       TEXT NOT NULL DEFAULT (strftime('%Y-%m-%dT%H:%M:%SZ','now'))
  );

  CREATE TABLE IF NOT EXISTS telemetry (
    id          INTEGER PRIMARY KEY AUTOINCREMENT,
    device_id   TEXT NOT NULL,
    signal      TEXT NOT NULL,
    value       REAL,
    value_str   TEXT,
    unit        TEXT NOT NULL DEFAULT '',
    quality     TEXT NOT NULL DEFAULT 'good',
    ts          TEXT NOT NULL,
    recorded_at TEXT NOT NULL DEFAULT (strftime('%Y-%m-%dT%H:%M:%SZ','now')),
    FOREIGN KEY (device_id) REFERENCES devices(id) ON DELETE CASCADE
  );

  CREATE INDEX IF NOT EXISTS idx_telemetry_device   ON telemetry(device_id);
  CREATE INDEX IF NOT EXISTS idx_telemetry_recorded ON telemetry(recorded_at DESC);
  CREATE INDEX IF NOT EXISTS idx_telemetry_signal   ON telemetry(device_id, signal);
`);

export default db;
