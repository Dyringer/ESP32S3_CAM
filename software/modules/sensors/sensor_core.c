/**
 ******************************************************************************
 * @file    sensor_core.c
 * @author  Daniel Dyringer
 * @date    2026-02-18
 * @brief   Sensor core — driver registry and polling task.
 *          The I2C bus is injected into driver configs by sensor_registry;
 *          sensor_core has no knowledge of it.
 ******************************************************************************
 */


/* Includes ---------------------------------------------------------------- */
#include "sensor_core.h"

#include <stdbool.h>
#include <stdlib.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "sensor_store.h"
#include "utils.h"


/* Constants and macros ---------------------------------------------------- */
// clang-format off

#define SENSOR_CORE_TASK_STACK      3584u
#define SENSOR_CORE_TASK_PRIO       3u
#define SENSOR_CORE_TASK_CORE       1

#define SENSOR_CORE_FAULT_RETRY_MS  2000u

// clang-format on

/* Private types ----------------------------------------------------------- */

typedef struct sensor_core_entry_s
{
    const sensor_driver_t* driver;
    TickType_t period;
    TickType_t next_due;
    sensor_types_state_t state;
} sensor_core_entry_t;


/* Private variables ------------------------------------------------------- */

static const char* sensor_core_tag = "sensor_core";

ESP_EVENT_DEFINE_BASE(SENSOR_CORE_EVENTS);

static SemaphoreHandle_t sensor_core_pause_mutex = NULL;

/* Written by sensor_core_register before start, then touched only by the
 * sensor_core task. */
static sensor_core_entry_t sensor_core_registry[SENSOR_ID_COUNT];
static uint8_t* sensor_core_sample_buffer = NULL;


/* Private functions ------------------------------------------------------- */

/* Signed difference keeps the comparison correct across tick-count wrap. */
static int32_t sensor_core_ticks_until(TickType_t due, TickType_t now)
{
    return (int32_t)(due - now);
}

/* Logged once per fault; retries stay at debug level so a missing sensor
 * doesn't flood the consoles every SENSOR_CORE_FAULT_RETRY_MS. */
static void sensor_core_fault(sensor_id_t id, const char* what, esp_err_t error_code, TickType_t now)
{
    sensor_core_entry_t* entry = &sensor_core_registry[id];

    if (entry->state == SENSOR_TYPES_STATE_FAULT)
    {
        ESP_LOGD(
            sensor_core_tag,
            "Sensor '%s' %s failed: %s",
            entry->driver->name,
            what,
            esp_err_to_name(error_code));
    }
    else
    {
        ESP_LOGE(
            sensor_core_tag,
            "Sensor '%s' %s failed: %s — retrying every %u ms",
            entry->driver->name,
            what,
            esp_err_to_name(error_code),
            (unsigned)SENSOR_CORE_FAULT_RETRY_MS);
    }

    entry->state    = SENSOR_TYPES_STATE_FAULT;
    entry->next_due = now + pdMS_TO_TICKS(SENSOR_CORE_FAULT_RETRY_MS);

    /* Readers see "no data" instead of the last good sample. */
    sensor_store_clear(id);
}

static void sensor_core_service(sensor_id_t id, TickType_t now)
{
    sensor_core_entry_t* entry = &sensor_core_registry[id];

    do
    {
        if (entry->state != SENSOR_TYPES_STATE_OK)
        {
            const esp_err_t init_error = entry->driver->api->init(entry->driver);

            if (init_error != ESP_OK)
            {
                sensor_core_fault(id, "init", init_error, now);
                break;
            }

            ESP_LOGI(
                sensor_core_tag,
                "Sensor '%s' %s",
                entry->driver->name,
                (entry->state == SENSOR_TYPES_STATE_FAULT) ? "recovered" : "OK");

            /* First read one period after init, once the sensor has converted. */
            entry->state    = SENSOR_TYPES_STATE_OK;
            entry->next_due = now + entry->period;
            break;
        }

        const esp_err_t read_error =
            entry->driver->api->read(entry->driver, sensor_core_sample_buffer);

        if (read_error != ESP_OK)
        {
            sensor_core_fault(id, "read", read_error, now);
            break;
        }

        const esp_err_t store_error =
            sensor_store_set(id, sensor_core_sample_buffer, entry->driver->sample_size);

        if (store_error == ESP_OK)
        {
            esp_event_post(
                SENSOR_CORE_EVENTS, SENSOR_CORE_EVENT_UPDATED, &id, sizeof(id), 0);
        }

        /* Fixed cadence without drift; after an overrun (e.g. a long pause) skip
         * the missed slots instead of reading back-to-back. */
        entry->next_due += entry->period;

        if (sensor_core_ticks_until(entry->next_due, now) <= 0)
        {
            entry->next_due = now + entry->period;
        }
    } while (0);
}

static TickType_t sensor_core_time_to_next(TickType_t now)
{
    int32_t wait = INT32_MAX;

    for (size_t i = 0u; i < (size_t)SENSOR_ID_COUNT; i++)
    {
        const sensor_core_entry_t* entry = &sensor_core_registry[i];

        if (entry->driver != NULL)
        {
            wait = MIN(wait, sensor_core_ticks_until(entry->next_due, now));
        }
    }

    return (TickType_t)MAX(wait, 0);
}

