#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define APP_MB_MAX_JOBS 8
#define APP_MB_MAX_REGS 64

/* Modbus function codes this gateway speaks as a master. */
#define APP_MB_FN_READ_COILS         1
#define APP_MB_FN_READ_DISCRETE      2
#define APP_MB_FN_READ_HOLDING       3
#define APP_MB_FN_READ_INPUT         4
#define APP_MB_FN_WRITE_COIL         5
#define APP_MB_FN_WRITE_HOLDING      6
#define APP_MB_FN_WRITE_COILS       15
#define APP_MB_FN_WRITE_HOLDINGS    16

typedef struct {
    bool     enabled;
    uint8_t  slave;
    uint8_t  function;     /* 1..4 only; the poll loop never writes */
    uint16_t start;
    uint16_t count;
    char     name[16];
} app_mb_job_cfg_t;

typedef struct {
    app_mb_job_cfg_t cfg;
    uint16_t  data[APP_MB_MAX_REGS];  /* coils are expanded to one word each */
    uint16_t  data_len;
    esp_err_t last_err;
    uint32_t  ok_count;
    uint32_t  err_count;
    int64_t   last_ms;     /* esp_timer millis of the last completed poll */
} app_mb_job_t;

esp_err_t app_modbus_init(void);

/* One transaction against a slave.
 *   in   - values to write (NULL for read functions)
 *   out  - where to put the response (NULL if the caller does not care)
 * Coil/discrete functions take and return one word per bit, 0 or 1; the
 * bit packing on the wire is handled here. Serialised against the poll
 * loop, so this can block for up to the Modbus response timeout. */
esp_err_t app_modbus_request(uint8_t slave, uint8_t function,
                             uint16_t start, uint16_t count,
                             const uint16_t *in, uint16_t *out, size_t out_max);

/* Poll table. Jobs are indexed 0..APP_MB_MAX_JOBS-1 and persist in NVS. */
esp_err_t app_modbus_set_job(int index, const app_mb_job_cfg_t *cfg);
esp_err_t app_modbus_clear_job(int index);
int       app_modbus_get_jobs(app_mb_job_t *out, int max);
esp_err_t app_modbus_save_jobs(void);

void app_modbus_set_polling(bool enabled);
bool app_modbus_polling(void);

#ifdef __cplusplus
}
#endif
