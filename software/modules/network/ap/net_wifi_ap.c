/**
 ******************************************************************************
 * @file    net_wifi_ap.c
 * @author  Daniel Dyringer
 * @date    2026-02-17
 * @brief   WiFi AP driver — start, stop, DHCP server managed by esp_netif.
 ******************************************************************************
 */


/* Includes ---------------------------------------------------------------- */
#include "net_wifi_ap.h"

#include <string.h>

#include "esp_log.h"
#include "esp_wifi.h"


/* Constants and macros ---------------------------------------------------- */
// clang-format off

#define NET_WIFI_AP_WPA2_PASSWORD_MIN   8u

// clang-format on

/* Private types ----------------------------------------------------------- */


/* Private variables ------------------------------------------------------- */

static const char* net_wifi_ap_tag = "net_wifi_ap";


/* Private functions ------------------------------------------------------- */


/* Exposed API ------------------------------------------------------------- */

esp_err_t net_wifi_ap_init(esp_netif_t** netif)
{
    esp_err_t error_code = ESP_OK;

    do
    {
        if (netif == NULL)
        {
            error_code = ESP_ERR_INVALID_ARG;
            break;
        }

        *netif = esp_netif_create_default_wifi_ap();

        if (*netif == NULL)
        {
            error_code = ESP_FAIL;
            break;
        }

        ESP_LOGI(net_wifi_ap_tag, "AP netif created");
    } while (0);

    return error_code;
}

esp_err_t net_wifi_ap_start(const char* ssid, const char* password)
{
    esp_err_t error_code = ESP_OK;

    do
    {
        if (ssid == NULL)
        {
            error_code = ESP_ERR_INVALID_ARG;
            break;
        }

        wifi_config_t config = {
            .ap =
                {
                    .max_connection = NET_WIFI_AP_MAX_CONNECTIONS,
                    .channel        = NET_WIFI_AP_CHANNEL,
                    .authmode       = WIFI_AUTH_OPEN,
                },
        };

        strlcpy((char*)config.ap.ssid, ssid, sizeof(config.ap.ssid));
        config.ap.ssid_len = (uint8_t)strlen((const char*)config.ap.ssid);

        if ((password != NULL) && (strlen(password) >= NET_WIFI_AP_WPA2_PASSWORD_MIN))
        {
            strlcpy((char*)config.ap.password, password, sizeof(config.ap.password));
            config.ap.authmode = WIFI_AUTH_WPA2_PSK;
        }

        /* STA stays enabled — the portal scans and connects through it. */
        error_code = esp_wifi_set_mode(WIFI_MODE_APSTA);

        if (error_code != ESP_OK)
        {
            break;
        }

        error_code = esp_wifi_set_config(WIFI_IF_AP, &config);

        if (error_code != ESP_OK)
        {
            break;
        }

        ESP_LOGI(
            net_wifi_ap_tag,
            "AP started: SSID='%s' (%s)",
            ssid,
            (config.ap.authmode == WIFI_AUTH_OPEN) ? "open" : "WPA2");
    } while (0);

    if (error_code != ESP_OK)
    {
        ESP_LOGE(net_wifi_ap_tag, "AP start failed: %s", esp_err_to_name(error_code));
    }

    return error_code;
}

esp_err_t net_wifi_ap_stop(void)
{
    wifi_mode_t mode     = WIFI_MODE_NULL;
    esp_err_t error_code = esp_wifi_get_mode(&mode);

    if ((error_code == ESP_OK) && (mode == WIFI_MODE_APSTA))
    {
        error_code = esp_wifi_set_mode(WIFI_MODE_STA);

        if (error_code == ESP_OK)
        {
            ESP_LOGI(net_wifi_ap_tag, "AP stopped");
        }
    }

    if (error_code != ESP_OK)
    {
        ESP_LOGE(net_wifi_ap_tag, "AP stop failed: %s", esp_err_to_name(error_code));
    }

    return error_code;
}

/**** END OF FILE ****/
