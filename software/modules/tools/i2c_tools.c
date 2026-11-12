/**
 ******************************************************************************
 * @file    i2c_tools.c
 * @author  Daniel Dyringer
 * @date    2026-02-18
 * @brief   I2C diagnostic shell commands — i2c scan / read / write.
 ******************************************************************************
 */


/* Includes ---------------------------------------------------------------- */
#include "i2c_tools.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "app_shell.h"
#include "bsp.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"


/* Constants and macros ---------------------------------------------------- */
// clang-format off

/* 0x00-0x07 and 0x78-0x7F are reserved by the I2C spec. */
#define I2C_TOOLS_ADDR_MIN      0x08u
#define I2C_TOOLS_ADDR_MAX      0x77u
#define I2C_TOOLS_BYTE_MAX      0xFFu
#define I2C_TOOLS_TIMEOUT_MS    50u
#define I2C_TOOLS_READ_MAX      32u

// clang-format on

/* Private types ----------------------------------------------------------- */


/* Private variables ------------------------------------------------------- */

/* Written once by i2c_tools_register before app_shell_init. */
static i2c_master_bus_handle_t i2c_tools_bus           = NULL;
static i2c_tools_pause_fn_t i2c_tools_pause_function   = NULL;
static i2c_tools_resume_fn_t i2c_tools_resume_function = NULL;


/* Private functions ------------------------------------------------------- */

static bool i2c_tools_parse(const char* text, int base, unsigned long max, unsigned long* result)
{
    char* end                 = NULL;
    const unsigned long value = strtoul(text, &end, base);
    bool valid                = false;

    if ((end != text) && (*end == '\0') && (value <= max))
    {
        *result = value;
        valid   = true;
    }

    return valid;
}

static esp_err_t i2c_tools_transfer(
    uint8_t address,
    const uint8_t* transmit_buffer,
    size_t transmit_length,
    uint8_t* receive_buffer,
    size_t receive_length)
{
    const i2c_device_config_t device_config = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address  = address,
        .scl_speed_hz    = BSP_I2C_FREQ_HZ,
    };

    const bool paused =
        (i2c_tools_pause_function != NULL) && (i2c_tools_pause_function() == ESP_OK);

    i2c_master_dev_handle_t device = NULL;

    esp_err_t error_code =
        i2c_master_bus_add_device(i2c_tools_bus, &device_config, &device);

    if (error_code == ESP_OK)
    {
        if (receive_length > 0u)
        {
            error_code = i2c_master_transmit_receive(
                device,
                transmit_buffer,
                transmit_length,
                receive_buffer,
                receive_length,
                (int)I2C_TOOLS_TIMEOUT_MS);
        }
        else
        {
            error_code = i2c_master_transmit(
                device, transmit_buffer, transmit_length, (int)I2C_TOOLS_TIMEOUT_MS);
        }

        i2c_master_bus_rm_device(device);
    }

    if (paused && (i2c_tools_resume_function != NULL))
    {
        i2c_tools_resume_function();
    }

    return error_code;
}

static void i2c_tools_cmd_scan(app_shell_print_fn_t print)
{
    uint32_t found = 0u;

    print("\n     0  1  2  3  4  5  6  7  8  9  a  b  c  d  e  f\n");

    for (uint32_t row = 0u; row <= 0x70u; row += 0x10u)
    {
        print("%02x: ", (unsigned)row);

        for (uint32_t column = 0u; column < 0x10u; column++)
        {
            const uint32_t address = row + column;

            if ((address < I2C_TOOLS_ADDR_MIN) || (address > I2C_TOOLS_ADDR_MAX))
            {
                print("   ");
                continue;
            }

            if (i2c_master_probe(i2c_tools_bus, (uint16_t)address, (int)I2C_TOOLS_TIMEOUT_MS)
                == ESP_OK)
            {
                print("%02x ", (unsigned)address);
                found++;
            }
            else
            {
                print("-- ");
            }

            /* Yield so a ~110-address scan doesn't starve lower-priority
             * Core 1 tasks. */
            vTaskDelay(pdMS_TO_TICKS(1u));
        }

        print("\n");
    }

    print("\n%u device(s) found\n\n", (unsigned)found);
}

