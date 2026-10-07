/**
 ******************************************************************************
 * @file    wifi_sta.c
 * @author  Daniel Dyringer
 * @date    2026-02-17
 * @brief   WiFi STA driver — connect, disconnect, scan.
 ******************************************************************************
 */


/* Includes ---------------------------------------------------------------- */
#include "wifi_sta.h"

#include <stdlib.h>
#include <string.h>
#include "wifi_types.h"
#include "esp_log.h"


/* Constants and macros ---------------------------------------------------- */
// clang-format off
#define WIFI_STA_SCAN_DWELL_MIN_MS  100u   /* ms — min dwell per channel     */
#define WIFI_STA_SCAN_DWELL_MAX_MS  200u   /* ms — max dwell per channel     */
// clang-format on


/* Private types ----------------------------------------------------------- */


/* Private variables ------------------------------------------------------- */

static const char* wifi_sta_tag = "wifi_sta";


/* Private functions ------------------------------------------------------- */


/* Exposed API ------------------------------------------------------------- */

esp_err_t net_wifi_sta_init(esp_netif_t** netif_out)
{
    esp_err_t error_code = ESP_OK;

    do
    {
        *netif_out = esp_netif_create_default_wifi_sta();

        if (!*netif_out)
        {
            error_code = ESP_FAIL;
            break;
        }

        ESP_LOGI(wifi_sta_tag, "STA netif created");
    } while (0);

    return error_code;
}

esp_err_t net_wifi_sta_connect(const char* ssid, const char* password)
{
    wifi_config_t cfg = {0};

    strlcpy((char*)cfg.sta.ssid, ssid, sizeof(cfg.sta.ssid));
    strlcpy((char*)cfg.sta.password, password, sizeof(cfg.sta.password));
    cfg.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;

    esp_err_t error_code = esp_wifi_set_config(WIFI_IF_STA, &cfg);

    if (error_code != ESP_OK)
    {
        ESP_LOGE(wifi_sta_tag, "esp_wifi_set_config failed: %s",
                 esp_err_to_name(error_code));
    }
    else
    {
        ESP_LOGI(wifi_sta_tag, "Connecting to '%s'...", ssid);
        error_code = esp_wifi_connect();
    }

    return error_code;
}

esp_err_t net_wifi_sta_disconnect(void)
{
    return esp_wifi_disconnect();
}

esp_err_t net_wifi_sta_scan(wifi_ap_record_t** out_records, uint16_t* out_count)
{
    esp_err_t error_code      = ESP_OK;
    wifi_ap_record_t* records = NULL;
    uint16_t count            = 0u;

    *out_records = NULL;
    *out_count   = 0u;

    const wifi_scan_config_t scan_cfg = {
        .scan_type            = WIFI_SCAN_TYPE_ACTIVE,
        .scan_time.active.min = WIFI_STA_SCAN_DWELL_MIN_MS,
        .scan_time.active.max = WIFI_STA_SCAN_DWELL_MAX_MS,
        .channel              = 0u,
        .show_hidden          = false,
    };

    esp_wifi_set_max_tx_power((int8_t)WIFI_MAX_POWER);

    do
    {
        error_code = esp_wifi_scan_start(&scan_cfg, true);

        if (error_code != ESP_OK)
        {
            ESP_LOGE(wifi_sta_tag, "Scan start failed: %s", esp_err_to_name(error_code));
            break;
        }

        esp_wifi_scan_get_ap_num(&count);

        if (count == 0u)
        {
            break;
        }

        records = malloc(sizeof(wifi_ap_record_t) * count);

        if (!records)
        {
            error_code = ESP_ERR_NO_MEM;
            break;
        }

        error_code = esp_wifi_scan_get_ap_records(&count, records);

        if (error_code != ESP_OK)
        {
            free(records);
            records = NULL;
            break;
        }

        *out_records = records;
        *out_count   = count;
        ESP_LOGI(wifi_sta_tag, "Scan complete: %d APs found", (int)count);
    } while (0);

    return error_code;
}

void net_wifi_sta_scan_free(wifi_ap_record_t* records)
{
    free(records);
}

/**** END OF FILE ****/
