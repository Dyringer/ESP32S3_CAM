/**
 ******************************************************************************
 * @file    smart_io_caps.h
 * @author  Daniel Dyringer
 * @date    2026-10-26
 * @brief   smart_io pin functions and capability bits used by board files.
 ******************************************************************************
 */

/* Define to prevent recursive inclusion ----------------------------------- */
#ifndef SMART_IO_CAPS_H_
#define SMART_IO_CAPS_H_

/* Includes ---------------------------------------------------------------- */
#include <stdint.h>


/* Constants and macros ---------------------------------------------------- */
// clang-format off

#define SMART_IO_CAP(function)  ((uint32_t)1u << (uint32_t)(function))

/* ---- Groups used by boards/<board>/bsp_smart_io.h ---- */
#define SMART_IO_CAP_DIGITAL    (SMART_IO_CAP(SMART_IO_FUNCTION_INPUT)              \
                                 | SMART_IO_CAP(SMART_IO_FUNCTION_OUTPUT))
#define SMART_IO_CAP_TIMER      (SMART_IO_CAP(SMART_IO_FUNCTION_PWM)                \
                                 | SMART_IO_CAP(SMART_IO_FUNCTION_PULSE))
#define SMART_IO_CAP_BUS        (SMART_IO_CAP(SMART_IO_FUNCTION_I2C_SDA)            \
                                 | SMART_IO_CAP(SMART_IO_FUNCTION_I2C_SCL)          \
                                 | SMART_IO_CAP(SMART_IO_FUNCTION_UART_TX)          \
                                 | SMART_IO_CAP(SMART_IO_FUNCTION_UART_RX))
#define SMART_IO_CAP_ADC        SMART_IO_CAP(SMART_IO_FUNCTION_ADC)
#define SMART_IO_CAP_GENERIC    (SMART_IO_CAP_DIGITAL | SMART_IO_CAP_TIMER | SMART_IO_CAP_BUS)
/* Board default: smart_io removes what the pad itself can't do (ADC, output). */
#define SMART_IO_CAP_ALL        (SMART_IO_CAP_GENERIC | SMART_IO_CAP_ADC)

// clang-format on

/* Types ------------------------------------------------------------------- */

/* Signals a single pin can carry. Bus roles need a different function on each
 * of their pins, so the list is per signal, not per role. */
typedef enum smart_io_function_e
{
    SMART_IO_FUNCTION_NONE    = 0, /* no pin; never a capability */
    SMART_IO_FUNCTION_INPUT   = 1,
    SMART_IO_FUNCTION_OUTPUT  = 2,
    SMART_IO_FUNCTION_ADC     = 3,
    SMART_IO_FUNCTION_PWM     = 4,
    SMART_IO_FUNCTION_PULSE   = 5,
    SMART_IO_FUNCTION_I2C_SDA = 6,
    SMART_IO_FUNCTION_I2C_SCL = 7,
    SMART_IO_FUNCTION_UART_TX = 8,
    SMART_IO_FUNCTION_UART_RX = 9,

    SMART_IO_FUNCTION_COUNT,
} smart_io_function_t;

_Static_assert(SMART_IO_FUNCTION_COUNT <= 32, "capability mask is a uint32_t");

/* Exposed API ------------------------------------------------------------- */


#endif /* SMART_IO_CAPS_H_ */

/**** END OF FILE ****/
