#!/usr/bin/env bash
# Runs the lid-sensor bin simulator — publishes every 1s (demo mode)
# Portal: http://localhost:8093   Login: admin / ecoroute
set -e
cd "$(dirname "$0")"
exec bun lid-sim.ts --port 8093 --interval 1
