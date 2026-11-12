/**
 ******************************************************************************
 * @file    http_panel_server.c
 * @author  Daniel Dyringer
 * @date    2026-03-09
 * @brief   HTTP panel core — server lifecycle, WebSocket client table, and
 *          push API.
 ******************************************************************************
 */


/* Includes ---------------------------------------------------------------- */
#include "http_panel_server.h"

#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "utils.h"
#include "wifi_connection.h"
#include "work_queue.h"


/* Constants and macros ---------------------------------------------------- */
// clang-format off

/* WiFi changes trigger the check right away; the period only catches a
 * dropped change event. A failed start is retried sooner. */
#define HTTP_PANEL_SERVER_CHECK_PERIOD_MS      5000u
#define HTTP_PANEL_SERVER_RETRY_PERIOD_MS      1000u

/* HTTPD_DEFAULT_CONFIG() leaves httpd unpinned. */
#define HTTP_PANEL_SERVER_TASK_PRIO            5u
#define HTTP_PANEL_SERVER_TASK_CORE            0

#define HTTP_PANEL_SERVER_MAX_URIS             40u
#define HTTP_PANEL_SERVER_MAX_LIFECYCLE_CBS    4u
#define HTTP_PANEL_SERVER_MAX_WS_CLOSE_CBS     4u
#define HTTP_PANEL_SERVER_WS_URI_LEN           32u

/* Longer than httpd's 5 s send timeout, so a slow but live send is never
 * counted as lost and queued twice. */
#define HTTP_PANEL_SERVER_PUSH_LOST_MS         8000u
#define HTTP_PANEL_SERVER_US_PER_MS            1000

/* Every holder of the WS mutex is short and non-blocking. */
#define HTTP_PANEL_SERVER_MUTEX_WAIT_MS        200u

/* Bigger payloads (smart_io stream batches) are copied into PSRAM. */
#define HTTP_PANEL_SERVER_PSRAM_PAYLOAD_MIN    4096u

/* TCP keep-alive reaps clients that vanished without a FIN (sleeping phone,
 * dropped WiFi): ~10 + 3 * 5 s. Pages that rarely push (OTA) would otherwise
 * hold a WS slot until reboot. */
#define HTTP_PANEL_SERVER_KEEPALIVE_IDLE_S     10
#define HTTP_PANEL_SERVER_KEEPALIVE_INTERVAL_S 5
#define HTTP_PANEL_SERVER_KEEPALIVE_COUNT      3

// clang-format on

/* Private types ----------------------------------------------------------- */

typedef struct http_panel_server_ws_client_s
{
    int fd; /**< -1 = free slot */
    char uri[HTTP_PANEL_SERVER_WS_URI_LEN];
} http_panel_server_ws_client_t;

typedef struct http_panel_server_push_arg_s
{
    httpd_handle_t server;
    int fd;
    const char* uri; /**< NULL = any WS client */
    char* payload;
    size_t length;
    http_panel_push_slot_t* slot; /**< NULL = no flow control */
    httpd_ws_type_t type;
} http_panel_server_push_arg_t;


/* Private variables ------------------------------------------------------- */

static const char* http_panel_server_tag = "http_panel";

/* Guarded by http_panel_server_ws_mutex, together with the client table. It is
 * cleared before httpd_stop() so no push can use a handle being freed. */
static httpd_handle_t http_panel_server_httpd = NULL;
static http_panel_server_ws_client_t
    http_panel_server_ws_clients[HTTP_PANEL_SERVER_MAX_WS_CLIENTS];
static SemaphoreHandle_t http_panel_server_ws_mutex = NULL;

static const httpd_uri_t* http_panel_server_uris[HTTP_PANEL_SERVER_MAX_URIS];
static uint8_t http_panel_server_uri_count = 0u;

static http_panel_server_lifecycle_cb_t
    http_panel_server_lifecycle_callbacks[HTTP_PANEL_SERVER_MAX_LIFECYCLE_CBS];
