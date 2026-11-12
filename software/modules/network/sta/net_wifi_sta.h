/**
 ******************************************************************************
 * @file    net_wifi_sta.h
 * @author  Daniel Dyringer
 * @date    2026-02-17
 * @brief   WiFi STA driver — connect, disconnect, scan.
 ******************************************************************************
 */

/* Define to prevent recursive inclusion ----------------------------------- */
#ifndef NET_WIFI_STA_H_
#define NET_WIFI_STA_H_

/* Includes ---------------------------------------------------------------- */
#include <stdint.h>

#include "esp_err.h"
#include "esp_netif.h"
#include "esp_wifi.h"

/* Constants and macros ---------------------------------------------------- */
// clang-format off


// clang-format on

/* Types ------------------------------------------------------------------- */


/* Exposed API ------------------------------------------------------------- */

esp_err_t net_wifi_sta_init(esp_netif_t** netif);
esp_err_t net_wifi_sta_connect(const char* ssid, const char* password);
esp_err_t net_wifi_sta_disconnect(void);

esp_err_t net_wifi_sta_scan(wifi_ap_record_t** records, uint16_t* count);
void net_wifi_sta_scan_free(wifi_ap_record_t* records);

#endif /* NET_WIFI_STA_H_ */

/**** END OF FILE ****/
