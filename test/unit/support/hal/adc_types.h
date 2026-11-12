/**
 ******************************************************************************
 * @file    adc_types.h
 * @author  Daniel Dyringer
 * @date    2026-10-28
 * @brief   Host stand-in for the ESP-IDF hal/adc_types.h (oneshot subset).
 ******************************************************************************
 */

/* Define to prevent recursive inclusion ----------------------------------- */
#ifndef HAL_ADC_TYPES_H_
#define HAL_ADC_TYPES_H_

/* Includes ---------------------------------------------------------------- */


/* Constants and macros ---------------------------------------------------- */
// clang-format off


// clang-format on

/* Types ------------------------------------------------------------------- */

typedef enum
{
    ADC_ATTEN_DB_0   = 0,
    ADC_ATTEN_DB_2_5 = 1,
    ADC_ATTEN_DB_6   = 2,
    ADC_ATTEN_DB_12  = 3,
} adc_atten_t;

typedef enum
{
    ADC_BITWIDTH_DEFAULT = 0,
    ADC_BITWIDTH_12      = 12,
} adc_bitwidth_t;

typedef enum
{
    ADC_ULP_MODE_DISABLE = 0,
} adc_ulp_mode_t;


/* Exposed API ------------------------------------------------------------- */


#endif /* HAL_ADC_TYPES_H_ */

/**** END OF FILE ****/
