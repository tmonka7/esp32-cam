#include "app_cmd.h"

#include <inttypes.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "app_ble.h"
#include "app_camera.h"
#include "app_gpio.h"
#include "app_modbus.h"
#include "app_uart.h"
#include "app_wifi.h"

static const char *TAG = "cmd";

#define MAX_ARGS 12

/* "uart monitor on" mirrors bridge traffic to the BLE notify channel. */
static bool s_uart_monitor;

static void monitor_cb(const uint8_t *data, size_t len, void *ctx)
{
    if (s_uart_monitor) {
        app_ble_notify((const char *)data, len);
    }
}

/* Appends to a bounded output buffer, tracking how much has been used. */
typedef struct {
    char  *buf;
    size_t size;
    size_t len;
} outbuf_t;

static void emit(outbuf_t *o, const char *fmt, ...) __attribute__((format(printf, 2, 3)));

static void emit(outbuf_t *o, const char *fmt, ...)
{
    if (o->len >= o->size - 1) {
        return;
    }
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(o->buf + o->len, o->size - o->len, fmt, ap);
    va_end(ap);

    if (n > 0) {
        o->len += (size_t)n;
        if (o->len >= o->size) {
            o->len = o->size - 1;   /* truncated */
        }
    }
}

static const char *HELP =
    "commands:\n"
    "  status | reboot | help\n"
    "  wifi <ssid> <pass>\n"
    "  gpio list | mode <pin> <in|inpu|inpd|out|od|off> | set <pin> <0|1> | get <pin> | save\n"
    "  uart baud <n> | send <text> | read | monitor <on|off>\n"
    "  mb read <slave> <fn> <start> <count>\n"
    "  mb write <slave> <fn> <start> <val...> | poll <on|off> | jobs\n"
    "  cam <control> <value>\n";

/* ------------------------------------------------------------------ */

