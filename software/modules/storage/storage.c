/**
 ******************************************************************************
 * @file    storage.c
 * @author  Daniel Dyringer
 * @date    2026-10-25
 * @brief   LittleFS on the "storage" partition.
 ******************************************************************************
 */


/* Includes ---------------------------------------------------------------- */
#include "storage.h"

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "esp_littlefs.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "sdkconfig.h"


/* Constants and macros ---------------------------------------------------- */
// clang-format off

#define STORAGE_UPLOAD_TEMP_PATH    STORAGE_MOUNT_POINT "/.upload.tmp"

/* Every path below the mount point starts with this prefix. */
#define STORAGE_ROOT_PREFIX         STORAGE_MOUNT_POINT "/"

/* CONFIG_LITTLEFS_OBJ_NAME_LEN includes the terminating NUL. */
#define STORAGE_NAME_LENGTH_MAX     ((size_t)CONFIG_LITTLEFS_OBJ_NAME_LEN - 1u)

#define STORAGE_ASCII_SPACE         0x20u
#define STORAGE_ASCII_DELETE        0x7Fu

// clang-format on

/* Private types ----------------------------------------------------------- */


/* Private variables ------------------------------------------------------- */

static const char* storage_tag = "storage";

/* Guards storage_mounted, storage_formatting and storage_hold_count. */
static portMUX_TYPE storage_lock = portMUX_INITIALIZER_UNLOCKED;

static bool storage_mounted        = false;
static bool storage_formatting     = false;
static uint32_t storage_hold_count = 0u;


/* Private functions ------------------------------------------------------- */

/* Checks '/'-separated segments of a path without its leading '/'. Scanning
 * stops at max_length so an unterminated string can't be overrun. */
static esp_err_t storage_validate_segments(const char* path, size_t max_length)
{
    esp_err_t error_code  = ESP_OK;
    size_t segment_length = 0u;
    size_t i              = 0u;
    bool done             = false;

    while ((!done) && (error_code == ESP_OK))
    {
        if (i >= max_length)
        {
            error_code = ESP_ERR_INVALID_ARG;
            break;
        }

        const uint8_t character = (uint8_t)path[i];

        if ((character == (uint8_t)'\0') || (character == (uint8_t)'/'))
        {
            if (segment_length == 0u)
            {
                error_code = ESP_ERR_INVALID_ARG;
            }

            done           = (character == (uint8_t)'\0');
            segment_length = 0u;
        }
        else if ((character < STORAGE_ASCII_SPACE) || (character == STORAGE_ASCII_DELETE))
        {
            error_code = ESP_ERR_INVALID_ARG;
        }
        else if ((segment_length == 0u) && (character == (uint8_t)'.'))
        {
            error_code = ESP_ERR_INVALID_ARG;
        }
        else
        {
            segment_length++;

            if (segment_length > STORAGE_NAME_LENGTH_MAX)
            {
                error_code = ESP_ERR_INVALID_ARG;
            }
        }

        i++;
    }

    return error_code;
}

static esp_err_t storage_errno_to_esp_err(int error_number)
{
    esp_err_t error_code = ESP_FAIL;

    switch (error_number)
    {
        case ENOENT:
        {
            error_code = ESP_ERR_NOT_FOUND;
        }
        break;
        case ENOTEMPTY:
        case EEXIST:
        case EBUSY:
        {
            error_code = ESP_ERR_INVALID_STATE;
        }
        break;
        default:
        {
            error_code = ESP_FAIL;
        }
        break;
    }

    return error_code;
}

static void storage_remove_upload_leftover(void)
{
    const int result = unlink(STORAGE_UPLOAD_TEMP_PATH);

    if (result == 0)
    {
        ESP_LOGI(storage_tag, "Removed leftover upload temp file");
    }
    else if (errno != ENOENT)
    {
        ESP_LOGW(storage_tag, "Can't remove upload temp file: errno %d", errno);
    }
    else
    {
        /* No leftover */
    }
}


/* Exposed API ------------------------------------------------------------- */

esp_err_t storage_init(void)
{
    esp_err_t error_code = ESP_OK;

    do
    {
        const esp_vfs_littlefs_conf_t config = {
            .base_path              = STORAGE_MOUNT_POINT,
            .partition_label        = STORAGE_PARTITION_LABEL,
            .format_if_mount_failed = true,
        };

        error_code = esp_vfs_littlefs_register(&config);

        if (error_code != ESP_OK)
        {
            ESP_LOGE(storage_tag, "Mount failed: %s", esp_err_to_name(error_code));
            break;
        }

        taskENTER_CRITICAL(&storage_lock);
        storage_mounted = true;
        taskEXIT_CRITICAL(&storage_lock);

        size_t total_bytes = 0u;
        size_t used_bytes  = 0u;

        error_code = storage_get_info(&total_bytes, &used_bytes);

        if (error_code != ESP_OK)
        {
            ESP_LOGW(storage_tag, "Info failed: %s", esp_err_to_name(error_code));
            error_code = ESP_OK;
        }
        else
        {
            ESP_LOGI(
                storage_tag,
                "Mounted at %s: %u / %u bytes used",
                STORAGE_MOUNT_POINT,
                (unsigned)used_bytes,
                (unsigned)total_bytes);
        }

        storage_remove_upload_leftover();
    } while (0);

    return error_code;
}

