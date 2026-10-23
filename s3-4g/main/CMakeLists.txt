idf_component_register(
    SRCS "main.cpp"
         "display.cpp"
         "layout.cpp"
         "ui_bindings.cpp"
         "rvdp_server.cpp"
         "calculator.cpp"
         "rvdp_protocol.cpp"
         "jpeg_soft.cpp"
    INCLUDE_DIRS "." "eez_generated"
    REQUIRES microlink nvs_flash esp_netif esp_event driver esp_wifi esp_http_server esp_timer lvgl
)

# 480x800 is the largest buffer this board's 2 MB of PSRAM can hold next to microlink's netmap buffers.
target_compile_definitions(${COMPONENT_LIB} PRIVATE RVDP_WIDTH=480 RVDP_HEIGHT=800 RVDP_STRIP_ROWS=8)
