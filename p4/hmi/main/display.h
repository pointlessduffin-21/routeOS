#pragma once
#include "lvgl.h"

// The physical 800x1280 display, the RVDP queues the UI reads input from, and the UI task.
lv_display_t* display_start();
void display_service();     // one pass of UI work: remote key input and UI tick