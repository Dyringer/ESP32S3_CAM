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
#include "wifi_connection.h"
#include "wifi_sta.h"


/* Private functions ------------------------------------------------------- */

static const char *wifi_shell_authmode_str(wifi_auth_mode_t m)
{
    const char *str;

    switch (m)
    {
        case WIFI_AUTH_OPEN:
        {
            str = "Open";
            break;
        }
        case WIFI_AUTH_WEP:
        {
            str = "WEP";
            break;
        }
        case WIFI_AUTH_WPA_PSK:
        {
            str = "WPA";
            break;
        }
        case WIFI_AUTH_WPA2_PSK:
        {
            str = "WPA2";
            break;
        }
        case WIFI_AUTH_WPA_WPA2_PSK:
        {
            str = "WPA/2";
            break;
        }
        case WIFI_AUTH_WPA3_PSK:
        {
            str = "WPA3";
            break;
        }
        case WIFI_AUTH_WPA2_WPA3_PSK:
        {
            str = "WPA2/3";
            break;
        }
        default:
        {
            str = "?";
            break;
        }
    }

    return str;
}

static void wifi_shell_cmd_status(app_shell_print_fn_t print)
{
    const wifi_conn_state_t st = wifi_connection_get_state();

    print("State    : %s\n", wifi_connection_state_str(st));

    char ssid[32] = {0};
    wifi_connection_get_ssid(ssid, sizeof(ssid));

    if (ssid[0])
    {
        print("SSID     : %s\n", ssid);
    }

    if (st == WIFI_CONN_STATE_CONNECTED)
    {
        esp_netif_t *ni = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");

        if (ni)
        {
            esp_netif_ip_info_t ip;
            esp_netif_get_ip_info(ni, &ip);
            print("IP       : " IPSTR "\n", IP2STR(&ip.ip));
            print("Gateway  : " IPSTR "\n", IP2STR(&ip.gw));
            print("Netmask  : " IPSTR "\n", IP2STR(&ip.netmask));
        }
    }
}

static void wifi_shell_cmd_scan(app_shell_print_fn_t print)
{
    wifi_ap_record_t *recs  = NULL;
    uint16_t          count = 0u;

    print("Scanning...\n");

    do
    {
        if (net_wifi_sta_scan(&recs, &count) != ESP_OK)
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
        print("%-32s  %5s  %-7s  %s\n",   "----", "----", "----", "--");

        for (uint16_t i = 0u; i < count; i++)
        {
            if (!recs[i].ssid[0])
            {
                continue;
            }

            print("%-32s  %5d  %-7s  %d\n",
                  (char *)recs[i].ssid,
                  recs[i].rssi,
                  wifi_shell_authmode_str(recs[i].authmode),
                  recs[i].primary);
        }

        print("\n%d network(s) found.\n", (int)count);

        net_wifi_sta_scan_free(recs);
    } while (0);
}

static void wifi_shell_cmd_join(int                  argc,
                                 char               **argv,
                                 app_shell_print_fn_t print)
{
    do
    {
        /* argv[0] = "join", argv[1] = ssid, argv[2] = pass (optional) */
        if (argc < 2)
        {
            print("Usage: wifi join <ssid> [pass]\n");
            break;
        }

        const char *ssid = argv[1];
        const char *pass = (argc >= 3) ? argv[2] : "";

        const esp_err_t error_code = wifi_connection_set_credentials(ssid, pass);

        if (error_code != ESP_OK)
        {
            print("Failed to apply credentials: %s\n", esp_err_to_name(error_code));
            break;
        }

        print("Credentials applied, connecting to '%s'...\n", ssid);
    } while (0);
}

static void wifi_shell_cmd_router(int                   argc,
                                   char                **argv,
                                   app_shell_print_fn_t  print)
{
    do
    {
        if (argc < 2)
        {
            print("Usage: wifi <status|scan|join|forget>\n");
            break;
        }

        if (strcmp(argv[1], "status") == 0)
        {
            wifi_shell_cmd_status(print);
            break;
        }

        if (strcmp(argv[1], "scan") == 0)
        {
            wifi_shell_cmd_scan(print);
            break;
        }

        if (strcmp(argv[1], "join") == 0)
        {
            /* pass sub-argv: argv+1 = {"join", ssid, pass, ...} */
            wifi_shell_cmd_join(argc - 1, argv + 1, print);
            break;
        }

        if (strcmp(argv[1], "forget") == 0)
        {
            print("Clearing WiFi credentials and rebooting...\n");
            wifi_connection_forget(); /* does not return */
            break;
        }

        print("Unknown subcommand: '%s'\n", argv[1]);
        print("Usage: wifi <status|scan|join|forget>\n");
    } while (0);
}


/* Exposed API ------------------------------------------------------------- */

esp_err_t wifi_shell_register(void)
{
    return app_shell_register_cmd(
        "wifi",
        "WiFi commands:  wifi <status|scan|join|forget>",
        wifi_shell_cmd_router);
}


/**** END OF FILE ****/
