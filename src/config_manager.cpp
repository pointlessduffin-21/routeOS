#include "config_manager.h"

const char* ConfigManager::NVS_NAMESPACE = "ecoroute";

void ConfigManager::begin() {
  _prefs.begin(NVS_NAMESPACE, false);
  Serial.println("[config] NVS initialized");
}

bool ConfigManager::isConfigured() {
  return _prefs.getBool("configured", false);
}

void ConfigManager::markConfigured() {
  _prefs.putBool("configured", true);
}

// ─── WiFi ───────────────────────────────────────────────────────────────────

String ConfigManager::getWifiSsid() {
  return _prefs.getString("wifiSsid", "");
}

void ConfigManager::setWifiSsid(const String& ssid) {
  _prefs.putString("wifiSsid", ssid);
}

String ConfigManager::getWifiPassword() {
  return _prefs.getString("wifiPass", "");
}

void ConfigManager::setWifiPassword(const String& password) {
  _prefs.putString("wifiPass", password);
}

// ─── Device Identity ────────────────────────────────────────────────────────

String ConfigManager::getDeviceCode() {
  return _prefs.getString("deviceCode", DEFAULT_DEVICE_CODE);
}

void ConfigManager::setDeviceCode(const String& code) {
  _prefs.putString("deviceCode", code);
}

// ─── API Settings ───────────────────────────────────────────────────────────

String ConfigManager::getApiUrl() {
  return _prefs.getString("api_url", DEFAULT_API_URL);
}

void ConfigManager::setApiUrl(const String& url) {
  _prefs.putString("api_url", url);
}

String ConfigManager::getApiKey() {
  return _prefs.getString("api_key", DEFAULT_API_KEY);
}

void ConfigManager::setApiKey(const String& key) {
  _prefs.putString("api_key", key);
}

// ─── Reporting ──────────────────────────────────────────────────────────────

uint32_t ConfigManager::getReportInterval() {
  return _prefs.getUInt("interval", DEFAULT_REPORT_INTERVAL_SEC);
}

void ConfigManager::setReportInterval(uint32_t seconds) {
  _prefs.putUInt("interval", seconds);
}

// ─── Bin Height ─────────────────────────────────────────────────────────────

float ConfigManager::getBinHeight() {
  return _prefs.getFloat("binHeight", DEFAULT_BIN_HEIGHT_CM);
}

void ConfigManager::setBinHeight(float cm) {
  _prefs.putFloat("binHeight", cm);
}

// ─── MQTT Settings ──────────────────────────────────────────────────────────

String ConfigManager::getMqttBroker() {
  return _prefs.getString("mqttBroker", MQTT_BROKER_HOST);
}

void ConfigManager::setMqttBroker(const String& broker) {
  _prefs.putString("mqttBroker", broker);
}

int ConfigManager::getMqttPort() {
  return _prefs.getInt("mqttPort", MQTT_BROKER_PORT);
}

void ConfigManager::setMqttPort(int port) {
  _prefs.putInt("mqttPort", port);
}

String ConfigManager::getMqttUser() {
  return _prefs.getString("mqttUser", "ecoroute");
}

void ConfigManager::setMqttUser(const String& user) {
  _prefs.putString("mqttUser", user);
}

String ConfigManager::getMqttPassword() {
  return _prefs.getString("mqttPass", "secret");
}

void ConfigManager::setMqttPassword(const String& pass) {
  _prefs.putString("mqttPass", pass);
}

String ConfigManager::getMqttTopic() {
  return _prefs.getString("mqttTopic", MQTT_TOPIC_PREFIX);
}

void ConfigManager::setMqttTopic(const String& topic) {
  _prefs.putString("mqttTopic", topic);
}

// ─── AP Mode ────────────────────────────────────────────────────────────────

uint8_t ConfigManager::getApMode() {
  return _prefs.getUChar("apMode", AP_MODE_ALWAYS);
}

void ConfigManager::setApMode(uint8_t mode) {
  _prefs.putUChar("apMode", mode);
}

// ─── Factory Reset ──────────────────────────────────────────────────────────

void ConfigManager::factoryReset() {
  Serial.println("[config] Factory reset - clearing all NVS data");
  _prefs.clear();
}
