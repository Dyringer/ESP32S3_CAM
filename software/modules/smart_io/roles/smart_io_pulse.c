/**
 ******************************************************************************
 * @file    smart_io_pulse.c
 * @author  Daniel Dyringer
 * @date    2026-10-31
 * @brief   pulse role.
 ******************************************************************************
 */


/* Includes ---------------------------------------------------------------- */
#include "smart_io_pulse.h"

#include "driver/rmt_encoder.h"
#include "driver/rmt_tx.h"
#include "esp_attr.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "hal/rmt_ll.h"
#include "smart_io.h"
#include "soc/soc_caps.h"
#include "utils.h"


/* Constants and macros ---------------------------------------------------- */
// clang-format off

#define SMART_IO_PULSE_SLOTS            RMT_LL_TX_CANDIDATES_PER_INST
#define SMART_IO_PULSE_RESOLUTION_HZ    1000000u   /* 1 tick = 1 us */
#define SMART_IO_PULSE_MEM_SYMBOLS      SOC_RMT_MEM_WORDS_PER_CHANNEL
#define SMART_IO_PULSE_QUEUE_DEPTH      4u
#define SMART_IO_PULSE_DURATION_MAX     32767u     /* 15-bit symbol duration */
#define SMART_IO_PULSE_WIDTH_US         1000u
#define SMART_IO_PULSE_LOOP_PERIOD_MAX  65534u
#define SMART_IO_PULSE_PERIOD_MAX_US    3600000000u
#define SMART_IO_PULSE_DELAY_MAX_MS     3600000u
#define SMART_IO_PULSE_COUNT_MAX        2147483647u /* rmt loop_count is an int */
#define SMART_IO_PULSE_FIRE_DEPTH       8u          /* power of two */
#define SMART_IO_PULSE_ENDLESS          UINT32_MAX  /* fires_left of an endless train */

// clang-format on

/* Private types ----------------------------------------------------------- */

/* Index in config.param_values. */
typedef enum smart_io_pulse_param_e
{
    SMART_IO_PULSE_PARAM_WIDTH_US  = 0,
    SMART_IO_PULSE_PARAM_PERIOD_US = 1,
    SMART_IO_PULSE_PARAM_COUNT     = 2,
    SMART_IO_PULSE_PARAM_DELAY_MS  = 3,
} smart_io_pulse_param_t;

/* SINGLE: one pulse. LOOP: the whole train is one RMT loop transmission (the
 * driver restarts the loop every 1023 pulses, a few us late). TIMER: periods
 * too long for RMT, esp_timer fires one RMT pulse per period. */
typedef enum smart_io_pulse_mode_e
{
    SMART_IO_PULSE_MODE_SINGLE = 0,
    SMART_IO_PULSE_MODE_LOOP   = 1,
    SMART_IO_PULSE_MODE_TIMER  = 2,
} smart_io_pulse_mode_t;

/* Copied from config.param_values at configure. */
typedef struct smart_io_pulse_params_s
{
    uint32_t width_us;
    uint32_t period_us;
    uint32_t count;
    uint32_t delay_ms;
} smart_io_pulse_params_t;

/* instance->state.role_state. Static, never freed: the esp_timer callback and
 * the RMT ISR may still run for a released pin, and only touch this.
 *
 * Every start goes through the esp_timer (delay 0 included), so the callback
 * is the only producer of fire times. fire_lock: the callback takes it with 0
 * timeout around rmt_transmit(); stop and release wait for it before touching
 * the channel. The atomics are written by the callback (transmits, fires_left),
 * the ISR (done) and the mutex holder (reset, under fire_lock). The rest is
 * guarded by the smart_io mutex. */
typedef struct smart_io_pulse_state_s
{
    smart_io_pulse_params_t params;
    smart_io_instance_t* instance;
    rmt_channel_handle_t channel;
    rmt_encoder_handle_t encoder;
    esp_timer_handle_t timer;
    SemaphoreHandle_t fire_lock;
    smart_io_ring_t fires; /* int64_t fire time, callback -> deferred */
    int64_t fire_buffer[SMART_IO_PULSE_FIRE_DEPTH];
    rmt_symbol_word_t symbols[2];
    size_t symbol_count;
    smart_io_pulse_mode_t mode;
    uint32_t fires_left;
    uint32_t transmits;
    uint32_t done;
    uint32_t sampled; /* transmissions whose samples were sent */
    bool running;
    bool periodic;      /* TIMER: periodic alarm armed; callback and stop only */
    bool start_sampled; /* LOOP: rising edge of the current train sent */
    bool used;
} smart_io_pulse_state_t;


