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

#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "wifi_connection.h"


/* Constants and macros ---------------------------------------------------- */
// clang-format off

#define HTTP_PANEL_SERVER_MONITOR_STACK     3072u
#define HTTP_PANEL_SERVER_MONITOR_PRIO      3u
#define HTTP_PANEL_SERVER_MONITOR_PERIOD_MS 1000u

/** @brief Core for the panel monitor task — pinned to the network core. */
#define HTTP_PANEL_SERVER_MONITOR_CORE      0

/** @brief Maximum URI handlers that can be registered before server starts. */
#define HTTP_PANEL_SERVER_MAX_URIS          16u

/** @brief Maximum lifecycle callbacks (pages owning async requests). */
#define HTTP_PANEL_SERVER_MAX_LIFECYCLE_CBS 4u

// clang-format on


/* Private types ----------------------------------------------------------- */

/** @brief One tracked WebSocket client. */
typedef struct
{
    int  fd;                                    /**< Socket fd, -1 = free    */
    char uri[32];                               /**< URI this client is on   */
} http_panel_ws_client_t;

/** @brief Work argument for a single async WS push. */
typedef struct
{
    httpd_handle_t  hd;
    int             fd;
    char           *payload;        /**< heap-alloc, freed here              */
    size_t          len;
    volatile bool  *pending;        /**< caller flag, cleared on completion  */
} http_panel_push_arg_t;


/* Private variables ------------------------------------------------------- */

static const char *http_panel_server_tag = "http_panel";

static httpd_handle_t           http_panel_server_hd   = NULL;
static http_panel_ws_client_t   http_panel_ws_clients[HTTP_PANEL_SERVER_MAX_WS_CLIENTS];
static SemaphoreHandle_t        http_panel_ws_mutex     = NULL;

/* URI handler table — populated before server start */
static const httpd_uri_t       *http_panel_uris[HTTP_PANEL_SERVER_MAX_URIS];
static uint8_t                  http_panel_uri_count = 0u;

/* Lifecycle callbacks — populated at init, invoked by the monitor task */
static http_panel_server_lifecycle_cb_t http_panel_lifecycle_cbs[HTTP_PANEL_SERVER_MAX_LIFECYCLE_CBS];
static uint8_t                          http_panel_lifecycle_cb_count = 0u;


/* Private functions ------------------------------------------------------- */

static void http_panel_server_close_fn(httpd_handle_t hd, int sockfd)
{
    (void)hd;
    ESP_LOGI(http_panel_server_tag, "Socket closed fd=%d", sockfd);
    http_panel_server_ws_remove(sockfd);
    close(sockfd);
}

static void http_panel_server_notify(bool running)
{
    for (uint8_t i = 0u; i < http_panel_lifecycle_cb_count; i++)
    {
        http_panel_lifecycle_cbs[i](running);
    }
}

static void http_panel_push_work(void *arg)
{
    http_panel_push_arg_t *pa = (http_panel_push_arg_t *)arg;

    httpd_ws_frame_t frm = {
        .type    = HTTPD_WS_TYPE_TEXT,
        .payload = (uint8_t *)pa->payload,
        .len     = pa->len,
    };

    const esp_err_t send_ret = httpd_ws_send_frame_async(pa->hd, pa->fd, &frm);

    if (send_ret != ESP_OK)
    {
        ESP_LOGW(http_panel_server_tag,
                 "ws send failed fd=%d: %s — dropping client",
                 pa->fd, esp_err_to_name(send_ret));
        http_panel_server_ws_remove(pa->fd);
    }

    /* Clear the caller's pending flag — must happen after send so the
     * next push carries fresh data, not a duplicate of this one. */
    if (pa->pending)
    {
        *pa->pending = false;
    }

    free(pa->payload);
    free(pa);
}

