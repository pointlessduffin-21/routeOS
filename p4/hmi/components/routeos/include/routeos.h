#pragma once
// routeOS Core for ESP-IDF (ESP32-P4). The same device API as modules/routeos-core, which is written for the Arduino
// framework and cannot run on a chip that has no Wi-Fi of its own. Same routes, same JSON, same update guard.
//
//   GET    /api/capabilities          public, read-only: chip, flash, PSRAM, memory, modules, features
//   GET    /api/system                token: Wi-Fi, memory, uptime, firmware size
//   GET    /api/logs?since=N          token: recent events (the last 48, in RAM)
//   POST   /api/reboot                token: restart the device
//   GET    /api/wifi, PUT /api/wifi   token: connection; change network (the old one returns if the new one fails)
//   GET/POST /api/wifi/scan           token: look for networks
//   GET    /api/ota, POST /api/ota/*  token: firmware update with rollback
//
// Drivers (sensors on GPIO) are not offered: this chip's pin map is not defined yet, so capabilities says
// drivers.supported = false and the driver routes answer 409.
#include <stddef.h>
#include <stdint.h>
#include <string>
#include "esp_http_server.h"

#define ROUTEOS_CORE_VERSION "1.1.1"

// Put this once in the firmware's main file, with string literals: ROUTEOS_IMAGE("routeos-p4-hmi", "1.0.0");
// It stamps a marker into the image so Termz and the updater recognise routeOS firmware. Linking fails without it.
#define ROUTEOS_IMAGE(name, version) \
  extern "C" const char routeos_image_tag[] __attribute__((used)) = \
      "ROUTEOS-IMAGE|1|" name "|" version "|core=" ROUTEOS_CORE_VERSION "|ota=1|END"

namespace routeos {

constexpr const char* CORE_VERSION = ROUTEOS_CORE_VERSION;

struct Module {
  const char* id;        // e.g. "rvdp"
  const char* version;   // e.g. "1.2"
};

// level: 'I' information, 'W' warning, 'E' error. Echoed to the serial console. Never log a secret.
void log(char level, const char* fmt, ...) __attribute__((format(printf, 2, 3)));
uint32_t bootId();
uint32_t millis();

// Once, early. `modules` must outlive the program. `allowManage` false answers every management call with 403.
void begin(const char* firmwareName, const char* firmwareVersion, const Module* modules, size_t count, bool allowManage);

// Start the station connection through the radio chip. A network saved through the API is tried first, then the one
// given here. Returns false when there is neither.
bool wifiBegin(const char* builtinSsid, const char* builtinPassword);

// Start the web server on port 80 with every Core route. Returns the handle so the firmware can add its own routes.
httpd_handle_t startServer();

// Registers the browser setup page and its link test. Called by startServer(); exposed so a firmware that runs its own
// server can still have the page.
void pageAttach(httpd_handle_t server);

// For the firmware's own routes.
bool authorized(httpd_req_t* req);                                   // replies 401/403/503 itself on failure
esp_err_t reply(httpd_req_t* req, int code, const std::string& json);
esp_err_t fail(httpd_req_t* req, int code, const std::string& message);

}  // namespace routeos
