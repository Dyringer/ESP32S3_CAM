/**
 ******************************************************************************
 * @file    http_panel_transfer.c
 * @author  Daniel Dyringer
 * @date    2026-10-25
 * @brief   Shared upload/download worker for http_panel pages.
 *
 *          The submitting handler detaches the request with
 *          httpd_req_async_handler_begin() and queues it; one worker task
 *          receives or sends the body and is the only caller of the page's
 *          ops, so WebSockets and other pages keep working meanwhile.
 *
 *          Async requests must complete before httpd_stop() frees the server,
 *          so on stop the lifecycle callback shuts the transfer socket down
 *          and waits for the worker to release the request.
 ******************************************************************************
 */


/* Includes ---------------------------------------------------------------- */
#include "http_panel_transfer.h"

#include <stdio.h>
#include <string.h>
#include <sys/socket.h>

#include "esp_attr.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "http_panel_server.h"
#include "utils.h"


/* Constants and macros ---------------------------------------------------- */
// clang-format off

#define HTTP_PANEL_TRANSFER_CHUNK               4096u
#define HTTP_PANEL_TRANSFER_MESSAGE_MAX         64u
#define HTTP_PANEL_TRANSFER_ERROR_BODY_MAX      96u

/* Below httpd (5), so serving other clients wins over transfers. Stack covers
 * esp_ota_end() image verification (needed 4096) and LittleFS through VFS and
 * newlib stdio (fopen/fwrite/fread/rename) in file ops, with headroom. */
#define HTTP_PANEL_TRANSFER_TASK_STACK          6144u
#define HTTP_PANEL_TRANSFER_TASK_PRIO           4u
/* Core 0 = network: the task is bound by TCP receive/send. */
#define HTTP_PANEL_TRANSFER_TASK_CORE           0

/* Consecutive httpd recv timeouts (5 s each) before the upload is dropped. */
#define HTTP_PANEL_TRANSFER_RECV_RETRIES        3u
#define HTTP_PANEL_TRANSFER_STOP_TIMEOUT_MS     5000u

#define HTTP_PANEL_TRANSFER_400                 "400 Bad Request"
#define HTTP_PANEL_TRANSFER_404                 "404 Not Found"
#define HTTP_PANEL_TRANSFER_409                 "409 Conflict"
#define HTTP_PANEL_TRANSFER_413                 "413 Payload Too Large"
#define HTTP_PANEL_TRANSFER_500                 "500 Internal Server Error"
#define HTTP_PANEL_TRANSFER_503                 "503 Service Unavailable"

// clang-format on

/* Private types ----------------------------------------------------------- */

/* fd is captured in the handler, where httpd_req_to_sockfd() is valid under
 * CONFIG_HTTPD_VALIDATE_REQ. */
typedef struct http_panel_transfer_job_s
{
    httpd_req_t* request; /* Async copy. */
    const http_panel_transfer_ops_t* ops;
    int fd;
} http_panel_transfer_job_t;

typedef union http_panel_transfer_context_u
{
    max_align_t alignment;
    uint8_t bytes[HTTP_PANEL_TRANSFER_CONTEXT_MAX];
} http_panel_transfer_context_t;


/* Private variables ------------------------------------------------------- */

static const char* http_panel_transfer_tag = "http_panel_transfer";

/* Holds at most one job: submit only queues while no transfer is active. */
static QueueHandle_t http_panel_transfer_queue = NULL;

/* Guards http_panel_transfer_accepting and http_panel_transfer_fd (httpd,
 * transfer and monitor tasks). */
static SemaphoreHandle_t http_panel_transfer_lock = NULL;

static bool http_panel_transfer_accepting = false;

/* Socket of the queued or running transfer; -1 = idle. */
static int http_panel_transfer_fd = -1;

/* Given by the transfer task, under http_panel_transfer_lock, after each job. */
static SemaphoreHandle_t http_panel_transfer_done = NULL;

/* Written by the httpd task in submit while idle (under the lock, before the
 * job is queued), then touched only by the transfer task until it goes idle. */
static http_panel_transfer_context_t http_panel_transfer_context = {0};

/* Touched only by the transfer task. */
/* PSRAM: never touch from an ISR or while the flash cache is disabled. */
EXT_RAM_BSS_ATTR static uint8_t http_panel_transfer_buffer[HTTP_PANEL_TRANSFER_CHUNK] = {0};


/* Private functions ------------------------------------------------------- */

