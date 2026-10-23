#pragma once
// The pieces of the RVDP transport that the UI and the display code share. Same names as the ESP32 firmware's
// hmi_transport.h (which includes Arduino.h and cannot be used here).
#include <atomic>
#include <stdint.h>
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "rvdp_protocol.h"

#define HMI_WIDTH RVDP_WIDTH
#define HMI_HEIGHT RVDP_HEIGHT

namespace hmi {
extern QueueHandle_t inputs, keys;
extern std::atomic<uint32_t> epoch, drops, sentBytes, batches;
extern std::atomic<uint32_t> inputEpoch;
extern std::atomic<bool> inputReady;
extern std::atomic<bool> ready, refresh, overflow;
void createQueues();
}