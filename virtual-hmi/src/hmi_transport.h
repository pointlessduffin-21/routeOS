#pragma once
#include <Arduino.h>
#include <atomic>
#include "rvdp_protocol.h"

namespace hmi {
extern QueueHandle_t outgoing, inputs, keys;
extern std::atomic<uint32_t> epoch, drops, sentBytes, batches;
extern std::atomic<uint32_t> inputEpoch;
extern std::atomic<bool> inputReady;
extern std::atomic<bool> ready, refresh, overflow;
extern std::atomic<uint32_t> uiMicros, uiStack;
void startNetwork();
void printMetrics(const char* stage);
}
