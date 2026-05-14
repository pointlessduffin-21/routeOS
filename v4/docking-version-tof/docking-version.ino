/*
 * EcoRoute Smart Bin — routeOS v4.0.0-v4 (Docking Station, Multi-Sensor, GPIO-Configurable + Simulator Mode)
 *
 * Board:    ESP32-WROOM-32U (30-pin devkit, U.FL external antenna, 3.3V logic)
 * Sensors:  HC-SR04 primary (5V, ECHO needs voltage divider!) +
 *           RCWL-1604 secondary (3.3V native) +
 *           CJMCU-531 / VL53L0X ToF tertiary (I2C: SDA=GPIO4, SCL=GPIO22)
 * Power:    USB / 5V supply (solar + TP4056 + 18650 in field deployment)
 *
 * Docking-station design:
 *   The ESP32 + sensors are fixed to the PVC frame above the bin slot.
 *   The inner liner bin is placed into the dock. Fill is only reported when
 *   the bin is present (avg distance <= binHeight x absentThreshold).
 *   No lid sensor needed — docking state replaces lid detection.
 *
 * Wiring (3-sensor config):
 *   HC-SR04  (PRIMARY):    VCC->Vin(5V), GND->GND, TRIG->GPIO32
 *                          ECHO->GPIO35 via 1k/2k divider (5V->3.3V REQUIRED)
 *                          [ ECHO(5V) --- 1kOhm --- GPIO35 --- 2kOhm --- GND ]
 *   RCWL-1604 (SECONDARY): VCC->3V3, GND->GND, TRIG->GPIO19, ECHO->GPIO21
 *   CJMCU-531/VL53L0X (TERTIARY, I2C):
 *                          VCC->3V3, GND->GND, SDA->GPIO4, SCL->GPIO22
 *                          (XSHUT and GPIO1 pins are optional — leave unconnected)
 *   Status LED:            GPIO2 (onboard)
 *   Cal/Boot button:       GPIO0 (BOOT)
 */

#include <WiFi.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>
#include <Preferences.h>
#include <base64.h>
#include <SPIFFS.h>
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#include <BLE2902.h>
#include <Update.h>
#include <HTTPClient.h>
#include "esp_ota_ops.h"
#include <Wire.h>
#include <VL53L0X.h>

// ═══════════════════════════════════════════════════════════════════════════════
//  DEFAULTS (overridden by saved config)
// ═══════════════════════════════════════════════════════════════════════════════

#define DEFAULT_DEVICE_CODE     "ECO-BIN-DOCK-1"
#define DEFAULT_WIFI_SSID       ""
#define DEFAULT_WIFI_PASS       ""

#define DEFAULT_MQTT_BROKER     "109.123.238.215"
#define DEFAULT_MQTT_PORT       1883
#define DEFAULT_MQTT_USER       "ecoroute"
#define DEFAULT_MQTT_PASS       "secret"
#define DEFAULT_MQTT_TOPIC      "ecoroute/trash_can/"

#define DEFAULT_PORTAL_USER     "admin"
#define DEFAULT_PORTAL_PASS     "ecoroute"

// ═══════════════════════════════════════════════════════════════════════════════
//  PUBLISH INTERVAL — fixed 1 second for demo
// ═══════════════════════════════════════════════════════════════════════════════

#define PUBLISH_INTERVAL_MS  1000     // MQTT publish every 1 second (demo mode)

// ═══════════════════════════════════════════════════════════════════════════════
//  PIN DEFINITIONS
// ═══════════════════════════════════════════════════════════════════════════════

// Sensor slot pin defaults (stored in NVS, configurable at runtime via web portal or serial)
// Slot 0: HC-SR04 fill sensor   — ECHO needs 1k/2k voltage divider (5V→3.3V)!
// Slot 1: RCWL-1604 dock detect — native 3.3V, no divider needed
// Slot 2: CJMCU-531/VL53L0X ToF — I2C (SDA=GPIO4, SCL=GPIO22), native 3.3V
// Slots 3: unassigned (set trig/echo to 0 to disable)
// NOTE: GPIOs 34, 35, 36, 39 are INPUT-ONLY on ESP32 — NEVER assign to TRIG!
#define MAX_SENSORS         4    // compile-time maximum slot count
#define DEFAULT_NUM_SENSORS 3
#define DEFAULT_TRIG_0      32
#define DEFAULT_ECHO_0      35
#define DEFAULT_TRIG_1      19
#define DEFAULT_ECHO_1      21
#define DEFAULT_TRIG_2      0    // ToF slot — trig/echo unused (I2C)
#define DEFAULT_ECHO_2      0
#define DEFAULT_TRIG_3      0
#define DEFAULT_ECHO_3      0

// Sensor types — stored per-slot in NVS
#define SENSOR_TYPE_US   0   // HC-SR04 / RCWL-1604 (ultrasonic, trig/echo pulse)
#define SENSOR_TYPE_TOF  1   // VL53L0X / CJMCU-531 (Time-of-Flight, I2C)

// VL53L0X I2C pins — GPIO4/GPIO22 avoids conflict with RCWL-1604 ECHO on GPIO21
#define TOF_SDA_PIN  4
#define TOF_SCL_PIN  22

#define LED_PIN             2
#define CALIBRATE_BTN_PIN   0

// Bin-absent threshold: if avg distance > binHeight * this, bin is not docked
#define BIN_ABSENT_THRESHOLD  1.4f

// ═══════════════════════════════════════════════════════════════════════════════
//  CONSTANTS
// ═══════════════════════════════════════════════════════════════════════════════

#define FIRMWARE_VERSION    "4.0.0-v4"
#define SENSOR_SAMPLES      5
#define SENSOR_TIMEOUT_US   50000     // 50ms — longer timeout for close objects
#define SENSOR_MIN_CM       1.0f      // allow readings as close as 1cm
#define SENSOR_MAX_CM       400.0f
#define ANOMALY_THRESHOLD   300.0f

#define FILL_CHANGE_THRESHOLD 5.0f

#define AP_CHANNEL          1
#define DNS_PORT            53

// ═══════════════════════════════════════════════════════════════════════════════
//  BLE PROVISIONING
// ═══════════════════════════════════════════════════════════════════════════════

#define BLE_DEVICE_NAME_PREFIX  "ECO-BIN-"
#define SERVICE_UUID            "ec010001-b100-4ec0-b100-ec00000e0000"
#define CHAR_WIFI_SSID_UUID     "ec010002-b100-4ec0-b100-ec00000e0000"
#define CHAR_WIFI_PASS_UUID     "ec010003-b100-4ec0-b100-ec00000e0000"
#define CHAR_DEVICE_CODE_UUID   "ec010004-b100-4ec0-b100-ec00000e0000"
#define CHAR_API_URL_UUID       "ec010005-b100-4ec0-b100-ec00000e0000"
#define CHAR_INTERVAL_UUID      "ec010006-b100-4ec0-b100-ec00000e0000"
#define CHAR_BIN_HEIGHT_UUID    "ec010007-b100-4ec0-b100-ec00000e0000"
#define CHAR_STATUS_UUID        "ec010008-b100-4ec0-b100-ec00000e0000"
#define CHAR_COMMAND_UUID       "ec010009-b100-4ec0-b100-ec00000e0000"

#define CMD_SAVE_AND_RESTART    0x01
#define CMD_FORCE_REPORT        0x02
#define CMD_FACTORY_RESET       0x03
#define CMD_ENABLE_AP           0x04
#define CMD_DISABLE_AP          0x05
#define CMD_SET_AP_HIDDEN       0x06
#define CMD_SET_AP_ON_DEMAND    0x07
#define CMD_SET_AP_ALWAYS       0x08
#define CMD_FIND_ME             0x09  // blink onboard LED — physical bin locator

// ═══════════════════════════════════════════════════════════════════════════════
//  AP BROADCAST MODES
// ═══════════════════════════════════════════════════════════════════════════════

#define AP_MODE_ALWAYS          0
#define AP_MODE_HIDDEN          1
#define AP_MODE_ON_DEMAND       2
#define AP_ON_DEMAND_TIMEOUT_MS 300000  // 5 minutes

// ═══════════════════════════════════════════════════════════════════════════════
//  GLOBALS
// ═══════════════════════════════════════════════════════════════════════════════

WiFiClient   wifiClient;
PubSubClient mqtt(wifiClient);
WebServer    webServer(80);
DNSServer    dnsServer;
Preferences  prefs;

// Sensor state (updated every loop cycle for web UI)
float currentDistanceCm = -1.0f;
float currentFillPercent = -1.0f;
int32_t currentRssi = 0;
bool  currentAnomaly = false;
bool  binPresent = true;         // true = bin is docked in station
bool  lastBinPresent = true;
bool  lastDiverged = false;      // set true when divergence filter fires (reliable bin-present signal)
unsigned long lastPublishTime = 0;
unsigned long lastSensorReadTime = 0;
unsigned long publishCount = 0;
bool  mqttConnectedFlag = false;
bool  wifiConnectedFlag = false;

// Calibration
float binHeightCm = 35.0f;       // default = inner liner height from diagram
float sensorOffsetCm = 0.0f;

// Change detection
float lastPublishedFill = -999.0f;

// Config (loaded from NVS)
String cfgDeviceCode;
String cfgWifiSsid;
String cfgWifiPass;
String cfgMqttBroker;
int    cfgMqttPort;
String cfgMqttUser;
String cfgMqttPass;
String cfgMqttTopic;
String cfgPortalUser;
String cfgPortalPass;

// AP config
String apSsid;
uint8_t cfgApMode = AP_MODE_ALWAYS;
bool    cfgAssumeFullOnFail = true;    // if ALL sensors fail with bin present, assume 100% full
bool    cfgPerSensorAssumeFull = true; // if ANY single sensor fails, treat it as 100% (blocked = full)
float   cfgFullDistanceCm = 0.0f;     // calibrated distance when bin is full (0 = not calibrated)

// Sensor refresh rate
uint32_t cfgSensorIntervalMs = 500;   // sensor read every 500ms for live demo
bool     cfgVerboseLogging = false;   // verbose per-sample logging
unsigned long lastFillChangeTime = 0; // when fill changed significantly
bool    apModeActive = false;
bool    apOnDemandActive = false;
unsigned long apOnDemandStart = 0;

// BLE state
BLEServer* bleServer = nullptr;
BLECharacteristic* bleStatusChar = nullptr;
bool bleClientConnected = false;
bool bleActive = false;
volatile bool bleCmdSave = false;
volatile bool bleCmdForceReport = false;
volatile bool bleCmdFactoryReset = false;
volatile bool bleCmdEnableAp = false;
volatile bool bleCmdDisableAp = false;
volatile int8_t blePendingApMode = -1;
volatile bool bleCmdFindMe = false;

// Find Me LED blink state (non-blocking)
unsigned long findMeBlinkEnd  = 0;
unsigned long findMeBlinkNext = 0;
bool          findMeBlinkOn   = false;
#define FIND_ME_BLINKS  10
#define FIND_ME_ON_MS   150
#define FIND_ME_OFF_MS  150

struct DiagResult {
  bool ultrasonicOk;
  float readings[10];
  int validCount;
  float medianCm;
  float minCm;
  float maxCm;
  bool echoPin;
};

struct CalResult {
  float median;
  int validCount;
  String log;
};

// ── SIMULATOR ─────────────────────────────────────────────────────────────────
// Simulator mode generates synthetic sensor readings without physical hardware.
// Useful for QEMU emulation, CI testing, and demo environments.
//
// Waveform modes:
//   0 = STATIC      — constant fill % (cfgSimSlot[i].staticFill)
//   1 = SAWTOOTH    — linear fill 0→100% over periodMs, then resets
//   2 = SINE        — sinusoidal between minFill and maxFill over periodMs
//   3 = RANDOM_WALK — bounded random drift between minFill and maxFill
//   4 = CUSTOM      — manually controlled via portal slider or serial SIM FILL
#define SIM_MODE_STATIC       0
#define SIM_MODE_SAWTOOTH     1
#define SIM_MODE_SINE         2
#define SIM_MODE_RANDOM_WALK  3
#define SIM_MODE_CUSTOM       4

struct SimSlotCfg {
  uint8_t mode;         // SIM_MODE_* constant
  float   staticFill;   // STATIC/CUSTOM target fill %
  float   periodMs;     // SAWTOOTH/SINE: full-cycle duration in ms (default 5 min)
  float   noiseCm;      // gaussian noise amplitude in cm (0 = noiseless)
  float   minFill;      // lower bound for SINE/RANDOM_WALK waveforms
  float   maxFill;      // upper bound for SINE/RANDOM_WALK waveforms
};

// ── Runtime sensor configuration (loaded from NVS on boot, overridden by sensors.json) ──
// Configurable at runtime via web portal (/config) or serial commands (SET SENSOR …).
// GPIOs 34, 35, 36, 39 are hardware input-only on ESP32 — never assign to TRIG slots.
uint8_t cfgNumSensors = DEFAULT_NUM_SENSORS;
uint8_t cfgTrigPins[MAX_SENSORS] = { DEFAULT_TRIG_0, DEFAULT_TRIG_1, DEFAULT_TRIG_2, DEFAULT_TRIG_3 };
uint8_t cfgEchoPins[MAX_SENSORS] = { DEFAULT_ECHO_0, DEFAULT_ECHO_1, DEFAULT_ECHO_2, DEFAULT_ECHO_3 };
uint8_t cfgSensorTypes[MAX_SENSORS] = { SENSOR_TYPE_US, SENSOR_TYPE_US, SENSOR_TYPE_TOF, SENSOR_TYPE_US };
char    cfgSensorTags[MAX_SENSORS][32] = {
  "HC-SR04(fill)", "RCWL-1604(dock)", "VL53L0X(tof)", "sensor3"
};

// Backward-compat macros — slot 0 is the primary fill sensor
#define TRIG_PIN  cfgTrigPins[0]
#define ECHO_PIN  cfgEchoPins[0]

// VL53L0X ToF sensor instance + availability flag
VL53L0X tofSensor;
bool    tofAvailable = false;

// Per-sensor last reading (for diagnostics + telemetry) — sized for MAX_SENSORS
float sensorDistances[MAX_SENSORS];
float sensorFills[MAX_SENSORS];

// ── SIMULATOR STATE ───────────────────────────────────────────────────────────
bool        simEnabled = false;           // master on/off (persisted in NVS)
SimSlotCfg  simCfg[MAX_SENSORS];         // per-slot waveform config
float       simCurrentFill[MAX_SENSORS]; // runtime fill % — mutated by RANDOM_WALK/CUSTOM
unsigned long simEpoch = 0;              // millis() snapshot when simulator was last enabled

// Forward declarations
void startAccessPoint(bool hidden);
void startAccessPoint();
void stopAccessPoint();
void enableOnDemandAp();
void updateSensors();
void updateSensorsSimulated();
float computeSimFill(int slot);
bool publishTelemetry();
float calculateFillPercent(float distanceCm);
void triggerFindMe();
void tickFindMe();
void setupSensorPins();
void loadSensorJson();
void onMqttMessage(char* topic, byte* payload, unsigned int length);
void performMqttOta(const String& url);

// ── MQTT OTA pending state (set in callback, consumed in loop) ──
static String   otaPendingUrl;
static bool     otaPending = false;

// ═══════════════════════════════════════════════════════════════════════════════
//  NVS CONFIG LOAD/SAVE
// ═══════════════════════════════════════════════════════════════════════════════

void loadConfig() {
  prefs.begin("ecoroute", true);

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
  binHeightCm    = prefs.getFloat("binHeight",    35.0f);
  sensorOffsetCm = prefs.getFloat("sensorOffset", 0.0f);
  cfgApMode      = prefs.getUChar("apMode",       AP_MODE_ALWAYS);
  cfgAssumeFullOnFail    = prefs.getBool("assumeFull",    true);
  cfgPerSensorAssumeFull = prefs.getBool("perSensorFull", true);
  cfgFullDistanceCm      = prefs.getFloat("fullDist",     0.0f);
  cfgSensorIntervalMs    = prefs.getUInt("sensorInt",     500);
  cfgVerboseLogging      = prefs.getBool("verbose",       false);

  // Sensor slot config (configurable count + per-slot GPIO pins + labels)
  cfgNumSensors = prefs.getUChar("numSensors", DEFAULT_NUM_SENSORS);
  if (cfgNumSensors < 1 || cfgNumSensors > MAX_SENSORS) cfgNumSensors = DEFAULT_NUM_SENSORS;
  cfgTrigPins[0] = prefs.getUChar("trig0", DEFAULT_TRIG_0);
  cfgEchoPins[0] = prefs.getUChar("echo0", DEFAULT_ECHO_0);
  prefs.getString("tag0", cfgSensorTags[0], sizeof(cfgSensorTags[0]));
  if (strlen(cfgSensorTags[0]) == 0) strlcpy(cfgSensorTags[0], "HC-SR04(fill)",  sizeof(cfgSensorTags[0]));
  cfgTrigPins[1] = prefs.getUChar("trig1", DEFAULT_TRIG_1);
  cfgEchoPins[1] = prefs.getUChar("echo1", DEFAULT_ECHO_1);
  prefs.getString("tag1", cfgSensorTags[1], sizeof(cfgSensorTags[1]));
  if (strlen(cfgSensorTags[1]) == 0) strlcpy(cfgSensorTags[1], "RCWL-1604(dock)", sizeof(cfgSensorTags[1]));
  cfgTrigPins[2] = prefs.getUChar("trig2", DEFAULT_TRIG_2);
  cfgEchoPins[2] = prefs.getUChar("echo2", DEFAULT_ECHO_2);
  prefs.getString("tag2", cfgSensorTags[2], sizeof(cfgSensorTags[2]));
  if (strlen(cfgSensorTags[2]) == 0) strlcpy(cfgSensorTags[2], "sensor2", sizeof(cfgSensorTags[2]));
  cfgTrigPins[3] = prefs.getUChar("trig3", DEFAULT_TRIG_3);
  cfgEchoPins[3] = prefs.getUChar("echo3", DEFAULT_ECHO_3);
  prefs.getString("tag3", cfgSensorTags[3], sizeof(cfgSensorTags[3]));
  if (strlen(cfgSensorTags[3]) == 0) strlcpy(cfgSensorTags[3], "sensor3", sizeof(cfgSensorTags[3]));

  // Per-slot sensor types (0=ultrasonic, 1=ToF/I2C)
  cfgSensorTypes[0] = prefs.getUChar("type0", SENSOR_TYPE_US);
  cfgSensorTypes[1] = prefs.getUChar("type1", SENSOR_TYPE_US);
  cfgSensorTypes[2] = prefs.getUChar("type2", SENSOR_TYPE_TOF);
  cfgSensorTypes[3] = prefs.getUChar("type3", SENSOR_TYPE_US);

  // ── Simulator config ──────────────────────────────────────────────────
  simEnabled = prefs.getBool("simOn", false);
#ifdef SIMULATOR_MODE_DEFAULT
  simEnabled = true;  // compile-time default: simulator always starts enabled
#endif
  for (int i = 0; i < MAX_SENSORS; i++) {
    // NVS keys are <=15 chars: sim0m, sim0f, sim0p, sim0n, sim0lo, sim0hi
    char pfx[8]; snprintf(pfx, sizeof(pfx), "sim%d", i);
    char key[12];
    snprintf(key, sizeof(key), "%sm", pfx);  simCfg[i].mode       = prefs.getUChar(key, SIM_MODE_SAWTOOTH);
    snprintf(key, sizeof(key), "%sf", pfx);  simCfg[i].staticFill = prefs.getFloat(key, 50.0f);
    snprintf(key, sizeof(key), "%sp", pfx);  simCfg[i].periodMs   = prefs.getFloat(key, 300000.0f); // 5 min
    snprintf(key, sizeof(key), "%sn", pfx);  simCfg[i].noiseCm    = prefs.getFloat(key, 1.0f);
    snprintf(key, sizeof(key), "%slo", pfx); simCfg[i].minFill    = prefs.getFloat(key, 0.0f);
    snprintf(key, sizeof(key), "%shi", pfx); simCfg[i].maxFill    = prefs.getFloat(key, 100.0f);
    simCurrentFill[i] = simCfg[i].staticFill;
  }
  if (simEnabled) simEpoch = millis();

  prefs.end();

  Serial.printf("[config] Device: %s, WiFi: %s, MQTT: %s:%d\n",
    cfgDeviceCode.c_str(), cfgWifiSsid.c_str(),
    cfgMqttBroker.c_str(), cfgMqttPort);
}

void saveConfig() {
  prefs.begin("ecoroute", false);

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
  prefs.putUChar("apMode",       cfgApMode);
  prefs.putBool("assumeFull",    cfgAssumeFullOnFail);
  prefs.putBool("perSensorFull", cfgPerSensorAssumeFull);
  prefs.putFloat("fullDist",     cfgFullDistanceCm);
  prefs.putUInt("sensorInt",     cfgSensorIntervalMs);
  prefs.putBool("verbose",       cfgVerboseLogging);

  // Sensor slot config
  prefs.putUChar("numSensors", cfgNumSensors);
  prefs.putUChar("trig0", cfgTrigPins[0]); prefs.putUChar("echo0", cfgEchoPins[0]); prefs.putString("tag0", cfgSensorTags[0]);
  prefs.putUChar("trig1", cfgTrigPins[1]); prefs.putUChar("echo1", cfgEchoPins[1]); prefs.putString("tag1", cfgSensorTags[1]);
  prefs.putUChar("trig2", cfgTrigPins[2]); prefs.putUChar("echo2", cfgEchoPins[2]); prefs.putString("tag2", cfgSensorTags[2]);
  prefs.putUChar("trig3", cfgTrigPins[3]); prefs.putUChar("echo3", cfgEchoPins[3]); prefs.putString("tag3", cfgSensorTags[3]);
  prefs.putUChar("type0", cfgSensorTypes[0]); prefs.putUChar("type1", cfgSensorTypes[1]);
  prefs.putUChar("type2", cfgSensorTypes[2]); prefs.putUChar("type3", cfgSensorTypes[3]);

  // ── Simulator config ──────────────────────────────────────────────────
  prefs.putBool("simOn", simEnabled);
  for (int i = 0; i < MAX_SENSORS; i++) {
    char pfx[8]; snprintf(pfx, sizeof(pfx), "sim%d", i);
    char key[12];
    snprintf(key, sizeof(key), "%sm", pfx);  prefs.putUChar(key, simCfg[i].mode);
    snprintf(key, sizeof(key), "%sf", pfx);  prefs.putFloat(key, simCfg[i].staticFill);
    snprintf(key, sizeof(key), "%sp", pfx);  prefs.putFloat(key, simCfg[i].periodMs);
    snprintf(key, sizeof(key), "%sn", pfx);  prefs.putFloat(key, simCfg[i].noiseCm);
    snprintf(key, sizeof(key), "%slo", pfx); prefs.putFloat(key, simCfg[i].minFill);
    snprintf(key, sizeof(key), "%shi", pfx); prefs.putFloat(key, simCfg[i].maxFill);
  }

  prefs.end();
  Serial.println("[config] Saved to NVS");
}

