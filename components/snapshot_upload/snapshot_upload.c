/**
 ******************************************************************************
 * @file    snapshot_upload.c
 * @author  Daniel Dyringer
 * @date    2026-10-04
 * @brief   Snapshot uploader — periodic JPEG POST to a remote endpoint.
 ******************************************************************************
 */


/* Includes ---------------------------------------------------------------- */
#include "snapshot_upload.h"

#include <stdio.h>
#include <string.h>

#include "camera.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "nvs.h"
#include "wifi_connection.h"


/* Constants and macros ---------------------------------------------------- */
// clang-format off

#define SNAPSHOT_UPLOAD_TASK_STACK      6144u
#define SNAPSHOT_UPLOAD_TASK_PRIO       3u
/** @brief Application core — Core 0 belongs to WiFi. */
#define SNAPSHOT_UPLOAD_TASK_CORE       1

/** @brief Connect + send + response budget for one upload. Shorter than the
 *         minimum interval would be nicer, but a slow uplink can need it. */
#define SNAPSHOT_UPLOAD_HTTP_TIMEOUT_MS 1000

/** @brief Task notification bits. */
#define SNAPSHOT_UPLOAD_NOTIFY_RECONFIG (1u << 0)  /**< Recompute the deadline. */
#define SNAPSHOT_UPLOAD_NOTIFY_NOW      (1u << 1)  /**< Upload immediately.     */

#define SNAPSHOT_UPLOAD_NVS_NAMESPACE   "snap_upload"
#define SNAPSHOT_UPLOAD_NVS_ENABLED     "enabled"
#define SNAPSHOT_UPLOAD_NVS_URL         "url"
#define SNAPSHOT_UPLOAD_NVS_USER        "user"
#define SNAPSHOT_UPLOAD_NVS_PASS        "pass"
#define SNAPSHOT_UPLOAD_NVS_INTERVAL    "interval"

// clang-format on


/* Private types ----------------------------------------------------------- */

typedef struct
{
    bool     enabled;
    char     url[SNAPSHOT_UPLOAD_URL_MAX];
    char     user[SNAPSHOT_UPLOAD_CRED_MAX];
    char     pass[SNAPSHOT_UPLOAD_CRED_MAX];
    uint32_t interval_s;
} snapshot_upload_config_t;


/* Private variables ------------------------------------------------------- */

static const char *snapshot_upload_tag = "snapshot_upload";

static SemaphoreHandle_t snapshot_upload_mutex = NULL;
static TaskHandle_t      snapshot_upload_task_handle = NULL;

/* Guarded by snapshot_upload_mutex */
static snapshot_upload_config_t snapshot_upload_cfg = {
    .interval_s = SNAPSHOT_UPLOAD_INTERVAL_DEFAULT_S,
};
static snapshot_upload_status_t snapshot_upload_stats = {
    .last_result = "never",
};


/* Private functions ------------------------------------------------------- */

static void snapshot_upload_notify(uint32_t bits)
{
    if (snapshot_upload_task_handle)
    {
        xTaskNotify(snapshot_upload_task_handle, bits, eSetBits);
    }
}

/** @brief Store one config field. A failed write is logged, not returned:
 *         the live config is already updated and keeps working until reboot. */
static void snapshot_upload_nvs_store(const char *key, const char *str, const uint32_t *u32)
{
    nvs_handle_t    handle     = 0;
    esp_err_t       error_code = nvs_open(SNAPSHOT_UPLOAD_NVS_NAMESPACE, NVS_READWRITE, &handle);

    if (error_code == ESP_OK)
    {
        error_code = str ? nvs_set_str(handle, key, str) : nvs_set_u32(handle, key, *u32);

        if (error_code == ESP_OK)
        {
            error_code = nvs_commit(handle);
        }

        nvs_close(handle);
    }

    if (error_code != ESP_OK)
    {
        ESP_LOGW(snapshot_upload_tag, "Saving '%s' to NVS failed: %s", key, esp_err_to_name(error_code));
    }
}

static void snapshot_upload_nvs_load_str(nvs_handle_t handle, const char *key, char *buf, size_t size)
{
    size_t len = size;

    if (nvs_get_str(handle, key, buf, &len) != ESP_OK)
    {
        buf[0] = '\0';
    }
}

