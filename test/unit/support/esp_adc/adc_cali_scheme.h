/**
 ******************************************************************************
 * @file    adc_cali_scheme.h
 * @author  Daniel Dyringer
 * @date    2026-10-28
 * @brief   Host stand-in for the ESP-IDF esp_adc/adc_cali_scheme.h (curve
 *          fitting only, like the ESP32-S3).
 ******************************************************************************
 */

/* Define to prevent recursive inclusion ----------------------------------- */
#ifndef ESP_ADC_ADC_CALI_SCHEME_H_
#define ESP_ADC_ADC_CALI_SCHEME_H_

/* Includes ---------------------------------------------------------------- */
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_oneshot.h"

/* Constants and macros ---------------------------------------------------- */
// clang-format off

#define ADC_CALI_SCHEME_CURVE_FITTING_SUPPORTED     1

// clang-format on

/* Types ------------------------------------------------------------------- */

typedef struct
{
    adc_unit_t unit_id;
    adc_channel_t chan;
    adc_atten_t atten;
    adc_bitwidth_t bitwidth;
} adc_cali_curve_fitting_config_t;


/* Exposed API ------------------------------------------------------------- */

esp_err_t adc_cali_create_scheme_curve_fitting(
    const adc_cali_curve_fitting_config_t* config,
    adc_cali_handle_t* ret_handle);
esp_err_t adc_cali_delete_scheme_curve_fitting(adc_cali_handle_t handle);


#endif /* ESP_ADC_ADC_CALI_SCHEME_H_ */

/**** END OF FILE ****/
