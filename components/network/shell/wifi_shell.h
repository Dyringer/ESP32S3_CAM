/**
 ******************************************************************************
 * @file    wifi_shell.h
 * @author  Daniel Dyringer
 * @date    2026-02-17
 * @brief   WiFi diagnostic shell commands — status, scan, join, forget.
 ******************************************************************************
 */

/* Define to prevent recursive inclusion ----------------------------------- */
#ifndef WIFI_SHELL_H_
#define WIFI_SHELL_H_

/* Includes ---------------------------------------------------------------- */

#include "esp_err.h"

/* Exposed API ------------------------------------------------------------- */

/**
 * @brief Register WiFi shell commands with app_shell.
 *
 *   wifi status              — current state, SSID, IP / gateway / netmask
 *   wifi scan                — active scan, prints a formatted RSSI table
 *   wifi join <ssid> [pass]  — apply credentials and trigger reconnect
 *   wifi forget              — erase NVS credentials, reboot
 *
 * @note  SSIDs containing spaces are not supported by the shell tokeniser.
 *
 * @return ESP_OK on success.
 */
esp_err_t wifi_shell_register(void);

#endif /* WIFI_SHELL_H_ */

/**** END OF FILE ****/
