#include "app_httpd.h"

#include <inttypes.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "esp_camera.h"
#include "esp_heap_caps.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_psram.h"
#include "esp_system.h"
#include "esp_timer.h"

#include "app_ble.h"
#include "app_camera.h"
#include "app_cmd.h"
#include "app_gpio.h"
#include "app_modbus.h"
#include "app_uart.h"
#include "app_wifi.h"

static const char *TAG = "httpd";

static httpd_handle_t s_web;
static httpd_handle_t s_stream;

extern const uint8_t index_html_start[] asm("_binary_index_html_start");
extern const uint8_t index_html_end[]   asm("_binary_index_html_end");

#define PART_BOUNDARY "frameboundary"
static const char *STREAM_CONTENT_TYPE = "multipart/x-mixed-replace;boundary=" PART_BOUNDARY;
static const char *STREAM_BOUNDARY     = "\r\n--" PART_BOUNDARY "\r\n";
static const char *STREAM_PART_HEADER  = "Content-Type: image/jpeg\r\nContent-Length: %u\r\n\r\n";

#define MAX_BODY 2048

/* ------------------------------------------------------------------ */
/* Helpers                                                             */
/* ------------------------------------------------------------------ */

static void set_cors(httpd_req_t *req)
{
    /* The UI is served from :80 but talks to :81 for video, and users often
     * drive the API from a separate tool. */
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
}

/* Sends and frees the tree. Always returns an esp_err_t fit to return from a
 * handler. */
static esp_err_t send_json(httpd_req_t *req, cJSON *root)
{
    char *text = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (text == NULL) {
        return httpd_resp_send_500(req);
    }
    httpd_resp_set_type(req, "application/json");
    set_cors(req);
    esp_err_t err = httpd_resp_sendstr(req, text);
    cJSON_free(text);
    return err;
}

static esp_err_t send_error(httpd_req_t *req, const char *status, const char *message)
{
    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "ok", false);
    cJSON_AddStringToObject(root, "error", message);
    char *text = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);

    httpd_resp_set_status(req, status);
    httpd_resp_set_type(req, "application/json");
    set_cors(req);
    esp_err_t err = httpd_resp_sendstr(req, text != NULL ? text : "{\"ok\":false}");
    if (text != NULL) {
        cJSON_free(text);
    }
    return err;
}

static esp_err_t send_ok(httpd_req_t *req)
{
    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "ok", true);
    return send_json(req, root);
}

/* Reads the whole request body. Caller frees. NULL on error or oversize. */
static char *read_body(httpd_req_t *req)
{
    size_t len = req->content_len;
    if (len == 0 || len > MAX_BODY) {
        return NULL;
    }

    char *buf = malloc(len + 1);
    if (buf == NULL) {
        return NULL;
    }

    size_t received = 0;
    while (received < len) {
        int r = httpd_req_recv(req, buf + received, len - received);
        if (r <= 0) {
            free(buf);
            return NULL;
        }
        received += (size_t)r;
    }
    buf[len] = '\0';
    return buf;
}

static cJSON *read_json_body(httpd_req_t *req)
{
    char *body = read_body(req);
    if (body == NULL) {
        return NULL;
    }
    cJSON *root = cJSON_Parse(body);
    free(body);
    return root;
}

/* ------------------------------------------------------------------ */
/* Static page                                                         */
/* ------------------------------------------------------------------ */

static esp_err_t index_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html");
    httpd_resp_set_hdr(req, "Content-Encoding", "identity");
    return httpd_resp_send(req, (const char *)index_html_start,
                           index_html_end - index_html_start);
}

/* ------------------------------------------------------------------ */
/* Camera                                                              */
/* ------------------------------------------------------------------ */

