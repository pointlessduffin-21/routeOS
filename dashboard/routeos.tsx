import { Badge } from "@/components/ui/badge";
import { Card, CardContent, CardHeader, CardTitle } from "@/components/ui/card";
import api from "@/lib/api";
import { cn } from "@/lib/utils";
import type { BinTelemetry, PaginatedResponse, SmartBin } from "@/types/api";
import { useQuery } from "@tanstack/react-query";
import {
    Activity,
    Battery,
    Cpu,
    Radio,
    RefreshCw,
    Signal,
    Wifi,
    WifiOff
} from "lucide-react";

function batteryColor(v: number | null): string {
  if (!v) return "text-[#52584E]";
  if (v >= 3.8) return "text-[#7FA67E]";
  if (v >= 3.5) return "text-[#C8E03A]";
  return "text-[#C97A3C]";
}

function batteryPct(v: number | null): number {
  if (!v) return 0;
  const min = 3.0, max = 4.2;
  return Math.max(0, Math.min(100, Math.round(((v - min) / (max - min)) * 100)));
}

function signalStrength(rssi: number | null): { label: string; color: string; bars: number } {
  if (rssi === null) return { label: "No signal", color: "text-[#52584E]", bars: 0 };
  if (rssi >= -60) return { label: "Excellent", color: "text-[#7FA67E]", bars: 4 };
  if (rssi >= -70) return { label: "Good", color: "text-[#7FA67E]", bars: 3 };
  if (rssi >= -80) return { label: "Fair", color: "text-[#C8E03A]", bars: 2 };
  return { label: "Weak", color: "text-[#C97A3C]", bars: 1 };
}

function SignalBars({ bars }: { bars: number }) {
  return (
    <span className="inline-flex items-end gap-px h-4">
      {[1, 2, 3, 4].map((b) => (
        <span
          key={b}
          className={cn(
            "inline-block w-1 rounded-sm transition-colors",
            b <= bars ? "bg-current" : "bg-[#1F2A23]"
          )}
          style={{ height: `${b * 4}px` }}
        />
      ))}
    </span>
  );
}

function timeAgo(dateStr: string | null): string {
  if (!dateStr) return "never";
  const seconds = Math.floor((Date.now() - new Date(dateStr).getTime()) / 1000);
  if (seconds < 60) return `${seconds}s ago`;
  const minutes = Math.floor(seconds / 60);
  if (minutes < 60) return `${minutes}m ago`;
  const hours = Math.floor(minutes / 60);
  if (hours < 24) return `${hours}h ago`;
  return `${Math.floor(hours / 24)}d ago`;
}

