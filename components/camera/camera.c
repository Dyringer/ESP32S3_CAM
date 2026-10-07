/**
 ******************************************************************************
 * @file    camera.c
 * @author  Daniel Dyringer
 * @date    2026-09-30
 * @brief   Camera core — OV3660 bring-up, runtime settings, frame access and
 *          illumination LED.
 ******************************************************************************
 */


/* Includes ---------------------------------------------------------------- */
#include "camera.h"

#include <string.h>

#include "bsp.h"
#include "driver/ledc.h"
#include "esp_camera.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "nvs.h"
#include "utils.h"


/* Constants and macros ---------------------------------------------------- */
// clang-format off

/** @brief Largest OV3660 output — frame buffers are sized for it at init. */
#define CAMERA_FRAMESIZE_MAX        FRAMESIZE_QXGA      /* 2048x1536        */

/** @brief Resolution applied after init. */
#define CAMERA_FRAMESIZE_BOOT       FRAMESIZE_HD        /* 1280x720         */

/** @brief JPEG quality at boot: 0-63, lower = better quality, bigger frames. */
#define CAMERA_JPEG_QUALITY_BOOT    10

/** @brief Frame buffers in PSRAM. Two lets capture overlap transmission. */
#define CAMERA_FB_COUNT             2u

/**
 * @brief Time the LED is lit before grabbing a flash still. With
 *        CAMERA_GRAB_LATEST the driver keeps capturing; this long lets at
 *        least one full frame be exposed with the LED on.
 */
#define CAMERA_FLASH_SETTLE_MS      150u

/** @brief Name of the pseudo-setting that controls the LED PWM duty. */
#define CAMERA_SETTING_LED          "led_intensity"

#define CAMERA_LED_DUTY_MAX         255
#define CAMERA_LED_DUTY_RES         LEDC_TIMER_8_BIT

/** @brief NVS namespace for saved settings — one i32 per setting, keyed by
 *         setting name, so adding or removing settings in a later firmware
 *         keeps every other saved value. Names must fit NVS_KEY_NAME_MAX_SIZE. */
#define CAMERA_NVS_NAMESPACE        "camera"

/** @brief OV3660 AEC gain ceiling registers (10-bit value, 0x3A18:0x3A19). */
#define CAMERA_OV3660_REG_GAINCEIL  0x3A18
#define CAMERA_OV3660_GAINCEIL_MASK 0x3FF

// clang-format on


/* Private types ----------------------------------------------------------- */

/** @brief Setting adapters. Called with camera_mutex held; set returns 0 on
 *         success like the sensor_t methods it wraps. */
typedef int (*camera_set_fn_t)(int value);
typedef esp_err_t (*camera_get_fn_t)(int *value);

typedef struct
{
    camera_setting_desc_t desc;
    bool                  needs_sensor; /**< false = local (LED), works without a sensor */
    camera_set_fn_t       set;
    camera_get_fn_t       get;
} camera_setting_t;


/* Private variables ------------------------------------------------------- */

static const char *camera_tag = "camera";

static bool              camera_ready = false;
static SemaphoreHandle_t camera_mutex = NULL;

/* LED state — guarded by camera_mutex */
static int  camera_led_duty  = 0;     /**< Configured intensity 0-255.       */
static bool camera_led_on    = false; /**< Lit on behalf of the stream.      */
static bool camera_led_flash = false; /**< Lit for a still capture.          */


/* Private functions ------------------------------------------------------- */

/** @brief Push the LED state to the PWM output. Caller holds camera_mutex. */
static void camera_led_apply_locked(void)
{
    const uint32_t duty =
        (camera_led_on || camera_led_flash) ? (uint32_t)camera_led_duty : 0u;

    ledc_set_duty(LEDC_LOW_SPEED_MODE, BSP_LED_LEDC_CHANNEL, duty);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, BSP_LED_LEDC_CHANNEL);
}

/* ---- Settings table ----------------------------------------------------- */

/**
 * @brief Sensor settings that map 1:1 onto a sensor_t method and a
 *        camera_status_t field: X(key, min, max, method, status_field).
 *
 *        Ranges match the OV3660 driver's own bounds checks. Table order is
 *        also the order saved values are restored in at boot: mode switches
 *        (aec, agc, awb_gain) come before the manual values they gate.
 */
