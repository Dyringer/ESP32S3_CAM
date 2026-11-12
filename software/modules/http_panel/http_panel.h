/**
 ******************************************************************************
 * @file    http_panel.h
 * @author  Daniel Dyringer
 * @date    2026-03-09
 * @brief   HTTP panel — public API.
 *
 *          Call http_panel_init() once after wifi_connection_init(),
 *          ota_init() and camera_init(). The server then starts and stops
 *          automatically with the WiFi connection.
 ******************************************************************************
 */

/* Define to prevent recursive inclusion ----------------------------------- */
#ifndef HTTP_PANEL_H_
#define HTTP_PANEL_H_

/* Includes ---------------------------------------------------------------- */
#include "esp_err.h"

/* Constants and macros ---------------------------------------------------- */
// clang-format off


// clang-format on

/* Types ------------------------------------------------------------------- */


/* Exposed API ------------------------------------------------------------- */

esp_err_t http_panel_init(void);

#endif /* HTTP_PANEL_H_ */

/**** END OF FILE ****/
