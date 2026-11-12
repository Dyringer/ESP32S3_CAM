/**
 ******************************************************************************
 * @file    wifi_connection.c
 * @author  Daniel Dyringer
 * @date    2026-02-17
 * @brief   WiFi connection manager — owns the STA/AP state machine,
 *          credential persistence (network_settings) and reconnection logic.
 *
 *          All state transitions happen on the net_manager task (Core 0);
 *          WiFi/IP event handlers and other tasks only post to its queue.
 ******************************************************************************
 */


/* Includes ---------------------------------------------------------------- */
#include "wifi_connection.h"

#include <ctype.h>
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
#include "net_wifi_ap.h"
#include "net_wifi_sta.h"
#include "network_settings.h"
#include "utils.h"
#include "wifi_provisioning.h"
#include "wifi_types.h"
#include "work_queue.h"


/* Constants and macros ---------------------------------------------------- */
// clang-format off

#define WIFI_CONNECTION_RECONNECT_DELAY_MIN_US  (1000ULL  * 1000ULL)
#define WIFI_CONNECTION_RECONNECT_DELAY_MAX_US  (60000ULL * 1000ULL)

#define WIFI_CONNECTION_PASSPHRASE_MIN          8u
#define WIFI_CONNECTION_SET_CREDENTIALS_WAIT_MS 100u

#define WIFI_CONNECTION_MESSAGE_QUEUE_LEN       16u
#define WIFI_CONNECTION_TASK_STACK              4096u
#define WIFI_CONNECTION_TASK_PRIO               10u

// clang-format on

/* Private types ----------------------------------------------------------- */

typedef enum wifi_connection_message_type_e
{
    WIFI_CONNECTION_MESSAGE_TYPE_STA_CONNECTED          = 0,
    WIFI_CONNECTION_MESSAGE_TYPE_STA_DISCONNECTED       = 1,
    WIFI_CONNECTION_MESSAGE_TYPE_GOT_IP                 = 2,
    WIFI_CONNECTION_MESSAGE_TYPE_AP_CLIENT_CONNECTED    = 3,
    WIFI_CONNECTION_MESSAGE_TYPE_AP_CLIENT_DISCONNECTED = 4,
    WIFI_CONNECTION_MESSAGE_TYPE_SET_CREDENTIALS        = 5,
    WIFI_CONNECTION_MESSAGE_TYPE_CONNECT                = 6,
} wifi_connection_message_type_t;

typedef struct wifi_connection_message_s
{
    wifi_connection_message_type_t type;
    union
    {
        struct
        {
            uint16_t reason;
        } disconnected;
        struct
        {
            esp_ip4_addr_t ip;
        } got_ip;
        struct
        {
            uint8_t mac[WIFI_TYPES_MAC_LEN];
        } client;
        struct
        {
            char ssid[WIFI_TYPES_SSID_LEN];
            char password[WIFI_TYPES_PASS_LEN];
        } credentials;
    } data;
} wifi_connection_message_t;


/* Private variables ------------------------------------------------------- */

static const char* wifi_connection_tag = "wifi_conn";

ESP_EVENT_DEFINE_BASE(WIFI_CONNECTION_EVENTS);

static SemaphoreHandle_t wifi_connection_mutex            = NULL;
static QueueHandle_t wifi_connection_message_queue        = NULL;
static QueueHandle_t wifi_connection_event_queue          = NULL;
static esp_timer_handle_t wifi_connection_reconnect_timer = NULL;

/* Guarded by wifi_connection_mutex; written only by net_manager. */
static wifi_connection_state_t wifi_connection_state = WIFI_CONNECTION_STATE_IDLE;

/* Touched only by net_manager */
static uint64_t wifi_connection_reconnect_delay_us = WIFI_CONNECTION_RECONNECT_DELAY_MIN_US;
static char wifi_connection_ssid[WIFI_TYPES_SSID_LEN]     = {0};
static char wifi_connection_password[WIFI_TYPES_PASS_LEN] = {0};
static bool wifi_connection_credentials_unsaved           = false;
static bool wifi_connection_ignore_leave                  = false;
static esp_ip4_addr_t wifi_connection_ip                  = {0};