static uint8_t http_panel_server_lifecycle_callback_count = 0u;

static http_panel_server_ws_close_cb_t
    http_panel_server_ws_close_callbacks[HTTP_PANEL_SERVER_MAX_WS_CLOSE_CBS];
static uint8_t http_panel_server_ws_close_callback_count = 0u;

/* Touched only by the work_queue task. */
static work_queue_item_t http_panel_server_work = {0};
static bool http_panel_server_running           = false;


/* Private functions ------------------------------------------------------- */

static void http_panel_server_close_fn(httpd_handle_t server, int sockfd)
{
    ARG_UNUSED(server);
    ESP_LOGI(http_panel_server_tag, "Socket closed fd=%d", sockfd);
    http_panel_server_ws_remove(sockfd);

    for (uint8_t i = 0u; i < http_panel_server_ws_close_callback_count; i++)
    {
        http_panel_server_ws_close_callbacks[i](sockfd);
    }

    close(sockfd);
}

static void http_panel_server_notify(bool running)
{
    for (uint8_t i = 0u; i < http_panel_server_lifecycle_callback_count; i++)
    {
        http_panel_server_lifecycle_callbacks[i](running);
    }
}

/* Caller holds http_panel_server_ws_mutex. NULL ws_uri matches any client. */
static bool http_panel_server_ws_matches(
    const http_panel_server_ws_client_t* client,
    const char* ws_uri)
{
    return (client->fd >= 0) && ((ws_uri == NULL) || (strcmp(client->uri, ws_uri) == 0));
}

/* Caller holds http_panel_server_ws_mutex. */
static bool http_panel_server_ws_registered_locked(int fd, const char* ws_uri)
{
    bool match = false;

    for (uint8_t i = 0u; i < HTTP_PANEL_SERVER_MAX_WS_CLIENTS; i++)
    {
        if (http_panel_server_ws_clients[i].fd == fd)
        {
            match = http_panel_server_ws_matches(&http_panel_server_ws_clients[i], ws_uri);
            break;
        }
    }

    return match;
}

static bool http_panel_server_ws_registered(int fd, const char* ws_uri)
{
    bool match = false;

    if (xSemaphoreTake(
            http_panel_server_ws_mutex, pdMS_TO_TICKS(HTTP_PANEL_SERVER_MUTEX_WAIT_MS))
        == pdTRUE)
    {
        match = http_panel_server_ws_registered_locked(fd, ws_uri);
        xSemaphoreGive(http_panel_server_ws_mutex);
    }

    return match;
}

static void http_panel_server_slot_acquire(http_panel_push_slot_t* slot)
{
    slot->since_us = esp_timer_get_time();
    __atomic_add_fetch(&slot->in_flight, 1u, __ATOMIC_SEQ_CST);
}

/* A slot reset by push_ready() after a lost push may see a late completion;
 * the count never goes below zero. Returns true when nothing is left in
 * flight. */
static bool http_panel_server_slot_release(http_panel_push_slot_t* slot)
{
    uint32_t current = __atomic_load_n(&slot->in_flight, __ATOMIC_SEQ_CST);

    while (
        (current > 0u)
        && !__atomic_compare_exchange_n(
            &slot->in_flight, &current, current - 1u, false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST))
    {
    }

    return __atomic_load_n(&slot->in_flight, __ATOMIC_SEQ_CST) == 0u;
}

/* The fd may have closed and been reused by another connection between
 * queueing and running the push. httpd task only. */
static bool http_panel_server_ws_is(httpd_handle_t server, int fd, const char* ws_uri)
{
    return (httpd_ws_get_fd_info(server, fd) == HTTPD_WS_CLIENT_WEBSOCKET)
           && http_panel_server_ws_registered(fd, ws_uri);
}

