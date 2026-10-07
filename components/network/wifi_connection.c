/**
 ******************************************************************************
 * @file    wifi_connection.c
 * @author  Daniel Dyringer
 * @date    2026-02-17
 * @brief   WiFi connection manager — owns the STA/AP state machine,
 *          NVS credential storage and reconnection logic.
 *          net_manager task is pinned to Core 0.
 ******************************************************************************
 */

/* Includes ---------------------------------------------------------------- */
#include "wifi_connection.h"

#include <inttypes.h>
#include <string.h>

#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "utils.h"
#include "wifi_ap.h"
#include "wifi_provisioning.h"
#include "wifi_sta.h"
#include "wifi_types.h"

/* Constants and macros ---------------------------------------------------- */
// clang-format off
#define WIFI_CONNECTION_NVS_NAMESPACE          "wifi_creds"
#define WIFI_CONNECTION_NVS_KEY_SSID           "ssid"
#define WIFI_CONNECTION_NVS_KEY_PASS           "pass"
#define WIFI_CONNECTION_RECONNECT_DELAY_MIN_US (1000ULL  * 1000ULL)
#define WIFI_CONNECTION_RECONNECT_DELAY_MAX_US (60000ULL * 1000ULL)
#define WIFI_CONNECTION_INT_QUEUE_LEN          16
#define WIFI_CONNECTION_TASK_STACK             4096
#define WIFI_CONNECTION_TASK_PRIO              10
// clang-format on

/* Private types ----------------------------------------------------------- */

/** Internal raw events — posted by WiFi/IP callbacks into the int queue.
 *  Processed exclusively by the net_manager task on Core 0.            */
typedef enum
{
    WIFI_CONNECTION_INT_EVT_STA_CONNECTED,
    WIFI_CONNECTION_INT_EVT_STA_DISCONNECTED,
    WIFI_CONNECTION_INT_EVT_GOT_IP,
    WIFI_CONNECTION_INT_EVT_AP_CLIENT_CONNECTED,
    WIFI_CONNECTION_INT_EVT_AP_CLIENT_DISCONNECTED,
    WIFI_CONNECTION_INT_EVT_SET_CREDENTIALS, /**< posted by provisioning   */
} wifi_connection_int_evt_type_t;

typedef struct
{
    wifi_connection_int_evt_type_t type;
    union
    {
        struct
        {
            int reason;
        } disconnected;
        struct
        {
            esp_ip4_addr_t ip;
        } got_ip;
        struct
        {
            uint8_t mac[WIFI_MAC_LEN];
        } client;
        struct
        {
            char ssid[WIFI_SSID_LEN];
            char pass[WIFI_PASS_LEN];
        } credentials;
    } data;
} wifi_connection_int_evt_t;

/* Private variables ------------------------------------------------------- */

ESP_EVENT_DEFINE_BASE(WIFI_CONN_EVENT);

static const char* wifi_connection_tag         = "wifi_conn";
static wifi_conn_state_t wifi_connection_state = WIFI_CONN_STATE_IDLE;
static SemaphoreHandle_t wifi_connection_mutex;
static QueueHandle_t wifi_connection_int_queue; /* callbacks  → task  */
static QueueHandle_t wifi_connection_pub_queue; /* task       → Core1 */
static esp_timer_handle_t wifi_connection_reconnect_timer;
static uint64_t wifi_connection_reconnect_delay_us =
    WIFI_CONNECTION_RECONNECT_DELAY_MIN_US;
static char wifi_connection_ssid[WIFI_SSID_LEN];
static char wifi_connection_pass[WIFI_PASS_LEN];
static esp_ip4_addr_t wifi_connection_ip;
static wifi_connection_state_cb_t wifi_connection_state_cb;

/* Private functions ------------------------------------------------------- */

static void wifi_connection_state_set(wifi_conn_state_t st)
{
    xSemaphoreTake(wifi_connection_mutex, portMAX_DELAY);
    wifi_connection_state = st;
    xSemaphoreGive(wifi_connection_mutex);
    ESP_LOGI(wifi_connection_tag, "State → %s", wifi_connection_state_str(st));

    if (wifi_connection_state_cb)
    {
        wifi_connection_state_cb(st);
    }
}

