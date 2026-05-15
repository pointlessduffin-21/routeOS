#!/usr/bin/env bun
/**
 * provision.ts — Interactive wizard that generates RouterOS .rsc scripts
 * and optionally pre-registers the device in the routeOS backend.
 *
 * Usage:  bun provision.ts
 */

import * as readline from "readline";

const rl = readline.createInterface({ input: process.stdin, output: process.stdout });
const ask = (q: string): Promise<string> => new Promise((r) => rl.question(q, r));
const askOpt = async (q: string, def: string): Promise<string> => {
  const ans = await ask(`${q} [${def}]: `);
  return ans.trim() || def;
};

function banner() {
  console.log("\n┌───────────────────────────────────────────┐");
  console.log("│   routeOS · RouterOS Provisioning Wizard   │");
  console.log("└───────────────────────────────────────────┘\n");
}

function generateScripts(cfg: Record<string, string>): string {
  const { host, apiPort, mqttPort, deviceId, deviceName, zone, machine, firmware, iface } = cfg;
  return `# ═══════════════════════════════════════════════════════
# routeOS — Auto-generated RouterOS provisioning scripts
# Device: ${deviceId}
# ═══════════════════════════════════════════════════════

# ── 0. Global variables (run once) ──────────────────────
:global syntagHost "${host}"
:global syntagESP32Port "${apiPort}"
:global mqttBrokerPort "${mqttPort}"
:global deviceId "${deviceId}"
:global deviceName "${deviceName}"
:global deviceZone "${zone}"
:global deviceMachine "${machine}"
:global deviceFirmware "${firmware}"

# ── 1. MQTT Broker ───────────────────────────────────────
/iot mqtt brokers add \\
  name="syntag-uns" \\
  address=${host} \\
  port=${mqttPort} \\
  client-id=${deviceId} \\
  connect-timeout=10s \\
  keepalive-interval=60s

# ── 2. Registration script ───────────────────────────────
/system/scripts add name="syntag-register" source={
  :local url ("http://" . $syntagHost . ":" . $syntagESP32Port . "/api/devices")
  :local payload ("{\\\"id\\\":\\\"\${deviceId}\\\",\\\"name\\\":\\\"\${deviceName}\\\",\\\"zone\\\":\\\"\${deviceZone}\\\",\\\"machine\\\":\\\"\${deviceMachine}\\\",\\\"firmware\\\":\\\"\${deviceFirmware}\\\",\\\"status\\\":\\\"online\\\",\\\"configuredMetrics\\\":[\\\"cpu_load\\\",\\\"free_memory\\\",\\\"uptime_seconds\\\",\\\"interface_errors\\\"]}")
  /tool fetch url=$url http-method=post http-header-field="Content-Type: application/json" http-data=$payload output=none as-value
  :log info ("syntag-register: registered at " . $url)
}

# ── 3. Heartbeat (30 s) ──────────────────────────────────
/system/scripts add name="syntag-heartbeat" source={
  :local topic ("plant/" . $deviceZone . "/" . $deviceMachine . "/status/heartbeat")
  :local ts [/system/clock get time]
  /iot mqtt publish broker="syntag-uns" topic=$topic message=("{\\\"value\\\":true,\\\"unit\\\":\\\"\\\",\\\"ts\\\":\\\"" . $ts . "\\\",\\\"quality\\\":\\\"good\\\"}") qos=0
}
/system/scheduler add name="syntag-heartbeat-sched" interval=30s on-event="syntag-heartbeat" start-time=startup

# ── 4. Telemetry (5 s) ───────────────────────────────────
/system/scripts add name="syntag-telemetry" source={
  :local ts [/system/clock get time]
  :local base ("plant/" . $deviceZone . "/" . $deviceMachine)
  :local cpu [/system/resource get cpu-load]
  :local mem [/system/resource get free-memory]
  :local up  [/system/resource get uptime]
  :local rxErr [/interface/ethernet get ${iface} rx-error]
  :local txErr [/interface/ethernet get ${iface} tx-error]
  /iot mqtt publish broker="syntag-uns" topic=($base . "/cpu_load")         message=("{\\\"value\\\":" . $cpu . ",\\\"unit\\\":\\\"%\\\",\\\"ts\\\":\\\"" . $ts . "\\\",\\\"quality\\\":\\\"good\\\"}") qos=1
  /iot mqtt publish broker="syntag-uns" topic=($base . "/free_memory")      message=("{\\\"value\\\":" . $mem . ",\\\"unit\\\":\\\"KB\\\",\\\"ts\\\":\\\"" . $ts . "\\\",\\\"quality\\\":\\\"good\\\"}") qos=1
  /iot mqtt publish broker="syntag-uns" topic=($base . "/uptime_seconds")   message=("{\\\"value\\\":\\\"" . $up . "\\\",\\\"unit\\\":\\\"s\\\",\\\"ts\\\":\\\"" . $ts . "\\\",\\\"quality\\\":\\\"good\\\"}") qos=1
  /iot mqtt publish broker="syntag-uns" topic=($base . "/interface_errors") message=("{\\\"rx_errors\\\":" . $rxErr . ",\\\"tx_errors\\\":" . $txErr . ",\\\"ts\\\":\\\"" . $ts . "\\\",\\\"quality\\\":\\\"good\\\"}") qos=1
}
/system/scheduler add name="syntag-telemetry-sched" interval=5s on-event="syntag-telemetry" start-time=startup

# ── 5. Offline on shutdown ───────────────────────────────
/system/scripts add name="syntag-offline" source={
  :local url ("http://" . $syntagHost . ":" . $syntagESP32Port . "/api/devices/" . $deviceId)
  /tool fetch url=$url http-method=put http-header-field="Content-Type: application/json" http-data="{\\\"status\\\":\\\"offline\\\"}" output=none as-value
}
/system/scheduler add name="syntag-offline-sched" on-event="syntag-offline" interval=0 start-time=shutdown

# ── Run registration now ─────────────────────────────────
/system/scripts run syntag-register
`;
}

