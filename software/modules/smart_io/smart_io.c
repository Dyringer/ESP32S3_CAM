/**
 ******************************************************************************
 * @file    smart_io.c
 * @author  Daniel Dyringer
 * @date    2026-10-31
 * @brief   Public API, mutex, configure/release flow, bus acquire, events.
 ******************************************************************************
 */


/* Includes ---------------------------------------------------------------- */
#include "smart_io.h"

#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "ota.h"
#include "smart_io_adc.h"
#include "smart_io_digital.h"
#include "smart_io_i2c.h"
#include "smart_io_pad.h"
#include "smart_io_params.h"
#include "smart_io_pulse.h"
#include "smart_io_pwm.h"
#include "smart_io_registry.h"
#include "smart_io_role.h"
#include "smart_io_store.h"
#include "smart_io_stream_source.h"
#include "smart_io_uart.h"
#include "utils.h"


/* Constants and macros ---------------------------------------------------- */
// clang-format off

#define SMART_IO_CHANGED_STREAMS        0x1u
#define SMART_IO_CHANGED_UNSAVED        0x2u

// clang-format on

/* Private types ----------------------------------------------------------- */


/* Private variables ------------------------------------------------------- */

ESP_EVENT_DEFINE_BASE(SMART_IO_EVENTS);

static const char* smart_io_tag = "smart_io";

static SemaphoreHandle_t smart_io_mutex = NULL;

static const smart_io_role_api_t* const smart_io_role_apis[SMART_IO_ROLE_COUNT] = {
    [SMART_IO_ROLE_INPUT]  = &smart_io_digital_input_api,
    [SMART_IO_ROLE_OUTPUT] = &smart_io_digital_output_api,
    [SMART_IO_ROLE_ADC]    = &smart_io_adc_api,
    [SMART_IO_ROLE_PWM]    = &smart_io_pwm_api,
    [SMART_IO_ROLE_PULSE]  = &smart_io_pulse_api,
    [SMART_IO_ROLE_I2C]    = &smart_io_i2c_api,
    [SMART_IO_ROLE_UART]   = &smart_io_uart_api,
};

static const char* const smart_io_function_names[SMART_IO_FUNCTION_COUNT] = {
    [SMART_IO_FUNCTION_NONE]    = "none",
    [SMART_IO_FUNCTION_INPUT]   = "input",
    [SMART_IO_FUNCTION_OUTPUT]  = "output",
    [SMART_IO_FUNCTION_ADC]     = "adc",
    [SMART_IO_FUNCTION_PWM]     = "pwm",
    [SMART_IO_FUNCTION_PULSE]   = "pulse",
    [SMART_IO_FUNCTION_I2C_SDA] = "i2c_sda",
    [SMART_IO_FUNCTION_I2C_SCL] = "i2c_scl",
    [SMART_IO_FUNCTION_UART_TX] = "uart_tx",
    [SMART_IO_FUNCTION_UART_RX] = "uart_rx",
};

/* Built once at init from smart_io_role_apis; read-only after. */
static smart_io_role_info_t smart_io_role_infos[SMART_IO_ROLE_COUNT];

/* VALUE ring, from the first VALUE stream until the last client detaches.
 * Guarded by the mutex: cached so a sample never waits on the stream lock. */
static smart_io_ring_t* smart_io_value_ring = NULL;

/* Guarded by the mutex. Changes are collected while it is held and posted by
 * smart_io_unlock() once it is released. */
static uint32_t smart_io_pending_values  = 0u;
static uint32_t smart_io_pending_configs = 0u;
static bool smart_io_pending_streams     = false;
static bool smart_io_is_unsaved          = false;

/* Changes not taken yet: ORed in by smart_io_unlock(), taken by the consumer
 * without the mutex, so atomic. posted: a CHANGED event is out and not taken
 * yet, so a burst of changes costs one post. */
static uint32_t smart_io_changed_values  = 0u;
static uint32_t smart_io_changed_configs = 0u;
static uint32_t smart_io_changed_flags   = 0u;
static bool smart_io_changed_posted      = false;

/* Set from sys_evt (atomic): an OTA is running, stream is refused. The streams
 * are stopped by smart_io_ota_work, since sys_evt never takes the mutex. */
static bool smart_io_ota_active            = false;
static work_queue_item_t smart_io_ota_work = {0};

/* Last saved or applied configuration; unsaved = the instances differ from it.
 * Guarded by the mutex. */
static smart_io_config_t smart_io_saved[SMART_IO_MAX_PINS];
static size_t smart_io_saved_count = 0u;


/* Private functions ------------------------------------------------------- */

/* ---- Roles ---- */

/* NULL for SMART_IO_ROLE_NONE and out of range. */
static const smart_io_role_api_t* smart_io_role_api_get(smart_io_role_t role)
{
    const smart_io_role_api_t* role_api = NULL;

    if ((uint32_t)role < (uint32_t)SMART_IO_ROLE_COUNT)
    {
        role_api = smart_io_role_apis[role];
    }

    return role_api;
}

static void smart_io_role_infos_build(void)
{
    for (size_t i = 0u; i < (size_t)SMART_IO_ROLE_COUNT; i++)
    {
        const smart_io_role_api_t* const role_api = smart_io_role_apis[i];

        if (role_api != NULL)
        {
            smart_io_role_infos[i].description = &role_api->description;
            smart_io_role_infos[i].writable    = (role_api->write != NULL);
            smart_io_role_infos[i].transfers   = (role_api->transfer != NULL);
            smart_io_role_infos[i].discovers   = (role_api->discover != NULL);
        }
    }
}

