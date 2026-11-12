/**
 ******************************************************************************
 * @file    smart_io_ws.c
 * @author  Daniel Dyringer
 * @date    2026-10-31
 * @brief   WebSocket transport of smart_io (/io/ws).
 ******************************************************************************
 */


/* Includes ---------------------------------------------------------------- */
#include "smart_io_ws.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "esp_attr.h"
#include "esp_event.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "http_panel_server.h"
#include "lwip/sockets.h"
#include "smart_io.h"
#include "smart_io_json.h"
#include "smart_io_stream.h"
#include "utils.h"


/* Constants and macros ---------------------------------------------------- */
// clang-format off

#define SMART_IO_WS_URI                 "/io/ws"
#define SMART_IO_WS_REQUEST_BUFFERS       8u
/* Request buffers + wake messages. */
#define SMART_IO_WS_QUEUE_DEPTH         (SMART_IO_WS_REQUEST_BUFFERS + 2u)
#define SMART_IO_WS_UPDATE_MS           50u
/* A client that doesn't take a response in this time is closed; the page
 * reconnects and sends hello again. */
#define SMART_IO_WS_SEND_TIMEOUT_MS     1000u
#define SMART_IO_WS_POLL_MS          10u
#define SMART_IO_WS_BUSY_SIZE           160u
#define SMART_IO_WS_US_PER_MS           1000
/* At least the largest frame (the stream scratch size). */
#define SMART_IO_WS_FRAMES_SIZE         24576u
#define SMART_IO_WS_STREAM_WAIT_MS      10u

#define SMART_IO_WS_FLAG_UNSAVED        0x1u
#define SMART_IO_WS_FLAG_STREAMS        0x2u
#define SMART_IO_WS_FLAG_FLOOD          0x4u

#define SMART_IO_WS_TASK_STACK          6144u
#define SMART_IO_WS_TASK_PRIO           3u
#define SMART_IO_WS_TASK_CORE           1

// clang-format on

_Static_assert(SMART_IO_WS_REQUEST_BUFFERS <= 32u, "free buffers are a uint32_t mask");

/* Private types ----------------------------------------------------------- */

typedef enum smart_io_ws_message_type_e
{
    SMART_IO_WS_MESSAGE_TYPE_REQUEST = 0,
    SMART_IO_WS_MESSAGE_TYPE_WAKE    = 1,
} smart_io_ws_message_type_t;

typedef struct smart_io_ws_message_s
{
    int fd;
    uint16_t length;
    uint8_t buffer_index;
    uint8_t type; /* smart_io_ws_message_type_t */
} smart_io_ws_message_t;

typedef struct smart_io_ws_client_s
{
    http_panel_push_slot_t slot;
    /* Under smart_io_ws_stream_mutex: sio_ws attaches, sys_evt sends. */
    http_panel_push_slot_t binary_slot;
    smart_io_stream_cursor_t cursor;
    bool attached;
    bool attach_failed; /* sio_ws only; retried by the next connection */
    int64_t last_update_us;
    uint32_t value_dirty;
    uint32_t config_dirty;
    uint32_t changed_flags;
    uint32_t flood_ms;
    /* Set by httpd under smart_io_ws_lock; everything above is sio_ws only. */
    int fd;
    bool just_connected;
} smart_io_ws_client_t;


/* Private variables ------------------------------------------------------- */

static const char* smart_io_ws_tag = "smart_io_ws";

static QueueHandle_t smart_io_ws_queue = NULL;

/* Guards the free buffer mask and the clients' fd / just_connected. */
static portMUX_TYPE smart_io_ws_lock = portMUX_INITIALIZER_UNLOCKED;

static smart_io_ws_client_t smart_io_ws_clients[HTTP_PANEL_SERVER_MAX_WS_CLIENTS];
static uint32_t smart_io_ws_free_buffers = 0u;

/* PSRAM: filled by httpd, read by sio_ws while the buffer is taken. */
EXT_RAM_BSS_ATTR static char smart_io_ws_request_buffers[SMART_IO_WS_REQUEST_BUFFERS]
                                                        [SMART_IO_JSON_REQUEST_MAX];
