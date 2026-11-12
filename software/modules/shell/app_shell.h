/**
 ******************************************************************************
 * @file    app_shell.h
 * @author  Daniel Dyringer
 * @date    2026-03-12
 * @brief   Abstract shell — unified command table for UART and web consoles.
 ******************************************************************************
 */

/* Define to prevent recursive inclusion ----------------------------------- */
#ifndef APP_SHELL_H_
#define APP_SHELL_H_

/* Includes ---------------------------------------------------------------- */
#include "esp_err.h"


/* Constants and macros ---------------------------------------------------- */
// clang-format off


// clang-format on

/* Types ------------------------------------------------------------------- */

/* All command output must go through this — never printf() directly. */
typedef void (*app_shell_print_fn_t)(const char* fmt, ...);

typedef void (*app_shell_cmd_fn_t)(int argc, char** argv, app_shell_print_fn_t print);

/* Exposed API ------------------------------------------------------------- */

esp_err_t app_shell_register_cmd(const char* name, const char* help, app_shell_cmd_fn_t handler);
esp_err_t app_shell_init(void);
void app_shell_dispatch(char* line, app_shell_print_fn_t print);

#endif /* APP_SHELL_H_ */

/**** END OF FILE ****/