#define CAMERA_SENSOR_SETTINGS(X)                                                 \
    /* Image */                                                                   \
    X(framesize,       0, CAMERA_FRAMESIZE_MAX, set_framesize,      framesize)      \
    X(quality,         4,   63,                 set_quality,        quality)        \
    X(brightness,     -3,    3,                 set_brightness,     brightness)     \
    X(contrast,       -3,    3,                 set_contrast,       contrast)       \
    X(saturation,     -4,    4,                 set_saturation,     saturation)     \
    X(sharpness,      -3,    3,                 set_sharpness,      sharpness)      \
    X(denoise,         0,    8,                 set_denoise,        denoise)        \
    X(special_effect,  0,    6,                 set_special_effect, special_effect) \
    X(hmirror,         0,    1,                 set_hmirror,        hmirror)        \
    X(vflip,           0,    1,                 set_vflip,          vflip)          \
    /* Exposure and gain */                                                       \
    X(aec,             0,    1,                 set_exposure_ctrl,  aec)            \
    X(aec2,            0,    1,                 set_aec2,           aec2)           \
    X(ae_level,       -5,    5,                 set_ae_level,       ae_level)       \
    X(aec_value,       0, 1536,                 set_aec_value,      aec_value)      \
    X(agc,             0,    1,                 set_gain_ctrl,      agc)            \
    X(agc_gain,        0,   64,                 set_agc_gain,       agc_gain)       \
    /* White balance */                                                           \
    X(awb,             0,    1,                 set_whitebal,       awb)            \
    X(awb_gain,        0,    1,                 set_awb_gain,       awb_gain)       \
    X(wb_mode,         0,    4,                 set_wb_mode,        wb_mode)        \
    /* Corrections */                                                             \
    X(bpc,             0,    1,                 set_bpc,            bpc)            \
    X(wpc,             0,    1,                 set_wpc,            wpc)            \
    X(raw_gma,         0,    1,                 set_raw_gma,        raw_gma)        \
    X(lenc,            0,    1,                 set_lenc,           lenc)           \
    X(dcw,             0,    1,                 set_dcw,            dcw)            \
    X(colorbar,        0,    1,                 set_colorbar,       colorbar)

#define CAMERA_SETTING_ADAPTERS(key, min_, max_, method, field)                  \
    static int camera_set_##key(int value)                                       \
    {                                                                            \
        sensor_t *s = esp_camera_sensor_get();                                   \
        return s->method(s, value);                                              \
    }                                                                            \
    static esp_err_t camera_get_##key(int *value)                                \
    {                                                                            \
        *value = (int)esp_camera_sensor_get()->status.field;                     \
        return ESP_OK;                                                           \
    }

