/**
 ******************************************************************************
 * @file    smart_io_page.c
 * @author  Daniel Dyringer
 * @date    2026-10-26
 * @brief   IO page (/io): serves the page; the WebSocket lives in smart_io_ws.
 ******************************************************************************
 */


/* Includes ---------------------------------------------------------------- */
#include "smart_io_page.h"

#include <stdint.h>

#include "esp_http_server.h"
#include "esp_log.h"
#include "http_panel_server.h"
#include "smart_io_ws.h"


/* Constants and macros ---------------------------------------------------- */
// clang-format off

#define SMART_IO_PAGE_URI   "/io"

// clang-format on

/* Private types ----------------------------------------------------------- */


/* Private variables ------------------------------------------------------- */

static const char* smart_io_page_tag = "smart_io_page";

extern const uint8_t smart_io_page_html_start[] asm("_binary_smart_io_page_html_start");
extern const uint8_t smart_io_page_html_end[] asm("_binary_smart_io_page_html_end");


/* Private functions ------------------------------------------------------- */

static esp_err_t smart_io_page_h_root(httpd_req_t* request)
{
    const size_t length = (size_t)(smart_io_page_html_end - smart_io_page_html_start);

    httpd_resp_set_type(request, "text/html");

    return httpd_resp_send(request, (const char*)smart_io_page_html_start, (ssize_t)length);
}


/* Exposed API ------------------------------------------------------------- */

esp_err_t smart_io_page_init(void)
{
    esp_err_t error_code = ESP_OK;

    /* Static: http_panel_server keeps the URI by pointer. */
    static const httpd_uri_t smart_io_page_uri = {
        .uri     = SMART_IO_PAGE_URI,
        .method  = HTTP_GET,
        .handler = smart_io_page_h_root,
    };

    do
    {
        error_code = http_panel_server_register_uri(&smart_io_page_uri);

        if (error_code != ESP_OK)
        {
            ESP_LOGE(
                smart_io_page_tag, "URI register failed: %s", esp_err_to_name(error_code));
            break;
        }

        error_code = smart_io_ws_init();

        if (error_code != ESP_OK)
        {
            ESP_LOGE(
                smart_io_page_tag, "WebSocket init failed: %s", esp_err_to_name(error_code));
            break;
        }

        ESP_LOGI(smart_io_page_tag, "Ready at %s", SMART_IO_PAGE_URI);
    } while (0);

    return error_code;
}

/**** END OF FILE ****/
