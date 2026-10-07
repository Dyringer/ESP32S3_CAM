/**
 ******************************************************************************
 * @file    http_panel_providers.c
 * @author  Daniel Dyringer
 * @date    2026-03-09
 * @brief   HTTP panel providers — formatted device values and actions
 *          shared by the panel pages.
 ******************************************************************************
 */


/* Includes ---------------------------------------------------------------- */
#include "http_panel_providers.h"

#include <stdio.h>
#include <string.h>

#include "bsp.h"
#include "camera.h"
#include "esp_app_desc.h"
#include "esp_chip_info.h"
#include "esp_flash.h"
#include "esp_heap_caps.h"
#include "esp_idf_version.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "ota.h"
#include "sysmon.h"
#include "wifi_connection.h"


/* Constants and macros ---------------------------------------------------- */
// clang-format off

#define HP_EM_DASH  "\xe2\x80\x94"

// clang-format on


/* Private functions ------------------------------------------------------- */

static const char *hp_chip_model_name(esp_chip_model_t model)
{
    switch (model)
    {
    case CHIP_ESP32:   return "ESP32";
    case CHIP_ESP32S2: return "ESP32-S2";
    case CHIP_ESP32S3: return "ESP32-S3";
    case CHIP_ESP32C3: return "ESP32-C3";
    default:           return "unknown";
    }
}


/* Exposed API ------------------------------------------------------------- */

/* ---- Runtime status ----------------------------------------------------- */

void hp_get_uptime(char *buf, size_t len)
{
    const int64_t  us  = esp_timer_get_time();
    const uint32_t sec = (uint32_t)(us / 1000000LL);
    const uint32_t h   = sec / 3600u;
    const uint32_t m   = (sec % 3600u) / 60u;
    const uint32_t s   = sec % 60u;

    snprintf(buf, len, "%luh %02lum %02lus",
             (unsigned long)h, (unsigned long)m, (unsigned long)s);
}

void hp_get_heap(char *buf, size_t len)
{
    snprintf(buf, len, "%u B",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
}

void hp_get_heap_min(char *buf, size_t len)
{
    snprintf(buf, len, "%lu B", (unsigned long)esp_get_minimum_free_heap_size());
}

void hp_get_psram(char *buf, size_t len)
{
    const size_t total = heap_caps_get_total_size(MALLOC_CAP_SPIRAM);

    if (total == 0u)
    {
        snprintf(buf, len, "none");
    }
    else
    {
        snprintf(buf, len, "%u / %u KB",
                 (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024u),
                 (unsigned)(total / 1024u));
    }
}

void hp_add_cpu_load(cJSON *obj)
{
    cJSON *cpu = cJSON_AddArrayToObject(obj, "cpu");

    for (int core = 0; core < SYSMON_CORE_COUNT; core++)
    {
        cJSON_AddItemToArray(cpu, cJSON_CreateNumber(sysmon_cpu_load(core)));
    }
}

void hp_get_wifi_state(char *buf, size_t len)
{
    strlcpy(buf, wifi_connection_state_str(wifi_connection_get_state()), len);
}

void hp_get_ssid(char *buf, size_t len)
{
    wifi_connection_get_ssid(buf, len);
}

void hp_get_rssi(char *buf, size_t len)
{
    wifi_ap_record_t ap = {0};

    if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK)
    {
        snprintf(buf, len, "%d dBm", (int)ap.rssi);
    }
    else
    {
        snprintf(buf, len, HP_EM_DASH);
    }
}

void hp_get_ip(char *buf, size_t len)
{
    const esp_netif_t *ni =
        esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");

    if (ni)
    {
        esp_netif_ip_info_t ip;
        esp_netif_get_ip_info((esp_netif_t *)ni, &ip);
        snprintf(buf, len, IPSTR, IP2STR(&ip.ip));
    }
    else
    {
        snprintf(buf, len, HP_EM_DASH);
    }
}

/* ---- Device configuration ----------------------------------------------- */

void hp_get_board(char *buf, size_t len)
{
    strlcpy(buf, BOARD_NAME, len);
}

void hp_get_chip(char *buf, size_t len)
{
    esp_chip_info_t info;
    esp_chip_info(&info);

    snprintf(buf, len, "%s rev %u.%u, %u core(s)",
             hp_chip_model_name(info.model),
             (unsigned)(info.revision / 100u),
             (unsigned)(info.revision % 100u),
             (unsigned)info.cores);
}

void hp_get_flash_size(char *buf, size_t len)
{
    uint32_t size = 0u;

    if (esp_flash_get_size(NULL, &size) == ESP_OK)
    {
        snprintf(buf, len, "%lu MB", (unsigned long)(size / (1024u * 1024u)));
    }
    else
    {
        snprintf(buf, len, HP_EM_DASH);
    }
}

void hp_get_mac(char *buf, size_t len)
{
    uint8_t mac[6] = {0};

    if (esp_read_mac(mac, ESP_MAC_WIFI_STA) == ESP_OK)
    {
        snprintf(buf, len, MACSTR, MAC2STR(mac));
    }
    else
    {
        snprintf(buf, len, HP_EM_DASH);
    }
}

void hp_get_idf_version(char *buf, size_t len)
{
    strlcpy(buf, esp_get_idf_version(), len);
}

void hp_get_build_date(char *buf, size_t len)
{
    const esp_app_desc_t *desc = esp_app_get_description();
    snprintf(buf, len, "%s %s", desc->date, desc->time);
}

void hp_get_partition(char *buf, size_t len)
{
    const esp_partition_t *running = esp_ota_get_running_partition();
    strlcpy(buf, running ? running->label : "?", len);
}

void hp_get_camera(char *buf, size_t len)
{
    strlcpy(buf, camera_sensor_name(), len);
}

/* ---- System actions ----------------------------------------------------- */

void hp_action_reboot(void)
{
    esp_restart();
}

void hp_action_factory_reset(void)
{
    wifi_connection_forget(); /* does not return */
}

/* ---- OTA ------------------------------------------------------------------ */

void hp_get_ota_version(char *buf, size_t len)
{
    strlcpy(buf, ota_get_running_version(), len);
}

void hp_action_ota_reboot(void)
{
    ota_status_t status;
    ota_get_status(&status);

    /* Only reboot for the case this button exists for — a validated update
     * staged and waiting. Rebooting unconditionally would let a stray/late
     * WS "reboot" message interrupt an upload still in RECEIVING, or restart
     * the device for no reason at all while idle. */
    if (status.state == OTA_STATE_PENDING_REBOOT)
    {
        esp_restart();
    }
}


/**** END OF FILE ****/
