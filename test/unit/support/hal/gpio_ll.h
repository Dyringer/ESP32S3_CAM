/**
 ******************************************************************************
 * @file    gpio_ll.h
 * @author  Daniel Dyringer
 * @date    2026-10-26
 * @brief   Host stand-in for the ESP-IDF hal/gpio_ll.h: reads the fake pad
 *          and masks its interrupt.
 ******************************************************************************
 */

/* Define to prevent recursive inclusion ----------------------------------- */
#ifndef HAL_GPIO_LL_H_
#define HAL_GPIO_LL_H_

/* Includes ---------------------------------------------------------------- */
#include <stdint.h>

/* Constants and macros ---------------------------------------------------- */
// clang-format off


// clang-format on

/* Types ------------------------------------------------------------------- */

typedef struct fake_smart_io_gpio_dev_s
{
    uint32_t unused;
} gpio_dev_t;


/* Exposed API ------------------------------------------------------------- */

extern gpio_dev_t GPIO;

int gpio_ll_get_level(gpio_dev_t* hw, uint32_t gpio_num);
void gpio_ll_intr_disable(gpio_dev_t* hw, uint32_t gpio_num);


#endif /* HAL_GPIO_LL_H_ */

/**** END OF FILE ****/