static wifi_connection_state_cb_t wifi_connection_state_callback = NULL;

static work_queue_item_t wifi_connection_restart_work = {0};


/* Private functions ------------------------------------------------------- */

static void wifi_connection_state_set(wifi_connection_state_t state)
{
    xSemaphoreTake(wifi_connection_mutex, portMAX_DELAY);
    wifi_connection_state = state;
    xSemaphoreGive(wifi_connection_mutex);

    ESP_LOGI(wifi_connection_tag, "State → %s", wifi_connection_state_str(state));

    if (wifi_connection_state_callback != NULL)
    {
        wifi_connection_state_callback(state);
    }

    /* Zero timeout: a full event queue must not stall net_manager. */
    const esp_err_t post_error = esp_event_post(
        WIFI_CONNECTION_EVENTS,
        WIFI_CONNECTION_NOTIFY_STATE_CHANGED,
        &state,
        sizeof(state),
        0u);

    if (post_error != ESP_OK)
    {
        ESP_LOGW(
            wifi_connection_tag, "State event dropped: %s", esp_err_to_name(post_error));
    }
}

static void wifi_connection_event_post(wifi_connection_event_id_t id, const esp_ip4_addr_t* ip)
{
    wifi_connection_event_t event = {.id = id};

    if (ip != NULL)
    {
        event.data.ip = *ip;
    }

    /* Nobody has to drain this queue; when it is full the event is dropped. */
    xQueueSend(wifi_connection_event_queue, &event, 0u);
}

/* ---- Settings ---- */

/* data_manager_save() also commits any other pending setting. */
static esp_err_t wifi_connection_credentials_save(void)
{
    esp_err_t error_code = data_manager_set(
        NETWORK_WIFI_SSID, wifi_connection_ssid, strlen(wifi_connection_ssid) + 1u);

    if (error_code == ESP_OK)
    {
        error_code = data_manager_set(
            NETWORK_WIFI_PASSWORD,
            wifi_connection_password,
            strlen(wifi_connection_password) + 1u);
    }

    if (error_code == ESP_OK)
    {
        error_code = data_manager_save();
    }

    return error_code;
}

static void wifi_connection_restart_handler(void* context)
{
    ARG_UNUSED(context);

    esp_restart();
}

/* ---- WiFi/IP event handlers and reconnect timer (post only) ---- */

/* Post only: the WiFi calls block and the credentials belong to net_manager.
 * Runs in the esp_timer task. */
static void wifi_connection_reconnect_cb(void* arg)
{
    ARG_UNUSED(arg);

    const wifi_connection_message_t message = {
        .type = WIFI_CONNECTION_MESSAGE_TYPE_CONNECT,
    };

    /* A dropped post would end the retry chain for good. */
    if (xQueueSend(wifi_connection_message_queue, &message, 0u) != pdTRUE)
    {
        esp_timer_start_once(
            wifi_connection_reconnect_timer, WIFI_CONNECTION_RECONNECT_DELAY_MIN_US);
    }
}

