/**
 ******************************************************************************
 * @file    ota_page.c
 * @author  Daniel Dyringer
 * @date    2026-08-13
 * @brief   OTA page — drag-and-drop firmware upload with progress.
 *
 *          Progress is pushed to /ota/ws clients from OTA_EVENTS, decoupled
 *          from the upload handler's control flow.
 *
 *          The upload runs through http_panel_transfer, so it never blocks
 *          the httpd task; its worker is the only caller of
 *          ota_begin/write/finish/abort.
 *
 *          No auth/CSRF — same LAN-only trust model as the rest of http_panel.
 ******************************************************************************
 */


/* Includes ---------------------------------------------------------------- */
#include "ota_page.h"

#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "esp_event.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "http_panel_providers.h"
#include "http_panel_server.h"
#include "http_panel_transfer.h"
#include "ota.h"
#include "utils.h"


/* Constants and macros ---------------------------------------------------- */
// clang-format off

#define OTA_PAGE_WS_URI                 "/ota/ws"
#define OTA_PAGE_WS_RX_MAX              64u
#define OTA_PAGE_NO_LENGTH_BODY         "{\"ok\":false,\"error\":\"missing Content-Length\"}"

// clang-format on

/* Private types ----------------------------------------------------------- */

typedef struct ota_page_item_s
{
    const char* label;
    hp_value_fn_t get_value;
} ota_page_item_t;


/* Private variables ------------------------------------------------------- */

static const char* ota_page_tag = "ota_page";

/* Touched only by the default event loop task. */
static http_panel_push_slot_t ota_page_push_slot = {0};

static const ota_page_item_t ota_page_info_items[] = {
    {"Firmware", hp_get_ota_version},
    {"Build", hp_get_build_date},
    {"ESP-IDF", hp_get_idf_version},
    {"Running partition", hp_get_partition},
    {"Next update slot", hp_get_ota_next_partition},
    {"Image state", hp_get_ota_image_state},
};

extern const uint8_t ota_page_html_start[] asm("_binary_ota_page_html_start");
extern const uint8_t ota_page_html_end[] asm("_binary_ota_page_html_end");


/* Private functions ------------------------------------------------------- */

static const char* ota_page_state_name(ota_state_t state)
{
    const char* name = NULL;

    switch (state)
    {
        case OTA_STATE_IDLE:
        {
            name = "idle";
        }
        break;
        case OTA_STATE_RECEIVING:
        {
            name = "receiving";
        }
        break;
        case OTA_STATE_VALIDATING:
        {
            name = "validating";
        }
        break;
        case OTA_STATE_APPLYING:
        {
            name = "applying";
        }
        break;
        case OTA_STATE_PENDING_REBOOT:
        {
            name = "pending_reboot";
        }
        break;
        case OTA_STATE_FAILED:
        {
            name = "failed";
        }
        break;
        default:
        {
            name = "unknown";
        }
        break;
    }

    return name;
}

static char* ota_page_build_json(const ota_status_t* status)
{
    char* json  = NULL;
    cJSON* root = cJSON_CreateObject();

    if (root != NULL)
    {
        cJSON_AddStringToObject(root, "state", ota_page_state_name(status->state));
        cJSON_AddNumberToObject(root, "bytes_written", (double)status->bytes_written);
        cJSON_AddNumberToObject(root, "bytes_expected", (double)status->bytes_expected);

        if (status->last_error != ESP_OK)
        {
            cJSON_AddStringToObject(root, "error", esp_err_to_name(status->last_error));
        }
        else
        {
            cJSON_AddNullToObject(root, "error");
        }

        json = cJSON_PrintUnformatted(root);
        cJSON_Delete(root);
    }

    return json; /* caller must free() */
}

/* Called from the default event loop task — safe to push from here. */
static void ota_page_on_ota_event(
    void* handler_arg,
    esp_event_base_t base,
    int32_t event_id,
    void* event_data)
{
    ARG_UNUSED(handler_arg);
    ARG_UNUSED(base);
    ARG_UNUSED(event_id);

    do
    {
        if (http_panel_server_ws_count(OTA_PAGE_WS_URI) == 0u)
        {
            break;
        }

        if (!http_panel_server_push_ready(&ota_page_push_slot))
        {
            break;
        }

        const ota_status_t* status = (const ota_status_t*)event_data;
        char* json                 = ota_page_build_json(status);

        if (json == NULL)
        {
            break;
        }

        http_panel_server_push(OTA_PAGE_WS_URI, json, strlen(json), &ota_page_push_slot);
        free(json);
    } while (0);
}

/* ---- HTTP handlers ---- */

static esp_err_t ota_page_h_root(httpd_req_t* request)
{
    const size_t length = (size_t)(ota_page_html_end - ota_page_html_start);

    httpd_resp_set_type(request, "text/html");

    return httpd_resp_send(request, (const char*)ota_page_html_start, (ssize_t)length);
}

/* Do NOT push a frame from here (IDF 6 WS handler change); the first frame
 * arrives with the next OTA_EVENTS event. */
static esp_err_t ota_page_ws_connected(httpd_req_t* request)
{
    const int fd = httpd_req_to_sockfd(request);

    /* An error makes httpd close the socket, so the page reconnects instead of
     * showing "Connected" without ever getting a push. */
    const esp_err_t error_code = http_panel_server_ws_add(fd, OTA_PAGE_WS_URI);

    if (error_code == ESP_OK)
    {
        ESP_LOGI(ota_page_tag, "Client connected fd=%d", fd);
    }

    return error_code;
}

