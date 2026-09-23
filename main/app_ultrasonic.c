#include "app_ultrasonic.h"

#include <math.h>
#include <stdbool.h>
#include <stdint.h>

#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"

static const char *TAG = "ultrasonic";

#define TRIG_GPIO              CONFIG_APP_ULTRASONIC_TRIG_GPIO
#define ECHO_GPIO              CONFIG_APP_ULTRASONIC_ECHO_GPIO
#define LIGHT_GPIO             CONFIG_APP_ULTRASONIC_LIGHT_GPIO
#define DETECTION_THRESHOLD_CM CONFIG_APP_ULTRASONIC_DETECTION_THRESHOLD_CM
#define PERSON_HOLD_MS         CONFIG_APP_ULTRASONIC_PERSON_HOLD_MS

/* 343 m/s there and back, so half of 0.0343 cm/us. */
#define CM_PER_US 0.01715f

/* The module resolves from ~2 cm and gives up past ~4 m. A 4 m round trip is
 * 23.3 ms of echo, so 30 ms covers the whole range with margin. */
#define MIN_VALID_CM     2.0f
#define MAX_VALID_CM     400.0f
#define ECHO_TIMEOUT_MS  30

/* The datasheet asks for >=60 ms between triggers so the previous burst has
 * died down. Three samples per decision, median-filtered. */
#define SAMPLE_GAP_MS 60
#define MEDIAN_WINDOW 3

static TaskHandle_t s_task;

/* Written by the ECHO interrupt, read by the task. */
static volatile int64_t s_echo_rise_us;
static volatile int64_t s_echo_width_us;

static volatile bool  s_person_present;
static volatile float s_last_distance_cm = INFINITY;

static void echo_isr(void *arg)
{
    (void)arg;

    /* xTaskCreate has not returned yet: nobody to notify. */
    if (s_task == NULL) {
        return;
    }

    int64_t now = esp_timer_get_time();

    if (gpio_get_level(ECHO_GPIO) != 0) {
        s_echo_rise_us = now;
        return;
    }

    /* Falling edge without a matching rise: stale, ignore it. */
    if (s_echo_rise_us == 0) {
        return;
    }

    s_echo_width_us = now - s_echo_rise_us;
    s_echo_rise_us = 0;

    BaseType_t higher_woke = pdFALSE;
    vTaskNotifyGiveFromISR(s_task, &higher_woke);
    if (higher_woke) {
        portYIELD_FROM_ISR();
    }
}

/* Fire one burst and wait for the interrupt to time the echo. Returns false
 * when nothing came back inside the window, which is also how the sensor
 * reports "out of range". */
static bool measure_once(float *out_cm)
{
    s_echo_rise_us = 0;
    s_echo_width_us = 0;
    ulTaskNotifyTake(pdTRUE, 0); /* drop a notification from a late edge */

    gpio_set_level(TRIG_GPIO, 0);
    esp_rom_delay_us(4);
    gpio_set_level(TRIG_GPIO, 1);
    esp_rom_delay_us(10);
    gpio_set_level(TRIG_GPIO, 0);

    if (ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(ECHO_TIMEOUT_MS)) == 0) {
        return false;
    }

    float cm = (float)s_echo_width_us * CM_PER_US;
    if (cm < MIN_VALID_CM || cm > MAX_VALID_CM) {
        return false;
    }

    *out_cm = cm;
    return true;
}

/* Median of up to MEDIAN_WINDOW samples. A single HC-SR04 reading picks up
 * spurious near echoes off the enclosure often enough to matter. */
static bool measure_distance_cm(float *out_cm)
{
    float samples[MEDIAN_WINDOW];
    int n = 0;

    for (int i = 0; i < MEDIAN_WINDOW; i++) {
        float cm;
        if (measure_once(&cm)) {
            samples[n++] = cm;
        }
        vTaskDelay(pdMS_TO_TICKS(SAMPLE_GAP_MS));
    }

    if (n == 0) {
        return false;
    }

    for (int i = 1; i < n; i++) {
        float v = samples[i];
        int j = i - 1;
        while (j >= 0 && samples[j] > v) {
            samples[j + 1] = samples[j];
            j--;
        }
        samples[j + 1] = v;
    }

    *out_cm = samples[n / 2];
    return true;
}

