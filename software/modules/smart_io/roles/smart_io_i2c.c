/**
 ******************************************************************************
 * @file    smart_io_i2c.c
 * @author  Daniel Dyringer
 * @date    2026-10-31
 * @brief   i2c role.
 ******************************************************************************
 */


/* Includes ---------------------------------------------------------------- */
#include "smart_io_i2c.h"

#include <stdlib.h>

#include "bsp_smart_io.h"
#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "esp_rom_sys.h"
#include "smart_io.h"
#include "utils.h"


/* Constants and macros ---------------------------------------------------- */
// clang-format off

#define SMART_IO_I2C_FREQUENCY_MIN_HZ   10000u
#define SMART_IO_I2C_FREQUENCY_MAX_HZ   1000000u
#define SMART_IO_I2C_FREQUENCY_HZ       100000u
#define SMART_IO_I2C_TIMEOUT_MIN_MS     1u
#define SMART_IO_I2C_TIMEOUT_MAX_MS     1000u
#define SMART_IO_I2C_TIMEOUT_MS         50u
#define SMART_IO_I2C_ADDRESS_MAX        0x7Fu
#define SMART_IO_I2C_DISCOVER_FIRST     0x08u   /* 0x00-0x07, 0x78-0x7F reserved */
#define SMART_IO_I2C_DISCOVER_LAST      0x77u
#define SMART_IO_I2C_GLITCH_CYCLES      7u
/* Internal pull-up (~45 kOhm) and pad capacitance settle well within this. */
#define SMART_IO_I2C_SETTLE_US          50u
#define SMART_IO_I2C_PORT_COUNT         ((uint32_t)I2C_NUM_MAX                     \
                                         - (uint32_t)BSP_SMART_IO_I2C_PORT_FIRST)

// clang-format on

/* Private types ----------------------------------------------------------- */

/* Index in config.param_values. */
typedef enum smart_io_i2c_param_e
{
    SMART_IO_I2C_PARAM_FREQUENCY_HZ = 0,
    SMART_IO_I2C_PARAM_PULLUPS      = 1,
    SMART_IO_I2C_PARAM_TIMEOUT_MS   = 2,
} smart_io_i2c_param_t;

/* Copied from config.param_values at configure. */
typedef struct smart_io_i2c_params_s
{
    uint32_t frequency_hz;
    uint32_t pullups;
    uint32_t timeout_ms;
} smart_io_i2c_params_t;

/* instance->state.role_state. Used without the mutex while the bus is busy. */
typedef struct smart_io_i2c_state_s
{
    smart_io_i2c_params_t params;
    i2c_master_bus_handle_t bus;
    i2c_master_dev_handle_t device; /* NULL until the first addressed transfer */
    uint32_t port;                  /* index from BSP_SMART_IO_I2C_PORT_FIRST */
    uint16_t device_address;
} smart_io_i2c_state_t;


/* Private variables ------------------------------------------------------- */

static const smart_io_param_descriptor_t smart_io_i2c_params[] = {
    [SMART_IO_I2C_PARAM_FREQUENCY_HZ] =
        {
            .name          = "frequency_hz",
            .unit          = "Hz",
            .option_names  = NULL,
            .min           = SMART_IO_I2C_FREQUENCY_MIN_HZ,
            .max           = SMART_IO_I2C_FREQUENCY_MAX_HZ,
            .default_value = SMART_IO_I2C_FREQUENCY_HZ,
            .type          = SMART_IO_PARAM_TYPE_U32,
            .shared        = false,
        },
    [SMART_IO_I2C_PARAM_PULLUPS] =
        {
            .name          = "pullups",
            .unit          = NULL,
            .option_names  = NULL,
            .min           = 0u,
            .max           = 1u,
            .default_value = 1u,
            .type          = SMART_IO_PARAM_TYPE_BOOL,
            .shared        = false,
        },
    [SMART_IO_I2C_PARAM_TIMEOUT_MS] =
        {
            .name          = "timeout_ms",
            .unit          = "ms",
            .option_names  = NULL,
            .min           = SMART_IO_I2C_TIMEOUT_MIN_MS,
            .max           = SMART_IO_I2C_TIMEOUT_MAX_MS,
            .default_value = SMART_IO_I2C_TIMEOUT_MS,
            .type          = SMART_IO_PARAM_TYPE_U32,
            .shared        = false,
        },
};

