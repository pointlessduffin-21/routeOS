#include <stdarg.h>
#include <string.h>
#include <time.h>
#include "esp_chip_info.h"
#include "esp_flash.h"#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_ota_ops.h"
#include "esp_psram.h"
#include "esp_random.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "ros_internal.h"

extern "C" const char routeos_image_tag[];       // stamped into the image by ROUTEOS_IMAGE(); linking fails without it

namespace routeos {

uint32_t millis() { return (uint32_t)(esp_timer_get_time() / 1000); }

// ---- Event log: the last 48 lines in RAM ----
namespace {
constexpr size_t SLOTS = 48, TEXT = 92;
struct Line { uint32_t seq, ms; char level; char text[TEXT]; };
Line ring[SLOTS];
uint32_t nextSeq = 1;
portMUX_TYPE mux = portMUX_INITIALIZER_UNLOCKED;
uint32_t boot = 0;

const char* gFirmwareName = "routeos";
const char* gFirmwareVersion = "0";
const Module* gModules = nullptr;
size_t gModuleCount = 0;
bool gManage = false;
char gToken[33] = {0};
const void* gImageTag = routeos_image_tag;          // keeps the marker in the image whatever the linker thinks
}  // namespace

uint32_t bootId() {
  if (!boot) boot = esp_random() | 1;
  return boot;
}

void log(char level, const char* fmt, ...) {
  char text[TEXT];
  va_list args;
  va_start(args, fmt);
  vsnprintf(text, sizeof(text), fmt, args);
  va_end(args);
  for (char* p = text; *p; p++) {
    if ((uint8_t)*p < 32 || (uint8_t)*p > 126) *p = '?';           // one printable line each
  }
  if (level != 'W' && level != 'E') level = 'I';
  uint32_t ms = millis();
  portENTER_CRITICAL(&mux);
  Line& line = ring[nextSeq % SLOTS];
  line.seq = nextSeq;
  line.ms = ms;
  line.level = level;
  memcpy(line.text, text, sizeof(text));
  nextSeq++;
  portEXIT_CRITICAL(&mux);
  printf("[core] %c %s\n", level, text);
}

static std::string logsJson(uint32_t since) {
  uint32_t last;
  portENTER_CRITICAL(&mux);
  last = nextSeq - 1;
  portEXIT_CRITICAL(&mux);
  uint32_t oldest = last >= SLOTS ? last - SLOTS + 1 : 1;
  uint32_t start = since + 1 > oldest ? since + 1 : oldest;
  Json j;
  cJSON_AddNumberToObject(j.root(), "schema", 1);
  cJSON_AddNumberToObject(j.root(), "boot_id", bootId());
  cJSON_AddNumberToObject(j.root(), "uptime_s", millis() / 1000);
  cJSON_AddNumberToObject(j.root(), "next", last);
  cJSON_AddNumberToObject(j.root(), "dropped", (since > 0 && since + 1 < oldest) ? oldest - (since + 1) : 0);
  cJSON* lines = cJSON_AddArrayToObject(j.root(), "lines");
  for (uint32_t seq = start; seq <= last && seq != 0; seq++) {
    Line copy;
    portENTER_CRITICAL(&mux);
    copy = ring[seq % SLOTS];
    portEXIT_CRITICAL(&mux);
    if (copy.seq != seq) continue;                                  // overwritten while we were reading
    cJSON* item = cJSON_CreateObject();
    cJSON_AddNumberToObject(item, "n", copy.seq);
    cJSON_AddNumberToObject(item, "t", copy.ms);
    cJSON_AddStringToObject(item, "l", copy.level == 'W' ? "W" : copy.level == 'E' ? "E" : "I");
    cJSON_AddStringToObject(item, "m", copy.text);
    cJSON_AddItemToArray(lines, item);
  }
  return j.print();
}

// ---- HTTP helpers ----
static const char* statusLine(int code) {
  switch (code) {
    case 200: return "200 OK";
    case 400: return "400 Bad Request";
    case 401: return "401 Unauthorized";
    case 403: return "403 Forbidden";
    case 404: return "404 Not Found";
    case 409: return "409 Conflict";
    case 413: return "413 Content Too Large";
    case 500: return "500 Internal Server Error";
    case 503: return "503 Service Unavailable";
    default: return "400 Bad Request";
  }
}

esp_err_t reply(httpd_req_t* req, int code, const std::string& json) {
  httpd_resp_set_status(req, statusLine(code));
  httpd_resp_set_type(req, "application/json");
  httpd_resp_set_hdr(req, "Cache-Control", "no-store");
  return httpd_resp_send(req, json.c_str(), json.size());
}

esp_err_t fail(httpd_req_t* req, int code, const std::string& message) {
  Json j;
  cJSON_AddStringToObject(j.root(), "error", message.c_str());
  return reply(req, code, j.print());
}

namespace internal {

bool readBody(httpd_req_t* req, size_t max, std::string& out) {
  if (req->content_len > max) { fail(req, 413, "That request body is too large"); return false; }
  out.assign(req->content_len, '\0');
  size_t got = 0;
  while (got < req->content_len) {
    int n = httpd_req_recv(req, &out[got], req->content_len - got);
    if (n == HTTPD_SOCK_ERR_TIMEOUT) continue;
    if (n <= 0) return false;                                       // the socket died; nothing to answer
    got += n;
  }
  return true;
}

bool tokenValid(httpd_req_t* req) {
  if (!gManage || !gToken[0]) return false;
  char got[96] = {0};
  if (httpd_req_get_hdr_value_str(req, "Authorization", got, sizeof(got)) != ESP_OK) return false;
  std::string want = std::string("Bearer ") + gToken;
  size_t n = strlen(got);
  uint8_t diff = n == want.size() ? 0 : 1;
  for (size_t i = 0; i < n && i < want.size(); i++) diff |= got[i] ^ want[i];   // constant time over the shorter
  return diff == 0;
}

const char* firmwareName() { return gFirmwareName; }
const char* firmwareVersion() { return gFirmwareVersion; }

const char* resetReasonText() {
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

static void rebootTask(void*) {
  vTaskDelay(pdMS_TO_TICKS(700));                                   // let the HTTP reply leave first
  esp_restart();
}
void rebootSoon() { xTaskCreate(rebootTask, "ros-reboot", 3072, nullptr, 5, nullptr); }

}  // namespace internal

bool authorized(httpd_req_t* req) {
  if (!gManage) { fail(req, 403, "Management is disabled in this firmware build."); return false; }
  if (!gToken[0]) { fail(req, 503, "The management token is not ready yet. Try again in a moment."); return false; }
  if (!internal::tokenValid(req)) {
    httpd_resp_set_hdr(req, "WWW-Authenticate", "Bearer");
    fail(req, 401, "The management token was rejected.");
    return false;
  }
  return true;
}

// ---- Self-check: restart rather than sit on a dead network ----
// A starved heap leaves Wi-Fi and TCP without packet buffers and nothing recovers (seen on the ESP32 after hours).
// Restarting is the only way out. The test is on internal memory, which is what the network stack allocates from.
static constexpr uint32_t LOW_BLOCK_BYTES = 8192, LOW_FREE_BYTES = 32768, LOW_SPAN_MS = 10000, CHECK_AFTER_MS = 20000;

static void memoryWatch() {
  static uint32_t lowSince = 0;
  if (millis() < CHECK_AFTER_MS) return;
  size_t largest = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  size_t freeBytes = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  if (largest >= LOW_BLOCK_BYTES && freeBytes >= LOW_FREE_BYTES) { lowSince = 0; return; }
  if (!lowSince) {
    lowSince = millis() | 1;
    log('W', "memory is nearly exhausted: largest block %u B, %u B free", (unsigned)largest, (unsigned)freeBytes);
    return;
  }
  if (millis() - lowSince < LOW_SPAN_MS) return;
  char why[88];
  snprintf(why, sizeof(why), "largest block %u B, %u KB free, up %u min", (unsigned)largest, (unsigned)(freeBytes / 1024), (unsigned)(millis() / 60000));
  log('E', "memory exhausted for %u s (%s): restarting", (unsigned)(LOW_SPAN_MS / 1000), why);
  Nvs("rossys", true).put("why", why);
  vTaskDelay(pdMS_TO_TICKS(300));
  esp_restart();
}

static void watchTask(void*) {
  for (;;) {
    vTaskDelay(pdMS_TO_TICKS(500));
    wifi::watch();
    memoryWatch();
  }
}

// ---- begin / token ----
void begin(const char* firmwareName, const char* firmwareVersion, const Module* modules, size_t count, bool allowManage) {
  (void)gImageTag;
  gFirmwareName = firmwareName;
  gFirmwareVersion = firmwareVersion;
  gModules = modules;
  gModuleCount = count;
  gManage = allowManage;
  log('I', "boot: %s %s (core %s), reset: %s, free memory %u KB", firmwareName, firmwareVersion, CORE_VERSION,
      internal::resetReasonText(), (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024));
  {
    Nvs p("rossys", true);
    std::string why = p.str("why");
    if (!why.empty()) {
      p.erase("why");
      log('W', "the last run was restarted by the self-check: %s", why.c_str());
    }
  }
  ota::begin();           // a freshly installed image is on trial; this may hand control back to the old one
  if (gManage) {
    std::string t = Nvs("roscore", false).str("token");
    if (t.size() == 32) strlcpy(gToken, t.c_str(), sizeof(gToken));
  }
  xTaskCreate(watchTask, "ros-watch", 4096, nullptr, 3, nullptr);
}

namespace wifi_hook {
// Called by the Wi-Fi code when the station has an address: the radio is on, so esp_random() is a true random source.
void networkUp() {
  ota::networkUp();
  if (!gManage || gToken[0]) {
    if (gManage) {
      printf("[core] management token: %s\n", gToken);          // serial only, never the log
      ESP_LOGI("ros-auth", "management token: %s", gToken);      // and the USB console, which is the only one some boards expose
    }
    return;
  }
  char token[33];
  for (int i = 0; i < 4; i++) snprintf(token + 8 * i, 9, "%08x", (unsigned)esp_random());
  Nvs("roscore", true).put("token", token);
  strlcpy(gToken, token, sizeof(gToken));
  printf("[core] created management token: %s\n", gToken);
  ESP_LOGI("ros-auth", "created management token: %s", gToken);
}
}  // namespace wifi_hook

// ---- Routes ----
static std::string capabilities() {
  Json j;
  cJSON* root = j.root();
  cJSON_AddNumberToObject(root, "schema", 1);
  esp_chip_info_t info;
  esp_chip_info(&info);
  uint32_t flashBytes = 0;
  esp_flash_get_size(nullptr, &flashBytes);
  uint8_t mac[6] = {0};
  esp_efuse_mac_get_default(mac);
  char id[8];
  snprintf(id, sizeof(id), "%02x%02x%02x", mac[3], mac[4], mac[5]);
  cJSON* dev = cJSON_AddObjectToObject(root, "device");
  cJSON_AddStringToObject(dev, "chip", "ESP32-P4");
  cJSON_AddStringToObject(dev, "family", "esp32p4");
  cJSON_AddNumberToObject(dev, "revision", info.revision);
  cJSON_AddNumberToObject(dev, "cores", info.cores);
  cJSON_AddNumberToObject(dev, "cpu_mhz", CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ);
  cJSON_AddNumberToObject(dev, "flash_mb", flashBytes / (1024 * 1024));
  cJSON_AddNumberToObject(dev, "psram_kb", esp_psram_get_size() / 1024);
  cJSON_AddStringToObject(dev, "sdk", esp_get_idf_version());
  cJSON_AddStringToObject(dev, "id", id);
  cJSON* mem = cJSON_AddObjectToObject(root, "memory");
  cJSON_AddNumberToObject(mem, "heap_total_kb", heap_caps_get_total_size(MALLOC_CAP_8BIT) / 1024);
  cJSON_AddNumberToObject(mem, "heap_free_kb", heap_caps_get_free_size(MALLOC_CAP_8BIT) / 1024);
  cJSON_AddNumberToObject(mem, "heap_min_kb", heap_caps_get_minimum_free_size(MALLOC_CAP_8BIT) / 1024);
  cJSON_AddNumberToObject(mem, "largest_block_kb", heap_caps_get_largest_free_block(MALLOC_CAP_8BIT) / 1024);
  const esp_partition_t* running = esp_ota_get_running_partition();
  cJSON* part = cJSON_AddObjectToObject(root, "partitions");
  cJSON_AddNumberToObject(part, "app_kb", running ? running->size / 1024 : 0);
  cJSON_AddBoolToObject(part, "ota", esp_ota_get_next_update_partition(nullptr) != nullptr);
  cJSON* fw = cJSON_AddObjectToObject(root, "firmware");
  cJSON_AddStringToObject(fw, "name", gFirmwareName);
  cJSON_AddStringToObject(fw, "version", gFirmwareVersion);
  cJSON* mods = cJSON_AddArrayToObject(root, "modules");
  cJSON* core = cJSON_CreateObject();
  cJSON_AddStringToObject(core, "id", "core");
  cJSON_AddStringToObject(core, "version", CORE_VERSION);
  cJSON_AddItemToArray(mods, core);
  for (size_t i = 0; i < gModuleCount; i++) {
    cJSON* m = cJSON_CreateObject();
    cJSON_AddStringToObject(m, "id", gModules[i].id);
    cJSON_AddStringToObject(m, "version", gModules[i].version);
    cJSON_AddItemToArray(mods, m);
  }
  cJSON* drv = cJSON_AddObjectToObject(root, "drivers");
  cJSON_AddBoolToObject(drv, "supported", false);
  cJSON_AddNumberToObject(drv, "installed", 0);
  cJSON_AddNumberToObject(drv, "max", 0);
  cJSON_AddNumberToObject(drv, "max_spec_bytes", 0);
  cJSON* pins = cJSON_AddObjectToObject(root, "pins");
  cJSON_AddArrayToObject(pins, "allowed");
  cJSON_AddArrayToObject(pins, "input_only");
  cJSON_AddArrayToObject(pins, "adc");
  cJSON* man = cJSON_AddObjectToObject(root, "manage");
  cJSON_AddBoolToObject(man, "enabled", gManage);
  cJSON_AddBoolToObject(man, "token_set", gManage && gToken[0]);
  cJSON* features = cJSON_AddArrayToObject(root, "features");
  if (gManage) {
    for (const char* f : {"system", "logs", "wifi", "ota"}) cJSON_AddItemToArray(features, cJSON_CreateString(f));
  }
  cJSON_AddNumberToObject(root, "uptime_s", millis() / 1000);
  cJSON_AddStringToObject(root, "reset_reason", internal::resetReasonText());
  return j.print();
}

static std::string systemJson() {
  Json j;
  cJSON* root = j.root();
  cJSON_AddNumberToObject(root, "schema", 1);
  cJSON* w = cJSON_AddObjectToObject(root, "wifi");
  bool up = wifi::up();
  cJSON_AddBoolToObject(w, "connected", up);
  cJSON_AddStringToObject(w, "ssid", up ? wifi::ssid().c_str() : "");
  cJSON_AddNumberToObject(w, "rssi", up ? wifi::rssi() : 0);
  cJSON_AddNumberToObject(w, "channel", up ? wifi::channel() : 0);
  cJSON_AddStringToObject(w, "ip", wifi::ip().c_str());
  cJSON_AddStringToObject(w, "mac", wifi::mac().c_str());
  cJSON* mem = cJSON_AddObjectToObject(root, "memory");
  cJSON_AddNumberToObject(mem, "heap_free", heap_caps_get_free_size(MALLOC_CAP_8BIT));
  cJSON_AddNumberToObject(mem, "heap_min", heap_caps_get_minimum_free_size(MALLOC_CAP_8BIT));
  cJSON_AddNumberToObject(mem, "largest_block", heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
  const esp_partition_t* running = esp_ota_get_running_partition();
  const esp_partition_t* other = esp_ota_get_next_update_partition(nullptr);
  cJSON* fw = cJSON_AddObjectToObject(root, "firmware");
  cJSON_AddNumberToObject(fw, "sketch_kb", running ? running->size / 1024 : 0);
  cJSON_AddNumberToObject(fw, "free_sketch_kb", other ? other->size / 1024 : 0);
  cJSON_AddNumberToObject(root, "uptime_s", millis() / 1000);
  cJSON_AddStringToObject(root, "reset_reason", internal::resetReasonText());
  return j.print();
}

httpd_handle_t startServer() {
  httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
  cfg.max_uri_handlers = 40;
  cfg.stack_size = 12288;
  cfg.recv_wait_timeout = 8;        // a client that stops sending is dropped; nothing waits for ever
  cfg.send_wait_timeout = 8;
  cfg.lru_purge_enable = true;      // a full socket table frees its oldest connection instead of refusing new ones
  cfg.max_open_sockets = 10;
  cfg.uri_match_fn = httpd_uri_match_wildcard;
  httpd_handle_t server = nullptr;
  if (httpd_start(&server, &cfg) != ESP_OK) { log('E', "the web server could not start"); return nullptr; }

  static const httpd_uri_t caps = {"/api/capabilities", HTTP_GET, [](httpd_req_t* r) { return reply(r, 200, capabilities()); }, nullptr};
  static const httpd_uri_t sys = {"/api/system", HTTP_GET, [](httpd_req_t* r) { return authorized(r) ? reply(r, 200, systemJson()) : ESP_OK; }, nullptr};
  static const httpd_uri_t logs = {"/api/logs", HTTP_GET, [](httpd_req_t* r) {
    if (!authorized(r)) return ESP_OK;
    char q[48] = {0}, v[16] = {0};
    long since = 0;
    if (httpd_req_get_url_query_str(r, q, sizeof(q)) == ESP_OK && httpd_query_key_value(q, "since", v, sizeof(v)) == ESP_OK) since = atol(v);
    return reply(r, 200, logsJson(since > 0 ? (uint32_t)since : 0));
  }, nullptr};
  static const httpd_uri_t reboot = {"/api/reboot", HTTP_POST, [](httpd_req_t* r) {
    if (!authorized(r)) return ESP_OK;
    log('I', "restart requested through the management API");
    esp_err_t e = reply(r, 200, "{\"ok\":true,\"rebooting\":true}");
    internal::rebootSoon();
    return e;
  }, nullptr};
  static const httpd_uri_t drivers = {"/api/drivers*", HTTP_GET, [](httpd_req_t* r) {
    if (!authorized(r)) return ESP_OK;
    return fail(r, 409, "Drivers are not supported on this chip yet");
  }, nullptr};
  httpd_register_uri_handler(server, &caps);
  httpd_register_uri_handler(server, &sys);
  httpd_register_uri_handler(server, &logs);
  httpd_register_uri_handler(server, &reboot);
  httpd_register_uri_handler(server, &drivers);
  wifi::attach(server);
  ota::attach(server);
  pageAttach(server);
  return server;
}

}  // namespace routeos
