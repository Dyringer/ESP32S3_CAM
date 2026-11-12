/**
 ******************************************************************************
 * @file    sensor_registry.c
 * @author  Daniel Dyringer
 * @date    2026-02-22
 * @brief   Application-level sensor registration — the only place that knows
 *          concrete driver types and BSP assignments.
 ******************************************************************************
 */


/* Includes ---------------------------------------------------------------- */
#include "sensor_registry.h"

#include "bsp.h"
#include "drivers/ltr308_driver.h"
#include "sensor_core.h"
#include "sensor_types.h"


/* Constants and macros ---------------------------------------------------- */
// clang-format off

/* Matches the sensor's 500 ms measurement rate. */
#define SENSOR_REGISTRY_LTR308_POLL_MS  500u

// clang-format on

/* Private types ----------------------------------------------------------- */


/* Private variables ------------------------------------------------------- */


/* Private functions ------------------------------------------------------- */


/* Exposed API ------------------------------------------------------------- */

esp_err_t sensor_registry_all(i2c_master_bus_handle_t bus)
{
    esp_err_t error_code = ESP_OK;

    LTR308_DRIVER_CREATE(als, SENSOR_REGISTRY_LTR308_POLL_MS, bus, BSP_LTR308_I2C_ADDR);

    do
    {
        if (bus == NULL)
        {
            error_code = ESP_ERR_INVALID_STATE;
            break;
        }

        error_code = sensor_core_register(SENSOR_ID_LTR308, &ltr308_driver_als);
    } while (0);

    return error_code;
}

/**** END OF FILE ****/