/* Guarded by the smart_io mutex. */
static uint32_t smart_io_i2c_ports_used = 0u;


/* Private functions ------------------------------------------------------- */

static bool smart_io_i2c_port_take(uint32_t* port)
{
    bool found = false;

    for (uint32_t i = 0u; (i < SMART_IO_I2C_PORT_COUNT) && !found; i++)
    {
        if ((smart_io_i2c_ports_used & ((uint32_t)1u << i)) == 0u)
        {
            smart_io_i2c_ports_used |= (uint32_t)1u << i;
            *port = i;
            found = true;
        }
    }

    return found;
}

static void smart_io_i2c_port_give(uint32_t port)
{
    smart_io_i2c_ports_used &= ~((uint32_t)1u << port);
}

/* An idle bus reads high on both lines; low means a missing pull-up or a
 * device holding the line. */
static esp_err_t smart_io_i2c_lines_check(
    const smart_io_config_t* config,
    bool pullups,
    smart_io_reason_t* reason)
{
    esp_err_t error_code          = ESP_OK;
    const gpio_config_t io_config = {
        .pin_bit_mask = ((uint64_t)1u << (uint32_t)config->gpio[0])
                        | ((uint64_t)1u << (uint32_t)config->gpio[1]),
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = pullups ? GPIO_PULLUP_ENABLE : GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };

    error_code = gpio_config(&io_config);

    if (error_code != ESP_OK)
    {
        smart_io_reason_set(
            reason, "I2C pin setup failed (%s)", esp_err_to_name(error_code));
    }
    else
    {
        esp_rom_delay_us(SMART_IO_I2C_SETTLE_US);

        if (gpio_get_level(config->gpio[0]) == 0)
        {
            error_code = ESP_ERR_INVALID_STATE;
            smart_io_reason_set(
                reason, "SDA (GPIO %d) held low / missing pull-up", (int)config->gpio[0]);
        }
        else if (gpio_get_level(config->gpio[1]) == 0)
        {
            error_code = ESP_ERR_INVALID_STATE;
            smart_io_reason_set(
                reason, "SCL (GPIO %d) held low / missing pull-up", (int)config->gpio[1]);
        }
        else
        {
            /* Both high: idle bus. */
        }
    }

    return error_code;
}

static void smart_io_i2c_release(smart_io_instance_t* instance)
{
    smart_io_i2c_state_t* const state = instance->state.role_state;

    if (state != NULL)
    {
        if (state->device != NULL)
        {
            i2c_master_bus_rm_device(state->device);
        }

        i2c_del_master_bus(state->bus);
        smart_io_i2c_port_give(state->port);
        free(state);
        instance->state.role_state = NULL;
    }
}

static esp_err_t smart_io_i2c_configure(smart_io_instance_t* instance, smart_io_reason_t* reason)
{
    esp_err_t error_code              = ESP_OK;
    smart_io_i2c_state_t* const state = calloc(1u, sizeof(*state));
    bool port_taken                   = false;

    do
    {
        if (state == NULL)
        {
            error_code = ESP_ERR_NO_MEM;
            smart_io_reason_set(reason, "Out of memory");
            break;
        }

        state->params.frequency_hz =
            instance->config.param_values[SMART_IO_I2C_PARAM_FREQUENCY_HZ];
        state->params.pullups = instance->config.param_values[SMART_IO_I2C_PARAM_PULLUPS];
        state->params.timeout_ms =
            instance->config.param_values[SMART_IO_I2C_PARAM_TIMEOUT_MS];

        port_taken = smart_io_i2c_port_take(&state->port);

        if (!port_taken)
        {
            error_code = ESP_ERR_NOT_FOUND;
            smart_io_reason_set(
                reason,
                "No free I2C port (%lu in use)",
                (unsigned long)SMART_IO_I2C_PORT_COUNT);
            break;
        }

        error_code = smart_io_i2c_lines_check(
            &instance->config, state->params.pullups != 0u, reason);

        if (error_code != ESP_OK)
        {
            break;
        }

        const i2c_master_bus_config_t bus_config = {
            .i2c_port =
                (i2c_port_num_t)((uint32_t)BSP_SMART_IO_I2C_PORT_FIRST + state->port),
            .sda_io_num        = instance->config.gpio[0],
            .scl_io_num        = instance->config.gpio[1],
            .clk_source        = I2C_CLK_SRC_DEFAULT,
            .glitch_ignore_cnt = SMART_IO_I2C_GLITCH_CYCLES,
            .intr_priority     = 0,
            .trans_queue_depth = 0u,
            .flags =
                {
                    .enable_internal_pullup = (state->params.pullups != 0u) ? 1u : 0u,
                    .allow_pd               = 0u,
                },
        };

        error_code = i2c_new_master_bus(&bus_config, &state->bus);

        if (error_code != ESP_OK)
        {
            smart_io_reason_set(
                reason, "I2C bus setup failed (%s)", esp_err_to_name(error_code));
        }
    } while (0);

    if (error_code == ESP_OK)
    {
        instance->state.role_state = state;
    }
    else
    {
        if (port_taken)
        {
            smart_io_i2c_port_give(state->port);
        }

        free(state);
    }

    return error_code;
}

