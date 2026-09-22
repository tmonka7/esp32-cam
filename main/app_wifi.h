#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    bool    connected;      /* station has an IP                         */
    bool    ap_active;      /* SoftAP fallback is running                */
    char    ssid[33];       /* station SSID being used                   */
    char    ip[16];         /* station IP, or "0.0.0.0"                  */
    char    ap_ip[16];      /* SoftAP IP when ap_active                  */
    int8_t  rssi;           /* station RSSI, 0 when not connected        */
} app_wifi_status_t;

/* Brings up the station interface and, if it cannot associate within
 * CONFIG_APP_WIFI_MAX_RETRY attempts, starts the fallback SoftAP. Retries
 * continue in the background either way, so a router coming back up is
 * picked up without a reboot. Never blocks for the association. */
esp_err_t app_wifi_init(void);

void app_wifi_get_status(app_wifi_status_t *out);

/* Stores credentials in NVS. They take effect on the next boot; the caller
 * decides whether to restart. */
esp_err_t app_wifi_set_credentials(const char *ssid, const char *password);

#ifdef __cplusplus
}
#endif
