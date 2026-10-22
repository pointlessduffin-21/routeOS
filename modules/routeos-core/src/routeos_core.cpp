#include "routeos_core.h"
#include "routeos_drivers.h"
#include "routeos_internal.h"
#include "routeos_log.h"
#include "routeos_ota.h"
#include "routeos_wifi.h"
#include <ArduinoJson.h>
#include <Preferences.h>
#include <WiFi.h>
#include <uri/UriBraces.h>
#include <esp_heap_caps.h>
#include <esp_ota_ops.h>
#include <esp_system.h>
#include <lwip/sockets.h>

extern "C" const char routeos_image_tag[];       // stamped into the image by ROUTEOS_IMAGE(); linking fails without it

namespace routeos {

// Holding the marker here, in code every routeOS firmware runs, keeps the linker from discarding it
// whatever else in the image turns out to be unreachable.
static const char* volatile gImageTag = routeos_image_tag;

static const char* gFirmwareName = "routeos";
static const char* gFirmwareVersion = "0";
static const Module* gModules = nullptr;
static size_t gModuleCount = 0;
static bool gManage = false;
static char gToken[33] = {0};

static const char* family() {
#if CONFIG_IDF_TARGET_ESP32
  return "esp32";
#elif CONFIG_IDF_TARGET_ESP32S2
  return "esp32s2";
#elif CONFIG_IDF_TARGET_ESP32S3
  return "esp32s3";
#elif CONFIG_IDF_TARGET_ESP32C3
  return "esp32c3";
#elif CONFIG_IDF_TARGET_ESP32C6
  return "esp32c6";
#else
  return "unknown";
#endif
}

static const char* resetReason() {
  switch (esp_reset_reason()) {
    case ESP_RST_POWERON: return "power-on";
    case ESP_RST_EXT: return "external reset";
    case ESP_RST_SW: return "software reset";
    case ESP_RST_PANIC: return "panic";
    case ESP_RST_INT_WDT: return "interrupt watchdog";
    case ESP_RST_TASK_WDT: return "task watchdog";
    case ESP_RST_WDT: return "watchdog";
    case ESP_RST_DEEPSLEEP: return "deep sleep wake";
    case ESP_RST_BROWNOUT: return "brownout (supply voltage dipped)";
    default: return "unknown";
  }
}

void begin(const char* firmwareName, const char* firmwareVersion, const Module* modules, size_t count, bool allowManage) {
  gFirmwareName = firmwareName;
  gFirmwareVersion = firmwareVersion;
  gModules = modules;
  gModuleCount = count;
  gManage = allowManage;
  log('I', "boot: %s %s (core %s), reset: %s, free memory %u KB", firmwareName, firmwareVersion, CORE_VERSION, resetReason(),
      (unsigned)(ESP.getFreeHeap() / 1024));
  ota::begin();           // a freshly installed image is on trial; this may hand control back to the old one
  drivers::begin();
  if (gManage) {
    Preferences p;
    p.begin("roscore", true);
    String t = p.getString("token", "");
    p.end();
    if (t.length() == 32) strlcpy(gToken, t.c_str(), sizeof(gToken));
  }
}

void onNetworkUp() {
  ota::networkUp();
  if (!gManage || gToken[0]) {
    if (gManage) Serial.printf("[core] management token: %s\n", gToken);     // serial only, never the log
    return;
  }
  // The radio is on now, so esp_random() is a true random source.
  char token[33];
  for (int i = 0; i < 4; i++) snprintf(token + 8 * i, 9, "%08x", (unsigned)esp_random());
  Preferences p;
  p.begin("roscore", false);
  p.putString("token", token);
  p.end();
  strlcpy(gToken, token, sizeof(gToken));
  Serial.printf("[core] created management token: %s\n", gToken);
}

static String capabilities() {
  JsonDocument doc;
  doc["schema"] = 1;
  JsonObject dev = doc["device"].to<JsonObject>();
  dev["chip"] = ESP.getChipModel();
  dev["family"] = family();
  dev["revision"] = ESP.getChipRevision();
  dev["cores"] = ESP.getChipCores();
  dev["cpu_mhz"] = ESP.getCpuFreqMHz();
  dev["flash_mb"] = ESP.getFlashChipSize() / (1024 * 1024);
  dev["psram_kb"] = ESP.getPsramSize() / 1024;
  dev["sdk"] = ESP.getSdkVersion();
  uint64_t mac = ESP.getEfuseMac();
  char tail[7];
  snprintf(tail, sizeof(tail), "%06x", (unsigned)((mac >> 24) & 0xffffff));
  dev["id"] = tail;
  JsonObject mem = doc["memory"].to<JsonObject>();
  mem["heap_total_kb"] = ESP.getHeapSize() / 1024;
  mem["heap_free_kb"] = ESP.getFreeHeap() / 1024;
  mem["heap_min_kb"] = ESP.getMinFreeHeap() / 1024;
  mem["largest_block_kb"] = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT) / 1024;
  JsonObject part = doc["partitions"].to<JsonObject>();
  const esp_partition_t* running = esp_ota_get_running_partition();
  part["app_kb"] = running ? running->size / 1024 : 0;
  part["ota"] = esp_ota_get_next_update_partition(nullptr) != nullptr;
  JsonObject fw = doc["firmware"].to<JsonObject>();
  fw["name"] = gFirmwareName;
  fw["version"] = gFirmwareVersion;
  JsonArray mods = doc["modules"].to<JsonArray>();
  for (size_t i = 0; i < gModuleCount; i++) {
    JsonObject m = mods.add<JsonObject>();
    m["id"] = gModules[i].id;
    m["version"] = gModules[i].version;
  }
  JsonObject drv = doc["drivers"].to<JsonObject>();
  drv["supported"] = drivers::supported();
  drv["installed"] = drivers::count();
  drv["max"] = drivers::MAX_DRIVERS;
  drv["max_spec_bytes"] = drivers::MAX_SPEC;
  drivers::describePins(doc["pins"].to<JsonObject>());
  JsonObject mg = doc["manage"].to<JsonObject>();
  mg["enabled"] = gManage;
  mg["token_set"] = gToken[0] != 0;
  // What a console may offer for this board. Each is a token-protected route group.
  JsonArray features = doc["features"].to<JsonArray>();
  features.add("system");
  features.add("logs");
  features.add("wifi");
  if (esp_ota_get_next_update_partition(nullptr)) features.add("ota");
  doc["uptime_s"] = millis() / 1000;
  doc["reset_reason"] = resetReason();
  String out;
  serializeJson(doc, out);
  return out;
}

