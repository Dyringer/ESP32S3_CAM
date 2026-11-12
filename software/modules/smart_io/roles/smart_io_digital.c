/**
 ******************************************************************************
 * @file    smart_io_digital.c
 * @author  Daniel Dyringer
 * @date    2026-10-31
 * @brief   input and output roles.
 ******************************************************************************
 */


/* Includes ---------------------------------------------------------------- */
#include "smart_io_digital.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "driver/gpio.h"
#include "esp_attr.h"
#include "esp_intr_alloc.h"
#include "esp_ipc.h"
#include "esp_timer.h"
#include "hal/gpio_ll.h"
#include "smart_io.h"
#include "smart_io_registry.h"
#include "smart_io_stream_source.h"
#include "utils.h"


/* Constants and macros ---------------------------------------------------- */
// clang-format off

#define SMART_IO_DIGITAL_DEBOUNCE_MAX_MS    1000u
#define SMART_IO_DIGITAL_DEBOUNCE_MS        10u
#define SMART_IO_DIGITAL_STREAM_PINS        32u
/* Control value refresh of a streamed input, at the update message rate. */
#define SMART_IO_DIGITAL_STREAM_REFRESH_MS  50u

// clang-format on

/* Private types ----------------------------------------------------------- */

/* Index in config.param_values. */
typedef enum smart_io_digital_input_param_e
{
    SMART_IO_DIGITAL_INPUT_PARAM_PULL        = 0,
    SMART_IO_DIGITAL_INPUT_PARAM_DEBOUNCE_MS = 1,
} smart_io_digital_input_param_t;

typedef enum smart_io_digital_output_param_e
{
    SMART_IO_DIGITAL_OUTPUT_PARAM_OPEN_DRAIN = 0,
    SMART_IO_DIGITAL_OUTPUT_PARAM_INITIAL    = 1,
} smart_io_digital_output_param_t;

typedef enum smart_io_digital_pull_e
{
    SMART_IO_DIGITAL_PULL_NONE = 0,
    SMART_IO_DIGITAL_PULL_UP   = 1,
    SMART_IO_DIGITAL_PULL_DOWN = 2,
} smart_io_digital_pull_t;

/* Copied from config.param_values at configure. */
typedef struct smart_io_digital_input_params_s
{
    uint32_t pull;
    uint32_t debounce_ms;
} smart_io_digital_input_params_t;

typedef struct smart_io_digital_output_params_s
{
    uint32_t open_drain;
    uint32_t initial;
} smart_io_digital_output_params_t;

/* instance->state.role_state, in smart_io_digital_input_states. */
typedef struct smart_io_digital_input_state_s
{
    smart_io_digital_input_params_t params;
    uint8_t index; /* pool index */
    /* Not streamed: interrupt off, debounce timer running. Streamed: refresh
     * timer running, the ISR doesn't push. Read by the ISR. */
    bool settling;
} smart_io_digital_input_state_t;

typedef struct smart_io_digital_output_state_s
{
    smart_io_digital_output_params_t params;
} smart_io_digital_output_state_t;


/* Private variables ------------------------------------------------------- */

static const char* const smart_io_digital_pull_names[] = {
    [SMART_IO_DIGITAL_PULL_NONE] = "none",
    [SMART_IO_DIGITAL_PULL_UP]   = "up",
    [SMART_IO_DIGITAL_PULL_DOWN] = "down",
};

static const smart_io_param_descriptor_t smart_io_digital_input_params[] = {
    [SMART_IO_DIGITAL_INPUT_PARAM_PULL] =
        {
            .name          = "pull",
            .unit          = NULL,
            .option_names  = smart_io_digital_pull_names,
            .min           = SMART_IO_DIGITAL_PULL_NONE,
            .max           = SMART_IO_DIGITAL_PULL_DOWN,
            .default_value = SMART_IO_DIGITAL_PULL_NONE,
            .type          = SMART_IO_PARAM_TYPE_ENUM,
            .shared        = false,
        },
    [SMART_IO_DIGITAL_INPUT_PARAM_DEBOUNCE_MS] =
        {
            .name          = "debounce_ms",
            .unit          = "ms",
            .option_names  = NULL,
            .min           = 0u,
            .max           = SMART_IO_DIGITAL_DEBOUNCE_MAX_MS,
            .default_value = SMART_IO_DIGITAL_DEBOUNCE_MS,
            .type          = SMART_IO_PARAM_TYPE_U32,
            .shared        = false,
        },
};

