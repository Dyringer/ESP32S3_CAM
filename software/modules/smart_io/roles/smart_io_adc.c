/**
 ******************************************************************************
 * @file    smart_io_adc.c
 * @author  Daniel Dyringer
 * @date    2026-10-31
 * @brief   adc role: oneshot reads, ADC1 continuous streams.
 *
 *          While an ADC1 pin is streamed, the continuous driver converts every
 *          configured ADC1 pin, so their reads come from the stream (oneshot
 *          reads fail while it holds the unit). Values are mV, scaled linearly
 *          from the attenuation's nominal range (no eFuse calibration yet).
 ******************************************************************************
 */


/* Includes ---------------------------------------------------------------- */
#include "smart_io_adc.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "esp_adc/adc_continuous.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_attr.h"
#include "esp_log.h"
#include "smart_io.h"
#include "smart_io_stream_source.h"
#include "soc/soc_caps.h"
#include "utils.h"


/* Constants and macros ---------------------------------------------------- */
// clang-format off

#define SMART_IO_ADC_RATE_MIN_HZ            10u
#define SMART_IO_ADC_RATE_MAX_HZ            50000u
#define SMART_IO_ADC_RATE_HZ                100u
/* Conversions per second over all channels of ADC1. */
#define SMART_IO_ADC_UNIT_MAX_HZ            50000u
#define SMART_IO_ADC_DRIVER_MIN_HZ          SOC_ADC_SAMPLE_FREQ_THRES_LOW
#define SMART_IO_ADC_US_PER_S               1000000u
/* One conversion frame (one ISR) per millisecond. */
#define SMART_IO_ADC_FRAMES_PER_S           1000u
#define SMART_IO_ADC_RAW_MAX                4095u
#define SMART_IO_ADC_SAMPLE_CHANNEL_SHIFT   12u
#define SMART_IO_ADC_POOL_SLOTS             32u

// clang-format on

/* Private types ----------------------------------------------------------- */

/* Index in config.param_values. */
typedef enum smart_io_adc_param_e
{
    SMART_IO_ADC_PARAM_ATTENUATION    = 0,
    SMART_IO_ADC_PARAM_SAMPLE_RATE_HZ = 1,
} smart_io_adc_param_t;

typedef enum smart_io_adc_attenuation_e
{
    SMART_IO_ADC_ATTENUATION_0DB   = 0,
    SMART_IO_ADC_ATTENUATION_2_5DB = 1,
    SMART_IO_ADC_ATTENUATION_6DB   = 2,
    SMART_IO_ADC_ATTENUATION_12DB  = 3,

    SMART_IO_ADC_ATTENUATION_COUNT,
} smart_io_adc_attenuation_t;

/* Copied from config.param_values at configure. */
typedef struct smart_io_adc_params_s
{
    uint32_t attenuation;
    uint32_t sample_rate_hz;
} smart_io_adc_params_t;

/* instance->state.role_state. */
typedef struct smart_io_adc_state_s
{
    smart_io_adc_params_t params;
    adc_unit_t unit;
    adc_channel_t channel;
    int32_t last_mv; /* last oneshot read, -1 before the first */
    uint8_t index;   /* pool index */
} smart_io_adc_state_t;


/* Private variables ------------------------------------------------------- */

static const char* smart_io_adc_tag = "smart_io_adc";

static const char* const smart_io_adc_attenuation_names[] = {
    [SMART_IO_ADC_ATTENUATION_0DB]   = "0",
    [SMART_IO_ADC_ATTENUATION_2_5DB] = "2.5",
    [SMART_IO_ADC_ATTENUATION_6DB]   = "6",
    [SMART_IO_ADC_ATTENUATION_12DB]  = "12",
};

static const adc_atten_t smart_io_adc_attenuations[SMART_IO_ADC_ATTENUATION_COUNT] = {
    [SMART_IO_ADC_ATTENUATION_0DB]   = ADC_ATTEN_DB_0,
    [SMART_IO_ADC_ATTENUATION_2_5DB] = ADC_ATTEN_DB_2_5,
    [SMART_IO_ADC_ATTENUATION_6DB]   = ADC_ATTEN_DB_6,
    [SMART_IO_ADC_ATTENUATION_12DB]  = ADC_ATTEN_DB_12,
};

