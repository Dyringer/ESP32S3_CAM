/**
 ******************************************************************************
 * @file    net_diag.c
 * @author  Daniel Dyringer
 * @date    2026-10-13
 * @brief   Network diagnostics — worker task, job queue and link details.
 ******************************************************************************
 */


/* Includes ---------------------------------------------------------------- */
#include "net_diag.h"

#include <string.h>

#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "net_diag_private.h"
#include "sdkconfig.h"
#include "utils.h"


/* Constants and macros ---------------------------------------------------- */
// clang-format off

/* Stack covers getaddrinfo() for ping and esp_wifi_scan_get_ap_records(). */
#define NET_DIAG_TASK_STACK     4096u
#define NET_DIAG_TASK_PRIO      3u
#define NET_DIAG_TASK_CORE      0
#define NET_DIAG_QUEUE_LEN      4u

// clang-format on

/* Private types ----------------------------------------------------------- */


/* Private variables ------------------------------------------------------- */

static const char* net_diag_tag = "net_diag";

static QueueHandle_t net_diag_queue = NULL;


/* Private functions ------------------------------------------------------- */

static void net_diag_run(const net_diag_job_t* job)
{
    switch (job->type)
    {
        case NET_DIAG_JOB_SCAN:
        {
            net_diag_scan_run();
        }
        break;
        case NET_DIAG_JOB_PING:
        {
            net_diag_ping_run(&job->config.ping);
        }
        break;
        case NET_DIAG_JOB_PING_CLEANUP:
        {
            net_diag_ping_cleanup();
        }
        break;
        case NET_DIAG_JOB_IPERF_START:
        {
            net_diag_iperf_run_start(&job->config.iperf);
        }
        break;
        case NET_DIAG_JOB_IPERF_STOP:
        {
            net_diag_iperf_run_stop();
        }
        break;
        default:
        {
            ESP_LOGW(net_diag_tag, "Unknown job %d", (int)job->type);
        }
        break;
    }
}

static void net_diag_task(void* arg)
{
    ARG_UNUSED(arg);

    net_diag_job_t job = {0};

    for (;;)
    {
        if (xQueueReceive(net_diag_queue, &job, portMAX_DELAY) == pdTRUE)
        {
            net_diag_run(&job);
        }
    }
}

static void net_diag_link_add_wifi(net_diag_link_t* link)
{
    wifi_ap_record_t ap = {0};

    link->associated = (esp_wifi_sta_get_ap_info(&ap) == ESP_OK);

    if (link->associated)
    {
        strlcpy(link->ssid, (const char*)ap.ssid, sizeof(link->ssid));
        memcpy(link->bssid, ap.bssid, sizeof(link->bssid));
        link->channel        = ap.primary;
        link->second_channel = ap.second;
        link->rssi           = ap.rssi;
        link->auth           = ap.authmode;
        link->phy_11b        = (ap.phy_11b != 0u);
        link->phy_11g        = (ap.phy_11g != 0u);
        link->phy_11n        = (ap.phy_11n != 0u);
        link->phy_11ax       = (ap.phy_11ax != 0u);
    }

    esp_wifi_get_mac(WIFI_IF_STA, link->mac);
    esp_wifi_get_max_tx_power(&link->tx_power_quarter_dbm);
    esp_wifi_get_ps(&link->power_save);
}

static void net_diag_link_add_netif(net_diag_link_t* link)
{
    esp_netif_t* const netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");

    if (netif != NULL)
    {
        esp_netif_get_ip_info(netif, &link->ip_info);

        esp_netif_dns_info_t dns = {0};

        if (esp_netif_get_dns_info(netif, ESP_NETIF_DNS_MAIN, &dns) == ESP_OK)
        {
            link->dns[0] = dns.ip;
        }

        if (esp_netif_get_dns_info(netif, ESP_NETIF_DNS_BACKUP, &dns) == ESP_OK)
        {
            link->dns[1] = dns.ip;
        }

        const char* hostname = NULL;

        if ((esp_netif_get_hostname(netif, &hostname) == ESP_OK) && (hostname != NULL))
        {
            strlcpy(link->hostname, hostname, sizeof(link->hostname));
        }

        esp_ip6_addr_t ip6[CONFIG_LWIP_IPV6_NUM_ADDRESSES] = {0};
        const int count = esp_netif_get_all_ip6(netif, ip6);

        for (int i = 0; (i < count) && (link->ip6_count < NET_DIAG_IP6_MAX); i++)
        {
            link->ip6[link->ip6_count] = ip6[i];
            link->ip6_count++;
        }
    }
}


