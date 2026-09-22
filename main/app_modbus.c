#include "app_modbus.h"

#include <string.h>

#include "driver/uart.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "mbcontroller.h"
#include "nvs.h"
#include "sdkconfig.h"

static const char *TAG = "modbus";

#define MB_PORT       ((uart_port_t)CONFIG_APP_MB_UART_PORT)
#define NVS_NAMESPACE "appmb"
#define NVS_KEY       "jobs"

static void             *s_master;
static bool              s_started;
static bool              s_polling;
static SemaphoreHandle_t s_bus_lock;    /* serialises transactions   */
static SemaphoreHandle_t s_job_lock;    /* protects the job table    */
static app_mb_job_t      s_jobs[APP_MB_MAX_JOBS];

static bool is_bitwise(uint8_t fn)
{
    return fn == APP_MB_FN_READ_COILS || fn == APP_MB_FN_READ_DISCRETE ||
           fn == APP_MB_FN_WRITE_COIL || fn == APP_MB_FN_WRITE_COILS;
}

static bool is_supported(uint8_t fn)
{
    switch (fn) {
    case APP_MB_FN_READ_COILS:
    case APP_MB_FN_READ_DISCRETE:
    case APP_MB_FN_READ_HOLDING:
    case APP_MB_FN_READ_INPUT:
    case APP_MB_FN_WRITE_COIL:
    case APP_MB_FN_WRITE_HOLDING:
    case APP_MB_FN_WRITE_COILS:
    case APP_MB_FN_WRITE_HOLDINGS:
        return true;
    default:
        return false;
    }
}

/* ------------------------------------------------------------------ */
/* Transactions                                                        */
/* ------------------------------------------------------------------ */

esp_err_t app_modbus_request(uint8_t slave, uint8_t function,
                             uint16_t start, uint16_t count,
                             const uint16_t *in, uint16_t *out, size_t out_max)
{
    if (!s_started) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!is_supported(function) || count == 0 || count > APP_MB_MAX_REGS) {
        return ESP_ERR_INVALID_ARG;
    }
    if (slave < 1 || slave > 247) {
        return ESP_ERR_INVALID_ARG;
    }

    /* The stack wants bits packed eight to a byte for coil functions and
     * plain 16-bit words for register functions. Callers deal in one word
     * per item either way. */
    uint8_t  bits[(APP_MB_MAX_REGS + 7) / 8] = { 0 };
    uint16_t regs[APP_MB_MAX_REGS]           = { 0 };
    bool     bitwise = is_bitwise(function);
    void    *payload = bitwise ? (void *)bits : (void *)regs;

    if (in != NULL) {
        if (bitwise) {
            for (uint16_t i = 0; i < count; i++) {
                if (in[i]) {
                    bits[i / 8] |= (uint8_t)(1u << (i % 8));
                }
            }
        } else {
            memcpy(regs, in, (size_t)count * sizeof(uint16_t));
        }
    }

    mb_param_request_t req = {
        .slave_addr = slave,
        .command    = function,
        .reg_start  = start,
        .reg_size   = count,
    };

    xSemaphoreTake(s_bus_lock, portMAX_DELAY);
    esp_err_t err = mbc_master_send_request(&req, payload);
    xSemaphoreGive(s_bus_lock);

    if (err == ESP_OK && out != NULL) {
        size_t n = count < out_max ? count : out_max;
        for (size_t i = 0; i < n; i++) {
            out[i] = bitwise ? (uint16_t)((bits[i / 8] >> (i % 8)) & 1u) : regs[i];
        }
    }
    return err;
}

/* ------------------------------------------------------------------ */
/* Poll loop                                                           */
/* ------------------------------------------------------------------ */