static void http_panel_server_push_work(void* arg)
{
    http_panel_server_push_arg_t* const push_arg = (http_panel_server_push_arg_t*)arg;

    if (http_panel_server_ws_is(push_arg->server, push_arg->fd, push_arg->uri))
    {
        httpd_ws_frame_t frame = {
            .type    = push_arg->type,
            .payload = (uint8_t*)push_arg->payload,
            .len     = push_arg->length,
        };

        const esp_err_t send_result =
            httpd_ws_send_frame_async(push_arg->server, push_arg->fd, &frame);

        if (send_result != ESP_OK)
        {
            ESP_LOGW(
                http_panel_server_tag,
                "ws send failed fd=%d: %s — closing client",
                push_arg->fd,
                esp_err_to_name(send_result));
            http_panel_server_ws_remove(push_arg->fd);

            /* Unregistering alone leaves the socket open: the page would show
             * "Connected" with frozen values and never reconnect. */
            httpd_sess_trigger_close(push_arg->server, push_arg->fd);
        }
    }

    /* Released only after the send so the next push carries fresh data. */
    if (push_arg->slot != NULL)
    {
        const bool idle = http_panel_server_slot_release(push_arg->slot);

        if (push_arg->slot->waiter != NULL)
        {
            xTaskNotifyGive(push_arg->slot->waiter);
        }

        if (idle && (push_arg->slot->done != NULL))
        {
            push_arg->slot->done(push_arg->slot->context);
        }
    }

    heap_caps_free(push_arg->payload);
    free(push_arg);
}

static void* http_panel_server_payload_alloc(size_t length)
{
    void* payload = NULL;

    if (length >= HTTP_PANEL_SERVER_PSRAM_PAYLOAD_MIN)
    {
        payload = heap_caps_malloc(length, MALLOC_CAP_SPIRAM);
    }

    if (payload == NULL)
    {
        payload = heap_caps_malloc(length, MALLOC_CAP_DEFAULT);
    }

    return payload;
}

/* Copies data and queues one send to fd. When slot is given it counts the push
 * as in flight before queueing and releases it again if queueing fails.
 * Caller holds http_panel_server_ws_mutex, which keeps server valid. */
static esp_err_t http_panel_server_queue_push(
    httpd_handle_t server,
    int fd,
    const char* ws_uri,
    const void* data,
    size_t length,
    httpd_ws_type_t type,
    http_panel_push_slot_t* slot)
{
    esp_err_t error_code = ESP_OK;
    http_panel_server_push_arg_t* const push_arg =
        malloc(sizeof(http_panel_server_push_arg_t));

    do
    {
        if (push_arg == NULL)
        {
            error_code = ESP_ERR_NO_MEM;
            break;
        }

        push_arg->payload = http_panel_server_payload_alloc(length);

        if (push_arg->payload == NULL)
        {
            error_code = ESP_ERR_NO_MEM;
            break;
        }

        push_arg->server = server;
        push_arg->fd     = fd;
        push_arg->uri    = ws_uri;
        push_arg->length = length;
        push_arg->slot   = slot;
        push_arg->type   = type;
        memcpy(push_arg->payload, data, length);

        if (slot != NULL)
        {
            http_panel_server_slot_acquire(slot);
        }

        error_code = httpd_queue_work(server, http_panel_server_push_work, push_arg);

        if ((error_code != ESP_OK) && (slot != NULL))
        {
            http_panel_server_slot_release(slot);
        }
    } while (0);

    if ((error_code != ESP_OK) && (push_arg != NULL))
    {
        heap_caps_free(push_arg->payload);
        free(push_arg);
    }

    return error_code;
}

