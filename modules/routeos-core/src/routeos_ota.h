#pragma once
// Over-the-air firmware updates with a safety net.
//
//   GET  /api/ota            what is running, what is in the other slot, update and guard state
//   POST /api/ota/begin      {"size":N} start receiving an image into the idle slot
//   POST /api/ota/chunk      raw bytes at the offset in the X-Ota-Offset header; resumable
//   POST /api/ota/finish     verify the whole image and make it the next boot
//   POST /api/ota/abort      forget a half-received image
//   POST /api/ota/rollback   boot the other slot instead (also the way back after a roll-forward)
//
// The running firmware is never touched until an image has arrived complete, carries the routeOS
// marker (ROUTEOS_IMAGE) and passes the ESP-IDF integrity check. A newly installed image is on trial:
// it must reach the network and stay up for 30 s. If it crashes three times, or does not get there
// within two minutes, the previous firmware is booted again by itself.
#include <Arduino.h>
#include <WebServer.h>

namespace routeos {
namespace ota {

// Boot-time guard. Call early, once the firmware name and version are known.
void begin();
// The device has reached the network (called by routeos::onNetworkUp).
void networkUp();
// An image is being received. Other subsystems keep out of its way (no Wi-Fi changes meanwhile).
bool active();
void attach(WebServer& http);

}  // namespace ota
}  // namespace routeos
