// Wi-Fi for the ESP32-P4: the chip has no radio, so esp_wifi talks to the ESP32-C6 on the board (esp_wifi_remote over
// esp_hosted). To the code here it is the ordinary esp_wifi API.
#include <ctype.h>
#include <string.h>
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"
#include "ros_internal.h"

namespace routeos {
namespace wifi_hook { void networkUp(); }

namespace {

struct Cred { char ssid[33]; char pass[65]; bool used; };
Cred builtin = {"", "", false};       // compiled into the firmware
Cred saved = {"", "", false};         // set through the API, kept in flash
uint8_t activeIdx = 0;
bool started = false;
volatile bool gotIp = false;
esp_netif_t* staNetif = nullptr;

volatile bool trialActive = false;
char trialSsid[33] = "";
uint32_t trialStartedMs = 0;
struct Result { char state[10]; char ssid[33]; char reason[72]; uint32_t atMs; };
Result lastResult = {"none", "", "", 0};

uint32_t disconnects = 0, lastDisconnectMs = 0, scanTouched = 0;
uint8_t lastReason = 0;

constexpr int MAX_SCAN = 40;
wifi_ap_record_t scanRecs[MAX_SCAN];
uint16_t scanCount = 0;
volatile bool scanRunning = false, scanHave = false, diagScan = false;

const char* reasonText(uint8_t reason) {
  switch (reason) {
    case 1: return "unspecified";
    case 2: return "authentication expired";
    case 3: return "the access point sent us away";
    case 4: return "inactive too long";
    case 5: return "the access point is full";
    case 8: return "left the access point";
    case 15: return "4-way handshake timeout (wrong password?)";
    case 16: return "group key update timeout";
    case 17: return "handshake information mismatch";
    case 23: return "802.1X authentication failed";
    case 200: return "beacon timeout: the access point stopped answering (weak signal?)";
    case 201: return "no access point with that name was found";
    case 202: return "authentication failed (wrong password?)";
    case 203: return "association failed";
    case 204: return "handshake timeout";
    case 205: return "connection failed";
    default: return "other reason";
  }
}

const char* authName(wifi_auth_mode_t mode) {
  switch (mode) {
    case WIFI_AUTH_OPEN: return "open";
    case WIFI_AUTH_WEP: return "WEP";
    case WIFI_AUTH_WPA_PSK: return "WPA";
    case WIFI_AUTH_WPA2_PSK: return "WPA2";
    case WIFI_AUTH_WPA_WPA2_PSK: return "WPA/WPA2";
    case WIFI_AUTH_WPA2_ENTERPRISE: return "WPA2-Enterprise";
    case WIFI_AUTH_WPA3_PSK: return "WPA3";
    case WIFI_AUTH_WPA2_WPA3_PSK: return "WPA2/WPA3";
    default: return "other";
  }
}

void loadSaved() {
  Nvs p("roswifi", false);
  std::string ssid = p.str("ssid"), pass = p.str("pass");
  if (ssid.size() >= 1 && ssid.size() <= 32 && pass.size() <= 64) {
    strlcpy(saved.ssid, ssid.c_str(), sizeof(saved.ssid));
    strlcpy(saved.pass, pass.c_str(), sizeof(saved.pass));
    saved.used = true;
  }
}

void storeSaved(const char* ssid, const char* pass) {
  Nvs p("roswifi", true);
  p.put("ssid", ssid);
  p.put("pass", pass);
  strlcpy(saved.ssid, ssid, sizeof(saved.ssid));
  strlcpy(saved.pass, pass, sizeof(saved.pass));
  saved.used = true;
}

// Saved first, then built-in; the same network is not listed twice.
int candidates(const Cred* out[2]) {
  int n = 0;
  if (saved.used) out[n++] = &saved;
  bool same = saved.used && !strcmp(saved.ssid, builtin.ssid) && !strcmp(saved.pass, builtin.pass);
  if (builtin.used && !same) out[n++] = &builtin;
  return n;
}

void connectTo(const char* ssid, const char* pass) {
  wifi_config_t cfg = {};
  strlcpy((char*)cfg.sta.ssid, ssid, sizeof(cfg.sta.ssid));
  strlcpy((char*)cfg.sta.password, pass, sizeof(cfg.sta.password));
  cfg.sta.threshold.authmode = pass[0] ? WIFI_AUTH_WPA_PSK : WIFI_AUTH_OPEN;
  // Join the strongest access point of a network, not the first one the radio hears.
  cfg.sta.scan_method = WIFI_ALL_CHANNEL_SCAN;
  cfg.sta.sort_method = WIFI_CONNECT_AP_BY_SIGNAL;
  cfg.sta.pmf_cfg.capable = true;
  cfg.sta.pmf_cfg.required = false;
  esp_wifi_set_config(WIFI_IF_STA, &cfg);
  esp_wifi_connect();
}

void onEvent(void*, esp_event_base_t base, int32_t id, void* data) {
  if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
    auto* e = (wifi_event_sta_disconnected_t*)data;
    gotIp = false;
    disconnects++;
    lastReason = e->reason;
    lastDisconnectMs = millis();
    static uint32_t loggedAt = 0;                       // a failing join reports once per attempt; keep the log readable
    if (millis() - loggedAt > 5000) {
      loggedAt = millis();
      log('W', "Wi-Fi lost: %s (code %u)", reasonText(e->reason), (unsigned)e->reason);
    }
    if (!trialActive && !diagScan) esp_wifi_connect();   // keep trying; a trial and a diagnostic scan manage their own
  } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
    gotIp = true;
    wifi_ap_record_t ap = {};
    esp_wifi_sta_get_ap_info(&ap);
    log('I', "Wi-Fi up: '%s' at %d dBm, address %s", (const char*)ap.ssid, (int)ap.rssi, wifi::ip().c_str());
    wifi_hook::networkUp();
  } else if (base == WIFI_EVENT && id == WIFI_EVENT_SCAN_DONE) {
    uint16_t n = MAX_SCAN;
    if (esp_wifi_scan_get_ap_records(&n, scanRecs) == ESP_OK) scanCount = n; else scanCount = 0;
    scanHave = true;
    scanRunning = false;
    if (diagScan) {                                    // a join keeps failing: say what the radio can hear at all
      diagScan = false;
      int best = -1;
      for (int i = 0; i < scanCount; i++) if (best < 0 || scanRecs[i].rssi > scanRecs[best].rssi) best = i;
      for (int i = 0; i < scanCount && i < 40; i++) printf("[scan] %2d '%s' rssi=%d ch=%d auth=%d\n", i, (const char*)scanRecs[i].ssid, (int)scanRecs[i].rssi, (int)scanRecs[i].primary, (int)scanRecs[i].authmode);
      if (best < 0) log('W', "Wi-Fi scan: the radio heard no networks at all (is the antenna connected?)");
      else log('I', "Wi-Fi scan: %u networks heard, strongest '%s' at %d dBm", (unsigned)scanCount, (const char*)scanRecs[best].ssid, (int)scanRecs[best].rssi);
      for (int i = 0; i < scanCount; i++) {
        if (builtin.used && !strcmp((const char*)scanRecs[i].ssid, builtin.ssid)) log('I', "Wi-Fi scan: '%s' heard at %d dBm on channel %d", builtin.ssid, (int)scanRecs[i].rssi, (int)scanRecs[i].primary);
      }
      esp_wifi_connect();
    }
  }
}

