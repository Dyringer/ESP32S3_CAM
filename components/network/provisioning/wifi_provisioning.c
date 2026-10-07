/**
 ******************************************************************************
 * @file    wifi_provisioning.c
 * @author  Daniel Dyringer
 * @date    2026-02-17
 * @brief   Captive portal provisioning — DNS redirect + HTTP config page.
 *
 *          This module owns the DNS server task and the public start/stop API.
 *          All HTTP concerns live in wifi_provisioning_http.c.
 ******************************************************************************
 */


/* Includes ---------------------------------------------------------------- */
#include "wifi_provisioning.h"
#include "wifi_provisioning_http.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "wifi_connection.h"


/* Constants and macros ---------------------------------------------------- */
// clang-format off
#define WIFI_PROVISIONING_DNS_PORT       53u
#define WIFI_PROVISIONING_AP_IP          "192.168.4.1"
#define WIFI_PROVISIONING_DNS_TASK_STACK 3072u
#define WIFI_PROVISIONING_DNS_TASK_PRIO  5u

/** @brief Core for the DNS provisioning task — pinned to the network core. */
#define WIFI_PROVISIONING_DNS_TASK_CORE  0
// clang-format on


/* Private variables ------------------------------------------------------- */

static const char *wifi_provisioning_tag = "wifi_prov";

static TaskHandle_t       wifi_provisioning_dns_task   = NULL;
static volatile bool      wifi_provisioning_dns_run    = false;
static esp_timer_handle_t wifi_provisioning_cred_timer = NULL;


/* Private functions ------------------------------------------------------- */

/**
 * @brief One-shot timer callback: apply provisioned credentials.
 *
 * @note  Calling wifi_provisioning_stop() (-> httpd_stop()) from within an
 *        httpd handler deadlocks.  This callback fires 500 ms after the HTTP
 *        response is sent, safely outside the httpd task context.
 */
static void wifi_provisioning_apply_creds_cb(void *arg)
{
    wifi_connection_set_credentials(wifi_prov_ssid, wifi_prov_pass);
}

/**
 * @brief DNS server task — redirects all UDP/53 queries to the AP IP.
 *
 *        Runs until wifi_provisioning_dns_run is cleared.  Exits by
 *        deleting itself; the socket is always closed before exit.
 */
static void wifi_provisioning_dns_task_fn(void *pv_param)
{
    int sock = -1;

    do
    {
        sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);

        if (sock < 0)
        {
            ESP_LOGE(wifi_provisioning_tag, "DNS socket() failed");
            break;
        }

        const int opt = 1;
        setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

        const struct sockaddr_in bind_addr = {
            .sin_family      = AF_INET,
            .sin_port        = htons(WIFI_PROVISIONING_DNS_PORT),
            .sin_addr.s_addr = htonl(INADDR_ANY),
        };

        if (bind(sock, (const struct sockaddr *)&bind_addr,
                 sizeof(bind_addr)) < 0)
        {
            ESP_LOGE(wifi_provisioning_tag, "DNS bind() failed");
            break;
        }

        uint32_t ap_ip_n = 0u;
        inet_pton(AF_INET, WIFI_PROVISIONING_AP_IP, &ap_ip_n);

        ESP_LOGI(wifi_provisioning_tag, "DNS server up -> all queries -> %s",
                 WIFI_PROVISIONING_AP_IP);

        uint8_t            rx[256] = {0u};
        uint8_t            tx[512] = {0u};
        struct sockaddr_in src     = {0};
        socklen_t          src_len = sizeof(src);

        while (wifi_provisioning_dns_run)
        {
            const struct timeval tv = {.tv_sec = 1, .tv_usec = 0};
            setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

            const int len = recvfrom(
                sock, rx, sizeof(rx), 0, (struct sockaddr *)&src, &src_len);

            if (len < 12)
            {
                continue;
            }

            memcpy(tx, rx, (size_t)len);
            tx[2] = 0x81u;
            tx[3] = 0x80u;
            tx[6] = 0x00u;
            tx[7] = 0x01u;
            tx[8] = tx[9] = tx[10] = tx[11] = 0x00u;

            uint8_t *a = tx + len;
            *a++       = 0xC0u;
            *a++       = 0x0Cu;
            *a++       = 0x00u;
            *a++       = 0x01u;
            *a++       = 0x00u;
            *a++       = 0x01u;
            *a++       = 0x00u;
            *a++       = 0x00u;
            *a++       = 0x00u;
            *a++       = 0x3Cu;
            *a++       = 0x00u;
            *a++       = 0x04u;
            memcpy(a, &ap_ip_n, 4u);
            a += 4u;

            sendto(
                sock, tx, (size_t)(a - tx), 0,
                (const struct sockaddr *)&src, src_len);
        }
    } while (0);

    if (sock >= 0)
    {
        close(sock);
    }

    ESP_LOGI(wifi_provisioning_tag, "DNS server stopped");
    vTaskDelete(NULL);
}


/* Exposed API ------------------------------------------------------------- */

esp_err_t wifi_provisioning_start(void)
{
    esp_err_t error_code = ESP_OK;

    do
    {
        const esp_timer_create_args_t tc = {
            .callback = wifi_provisioning_apply_creds_cb,
            .name     = "prov_cred",
        };

        error_code = esp_timer_create(&tc, &wifi_provisioning_cred_timer);

        if (error_code != ESP_OK)
        {
            ESP_LOGE(wifi_provisioning_tag,
                     "esp_timer_create failed: %s", esp_err_to_name(error_code));
            break;
        }

        wifi_provisioning_dns_run = true;
        xTaskCreatePinnedToCore(
            wifi_provisioning_dns_task_fn,
            "dns_prov",
            WIFI_PROVISIONING_DNS_TASK_STACK,
            NULL,
            WIFI_PROVISIONING_DNS_TASK_PRIO,
            &wifi_provisioning_dns_task,
            WIFI_PROVISIONING_DNS_TASK_CORE);

        error_code = wifi_provisioning_http_start(wifi_provisioning_cred_timer);

        if (error_code != ESP_OK)
        {
            /* HTTP failed — tear down the DNS task we just started */
            wifi_provisioning_dns_run = false;
            wifi_provisioning_dns_task = NULL;
            esp_timer_delete(wifi_provisioning_cred_timer);
            wifi_provisioning_cred_timer = NULL;
            break;
        }

        ESP_LOGI(wifi_provisioning_tag, "Portal up at http://%s",
                 WIFI_PROVISIONING_AP_IP);
    } while (0);

    return error_code;
}

esp_err_t wifi_provisioning_stop(void)
{
    if (wifi_provisioning_cred_timer != NULL)
    {
        esp_timer_stop(wifi_provisioning_cred_timer);
        esp_timer_delete(wifi_provisioning_cred_timer);
        wifi_provisioning_cred_timer = NULL;
    }

    wifi_provisioning_http_stop();

    wifi_provisioning_dns_run = false;

    ESP_LOGI(wifi_provisioning_tag, "Portal stopped");
    return ESP_OK;
}


/**** END OF FILE ****/
