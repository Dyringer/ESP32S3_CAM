/**
 ******************************************************************************
 * @file    adc_oneshot.h
 * @author  Daniel Dyringer
 * @date    2026-10-26
 * @brief   Host stand-in for the ESP-IDF esp_adc/adc_oneshot.h with the ESP32-S3
 *          pad map: GPIO 1-10 on ADC1, GPIO 11-20 on ADC2.
 ******************************************************************************
 */

/* Define to prevent recursive inclusion ----------------------------------- */
#ifndef ESP_ADC_ADC_ONESHOT_H_
#define ESP_ADC_ADC_ONESHOT_H_

/* Includes ---------------------------------------------------------------- */
#include "esp_err.h"
#include "hal/adc_types.h"

/* Constants and macros ---------------------------------------------------- */
// clang-format off


// clang-format on

/* Types ------------------------------------------------------------------- */

typedef enum
{
    ADC_UNIT_1 = 0,
    ADC_UNIT_2 = 1,
} adc_unit_t;

typedef enum
{
    ADC_CHANNEL_0 = 0,
    ADC_CHANNEL_1,
    ADC_CHANNEL_2,
    ADC_CHANNEL_3,
    ADC_CHANNEL_4,
    ADC_CHANNEL_5,
    ADC_CHANNEL_6,
    ADC_CHANNEL_7,
    ADC_CHANNEL_8,
    ADC_CHANNEL_9,
} adc_channel_t;

typedef struct adc_oneshot_unit_ctx_t* adc_oneshot_unit_handle_t;

typedef struct
{
    adc_unit_t unit_id;
    uint32_t clk_src;
    adc_ulp_mode_t ulp_mode;
} adc_oneshot_unit_init_cfg_t;

typedef struct
{
    adc_atten_t atten;
    adc_bitwidth_t bitwidth;
} adc_oneshot_chan_cfg_t;


/* Exposed API ------------------------------------------------------------- */

esp_err_t adc_oneshot_io_to_channel(
    int io_num,
    adc_unit_t* const unit_id,
    adc_channel_t* const channel);
esp_err_t adc_oneshot_new_unit(
    const adc_oneshot_unit_init_cfg_t* init_config,
    adc_oneshot_unit_handle_t* ret_unit);
esp_err_t adc_oneshot_config_channel(
    adc_oneshot_unit_handle_t handle,
    adc_channel_t channel,
    const adc_oneshot_chan_cfg_t* config);
esp_err_t adc_oneshot_read(adc_oneshot_unit_handle_t handle, adc_channel_t channel, int* out_raw);
esp_err_t adc_oneshot_del_unit(adc_oneshot_unit_handle_t handle);


#endif /* ESP_ADC_ADC_ONESHOT_H_ */

/**** END OF FILE ****/
