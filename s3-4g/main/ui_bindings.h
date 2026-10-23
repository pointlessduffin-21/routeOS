#pragma once
#include <stdint.h>
struct DemoState {
  char page[12],expression[65],result[32];
  uint16_t historyCount,noteLength;
};
void demoInit();
void demoTick();
void demoSnapshot(DemoState& state);