static void wifi_connection_pub_post(
    wifi_conn_event_id_t id, const wifi_conn_pub_event_t *evt)
{
    wifi_conn_pub_event_t e = {.id = id};

    if (evt)
    {
        e = *evt;
    }
    else
    {
        e.id = id;
    }

    xQueueSend(wifi_connection_pub_queue, &e, 0);
}

/* ── NVS ──────────────────────────────────────────────────────────────────── */

static esp_err_t wifi_connection_nvs_load(
    char *ssid, size_t ssid_sz, char *pass, size_t pass_sz)
{
    nvs_handle_t h;
    esp_err_t    error_code = nvs_open(
        WIFI_CONNECTION_NVS_NAMESPACE, NVS_READONLY, &h);

    if (error_code != ESP_OK)
    {
        return error_code;
    }

    error_code = nvs_get_str(h, WIFI_CONNECTION_NVS_KEY_SSID, ssid, &ssid_sz);

    if (error_code == ESP_OK)
    {
        error_code = nvs_get_str(
            h, WIFI_CONNECTION_NVS_KEY_PASS, pass, &pass_sz);
    }

    nvs_close(h);
    return error_code;
}

static esp_err_t wifi_connection_nvs_save(const char *ssid, const char *pass)
{
    nvs_handle_t h;
    esp_err_t    error_code = nvs_open(
        WIFI_CONNECTION_NVS_NAMESPACE, NVS_READWRITE, &h);

    if (error_code != ESP_OK)
    {
        ESP_LOGE(wifi_connection_tag, "nvs_open failed: %s",
                 esp_err_to_name(error_code));
        return error_code;
    }

    nvs_set_str(h, WIFI_CONNECTION_NVS_KEY_SSID, ssid);
    nvs_set_str(h, WIFI_CONNECTION_NVS_KEY_PASS, pass);
    error_code = nvs_commit(h);
    nvs_close(h);
    return error_code;
}

static esp_err_t wifi_connection_nvs_erase(void)
{
    nvs_handle_t h;
    esp_err_t    error_code = nvs_open(
        WIFI_CONNECTION_NVS_NAMESPACE, NVS_READWRITE, &h);

    if (error_code != ESP_OK)
    {
        ESP_LOGE(wifi_connection_tag, "nvs_open failed: %s",
                 esp_err_to_name(error_code));
        return error_code;
    }

    nvs_erase_key(h, WIFI_CONNECTION_NVS_KEY_SSID);
    nvs_erase_key(h, WIFI_CONNECTION_NVS_KEY_PASS);
    error_code = nvs_commit(h);
    nvs_close(h);
    return error_code;
}

/* ── Reconnect timer callback (runs in esp_timer task, Core 0) ───────────── */

static void wifi_connection_reconnect_cb(void* arg)
{
    /* Post to int queue so net_manager handles it — keep timer cb minimal */
    wifi_connection_int_evt_t evt = {
        .type = WIFI_CONNECTION_INT_EVT_STA_CONNECTED, /* triggers reconnect */
    };
    /* Reuse STA_CONNECTED path: net_manager will call net_wifi_sta_connect */
    net_wifi_sta_connect(wifi_connection_ssid, wifi_connection_pass);
    wifi_connection_reconnect_delay_us = MIN(
        wifi_connection_reconnect_delay_us * 2, WIFI_CONNECTION_RECONNECT_DELAY_MAX_US);
}

/* ── WiFi / IP event handlers (run in sys_evt task — post only, no logic) ── */

