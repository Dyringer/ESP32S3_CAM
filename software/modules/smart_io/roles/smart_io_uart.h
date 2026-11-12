/**
 ******************************************************************************
 * @file    smart_io_uart.h
 * @author  Daniel Dyringer
 * @date    2026-10-31
 * @brief   uart role: UART driver, non-blocking TX, RX source for the stream.
 ******************************************************************************
 */

/* Define to prevent recursive inclusion ----------------------------------- */
#ifndef SMART_IO_UART_H_
#define SMART_IO_UART_H_

/* Includes ---------------------------------------------------------------- */
#include "smart_io_role.h"


/* Constants and macros ---------------------------------------------------- */
// clang-format off


// clang-format on

/* Types ------------------------------------------------------------------- */


/* Exposed API ------------------------------------------------------------- */

extern const smart_io_role_api_t smart_io_uart_api;

#endif /* SMART_IO_UART_H_ */

/**** END OF FILE ****/
