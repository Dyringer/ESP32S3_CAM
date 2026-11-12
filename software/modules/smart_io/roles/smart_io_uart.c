/**
 ******************************************************************************
 * @file    smart_io_uart.c
 * @author  Daniel Dyringer
 * @date    2026-10-31
 * @brief   uart role.
 ******************************************************************************
 */


/* Includes ---------------------------------------------------------------- */
#include "smart_io_uart.h"

#include <stdlib.h>

#include "bsp_smart_io.h"
#include "driver/uart.h"
#include "esp_intr_alloc.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "smart_io.h"
#include "utils.h"


/* Constants and macros ---------------------------------------------------- */
// clang-format off

#define SMART_IO_UART_TX_BUFFER_SIZE    1024u
#define SMART_IO_UART_RX_BUFFER_MIN     256u
#define SMART_IO_UART_RX_BUFFER_MAX     4096u
/* RX buffer = 4 stream ticks (40 ms) at ~10 bits per byte: baud / 250. */
#define SMART_IO_UART_RX_BAUD_PER_BYTE  250u
/* UART_DATA events come every ~120 bytes: 32 last over a 40 ms stall at
 * 1 Mbaud, so an overflow event isn't crowded out. */
#define SMART_IO_UART_EVENT_DEPTH       32
#define SMART_IO_UART_BAUD_MIN          300u
#define SMART_IO_UART_BAUD_MAX          5000000u
#define SMART_IO_UART_BAUD              115200u
#define SMART_IO_UART_DATA_BITS_MIN     5u
#define SMART_IO_UART_DATA_BITS_MAX     8u
#define SMART_IO_UART_PORT_COUNT        ((uint32_t)UART_NUM_MAX                    \
                                         - (uint32_t)BSP_SMART_IO_UART_PORT_FIRST)

// clang-format on

/* Private types ----------------------------------------------------------- */

/* Index in config.param_values. */
typedef enum smart_io_uart_param_e
{
    SMART_IO_UART_PARAM_BAUD      = 0,
    SMART_IO_UART_PARAM_DATA_BITS = 1,
    SMART_IO_UART_PARAM_PARITY    = 2,
    SMART_IO_UART_PARAM_STOP_BITS = 3,
} smart_io_uart_param_t;

typedef enum smart_io_uart_parity_e
{
    SMART_IO_UART_PARITY_NONE = 0,
    SMART_IO_UART_PARITY_EVEN = 1,
    SMART_IO_UART_PARITY_ODD  = 2,
} smart_io_uart_parity_t;

typedef enum smart_io_uart_stop_bits_e
{
    SMART_IO_UART_STOP_BITS_1   = 0,
    SMART_IO_UART_STOP_BITS_1_5 = 1,
    SMART_IO_UART_STOP_BITS_2   = 2,
} smart_io_uart_stop_bits_t;

/* Copied from config.param_values at configure. */
typedef struct smart_io_uart_params_s
{
    uint32_t baud;
    uint32_t data_bits;
    uint32_t parity;
    uint32_t stop_bits;
} smart_io_uart_params_t;

/* instance->state.role_state. transfer (bus busy) and the stream drain use it
 * without the mutex; neither changes it. */
typedef struct smart_io_uart_state_s
{
    smart_io_uart_params_t params;
    QueueHandle_t events;
    uint32_t port; /* index from BSP_SMART_IO_UART_PORT_FIRST */
} smart_io_uart_state_t;


/* Private variables ------------------------------------------------------- */

static const char* const smart_io_uart_parity_names[] = {
    [SMART_IO_UART_PARITY_NONE] = "none",
    [SMART_IO_UART_PARITY_EVEN] = "even",
    [SMART_IO_UART_PARITY_ODD]  = "odd",
};

static const char* const smart_io_uart_stop_bits_names[] = {
    [SMART_IO_UART_STOP_BITS_1]   = "1",
    [SMART_IO_UART_STOP_BITS_1_5] = "1.5",
    [SMART_IO_UART_STOP_BITS_2]   = "2",
};

