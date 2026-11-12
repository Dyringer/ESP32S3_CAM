/**
 ******************************************************************************
 * @file    semphr.h
 * @author  Daniel Dyringer
 * @date    2026-10-26
 * @brief   Host stand-in for the FreeRTOS semphr.h: one recursive mutex whose
 *          nesting depth is tracked by fake_smart_io.
 ******************************************************************************
 */

/* Define to prevent recursive inclusion ----------------------------------- */
#ifndef SEMPHR_H_
#define SEMPHR_H_

/* Includes ---------------------------------------------------------------- */
#include "freertos/FreeRTOS.h"

/* Constants and macros ---------------------------------------------------- */
// clang-format off


// clang-format on

/* Types ------------------------------------------------------------------- */

typedef struct fake_smart_io_semaphore_s* SemaphoreHandle_t;


/* Exposed API ------------------------------------------------------------- */

SemaphoreHandle_t xSemaphoreCreateRecursiveMutex(void);
BaseType_t xSemaphoreTakeRecursive(SemaphoreHandle_t semaphore, TickType_t ticks_to_wait);
BaseType_t xSemaphoreGiveRecursive(SemaphoreHandle_t semaphore);
SemaphoreHandle_t xSemaphoreCreateMutex(void);
BaseType_t xSemaphoreTake(SemaphoreHandle_t semaphore, TickType_t ticks_to_wait);
BaseType_t xSemaphoreGive(SemaphoreHandle_t semaphore);
TaskHandle_t xSemaphoreGetMutexHolder(SemaphoreHandle_t semaphore);
void vSemaphoreDelete(SemaphoreHandle_t semaphore);


#endif /* SEMPHR_H_ */

/**** END OF FILE ****/