bool storage_is_mounted(void)
{
    taskENTER_CRITICAL(&storage_lock);
    const bool mounted = (storage_mounted && (!storage_formatting));
    taskEXIT_CRITICAL(&storage_lock);

    return mounted;
}

esp_err_t storage_get_info(size_t* total_bytes, size_t* used_bytes)
{
    esp_err_t error_code = ESP_OK;

    storage_hold();

    if ((total_bytes == NULL) || (used_bytes == NULL))
    {
        error_code = ESP_ERR_INVALID_ARG;
    }
    else if (!storage_is_mounted())
    {
        error_code = ESP_ERR_INVALID_STATE;
    }
    else
    {
        error_code = esp_littlefs_info(STORAGE_PARTITION_LABEL, total_bytes, used_bytes);
    }

    storage_release();

    return error_code;
}

esp_err_t storage_resolve_path(const char* relative_path, char* full_path, size_t full_path_size)
{
    esp_err_t error_code = ESP_OK;

    do
    {
        if ((relative_path == NULL) || (full_path == NULL) || (full_path_size == 0u))
        {
            error_code = ESP_ERR_INVALID_ARG;
            break;
        }

        const char* const path =
            (relative_path[0] == '/') ? &relative_path[1] : relative_path;
        int length = 0;

        if (path[0] == '\0')
        {
            length = snprintf(full_path, full_path_size, "%s", STORAGE_MOUNT_POINT);
        }
        else
        {
            error_code = storage_validate_segments(path, full_path_size);

            if (error_code != ESP_OK)
            {
                break;
            }

            length =
                snprintf(full_path, full_path_size, "%s/%s", STORAGE_MOUNT_POINT, path);
        }

        if ((length < 0) || ((size_t)length >= full_path_size))
        {
            error_code = ESP_ERR_INVALID_ARG;
        }
    } while (0);

    if ((error_code != ESP_OK) && (full_path != NULL) && (full_path_size > 0u))
    {
        full_path[0] = '\0';
    }

    return error_code;
}

esp_err_t storage_remove(const char* full_path)
{
    esp_err_t error_code = ESP_OK;

    storage_hold();

    do
    {
        if ((full_path == NULL)
            || (strncmp(full_path, STORAGE_ROOT_PREFIX, strlen(STORAGE_ROOT_PREFIX)) != 0)
            || (full_path[strlen(STORAGE_ROOT_PREFIX)] == '\0'))
        {
            error_code = ESP_ERR_INVALID_ARG;
            break;
        }

        if (!storage_is_mounted())
        {
            error_code = ESP_ERR_INVALID_STATE;
            break;
        }

        struct stat info = {0};

        if (stat(full_path, &info) != 0)
        {
            error_code = storage_errno_to_esp_err(errno);
            break;
        }

        const int result = S_ISDIR(info.st_mode) ? rmdir(full_path) : unlink(full_path);

        if (result != 0)
        {
            error_code = storage_errno_to_esp_err(errno);
        }
    } while (0);

    storage_release();

    return error_code;
}

const char* storage_upload_temp_path(void)
{
    return STORAGE_UPLOAD_TEMP_PATH;
}

void storage_hold(void)
{
    taskENTER_CRITICAL(&storage_lock);
    storage_hold_count++;
    taskEXIT_CRITICAL(&storage_lock);
}

void storage_release(void)
{
    taskENTER_CRITICAL(&storage_lock);

    if (storage_hold_count > 0u)
    {
        storage_hold_count--;
    }

    taskEXIT_CRITICAL(&storage_lock);
}

esp_err_t storage_format(void)
{
    esp_err_t error_code = ESP_OK;

    /* A hold taken after storage_formatting is set sees storage_is_mounted()
     * return false. This only closes the race if callers hold first and check
     * storage_is_mounted() second. */
    taskENTER_CRITICAL(&storage_lock);

    if ((!storage_mounted) || storage_formatting || (storage_hold_count > 0u))
    {
        error_code = ESP_ERR_INVALID_STATE;
    }
    else
    {
        storage_formatting = true;
    }

    taskEXIT_CRITICAL(&storage_lock);

    if (error_code == ESP_OK)
    {
        ESP_LOGW(storage_tag, "Formatting partition \"%s\"", STORAGE_PARTITION_LABEL);

        /* Unmounts, formats and remounts in place; the VFS registration stays. */
        error_code = esp_littlefs_format(STORAGE_PARTITION_LABEL);

        taskENTER_CRITICAL(&storage_lock);
        storage_formatting = false;

        if (error_code != ESP_OK)
        {
            storage_mounted = false;
        }

        taskEXIT_CRITICAL(&storage_lock);

        if (error_code != ESP_OK)
        {
            ESP_LOGE(storage_tag, "Format failed: %s", esp_err_to_name(error_code));
        }
        else
        {
            ESP_LOGI(storage_tag, "Format done");
        }
    }

    return error_code;
}

/**** END OF FILE ****/
