/**
 ******************************************************************************
 * @file    smart_io_pwm.c
 * @author  Daniel Dyringer
 * @date    2026-10-31
 * @brief   pwm role.
 ******************************************************************************
 */


/* Includes ---------------------------------------------------------------- */
#include "smart_io_pwm.h"

#include <stdio.h>
#include <stdlib.h>

#include "bsp_smart_io.h"
#include "driver/ledc.h"
#include "esp_timer.h"
#include "smart_io.h"
#include "utils.h"


/* Constants and macros ---------------------------------------------------- */
// clang-format off

#define SMART_IO_PWM_DUTY_PERMILLE_MAX  1000u
#define SMART_IO_PWM_FREQUENCY_MIN_HZ   5u
#define SMART_IO_PWM_FREQUENCY_MAX_HZ   40000000u
#define SMART_IO_PWM_FREQUENCY_HZ       1000u
#define SMART_IO_PWM_MODE               LEDC_LOW_SPEED_MODE
/* LEDC_AUTO_CLK resolves to APB. On the S3 every LEDC timer shares one clock
 * source, and the camera XCLK and LED timers already use LEDC_AUTO_CLK. */
#define SMART_IO_PWM_CLOCK              LEDC_AUTO_CLK
#define SMART_IO_PWM_SOURCE_HZ          80000000u
#define SMART_IO_PWM_RESOLUTION_MAX     14u
#define SMART_IO_PWM_TIMER_COUNT        ((uint32_t)LEDC_TIMER_MAX                  \
                                         - (uint32_t)BSP_SMART_IO_LEDC_TIMER_FIRST)
#define SMART_IO_PWM_CHANNEL_COUNT      ((uint32_t)LEDC_CHANNEL_MAX                \
                                         - (uint32_t)BSP_SMART_IO_LEDC_CHANNEL_FIRST)

// clang-format on

/* Private types ----------------------------------------------------------- */

/* Index in config.param_values. */
typedef enum smart_io_pwm_param_e
{
    SMART_IO_PWM_PARAM_FREQUENCY_HZ = 0,
    SMART_IO_PWM_PARAM_DUTY         = 1,
} smart_io_pwm_param_t;

/* Copied from config.param_values at configure. */
typedef struct smart_io_pwm_params_s
{
    uint32_t frequency_hz;
    uint32_t duty;
} smart_io_pwm_params_t;

/* One LEDC timer, shared by every pin with the same frequency (the resolution
 * follows from it). users 0: free. */
typedef struct smart_io_pwm_timer_s
{
    uint32_t frequency_hz;
    uint32_t users;
} smart_io_pwm_timer_t;

/* instance->state.role_state. */
typedef struct smart_io_pwm_state_s
{
    smart_io_pwm_params_t params;
    uint32_t resolution_bits;
    uint32_t timer;   /* index in smart_io_pwm_timers */
    uint32_t channel; /* index from BSP_SMART_IO_LEDC_CHANNEL_FIRST */
} smart_io_pwm_state_t;


/* Private variables ------------------------------------------------------- */

static const smart_io_param_descriptor_t smart_io_pwm_params[] = {
    [SMART_IO_PWM_PARAM_FREQUENCY_HZ] =
        {
            .name          = "frequency_hz",
            .unit          = "Hz",
            .option_names  = NULL,
            .min           = SMART_IO_PWM_FREQUENCY_MIN_HZ,
            .max           = SMART_IO_PWM_FREQUENCY_MAX_HZ,
            .default_value = SMART_IO_PWM_FREQUENCY_HZ,
            .type          = SMART_IO_PARAM_TYPE_U32,
            .shared        = false,
        },
    [SMART_IO_PWM_PARAM_DUTY] =
        {
            .name          = "duty",
            .unit          = "‰",
            .option_names  = NULL,
            .min           = 0u,
            .max           = SMART_IO_PWM_DUTY_PERMILLE_MAX,
            .default_value = 0u,
            .type          = SMART_IO_PARAM_TYPE_U32,
            .shared        = false,
        },
};

/* Guarded by the smart_io mutex. */
static smart_io_pwm_timer_t smart_io_pwm_timers[SMART_IO_PWM_TIMER_COUNT];
static uint32_t smart_io_pwm_channels_used = 0u;


/* Private functions ------------------------------------------------------- */