static const smart_io_param_descriptor_t smart_io_digital_output_params[] = {
    [SMART_IO_DIGITAL_OUTPUT_PARAM_OPEN_DRAIN] =
        {
            .name          = "open_drain",
            .unit          = NULL,
            .option_names  = NULL,
            .min           = 0u,
            .max           = 1u,
            .default_value = 0u,
            .type          = SMART_IO_PARAM_TYPE_BOOL,
            .shared        = false,
        },
    [SMART_IO_DIGITAL_OUTPUT_PARAM_INITIAL] =
        {
            .name          = "initial",
            .unit          = NULL,
            .option_names  = NULL,
            .min           = 0u,
            .max           = 1u,
            .default_value = 0u,
            .type          = SMART_IO_PARAM_TYPE_U32,
            .shared        = false,
        },
};


/* Static, one per pool pin: the ISR reads an input's state, so it must never
 * be freed. In internal RAM for the ISR. */
static DRAM_ATTR smart_io_digital_input_state_t
    smart_io_digital_input_states[SMART_IO_MAX_PINS];

/* Streamed inputs. Written under the smart_io mutex, read by the ISR. */
static uint32_t smart_io_digital_streamed = 0u;
static gpio_num_t smart_io_digital_stream_gpios[SMART_IO_DIGITAL_STREAM_PINS];
static smart_io_ring_t* smart_io_digital_ring = NULL;


/* Private functions ------------------------------------------------------- */

static int32_t smart_io_digital_level(gpio_num_t gpio)
{
    return (gpio_get_level(gpio) != 0) ? 1 : 0;
}

/* ---- Input ---- */

static bool smart_io_digital_is_streamed(const smart_io_digital_input_state_t* state)
{
    const uint32_t streamed = __atomic_load_n(&smart_io_digital_streamed, __ATOMIC_ACQUIRE);

    return (streamed & ((uint32_t)1u << state->index)) != 0u;
}

/* Levels of every streamed input in pool order, so simultaneous edges share
 * one snapshot and timing between pins is exact. */
static uint32_t IRAM_ATTR smart_io_digital_snapshot(uint32_t streamed)
{
    uint32_t levels    = 0u;
    uint32_t remaining = streamed;

    while (remaining != 0u)
    {
        const uint32_t index = (uint32_t)__builtin_ctz(remaining);

        remaining &= remaining - 1u;

        if (gpio_ll_get_level(&GPIO, (uint32_t)smart_io_digital_stream_gpios[index]) != 0)
        {
            levels |= (uint32_t)1u << index;
        }
    }

    return levels;
}

/* IRAM, may run with the flash cache off: LL only. arg is the instance slot,
 * which is static, so a late interrupt after release only queues a deferred
 * call that smart_io drops or the new role ignores. A streamed pin keeps its
 * interrupt on (until the edge budget is spent) and asks for a control value
 * refresh only when none is pending, so fast edges cost no work_queue runs. */
static void IRAM_ATTR smart_io_digital_isr(void* arg)
{
    smart_io_instance_t* const instance               = arg;
    const smart_io_digital_input_state_t* const state = instance->state.role_state;
    const uint32_t gpio = (uint32_t)instance->config.gpio[0];
    const uint32_t streamed = __atomic_load_n(&smart_io_digital_streamed, __ATOMIC_ACQUIRE);

    if ((state != NULL) && ((streamed & ((uint32_t)1u << state->index)) != 0u))
    {
        const smart_io_stream_digital_sample_t sample = {
            .time_us = (uint32_t)esp_timer_get_time(),
            .levels  = smart_io_digital_snapshot(streamed),
        };

        smart_io_ring_push(smart_io_digital_ring, &sample, sizeof(sample));

        if (smart_io_stream_edge_from_isr())
        {
            gpio_ll_intr_disable(&GPIO, gpio);
        }

        if (!__atomic_load_n(&state->settling, __ATOMIC_ACQUIRE))
        {
            work_queue_push_from_isr(&instance->work);
        }
    }
    else
    {
        gpio_ll_intr_disable(&GPIO, gpio);
        work_queue_push_from_isr(&instance->work);
    }
}

