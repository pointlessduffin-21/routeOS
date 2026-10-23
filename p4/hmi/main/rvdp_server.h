#pragma once
// RVDP screen transport for the ESP32-P4 panel.
//
// The panel is 800x1280 and the adapter owns its flush path, so this module copies every area LVGL flushes into a
// PSRAM shadow frame and streams changed bands to one viewer at a time on ws://<device>:81/rvdp. The protocol,
// packet layout and input epochs are the same ones the 240x135 firmware speaks (see protocol/rvdp-v1.md).
#include <stdint.h>
#include "esp_http_server.h"
#include "lvgl.h"

namespace rvdp_transport {

// Wrap the display's flush so every rendered area is captured, then start the sender. Call once, before the adapter
// starts rendering: anything drawn before this is not in the shadow.
void attach(lv_display_t* display);

// Serve /rvdp on port 81 and start the sender task.
void begin();

// Bands still waiting to go out; reported by /health.
uint32_t pending();

// The captured frame itself (800x1280 RGB565, PSRAM) and a counter that changes whenever pixels did. The panel is
// driven from this same buffer, so what the glass shows is exactly what a viewer receives.
const void* panel_frame();
uint32_t panel_frame_version();

}  // namespace rvdp_transport
