/**
 ******************************************************************************
 * @file    wifi_provisioning_http.c
 * @author  Daniel Dyringer
 * @date    2026-03-04
 * @brief   Captive portal HTTP layer — server lifecycle and URI handlers.
 ******************************************************************************
 */


/* Includes ---------------------------------------------------------------- */
#include "wifi_provisioning_http.h"

#include <stdio.h>
#include <string.h>

#include "cJSON.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "net_wifi_sta.h"
#include "utils.h"
#include "wifi_connection.h"
#include "wifi_types.h"


/* Constants and macros ---------------------------------------------------- */
// clang-format off

#define WIFI_PROVISIONING_HTTP_AP_IP            "192.168.4.1"
#define WIFI_PROVISIONING_HTTP_TASK_STACK       6144u
/* HTTPD_DEFAULT_CONFIG() leaves httpd unpinned. */
#define WIFI_PROVISIONING_HTTP_TASK_PRIO        5u
#define WIFI_PROVISIONING_HTTP_TASK_CORE        0
#define WIFI_PROVISIONING_HTTP_ERROR_BODY_MAX   192u
#define WIFI_PROVISIONING_HTTP_REQUEST_BODY_MAX 256u

/* Lets the HTTP response flush before the credentials are applied. */
#define WIFI_PROVISIONING_HTTP_APPLY_DELAY_US   (500ULL * 1000ULL)

// clang-format on

/* Private types ----------------------------------------------------------- */


/* Private variables ------------------------------------------------------- */

static const char* wifi_provisioning_http_tag = "wifi_prov_http";

static httpd_handle_t wifi_provisioning_http_server          = NULL;
static esp_timer_handle_t wifi_provisioning_http_apply_timer = NULL;

/* Written by POST /configure, read later by the apply timer callback. */
static char wifi_provisioning_http_ssid[WIFI_TYPES_SSID_LEN]     = {0};
static char wifi_provisioning_http_password[WIFI_TYPES_PASS_LEN] = {0};

/* EMBED_FILES symbols: _binary_<file name, non-alphanumerics -> '_'>_start/_end.
 * GCC asm labels bind the C names to those linker symbols. */
extern const uint8_t wifi_provisioning_http_html_start[] asm(
    "_binary_wifi_provisioning_html_start");
extern const uint8_t wifi_provisioning_http_html_end[] asm(
    "_binary_wifi_provisioning_html_end");


/* Private functions ------------------------------------------------------- */

/* Deferred via timer: wifi_provisioning_stop() -> httpd_stop() deadlocks
 * when called from within an httpd handler. */
static void wifi_provisioning_http_apply_cb(void* arg)
{
    ARG_UNUSED(arg);

    const esp_err_t error_code = wifi_connection_set_credentials(
        wifi_provisioning_http_ssid, wifi_provisioning_http_password);

    if (error_code != ESP_OK)
    {
        ESP_LOGE(
            wifi_provisioning_http_tag,
            "Applying credentials failed: %s",
            esp_err_to_name(error_code));
    }
}

/* JSON, not httpd_resp_send_err()'s HTML, so the page can show the reason. */
static esp_err_t wifi_provisioning_http_send_error(httpd_req_t* request, const char* message)
{
    char body[WIFI_PROVISIONING_HTTP_ERROR_BODY_MAX] = {0};

    snprintf(body, sizeof(body), "{\"ok\":false,\"error\":\"%s\"}", message);
    httpd_resp_set_status(request, "400 Bad Request");
    httpd_resp_set_type(request, "application/json");

    return httpd_resp_sendstr(request, body);
}

static esp_err_t wifi_provisioning_http_h_root(httpd_req_t* request)
{
    const size_t length =
        (size_t)(wifi_provisioning_http_html_end - wifi_provisioning_http_html_start);

    httpd_resp_set_type(request, "text/html");

    return httpd_resp_send(
        request, (const char*)wifi_provisioning_http_html_start, (ssize_t)length);
}

static esp_err_t wifi_provisioning_http_h_scan(httpd_req_t* request)
{
    esp_err_t error_code      = ESP_OK;
    wifi_ap_record_t* records = NULL;
    uint16_t count            = 0u;
    cJSON* array              = NULL;
    char* json                = NULL;

    do
    {
        if (net_wifi_sta_scan(&records, &count) != ESP_OK)
        {
            error_code = httpd_resp_send_err(
                request, HTTPD_500_INTERNAL_SERVER_ERROR, "Scan failed");
            break;
        }

        array = cJSON_CreateArray();

        if (array == NULL)
        {
            error_code = httpd_resp_send_err(
                request, HTTPD_500_INTERNAL_SERVER_ERROR, "Out of memory");
            break;
        }

        for (uint16_t i = 0u; i < count; i++)
        {
            if (records[i].ssid[0] != 0u)
            {
                cJSON* access_point = cJSON_CreateObject();

                cJSON_AddStringToObject(access_point, "ssid", (const char*)records[i].ssid);
                cJSON_AddNumberToObject(access_point, "rssi", records[i].rssi);
                cJSON_AddItemToArray(array, access_point);
            }
        }

        json = cJSON_PrintUnformatted(array);

        if (json == NULL)
        {
            error_code = httpd_resp_send_err(
                request, HTTPD_500_INTERNAL_SERVER_ERROR, "Out of memory");
            break;
        }

        httpd_resp_set_type(request, "application/json");
        error_code = httpd_resp_send(request, json, HTTPD_RESP_USE_STRLEN);
    } while (0);

    cJSON_Delete(array);
    cJSON_free(json);
    net_wifi_sta_scan_free(records);

    return error_code;
}

