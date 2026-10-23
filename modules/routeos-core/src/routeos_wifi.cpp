#include "routeos_wifi.h"
#include "routeos_internal.h"
#include "routeos_log.h"
#include "routeos_ota.h"
#include <ArduinoJson.h>
#include <Preferences.h>
#include <WiFi.h>

namespace routeos {

namespace {

// ---- Networks the device may join ----
struct Cred {
  char ssid[33];
  char pass[65];
  bool used;
};
Cred builtin = {"", "", false};     // compiled into the firmware
Cred saved = {"", "", false};       // set through the API, kept in flash
uint8_t activeIdx = 0;               // which candidate the supervisor is currently using
bool started = false;

// ---- A requested change of network, which undoes itself if the new one does not work ----
volatile bool trialActive = false;
char trialSsid[33] = "";
uint32_t trialStartedMs = 0;
struct Result {
  char state[10];                    // none, ok, reverted
  char ssid[33];
  char reason[72];
  uint32_t atMs;
};
Result lastResult = {"none", "", "", 0};

// ---- Why the radio let go, for diagnosing a weak link ----
uint32_t disconnects = 0;
uint8_t lastReason = 0;
uint32_t lastDisconnectMs = 0;
uint32_t scanTouched = 0;

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

// ---- Saved network ----
void loadSaved() {
  Preferences p;
  p.begin("roswifi", true);
  String ssid = p.getString("ssid", "");
  String pass = p.getString("pass", "");
  p.end();
  if (ssid.length() >= 1 && ssid.length() <= 32 && pass.length() <= 64) {
    strlcpy(saved.ssid, ssid.c_str(), sizeof(saved.ssid));
    strlcpy(saved.pass, pass.c_str(), sizeof(saved.pass));
    saved.used = true;
  }
}

void storeSaved(const char* ssid, const char* pass) {
  Preferences p;
  p.begin("roswifi", false);
  p.putString("ssid", ssid);
  p.putString("pass", pass);
  p.end();
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

void onWifiEvent(arduino_event_id_t event, arduino_event_info_t info) {
  if (event == ARDUINO_EVENT_WIFI_STA_DISCONNECTED) {
    uint8_t reason = info.wifi_sta_disconnected.reason;
    disconnects++;
    lastReason = reason;
    lastDisconnectMs = millis();
    // A failing join reports once per attempt; keep the log readable.
    static uint32_t loggedAt = 0;
    if (millis() - loggedAt > 5000) {
      loggedAt = millis();
      log('W', "Wi-Fi lost: %s (code %u)", reasonText(reason), (unsigned)reason);
    }
  } else if (event == ARDUINO_EVENT_WIFI_STA_GOT_IP) {
    log('I', "Wi-Fi up: '%s' at %d dBm, address %s", WiFi.SSID().c_str(), (int)WiFi.RSSI(), WiFi.localIP().toString().c_str());
  }
}

bool waitConnected(uint32_t timeoutMs) {
  uint32_t start = millis();
  while (millis() - start < timeoutMs) {
    if (WiFi.status() == WL_CONNECTED && WiFi.localIP() != IPAddress(0, 0, 0, 0)) return true;
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

// ---- Supervisor: if the network in use stays down, try the other one ----
void supervise() {
  static uint32_t changedAt = millis();
  if (!started) return;                 // wifiBegin() has not run
  if (trialActive || ota::active()) { changedAt = millis(); return; }
  if (WiFi.status() == WL_CONNECTED) {
    changedAt = millis();
  } else {
    const Cred* list[2];
    int n = candidates(list);
    if (n > 1 && millis() - changedAt > 30000) {
      activeIdx = (activeIdx + 1) % n;
      const Cred* next = list[activeIdx];
      log('W', "Wi-Fi: no connection for 30 s, trying the %s network '%s'", next == &saved ? "saved" : "built-in", next->ssid);
      WiFi.disconnect(false, false);
      vTaskDelay(pdMS_TO_TICKS(200));
      WiFi.begin(next->ssid, next->pass);
      changedAt = millis();
    }
  }
  if (scanTouched && millis() - scanTouched > 120000) {      // do not hold scan results for ever
    if (WiFi.scanComplete() >= 0) WiFi.scanDelete();
    scanTouched = 0;
  }
}

// ---- A change of network ----
struct TrialArgs {
  char ssid[33];
  char pass[65];
  char prevSsid[33];
  char prevPass[65];
};

void trialTask(void* arg) {
  TrialArgs* a = (TrialArgs*)arg;
  vTaskDelay(pdMS_TO_TICKS(900));                 // the HTTP reply has to leave before the radio moves
  log('I', "Wi-Fi: trying '%s'; the previous network returns if it fails", a->ssid);
  WiFi.disconnect(false, false);
  vTaskDelay(pdMS_TO_TICKS(300));
  lastReason = 0;
  WiFi.begin(a->ssid, a->pass);
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
      WiFi.disconnect(false, false);
      vTaskDelay(pdMS_TO_TICKS(300));
      WiFi.begin(a->prevSsid, a->prevPass);
      if (waitConnected(25000)) log('I', "Wi-Fi: back on '%s'", a->prevSsid);
      else log('E', "Wi-Fi: could not return to '%s' either; will keep trying", a->prevSsid);
    }
  }
  trialActive = false;
  free(a);
  vTaskDelete(nullptr);
}

bool printable(const String& s) {
  for (size_t i = 0; i < s.length(); i++) {
    if ((uint8_t)s[i] < 32 || (uint8_t)s[i] > 126) return false;
  }
  return true;
}

// Returns 0 when a change was started, otherwise the HTTP status to answer with.
int apply(const String& ssid, const String& pass, String& error) {
  if (ssid.length() < 1 || ssid.length() > 32 || !printable(ssid)) {
    error = "The network name must be 1 to 32 printable characters";
    return 400;
  }
  bool hex64 = pass.length() == 64;
  for (size_t i = 0; hex64 && i < pass.length(); i++) hex64 = isxdigit((unsigned char)pass[i]);
  if (pass.length() && !(pass.length() >= 8 && pass.length() <= 63 && printable(pass)) && !hex64) {
    error = "The password must be 8 to 63 characters, or empty for an open network";
    return 400;
  }
  if (trialActive) { error = "A network change is already in progress"; return 409; }
  if (ota::active()) { error = "A firmware update is running. Change the network after it finishes"; return 409; }
  if (WiFi.scanComplete() == WIFI_SCAN_RUNNING) { error = "A scan is running. Try again in a few seconds"; return 409; }
  TrialArgs* a = (TrialArgs*)calloc(1, sizeof(TrialArgs));
  if (!a) { error = "Not enough free memory to change the network"; return 503; }
  strlcpy(a->ssid, ssid.c_str(), sizeof(a->ssid));
  strlcpy(a->pass, pass.c_str(), sizeof(a->pass));
  if (WiFi.status() == WL_CONNECTED) {
    strlcpy(a->prevSsid, WiFi.SSID().c_str(), sizeof(a->prevSsid));
    strlcpy(a->prevPass, WiFi.psk().c_str(), sizeof(a->prevPass));
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
  if (xTaskCreate(trialTask, "ros-wifi-try", 4096, a, 1, nullptr) != pdPASS) {
    trialActive = false;
    free(a);
    error = "Not enough free memory to change the network";
    return 503;
  }
  return 0;
}

// ---- JSON ----
String statusJson() {
  JsonDocument doc;
  doc["schema"] = 1;
  bool up = WiFi.status() == WL_CONNECTED;
  doc["connected"] = up;
  doc["ssid"] = up ? WiFi.SSID() : String("");
  doc["bssid"] = up ? WiFi.BSSIDstr() : String("");
  doc["rssi"] = up ? (int)WiFi.RSSI() : 0;
  doc["channel"] = up ? (int)WiFi.channel() : 0;
  doc["ip"] = WiFi.localIP().toString();
  doc["mac"] = WiFi.macAddress();
  const char* source = "none";
  if (up) {
    String now = WiFi.SSID();
    if (saved.used && now == saved.ssid) source = "saved";
    else if (builtin.used && now == builtin.ssid) source = "built-in";
    else source = "other";
  }
  doc["source"] = source;
  doc["saved_ssid"] = String(saved.used ? saved.ssid : "");
  doc["builtin_ssid"] = String(builtin.used ? builtin.ssid : "");
  doc["disconnects"] = disconnects;
  if (lastDisconnectMs) {
    JsonObject d = doc["last_disconnect"].to<JsonObject>();
    d["code"] = lastReason;
    d["text"] = String(reasonText(lastReason));
    d["ago_s"] = (millis() - lastDisconnectMs) / 1000;
  } else {
    doc["last_disconnect"] = nullptr;
  }
  JsonObject trial = doc["trial"].to<JsonObject>();
  trial["active"] = (bool)trialActive;
  trial["ssid"] = String(trialActive ? trialSsid : "");
  trial["elapsed_s"] = trialActive ? (millis() - trialStartedMs) / 1000 : 0;
  JsonObject result = doc["result"].to<JsonObject>();
  result["state"] = String(lastResult.state);
  result["ssid"] = String(lastResult.ssid);
  result["reason"] = String(lastResult.reason);
  result["ago_s"] = lastResult.atMs ? (millis() - lastResult.atMs) / 1000 : 0;
  String out;
  serializeJson(doc, out);
  return out;
}

String scanJson() {
  JsonDocument doc;
  doc["schema"] = 1;
  JsonArray list = doc["networks"].to<JsonArray>();
  int n = WiFi.scanComplete();
  scanTouched = millis();
  if (n == WIFI_SCAN_RUNNING) {
    doc["running"] = true;
  } else if (n < 0) {
    doc["running"] = false;
    doc["started"] = false;
  } else {
    doc["running"] = false;
    doc["started"] = true;
    doc["total"] = n;
    int order[40];
    int count = n < 40 ? n : 40;
    for (int i = 0; i < count; i++) order[i] = i;
    for (int i = 1; i < count; i++) {                       // strongest first
      int key = order[i], j = i - 1;
      while (j >= 0 && WiFi.RSSI(order[j]) < WiFi.RSSI(key)) { order[j + 1] = order[j]; j--; }
      order[j + 1] = key;
    }
    bool up = WiFi.status() == WL_CONNECTED;
    String mine = up ? WiFi.BSSIDstr() : String("");
    for (int k = 0; k < count && k < 24; k++) {
      int i = order[k];
      wifi_auth_mode_t mode = WiFi.encryptionType(i);
      JsonObject o = list.add<JsonObject>();
      o["ssid"] = WiFi.SSID(i);
      o["bssid"] = WiFi.BSSIDstr(i);
      o["rssi"] = (int)WiFi.RSSI(i);
      o["channel"] = (int)WiFi.channel(i);
      o["auth"] = String(authName(mode));
      o["open"] = mode == WIFI_AUTH_OPEN;
      o["supported"] = mode != WIFI_AUTH_WPA2_ENTERPRISE;
      o["connected"] = up && WiFi.BSSIDstr(i) == mine;
    }
  }
  String out;
  serializeJson(doc, out);
  return out;
}

}  // namespace

bool wifiBegin(const char* ssid, const char* password) {
  strlcpy(builtin.ssid, ssid ? ssid : "", sizeof(builtin.ssid));
  strlcpy(builtin.pass, password ? password : "", sizeof(builtin.pass));
  builtin.used = builtin.ssid[0] != 0;
  loadSaved();
  const Cred* list[2];
  int n = candidates(list);
  if (!n) return false;
  WiFi.persistent(false);
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  // Join the strongest access point of a network, not the first one the radio hears.
  WiFi.setScanMethod(WIFI_ALL_CHANNEL_SCAN);
  WiFi.setSortMethod(WIFI_CONNECT_AP_BY_SIGNAL);
  WiFi.onEvent(onWifiEvent);
  activeIdx = 0;
  log('I', "Wi-Fi: joining the %s network '%s'", list[0] == &saved ? "saved" : "built-in", list[0]->ssid);
  WiFi.begin(list[0]->ssid, list[0]->pass);
  started = true;                       // from now on the Core's housekeeping task supervises the connection
  return true;
}

namespace wifi {

bool changing() { return trialActive; }

void watch() { supervise(); }

void attach(WebServer& http) {
  http.on("/api/wifi", HTTP_GET, [&http]() {
    if (internal::authorized(http)) internal::reply(http, 200, statusJson());
  });
  http.on("/api/wifi", HTTP_PUT, [&http]() {
    if (!internal::authorized(http)) return;
    JsonDocument doc;
    if (deserializeJson(doc, http.arg("plain")) || !doc.is<JsonObject>()) {
      internal::fail(http, 400, "Send JSON like {\"ssid\":\"name\",\"password\":\"secret\"}");
      return;
    }
    String ssid = doc["ssid"] | "";
    String pass = doc["password"] | "";
    String error;
    int code = apply(ssid, pass, error);
    if (code) {
      internal::fail(http, code, error);
      return;
    }
    internal::reply(http, 200, "{\"ok\":true,\"state\":\"trying\",\"revert_after_s\":20}");
  });
  http.on("/api/wifi/scan", HTTP_POST, [&http]() {
    if (!internal::authorized(http)) return;
    if (trialActive) { internal::fail(http, 409, "A network change is in progress"); return; }
    if (ota::active()) { internal::fail(http, 409, "A firmware update is running"); return; }
    if (WiFi.scanComplete() != WIFI_SCAN_RUNNING) {
      WiFi.scanDelete();
      if (WiFi.scanNetworks(true, false) == WIFI_SCAN_FAILED) {
        internal::fail(http, 500, "The radio could not start a scan");
        return;
      }
      log('I', "Wi-Fi: scanning for networks");
    }
    scanTouched = millis();
    internal::reply(http, 200, "{\"ok\":true,\"running\":true}");
  });
  http.on("/api/wifi/scan", HTTP_GET, [&http]() {
    if (internal::authorized(http)) internal::reply(http, 200, scanJson());
  });
}

}  // namespace wifi
}  // namespace routeos
