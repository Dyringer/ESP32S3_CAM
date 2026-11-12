/**
 ******************************************************************************
 * @file    common_page.c
 * @author  Daniel Dyringer
 * @date    2026-10-13
 * @brief   Shared panel assets — common stylesheet, header and tab bar.
 ******************************************************************************
 */


/* Includes ---------------------------------------------------------------- */
#include "common_page.h"

#include <stdint.h>

#include "esp_http_server.h"
#include "http_panel_server.h"
#include "utils.h"


/* Constants and macros ---------------------------------------------------- */
// clang-format off


// clang-format on

/* Private types ----------------------------------------------------------- */


/* Private variables ------------------------------------------------------- */

extern const uint8_t common_page_css_start[] asm("_binary_panel_css_start");
extern const uint8_t common_page_css_end[] asm("_binary_panel_css_end");
extern const uint8_t common_page_js_start[] asm("_binary_panel_js_start");
extern const uint8_t common_page_js_end[] asm("_binary_panel_js_end");


/* Private functions ------------------------------------------------------- */

/* no-cache: no ETag is sent, but a stale copy must not outlive an OTA update. */
static esp_err_t common_page_send(
    httpd_req_t* request,
    const char* type,
    const uint8_t* start,
    const uint8_t* end)
{
    httpd_resp_set_type(request, type);
    httpd_resp_set_hdr(request, "Cache-Control", "no-cache");

    return httpd_resp_send(request, (const char*)start, (ssize_t)(end - start));
}

static esp_err_t common_page_h_css(httpd_req_t* request)
{
    return common_page_send(
        request, "text/css", common_page_css_start, common_page_css_end);
}

static esp_err_t common_page_h_js(httpd_req_t* request)
{
    return common_page_send(
        request, "application/javascript", common_page_js_start, common_page_js_end);
}


/* Exposed API ------------------------------------------------------------- */

esp_err_t common_page_init(void)
{
    esp_err_t error_code = ESP_OK;

    /* Static: http_panel_server keeps the URIs by pointer. */
    static const httpd_uri_t common_page_uris[] = {
        {
            .uri     = "/panel.css",
            .method  = HTTP_GET,
            .handler = common_page_h_css,
        },
        {
            .uri     = "/panel.js",
            .method  = HTTP_GET,
            .handler = common_page_h_js,
        },
    };

    for (size_t i = 0u; (error_code == ESP_OK) && (i < ARRAY_SIZE(common_page_uris)); i++)
    {
        error_code = http_panel_server_register_uri(&common_page_uris[i]);
    }

    return error_code;
}

/**** END OF FILE ****/
