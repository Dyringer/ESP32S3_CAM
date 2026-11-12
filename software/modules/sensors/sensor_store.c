/**
 ******************************************************************************
 * @file    sensor_store.c
 * @author  Daniel Dyringer
 * @date    2026-02-22
 * @brief   Latest-sample store keyed by sensor_id_t.
 ******************************************************************************
 */


/* Includes ---------------------------------------------------------------- */
#include "sensor_store.h"

#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"


/* Constants and macros ---------------------------------------------------- */
// clang-format off

#define SENSOR_STORE_SET_TIMEOUT_MS 10u
#define SENSOR_STORE_GET_TIMEOUT_MS 100u

// clang-format on

/* Private types ----------------------------------------------------------- */

typedef struct sensor_store_slot_s
{
    void* data;
    size_t size;
    bool valid;
} sensor_store_slot_t;


/* Private variables ------------------------------------------------------- */

static const char* sensor_store_tag = "sensor_store";

/* Guarded by sensor_store_mutex */
static sensor_store_slot_t sensor_store_slots[SENSOR_ID_COUNT];
static SemaphoreHandle_t sensor_store_mutex = NULL;


/* Private functions ------------------------------------------------------- */


/* Exposed API ------------------------------------------------------------- */

esp_err_t sensor_store_init(void)
{
    esp_err_t error_code = ESP_OK;

    sensor_store_mutex = xSemaphoreCreateMutex();

    if (sensor_store_mutex == NULL)
    {
        ESP_LOGE(sensor_store_tag, "Failed to create mutex");
        error_code = ESP_ERR_NO_MEM;
    }

    return error_code;
}

esp_err_t sensor_store_set(sensor_id_t id, const void* data, size_t size)
{
    esp_err_t error_code = ESP_OK;

    do
    {
        if ((id >= SENSOR_ID_COUNT) || (data == NULL) || (sensor_store_mutex == NULL))
        {
            error_code = ESP_ERR_INVALID_ARG;
            break;
        }

        if (xSemaphoreTake(sensor_store_mutex, pdMS_TO_TICKS(SENSOR_STORE_SET_TIMEOUT_MS))
            != pdTRUE)
        {
            error_code = ESP_ERR_TIMEOUT;
            break;
        }

        sensor_store_slot_t* slot = &sensor_store_slots[id];

        if (slot->data == NULL)
        {
            slot->data = malloc(size);
            slot->size = (slot->data != NULL) ? size : 0u;
        }

        if (slot->data == NULL)
        {
            error_code = ESP_ERR_NO_MEM;
        }
        else if (slot->size != size)
        {
            error_code = ESP_ERR_INVALID_SIZE;
        }
        else
        {
            memcpy(slot->data, data, size);
            slot->valid = true;
        }

        xSemaphoreGive(sensor_store_mutex);
    } while (0);

    if (error_code != ESP_OK)
    {
        ESP_LOGE(
            sensor_store_tag,
            "set id=%d (%u bytes) failed: %s",
            (int)id,
            (unsigned)size,
            esp_err_to_name(error_code));
    }

    return error_code;
}

esp_err_t sensor_store_get(sensor_id_t id, void* data, size_t size)
{
    esp_err_t error_code = ESP_OK;

    do
    {
        if ((id >= SENSOR_ID_COUNT) || (data == NULL))
        {
            error_code = ESP_ERR_INVALID_ARG;
            break;
        }

        if (sensor_store_mutex == NULL)
        {
            error_code = ESP_ERR_INVALID_STATE;
            break;
        }

        if (xSemaphoreTake(sensor_store_mutex, pdMS_TO_TICKS(SENSOR_STORE_GET_TIMEOUT_MS))
            != pdTRUE)
        {
            error_code = ESP_ERR_TIMEOUT;
            break;
        }

        const sensor_store_slot_t* slot = &sensor_store_slots[id];

        if (!slot->valid)
        {
            error_code = ESP_ERR_NOT_FOUND;
        }
        else if (slot->size != size)
        {
            error_code = ESP_ERR_INVALID_SIZE;
        }
        else
        {
            memcpy(data, slot->data, size);
        }

        xSemaphoreGive(sensor_store_mutex);
    } while (0);

    return error_code;
}

esp_err_t sensor_store_clear(sensor_id_t id)
{
    esp_err_t error_code = ESP_OK;

    do
    {
        if ((id >= SENSOR_ID_COUNT) || (sensor_store_mutex == NULL))
        {
            error_code = ESP_ERR_INVALID_ARG;
            break;
        }

        if (xSemaphoreTake(sensor_store_mutex, pdMS_TO_TICKS(SENSOR_STORE_SET_TIMEOUT_MS))
            != pdTRUE)
        {
            error_code = ESP_ERR_TIMEOUT;
            break;
        }

        sensor_store_slots[id].valid = false;
        xSemaphoreGive(sensor_store_mutex);
    } while (0);

    return error_code;
}

/**** END OF FILE ****/