static esp_err_t capture_handler(httpd_req_t *req)
{
    if (!app_camera_ready()) {
        return send_error(req, "503 Service Unavailable", "camera not initialised");
    }

    camera_fb_t *fb = esp_camera_fb_get();
    if (fb == NULL) {
        return send_error(req, "500 Internal Server Error", "frame capture failed");
    }

    httpd_resp_set_type(req, "image/jpeg");
    httpd_resp_set_hdr(req, "Content-Disposition", "inline; filename=capture.jpg");
    set_cors(req);

    esp_err_t err;
    if (fb->format == PIXFORMAT_JPEG) {
        err = httpd_resp_send(req, (const char *)fb->buf, fb->len);
        esp_camera_fb_return(fb);
    } else {
        /* Only reachable if someone reconfigures the sensor to a raw format. */
        uint8_t *jpg = NULL;
        size_t   jpg_len = 0;
        bool ok = frame2jpg(fb, 80, &jpg, &jpg_len);
        esp_camera_fb_return(fb);
        if (!ok) {
            return send_error(req, "500 Internal Server Error", "JPEG conversion failed");
        }
        err = httpd_resp_send(req, (const char *)jpg, jpg_len);
        free(jpg);
    }
    return err;
}

static esp_err_t stream_handler(httpd_req_t *req)
{
    if (!app_camera_ready()) {
        return send_error(req, "503 Service Unavailable", "camera not initialised");
    }

    esp_err_t err = httpd_resp_set_type(req, STREAM_CONTENT_TYPE);
    if (err != ESP_OK) {
        return err;
    }
    set_cors(req);
    httpd_resp_set_hdr(req, "X-Framerate", "60");

    int64_t  last_us = esp_timer_get_time();
    uint32_t frames  = 0;
    char     part[64];

    while (true) {
        camera_fb_t *fb = esp_camera_fb_get();
        if (fb == NULL) {
            ESP_LOGW(TAG, "frame capture failed, ending stream");
            break;
        }

        uint8_t *payload = fb->buf;
        size_t   length  = fb->len;
        uint8_t *converted = NULL;

        if (fb->format != PIXFORMAT_JPEG) {
            size_t jpg_len = 0;
            if (!frame2jpg(fb, 80, &converted, &jpg_len)) {
                esp_camera_fb_return(fb);
                break;
            }
            payload = converted;
            length  = jpg_len;
        }

        err = httpd_resp_send_chunk(req, STREAM_BOUNDARY, strlen(STREAM_BOUNDARY));
        if (err == ESP_OK) {
            int n = snprintf(part, sizeof(part), STREAM_PART_HEADER, (unsigned)length);
            err = httpd_resp_send_chunk(req, part, n);
        }
        if (err == ESP_OK) {
            err = httpd_resp_send_chunk(req, (const char *)payload, length);
        }

        if (converted != NULL) {
            free(converted);
        }
        esp_camera_fb_return(fb);

        if (err != ESP_OK) {
            /* Normal path when the browser closes the tab. */
            ESP_LOGI(TAG, "stream client gone");
            break;
        }

        if (++frames % 100 == 0) {
            int64_t now = esp_timer_get_time();
            ESP_LOGI(TAG, "stream %.1f fps", 100.0 * 1000000.0 / (double)(now - last_us));
            last_us = now;
        }
    }

    httpd_resp_send_chunk(req, NULL, 0);
    return ESP_OK;
}

static esp_err_t camera_get_handler(httpd_req_t *req)
{
    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "ready", app_camera_ready());
    cJSON_AddStringToObject(root, "sensor", app_camera_sensor_name());

    cJSON *settings = cJSON_AddObjectToObject(root, "settings");
    const char *const *names = app_camera_setting_names();
    for (int i = 0; names[i] != NULL; i++) {
        int value;
        if (app_camera_get_setting(names[i], &value) == ESP_OK) {
            cJSON_AddNumberToObject(settings, names[i], value);
        }
    }
    return send_json(req, root);
}

static esp_err_t camera_post_handler(httpd_req_t *req)
{
    cJSON *body = read_json_body(req);
    if (body == NULL) {
        return send_error(req, "400 Bad Request", "expected a JSON object of controls");
    }

    int applied = 0;
    cJSON *item = NULL;
    cJSON_ArrayForEach(item, body) {
        if (!cJSON_IsNumber(item) || item->string == NULL) {
            continue;
        }
        if (app_camera_set_setting(item->string, item->valueint) == ESP_OK) {
            applied++;
        } else {
            ESP_LOGW(TAG, "rejected camera control \"%s\"", item->string);
        }
    }
    cJSON_Delete(body);

    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "ok", true);
    cJSON_AddNumberToObject(root, "applied", applied);
    return send_json(req, root);
}

