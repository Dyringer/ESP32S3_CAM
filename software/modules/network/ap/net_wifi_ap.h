/**
 ******************************************************************************
 * @file    net_wifi_ap.h
 * @author  Daniel Dyringer
 * @date    2026-02-17
 * @brief   WiFi AP driver — start, stop, DHCP server managed by esp_netif.
 ******************************************************************************
 */

/* Define to prevent recursive inclusion ----------------------------------- */
#ifndef NET_WIFI_AP_H_
#define NET_WIFI_AP_H_

/* Includes ---------------------------------------------------------------- */
#include "esp_err.h"
#include "esp_netif.h"

/* Constants and macros ---------------------------------------------------- */
// clang-format off

#define NET_WIFI_AP_DEFAULT_SSID        "ESP32S3-Config"
#define NET_WIFI_AP_DEFAULT_PASSWORD    ""   /* empty = open network */
#define NET_WIFI_AP_MAX_CONNECTIONS     4u
#define NET_WIFI_AP_CHANNEL             1u

// clang-format on

/* Types ------------------------------------------------------------------- */


/* Exposed API ------------------------------------------------------------- */

esp_err_t net_wifi_ap_init(esp_netif_t** netif);
esp_err_t net_wifi_ap_start(const char* ssid, const char* password);
esp_err_t net_wifi_ap_stop(void);

#endif /* NET_WIFI_AP_H_ */

/**** END OF FILE ****/