// ═══════════════════════════════════════════════════════════════════════════════
//  SENSOR PIN SETUP + SPIFFS JSON LOADER
// ═══════════════════════════════════════════════════════════════════════════════

// Call after loadConfig() (and optionally loadSensorJson()) to configure GPIO modes.
void setupSensorPins() {
  for (int i = 0; i < cfgNumSensors; i++) {
    if (cfgSensorTypes[i] == SENSOR_TYPE_TOF) {
      Serial.printf("[sensors] Slot %d: VL53L0X ToF I2C SDA=GPIO%d SCL=GPIO%d tag='%s'\n",
        i, TOF_SDA_PIN, TOF_SCL_PIN, cfgSensorTags[i]);
      continue;
    }
    uint8_t trig = cfgTrigPins[i];
    uint8_t echo = cfgEchoPins[i];
    if (trig == 0 && echo == 0) continue;  // slot not configured
    // GPIOs 34/35/36/39 are hardware input-only — skip OUTPUT config for those
    if (trig > 0 && trig < 34) {
      pinMode(trig, OUTPUT);
      digitalWrite(trig, LOW);
    }
    if (echo > 0) pinMode(echo, INPUT);
    Serial.printf("[sensors] Slot %d: TRIG=GPIO%d ECHO=GPIO%d tag='%s'\n",
      i, trig, echo, cfgSensorTags[i]);
  }

  // Initialize I2C and VL53L0X for any ToF-type slots
  bool tofNeeded = false;
  for (int i = 0; i < cfgNumSensors; i++) {
    if (cfgSensorTypes[i] == SENSOR_TYPE_TOF) { tofNeeded = true; break; }
  }
  if (tofNeeded) {
    Wire.begin(TOF_SDA_PIN, TOF_SCL_PIN);
    tofSensor.setTimeout(500);
    if (tofSensor.init()) {
      tofSensor.startContinuous(50);
      tofAvailable = true;
      Serial.printf("[tof] VL53L0X initialized — SDA=GPIO%d SCL=GPIO%d\n", TOF_SDA_PIN, TOF_SCL_PIN);
    } else {
      tofAvailable = false;
      Serial.printf("[tof] VL53L0X init FAILED — check wiring: SDA=GPIO%d, SCL=GPIO%d, VCC=3V3\n",
        TOF_SDA_PIN, TOF_SCL_PIN);
    }
  }
}

// Load sensors.json from SPIFFS — overrides NVS sensor config if present.
// Format: [{"slot":0,"trig":32,"echo":35,"tag":"HC-SR04(fill)"},...]
void loadSensorJson() {
  if (!SPIFFS.begin()) return;  // not formatted — silently skip
  if (!SPIFFS.exists("/sensors.json")) { SPIFFS.end(); return; }
  File f = SPIFFS.open("/sensors.json", "r");
  if (!f || f.size() == 0) { SPIFFS.end(); return; }
  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, f);
  f.close();
  SPIFFS.end();
  if (err) {
    Serial.printf("[sensors.json] Parse error: %s — using NVS values\n", err.c_str());
    return;
  }
  JsonArray arr = doc.as<JsonArray>();
  int n = 0;
  for (JsonObject slot : arr) {
    int idx = slot["slot"] | -1;
    if (idx < 0 || idx >= MAX_SENSORS) continue;
    if (slot["trig"].is<int>()) cfgTrigPins[idx]  = (uint8_t)slot["trig"].as<int>();
    if (slot["echo"].is<int>()) cfgEchoPins[idx]  = (uint8_t)slot["echo"].as<int>();
    if (slot["type"].is<int>()) cfgSensorTypes[idx] = (uint8_t)slot["type"].as<int>();
    if (slot["tag"].is<const char*>())
      strlcpy(cfgSensorTags[idx], slot["tag"].as<const char*>(), sizeof(cfgSensorTags[0]));
    n++;
  }
  if (n > 0 && n <= MAX_SENSORS) cfgNumSensors = (uint8_t)n;
  Serial.printf("[sensors.json] Loaded %d slot(s) from SPIFFS — overrides NVS\n", n);
}

// ═══════════════════════════════════════════════════════════════════════════════
//  ULTRASONIC SENSOR
// ═══════════════════════════════════════════════════════════════════════════════

float readDistanceSingleOn(uint8_t trig, uint8_t echo, const char* tag) {
  digitalWrite(trig, LOW);
  delayMicroseconds(2);
  digitalWrite(trig, HIGH);
  delayMicroseconds(10);
  digitalWrite(trig, LOW);

  unsigned long duration = pulseIn(echo, HIGH, SENSOR_TIMEOUT_US);
  if (duration == 0) {
    if (cfgVerboseLogging) Serial.printf("[%s]       pulse timeout (no echo)\n", tag);
    return -1.0f;
  }

  float distanceCm = (float)duration * 0.01715f;
  if (distanceCm < SENSOR_MIN_CM || distanceCm > SENSOR_MAX_CM) {
    if (cfgVerboseLogging) Serial.printf("[%s]       out of range: %.1f cm (duration=%lu us)\n", tag, distanceCm, duration);
    return -1.0f;
  }

  if (cfgVerboseLogging) Serial.printf("[%s]       echo=%lu us -> %.1f cm\n", tag, duration, distanceCm);
  return distanceCm;
}

// Backwards-compatible wrapper — reads primary sensor
float readDistanceSingle() {
  return readDistanceSingleOn(TRIG_PIN, ECHO_PIN, "us1");
}

// Median read for an arbitrary sensor pair
float readDistanceMedianOn(uint8_t trig, uint8_t echo, const char* tag) {
  if (cfgVerboseLogging) Serial.printf("[%s]       sampling %d readings...\n", tag, SENSOR_SAMPLES);
  float readings[SENSOR_SAMPLES];
  int validCount = 0;

  for (int i = 0; i < SENSOR_SAMPLES; i++) {
    if (cfgVerboseLogging) Serial.printf("[%s]       sample %d/%d: ", tag, i + 1, SENSOR_SAMPLES);
    float d = readDistanceSingleOn(trig, echo, tag);
    if (d > 0) readings[validCount++] = d;
    delay(40);
  }

  if (validCount == 0) {
    if (cfgVerboseLogging) Serial.printf("[%s]       all failed — retrying with 200ms delays...\n", tag);
    for (int i = 0; i < 3; i++) {
      delay(200);
      if (cfgVerboseLogging) Serial.printf("[%s]       retry %d/3: ", tag, i + 1);
      float d = readDistanceSingleOn(trig, echo, tag);
      if (d > 0) readings[validCount++] = d;
    }
  }

  if (validCount == 0) {
    if (cfgVerboseLogging) Serial.printf("[%s]       FAIL — no valid readings\n", tag);
    return -1.0f;
  }

  for (int i = 1; i < validCount; i++) {
    float key = readings[i];
    int j = i - 1;
    while (j >= 0 && readings[j] > key) {
      readings[j + 1] = readings[j];
      j--;
    }
    readings[j + 1] = key;
  }

  float median = readings[validCount / 2];
  if (cfgVerboseLogging) {
    Serial.printf("[%s]       median: %.1f cm (valid %d/%d)\n", tag, median, validCount, SENSOR_SAMPLES);
  }
  return median;
}

// Backwards-compatible wrapper — reads primary sensor
float readDistanceMedian() {
  return readDistanceMedianOn(TRIG_PIN, ECHO_PIN, "us1");
}

// VL53L0X Time-of-Flight distance reading (I2C, continuous mode)
float readTofDistanceCm() {
  if (!tofAvailable) return -1.0f;
  uint16_t d = tofSensor.readRangeContinuousMillimeters();
  if (tofSensor.timeoutOccurred()) {
    Serial.println("[tof] timeout");
    return -1.0f;
  }
  float cm = d / 10.0f;
  if (cm < SENSOR_MIN_CM || cm > SENSOR_MAX_CM) {
    if (cfgVerboseLogging) Serial.printf("[tof] out of range: %.1f cm (raw=%u mm)\n", cm, d);
    return -1.0f;
  }
  if (cfgVerboseLogging) Serial.printf("[tof] range=%u mm -> %.1f cm\n", d, cm);
  return cm;
}

// Minimum plausible distance for a real echo. Anything shorter is almost
// certainly cross-talk from an adjacent sensor's residual pulse.
#define MIN_PLAUSIBLE_CM       5.0f
#define INTER_SENSOR_DELAY_MS  200  // let ultrasonic echoes dissipate between sensors
#define SENSOR_DIVERGENCE_CM   10.0f // if sensors disagree >this, trust the farther one (more empty)

// Read all configured sensors sequentially. Returns the MEAN fill % of valid sensors.
float readAllSensorsAvgFill(float* outAvgDistance, int* outValidCount, bool* outAllFailed) {
  float fillSum = 0.0f;
  float distSum = 0.0f;
  int validFills = 0;
  int validDists = 0;

  for (int i = 0; i < cfgNumSensors; i++) {
    float d;
    if (cfgSensorTypes[i] == SENSOR_TYPE_TOF) {
      d = readTofDistanceCm();
    } else {
      d = readDistanceMedianOn(cfgTrigPins[i], cfgEchoPins[i], cfgSensorTags[i]);
    }

    // Reject implausibly short readings — almost always cross-talk from adjacent sensors
    if (d > 0 && d < MIN_PLAUSIBLE_CM) {
      Serial.printf("[%s]       REJECTED %.1f cm (below %.1f cm — likely crosstalk from another sensor)\n",
        cfgSensorTags[i], d, MIN_PLAUSIBLE_CM);
      d = -1.0f;
    }

    sensorDistances[i] = d;

    if (d > 0) {
      float f = calculateFillPercent(d);
      if (f <= 0.0f && d > binHeightCm) f = 0.0f;
      sensorFills[i] = f;
      if (f >= 0) { fillSum += f; validFills++; }
      distSum += d; validDists++;
    } else if (cfgPerSensorAssumeFull) {
      if (cfgVerboseLogging) Serial.printf("[%s]       fallback: failed read -> assuming 100%% (cfgPerSensorAssumeFull ON)\n", cfgSensorTags[i]);
      sensorFills[i] = 100.0f;
      fillSum += 100.0f; validFills++;
    } else {
      sensorFills[i] = -1.0f;
    }

    if (cfgSensorTypes[i] != SENSOR_TYPE_TOF) delay(INTER_SENSOR_DELAY_MS);
  }

  // ── Divergence filter ──────────────────────────────────────────────────────
  // If exactly 2 sensors are valid and disagree by > SENSOR_DIVERGENCE_CM,
  // the closer reading is likely a wall-reflection artefact (wide-beam radar).
  // Use only the farther sensor for fill; report the closer one as suspect.
  // IMPORTANT: divergence also serves as the bin-present signal — the RCWL-1604
  // wide beam hitting the inner tapered wall of the liner means the bin IS in dock.
  if (validDists == 2) {
    float d0 = sensorDistances[0], d1 = sensorDistances[1];
    if (d0 > 0 && d1 > 0 && fabsf(d0 - d1) > SENSOR_DIVERGENCE_CM) {
      int farIdx  = (d0 > d1) ? 0 : 1;
      Serial.printf("[diverge] %s=%.1f vs %s=%.1f diff=%.1f cm > %.0f — using %s (farther)\n",
        cfgSensorTags[0], d0, cfgSensorTags[1], d1,
        fabsf(d0 - d1), SENSOR_DIVERGENCE_CM, cfgSensorTags[farIdx]);
      // Recalculate using only the farther sensor
      float farDist = (farIdx == 0) ? d0 : d1;
      float farFill = calculateFillPercent(farDist);
      lastDiverged = true;
      if (outValidCount)  *outValidCount  = 1;
      if (outAllFailed)   *outAllFailed   = false;
      if (outAvgDistance) *outAvgDistance = farDist;
      return (farFill >= 0) ? farFill : 0.0f;
    }
  }
  lastDiverged = false;

  if (outValidCount) *outValidCount = validFills;
  if (outAllFailed)  *outAllFailed  = (validDists == 0);
  if (outAvgDistance) *outAvgDistance = (validDists > 0) ? (distSum / validDists) : -1.0f;

  if (validFills == 0) return -1.0f;
  return fillSum / validFills;
}

// ═══════════════════════════════════════════════════════════════════════════════
//  FILL LEVEL
// ═══════════════════════════════════════════════════════════════════════════════

float calculateFillPercent(float distanceCm) {
  if (distanceCm < 0) return -1.0f;
  if (binHeightCm <= 0) return -1.0f;

  float eff = distanceCm - sensorOffsetCm;
  if (eff < 0) eff = 0;

  // If we have a calibrated full distance, use two-point mapping
  if (cfgFullDistanceCm > 0 && cfgFullDistanceCm < binHeightCm) {
    // empty = binHeightCm (far), full = cfgFullDistanceCm (close)
    float range = binHeightCm - cfgFullDistanceCm;
    if (range <= 0) range = 1.0f;
    float pct = (1.0f - ((eff - cfgFullDistanceCm) / range)) * 100.0f;
    return constrain(pct, 0.0f, 100.0f);
  }

  // Default: linear from bin height
  float pct = (1.0f - (eff / binHeightCm)) * 100.0f;
  return constrain(pct, 0.0f, 100.0f);
}

// ═══════════════════════════════════════════════════════════════════════════════
//  ADAPTIVE INTERVAL — always 1s for demo
// ═══════════════════════════════════════════════════════════════════════════════

unsigned long getSensorReadIntervalMs() {
  return (unsigned long)cfgSensorIntervalMs;
}

unsigned long getAdaptiveIntervalMs(float) {
  return PUBLISH_INTERVAL_MS;
}

bool shouldPublishNow(float fillPct) {
  if (lastPublishedFill >= 0 && fillPct >= 0) {
    if (abs(fillPct - lastPublishedFill) >= FILL_CHANGE_THRESHOLD) return true;
  }
  if (binPresent != lastBinPresent) {
    Serial.printf("[adaptive] Bin %s — publishing now\n", binPresent ? "DOCKED" : "REMOVED");
    return true;
  }
  return false;
}

// ═══════════════════════════════════════════════════════════════════════════════
//  SIMULATOR ENGINE
// ═══════════════════════════════════════════════════════════════════════════════

// Compute synthetic fill % for a single sensor slot based on its waveform config.
// Called every sensor cycle when simEnabled == true.
float computeSimFill(int slot) {
  if (slot < 0 || slot >= MAX_SENSORS) return 50.0f;
  SimSlotCfg& c = simCfg[slot];
  float t = (float)(millis() - simEpoch);  // ms since sim epoch

  switch (c.mode) {
    case SIM_MODE_STATIC:
      return constrain(c.staticFill, 0.0f, 100.0f);

    case SIM_MODE_SAWTOOTH: {
      // Fill rises linearly from minFill to maxFill over periodMs, then resets
      float period = max(c.periodMs, 1000.0f);
      float phase  = fmod(t, period) / period;       // 0.0 → 1.0
      float range  = c.maxFill - c.minFill;
      return c.minFill + phase * range;
    }

    case SIM_MODE_SINE: {
      // Sinusoidal between minFill and maxFill over periodMs
      float period = max(c.periodMs, 1000.0f);
      float phase  = fmod(t, period) / period * TWO_PI;
      float range  = c.maxFill - c.minFill;
      return c.minFill + (range / 2.0f) * (1.0f - cosf(phase));
    }

    case SIM_MODE_RANDOM_WALK:
      // Runtime fill is mutated externally in updateSensorsSimulated — return stored value
      return constrain(simCurrentFill[slot], c.minFill, c.maxFill);

    case SIM_MODE_CUSTOM:
      // Manually set via portal slider or "SIM FILL" serial command
      return constrain(simCurrentFill[slot], 0.0f, 100.0f);

    default:
      return 50.0f;
  }
}

// Populate sensorDistances[], sensorFills[], currentFillPercent, currentDistanceCm
// from the waveform engine instead of real GPIO sensors.
// WiFi / MQTT / web portal continue to work normally — only sensor data is synthetic.
void updateSensorsSimulated() {
  if (WiFi.status() == WL_CONNECTED) currentRssi = WiFi.RSSI();

  float fillSum = 0.0f;
  float distSum = 0.0f;

  for (int i = 0; i < cfgNumSensors; i++) {
    // For random-walk: apply a bounded drift each cycle
    if (simCfg[i].mode == SIM_MODE_RANDOM_WALK) {
      float drift = (float)(random(-200, 200)) / 100.0f;  // ±2.0 % per cycle
      simCurrentFill[i] = constrain(simCurrentFill[i] + drift, simCfg[i].minFill, simCfg[i].maxFill);
    }

    float fill = computeSimFill(i);

    // Add per-slot noise (cm → fill %): noise_pct = noise_cm / binHeight * 100
    float noisePct = (binHeightCm > 0.5f) ? (simCfg[i].noiseCm / binHeightCm) * 100.0f : 0.0f;
    if (noisePct > 0.0f) {
      float noiseOff = ((float)(random(-100, 100)) / 100.0f) * noisePct;
      fill = constrain(fill + noiseOff, 0.0f, 100.0f);
    }

    // Convert fill % → distance cm (inverse of calculateFillPercent)
    float dist = binHeightCm * (1.0f - fill / 100.0f) + sensorOffsetCm;
    dist = max(dist, 0.5f);

    sensorDistances[i] = dist;
    sensorFills[i]     = fill;
    fillSum += fill;
    distSum += dist;
  }

  int n = (cfgNumSensors > 0) ? cfgNumSensors : 1;
  currentFillPercent = fillSum / n;
  currentDistanceCm  = distSum / n;
  binPresent         = true;   // bin is always "docked" in simulator
  currentAnomaly     = false;
  lastDiverged       = false;
  lastSensorReadTime = millis();

  Serial.printf("[sim] fill=%.1f%% dist=%.1fcm", currentFillPercent, currentDistanceCm);
  for (int i = 0; i < cfgNumSensors; i++)
    Serial.printf(" | s%d=%.1f%%", i, sensorFills[i]);
  Serial.println();
}

// ═══════════════════════════════════════════════════════════════════════════════
//  CALIBRATION
// ═══════════════════════════════════════════════════════════════════════════════

CalResult calibrateRead(int durationSec, bool readUltrasonic) {
  CalResult r;
  r.median = -1.0f;
  r.validCount = 0;
  r.log = "";

  // Ultrasonic sampling — average across ultrasonic sensors per cycle (skip ToF slots)
  if (readUltrasonic) {
    int usSensorCount = 0;
    for (int s = 0; s < cfgNumSensors; s++) if (cfgSensorTypes[s] != SENSOR_TYPE_TOF) usSensorCount++;
    Serial.printf("[cal] Sampling ultrasonic (%d-sensor average)...\n", usSensorCount);
    r.log += "\nUltrasonic Sampling (" + String(usSensorCount) + "-sensor average):\n";
    float readings[10];
    int vc = 0;
    for (int i = 0; i < 10; i++) {
      float distSum = 0; int vd = 0;
      String perSensor = "";
      for (int s = 0; s < cfgNumSensors; s++) {
        if (cfgSensorTypes[s] == SENSOR_TYPE_TOF) continue;  // skip ToF in calibration
        float d = readDistanceSingleOn(cfgTrigPins[s], cfgEchoPins[s], cfgSensorTags[s]);
        if (d > 0) { distSum += d; vd++; perSensor += String(d,1); }
        else perSensor += "--";
        if (s < cfgNumSensors-1) perSensor += "/";
        delay(30);
      }
      if (vd > 0) {
        float avg = distSum / vd;
        readings[vc++] = avg;
        r.log += "  Cycle " + String(i + 1) + ": " + perSensor + " -> avg " + String(avg,1) + " cm (" + String(vd) + "/" + String(usSensorCount) + ")\n";
      } else {
        r.log += "  Cycle " + String(i + 1) + ": ALL FAILED (" + perSensor + ")\n";
      }
      delay(60);
    }
    r.validCount = vc;
    if (vc >= 3) {
      for (int i = 1; i < vc; i++) {
        float key = readings[i]; int j = i - 1;
        while (j >= 0 && readings[j] > key) { readings[j+1] = readings[j]; j--; }
        readings[j+1] = key;
      }
      r.median = readings[vc / 2];
    } else if (vc > 0) {
      r.median = readings[0];
    }
    r.log += "  Valid cycles: " + String(vc) + "/10, Median: " + (r.median > 0 ? String(r.median, 1) + " cm" : "N/A") + "\n";
  }

  return r;
}

// Legacy simple calibration (still used by boot button)
String runCalibration() {
  CalResult r = calibrateRead(3, true);
  if (r.median > 0) {
    binHeightCm = r.median - sensorOffsetCm;
    if (binHeightCm < 5.0f) binHeightCm = 5.0f;
    saveConfig();
  }
  return r.log + "\nBin height set to: " + String(binHeightCm, 1) + " cm";
}

// ═══════════════════════════════════════════════════════════════════════════════
//  SENSOR UPDATE (non-blocking, called from loop)
// ═══════════════════════════════════════════════════════════════════════════════

