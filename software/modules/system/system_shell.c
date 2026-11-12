/**
 ******************************************************************************
 * @file    system_shell.c
 * @author  Daniel Dyringer
 * @date    2026-10-07
 * @brief   System shell commands — reboot, factory_reset.
 ******************************************************************************
 */


/* Includes ---------------------------------------------------------------- */
#include "system_shell.h"

#include <string.h>

#include "app_shell.h"
#include "system.h"
#include "utils.h"


/* Constants and macros ---------------------------------------------------- */
// clang-format off


// clang-format on

/* Private types ----------------------------------------------------------- */


/* Private variables ------------------------------------------------------- */


/* Private functions ------------------------------------------------------- */

static void system_shell_cmd_reboot(int argc, char** argv, app_shell_print_fn_t print)
{
    ARG_UNUSED(argc);
    ARG_UNUSED(argv);

    print("Rebooting...\n");
    system_reboot();
}

/* Needs "confirm" so a stray or mistyped command can't wipe the device. */
static void system_shell_cmd_factory_reset(int argc, char** argv, app_shell_print_fn_t print)
{
    if ((argc < 2) || (strcmp(argv[1], "confirm") != 0))
    {
        print("Erases ALL settings (WiFi, camera, upload) and reboots into the\n");
        print("setup portal. Run 'factory_reset confirm' to proceed.\n");
    }
    else
    {
        print("Erasing all settings and rebooting...\n");

        const esp_err_t error_code = system_factory_reset();

        if (error_code != ESP_OK)
        {
            print("Factory reset failed: %s\n", esp_err_to_name(error_code));
        }
    }
}


/* Exposed API ------------------------------------------------------------- */

esp_err_t system_shell_register(void)
{
    esp_err_t error_code = ESP_OK;

    do
    {
        error_code = app_shell_register_cmd(
            "reboot", "Restart the device", system_shell_cmd_reboot);

        if (error_code != ESP_OK)
        {
            break;
        }

        error_code = app_shell_register_cmd(
            "factory_reset",
            "Erase all settings and reboot into the setup portal",
            system_shell_cmd_factory_reset);
    } while (0);

    return error_code;
}

/**** END OF FILE ****/