/* ------------------------------------------------------------------ */
/* GPIO                                                                */
/* ------------------------------------------------------------------ */

static esp_err_t gpio_get_handler(httpd_req_t *req)
{
    app_gpio_info_t pins[APP_GPIO_MAX_PINS];
    int n = app_gpio_list(pins, APP_GPIO_MAX_PINS);

    cJSON *root = cJSON_CreateObject();
    cJSON *list = cJSON_AddArrayToObject(root, "pins");
    for (int i = 0; i < n; i++) {
        cJSON *entry = cJSON_CreateObject();
        cJSON_AddNumberToObject(entry, "pin", pins[i].pin);
        cJSON_AddStringToObject(entry, "mode", app_gpio_mode_to_string(pins[i].mode));
        cJSON_AddNumberToObject(entry, "level", pins[i].level);
        cJSON_AddItemToArray(list, entry);
    }
    return send_json(req, root);
}

static esp_err_t gpio_post_handler(httpd_req_t *req)
{
    cJSON *body = read_json_body(req);
    if (body == NULL) {
        return send_error(req, "400 Bad Request", "expected JSON");
    }

    cJSON *pin_item = cJSON_GetObjectItem(body, "pin");
    if (!cJSON_IsNumber(pin_item)) {
        cJSON_Delete(body);
        return send_error(req, "400 Bad Request", "\"pin\" is required");
    }
    int pin = pin_item->valueint;

    cJSON *mode_item  = cJSON_GetObjectItem(body, "mode");
    cJSON *level_item = cJSON_GetObjectItem(body, "level");
    cJSON *save_item  = cJSON_GetObjectItem(body, "save");

    esp_err_t err = ESP_OK;

    if (cJSON_IsString(mode_item)) {
        if (strcmp(mode_item->valuestring, "off") == 0 ||
            strcmp(mode_item->valuestring, "disabled") == 0) {
            err = app_gpio_release(pin);
        } else {
            app_gpio_mode_t mode = app_gpio_mode_from_string(mode_item->valuestring);
            if (mode == APP_GPIO_MODE_DISABLED) {
                cJSON_Delete(body);
                return send_error(req, "400 Bad Request", "unknown mode");
            }
            err = app_gpio_configure(pin, mode);
        }
    }

    if (err == ESP_OK && cJSON_IsNumber(level_item)) {
        err = app_gpio_set_level(pin, level_item->valueint);
    }

    bool save = cJSON_IsTrue(save_item);
    cJSON_Delete(body);

    if (err == ESP_ERR_INVALID_STATE) {
        const char *why = "a peripheral";
        app_gpio_is_reserved(pin, &why);
        char msg[96];
        snprintf(msg, sizeof(msg), "GPIO%d is reserved for %s", pin, why);
        return send_error(req, "409 Conflict", msg);
    }
    if (err != ESP_OK) {
        return send_error(req, "400 Bad Request", esp_err_to_name(err));
    }

    if (save) {
        app_gpio_save();
    }
    return gpio_get_handler(req);
}

/* ------------------------------------------------------------------ */
/* UART                                                                */
/* ------------------------------------------------------------------ */

static esp_err_t uart_get_handler(httpd_req_t *req)
{
    uint8_t buf[512];
    size_t  n = app_uart_read(buf, sizeof(buf));

    app_uart_stats_t stats;
    app_uart_get_stats(&stats);

    cJSON *root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "baud", stats.baud);
    cJSON_AddNumberToObject(root, "total_rx", (double)stats.total_rx);
    cJSON_AddNumberToObject(root, "total_tx", (double)stats.total_tx);
    cJSON_AddNumberToObject(root, "dropped", (double)stats.dropped);
    cJSON_AddNumberToObject(root, "buffered", (double)stats.buffered);
    cJSON_AddNumberToObject(root, "length", (double)n);

    /* Hex keeps binary protocols intact; text is a convenience view. */
    char *hex = malloc(n * 2 + 1);
    char *txt = malloc(n + 1);
    if (hex != NULL && txt != NULL) {
        for (size_t i = 0; i < n; i++) {
            snprintf(hex + i * 2, 3, "%02x", buf[i]);
            txt[i] = (buf[i] >= 0x20 && buf[i] < 0x7f) ? (char)buf[i] : '.';
        }
        hex[n * 2] = '\0';
        txt[n] = '\0';
        cJSON_AddStringToObject(root, "hex", hex);
        cJSON_AddStringToObject(root, "text", txt);
    }
    free(hex);
    free(txt);

    return send_json(req, root);
}

