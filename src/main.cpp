/*
 * EcoRoute Smart Bin — Production Firmware v4.0.0
 *
 * Board:    ESP32-WROOM-32 (any dev board, 3.3V logic)
 * Sensors:  3× HC-SR04 ultrasonic (US1 left, US2 center, US3 right) + HW-201 IR (lid detect)
 * Power:    2x 18650 parallel via TP4056 charge controller
 *
 * Features:
 *   - HC-SR04 ultrasonic with 5-sample median filtering
 *   - HW-201 IR lid detection (LOW = closed)
 *   - Battery voltage via ADC with 2x 100K divider
 *   - Adaptive MQTT publish interval based on fill level
 *   - NVS persistent configuration (Preferences library)
 *   - Captive portal JSON API for provisioning (AP mode)
 *   - HTTP Basic Auth on all portal endpoints
 *   - Deep sleep when fill < 30% on battery power
 *   - Hardware watchdog timer (5 minute timeout)
 *   - LED status indicators (GPIO 2)
 *
 * Wiring:
 *   HC-SR04:
 *     VCC  -> 5V (or 3.3V for 3.3V-tolerant modules)
 *     GND  -> GND
 *     TRIG -> GPIO 5
 *     ECHO -> GPIO 18
 *
 *   HW-201 (IR lid detect):
 *     VCC  -> 3V3
 *     GND  -> GND
 *     OUT  -> GPIO 4  (LOW = object detected = lid closed)
 *
 *   Battery (via TP4056 OUT):
 *     OUT+ --[100K]--+--[100K]-- GND
 *                    +-- GPIO 34 (ADC)
 *     OUT+ -> VIN
 *     OUT- -> GND
 *
 * MQTT Topic: ecoroute/trash_can/<device_code>
 */

#include <WiFi.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>
#include <Preferences.h>
#include <esp_task_wdt.h>
#include <esp_idf_version.h>
#include <esp_sleep.h>

#include "config_manager.h"
#include "ble_provisioning.h"

// =============================================================================
//  COMPILE-TIME DEFAULTS (overridden by NVS stored config)
// =============================================================================

#define DEFAULT_WIFI_SSID       ""
#define DEFAULT_WIFI_PASS       ""

#define DEFAULT_MQTT_BROKER     "109.123.238.215"
#define DEFAULT_MQTT_PORT       1883
#define DEFAULT_MQTT_USER       "ecoroute"
#define DEFAULT_MQTT_PASS       "secret"
#define DEFAULT_MQTT_TOPIC      "ecoroute/trash_can/"

#define DEFAULT_BIN_HEIGHT_CM   100.0f
#define DEFAULT_SENSOR_OFFSET   2.0f

#define DEFAULT_PORTAL_USER     "admin"
#define DEFAULT_PORTAL_PASS     "ecoroute"

// =============================================================================
//  ADAPTIVE PUBLISH INTERVALS (seconds)
// =============================================================================

#define INTERVAL_LOW_SEC        180       // fill < 50%  -> every 3 min
#define INTERVAL_MEDIUM_SEC     90        // fill 50-75% -> every 90 s
#define INTERVAL_HIGH_SEC       30        // fill > 75%  -> every 30 s
#define INTERVAL_HEARTBEAT_SEC  600       // max silence -> every 10 min

// =============================================================================
//  PIN DEFINITIONS
// =============================================================================

// US1 — Left sensor
#define US1_TRIG_PIN        12
#define US1_ECHO_PIN        13
// US2 — Center sensor (original single-sensor position)
#define US2_TRIG_PIN        5
#define US2_ECHO_PIN        18
// US3 — Right sensor
#define US3_TRIG_PIN        19
#define US3_ECHO_PIN        21

// Legacy aliases kept so existing references compile
#define TRIG_PIN            US2_TRIG_PIN
#define ECHO_PIN            US2_ECHO_PIN

#define IR_PIN              4         // HW-201 OUT: LOW = object detected (lid closed)
#define BATTERY_ADC_PIN     34
#define LED_PIN             2

// =============================================================================
//  SENSOR CONSTANTS
// =============================================================================

#define FIRMWARE_VERSION    "4.0.0"
#define SENSOR_SAMPLES      5
#define SENSOR_TIMEOUT_US   30000     // 30ms timeout ~= 5m max range
#define SENSOR_MIN_CM       2.0f
#define SENSOR_MAX_CM       400.0f
#define SENSOR_DELAY_MS     30        // delay between ultrasonic readings

#define IR_DEBOUNCE_READS   5
#define IR_DEBOUNCE_DELAY   10        // ms between debounce reads

// =============================================================================
//  BATTERY CONSTANTS
// =============================================================================

#define BATT_FULL_V         4.20f
#define BATT_NOMINAL_V      3.70f
#define BATT_LOW_V          3.30f
#define BATT_CRITICAL_V     3.00f
#define BATT_DIVIDER        2.0f      // 2x 100K resistor divider
#define ADC_VREF            3.3f
#define ADC_RESOLUTION      4095.0f
#define BATT_ADC_SAMPLES    16

// =============================================================================
//  TIMING CONSTANTS
// =============================================================================

#define WDT_TIMEOUT_SEC     300       // 5 minute watchdog
#define WIFI_CONNECT_TRIES  20        // 20 * 500ms = 10s max
#define SENSOR_READ_INTERVAL_MS 10000 // read sensors every 10s
#define RECONNECT_INTERVAL_MS   30000 // try reconnect every 30s
#define FILL_CHANGE_THRESHOLD   5.0f  // publish immediately if fill changes by 5%

#define DEEP_SLEEP_FILL_THRESHOLD 30.0f
#define DEEP_SLEEP_DURATION_US    (INTERVAL_LOW_SEC * 1000000ULL)

#define AP_CHANNEL          1
#define DNS_PORT            53

// =============================================================================
//  FORWARD DECLARATIONS
// =============================================================================

void startAccessPoint(bool hidden);
void startAccessPoint();
void stopAccessPoint();
void enableOnDemandAp();
void updateSensors();
bool publishTelemetry();

// =============================================================================
//  LED BLINK PATTERNS
// =============================================================================

enum LedPattern {
  LED_OFF,
  LED_SOLID,          // connected
  LED_FAST_BLINK,     // connecting
  LED_SLOW_BLINK      // AP mode
};

// =============================================================================
//  GLOBAL STATE
// =============================================================================

WiFiClient   wifiClient;
PubSubClient mqtt(wifiClient);
WebServer    webServer(80);
DNSServer    dnsServer;
Preferences  prefs;

// Current sensor readings — per-sensor (US1=left, US2=center, US3=right)
float    currentDistanceLeft   = -1.0f;
float    currentFillLeft       = -1.0f;
float    currentDistanceCenter = -1.0f;
float    currentFillCenter     = -1.0f;
float    currentDistanceRight  = -1.0f;
float    currentFillRight      = -1.0f;

// Fused readings (averaged across valid sensors)
float    currentDistanceCm   = -1.0f;
float    currentFillPercent  = -1.0f;
float    currentBatteryV     = 0.0f;
int      currentBatteryPct   = 0;
int32_t  currentRssi         = 0;
bool     currentAnomaly      = false;
bool     currentLidClosed    = true;

// State tracking
bool     lastLidState        = true;
float    lastPublishedFill   = -999.0f;
float    lastPublishedBatt   = -999.0f;

unsigned long lastPublishTime     = 0;
unsigned long lastSensorReadTime  = 0;
unsigned long lastReconnectTime   = 0;
unsigned long publishCount        = 0;

bool     mqttConnected       = false;
bool     wifiConnected       = false;
bool     apModeActive        = false;

// Configuration (loaded from NVS)
String   cfgDeviceCode;
String   cfgWifiSsid;
String   cfgWifiPass;
String   cfgMqttBroker;
int      cfgMqttPort;
String   cfgMqttUser;
String   cfgMqttPass;
String   cfgMqttTopic;
String   cfgPortalUser;
String   cfgPortalPass;
float    binHeightCm;
float    sensorOffsetCm;

// LED
LedPattern   currentLedPattern = LED_OFF;
unsigned long lastLedToggle    = 0;
bool         ledState          = false;

// AP SSID (derived from device code)
String   apSsid;

// BLE provisioning
ConfigManager    cfgManager;
BleProvisioning  bleProvisioning;
bool             bleActive = false;

