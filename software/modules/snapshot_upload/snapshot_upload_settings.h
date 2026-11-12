/**
 ******************************************************************************
 * @file    snapshot_upload_settings.h
 * @author  Daniel Dyringer
 * @date    2026-10-11
 * @brief   Snapshot uploader settings, registered in data_manager. Access
 *          them with data_manager_get/set and the ids below.
 ******************************************************************************
 */

/* Define to prevent recursive inclusion ----------------------------------- */
#ifndef SNAPSHOT_UPLOAD_SETTINGS_H_
#define SNAPSHOT_UPLOAD_SETTINGS_H_

/* Includes ---------------------------------------------------------------- */
#include "data_manager.h"
#include "esp_err.h"

/* Constants and macros ---------------------------------------------------- */
// clang-format off

/* bool */
#define SNAPSHOT_UPLOAD_ENABLED_NAME   "snapshot_upload_enabled"
#define SNAPSHOT_UPLOAD_ENABLED        DATA_MANAGER_ID(SNAPSHOT_UPLOAD_ENABLED_NAME)
/* char[SNAPSHOT_UPLOAD_URL_MAX], empty or http(s)://... */
#define SNAPSHOT_UPLOAD_URL_NAME       "snapshot_upload_url"
#define SNAPSHOT_UPLOAD_URL            DATA_MANAGER_ID(SNAPSHOT_UPLOAD_URL_NAME)
/* char[SNAPSHOT_UPLOAD_CRED_MAX], empty = no auth */
#define SNAPSHOT_UPLOAD_USER_NAME      "snapshot_upload_user"
#define SNAPSHOT_UPLOAD_USER           DATA_MANAGER_ID(SNAPSHOT_UPLOAD_USER_NAME)
/* char[SNAPSHOT_UPLOAD_CRED_MAX], secret */
#define SNAPSHOT_UPLOAD_PASSWORD_NAME  "snapshot_upload_password"
#define SNAPSHOT_UPLOAD_PASSWORD       DATA_MANAGER_ID(SNAPSHOT_UPLOAD_PASSWORD_NAME)
/* uint32_t seconds */
#define SNAPSHOT_UPLOAD_INTERVAL_NAME  "snapshot_upload_interval"
#define SNAPSHOT_UPLOAD_INTERVAL       DATA_MANAGER_ID(SNAPSHOT_UPLOAD_INTERVAL_NAME)

// clang-format on

/* Types ------------------------------------------------------------------- */


/* Exposed API ------------------------------------------------------------- */

esp_err_t snapshot_upload_settings_init(data_manager_on_change_t on_change);
esp_err_t snapshot_upload_settings_restore_defaults(void);

#endif /* SNAPSHOT_UPLOAD_SETTINGS_H_ */

/**** END OF FILE ****/
