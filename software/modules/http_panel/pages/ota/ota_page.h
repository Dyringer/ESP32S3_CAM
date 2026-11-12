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

/* Constants and macros ---------------------------------------------------- */
// clang-format off


// clang-format on

/* Types ------------------------------------------------------------------- */


/* Exposed API ------------------------------------------------------------- */

esp_err_t ota_page_init(void);

#endif /* OTA_PAGE_H_ */

/**** END OF FILE ****/
