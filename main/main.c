#include "esp_event.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_psram.h"
#include "esp_system.h"
#include "nvs_flash.h"

#include "app_ble.h"
#include "app_camera.h"
#include "app_gpio.h"
#include "app_httpd.h"
#include "app_modbus.h"
#include "app_uart.h"
#include "app_wifi.h"

static const char *TAG = "main";

void app_main(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    /* Field I/O first. If Wi-Fi or the camera is broken the board should
     * still drive its outputs and talk to the bus. */
    ESP_ERROR_CHECK(app_gpio_init());

    err = app_uart_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "UART bridge unavailable: %s", esp_err_to_name(err));
    }

    err = app_modbus_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Modbus master unavailable: %s", esp_err_to_name(err));
    }

    err = app_camera_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "camera unavailable: %s (check the ribbon cable)",
                 esp_err_to_name(err));
    }

    err = app_ble_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "BLE unavailable: %s", esp_err_to_name(err));
    }

    ESP_ERROR_CHECK(app_wifi_init());
    ESP_ERROR_CHECK(app_httpd_start());

    ESP_LOGI(TAG, "up: heap %u (internal %u), psram %u",
             (unsigned)esp_get_free_heap_size(),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)(esp_psram_is_initialized() ? esp_psram_get_size() : 0));
}