static void wifi_connection_on_wifi_event(void* arg, esp_event_base_t base, int32_t id, void* data)
{
    ARG_UNUSED(arg);
    ARG_UNUSED(base);

    wifi_connection_message_t message = {0};
    bool post                         = true;

    switch ((wifi_event_t)id)
    {
        case WIFI_EVENT_STA_CONNECTED:
        {
            message.type = WIFI_CONNECTION_MESSAGE_TYPE_STA_CONNECTED;
        }
        break;
        case WIFI_EVENT_STA_DISCONNECTED:
        {
            const wifi_event_sta_disconnected_t* event = data;

            message.type = WIFI_CONNECTION_MESSAGE_TYPE_STA_DISCONNECTED;
            message.data.disconnected.reason = (uint16_t)event->reason;
        }
        break;
        case WIFI_EVENT_AP_STACONNECTED:
        {
            const wifi_event_ap_staconnected_t* event = data;

            message.type = WIFI_CONNECTION_MESSAGE_TYPE_AP_CLIENT_CONNECTED;
            memcpy(message.data.client.mac, event->mac, WIFI_TYPES_MAC_LEN);
        }
        break;
        case WIFI_EVENT_AP_STADISCONNECTED:
        {
            const wifi_event_ap_stadisconnected_t* event = data;

            message.type = WIFI_CONNECTION_MESSAGE_TYPE_AP_CLIENT_DISCONNECTED;
            memcpy(message.data.client.mac, event->mac, WIFI_TYPES_MAC_LEN);
        }
        break;
        default:
        {
            post = false;
        }
        break;
    }

    if (post)
    {
        xQueueSendFromISR(wifi_connection_message_queue, &message, NULL);
    }
}

static void wifi_connection_on_ip_event(void* arg, esp_event_base_t base, int32_t id, void* data)
{
    ARG_UNUSED(arg);
    ARG_UNUSED(base);

    if (id == IP_EVENT_STA_GOT_IP)
    {
        const ip_event_got_ip_t* event          = data;
        const wifi_connection_message_t message = {
            .type           = WIFI_CONNECTION_MESSAGE_TYPE_GOT_IP,
            .data.got_ip.ip = event->ip_info.ip,
        };

        xQueueSendFromISR(wifi_connection_message_queue, &message, NULL);
    }
}

/* ---- net_manager task — Core 0 ---- */

static void wifi_connection_schedule_retry(void)
{
    ESP_LOGW(
        wifi_connection_tag,
        "Retrying in %" PRIu64 " ms",
        wifi_connection_reconnect_delay_us / 1000u);

    esp_timer_stop(wifi_connection_reconnect_timer);

    const esp_err_t error_code = esp_timer_start_once(
        wifi_connection_reconnect_timer, wifi_connection_reconnect_delay_us);

    if (error_code != ESP_OK)
    {
        ESP_LOGE(
            wifi_connection_tag,
            "Retry timer start failed: %s",
            esp_err_to_name(error_code));
    }

    wifi_connection_reconnect_delay_us = MIN(
        wifi_connection_reconnect_delay_us * 2u, WIFI_CONNECTION_RECONNECT_DELAY_MAX_US);
}

/* A synchronous failure produces no DISCONNECTED event, so it has to re-arm
 * the retry itself or the device stays RECONNECTING for good. */
static void wifi_connection_connect_now(void)
{
    const esp_err_t error_code =
        net_wifi_sta_connect(wifi_connection_ssid, wifi_connection_password);

    if (error_code != ESP_OK)
    {
        ESP_LOGW(wifi_connection_tag, "Connect failed: %s", esp_err_to_name(error_code));
        wifi_connection_schedule_retry();
    }
}

static void wifi_connection_handle_sta_connected(void)
{
    wifi_connection_ignore_leave = false;
    ESP_LOGI(wifi_connection_tag, "STA associated, waiting for IP...");
}

static void wifi_connection_handle_sta_disconnected(uint16_t reason)
{
    /* Our own disconnect before switching networks, not a link loss — the new
     * connect is already under way. */
    if (wifi_connection_ignore_leave && (reason == (uint16_t)WIFI_REASON_ASSOC_LEAVE))
    {
        wifi_connection_ignore_leave = false;
    }
    else
    {
        ESP_LOGW(wifi_connection_tag, "STA disconnected (reason=%u)", (unsigned)reason);
        wifi_connection_state_set(WIFI_CONNECTION_STATE_RECONNECTING);
        wifi_connection_event_post(WIFI_CONNECTION_EVENT_ID_DISCONNECTED, NULL);
        wifi_connection_schedule_retry();
    }
}

