/**
 ******************************************************************************
 * @file    camera_led.h
 * @author  Daniel Dyringer
 * @date    2026-10-07
 * @brief   Illumination LED — LEDC PWM output only. Owned by camera.c, which
 *          decides when and how bright it is lit.
 ******************************************************************************
 */

/* Define to prevent recursive inclusion ----------------------------------- */
#ifndef CAMERA_LED_H_
#define CAMERA_LED_H_

/* Includes ---------------------------------------------------------------- */
#include <stdint.h>

#include "esp_err.h"

/* Constants and macros ---------------------------------------------------- */
// clang-format off

#define CAMERA_LED_DUTY_MAX     255u

// clang-format on

/* Types ------------------------------------------------------------------- */


/* Exposed API ------------------------------------------------------------- */

esp_err_t camera_led_init(void);
esp_err_t camera_led_set_duty(uint32_t duty);

#endif /* CAMERA_LED_H_ */

/**** END OF FILE ****/
