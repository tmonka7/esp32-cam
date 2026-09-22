#include "app_camera.h"

#include <string.h>

#include "esp_camera.h"
#include "esp_log.h"
#include "esp_psram.h"
#include "sdkconfig.h"

static const char *TAG = "cam";

static bool s_ready;
static const char *s_sensor_name = "none";

esp_err_t app_camera_init(void)
{
    camera_config_t config = {
        .pin_pwdn      = CONFIG_APP_CAM_PIN_PWDN,
        .pin_reset     = CONFIG_APP_CAM_PIN_RESET,
        .pin_xclk      = CONFIG_APP_CAM_PIN_XCLK,
        .pin_sccb_sda  = CONFIG_APP_CAM_PIN_SIOD,
        .pin_sccb_scl  = CONFIG_APP_CAM_PIN_SIOC,
        .pin_d7        = CONFIG_APP_CAM_PIN_D7,
        .pin_d6        = CONFIG_APP_CAM_PIN_D6,
        .pin_d5        = CONFIG_APP_CAM_PIN_D5,
        .pin_d4        = CONFIG_APP_CAM_PIN_D4,
        .pin_d3        = CONFIG_APP_CAM_PIN_D3,
        .pin_d2        = CONFIG_APP_CAM_PIN_D2,
        .pin_d1        = CONFIG_APP_CAM_PIN_D1,
        .pin_d0        = CONFIG_APP_CAM_PIN_D0,
        .pin_vsync     = CONFIG_APP_CAM_PIN_VSYNC,
        .pin_href      = CONFIG_APP_CAM_PIN_HREF,
        .pin_pclk      = CONFIG_APP_CAM_PIN_PCLK,
        .xclk_freq_hz  = CONFIG_APP_CAM_XCLK_FREQ_HZ,
        .ledc_timer    = LEDC_TIMER_0,
        .ledc_channel  = LEDC_CHANNEL_0,
        .pixel_format  = PIXFORMAT_JPEG,
        .frame_size    = FRAMESIZE_SVGA,
        .jpeg_quality  = CONFIG_APP_CAM_JPEG_QUALITY,
        .fb_count      = CONFIG_APP_CAM_FB_COUNT,
        .fb_location   = CAMERA_FB_IN_PSRAM,
        /* LATEST keeps the stream close to real time: a slow HTTP client
         * drops frames instead of watching an ever-growing delay. */
        .grab_mode     = CAMERA_GRAB_LATEST,
    };

    if (!esp_psram_is_initialized()) {
        ESP_LOGW(TAG, "no PSRAM detected, limiting the sensor to QVGA");
        config.frame_size  = FRAMESIZE_QVGA;
        config.fb_location = CAMERA_FB_IN_DRAM;
        config.fb_count    = 1;
        config.grab_mode   = CAMERA_GRAB_WHEN_EMPTY;
    }

    esp_err_t err = esp_camera_init(&config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_camera_init failed: %s", esp_err_to_name(err));
        return err;
    }

    sensor_t *s = esp_camera_sensor_get();
    if (s == NULL) {
        esp_camera_deinit();
        return ESP_ERR_NOT_FOUND;
    }

    camera_sensor_info_t *info = esp_camera_sensor_get_info(&s->id);
    s_sensor_name = (info != NULL && info->name != NULL) ? info->name : "unknown";

    /* The OV3660 module ships mounted upside down and over-saturated. */
    if (s->id.PID == OV3660_PID) {
        s->set_vflip(s, 1);
        s->set_brightness(s, 1);
        s->set_saturation(s, -2);
    }

    s_ready = true;
    ESP_LOGI(TAG, "%s ready, %s frame buffers",
             s_sensor_name, esp_psram_is_initialized() ? "PSRAM" : "DRAM");
    return ESP_OK;
}

bool app_camera_ready(void)
{
    return s_ready;
}

const char *app_camera_sensor_name(void)
{
    return s_sensor_name;
}

static const char *const k_setting_names[] = {
    "framesize", "quality", "brightness", "contrast", "saturation", "sharpness",
    "denoise", "gainceiling", "colorbar", "whitebal", "gain_ctrl", "exposure_ctrl",
    "hmirror", "vflip", "aec2", "awb_gain", "agc_gain", "aec_value",
    "special_effect", "wb_mode", "ae_level", "dcw", "bpc", "wpc", "raw_gma", "lenc",
    NULL,
};

