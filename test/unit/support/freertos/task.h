/**
 ******************************************************************************
 * @file    task.h
 * @author  Daniel Dyringer
 * @date    2026-10-24
 * @brief   Host stand-in for the FreeRTOS task.h used by unit tests. Task
 *          creation and notifications are recorded by fake_idf.
 ******************************************************************************
 */

/* Define to prevent recursive inclusion ----------------------------------- */
#ifndef TASK_H_
#define TASK_H_

/* Includes ---------------------------------------------------------------- */
#include "freertos/FreeRTOS.h"

/* Constants and macros ---------------------------------------------------- */
// clang-format off


// clang-format on

/* Types ------------------------------------------------------------------- */


/* Exposed API ------------------------------------------------------------- */

BaseType_t xTaskCreatePinnedToCore(
    TaskFunction_t task_function,
    const char* name,
    uint32_t stack_depth,
    void* parameters,
    UBaseType_t priority,
    TaskHandle_t* created_task,
    BaseType_t core_id);
BaseType_t xTaskNotifyGive(TaskHandle_t task_to_notify);
void vTaskNotifyGiveFromISR(TaskHandle_t task_to_notify, BaseType_t* higher_priority_task_woken);
uint32_t ulTaskNotifyTake(BaseType_t clear_count_on_exit, TickType_t ticks_to_wait);
TaskHandle_t xTaskGetCurrentTaskHandle(void);

#endif /* TASK_H_ */

/**** END OF FILE ****/