static void wifi_connection_handle_connect(void)
{
    if (wifi_connection_state != WIFI_CONNECTION_STATE_CONNECTED)
    {
        wifi_connection_connect_now();
    }
}

static void wifi_connection_handle_got_ip(esp_ip4_addr_t ip)
{
    ESP_LOGI(wifi_connection_tag, "IP acquired: " IPSTR, IP2STR(&ip));

    esp_timer_stop(wifi_connection_reconnect_timer);
    wifi_connection_reconnect_delay_us = WIFI_CONNECTION_RECONNECT_DELAY_MIN_US;
    wifi_connection_ip                 = ip;

    /* Saved only once proven, so a mistyped password never outlives a reboot. */
    if (wifi_connection_credentials_unsaved)
    {
        const esp_err_t error_code = wifi_connection_credentials_save();

        if (error_code == ESP_OK)
        {
            wifi_connection_credentials_unsaved = false;
            ESP_LOGI(wifi_connection_tag, "Credentials saved");
        }
        else
        {
            ESP_LOGE(
                wifi_connection_tag,
                "Saving credentials failed: %s",
                esp_err_to_name(error_code));
        }
    }

    /* The panel listens on every interface and starts on CONNECTED, so the
     * open setup AP has to be gone first. */
    net_wifi_ap_stop();

    wifi_connection_state_set(WIFI_CONNECTION_STATE_CONNECTED);
    wifi_connection_event_post(WIFI_CONNECTION_EVENT_ID_CONNECTED, &ip);
}

static void wifi_connection_handle_ap_client(const uint8_t* mac, const char* change)
{
    ESP_LOGI(wifi_connection_tag, "Client %s " MACSTR, change, MAC2STR(mac));
}

static void wifi_connection_handle_set_credentials(const char* ssid, const char* password)
{
    ESP_LOGI(wifi_connection_tag, "Applying credentials for '%s'", ssid);

    esp_timer_stop(wifi_connection_reconnect_timer);
    strlcpy(wifi_connection_ssid, ssid, sizeof(wifi_connection_ssid));
    strlcpy(wifi_connection_password, password, sizeof(wifi_connection_password));
    wifi_connection_credentials_unsaved = true;

    /* The portal's reply went out before this message was posted, and the
     * portal never comes back at runtime. */
    wifi_provisioning_stop();
    net_wifi_ap_stop();

    /* esp_wifi_connect() needs the old link gone first. */
    if ((wifi_connection_state == WIFI_CONNECTION_STATE_CONNECTED)
        || (wifi_connection_state == WIFI_CONNECTION_STATE_CONNECTING))
    {
        wifi_connection_ignore_leave = (net_wifi_sta_disconnect() == ESP_OK);
    }

    wifi_connection_reconnect_delay_us = WIFI_CONNECTION_RECONNECT_DELAY_MIN_US;
    wifi_connection_state_set(WIFI_CONNECTION_STATE_CONNECTING);
    wifi_connection_event_post(WIFI_CONNECTION_EVENT_ID_PROVISIONED, NULL);
    wifi_connection_connect_now();
}

static void wifi_connection_handle_message(const wifi_connection_message_t* message)
{
    switch (message->type)
    {
        case WIFI_CONNECTION_MESSAGE_TYPE_STA_CONNECTED:
        {
            wifi_connection_handle_sta_connected();
        }
        break;
        case WIFI_CONNECTION_MESSAGE_TYPE_STA_DISCONNECTED:
        {
            wifi_connection_handle_sta_disconnected(message->data.disconnected.reason);
        }
        break;
        case WIFI_CONNECTION_MESSAGE_TYPE_CONNECT:
        {
            wifi_connection_handle_connect();
        }
        break;
        case WIFI_CONNECTION_MESSAGE_TYPE_GOT_IP:
        {
            wifi_connection_handle_got_ip(message->data.got_ip.ip);
        }
        break;
        case WIFI_CONNECTION_MESSAGE_TYPE_AP_CLIENT_CONNECTED:
        {
            wifi_connection_handle_ap_client(message->data.client.mac, "connected");
        }
        break;
        case WIFI_CONNECTION_MESSAGE_TYPE_AP_CLIENT_DISCONNECTED:
        {
            wifi_connection_handle_ap_client(message->data.client.mac, "disconnected");
        }
        break;
        case WIFI_CONNECTION_MESSAGE_TYPE_SET_CREDENTIALS:
        {
            wifi_connection_handle_set_credentials(
                message->data.credentials.ssid, message->data.credentials.password);
        }
        break;
        default:
        {
            /* All message types handled explicitly */
        }
        break;
    }
}

