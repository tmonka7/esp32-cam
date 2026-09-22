#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Starts two httpd instances:
 *   :80  UI and the JSON API
 *   :81  /stream, the MJPEG multipart feed
 *
 * They are separate because esp_http_server serves one request at a time per
 * instance, and the stream handler holds its request open indefinitely. */
esp_err_t app_httpd_start(void);

#ifdef __cplusplus
}
#endif
