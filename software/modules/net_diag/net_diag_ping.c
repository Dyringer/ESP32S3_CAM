/**
 ******************************************************************************
 * @file    net_diag_ping.c
 * @author  Daniel Dyringer
 * @date    2026-10-13
 * @brief   Network diagnostics — ICMP ping (IPv4).
 *
 *          The esp_ping session runs on its own task and reports through
 *          callbacks; the worker task only resolves, starts and deletes it.
 ******************************************************************************
 */


/* Includes ---------------------------------------------------------------- */
#include <stdbool.h>
#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "lwip/inet.h"
#include "lwip/netdb.h"
#include "net_diag.h"
#include "net_diag_private.h"
#include "ping/ping_sock.h"
#include "utils.h"


/* Constants and macros ---------------------------------------------------- */
// clang-format off

#define NET_DIAG_PING_LOCK_TIMEOUT_MS   100u

// clang-format on

/* Private types ----------------------------------------------------------- */


/* Private variables ------------------------------------------------------- */

static const char* net_diag_ping_tag = "net_diag_ping";

/* Guarded by net_diag_ping_lock (worker and esp_ping tasks) */
static SemaphoreHandle_t net_diag_ping_lock        = NULL;
static net_diag_ping_result_t net_diag_ping_result = {0};
static bool net_diag_ping_ended                    = false;

/* Touched only by the worker task */
static esp_ping_handle_t net_diag_ping_session = NULL;


/* Private functions ------------------------------------------------------- */

static void net_diag_ping_update_transmitted(esp_ping_handle_t session)
{
    uint32_t transmitted = 0u;

    esp_ping_get_profile(session, ESP_PING_PROF_REQUEST, &transmitted, sizeof(transmitted));
    net_diag_ping_result.transmitted = (uint16_t)transmitted;
}

/* ---- esp_ping callbacks (esp_ping task) ---- */

static void net_diag_ping_on_success(esp_ping_handle_t session, void* arg)
{
    ARG_UNUSED(arg);

    uint32_t elapsed_ms = 0u;

    esp_ping_get_profile(session, ESP_PING_PROF_TIMEGAP, &elapsed_ms, sizeof(elapsed_ms));

    xSemaphoreTake(net_diag_ping_lock, portMAX_DELAY);

    if (net_diag_ping_result.received == 0u)
    {
        net_diag_ping_result.min_ms = elapsed_ms;
        net_diag_ping_result.max_ms = elapsed_ms;
    }

    net_diag_ping_result.min_ms = MIN(net_diag_ping_result.min_ms, elapsed_ms);
    net_diag_ping_result.max_ms = MAX(net_diag_ping_result.max_ms, elapsed_ms);
    net_diag_ping_result.total_ms += elapsed_ms;
    net_diag_ping_result.last_ms = elapsed_ms;
    net_diag_ping_result.received++;
    net_diag_ping_update_transmitted(session);

    xSemaphoreGive(net_diag_ping_lock);
}

static void net_diag_ping_on_timeout(esp_ping_handle_t session, void* arg)
{
    ARG_UNUSED(arg);

    xSemaphoreTake(net_diag_ping_lock, portMAX_DELAY);
    net_diag_ping_update_transmitted(session);
    xSemaphoreGive(net_diag_ping_lock);
}

/* The session can't delete itself from its own task: the worker does it. */
static void net_diag_ping_on_end(esp_ping_handle_t session, void* arg)
{
    ARG_UNUSED(arg);

    xSemaphoreTake(net_diag_ping_lock, portMAX_DELAY);
    net_diag_ping_update_transmitted(session);
    net_diag_ping_result.state = NET_DIAG_STATE_DONE;
    net_diag_ping_ended        = true;
    xSemaphoreGive(net_diag_ping_lock);

    const net_diag_job_t job = {.type = NET_DIAG_JOB_PING_CLEANUP};

    /* On a full queue the next ping run deletes the session instead. */
    net_diag_submit(&job);
}

/* ---- Worker ---- */

static void net_diag_ping_close(void)
{
    if (net_diag_ping_session != NULL)
    {
        esp_ping_stop(net_diag_ping_session);
        esp_ping_delete_session(net_diag_ping_session);
        net_diag_ping_session = NULL;
    }
}

static esp_err_t net_diag_ping_resolve(
    const char* host,
    ip_addr_t* target,
    char* address,
    size_t address_size)
{
    esp_err_t error_code         = ESP_OK;
    const struct addrinfo hints  = {.ai_family = AF_INET};
    struct addrinfo* information = NULL;

    if ((getaddrinfo(host, NULL, &hints, &information) != 0) || (information == NULL))
    {
        error_code = ESP_ERR_NOT_FOUND;
    }
    else
    {
        const struct sockaddr_in* const address4 =
            (const struct sockaddr_in*)information->ai_addr;

        inet_addr_to_ip4addr(ip_2_ip4(target), &address4->sin_addr);
        IP_SET_TYPE_VAL(*target, IPADDR_TYPE_V4);
        inet_ntoa_r(address4->sin_addr, address, address_size);
    }

    freeaddrinfo(information);

    return error_code;
}

static esp_err_t net_diag_ping_open(const net_diag_ping_config_t* config, ip_addr_t target)
{
    esp_ping_config_t ping_config = ESP_PING_DEFAULT_CONFIG();

    ping_config.target_addr = target;
    ping_config.count       = config->count;
    ping_config.data_size   = config->size;
    ping_config.interval_ms = config->interval_ms;
    ping_config.timeout_ms  = config->timeout_ms;

    const esp_ping_callbacks_t callbacks = {
        .cb_args         = NULL,
        .on_ping_success = net_diag_ping_on_success,
        .on_ping_timeout = net_diag_ping_on_timeout,
        .on_ping_end     = net_diag_ping_on_end,
    };

    esp_err_t error_code =
        esp_ping_new_session(&ping_config, &callbacks, &net_diag_ping_session);

    if (error_code == ESP_OK)
    {
        error_code = esp_ping_start(net_diag_ping_session);
    }

    return error_code;
}