const char *const *app_camera_setting_names(void)
{
    return k_setting_names;
}

esp_err_t app_camera_set_setting(const char *name, int value)
{
    if (!s_ready) {
        return ESP_ERR_INVALID_STATE;
    }
    sensor_t *s = esp_camera_sensor_get();
    if (s == NULL || name == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    int rc;
    if (strcmp(name, "framesize") == 0) {
        if (value < 0 || value > FRAMESIZE_QXGA) {
            return ESP_ERR_INVALID_ARG;
        }
        rc = s->set_framesize(s, (framesize_t)value);
    } else if (strcmp(name, "gainceiling") == 0) {
        rc = s->set_gainceiling(s, (gainceiling_t)value);
    }
#define CAM_SETTER(key, fn)                    \
    else if (strcmp(name, key) == 0) {         \
        rc = s->fn(s, value);                  \
    }
    CAM_SETTER("quality",        set_quality)
    CAM_SETTER("brightness",     set_brightness)
    CAM_SETTER("contrast",       set_contrast)
    CAM_SETTER("saturation",     set_saturation)
    CAM_SETTER("sharpness",      set_sharpness)
    CAM_SETTER("denoise",        set_denoise)
    CAM_SETTER("colorbar",       set_colorbar)
    CAM_SETTER("whitebal",       set_whitebal)
    CAM_SETTER("gain_ctrl",      set_gain_ctrl)
    CAM_SETTER("exposure_ctrl",  set_exposure_ctrl)
    CAM_SETTER("hmirror",        set_hmirror)
    CAM_SETTER("vflip",          set_vflip)
    CAM_SETTER("aec2",           set_aec2)
    CAM_SETTER("awb_gain",       set_awb_gain)
    CAM_SETTER("agc_gain",       set_agc_gain)
    CAM_SETTER("aec_value",      set_aec_value)
    CAM_SETTER("special_effect", set_special_effect)
    CAM_SETTER("wb_mode",        set_wb_mode)
    CAM_SETTER("ae_level",       set_ae_level)
    CAM_SETTER("dcw",            set_dcw)
    CAM_SETTER("bpc",            set_bpc)
    CAM_SETTER("wpc",            set_wpc)
    CAM_SETTER("raw_gma",        set_raw_gma)
    CAM_SETTER("lenc",           set_lenc)
#undef CAM_SETTER
    else {
        return ESP_ERR_NOT_FOUND;
    }

    return rc == 0 ? ESP_OK : ESP_FAIL;
}

esp_err_t app_camera_get_setting(const char *name, int *out_value)
{
    if (!s_ready || out_value == NULL || name == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    sensor_t *s = esp_camera_sensor_get();
    if (s == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    camera_status_t *st = &s->status;

#define CAM_GETTER(key, field)                 \
    if (strcmp(name, key) == 0) {              \
        *out_value = (int)st->field;           \
        return ESP_OK;                         \
    }
    CAM_GETTER("framesize",      framesize)
    CAM_GETTER("quality",        quality)
    CAM_GETTER("brightness",     brightness)
    CAM_GETTER("contrast",       contrast)
    CAM_GETTER("saturation",     saturation)
    CAM_GETTER("sharpness",      sharpness)
    CAM_GETTER("denoise",        denoise)
    CAM_GETTER("gainceiling",    gainceiling)
    CAM_GETTER("colorbar",       colorbar)
    CAM_GETTER("whitebal",       awb)
    CAM_GETTER("gain_ctrl",      agc)
    CAM_GETTER("exposure_ctrl",  aec)
    CAM_GETTER("hmirror",        hmirror)
    CAM_GETTER("vflip",          vflip)
    CAM_GETTER("aec2",           aec2)
    CAM_GETTER("awb_gain",       awb_gain)
    CAM_GETTER("agc_gain",       agc_gain)
    CAM_GETTER("aec_value",      aec_value)
    CAM_GETTER("special_effect", special_effect)
    CAM_GETTER("wb_mode",        wb_mode)
    CAM_GETTER("ae_level",       ae_level)
    CAM_GETTER("dcw",            dcw)
    CAM_GETTER("bpc",            bpc)
    CAM_GETTER("wpc",            wpc)
    CAM_GETTER("raw_gma",        raw_gma)
    CAM_GETTER("lenc",           lenc)
#undef CAM_GETTER

    return ESP_ERR_NOT_FOUND;
}
