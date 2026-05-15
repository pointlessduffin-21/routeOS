import db from "../db";
import type { Device, DeviceRow, RegisterDeviceBody, UpdateDeviceBody } from "../types";

function rowToDevice(row: DeviceRow): Device {
  return {
    id: row.id,
    name: row.name,
    zone: row.zone,
    machine: row.machine,
    firmware: row.firmware,
    status: row.status,
    configuredMetrics: JSON.parse(row.configured_metrics ?? "[]"),
    sensorTypes: JSON.parse(row.sensor_types ?? "[]"),
    ipAddress: row.ip_address,
    lastSeen: row.last_seen,
    registeredAt: row.registered_at,
    updatedAt: row.updated_at,
  };
}

const now = () => new Date().toISOString();

export async function handleDevices(
  req: Request,
  url: URL,
  corsHeaders: Record<string, string>
): Promise<Response> {
  const parts = url.pathname.replace(/^\/api\/devices\/?/, "").split("/").filter(Boolean);
  const deviceId = parts[0] ?? null;

  // ─── POST /api/devices — register ───────────────────────────────────────────
  if (!deviceId && req.method === "POST") {
    let body: Partial<RegisterDeviceBody>;
    try {
      body = await req.json();
    } catch {
      return Response.json({ error: "Invalid JSON body" }, { status: 400, headers: corsHeaders });
    }

    if (!body.id || !body.name) {
      return Response.json({ error: "id and name are required" }, { status: 400, headers: corsHeaders });
    }

    const exists = db.query("SELECT id FROM devices WHERE id = ?").get(body.id);
    const ts = now();

    if (exists) {
      // Re-registration — update status to online and refresh metadata
      db.query(`
        UPDATE devices SET
          name               = ?,
          zone               = ?,
          machine            = ?,
          firmware           = ?,
          status             = 'online',
          configured_metrics = ?,
          sensor_types       = ?,
          ip_address         = ?,
          last_seen          = ?,
          updated_at         = ?
        WHERE id = ?
      `).run(
        body.name,
        body.zone ?? "",
        body.machine ?? "",
        body.firmware ?? "",
        JSON.stringify(body.configuredMetrics ?? []),
        JSON.stringify(body.sensorTypes ?? []),
        body.ipAddress ?? null,
        ts, ts,
        body.id
      );
      const updated = db.query("SELECT * FROM devices WHERE id = ?").get(body.id) as DeviceRow;
      console.log(`[devices] Re-registered: ${body.id}`);
      return Response.json(
        { message: "Device re-registered", data: rowToDevice(updated) },
        { status: 200, headers: corsHeaders }
      );
    }

    db.query(`
      INSERT INTO devices (id, name, zone, machine, firmware, status, configured_metrics, sensor_types, ip_address, last_seen, registered_at, updated_at)
      VALUES (?, ?, ?, ?, ?, 'online', ?, ?, ?, ?, ?, ?)
    `).run(
      body.id,
      body.name,
      body.zone ?? "",
      body.machine ?? "",
      body.firmware ?? "",
      JSON.stringify(body.configuredMetrics ?? []),
      JSON.stringify(body.sensorTypes ?? []),
      body.ipAddress ?? null,
      ts, ts, ts
    );

    const created = db.query("SELECT * FROM devices WHERE id = ?").get(body.id) as DeviceRow;
    console.log(`[devices] Registered: ${body.id} (${body.name})`);
    return Response.json(
      { message: "Device registered", data: rowToDevice(created) },
      { status: 201, headers: corsHeaders }
    );
  }

  // ─── GET /api/devices — list all ────────────────────────────────────────────
  if (!deviceId && req.method === "GET") {
    const zone    = url.searchParams.get("zone");
    const status  = url.searchParams.get("status");
    const limit   = parseInt(url.searchParams.get("limit") ?? "100");
    const offset  = parseInt(url.searchParams.get("offset") ?? "0");

    let query = "SELECT * FROM devices";
    const params: unknown[] = [];
    const where: string[] = [];

    if (zone)   { where.push("zone = ?");   params.push(zone); }
    if (status) { where.push("status = ?"); params.push(status); }
    if (where.length) query += " WHERE " + where.join(" AND ");
    query += " ORDER BY registered_at DESC LIMIT ? OFFSET ?";
    params.push(limit, offset);

    const rows = db.query(query).all(...params) as DeviceRow[];
    const total = (db.query("SELECT COUNT(*) as n FROM devices").get() as { n: number }).n;

    return Response.json(
      { data: rows.map(rowToDevice), total, limit, offset },
      { headers: corsHeaders }
    );
  }

  if (!deviceId) {
    return Response.json({ error: "Method not allowed" }, { status: 405, headers: corsHeaders });
  }

  // ─── GET /api/devices/:id ────────────────────────────────────────────────────
  if (req.method === "GET") {
    const row = db.query("SELECT * FROM devices WHERE id = ?").get(deviceId) as DeviceRow | null;
    if (!row) return Response.json({ error: "Device not found" }, { status: 404, headers: corsHeaders });
    return Response.json({ data: rowToDevice(row) }, { headers: corsHeaders });
  }

  // ─── PUT /api/devices/:id — update / heartbeat / offline ────────────────────
  if (req.method === "PUT" || req.method === "PATCH") {
    const row = db.query("SELECT id FROM devices WHERE id = ?").get(deviceId);
    if (!row) return Response.json({ error: "Device not found" }, { status: 404, headers: corsHeaders });

    let body: Partial<UpdateDeviceBody> = {};
    try {
      body = await req.json();
    } catch { /* allow empty body for heartbeat */ }

    const sets: string[] = ["updated_at = ?"];
    const params: unknown[] = [now()];

    if (body.name !== undefined)             { sets.push("name = ?");               params.push(body.name); }
    if (body.zone !== undefined)             { sets.push("zone = ?");               params.push(body.zone); }
    if (body.machine !== undefined)          { sets.push("machine = ?");            params.push(body.machine); }
    if (body.firmware !== undefined)         { sets.push("firmware = ?");           params.push(body.firmware); }
    if (body.status !== undefined)           { sets.push("status = ?");             params.push(body.status); }
    if (body.configuredMetrics !== undefined){ sets.push("configured_metrics = ?"); params.push(JSON.stringify(body.configuredMetrics)); }
    if (body.sensorTypes !== undefined)      { sets.push("sensor_types = ?");       params.push(JSON.stringify(body.sensorTypes)); }
    if (body.ipAddress !== undefined)        { sets.push("ip_address = ?");         params.push(body.ipAddress); }

    params.push(deviceId);
    db.query(`UPDATE devices SET ${sets.join(", ")} WHERE id = ?`).run(...params);

    const updated = db.query("SELECT * FROM devices WHERE id = ?").get(deviceId) as DeviceRow;
    if (body.status) console.log(`[devices] ${deviceId} → ${body.status}`);
    return Response.json({ message: "Device updated", data: rowToDevice(updated) }, { headers: corsHeaders });
  }

  // ─── DELETE /api/devices/:id ─────────────────────────────────────────────────
  if (req.method === "DELETE") {
    const row = db.query("SELECT id FROM devices WHERE id = ?").get(deviceId);
    if (!row) return Response.json({ error: "Device not found" }, { status: 404, headers: corsHeaders });
    db.query("DELETE FROM devices WHERE id = ?").run(deviceId);
    console.log(`[devices] Deleted: ${deviceId}`);
    return Response.json({ message: "Device removed" }, { headers: corsHeaders });
  }

  return Response.json({ error: "Method not allowed" }, { status: 405, headers: corsHeaders });
}

// Called by the MQTT subscriber to keep lastSeen fresh
export function touchDevice(deviceId: string): void {
  const ts = now();
  db.query(`
    UPDATE devices SET last_seen = ?, status = 'online', updated_at = ?
    WHERE id = ?
  `).run(ts, ts, deviceId);
}
