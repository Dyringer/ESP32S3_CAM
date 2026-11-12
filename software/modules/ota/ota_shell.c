/**
 ******************************************************************************
 * @file    ota_shell.c
 * @author  Daniel Dyringer
 * @date    2026-08-13
 * @brief   OTA shell commands implementation.
 ******************************************************************************
 */


/* Includes ---------------------------------------------------------------- */
#include "ota_shell.h"

#include <string.h>

#include "app_shell.h"
#include "esp_ota_ops.h"
#include "ota.h"


/* Constants and macros ---------------------------------------------------- */
// clang-format off


// clang-format on

/* Private types ----------------------------------------------------------- */


/* Private variables ------------------------------------------------------- */


/* Private functions ------------------------------------------------------- */

static const char* ota_shell_state_name(ota_state_t state)
{
    const char* name = NULL;

    switch (state)
    {
        case OTA_STATE_IDLE:
        {
            name = "idle";
        }
        break;
        case OTA_STATE_RECEIVING:
        {
            name = "receiving";
        }
        break;
        case OTA_STATE_VALIDATING:
        {
            name = "validating";
        }
        break;
        case OTA_STATE_APPLYING:
        {
            name = "applying";
        }
        break;
        case OTA_STATE_PENDING_REBOOT:
        {
            name = "pending reboot";
        }
        break;
        case OTA_STATE_FAILED:
        {
            name = "failed";
        }
        break;
        default:
        {
            name = "unknown";
        }
        break;
    }

    return name;
}

static void ota_shell_cmd_status(app_shell_print_fn_t print)
{
    const esp_partition_t* running = esp_ota_get_running_partition();
    ota_status_t status            = {0};

    ota_get_status(&status);

    print("Running version: %s\n", ota_get_running_version());
    print("Running partition: %s\n", (running != NULL) ? running->label : "?");
    print("State: %s\n", ota_shell_state_name(status.state));

    if (status.bytes_expected > 0u)
    {
        print(
            "Progress: %u / %u bytes\n",
            (unsigned)status.bytes_written,
            (unsigned)status.bytes_expected);
    }
    else if (status.bytes_written > 0u)
    {
        print("Bytes written: %u\n", (unsigned)status.bytes_written);
    }
    else
    {
        /* Nothing received yet */
    }

    if (status.last_error != ESP_OK)
    {
        print("Last error: %s\n", esp_err_to_name(status.last_error));
    }
}

static void ota_shell_cmd(int argc, char** argv, app_shell_print_fn_t print)
{
    if ((argc < 2) || (strcmp(argv[1], "status") != 0))
    {
        print("Usage:\n");
        print("  ota status   print running version and last OTA result\n");
    }
    else
    {
        ota_shell_cmd_status(print);
    }
}


/* Exposed API ------------------------------------------------------------- */

esp_err_t ota_shell_register(void)
{
    return app_shell_register_cmd("ota", "OTA update status: ota status", ota_shell_cmd);
}

/**** END OF FILE ****/