static const char* http_panel_transfer_status(esp_err_t error)
{
    const char* status = NULL;

    switch (error)
    {
        case ESP_ERR_INVALID_ARG:
        {
            status = HTTP_PANEL_TRANSFER_400;
        }
        break;
        case ESP_ERR_NOT_FOUND:
        {
            status = HTTP_PANEL_TRANSFER_404;
        }
        break;
        case ESP_ERR_INVALID_STATE:
        {
            status = HTTP_PANEL_TRANSFER_409;
        }
        break;
        case ESP_ERR_INVALID_SIZE:
        {
            status = HTTP_PANEL_TRANSFER_413;
        }
        break;
        default:
        {
            status = HTTP_PANEL_TRANSFER_500;
        }
        break;
    }

    return status;
}

/* Returns ESP_FAIL so the socket gets closed: purging an unread body would
 * block the httpd task for the whole transfer. */
static esp_err_t http_panel_transfer_send_error(
    httpd_req_t* request,
    const char* status,
    const char* message)
{
    char body[HTTP_PANEL_TRANSFER_ERROR_BODY_MAX] = {0};

    ESP_LOGE(http_panel_transfer_tag, "Transfer failed: %s", message);

    httpd_resp_set_status(request, status);
    httpd_resp_set_type(request, "application/json");
    snprintf(body, sizeof(body), "{\"ok\":false,\"error\":\"%s\"}", message);
    httpd_resp_send(request, body, HTTPD_RESP_USE_STRLEN);

    return ESP_FAIL;
}

static bool http_panel_transfer_ops_valid(const http_panel_transfer_ops_t* ops)
{
    bool valid = false;

    if ((ops != NULL) && (ops->open != NULL) && (ops->close != NULL))
    {
        valid = ((ops->write != NULL) != (ops->read != NULL));
    }

    return valid;
}

/* ---- Upload ---- */

static esp_err_t http_panel_transfer_receive(
    const http_panel_transfer_job_t* job,
    char* message,
    size_t message_size)
{
    esp_err_t error_code = ESP_OK;
    size_t remaining     = job->request->content_len;
    uint8_t timeouts     = 0u;

    while ((error_code == ESP_OK) && (remaining > 0u))
    {
        const size_t want = MIN(remaining, sizeof(http_panel_transfer_buffer));
        const int received =
            httpd_req_recv(job->request, (char*)http_panel_transfer_buffer, want);

        if ((received == HTTPD_SOCK_ERR_TIMEOUT)
            && (timeouts < HTTP_PANEL_TRANSFER_RECV_RETRIES))
        {
            timeouts++;
            ESP_LOGW(
                http_panel_transfer_tag,
                "Receive timeout %u/%u, %u bytes left",
                (unsigned)timeouts,
                (unsigned)HTTP_PANEL_TRANSFER_RECV_RETRIES,
                (unsigned)remaining);
        }
        else if (received <= 0)
        {
            ESP_LOGE(http_panel_transfer_tag, "httpd_req_recv failed: %d", received);
            error_code = ESP_FAIL;
            snprintf(message, message_size, "upload read failed");
        }
        else
        {
            error_code = job->ops->write(
                http_panel_transfer_context.bytes,
                http_panel_transfer_buffer,
                (size_t)received);

            if (error_code != ESP_OK)
            {
                snprintf(
                    message, message_size, "write failed: %s", esp_err_to_name(error_code));
            }
            else
            {
                remaining -= (size_t)received;
                timeouts = 0u;
            }
        }
    }

    return error_code;
}

static esp_err_t http_panel_transfer_upload(const http_panel_transfer_job_t* job)
{
    char message[HTTP_PANEL_TRANSFER_MESSAGE_MAX] = {0};
    const char* status                            = HTTP_PANEL_TRANSFER_500;
    void* const context                           = http_panel_transfer_context.bytes;
    esp_err_t error_code                          = job->ops->open(context, job->request);

    if (error_code != ESP_OK)
    {
        status = http_panel_transfer_status(error_code);
        snprintf(message, sizeof(message), "open failed: %s", esp_err_to_name(error_code));
    }
    else
    {
        error_code = http_panel_transfer_receive(job, message, sizeof(message));

        const esp_err_t close_error = job->ops->close(context, (error_code == ESP_OK));

        if ((error_code == ESP_OK) && (close_error != ESP_OK))
        {
            error_code = close_error;
            snprintf(
                message, sizeof(message), "close failed: %s", esp_err_to_name(close_error));
        }
    }

    if (error_code != ESP_OK)
    {
        error_code = http_panel_transfer_send_error(job->request, status, message);
    }
    else
    {
        httpd_resp_set_type(job->request, "application/json");
        error_code = httpd_resp_sendstr(job->request, "{\"ok\":true}");
    }

    return error_code;
}

