/**
 ******************************************************************************
 * @file    wifi_connection.h
 * @author  Daniel Dyringer
 * @date    2026-02-17
 * @brief   WiFi connection manager — owns the STA/AP state machine,
 *          NVS credential storage and reconnection logic.
 *
 *          The net_manager task is pinned to Core 0. State callbacks run
 *          synchronously on it and must not block. The event queue carries
 *          wifi_connection_event_t items.
 ******************************************************************************
 */

/* Define to prevent recursive inclusion ----------------------------------- */
#ifndef WIFI_CONNECTION_H_
#define WIFI_CONNECTION_H_

/* Includes ---------------------------------------------------------------- */
#include <stdbool.h>
#include <stddef.h>

#include "esp_err.h"
#include "esp_event.h"
#include "esp_netif_ip_addr.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

/* Constants and macros ---------------------------------------------------- */
// clang-format off

#define WIFI_CONNECTION_CORE            0
#define WIFI_CONNECTION_EVENT_QUEUE_LEN 8u

/* What wifi_connection_credentials_valid() accepts, for user-facing errors. */
#define WIFI_CONNECTION_CREDENTIALS_RULE \
    "SSID 1-32 characters; password empty (open), 8-63 characters or 64 hex digits"

// clang-format on

/* Types ------------------------------------------------------------------- */

/* Posted without blocking: a full event queue drops the notification. */
ESP_EVENT_DECLARE_BASE(WIFI_CONNECTION_EVENTS);

typedef enum wifi_connection_notify_e
{
    /* event_data is the new wifi_connection_state_t. */
    WIFI_CONNECTION_NOTIFY_STATE_CHANGED = 0,
} wifi_connection_notify_t;

typedef enum wifi_connection_event_id_e
{
    WIFI_CONNECTION_EVENT_ID_CONNECTED            = 0,
    WIFI_CONNECTION_EVENT_ID_DISCONNECTED         = 1,
    WIFI_CONNECTION_EVENT_ID_PROVISIONING_STARTED = 2,
    WIFI_CONNECTION_EVENT_ID_PROVISIONED          = 3,
} wifi_connection_event_id_t;

typedef struct wifi_connection_event_s
{
    wifi_connection_event_id_t id;
    union
    {
        esp_ip4_addr_t ip; /**< valid on WIFI_CONNECTION_EVENT_ID_CONNECTED */
    } data;
} wifi_connection_event_t;

typedef enum wifi_connection_state_e
{
    WIFI_CONNECTION_STATE_IDLE         = 0,
    WIFI_CONNECTION_STATE_PROVISIONING = 1,
    WIFI_CONNECTION_STATE_CONNECTING   = 2,
    WIFI_CONNECTION_STATE_CONNECTED    = 3,
    WIFI_CONNECTION_STATE_RECONNECTING = 4,
} wifi_connection_state_t;

typedef void (*wifi_connection_state_cb_t)(wifi_connection_state_t state);

/* Exposed API ------------------------------------------------------------- */

esp_err_t wifi_connection_init(void);
esp_err_t wifi_connection_start(void);
bool wifi_connection_credentials_valid(const char* ssid, const char* password);

/* Validated, then saved to NVS only once the network gives an IP. */
esp_err_t wifi_connection_set_credentials(const char* ssid, const char* password);

/* Returns only on failure. */
esp_err_t wifi_connection_forget(void);

wifi_connection_state_t wifi_connection_get_state(void);
const char* wifi_connection_state_str(wifi_connection_state_t state);
esp_err_t wifi_connection_get_ssid(char* buffer, size_t length);
esp_ip4_addr_t wifi_connection_get_ip(void);

void wifi_connection_register_state_cb(wifi_connection_state_cb_t callback);
QueueHandle_t wifi_connection_get_event_queue(void);

#endif /* WIFI_CONNECTION_H_ */

/**** END OF FILE ****/