// AP broadcast mode
uint8_t  cfgApMode            = AP_MODE_ALWAYS;
bool     apOnDemandActive     = false;       // true when on-demand AP is temporarily enabled
unsigned long apOnDemandStart = 0;           // millis() when on-demand AP was enabled

// =============================================================================
//  NVS CONFIGURATION
// =============================================================================

void loadConfig() {
  prefs.begin("ecoroute", true);  // read-only

  cfgDeviceCode  = prefs.getString("deviceCode", DEFAULT_DEVICE_CODE);
  cfgWifiSsid    = prefs.getString("wifiSsid",   DEFAULT_WIFI_SSID);
  cfgWifiPass    = prefs.getString("wifiPass",    DEFAULT_WIFI_PASS);
  cfgMqttBroker  = prefs.getString("mqttBroker",  DEFAULT_MQTT_BROKER);
  cfgMqttPort    = prefs.getInt("mqttPort",       DEFAULT_MQTT_PORT);
  cfgMqttUser    = prefs.getString("mqttUser",    DEFAULT_MQTT_USER);
  cfgMqttPass    = prefs.getString("mqttPass",    DEFAULT_MQTT_PASS);
  cfgMqttTopic   = prefs.getString("mqttTopic",   DEFAULT_MQTT_TOPIC);
  cfgPortalUser  = prefs.getString("portalUser",  DEFAULT_PORTAL_USER);
  cfgPortalPass  = prefs.getString("portalPass",  DEFAULT_PORTAL_PASS);
  binHeightCm    = prefs.getFloat("binHeight",    DEFAULT_BIN_HEIGHT_CM);
  sensorOffsetCm = prefs.getFloat("sensorOffset", DEFAULT_SENSOR_OFFSET);
  cfgApMode      = prefs.getUChar("apMode",       AP_MODE_ALWAYS);

  prefs.end();

  Serial.printf("[config] Loaded — Device: %s, WiFi: '%s', MQTT: %s:%d\n",
    cfgDeviceCode.c_str(), cfgWifiSsid.c_str(),
    cfgMqttBroker.c_str(), cfgMqttPort);
  Serial.printf("[config] Bin height: %.1f cm, Sensor offset: %.1f cm\n",
    binHeightCm, sensorOffsetCm);
}

void saveConfig() {
  prefs.begin("ecoroute", false);  // read-write

  prefs.putString("deviceCode", cfgDeviceCode);
  prefs.putString("wifiSsid",   cfgWifiSsid);
  prefs.putString("wifiPass",   cfgWifiPass);
  prefs.putString("mqttBroker", cfgMqttBroker);
  prefs.putInt("mqttPort",      cfgMqttPort);
  prefs.putString("mqttUser",   cfgMqttUser);
  prefs.putString("mqttPass",   cfgMqttPass);
  prefs.putString("mqttTopic",  cfgMqttTopic);
  prefs.putString("portalUser", cfgPortalUser);
  prefs.putString("portalPass", cfgPortalPass);
  prefs.putFloat("binHeight",   binHeightCm);
  prefs.putFloat("sensorOffset", sensorOffsetCm);

  prefs.end();
  Serial.println("[config] Saved to NVS");
}

// =============================================================================
//  HC-SR04 ULTRASONIC SENSOR
// =============================================================================

// Single distance reading from a specific TRIG/ECHO pin pair
float readDistanceSingle(uint8_t trigPin, uint8_t echoPin) {
  digitalWrite(trigPin, LOW);
  delayMicroseconds(2);
  digitalWrite(trigPin, HIGH);
  delayMicroseconds(10);
  digitalWrite(trigPin, LOW);

  unsigned long duration = pulseIn(echoPin, HIGH, SENSOR_TIMEOUT_US);
  if (duration == 0) return -1.0f;

  float distanceCm = (float)duration * 0.01715f;
  if (distanceCm < SENSOR_MIN_CM || distanceCm > SENSOR_MAX_CM) return -1.0f;
  return distanceCm;
}

// Median-filtered reading from a specific sensor
float readDistanceMedian(uint8_t trigPin, uint8_t echoPin) {
  float readings[SENSOR_SAMPLES];
  int validCount = 0;

  for (int i = 0; i < SENSOR_SAMPLES; i++) {
    float d = readDistanceSingle(trigPin, echoPin);
    if (d > 0) readings[validCount++] = d;
    delay(SENSOR_DELAY_MS);
  }

  if (validCount < (SENSOR_SAMPLES / 2 + 1)) {
    Serial.printf("[sensor] US(%d/%d): only %d/%d valid\n", trigPin, echoPin, validCount, SENSOR_SAMPLES);
    return -1.0f;
  }

  for (int i = 1; i < validCount; i++) {
    float key = readings[i];
    int j = i - 1;
    while (j >= 0 && readings[j] > key) { readings[j + 1] = readings[j]; j--; }
    readings[j + 1] = key;
  }
  return readings[validCount / 2];
}

// =============================================================================
//  FILL LEVEL CALCULATION
// =============================================================================

float calculateFillPercent(float distanceCm) {
  if (distanceCm < 0) return -1.0f;

  // Account for sensor mounting offset (distance from sensor to bin rim)
  float effectiveDistance = distanceCm - sensorOffsetCm;
  if (effectiveDistance < 0) effectiveDistance = 0;

  // Fill = (1 - distance/height) * 100
  float pct = (1.0f - (effectiveDistance / binHeightCm)) * 100.0f;
  return constrain(pct, 0.0f, 100.0f);
}

// Anomaly detection: flag if distance < 2cm or > bin_height + 10cm
bool checkAnomaly(float distanceCm) {
  if (distanceCm < 0) return true;  // sensor failure
  if (distanceCm < SENSOR_MIN_CM) return true;
  if (distanceCm > (binHeightCm + sensorOffsetCm + 10.0f)) return true;
  return false;
}

// =============================================================================
//  BATTERY MONITORING
// =============================================================================

float readBatteryVoltage() {
  long sum = 0;
  for (int i = 0; i < BATT_ADC_SAMPLES; i++) {
    sum += analogRead(BATTERY_ADC_PIN);
    delay(2);
  }
  // actual_voltage = adc_reading * divider_ratio * vref / adc_max
  // With 2x 100K divider: actual = reading * 2 * 3.3 / 4095
  return ((float)sum / (float)BATT_ADC_SAMPLES / ADC_RESOLUTION) * ADC_VREF * BATT_DIVIDER;
}

int batteryPercentage(float voltage) {
  // Piecewise linear approximation of Li-ion discharge curve
  if (voltage >= BATT_FULL_V) return 100;
  if (voltage <= BATT_CRITICAL_V) return 0;
  if (voltage >= 3.90f) return 75 + (int)((voltage - 3.90f) / 0.30f * 25.0f);
  if (voltage >= 3.70f) return 50 + (int)((voltage - 3.70f) / 0.20f * 25.0f);
  if (voltage >= 3.50f) return 25 + (int)((voltage - 3.50f) / 0.20f * 25.0f);
  if (voltage >= 3.30f) return 10 + (int)((voltage - 3.30f) / 0.20f * 15.0f);
  return (int)((voltage - BATT_CRITICAL_V) / 0.30f * 10.0f);
}

bool isOnBattery() {
  // If battery voltage is in a reasonable range, we are on battery power
  // USB power typically shows ~4.5V+ through the divider, or near 0V if no battery
  return (currentBatteryV > 2.5f && currentBatteryV < BATT_FULL_V + 0.1f);
}

// =============================================================================
//  HW-201 IR LID DETECTION
// =============================================================================

bool readLidClosed() {
  // Debounce: take multiple reads and use majority vote
  // HW-201: LOW = object detected = lid closed
  int detectedCount = 0;
  for (int i = 0; i < IR_DEBOUNCE_READS; i++) {
    if (digitalRead(IR_PIN) == LOW) {
      detectedCount++;
    }
    delay(IR_DEBOUNCE_DELAY);
  }
  return (detectedCount >= (IR_DEBOUNCE_READS / 2 + 1));
}

// =============================================================================
//  SENSOR UPDATE (called periodically from loop)
// =============================================================================