/* Private variables ------------------------------------------------------- */

static const smart_io_param_descriptor_t smart_io_pulse_params[] = {
    [SMART_IO_PULSE_PARAM_WIDTH_US] =
        {
            .name          = "width_us",
            .unit          = "us",
            .option_names  = NULL,
            .min           = 1u,
            .max           = SMART_IO_PULSE_DURATION_MAX,
            .default_value = SMART_IO_PULSE_WIDTH_US,
            .type          = SMART_IO_PARAM_TYPE_U32,
            .shared        = false,
        },
    [SMART_IO_PULSE_PARAM_PERIOD_US] =
        {
            .name          = "period_us",
            .unit          = "us",
            .option_names  = NULL,
            .min           = 0u,
            .max           = SMART_IO_PULSE_PERIOD_MAX_US,
            .default_value = 0u,
            .type          = SMART_IO_PARAM_TYPE_U32,
            .shared        = false,
        },
    [SMART_IO_PULSE_PARAM_COUNT] =
        {
            .name          = "count",
            .unit          = NULL,
            .option_names  = NULL,
            .min           = 0u,
            .max           = SMART_IO_PULSE_COUNT_MAX,
            .default_value = 0u,
            .type          = SMART_IO_PARAM_TYPE_U32,
            .shared        = false,
        },
    [SMART_IO_PULSE_PARAM_DELAY_MS] =
        {
            .name          = "delay_ms",
            .unit          = "ms",
            .option_names  = NULL,
            .min           = 0u,
            .max           = SMART_IO_PULSE_DELAY_MAX_MS,
            .default_value = 0u,
            .type          = SMART_IO_PARAM_TYPE_U32,
            .shared        = false,
        },
};

static smart_io_pulse_state_t smart_io_pulse_slots[SMART_IO_PULSE_SLOTS];


/* Private functions ------------------------------------------------------- */

/* ---- Pattern ---- */

static smart_io_pulse_mode_t smart_io_pulse_mode(const smart_io_pulse_params_t* params)
{
    smart_io_pulse_mode_t mode = SMART_IO_PULSE_MODE_TIMER;

    if (params->period_us == 0u)
    {
        mode = SMART_IO_PULSE_MODE_SINGLE;
    }
    else if (params->period_us <= SMART_IO_PULSE_LOOP_PERIOD_MAX)
    {
        mode = SMART_IO_PULSE_MODE_LOOP;
    }
    else
    {
        /* Long period: one pulse per esp_timer alarm. */
    }

    return mode;
}

static rmt_symbol_word_t smart_io_pulse_symbol(
    uint32_t level0,
    uint32_t duration0,
    uint32_t level1,
    uint32_t duration1)
{
    rmt_symbol_word_t symbol = {.val = 0u};

    symbol.level0    = (uint16_t)level0;
    symbol.duration0 = (uint16_t)duration0;
    symbol.level1    = (uint16_t)level1;
    symbol.duration1 = (uint16_t)duration1;

    return symbol;
}

/* A duration of 0 ends the transmission, so a low time longer than one
 * duration is split over three, each at least 1 us. */
static void smart_io_pulse_pattern(smart_io_pulse_state_t* state)
{
    const uint32_t width = state->params.width_us;

    if (state->mode != SMART_IO_PULSE_MODE_LOOP)
    {
        state->symbols[0]   = smart_io_pulse_symbol(1u, width, 0u, 1u);
        state->symbol_count = 1u;
    }
    else if ((state->params.period_us - width) <= SMART_IO_PULSE_DURATION_MAX)
    {
        state->symbols[0] =
            smart_io_pulse_symbol(1u, width, 0u, state->params.period_us - width);
        state->symbol_count = 1u;
    }
    else
    {
        const uint32_t low = state->params.period_us - width;
        uint32_t first     = SMART_IO_PULSE_DURATION_MAX;
        uint32_t rest      = low - first;

        if (rest < 2u)
        {
            first--;
            rest++;
        }

        state->symbols[0] = smart_io_pulse_symbol(1u, width, 0u, first);
        state->symbols[1] = smart_io_pulse_symbol(0u, rest / 2u, 0u, rest - (rest / 2u));
        state->symbol_count = 2u;
    }
}

/* Last falling edge a fire produces, no later than now_us. An endless LOOP
 * train only ends when it is stopped. */
