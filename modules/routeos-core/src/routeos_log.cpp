#include "routeos_log.h"
#include <ArduinoJson.h>
#include <esp_system.h>
#include <stdarg.h>

namespace routeos {

namespace {
constexpr size_t SLOTS = 48;
constexpr size_t TEXT = 92;
struct Line {
  uint32_t seq;
  uint32_t ms;
  char level;
  char text[TEXT];
};
Line ring[SLOTS];
uint32_t nextSeq = 1;                       // sequence number the next line will get
portMUX_TYPE mux = portMUX_INITIALIZER_UNLOCKED;
uint32_t boot = 0;
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
  // One printable line each, whatever was formatted into it.
  for (char* p = text; *p; p++) {
    if ((uint8_t)*p < 32 || (uint8_t)*p > 126) *p = '?';
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
  Serial.printf("[core] %c %s\n", level, text);
}

String logsJson(uint32_t since) {
  uint32_t last;
  portENTER_CRITICAL(&mux);
  last = nextSeq - 1;
  portEXIT_CRITICAL(&mux);
  uint32_t oldest = last >= SLOTS ? last - SLOTS + 1 : 1;
  uint32_t start = since + 1 > oldest ? since + 1 : oldest;

  JsonDocument doc;
  doc["schema"] = 1;
  doc["boot_id"] = bootId();
  doc["uptime_s"] = millis() / 1000;
  doc["next"] = last;
  // Lines the reader never saw because the buffer wrapped first.
  doc["dropped"] = (since > 0 && since + 1 < oldest) ? oldest - (since + 1) : 0;
  JsonArray lines = doc["lines"].to<JsonArray>();
  for (uint32_t seq = start; seq <= last && seq != 0; seq++) {
    Line copy;
    portENTER_CRITICAL(&mux);
    copy = ring[seq % SLOTS];
    portEXIT_CRITICAL(&mux);
    if (copy.seq != seq) continue;           // overwritten while we were reading
    JsonObject item = lines.add<JsonObject>();
    item["n"] = copy.seq;
    item["t"] = copy.ms;
    item["l"] = copy.level == 'W' ? "W" : copy.level == 'E' ? "E" : "I";     // literals: safe to reference
    item["m"] = String(copy.text);                                          // a String is always copied
  }
  String out;
  serializeJson(doc, out);
  return out;
}

}  // namespace routeos