/** @brief Load the saved config; missing keys keep their defaults. */
static void snapshot_upload_config_load(void)
{
    nvs_handle_t handle = 0;

    if (nvs_open(SNAPSHOT_UPLOAD_NVS_NAMESPACE, NVS_READONLY, &handle) != ESP_OK)
    {
        return;  /* nothing saved yet */
    }

    uint32_t enabled  = 0u;
    uint32_t interval = 0u;

    if (nvs_get_u32(handle, SNAPSHOT_UPLOAD_NVS_ENABLED, &enabled) == ESP_OK)
    {
        snapshot_upload_cfg.enabled = (enabled != 0u);
    }

    if (nvs_get_u32(handle, SNAPSHOT_UPLOAD_NVS_INTERVAL, &interval) == ESP_OK
        && interval >= SNAPSHOT_UPLOAD_INTERVAL_MIN_S
        && interval <= SNAPSHOT_UPLOAD_INTERVAL_MAX_S)
    {
        snapshot_upload_cfg.interval_s = interval;
    }

    snapshot_upload_nvs_load_str(handle, SNAPSHOT_UPLOAD_NVS_URL,
                                 snapshot_upload_cfg.url, sizeof(snapshot_upload_cfg.url));
    snapshot_upload_nvs_load_str(handle, SNAPSHOT_UPLOAD_NVS_USER,
                                 snapshot_upload_cfg.user, sizeof(snapshot_upload_cfg.user));
    snapshot_upload_nvs_load_str(handle, SNAPSHOT_UPLOAD_NVS_PASS,
                                 snapshot_upload_cfg.pass, sizeof(snapshot_upload_cfg.pass));

    nvs_close(handle);
}

/**
 * @brief Copy the latest frame into a PSRAM buffer and hand the driver
 *        buffer straight back, so the upload never starves the live stream.
 *        Caller frees *out with heap_caps_free().
 */
static esp_err_t snapshot_upload_grab(uint8_t **out, size_t *out_len)
{
    camera_frame_t frame      = { 0 };
    esp_err_t      error_code = camera_frame_acquire(&frame);

    if (error_code != ESP_OK)
    {
        return error_code;
    }

    uint8_t *copy = heap_caps_malloc(frame.len, MALLOC_CAP_SPIRAM);

    if (copy)
    {
        memcpy(copy, frame.buf, frame.len);
        *out     = copy;
        *out_len = frame.len;
    }
    else
    {
        error_code = ESP_ERR_NO_MEM;
    }

    camera_frame_release(&frame);
    return error_code;
}

static esp_err_t snapshot_upload_post(const snapshot_upload_config_t *cfg,
                                      const uint8_t                  *jpeg,
                                      size_t                          len,
                                      int                            *http_status)
{
    const bool auth = (cfg->user[0] != '\0');

    const esp_http_client_config_t client_cfg = {
        .url        = cfg->url,
        .method     = HTTP_METHOD_POST,
        .timeout_ms = SNAPSHOT_UPLOAD_HTTP_TIMEOUT_MS,
        /* Sent pre-emptively. A 401 then means wrong credentials, so a retry
         * with the same ones is pointless — report it instead. */
        .auth_type                 = auth ? HTTP_AUTH_TYPE_BASIC : HTTP_AUTH_TYPE_NONE,
        .username                  = auth ? cfg->user : NULL,
        .password                  = auth ? cfg->pass : NULL,
        .max_authorization_retries = -1,
        /* Only used for https:// URLs. */
        .crt_bundle_attach = esp_crt_bundle_attach,
    };

    esp_http_client_handle_t client = esp_http_client_init(&client_cfg);

    if (!client)
    {
        return ESP_ERR_NO_MEM;
    }

    esp_http_client_set_header(client, "Content-Type", "image/jpeg");
    esp_http_client_set_post_field(client, (const char *)jpeg, (int)len);

    const esp_err_t error_code = esp_http_client_perform(client);

    /* Read even on error: a 401 makes perform() fail after the status line
     * has arrived, and the status is the more useful diagnostic. */
    *http_status = esp_http_client_get_status_code(client);

    esp_http_client_cleanup(client);
    return error_code;
}

/** @brief Update the statistics; log only when the outcome changes, so a
 *         long outage doesn't flood the console every interval. */
static void snapshot_upload_record(bool ok, const char *result, size_t len, int64_t start_us)
{
    const int64_t now_us  = esp_timer_get_time();
    bool          changed = false;

    xSemaphoreTake(snapshot_upload_mutex, portMAX_DELAY);

    changed = (strcmp(snapshot_upload_stats.last_result, result) != 0);
    strlcpy(snapshot_upload_stats.last_result, result, sizeof(snapshot_upload_stats.last_result));
    snapshot_upload_stats.last_attempt_us  = start_us;
    snapshot_upload_stats.last_size        = len;
    snapshot_upload_stats.last_duration_ms = (uint32_t)((now_us - start_us) / 1000);

    if (ok)
    {
        snapshot_upload_stats.ok_count++;
        snapshot_upload_stats.last_ok_us = start_us;
    }
    else
    {
        snapshot_upload_stats.fail_count++;
    }

    xSemaphoreGive(snapshot_upload_mutex);

    if (changed)
    {
        if (ok)
        {
            ESP_LOGI(snapshot_upload_tag, "Uploading OK (%u bytes)", (unsigned)len);
        }
        else
        {
            ESP_LOGW(snapshot_upload_tag, "Upload failed: %s", result);
        }
    }
}

