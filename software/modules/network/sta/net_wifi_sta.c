/**
 ******************************************************************************
 * @file    net_wifi_sta.c
 * @author  Daniel Dyringer
 * @date    2026-02-17
 * @brief   WiFi STA driver — connect, disconnect, scan.
 ******************************************************************************
 */


/* Includes ---------------------------------------------------------------- */
#include "net_wifi_sta.h"

#include <stdlib.h>
#include <string.h>

#include "esp_log.h"
#include "utils.h"
#include "wifi_types.h"


/* Constants and macros ---------------------------------------------------- */
// clang-format off

#define NET_WIFI_STA_SCAN_DWELL_MIN_MS  100u
#define NET_WIFI_STA_SCAN_DWELL_MAX_MS  200u

// clang-format on

/* Private types ----------------------------------------------------------- */


/* Private variables ------------------------------------------------------- */

static const char* net_wifi_sta_tag = "net_wifi_sta";


/* Private functions ------------------------------------------------------- */


/* Exposed API ------------------------------------------------------------- */

esp_err_t net_wifi_sta_init(esp_netif_t** netif)
{
    esp_err_t error_code = ESP_OK;

    do
    {
        if (netif == NULL)
        {
            error_code = ESP_ERR_INVALID_ARG;
            break;
        }

        *netif = esp_netif_create_default_wifi_sta();

        if (*netif == NULL)
        {
            error_code = ESP_FAIL;
            break;
        }

        ESP_LOGI(net_wifi_sta_tag, "STA netif created");
    } while (0);

    return error_code;
}

esp_err_t net_wifi_sta_connect(const char* ssid, const char* password)
{
    esp_err_t error_code = ESP_OK;

    do
    {
        if ((ssid == NULL) || (password == NULL))
        {
            error_code = ESP_ERR_INVALID_ARG;
            break;
        }

        wifi_config_t config = {0};

        /* strlcpy would drop the last byte of a 32-char SSID or 64-hex PSK. */
        memcpy(config.sta.ssid, ssid, MIN(strlen(ssid), sizeof(config.sta.ssid)));
        memcpy(
            config.sta.password,
            password,
            MIN(strlen(password), sizeof(config.sta.password)));

        /* An empty password means an open network; a WPA2 threshold rejects it. */
        config.sta.threshold.authmode =
            (password[0] != '\0') ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;

        error_code = esp_wifi_set_config(WIFI_IF_STA, &config);

        if (error_code != ESP_OK)
        {
            ESP_LOGE(
                net_wifi_sta_tag,
                "esp_wifi_set_config failed: %s",
                esp_err_to_name(error_code));
            break;
        }

        ESP_LOGI(net_wifi_sta_tag, "Connecting to '%s'...", ssid);
        error_code = esp_wifi_connect();
    } while (0);

    return error_code;
}

esp_err_t net_wifi_sta_disconnect(void)
{
    return esp_wifi_disconnect();
}

esp_err_t net_wifi_sta_scan(wifi_ap_record_t** records, uint16_t* count)
{
    esp_err_t error_code         = ESP_OK;
    wifi_ap_record_t* ap_records = NULL;
    uint16_t ap_count            = 0u;

    do
    {
        if ((records == NULL) || (count == NULL))
        {
            error_code = ESP_ERR_INVALID_ARG;
            break;
        }

        *records = NULL;
        *count   = 0u;

        const wifi_scan_config_t scan_config = {
            .scan_type            = WIFI_SCAN_TYPE_ACTIVE,
            .scan_time.active.min = NET_WIFI_STA_SCAN_DWELL_MIN_MS,
            .scan_time.active.max = NET_WIFI_STA_SCAN_DWELL_MAX_MS,
            .channel              = 0u,
            .show_hidden          = false,
        };

        esp_wifi_set_max_tx_power((int8_t)WIFI_TYPES_SCAN_TX_POWER);

        error_code = esp_wifi_scan_start(&scan_config, true);

        if (error_code != ESP_OK)
        {
            ESP_LOGE(
                net_wifi_sta_tag, "Scan start failed: %s", esp_err_to_name(error_code));
            break;
        }

        error_code = esp_wifi_scan_get_ap_num(&ap_count);

        if ((error_code != ESP_OK) || (ap_count == 0u))
        {
            break;
        }

        ap_records = malloc(sizeof(wifi_ap_record_t) * ap_count);

        if (ap_records == NULL)
        {
            /* The driver keeps the result list until it is read or cleared. */
            esp_wifi_clear_ap_list();
            error_code = ESP_ERR_NO_MEM;
            break;
        }

        error_code = esp_wifi_scan_get_ap_records(&ap_count, ap_records);

        if (error_code != ESP_OK)
        {
            free(ap_records);
            ap_records = NULL;
            break;
        }

        *records = ap_records;
        *count   = ap_count;
        ESP_LOGI(net_wifi_sta_tag, "Scan complete: %u APs found", (unsigned)ap_count);
    } while (0);

    return error_code;
}

void net_wifi_sta_scan_free(wifi_ap_record_t* records)
{
    free(records);
}

/**** END OF FILE ****/
