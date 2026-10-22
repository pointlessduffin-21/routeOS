#pragma once
// routeOS Core: what Termz talks to when it checks a board, manages it and installs packages.
//
//   GET    /api/capabilities          public, read-only: chip, flash, PSRAM, memory, pins, modules, features
//   GET    /api/system                token: Wi-Fi, memory, uptime, firmware size
//   GET    /api/logs?since=N          token: recent events (the last 48, in RAM)
//   POST   /api/reboot                token: restart the device
//   GET    /api/wifi                  token: connection, saved network, why it last dropped
//   GET    /api/wifi/scan, POST ...   token: look for networks (start with POST, read with GET)
//   PUT    /api/wifi                  token: change network; the old one returns if the new one fails
//   GET    /api/ota, POST /api/ota/*  token: firmware update, rollback (see routeos_ota.h)
//   GET    /api/drivers               token: installed driver specs
//   GET    /api/drivers/readings      token: latest value of every driver
//   PUT    /api/drivers/{id}          token: install or replace a driver (validated here, never trusted)
//   DELETE /api/drivers/{id}          token: remove a driver
//
// "token" is `Authorization: Bearer <token>`. The token is created on first network start,
// kept in NVS, and printed on the serial console. Management is compiled in only when the
// firmware is built with -DROUTEOS_MANAGE=1.
#include <Arduino.h>
#include <WebServer.h>

#define ROUTEOS_CORE_VERSION "1.1.0"

// Put this once in the firmware's main file, with string literals:
//
//   ROUTEOS_IMAGE("routeos-orbit-hmi", "1.1.0");
//
// It stamps a marker into the image. Termz and this library's updater look for it, so a file is
// recognised as routeOS firmware (and as one that guards its own start) before it is installed.
// Linking fails without it, on purpose.
#define ROUTEOS_IMAGE(name, version) \
  extern "C" const char routeos_image_tag[] __attribute__((used)) = \
      "ROUTEOS-IMAGE|1|" name "|" version "|core=" ROUTEOS_CORE_VERSION "|ota=1|END"

namespace routeos {

constexpr const char* CORE_VERSION = ROUTEOS_CORE_VERSION;

struct Module {
  const char* id;        // e.g. "rvdp"
  const char* version;   // e.g. "1.1"
};

// Call once from setup(). `modules` must outlive the program (use a static array).
void begin(const char* firmwareName, const char* firmwareVersion, const Module* modules, size_t count, bool allowManage);

// Call once the network is up. Creates the management token the first time.
void onNetworkUp();

// Register the routes. Call before WebServer::begin().
void attach(WebServer& http);

}  // namespace routeos

#include "routeos_log.h"
#include "routeos_wifi.h"
