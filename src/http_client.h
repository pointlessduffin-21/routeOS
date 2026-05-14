#ifndef ECOROUTE_HTTP_CLIENT_H
#define ECOROUTE_HTTP_CLIENT_H

#include <Arduino.h>
#include "sensor_reader.h"

struct TelemetryResponse {
  bool success;
  int httpCode;
  int telemetryId;
  String error;
};

class EcoRouteHttpClient {
public:
  // Post a telemetry reading to the backend API
  TelemetryResponse postTelemetry(
    const char* url,
    const char* apiKey,
    const char* deviceCode,
    const SensorReading& reading
  );

  // Simple heartbeat check
  bool heartbeat(const char* baseUrl, const char* apiKey);
};

#endif // ECOROUTE_HTTP_CLIENT_H
