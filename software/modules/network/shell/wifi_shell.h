/**
 ******************************************************************************
 * @file    wifi_shell.h
 * @author  Daniel Dyringer
 * @date    2026-02-17
 * @brief   WiFi diagnostic shell commands.
 *
 *          Commands:
 *            wifi status               Connection state, SSID, IP
 *            wifi scan                 List nearby access points
 *            wifi join <ssid> [pass]   Apply credentials and connect
 *            wifi forget               Clear credentials and reboot
 ******************************************************************************
 */

/* Define to prevent recursive inclusion ----------------------------------- */
#ifndef WIFI_SHELL_H_
#define WIFI_SHELL_H_

/* Includes ---------------------------------------------------------------- */
#include "esp_err.h"

/* Constants and macros ---------------------------------------------------- */
// clang-format off


// clang-format on

/* Types ------------------------------------------------------------------- */


/* Exposed API ------------------------------------------------------------- */

esp_err_t wifi_shell_register(void);

#endif /* WIFI_SHELL_H_ */

/**** END OF FILE ****/
