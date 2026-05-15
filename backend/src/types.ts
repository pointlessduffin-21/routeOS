// Shared TypeScript types across routeOS backend

export type DeviceStatus = "online" | "offline" | "degraded";

export interface Device {
  id: string;
  name: string;
  zone: string;
  machine: string;
  firmware: string;
  status: DeviceStatus;
  configuredMetrics: string[];
  sensorTypes: string[];
  ipAddress: string | null;
  lastSeen: string | null;
  registeredAt: string;
  updatedAt: string;
}

export interface DeviceRow {
  id: string;
  name: string;
  zone: string;
  machine: string;
  firmware: string;
  status: DeviceStatus;
  configured_metrics: string;   // JSON array
  sensor_types: string;         // JSON array
  ip_address: string | null;
  last_seen: string | null;
  registered_at: string;
  updated_at: string;
}

export interface TelemetryPoint {
  id: number;
  deviceId: string;
  signal: string;
  value: number | null;
  valueStr: string | null;
  unit: string;
  quality: string;
  ts: string;
  recordedAt: string;
}

// ─── Marketplace types ───────────────────────────────────────────────────────

export type SignalType = "float" | "integer" | "boolean" | "string";

export interface SensorSignal {
  name: string;
  unit: string;
  type: SignalType;
  min?: number;
  max?: number;
  description: string;
  alarmHigh?: number;
  alarmLow?: number;
}

export interface SensorType {
  id: string;
  name: string;
  category: string;
  description: string;
  manufacturer: string;
  interface: string;
  icon: string;
  tags: string[];
  signals: SensorSignal[];
  mqttTopicPattern: string;
  gpioMap: Record<string, string> | null;
  configSchema: Record<string, unknown> | null;
  firmwareHint: string | null;
  routerosHint: string | null;
  datasheet: string | null;
}

// ─── API request/response shapes ────────────────────────────────────────────

export interface RegisterDeviceBody {
  id: string;
  name: string;
  zone: string;
  machine: string;
  firmware?: string;
  status?: DeviceStatus;
  configuredMetrics?: string[];
  sensorTypes?: string[];
  ipAddress?: string;
}

export interface UpdateDeviceBody {
  name?: string;
  zone?: string;
  machine?: string;
  firmware?: string;
  status?: DeviceStatus;
  configuredMetrics?: string[];
  sensorTypes?: string[];
  ipAddress?: string;
}

export interface ApiResponse<T> {
  data?: T;
  error?: string;
  message?: string;
}
