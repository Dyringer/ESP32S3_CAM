/**
 ******************************************************************************
 * @file    net_diag_scan.c
 * @author  Daniel Dyringer
 * @date    2026-10-13
 * @brief   Network diagnostics — WiFi AP scan.
 *
 *          While connected, the radio leaves the home channel for each scanned
 *          channel, so traffic stalls for the ~2 s the scan takes.
 ******************************************************************************
 */


/* Includes ---------------------------------------------------------------- */
#include <string.h>

#include "esp_attr.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "net_diag.h"
#include "net_diag_private.h"
#include "net_wifi_sta.h"
#include "utils.h"


/* Constants and macros ---------------------------------------------------- */
// clang-format off

#define NET_DIAG_SCAN_LOCK_TIMEOUT_MS   100u

// clang-format on

/* Private types ----------------------------------------------------------- */


/* Private variables ------------------------------------------------------- */

static const char* net_diag_scan_tag = "net_diag_scan";

/* Guarded by net_diag_scan_lock */
static SemaphoreHandle_t net_diag_scan_lock = NULL;

/* Guarded by net_diag_scan_lock. PSRAM: never touch from an ISR or while the
 * flash cache is disabled. */
EXT_RAM_BSS_ATTR static net_diag_scan_result_t net_diag_scan_result = {0};


/* Private functions ------------------------------------------------------- */

static void net_diag_scan_store(const wifi_ap_record_t* records, uint16_t found, esp_err_t error)
{
    xSemaphoreTake(net_diag_scan_lock, portMAX_DELAY);

    const uint16_t count = (uint16_t)MIN((uint32_t)found, NET_DIAG_SCAN_MAX_APS);

    for (uint16_t i = 0u; i < count; i++)
    {
        net_diag_ap_t* const ap = &net_diag_scan_result.aps[i];

        strlcpy(ap->ssid, (const char*)records[i].ssid, sizeof(ap->ssid));
        memcpy(ap->bssid, records[i].bssid, sizeof(ap->bssid));
        ap->auth    = records[i].authmode;
        ap->channel = records[i].primary;
        ap->rssi    = records[i].rssi;
    }

    net_diag_scan_result.count = count;
    net_diag_scan_result.found = found;
    net_diag_scan_result.error = error;
    net_diag_scan_result.state =
        (error == ESP_OK) ? NET_DIAG_STATE_DONE : NET_DIAG_STATE_FAILED;

    xSemaphoreGive(net_diag_scan_lock);
}


/* Exposed API ------------------------------------------------------------- */

esp_err_t net_diag_scan_init(void)
{
    esp_err_t error_code = ESP_OK;

    net_diag_scan_lock = xSemaphoreCreateMutex();

    if (net_diag_scan_lock == NULL)
    {
        error_code = ESP_ERR_NO_MEM;
    }

    return error_code;
}

void net_diag_scan_run(void)
{
    wifi_ap_record_t* records = NULL;
    uint16_t found            = 0u;

    const esp_err_t error_code = net_wifi_sta_scan(&records, &found);

    if (error_code != ESP_OK)
    {
        ESP_LOGW(net_diag_scan_tag, "Scan failed: %s", esp_err_to_name(error_code));
    }

    /* The driver returns the list sorted by RSSI, strongest first. */
    net_diag_scan_store(records, (error_code == ESP_OK) ? found : 0u, error_code);
    net_wifi_sta_scan_free(records);
}

esp_err_t net_diag_scan_start(void)
{
    esp_err_t error_code = ESP_OK;

    do
    {
        if (net_diag_scan_lock == NULL)
        {
            error_code = ESP_ERR_INVALID_STATE;
            break;
        }

        xSemaphoreTake(net_diag_scan_lock, portMAX_DELAY);

        if (net_diag_scan_result.state == NET_DIAG_STATE_RUNNING)
        {
            error_code = ESP_ERR_INVALID_STATE;
        }
        else
        {
            const net_diag_job_t job = {.type = NET_DIAG_JOB_SCAN};

            error_code = net_diag_submit(&job);

            if (error_code == ESP_OK)
            {
                net_diag_scan_result.state = NET_DIAG_STATE_RUNNING;
            }
        }

        xSemaphoreGive(net_diag_scan_lock);
    } while (0);

    return error_code;
}

esp_err_t net_diag_scan_get(net_diag_scan_result_t* result)
{
    esp_err_t error_code = ESP_OK;

    if ((result == NULL) || (net_diag_scan_lock == NULL))
    {
        error_code = ESP_ERR_INVALID_STATE;
    }
    else if (xSemaphoreTake(net_diag_scan_lock, pdMS_TO_TICKS(NET_DIAG_SCAN_LOCK_TIMEOUT_MS)) != pdTRUE)
    {
        error_code = ESP_ERR_TIMEOUT;
    }
    else
    {
        *result = net_diag_scan_result;
        xSemaphoreGive(net_diag_scan_lock);
    }

    return error_code;
}

/**** END OF FILE ****/
