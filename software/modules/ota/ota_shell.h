/**
 ******************************************************************************
 * @file    ota_shell.h
 * @author  Daniel Dyringer
 * @date    2026-08-13
 * @brief   OTA shell commands.
 *
 *          Commands:
 *            ota status                Running version and last OTA result
 ******************************************************************************
 */

/* Define to prevent recursive inclusion ----------------------------------- */
#ifndef OTA_SHELL_H_
#define OTA_SHELL_H_

/* Includes ---------------------------------------------------------------- */
#include "esp_err.h"

/* Constants and macros ---------------------------------------------------- */
// clang-format off


// clang-format on

/* Types ------------------------------------------------------------------- */


/* Exposed API ------------------------------------------------------------- */

esp_err_t ota_shell_register(void);

#endif /* OTA_SHELL_H_ */

/**** END OF FILE ****/