static void smart_io_digital_input_release(smart_io_instance_t* instance)
{
    const gpio_num_t gpio = instance->config.gpio[0];

    gpio_intr_disable(gpio);
    gpio_isr_handler_remove(gpio);
    instance->state.role_state = NULL;
}

static esp_err_t smart_io_digital_input_configure(
    smart_io_instance_t* instance,
    smart_io_reason_t* reason)
{
    esp_err_t error_code  = ESP_OK;
    const gpio_num_t gpio = instance->config.gpio[0];
    smart_io_digital_input_state_t* const state =
        &smart_io_digital_input_states[instance->index];

    memset(state, 0, sizeof(*state));
    instance->state.role_state = state;

    do
    {
        state->index = instance->index;
        state->params.pull =
            instance->config.param_values[SMART_IO_DIGITAL_INPUT_PARAM_PULL];
        state->params.debounce_ms =
            instance->config.param_values[SMART_IO_DIGITAL_INPUT_PARAM_DEBOUNCE_MS];

        const gpio_config_t io_config = {
            .pin_bit_mask = (uint64_t)1u << (uint32_t)gpio,
            .mode         = GPIO_MODE_INPUT,
            .pull_up_en   = (state->params.pull == SMART_IO_DIGITAL_PULL_UP) ?
                                GPIO_PULLUP_ENABLE :
                                GPIO_PULLUP_DISABLE,
            .pull_down_en = (state->params.pull == SMART_IO_DIGITAL_PULL_DOWN) ?
                                GPIO_PULLDOWN_ENABLE :
                                GPIO_PULLDOWN_DISABLE,
            .intr_type    = GPIO_INTR_ANYEDGE,
        };

        error_code = gpio_config(&io_config);

        if (error_code == ESP_OK)
        {
            error_code = gpio_isr_handler_add(gpio, smart_io_digital_isr, instance);
        }

        if (error_code == ESP_OK)
        {
            error_code = gpio_intr_enable(gpio);
        }

        if (error_code != ESP_OK)
        {
            smart_io_reason_set(
                reason,
                "GPIO %d: interrupt setup failed (%s)",
                (int)gpio,
                esp_err_to_name(error_code));
            break;
        }

        /* Read after the interrupt is on: a later edge queues a fresh read. */
        smart_io_value_changed(instance, smart_io_digital_level(gpio));
    } while (0);

    if (error_code != ESP_OK)
    {
        smart_io_digital_input_release(instance);
    }

    return error_code;
}

static esp_err_t smart_io_digital_input_read(
    smart_io_instance_t* instance,
    int32_t* value,
    smart_io_reason_t* reason)
{
    ARG_UNUSED(reason);

    *value = smart_io_digital_level(instance->config.gpio[0]);

    return ESP_OK;
}

/* First call after an edge starts the debounce wait, the second re-arms the
 * interrupt and reports the settled level. Streamed pins: the first starts the
 * refresh wait, the second reports the level; the interrupt stays on. */
static void smart_io_digital_input_deferred(smart_io_instance_t* instance)
{
    smart_io_digital_input_state_t* const state = instance->state.role_state;
    const gpio_num_t gpio                       = instance->config.gpio[0];

    if ((state != NULL) && smart_io_digital_is_streamed(state) && !state->settling)
    {
        __atomic_store_n(&state->settling, true, __ATOMIC_RELEASE);
        work_queue_schedule(&instance->work, SMART_IO_DIGITAL_STREAM_REFRESH_MS);
    }
    else if ((state != NULL) && smart_io_digital_is_streamed(state))
    {
        __atomic_store_n(&state->settling, false, __ATOMIC_RELEASE);
        smart_io_value_changed(instance, smart_io_digital_level(gpio));
    }
    else if ((state != NULL) && !state->settling && (state->params.debounce_ms != 0u))
    {
        state->settling = true;
        work_queue_schedule(&instance->work, state->params.debounce_ms);
    }
    else if (state != NULL)
    {
        state->settling = false;
        gpio_intr_enable(gpio);
        smart_io_value_changed(instance, smart_io_digital_level(gpio));
    }
    else
    {
        /* Instance released while the work was queued. */
    }
}