static void wifi_connection_net_manager_task(void* arg)
{
    ARG_UNUSED(arg);

    for (;;)
    {
        wifi_connection_message_t message = {0};

        if (xQueueReceive(wifi_connection_message_queue, &message, portMAX_DELAY) == pdTRUE)
        {
            wifi_connection_handle_message(&message);
        }
    }
}

static esp_err_t wifi_connection_start_provisioning(void)
{
    esp_err_t error_code = ESP_OK;

    do
    {
        ESP_LOGW(wifi_connection_tag, "No credentials — starting provisioning portal");
        wifi_connection_state_set(WIFI_CONNECTION_STATE_PROVISIONING);

        error_code =
            net_wifi_ap_start(NET_WIFI_AP_DEFAULT_SSID, NET_WIFI_AP_DEFAULT_PASSWORD);

        if (error_code != ESP_OK)
        {
            break;
        }

        error_code = wifi_provisioning_start();

        if (error_code != ESP_OK)
        {
            break;
        }

        wifi_connection_event_post(WIFI_CONNECTION_EVENT_ID_PROVISIONING_STARTED, NULL);
    } while (0);

    return error_code;
}


/* Exposed API ------------------------------------------------------------- */

esp_err_t wifi_connection_init(void)
{
    esp_err_t error_code = ESP_OK;

    do
    {
        wifi_connection_mutex         = xSemaphoreCreateMutex();
        wifi_connection_message_queue = xQueueCreate(
            WIFI_CONNECTION_MESSAGE_QUEUE_LEN, sizeof(wifi_connection_message_t));
        wifi_connection_event_queue =
            xQueueCreate(WIFI_CONNECTION_EVENT_QUEUE_LEN, sizeof(wifi_connection_event_t));

        if ((wifi_connection_mutex == NULL) || (wifi_connection_message_queue == NULL)
            || (wifi_connection_event_queue == NULL))
        {
            error_code = ESP_ERR_NO_MEM;
            break;
        }

        error_code = network_settings_init(NULL);

        if (error_code != ESP_OK)
        {
            break;
        }

        error_code = work_queue_item_init(
            &wifi_connection_restart_work, wifi_connection_restart_handler, NULL);

        if (error_code != ESP_OK)
        {
            break;
        }

        error_code = esp_netif_init();

        if (error_code != ESP_OK)
        {
            break;
        }

        esp_netif_t* sta_netif = NULL;
        esp_netif_t* ap_netif  = NULL;

        error_code = net_wifi_sta_init(&sta_netif);

        if (error_code != ESP_OK)
        {
            break;
        }

        error_code = net_wifi_ap_init(&ap_netif);

        if (error_code != ESP_OK)
        {
            break;
        }

        const wifi_init_config_t wifi_init_config = WIFI_INIT_CONFIG_DEFAULT();

        error_code = esp_wifi_init(&wifi_init_config);

        if (error_code != ESP_OK)
        {
            break;
        }

        /* AP is added only for the provisioning portal (net_wifi_ap_start). */
        error_code = esp_wifi_set_mode(WIFI_MODE_STA);

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
            WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_connection_on_wifi_event, NULL, NULL);

        if (error_code != ESP_OK)
        {
            break;
        }

        error_code = esp_event_handler_instance_register(
            IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_connection_on_ip_event, NULL, NULL);

        if (error_code != ESP_OK)
        {
            break;
        }

        const esp_timer_create_args_t timer_config = {
            .callback = wifi_connection_reconnect_cb,
            .name     = "wifi_reconnect",
        };

        error_code = esp_timer_create(&timer_config, &wifi_connection_reconnect_timer);

        if (error_code != ESP_OK)
        {
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

        const esp_err_t power_result =
            esp_wifi_set_max_tx_power((int8_t)WIFI_TYPES_MAX_TX_POWER);

        if (power_result != ESP_OK)
        {
            ESP_LOGW(
                wifi_connection_tag,
                "Setting TX power failed: %s",
                esp_err_to_name(power_result));
        }

        const BaseType_t task_created = xTaskCreatePinnedToCore(
            wifi_connection_net_manager_task,
            "net_manager",
            WIFI_CONNECTION_TASK_STACK,
            NULL,
            WIFI_CONNECTION_TASK_PRIO,
            NULL,
            WIFI_CONNECTION_CORE);

        if (task_created != pdPASS)
        {
            error_code = ESP_ERR_NO_MEM;
            break;
        }

        ESP_LOGI(
            wifi_connection_tag,
            "WiFi initialized, net_manager running on Core %d",
            WIFI_CONNECTION_CORE);
    } while (0);

    if (error_code != ESP_OK)
    {
        ESP_LOGE(wifi_connection_tag, "Init failed: %s", esp_err_to_name(error_code));
    }

    return error_code;
}

