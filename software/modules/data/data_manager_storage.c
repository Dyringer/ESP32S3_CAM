/**
 ******************************************************************************
 * @file    data_manager_storage.c
 * @author  Daniel Dyringer
 * @date    2026-10-11
 * @brief   NVS backend of data_manager: one blob per value, keyed by its id.
 ******************************************************************************
 */


/* Includes ---------------------------------------------------------------- */
#include "data_manager_storage.h"

#include <inttypes.h>
#include <stdbool.h>
#include <stdio.h>

#include "nvs.h"


/* Constants and macros ---------------------------------------------------- */
// clang-format off

/* Keys are the id in hex, so value names are not limited by
 * NVS_KEY_NAME_MAX_SIZE. */
#define DATA_MANAGER_STORAGE_NVS_NAMESPACE  "data_manager"
#define DATA_MANAGER_STORAGE_KEY_SIZE       9u

// clang-format on

/* Private types ----------------------------------------------------------- */


/* Private variables ------------------------------------------------------- */


/* Private functions ------------------------------------------------------- */

static void data_manager_storage_key(uint32_t id, char* key)
{
    snprintf(key, DATA_MANAGER_STORAGE_KEY_SIZE, "%08" PRIx32, id);
}


/* Exposed API ------------------------------------------------------------- */

esp_err_t data_manager_storage_load(uint32_t id, void* value, size_t size)
{
    esp_err_t error_code                    = ESP_OK;
    char key[DATA_MANAGER_STORAGE_KEY_SIZE] = {0};
    nvs_handle_t handle                     = 0;
    bool opened                             = false;

    data_manager_storage_key(id, key);

    do
    {
        error_code = nvs_open(DATA_MANAGER_STORAGE_NVS_NAMESPACE, NVS_READONLY, &handle);

        if (error_code != ESP_OK)
        {
            break;
        }

        opened = true;

        size_t stored_size = 0u;

        error_code = nvs_get_blob(handle, key, NULL, &stored_size);

        if (error_code != ESP_OK)
        {
            break;
        }

        if (stored_size != size)
        {
            error_code = ESP_ERR_INVALID_SIZE;
            break;
        }

        error_code = nvs_get_blob(handle, key, value, &stored_size);
    } while (0);

    if (opened)
    {
        nvs_close(handle);
    }

    /* Covers both a missing key and a namespace that was never created. */
    if (error_code == ESP_ERR_NVS_NOT_FOUND)
    {
        error_code = ESP_ERR_NOT_FOUND;
    }

    return error_code;
}

esp_err_t data_manager_storage_save(uint32_t id, const void* value, size_t size)
{
    esp_err_t error_code                    = ESP_OK;
    char key[DATA_MANAGER_STORAGE_KEY_SIZE] = {0};
    nvs_handle_t handle                     = 0;

    data_manager_storage_key(id, key);

    do
    {
        error_code = nvs_open(DATA_MANAGER_STORAGE_NVS_NAMESPACE, NVS_READWRITE, &handle);

        if (error_code != ESP_OK)
        {
            break;
        }

        error_code = nvs_set_blob(handle, key, value, size);

        if (error_code == ESP_OK)
        {
            error_code = nvs_commit(handle);
        }

        nvs_close(handle);
    } while (0);

    return error_code;
}

esp_err_t data_manager_storage_erase(uint32_t id)
{
    esp_err_t error_code                    = ESP_OK;
    char key[DATA_MANAGER_STORAGE_KEY_SIZE] = {0};
    nvs_handle_t handle                     = 0;

    data_manager_storage_key(id, key);

    do
    {
        error_code = nvs_open(DATA_MANAGER_STORAGE_NVS_NAMESPACE, NVS_READWRITE, &handle);

        if (error_code != ESP_OK)
        {
            break;
        }

        error_code = nvs_erase_key(handle, key);

        if (error_code == ESP_ERR_NVS_NOT_FOUND)
        {
            error_code = ESP_OK;
        }

        if (error_code == ESP_OK)
        {
            error_code = nvs_commit(handle);
        }

        nvs_close(handle);
    } while (0);

    return error_code;
}

/**** END OF FILE ****/
