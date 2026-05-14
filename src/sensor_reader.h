#ifndef ECOROUTE_SENSOR_READER_H
#define ECOROUTE_SENSOR_READER_H

#include <Arduino.h>
#include "config.h"

struct SensorReading {
  float distanceCm;         // Raw distance from ultrasonic sensor
  float fillLevelPercent;   // Calculated fill percentage (0-100)
  float batteryVoltage;     // Battery voltage in volts
  int32_t signalStrength;   // WiFi RSSI in dBm
  bool anomalyFlag;         // True if distance reading is anomalous
};

class SensorReader {
public:
  void begin();

  // Read all sensors and return combined result
  SensorReading readAll(float binHeightCm);

  // Individual sensor readings
  float readDistanceCm();
  float readBatteryVoltage();
  int32_t readWifiRssi();

private:
  // Sort helper for median filter
  static void sortArray(float arr[], int size);
};

#endif // ECOROUTE_SENSOR_READER_H
