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
#include "data_manager.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "mbedtls/platform_util.h"
#include "snapshot_upload_settings.h"
#include "utils.h"
#include "wifi_connection.h"


/* Constants and macros ---------------------------------------------------- */
// clang-format off

#define SNAPSHOT_UPLOAD_TASK_STACK      6144u
#define SNAPSHOT_UPLOAD_TASK_PRIO       3u
/* Core 0 belongs to WiFi. */
#define SNAPSHOT_UPLOAD_TASK_CORE       1

#define SNAPSHOT_UPLOAD_HTTP_TIMEOUT_MS 1000u

#define SNAPSHOT_UPLOAD_NOTIFY_RECONFIG (1u << 0u)
#define SNAPSHOT_UPLOAD_NOTIFY_NOW      (1u << 1u)

// clang-format on

/* Private types ----------------------------------------------------------- */

typedef struct snapshot_upload_config_s
{
    char url[SNAPSHOT_UPLOAD_URL_MAX];
    char user[SNAPSHOT_UPLOAD_CRED_MAX];
    char password[SNAPSHOT_UPLOAD_CRED_MAX];
    uint32_t interval_s;
    bool enabled;
} snapshot_upload_config_t;


/* Private variables ------------------------------------------------------- */

static const char* snapshot_upload_tag = "snapshot_upload";

static SemaphoreHandle_t snapshot_upload_mutex  = NULL;
static TaskHandle_t snapshot_upload_task_handle = NULL;

/* Guarded by snapshot_upload_mutex */
static snapshot_upload_status_t snapshot_upload_statistics = {
    .last_result = "never",
};


/* Private functions ------------------------------------------------------- */

static void snapshot_upload_notify(uint32_t bits)
{
    if (snapshot_upload_task_handle != NULL)
    {
        xTaskNotify(snapshot_upload_task_handle, bits, eSetBits);
    }
}

/* Runs in the work_queue task. Only wakes the upload task, which re-reads its
 * config, so a burst of changes costs nothing and needs no debounce. An
 * immediate upload is an action, sent by snapshot_upload_set_enabled(). */
static void snapshot_upload_on_settings_change(uint32_t id)
{
    ARG_UNUSED(id);

    snapshot_upload_notify(SNAPSHOT_UPLOAD_NOTIFY_RECONFIG);
}

static esp_err_t snapshot_upload_config_read(snapshot_upload_config_t* config)
{
    esp_err_t error_code = data_manager_get(
        SNAPSHOT_UPLOAD_ENABLED, &config->enabled, sizeof(config->enabled));

    if (error_code == ESP_OK)
    {
        error_code =
            data_manager_get(SNAPSHOT_UPLOAD_URL, config->url, sizeof(config->url));
    }

    if (error_code == ESP_OK)
    {
        error_code =
            data_manager_get(SNAPSHOT_UPLOAD_USER, config->user, sizeof(config->user));
    }

    if (error_code == ESP_OK)
    {
        error_code = data_manager_get(
            SNAPSHOT_UPLOAD_PASSWORD, config->password, sizeof(config->password));
    }

    if (error_code == ESP_OK)
    {
        error_code = data_manager_get(
            SNAPSHOT_UPLOAD_INTERVAL, &config->interval_s, sizeof(config->interval_s));
    }

    return error_code;
}

/* Setters persist right away, unlike the camera's explicit Save. */
static esp_err_t snapshot_upload_store(uint32_t id, const void* value, size_t size)
{
    esp_err_t error_code = data_manager_set(id, value, size);

    if (error_code == ESP_OK)
    {
        error_code = data_manager_save();
    }

    return error_code;
}

/* Releases the driver buffer right after the copy, so a slow upload never
 * starves the live stream. Caller frees *out with heap_caps_free(). */
static esp_err_t snapshot_upload_grab(uint8_t** out, size_t* out_length)
{
    camera_frame_t frame = {0};
    esp_err_t error_code = camera_frame_acquire(&frame);

    if (error_code == ESP_OK)
    {
        uint8_t* const copy = heap_caps_malloc(frame.length, MALLOC_CAP_SPIRAM);

        if (copy != NULL)
        {
            memcpy(copy, frame.buffer, frame.length);
            *out        = copy;
            *out_length = frame.length;
        }
        else
        {
            error_code = ESP_ERR_NO_MEM;
        }

        camera_frame_release(&frame);
    }

    return error_code;
}

