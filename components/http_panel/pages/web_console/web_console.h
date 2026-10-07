/**
 ******************************************************************************
 * @file    web_console.h
 * @author  Daniel Dyringer
 * @date    2026-03-09
 * @brief   Web console — ESP log capture and WebSocket push.
 *
 *          Owns the ring buffer, ESP log vprintf hook, push task, and the
 *          HTTP/WS URI handlers for the console page.
 *
 *          web_shell.c is the only intended caller of web_console_write().
 *
 * @note    Private to the http_panel component.
 ******************************************************************************
 */

/* Define to prevent recursive inclusion ----------------------------------- */
#ifndef WEB_CONSOLE_H_
#define WEB_CONSOLE_H_

/* Includes ---------------------------------------------------------------- */

#include <stdarg.h>
#include <stddef.h>

#include "esp_err.h"

/* Constants and macros ---------------------------------------------------- */
// clang-format off

/** @brief WebSocket URI for the console stream. */
#define WEB_CONSOLE_WS_URI   "/console/ws"

// clang-format on

/* Exposed API ------------------------------------------------------------- */

/**
 * @brief Write a formatted string into the console ring buffer.
 *        Output will be pushed to all connected /console/ws clients.
 *        Safe to call from any task context.
 *
 * @param fmt  printf-style format string.
 * @param ...  Variadic arguments.
 */
void web_console_write(const char *fmt, ...);

/**
 * @brief Initialise the web console.
 *        Creates the ring buffer semaphore, registers HTTP and WS URI
 *        handlers, spawns the push task, and installs the ESP log hook.
 *        Call from http_panel_init() before web_shell_init().
 */
esp_err_t web_console_init(void);

#endif /* WEB_CONSOLE_H_ */

/**** END OF FILE ****/