/* Decodes an even-length hex string in place into bytes. Returns -1 on a
 * stray character. */
static int hex_decode(const char *hex, uint8_t *out, size_t out_max)
{
    size_t len = strlen(hex);
    if (len % 2 != 0 || len / 2 > out_max) {
        return -1;
    }
    for (size_t i = 0; i < len; i += 2) {
        char pair[3] = { hex[i], hex[i + 1], '\0' };
        char *end = NULL;
        long v = strtol(pair, &end, 16);
        if (end != pair + 2) {
            return -1;
        }
        out[i / 2] = (uint8_t)v;
    }
    return (int)(len / 2);
}

static esp_err_t uart_post_handler(httpd_req_t *req)
{
    cJSON *body = read_json_body(req);
    if (body == NULL) {
        return send_error(req, "400 Bad Request", "expected JSON");
    }

    cJSON *baud = cJSON_GetObjectItem(body, "baud");
    if (cJSON_IsNumber(baud)) {
        esp_err_t err = app_uart_set_baud((uint32_t)baud->valuedouble);
        if (err != ESP_OK) {
            cJSON_Delete(body);
            return send_error(req, "400 Bad Request", "unsupported baud rate");
        }
    }

    if (cJSON_IsTrue(cJSON_GetObjectItem(body, "flush"))) {
        app_uart_flush();
    }

    cJSON *text = cJSON_GetObjectItem(body, "text");
    cJSON *hex  = cJSON_GetObjectItem(body, "hex");
    esp_err_t err = ESP_OK;

    if (cJSON_IsString(text)) {
        err = app_uart_write((const uint8_t *)text->valuestring,
                             strlen(text->valuestring));
    } else if (cJSON_IsString(hex)) {
        uint8_t bytes[MAX_BODY / 2];
        int n = hex_decode(hex->valuestring, bytes, sizeof(bytes));
        if (n < 0) {
            cJSON_Delete(body);
            return send_error(req, "400 Bad Request", "malformed hex payload");
        }
        err = app_uart_write(bytes, (size_t)n);
    }

    cJSON_Delete(body);

    if (err != ESP_OK) {
        return send_error(req, "500 Internal Server Error", esp_err_to_name(err));
    }
    return send_ok(req);
}

/* ------------------------------------------------------------------ */
/* Modbus                                                              */
/* ------------------------------------------------------------------ */

static esp_err_t modbus_get_handler(httpd_req_t *req)
{
    app_mb_job_t jobs[APP_MB_MAX_JOBS];
    int n = app_modbus_get_jobs(jobs, APP_MB_MAX_JOBS);

    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "polling", app_modbus_polling());
    cJSON_AddNumberToObject(root, "period_ms", CONFIG_APP_MB_POLL_PERIOD_MS);

    cJSON *list = cJSON_AddArrayToObject(root, "jobs");
    for (int i = 0; i < n; i++) {
        if (!jobs[i].cfg.enabled) {
            continue;
        }
        cJSON *job = cJSON_CreateObject();
        cJSON_AddNumberToObject(job, "index", i);
        cJSON_AddStringToObject(job, "name", jobs[i].cfg.name);
        cJSON_AddNumberToObject(job, "slave", jobs[i].cfg.slave);
        cJSON_AddNumberToObject(job, "function", jobs[i].cfg.function);
        cJSON_AddNumberToObject(job, "start", jobs[i].cfg.start);
        cJSON_AddNumberToObject(job, "count", jobs[i].cfg.count);
        cJSON_AddNumberToObject(job, "ok", jobs[i].ok_count);
        cJSON_AddNumberToObject(job, "errors", jobs[i].err_count);
        cJSON_AddNumberToObject(job, "age_ms",
                                jobs[i].last_ms > 0
                                    ? (double)(esp_timer_get_time() / 1000 - jobs[i].last_ms)
                                    : -1);
        cJSON_AddStringToObject(job, "status", esp_err_to_name(jobs[i].last_err));

        cJSON *values = cJSON_AddArrayToObject(job, "values");
        for (int k = 0; k < jobs[i].data_len; k++) {
            cJSON_AddItemToArray(values, cJSON_CreateNumber(jobs[i].data[k]));
        }
        cJSON_AddItemToArray(list, job);
    }
    return send_json(req, root);
}

