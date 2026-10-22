#pragma once
// A small event log kept in RAM (the last 48 lines, gone after a restart), readable through
// GET /api/logs. Put events in it, never secrets: no tokens, no Wi-Fi passwords.
#include <Arduino.h>

namespace routeos {

// level: 'I' information, 'W' warning, 'E' error. Also echoed to the serial console.
void log(char level, const char* fmt, ...) __attribute__((format(printf, 2, 3)));

// Random per start. A reader that sees it change knows the sequence numbers began again.
uint32_t bootId();

// JSON for lines newer than `since` (0 for everything held): {"boot_id","next","lines":[{"n","t","l","m"}]}.
String logsJson(uint32_t since);

}  // namespace routeos
