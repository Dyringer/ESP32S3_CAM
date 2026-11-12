/**
 ******************************************************************************
 * @file    wifi_provisioning_http.h
 * @author  Daniel Dyringer
 * @date    2026-03-04
 * @brief   Captive portal HTTP layer — server lifecycle and URI handlers.
 ******************************************************************************
 */

/* Define to prevent recursive inclusion ----------------------------------- */
#ifndef WIFI_PROVISIONING_HTTP_H_
#define WIFI_PROVISIONING_HTTP_H_

/* Includes ---------------------------------------------------------------- */
#include "esp_err.h"

/* Constants and macros ---------------------------------------------------- */
// clang-format off


// clang-format on

/* Types ------------------------------------------------------------------- */


/* Exposed API ------------------------------------------------------------- */

esp_err_t wifi_provisioning_http_start(void);
esp_err_t wifi_provisioning_http_stop(void);

#endif /* WIFI_PROVISIONING_HTTP_H_ */

/**** END OF FILE ****/
