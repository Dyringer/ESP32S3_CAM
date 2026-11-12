/**
 ******************************************************************************
 * @file    main.c
 * @author  Daniel Dyringer
 * @date    2026-02-17
 * @brief   Application entry point
 ******************************************************************************
 */


/* Includes ---------------------------------------------------------------- */
#include "app_shell.h"
#include "camera.h"
#include "data_manager.h"
#include "data_manager_shell.h"
#include "esp_err.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "freertos/FreeRTOS.h"
#include "http_panel.h"
#include "i2c_bus.h"
#include "i2c_tools.h"
#include "net_diag.h"
#include "net_diag_shell.h"
#include "nvs_flash.h"
#include "ota.h"
#include "ota_shell.h"
#include "sensor_core.h"
#include "sensor_registry.h"
#include "sensor_shell.h"
#if BOARD_HAS_SMART_IO
#include "smart_io.h"
#include "smart_io_shell.h"
#endif
#include "snapshot_upload.h"
#include "snapshot_upload_shell.h"
#include "storage.h"
#include "storage_shell.h"
#include "sysmon.h"
#include "sysmon_shell.h"
#include "system_shell.h"
#include "uart_console.h"
#include "wifi_connection.h"
#include "wifi_shell.h"
#include "work_queue.h"


/* Constants and macros ---------------------------------------------------- */
// clang-format off


// clang-format on

/* Private types ----------------------------------------------------------- */


/* Private variables ------------------------------------------------------- */

static const char* main_tag = "main";


/* Private functions ------------------------------------------------------- */


/* Exposed API ------------------------------------------------------------- */

void app_main(void)
{
    ESP_LOGI(main_tag, "Booting on Core %d", xPortGetCoreID());

    esp_err_t error_code = nvs_flash_init();

    if ((error_code == ESP_ERR_NVS_NO_FREE_PAGES)
        || (error_code == ESP_ERR_NVS_NEW_VERSION_FOUND))
    {
        ESP_LOGW(main_tag, "NVS partition wiped, re-initializing");
        ESP_ERROR_CHECK(nvs_flash_erase());
        error_code = nvs_flash_init();
    }

    ESP_ERROR_CHECK(error_code);

    error_code = esp_event_loop_create_default();

    if (error_code != ESP_OK)
    {
        ESP_LOGE(
            main_tag,
            "Default event loop unavailable (%s) — WiFi and camera will fail",
            esp_err_to_name(error_code));
    }

    error_code = work_queue_init();

    if (error_code != ESP_OK)
    {
        ESP_LOGE(
            main_tag,
            "Work queue unavailable (%s) — no deferred work",
            esp_err_to_name(error_code));
    }

    error_code = data_manager_init();

    if (error_code != ESP_OK)
    {
        ESP_LOGE(
            main_tag,
            "Data manager unavailable (%s) — settings fall back to defaults",
            esp_err_to_name(error_code));
    }

    error_code = sysmon_init();

    if (error_code != ESP_OK)
    {
        ESP_LOGE(
            main_tag,
            "sysmon unavailable (%s) — no CPU load readout",
            esp_err_to_name(error_code));
    }

    error_code = i2c_bus_init();

    if (error_code != ESP_OK)
    {
        ESP_LOGE(
            main_tag,
            "I2C bus unavailable (%s) — camera and sensors will fail",
            esp_err_to_name(error_code));
    }

    error_code = camera_init();

    if (error_code != ESP_OK)
    {
        ESP_LOGE(
            main_tag,
            "Camera unavailable (%s) — continuing without video",
            esp_err_to_name(error_code));
    }

#if BOARD_HAS_SMART_IO
    error_code = smart_io_init();

    if (error_code != ESP_OK)
    {
        ESP_LOGE(main_tag, "Smart IO unavailable (%s)", esp_err_to_name(error_code));
    }
#endif

    error_code = storage_init();

    if (error_code != ESP_OK)
    {
        ESP_LOGE(
            main_tag,
            "Storage unavailable (%s) — no file manager",
            esp_err_to_name(error_code));
    }

    error_code = wifi_connection_init();

    if (error_code == ESP_OK)
    {
        error_code = wifi_connection_start();
    }

    if (error_code != ESP_OK)
    {
        ESP_LOGE(
            main_tag, "WiFi unavailable (%s) — console only", esp_err_to_name(error_code));
    }

    error_code = ota_init();

    if (error_code != ESP_OK)
    {
        ESP_LOGE(main_tag, "OTA unavailable (%s)", esp_err_to_name(error_code));
    }

    error_code = net_diag_init();

    if (error_code != ESP_OK)
    {
        ESP_LOGE(
            main_tag, "Network diagnostics unavailable (%s)", esp_err_to_name(error_code));
    }

    error_code = http_panel_init();

    if (error_code != ESP_OK)
    {
        ESP_LOGE(main_tag, "Web panel unavailable (%s)", esp_err_to_name(error_code));
    }

    error_code = snapshot_upload_init();

    if (error_code != ESP_OK)
    {
        ESP_LOGE(main_tag, "Snapshot upload unavailable (%s)", esp_err_to_name(error_code));
    }

    error_code = sensor_registry_all(i2c_bus_handle());

    if (error_code == ESP_OK)
    {
        error_code = sensor_core_start();
    }

    if (error_code != ESP_OK)
    {
        ESP_LOGE(main_tag, "Sensors unavailable (%s)", esp_err_to_name(error_code));
    }

    ESP_ERROR_CHECK(uart_console_init());
    ESP_ERROR_CHECK(wifi_shell_register());
    ESP_ERROR_CHECK(ota_shell_register());
    ESP_ERROR_CHECK(net_diag_shell_register());
    ESP_ERROR_CHECK(snapshot_upload_shell_register());
    ESP_ERROR_CHECK(sysmon_shell_register());
    ESP_ERROR_CHECK(sensor_shell_register());
    ESP_ERROR_CHECK(system_shell_register());
    ESP_ERROR_CHECK(data_manager_shell_register());
    ESP_ERROR_CHECK(storage_shell_register());
#if BOARD_HAS_SMART_IO
    ESP_ERROR_CHECK(smart_io_shell_register());
#endif
    ESP_ERROR_CHECK(
        i2c_tools_register(i2c_bus_handle(), sensor_core_pause, sensor_core_resume));
    ESP_ERROR_CHECK(app_shell_init());
    ESP_ERROR_CHECK(uart_console_start());

    ESP_ERROR_CHECK(esp_ota_mark_app_valid_cancel_rollback());

    ESP_LOGI(main_tag, "Init complete — tasks running");
}

/**** END OF FILE ****/
