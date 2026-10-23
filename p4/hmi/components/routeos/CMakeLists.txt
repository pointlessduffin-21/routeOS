idf_component_register(
    SRCS ros_core.cpp ros_wifi.cpp ros_ota.cpp
    INCLUDE_DIRS include .
    REQUIRES esp_http_server esp_http_client esp-tls mbedtls esp_wifi esp_netif esp_event nvs_flash json esp_timer esp_psram app_update esp_partition spi_flash esp_app_format esp_system heap
    PRIV_REQUIRES esp_hw_support)