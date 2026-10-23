#pragma once
#define LV_CONF_H
#define LV_COLOR_DEPTH 16
#define LV_COLOR_16_SWAP 0
// The calculator UI (40 widgets, no shadows, gradients or images, length-limited text areas) leaves about 8 KB of this
// pool free once built (lv_mem_monitor). Every KB not held here is a KB for Wi-Fi and TCP buffers, which is what ran
// out on the lab board.
#define LV_MEM_SIZE (24U * 1024U)
#define LV_DISP_DEF_REFR_PERIOD 200
#define LV_INDEV_DEF_READ_PERIOD 20
#define LV_TICK_CUSTOM 0
#define LV_USE_LOG 0
#define LV_USE_PERF_MONITOR 0
#define LV_USE_MEM_MONITOR 0
#define LV_FONT_MONTSERRAT_14 1
#define LV_FONT_MONTSERRAT_10 1
#define LV_FONT_MONTSERRAT_12 1
#define LV_FONT_MONTSERRAT_24 1
#define LV_USE_THEME_DEFAULT 1
#define LV_THEME_DEFAULT_DARK 1
#define LV_BUILD_EXAMPLES 0
#define LV_USE_DEMO_WIDGETS 0
#define LV_USE_DEMO_BENCHMARK 0
