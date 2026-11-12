/**
 ******************************************************************************
 * @file    camera.c
 * @author  Daniel Dyringer
 * @date    2026-09-30
 * @brief   Camera core — OV3660 bring-up, frame access and illumination LED.
 *          Applies camera_settings values to the hardware.
 ******************************************************************************
 */


/* Includes ---------------------------------------------------------------- */
#include "camera.h"

#include <string.h>

#include "bsp.h"
#include "camera_led.h"
#include "camera_settings.h"
#include "esp_camera.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "utils.h"


/* Constants and macros ---------------------------------------------------- */
// clang-format off

/* Two buffers let capture overlap transmission. */
#define CAMERA_FB_COUNT             2u

/* Long enough for at least one full frame to be exposed with the LED on,
 * since CAMERA_GRAB_LATEST keeps the driver capturing. */
#define CAMERA_FLASH_SETTLE_MS      150u

/* ---- Sensor appliers ---- */

/* Order is the apply order: mode switches (aec, agc, awb_gain) precede the
 * values they gate. */
#define CAMERA_SENSOR_METHODS(X)                                        \
    X(CAMERA_FRAMESIZE,      set_framesize)                             \
    X(CAMERA_QUALITY,        set_quality)                               \
    X(CAMERA_BRIGHTNESS,     set_brightness)                            \
    X(CAMERA_CONTRAST,       set_contrast)                              \
    X(CAMERA_SATURATION,     set_saturation)                            \
    X(CAMERA_SHARPNESS,      set_sharpness)                             \
    X(CAMERA_DENOISE,        set_denoise)                               \
    X(CAMERA_SPECIAL_EFFECT, set_special_effect)                        \
    X(CAMERA_HMIRROR,        set_hmirror)                               \
    X(CAMERA_VFLIP,          set_vflip)                                 \
    X(CAMERA_AEC,            set_exposure_ctrl)                         \
    X(CAMERA_AEC2,           set_aec2)                                  \
    X(CAMERA_AE_LEVEL,       set_ae_level)                              \
    X(CAMERA_AEC_VALUE,      set_aec_value)                             \
    X(CAMERA_AGC,            set_gain_ctrl)                             \
    X(CAMERA_AGC_GAIN,       set_agc_gain)                              \
    X(CAMERA_AWB,            set_whitebal)                              \
    X(CAMERA_AWB_GAIN,       set_awb_gain)                              \
    X(CAMERA_WB_MODE,        set_wb_mode)                               \
    X(CAMERA_BPC,            set_bpc)                                   \
    X(CAMERA_WPC,            set_wpc)                                   \
    X(CAMERA_RAW_GMA,        set_raw_gma)                               \
    X(CAMERA_LENC,           set_lenc)                                  \
    X(CAMERA_DCW,            set_dcw)                                   \
    X(CAMERA_COLORBAR,       set_colorbar)                              \
    X(CAMERA_GAINCEILING,    set_gainceiling)

#define CAMERA_SENSOR_ADAPTER(setting, method)                          \
    static int camera_sensor_##method(sensor_t* sensor, int value)      \
    {                                                                   \
        return sensor->method(sensor, value);                           \
    }

#define CAMERA_SENSOR_ROW(setting, method)  {(setting), camera_sensor_##method},

// clang-format on

/* Private types ----------------------------------------------------------- */

/* Called with camera_mutex held. Returns 0 on success, like sensor_t. */
typedef int (*camera_sensor_apply_fn_t)(sensor_t* sensor, int value);

typedef struct camera_sensor_applier_s
{
    uint32_t id;
    camera_sensor_apply_fn_t apply;
} camera_sensor_applier_t;


/* Private variables ------------------------------------------------------- */

static const char* camera_tag = "camera";

static bool camera_ready              = false;
static SemaphoreHandle_t camera_mutex = NULL;

/* Guarded by camera_mutex */
static int camera_light_intensity = 0;
static bool camera_light_on       = false;
static bool camera_light_flash    = false;


/* Private functions ------------------------------------------------------- */

/* ---- Sensor appliers ---- */

CAMERA_SENSOR_METHODS(CAMERA_SENSOR_ADAPTER)

static const camera_sensor_applier_t camera_sensor_appliers[] = {
    CAMERA_SENSOR_METHODS(CAMERA_SENSOR_ROW)};

