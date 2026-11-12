/**
 ******************************************************************************
 * @file    camera_page.h
 * @author  Daniel Dyringer
 * @date    2026-09-30
 * @brief   Camera page — live MJPEG view, snapshot and sensor settings.
 ******************************************************************************
 */

/* Define to prevent recursive inclusion ----------------------------------- */
#ifndef CAMERA_PAGE_H_
#define CAMERA_PAGE_H_

/* Includes ---------------------------------------------------------------- */
#include "esp_err.h"

/* Constants and macros ---------------------------------------------------- */
// clang-format off


// clang-format on

/* Types ------------------------------------------------------------------- */


/* Exposed API ------------------------------------------------------------- */

esp_err_t camera_page_init(void);

#endif /* CAMERA_PAGE_H_ */

/**** END OF FILE ****/
