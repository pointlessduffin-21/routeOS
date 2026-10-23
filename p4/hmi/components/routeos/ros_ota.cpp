// Over-the-air firmware updates with a safety net, for ESP-IDF. Same protocol and same guard as the Arduino Core:
// the running firmware is untouched until a whole image has arrived, carries the routeOS marker and passes the
// IDF integrity check. A new image is on trial: it must reach the network and stay up for 30 s, or the previous
// firmware returns by itself (three failed starts, or no network within two minutes).
#include <string.h>
#include "esp_app_format.h"
#include "esp_heap_caps.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "ros_internal.h"

extern "C" const char routeos_image_tag[];

namespace routeos {
namespace ota {

namespace {

constexpr size_t CHUNK_MAX = 32768;             // what a resend costs if one piece is lost
constexpr uint32_t CONFIRM_AFTER_MS = 30000, GIVE_UP_AFTER_MS = 120000, QUIET_ABANDON_MS = 60000;
constexpr uint8_t MAX_STARTS = 3;

struct Tag { bool found, guard; char name[40]; char version[24]; char core[12]; };
struct Rx {
  bool active;
  size_t total, received;
  uint32_t lastActivity;
  char error[100];
  Tag tag;
  uint8_t match;
  bool collecting;
  uint8_t len;
  char buf[96];
};
Rx rx;
esp_ota_handle_t handle = 0;
const esp_partition_t* target = nullptr;
bool networkIsUp = false, guarding = false;

const char PREFIX[] = "ROUTEOS-IMAGE|1|";
constexpr uint8_t PREFIX_LEN = sizeof(PREFIX) - 1;

void parseTag() {
  // name|version|core=1.1.1|ota=1|END
  char* fields[5];
  int n = 0;
  char* p = rx.buf;
  while (n < 5) {
    fields[n++] = p;
    char* bar = strchr(p, '|');
    if (!bar) break;
    *bar = 0;
    p = bar + 1;
  }
  if (n < 5 || strcmp(fields[4], "END") || strncmp(fields[2], "core=", 5) || strncmp(fields[3], "ota=", 4)) return;
  if (!fields[0][0] || !fields[1][0]) return;
  strlcpy(rx.tag.name, fields[0], sizeof(rx.tag.name));
  strlcpy(rx.tag.version, fields[1], sizeof(rx.tag.version));
  strlcpy(rx.tag.core, fields[2] + 5, sizeof(rx.tag.core));
  rx.tag.guard = fields[3][4] == '1';
  rx.tag.found = true;
}

// The marker is found while the bytes stream past, so the whole image never has to be held.
void scan(const uint8_t* data, size_t n) {
  for (size_t i = 0; i < n; i++) {
    uint8_t c = data[i];
    if (rx.collecting) {
      if (c == 0 || rx.len >= sizeof(rx.buf) - 1) {
        rx.buf[rx.len] = 0;
        parseTag();
        rx.collecting = false;
        rx.match = 0;
      } else {
        rx.buf[rx.len++] = c;
      }
      continue;
    }
    if (rx.tag.found) continue;
    if (c == (uint8_t)PREFIX[rx.match]) {
      if (++rx.match == PREFIX_LEN) { rx.collecting = true; rx.len = 0; }
    } else {
      rx.match = c == (uint8_t)PREFIX[0] ? 1 : 0;
    }
  }
}

// Size of the image stored in a slot, from its headers (cached for the running one).
size_t imageSize(const esp_partition_t* part) {
  if (!part) return 0;
  esp_image_header_t head;
  if (esp_partition_read(part, 0, &head, sizeof(head)) != ESP_OK || head.magic != ESP_IMAGE_HEADER_MAGIC) return 0;
  size_t off = sizeof(head);
  for (int i = 0; i < head.segment_count; i++) {
    esp_image_segment_header_t seg;
    if (esp_partition_read(part, off, &seg, sizeof(seg)) != ESP_OK) return 0;
    off += sizeof(seg) + seg.data_len;
  }
  off = (off + 1 + 15) & ~(size_t)15;             // checksum byte, padded to 16
  if (head.hash_appended) off += 32;
  return off;
}

void noteSwitch(Nvs& p, const char* kind, const std::string& text) {
  const esp_partition_t* running = esp_ota_get_running_partition();
  p.put("prev", running ? running->label : "");
  p.put("prev_ver", internal::firmwareVersion());
  p.put("prev_name", internal::firmwareName());
  p.putFlag("other_ok", true);
  p.put("last_kind", kind);
  p.put("last_text", text);
}

void abortReceiving(const char* why) {
  if (handle) { esp_ota_abort(handle); handle = 0; }
  rx.active = false;
  strlcpy(rx.error, why, sizeof(rx.error));
  log('W', "firmware update abandoned: %s", why);
}

// Boot the slot we came from. Returns only when that is impossible.
bool rollbackNow(const char* reason) {
  Nvs p("rosota", true);
  std::string label = p.str("prev");
  const esp_partition_t* back = label.empty() ? nullptr : esp_partition_find_first(ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_ANY, label.c_str());
  esp_app_desc_t desc;
  bool ok = back && esp_ota_get_partition_description(back, &desc) == ESP_OK && esp_ota_set_boot_partition(back) == ESP_OK;
  p.putFlag("pending", false);
  p.putU8("boots", 0);
  if (!ok) {
    p.put("last_kind", "failed");
    p.put("last_text", std::string("The new firmware did not start properly, and the previous firmware could not be restored: ") + reason);
    log('E', "cannot roll back (%s): the previous firmware is not usable", reason);
    return false;
  }
  noteSwitch(p, "rolled-back", std::string("Rolled back from ") + internal::firmwareVersion() + " because " + reason);
  log('E', "rolling back to the previous firmware because %s", reason);
  vTaskDelay(pdMS_TO_TICKS(400));
  esp_restart();
  return true;
}

void guardTask(void*) {
  for (;;) {
    vTaskDelay(pdMS_TO_TICKS(500));
    uint32_t up = millis();
    if (networkIsUp && up >= CONFIRM_AFTER_MS) {
      Nvs p("rosota", true);
      p.putFlag("pending", false);
      p.putU8("boots", 0);
      p.put("last_kind", "confirmed");
      p.put("last_text", std::string("Firmware ") + internal::firmwareVersion() + " started cleanly and is confirmed");
      guarding = false;
      log('I', "new firmware confirmed: up on the network for 30 s");
      vTaskDelete(nullptr);
    }
    if (up >= GIVE_UP_AFTER_MS) {
      rollbackNow("it did not reach the network within 2 minutes");
      guarding = false;
      vTaskDelete(nullptr);
    }
  }
}

std::string infoJson() {
  Json j;
  cJSON* root = j.root();
  cJSON_AddNumberToObject(root, "schema", 1);
  cJSON_AddNumberToObject(root, "boot_id", bootId());          // changes on every start: how a console knows it restarted
  cJSON_AddNumberToObject(root, "uptime_s", millis() / 1000);
  const esp_partition_t* running = esp_ota_get_running_partition();
  const esp_partition_t* other = esp_ota_get_next_update_partition(nullptr);
  Nvs p("rosota", false);

  static size_t runningImage = 0;
  if (!runningImage) runningImage = imageSize(running);
  cJSON* run = cJSON_AddObjectToObject(root, "running");
  cJSON_AddStringToObject(run, "label", running ? running->label : "");
  cJSON_AddNumberToObject(run, "slot_kb", running ? running->size / 1024 : 0);
  cJSON_AddNumberToObject(run, "image_kb", runningImage / 1024);
  cJSON_AddStringToObject(run, "name", internal::firmwareName());
  cJSON_AddStringToObject(run, "version", internal::firmwareVersion());
  cJSON_AddStringToObject(run, "tag", routeos_image_tag);

  esp_app_desc_t desc;
  bool present = other && esp_ota_get_partition_description(other, &desc) == ESP_OK;
  bool known = present && p.flag("other_ok") && p.str("prev") == other->label;
  cJSON* oth = cJSON_AddObjectToObject(root, "other");
  cJSON_AddStringToObject(oth, "label", other ? other->label : "");
  cJSON_AddNumberToObject(oth, "slot_kb", other ? other->size / 1024 : 0);
  cJSON_AddBoolToObject(oth, "present", present);
  cJSON_AddBoolToObject(oth, "verified", known);
  cJSON_AddStringToObject(oth, "version", known ? p.str("prev_ver").c_str() : "");
  cJSON_AddStringToObject(oth, "name", known ? p.str("prev_name").c_str() : "");

  cJSON* up = cJSON_AddObjectToObject(root, "update");
  cJSON_AddBoolToObject(up, "active", rx.active);
  cJSON_AddNumberToObject(up, "received", rx.received);
  cJSON_AddNumberToObject(up, "total", rx.total);
  cJSON_AddStringToObject(up, "error", rx.error);
  cJSON_AddNumberToObject(up, "chunk_max", CHUNK_MAX);

  bool pending = p.flag("pending");
  cJSON* g = cJSON_AddObjectToObject(root, "guard");
  cJSON_AddBoolToObject(g, "pending", pending);
  cJSON_AddBoolToObject(g, "watching", guarding);
  cJSON_AddNumberToObject(g, "starts", p.u8("boots"));
  cJSON_AddNumberToObject(g, "max_starts", MAX_STARTS);
  cJSON_AddNumberToObject(g, "confirm_in_s", pending && millis() < CONFIRM_AFTER_MS ? (CONFIRM_AFTER_MS - millis()) / 1000 : 0);

  cJSON* last = cJSON_AddObjectToObject(root, "last");
  cJSON_AddStringToObject(last, "kind", p.str("last_kind", "none").c_str());
  cJSON_AddStringToObject(last, "text", p.str("last_text").c_str());
  return j.print();
}

esp_err_t handleBegin(httpd_req_t* r) {
  if (!authorized(r)) return ESP_OK;
  std::string body;
  if (!internal::readBody(r, 256, body)) return ESP_FAIL;
  cJSON* doc = cJSON_Parse(body.c_str());
  cJSON* sz = doc ? cJSON_GetObjectItem(doc, "size") : nullptr;
  if (!cJSON_IsNumber(sz)) { cJSON_Delete(doc); return fail(r, 400, "Send JSON like {\"size\":1048576}"); }
  size_t size = (size_t)sz->valuedouble;
  cJSON_Delete(doc);
  if (rx.active) {
    if (millis() - rx.lastActivity < QUIET_ABANDON_MS) return fail(r, 409, "An update is already in progress. Abort it, or let it finish");
    abortReceiving("the previous upload went quiet");
  }
  const esp_partition_t* next = esp_ota_get_next_update_partition(nullptr);
  if (!next) return fail(r, 500, "This device has no second firmware slot, so it cannot update itself");
  if (size < 65536) return fail(r, 400, "That is too small to be a firmware image");
  if (size > next->size) {
    return fail(r, 413, "The image is " + std::to_string(size / 1024) + " KB but the update slot holds " + std::to_string(next->size / 1024) + " KB");
  }
  if (esp_ota_begin(next, OTA_WITH_SEQUENTIAL_WRITES, &handle) != ESP_OK) { handle = 0; return fail(r, 500, "Could not start the update"); }
  target = next;
  memset(&rx, 0, sizeof(rx));
  rx.active = true;
  rx.total = size;
  rx.lastActivity = millis();
  Nvs("rosota", true).putFlag("other_ok", false);                // the slot about to be overwritten held the way back
  log('I', "firmware update started: %u KB into %s", (unsigned)(size / 1024), next->label);
  return reply(r, 200, "{\"ok\":true,\"received\":0,\"chunk_max\":" + std::to_string(CHUNK_MAX) + "}");
}

// One piece of the image, multipart/form-data with the bytes in a file part and the offset in X-Ota-Offset.
esp_err_t handleChunk(httpd_req_t* r) {
  if (!authorized(r)) return ESP_OK;
  if (!rx.active) return fail(r, 409, "No update in progress. Start one first");
  if (r->content_len == 0 || r->content_len > CHUNK_MAX + 2048) return fail(r, 413, "That piece is too large");
  char type[200] = {0}, offHeader[24] = {0};
  httpd_req_get_hdr_value_str(r, "Content-Type", type, sizeof(type));
  httpd_req_get_hdr_value_str(r, "X-Ota-Offset", offHeader, sizeof(offHeader));
  const char* b = strstr(type, "boundary=");
  if (!b) return fail(r, 400, "Send each piece as multipart/form-data with the bytes in a file part");
  std::string delimiter = std::string("\r\n--") + (b + 9);
  if (delimiter.size() > 100) return fail(r, 400, "That multipart boundary is too long");

  uint8_t* body = (uint8_t*)heap_caps_malloc(r->content_len, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!body) return fail(r, 503, "Not enough free memory to receive a piece right now");
  size_t got = 0;
  while (got < r->content_len) {
    int n = httpd_req_recv(r, (char*)body + got, r->content_len - got);
    if (n == HTTPD_SOCK_ERR_TIMEOUT) continue;
    if (n <= 0) {                                                // the link died mid-piece; nothing was written
      heap_caps_free(body);
      log('W', "an upload piece was cut short at %u of %u bytes", (unsigned)got, (unsigned)r->content_len);
      return ESP_FAIL;
    }
    got += n;
  }
  rx.lastActivity = millis();
  const uint8_t* data = nullptr;
  size_t len = 0;
  for (size_t i = 0; i + 4 <= got; i++) {                        // the part's headers end at the first blank line
    if (!memcmp(body + i, "\r\n\r\n", 4)) { data = body + i + 4; break; }
  }
  if (data) {
    const uint8_t* end = nullptr;
    for (const uint8_t* p = body + got - delimiter.size(); p >= data; p--) {
      if (!memcmp(p, delimiter.data(), delimiter.size())) { end = p; break; }
    }
    if (end) len = end - data;
  }
  esp_err_t result;
  if (!data || len == 0) {
    result = fail(r, 400, "Send each piece as multipart/form-data with the bytes in a file part");
  } else if (!offHeader[0] || (size_t)strtoul(offHeader, nullptr, 10) != rx.received) {
    result = fail(r, 409, "The device holds " + std::to_string(rx.received) + " bytes, not " + offHeader + ". Continue from there (received " +
                              std::to_string(rx.received) + " of " + std::to_string(rx.total) + ")");
  } else if (rx.received + len > rx.total) {
    result = fail(r, 400, "More data than the announced size (received " + std::to_string(rx.received) + " of " + std::to_string(rx.total) + ")");
  } else if (esp_ota_write(handle, data, len) != ESP_OK) {
    abortReceiving("the flash could not be written");
    result = fail(r, 500, "Flash write failed");
  } else {
    scan(data, len);
    rx.received += len;
    result = reply(r, 200, "{\"ok\":true,\"received\":" + std::to_string(rx.received) + ",\"total\":" + std::to_string(rx.total) + "}");
  }
  heap_caps_free(body);
  return result;
}

esp_err_t handleFinish(httpd_req_t* r) {
  if (!authorized(r)) return ESP_OK;
  if (!rx.active) return fail(r, 409, "No update in progress");
  if (rx.received != rx.total) return fail(r, 409, "Only " + std::to_string(rx.received) + " of " + std::to_string(rx.total) + " bytes have arrived");
  if (!rx.tag.found) {
    abortReceiving("the image has no routeOS marker");
    return fail(r, 400, "This is not a routeOS firmware image (it has no routeOS marker), so it was not installed. The device keeps running its current firmware");
  }
  if (!rx.tag.guard) {
    abortReceiving("the image cannot guard its own start");
    return fail(r, 400, "This image cannot roll itself back if it fails to start, so it was not installed. The device keeps running its current firmware");
  }
  esp_err_t err = esp_ota_end(handle);
  handle = 0;
  if (err == ESP_OK) err = esp_ota_set_boot_partition(target);
  if (err != ESP_OK) {
    rx.active = false;
    strlcpy(rx.error, esp_err_to_name(err), sizeof(rx.error));
    log('E', "firmware image rejected: %s", esp_err_to_name(err));
    return fail(r, 400, std::string("The image failed the device's own integrity check (") + esp_err_to_name(err) + "), so it was not installed. The device keeps running its current firmware");
  }
  rx.active = false;
  {
    Nvs p("rosota", true);
    p.putFlag("pending", true);                                  // the new image is on trial until it proves itself
    p.putU8("boots", 0);
    noteSwitch(p, "installed", std::string("Installed ") + rx.tag.name + " " + rx.tag.version + " over " + internal::firmwareVersion());
  }
  log('I', "firmware %s %s installed; restarting into it", rx.tag.name, rx.tag.version);
  esp_err_t e = reply(r, 200, std::string("{\"ok\":true,\"rebooting\":true,\"installed\":{\"name\":\"") + rx.tag.name + "\",\"version\":\"" + rx.tag.version + "\"}}");
  internal::rebootSoon();
  return e;
}

esp_err_t handleAbort(httpd_req_t* r) {
  if (!authorized(r)) return ESP_OK;
  if (rx.active) abortReceiving("cancelled");
  return reply(r, 200, "{\"ok\":true}");
}

esp_err_t handleRollback(httpd_req_t* r) {
  if (!authorized(r)) return ESP_OK;
  if (rx.active) return fail(r, 409, "A firmware update is in progress");
  const esp_partition_t* other = esp_ota_get_next_update_partition(nullptr);
  esp_app_desc_t desc;
  if (!other || esp_ota_get_partition_description(other, &desc) != ESP_OK) return fail(r, 409, "There is no other firmware stored on this device to go back to");
  std::string leaving = internal::firmwareVersion();
  if (esp_ota_set_boot_partition(other) != ESP_OK) return fail(r, 409, "The stored firmware failed its integrity check, so the device stays on the current one");
  {
    Nvs p("rosota", true);
    p.putFlag("pending", true);                                  // whichever way it goes, the image switched to is guarded too
    p.putU8("boots", 0);
    noteSwitch(p, "switched", "Switched away from " + leaving + " by request");
  }
  log('I', "switching to the other firmware slot (%s) by request", other->label);
  esp_err_t e = reply(r, 200, "{\"ok\":true,\"rebooting\":true}");
  internal::rebootSoon();
  return e;
}

}  // namespace

void begin() {
  Nvs p("rosota", true);
  bool pending = p.flag("pending");
  uint8_t starts = p.u8("boots");
  if (!pending) return;
  if (starts >= MAX_STARTS) {
    rollbackNow("it failed to stay up on 3 starts in a row");        // restarts, unless there is nothing to go back to
    return;
  }
  p.putU8("boots", starts + 1);
  guarding = true;
  log('W', "new firmware on trial (start %u of %u): it must stay up on the network for 30 s, or the previous one returns",
      (unsigned)(starts + 1), (unsigned)MAX_STARTS);
  xTaskCreate(guardTask, "ros-guard", 4096, nullptr, 3, nullptr);
}

void networkUp() { networkIsUp = true; }
bool active() { return rx.active; }

void attach(httpd_handle_t server) {
  static const httpd_uri_t info = {"/api/ota", HTTP_GET, [](httpd_req_t* r) { return authorized(r) ? reply(r, 200, infoJson()) : ESP_OK; }, nullptr};
  static const httpd_uri_t begin = {"/api/ota/begin", HTTP_POST, handleBegin, nullptr};
  static const httpd_uri_t chunk = {"/api/ota/chunk", HTTP_POST, handleChunk, nullptr};
  static const httpd_uri_t finish = {"/api/ota/finish", HTTP_POST, handleFinish, nullptr};
  static const httpd_uri_t abort = {"/api/ota/abort", HTTP_POST, handleAbort, nullptr};
  static const httpd_uri_t rollback = {"/api/ota/rollback", HTTP_POST, handleRollback, nullptr};
  httpd_register_uri_handler(server, &info);
  httpd_register_uri_handler(server, &begin);
  httpd_register_uri_handler(server, &chunk);
  httpd_register_uri_handler(server, &finish);
  httpd_register_uri_handler(server, &abort);
  httpd_register_uri_handler(server, &rollback);
}

}  // namespace ota
}  // namespace routeos