/* ---- Apply ---- */

static void camera_light_apply_locked(void)
{
    const uint32_t duty =
        (camera_light_on || camera_light_flash) ? (uint32_t)camera_light_intensity : 0u;
    const esp_err_t error_code = camera_led_set_duty(duty);

    if (error_code != ESP_OK)
    {
        ESP_LOGW(
            camera_tag,
            "LED duty %lu failed: %s",
            (unsigned long)duty,
            esp_err_to_name(error_code));
    }
}

static camera_sensor_apply_fn_t camera_sensor_applier_find(uint32_t id)
{
    camera_sensor_apply_fn_t apply = NULL;

    for (size_t i = 0u; i < ARRAY_SIZE(camera_sensor_appliers); i++)
    {
        if (camera_sensor_appliers[i].id == id)
        {
            apply = camera_sensor_appliers[i].apply;
            break;
        }
    }

    return apply;
}

static void camera_apply_locked(uint32_t id)
{
    int32_t value = 0;

    if (data_manager_get(id, &value, sizeof(value)) != ESP_OK)
    {
        ESP_LOGW(camera_tag, "Reading setting 0x%08lx failed", (unsigned long)id);
    }
    else if (id == CAMERA_LED_INTENSITY)
    {
        camera_light_intensity = (int)value;
        camera_light_apply_locked();
    }
    else
    {
        const camera_sensor_apply_fn_t apply = camera_sensor_applier_find(id);

        if ((apply != NULL) && (apply(esp_camera_sensor_get(), (int)value) != 0))
        {
            ESP_LOGW(
                camera_tag,
                "%s=%ld rejected by sensor",
                data_manager_descriptor(id)->name,
                (long)value);
        }
    }
}

static void camera_apply_all_locked(void)
{
    for (size_t i = 0u; i < ARRAY_SIZE(camera_sensor_appliers); i++)
    {
        camera_apply_locked(camera_sensor_appliers[i].id);
    }

    camera_apply_locked(CAMERA_LED_INTENSITY);
}

/* Runs in the work_queue task. Applying one setting is a short SCCB write, so
 * it is done right away rather than debounced. */
static void camera_on_settings_change(uint32_t id)
{
    xSemaphoreTake(camera_mutex, portMAX_DELAY);

    if (camera_ready)
    {
        camera_apply_locked(id);
    }

    xSemaphoreGive(camera_mutex);
}

/* ---- Frames ---- */

static void camera_frame_fill(camera_frame_t* frame, camera_fb_t* frame_buffer)
{
    frame->buffer       = frame_buffer->buf;
    frame->length       = frame_buffer->len;
    frame->width        = (uint16_t)frame_buffer->width;
    frame->height       = (uint16_t)frame_buffer->height;
    frame->driver_frame = frame_buffer;
}


/* Exposed API ------------------------------------------------------------- */

esp_err_t camera_init(void)
{
    esp_err_t error_code = ESP_OK;

    do
    {
        camera_mutex = xSemaphoreCreateMutex();

        if (camera_mutex == NULL)
        {
            error_code = ESP_ERR_NO_MEM;
            break;
        }

        error_code = camera_settings_init(camera_on_settings_change);

        if (error_code != ESP_OK)
        {
            break;
        }

        /* Before the probe, so the LED pin is driven low even if it fails. */
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

        const data_manager_descriptor_t* const framesize =
            data_manager_descriptor(CAMERA_FRAMESIZE);
        int32_t quality = 0;

        error_code = data_manager_get(CAMERA_QUALITY, &quality, sizeof(quality));

        if ((error_code != ESP_OK) || (framesize == NULL))
        {
            ESP_LOGE(camera_tag, "Settings unavailable");
            error_code = ESP_ERR_INVALID_STATE;
            break;
        }

        const camera_config_t config = {
            .pin_pwdn  = BSP_CAMERA_PWDN,
            .pin_reset = BSP_CAMERA_RESET,
            .pin_xclk  = BSP_CAMERA_XCLK,

            /* -1 makes the driver attach to the shared bus from i2c_bus. */
            .pin_sccb_sda  = -1,
            .pin_sccb_scl  = -1,
            .sccb_i2c_port = BSP_I2C_PORT,

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
            /* Sizes the JPEG buffers, so raising resolution at runtime
             * won't truncate frames. The stored framesize is applied below. */
            .frame_size   = (framesize_t)framesize->maximum,
            .jpeg_quality = (int)quality,
            .fb_count     = CAMERA_FB_COUNT,
            .fb_location  = CAMERA_FB_IN_PSRAM,
            .grab_mode    = CAMERA_GRAB_LATEST,
        };

        error_code = esp_camera_init(&config);

        if (error_code != ESP_OK)
        {
            ESP_LOGE(camera_tag, "esp_camera_init failed: %s", esp_err_to_name(error_code));
            break;
        }

        const sensor_t* sensor = esp_camera_sensor_get();

        if (sensor == NULL)
        {
            ESP_LOGE(camera_tag, "Sensor handle unavailable");
            esp_camera_deinit();
            error_code = ESP_ERR_NOT_FOUND;
            break;
        }

        if (sensor->id.PID != OV3660_PID)
        {
            ESP_LOGW(
                camera_tag,
                "Unexpected sensor PID 0x%04x (expected OV3660)",
                (unsigned)sensor->id.PID);
        }

        xSemaphoreTake(camera_mutex, portMAX_DELAY);
        camera_apply_all_locked();
        camera_ready = true;
        xSemaphoreGive(camera_mutex);

        ESP_LOGI(
            camera_tag,
            "%s ready — JPEG, %u frame buffer(s) in PSRAM",
            camera_sensor_name(),
            (unsigned)CAMERA_FB_COUNT);
    } while (0);

    return error_code;
}