/* ---- Download ---- */

static esp_err_t http_panel_transfer_stream(
    const http_panel_transfer_job_t* job,
    char* message,
    size_t message_size,
    bool* started)
{
    esp_err_t error_code = ESP_OK;
    bool finished        = false;

    while ((error_code == ESP_OK) && !finished)
    {
        size_t length = 0u;

        error_code = job->ops->read(
            http_panel_transfer_context.bytes,
            http_panel_transfer_buffer,
            sizeof(http_panel_transfer_buffer),
            &length);

        if (error_code != ESP_OK)
        {
            snprintf(message, message_size, "read failed: %s", esp_err_to_name(error_code));
        }
        else if (length > sizeof(http_panel_transfer_buffer))
        {
            error_code = ESP_ERR_INVALID_SIZE;
            snprintf(message, message_size, "read overran the buffer");
        }
        else if (length == 0u)
        {
            finished = true;
        }
        else
        {
            *started   = true;
            error_code = httpd_resp_send_chunk(
                job->request, (const char*)http_panel_transfer_buffer, (ssize_t)length);

            if (error_code != ESP_OK)
            {
                snprintf(message, message_size, "send failed");
            }
        }
    }

    return error_code;
}

/* Once a chunk has gone out the 200 is committed, so later failures just drop
 * the connection (the missing terminating chunk marks the body incomplete). */
static esp_err_t http_panel_transfer_download(const http_panel_transfer_job_t* job)
{
    char message[HTTP_PANEL_TRANSFER_MESSAGE_MAX] = {0};
    bool started                                  = false;
    void* const context                           = http_panel_transfer_context.bytes;
    esp_err_t error_code                          = job->ops->open(context, job->request);

    if (error_code != ESP_OK)
    {
        snprintf(message, sizeof(message), "open failed: %s", esp_err_to_name(error_code));
    }
    else
    {
        error_code = http_panel_transfer_stream(job, message, sizeof(message), &started);

        /* Before the terminating chunk, so a close failure still truncates
         * the response. */
        const esp_err_t close_error = job->ops->close(context, (error_code == ESP_OK));

        if ((error_code == ESP_OK) && (close_error != ESP_OK))
        {
            error_code = close_error;
            snprintf(
                message, sizeof(message), "close failed: %s", esp_err_to_name(close_error));
        }

        if (error_code == ESP_OK)
        {
            error_code = httpd_resp_send_chunk(job->request, NULL, 0);
        }
    }

    if ((error_code != ESP_OK) && !started)
    {
        error_code = http_panel_transfer_send_error(
            job->request, http_panel_transfer_status(error_code), message);
    }
    else if (error_code != ESP_OK)
    {
        ESP_LOGE(http_panel_transfer_tag, "Download aborted: %s", message);
    }
    else
    {
        /* Complete. */
    }

    return error_code;
}

/* ---- Worker and lifecycle ---- */

static void http_panel_transfer_task(void* arg)
{
    ARG_UNUSED(arg);

    while (true)
    {
        http_panel_transfer_job_t job = {0};

        if (xQueueReceive(http_panel_transfer_queue, &job, portMAX_DELAY) == pdTRUE)
        {
            esp_err_t error_code = ESP_OK;

            if (job.ops->write != NULL)
            {
                error_code = http_panel_transfer_upload(&job);
            }
            else
            {
                error_code = http_panel_transfer_download(&job);
            }

            /* Before completing: complete() hands the fd back to httpd (higher
             * prio, same core), which would parse unread body bytes as a new
             * request; a later trigger_close could hit a reused fd. On EOF
             * httpd deletes the session itself. */
            if (error_code != ESP_OK)
            {
                shutdown(job.fd, SHUT_RDWR);
            }

            httpd_req_async_handler_complete(job.request); /* frees request */

            xSemaphoreTake(http_panel_transfer_lock, portMAX_DELAY);
            http_panel_transfer_fd = -1;
            xSemaphoreGive(http_panel_transfer_done);
            xSemaphoreGive(http_panel_transfer_lock);
        }
    }
}