static bool http_panel_server_start(void)
{
    httpd_handle_t server = NULL;

    do
    {
        httpd_config_t config = HTTPD_DEFAULT_CONFIG();
        config.server_port    = HTTP_PANEL_SERVER_PORT;
        config.stack_size     = HTTP_PANEL_SERVER_TASK_STACK;
        config.task_priority  = HTTP_PANEL_SERVER_TASK_PRIO;
        config.core_id        = HTTP_PANEL_SERVER_TASK_CORE;
        /* LRU purge can evict an idle WS socket before its upgrade handshake
         * reaches the handler. Without it a full table refuses new
         * connections instead of killing live ones. */
        config.lru_purge_enable    = false;
        config.max_open_sockets    = HTTP_PANEL_SERVER_MAX_OPEN_SOCKETS;
        config.max_uri_handlers    = HTTP_PANEL_SERVER_MAX_URIS;
        config.close_fn            = http_panel_server_close_fn;
        config.keep_alive_enable   = true;
        config.keep_alive_idle     = HTTP_PANEL_SERVER_KEEPALIVE_IDLE_S;
        config.keep_alive_interval = HTTP_PANEL_SERVER_KEEPALIVE_INTERVAL_S;
        config.keep_alive_count    = HTTP_PANEL_SERVER_KEEPALIVE_COUNT;

        const esp_err_t error_code = httpd_start(&server, &config);

        if (error_code != ESP_OK)
        {
            ESP_LOGE(
                http_panel_server_tag,
                "httpd_start failed: %s — retrying",
                esp_err_to_name(error_code));
            break;
        }

        for (uint8_t i = 0u; i < http_panel_server_uri_count; i++)
        {
            const esp_err_t register_result =
                httpd_register_uri_handler(server, http_panel_server_uris[i]);

            ESP_LOGI(
                http_panel_server_tag,
                "register URI '%s' (%s) -> %s",
                http_panel_server_uris[i]->uri,
                http_panel_server_uris[i]->is_websocket ? "ws" : "http",
                esp_err_to_name(register_result));
        }

        ESP_LOGI(
            http_panel_server_tag,
            "Server up on port %u, %u handler(s)",
            HTTP_PANEL_SERVER_PORT,
            (unsigned)http_panel_server_uri_count);

        xSemaphoreTake(http_panel_server_ws_mutex, portMAX_DELAY);
        http_panel_server_httpd = server;
        xSemaphoreGive(http_panel_server_ws_mutex);

        http_panel_server_notify(true);
    } while (0);

    return server != NULL;
}

static void http_panel_server_stop(void)
{
    if (http_panel_server_running)
    {
        /* Let pages finish their async requests while the handle is valid. */
        http_panel_server_notify(false);

        /* Pushes hold the mutex while they use the handle, so after this
         * block none can start or still be running on it. No timeout: leaving
         * the handle published would be a use after free. */
        xSemaphoreTake(http_panel_server_ws_mutex, portMAX_DELAY);
        const httpd_handle_t server = http_panel_server_httpd;
        http_panel_server_httpd     = NULL;

        for (uint8_t i = 0u; i < HTTP_PANEL_SERVER_MAX_WS_CLIENTS; i++)
        {
            http_panel_server_ws_clients[i].fd = -1;
        }

        xSemaphoreGive(http_panel_server_ws_mutex);

        httpd_stop(server);

        ESP_LOGI(http_panel_server_tag, "Server stopped");
    }
}

/* work_queue task. Stopping waits for the camera and OTA pages to drain their
 * sockets, which they shut down first, so it normally takes milliseconds. */
static void http_panel_server_check(void* context)
{
    ARG_UNUSED(context);

    const bool wifi_connected =
        (wifi_connection_get_state() == WIFI_CONNECTION_STATE_CONNECTED);

    if (wifi_connected && !http_panel_server_running)
    {
        http_panel_server_running = http_panel_server_start();
    }
    else if (!wifi_connected && http_panel_server_running)
    {
        http_panel_server_stop();
        http_panel_server_running = false;
    }
    else
    {
        /* Server already matches the WiFi state. */
    }

    work_queue_schedule(
        &http_panel_server_work,
        (wifi_connected == http_panel_server_running) ? HTTP_PANEL_SERVER_CHECK_PERIOD_MS :
                                                        HTTP_PANEL_SERVER_RETRY_PERIOD_MS);
}