/* ---- Events and locking ---- */

/* A failed post keeps the changes; the next change posts again. */
static void smart_io_changes_notify(void)
{
    if (!__atomic_exchange_n(&smart_io_changed_posted, true, __ATOMIC_SEQ_CST))
    {
        const esp_err_t error_code =
            esp_event_post(SMART_IO_EVENTS, (int32_t)SMART_IO_EVENT_CHANGED, NULL, 0u, 0);

        if (error_code != ESP_OK)
        {
            __atomic_store_n(&smart_io_changed_posted, false, __ATOMIC_SEQ_CST);
            ESP_LOGD(smart_io_tag, "CHANGED not posted: %s", esp_err_to_name(error_code));
        }
    }
}

static bool smart_io_config_is_saved(const smart_io_config_t* config)
{
    bool found = false;

    for (size_t i = 0u; (i < smart_io_saved_count) && !found; i++)
    {
        found = (memcmp(&smart_io_saved[i], config, sizeof(smart_io_config_t)) == 0);
    }

    return found;
}

/* Configs are normalised at configure, so equal settings compare equal. */
static bool smart_io_unsaved_compute(void)
{
    size_t count                         = 0u;
    smart_io_instance_t* const instances = smart_io_registry_instances(&count);
    size_t configured                    = 0u;
    bool unsaved                         = false;

    for (size_t i = 0u; i < count; i++)
    {
        if (instances[i].role_api != NULL)
        {
            configured++;
            unsaved = unsaved || !smart_io_config_is_saved(&instances[i].config);
        }
    }

    return unsaved || (configured != smart_io_saved_count);
}

static void smart_io_lock(void)
{
    xSemaphoreTake(smart_io_mutex, portMAX_DELAY);
}

static void smart_io_unlock(void)
{
    const uint32_t values  = smart_io_pending_values;
    const uint32_t configs = smart_io_pending_configs;
    const bool unsaved     = smart_io_unsaved_compute();
    uint32_t flags         = 0u;

    if (smart_io_pending_streams)
    {
        flags |= SMART_IO_CHANGED_STREAMS;
    }

    if (unsaved != smart_io_is_unsaved)
    {
        flags |= SMART_IO_CHANGED_UNSAVED;
    }

    smart_io_pending_values  = 0u;
    smart_io_pending_configs = 0u;
    smart_io_pending_streams = false;
    smart_io_is_unsaved      = unsaved;

    xSemaphoreGive(smart_io_mutex);

    if ((values != 0u) || (configs != 0u) || (flags != 0u))
    {
        __atomic_fetch_or(&smart_io_changed_values, values, __ATOMIC_SEQ_CST);
        __atomic_fetch_or(&smart_io_changed_configs, configs, __ATOMIC_SEQ_CST);
        __atomic_fetch_or(&smart_io_changed_flags, flags, __ATOMIC_SEQ_CST);
        smart_io_changes_notify();
    }
}

/* ---- Pins ---- */

static uint32_t smart_io_config_mask(const smart_io_config_t* config)
{
    uint32_t mask = 0u;

    for (size_t i = 0u; i < SMART_IO_BUS_PIN_COUNT; i++)
    {
        uint8_t index = 0u;

        if (smart_io_registry_pin_index(config->gpio[i], &index) == ESP_OK)
        {
            mask |= (uint32_t)1u << index;
        }
    }

    return mask;
}

static size_t smart_io_pin_count(const smart_io_role_api_t* role_api)
{
    return (role_api->description.pin_functions[1] != SMART_IO_FUNCTION_NONE) ? 2u : 1u;
}

/* ---- Instances ---- */

static void smart_io_deferred(void* context)
{
    smart_io_instance_t* const instance = context;

    smart_io_lock();

    if ((instance->role_api != NULL) && (instance->role_api->deferred != NULL))
    {
        instance->role_api->deferred(instance);
    }

    smart_io_unlock();
}

/* ---- Streams ---- */

static bool smart_io_instance_streamed(const smart_io_instance_t* instance)
{
    return (smart_io_stream_mask() & ((uint32_t)1u << instance->index)) != 0u;
}

/* Mutex held: the VALUE ring has one producer, whoever holds the mutex. */
static void smart_io_value_push(uint8_t index, int32_t value, int64_t time_us)
{
    if (smart_io_value_ring != NULL)
    {
        const smart_io_stream_value_sample_t sample = {
            .time_us = (uint32_t)time_us,
            .value   = value,
            .index   = index,
        };

        smart_io_ring_push(smart_io_value_ring, &sample, sizeof(sample));
    }
}

static void smart_io_instance_stream_end(smart_io_instance_t* instance)
{
    if (smart_io_instance_streamed(instance))
    {
        smart_io_stream_stop(instance->index);

        if (instance->role_api->stream_stop != NULL)
        {
            instance->role_api->stream_stop(instance);
        }

        smart_io_pending_streams = true;
    }
}

