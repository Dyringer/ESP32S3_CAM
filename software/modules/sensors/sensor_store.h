/**
 ******************************************************************************
 * @file    sensor_store.h
 * @author  Daniel Dyringer
 * @date    2026-02-22
 * @brief   Latest-sample store keyed by sensor_id_t. Values are opaque copies
 *          of each driver's sample struct.
 ******************************************************************************
 */

/* Define to prevent recursive inclusion ----------------------------------- */
#ifndef SENSOR_STORE_H_
#define SENSOR_STORE_H_

/* Includes ---------------------------------------------------------------- */
#include <stddef.h>

#include "esp_err.h"
#include "sensor_types.h"

/* Constants and macros ---------------------------------------------------- */
// clang-format off


// clang-format on

/* Types ------------------------------------------------------------------- */


/* Exposed API ------------------------------------------------------------- */

esp_err_t sensor_store_init(void);

/* A slot is sized by its first set; later sets and gets must use the same
 * size (ESP_ERR_INVALID_SIZE otherwise). */
esp_err_t sensor_store_set(sensor_id_t id, const void* data, size_t size);

/* ESP_ERR_NOT_FOUND until the sensor has produced its first sample, and
 * again after sensor_store_clear until the next set. */
esp_err_t sensor_store_get(sensor_id_t id, void* data, size_t size);

esp_err_t sensor_store_clear(sensor_id_t id);

#endif /* SENSOR_STORE_H_ */

/**** END OF FILE ****/