// Live state for a management console. Behind the token: it names the Wi-Fi network and addresses.
static String systemInfo() {
  JsonDocument doc;
  doc["schema"] = 1;
  JsonObject wifi = doc["wifi"].to<JsonObject>();
  bool up = WiFi.status() == WL_CONNECTED;
  wifi["connected"] = up;
  wifi["ssid"] = up ? WiFi.SSID() : String("");
  wifi["rssi"] = up ? WiFi.RSSI() : 0;
  wifi["channel"] = up ? WiFi.channel() : 0;
  wifi["ip"] = WiFi.localIP().toString();
  wifi["mac"] = WiFi.macAddress();
  JsonObject mem = doc["memory"].to<JsonObject>();
  mem["heap_free"] = ESP.getFreeHeap();
  mem["heap_min"] = ESP.getMinFreeHeap();
  mem["largest_block"] = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
  JsonObject fw = doc["firmware"].to<JsonObject>();
  fw["sketch_kb"] = ESP.getSketchSize() / 1024;
  fw["free_sketch_kb"] = ESP.getFreeSketchSpace() / 1024;
  doc["uptime_s"] = millis() / 1000;
  doc["reset_reason"] = resetReason();
  String out;
  serializeJson(doc, out);
  return out;
}

static void rebootTask(void*) {
  vTaskDelay(pdMS_TO_TICKS(700));     // let the HTTP reply leave first
  ESP.restart();
}

// ---- A web server that does not let one odd client stop the device (see Server in routeos_core.h) ----
static constexpr uint32_t REQUEST_LIMIT_MS = 30000;   // far longer than any honest request takes
static volatile uint32_t gServing = 0;                // millis() when the current handleClient() began; 0 when idle
static Server* gServer = nullptr;

void Server::handleClient() {
  static_cast<Stream&>(_currentClient).setTimeout(1000);     // the library re-arms 5 s after each request
  gServer = this;
  gServing = millis() | 1;
  WebServer::handleClient();
  gServing = 0;
}

