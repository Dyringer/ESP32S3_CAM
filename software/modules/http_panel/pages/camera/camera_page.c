/**
 ******************************************************************************
 * @file    camera_page.c
 * @author  Daniel Dyringer
 * @date    2026-09-30
 * @brief   Camera page — live MJPEG view, snapshot and sensor settings.
 *
 *          Streaming never blocks the httpd task: requests are detached with
 *          httpd_req_async_handler_begin() and served by a dedicated stream
 *          task, which sends each frame to every viewer.
 *
 *          Async requests must complete before httpd_stop() frees the server,
 *          so on stop the lifecycle callback refuses new viewers, shuts the
 *          stream sockets down and waits for the task to release them.
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

#include "cJSON.h"
#include "camera.h"
#include "camera_settings.h"
#include "data_manager.h"
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

/* Each viewer costs a socket and WiFi bandwidth — every frame goes to all. */
#define CAMERA_PAGE_MAX_STREAMS         2u

#define CAMERA_PAGE_STREAM_TASK_STACK   3584u
#define CAMERA_PAGE_STREAM_TASK_PRIO    4u
/* Core 0 belongs to WiFi. */
#define CAMERA_PAGE_STREAM_TASK_CORE    1

#define CAMERA_PAGE_STOP_TIMEOUT_MS     5000u
#define CAMERA_PAGE_RETRY_DELAY_MS      50u

#define CAMERA_PAGE_QUERY_LEN           64u
#define CAMERA_PAGE_VAR_LEN             32u
#define CAMERA_PAGE_VALUE_LEN           16u
#define CAMERA_PAGE_PART_HEAD_MAX       96u

/* The page uses setting names without the "camera_" prefix. */
#define CAMERA_PAGE_PREFIX_LEN          (sizeof(CAMERA_SETTINGS_PREFIX) - 1u)

#define CAMERA_PAGE_PART_BOUNDARY       "esp32s3camframe"
#define CAMERA_PAGE_STREAM_TYPE         \
    "multipart/x-mixed-replace;boundary=" CAMERA_PAGE_PART_BOUNDARY
#define CAMERA_PAGE_STREAM_BOUNDARY     "\r\n--" CAMERA_PAGE_PART_BOUNDARY "\r\n"
#define CAMERA_PAGE_STREAM_PART         \
    "Content-Type: image/jpeg\r\nContent-Length: %u\r\n\r\n"

#define CAMERA_PAGE_503                 "503 Service Unavailable"

// clang-format on

/* Private types ----------------------------------------------------------- */

/* fd is captured in the handler, where httpd_req_to_sockfd() is valid under
 * CONFIG_HTTPD_VALIDATE_REQ. */
typedef struct camera_page_viewer_s
{
    httpd_req_t* request; /* Async copy; NULL = stop wake-up. */
    int fd;
} camera_page_viewer_t;


/* Private variables ------------------------------------------------------- */

static const char* camera_page_tag = "camera_page";

static QueueHandle_t camera_page_stream_queue = NULL;

/* Guards camera_page_accepting and camera_page_stream_fds (httpd, stream and
 * monitor tasks). */
static SemaphoreHandle_t camera_page_lock = NULL;

static bool camera_page_accepting = false;

/* Accepted viewers, queued or streaming; -1 = free. */
static int camera_page_stream_fds[CAMERA_PAGE_MAX_STREAMS] = {0};

static SemaphoreHandle_t camera_page_stopped = NULL;

static volatile uint32_t camera_page_fps_x10 = 0u;

extern const uint8_t camera_page_html_start[] asm("_binary_camera_page_html_start");
extern const uint8_t camera_page_html_end[] asm("_binary_camera_page_html_end");


/* Private functions ------------------------------------------------------- */

static esp_err_t camera_page_send_503(httpd_req_t* request, const char* message)
{
    httpd_resp_set_status(request, CAMERA_PAGE_503);
    httpd_resp_set_type(request, "text/plain");

    return httpd_resp_sendstr(request, message);
}

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

/* ---- Stream task ---- */

