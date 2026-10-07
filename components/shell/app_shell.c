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

#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "esp_log.h"


/* Constants and macros ---------------------------------------------------- */
// clang-format off

/** @brief Maximum number of registered commands (includes built-in "help"). */
#define APP_SHELL_CMD_TABLE_SIZE    24u

/** @brief Maximum number of tokens per command line (a quoted string counts
 *         as one). */
#define APP_SHELL_ARGC_MAX          8u

/** @brief Maximum echo line length: "> " + line + "\n" + NUL. */
#define APP_SHELL_ECHO_MAX          258u

// clang-format on


/* Private types ----------------------------------------------------------- */

typedef struct
{
    const char           *name;
    const char           *help;
    app_shell_cmd_fn_t    handler;
} app_shell_cmd_entry_t;


/* Private variables ------------------------------------------------------- */

static const char *app_shell_tag = "app_shell";

static app_shell_cmd_entry_t app_shell_cmds[APP_SHELL_CMD_TABLE_SIZE];
static uint8_t               app_shell_cmd_count = 0u;


/* Private functions ------------------------------------------------------- */

static void app_shell_cmd_help(int                   argc,
                                char                **argv,
                                app_shell_print_fn_t  print)
{
    (void)argc;
    (void)argv;
    print("Available commands:\n");

    for (uint8_t i = 0u; i < app_shell_cmd_count; i++)
    {
        print("  %-16s %s\n",
              app_shell_cmds[i].name,
              app_shell_cmds[i].help ? app_shell_cmds[i].help : "");
    }
}

/**
 * @brief Split @p line in place into whitespace-separated tokens.
 *
 *        Double quotes group text containing spaces into one token and are
 *        removed: `auth cam "my secret"` -> {auth, cam, my secret}. Inside
 *        quotes, \" and \\ stand for a literal quote and backslash; `""` is
 *        an empty token. Outside quotes a backslash is an ordinary character.
 *        Tokens beyond @p argc_max are ignored.
 *
 *        Rewriting in place is safe: removing quotes and escapes only ever
 *        moves characters left, so the write position never passes the read
 *        position.
 *
 * @return Token count, or -1 on an unterminated quote.
 */
static int app_shell_tokenise(char *line, char **argv, int argc_max)
{
    char *src  = line;
    char *dst  = line;
    int   argc = 0;

    for (;;)
    {
        while (*src == ' ' || *src == '\t')
        {
            src++;
        }

        if (*src == '\0' || argc >= argc_max)
        {
            break;
        }

        argv[argc++] = dst;

        bool quoted = false;

        while (*src != '\0' && (quoted || (*src != ' ' && *src != '\t')))
        {
            if (*src == '"')
            {
                quoted = !quoted;
                src++;
                continue;
            }

            if (quoted && *src == '\\' && (src[1] == '"' || src[1] == '\\'))
            {
                src++;
            }

            *dst++ = *src++;
        }

        if (quoted)
        {
            return -1;
        }

        if (*src != '\0')
        {
            src++;  /* step over the separator before terminating the token */
        }

        *dst++ = '\0';
    }

    return argc;
}


/* Exposed API ------------------------------------------------------------- */

esp_err_t app_shell_register_cmd(const char           *name,
                                  const char           *help,
                                  app_shell_cmd_fn_t    handler)
{
    esp_err_t error_code;

    if (app_shell_cmd_count >= APP_SHELL_CMD_TABLE_SIZE)
    {
        ESP_LOGE(app_shell_tag, "Command table full");
        error_code = ESP_ERR_NO_MEM;
    }
    else
    {
        app_shell_cmds[app_shell_cmd_count].name    = name;
        app_shell_cmds[app_shell_cmd_count].help    = help;
        app_shell_cmds[app_shell_cmd_count].handler = handler;
        app_shell_cmd_count++;

        error_code = ESP_OK;
    }

    return error_code;
}

esp_err_t app_shell_init(void)
{
    return app_shell_register_cmd("help", "List available commands",
                                  app_shell_cmd_help);
}

void app_shell_dispatch(char *line, app_shell_print_fn_t print)
{
    do
    {
        /* Echo input back to caller's transport */
        char echo[APP_SHELL_ECHO_MAX];
        snprintf(echo, sizeof(echo), "> %s\n", line);
        print("%s", echo);

        /* Tokenise in-place */
        char     *argv[APP_SHELL_ARGC_MAX];
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

        for (uint8_t i = 0u; i < app_shell_cmd_count; i++)
        {
            if (strcmp(argv[0], app_shell_cmds[i].name) == 0)
            {
                app_shell_cmds[i].handler(argc, argv, print);
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