static void requestWatchdog(void*) {
  for (;;) {
    vTaskDelay(pdMS_TO_TICKS(1000));
    uint32_t since = gServing;
    if (since && gServer && millis() - since > REQUEST_LIMIT_MS) {
      int fd = gServer->currentFd();
      log('W', "a web request has been stuck for %u s; closing its connection", (unsigned)(REQUEST_LIMIT_MS / 1000));
      if (fd >= 0) lwip_shutdown(fd, SHUT_RDWR);        // the library's readers give up when the socket dies
    }
  }
}

namespace internal {

void reply(WebServer& http, int code, const String& body) {
  http.sendHeader("Cache-Control", "no-store");
  http.send(code, "application/json", body);
}

void fail(WebServer& http, int code, const String& message) {
  JsonDocument doc;
  doc["error"] = message;
  String out;
  serializeJson(doc, out);
  reply(http, code, out);
}

bool tokenValid(WebServer& http) {
  if (!gManage || !gToken[0]) return false;
  String got = http.header("Authorization");
  String want = String("Bearer ") + gToken;
  uint8_t diff = got.length() == want.length() ? 0 : 1;
  for (size_t i = 0; i < got.length() && i < want.length(); i++) diff |= got[i] ^ want[i];
  return diff == 0;
}

bool authorized(WebServer& http) {
  if (!gManage) { fail(http, 403, "Management is disabled in this firmware build."); return false; }
  if (!gToken[0]) { fail(http, 503, "The management token is not ready yet. Try again in a moment."); return false; }
  if (!tokenValid(http)) {
    http.sendHeader("WWW-Authenticate", "Bearer");
    fail(http, 401, "The management token was rejected.");
    return false;
  }
  return true;
}

void rebootSoon() { xTaskCreate(rebootTask, "ros-reboot", 2048, nullptr, 1, nullptr); }
const char* firmwareName() { return gFirmwareName; }
const char* firmwareVersion() { return gFirmwareVersion; }
const char* resetReasonText() { return resetReason(); }

}  // namespace internal

void attach(WebServer& http) {
  using namespace internal;
  static const char* headers[] = {"Authorization", "X-Ota-Offset"};
  http.collectHeaders(headers, 2);
  http.on("/api/capabilities", HTTP_GET, [&http]() { reply(http, 200, capabilities()); });
  http.on("/api/system", HTTP_GET, [&http]() { if (authorized(http)) reply(http, 200, systemInfo()); });
  http.on("/api/logs", HTTP_GET, [&http]() {
    if (!authorized(http)) return;
    long since = http.arg("since").toInt();
    reply(http, 200, logsJson(since > 0 ? (uint32_t)since : 0));
  });
  http.on("/api/reboot", HTTP_POST, [&http]() {
    if (!authorized(http)) return;
    log('I', "restart requested through the management API");
    reply(http, 200, "{\"ok\":true,\"rebooting\":true}");
    rebootSoon();
  });
  wifi::attach(http);
  ota::attach(http);
  xTaskCreate(requestWatchdog, "ros-http", 2048, nullptr, 1, nullptr);
  http.on("/api/drivers", HTTP_GET, [&http]() { if (authorized(http)) reply(http, 200, drivers::listJson()); });
  http.on("/api/drivers/readings", HTTP_GET, [&http]() { if (authorized(http)) reply(http, 200, drivers::readingsJson()); });
  http.on(UriBraces("/api/drivers/{}"), HTTP_PUT, [&http]() {
    if (!authorized(http)) return;
    String error;
    String id = http.pathArg(0);
    if (!drivers::install(id, http.arg("plain"), error)) { fail(http, 400, error); return; }
    JsonDocument doc;
    doc["ok"] = true;
    doc["id"] = id;
    String out;
    serializeJson(doc, out);
    reply(http, 200, out);
  });
  http.on(UriBraces("/api/drivers/{}"), HTTP_DELETE, [&http]() {
    if (!authorized(http)) return;
    String error;
    if (!drivers::remove(http.pathArg(0), error)) { fail(http, 404, error); return; }
    reply(http, 200, "{\"ok\":true}");
  });
}

}  // namespace routeos
