/**
 ******************************************************************************
 * @file    network_page.c
 * @author  Daniel Dyringer
 * @date    2026-10-13
 * @brief   Network page — link details, WiFi AP scan, ping and iperf.
 *
 *          Tools run in net_diag; handlers only queue them and the page polls
 *          /network/data for results.
 *
 *          No auth/CSRF — same LAN-only trust model as the rest of
 *          http_panel. Anyone on the LAN can start scans, pings and iperf runs.
 ******************************************************************************
 */


/* Includes ---------------------------------------------------------------- */
#include "network_page.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>

#include "cJSON.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "http_panel_providers.h"
#include "http_panel_server.h"
#include "lwip/inet.h"
#include "net_diag.h"
#include "utils.h"


/* Constants and macros ---------------------------------------------------- */
// clang-format off

#define NETWORK_PAGE_BODY_MAX       256u
#define NETWORK_PAGE_RECV_RETRIES   3u
#define NETWORK_PAGE_VALUE_LEN      64u
#define NETWORK_PAGE_EM_DASH        "\xe2\x80\x94"

// clang-format on

/* Private types ----------------------------------------------------------- */


/* Private variables ------------------------------------------------------- */

static const char* network_page_tag = "network_page";

extern const uint8_t network_page_html_start[] asm("_binary_network_page_html_start");
extern const uint8_t network_page_html_end[] asm("_binary_network_page_html_end");


/* Private functions ------------------------------------------------------- */

/* ---- Formatting ---- */

static const char* network_page_bandwidth_name(wifi_second_chan_t second_channel)
{
    const char* name = NULL;

    switch (second_channel)
    {
        case WIFI_SECOND_CHAN_ABOVE:
        {
            name = "HT40+";
        }
        break;
        case WIFI_SECOND_CHAN_BELOW:
        {
            name = "HT40-";
        }
        break;
        default:
        {
            name = "HT20";
        }
        break;
    }

    return name;
}

