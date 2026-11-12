/**
 ******************************************************************************
 * @file    web_console.h
 * @author  Daniel Dyringer
 * @date    2026-03-09
 * @brief   Web console — ESP log capture and WebSocket push.
 *
 *          web_console_write() is safe to call from any task.
 ******************************************************************************
 */

/* Define to prevent recursive inclusion ----------------------------------- */
#ifndef WEB_CONSOLE_H_
#define WEB_CONSOLE_H_

/* Includes ---------------------------------------------------------------- */
#include "esp_err.h"

/* Constants and macros ---------------------------------------------------- */
// clang-format off

#define WEB_CONSOLE_WS_URI   "/console/ws"

// clang-format on

/* Types ------------------------------------------------------------------- */


/* Exposed API ------------------------------------------------------------- */

void web_console_write(const char* fmt, ...);
esp_err_t web_console_init(void);

#endif /* WEB_CONSOLE_H_ */

/**** END OF FILE ****/
