/**
 ******************************************************************************
 * @file    ota_page.h
 * @author  Daniel Dyringer
 * @date    2026-08-13
 * @brief   OTA page — drag-and-drop firmware upload with progress.
 ******************************************************************************
 */

/* Define to prevent recursive inclusion ----------------------------------- */
#ifndef OTA_PAGE_H_
#define OTA_PAGE_H_

/* Includes ---------------------------------------------------------------- */

#include "esp_err.h"

/* Exposed API ------------------------------------------------------------- */

/**
 * @brief Initialise the OTA page: registers GET /ota, GET /ota/ws,
 *        POST /ota/upload, and subscribes to OTA_EVENTS.
 *
 * @return ESP_OK on success.
 */
esp_err_t ota_page_init(void);

#endif /* OTA_PAGE_H_ */

/**** END OF FILE ****/