/* Nominal input range per attenuation (ESP32-S3 datasheet, ADC characteristics). */
static const uint32_t smart_io_adc_full_scale_mv[SMART_IO_ADC_ATTENUATION_COUNT] = {
    [SMART_IO_ADC_ATTENUATION_0DB]   = 950u,
    [SMART_IO_ADC_ATTENUATION_2_5DB] = 1250u,
    [SMART_IO_ADC_ATTENUATION_6DB]   = 1750u,
    [SMART_IO_ADC_ATTENUATION_12DB]  = 3100u,
};

static const smart_io_param_descriptor_t smart_io_adc_params[] = {
    [SMART_IO_ADC_PARAM_ATTENUATION] =
        {
            .name          = "atten",
            .unit          = "dB",
            .option_names  = smart_io_adc_attenuation_names,
            .min           = SMART_IO_ADC_ATTENUATION_0DB,
            .max           = SMART_IO_ADC_ATTENUATION_12DB,
            .default_value = SMART_IO_ADC_ATTENUATION_12DB,
            .type          = SMART_IO_PARAM_TYPE_ENUM,
            .shared        = true,
        },
    [SMART_IO_ADC_PARAM_SAMPLE_RATE_HZ] =
        {
            .name          = "sample_rate_hz",
            .unit          = "Hz",
            .option_names  = NULL,
            .min           = SMART_IO_ADC_RATE_MIN_HZ,
            .max           = SMART_IO_ADC_RATE_MAX_HZ,
            .default_value = SMART_IO_ADC_RATE_HZ,
            .type          = SMART_IO_PARAM_TYPE_U32,
            .shared        = false,
        },
};

/* Everything below is guarded by the smart_io mutex, except where noted. */
static adc_oneshot_unit_handle_t smart_io_adc_units[SOC_ADC_PERIPH_NUM];
static uint32_t smart_io_adc_unit_users[SOC_ADC_PERIPH_NUM];

/* Configured ADC1 pins by pool index: the continuous pattern. */
static smart_io_adc_state_t* smart_io_adc_unit1_pins[SMART_IO_ADC_POOL_SLOTS];
static uint32_t smart_io_adc_streamed                  = 0u; /* pool mask */
static adc_continuous_handle_t smart_io_adc_continuous = NULL;
/* Read by the ISR, set before the driver starts. */
static smart_io_ring_t* smart_io_adc_ring = NULL;


/* Private functions ------------------------------------------------------- */

static int32_t smart_io_adc_millivolts(const smart_io_adc_state_t* state, int32_t raw)
{
    return (int32_t)(((uint32_t)raw * smart_io_adc_full_scale_mv[state->params.attenuation])
                     / SMART_IO_ADC_RAW_MAX);
}

/* ---- Continuous (ADC1) ---- */

/* IRAM, may run with the flash cache off. One conversion frame: every result
 * is pushed, invalid ones too, so the stream can time them by position. */
static bool IRAM_ATTR smart_io_adc_on_conv_done(
    adc_continuous_handle_t handle,
    const adc_continuous_evt_data_t* event,
    void* user_data)
{
    ARG_UNUSED(handle);
    ARG_UNUSED(user_data);

    uint32_t invalid = 0u;

    for (uint32_t offset = 0u; (offset + SOC_ADC_DIGI_RESULT_BYTES) <= event->size;
         offset += SOC_ADC_DIGI_RESULT_BYTES)
    {
        adc_digi_output_data_t result = {0};

        memcpy(&result, &event->conv_frame_buffer[offset], sizeof(result));

        const smart_io_stream_adc_sample_t sample =
            (smart_io_stream_adc_sample_t)(((uint32_t)result.type2.channel
                                            << SMART_IO_ADC_SAMPLE_CHANNEL_SHIFT)
                                           | (uint32_t)result.type2.data);

        if ((result.type2.unit != 0u) || (result.type2.channel >= SOC_ADC_CHANNEL_NUM(0)))
        {
            invalid++;
        }

        smart_io_ring_push(smart_io_adc_ring, &sample, sizeof(sample));
    }

    smart_io_stream_adc_frame_from_isr(
        __atomic_load_n(&smart_io_adc_ring->head, __ATOMIC_RELAXED), invalid);

    return false;
}

static void smart_io_adc_continuous_stop(void)
{
    if (smart_io_adc_continuous != NULL)
    {
        adc_continuous_stop(smart_io_adc_continuous);
        adc_continuous_deinit(smart_io_adc_continuous);
        smart_io_adc_continuous = NULL;
    }
}

