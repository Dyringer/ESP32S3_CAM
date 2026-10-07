/**
 ******************************************************************************
 * @file    camera_shell.c
 * @author  Daniel Dyringer
 * @date    2026-09-30
 * @brief   Camera shell commands — status, set, save, defaults, reg.
 ******************************************************************************
 */


/* Includes ---------------------------------------------------------------- */
#include "camera_shell.h"

#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include "app_shell.h"
#include "camera.h"


/* Constants and macros ---------------------------------------------------- */
// clang-format off

#define CAMERA_SHELL_USAGE \
    "Usage: camera <status | set <name> <value> | save | defaults |\n" \
    "               reg <reg> <mask> [value]>\n"

// clang-format on


/* Private functions ------------------------------------------------------- */

/** @brief Parse a decimal or 0x-prefixed hex integer. */
static bool camera_shell_parse_int(const char *str, int *out)
{
    char       *end   = NULL;
    const long  value = strtol(str, &end, 0);

    if (end == str || *end != '\0')
    {
        return false;
    }

    *out = (int)value;
    return true;
}

static void camera_shell_cmd_status(app_shell_print_fn_t print)
{
    print("Sensor   : %s\n", camera_sensor_name());

    if (!camera_is_ready())
    {
        print("Camera not initialised.\n");
        return;
    }

    int      framesize = 0;
    uint16_t width     = 0u;
    uint16_t height    = 0u;

    if (camera_setting_get(CAMERA_SETTING_FRAMESIZE, &framesize) == ESP_OK
        && camera_framesize_resolution(framesize, &width, &height) == ESP_OK)
    {
        print("Frame    : %ux%u\n", (unsigned)width, (unsigned)height);
    }

    print("Settings : %s\n", camera_settings_unsaved() ? "unsaved changes" : "saved");

    print("\n%-16s %6s   %s\n", "Setting", "Value", "Range");

    for (size_t i = 0u; i < camera_setting_count(); i++)
    {
        const camera_setting_desc_t *desc  = camera_setting_at(i);
        int                          value = 0;

        if (camera_setting_get(desc->name, &value) == ESP_OK)
        {
            print("%-16s %6d   %d..%d\n", desc->name, value, desc->min, desc->max);
        }
        else
        {
            print("%-16s %6s   %d..%d\n", desc->name, "?", desc->min, desc->max);
        }
    }
}

static void camera_shell_cmd_set(int argc, char **argv, app_shell_print_fn_t print)
{
    int value = 0;

    /* argv[0] = "set", argv[1] = name, argv[2] = value */
    if (argc < 3 || !camera_shell_parse_int(argv[2], &value))
    {
        print("Usage: camera set <name> <value>   (names: camera status)\n");
        return;
    }

    const esp_err_t error_code = camera_setting_set(argv[1], value);

    switch (error_code)
    {
    case ESP_OK:
        print("%s = %d\n", argv[1], value);
        break;
    case ESP_ERR_NOT_FOUND:
        print("Unknown setting '%s' (see: camera status)\n", argv[1]);
        break;
    case ESP_ERR_INVALID_ARG:
        print("Value %d out of range for '%s'\n", value, argv[1]);
        break;
    default:
        print("Failed: %s\n", esp_err_to_name(error_code));
        break;
    }
}

static void camera_shell_cmd_reg(int argc, char **argv, app_shell_print_fn_t print)
{
    int reg   = 0;
    int mask  = 0;
    int value = 0;

    /* argv[0] = "reg", argv[1] = reg, argv[2] = mask, argv[3] = value (opt) */
    if (argc < 3
        || !camera_shell_parse_int(argv[1], &reg)
        || !camera_shell_parse_int(argv[2], &mask)
        || (argc >= 4 && !camera_shell_parse_int(argv[3], &value)))
    {
        print("Usage: camera reg <reg> <mask> [value]   e.g. camera reg 0x3a18 0x3ff\n");
        return;
    }

    esp_err_t error_code = ESP_OK;

    if (argc >= 4)
    {
        error_code = camera_reg_set(reg, mask, value);

        if (error_code != ESP_OK)
        {
            print("Write failed: %s\n", esp_err_to_name(error_code));
            return;
        }
    }

    error_code = camera_reg_get(reg, mask, &value);

    if (error_code != ESP_OK)
    {
        print("Read failed: %s\n", esp_err_to_name(error_code));
        return;
    }

    print("reg 0x%04x & 0x%x = 0x%x (%d)\n", reg, mask, value, value);
}

static void camera_shell_cmd_router(int                   argc,
                                    char                **argv,
                                    app_shell_print_fn_t  print)
{
    do
    {
        if (argc < 2)
        {
            print(CAMERA_SHELL_USAGE);
            break;
        }

        if (strcmp(argv[1], "status") == 0)
        {
            camera_shell_cmd_status(print);
            break;
        }

        if (strcmp(argv[1], "set") == 0)
        {
            camera_shell_cmd_set(argc - 1, argv + 1, print);
            break;
        }

        if (strcmp(argv[1], "save") == 0)
        {
            const esp_err_t error_code = camera_settings_save();

            if (error_code == ESP_OK)
            {
                print("Settings saved — applied at every boot\n");
            }
            else
            {
                print("Save failed: %s\n", esp_err_to_name(error_code));
            }

            break;
        }

        if (strcmp(argv[1], "defaults") == 0)
        {
            const esp_err_t error_code = camera_settings_restore_defaults();

            if (error_code == ESP_OK)
            {
                print("Defaults restored, saved settings erased\n");
            }
            else
            {
                print("Restore failed: %s\n", esp_err_to_name(error_code));
            }

            break;
        }

        if (strcmp(argv[1], "reg") == 0)
        {
            camera_shell_cmd_reg(argc - 1, argv + 1, print);
            break;
        }

        print("Unknown subcommand: '%s'\n", argv[1]);
        print(CAMERA_SHELL_USAGE);
    } while (0);
}


/* Exposed API ------------------------------------------------------------- */

esp_err_t camera_shell_register(void)
{
    return app_shell_register_cmd(
        "camera",
        "Camera commands: camera <status|set|save|defaults|reg>",
        camera_shell_cmd_router);
}


/**** END OF FILE ****/
