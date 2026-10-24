#include "display.h"
#include "esp_check.h"
#include "esp_log.h"
#include "bsp/esp-bsp.h"
#include "bsp/display.h"
#include "p4_transport.h"
#include "rvdp_server.h"
#include "ui_bindings.h"

namespace hmi {
QueueHandle_t inputs, keys;
std::atomic<uint32_t> epoch{0}, drops{0}, sentBytes{0}, batches{0}, inputEpoch{0};
std::atomic<uint32_t> viewerDrops{0}, wedges{0}, sendFails{0};
std::atomic<bool> inputReady{false}, ready{false}, refresh{false}, overflow{false};
void createQueues() {
    inputs = xQueueCreate(16, sizeof(rvdp::Input));
    keys = xQueueCreate(32, sizeof(rvdp::KeyInput));
}
}  // namespace hmi

static const char* TAG = "display";

// The board is a Guition JC8012P4A1. This project builds on the vendor BSP for it, which has the panel's real DSI
// timing (2 lanes at 1.5 Gbps), backlight on GPIO 23, reset on 27, and the GSL3680 touch controller.
//
// We cannot use bsp_display_start() directly: it asks for software rotation, which makes esp_lvgl_port allocate an
// extra full-frame 800x1280 buffer (2 MB) in internal DMA RAM. That allocation always fails on this board, so the
// BSP returns no display at all. We stay in the panel's native portrait orientation (no rotation) and use two
// 64-line draw buffers.
//
// Those draw buffers live in PSRAM, not internal RAM: the ESP32-P4 can DMA out of PSRAM, and every kilobyte of
// internal RAM is needed by the Wi-Fi stack and the HTTP server that streams RVDP bands.
lv_display_t* display_start() {
    hmi::createQueues();

    const bsp_display_cfg_t cfg = {
        .lvgl_port_cfg = ESP_LVGL_PORT_INIT_CONFIG(),
        .buffer_size = 800 * 64,
        .double_buffer = true,
        .flags = {
            .buff_dma = true,
            .buff_spiram = true,
            .sw_rotate = false,
        },
    };

    lv_display_t* disp = bsp_display_start_with_config(&cfg);
    if (!disp) {
        ESP_LOGE(TAG, "display start failed");
        return nullptr;
    }
    ESP_ERROR_CHECK(bsp_display_backlight_on());
    // From here every flushed area is also copied into the RVDP shadow frame.
    rvdp_transport::attach(disp);

    bsp_display_lock(0);
    demoInit();
    bsp_display_unlock();

    ESP_LOGI(TAG, "%dx%d display running", (int)lv_display_get_horizontal_resolution(disp),
             (int)lv_display_get_vertical_resolution(disp));
    return disp;
}

void display_service() {
    bsp_display_lock(0);
    demoTick();
    bsp_display_unlock();
}