static ledc_timer_t smart_io_pwm_timer_id(uint32_t timer)
{
    return (ledc_timer_t)((uint32_t)BSP_SMART_IO_LEDC_TIMER_FIRST + timer);
}

static ledc_channel_t smart_io_pwm_channel_id(uint32_t channel)
{
    return (ledc_channel_t)((uint32_t)BSP_SMART_IO_LEDC_CHANNEL_FIRST + channel);
}

static uint32_t smart_io_pwm_resolution(uint32_t frequency_hz)
{
    const uint32_t bits =
        ledc_find_suitable_duty_resolution(SMART_IO_PWM_SOURCE_HZ, frequency_hz);

    return CLAMP(bits, 1u, SMART_IO_PWM_RESOLUTION_MAX);
}

/* Nearest step; 1000 ‰ = 2^bits, which LEDC treats as always high. */
static uint32_t smart_io_pwm_duty_counts(uint32_t permille, uint32_t resolution_bits)
{
    const uint32_t full = (uint32_t)1u << resolution_bits;

    return ((permille * full) + (SMART_IO_PWM_DUTY_PERMILLE_MAX / 2u))
           / SMART_IO_PWM_DUTY_PERMILLE_MAX;
}

static int32_t smart_io_pwm_duty_permille(uint32_t counts, uint32_t resolution_bits)
{
    const uint32_t full = (uint32_t)1u << resolution_bits;

    return (int32_t)(((counts * SMART_IO_PWM_DUTY_PERMILLE_MAX) + (full / 2u)) / full);
}

static bool smart_io_pwm_channel_take(uint32_t* channel)
{
    bool found = false;

    for (uint32_t i = 0u; (i < SMART_IO_PWM_CHANNEL_COUNT) && !found; i++)
    {
        if ((smart_io_pwm_channels_used & ((uint32_t)1u << i)) == 0u)
        {
            smart_io_pwm_channels_used |= (uint32_t)1u << i;
            *channel = i;
            found    = true;
        }
    }

    return found;
}

static void smart_io_pwm_channel_give(uint32_t channel)
{
    smart_io_pwm_channels_used &= ~((uint32_t)1u << channel);
}

/* A timer already running the frequency is shared, otherwise a free one. */
static bool smart_io_pwm_timer_find(uint32_t frequency_hz, uint32_t* timer)
{
    bool shared = false;
    bool found  = false;

    for (uint32_t i = 0u; (i < SMART_IO_PWM_TIMER_COUNT) && !shared; i++)
    {
        if ((smart_io_pwm_timers[i].users != 0u)
            && (smart_io_pwm_timers[i].frequency_hz == frequency_hz))
        {
            *timer = i;
            shared = true;
        }
    }

    for (uint32_t i = 0u; (i < SMART_IO_PWM_TIMER_COUNT) && !shared && !found; i++)
    {
        if (smart_io_pwm_timers[i].users == 0u)
        {
            *timer = i;
            found  = true;
        }
    }

    return shared || found;
}

static void smart_io_pwm_timers_busy_reason(smart_io_reason_t* reason)
{
    char list[SMART_IO_REASON_SIZE] = {0};
    size_t length                   = 0u;

    for (uint32_t i = 0u; (i < SMART_IO_PWM_TIMER_COUNT) && (length < sizeof(list)); i++)
    {
        const int written = snprintf(
            &list[length],
            sizeof(list) - length,
            "%s%lu Hz",
            (i == 0u) ? "" : ", ",
            (unsigned long)smart_io_pwm_timers[i].frequency_hz);

        length += (written > 0) ? (size_t)written : 0u;
    }

    smart_io_reason_set(reason, "No free LEDC timer (in use: %s)", list);
}

static esp_err_t smart_io_pwm_timer_use(uint32_t timer, uint32_t frequency_hz)
{
    esp_err_t error_code = ESP_OK;

    if (smart_io_pwm_timers[timer].users == 0u)
    {
        const ledc_timer_config_t timer_config = {
            .speed_mode      = SMART_IO_PWM_MODE,
            .duty_resolution = (ledc_timer_bit_t)smart_io_pwm_resolution(frequency_hz),
            .timer_num       = smart_io_pwm_timer_id(timer),
            .freq_hz         = frequency_hz,
            .clk_cfg         = SMART_IO_PWM_CLOCK,
            .deconfigure     = false,
        };

        error_code = ledc_timer_config(&timer_config);
    }

    if (error_code == ESP_OK)
    {
        smart_io_pwm_timers[timer].frequency_hz = frequency_hz;
        smart_io_pwm_timers[timer].users++;
    }

    return error_code;
}

