/**
 ******************************************************************************
 * @file    sysmon.h
 * @author  Daniel Dyringer
 * @date    2026-09-30
 * @brief   System monitor — per-core and per-task CPU load.
 *
 *          Load = 100 % - idle-task share of each SYSMON_SAMPLE_PERIOD_MS
 *          window. Readers get the last completed window, so reading never
 *          disturbs the measurement.
 *
 * @note    Requires CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS (sdkconfig.defaults).
 ******************************************************************************
 */

/* Define to prevent recursive inclusion ----------------------------------- */
#ifndef SYSMON_H_
#define SYSMON_H_

/* Includes ---------------------------------------------------------------- */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"

/* Constants and macros ---------------------------------------------------- */
// clang-format off

#define SYSMON_CORE_COUNT           CONFIG_FREERTOS_NUMBER_OF_CORES
#define SYSMON_SAMPLE_PERIOD_MS     1000u

/* Task table capacity; the per-task readout pauses while more tasks exist. */
#define SYSMON_MAX_TASKS            40u

#define SYSMON_TASK_NO_AFFINITY     (-1)

// clang-format on

/* Types ------------------------------------------------------------------- */

typedef struct sysmon_task_s
{
    char name[CONFIG_FREERTOS_MAX_TASK_NAME_LEN];
    uint32_t stack_free; /* bytes, lowest since the task was created */
    uint32_t stack_size; /* bytes, 0 if unknown */
    eTaskState state;
    uint16_t cpu_permille; /* of one core over the last window */
    uint8_t priority;
    uint8_t base_priority;
    int8_t core; /* SYSMON_TASK_NO_AFFINITY when unpinned */
    bool stack_in_psram;
} sysmon_task_t;

/* Exposed API ------------------------------------------------------------- */

esp_err_t sysmon_init(void);
int sysmon_cpu_load(int core);

/* Sorted by CPU load, busiest first. ESP_ERR_INVALID_STATE until the first
 * window completes. */
esp_err_t sysmon_get_tasks(sysmon_task_t* tasks, size_t capacity, size_t* count);

#endif /* SYSMON_H_ */

/**** END OF FILE ****/
