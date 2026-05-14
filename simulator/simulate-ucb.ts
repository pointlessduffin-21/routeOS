/**
 * UCB Presentation Simulator Wrapper
 * 
 * This script runs the existing dock-sim.ts with a preset configuration
 * specifically for the University of Cebu - Banilad presentation.
 * It targets UCB-BIN-001 and pushes data every 5 seconds.
 */

import { spawn } from "child_process";
import path from "path";

const simulatorPath = path.join(process.cwd(), "firmware/simulator/dock-sim.ts");

console.log("🚀 Starting UCB Presentation Simulator...");
console.log("Target: UCB-BIN-001 (UCB Banilad)");
console.log("Interval: 5 seconds");
console.log("------------------------------------------");

const child = spawn("bun", [simulatorPath, "--device", "UCB-BIN-001", "--interval", "5"], {
  stdio: "inherit"
});

child.on("close", (code) => {
  console.log(`Simulator exited with code ${code}`);
});