/* Default event loop task: only queues the check. */
static void http_panel_server_on_wifi_change(
    void* arg,
    esp_event_base_t base,
    int32_t event_id,
    void* event_data)
{
    ARG_UNUSED(arg);
    ARG_UNUSED(base);
    ARG_UNUSED(event_id);
    ARG_UNUSED(event_data);

    work_queue_push(&http_panel_server_work);
}


static esp_err_t http_panel_server_push_fd_type(
    int fd,
    const char* ws_uri,
    const void* data,
    size_t length,
    httpd_ws_type_t type,
    http_panel_push_slot_t* slot)
{
    esp_err_t error_code = ESP_OK;
    bool locked          = false;

    do
    {
        if ((ws_uri == NULL) || (data == NULL) || (length == 0u))
        {
            error_code = ESP_ERR_INVALID_ARG;
            break;
        }

        if (xSemaphoreTake(
                http_panel_server_ws_mutex, pdMS_TO_TICKS(HTTP_PANEL_SERVER_MUTEX_WAIT_MS))
            != pdTRUE)
        {
            error_code = ESP_ERR_TIMEOUT;
            break;
        }

        locked = true;

        if ((http_panel_server_httpd == NULL)
            || !http_panel_server_ws_registered_locked(fd, ws_uri))
        {
            error_code = ESP_ERR_INVALID_STATE;
            break;
        }

        error_code = http_panel_server_queue_push(
            http_panel_server_httpd, fd, ws_uri, data, length, type, slot);
    } while (0);

    if (locked)
    {
        xSemaphoreGive(http_panel_server_ws_mutex);
    }

    return error_code;
}


/* Exposed API ------------------------------------------------------------- */

esp_err_t http_panel_server_push_fd(
    int fd,
    const char* ws_uri,
    const char* data,
    size_t length,
    http_panel_push_slot_t* slot)
{
    return http_panel_server_push_fd_type(
        fd, ws_uri, data, length, HTTPD_WS_TYPE_TEXT, slot);
}

esp_err_t http_panel_server_push_binary_fd(
    int fd,
    const char* ws_uri,
    const uint8_t* data,
    size_t length,
    http_panel_push_slot_t* slot)
{
    return http_panel_server_push_fd_type(
        fd, ws_uri, data, length, HTTPD_WS_TYPE_BINARY, slot);
}

esp_err_t http_panel_server_init(void)
{
    esp_err_t error_code = ESP_OK;

    http_panel_server_ws_mutex = xSemaphoreCreateMutex();

    if (http_panel_server_ws_mutex == NULL)
    {
        error_code = ESP_ERR_NO_MEM;
    }
    else
    {
        for (uint8_t i = 0u; i < HTTP_PANEL_SERVER_MAX_WS_CLIENTS; i++)
        {
            http_panel_server_ws_clients[i].fd = -1;
        }

        error_code =
            work_queue_item_init(&http_panel_server_work, http_panel_server_check, NULL);
    }

    return error_code;
}

esp_err_t http_panel_server_enable(void)
{
    const esp_err_t error_code = work_queue_push(&http_panel_server_work);

    const esp_err_t register_result = esp_event_handler_instance_register(
        WIFI_CONNECTION_EVENTS,
        WIFI_CONNECTION_NOTIFY_STATE_CHANGED,
        http_panel_server_on_wifi_change,
        NULL,
        NULL);

    if (register_result != ESP_OK)
    {
        ESP_LOGW(
            http_panel_server_tag,
            "No WiFi change events (%s): server follows WiFi within %u ms",
            esp_err_to_name(register_result),
            (unsigned)HTTP_PANEL_SERVER_CHECK_PERIOD_MS);
    }

    return error_code;
}

esp_err_t http_panel_server_register_uri(const httpd_uri_t* uri)
{
    esp_err_t error_code = ESP_OK;

    if (http_panel_server_uri_count >= HTTP_PANEL_SERVER_MAX_URIS)
    {
        error_code = ESP_ERR_NO_MEM;
    }
    else
    {
        http_panel_server_uris[http_panel_server_uri_count] = uri;
        http_panel_server_uri_count++;
    }

    return error_code;
}

