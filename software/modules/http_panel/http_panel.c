/**
 ******************************************************************************
 * @file    http_panel.c
 * @author  Daniel Dyringer
 * @date    2026-03-09
 * @brief   HTTP panel — thin public façade.
 ******************************************************************************
 */


/* Includes ---------------------------------------------------------------- */
#include "http_panel.h"

#include "camera_page.h"
#include "common_page.h"
#include "core/http_panel_server.h"
#include "dashboard.h"
#include "data_manager_page.h"
#include "esp_log.h"
#include "files_page.h"
#include "http_panel_transfer.h"
#include "network_page.h"
#include "ota_page.h"
#if BOARD_HAS_SMART_IO
#include "smart_io_page.h"
#endif
#include "sysinfo_page.h"
#include "web_console.h"


/* Constants and macros ---------------------------------------------------- */
// clang-format off


// clang-format on

/* Private types ----------------------------------------------------------- */


/* Private variables ------------------------------------------------------- */

static const char* http_panel_tag = "http_panel";


/* Private functions ------------------------------------------------------- */


/* Exposed API ------------------------------------------------------------- */

esp_err_t http_panel_init(void)
{
    esp_err_t error_code = ESP_OK;

    do
    {
        error_code = http_panel_server_init();

        if (error_code != ESP_OK)
        {
            ESP_LOGE(
                http_panel_tag, "Server core init failed: %s", esp_err_to_name(error_code));
            break;
        }

        error_code = http_panel_transfer_init();

        if (error_code != ESP_OK)
        {
            ESP_LOGE(
                http_panel_tag, "Transfer init failed: %s", esp_err_to_name(error_code));
            break;
        }

        error_code = common_page_init();

        if (error_code != ESP_OK)
        {
            ESP_LOGE(http_panel_tag, "Assets init failed: %s", esp_err_to_name(error_code));
            break;
        }

        error_code = dashboard_init();

        if (error_code != ESP_OK)
        {
            ESP_LOGE(
                http_panel_tag, "Dashboard init failed: %s", esp_err_to_name(error_code));
            break;
        }

        error_code = web_console_init();

        if (error_code != ESP_OK)
        {
            ESP_LOGE(
                http_panel_tag, "Web console init failed: %s", esp_err_to_name(error_code));
            break;
        }

        error_code = ota_page_init();

        if (error_code != ESP_OK)
        {
            ESP_LOGE(
                http_panel_tag, "OTA page init failed: %s", esp_err_to_name(error_code));
            break;
        }

        error_code = camera_page_init();

        if (error_code != ESP_OK)
        {
            ESP_LOGE(
                http_panel_tag, "Camera page init failed: %s", esp_err_to_name(error_code));
            break;
        }

        error_code = data_manager_page_init();

        if (error_code != ESP_OK)
        {
            ESP_LOGE(
                http_panel_tag,
                "Data manager page init failed: %s",
                esp_err_to_name(error_code));
            break;
        }

        error_code = network_page_init();

        if (error_code != ESP_OK)
        {
            ESP_LOGE(
                http_panel_tag, "Network page init failed: %s", esp_err_to_name(error_code));
            break;
        }

        error_code = sysinfo_page_init();

        if (error_code != ESP_OK)
        {
            ESP_LOGE(
                http_panel_tag,
                "System info page init failed: %s",
                esp_err_to_name(error_code));
            break;
        }

        error_code = files_page_init();

        if (error_code != ESP_OK)
        {
            ESP_LOGE(
                http_panel_tag, "Files page init failed: %s", esp_err_to_name(error_code));
            break;
        }

#if BOARD_HAS_SMART_IO
        error_code = smart_io_page_init();

        if (error_code != ESP_OK)
        {
            ESP_LOGE(
                http_panel_tag, "IO page init failed: %s", esp_err_to_name(error_code));
            break;
        }
#endif

        error_code = http_panel_server_enable();

        if (error_code != ESP_OK)
        {
            ESP_LOGE(
                http_panel_tag, "Server enable failed: %s", esp_err_to_name(error_code));
            break;
        }

        ESP_LOGI(http_panel_tag, "Initialised");
    } while (0);

    return error_code;
}

/**** END OF FILE ****/