void updateSensors() {
  // ── SIMULATOR BRANCH ─────────────────────────────────────────────────────
  if (simEnabled) { updateSensorsSimulated(); return; }
  // ── REAL SENSORS BELOW ────────────────────────────────────────────────
  unsigned long t0 = millis();
  float prevFill = currentFillPercent;

  if (cfgVerboseLogging) {
    Serial.println("───────────── sensor read ─────────────");
    Serial.printf("[sensor]   t=%lu ms  uptime=%lus\n", t0, t0 / 1000);
  }

  // WiFi RSSI
  if (WiFi.status() == WL_CONNECTED) currentRssi = WiFi.RSSI();

  // Read both sensors
  float avgFill = -1.0f;
  float avgDist = -1.0f;
  int validCount = 0;
  bool usAllFailed = false;

  avgFill = readAllSensorsAvgFill(&avgDist, &validCount, &usAllFailed);

  // Bin-present detection:
  // 1. If divergence fired → RCWL saw inner walls of liner = bin is definitely in dock.
  // 2. If no divergence AND sensors agree near/above binHeight → dock floor visible = bin absent.
  //    (RCWL penetrates empty dock and reads floor; HC-SR04 also reads floor. Both ~binHeight.)
  // 3. If no divergence AND sensors agree well below binHeight → trash filling past wall zone.
  if (lastDiverged) {
    binPresent = true;  // wall reflection = liner is in dock
  } else if (avgDist > 0 && binHeightCm > 0) {
    // Absent when both sensors see the floor (avgDist close to or beyond calibrated height)
    binPresent = (avgDist < binHeightCm - 2.0f);
  } else if (usAllFailed) {
    binPresent = cfgAssumeFullOnFail;
  }

  // Fill level
  if (binPresent) {
    if (validCount > 0) {
      currentDistanceCm  = avgDist;
      currentFillPercent = avgFill;
    } else if (cfgAssumeFullOnFail) {
      currentFillPercent = 100.0f;
      currentDistanceCm  = 0.0f;
    } else {
      currentDistanceCm  = -1.0f;
      currentFillPercent = -1.0f;
    }
  } else {
    // Bin not in dock — report empty
    currentFillPercent = 0.0f;
    currentDistanceCm  = avgDist;
  }

  currentAnomaly = (binPresent && currentDistanceCm < 0) || (currentDistanceCm > ANOMALY_THRESHOLD);
  lastSensorReadTime = millis();

  if (prevFill >= 0 && currentFillPercent >= 0 && fabs(currentFillPercent - prevFill) > 1.0f) {
    lastFillChangeTime = millis();
  }

  if (!cfgVerboseLogging) {
    String sensorLog = "[sensor] " + String(t0/1000) + "s";
    for (int i = 0; i < cfgNumSensors; i++) {
      sensorLog += " | ";
      sensorLog += cfgSensorTags[i];
      sensorLog += "=";
      sensorLog += (sensorDistances[i] > 0) ? (String(sensorDistances[i], 1) + "cm") : "FAIL";
    }
    sensorLog += " | avg=" + String(currentDistanceCm, 1) + "cm fill=" + String(currentFillPercent, 1)
               + "% bin=" + (binPresent ? "DOCKED" : "ABSENT")
               + " | " + String(millis() - t0) + "ms";
    Serial.println(sensorLog);
  } else {
    for (int i = 0; i < cfgNumSensors; i++)
      Serial.printf("[per-sensor] %s=%.1f cm (%.1f%%)\n",
        cfgSensorTags[i], sensorDistances[i], sensorFills[i]);
    Serial.printf("[result]   avg_distance=%.1f cm  avg_fill=%.1f%%  bin=%s  anomaly=%s  valid=%d/%d\n",
      currentDistanceCm, currentFillPercent,
      binPresent ? "DOCKED" : "ABSENT",
      currentAnomaly ? "YES" : "no",
      validCount, cfgNumSensors);
    Serial.printf("[timing]   read took %lu ms\n", millis() - t0);
    Serial.println("───────────────────────────────────────");
  }
}

// ═══════════════════════════════════════════════════════════════════════════════
//  WiFi + MQTT
// ═══════════════════════════════════════════════════════════════════════════════

bool connectWifiSta() {
  if (cfgWifiSsid.length() == 0) return false;

  Serial.printf("[wifi] Connecting to '%s'...\n", cfgWifiSsid.c_str());
  WiFi.begin(cfgWifiSsid.c_str(), cfgWifiPass.c_str());

  int attempts = 0;
  while (WiFi.status() != WL_CONNECTED && attempts < 20) {
    delay(500);
    Serial.print(".");
    attempts++;
  }

  wifiConnectedFlag = (WiFi.status() == WL_CONNECTED);
  if (wifiConnectedFlag) {
    Serial.printf("\n[wifi] Connected! IP: %s RSSI: %d dBm\n",
      WiFi.localIP().toString().c_str(), WiFi.RSSI());
  } else {
    Serial.println("\n[wifi] Failed to connect");
  }
  return wifiConnectedFlag;
}

bool connectMqttNonBlocking() {
  if (cfgMqttBroker.length() == 0) return false;
  if (mqtt.connected()) { mqttConnectedFlag = true; return true; }

  // Stable client ID (no random suffix) — avoids broker session churn
  String clientId = "ecoroute-" + cfgDeviceCode;
  unsigned long t0 = millis();
  Serial.printf("[mqtt] Connecting to %s:%d as '%s'...\n",
    cfgMqttBroker.c_str(), cfgMqttPort, clientId.c_str());

  mqtt.setServer(cfgMqttBroker.c_str(), cfgMqttPort);
  mqtt.setBufferSize(768);        // slightly larger for new payload with 3-sensor data
  mqtt.setKeepAlive(30);          // was 60s — faster drop detection
  mqtt.setSocketTimeout(5);       // was 15s default — fail fast on network issues
  mqtt.setCallback(onMqttMessage);

  if (mqtt.connect(clientId.c_str(), cfgMqttUser.c_str(), cfgMqttPass.c_str())) {
    Serial.printf("[mqtt] Connected in %lu ms\n", millis() - t0);
    mqttConnectedFlag = true;
    rebuildMqttTopic();  // refresh cached topic when connecting
    // Subscribe to device command topic for remote OTA + control
    String cmdTopic = "ecoroute/cmd/" + cfgDeviceCode;
    mqtt.subscribe(cmdTopic.c_str());
    Serial.printf("[mqtt] Subscribed to %s\n", cmdTopic.c_str());
    return true;
  }
  Serial.printf("[mqtt] Failed (rc=%d, took %lu ms)\n", mqtt.state(), millis() - t0);
  mqttConnectedFlag = false;
  return false;
}

// ═══════════════════════════════════════════════════════════════════════════════
//  TELEMETRY PUBLISH
// ═══════════════════════════════════════════════════════════════════════════════

// Human-readable slot position labels (indexed by slot number)
static const char* SENSOR_POSITIONS[] = { "primary", "secondary", "tertiary", "quaternary" };

// Cached topic string
String cachedMqttTopic;
static void rebuildMqttTopic() {
  cachedMqttTopic = cfgMqttTopic + cfgDeviceCode;
}

bool publishTelemetry() {
  if (!mqtt.connected()) {
    Serial.println("[mqtt]     not connected — skipping publish");
    return false;
  }

  unsigned long t0 = millis();

  JsonDocument doc;
  doc["device_code"]        = cfgDeviceCode;
  doc["firmware_version"]   = FIRMWARE_VERSION;
  doc["timestamp_ms"]       = millis();

  doc["fill_level_percent"] = (currentFillPercent >= 0) ? round(currentFillPercent * 10.0f) / 10.0f : 0.0f;
  doc["distance_cm"]        = (currentDistanceCm >= 0)  ? round(currentDistanceCm  * 10.0f) / 10.0f : -1.0f;
  doc["bin_present"]        = binPresent;
  doc["anomaly_flag"]       = currentAnomaly;
  doc["signal_strength"]    = currentRssi;
  doc["simulated"]          = simEnabled;
  if (simEnabled) doc["sim_pattern"] = simCfg[0].mode;  // pattern of primary slot

  // Per-sensor fields
  doc["primary_fill_percent"]     = (sensorFills[0] >= 0)     ? round(sensorFills[0]     * 10.0f) / 10.0f : -1.0f;
  doc["primary_distance_cm"]      = (sensorDistances[0] >= 0) ? round(sensorDistances[0] * 10.0f) / 10.0f : -1.0f;
  doc["secondary_fill_percent"]   = (sensorFills[1] >= 0)     ? round(sensorFills[1]     * 10.0f) / 10.0f : -1.0f;
  doc["secondary_distance_cm"]    = (sensorDistances[1] >= 0) ? round(sensorDistances[1] * 10.0f) / 10.0f : -1.0f;

  JsonObject sensors = doc["sensors"].to<JsonObject>();
  for (int i = 0; i < cfgNumSensors; i++) {
    JsonObject s = sensors[SENSOR_POSITIONS[i]].to<JsonObject>();
    s["distance_cm"]  = (sensorDistances[i] >= 0) ? round(sensorDistances[i] * 10.0f) / 10.0f : -1.0f;
    s["fill_percent"] = (sensorFills[i] >= 0)     ? round(sensorFills[i]     * 10.0f) / 10.0f : -1.0f;
    s["trig_gpio"]    = cfgTrigPins[i];
    s["echo_gpio"]    = cfgEchoPins[i];
    s["type"]         = (cfgSensorTypes[i] == SENSOR_TYPE_TOF) ? "tof" : "ultrasonic";
  }

  char payload[512];
  size_t len = serializeJson(doc, payload, sizeof(payload));

  if (cachedMqttTopic.length() == 0) rebuildMqttTopic();
  unsigned long sendStart = millis();
  bool ok = mqtt.publish(cachedMqttTopic.c_str(), payload, len);
  unsigned long sendMs = millis() - sendStart;

  if (ok) {
    lastPublishedFill = currentFillPercent;
    lastBinPresent    = binPresent;
    lastPublishTime   = millis();
    publishCount++;
  }

  Serial.printf("[mqtt] %s #%lu -> %s (payload %u B, took %lu ms)\n",
    ok ? "OK" : "FAIL", publishCount, cachedMqttTopic.c_str(), (unsigned)len, sendMs);

  return ok;
}

// ═══════════════════════════════════════════════════════════════════════════════
//  AP MODE CONTROL
// ═══════════════════════════════════════════════════════════════════════════════

void startAccessPoint(bool hidden) {
  apSsid = "EcoRoute-" + cfgDeviceCode;
  WiFi.mode(WIFI_AP_STA);
  WiFi.softAP(apSsid.c_str(), NULL, AP_CHANNEL, hidden ? 1 : 0);
  apModeActive = true;
  dnsServer.start(DNS_PORT, "*", WiFi.softAPIP());
  Serial.printf("[ap] Started '%s'%s — Portal at http://192.168.4.1\n",
    apSsid.c_str(), hidden ? " (HIDDEN)" : "");
}

void startAccessPoint() {
  startAccessPoint(cfgApMode == AP_MODE_HIDDEN);
}

void stopAccessPoint() {
  if (!apModeActive) return;
  dnsServer.stop();
  WiFi.softAPdisconnect(true);
  if (cfgWifiSsid.length() > 0) WiFi.mode(WIFI_STA);
  apModeActive = false;
  apOnDemandActive = false;
  Serial.println("[ap] Access point stopped");
}

void enableOnDemandAp() {
  if (apModeActive) return;
  Serial.println("[ap] On-demand AP enabled (5 min timeout)");
  startAccessPoint(false);
  apOnDemandActive = true;
  apOnDemandStart = millis();
}

// ═══════════════════════════════════════════════════════════════════════════════
//  BLE PROVISIONING (inline)
// ═══════════════════════════════════════════════════════════════════════════════

class BleServerCB : public BLEServerCallbacks {
  void onConnect(BLEServer* s) override { bleClientConnected = true; Serial.println("[ble] Client connected"); }
  void onDisconnect(BLEServer* s) override { bleClientConnected = false; Serial.println("[ble] Client disconnected"); s->startAdvertising(); }
};

class BleWifiSsidCB : public BLECharacteristicCallbacks {
  void onWrite(BLECharacteristic* c) override { cfgWifiSsid = c->getValue().c_str(); Serial.printf("[ble] WiFi SSID set: %s\n", cfgWifiSsid.c_str()); }
};
class BleWifiPassCB : public BLECharacteristicCallbacks {
  void onWrite(BLECharacteristic* c) override { cfgWifiPass = c->getValue().c_str(); Serial.println("[ble] WiFi password set"); }
};
class BleDeviceCodeCB : public BLECharacteristicCallbacks {
  void onWrite(BLECharacteristic* c) override { cfgDeviceCode = c->getValue().c_str(); Serial.printf("[ble] Device code set: %s\n", cfgDeviceCode.c_str()); }
  void onRead(BLECharacteristic* c) override { c->setValue(cfgDeviceCode.c_str()); }
};
class BleApiUrlCB : public BLECharacteristicCallbacks {
  void onWrite(BLECharacteristic* c) override { Serial.printf("[ble] API URL set: %s\n", c->getValue().c_str()); }
};
class BleIntervalCB : public BLECharacteristicCallbacks {
  void onWrite(BLECharacteristic* c) override {
    if (c->getLength() >= 4) { uint32_t v; memcpy(&v, c->getData(), 4); Serial.printf("[ble] Interval set: %u s\n", v); }
  }
};
class BleBinHeightCB : public BLECharacteristicCallbacks {
  void onWrite(BLECharacteristic* c) override {
    if (c->getLength() >= 4) { float v; memcpy(&v, c->getData(), 4); binHeightCm = v; Serial.printf("[ble] Bin height set: %.1f cm\n", v); }
  }
  void onRead(BLECharacteristic* c) override { c->setValue((uint8_t*)&binHeightCm, 4); }
};
class BleCmdCB : public BLECharacteristicCallbacks {
  void onWrite(BLECharacteristic* c) override {
    if (c->getLength() < 1) return;
    uint8_t cmd = c->getData()[0];
    Serial.printf("[ble] Command: 0x%02X\n", cmd);
    switch (cmd) {
      case CMD_SAVE_AND_RESTART:  bleCmdSave = true; break;
      case CMD_FORCE_REPORT:      bleCmdForceReport = true; break;
      case CMD_FACTORY_RESET:     bleCmdFactoryReset = true; break;
      case CMD_ENABLE_AP:         bleCmdEnableAp = true; break;
      case CMD_DISABLE_AP:        bleCmdDisableAp = true; break;
      case CMD_SET_AP_HIDDEN:     blePendingApMode = AP_MODE_HIDDEN; break;
      case CMD_SET_AP_ON_DEMAND:  blePendingApMode = AP_MODE_ON_DEMAND; break;
      case CMD_SET_AP_ALWAYS:     blePendingApMode = AP_MODE_ALWAYS; break;
      case CMD_FIND_ME:           bleCmdFindMe = true; break;
    }
  }
};

void startBLE() {
  // Use device code directly as BLE name (already has ECO-BIN- prefix)
  String bleName = cfgDeviceCode;
  BLEDevice::init(bleName.c_str());
  BLEDevice::setMTU(512);

  bleServer = BLEDevice::createServer();
  bleServer->setCallbacks(new BleServerCB());

  BLEService* svc = bleServer->createService(BLEUUID(SERVICE_UUID), 30);

  auto* c1 = svc->createCharacteristic(BLEUUID(CHAR_WIFI_SSID_UUID), BLECharacteristic::PROPERTY_WRITE);
  c1->setCallbacks(new BleWifiSsidCB());
  auto* c2 = svc->createCharacteristic(BLEUUID(CHAR_WIFI_PASS_UUID), BLECharacteristic::PROPERTY_WRITE);
  c2->setCallbacks(new BleWifiPassCB());
  auto* c3 = svc->createCharacteristic(BLEUUID(CHAR_DEVICE_CODE_UUID), BLECharacteristic::PROPERTY_READ | BLECharacteristic::PROPERTY_WRITE);
  c3->setCallbacks(new BleDeviceCodeCB());
  if (cfgDeviceCode.length() > 0) c3->setValue(cfgDeviceCode.c_str());
  auto* c4 = svc->createCharacteristic(BLEUUID(CHAR_API_URL_UUID), BLECharacteristic::PROPERTY_WRITE);
  c4->setCallbacks(new BleApiUrlCB());
  auto* c5 = svc->createCharacteristic(BLEUUID(CHAR_INTERVAL_UUID), BLECharacteristic::PROPERTY_READ | BLECharacteristic::PROPERTY_WRITE);
  c5->setCallbacks(new BleIntervalCB());
  auto* c6 = svc->createCharacteristic(BLEUUID(CHAR_BIN_HEIGHT_UUID), BLECharacteristic::PROPERTY_READ | BLECharacteristic::PROPERTY_WRITE);
  c6->setCallbacks(new BleBinHeightCB());
  c6->setValue((uint8_t*)&binHeightCm, 4);

  bleStatusChar = svc->createCharacteristic(BLEUUID(CHAR_STATUS_UUID), BLECharacteristic::PROPERTY_READ | BLECharacteristic::PROPERTY_NOTIFY);
  bleStatusChar->addDescriptor(new BLE2902());

  auto* c8 = svc->createCharacteristic(BLEUUID(CHAR_COMMAND_UUID), BLECharacteristic::PROPERTY_WRITE);
  c8->setCallbacks(new BleCmdCB());

  svc->start();

  BLEAdvertising* adv = BLEDevice::getAdvertising();
  adv->addServiceUUID(BLEUUID(SERVICE_UUID));
  adv->setScanResponse(true);
  adv->setMinPreferred(0x06);
  BLEDevice::startAdvertising();

  bleActive = true;
  Serial.printf("[ble] GATT server started as '%s'\n", bleName.c_str());
}

void updateBleStatus() {
  if (!bleStatusChar) return;
  JsonDocument doc;
  doc["wifi"] = wifiConnectedFlag;
  doc["configured"] = (cfgWifiSsid.length() > 0);
  doc["fw"] = FIRMWARE_VERSION;
  doc["deviceCode"] = cfgDeviceCode;
  doc["apMode"] = cfgApMode;
  String json;
  serializeJson(doc, json);
  bleStatusChar->setValue(json.c_str());
  bleStatusChar->notify();
}

void handleBleCommands() {
  if (!bleActive) return;

  if (bleClientConnected) updateBleStatus();

  if (bleCmdSave) {
    Serial.println("[ble] Save+restart command");
    bleCmdSave = false;
    saveConfig();
    BLEDevice::deinit(false);
    delay(500);
    ESP.restart();
  }
  if (bleCmdForceReport) {
    Serial.println("[ble] Force publish command");
    bleCmdForceReport = false;
    updateSensors();
    publishTelemetry();
  }
  if (bleCmdFactoryReset) {
    Serial.println("[ble] Factory reset command");
    bleCmdFactoryReset = false;
    BLEDevice::deinit(false);
    prefs.begin("ecoroute", false); prefs.clear(); prefs.end();
    delay(500);
    ESP.restart();
  }
  if (bleCmdEnableAp) {
    Serial.println("[ble] Enable AP command");
    bleCmdEnableAp = false;
    enableOnDemandAp();
  }
  if (bleCmdDisableAp) {
    Serial.println("[ble] Disable AP command");
    bleCmdDisableAp = false;
    stopAccessPoint();
  }
  if (blePendingApMode >= 0) {
    Serial.printf("[ble] AP mode change to %d\n", blePendingApMode);
    cfgApMode = (uint8_t)blePendingApMode;
    blePendingApMode = -1;
    saveConfig();
    if (cfgApMode == AP_MODE_ON_DEMAND) { stopAccessPoint(); }
    else { if (apModeActive) stopAccessPoint(); startAccessPoint(); }
  }
  if (bleCmdFindMe) {
    bleCmdFindMe = false;
    triggerFindMe();
  }
}

// Non-blocking Find Me LED blink — call every loop()
void tickFindMe() {
  if (findMeBlinkEnd == 0) return;
  unsigned long now = millis();
  if (now >= findMeBlinkEnd) {
    findMeBlinkEnd = 0;
    digitalWrite(LED_PIN, LOW);
    return;
  }
  if (now >= findMeBlinkNext) {
    findMeBlinkOn = !findMeBlinkOn;
    digitalWrite(LED_PIN, findMeBlinkOn ? HIGH : LOW);
    findMeBlinkNext = now + (findMeBlinkOn ? FIND_ME_ON_MS : FIND_ME_OFF_MS);
  }
}

void triggerFindMe() {
  Serial.printf("[find-me] Triggered — blinking LED %dx\n", FIND_ME_BLINKS);
  findMeBlinkOn   = true;
  digitalWrite(LED_PIN, HIGH);
  unsigned long now = millis();
  findMeBlinkNext = now + FIND_ME_ON_MS;
  findMeBlinkEnd  = now + (unsigned long)(FIND_ME_ON_MS + FIND_ME_OFF_MS) * FIND_ME_BLINKS;
}

// ═══════════════════════════════════════════════════════════════════════════════
//  HTTP AUTH HELPER
// ═══════════════════════════════════════════════════════════════════════════════

bool checkAuth() {
  if (!webServer.authenticate(cfgPortalUser.c_str(), cfgPortalPass.c_str())) {
    webServer.requestAuthentication(BASIC_AUTH, "EcoRoute Smart Bin");
    return false;
  }
  return true;
}

// ═══════════════════════════════════════════════════════════════════════════════
//  HTML HELPERS
// ═══════════════════════════════════════════════════════════════════════════════

String htmlHead(const String& title) {
  return R"(<!DOCTYPE html><html lang='en'><head><meta charset='utf-8'>
<meta name='viewport' content='width=device-width,initial-scale=1'>
<title>)" + title + R"( — routeOS</title>
<link rel='stylesheet' href='/style.css'></head><body>)";
}

