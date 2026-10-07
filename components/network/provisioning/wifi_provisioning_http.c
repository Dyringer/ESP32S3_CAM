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

#include "cJSON.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "wifi_sta.h"
#include "wifi_types.h"


/* Constants and macros ---------------------------------------------------- */
// clang-format off
#define WIFI_PROV_HTTP_AP_IP         "192.168.4.1"
#define WIFI_PROV_HTTP_TASK_STACK    6144u

/** @brief Delay before applying credentials to let the HTTP response flush. */
#define WIFI_PROV_HTTP_CRED_DELAY_US (500u * 1000ULL)
// clang-format on


/* Private variables ------------------------------------------------------- */

static const char *wifi_prov_http_tag = "wifi_prov_http";

static httpd_handle_t     wifi_prov_httpd      = NULL;
static esp_timer_handle_t wifi_prov_cred_timer = NULL;

/**
 * @brief Credential buffers written by POST /configure.
 *        Declared extern in wifi_provisioning_http.h so the deferred timer
 *        callback in wifi_provisioning.c can read them after the HTTP response
 *        is sent.
 */
char wifi_prov_ssid[WIFI_SSID_LEN];
char wifi_prov_pass[WIFI_PASS_LEN];

/* HTML provisioning page — embedded at link time from wifi_provisioning.html.
 * Edit wifi_provisioning.html directly; do not modify these declarations.
 * @note  ESP-IDF EMBED_FILES appends _start and _end symbols to the filename
 *        with all non-alphanumeric characters replaced by underscores.       */
extern const uint8_t wifi_provisioning_html_start[]
    asm("_binary_wifi_provisioning_html_start");
extern const uint8_t wifi_provisioning_html_end[]
    asm("_binary_wifi_provisioning_html_end");


/* Private functions ------------------------------------------------------- */

static esp_err_t wifi_prov_h_root(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html");
    const size_t len =
        (size_t)(wifi_provisioning_html_end - wifi_provisioning_html_start);
    return httpd_resp_send(
        req, (const char *)wifi_provisioning_html_start, (ssize_t)len);
}

static esp_err_t wifi_prov_h_scan(httpd_req_t *req)
{
    esp_err_t        error_code = ESP_OK;
    wifi_ap_record_t *recs = NULL;
    uint16_t         count = 0u;
    cJSON            *arr  = NULL;
    char             *json = NULL;

    do
    {
        if (net_wifi_sta_scan(&recs, &count) != ESP_OK)
        {
            error_code = httpd_resp_send_err(
                req, HTTPD_500_INTERNAL_SERVER_ERROR, "Scan failed");
            break;
        }

        arr = cJSON_CreateArray();

        for (uint16_t i = 0u; i < count; i++)
        {
            if (!recs[i].ssid[0])
            {
                continue;
            }

            cJSON *ap = cJSON_CreateObject();
            cJSON_AddStringToObject(ap, "ssid", (char *)recs[i].ssid);
            cJSON_AddNumberToObject(ap, "rssi", recs[i].rssi);
            cJSON_AddItemToArray(arr, ap);
        }

        json = cJSON_PrintUnformatted(arr);
        httpd_resp_set_type(req, "application/json");
        error_code = httpd_resp_send(req, json, HTTPD_RESP_USE_STRLEN);
    } while (0);

    cJSON_Delete(arr);

    if (json != NULL)
    {
        free(json);
    }

    if (recs != NULL)
    {
        net_wifi_sta_scan_free(recs);
    }

    return error_code;
}

