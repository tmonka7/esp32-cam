#include "app_uart.h"

#include <inttypes.h>
#include <string.h>

#include "driver/uart.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "sdkconfig.h"

static const char *TAG = "uart";

#define UART_PORT      ((uart_port_t)CONFIG_APP_UART_PORT_NUM)
#define RING_SIZE      CONFIG_APP_UART_RX_BUF
#define DRIVER_RX_SIZE (RING_SIZE < 2048 ? 2048 : RING_SIZE)

/* Bytes that arrived but have not been collected through /api/uart yet. The
 * newest data matters most on a debug bridge, so a full buffer drops from the
 * front rather than refusing new bytes. */
static uint8_t s_ring[RING_SIZE];
static size_t  s_head;    /* next write index                 */
static size_t  s_count;   /* bytes currently held             */

static SemaphoreHandle_t s_lock;
static uint32_t          s_baud = CONFIG_APP_UART_BAUD;
static uint64_t          s_total_rx;
static uint64_t          s_total_tx;
static uint64_t          s_dropped;

static app_uart_rx_cb_t  s_cb;
static void             *s_cb_ctx;

static void ring_push(const uint8_t *data, size_t len)
{
    if (len >= RING_SIZE) {
        /* Only the tail can possibly survive. */
        data += len - RING_SIZE;
        s_dropped += len - RING_SIZE;
        len = RING_SIZE;
        s_count = 0;
        s_head = 0;
    }

    for (size_t i = 0; i < len; i++) {
        s_ring[s_head] = data[i];
        s_head = (s_head + 1) % RING_SIZE;
        if (s_count < RING_SIZE) {
            s_count++;
        } else {
            s_dropped++;   /* overwrote the oldest unread byte */
        }
    }
}

static void uart_rx_task(void *arg)
{
    uint8_t buf[256];

    for (;;) {
        int n = uart_read_bytes(UART_PORT, buf, sizeof(buf), pdMS_TO_TICKS(50));
        if (n <= 0) {
            continue;
        }

        xSemaphoreTake(s_lock, portMAX_DELAY);
        ring_push(buf, (size_t)n);
        s_total_rx += (uint64_t)n;
        app_uart_rx_cb_t cb = s_cb;
        void *ctx = s_cb_ctx;
        xSemaphoreGive(s_lock);

        if (cb != NULL) {
            cb(buf, (size_t)n, ctx);
        }
    }
}

esp_err_t app_uart_init(void)
{
    s_lock = xSemaphoreCreateMutex();
    if (s_lock == NULL) {
        return ESP_ERR_NO_MEM;
    }

    const uart_config_t cfg = {
        .baud_rate = CONFIG_APP_UART_BAUD,
        .data_bits = UART_DATA_8_BITS,
        .parity    = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };

    esp_err_t err = uart_driver_install(UART_PORT, DRIVER_RX_SIZE, DRIVER_RX_SIZE, 0, NULL, 0);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "uart_driver_install: %s", esp_err_to_name(err));
        return err;
    }
    err = uart_param_config(UART_PORT, &cfg);
    if (err != ESP_OK) {
        return err;
    }
    err = uart_set_pin(UART_PORT,
                       CONFIG_APP_UART_TX_GPIO,
                       CONFIG_APP_UART_RX_GPIO,
                       UART_PIN_NO_CHANGE,
                       UART_PIN_NO_CHANGE);
    if (err != ESP_OK) {
        return err;
    }

    if (xTaskCreate(uart_rx_task, "uart_rx", 3072, NULL, 9, NULL) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "bridge on UART%d, TX=%d RX=%d, %" PRIu32 " baud",
             CONFIG_APP_UART_PORT_NUM, CONFIG_APP_UART_TX_GPIO,
             CONFIG_APP_UART_RX_GPIO, s_baud);
    return ESP_OK;
}

esp_err_t app_uart_write(const uint8_t *data, size_t len)
{
    if (data == NULL || len == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    int written = uart_write_bytes(UART_PORT, (const char *)data, len);
    if (written < 0) {
        return ESP_FAIL;
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_total_tx += (uint64_t)written;
    xSemaphoreGive(s_lock);

    return (size_t)written == len ? ESP_OK : ESP_ERR_TIMEOUT;
}

size_t app_uart_read(uint8_t *out, size_t max)
{
    if (out == NULL || max == 0) {
        return 0;
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);

    size_t n = s_count < max ? s_count : max;
    /* Oldest byte sits s_count positions behind the write head. */
    size_t tail = (s_head + RING_SIZE - s_count) % RING_SIZE;
    for (size_t i = 0; i < n; i++) {
        out[i] = s_ring[(tail + i) % RING_SIZE];
    }
    s_count -= n;

    xSemaphoreGive(s_lock);
    return n;
}

void app_uart_flush(void)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_count = 0;
    s_head  = 0;
    xSemaphoreGive(s_lock);
    uart_flush_input(UART_PORT);
}

esp_err_t app_uart_set_baud(uint32_t baud)
{
    if (baud < 300 || baud > 5000000) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t err = uart_set_baudrate(UART_PORT, baud);
    if (err == ESP_OK) {
        s_baud = baud;
    }
    return err;
}

void app_uart_get_stats(app_uart_stats_t *out)
{
    if (out == NULL) {
        return;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    out->baud     = s_baud;
    out->buffered = s_count;
    out->total_rx = s_total_rx;
    out->total_tx = s_total_tx;
    out->dropped  = s_dropped;
    xSemaphoreGive(s_lock);
}

void app_uart_set_rx_callback(app_uart_rx_cb_t cb, void *ctx)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_cb     = cb;
    s_cb_ctx = ctx;
    xSemaphoreGive(s_lock);
}
