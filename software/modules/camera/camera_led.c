/**
 ******************************************************************************
 * @file    camera_led.c
 * @author  Daniel Dyringer
 * @date    2026-10-07
 * @brief   Illumination LED — LEDC PWM output only.
 ******************************************************************************
 */


/* Includes ---------------------------------------------------------------- */
#include "camera_led.h"

#include "bsp.h"
#include "driver/ledc.h"
#include "esp_log.h"
#include "utils.h"


/* Constants and macros ---------------------------------------------------- */
// clang-format off

#define CAMERA_LED_DUTY_RES     LEDC_TIMER_8_BIT

// clang-format on

/* Private types ----------------------------------------------------------- */


/* Private variables ------------------------------------------------------- */

static const char* camera_led_tag = "camera_led";


/* Private functions ------------------------------------------------------- */


/* Exposed API ------------------------------------------------------------- */

esp_err_t camera_led_init(void)
{
    esp_err_t error_code = ESP_OK;

    do
    {
        const ledc_timer_config_t timer_config = {
            .speed_mode      = LEDC_LOW_SPEED_MODE,
            .duty_resolution = CAMERA_LED_DUTY_RES,
            .timer_num       = BSP_LED_LEDC_TIMER,
            .freq_hz         = BSP_LED_PWM_FREQ_HZ,
            .clk_cfg         = LEDC_AUTO_CLK,
        };

        error_code = ledc_timer_config(&timer_config);

        if (error_code != ESP_OK)
        {
            break;
        }

        const ledc_channel_config_t channel_config = {
            .gpio_num   = BSP_LED_GPIO,
            .speed_mode = LEDC_LOW_SPEED_MODE,
            .channel    = BSP_LED_LEDC_CHANNEL,
            .timer_sel  = BSP_LED_LEDC_TIMER,
            .duty       = 0u,
            .hpoint     = 0,
        };

        error_code = ledc_channel_config(&channel_config);
    } while (0);

    if (error_code != ESP_OK)
    {
        ESP_LOGE(camera_led_tag, "LED PWM init failed: %s", esp_err_to_name(error_code));
    }

    return error_code;
}

esp_err_t camera_led_set_duty(uint32_t duty)
{
    esp_err_t error_code = ledc_set_duty(
        LEDC_LOW_SPEED_MODE, BSP_LED_LEDC_CHANNEL, MIN(duty, CAMERA_LED_DUTY_MAX));

    if (error_code == ESP_OK)
    {
        error_code = ledc_update_duty(LEDC_LOW_SPEED_MODE, BSP_LED_LEDC_CHANNEL);
    }

    return error_code;
}

/**** END OF FILE ****/
