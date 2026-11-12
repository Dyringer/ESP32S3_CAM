/**
 ******************************************************************************
 * @file    snapshot_upload_settings.c
 * @author  Daniel Dyringer
 * @date    2026-10-11
 * @brief   Snapshot uploader settings, registered in data_manager.
 ******************************************************************************
 */


/* Includes ---------------------------------------------------------------- */
#include "snapshot_upload_settings.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "snapshot_upload.h"
#include "utils.h"


/* Constants and macros ---------------------------------------------------- */
// clang-format off


// clang-format on

/* Private types ----------------------------------------------------------- */


/* Private variables ------------------------------------------------------- */

/* Guarded by the data_manager lock: access only through data_manager. */
static bool snapshot_upload_settings_enabled                            = false;
static char snapshot_upload_settings_url[SNAPSHOT_UPLOAD_URL_MAX]       = {0};
static char snapshot_upload_settings_user[SNAPSHOT_UPLOAD_CRED_MAX]     = {0};
static char snapshot_upload_settings_password[SNAPSHOT_UPLOAD_CRED_MAX] = {0};
static uint32_t snapshot_upload_settings_interval_s                     = 0u;

static const bool snapshot_upload_settings_enabled_default = false;
static const uint32_t snapshot_upload_settings_interval_default =
    SNAPSHOT_UPLOAD_INTERVAL_DEFAULT_S;

/* Default of every string setting: at least as large as each of them. */
static const char snapshot_upload_settings_empty[SNAPSHOT_UPLOAD_URL_MAX] = {0};


/* Private functions ------------------------------------------------------- */

static esp_err_t snapshot_upload_settings_validate_url(const void* value, size_t size)
{
    ARG_UNUSED(size);

    const char* const url = value;
    esp_err_t error_code  = ESP_OK;

    if ((url[0] != '\0') && (strncmp(url, "http://", 7u) != 0)
        && (strncmp(url, "https://", 8u) != 0))
    {
        error_code = ESP_ERR_INVALID_ARG;
    }

    return error_code;
}

// clang-format off
static const data_manager_descriptor_t snapshot_upload_settings_table[] = {
    {
        .name          = SNAPSHOT_UPLOAD_ENABLED_NAME,
        .value         = &snapshot_upload_settings_enabled,
        .default_value = &snapshot_upload_settings_enabled_default,
        .size          = sizeof(snapshot_upload_settings_enabled),
        .id            = SNAPSHOT_UPLOAD_ENABLED,
        .type          = DATA_MANAGER_TYPE_BOOL,
        .persistent    = true,
    },
    {
        .name          = SNAPSHOT_UPLOAD_URL_NAME,
        .value         = snapshot_upload_settings_url,
        .default_value = snapshot_upload_settings_empty,
        .validator     = snapshot_upload_settings_validate_url,
        .size          = sizeof(snapshot_upload_settings_url),
        .id            = SNAPSHOT_UPLOAD_URL,
        .type          = DATA_MANAGER_TYPE_STRING,
        .persistent    = true,
    },
    {
        .name          = SNAPSHOT_UPLOAD_USER_NAME,
        .value         = snapshot_upload_settings_user,
        .default_value = snapshot_upload_settings_empty,
        .size          = sizeof(snapshot_upload_settings_user),
        .id            = SNAPSHOT_UPLOAD_USER,
        .type          = DATA_MANAGER_TYPE_STRING,
        .persistent    = true,
    },
    {
        .name          = SNAPSHOT_UPLOAD_PASSWORD_NAME,
        .value         = snapshot_upload_settings_password,
        .default_value = snapshot_upload_settings_empty,
        .size          = sizeof(snapshot_upload_settings_password),
        .id            = SNAPSHOT_UPLOAD_PASSWORD,
        .type          = DATA_MANAGER_TYPE_STRING,
        .persistent    = true,
        .secret        = true,
    },
    {
        .name          = SNAPSHOT_UPLOAD_INTERVAL_NAME,
        .value         = &snapshot_upload_settings_interval_s,
        .default_value = &snapshot_upload_settings_interval_default,
        .size          = sizeof(snapshot_upload_settings_interval_s),
        .id            = SNAPSHOT_UPLOAD_INTERVAL,
        .type          = DATA_MANAGER_TYPE_U32,
        .minimum       = (int32_t)SNAPSHOT_UPLOAD_INTERVAL_MIN_S,
        .maximum       = (int32_t)SNAPSHOT_UPLOAD_INTERVAL_MAX_S,
        .persistent    = true,
    },
};
// clang-format on


/* Exposed API ------------------------------------------------------------- */

esp_err_t snapshot_upload_settings_init(data_manager_on_change_t on_change)
{
    return data_manager_register(
        snapshot_upload_settings_table,
        ARRAY_SIZE(snapshot_upload_settings_table),
        on_change);
}

esp_err_t snapshot_upload_settings_restore_defaults(void)
{
    return data_manager_reset(
        snapshot_upload_settings_table, ARRAY_SIZE(snapshot_upload_settings_table));
}

/**** END OF FILE ****/