static const char* network_page_power_save_name(wifi_ps_type_t power_save)
{
    const char* name = NULL;

    switch (power_save)
    {
        case WIFI_PS_NONE:
        {
            name = "off";
        }
        break;
        case WIFI_PS_MIN_MODEM:
        {
            name = "min modem";
        }
        break;
        case WIFI_PS_MAX_MODEM:
        {
            name = "max modem";
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

static bool network_page_ip_is_set(const esp_ip_addr_t* address)
{
    bool is_set = false;

    if (address->type == ESP_IPADDR_TYPE_V6)
    {
        for (size_t i = 0u; i < ARRAY_SIZE(address->u_addr.ip6.addr); i++)
        {
            if (address->u_addr.ip6.addr[i] != 0u)
            {
                is_set = true;
            }
        }
    }
    else
    {
        is_set = (address->u_addr.ip4.addr != 0u);
    }

    return is_set;
}

static void network_page_format_ip(const esp_ip_addr_t* address, char* buffer, size_t length)
{
    if (address->type == ESP_IPADDR_TYPE_V6)
    {
        snprintf(buffer, length, IPV6STR, IPV62STR(address->u_addr.ip6));
    }
    else
    {
        snprintf(buffer, length, IPSTR, IP2STR(&address->u_addr.ip4));
    }
}

static void network_page_format_phy(const net_diag_link_t* link, char* buffer, size_t length)
{
    snprintf(
        buffer,
        length,
        "802.11%s%s%s%s",
        link->phy_11b ? "b" : "",
        link->phy_11g ? "/g" : "",
        link->phy_11n ? "/n" : "",
        link->phy_11ax ? "/ax" : "");
}

/* ---- Link ---- */

static void network_page_add_row(cJSON* array, const char* label, const char* value)
{
    cJSON* const item = cJSON_CreateObject();

    cJSON_AddStringToObject(item, "label", label);
    cJSON_AddStringToObject(item, "value", value);
    cJSON_AddItemToArray(array, item);
}

static void network_page_add_radio(cJSON* array, const net_diag_link_t* link)
{
    char value[NETWORK_PAGE_VALUE_LEN] = {0};

    network_page_add_row(array, "SSID", link->ssid);

    snprintf(value, sizeof(value), MACSTR, MAC2STR(link->bssid));
    network_page_add_row(array, "BSSID", value);

    snprintf(
        value,
        sizeof(value),
        "%u (%s)",
        (unsigned)link->channel,
        network_page_bandwidth_name(link->second_channel));
    network_page_add_row(array, "Channel", value);

    snprintf(value, sizeof(value), "%d dBm", (int)link->rssi);
    network_page_add_row(array, "RSSI", value);

    network_page_format_phy(link, value, sizeof(value));
    network_page_add_row(array, "PHY", value);

    network_page_add_row(array, "Security", net_diag_auth_name(link->auth));
}

static void network_page_add_addresses(cJSON* array, const net_diag_link_t* link)
{
    char value[NETWORK_PAGE_VALUE_LEN] = {0};

    snprintf(value, sizeof(value), IPSTR, IP2STR(&link->ip_info.ip));
    network_page_add_row(array, "IP", value);

    snprintf(value, sizeof(value), IPSTR, IP2STR(&link->ip_info.netmask));
    network_page_add_row(array, "Netmask", value);

    snprintf(value, sizeof(value), IPSTR, IP2STR(&link->ip_info.gw));
    network_page_add_row(array, "Gateway", value);

    for (size_t i = 0u; i < ARRAY_SIZE(link->dns); i++)
    {
        if (network_page_ip_is_set(&link->dns[i]))
        {
            network_page_format_ip(&link->dns[i], value, sizeof(value));
            network_page_add_row(array, (i == 0u) ? "DNS" : "DNS backup", value);
        }
    }

    for (uint8_t i = 0u; i < link->ip6_count; i++)
    {
        snprintf(value, sizeof(value), IPV6STR, IPV62STR(link->ip6[i]));
        network_page_add_row(array, "IPv6", value);
    }
}

static void network_page_add_link(cJSON* root)
{
    cJSON* const array                 = cJSON_AddArrayToObject(root, "link");
    char value[NETWORK_PAGE_VALUE_LEN] = {0};
    net_diag_link_t link               = {0};

    net_diag_link_get(&link);

    hp_get_wifi_state(value, sizeof(value));
    network_page_add_row(array, "WiFi state", value);

    if (link.associated)
    {
        network_page_add_radio(array, &link);
        network_page_add_addresses(array, &link);
    }

    network_page_add_row(
        array, "Hostname", (link.hostname[0] != '\0') ? link.hostname : NETWORK_PAGE_EM_DASH);

    snprintf(value, sizeof(value), MACSTR, MAC2STR(link.mac));
    network_page_add_row(array, "MAC", value);

    snprintf(
        value,
        sizeof(value),
        "%d.%02d dBm",
        (int)(link.tx_power_quarter_dbm / 4),
        (int)((link.tx_power_quarter_dbm % 4) * 25));
    network_page_add_row(array, "Max TX power", value);

    network_page_add_row(
        array, "Power save", network_page_power_save_name(link.power_save));

    snprintf(value, sizeof(value), IPSTR, IP2STR(&link.ip_info.ip));
    cJSON_AddStringToObject(root, "device_ip", value);
    snprintf(value, sizeof(value), IPSTR, IP2STR(&link.ip_info.gw));
    cJSON_AddStringToObject(root, "gateway", value);
}

/* ---- Tools ---- */

static void network_page_add_state(cJSON* object, net_diag_state_t state, esp_err_t error)
{
    cJSON_AddStringToObject(object, "state", net_diag_state_name(state));

    if (state == NET_DIAG_STATE_FAILED)
    {
        cJSON_AddStringToObject(object, "error", esp_err_to_name(error));
    }
}

static void network_page_add_scan(cJSON* root)
{
    net_diag_scan_result_t* const result = malloc(sizeof(net_diag_scan_result_t));

    if ((result != NULL) && (net_diag_scan_get(result) == ESP_OK))
    {
        cJSON* const scan = cJSON_AddObjectToObject(root, "scan");
        cJSON* const aps  = cJSON_AddArrayToObject(scan, "aps");
        char bssid[18]    = {0};

        network_page_add_state(scan, result->state, result->error);
        cJSON_AddNumberToObject(scan, "found", (double)result->found);

        for (uint16_t i = 0u; i < result->count; i++)
        {
            const net_diag_ap_t* const ap = &result->aps[i];
            cJSON* const item             = cJSON_CreateObject();

            snprintf(bssid, sizeof(bssid), MACSTR, MAC2STR(ap->bssid));
            cJSON_AddStringToObject(item, "ssid", ap->ssid);
            cJSON_AddStringToObject(item, "bssid", bssid);
            cJSON_AddNumberToObject(item, "channel", (double)ap->channel);
            cJSON_AddNumberToObject(item, "rssi", (double)ap->rssi);
            cJSON_AddStringToObject(item, "auth", net_diag_auth_name(ap->auth));
            cJSON_AddItemToArray(aps, item);
        }
    }

    free(result);
}

static void network_page_add_ping(cJSON* root)
{
    net_diag_ping_result_t result = {0};

    if (net_diag_ping_get(&result) == ESP_OK)
    {
        cJSON* const ping = cJSON_AddObjectToObject(root, "ping");

        network_page_add_state(ping, result.state, result.error);
        cJSON_AddStringToObject(ping, "host", result.host);
        cJSON_AddStringToObject(ping, "address", result.address);
        cJSON_AddNumberToObject(ping, "count", (double)result.count);
        cJSON_AddNumberToObject(ping, "size", (double)result.size);
        cJSON_AddNumberToObject(ping, "interval_ms", (double)result.interval_ms);
        cJSON_AddNumberToObject(ping, "timeout_ms", (double)result.timeout_ms);
        cJSON_AddNumberToObject(ping, "sent", (double)result.transmitted);
        cJSON_AddNumberToObject(ping, "received", (double)result.received);
        cJSON_AddNumberToObject(ping, "min_ms", (double)result.min_ms);
        cJSON_AddNumberToObject(ping, "max_ms", (double)result.max_ms);
        cJSON_AddNumberToObject(ping, "last_ms", (double)result.last_ms);
        cJSON_AddNumberToObject(
            ping,
            "avg_ms",
            (result.received > 0u) ? ((double)result.total_ms / (double)result.received) :
                                     0.0);
    }
}

static void network_page_add_iperf(cJSON* root)
{
    net_diag_iperf_result_t* const result = malloc(sizeof(net_diag_iperf_result_t));

    if ((result != NULL) && (net_diag_iperf_get(result) == ESP_OK))
    {
        cJSON* const iperf   = cJSON_AddObjectToObject(root, "iperf");
        cJSON* const periods = cJSON_AddArrayToObject(iperf, "periods");

        network_page_add_state(iperf, result->state, result->error);
        cJSON_AddStringToObject(
            iperf,
            "mode",
            (result->config.mode == NET_DIAG_IPERF_MODE_SERVER) ? "server" : "client");
        cJSON_AddStringToObject(
            iperf,
            "protocol",
            (result->config.protocol == NET_DIAG_IPERF_PROTOCOL_UDP) ? "udp" : "tcp");
        cJSON_AddNumberToObject(iperf, "time", (double)result->config.time_s);
        cJSON_AddBoolToObject(iperf, "transferring", result->transferring);
        cJSON_AddNumberToObject(iperf, "total_bytes", (double)result->total_bytes);
        cJSON_AddNumberToObject(
            iperf, "average_bps", (double)result->average_bits_per_second);

        for (uint16_t i = 0u; i < result->period_count; i++)
        {
            cJSON_AddItemToArray(
                periods, cJSON_CreateNumber((double)result->periods[i].bits_per_second));
        }
    }

    free(result);
}

/* Prefills the iperf client target: the PC with the browser is the usual
 * iperf server. httpd listens on IPv6, so IPv4 peers arrive v4-mapped. */
static void network_page_client_ip(httpd_req_t* request, char* buffer, size_t length)
{
    struct sockaddr_storage peer = {0};
    socklen_t peer_length        = sizeof(peer);
    struct in_addr address4      = {0};

    if (getpeername(httpd_req_to_sockfd(request), (struct sockaddr*)&peer, &peer_length)
        == 0)
    {
        if (peer.ss_family == AF_INET6)
        {
            address4.s_addr = ((const struct sockaddr_in6*)&peer)->sin6_addr.un.u32_addr[3];
        }
        else
        {
            address4 = ((const struct sockaddr_in*)&peer)->sin_addr;
        }

        inet_ntoa_r(address4, buffer, length);
    }
}

static char* network_page_build_json(httpd_req_t* request)
{
    char* json  = NULL;
    cJSON* root = cJSON_CreateObject();

    if (root != NULL)
    {
        char client_ip[NET_DIAG_ADDRESS_LEN] = {0};

        network_page_client_ip(request, client_ip, sizeof(client_ip));
        cJSON_AddStringToObject(root, "client_ip", client_ip);
        network_page_add_link(root);
        network_page_add_scan(root);
        network_page_add_ping(root);
        network_page_add_iperf(root);

        json = cJSON_PrintUnformatted(root);
        cJSON_Delete(root);
    }

    return json; /* caller must free() */
}

/* ---- Requests ---- */

static esp_err_t network_page_receive_json(httpd_req_t* request, cJSON** root)
{
    esp_err_t error_code                  = ESP_OK;
    char body[NETWORK_PAGE_BODY_MAX + 1u] = {0};

    do
    {
        const size_t length = request->content_len;

        if ((length == 0u) || (length > NETWORK_PAGE_BODY_MAX))
        {
            error_code = ESP_ERR_INVALID_SIZE;
            break;
        }

        size_t received = 0u;
        uint8_t retries = 0u;

        while ((error_code == ESP_OK) && (received < length))
        {
            const int result = httpd_req_recv(request, &body[received], length - received);

            if ((result == HTTPD_SOCK_ERR_TIMEOUT) && (retries < NETWORK_PAGE_RECV_RETRIES))
            {
                retries++;
            }
            else if (result <= 0)
            {
                error_code = ESP_FAIL;
            }
            else
            {
                received += (size_t)result;
            }
        }

        if (error_code != ESP_OK)
        {
            break;
        }

        *root = cJSON_Parse(body);

        if (*root == NULL)
        {
            error_code = ESP_ERR_INVALID_ARG;
        }
    } while (0);

    return error_code;
}

static uint32_t network_page_json_uint(const cJSON* root, const char* name, uint32_t fallback)
{
    const cJSON* const item = cJSON_GetObjectItemCaseSensitive(root, name);
    uint32_t value          = fallback;

    if (cJSON_IsNumber(item) && (item->valuedouble >= 0.0)
        && (item->valuedouble <= (double)UINT32_MAX))
    {
        value = (uint32_t)item->valuedouble;
    }

    return value;
}

static void network_page_json_string(const cJSON* root, const char* name, char* buffer, size_t length)
{
    const cJSON* const item = cJSON_GetObjectItemCaseSensitive(root, name);

    if (cJSON_IsString(item))
    {
        strlcpy(buffer, item->valuestring, length);
    }
}

static esp_err_t network_page_send_result(httpd_req_t* request, esp_err_t result)
{
    esp_err_t error_code = ESP_OK;

    if (result != ESP_OK)
    {
        ESP_LOGW(network_page_tag, "%s failed: %s", request->uri, esp_err_to_name(result));
    }

    switch (result)
    {
        case ESP_OK:
        {
            error_code = httpd_resp_sendstr(request, "OK");
        }
        break;
        case ESP_ERR_INVALID_ARG:
        {
            error_code =
                httpd_resp_send_err(request, HTTPD_400_BAD_REQUEST, "Invalid input");
        }
        break;
        case ESP_ERR_INVALID_SIZE:
        {
            error_code =
                httpd_resp_send_err(request, HTTPD_400_BAD_REQUEST, "Body too long");
        }
        break;
        case ESP_ERR_INVALID_STATE:
        {
            error_code = httpd_resp_send_custom_err(
                request, "409 Conflict", "Already running or not running");
        }
        break;
        default:
        {
            error_code = httpd_resp_send_err(
                request, HTTPD_500_INTERNAL_SERVER_ERROR, esp_err_to_name(result));
        }
        break;
    }

    return error_code;
}

/* ---- HTTP handlers ---- */

static esp_err_t network_page_h_root(httpd_req_t* request)
{
    const size_t length = (size_t)(network_page_html_end - network_page_html_start);

    httpd_resp_set_type(request, "text/html");

    return httpd_resp_send(request, (const char*)network_page_html_start, (ssize_t)length);
}

static esp_err_t network_page_h_data(httpd_req_t* request)
{
    esp_err_t error_code = ESP_OK;
    char* json           = network_page_build_json(request);

    if (json == NULL)
    {
        error_code = httpd_resp_send_500(request);
    }
    else
    {
        httpd_resp_set_type(request, "application/json");
        httpd_resp_set_hdr(request, "Cache-Control", "no-store");
        error_code = httpd_resp_sendstr(request, json);
        free(json);
    }

    return error_code;
}

static esp_err_t network_page_h_scan(httpd_req_t* request)
{
    return network_page_send_result(request, net_diag_scan_start());
}

static esp_err_t network_page_h_ping(httpd_req_t* request)
{
    cJSON* root                   = NULL;
    net_diag_ping_config_t config = {0};

    esp_err_t result = network_page_receive_json(request, &root);

    if (result == ESP_OK)
    {
        network_page_json_string(root, "host", config.host, sizeof(config.host));
        config.count = (uint16_t)MIN(
            network_page_json_uint(root, "count", 0u),
            (uint32_t)NET_DIAG_PING_MAX_COUNT + 1u);
        config.size = network_page_json_uint(root, "size", NET_DIAG_PING_DEFAULT_SIZE);
        config.interval_ms =
            network_page_json_uint(root, "interval_ms", NET_DIAG_PING_DEFAULT_INTERVAL_MS);
        config.timeout_ms =
            network_page_json_uint(root, "timeout_ms", NET_DIAG_PING_DEFAULT_TIMEOUT_MS);
        result = net_diag_ping_start(&config);
    }

    cJSON_Delete(root);

    return network_page_send_result(request, result);
}

static esp_err_t network_page_h_iperf(httpd_req_t* request)
{
    cJSON* root                    = NULL;
    net_diag_iperf_config_t config = {0};
    char text[8]                   = {0};

    esp_err_t result = network_page_receive_json(request, &root);

    if (result == ESP_OK)
    {
        network_page_json_string(root, "mode", text, sizeof(text));
        config.mode = (strcmp(text, "client") == 0) ? NET_DIAG_IPERF_MODE_CLIENT :
                                                      NET_DIAG_IPERF_MODE_SERVER;

        network_page_json_string(root, "protocol", text, sizeof(text));
        config.protocol = (strcmp(text, "udp") == 0) ? NET_DIAG_IPERF_PROTOCOL_UDP :
                                                       NET_DIAG_IPERF_PROTOCOL_TCP;

        network_page_json_string(root, "host", config.host, sizeof(config.host));
        config.port = (uint16_t)MIN(
            network_page_json_uint(root, "port", NET_DIAG_IPERF_DEFAULT_PORT), 65535u);
        config.time_s = (uint16_t)MIN(
            network_page_json_uint(root, "time", 0u),
            (uint32_t)NET_DIAG_IPERF_MAX_TIME_S + 1u);
        config.bandwidth_kbps = network_page_json_uint(root, "bandwidth_kbps", 0u);

        result = net_diag_iperf_start(&config);
    }

    cJSON_Delete(root);

    return network_page_send_result(request, result);
}

static esp_err_t network_page_h_iperf_stop(httpd_req_t* request)
{
    return network_page_send_result(request, net_diag_iperf_stop());
}


/* Exposed API ------------------------------------------------------------- */

esp_err_t network_page_init(void)
{
    esp_err_t error_code = ESP_OK;

    /* Static: http_panel_server keeps the URIs by pointer. */
    static const httpd_uri_t network_page_uris[] = {
        {
            .uri     = "/network",
            .method  = HTTP_GET,
            .handler = network_page_h_root,
        },
        {
            .uri     = "/network/data",
            .method  = HTTP_GET,
            .handler = network_page_h_data,
        },
        {
            .uri     = "/network/scan",
            .method  = HTTP_POST,
            .handler = network_page_h_scan,
        },
        {
            .uri     = "/network/ping",
            .method  = HTTP_POST,
            .handler = network_page_h_ping,
        },
        {
            .uri     = "/network/iperf",
            .method  = HTTP_POST,
            .handler = network_page_h_iperf,
        },
        {
            .uri     = "/network/iperf/stop",
            .method  = HTTP_POST,
            .handler = network_page_h_iperf_stop,
        },
    };

    for (size_t i = 0u; (error_code == ESP_OK) && (i < ARRAY_SIZE(network_page_uris)); i++)
    {
        error_code = http_panel_server_register_uri(&network_page_uris[i]);
    }

    return error_code;
}

/**** END OF FILE ****/
