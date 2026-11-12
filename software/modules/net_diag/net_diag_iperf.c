/**
 ******************************************************************************
 * @file    net_diag_iperf.c
 * @author  Daniel Dyringer
 * @date    2026-10-13
 * @brief   Network diagnostics — iperf2 server/client (espressif/iperf).
 *
 *          Results are captured by overriding the component's weak
 *          iperf_report_output(), so nothing is printed to stdout.
 *
 * @note    espressif/iperf 1.0.3 pins its traffic task to the last core
 *          (core 1) and a TCP server gives up when no client connects within
 *          5 s (IPERF_SOCKET_ACCEPT_TIMEOUT); a UDP server waits 10 s.
 ******************************************************************************
 */


/* Includes ---------------------------------------------------------------- */
#include <stdbool.h>
#include <string.h>

#include "esp_log.h"
#include "esp_netif.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "iperf.h"
#include "net_diag.h"
#include "net_diag_private.h"
#include "utils.h"


/* Constants and macros ---------------------------------------------------- */
// clang-format off

#define NET_DIAG_IPERF_INTERVAL_S           1u
#define NET_DIAG_IPERF_TASK_PRIO            4u
#define NET_DIAG_IPERF_MAX_BANDWIDTH_KBPS   100000u
#define NET_DIAG_IPERF_LOCK_TIMEOUT_MS      100u

// clang-format on

/* Private types ----------------------------------------------------------- */


/* Private variables ------------------------------------------------------- */

static const char* net_diag_iperf_tag = "net_diag_iperf";

/* Guarded by net_diag_iperf_lock (worker, iperf traffic and report tasks) */
static SemaphoreHandle_t net_diag_iperf_lock         = NULL;
static net_diag_iperf_result_t net_diag_iperf_result = {0};
static iperf_id_t net_diag_iperf_id                  = -1;
static bool net_diag_iperf_started                   = false;


/* Private functions ------------------------------------------------------- */

static esp_err_t net_diag_iperf_validate(const net_diag_iperf_config_t* config)
{
    esp_err_t error_code   = ESP_OK;
    esp_ip4_addr_t address = {0};

    if ((config == NULL) || (config->port == 0u) || (config->time_s == 0u)
        || (config->time_s > NET_DIAG_IPERF_MAX_TIME_S)
        || (config->bandwidth_kbps > NET_DIAG_IPERF_MAX_BANDWIDTH_KBPS))
    {
        error_code = ESP_ERR_INVALID_ARG;
    }
    else if ((config->mode != NET_DIAG_IPERF_MODE_SERVER) && (config->mode != NET_DIAG_IPERF_MODE_CLIENT))
    {
        error_code = ESP_ERR_INVALID_ARG;
    }
    else if ((config->protocol != NET_DIAG_IPERF_PROTOCOL_TCP) && (config->protocol != NET_DIAG_IPERF_PROTOCOL_UDP))
    {
        error_code = ESP_ERR_INVALID_ARG;
    }
    else if (
        (config->mode == NET_DIAG_IPERF_MODE_CLIENT)
        && (esp_netif_str_to_ip4(config->host, &address) != ESP_OK))
    {
        error_code = ESP_ERR_INVALID_ARG;
    }
    else
    {
        /* Valid. */
    }

    return error_code;
}

