#pragma once
// Shared by the Core's own source files. Not part of the public API.
#include <Arduino.h>
#include <WebServer.h>

namespace routeos {
namespace internal {

// Token check for management routes. On failure it sends the reply itself (401, 403 or 503).
bool authorized(WebServer& http);
// The same check without sending anything, for handlers that reply later (streaming uploads).
bool tokenValid(WebServer& http);
void reply(WebServer& http, int code, const String& body);
void fail(WebServer& http, int code, const String& message);
// Restart shortly, so the HTTP reply that announced it can leave first.
void rebootSoon();

const char* firmwareName();
const char* firmwareVersion();
const char* resetReasonText();

}  // namespace internal
}  // namespace routeos