/* Edge flood, from sio_strm: the instance is stable while it is streamed. */
static void smart_io_digital_input_pause(void* context)
{
    const smart_io_instance_t* const instance = context;

    gpio_intr_disable(instance->config.gpio[0]);
}

static int32_t smart_io_digital_input_resume(void* context)
{
    const smart_io_instance_t* const instance = context;
    const gpio_num_t gpio                     = instance->config.gpio[0];

    gpio_intr_enable(gpio);

    return smart_io_digital_level(gpio);
}

/* The bit goes on first: snapshots taken from since_us on carry this pin, and
 * the level read after it is the starting point. */
static esp_err_t smart_io_digital_input_stream_start(
    smart_io_instance_t* instance,
    smart_io_ring_t* ring,
    smart_io_stream_source_t* source,
    smart_io_reason_t* reason)
{
    ARG_UNUSED(reason);

    smart_io_digital_input_state_t* const state = instance->state.role_state;
    const gpio_num_t gpio                       = instance->config.gpio[0];

    smart_io_digital_ring                       = ring;
    smart_io_digital_stream_gpios[state->index] = gpio;
    __atomic_or_fetch(
        &smart_io_digital_streamed, (uint32_t)1u << state->index, __ATOMIC_RELEASE);

    source->since_us = esp_timer_get_time();
    source->initial  = smart_io_digital_level(gpio);
    source->pause    = smart_io_digital_input_pause;
    source->resume   = smart_io_digital_input_resume;
    source->context  = instance;
    state->settling  = false;
    gpio_intr_enable(gpio);

    return ESP_OK;
}

/* The interrupt may be off (edge flood or a pending debounce): back on, so
 * the pin returns to the debounced control path. */
static void smart_io_digital_input_stream_stop(smart_io_instance_t* instance)
{
    const smart_io_digital_input_state_t* const state = instance->state.role_state;

    __atomic_and_fetch(
        &smart_io_digital_streamed, ~((uint32_t)1u << state->index), __ATOMIC_RELEASE);
    gpio_intr_enable(instance->config.gpio[0]);
}

/* ---- Output ---- */

static void smart_io_digital_output_release(smart_io_instance_t* instance)
{
    free(instance->state.role_state);
    instance->state.role_state = NULL;
}

static esp_err_t smart_io_digital_output_configure(
    smart_io_instance_t* instance,
    smart_io_reason_t* reason)
{
    esp_err_t error_code                         = ESP_OK;
    const gpio_num_t gpio                        = instance->config.gpio[0];
    smart_io_digital_output_state_t* const state = calloc(1u, sizeof(*state));

    instance->state.role_state = state;

    do
    {
        if (state == NULL)
        {
            error_code = ESP_ERR_NO_MEM;
            smart_io_reason_set(reason, "Out of memory");
            break;
        }

        state->params.open_drain =
            instance->config.param_values[SMART_IO_DIGITAL_OUTPUT_PARAM_OPEN_DRAIN];
        state->params.initial =
            instance->config.param_values[SMART_IO_DIGITAL_OUTPUT_PARAM_INITIAL];

        const gpio_config_t io_config = {
            .pin_bit_mask = (uint64_t)1u << (uint32_t)gpio,
            .mode =
                (state->params.open_drain != 0u) ? GPIO_MODE_OUTPUT_OD : GPIO_MODE_OUTPUT,
            .pull_up_en   = GPIO_PULLUP_DISABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type    = GPIO_INTR_DISABLE,
        };

        /* Level first, so enabling the driver never glitches the pin. */
        error_code = gpio_set_level(gpio, state->params.initial);

        if (error_code == ESP_OK)
        {
            error_code = gpio_config(&io_config);
        }

        if (error_code != ESP_OK)
        {
            smart_io_reason_set(
                reason,
                "GPIO %d: output setup failed (%s)",
                (int)gpio,
                esp_err_to_name(error_code));
            break;
        }

        smart_io_value_changed(instance, (int32_t)state->params.initial);
        smart_io_value_sample(
            instance, (int32_t)state->params.initial, esp_timer_get_time());
    } while (0);

    if ((error_code != ESP_OK) && (state != NULL))
    {
        smart_io_digital_output_release(instance);
    }

    return error_code;
}

