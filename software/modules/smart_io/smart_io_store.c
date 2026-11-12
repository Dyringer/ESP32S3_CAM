/**
 ******************************************************************************
 * @file    smart_io_store.c
 * @author  Daniel Dyringer
 * @date    2026-10-31
 * @brief   Saved configuration blob in data_manager.
 ******************************************************************************
 */


/* Includes ---------------------------------------------------------------- */
#include "smart_io_store.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "data_manager.h"
#include "esp_log.h"
#include "smart_io_registry.h"


/* Constants and macros ---------------------------------------------------- */
// clang-format off

/* Bump on any change of smart_io_config_t or a role's param table order. */
#define SMART_IO_STORE_VERSION      1u
#define SMART_IO_STORE_NAME         "smart_io_config"
#define SMART_IO_STORE_ID           DATA_MANAGER_ID(SMART_IO_STORE_NAME)

// clang-format on

/* Private types ----------------------------------------------------------- */

/* version 0 with count 0 is the data_manager default: nothing saved. */
typedef struct smart_io_store_blob_s
{
    uint32_t version;
    uint32_t count;
    smart_io_config_t configs[SMART_IO_MAX_PINS];
} smart_io_store_blob_t;


/* Private variables ------------------------------------------------------- */

static const char* smart_io_store_tag = "smart_io_store";

static smart_io_store_blob_t smart_io_store_value;
static const smart_io_store_blob_t smart_io_store_default = {0};

static esp_err_t smart_io_store_validate(const void* value, size_t size);

static const data_manager_descriptor_t smart_io_store_descriptor = {
    .name          = SMART_IO_STORE_NAME,
    .value         = &smart_io_store_value,
    .default_value = &smart_io_store_default,
    .validator     = smart_io_store_validate,
    .size          = sizeof(smart_io_store_blob_t),
    .id            = SMART_IO_STORE_ID,
    .type          = DATA_MANAGER_TYPE_BLOB,
    .minimum       = 0,
    .maximum       = 0,
    .persistent    = true,
    .secret        = false,
};


/* Private functions ------------------------------------------------------- */

static esp_err_t smart_io_store_validate(const void* value, size_t size)
{
    esp_err_t error_code              = ESP_OK;
    const smart_io_store_blob_t* blob = value;

    if ((size != sizeof(smart_io_store_blob_t)) || (blob->count > SMART_IO_MAX_PINS))
    {
        error_code = ESP_ERR_INVALID_SIZE;
    }

    return error_code;
}


/* Exposed API ------------------------------------------------------------- */

esp_err_t smart_io_store_init(void)
{
    return data_manager_register(&smart_io_store_descriptor, 1u, NULL);
}

esp_err_t smart_io_store_save(const smart_io_config_t* configs, size_t count)
{
    esp_err_t error_code              = ESP_OK;
    smart_io_store_blob_t* const blob = calloc(1u, sizeof(smart_io_store_blob_t));

    do
    {
        if (blob == NULL)
        {
            error_code = ESP_ERR_NO_MEM;
            break;
        }

        if (count > SMART_IO_MAX_PINS)
        {
            error_code = ESP_ERR_INVALID_SIZE;
            break;
        }

        blob->version = SMART_IO_STORE_VERSION;
        blob->count   = (uint32_t)count;
        memcpy(blob->configs, configs, count * sizeof(smart_io_config_t));

        error_code =
            data_manager_set(SMART_IO_STORE_ID, blob, sizeof(smart_io_store_blob_t));

        if (error_code != ESP_OK)
        {
            break;
        }

        error_code = data_manager_save();
    } while (0);

    if (error_code != ESP_OK)
    {
        ESP_LOGE(smart_io_store_tag, "Save failed: %s", esp_err_to_name(error_code));
    }

    free(blob);

    return error_code;
}

esp_err_t smart_io_store_load(smart_io_config_t* configs, size_t capacity, size_t* count)
{
    esp_err_t error_code              = ESP_OK;
    smart_io_store_blob_t* const blob = calloc(1u, sizeof(smart_io_store_blob_t));

    *count = 0u;

    do
    {
        if (blob == NULL)
        {
            error_code = ESP_ERR_NO_MEM;
            break;
        }

        error_code =
            data_manager_get(SMART_IO_STORE_ID, blob, sizeof(smart_io_store_blob_t));

        if (error_code != ESP_OK)
        {
            break;
        }

        if ((blob->version == 0u) && (blob->count == 0u))
        {
            error_code = ESP_ERR_NOT_FOUND;
            break;
        }

        if (blob->version != SMART_IO_STORE_VERSION)
        {
            ESP_LOGW(
                smart_io_store_tag,
                "Saved configuration version %lu ignored (expected %lu)",
                (unsigned long)blob->version,
                (unsigned long)SMART_IO_STORE_VERSION);
            error_code = ESP_ERR_INVALID_VERSION;
            break;
        }

        if (blob->count > capacity)
        {
            error_code = ESP_ERR_INVALID_SIZE;
            break;
        }

        memcpy(configs, blob->configs, blob->count * sizeof(smart_io_config_t));
        *count = blob->count;
    } while (0);

    free(blob);

    return error_code;
}

/**** END OF FILE ****/
