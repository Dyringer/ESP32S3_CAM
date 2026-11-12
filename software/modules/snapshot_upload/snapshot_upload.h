/**
 ******************************************************************************
 * @file    snapshot_upload.h
 * @author  Daniel Dyringer
 * @date    2026-10-04
 * @brief   Snapshot uploader — periodically POSTs the latest camera frame
 *          to a remote HTTP endpoint.
 *
 *          Request: POST <url>, Content-Type: image/jpeg, raw JPEG body,
 *          optional HTTP Basic Auth (sent pre-emptively).
 *
 *          Settings live in snapshot_upload_settings; every setter saves
 *          right away (data_manager_save() covers all pending changes).
 *          Requires data_manager_init() before snapshot_upload_init().
 *          Uploads are skipped while WiFi is not connected.
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

#define SNAPSHOT_UPLOAD_URL_MAX             192u
#define SNAPSHOT_UPLOAD_CRED_MAX            64u
#define SNAPSHOT_UPLOAD_RESULT_MAX          48u

#define SNAPSHOT_UPLOAD_INTERVAL_DEFAULT_S  10u
#define SNAPSHOT_UPLOAD_INTERVAL_MIN_S      2u
#define SNAPSHOT_UPLOAD_INTERVAL_MAX_S      3600u

// clang-format on

/* Types ------------------------------------------------------------------- */

typedef struct snapshot_upload_status_s
{
    bool enabled;
    char url[SNAPSHOT_UPLOAD_URL_MAX];
    char user[SNAPSHOT_UPLOAD_CRED_MAX]; /**< Empty = no auth.         */
    bool has_password;                   /**< Password itself is never exposed. */
    uint32_t interval_s;

    /* Statistics since boot */
    uint32_t ok_count;
    uint32_t fail_count;
    int64_t last_attempt_us; /**< esp_timer time, 0 = never. */
    int64_t last_ok_us;      /**< esp_timer time, 0 = never. */
    size_t last_size;        /**< Bytes of the last attempt. */
    uint32_t last_duration_ms;
    char last_result[SNAPSHOT_UPLOAD_RESULT_MAX]; /**< "ok", "HTTP 401", ... */
} snapshot_upload_status_t;

/* Exposed API ------------------------------------------------------------- */

esp_err_t snapshot_upload_init(void);

esp_err_t snapshot_upload_set_url(const char* url);
esp_err_t snapshot_upload_set_auth(const char* user, const char* password);
esp_err_t snapshot_upload_set_interval(uint32_t interval_s);
esp_err_t snapshot_upload_set_enabled(bool enabled);

void snapshot_upload_get_status(snapshot_upload_status_t* status);

#endif /* SNAPSHOT_UPLOAD_H_ */

/**** END OF FILE ****/
