#include "ble_provisioning.h"
#include "config.h"
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#include <BLE2902.h>
#include <ArduinoJson.h>

// ─── Module state ───────────────────────────────────────────────────────────

static ConfigManager* g_config = nullptr;
static BLEServer* g_server = nullptr;
static BLECharacteristic* g_statusChar = nullptr;
static bool g_clientConnected = false;
static volatile bool g_cmdSave = false;
static volatile bool g_cmdForceReport = false;
static volatile bool g_cmdFactoryReset = false;
static volatile bool g_cmdEnableAp = false;
static volatile bool g_cmdDisableAp = false;
static volatile int8_t g_pendingApMode = -1;  // -1 = no change pending

// ─── Server Callbacks ───────────────────────────────────────────────────────

class ServerCallbacks : public BLEServerCallbacks {
  void onConnect(BLEServer* server) override {
    g_clientConnected = true;
    Serial.println("[ble] Client connected");
  }
  void onDisconnect(BLEServer* server) override {
    g_clientConnected = false;
    Serial.println("[ble] Client disconnected");
    // Restart advertising so another client can connect
    server->startAdvertising();
  }
};

// ─── Characteristic Callbacks ───────────────────────────────────────────────

class WifiSsidCallback : public BLECharacteristicCallbacks {
  void onWrite(BLECharacteristic* c) override {
    String val = c->getValue().c_str();
    Serial.printf("[ble] WiFi SSID set: %s\n", val.c_str());
    g_config->setWifiSsid(val);
  }
};

class WifiPassCallback : public BLECharacteristicCallbacks {
  void onWrite(BLECharacteristic* c) override {
    String val = c->getValue().c_str();
    Serial.println("[ble] WiFi password set (hidden)");
    g_config->setWifiPassword(val);
  }
};

class DeviceCodeCallback : public BLECharacteristicCallbacks {
  void onWrite(BLECharacteristic* c) override {
    String val = c->getValue().c_str();
    Serial.printf("[ble] Device code set: %s\n", val.c_str());
    g_config->setDeviceCode(val);
  }
  void onRead(BLECharacteristic* c) override {
    String code = g_config->getDeviceCode();
    c->setValue(code.c_str());
  }
};

class ApiUrlCallback : public BLECharacteristicCallbacks {
  void onWrite(BLECharacteristic* c) override {
    String val = c->getValue().c_str();
    Serial.printf("[ble] API URL set: %s\n", val.c_str());
    g_config->setApiUrl(val);
  }
};

class IntervalCallback : public BLECharacteristicCallbacks {
  void onWrite(BLECharacteristic* c) override {
    if (c->getLength() >= 4) {
      uint32_t interval;
      memcpy(&interval, c->getData(), sizeof(uint32_t));
      Serial.printf("[ble] Report interval set: %u seconds\n", interval);
      g_config->setReportInterval(interval);
    }
  }
  void onRead(BLECharacteristic* c) override {
    uint32_t interval = g_config->getReportInterval();
    c->setValue((uint8_t*)&interval, sizeof(uint32_t));
  }
};

class BinHeightCallback : public BLECharacteristicCallbacks {
  void onWrite(BLECharacteristic* c) override {
    if (c->getLength() >= 4) {
      float height;
      memcpy(&height, c->getData(), sizeof(float));
      Serial.printf("[ble] Bin height set: %.1f cm\n", height);
      g_config->setBinHeight(height);
    }
  }
  void onRead(BLECharacteristic* c) override {
    float height = g_config->getBinHeight();
    c->setValue((uint8_t*)&height, sizeof(float));
  }
};

class CommandCallback : public BLECharacteristicCallbacks {
  void onWrite(BLECharacteristic* c) override {
    if (c->getLength() >= 1) {
      uint8_t cmd = c->getData()[0];
      Serial.printf("[ble] Command received: 0x%02X\n", cmd);
      switch (cmd) {
        case CMD_SAVE_AND_RESTART:
          g_cmdSave = true;
          break;
        case CMD_FORCE_REPORT:
          g_cmdForceReport = true;
          break;
        case CMD_FACTORY_RESET:
          g_cmdFactoryReset = true;
          break;
        case CMD_ENABLE_AP:
          g_cmdEnableAp = true;
          break;
        case CMD_DISABLE_AP:
          g_cmdDisableAp = true;
          break;
        case CMD_SET_AP_HIDDEN:
          g_pendingApMode = AP_MODE_HIDDEN;
          break;
        case CMD_SET_AP_ON_DEMAND:
          g_pendingApMode = AP_MODE_ON_DEMAND;
          break;
        case CMD_SET_AP_ALWAYS:
          g_pendingApMode = AP_MODE_ALWAYS;
          break;
        default:
          Serial.printf("[ble] Unknown command: 0x%02X\n", cmd);
          break;
      }
    }
  }
};

// ─── Public API ─────────────────────────────────────────────────────────────

