/**
 ******************************************************************************
 * @file    ota.h
 * @author  Daniel Dyringer
 * @date    2026-08-13
 * @brief   OTA core — push-style firmware update wrapper around esp_ota_ops.
 *
 *          Knows nothing about HTTP servers. Bytes are handed in by
 *          a caller (the http_panel OTA page's POST handler) rather than
 *          pulled from a URL, so this wraps esp_ota_ops.h directly instead
 *          of esp_https_ota.
 *
 *          State machine: IDLE -> RECEIVING -> VALIDATING -> APPLYING ->
 *          (PENDING_REBOOT | FAILED) -> IDLE (on abort or after a fresh
 *          ota_begin()).
 *
 *          Progress and terminal state are posted on OTA_EVENTS so consumers
 *          (e.g. the http_panel OTA page WS push) can subscribe
 *          independently.
 *
 *          Does not reboot automatically on success — the caller decides
 *          when, so a client can observe "100%" before the device drops off
 *          the network.
 ******************************************************************************
 */

/* Define to prevent recursive inclusion ----------------------------------- */
#ifndef OTA_H_
#define OTA_H_

/* Includes ------------------------------------------------------------------ */

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "esp_event.h"

/* Events -------------------------------------------------------------------- */

/**
 * @brief Event base for ota events posted to the default event loop.
 */
ESP_EVENT_DECLARE_BASE(OTA_EVENTS);

/**
 * @brief Events posted by ota to the default event loop.
 *        event_data points to an ota_status_t snapshot for all three.
 */
typedef enum
{
    OTA_EVENT_PROGRESS, /**< Posted after each ota_write() call.            */
    OTA_EVENT_DONE,     /**< Posted once, after a successful ota_finish().  */
    OTA_EVENT_FAILED,   /**< Posted once, on any error or ota_abort().      */
} ota_event_t;

/* Types ----------------------------------------------------------------------- */

typedef enum
{
    OTA_STATE_IDLE,
    OTA_STATE_RECEIVING,
    OTA_STATE_VALIDATING,
    OTA_STATE_APPLYING,
    OTA_STATE_PENDING_REBOOT,
    OTA_STATE_FAILED,
} ota_state_t;

typedef struct
{
    ota_state_t state;
    size_t      bytes_written;
    size_t      bytes_expected; /**< From Content-Length; 0 if unknown.     */
    esp_err_t   last_error;
} ota_status_t;

/* Exposed API ----------------------------------------------------------------- */

/**
 * @brief Initialise the OTA core: register the OTA_EVENTS base, reset state
 *        to IDLE. Call once, after the default event loop exists.
 */
esp_err_t ota_init(void);

/**
 * @brief Begin an OTA update: finds the next free OTA partition and calls
 *        esp_ota_begin() on it.
 *
 * @param image_size_hint  Expected total size in bytes (e.g. from the HTTP
 *                          Content-Length header), or 0/OTA_SIZE_UNKNOWN if
 *                          not known.
 *
 * @return ESP_OK on success. ESP_ERR_INVALID_STATE if an OTA is already in
 *         progress (state is not IDLE or FAILED).
 */
esp_err_t ota_begin(size_t image_size_hint);

/**
 * @brief Write the next chunk of firmware data.
 *
 *        The first call validates the image header magic byte before
 *        committing to esp_ota_write(), so a bad file fails in milliseconds
 *        rather than after streaming the whole thing.
 *
 * @param data  Chunk buffer.
 * @param len   Chunk length in bytes.
 *
 * @return ESP_OK on success. ESP_ERR_INVALID_STATE if not RECEIVING.
 */
esp_err_t ota_write(const uint8_t *data, size_t len);

/**
 * @brief Finish the update: esp_ota_end() (validates the image) then
 *        esp_ota_set_boot_partition(). Does not reboot.
 *
 * @return ESP_OK — state becomes PENDING_REBOOT. Any error — state becomes
 *         FAILED and the partial write is not booted.
 */
esp_err_t ota_finish(void);

/**
 * @brief Abort an in-progress update via esp_ota_abort() and return to IDLE.
 *
 *        No-op if no session is open (state is already IDLE or FAILED) —
 *        in the FAILED case, the failing call already set last_error and
 *        notified OTA_EVENTS; calling this would erase that record instead
 *        of adding new information.
 */
esp_err_t ota_abort(void);

/**
 * @brief Copy the current status snapshot into @p out.
 */
void ota_get_status(ota_status_t *out);

/**
 * @brief Return the version string of the currently running app image
 *        (esp_app_desc_t.version, derived from PROJECT_VER / git describe
 *        at build time).
 */
const char *ota_get_running_version(void);

#endif /* OTA_H_ */

/**** END OF FILE ****/