static void snapshot_upload_run(void)
{
    snapshot_upload_config_t cfg;

    xSemaphoreTake(snapshot_upload_mutex, portMAX_DELAY);
    cfg = snapshot_upload_cfg;
    xSemaphoreGive(snapshot_upload_mutex);

    const int64_t start_us    = esp_timer_get_time();
    uint8_t      *jpeg        = NULL;
    size_t        len         = 0u;
    int           http_status = 0;
    bool          ok          = false;
    char          result[SNAPSHOT_UPLOAD_RESULT_MAX];

    do
    {
        if (wifi_connection_get_state() != WIFI_CONN_STATE_CONNECTED)
        {
            strlcpy(result, "WiFi not connected", sizeof(result));
            break;
        }

        esp_err_t error_code = snapshot_upload_grab(&jpeg, &len);

        if (error_code != ESP_OK)
        {
            snprintf(result, sizeof(result), "capture: %s", esp_err_to_name(error_code));
            break;
        }

        error_code = snapshot_upload_post(&cfg, jpeg, len, &http_status);

        if (error_code == ESP_OK && http_status >= 200 && http_status < 300)
        {
            strlcpy(result, "ok", sizeof(result));
            ok = true;
        }
        else if (http_status > 0)
        {
            snprintf(result, sizeof(result), "HTTP %d%s", http_status,
                     (http_status == 401) ? " (wrong credentials)" : "");
        }
        else
        {
            snprintf(result, sizeof(result), "%s", esp_err_to_name(error_code));
        }
    } while (0);

    /* Wipe the password copy — it has no business lingering on the stack. */
    memset(cfg.pass, 0, sizeof(cfg.pass));
    heap_caps_free(jpeg);
    snapshot_upload_record(ok, result, len, start_us);
}

static void snapshot_upload_task(void *arg)
{
    (void)arg;

    TickType_t last_run = xTaskGetTickCount();

    for (;;)
    {
        xSemaphoreTake(snapshot_upload_mutex, portMAX_DELAY);
        const bool     active     = snapshot_upload_cfg.enabled && (snapshot_upload_cfg.url[0] != '\0');
        const uint32_t interval_s = snapshot_upload_cfg.interval_s;
        xSemaphoreGive(snapshot_upload_mutex);

        /* Deadline is measured from the start of the previous upload, so the
         * period stays steady regardless of how long each upload takes. */
        TickType_t wait = portMAX_DELAY;

        if (active)
        {
            const TickType_t period  = pdMS_TO_TICKS(interval_s * 1000u);
            const TickType_t elapsed = xTaskGetTickCount() - last_run;

            wait = (elapsed < period) ? (period - elapsed) : 0u;
        }

        uint32_t bits = 0u;

        if (xTaskNotifyWait(0u, UINT32_MAX, &bits, wait) == pdTRUE
            && (bits & SNAPSHOT_UPLOAD_NOTIFY_NOW) == 0u)
        {
            continue;  /* reconfigured — recompute the deadline */
        }

        last_run = xTaskGetTickCount();
        snapshot_upload_run();
    }
}


/* Exposed API ------------------------------------------------------------- */

esp_err_t snapshot_upload_init(void)
{
    esp_err_t error_code = ESP_OK;

    do
    {
        snapshot_upload_mutex = xSemaphoreCreateMutex();

        if (!snapshot_upload_mutex)
        {
            error_code = ESP_ERR_NO_MEM;
            break;
        }

        snapshot_upload_config_load();

        if (xTaskCreatePinnedToCore(snapshot_upload_task, "snap_upload",
                                    SNAPSHOT_UPLOAD_TASK_STACK, NULL,
                                    SNAPSHOT_UPLOAD_TASK_PRIO,
                                    &snapshot_upload_task_handle,
                                    SNAPSHOT_UPLOAD_TASK_CORE) != pdPASS)
        {
            error_code = ESP_ERR_NO_MEM;
            break;
        }

        ESP_LOGI(snapshot_upload_tag, "%s, every %u s -> %s",
                 snapshot_upload_cfg.enabled ? "Enabled" : "Disabled",
                 (unsigned)snapshot_upload_cfg.interval_s,
                 snapshot_upload_cfg.url[0] ? snapshot_upload_cfg.url : "(no URL)");
    } while (0);

    return error_code;
}

