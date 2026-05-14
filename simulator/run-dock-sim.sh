#!/usr/bin/env bash
# Runs the docking station simulator — publishes every 1s (demo mode)
# Portal: http://localhost:8091   Login: admin / ecoroute
set -e
cd "$(dirname "$0")"
exec bun dock-sim.ts --port 8091 --interval 1