static void net_diag_iperf_build_config(
    const net_diag_iperf_config_t* config,
    iperf_cfg_t* iperf_config)
{
    memset(iperf_config, 0, sizeof(*iperf_config));

    iperf_config->format                = MBITS_PER_SEC;
    iperf_config->interval              = NET_DIAG_IPERF_INTERVAL_S;
    iperf_config->time                  = config->time_s;
    iperf_config->bw_lim                = IPERF_DEFAULT_NO_BW_LIMIT;
    iperf_config->traffic_task_priority = NET_DIAG_IPERF_TASK_PRIO;

    iperf_config->flag = (config->protocol == NET_DIAG_IPERF_PROTOCOL_UDP) ?
                             IPERF_FLAG_UDP :
                             IPERF_FLAG_TCP;

    if (config->mode == NET_DIAG_IPERF_MODE_SERVER)
    {
        iperf_config->flag |= IPERF_FLAG_SERVER;
        iperf_config->source.type = ESP_IPADDR_TYPE_V4; /* 0.0.0.0 = any */
        iperf_config->sport       = config->port;
    }
    else
    {
        iperf_config->flag |= IPERF_FLAG_CLIENT;
        iperf_config->destination.type = ESP_IPADDR_TYPE_V4;
        esp_netif_str_to_ip4(config->host, &iperf_config->destination.u_addr.ip4);
        iperf_config->dport = config->port;

        if ((config->protocol == NET_DIAG_IPERF_PROTOCOL_UDP)
            && (config->bandwidth_kbps > 0u))
        {
            iperf_config->bw_lim = (int32_t)(config->bandwidth_kbps * 1000u);
        }
    }
}

/* ---- iperf callbacks (iperf traffic and report tasks) ---- */

static void net_diag_iperf_on_state(iperf_id_t id, iperf_state_data_t* data, void* arg)
{
    ARG_UNUSED(id);
    ARG_UNUSED(arg);

    xSemaphoreTake(net_diag_iperf_lock, portMAX_DELAY);

    switch (data->state)
    {
        case IPERF_STARTED:
        {
            net_diag_iperf_started             = true;
            net_diag_iperf_result.transferring = true;
        }
        break;
        case IPERF_STOPPED:
        {
            net_diag_iperf_result.transferring = false;
        }
        break;
        case IPERF_CLOSED:
        {
            net_diag_iperf_result.transferring = false;
            net_diag_iperf_id                  = -1;

            if (net_diag_iperf_started)
            {
                net_diag_iperf_result.state = NET_DIAG_STATE_DONE;
            }
            else
            {
                /* Server: nobody connected in time. Client: connect failed. */
                net_diag_iperf_result.error =
                    (net_diag_iperf_result.config.mode == NET_DIAG_IPERF_MODE_SERVER) ?
                        ESP_ERR_TIMEOUT :
                        ESP_FAIL;
                net_diag_iperf_result.state = NET_DIAG_STATE_FAILED;
            }
        }
        break;
        default:
        {
            /* IPERF_RUNNING: periods arrive through iperf_report_output(). */
        }
        break;
    }

    xSemaphoreGive(net_diag_iperf_lock);
}


/* Exposed API ------------------------------------------------------------- */

/* Overrides the weak default in espressif/iperf, which printf()s. */
void iperf_report_output(const iperf_report_t* report)
{
    xSemaphoreTake(net_diag_iperf_lock, portMAX_DELAY);

    const iperf_traffic_report_t* const traffic = &report->traffic;
    const uint32_t seconds = traffic->end_sec - traffic->period_start_sec;

    if ((report->report_type == IPERF_REPORT_PERIOD)
        && (net_diag_iperf_result.period_count < NET_DIAG_IPERF_MAX_PERIODS)
        && (seconds > 0u))
    {
        net_diag_iperf_period_t* const period =
            &net_diag_iperf_result.periods[net_diag_iperf_result.period_count];

        period->start_s = (uint16_t)traffic->period_start_sec;
        period->end_s   = (uint16_t)traffic->end_sec;
        period->bits_per_second =
            (uint32_t)((traffic->period_bytes * 8.0) / (double)seconds);
        net_diag_iperf_result.period_count++;
    }

    if (report->report_type != IPERF_REPORT_CONNECT_INFO)
    {
        net_diag_iperf_result.total_bytes = traffic->total_transfer_bytes;

        if (traffic->end_sec > 0u)
        {
            net_diag_iperf_result.average_bits_per_second =
                (uint32_t)((traffic->total_transfer_bytes * 8u) / traffic->end_sec);
        }
    }

    xSemaphoreGive(net_diag_iperf_lock);
}

esp_err_t net_diag_iperf_init(void)
{
    esp_err_t error_code = ESP_OK;

    net_diag_iperf_lock = xSemaphoreCreateMutex();

    if (net_diag_iperf_lock == NULL)
    {
        error_code = ESP_ERR_NO_MEM;
    }

    return error_code;
}

