/**
 ******************************************************************************
 * @file    sensor_shell.h
 * @author  Daniel Dyringer
 * @date    2026-02-18
 * @brief   Sensor shell commands — sensor.
 ******************************************************************************
 */

/* Define to prevent recursive inclusion ----------------------------------- */
#ifndef SENSOR_SHELL_H_
#define SENSOR_SHELL_H_

/* Includes ---------------------------------------------------------------- */
#include "esp_err.h"

/* Constants and macros ---------------------------------------------------- */
// clang-format off


// clang-format on

/* Types ------------------------------------------------------------------- */


/* Exposed API ------------------------------------------------------------- */

esp_err_t sensor_shell_register(void);

#endif /* SENSOR_SHELL_H_ */

/**** END OF FILE ****/
