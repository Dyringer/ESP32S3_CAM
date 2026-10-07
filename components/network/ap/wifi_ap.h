/**
 ******************************************************************************
 * @file    wifi_ap.h
 * @author  Daniel Dyringer
 * @date    2026-02-17
 * @brief   WiFi AP driver — start, stop, DHCP server managed by esp_netif.
 ******************************************************************************
 */

/* Define to prevent recursive inclusion ----------------------------------- */
#ifndef WIFI_AP_H_
#define WIFI_AP_H_

/* Includes ---------------------------------------------------------------- */

#include "esp_err.h"
#include "esp_netif.h"

/* Constants and macros ---------------------------------------------------- */
// clang-format off
#define WIFI_AP_DEFAULT_SSID        "ESP32S3-Config"
#define WIFI_AP_DEFAULT_PASSWORD    ""   /* empty = open network */
#define WIFI_AP_MAX_CONNECTIONS     4
#define WIFI_AP_CHANNEL             1
// clang-format on

/* Types ------------------------------------------------------------------- */


/* Exposed API ------------------------------------------------------------- */

/** @brief Create and register the AP netif. Must be called before esp_wifi_start(). */
esp_err_t wifi_ap_init(esp_netif_t** netif_out);

/**
 * @brief Configure and start AP.
 *        password == NULL or len < 8 → open network.
 */
esp_err_t wifi_ap_start(const char* ssid, const char* password);

/** @brief Hook for future use — driver lifecycle is owned by wifi_connection. */
esp_err_t wifi_ap_stop(void);

#endif /* WIFI_AP_H_ */

/**** END OF FILE ****/