void updateSensors() {
  currentLidClosed  = readLidClosed();
  currentBatteryV   = readBatteryVoltage();
  currentBatteryPct = batteryPercentage(currentBatteryV);

  if (WiFi.status() == WL_CONNECTED) {
    currentRssi = WiFi.RSSI();
  }

  if (currentLidClosed) {
    // Read all three sensors sequentially
    currentDistanceLeft   = readDistanceMedian(US1_TRIG_PIN, US1_ECHO_PIN);
    currentDistanceCenter = readDistanceMedian(US2_TRIG_PIN, US2_ECHO_PIN);
    currentDistanceRight  = readDistanceMedian(US3_TRIG_PIN, US3_ECHO_PIN);

    currentFillLeft   = calculateFillPercent(currentDistanceLeft);
    currentFillCenter = calculateFillPercent(currentDistanceCenter);
    currentFillRight  = calculateFillPercent(currentDistanceRight);

    // Fuse: average across valid sensors only
    float sum = 0.0f;
    int   cnt = 0;
    if (currentFillLeft   >= 0) { sum += currentFillLeft;   cnt++; }
    if (currentFillCenter >= 0) { sum += currentFillCenter; cnt++; }
    if (currentFillRight  >= 0) { sum += currentFillRight;  cnt++; }

    if (cnt > 0) {
      currentFillPercent = sum / cnt;
      // Representative distance: use center sensor if valid, else best available
      currentDistanceCm = (currentDistanceCenter >= 0) ? currentDistanceCenter
                        : (currentDistanceLeft   >= 0) ? currentDistanceLeft
                        : currentDistanceRight;
    } else {
      currentFillPercent = -1.0f;
      currentDistanceCm  = -1.0f;
    }
  } else {
    currentDistanceLeft = currentDistanceCenter = currentDistanceRight = -1.0f;
    currentFillLeft     = currentFillCenter     = currentFillRight     = -1.0f;
    currentDistanceCm   = -1.0f;
  }

  currentAnomaly = checkAnomaly(currentDistanceCm);
  lastSensorReadTime = millis();

  Serial.printf("[sensor] L=%.1f%%(%.1fcm) C=%.1f%%(%.1fcm) R=%.1f%%(%.1fcm) | Fused=%.1f%% | Lid=%s | Batt=%.2fV\n",
    currentFillLeft,   currentDistanceLeft,
    currentFillCenter, currentDistanceCenter,
    currentFillRight,  currentDistanceRight,
    currentFillPercent,
    currentLidClosed ? "CLOSED" : "OPEN",
    currentBatteryV);
}

// =============================================================================
//  ADAPTIVE PUBLISH INTERVAL
// =============================================================================

unsigned long getPublishIntervalMs() {
  unsigned long sec;

  if (currentFillPercent < 0) {
    sec = INTERVAL_MEDIUM_SEC;             // unknown fill -> medium rate
  } else if (currentFillPercent > 75.0f) {
    sec = INTERVAL_HIGH_SEC;               // > 75% -> every 1 min
  } else if (currentFillPercent >= 50.0f) {
    sec = INTERVAL_MEDIUM_SEC;             // 50-75% -> every 5 min
  } else {
    sec = INTERVAL_LOW_SEC;                // < 50% -> every 10 min
  }

  // Critical battery: slow down to heartbeat only
  if (currentBatteryV > 0.5f && currentBatteryV < BATT_CRITICAL_V) {
    sec = INTERVAL_HEARTBEAT_SEC;
  }

  return sec * 1000UL;
}

bool shouldPublishImmediately() {
  // Significant fill level change
  if (lastPublishedFill >= 0 && currentFillPercent >= 0) {
    if (abs(currentFillPercent - lastPublishedFill) >= FILL_CHANGE_THRESHOLD) {
      Serial.println("[adaptive] Fill changed significantly — publishing now");
      return true;
    }
  }

  // Lid just closed — publish immediately to capture new fill level
  // Do NOT publish when lid opens (readings are unreliable while bin is in use)
  if (currentLidClosed && !lastLidState) {
    Serial.println("[adaptive] Lid CLOSED — publishing now");
    return true;
  }

  // Battery dropped to low threshold
  if (lastPublishedBatt > BATT_LOW_V && currentBatteryV <= BATT_LOW_V) {
    Serial.println("[adaptive] Battery low — publishing now");
    return true;
  }

  // Heartbeat: enforce max 30 min silence
  if (lastPublishTime > 0 &&
      (millis() - lastPublishTime) >= (INTERVAL_HEARTBEAT_SEC * 1000UL)) {
    Serial.println("[adaptive] Heartbeat interval reached — publishing now");
    return true;
  }

  return false;
}

// =============================================================================
//  WiFi CONNECTION
// =============================================================================

bool connectWifiStation() {
  if (cfgWifiSsid.length() == 0) {
    Serial.println("[wifi] No SSID configured");
    return false;
  }

  Serial.printf("[wifi] Connecting to '%s'...\n", cfgWifiSsid.c_str());
  WiFi.begin(cfgWifiSsid.c_str(), cfgWifiPass.c_str());

  int attempts = 0;
  while (WiFi.status() != WL_CONNECTED && attempts < WIFI_CONNECT_TRIES) {
    delay(500);
    Serial.print(".");
    esp_task_wdt_reset();  // pet watchdog during connection
    attempts++;
  }

  wifiConnected = (WiFi.status() == WL_CONNECTED);

  if (wifiConnected) {
    Serial.printf("\n[wifi] Connected — IP: %s, RSSI: %d dBm\n",
      WiFi.localIP().toString().c_str(), WiFi.RSSI());
    currentRssi = WiFi.RSSI();
  } else {
    Serial.println("\n[wifi] Connection failed");
  }

  return wifiConnected;
}

// =============================================================================
//  MQTT CONNECTION + PUBLISH
// =============================================================================

bool connectMqtt() {
  if (cfgMqttBroker.length() == 0) return false;
  if (mqtt.connected()) {
    mqttConnected = true;
    return true;
  }

  String clientId = "ecoroute-" + cfgDeviceCode + "-" + String(esp_random() % 1000);
  Serial.printf("[mqtt] Connecting to %s:%d as '%s'...\n",
    cfgMqttBroker.c_str(), cfgMqttPort, clientId.c_str());

  mqtt.setServer(cfgMqttBroker.c_str(), cfgMqttPort);
  mqtt.setBufferSize(512);
  mqtt.setKeepAlive(60);

  bool ok;
  if (cfgMqttUser.length() > 0) {
    ok = mqtt.connect(clientId.c_str(), cfgMqttUser.c_str(), cfgMqttPass.c_str());
  } else {
    ok = mqtt.connect(clientId.c_str());
  }

  mqttConnected = ok;
  if (ok) {
    Serial.println("[mqtt] Connected!");
  } else {
    Serial.printf("[mqtt] Failed (rc=%d)\n", mqtt.state());
  }
  return ok;
}

bool publishTelemetry() {
  if (!mqtt.connected()) {
    Serial.println("[mqtt] Not connected — skipping publish");
    return false;
  }

  // Skip publish when lid is open — readings are unreliable while bin is in use
  if (!currentLidClosed) {
    Serial.println("[mqtt] Lid OPEN — skipping publish (unreliable readings)");
    return false;
  }

  // Build JSON payload (matches backend expected format)
  JsonDocument doc;
  doc["device_code"]        = cfgDeviceCode;
  doc["fill_level_percent"] = (currentFillPercent >= 0)
    ? round(currentFillPercent * 10.0f) / 10.0f : 0.0f;
  doc["distance_cm"]        = (currentDistanceCm >= 0)
    ? round(currentDistanceCm * 10.0f) / 10.0f : -1.0f;
  doc["battery_voltage"]    = round(currentBatteryV * 100.0f) / 100.0f;
  doc["signal_strength"]    = currentRssi;
  doc["lid_closed"]         = currentLidClosed;
  doc["anomaly_flag"]       = currentAnomaly;
  doc["firmware_version"]   = FIRMWARE_VERSION;

  // Per-sensor readings for the heatmap (US1=left, US2=center, US3=right)
  JsonObject meta    = doc["metadata"].to<JsonObject>();
  JsonObject sensors = meta["sensors"].to<JsonObject>();

  if (currentFillLeft >= 0) {
    JsonObject s = sensors["left"].to<JsonObject>();
    s["fill_percent"]  = round(currentFillLeft  * 10.0f) / 10.0f;
    s["distance_cm"]   = round(currentDistanceLeft * 10.0f) / 10.0f;
  }
  if (currentFillCenter >= 0) {
    JsonObject s = sensors["center"].to<JsonObject>();
    s["fill_percent"]  = round(currentFillCenter  * 10.0f) / 10.0f;
    s["distance_cm"]   = round(currentDistanceCenter * 10.0f) / 10.0f;
  }
  if (currentFillRight >= 0) {
    JsonObject s = sensors["right"].to<JsonObject>();
    s["fill_percent"]  = round(currentFillRight  * 10.0f) / 10.0f;
    s["distance_cm"]   = round(currentDistanceRight * 10.0f) / 10.0f;
  }

  char payload[640];
  size_t len = serializeJson(doc, payload, sizeof(payload));

  String topic = cfgMqttTopic + cfgDeviceCode;
  bool ok = mqtt.publish(topic.c_str(), payload, len);

  if (ok) {
    lastPublishedFill = currentFillPercent;
    lastPublishedBatt = currentBatteryV;
    lastLidState      = currentLidClosed;
    lastPublishTime   = millis();
    publishCount++;

    Serial.println("-------------------------------------");
    Serial.printf("[publish] #%lu -> %s\n", publishCount, topic.c_str());
    Serial.printf("[publish] %s\n", payload);
    Serial.println("-------------------------------------");
  } else {
    Serial.println("[mqtt] Publish FAILED");
  }

  return ok;
}

