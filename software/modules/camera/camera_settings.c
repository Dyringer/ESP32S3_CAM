/**
 ******************************************************************************
 * @file    camera_settings.c
 * @author  Daniel Dyringer
 * @date    2026-10-07
 * @brief   Camera settings, registered in data_manager.
 ******************************************************************************
 */


/* Includes ---------------------------------------------------------------- */
#include "camera_settings.h"

#include <stdint.h>

#include "sensor.h"
#include "utils.h"


/* Constants and macros ---------------------------------------------------- */
// clang-format off

/* File-scope compound literals have static storage, so every entry gets its
 * own value and default that live as long as the table. */
#define CAMERA_SETTINGS_ENTRY(setting, low, high, initial)                  \
    {                                                                       \
        .name          = setting##_NAME,                                    \
        .value         = &(int32_t){0},                                     \
        .default_value = &(const int32_t){(initial)},                       \
        .size          = sizeof(int32_t),                                   \
        .id            = (setting),                                         \
        .type          = DATA_MANAGER_TYPE_I32,                             \
        .minimum       = (low),                                             \
        .maximum       = (high),                                            \
        .persistent    = true,                                              \
    }

// clang-format on

/* Private types ----------------------------------------------------------- */


/* Private variables ------------------------------------------------------- */

/* Ranges match the OV3660 driver's bounds checks. Defaults are the driver's
 * reset state, except vflip, brightness and saturation: the image comes out
 * upside down and over-saturated with the driver defaults. */
// clang-format off
static const data_manager_descriptor_t camera_settings_table[] = {
    CAMERA_SETTINGS_ENTRY(CAMERA_FRAMESIZE, 0, FRAMESIZE_QXGA, FRAMESIZE_HD),
    CAMERA_SETTINGS_ENTRY(CAMERA_QUALITY, 4,   63,  10),
    CAMERA_SETTINGS_ENTRY(CAMERA_BRIGHTNESS, -3,    3,   1),
    CAMERA_SETTINGS_ENTRY(CAMERA_CONTRAST, -3,    3,   0),
    CAMERA_SETTINGS_ENTRY(CAMERA_SATURATION, -4,    4,  -2),
    CAMERA_SETTINGS_ENTRY(CAMERA_SHARPNESS, -3,    3,   1),
    CAMERA_SETTINGS_ENTRY(CAMERA_DENOISE, 0,    8,   0),
    CAMERA_SETTINGS_ENTRY(CAMERA_SPECIAL_EFFECT, 0,    6,   0),
    CAMERA_SETTINGS_ENTRY(CAMERA_HMIRROR, 0,    1,   0),
    CAMERA_SETTINGS_ENTRY(CAMERA_VFLIP, 0,    1,   1),
    CAMERA_SETTINGS_ENTRY(CAMERA_AEC, 0,    1,   1),
    CAMERA_SETTINGS_ENTRY(CAMERA_AEC2, 0,    1,   0),
    CAMERA_SETTINGS_ENTRY(CAMERA_AE_LEVEL, -5,    5,   0),
    CAMERA_SETTINGS_ENTRY(CAMERA_AEC_VALUE, 0, 1536, 490),
    CAMERA_SETTINGS_ENTRY(CAMERA_AGC, 0,    1,   1),
    CAMERA_SETTINGS_ENTRY(CAMERA_AGC_GAIN, 0,   64,   2),
    CAMERA_SETTINGS_ENTRY(CAMERA_AWB, 0,    1,   1),
    CAMERA_SETTINGS_ENTRY(CAMERA_AWB_GAIN, 0,    1,   0),
    CAMERA_SETTINGS_ENTRY(CAMERA_WB_MODE, 0,    4,   0),
    CAMERA_SETTINGS_ENTRY(CAMERA_BPC, 0,    1,   1),
    CAMERA_SETTINGS_ENTRY(CAMERA_WPC, 0,    1,   1),
    CAMERA_SETTINGS_ENTRY(CAMERA_RAW_GMA, 0,    1,   1),
    CAMERA_SETTINGS_ENTRY(CAMERA_LENC, 0,    1,   1),
    CAMERA_SETTINGS_ENTRY(CAMERA_DCW, 0,    1,   1),
    CAMERA_SETTINGS_ENTRY(CAMERA_COLORBAR, 0,    1,   0),
    CAMERA_SETTINGS_ENTRY(CAMERA_GAINCEILING, 0,  511, 248),
    CAMERA_SETTINGS_ENTRY(CAMERA_LED_INTENSITY, 0,  255,   0),
};
// clang-format on


/* Private functions ------------------------------------------------------- */


/* Exposed API ------------------------------------------------------------- */

esp_err_t camera_settings_init(data_manager_on_change_t on_change)
{
    return data_manager_register(
        camera_settings_table, ARRAY_SIZE(camera_settings_table), on_change);
}

esp_err_t camera_settings_restore_defaults(void)
{
    return data_manager_reset(camera_settings_table, ARRAY_SIZE(camera_settings_table));
}

/**** END OF FILE ****/