void BleProvisioning::begin(ConfigManager* configManager, const char* deviceName) {
  g_config = configManager;
  g_cmdSave = false;
  g_cmdForceReport = false;
  g_cmdFactoryReset = false;

  BLEDevice::init(deviceName);

  // Request larger MTU for long strings (API URL, WiFi password)
  BLEDevice::setMTU(512);

  g_server = BLEDevice::createServer();
  g_server->setCallbacks(new ServerCallbacks());

  BLEService* service = g_server->createService(BLEUUID(SERVICE_UUID), 30);  // 30 handles

  // WiFi SSID (Write)
  BLECharacteristic* ssidChar = service->createCharacteristic(
    BLEUUID(CHAR_WIFI_SSID_UUID),
    BLECharacteristic::PROPERTY_WRITE
  );
  ssidChar->setCallbacks(new WifiSsidCallback());

  // WiFi Password (Write)
  BLECharacteristic* passChar = service->createCharacteristic(
    BLEUUID(CHAR_WIFI_PASS_UUID),
    BLECharacteristic::PROPERTY_WRITE
  );
  passChar->setCallbacks(new WifiPassCallback());

  // Device Code (Read/Write)
  BLECharacteristic* codeChar = service->createCharacteristic(
    BLEUUID(CHAR_DEVICE_CODE_UUID),
    BLECharacteristic::PROPERTY_READ | BLECharacteristic::PROPERTY_WRITE
  );
  codeChar->setCallbacks(new DeviceCodeCallback());
  String currentCode = g_config->getDeviceCode();
  if (currentCode.length() > 0) {
    codeChar->setValue(currentCode.c_str());
  }

  // API URL (Write)
  BLECharacteristic* urlChar = service->createCharacteristic(
    BLEUUID(CHAR_API_URL_UUID),
    BLECharacteristic::PROPERTY_WRITE
  );
  urlChar->setCallbacks(new ApiUrlCallback());

  // Report Interval (Read/Write) - uint32 little-endian
  BLECharacteristic* intervalChar = service->createCharacteristic(
    BLEUUID(CHAR_INTERVAL_UUID),
    BLECharacteristic::PROPERTY_READ | BLECharacteristic::PROPERTY_WRITE
  );
  intervalChar->setCallbacks(new IntervalCallback());
  uint32_t currentInterval = g_config->getReportInterval();
  intervalChar->setValue((uint8_t*)&currentInterval, sizeof(uint32_t));

  // Bin Height (Read/Write) - float32 little-endian
  BLECharacteristic* heightChar = service->createCharacteristic(
    BLEUUID(CHAR_BIN_HEIGHT_UUID),
    BLECharacteristic::PROPERTY_READ | BLECharacteristic::PROPERTY_WRITE
  );
  heightChar->setCallbacks(new BinHeightCallback());
  float currentHeight = g_config->getBinHeight();
  heightChar->setValue((uint8_t*)&currentHeight, sizeof(float));

  // Device Status (Read/Notify)
  g_statusChar = service->createCharacteristic(
    BLEUUID(CHAR_STATUS_UUID),
    BLECharacteristic::PROPERTY_READ | BLECharacteristic::PROPERTY_NOTIFY
  );
  g_statusChar->addDescriptor(new BLE2902());

  // Command (Write)
  BLECharacteristic* cmdChar = service->createCharacteristic(
    BLEUUID(CHAR_COMMAND_UUID),
    BLECharacteristic::PROPERTY_WRITE
  );
  cmdChar->setCallbacks(new CommandCallback());

  service->start();

  // Start advertising
  BLEAdvertising* advertising = BLEDevice::getAdvertising();
  advertising->addServiceUUID(BLEUUID(SERVICE_UUID));
  advertising->setScanResponse(true);
  advertising->setMinPreferred(0x06);  // Helps with iPhone connection
  BLEDevice::startAdvertising();

  Serial.printf("[ble] GATT server started, advertising as '%s'\n", deviceName);
}

void BleProvisioning::stop() {
  if (g_server) {
    BLEDevice::deinit(false);
    g_server = nullptr;
    g_statusChar = nullptr;
    Serial.println("[ble] Stopped");
  }
}

bool BleProvisioning::isClientConnected() {
  return g_clientConnected;
}

bool BleProvisioning::hasSaveCommand() {
  return g_cmdSave;
}

bool BleProvisioning::hasForceReportCommand() {
  return g_cmdForceReport;
}

bool BleProvisioning::hasFactoryResetCommand() {
  return g_cmdFactoryReset;
}

bool BleProvisioning::hasEnableApCommand() {
  return g_cmdEnableAp;
}

bool BleProvisioning::hasDisableApCommand() {
  return g_cmdDisableAp;
}

int8_t BleProvisioning::getPendingApMode() {
  return g_pendingApMode;
}

void BleProvisioning::clearCommands() {
  g_cmdSave = false;
  g_cmdForceReport = false;
  g_cmdFactoryReset = false;
  g_cmdEnableAp = false;
  g_cmdDisableAp = false;
  g_pendingApMode = -1;
}

void BleProvisioning::updateStatus(float batteryVoltage, bool wifiConnected) {
  if (!g_statusChar) return;

  JsonDocument doc;
  doc["wifi"] = wifiConnected;
  doc["configured"] = g_config->isConfigured();
  doc["battery"] = round(batteryVoltage * 100.0f) / 100.0f;
  doc["fw"] = FIRMWARE_VERSION;
  doc["deviceCode"] = g_config->getDeviceCode();
  doc["interval"] = g_config->getReportInterval();
  doc["apMode"] = g_config->getApMode();

  String json;
  serializeJson(doc, json);
  g_statusChar->setValue(json.c_str());
  g_statusChar->notify();
}