bool waitConnected(uint32_t timeoutMs) {
  uint32_t start = millis();
  while (millis() - start < timeoutMs) {
    if (gotIp) return true;
    vTaskDelay(pdMS_TO_TICKS(250));
  }
  return false;
}

void setResult(const char* state, const char* ssid, const char* reason) {
  strlcpy(lastResult.state, state, sizeof(lastResult.state));
  strlcpy(lastResult.ssid, ssid, sizeof(lastResult.ssid));
  strlcpy(lastResult.reason, reason, sizeof(lastResult.reason));
  lastResult.atMs = millis();
}

// ---- A change of network, which undoes itself if the new one does not work ----
struct TrialArgs { char ssid[33]; char pass[65]; char prevSsid[33]; char prevPass[65]; };

void trialTask(void* arg) {
  TrialArgs* a = (TrialArgs*)arg;
  vTaskDelay(pdMS_TO_TICKS(900));                        // the HTTP reply has to leave before the radio moves
  log('I', "Wi-Fi: trying '%s'; the previous network returns if it fails", a->ssid);
  esp_wifi_disconnect();
  vTaskDelay(pdMS_TO_TICKS(300));
  lastReason = 0;
  connectTo(a->ssid, a->pass);
  if (waitConnected(20000)) {
    storeSaved(a->ssid, a->pass);
    activeIdx = 0;
    setResult("ok", a->ssid, "");
    log('I', "Wi-Fi: joined '%s' and saved it", a->ssid);
  } else {
    char why[72];
    snprintf(why, sizeof(why), "%s (code %u)", lastReason ? reasonText(lastReason) : "no answer within 20 s", (unsigned)lastReason);
    log('W', "Wi-Fi: could not join '%s': %s", a->ssid, why);
    setResult("reverted", a->ssid, why);
    if (a->prevSsid[0]) {
      esp_wifi_disconnect();
      vTaskDelay(pdMS_TO_TICKS(300));
      connectTo(a->prevSsid, a->prevPass);
      if (waitConnected(25000)) log('I', "Wi-Fi: back on '%s'", a->prevSsid);
      else log('E', "Wi-Fi: could not return to '%s' either; will keep trying", a->prevSsid);
    }
  }
  trialActive = false;
  free(a);
  vTaskDelete(nullptr);
}

