#include "app_wifi.h"

#include <string.h>

#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs.h"
#include "sdkconfig.h"

static const char *TAG = "wifi";

#define NVS_NAMESPACE "wifi"

static esp_netif_t *s_sta_netif;
static esp_netif_t *s_ap_netif;

static int      s_retry;
static bool     s_connected;
static bool     s_ap_active;
static char     s_ip[16] = "0.0.0.0";
static char     s_ssid[33];
static esp_timer_handle_t s_retry_timer;

static void start_softap(void);

/* ------------------------------------------------------------------ */
/* Credentials                                                         */
/* ------------------------------------------------------------------ */

static void load_credentials(char *ssid, size_t ssid_len, char *pass, size_t pass_len)
{
    /* Kconfig is the fallback; anything stored in NVS wins so the board can
     * be re-provisioned in the field over BLE. */
    strlcpy(ssid, CONFIG_APP_WIFI_SSID, ssid_len);
    strlcpy(pass, CONFIG_APP_WIFI_PASSWORD, pass_len);

    nvs_handle_t nvs;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &nvs) != ESP_OK) {
        return;
    }

    size_t len = ssid_len;
    char   tmp[64];
    if (nvs_get_str(nvs, "ssid", tmp, &len) == ESP_OK && tmp[0] != '\0') {
        strlcpy(ssid, tmp, ssid_len);
        len = sizeof(tmp);
        if (nvs_get_str(nvs, "pass", tmp, &len) == ESP_OK) {
            strlcpy(pass, tmp, pass_len);
        } else {
            pass[0] = '\0';
        }
        ESP_LOGI(TAG, "using credentials from NVS");
    }
    nvs_close(nvs);
}

esp_err_t app_wifi_set_credentials(const char *ssid, const char *password)
{
    if (ssid == NULL || ssid[0] == '\0' || strlen(ssid) > 32) {
        return ESP_ERR_INVALID_ARG;
    }
    if (password != NULL && strlen(password) > 63) {
        return ESP_ERR_INVALID_ARG;
    }

    nvs_handle_t nvs;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_set_str(nvs, "ssid", ssid);
    if (err == ESP_OK) {
        err = nvs_set_str(nvs, "pass", password != NULL ? password : "");
    }
    if (err == ESP_OK) {
        err = nvs_commit(nvs);
    }
    nvs_close(nvs);
    return err;
}

/* ------------------------------------------------------------------ */
/* Events                                                              */
/* ------------------------------------------------------------------ */

static void on_wifi_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    switch (id) {
    case WIFI_EVENT_STA_START:
        esp_wifi_connect();
        break;

    case WIFI_EVENT_STA_DISCONNECTED: {
        wifi_event_sta_disconnected_t *ev = data;
        s_connected = false;
        strcpy(s_ip, "0.0.0.0");

        if (s_retry < CONFIG_APP_WIFI_MAX_RETRY) {
            s_retry++;
            ESP_LOGW(TAG, "disconnected (reason %d), retry %d/%d",
                     ev->reason, s_retry, CONFIG_APP_WIFI_MAX_RETRY);
            esp_wifi_connect();
        } else {
            /* Give up on the burst of fast retries and hand the user a SoftAP
             * to talk to. The periodic timer keeps trying in the background. */
            ESP_LOGW(TAG, "station association failed, falling back to SoftAP");
            start_softap();
        }
        break;
    }

    case WIFI_EVENT_AP_STACONNECTED:
        ESP_LOGI(TAG, "client joined the fallback AP");
        break;

    default:
        break;
    }
}

static void on_got_ip(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    ip_event_got_ip_t *ev = data;
    snprintf(s_ip, sizeof(s_ip), IPSTR, IP2STR(&ev->ip_info.ip));
    s_retry = 0;
    s_connected = true;
    ESP_LOGI(TAG, "connected, http://%s/", s_ip);
}

static void retry_timer_cb(void *arg)
{
    if (s_connected) {
        return;
    }
    /* Reset the counter so the next failure burst also gets MAX_RETRY tries. */
    s_retry = 0;
    ESP_LOGI(TAG, "background reconnect attempt");
    esp_wifi_connect();
}

