/**
 ******************************************************************************
 * @file    wifi_provisioning.h
 * @author  Daniel Dyringer
 * @date    2026-02-17
 * @brief   Captive portal provisioning — DNS redirect + HTTP config page.
 ******************************************************************************
 */

/* Define to prevent recursive inclusion ----------------------------------- */
#ifndef WIFI_PROVISIONING_H_
#define WIFI_PROVISIONING_H_

/* Includes ---------------------------------------------------------------- */

#include "esp_err.h"

/* Constants and macros ---------------------------------------------------- */
// clang-format off

// clang-format on

/* Types ------------------------------------------------------------------- */


/* Exposed API ------------------------------------------------------------- */

/**
 * @brief Start the captive portal:
 *        - DNS server on UDP/53 (redirects all queries to AP IP)
 *        - HTTP server:
 *            GET  /          → provisioning page (scan dropdown + password form)
 *            GET  /scan      → JSON array of {ssid, rssi}
 *            POST /configure → trigger wifi_connection_set_credentials()
 *            *               → 302 redirect to /
 */
esp_err_t wifi_provisioning_start(void);

/** @brief Stop HTTP server and DNS task. */
esp_err_t wifi_provisioning_stop(void);

#endif /* WIFI_PROVISIONING_H_ */

/**** END OF FILE ****/