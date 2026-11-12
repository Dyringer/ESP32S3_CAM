/**
 ******************************************************************************
 * @file    network_settings.c
 * @author  Daniel Dyringer
 * @date    2026-10-11
 * @brief   Network settings, registered in data_manager.
 ******************************************************************************
 */


/* Includes ---------------------------------------------------------------- */
#include "network_settings.h"

#include "utils.h"
#include "wifi_types.h"


/* Constants and macros ---------------------------------------------------- */
// clang-format off


// clang-format on

/* Private types ----------------------------------------------------------- */


/* Private variables ------------------------------------------------------- */

/* Guarded by the data_manager lock: access only through data_manager. */
static char network_settings_wifi_ssid[WIFI_TYPES_SSID_LEN]     = {0};
static char network_settings_wifi_password[WIFI_TYPES_PASS_LEN] = {0};

/* Default of both strings: at least as large as each of them. */
static const char network_settings_empty[WIFI_TYPES_PASS_LEN] = {0};

// clang-format off
static const data_manager_descriptor_t network_settings_table[] = {
    {
        .name          = NETWORK_WIFI_SSID_NAME,
        .value         = network_settings_wifi_ssid,
        .default_value = network_settings_empty,
        .size          = sizeof(network_settings_wifi_ssid),
        .id            = NETWORK_WIFI_SSID,
        .type          = DATA_MANAGER_TYPE_STRING,
        .persistent    = true,
    },
    {
        .name          = NETWORK_WIFI_PASSWORD_NAME,
        .value         = network_settings_wifi_password,
        .default_value = network_settings_empty,
        .size          = sizeof(network_settings_wifi_password),
        .id            = NETWORK_WIFI_PASSWORD,
        .type          = DATA_MANAGER_TYPE_STRING,
        .persistent    = true,
        .secret        = true,
    },
};
// clang-format on


/* Private functions ------------------------------------------------------- */


/* Exposed API ------------------------------------------------------------- */

esp_err_t network_settings_init(data_manager_on_change_t on_change)
{
    return data_manager_register(
        network_settings_table, ARRAY_SIZE(network_settings_table), on_change);
}

esp_err_t network_settings_restore_defaults(void)
{
    return data_manager_reset(network_settings_table, ARRAY_SIZE(network_settings_table));
}

/**** END OF FILE ****/
