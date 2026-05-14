/*
 * EcoRoute Firmware Simulator
 *
 * Simulates 7 virtual smart bins (ECO-BIN-1002 through ECO-BIN-1008)
 * publishing MQTT telemetry to the broker at 109.123.238.215:1883.
 * ECO-BIN-1001 is excluded because it runs on a physical ESP32.
 *
 * Topic format: ecoroute/trash_can/<device_code>
 *
 * Usage:
 *   bun run firmware/simulator/simulate.ts
 *   bun run firmware/simulator/simulate.ts --interval 10
 *   bun run firmware/simulator/simulate.ts --once
 *
 * Environment:
 *   MQTT_BROKER_URL   MQTT broker (default: mqtt://109.123.238.215:1883)
 */

import mqtt from "mqtt";

// ─── Configuration ───────────────────────────────────────────────────────────

const BROKER_URL  = process.env.MQTT_BROKER_URL ?? "mqtt://109.123.238.215:1883";
const TOPIC_PREFIX = "ecoroute/trash_can/";

const args = process.argv.slice(2);
function getArg(name: string, fallback: string): string {
  const idx = args.indexOf(`--${name}`);
  return idx >= 0 && args[idx + 1] ? args[idx + 1] : fallback;
}
const REPORT_INTERVAL_SEC = parseInt(getArg("interval", "30"));
const ONCE = args.includes("--once");

// ECO-BIN-1001 is the physical ESP32 — skip it in simulation
const PHYSICAL_DEVICE = "ECO-BIN-1001";

// ─── Simulated bin state ─────────────────────────────────────────────────────

interface SimBin {
  deviceCode: string;
  binHeightCm: number;
  fillPercent: number;
  fillRate: number;
  batteryVoltage: number;
  signalStrength: number;
}

const BIN_PROFILES: Array<{ code: string; height: number; startFill: number; rate: number; rssi: number }> = [
  // ECO-BIN-1001 is physical — not listed here
  { code: "ECO-BIN-1002", height: 100, startFill: 35, rate: 2.5, rssi: -58 },  // moderate area
  { code: "ECO-BIN-1003", height:  90, startFill:  5, rate: 1.0, rssi: -72 },  // quiet residential
  { code: "ECO-BIN-1004", height: 100, startFill: 40, rate: 2.0, rssi: -65 },  // park area
  { code: "ECO-BIN-1005", height: 120, startFill: 68, rate: 4.5, rssi: -55 },  // busy commercial
  { code: "ECO-BIN-1006", height:  80, startFill: 25, rate: 3.0, rssi: -70 },  // school zone
  { code: "ECO-BIN-1007", height: 100, startFill: 38, rate: 2.8, rssi: -62 },  // market street
  { code: "ECO-BIN-1008", height: 110, startFill: 60, rate: 1.5, rssi: -48 },  // near gateway
];

function createBins(): SimBin[] {
  return BIN_PROFILES.map((p) => ({
    deviceCode: p.code,
    binHeightCm: p.height,
    fillPercent: p.startFill + (Math.random() * 10 - 5),
    fillRate: p.rate,
    batteryVoltage: 5.0,
    signalStrength: p.rssi,
  }));
}

// ─── Simulation logic ────────────────────────────────────────────────────────

function tickBin(bin: SimBin): void {
  bin.fillPercent += bin.fillRate * (0.7 + Math.random() * 0.6);

  if (bin.fillPercent >= 95 && Math.random() < 0.3) {
    bin.fillPercent = 2 + Math.random() * 8;
    console.log(`  🗑️  ${bin.deviceCode} was collected! Reset to ${bin.fillPercent.toFixed(1)}%`);
  }

  bin.fillPercent = Math.min(100, Math.max(0, bin.fillPercent));

  bin.signalStrength += Math.floor(Math.random() * 7) - 3;
  bin.signalStrength = Math.max(-90, Math.min(-40, bin.signalStrength));
}

function buildPayload(bin: SimBin) {
  const distanceCm = bin.binHeightCm * (1 - bin.fillPercent / 100);
  return {
    device_code:         bin.deviceCode,
    fill_level_percent:  Math.round(bin.fillPercent * 10) / 10,
    distance_cm:         Math.round(distanceCm * 10) / 10,
    battery_voltage:     Math.round(bin.batteryVoltage * 100) / 100,
    signal_strength:     bin.signalStrength,
    anomaly_flag:        distanceCm < 0 || distanceCm > 300,
    firmware_version:    "1.0.0",
  };
}

// ─── MQTT publish ─────────────────────────────────────────────────────────────

async function reportAll(client: mqtt.MqttClient, bins: SimBin[]): Promise<void> {
  const timestamp = new Date().toLocaleTimeString();
  console.log(`\n[${timestamp}] ─── Reporting cycle ───────────────────────`);

  for (const bin of bins) {
    tickBin(bin);
    const payload = buildPayload(bin);
    const topic = `${TOPIC_PREFIX}${bin.deviceCode}`;
    const message = JSON.stringify(payload);

    await new Promise<void>((resolve, reject) => {
      client.publish(topic, message, { qos: 1 }, (err) => {
        if (err) reject(err);
        else resolve();
      });
    }).then(() => {
      console.log(
        `  ✅ ${payload.device_code}  fill=${payload.fill_level_percent.toFixed(1).padStart(5)}%  ` +
        `dist=${payload.distance_cm.toFixed(1).padStart(6)}cm  ` +
        `rssi=${payload.signal_strength}dBm  → ${topic}`
      );
    }).catch((err) => {
      console.log(`  ❌ ${payload.device_code}  publish error: ${err.message}`);
    });
  }
}

// ─── Main ─────────────────────────────────────────────────────────────────────

async function main() {
  const bins = createBins();

  console.log("╔══════════════════════════════════════════════════╗");
  console.log("║       EcoRoute Firmware Simulator v1.0.0        ║");
  console.log("╚══════════════════════════════════════════════════╝");
  console.log();
  console.log(`  Broker:    ${BROKER_URL}`);
  console.log(`  Topic:     ${TOPIC_PREFIX}<device_code>`);
  console.log(`  Physical:  ${PHYSICAL_DEVICE} (excluded from simulation)`);
  console.log(`  Simulated: ${bins.length} bins (${bins.map(b => b.deviceCode.replace("ECO-BIN-","")).join(", ")})`);
  console.log(`  Interval:  ${REPORT_INTERVAL_SEC}s`);
  console.log(`  Mode:      ${ONCE ? "single report" : "continuous"}`);
  console.log();

  const client = await mqtt.connectAsync(BROKER_URL, {
    clientId: `ecoroute-simulator-${Date.now()}`,
    clean: true,
  });
  console.log(`Connected to ${BROKER_URL}\n`);

  if (ONCE) {
    await reportAll(client, bins);
    console.log("\nDone (single report mode).");
    await client.endAsync();
    return;
  }

  console.log("Press Ctrl+C to stop.\n");
  await reportAll(client, bins);

  const timer = setInterval(async () => {
    await reportAll(client, bins);
  }, REPORT_INTERVAL_SEC * 1000);

  process.on("SIGINT", async () => {
    clearInterval(timer);
    await client.endAsync();
    console.log("\nSimulator stopped.");
    process.exit(0);
  });
}

main().catch((err) => {
  console.error("Fatal:", err.message);
  process.exit(1);
});
