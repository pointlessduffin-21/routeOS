#ifndef ECOROUTE_WIFI_MANAGER_H
#define ECOROUTE_WIFI_MANAGER_H

#include <Arduino.h>
#include "config.h"

class WifiManager {
public:
  // Connect to WiFi with stored credentials
  bool connect(const char* ssid, const char* password, uint32_t timeoutMs = WIFI_CONNECT_TIMEOUT_MS);

  // Disconnect and turn off WiFi radio
  void disconnect();

  // Check if connected
  bool isConnected();

  // Get local IP as string
  String getLocalIp();
};

#endif // ECOROUTE_WIFI_MANAGER_H
