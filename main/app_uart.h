#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Callback for every chunk of bytes that arrives on the bridge. Runs on the
 * UART RX task, so it must not block for long. */
typedef void (*app_uart_rx_cb_t)(const uint8_t *data, size_t len, void *ctx);

typedef struct {
    uint32_t baud;
    size_t   buffered;    /* bytes waiting in the history buffer */
    uint64_t total_rx;
    uint64_t total_tx;
    uint64_t dropped;     /* oldest bytes discarded when the buffer wrapped */
} app_uart_stats_t;

esp_err_t app_uart_init(void);

esp_err_t app_uart_write(const uint8_t *data, size_t len);

/* Drains up to max bytes, oldest first. Returns the number copied. */
size_t app_uart_read(uint8_t *out, size_t max);

/* Discards everything buffered. */
void app_uart_flush(void);

esp_err_t app_uart_set_baud(uint32_t baud);

void app_uart_get_stats(app_uart_stats_t *out);

/* Only one subscriber; pass NULL to unsubscribe. */
void app_uart_set_rx_callback(app_uart_rx_cb_t cb, void *ctx);

#ifdef __cplusplus
}
#endif
