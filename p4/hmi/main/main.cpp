#include <stdio.h>
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"
#include "display.h"
#include "routeos.h"
#include "p4_transport.h"
#include "rvdp_server.h"
#include "ui_bindings.h"
#if __has_include("wifi_secrets.h")
#include "wifi_secrets.h"
#endif
#ifndef HMI_WIFI_SSID
#define HMI_WIFI_SSID ""
#define HMI_WIFI_PASSWORD ""
#endif

static const char* TAG = "routeos-p4";

#ifndef FW_NAME
#define FW_NAME "routeos-p4-hmi"
#endif
#ifndef FW_VERSION
#define FW_VERSION "1.0.0"
#endif
// Marks this image as routeOS firmware, so Termz and the device's updater will accept it.
ROUTEOS_IMAGE(FW_NAME, FW_VERSION);
static const routeos::Module kModules[] = {{"hmi", "1.0.0"}, {"rvdp", "1.1"}};

static void ui_task(void*) {
    for (;;) {
        display_service();
        vTaskDelay(pdMS_TO_TICKS(5));
    }
}

// What Termz's health strip polls. Same fields as the ESP32 firmware's /health.
static esp_err_t health(httpd_req_t* req) {
    DemoState state;
    demoSnapshot(state);
    char body[640];
    snprintf(body, sizeof(body),
             "{\"width\":%d,\"height\":%d,\"heap\":%u,\"min_heap\":%u,\"largest_block\":%u,\"psram\":%u,\"queue\":%u,\"drops\":%u,"
             "\"batches\":%u,\"bytes\":%u,\"ui_us\":0,\"ui_stack\":0,\"net_stack\":0,\"control_enabled\":true,\"page\":\"%s\","
             "\"expression\":\"%s\",\"result\":\"%s\",\"history_count\":%u,\"note_length\":%u}",
             HMI_WIDTH, HMI_HEIGHT, (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL), (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL), (unsigned)heap_caps_get_total_size(MALLOC_CAP_SPIRAM),
             (unsigned)rvdp_transport::pending(), (unsigned)hmi::drops.load(), (unsigned)hmi::batches.load(), (unsigned)hmi::sentBytes.load(),
             state.page, state.expression, state.result, state.historyCount, state.noteLength);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, body, HTTPD_RESP_USE_STRLEN);
}

extern "C" void app_main(void) {
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        nvs_flash_init();
    }
    routeos::begin(FW_NAME, FW_VERSION, kModules, sizeof(kModules) / sizeof(kModules[0]), true);
    display_start();
    xTaskCreate(ui_task, "ui", 8192, NULL, 4, NULL);
    if (!routeos::wifiBegin(HMI_WIFI_SSID, HMI_WIFI_PASSWORD)) ESP_LOGW(TAG, "no Wi-Fi network configured");
    httpd_handle_t server = routeos::startServer();
    if (server) {
        static const httpd_uri_t uri = {"/health", HTTP_GET, health, nullptr};
        httpd_register_uri_handler(server, &uri);
    }
    // The screen transport lives on its own server, port 81, so a busy viewer never delays management requests.
    rvdp_transport::begin();
    ESP_LOGI(TAG, "calculator and notepad running");
}