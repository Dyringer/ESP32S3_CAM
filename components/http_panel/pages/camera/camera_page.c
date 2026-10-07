/**
 ******************************************************************************
 * @file    camera_page.c
 * @author  Daniel Dyringer
 * @date    2026-09-30
 * @brief   Camera page — live MJPEG view, snapshot and sensor settings.
 *
 *          Architecture:
 *            - GET /camera          serves camera_page.html
 *            - GET /camera/stream   MJPEG stream (multipart/x-mixed-replace)
 *            - GET /camera/capture  single JPEG still (LED flash if set)
 *            - GET /camera/status   JSON: sensor, settings + ranges,
 *                                   resolution table, viewers, fps, CPU load
 *            - GET /camera/control?var=<name>&val=<int>   apply one setting
 *            - POST /camera/settings?action=save|defaults   persist to NVS /
 *                                   restore boot defaults
 *
 *          Streaming never blocks the httpd task: the stream handler detaches
 *          the request with httpd_req_async_handler_begin() and hands it to a
 *          dedicated stream task, which grabs each frame once and sends it to
 *          every viewer. The rest of the panel stays responsive while video
 *          runs — no second server or port is needed.
 *
 *          Async requests must be completed before httpd_stop() frees the
 *          server, so the page registers a lifecycle callback: on "stopping"
 *          it refuses new viewers, shuts the stream sockets down (unblocking
 *          any send in progress) and waits for the task to release them.
 *
 *          No auth — same LAN-only trust model as the rest of http_panel.
 ******************************************************************************
 */


/* Includes ---------------------------------------------------------------- */
#include "camera_page.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>

#include "camera.h"
#include "cJSON.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "http_panel_providers.h"
#include "http_panel_server.h"
#include "utils.h"


/* Constants and macros ---------------------------------------------------- */
// clang-format off

/** @brief Simultaneous stream viewers. Each costs one socket and a share of
 *         the WiFi bandwidth — every frame is sent to every viewer. */
#define CAMERA_PAGE_MAX_STREAMS         2u

#define CAMERA_PAGE_STREAM_TASK_STACK   4096u
#define CAMERA_PAGE_STREAM_TASK_PRIO    4u
/** @brief Application core — Core 0 belongs to WiFi. */
#define CAMERA_PAGE_STREAM_TASK_CORE    1

/** @brief Upper bound for the stream task to release its viewers on stop. */
#define CAMERA_PAGE_STOP_TIMEOUT_MS     5000u

/** @brief Back-off after a failed frame grab. */
#define CAMERA_PAGE_RETRY_DELAY_MS      50u

#define CAMERA_PAGE_QUERY_LEN           64u
#define CAMERA_PAGE_VAR_LEN             32u
#define CAMERA_PAGE_VAL_LEN             16u

#define CAMERA_PAGE_PART_BOUNDARY       "esp32s3camframe"
#define CAMERA_PAGE_STREAM_TYPE         "multipart/x-mixed-replace;boundary=" CAMERA_PAGE_PART_BOUNDARY
#define CAMERA_PAGE_STREAM_BOUNDARY     "\r\n--" CAMERA_PAGE_PART_BOUNDARY "\r\n"
#define CAMERA_PAGE_STREAM_PART         "Content-Type: image/jpeg\r\nContent-Length: %u\r\n\r\n"

#define CAMERA_PAGE_503                 "503 Service Unavailable"

// clang-format on


/* Private types ----------------------------------------------------------- */

/** @brief One detached stream request. fd is captured in the handler, where
 *         httpd_req_to_sockfd() is valid under CONFIG_HTTPD_VALIDATE_REQ. */
typedef struct
{
    httpd_req_t *req;   /**< Async copy; NULL = stop wake-up.            */
    int          fd;
} camera_page_viewer_t;


/* Private variables ------------------------------------------------------- */

static const char *camera_page_tag = "camera_page";

/** @brief Detached stream requests (camera_page_viewer_t), httpd task ->
 *         stream task. An item with req == NULL is a stop wake-up. */
static QueueHandle_t camera_page_stream_queue = NULL;

/**
 * @brief Guards camera_page_accepting and camera_page_stream_fds — shared by
 *        the httpd task (handler), the stream task and the monitor task
 *        (lifecycle callback).
 */
static SemaphoreHandle_t camera_page_lock = NULL;

/** @brief True while the server runs; cleared first thing on stop. */
static bool camera_page_accepting = false;

