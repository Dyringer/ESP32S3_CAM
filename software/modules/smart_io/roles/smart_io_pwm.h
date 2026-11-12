/**
 ******************************************************************************
 * @file    smart_io_pwm.h
 * @author  Daniel Dyringer
 * @date    2026-10-31
 * @brief   pwm role: LEDC timer and channel allocation, timer sharing between
 *          pins with the same frequency and resolution.
 ******************************************************************************
 */

/* Define to prevent recursive inclusion ----------------------------------- */
#ifndef SMART_IO_PWM_H_
#define SMART_IO_PWM_H_

/* Includes ---------------------------------------------------------------- */
#include "smart_io_role.h"


/* Constants and macros ---------------------------------------------------- */
// clang-format off


// clang-format on

/* Types ------------------------------------------------------------------- */


/* Exposed API ------------------------------------------------------------- */

extern const smart_io_role_api_t smart_io_pwm_api;

#endif /* SMART_IO_PWM_H_ */

/**** END OF FILE ****/