/* POST /api/modbus
 *   {"action":"read",  "slave":1, "function":3, "start":0, "count":10}
 *   {"action":"write", "slave":1, "function":6, "start":0, "values":[123]}
 *   {"action":"poll",  "enabled":true}
 *   {"action":"job",   "index":0, "slave":1, "function":3, "start":0,
 *                      "count":10, "name":"meter", "enabled":true, "save":true}
 */
static esp_err_t modbus_post_handler(httpd_req_t *req)
{
    cJSON *body = read_json_body(req);
    if (body == NULL) {
        return send_error(req, "400 Bad Request", "expected JSON");
    }

    cJSON *action_item = cJSON_GetObjectItem(body, "action");
    const char *action = cJSON_IsString(action_item) ? action_item->valuestring : "read";

    if (strcmp(action, "poll") == 0) {
        app_modbus_set_polling(cJSON_IsTrue(cJSON_GetObjectItem(body, "enabled")));
        cJSON_Delete(body);
        return modbus_get_handler(req);
    }

    if (strcmp(action, "job") == 0) {
        cJSON *index_item = cJSON_GetObjectItem(body, "index");
        if (!cJSON_IsNumber(index_item)) {
            cJSON_Delete(body);
            return send_error(req, "400 Bad Request", "\"index\" is required");
        }
        int index = index_item->valueint;
        bool save = cJSON_IsTrue(cJSON_GetObjectItem(body, "save"));

        cJSON *enabled = cJSON_GetObjectItem(body, "enabled");
        esp_err_t err;
        if (enabled != NULL && cJSON_IsFalse(enabled)) {
            err = app_modbus_clear_job(index);
        } else {
            app_mb_job_cfg_t cfg = { .enabled = true };
            cJSON *it;
            if ((it = cJSON_GetObjectItem(body, "slave")) && cJSON_IsNumber(it)) {
                cfg.slave = (uint8_t)it->valueint;
            }
            if ((it = cJSON_GetObjectItem(body, "function")) && cJSON_IsNumber(it)) {
                cfg.function = (uint8_t)it->valueint;
            }
            if ((it = cJSON_GetObjectItem(body, "start")) && cJSON_IsNumber(it)) {
                cfg.start = (uint16_t)it->valueint;
            }
            if ((it = cJSON_GetObjectItem(body, "count")) && cJSON_IsNumber(it)) {
                cfg.count = (uint16_t)it->valueint;
            }
            if ((it = cJSON_GetObjectItem(body, "name")) && cJSON_IsString(it)) {
                strlcpy(cfg.name, it->valuestring, sizeof(cfg.name));
            }
            err = app_modbus_set_job(index, &cfg);
        }
        cJSON_Delete(body);

        if (err != ESP_OK) {
            return send_error(req, "400 Bad Request", esp_err_to_name(err));
        }
        if (save) {
            app_modbus_save_jobs();
        }
        return modbus_get_handler(req);
    }

    /* read / write: a single transaction on the bus. */
    cJSON *it;
    uint8_t  slave    = 1;
    uint8_t  function = APP_MB_FN_READ_HOLDING;
    uint16_t start    = 0;
    uint16_t count    = 1;

    if ((it = cJSON_GetObjectItem(body, "slave")) && cJSON_IsNumber(it)) {
        slave = (uint8_t)it->valueint;
    }
    if ((it = cJSON_GetObjectItem(body, "function")) && cJSON_IsNumber(it)) {
        function = (uint8_t)it->valueint;
    }
    if ((it = cJSON_GetObjectItem(body, "start")) && cJSON_IsNumber(it)) {
        start = (uint16_t)it->valueint;
    }

    uint16_t values[APP_MB_MAX_REGS];
    bool writing = (strcmp(action, "write") == 0);

    if (writing) {
        cJSON *arr = cJSON_GetObjectItem(body, "values");
        if (!cJSON_IsArray(arr)) {
            cJSON_Delete(body);
            return send_error(req, "400 Bad Request", "\"values\" array is required");
        }
        count = 0;
        cJSON *v = NULL;
        cJSON_ArrayForEach(v, arr) {
            if (count >= APP_MB_MAX_REGS) {
                break;
            }
            values[count++] = (uint16_t)(cJSON_IsNumber(v) ? v->valueint : 0);
        }
        if (count == 0) {
            cJSON_Delete(body);
            return send_error(req, "400 Bad Request", "\"values\" is empty");
        }
    } else {
        if ((it = cJSON_GetObjectItem(body, "count")) && cJSON_IsNumber(it)) {
            count = (uint16_t)it->valueint;
        }
    }
    cJSON_Delete(body);

    uint16_t result[APP_MB_MAX_REGS];
    esp_err_t err = app_modbus_request(slave, function, start, count,
                                       writing ? values : NULL,
                                       writing ? NULL : result,
                                       APP_MB_MAX_REGS);
    if (err != ESP_OK) {
        /* A timeout is the slave's fault, not a malformed request, so it gets
         * 502 rather than 400. */
        return send_error(req, err == ESP_ERR_INVALID_ARG ? "400 Bad Request"
                                                          : "502 Bad Gateway",
                          esp_err_to_name(err));
    }

    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "ok", true);
    if (!writing) {
        cJSON *arr = cJSON_AddArrayToObject(root, "values");
        for (uint16_t i = 0; i < count && i < APP_MB_MAX_REGS; i++) {
            cJSON_AddItemToArray(arr, cJSON_CreateNumber(result[i]));
        }
    }
    return send_json(req, root);
}