static esp_err_t smart_io_digital_output_read(
    smart_io_instance_t* instance,
    int32_t* value,
    smart_io_reason_t* reason)
{
    ARG_UNUSED(reason);

    *value = instance->state.value;

    return ESP_OK;
}

static esp_err_t smart_io_digital_output_write(
    smart_io_instance_t* instance,
    int32_t value,
    int32_t* applied,
    smart_io_reason_t* reason)
{
    const gpio_num_t gpio      = instance->config.gpio[0];
    const esp_err_t error_code = gpio_set_level(gpio, (value != 0) ? 1u : 0u);

    if (error_code == ESP_OK)
    {
        *applied = (value != 0) ? 1 : 0;

        if (*applied != instance->state.value)
        {
            smart_io_value_sample(instance, *applied, esp_timer_get_time());
        }
    }
    else
    {
        smart_io_reason_set(
            reason, "GPIO %d: write failed (%s)", (int)gpio, esp_err_to_name(error_code));
    }

    return error_code;
}

/* ---- Init ---- */

static void smart_io_digital_install(void* arg)
{
    esp_err_t* const result = arg;

    *result = gpio_install_isr_service(ESP_INTR_FLAG_IRAM);
}


/* Exposed API ------------------------------------------------------------- */

const smart_io_role_api_t smart_io_digital_input_api = {
    .description =
        {
            .name              = "input",
            .param_descriptors = smart_io_digital_input_params,
            .param_count       = ARRAY_SIZE(smart_io_digital_input_params),
            .value_unit        = "level",
            .value_min         = 0,
            .value_max         = 1,
            .role              = SMART_IO_ROLE_INPUT,
            .stream_kind       = SMART_IO_STREAM_KIND_DIGITAL,
            .pin_functions     = {SMART_IO_FUNCTION_INPUT, SMART_IO_FUNCTION_NONE},
        },
    .validate     = NULL,
    .configure    = smart_io_digital_input_configure,
    .release      = smart_io_digital_input_release,
    .read         = smart_io_digital_input_read,
    .write        = NULL,
    .stream_start = smart_io_digital_input_stream_start,
    .stream_stop  = smart_io_digital_input_stream_stop,
    .transfer     = NULL,
    .discover     = NULL,
    .deferred     = smart_io_digital_input_deferred,
};

const smart_io_role_api_t smart_io_digital_output_api = {
    .description =
        {
            .name              = "output",
            .param_descriptors = smart_io_digital_output_params,
            .param_count       = ARRAY_SIZE(smart_io_digital_output_params),
            .value_unit        = "level",
            .value_min         = 0,
            .value_max         = 1,
            .role              = SMART_IO_ROLE_OUTPUT,
            .stream_kind       = SMART_IO_STREAM_KIND_VALUE,
            .pin_functions     = {SMART_IO_FUNCTION_OUTPUT, SMART_IO_FUNCTION_NONE},
        },
    .validate     = NULL,
    .configure    = smart_io_digital_output_configure,
    .release      = smart_io_digital_output_release,
    .read         = smart_io_digital_output_read,
    .write        = smart_io_digital_output_write,
    .stream_start = NULL,
    .stream_stop  = NULL,
    .transfer     = NULL,
    .discover     = NULL,
    .deferred     = NULL,
};

/* The service is shared with other modules; their handlers run on core 1 too
 * and must be IRAM. Already installed counts as success. */
esp_err_t smart_io_digital_init(void)
{
    esp_err_t error_code = ESP_OK;

    const esp_err_t ipc_error = esp_ipc_call_blocking(
        SMART_IO_DIGITAL_ISR_CORE, smart_io_digital_install, &error_code);

    if (ipc_error != ESP_OK)
    {
        error_code = ipc_error;
    }
    else if (error_code == ESP_ERR_INVALID_STATE)
    {
        error_code = ESP_OK;
    }
    else
    {
        /* Installed by smart_io, or failed. */
    }

    return error_code;
}

/**** END OF FILE ****/
