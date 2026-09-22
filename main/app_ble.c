#include "app_ble.h"

#include <stdlib.h>
#include <string.h>

#include "esp_log.h"
#include "sdkconfig.h"

#if CONFIG_APP_BLE_ENABLED

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#include "host/ble_hs.h"
#include "host/util/util.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"

#include "app_cmd.h"

static const char *TAG = "ble";

#define CMD_LINE_MAX 200
#define REPLY_MAX    1024

/* Nordic UART Service. Stored little-endian, which is why the bytes read
 * backwards compared with the 6E400001-B5A3-F393-E0A9-E50E24DCCA9E form. */
static const ble_uuid128_t s_svc_uuid = BLE_UUID128_INIT(
    0x9e, 0xca, 0xdc, 0x24, 0x0e, 0xe5, 0xa9, 0xe0,
    0x93, 0xf3, 0xa3, 0xb5, 0x01, 0x00, 0x40, 0x6e);

static const ble_uuid128_t s_rx_uuid = BLE_UUID128_INIT(
    0x9e, 0xca, 0xdc, 0x24, 0x0e, 0xe5, 0xa9, 0xe0,
    0x93, 0xf3, 0xa3, 0xb5, 0x02, 0x00, 0x40, 0x6e);

static const ble_uuid128_t s_tx_uuid = BLE_UUID128_INIT(
    0x9e, 0xca, 0xdc, 0x24, 0x0e, 0xe5, 0xa9, 0xe0,
    0x93, 0xf3, 0xa3, 0xb5, 0x03, 0x00, 0x40, 0x6e);

static uint8_t        s_own_addr_type;
static uint16_t       s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
static uint16_t       s_tx_val_handle;
static bool           s_subscribed;
static QueueHandle_t  s_cmd_queue;

static void ble_advertise(void);

/* ------------------------------------------------------------------ */
/* GATT                                                                */
/* ------------------------------------------------------------------ */

static int gatt_rx_cb(uint16_t conn_handle, uint16_t attr_handle,
                      struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    if (ctxt->op != BLE_GATT_ACCESS_OP_WRITE_CHR) {
        return BLE_ATT_ERR_UNLIKELY;
    }

    uint16_t len = OS_MBUF_PKTLEN(ctxt->om);
    if (len == 0 || len >= CMD_LINE_MAX) {
        return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
    }

    char line[CMD_LINE_MAX];
    uint16_t copied = 0;
    if (ble_hs_mbuf_to_flat(ctxt->om, line, sizeof(line) - 1, &copied) != 0) {
        return BLE_ATT_ERR_UNLIKELY;
    }
    line[copied] = '\0';

    /* Commands can block on the Modbus bus for a couple of hundred
     * milliseconds. The NimBLE host task must not be the one waiting. */
    if (xQueueSend(s_cmd_queue, line, 0) != pdTRUE) {
        return BLE_ATT_ERR_INSUFFICIENT_RES;
    }
    return 0;
}

static int gatt_tx_cb(uint16_t conn_handle, uint16_t attr_handle,
                      struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    /* Notify-only; a read gets nothing useful. */
    return BLE_ATT_ERR_READ_NOT_PERMITTED;
}

static const struct ble_gatt_svc_def s_gatt_svcs[] = {
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = &s_svc_uuid.u,
        .characteristics = (struct ble_gatt_chr_def[]){
            {
                .uuid      = &s_rx_uuid.u,
                .access_cb = gatt_rx_cb,
                .flags     = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_NO_RSP,
            },
            {
                .uuid       = &s_tx_uuid.u,
                .access_cb  = gatt_tx_cb,
                .val_handle = &s_tx_val_handle,
                .flags      = BLE_GATT_CHR_F_NOTIFY,
            },
            { 0 },
        },
    },
    { 0 },
};

/* ------------------------------------------------------------------ */
/* GAP                                                                 */
/* ------------------------------------------------------------------ */

static int gap_event(struct ble_gap_event *event, void *arg)
{
    switch (event->type) {
    case BLE_GAP_EVENT_CONNECT:
        if (event->connect.status == 0) {
            s_conn_handle = event->connect.conn_handle;
            ESP_LOGI(TAG, "connected (handle %u)", s_conn_handle);
        } else {
            ESP_LOGW(TAG, "connect failed (%d)", event->connect.status);
            ble_advertise();
        }
        break;

    case BLE_GAP_EVENT_DISCONNECT:
        ESP_LOGI(TAG, "disconnected (reason %d)", event->disconnect.reason);
        s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
        s_subscribed  = false;
        ble_advertise();
        break;

    case BLE_GAP_EVENT_SUBSCRIBE:
        if (event->subscribe.attr_handle == s_tx_val_handle) {
            s_subscribed = event->subscribe.cur_notify;
            ESP_LOGI(TAG, "notifications %s", s_subscribed ? "on" : "off");
        }
        break;

    case BLE_GAP_EVENT_MTU:
        ESP_LOGI(TAG, "MTU now %d", event->mtu.value);
        break;

    case BLE_GAP_EVENT_ADV_COMPLETE:
        ble_advertise();
        break;

    default:
        break;
    }
    return 0;
}

