#ifndef ECOROUTE_MQTT_CLIENT_H
#define ECOROUTE_MQTT_CLIENT_H

#include <Arduino.h>
#include "sensor_reader.h"

struct MqttPublishResult {
  bool success;
  String error;
};

class EcoRouteMqttClient {
public:
  // Connect to broker and publish one telemetry message, then disconnect.
  // Topic: ecoroute/trash_can/<deviceCode>
  MqttPublishResult publishTelemetry(
    const char* deviceCode,
    const SensorReading& reading
  );
};

#endif // ECOROUTE_MQTT_CLIENT_H