export function RouteOSPage() {
  const { data: binsRes, isLoading, refetch, isFetching } = useQuery({
    queryKey: ["bins"],
    queryFn: async () => {
      const res = await api.get<PaginatedResponse<SmartBin>>("/bins");
      return res.data;
    },
    refetchInterval: 5_000,
  });

  const { data: telemetryRes } = useQuery({
    queryKey: ["bins-telemetry"],
    queryFn: async () => {
      const res = await api.get<PaginatedResponse<BinTelemetry>>("/telemetry?limit=200&sort=-recordedAt");
      const map: Record<string, BinTelemetry> = {};
      for (const t of res.data.data) {
        if (!map[t.deviceId]) map[t.deviceId] = t;
      }
      return map;
    },
    refetchInterval: 5_000,
  });

  const bins = binsRes?.data ?? [];
  const tMap = telemetryRes ?? {};

  const online = bins.filter((b) => b.status === "active").length;
  const offline = bins.filter((b) => b.status === "offline").length;

  return (
    <div className="space-y-8">
      {/* Masthead */}
      <div className="flex items-start justify-between">
        <div>
          <p className="eyebrow mb-2 text-[#C8E03A]">Embedded firmware · ESP32-WROOM</p>
          <h1
            className="text-[2.4rem] leading-[0.9] tracking-[-0.03em] text-[#F0EAD6]"
            style={{ fontFamily: "var(--font-display)", fontWeight: 300 }}
          >
            route<span className="italic text-[#C8E03A]">OS</span>
          </h1>
          <p className="caption mt-2 text-[#52584E]">v3.2.0-tc3 · routeOS fleet management</p>
        </div>
        <button
          onClick={() => refetch()}
          className={cn(
            "flex items-center gap-2 rounded-md border border-[#1F2A23] bg-[#1a2620] px-3 py-2 text-[12px] text-[#9CA896] hover:text-[#F0EAD6] transition-colors",
          )}
          style={{ fontFamily: "var(--font-mono)" }}
        >
          <RefreshCw className={cn("h-3.5 w-3.5", isFetching && "animate-spin")} />
          Refresh
        </button>
      </div>

      <div className="rule" />

      {/* Fleet stats strip */}
      <div className="grid grid-cols-2 gap-6 md:grid-cols-4">
        <div>
          <p className="eyebrow mb-2 text-[#52584E]">Total devices</p>
          <p
            className="text-[2.2rem] leading-none tracking-[-0.02em] text-[#F0EAD6] tabular-nums"
            style={{ fontFamily: "var(--font-display)", fontWeight: 300 }}
          >
            {bins.length}
          </p>
        </div>
        <div>
          <p className="eyebrow mb-2 text-[#52584E]">Online</p>
          <p
            className="text-[2.2rem] leading-none tracking-[-0.02em] text-[#7FA67E] tabular-nums"
            style={{ fontFamily: "var(--font-display)", fontWeight: 300 }}
          >
            {online}
          </p>
        </div>
        <div>
          <p className="eyebrow mb-2 text-[#52584E]">Offline</p>
          <p
            className="text-[2.2rem] leading-none tracking-[-0.02em] tabular-nums"
            style={{
              fontFamily: "var(--font-display)",
              fontWeight: 300,
              color: offline > 0 ? "#C97A3C" : "#52584E",
            }}
          >
            {offline}
          </p>
        </div>
        <div>
          <p className="eyebrow mb-2 text-[#52584E]">Firmware</p>
          <p
            className="text-[1.1rem] leading-none tracking-[-0.01em] text-[#9CA896] font-[family-name:var(--font-mono)]"
          >
            v3.2.0-tc3
          </p>
        </div>
      </div>

      <div className="rule" />

      {/* Device fleet grid */}
      {isLoading ? (
        <div className="flex items-center justify-center py-20">
          <div className="flex flex-col items-center gap-4">
            <RefreshCw className="h-8 w-8 animate-spin text-[#52584E]" />
            <p className="caption">Scanning fleet...</p>
          </div>
        </div>
      ) : bins.length === 0 ? (
        <div className="flex flex-col items-center justify-center py-24 text-center">
          <Cpu className="mb-4 h-12 w-12 text-[#2D3A2E]" />
          <p className="eyebrow mb-2 text-[#52584E]">No devices provisioned</p>
          <p className="text-[#52584E] text-sm max-w-xs">
            Use the mobile app to provision your first routeOS device via BLE.
          </p>
        </div>
      ) : (
        <div className="grid grid-cols-1 gap-4 sm:grid-cols-2 lg:grid-cols-3">
          {bins.map((bin) => {
            const t = tMap[bin.id] ?? null;
            const isOnline = bin.status === "active";
            const isOffline = bin.status === "offline";
            const fill = t?.fillLevelPercent ?? null;
            const rssi = t?.signalStrength ?? null;
            const sig = signalStrength(rssi);
            const bv = t?.batteryVoltage ?? null;

            return (
              <Card
                key={bin.id}
                className={cn(
                  "transition-colors",
                  isOffline && "border-[#C97A3C]/20 opacity-60"
                )}
              >
                <CardHeader className="pb-3">
                  <div className="flex items-center justify-between">
                    <div className="flex items-center gap-2">
                      <Cpu className={cn("h-4 w-4", isOnline ? "text-[#C8E03A]" : "text-[#52584E]")} />
                      <CardTitle className="text-[#C8E03A] text-[10px]">
                        {bin.deviceCode}
                      </CardTitle>
                    </div>
                    <Badge variant={isOnline ? "success" : isOffline ? "destructive" : "secondary"}>
                      {bin.status}
                    </Badge>
                  </div>
                </CardHeader>

                <CardContent className="space-y-4">
                  {/* Fill level */}
                  {fill !== null && (
                    <div>
                      <div className="mb-1 flex justify-between">
                        <p className="caption text-[#52584E]">Fill level</p>
                        <p
                          className="text-[11px] tabular-nums"
                          style={{ fontFamily: "var(--font-mono)", color: fill > 80 ? "#C97A3C" : fill > 50 ? "#C8E03A" : "#7FA67E" }}
                        >
                          {fill.toFixed(0)}%
                        </p>
                      </div>
                      <div className="h-1.5 w-full overflow-hidden rounded-full bg-[#1a2620]">
                        <div
                          className={cn(
                            "h-full rounded-full transition-all",
                            fill > 80 ? "bg-[#C97A3C]" : fill > 50 ? "bg-[#C8E03A]" : "bg-[#7FA67E]"
                          )}
                          style={{ width: `${fill}%` }}
                        />
                      </div>
                    </div>
                  )}

                  {/* Telemetry row */}
                  <div className="grid grid-cols-3 gap-2">
                    {/* Signal */}
                    <div className="space-y-1">
                      <p className="caption text-[#52584E]">Signal</p>
                      <div className={cn("flex items-center gap-1.5", sig.color)}>
                        <SignalBars bars={sig.bars} />
                        <span style={{ fontFamily: "var(--font-mono)", fontSize: 10 }}>{rssi ?? "—"}</span>
                      </div>
                    </div>

                    {/* Battery */}
                    <div className="space-y-1">
                      <p className="caption text-[#52584E]">Battery</p>
                      <div className={cn("flex items-center gap-1", batteryColor(bv))}>
                        <Battery className="h-3 w-3" />
                        <span style={{ fontFamily: "var(--font-mono)", fontSize: 10 }}>
                          {bv != null ? `${bv.toFixed(1)}V` : "—"}
                        </span>
                      </div>
                      {bv != null && (
                        <div className="h-1 w-full overflow-hidden rounded-full bg-[#1a2620]">
                          <div
                            className={cn("h-full rounded-full", batteryColor(bv).replace("text-", "bg-"))}
                            style={{ width: `${batteryPct(bv)}%` }}
                          />
                        </div>
                      )}
                    </div>

                    {/* Lid */}
                    <div className="space-y-1">
                      <p className="caption text-[#52584E]">Lid</p>
                      <div className="flex items-center gap-1 text-[#9CA896]">
                        <Activity className="h-3 w-3" />
                        <span style={{ fontFamily: "var(--font-mono)", fontSize: 10 }}>
                          {t?.lidClosed != null ? (t.lidClosed ? "Closed" : "Open") : "—"}
                        </span>
                      </div>
                    </div>
                  </div>

                  {/* Footer row */}
                  <div className="flex items-center justify-between pt-1 border-t border-[#1F2A23]">
                    <div className="flex items-center gap-1.5 text-[#52584E]">
                      {isOnline ? (
                        <Wifi className="h-3 w-3 text-[#7FA67E]" />
                      ) : (
                        <WifiOff className="h-3 w-3 text-[#C97A3C]" />
                      )}
                      <span className="caption">{isOnline ? "connected" : "disconnected"}</span>
                    </div>
                    <span className="caption">
                      {timeAgo(bin.lastSeenAt ?? null)}
                    </span>
                  </div>

                  {/* Firmware version */}
                  {bin.firmwareVersion && (
                    <div className="flex items-center gap-1.5">
                      <Radio className="h-3 w-3 text-[#52584E] shrink-0" />
                      <span
                        className="caption text-[#52584E]"
                        style={{ fontFamily: "var(--font-mono)" }}
                      >
                        fw {bin.firmwareVersion}
                      </span>
                    </div>
                  )}

                  {/* Location */}
                  <div className="flex items-center gap-1.5">
                    <Signal className="h-3 w-3 text-[#52584E] shrink-0" />
                    <span
                      className="caption text-[#52584E] tabular-nums"
                      style={{ fontFamily: "var(--font-mono)" }}
                    >
                      {bin.latitude.toFixed(5)}, {bin.longitude.toFixed(5)}
                    </span>
                  </div>
                </CardContent>
              </Card>
            );
          })}
        </div>
      )}

      {/* Spec footer */}
      <div className="rule mt-4" />
      <div className="grid grid-cols-2 gap-4 md:grid-cols-4 pb-4">
        {[
          { label: "Sensor", value: "3× RCWL-9610A" },
          { label: "Lid sensor", value: "HW-201 IR" },
          { label: "MCU", value: "ESP32-WROOM" },
          { label: "Protocol", value: "MQTT · JSON" },
        ].map(({ label, value }) => (
          <div key={label}>
            <p className="eyebrow mb-1 text-[#52584E]">{label}</p>
            <p className="text-[#9CA896] text-[12px]" style={{ fontFamily: "var(--font-mono)" }}>
              {value}
            </p>
          </div>
        ))}
      </div>
    </div>
  );
}
