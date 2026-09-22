#include "app_gpio.h"

#include <string.h>

#include "driver/gpio.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "nvs.h"
#include "sdkconfig.h"

static const char *TAG = "gpio";

#define NVS_NAMESPACE "appgpio"
#define NVS_KEY       "pins"

typedef struct {
    int8_t  pin;     /* -1 when the slot is free */
    uint8_t mode;
    uint8_t level;   /* last level written to an output */
} pin_entry_t;

static pin_entry_t       s_pins[APP_GPIO_MAX_PINS];
static SemaphoreHandle_t s_lock;

/* ------------------------------------------------------------------ */
/* Reserved pins                                                       */
/* ------------------------------------------------------------------ */

static const struct {
    int         pin;
    const char *why;
} k_reserved[] = {
    { CONFIG_APP_CAM_PIN_PWDN,  "camera PWDN"  },
    { CONFIG_APP_CAM_PIN_RESET, "camera RESET" },
    { CONFIG_APP_CAM_PIN_XCLK,  "camera XCLK"  },
    { CONFIG_APP_CAM_PIN_SIOD,  "camera SIOD"  },
    { CONFIG_APP_CAM_PIN_SIOC,  "camera SIOC"  },
    { CONFIG_APP_CAM_PIN_D7,    "camera D7"    },
    { CONFIG_APP_CAM_PIN_D6,    "camera D6"    },
    { CONFIG_APP_CAM_PIN_D5,    "camera D5"    },
    { CONFIG_APP_CAM_PIN_D4,    "camera D4"    },
    { CONFIG_APP_CAM_PIN_D3,    "camera D3"    },
    { CONFIG_APP_CAM_PIN_D2,    "camera D2"    },
    { CONFIG_APP_CAM_PIN_D1,    "camera D1"    },
    { CONFIG_APP_CAM_PIN_D0,    "camera D0"    },
    { CONFIG_APP_CAM_PIN_VSYNC, "camera VSYNC" },
    { CONFIG_APP_CAM_PIN_HREF,  "camera HREF"  },
    { CONFIG_APP_CAM_PIN_PCLK,  "camera PCLK"  },

    { CONFIG_APP_UART_TX_GPIO,  "UART bridge TX" },
    { CONFIG_APP_UART_RX_GPIO,  "UART bridge RX" },

    { CONFIG_APP_MB_TX_GPIO,    "Modbus TX"      },
    { CONFIG_APP_MB_RX_GPIO,    "Modbus RX"      },
    { CONFIG_APP_MB_RTS_GPIO,   "Modbus DE/RE"   },

    { 19, "USB D-" },
    { 20, "USB D+" },
    { 43, "console TX" },
    { 44, "console RX" },
};

bool app_gpio_is_reserved(int pin, const char **why)
{
    /* SPI flash and octal PSRAM. Not broken out on this board, but a typo in
     * an API call must not be able to brick the running image. */
    if (pin >= 26 && pin <= 37) {
        if (why != NULL) {
            *why = "flash / PSRAM bus";
        }
        return true;
    }

    for (size_t i = 0; i < sizeof(k_reserved) / sizeof(k_reserved[0]); i++) {
        /* Disabled peripheral pins are configured as -1 and never match a
         * real pin number. */
        if (k_reserved[i].pin >= 0 && k_reserved[i].pin == pin) {
            if (why != NULL) {
                *why = k_reserved[i].why;
            }
            return true;
        }
    }
    return false;
}

/* ------------------------------------------------------------------ */
/* Table helpers (caller holds s_lock)                                 */
/* ------------------------------------------------------------------ */

static pin_entry_t *find_entry(int pin)
{
    for (int i = 0; i < APP_GPIO_MAX_PINS; i++) {
        if (s_pins[i].pin == pin) {
            return &s_pins[i];
        }
    }
    return NULL;
}

static pin_entry_t *claim_entry(int pin)
{
    pin_entry_t *e = find_entry(pin);
    if (e != NULL) {
        return e;
    }
    for (int i = 0; i < APP_GPIO_MAX_PINS; i++) {
        if (s_pins[i].pin < 0) {
            s_pins[i].pin = (int8_t)pin;
            return &s_pins[i];
        }
    }
    return NULL;
}