static void ultrasonic_task(void *arg)
{
    (void)arg;

    int64_t last_seen_us = 0;
    bool    seen_once = false;

    while (1) {
        float cm = INFINITY;
        bool  valid = measure_distance_cm(&cm);

        s_last_distance_cm = valid ? cm : INFINITY;

        int64_t now = esp_timer_get_time();
        bool    detected_now = valid && cm <= (float)DETECTION_THRESHOLD_CM;

        if (detected_now) {
            last_seen_us = now;
            seen_once = true;
        }

        /* Hold measures time since the target was last seen, so it keeps the
         * light on across dropouts and for a moment after they walk away. */
        bool present = detected_now ||
                       (seen_once &&
                        (now - last_seen_us) < (int64_t)PERSON_HOLD_MS * 1000);

        if (present != s_person_present) {
            s_person_present = present;
            gpio_set_level(LIGHT_GPIO, present ? 1 : 0);
            if (present) {
                ESP_LOGI(TAG, "detected at %.0f cm, light on", (double)cm);
            } else {
                ESP_LOGI(TAG, "clear, light off");
            }
        }
    }
}

esp_err_t app_ultrasonic_init(void)
{
    if (!GPIO_IS_VALID_OUTPUT_GPIO(TRIG_GPIO) ||
        !GPIO_IS_VALID_OUTPUT_GPIO(LIGHT_GPIO) ||
        !GPIO_IS_VALID_GPIO(ECHO_GPIO)) {
        ESP_LOGE(TAG, "bad pins: trig=%d echo=%d light=%d",
                 TRIG_GPIO, ECHO_GPIO, LIGHT_GPIO);
        return ESP_ERR_INVALID_ARG;
    }

    gpio_config_t out_conf = {
        .pin_bit_mask = (1ULL << TRIG_GPIO) | (1ULL << LIGHT_GPIO),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };

    esp_err_t err = gpio_config(&out_conf);
    if (err != ESP_OK) {
        return err;
    }

    gpio_set_level(TRIG_GPIO, 0);
    gpio_set_level(LIGHT_GPIO, 0);

    /* ECHO is driven by the sensor. Input only - configuring it as an output
     * too would fight the module's push-pull driver. The pulldown just keeps
     * the line defined when nothing is plugged in. */
    gpio_config_t in_conf = {
        .pin_bit_mask = 1ULL << ECHO_GPIO,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_ENABLE,
        .intr_type = GPIO_INTR_ANYEDGE,
    };

    err = gpio_config(&in_conf);
    if (err != ESP_OK) {
        return err;
    }

    err = gpio_install_isr_service(ESP_INTR_FLAG_LEVEL1);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        return err; /* INVALID_STATE just means someone else installed it */
    }

    /* Task before handler: the ISR needs somewhere to send its notification. */
    if (xTaskCreatePinnedToCore(ultrasonic_task, "ultrasonic", 4096, NULL, 5,
                                &s_task, 0) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }

    err = gpio_isr_handler_add(ECHO_GPIO, echo_isr, NULL);
    if (err != ESP_OK) {
        vTaskDelete(s_task);
        s_task = NULL;
        return err;
    }

    ESP_LOGI(TAG, "trig=%d echo=%d light=%d, %d cm threshold, %d ms hold",
             TRIG_GPIO, ECHO_GPIO, LIGHT_GPIO, DETECTION_THRESHOLD_CM,
             PERSON_HOLD_MS);
    return ESP_OK;
}

bool app_ultrasonic_person_present(void)
{
    return s_person_present;
}

float app_ultrasonic_distance_cm(void)
{
    return s_last_distance_cm;
}
