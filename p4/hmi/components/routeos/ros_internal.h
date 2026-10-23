#pragma once
// Shared by the Core's own source files. Not part of the public API.
#include <string>
#include "esp_http_server.h"
#include "nvs.h"
#include "routeos.h"
#include "cJSON.h"

namespace routeos {

// A JSON object being built; print() gives the text and frees everything.
class Json {
 public:
  Json() : root_(cJSON_CreateObject()) {}
  explicit Json(cJSON* adopt) : root_(adopt) {}
  ~Json() { cJSON_Delete(root_); }
  Json(const Json&) = delete;
  cJSON* root() { return root_; }
  std::string print() {
    char* s = cJSON_PrintUnformatted(root_);
    std::string out = s ? s : "{}";
    cJSON_free(s);
    return out;
  }
 private:
  cJSON* root_;
};

// One NVS namespace, opened for the life of the object.
class Nvs {
 public:
  Nvs(const char* ns, bool write) { ok_ = nvs_open(ns, write ? NVS_READWRITE : NVS_READONLY, &h_) == ESP_OK; }
  ~Nvs() { if (ok_) nvs_close(h_); }
  std::string str(const char* key, const char* def = "") const {
    if (!ok_) return def;
    size_t n = 0;
    if (nvs_get_str(h_, key, nullptr, &n) != ESP_OK || n == 0) return def;
    std::string s(n, '\0');
    if (nvs_get_str(h_, key, &s[0], &n) != ESP_OK) return def;
    s.resize(n ? n - 1 : 0);
    return s;
  }
  void put(const char* key, const std::string& v) { if (ok_) { nvs_set_str(h_, key, v.c_str()); nvs_commit(h_); } }
  bool flag(const char* key, bool def = false) const { uint8_t v = def; if (ok_) nvs_get_u8(h_, key, &v); return v != 0; }
  void putFlag(const char* key, bool v) { if (ok_) { nvs_set_u8(h_, key, v ? 1 : 0); nvs_commit(h_); } }
  uint8_t u8(const char* key, uint8_t def = 0) const { uint8_t v = def; if (ok_) nvs_get_u8(h_, key, &v); return v; }
  void putU8(const char* key, uint8_t v) { if (ok_) { nvs_set_u8(h_, key, v); nvs_commit(h_); } }
  void erase(const char* key) { if (ok_) { nvs_erase_key(h_, key); nvs_commit(h_); } }
 private:
  nvs_handle_t h_ = 0;
  bool ok_ = false;
};

namespace internal {
// The request body, at most `max` bytes. Replies 400/413 itself and returns false on failure.
bool readBody(httpd_req_t* req, size_t max, std::string& out);
bool tokenValid(httpd_req_t* req);
const char* firmwareName();
const char* firmwareVersion();
const char* resetReasonText();
void rebootSoon();
}  // namespace internal

namespace wifi {
void attach(httpd_handle_t server);
void watch();                     // called twice a second by the housekeeping task
bool changing();
bool up();
std::string ssid();
int rssi();
int channel();
std::string ip();
std::string mac();
}  // namespace wifi

namespace ota {
void begin();                     // boot-time guard
void networkUp();
bool active();
void attach(httpd_handle_t server);
}  // namespace ota

}  // namespace routeos