static const smart_io_param_descriptor_t smart_io_uart_params[] = {
    [SMART_IO_UART_PARAM_BAUD] =
        {
            .name          = "baud",
            .unit          = "baud",
            .option_names  = NULL,
            .min           = SMART_IO_UART_BAUD_MIN,
            .max           = SMART_IO_UART_BAUD_MAX,
            .default_value = SMART_IO_UART_BAUD,
            .type          = SMART_IO_PARAM_TYPE_U32,
            .shared        = false,
        },
    [SMART_IO_UART_PARAM_DATA_BITS] =
        {
            .name          = "data_bits",
            .unit          = NULL,
            .option_names  = NULL,
            .min           = SMART_IO_UART_DATA_BITS_MIN,
            .max           = SMART_IO_UART_DATA_BITS_MAX,
            .default_value = SMART_IO_UART_DATA_BITS_MAX,
            .type          = SMART_IO_PARAM_TYPE_U32,
            .shared        = false,
        },
    [SMART_IO_UART_PARAM_PARITY] =
        {
            .name          = "parity",
            .unit          = NULL,
            .option_names  = smart_io_uart_parity_names,
            .min           = SMART_IO_UART_PARITY_NONE,
            .max           = SMART_IO_UART_PARITY_ODD,
            .default_value = SMART_IO_UART_PARITY_NONE,
            .type          = SMART_IO_PARAM_TYPE_ENUM,
            .shared        = false,
        },
    [SMART_IO_UART_PARAM_STOP_BITS] =
        {
            .name          = "stop_bits",
            .unit          = NULL,
            .option_names  = smart_io_uart_stop_bits_names,
            .min           = SMART_IO_UART_STOP_BITS_1,
            .max           = SMART_IO_UART_STOP_BITS_2,
            .default_value = SMART_IO_UART_STOP_BITS_1,
            .type          = SMART_IO_PARAM_TYPE_ENUM,
            .shared        = false,
        },
};

/* Guarded by the smart_io mutex. */
static uint32_t smart_io_uart_ports_used = 0u;


/* Private functions ------------------------------------------------------- */

static uart_port_t smart_io_uart_port_id(uint32_t port)
{
    return (uart_port_t)((uint32_t)BSP_SMART_IO_UART_PORT_FIRST + port);
}

static bool smart_io_uart_port_take(uint32_t* port)
{
    bool found = false;

    for (uint32_t i = 0u; (i < SMART_IO_UART_PORT_COUNT) && !found; i++)
    {
        if ((smart_io_uart_ports_used & ((uint32_t)1u << i)) == 0u)
        {
            smart_io_uart_ports_used |= (uint32_t)1u << i;
            *port = i;
            found = true;
        }
    }

    return found;
}

static void smart_io_uart_port_give(uint32_t port)
{
    smart_io_uart_ports_used &= ~((uint32_t)1u << port);
}

static uart_parity_t smart_io_uart_parity(uint32_t parity)
{
    uart_parity_t mode = UART_PARITY_DISABLE;

    switch ((smart_io_uart_parity_t)parity)
    {
        case SMART_IO_UART_PARITY_EVEN:
        {
            mode = UART_PARITY_EVEN;
        }
        break;
        case SMART_IO_UART_PARITY_ODD:
        {
            mode = UART_PARITY_ODD;
        }
        break;
        default:
        {
            mode = UART_PARITY_DISABLE;
        }
        break;
    }

    return mode;
}

static uart_stop_bits_t smart_io_uart_stop_bits(uint32_t stop_bits)
{
    uart_stop_bits_t bits = UART_STOP_BITS_1;

    switch ((smart_io_uart_stop_bits_t)stop_bits)
    {
        case SMART_IO_UART_STOP_BITS_1_5:
        {
            bits = UART_STOP_BITS_1_5;
        }
        break;
        case SMART_IO_UART_STOP_BITS_2:
        {
            bits = UART_STOP_BITS_2;
        }
        break;
        default:
        {
            bits = UART_STOP_BITS_1;
        }
        break;
    }

    return bits;
}