// =============================================================================
//  LED STATUS INDICATORS
// =============================================================================

void setLedPattern(LedPattern pattern) {
  currentLedPattern = pattern;
  if (pattern == LED_SOLID) {
    digitalWrite(LED_PIN, HIGH);
    ledState = true;
  } else if (pattern == LED_OFF) {
    digitalWrite(LED_PIN, LOW);
    ledState = false;
  }
}

void updateLed() {
  unsigned long now = millis();
  unsigned long interval;

  switch (currentLedPattern) {
    case LED_FAST_BLINK:
      interval = 150;  // ms
      break;
    case LED_SLOW_BLINK:
      interval = 1000; // ms
      break;
    default:
      return;  // SOLID and OFF are handled in setLedPattern
  }

  if (now - lastLedToggle >= interval) {
    ledState = !ledState;
    digitalWrite(LED_PIN, ledState ? HIGH : LOW);
    lastLedToggle = now;
  }
}

// =============================================================================
//  HTTP BASIC AUTH HELPER
// =============================================================================

bool checkAuth() {
  if (!webServer.authenticate(cfgPortalUser.c_str(), cfgPortalPass.c_str())) {
    webServer.requestAuthentication(BASIC_AUTH, "EcoRoute Smart Bin");
    return false;
  }
  return true;
}

void sendJsonResponse(int code, JsonDocument& doc) {
  char buf[1024];
  size_t len = serializeJson(doc, buf, sizeof(buf));
  webServer.send(code, "application/json", buf);
}

void sendJsonError(int code, const char* message) {
  JsonDocument doc;
  doc["error"] = message;
  sendJsonResponse(code, doc);
}

void sendJsonOk(const char* message) {
  JsonDocument doc;
  doc["status"] = "ok";
  doc["message"] = message;
  sendJsonResponse(200, doc);
}

// =============================================================================
//  CAPTIVE PORTAL — JSON API ENDPOINTS
// =============================================================================

// GET /api/status — current readings + connection info
void handleApiStatus() {
  if (!checkAuth()) return;

  JsonDocument doc;

  // Device info
  doc["device_code"]       = cfgDeviceCode;
  doc["firmware_version"]  = FIRMWARE_VERSION;
  doc["uptime_sec"]        = millis() / 1000;

  // Sensor readings
  doc["fill_level_percent"] = (currentFillPercent >= 0)
    ? round(currentFillPercent * 10.0f) / 10.0f : -1.0f;
  doc["distance_cm"]       = (currentDistanceCm >= 0)
    ? round(currentDistanceCm * 10.0f) / 10.0f : -1.0f;
  doc["lid_closed"]        = currentLidClosed;
  doc["anomaly_flag"]      = currentAnomaly;

  // Battery
  doc["battery_voltage"]   = round(currentBatteryV * 100.0f) / 100.0f;
  doc["battery_percent"]   = currentBatteryPct;

  // Connection
  doc["wifi_connected"]    = wifiConnected;
  doc["wifi_ssid"]         = cfgWifiSsid;
  doc["rssi"]              = currentRssi;
  doc["mqtt_connected"]    = mqttConnected;
  doc["mqtt_broker"]       = cfgMqttBroker;
  doc["mqtt_port"]         = cfgMqttPort;

  // Config
  doc["bin_height_cm"]     = binHeightCm;
  doc["sensor_offset_cm"]  = sensorOffsetCm;
  doc["publish_count"]     = publishCount;
  doc["ap_active"]         = apModeActive;

  // AP broadcast mode
  const char* apModeStr = "always";
  if (cfgApMode == AP_MODE_HIDDEN)    apModeStr = "hidden";
  if (cfgApMode == AP_MODE_ON_DEMAND) apModeStr = "on_demand";
  doc["ap_mode"]           = apModeStr;
  doc["ap_ssid_hidden"]    = (cfgApMode == AP_MODE_HIDDEN);
  doc["ble_active"]        = bleActive;

  sendJsonResponse(200, doc);
}

// POST /api/wifi — configure WiFi credentials
// Body: {"ssid": "...", "password": "..."}
void handleApiWifi() {
  if (!checkAuth()) return;

  if (webServer.method() != HTTP_POST) {
    sendJsonError(405, "Method not allowed");
    return;
  }

  String body = webServer.arg("plain");
  if (body.length() == 0) {
    sendJsonError(400, "Missing JSON body");
    return;
  }

  JsonDocument req;
  DeserializationError err = deserializeJson(req, body);
  if (err) {
    sendJsonError(400, "Invalid JSON");
    return;
  }

  if (req["ssid"].is<const char*>()) {
    cfgWifiSsid = req["ssid"].as<String>();
  } else {
    sendJsonError(400, "Missing 'ssid' field");
    return;
  }

  if (req["password"].is<const char*>()) {
    cfgWifiPass = req["password"].as<String>();
  }

  saveConfig();

  JsonDocument doc;
  doc["status"] = "ok";
  doc["message"] = "WiFi credentials saved. Reboot to apply.";
  doc["ssid"] = cfgWifiSsid;
  sendJsonResponse(200, doc);
}

// POST /api/mqtt — configure MQTT settings
// Body: {"broker": "...", "port": 1883, "username": "...", "password": "...", "topic_prefix": "..."}
void handleApiMqtt() {
  if (!checkAuth()) return;

  if (webServer.method() != HTTP_POST) {
    sendJsonError(405, "Method not allowed");
    return;
  }

  String body = webServer.arg("plain");
  if (body.length() == 0) {
    sendJsonError(400, "Missing JSON body");
    return;
  }

  JsonDocument req;
  DeserializationError err = deserializeJson(req, body);
  if (err) {
    sendJsonError(400, "Invalid JSON");
    return;
  }

  if (req["broker"].is<const char*>()) cfgMqttBroker = req["broker"].as<String>();
  if (req["port"].is<int>())           cfgMqttPort   = req["port"].as<int>();
  if (req["username"].is<const char*>()) cfgMqttUser = req["username"].as<String>();
  if (req["password"].is<const char*>()) cfgMqttPass = req["password"].as<String>();
  if (req["topic_prefix"].is<const char*>()) cfgMqttTopic = req["topic_prefix"].as<String>();

  saveConfig();

  // Reconnect MQTT with new settings
  mqtt.disconnect();
  mqttConnected = false;
  connectMqtt();

  JsonDocument doc;
  doc["status"] = "ok";
  doc["message"] = "MQTT settings saved and reconnected.";
  doc["broker"] = cfgMqttBroker;
  doc["port"] = cfgMqttPort;
  doc["connected"] = mqttConnected;
  sendJsonResponse(200, doc);
}

