/**
 ******************************************************************************
 * @file    bsp_smart_io.h
 * @author  Daniel Dyringer
 * @date    2026-10-26
 * @brief   smart_io pin pool and peripherals for the esp32s3_n16r8_dk board.
 *          Included only by the smart_io component.
 ******************************************************************************
 */

/* Define to prevent recursive inclusion ----------------------------------- */
#ifndef BSP_SMART_IO_H_
#define BSP_SMART_IO_H_

/* Includes ---------------------------------------------------------------- */
#include "hal/i2c_types.h"
#include "hal/ledc_types.h"
#include "hal/uart_types.h"
#include "smart_io_caps.h"
#include "soc/gpio_num.h"


/* Constants and macros ---------------------------------------------------- */
// clang-format off

/* ---- Pool: pins the user may reassign at runtime ---- */
/*    One line per pin. SMART_IO_CAP_ALL offers everything the pad supports;
 *    use a narrower mask for a pin wired to something. GPIO 19/20 stay out:
 *    reserved for a future USB transport.                                   */
#define BSP_SMART_IO_PINS(X)                  \
    X(GPIO_NUM_10, SMART_IO_CAP_ALL)          \
    X(GPIO_NUM_11, SMART_IO_CAP_ALL)          \
    X(GPIO_NUM_12, SMART_IO_CAP_ALL)          \
    X(GPIO_NUM_13, SMART_IO_CAP_ALL)          \
    X(GPIO_NUM_38, SMART_IO_CAP_ALL)          \
    X(GPIO_NUM_39, SMART_IO_CAP_ALL)          \
    X(GPIO_NUM_40, SMART_IO_CAP_ALL)          \
    X(GPIO_NUM_41, SMART_IO_CAP_ALL)          \
    X(GPIO_NUM_42, SMART_IO_CAP_ALL)

/* ---- First peripheral instances smart_io may use, up to the SoC's last ---- */
#define BSP_SMART_IO_LEDC_TIMER_FIRST     LEDC_TIMER_2      /* 0: camera XCLK, 1: LED */
#define BSP_SMART_IO_LEDC_CHANNEL_FIRST   LEDC_CHANNEL_2    /* 0: camera XCLK, 1: LED */
#define BSP_SMART_IO_UART_PORT_FIRST      UART_NUM_1        /* 0: console             */
#define BSP_SMART_IO_I2C_PORT_FIRST       I2C_NUM_1         /* 0: camera, sensors     */

// clang-format on

/* Types ------------------------------------------------------------------- */


/* Exposed API ------------------------------------------------------------- */


#endif /* BSP_SMART_IO_H_ */

/**** END OF FILE ****/
