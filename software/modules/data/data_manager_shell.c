/**
 ******************************************************************************
 * @file    data_manager_shell.c
 * @author  Daniel Dyringer
 * @date    2026-10-11
 * @brief   Data manager shell commands: list, get, set, save.
 ******************************************************************************
 */


/* Includes ---------------------------------------------------------------- */
#include "data_manager_shell.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "app_shell.h"
#include "data_manager.h"
#include "data_manager_text.h"


/* Constants and macros ---------------------------------------------------- */
// clang-format off

#define DATA_MANAGER_SHELL_USAGE \
    "Usage: data <list | get <name> | set <name> <value> | save>\n" \
    "  Quote strings containing spaces: data set device_name \"desk cam\"\n"

#define DATA_MANAGER_SHELL_BLOB_PRINT_MAX   32u

// clang-format on

/* Private types ----------------------------------------------------------- */


/* Private variables ------------------------------------------------------- */


/* Private functions ------------------------------------------------------- */

static void data_manager_shell_print_value(
    app_shell_print_fn_t print,
    const data_manager_descriptor_t* descriptor,
    const uint8_t* buffer)
{
    if (descriptor->type == DATA_MANAGER_TYPE_STRING)
    {
        print("\"%s\"", (const char*)buffer);
    }
    else
    {
        char text[(DATA_MANAGER_SHELL_BLOB_PRINT_MAX * 2u) + 1u] = {0};

        const esp_err_t error_code =
            data_manager_text_format(descriptor, buffer, text, sizeof(text));

        print("%s", text);

        if ((error_code == ESP_ERR_INVALID_SIZE)
            && (descriptor->type == DATA_MANAGER_TYPE_BLOB))
        {
            print("... (%u bytes)", (unsigned)descriptor->size);
        }
    }
}

/* ---- Commands ---- */

static void data_manager_shell_print_entry(
    app_shell_print_fn_t print,
    const data_manager_descriptor_t* descriptor)
{
    uint8_t* const buffer = malloc(descriptor->size);

    print(
        "%-32s %-6s %c%c ",
        descriptor->name,
        data_manager_text_type_name(descriptor->type),
        descriptor->persistent ? 'P' : '-',
        descriptor->secret ? 'S' : '-');

    if (buffer == NULL)
    {
        print("(no memory)\n");
    }
    else
    {
        const esp_err_t error_code =
            data_manager_get(descriptor->id, buffer, descriptor->size);

        if ((error_code == ESP_OK) && descriptor->secret)
        {
            /* Strings are zero-padded, so an empty secret starts with zero. */
            print("%s\n", (buffer[0] != 0u) ? "********" : "\"\"");
        }
        else if (error_code == ESP_OK)
        {
            data_manager_shell_print_value(print, descriptor, buffer);
            print("\n");
        }
        else
        {
            print("(%s)\n", esp_err_to_name(error_code));
        }
    }

    free(buffer);
}

static void data_manager_shell_cmd_list(app_shell_print_fn_t print)
{
    const size_t count = data_manager_count();

    print("%-32s %-6s %-2s %s\n", "Name", "Type", "PS", "Value");

    for (size_t i = 0u; i < count; i++)
    {
        const data_manager_descriptor_t* const descriptor = data_manager_at(i);

        if (descriptor != NULL)
        {
            data_manager_shell_print_entry(print, descriptor);
        }
    }

    print("%u setting(s); P = persistent, S = secret", (unsigned)count);
    print("%s\n", data_manager_unsaved() ? "; unsaved changes" : "");
}

static void data_manager_shell_cmd_get(const char* name, app_shell_print_fn_t print)
{
    const data_manager_descriptor_t* const descriptor = data_manager_find(name);

    if (descriptor != NULL)
    {
        data_manager_shell_print_entry(print, descriptor);
    }
    else
    {
        print("Unknown setting: %s\n", name);
    }
}

static void data_manager_shell_cmd_set(
    const char* name,
    const char* text,
    app_shell_print_fn_t print)
{
    const data_manager_descriptor_t* const descriptor = data_manager_find(name);
    uint8_t* buffer                                   = NULL;
    size_t size                                       = 0u;
    esp_err_t error_code                              = ESP_OK;

    do
    {
        if (descriptor == NULL)
        {
            print("Unknown setting: %s\n", name);
            break;
        }

        buffer = calloc(1u, descriptor->size);

        if (buffer == NULL)
        {
            print("Failed: %s\n", esp_err_to_name(ESP_ERR_NO_MEM));
            break;
        }

        error_code = data_manager_text_parse(descriptor, text, buffer, &size);

        if (error_code != ESP_OK)
        {
            print(
                "Invalid %s value (%s)\n",
                data_manager_text_type_name(descriptor->type),
                esp_err_to_name(error_code));
            break;
        }

        error_code = data_manager_set(descriptor->id, buffer, size);

        if ((error_code == ESP_ERR_INVALID_ARG)
            && (descriptor->minimum < descriptor->maximum))
        {
            print(
                "Out of range %ld..%ld\n",
                (long)descriptor->minimum,
                (long)descriptor->maximum);
            break;
        }

        if (error_code != ESP_OK)
        {
            print("Failed: %s\n", esp_err_to_name(error_code));
            break;
        }

        data_manager_shell_print_entry(print, descriptor);
        print("OK (not saved; run 'data save' to keep it)\n");
    } while (0);

    free(buffer);
}

static void data_manager_shell_cmd_save(app_shell_print_fn_t print)
{
    const esp_err_t error_code = data_manager_save();

    if (error_code == ESP_OK)
    {
        print("OK (saving)\n");
    }
    else
    {
        print("Failed: %s\n", esp_err_to_name(error_code));
    }
}

static void data_manager_shell_cmd(int argc, char** argv, app_shell_print_fn_t print)
{
    if ((argc == 2) && (strcmp(argv[1], "list") == 0))
    {
        data_manager_shell_cmd_list(print);
    }
    else if ((argc == 3) && (strcmp(argv[1], "get") == 0))
    {
        data_manager_shell_cmd_get(argv[2], print);
    }
    else if ((argc == 4) && (strcmp(argv[1], "set") == 0))
    {
        data_manager_shell_cmd_set(argv[2], argv[3], print);
    }
    else if ((argc == 2) && (strcmp(argv[1], "save") == 0))
    {
        data_manager_shell_cmd_save(print);
    }
    else
    {
        print(DATA_MANAGER_SHELL_USAGE);
    }
}


/* Exposed API ------------------------------------------------------------- */

esp_err_t data_manager_shell_register(void)
{
    return app_shell_register_cmd(
        "data",
        "Settings: data <list | get <name> | set <name> <value> | save>",
        data_manager_shell_cmd);
}

/**** END OF FILE ****/
