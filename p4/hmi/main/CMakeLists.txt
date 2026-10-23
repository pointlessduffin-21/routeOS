# The calculator logic and the RVDP codec are the same source files the ESP32 firmware (virtual-hmi) builds.
# screens.h from the EEZ export only supplies the widget list; layout.cpp draws the screens at the panel's size.
set(SHARED ${CMAKE_CURRENT_LIST_DIR}/../../../virtual-hmi/src)
idf_component_register(
    SRCS main.cpp display.cpp layout.cpp ui_bindings.cpp rvdp_server.cpp
         ${SHARED}/calculator.cpp ${SHARED}/rvdp_protocol.cpp
    INCLUDE_DIRS . ${SHARED} ${SHARED}/eez_generated
    REQUIRES routeos esp32_p4_function_ev_board esp_driver_gpio esp_driver_jpeg esp_timer nvs_flash esp_http_server esp_wifi)

# The panel is 800x1280, so a strip is 8 rows: 12,800 bytes per rectangle, still under the protocol's 16,384 limit.
# 8-row bands: 12.8 KB per frame, 160 frames for a full 800x1280 screen. Each band is written in full (partial writes
# are resumed), so a weak link only slows the stream down instead of breaking it.
target_compile_definitions(${COMPONENT_LIB} PRIVATE RVDP_WIDTH=800 RVDP_HEIGHT=1280 RVDP_STRIP_ROWS=8)