bool printable(const std::string& s) {
  for (unsigned char c : s) if (c < 32 || c > 126) return false;
  return true;
}

// Returns 0 when a change was started, otherwise the HTTP status to answer with.
int apply(const std::string& ssid, const std::string& pass, std::string& error) {
  if (ssid.size() < 1 || ssid.size() > 32 || !printable(ssid)) {
    error = "The network name must be 1 to 32 printable characters";
    return 400;
  }
  bool hex64 = pass.size() == 64;
  for (size_t i = 0; hex64 && i < pass.size(); i++) hex64 = isxdigit((unsigned char)pass[i]);
  if (!pass.empty() && !(pass.size() >= 8 && pass.size() <= 63 && printable(pass)) && !hex64) {
    error = "The password must be 8 to 63 characters, or empty for an open network";
    return 400;
  }
  if (trialActive) { error = "A network change is already in progress"; return 409; }
  if (ota::active()) { error = "A firmware update is running. Change the network after it finishes"; return 409; }
  if (scanRunning) { error = "A scan is running. Try again in a few seconds"; return 409; }
  TrialArgs* a = (TrialArgs*)calloc(1, sizeof(TrialArgs));
  if (!a) { error = "Not enough free memory to change the network"; return 503; }
  strlcpy(a->ssid, ssid.c_str(), sizeof(a->ssid));
  strlcpy(a->pass, pass.c_str(), sizeof(a->pass));
  wifi_config_t cur = {};
  if (gotIp && esp_wifi_get_config(WIFI_IF_STA, &cur) == ESP_OK) {
    strlcpy(a->prevSsid, (const char*)cur.sta.ssid, sizeof(a->prevSsid));
    strlcpy(a->prevPass, (const char*)cur.sta.password, sizeof(a->prevPass));
  } else {
    const Cred* list[2];
    int n = candidates(list);
    if (n) {
      strlcpy(a->prevSsid, list[activeIdx % n]->ssid, sizeof(a->prevSsid));
      strlcpy(a->prevPass, list[activeIdx % n]->pass, sizeof(a->prevPass));
    }
  }
  strlcpy(trialSsid, a->ssid, sizeof(trialSsid));
  trialStartedMs = millis();
  trialActive = true;
  if (xTaskCreate(trialTask, "ros-wifi-try", 6144, a, 4, nullptr) != pdPASS) {
    trialActive = false;
    free(a);
    error = "Not enough free memory to change the network";
    return 503;
  }
  return 0;
}

std::string bssidText(const uint8_t* b) {
  char s[18];
  snprintf(s, sizeof(s), "%02X:%02X:%02X:%02X:%02X:%02X", b[0], b[1], b[2], b[3], b[4], b[5]);
  return s;
}

