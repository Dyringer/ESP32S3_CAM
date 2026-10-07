/**
 ******************************************************************************
 * @file    app_shell.h
 * @author  Daniel Dyringer
 * @date    2026-03-12
 * @brief   Abstract shell — unified command table for UART and web consoles.
 *
 *          Commands are registered once via app_shell_register_cmd().
 *          Dispatch is transport-agnostic: each caller supplies its own
 *          print callback, so output goes to whichever transport invoked
 *          the command (UART or WebSocket).
 *
 *          Typical usage:
 *            1. Call app_shell_register_cmd() for every command module.
 *            2. Call app_shell_init() to register the built-in "help" command.
 *            3. UART REPL: app_shell_dispatch(line, uart_print_fn)
 *               Web console: app_shell_dispatch(line, ws_print_fn)
 ******************************************************************************
 */

/* Define to prevent recursive inclusion ----------------------------------- */
#ifndef APP_SHELL_H_
#define APP_SHELL_H_

/* Includes ---------------------------------------------------------------- */

#include "esp_err.h"

/* Types ------------------------------------------------------------------- */

/**
 * @brief Output function passed to every command handler.
 *        Use this to write all command output — do not call printf() directly.
 *
 * @param fmt  printf-style format string.
 * @param ...  Variadic arguments.
 */
typedef void (*app_shell_print_fn_t)(const char *fmt, ...);

/**
 * @brief Command handler signature.
 *
 * @param argc   Argument count (includes command name at argv[0]).
 * @param argv   Null-terminated argument vector.
 * @param print  Output callback — all output must go through this.
 */
typedef void (*app_shell_cmd_fn_t)(int                   argc,
                                   char                **argv,
                                   app_shell_print_fn_t  print);

/* Exposed API ------------------------------------------------------------- */

/**
 * @brief Register a command in the global command table.
 *        Must be called before app_shell_init().
 *
 * @param name     Command name matched against the first token of the line.
 * @param help     One-line description shown by the built-in "help" command.
 *                 May be NULL.
 * @param handler  Function called when the command is matched.
 *
 * @return ESP_OK on success, ESP_ERR_NO_MEM if the table is full.
 */
esp_err_t app_shell_register_cmd(const char           *name,
                                  const char           *help,
                                  app_shell_cmd_fn_t    handler);

/**
 * @brief Initialise the shell.
 *        Registers the built-in "help" command.
 *        Call once after all app_shell_register_cmd() calls.
 *
 * @return ESP_OK on success, ESP_ERR_NO_MEM if the table is full.
 */
esp_err_t app_shell_init(void);

/**
 * @brief Tokenise @p line and dispatch to the matching command.
 *
 *        Tokens are separated by spaces/tabs. Double quotes keep spaces
 *        inside one token and are stripped (`"my secret"` -> my secret);
 *        inside quotes, \" and \\ give a literal quote and backslash. A line
 *        with an unterminated quote is rejected without dispatching.
 *
 *        Echoes the raw input prefixed with "> " through @p print.
 *        Calls the matching handler with @p print as the output callback.
 *        Prints an "Unknown command" message if no match is found.
 *
 * @param line   NUL-terminated command line; modified in-place by the
 *               tokeniser.
 * @param print  Output callback used for echo, command output, and errors.
 */
void app_shell_dispatch(char *line, app_shell_print_fn_t print);

#endif /* APP_SHELL_H_ */

/**** END OF FILE ****/
