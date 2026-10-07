/**
 ******************************************************************************
 * @file    ota_page.c
 * @author  Daniel Dyringer
 * @date    2026-08-13
 * @brief   OTA page — drag-and-drop firmware upload with progress.
 *
 *          Architecture:
 *            - GET /ota          serves ota_page.html
 *            - GET /ota/ws       WebSocket endpoint, progress push
 *            - POST /ota/upload  raw firmware body, first streaming-body
 *              handler in this codebase (see OTA_TODO.md Section 4)
 *            - Subscribes to OTA_EVENTS; pushes a JSON progress frame to all
 *              /ota/ws clients on every event (option B from the TODO doc —
 *              decoupled from the POST handler's own control flow).
 *
 *          Push protocol (JSON frames pushed to all /ota/ws clients):
 *            { "state": "receiving", "bytes_written": 1234,
 *              "bytes_expected": 567890, "error": null }
 *
 *          No auth/CSRF — matches the rest of http_panel's LAN-only trust
 *          model (see OTA_TODO.md Section 4).
 ******************************************************************************
 */


/* Includes ---------------------------------------------------------------- */
#include "ota_page.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "esp_event.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "http_panel_providers.h"
#include "http_panel_server.h"
#include "ota.h"

/* Constants and macros ------------------------------------------------------ */
// clang-format off

#define OTA_PAGE_WS_URI          "/ota/ws"
#define OTA_PAGE_WS_RX_BUF       64u

/** @brief Chunk size for the streaming POST /ota/upload body reads. */
#define OTA_PAGE_UPLOAD_CHUNK    4096u

// clang-format on

/* Private variables --------------------------------------------------------- */

static const char *ota_page_tag = "ota_page";

/**
 * @brief True while a push work item is queued in the httpd task.
 *        Prevents queuing a second push before the first is delivered — see
 *        http_panel_server_push()'s pending-flag contract.
 */
static volatile bool ota_page_push_pending = false;

extern const uint8_t ota_page_html_start[] asm("_binary_ota_page_html_start");
extern const uint8_t ota_page_html_end[]   asm("_binary_ota_page_html_end");

/* Private functions ----------------------------------------------------------- */

static const char *ota_page_state_name(ota_state_t state)
{
    switch (state)
    {
    case OTA_STATE_IDLE:           return "idle";
    case OTA_STATE_RECEIVING:      return "receiving";
    case OTA_STATE_VALIDATING:     return "validating";
    case OTA_STATE_APPLYING:       return "applying";
    case OTA_STATE_PENDING_REBOOT: return "pending_reboot";
    case OTA_STATE_FAILED:         return "failed";
    default:                       return "unknown";
    }
}

static char *ota_page_build_json(const ota_status_t *status)
{
    cJSON *root = cJSON_CreateObject();

    if (!root)
    {
        return NULL;
    }

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

    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    return json; /* caller must free() */
}

/* Called from the default event loop task — safe to push from here. */
static void ota_page_on_ota_event(
    void             *handler_arg,
    esp_event_base_t  base,
    int32_t           event_id,
    void             *event_data)
{
    (void)handler_arg;
    (void)base;
    (void)event_id;

    do
    {
        if (http_panel_server_ws_count(OTA_PAGE_WS_URI) == 0u)
        {
            break;
        }

        if (ota_page_push_pending)
        {
            break;
        }

        const ota_status_t *status = (const ota_status_t *)event_data;
        char *json = ota_page_build_json(status);

        if (!json)
        {
            break;
        }

        ota_page_push_pending = true;
        http_panel_server_push(OTA_PAGE_WS_URI, json, strlen(json),
                               &ota_page_push_pending);
        free(json);
    } while (0);
}

/* ---- HTTP handlers ------------------------------------------------------ */

static esp_err_t ota_page_h_root(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html");
    const size_t len = (size_t)(ota_page_html_end - ota_page_html_start);
    return httpd_resp_send(req, (const char *)ota_page_html_start, (ssize_t)len);
}

/* Called by httpd right after the WS handshake completes. See
 * [[idf6-ws-handler-breaking-change]] — do NOT push a frame from here; the
 * first frame arrives on the next OTA_EVENTS event. */
static esp_err_t ota_page_ws_connected(httpd_req_t *req)
{
    const int fd = httpd_req_to_sockfd(req);
    http_panel_server_ws_add(fd, OTA_PAGE_WS_URI);
    ESP_LOGI(ota_page_tag, "Client connected fd=%d", fd);
    return ESP_OK;
}

static esp_err_t ota_page_h_ws(httpd_req_t *req)
{
    esp_err_t error_code = ESP_OK;

    do
    {
        uint8_t          buf[OTA_PAGE_WS_RX_BUF] = {0};
        httpd_ws_frame_t frm = { .type = HTTPD_WS_TYPE_TEXT, .payload = buf };

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

        const cJSON *j_act = cJSON_GetObjectItemCaseSensitive(root, "action");

        if (cJSON_IsString(j_act) && strcmp(j_act->valuestring, "reboot") == 0)
        {
            ESP_LOGI(ota_page_tag, "action 'reboot'");
            /* No-op (returns normally) unless an update is actually staged
             * and waiting — see hp_action_ota_reboot()'s doc comment. */
            hp_action_ota_reboot();
        }

        cJSON_Delete(root);
    } while (0);

    return error_code;
}

