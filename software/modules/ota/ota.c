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

#include <stdbool.h>
#include <string.h>

#include "esp_app_format.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_timer.h"
#include "utils.h"
#include "version.h"


/* Constants and macros ---------------------------------------------------- */
// clang-format off

/* ota_write() runs once per 4 KB upload chunk, so PROGRESS events are
 * rate-capped. DONE and FAILED always post. */
#define OTA_PROGRESS_MIN_INTERVAL_US  200000LL

#define OTA_PROGRESS_LOG_STEP_PERCENT 10u

/* Flash block size. In sequential mode esp_ota_write() erases exactly the
 * range it writes, so a 64 KB aligned write gets one block erase instead of 16
 * sector erases (typ. 150 ms vs 720 ms on the W25Q128). */
#define OTA_WRITE_BLOCK_SIZE          (64u * 1024u)

// clang-format on

/* Private types ----------------------------------------------------------- */


/* Private variables ------------------------------------------------------- */

static const char* ota_tag = "ota";

ESP_EVENT_DEFINE_BASE(OTA_EVENTS);

static ota_status_t ota_status                     = {.state = OTA_STATE_IDLE};
static esp_ota_handle_t ota_handle                 = 0u;
static const esp_partition_t* ota_target_partition = NULL;
static bool ota_header_checked                     = false;
static int64_t ota_last_progress_us                = 0;
static int64_t ota_begin_us                        = 0;
static uint32_t ota_next_log_percent               = 0u;

/* PSRAM, allocated by the first update and kept for the next one. */
static uint8_t* ota_block_buffer = NULL;
static size_t ota_block_length   = 0u;


/* Private functions ------------------------------------------------------- */

static const char* ota_state_name(ota_state_t state)
{
    const char* name = NULL;

    switch (state)
    {
        case OTA_STATE_IDLE:
        {
            name = "idle";
        }
        break;
        case OTA_STATE_RECEIVING:
        {
            name = "receiving";
        }
        break;
        case OTA_STATE_VALIDATING:
        {
            name = "validating";
        }
        break;
        case OTA_STATE_APPLYING:
        {
            name = "applying";
        }
        break;
        case OTA_STATE_PENDING_REBOOT:
        {
            name = "pending_reboot";
        }
        break;
        case OTA_STATE_FAILED:
        {
            name = "failed";
        }
        break;
        default:
        {
            name = "unknown";
        }
        break;
    }

    return name;
}

/* ---- Logging ---- */

/* Integer tenths: the log format avoids float printf. */
static uint32_t ota_elapsed_tenths_s(void)
{
    return (uint32_t)((esp_timer_get_time() - ota_begin_us) / 100000LL);
}

static uint32_t ota_rate_tenths_kbps(void)
{
    const int64_t elapsed_us = esp_timer_get_time() - ota_begin_us;
    uint32_t rate            = 0u;

    if (elapsed_us > 0)
    {
        rate = (uint32_t)(((uint64_t)ota_status.bytes_written * 10000000ull)
                          / (1024ull * (uint64_t)elapsed_us));
    }

    return rate;
}

static void ota_log_progress(void)
{
    if (ota_status.bytes_expected > 0u)
    {
        const uint32_t percent = (uint32_t)(((uint64_t)ota_status.bytes_written * 100u)
                                            / ota_status.bytes_expected);

        if (percent >= ota_next_log_percent)
        {
            const uint32_t rate    = ota_rate_tenths_kbps();
            const uint32_t elapsed = ota_elapsed_tenths_s();

            ESP_LOGI(
                ota_tag,
                "%lu%% (%u / %u bytes), %lu.%lu KB/s, %lu.%lu s",
                (unsigned long)percent,
                (unsigned)ota_status.bytes_written,
                (unsigned)ota_status.bytes_expected,
                (unsigned long)(rate / 10u),
                (unsigned long)(rate % 10u),
                (unsigned long)(elapsed / 10u),
                (unsigned long)(elapsed % 10u));

            ota_next_log_percent = ((percent / OTA_PROGRESS_LOG_STEP_PERCENT) + 1u)
                                   * OTA_PROGRESS_LOG_STEP_PERCENT;
        }
    }
}

/* ---- Events ---- */

static void ota_notify(ota_event_t event)
{
    const esp_err_t error_code =
        esp_event_post(OTA_EVENTS, (int32_t)event, &ota_status, sizeof(ota_status), 0u);

    if (error_code != ESP_OK)
    {
        ESP_LOGW(ota_tag, "esp_event_post failed: %s", esp_err_to_name(error_code));
    }
}

static void ota_notify_progress(void)
{
    const int64_t now = esp_timer_get_time();

    if ((now - ota_last_progress_us) >= OTA_PROGRESS_MIN_INTERVAL_US)
    {
        ota_last_progress_us = now;
        ota_notify(OTA_EVENT_PROGRESS);
    }
}