void handleCSS() {
  static const char CSS_P[] PROGMEM = R"css(:root{
--bg:#0c160c;--surface:#162116;--surface-2:#1c2b1c;--border:#243824;--border-strong:#2f502f;
--text:#e8f2e8;--text-2:#7a9e7a;--text-3:#4a6a4a;
--brand:#4ade80;--brand-2:#22c55e;--brand-soft:#0f2914;
--ok:#4ade80;--warn:#facc15;--err:#f87171;--info:#60a5fa;
--ok-soft:#0f2914;--warn-soft:#422006;--err-soft:#4c0f0f;--info-soft:#0f1e40;
--shadow-sm:0 1px 2px rgba(0,0,0,.5);--shadow:0 1px 3px rgba(0,0,0,.6),0 1px 2px rgba(0,0,0,.5);--shadow-lg:0 10px 25px -5px rgba(0,0,0,.7),0 4px 6px -2px rgba(0,0,0,.6);
--r-sm:6px;--r:10px;--r-lg:14px;--r-xl:20px;
--radius-md:var(--r-sm);--radius-lg:var(--r);
--primary-500:var(--brand);--primary-600:var(--brand-2);--primary-50:var(--brand-soft);
--slate-50:var(--surface-2);--slate-100:var(--surface-2);--slate-200:var(--border);--slate-400:var(--text-3);--slate-600:var(--text-2);--slate-900:var(--text);--white:var(--surface);
--success-bg:var(--ok-soft);--success-text:var(--ok);--warning-bg:var(--warn-soft);--warning-text:var(--warn);--danger-bg:var(--err-soft);--danger-text:var(--err);
}
*{box-sizing:border-box;margin:0;padding:0}
body{font-family:-apple-system,BlinkMacSystemFont,'Inter','Segoe UI',sans-serif;background:var(--bg);color:var(--text);font-size:14px;line-height:1.5;-webkit-font-smoothing:antialiased}
.topbar{background:var(--surface);backdrop-filter:blur(8px);border-bottom:1px solid var(--border);position:sticky;top:0;z-index:50}
.topbar-inner{max-width:980px;margin:0 auto;padding:14px 20px;display:flex;align-items:center;gap:16px;flex-wrap:wrap}
.brand{display:flex;align-items:center;gap:10px;text-decoration:none;color:var(--text);margin-right:auto}
.brand-mark{width:32px;height:32px;border-radius:8px;background:linear-gradient(135deg,var(--brand) 0%,var(--brand-2) 100%);display:flex;align-items:center;justify-content:center;color:#0c160c;font-weight:800;font-size:14px;letter-spacing:-.5px;box-shadow:0 4px 12px rgba(74,222,128,.25)}
.brand-text{display:flex;flex-direction:column;line-height:1.1}
.brand-name{font-weight:700;font-size:15px;letter-spacing:-.3px}
.brand-sub{font-size:11px;color:var(--text-3);font-weight:500}
.nav-links{display:flex;gap:2px;flex-wrap:wrap}
.nav-links a{color:var(--text-2);text-decoration:none;font-size:13px;font-weight:500;padding:7px 12px;border-radius:var(--r-sm);transition:all .15s}
.nav-links a:hover{color:var(--text);background:var(--surface-2)}
.nav-links a.active{color:var(--brand);background:var(--brand-soft)}
.container{max-width:980px;margin:0 auto;padding:24px 20px 60px}
h1{font-size:24px;font-weight:700;letter-spacing:-.5px;margin-bottom:4px}
h2{font-size:15px;font-weight:600;margin-bottom:14px;color:var(--text);letter-spacing:-.2px}
.card{background:var(--surface);border:1px solid var(--border);border-radius:var(--r-lg);padding:20px;margin-bottom:16px;box-shadow:var(--shadow-sm);transition:box-shadow .2s}
.card:hover{box-shadow:var(--shadow)}
.card.hero{padding:28px;background:linear-gradient(135deg,var(--surface) 0%,var(--surface-2) 100%)}
.grid{display:grid;grid-template-columns:repeat(auto-fit,minmax(160px,1fr));gap:12px;margin-bottom:16px}
.hmi-grid{display:grid;grid-template-columns:repeat(auto-fit,minmax(160px,1fr));gap:12px;margin-bottom:16px}
.stat{background:var(--surface);border:1px solid var(--border);border-radius:var(--r);padding:14px;box-shadow:var(--shadow-sm);transition:all .2s}
.stat:hover{border-color:var(--border-strong);transform:translateY(-1px);box-shadow:var(--shadow)}
.stat-label,.card-label{font-size:11px;font-weight:600;text-transform:uppercase;letter-spacing:.06em;color:var(--text-3);margin-bottom:6px}
.stat-value,.card-value{font-size:22px;font-weight:700;color:var(--text);letter-spacing:-.4px;line-height:1.1}
.stat-unit,.card-unit{font-size:12px;font-weight:500;color:var(--text-3);margin-left:3px}
.stat-meta{font-size:12px;color:var(--text-2);margin-top:6px;display:flex;align-items:center;gap:6px}
.stat.ok .stat-value,.stat-ok .card-value,.stat-ok .stat-value{color:var(--ok)}
.stat.warn .stat-value,.stat-warn .card-value,.stat-warn .stat-value{color:var(--warn)}
.stat.crit .stat-value,.stat-crit .card-value,.stat-crit .stat-value{color:var(--err)}
.stat.info .stat-value,.stat-info .card-value,.stat-info .stat-value{color:var(--info)}
.hmi-grid>.card{background:var(--surface);border:1px solid var(--border);border-radius:var(--r);padding:14px;margin:0;box-shadow:none}
.hmi-grid>.card:hover{border-color:var(--border-strong)}
label{display:block;font-size:12px;font-weight:600;color:var(--text-2);margin:14px 0 6px;letter-spacing:.01em}
input[type=text],input[type=password],input[type=number],select{width:100%;padding:9px 12px;background:var(--surface);border:1px solid var(--border);border-radius:var(--r-sm);color:var(--text);font-size:14px;outline:none;transition:all .15s;font-family:inherit}
input:focus,select:focus{border-color:var(--brand);box-shadow:0 0 0 3px rgba(99,102,241,.12)}
input[type=checkbox]{width:18px;height:18px;accent-color:var(--brand)}
.btn{display:inline-flex;align-items:center;justify-content:center;padding:9px 16px;border:1px solid transparent;border-radius:var(--r-sm);font-size:13px;font-weight:600;cursor:pointer;gap:6px;transition:all .15s;text-decoration:none;font-family:inherit;letter-spacing:.01em}
.btn:active{transform:scale(.98)}
.btn-primary{background:var(--brand);color:#fff;box-shadow:0 1px 2px rgba(99,102,241,.2)}
.btn-primary:hover{background:#5558e3}
.btn-outline{background:transparent;border-color:var(--border);color:var(--text)}
.btn-outline:hover{background:var(--surface-2);border-color:var(--border-strong)}
.btn-orange{background:var(--warn);color:#fff} .btn-orange:hover{filter:brightness(.95)}
.btn-red{background:var(--err);color:#fff} .btn-red:hover{filter:brightness(.95)}
.btn-blue{background:var(--info);color:#fff} .btn-blue:hover{filter:brightness(.95)}
.row{display:flex;gap:8px;margin-top:14px;flex-wrap:wrap}
.badge{display:inline-flex;align-items:center;gap:4px;padding:3px 9px;border-radius:99px;font-size:11px;font-weight:600;letter-spacing:.02em}
.badge-success{background:var(--ok-soft);color:var(--ok)}
.badge-warning{background:var(--warn-soft);color:var(--warn)}
.badge-danger{background:var(--err-soft);color:var(--err)}
.dot,.status-dot{display:inline-block;width:8px;height:8px;border-radius:50%;flex-shrink:0}
.dot.online,.status-dot.online{background:var(--ok);box-shadow:0 0 0 3px var(--ok-soft)}
.dot.offline,.status-dot.offline{background:var(--err);box-shadow:0 0 0 3px var(--err-soft)}
.dot.pulse{animation:pulse 2s infinite}
@keyframes pulse{0%,100%{opacity:1}50%{opacity:.4}}
.msg{padding:12px 16px;border-radius:var(--r-sm);margin-bottom:14px;font-size:13px;font-weight:500;display:flex;align-items:center;gap:8px}
.msg-ok{background:var(--ok-soft);color:var(--ok);border:1px solid var(--ok)}
.msg-err{background:var(--err-soft);color:var(--err);border:1px solid var(--err)}
pre{background:var(--surface-2);padding:14px;border-radius:var(--r-sm);font-size:12px;overflow-x:auto;white-space:pre-wrap;color:var(--text-2);max-height:340px;overflow-y:auto;border:1px solid var(--border);font-family:'JetBrains Mono','Menlo',monospace;line-height:1.6}
.bar-bg{background:var(--border);border-radius:99px;height:8px;overflow:hidden;margin-top:14px}
.bar-fill{height:100%;border-radius:99px;transition:width .8s cubic-bezier(.16,1,.3,1);background:linear-gradient(90deg,var(--ok),var(--warn) 60%,var(--err))}
.flex{display:flex}.items-center{align-items:center}.justify-between{justify-content:space-between}.gap-2{gap:8px}.gap-3{gap:12px}.gap-4{gap:16px}
.mt{margin-top:14px}.mt-2{margin-top:8px}.mb-4{margin-bottom:16px}
.text-muted{color:var(--text-2)}.text-sm{font-size:12px}.text-xs{font-size:11px}
.fill-display{display:flex;align-items:baseline;gap:4px;margin:18px 0;font-feature-settings:'tnum'}
.fill-num{font-size:64px;font-weight:800;letter-spacing:-3px;line-height:1;background:linear-gradient(135deg,var(--brand),var(--brand-2));-webkit-background-clip:text;background-clip:text;color:transparent;transition:all .3s}
.fill-pct{font-size:24px;font-weight:600;color:var(--text-3)}
.section-info{padding:14px;background:var(--surface-2);border:1px solid var(--border);border-radius:var(--r);margin-top:14px}
.kv{display:flex;justify-content:space-between;padding:6px 0;font-size:13px}
.kv:not(:last-child){border-bottom:1px solid var(--border)}
.kv-key{color:var(--text-2)} .kv-val{font-weight:600;color:var(--text);font-feature-settings:'tnum'}
table{width:100%;border-collapse:collapse;font-size:13px}
th,td{padding:10px 8px;text-align:left;border-bottom:1px solid var(--border)}
th{font-weight:600;color:var(--text-2);font-size:11px;text-transform:uppercase;letter-spacing:.06em}
tr:last-child td{border-bottom:none}
.live-tag{display:inline-flex;align-items:center;gap:6px;padding:4px 10px;background:var(--ok-soft);color:var(--ok);border-radius:99px;font-size:11px;font-weight:600;letter-spacing:.02em}
.fade-in{animation:fadeIn .4s ease}
@keyframes fadeIn{from{opacity:0;transform:translateY(4px)}to{opacity:1;transform:none}}
.stat .val{font-size:20px;font-weight:700;color:var(--text);line-height:1.1}
.stat .lbl{font-size:11px;font-weight:600;text-transform:uppercase;letter-spacing:.06em;color:var(--text-3);margin-top:4px}
.table-wrap{overflow-x:auto;border-radius:var(--r-sm)}
)css";
  webServer.send(200, "text/css; charset=UTF-8", CSS_P);
}

String htmlNav(const String& active) {
  String h = "<div class='topbar'><div class='topbar-inner'>";
  h += "<a href='/' class='brand'>";
  h += "<div class='brand-mark'>r</div>";
  h += "<div class='brand-text'><span class='brand-name'>routeOS</span><span class='brand-sub'>" + cfgDeviceCode + "</span></div>";
  h += "</a>";
  h += "<div class='nav-links'>";
  auto link = [&](const String& href, const String& label, const String& id) {
    h += "<a href='" + href + "'" + (active == id ? " class='active'" : "") + ">" + label + "</a>";
  };
  link("/", "Dashboard", "dash");
  link("/diagnostics", "Diagnostics", "diag");
  link("/calibrate", "Calibrate", "cal");
  link("/config", "Config", "config");
  link("/mqtt", "MQTT", "mqtt");
  link("/simulator", "Simulator", "sim");
  link("/backup", "Backup", "backup");
  link("/portal", "Security", "portal");
  h += "</div></div></div>";
  return h;
}

String htmlEnd() { return "</body></html>"; }

String fillColor(float pct) {
  if (pct >= 90) return "#dc2626";
  if (pct >= 70) return "#d97706";
  if (pct >= 50) return "#ca8a04";
  return "#16a34a";
}

String fillBadge(float pct) {
  if (pct >= 90) return "<span class='badge badge-danger'>Critical</span>";
  if (pct >= 70) return "<span class='badge badge-warning'>High</span>";
  if (pct >= 50) return "<span class='badge badge-warning'>Medium</span>";
  return "<span class='badge badge-success'>Low</span>";
}

// ═══════════════════════════════════════════════════════════════════════════════
//  WEB ROUTES
// ═══════════════════════════════════════════════════════════════════════════════

void handleDashboard() {
  if (!checkAuth()) return;

  String h = htmlHead("Dashboard") + htmlNav("dash");
  h += "<div class='container'>";

  // Header
  h += "<div class='flex items-center justify-between mb-4'>";
  h += "<div><h1>" + cfgDeviceCode + "</h1><span class='text-sm text-muted'>routeOS v" FIRMWARE_VERSION " · <span id='uptime'>--</span></span></div>";
  h += "<span class='live-tag'><span class='dot online pulse' style='width:6px;height:6px'></span>LIVE</span>";
  h += "</div>";

  // Per-sensor row — one card per active slot
  h += "<div class='card'><div class='flex items-center justify-between mb-4'><h2>Ultrasonic Sensors (" + String(cfgNumSensors) + ")</h2><span class='text-xs text-muted'>per-sensor live readings</span></div>";
  h += "<div style='display:grid;grid-template-columns:repeat(" + String(cfgNumSensors) + ",1fr);gap:12px'>";
  for (int i = 0; i < cfgNumSensors; i++) {
    h += "<div class='stat' id='s-card-" + String(i) + "' style='text-align:center'>";
    h += "<div class='stat-label'>" + String(cfgSensorTags[i]) + "</div>";
    h += "<div class='stat-value' style='font-size:26px;margin:8px 0'><span id='s-dist-" + String(i) + "'>--</span><span class='stat-unit'>cm</span></div>";
    h += "<div class='bar-bg' style='margin-top:8px'><div id='s-bar-" + String(i) + "' class='bar-fill' style='width:0%'></div></div>";
    h += "<div class='stat-meta' style='justify-content:center'><span id='s-fill-" + String(i) + "'>--</span>% fill &middot; GPIO " + String(cfgTrigPins[i]) + "/" + String(cfgEchoPins[i]) + "</div>";
    h += "</div>";
  }
  h += "</div></div>";

  // Combined hero card
  h += "<div class='card hero'>";
  h += "<div class='flex items-center justify-between'><h2>Combined Fill Level (averaged across " + String(cfgNumSensors) + " sensor" + (cfgNumSensors > 1 ? "s" : "") + ")</h2><span id='fill-badge' class='badge badge-success'>--</span></div>";
  h += "<div class='fill-display'><span id='fill-num' class='fill-num'>--</span><span class='fill-pct'>%</span></div>";
  h += "<div class='text-sm text-muted'>Avg distance <span id='fill-dist'>--</span> cm across <span id='fill-valid'>--</span> sensor(s) &middot; refreshes every <span id='fill-rate'>--</span></div>";
  h += "<div class='bar-bg'><div id='fill-bar' class='bar-fill' style='width:0%'></div></div>";
  h += "</div>";

  // Stats grid
  h += "<div class='grid'>";
  h += "<div id='stat-bin' class='stat ok'><div class='stat-label'>Bin Docked</div><div class='stat-value' style='font-size:18px;display:flex;align-items:center;gap:8px'><span id='bin-dot' class='dot online'></span><span id='bin-text'>--</span></div><div class='stat-meta'><span id='bin-meta'>--</span></div></div>";
  h += "<div class='stat info'><div class='stat-label'>WiFi Signal</div><div class='stat-value'><span id='rssi'>--</span><span class='stat-unit'>dBm</span></div><div class='stat-meta'><span id='wifi-dot' class='dot offline'></span><span id='wifi-text'>--</span></div></div>";
  h += "<div class='stat'><div class='stat-label'>Telemetry Published</div><div class='stat-value'><span id='pub-count'>--</span></div><div class='stat-meta'>Total messages sent</div></div>";
  h += "<div class='stat'><div class='stat-label'>Last Publish</div><div class='stat-value' style='font-size:18px'><span id='last-pub'>--</span></div><div class='stat-meta'>MQTT: <span id='mqtt-text'>--</span></div></div>";
  h += "</div>";

  // Actions
  h += "<div class='card'><h2>Quick Actions</h2>";
  h += "<div class='row'>";
  h += "<form method='POST' action='/api/force-publish' style='display:inline'><button class='btn btn-primary' type='submit'>Force Publish</button></form>";
  h += "<form method='POST' action='/api/read-sensors' style='display:inline'><button class='btn btn-outline' type='submit'>Refresh Now</button></form>";
  h += "<form method='POST' action='/api/reboot' onsubmit=\"return confirm('Reboot routeOS?')\" style='display:inline'><button class='btn btn-red' type='submit'>Reboot Device</button></form>";
  h += "</div></div>";

  // Live update script — polls /api/status, updates DOM in place
  h += R"(<script>
const $=(id)=>document.getElementById(id);
const fmtUptime=(s)=>{if(s>=86400)return Math.floor(s/86400)+'d '+Math.floor((s%86400)/3600)+'h';if(s>=3600)return Math.floor(s/3600)+'h '+Math.floor((s%3600)/60)+'m';if(s>=60)return Math.floor(s/60)+'m '+(s%60)+'s';return s+'s'};
const fillBadgeClass=(p)=>{if(p>=90)return['badge-danger','Critical'];if(p>=70)return['badge-warning','High'];if(p>=50)return['badge-warning','Medium'];return['badge-success','Low']};
const tick=async()=>{try{
const r=await fetch('/api/status',{credentials:'include'});if(!r.ok)return;
const d=await r.json();
const fp=d.fill_level_percent;const fpVal=fp>=0?fp:0;
$('fill-num').textContent=fp>=0?fp.toFixed(1):'N/A';
$('fill-bar').style.width=fpVal+'%';
const [bc,bt]=fillBadgeClass(fpVal);
const fb=$('fill-badge');fb.className='badge '+bc;fb.textContent=bt;
$('fill-dist').textContent=d.distance_cm>=0?d.distance_cm.toFixed(1):'N/A';
$('uptime').textContent='Uptime '+fmtUptime(d.uptime_sec);
$('rssi').textContent=d.rssi||0;
$('wifi-dot').className='dot '+(d.wifi_connected?'online':'offline');
$('wifi-text').textContent=d.wifi_connected?'Connected':'Disconnected';
$('bin-dot').className='dot '+(d.bin_present?'online':'offline');
$('bin-text').textContent=d.bin_present?'Docked':'Absent';
$('bin-meta').textContent=d.bin_present?'Bin at station':'No bin present';
$('stat-bin').className='stat '+(d.bin_present?'ok':'warn');
$('pub-count').textContent=d.publish_count||0;
const lp=d.last_publish_sec;$('last-pub').textContent=lp>=0?(lp<60?lp+'s ago':lp<3600?Math.floor(lp/60)+'m ago':Math.floor(lp/3600)+'h ago'):'Never';
$('mqtt-text').textContent=d.mqtt_connected?'Online':'Offline';
$('mqtt-text').style.color=d.mqtt_connected?'var(--ok)':'var(--err)';
// Per-sensor row
if(d.sensors && d.sensors.length){
  let valid=0;
  for(let i=0;i<d.sensors.length;i++){
    const s=d.sensors[i];
    const distEl=$('s-dist-'+i); const fillEl=$('s-fill-'+i); const barEl=$('s-bar-'+i); const card=$('s-card-'+i);
    const ok=(s.distance>=0);
    if(ok) valid++;
    if(distEl) distEl.textContent=ok?s.distance.toFixed(1):'FAIL';
    if(fillEl) fillEl.textContent=s.fill>=0?s.fill.toFixed(1):'--';
    if(barEl) barEl.style.width=(s.fill>=0?s.fill:0)+'%';
    if(card) card.className='stat '+(ok?'ok':'crit');
  }
  const vEl=$('fill-valid'); if(vEl) vEl.textContent=valid+'/'+d.sensors.length;
}
}catch(e){}};
$('fill-rate').textContent='~1s';
tick();setInterval(tick,1500);
</script>)";
  h += "</div>" + htmlEnd();
  webServer.send(200, "text/html; charset=UTF-8", h);
}

void handleConfig() {
  if (!checkAuth()) return;

  String msg = "";
  if (webServer.hasArg("saved")) msg = "<div class='msg msg-ok'>Configuration saved! Restart device to apply WiFi changes.</div>";

  String h = htmlHead("Configuration") + htmlNav("config");
  h += "<div class='container'>" + msg;

  h += "<form method='POST' action='/api/save-config'>";

  // Device
  h += "<div class='card'><h2>Device</h2>";
  h += "<label>Device Code</label><input type='text' name='deviceCode' value='" + cfgDeviceCode + "'>";
  h += "</div>";

  // WiFi
  h += "<div class='card'><h2>WiFi Station</h2>";
  h += "<label>SSID</label><input type='text' name='wifiSsid' value='" + cfgWifiSsid + "'>";
  h += "<label>Password</label><input type='password' name='wifiPass' value='" + cfgWifiPass + "'>";
  h += "</div>";

  // Sensor
  h += "<div class='card'><h2>Sensor</h2>";
  h += "<label>Bin Height (cm)</label><input type='number' name='binHeight' value='" + String(binHeightCm, 1) + "' step='0.1'>";
  h += "<label>Sensor Offset (cm)</label><input type='number' name='sensorOffset' value='" + String(sensorOffsetCm, 1) + "' step='0.1'>";
  // Sensor failure behaviour
  h += "<div style='margin-top:12px;padding:12px;background:var(--surface-2);border-radius:var(--r-sm)'>";
  h += "<label style='font-weight:600;margin-bottom:8px'>Sensor Failure Behaviour</label>";

  h += "<label style='display:flex;align-items:center;gap:8px;margin:0 0 4px;cursor:pointer;font-weight:500'>";
  h += "<input type='checkbox' name='perSensorFull' value='1'" + String(cfgPerSensorAssumeFull ? " checked" : "") + ">";
  h += "<span>Per-sensor fallback: failed sensor &rarr; treat as 100% full</span></label>";
  h += "<div class='text-sm text-muted' style='margin:0 0 10px 26px'>When one sensor fails to echo (contents blocking it), count it as 100% in the average. Recommended ON.</div>";

  h += "<label style='display:flex;align-items:center;gap:8px;margin:0 0 4px;cursor:pointer;font-weight:500'>";
  h += "<input type='checkbox' name='assumeFull' value='1'" + String(cfgAssumeFullOnFail ? " checked" : "") + ">";
  h += "<span>Global fallback: ALL sensors fail &rarr; report 100% full</span></label>";
  h += "<div class='text-sm text-muted' style='margin:0 0 0 26px'>Last-resort fallback when every sensor fails. Reports 100% instead of N/A.</div>";
  h += "</div>";

  // Sensor refresh rate
  unsigned long currentInterval = getSensorReadIntervalMs();
  h += "<div style='margin-top:12px;padding:12px;background:var(--surface-2);border-radius:var(--r-sm)'>";
  h += "<label>Sensor Refresh Rate</label>";
  h += "<select name='sensorInt' style='width:100%;padding:8px 12px;border:1px solid var(--border);border-radius:var(--r-sm);font-size:14px;margin-top:4px'>";
  h += "<option value='500'" + String(cfgSensorIntervalMs == 500 ? " selected" : "") + ">Real-time (0.5 sec)</option>";
  h += "<option value='1000'" + String(cfgSensorIntervalMs == 1000 ? " selected" : "") + ">Fast (1 sec) — recommended</option>";
  h += "<option value='2000'" + String(cfgSensorIntervalMs == 2000 ? " selected" : "") + ">Normal (2 sec)</option>";
  h += "<option value='5000'" + String(cfgSensorIntervalMs == 5000 ? " selected" : "") + ">Slow (5 sec)</option>";
  h += "<option value='10000'" + String(cfgSensorIntervalMs == 10000 ? " selected" : "") + ">Power-saver (10 sec)</option>";
  h += "</select>";
  h += "<div style='margin-top:8px;padding:8px;background:var(--surface);border:1px solid var(--border);border-radius:var(--r-sm)'>";
  h += "<div class='text-sm' style='display:flex;justify-content:space-between'>";
  h += "<span>Current effective rate:</span><strong>" + String(currentInterval) + " ms</strong></div>";
  h += "</div>";
  h += "<label style='margin-top:8px;display:inline-flex;align-items:center;gap:8px;cursor:pointer'>";
  h += "<input type='checkbox' name='verbose' value='1'" + String(cfgVerboseLogging ? " checked" : "") + " style='width:18px;height:18px'>";
  h += "<span>Verbose serial logging</span></label>";
  h += "<div class='text-sm text-muted' style='margin-top:4px'>Detailed per-sample logs (recommended OFF at fast refresh rates to avoid serial spam).</div>";
  h += "</div>";
  h += "</div>";

  // Sensor Slots section
  h += "<div class='card'><h2>Sensor Slots</h2>";
  h += "<p class='text-sm text-muted' style='margin-bottom:12px'>Configure up to " + String(MAX_SENSORS) + " sensor slots. Slot 2 defaults to <strong>ToF (I2C)</strong> for the CJMCU-531/VL53L0X &mdash; TRIG/ECHO are ignored for ToF slots. <strong style='color:var(--warn)'>Warning:</strong> GPIOs 34, 35, 36, 39 are input-only &mdash; never assign to TRIG.</p>";
  h += "<label>Active Sensor Count</label>";
  h += "<select name='sensorCount'>";
  for (int i = 1; i <= MAX_SENSORS; i++) {
    h += "<option value='" + String(i) + "'" + (cfgNumSensors == i ? " selected" : "") + ">" + String(i) + " sensor" + (i > 1 ? "s" : "") + "</option>";
  }
  h += "</select>";
  for (int i = 0; i < MAX_SENSORS; i++) {
    String dimmed = (i >= (int)cfgNumSensors) ? ";opacity:.5" : "";
    h += "<div style='margin-top:12px;padding:12px;background:var(--surface-2);border-radius:var(--r-sm)" + dimmed + "'>";
    h += "<div style='font-weight:600;margin-bottom:8px'>Slot " + String(i);
    if (i == 0) h += " <span class='badge badge-success'>primary</span>";
    else if (i == 1) h += " <span class='badge'>secondary</span>";
    else if (i == 2) h += " <span class='badge' style='background:var(--info,#0ea5e9)'>ToF</span>";
    h += "</div>";
    h += "<div style='display:grid;grid-template-columns:110px 80px 80px 1fr;gap:8px'>";
    h += "<div><label style='margin:0 0 4px'>Type</label><select name='type" + String(i) + "'>";
    h += "<option value='0'" + String(cfgSensorTypes[i] == SENSOR_TYPE_US  ? " selected" : "") + ">Ultrasonic</option>";
    h += "<option value='1'" + String(cfgSensorTypes[i] == SENSOR_TYPE_TOF ? " selected" : "") + ">ToF / I2C</option>";
    h += "</select></div>";
    h += "<div><label style='margin:0 0 4px'>TRIG GPIO</label><input type='number' name='trig" + String(i) + "' value='" + String(cfgTrigPins[i]) + "' min='0' max='39'></div>";
    h += "<div><label style='margin:0 0 4px'>ECHO GPIO</label><input type='number' name='echo" + String(i) + "' value='" + String(cfgEchoPins[i]) + "' min='0' max='39'></div>";
    h += "<div><label style='margin:0 0 4px'>Label</label><input type='text' name='tag" + String(i) + "' value='" + String(cfgSensorTags[i]) + "' maxlength='31'></div>";
    h += "</div></div>";
  }
  h += "</div>";

  h += "<button class='btn btn-primary' type='submit'>Save Configuration</button>";
  h += "</form>";

  h += "<form method='POST' action='/api/factory-reset' class='mt'>";
  h += "<button class='btn btn-red' type='submit' onclick=\"return confirm('Reset ALL settings to factory defaults?')\">Factory Reset</button>";
  h += "</form>";

  h += "</div>" + htmlEnd();
  webServer.send(200, "text/html; charset=UTF-8", h);
}

void handleMqtt() {
  if (!checkAuth()) return;

  String msg = "";
  if (webServer.hasArg("saved")) msg = "<div class='msg msg-ok'>MQTT settings saved!</div>";
  if (webServer.hasArg("tested")) {
    msg = mqtt.connected()
      ? "<div class='msg msg-ok'>MQTT connection test successful!</div>"
      : "<div class='msg msg-err'>MQTT connection test failed (rc=" + String(mqtt.state()) + ")</div>";
  }

  String h = htmlHead("MQTT Settings") + htmlNav("mqtt");
  h += "<div class='container'>" + msg;

  h += "<form method='POST' action='/api/save-mqtt'>";
  h += "<div class='card'><h2>MQTT Broker</h2>";
  h += "<label>Broker Host</label><input type='text' name='mqttBroker' value='" + cfgMqttBroker + "'>";
  h += "<label>Port</label><input type='number' name='mqttPort' value='" + String(cfgMqttPort) + "'>";
  h += "<label>Username</label><input type='text' name='mqttUser' value='" + cfgMqttUser + "'>";
  h += "<label>Password</label><input type='password' name='mqttPass' value='" + cfgMqttPass + "'>";
  h += "<label>Topic Prefix</label><input type='text' name='mqttTopic' value='" + cfgMqttTopic + "'>";
  h += "<div style='font-size:12px;color:var(--text-2);margin-top:4px'>Full topic: " + cfgMqttTopic + cfgDeviceCode + "</div>";
  h += "</div>";
  h += "<div class='row'>";
  h += "<button class='btn btn-primary' type='submit'>Save MQTT</button>";
  h += "</div></form>";

  h += "<div class='row'>";
  h += "<form method='POST' action='/api/test-mqtt'><button class='btn btn-blue' type='submit'>Test Connection</button></form>";
  h += "<form method='POST' action='/api/force-publish'><button class='btn btn-orange' type='submit'>Force Publish</button></form>";
  h += "</div>";

  h += "</div>" + htmlEnd();
  webServer.send(200, "text/html; charset=UTF-8", h);
}

void handleCalibrate() {
  if (!checkAuth()) return;

  String msg = "";
  if (webServer.hasArg("done")) msg = "<div class='msg msg-ok'>Calibration saved!</div>";

  String h = htmlHead("Calibration") + htmlNav("cal");
  h += "<div class='container'>" + msg;

  // Current values
  h += "<div class='card'><h2>Current Calibration</h2>";
  h += "<div class='grid'>";
  h += "<div class='stat info'><div class='stat-label'>Bin Height (empty)</div><div class='stat-value'>" + String(binHeightCm, 1) + "<span class='stat-unit'>cm</span></div></div>";
  h += "<div class='stat'><div class='stat-label'>Full Distance</div><div class='stat-value'>" + (cfgFullDistanceCm > 0 ? String(cfgFullDistanceCm, 1) : "N/A") + "<span class='stat-unit'>cm</span></div></div>";
  h += "<div class='stat'><div class='stat-label'>Bin Present</div><div class='stat-value' style='font-size:16px'>" + String(binPresent ? "Docked" : "Absent") + "</div></div>";
  h += "<div class='stat'><div class='stat-label'>Live Distance</div><div class='stat-value'>" + (currentDistanceCm > 0 ? String(currentDistanceCm, 1) : "N/A") + "<span class='stat-unit'>cm</span></div></div>";
  h += "</div></div>";

  // Guided calibration wizard
  h += "<div class='card'><h2>Guided Calibration Wizard</h2>";
  h += "<p class='text-sm text-muted' style='margin-bottom:16px'>Follow each step in order. Each step takes 5 seconds of sampling.</p>";

  // Step 1: Empty bin
  h += "<div style='padding:12px;background:var(--surface-2);border-radius:var(--r-sm);margin-bottom:12px'>";
  h += "<div class='flex items-center gap-2'><strong style='color:var(--brand)'>Step 1:</strong> <span>Empty Bin (lid closed)</span></div>";
  h += "<p class='text-sm text-muted' style='margin:4px 0 8px'>Remove everything from the bin and close the lid. Measures the distance to the bottom = bin height.</p>";
  h += "<form method='POST' action='/api/cal-step'><input type='hidden' name='step' value='empty'>";
  h += "<button class='btn btn-primary' type='submit'>Calibrate Empty Bin</button></form></div>";

  // Step 2: Full bin
  h += "<div style='padding:12px;background:var(--surface-2);border-radius:var(--r-sm);margin-bottom:12px'>";
  h += "<div class='flex items-center gap-2'><strong style='color:var(--brand)'>Step 2:</strong> <span>Full Bin (lid closed)</span></div>";
  h += "<p class='text-sm text-muted' style='margin:4px 0 8px'>Fill the bin completely and close the lid. Measures the minimum distance = 100% full reference.</p>";
  h += "<form method='POST' action='/api/cal-step'><input type='hidden' name='step' value='full'>";
  h += "<button class='btn btn-orange' type='submit'>Calibrate Full Bin</button></form></div>";

  // Step 3: Lid open
  h += "<div style='padding:12px;background:var(--surface-2);border-radius:var(--r-sm);margin-bottom:12px;opacity:0.4'>";
  h += "<div class='flex items-center gap-2'><strong style='color:var(--text-3)'>Step 3:</strong> <span style='color:var(--text-3)'>Lid Open &mdash; N/A (docking station, no lid)</span></div>";
  h += "</div>";

  // Step 4: Lid closed
  h += "<div style='padding:12px;background:var(--surface-2);border-radius:var(--r-sm);margin-bottom:12px;opacity:0.4'>";
  h += "<div class='flex items-center gap-2'><strong style='color:var(--text-3)'>Step 4:</strong> <span style='color:var(--text-3)'>Lid Closed &mdash; N/A (docking station, no lid)</span></div>";
  h += "</div>";
  h += "</div>";

  // Manual override
  h += "<div class='card'><h2>Manual Override</h2>";
  h += "<form method='POST' action='/api/save-config'>";
  h += "<label>Bin Height (cm)</label><input type='number' name='binHeight' value='" + String(binHeightCm, 1) + "' step='0.1'>";
  h += "<label>Full Distance (cm)</label><input type='number' name='fullDist' value='" + String(cfgFullDistanceCm, 1) + "' step='0.1'>";
  h += "<label>Sensor Offset (cm)</label><input type='number' name='sensorOffset' value='" + String(sensorOffsetCm, 1) + "' step='0.1'>";
  h += "<input type='hidden' name='deviceCode' value='" + cfgDeviceCode + "'>";
  h += "<input type='hidden' name='wifiSsid' value='" + cfgWifiSsid + "'>";
  h += "<input type='hidden' name='wifiPass' value='" + cfgWifiPass + "'>";
  h += "<button class='btn btn-blue mt' type='submit'>Save Manual Values</button>";
  h += "</form></div>";

  h += "</div>" + htmlEnd();
  webServer.send(200, "text/html; charset=UTF-8", h);
}

void handlePortalSecurity() {
  if (!checkAuth()) return;

  String msg = "";
  if (webServer.hasArg("saved")) msg = "<div class='msg msg-ok'>Portal credentials updated! Use new credentials on next login.</div>";

  String h = htmlHead("Portal Security") + htmlNav("portal");
  h += "<div class='container'>" + msg;

  h += "<form method='POST' action='/api/save-portal'>";
  h += "<div class='card'><h2>Portal Login Credentials</h2>";
  h += "<p style='color:#94a3b8;font-size:13px;margin-bottom:12px'>Change the username and password used to access this configuration portal.</p>";
  h += "<label>Username</label><input type='text' name='portalUser' value='" + cfgPortalUser + "'>";
  h += "<label>New Password</label><input type='password' name='portalPass' placeholder='Enter new password'>";
  h += "<label>Confirm Password</label><input type='password' name='portalPassConfirm' placeholder='Confirm new password'>";
  h += "</div>";
  h += "<button class='btn btn-primary' type='submit'>Update Credentials</button>";
  h += "</form>";

  h += "<div class='card mt'><h2>Access Point Info</h2>";
  h += "<div class='grid'>";
  h += "<div class='stat info'><div class='stat-label'>AP SSID</div><div class='stat-value' style='font-size:16px'>" + apSsid + "</div></div>";
  h += "<div class='stat'><div class='stat-label'>Portal IP</div><div class='stat-value' style='font-size:16px'>192.168.4.1</div></div>";
  h += "</div></div>";

  h += "</div>" + htmlEnd();
  webServer.send(200, "text/html; charset=UTF-8", h);
}

// ═══════════════════════════════════════════════════════════════════════════════
//  SIMULATOR WEB UI + API
// ═══════════════════════════════════════════════════════════════════════════════

void handleSimulator() {
  if (!checkAuth()) return;

  String msg = "";
  if (webServer.hasArg("saved")) msg = "<div class='msg msg-ok'>Simulator configuration saved.</div>";
  if (webServer.hasArg("on"))    msg = "<div class='msg msg-ok'>Simulator ENABLED \u2014 telemetry is now synthetic. All MQTT payloads include <code>simulated:true</code>.</div>";
  if (webServer.hasArg("off"))   msg = "<div class='msg msg-ok'>Simulator disabled \u2014 reading real sensors.</div>";

  String h = htmlHead("Simulator") + htmlNav("sim");
  h += "<div class='container'>" + msg;

  // Status banner
  if (simEnabled) {
    h += "<div style='padding:14px 18px;background:#7c3aed22;border:1px solid #7c3aed;border-radius:var(--r);margin-bottom:16px;display:flex;align-items:center;gap:12px'>";
    h += "<span style='font-size:22px'>&#x1F9EA;</span>";
    h += "<div><strong style='color:#a78bfa'>SIMULATOR ACTIVE</strong> &mdash; All sensor readings are synthetic. MQTT payloads include <code>\"simulated\":true</code>. Disable before deploying in the field.</div>";
    h += "</div>";
  } else {
    h += "<div style='padding:14px 18px;background:var(--surface-2);border:1px solid var(--border);border-radius:var(--r);margin-bottom:16px;display:flex;align-items:center;gap:12px'>";
    h += "<span style='font-size:22px'>&#x1F52C;</span>";
    h += "<div><strong>Simulator Disabled</strong> &mdash; Device is reading from real physical sensors.</div>";
    h += "</div>";
  }

  // Toggle card
  h += "<div class='card'><h2>Simulator Control</h2>";
  h += "<div class='row'>";
  if (!simEnabled) {
    h += "<form method='POST' action='/api/sim-toggle'><input type='hidden' name='enable' value='1'>";
    h += "<button class='btn btn-orange' type='submit'>&#x25B6; Enable Simulator</button></form>";
  } else {
    h += "<form method='POST' action='/api/sim-toggle'><input type='hidden' name='enable' value='0'>";
    h += "<button class='btn btn-primary' type='submit'>&#x23F9; Disable Simulator</button></form>";
  }
  h += "</div>";
  h += "<p class='text-sm text-muted' style='margin-top:10px'>When enabled, the device skips GPIO sensor reads and generates synthetic fill levels from the selected waveform. WiFi, MQTT, and the web portal remain fully functional.</p>";
  h += "</div>";

  // Quick presets
  h += "<div class='card'><h2>Quick Presets</h2>";
  h += "<p class='text-sm text-muted' style='margin-bottom:12px'>Apply a common simulation scenario to all slots instantly.</p>";
  h += "<div class='row' style='flex-wrap:wrap;gap:8px'>";
  auto preset = [&](const String& val, const String& label, const String& btnClass) {
    h += "<form method='POST' action='/api/sim-preset'><input type='hidden' name='preset' value='" + val + "'>";
    h += "<button class='btn " + btnClass + "' type='submit'>" + label + "</button></form>";
  };
  preset("slow_fill",  "Slow Fill (10 min)",  "btn-outline");
  preset("fast_fill",  "Fast Fill (60 sec)",  "btn-outline");
  preset("half_full",  "Static 50%",          "btn-blue");
  preset("critical",   "Critical 95%",        "btn-red");
  preset("sine_day",   "Day Cycle (sine)",    "btn-blue");
  preset("random",     "Random Walk",         "btn-outline");
  h += "</div></div>";

  // Per-slot waveform config
  static const char* modeNames[] = {
    "0 \u2013 Static (fixed %)",
    "1 \u2013 Sawtooth (fill &amp; reset)",
    "2 \u2013 Sine wave",
    "3 \u2013 Random Walk",
    "4 \u2013 Custom (manual slider)"
  };

  h += "<form method='POST' action='/api/save-sim'>";
  for (int i = 0; i < cfgNumSensors; i++) {
    String si = String(i);
    h += "<div class='card'>";
    h += "<div class='flex items-center justify-between mb-4'>";
    h += "<h2>Slot " + si + " &mdash; " + String(cfgSensorTags[i]) + "</h2>";
    h += "<div id='sim-fill-" + si + "' style='font-size:32px;font-weight:800;color:var(--brand)'>";
    h += simEnabled ? String(simCurrentFill[i], 1) + "%" : "--";
    h += "</div></div>";

    // Waveform selector
    h += "<label>Waveform Mode</label>";
    h += "<select name='mode" + si + "' id='simmode" + si + "' onchange='showSlot(" + si + ")'>";
    for (int m = 0; m <= 4; m++) {
      h += "<option value='" + String(m) + "'" + (simCfg[i].mode == m ? " selected" : "") + ">" + modeNames[m] + "</option>";
    }
    h += "</select>";

    // Parameter grid
    h += "<div style='display:grid;grid-template-columns:1fr 1fr 1fr;gap:12px;margin-top:12px'>";
    h += "<div><label>Static Fill %</label><input type='number' name='sf" + si + "' value='" + String(simCfg[i].staticFill, 1) + "' min='0' max='100' step='0.5'></div>";
    h += "<div><label>Period (ms)</label><input type='number' name='period" + si + "' value='" + String((int)simCfg[i].periodMs) + "' min='1000' max='3600000' step='1000'></div>";
    h += "<div><label>Noise (cm)</label><input type='number' name='noise" + si + "' value='" + String(simCfg[i].noiseCm, 1) + "' min='0' max='20' step='0.5'></div>";
    h += "<div><label>Min Fill %</label><input type='number' name='min" + si + "' value='" + String(simCfg[i].minFill, 1) + "' min='0' max='100' step='1'></div>";
    h += "<div><label>Max Fill %</label><input type='number' name='max" + si + "' value='" + String(simCfg[i].maxFill, 1) + "' min='0' max='100' step='1'></div>";
    h += "<div id='slider-wrap-" + si + "'><label>Manual Fill %</label>";
    h += "<div style='display:flex;align-items:center;gap:8px'>";
    h += "<input type='range' name='custom" + si + "' id='slider" + si + "' min='0' max='100' step='1' value='" + String((int)simCurrentFill[i]) + "' style='flex:1;accent-color:var(--brand)' oninput=\"document.getElementById('slv" + si + "').textContent=this.value+'%'\">";
    h += "<span id='slv" + si + "' style='font-weight:600;min-width:40px'>" + String((int)simCurrentFill[i]) + "%</span></div>";
    h += "</div>";
    h += "</div>";

    // Fill bar
    float barPct = simEnabled ? constrain(simCurrentFill[i], 0.0f, 100.0f) : 0.0f;
    h += "<div class='bar-bg' style='margin-top:12px'><div id='sim-bar-" + si + "' class='bar-fill' style='width:" + String(barPct, 0) + "%'></div></div>";
    h += "</div>"; // card
  }

  h += "<div class='row' style='margin-top:16px'>";
  h += "<button class='btn btn-primary' type='submit'>Save Waveform Config</button>";
  h += "</div></form>";

  // Live JS
  h += "<script>\n";
  h += "function showSlot(i){\n";
  h += "  const m=parseInt(document.getElementById('simmode'+i).value);\n";
  h += "  const w=document.getElementById('slider-wrap-'+i);\n";
  h += "  if(w) w.style.display=(m===4)?'':'none';\n";
  h += "}\n";
  h += "for(let i=0;i<" + String(cfgNumSensors) + ";i++) showSlot(i);\n";

  if (simEnabled) {
    h += "const tick=async()=>{try{\n";
    h += "  const r=await fetch('/api/status',{credentials:'include'});if(!r.ok)return;\n";
    h += "  const d=await r.json();\n";
    h += "  if(d.sensors){for(let i=0;i<d.sensors.length;i++){\n";
    h += "    const s=d.sensors[i];\n";
    h += "    const fe=document.getElementById('sim-fill-'+i); if(fe) fe.textContent=(s.fill>=0?s.fill.toFixed(1):'--')+'%';\n";
    h += "    const be=document.getElementById('sim-bar-'+i); if(be) be.style.width=(s.fill>=0?s.fill:0)+'%';\n";
    h += "  }}}catch(e){}};\n";
    h += "tick();setInterval(tick,1500);\n";
  }
  h += "</script>";

  h += "</div>" + htmlEnd();
  webServer.send(200, "text/html; charset=UTF-8", h);
}

void handleApiSimToggle() {
  if (!checkAuth()) return;
  bool enable = (webServer.arg("enable") == "1");
  simEnabled = enable;
  if (enable) {
    simEpoch = millis();
    Serial.println("[sim] Simulator ENABLED via web portal");
  } else {
    Serial.println("[sim] Simulator DISABLED via web portal — resuming real sensors");
  }
  saveConfig();
  webServer.sendHeader("Location", String("/simulator?") + (enable ? "on=1" : "off=1"));
  webServer.send(302, "text/plain", "OK");
}

void handleApiSaveSim() {
  if (!checkAuth()) return;
  for (int i = 0; i < cfgNumSensors; i++) {
    String si = String(i);
    if (webServer.hasArg("mode"   + si)) simCfg[i].mode       = (uint8_t)constrain(webServer.arg("mode"   + si).toInt(), 0, 4);
    if (webServer.hasArg("sf"     + si)) simCfg[i].staticFill = constrain(webServer.arg("sf"     + si).toFloat(), 0.0f, 100.0f);
    if (webServer.hasArg("period" + si)) simCfg[i].periodMs   = max((float)webServer.arg("period" + si).toInt(), 1000.0f);
    if (webServer.hasArg("noise"  + si)) simCfg[i].noiseCm    = max(webServer.arg("noise"  + si).toFloat(), 0.0f);
    if (webServer.hasArg("min"    + si)) simCfg[i].minFill    = constrain(webServer.arg("min"    + si).toFloat(), 0.0f, 100.0f);
    if (webServer.hasArg("max"    + si)) simCfg[i].maxFill    = constrain(webServer.arg("max"    + si).toFloat(), 0.0f, 100.0f);
    if (webServer.hasArg("custom" + si)) {
      float v = constrain(webServer.arg("custom" + si).toFloat(), 0.0f, 100.0f);
      simCurrentFill[i]    = v;
      simCfg[i].staticFill = v;
    }
    // Ensure min <= max
    if (simCfg[i].maxFill < simCfg[i].minFill) simCfg[i].maxFill = simCfg[i].minFill + 1.0f;
  }
  simEpoch = millis();  // restart waveforms from now
  saveConfig();
  webServer.sendHeader("Location", "/simulator?saved=1");
  webServer.send(302, "text/plain", "Saved");
}

void handleApiSimPreset() {
  if (!checkAuth()) return;
  String preset = webServer.arg("preset");

  // Apply preset to all active slots
  for (int i = 0; i < MAX_SENSORS; i++) {
    simCfg[i].noiseCm = 1.0f;
    simCfg[i].minFill = 0.0f;
    simCfg[i].maxFill = 100.0f;
  }

  if (preset == "slow_fill") {
    for (int i = 0; i < MAX_SENSORS; i++) { simCfg[i].mode = SIM_MODE_SAWTOOTH; simCfg[i].periodMs = 600000.0f; }
  } else if (preset == "fast_fill") {
    for (int i = 0; i < MAX_SENSORS; i++) { simCfg[i].mode = SIM_MODE_SAWTOOTH; simCfg[i].periodMs = 60000.0f; }
  } else if (preset == "half_full") {
    for (int i = 0; i < MAX_SENSORS; i++) { simCfg[i].mode = SIM_MODE_STATIC; simCfg[i].staticFill = 50.0f; simCurrentFill[i] = 50.0f; }
  } else if (preset == "critical") {
    for (int i = 0; i < MAX_SENSORS; i++) { simCfg[i].mode = SIM_MODE_STATIC; simCfg[i].staticFill = 95.0f; simCurrentFill[i] = 95.0f; }
  } else if (preset == "sine_day") {
    for (int i = 0; i < MAX_SENSORS; i++) {
      simCfg[i].mode = SIM_MODE_SINE; simCfg[i].periodMs = 86400000.0f;  // 24 h
      simCfg[i].minFill = 10.0f; simCfg[i].maxFill = 90.0f;
    }
  } else if (preset == "random") {
    for (int i = 0; i < MAX_SENSORS; i++) {
      simCfg[i].mode = SIM_MODE_RANDOM_WALK; simCfg[i].minFill = 0.0f; simCfg[i].maxFill = 100.0f;
      simCurrentFill[i] = 50.0f;
    }
  }

  simEpoch = millis();
  simEnabled = true;
  saveConfig();
  webServer.sendHeader("Location", "/simulator?on=1");
  webServer.send(302, "text/plain", "Preset applied");
}

// ─── API ENDPOINTS ──────────────────────────────────────────────────────────

void handleApiForcePublish() {
  if (!checkAuth()) return;
  updateSensors();
  bool ok = publishTelemetry();
  String referer = webServer.header("Referer");
  if (referer.length() == 0) referer = "/";
  webServer.sendHeader("Location", referer);
  webServer.send(302, "text/plain", ok ? "Published" : "Failed");
}

void handleApiReadSensors() {
  if (!checkAuth()) return;
  updateSensors();
  webServer.sendHeader("Location", "/");
  webServer.send(302, "text/plain", "OK");
}

void handleApiSaveConfig() {
  if (!checkAuth()) return;

  if (webServer.hasArg("deviceCode")) cfgDeviceCode = webServer.arg("deviceCode");
  if (webServer.hasArg("wifiSsid"))   cfgWifiSsid = webServer.arg("wifiSsid");
  if (webServer.hasArg("wifiPass") && webServer.arg("wifiPass").length() > 0) {
    cfgWifiPass = webServer.arg("wifiPass");
  }
  if (webServer.hasArg("binHeight"))    binHeightCm = webServer.arg("binHeight").toFloat();
  if (webServer.hasArg("sensorOffset")) sensorOffsetCm = webServer.arg("sensorOffset").toFloat();
  if (webServer.hasArg("fullDist"))     cfgFullDistanceCm = webServer.arg("fullDist").toFloat();
  cfgAssumeFullOnFail = webServer.hasArg("assumeFull");
  cfgPerSensorAssumeFull = webServer.hasArg("perSensorFull");
  if (webServer.hasArg("sensorInt"))  cfgSensorIntervalMs = webServer.arg("sensorInt").toInt();
  cfgVerboseLogging = webServer.hasArg("verbose");

  // Sensor slot configuration
  if (webServer.hasArg("sensorCount")) {
    int n = webServer.arg("sensorCount").toInt();
    if (n >= 1 && n <= MAX_SENSORS) cfgNumSensors = (uint8_t)n;
  }
  auto isInputOnly = [](uint8_t g){ return g == 34 || g == 35 || g == 36 || g == 39; };
  for (int i = 0; i < MAX_SENSORS; i++) {
    String ti = "trig" + String(i);
    String ei = "echo" + String(i);
    String gi = "tag"  + String(i);
    if (webServer.hasArg(ti)) {
      uint8_t pin = (uint8_t)webServer.arg(ti).toInt();
      if (!isInputOnly(pin)) cfgTrigPins[i] = pin;
      else Serial.printf("[config] Rejected trig%d=%d (input-only GPIO)\n", i, pin);
    }
    if (webServer.hasArg(ei)) cfgEchoPins[i] = (uint8_t)webServer.arg(ei).toInt();
    if (webServer.hasArg(gi) && webServer.arg(gi).length() > 0)
      strlcpy(cfgSensorTags[i], webServer.arg(gi).c_str(), sizeof(cfgSensorTags[0]));
    String typi = "type" + String(i);
    if (webServer.hasArg(typi)) cfgSensorTypes[i] = (uint8_t)webServer.arg(typi).toInt();
  }
  setupSensorPins();

  saveConfig();
  webServer.sendHeader("Location", "/config?saved=1");
  webServer.send(302, "text/plain", "Saved");
}

void handleApiSaveMqtt() {
  if (!checkAuth()) return;

  if (webServer.hasArg("mqttBroker")) cfgMqttBroker = webServer.arg("mqttBroker");
  if (webServer.hasArg("mqttPort"))   cfgMqttPort = webServer.arg("mqttPort").toInt();
  if (webServer.hasArg("mqttUser"))   cfgMqttUser = webServer.arg("mqttUser");
  if (webServer.hasArg("mqttPass") && webServer.arg("mqttPass").length() > 0) {
    cfgMqttPass = webServer.arg("mqttPass");
  }
  if (webServer.hasArg("mqttTopic"))  cfgMqttTopic = webServer.arg("mqttTopic");

  saveConfig();
  rebuildMqttTopic();  // refresh cached publish topic

  // Reconnect MQTT with new settings
  mqtt.disconnect();
  mqttConnectedFlag = false;
  mqtt.setServer(cfgMqttBroker.c_str(), cfgMqttPort);
  connectMqttNonBlocking();

  webServer.sendHeader("Location", "/mqtt?saved=1");
  webServer.send(302, "text/plain", "Saved");
}

void handleApiTestMqtt() {
  if (!checkAuth()) return;

  mqtt.disconnect();
  mqttConnectedFlag = false;
  mqtt.setServer(cfgMqttBroker.c_str(), cfgMqttPort);
  connectMqttNonBlocking();

  webServer.sendHeader("Location", "/mqtt?tested=1");
  webServer.send(302, "text/plain", "Tested");
}

void handleApiCalibrate() {
  if (!checkAuth()) return;
  String result = runCalibration();
  String h = htmlHead("Calibration Result") + htmlNav("cal");
  h += "<div class='container'>";
  h += "<div class='card'><h2>Quick Calibration Result</h2><pre>" + result + "</pre></div>";
  h += "<a href='/calibrate?done=1' class='btn btn-primary'>Back to Calibration</a>";
  h += "</div>" + htmlEnd();
  webServer.send(200, "text/html; charset=UTF-8", h);
}

void handleApiCalStep() {
  if (!checkAuth()) return;

  String step = webServer.arg("step");
  Serial.printf("[cal] === GUIDED CALIBRATION: step=%s ===\n", step.c_str());

  String title, summary;
  String resultClass = "msg-ok";

  if (step == "empty") {
    // Step 1: Empty bin, lid closed — measure bin height
    title = "Step 1: Empty Bin";
    CalResult r = calibrateRead(5, true);

    if (r.median > 0) {
      binHeightCm = r.median;
      saveConfig();
      summary = "Bin height set to " + String(binHeightCm, 1) + " cm (distance to bottom when empty)";
      Serial.printf("[cal] Empty bin height: %.1f cm\n", binHeightCm);
    } else {
      summary = "ERROR: Could not get valid ultrasonic readings. Check sensor.";
      resultClass = "msg-err";
    }
    summary += "\n\n" + r.log;

  } else if (step == "full") {
    // Step 2: Full bin, lid closed — measure full distance
    title = "Step 2: Full Bin";
    CalResult r = calibrateRead(5, true);

    if (r.median > 0) {
      cfgFullDistanceCm = r.median;
      saveConfig();
      float pct = calculateFillPercent(r.median);
      summary = "Full distance set to " + String(cfgFullDistanceCm, 1) + " cm\n";
      summary += "With bin height " + String(binHeightCm, 1) + " cm, this reads as " + String(pct, 1) + "% fill";
      Serial.printf("[cal] Full distance: %.1f cm\n", cfgFullDistanceCm);
    } else {
      // No echo at all = sensor is blocked = very full
      cfgFullDistanceCm = 1.0f;
      saveConfig();
      summary = "Ultrasonic got no readings — bin contents are blocking the sensor.\nFull distance set to 1.0 cm (minimum).";
      Serial.println("[cal] Full distance: 1.0 cm (no echo — extremely full)");
    }
    summary += "\n\n" + r.log;

  } else if (step == "lid_open" || step == "lid_closed") {
    title = "N/A";
    summary = "Lid calibration not applicable for docking station design.\nThis unit uses bin-present detection based on ultrasonic distance.";
    resultClass = "msg-warn";

  } else {
    title = "Error";
    summary = "Unknown calibration step: " + step;
    resultClass = "msg-err";
  }

  // Render result page
  String h = htmlHead(title) + htmlNav("cal");
  h += "<div class='container'>";
  h += "<div class='" + resultClass + "' style='padding:12px;border-radius:var(--radius-md);margin-bottom:12px'><strong>" + title + "</strong></div>";
  h += "<div class='card'><h2>Results</h2><pre>" + summary + "</pre></div>";
  h += "<a href='/calibrate' class='btn btn-primary'>Back to Calibration</a>";
  h += "</div>" + htmlEnd();
  webServer.send(200, "text/html; charset=UTF-8", h);
}

void handleApiSavePortal() {
  if (!checkAuth()) return;

  String newUser = webServer.arg("portalUser");
  String newPass = webServer.arg("portalPass");
  String confirm = webServer.arg("portalPassConfirm");

  if (newPass.length() > 0 && newPass != confirm) {
    String h = htmlHead("Error") + htmlNav("portal");
    h += "<div class='container'><div class='msg msg-err'>Passwords do not match!</div>";
    h += "<a href='/portal' class='btn btn-blue'>Go Back</a></div>" + htmlEnd();
    webServer.send(200, "text/html; charset=UTF-8", h);
    return;
  }

  if (newUser.length() > 0) cfgPortalUser = newUser;
  if (newPass.length() > 0) cfgPortalPass = newPass;

  saveConfig();
  webServer.sendHeader("Location", "/portal?saved=1");
  webServer.send(302, "text/plain", "Saved");
}

void handleApiReboot() {
  if (!checkAuth()) return;

  String h = htmlHead("Rebooting") + htmlNav("dash");
  h += "<div class='container'><div class='msg msg-ok'>Device is rebooting in 2 seconds...</div>";
  h += "<p class='text-sm text-muted'>Reconnect to the WiFi AP after restart.</p></div>" + htmlEnd();
  webServer.send(200, "text/html; charset=UTF-8", h);

  delay(2000);
  ESP.restart();
}

void handleApiFactoryReset() {
  if (!checkAuth()) return;

  prefs.begin("ecoroute", false);
  prefs.clear();
  prefs.end();

  String h = htmlHead("Factory Reset") + htmlNav("config");
  h += "<div class='container'><div class='msg msg-ok'>Factory reset complete. Device will restart in 3 seconds...</div></div>" + htmlEnd();
  webServer.send(200, "text/html; charset=UTF-8", h);

  delay(3000);
  ESP.restart();
}

// ═══════════════════════════════════════════════════════════════════════════════
//  OTA UPDATE — HTTP /update endpoint (browser or serial-web.py push)
// ═══════════════════════════════════════════════════════════════════════════════

// ═══════════════════════════════════════════════════════════════════════════════
//  CONFIG EXPORT / IMPORT
// ═══════════════════════════════════════════════════════════════════════════════

void handleApiExportConfig() {
  if (!checkAuth()) return;

  JsonDocument doc;
  doc["_version"]       = FIRMWARE_VERSION;
  doc["deviceCode"]     = cfgDeviceCode;
  doc["wifiSsid"]       = cfgWifiSsid;
  doc["wifiPass"]       = cfgWifiPass;
  doc["mqttBroker"]     = cfgMqttBroker;
  doc["mqttPort"]       = cfgMqttPort;
  doc["mqttUser"]       = cfgMqttUser;
  doc["mqttPass"]       = cfgMqttPass;
  doc["mqttTopic"]      = cfgMqttTopic;
  doc["portalUser"]     = cfgPortalUser;
  doc["portalPass"]     = cfgPortalPass;
  doc["binHeight"]      = binHeightCm;
  doc["sensorOffset"]   = sensorOffsetCm;
  doc["apMode"]         = cfgApMode;
  doc["assumeFull"]     = cfgAssumeFullOnFail;
  doc["perSensorFull"]  = cfgPerSensorAssumeFull;
  doc["fullDist"]       = cfgFullDistanceCm;
  doc["sensorInt"]      = cfgSensorIntervalMs;
  doc["verbose"]        = cfgVerboseLogging;
  doc["numSensors"]     = cfgNumSensors;
  doc["simEnabled"]     = simEnabled;

  JsonArray sensorsArr = doc["sensors"].to<JsonArray>();
  for (int i = 0; i < MAX_SENSORS; i++) {
    JsonObject s = sensorsArr.add<JsonObject>();
    s["trig"] = cfgTrigPins[i];
    s["echo"] = cfgEchoPins[i];
    s["tag"]  = cfgSensorTags[i];
  }

  JsonArray simArr = doc["sim"].to<JsonArray>();
  for (int i = 0; i < MAX_SENSORS; i++) {
    JsonObject s = simArr.add<JsonObject>();
    s["mode"]       = simCfg[i].mode;
    s["staticFill"] = simCfg[i].staticFill;
    s["periodMs"]   = simCfg[i].periodMs;
    s["noiseCm"]    = simCfg[i].noiseCm;
    s["minFill"]    = simCfg[i].minFill;
    s["maxFill"]    = simCfg[i].maxFill;
  }

  String out;
  serializeJsonPretty(doc, out);

  String filename = "routeos-config-" + cfgDeviceCode + ".json";
  webServer.sendHeader("Content-Disposition", "attachment; filename=\"" + filename + "\"");
  webServer.send(200, "application/json; charset=UTF-8", out);
}

// Buffer for incoming config import (filled by handleImportUpload, consumed by handleApiImportConfig)
static String importBuffer;

void handleImportUpload() {
  HTTPUpload& upload = webServer.upload();
  if (upload.status == UPLOAD_FILE_START) {
    importBuffer = "";
    Serial.println("[import] Config upload start");
  } else if (upload.status == UPLOAD_FILE_WRITE) {
    for (size_t i = 0; i < upload.currentSize; i++) {
      importBuffer += (char)upload.buf[i];
    }
  } else if (upload.status == UPLOAD_FILE_END) {
    Serial.printf("[import] Config upload complete: %u bytes\n", upload.totalSize);
  }
}

void handleApiImportConfig() {
  if (!checkAuth()) return;

  if (importBuffer.isEmpty()) {
    webServer.send(400, "application/json", "{\"ok\":false,\"error\":\"No config data received\"}");
    return;
  }

  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, importBuffer);
  importBuffer = "";  // free memory immediately

  if (err) {
    webServer.send(400, "application/json", "{\"ok\":false,\"error\":\"Invalid JSON\"}");
    return;
  }

  // Apply each field only if present — never overwrite with garbage
  if (doc["deviceCode"].is<const char*>())  cfgDeviceCode  = doc["deviceCode"].as<String>();
  if (doc["wifiSsid"].is<const char*>())    cfgWifiSsid    = doc["wifiSsid"].as<String>();
  if (doc["wifiPass"].is<const char*>() && strlen(doc["wifiPass"]) > 0)
    cfgWifiPass = doc["wifiPass"].as<String>();
  if (doc["mqttBroker"].is<const char*>())  cfgMqttBroker  = doc["mqttBroker"].as<String>();
  if (doc["mqttPort"].is<int>())            cfgMqttPort    = doc["mqttPort"];
  if (doc["mqttUser"].is<const char*>())    cfgMqttUser    = doc["mqttUser"].as<String>();
  if (doc["mqttPass"].is<const char*>() && strlen(doc["mqttPass"]) > 0)
    cfgMqttPass = doc["mqttPass"].as<String>();
  if (doc["mqttTopic"].is<const char*>())   cfgMqttTopic   = doc["mqttTopic"].as<String>();
  if (doc["portalUser"].is<const char*>())  cfgPortalUser  = doc["portalUser"].as<String>();
  if (doc["portalPass"].is<const char*>() && strlen(doc["portalPass"]) > 0)
    cfgPortalPass = doc["portalPass"].as<String>();
  if (doc["binHeight"].is<float>())         binHeightCm         = doc["binHeight"];
  if (doc["sensorOffset"].is<float>())      sensorOffsetCm      = doc["sensorOffset"];
  if (doc["apMode"].is<int>())              cfgApMode           = (uint8_t)(int)doc["apMode"];
  if (doc["assumeFull"].is<bool>())         cfgAssumeFullOnFail    = doc["assumeFull"];
  if (doc["perSensorFull"].is<bool>())      cfgPerSensorAssumeFull = doc["perSensorFull"];
  if (doc["fullDist"].is<float>())          cfgFullDistanceCm   = doc["fullDist"];
  if (doc["sensorInt"].is<int>())           cfgSensorIntervalMs = (uint32_t)(int)doc["sensorInt"];
  if (doc["verbose"].is<bool>())            cfgVerboseLogging   = doc["verbose"];
  if (doc["simEnabled"].is<bool>())         simEnabled          = doc["simEnabled"];

  if (doc["numSensors"].is<int>()) {
    int n = doc["numSensors"];
    if (n >= 1 && n <= MAX_SENSORS) cfgNumSensors = (uint8_t)n;
  }

  if (doc["sensors"].is<JsonArray>()) {
    auto isInputOnly = [](uint8_t g){ return g == 34 || g == 35 || g == 36 || g == 39; };
    int i = 0;
    for (JsonObject s : doc["sensors"].as<JsonArray>()) {
      if (i >= MAX_SENSORS) break;
      if (s["trig"].is<int>()) {
        uint8_t pin = (uint8_t)(int)s["trig"];
        if (!isInputOnly(pin)) cfgTrigPins[i] = pin;
      }
      if (s["echo"].is<int>()) cfgEchoPins[i] = (uint8_t)(int)s["echo"];
      if (s["tag"].is<const char*>())
        strlcpy(cfgSensorTags[i], s["tag"], sizeof(cfgSensorTags[0]));
      i++;
    }
  }

  if (doc["sim"].is<JsonArray>()) {
    int i = 0;
    for (JsonObject s : doc["sim"].as<JsonArray>()) {
      if (i >= MAX_SENSORS) break;
      if (s["mode"].is<int>())        simCfg[i].mode       = (uint8_t)(int)s["mode"];
      if (s["staticFill"].is<float>()) simCfg[i].staticFill = s["staticFill"];
      if (s["periodMs"].is<float>())   simCfg[i].periodMs   = s["periodMs"];
      if (s["noiseCm"].is<float>())    simCfg[i].noiseCm    = s["noiseCm"];
      if (s["minFill"].is<float>())    simCfg[i].minFill    = s["minFill"];
      if (s["maxFill"].is<float>())    simCfg[i].maxFill    = s["maxFill"];
      i++;
    }
  }

  setupSensorPins();
  saveConfig();

  webServer.send(200, "application/json",
    "{\"ok\":true,\"msg\":\"Config imported and saved — reboot to fully apply all changes\"}");
}

// ═══════════════════════════════════════════════════════════════════════════════
//  FIRMWARE ROLLBACK
// ═══════════════════════════════════════════════════════════════════════════════

void handleApiRollback() {
  if (!checkAuth()) return;

  const esp_partition_t* running = esp_ota_get_running_partition();
  const esp_partition_t* candidate = nullptr;

  esp_partition_iterator_t it = esp_partition_find(
    ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_ANY, nullptr);
  while (it != nullptr) {
    const esp_partition_t* p = esp_partition_get(it);
    if (p != running &&
        p->subtype >= ESP_PARTITION_SUBTYPE_APP_OTA_0 &&
        p->subtype <= ESP_PARTITION_SUBTYPE_APP_OTA_15) {
      esp_ota_img_states_t state;
      if (esp_ota_get_state_partition(p, &state) == ESP_OK &&
          state == ESP_OTA_IMG_VALID) {
        candidate = p;
        break;
      }
    }
    it = esp_partition_next(it);
  }
  esp_partition_iterator_release(it);

  if (!candidate) {
    webServer.send(400, "application/json",
      "{\"ok\":false,\"error\":\"No valid previous OTA partition found. Perform at least one OTA update first.\"}");
    return;
  }

  esp_err_t err = esp_ota_set_boot_partition(candidate);
  if (err != ESP_OK) {
    webServer.send(500, "application/json",
      "{\"ok\":false,\"error\":\"esp_ota_set_boot_partition failed\"}");
    return;
  }

  webServer.send(200, "application/json",
    "{\"ok\":true,\"msg\":\"Rollback boot partition set — rebooting to previous firmware\"}");
  delay(500);
  ESP.restart();
}

// ═══════════════════════════════════════════════════════════════════════════════
//  BACKUP & RESTORE PAGE
// ═══════════════════════════════════════════════════════════════════════════════

void handleBackup() {
  if (!checkAuth()) return;
  String h = htmlHead("Backup &amp; Restore") + htmlNav("backup");
  h += R"html(
<div class='container'>
  <div class='card' style='margin-bottom:16px'>
    <h2>Export Configuration</h2>
    <p class='text-sm text-muted'>Download a JSON backup of all device settings: WiFi, MQTT, sensor GPIO pins, calibration, and simulator config.</p>
    <div class='btn-row' style='margin-top:12px'>
      <a href='/api/export-config' class='btn btn-primary'>Download Config JSON</a>
    </div>
  </div>
  <div class='card' style='margin-bottom:16px'>
    <h2>Import Configuration</h2>
    <p class='text-sm text-muted'>Upload a previously exported JSON to restore or clone settings. Changes are saved to NVS immediately.</p>
    <form id='importForm' method='POST' action='/api/import-config' enctype='multipart/form-data'>
      <div class='field'><label>Config JSON file</label>
        <input type='file' name='config' accept='.json,application/json' required></div>
      <div class='btn-row' style='margin-top:12px'>
        <button type='submit' class='btn btn-primary'>Import &amp; Apply</button>
      </div>
    </form>
    <div id='importResult' style='display:none;margin-top:8px'></div>
    <script>
      document.getElementById('importForm').addEventListener('submit', function(e) {
        e.preventDefault();
        var fd = new FormData(this);
        fetch('/api/import-config', {method:'POST', body:fd})
          .then(function(r){ return r.json(); })
          .then(function(d){
            var el = document.getElementById('importResult');
            el.style.display='block';
            el.className = d.ok ? 'msg-ok' : 'msg-err';
            el.textContent = d.ok ? d.msg : d.error;
          }).catch(function(){
            var el = document.getElementById('importResult');
            el.style.display='block'; el.className='msg-err';
            el.textContent = 'Request failed';
          });
      });
    </script>
  </div>
  <div class='card'>
    <h2>Firmware Rollback</h2>
    <p class='text-sm text-muted'>Revert to the previously flashed OTA firmware. Only available after at least one OTA update has been performed successfully.</p>
    <div id='rbResult' style='display:none;margin-top:8px'></div>
    <div class='btn-row' style='margin-top:12px'>
      <button onclick='doRollback()' class='btn btn-danger'>Rollback Firmware</button>
    </div>
    <script>
      function doRollback() {
        if (!confirm('Roll back to the previous firmware? The device will reboot immediately.')) return;
        fetch('/api/rollback', {method:'POST'})
          .then(function(r){ return r.json(); })
          .then(function(d){
            var el = document.getElementById('rbResult');
            el.style.display='block';
            el.className = d.ok ? 'msg-ok' : 'msg-err';
            el.textContent = d.ok ? d.msg : d.error;
          }).catch(function(){
            var el = document.getElementById('rbResult');
            el.style.display='block'; el.className='msg-err';
            el.textContent = 'Request failed';
          });
      }
    </script>
  </div>
</div>)html";
  h += htmlEnd();
  webServer.send(200, "text/html; charset=UTF-8", h);
}

// Upload handler: called repeatedly with chunks of the binary
void handleOtaUpload() {
  HTTPUpload& upload = webServer.upload();
  if (upload.status == UPLOAD_FILE_START) {
    Serial.printf("[ota] HTTP upload start: %s\n", upload.filename.c_str());
    // Pass UPDATE_SIZE_UNKNOWN so the library figures out size from chunks
    if (!Update.begin(UPDATE_SIZE_UNKNOWN)) {
      Serial.print("[ota] begin failed: ");
      Update.printError(Serial);
    }
  } else if (upload.status == UPLOAD_FILE_WRITE) {
    if (Update.write(upload.buf, upload.currentSize) != upload.currentSize) {
      Serial.print("[ota] write error: ");
      Update.printError(Serial);
    }
  } else if (upload.status == UPLOAD_FILE_END) {
    if (Update.end(true)) {
      Serial.printf("[ota] HTTP update success: %u bytes written\n", upload.totalSize);
    } else {
      Serial.print("[ota] end error: ");
      Update.printError(Serial);
    }
  }
}

// Completion handler: sent after all chunks — JSON response then reboot
void handleOtaComplete() {
  if (!checkAuth()) return;
  if (Update.hasError()) {
    webServer.send(400, "application/json", "{\"ok\":false,\"error\":\"OTA failed\"}");
    return;
  }
  webServer.sendHeader("Connection", "close");
  webServer.send(200, "application/json", "{\"ok\":true,\"msg\":\"Update applied — rebooting\"}");
  delay(500);
  ESP.restart();
}

// GET /update — simple upload form for browser-based OTA
void handleOtaPage() {
  if (!checkAuth()) return;
  String h = htmlHead("OTA Update") + htmlNav("config");
  h += R"(
<div class='container'>
  <h2>Firmware OTA Update</h2>
  <p class='text-sm text-muted'>Upload a compiled <code>.bin</code> file to update the firmware over WiFi.</p>
  <form method='POST' action='/update' enctype='multipart/form-data'>
    <div class='field'><label>Firmware binary (.bin)</label>
      <input type='file' name='firmware' accept='.bin' required></div>
    <div class='btn-row' style='margin-top:12px'>
      <button type='submit' class='btn btn-primary'>Upload &amp; Flash</button>
    </div>
  </form>
  <p class='text-sm text-muted' style='margin-top:8px'>Device will reboot automatically after a successful update.</p>
</div>)";
  h += htmlEnd();
  webServer.send(200, "text/html; charset=UTF-8", h);
}

