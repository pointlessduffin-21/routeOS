import { db } from "./db";
import { loadCatalog } from "./marketplace";
import { handleDevices, touchDevice } from "./routes/devices";
import { handleMarketplace } from "./routes/marketplace";
import mqtt from "mqtt";

const PORT = parseInt(process.env.PORT ?? "4003");
const MQTT_URL = process.env.MQTT_URL ?? "mqtt://localhost:1883";

// ─── Load sensor catalog ─────────────────────────────────────────────────────
loadCatalog();

// ─── MQTT subscriber — keeps device lastSeen fresh ───────────────────────────
function startMqttSubscriber(): void {
  const client = mqtt.connect(MQTT_URL, {
    clientId: `routeos-backend-${Date.now()}`,
    reconnectPeriod: 5000,
    connectTimeout: 10000,
  });

  client.on("connect", () => {
    console.log(`[mqtt] Connected to ${MQTT_URL}`);
    client.subscribe("plant/#", { qos: 0 }, (err) => {
      if (err) console.error("[mqtt] Subscribe error:", err.message);
      else console.log("[mqtt] Subscribed to plant/#");
    });
  });

  client.on("message", (topic: string) => {
    // topic format: plant/{zone}/{machine}/{signal}
    // device id is derived from zone+machine
    const parts = topic.split("/");
    if (parts.length >= 4) {
      const zone    = parts[1];
      const machine = parts[2];
      // Try to find the device by zone+machine
      touchDeviceByZoneMachine(zone, machine);
    }
  });

  client.on("error", (err) => {
    console.warn("[mqtt] Error:", err.message);
  });

  client.on("offline", () => {
    console.warn("[mqtt] Broker offline — will retry");
  });
}

function touchDeviceByZoneMachine(zone: string, machine: string): void {
  const row = db
    .query("SELECT id FROM devices WHERE zone = ? AND machine = ?")
    .get(zone, machine) as { id: string } | null;
  if (row) touchDevice(row.id);
}

// ─── HTTP server ─────────────────────────────────────────────────────────────
const CORS = {
  "Access-Control-Allow-Origin": "*",
  "Access-Control-Allow-Methods": "GET, POST, PUT, PATCH, DELETE, OPTIONS",
  "Access-Control-Allow-Headers": "Content-Type, Authorization",
};

const server = Bun.serve({
  port: PORT,
  hostname: "0.0.0.0",

  async fetch(req) {
    const url = new URL(req.url);
    const path = url.pathname;

    if (req.method === "OPTIONS") {
      return new Response(null, { status: 204, headers: CORS });
    }

    if (path === "/api/health") {
      return Response.json(
        { status: "ok", timestamp: new Date().toISOString(), port: PORT },
        { headers: CORS }
      );
    }

    if (path.startsWith("/api/devices")) {
      return handleDevices(req, url, CORS);
    }

    if (path.startsWith("/api/marketplace")) {
      return handleMarketplace(req, url, CORS);
    }

    return Response.json({ error: "Not Found" }, { status: 404, headers: CORS });
  },
});

// Try to connect to MQTT (non-fatal if broker not up yet)
try {
  startMqttSubscriber();
} catch (e) {
  console.warn("[mqtt] Could not start subscriber:", e);
}

console.log("╔══════════════════════════════════════════════════════╗");
console.log("║      routeOS Device Registry & Backend  v1.0.0      ║");
console.log("╚══════════════════════════════════════════════════════╝");
console.log(`  REST API  http://0.0.0.0:${PORT}/api`);
console.log(`  Health    http://0.0.0.0:${PORT}/api/health`);
console.log(`  Devices   http://0.0.0.0:${PORT}/api/devices`);
console.log(`  Catalog   http://0.0.0.0:${PORT}/api/marketplace`);
console.log(`  MQTT sub  ${MQTT_URL}  (plant/#)`);
console.log();
