#include "wifi_manager.h"
#include <WiFi.h>

bool WifiManager::connect(const char* ssid, const char* password, uint32_t timeoutMs) {
  Serial.printf("[wifi] Connecting to '%s'...\n", ssid);

  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid, password);

  uint32_t startMs = millis();
  while (WiFi.status() != WL_CONNECTED) {
    if (millis() - startMs > timeoutMs) {
      Serial.println("[wifi] Connection timed out");
      WiFi.disconnect(true);
      return false;
    }
    delay(500);
    Serial.print(".");
  }

  Serial.printf("\n[wifi] Connected! IP: %s\n", WiFi.localIP().toString().c_str());
  return true;
}

void WifiManager::disconnect() {
  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);
  Serial.println("[wifi] Disconnected");
}

bool WifiManager::isConnected() {
  return WiFi.status() == WL_CONNECTED;
}

String WifiManager::getLocalIp() {
  return WiFi.localIP().toString();
}