static esp_err_t ota_page_h_version(httpd_req_t *req)
{
    esp_err_t error_code;
    char buf[HTTP_PANEL_PROVIDER_VALUE_LEN];
    hp_get_ota_version(buf, sizeof(buf));

    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "version", buf);
    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);

    if (!json)
    {
        httpd_resp_send_500(req);
        error_code = ESP_FAIL;
    }
    else
    {
        httpd_resp_set_type(req, "application/json");
        error_code = httpd_resp_send(req, json, HTTPD_RESP_USE_STRLEN);
        free(json);
    }

    return error_code;
}

/**
 * @brief Send a small JSON error response. Also calls ota_abort() as a
 *        safety net for failures ota.c itself doesn't already know about
 *        (e.g. a missing Content-Length, rejected before ota_begin() ever
 *        runs) — ota_abort() no-ops if ota.c already moved to FAILED and
 *        notified for this failure, so the state/error set by ota_write()/
 *        ota_finish() themselves is preserved, not overwritten.
 */
static esp_err_t ota_page_fail(httpd_req_t *req, int http_status,
                               const char *msg)
{
    ota_abort();
    httpd_resp_set_status(req, http_status == 400 ? "400 Bad Request"
                                                    : "500 Internal Server Error");
    httpd_resp_set_type(req, "application/json");
    char body[96];
    snprintf(body, sizeof(body), "{\"ok\":false,\"error\":\"%s\"}", msg);
    httpd_resp_send(req, body, HTTPD_RESP_USE_STRLEN);
    return ESP_FAIL;
}

static esp_err_t ota_page_h_upload(httpd_req_t *req)
{
    esp_err_t error_code = ESP_OK;

    do
    {
        if (req->content_len == 0u)
        {
            error_code = ota_page_fail(req, 400, "missing Content-Length");
            break;
        }

        if (ota_begin(req->content_len) != ESP_OK)
        {
            error_code = ota_page_fail(req, 500, "ota_begin failed");
            break;
        }

        static uint8_t buf[OTA_PAGE_UPLOAD_CHUNK];
        size_t         remaining    = req->content_len;
        bool           upload_failed = false;

        while (remaining > 0u)
        {
            const size_t want = remaining < sizeof(buf) ? remaining : sizeof(buf);
            const int    read  = httpd_req_recv(req, (char *)buf, want);

            if (read <= 0)
            {
                if (read == HTTPD_SOCK_ERR_TIMEOUT)
                {
                    continue; /* retry on timeout, same as other recv loops */
                }

                ESP_LOGE(ota_page_tag, "httpd_req_recv failed: %d", read);
                error_code = ota_page_fail(req, 500, "upload read failed");
                upload_failed = true;
                break;
            }

            if (ota_write(buf, (size_t)read) != ESP_OK)
            {
                error_code = ota_page_fail(req, 500, "ota_write failed");
                upload_failed = true;
                break;
            }

            remaining -= (size_t)read;
        }

        if (upload_failed)
        {
            break;
        }

        if (ota_finish() != ESP_OK)
        {
            error_code = ota_page_fail(req, 500, "ota_finish failed");
            break;
        }

        httpd_resp_set_type(req, "application/json");
        error_code = httpd_resp_send(req, "{\"ok\":true}", HTTPD_RESP_USE_STRLEN);
    } while (0);

    return error_code;
}

/* ---- URI descriptors ---------------------------------------------------- */

static const httpd_uri_t ota_page_uri_root = {
    .uri     = "/ota",
    .method  = HTTP_GET,
    .handler = ota_page_h_root,
};

static const httpd_uri_t ota_page_uri_ws = {
    .uri                  = OTA_PAGE_WS_URI,
    .method               = HTTP_GET,
    .handler              = ota_page_h_ws,
    .is_websocket         = true,
    .ws_post_handshake_cb = ota_page_ws_connected,
};

static const httpd_uri_t ota_page_uri_upload = {
    .uri     = "/ota/upload",
    .method  = HTTP_POST,
    .handler = ota_page_h_upload,
};

static const httpd_uri_t ota_page_uri_version = {
    .uri     = "/ota/version",
    .method  = HTTP_GET,
    .handler = ota_page_h_version,
};


/* Exposed API ------------------------------------------------------------- */

esp_err_t ota_page_init(void)
{
    esp_err_t error_code = ESP_OK;

    do
    {
        error_code = http_panel_server_register_uri(&ota_page_uri_root);

        if (error_code != ESP_OK)
        {
            break;
        }

        error_code = http_panel_server_register_uri(&ota_page_uri_ws);

        if (error_code != ESP_OK)
        {
            break;
        }

        error_code = http_panel_server_register_uri(&ota_page_uri_upload);

        if (error_code != ESP_OK)
        {
            break;
        }

        error_code = http_panel_server_register_uri(&ota_page_uri_version);

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