static uint32_t smart_io_adc_pattern(adc_digi_pattern_config_t* pattern)
{
    uint32_t length = 0u;

    for (size_t i = 0u; (i < SMART_IO_ADC_POOL_SLOTS) && (length < SOC_ADC_PATT_LEN_MAX);
         i++)
    {
        const smart_io_adc_state_t* const state = smart_io_adc_unit1_pins[i];

        if (state != NULL)
        {
            pattern[length].atten =
                (uint8_t)smart_io_adc_attenuations[state->params.attenuation];
            pattern[length].channel   = (uint8_t)state->channel;
            pattern[length].unit      = (uint8_t)ADC_UNIT_1;
            pattern[length].bit_width = (uint8_t)SOC_ADC_DIGI_MAX_BITWIDTH;
            length++;
        }
    }

    return length;
}

/* Highest rate asked for by a streamed pin, plus extra_hz (a pin about to be
 * streamed). */
static uint32_t smart_io_adc_channel_rate(uint32_t extra_hz)
{
    uint32_t rate_hz = extra_hz;

    for (size_t i = 0u; i < SMART_IO_ADC_POOL_SLOTS; i++)
    {
        const smart_io_adc_state_t* const state = smart_io_adc_unit1_pins[i];

        if ((state != NULL) && ((smart_io_adc_streamed & ((uint32_t)1u << i)) != 0u))
        {
            rate_hz = MAX(rate_hz, state->params.sample_rate_hz);
        }
    }

    return rate_hz;
}

/* (Re)starts the driver on every configured ADC1 pin, or stops it when no
 * ADC1 pin is streamed. Conversions are a whole number of µs apart so the
 * stream can time them exactly; slower pins are decimated by the stream. */
static esp_err_t smart_io_adc_continuous_apply(void)
{
    esp_err_t error_code                                    = ESP_OK;
    adc_digi_pattern_config_t pattern[SOC_ADC_PATT_LEN_MAX] = {0};

    smart_io_adc_continuous_stop();

    const uint32_t length = smart_io_adc_pattern(pattern);

    do
    {
        if ((smart_io_adc_streamed == 0u) || (length == 0u))
        {
            break;
        }

        const uint32_t wanted_hz = CLAMP(
            smart_io_adc_channel_rate(0u) * length,
            SMART_IO_ADC_DRIVER_MIN_HZ,
            SMART_IO_ADC_UNIT_MAX_HZ);
        const uint32_t conversion_us = SMART_IO_ADC_US_PER_S / wanted_hz;
        const uint32_t driver_hz     = SMART_IO_ADC_US_PER_S / conversion_us;
        const uint32_t frame_size    = MAX(1u, driver_hz / SMART_IO_ADC_FRAMES_PER_S)
                                    * SOC_ADC_DIGI_DATA_BYTES_PER_CONV;
        const adc_continuous_handle_cfg_t handle_config = {
            .max_store_buf_size = frame_size,
            .conv_frame_size    = frame_size,
            .flags              = {.flush_pool = 1u},
        };
        const adc_continuous_config_t config = {
            .pattern_num    = length,
            .adc_pattern    = pattern,
            .sample_freq_hz = driver_hz,
            .conv_mode      = ADC_CONV_SINGLE_UNIT_1,
        };
        const adc_continuous_evt_cbs_t callbacks = {
            .on_conv_done = smart_io_adc_on_conv_done,
            .on_pool_ovf  = NULL,
        };

        error_code = adc_continuous_new_handle(&handle_config, &smart_io_adc_continuous);

        if (error_code == ESP_OK)
        {
            error_code = adc_continuous_config(smart_io_adc_continuous, &config);
        }

        if (error_code == ESP_OK)
        {
            error_code = adc_continuous_register_event_callbacks(
                smart_io_adc_continuous, &callbacks, NULL);
        }

        if (error_code == ESP_OK)
        {
            smart_io_stream_adc_timing(conversion_us, length);
            error_code = adc_continuous_start(smart_io_adc_continuous);
        }

        if (error_code != ESP_OK)
        {
            ESP_LOGE(
                smart_io_adc_tag,
                "Continuous start failed: %s",
                esp_err_to_name(error_code));
            smart_io_adc_continuous_stop();
        }
    } while (0);

    return error_code;
}

/* ---- Role ---- */

