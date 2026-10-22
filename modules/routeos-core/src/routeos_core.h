#pragma once
// routeOS Core: what Termz talks to when it checks a board and installs packages.
//
//   GET    /api/capabilities          public, read-only: chip, flash, PSRAM, memory, pins, modules
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

namespace routeos {

constexpr const char* CORE_VERSION = "1.0.0";

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