static esp_err_t smart_io_instance_stream_begin(
    smart_io_instance_t* instance,
    smart_io_reason_t* reason)
{
    esp_err_t error_code                      = ESP_OK;
    const smart_io_role_api_t* const role_api = instance->role_api;
    const smart_io_stream_kind_t kind         = role_api->description.stream_kind;
    smart_io_stream_source_t source           = {0};

    do
    {
        smart_io_ring_t* ring = NULL;

        error_code = smart_io_stream_ring(kind, &ring);

        if (error_code != ESP_OK)
        {
            smart_io_reason_set(reason, "Out of memory");
            break;
        }

        if (kind == SMART_IO_STREAM_KIND_VALUE)
        {
            smart_io_value_ring = ring;
        }

        source.since_us = esp_timer_get_time();
        source.initial  = instance->state.value;

        if (role_api->stream_start != NULL)
        {
            error_code = role_api->stream_start(instance, ring, &source, reason);

            if (error_code != ESP_OK)
            {
                break;
            }
        }

        error_code = smart_io_stream_start(instance->index, kind, &source);

        if (error_code != ESP_OK)
        {
            if (role_api->stream_stop != NULL)
            {
                role_api->stream_stop(instance);
            }

            smart_io_reason_set(reason, "Stream start failed");
            break;
        }

        /* The starting point; later samples come from smart_io_value_sample(). */
        if ((kind == SMART_IO_STREAM_KIND_VALUE) && instance->state.value_valid)
        {
            smart_io_value_push(instance->index, source.initial, source.since_us);
        }

        smart_io_pending_streams = true;
    } while (0);

    return error_code;
}

static void smart_io_instances_unstream(void)
{
    size_t count                         = 0u;
    smart_io_instance_t* const instances = smart_io_registry_instances(&count);

    for (size_t i = 0u; i < count; i++)
    {
        if (instances[i].role_api != NULL)
        {
            smart_io_instance_stream_end(&instances[i]);
        }
    }
}

static void smart_io_on_ota_work(void* context)
{
    ARG_UNUSED(context);

    smart_io_lock();
    smart_io_instances_unstream();
    smart_io_unlock();
}

static void smart_io_on_ota_event(
    void* handler_arg,
    esp_event_base_t base,
    int32_t event_id,
    void* event_data)
{
    ARG_UNUSED(handler_arg);
    ARG_UNUSED(base);
    ARG_UNUSED(event_data);

    if (event_id == (int32_t)OTA_EVENT_PROGRESS)
    {
        if (!__atomic_exchange_n(&smart_io_ota_active, true, __ATOMIC_SEQ_CST))
        {
            work_queue_push(&smart_io_ota_work);
        }
    }
    else
    {
        __atomic_store_n(&smart_io_ota_active, false, __ATOMIC_SEQ_CST);
    }
}

/* ---- Release ---- */

static void smart_io_instance_release(smart_io_instance_t* instance)
{
    const smart_io_config_t config = instance->config;

    smart_io_instance_stream_end(instance);
    instance->role_api->release(instance);
    work_queue_cancel(&instance->work);
    smart_io_pending_configs |= smart_io_config_mask(&config);
    smart_io_registry_free(instance);

    for (size_t i = 0u; i < SMART_IO_BUS_PIN_COUNT; i++)
    {
        smart_io_pad_park(config.gpio[i]);
    }
}

static void smart_io_release_all(void)
{
    size_t count                         = 0u;
    smart_io_instance_t* const instances = smart_io_registry_instances(&count);

    for (size_t i = 0u; i < count; i++)
    {
        if (instances[i].role_api != NULL)
        {
            smart_io_instance_release(&instances[i]);
        }
    }
}

static size_t smart_io_collect(smart_io_config_t* configs)
{
    size_t count                         = 0u;
    size_t collected                     = 0u;
    smart_io_instance_t* const instances = smart_io_registry_instances(&count);

    for (size_t i = 0u; i < count; i++)
    {
        if (instances[i].role_api != NULL)
        {
            configs[collected] = instances[i].config;
            collected++;
        }
    }

    return collected;
}

/* ---- Configure ---- */

/* Letters, digits, '_' and '-', not only digits (the shell reads those as a
 * GPIO number). Empty means no label. */
static bool smart_io_label_valid(const char* label)
{
    bool valid          = true;
    bool has_letter     = false;
    const size_t length = strnlen(label, SMART_IO_LABEL_SIZE);

    for (size_t i = 0u; (i < length) && valid; i++)
    {
        const char character = label[i];
        const bool is_digit  = (character >= '0') && (character <= '9');
        const bool is_letter = ((character >= 'a') && (character <= 'z'))
                               || ((character >= 'A') && (character <= 'Z'))
                               || (character == '_') || (character == '-');

        valid      = is_digit || is_letter;
        has_letter = has_letter || is_letter;
    }

    return valid && (length < SMART_IO_LABEL_SIZE) && ((length == 0u) || has_letter);
}