esp_err_t wifi_connection_start(void)
{
    esp_err_t error_code               = ESP_OK;
    char ssid[WIFI_TYPES_SSID_LEN]     = {0};
    char password[WIFI_TYPES_PASS_LEN] = {0};
    esp_err_t load_result = data_manager_get(NETWORK_WIFI_SSID, ssid, sizeof(ssid));

    if (load_result == ESP_OK)
    {
        load_result = data_manager_get(NETWORK_WIFI_PASSWORD, password, sizeof(password));
    }

    if ((load_result == ESP_OK) && (ssid[0] != '\0'))
    {
        strlcpy(wifi_connection_ssid, ssid, sizeof(wifi_connection_ssid));
        strlcpy(wifi_connection_password, password, sizeof(wifi_connection_password));
        wifi_connection_state_set(WIFI_CONNECTION_STATE_CONNECTING);

        /* net_manager retries if this first attempt fails. */
        const wifi_connection_message_t message = {
            .type = WIFI_CONNECTION_MESSAGE_TYPE_CONNECT,
        };

        if (xQueueSend(wifi_connection_message_queue, &message, 0u) != pdTRUE)
        {
            error_code = ESP_ERR_NO_MEM;
        }
    }
    else
    {
        error_code = wifi_connection_start_provisioning();
    }

    return error_code;
}

bool wifi_connection_credentials_valid(const char* ssid, const char* password)
{
    const size_t ssid_length     = (ssid != NULL) ? strlen(ssid) : 0u;
    const size_t password_length = (password != NULL) ? strlen(password) : 0u;
    bool valid = ((ssid_length > 0u) && (ssid_length < WIFI_TYPES_SSID_LEN));

    if (password_length == (WIFI_TYPES_PASS_LEN - 1u))
    {
        size_t i = 0u;

        while (valid && (i < password_length))
        {
            valid = (isxdigit((uint8_t)password[i]) != 0);
            i++;
        }
    }
    else if (
        (password_length != 0u)
        && ((password_length < WIFI_CONNECTION_PASSPHRASE_MIN)
            || (password_length > (WIFI_TYPES_PASS_LEN - 2u))))
    {
        valid = false;
    }
    else
    {
        /* Open network or a passphrase of valid length */
    }

    return valid;
}