static esp_err_t smart_io_uart_hardware_setup(
    const smart_io_instance_t* instance,
    smart_io_uart_state_t* state,
    smart_io_reason_t* reason)
{
    const uart_port_t port          = smart_io_uart_port_id(state->port);
    const uart_config_t uart_config = {
        .baud_rate = (int)state->params.baud,
        .data_bits =
            (uart_word_length_t)(state->params.data_bits - SMART_IO_UART_DATA_BITS_MIN),
        .parity                = smart_io_uart_parity(state->params.parity),
        .stop_bits             = smart_io_uart_stop_bits(state->params.stop_bits),
        .flow_ctrl             = UART_HW_FLOWCTRL_DISABLE,
        .rx_flow_ctrl_thresh   = 0u,
        .rx_glitch_filt_thresh = 0u,
        .source_clk            = UART_SCLK_DEFAULT,
        .flags =
            {
                .allow_pd            = 0u,
                .backup_before_sleep = 0u,
            },
    };
    const uint32_t rx_size = CLAMP(
        state->params.baud / SMART_IO_UART_RX_BAUD_PER_BYTE,
        SMART_IO_UART_RX_BUFFER_MIN,
        SMART_IO_UART_RX_BUFFER_MAX);
    esp_err_t error_code = uart_param_config(port, &uart_config);

    if (error_code == ESP_OK)
    {
        error_code = uart_set_pin(
            port,
            instance->config.gpio[0],
            instance->config.gpio[1],
            UART_PIN_NO_CHANGE,
            UART_PIN_NO_CHANGE);
    }

    /* The driver's interrupt lands on this task's core (SMART_IO2.md §5.1). IRAM
     * to match CONFIG_UART_ISR_IN_IRAM, which the driver would otherwise log. */
    if (error_code == ESP_OK)
    {
        error_code = uart_driver_install(
            port,
            (int)rx_size,
            (int)SMART_IO_UART_TX_BUFFER_SIZE,
            SMART_IO_UART_EVENT_DEPTH,
            &state->events,
            ESP_INTR_FLAG_IRAM);
    }

    if (error_code != ESP_OK)
    {
        smart_io_reason_set(
            reason, "UART%d setup failed (%s)", (int)port, esp_err_to_name(error_code));
    }

    return error_code;
}

static void smart_io_uart_release(smart_io_instance_t* instance)
{
    smart_io_uart_state_t* const state = instance->state.role_state;

    if (state != NULL)
    {
        uart_driver_delete(smart_io_uart_port_id(state->port));
        smart_io_uart_port_give(state->port);
        free(state);
        instance->state.role_state = NULL;
    }
}

static esp_err_t smart_io_uart_configure(smart_io_instance_t* instance, smart_io_reason_t* reason)
{
    esp_err_t error_code               = ESP_OK;
    smart_io_uart_state_t* const state = calloc(1u, sizeof(*state));
    bool port_taken                    = false;

    do
    {
        if (state == NULL)
        {
            error_code = ESP_ERR_NO_MEM;
            smart_io_reason_set(reason, "Out of memory");
            break;
        }

        state->params.baud = instance->config.param_values[SMART_IO_UART_PARAM_BAUD];
        state->params.data_bits =
            instance->config.param_values[SMART_IO_UART_PARAM_DATA_BITS];
        state->params.parity = instance->config.param_values[SMART_IO_UART_PARAM_PARITY];
        state->params.stop_bits =
            instance->config.param_values[SMART_IO_UART_PARAM_STOP_BITS];

        port_taken = smart_io_uart_port_take(&state->port);

        if (!port_taken)
        {
            error_code = ESP_ERR_NOT_FOUND;
            smart_io_reason_set(
                reason, "No free UART (%lu in use)", (unsigned long)SMART_IO_UART_PORT_COUNT);
            break;
        }

        error_code = smart_io_uart_hardware_setup(instance, state, reason);
    } while (0);

    if (error_code == ESP_OK)
    {
        instance->state.role_state = state;
    }
    else
    {
        if (port_taken)
        {
            smart_io_uart_port_give(state->port);
        }

        free(state);
    }

    return error_code;
}

