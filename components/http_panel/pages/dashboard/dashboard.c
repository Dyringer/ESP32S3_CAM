/**
 ******************************************************************************
 * @file    dashboard.c
 * @author  Daniel Dyringer
 * @date    2026-03-09
 * @brief   Dashboard page — device status and configuration.
 *
 *          Architecture:
 *            - GET /          serves dashboard.html
 *            - GET /ws        WebSocket endpoint for data push and actions
 *            - A periodic esp_timer builds a JSON snapshot from providers and
 *              pushes it to all /ws clients.
 *
 *          Push protocol (JSON frame pushed to all /ws clients):
 *            { "cpu":    [ <core0 %>, <core1 %> ],   (-1 = still measuring)
 *              "status": [ {"label": "...", "value": "..."}, ... ],
 *              "config": [ {"label": "...", "value": "..."}, ... ] }
 *
 *          Inbound (browser -> device):
 *            { "action": "reboot" | "factory_reset" }
 ******************************************************************************
 */


/* Includes ---------------------------------------------------------------- */
#include "dashboard.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "http_panel_providers.h"
#include "http_panel_server.h"
#include "utils.h"


/* Constants and macros ---------------------------------------------------- */
// clang-format off

#define DASHBOARD_WS_URI            "/ws"
#define DASHBOARD_WS_RX_BUF         128u

/** @brief Interval between dashboard pushes in microseconds. */
#define DASHBOARD_PUSH_INTERVAL_US  1000000LL  /* 1 s */

// clang-format on


/* Private types ----------------------------------------------------------- */

typedef struct
{
    const char    *label;
    hp_value_fn_t  fn;
} dashboard_item_t;


/* Private variables ------------------------------------------------------- */

static const char *dashboard_tag = "dashboard";

/**
 * @brief True while a push work item is queued in the httpd task.
 *        Cleared by http_panel_push_work() after the frame is sent.
 *        Prevents queuing a second push before the first is delivered.
 */
static volatile bool dashboard_push_pending = false;

/** @brief Periodic push timer. */
static esp_timer_handle_t dashboard_timer = NULL;

static const dashboard_item_t dashboard_status_items[] = {
    { "Uptime",         hp_get_uptime     },
    { "Free heap",      hp_get_heap       },
    { "Min free heap",  hp_get_heap_min   },
    { "PSRAM free",     hp_get_psram      },
    { "WiFi state",     hp_get_wifi_state },
    { "SSID",           hp_get_ssid       },
    { "RSSI",           hp_get_rssi       },
    { "IP",             hp_get_ip         },
};

static const dashboard_item_t dashboard_config_items[] = {
    { "Board",          hp_get_board       },
    { "Chip",           hp_get_chip        },
    { "Flash",          hp_get_flash_size  },
    { "Camera",         hp_get_camera      },
    { "MAC",            hp_get_mac         },
    { "Firmware",       hp_get_ota_version },
    { "Build",          hp_get_build_date  },
    { "ESP-IDF",        hp_get_idf_version },
    { "Partition",      hp_get_partition   },
};

extern const uint8_t dashboard_html_start[]
    asm("_binary_dashboard_html_start");
extern const uint8_t dashboard_html_end[]
    asm("_binary_dashboard_html_end");


/* Private functions ------------------------------------------------------- */

static void dashboard_add_items(cJSON                  *arr,
                                const dashboard_item_t *items,
                                size_t                  count)
{
    char buf[HTTP_PANEL_PROVIDER_VALUE_LEN];

    for (size_t i = 0u; i < count; i++)
    {
        buf[0] = '\0';
        items[i].fn(buf, sizeof(buf));
        cJSON *item = cJSON_CreateObject();
        cJSON_AddStringToObject(item, "label", items[i].label);
        cJSON_AddStringToObject(item, "value", buf);
        cJSON_AddItemToArray(arr, item);
    }
}

static char *dashboard_build_json(void)
{
    cJSON *root = cJSON_CreateObject();

    if (!root)
    {
        return NULL;
    }

    hp_add_cpu_load(root);
    dashboard_add_items(cJSON_AddArrayToObject(root, "status"),
                        dashboard_status_items, ARRAY_SIZE(dashboard_status_items));
    dashboard_add_items(cJSON_AddArrayToObject(root, "config"),
                        dashboard_config_items, ARRAY_SIZE(dashboard_config_items));

    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    return json; /* caller must free() */
}

/**
 * @brief Build and queue a dashboard frame to all connected clients.
 *        No-op when there are no clients or a push is already in flight.
 *
 * @note  Never call this from the WS GET (handshake) handler — sending a frame
 *        before the upgrade completes causes ECONNRESET. Pushes come only from
 *        the periodic timer.
 */
