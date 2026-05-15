import { getAllSensors, getCategories, getSensorById, searchSensors } from "../marketplace";

export async function handleMarketplace(
  req: Request,
  url: URL,
  corsHeaders: Record<string, string>
): Promise<Response> {
  const path = url.pathname.replace(/^\/api\/marketplace\/?/, "");
  const parts = path.split("/").filter(Boolean);

  // GET /api/marketplace/categories
  if (parts[0] === "categories" && req.method === "GET") {
    return Response.json({ data: getCategories() }, { headers: corsHeaders });
  }

  // GET /api/marketplace/search?q=...
  if (parts[0] === "search" && req.method === "GET") {
    const q = url.searchParams.get("q") ?? "";
    return Response.json({ data: searchSensors(q) }, { headers: corsHeaders });
  }

  // GET /api/marketplace/:id
  if (parts[0] && req.method === "GET") {
    const sensor = getSensorById(parts[0]);
    if (!sensor) return Response.json({ error: "Sensor type not found" }, { status: 404, headers: corsHeaders });
    return Response.json({ data: sensor }, { headers: corsHeaders });
  }

  // GET /api/marketplace
  if (!parts[0] && req.method === "GET") {
    const category = url.searchParams.get("category");
    let sensors = getAllSensors();
    if (category) sensors = sensors.filter((s) => s.category === category);
    return Response.json({ data: sensors, total: sensors.length }, { headers: corsHeaders });
  }

  return Response.json({ error: "Not found" }, { status: 404, headers: corsHeaders });
}