#define CAMERA_SETTING_ROW(key, min_, max_, method, field)                       \
    { { #key, (min_), (max_) }, true, camera_set_##key, camera_get_##key },

CAMERA_SENSOR_SETTINGS(CAMERA_SETTING_ADAPTERS)

static int camera_set_gainceiling(int value)
{
    sensor_t *s = esp_camera_sensor_get();
    return s->set_gainceiling(s, (gainceiling_t)value);
}

/* camera_status_t.gainceiling is uint8_t, but the OV3660 driver stores its
 * 10-bit register value there — read the register instead of the truncated
 * cached copy. */
static esp_err_t camera_get_gainceiling(int *value)
{
    sensor_t *s   = esp_camera_sensor_get();
    const int reg = s->get_reg(s, CAMERA_OV3660_REG_GAINCEIL, CAMERA_OV3660_GAINCEIL_MASK);

    if (reg < 0)
    {
        return ESP_FAIL;
    }

    *value = reg;
    return ESP_OK;
}

static int camera_set_led(int value)
{
    camera_led_duty = value;
    camera_led_apply_locked();
    return 0;
}

static esp_err_t camera_get_led(int *value)
{
    *value = camera_led_duty;
    return ESP_OK;
}

static const camera_setting_t camera_settings[] = {
    CAMERA_SENSOR_SETTINGS(CAMERA_SETTING_ROW)
    { { "gainceiling",      0, 511                 }, true,  camera_set_gainceiling, camera_get_gainceiling },
    { { CAMERA_SETTING_LED, 0, CAMERA_LED_DUTY_MAX }, false, camera_set_led,         camera_get_led         },
};

#define CAMERA_SETTING_COUNT  ARRAY_SIZE(camera_settings)

/** @brief Every setting's value right after the boot defaults — what
 *         camera_settings_restore_defaults() goes back to. */
static int camera_default_values[CAMERA_SETTING_COUNT];

/** @brief What the next boot will apply (defaults overlaid with the NVS set).
 *         "Unsaved" means the live values differ from this. */
static int camera_saved_values[CAMERA_SETTING_COUNT];

static const camera_setting_t *camera_setting_find(const char *name)
{
    for (size_t i = 0u; i < CAMERA_SETTING_COUNT; i++)
    {
        if (strcmp(camera_settings[i].desc.name, name) == 0)
        {
            return &camera_settings[i];
        }
    }

    return NULL;
}

static bool camera_setting_available(const camera_setting_t *setting)
{
    return camera_mutex && (camera_ready || !setting->needs_sensor);
}

/** @brief Apply @p value to @p setting — range already checked by the caller. */
static esp_err_t camera_setting_apply(const camera_setting_t *setting, int value)
{
    if (!camera_setting_available(setting))
    {
        return ESP_ERR_INVALID_STATE;
    }

    xSemaphoreTake(camera_mutex, portMAX_DELAY);
    const int rc = setting->set(value);
    xSemaphoreGive(camera_mutex);

    return (rc == 0) ? ESP_OK : ESP_FAIL;
}

static esp_err_t camera_setting_read(const camera_setting_t *setting, int *value)
{
    if (!camera_setting_available(setting))
    {
        return ESP_ERR_INVALID_STATE;
    }

    xSemaphoreTake(camera_mutex, portMAX_DELAY);
    const esp_err_t error_code = setting->get(value);
    xSemaphoreGive(camera_mutex);

    return error_code;
}

/** @brief Record every setting's current value as the restore target. */
static void camera_capture_defaults(void)
{
    for (size_t i = 0u; i < CAMERA_SETTING_COUNT; i++)
    {
        if (camera_setting_read(&camera_settings[i], &camera_default_values[i]) != ESP_OK)
        {
            camera_default_values[i] = camera_settings[i].desc.min;
        }

        camera_saved_values[i] = camera_default_values[i];
    }
}

/** @brief Apply every setting found in NVS. Missing keys keep their value. */
static void camera_settings_load(void)
{
    nvs_handle_t h;

    if (nvs_open(CAMERA_NVS_NAMESPACE, NVS_READONLY, &h) != ESP_OK)
    {
        ESP_LOGI(camera_tag, "No saved settings — using defaults");
        return;
    }

    unsigned restored = 0u;

    for (size_t i = 0u; i < CAMERA_SETTING_COUNT; i++)
    {
        const camera_setting_desc_t *desc  = &camera_settings[i].desc;
        int32_t                      value = 0;

        if (nvs_get_i32(h, desc->name, &value) != ESP_OK)
        {
            continue;
        }

        if (value < desc->min || value > desc->max)
        {
            ESP_LOGW(camera_tag, "Saved %s=%ld out of range — ignored",
                     desc->name, (long)value);
            continue;
        }

        if (camera_setting_apply(&camera_settings[i], (int)value) == ESP_OK)
        {
            camera_saved_values[i] = (int)value;
            restored++;
        }
        else
        {
            ESP_LOGW(camera_tag, "Saved %s=%ld rejected by sensor", desc->name, (long)value);
        }
    }

    nvs_close(h);
    ESP_LOGI(camera_tag, "Restored %u saved setting(s)", restored);
}

static esp_err_t camera_led_init(void)
{
    const ledc_timer_config_t timer_cfg = {
        .speed_mode      = LEDC_LOW_SPEED_MODE,
        .duty_resolution = CAMERA_LED_DUTY_RES,
        .timer_num       = BSP_LED_LEDC_TIMER,
        .freq_hz         = BSP_LED_PWM_FREQ_HZ,
        .clk_cfg         = LEDC_AUTO_CLK,
    };

    esp_err_t error_code = ledc_timer_config(&timer_cfg);

    if (error_code == ESP_OK)
    {
        const ledc_channel_config_t channel_cfg = {
            .gpio_num   = BSP_LED_GPIO,
            .speed_mode = LEDC_LOW_SPEED_MODE,
            .channel    = BSP_LED_LEDC_CHANNEL,
            .timer_sel  = BSP_LED_LEDC_TIMER,
            .duty       = 0u,
            .hpoint     = 0,
        };

        error_code = ledc_channel_config(&channel_cfg);
    }

    if (error_code != ESP_OK)
    {
        ESP_LOGE(camera_tag, "LED PWM init failed: %s", esp_err_to_name(error_code));
    }

    return error_code;
}

static void camera_frame_fill(camera_frame_t *frame, camera_fb_t *fb)
{
    frame->buf          = fb->buf;
    frame->len          = fb->len;
    frame->width        = (uint16_t)fb->width;
    frame->height       = (uint16_t)fb->height;
    frame->priv         = fb;
}

/** @brief Apply the boot-time picture defaults. Caller holds camera_mutex. */
static void camera_apply_boot_defaults_locked(sensor_t *s)
{
    /* The OV3660 image comes out upside down and over-saturated with the
     * driver defaults — same correction the Arduino sketch applied. */
    s->set_vflip(s, 1);
    s->set_brightness(s, 1);
    s->set_saturation(s, -2);

    /* Buffers were allocated for CAMERA_FRAMESIZE_MAX; drop to the working
     * resolution. */
    s->set_framesize(s, CAMERA_FRAMESIZE_BOOT);
}


/* Exposed API ------------------------------------------------------------- */

esp_err_t camera_init(void)
{
    esp_err_t error_code = ESP_OK;

    do
    {
        camera_mutex = xSemaphoreCreateMutex();

        if (!camera_mutex)
        {
            error_code = ESP_ERR_NO_MEM;
            break;
        }

        /* LED first — independent of the sensor, and leaves GPIO47 driven
         * low rather than floating if the camera probe fails. */
        error_code = camera_led_init();

        if (error_code != ESP_OK)
        {
            break;
        }

        if (heap_caps_get_total_size(MALLOC_CAP_SPIRAM) == 0u)
        {
            ESP_LOGE(camera_tag, "No PSRAM — frame buffers cannot be allocated");
            error_code = ESP_ERR_NO_MEM;
            break;
        }

        const camera_config_t cfg = {
            .pin_pwdn     = BSP_CAMERA_PWDN,
            .pin_reset    = BSP_CAMERA_RESET,
            .pin_xclk     = BSP_CAMERA_XCLK,
            .pin_sccb_sda = BSP_CAMERA_SIOD,
            .pin_sccb_scl = BSP_CAMERA_SIOC,
            .pin_d7       = BSP_CAMERA_D7,
            .pin_d6       = BSP_CAMERA_D6,
            .pin_d5       = BSP_CAMERA_D5,
            .pin_d4       = BSP_CAMERA_D4,
            .pin_d3       = BSP_CAMERA_D3,
            .pin_d2       = BSP_CAMERA_D2,
            .pin_d1       = BSP_CAMERA_D1,
            .pin_d0       = BSP_CAMERA_D0,
            .pin_vsync    = BSP_CAMERA_VSYNC,
            .pin_href     = BSP_CAMERA_HREF,
            .pin_pclk     = BSP_CAMERA_PCLK,
            .xclk_freq_hz = BSP_CAMERA_XCLK_FREQ_HZ,
            .ledc_timer   = BSP_CAMERA_LEDC_TIMER,
            .ledc_channel = BSP_CAMERA_LEDC_CHANNEL,
            .pixel_format = PIXFORMAT_JPEG,
            /* JPEG buffer size is derived from this (w*h/5), so allocate for
             * the largest resolution — otherwise raising it at runtime
             * would truncate frames. Dropped to the boot size below. */
            .frame_size   = CAMERA_FRAMESIZE_MAX,
            .jpeg_quality = CAMERA_JPEG_QUALITY_BOOT,
            .fb_count     = CAMERA_FB_COUNT,
            .fb_location  = CAMERA_FB_IN_PSRAM,
            .grab_mode    = CAMERA_GRAB_LATEST,
        };

        error_code = esp_camera_init(&cfg);

        if (error_code != ESP_OK)
        {
            ESP_LOGE(camera_tag, "esp_camera_init failed: %s", esp_err_to_name(error_code));
            break;
        }

        sensor_t *s = esp_camera_sensor_get();

        if (!s)
        {
            ESP_LOGE(camera_tag, "Sensor handle unavailable");
            esp_camera_deinit();
            error_code = ESP_ERR_NOT_FOUND;
            break;
        }

        if (s->id.PID != OV3660_PID)
        {
            ESP_LOGW(camera_tag, "Unexpected sensor PID 0x%04x (expected OV3660)",
                     (unsigned)s->id.PID);
        }

        xSemaphoreTake(camera_mutex, portMAX_DELAY);
        camera_apply_boot_defaults_locked(s);
        xSemaphoreGive(camera_mutex);

        camera_ready = true;

        /* Snapshot the defaults before the saved set overrides them. */
        camera_capture_defaults();
        camera_settings_load();

        ESP_LOGI(camera_tag, "%s ready — %ux%u JPEG, quality %d, %u frame buffer(s) in PSRAM",
                 camera_sensor_name(),
                 resolution[CAMERA_FRAMESIZE_BOOT].width,
                 resolution[CAMERA_FRAMESIZE_BOOT].height,
                 CAMERA_JPEG_QUALITY_BOOT,
                 (unsigned)CAMERA_FB_COUNT);
    } while (0);

    return error_code;
}

bool camera_is_ready(void)
{
    return camera_ready;
}

const char *camera_sensor_name(void)
{
    const char *name = "not detected";

    if (camera_ready)
    {
        sensor_t                   *s    = esp_camera_sensor_get();
        const camera_sensor_info_t *info = s ? esp_camera_sensor_get_info(&s->id) : NULL;

        if (info)
        {
            name = info->name;
        }
    }

    return name;
}

/* ---- Settings ----------------------------------------------------------- */

size_t camera_setting_count(void)
{
    return CAMERA_SETTING_COUNT;
}

const camera_setting_desc_t *camera_setting_at(size_t index)
{
    return (index < camera_setting_count()) ? &camera_settings[index].desc : NULL;
}

esp_err_t camera_setting_set(const char *name, int value)
{
    const camera_setting_t *setting = name ? camera_setting_find(name) : NULL;

    if (!setting)
    {
        return ESP_ERR_NOT_FOUND;
    }

    if (value < setting->desc.min || value > setting->desc.max)
    {
        return ESP_ERR_INVALID_ARG;
    }

    const esp_err_t error_code = camera_setting_apply(setting, value);

    if (error_code == ESP_OK)
    {
        ESP_LOGI(camera_tag, "%s = %d", name, value);
    }
    else if (error_code == ESP_FAIL)
    {
        ESP_LOGW(camera_tag, "%s=%d rejected by sensor", name, value);
    }

    return error_code;
}

esp_err_t camera_setting_get(const char *name, int *value)
{
    const camera_setting_t *setting = name ? camera_setting_find(name) : NULL;

    if (!setting || !value)
    {
        return ESP_ERR_NOT_FOUND;
    }

    return camera_setting_read(setting, value);
}

esp_err_t camera_settings_save(void)
{
    esp_err_t    error_code = ESP_OK;
    int          values[CAMERA_SETTING_COUNT];
    nvs_handle_t h;

    do
    {
        if (!camera_ready)
        {
            error_code = ESP_ERR_INVALID_STATE;
            break;
        }

        error_code = nvs_open(CAMERA_NVS_NAMESPACE, NVS_READWRITE, &h);

        if (error_code != ESP_OK)
        {
            break;
        }

        for (size_t i = 0u; i < CAMERA_SETTING_COUNT && error_code == ESP_OK; i++)
        {
            values[i] = camera_saved_values[i];

            if (camera_setting_read(&camera_settings[i], &values[i]) == ESP_OK)
            {
                error_code = nvs_set_i32(h, camera_settings[i].desc.name, values[i]);
            }
        }

        if (error_code == ESP_OK)
        {
            error_code = nvs_commit(h);
        }

        nvs_close(h);
    } while (0);

    if (error_code == ESP_OK)
    {
        memcpy(camera_saved_values, values, sizeof(camera_saved_values));
        ESP_LOGI(camera_tag, "Settings saved");
    }
    else
    {
        ESP_LOGE(camera_tag, "Saving settings failed: %s", esp_err_to_name(error_code));
    }

    return error_code;
}

esp_err_t camera_settings_restore_defaults(void)
{
    esp_err_t error_code = ESP_OK;

    do
    {
        if (!camera_ready)
        {
            error_code = ESP_ERR_INVALID_STATE;
            break;
        }

        for (size_t i = 0u; i < CAMERA_SETTING_COUNT; i++)
        {
            if (camera_setting_apply(&camera_settings[i], camera_default_values[i]) != ESP_OK)
            {
                ESP_LOGW(camera_tag, "Default %s=%d rejected by sensor",
                         camera_settings[i].desc.name, camera_default_values[i]);
            }
        }

        nvs_handle_t h;

        /* ESP_ERR_NVS_NOT_FOUND from open just means nothing was saved. */
        if (nvs_open(CAMERA_NVS_NAMESPACE, NVS_READWRITE, &h) == ESP_OK)
        {
            error_code = nvs_erase_all(h);

            if (error_code == ESP_OK)
            {
                error_code = nvs_commit(h);
            }

            nvs_close(h);
        }
    } while (0);

    if (error_code == ESP_OK)
    {
        memcpy(camera_saved_values, camera_default_values, sizeof(camera_saved_values));
        ESP_LOGI(camera_tag, "Defaults restored, saved settings erased");
    }
    else
    {
        ESP_LOGE(camera_tag, "Restoring defaults failed: %s", esp_err_to_name(error_code));
    }

    return error_code;
}

bool camera_settings_unsaved(void)
{
    for (size_t i = 0u; i < CAMERA_SETTING_COUNT; i++)
    {
        int value = 0;

        if (camera_setting_read(&camera_settings[i], &value) == ESP_OK
            && value != camera_saved_values[i])
        {
            return true;
        }
    }

    return false;
}

esp_err_t camera_framesize_resolution(int framesize, uint16_t *width, uint16_t *height)
{
    if (framesize < 0 || framesize > CAMERA_FRAMESIZE_MAX || !width || !height)
    {
        return ESP_ERR_INVALID_ARG;
    }

    *width  = resolution[framesize].width;
    *height = resolution[framesize].height;
    return ESP_OK;
}

/* ---- Frames ------------------------------------------------------------- */

esp_err_t camera_frame_acquire(camera_frame_t *frame)
{
    esp_err_t error_code = ESP_OK;

    do
    {
        if (!frame)
        {
            error_code = ESP_ERR_INVALID_ARG;
            break;
        }

        memset(frame, 0, sizeof(*frame));

        if (!camera_ready)
        {
            error_code = ESP_ERR_INVALID_STATE;
            break;
        }

        camera_fb_t *fb = esp_camera_fb_get();

        if (!fb)
        {
            ESP_LOGW(camera_tag, "Frame capture failed");
            error_code = ESP_FAIL;
            break;
        }

        camera_frame_fill(frame, fb);
    } while (0);

    return error_code;
}

esp_err_t camera_still_acquire(camera_frame_t *frame)
{
    bool flash = false;

    if (camera_mutex)
    {
        xSemaphoreTake(camera_mutex, portMAX_DELAY);
        flash = camera_ready && (camera_led_duty > 0) && !camera_led_on;

        if (flash)
        {
            camera_led_flash = true;
            camera_led_apply_locked();
        }

        xSemaphoreGive(camera_mutex);
    }

    if (flash)
    {
        vTaskDelay(pdMS_TO_TICKS(CAMERA_FLASH_SETTLE_MS));
    }

    const esp_err_t error_code = camera_frame_acquire(frame);

    if (flash)
    {
        xSemaphoreTake(camera_mutex, portMAX_DELAY);
        camera_led_flash = false;
        camera_led_apply_locked();  /* stays lit if a stream started meanwhile */
        xSemaphoreGive(camera_mutex);
    }

    return error_code;
}

void camera_frame_release(camera_frame_t *frame)
{
    if (frame && frame->priv)
    {
        esp_camera_fb_return((camera_fb_t *)frame->priv);
        frame->priv = NULL;
        frame->buf  = NULL;
        frame->len  = 0u;
    }
}

/* ---- LED ---------------------------------------------------------------- */

void camera_led_enable(bool on)
{
    if (!camera_mutex)
    {
        return;
    }

    xSemaphoreTake(camera_mutex, portMAX_DELAY);
    camera_led_on = on;
    camera_led_apply_locked();
    xSemaphoreGive(camera_mutex);
}

/* ---- Raw register access ------------------------------------------------ */

esp_err_t camera_reg_get(int reg, int mask, int *value)
{
    if (!camera_ready)
    {
        return ESP_ERR_INVALID_STATE;
    }

    if (!value)
    {
        return ESP_ERR_INVALID_ARG;
    }

    xSemaphoreTake(camera_mutex, portMAX_DELAY);
    sensor_t *s  = esp_camera_sensor_get();
    const int rc = s->get_reg(s, reg, mask);
    xSemaphoreGive(camera_mutex);

    if (rc < 0)
    {
        return ESP_FAIL;
    }

    *value = rc;
    return ESP_OK;
}

esp_err_t camera_reg_set(int reg, int mask, int value)
{
    if (!camera_ready)
    {
        return ESP_ERR_INVALID_STATE;
    }

    xSemaphoreTake(camera_mutex, portMAX_DELAY);
    sensor_t *s  = esp_camera_sensor_get();
    const int rc = s->set_reg(s, reg, mask, value);
    xSemaphoreGive(camera_mutex);

    return (rc < 0) ? ESP_FAIL : ESP_OK;
}


/**** END OF FILE ****/