static void net_diag_ping_fail(esp_err_t error)
{
    xSemaphoreTake(net_diag_ping_lock, portMAX_DELAY);
    net_diag_ping_result.error = error;
    net_diag_ping_result.state = NET_DIAG_STATE_FAILED;
    xSemaphoreGive(net_diag_ping_lock);
}


/* Exposed API ------------------------------------------------------------- */

esp_err_t net_diag_ping_init(void)
{
    esp_err_t error_code = ESP_OK;

    net_diag_ping_lock = xSemaphoreCreateMutex();

    if (net_diag_ping_lock == NULL)
    {
        error_code = ESP_ERR_NO_MEM;
    }

    return error_code;
}

void net_diag_ping_run(const net_diag_ping_config_t* config)
{
    ip_addr_t target                   = {0};
    char address[NET_DIAG_ADDRESS_LEN] = {0};

    net_diag_ping_close();

    xSemaphoreTake(net_diag_ping_lock, portMAX_DELAY);
    net_diag_ping_ended = false;
    xSemaphoreGive(net_diag_ping_lock);

    esp_err_t error_code =
        net_diag_ping_resolve(config->host, &target, address, sizeof(address));

    if (error_code == ESP_OK)
    {
        xSemaphoreTake(net_diag_ping_lock, portMAX_DELAY);
        strlcpy(
            net_diag_ping_result.address, address, sizeof(net_diag_ping_result.address));
        xSemaphoreGive(net_diag_ping_lock);

        error_code = net_diag_ping_open(config, target);
    }

    if (error_code != ESP_OK)
    {
        ESP_LOGW(
            net_diag_ping_tag,
            "Ping %s failed: %s",
            config->host,
            esp_err_to_name(error_code));
        net_diag_ping_close();
        net_diag_ping_fail(error_code);
    }
}

/* A new run may have replaced the session since the cleanup job was queued:
 * only a session that reported its end is deleted. */
void net_diag_ping_cleanup(void)
{
    xSemaphoreTake(net_diag_ping_lock, portMAX_DELAY);
    const bool ended    = net_diag_ping_ended;
    net_diag_ping_ended = false;
    xSemaphoreGive(net_diag_ping_lock);

    if (ended)
    {
        net_diag_ping_close();
    }
}

esp_err_t net_diag_ping_start(const net_diag_ping_config_t* config)
{
    esp_err_t error_code = ESP_OK;

    do
    {
        if ((config == NULL) || (config->host[0] == '\0')
            || (strnlen(config->host, sizeof(config->host)) >= sizeof(config->host))
            || (config->count == 0u) || (config->count > NET_DIAG_PING_MAX_COUNT)
            || (config->size < NET_DIAG_PING_MIN_SIZE)
            || (config->size > NET_DIAG_PING_MAX_SIZE)
            || (config->interval_ms < NET_DIAG_PING_MIN_INTERVAL_MS)
            || (config->interval_ms > NET_DIAG_PING_MAX_INTERVAL_MS)
            || (config->timeout_ms < NET_DIAG_PING_MIN_TIMEOUT_MS)
            || (config->timeout_ms > NET_DIAG_PING_MAX_TIMEOUT_MS))
        {
            error_code = ESP_ERR_INVALID_ARG;
            break;
        }

        if (net_diag_ping_lock == NULL)
        {
            error_code = ESP_ERR_INVALID_STATE;
            break;
        }

        xSemaphoreTake(net_diag_ping_lock, portMAX_DELAY);

        if (net_diag_ping_result.state == NET_DIAG_STATE_RUNNING)
        {
            error_code = ESP_ERR_INVALID_STATE;
        }
        else
        {
            const net_diag_job_t job = {
                .type        = NET_DIAG_JOB_PING,
                .config.ping = *config,
            };

            error_code = net_diag_submit(&job);

            if (error_code == ESP_OK)
            {
                memset(&net_diag_ping_result, 0, sizeof(net_diag_ping_result));
                strlcpy(
                    net_diag_ping_result.host,
                    config->host,
                    sizeof(net_diag_ping_result.host));
                net_diag_ping_result.count       = config->count;
                net_diag_ping_result.size        = config->size;
                net_diag_ping_result.interval_ms = config->interval_ms;
                net_diag_ping_result.timeout_ms  = config->timeout_ms;
                net_diag_ping_result.state       = NET_DIAG_STATE_RUNNING;
            }
        }

        xSemaphoreGive(net_diag_ping_lock);
    } while (0);

    return error_code;
}

esp_err_t net_diag_ping_get(net_diag_ping_result_t* result)
{
    esp_err_t error_code = ESP_OK;

    if ((result == NULL) || (net_diag_ping_lock == NULL))
    {
        error_code = ESP_ERR_INVALID_STATE;
    }
    else if (xSemaphoreTake(net_diag_ping_lock, pdMS_TO_TICKS(NET_DIAG_PING_LOCK_TIMEOUT_MS)) != pdTRUE)
    {
        error_code = ESP_ERR_TIMEOUT;
    }
    else
    {
        *result = net_diag_ping_result;
        xSemaphoreGive(net_diag_ping_lock);
    }

    return error_code;
}

/**** END OF FILE ****/