static esp_err_t smart_io_adc_unit_take(adc_unit_t unit)
{
    esp_err_t error_code = ESP_OK;

    if (smart_io_adc_unit_users[unit] == 0u)
    {
        const adc_oneshot_unit_init_cfg_t unit_config = {
            .unit_id  = unit,
            .clk_src  = 0,
            .ulp_mode = ADC_ULP_MODE_DISABLE,
        };

        error_code = adc_oneshot_new_unit(&unit_config, &smart_io_adc_units[unit]);
    }

    if (error_code == ESP_OK)
    {
        smart_io_adc_unit_users[unit]++;
    }

    return error_code;
}

static void smart_io_adc_unit_give(adc_unit_t unit)
{
    if (smart_io_adc_unit_users[unit] > 0u)
    {
        smart_io_adc_unit_users[unit]--;

        if (smart_io_adc_unit_users[unit] == 0u)
        {
            adc_oneshot_del_unit(smart_io_adc_units[unit]);
            smart_io_adc_units[unit] = NULL;
        }
    }
}

static void smart_io_adc_release(smart_io_instance_t* instance)
{
    smart_io_adc_state_t* const state = instance->state.role_state;

    if (state != NULL)
    {
        smart_io_adc_unit_give(state->unit);

        if ((state->unit == ADC_UNIT_1) && (smart_io_adc_unit1_pins[state->index] == state))
        {
            smart_io_adc_unit1_pins[state->index] = NULL;

            /* Pattern changed: the streams of the other pins go on without it. */
            if (smart_io_adc_continuous != NULL)
            {
                smart_io_adc_continuous_apply();
            }
        }

        free(state);
        instance->state.role_state = NULL;
    }
}

static esp_err_t smart_io_adc_configure(smart_io_instance_t* instance, smart_io_reason_t* reason)
{
    esp_err_t error_code              = ESP_OK;
    const gpio_num_t gpio             = instance->config.gpio[0];
    smart_io_adc_state_t* const state = calloc(1u, sizeof(*state));
    bool unit_taken                   = false;

    instance->state.role_state = state;

    do
    {
        if (state == NULL)
        {
            error_code = ESP_ERR_NO_MEM;
            smart_io_reason_set(reason, "Out of memory");
            break;
        }

        state->params.attenuation =
            instance->config.param_values[SMART_IO_ADC_PARAM_ATTENUATION];
        state->params.sample_rate_hz =
            instance->config.param_values[SMART_IO_ADC_PARAM_SAMPLE_RATE_HZ];
        state->index   = instance->index;
        state->last_mv = -1;

        error_code = adc_oneshot_io_to_channel((int)gpio, &state->unit, &state->channel);

        if (error_code == ESP_OK)
        {
            error_code = smart_io_adc_unit_take(state->unit);
            unit_taken = (error_code == ESP_OK);
        }

        if (error_code == ESP_OK)
        {
            const adc_oneshot_chan_cfg_t channel_config = {
                .atten    = smart_io_adc_attenuations[state->params.attenuation],
                .bitwidth = ADC_BITWIDTH_12,
            };

            error_code = adc_oneshot_config_channel(
                smart_io_adc_units[state->unit], state->channel, &channel_config);
        }

        if (error_code != ESP_OK)
        {
            smart_io_reason_set(
                reason,
                "GPIO %d: ADC setup failed (%s)",
                (int)gpio,
                esp_err_to_name(error_code));
            break;
        }

        if (state->unit == ADC_UNIT_1)
        {
            smart_io_adc_unit1_pins[state->index] = state;

            if (smart_io_adc_continuous != NULL)
            {
                smart_io_adc_continuous_apply();
            }
        }
    } while (0);

    if (error_code != ESP_OK)
    {
        if (unit_taken)
        {
            smart_io_adc_unit_give(state->unit);
        }

        free(state);
        instance->state.role_state = NULL;
    }

    return error_code;
}