/* Requests refused as busy land here. httpd task only. */
EXT_RAM_BSS_ATTR static char smart_io_ws_refused_request[SMART_IO_JSON_REQUEST_MAX];
/* sio_ws only. */
EXT_RAM_BSS_ATTR static char smart_io_ws_response[SMART_IO_JSON_RESPONSE_SIZE];
/* sys_evt only. */
EXT_RAM_BSS_ATTR static uint8_t smart_io_ws_frames[SMART_IO_WS_FRAMES_SIZE];

/* Held for µs: sio_ws flips attached, sys_evt reads a cursor and pushes. */
static SemaphoreHandle_t smart_io_ws_stream_mutex = NULL;

/* Set from sys_evt, taken by sio_ws: atomic operations only. Flood pause in
 * ms, 0 = none since the last take. */
static uint32_t smart_io_ws_pending_flood = 0u;
static bool smart_io_ws_wake_pending      = false;


/* Private functions ------------------------------------------------------- */

/* ---- Request buffers ---- */

/* SMART_IO_WS_REQUEST_BUFFERS when none is free. */
static uint8_t smart_io_ws_buffer_take(void)
{
    uint8_t index = (uint8_t)SMART_IO_WS_REQUEST_BUFFERS;

    taskENTER_CRITICAL(&smart_io_ws_lock);

    if (smart_io_ws_free_buffers != 0u)
    {
        index = (uint8_t)__builtin_ctz(smart_io_ws_free_buffers);
        smart_io_ws_free_buffers &= ~((uint32_t)1u << index);
    }

    taskEXIT_CRITICAL(&smart_io_ws_lock);

    return index;
}

static void smart_io_ws_buffer_give(uint8_t index)
{
    taskENTER_CRITICAL(&smart_io_ws_lock);
    smart_io_ws_free_buffers |= (uint32_t)1u << index;
    taskEXIT_CRITICAL(&smart_io_ws_lock);
}

/* ---- Clients ---- */

static smart_io_ws_client_t* smart_io_ws_client_find(int fd)
{
    smart_io_ws_client_t* client = NULL;

    taskENTER_CRITICAL(&smart_io_ws_lock);

    for (size_t i = 0u; (i < ARRAY_SIZE(smart_io_ws_clients)) && (client == NULL); i++)
    {
        if (smart_io_ws_clients[i].fd == fd)
        {
            client = &smart_io_ws_clients[i];
        }
    }

    taskEXIT_CRITICAL(&smart_io_ws_lock);

    return client;
}

static bool smart_io_ws_client_has_changes(const smart_io_ws_client_t* client)
{
    return (client->value_dirty != 0u) || (client->config_dirty != 0u)
           || (client->changed_flags != 0u);
}

static void smart_io_ws_client_clear_changes(smart_io_ws_client_t* client)
{
    client->value_dirty   = 0u;
    client->config_dirty  = 0u;
    client->changed_flags = 0u;
    client->flood_ms      = 0u;
}

/* ---- Events (sys_evt) ---- */

/* One binary push per client with every new frame since its cursor. A client
 * whose previous push is still out is skipped; the next notification
 * catches up. */
static void smart_io_ws_send_frames(void)
{
    for (size_t i = 0u; i < ARRAY_SIZE(smart_io_ws_clients); i++)
    {
        smart_io_ws_client_t* const client = &smart_io_ws_clients[i];

        if (xSemaphoreTake(
                smart_io_ws_stream_mutex, pdMS_TO_TICKS(SMART_IO_WS_STREAM_WAIT_MS))
            != pdTRUE)
        {
            break;
        }

        taskENTER_CRITICAL(&smart_io_ws_lock);
        const int fd = client->fd;
        taskEXIT_CRITICAL(&smart_io_ws_lock);

        if (client->attached && (fd >= 0)
            && http_panel_server_push_ready(&client->binary_slot))
        {
            const smart_io_stream_cursor_t cursor = client->cursor;
            size_t length                         = 0u;

            smart_io_stream_read(
                &client->cursor, smart_io_ws_frames, sizeof(smart_io_ws_frames), &length);

            /* Not queued: the same frames go out with the next notification. */
            if ((length > 0u)
                && (http_panel_server_push_binary_fd(
                        fd, SMART_IO_WS_URI, smart_io_ws_frames, length, &client->binary_slot)
                    != ESP_OK))
            {
                client->cursor = cursor;
            }
        }

        xSemaphoreGive(smart_io_ws_stream_mutex);
    }
}

