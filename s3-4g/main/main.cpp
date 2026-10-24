// routeOS node on the Waveshare ESP32-S3-A-SIM7670X-4G (V2.0).
//
// What runs here:
//   * the Orbit calculator/notes UI, rendered headlessly and streamed over RVDP (WebSocket, port 81)
//   * LTE over the on-board SIM7670G, with a real Tailscale node on top of it (microlink)
//   * WiFi first when it is reachable, cellular otherwise - microlink's net switch owns that decision
//
// Board wiring (V2.0; the "4G" DIP switch on the back must be ON or GPIO17/18 are dead):
//   modem UART    ESP TX GPIO18 -> modem RX, ESP RX GPIO17 <- modem TX (through a TXB0104 shifter)
//   modem enable  GPIO21 held HIGH (MOSFET gate; documented as possibly not fitted on some units)
//
// The UI is started first and the network second, deliberately: the display works with no link at all, and a viewer can
// connect the moment either network comes up.
#include <stdio.h>
#include <string.h>

#include "driver/gpio.h"
#include "esp_event.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"

#include "display.h"
#include "microlink.h"
#include "ml_cellular.h"
#include "ml_net_switch.h"
#include "rvdp_server.h"
// Credentials are not in the repository, so the header may not exist. Building without it is normal: the board runs
// cellular-only and says so at boot. Copy wifi_secrets.h.example to wifi_secrets.h to give it a Wi-Fi to try first.
#if __has_include("wifi_secrets.h")
#include "wifi_secrets.h"
#else
#define HMI_WIFI_SSID ""
#define HMI_WIFI_PASSWORD ""
#endif

static const char* TAG = "routeos-4g";
static constexpr gpio_num_t kModemEnablePin = GPIO_NUM_21;

// routeos::log is what rvdp_server.cpp writes its events through; map it onto ESP-IDF logging.
namespace routeos {
void log(char level, const char* format, ...) {
    va_list args;
    va_start(args, format);
    esp_log_writev(level == 'E' ? ESP_LOG_ERROR : level == 'W' ? ESP_LOG_WARN : ESP_LOG_INFO, "rvdp", format, args);
    va_end(args);
}
}  // namespace routeos

namespace {

// The board gates modem power through a MOSFET on GPIO21. Holding it high is what the vendor firmware does.
//
// It is pulsed low first on purpose: if this board reset while the modem was mid-session the modem can still be in data
// mode and then never answers AT again, which looks exactly like a dead modem on the next boot.
void modemPowerOn() {
    gpio_config_t cfg = {};
    cfg.pin_bit_mask = 1ULL << kModemEnablePin;
    cfg.mode = GPIO_MODE_OUTPUT;
    cfg.pull_up_en = GPIO_PULLUP_DISABLE;
    cfg.pull_down_en = GPIO_PULLDOWN_DISABLE;
    cfg.intr_type = GPIO_INTR_DISABLE;
    gpio_config(&cfg);
    gpio_set_level(kModemEnablePin, 0);
    vTaskDelay(pdMS_TO_TICKS(400));
    gpio_set_level(kModemEnablePin, 1);
    vTaskDelay(pdMS_TO_TICKS(1500));
}

const char* vpn_address() {
    static char address[16] = "--";
    microlink_t* link = ml_net_switch_get_handle();
    if (link && microlink_is_connected(link)) microlink_ip_to_str(microlink_get_vpn_ip(link), address);
    return address;
}

void on_net_connected(ml_net_type_t type, void*) {
    if (type == ML_NET_CELLULAR) {
        ESP_LOGI(TAG, "cellular up: RVDP is at ws://%s:81/rvdp", vpn_address());
        return;
    }
    esp_netif_t* netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    esp_netif_ip_info_t info = {};
    if (netif && esp_netif_get_ip_info(netif, &info) == ESP_OK && info.ip.addr) {
        ESP_LOGI(TAG, "wifi up: RVDP is at ws://" IPSTR ":81/rvdp", IP2STR(&info.ip));
    } else {
        ESP_LOGI(TAG, "wifi up, no address yet");
    }
}

void on_net_switching(ml_net_type_t from, ml_net_type_t to, void*) {
    ESP_LOGW(TAG, "transport %s -> %s", from == ML_NET_WIFI ? "wifi" : "cellular",
             to == ML_NET_WIFI ? "wifi" : "cellular");
}

void status_task(void*) {
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(30000));
        ml_cellular_info_t cell = {};
        ml_cellular_get_info(&cell);
        ESP_LOGI(TAG, "status: %s via %s | vpn %s | signal %d dBm | cell %s | heap %u | psram %u | rvdp waiting %u",
                 ml_net_switch_state_str(ml_net_switch_get_state()),
                 ml_net_switch_get_active() == ML_NET_WIFI ? "wifi" : "cellular", vpn_address(), cell.rssi_dbm,
                 cell.data_connected ? "up" : "down", (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM), (unsigned)rvdp_transport::pending());
    }
}

}  // namespace

// C linkage: this file is compiled as C++, and FreeRTOS is looking for the unmangled symbol.
extern "C" void app_main(void) {
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        nvs_flash_init();
    }
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    // Display first: it needs no network, and RVDP starts listening immediately.
    if (!display_start()) {
        ESP_LOGE(TAG, "display failed to start");
        return;
    }
    rvdp_transport::begin();
    modemPowerOn();

    xTaskCreate(status_task, "status", 4096, nullptr, 2, nullptr);

    // Tailscale is optional at this stage: without an auth key the node still serves the UI on the local link.
    if (CONFIG_ML_TAILSCALE_AUTH_KEY[0] == '\0') {
        ESP_LOGW(TAG, "no Tailscale auth key - not joining the tailnet");
    }

    // WiFi first (it is unreachable here, cubeIoT is a 5 GHz SSID), then cellular. Failback is off because the WiFi
    // network this board can see is not the one configured; retrying it forever would only stall the modem.
    ml_net_switch_config_t config = {
        .wifi_ssid = HMI_WIFI_SSID,
        .wifi_password = HMI_WIFI_PASSWORD,
        .sim_pin = nullptr,
        .apn = CONFIG_ML_CELLULAR_APN,
        .tailscale_auth_key = CONFIG_ML_TAILSCALE_AUTH_KEY,
        .device_name = CONFIG_ML_DEVICE_NAME[0] ? CONFIG_ML_DEVICE_NAME : "routeos-4g",
        .wifi_timeout_ms = 6000,
        .health_check_interval_ms = 30000,
        .health_fail_count = 3,
        .failback_check_ms = 0,
        .on_connected = on_net_connected,
        .on_disconnected = nullptr,
        .on_switching = on_net_switching,
        .user_data = nullptr,
    };
    ESP_ERROR_CHECK(ml_net_switch_init(&config));
    ESP_ERROR_CHECK(ml_net_switch_start());
    ESP_LOGI(TAG, "net switch started (apn %s)", CONFIG_ML_CELLULAR_APN);
}
