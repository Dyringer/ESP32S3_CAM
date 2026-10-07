/**
 ******************************************************************************
 * @file    ota.c
 * @author  Daniel Dyringer
 * @date    2026-08-13
 * @brief   OTA core — push-style firmware update wrapper around esp_ota_ops.
 ******************************************************************************
 */


/* Includes ---------------------------------------------------------------- */
#include "ota.h"

#include <string.h>

#include "esp_app_desc.h"
#include "esp_app_format.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_timer.h"

/* Constants and macros ------------------------------------------------------ */
// clang-format off

ESP_EVENT_DEFINE_BASE(OTA_EVENTS);

/**
 * @brief Minimum interval between OTA_EVENT_PROGRESS notifications in
 *        microseconds. ota_write() is called once per upload chunk (4 KB —
 *        see OTA_PAGE_UPLOAD_CHUNK), i.e. hundreds of times per image; without
 *        this cap every chunk would post an event (a copy of ota_status_t
 *        through the default event loop) that no consumer needs at that
 *        rate. DONE and FAILED always post immediately (terminal, at most
 *        once each).
 */
#define OTA_PROGRESS_MIN_INTERVAL_US  200000LL  /* 200 ms = 5 Hz */

// clang-format on

/* Private variables --------------------------------------------------------- */

static const char *ota_tag = "ota";

static ota_status_t          s_status = { .state = OTA_STATE_IDLE };
static esp_ota_handle_t       s_handle;
static const esp_partition_t *s_target_partition;

/**
 * @brief True once the first ota_write() chunk has been checked for a valid
 *        image header magic byte. Reset in ota_begin().
 */
static bool s_header_checked;

/** @brief Timestamp of the last OTA_EVENT_PROGRESS notification (us). */
static int64_t s_last_progress_us = 0;

/* Private functions ----------------------------------------------------------- */

static void ota_notify(ota_event_t event)
{
    esp_err_t error_code = esp_event_post(OTA_EVENTS, event, &s_status,
                                          sizeof(s_status), 0);

    if (error_code != ESP_OK)
    {
        ESP_LOGW(ota_tag, "esp_event_post failed: %s", esp_err_to_name(error_code));
    }
}

/**
 * @brief Notify of a progress update, rate-capped to
 *        OTA_PROGRESS_MIN_INTERVAL_US — see the macro's doc comment for why.
 */
static void ota_notify_progress(void)
{
    const int64_t now = esp_timer_get_time();

    if ((now - s_last_progress_us) < OTA_PROGRESS_MIN_INTERVAL_US)
    {
        return;
    }

    s_last_progress_us = now;
    ota_notify(OTA_EVENT_PROGRESS);
}

/* Exposed API ----------------------------------------------------------------- */

esp_err_t ota_init(void)
{
    s_status.state          = OTA_STATE_IDLE;
    s_status.bytes_written  = 0u;
    s_status.bytes_expected = 0u;
    s_status.last_error     = ESP_OK;

    ESP_LOGI(ota_tag, "Initialised — running version %s",
             ota_get_running_version());

    return ESP_OK;
}

esp_err_t ota_begin(size_t image_size_hint)
{
    esp_err_t error_code = ESP_OK;

    do
    {
        if (s_status.state != OTA_STATE_IDLE && s_status.state != OTA_STATE_FAILED)
        {
            /* Blocks RECEIVING/VALIDATING/APPLYING (an update is actively in
             * flight) and, importantly, PENDING_REBOOT too: esp_ota_set_boot_
             * partition() has already pointed the next boot at the partition
             * esp_ota_get_next_update_partition() would hand out again here
             * (esp_ota_get_running_partition() still reports the OLD partition
             * until the reboot actually happens), so without this a second
             * upload would silently overwrite the already-validated, staged
             * image before the user ever reboots into it. */
            ESP_LOGW(ota_tag, "ota_begin() while state=%d", s_status.state);
            error_code = ESP_ERR_INVALID_STATE;
            break;
        }

        s_target_partition = esp_ota_get_next_update_partition(NULL);

        if (!s_target_partition)
        {
            ESP_LOGE(ota_tag, "No free OTA partition found");
            s_status.state      = OTA_STATE_FAILED;
            s_status.last_error = ESP_ERR_NOT_FOUND;
            ota_notify(OTA_EVENT_FAILED);
            error_code = ESP_ERR_NOT_FOUND;
            break;
        }

        /* OTA_WITH_SEQUENTIAL_WRITES defers erasing to esp_ota_write(), which
         * erases only the sectors each chunk actually touches. The alternative
         * (passing the real image size) makes esp_ota_begin() erase the whole
         * ~1.4 MB partition in one esp_partition_erase_range() call up front —
         * that single call disables the flash cache and stalls both cores via
         * an IPC block long enough to trip the IDLE task watchdog. Spreading
         * the erase across the write loop keeps each blocking window small. */
        error_code =
            esp_ota_begin(s_target_partition, OTA_WITH_SEQUENTIAL_WRITES, &s_handle);

        if (error_code != ESP_OK)
        {
            ESP_LOGE(ota_tag, "esp_ota_begin failed: %s", esp_err_to_name(error_code));
            s_status.state      = OTA_STATE_FAILED;
            s_status.last_error = error_code;
            ota_notify(OTA_EVENT_FAILED);
            break;
        }

        s_status.state          = OTA_STATE_RECEIVING;
        s_status.bytes_written  = 0u;
        s_status.bytes_expected = image_size_hint;
        s_status.last_error     = ESP_OK;
        s_header_checked        = false;
        s_last_progress_us      = 0; /* force the first progress notify through */

        ESP_LOGI(ota_tag, "Begin — target partition '%s', expecting %u bytes",
                 s_target_partition->label, (unsigned)image_size_hint);

        ota_notify_progress();
    } while (0);

    return error_code;
}