std::string statusJson() {
  Json j;
  cJSON* root = j.root();
  cJSON_AddNumberToObject(root, "schema", 1);
  bool up = gotIp;
  wifi_ap_record_t ap = {};
  if (up && esp_wifi_sta_get_ap_info(&ap) != ESP_OK) up = false;
  cJSON_AddBoolToObject(root, "connected", up);
  cJSON_AddStringToObject(root, "ssid", up ? (const char*)ap.ssid : "");
  cJSON_AddStringToObject(root, "bssid", up ? bssidText(ap.bssid).c_str() : "");
  cJSON_AddNumberToObject(root, "rssi", up ? ap.rssi : 0);
  cJSON_AddNumberToObject(root, "channel", up ? ap.primary : 0);
  cJSON_AddStringToObject(root, "ip", wifi::ip().c_str());
  cJSON_AddStringToObject(root, "mac", wifi::mac().c_str());
  const char* source = "none";
  if (up) {
    if (saved.used && !strcmp((const char*)ap.ssid, saved.ssid)) source = "saved";
    else if (builtin.used && !strcmp((const char*)ap.ssid, builtin.ssid)) source = "built-in";
    else source = "other";
  }
  cJSON_AddStringToObject(root, "source", source);
  cJSON_AddStringToObject(root, "saved_ssid", saved.used ? saved.ssid : "");
  cJSON_AddStringToObject(root, "builtin_ssid", builtin.used ? builtin.ssid : "");
  cJSON_AddNumberToObject(root, "disconnects", disconnects);
  if (lastDisconnectMs) {
    cJSON* d = cJSON_AddObjectToObject(root, "last_disconnect");
    cJSON_AddNumberToObject(d, "code", lastReason);
    cJSON_AddStringToObject(d, "text", reasonText(lastReason));
    cJSON_AddNumberToObject(d, "ago_s", (millis() - lastDisconnectMs) / 1000);
  } else {
    cJSON_AddNullToObject(root, "last_disconnect");
  }
  cJSON* trial = cJSON_AddObjectToObject(root, "trial");
  cJSON_AddBoolToObject(trial, "active", trialActive);
  cJSON_AddStringToObject(trial, "ssid", trialActive ? trialSsid : "");
  cJSON_AddNumberToObject(trial, "elapsed_s", trialActive ? (millis() - trialStartedMs) / 1000 : 0);
  cJSON* result = cJSON_AddObjectToObject(root, "result");
  cJSON_AddStringToObject(result, "state", lastResult.state);
  cJSON_AddStringToObject(result, "ssid", lastResult.ssid);
  cJSON_AddStringToObject(result, "reason", lastResult.reason);
  cJSON_AddNumberToObject(result, "ago_s", lastResult.atMs ? (millis() - lastResult.atMs) / 1000 : 0);
  return j.print();
}

std::string scanJson() {
  Json j;
  cJSON* root = j.root();
  cJSON_AddNumberToObject(root, "schema", 1);
  cJSON* list = cJSON_AddArrayToObject(root, "networks");
  scanTouched = millis();
  if (scanRunning) {
    cJSON_AddBoolToObject(root, "running", true);
  } else if (!scanHave) {
    cJSON_AddBoolToObject(root, "running", false);
    cJSON_AddBoolToObject(root, "started", false);
  } else {
    cJSON_AddBoolToObject(root, "running", false);
    cJSON_AddBoolToObject(root, "started", true);
    cJSON_AddNumberToObject(root, "total", scanCount);
    int order[MAX_SCAN];
    int count = scanCount < MAX_SCAN ? scanCount : MAX_SCAN;
    for (int i = 0; i < count; i++) order[i] = i;
    for (int i = 1; i < count; i++) {                       // strongest first
      int key = order[i], k = i - 1;
      while (k >= 0 && scanRecs[order[k]].rssi < scanRecs[key].rssi) { order[k + 1] = order[k]; k--; }
      order[k + 1] = key;
    }
    wifi_ap_record_t mine = {};
    bool up = gotIp && esp_wifi_sta_get_ap_info(&mine) == ESP_OK;
    for (int k = 0; k < count && k < 24; k++) {
      const wifi_ap_record_t& r = scanRecs[order[k]];
      cJSON* o = cJSON_CreateObject();
      cJSON_AddStringToObject(o, "ssid", (const char*)r.ssid);
      cJSON_AddStringToObject(o, "bssid", bssidText(r.bssid).c_str());
      cJSON_AddNumberToObject(o, "rssi", r.rssi);
      cJSON_AddNumberToObject(o, "channel", r.primary);
      cJSON_AddStringToObject(o, "auth", authName(r.authmode));
      cJSON_AddBoolToObject(o, "open", r.authmode == WIFI_AUTH_OPEN);
      cJSON_AddBoolToObject(o, "supported", r.authmode != WIFI_AUTH_WPA2_ENTERPRISE);
      cJSON_AddBoolToObject(o, "connected", up && !memcmp(r.bssid, mine.bssid, 6));
      cJSON_AddItemToArray(list, o);
    }
  }
  return j.print();
}

}  // namespace