/* ------------------------------------------------------------------ */
/* Status, console, Wi-Fi                                              */
/* ------------------------------------------------------------------ */

static esp_err_t status_handler(httpd_req_t *req)
{
    app_wifi_status_t wifi;
    app_wifi_get_status(&wifi);

    app_uart_stats_t uart;
    app_uart_get_stats(&uart);

    cJSON *root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "uptime_s", (double)(esp_timer_get_time() / 1000000));
    cJSON_AddNumberToObject(root, "heap_free", esp_get_free_heap_size());
    cJSON_AddNumberToObject(root, "heap_internal_free",
                            heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
    cJSON_AddNumberToObject(root, "psram_size",
                            esp_psram_is_initialized() ? esp_psram_get_size() : 0);

    cJSON *w = cJSON_AddObjectToObject(root, "wifi");
    cJSON_AddBoolToObject(w, "connected", wifi.connected);
    cJSON_AddBoolToObject(w, "ap_active", wifi.ap_active);
    cJSON_AddStringToObject(w, "ssid", wifi.ssid);
    cJSON_AddStringToObject(w, "ip", wifi.ip);
    cJSON_AddStringToObject(w, "ap_ip", wifi.ap_ip);
    cJSON_AddNumberToObject(w, "rssi", wifi.rssi);

    cJSON *c = cJSON_AddObjectToObject(root, "camera");
    cJSON_AddBoolToObject(c, "ready", app_camera_ready());
    cJSON_AddStringToObject(c, "sensor", app_camera_sensor_name());

    cJSON *u = cJSON_AddObjectToObject(root, "uart");
    cJSON_AddNumberToObject(u, "baud", uart.baud);
    cJSON_AddNumberToObject(u, "buffered", (double)uart.buffered);
    cJSON_AddNumberToObject(u, "total_rx", (double)uart.total_rx);
    cJSON_AddNumberToObject(u, "total_tx", (double)uart.total_tx);

    cJSON *m = cJSON_AddObjectToObject(root, "modbus");
    cJSON_AddBoolToObject(m, "polling", app_modbus_polling());

    cJSON *b = cJSON_AddObjectToObject(root, "ble");
    cJSON_AddBoolToObject(b, "enabled", app_ble_enabled());
    cJSON_AddBoolToObject(b, "connected", app_ble_connected());
    cJSON_AddStringToObject(b, "name", app_ble_device_name());

    return send_json(req, root);
}