// POST /api/device — configure device code + bin height
// Body: {"device_code": "...", "bin_height_cm": 100.0, "sensor_offset_cm": 2.0}
void handleApiDevice() {
  if (!checkAuth()) return;

  if (webServer.method() != HTTP_POST) {
    sendJsonError(405, "Method not allowed");
    return;
  }

  String body = webServer.arg("plain");
  if (body.length() == 0) {
    sendJsonError(400, "Missing JSON body");
    return;
  }

  JsonDocument req;
  DeserializationError err = deserializeJson(req, body);
  if (err) {
    sendJsonError(400, "Invalid JSON");
    return;
  }

  if (req["device_code"].is<const char*>()) {
    cfgDeviceCode = req["device_code"].as<String>();
    apSsid = "ecoroute-" + cfgDeviceCode;
  }
  if (req["bin_height_cm"].is<float>()) {
    float h = req["bin_height_cm"].as<float>();
    if (h >= 10.0f && h <= 500.0f) {
      binHeightCm = h;
    } else {
      sendJsonError(400, "bin_height_cm must be between 10 and 500");
      return;
    }
  }
  if (req["sensor_offset_cm"].is<float>()) {
    float o = req["sensor_offset_cm"].as<float>();
    if (o >= 0.0f && o <= 50.0f) {
      sensorOffsetCm = o;
    }
  }

  saveConfig();

  JsonDocument doc;
  doc["status"] = "ok";
  doc["message"] = "Device settings saved.";
  doc["device_code"] = cfgDeviceCode;
  doc["bin_height_cm"] = binHeightCm;
  doc["sensor_offset_cm"] = sensorOffsetCm;
  sendJsonResponse(200, doc);
}

// POST /api/reboot — save and restart
void handleApiReboot() {
  if (!checkAuth()) return;

  sendJsonOk("Rebooting in 2 seconds...");
  delay(2000);
  ESP.restart();
}

// POST /api/force-publish — force an immediate telemetry publish
void handleApiForcePublish() {
  if (!checkAuth()) return;

  updateSensors();
  bool ok = publishTelemetry();

  JsonDocument doc;
  doc["status"] = ok ? "ok" : "error";
  doc["message"] = ok ? "Telemetry published" : "Publish failed (MQTT not connected)";
  doc["fill_level_percent"] = currentFillPercent;
  doc["distance_cm"] = currentDistanceCm;
  sendJsonResponse(ok ? 200 : 503, doc);
}

// POST /api/factory-reset — clear all NVS and reboot
void handleApiFactoryReset() {
  if (!checkAuth()) return;

  prefs.begin("ecoroute", false);
  prefs.clear();
  prefs.end();

  Serial.println("[config] Factory reset — NVS cleared");
  sendJsonOk("Factory reset complete. Rebooting in 3 seconds...");
  delay(3000);
  ESP.restart();
}

// POST /api/ap-mode — configure AP broadcast mode
// Body: {"mode": "always"|"hidden"|"on_demand"} or {"action": "enable"|"disable"}
void handleApiApMode() {
  if (!checkAuth()) return;

  String body = webServer.arg("plain");
  if (body.length() == 0) {
    sendJsonError(400, "Missing JSON body");
    return;
  }

  JsonDocument req;
  if (deserializeJson(req, body)) {
    sendJsonError(400, "Invalid JSON");
    return;
  }

  // Handle immediate enable/disable action
  if (req["action"].is<const char*>()) {
    String action = req["action"].as<String>();
    if (action == "enable") {
      enableOnDemandAp();
      sendJsonOk("AP enabled (on-demand, will auto-disable after timeout)");
    } else if (action == "disable") {
      stopAccessPoint();
      sendJsonOk("AP disabled");
    } else {
      sendJsonError(400, "Unknown action. Use 'enable' or 'disable'");
    }
    return;
  }

  // Handle mode change
  if (req["mode"].is<const char*>()) {
    String mode = req["mode"].as<String>();
    uint8_t newMode;

    if (mode == "always") {
      newMode = AP_MODE_ALWAYS;
    } else if (mode == "hidden") {
      newMode = AP_MODE_HIDDEN;
    } else if (mode == "on_demand") {
      newMode = AP_MODE_ON_DEMAND;
    } else {
      sendJsonError(400, "Unknown mode. Use 'always', 'hidden', or 'on_demand'");
      return;
    }

    cfgApMode = newMode;
    prefs.begin("ecoroute", false);
    prefs.putUChar("apMode", cfgApMode);
    prefs.end();
    cfgManager.setApMode(cfgApMode);

    // Apply immediately
    if (cfgApMode == AP_MODE_ON_DEMAND) {
      // Don't stop AP right now — the client is connected to it!
      // Just save the mode; it takes effect on next boot
      JsonDocument doc;
      doc["status"] = "ok";
      doc["message"] = "AP mode set to on-demand. Takes effect on next reboot.";
      doc["ap_mode"] = "on_demand";
      sendJsonResponse(200, doc);
    } else {
      // For always/hidden, restart AP with new visibility
      if (apModeActive) stopAccessPoint();
      startAccessPoint();

      JsonDocument doc;
      doc["status"] = "ok";
      doc["message"] = mode == "hidden"
        ? "AP mode set to hidden. SSID is no longer broadcast."
        : "AP mode set to always visible.";
      doc["ap_mode"] = mode;
      doc["ap_active"] = apModeActive;
      sendJsonResponse(200, doc);
    }
    return;
  }

  sendJsonError(400, "Provide 'mode' or 'action' field");
}

// =============================================================================
//  routeOS WEB UI
// =============================================================================