// ---- Public ----
bool wifiBegin(const char* ssid, const char* password) {
  strlcpy(builtin.ssid, ssid ? ssid : "", sizeof(builtin.ssid));
  strlcpy(builtin.pass, password ? password : "", sizeof(builtin.pass));
  builtin.used = builtin.ssid[0] != 0;
  loadSaved();
  const Cred* list[2];
  int n = candidates(list);
  if (!n) return false;

  ESP_ERROR_CHECK(esp_netif_init());
  esp_err_t e = esp_event_loop_create_default();
  if (e != ESP_OK && e != ESP_ERR_INVALID_STATE) ESP_ERROR_CHECK(e);
  staNetif = esp_netif_create_default_wifi_sta();
  wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
  ESP_ERROR_CHECK(esp_wifi_init(&init));
  ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, onEvent, nullptr));
  ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, onEvent, nullptr));
  ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
  ESP_ERROR_CHECK(esp_wifi_start());
  // Modem sleep (the default) wakes the radio only at the access point's beacons. On a weak link that made replies late
  // or lost (ping median 118 ms with 10% loss against 11 ms without it, measured on the ESP32). Mains power is on hand.
  esp_wifi_set_ps(WIFI_PS_NONE);
  activeIdx = 0;
  log('I', "Wi-Fi: joining the %s network '%s'", list[0] == &saved ? "saved" : "built-in", list[0]->ssid);
  connectTo(list[0]->ssid, list[0]->pass);
  started = true;
  return true;
}

