#pragma once

#include <stdbool.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Ultrasonic presence detector (HC-SR04 style TRIG/ECHO module).
 *
 * app_ultrasonic_init() configures the pins, hooks an edge interrupt onto
 * ECHO and starts a background task that drives the light output. */
esp_err_t app_ultrasonic_init(void);

/* True while the light is on: something is within the configured threshold,
 * or was within the last CONFIG_APP_ULTRASONIC_PERSON_HOLD_MS. */
bool app_ultrasonic_person_present(void);

/* Last good reading in cm, or INFINITY when the sensor last reported no echo
 * (out of range, or nothing wired up). Cached - does not fire the sensor. */
float app_ultrasonic_distance_cm(void);

#ifdef __cplusplus
}
#endif
