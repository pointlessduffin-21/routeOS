#include "http_client.h"
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include "config.h"

TelemetryResponse EcoRouteHttpClient::postTelemetry(
  const char* url,
  const char* apiKey,
  const char* deviceCode,
  const SensorReading& reading
) {
  TelemetryResponse result = { false, 0, -1, "" };

  HTTPClient http;
  http.begin(url);
  http.setTimeout(HTTP_TIMEOUT_MS);
  http.addHeader("Content-Type", "application/json");
  http.addHeader("X-Device-API-Key", apiKey);

  // Build JSON payload
  JsonDocument doc;
  doc["deviceCode"] = deviceCode;
  doc["fillLevelPercent"] = round(reading.fillLevelPercent * 10.0f) / 10.0f;  // 1 decimal
  doc["distanceCm"] = round(reading.distanceCm * 10.0f) / 10.0f;
  doc["batteryVoltage"] = round(reading.batteryVoltage * 100.0f) / 100.0f;    // 2 decimals
  doc["signalStrength"] = reading.signalStrength;
  doc["anomalyFlag"] = reading.anomalyFlag;

  String payload;
  serializeJson(doc, payload);

  Serial.printf("[http] POST %s\n", url);
  Serial.printf("[http] Payload: %s\n", payload.c_str());

  int httpCode = http.POST(payload);
  result.httpCode = httpCode;

  if (httpCode > 0) {
    String response = http.getString();
    Serial.printf("[http] Response (%d): %s\n", httpCode, response.c_str());

    if (httpCode == 201) {
      result.success = true;

      // Parse response to get telemetryId
      JsonDocument respDoc;
      DeserializationError err = deserializeJson(respDoc, response);
      if (!err && respDoc["data"]["telemetryId"].is<int>()) {
        result.telemetryId = respDoc["data"]["telemetryId"].as<int>();
      }
    } else {
      // Parse error message
      JsonDocument errDoc;
      DeserializationError err = deserializeJson(errDoc, response);
      if (!err && errDoc["error"].is<const char*>()) {
        result.error = errDoc["error"].as<String>();
      } else {
        result.error = "HTTP " + String(httpCode);
      }
    }
  } else {
    result.error = "Connection failed: " + http.errorToString(httpCode);
    Serial.printf("[http] Error: %s\n", result.error.c_str());
  }

  http.end();
  return result;
}

bool EcoRouteHttpClient::heartbeat(const char* baseUrl, const char* apiKey) {
  // Derive heartbeat URL from telemetry URL by replacing /telemetry with /heartbeat
  String url = String(baseUrl);
  int lastSlash = url.lastIndexOf('/');
  if (lastSlash > 0) {
    url = url.substring(0, lastSlash) + "/heartbeat";
  }

  HTTPClient http;
  http.begin(url);
  http.setTimeout(HTTP_TIMEOUT_MS);
  http.addHeader("X-Device-API-Key", apiKey);

  int httpCode = http.POST("");
  bool ok = (httpCode == 200);

  Serial.printf("[http] Heartbeat %s (%d)\n", ok ? "OK" : "FAILED", httpCode);

  http.end();
  return ok;
}