static esp_err_t smart_io_pins_check(
    const smart_io_config_t* config,
    const smart_io_role_api_t* role_api,
    smart_io_reason_t* reason)
{
    esp_err_t error_code             = ESP_OK;
    size_t pool_count                = 0u;
    const smart_io_pin_t* const pool = smart_io_registry_pins(&pool_count);
    const size_t pin_count           = smart_io_pin_count(role_api);

    for (size_t i = 0u; (i < pin_count) && (error_code == ESP_OK); i++)
    {
        const gpio_num_t gpio              = config->gpio[i];
        const smart_io_function_t function = role_api->description.pin_functions[i];
        uint8_t index                      = 0u;

        if (smart_io_registry_pin_index(gpio, &index) != ESP_OK)
        {
            error_code = ESP_ERR_INVALID_ARG;
            smart_io_reason_set(reason, "GPIO %d is not a smart_io pin", (int)gpio);
        }
        else if ((pool[index].caps & SMART_IO_CAP(function)) == 0u)
        {
            error_code = ESP_ERR_NOT_SUPPORTED;
            smart_io_reason_set(
                reason, "GPIO %d can't be %s", (int)gpio, smart_io_function_name(function));
        }
        else
        {
            /* Pin fits the function. */
        }
    }

    if ((error_code == ESP_OK) && (pin_count == 2u) && (config->gpio[0] == config->gpio[1]))
    {
        error_code = ESP_ERR_INVALID_ARG;
        smart_io_reason_set(
            reason, "%s needs two different pins", role_api->description.name);
    }

    if ((error_code == ESP_OK) && (pin_count == 1u) && (config->gpio[1] != GPIO_NUM_NC))
    {
        error_code = ESP_ERR_INVALID_ARG;
        smart_io_reason_set(reason, "%s uses one pin", role_api->description.name);
    }

    return error_code;
}

/* Checks without touching hardware or instances; output is the normalised
 * copy, with unused params and label bytes zeroed so configs compare with
 * memcmp. */
static esp_err_t smart_io_config_prepare(
    const smart_io_config_t* config,
    smart_io_config_t* normalised,
    const smart_io_role_api_t** role_api,
    smart_io_reason_t* reason)
{
    esp_err_t error_code = ESP_OK;

    do
    {
        *role_api = smart_io_role_api_get(config->role);

        if (*role_api == NULL)
        {
            error_code = ESP_ERR_NOT_SUPPORTED;
            smart_io_reason_set(reason, "Unknown role %d", (int)config->role);
            break;
        }

        error_code = smart_io_pins_check(config, *role_api, reason);

        if (error_code != ESP_OK)
        {
            break;
        }

        if (!smart_io_label_valid(config->label))
        {
            error_code = ESP_ERR_INVALID_ARG;
            smart_io_reason_set(
                reason,
                "Label: up to %u of A-Z a-z 0-9 _ -, not only digits",
                (unsigned)(SMART_IO_LABEL_SIZE - 1u));
            break;
        }

        memset(normalised, 0, sizeof(smart_io_config_t));
        normalised->role    = config->role;
        normalised->gpio[0] = config->gpio[0];
        normalised->gpio[1] =
            (smart_io_pin_count(*role_api) == 2u) ? config->gpio[1] : GPIO_NUM_NC;
        strlcpy(normalised->label, config->label, sizeof(normalised->label));
        memcpy(
            normalised->param_values,
            config->param_values,
            (*role_api)->description.param_count * sizeof(uint32_t));

        error_code = smart_io_params_check(
            &(*role_api)->description, normalised->param_values, reason);

        if ((error_code == ESP_OK) && ((*role_api)->validate != NULL))
        {
            error_code = (*role_api)->validate(normalised, reason);
        }
    } while (0);

    return error_code;
}

/* Releases whatever uses the pins, then sets up the new instance. Hardware
 * failure leaves the pins released. */
static esp_err_t smart_io_configure_locked(
    const smart_io_config_t* config,
    smart_io_reason_t* reason)
{
    esp_err_t error_code                = ESP_OK;
    smart_io_config_t normalised        = {0};
    const smart_io_role_api_t* role_api = NULL;

    do
    {
        error_code = smart_io_config_prepare(config, &normalised, &role_api, reason);

        if (error_code != ESP_OK)
        {
            break;
        }

        smart_io_instance_t* const first = smart_io_registry_find_gpio(normalised.gpio[0]);
        smart_io_instance_t* const second = smart_io_registry_find_gpio(normalised.gpio[1]);
        const smart_io_instance_t* const owner =
            smart_io_registry_find_label(normalised.label);

        if (((first != NULL) && first->state.busy)
            || ((second != NULL) && second->state.busy))
        {
            error_code = ESP_ERR_INVALID_STATE;
            smart_io_reason_set(reason, "Bus busy, try again");
            break;
        }

        if ((owner != NULL) && (owner != first) && (owner != second))
        {
            error_code = ESP_ERR_INVALID_ARG;
            smart_io_reason_set(
                reason,
                "Label %s is used by GPIO %d",
                normalised.label,
                (int)owner->config.gpio[0]);
            break;
        }

        if (first != NULL)
        {
            smart_io_instance_release(first);
        }

        if ((second != NULL) && (second != first))
        {
            smart_io_instance_release(second);
        }

        smart_io_instance_t* const instance = smart_io_registry_alloc();

        if (instance == NULL)
        {
            error_code = ESP_ERR_NO_MEM;
            smart_io_reason_set(reason, "No free slot");
            break;
        }

        /* gpio[0] is a pool pin: checked with the pin caps. */
        smart_io_registry_pin_index(normalised.gpio[0], &instance->index);
        instance->config   = normalised;
        instance->role_api = role_api;
        smart_io_pending_configs |= smart_io_config_mask(&normalised);

        error_code = role_api->configure(instance, reason);

        if (error_code != ESP_OK)
        {
            smart_io_registry_free(instance);
            smart_io_pad_park(normalised.gpio[0]);
            smart_io_pad_park(normalised.gpio[1]);
        }
    } while (0);

    return error_code;
}

