idf_component_register(
    SRCS "main.cpp"
    INCLUDE_DIRS "."
    REQUIRES
        app_bsp
        port_bsp
        lvgl__lvgl
        nvs_flash
        esp_wifi
        esp_netif
        esp_event
        esp_http_client
        esp_http_server
        esp_timer
        json
        mbedtls)