static esp_err_t snapshot_upload_post(
    const snapshot_upload_config_t* config,
    const uint8_t* jpeg,
    size_t length,
    int* http_status)
{
    esp_err_t error_code    = ESP_OK;
    const bool authenticate = (config->user[0] != '\0');

    const esp_http_client_config_t client_config = {
        .url        = config->url,
        .method     = HTTP_METHOD_POST,
        .timeout_ms = (int)SNAPSHOT_UPLOAD_HTTP_TIMEOUT_MS,
        /* Sent pre-emptively. A 401 then means wrong credentials, so a retry
         * with the same ones is pointless — report it instead. */
        .auth_type = authenticate ? HTTP_AUTH_TYPE_BASIC : HTTP_AUTH_TYPE_NONE,
        .username  = authenticate ? config->user : NULL,
        .password  = authenticate ? config->password : NULL,
        .max_authorization_retries = -1,
        /* Only used for https:// URLs. */
        .crt_bundle_attach = esp_crt_bundle_attach,
    };

    esp_http_client_handle_t client = esp_http_client_init(&client_config);

    if (client == NULL)
    {
        error_code = ESP_ERR_NO_MEM;
    }
    else
    {
        esp_http_client_set_header(client, "Content-Type", "image/jpeg");
        esp_http_client_set_post_field(client, (const char*)jpeg, (int)length);

        error_code = esp_http_client_perform(client);

        /* Read even on error: a 401 makes perform() fail after the status line
         * has arrived, and the status is the more useful diagnostic. */
        *http_status = esp_http_client_get_status_code(client);

        esp_http_client_cleanup(client);
    }

    return error_code;
}

/* Logs only when the outcome changes, so an outage doesn't flood the console. */
static void snapshot_upload_record(bool ok, const char* result, size_t length, int64_t start_us)
{
    const int64_t now_us = esp_timer_get_time();

    xSemaphoreTake(snapshot_upload_mutex, portMAX_DELAY);

    const bool changed = (strcmp(snapshot_upload_statistics.last_result, result) != 0);
    strlcpy(
        snapshot_upload_statistics.last_result,
        result,
        sizeof(snapshot_upload_statistics.last_result));
    snapshot_upload_statistics.last_attempt_us  = start_us;
    snapshot_upload_statistics.last_size        = length;
    snapshot_upload_statistics.last_duration_ms = (uint32_t)((now_us - start_us) / 1000);

    if (ok)
    {
        snapshot_upload_statistics.ok_count++;
        snapshot_upload_statistics.last_ok_us = start_us;
    }
    else
    {
        snapshot_upload_statistics.fail_count++;
    }

    xSemaphoreGive(snapshot_upload_mutex);

    if (changed)
    {
        if (ok)
        {
            ESP_LOGI(snapshot_upload_tag, "Uploading OK (%u bytes)", (unsigned)length);
        }
        else
        {
            ESP_LOGW(snapshot_upload_tag, "Upload failed: %s", result);
        }
    }
}

static void snapshot_upload_run(void)
{
    snapshot_upload_config_t config         = {0};
    const esp_err_t settings_error          = snapshot_upload_config_read(&config);
    const int64_t start_us                  = esp_timer_get_time();
    uint8_t* jpeg                           = NULL;
    size_t length                           = 0u;
    int http_status                         = 0;
    bool ok                                 = false;
    char result[SNAPSHOT_UPLOAD_RESULT_MAX] = {0};

    do
    {
        if (settings_error != ESP_OK)
        {
            snprintf(
                result, sizeof(result), "settings: %s", esp_err_to_name(settings_error));
            break;
        }

        if (wifi_connection_get_state() != WIFI_CONNECTION_STATE_CONNECTED)
        {
            strlcpy(result, "WiFi not connected", sizeof(result));
            break;
        }

        esp_err_t error_code = snapshot_upload_grab(&jpeg, &length);

        if (error_code != ESP_OK)
        {
            snprintf(result, sizeof(result), "capture: %s", esp_err_to_name(error_code));
            break;
        }

        error_code = snapshot_upload_post(&config, jpeg, length, &http_status);

        if ((error_code == ESP_OK) && (http_status >= 200) && (http_status < 300))
        {
            strlcpy(result, "ok", sizeof(result));
            ok = true;
        }
        else if (http_status > 0)
        {
            snprintf(
                result,
                sizeof(result),
                "HTTP %d%s",
                http_status,
                (http_status == 401) ? " (wrong credentials)" : "");
        }
        else
        {
            snprintf(result, sizeof(result), "%s", esp_err_to_name(error_code));
        }
    } while (0);

    /* Don't leave the password lingering on the stack. A plain memset of a
     * dying local may be optimised away. */
    mbedtls_platform_zeroize(config.password, sizeof(config.password));
    heap_caps_free(jpeg);
    snapshot_upload_record(ok, result, length, start_us);
}

