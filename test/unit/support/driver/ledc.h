/**
 ******************************************************************************
 * @file    ledc.h
 * @author  Daniel Dyringer
 * @date    2026-10-28
 * @brief   Host stand-in for the ESP-IDF driver/ledc.h (the calls smart_io
 *          uses). State lives in fake_smart_io_ledc.
 ******************************************************************************
 */

/* Define to prevent recursive inclusion ----------------------------------- */
#ifndef DRIVER_LEDC_H_
#define DRIVER_LEDC_H_

/* Includes ---------------------------------------------------------------- */
#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "hal/ledc_types.h"

/* Constants and macros ---------------------------------------------------- */
// clang-format off


// clang-format on

/* Types ------------------------------------------------------------------- */

typedef enum
{
    LEDC_LOW_SPEED_MODE = 0,
    LEDC_SPEED_MODE_MAX,
} ledc_mode_t;

typedef enum
{
    LEDC_AUTO_CLK = 0,
} ledc_clk_cfg_t;

typedef enum
{
    LEDC_TIMER_1_BIT  = 1,
    LEDC_TIMER_14_BIT = 14,
    LEDC_TIMER_BIT_MAX,
} ledc_timer_bit_t;

typedef struct
{
    int gpio_num;
    ledc_mode_t speed_mode;
    ledc_channel_t channel;
    ledc_timer_t timer_sel;
    uint32_t duty;
    int hpoint;
    bool deconfigure;
} ledc_channel_config_t;

typedef struct
{
    ledc_mode_t speed_mode;
    ledc_timer_bit_t duty_resolution;
    ledc_timer_t timer_num;
    uint32_t freq_hz;
    ledc_clk_cfg_t clk_cfg;
    bool deconfigure;
} ledc_timer_config_t;


/* Exposed API ------------------------------------------------------------- */

esp_err_t ledc_timer_config(const ledc_timer_config_t* timer_conf);
esp_err_t ledc_channel_config(const ledc_channel_config_t* ledc_conf);
uint32_t ledc_find_suitable_duty_resolution(uint32_t src_clk_freq, uint32_t timer_freq);
esp_err_t ledc_set_duty(ledc_mode_t speed_mode, ledc_channel_t channel, uint32_t duty);
esp_err_t ledc_update_duty(ledc_mode_t speed_mode, ledc_channel_t channel);
esp_err_t ledc_stop(ledc_mode_t speed_mode, ledc_channel_t channel, uint32_t idle_level);
esp_err_t ledc_timer_pause(ledc_mode_t speed_mode, ledc_timer_t timer_sel);


#endif /* DRIVER_LEDC_H_ */

/**** END OF FILE ****/
