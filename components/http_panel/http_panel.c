/**
 ******************************************************************************
 * @file    http_panel.c
 * @author  Daniel Dyringer
 * @date    2026-03-09
 * @brief   HTTP panel — thin public façade.
 *          Initialises the server core and each page module in order.
 ******************************************************************************
 */


/* Includes ---------------------------------------------------------------- */
#include "http_panel.h"

#include "camera_page.h"
#include "core/http_panel_server.h"
#include "dashboard.h"
#include "esp_log.h"
#include "ota_page.h"
#include "web_console.h"


/* Private variables ------------------------------------------------------- */

static const char *http_panel_tag = "http_panel";


/* Exposed API ------------------------------------------------------------- */

esp_err_t http_panel_init(void)
{
    esp_err_t error_code = ESP_OK;

    do
    {
        error_code = http_panel_server_init();

        if (error_code != ESP_OK)
        {
            ESP_LOGE(http_panel_tag, "Server core init failed: %s",
                     esp_err_to_name(error_code));
            break;
        }

        error_code = dashboard_init();

        if (error_code != ESP_OK)
        {
            ESP_LOGE(http_panel_tag, "Dashboard init failed: %s",
                     esp_err_to_name(error_code));
            break;
        }

        error_code = web_console_init();

        if (error_code != ESP_OK)
        {
            ESP_LOGE(http_panel_tag, "Web console init failed: %s",
                     esp_err_to_name(error_code));
            break;
        }

        error_code = ota_page_init();

        if (error_code != ESP_OK)
        {
            ESP_LOGE(http_panel_tag, "OTA page init failed: %s",
                     esp_err_to_name(error_code));
            break;
        }

        error_code = camera_page_init();

        if (error_code != ESP_OK)
        {
            ESP_LOGE(http_panel_tag, "Camera page init failed: %s",
                     esp_err_to_name(error_code));
            break;
        }

        ESP_LOGI(http_panel_tag, "Initialised");
    } while (0);

    return error_code;
}


/**** END OF FILE ****/