/* Shared params follow the pin configured last: every other pin of the role is
 * configured again with that value. A pin that fails it ends up released, as
 * any failed configure does. */
static void smart_io_params_sync(const smart_io_config_t* config)
{
    const smart_io_role_api_t* const role_api = smart_io_role_api_get(config->role);
    size_t count                              = 0u;
    smart_io_instance_t* const instances      = smart_io_registry_instances(&count);

    for (size_t i = 0u; (role_api != NULL) && (i < count); i++)
    {
        smart_io_config_t synced = instances[i].config;
        bool changed             = false;

        for (size_t j = 0u; (instances[i].role_api == role_api)
                            && (j < role_api->description.param_count);
             j++)
        {
            if (role_api->description.param_descriptors[j].shared
                && (synced.param_values[j] != config->param_values[j]))
            {
                synced.param_values[j] = config->param_values[j];
                changed                = true;
            }
        }

        if (changed)
        {
            smart_io_reason_t reason   = {0};
            const esp_err_t error_code = smart_io_configure_locked(&synced, &reason);

            if (error_code != ESP_OK)
            {
                ESP_LOGW(
                    smart_io_tag,
                    "GPIO %d: shared %s setting not applied: %s",
                    (int)synced.gpio[0],
                    role_api->description.name,
                    reason.text);
            }
        }
    }
}

static bool smart_io_any_busy(void)
{
    size_t count                               = 0u;
    const smart_io_instance_t* const instances = smart_io_registry_instances(&count);
    bool busy                                  = false;

    for (size_t i = 0u; (i < count) && !busy; i++)
    {
        busy = (instances[i].role_api != NULL) && instances[i].state.busy;
    }

    return busy;
}

static void smart_io_apply_locked(const smart_io_config_t* configs, size_t count)
{
    smart_io_release_all();
    memcpy(smart_io_saved, configs, count * sizeof(smart_io_config_t));
    smart_io_saved_count = count;

    for (size_t i = 0u; i < count; i++)
    {
        smart_io_reason_t reason        = {0};
        const esp_err_t configure_error = smart_io_configure_locked(&configs[i], &reason);

        if (configure_error != ESP_OK)
        {
            ESP_LOGW(
                smart_io_tag,
                "Saved GPIO %d dropped: %s (%s)",
                (int)configs[i].gpio[0],
                reason.text,
                esp_err_to_name(configure_error));
        }
    }
}

/* An unusable blob leaves the current configuration alone; nothing saved
 * releases everything. A bus in use refuses it: its role runs without the
 * mutex. */
static esp_err_t smart_io_apply_saved(smart_io_reason_t* reason)
{
    esp_err_t error_code = ESP_OK;
    size_t count         = 0u;
    smart_io_config_t* const configs = calloc(SMART_IO_MAX_PINS, sizeof(smart_io_config_t));

    do
    {
        if (configs == NULL)
        {
            error_code = ESP_ERR_NO_MEM;
            break;
        }

        error_code = smart_io_store_load(configs, SMART_IO_MAX_PINS, &count);

        if (error_code == ESP_ERR_NOT_FOUND)
        {
            error_code = ESP_OK;
        }

        if (error_code != ESP_OK)
        {
            ESP_LOGW(
                smart_io_tag,
                "Saved configuration not applied: %s",
                esp_err_to_name(error_code));
            break;
        }

        smart_io_lock();

        if (smart_io_any_busy())
        {
            error_code = ESP_ERR_INVALID_STATE;
            smart_io_reason_set(reason, "Bus busy, try again");
        }
        else
        {
            smart_io_apply_locked(configs, count);
        }

        smart_io_unlock();
    } while (0);

    free(configs);

    return error_code;
}

/* NULL when gpio has no instance; reason is set. */
static smart_io_instance_t* smart_io_instance_get(gpio_num_t gpio, smart_io_reason_t* reason)
{
    smart_io_instance_t* const instance = smart_io_registry_find_gpio(gpio);

    if (instance == NULL)
    {
        smart_io_reason_set(reason, "GPIO %d is not configured", (int)gpio);
    }

    return instance;
}

/* ---- Buses ---- */

/* Marks the bus busy, so configure, release and revert keep off it while its
 * role runs without the mutex. Every success needs smart_io_bus_release(). */
static esp_err_t smart_io_bus_acquire(
    gpio_num_t gpio,
    bool discover,
    smart_io_instance_t** acquired,
    smart_io_reason_t* reason)
{
    esp_err_t error_code = ESP_OK;

    smart_io_lock();

    smart_io_instance_t* const instance = smart_io_instance_get(gpio, reason);

    if (instance == NULL)
    {
        error_code = ESP_ERR_NOT_FOUND;
    }
    else if (
        (discover && (instance->role_api->discover == NULL))
        || (!discover && (instance->role_api->transfer == NULL)))
    {
        error_code = ESP_ERR_NOT_SUPPORTED;
        smart_io_reason_set(
            reason,
            "%s has no %s",
            instance->role_api->description.name,
            discover ? "discover" : "transfer");
    }
    else if (instance->state.busy)
    {
        error_code = ESP_ERR_INVALID_STATE;
        smart_io_reason_set(reason, "Bus busy, try again");
    }
    else
    {
        instance->state.busy = true;
        *acquired            = instance;
    }

    smart_io_unlock();

    return error_code;
}

