/**
 ******************************************************************************
 * @file    wifi_connection.h
 * @author  Daniel Dyringer
 * @date    2026-02-17
 * @brief   WiFi connection manager — owns the STA/AP state machine,
 *          NVS credential storage and reconnection logic.
 *          net_manager task is pinned to Core 0.
 ******************************************************************************
 */

/* Define to prevent recursive inclusion ----------------------------------- */
#ifndef WIFI_CONNECTION_H_
#define WIFI_CONNECTION_H_

/* Includes ---------------------------------------------------------------- */

#include "esp_err.h"
#include "esp_event.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include <stddef.h>
#include "esp_netif_ip_addr.h"

/* Constants and macros ---------------------------------------------------- */
// clang-format off
#define WIFI_CONNECTION_CORE            0
#define WIFI_CONNECTION_PUB_QUEUE_LEN   8
// clang-format on

/* Types ------------------------------------------------------------------- */

typedef enum
{
    WIFI_CONN_EVENT_CONNECTED,
    WIFI_CONN_EVENT_DISCONNECTED,
    WIFI_CONN_EVENT_PROVISIONING_STARTED,
    WIFI_CONN_EVENT_PROVISIONED,
} wifi_conn_event_id_t;

/** Public event posted to wifi_conn_pub_queue — consumed by Core 1 tasks. */
typedef struct
{
    wifi_conn_event_id_t id;
    union
    {
        esp_ip4_addr_t ip;   /**< valid on WIFI_CONN_EVENT_CONNECTED */
    } data;
} wifi_conn_pub_event_t;

typedef enum
{
    WIFI_CONN_STATE_IDLE = 0,
    WIFI_CONN_STATE_PROVISIONING,
    WIFI_CONN_STATE_CONNECTING,
    WIFI_CONN_STATE_CONNECTED,
    WIFI_CONN_STATE_RECONNECTING,
} wifi_conn_state_t;

/* Exposed API ------------------------------------------------------------- */

/** @brief Init netifs, WiFi driver (APSTA), queues, event handlers.
 *         Spawns net_manager task pinned to Core 0. */
esp_err_t wifi_connection_init(void);

/** @brief Start connection logic — credentials in NVS → STA, else → portal. */
esp_err_t wifi_connection_start(void);

/** @brief Called by wifi_provisioning when credentials are submitted. */
esp_err_t wifi_connection_set_credentials(const char *ssid, const char *password);

/** @brief Erase NVS credentials and reboot into provisioning. */
esp_err_t wifi_connection_forget(void);

wifi_conn_state_t wifi_connection_get_state(void);
const char       *wifi_connection_state_str(wifi_conn_state_t state);
esp_err_t         wifi_connection_get_ssid(char *buf, size_t len);

/** @brief Last IP acquired via DHCP. Only meaningful once state is
 *         WIFI_CONN_STATE_CONNECTED; zeroed otherwise. */
esp_ip4_addr_t wifi_connection_get_ip(void);

/** @brief Callback type for wifi_connection_register_state_cb(). Invoked
 *         synchronously from the net_manager task (Core 0) on every state
 *         transition — keep implementations short and non-blocking. */
typedef void (*wifi_connection_state_cb_t)(wifi_conn_state_t state);

/**
 * @brief Register a callback invoked on every WiFi connection state change.
 *        Single slot — registering again replaces the previous callback.
 *        Intended for a display app to learn about connectivity without
 *        polling; the app itself should still poll wifi_connection_get_state()
 *        for its own UI refresh cadence rather than relying on callback
 *        frequency.
 */
void wifi_connection_register_state_cb(wifi_connection_state_cb_t cb);

/**
 * @brief Returns the public event queue.
 *        Core 1 tasks block on this to receive network state changes.
 *        Item type: wifi_conn_pub_event_t
 */
QueueHandle_t wifi_connection_get_event_queue(void);

#endif /* WIFI_CONNECTION_H_ */

/**** END OF FILE ****/