static int64_t smart_io_pulse_fall_us(
    const smart_io_pulse_state_t* state,
    int64_t fire_us,
    int64_t now_us)
{
    int64_t fall_us = fire_us + (int64_t)state->params.width_us;

    if ((state->mode == SMART_IO_PULSE_MODE_LOOP) && (state->params.count == 0u))
    {
        fall_us = now_us;
    }
    else if (state->mode == SMART_IO_PULSE_MODE_LOOP)
    {
        fall_us += (int64_t)(state->params.count - 1u) * (int64_t)state->params.period_us;
    }
    else
    {
        /* SINGLE, TIMER: one pulse per fire. */
    }

    return MIN(fall_us, now_us);
}

/* ---- Hardware callbacks ---- */

static bool IRAM_ATTR smart_io_pulse_on_done(
    rmt_channel_handle_t channel,
    const rmt_tx_done_event_data_t* event,
    void* context)
{
    ARG_UNUSED(channel);
    ARG_UNUSED(event);

    smart_io_pulse_state_t* const state = context;

    __atomic_add_fetch(&state->done, 1u, __ATOMIC_RELEASE);
    work_queue_push_from_isr(&state->instance->work);

    return false;
}

static void smart_io_pulse_fire(smart_io_pulse_state_t* state)
{
    const int64_t now_us                        = esp_timer_get_time();
    const rmt_transmit_config_t transmit_config = {
        .loop_count = (state->mode != SMART_IO_PULSE_MODE_LOOP) ? 0 :
                      (state->params.count == 0u)               ? -1 :
                                                                  (int)state->params.count,
        .flags =
            {
                .eot_level         = 0u,
                .queue_nonblocking = 1u,
            },
    };

    const esp_err_t error_code = rmt_transmit(
        state->channel,
        state->encoder,
        state->symbols,
        state->symbol_count * sizeof(rmt_symbol_word_t),
        &transmit_config);

    if (error_code == ESP_OK)
    {
        smart_io_ring_push(&state->fires, &now_us, sizeof(now_us));
        __atomic_add_fetch(&state->transmits, 1u, __ATOMIC_RELEASE);
    }
}

/* esp_timer task. Never waits: a stop or release holding fire_lock wins and
 * this alarm is skipped. */
static void smart_io_pulse_on_timer(void* context)
{
    smart_io_pulse_state_t* const state = context;

    if (xSemaphoreTake(state->fire_lock, 0) == pdTRUE)
    {
        const uint32_t fires_left = __atomic_load_n(&state->fires_left, __ATOMIC_ACQUIRE);

        if (__atomic_load_n(&state->running, __ATOMIC_ACQUIRE) && (fires_left != 0u))
        {
            smart_io_pulse_fire(state);

            const uint32_t remaining =
                (fires_left == SMART_IO_PULSE_ENDLESS) ? fires_left : (fires_left - 1u);

            __atomic_store_n(&state->fires_left, remaining, __ATOMIC_RELEASE);

            if ((state->mode == SMART_IO_PULSE_MODE_TIMER) && (remaining == 0u))
            {
                esp_timer_stop(state->timer);
                state->periodic = false;
            }
            else if ((state->mode == SMART_IO_PULSE_MODE_TIMER) && !state->periodic)
            {
                state->periodic =
                    esp_timer_start_periodic(state->timer, state->params.period_us)
                    == ESP_OK;
            }
            else
            {
                /* SINGLE and LOOP fire once. */
            }

            work_queue_push(&state->instance->work);
        }

        xSemaphoreGive(state->fire_lock);
    }
}

/* ---- Samples and state ---- */

/* Fire times wait in the ring until their transmission is done. A LOOP train
 * is long, so its rising edge goes out as soon as it starts. */
static void smart_io_pulse_samples(smart_io_pulse_state_t* state)
{
    smart_io_instance_t* const instance = state->instance;
    int64_t fire_us                     = 0;
    bool more                           = true;

    while (more && smart_io_ring_peek(&state->fires, &fire_us, sizeof(fire_us)))
    {
        const uint32_t done = __atomic_load_n(&state->done, __ATOMIC_ACQUIRE);

        if (state->sampled == done)
        {
            if ((state->mode == SMART_IO_PULSE_MODE_LOOP) && !state->start_sampled)
            {
                smart_io_value_sample(instance, 1, fire_us);
                state->start_sampled = true;
            }

            more = false;
        }
        else
        {
            if (!state->start_sampled)
            {
                smart_io_value_sample(instance, 1, fire_us);
            }

            smart_io_value_sample(
                instance, 0, smart_io_pulse_fall_us(state, fire_us, esp_timer_get_time()));
            smart_io_ring_drop(&state->fires);
            state->sampled++;
            state->start_sampled = false;
        }
    }
}

