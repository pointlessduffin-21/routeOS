#include "mqtt_client.h"
#include "config.h"

#include <WiFi.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>

MqttPublishResult EcoRouteMqttClient::publishTelemetry(
  const char* deviceCode,
  const SensorReading& reading
) {
  WiFiClient wifiClient;
  PubSubClient mqtt(wifiClient);

  mqtt.setServer(MQTT_BROKER_HOST, MQTT_BROKER_PORT);

  // Build client ID from device code to avoid collisions
  String clientId = String("ecoroute-") + deviceCode;

  Serial.printf("[mqtt] Connecting to %s:%d as %s...\n",
    MQTT_BROKER_HOST, MQTT_BROKER_PORT, clientId.c_str());

  uint32_t start = millis();
  while (!mqtt.connected()) {
    if (mqtt.connect(clientId.c_str())) {
      Serial.println("[mqtt] Connected");
      break;
    }
    if (millis() - start > MQTT_CONNECT_TIMEOUT_MS) {
      return { false, String("Connect timeout, rc=") + mqtt.state() };
    }
    delay(500);
  }

  // Build topic: ecoroute/trash_can/<device_code>
  String topic = String(MQTT_TOPIC_PREFIX) + deviceCode;

  // Build JSON payload
  JsonDocument doc;
  doc["device_code"]        = deviceCode;
  doc["fill_level_percent"] = reading.fillLevelPercent;
  doc["distance_cm"]        = reading.distanceCm;
  doc["battery_voltage"]    = reading.batteryVoltage;
  doc["signal_strength"]    = reading.signalStrength;
  doc["anomaly_flag"]       = reading.anomalyFlag;
  doc["firmware_version"]   = FIRMWARE_VERSION;

  char payload[256];
  size_t len = serializeJson(doc, payload, sizeof(payload));

  bool ok = mqtt.publish(topic.c_str(), payload, len);
  mqtt.disconnect();

  if (!ok) {
    return { false, "publish failed" };
  }

  Serial.printf("[mqtt] Published to %s: %s\n", topic.c_str(), payload);
  return { true, "" };
}
