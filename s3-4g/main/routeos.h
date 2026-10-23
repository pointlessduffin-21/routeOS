#pragma once
// The one piece of the routeOS IDF core this build needs: the log line that the RVDP transport writes its events to.
// Kept as a shim so rvdp_server.cpp stays identical to the P4 version.
namespace routeos {
void log(char level, const char* format, ...);
}
