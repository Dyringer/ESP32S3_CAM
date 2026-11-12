/**
 ******************************************************************************
 * @file    network_settings.h
 * @author  Daniel Dyringer
 * @date    2026-10-11
 * @brief   Network settings, registered in data_manager. Access them with
 *          data_manager_get/set and the ids below.
 ******************************************************************************
 */

/* Define to prevent recursive inclusion ----------------------------------- */
#ifndef NETWORK_SETTINGS_H_
#define NETWORK_SETTINGS_H_

/* Includes ---------------------------------------------------------------- */
#include "data_manager.h"
#include "esp_err.h"

/* Constants and macros ---------------------------------------------------- */
// clang-format off

/* char[WIFI_TYPES_SSID_LEN] */
#define NETWORK_WIFI_SSID_NAME      "network_wifi_ssid"
#define NETWORK_WIFI_SSID           DATA_MANAGER_ID(NETWORK_WIFI_SSID_NAME)
/* char[WIFI_TYPES_PASS_LEN], secret */
#define NETWORK_WIFI_PASSWORD_NAME  "network_wifi_password"
#define NETWORK_WIFI_PASSWORD       DATA_MANAGER_ID(NETWORK_WIFI_PASSWORD_NAME)

// clang-format on

/* Types ------------------------------------------------------------------- */


/* Exposed API ------------------------------------------------------------- */

esp_err_t network_settings_init(data_manager_on_change_t on_change);
esp_err_t network_settings_restore_defaults(void);

#endif /* NETWORK_SETTINGS_H_ */

/**** END OF FILE ****/
