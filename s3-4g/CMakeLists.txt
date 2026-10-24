cmake_minimum_required(VERSION 3.16)

# routeOS node on the Waveshare ESP32-S3-A-SIM7670X-4G: LTE (PPP) + Tailscale (microlink) + the routeOS core.
#
# sdkconfig.defaults is complete and committed: a fresh clone reproduces the same configuration with no menconfig.
# sdkconfig.credentials holds only CONFIG_ML_TAILSCALE_AUTH_KEY and the node name, and is gitignored. It is optional -
# without it the firmware builds and runs cellular-only.
set(SDKCONFIG_DEFAULTS "sdkconfig.defaults")
if(EXISTS "${CMAKE_CURRENT_LIST_DIR}/sdkconfig.credentials")
    set(SDKCONFIG_DEFAULTS "sdkconfig.defaults;${CMAKE_CURRENT_LIST_DIR}/sdkconfig.credentials")
endif()

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(routeos_s3_4g)
