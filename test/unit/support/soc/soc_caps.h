/**
 ******************************************************************************
 * @file    soc_caps.h
 * @author  Daniel Dyringer
 * @date    2026-10-27
 * @brief   Host stand-in for the ESP32-S3 soc/soc_caps.h used by unit tests.
 ******************************************************************************
 */

/* Define to prevent recursive inclusion ----------------------------------- */
#ifndef SOC_CAPS_H_
#define SOC_CAPS_H_

/* Includes ---------------------------------------------------------------- */


/* Constants and macros ---------------------------------------------------- */
// clang-format off

/* Same values as the ESP32-S3 header. */
#define SOC_LEDC_TIMER_NUM      (4)
#define SOC_LEDC_CHANNEL_NUM    (8)
#define SOC_UART_HP_NUM         (3)

// clang-format on

/* Types ------------------------------------------------------------------- */


/* Exposed API ------------------------------------------------------------- */


#endif /* SOC_CAPS_H_ */

/**** END OF FILE ****/
