/**
 ******************************************************************************
 * @file    i2c_bus.h
 * @author  Daniel Dyringer
 * @date    2026-10-07
 * @brief   Shared I2C master bus — OV3660 SCCB and on-board sensors.
 ******************************************************************************
 */

/* Define to prevent recursive inclusion ----------------------------------- */
#ifndef I2C_BUS_H_
#define I2C_BUS_H_

/* Includes ---------------------------------------------------------------- */
#include "driver/i2c_master.h"
#include "esp_err.h"

/* Constants and macros ---------------------------------------------------- */
// clang-format off


// clang-format on

/* Types ------------------------------------------------------------------- */


/* Exposed API ------------------------------------------------------------- */

/* Must run before camera_init — the camera driver attaches to this bus by
 * port number instead of creating its own. */
esp_err_t i2c_bus_init(void);

/* NULL until i2c_bus_init succeeds. */
i2c_master_bus_handle_t i2c_bus_handle(void);

#endif /* I2C_BUS_H_ */

/**** END OF FILE ****/