static void sensor_core_task(void* arg)
{
    ARG_UNUSED(arg);

    const TickType_t start = xTaskGetTickCount();

    for (size_t i = 0u; i < (size_t)SENSOR_ID_COUNT; i++)
    {
        sensor_core_registry[i].next_due = start;
    }

    while (true)
    {
        const TickType_t wait = sensor_core_time_to_next(xTaskGetTickCount());

        if (wait > 0u)
        {
            vTaskDelay(wait);
        }

        xSemaphoreTake(sensor_core_pause_mutex, portMAX_DELAY);

        const TickType_t now = xTaskGetTickCount();

        for (size_t i = 0u; i < (size_t)SENSOR_ID_COUNT; i++)
        {
            const sensor_core_entry_t* entry = &sensor_core_registry[i];

            if ((entry->driver != NULL)
                && (sensor_core_ticks_until(entry->next_due, now) <= 0))
            {
                sensor_core_service((sensor_id_t)i, now);
            }
        }

        xSemaphoreGive(sensor_core_pause_mutex);
    }
}


/* Exposed API ------------------------------------------------------------- */

esp_err_t sensor_core_register(sensor_id_t id, const sensor_driver_t* driver)
{
    esp_err_t error_code = ESP_OK;

    if ((id >= SENSOR_ID_COUNT) || (driver == NULL))
    {
        ESP_LOGE(sensor_core_tag, "Invalid registration (id=%d)", (int)id);
        error_code = ESP_ERR_INVALID_ARG;
    }
    else
    {
        sensor_core_entry_t* entry = &sensor_core_registry[id];

        entry->driver   = driver;
        entry->period   = MAX(pdMS_TO_TICKS(driver->poll_interval_ms), (TickType_t)1u);
        entry->next_due = 0u;
        entry->state    = SENSOR_TYPES_STATE_UNINITIALIZED;

        ESP_LOGI(
            sensor_core_tag,
            "Registered '%s' (id=%d, poll %lu ms)",
            driver->name,
            (int)id,
            (unsigned long)driver->poll_interval_ms);
    }

    return error_code;
}

esp_err_t sensor_core_start(void)
{
    esp_err_t error_code = ESP_OK;

    do
    {
        size_t max_sample_size = 0u;

        for (size_t i = 0u; i < (size_t)SENSOR_ID_COUNT; i++)
        {
            if (sensor_core_registry[i].driver != NULL)
            {
                max_sample_size =
                    MAX(max_sample_size, sensor_core_registry[i].driver->sample_size);
            }
        }

        if (max_sample_size == 0u)
        {
            ESP_LOGW(sensor_core_tag, "No sensors registered");
            error_code = ESP_ERR_INVALID_STATE;
            break;
        }

        sensor_core_sample_buffer = malloc(max_sample_size);
        sensor_core_pause_mutex   = xSemaphoreCreateMutex();

        if ((sensor_core_sample_buffer == NULL) || (sensor_core_pause_mutex == NULL))
        {
            error_code = ESP_ERR_NO_MEM;
            break;
        }

        error_code = sensor_store_init();

        if (error_code != ESP_OK)
        {
            break;
        }

        const BaseType_t created = xTaskCreatePinnedToCore(
            sensor_core_task,
            "sensor_core",
            SENSOR_CORE_TASK_STACK,
            NULL,
            SENSOR_CORE_TASK_PRIO,
            NULL,
            SENSOR_CORE_TASK_CORE);

        if (created != pdPASS)
        {
            error_code = ESP_ERR_NO_MEM;
            break;
        }

        ESP_LOGI(sensor_core_tag, "Polling task on core %d", SENSOR_CORE_TASK_CORE);
    } while (0);

    if ((error_code != ESP_OK) && (error_code != ESP_ERR_INVALID_STATE))
    {
        ESP_LOGE(sensor_core_tag, "Start failed: %s", esp_err_to_name(error_code));

        free(sensor_core_sample_buffer);
        sensor_core_sample_buffer = NULL;

        if (sensor_core_pause_mutex != NULL)
        {
            vSemaphoreDelete(sensor_core_pause_mutex);
            sensor_core_pause_mutex = NULL;
        }
    }

    return error_code;
}

esp_err_t sensor_core_pause(void)
{
    esp_err_t error_code = ESP_OK;

    if (sensor_core_pause_mutex == NULL)
    {
        error_code = ESP_ERR_INVALID_STATE;
    }
    else
    {
        xSemaphoreTake(sensor_core_pause_mutex, portMAX_DELAY);
        ESP_LOGI(sensor_core_tag, "Polling paused");
    }

    return error_code;
}

esp_err_t sensor_core_resume(void)
{
    esp_err_t error_code = ESP_OK;

    if (sensor_core_pause_mutex == NULL)
    {
        error_code = ESP_ERR_INVALID_STATE;
    }
    else
    {
        xSemaphoreGive(sensor_core_pause_mutex);
        ESP_LOGI(sensor_core_tag, "Polling resumed");
    }

    return error_code;
}

/**** END OF FILE ****/
