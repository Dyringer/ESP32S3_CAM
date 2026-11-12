/**
 ******************************************************************************
 * @file    ledc_types.h
 * @author  Daniel Dyringer
 * @date    2026-10-26
 * @brief   Host stand-in for the ESP-IDF hal/ledc_types.h.
 ******************************************************************************
 */

/* Define to prevent recursive inclusion ----------------------------------- */
#ifndef HAL_LEDC_TYPES_H_
#define HAL_LEDC_TYPES_H_

/* Includes ---------------------------------------------------------------- */


/* Constants and macros ---------------------------------------------------- */
// clang-format off


// clang-format on

/* Types ------------------------------------------------------------------- */

typedef enum
{
    LEDC_TIMER_0 = 0,
    LEDC_TIMER_1,
    LEDC_TIMER_2,
    LEDC_TIMER_3,
    LEDC_TIMER_MAX,
} ledc_timer_t;

typedef enum
{
    LEDC_CHANNEL_0 = 0,
    LEDC_CHANNEL_1,
    LEDC_CHANNEL_2,
    LEDC_CHANNEL_3,
    LEDC_CHANNEL_4,
    LEDC_CHANNEL_5,
    LEDC_CHANNEL_6,
    LEDC_CHANNEL_7,
    LEDC_CHANNEL_MAX,
} ledc_channel_t;


/* Exposed API ------------------------------------------------------------- */


#endif /* HAL_LEDC_TYPES_H_ */

/**** END OF FILE ****/
