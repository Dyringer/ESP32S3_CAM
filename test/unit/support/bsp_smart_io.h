/**
 ******************************************************************************
 * @file    bsp_smart_io.h
 * @author  Daniel Dyringer
 * @date    2026-10-26
 * @brief   Host stand-in for boards/<board>/bsp_smart_io.h. The pool exercises
 *          every init check: 38 has no ADC channel, 46 is input-only in
 *          the fake, 47 is the LED, 12 is listed twice and 22 doesn't
 *          exist. Effective pool: 10, 11, 12, 13, 38, 46.
 ******************************************************************************
 */

/* Define to prevent recursive inclusion ----------------------------------- */
#ifndef BSP_SMART_IO_H_
#define BSP_SMART_IO_H_

/* Includes ---------------------------------------------------------------- */
#include "hal/ledc_types.h"
#include "hal/uart_types.h"
#include "smart_io_caps.h"
#include "soc/gpio_num.h"


/* Constants and macros ---------------------------------------------------- */
// clang-format off

/* 38 has no ADC, 46 is input-only, 12 and 13 are restricted by the board. */
#define BSP_SMART_IO_PINS(X)                  \
    X(GPIO_NUM_10, SMART_IO_CAP_ALL)          \
    X(GPIO_NUM_11, SMART_IO_CAP_ALL)          \
    X(GPIO_NUM_12, SMART_IO_CAP_GENERIC)      \
    X(GPIO_NUM_13, SMART_IO_CAP_GENERIC)      \
    X(GPIO_NUM_38, SMART_IO_CAP_ALL)          \
    X(GPIO_NUM_46, SMART_IO_CAP_ALL)

#define BSP_SMART_IO_LEDC_TIMER_FIRST     LEDC_TIMER_2
#define BSP_SMART_IO_LEDC_CHANNEL_FIRST   LEDC_CHANNEL_2
#define BSP_SMART_IO_UART_PORT_FIRST      UART_NUM_1

// clang-format on

/* Types ------------------------------------------------------------------- */


/* Exposed API ------------------------------------------------------------- */


#endif /* BSP_SMART_IO_H_ */

/**** END OF FILE ****/
