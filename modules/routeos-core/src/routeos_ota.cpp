#include "routeos_ota.h"
#include "routeos_internal.h"
#include "routeos_log.h"
#include <ArduinoJson.h>
#include <Preferences.h>
#include <Update.h>
#include <esp_ota_ops.h>
#include <esp_partition.h>
#include <lwip/sockets.h>
#include <stdarg.h>

extern "C" const char routeos_image_tag[];       // stamped into the image by ROUTEOS_IMAGE()

namespace routeos {
namespace ota {

namespace {

constexpr size_t CHUNK_MAX = 16384;             // what a resend costs if one piece is lost
constexpr uint32_t STALL_MS = 8000;             // an upload socket silent this long is closed (see uploadWatchdog)
constexpr uint32_t CONFIRM_AFTER_MS = 30000;    // a new image must stay up this long on the network
constexpr uint32_t GIVE_UP_AFTER_MS = 120000;   // ...and must get there within this long
constexpr uint8_t MAX_STARTS = 3;               // starts allowed before going back
constexpr uint32_t QUIET_ABANDON_MS = 60000;    // an upload this quiet is considered abandoned

// ---- The marker a routeOS image carries, found while the bytes stream past ----
struct Tag {
  bool found;
  bool guard;
  char name[40];
  char version[24];
  char core[12];
};

struct Rx {
  bool active;
  size_t total;
  size_t received;
  uint32_t lastActivity;
  char error[100];
  Tag tag;
  uint8_t match;            // progress through the marker prefix
  bool collecting;          // reading the rest of the marker
  uint8_t len;
  char buf[96];
};
Rx rx;

struct Chunk {              // the piece being received right now
  bool seen;                // an upload callback ran for this request
  bool inFlight;            // bytes are expected on the socket
  bool authOk;
  int status;               // 0 while fine, otherwise the HTTP status to answer with
  char error[100];
  int fd;                   // the socket, so a stalled upload can be cut
  uint32_t lastProgress;
};
Chunk chunk = {false, false, false, 0, "", -1, 0};

bool networkIsUp = false;
bool guarding = false;

const char PREFIX[] = "ROUTEOS-IMAGE|1|";
constexpr uint8_t PREFIX_LEN = sizeof(PREFIX) - 1;

void parseTag() {
  // name|version|core=1.1.0|ota=1|END
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
      rx.match = c == (uint8_t)PREFIX[0] ? 1 : 0;      // no proper border in the prefix, so this is exact
    }
  }
}

// ---- Persistent bookkeeping (namespace "rosota") ----
String getString(const char* key) {
  Preferences p;
  p.begin("rosota", true);
  String v = p.getString(key, "");
  p.end();
  return v;
}

// Record that `leaving` is where we came from, so the way back is known.
void noteSwitch(Preferences& p, const char* kind, const String& text) {
  const esp_partition_t* running = esp_ota_get_running_partition();
  p.putString("prev", running ? running->label : "");
  p.putString("prev_ver", internal::firmwareVersion());
  p.putString("prev_name", internal::firmwareName());
  p.putBool("other_ok", true);
  p.putString("last_kind", kind);
  p.putString("last_text", text);
}

void abortReceiving(const char* why) {
  Update.abort();
  rx.active = false;
  strlcpy(rx.error, why, sizeof(rx.error));
  log('W', "firmware update abandoned: %s", why);
}

// Boot the slot we came from. Returns only when that is impossible.
bool rollbackNow(const char* reason) {
  Preferences p;
  p.begin("rosota", false);
  String label = p.getString("prev", "");
  const esp_partition_t* target = label.length() ? esp_partition_find_first(ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_ANY, label.c_str()) : nullptr;
  esp_app_desc_t desc;
  bool ok = target && esp_ota_get_partition_description(target, &desc) == ESP_OK && esp_ota_set_boot_partition(target) == ESP_OK;
  p.putBool("pending", false);
  p.putUChar("boots", 0);
  if (!ok) {
    p.putString("last_kind", "failed");
    p.putString("last_text", String("The new firmware did not start properly, and the previous firmware could not be restored: ") + reason);
    p.end();
    log('E', "cannot roll back (%s): the previous firmware is not usable", reason);
    return false;
  }
  noteSwitch(p, "rolled-back", String("Rolled back from ") + internal::firmwareVersion() + " because " + reason);
  p.end();
  log('E', "rolling back to the previous firmware because %s", reason);
  delay(400);
  ESP.restart();
  return true;
}

void guardTask(void*) {
  for (;;) {
    vTaskDelay(pdMS_TO_TICKS(500));
    uint32_t up = millis();
    if (networkIsUp && up >= CONFIRM_AFTER_MS) {
      Preferences p;
      p.begin("rosota", false);
      p.putBool("pending", false);
      p.putUChar("boots", 0);
      p.putString("last_kind", "confirmed");
      p.putString("last_text", String("Firmware ") + internal::firmwareVersion() + " started cleanly and is confirmed");
      p.end();
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

// ---- JSON ----
String infoJson() {
  JsonDocument doc;
  doc["schema"] = 1;
  doc["boot_id"] = bootId();                        // changes on every start: how a console knows the device restarted
  doc["uptime_s"] = millis() / 1000;
  const esp_partition_t* running = esp_ota_get_running_partition();
  const esp_partition_t* other = esp_ota_get_next_update_partition(nullptr);
  Preferences p;
  p.begin("rosota", true);

  JsonObject run = doc["running"].to<JsonObject>();
  run["label"] = String(running ? running->label : "");
  run["slot_kb"] = running ? running->size / 1024 : 0;
  run["image_kb"] = ESP.getSketchSize() / 1024;
  run["name"] = String(internal::firmwareName());
  run["version"] = String(internal::firmwareVersion());
  run["tag"] = String(routeos_image_tag);

  esp_app_desc_t desc;
  bool present = other && esp_ota_get_partition_description(other, &desc) == ESP_OK;
  String prevLabel = p.getString("prev", "");
  bool known = present && p.getBool("other_ok", false) && other && prevLabel == other->label;
  JsonObject oth = doc["other"].to<JsonObject>();
  oth["label"] = String(other ? other->label : "");
  oth["slot_kb"] = other ? other->size / 1024 : 0;
  oth["present"] = present;                         // holds something that looks like firmware
  oth["verified"] = known;                          // it was whole when it was stored here
  oth["version"] = known ? p.getString("prev_ver", "") : String("");
  oth["name"] = known ? p.getString("prev_name", "") : String("");

  JsonObject up = doc["update"].to<JsonObject>();
  up["active"] = rx.active;
  up["received"] = rx.received;
  up["total"] = rx.total;
  up["error"] = String(rx.error);
  up["chunk_max"] = CHUNK_MAX;

  bool pending = p.getBool("pending", false);
  JsonObject g = doc["guard"].to<JsonObject>();
  g["pending"] = pending;
  g["watching"] = guarding;                         // the guard task is running in this boot
  g["starts"] = p.getUChar("boots", 0);
  g["max_starts"] = MAX_STARTS;
  g["confirm_in_s"] = pending && millis() < CONFIRM_AFTER_MS ? (CONFIRM_AFTER_MS - millis()) / 1000 : 0;

  JsonObject last = doc["last"].to<JsonObject>();
  last["kind"] = p.getString("last_kind", "none");
  last["text"] = p.getString("last_text", "");
  p.end();
  String out;
  serializeJson(doc, out);
  return out;
}

// ---- Routes ----
void handleBegin(WebServer& http) {
  using namespace internal;
  if (!authorized(http)) return;
  JsonDocument doc;
  if (deserializeJson(doc, http.arg("plain")) || !doc.is<JsonObject>()) {
    fail(http, 400, "Send JSON like {\"size\":1048576}");
    return;
  }
  size_t size = doc["size"] | 0;
  if (rx.active) {
    if (millis() - rx.lastActivity < QUIET_ABANDON_MS) {
      fail(http, 409, "An update is already in progress. Abort it, or let it finish");
      return;
    }
    abortReceiving("the previous upload went quiet");
  }
  const esp_partition_t* next = esp_ota_get_next_update_partition(nullptr);
  if (!next) { fail(http, 500, "This device has no second firmware slot, so it cannot update itself"); return; }
  if (size < 65536) { fail(http, 400, "That is too small to be a firmware image"); return; }
  if (size > next->size) {
    fail(http, 413, String("The image is ") + (unsigned)(size / 1024) + " KB but the update slot holds " + (unsigned)(next->size / 1024) + " KB");
    return;
  }
  if (ESP.getFreeHeap() < 30000) { fail(http, 503, "Not enough free memory to start an update right now"); return; }
  if (!Update.begin(size, U_FLASH)) {
    fail(http, 500, String("Could not start the update: ") + Update.errorString());
    return;
  }
  memset(&rx, 0, sizeof(rx));
  rx.active = true;
  rx.total = size;
  rx.lastActivity = millis();
  {
    // The slot about to be overwritten held the way back.
    Preferences p;
    p.begin("rosota", false);
    p.putBool("other_ok", false);
    p.end();
  }
  log('I', "firmware update started: %u KB into %s", (unsigned)(size / 1024), next->label);
  reply(http, 200, String("{\"ok\":true,\"received\":0,\"chunk_max\":") + CHUNK_MAX + "}");
}

void chunkError(int status, const char* fmt, ...) __attribute__((format(printf, 2, 3)));
void chunkError(int status, const char* fmt, ...) {
  chunk.status = status;
  va_list args;
  va_start(args, fmt);
  vsnprintf(chunk.error, sizeof(chunk.error), fmt, args);
  va_end(args);
}

void chunkDone(WebServer& http) {
  using namespace internal;
  bool seen = chunk.seen;                           // take and clear first, so nothing stale survives a refused request
  chunk.seen = false;
  chunk.inFlight = false;
  if (!authorized(http)) return;                    // a request that bypassed the upload path is still checked
  if (!seen) { fail(http, 400, "Send each piece as multipart/form-data with the bytes in a file part"); return; }
  if (chunk.status) {
    int status = chunk.status;
    String message = chunk.error;
    chunk.status = 0;
    fail(http, status, message + " (received " + (unsigned)rx.received + " of " + (unsigned)rx.total + ")");
    return;
  }
  reply(http, 200, String("{\"ok\":true,\"received\":") + (unsigned)rx.received + ",\"total\":" + (unsigned)rx.total + "}");
}
// A piece of the image arrives as multipart/form-data, the way Arduino's own OTA example receives a file.
// (The library's raw-body reader asks the socket for a whole buffer even when fewer bytes remain, and busy-waits
// for the rest: with the stream timeout left at 5 s that starves the idle task and the watchdog reboots the board.)
void chunkUpload(WebServer& http, HTTPUpload& up) {
  if (up.status == UPLOAD_FILE_START) {
    chunk.seen = true;
    chunk.status = 0;
    chunk.error[0] = 0;
    chunk.inFlight = true;
    chunk.lastProgress = millis();
    chunk.fd = http.client().fd();
    chunk.authOk = internal::tokenValid(http);
    if (!chunk.authOk) return;
    if (!rx.active) { chunkError(409, "No update in progress. Start one first"); return; }
    String offset = http.header("X-Ota-Offset");
    if (!offset.length() || (size_t)strtoul(offset.c_str(), nullptr, 10) != rx.received) {
      chunkError(409, "The device holds %u bytes, not %s. Continue from there", (unsigned)rx.received, offset.c_str());
      return;
    }
    rx.lastActivity = millis();
  } else if (up.status == UPLOAD_FILE_WRITE) {
    chunk.lastProgress = millis();
    if (!chunk.authOk || chunk.status) return;       // the final handler answers
    size_t n = up.currentSize;
    if (rx.received + n > rx.total) { chunkError(400, "More data than the announced size"); return; }
    if (Update.write(up.buf, n) != n) {
      chunkError(500, "Flash write failed: %s", Update.errorString());
      abortReceiving("the flash could not be written");
      return;
    }
    scan(up.buf, n);
    rx.received += n;
    rx.lastActivity = millis();
  } else if (up.status == UPLOAD_FILE_END) {
    chunk.inFlight = false;
  } else if (up.status == UPLOAD_FILE_ABORTED) {
    chunk.inFlight = false;
    if (rx.active) log('W', "an upload piece was cut short at %u of %u bytes", (unsigned)rx.received, (unsigned)rx.total);
  }
}

// The multipart reader waits for bytes with no timeout. If the link dies mid-piece the web server would wait for ever
// (and the display with it). This closes an upload socket that has gone quiet; the reader then gives up cleanly.
void uploadWatchdog(void*) {
  for (;;) {
    vTaskDelay(pdMS_TO_TICKS(500));
    if (chunk.inFlight && chunk.fd >= 0 && millis() - chunk.lastProgress > STALL_MS) {
      log('W', "an upload piece went quiet for %u s; closing it", (unsigned)(STALL_MS / 1000));
      chunk.inFlight = false;
      lwip_shutdown(chunk.fd, SHUT_RDWR);
    }
  }
}

class ChunkHandler : public RequestHandler {
 public:
  bool canHandle(HTTPMethod method, String uri) override { return method == HTTP_POST && uri == "/api/ota/chunk"; }
  bool canUpload(String uri) override { return uri == "/api/ota/chunk"; }
  // canRaw() is deliberately left false: a body that is not multipart is read the ordinary, safe way.
  bool handle(WebServer& http, HTTPMethod method, String uri) override {
    if (!canHandle(method, uri)) return false;
    chunkDone(http);
    return true;
  }
  void upload(WebServer& http, String uri, HTTPUpload& up) override {
    if (canUpload(uri)) chunkUpload(http, up);
  }
};
void handleFinish(WebServer& http) {
  using namespace internal;
  if (!authorized(http)) return;
  if (!rx.active) { fail(http, 409, "No update in progress"); return; }
  if (rx.received != rx.total) {
    fail(http, 409, String("Only ") + (unsigned)rx.received + " of " + (unsigned)rx.total + " bytes have arrived");
    return;
  }
  if (!rx.tag.found) {
    abortReceiving("the image has no routeOS marker");
    fail(http, 400, "This is not a routeOS firmware image (it has no routeOS marker), so it was not installed. The device keeps running its current firmware");
    return;
  }
  if (!rx.tag.guard) {
    abortReceiving("the image cannot guard its own start");
    fail(http, 400, "This image cannot roll itself back if it fails to start, so it was not installed. The device keeps running its current firmware");
    return;
  }
  if (!Update.end(false)) {
    String why = Update.errorString();
    rx.active = false;
    strlcpy(rx.error, why.c_str(), sizeof(rx.error));
    log('E', "firmware image rejected: %s", why.c_str());
    fail(http, 400, "The image failed the device's own integrity check (" + why + "), so it was not installed. The device keeps running its current firmware");
    return;
  }
  rx.active = false;
  {
    Preferences p;
    p.begin("rosota", false);
    p.putBool("pending", true);                     // the new image is on trial until it proves itself
    p.putUChar("boots", 0);
    noteSwitch(p, "installed", String("Installed ") + rx.tag.name + " " + rx.tag.version + " over " + firmwareVersion());
    p.end();
  }
  log('I', "firmware %s %s installed; restarting into it", rx.tag.name, rx.tag.version);
  reply(http, 200, String("{\"ok\":true,\"rebooting\":true,\"installed\":{\"name\":\"") + rx.tag.name + "\",\"version\":\"" + rx.tag.version + "\"}}");
  rebootSoon();
}

void handleAbort(WebServer& http) {
  using namespace internal;
  if (!authorized(http)) return;
  if (rx.active) abortReceiving("cancelled");
  reply(http, 200, "{\"ok\":true}");
}

void handleRollback(WebServer& http) {
  using namespace internal;
  if (!authorized(http)) return;
  if (rx.active) { fail(http, 409, "A firmware update is in progress"); return; }
  const esp_partition_t* other = esp_ota_get_next_update_partition(nullptr);
  esp_app_desc_t desc;
  if (!other || esp_ota_get_partition_description(other, &desc) != ESP_OK) {
    fail(http, 409, "There is no other firmware stored on this device to go back to");
    return;
  }
  String leaving = firmwareVersion();
  if (esp_ota_set_boot_partition(other) != ESP_OK) {
    fail(http, 409, "The stored firmware failed its integrity check, so the device stays on the current one");
    return;
  }
  {
    Preferences p;
    p.begin("rosota", false);
    p.putBool("pending", true);                     // whichever way it goes, the image switched to is guarded too
    p.putUChar("boots", 0);
    noteSwitch(p, "switched", String("Switched away from ") + leaving + " by request");
    p.end();
  }
  log('I', "switching to the other firmware slot (%s) by request", other->label);
  reply(http, 200, "{\"ok\":true,\"rebooting\":true}");
  rebootSoon();
}

}  // namespace

void begin() {
  Preferences p;
  p.begin("rosota", false);
  bool pending = p.getBool("pending", false);
  uint8_t starts = p.getUChar("boots", 0);
  if (!pending) {
    p.end();
    return;
  }
  if (starts >= MAX_STARTS) {
    p.end();
    rollbackNow("it failed to stay up on 3 starts in a row");      // restarts, unless there is nothing to go back to
    return;
  }
  p.putUChar("boots", starts + 1);
  p.end();
  guarding = true;
  log('W', "new firmware on trial (start %u of %u): it must stay up on the network for 30 s, or the previous one returns",
      (unsigned)(starts + 1), (unsigned)MAX_STARTS);
  xTaskCreate(guardTask, "ros-guard", 3072, nullptr, 1, nullptr);
}

void networkUp() { networkIsUp = true; }

bool active() { return rx.active; }

void attach(WebServer& http) {
  http.on("/api/ota", HTTP_GET, [&http]() {
    if (internal::authorized(http)) internal::reply(http, 200, infoJson());
  });
  http.on("/api/ota/begin", HTTP_POST, [&http]() { handleBegin(http); });
  http.addHandler(new ChunkHandler());                       // owned by the server for the life of the program
  xTaskCreate(uploadWatchdog, "ros-upload", 2048, nullptr, 1, nullptr);
  http.on("/api/ota/finish", HTTP_POST, [&http]() { handleFinish(http); });
  http.on("/api/ota/abort", HTTP_POST, [&http]() { handleAbort(http); });
  http.on("/api/ota/rollback", HTTP_POST, [&http]() { handleRollback(http); });
}

}  // namespace ota
}  // namespace routeos