static void http_panel_server_start(void)
{
    do
    {
        if (http_panel_server_hd)
        {
            break;
        }

        httpd_config_t cfg   = HTTPD_DEFAULT_CONFIG();
        cfg.server_port      = HTTP_PANEL_SERVER_PORT;
        cfg.stack_size       = HTTP_PANEL_SERVER_TASK_STACK;
        /* LRU purge disabled: with it on, an idle WebSocket socket can be evicted
         * the moment a new connection arrives — before the upgrade handshake is
         * dispatched to the handler — which drops the WS connection. With it off a
         * full table cleanly refuses new connections instead of killing live ones.
         */
        cfg.lru_purge_enable = false;
        /* httpd reserves 3 sockets internally; size the rest for page loads (kept
         * alive) plus WS clients across all panel pages. Bounded by
         * CONFIG_LWIP_MAX_SOCKETS (16). */
        cfg.max_open_sockets = HTTP_PANEL_SERVER_MAX_OPEN_SOCKETS;
        cfg.max_uri_handlers = HTTP_PANEL_SERVER_MAX_URIS;
        cfg.close_fn         = http_panel_server_close_fn;

        if (httpd_start(&http_panel_server_hd, &cfg) != ESP_OK)
        {
            ESP_LOGE(http_panel_server_tag, "httpd_start failed");
            break;
        }

        for (uint8_t i = 0u; i < http_panel_uri_count; i++)
        {
            const esp_err_t error_code = httpd_register_uri_handler(
                http_panel_server_hd, http_panel_uris[i]);

            ESP_LOGI(http_panel_server_tag, "register URI '%s' (%s) -> %s",
                     http_panel_uris[i]->uri,
                     http_panel_uris[i]->is_websocket ? "ws" : "http",
                     esp_err_to_name(error_code));
        }

        ESP_LOGI(http_panel_server_tag, "Server up on port %u, %u handler(s)",
                 HTTP_PANEL_SERVER_PORT, (unsigned)http_panel_uri_count);

        http_panel_server_notify(true);
    } while (0);
}

static void http_panel_server_stop(void)
{
    if (!http_panel_server_hd)
    {
        return;
    }

    /* Let pages finish their async requests while the handle is valid. */
    http_panel_server_notify(false);

    httpd_stop(http_panel_server_hd);
    http_panel_server_hd = NULL;

    if (xSemaphoreTake(http_panel_ws_mutex, pdMS_TO_TICKS(200u)) == pdTRUE)
    {
        for (uint8_t i = 0u; i < HTTP_PANEL_SERVER_MAX_WS_CLIENTS; i++)
        {
            http_panel_ws_clients[i].fd = -1;
        }

        xSemaphoreGive(http_panel_ws_mutex);
    }

    ESP_LOGI(http_panel_server_tag, "Server stopped");
}

static void http_panel_server_monitor_task(void *pv)
{
    bool up = false;

    while (true)
    {
        vTaskDelay(pdMS_TO_TICKS(HTTP_PANEL_SERVER_MONITOR_PERIOD_MS));

        const bool want = (wifi_connection_get_state() == WIFI_CONN_STATE_CONNECTED);

        if (want && !up)
        {
            http_panel_server_start();
            up = true;
        }
        else if (!want && up)
        {
            http_panel_server_stop();
            up = false;
        }
    }
}


/* Exposed API ------------------------------------------------------------- */

esp_err_t http_panel_server_init(void)
{
    esp_err_t error_code;

    http_panel_ws_mutex = xSemaphoreCreateMutex();

    if (!http_panel_ws_mutex)
    {
        error_code = ESP_ERR_NO_MEM;
    }
    else
    {
        for (uint8_t i = 0u; i < HTTP_PANEL_SERVER_MAX_WS_CLIENTS; i++)
        {
            http_panel_ws_clients[i].fd = -1;
        }

        const BaseType_t task_created = xTaskCreatePinnedToCore(
            http_panel_server_monitor_task,
            "panel_mon",
            HTTP_PANEL_SERVER_MONITOR_STACK,
            NULL,
            HTTP_PANEL_SERVER_MONITOR_PRIO,
            NULL,
            HTTP_PANEL_SERVER_MONITOR_CORE);

        error_code = (task_created == pdPASS) ? ESP_OK : ESP_FAIL;
    }

    return error_code;
}

esp_err_t http_panel_server_register_uri(const httpd_uri_t *uri)
{
    esp_err_t error_code;

    if (http_panel_uri_count >= HTTP_PANEL_SERVER_MAX_URIS)
    {
        error_code = ESP_ERR_NO_MEM;
    }
    else
    {
        http_panel_uris[http_panel_uri_count++] = uri;
        error_code = ESP_OK;
    }

    return error_code;
}