static esp_err_t camera_page_send_part(httpd_req_t* request, const camera_frame_t* frame)
{
    char head[CAMERA_PAGE_PART_HEAD_MAX] = {0};

    const int head_length = snprintf(
        head,
        sizeof(head),
        CAMERA_PAGE_STREAM_BOUNDARY CAMERA_PAGE_STREAM_PART,
        (unsigned)frame->length);

    esp_err_t error_code = httpd_resp_send_chunk(request, head, (ssize_t)head_length);

    if (error_code == ESP_OK)
    {
        error_code = httpd_resp_send_chunk(
            request, (const char*)frame->buffer, (ssize_t)frame->length);
    }

    return error_code;
}

/* The socket is closed because the chunked response never ended. */
static void camera_page_stream_drop(camera_page_viewer_t* viewers, uint8_t* count, uint8_t index)
{
    httpd_req_t* request        = viewers[index].request;
    const httpd_handle_t server = request->handle;
    const int fd                = viewers[index].fd;

    httpd_req_async_handler_complete(request); /* frees request */
    httpd_sess_trigger_close(server, fd);

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
        camera_light_enable(false);
        camera_page_fps_x10 = 0u;
    }
}

static void camera_page_stream_task(void* arg)
{
    ARG_UNUSED(arg);

    camera_page_viewer_t viewers[CAMERA_PAGE_MAX_STREAMS] = {0};
    uint8_t count                                         = 0u;
    int64_t window_start                                  = 0;
    uint32_t window_frames                                = 0u;

    while (true)
    {
        camera_page_viewer_t incoming = {0};
        TickType_t wait               = (count == 0u) ? portMAX_DELAY : 0u;
        bool stop                     = false;

        while (xQueueReceive(camera_page_stream_queue, &incoming, wait) == pdTRUE)
        {
            wait = 0u;

            if (incoming.request == NULL)
            {
                stop = true;
            }
            else if (count < CAMERA_PAGE_MAX_STREAMS)
            {
                viewers[count] = incoming;
                count++;

                ESP_LOGI(
                    camera_page_tag,
                    "Viewer joined fd=%d (%u watching)",
                    incoming.fd,
                    (unsigned)count);

                if (count == 1u)
                {
                    camera_light_enable(true);
                    window_start  = esp_timer_get_time();
                    window_frames = 0u;
                }
            }
            else
            {
                /* Not reached: the stream handler enforces the viewer limit. */
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

        camera_frame_t frame = {0};

        if (camera_frame_acquire(&frame) != ESP_OK)
        {
            vTaskDelay(pdMS_TO_TICKS(CAMERA_PAGE_RETRY_DELAY_MS));
            continue;
        }

        /* Walk backwards so dropping a viewer does not skip the next one. */
        uint8_t i = count;

        while (i > 0u)
        {
            i--;

            if (camera_page_send_part(viewers[i].request, &frame) != ESP_OK)
            {
                camera_page_stream_drop(viewers, &count, i);
            }
        }

        camera_frame_release(&frame);

        window_frames++;

        const int64_t elapsed = esp_timer_get_time() - window_start;

        if (elapsed >= 1000000LL)
        {
            if (count > 0u)
            {
                camera_page_fps_x10 =
                    (uint32_t)(((int64_t)window_frames * 10000000LL) / elapsed);
            }
            else
            {
                camera_page_fps_x10 = 0u;
            }

            window_start += elapsed;
            window_frames = 0u;
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
        /* Unblock any send in progress instead of waiting out the send timeout. */
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

    /* Nothing can enqueue past this point, and the stream task drains the
     * whole queue (viewers ahead of the wake-up included) before dropping. */
    if (!running && busy)
    {
        xSemaphoreTake(camera_page_stopped, 0u); /* clear a stale give */

        const camera_page_viewer_t wake = {.request = NULL, .fd = -1};
        xQueueSend(camera_page_stream_queue, &wake, 0u);

        if (xSemaphoreTake(camera_page_stopped, pdMS_TO_TICKS(CAMERA_PAGE_STOP_TIMEOUT_MS))
            != pdTRUE)
        {
            ESP_LOGE(camera_page_tag, "Stream task did not release its viewers");
        }
    }
}

/* ---- HTTP handlers ---- */

static esp_err_t camera_page_h_root(httpd_req_t* request)
{
    const size_t length = (size_t)(camera_page_html_end - camera_page_html_start);

    httpd_resp_set_type(request, "text/html");

    return httpd_resp_send(request, (const char*)camera_page_html_start, (ssize_t)length);
}

static esp_err_t camera_page_h_stream(httpd_req_t* request)
{
    esp_err_t error_code = ESP_OK;

    do
    {
        if (!camera_is_ready())
        {
            error_code = camera_page_send_503(request, "Camera not available");
            break;
        }

        /* Headers are copied into the detached request by begin(). */
        httpd_resp_set_type(request, CAMERA_PAGE_STREAM_TYPE);
        httpd_resp_set_hdr(request, "Cache-Control", "no-store");

        xSemaphoreTake(camera_page_lock, portMAX_DELAY);

        if (!camera_page_accepting
            || (camera_page_viewer_count_locked() >= CAMERA_PAGE_MAX_STREAMS))
        {
            xSemaphoreGive(camera_page_lock);
            error_code = camera_page_send_503(request, "Viewer limit reached");
            break;
        }

        camera_page_viewer_t viewer = {
            .request = NULL,
            .fd      = httpd_req_to_sockfd(request),
        };

        if (httpd_req_async_handler_begin(request, &viewer.request) != ESP_OK)
        {
            xSemaphoreGive(camera_page_lock);
            error_code = httpd_resp_send_500(request);
            break;
        }

        /* Queue holds MAX_STREAMS + 1 items, so this cannot fail. */
        xQueueSend(camera_page_stream_queue, &viewer, 0u);

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

static esp_err_t camera_page_h_capture(httpd_req_t* request)
{
    esp_err_t error_code = ESP_OK;
    camera_frame_t frame = {0};

    do
    {
        if (!camera_is_ready())
        {
            error_code = camera_page_send_503(request, "Camera not available");
            break;
        }

        if (camera_still_acquire(&frame) != ESP_OK)
        {
            error_code = httpd_resp_send_500(request);
            break;
        }

        httpd_resp_set_type(request, "image/jpeg");
        httpd_resp_set_hdr(request, "Content-Disposition", "inline; filename=capture.jpg");
        httpd_resp_set_hdr(request, "Cache-Control", "no-store");

        error_code =
            httpd_resp_send(request, (const char*)frame.buffer, (ssize_t)frame.length);

        ESP_LOGI(
            camera_page_tag,
            "Still %ux%u, %u bytes",
            (unsigned)frame.width,
            (unsigned)frame.height,
            (unsigned)frame.length);

        camera_frame_release(&frame);
    } while (0);

    return error_code;
}

static char* camera_page_build_status(void)
{
    char* json  = NULL;
    cJSON* root = NULL;

    do
    {
        root = cJSON_CreateObject();

        if (root == NULL)
        {
            break;
        }

        xSemaphoreTake(camera_page_lock, portMAX_DELAY);
        const uint8_t viewers = camera_page_viewer_count_locked();
        xSemaphoreGive(camera_page_lock);

        cJSON_AddBoolToObject(root, "ready", camera_is_ready());
        cJSON_AddBoolToObject(root, "unsaved", data_manager_unsaved());
        cJSON_AddStringToObject(root, "sensor", camera_sensor_name());
        cJSON_AddNumberToObject(root, "viewers", (double)viewers);
        cJSON_AddNumberToObject(root, "max_viewers", (double)CAMERA_PAGE_MAX_STREAMS);
        cJSON_AddNumberToObject(root, "fps", (double)camera_page_fps_x10 / 10.0);

        hp_add_cpu_load(root);

        cJSON* settings = cJSON_AddObjectToObject(root, "settings");

        const size_t count = data_manager_count();

        for (size_t i = 0u; i < count; i++)
        {
            const data_manager_descriptor_t* const descriptor = data_manager_at(i);
            int32_t value                                     = 0;

            if ((descriptor == NULL)
                || (strncmp(descriptor->name, CAMERA_SETTINGS_PREFIX, CAMERA_PAGE_PREFIX_LEN)
                    != 0)
                || (descriptor->type != DATA_MANAGER_TYPE_I32)
                || (data_manager_get(descriptor->id, &value, sizeof(value)) != ESP_OK))
            {
                continue;
            }

            cJSON* item = cJSON_AddObjectToObject(
                settings, &descriptor->name[CAMERA_PAGE_PREFIX_LEN]);
            cJSON_AddNumberToObject(item, "value", (double)value);
            cJSON_AddNumberToObject(item, "min", (double)descriptor->minimum);
            cJSON_AddNumberToObject(item, "max", (double)descriptor->maximum);
        }

        cJSON* sizes    = cJSON_AddArrayToObject(root, "framesizes");
        uint16_t width  = 0u;
        uint16_t height = 0u;

        for (int framesize = 0;
             camera_framesize_resolution(framesize, &width, &height) == ESP_OK;
             framesize++)
        {
            cJSON* pair = cJSON_CreateArray();
            cJSON_AddItemToArray(pair, cJSON_CreateNumber((double)width));
            cJSON_AddItemToArray(pair, cJSON_CreateNumber((double)height));
            cJSON_AddItemToArray(sizes, pair);
        }

        json = cJSON_PrintUnformatted(root);
    } while (0);

    cJSON_Delete(root);

    return json; /* caller must free() */
}

static esp_err_t camera_page_h_status(httpd_req_t* request)
{
    esp_err_t error_code = ESP_OK;
    char* json           = camera_page_build_status();

    if (json == NULL)
    {
        error_code = httpd_resp_send_500(request);
    }
    else
    {
        httpd_resp_set_type(request, "application/json");
        httpd_resp_set_hdr(request, "Cache-Control", "no-store");
        error_code = httpd_resp_sendstr(request, json);
        free(json);
    }

    return error_code;
}

static esp_err_t camera_page_h_control(httpd_req_t* request)
{
    esp_err_t error_code = ESP_OK;

    do
    {
        char query[CAMERA_PAGE_QUERY_LEN]      = {0};
        char name[CAMERA_PAGE_VAR_LEN]         = {0};
        char value_text[CAMERA_PAGE_VALUE_LEN] = {0};

        if ((httpd_req_get_url_query_str(request, query, sizeof(query)) != ESP_OK)
            || (httpd_query_key_value(query, "var", name, sizeof(name)) != ESP_OK)
            || (httpd_query_key_value(query, "val", value_text, sizeof(value_text))
                != ESP_OK))
        {
            error_code = httpd_resp_send_err(
                request, HTTPD_400_BAD_REQUEST, "Expected ?var=<name>&val=<int>");
            break;
        }

        char* end        = NULL;
        const long value = strtol(value_text, &end, 10);

        if ((end == value_text) || (*end != '\0'))
        {
            error_code = httpd_resp_send_err(
                request, HTTPD_400_BAD_REQUEST, "val must be an integer");
            break;
        }

        char full_name[sizeof(CAMERA_SETTINGS_PREFIX) + CAMERA_PAGE_VAR_LEN] = {0};

        snprintf(full_name, sizeof(full_name), "%s%s", CAMERA_SETTINGS_PREFIX, name);

        const data_manager_descriptor_t* const descriptor = data_manager_find(full_name);
        const int32_t setting_value                       = (int32_t)value;
        esp_err_t result                                  = ESP_ERR_NOT_FOUND;

        if ((descriptor != NULL) && (descriptor->type == DATA_MANAGER_TYPE_I32))
        {
            result =
                data_manager_set(descriptor->id, &setting_value, sizeof(setting_value));
        }

        switch (result)
        {
            case ESP_OK:
            {
                error_code = httpd_resp_sendstr(request, "OK");
            }
            break;
            case ESP_ERR_NOT_FOUND:
            {
                error_code =
                    httpd_resp_send_err(request, HTTPD_404_NOT_FOUND, "Unknown setting");
            }
            break;
            case ESP_ERR_INVALID_ARG:
            {
                error_code = httpd_resp_send_err(
                    request, HTTPD_400_BAD_REQUEST, "Value out of range");
            }
            break;
            case ESP_ERR_INVALID_STATE:
            {
                error_code =
                    camera_page_send_503(request, "Camera settings not available");
            }
            break;
            default:
            {
                error_code = httpd_resp_send_err(
                    request, HTTPD_500_INTERNAL_SERVER_ERROR, esp_err_to_name(result));
            }
            break;
        }
    } while (0);

    return error_code;
}

static esp_err_t camera_page_h_settings(httpd_req_t* request)
{
    esp_err_t error_code = ESP_OK;

    do
    {
        char query[CAMERA_PAGE_QUERY_LEN] = {0};
        char action[CAMERA_PAGE_VAR_LEN]  = {0};

        if ((httpd_req_get_url_query_str(request, query, sizeof(query)) != ESP_OK)
            || (httpd_query_key_value(query, "action", action, sizeof(action)) != ESP_OK))
        {
            error_code = httpd_resp_send_err(
                request, HTTPD_400_BAD_REQUEST, "Expected ?action=save|defaults");
            break;
        }

        esp_err_t result = ESP_OK;

        if (strcmp(action, "save") == 0)
        {
            result = data_manager_save();
        }
        else if (strcmp(action, "defaults") == 0)
        {
            result = camera_settings_restore_defaults();
        }
        else
        {
            error_code =
                httpd_resp_send_err(request, HTTPD_400_BAD_REQUEST, "Unknown action");
            break;
        }

        if (result == ESP_ERR_INVALID_STATE)
        {
            error_code = camera_page_send_503(request, "Camera not available");
        }
        else if (result != ESP_OK)
        {
            error_code = httpd_resp_send_err(
                request, HTTPD_500_INTERNAL_SERVER_ERROR, esp_err_to_name(result));
        }
        else
        {
            error_code = httpd_resp_sendstr(request, "OK");
        }
    } while (0);

    return error_code;
}


/* Exposed API ------------------------------------------------------------- */

esp_err_t camera_page_init(void)
{
    esp_err_t error_code = ESP_OK;

    /* Static: http_panel_server keeps the URIs by pointer. */
    static const httpd_uri_t camera_page_uris[] = {
        {
            .uri     = "/camera",
            .method  = HTTP_GET,
            .handler = camera_page_h_root,
        },
        {
            .uri     = "/camera/stream",
            .method  = HTTP_GET,
            .handler = camera_page_h_stream,
        },
        {
            .uri     = "/camera/capture",
            .method  = HTTP_GET,
            .handler = camera_page_h_capture,
        },
        {
            .uri     = "/camera/status",
            .method  = HTTP_GET,
            .handler = camera_page_h_status,
        },
        {
            .uri     = "/camera/control",
            .method  = HTTP_GET,
            .handler = camera_page_h_control,
        },
        {
            .uri     = "/camera/settings",
            .method  = HTTP_POST,
            .handler = camera_page_h_settings,
        },
    };

    do
    {
        for (uint8_t i = 0u; i < CAMERA_PAGE_MAX_STREAMS; i++)
        {
            camera_page_stream_fds[i] = -1;
        }

        camera_page_lock    = xSemaphoreCreateMutex();
        camera_page_stopped = xSemaphoreCreateBinary();
        camera_page_stream_queue =
            xQueueCreate(CAMERA_PAGE_MAX_STREAMS + 1u, sizeof(camera_page_viewer_t));

        if ((camera_page_lock == NULL) || (camera_page_stopped == NULL)
            || (camera_page_stream_queue == NULL))
        {
            error_code = ESP_ERR_NO_MEM;
            break;
        }

        for (size_t i = 0u; i < ARRAY_SIZE(camera_page_uris); i++)
        {
            error_code = http_panel_server_register_uri(&camera_page_uris[i]);

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
