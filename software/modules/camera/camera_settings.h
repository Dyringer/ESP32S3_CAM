/**
 ******************************************************************************
 * @file    camera_settings.h
 * @author  Daniel Dyringer
 * @date    2026-10-07
 * @brief   Camera settings, registered in data_manager. Access them with
 *          data_manager_get/set and the ids below.
 ******************************************************************************
 */

/* Define to prevent recursive inclusion ----------------------------------- */
#ifndef CAMERA_SETTINGS_H_
#define CAMERA_SETTINGS_H_

/* Includes ---------------------------------------------------------------- */
#include "data_manager.h"
#include "esp_err.h"

/* Constants and macros ---------------------------------------------------- */
// clang-format off

#define CAMERA_SETTINGS_PREFIX      "camera_"

/* All int32_t. */
#define CAMERA_FRAMESIZE_NAME       "camera_framesize"
#define CAMERA_FRAMESIZE            DATA_MANAGER_ID(CAMERA_FRAMESIZE_NAME)
#define CAMERA_QUALITY_NAME         "camera_quality"
#define CAMERA_QUALITY              DATA_MANAGER_ID(CAMERA_QUALITY_NAME)
#define CAMERA_BRIGHTNESS_NAME      "camera_brightness"
#define CAMERA_BRIGHTNESS           DATA_MANAGER_ID(CAMERA_BRIGHTNESS_NAME)
#define CAMERA_CONTRAST_NAME        "camera_contrast"
#define CAMERA_CONTRAST             DATA_MANAGER_ID(CAMERA_CONTRAST_NAME)
#define CAMERA_SATURATION_NAME      "camera_saturation"
#define CAMERA_SATURATION           DATA_MANAGER_ID(CAMERA_SATURATION_NAME)
#define CAMERA_SHARPNESS_NAME       "camera_sharpness"
#define CAMERA_SHARPNESS            DATA_MANAGER_ID(CAMERA_SHARPNESS_NAME)
#define CAMERA_DENOISE_NAME         "camera_denoise"
#define CAMERA_DENOISE              DATA_MANAGER_ID(CAMERA_DENOISE_NAME)
#define CAMERA_SPECIAL_EFFECT_NAME  "camera_special_effect"
#define CAMERA_SPECIAL_EFFECT       DATA_MANAGER_ID(CAMERA_SPECIAL_EFFECT_NAME)
#define CAMERA_HMIRROR_NAME         "camera_hmirror"
#define CAMERA_HMIRROR              DATA_MANAGER_ID(CAMERA_HMIRROR_NAME)
#define CAMERA_VFLIP_NAME           "camera_vflip"
#define CAMERA_VFLIP                DATA_MANAGER_ID(CAMERA_VFLIP_NAME)
#define CAMERA_AEC_NAME             "camera_aec"
#define CAMERA_AEC                  DATA_MANAGER_ID(CAMERA_AEC_NAME)
#define CAMERA_AEC2_NAME            "camera_aec2"
#define CAMERA_AEC2                 DATA_MANAGER_ID(CAMERA_AEC2_NAME)
#define CAMERA_AE_LEVEL_NAME        "camera_ae_level"
#define CAMERA_AE_LEVEL             DATA_MANAGER_ID(CAMERA_AE_LEVEL_NAME)
#define CAMERA_AEC_VALUE_NAME       "camera_aec_value"
#define CAMERA_AEC_VALUE            DATA_MANAGER_ID(CAMERA_AEC_VALUE_NAME)
#define CAMERA_AGC_NAME             "camera_agc"
#define CAMERA_AGC                  DATA_MANAGER_ID(CAMERA_AGC_NAME)
#define CAMERA_AGC_GAIN_NAME        "camera_agc_gain"
#define CAMERA_AGC_GAIN             DATA_MANAGER_ID(CAMERA_AGC_GAIN_NAME)
#define CAMERA_AWB_NAME             "camera_awb"
#define CAMERA_AWB                  DATA_MANAGER_ID(CAMERA_AWB_NAME)
#define CAMERA_AWB_GAIN_NAME        "camera_awb_gain"
#define CAMERA_AWB_GAIN             DATA_MANAGER_ID(CAMERA_AWB_GAIN_NAME)
#define CAMERA_WB_MODE_NAME         "camera_wb_mode"
#define CAMERA_WB_MODE              DATA_MANAGER_ID(CAMERA_WB_MODE_NAME)
#define CAMERA_BPC_NAME             "camera_bpc"
#define CAMERA_BPC                  DATA_MANAGER_ID(CAMERA_BPC_NAME)
#define CAMERA_WPC_NAME             "camera_wpc"
#define CAMERA_WPC                  DATA_MANAGER_ID(CAMERA_WPC_NAME)
#define CAMERA_RAW_GMA_NAME         "camera_raw_gma"
#define CAMERA_RAW_GMA              DATA_MANAGER_ID(CAMERA_RAW_GMA_NAME)
#define CAMERA_LENC_NAME            "camera_lenc"
#define CAMERA_LENC                 DATA_MANAGER_ID(CAMERA_LENC_NAME)
#define CAMERA_DCW_NAME             "camera_dcw"
#define CAMERA_DCW                  DATA_MANAGER_ID(CAMERA_DCW_NAME)
#define CAMERA_COLORBAR_NAME        "camera_colorbar"
#define CAMERA_COLORBAR             DATA_MANAGER_ID(CAMERA_COLORBAR_NAME)
#define CAMERA_GAINCEILING_NAME     "camera_gainceiling"
#define CAMERA_GAINCEILING          DATA_MANAGER_ID(CAMERA_GAINCEILING_NAME)
#define CAMERA_LED_INTENSITY_NAME   "camera_led_intensity"
#define CAMERA_LED_INTENSITY        DATA_MANAGER_ID(CAMERA_LED_INTENSITY_NAME)

// clang-format on

/* Types ------------------------------------------------------------------- */


/* Exposed API ------------------------------------------------------------- */

esp_err_t camera_settings_init(data_manager_on_change_t on_change);
esp_err_t camera_settings_restore_defaults(void);

#endif /* CAMERA_SETTINGS_H_ */

/**** END OF FILE ****/