esp_err_t snapshot_upload_set_url(const char *url)
{
    if (!url
        || (strncmp(url, "http://", 7) != 0 && strncmp(url, "https://", 8) != 0)
        || strlen(url) >= SNAPSHOT_UPLOAD_URL_MAX)
    {
        return ESP_ERR_INVALID_ARG;
    }

    xSemaphoreTake(snapshot_upload_mutex, portMAX_DELAY);
    strlcpy(snapshot_upload_cfg.url, url, sizeof(snapshot_upload_cfg.url));
    xSemaphoreGive(snapshot_upload_mutex);

    snapshot_upload_nvs_store(SNAPSHOT_UPLOAD_NVS_URL, url, NULL);
    snapshot_upload_notify(SNAPSHOT_UPLOAD_NOTIFY_RECONFIG);
    return ESP_OK;
}

esp_err_t snapshot_upload_set_auth(const char *user, const char *password)
{
    if (!user)
    {
        user = "";
    }

    if (!password || user[0] == '\0')
    {
        password = "";
    }

    if (strlen(user) >= SNAPSHOT_UPLOAD_CRED_MAX || strlen(password) >= SNAPSHOT_UPLOAD_CRED_MAX)
    {
        return ESP_ERR_INVALID_ARG;
    }

    xSemaphoreTake(snapshot_upload_mutex, portMAX_DELAY);
    strlcpy(snapshot_upload_cfg.user, user, sizeof(snapshot_upload_cfg.user));
    strlcpy(snapshot_upload_cfg.pass, password, sizeof(snapshot_upload_cfg.pass));
    xSemaphoreGive(snapshot_upload_mutex);

    snapshot_upload_nvs_store(SNAPSHOT_UPLOAD_NVS_USER, user, NULL);
    snapshot_upload_nvs_store(SNAPSHOT_UPLOAD_NVS_PASS, password, NULL);
    return ESP_OK;
}

esp_err_t snapshot_upload_set_interval(uint32_t interval_s)
{
    if (interval_s < SNAPSHOT_UPLOAD_INTERVAL_MIN_S || interval_s > SNAPSHOT_UPLOAD_INTERVAL_MAX_S)
    {
        return ESP_ERR_INVALID_ARG;
    }

    xSemaphoreTake(snapshot_upload_mutex, portMAX_DELAY);
    snapshot_upload_cfg.interval_s = interval_s;
    xSemaphoreGive(snapshot_upload_mutex);

    snapshot_upload_nvs_store(SNAPSHOT_UPLOAD_NVS_INTERVAL, NULL, &interval_s);
    snapshot_upload_notify(SNAPSHOT_UPLOAD_NOTIFY_RECONFIG);
    return ESP_OK;
}

esp_err_t snapshot_upload_set_enabled(bool enabled)
{
    xSemaphoreTake(snapshot_upload_mutex, portMAX_DELAY);
    const bool has_url = (snapshot_upload_cfg.url[0] != '\0');

    if (enabled && !has_url)
    {
        xSemaphoreGive(snapshot_upload_mutex);
        return ESP_ERR_INVALID_STATE;
    }

    snapshot_upload_cfg.enabled = enabled;
    xSemaphoreGive(snapshot_upload_mutex);

    const uint32_t value = enabled ? 1u : 0u;

    snapshot_upload_nvs_store(SNAPSHOT_UPLOAD_NVS_ENABLED, NULL, &value);
    snapshot_upload_notify(enabled ? SNAPSHOT_UPLOAD_NOTIFY_NOW : SNAPSHOT_UPLOAD_NOTIFY_RECONFIG);
    return ESP_OK;
}

void snapshot_upload_get_status(snapshot_upload_status_t *out)
{
    if (!out)
    {
        return;
    }

    if (!snapshot_upload_mutex)
    {
        memset(out, 0, sizeof(*out));
        return;
    }

    xSemaphoreTake(snapshot_upload_mutex, portMAX_DELAY);
    *out              = snapshot_upload_stats;
    out->enabled      = snapshot_upload_cfg.enabled;
    out->interval_s   = snapshot_upload_cfg.interval_s;
    out->has_password = (snapshot_upload_cfg.pass[0] != '\0');
    strlcpy(out->url, snapshot_upload_cfg.url, sizeof(out->url));
    strlcpy(out->user, snapshot_upload_cfg.user, sizeof(out->user));
    xSemaphoreGive(snapshot_upload_mutex);
}


/**** END OF FILE ****/
