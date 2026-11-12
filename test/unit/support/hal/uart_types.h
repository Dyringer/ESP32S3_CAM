/**
 ******************************************************************************
 * @file    uart_types.h
 * @author  Daniel Dyringer
 * @date    2026-10-26
 * @brief   Host stand-in for the ESP-IDF hal/uart_types.h.
 ******************************************************************************
 */

/* Define to prevent recursive inclusion ----------------------------------- */
#ifndef HAL_UART_TYPES_H_
#define HAL_UART_TYPES_H_

/* Includes ---------------------------------------------------------------- */


/* Constants and macros ---------------------------------------------------- */
// clang-format off


// clang-format on

/* Types ------------------------------------------------------------------- */

typedef enum
{
    UART_NUM_0 = 0,
    UART_NUM_1,
    UART_NUM_2,
    UART_NUM_MAX,
} uart_port_t;


/* Exposed API ------------------------------------------------------------- */


#endif /* HAL_UART_TYPES_H_ */

/**** END OF FILE ****/