static void smart_io_ws_wake(void)
{
    if (!__atomic_exchange_n(&smart_io_ws_wake_pending, true, __ATOMIC_SEQ_CST))
    {
        const smart_io_ws_message_t message = {
            .fd           = -1,
            .length       = 0u,
            .buffer_index = 0u,
            .type         = (uint8_t)SMART_IO_WS_MESSAGE_TYPE_WAKE,
        };

        /* A full queue keeps sio_ws busy; it collects changes after every message. */
        xQueueSend(smart_io_ws_queue, &message, 0u);
    }
}

static void smart_io_ws_on_event(
    void* handler_arg,
    esp_event_base_t base,
    int32_t event_id,
    void* event_data)
{
    ARG_UNUSED(handler_arg);
    ARG_UNUSED(base);

    bool changed = true;

    switch ((smart_io_event_t)event_id)
    {
        case SMART_IO_EVENT_STREAM_FRAME:
        {
            smart_io_ws_send_frames();
            changed = false;
        }
        break;
        case SMART_IO_EVENT_CHANGED:
        {
            /* sio_ws takes the changes from smart_io. */
        }
        break;
        case SMART_IO_EVENT_STREAM_FLOOD:
        {
            __atomic_store_n(
                &smart_io_ws_pending_flood, *(const uint32_t*)event_data, __ATOMIC_SEQ_CST);
        }
        break;
        default:
        {
            changed = false;
        }
        break;
    }

    if (changed)
    {
        smart_io_ws_wake();
    }
}

/* ---- sio_ws task ---- */

/* Just connected clients start clean: they ask for hello, which carries everything. */
static void smart_io_ws_collect_changes(void)
{
    smart_io_changes_t changes = {0};

    __atomic_store_n(&smart_io_ws_wake_pending, false, __ATOMIC_SEQ_CST);
    smart_io_changes_take(&changes);

    const uint32_t flood =
        __atomic_exchange_n(&smart_io_ws_pending_flood, 0u, __ATOMIC_SEQ_CST);
    uint32_t flags = 0u;

    if (changes.streams)
    {
        flags |= SMART_IO_WS_FLAG_STREAMS;
    }

    if (changes.unsaved)
    {
        flags |= SMART_IO_WS_FLAG_UNSAVED;
    }

    if (flood != 0u)
    {
        flags |= SMART_IO_WS_FLAG_FLOOD;
    }

    for (size_t i = 0u; i < ARRAY_SIZE(smart_io_ws_clients); i++)
    {
        smart_io_ws_client_t* const client = &smart_io_ws_clients[i];

        taskENTER_CRITICAL(&smart_io_ws_lock);
        const int fd              = client->fd;
        const bool just_connected = client->just_connected;
        client->just_connected    = false;
        taskEXIT_CRITICAL(&smart_io_ws_lock);

        if (just_connected || (fd < 0))
        {
            smart_io_ws_client_clear_changes(client);
        }

        if (fd >= 0)
        {
            client->value_dirty |= changes.values;
            client->config_dirty |= changes.configs;
            client->changed_flags |= flags;

            if ((flags & SMART_IO_WS_FLAG_FLOOD) != 0u)
            {
                client->flood_ms = flood;
            }
        }
    }
}

/* Every connected client holds one stream attachment, released when it is
 * gone. Comparing state instead of queueing attach/detach messages means a
 * full queue can't leak an attachment. */
static void smart_io_ws_reconcile_clients(void)
{
    for (size_t i = 0u; i < ARRAY_SIZE(smart_io_ws_clients); i++)
    {
        smart_io_ws_client_t* const client = &smart_io_ws_clients[i];

        taskENTER_CRITICAL(&smart_io_ws_lock);
        const int fd = client->fd;
        taskEXIT_CRITICAL(&smart_io_ws_lock);

        if ((fd >= 0) && !client->attached && !client->attach_failed)
        {
            smart_io_stream_cursor_t cursor = {0};
            const esp_err_t error_code      = smart_io_client_attach(&cursor);

            if (error_code == ESP_OK)
            {
                xSemaphoreTake(smart_io_ws_stream_mutex, portMAX_DELAY);
                client->cursor   = cursor;
                client->attached = true;
                xSemaphoreGive(smart_io_ws_stream_mutex);
            }
            else
            {
                client->attach_failed = true;
                ESP_LOGW(
                    smart_io_ws_tag,
                    "fd=%d gets no streams: %s",
                    fd,
                    esp_err_to_name(error_code));
            }
        }
        else if ((fd < 0) && client->attached)
        {
            xSemaphoreTake(smart_io_ws_stream_mutex, portMAX_DELAY);
            client->attached = false;
            xSemaphoreGive(smart_io_ws_stream_mutex);
            smart_io_client_detach();
        }
        else if (fd < 0)
        {
            client->attach_failed = false;
        }
        else
        {
            /* Attached and connected, or refused until it reconnects. */
        }
    }
}

