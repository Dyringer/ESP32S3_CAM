/**
 ******************************************************************************
 * @file    FreeRTOS.h
 * @author  Daniel Dyringer
 * @date    2026-10-24
 * @brief   Host stand-in for the FreeRTOS.h used by unit tests. Critical
 *          sections only track their nesting depth in fake_idf.
 ******************************************************************************
 */

/* Define to prevent recursive inclusion ----------------------------------- */
#ifndef FREERTOS_H_
#define FREERTOS_H_

/* Includes ---------------------------------------------------------------- */
#include <stdint.h>

#include "fake_idf.h"

/* Constants and macros ---------------------------------------------------- */
// clang-format off

#define pdFALSE                         ((BaseType_t)0)
#define pdTRUE                          ((BaseType_t)1)
#define pdPASS                          pdTRUE
#define pdFAIL                          pdFALSE
#define portMAX_DELAY                   ((TickType_t)0xFFFFFFFFu)
#define pdMS_TO_TICKS(time_ms)          ((TickType_t)(time_ms))     /* 1 kHz tick */

#define portMUX_INITIALIZER_UNLOCKED    {0}

#define taskENTER_CRITICAL(mux)         fake_idf_critical_enter(mux)
#define taskEXIT_CRITICAL(mux)          fake_idf_critical_exit(mux)
#define taskENTER_CRITICAL_ISR(mux)     fake_idf_critical_enter(mux)
#define taskEXIT_CRITICAL_ISR(mux)      fake_idf_critical_exit(mux)
#define portYIELD_FROM_ISR(...)         do {} while (0)

// clang-format on

/* Types ------------------------------------------------------------------- */

typedef int32_t BaseType_t;
typedef uint32_t UBaseType_t;
typedef uint32_t TickType_t;

typedef struct
{
    uint32_t owner;
} portMUX_TYPE;

typedef struct tskTaskControlBlock* TaskHandle_t;
typedef void (*TaskFunction_t)(void* parameters);

/* Exposed API ------------------------------------------------------------- */


#endif /* FREERTOS_H_ */

/**** END OF FILE ****/
