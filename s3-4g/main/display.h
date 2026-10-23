#pragma once
#include <stdint.h>
#include "lvgl.h"

// The headless LVGL display, the RVDP queues the UI reads input from, and the UI task.
lv_display_t* display_start();
void display_service();     // one pass of UI work: remote key input and UI tick

// LVGL is not thread safe: the UI task, the RVDP server and anything else that touches widgets must hold this.
bool display_lock(uint32_t timeoutMs);   // 0 = wait forever
void display_unlock();
