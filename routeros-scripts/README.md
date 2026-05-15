# routeOS · RouterOS Script Templates
# =====================================
# Copy the numbered scripts to your RouterOS device and import them in order.
# Requires RouterOS 7.x for /iot mqtt commands.

## Quick Start

```bash
# 1. Edit 00-variables.rsc with your server IP, device ID, zone, and machine
# 2. SCP all scripts to router
scp routeros-scripts/*.rsc admin@<router-ip>:/flash/routeos/

# 3. In RouterOS terminal, import in order:
/import flash/routeos/00-variables.rsc
/import flash/routeos/04-broker-setup.rsc
/import flash/routeos/01-register.rsc
/import flash/routeos/02-heartbeat.rsc
/import flash/routeos/03-telemetry.rsc
/import flash/routeos/05-offline.rsc
```

## Script Reference

| File | Purpose | Run |
|------|---------|-----|
| `00-variables.rsc` | Set global config variables | Once |
| `01-register.rsc` | Register device via HTTP POST | On boot |
| `02-heartbeat.rsc` | Publish MQTT heartbeat every 30 s | Scheduled |
| `03-telemetry.rsc` | Publish CPU/RAM/uptime/errors every 5 s | Scheduled |
| `04-broker-setup.rsc` | Configure MQTT broker connection | Once |
| `05-offline.rsc` | Notify backend of shutdown | On shutdown |

## Auto-generate using the provisioning wizard

```bash
cd tools && bun provision.ts
```
