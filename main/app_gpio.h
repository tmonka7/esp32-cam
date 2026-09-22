#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define APP_GPIO_MAX_PINS 12

typedef enum {
    APP_GPIO_MODE_DISABLED = 0,
    APP_GPIO_MODE_INPUT,
    APP_GPIO_MODE_INPUT_PULLUP,
    APP_GPIO_MODE_INPUT_PULLDOWN,
    APP_GPIO_MODE_OUTPUT,
    APP_GPIO_MODE_OUTPUT_OD,
} app_gpio_mode_t;

typedef struct {
    int             pin;
    app_gpio_mode_t mode;
    int             level;   /* live level, read back from the pad */
} app_gpio_info_t;

/* Restores any pin configuration saved by app_gpio_save(). */
esp_err_t app_gpio_init(void);

/* True when the pin is spoken for by the camera, a UART, the flash/PSRAM
 * bus, USB or the console. *why gets a short human-readable reason. */
bool app_gpio_is_reserved(int pin, const char **why);

/* ESP_ERR_INVALID_ARG      - not a GPIO on this chip
 * ESP_ERR_INVALID_STATE    - reserved by a peripheral
 * ESP_ERR_NOT_SUPPORTED    - output requested on an input-only pad
 * ESP_ERR_NO_MEM           - APP_GPIO_MAX_PINS already claimed */
esp_err_t app_gpio_configure(int pin, app_gpio_mode_t mode);

/* Returns the pin to its reset state and drops it from the table. */
esp_err_t app_gpio_release(int pin);

esp_err_t app_gpio_set_level(int pin, int level);
esp_err_t app_gpio_get_level(int pin, int *out_level);

/* Fills up to max entries; returns how many pins are configured. */
int app_gpio_list(app_gpio_info_t *out, int max);

/* Persists the current table to NVS so it survives a power cycle. */
esp_err_t app_gpio_save(void);

app_gpio_mode_t app_gpio_mode_from_string(const char *s);
const char     *app_gpio_mode_to_string(app_gpio_mode_t mode);

#ifdef __cplusplus
}
#endif