static esp_err_t wifi_prov_h_configure(httpd_req_t *req)
{
    esp_err_t error_code = ESP_OK;
    cJSON     *root = NULL;

    do
    {
        char body[256] = {0};

        if (httpd_req_recv(req, body, sizeof(body) - 1u) <= 0)
        {
            error_code = httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Empty body");
            break;
        }

        root                = cJSON_Parse(body);
        const cJSON *j_ssid = cJSON_GetObjectItemCaseSensitive(root, "ssid");
        const cJSON *j_pass = cJSON_GetObjectItemCaseSensitive(root, "pass");

        if ((!cJSON_IsString(j_ssid)) || (!j_ssid->valuestring[0]))
        {
            error_code = httpd_resp_send_err(
                req, HTTPD_400_BAD_REQUEST, "Missing SSID");
            break;
        }

        strlcpy(
            wifi_prov_ssid, j_ssid->valuestring, sizeof(wifi_prov_ssid));
        strlcpy(
            wifi_prov_pass,
            cJSON_IsString(j_pass) ? j_pass->valuestring : "",
            sizeof(wifi_prov_pass));

        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, "{\"ok\":true}");

        esp_timer_start_once(wifi_prov_cred_timer, WIFI_PROV_HTTP_CRED_DELAY_US);
    } while (0);

    cJSON_Delete(root);

    return error_code;
}

static esp_err_t wifi_prov_h_redirect(httpd_req_t *req)
{
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", "http://" WIFI_PROV_HTTP_AP_IP "/");
    return httpd_resp_send(req, NULL, 0);
}


/* URI table --------------------------------------------------------------- */

static const httpd_uri_t WIFI_PROV_URI_ROOT = {
    .uri     = "/",
    .method  = HTTP_GET,
    .handler = wifi_prov_h_root,
};

static const httpd_uri_t WIFI_PROV_URI_SCAN = {
    .uri     = "/scan",
    .method  = HTTP_GET,
    .handler = wifi_prov_h_scan,
};

static const httpd_uri_t WIFI_PROV_URI_CONFIGURE = {
    .uri     = "/configure",
    .method  = HTTP_POST,
    .handler = wifi_prov_h_configure,
};

static const httpd_uri_t WIFI_PROV_URI_CATCH = {
    .uri     = "/*",
    .method  = HTTP_GET,
    .handler = wifi_prov_h_redirect,
};


/* Exposed API ------------------------------------------------------------- */

esp_err_t wifi_provisioning_http_start(esp_timer_handle_t cred_timer)
{
    esp_err_t error_code = ESP_OK;

    do
    {
        if (wifi_prov_httpd != NULL)
        {
            break;
        }

        wifi_prov_cred_timer = cred_timer;

        httpd_config_t cfg   = HTTPD_DEFAULT_CONFIG();
        cfg.stack_size       = WIFI_PROV_HTTP_TASK_STACK;
        cfg.lru_purge_enable = true;
        cfg.uri_match_fn     = httpd_uri_match_wildcard;

        error_code = httpd_start(&wifi_prov_httpd, &cfg);

        if (error_code != ESP_OK)
        {
            ESP_LOGE(wifi_prov_http_tag, "httpd_start failed: %s",
                     esp_err_to_name(error_code));
            break;
        }

        httpd_register_uri_handler(wifi_prov_httpd, &WIFI_PROV_URI_ROOT);
        httpd_register_uri_handler(wifi_prov_httpd, &WIFI_PROV_URI_SCAN);
        httpd_register_uri_handler(wifi_prov_httpd, &WIFI_PROV_URI_CONFIGURE);
        httpd_register_uri_handler(wifi_prov_httpd, &WIFI_PROV_URI_CATCH);

        ESP_LOGI(wifi_prov_http_tag, "HTTP server up at http://%s",
                 WIFI_PROV_HTTP_AP_IP);
    } while (0);

    return error_code;
}

esp_err_t wifi_provisioning_http_stop(void)
{
    if (wifi_prov_httpd != NULL)
    {
        httpd_stop(wifi_prov_httpd);
        wifi_prov_httpd = NULL;
        ESP_LOGI(wifi_prov_http_tag, "HTTP server stopped");
    }

    wifi_prov_cred_timer = NULL;

    return ESP_OK;
}


/**** END OF FILE ****/