void handleRoot() {
  // ── Live state ──────────────────────────────────────────────────────
  float fp      = currentFillPercent < 0 ? 0 : currentFillPercent;
  float dist    = currentDistanceCm  < 0 ? 0 : currentDistanceCm;
  int   fillInt = (int)fp;
  int   battInt = currentBatteryPct;

  String fillPct    = String(fillInt);
  String distCm     = String(dist, 1);
  String battV      = String(currentBatteryV, 2);
  String battPctStr = String(battInt);
  String lidStr     = currentLidClosed ? "Closed" : "Open";
  String ssidStr    = cfgWifiSsid.length() > 0 ? cfgWifiSsid : "&mdash;";
  String rssiStr    = wifiConnected ? (String(currentRssi) + " dBm") : "&mdash;";

  // Fill bar color — chartreuse when filling, terracotta when critical
  String fillColor = "#7FA67E";
  if (fp > 80) fillColor = "#C97A3C";
  else if (fp > 50) fillColor = "#C8E03A";

  // Next publish countdown
  unsigned long nowMs    = millis();
  unsigned long elapsed  = (lastPublishTime > 0) ? (nowMs - lastPublishTime) : nowMs;
  unsigned long interval = getPublishIntervalMs();
  long          remaining = (long)(interval) - (long)(elapsed);
  String nextPubStr = "&mdash;";
  if (remaining > 0) {
    unsigned long remSec = remaining / 1000UL;
    unsigned long m = remSec / 60;
    unsigned long s = remSec % 60;
    nextPubStr = String(m) + "m " + String(s) + "s";
  } else {
    nextPubStr = "now";
  }

  String mqttColor = mqttConnected ? "#7FA67E" : "#52584E";
  String mqttStr   = mqttConnected ? "Connected" : "Disconnected";
  String liveColor = wifiConnected ? "#C8E03A" : "#52584E";

  String html = R"rawliteral(<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<meta http-equiv="refresh" content="30">
<title>routeOS &mdash; )rawliteral" + cfgDeviceCode + R"rawliteral(</title>
<style>
  @import url('https://fonts.googleapis.com/css2?family=JetBrains+Mono:wght@400;500;700&display=swap');
  *,*::before,*::after{box-sizing:border-box;margin:0;padding:0}
  :root{
    --ink:#0B1712;--ink-raised:#111e18;--ink-muted:#1a2620;
    --paper:#F0EAD6;--paper-dim:#9CA896;--paper-faint:#52584E;
    --gold:#C8E03A;--moss:#7FA67E;--terracotta:#C97A3C;
    --rule:#1F2A23;--f:'JetBrains Mono',monospace;
  }
  html,body{height:100%;background:var(--ink);color:var(--paper);font-family:var(--f);font-size:12px;-webkit-font-smoothing:antialiased}
  body{display:flex;flex-direction:column;min-height:100vh}

  /* Breadcrumb */
  .bc{background:var(--ink-raised);border-bottom:1px solid var(--rule);padding:10px 20px;display:flex;align-items:center;justify-content:space-between}
  .bc-path{color:var(--paper-faint);font-size:10px;letter-spacing:.08em}
  .bc-path b{color:var(--paper-dim)}
  .bc-badge{background:var(--gold);color:var(--ink);padding:2px 8px;border-radius:2px;font-weight:700;font-size:9px;letter-spacing:.2em}

  /* Header */
  .hdr{padding:14px 20px;border-bottom:1px solid var(--rule);display:flex;align-items:center;justify-content:space-between}
  .hdr-left{display:flex;align-items:center;gap:10px}
  .ros{background:var(--gold);color:var(--ink);font-weight:700;font-size:11px;padding:4px 9px;border-radius:3px;letter-spacing:.04em}
  .ver{font-size:13px;color:var(--paper-dim)}
  .live{display:flex;align-items:center;gap:6px;font-size:10px;letter-spacing:.15em;text-transform:uppercase}
  .live-dot{width:7px;height:7px;border-radius:50%;background:)rawliteral" + liveColor + R"rawliteral(;animation:)rawliteral" + String(wifiConnected ? "blink 2s infinite" : "none") + R"rawliteral(}
  @keyframes blink{0%,100%{opacity:1}50%{opacity:.3}}

  /* Device row */
  .dr{padding:10px 20px;border-bottom:1px solid var(--rule);display:flex;align-items:center;gap:0;background:var(--ink-raised);overflow-x:auto}
  .dr-item{display:flex;align-items:center;gap:7px;padding-right:20px;margin-right:20px;border-right:1px solid var(--rule);white-space:nowrap}
  .dr-item:last-child{border-right:none}
  .dr-lbl{color:var(--paper-faint);font-size:9px;letter-spacing:.18em;text-transform:uppercase}
  .dr-val{color:var(--paper);font-size:12px}

  /* Content */
  .content{flex:1;padding:16px 20px;display:flex;flex-direction:column;gap:14px;overflow-y:auto}

  /* Fill card */
  .fill-card{background:var(--ink-raised);border:1px solid var(--rule);border-radius:6px;padding:18px}
  .fill-hdr{display:flex;align-items:flex-start;justify-content:space-between;margin-bottom:14px}
  .fill-lbl{color:var(--paper-faint);font-size:9px;letter-spacing:.18em;text-transform:uppercase;margin-bottom:6px}
  .fill-num{font-size:clamp(3rem,14vw,4.8rem);font-weight:500;line-height:1;letter-spacing:-.03em;color:)rawliteral" + fillColor + R"rawliteral(}
  .fill-right{text-align:right}
  .fill-dist{font-size:1.5rem;color:var(--paper-dim);line-height:1;letter-spacing:-.02em}
  .fill-unit{color:var(--paper-faint);font-size:9px;letter-spacing:.12em;text-transform:uppercase;margin-top:3px}
  .bar-wrap{height:8px;background:var(--ink-muted);border-radius:4px;overflow:hidden;margin-bottom:7px}
  .bar-fill{height:100%;border-radius:4px;background:)rawliteral" + fillColor + R"rawliteral(;width:)rawliteral" + fillPct + R"rawliteral(%}
  .bar-lbls{display:flex;justify-content:space-between;font-size:9px;letter-spacing:.1em;text-transform:uppercase;color:var(--paper-faint)}
  .bar-lbls .mid{color:var(--paper-dim)}

  /* Sensors */
  .s-hdr{color:var(--paper-faint);font-size:9px;letter-spacing:.18em;text-transform:uppercase;margin-bottom:8px}
  .s-box{background:var(--ink-raised);border:1px solid var(--rule);border-radius:6px;padding:2px 14px}
  .s-row{display:flex;align-items:center;gap:10px;padding:9px 0;border-bottom:1px solid var(--rule)}
  .s-row:last-child{border-bottom:none}
  .s-name{width:30px;flex-shrink:0;font-size:9px;letter-spacing:.15em;text-transform:uppercase;color:var(--paper-faint)}
  .s-name.on{color:var(--gold)}
  .s-bw{flex:1;height:4px;background:var(--ink-muted);border-radius:2px;overflow:hidden}
  .s-b{height:100%;border-radius:2px;background:)rawliteral" + fillColor + R"rawliteral(}
  .s-pct{width:34px;text-align:right;flex-shrink:0;font-size:11px;color:var(--paper-dim)}
  .s-pct.dim{color:var(--paper-faint)}
  .s-cm{width:54px;text-align:right;flex-shrink:0;font-size:10px;color:var(--paper-faint)}
  .s-cm.on{color:var(--paper-dim)}

  /* Stats */
  .stats{display:grid;grid-template-columns:repeat(3,1fr);gap:10px}
  .stat{background:var(--ink-raised);border:1px solid var(--rule);border-radius:5px;padding:12px}
  .stat-lbl{color:var(--paper-faint);font-size:9px;letter-spacing:.18em;text-transform:uppercase;margin-bottom:7px}
  .stat-val{font-size:13px;color:var(--paper)}

  /* Tab bar */
  .tabs{background:var(--ink-raised);border-top:1px solid var(--rule);display:flex;overflow-x:auto;scrollbar-width:none;flex-shrink:0}
  .tabs::-webkit-scrollbar{display:none}
  .tab{flex:1;min-width:72px;padding:13px 6px;text-align:center;font-size:8px;letter-spacing:.16em;text-transform:uppercase;color:var(--paper-faint);border-top:2px solid transparent;cursor:default;white-space:nowrap}
  .tab.on{color:var(--gold);border-top-color:var(--gold)}
</style>
</head>
<body>

<div class="bc">
  <div class="bc-path">192.168.4.1 / <b>)rawliteral" + cfgDeviceCode + R"rawliteral(</b></div>
  <div class="bc-badge">ROUTEOS</div>
</div>

<div class="hdr">
  <div class="hdr-left">
    <div class="ros">rOS</div>
    <div class="ver">routeOS v)rawliteral" + String(FIRMWARE_VERSION) + R"rawliteral(</div>
  </div>
  <div class="live" style="color:)rawliteral" + liveColor + R"rawliteral(">
    <div class="live-dot"></div>LIVE
  </div>
</div>

<div class="dr">
  <div class="dr-item">
    <span class="dr-lbl">Device</span>
    <span class="dr-val">)rawliteral" + cfgDeviceCode + R"rawliteral(</span>
  </div>
  <div class="dr-item">
    <span class="dr-lbl">MQTT</span>
    <span class="dr-val" style="color:)rawliteral" + mqttColor + R"rawliteral(">)rawliteral" + mqttStr + R"rawliteral(</span>
  </div>
  <div class="dr-item">
    <span class="dr-lbl">Signal</span>
    <span class="dr-val">)rawliteral" + rssiStr + R"rawliteral(</span>
  </div>
</div>

<div class="content">

  <div class="fill-card">
    <div class="fill-hdr">
      <div>
        <div class="fill-lbl">Fill Level</div>
        <div class="fill-num">)rawliteral" + fillPct + R"rawliteral(%</div>
      </div>
      <div class="fill-right">
        <div class="fill-dist">)rawliteral" + distCm + R"rawliteral(</div>
        <div class="fill-unit">cm from sensor</div>
      </div>
    </div>
    <div class="bar-wrap"><div class="bar-fill"></div></div>
    <div class="bar-lbls">
      <span>EMPTY</span>
      <span class="mid">)rawliteral" + fillPct + R"rawliteral(%</span>
      <span>FULL</span>
    </div>
  </div>

  <div>
    <div class="s-hdr">Sensors &middot; HC-SR04</div>
    <div class="s-box">
      <div class="s-row">
        <span class="s-name )rawliteral" + String(currentFillLeft >= 0 ? "on" : "") + R"rawliteral(">LEFT</span>
        <div class="s-bw"><div class="s-b" style="width:)rawliteral" + String(currentFillLeft >= 0 ? String((int)currentFillLeft) : "0") + R"rawliteral(%"></div></div>
        <span class="s-pct )rawliteral" + String(currentFillLeft >= 0 ? "" : "dim") + R"rawliteral(">)rawliteral" + String(currentFillLeft >= 0 ? (String((int)currentFillLeft) + "%") : "&mdash;") + R"rawliteral(</span>
        <span class="s-cm )rawliteral" + String(currentDistanceLeft >= 0 ? "on" : "") + R"rawliteral(">)rawliteral" + String(currentDistanceLeft >= 0 ? (String(currentDistanceLeft, 1) + " cm") : "&mdash;") + R"rawliteral(</span>
      </div>
      <div class="s-row">
        <span class="s-name on">CTR</span>
        <div class="s-bw"><div class="s-b" style="width:)rawliteral" + fillPct + R"rawliteral(%"></div></div>
        <span class="s-pct">)rawliteral" + fillPct + R"rawliteral(%</span>
        <span class="s-cm on">)rawliteral" + distCm + R"rawliteral( cm</span>
      </div>
      <div class="s-row">
        <span class="s-name )rawliteral" + String(currentFillRight >= 0 ? "on" : "") + R"rawliteral(">RIGHT</span>
        <div class="s-bw"><div class="s-b" style="width:)rawliteral" + String(currentFillRight >= 0 ? String((int)currentFillRight) : "0") + R"rawliteral(%"></div></div>
        <span class="s-pct )rawliteral" + String(currentFillRight >= 0 ? "" : "dim") + R"rawliteral(">)rawliteral" + String(currentFillRight >= 0 ? (String((int)currentFillRight) + "%") : "&mdash;") + R"rawliteral(</span>
        <span class="s-cm )rawliteral" + String(currentDistanceRight >= 0 ? "on" : "") + R"rawliteral(">)rawliteral" + String(currentDistanceRight >= 0 ? (String(currentDistanceRight, 1) + " cm") : "&mdash;") + R"rawliteral(</span>
      </div>
    </div>
  </div>

  <div class="stats">
    <div class="stat">
      <div class="stat-lbl">LID</div>
      <div class="stat-val" style="color:)rawliteral" + String(currentLidClosed ? "var(--moss)" : "var(--gold)") + R"rawliteral(">)rawliteral" + lidStr + R"rawliteral(</div>
    </div>
    <div class="stat">
      <div class="stat-lbl">WIFI</div>
      <div class="stat-val" style="color:)rawliteral" + String(wifiConnected ? "var(--moss)" : "var(--terracotta)") + R"rawliteral(">)rawliteral" + String(wifiConnected ? "Connected" : "No conn.") + R"rawliteral(</div>
    </div>
    <div class="stat">
      <div class="stat-lbl">NEXT PUB</div>
      <div class="stat-val">)rawliteral" + nextPubStr + R"rawliteral(</div>
    </div>
  </div>

</div>

<div class="tabs">
  <div class="tab on">Dashboard</div>
  <div class="tab">Diagnostics</div>
  <div class="tab">Calibrate</div>
  <div class="tab">Config</div>
  <div class="tab">MQTT</div>
  <div class="tab">Security</div>
</div>

</body>
</html>)rawliteral";

  webServer.send(200, "text/html; charset=UTF-8", html);
}