/* One device handle per bus, moved to each new address. */
static esp_err_t smart_io_i2c_device_select(smart_io_i2c_state_t* state, uint16_t address)
{
    esp_err_t error_code = ESP_OK;

    if (state->device == NULL)
    {
        const i2c_device_config_t device_config = {
            .dev_addr_length = I2C_ADDR_BIT_LEN_7,
            .device_address  = address,
            .scl_speed_hz    = state->params.frequency_hz,
            .scl_wait_us     = 0u,
            .flags           = {.disable_ack_check = 0u},
        };

        error_code = i2c_master_bus_add_device(state->bus, &device_config, &state->device);
    }
    else if (state->device_address != address)
    {
        error_code = i2c_master_device_change_address(
            state->device, address, (int)state->params.timeout_ms);
    }
    else
    {
        /* Already addressed. */
    }

    if (error_code == ESP_OK)
    {
        state->device_address = address;
    }

    return error_code;
}

/* NOTE: IDF 6.1 returns ESP_ERR_INVALID_RESPONSE for both a NACK and a timeout
 * on device transfers (esp_driver_i2c/i2c_master.c, s_i2c_transaction_start).
 * A line still held low tells the timeout apart. */
static esp_err_t smart_io_i2c_result(
    smart_io_instance_t* instance,
    esp_err_t error,
    uint16_t address,
    smart_io_reason_t* reason)
{
    const smart_io_i2c_state_t* const state = instance->state.role_state;
    esp_err_t error_code                    = error;
    const bool stuck = (gpio_get_level(instance->config.gpio[0]) == 0)
                       || (gpio_get_level(instance->config.gpio[1]) == 0);

    if (error == ESP_OK)
    {
        /* Success. */
    }
    else if (
        (error == ESP_ERR_TIMEOUT) || (error == ESP_ERR_INVALID_STATE)
        || ((error == ESP_ERR_INVALID_RESPONSE) && stuck))
    {
        i2c_master_bus_reset(state->bus);
        error_code = ESP_ERR_TIMEOUT;
        smart_io_reason_set(reason, "Timeout at 0x%02X, bus reset", (unsigned)address);
    }
    else if ((error == ESP_ERR_INVALID_RESPONSE) || (error == ESP_ERR_NOT_FOUND))
    {
        error_code = ESP_ERR_NOT_FOUND;
        smart_io_reason_set(reason, "No ACK from 0x%02X", (unsigned)address);
    }
    else
    {
        smart_io_reason_set(
            reason,
            "Transfer to 0x%02X failed (%s)",
            (unsigned)address,
            esp_err_to_name(error));
    }

    return error_code;
}

