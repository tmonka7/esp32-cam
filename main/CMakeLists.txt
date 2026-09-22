idf_component_register(
    SRCS
        "main.c"
        "app_wifi.c"
        "app_camera.c"
        "app_gpio.c"
        "app_uart.c"
        "app_modbus.c"
        "app_cmd.c"
        "app_ble.c"
        "app_httpd.c"
    INCLUDE_DIRS "."
    EMBED_FILES "www/index.html"
    REQUIRES
        esp_wifi
        esp_netif
        esp_event
        esp_timer
        esp_psram
        esp_http_server
        nvs_flash
        driver
        json
        bt
)
