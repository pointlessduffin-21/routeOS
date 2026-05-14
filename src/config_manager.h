#ifndef ECOROUTE_CONFIG_MANAGER_H
#define ECOROUTE_CONFIG_MANAGER_H

#include <Arduino.h>
#include <Preferences.h>
#include "config.h"

class ConfigManager {
public:
  void begin();

  // Check if device has been provisioned
  bool isConfigured();
  void markConfigured();

  // WiFi credentials
  String getWifiSsid();
  void setWifiSsid(const String& ssid);
  String getWifiPassword();
  void setWifiPassword(const String& password);

  // Device identity
  String getDeviceCode();
  void setDeviceCode(const String& code);

  // API settings
  String getApiUrl();
  void setApiUrl(const String& url);
  String getApiKey();
  void setApiKey(const String& key);

  // Reporting interval in seconds
  uint32_t getReportInterval();
  void setReportInterval(uint32_t seconds);

  // Bin physical height in cm
  float getBinHeight();
  void setBinHeight(float cm);

  // MQTT settings
  String getMqttBroker();
  void setMqttBroker(const String& broker);
  int getMqttPort();
  void setMqttPort(int port);
  String getMqttUser();
  void setMqttUser(const String& user);
  String getMqttPassword();
  void setMqttPassword(const String& pass);
  String getMqttTopic();
  void setMqttTopic(const String& topic);

  // AP broadcast mode (AP_MODE_ALWAYS, AP_MODE_HIDDEN, AP_MODE_ON_DEMAND)
  uint8_t getApMode();
  void setApMode(uint8_t mode);

  // Factory reset - clears all stored config
  void factoryReset();

private:
  Preferences _prefs;
  static const char* NVS_NAMESPACE;
};

#endif // ECOROUTE_CONFIG_MANAGER_H