static esp_err_t smart_io_i2c_transfer_one(
    smart_io_i2c_state_t* state,
    const smart_io_transfer_t* transfer)
{
    esp_err_t error_code = ESP_OK;
    const int timeout_ms = (int)state->params.timeout_ms;
    const bool has_tx    = transfer->tx_length > 0u;
    const bool has_rx    = transfer->rx_length > 0u;

    if (!has_tx && !has_rx)
    {
        error_code = i2c_master_probe(state->bus, transfer->address, timeout_ms);
    }
    else
    {
        error_code = smart_io_i2c_device_select(state, transfer->address);
    }

    if ((error_code == ESP_OK) && has_tx && has_rx)
    {
        error_code = i2c_master_transmit_receive(
            state->device,
            transfer->tx,
            transfer->tx_length,
            transfer->rx,
            transfer->rx_length,
            timeout_ms);
    }
    else if ((error_code == ESP_OK) && has_tx)
    {
        error_code = i2c_master_transmit(
            state->device, transfer->tx, transfer->tx_length, timeout_ms);
    }
    else if ((error_code == ESP_OK) && has_rx)
    {
        error_code = i2c_master_receive(
            state->device, transfer->rx, transfer->rx_length, timeout_ms);
    }
    else
    {
        /* Probe done or failed. */
    }

    return error_code;
}

static esp_err_t smart_io_i2c_transfer(
    smart_io_instance_t* instance,
    const smart_io_transfer_t* transfers,
    size_t count,
    size_t* failed,
    smart_io_reason_t* reason)
{
    esp_err_t error_code = ESP_OK;

    for (size_t i = 0u; (i < count) && (error_code == ESP_OK); i++)
    {
        if (transfers[i].address > SMART_IO_I2C_ADDRESS_MAX)
        {
            error_code = ESP_ERR_INVALID_ARG;
            *failed    = i;
            smart_io_reason_set(reason, "Address must be 0x00-0x7F (7 bit)");
        }
    }

    for (size_t i = 0u; (i < count) && (error_code == ESP_OK); i++)
    {
        error_code = smart_io_i2c_result(
            instance,
            smart_io_i2c_transfer_one(instance->state.role_state, &transfers[i]),
            transfers[i].address,
            reason);

        if (error_code != ESP_OK)
        {
            *failed = i;
        }
    }

    return error_code;
}

static esp_err_t smart_io_i2c_discover(
    smart_io_instance_t* instance,
    uint16_t* found,
    size_t capacity,
    size_t* count,
    smart_io_reason_t* reason)
{
    const smart_io_i2c_state_t* const state = instance->state.role_state;
    esp_err_t error_code                    = ESP_OK;

    *count = 0u;

    for (uint16_t address = SMART_IO_I2C_DISCOVER_FIRST;
         (address <= SMART_IO_I2C_DISCOVER_LAST) && (error_code == ESP_OK);
         address++)
    {
        const esp_err_t probe_error =
            i2c_master_probe(state->bus, address, (int)state->params.timeout_ms);

        if ((probe_error == ESP_OK) && (*count < capacity))
        {
            found[*count] = address;
            (*count)++;
        }
        else if (probe_error != ESP_ERR_NOT_FOUND)
        {
            error_code = smart_io_i2c_result(instance, probe_error, address, reason);
        }
        else
        {
            /* Nobody at this address. */
        }
    }

    return error_code;
}


/* Exposed API ------------------------------------------------------------- */

const smart_io_role_api_t smart_io_i2c_api = {
    .description =
        {
            .name              = "i2c",
            .param_descriptors = smart_io_i2c_params,
            .param_count       = ARRAY_SIZE(smart_io_i2c_params),
            .value_unit        = NULL,
            .value_min         = 0,
            .value_max         = 0,
            .role              = SMART_IO_ROLE_I2C,
            .stream_kind       = SMART_IO_STREAM_KIND_NONE,
            .pin_functions     = {SMART_IO_FUNCTION_I2C_SDA, SMART_IO_FUNCTION_I2C_SCL},
        },
    .validate     = NULL,
    .configure    = smart_io_i2c_configure,
    .release      = smart_io_i2c_release,
    .read         = NULL,
    .write        = NULL,
    .stream_start = NULL,
    .stream_stop  = NULL,
    .transfer     = smart_io_i2c_transfer,
    .discover     = smart_io_i2c_discover,
    .deferred     = NULL,
};

/**** END OF FILE ****/