esp_err_t ota_write(const uint8_t *data, size_t len)
{
    esp_err_t error_code = ESP_OK;

    do
    {
        if (s_status.state != OTA_STATE_RECEIVING)
        {
            error_code = ESP_ERR_INVALID_STATE;
            break;
        }

        if (!s_header_checked)
        {
            s_header_checked = true;

            if (len < sizeof(esp_image_header_t)
                || data[0] != ESP_IMAGE_HEADER_MAGIC)
            {
                ESP_LOGE(ota_tag, "First chunk fails image magic-byte check");
                esp_ota_abort(s_handle);
                s_status.state      = OTA_STATE_FAILED;
                s_status.last_error = ESP_ERR_OTA_VALIDATE_FAILED;
                ota_notify(OTA_EVENT_FAILED);
                error_code = ESP_ERR_OTA_VALIDATE_FAILED;
                break;
            }
        }

        error_code = esp_ota_write(s_handle, data, len);

        if (error_code != ESP_OK)
        {
            ESP_LOGE(ota_tag, "esp_ota_write failed: %s", esp_err_to_name(error_code));
            esp_ota_abort(s_handle);
            s_status.state      = OTA_STATE_FAILED;
            s_status.last_error = error_code;
            ota_notify(OTA_EVENT_FAILED);
            break;
        }

        s_status.bytes_written += len;
        ota_notify_progress();
    } while (0);

    return error_code;
}

esp_err_t ota_finish(void)
{
    esp_err_t error_code = ESP_OK;

    do
    {
        if (s_status.state != OTA_STATE_RECEIVING)
        {
            error_code = ESP_ERR_INVALID_STATE;
            break;
        }

        s_status.state = OTA_STATE_VALIDATING;

        error_code = esp_ota_end(s_handle);

        if (error_code != ESP_OK)
        {
            ESP_LOGE(ota_tag, "esp_ota_end failed: %s", esp_err_to_name(error_code));
            s_status.state      = OTA_STATE_FAILED;
            s_status.last_error = error_code;
            ota_notify(OTA_EVENT_FAILED);
            break;
        }

        s_status.state = OTA_STATE_APPLYING;

        error_code = esp_ota_set_boot_partition(s_target_partition);

        if (error_code != ESP_OK)
        {
            ESP_LOGE(ota_tag, "esp_ota_set_boot_partition failed: %s",
                     esp_err_to_name(error_code));
            s_status.state      = OTA_STATE_FAILED;
            s_status.last_error = error_code;
            ota_notify(OTA_EVENT_FAILED);
            break;
        }

        s_status.state      = OTA_STATE_PENDING_REBOOT;
        s_status.last_error = ESP_OK;

        ESP_LOGI(ota_tag, "Update applied — %u bytes written, reboot needed",
                 (unsigned)s_status.bytes_written);

        ota_notify(OTA_EVENT_DONE);
    } while (0);

    return error_code;
}

esp_err_t ota_abort(void)
{
    esp_err_t error_code = ESP_OK;

    /* Already FAILED (or IDLE with nothing to cancel): the state, error and
     * FAILED notification were already set by whichever ota_write()/
     * ota_finish() check triggered this, or there was never a session to
     * begin with. Re-running the IDLE reset here would erase that error
     * for status callers (ota_shell.c, ota_page.c's WS clients) — so only
     * act when a session is actually still open. */
    if (s_status.state == OTA_STATE_RECEIVING
        || s_status.state == OTA_STATE_VALIDATING)
    {
        error_code = esp_ota_abort(s_handle);

        s_status.state      = OTA_STATE_IDLE;
        s_status.last_error = ESP_OK;

        /* Explicit cancel — notify like every other transition. */
        ota_notify(OTA_EVENT_FAILED);

        ESP_LOGI(ota_tag, "Aborted");
    }

    return error_code;
}

void ota_get_status(ota_status_t *out)
{
    if (out)
    {
        *out = s_status;
    }
}

const char *ota_get_running_version(void)
{
    return esp_app_get_description()->version;
}


/**** END OF FILE ****/