static void cmd_status(outbuf_t *o)
{
    app_wifi_status_t wifi;
    app_wifi_get_status(&wifi);

    app_uart_stats_t uart;
    app_uart_get_stats(&uart);

    emit(o, "uptime  %lld s\n", esp_timer_get_time() / 1000000);
    emit(o, "heap    %u free (%u internal)\n",
         (unsigned)esp_get_free_heap_size(),
         (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
    emit(o, "wifi    %s ssid=%s ip=%s rssi=%d%s\n",
         wifi.connected ? "up" : "down", wifi.ssid, wifi.ip, wifi.rssi,
         wifi.ap_active ? " (softap up)" : "");
    emit(o, "camera  %s (%s)\n",
         app_camera_ready() ? "ready" : "offline", app_camera_sensor_name());
    emit(o, "uart    %" PRIu32 " baud, rx=%llu tx=%llu buffered=%u\n",
         uart.baud, (unsigned long long)uart.total_rx,
         (unsigned long long)uart.total_tx, (unsigned)uart.buffered);
    emit(o, "modbus  poll %s\n", app_modbus_polling() ? "on" : "off");
    emit(o, "ble     %s\n", app_ble_connected() ? "connected" : "advertising");
}

static void cmd_gpio(outbuf_t *o, int argc, char **argv)
{
    if (argc < 2) {
        emit(o, "usage: gpio list|mode|set|get|save\n");
        return;
    }

    if (strcmp(argv[1], "list") == 0) {
        app_gpio_info_t pins[APP_GPIO_MAX_PINS];
        int n = app_gpio_list(pins, APP_GPIO_MAX_PINS);
        if (n == 0) {
            emit(o, "no pins configured\n");
        }
        for (int i = 0; i < n; i++) {
            emit(o, "GPIO%-2d %-14s %d\n",
                 pins[i].pin, app_gpio_mode_to_string(pins[i].mode), pins[i].level);
        }
        return;
    }

    if (strcmp(argv[1], "save") == 0) {
        esp_err_t err = app_gpio_save();
        emit(o, "%s\n", err == ESP_OK ? "saved" : esp_err_to_name(err));
        return;
    }

    if (argc < 3) {
        emit(o, "usage: gpio %s <pin> [...]\n", argv[1]);
        return;
    }
    int pin = atoi(argv[2]);

    if (strcmp(argv[1], "mode") == 0) {
        if (argc < 4) {
            emit(o, "usage: gpio mode <pin> <in|inpu|inpd|out|od|off>\n");
            return;
        }
        if (strcmp(argv[3], "off") == 0) {
            esp_err_t err = app_gpio_release(pin);
            emit(o, "%s\n", err == ESP_OK ? "released" : esp_err_to_name(err));
            return;
        }
        app_gpio_mode_t mode = app_gpio_mode_from_string(argv[3]);
        if (mode == APP_GPIO_MODE_DISABLED) {
            emit(o, "unknown mode \"%s\"\n", argv[3]);
            return;
        }
        esp_err_t err = app_gpio_configure(pin, mode);
        if (err == ESP_ERR_INVALID_STATE) {
            const char *why = "a peripheral";
            app_gpio_is_reserved(pin, &why);
            emit(o, "GPIO%d is reserved for %s\n", pin, why);
        } else {
            emit(o, "%s\n", err == ESP_OK ? "ok" : esp_err_to_name(err));
        }
        return;
    }

    if (strcmp(argv[1], "set") == 0) {
        if (argc < 4) {
            emit(o, "usage: gpio set <pin> <0|1>\n");
            return;
        }
        esp_err_t err = app_gpio_set_level(pin, atoi(argv[3]));
        emit(o, "%s\n", err == ESP_OK ? "ok" : esp_err_to_name(err));
        return;
    }

    if (strcmp(argv[1], "get") == 0) {
        int level;
        esp_err_t err = app_gpio_get_level(pin, &level);
        if (err == ESP_OK) {
            emit(o, "GPIO%d = %d\n", pin, level);
        } else {
            emit(o, "%s\n", esp_err_to_name(err));
        }
        return;
    }

    emit(o, "unknown gpio subcommand \"%s\"\n", argv[1]);
}

static void cmd_uart(outbuf_t *o, int argc, char **argv, const char *raw)
{
    if (argc < 2) {
        emit(o, "usage: uart baud|send|read|monitor\n");
        return;
    }

    if (strcmp(argv[1], "baud") == 0 && argc >= 3) {
        esp_err_t err = app_uart_set_baud((uint32_t)strtoul(argv[2], NULL, 10));
        emit(o, "%s\n", err == ESP_OK ? "ok" : esp_err_to_name(err));
        return;
    }

    if (strcmp(argv[1], "send") == 0) {
        /* Take the payload from the untokenised line so spaces survive. */
        const char *p = strstr(raw, "send");
        if (p == NULL || p[4] == '\0') {
            emit(o, "usage: uart send <text>\n");
            return;
        }
        p += 5;
        esp_err_t err = app_uart_write((const uint8_t *)p, strlen(p));
        emit(o, "%s %u bytes\n", err == ESP_OK ? "sent" : "partial", (unsigned)strlen(p));
        return;
    }

    if (strcmp(argv[1], "read") == 0) {
        uint8_t buf[256];
        size_t n = app_uart_read(buf, sizeof(buf) - 1);
        if (n == 0) {
            emit(o, "(empty)\n");
            return;
        }
        buf[n] = '\0';
        for (size_t i = 0; i < n; i++) {
            /* Keep the reply printable: a BLE client should not have to cope
             * with raw control bytes. */
            if (buf[i] < 0x20 && buf[i] != '\n' && buf[i] != '\r' && buf[i] != '\t') {
                buf[i] = '.';
            }
        }
        emit(o, "%s\n", (char *)buf);
        return;
    }

    if (strcmp(argv[1], "monitor") == 0 && argc >= 3) {
        s_uart_monitor = strcmp(argv[2], "on") == 0;
        app_uart_set_rx_callback(s_uart_monitor ? monitor_cb : NULL, NULL);
        emit(o, "monitor %s\n", s_uart_monitor ? "on" : "off");
        return;
    }

    emit(o, "unknown uart subcommand \"%s\"\n", argv[1]);
}

static void cmd_modbus(outbuf_t *o, int argc, char **argv)
{
    if (argc < 2) {
        emit(o, "usage: mb read|write|poll|jobs\n");
        return;
    }

    if (strcmp(argv[1], "poll") == 0 && argc >= 3) {
        app_modbus_set_polling(strcmp(argv[2], "on") == 0);
        emit(o, "poll %s\n", app_modbus_polling() ? "on" : "off");
        return;
    }

    if (strcmp(argv[1], "jobs") == 0) {
        app_mb_job_t jobs[APP_MB_MAX_JOBS];
        int n = app_modbus_get_jobs(jobs, APP_MB_MAX_JOBS);
        bool any = false;
        for (int i = 0; i < n; i++) {
            if (!jobs[i].cfg.enabled) {
                continue;
            }
            any = true;
            emit(o, "%d %s slave=%u fn=%u @%u x%u ok=%lu err=%lu",
                 i, jobs[i].cfg.name[0] ? jobs[i].cfg.name : "-",
                 (unsigned)jobs[i].cfg.slave, (unsigned)jobs[i].cfg.function,
                 (unsigned)jobs[i].cfg.start, (unsigned)jobs[i].cfg.count,
                 (unsigned long)jobs[i].ok_count, (unsigned long)jobs[i].err_count);
            for (int k = 0; k < jobs[i].data_len && k < 8; k++) {
                emit(o, " %u", (unsigned)jobs[i].data[k]);
            }
            emit(o, "\n");
        }
        if (!any) {
            emit(o, "no jobs\n");
        }
        return;
    }

    if (strcmp(argv[1], "read") == 0) {
        if (argc < 6) {
            emit(o, "usage: mb read <slave> <fn> <start> <count>\n");
            return;
        }
        uint8_t  slave = (uint8_t)atoi(argv[2]);
        uint8_t  fn    = (uint8_t)atoi(argv[3]);
        uint16_t start = (uint16_t)atoi(argv[4]);
        uint16_t count = (uint16_t)atoi(argv[5]);

        uint16_t vals[APP_MB_MAX_REGS];
        esp_err_t err = app_modbus_request(slave, fn, start, count, NULL,
                                           vals, APP_MB_MAX_REGS);
        if (err != ESP_OK) {
            emit(o, "error %s\n", esp_err_to_name(err));
            return;
        }
        for (uint16_t i = 0; i < count && i < APP_MB_MAX_REGS; i++) {
            emit(o, "%u%s", (unsigned)vals[i], (i + 1 == count) ? "\n" : " ");
        }
        return;
    }

    if (strcmp(argv[1], "write") == 0) {
        if (argc < 6) {
            emit(o, "usage: mb write <slave> <fn> <start> <val...>\n");
            return;
        }
        uint8_t  slave = (uint8_t)atoi(argv[2]);
        uint8_t  fn    = (uint8_t)atoi(argv[3]);
        uint16_t start = (uint16_t)atoi(argv[4]);

        uint16_t vals[APP_MB_MAX_REGS];
        uint16_t count = 0;
        for (int i = 5; i < argc && count < APP_MB_MAX_REGS; i++) {
            vals[count++] = (uint16_t)strtoul(argv[i], NULL, 0);
        }

        esp_err_t err = app_modbus_request(slave, fn, start, count, vals, NULL, 0);
        emit(o, "%s\n", err == ESP_OK ? "ok" : esp_err_to_name(err));
        return;
    }

    emit(o, "unknown mb subcommand \"%s\"\n", argv[1]);
}

static void cmd_cam(outbuf_t *o, int argc, char **argv)
{
    if (argc < 2) {
        const char *const *names = app_camera_setting_names();
        emit(o, "controls:");
        for (int i = 0; names[i] != NULL; i++) {
            emit(o, " %s", names[i]);
        }
        emit(o, "\n");
        return;
    }

    if (argc == 2) {
        int value;
        esp_err_t err = app_camera_get_setting(argv[1], &value);
        if (err == ESP_OK) {
            emit(o, "%s = %d\n", argv[1], value);
        } else {
            emit(o, "%s\n", esp_err_to_name(err));
        }
        return;
    }

    esp_err_t err = app_camera_set_setting(argv[1], atoi(argv[2]));
    emit(o, "%s\n", err == ESP_OK ? "ok" : esp_err_to_name(err));
}

static void reboot_task(void *arg)
{
    /* Let the reply reach the client before the radio goes away. */
    vTaskDelay(pdMS_TO_TICKS(500));
    esp_restart();
}

/* ------------------------------------------------------------------ */

size_t app_cmd_execute(const char *line, char *out, size_t out_size)
{
    if (out == NULL || out_size == 0) {
        return 0;
    }
    out[0] = '\0';

    outbuf_t o = { .buf = out, .size = out_size, .len = 0 };

    if (line == NULL) {
        emit(&o, "empty command\n");
        return o.len;
    }

    /* strtok mutates, and "uart send" wants the original spacing. */
    char work[256];
    strlcpy(work, line, sizeof(work));

    char raw[256];
    strlcpy(raw, line, sizeof(raw));

    /* Trim the trailing newline a terminal or BLE client may append. */
    for (char *p = work + strlen(work); p > work && (p[-1] == '\n' || p[-1] == '\r'); p--) {
        p[-1] = '\0';
    }
    for (char *p = raw + strlen(raw); p > raw && (p[-1] == '\n' || p[-1] == '\r'); p--) {
        p[-1] = '\0';
    }

    char *argv[MAX_ARGS];
    int   argc = 0;
    char *save = NULL;
    for (char *tok = strtok_r(work, " \t", &save);
         tok != NULL && argc < MAX_ARGS;
         tok = strtok_r(NULL, " \t", &save)) {
        argv[argc++] = tok;
    }

    if (argc == 0) {
        emit(&o, "%s", HELP);
        return o.len;
    }

    ESP_LOGI(TAG, "> %s", raw);

    if (strcmp(argv[0], "help") == 0) {
        emit(&o, "%s", HELP);
    } else if (strcmp(argv[0], "status") == 0) {
        cmd_status(&o);
    } else if (strcmp(argv[0], "reboot") == 0) {
        emit(&o, "rebooting\n");
        xTaskCreate(reboot_task, "reboot", 2048, NULL, 5, NULL);
    } else if (strcmp(argv[0], "wifi") == 0) {
        if (argc < 3) {
            emit(&o, "usage: wifi <ssid> <password>\n");
        } else {
            esp_err_t err = app_wifi_set_credentials(argv[1], argv[2]);
            emit(&o, "%s\n", err == ESP_OK ? "stored, reboot to apply"
                                           : esp_err_to_name(err));
        }
    } else if (strcmp(argv[0], "gpio") == 0) {
        cmd_gpio(&o, argc, argv);
    } else if (strcmp(argv[0], "uart") == 0) {
        cmd_uart(&o, argc, argv, raw);
    } else if (strcmp(argv[0], "mb") == 0) {
        cmd_modbus(&o, argc, argv);
    } else if (strcmp(argv[0], "cam") == 0) {
        cmd_cam(&o, argc, argv);
    } else {
        emit(&o, "unknown command \"%s\", try help\n", argv[0]);
    }

    return o.len;
}
