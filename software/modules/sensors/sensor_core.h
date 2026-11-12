/**
 ******************************************************************************
 * @file    sensor_core.h
 * @author  Daniel Dyringer
 * @date    2026-02-18
 * @brief   Sensor core — driver registry and polling task.
 *          The I2C bus is injected into driver configs by sensor_registry;
 *          sensor_core has no knowledge of it.
 ******************************************************************************
 */

/* Define to prevent recursive inclusion ----------------------------------- */
#ifndef SENSOR_CORE_H_
#define SENSOR_CORE_H_

/* Includes ---------------------------------------------------------------- */
#include "esp_err.h"
#include "esp_event.h"
#include "sensor_driver.h"
#include "sensor_types.h"

/* Constants and macros ---------------------------------------------------- */
// clang-format off


// clang-format on

/* Types ------------------------------------------------------------------- */

ESP_EVENT_DECLARE_BASE(SENSOR_CORE_EVENTS);

typedef enum sensor_core_event_e
{
    /* Posted to the default loop after each stored sample; event_data is the
     * sensor_id_t. */
    SENSOR_CORE_EVENT_UPDATED = 0,
} sensor_core_event_t;

/* Exposed API ------------------------------------------------------------- */

/* All registrations must happen before sensor_core_start. */
esp_err_t sensor_core_register(sensor_id_t id, const sensor_driver_t* driver);

/* Driver init runs inside the polling task, not in the caller. */
esp_err_t sensor_core_start(void);

/* Pause blocks until the current poll cycle finishes. Resume must be called
 * from the task that paused. */
esp_err_t sensor_core_pause(void);
esp_err_t sensor_core_resume(void);

#endif /* SENSOR_CORE_H_ */

/**** END OF FILE ****/
