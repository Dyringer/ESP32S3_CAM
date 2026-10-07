/**
 ******************************************************************************
 * @file    snapshot_upload.h
 * @author  Daniel Dyringer
 * @date    2026-10-04
 * @brief   Snapshot uploader — periodically POSTs the latest camera frame
 *          to a remote HTTP endpoint (the mikrus VPS /parking page).
 *
 *          A dedicated task on Core 1 wakes every interval, copies the
 *          latest JPEG frame into PSRAM, releases the driver buffer and only
 *          then uploads — a slow uplink never holds a frame buffer, so the
 *          live stream on the http_panel camera page keeps running.
 *
 *          Request: POST <url>, Content-Type: image/jpeg, raw JPEG body,
 *          optional HTTP Basic Auth (sent pre-emptively).
 *
 *          Push only: uploads run on the device's own timer, nothing remote
 *          can trigger one. The server just keeps the newest image.
 *
 *          Configuration (URL, credentials, interval, on/off) lives in NVS
 *          and every setter persists immediately. Uploads are skipped while
 *          WiFi is not connected.
 ******************************************************************************
 */

/* Define to prevent recursive inclusion ----------------------------------- */
#ifndef SNAPSHOT_UPLOAD_H_
#define SNAPSHOT_UPLOAD_H_

/* Includes ---------------------------------------------------------------- */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

/* Constants and macros ---------------------------------------------------- */
// clang-format off

#define SNAPSHOT_UPLOAD_URL_MAX             192u  /**< Incl. terminator.   */
#define SNAPSHOT_UPLOAD_CRED_MAX            64u   /**< Incl. terminator.   */
#define SNAPSHOT_UPLOAD_RESULT_MAX          48u   /**< Incl. terminator.   */

#define SNAPSHOT_UPLOAD_INTERVAL_DEFAULT_S  10u
#define SNAPSHOT_UPLOAD_INTERVAL_MIN_S      2u
#define SNAPSHOT_UPLOAD_INTERVAL_MAX_S      3600u

// clang-format on

/* Types ------------------------------------------------------------------- */

/** @brief Configuration and statistics snapshot. */
typedef struct
{
    /* Configuration */
    bool     enabled;
    char     url[SNAPSHOT_UPLOAD_URL_MAX];
    char     user[SNAPSHOT_UPLOAD_CRED_MAX];    /**< Empty = no auth.         */
    bool     has_password;                      /**< Password itself is never exposed. */
    uint32_t interval_s;

    /* Statistics since boot */
    uint32_t ok_count;
    uint32_t fail_count;
    int64_t  last_attempt_us;                   /**< esp_timer time, 0 = never. */
    int64_t  last_ok_us;                        /**< esp_timer time, 0 = never. */
    size_t   last_size;                         /**< Bytes of the last attempt. */
    uint32_t last_duration_ms;
    char     last_result[SNAPSHOT_UPLOAD_RESULT_MAX]; /**< "ok", "HTTP 401", ... */
} snapshot_upload_status_t;

/* Exposed API ------------------------------------------------------------- */

/**
 * @brief Load the saved configuration from NVS and start the upload task.
 *        Call after nvs_flash_init(), camera_init() and wifi_connection_init().
 */
esp_err_t snapshot_upload_init(void);

/**
 * @brief Set the upload endpoint, e.g.
 *        "http://frog03.mikr.us:30483/parking/api/upload".
 *
 * @return ESP_OK, or ESP_ERR_INVALID_ARG if it is not an http(s):// URL or
 *         does not fit SNAPSHOT_UPLOAD_URL_MAX.
 */
esp_err_t snapshot_upload_set_url(const char *url);

/**
 * @brief Set the HTTP Basic Auth credentials. NULL or empty @p user
 *        disables auth.
 *
 * @return ESP_OK, or ESP_ERR_INVALID_ARG if either does not fit
 *         SNAPSHOT_UPLOAD_CRED_MAX.
 */
esp_err_t snapshot_upload_set_auth(const char *user, const char *password);

/**
 * @brief Set the upload period in seconds.
 *
 * @return ESP_OK, or ESP_ERR_INVALID_ARG outside
 *         SNAPSHOT_UPLOAD_INTERVAL_MIN_S..SNAPSHOT_UPLOAD_INTERVAL_MAX_S.
 */
esp_err_t snapshot_upload_set_interval(uint32_t interval_s);

/**
 * @brief Start or stop periodic uploads. Enabling uploads one frame right
 *        away instead of waiting a full interval.
 *
 * @return ESP_OK, or ESP_ERR_INVALID_STATE when enabling without a URL.
 */
esp_err_t snapshot_upload_set_enabled(bool enabled);

/** @brief Copy the current configuration and statistics into @p out. */
void snapshot_upload_get_status(snapshot_upload_status_t *out);

#endif /* SNAPSHOT_UPLOAD_H_ */

/**** END OF FILE ****/