/* Waits for the client's text slot and retries a push that wasn't queued, so
 * responses are never dropped while the client is there. */
static void smart_io_ws_send_response(int fd, size_t length)
{
    smart_io_ws_client_t* const client = smart_io_ws_client_find(fd);
    const int64_t start_us             = esp_timer_get_time();
    const int64_t timeout_us = (int64_t)SMART_IO_WS_SEND_TIMEOUT_MS * SMART_IO_WS_US_PER_MS;
    bool done = false;

    while ((client != NULL) && !done)
    {
        esp_err_t error_code = ESP_ERR_NOT_FINISHED;

        if (http_panel_server_push_ready(&client->slot))
        {
            error_code = http_panel_server_push_fd(
                fd, SMART_IO_WS_URI, smart_io_ws_response, length, &client->slot);
        }

        /* Queued, or the client is gone. */
        if ((error_code == ESP_OK) || (error_code == ESP_ERR_INVALID_STATE))
        {
            done = true;
        }
        else if ((esp_timer_get_time() - start_us) >= timeout_us)
        {
            ESP_LOGW(
                smart_io_ws_tag,
                "fd=%d: response not sent in %u ms, closing",
                fd,
                (unsigned)SMART_IO_WS_SEND_TIMEOUT_MS);
            http_panel_server_close_client(fd);
            break;
        }
        else
        {
            ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(SMART_IO_WS_POLL_MS));
        }
    }
}

static void smart_io_ws_handle_request(const smart_io_ws_message_t* message)
{
    size_t length              = 0u;
    const esp_err_t error_code = smart_io_json_handle(
        smart_io_ws_request_buffers[message->buffer_index],
        message->length,
        smart_io_ws_response,
        sizeof(smart_io_ws_response),
        &length);

    smart_io_ws_buffer_give(message->buffer_index);

    if (error_code == ESP_OK)
    {
        smart_io_ws_send_response(message->fd, length);
    }
    else
    {
        ESP_LOGE(smart_io_ws_tag, "No response: %s", esp_err_to_name(error_code));
    }
}

/* True while a client still has changes to send. */
static bool smart_io_ws_send_updates(void)
{
    bool updates_pending = false;
    const int64_t now_us = esp_timer_get_time();

    for (size_t i = 0u; i < ARRAY_SIZE(smart_io_ws_clients); i++)
    {
        smart_io_ws_client_t* const client = &smart_io_ws_clients[i];

        taskENTER_CRITICAL(&smart_io_ws_lock);
        const int fd = client->fd;
        taskEXIT_CRITICAL(&smart_io_ws_lock);

        const bool due = (now_us - client->last_update_us)
                         >= ((int64_t)SMART_IO_WS_UPDATE_MS * SMART_IO_WS_US_PER_MS);

        if ((fd < 0) || !smart_io_ws_client_has_changes(client))
        {
            /* Nothing to send. */
        }
        else if (!due || !http_panel_server_push_ready(&client->slot))
        {
            updates_pending = true;
        }
        else
        {
            const smart_io_json_update_t update = {
                .value_mask  = client->value_dirty,
                .config_mask = client->config_dirty,
                .flood_ms    = client->flood_ms,
                .streams     = (client->changed_flags & SMART_IO_WS_FLAG_STREAMS) != 0u,
                .unsaved     = (client->changed_flags & SMART_IO_WS_FLAG_UNSAVED) != 0u,
            };
            size_t length              = 0u;
            const esp_err_t build_code = smart_io_json_update(
                &update, smart_io_ws_response, sizeof(smart_io_ws_response), &length);
            esp_err_t push_code = ESP_OK;

            if (build_code == ESP_OK)
            {
                push_code = http_panel_server_push_fd(
                    fd, SMART_IO_WS_URI, smart_io_ws_response, length, &client->slot);
            }
            else
            {
                ESP_LOGE(
                    smart_io_ws_tag, "Update not built: %s", esp_err_to_name(build_code));
            }

            /* Not queued (httpd work queue full, no memory, mutex busy): the
             * changes stay and go out with the next try. */
            if ((push_code != ESP_OK) && (push_code != ESP_ERR_INVALID_STATE))
            {
                updates_pending = true;
            }
            else
            {
                smart_io_ws_client_clear_changes(client);
                client->last_update_us = now_us;
            }
        }
    }

    return updates_pending;
}

