#pragma once

#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Nordic UART Service (6E400001-...) carrying the app_cmd console, so any
 * off-the-shelf "BLE UART" phone app can drive the board without Wi-Fi.
 *   RX 6E400002 - write, phone -> board, one command per write
 *   TX 6E400003 - notify, board -> phone, reply text
 *
 * Compiled out when CONFIG_APP_BLE_ENABLED is off; the stubs then succeed
 * and report "not connected". */
esp_err_t app_ble_init(void);

bool app_ble_enabled(void);
bool app_ble_connected(void);

/* Pushes unsolicited text to the subscriber, chunked to the negotiated MTU.
 * A no-op when nobody is subscribed. */
void app_ble_notify(const char *text, size_t len);

/* Advertised name, or "" when BLE is disabled. */
const char *app_ble_device_name(void);

#ifdef __cplusplus
}
#endif