esp_err_t http_panel_server_register_lifecycle_cb(http_panel_server_lifecycle_cb_t cb)
{
    esp_err_t error_code;

    if (!cb)
    {
        error_code = ESP_ERR_INVALID_ARG;
    }
    else if (http_panel_lifecycle_cb_count >= HTTP_PANEL_SERVER_MAX_LIFECYCLE_CBS)
    {
        error_code = ESP_ERR_NO_MEM;
    }
    else
    {
        http_panel_lifecycle_cbs[http_panel_lifecycle_cb_count++] = cb;
        error_code = ESP_OK;
    }

    return error_code;
}

void http_panel_server_ws_add(int fd, const char *ws_uri)
{
    if (xSemaphoreTake(http_panel_ws_mutex, pdMS_TO_TICKS(200u)) != pdTRUE)
    {
        return;
    }

    for (uint8_t i = 0u; i < HTTP_PANEL_SERVER_MAX_WS_CLIENTS; i++)
    {
        if (http_panel_ws_clients[i].fd < 0)
        {
            http_panel_ws_clients[i].fd = fd;
            strlcpy(http_panel_ws_clients[i].uri, ws_uri,
                    sizeof(http_panel_ws_clients[i].uri));
            break;
        }
    }

    xSemaphoreGive(http_panel_ws_mutex);
}

void http_panel_server_ws_remove(int fd)
{
    if (xSemaphoreTake(http_panel_ws_mutex, pdMS_TO_TICKS(200u)) != pdTRUE)
    {
        return;
    }

    for (uint8_t i = 0u; i < HTTP_PANEL_SERVER_MAX_WS_CLIENTS; i++)
    {
        if (http_panel_ws_clients[i].fd == fd)
        {
            http_panel_ws_clients[i].fd = -1;
            break;
        }
    }

    xSemaphoreGive(http_panel_ws_mutex);
}

void http_panel_server_push(const char    *ws_uri,
                             const char    *data,
                             size_t         len,
                             volatile bool *pending)
{
    do
    {
        if (!http_panel_server_hd || !data || len == 0u)
        {
            break;
        }

        int     fds[HTTP_PANEL_SERVER_MAX_WS_CLIENTS];
        uint8_t count = 0u;

        if (xSemaphoreTake(http_panel_ws_mutex, pdMS_TO_TICKS(200u)) != pdTRUE)
        {
            break;
        }

        for (uint8_t i = 0u; i < HTTP_PANEL_SERVER_MAX_WS_CLIENTS; i++)
        {
            if (http_panel_ws_clients[i].fd < 0)
            {
                continue;
            }

            if (ws_uri && strcmp(http_panel_ws_clients[i].uri, ws_uri) != 0)
            {
                continue;
            }

            fds[count++] = http_panel_ws_clients[i].fd;
        }

        xSemaphoreGive(http_panel_ws_mutex);

        /* No connected clients — nothing to allocate or send */
        if (count == 0u)
        {
            break;
        }

        for (uint8_t i = 0u; i < count; i++)
        {
            http_panel_push_arg_t *pa = malloc(sizeof(http_panel_push_arg_t));

            if (!pa)
            {
                break;
            }

            pa->hd      = http_panel_server_hd;
            pa->fd      = fds[i];
            pa->len     = len;
            pa->pending = (i == 0u) ? pending : NULL; /* only first client clears flag */
            pa->payload = malloc(len);

            if (!pa->payload)
            {
                free(pa);
                break;
            }

            memcpy(pa->payload, data, len);

            if (httpd_queue_work(http_panel_server_hd,
                                 http_panel_push_work, pa) != ESP_OK)
            {
                free(pa->payload);
                free(pa);
            }
        }
    } while (0);
}

uint8_t http_panel_server_ws_count(const char *ws_uri)
{
    uint8_t count = 0u;

    if (xSemaphoreTake(http_panel_ws_mutex, pdMS_TO_TICKS(200u)) == pdTRUE)
    {
        for (uint8_t i = 0u; i < HTTP_PANEL_SERVER_MAX_WS_CLIENTS; i++)
        {
            if (http_panel_ws_clients[i].fd < 0)
            {
                continue;
            }

            if (ws_uri && strcmp(http_panel_ws_clients[i].uri, ws_uri) != 0)
            {
                continue;
            }

            count++;
        }

        xSemaphoreGive(http_panel_ws_mutex);
    }

    return count;
}

httpd_handle_t http_panel_server_handle(void)
{
    return http_panel_server_hd;
}


/**** END OF FILE ****/