static esp_err_t smart_io_adc_read(
    smart_io_instance_t* instance,
    int32_t* value,
    smart_io_reason_t* reason)
{
    esp_err_t error_code              = ESP_OK;
    smart_io_adc_state_t* const state = instance->state.role_state;
    int raw                           = 0;
    int32_t latest                    = 0;
    const bool streaming = (state->unit == ADC_UNIT_1) && (smart_io_adc_continuous != NULL);

    if (streaming && smart_io_stream_adc_latest((uint8_t)state->channel, &latest))
    {
        *value = smart_io_adc_millivolts(state, latest);
    }
    else if (streaming && (state->last_mv >= 0))
    {
        /* No streamed sample yet. */
        *value = state->last_mv;
    }
    else if (streaming)
    {
        error_code = ESP_ERR_TIMEOUT;
    }
    else
    {
        error_code =
            adc_oneshot_read(smart_io_adc_units[state->unit], state->channel, &raw);

        if (error_code == ESP_OK)
        {
            *value         = smart_io_adc_millivolts(state, (int32_t)raw);
            state->last_mv = *value;
        }
    }

    if (error_code != ESP_OK)
    {
        /* ADC2 is shared with WiFi; ADC1 waits for its first streamed sample. */
        smart_io_reason_set(
            reason,
            "GPIO %d: ADC%d busy, try again",
            (int)instance->config.gpio[0],
            (int)state->unit + 1);
    }

    return error_code;
}

static esp_err_t smart_io_adc_stream_start(
    smart_io_instance_t* instance,
    smart_io_ring_t* ring,
    smart_io_stream_source_t* source,
    smart_io_reason_t* reason)
{
    esp_err_t error_code                                    = ESP_OK;
    const smart_io_adc_state_t* const state                 = instance->state.role_state;
    uint32_t length                                         = 0u;
    adc_digi_pattern_config_t pattern[SOC_ADC_PATT_LEN_MAX] = {0};

    length = smart_io_adc_pattern(pattern);

    const uint32_t unit_hz =
        smart_io_adc_channel_rate(state->params.sample_rate_hz) * length;

    if (state->unit != ADC_UNIT_1)
    {
        error_code = ESP_ERR_NOT_SUPPORTED;
        smart_io_reason_set(reason, "ADC2 pins can't be streamed, use an ADC1 pin");
    }
    else if (unit_hz > SMART_IO_ADC_UNIT_MAX_HZ)
    {
        error_code = ESP_ERR_INVALID_ARG;
        smart_io_reason_set(
            reason,
            "ADC1 would convert %lu/s (%lu pins at %lu Hz), max %lu",
            (unsigned long)unit_hz,
            (unsigned long)length,
            (unsigned long)(unit_hz / length),
            (unsigned long)SMART_IO_ADC_UNIT_MAX_HZ);
    }
    else
    {
        source->adc_channel   = (uint8_t)state->channel;
        source->full_scale_mv = smart_io_adc_full_scale_mv[state->params.attenuation];
        source->period_us     = SMART_IO_ADC_US_PER_S / state->params.sample_rate_hz;
        smart_io_adc_ring     = ring;
        smart_io_adc_streamed |= (uint32_t)1u << state->index;

        error_code = smart_io_adc_continuous_apply();

        if (error_code != ESP_OK)
        {
            smart_io_adc_streamed &= ~((uint32_t)1u << state->index);
            smart_io_reason_set(
                reason, "ADC1 start failed (%s)", esp_err_to_name(error_code));
        }
    }

    return error_code;
}

static void smart_io_adc_stream_stop(smart_io_instance_t* instance)
{
    const smart_io_adc_state_t* const state = instance->state.role_state;

    smart_io_adc_streamed &= ~((uint32_t)1u << state->index);
    smart_io_adc_continuous_apply();
}


/* Exposed API ------------------------------------------------------------- */

const smart_io_role_api_t smart_io_adc_api = {
    .description =
        {
            .name              = "adc",
            .param_descriptors = smart_io_adc_params,
            .param_count       = ARRAY_SIZE(smart_io_adc_params),
            .value_unit        = "mV",
            .value_min         = 0,
            .value_max         = 3300,
            .role              = SMART_IO_ROLE_ADC,
            .stream_kind       = SMART_IO_STREAM_KIND_SERIES,
            .pin_functions     = {SMART_IO_FUNCTION_ADC, SMART_IO_FUNCTION_NONE},
        },
    .validate     = NULL,
    .configure    = smart_io_adc_configure,
    .release      = smart_io_adc_release,
    .read         = smart_io_adc_read,
    .write        = NULL,
    .stream_start = smart_io_adc_stream_start,
    .stream_stop  = smart_io_adc_stream_stop,
    .transfer     = NULL,
    .discover     = NULL,
    .deferred     = NULL,
};

/**** END OF FILE ****/