/* ---- Block buffer ---- */

static esp_err_t ota_flush_block(void)
{
    esp_err_t error_code = ESP_OK;

    if (ota_block_length > 0u)
    {
        error_code       = esp_ota_write(ota_handle, ota_block_buffer, ota_block_length);
        ota_block_length = 0u;
    }

    return error_code;
}

static esp_err_t ota_buffer_data(const uint8_t* data, size_t length)
{
    esp_err_t error_code = ESP_OK;
    size_t offset        = 0u;

    while ((error_code == ESP_OK) && (offset < length))
    {
        const size_t chunk = MIN(length - offset, OTA_WRITE_BLOCK_SIZE - ota_block_length);

        memcpy(&ota_block_buffer[ota_block_length], &data[offset], chunk);
        ota_block_length += chunk;
        offset += chunk;

        if (ota_block_length == OTA_WRITE_BLOCK_SIZE)
        {
            error_code = ota_flush_block();
        }
    }

    return error_code;
}


/* Exposed API ------------------------------------------------------------- */

esp_err_t ota_init(void)
{
    ota_status.state          = OTA_STATE_IDLE;
    ota_status.bytes_written  = 0u;
    ota_status.bytes_expected = 0u;
    ota_status.last_error     = ESP_OK;

    ESP_LOGI(ota_tag, "Initialised — running version %s", ota_get_running_version());

    return ESP_OK;
}

esp_err_t ota_begin(size_t image_size_hint)
{
    esp_err_t error_code = ESP_OK;

    do
    {
        if ((ota_status.state != OTA_STATE_IDLE) && (ota_status.state != OTA_STATE_FAILED))
        {
            /* Also blocks PENDING_REBOOT: until the reboot, the staged
             * partition is still the "next update partition", so a second
             * upload would overwrite it. */
            const char* const hint =
                (ota_status.state == OTA_STATE_PENDING_REBOOT) ? ", reboot first" : "";

            ESP_LOGW(
                ota_tag,
                "Update rejected: state is %s%s",
                ota_state_name(ota_status.state),
                hint);
            error_code = ESP_ERR_INVALID_STATE;
            break;
        }

        ota_target_partition = esp_ota_get_next_update_partition(NULL);

        if (ota_target_partition == NULL)
        {
            ESP_LOGE(ota_tag, "No free OTA partition found");
            ota_status.state      = OTA_STATE_FAILED;
            ota_status.last_error = ESP_ERR_NOT_FOUND;
            ota_notify(OTA_EVENT_FAILED);
            error_code = ESP_ERR_NOT_FOUND;
            break;
        }

        if (ota_block_buffer == NULL)
        {
            ota_block_buffer = heap_caps_malloc(OTA_WRITE_BLOCK_SIZE, MALLOC_CAP_SPIRAM);
        }

        if (ota_block_buffer == NULL)
        {
            ESP_LOGE(ota_tag, "No memory for the write buffer");
            ota_status.state      = OTA_STATE_FAILED;
            ota_status.last_error = ESP_ERR_NO_MEM;
            ota_notify(OTA_EVENT_FAILED);
            error_code = ESP_ERR_NO_MEM;
            break;
        }

        /* Sequential writes erase only what each write covers; erasing the whole
         * partition up front stalls both cores long enough to trip the IDLE task
         * watchdog. OTA_WRITE_BLOCK_SIZE writes still get block erases. */
        error_code =
            esp_ota_begin(ota_target_partition, OTA_WITH_SEQUENTIAL_WRITES, &ota_handle);

        if (error_code != ESP_OK)
        {
            ESP_LOGE(ota_tag, "esp_ota_begin failed: %s", esp_err_to_name(error_code));
            ota_status.state      = OTA_STATE_FAILED;
            ota_status.last_error = error_code;
            ota_notify(OTA_EVENT_FAILED);
            break;
        }

        ota_status.state          = OTA_STATE_RECEIVING;
        ota_status.bytes_written  = 0u;
        ota_status.bytes_expected = image_size_hint;
        ota_status.last_error     = ESP_OK;
        ota_header_checked        = false;
        ota_block_length          = 0u;
        ota_last_progress_us      = 0; /* force the first progress notify through */
        ota_begin_us              = esp_timer_get_time();
        ota_next_log_percent      = OTA_PROGRESS_LOG_STEP_PERCENT;

        ESP_LOGI(
            ota_tag,
            "Begin — target partition '%s', expecting %u bytes",
            ota_target_partition->label,
            (unsigned)image_size_hint);

        ota_notify_progress();
    } while (0);

    return error_code;
}

