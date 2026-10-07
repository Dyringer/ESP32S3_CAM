/**
 ******************************************************************************
 * @file    wifi_sta.h
 * @author  Daniel Dyringer
 * @date    2026-02-17
 * @brief   WiFi STA driver — connect, disconnect, scan.
 ******************************************************************************
 */

/* Define to prevent recursive inclusion ----------------------------------- */
#ifndef WIFI_STA_H_
#define WIFI_STA_H_

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

/**
 * @brief Create and register the STA netif. Must be called before esp_wifi_start().
 */
esp_err_t net_wifi_sta_init(esp_netif_t** netif_out);

/**
 * @brief Set STA config and issue esp_wifi_connect().
 *        Result is reported via WIFI_EVENT_STA_CONNECTED / IP_EVENT_STA_GOT_IP.
 */
esp_err_t net_wifi_sta_connect(const char* ssid, const char* password);

/** @brief Disconnect from the current AP. */
esp_err_t net_wifi_sta_disconnect(void);

/**
 * @brief Blocking scan. Allocates *out_records — caller must free
 *        with net_wifi_sta_scan_free().
 */
esp_err_t net_wifi_sta_scan(wifi_ap_record_t** out_records, uint16_t* out_count);

void net_wifi_sta_scan_free(wifi_ap_record_t* records);

#endif /* WIFI_STA_H_ */

/**** END OF FILE ****/