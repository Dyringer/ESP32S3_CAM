/**
 ******************************************************************************
 * @file    gpio.h
 * @author  Daniel Dyringer
 * @date    2026-10-26
 * @brief   Host stand-in for the ESP-IDF driver/gpio.h. Pad state lives in
 *          fake_smart_io; GPIO 22-25 don't exist and GPIO 46 is input-only.
 ******************************************************************************
 */

/* Define to prevent recursive inclusion ----------------------------------- */
#ifndef DRIVER_GPIO_H_
#define DRIVER_GPIO_H_

/* Includes ---------------------------------------------------------------- */
#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "soc/gpio_num.h"

/* Constants and macros ---------------------------------------------------- */
// clang-format off

#define ESP_INTR_FLAG_IRAM              (1 << 10)

#define GPIO_MODE_DEF_DISABLE           (0)
#define GPIO_MODE_DEF_INPUT             (1 << 0)
#define GPIO_MODE_DEF_OUTPUT            (1 << 1)
#define GPIO_MODE_DEF_OD                (1 << 2)

#define GPIO_IS_VALID_GPIO(gpio)        fake_smart_io_gpio_valid((int32_t)(gpio))
#define GPIO_IS_VALID_OUTPUT_GPIO(gpio) fake_smart_io_gpio_valid_output((int32_t)(gpio))

// clang-format on

/* Types ------------------------------------------------------------------- */

typedef enum
{
    GPIO_INTR_DISABLE = 0,
    GPIO_INTR_POSEDGE = 1,
    GPIO_INTR_NEGEDGE = 2,
    GPIO_INTR_ANYEDGE = 3,
    GPIO_INTR_MAX,
} gpio_int_type_t;

typedef enum
{
    GPIO_MODE_DISABLE   = GPIO_MODE_DEF_DISABLE,
    GPIO_MODE_INPUT     = GPIO_MODE_DEF_INPUT,
    GPIO_MODE_OUTPUT    = GPIO_MODE_DEF_OUTPUT,
    GPIO_MODE_OUTPUT_OD = GPIO_MODE_DEF_OUTPUT | GPIO_MODE_DEF_OD,
    GPIO_MODE_INPUT_OUTPUT_OD =
        GPIO_MODE_DEF_INPUT | GPIO_MODE_DEF_OUTPUT | GPIO_MODE_DEF_OD,
    GPIO_MODE_INPUT_OUTPUT = GPIO_MODE_DEF_INPUT | GPIO_MODE_DEF_OUTPUT,
} gpio_mode_t;

typedef enum
{
    GPIO_PULLUP_DISABLE = 0,
    GPIO_PULLUP_ENABLE  = 1,
} gpio_pullup_t;

typedef enum
{
    GPIO_PULLDOWN_DISABLE = 0,
    GPIO_PULLDOWN_ENABLE  = 1,
} gpio_pulldown_t;

typedef struct
{
    uint64_t pin_bit_mask;
    gpio_mode_t mode;
    gpio_pullup_t pull_up_en;
    gpio_pulldown_t pull_down_en;
    gpio_int_type_t intr_type;
} gpio_config_t;

typedef void (*gpio_isr_t)(void* arg);


/* Exposed API ------------------------------------------------------------- */

bool fake_smart_io_gpio_valid(int32_t gpio);
bool fake_smart_io_gpio_valid_output(int32_t gpio);

esp_err_t gpio_config(const gpio_config_t* config);
esp_err_t gpio_set_level(gpio_num_t gpio_num, uint32_t level);
int gpio_get_level(gpio_num_t gpio_num);
esp_err_t gpio_set_intr_type(gpio_num_t gpio_num, gpio_int_type_t intr_type);
esp_err_t gpio_intr_enable(gpio_num_t gpio_num);
esp_err_t gpio_intr_disable(gpio_num_t gpio_num);
esp_err_t gpio_install_isr_service(int intr_alloc_flags);
esp_err_t gpio_isr_handler_add(gpio_num_t gpio_num, gpio_isr_t isr_handler, void* args);
esp_err_t gpio_isr_handler_remove(gpio_num_t gpio_num);


#endif /* DRIVER_GPIO_H_ */

/**** END OF FILE ****/
