/**
 ******************************************************************************
 * @file    net_diag_shell.c
 * @author  Daniel Dyringer
 * @date    2026-10-17
 * @brief   Network diagnostics shell commands — ping.
 ******************************************************************************
 */


/* Includes ---------------------------------------------------------------- */
#include "net_diag_shell.h"

#include <stdlib.h>
#include <string.h>

#include "app_shell.h"
#include "esp_err.h"
#include "net_diag.h"
#include "utils.h"


/* Constants and macros ---------------------------------------------------- */
// clang-format off


// clang-format on

/* Private types ----------------------------------------------------------- */


/* Private variables ------------------------------------------------------- */


/* Private functions ------------------------------------------------------- */

static void net_diag_shell_print_usage(app_shell_print_fn_t print)
{
    print("Usage:\n");
    print("  ping start <host> [-c count] [-s bytes] [-i ms] [-W ms]\n");
    print("  ping status\n");
    print(
        "    -c count 1-%u (default 4), -s payload %u-%u B (default %u),\n",
        (unsigned)NET_DIAG_PING_MAX_COUNT,
        (unsigned)NET_DIAG_PING_MIN_SIZE,
        (unsigned)NET_DIAG_PING_MAX_SIZE,
        (unsigned)NET_DIAG_PING_DEFAULT_SIZE);
    print(
        "    -i interval %u-%u ms (default %u), -W timeout %u-%u ms (default %u)\n",
        (unsigned)NET_DIAG_PING_MIN_INTERVAL_MS,
        (unsigned)NET_DIAG_PING_MAX_INTERVAL_MS,
        (unsigned)NET_DIAG_PING_DEFAULT_INTERVAL_MS,
        (unsigned)NET_DIAG_PING_MIN_TIMEOUT_MS,
        (unsigned)NET_DIAG_PING_MAX_TIMEOUT_MS,
        (unsigned)NET_DIAG_PING_DEFAULT_TIMEOUT_MS);
}

static esp_err_t net_diag_shell_parse_uint(const char* text, uint32_t* value)
{
    esp_err_t error_code       = ESP_OK;
    char* end                  = NULL;
    const unsigned long parsed = strtoul(text, &end, 10);

    if ((text[0] == '\0') || (*end != '\0') || (parsed > UINT32_MAX))
    {
        error_code = ESP_ERR_INVALID_ARG;
    }
    else
    {
        *value = (uint32_t)parsed;
    }

    return error_code;
}

static esp_err_t net_diag_shell_parse_ping(int argc, char** argv, net_diag_ping_config_t* config)
{
    esp_err_t error_code = ESP_OK;
    uint32_t count       = 4u;

    config->size        = NET_DIAG_PING_DEFAULT_SIZE;
    config->interval_ms = NET_DIAG_PING_DEFAULT_INTERVAL_MS;
    config->timeout_ms  = NET_DIAG_PING_DEFAULT_TIMEOUT_MS;

    strlcpy(config->host, argv[2], sizeof(config->host));

    for (int i = 3; (i < argc) && (error_code == ESP_OK); i += 2)
    {
        uint32_t* target = NULL;

        if (strcmp(argv[i], "-c") == 0)
        {
            target = &count;
        }
        else if (strcmp(argv[i], "-s") == 0)
        {
            target = &config->size;
        }
        else if (strcmp(argv[i], "-i") == 0)
        {
            target = &config->interval_ms;
        }
        else if (strcmp(argv[i], "-W") == 0)
        {
            target = &config->timeout_ms;
        }
        else
        {
            error_code = ESP_ERR_INVALID_ARG;
        }

        if ((target != NULL) && ((i + 1) < argc))
        {
            error_code = net_diag_shell_parse_uint(argv[i + 1], target);
        }
        else
        {
            error_code = ESP_ERR_INVALID_ARG;
        }
    }

    config->count = (uint16_t)MIN(count, (uint32_t)UINT16_MAX);

    return error_code;
}

static void net_diag_shell_ping_start(int argc, char** argv, app_shell_print_fn_t print)
{
    net_diag_ping_config_t config = {0};

    if ((argc < 3) || (net_diag_shell_parse_ping(argc, argv, &config) != ESP_OK))
    {
        net_diag_shell_print_usage(print);
    }
    else
    {
        const esp_err_t error_code = net_diag_ping_start(&config);

        if (error_code == ESP_OK)
        {
            print("Ping started, see: ping status\n");
        }
        else if (error_code == ESP_ERR_INVALID_STATE)
        {
            print("A ping is already running\n");
        }
        else if (error_code == ESP_ERR_INVALID_ARG)
        {
            net_diag_shell_print_usage(print);
        }
        else
        {
            print("Ping failed to start: %s\n", esp_err_to_name(error_code));
        }
    }
}

static void net_diag_shell_ping_status(app_shell_print_fn_t print)
{
    net_diag_ping_result_t result = {0};
    const esp_err_t error_code    = net_diag_ping_get(&result);

    if (error_code != ESP_OK)
    {
        print("Ping status unavailable: %s\n", esp_err_to_name(error_code));
    }
    else if (result.state == NET_DIAG_STATE_IDLE)
    {
        print("No ping run yet\n");
    }
    else if (result.state == NET_DIAG_STATE_FAILED)
    {
        print("Ping %s failed: %s\n", result.host, esp_err_to_name(result.error));
    }
    else
    {
        print(
            "%s (%s): %u B every %u ms, %s\n",
            result.host,
            result.address,
            (unsigned)result.size,
            (unsigned)result.interval_ms,
            net_diag_state_name(result.state));
        print(
            "Replies: %u/%u of %u\n",
            (unsigned)result.received,
            (unsigned)result.transmitted,
            (unsigned)result.count);

        if (result.received > 0u)
        {
            print(
                "min/avg/max = %u/%u/%u ms, last %u ms\n",
                (unsigned)result.min_ms,
                (unsigned)(result.total_ms / result.received),
                (unsigned)result.max_ms,
                (unsigned)result.last_ms);
        }
    }
}

static void net_diag_shell_cmd_ping(int argc, char** argv, app_shell_print_fn_t print)
{
    if ((argc >= 2) && (strcmp(argv[1], "start") == 0))
    {
        net_diag_shell_ping_start(argc, argv, print);
    }
    else if ((argc == 2) && (strcmp(argv[1], "status") == 0))
    {
        net_diag_shell_ping_status(print);
    }
    else
    {
        net_diag_shell_print_usage(print);
    }
}


/* Exposed API ------------------------------------------------------------- */

esp_err_t net_diag_shell_register(void)
{
    return app_shell_register_cmd(
        "ping",
        "ICMP ping: ping start <host> [-c n] [-s bytes] [-i ms] [-W ms]",
        net_diag_shell_cmd_ping);
}

/**** END OF FILE ****/
