/**
 ******************************************************************************
 * @file    wifi_types.h
 * @author  Daniel Dyringer
 * @date    2026-02-22
 * @brief   Shared type and constant definitions for the network component.
 ******************************************************************************
 */

/* Define to prevent recursive inclusion ----------------------------------- */
#ifndef WIFI_TYPES_H_
#define WIFI_TYPES_H_

/* Includes ---------------------------------------------------------------- */


/* Constants and macros ---------------------------------------------------- */
// clang-format off

/* String buffers, NUL included: SSID up to 32 chars, passphrase up to 63 or
 * a raw 64-hex-digit PSK. wifi_config_t holds them unterminated. */
#define WIFI_TYPES_SSID_LEN         33u
#define WIFI_TYPES_PASS_LEN         65u
#define WIFI_TYPES_MAC_LEN          6u

/* esp_wifi_set_max_tx_power() units of 0.25 dBm */
#define WIFI_TYPES_SCAN_TX_POWER    80
#define WIFI_TYPES_MAX_TX_POWER     84

// clang-format on

/* Types ------------------------------------------------------------------- */


/* Exposed API ------------------------------------------------------------- */


#endif /* WIFI_TYPES_H_ */

/**** END OF FILE ****/