static void poll_task(void *arg)
{
    for (;;) {
        if (!s_polling) {
            vTaskDelay(pdMS_TO_TICKS(200));
            continue;
        }

        for (int i = 0; i < APP_MB_MAX_JOBS; i++) {
            /* Copy the config out before going on the wire: a REST call may
             * rewrite the table while this transaction is in flight. */
            xSemaphoreTake(s_job_lock, portMAX_DELAY);
            app_mb_job_cfg_t cfg = s_jobs[i].cfg;
            xSemaphoreGive(s_job_lock);

            if (!cfg.enabled || cfg.count == 0) {
                continue;
            }

            uint16_t buf[APP_MB_MAX_REGS];
            esp_err_t err = app_modbus_request(cfg.slave, cfg.function,
                                               cfg.start, cfg.count,
                                               NULL, buf, APP_MB_MAX_REGS);

            xSemaphoreTake(s_job_lock, portMAX_DELAY);
            /* Only publish if the slot still describes the same request. */
            if (memcmp(&s_jobs[i].cfg, &cfg, sizeof(cfg)) == 0) {
                s_jobs[i].last_err = err;
                s_jobs[i].last_ms  = esp_timer_get_time() / 1000;
                if (err == ESP_OK) {
                    memcpy(s_jobs[i].data, buf, (size_t)cfg.count * sizeof(uint16_t));
                    s_jobs[i].data_len = cfg.count;
                    s_jobs[i].ok_count++;
                } else {
                    s_jobs[i].err_count++;
                }
            }
            xSemaphoreGive(s_job_lock);

            if (err != ESP_OK) {
                ESP_LOGD(TAG, "job %d (slave %u fn %u): %s",
                         i, cfg.slave, cfg.function, esp_err_to_name(err));
            }
        }

        vTaskDelay(pdMS_TO_TICKS(CONFIG_APP_MB_POLL_PERIOD_MS));
    }
}

/* ------------------------------------------------------------------ */
/* Job table                                                           */
/* ------------------------------------------------------------------ */

esp_err_t app_modbus_set_job(int index, const app_mb_job_cfg_t *cfg)
{
    if (index < 0 || index >= APP_MB_MAX_JOBS || cfg == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    /* The poll loop only reads; writing on a timer is a foot-gun on a live
     * plant bus. Writes go through app_modbus_request(). */
    if (cfg->function < APP_MB_FN_READ_COILS || cfg->function > APP_MB_FN_READ_INPUT) {
        return ESP_ERR_INVALID_ARG;
    }
    if (cfg->count == 0 || cfg->count > APP_MB_MAX_REGS) {
        return ESP_ERR_INVALID_ARG;
    }
    if (cfg->slave < 1 || cfg->slave > 247) {
        return ESP_ERR_INVALID_ARG;
    }

    xSemaphoreTake(s_job_lock, portMAX_DELAY);
    memset(&s_jobs[index], 0, sizeof(s_jobs[index]));
    s_jobs[index].cfg = *cfg;
    s_jobs[index].cfg.name[sizeof(s_jobs[index].cfg.name) - 1] = '\0';
    xSemaphoreGive(s_job_lock);

    return ESP_OK;
}

esp_err_t app_modbus_clear_job(int index)
{
    if (index < 0 || index >= APP_MB_MAX_JOBS) {
        return ESP_ERR_INVALID_ARG;
    }
    xSemaphoreTake(s_job_lock, portMAX_DELAY);
    memset(&s_jobs[index], 0, sizeof(s_jobs[index]));
    xSemaphoreGive(s_job_lock);
    return ESP_OK;
}

int app_modbus_get_jobs(app_mb_job_t *out, int max)
{
    if (out == NULL) {
        return 0;
    }
    int n = max < APP_MB_MAX_JOBS ? max : APP_MB_MAX_JOBS;
    xSemaphoreTake(s_job_lock, portMAX_DELAY);
    memcpy(out, s_jobs, (size_t)n * sizeof(app_mb_job_t));
    xSemaphoreGive(s_job_lock);
    return n;
}

esp_err_t app_modbus_save_jobs(void)
{
    app_mb_job_cfg_t cfgs[APP_MB_MAX_JOBS];

    xSemaphoreTake(s_job_lock, portMAX_DELAY);
    for (int i = 0; i < APP_MB_MAX_JOBS; i++) {
        cfgs[i] = s_jobs[i].cfg;
    }
    xSemaphoreGive(s_job_lock);

    nvs_handle_t nvs;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_set_blob(nvs, NVS_KEY, cfgs, sizeof(cfgs));
    if (err == ESP_OK) {
        err = nvs_commit(nvs);
    }
    nvs_close(nvs);
    return err;
}

static void restore_jobs(void)
{
    nvs_handle_t nvs;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &nvs) != ESP_OK) {
        return;
    }

    app_mb_job_cfg_t cfgs[APP_MB_MAX_JOBS];
    size_t len = sizeof(cfgs);
    esp_err_t err = nvs_get_blob(nvs, NVS_KEY, cfgs, &len);
    nvs_close(nvs);

    if (err != ESP_OK || len != sizeof(cfgs)) {
        return;
    }
    for (int i = 0; i < APP_MB_MAX_JOBS; i++) {
        if (cfgs[i].enabled) {
            app_modbus_set_job(i, &cfgs[i]);
        }
    }
    ESP_LOGI(TAG, "poll table restored from NVS");
}

