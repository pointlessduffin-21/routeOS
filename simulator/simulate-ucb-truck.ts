/**
 * UCB Defense - Moving Truck Simulator
 * 
 * This script connects to the local backend, authenticates as the UCB Dispatcher,
 * and triggers the backend's built-in Route Execution Simulator.
 * 
 * Usage:
 * bun run firmware/simulator/simulate-ucb-truck.ts
 * 
 * Then open the web dashboard -> Operations Monitor. You will see a route 
 * automatically created and a truck icon moving between the bins!
 */

async function runSimulator() {
  const API_URL = "http://localhost:3000/api/v1";

  console.log("🚚 EcoRoute UCB Truck Simulator");
  console.log("=================================");

  try {
    // 1. Authenticate to get a token
    console.log("[1/3] Authenticating as UCB Dispatcher...");
    const loginRes = await fetch(`${API_URL}/auth/login`, {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify({
        email: "dispatcher.ucb@ecoroute.io",
        password: "ecoroute2026"
      })
    });

    if (!loginRes.ok) {
      throw new Error(`Login failed: ${loginRes.status} ${loginRes.statusText}`);
    }

    const loginData = await loginRes.json();
    const token = loginData.data?.session?.accessToken;
    if (!token) {
      throw new Error("Login succeeded but no access token was returned");
    }
    
    // We need to fetch the user's subdivision ID.
    // The easiest way is to hit the /auth/me endpoint.
    const meRes = await fetch(`${API_URL}/auth/me`, {
      headers: { "Authorization": `Bearer ${token}` }
    });
    
    if (!meRes.ok) {
      throw new Error("Failed to fetch user profile");
    }
    
    const meData = await meRes.json();
    const subdivisionId = meData.data.subdivisionId;
    
    if (!subdivisionId) {
      throw new Error("Dispatcher is not assigned to a subdivision!");
    }

    console.log(`[2/3] Authenticated successfully. Subdivision ID: ${subdivisionId}`);

    // 2. Trigger the simulation
    console.log("[3/3] Triggering the backend route execution simulator...");
    console.log("      (Keep the Operations Monitor open in your browser!)");
    console.log("");
    
    const simRes = await fetch(`${API_URL}/routes/simulate`, {
      method: "POST",
      headers: {
        "Content-Type": "application/json",
        "Authorization": `Bearer ${token}`
      },
      body: JSON.stringify({ subdivisionId })
    });

    if (!simRes.ok) {
        const errData = await simRes.text();
        throw new Error(`Simulation request failed: ${simRes.status} - ${errData}`);
    }

    const simData = await simRes.json();
    
    console.log("✅ Simulation completed on backend!");
    console.log("Events generated:");
    for (const event of simData.events ?? []) {
        console.log(`  [Step ${event.step}] ${event.action.toUpperCase()}: ${event.detail}`);
    }

  } catch (err) {
    console.error("❌ Simulation failed:", err);
  }
}

runSimulator();
