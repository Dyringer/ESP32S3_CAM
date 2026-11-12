/**
 ******************************************************************************
 * @file    wifi_provisioning.c
 * @author  Daniel Dyringer
 * @date    2026-02-17
 * @brief   Captive portal provisioning — DNS redirect + HTTP config page.
 ******************************************************************************
 */


/* Includes ---------------------------------------------------------------- */
#include "wifi_provisioning.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <stdbool.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "utils.h"
#include "wifi_provisioning_http.h"


/* Constants and macros ---------------------------------------------------- */
// clang-format off

#define WIFI_PROVISIONING_AP_IP             "192.168.4.1"

#define WIFI_PROVISIONING_DNS_PORT          53u
#define WIFI_PROVISIONING_DNS_HEADER_LEN    12u
#define WIFI_PROVISIONING_DNS_QUERY_MAX     256u
#define WIFI_PROVISIONING_DNS_ANSWER_LEN    16u
#define WIFI_PROVISIONING_DNS_TIMEOUT_S     1

#define WIFI_PROVISIONING_DNS_TASK_STACK    3072u
#define WIFI_PROVISIONING_DNS_TASK_PRIO     5u
#define WIFI_PROVISIONING_DNS_TASK_CORE     0

// clang-format on

/* Private types ----------------------------------------------------------- */


/* Private variables ------------------------------------------------------- */

static const char* wifi_provisioning_tag = "wifi_prov";

static volatile bool wifi_provisioning_dns_run = false;

// clang-format off

/* Overwrites bytes 2-11 of the query to turn it into the response in place
 * (RFC 1035 4.1.1). Bytes 0-1 (query ID) stay as received so the client can
 * match it. */
static const uint8_t wifi_provisioning_dns_flags_and_counts[] = {
    0x81u,          /* QR=1 response, opcode 0, RD=1 echoed     */
    0x80u,          /* RA=1, RCODE=0 no error                   */
    0x00u, 0x01u,   /* QDCOUNT = 1: the question echoed back    */
    0x00u, 0x01u,   /* ANCOUNT = 1: the A record appended below */
    0x00u, 0x00u,   /* NSCOUNT = 0                              */
    0x00u, 0x00u,   /* ARCOUNT = 0                              */
};

/* One A record (RFC 1035 4.1.3) resolving every name to the AP, so any URL
 * the phone opens lands on the portal. RDATA, the AP address, follows it. */
static const uint8_t wifi_provisioning_dns_answer[] = {
    0xC0u, 0x0Cu,               /* NAME: pointer to offset 12 (the question) */
    0x00u, 0x01u,               /* TYPE  = 1 (A, IPv4 address)               */
    0x00u, 0x01u,               /* CLASS = 1 (IN, internet)                  */
    0x00u, 0x00u, 0x00u, 0x3Cu, /* TTL   = 60 s, clients re-ask after setup  */
    0x00u, 0x04u,               /* RDLENGTH = 4 bytes                        */
};

// clang-format on

_Static_assert(
    (sizeof(wifi_provisioning_dns_answer) + sizeof(uint32_t))
        == WIFI_PROVISIONING_DNS_ANSWER_LEN,
    "DNS reply buffer is sized for exactly one A record");


/* Private functions ------------------------------------------------------- */

/* Returns the reply length, or 0 if the query cannot be answered. */
static size_t wifi_provisioning_dns_build_reply(
    uint8_t* message,
    size_t query_length,
    uint32_t ap_ip_network_order)
{
    size_t reply_length = 0u;

    /* Header + first question only: an EDNS OPT record echoed from the
     * additional section ahead of the answer gets parsed as the answer, and
     * clients see no A record. */
    size_t question_end = WIFI_PROVISIONING_DNS_HEADER_LEN;

    /* QNAME is a list of length-prefixed labels ending in a zero byte,
     * e.g. 7"example"3"com"0. A length with the top two bits set is a
     * compression pointer, which never appears in a query — the check below
     * rejects it. */
    while ((question_end < query_length) && (message[question_end] != 0u)
           && ((message[question_end] & 0xC0u) == 0u))
    {
        question_end += (size_t)message[question_end] + 1u;
    }

    question_end += 1u + 4u; /* root label, QTYPE, QCLASS */

    if ((question_end <= query_length) && (message[question_end - 5u] == 0u))
    {
        uint8_t* const answer = &message[question_end];

        memcpy(
            &message[2],
            wifi_provisioning_dns_flags_and_counts,
            sizeof(wifi_provisioning_dns_flags_and_counts));
        memcpy(answer, wifi_provisioning_dns_answer, sizeof(wifi_provisioning_dns_answer));
        memcpy(
            &answer[sizeof(wifi_provisioning_dns_answer)],
            &ap_ip_network_order,
            sizeof(ap_ip_network_order));

        reply_length = question_end + WIFI_PROVISIONING_DNS_ANSWER_LEN;
    }

    return reply_length;
}