static void smart_io_pulse_counters_reset(smart_io_pulse_state_t* state)
{
    smart_io_ring_init(
        &state->fires, (uint8_t*)state->fire_buffer, SMART_IO_PULSE_FIRE_DEPTH);
    __atomic_store_n(&state->transmits, 0u, __ATOMIC_RELEASE);
    __atomic_store_n(&state->done, 0u, __ATOMIC_RELEASE);
    __atomic_store_n(&state->fires_left, 0u, __ATOMIC_RELEASE);
    state->sampled       = 0u;
    state->start_sampled = false;
    state->periodic      = false;
}

/* Waits at most one rmt_transmit() in the callback. Disabling the channel
 * aborts a running transmission; the pin goes to the idle level (low).
 * Pending fires are closed at their fall time, or now if cut short. */
static void smart_io_pulse_halt(smart_io_pulse_state_t* state, bool keep_channel)
{
    const int64_t now_us = esp_timer_get_time();
    int64_t fire_us      = 0;

    xSemaphoreTake(state->fire_lock, portMAX_DELAY);

    __atomic_store_n(&state->running, false, __ATOMIC_RELEASE);
    esp_timer_stop(state->timer);
    rmt_disable(state->channel);

    if (keep_channel)
    {
        rmt_enable(state->channel);
    }

    while (smart_io_ring_pop(&state->fires, &fire_us, sizeof(fire_us)))
    {
        if (!state->start_sampled)
        {
            smart_io_value_sample(state->instance, 1, fire_us);
        }

        smart_io_value_sample(
            state->instance, 0, smart_io_pulse_fall_us(state, fire_us, now_us));
        state->start_sampled = false;
    }

    smart_io_pulse_counters_reset(state);

    xSemaphoreGive(state->fire_lock);
}

/* ---- Role ---- */

static esp_err_t smart_io_pulse_validate(
    const smart_io_config_t* config,
    smart_io_reason_t* reason)
{
    esp_err_t error_code  = ESP_OK;
    const uint32_t width  = config->param_values[SMART_IO_PULSE_PARAM_WIDTH_US];
    const uint32_t period = config->param_values[SMART_IO_PULSE_PARAM_PERIOD_US];

    if ((period != 0u) && (period <= width))
    {
        error_code = ESP_ERR_INVALID_ARG;
        smart_io_reason_set(
            reason, "Period must be longer than the width (%lu us)", (unsigned long)width);
    }

    return error_code;
}

static smart_io_pulse_state_t* smart_io_pulse_slot_take(void)
{
    smart_io_pulse_state_t* slot = NULL;

    for (size_t i = 0u; (i < SMART_IO_PULSE_SLOTS) && (slot == NULL); i++)
    {
        if (!smart_io_pulse_slots[i].used)
        {
            slot = &smart_io_pulse_slots[i];
        }
    }

    return slot;
}

/* The lock and the timer are created on first use and kept: a late callback
 * may still hold either. */
static esp_err_t smart_io_pulse_slot_prepare(smart_io_pulse_state_t* state)
{
    esp_err_t error_code = ESP_OK;

    if (state->fire_lock == NULL)
    {
        state->fire_lock = xSemaphoreCreateBinary();

        if (state->fire_lock == NULL)
        {
            error_code = ESP_ERR_NO_MEM;
        }
        else
        {
            xSemaphoreGive(state->fire_lock);
        }
    }

    if ((error_code == ESP_OK) && (state->timer == NULL))
    {
        const esp_timer_create_args_t timer_args = {
            .callback              = smart_io_pulse_on_timer,
            .arg                   = state,
            .dispatch_method       = ESP_TIMER_TASK,
            .name                  = "sio_pulse",
            .skip_unhandled_events = true,
        };

        error_code = esp_timer_create(&timer_args, &state->timer);
    }

    return error_code;
}

static void smart_io_pulse_channel_delete(smart_io_pulse_state_t* state)
{
    if (state->channel != NULL)
    {
        rmt_del_channel(state->channel);
        state->channel = NULL;
    }

    if (state->encoder != NULL)
    {
        rmt_del_encoder(state->encoder);
        state->encoder = NULL;
    }
}

