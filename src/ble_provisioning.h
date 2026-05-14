#ifndef ECOROUTE_BLE_PROVISIONING_H
#define ECOROUTE_BLE_PROVISIONING_H

#include <Arduino.h>
#include "config_manager.h"

class BleProvisioning {
public:
  // Initialize BLE GATT server with provisioning service
  void begin(ConfigManager* configManager, const char* deviceName);

  // Stop BLE advertising and deinit
  void stop();

  // Check if a mobile app client is connected
  bool isClientConnected();

  // Check if a save command (0x01) was received
  bool hasSaveCommand();

  // Check if a force-report command (0x02) was received
  bool hasForceReportCommand();

  // Check if a factory-reset command (0x03) was received
  bool hasFactoryResetCommand();

  // Check if an enable-AP command (0x04) was received
  bool hasEnableApCommand();

  // Check if a disable-AP command (0x05) was received
  bool hasDisableApCommand();

  // Get pending AP mode change (0x06-0x08), returns -1 if none
  int8_t getPendingApMode();

  // Clear command flags
  void clearCommands();

  // Update the status characteristic with current device info
  void updateStatus(float batteryVoltage, bool wifiConnected);
};

#endif // ECOROUTE_BLE_PROVISIONING_H