esp_err_t http_panel_server_register_lifecycle_cb(http_panel_server_lifecycle_cb_t callback)
{
    esp_err_t error_code = ESP_OK;

    if (callback == NULL)
    {
        error_code = ESP_ERR_INVALID_ARG;
    }
    else if (http_panel_server_lifecycle_callback_count >= HTTP_PANEL_SERVER_MAX_LIFECYCLE_CBS)
    {
        error_code = ESP_ERR_NO_MEM;
    }
    else
    {
        http_panel_server_lifecycle_callbacks[http_panel_server_lifecycle_callback_count] =
            callback;
        http_panel_server_lifecycle_callback_count++;
    }

    return error_code;
}

esp_err_t http_panel_server_register_ws_close_cb(http_panel_server_ws_close_cb_t callback)
{
    esp_err_t error_code = ESP_OK;

    if (callback == NULL)
    {
        error_code = ESP_ERR_INVALID_ARG;
    }
    else if (http_panel_server_ws_close_callback_count >= HTTP_PANEL_SERVER_MAX_WS_CLOSE_CBS)
    {
        error_code = ESP_ERR_NO_MEM;
    }
    else
    {
        http_panel_server_ws_close_callbacks[http_panel_server_ws_close_callback_count] =
            callback;
        http_panel_server_ws_close_callback_count++;
    }

    return error_code;
}

esp_err_t http_panel_server_ws_add(int fd, const char* ws_uri)
{
    esp_err_t error_code = ESP_ERR_TIMEOUT;

    if (xSemaphoreTake(
            http_panel_server_ws_mutex, pdMS_TO_TICKS(HTTP_PANEL_SERVER_MUTEX_WAIT_MS))
        == pdTRUE)
    {
        bool added        = false;
        int8_t free_index = -1;
        int8_t fd_index   = -1;

        for (uint8_t i = 0u; i < HTTP_PANEL_SERVER_MAX_WS_CLIENTS; i++)
        {
            if (http_panel_server_ws_clients[i].fd == fd)
            {
                fd_index = (int8_t)i;
            }
            else if ((http_panel_server_ws_clients[i].fd < 0) && (free_index < 0))
            {
                free_index = (int8_t)i;
            }
            else
            {
                /* Slot taken by another client. */
            }
        }

        /* Reusing the fd's entry keeps a reused fd from being listed twice. */
        const int8_t index = (fd_index >= 0) ? fd_index : free_index;

        if (index >= 0)
        {
            http_panel_server_ws_clients[index].fd = fd;
            strlcpy(
                http_panel_server_ws_clients[index].uri,
                ws_uri,
                sizeof(http_panel_server_ws_clients[index].uri));
            added = true;
        }

        xSemaphoreGive(http_panel_server_ws_mutex);

        if (added)
        {
            error_code = ESP_OK;
        }
        else
        {
            error_code = ESP_ERR_NO_MEM;
            ESP_LOGW(
                http_panel_server_tag,
                "WS client table full — fd=%d on %s gets no pushes",
                fd,
                ws_uri);
        }
    }

    return error_code;
}

/* No timeout: a removal that gave up would leave a dead fd in the table for
 * good, taking a slot and matching whichever connection reuses that fd. Every
 * holder of the mutex is short, so the wait is bounded. */
void http_panel_server_ws_remove(int fd)
{
    xSemaphoreTake(http_panel_server_ws_mutex, portMAX_DELAY);

    for (uint8_t i = 0u; i < HTTP_PANEL_SERVER_MAX_WS_CLIENTS; i++)
    {
        if (http_panel_server_ws_clients[i].fd == fd)
        {
            http_panel_server_ws_clients[i].fd = -1;
        }
    }

    xSemaphoreGive(http_panel_server_ws_mutex);
}