/* ------------------------------------------------------------------ */
/* SoftAP fallback                                                     */
/* ------------------------------------------------------------------ */

static void start_softap(void)
{
    if (s_ap_active) {
        return;
    }

    wifi_config_t ap = { 0 };
    strlcpy((char *)ap.ap.ssid, CONFIG_APP_AP_SSID, sizeof(ap.ap.ssid));
    ap.ap.ssid_len       = strlen(CONFIG_APP_AP_SSID);
    ap.ap.channel        = 1;
    ap.ap.max_connection = 4;

    const char *pass = CONFIG_APP_AP_PASSWORD;
    if (strlen(pass) >= 8) {
        strlcpy((char *)ap.ap.password, pass, sizeof(ap.ap.password));
        ap.ap.authmode = WIFI_AUTH_WPA2_PSK;
    } else {
        ap.ap.authmode = WIFI_AUTH_OPEN;
    }

    /* APSTA rather than AP: the station side must stay alive for the
     * background reconnect timer to have anything to do. */
    esp_err_t err = esp_wifi_set_mode(WIFI_MODE_APSTA);
    if (err == ESP_OK) {
        err = esp_wifi_set_config(WIFI_IF_AP, &ap);
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "SoftAP start failed: %s", esp_err_to_name(err));
        return;
    }

    s_ap_active = true;
    ESP_LOGI(TAG, "SoftAP \"%s\" up", CONFIG_APP_AP_SSID);
}

/* ------------------------------------------------------------------ */
/* Public API                                                          */
/* ------------------------------------------------------------------ */

esp_err_t app_wifi_init(void)
{
    ESP_ERROR_CHECK(esp_netif_init());

    s_sta_netif = esp_netif_create_default_wifi_sta();
    s_ap_netif  = esp_netif_create_default_wifi_ap();
    if (s_sta_netif == NULL || s_ap_netif == NULL) {
        return ESP_ERR_NO_MEM;
    }

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, on_wifi_event, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, on_got_ip, NULL, NULL));

    char pass[64];
    load_credentials(s_ssid, sizeof(s_ssid), pass, sizeof(pass));

    wifi_config_t sta = { 0 };
    strlcpy((char *)sta.sta.ssid, s_ssid, sizeof(sta.sta.ssid));
    strlcpy((char *)sta.sta.password, pass, sizeof(sta.sta.password));
    sta.sta.threshold.authmode = pass[0] != '\0' ? WIFI_AUTH_WPA_PSK : WIFI_AUTH_OPEN;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &sta));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    ESP_ERROR_CHECK(esp_wifi_start());

    /* Power save costs the MJPEG stream several frames per second. */
    esp_wifi_set_ps(WIFI_PS_NONE);

    const esp_timer_create_args_t timer_args = {
        .callback = retry_timer_cb,
        .name     = "wifi_retry",
    };
    ESP_ERROR_CHECK(esp_timer_create(&timer_args, &s_retry_timer));
    ESP_ERROR_CHECK(esp_timer_start_periodic(
        s_retry_timer, (uint64_t)CONFIG_APP_WIFI_RETRY_PERIOD_S * 1000000ULL));

    ESP_LOGI(TAG, "joining \"%s\"", s_ssid);
    return ESP_OK;
}

void app_wifi_get_status(app_wifi_status_t *out)
{
    if (out == NULL) {
        return;
    }
    memset(out, 0, sizeof(*out));

    out->connected = s_connected;
    out->ap_active = s_ap_active;
    strlcpy(out->ssid, s_ssid, sizeof(out->ssid));
    strlcpy(out->ip, s_ip, sizeof(out->ip));
    strcpy(out->ap_ip, "0.0.0.0");

    if (s_connected) {
        wifi_ap_record_t rec;
        if (esp_wifi_sta_get_ap_info(&rec) == ESP_OK) {
            out->rssi = rec.rssi;
        }
    }

    if (s_ap_active && s_ap_netif != NULL) {
        esp_netif_ip_info_t info;
        if (esp_netif_get_ip_info(s_ap_netif, &info) == ESP_OK) {
            snprintf(out->ap_ip, sizeof(out->ap_ip), IPSTR, IP2STR(&info.ip));
        }
    }
}
