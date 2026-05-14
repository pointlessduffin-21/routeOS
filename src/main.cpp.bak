#include <Arduino.h>
#include "config.h"
#include "config_manager.h"
#include "sensor_reader.h"
#include "wifi_manager.h"
#include "mqtt_client.h"
#include "ble_provisioning.h"

// ─── Global objects ─────────────────────────────────────────────────────────

ConfigManager configManager;
SensorReader sensors;
WifiManager wifi;
EcoRouteMqttClient mqttClient;
BleProvisioning ble;

// ─── Deep sleep helper ──────────────────────────────────────────────────────

void enterDeepSleep(uint32_t seconds) {
  Serial.printf("[main] Entering deep sleep for %u seconds...\n", seconds);
  Serial.flush();

  // Hold TRIG pin LOW during sleep
  gpio_hold_en(HCSR04_TRIG_PIN);
  gpio_deep_sleep_hold_en();

  esp_sleep_enable_timer_wakeup((uint64_t)seconds * 1000000ULL);
  esp_deep_sleep_start();
}

// ─── BLE Provisioning Mode ──────────────────────────────────────────────────

void runProvisioningMode() {
  Serial.println("[main] === PROVISIONING MODE ===");
  Serial.println("[main] Waiting for mobile app to configure via BLE...");

  // Use a name based on the chip's MAC address if no device code set
  String deviceName;
  String storedCode = configManager.getDeviceCode();
  if (storedCode.length() > 0) {
    deviceName = storedCode;
  } else {
    uint64_t mac = ESP.getEfuseMac();
    char macStr[5];
    snprintf(macStr, sizeof(macStr), "%04X", (uint16_t)(mac >> 32));
    deviceName = String(BLE_DEVICE_NAME_PREFIX) + macStr;
  }

  // Read battery for status updates
  sensors.begin();
  float battery = sensors.readBatteryVoltage();

  // Start BLE GATT server
  ble.begin(&configManager, deviceName.c_str());
  ble.updateStatus(battery, false);

  // Blink LED to indicate provisioning mode
  pinMode(LED_PIN, OUTPUT);

  uint32_t bleStartMs = millis();
  uint32_t lastBlinkMs = 0;
  bool ledState = false;

  while (true) {
    // Check for commands
    if (ble.hasSaveCommand()) {
      Serial.println("[main] Save command received!");
      ble.clearCommands();

      // Validate we have minimum config
      String ssid = configManager.getWifiSsid();
      String code = configManager.getDeviceCode();
      String url = configManager.getApiUrl();

      if (ssid.length() > 0 && code.length() > 0 && url.length() > 0) {
        configManager.markConfigured();
        Serial.println("[main] Configuration saved. Restarting...");
        delay(500);
        ESP.restart();
      } else {
        Serial.println("[main] Incomplete config - need WiFi SSID, device code, and API URL");
      }
    }

    if (ble.hasFactoryResetCommand()) {
      Serial.println("[main] Factory reset command received!");
      ble.clearCommands();
      configManager.factoryReset();
      Serial.println("[main] Factory reset complete. Restarting...");
      delay(500);
      ESP.restart();
    }

    if (ble.hasForceReportCommand()) {
      Serial.println("[main] Force report command - ignored in provisioning mode");
      ble.clearCommands();
    }

    // Blink LED every 500ms in provisioning mode
    if (millis() - lastBlinkMs > 500) {
      ledState = !ledState;
      digitalWrite(LED_PIN, ledState);
      lastBlinkMs = millis();
    }

    // Update status periodically (every 5 seconds)
    static uint32_t lastStatusUpdate = 0;
    if (millis() - lastStatusUpdate > 5000) {
      battery = sensors.readBatteryVoltage();
      ble.updateStatus(battery, false);
      lastStatusUpdate = millis();
    }

    // Timeout: go to deep sleep after BLE_ADVERTISING_TIMEOUT_SEC
    if (millis() - bleStartMs > (uint32_t)BLE_ADVERTISING_TIMEOUT_SEC * 1000) {
      Serial.println("[main] BLE advertising timeout. Sleeping for 60s, then retry...");
      ble.stop();
      enterDeepSleep(60);  // Wake up in 1 min and try BLE again
    }

    delay(100);
  }
}

// ─── Telemetry Reporting Mode ───────────────────────────────────────────────

void runReportingMode() {
  Serial.println("[main] === REPORTING MODE ===");

  String ssid = configManager.getWifiSsid();
  String pass = configManager.getWifiPassword();
  String deviceCode = configManager.getDeviceCode();
  float binHeight = configManager.getBinHeight();
  uint32_t interval = configManager.getReportInterval();

  Serial.printf("[main] Device: %s | Bin: %.0f cm | Interval: %us\n",
    deviceCode.c_str(), binHeight, interval);
  Serial.printf("[main] MQTT: %s:%d\n", MQTT_BROKER_HOST, MQTT_BROKER_PORT);

  // Turn on LED solid during reporting
  pinMode(LED_PIN, OUTPUT);
  digitalWrite(LED_PIN, HIGH);

  // Step 1: Connect to WiFi
  if (!wifi.connect(ssid.c_str(), pass.c_str())) {
    Serial.println("[main] WiFi failed. Sleeping and retrying...");
    enterDeepSleep(60);  // Retry in 1 minute
    return;
  }

  // Step 2: Read sensors
  sensors.begin();
  SensorReading reading = sensors.readAll(binHeight);

  // Step 3: Publish telemetry via MQTT
  // Topic: ecoroute/trash_can/<device_code>
  MqttPublishResult result = mqttClient.publishTelemetry(
    deviceCode.c_str(),
    reading
  );

  if (result.success) {
    Serial.println("[main] Telemetry published!");
  } else {
    Serial.printf("[main] Telemetry failed: %s\n", result.error.c_str());
  }

  // Step 4: Disconnect WiFi and sleep
  wifi.disconnect();
  digitalWrite(LED_PIN, LOW);

  enterDeepSleep(interval);
}

// ─── Arduino Entry Points ───────────────────────────────────────────────────

void setup() {
  Serial.begin(115200);
  delay(100);

  Serial.println();
  Serial.println("╔══════════════════════════════════════╗");
  Serial.println("║   EcoRoute Smart Bin Firmware v" FIRMWARE_VERSION "  ║");
  Serial.println("╚══════════════════════════════════════╝");
  Serial.println();

  // Check wake reason
  esp_sleep_wakeup_cause_t wakeup = esp_sleep_get_wakeup_cause();
  if (wakeup == ESP_SLEEP_WAKEUP_TIMER) {
    Serial.println("[main] Woke up from deep sleep (timer)");
  } else {
    Serial.println("[main] Fresh boot / reset");
  }

  // Load configuration from NVS
  configManager.begin();

  if (configManager.isConfigured()) {
    runReportingMode();
  } else {
    runProvisioningMode();
  }
}

void loop() {
  // loop() is only reached in provisioning mode (runProvisioningMode has its own loop)
  // If we somehow get here, just sleep
  delay(1000);
}