static void smart_io_ws_task(void* arg)
{
    ARG_UNUSED(arg);

    bool updates_pending = false;

    while (true)
    {
        smart_io_ws_message_t message = {0};
        const TickType_t wait =
            updates_pending ? pdMS_TO_TICKS(SMART_IO_WS_POLL_MS) : portMAX_DELAY;
        const bool received = (xQueueReceive(smart_io_ws_queue, &message, wait) == pdTRUE);

        /* Before the request: a hello must not have its later changes cleared
         * by the reset of a just connected client. */
        smart_io_ws_collect_changes();
        smart_io_ws_reconcile_clients();

        if (received && (message.type == (uint8_t)SMART_IO_WS_MESSAGE_TYPE_REQUEST))
        {
            smart_io_ws_handle_request(&message);
        }

        updates_pending = smart_io_ws_send_updates();
    }
}

/* ---- httpd handlers ---- */

/* IDF 6.0 no longer invokes the URI handler on the handshake GET (commit
 * c9daea6, issue #18539), so the fd is registered here. */
static esp_err_t smart_io_ws_on_connect(httpd_req_t* request)
{
    const int fd         = httpd_req_to_sockfd(request);
    esp_err_t error_code = http_panel_server_ws_add(fd, SMART_IO_WS_URI);

    if (error_code == ESP_OK)
    {
        bool added = false;

        taskENTER_CRITICAL(&smart_io_ws_lock);

        for (size_t i = 0u; (i < ARRAY_SIZE(smart_io_ws_clients)) && !added; i++)
        {
            if (smart_io_ws_clients[i].fd < 0)
            {
                smart_io_ws_clients[i].fd             = fd;
                smart_io_ws_clients[i].just_connected = true;
                added                                 = true;
            }
        }

        taskEXIT_CRITICAL(&smart_io_ws_lock);

        if (!added)
        {
            http_panel_server_ws_remove(fd);
            error_code = ESP_ERR_NO_MEM;
        }
    }

    if (error_code == ESP_OK)
    {
        /* Small frames go out at once instead of waiting for Nagle. */
        const int no_delay = 1;

        setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &no_delay, sizeof(no_delay));
        smart_io_ws_wake();
    }

    ESP_LOGI(smart_io_ws_tag, "Client fd=%d: %s", fd, esp_err_to_name(error_code));

    return error_code;
}

/* Every closed socket of the server lands here. */
static void smart_io_ws_on_close(int fd)
{
    bool found = false;

    taskENTER_CRITICAL(&smart_io_ws_lock);

    for (size_t i = 0u; i < ARRAY_SIZE(smart_io_ws_clients); i++)
    {
        if (smart_io_ws_clients[i].fd == fd)
        {
            smart_io_ws_clients[i].fd = -1;
            found                     = true;
        }
    }

    taskEXIT_CRITICAL(&smart_io_ws_lock);

    if (found)
    {
        smart_io_ws_wake();
    }
}

static void smart_io_ws_reply_busy(int fd, const char* request, size_t request_length)
{
    char response[SMART_IO_WS_BUSY_SIZE] = {0};
    size_t length                        = 0u;

    if (smart_io_json_error(
            request,
            request_length,
            ESP_ERR_NO_MEM,
            "Busy, try again",
            response,
            sizeof(response),
            &length)
        == ESP_OK)
    {
        http_panel_server_push_fd(fd, SMART_IO_WS_URI, response, length, NULL);
    }
}

/* IDF 6.0: only invoked for inbound data frames. Never waits: httpd serves
 * every page. */