/* Lifecycle — runs in the panel monitor task. */
static void http_panel_transfer_on_server(bool running)
{
    xSemaphoreTake(http_panel_transfer_lock, portMAX_DELAY);
    http_panel_transfer_accepting = running;

    const bool busy = (http_panel_transfer_fd >= 0);

    if (!running && busy)
    {
        /* Fails the pending receive/send instead of waiting out its timeout. */
        shutdown(http_panel_transfer_fd, SHUT_RDWR);
        xSemaphoreTake(http_panel_transfer_done, 0u); /* clear a stale give */
    }

    xSemaphoreGive(http_panel_transfer_lock);

    if (!running && busy)
    {
        if (xSemaphoreTake(
                http_panel_transfer_done, pdMS_TO_TICKS(HTTP_PANEL_TRANSFER_STOP_TIMEOUT_MS))
            != pdTRUE)
        {
            ESP_LOGE(http_panel_transfer_tag, "Transfer task did not release its request");
        }
    }
}


/* Exposed API ------------------------------------------------------------- */

esp_err_t http_panel_transfer_init(void)
{
    esp_err_t error_code = ESP_OK;

    do
    {
        http_panel_transfer_lock  = xSemaphoreCreateMutex();
        http_panel_transfer_done  = xSemaphoreCreateBinary();
        http_panel_transfer_queue = xQueueCreate(1u, sizeof(http_panel_transfer_job_t));

        if ((http_panel_transfer_lock == NULL) || (http_panel_transfer_done == NULL)
            || (http_panel_transfer_queue == NULL))
        {
            error_code = ESP_ERR_NO_MEM;
            break;
        }

        error_code = http_panel_server_register_lifecycle_cb(http_panel_transfer_on_server);

        if (error_code != ESP_OK)
        {
            break;
        }

        const BaseType_t task_created = xTaskCreatePinnedToCore(
            http_panel_transfer_task,
            "panel_xfer",
            HTTP_PANEL_TRANSFER_TASK_STACK,
            NULL,
            HTTP_PANEL_TRANSFER_TASK_PRIO,
            NULL,
            HTTP_PANEL_TRANSFER_TASK_CORE);

        error_code = (task_created == pdPASS) ? ESP_OK : ESP_ERR_NO_MEM;
    } while (0);

    if (error_code != ESP_OK)
    {
        ESP_LOGE(http_panel_transfer_tag, "Init failed: %s", esp_err_to_name(error_code));
    }

    return error_code;
}

esp_err_t http_panel_transfer_submit(
    httpd_req_t* request,
    const http_panel_transfer_ops_t* ops,
    const void* context,
    size_t context_size)
{
    esp_err_t error_code = ESP_OK;

    do
    {
        if (!http_panel_transfer_ops_valid(ops)
            || (context_size > sizeof(http_panel_transfer_context.bytes))
            || ((context == NULL) && (context_size > 0u)))
        {
            ESP_LOGE(
                http_panel_transfer_tag,
                "Invalid submit: ops or context (%u bytes)",
                (unsigned)context_size);
            error_code = http_panel_transfer_send_error(
                request, HTTP_PANEL_TRANSFER_500, "invalid transfer");
            break;
        }

        xSemaphoreTake(http_panel_transfer_lock, portMAX_DELAY);

        if (!http_panel_transfer_accepting)
        {
            xSemaphoreGive(http_panel_transfer_lock);
            error_code = http_panel_transfer_send_error(
                request, HTTP_PANEL_TRANSFER_503, "server stopping");
            break;
        }

        if (http_panel_transfer_fd >= 0)
        {
            xSemaphoreGive(http_panel_transfer_lock);
            error_code = http_panel_transfer_send_error(
                request, HTTP_PANEL_TRANSFER_409, "transfer in progress");
            break;
        }

        http_panel_transfer_job_t job = {
            .request = NULL,
            .ops     = ops,
            .fd      = httpd_req_to_sockfd(request),
        };

        if (httpd_req_async_handler_begin(request, &job.request) != ESP_OK)
        {
            xSemaphoreGive(http_panel_transfer_lock);
            error_code = http_panel_transfer_send_error(
                request, HTTP_PANEL_TRANSFER_500, "no memory");
            break;
        }

        if (context_size > 0u)
        {
            memcpy(http_panel_transfer_context.bytes, context, context_size);
        }

        /* No transfer active, so the one-slot queue is empty. */
        xQueueSend(http_panel_transfer_queue, &job, 0u);
        http_panel_transfer_fd = job.fd;

        xSemaphoreGive(http_panel_transfer_lock);
    } while (0);

    return error_code;
}

/**** END OF FILE ****/