static void smart_io_bus_release(smart_io_instance_t* instance)
{
    smart_io_lock();
    instance->state.busy = false;
    smart_io_unlock();
}

static esp_err_t smart_io_transfers_check(
    const smart_io_transfer_t* transfers,
    size_t count,
    size_t* failed,
    smart_io_reason_t* reason)
{
    esp_err_t error_code = ESP_OK;

    if ((count == 0u) || (count > SMART_IO_TRANSFER_COUNT_MAX))
    {
        error_code = ESP_ERR_INVALID_ARG;
        smart_io_reason_set(
            reason, "1-%u transfers per request", (unsigned)SMART_IO_TRANSFER_COUNT_MAX);
    }

    for (size_t i = 0u; (i < count) && (error_code == ESP_OK); i++)
    {
        if ((transfers[i].tx_length > SMART_IO_TRANSFER_DATA_MAX)
            || (transfers[i].rx_length > SMART_IO_TRANSFER_DATA_MAX))
        {
            error_code = ESP_ERR_INVALID_SIZE;
            *failed    = i;
            smart_io_reason_set(
                reason,
                "At most %u bytes per direction",
                (unsigned)SMART_IO_TRANSFER_DATA_MAX);
        }
    }

    return error_code;
}


/* Exposed API ------------------------------------------------------------- */

esp_err_t smart_io_init(void)
{
    esp_err_t error_code = ESP_OK;

    smart_io_role_infos_build();

    do
    {
        smart_io_mutex = xSemaphoreCreateMutex();

        if (smart_io_mutex == NULL)
        {
            error_code = ESP_ERR_NO_MEM;
            break;
        }

        error_code = smart_io_registry_init();

        if (error_code != ESP_OK)
        {
            ESP_LOGE(smart_io_tag, "Pin pool lists a GPIO twice");
            break;
        }

        size_t count                         = 0u;
        smart_io_instance_t* const instances = smart_io_registry_instances(&count);
        const smart_io_pin_t* const pins     = smart_io_registry_pins(NULL);

        for (size_t i = 0u; (i < count) && (error_code == ESP_OK); i++)
        {
            error_code =
                work_queue_item_init(&instances[i].work, smart_io_deferred, &instances[i]);
            smart_io_pad_park(pins[i].gpio);
        }

        if (error_code != ESP_OK)
        {
            break;
        }

        error_code = smart_io_stream_init();

        if (error_code != ESP_OK)
        {
            break;
        }

        error_code = work_queue_item_init(&smart_io_ota_work, smart_io_on_ota_work, NULL);

        if (error_code != ESP_OK)
        {
            break;
        }

        error_code = esp_event_handler_register(
            OTA_EVENTS, ESP_EVENT_ANY_ID, smart_io_on_ota_event, NULL);

        if (error_code != ESP_OK)
        {
            break;
        }

        error_code = smart_io_digital_init();

        if (error_code != ESP_OK)
        {
            ESP_LOGE(
                smart_io_tag,
                "GPIO interrupt service failed: %s",
                esp_err_to_name(error_code));
            break;
        }

        error_code = smart_io_store_init();

        if (error_code != ESP_OK)
        {
            break;
        }

        smart_io_apply_saved(NULL);
    } while (0);

    if (error_code != ESP_OK)
    {
        ESP_LOGE(smart_io_tag, "Init failed: %s", esp_err_to_name(error_code));
    }

    return error_code;
}

void smart_io_reason_set(smart_io_reason_t* reason, const char* fmt, ...)
{
    if (reason != NULL)
    {
        va_list args;

        va_start(args, fmt);
        vsnprintf(reason->text, sizeof(reason->text), fmt, args);
        va_end(args);
    }
}

/* ---- Pins and roles ---- */

const smart_io_pin_t* smart_io_pins(size_t* count)
{
    return smart_io_registry_pins(count);
}

esp_err_t smart_io_lookup(const char* label, gpio_num_t* gpio)
{
    esp_err_t error_code = ESP_ERR_NOT_FOUND;

    smart_io_lock();

    const smart_io_instance_t* const instance = smart_io_registry_find_label(label);

    if (instance != NULL)
    {
        *gpio      = instance->config.gpio[0];
        error_code = ESP_OK;
    }

    smart_io_unlock();

    return error_code;
}

const smart_io_role_info_t* smart_io_role_info(smart_io_role_t role)
{
    const smart_io_role_info_t* info = NULL;

    if (((uint32_t)role < (uint32_t)SMART_IO_ROLE_COUNT)
        && (smart_io_role_infos[role].description != NULL))
    {
        info = &smart_io_role_infos[role];
    }

    return info;
}

esp_err_t smart_io_role_find(const char* name, smart_io_role_t* role)
{
    esp_err_t error_code = ESP_ERR_NOT_FOUND;

    for (size_t i = 0u; (i < (size_t)SMART_IO_ROLE_COUNT) && (error_code != ESP_OK); i++)
    {
        const smart_io_role_api_t* const role_api = smart_io_role_apis[i];

        if ((role_api != NULL) && (strcmp(role_api->description.name, name) == 0))
        {
            *role      = role_api->description.role;
            error_code = ESP_OK;
        }
    }

    return error_code;
}

