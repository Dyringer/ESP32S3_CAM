/**
 ******************************************************************************
 * @file    i2c_bus.c
 * @author  Daniel Dyringer
 * @date    2026-10-07
 * @brief   Shared I2C master bus — OV3660 SCCB and on-board sensors.
 ******************************************************************************
 */


/* Includes ---------------------------------------------------------------- */
#include "i2c_bus.h"

#include "bsp.h"
#include "esp_log.h"


/* Constants and macros ---------------------------------------------------- */
// clang-format off

#define I2C_BUS_GLITCH_IGNORE_CNT   7u

// clang-format on

/* Private types ----------------------------------------------------------- */


/* Private variables ------------------------------------------------------- */

static const char* i2c_bus_tag = "i2c_bus";

static i2c_master_bus_handle_t i2c_bus_master = NULL;


/* Private functions ------------------------------------------------------- */


/* Exposed API ------------------------------------------------------------- */

esp_err_t i2c_bus_init(void)
{
    const i2c_master_bus_config_t bus_config = {
        .i2c_port                     = BSP_I2C_PORT,
        .sda_io_num                   = BSP_I2C_SDA,
        .scl_io_num                   = BSP_I2C_SCL,
        .clk_source                   = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt            = I2C_BUS_GLITCH_IGNORE_CNT,
        .flags.enable_internal_pullup = true,
    };

    const esp_err_t error_code = i2c_new_master_bus(&bus_config, &i2c_bus_master);

    if (error_code == ESP_OK)
    {
        ESP_LOGI(
            i2c_bus_tag,
            "I2C%d ready (SDA=%d SCL=%d)",
            (int)BSP_I2C_PORT,
            (int)BSP_I2C_SDA,
            (int)BSP_I2C_SCL);
    }
    else
    {
        i2c_bus_master = NULL;
        ESP_LOGE(i2c_bus_tag, "i2c_new_master_bus failed: %s", esp_err_to_name(error_code));
    }

    return error_code;
}

i2c_master_bus_handle_t i2c_bus_handle(void)
{
    return i2c_bus_master;
}

/**** END OF FILE ****/
