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


// clang-format on

/* Types ------------------------------------------------------------------- */


/* Exposed API ------------------------------------------------------------- */

esp_err_t uart_console_init(void);
esp_err_t uart_console_start(void);

#endif /* UART_CONSOLE_H_ */

/**** END OF FILE ****/
