#pragma once
// Wi-Fi management for the Core: a network saved on the device, a scan, and changes that
// undo themselves. Routes: GET/PUT /api/wifi, GET/POST /api/wifi/scan (see routeos_core.h).
#include <Arduino.h>
#include <WebServer.h>

namespace routeos {

// Start the station connection. A network saved through the API is tried first, then the one
// built into the firmware. Returns false when there is neither, so the caller can open an
// access point instead. Call from the task that owns the network, once.
bool wifiBegin(const char* builtinSsid, const char* builtinPassword);

namespace wifi {
void attach(WebServer& http);
// Was a change of network started and not yet settled?
bool changing();
}  // namespace wifi

}  // namespace routeos