/** @brief Socket of every accepted viewer (queued or streaming), -1 = free. */
static int camera_page_stream_fds[CAMERA_PAGE_MAX_STREAMS];

/** @brief Given by the stream task once a stop request is handled. */
static SemaphoreHandle_t camera_page_stopped = NULL;

/** @brief Delivered frames per second x10, updated by the stream task. */
static volatile uint32_t camera_page_fps_x10 = 0u;

extern const uint8_t camera_page_html_start[] asm("_binary_camera_page_html_start");
extern const uint8_t camera_page_html_end[]   asm("_binary_camera_page_html_end");


/* Private functions ------------------------------------------------------- */

static esp_err_t camera_page_send_503(httpd_req_t *req, const char *msg)
{
    httpd_resp_set_status(req, CAMERA_PAGE_503);
    httpd_resp_set_type(req, "text/plain");
    return httpd_resp_sendstr(req, msg);
}

/** @brief Accepted viewers, queued or streaming. Caller holds camera_page_lock. */
static uint8_t camera_page_viewer_count_locked(void)
{
    uint8_t count = 0u;

    for (uint8_t i = 0u; i < CAMERA_PAGE_MAX_STREAMS; i++)
    {
        if (camera_page_stream_fds[i] >= 0)
        {
            count++;
        }
    }

    return count;
}

/* ---- Stream task -------------------------------------------------------- */

static esp_err_t camera_page_send_part(httpd_req_t *req, const camera_frame_t *frame)
{
    char      head[96];
    const int head_len = snprintf(head, sizeof(head),
                                  CAMERA_PAGE_STREAM_BOUNDARY CAMERA_PAGE_STREAM_PART,
                                  (unsigned)frame->len);

    esp_err_t error_code = httpd_resp_send_chunk(req, head, head_len);

    if (error_code == ESP_OK)
    {
        error_code = httpd_resp_send_chunk(req, (const char *)frame->buf,
                                           (ssize_t)frame->len);
    }

    return error_code;
}

/**
 * @brief Finish viewer @p index: complete the async request, close its
 *        socket (the chunked response never ended, so it cannot be reused),
 *        and free its slot.
 */
static void camera_page_stream_drop(camera_page_viewer_t *viewers, uint8_t *count, uint8_t index)
{
    httpd_req_t   *req = viewers[index].req;
    httpd_handle_t hd  = req->handle;
    const int      fd  = viewers[index].fd;

    httpd_req_async_handler_complete(req);   /* frees req */
    httpd_sess_trigger_close(hd, fd);

    xSemaphoreTake(camera_page_lock, portMAX_DELAY);

    for (uint8_t i = 0u; i < CAMERA_PAGE_MAX_STREAMS; i++)
    {
        if (camera_page_stream_fds[i] == fd)
        {
            camera_page_stream_fds[i] = -1;
            break;
        }
    }

    xSemaphoreGive(camera_page_lock);

    for (uint8_t i = index; (i + 1u) < *count; i++)
    {
        viewers[i] = viewers[i + 1u];
    }

    (*count)--;

    ESP_LOGI(camera_page_tag, "Viewer left fd=%d (%u watching)", fd, (unsigned)*count);

    if (*count == 0u)
    {
        camera_led_enable(false);
        camera_page_fps_x10 = 0u;
    }
}

static void camera_page_stream_task(void *arg)
{
    (void)arg;

    camera_page_viewer_t viewers[CAMERA_PAGE_MAX_STREAMS] = { 0 };
    uint8_t      count         = 0u;
    int64_t      window_start  = 0;
    uint32_t     window_frames = 0u;

    while (true)
    {
        /* Idle: sleep until a viewer (or a stop wake-up) arrives.
         * Streaming: just pick up anything queued, without blocking. */
        camera_page_viewer_t incoming = { 0 };
        TickType_t           wait     = (count == 0u) ? portMAX_DELAY : 0;
        bool                 stop     = false;

        while (xQueueReceive(camera_page_stream_queue, &incoming, wait) == pdTRUE)
        {
            wait = 0;

            if (!incoming.req)
            {
                stop = true;   /* wake-up from camera_page_on_server() */
            }
            else if (count < CAMERA_PAGE_MAX_STREAMS)
            {
                viewers[count++] = incoming;
                ESP_LOGI(camera_page_tag, "Viewer joined fd=%d (%u watching)",
                         incoming.fd, (unsigned)count);

                if (count == 1u)
                {
                    camera_led_enable(true);
                    window_start  = esp_timer_get_time();
                    window_frames = 0u;
                }
            }
        }

        if (stop)
        {
            while (count > 0u)
            {
                camera_page_stream_drop(viewers, &count, count - 1u);
            }

            xSemaphoreGive(camera_page_stopped);
            continue;
        }

        if (count == 0u)
        {
            continue;
        }

        camera_frame_t frame;

        if (camera_frame_acquire(&frame) != ESP_OK)
        {
            vTaskDelay(pdMS_TO_TICKS(CAMERA_PAGE_RETRY_DELAY_MS));
            continue;
        }

        /* Walk backwards so dropping a viewer does not skip the next one. */
        for (uint8_t i = count; i-- > 0u;)
        {
            if (camera_page_send_part(viewers[i].req, &frame) != ESP_OK)
            {
                camera_page_stream_drop(viewers, &count, i);
            }
        }

        camera_frame_release(&frame);

        window_frames++;
        const int64_t elapsed = esp_timer_get_time() - window_start;

        if (elapsed >= 1000000LL)
        {
            camera_page_fps_x10 = (count > 0u)
                ? (uint32_t)((int64_t)window_frames * 10000000LL / elapsed)
                : 0u;
            window_start  += elapsed;
            window_frames  = 0u;
        }
    }
}

