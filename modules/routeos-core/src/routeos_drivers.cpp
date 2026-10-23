#include "routeos_drivers.h"
#include "routeos_log.h"
#include <Preferences.h>
#include <Wire.h>
#include <math.h>
#include <string.h>

namespace routeos {
namespace drivers {

// ---- Pin map (classic ESP32). Other chips have no map yet, so drivers stay disabled there. ----
#if CONFIG_IDF_TARGET_ESP32
static const uint8_t USABLE[]     = {4, 5, 13, 14, 16, 17, 18, 19, 21, 22, 23, 25, 26, 27, 32, 33, 34, 35, 36, 39};
static const uint8_t INPUT_ONLY[] = {34, 35, 36, 39};
static const uint8_t ADC1[]       = {32, 33, 34, 35, 36, 39};   // ADC2 is unusable while Wi-Fi is on
static const size_t USABLE_N = sizeof(USABLE), INPUT_ONLY_N = sizeof(INPUT_ONLY), ADC1_N = sizeof(ADC1);
static const uint8_t I2C_SDA = 21, I2C_SCL = 22;
static const bool SUPPORTED = true;
#else
static const uint8_t USABLE[] = {255}, INPUT_ONLY[] = {255}, ADC1[] = {255};
static const size_t USABLE_N = 0, INPUT_ONLY_N = 0, ADC1_N = 0;
static const uint8_t I2C_SDA = 255, I2C_SCL = 255;
static const bool SUPPORTED = false;
#endif

static bool in(const uint8_t* list, size_t n, long v) {
  for (size_t i = 0; i < n; i++) if (list[i] == v) return true;
  return false;
}

enum Type : uint8_t { T_ADC, T_DIGITAL, T_PULSE, T_I2C };
enum Quality : uint8_t { Q_PENDING, Q_GOOD, Q_BAD };

struct Driver {
  bool used = false;
  uint32_t gen = 0;
  char id[33] = {0};
  char spec[MAX_SPEC + 1] = {0};
  Type type = T_DIGITAL;
  uint8_t pinA = 0, pinB = 0;
  uint32_t intervalMs = 10000;
  uint8_t samples = 1;
  bool invert = false;
  uint8_t pull = 0;                      // 0 none, 1 up, 2 down
  uint8_t addr = 0, reg = 0, bytes = 1, shift = 0;
  bool big = true, sgn = false;
  char signal[33] = {0};
  char unit[9] = {0};
  float scale = 1, offset = 0;
  // runtime
  Quality quality = Q_PENDING;
  float value = 0;
  uint32_t sampledAt = 0, nextAt = 0, count = 0, errors = 0;
};

static Driver table[MAX_DRIVERS];
static SemaphoreHandle_t lockHandle = nullptr;
static uint32_t generation = 0;
static bool wireStarted = false;

static void lock() { xSemaphoreTake(lockHandle, portMAX_DELAY); }
static void unlock() { xSemaphoreGive(lockHandle); }

// ---- Validation ----
static bool validId(const char* s) {
  size_t n = strlen(s);
  if (n < 2 || n > 32) return false;
  for (size_t i = 0; i < n; i++) if (!((s[i] >= 'a' && s[i] <= 'z') || (s[i] >= '0' && s[i] <= '9') || s[i] == '-')) return false;
  return s[0] != '-' && s[n - 1] != '-';
}
static bool validSignal(const char* s) {
  size_t n = strlen(s);
  if (n < 1 || n > 32 || !(s[0] >= 'a' && s[0] <= 'z')) return false;
  for (size_t i = 0; i < n; i++) if (!((s[i] >= 'a' && s[i] <= 'z') || (s[i] >= '0' && s[i] <= '9') || s[i] == '_')) return false;
  return true;
}
static bool validUnit(const char* s) {
  size_t n = strlen(s);
  if (n > 8) return false;
  for (size_t i = 0; i < n; i++) if (s[i] < 0x20 || s[i] > 0x7e || s[i] == '"' || s[i] == '\\') return false;
  return true;
}

static uint64_t claims(const Driver& d) {
  switch (d.type) {
    case T_ADC: case T_DIGITAL: return 1ULL << d.pinA;
    case T_PULSE: return (1ULL << d.pinA) | (1ULL << d.pinB);
    case T_I2C: return (1ULL << I2C_SDA) | (1ULL << I2C_SCL);
  }
  return 0;
}

static bool readInt(JsonObjectConst o, const char* key, long def, long lo, long hi, long& out, String& err) {
  JsonVariantConst v = o[key];
  if (v.isNull()) { out = def; return true; }
  if (!v.is<long>()) { err = String(key) + " must be a whole number"; return false; }
  out = v.as<long>();
  if (out < lo || out > hi) { err = String(key) + " must be " + lo + " to " + hi; return false; }
  return true;
}
static bool readBool(JsonObjectConst o, const char* key, bool def, bool& out, String& err) {
  JsonVariantConst v = o[key];
  if (v.isNull()) { out = def; return true; }
  if (!v.is<bool>()) { err = String(key) + " must be true or false"; return false; }
  out = v.as<bool>();
  return true;
}
static bool readFloat(JsonObjectConst o, const char* key, float def, float limit, float& out, String& err) {
  JsonVariantConst v = o[key];
  if (v.isNull()) { out = def; return true; }
  if (!v.is<float>()) { err = String(key) + " must be a number"; return false; }
  out = v.as<float>();
  if (!isfinite(out) || fabsf(out) > limit) { err = String(key) + " is out of range"; return false; }
  return true;
}

static bool parse(JsonObjectConst o, const char* expectedId, Driver& d, String& err) {
  static const char* const allowed[] = {"id", "type", "interval_ms", "pins", "samples", "invert", "pull",
                                        "address", "register", "bytes", "endian", "signed", "shift", "signal"};
  for (JsonPairConst kv : o) {
    bool ok = false;
    for (const char* a : allowed) if (!strcmp(kv.key().c_str(), a)) ok = true;
    if (!ok) { err = String("Unknown field: ") + kv.key().c_str(); return false; }
  }
  const char* id = o["id"] | "";
  if (!validId(id) || strcmp(id, expectedId)) { err = "id must match the URL: 2 to 32 characters, a-z 0-9 and -"; return false; }
  strlcpy(d.id, id, sizeof(d.id));

  const char* type = o["type"] | "";
  JsonObjectConst pins = o["pins"].as<JsonObjectConst>();
  long v;
  if (!strcmp(type, "adc")) d.type = T_ADC;
  else if (!strcmp(type, "digital")) d.type = T_DIGITAL;
  else if (!strcmp(type, "pulse_echo")) d.type = T_PULSE;
  else if (!strcmp(type, "i2c_reg")) d.type = T_I2C;
  else { err = "type must be adc, digital, pulse_echo or i2c_reg"; return false; }

  if (!readInt(o, "interval_ms", 10000, 1000, 3600000, v, err)) return false;
  d.intervalMs = (uint32_t)v;
  long defSamples = d.type == T_ADC ? 8 : d.type == T_DIGITAL ? 3 : d.type == T_PULSE ? 5 : 1;
  long maxSamples = d.type == T_PULSE ? 7 : 16;
  if (!readInt(o, "samples", defSamples, 1, maxSamples, v, err)) return false;
  d.samples = (uint8_t)v;
  if (!SUPPORTED) { err = "Drivers are not supported on this chip yet"; return false; }

  switch (d.type) {
    case T_ADC:
      if (!readInt(pins, "pin", -1, 0, 39, v, err) || v < 0) { if (err.isEmpty()) err = "pins.pin is required"; return false; }
      if (!in(ADC1, ADC1_N, v)) { err = "pins.pin must be an ADC1 pin (32, 33, 34, 35, 36 or 39)"; return false; }
      d.pinA = (uint8_t)v;
      break;
    case T_DIGITAL: {
      if (!readInt(pins, "pin", -1, 0, 39, v, err) || v < 0) { if (err.isEmpty()) err = "pins.pin is required"; return false; }
      if (!in(USABLE, USABLE_N, v)) { err = "pins.pin is not a usable GPIO on this board"; return false; }
      d.pinA = (uint8_t)v;
      const char* pull = o["pull"] | "none";
      d.pull = !strcmp(pull, "up") ? 1 : !strcmp(pull, "down") ? 2 : 0;
      if (d.pull == 0 && strcmp(pull, "none")) { err = "pull must be none, up or down"; return false; }
      if (d.pull && in(INPUT_ONLY, INPUT_ONLY_N, d.pinA)) { err = "GPIO 34 to 39 have no internal pull resistor"; return false; }
      if (!readBool(o, "invert", false, d.invert, err)) return false;
      break;
    }
    case T_PULSE: {
      long trig, echo;
      if (!readInt(pins, "trig", -1, 0, 39, trig, err) || trig < 0) { if (err.isEmpty()) err = "pins.trig is required"; return false; }
      if (!readInt(pins, "echo", -1, 0, 39, echo, err) || echo < 0) { if (err.isEmpty()) err = "pins.echo is required"; return false; }
      if (!in(USABLE, USABLE_N, trig) || in(INPUT_ONLY, INPUT_ONLY_N, trig)) { err = "pins.trig must be an output-capable GPIO"; return false; }
      if (!in(USABLE, USABLE_N, echo) || trig == echo) { err = "pins.echo must be a different usable GPIO"; return false; }
      d.pinA = (uint8_t)trig; d.pinB = (uint8_t)echo;
      break;
    }
    case T_I2C: {
      if (!pins.isNull()) { err = "I2C drivers use the fixed bus on GPIO 21 (SDA) and 22 (SCL); omit pins"; return false; }
      if (!readInt(o, "address", -1, 0x03, 0x77, v, err) || v < 0) { if (err.isEmpty()) err = "address is required (3 to 119)"; return false; }
      d.addr = (uint8_t)v;
      if (!readInt(o, "register", -1, 0, 255, v, err) || v < 0) { if (err.isEmpty()) err = "register is required (0 to 255)"; return false; }
      d.reg = (uint8_t)v;
      if (!readInt(o, "bytes", 2, 1, 4, v, err)) return false;
      d.bytes = (uint8_t)v;
      if (!readInt(o, "shift", 0, 0, 24, v, err)) return false;
      d.shift = (uint8_t)v;
      const char* endian = o["endian"] | "big";
      if (strcmp(endian, "big") && strcmp(endian, "little")) { err = "endian must be big or little"; return false; }
      d.big = !strcmp(endian, "big");
      if (!readBool(o, "signed", false, d.sgn, err)) return false;
      break;
    }
  }

  JsonObjectConst sig = o["signal"].as<JsonObjectConst>();
  if (sig.isNull()) { err = "signal is required"; return false; }
  for (JsonPairConst kv : sig) {
    const char* k = kv.key().c_str();
    if (strcmp(k, "name") && strcmp(k, "unit") && strcmp(k, "scale") && strcmp(k, "offset")) { err = String("Unknown signal field: ") + k; return false; }
  }
  const char* name = sig["name"] | "";
  const char* unit = sig["unit"] | "";
  if (!validSignal(name)) { err = "signal.name must be lowercase letters, digits and _, starting with a letter (32 max)"; return false; }
  if (!validUnit(unit)) { err = "signal.unit must be printable ASCII, 8 characters at most"; return false; }
  strlcpy(d.signal, name, sizeof(d.signal));
  strlcpy(d.unit, unit, sizeof(d.unit));
  if (!readFloat(sig, "scale", 1.0f, 1e6f, d.scale, err)) return false;
  if (!readFloat(sig, "offset", 0.0f, 1e9f, d.offset, err)) return false;
  return true;
}

// ---- Sampling ----
static bool sampleDriver(const Driver& d, float& out) {
  float raw = 0;
  switch (d.type) {
    case T_ADC: {
      analogSetPinAttenuation(d.pinA, ADC_11db);
      uint32_t sum = 0;
      for (uint8_t i = 0; i < d.samples; i++) { sum += analogReadMilliVolts(d.pinA); delayMicroseconds(200); }
      raw = (float)sum / d.samples;
      break;
    }
    case T_DIGITAL: {
      pinMode(d.pinA, d.pull == 1 ? INPUT_PULLUP : d.pull == 2 ? INPUT_PULLDOWN : INPUT);
      delay(2);
      uint8_t high = 0;
      for (uint8_t i = 0; i < d.samples; i++) { high += digitalRead(d.pinA) ? 1 : 0; delay(2); }
      bool level = high * 2 > d.samples;
      raw = (level != d.invert) ? 1.0f : 0.0f;
      break;
    }
    case T_PULSE: {
      pinMode(d.pinA, OUTPUT);
      pinMode(d.pinB, INPUT);
      uint32_t us[8];
      uint8_t n = 0;
      for (uint8_t i = 0; i < d.samples && n < 8; i++) {
        digitalWrite(d.pinA, LOW); delayMicroseconds(2);
        digitalWrite(d.pinA, HIGH); delayMicroseconds(10);
        digitalWrite(d.pinA, LOW);
        uint32_t t = pulseIn(d.pinB, HIGH, 30000);
        if (t > 0) us[n++] = t;
        vTaskDelay(pdMS_TO_TICKS(25));
      }
      if (!n) return false;
      for (uint8_t i = 1; i < n; i++) { uint32_t k = us[i]; int j = i - 1; while (j >= 0 && us[j] > k) { us[j + 1] = us[j]; j--; } us[j + 1] = k; }
      raw = (float)us[n / 2];
      break;
    }
    case T_I2C: {
      if (!wireStarted) { Wire.begin(I2C_SDA, I2C_SCL, 100000); Wire.setTimeOut(50); wireStarted = true; }
      Wire.beginTransmission(d.addr);
      Wire.write(d.reg);
      if (Wire.endTransmission(false) != 0) { Wire.endTransmission(); return false; }
      if (Wire.requestFrom((int)d.addr, (int)d.bytes) != d.bytes) return false;
      uint32_t word = 0;
      for (uint8_t i = 0; i < d.bytes; i++) {
        uint32_t b = (uint32_t)Wire.read() & 0xff;
        if (d.big) word = (word << 8) | b; else word |= b << (8 * i);
      }
      if (d.sgn) {
        int bits = d.bytes * 8;
        int32_t s = (bits < 32 && (word & (1UL << (bits - 1)))) ? (int32_t)(word | (~0UL << bits)) : (int32_t)word;
        raw = (float)(s >> d.shift);
      } else {
        raw = (float)(word >> d.shift);
      }
      break;
    }
  }
  out = raw * d.scale + d.offset;
  return isfinite(out);
}

static void samplerTask(void*) {
  for (;;) {
    for (size_t i = 0; i < MAX_DRIVERS; i++) {
      Driver local;
      bool due = false;
      lock();
      if (table[i].used && (int32_t)(millis() - table[i].nextAt) >= 0) { local = table[i]; due = true; }
      unlock();
      if (!due) continue;
      float value = 0;
      bool ok = sampleDriver(local, value);
      bool lost = false, regained = false;
      lock();
      if (table[i].used && table[i].gen == local.gen) {
        table[i].sampledAt = millis();
        table[i].count++;
        Quality before = table[i].quality;
        if (ok) { table[i].value = value; table[i].quality = Q_GOOD; }
        else { table[i].quality = Q_BAD; table[i].errors++; }
        table[i].nextAt = millis() + table[i].intervalMs;
        lost = !ok && before != Q_BAD;                // report changes, not every sample
        regained = ok && before == Q_BAD;
      }
      unlock();
      if (lost) log('W', "driver %s: no valid reading", local.id);
      if (regained) log('I', "driver %s: reading again", local.id);
    }
    vTaskDelay(pdMS_TO_TICKS(100));
  }
}

// The sampler costs a task's stack and control block, and a display-only board has no drivers, so it starts with the first one.
static void ensureSampler() {
  static bool running = false;
  if (running) return;
  running = xTaskCreatePinnedToCore(samplerTask, "ros-drivers", 4096, nullptr, 1, nullptr, 1) == pdPASS;
}

// ---- Persistence ----
static void persist() {                  // caller holds the lock
  String all = "[";
  bool first = true;
  for (size_t i = 0; i < MAX_DRIVERS; i++) {
    if (!table[i].used) continue;
    if (!first) all += ",";
    all += table[i].spec;
    first = false;
  }
  all += "]";
  Preferences p;
  p.begin("rosdrv", false);
  p.putString("specs", all);
  p.end();
}

static bool conflicts(const Driver& d, size_t ignore, String& err) {   // caller holds the lock
  for (size_t i = 0; i < MAX_DRIVERS; i++) {
    if (!table[i].used || i == ignore) continue;
    if (d.type == T_I2C && table[i].type == T_I2C) continue;           // the bus is shared
    if (claims(d) & claims(table[i])) { err = String("Pin already used by driver ") + table[i].id; return true; }
  }
  return false;
}

static bool store(const String& id, const String& body, String& error, bool save) {
  if (body.length() == 0 || body.length() > MAX_SPEC) { error = String("Driver spec must be 1 to ") + MAX_SPEC + " bytes"; return false; }
  JsonDocument doc;
  if (deserializeJson(doc, body)) { error = "Driver spec is not valid JSON"; return false; }
  if (!doc.is<JsonObject>()) { error = "Driver spec must be a JSON object"; return false; }
  Driver d;
  if (!parse(doc.as<JsonObjectConst>(), id.c_str(), d, error)) return false;
  lock();
  size_t slot = MAX_DRIVERS, freeSlot = MAX_DRIVERS;
  for (size_t i = 0; i < MAX_DRIVERS; i++) {
    if (table[i].used && !strcmp(table[i].id, d.id)) slot = i;
    if (!table[i].used && freeSlot == MAX_DRIVERS) freeSlot = i;
  }
  size_t target = slot != MAX_DRIVERS ? slot : freeSlot;
  bool ok = true;
  if (target == MAX_DRIVERS) { error = String("At most ") + MAX_DRIVERS + " drivers can be installed"; ok = false; }
  else if (conflicts(d, slot, error)) ok = false;
  if (ok) {
    d.used = true;
    d.gen = ++generation;
    d.nextAt = millis();
    strlcpy(d.spec, body.c_str(), sizeof(d.spec));
    table[target] = d;
    if (save) persist();
  }
  unlock();
  if (ok) ensureSampler();
  return ok;
}

// ---- Public API ----
bool supported() { return SUPPORTED; }

size_t count() {
  size_t n = 0;
  lock();
  for (size_t i = 0; i < MAX_DRIVERS; i++) n += table[i].used;
  unlock();
  return n;
}

void begin() {
  lockHandle = xSemaphoreCreateMutex();
  if (!SUPPORTED) return;
  Preferences p;
  p.begin("rosdrv", true);
  String saved = p.getString("specs", "[]");
  p.end();
  JsonDocument doc;
  if (!deserializeJson(doc, saved) && doc.is<JsonArray>()) {
    for (JsonObjectConst item : doc.as<JsonArrayConst>()) {
      String body, error;
      serializeJson(item, body);
      String id = item["id"] | "";
      if (!store(id, body, error, false)) log('W', "skipped saved driver '%s': %s", id.c_str(), error.c_str());
    }
  }
  log('I', "drivers ready: %u installed", (unsigned)count());
}

void describePins(JsonObject out) {
  JsonArray allowed = out["allowed"].to<JsonArray>();
  for (size_t i = 0; i < USABLE_N; i++) allowed.add(USABLE[i]);
  JsonArray inputOnly = out["input_only"].to<JsonArray>();
  for (size_t i = 0; i < INPUT_ONLY_N; i++) inputOnly.add(INPUT_ONLY[i]);
  JsonArray adc = out["adc"].to<JsonArray>();
  for (size_t i = 0; i < ADC1_N; i++) adc.add(ADC1[i]);
  if (SUPPORTED) { JsonObject i2c = out["i2c"].to<JsonObject>(); i2c["sda"] = I2C_SDA; i2c["scl"] = I2C_SCL; }
}

String listJson() {
  String out = "{\"drivers\":[";
  bool first = true;
  lock();
  for (size_t i = 0; i < MAX_DRIVERS; i++) {
    if (!table[i].used) continue;
    if (!first) out += ",";
    out += table[i].spec;
    first = false;
  }
  unlock();
  return out + "]}";
}

String readingsJson() {
  JsonDocument doc;
  JsonArray list = doc["readings"].to<JsonArray>();
  uint32_t now = millis();
  lock();
  for (size_t i = 0; i < MAX_DRIVERS; i++) {
    const Driver& d = table[i];
    if (!d.used) continue;
    JsonObject r = list.add<JsonObject>();
    r["id"] = d.id;
    r["signal"] = d.signal;
    r["unit"] = d.unit;
    r["quality"] = d.quality == Q_GOOD ? "good" : d.quality == Q_BAD ? "bad" : "pending";
    if (d.quality == Q_GOOD) r["value"] = d.value; else r["value"] = nullptr;
    r["age_ms"] = d.count ? now - d.sampledAt : 0;
    r["interval_ms"] = d.intervalMs;
    r["samples_taken"] = d.count;
    r["errors"] = d.errors;
  }
  unlock();
  String out;
  serializeJson(doc, out);
  return out;
}

bool install(const String& id, const String& body, String& error) {
  bool ok = store(id, body, error, true);
  if (ok) log('I', "driver %s installed", id.c_str());
  return ok;
}

bool remove(const String& id, String& error) {
  bool found = false;
  lock();
  for (size_t i = 0; i < MAX_DRIVERS; i++) {
    if (table[i].used && id == table[i].id) { table[i] = Driver(); found = true; break; }
  }
  if (found) persist();
  unlock();
  if (!found) error = "No such driver";
  else log('I', "driver %s removed", id.c_str());
  return found;
}

}  // namespace drivers
}  // namespace routeos