const char* smart_io_function_name(smart_io_function_t function)
{
    const char* name = "unknown";

    if ((uint32_t)function < (uint32_t)SMART_IO_FUNCTION_COUNT)
    {
        name = smart_io_function_names[function];
    }

    return name;
}

/* ---- Configuration ---- */

esp_err_t smart_io_config_get(gpio_num_t gpio, smart_io_config_t* config)
{
    esp_err_t error_code = ESP_ERR_NOT_FOUND;

    smart_io_lock();

    const smart_io_instance_t* const instance = smart_io_registry_find_gpio(gpio);

    if (instance != NULL)
    {
        *config    = instance->config;
        error_code = ESP_OK;
    }

    smart_io_unlock();

    return error_code;
}

esp_err_t smart_io_configure(const smart_io_config_t* config, smart_io_reason_t* reason)
{
    smart_io_lock();

    const esp_err_t error_code = smart_io_configure_locked(config, reason);

    if (error_code == ESP_OK)
    {
        smart_io_params_sync(config);
    }

    smart_io_unlock();

    return error_code;
}

esp_err_t smart_io_release(gpio_num_t gpio, smart_io_reason_t* reason)
{
    esp_err_t error_code = ESP_OK;

    smart_io_lock();

    smart_io_instance_t* const instance = smart_io_instance_get(gpio, reason);

    if (instance == NULL)
    {
        error_code = ESP_ERR_NOT_FOUND;
    }
    else if (instance->state.busy)
    {
        error_code = ESP_ERR_INVALID_STATE;
        smart_io_reason_set(reason, "Bus busy, try again");
    }
    else
    {
        smart_io_instance_release(instance);
    }

    smart_io_unlock();

    return error_code;
}

esp_err_t smart_io_save(void)
{
    esp_err_t error_code = ESP_OK;
    smart_io_config_t* const configs = calloc(SMART_IO_MAX_PINS, sizeof(smart_io_config_t));

    do
    {
        if (configs == NULL)
        {
            error_code = ESP_ERR_NO_MEM;
            break;
        }

        smart_io_lock();
        const size_t count = smart_io_collect(configs);
        smart_io_unlock();

        error_code = smart_io_store_save(configs, count);

        if (error_code != ESP_OK)
        {
            break;
        }

        smart_io_lock();
        memcpy(smart_io_saved, configs, count * sizeof(smart_io_config_t));
        smart_io_saved_count = count;
        smart_io_unlock();
    } while (0);

    free(configs);

    return error_code;
}

esp_err_t smart_io_revert(smart_io_reason_t* reason)
{
    return smart_io_apply_saved(reason);
}

bool smart_io_unsaved(void)
{
    smart_io_lock();

    const bool unsaved = smart_io_is_unsaved;

    smart_io_unlock();

    return unsaved;
}

/* ---- Values ---- */

esp_err_t smart_io_read(gpio_num_t gpio, int32_t* value, smart_io_reason_t* reason)
{
    esp_err_t error_code = ESP_OK;

    smart_io_lock();

    smart_io_instance_t* const instance = smart_io_instance_get(gpio, reason);

    if (instance == NULL)
    {
        error_code = ESP_ERR_NOT_FOUND;
    }
    else if (instance->role_api->read == NULL)
    {
        error_code = ESP_ERR_NOT_SUPPORTED;
        smart_io_reason_set(
            reason, "%s has no value to read", instance->role_api->description.name);
    }
    else
    {
        error_code = instance->role_api->read(instance, value, reason);
    }

    smart_io_unlock();

    return error_code;
}

esp_err_t smart_io_write(gpio_num_t gpio, int32_t value, int32_t* applied, smart_io_reason_t* reason)
{
    esp_err_t error_code  = ESP_OK;
    int32_t applied_value = value;

    smart_io_lock();

    smart_io_instance_t* const instance = smart_io_instance_get(gpio, reason);

    if (instance == NULL)
    {
        error_code = ESP_ERR_NOT_FOUND;
    }
    else if (instance->role_api->write == NULL)
    {
        error_code = ESP_ERR_NOT_SUPPORTED;
        smart_io_reason_set(
            reason, "%s is not writable", instance->role_api->description.name);
    }
    else if (
        (value < instance->role_api->description.value_min)
        || (value > instance->role_api->description.value_max))
    {
        error_code = ESP_ERR_INVALID_ARG;
        smart_io_reason_set(
            reason,
            "Value must be %ld-%ld",
            (long)instance->role_api->description.value_min,
            (long)instance->role_api->description.value_max);
    }
    else
    {
        error_code = instance->role_api->write(instance, value, &applied_value, reason);

        if (error_code == ESP_OK)
        {
            smart_io_value_changed(instance, applied_value);
        }
    }

    smart_io_unlock();

    if ((error_code == ESP_OK) && (applied != NULL))
    {
        *applied = applied_value;
    }

    return error_code;
}

void smart_io_value_changed(smart_io_instance_t* instance, int32_t value)
{
    if (!instance->state.value_valid || (instance->state.value != value))
    {
        instance->state.value       = value;
        instance->state.value_valid = true;
        smart_io_pending_values |= smart_io_config_mask(&instance->config);
    }
}

