#pragma once
// Declarative driver engine. A driver is data, not code: one of four fixed sampling
// primitives plus a scale and offset. Every field is validated here, whatever sent it.
#include <Arduino.h>
#include <ArduinoJson.h>

namespace routeos {
namespace drivers {

constexpr size_t MAX_DRIVERS = 8;
constexpr size_t MAX_SPEC = 380;   // bytes of JSON per driver

bool supported();                                  // false on chips whose pin map is not defined yet
void begin();                                      // load saved drivers, start the sampler task
size_t count();
void describePins(JsonObject out);                 // allowed / input-only / ADC1 / I2C pins
String listJson();                                 // {"drivers":[...]}
String readingsJson();                             // {"readings":[...]}
bool install(const String& id, const String& body, String& error);
bool remove(const String& id, String& error);

}  // namespace drivers
}  // namespace routeos