/* Exposed API ------------------------------------------------------------- */

esp_err_t net_diag_init(void)
{
    esp_err_t error_code = ESP_OK;

    do
    {
        net_diag_queue = xQueueCreate(NET_DIAG_QUEUE_LEN, sizeof(net_diag_job_t));

        if (net_diag_queue == NULL)
        {
            error_code = ESP_ERR_NO_MEM;
            break;
        }

        error_code = net_diag_scan_init();

        if (error_code != ESP_OK)
        {
            break;
        }

        error_code = net_diag_ping_init();

        if (error_code != ESP_OK)
        {
            break;
        }

        error_code = net_diag_iperf_init();

        if (error_code != ESP_OK)
        {
            break;
        }

        const BaseType_t created = xTaskCreatePinnedToCore(
            net_diag_task,
            "net_diag",
            NET_DIAG_TASK_STACK,
            NULL,
            NET_DIAG_TASK_PRIO,
            NULL,
            NET_DIAG_TASK_CORE);

        if (created != pdPASS)
        {
            error_code = ESP_ERR_NO_MEM;
        }
    } while (0);

    if (error_code != ESP_OK)
    {
        ESP_LOGE(net_diag_tag, "Init failed: %s", esp_err_to_name(error_code));
    }

    return error_code;
}

esp_err_t net_diag_submit(const net_diag_job_t* job)
{
    esp_err_t error_code = ESP_OK;

    if (net_diag_queue == NULL)
    {
        error_code = ESP_ERR_INVALID_STATE;
    }
    else if (xQueueSend(net_diag_queue, job, 0) != pdTRUE)
    {
        error_code = ESP_ERR_NO_MEM;
    }
    else
    {
        /* Queued. */
    }

    return error_code;
}

esp_err_t net_diag_link_get(net_diag_link_t* link)
{
    esp_err_t error_code = ESP_OK;

    if (link == NULL)
    {
        error_code = ESP_ERR_INVALID_ARG;
    }
    else
    {
        memset(link, 0, sizeof(*link));
        net_diag_link_add_wifi(link);
        net_diag_link_add_netif(link);
    }

    return error_code;
}

const char* net_diag_auth_name(wifi_auth_mode_t auth)
{
    const char* name = NULL;

    switch (auth)
    {
        case WIFI_AUTH_OPEN:
        {
            name = "open";
        }
        break;
        case WIFI_AUTH_WEP:
        {
            name = "WEP";
        }
        break;
        case WIFI_AUTH_WPA_PSK:
        {
            name = "WPA";
        }
        break;
        case WIFI_AUTH_WPA2_PSK:
        {
            name = "WPA2";
        }
        break;
        case WIFI_AUTH_WPA_WPA2_PSK:
        {
            name = "WPA/WPA2";
        }
        break;
        case WIFI_AUTH_WPA2_ENTERPRISE:
        {
            name = "WPA2-Enterprise";
        }
        break;
        case WIFI_AUTH_WPA3_PSK:
        {
            name = "WPA3";
        }
        break;
        case WIFI_AUTH_WPA2_WPA3_PSK:
        {
            name = "WPA2/WPA3";
        }
        break;
        case WIFI_AUTH_WPA3_ENTERPRISE:
        {
            name = "WPA3-Enterprise";
        }
        break;
        case WIFI_AUTH_OWE:
        {
            name = "OWE";
        }
        break;
        default:
        {
            name = "other";
        }
        break;
    }

    return name;
}

const char* net_diag_state_name(net_diag_state_t state)
{
    const char* name = NULL;

    switch (state)
    {
        case NET_DIAG_STATE_IDLE:
        {
            name = "idle";
        }
        break;
        case NET_DIAG_STATE_RUNNING:
        {
            name = "running";
        }
        break;
        case NET_DIAG_STATE_DONE:
        {
            name = "done";
        }
        break;
        case NET_DIAG_STATE_FAILED:
        {
            name = "failed";
        }
        break;
        default:
        {
            name = "unknown";
        }
        break;
    }

    return name;
}

/**** END OF FILE ****/
