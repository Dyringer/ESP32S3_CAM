/**
 ******************************************************************************
 * @file    wifi_shell.c
 * @author  Daniel Dyringer
 * @date    2026-02-17
 * @brief   WiFi diagnostic shell commands — status, scan, join, forget.
 ******************************************************************************
 */


/* Includes ---------------------------------------------------------------- */
#include "wifi_shell.h"

#include <stdbool.h>
#include <string.h>

#include "app_shell.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "net_wifi_sta.h"
#include "wifi_connection.h"
#include "wifi_types.h"


/* Constants and macros ---------------------------------------------------- */
// clang-format off

#define WIFI_SHELL_USAGE    "Usage: wifi <status|scan|join|forget>\n"

// clang-format on

/* Private types ----------------------------------------------------------- */


/* Private variables ------------------------------------------------------- */


/* Private functions ------------------------------------------------------- */

static const char* wifi_shell_authmode_str(wifi_auth_mode_t auth_mode)
{
    const char* name = NULL;

    switch (auth_mode)
    {
        case WIFI_AUTH_OPEN:
        {
            name = "Open";
        }
        break;
        case WIFI_AUTH_WEP:
        {
            name = "WEP";
        }
        break;
        case WIFI_AUTH_WPA_PSK:
        {
            name = "WPA";
        }
        break;
        case WIFI_AUTH_WPA2_PSK:
        {
            name = "WPA2";
        }
        break;
        case WIFI_AUTH_WPA_WPA2_PSK:
        {
            name = "WPA/2";
        }
        break;
        case WIFI_AUTH_WPA3_PSK:
        {
            name = "WPA3";
        }
        break;
        case WIFI_AUTH_WPA2_WPA3_PSK:
        {
            name = "WPA2/3";
        }
        break;
        default:
        {
            name = "?";
        }
        break;
    }

    return name;
}

static void wifi_shell_cmd_status(app_shell_print_fn_t print)
{
    const wifi_connection_state_t state = wifi_connection_get_state();

    print("State    : %s\n", wifi_connection_state_str(state));

    char ssid[WIFI_TYPES_SSID_LEN] = {0};

    if ((wifi_connection_get_ssid(ssid, sizeof(ssid)) == ESP_OK) && (ssid[0] != '\0'))
    {
        print("SSID     : %s\n", ssid);
    }

    if (state == WIFI_CONNECTION_STATE_CONNECTED)
    {
        esp_netif_t* netif     = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
        esp_netif_ip_info_t ip = {0};

        if ((netif != NULL) && (esp_netif_get_ip_info(netif, &ip) == ESP_OK))
        {
            print("IP       : " IPSTR "\n", IP2STR(&ip.ip));
            print("Gateway  : " IPSTR "\n", IP2STR(&ip.gw));
            print("Netmask  : " IPSTR "\n", IP2STR(&ip.netmask));
        }
    }
}

static void wifi_shell_cmd_scan(app_shell_print_fn_t print)
{
    wifi_ap_record_t* records = NULL;
    uint16_t count            = 0u;

    print("Scanning...\n");

    do
    {
        if (net_wifi_sta_scan(&records, &count) != ESP_OK)
        {
            print("Scan failed.\n");
            break;
        }

        if (count == 0u)
        {
            print("No networks found.\n");
            break;
        }

        print("\n%-32s  %5s  %-7s  %s\n", "SSID", "RSSI", "Auth", "Ch");
        print("%-32s  %5s  %-7s  %s\n", "----", "----", "----", "--");

        for (uint16_t i = 0u; i < count; i++)
        {
            if (records[i].ssid[0] != 0u)
            {
                print(
                    "%-32s  %5d  %-7s  %u\n",
                    (const char*)records[i].ssid,
                    records[i].rssi,
                    wifi_shell_authmode_str(records[i].authmode),
                    (unsigned)records[i].primary);
            }
        }

        print("\n%u network(s) found.\n", (unsigned)count);
    } while (0);

    net_wifi_sta_scan_free(records);
}

static void wifi_shell_cmd_join(int argc, char** argv, app_shell_print_fn_t print)
{
    do
    {
        if (argc < 2)
        {
            print("Usage: wifi join <ssid> [pass]\n");
            break;
        }

        const char* ssid     = argv[1];
        const char* password = (argc >= 3) ? argv[2] : "";

        const esp_err_t error_code = wifi_connection_set_credentials(ssid, password);

        if (error_code == ESP_ERR_INVALID_ARG)
        {
            print("Invalid: " WIFI_CONNECTION_CREDENTIALS_RULE "\n");
            break;
        }

        if (error_code != ESP_OK)
        {
            print("Failed to apply credentials: %s\n", esp_err_to_name(error_code));
            break;
        }

        print("Connecting to '%s' — saved only once connected\n", ssid);
    } while (0);
}

static void wifi_shell_cmd_forget(app_shell_print_fn_t print)
{
    print("Clearing WiFi credentials and rebooting...\n");

    const esp_err_t error_code = wifi_connection_forget();

    if (error_code != ESP_OK)
    {
        print("Failed: %s\n", esp_err_to_name(error_code));
    }
}

static void wifi_shell_cmd_router(int argc, char** argv, app_shell_print_fn_t print)
{
    if (argc < 2)
    {
        print(WIFI_SHELL_USAGE);
    }
    else if (strcmp(argv[1], "status") == 0)
    {
        wifi_shell_cmd_status(print);
    }
    else if (strcmp(argv[1], "scan") == 0)
    {
        wifi_shell_cmd_scan(print);
    }
    else if (strcmp(argv[1], "join") == 0)
    {
        wifi_shell_cmd_join(argc - 1, &argv[1], print);
    }
    else if (strcmp(argv[1], "forget") == 0)
    {
        wifi_shell_cmd_forget(print);
    }
    else
    {
        print("Unknown subcommand: '%s'\n", argv[1]);
        print(WIFI_SHELL_USAGE);
    }
}


/* Exposed API ------------------------------------------------------------- */

esp_err_t wifi_shell_register(void)
{
    return app_shell_register_cmd(
        "wifi", "WiFi commands:  wifi <status|scan|join|forget>", wifi_shell_cmd_router);
}

/**** END OF FILE ****/