static esp_err_t smart_io_ws_on_frame(httpd_req_t* request)
{
    const int fd           = httpd_req_to_sockfd(request);
    httpd_ws_frame_t frame = {0};
    esp_err_t error_code   = httpd_ws_recv_frame(request, &frame, 0u);

    do
    {
        if ((error_code != ESP_OK) || (frame.len == 0u))
        {
            break;
        }

        /* Can't be skipped without reading it: the connection is closed. */
        if (frame.len > SMART_IO_JSON_REQUEST_MAX)
        {
            ESP_LOGW(
                smart_io_ws_tag, "fd=%d: %u B request refused", fd, (unsigned)frame.len);
            error_code = ESP_ERR_INVALID_SIZE;
            break;
        }

        const uint8_t index = smart_io_ws_buffer_take();
        const bool queued   = (index < SMART_IO_WS_REQUEST_BUFFERS);
        char* const buffer =
            queued ? smart_io_ws_request_buffers[index] : smart_io_ws_refused_request;

        frame.payload = (uint8_t*)buffer;
        error_code    = httpd_ws_recv_frame(request, &frame, frame.len);

        bool sent = false;

        if (queued && (error_code == ESP_OK))
        {
            const smart_io_ws_message_t message = {
                .fd           = fd,
                .length       = (uint16_t)frame.len,
                .buffer_index = index,
                .type         = (uint8_t)SMART_IO_WS_MESSAGE_TYPE_REQUEST,
            };

            sent = (xQueueSend(smart_io_ws_queue, &message, 0u) == pdTRUE);
        }

        if (queued && !sent)
        {
            smart_io_ws_buffer_give(index);
        }

        if ((error_code == ESP_OK) && !sent)
        {
            smart_io_ws_reply_busy(fd, buffer, frame.len);
        }
    } while (0);

    return error_code;
}


/* Exposed API ------------------------------------------------------------- */

esp_err_t smart_io_ws_init(void)
{
    esp_err_t error_code = ESP_OK;
    TaskHandle_t task    = NULL;

    /* Static: http_panel_server keeps the URI by pointer. */
    static const httpd_uri_t smart_io_ws_uri = {
        .uri                      = SMART_IO_WS_URI,
        .method                   = HTTP_GET,
        .handler                  = smart_io_ws_on_frame,
        .user_ctx                 = NULL,
        .is_websocket             = true,
        .handle_ws_control_frames = false,
        .supported_subprotocol    = NULL,
        .ws_post_handshake_cb     = smart_io_ws_on_connect,
    };

    do
    {
        smart_io_ws_free_buffers = ((uint32_t)1u << SMART_IO_WS_REQUEST_BUFFERS) - 1u;

        smart_io_ws_stream_mutex = xSemaphoreCreateMutex();

        if (smart_io_ws_stream_mutex == NULL)
        {
            error_code = ESP_ERR_NO_MEM;
            break;
        }

        smart_io_ws_queue =
            xQueueCreate(SMART_IO_WS_QUEUE_DEPTH, sizeof(smart_io_ws_message_t));

        if (smart_io_ws_queue == NULL)
        {
            error_code = ESP_ERR_NO_MEM;
            break;
        }

        const BaseType_t task_created = xTaskCreatePinnedToCore(
            smart_io_ws_task,
            "sio_ws",
            SMART_IO_WS_TASK_STACK,
            NULL,
            SMART_IO_WS_TASK_PRIO,
            &task,
            SMART_IO_WS_TASK_CORE);

        if (task_created != pdPASS)
        {
            error_code = ESP_ERR_NO_MEM;
            break;
        }

        for (size_t i = 0u; i < ARRAY_SIZE(smart_io_ws_clients); i++)
        {
            smart_io_ws_clients[i].fd          = -1;
            smart_io_ws_clients[i].slot.waiter = task;
        }

        error_code = esp_event_handler_register(
            SMART_IO_EVENTS, ESP_EVENT_ANY_ID, smart_io_ws_on_event, NULL);

        if (error_code != ESP_OK)
        {
            break;
        }

        error_code = http_panel_server_register_ws_close_cb(smart_io_ws_on_close);

        if (error_code != ESP_OK)
        {
            break;
        }

        error_code = http_panel_server_register_uri(&smart_io_ws_uri);
    } while (0);

    if (error_code != ESP_OK)
    {
        ESP_LOGE(smart_io_ws_tag, "Init failed: %s", esp_err_to_name(error_code));
    }

    return error_code;
}

/**** END OF FILE ****/