static void wifi_connection_on_wifi_event(void* arg, esp_event_base_t base, int32_t id, void* data)
{
    wifi_connection_int_evt_t evt = {0};

    switch ((wifi_event_t)id)
    {
        case WIFI_EVENT_STA_CONNECTED:
            evt.type = WIFI_CONNECTION_INT_EVT_STA_CONNECTED;
            break;

        case WIFI_EVENT_STA_DISCONNECTED:
        {
            wifi_event_sta_disconnected_t* e = data;
            evt.type                         = WIFI_CONNECTION_INT_EVT_STA_DISCONNECTED;
            evt.data.disconnected.reason     = e->reason;
            break;
        }

        case WIFI_EVENT_AP_STACONNECTED:
        {
            wifi_event_ap_staconnected_t* e = data;
            evt.type                        = WIFI_CONNECTION_INT_EVT_AP_CLIENT_CONNECTED;
            memcpy(evt.data.client.mac, e->mac, WIFI_MAC_LEN);
            break;
        }

        case WIFI_EVENT_AP_STADISCONNECTED:
        {
            wifi_event_ap_stadisconnected_t* e = data;
            evt.type = WIFI_CONNECTION_INT_EVT_AP_CLIENT_DISCONNECTED;
            memcpy(evt.data.client.mac, e->mac, WIFI_MAC_LEN);
            break;
        }

        default:
            return; /* ignore unhandled events */
    }

    xQueueSendFromISR(wifi_connection_int_queue, &evt, NULL);
}

static void wifi_connection_on_ip_event(void* arg, esp_event_base_t base, int32_t id, void* data)
{
    if (id != IP_EVENT_STA_GOT_IP)
    {
        return;
    }

    ip_event_got_ip_t* e          = data;
    wifi_connection_int_evt_t evt = {
        .type           = WIFI_CONNECTION_INT_EVT_GOT_IP,
        .data.got_ip.ip = e->ip_info.ip,
    };
    xQueueSendFromISR(wifi_connection_int_queue, &evt, NULL);
}

/* ── net_manager task — Core 0 ───────────────────────────────────────────── */

static void wifi_connection_net_manager_task(void* arg)
{
    wifi_connection_int_evt_t evt;

    for (;;)
    {
        if (xQueueReceive(wifi_connection_int_queue, &evt, portMAX_DELAY) != pdTRUE)
            continue;

        switch (evt.type)
        {

            case WIFI_CONNECTION_INT_EVT_STA_CONNECTED:
                ESP_LOGI(wifi_connection_tag, "STA associated, waiting for IP...");
                break;

            case WIFI_CONNECTION_INT_EVT_STA_DISCONNECTED:
                ESP_LOGW(
                    wifi_connection_tag,
                    "STA disconnected (reason=%d), retry in %" PRIu64 "ms",
                    evt.data.disconnected.reason,
                    wifi_connection_reconnect_delay_us / 1000);
                wifi_connection_state_set(WIFI_CONN_STATE_RECONNECTING);
                wifi_connection_pub_post(WIFI_CONN_EVENT_DISCONNECTED, NULL);
                esp_timer_start_once(
                    wifi_connection_reconnect_timer, wifi_connection_reconnect_delay_us);
                break;

            case WIFI_CONNECTION_INT_EVT_GOT_IP:
            {
                ESP_LOGI(
                    wifi_connection_tag, "IP acquired: " IPSTR, IP2STR(&evt.data.got_ip.ip));
                esp_timer_stop(wifi_connection_reconnect_timer);
                wifi_connection_reconnect_delay_us = WIFI_CONNECTION_RECONNECT_DELAY_MIN_US;
                wifi_connection_ip = evt.data.got_ip.ip;
                wifi_connection_state_set(WIFI_CONN_STATE_CONNECTED);
                wifi_conn_pub_event_t pub = {
                    .id      = WIFI_CONN_EVENT_CONNECTED,
                    .data.ip = evt.data.got_ip.ip,
                };
                wifi_connection_pub_post(WIFI_CONN_EVENT_CONNECTED, &pub);
                break;
            }

            case WIFI_CONNECTION_INT_EVT_AP_CLIENT_CONNECTED:
                ESP_LOGI(
                    wifi_connection_tag,
                    "Client connected " MACSTR,
                    MAC2STR(evt.data.client.mac));
                break;

            case WIFI_CONNECTION_INT_EVT_AP_CLIENT_DISCONNECTED:
                ESP_LOGI(
                    wifi_connection_tag,
                    "Client disconnected " MACSTR,
                    MAC2STR(evt.data.client.mac));
                break;

            case WIFI_CONNECTION_INT_EVT_SET_CREDENTIALS:
                ESP_LOGI(
                    wifi_connection_tag,
                    "Applying credentials for '%s'",
                    evt.data.credentials.ssid);
                strlcpy(
                    wifi_connection_ssid,
                    evt.data.credentials.ssid,
                    sizeof(wifi_connection_ssid));
                strlcpy(
                    wifi_connection_pass,
                    evt.data.credentials.pass,
                    sizeof(wifi_connection_pass));
                wifi_connection_nvs_save(wifi_connection_ssid, wifi_connection_pass);
                wifi_provisioning_stop();
                wifi_connection_reconnect_delay_us = WIFI_CONNECTION_RECONNECT_DELAY_MIN_US;
                wifi_connection_state_set(WIFI_CONN_STATE_CONNECTING);
                wifi_connection_pub_post(WIFI_CONN_EVENT_PROVISIONED, NULL);
                net_wifi_sta_connect(wifi_connection_ssid, wifi_connection_pass);
                break;

            default:
                break;
        }
    }
}