// ═══════════════════════════════════════════════════════════════════════════════
//  OTA — MQTT pull (backend publishes cmd to ecoroute/cmd/<device_code>)
// ═══════════════════════════════════════════════════════════════════════════════

// Runs in main loop (not in callback) to avoid stack overflow from HTTPClient
void performMqttOta(const String& url) {
  Serial.printf("[ota] MQTT OTA start — fetching: %s\n", url.c_str());

  HTTPClient http;
  http.begin(url);
  http.setTimeout(30000);
  int httpCode = http.GET();

  if (httpCode != 200) {
    Serial.printf("[ota] HTTP GET failed (code %d)\n", httpCode);
    http.end();
    return;
  }

  int contentLength = http.getSize();
  Serial.printf("[ota] Content-Length: %d bytes\n", contentLength);

  bool canBegin = (contentLength > 0)
    ? Update.begin(contentLength)
    : Update.begin(UPDATE_SIZE_UNKNOWN);

  if (!canBegin) {
    Serial.print("[ota] Update.begin failed: ");
    Update.printError(Serial);
    http.end();
    return;
  }

  WiFiClient* stream = http.getStreamPtr();
  size_t written = Update.writeStream(*stream);
  Serial.printf("[ota] Written: %u bytes\n", (unsigned int)written);

  if (!Update.end(true)) {
    Serial.print("[ota] Update.end error: ");
    Update.printError(Serial);
    http.end();
    return;
  }

  if (!Update.isFinished()) {
    Serial.println("[ota] Update not finished — something went wrong");
    http.end();
    return;
  }

  http.end();
  Serial.println("[ota] MQTT OTA success — publishing event and rebooting");

  // Publish completion event before rebooting
  if (mqtt.connected()) {
    String topic = "ecoroute/trash_can/" + cfgDeviceCode;
    String payload = "{\"event\":\"ota_complete\",\"version\":\"" FIRMWARE_VERSION "\",\"source\":\"mqtt\"}";
    mqtt.publish(topic.c_str(), payload.c_str());
    delay(200);
  }
  delay(500);
  ESP.restart();
}