/* Lifecycle — runs in the panel monitor task. */
static void camera_page_on_server(bool running)
{
    xSemaphoreTake(camera_page_lock, portMAX_DELAY);
    camera_page_accepting = running;

    if (!running)
    {
        /* Unblock any send in progress so the stream task drops the viewer
         * immediately instead of waiting out the socket send timeout. */
        for (uint8_t i = 0u; i < CAMERA_PAGE_MAX_STREAMS; i++)
        {
            if (camera_page_stream_fds[i] >= 0)
            {
                shutdown(camera_page_stream_fds[i], SHUT_RDWR);
            }
        }
    }

    const bool busy = (camera_page_viewer_count_locked() > 0u);
    xSemaphoreGive(camera_page_lock);

    /* No handler can enqueue past this point (accepting is false), and the
     * stream task drains the whole queue — viewers queued ahead of the
     * wake-up included — before it drops everyone. */
    if (!running && busy)
    {
        (void)xSemaphoreTake(camera_page_stopped, 0);  /* clear a stale give */

        const camera_page_viewer_t wake = { .req = NULL, .fd = -1 };
        (void)xQueueSend(camera_page_stream_queue, &wake, 0);

        if (xSemaphoreTake(camera_page_stopped,
                           pdMS_TO_TICKS(CAMERA_PAGE_STOP_TIMEOUT_MS)) != pdTRUE)
        {
            ESP_LOGE(camera_page_tag, "Stream task did not release its viewers");
        }
    }
}

/* ---- HTTP handlers ------------------------------------------------------ */

static esp_err_t camera_page_h_root(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html");
    const size_t len = (size_t)(camera_page_html_end - camera_page_html_start);
    return httpd_resp_send(req, (const char *)camera_page_html_start, (ssize_t)len);
}

static esp_err_t camera_page_h_stream(httpd_req_t *req)
{
    esp_err_t error_code = ESP_OK;

    do
    {
        if (!camera_is_ready())
        {
            error_code = camera_page_send_503(req, "Camera not available");
            break;
        }

        /* Headers are copied into the detached request by begin(). */
        httpd_resp_set_type(req, CAMERA_PAGE_STREAM_TYPE);
        httpd_resp_set_hdr(req, "Cache-Control", "no-store");

        xSemaphoreTake(camera_page_lock, portMAX_DELAY);

        if (!camera_page_accepting
            || camera_page_viewer_count_locked() >= CAMERA_PAGE_MAX_STREAMS)
        {
            xSemaphoreGive(camera_page_lock);
            error_code = camera_page_send_503(req, "Viewer limit reached");
            break;
        }

        camera_page_viewer_t viewer = { .req = NULL, .fd = httpd_req_to_sockfd(req) };

        if (httpd_req_async_handler_begin(req, &viewer.req) != ESP_OK)
        {
            xSemaphoreGive(camera_page_lock);
            error_code = httpd_resp_send_500(req);
            break;
        }

        /* The queue holds MAX_STREAMS + 1 items and at most MAX_STREAMS
         * viewers are ever accepted, so this cannot fail. */
        (void)xQueueSend(camera_page_stream_queue, &viewer, 0);

        /* Below the limit, so a free slot exists. */
        for (uint8_t i = 0u; i < CAMERA_PAGE_MAX_STREAMS; i++)
        {
            if (camera_page_stream_fds[i] < 0)
            {
                camera_page_stream_fds[i] = viewer.fd;
                break;
            }
        }

        xSemaphoreGive(camera_page_lock);
    } while (0);

    return error_code;
}