static void dashboard_push(void)
{
    do
    {
        if (http_panel_server_ws_count(DASHBOARD_WS_URI) == 0u)
        {
            break;
        }

        if (dashboard_push_pending)
        {
            break;
        }

        dashboard_push_pending = true;

        char *json = dashboard_build_json();

        if (!json)
        {
            dashboard_push_pending = false;
            break;
        }

        http_panel_server_push(DASHBOARD_WS_URI, json, strlen(json),
                               &dashboard_push_pending);
        free(json);
    } while (0);
}

static void dashboard_timer_cb(void *arg)
{
    (void)arg;
    dashboard_push();
}

/* ---- HTTP handlers ------------------------------------------------------ */

static esp_err_t dashboard_h_root(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html");
    const size_t len =
        (size_t)(dashboard_html_end - dashboard_html_start);
    return httpd_resp_send(
        req, (const char *)dashboard_html_start, (ssize_t)len);
}

/* Called by httpd right after the WS handshake completes. In ESP-IDF 6.0 the
 * URI handler is no longer invoked on the handshake GET (commit c9daea6,
 * issue #18539), so the client fd is registered here instead. Do NOT push a
 * frame from this callback — the upgrade response has only just been sent; the
 * periodic timer delivers the first frame within DASHBOARD_PUSH_INTERVAL_US. */
static esp_err_t dashboard_ws_connected(httpd_req_t *req)
{
    const int fd = httpd_req_to_sockfd(req);
    http_panel_server_ws_add(fd, DASHBOARD_WS_URI);
    ESP_LOGI(dashboard_tag, "Client connected fd=%d", fd);
    return ESP_OK;
}

static esp_err_t dashboard_h_ws(httpd_req_t *req)
{
    esp_err_t error_code = ESP_OK;

    /* In IDF 6.0 this handler is only invoked for inbound data frames; the
     * handshake GET is handled by dashboard_ws_connected() above. */
    do
    {
        uint8_t          buf[DASHBOARD_WS_RX_BUF] = {0};
        httpd_ws_frame_t frm = { .type = HTTPD_WS_TYPE_TEXT,
                                  .payload = buf };

        error_code = httpd_ws_recv_frame(req, &frm, sizeof(buf) - 1u);

        if (error_code != ESP_OK)
        {
            break;
        }

        buf[frm.len] = '\0';
        cJSON *root  = cJSON_Parse((char *)buf);

        if (!root)
        {
            break;
        }

        const cJSON *j_act =
            cJSON_GetObjectItemCaseSensitive(root, "action");

        if (cJSON_IsString(j_act))
        {
            ESP_LOGI(dashboard_tag, "action '%s'", j_act->valuestring);

            if (strcmp(j_act->valuestring, "reboot") == 0)
            {
                hp_action_reboot();
            }
            else if (strcmp(j_act->valuestring, "factory_reset") == 0)
            {
                hp_action_factory_reset();
            }
        }

        cJSON_Delete(root);
    } while (0);

    return error_code;
}

/* ---- URI descriptors ---------------------------------------------------- */

static const httpd_uri_t dashboard_uri_root = {
    .uri     = "/",
    .method  = HTTP_GET,
    .handler = dashboard_h_root,
};

static const httpd_uri_t dashboard_uri_ws = {
    .uri                  = DASHBOARD_WS_URI,
    .method               = HTTP_GET,
    .handler              = dashboard_h_ws,
    .is_websocket         = true,
    .ws_post_handshake_cb = dashboard_ws_connected,
};


/* Exposed API ------------------------------------------------------------- */

esp_err_t dashboard_init(void)
{
    esp_err_t error_code = ESP_OK;

    do
    {
        error_code = http_panel_server_register_uri(&dashboard_uri_root);

        if (error_code != ESP_OK)
        {
            break;
        }

        error_code = http_panel_server_register_uri(&dashboard_uri_ws);

        if (error_code != ESP_OK)
        {
            break;
        }

        const esp_timer_create_args_t timer_args = {
            .callback = dashboard_timer_cb,
            .name     = "dashboard_push",
        };

        error_code = esp_timer_create(&timer_args, &dashboard_timer);

        if (error_code != ESP_OK)
        {
            break;
        }

        /* dashboard_push() no-ops when no clients are connected, so this is
         * cheap while idle. */
        error_code = esp_timer_start_periodic(dashboard_timer,
                                              DASHBOARD_PUSH_INTERVAL_US);
    } while (0);

    return error_code;
}


/**** END OF FILE ****/