static esp_err_t cmd_handler(httpd_req_t *req)
{
    char *body = read_body(req);
    if (body == NULL) {
        return send_error(req, "400 Bad Request", "expected a command line as the body");
    }

    char *reply = malloc(1024);
    if (reply == NULL) {
        free(body);
        return send_error(req, "500 Internal Server Error", "out of memory");
    }

    app_cmd_execute(body, reply, 1024);
    free(body);

    httpd_resp_set_type(req, "text/plain");
    set_cors(req);
    esp_err_t err = httpd_resp_sendstr(req, reply);
    free(reply);
    return err;
}

static esp_err_t wifi_post_handler(httpd_req_t *req)
{
    cJSON *body = read_json_body(req);
    if (body == NULL) {
        return send_error(req, "400 Bad Request", "expected JSON");
    }

    cJSON *ssid = cJSON_GetObjectItem(body, "ssid");
    cJSON *pass = cJSON_GetObjectItem(body, "password");
    if (!cJSON_IsString(ssid)) {
        cJSON_Delete(body);
        return send_error(req, "400 Bad Request", "\"ssid\" is required");
    }

    esp_err_t err = app_wifi_set_credentials(
        ssid->valuestring, cJSON_IsString(pass) ? pass->valuestring : "");
    cJSON_Delete(body);

    if (err != ESP_OK) {
        return send_error(req, "400 Bad Request", esp_err_to_name(err));
    }
    return send_ok(req);
}

/* ------------------------------------------------------------------ */
/* Registration                                                        */
/* ------------------------------------------------------------------ */

static const httpd_uri_t k_web_uris[] = {
    { .uri = "/",             .method = HTTP_GET,  .handler = index_handler },
    { .uri = "/capture",      .method = HTTP_GET,  .handler = capture_handler },
    { .uri = "/api/status",   .method = HTTP_GET,  .handler = status_handler },
    { .uri = "/api/camera",   .method = HTTP_GET,  .handler = camera_get_handler },
    { .uri = "/api/camera",   .method = HTTP_POST, .handler = camera_post_handler },
    { .uri = "/api/gpio",     .method = HTTP_GET,  .handler = gpio_get_handler },
    { .uri = "/api/gpio",     .method = HTTP_POST, .handler = gpio_post_handler },
    { .uri = "/api/uart",     .method = HTTP_GET,  .handler = uart_get_handler },
    { .uri = "/api/uart",     .method = HTTP_POST, .handler = uart_post_handler },
    { .uri = "/api/modbus",   .method = HTTP_GET,  .handler = modbus_get_handler },
    { .uri = "/api/modbus",   .method = HTTP_POST, .handler = modbus_post_handler },
    { .uri = "/api/wifi",     .method = HTTP_POST, .handler = wifi_post_handler },
    { .uri = "/api/cmd",      .method = HTTP_POST, .handler = cmd_handler },
};

esp_err_t app_httpd_start(void)
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.max_uri_handlers = sizeof(k_web_uris) / sizeof(k_web_uris[0]) + 2;
    config.lru_purge_enable = true;
    config.stack_size       = 6144;

    esp_err_t err = httpd_start(&s_web, &config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "port 80 failed: %s", esp_err_to_name(err));
        return err;
    }
    for (size_t i = 0; i < sizeof(k_web_uris) / sizeof(k_web_uris[0]); i++) {
        ESP_ERROR_CHECK(httpd_register_uri_handler(s_web, &k_web_uris[i]));
    }

    /* Separate instance: stream_handler never returns while a viewer is
     * watching, and one instance serves one request at a time. */
    httpd_config_t stream_config = HTTPD_DEFAULT_CONFIG();
    stream_config.server_port      = 81;
    stream_config.ctrl_port        = config.ctrl_port + 1;
    stream_config.max_uri_handlers = 1;
    stream_config.lru_purge_enable = true;
    stream_config.stack_size       = 6144;

    err = httpd_start(&s_stream, &stream_config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "port 81 failed: %s", esp_err_to_name(err));
        return err;
    }

    const httpd_uri_t stream_uri = {
        .uri = "/stream", .method = HTTP_GET, .handler = stream_handler,
    };
    ESP_ERROR_CHECK(httpd_register_uri_handler(s_stream, &stream_uri));

    ESP_LOGI(TAG, "UI on :80, MJPEG stream on :81/stream");
    return ESP_OK;
}
