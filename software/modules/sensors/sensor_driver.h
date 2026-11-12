/**
 ******************************************************************************
 * @file    sensor_driver.h
 * @author  Daniel Dyringer
 * @date    2026-02-18
 * @brief   Sensor driver vtable — implement this interface for each sensor.
 ******************************************************************************
 */

/* Define to prevent recursive inclusion ----------------------------------- */
#ifndef SENSOR_DRIVER_H_
#define SENSOR_DRIVER_H_

/* Includes ---------------------------------------------------------------- */
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

/* Constants and macros ---------------------------------------------------- */
// clang-format off

/* Declares the static config/data/driver objects for one instance. The
 * driver's CREATE macro assigns config fields in separate statements after
 * this — brace initialisers passed as macro arguments get split on commas. */
#define SENSOR_DRIVER_CREATE(_driver_name, _type, _api, _period_ms)               \
    static _type##_config_t _type##_config_##_driver_name;                        \
    static _type##_data_t   _type##_data_##_driver_name;                          \
    static sensor_driver_t  _type##_driver_##_driver_name = {                     \
        .name             = #_driver_name,                                        \
        .poll_interval_ms = (_period_ms),                                         \
        .sample_size      = sizeof(_type##_sample_t),                             \
        .api              = &(_api),                                              \
        .config           = &_type##_config_##_driver_name,                       \
        .data             = &_type##_data_##_driver_name,                         \
    }

// clang-format on

/* Types ------------------------------------------------------------------- */

typedef struct sensor_driver_s sensor_driver_t;

typedef esp_err_t (*sensor_driver_init_fn_t)(const sensor_driver_t* driver);
typedef esp_err_t (*sensor_driver_deinit_fn_t)(const sensor_driver_t* driver);
typedef esp_err_t (*sensor_driver_read_fn_t)(const sensor_driver_t* driver, void* sample);

/* Called only from the sensor_core task. init is re-run after a fault, so it
 * must be safe to call again without deinit. */
typedef struct sensor_driver_api_s
{
    sensor_driver_init_fn_t init;
    sensor_driver_deinit_fn_t deinit;
    sensor_driver_read_fn_t read;
} sensor_driver_api_t;

struct sensor_driver_s
{
    const char* name;
    uint32_t poll_interval_ms;
    size_t sample_size;
    const sensor_driver_api_t* api;

    const void* config;
    void* data;
};

/* Exposed API ------------------------------------------------------------- */


#endif /* SENSOR_DRIVER_H_ */

/**** END OF FILE ****/