esp_err_t ota_write(const uint8_t* data, size_t length)
{
    esp_err_t error_code = ESP_OK;

    do
    {
        if (ota_status.state != OTA_STATE_RECEIVING)
        {
            error_code = ESP_ERR_INVALID_STATE;
            break;
        }

        if (!ota_header_checked)
        {
            ota_header_checked = true;

            if ((length < sizeof(esp_image_header_t))
                || (data[0] != ESP_IMAGE_HEADER_MAGIC))
            {
                ESP_LOGE(ota_tag, "First chunk fails image magic-byte check");
                esp_ota_abort(ota_handle);
                ota_status.state      = OTA_STATE_FAILED;
                ota_status.last_error = ESP_ERR_OTA_VALIDATE_FAILED;
                ota_notify(OTA_EVENT_FAILED);
                error_code = ESP_ERR_OTA_VALIDATE_FAILED;
                break;
            }
        }

        error_code = ota_buffer_data(data, length);

        if (error_code != ESP_OK)
        {
            ESP_LOGE(ota_tag, "esp_ota_write failed: %s", esp_err_to_name(error_code));
            esp_ota_abort(ota_handle);
            ota_status.state      = OTA_STATE_FAILED;
            ota_status.last_error = error_code;
            ota_notify(OTA_EVENT_FAILED);
            break;
        }

        ota_status.bytes_written += length;
        ota_notify_progress();
        ota_log_progress();
    } while (0);

    return error_code;
}

esp_err_t ota_finish(void)
{
    esp_err_t error_code = ESP_OK;

    do
    {
        if (ota_status.state != OTA_STATE_RECEIVING)
        {
            error_code = ESP_ERR_INVALID_STATE;
            break;
        }

        error_code = ota_flush_block();

        if (error_code != ESP_OK)
        {
            ESP_LOGE(ota_tag, "esp_ota_write failed: %s", esp_err_to_name(error_code));
            esp_ota_abort(ota_handle);
            ota_status.state      = OTA_STATE_FAILED;
            ota_status.last_error = error_code;
            ota_notify(OTA_EVENT_FAILED);
            break;
        }

        ota_status.state = OTA_STATE_VALIDATING;

        const uint32_t rate    = ota_rate_tenths_kbps();
        const uint32_t elapsed = ota_elapsed_tenths_s();

        ESP_LOGI(
            ota_tag,
            "Received %u bytes in %lu.%lu s (%lu.%lu KB/s), validating image",
            (unsigned)ota_status.bytes_written,
            (unsigned long)(elapsed / 10u),
            (unsigned long)(elapsed % 10u),
            (unsigned long)(rate / 10u),
            (unsigned long)(rate % 10u));

        error_code = esp_ota_end(ota_handle);

        if (error_code != ESP_OK)
        {
            ESP_LOGE(ota_tag, "esp_ota_end failed: %s", esp_err_to_name(error_code));
            ota_status.state      = OTA_STATE_FAILED;
            ota_status.last_error = error_code;
            ota_notify(OTA_EVENT_FAILED);
            break;
        }

        ota_status.state = OTA_STATE_APPLYING;

        error_code = esp_ota_set_boot_partition(ota_target_partition);

        if (error_code != ESP_OK)
        {
            ESP_LOGE(
                ota_tag,
                "esp_ota_set_boot_partition failed: %s",
                esp_err_to_name(error_code));
            ota_status.state      = OTA_STATE_FAILED;
            ota_status.last_error = error_code;
            ota_notify(OTA_EVENT_FAILED);
            break;
        }

        ota_status.state      = OTA_STATE_PENDING_REBOOT;
        ota_status.last_error = ESP_OK;

        ESP_LOGI(
            ota_tag,
            "Update applied — %u bytes written, reboot needed",
            (unsigned)ota_status.bytes_written);

        ota_notify(OTA_EVENT_DONE);
    } while (0);

    return error_code;
}

esp_err_t ota_abort(void)
{
    esp_err_t error_code = ESP_OK;

    /* Only act on an open session: resetting a FAILED session to IDLE would
     * erase last_error before status callers see it. */
    if ((ota_status.state == OTA_STATE_RECEIVING)
        || (ota_status.state == OTA_STATE_VALIDATING))
    {
        error_code       = esp_ota_abort(ota_handle);
        ota_block_length = 0u;

        ESP_LOGW(
            ota_tag,
            "Aborted at %u / %u bytes",
            (unsigned)ota_status.bytes_written,
            (unsigned)ota_status.bytes_expected);

        ota_status.state      = OTA_STATE_IDLE;
        ota_status.last_error = ESP_OK;

        ota_notify(OTA_EVENT_FAILED);
    }

    return error_code;
}

void ota_get_status(ota_status_t* status)
{
    if (status != NULL)
    {
        *status = ota_status;
    }
}

const char* ota_get_running_version(void)
{
    return ESP32S3_CAM_PROJECT_VERSION;
}

/**** END OF FILE ****/
