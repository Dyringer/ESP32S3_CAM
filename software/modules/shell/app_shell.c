/**
 ******************************************************************************
 * @file    app_shell.c
 * @author  Daniel Dyringer
 * @date    2026-03-12
 * @brief   Abstract shell — unified command table for UART and web consoles.
 ******************************************************************************
 */


/* Includes ---------------------------------------------------------------- */
#include "app_shell.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "utils.h"


/* Constants and macros ---------------------------------------------------- */
// clang-format off

#define APP_SHELL_CMD_TABLE_SIZE    24u
#define APP_SHELL_ARGC_MAX          8u

/* "> " + line + "\n" + NUL */
#define APP_SHELL_ECHO_MAX          258u

// clang-format on

/* Private types ----------------------------------------------------------- */

typedef struct app_shell_cmd_entry_s
{
    const char* name;
    const char* help;
    app_shell_cmd_fn_t handler;
} app_shell_cmd_entry_t;


/* Private variables ------------------------------------------------------- */

static const char* app_shell_tag = "app_shell";

static app_shell_cmd_entry_t app_shell_commands[APP_SHELL_CMD_TABLE_SIZE];
static uint8_t app_shell_command_count = 0u;


/* Private functions ------------------------------------------------------- */

static void app_shell_cmd_help(int argc, char** argv, app_shell_print_fn_t print)
{
    ARG_UNUSED(argc);
    ARG_UNUSED(argv);
    print("Available commands:\n");

    for (uint8_t i = 0u; i < app_shell_command_count; i++)
    {
        print(
            "  %-16s %s\n",
            app_shell_commands[i].name,
            (app_shell_commands[i].help != NULL) ? app_shell_commands[i].help : "");
    }
}

/* Double quotes group a token and are stripped; inside them \" and \\ are
 * escapes. In-place is safe: removing quotes and escapes only moves
 * characters left. Returns -1 on an unterminated quote. */
static int app_shell_tokenise(char* line, char** argv, int argc_max)
{
    char* source      = line;
    char* destination = line;
    int argc          = 0;
    bool unterminated = false;

    for (;;)
    {
        while ((*source == ' ') || (*source == '\t'))
        {
            source++;
        }

        if ((*source == '\0') || (argc >= argc_max))
        {
            break;
        }

        argv[argc] = destination;
        argc++;

        bool quoted = false;

        while ((*source != '\0') && (quoted || ((*source != ' ') && (*source != '\t'))))
        {
            if (*source == '"')
            {
                quoted = !quoted;
                source++;
                continue;
            }

            if (quoted && (*source == '\\') && ((source[1] == '"') || (source[1] == '\\')))
            {
                source++;
            }

            *destination = *source;
            destination++;
            source++;
        }

        if (quoted)
        {
            unterminated = true;
            break;
        }

        if (*source != '\0')
        {
            source++; /* step over the separator before terminating the token */
        }

        *destination = '\0';
        destination++;
    }

    return unterminated ? -1 : argc;
}


/* Exposed API ------------------------------------------------------------- */

esp_err_t app_shell_register_cmd(const char* name, const char* help, app_shell_cmd_fn_t handler)
{
    esp_err_t error_code = ESP_OK;

    if (app_shell_command_count >= APP_SHELL_CMD_TABLE_SIZE)
    {
        ESP_LOGE(app_shell_tag, "Command table full");
        error_code = ESP_ERR_NO_MEM;
    }
    else
    {
        app_shell_commands[app_shell_command_count].name    = name;
        app_shell_commands[app_shell_command_count].help    = help;
        app_shell_commands[app_shell_command_count].handler = handler;
        app_shell_command_count++;
    }

    return error_code;
}

esp_err_t app_shell_init(void)
{
    return app_shell_register_cmd("help", "List available commands", app_shell_cmd_help);
}

void app_shell_dispatch(char* line, app_shell_print_fn_t print)
{
    do
    {
        char echo[APP_SHELL_ECHO_MAX] = {0};
        snprintf(echo, sizeof(echo), "> %s\n", line);
        print("%s", echo);

        char* argv[APP_SHELL_ARGC_MAX] = {NULL};
        const int argc = app_shell_tokenise(line, argv, (int)APP_SHELL_ARGC_MAX);

        if (argc < 0)
        {
            print("Unterminated quote\n");
            break;
        }

        if (argc == 0)
        {
            break;
        }

        bool matched = false;

        for (uint8_t i = 0u; i < app_shell_command_count; i++)
        {
            if (strcmp(argv[0], app_shell_commands[i].name) == 0)
            {
                app_shell_commands[i].handler(argc, argv, print);
                matched = true;
                break;
            }
        }

        if (matched)
        {
            break;
        }

        print("Unknown command: %s  (try 'help')\n", argv[0]);
    } while (0);
}

/**** END OF FILE ****/