static esp_err_t smart_io_pulse_channel_create(smart_io_pulse_state_t* state, gpio_num_t gpio)
{
    esp_err_t error_code                         = ESP_OK;
    const rmt_tx_channel_config_t channel_config = {
        .gpio_num          = gpio,
        .clk_src           = RMT_CLK_SRC_DEFAULT,
        .resolution_hz     = SMART_IO_PULSE_RESOLUTION_HZ,
        .mem_block_symbols = SMART_IO_PULSE_MEM_SYMBOLS,
        .trans_queue_depth = SMART_IO_PULSE_QUEUE_DEPTH,
        .intr_priority     = 0,
        .flags =
            {
                .invert_out = 0u,
                .with_dma   = 0u,
                .allow_pd   = 0u,
                .init_level = 0u,
            },
    };
    const rmt_copy_encoder_config_t encoder_config = {};
    const rmt_tx_event_callbacks_t callbacks       = {
              .on_trans_done = smart_io_pulse_on_done,
    };

    do
    {
        error_code = rmt_new_tx_channel(&channel_config, &state->channel);

        if (error_code != ESP_OK)
        {
            state->channel = NULL;
            break;
        }

        error_code = rmt_new_copy_encoder(&encoder_config, &state->encoder);

        if (error_code != ESP_OK)
        {
            state->encoder = NULL;
            break;
        }

        error_code = rmt_tx_register_event_callbacks(state->channel, &callbacks, state);

        if (error_code != ESP_OK)
        {
            break;
        }

        error_code = rmt_enable(state->channel);
    } while (0);

    if (error_code != ESP_OK)
    {
        smart_io_pulse_channel_delete(state);
    }

    return error_code;
}

static void smart_io_pulse_release(smart_io_instance_t* instance)
{
    smart_io_pulse_state_t* const state = instance->state.role_state;

    if (state != NULL)
    {
        smart_io_pulse_halt(state, false);
        smart_io_pulse_channel_delete(state);
        state->used                = false;
        instance->state.role_state = NULL;
    }
}

static esp_err_t smart_io_pulse_configure(smart_io_instance_t* instance, smart_io_reason_t* reason)
{
    esp_err_t error_code                = ESP_OK;
    const gpio_num_t gpio               = instance->config.gpio[0];
    smart_io_pulse_state_t* const state = smart_io_pulse_slot_take();

    do
    {
        if (state == NULL)
        {
            error_code = ESP_ERR_NOT_FOUND;
            smart_io_reason_set(
                reason, "All %u pulse channels in use", (unsigned)SMART_IO_PULSE_SLOTS);
            break;
        }

        error_code = smart_io_pulse_slot_prepare(state);

        if (error_code != ESP_OK)
        {
            smart_io_reason_set(reason, "Out of memory");
            break;
        }

        state->instance = instance;
        state->params.width_us =
            instance->config.param_values[SMART_IO_PULSE_PARAM_WIDTH_US];
        state->params.period_us =
            instance->config.param_values[SMART_IO_PULSE_PARAM_PERIOD_US];
        state->params.count = instance->config.param_values[SMART_IO_PULSE_PARAM_COUNT];
        state->params.delay_ms =
            instance->config.param_values[SMART_IO_PULSE_PARAM_DELAY_MS];
        state->mode = smart_io_pulse_mode(&state->params);
        smart_io_pulse_pattern(state);
        smart_io_pulse_counters_reset(state);
        __atomic_store_n(&state->running, false, __ATOMIC_RELEASE);

        error_code = smart_io_pulse_channel_create(state, gpio);

        if (error_code != ESP_OK)
        {
            smart_io_reason_set(
                reason,
                "GPIO %d: RMT setup failed (%s)",
                (int)gpio,
                esp_err_to_name(error_code));
            break;
        }

        state->used                = true;
        instance->state.role_state = state;
        smart_io_value_changed(instance, 0);
        smart_io_value_sample(instance, 0, esp_timer_get_time());
    } while (0);

    return error_code;
}

static esp_err_t smart_io_pulse_read(
    smart_io_instance_t* instance,
    int32_t* value,
    smart_io_reason_t* reason)
{
    ARG_UNUSED(reason);

    *value = instance->state.value;

    return ESP_OK;
}

/* Absolute: 1 while running is a no-op, 1 after the train ended starts it
 * again, 0 stops it. */
