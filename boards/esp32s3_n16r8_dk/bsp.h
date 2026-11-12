/**
 ******************************************************************************
 * @file    bsp.h
 * @author  Daniel Dyringer
 * @date    2026-02-17
 * @brief   Board Support Package — GPIO and peripheral pin assignments
 *          for the esp32s3_n16r8_dk board.
 ******************************************************************************
 */

/* Define to prevent recursive inclusion ----------------------------------- */
#ifndef BSP_H_
#define BSP_H_

/* Includes ---------------------------------------------------------------- */
#include "hal/i2c_types.h"
#include "hal/ledc_types.h"
#include "soc/gpio_num.h"


/* Constants and macros ---------------------------------------------------- */
// clang-format off

#define BOARD_NAME              "esp32s3_n16r8_dk"

/* ---- Avoided GPIOs (do not use) ---- */
/*  GPIO  0        : strapping pin (boot mode)
 *  GPIO  3        : strapping pin
 *  GPIO 22-25     : do not exist on ESP32-S3
 *  GPIO 26-37     : internal octal flash / PSRAM on ESP32-S3-WROOM-1
 *                   (N16R8 uses octal PSRAM — GPIO 26-37 are unavailable)
 *  GPIO 43        : UART0 TX  (serial console)
 *  GPIO 44        : UART0 RX  (serial console)
 *  GPIO 45        : strapping pin (VDD_SPI voltage)
 *  GPIO 46        : strapping pin, input-only
 */

/* ---- I2C bus — shared: OV3660 SCCB + LTR-308 ---- */
/*    Created by i2c_bus before camera_init; the camera driver borrows it.   */
#define BSP_I2C_PORT            I2C_NUM_0
#define BSP_I2C_SDA             GPIO_NUM_8
#define BSP_I2C_SCL             GPIO_NUM_9
#define BSP_I2C_FREQ_HZ         100000u         /* = CONFIG_SCCB_CLK_FREQ    */

/* ---- LTR-308ALS-01 ambient light sensor ---- */
#define BSP_LTR308_I2C_ADDR     0x53u
#define BSP_LTR308_INT          GPIO_NUM_48     /* SPICLK_N, active low, OD  */

/* ---- OV3660 camera — 8-bit DVP parallel bus ---- */
/*    PWDN and RESET are not wired (-1). SCCB runs on the BSP_I2C bus.       */
#define BSP_CAMERA_PWDN         (-1)
#define BSP_CAMERA_RESET        (-1)
#define BSP_CAMERA_XCLK         GPIO_NUM_5
#define BSP_CAMERA_D0           GPIO_NUM_16     /* Y2                        */
#define BSP_CAMERA_D1           GPIO_NUM_18     /* Y3                        */
#define BSP_CAMERA_D2           GPIO_NUM_21     /* Y4                        */
#define BSP_CAMERA_D3           GPIO_NUM_17     /* Y5                        */
#define BSP_CAMERA_D4           GPIO_NUM_14     /* Y6                        */
#define BSP_CAMERA_D5           GPIO_NUM_7      /* Y7                        */
#define BSP_CAMERA_D6           GPIO_NUM_6      /* Y8                        */
#define BSP_CAMERA_D7           GPIO_NUM_4      /* Y9                        */
#define BSP_CAMERA_VSYNC        GPIO_NUM_1
#define BSP_CAMERA_HREF         GPIO_NUM_2
#define BSP_CAMERA_PCLK         GPIO_NUM_15
#define BSP_CAMERA_XCLK_FREQ_HZ (20 * 1000 * 1000)
#define BSP_CAMERA_LEDC_TIMER   LEDC_TIMER_0
#define BSP_CAMERA_LEDC_CHANNEL LEDC_CHANNEL_0

/* ---- Illumination LED — PWM brightness ---- */
/*    Must not share the camera's LEDC timer/channel (XCLK runs at 20 MHz). */
#define BSP_LED_GPIO            GPIO_NUM_47
#define BSP_LED_LEDC_TIMER      LEDC_TIMER_1
#define BSP_LED_LEDC_CHANNEL    LEDC_CHANNEL_1
#define BSP_LED_PWM_FREQ_HZ     5000u

// clang-format on

/* Types ------------------------------------------------------------------- */


/* Exposed API ------------------------------------------------------------- */


#endif /* BSP_H_ */

/**** END OF FILE ****/