static void i2c_tools_cmd_read(int argc, char** argv, app_shell_print_fn_t print)
{
    unsigned long address          = 0u;
    unsigned long register_address = 0u;
    unsigned long length           = 1u;

    do
    {
        if ((argc < 3) || !i2c_tools_parse(argv[1], 16, I2C_TOOLS_ADDR_MAX, &address)
            || !i2c_tools_parse(argv[2], 16, I2C_TOOLS_BYTE_MAX, &register_address)
            || ((argc >= 4) && !i2c_tools_parse(argv[3], 10, I2C_TOOLS_READ_MAX, &length))
            || (length == 0u))
        {
            print("Usage: i2c read <addr> <reg> [len 1-%u]\n", I2C_TOOLS_READ_MAX);
            break;
        }

        const uint8_t register_byte        = (uint8_t)register_address;
        uint8_t buffer[I2C_TOOLS_READ_MAX] = {0};

        const esp_err_t error_code =
            i2c_tools_transfer((uint8_t)address, &register_byte, 1u, buffer, length);

        if (error_code != ESP_OK)
        {
            print("Read failed: %s\n", esp_err_to_name(error_code));
            break;
        }

        print(
            "0x%02lX reg 0x%02lX [%lu byte%s]:\n  ",
            address,
            register_address,
            length,
            (length > 1u) ? "s" : "");

        for (size_t i = 0u; i < length; i++)
        {
            print("0x%02X ", buffer[i]);

            if ((((i + 1u) % 8u) == 0u) && ((i + 1u) < length))
            {
                print("\n  ");
            }
        }

        print("\n");
    } while (0);
}

static void i2c_tools_cmd_write(int argc, char** argv, app_shell_print_fn_t print)
{
    unsigned long address          = 0u;
    unsigned long register_address = 0u;
    unsigned long value            = 0u;

    do
    {
        if ((argc < 4) || !i2c_tools_parse(argv[1], 16, I2C_TOOLS_ADDR_MAX, &address)
            || !i2c_tools_parse(argv[2], 16, I2C_TOOLS_BYTE_MAX, &register_address)
            || !i2c_tools_parse(argv[3], 16, I2C_TOOLS_BYTE_MAX, &value))
        {
            print("Usage: i2c write <addr> <reg> <val>\n");
            break;
        }

        const uint8_t frame[2u] = {(uint8_t)register_address, (uint8_t)value};
        const esp_err_t error_code =
            i2c_tools_transfer((uint8_t)address, frame, sizeof(frame), NULL, 0u);

        if (error_code != ESP_OK)
        {
            print("Write failed: %s\n", esp_err_to_name(error_code));
            break;
        }

        print("0x%02lX reg 0x%02lX <- 0x%02lX OK\n", address, register_address, value);
    } while (0);
}

static void i2c_tools_cmd(int argc, char** argv, app_shell_print_fn_t print)
{
    if (i2c_tools_bus == NULL)
    {
        print("I2C bus not initialised\n");
    }
    else if ((argc >= 2) && (strcmp(argv[1], "scan") == 0))
    {
        i2c_tools_cmd_scan(print);
    }
    else if ((argc >= 2) && (strcmp(argv[1], "read") == 0))
    {
        i2c_tools_cmd_read(argc - 1, &argv[1], print);
    }
    else if ((argc >= 2) && (strcmp(argv[1], "write") == 0))
    {
        i2c_tools_cmd_write(argc - 1, &argv[1], print);
    }
    else
    {
        print("Usage:\n");
        print("  i2c scan\n");
        print("  i2c read  <addr> <reg> [len]\n");
        print("  i2c write <addr> <reg> <val>\n");
        print("  (addr/reg/val hex, 0x prefix optional; len decimal)\n");
    }
}


/* Exposed API ------------------------------------------------------------- */

esp_err_t i2c_tools_register(
    i2c_master_bus_handle_t bus,
    i2c_tools_pause_fn_t pause_function,
    i2c_tools_resume_fn_t resume_function)
{
    i2c_tools_bus             = bus;
    i2c_tools_pause_function  = pause_function;
    i2c_tools_resume_function = resume_function;

    return app_shell_register_cmd("i2c", "I2C tools: i2c <scan|read|write>", i2c_tools_cmd);
}

/**** END OF FILE ****/