static esp_err_t smart_io_pulse_write(
    smart_io_instance_t* instance,
    int32_t value,
    int32_t* applied,
    smart_io_reason_t* reason)
{
    esp_err_t error_code                = ESP_OK;
    smart_io_pulse_state_t* const state = instance->state.role_state;
    const bool running = __atomic_load_n(&state->running, __ATOMIC_ACQUIRE);

    if ((value == 0) && running)
    {
        smart_io_pulse_halt(state, true);
    }
    else if ((value != 0) && !running)
    {
        const bool endless =
            (state->mode == SMART_IO_PULSE_MODE_TIMER) && (state->params.count == 0u);
        const uint32_t fires =
            (state->mode == SMART_IO_PULSE_MODE_TIMER) ? state->params.count : 1u;

        /* Under fire_lock: an alarm of the previous train still being
         * dispatched must not see the new counters half set. */
        xSemaphoreTake(state->fire_lock, portMAX_DELAY);
        smart_io_pulse_counters_reset(state);
        __atomic_store_n(
            &state->fires_left, endless ? SMART_IO_PULSE_ENDLESS : fires, __ATOMIC_RELEASE);
        __atomic_store_n(&state->running, true, __ATOMIC_RELEASE);

        error_code =
            esp_timer_start_once(state->timer, (uint64_t)state->params.delay_ms * 1000u);

        if (error_code != ESP_OK)
        {
            __atomic_store_n(&state->running, false, __ATOMIC_RELEASE);
        }

        xSemaphoreGive(state->fire_lock);

        if (error_code != ESP_OK)
        {
            smart_io_reason_set(
                reason,
                "GPIO %d: start failed (%s)",
                (int)instance->config.gpio[0],
                esp_err_to_name(error_code));
        }
    }
    else
    {
        /* Already in the state asked for. */
    }

    *applied = (value != 0) ? 1 : 0;

    return error_code;
}

/* Fire, done ISR: send the samples, and go idle once the last transmission
 * of a finite train is done. */
static void smart_io_pulse_deferred(smart_io_instance_t* instance)
{
    smart_io_pulse_state_t* const state = instance->state.role_state;

    if ((state != NULL) && __atomic_load_n(&state->running, __ATOMIC_ACQUIRE))
    {
        smart_io_pulse_samples(state);

        const uint32_t fires_left = __atomic_load_n(&state->fires_left, __ATOMIC_ACQUIRE);
        const uint32_t transmits  = __atomic_load_n(&state->transmits, __ATOMIC_ACQUIRE);
        const uint32_t done       = __atomic_load_n(&state->done, __ATOMIC_ACQUIRE);

        if ((fires_left == 0u) && (done == transmits))
        {
            __atomic_store_n(&state->running, false, __ATOMIC_RELEASE);
            smart_io_value_changed(instance, 0);
        }
    }
}

/* The stream shows the pin, not the running state: a LOOP train is drawn as
 * one high lane, other modes are low between pulses. */
static esp_err_t smart_io_pulse_stream_start(
    smart_io_instance_t* instance,
    smart_io_ring_t* ring,
    smart_io_stream_source_t* source,
    smart_io_reason_t* reason)
{
    ARG_UNUSED(ring);
    ARG_UNUSED(reason);

    const smart_io_pulse_state_t* const state = instance->state.role_state;

    source->initial =
        ((state->mode == SMART_IO_PULSE_MODE_LOOP) && state->start_sampled) ? 1 : 0;

    return ESP_OK;
}


/* Exposed API ------------------------------------------------------------- */

const smart_io_role_api_t smart_io_pulse_api = {
    .description =
        {
            .name              = "pulse",
            .param_descriptors = smart_io_pulse_params,
            .param_count       = ARRAY_SIZE(smart_io_pulse_params),
            .value_unit        = "active",
            .value_min         = 0,
            .value_max         = 1,
            .role              = SMART_IO_ROLE_PULSE,
            .stream_kind       = SMART_IO_STREAM_KIND_VALUE,
            .pin_functions     = {SMART_IO_FUNCTION_PULSE, SMART_IO_FUNCTION_NONE},
        },
    .validate     = smart_io_pulse_validate,
    .configure    = smart_io_pulse_configure,
    .release      = smart_io_pulse_release,
    .read         = smart_io_pulse_read,
    .write        = smart_io_pulse_write,
    .stream_start = smart_io_pulse_stream_start,
    .stream_stop  = NULL,
    .transfer     = NULL,
    .discover     = NULL,
    .deferred     = smart_io_pulse_deferred,
};

/**** END OF FILE ****/
