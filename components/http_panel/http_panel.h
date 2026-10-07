/**
 ******************************************************************************
 * @file    http_panel.h
 * @author  Daniel Dyringer
 * @date    2026-03-09
 * @brief   HTTP panel — public API.
 *
 *          Single entry point for the entire http_panel component.
 *          After http_panel_init() the following pages are available once
 *          WiFi connects:
 *
 *            GET /            Dashboard (device status and configuration)
 *            GET /ws          Dashboard WebSocket (periodic JSON status push)
 *            GET /console     Terminal page (ESP log stream)
 *            GET /console/ws  Console WebSocket (one line per log entry)
 *            GET /ota         OTA firmware upload page
 *            GET /ota/ws      OTA WebSocket (progress push)
 *            POST /ota/upload Raw firmware body upload
 *            GET /ota/version Running firmware version (JSON)
 *            GET /camera          Camera page (live view, snapshot, settings)
 *            GET /camera/stream   MJPEG stream
 *            GET /camera/capture  Single JPEG still
 *            GET /camera/status   Camera settings + ranges (JSON)
 *            GET /camera/control  Apply one setting (?var=<name>&val=<int>)
 *            POST /camera/settings  ?action=save (to NVS) | defaults
 ******************************************************************************
 */

/* Define to prevent recursive inclusion ----------------------------------- */
#ifndef HTTP_PANEL_H_
#define HTTP_PANEL_H_

/* Includes ---------------------------------------------------------------- */

#include "esp_err.h"

/* Exposed API ------------------------------------------------------------- */

/**
 * @brief Initialise the HTTP panel.
 *
 *        - Creates the server core (WS client table, monitor task).
 *        - Initialises the dashboard page (registers URIs, starts the
 *          periodic status push timer).
 *        - Initialises the console page (installs vprintf hook, spawns push
 *          task, registers URIs).
 *
 *        Call once after wifi_connection_init(), ota_init() and
 *        camera_init().
 *        The HTTP server starts automatically when WiFi connects.
 *
 * @return ESP_OK on success.
 */
esp_err_t http_panel_init(void);

#endif /* HTTP_PANEL_H_ */

/**** END OF FILE ****/
