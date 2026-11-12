/**
 ******************************************************************************
 * @file    sensor_types.h
 * @author  Daniel Dyringer
 * @date    2026-02-22
 * @brief   Sensor-wide shared types.
 ******************************************************************************
 */

/* Define to prevent recursive inclusion ----------------------------------- */
#ifndef SENSOR_TYPES_H_
#define SENSOR_TYPES_H_

/* Includes ---------------------------------------------------------------- */
#include <stdint.h>

/* Constants and macros ---------------------------------------------------- */
// clang-format off


// clang-format on

/* Types ------------------------------------------------------------------- */

typedef enum sensor_types_state_e
{
    SENSOR_TYPES_STATE_UNINITIALIZED = 0,
    SENSOR_TYPES_STATE_OK            = 1,
    SENSOR_TYPES_STATE_FAULT         = 2,
} sensor_types_state_t;

/* Index into sensor_core and sensor_store. To add a sensor, insert before
 * SENSOR_ID_COUNT. */
typedef enum sensor_id_e
{
    SENSOR_ID_LTR308 = 0,
    SENSOR_ID_COUNT,
} sensor_id_t;

/* Exposed API ------------------------------------------------------------- */


#endif /* SENSOR_TYPES_H_ */

/**** END OF FILE ****/
