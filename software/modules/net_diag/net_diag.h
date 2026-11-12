/**
 ******************************************************************************
 * @file    net_diag.h
 * @author  Daniel Dyringer
 * @date    2026-10-13
 * @brief   Network diagnostics — link details, WiFi AP scan, ping and iperf.
 *
 *          Every tool runs asynchronously: *_start() queues the job and
 *          returns at once, *_get() copies the latest result. Safe to call
 *          from any task, including httpd handlers; one run per tool at a
 *          time (ESP_ERR_INVALID_STATE while busy).
 ******************************************************************************
 */

/* Define to prevent recursive inclusion ----------------------------------- */
#ifndef NET_DIAG_H_
#define NET_DIAG_H_

/* Includes ---------------------------------------------------------------- */
#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "esp_netif.h"
#include "esp_wifi_types.h"

/* Constants and macros ---------------------------------------------------- */
// clang-format off

#define NET_DIAG_SSID_LEN               33u
#define NET_DIAG_HOSTNAME_LEN           33u
#define NET_DIAG_HOST_LEN               64u
#define NET_DIAG_ADDRESS_LEN            46u
#define NET_DIAG_IP6_MAX                3u

#define NET_DIAG_SCAN_MAX_APS           32u

#define NET_DIAG_PING_MAX_COUNT         100u
#define NET_DIAG_PING_MIN_SIZE          1u
#define NET_DIAG_PING_MAX_SIZE          65500u
#define NET_DIAG_PING_DEFAULT_SIZE      64u
#define NET_DIAG_PING_MIN_INTERVAL_MS   10u
#define NET_DIAG_PING_MAX_INTERVAL_MS   60000u
#define NET_DIAG_PING_DEFAULT_INTERVAL_MS 1000u
#define NET_DIAG_PING_MIN_TIMEOUT_MS    10u
#define NET_DIAG_PING_MAX_TIMEOUT_MS    10000u
#define NET_DIAG_PING_DEFAULT_TIMEOUT_MS 1000u

#define NET_DIAG_IPERF_DEFAULT_PORT     5001u
#define NET_DIAG_IPERF_MAX_TIME_S       60u
#define NET_DIAG_IPERF_MAX_PERIODS      NET_DIAG_IPERF_MAX_TIME_S

// clang-format on

/* Types ------------------------------------------------------------------- */

typedef enum net_diag_state_e
{
    NET_DIAG_STATE_IDLE    = 0,
    NET_DIAG_STATE_RUNNING = 1,
    NET_DIAG_STATE_DONE    = 2,
    NET_DIAG_STATE_FAILED  = 3,
} net_diag_state_t;

typedef struct net_diag_link_s
{
    esp_netif_ip_info_t ip_info;
    esp_ip_addr_t dns[2];
    esp_ip6_addr_t ip6[NET_DIAG_IP6_MAX];
    wifi_auth_mode_t auth;
    wifi_second_chan_t second_channel;
    wifi_ps_type_t power_save;
    char ssid[NET_DIAG_SSID_LEN];
    char hostname[NET_DIAG_HOSTNAME_LEN];
    uint8_t bssid[6];
    uint8_t mac[6];
    uint8_t channel;
    uint8_t ip6_count;
    int8_t rssi;
    int8_t tx_power_quarter_dbm;
    bool associated;
    bool phy_11b;
    bool phy_11g;
    bool phy_11n;
    bool phy_11ax;
} net_diag_link_t;

typedef struct net_diag_ap_s
{
    wifi_auth_mode_t auth;
    char ssid[NET_DIAG_SSID_LEN];
    uint8_t bssid[6];
    uint8_t channel;
    int8_t rssi;
} net_diag_ap_t;

typedef struct net_diag_scan_result_s
{
    net_diag_ap_t aps[NET_DIAG_SCAN_MAX_APS]; /* strongest first */
    esp_err_t error;
    uint16_t count; /* stored in aps */
    uint16_t found; /* reported by the driver, may exceed count */
    net_diag_state_t state;
} net_diag_scan_result_t;

typedef struct net_diag_ping_config_s
{
    char host[NET_DIAG_HOST_LEN]; /* hostname or IPv4 address */
    uint32_t size;                /* ICMP payload bytes */
    uint32_t interval_ms;         /* period between requests */
    uint32_t timeout_ms;          /* wait for a reply */
    uint16_t count;
} net_diag_ping_config_t;

typedef struct net_diag_ping_result_s
{
    char host[NET_DIAG_HOST_LEN];
    char address[NET_DIAG_ADDRESS_LEN];
    esp_err_t error;
    uint32_t min_ms;
    uint32_t max_ms;
    uint32_t total_ms; /* sum over received replies */
    uint32_t last_ms;
    uint32_t size;
    uint32_t interval_ms;
    uint32_t timeout_ms;
    uint16_t count;
    uint16_t transmitted;
    uint16_t received;
    net_diag_state_t state;
} net_diag_ping_result_t;

typedef enum net_diag_iperf_mode_e
{
    NET_DIAG_IPERF_MODE_SERVER = 0,
    NET_DIAG_IPERF_MODE_CLIENT = 1,
} net_diag_iperf_mode_t;

typedef enum net_diag_iperf_protocol_e
{
    NET_DIAG_IPERF_PROTOCOL_TCP = 0,
    NET_DIAG_IPERF_PROTOCOL_UDP = 1,
} net_diag_iperf_protocol_t;

typedef struct net_diag_iperf_config_s
{
    char host[NET_DIAG_HOST_LEN]; /* client only: IPv4 address of the iperf2 server */
    uint32_t bandwidth_kbps;      /* UDP client only; 0 = unlimited */
    uint16_t port;
    uint16_t time_s;
    net_diag_iperf_mode_t mode;
    net_diag_iperf_protocol_t protocol;
} net_diag_iperf_config_t;

typedef struct net_diag_iperf_period_s
{
    uint32_t bits_per_second;
    uint16_t start_s;
    uint16_t end_s;
} net_diag_iperf_period_t;

typedef struct net_diag_iperf_result_s
{
    uint64_t total_bytes;
    net_diag_iperf_config_t config;
    net_diag_iperf_period_t periods[NET_DIAG_IPERF_MAX_PERIODS];
    esp_err_t error;
    uint32_t average_bits_per_second;
    uint16_t period_count;
    net_diag_state_t state;
    bool transferring;
} net_diag_iperf_result_t;

/* Exposed API ------------------------------------------------------------- */

esp_err_t net_diag_init(void);

esp_err_t net_diag_link_get(net_diag_link_t* link);
const char* net_diag_auth_name(wifi_auth_mode_t auth);
const char* net_diag_state_name(net_diag_state_t state);

esp_err_t net_diag_scan_start(void);
esp_err_t net_diag_scan_get(net_diag_scan_result_t* result);

esp_err_t net_diag_ping_start(const net_diag_ping_config_t* config);
esp_err_t net_diag_ping_get(net_diag_ping_result_t* result);

esp_err_t net_diag_iperf_start(const net_diag_iperf_config_t* config);
esp_err_t net_diag_iperf_stop(void);
esp_err_t net_diag_iperf_get(net_diag_iperf_result_t* result);

#endif /* NET_DIAG_H_ */

/**** END OF FILE ****/