static esp_err_t apply_mode(int pin, app_gpio_mode_t mode)
{
    gpio_config_t cfg = {
        .pin_bit_mask = 1ULL << pin,
        .intr_type    = GPIO_INTR_DISABLE,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
    };

    switch (mode) {
    case APP_GPIO_MODE_INPUT:
        cfg.mode = GPIO_MODE_INPUT;
        break;
    case APP_GPIO_MODE_INPUT_PULLUP:
        cfg.mode = GPIO_MODE_INPUT;
        cfg.pull_up_en = GPIO_PULLUP_ENABLE;
        break;
    case APP_GPIO_MODE_INPUT_PULLDOWN:
        cfg.mode = GPIO_MODE_INPUT;
        cfg.pull_down_en = GPIO_PULLDOWN_ENABLE;
        break;
    case APP_GPIO_MODE_OUTPUT:
        /* INPUT_OUTPUT so gpio_get_level() reads the pad back rather than
         * echoing whatever we last wrote. */
        cfg.mode = GPIO_MODE_INPUT_OUTPUT;
        break;
    case APP_GPIO_MODE_OUTPUT_OD:
        cfg.mode = GPIO_MODE_INPUT_OUTPUT_OD;
        break;
    default:
        return ESP_ERR_INVALID_ARG;
    }

    return gpio_config(&cfg);
}

/* ------------------------------------------------------------------ */
/* Persistence                                                         */
/* ------------------------------------------------------------------ */

esp_err_t app_gpio_save(void)
{
    nvs_handle_t nvs;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs);
    if (err != ESP_OK) {
        return err;
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);
    err = nvs_set_blob(nvs, NVS_KEY, s_pins, sizeof(s_pins));
    xSemaphoreGive(s_lock);

    if (err == ESP_OK) {
        err = nvs_commit(nvs);
    }
    nvs_close(nvs);
    return err;
}

static void restore(void)
{
    nvs_handle_t nvs;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &nvs) != ESP_OK) {
        return;
    }

    pin_entry_t saved[APP_GPIO_MAX_PINS];
    size_t len = sizeof(saved);
    esp_err_t err = nvs_get_blob(nvs, NVS_KEY, saved, &len);
    nvs_close(nvs);

    if (err != ESP_OK || len != sizeof(saved)) {
        return;
    }

    for (int i = 0; i < APP_GPIO_MAX_PINS; i++) {
        if (saved[i].pin < 0 || saved[i].mode == APP_GPIO_MODE_DISABLED) {
            continue;
        }
        if (app_gpio_configure(saved[i].pin, saved[i].mode) != ESP_OK) {
            continue;
        }
        if (saved[i].mode == APP_GPIO_MODE_OUTPUT ||
            saved[i].mode == APP_GPIO_MODE_OUTPUT_OD) {
            app_gpio_set_level(saved[i].pin, saved[i].level);
        }
        ESP_LOGI(TAG, "restored GPIO%d as %s",
                 saved[i].pin, app_gpio_mode_to_string(saved[i].mode));
    }
}

/* ------------------------------------------------------------------ */
/* Public API                                                          */
/* ------------------------------------------------------------------ */

esp_err_t app_gpio_init(void)
{
    s_lock = xSemaphoreCreateMutex();
    if (s_lock == NULL) {
        return ESP_ERR_NO_MEM;
    }
    for (int i = 0; i < APP_GPIO_MAX_PINS; i++) {
        s_pins[i].pin = -1;
    }
    restore();
    return ESP_OK;
}