namespace wifi {

bool changing() { return trialActive; }
bool up() { return gotIp; }

std::string ssid() {
  wifi_ap_record_t ap = {};
  return gotIp && esp_wifi_sta_get_ap_info(&ap) == ESP_OK ? (const char*)ap.ssid : "";
}
int rssi() {
  wifi_ap_record_t ap = {};
  return gotIp && esp_wifi_sta_get_ap_info(&ap) == ESP_OK ? ap.rssi : 0;
}
int channel() {
  wifi_ap_record_t ap = {};
  return gotIp && esp_wifi_sta_get_ap_info(&ap) == ESP_OK ? ap.primary : 0;
}
std::string ip() {
  esp_netif_ip_info_t info = {};
  if (!staNetif || esp_netif_get_ip_info(staNetif, &info) != ESP_OK) return "0.0.0.0";
  char s[16];
  snprintf(s, sizeof(s), IPSTR, IP2STR(&info.ip));
  return s;
}
std::string mac() {
  uint8_t m[6] = {0};
  esp_wifi_get_mac(WIFI_IF_STA, m);
  char s[18];
  snprintf(s, sizeof(s), "%02X:%02X:%02X:%02X:%02X:%02X", m[0], m[1], m[2], m[3], m[4], m[5]);
  return s;
}

// If the network in use stays down, try the other one.
void watch() {
  static uint32_t changedAt = millis();
  if (!started) return;
  if (trialActive || ota::active()) { changedAt = millis(); return; }
  if (gotIp) {
    changedAt = millis();
  } else {
    const Cred* list[2];
    int n = candidates(list);
    if (n > 1 && millis() - changedAt > 30000) {
      activeIdx = (activeIdx + 1) % n;
      const Cred* next = list[activeIdx];
      log('W', "Wi-Fi: no connection for 30 s, trying the %s network '%s'", next == &saved ? "saved" : "built-in", next->ssid);
      esp_wifi_disconnect();
      vTaskDelay(pdMS_TO_TICKS(200));
      connectTo(next->ssid, next->pass);
      changedAt = millis();
    }
  }
  static uint32_t diagAt = 0;
  if (!gotIp && !trialActive && !scanRunning && millis() > 15000 && (diagAt == 0 || millis() - diagAt > 60000)) {
    diagAt = millis();
    diagScan = true;                                   // the event handler stops reconnecting while this runs
    log('I', "Wi-Fi: pausing to scan for what the radio can hear");
    esp_err_t de = esp_wifi_disconnect();
    log('I', "Wi-Fi: disconnect returned %s", esp_err_to_name(de));
    vTaskDelay(pdMS_TO_TICKS(500));
    wifi_scan_config_t sc = {};
    esp_err_t se = esp_wifi_scan_start(&sc, false);
    log('I', "Wi-Fi: diagnostic scan started (%s)", esp_err_to_name(se));
    if (se == ESP_OK) scanRunning = true;
    else { diagScan = false; esp_wifi_connect(); }
  }
  if (scanTouched && millis() - scanTouched > 120000) {      // do not hold scan results for ever
    scanHave = false;
    scanTouched = 0;
  }
}

void attach(httpd_handle_t server) {
  static const httpd_uri_t get = {"/api/wifi", HTTP_GET, [](httpd_req_t* r) { return authorized(r) ? reply(r, 200, statusJson()) : ESP_OK; }, nullptr};
  static const httpd_uri_t put = {"/api/wifi", HTTP_PUT, [](httpd_req_t* r) {
    if (!authorized(r)) return ESP_OK;
    std::string body;
    if (!internal::readBody(r, 512, body)) return ESP_FAIL;
    cJSON* doc = cJSON_Parse(body.c_str());
    cJSON* s = doc ? cJSON_GetObjectItem(doc, "ssid") : nullptr;
    cJSON* p = doc ? cJSON_GetObjectItem(doc, "password") : nullptr;
    if (!doc || !cJSON_IsString(s)) {
      cJSON_Delete(doc);
      return fail(r, 400, "Send JSON like {\"ssid\":\"name\",\"password\":\"secret\"}");
    }
    std::string ssid = s->valuestring, pass = cJSON_IsString(p) ? p->valuestring : "", error;
    cJSON_Delete(doc);
    int code = apply(ssid, pass, error);
    if (code) return fail(r, code, error);
    return reply(r, 200, "{\"ok\":true,\"state\":\"trying\",\"revert_after_s\":20}");
  }, nullptr};
  static const httpd_uri_t scanStart = {"/api/wifi/scan", HTTP_POST, [](httpd_req_t* r) {
    if (!authorized(r)) return ESP_OK;
    if (trialActive) return fail(r, 409, "A network change is in progress");
    if (ota::active()) return fail(r, 409, "A firmware update is running");
    if (!scanRunning) {
      wifi_scan_config_t cfg = {};
      scanHave = false;
      if (esp_wifi_scan_start(&cfg, false) != ESP_OK) return fail(r, 500, "The radio could not start a scan");
      scanRunning = true;
      log('I', "Wi-Fi: scanning for networks");
    }
    scanTouched = millis();
    return reply(r, 200, "{\"ok\":true,\"running\":true}");
  }, nullptr};
  static const httpd_uri_t scanGet = {"/api/wifi/scan", HTTP_GET, [](httpd_req_t* r) { return authorized(r) ? reply(r, 200, scanJson()) : ESP_OK; }, nullptr};
  httpd_register_uri_handler(server, &get);
  httpd_register_uri_handler(server, &put);
  httpd_register_uri_handler(server, &scanStart);
  httpd_register_uri_handler(server, &scanGet);
}

}  // namespace wifi
}  // namespace routeos