void net_diag_iperf_run_start(const net_diag_iperf_config_t* config)
{
    iperf_cfg_t iperf_config = {0};

    net_diag_iperf_build_config(config, &iperf_config);
    iperf_config.state_handler = net_diag_iperf_on_state;

    const iperf_id_t id = iperf_start_instance(&iperf_config);

    xSemaphoreTake(net_diag_iperf_lock, portMAX_DELAY);

    if (id < 0)
    {
        ESP_LOGW(net_diag_iperf_tag, "iperf start failed");
        net_diag_iperf_result.error = ESP_FAIL;
        net_diag_iperf_result.state = NET_DIAG_STATE_FAILED;
    }
    else if (net_diag_iperf_result.state == NET_DIAG_STATE_RUNNING)
    {
        /* Not stored if the instance already closed (failed connect). */
        net_diag_iperf_id = id;
    }
    else
    {
        /* Already closed. */
    }

    xSemaphoreGive(net_diag_iperf_lock);
}

void net_diag_iperf_run_stop(void)
{
    xSemaphoreTake(net_diag_iperf_lock, portMAX_DELAY);
    const iperf_id_t id = net_diag_iperf_id;
    xSemaphoreGive(net_diag_iperf_lock);

    if (id > 0)
    {
        iperf_stop_instance(id);
    }
}

esp_err_t net_diag_iperf_start(const net_diag_iperf_config_t* config)
{
    esp_err_t error_code = net_diag_iperf_validate(config);

    do
    {
        if (error_code != ESP_OK)
        {
            break;
        }

        if (net_diag_iperf_lock == NULL)
        {
            error_code = ESP_ERR_INVALID_STATE;
            break;
        }

        xSemaphoreTake(net_diag_iperf_lock, portMAX_DELAY);

        if (net_diag_iperf_result.state == NET_DIAG_STATE_RUNNING)
        {
            error_code = ESP_ERR_INVALID_STATE;
        }
        else
        {
            const net_diag_job_t job = {
                .type         = NET_DIAG_JOB_IPERF_START,
                .config.iperf = *config,
            };

            error_code = net_diag_submit(&job);

            if (error_code == ESP_OK)
            {
                memset(&net_diag_iperf_result, 0, sizeof(net_diag_iperf_result));
                net_diag_iperf_result.config = *config;
                net_diag_iperf_result.state  = NET_DIAG_STATE_RUNNING;
                net_diag_iperf_started       = false;
            }
        }

        xSemaphoreGive(net_diag_iperf_lock);
    } while (0);

    return error_code;
}

esp_err_t net_diag_iperf_stop(void)
{
    esp_err_t error_code = ESP_OK;

    do
    {
        if (net_diag_iperf_lock == NULL)
        {
            error_code = ESP_ERR_INVALID_STATE;
            break;
        }

        xSemaphoreTake(net_diag_iperf_lock, portMAX_DELAY);
        const bool running = (net_diag_iperf_result.state == NET_DIAG_STATE_RUNNING);
        xSemaphoreGive(net_diag_iperf_lock);

        if (!running)
        {
            error_code = ESP_ERR_INVALID_STATE;
            break;
        }

        const net_diag_job_t job = {.type = NET_DIAG_JOB_IPERF_STOP};

        error_code = net_diag_submit(&job);
    } while (0);

    return error_code;
}

esp_err_t net_diag_iperf_get(net_diag_iperf_result_t* result)
{
    esp_err_t error_code = ESP_OK;

    if ((result == NULL) || (net_diag_iperf_lock == NULL))
    {
        error_code = ESP_ERR_INVALID_STATE;
    }
    else if (xSemaphoreTake(net_diag_iperf_lock, pdMS_TO_TICKS(NET_DIAG_IPERF_LOCK_TIMEOUT_MS)) != pdTRUE)
    {
        error_code = ESP_ERR_TIMEOUT;
    }
    else
    {
        *result = net_diag_iperf_result;
        xSemaphoreGive(net_diag_iperf_lock);
    }

    return error_code;
}

/**** END OF FILE ****/
