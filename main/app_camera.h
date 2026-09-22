#pragma once

#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Initialises the sensor with the pin map from Kconfig. Returns the driver
 * error unchanged on failure; the rest of the firmware stays usable so a
 * dead ribbon cable does not take the field bus offline. */
esp_err_t app_camera_init(void);

bool app_camera_ready(void);

/* Sensor name, e.g. "OV2640". "none" when the camera failed to initialise. */
const char *app_camera_sensor_name(void);

/* Applies one named sensor control. Accepted names match the keys returned by
 * app_camera_get_setting(), e.g. "framesize", "quality", "hmirror", "vflip".
 * Returns ESP_ERR_NOT_FOUND for an unknown name. */
esp_err_t app_camera_set_setting(const char *name, int value);

/* Reads one named sensor control back. */
esp_err_t app_camera_get_setting(const char *name, int *out_value);

/* NULL-terminated list of every control name this build accepts. */
const char *const *app_camera_setting_names(void);

#ifdef __cplusplus
}
#endif
