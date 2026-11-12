/**
 ******************************************************************************
 * @file    sensor_shell.c
 * @author  Daniel Dyringer
 * @date    2026-02-18
 * @brief   Sensor shell commands — sensor.
 ******************************************************************************
 */


/* Includes ---------------------------------------------------------------- */
#include "sensor_shell.h"

#include "app_shell.h"
#include "drivers/ltr308_driver.h"
#include "sensor_store.h"
#include "sensor_types.h"
#include "utils.h"


/* Constants and macros ---------------------------------------------------- */
// clang-format off


// clang-format on

/* Private types ----------------------------------------------------------- */


/* Private variables ------------------------------------------------------- */


/* Private functions ------------------------------------------------------- */

static void sensor_shell_cmd(int argc, char** argv, app_shell_print_fn_t print)
{
    ARG_UNUSED(argc);
    ARG_UNUSED(argv);

    ltr308_sample_t als = {0};

    const esp_err_t error_code = sensor_store_get(SENSOR_ID_LTR308, &als, sizeof(als));

    if (error_code == ESP_OK)
    {
        print("LTR-308 : %.1f lux (raw %lu)\n", (double)als.lux, (unsigned long)als.raw);
    }
    else
    {
        print("LTR-308 : no data (%s)\n", esp_err_to_name(error_code));
    }
}


/* Exposed API ------------------------------------------------------------- */

esp_err_t sensor_shell_register(void)
{
    return app_shell_register_cmd(
        "sensor", "Print latest sensor readings", sensor_shell_cmd);
}

/**** END OF FILE ****/