static esp_err_t camera_page_h_capture(httpd_req_t *req)
{
    esp_err_t      error_code = ESP_OK;
    camera_frame_t frame;

    do
    {
        if (!camera_is_ready())
        {
            error_code = camera_page_send_503(req, "Camera not available");
            break;
        }

        if (camera_still_acquire(&frame) != ESP_OK)
        {
            error_code = httpd_resp_send_500(req);
            break;
        }

        httpd_resp_set_type(req, "image/jpeg");
        httpd_resp_set_hdr(req, "Content-Disposition", "inline; filename=capture.jpg");
        httpd_resp_set_hdr(req, "Cache-Control", "no-store");

        error_code = httpd_resp_send(req, (const char *)frame.buf, (ssize_t)frame.len);

        ESP_LOGI(camera_page_tag, "Still %ux%u, %u bytes",
                 (unsigned)frame.width, (unsigned)frame.height, (unsigned)frame.len);

        camera_frame_release(&frame);
    } while (0);

    return error_code;
}

static char *camera_page_build_status(void)
{
    cJSON *root = cJSON_CreateObject();

    if (!root)
    {
        return NULL;
    }

    xSemaphoreTake(camera_page_lock, portMAX_DELAY);
    const uint8_t viewers = camera_page_viewer_count_locked();
    xSemaphoreGive(camera_page_lock);

    cJSON_AddBoolToObject(root, "ready", camera_is_ready());
    cJSON_AddBoolToObject(root, "unsaved", camera_settings_unsaved());
    cJSON_AddStringToObject(root, "sensor", camera_sensor_name());
    cJSON_AddNumberToObject(root, "viewers", viewers);
    cJSON_AddNumberToObject(root, "max_viewers", CAMERA_PAGE_MAX_STREAMS);
    cJSON_AddNumberToObject(root, "fps", (double)camera_page_fps_x10 / 10.0);

    hp_add_cpu_load(root);

    /* Every setting with its range, so the page never hard-codes limits. */
    cJSON *settings = cJSON_AddObjectToObject(root, "settings");

    for (size_t i = 0u; i < camera_setting_count(); i++)
    {
        const camera_setting_desc_t *desc  = camera_setting_at(i);
        int                          value = 0;

        if (camera_setting_get(desc->name, &value) != ESP_OK)
        {
            continue;
        }

        cJSON *item = cJSON_AddObjectToObject(settings, desc->name);
        cJSON_AddNumberToObject(item, "value", value);
        cJSON_AddNumberToObject(item, "min", desc->min);
        cJSON_AddNumberToObject(item, "max", desc->max);
    }

    /* framesize value N -> [width, height] */
    cJSON *sizes = cJSON_AddArrayToObject(root, "framesizes");
    uint16_t w   = 0u;
    uint16_t h   = 0u;

    for (int fs = 0; camera_framesize_resolution(fs, &w, &h) == ESP_OK; fs++)
    {
        cJSON *pair = cJSON_CreateArray();
        cJSON_AddItemToArray(pair, cJSON_CreateNumber(w));
        cJSON_AddItemToArray(pair, cJSON_CreateNumber(h));
        cJSON_AddItemToArray(sizes, pair);
    }

    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    return json; /* caller must free() */
}

static esp_err_t camera_page_h_status(httpd_req_t *req)
{
    char *json = camera_page_build_status();

    if (!json)
    {
        return httpd_resp_send_500(req);
    }

    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    const esp_err_t error_code = httpd_resp_sendstr(req, json);
    free(json);
    return error_code;
}

static esp_err_t camera_page_h_control(httpd_req_t *req)
{
    char query[CAMERA_PAGE_QUERY_LEN];
    char var[CAMERA_PAGE_VAR_LEN];
    char val[CAMERA_PAGE_VAL_LEN];

    if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK
        || httpd_query_key_value(query, "var", var, sizeof(var)) != ESP_OK
        || httpd_query_key_value(query, "val", val, sizeof(val)) != ESP_OK)
    {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Expected ?var=<name>&val=<int>");
    }

    char      *end   = NULL;
    const long value = strtol(val, &end, 10);

    if (end == val || *end != '\0')
    {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "val must be an integer");
    }

    esp_err_t error_code;

    switch (camera_setting_set(var, (int)value))
    {
    case ESP_OK:
        error_code = httpd_resp_sendstr(req, "OK");
        break;
    case ESP_ERR_NOT_FOUND:
        error_code = httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "Unknown setting");
        break;
    case ESP_ERR_INVALID_ARG:
        error_code = httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Value out of range");
        break;
    case ESP_ERR_INVALID_STATE:
        error_code = camera_page_send_503(req, "Camera not available");
        break;
    default:
        error_code = httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                                         "Sensor rejected the value");
        break;
    }

    return error_code;
}

