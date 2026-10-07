/**
 ******************************************************************************
 * @file    camera_page.h
 * @author  Daniel Dyringer
 * @date    2026-09-30
 * @brief   Camera page — live MJPEG view, snapshot and sensor settings.
 *
 * @note    Private to the http_panel component.
 ******************************************************************************
 */

/* Define to prevent recursive inclusion ----------------------------------- */
#ifndef CAMERA_PAGE_H_
#define CAMERA_PAGE_H_

/* Includes ---------------------------------------------------------------- */

#include "esp_err.h"

/* Exposed API ------------------------------------------------------------- */

/**
 * @brief Initialise the camera page.
 *        Registers URI handlers and the server lifecycle callback, and
 *        spawns the stream task. Call from http_panel_init().
 */
esp_err_t camera_page_init(void);

#endif /* CAMERA_PAGE_H_ */

/**** END OF FILE ****/