// Captive portal redirect for any unknown URL
void handleNotFound() {
  // Redirect unknown URLs to the routeOS portal UI
  webServer.sendHeader("Location", "http://192.168.4.1/");
  webServer.send(302, "text/plain", "Redirecting to routeOS portal...");
}

// =============================================================================
//  POWER MANAGEMENT
// =============================================================================

void enterDeepSleep(unsigned long durationUs) {
  Serial.printf("[power] Entering deep sleep for %lu seconds\n", durationUs / 1000000UL);
  Serial.flush();

  // Disconnect cleanly
  mqtt.disconnect();
  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);

  // Configure wake-up timer
  esp_sleep_enable_timer_wakeup(durationUs);

  // Enter deep sleep
  esp_deep_sleep_start();
  // Execution stops here; on wake, setup() runs from the start
}

void enterLightSleep(unsigned long durationMs) {
  Serial.printf("[power] Light sleep for %lu ms\n", durationMs);
  Serial.flush();

  esp_sleep_enable_timer_wakeup(durationMs * 1000ULL);
  esp_light_sleep_start();

  // Execution resumes here after wake
  Serial.println("[power] Woke from light sleep");
}

bool shouldDeepSleep() {
  // Deep sleep when: fill < 30%, on battery, WiFi+MQTT working, not in AP-only mode
  if (!isOnBattery()) return false;
  if (apModeActive && !wifiConnected) return false;  // need portal accessible
  if (currentFillPercent < 0) return false;           // unknown fill — stay awake
  if (currentFillPercent >= DEEP_SLEEP_FILL_THRESHOLD) return false;
  if (currentBatteryV < BATT_CRITICAL_V) return false; // critical battery -> stay awake for alerts
  return true;
}

// =============================================================================
//  CAPTIVE PORTAL / AP MODE SETUP
// =============================================================================

void startAccessPoint(bool hidden) {
  apSsid = "ecoroute-" + cfgDeviceCode;

  // Use AP+STA mode so we can serve the portal AND connect to WiFi
  WiFi.mode(WIFI_AP_STA);

  // hidden=true: SSID not broadcast (WiFi Direct-like — only BLE or direct connect)
  // max_connection=4, beacon_interval=100ms are ESP32 defaults
  WiFi.softAP(apSsid.c_str(), NULL, AP_CHANNEL, hidden ? 1 : 0);

  if (hidden) {
    Serial.printf("[ap] Started '%s' (HIDDEN) — Portal at http://192.168.4.1\n", apSsid.c_str());
  } else {
    Serial.printf("[ap] Started '%s' — Portal at http://192.168.4.1\n", apSsid.c_str());
  }
  apModeActive = true;

  // DNS server for captive portal redirect (all domains -> 192.168.4.1)
  dnsServer.start(DNS_PORT, "*", WiFi.softAPIP());
}

void startAccessPoint() {
  startAccessPoint(cfgApMode == AP_MODE_HIDDEN);
}

void stopAccessPoint() {
  if (!apModeActive) return;

  dnsServer.stop();
  WiFi.softAPdisconnect(true);

  // Switch to STA-only if we have WiFi configured, otherwise keep AP+STA
  if (cfgWifiSsid.length() > 0) {
    WiFi.mode(WIFI_STA);
  }

  apModeActive = false;
  apOnDemandActive = false;
  Serial.println("[ap] Access point stopped");
}

void enableOnDemandAp() {
  if (apModeActive) return;  // already running

  Serial.println("[ap] On-demand AP enabled (will auto-disable after timeout)");
  startAccessPoint(false);  // visible for on-demand
  apOnDemandActive = true;
  apOnDemandStart = millis();
}

void setupWebServer() {
  // routeOS portal UI
  webServer.on("/",                  HTTP_GET,  handleRoot);

  // JSON API endpoints
  webServer.on("/api/status",        HTTP_GET,  handleApiStatus);
  webServer.on("/api/wifi",          HTTP_POST, handleApiWifi);
  webServer.on("/api/mqtt",          HTTP_POST, handleApiMqtt);
  webServer.on("/api/device",        HTTP_POST, handleApiDevice);
  webServer.on("/api/reboot",        HTTP_POST, handleApiReboot);
  webServer.on("/api/force-publish", HTTP_POST, handleApiForcePublish);
  webServer.on("/api/factory-reset", HTTP_POST, handleApiFactoryReset);
  webServer.on("/api/ap-mode",      HTTP_POST, handleApiApMode);

  webServer.onNotFound(handleNotFound);
  webServer.begin();

  Serial.println("[web] JSON API server started on port 80");
}

// =============================================================================
//  WATCHDOG TIMER
// =============================================================================

void initWatchdog() {
#if ESP_IDF_VERSION_MAJOR >= 5
  esp_task_wdt_config_t wdt_config = {
    .timeout_ms = WDT_TIMEOUT_SEC * 1000,
    .idle_core_mask = (1 << portNUM_PROCESSORS) - 1,
    .trigger_panic = true
  };
  esp_task_wdt_reconfigure(&wdt_config);
#else
  esp_task_wdt_init(WDT_TIMEOUT_SEC, true);
#endif
  esp_task_wdt_add(NULL);  // add current task to watchdog
  Serial.printf("[wdt] Watchdog configured — %d second timeout\n", WDT_TIMEOUT_SEC);
}

// =============================================================================
//  SETUP
// =============================================================================

