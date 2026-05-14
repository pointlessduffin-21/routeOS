#include "sensor_reader.h"
#include <WiFi.h>

void SensorReader::begin() {
  // HC-SR04 pins
  pinMode(HCSR04_TRIG_PIN, OUTPUT);
  pinMode(HCSR04_ECHO_PIN, INPUT);
  digitalWrite(HCSR04_TRIG_PIN, LOW);

  // Battery ADC
  analogReadResolution(12);  // 12-bit ADC (0-4095)
  analogSetAttenuation(ADC_11db);  // Full range ~0-3.3V

  Serial.println("[sensor] Initialized HC-SR04 and battery ADC");
}

// ─── Distance Measurement ───────────────────────────────────────────────────

float SensorReader::readDistanceCm() {
  float samples[NUM_DISTANCE_SAMPLES];
  int validCount = 0;

  for (int i = 0; i < NUM_DISTANCE_SAMPLES; i++) {
    // Send trigger pulse
    digitalWrite(HCSR04_TRIG_PIN, LOW);
    delayMicroseconds(2);
    digitalWrite(HCSR04_TRIG_PIN, HIGH);
    delayMicroseconds(10);
    digitalWrite(HCSR04_TRIG_PIN, LOW);

    // Measure echo pulse duration
    long duration = pulseIn(HCSR04_ECHO_PIN, HIGH, ECHO_TIMEOUT_US);

    if (duration > 0) {
      // Speed of sound: 343 m/s = 0.0343 cm/us, divide by 2 for round trip
      float distance = (float)duration * 0.0343f / 2.0f;
      samples[validCount++] = distance;
    }

    delay(SAMPLE_DELAY_MS);
  }

  if (validCount == 0) {
    Serial.println("[sensor] HC-SR04: No valid readings (all timed out)");
    return -1.0f;  // Will trigger anomaly flag
  }

  // Return median of valid readings
  sortArray(samples, validCount);
  float median = samples[validCount / 2];

  Serial.printf("[sensor] HC-SR04: %.1f cm (median of %d samples)\n", median, validCount);
  return median;
}

// ─── Battery Voltage ────────────────────────────────────────────────────────

float SensorReader::readBatteryVoltage() {
  // Take average of multiple ADC reads for stability
  long sum = 0;
  const int numReads = 10;

  for (int i = 0; i < numReads; i++) {
    sum += analogRead(BATTERY_ADC_PIN);
    delay(5);
  }

  float avgAdc = (float)sum / numReads;
  float voltage = (avgAdc / 4095.0f) * ADC_VREF * VOLTAGE_DIVIDER_RATIO;

  Serial.printf("[sensor] Battery: %.2fV (ADC avg: %.0f)\n", voltage, avgAdc);
  return voltage;
}

// ─── WiFi Signal Strength ───────────────────────────────────────────────────

int32_t SensorReader::readWifiRssi() {
  int32_t rssi = WiFi.RSSI();
  Serial.printf("[sensor] WiFi RSSI: %d dBm\n", rssi);
  return rssi;
}

// ─── Combined Reading ───────────────────────────────────────────────────────

SensorReading SensorReader::readAll(float binHeightCm) {
  SensorReading reading;

  reading.distanceCm = readDistanceCm();
  reading.batteryVoltage = readBatteryVoltage();
  reading.signalStrength = readWifiRssi();

  // Calculate fill percentage: empty bin = full distance, full bin = 0 distance
  if (reading.distanceCm >= 0 && binHeightCm > 0) {
    float fillRatio = 1.0f - (reading.distanceCm / binHeightCm);
    reading.fillLevelPercent = constrain(fillRatio * 100.0f, 0.0f, 100.0f);
  } else {
    reading.fillLevelPercent = 0.0f;
  }

  // Anomaly detection: negative distance or beyond 300cm
  reading.anomalyFlag = (reading.distanceCm < 0 || reading.distanceCm > 300.0f);

  Serial.printf("[sensor] Fill: %.1f%% | Distance: %.1f cm | Battery: %.2fV | RSSI: %d dBm | Anomaly: %s\n",
    reading.fillLevelPercent,
    reading.distanceCm,
    reading.batteryVoltage,
    reading.signalStrength,
    reading.anomalyFlag ? "YES" : "no"
  );

  return reading;
}

// ─── Sort Helper ────────────────────────────────────────────────────────────

void SensorReader::sortArray(float arr[], int size) {
  for (int i = 0; i < size - 1; i++) {
    for (int j = 0; j < size - i - 1; j++) {
      if (arr[j] > arr[j + 1]) {
        float temp = arr[j];
        arr[j] = arr[j + 1];
        arr[j + 1] = temp;
      }
    }
  }
}