static void ble_advertise(void)
{
    struct ble_hs_adv_fields fields = { 0 };
    fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    fields.tx_pwr_lvl_is_present = 1;
    fields.tx_pwr_lvl = BLE_HS_ADV_TX_PWR_LVL_AUTO;

    const char *name = ble_svc_gap_device_name();
    fields.name             = (uint8_t *)name;
    fields.name_len         = strlen(name);
    fields.name_is_complete = 1;

    int rc = ble_gap_adv_set_fields(&fields);
    if (rc != 0) {
        ESP_LOGE(TAG, "adv_set_fields: %d", rc);
        return;
    }

    /* A 128-bit UUID plus the name overflows the 31-byte advertisement, so
     * the service UUID rides in the scan response instead. */
    struct ble_hs_adv_fields rsp = { 0 };
    rsp.uuids128             = (ble_uuid128_t *)&s_svc_uuid;
    rsp.num_uuids128         = 1;
    rsp.uuids128_is_complete = 1;
    ble_gap_adv_rsp_set_fields(&rsp);

    struct ble_gap_adv_params adv_params = {
        .conn_mode = BLE_GAP_CONN_MODE_UND,
        .disc_mode = BLE_GAP_DISC_MODE_GEN,
    };

    rc = ble_gap_adv_start(s_own_addr_type, NULL, BLE_HS_FOREVER,
                           &adv_params, gap_event, NULL);
    if (rc != 0 && rc != BLE_HS_EALREADY) {
        ESP_LOGE(TAG, "adv_start: %d", rc);
    }
}

static void on_sync(void)
{
    ble_hs_util_ensure_addr(0);
    int rc = ble_hs_id_infer_auto(0, &s_own_addr_type);
    if (rc != 0) {
        ESP_LOGE(TAG, "id_infer_auto: %d", rc);
        return;
    }
    ESP_LOGI(TAG, "stack synced, advertising as \"%s\"", ble_svc_gap_device_name());
    ble_advertise();
}

static void on_reset(int reason)
{
    ESP_LOGW(TAG, "stack reset, reason %d", reason);
    s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
    s_subscribed  = false;
}

static void ble_host_task(void *param)
{
    nimble_port_run();
    nimble_port_freertos_deinit();
}

/* ------------------------------------------------------------------ */
/* Command worker                                                      */
/* ------------------------------------------------------------------ */

static void cmd_task(void *arg)
{
    char line[CMD_LINE_MAX];
    char *reply = malloc(REPLY_MAX);
    if (reply == NULL) {
        ESP_LOGE(TAG, "no memory for the reply buffer");
        vTaskDelete(NULL);
        return;
    }

    for (;;) {
        if (xQueueReceive(s_cmd_queue, line, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        size_t n = app_cmd_execute(line, reply, REPLY_MAX);
        if (n > 0) {
            app_ble_notify(reply, n);
        }
    }
}

/* ------------------------------------------------------------------ */
/* Public API                                                          */
/* ------------------------------------------------------------------ */

void app_ble_notify(const char *text, size_t len)
{
    if (!s_subscribed || s_conn_handle == BLE_HS_CONN_HANDLE_NONE ||
        text == NULL || len == 0) {
        return;
    }

    /* Three bytes of every packet go to the ATT notification header. */
    uint16_t mtu = ble_att_mtu(s_conn_handle);
    size_t chunk = (mtu > 3) ? (size_t)(mtu - 3) : 20;

    while (len > 0) {
        size_t n = len < chunk ? len : chunk;
        struct os_mbuf *om = ble_hs_mbuf_from_flat(text, n);
        if (om == NULL) {
            /* Out of mbufs; the client will have to ask again. */
            return;
        }
        if (ble_gattc_notify_custom(s_conn_handle, s_tx_val_handle, om) != 0) {
            return;
        }
        text += n;
        len  -= n;
    }
}

bool app_ble_enabled(void)
{
    return true;
}

bool app_ble_connected(void)
{
    return s_conn_handle != BLE_HS_CONN_HANDLE_NONE;
}

const char *app_ble_device_name(void)
{
    return CONFIG_APP_BLE_DEVICE_NAME;
}

esp_err_t app_ble_init(void)
{
    s_cmd_queue = xQueueCreate(4, CMD_LINE_MAX);
    if (s_cmd_queue == NULL) {
        return ESP_ERR_NO_MEM;
    }

    esp_err_t err = nimble_port_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nimble_port_init: %s", esp_err_to_name(err));
        return err;
    }

    ble_hs_cfg.sync_cb  = on_sync;
    ble_hs_cfg.reset_cb = on_reset;

    ble_svc_gap_init();
    ble_svc_gatt_init();

    int rc = ble_gatts_count_cfg(s_gatt_svcs);
    if (rc == 0) {
        rc = ble_gatts_add_svcs(s_gatt_svcs);
    }
    if (rc != 0) {
        ESP_LOGE(TAG, "GATT registration failed: %d", rc);
        return ESP_FAIL;
    }

    rc = ble_svc_gap_device_name_set(CONFIG_APP_BLE_DEVICE_NAME);
    if (rc != 0) {
        ESP_LOGW(TAG, "device_name_set: %d", rc);
    }

    if (xTaskCreate(cmd_task, "ble_cmd", 5120, NULL, 5, NULL) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }

    nimble_port_freertos_init(ble_host_task);
    return ESP_OK;
}

#else  /* !CONFIG_APP_BLE_ENABLED */

esp_err_t app_ble_init(void)            { return ESP_OK; }
bool      app_ble_enabled(void)         { return false; }
bool      app_ble_connected(void)       { return false; }
void      app_ble_notify(const char *text, size_t len) { (void)text; (void)len; }
const char *app_ble_device_name(void)   { return ""; }

#endif /* CONFIG_APP_BLE_ENABLED */