/* The last user pauses and deconfigures it (pause first, IDF requires it). */
static void smart_io_pwm_timer_unuse(uint32_t timer)
{
    smart_io_pwm_timers[timer].users--;

    if (smart_io_pwm_timers[timer].users == 0u)
    {
        const ledc_timer_config_t timer_config = {
            .speed_mode      = SMART_IO_PWM_MODE,
            .duty_resolution = LEDC_TIMER_1_BIT,
            .timer_num       = smart_io_pwm_timer_id(timer),
            .freq_hz         = 0u,
            .clk_cfg         = SMART_IO_PWM_CLOCK,
            .deconfigure     = true,
        };

        ledc_timer_pause(SMART_IO_PWM_MODE, smart_io_pwm_timer_id(timer));
        ledc_timer_config(&timer_config);
        smart_io_pwm_timers[timer].frequency_hz = 0u;
    }
}

static void smart_io_pwm_release(smart_io_instance_t* instance)
{
    smart_io_pwm_state_t* const state = instance->state.role_state;

    if (state != NULL)
    {
        const ledc_channel_config_t channel_config = {
            .gpio_num    = (int)instance->config.gpio[0],
            .speed_mode  = SMART_IO_PWM_MODE,
            .channel     = smart_io_pwm_channel_id(state->channel),
            .timer_sel   = smart_io_pwm_timer_id(state->timer),
            .duty        = 0u,
            .hpoint      = 0,
            .sleep_mode  = LEDC_SLEEP_MODE_NO_ALIVE_NO_PD,
            .flags       = {.output_invert = 0u},
            .deconfigure = true,
        };

        ledc_stop(SMART_IO_PWM_MODE, smart_io_pwm_channel_id(state->channel), 0u);
        ledc_channel_config(&channel_config);
        smart_io_pwm_timer_unuse(state->timer);
        smart_io_pwm_channel_give(state->channel);
        free(state);
        instance->state.role_state = NULL;
    }
}

static esp_err_t smart_io_pwm_hardware_setup(
    smart_io_instance_t* instance,
    smart_io_pwm_state_t* state,
    smart_io_reason_t* reason)
{
    esp_err_t error_code  = ESP_OK;
    const gpio_num_t gpio = instance->config.gpio[0];
    bool channel_taken    = false;
    bool timer_used       = false;

    do
    {
        channel_taken = smart_io_pwm_channel_take(&state->channel);

        if (!channel_taken)
        {
            error_code = ESP_ERR_NOT_FOUND;
            smart_io_reason_set(
                reason,
                "No free LEDC channel (%lu in use)",
                (unsigned long)SMART_IO_PWM_CHANNEL_COUNT);
            break;
        }

        if (!smart_io_pwm_timer_find(state->params.frequency_hz, &state->timer))
        {
            error_code = ESP_ERR_NOT_FOUND;
            smart_io_pwm_timers_busy_reason(reason);
            break;
        }

        error_code = smart_io_pwm_timer_use(state->timer, state->params.frequency_hz);

        if (error_code != ESP_OK)
        {
            smart_io_reason_set(
                reason,
                "LEDC timer for %lu Hz failed (%s)",
                (unsigned long)state->params.frequency_hz,
                esp_err_to_name(error_code));
            break;
        }

        timer_used = true;

        /* The duty is set by the same call that attaches the pin, so the pin
         * never runs at a stale duty. */
        const ledc_channel_config_t channel_config = {
            .gpio_num   = (int)gpio,
            .speed_mode = SMART_IO_PWM_MODE,
            .channel    = smart_io_pwm_channel_id(state->channel),
            .timer_sel  = smart_io_pwm_timer_id(state->timer),
            .duty = smart_io_pwm_duty_counts(state->params.duty, state->resolution_bits),
            .hpoint      = 0,
            .sleep_mode  = LEDC_SLEEP_MODE_NO_ALIVE_NO_PD,
            .flags       = {.output_invert = 0u},
            .deconfigure = false,
        };

        error_code = ledc_channel_config(&channel_config);

        if (error_code != ESP_OK)
        {
            smart_io_reason_set(
                reason,
                "GPIO %d: LEDC channel setup failed (%s)",
                (int)gpio,
                esp_err_to_name(error_code));
        }
    } while (0);

    if ((error_code != ESP_OK) && timer_used)
    {
        smart_io_pwm_timer_unuse(state->timer);
    }

    if ((error_code != ESP_OK) && channel_taken)
    {
        smart_io_pwm_channel_give(state->channel);
    }

    return error_code;
}