bool http_panel_server_push_ready(http_panel_push_slot_t* slot)
{
    bool ready = true;

    if (__atomic_load_n(&slot->in_flight, __ATOMIC_SEQ_CST) != 0u)
    {
        const int64_t age_us = esp_timer_get_time() - slot->since_us;

        if (age_us < ((int64_t)HTTP_PANEL_SERVER_PUSH_LOST_MS * HTTP_PANEL_SERVER_US_PER_MS))
        {
            ready = false;
        }
        else
        {
            ESP_LOGW(
                http_panel_server_tag,
                "WS push not sent after %lld ms — assuming dropped",
                (long long)(age_us / HTTP_PANEL_SERVER_US_PER_MS));
            __atomic_store_n(&slot->in_flight, 0u, __ATOMIC_SEQ_CST);
        }
    }

    return ready;
}

void http_panel_server_push(
    const char* ws_uri,
    const char* data,
    size_t length,
    http_panel_push_slot_t* slot)
{
    if ((data != NULL) && (length != 0u)
        && (xSemaphoreTake(
                http_panel_server_ws_mutex, pdMS_TO_TICKS(HTTP_PANEL_SERVER_MUTEX_WAIT_MS))
            == pdTRUE))
    {
        const httpd_handle_t server = http_panel_server_httpd;

        for (uint8_t i = 0u; (server != NULL) && (i < HTTP_PANEL_SERVER_MAX_WS_CLIENTS); i++)
        {
            if (http_panel_server_ws_matches(&http_panel_server_ws_clients[i], ws_uri))
            {
                /* Each client's item carries the slot: it reads as idle only
                 * once every frame has gone out. */
                const esp_err_t push_result = http_panel_server_queue_push(
                    server,
                    http_panel_server_ws_clients[i].fd,
                    ws_uri,
                    data,
                    length,
                    HTTPD_WS_TYPE_TEXT,
                    slot);

                /* A failed queue only loses this client's frame. */
                if (push_result == ESP_ERR_NO_MEM)
                {
                    break;
                }
            }
        }

        xSemaphoreGive(http_panel_server_ws_mutex);
    }
}


esp_err_t http_panel_server_close_client(int fd)
{
    esp_err_t error_code = ESP_ERR_TIMEOUT;

    if (xSemaphoreTake(
            http_panel_server_ws_mutex, pdMS_TO_TICKS(HTTP_PANEL_SERVER_MUTEX_WAIT_MS))
        == pdTRUE)
    {
        error_code = ESP_ERR_INVALID_STATE;

        if (http_panel_server_httpd != NULL)
        {
            error_code = httpd_sess_trigger_close(http_panel_server_httpd, fd);
        }

        xSemaphoreGive(http_panel_server_ws_mutex);
    }

    return error_code;
}

uint8_t http_panel_server_ws_count(const char* ws_uri)
{
    uint8_t count = 0u;

    if (xSemaphoreTake(
            http_panel_server_ws_mutex, pdMS_TO_TICKS(HTTP_PANEL_SERVER_MUTEX_WAIT_MS))
        == pdTRUE)
    {
        for (uint8_t i = 0u; i < HTTP_PANEL_SERVER_MAX_WS_CLIENTS; i++)
        {
            if (http_panel_server_ws_matches(&http_panel_server_ws_clients[i], ws_uri))
            {
                count++;
            }
        }

        xSemaphoreGive(http_panel_server_ws_mutex);
    }

    return count;
}

httpd_handle_t http_panel_server_handle(void)
{
    httpd_handle_t server = NULL;

    if (xSemaphoreTake(
            http_panel_server_ws_mutex, pdMS_TO_TICKS(HTTP_PANEL_SERVER_MUTEX_WAIT_MS))
        == pdTRUE)
    {
        server = http_panel_server_httpd;
        xSemaphoreGive(http_panel_server_ws_mutex);
    }

    return server;
}

/**** END OF FILE ****/