esp_err_t wifi_connection_set_credentials(const char* ssid, const char* password)
{
    esp_err_t error_code = ESP_OK;

    do
    {
        if (!wifi_connection_credentials_valid(ssid, password))
        {
            error_code = ESP_ERR_INVALID_ARG;
            break;
        }

        /* NULL if wifi_connection_init failed; boot carries on without WiFi. */
        if (wifi_connection_message_queue == NULL)
        {
            error_code = ESP_ERR_INVALID_STATE;
            break;
        }

        wifi_connection_message_t message = {
            .type = WIFI_CONNECTION_MESSAGE_TYPE_SET_CREDENTIALS,
        };

        strlcpy(message.data.credentials.ssid, ssid, sizeof(message.data.credentials.ssid));
        strlcpy(
            message.data.credentials.password,
            (password != NULL) ? password : "",
            sizeof(message.data.credentials.password));

        if (xQueueSend(
                wifi_connection_message_queue,
                &message,
                pdMS_TO_TICKS(WIFI_CONNECTION_SET_CREDENTIALS_WAIT_MS))
            != pdTRUE)
        {
            error_code = ESP_ERR_TIMEOUT;
        }
    } while (0);

    return error_code;
}

/* The work_queue task runs items in push order, so the restart pushed here
 * follows the data_manager work that erases the credentials. */
esp_err_t wifi_connection_forget(void)
{
    esp_err_t error_code = network_settings_restore_defaults();

    if (error_code == ESP_OK)
    {
        error_code = work_queue_push(&wifi_connection_restart_work);
    }

    if (error_code == ESP_OK)
    {
        ESP_LOGW(wifi_connection_tag, "Credentials erased, rebooting...");
    }
    else
    {
        ESP_LOGE(
            wifi_connection_tag,
            "Erasing credentials failed: %s — not rebooting",
            esp_err_to_name(error_code));
    }

    return error_code;
}

wifi_connection_state_t wifi_connection_get_state(void)
{
    wifi_connection_state_t state = WIFI_CONNECTION_STATE_IDLE;

    /* NULL if wifi_connection_init failed; boot carries on without WiFi. */
    if (wifi_connection_mutex != NULL)
    {
        xSemaphoreTake(wifi_connection_mutex, portMAX_DELAY);
        state = wifi_connection_state;
        xSemaphoreGive(wifi_connection_mutex);
    }

    return state;
}

const char* wifi_connection_state_str(wifi_connection_state_t state)
{
    const char* name = NULL;

    switch (state)
    {
        case WIFI_CONNECTION_STATE_IDLE:
        {
            name = "IDLE";
        }
        break;
        case WIFI_CONNECTION_STATE_PROVISIONING:
        {
            name = "PROVISIONING";
        }
        break;
        case WIFI_CONNECTION_STATE_CONNECTING:
        {
            name = "CONNECTING";
        }
        break;
        case WIFI_CONNECTION_STATE_CONNECTED:
        {
            name = "CONNECTED";
        }
        break;
        case WIFI_CONNECTION_STATE_RECONNECTING:
        {
            name = "RECONNECTING";
        }
        break;
        default:
        {
            name = "UNKNOWN";
        }
        break;
    }

    return name;
}

esp_err_t wifi_connection_get_ssid(char* buffer, size_t length)
{
    esp_err_t error_code = ESP_OK;

    if ((buffer == NULL) || (length == 0u))
    {
        error_code = ESP_ERR_INVALID_ARG;
    }
    else
    {
        strlcpy(buffer, wifi_connection_ssid, length);
    }

    return error_code;
}

esp_ip4_addr_t wifi_connection_get_ip(void)
{
    return wifi_connection_ip;
}

void wifi_connection_register_state_cb(wifi_connection_state_cb_t callback)
{
    wifi_connection_state_callback = callback;
}

QueueHandle_t wifi_connection_get_event_queue(void)
{
    return wifi_connection_event_queue;
}

/**** END OF FILE ****/