static esp_err_t smart_io_pwm_configure(smart_io_instance_t* instance, smart_io_reason_t* reason)
{
    esp_err_t error_code              = ESP_OK;
    smart_io_pwm_state_t* const state = calloc(1u, sizeof(*state));

    do
    {
        if (state == NULL)
        {
            error_code = ESP_ERR_NO_MEM;
            smart_io_reason_set(reason, "Out of memory");
            break;
        }

        state->params.frequency_hz =
            instance->config.param_values[SMART_IO_PWM_PARAM_FREQUENCY_HZ];
        state->params.duty     = instance->config.param_values[SMART_IO_PWM_PARAM_DUTY];
        state->resolution_bits = smart_io_pwm_resolution(state->params.frequency_hz);

        error_code = smart_io_pwm_hardware_setup(instance, state, reason);

        if (error_code != ESP_OK)
        {
            free(state);
            break;
        }

        instance->state.role_state = state;

        const int32_t applied = smart_io_pwm_duty_permille(
            smart_io_pwm_duty_counts(state->params.duty, state->resolution_bits),
            state->resolution_bits);

        smart_io_value_changed(instance, applied);
        smart_io_value_sample(instance, applied, esp_timer_get_time());
    } while (0);

    return error_code;
}

static esp_err_t smart_io_pwm_read(
    smart_io_instance_t* instance,
    int32_t* value,
    smart_io_reason_t* reason)
{
    ARG_UNUSED(reason);

    *value = instance->state.value;

    return ESP_OK;
}

static esp_err_t smart_io_pwm_write(
    smart_io_instance_t* instance,
    int32_t value,
    int32_t* applied,
    smart_io_reason_t* reason)
{
    const smart_io_pwm_state_t* const state = instance->state.role_state;
    const ledc_channel_t channel            = smart_io_pwm_channel_id(state->channel);
    const uint32_t counts =
        smart_io_pwm_duty_counts((uint32_t)value, state->resolution_bits);
    esp_err_t error_code = ledc_set_duty(SMART_IO_PWM_MODE, channel, counts);

    if (error_code == ESP_OK)
    {
        error_code = ledc_update_duty(SMART_IO_PWM_MODE, channel);
    }

    if (error_code == ESP_OK)
    {
        *applied = smart_io_pwm_duty_permille(counts, state->resolution_bits);

        if (*applied != instance->state.value)
        {
            smart_io_value_sample(instance, *applied, esp_timer_get_time());
        }
    }
    else
    {
        smart_io_reason_set(
            reason,
            "GPIO %d: duty write failed (%s)",
            (int)instance->config.gpio[0],
            esp_err_to_name(error_code));
    }

    return error_code;
}


/* Exposed API ------------------------------------------------------------- */

const smart_io_role_api_t smart_io_pwm_api = {
    .description =
        {
            .name              = "pwm",
            .param_descriptors = smart_io_pwm_params,
            .param_count       = ARRAY_SIZE(smart_io_pwm_params),
            .value_unit        = "‰",
            .value_min         = 0,
            .value_max         = (int32_t)SMART_IO_PWM_DUTY_PERMILLE_MAX,
            .role              = SMART_IO_ROLE_PWM,
            .stream_kind       = SMART_IO_STREAM_KIND_VALUE,
            .pin_functions     = {SMART_IO_FUNCTION_PWM, SMART_IO_FUNCTION_NONE},
        },
    .validate     = NULL,
    .configure    = smart_io_pwm_configure,
    .release      = smart_io_pwm_release,
    .read         = smart_io_pwm_read,
    .write        = smart_io_pwm_write,
    .stream_start = NULL,
    .stream_stop  = NULL,
    .transfer     = NULL,
    .discover     = NULL,
    .deferred     = NULL,
};

/**** END OF FILE ****/
