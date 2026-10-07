/**
 ******************************************************************************
 * @file    ota_shell.h
 * @author  Daniel Dyringer
 * @date    2026-08-13
 * @brief   OTA shell commands.
 ******************************************************************************
 */

/* Define to prevent recursive inclusion ----------------------------------- */
#ifndef OTA_SHELL_H_
#define OTA_SHELL_H_

/* Includes ---------------------------------------------------------------- */

#include "esp_err.h"

/* Exposed API ------------------------------------------------------------- */

/**
 * @brief Register OTA shell commands with app_shell.
 *
 *   ota status — print running version, target partition, and last OTA
 *                result
 *
 * @return ESP_OK on success.
 */
esp_err_t ota_shell_register(void);

#endif /* OTA_SHELL_H_ */

/**** END OF FILE ****/
