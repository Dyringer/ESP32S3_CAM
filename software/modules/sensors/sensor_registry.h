/**
 ******************************************************************************
 * @file    sensor_registry.h
 * @author  Daniel Dyringer
 * @date    2026-02-22
 * @brief   Application-level sensor registration — the only place that knows
 *          concrete driver types and BSP assignments.
 ******************************************************************************
 */

/* Define to prevent recursive inclusion ----------------------------------- */
#ifndef SENSOR_REGISTRY_H_
#define SENSOR_REGISTRY_H_

/* Includes ---------------------------------------------------------------- */
#include "driver/i2c_master.h"
#include "esp_err.h"

/* Constants and macros ---------------------------------------------------- */
// clang-format off


// clang-format on

/* Types ------------------------------------------------------------------- */


/* Exposed API ------------------------------------------------------------- */

/* Call before sensor_core_start. */
esp_err_t sensor_registry_all(i2c_master_bus_handle_t bus);

#endif /* SENSOR_REGISTRY_H_ */

/**** END OF FILE ****/