static void wifi_provisioning_dns_serve(int32_t socket_fd)
{
    uint32_t ap_ip_network_order = 0u;

    inet_pton(AF_INET, WIFI_PROVISIONING_AP_IP, &ap_ip_network_order);

    ESP_LOGI(
        wifi_provisioning_tag, "DNS server up -> all queries -> %s", WIFI_PROVISIONING_AP_IP);

    /* The reply is built in place: query, then the answer appended. */
    uint8_t message[WIFI_PROVISIONING_DNS_QUERY_MAX + WIFI_PROVISIONING_DNS_ANSWER_LEN] = {
        0u};

    while (wifi_provisioning_dns_run)
    {
        struct sockaddr_in source = {0};
        socklen_t source_length   = sizeof(source);

        const int32_t length = (int32_t)recvfrom(
            socket_fd,
            message,
            WIFI_PROVISIONING_DNS_QUERY_MAX,
            0,
            (struct sockaddr*)&source,
            &source_length);

        if (length >= (int32_t)WIFI_PROVISIONING_DNS_HEADER_LEN)
        {
            const size_t reply_length = wifi_provisioning_dns_build_reply(
                message, (size_t)length, ap_ip_network_order);

            if (reply_length > 0u)
            {
                sendto(
                    socket_fd,
                    message,
                    reply_length,
                    0,
                    (const struct sockaddr*)&source,
                    source_length);
            }
        }
    }
}

static void wifi_provisioning_dns_task(void* arg)
{
    ARG_UNUSED(arg);

    int32_t socket_fd = -1;

    do
    {
        socket_fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);

        if (socket_fd < 0)
        {
            ESP_LOGE(wifi_provisioning_tag, "DNS socket() failed");
            break;
        }

        const int32_t reuse_address = 1;

        setsockopt(
            socket_fd, SOL_SOCKET, SO_REUSEADDR, &reuse_address, sizeof(reuse_address));

        /* Bounds each recvfrom() so the task notices wifi_provisioning_stop(). */
        const struct timeval timeout = {
            .tv_sec  = WIFI_PROVISIONING_DNS_TIMEOUT_S,
            .tv_usec = 0,
        };

        setsockopt(socket_fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));

        const struct sockaddr_in bind_address = {
            .sin_family      = AF_INET,
            .sin_port        = htons(WIFI_PROVISIONING_DNS_PORT),
            .sin_addr.s_addr = htonl(INADDR_ANY),
        };

        if (bind(socket_fd, (const struct sockaddr*)&bind_address, sizeof(bind_address)) < 0)
        {
            ESP_LOGE(wifi_provisioning_tag, "DNS bind() failed");
            break;
        }

        wifi_provisioning_dns_serve(socket_fd);
    } while (0);

    if (socket_fd >= 0)
    {
        close(socket_fd);
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
        wifi_provisioning_dns_run = true;

        const BaseType_t task_created = xTaskCreatePinnedToCore(
            wifi_provisioning_dns_task,
            "dns_prov",
            WIFI_PROVISIONING_DNS_TASK_STACK,
            NULL,
            WIFI_PROVISIONING_DNS_TASK_PRIO,
            NULL,
            WIFI_PROVISIONING_DNS_TASK_CORE);

        /* Not fatal: the portal still works by IP, only the captive-portal
         * pop-up is lost. */
        if (task_created != pdPASS)
        {
            wifi_provisioning_dns_run = false;
            ESP_LOGE(wifi_provisioning_tag, "DNS task creation failed");
        }

        error_code = wifi_provisioning_http_start();

        if (error_code != ESP_OK)
        {
            wifi_provisioning_dns_run = false;
            break;
        }

        ESP_LOGI(wifi_provisioning_tag, "Portal up at http://%s", WIFI_PROVISIONING_AP_IP);
    } while (0);

    return error_code;
}

esp_err_t wifi_provisioning_stop(void)
{
    const esp_err_t error_code = wifi_provisioning_http_stop();

    wifi_provisioning_dns_run = false;

    ESP_LOGI(wifi_provisioning_tag, "Portal stopped");

    return error_code;
}

/**** END OF FILE ****/