static esp_err_t camera_page_h_settings(httpd_req_t *req)
{
    char query[CAMERA_PAGE_QUERY_LEN];
    char action[CAMERA_PAGE_VAR_LEN];

    if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK
        || httpd_query_key_value(query, "action", action, sizeof(action)) != ESP_OK)
    {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Expected ?action=save|defaults");
    }

    esp_err_t result;

    if (strcmp(action, "save") == 0)
    {
        result = camera_settings_save();
    }
    else if (strcmp(action, "defaults") == 0)
    {
        result = camera_settings_restore_defaults();
    }
    else
    {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Unknown action");
    }

    if (result == ESP_ERR_INVALID_STATE)
    {
        return camera_page_send_503(req, "Camera not available");
    }

    if (result != ESP_OK)
    {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, esp_err_to_name(result));
    }

    return httpd_resp_sendstr(req, "OK");
}

/* ---- URI descriptors ---------------------------------------------------- */

static const httpd_uri_t camera_page_uri_root = {
    .uri     = "/camera",
    .method  = HTTP_GET,
    .handler = camera_page_h_root,
};

static const httpd_uri_t camera_page_uri_stream = {
    .uri     = "/camera/stream",
    .method  = HTTP_GET,
    .handler = camera_page_h_stream,
};

static const httpd_uri_t camera_page_uri_capture = {
    .uri     = "/camera/capture",
    .method  = HTTP_GET,
    .handler = camera_page_h_capture,
};

static const httpd_uri_t camera_page_uri_status = {
    .uri     = "/camera/status",
    .method  = HTTP_GET,
    .handler = camera_page_h_status,
};

static const httpd_uri_t camera_page_uri_control = {
    .uri     = "/camera/control",
    .method  = HTTP_GET,
    .handler = camera_page_h_control,
};

static const httpd_uri_t camera_page_uri_settings = {
    .uri     = "/camera/settings",
    .method  = HTTP_POST,
    .handler = camera_page_h_settings,
};


/* Exposed API ------------------------------------------------------------- */

esp_err_t camera_page_init(void)
{
    esp_err_t error_code = ESP_OK;

    do
    {
        for (uint8_t i = 0u; i < CAMERA_PAGE_MAX_STREAMS; i++)
        {
            camera_page_stream_fds[i] = -1;
        }

        camera_page_lock         = xSemaphoreCreateMutex();
        camera_page_stopped      = xSemaphoreCreateBinary();
        camera_page_stream_queue = xQueueCreate(CAMERA_PAGE_MAX_STREAMS + 1u,
                                                sizeof(camera_page_viewer_t));

        if (!camera_page_lock || !camera_page_stopped || !camera_page_stream_queue)
        {
            error_code = ESP_ERR_NO_MEM;
            break;
        }

        const httpd_uri_t *uris[] = {
            &camera_page_uri_root,
            &camera_page_uri_stream,
            &camera_page_uri_capture,
            &camera_page_uri_status,
            &camera_page_uri_control,
            &camera_page_uri_settings,
        };

        for (size_t i = 0u; i < ARRAY_SIZE(uris); i++)
        {
            error_code = http_panel_server_register_uri(uris[i]);

            if (error_code != ESP_OK)
            {
                break;
            }
        }

        if (error_code != ESP_OK)
        {
            break;
        }

        error_code = http_panel_server_register_lifecycle_cb(camera_page_on_server);

        if (error_code != ESP_OK)
        {
            break;
        }

        const BaseType_t task_created = xTaskCreatePinnedToCore(
            camera_page_stream_task,
            "cam_stream",
            CAMERA_PAGE_STREAM_TASK_STACK,
            NULL,
            CAMERA_PAGE_STREAM_TASK_PRIO,
            NULL,
            CAMERA_PAGE_STREAM_TASK_CORE);

        error_code = (task_created == pdPASS) ? ESP_OK : ESP_ERR_NO_MEM;
    } while (0);

    return error_code;
}


/**** END OF FILE ****/