// MQTT message callback — handles incoming commands on ecoroute/cmd/<device_code>
void onMqttMessage(char* topic, byte* payload, unsigned int length) {
  // Safety: null-terminate the payload
  char buf[512];
  size_t copyLen = (length < sizeof(buf) - 1) ? length : sizeof(buf) - 1;
  memcpy(buf, payload, copyLen);
  buf[copyLen] = '\0';

  Serial.printf("[mqtt] Message on %s: %s\n", topic, buf);

  JsonDocument doc;
  if (deserializeJson(doc, buf) != DeserializationError::Ok) {
    Serial.println("[mqtt] cmd parse error");
    return;
  }

  const char* cmd = doc["cmd"];
  if (!cmd) return;

  if (strcmp(cmd, "ota") == 0) {
    const char* url = doc["url"];
    if (url && strlen(url) > 0) {
      otaPendingUrl = String(url);
      otaPending = true;
      Serial.printf("[mqtt] OTA pending: %s\n", url);
    }
  } else if (strcmp(cmd, "reboot") == 0) {
    Serial.println("[mqtt] cmd=reboot — rebooting");
    delay(200);
    ESP.restart();
  } else if (strcmp(cmd, "find_me") == 0) {
    triggerFindMe();
  } else {
    Serial.printf("[mqtt] Unknown cmd: %s\n", cmd);
  }
}

