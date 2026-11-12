/**
 ******************************************************************************
 * @file    dashboard.c
 * @author  Daniel Dyringer
 * @date    2026-03-09
 * @brief   Dashboard page — status, sensors and device actions.
 ******************************************************************************
 */


/* Includes ---------------------------------------------------------------- */
#include "dashboard.h"

#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "http_panel_providers.h"
#include "http_panel_server.h"
#include "utils.h"
#include "work_queue.h"


/* Constants and macros ---------------------------------------------------- */
// clang-format off

#define DASHBOARD_WS_URI            "/ws"
#define DASHBOARD_WS_RX_MAX         128u
#define DASHBOARD_PUSH_PERIOD_MS    1000u

// clang-format on

/* Private types ----------------------------------------------------------- */

typedef struct dashboard_item_s
{
    const char* label;
    hp_value_fn_t get_value;
} dashboard_item_t;


/* Private variables ------------------------------------------------------- */

static const char* dashboard_tag = "dashboard";

/* Touched only by the work_queue task. */
static http_panel_push_slot_t dashboard_push_slot = {0};

static work_queue_item_t dashboard_work = {0};

static const dashboard_item_t dashboard_status_items[] = {
    {"Uptime", hp_get_uptime},
};

static const dashboard_item_t dashboard_sensor_items[] = {
    {"Ambient light", hp_get_lux},
};

extern const uint8_t dashboard_html_start[] asm("_binary_dashboard_html_start");
extern const uint8_t dashboard_html_end[] asm("_binary_dashboard_html_end");


/* Private functions ------------------------------------------------------- */

static void dashboard_add_items(cJSON* array, const dashboard_item_t* items, size_t count)
{
    char buffer[HTTP_PANEL_PROVIDER_VALUE_LEN] = {0};

    for (size_t i = 0u; i < count; i++)
    {
        buffer[0] = '\0';
        items[i].get_value(buffer, sizeof(buffer));

        cJSON* item = cJSON_CreateObject();
        cJSON_AddStringToObject(item, "label", items[i].label);
        cJSON_AddStringToObject(item, "value", buffer);
        cJSON_AddItemToArray(array, item);
    }
}

static char* dashboard_build_json(void)
{
    char* json  = NULL;
    cJSON* root = cJSON_CreateObject();

    if (root != NULL)
    {
        dashboard_add_items(
            cJSON_AddArrayToObject(root, "status"),
            dashboard_status_items,
            ARRAY_SIZE(dashboard_status_items));
        dashboard_add_items(
            cJSON_AddArrayToObject(root, "sensors"),
            dashboard_sensor_items,
            ARRAY_SIZE(dashboard_sensor_items));

        json = cJSON_PrintUnformatted(root);
        cJSON_Delete(root);
    }

    return json; /* caller must free() */
}

static void dashboard_push(void)
{
    do
    {
        if (!http_panel_server_push_ready(&dashboard_push_slot))
        {
            break;
        }

        char* json = dashboard_build_json();

        if (json == NULL)
        {
            break;
        }

        http_panel_server_push(DASHBOARD_WS_URI, json, strlen(json), &dashboard_push_slot);
        free(json);
    } while (0);
}

/* work_queue task. Runs only while a client is connected: the last one
 * leaving ends the chain, the next connect starts it again. */
static void dashboard_push_work(void* context)
{
    ARG_UNUSED(context);

    if (http_panel_server_ws_count(DASHBOARD_WS_URI) > 0u)
    {
        dashboard_push();
        work_queue_schedule(&dashboard_work, DASHBOARD_PUSH_PERIOD_MS);
    }
}

/* ---- HTTP handlers ---- */

static esp_err_t dashboard_h_root(httpd_req_t* request)
{
    const size_t length = (size_t)(dashboard_html_end - dashboard_html_start);

    httpd_resp_set_type(request, "text/html");

    return httpd_resp_send(request, (const char*)dashboard_html_start, (ssize_t)length);
}

/* IDF 6.0 no longer invokes the URI handler on the handshake GET (commit
 * c9daea6, issue #18539), so the fd is registered here. Do NOT push from here:
 * the upgrade response has only just been sent. */
static esp_err_t dashboard_ws_connected(httpd_req_t* request)
{
    const int fd = httpd_req_to_sockfd(request);

    /* An error makes httpd close the socket, so the page reconnects instead of
     * showing "Connected" without ever getting a push. */
    const esp_err_t error_code = http_panel_server_ws_add(fd, DASHBOARD_WS_URI);

    if (error_code == ESP_OK)
    {
        ESP_LOGI(dashboard_tag, "Client connected fd=%d", fd);

        /* Delayed, never pushed: a frame sent before the upgrade completes
         * causes ECONNRESET. A no-op while the chain already runs. */
        work_queue_schedule(&dashboard_work, DASHBOARD_PUSH_PERIOD_MS);
    }

    return error_code;
}

static esp_err_t dashboard_h_ws(httpd_req_t* request)
{
    esp_err_t error_code = ESP_OK;

    /* IDF 6.0: only invoked for inbound data frames. */
    do
    {
        uint8_t buffer[DASHBOARD_WS_RX_MAX] = {0};
        httpd_ws_frame_t frame = {.type = HTTPD_WS_TYPE_TEXT, .payload = buffer};

        error_code = httpd_ws_recv_frame(request, &frame, sizeof(buffer) - 1u);

        if (error_code != ESP_OK)
        {
            break;
        }

        buffer[frame.len] = '\0';
        cJSON* root       = cJSON_Parse((char*)buffer);

        if (root == NULL)
        {
            break;
        }

        const cJSON* json_action = cJSON_GetObjectItemCaseSensitive(root, "action");

        if (cJSON_IsString(json_action))
        {
            ESP_LOGI(dashboard_tag, "action '%s'", json_action->valuestring);

            if (strcmp(json_action->valuestring, "reboot") == 0)
            {
                hp_action_reboot();
            }
            else if (strcmp(json_action->valuestring, "factory_reset") == 0)
            {
                hp_action_factory_reset();
            }
            else
            {
                /* Unknown actions are only logged. */
            }
        }

        cJSON_Delete(root);
    } while (0);

    return error_code;
}


/* Exposed API ------------------------------------------------------------- */

esp_err_t dashboard_init(void)
{
    esp_err_t error_code = ESP_OK;

    /* Static: http_panel_server keeps the URIs by pointer. */
    static const httpd_uri_t dashboard_uris[] = {
        {
            .uri     = "/",
            .method  = HTTP_GET,
            .handler = dashboard_h_root,
        },
        {
            .uri                  = DASHBOARD_WS_URI,
            .method               = HTTP_GET,
            .handler              = dashboard_h_ws,
            .is_websocket         = true,
            .ws_post_handshake_cb = dashboard_ws_connected,
        },
    };

    do
    {
        for (size_t i = 0u; i < ARRAY_SIZE(dashboard_uris); i++)
        {
            error_code = http_panel_server_register_uri(&dashboard_uris[i]);

            if (error_code != ESP_OK)
            {
                break;
            }
        }

        if (error_code != ESP_OK)
        {
            break;
        }

        error_code = work_queue_item_init(&dashboard_work, dashboard_push_work, NULL);
    } while (0);

    return error_code;
}

/**** END OF FILE ****/