void smart_io_value_sample(smart_io_instance_t* instance, int32_t value, int64_t time_us)
{
    if ((instance->role_api->description.stream_kind == SMART_IO_STREAM_KIND_VALUE)
        && smart_io_instance_streamed(instance))
    {
        smart_io_value_push(instance->index, value, time_us);
    }
}

/* ---- Buses ---- */

esp_err_t smart_io_transfer(
    gpio_num_t gpio,
    const smart_io_transfer_t* transfers,
    size_t count,
    size_t* failed,
    smart_io_reason_t* reason)
{
    smart_io_instance_t* instance = NULL;
    esp_err_t error_code          = ESP_OK;

    *failed = count;

    do
    {
        error_code = smart_io_transfers_check(transfers, count, failed, reason);

        if (error_code != ESP_OK)
        {
            break;
        }

        error_code = smart_io_bus_acquire(gpio, false, &instance, reason);

        if (error_code != ESP_OK)
        {
            break;
        }

        error_code =
            instance->role_api->transfer(instance, transfers, count, failed, reason);
        smart_io_bus_release(instance);
    } while (0);

    return error_code;
}

esp_err_t smart_io_discover(
    gpio_num_t gpio,
    uint16_t* found,
    size_t capacity,
    size_t* count,
    smart_io_reason_t* reason)
{
    smart_io_instance_t* instance = NULL;
    esp_err_t error_code          = smart_io_bus_acquire(gpio, true, &instance, reason);

    *count = 0u;

    if (error_code == ESP_OK)
    {
        error_code = instance->role_api->discover(instance, found, capacity, count, reason);
        smart_io_bus_release(instance);
    }

    return error_code;
}

/* ---- Streams ---- */

esp_err_t smart_io_stream(gpio_num_t gpio, smart_io_reason_t* reason)
{
    esp_err_t error_code = ESP_OK;

    smart_io_lock();

    smart_io_instance_t* const instance = smart_io_instance_get(gpio, reason);

    if (instance == NULL)
    {
        error_code = ESP_ERR_NOT_FOUND;
    }
    else if (instance->role_api->description.stream_kind == SMART_IO_STREAM_KIND_NONE)
    {
        error_code = ESP_ERR_NOT_SUPPORTED;
        smart_io_reason_set(
            reason, "%s can't be streamed", instance->role_api->description.name);
    }
    else if (__atomic_load_n(&smart_io_ota_active, __ATOMIC_SEQ_CST))
    {
        error_code = ESP_ERR_INVALID_STATE;
        smart_io_reason_set(reason, "Firmware update running");
    }
    else if (smart_io_stream_clients() == 0u)
    {
        error_code = ESP_ERR_INVALID_STATE;
        smart_io_reason_set(reason, "No stream client attached");
    }
    else if (smart_io_instance_streamed(instance))
    {
        /* Already streamed. */
    }
    else
    {
        error_code = smart_io_instance_stream_begin(instance, reason);
    }

    smart_io_unlock();

    return error_code;
}

esp_err_t smart_io_unstream(gpio_num_t gpio, smart_io_reason_t* reason)
{
    esp_err_t error_code = ESP_OK;

    smart_io_lock();

    smart_io_instance_t* const instance = smart_io_instance_get(gpio, reason);

    if (instance == NULL)
    {
        error_code = ESP_ERR_NOT_FOUND;
    }
    else
    {
        smart_io_instance_stream_end(instance);
    }

    smart_io_unlock();

    return error_code;
}

uint32_t smart_io_streams(void)
{
    return smart_io_stream_mask();
}

/* posted is cleared before the masks are taken: a change ORed in after the
 * take always sees it clear and posts again. */
void smart_io_changes_take(smart_io_changes_t* changes)
{
    __atomic_store_n(&smart_io_changed_posted, false, __ATOMIC_SEQ_CST);

    const uint32_t flags =
        __atomic_exchange_n(&smart_io_changed_flags, 0u, __ATOMIC_SEQ_CST);

    changes->values = __atomic_exchange_n(&smart_io_changed_values, 0u, __ATOMIC_SEQ_CST);
    changes->configs = __atomic_exchange_n(&smart_io_changed_configs, 0u, __ATOMIC_SEQ_CST);
    changes->streams = (flags & SMART_IO_CHANGED_STREAMS) != 0u;
    changes->unsaved = (flags & SMART_IO_CHANGED_UNSAVED) != 0u;
}

esp_err_t smart_io_client_attach(smart_io_stream_cursor_t* cursor)
{
    smart_io_lock();

    const esp_err_t error_code = smart_io_stream_attach(cursor);

    smart_io_unlock();

    return error_code;
}

/* The last client takes every stream with it. */
void smart_io_client_detach(void)
{
    smart_io_lock();

    if (smart_io_stream_clients() == 1u)
    {
        /* The last detach frees the rings. An ISR still pushing into one is
         * safe only because it can't run while this task does: same core. */
        assert(xPortGetCoreID() == SMART_IO_DIGITAL_ISR_CORE);
        smart_io_instances_unstream();
        smart_io_value_ring = NULL; /* freed by the last detach */
    }

    smart_io_stream_detach();

    smart_io_unlock();
}

/**** END OF FILE ****/
