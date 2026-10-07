/**
 ******************************************************************************
 * @file    uart_console.h
 * @author  Daniel Dyringer
 * @date    2026-02-17
 * @brief   UART console — init and REPL task.
 ******************************************************************************
 */

/* Define to prevent recursive inclusion ----------------------------------- */
#ifndef UART_CONSOLE_H_
#define UART_CONSOLE_H_

/* Includes ---------------------------------------------------------------- */

#include "esp_err.h"

/* Constants and macros ---------------------------------------------------- */
// clang-format off

/** @brief FreeRTOS core the REPL task is pinned to.
 *  @note  Core 1 is the application core; Core 0 is reserved for network. */
#define UART_CONSOLE_CORE  1

// clang-format on

/* Exposed API ------------------------------------------------------------- */

/**
 * @brief Configure UART and initialise linenoise.
 *        Call before app_shell_init() and all command registrations.
 */
esp_err_t uart_console_init(void);

/**
 * @brief Spawn the REPL as a FreeRTOS task pinned to UART_CONSOLE_CORE.
 *        Call after uart_console_init() and app_shell_init().
 */
esp_err_t uart_console_start(void);

#endif /* UART_CONSOLE_H_ */

/**** END OF FILE ****/
