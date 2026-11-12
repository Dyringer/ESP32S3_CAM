/**
 ******************************************************************************
 * @file    adc_cali.h
 * @author  Daniel Dyringer
 * @date    2026-10-28
 * @brief   Host stand-in for the ESP-IDF esp_adc/adc_cali.h. The fake
 *          calibration is the identity: millivolts = raw.
 ******************************************************************************
 */

/* Define to prevent recursive inclusion ----------------------------------- */
#ifndef ESP_ADC_ADC_CALI_H_
#define ESP_ADC_ADC_CALI_H_

/* Includes ---------------------------------------------------------------- */
#include "esp_err.h"

/* Constants and macros ---------------------------------------------------- */
// clang-format off


// clang-format on

/* Types ------------------------------------------------------------------- */

typedef struct adc_cali_scheme_t* adc_cali_handle_t;


/* Exposed API ------------------------------------------------------------- */

esp_err_t adc_cali_raw_to_voltage(adc_cali_handle_t handle, int raw, int* voltage);


#endif /* ESP_ADC_ADC_CALI_H_ */

/**** END OF FILE ****/