/* Exposed API ------------------------------------------------------------- */

esp_err_t wifi_connection_init(void)
{
    esp_err_t error_code;

    do
    {
        wifi_connection_mutex = xSemaphoreCreateMutex();
        wifi_connection_int_queue = xQueueCreate(
            WIFI_CONNECTION_INT_QUEUE_LEN, sizeof(wifi_connection_int_evt_t));
        wifi_connection_pub_queue = xQueueCreate(
            WIFI_CONNECTION_PUB_QUEUE_LEN, sizeof(wifi_conn_pub_event_t));

        error_code = esp_netif_init();

        if (error_code != ESP_OK)
        {
            ESP_LOGE(wifi_connection_tag, "esp_netif_init failed: %s",
                     esp_err_to_name(error_code));
            break;
        }

        error_code = esp_event_loop_create_default();

        if (error_code != ESP_OK)
        {
            ESP_LOGE(wifi_connection_tag,
                     "esp_event_loop_create_default failed: %s",
                     esp_err_to_name(error_code));
            break;
        }

        esp_netif_t *sta_netif;
        esp_netif_t *ap_netif;

        error_code = net_wifi_sta_init(&sta_netif);

        if (error_code != ESP_OK)
        {
            break;
        }

        error_code = wifi_ap_init(&ap_netif);

        if (error_code != ESP_OK)
        {
            break;
        }

        wifi_init_config_t wifi_cfg = WIFI_INIT_CONFIG_DEFAULT();

        error_code = esp_wifi_init(&wifi_cfg);

        if (error_code != ESP_OK)
        {
            ESP_LOGE(wifi_connection_tag, "esp_wifi_init failed: %s",
                     esp_err_to_name(error_code));
            break;
        }

        error_code = esp_wifi_set_mode(WIFI_MODE_APSTA);

        if (error_code != ESP_OK)
        {
            break;
        }

        error_code = esp_wifi_set_storage(WIFI_STORAGE_RAM);

        if (error_code != ESP_OK)
        {
            break;
        }

        error_code = esp_event_handler_instance_register(
            WIFI_EVENT, ESP_EVENT_ANY_ID,
            wifi_connection_on_wifi_event, NULL, NULL);

        if (error_code != ESP_OK)
        {
            break;
        }

        error_code = esp_event_handler_instance_register(
            IP_EVENT, IP_EVENT_STA_GOT_IP,
            wifi_connection_on_ip_event, NULL, NULL);

        if (error_code != ESP_OK)
        {
            break;
        }

        const esp_timer_create_args_t timer_cfg = {
            .callback = wifi_connection_reconnect_cb,
            .name     = "wifi_reconnect",
        };

        /** @brief Reconnect timer — one-shot, re-armed on each disconnect. */
        error_code = esp_timer_create(&timer_cfg, &wifi_connection_reconnect_timer);

        if (error_code != ESP_OK)
        {
            ESP_LOGE(wifi_connection_tag, "esp_timer_create failed: %s",
                     esp_err_to_name(error_code));
            break;
        }

        error_code = esp_wifi_start();

        if (error_code != ESP_OK)
        {
            break;
        }

        error_code = esp_wifi_set_ps(WIFI_PS_NONE);

        if (error_code != ESP_OK)
        {
            break;
        }

        esp_wifi_set_max_tx_power(84);

        xTaskCreatePinnedToCore(
            wifi_connection_net_manager_task,
            "net_manager",
            WIFI_CONNECTION_TASK_STACK,
            NULL,
            WIFI_CONNECTION_TASK_PRIO,
            NULL,
            WIFI_CONNECTION_CORE);

        ESP_LOGI(
            wifi_connection_tag,
            "WiFi initialized, net_manager running on Core %d",
            WIFI_CONNECTION_CORE);
    }
    while (0);

    return error_code;
}

