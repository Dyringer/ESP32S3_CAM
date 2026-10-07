/**
 ******************************************************************************
 * @file    wifi_provisioning_http.h
 * @author  Daniel Dyringer
 * @date    2026-03-04
 * @brief   Captive portal HTTP layer — server lifecycle and URI handlers.
 *
 *          This module owns the esp_httpd instance used during provisioning.
 *          DNS redirect and public API live in wifi_provisioning.c.
 *
 *          Endpoints:
 *            GET  /           — provisioning page (scan dropdown + password form)
 *            GET  /scan       — JSON array of {ssid, rssi}
 *            POST /configure  — stores credentials and arms apply timer
 *            *                — 302 redirect to /
 ******************************************************************************
 */

/* Define to prevent recursive inclusion ----------------------------------- */
#ifndef WIFI_PROVISIONING_HTTP_H_
#define WIFI_PROVISIONING_HTTP_H_

/* Includes ---------------------------------------------------------------- */

#include "esp_err.h"
#include "esp_timer.h"
#include "wifi_types.h"

/* Credential buffers ------------------------------------------------------ */

/**
 * @brief SSID written by POST /configure, read by the deferred credential
 *        timer callback in wifi_provisioning.c.
 */
extern char wifi_prov_ssid[WIFI_SSID_LEN];

/**
 * @brief Password written by POST /configure, read by the deferred credential
 *        timer callback in wifi_provisioning.c.
 */
extern char wifi_prov_pass[WIFI_PASS_LEN];

/* Exposed API ------------------------------------------------------------- */

/**
 * @brief Start the provisioning HTTP server.
 *
 * @param cred_timer  Pointer to a started one-shot timer that applies
 *                    credentials after the HTTP response is sent.
 *                    The timer is armed by the POST /configure handler
 *                    500 ms after a valid request is received.
 * @return ESP_OK on success, otherwise an esp_err_t from httpd_start().
 */
esp_err_t wifi_provisioning_http_start(esp_timer_handle_t cred_timer);

/**
 * @brief Stop the provisioning HTTP server and release its handle.
 * @return ESP_OK unconditionally (mirrors wifi_provisioning_stop style).
 */
esp_err_t wifi_provisioning_http_stop(void);

#endif /* WIFI_PROVISIONING_HTTP_H_ */

/**** END OF FILE ****/