void handleApiJson() {
  if (!checkAuth()) return;

  JsonDocument doc;
  doc["device_code"]       = cfgDeviceCode;
  doc["firmware"]          = FIRMWARE_VERSION;
  doc["fill_level_percent"] = currentFillPercent;
  doc["distance_cm"]       = currentDistanceCm;
  doc["rssi"]              = currentRssi;
  doc["bin_present"]       = binPresent;
  doc["mqtt_connected"]    = mqttConnectedFlag;
  doc["wifi_connected"]    = wifiConnectedFlag;
  doc["uptime_sec"]        = millis() / 1000;
  doc["publish_count"]     = publishCount;
  doc["last_publish_sec"]  = lastPublishTime > 0 ? (long)((millis() - lastPublishTime) / 1000) : -1;
  doc["bin_height_cm"]     = binHeightCm;
  doc["sensor_offset_cm"]  = sensorOffsetCm;
  // Per-sensor breakdown for diagnostics UI
  JsonArray sensors = doc["sensors"].to<JsonArray>();
  for (int i = 0; i < cfgNumSensors; i++) {
    JsonObject s = sensors.add<JsonObject>();
    s["id"]       = cfgSensorTags[i];
    s["trig"]     = cfgTrigPins[i];
    s["echo"]     = cfgEchoPins[i];
    s["distance"] = sensorDistances[i];
    s["fill"]     = sensorFills[i];
  }

  char buf[768];
  serializeJson(doc, buf);
  webServer.send(200, "application/json", buf);
}

// ─── DIAGNOSTICS ────────────────────────────────────────────────────────────

DiagResult runDiagnostics() {
  DiagResult d;
  memset(&d, 0, sizeof(d));
  d.echoPin = digitalRead(ECHO_PIN);
  d.validCount = 0;
  d.minCm = 9999;
  d.maxCm = 0;
  for (int i = 0; i < 10; i++) {
    float dist = readDistanceSingle();
    d.readings[i] = dist;
    if (dist > 0) { d.validCount++; if (dist < d.minCm) d.minCm = dist; if (dist > d.maxCm) d.maxCm = dist; }
    delay(50);
  }
  d.ultrasonicOk = (d.validCount >= 3);
  d.medianCm = readDistanceMedian();
  return d;
}

void handleDiagnostics() {
  if (!checkAuth()) return;
  updateSensors();
  String fillCol = fillColor(currentFillPercent >= 0 ? currentFillPercent : 0);

  String h = htmlHead("Diagnostics") + htmlNav("diag");
  h += "<div class='container'>";
  h += "<div class='flex items-center justify-between mb-4'>";
  h += "<h1>Sensor Diagnostics</h1>";
  h += "<form method='POST' action='/api/run-diag'><button class='btn btn-primary' type='submit'>Run Full Test</button></form>";
  h += "</div>";
  h += "<div class='card'><div class='flex items-center justify-between mb-4'><h2>Live Averaged Readings</h2><span class='live-tag'><span class='dot online pulse' style='width:6px;height:6px'></span>LIVE</span></div>";
  h += "<div class='grid'>";
  h += "<div class='stat info'><div class='stat-label'>Avg Distance</div><div class='stat-value'><span id='d-dist'>--</span><span class='stat-unit'>cm</span></div></div>";
  h += "<div class='stat'><div class='stat-label'>Avg Fill Level</div><div class='stat-value' id='d-fill-wrap'><span id='d-fill'>--</span><span class='stat-unit'>%</span></div></div>";
  h += "<div class='stat ok' id='d-bin-wrap'><div class='stat-label'>Bin Docked</div><div class='stat-value' style='font-size:18px;display:flex;align-items:center;gap:8px'><span id='d-bin-dot' class='dot online'></span><span id='d-bin'>--</span></div></div>";
  h += "</div></div>";

  // Per-sensor live cards
  h += "<div class='card'><h2>Per-Sensor Readings (2 ultrasonic)</h2>";
  h += "<div class='grid'>";
  for (int i = 0; i < cfgNumSensors; i++) {
    h += "<div class='stat' id='us-card-" + String(i) + "'>";
    h += "<div class='stat-label'>Sensor " + String(i + 1) + " (" + String(cfgSensorTags[i]) + ")</div>";
    h += "<div class='stat-value'><span id='us-dist-" + String(i) + "'>--</span><span class='stat-unit'>cm</span></div>";
    h += "<div class='stat-meta'>GPIO " + String(cfgTrigPins[i]) + "/" + String(cfgEchoPins[i]) + " &middot; <span id='us-fill-" + String(i) + "'>--</span>% fill</div>";
    h += "</div>";
  }
  h += "</div></div>";

  h += "<div class='card'><h2>GPIO Pin Configuration</h2><table>";
  h += "<tr><th>Component</th><th>TRIG GPIO</th><th>ECHO GPIO</th><th>Direction</th></tr>";
  for (int i = 0; i < cfgNumSensors; i++) {
    h += "<tr><td>" + String(cfgSensorTags[i]) + "</td><td>" + String(cfgTrigPins[i]) + "</td><td>" + String(cfgEchoPins[i]) + "</td><td>OUT/IN</td></tr>";
  }
  h += "<tr><td>Status LED</td><td colspan='2'>" + String(LED_PIN) + "</td><td>OUTPUT</td></tr>";
  h += "<tr><td>Boot Button</td><td colspan='2'>" + String(CALIBRATE_BTN_PIN) + "</td><td>INPUT_PULLUP</td></tr>";
  h += "</table></div>";
  h += R"(<script>
const $=(id)=>document.getElementById(id);
const diag=async()=>{try{
const r=await fetch('/api/status',{credentials:'include'});if(!r.ok)return;
const d=await r.json();
$('d-dist').textContent=d.distance_cm>=0?d.distance_cm.toFixed(1):'N/A';
$('d-fill').textContent=d.fill_level_percent>=0?d.fill_level_percent.toFixed(1):'N/A';
$('d-bin').textContent=d.bin_present?'Docked':'Absent';
$('d-bin-dot').className='dot '+(d.bin_present?'online':'offline');
$('d-bin-wrap').className='stat '+(d.bin_present?'ok':'warn');
if(d.sensors && d.sensors.length){
  for(let i=0;i<d.sensors.length;i++){
    const s=d.sensors[i];
    const distEl=$('us-dist-'+i); const fillEl=$('us-fill-'+i); const card=$('us-card-'+i);
    if(distEl) distEl.textContent=s.distance>=0?s.distance.toFixed(1):'FAIL';
    if(fillEl) fillEl.textContent=s.fill>=0?s.fill.toFixed(1):'--';
    if(card) card.className='stat '+(s.distance>=0?'ok':'crit');
  }
}
}catch(e){}};
diag();setInterval(diag,1000);
</script>)";
  h += "</div>" + htmlEnd();
  webServer.send(200, "text/html; charset=UTF-8", h);
}

void handleApiRunDiag() {
  if (!checkAuth()) return;
  DiagResult d = runDiagnostics();
  String h = htmlHead("Diagnostics Report") + htmlNav("diag");
  h += "<div class='container'><h1 style='margin-bottom:16px'>Sensor Test Report</h1>";

  String usIcon = d.ultrasonicOk ? "<span class='badge badge-success'>PASS</span>" : "<span class='badge badge-danger'>FAIL</span>";
  h += "<div class='card'><div class='flex items-center justify-between mb-4'><h2>Ultrasonic Sensors (" + String(cfgNumSensors) + " active)</h2>" + usIcon + "</div>";
  h += "<p class='text-sm text-muted mb-4'>Primary: GPIO " + String(cfgTrigPins[0]) + " (TRIG) + GPIO " + String(cfgEchoPins[0]) + " (ECHO)</p>";
  h += "<div class='grid'>";
  h += "<div class='stat " + String(d.ultrasonicOk ? "ok" : "crit") + "'><div class='stat-label'>Valid Reads</div><div class='stat-value'>" + String(d.validCount) + "<span class='stat-unit'>/ 10</span></div></div>";
  h += "<div class='stat info'><div class='stat-label'>Median</div><div class='stat-value'>" + (d.medianCm > 0 ? String(d.medianCm, 1) : String("N/A")) + "<span class='stat-unit'>cm</span></div></div>";
  if (d.validCount > 0) { h += "<div class='stat'><div class='stat-label'>Min</div><div class='stat-value'>" + String(d.minCm, 1) + "<span class='stat-unit'>cm</span></div></div>"; h += "<div class='stat'><div class='stat-label'>Max</div><div class='stat-value'>" + String(d.maxCm, 1) + "<span class='stat-unit'>cm</span></div></div>"; }
  h += "</div>";
  h += "<table><tr><th>#</th><th>Distance</th><th>Status</th></tr>";
  for (int i = 0; i < 10; i++) { h += "<tr><td>" + String(i+1) + "</td><td>" + (d.readings[i] > 0 ? String(d.readings[i], 1) + " cm" : String("&mdash;")) + "</td><td>" + (d.readings[i] > 0 ? "<span class='badge badge-success'>OK</span>" : "<span class='badge badge-danger'>FAIL</span>") + "</td></tr>"; }
  h += "</table>";
  if (!d.ultrasonicOk) { h += "<div class='msg msg-err mt'>Check TRIG/ECHO wiring. Verify 3.3V on RCWL-1604 and 5V on HC-SR04. Ensure voltage divider on HC-SR04 ECHO line.</div>"; }
  h += "</div>";

  String passClass = d.ultrasonicOk ? "msg-ok" : "msg-err";
  h += "<div class='msg " + passClass + "' style='justify-content:center;font-size:18px;font-weight:700;padding:16px'>Ultrasonic: " + String(d.ultrasonicOk ? "PASS" : "FAIL") + "</div>";
  h += "<a href='/diagnostics' class='btn btn-primary'>Back to Diagnostics</a></div>" + htmlEnd();
  webServer.send(200, "text/html; charset=UTF-8", h);
}

// Captive portal redirect
void handleNotFound() {
  webServer.sendHeader("Location", "http://192.168.4.1/");
  webServer.send(302, "text/plain", "Redirecting to portal...");
}

// ═══════════════════════════════════════════════════════════════════════════════
//  SETUP & LOOP
// ═══════════════════════════════════════════════════════════════════════════════