static esp_err_t wifi_provisioning_http_h_configure(httpd_req_t* request)
{
    esp_err_t error_code = ESP_OK;
    cJSON* root          = NULL;

    do
    {
        char body[WIFI_PROVISIONING_HTTP_REQUEST_BODY_MAX] = {0};

        if (httpd_req_recv(request, body, sizeof(body) - 1u) <= 0)
        {
            error_code = wifi_provisioning_http_send_error(request, "Empty request.");
            break;
        }

        root = cJSON_Parse(body);

        const cJSON* json_ssid     = cJSON_GetObjectItemCaseSensitive(root, "ssid");
        const cJSON* json_password = cJSON_GetObjectItemCaseSensitive(root, "pass");
        const char* password =
            cJSON_IsString(json_password) ? json_password->valuestring : "";

        /* Checked before the copy so an over-long value is rejected, not
         * silently truncated into a network that never connects. */
        if (!cJSON_IsString(json_ssid)
            || !wifi_connection_credentials_valid(json_ssid->valuestring, password))
        {
            error_code = wifi_provisioning_http_send_error(
                request, WIFI_CONNECTION_CREDENTIALS_RULE ".");
            break;
        }

        strlcpy(
            wifi_provisioning_http_ssid,
            json_ssid->valuestring,
            sizeof(wifi_provisioning_http_ssid));
        strlcpy(
            wifi_provisioning_http_password,
            password,
            sizeof(wifi_provisioning_http_password));

        httpd_resp_set_type(request, "application/json");
        error_code = httpd_resp_sendstr(request, "{\"ok\":true}");

        const esp_err_t timer_result = esp_timer_start_once(
            wifi_provisioning_http_apply_timer, WIFI_PROVISIONING_HTTP_APPLY_DELAY_US);

        if (timer_result != ESP_OK)
        {
            ESP_LOGE(
                wifi_provisioning_http_tag,
                "Apply timer start failed: %s",
                esp_err_to_name(timer_result));
        }
    } while (0);

    cJSON_Delete(root);

    return error_code;
}

static esp_err_t wifi_provisioning_http_h_redirect(httpd_req_t* request)
{
    httpd_resp_set_status(request, "302 Found");
    httpd_resp_set_hdr(request, "Location", "http://" WIFI_PROVISIONING_HTTP_AP_IP "/");

    return httpd_resp_send(request, NULL, 0);
}

static esp_err_t wifi_provisioning_http_register_uris(void)
{
    esp_err_t error_code = ESP_OK;

    /* httpd copies each entry, so a local table is enough. The catch-all has
     * to be last: wildcard matching takes the first registered match. */
    const httpd_uri_t uris[] = {
        {
            .uri     = "/",
            .method  = HTTP_GET,
            .handler = wifi_provisioning_http_h_root,
        },
        {
            .uri     = "/scan",
            .method  = HTTP_GET,
            .handler = wifi_provisioning_http_h_scan,
        },
        {
            .uri     = "/configure",
            .method  = HTTP_POST,
            .handler = wifi_provisioning_http_h_configure,
        },
        {
            .uri     = "/*",
            .method  = HTTP_GET,
            .handler = wifi_provisioning_http_h_redirect,
        },
    };

    for (size_t i = 0u; (i < ARRAY_SIZE(uris)) && (error_code == ESP_OK); i++)
    {
        error_code = httpd_register_uri_handler(wifi_provisioning_http_server, &uris[i]);
    }

    return error_code;
}


/* Exposed API ------------------------------------------------------------- */

esp_err_t wifi_provisioning_http_start(void)
{
    esp_err_t error_code = ESP_OK;

    do
    {
        if (wifi_provisioning_http_server != NULL)
        {
            break;
        }

        const esp_timer_create_args_t timer_config = {
            .callback = wifi_provisioning_http_apply_cb,
            .name     = "prov_cred",
        };

        error_code = esp_timer_create(&timer_config, &wifi_provisioning_http_apply_timer);

        if (error_code != ESP_OK)
        {
            break;
        }

        httpd_config_t config   = HTTPD_DEFAULT_CONFIG();
        config.stack_size       = WIFI_PROVISIONING_HTTP_TASK_STACK;
        config.task_priority    = WIFI_PROVISIONING_HTTP_TASK_PRIO;
        config.core_id          = WIFI_PROVISIONING_HTTP_TASK_CORE;
        config.lru_purge_enable = true;
        config.uri_match_fn     = httpd_uri_match_wildcard;

        error_code = httpd_start(&wifi_provisioning_http_server, &config);

        if (error_code != ESP_OK)
        {
            break;
        }

        error_code = wifi_provisioning_http_register_uris();

        if (error_code != ESP_OK)
        {
            break;
        }

        ESP_LOGI(
            wifi_provisioning_http_tag,
            "HTTP server up at http://%s",
            WIFI_PROVISIONING_HTTP_AP_IP);
    } while (0);

    if (error_code != ESP_OK)
    {
        ESP_LOGE(
            wifi_provisioning_http_tag, "Start failed: %s", esp_err_to_name(error_code));
        wifi_provisioning_http_stop();
    }

    return error_code;
}

esp_err_t wifi_provisioning_http_stop(void)
{
    /* Server first: a /configure still in its handler arms the apply timer. */
    if (wifi_provisioning_http_server != NULL)
    {
        httpd_stop(wifi_provisioning_http_server);
        wifi_provisioning_http_server = NULL;
        ESP_LOGI(wifi_provisioning_http_tag, "HTTP server stopped");
    }

    if (wifi_provisioning_http_apply_timer != NULL)
    {
        esp_timer_stop(wifi_provisioning_http_apply_timer);
        esp_timer_delete(wifi_provisioning_http_apply_timer);
        wifi_provisioning_http_apply_timer = NULL;
    }

    return ESP_OK;
}

/**** END OF FILE ****/
