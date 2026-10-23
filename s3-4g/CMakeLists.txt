cmake_minimum_required(VERSION 3.16)

# routeOS node on the Waveshare ESP32-S3-A-SIM7670X-4G: LTE (PPP) + Tailscale (microlink) + the routeOS core.
# The auth key lives in sdkconfig.credentials, which is gitignored.
set(SDKCONFIG_DEFAULTS "sdkconfig.defaults;sdkconfig.credentials")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(routeos_s3_4g)