void app_modbus_set_polling(bool enabled)
{
    s_polling = enabled;
}

bool app_modbus_polling(void)
{
    return s_polling;
}

/* ------------------------------------------------------------------ */
/* Bring-up                                                            */
/* ------------------------------------------------------------------ */

static uart_parity_t configured_parity(void)
{
#if defined(CONFIG_APP_MB_PARITY_EVEN)
    return UART_PARITY_EVEN;
#elif defined(CONFIG_APP_MB_PARITY_ODD)
    return UART_PARITY_ODD;
#else
    return UART_PARITY_DISABLE;
#endif
}

esp_err_t app_modbus_init(void)
{
    s_bus_lock = xSemaphoreCreateMutex();
    s_job_lock = xSemaphoreCreateMutex();
    if (s_bus_lock == NULL || s_job_lock == NULL) {
        return ESP_ERR_NO_MEM;
    }

    esp_err_t err = mbc_master_init(MB_PORT_SERIAL_MASTER, &s_master);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "mbc_master_init: %s", esp_err_to_name(err));
        return err;
    }

    mb_communication_info_t comm = { 0 };
    comm.mode     = MB_MODE_RTU;
    comm.port     = MB_PORT;
    comm.baudrate = CONFIG_APP_MB_BAUD;
    comm.parity   = configured_parity();

    err = mbc_master_setup((void *)&comm);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "mbc_master_setup: %s", esp_err_to_name(err));
        return err;
    }

    /* Pins have to be assigned after setup(): that is when the controller
     * installs the UART driver this call reconfigures. */
    err = uart_set_pin(MB_PORT,
                       CONFIG_APP_MB_TX_GPIO,
                       CONFIG_APP_MB_RX_GPIO,
                       CONFIG_APP_MB_RTS_GPIO,
                       UART_PIN_NO_CHANGE);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "uart_set_pin: %s", esp_err_to_name(err));
        return err;
    }

    err = mbc_master_start();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "mbc_master_start: %s", esp_err_to_name(err));
        return err;
    }

    /* Half duplex drives DE/RE from RTS automatically. Without a transceiver
     * (plain 3V3 TTL) stay in plain UART mode so RTS is left alone. */
#if CONFIG_APP_MB_RTS_GPIO >= 0
    err = uart_set_mode(MB_PORT, UART_MODE_RS485_HALF_DUPLEX);
#else
    err = uart_set_mode(MB_PORT, UART_MODE_UART);
#endif
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "uart_set_mode: %s", esp_err_to_name(err));
        return err;
    }

    s_started = true;
    restore_jobs();

#if CONFIG_APP_MB_AUTOSTART_POLL
    s_polling = true;
#endif

    if (xTaskCreate(poll_task, "mb_poll", 4096, NULL, 5, NULL) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "RTU master on UART%d, TX=%d RX=%d DE/RE=%d, %d baud",
             CONFIG_APP_MB_UART_PORT, CONFIG_APP_MB_TX_GPIO,
             CONFIG_APP_MB_RX_GPIO, CONFIG_APP_MB_RTS_GPIO, CONFIG_APP_MB_BAUD);
    return ESP_OK;
}