bool camera_is_ready(void)
{
    return camera_ready;
}

const char* camera_sensor_name(void)
{
    const char* name = "not detected";

    if (camera_ready)
    {
        sensor_t* sensor = esp_camera_sensor_get();
        const camera_sensor_info_t* info =
            (sensor != NULL) ? esp_camera_sensor_get_info(&sensor->id) : NULL;

        if (info != NULL)
        {
            name = info->name;
        }
    }

    return name;
}

esp_err_t camera_framesize_resolution(int framesize, uint16_t* width, uint16_t* height)
{
    esp_err_t error_code = ESP_OK;

    const data_manager_descriptor_t* const descriptor =
        data_manager_descriptor(CAMERA_FRAMESIZE);

    if ((descriptor == NULL) || (framesize < descriptor->minimum)
        || (framesize > descriptor->maximum) || (width == NULL) || (height == NULL))
    {
        error_code = ESP_ERR_INVALID_ARG;
    }
    else
    {
        *width  = resolution[framesize].width;
        *height = resolution[framesize].height;
    }

    return error_code;
}

/* ---- Frames ---- */

esp_err_t camera_frame_acquire(camera_frame_t* frame)
{
    esp_err_t error_code = ESP_OK;

    do
    {
        if (frame == NULL)
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

        camera_fb_t* frame_buffer = esp_camera_fb_get();

        if (frame_buffer == NULL)
        {
            ESP_LOGW(camera_tag, "Frame capture failed");
            error_code = ESP_FAIL;
            break;
        }

        camera_frame_fill(frame, frame_buffer);
    } while (0);

    return error_code;
}

esp_err_t camera_still_acquire(camera_frame_t* frame)
{
    bool flash = false;

    if (camera_mutex != NULL)
    {
        xSemaphoreTake(camera_mutex, portMAX_DELAY);
        flash = camera_ready && (camera_light_intensity > 0) && !camera_light_on;

        if (flash)
        {
            camera_light_flash = true;
            camera_light_apply_locked();
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
        camera_light_flash = false;
        camera_light_apply_locked(); /* stays lit if a stream started meanwhile */
        xSemaphoreGive(camera_mutex);
    }

    return error_code;
}

void camera_frame_release(camera_frame_t* frame)
{
    if ((frame != NULL) && (frame->driver_frame != NULL))
    {
        esp_camera_fb_return((camera_fb_t*)frame->driver_frame);
        frame->driver_frame = NULL;
        frame->buffer       = NULL;
        frame->length       = 0u;
    }
}

/* ---- Light ---- */

void camera_light_enable(bool on)
{
    if (camera_mutex != NULL)
    {
        xSemaphoreTake(camera_mutex, portMAX_DELAY);
        camera_light_on = on;
        camera_light_apply_locked();
        xSemaphoreGive(camera_mutex);
    }
}

/**** END OF FILE ****/