void setup() {
  Serial.begin(115200);
  delay(100);

  // Initialize GPIO
  pinMode(LED_PIN, OUTPUT);
  // All three ultrasonic sensors
  pinMode(US1_TRIG_PIN, OUTPUT); pinMode(US1_ECHO_PIN, INPUT);
  pinMode(US2_TRIG_PIN, OUTPUT); pinMode(US2_ECHO_PIN, INPUT);
  pinMode(US3_TRIG_PIN, OUTPUT); pinMode(US3_ECHO_PIN, INPUT);
  pinMode(IR_PIN, INPUT);
  analogSetAttenuation(ADC_11db);  // full 0-3.3V range on ADC

  // Indicate boot with fast blink
  setLedPattern(LED_FAST_BLINK);

  // Load persistent configuration
  loadConfig();
  apSsid = "ecoroute-" + cfgDeviceCode;

  Serial.println();
  Serial.println("========================================");
  Serial.println("  EcoRoute Smart Bin v" FIRMWARE_VERSION);
  Serial.printf( "  Device:   %s\n", cfgDeviceCode.c_str());
  Serial.println("  Sensor:   HC-SR04 + HW-201 IR");
  Serial.println("  Power:    2x 18650 via TP4056");
  Serial.printf( "  Portal:   %s @ 192.168.4.1\n", apSsid.c_str());
  Serial.println("========================================");
  Serial.println();

  // Initialize watchdog timer (5 minute timeout)
  initWatchdog();

  // Initial sensor reading for self-test
  updateSensors();
  Serial.printf("[self-test] Distance: %.1f cm, Fill: %.1f%%\n",
    currentDistanceCm, currentFillPercent);
  Serial.printf("[self-test] Lid: %s, Battery: %.2fV (%d%%)\n",
    currentLidClosed ? "CLOSED" : "OPEN",
    currentBatteryV, currentBatteryPct);

  // Start AP for captive portal based on configured mode
  bool isConfigured = cfgWifiSsid.length() > 0;
  if (cfgApMode == AP_MODE_ON_DEMAND && isConfigured) {
    // On-demand mode with WiFi configured: don't start AP, use STA-only
    Serial.println("[ap] On-demand mode — AP disabled (use BLE to enable)");
    WiFi.mode(WIFI_STA);
  } else {
    // ALWAYS or HIDDEN mode (or unconfigured device always gets AP for first setup)
    startAccessPoint();
  }

  // Try connecting to configured WiFi
  if (cfgWifiSsid.length() > 0) {
    setLedPattern(LED_FAST_BLINK);
    if (connectWifiStation()) {
      // Connected to WiFi — try MQTT
      if (connectMqtt()) {
        // Publish initial reading
        publishTelemetry();
        setLedPattern(LED_SOLID);
      }
    }
  }

  // If no WiFi configured or connection failed, show AP mode pattern
  if (!wifiConnected) {
    setLedPattern(LED_SLOW_BLINK);
    Serial.println("[wifi] No WiFi connection — use captive portal to configure");
  }

  // Start web server (works in both AP and STA mode)
  setupWebServer();

  // Start BLE provisioning (allows mobile app to configure device via Bluetooth)
  cfgManager.begin();
  String bleName = String(BLE_DEVICE_NAME_PREFIX) + cfgDeviceCode;
  bleProvisioning.begin(&cfgManager, bleName.c_str());
  bleActive = true;
  Serial.printf("[ble] Provisioning active as '%s'\n", bleName.c_str());

  Serial.println("[boot] Setup complete\n");
}

// =============================================================================
//  MAIN LOOP
// =============================================================================

void loop() {
  unsigned long now = millis();

  // Pet the watchdog
  esp_task_wdt_reset();

  // Update LED pattern
  updateLed();

  // Process captive portal DNS + web requests
  dnsServer.processNextRequest();
  webServer.handleClient();

  // MQTT keepalive
  if (mqtt.connected()) {
    mqtt.loop();
  }

  // ---- BLE provisioning commands ----
  if (bleActive) {
    // Update BLE status characteristic so mobile app can read current state
    if (bleProvisioning.isClientConnected()) {
      bleProvisioning.updateStatus(currentBatteryV, wifiConnected);
    }

    // Handle save+restart command from mobile app
    if (bleProvisioning.hasSaveCommand()) {
      Serial.println("[ble] Save command received — reloading config and restarting");
      cfgManager.markConfigured();
      bleProvisioning.clearCommands();
      bleProvisioning.stop();
      delay(500);
      ESP.restart();
    }

    // Handle force-publish command from mobile app
    if (bleProvisioning.hasForceReportCommand()) {
      Serial.println("[ble] Force publish command received");
      bleProvisioning.clearCommands();
      updateSensors();
      publishTelemetry();
    }

    // Handle factory reset command from mobile app
    if (bleProvisioning.hasFactoryResetCommand()) {
      Serial.println("[ble] Factory reset command received");
      bleProvisioning.clearCommands();
      bleProvisioning.stop();
      cfgManager.factoryReset();
      delay(500);
      ESP.restart();
    }

    // Handle enable-AP command (WiFi Direct-like on-demand)
    if (bleProvisioning.hasEnableApCommand()) {
      Serial.println("[ble] Enable AP command received");
      bleProvisioning.clearCommands();
      enableOnDemandAp();
    }

    // Handle disable-AP command
    if (bleProvisioning.hasDisableApCommand()) {
      Serial.println("[ble] Disable AP command received");
      bleProvisioning.clearCommands();
      stopAccessPoint();
    }

    // Handle AP mode change command
    int8_t newApMode = bleProvisioning.getPendingApMode();
    if (newApMode >= 0) {
      Serial.printf("[ble] AP mode change to %d\n", newApMode);
      bleProvisioning.clearCommands();
      cfgApMode = (uint8_t)newApMode;
      cfgManager.setApMode(cfgApMode);

      // Also persist via main prefs for loadConfig() on next boot
      prefs.begin("ecoroute", false);
      prefs.putUChar("apMode", cfgApMode);
      prefs.end();

      // Apply immediately
      if (cfgApMode == AP_MODE_ON_DEMAND) {
        stopAccessPoint();
      } else {
        if (apModeActive) stopAccessPoint();
        startAccessPoint();
      }
    }
  }

  // ---- On-demand AP timeout ----
  if (apOnDemandActive && (now - apOnDemandStart >= AP_ON_DEMAND_TIMEOUT_MS)) {
    Serial.println("[ap] On-demand timeout — disabling AP");
    stopAccessPoint();
  }

  // ---- Reconnection (non-blocking, every 30s) ----
  if (now - lastReconnectTime >= RECONNECT_INTERVAL_MS) {
    if (cfgWifiSsid.length() > 0 && WiFi.status() != WL_CONNECTED) {
      wifiConnected = false;
      setLedPattern(LED_FAST_BLINK);
      Serial.println("[wifi] Attempting reconnection...");
      WiFi.begin(cfgWifiSsid.c_str(), cfgWifiPass.c_str());
      // Non-blocking: check result next iteration
    }

    if (WiFi.status() == WL_CONNECTED) {
      if (!wifiConnected) {
        wifiConnected = true;
        Serial.printf("[wifi] Reconnected — IP: %s\n",
          WiFi.localIP().toString().c_str());
      }
      if (!mqtt.connected()) {
        connectMqtt();
      }
      if (mqttConnected) {
        setLedPattern(LED_SOLID);
      }
    }

    lastReconnectTime = now;
  }

  // ---- Periodic sensor read (every 10s) ----
  if (now - lastSensorReadTime >= SENSOR_READ_INTERVAL_MS) {
    updateSensors();

    // Check for conditions requiring immediate publish
    if (shouldPublishImmediately()) {
      digitalWrite(LED_PIN, HIGH);
      publishTelemetry();
      updateLed();  // restore LED pattern
      // Update lid state even if publish failed (prevent re-triggering)
      lastLidState = currentLidClosed;
    }
  }

  // ---- Adaptive interval publish ----
  unsigned long interval = getPublishIntervalMs();
  if (lastPublishTime > 0 && (now - lastPublishTime >= interval)) {
    updateSensors();
    digitalWrite(LED_PIN, HIGH);
    publishTelemetry();
    updateLed();
  }

  // ---- First publish after boot ----
  if (lastPublishTime == 0 && mqttConnected && now > 5000) {
    updateSensors();
    publishTelemetry();
  }

  // ---- Power management ----
  if (lastPublishTime > 0 && shouldDeepSleep()) {
    // Deep sleep between readings when fill < 30% and on battery
    enterDeepSleep(DEEP_SLEEP_DURATION_US);
    // Never reaches here — setup() runs on wake
  } else if (isOnBattery() && currentFillPercent >= 0 &&
             currentFillPercent < 50.0f && !apModeActive) {
    // Light sleep when on battery, fill moderate, and not serving portal
    // Only sleep for short periods to remain responsive
    // (Skip if portal is being actively used)
  }

  // Short delay to prevent busy-looping; fast enough for web server + MQTT
  delay(10);
}
