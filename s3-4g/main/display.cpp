// Headless display for the ESP32-S3 4G node.
//
// There is no panel on this board, so LVGL renders into a PSRAM draw buffer and the flush callback only marks the work
// done. The RVDP transport wraps that flush and keeps the shadow frame, which means the browser is the only screen this
// node ever has — and the UI code itself is the same as the P4's, chosen by RVDP_WIDTH/RVDP_HEIGHT at build time.
#include <atomic>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "lvgl.h"

#include "display.h"
#include "p4_transport.h"
#include "rvdp_server.h"
#include "ui_bindings.h"

static const char* TAG = "display";

namespace hmi {
QueueHandle_t inputs, keys;
std::atomic<uint32_t> epoch{0}, drops{0}, sentBytes{0}, batches{0}, inputEpoch{0};
std::atomic<bool> inputReady{false}, ready{false}, refresh{false}, overflow{false};

void createQueues() {
    if (!inputs) inputs = xQueueCreate(16, sizeof(rvdp::Input));
    if (!keys) keys = xQueueCreate(32, sizeof(rvdp::KeyInput));
}
}  // namespace hmi

namespace {
// LVGL runs in its own task here rather than inside the adapter's: a recursive mutex keeps the UI, the RVDP server and
// this file from touching the tree at the same time.
SemaphoreHandle_t uiLock;
lv_display_t* panel;

void flush(lv_display_t* disp, const lv_area_t* area, uint8_t* pixels) {
    (void)area;
    (void)pixels;
    lv_display_flush_ready(disp);        // nothing to push: the RVDP capture already kept the pixels
}

void tick(void*) {
    for (;;) {
        if (xSemaphoreTakeRecursive(uiLock, portMAX_DELAY) == pdTRUE) {
            demoTick();
            lv_timer_handler();
            xSemaphoreGiveRecursive(uiLock);
        }
        vTaskDelay(pdMS_TO_TICKS(5));
    }
}
}  // namespace

bool display_lock(uint32_t timeoutMs) {
    return xSemaphoreTakeRecursive(uiLock, timeoutMs ? pdMS_TO_TICKS(timeoutMs) : portMAX_DELAY) == pdTRUE;
}

void display_unlock() {
    xSemaphoreGiveRecursive(uiLock);
}

lv_display_t* display_start() {
    hmi::createQueues();
    uiLock = xSemaphoreCreateRecursiveMutex();
    lv_init();

    panel = lv_display_create(HMI_WIDTH, HMI_HEIGHT);
    if (!panel) {
        ESP_LOGE(TAG, "cannot create the LVGL display");
        return nullptr;
    }
    // Two 64-line draw buffers in PSRAM: internal RAM on this board is needed by lwIP, PPP and the VPN.
    const size_t bufferBytes = size_t(HMI_WIDTH) * 64 * 2;
    void* buffer1 = heap_caps_malloc(bufferBytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    void* buffer2 = heap_caps_malloc(bufferBytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!buffer1 || !buffer2) {
        ESP_LOGE(TAG, "no PSRAM for the LVGL draw buffers (%u bytes each)", (unsigned)bufferBytes);
        return nullptr;
    }
    lv_display_set_buffers(panel, buffer1, buffer2, bufferBytes, LV_DISPLAY_RENDER_MODE_PARTIAL);

    // Wrap the flush BEFORE LVGL ever draws, so the very first frame is captured too.
    lv_display_set_flush_cb(panel, flush);
    rvdp_transport::attach(panel);

    if (display_lock(0)) {
        demoInit();
        display_unlock();
    }
    xTaskCreate(tick, "ui", 6144, nullptr, 4, nullptr);
    ESP_LOGI(TAG, "headless %dx%d display running", HMI_WIDTH, HMI_HEIGHT);
    return panel;
}

void display_service() {
    // The UI runs in its own task; kept so the shared UI code links unchanged.
}
