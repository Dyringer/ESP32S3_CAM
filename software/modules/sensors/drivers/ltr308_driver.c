/**
 ******************************************************************************
 * @file    ltr308_driver.c
 * @author  Daniel Dyringer
 * @date    2026-10-07
 * @brief   LTR-308ALS-01 ambient light sensor driver.
 *          Register map from DFRobot_LTR308 (MIT,
 *          github.com/DFRobot/DFRobot_LTR308).
 ******************************************************************************
 */


/* Includes ---------------------------------------------------------------- */
#include "ltr308_driver.h"

#include "bsp.h"
#include "esp_log.h"


/* Constants and macros ---------------------------------------------------- */
// clang-format off

#define LTR308_DRIVER_REG_MAIN_CTRL         0x00u
#define LTR308_DRIVER_REG_MEAS_RATE         0x04u
#define LTR308_DRIVER_REG_GAIN              0x05u
#define LTR308_DRIVER_REG_PART_ID           0x06u
#define LTR308_DRIVER_REG_DATA_0            0x0Du

#define LTR308_DRIVER_PART_ID               0xB1u
#define LTR308_DRIVER_MAIN_CTRL_ALS_EN      0x02u

/* 3x gain, 18 bit / 100 ms conversion, 500 ms rate: ~0.2 to ~52k lux. */
#define LTR308_DRIVER_GAIN_3X               0x01u
#define LTR308_DRIVER_MEAS_18BIT_500MS      0x23u

/* Datasheet: lux = 0.6 * counts / (gain * integration time / 100 ms) */
#define LTR308_DRIVER_LUX_PER_COUNT         (0.6f / 3.0f)

/* Signed: i2c_master_transmit*() take an int timeout (-1 = wait forever). */
#define LTR308_DRIVER_I2C_TIMEOUT_MS        50

// clang-format on

/* Private types ----------------------------------------------------------- */


/* Private variables ------------------------------------------------------- */

static const char* ltr308_driver_tag = "ltr308";


/* Private functions ------------------------------------------------------- */

static esp_err_t ltr308_driver_write_reg(
    i2c_master_dev_handle_t device,
    uint8_t register_address,
    uint8_t value)
{
    const uint8_t frame[2] = {register_address, value};

    return i2c_master_transmit(device, frame, sizeof(frame), LTR308_DRIVER_I2C_TIMEOUT_MS);
}

static esp_err_t ltr308_driver_read_regs(
    i2c_master_dev_handle_t device,
    uint8_t register_address,
    uint8_t* buffer,
    size_t length)
{
    return i2c_master_transmit_receive(
        device, &register_address, 1u, buffer, length, LTR308_DRIVER_I2C_TIMEOUT_MS);
}

static esp_err_t ltr308_driver_init(const sensor_driver_t* driver)
{
    const ltr308_config_t* config = driver->config;
    ltr308_data_t* data           = driver->data;
    esp_err_t error_code          = ESP_OK;

    do
    {
        if (config->i2c_bus == NULL)
        {
            error_code = ESP_ERR_INVALID_STATE;
            break;
        }

        /* Kept across sensor_core fault retries so each retry doesn't add
         * another device to the bus. */
        if (data->device == NULL)
        {
            const i2c_device_config_t device_config = {
                .dev_addr_length = I2C_ADDR_BIT_LEN_7,
                .device_address  = config->i2c_address,
                .scl_speed_hz    = BSP_I2C_FREQ_HZ,
            };

            error_code =
                i2c_master_bus_add_device(config->i2c_bus, &device_config, &data->device);

            if (error_code != ESP_OK)
            {
                data->device = NULL;
                break;
            }
        }

        uint8_t part_id = 0u;

        error_code =
            ltr308_driver_read_regs(data->device, LTR308_DRIVER_REG_PART_ID, &part_id, 1u);

        if (error_code != ESP_OK)
        {
            break;
        }

        if (part_id != LTR308_DRIVER_PART_ID)
        {
            ESP_LOGD(
                ltr308_driver_tag,
                "Part ID 0x%02X, expected 0x%02X",
                part_id,
                LTR308_DRIVER_PART_ID);
            error_code = ESP_ERR_INVALID_RESPONSE;
            break;
        }

        error_code = ltr308_driver_write_reg(
            data->device, LTR308_DRIVER_REG_GAIN, LTR308_DRIVER_GAIN_3X);

        if (error_code != ESP_OK)
        {
            break;
        }

        error_code = ltr308_driver_write_reg(
            data->device, LTR308_DRIVER_REG_MEAS_RATE, LTR308_DRIVER_MEAS_18BIT_500MS);

        if (error_code != ESP_OK)
        {
            break;
        }

        error_code = ltr308_driver_write_reg(
            data->device, LTR308_DRIVER_REG_MAIN_CTRL, LTR308_DRIVER_MAIN_CTRL_ALS_EN);
    } while (0);

    return error_code;
}

static esp_err_t ltr308_driver_read(const sensor_driver_t* driver, void* sample)
{
    const ltr308_data_t* data = driver->data;
    ltr308_sample_t* out      = sample;
    uint8_t buffer[3]         = {0u};

    const esp_err_t error_code = ltr308_driver_read_regs(
        data->device, LTR308_DRIVER_REG_DATA_0, buffer, sizeof(buffer));

    if (error_code == ESP_OK)
    {
        out->raw = (((uint32_t)buffer[2] & 0x0Fu) << 16u) | ((uint32_t)buffer[1] << 8u)
                   | (uint32_t)buffer[0];
        out->lux = (float)out->raw * LTR308_DRIVER_LUX_PER_COUNT;
    }

    return error_code;
}

static esp_err_t ltr308_driver_deinit(const sensor_driver_t* driver)
{
    ltr308_data_t* data  = driver->data;
    esp_err_t error_code = ESP_OK;

    if (data->device != NULL)
    {
        ltr308_driver_write_reg(data->device, LTR308_DRIVER_REG_MAIN_CTRL, 0u);
        error_code   = i2c_master_bus_rm_device(data->device);
        data->device = NULL;
    }

    return error_code;
}


/* Exposed API ------------------------------------------------------------- */

const sensor_driver_api_t ltr308_driver_api = {
    .init   = ltr308_driver_init,
    .deinit = ltr308_driver_deinit,
    .read   = ltr308_driver_read,
};

/**** END OF FILE ****/