async function preRegister(cfg: Record<string, string>): Promise<void> {
  const url  = `http://${cfg.host}:${cfg.apiPort}/api/devices`;
  const body = JSON.stringify({
    id: cfg.deviceId,
    name: cfg.deviceName,
    zone: cfg.zone,
    machine: cfg.machine,
    firmware: cfg.firmware,
    status: "online",
    configuredMetrics: ["cpu_load", "free_memory", "uptime_seconds", "interface_errors"],
    sensorTypes: ["routeros-system", "routeros-interfaces"],
  });
  try {
    const res = await fetch(url, { method: "POST", headers: { "Content-Type": "application/json" }, body });
    const data = await res.json() as { data?: unknown; error?: string };
    if (res.ok) {
      console.log(`\n✓ Pre-registered device at ${url}`);
    } else {
      console.error(`\n✗ Registration failed: ${data.error ?? res.statusText}`);
    }
  } catch (e) {
    console.error(`\n✗ Cannot reach backend at ${url} — scripts still generated`);
  }
}

async function main() {
  banner();
  console.log("Answer the following questions to generate your RouterOS scripts.\n");

  const cfg: Record<string, string> = {
    host:       await askOpt("routeOS backend host IP", "192.168.1.100"),
    apiPort:    await askOpt("ESP32 backend port",      "4003"),
    mqttPort:   await askOpt("MQTT broker port",        "1883"),
    deviceId:   await askOpt("Device ID",               "routeros-core-zone-a"),
    deviceName: await askOpt("Device display name",     "RouterOS Core Switch"),
    zone:       await askOpt("Zone name",               "Zone-A"),
    machine:    await askOpt("Machine / node name",     "core-switch"),
    firmware:   await askOpt("RouterOS firmware string","RouterOS 7.15.3"),
    iface:      await askOpt("Ethernet interface name", "ether1"),
  };

  const scripts = generateScripts(cfg);

  // Write to file
  const outFile = `${cfg.deviceId}-provision.rsc`;
  await Bun.write(outFile, scripts);
  console.log(`\n✓ Scripts written to: ${outFile}`);

  const preReg = (await askOpt("Pre-register device in backend now? (y/n)", "y")).toLowerCase();
  if (preReg === "y") {
    await preRegister(cfg);
  }

  console.log("\n─── Next steps ───────────────────────────────────");
  console.log(`1. Copy ${outFile} to your RouterOS device:`);
  console.log(`   scp ${outFile} admin@<router-ip>:/flash/`);
  console.log(`2. In RouterOS terminal:`);
  console.log(`   /import flash/${outFile}`);
  console.log("──────────────────────────────────────────────────\n");

  rl.close();
}

main().catch((e) => { console.error(e); process.exit(1); });