esp_err_t app_gpio_configure(int pin, app_gpio_mode_t mode)
{
    if (mode == APP_GPIO_MODE_DISABLED) {
        return app_gpio_release(pin);
    }
    if (!GPIO_IS_VALID_GPIO(pin)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (app_gpio_is_reserved(pin, NULL)) {
        return ESP_ERR_INVALID_STATE;
    }
    if ((mode == APP_GPIO_MODE_OUTPUT || mode == APP_GPIO_MODE_OUTPUT_OD) &&
        !GPIO_IS_VALID_OUTPUT_GPIO(pin)) {
        return ESP_ERR_NOT_SUPPORTED;
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);

    pin_entry_t *e = claim_entry(pin);
    if (e == NULL) {
        xSemaphoreGive(s_lock);
        return ESP_ERR_NO_MEM;
    }

    esp_err_t err = apply_mode(pin, mode);
    if (err == ESP_OK) {
        e->mode = (uint8_t)mode;
    } else if (e->mode == APP_GPIO_MODE_DISABLED) {
        e->pin = -1;   /* roll the claim back */
    }

    xSemaphoreGive(s_lock);
    return err;
}

esp_err_t app_gpio_release(int pin)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    pin_entry_t *e = find_entry(pin);
    if (e == NULL) {
        xSemaphoreGive(s_lock);
        return ESP_ERR_NOT_FOUND;
    }
    e->pin   = -1;
    e->mode  = APP_GPIO_MODE_DISABLED;
    e->level = 0;
    xSemaphoreGive(s_lock);

    return gpio_reset_pin(pin);
}

esp_err_t app_gpio_set_level(int pin, int level)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    pin_entry_t *e = find_entry(pin);
    if (e == NULL) {
        xSemaphoreGive(s_lock);
        return ESP_ERR_NOT_FOUND;
    }
    if (e->mode != APP_GPIO_MODE_OUTPUT && e->mode != APP_GPIO_MODE_OUTPUT_OD) {
        xSemaphoreGive(s_lock);
        return ESP_ERR_INVALID_STATE;
    }
    e->level = level != 0;
    xSemaphoreGive(s_lock);

    return gpio_set_level(pin, level != 0);
}

esp_err_t app_gpio_get_level(int pin, int *out_level)
{
    if (out_level == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    pin_entry_t *e = find_entry(pin);
    xSemaphoreGive(s_lock);

    if (e == NULL) {
        return ESP_ERR_NOT_FOUND;
    }
    *out_level = gpio_get_level(pin);
    return ESP_OK;
}

int app_gpio_list(app_gpio_info_t *out, int max)
{
    int n = 0;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    for (int i = 0; i < APP_GPIO_MAX_PINS && n < max; i++) {
        if (s_pins[i].pin < 0) {
            continue;
        }
        out[n].pin   = s_pins[i].pin;
        out[n].mode  = (app_gpio_mode_t)s_pins[i].mode;
        out[n].level = gpio_get_level(s_pins[i].pin);
        n++;
    }
    xSemaphoreGive(s_lock);
    return n;
}

app_gpio_mode_t app_gpio_mode_from_string(const char *s)
{
    if (s == NULL) {
        return APP_GPIO_MODE_DISABLED;
    }
    if (strcmp(s, "in") == 0 || strcmp(s, "input") == 0) {
        return APP_GPIO_MODE_INPUT;
    }
    if (strcmp(s, "inpu") == 0 || strcmp(s, "input_pullup") == 0) {
        return APP_GPIO_MODE_INPUT_PULLUP;
    }
    if (strcmp(s, "inpd") == 0 || strcmp(s, "input_pulldown") == 0) {
        return APP_GPIO_MODE_INPUT_PULLDOWN;
    }
    if (strcmp(s, "out") == 0 || strcmp(s, "output") == 0) {
        return APP_GPIO_MODE_OUTPUT;
    }
    if (strcmp(s, "od") == 0 || strcmp(s, "output_od") == 0) {
        return APP_GPIO_MODE_OUTPUT_OD;
    }
    return APP_GPIO_MODE_DISABLED;
}

const char *app_gpio_mode_to_string(app_gpio_mode_t mode)
{
    switch (mode) {
    case APP_GPIO_MODE_INPUT:          return "input";
    case APP_GPIO_MODE_INPUT_PULLUP:   return "input_pullup";
    case APP_GPIO_MODE_INPUT_PULLDOWN: return "input_pulldown";
    case APP_GPIO_MODE_OUTPUT:         return "output";
    case APP_GPIO_MODE_OUTPUT_OD:      return "output_od";
    default:                           return "disabled";
    }
}