esp_err_t wifi_connection_start(void)
{
    char ssid[WIFI_SSID_LEN] = {0};
    char pass[WIFI_PASS_LEN] = {0};
    esp_err_t error_code     = ESP_OK;

    do
    {
        if (wifi_connection_nvs_load(ssid, sizeof(ssid), pass, sizeof(pass)) == ESP_OK
            && ssid[0])
        {
            strlcpy(wifi_connection_ssid, ssid, sizeof(wifi_connection_ssid));
            strlcpy(wifi_connection_pass, pass, sizeof(wifi_connection_pass));
            wifi_connection_state_set(WIFI_CONN_STATE_CONNECTING);

            error_code = net_wifi_sta_connect(wifi_connection_ssid, wifi_connection_pass);
            break;
        }

        ESP_LOGW(wifi_connection_tag,
                 "No credentials — starting provisioning portal");
        wifi_connection_state_set(WIFI_CONN_STATE_PROVISIONING);

        error_code = wifi_ap_start(
            WIFI_AP_DEFAULT_SSID, WIFI_AP_DEFAULT_PASSWORD);

        if (error_code != ESP_OK)
        {
            break;
        }

        error_code = wifi_provisioning_start();

        if (error_code != ESP_OK)
        {
            break;
        }

        wifi_connection_pub_post(WIFI_CONN_EVENT_PROVISIONING_STARTED, NULL);
    }
    while (0);

    return error_code;
}

esp_err_t wifi_connection_set_credentials(const char* ssid, const char* password)
{
    /* Called from wifi_provisioning (httpd task, Core 0) — post to int queue
     * so net_manager handles the state transition safely.               */
    wifi_connection_int_evt_t evt = {.type = WIFI_CONNECTION_INT_EVT_SET_CREDENTIALS};
    strlcpy(evt.data.credentials.ssid, ssid, sizeof(evt.data.credentials.ssid));
    strlcpy(evt.data.credentials.pass, password, sizeof(evt.data.credentials.pass));
    return xQueueSend(wifi_connection_int_queue, &evt, pdMS_TO_TICKS(100)) == pdTRUE ?
               ESP_OK :
               ESP_ERR_TIMEOUT;
}

esp_err_t wifi_connection_forget(void)
{
    ESP_LOGW(wifi_connection_tag, "Erasing credentials, rebooting...");
    wifi_connection_nvs_erase();
    esp_restart();
    return ESP_OK; /* unreachable */
}

wifi_conn_state_t wifi_connection_get_state(void)
{
    xSemaphoreTake(wifi_connection_mutex, portMAX_DELAY);
    wifi_conn_state_t st = wifi_connection_state;
    xSemaphoreGive(wifi_connection_mutex);
    return st;
}

const char* wifi_connection_state_str(wifi_conn_state_t st)
{
    switch (st)
    {
        case WIFI_CONN_STATE_IDLE:
            return "IDLE";
        case WIFI_CONN_STATE_PROVISIONING:
            return "PROVISIONING";
        case WIFI_CONN_STATE_CONNECTING:
            return "CONNECTING";
        case WIFI_CONN_STATE_CONNECTED:
            return "CONNECTED";
        case WIFI_CONN_STATE_RECONNECTING:
            return "RECONNECTING";
        default:
            return "UNKNOWN";
    }
}

esp_err_t wifi_connection_get_ssid(char* buf, size_t len)
{
    strlcpy(buf, wifi_connection_ssid, len);
    return ESP_OK;
}

esp_ip4_addr_t wifi_connection_get_ip(void)
{
    return wifi_connection_ip;
}

void wifi_connection_register_state_cb(wifi_connection_state_cb_t cb)
{
    wifi_connection_state_cb = cb;
}

QueueHandle_t wifi_connection_get_event_queue(void)
{
    return wifi_connection_pub_queue;
}

/**** END OF FILE ****/