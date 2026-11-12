/**
 ******************************************************************************
 * @file    smart_io_pad.c
 * @author  Daniel Dyringer
 * @date    2026-11-06
 * @brief   Pool pins as pads: what the silicon allows and the released
 *          state.
 ******************************************************************************
 */


/* Includes ---------------------------------------------------------------- */
#include "smart_io_pad.h"

#include "driver/gpio.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_log.h"
#include "esp_private/esp_gpio_reserve.h"
#include "hal/gpio_types.h"
#include "smart_io_caps.h"


/* Constants and macros ---------------------------------------------------- */
// clang-format off

#define SMART_IO_PAD_OUTPUT_CAPS    (SMART_IO_CAP(SMART_IO_FUNCTION_OUTPUT)     \
                                     | SMART_IO_CAP_TIMER                       \
                                     | SMART_IO_CAP(SMART_IO_FUNCTION_I2C_SDA)  \
                                     | SMART_IO_CAP(SMART_IO_FUNCTION_I2C_SCL)  \
                                     | SMART_IO_CAP(SMART_IO_FUNCTION_UART_TX))

// clang-format on

/* Private types ----------------------------------------------------------- */


/* Private variables ------------------------------------------------------- */

static const char* smart_io_pad_tag = "smart_io_pad";


/* Private functions ------------------------------------------------------- */


/* Exposed API ------------------------------------------------------------- */

uint32_t smart_io_pad_caps(gpio_num_t gpio, uint32_t board_caps)
{
    uint32_t caps         = board_caps & (uint32_t)SMART_IO_CAP_ALL;
    adc_unit_t unit       = ADC_UNIT_1;
    adc_channel_t channel = ADC_CHANNEL_0;

    if (adc_oneshot_io_to_channel((int)gpio, &unit, &channel) != ESP_OK)
    {
        caps &= ~(uint32_t)SMART_IO_CAP_ADC;
    }

    if (!GPIO_IS_VALID_OUTPUT_GPIO(gpio))
    {
        caps &= ~(uint32_t)SMART_IO_PAD_OUTPUT_CAPS;
    }

    return caps;
}

void smart_io_pad_park(gpio_num_t gpio)
{
    if (gpio != GPIO_NUM_NC)
    {
        const uint64_t mask           = (uint64_t)1u << (uint32_t)gpio;
        const gpio_config_t io_config = {
            .pin_bit_mask = mask,
            .mode         = GPIO_MODE_INPUT,
            .pull_up_en   = GPIO_PULLUP_DISABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type    = GPIO_INTR_DISABLE,
        };

        /* NOTE: gpio_config() reserves a pin it makes an output and never
         * revokes it, so switching back to input logs "conflict found for
         * GPIO[n]" (IDF 6.1 esp_driver_gpio/src/gpio.c, gpio_config). Pool
         * pins belong to smart_io alone, so the reservation is dropped here. */
        esp_gpio_revoke(mask);

        const esp_err_t error_code = gpio_config(&io_config);

        if (error_code != ESP_OK)
        {
            ESP_LOGE(
                smart_io_pad_tag,
                "GPIO %d: release failed (%s)",
                (int)gpio,
                esp_err_to_name(error_code));
        }
    }
}

/**** END OF FILE ****/