static void snapshot_upload_task(void* arg)
{
    ARG_UNUSED(arg);

    TickType_t last_run = xTaskGetTickCount();

    for (;;)
    {
        bool enabled                      = false;
        uint32_t interval_s               = 0u;
        char url[SNAPSHOT_UPLOAD_URL_MAX] = {0};

        /* On a read failure the defaults above keep the task idle. */
        data_manager_get(SNAPSHOT_UPLOAD_ENABLED, &enabled, sizeof(enabled));
        data_manager_get(SNAPSHOT_UPLOAD_URL, url, sizeof(url));
        data_manager_get(SNAPSHOT_UPLOAD_INTERVAL, &interval_s, sizeof(interval_s));

        const bool active = enabled && (url[0] != '\0');

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

        if ((xTaskNotifyWait(0u, UINT32_MAX, &bits, wait) == pdTRUE)
            && ((bits & SNAPSHOT_UPLOAD_NOTIFY_NOW) == 0u))
        {
            continue; /* reconfigured — recompute the deadline */
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

        if (snapshot_upload_mutex == NULL)
        {
            error_code = ESP_ERR_NO_MEM;
            break;
        }

        error_code = snapshot_upload_settings_init(snapshot_upload_on_settings_change);

        if (error_code != ESP_OK)
        {
            break;
        }

        if (xTaskCreatePinnedToCore(
                snapshot_upload_task,
                "snap_upload",
                SNAPSHOT_UPLOAD_TASK_STACK,
                NULL,
                SNAPSHOT_UPLOAD_TASK_PRIO,
                &snapshot_upload_task_handle,
                SNAPSHOT_UPLOAD_TASK_CORE)
            != pdPASS)
        {
            error_code = ESP_ERR_NO_MEM;
            break;
        }

        snapshot_upload_config_t config = {0};

        if (snapshot_upload_config_read(&config) == ESP_OK)
        {
            ESP_LOGI(
                snapshot_upload_tag,
                "%s, every %u s -> %s",
                config.enabled ? "Enabled" : "Disabled",
                (unsigned)config.interval_s,
                (config.url[0] != '\0') ? config.url : "(no URL)");
        }

        mbedtls_platform_zeroize(config.password, sizeof(config.password));
    } while (0);

    if (error_code != ESP_OK)
    {
        ESP_LOGE(snapshot_upload_tag, "Init failed: %s", esp_err_to_name(error_code));
    }

    return error_code;
}

esp_err_t snapshot_upload_set_url(const char* url)
{
    esp_err_t error_code = ESP_OK;

    /* The http(s):// prefix is checked by the setting's validator; an empty URL
     * is a valid stored value but not a valid target. */
    if ((url == NULL) || (url[0] == '\0') || (strlen(url) >= SNAPSHOT_UPLOAD_URL_MAX))
    {
        error_code = ESP_ERR_INVALID_ARG;
    }
    else
    {
        error_code = snapshot_upload_store(SNAPSHOT_UPLOAD_URL, url, strlen(url) + 1u);
    }

    return error_code;
}

esp_err_t snapshot_upload_set_auth(const char* user, const char* password)
{
    esp_err_t error_code = ESP_OK;

    if (user == NULL)
    {
        user = "";
    }

    if ((password == NULL) || (user[0] == '\0'))
    {
        password = "";
    }

    if ((strlen(user) >= SNAPSHOT_UPLOAD_CRED_MAX)
        || (strlen(password) >= SNAPSHOT_UPLOAD_CRED_MAX))
    {
        error_code = ESP_ERR_INVALID_ARG;
    }
    else
    {
        error_code = data_manager_set(SNAPSHOT_UPLOAD_USER, user, strlen(user) + 1u);

        if (error_code == ESP_OK)
        {
            error_code = snapshot_upload_store(
                SNAPSHOT_UPLOAD_PASSWORD, password, strlen(password) + 1u);
        }
    }

    return error_code;
}

/* The range comes from the setting's descriptor: out of range is
 * ESP_ERR_INVALID_ARG. */
esp_err_t snapshot_upload_set_interval(uint32_t interval_s)
{
    return snapshot_upload_store(SNAPSHOT_UPLOAD_INTERVAL, &interval_s, sizeof(interval_s));
}

esp_err_t snapshot_upload_set_enabled(bool enabled)
{
    char url[SNAPSHOT_UPLOAD_URL_MAX] = {0};
    esp_err_t error_code = data_manager_get(SNAPSHOT_UPLOAD_URL, url, sizeof(url));

    if ((error_code == ESP_OK) && enabled && (url[0] == '\0'))
    {
        error_code = ESP_ERR_INVALID_STATE;
    }

    if (error_code == ESP_OK)
    {
        error_code =
            snapshot_upload_store(SNAPSHOT_UPLOAD_ENABLED, &enabled, sizeof(enabled));
    }

    /* The only place that asks for an immediate upload, also when the value
     * did not change ("on" while already on). */
    if (error_code == ESP_OK)
    {
        snapshot_upload_notify(
            enabled ? SNAPSHOT_UPLOAD_NOTIFY_NOW : SNAPSHOT_UPLOAD_NOTIFY_RECONFIG);
    }

    return error_code;
}

void snapshot_upload_get_status(snapshot_upload_status_t* status)
{
    snapshot_upload_config_t config = {0};

    if (status != NULL)
    {
        memset(status, 0, sizeof(*status));

        if (snapshot_upload_mutex != NULL)
        {
            xSemaphoreTake(snapshot_upload_mutex, portMAX_DELAY);
            *status = snapshot_upload_statistics;
            xSemaphoreGive(snapshot_upload_mutex);
        }

        if (snapshot_upload_config_read(&config) == ESP_OK)
        {
            status->enabled      = config.enabled;
            status->interval_s   = config.interval_s;
            status->has_password = (config.password[0] != '\0');
            strlcpy(status->url, config.url, sizeof(status->url));
            strlcpy(status->user, config.user, sizeof(status->user));
        }

        mbedtls_platform_zeroize(config.password, sizeof(config.password));
    }
}

/**** END OF FILE ****/