/* Sends only what fits the TX buffer at once, so it never blocks. */
static esp_err_t smart_io_uart_transfer(
    smart_io_instance_t* instance,
    const smart_io_transfer_t* transfers,
    size_t count,
    size_t* failed,
    smart_io_reason_t* reason)
{
    const smart_io_uart_state_t* const state = instance->state.role_state;
    const uart_port_t port                   = smart_io_uart_port_id(state->port);
    esp_err_t error_code                     = ESP_OK;

    for (size_t i = 0u; (i < count) && (error_code == ESP_OK); i++)
    {
        if ((transfers[i].address != 0u) || (transfers[i].rx_length != 0u))
        {
            error_code = ESP_ERR_INVALID_ARG;
            *failed    = i;
            smart_io_reason_set(reason, "uart sends only: addr and rx must be 0");
        }
    }

    for (size_t i = 0u; (i < count) && (error_code == ESP_OK); i++)
    {
        size_t free_size = 0u;

        error_code = uart_get_tx_buffer_free_size(port, &free_size);

        if (error_code != ESP_OK)
        {
            smart_io_reason_set(
                reason,
                "UART%d TX state unknown (%s)",
                (int)port,
                esp_err_to_name(error_code));
        }
        else if (free_size < transfers[i].tx_length)
        {
            error_code = ESP_ERR_INVALID_STATE;
            smart_io_reason_set(reason, "TX full (%u bytes free)", (unsigned)free_size);
        }
        else if (uart_write_bytes(port, transfers[i].tx, transfers[i].tx_length) < 0)
        {
            error_code = ESP_FAIL;
            smart_io_reason_set(reason, "UART%d write failed", (int)port);
        }
        else
        {
            /* Queued. */
        }

        if (error_code != ESP_OK)
        {
            *failed = i;
        }
    }

    return error_code;
}

/* sio_strm. Reads only what is buffered and drains the event queue for
 * losses: both calls return at once. */
static size_t smart_io_uart_drain(void* context, uint8_t* buffer, size_t capacity, bool* overflow)
{
    const smart_io_uart_state_t* const state = context;
    const uart_port_t port                   = smart_io_uart_port_id(state->port);
    size_t buffered                          = 0u;
    size_t count                             = 0u;
    uart_event_t event                       = {0};

    if (uart_get_buffered_data_len(port, &buffered) == ESP_OK)
    {
        const int read =
            uart_read_bytes(port, buffer, (uint32_t)MIN(buffered, capacity), 0u);

        count = (read > 0) ? (size_t)read : 0u;
    }

    while (xQueueReceive(state->events, &event, 0u) == pdTRUE)
    {
        if ((event.type == UART_FIFO_OVF) || (event.type == UART_BUFFER_FULL))
        {
            *overflow = true;
        }
    }

    return count;
}

/* Losses before the stream started are not this stream's: the event queue
 * starts empty. Bytes still buffered go out with the first tick. */
static esp_err_t smart_io_uart_stream_start(
    smart_io_instance_t* instance,
    smart_io_ring_t* ring,
    smart_io_stream_source_t* source,
    smart_io_reason_t* reason)
{
    smart_io_uart_state_t* const state = instance->state.role_state;

    ARG_UNUSED(ring);
    ARG_UNUSED(reason);

    xQueueReset(state->events);
    source->drain   = smart_io_uart_drain;
    source->context = state;

    return ESP_OK;
}


/* Exposed API ------------------------------------------------------------- */

const smart_io_role_api_t smart_io_uart_api = {
    .description =
        {
            .name              = "uart",
            .param_descriptors = smart_io_uart_params,
            .param_count       = ARRAY_SIZE(smart_io_uart_params),
            .value_unit        = NULL,
            .value_min         = 0,
            .value_max         = 0,
            .role              = SMART_IO_ROLE_UART,
            .stream_kind       = SMART_IO_STREAM_KIND_BYTES,
            .pin_functions     = {SMART_IO_FUNCTION_UART_TX, SMART_IO_FUNCTION_UART_RX},
        },
    .validate     = NULL,
    .configure    = smart_io_uart_configure,
    .release      = smart_io_uart_release,
    .read         = NULL,
    .write        = NULL,
    .stream_start = smart_io_uart_stream_start,
    .stream_stop  = NULL,
    .transfer     = smart_io_uart_transfer,
    .discover     = NULL,
    .deferred     = NULL,
};

/**** END OF FILE ****/
