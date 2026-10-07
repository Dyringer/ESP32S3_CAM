/**
 ******************************************************************************
 * @file    wifi_ap.c
 * @author  Daniel Dyringer
 * @date    2026-02-17
 * @brief   WiFi AP driver — start, stop, DHCP server managed by esp_netif.
 ******************************************************************************
 */


/* Includes ---------------------------------------------------------------- */
#include "wifi_ap.h"

#include <string.h>

#include "esp_log.h"
#include "esp_wifi.h"


/* Constants and macros ---------------------------------------------------- */
// clang-format off

// clang-format on


/* Private types ----------------------------------------------------------- */


/* Private variables ------------------------------------------------------- */

static const char* wifi_ap_tag = "wifi_ap";


/* Private functions ------------------------------------------------------- */


/* Exposed API ------------------------------------------------------------- */

esp_err_t wifi_ap_init(esp_netif_t** netif_out)
{
    esp_err_t error_code;

    *netif_out = esp_netif_create_default_wifi_ap();

    if (!*netif_out)
    {
        error_code = ESP_FAIL;
    }
    else
    {
        ESP_LOGI(wifi_ap_tag, "AP netif created");
        error_code = ESP_OK;
    }

    return error_code;
}

esp_err_t wifi_ap_start(const char* ssid, const char* password)
{
    wifi_config_t cfg = {
        .ap =
            {
                .max_connection = WIFI_AP_MAX_CONNECTIONS,
                .channel        = WIFI_AP_CHANNEL,
                .authmode       = WIFI_AUTH_OPEN,
            },
    };

    strlcpy((char*)cfg.ap.ssid, ssid, sizeof(cfg.ap.ssid));
    cfg.ap.ssid_len = (uint8_t)strlen(ssid);

    if (password && strlen(password) >= 8)
    {
        strlcpy((char*)cfg.ap.password, password, sizeof(cfg.ap.password));
        cfg.ap.authmode = WIFI_AUTH_WPA2_PSK;
    }

    esp_err_t error_code = esp_wifi_set_config(WIFI_IF_AP, &cfg);

    if (error_code != ESP_OK)
    {
        ESP_LOGE(wifi_ap_tag, "esp_wifi_set_config failed: %s",
                 esp_err_to_name(error_code));
    }
    else
    {
        ESP_LOGI(
            wifi_ap_tag,
            "AP started: SSID='%s' (%s)",
            ssid,
            cfg.ap.authmode == WIFI_AUTH_OPEN ? "open" : "WPA2");
    }

    return error_code;
}

esp_err_t wifi_ap_stop(void)
{
    ESP_LOGI(wifi_ap_tag, "AP stop requested");
    return ESP_OK;
}


/**** END OF FILE ****/