static esp_err_t ota_page_h_ws(httpd_req_t* request)
{
    esp_err_t error_code = ESP_OK;

    do
    {
        uint8_t buffer[OTA_PAGE_WS_RX_MAX] = {0};
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

        if (cJSON_IsString(json_action) && (strcmp(json_action->valuestring, "reboot") == 0))
        {
            ESP_LOGI(ota_page_tag, "action 'reboot'");
            /* No-op unless an update is staged. */
            hp_action_ota_reboot();
        }

        cJSON_Delete(root);
    } while (0);

    return error_code;
}

static char* ota_page_build_info(void)
{
    char* json  = NULL;
    cJSON* root = cJSON_CreateObject();

    if (root != NULL)
    {
        cJSON* const array                         = cJSON_AddArrayToObject(root, "info");
        char buffer[HTTP_PANEL_PROVIDER_VALUE_LEN] = {0};

        for (size_t i = 0u; i < ARRAY_SIZE(ota_page_info_items); i++)
        {
            buffer[0] = '\0';
            ota_page_info_items[i].get_value(buffer, sizeof(buffer));

            cJSON* const item = cJSON_CreateObject();
            cJSON_AddStringToObject(item, "label", ota_page_info_items[i].label);
            cJSON_AddStringToObject(item, "value", buffer);
            cJSON_AddItemToArray(array, item);
        }

        json = cJSON_PrintUnformatted(root);
        cJSON_Delete(root);
    }

    return json; /* caller must free() */
}

static esp_err_t ota_page_h_info(httpd_req_t* request)
{
    esp_err_t error_code = ESP_OK;
    char* json           = ota_page_build_info();

    if (json == NULL)
    {
        httpd_resp_send_500(request);
        error_code = ESP_FAIL;
    }
    else
    {
        httpd_resp_set_type(request, "application/json");
        error_code = httpd_resp_send(request, json, HTTPD_RESP_USE_STRLEN);
        free(json);
    }

    return error_code;
}

/* ---- Upload ---- */

static esp_err_t ota_page_upload_open(void* context, httpd_req_t* request)
{
    ARG_UNUSED(context);

    const esp_err_t begin_error = ota_begin(request->content_len);

    /* ota.c logs the cause. ESP_FAIL keeps the 500 this endpoint has always
     * answered; the transfer status mapping would turn e.g. a pending reboot
     * (ESP_ERR_INVALID_STATE) into 409. */
    return (begin_error == ESP_OK) ? ESP_OK : ESP_FAIL;
}

static esp_err_t ota_page_upload_write(void* context, const uint8_t* data, size_t length)
{
    ARG_UNUSED(context);

    return ota_write(data, length);
}

static esp_err_t ota_page_upload_close(void* context, bool success)
{
    ARG_UNUSED(context);

    esp_err_t error_code = ESP_OK;

    if (success)
    {
        error_code = ota_finish();
    }
    else
    {
        /* Covers failures ota.c doesn't know about (e.g. a dropped client);
         * it no-ops if ota.c already failed, keeping its error. */
        error_code = ota_abort();
    }

    return error_code;
}

static const http_panel_transfer_ops_t ota_page_upload_ops = {
    .open  = ota_page_upload_open,
    .write = ota_page_upload_write,
    .read  = NULL,
    .close = ota_page_upload_close,
};

static esp_err_t ota_page_h_upload(httpd_req_t* request)
{
    esp_err_t error_code = ESP_OK;

    if (request->content_len == 0u)
    {
        ESP_LOGE(ota_page_tag, "Upload failed: missing Content-Length");
        httpd_resp_set_status(request, "400 Bad Request");
        httpd_resp_set_type(request, "application/json");
        httpd_resp_sendstr(request, OTA_PAGE_NO_LENGTH_BODY);
        /* Closes the socket, as for every other upload error. */
        error_code = ESP_FAIL;
    }
    else
    {
        error_code = http_panel_transfer_submit(request, &ota_page_upload_ops, NULL, 0u);
    }

    return error_code;
}


/* Exposed API ------------------------------------------------------------- */

esp_err_t ota_page_init(void)
{
    esp_err_t error_code = ESP_OK;

    /* Static: http_panel_server keeps the URIs by pointer. */
    static const httpd_uri_t ota_page_uris[] = {
        {
            .uri     = "/ota",
            .method  = HTTP_GET,
            .handler = ota_page_h_root,
        },
        {
            .uri                  = OTA_PAGE_WS_URI,
            .method               = HTTP_GET,
            .handler              = ota_page_h_ws,
            .is_websocket         = true,
            .ws_post_handshake_cb = ota_page_ws_connected,
        },
        {
            .uri     = "/ota/upload",
            .method  = HTTP_POST,
            .handler = ota_page_h_upload,
        },
        {
            .uri     = "/ota/info",
            .method  = HTTP_GET,
            .handler = ota_page_h_info,
        },
    };

    do
    {
        for (size_t i = 0u; i < ARRAY_SIZE(ota_page_uris); i++)
        {
            error_code = http_panel_server_register_uri(&ota_page_uris[i]);

            if (error_code != ESP_OK)
            {
                break;
            }
        }

        if (error_code != ESP_OK)
        {
            break;
        }

        error_code = esp_event_handler_register(
            OTA_EVENTS, ESP_EVENT_ANY_ID, ota_page_on_ota_event, NULL);
    } while (0);

    return error_code;
}

/**** END OF FILE ****/
