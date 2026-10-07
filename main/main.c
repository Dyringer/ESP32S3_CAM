/**
 ******************************************************************************
 * @file    main.c
 * @author  Daniel Dyringer
 * @date    2026-02-17
 * @brief   Application entry point — module init sequence only.
 *          Core 0: network (net_manager task spawned by wifi_connection_init)
 *          Core 1: console REPL task spawned by uart_console_start
 ******************************************************************************
 */


/* Includes ---------------------------------------------------------------- */
#include "app_shell.h"
#include "camera.h"
#include "camera_shell.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "http_panel.h"
#include "nvs_flash.h"
#include "ota.h"
#include "ota_shell.h"
#include "snapshot_upload.h"
#include "snapshot_upload_shell.h"
#include "sysmon.h"
#include "sysmon_shell.h"
#include "uart_console.h"
#include "wifi_connection.h"
#include "wifi_shell.h"

/* Private variables ------------------------------------------------------- */

static const char *main_tag = "main";


/* Exposed API ------------------------------------------------------------- */

void app_main(void)
{
    ESP_LOGI(main_tag, "Booting on Core %d", xPortGetCoreID());

    /* NVS must come first — WiFi credentials are stored here */
    esp_err_t error_code = nvs_flash_init();

    if (error_code == ESP_ERR_NVS_NO_FREE_PAGES
        || error_code == ESP_ERR_NVS_NEW_VERSION_FOUND)
    {
        ESP_LOGW(main_tag, "NVS partition wiped, re-initializing");
        ESP_ERROR_CHECK(nvs_flash_erase());
        error_code = nvs_flash_init();
    }

    ESP_ERROR_CHECK(error_code);

    /* CPU load sampler — first reading one second from now */
    ESP_ERROR_CHECK(sysmon_init());

    /* Camera — before WiFi, while PSRAM and DMA-capable RAM are unfragmented.
     * Deliberately not fatal: a missing or faulty sensor must not stop the
     * device from booting, or OTA and the console would be unreachable too. */
    if (camera_init() != ESP_OK)
    {
        ESP_LOGE(main_tag, "Camera unavailable — continuing without video");
    }

    /* Core 0 — network (spawns net_manager task pinned to Core 0) */
    ESP_ERROR_CHECK(wifi_connection_init());
    ESP_ERROR_CHECK(wifi_connection_start());
    ESP_ERROR_CHECK(ota_init());
    ESP_ERROR_CHECK(http_panel_init());

    /* Periodic snapshot push to the remote server — idles until WiFi is up */
    ESP_ERROR_CHECK(snapshot_upload_init());

    /* Console — register commands, init shell, then start UART REPL task */
    ESP_ERROR_CHECK(uart_console_init());
    ESP_ERROR_CHECK(wifi_shell_register());
    ESP_ERROR_CHECK(ota_shell_register());
    ESP_ERROR_CHECK(camera_shell_register());
    ESP_ERROR_CHECK(snapshot_upload_shell_register());
    ESP_ERROR_CHECK(sysmon_shell_register());
    ESP_ERROR_CHECK(app_shell_init());
    ESP_ERROR_CHECK(uart_console_start());

    /* Confirm this image is good — cancels the rollback countdown armed by
     * CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE. Placed last, after every other
     * init step has returned ESP_OK, so a crash anywhere above still rolls
     * back to the previous working image. */
    ESP_ERROR_CHECK(esp_ota_mark_app_valid_cancel_rollback());

    ESP_LOGI(main_tag, "Init complete — tasks running");
}


/**** END OF FILE ****/
