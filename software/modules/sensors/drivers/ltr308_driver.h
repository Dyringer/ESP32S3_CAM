/**
 ******************************************************************************
 * @file    ltr308_driver.h
 * @author  Daniel Dyringer
 * @date    2026-10-07
 * @brief   LTR-308ALS-01 ambient light sensor driver.
 *          Register map from DFRobot_LTR308 (MIT,
 *          github.com/DFRobot/DFRobot_LTR308).
 ******************************************************************************
 */

/* Define to prevent recursive inclusion ----------------------------------- */
#ifndef LTR308_DRIVER_H_
#define LTR308_DRIVER_H_

/* Includes ---------------------------------------------------------------- */
#include <stdint.h>

#include "driver/i2c_master.h"
#include "sensor_driver.h"

/* Constants and macros ---------------------------------------------------- */
// clang-format off

#define LTR308_DRIVER_CREATE(_driver_name, _period_ms, _i2c_bus, _i2c_address)  \
    SENSOR_DRIVER_CREATE(_driver_name, ltr308, ltr308_driver_api, _period_ms);  \
    ltr308_config_##_driver_name.i2c_bus     = (_i2c_bus);                      \
    ltr308_config_##_driver_name.i2c_address = (_i2c_address);                  \
    ltr308_data_##_driver_name.device        = NULL

// clang-format on

/* Types ------------------------------------------------------------------- */

typedef struct ltr308_sample_s
{
    uint32_t raw;
    float lux;
} ltr308_sample_t;

typedef struct ltr308_config_s
{
    i2c_master_bus_handle_t i2c_bus;
    uint8_t i2c_address;
} ltr308_config_t;

typedef struct ltr308_data_s
{
    i2c_master_dev_handle_t device;
} ltr308_data_t;

/* Exposed API ------------------------------------------------------------- */

extern const sensor_driver_api_t ltr308_driver_api;

#endif /* LTR308_DRIVER_H_ */

/**** END OF FILE ****/