void setup() {
  Serial.begin(115200);
  delay(100);

  pinMode(LED_PIN, OUTPUT);
  loadConfig();
  loadSensorJson();  // SPIFFS /sensors.json overrides NVS if present
  setupSensorPins();
  pinMode(CALIBRATE_BTN_PIN, INPUT_PULLUP);

  rebuildMqttTopic();

  apSsid = "EcoRoute-" + cfgDeviceCode;

  Serial.println();
  Serial.println("════════════════════════════════════════");
  Serial.println("  EcoRoute Smart Bin v" FIRMWARE_VERSION);
  Serial.println("  Device:   " + cfgDeviceCode);
  Serial.println("  Board:    ESP32-WROOM-32U (external antenna)");
  Serial.println("  Sensors:  " + String(cfgNumSensors) + " slot(s) active");
  Serial.printf( "  AP Mode:  %s\n", cfgApMode == AP_MODE_ALWAYS ? "ALWAYS" : cfgApMode == AP_MODE_HIDDEN ? "HIDDEN" : "ON-DEMAND");
  Serial.println("  Portal:   " + apSsid + " @ 192.168.4.1");
  if (simEnabled) {
    Serial.println("  *** SIMULATOR MODE ACTIVE — synthetic sensor data ***");
  }
  Serial.println("════════════════════════════════════════");
  Serial.println();

  // ── Ultrasonic sensor diagnostic at boot ────────────────────────────────
  // Skipped in simulator mode — no physical sensors to test
  Serial.printf("[boot] Bin height config: %.1f cm, Sensor offset: %.1f cm\n", binHeightCm, sensorOffsetCm);
  if (simEnabled) {
    Serial.println("[boot] SIMULATOR MODE — skipping hardware sensor diagnostic");
    for (int i = 0; i < cfgNumSensors; i++)
      Serial.printf("[boot]   Slot %d (%s): sim mode=%d period=%.0fms range=%.0f-%.0f%%\n",
        i, cfgSensorTags[i], simCfg[i].mode, simCfg[i].periodMs, simCfg[i].minFill, simCfg[i].maxFill);
  } else {
  int totalOk = 0;
  for (int s = 0; s < cfgNumSensors; s++) {
    if (cfgSensorTypes[s] == SENSOR_TYPE_TOF) {
      Serial.printf("\n[boot] --- Sensor %d (%s) VL53L0X I2C SDA=GPIO%d SCL=GPIO%d ---\n",
        s + 1, cfgSensorTags[s], TOF_SDA_PIN, TOF_SCL_PIN);
      if (tofAvailable) {
        float d = readTofDistanceCm();
        Serial.printf("[boot]   Reading: %s\n", d > 0 ? (String(d, 1) + " cm").c_str() : "no reading");
        Serial.printf("[boot] Sensor %d: OK (VL53L0X)\n", s + 1);
        totalOk++;
      } else {
        Serial.printf("[boot] Sensor %d: FAILED — VL53L0X not detected\n", s + 1);
        Serial.printf("[boot]   -> Check: VCC=3V3, GND, SDA=GPIO%d, SCL=GPIO%d\n", TOF_SDA_PIN, TOF_SCL_PIN);
      }
      continue;
    }
    Serial.printf("\n[boot] --- Sensor %d (%s) TRIG=GPIO%d ECHO=GPIO%d ---\n",
      s + 1, cfgSensorTags[s], cfgTrigPins[s], cfgEchoPins[s]);
    int validCount = 0;
    float firstValid = -1.0f;
    for (int i = 0; i < 5; i++) {
      float d = readDistanceSingleOn(cfgTrigPins[s], cfgEchoPins[s], cfgSensorTags[s]);
      Serial.printf("[boot]   Reading %d: %s\n", i + 1,
        d > 0 ? (String(d, 1) + " cm").c_str() : "FAILED (no echo)");
      if (d > 0) { validCount++; if (firstValid < 0) firstValid = d; }
      delay(80);
    }
    if (validCount >= 3) {
      Serial.printf("[boot] Sensor %d: OK (%d/5 readings valid)\n", s + 1, validCount);
      totalOk++;
    } else if (validCount > 0) {
      Serial.printf("[boot] Sensor %d: FLAKY (%d/5 readings valid) — check wiring / alignment\n", s + 1, validCount);
    } else {
      Serial.printf("[boot] Sensor %d: FAILED (0/5 readings) — sensor disconnected, wired wrong, or blocked\n", s + 1);
      Serial.printf("[boot]   -> Check: VCC=3.3V, GND, TRIG=GPIO%d, ECHO=GPIO%d\n", cfgTrigPins[s], cfgEchoPins[s]);
    }
    delay(50);
  }
  Serial.printf("\n[boot] Sensor summary: %d/%d sensors working\n", totalOk, cfgNumSensors);
  if (totalOk == 0) {
    Serial.println("[boot] CRITICAL: No ultrasonic sensors working — fill readings disabled");
  } else if (totalOk < cfgNumSensors) {
    Serial.printf("[boot] Degraded: will average across %d working sensor(s)\n", totalOk);
  } else {
    Serial.printf("[boot] All %d sensor(s) operational\n", cfgNumSensors);
  }
  } // end !simEnabled diagnostic block

  // Boot calibration via button hold
  delay(500);
  if (digitalRead(CALIBRATE_BTN_PIN) == LOW) {
    Serial.println("\n[boot] BOOT BUTTON HELD — running auto-calibration");
    runCalibration();
  }

  Serial.println("\n[boot] === DIAGNOSTIC COMPLETE ===\n");

  // Initial sensor read
  updateSensors();

  // Start AP based on mode
  bool isConfigured = cfgWifiSsid.length() > 0;
  if (cfgApMode == AP_MODE_ON_DEMAND && isConfigured) {
    Serial.println("[ap] On-demand mode — AP disabled (use BLE to enable)");
    WiFi.mode(WIFI_STA);
  } else {
    startAccessPoint();
  }

  // Connect to configured WiFi
  if (isConfigured) {
    connectWifiSta();
  }

  // MQTT
  if (wifiConnectedFlag) {
    connectMqttNonBlocking();
    if (mqttConnectedFlag) publishTelemetry();
  }

  // Web server routes
  webServer.on("/style.css",          handleCSS);
  webServer.on("/",                   handleDashboard);
  webServer.on("/config",             handleConfig);
  webServer.on("/mqtt",               handleMqtt);
  webServer.on("/calibrate",          handleCalibrate);
  webServer.on("/portal",             handlePortalSecurity);
  webServer.on("/diagnostics",        handleDiagnostics);
  webServer.on("/api/run-diag",       HTTP_POST, handleApiRunDiag);
  webServer.on("/api/force-publish",  HTTP_POST, handleApiForcePublish);
  webServer.on("/api/read-sensors",   HTTP_POST, handleApiReadSensors);
  webServer.on("/api/save-config",    HTTP_POST, handleApiSaveConfig);
  webServer.on("/api/save-mqtt",      HTTP_POST, handleApiSaveMqtt);
  webServer.on("/api/test-mqtt",      HTTP_POST, handleApiTestMqtt);
  webServer.on("/api/calibrate",      HTTP_POST, handleApiCalibrate);
  webServer.on("/api/cal-step",       HTTP_POST, handleApiCalStep);
  webServer.on("/api/reboot",         HTTP_POST, handleApiReboot);
  webServer.on("/api/factory-reset",  HTTP_POST, handleApiFactoryReset);
  webServer.on("/api/status",         HTTP_GET,  handleApiJson);
  // Simulator endpoints
  webServer.on("/simulator",          HTTP_GET,  handleSimulator);
  webServer.on("/api/sim-toggle",     HTTP_POST, handleApiSimToggle);
  webServer.on("/api/save-sim",       HTTP_POST, handleApiSaveSim);
  webServer.on("/api/sim-preset",     HTTP_POST, handleApiSimPreset);
  // OTA endpoints (HTTP Basic Auth gated)
  webServer.on("/ota",    HTTP_GET,  handleOtaPage);
  webServer.on("/update", HTTP_POST, handleOtaComplete, handleOtaUpload);
  // Backup / Restore / Rollback endpoints
  webServer.on("/backup",             HTTP_GET,  handleBackup);
  webServer.on("/api/export-config",  HTTP_GET,  handleApiExportConfig);
  webServer.on("/api/import-config",  HTTP_POST, handleApiImportConfig, handleImportUpload);
  webServer.on("/api/rollback",       HTTP_POST, handleApiRollback);

  const char* headerKeys[] = {"Referer"};
  webServer.collectHeaders(headerKeys, 1);
  webServer.onNotFound(handleNotFound);
  webServer.begin();
  Serial.println("[web] Server started on port 80");

  // Start BLE provisioning
  startBLE();

  Serial.println("[boot] Setup complete — entering main loop\n");
}

// ─── Serial command handler ────────────────────────────────────────────────
// Accepts one-line commands over USB for runtime config changes without
// needing BLE or the web portal. Used by the host-side USB web bridge.
//   SET IRINV 0|1    → flip IR sensor inversion and save to NVS
//   STATUS           → print current config
//   REBOOT           → software reboot
static String serialCmdBuf;

static void processSerialCommand(const String& cmd) {
  Serial.printf("[cmd] recv: %s\n", cmd.c_str());

  // ── SET commands (key value) ───────────────────────────────────────────
  if (cmd.startsWith("SET ")) {
    String rest = cmd.substring(4);
    int sp = rest.indexOf(' ');
    if (sp < 0) { Serial.println("[cmd] err: SET needs KEY VALUE"); return; }
    String key = rest.substring(0, sp);
    String val = rest.substring(sp + 1);
    key.toUpperCase();

    if (key == "BINHEIGHT") {
      binHeightCm = val.toFloat();
      if (binHeightCm < 1.0f) binHeightCm = 1.0f;
    } else if (key == "FULLDIST") {
      cfgFullDistanceCm = val.toFloat();
    } else if (key == "SENSOROFFSET") {
      sensorOffsetCm = val.toFloat();
    } else if (key == "ASSUMEFULL") {
      cfgAssumeFullOnFail = (val.toInt() != 0);
    } else if (key == "PERSENSORFULL") {
      cfgPerSensorAssumeFull = (val.toInt() != 0);
    } else if (key == "WIFI") {
      int sep = val.indexOf(' ');
      if (sep > 0) {
        cfgWifiSsid = val.substring(0, sep);
        cfgWifiPass = val.substring(sep + 1);
      } else {
        cfgWifiSsid = val;
        cfgWifiPass = "";
      }
      Serial.printf("[cmd] wifi ssid='%s'\n", cfgWifiSsid.c_str());
    } else if (key == "MQTT") {
      int sep = val.indexOf(' ');
      if (sep > 0) {
        cfgMqttBroker = val.substring(0, sep);
        cfgMqttPort = val.substring(sep + 1).toInt();
      } else {
        cfgMqttBroker = val;
      }
      Serial.printf("[cmd] mqtt broker='%s' port=%d\n",
                    cfgMqttBroker.c_str(), cfgMqttPort);
    } else if (key == "DEVICE") {
      cfgDeviceCode = val;
    } else if (key == "APMODE") {
      int v = val.toInt();
      if (v >= 0 && v <= 2) cfgApMode = (uint8_t)v;
    } else if (key == "SENSORINT") {
      int v = val.toInt();
      if (v >= 100 && v <= 60000) cfgSensorIntervalMs = (uint32_t)v;
    } else if (key == "VERBOSE") {
      cfgVerboseLogging = (val.toInt() != 0);
    } else {
      Serial.printf("[cmd] err: unknown key '%s'\n", key.c_str());
      return;
    }
    saveConfig();
    Serial.printf("[cmd] ok: %s saved to NVS\n", key.c_str());

  // ── CAL — trigger auto-calibration ─────────────────────────────────────
  } else if (cmd == "CAL") {
    Serial.println("[cmd] running calibration...");
    String result = runCalibration();
    saveConfig();
    Serial.printf("[cmd] cal done: binHeight=%.1f cm\n", binHeightCm);

  // ── STATUS — dump current config ───────────────────────────────────────
  } else if (cmd == "STATUS") {
    Serial.println("[cmd] === CONFIG ===");
    Serial.printf("[cmd] device=%s\n", cfgDeviceCode.c_str());
    Serial.printf("[cmd] binHeight=%.1f sensorOffset=%.1f fullDist=%.1f\n",
                  binHeightCm, sensorOffsetCm, cfgFullDistanceCm);
    Serial.printf("[cmd] assumeFull=%d verbose=%d\n",
                  cfgAssumeFullOnFail ? 1 : 0, cfgVerboseLogging ? 1 : 0);
    Serial.printf("[cmd] wifi='%s' mqtt=%s:%d\n",
                  cfgWifiSsid.c_str(), cfgMqttBroker.c_str(), cfgMqttPort);
    Serial.printf("[cmd] apMode=%d sensorInt=%lu ms verbose=%d\n",
                  cfgApMode, (unsigned long)cfgSensorIntervalMs, cfgVerboseLogging ? 1 : 0);
    Serial.printf("[cmd] effective interval now: %lu ms\n", getSensorReadIntervalMs());
    Serial.printf("[cmd] fill=%.1f%% dist=%.1f bin=%s\n",
                  currentFillPercent, currentDistanceCm,
                  binPresent ? "DOCKED" : "ABSENT");

  // ── WIFI — reconnect ──────────────────────────────────────────────────
  } else if (cmd == "RECONNECT") {
    Serial.println("[cmd] reconnecting WiFi + MQTT...");
    WiFi.disconnect();
    if (cfgWifiSsid.length() > 0)
      WiFi.begin(cfgWifiSsid.c_str(), cfgWifiPass.c_str());

  // ── FINDME — trigger Find Me LED blink ────────────────────────────────
  } else if (cmd == "FINDME") {
    triggerFindMe();
    Serial.println("[cmd] Find Me triggered (LED blink x10)");

  // ── GET SENSORS — dump runtime sensor config ───────────────────────────
  } else if (cmd == "GET SENSORS") {
    Serial.printf("[cmd] numSensors=%d\n", cfgNumSensors);
    for (int i = 0; i < cfgNumSensors; i++)
      Serial.printf("[cmd]   slot%d: trig=GPIO%d echo=GPIO%d tag='%s'\n",
                    i, cfgTrigPins[i], cfgEchoPins[i], cfgSensorTags[i]);

  // ── SET SENSOR ... — configure sensor slots ────────────────────────────
  } else if (cmd.startsWith("SET SENSOR ")) {
    String rest = cmd.substring(11);  // after "SET SENSOR "
    if (rest.startsWith("COUNT ")) {
      int n = rest.substring(6).toInt();
      if (n >= 1 && n <= MAX_SENSORS) {
        cfgNumSensors = (uint8_t)n;
        setupSensorPins();
        saveConfig();
        Serial.printf("[cmd] ok: numSensors=%d\n", cfgNumSensors);
      } else {
        Serial.printf("[cmd] err: count must be 1-%d\n", MAX_SENSORS);
      }
    } else {
      // "SET SENSOR <slot> TRIG/ECHO/TAG <val>"
      int sp = rest.indexOf(' ');
      if (sp < 0) { Serial.println("[cmd] err: SET SENSOR <slot> TRIG/ECHO/TAG <val>"); return; }
      int slot = rest.substring(0, sp).toInt();
      String kv  = rest.substring(sp + 1);
      int sp2 = kv.indexOf(' ');
      if (sp2 < 0) { Serial.println("[cmd] err: missing value"); return; }
      String key = kv.substring(0, sp2); key.toUpperCase();
      String val = kv.substring(sp2 + 1);
      if (slot < 0 || slot >= MAX_SENSORS) { Serial.printf("[cmd] err: slot must be 0-%d\n", MAX_SENSORS - 1); return; }
      if (key == "TRIG") {
        uint8_t pin = (uint8_t)val.toInt();
        if (pin == 34 || pin == 35 || pin == 36 || pin == 39) {
          Serial.printf("[cmd] err: GPIO%d is input-only, cannot use as TRIG\n", pin);
        } else {
          cfgTrigPins[slot] = pin;
          setupSensorPins();
          saveConfig();
          Serial.printf("[cmd] ok: slot%d trig=GPIO%d\n", slot, pin);
        }
      } else if (key == "ECHO") {
        cfgEchoPins[slot] = (uint8_t)val.toInt();
        setupSensorPins();
        saveConfig();
        Serial.printf("[cmd] ok: slot%d echo=GPIO%d\n", slot, cfgEchoPins[slot]);
      } else if (key == "TAG") {
        strlcpy(cfgSensorTags[slot], val.c_str(), sizeof(cfgSensorTags[0]));
        saveConfig();
        Serial.printf("[cmd] ok: slot%d tag='%s'\n", slot, cfgSensorTags[slot]);
      } else {
        Serial.printf("[cmd] err: unknown key '%s' (use TRIG/ECHO/TAG)\n", key.c_str());
      }
    }

  // ── REBOOT ─────────────────────────────────────────────────────────────
  } else if (cmd == "REBOOT") {
    Serial.println("[cmd] rebooting in 500ms");
    delay(500);
    ESP.restart();

  // ── SIM commands ─────────────────────────────────────────────────────────
  } else if (cmd == "SIM ON") {
    simEnabled = true;
    simEpoch = millis();
    saveConfig();
    Serial.println("[cmd] simulator ON");

  } else if (cmd == "SIM OFF") {
    simEnabled = false;
    saveConfig();
    Serial.println("[cmd] simulator OFF — resuming real sensors");

  } else if (cmd == "SIM STATUS") {
    Serial.printf("[cmd] sim=%s  epoch=%lums ago\n", simEnabled ? "ON" : "OFF", millis() - simEpoch);
    for (int i = 0; i < cfgNumSensors; i++) {
      Serial.printf("[cmd]   slot%d: mode=%d fill=%.1f%% period=%.0fms noise=%.1fcm range=%.0f-%.0f%%\n",
        i, simCfg[i].mode, simCurrentFill[i], simCfg[i].periodMs,
        simCfg[i].noiseCm, simCfg[i].minFill, simCfg[i].maxFill);
    }

  } else if (cmd.startsWith("SIM FILL ")) {
    // SIM FILL <pct>           — set slot 0
    // SIM FILL <slot> <pct>    — set specific slot
    String rest = cmd.substring(9);
    int sp = rest.indexOf(' ');
    int slot = 0;
    float pct = 0;
    if (sp > 0) {
      slot = rest.substring(0, sp).toInt();
      pct  = rest.substring(sp + 1).toFloat();
    } else {
      pct = rest.toFloat();
    }
    if (slot < 0 || slot >= MAX_SENSORS) {
      Serial.printf("[cmd] err: slot must be 0-%d\n", MAX_SENSORS - 1);
    } else {
      simCurrentFill[slot]    = constrain(pct, 0.0f, 100.0f);
      simCfg[slot].staticFill = simCurrentFill[slot];
      simCfg[slot].mode       = SIM_MODE_CUSTOM;
      if (!simEnabled) { simEnabled = true; simEpoch = millis(); }
      saveConfig();
      Serial.printf("[cmd] ok: sim slot%d fill=%.1f%% (mode=CUSTOM)\n", slot, simCurrentFill[slot]);
    }

  } else if (cmd.startsWith("SIM MODE ")) {
    // SIM MODE <slot> <0-4>
    String rest = cmd.substring(9);
    int sp = rest.indexOf(' ');
    if (sp < 0) {
      Serial.println("[cmd] err: SIM MODE <slot> <mode 0-4>");
    } else {
      int slot = rest.substring(0, sp).toInt();
      int mode = rest.substring(sp + 1).toInt();
      if (slot < 0 || slot >= MAX_SENSORS) {
        Serial.printf("[cmd] err: slot must be 0-%d\n", MAX_SENSORS - 1);
      } else if (mode < 0 || mode > 4) {
        Serial.println("[cmd] err: mode 0=static 1=sawtooth 2=sine 3=random 4=custom");
      } else {
        simCfg[slot].mode = (uint8_t)mode;
        saveConfig();
        Serial.printf("[cmd] ok: sim slot%d mode=%d\n", slot, mode);
      }
    }

  } else {
    Serial.printf("[cmd] unknown: %s\n", cmd.c_str());
    Serial.println("[cmd] commands: SET <key> <val>, GET SENSORS, SET SENSOR ..., SIM ON/OFF/STATUS, SIM FILL [slot] <pct>, SIM MODE <slot> <0-4>, FINDME, STATUS, CAL, RECONNECT, REBOOT");
  }
}

static void handleSerialCommands() {
  while (Serial.available()) {
    char c = (char)Serial.read();
    if (c == '\r') continue;
    if (c == '\n') {
      String cmd = serialCmdBuf;
      serialCmdBuf = "";
      cmd.trim();
      if (cmd.length() > 0) processSerialCommand(cmd);
    } else if (serialCmdBuf.length() < 64) {
      serialCmdBuf += c;
    }
  }
}

void loop() {
  unsigned long now = millis();

  // Process DNS + web requests
  if (apModeActive) dnsServer.processNextRequest();
  webServer.handleClient();

  // MQTT maintenance
  if (mqtt.connected()) mqtt.loop();

  // MQTT-triggered OTA (processed in loop, not in callback, to avoid stack issues)
  if (otaPending) {
    otaPending = false;
    performMqttOta(otaPendingUrl);
    otaPendingUrl = "";
  }

  // BLE commands
  handleBleCommands();
  tickFindMe();

  // USB serial commands (from host-side web bridge)
  handleSerialCommands();

  // On-demand AP timeout
  if (apOnDemandActive && (now - apOnDemandStart >= AP_ON_DEMAND_TIMEOUT_MS)) {
    Serial.println("[ap] On-demand timeout — disabling AP");
    stopAccessPoint();
  }

  // ── Reconnection (non-blocking, try every 30s) ──────────────────────────
  static unsigned long lastReconnect = 0;
  if (now - lastReconnect >= 30000UL) {
    if (cfgWifiSsid.length() > 0 && WiFi.status() != WL_CONNECTED) {
      wifiConnectedFlag = false;
      WiFi.begin(cfgWifiSsid.c_str(), cfgWifiPass.c_str());
    }
    if (WiFi.status() == WL_CONNECTED) {
      wifiConnectedFlag = true;
      if (!mqtt.connected()) connectMqttNonBlocking();
    }
    lastReconnect = now;
  }

  // ── Sensor read at configured interval ────────────────────────────────
  if (now - lastSensorReadTime >= getSensorReadIntervalMs()) {
    updateSensors();
  }

  // ── Publish every PUBLISH_INTERVAL_MS (1 second for demo) ──────────────
  if (now - lastPublishTime >= PUBLISH_INTERVAL_MS) {
    if (mqttConnectedFlag) {
      digitalWrite(LED_PIN, HIGH);
      publishTelemetry();
      digitalWrite(LED_PIN, LOW);
    }
  }

  // First publish after boot
  if (lastPublishTime == 0 && mqttConnectedFlag && now > 5000) {
    publishTelemetry();
  }

  delay(10);
}
