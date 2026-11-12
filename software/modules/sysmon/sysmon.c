/**
 ******************************************************************************
 * @file    sysmon.c
 * @author  Daniel Dyringer
 * @date    2026-09-30
 * @brief   System monitor — per-core and per-task CPU load.
 ******************************************************************************
 */


/* Includes ---------------------------------------------------------------- */
#include "sysmon.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "esp_attr.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_memory_utils.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/freertos_debug.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "utils.h"
#include "work_queue.h"


/* Constants and macros ---------------------------------------------------- */
// clang-format off

#define SYSMON_LOCK_TIMEOUT_MS  100u

/* Sanity bound for a stack size read from a TCB that may already be freed. */
#define SYSMON_STACK_SIZE_MAX   (256u * 1024u)

// clang-format on

/* Private types ----------------------------------------------------------- */


/* Private variables ------------------------------------------------------- */

static const char* sysmon_tag = "sysmon";

/* Touched only by the work_queue task. */
static work_queue_item_t sysmon_work                                       = {0};
static int64_t sysmon_previous_wall_us                                     = 0;
static configRUN_TIME_COUNTER_TYPE sysmon_previous_idle[SYSMON_CORE_COUNT] = {0};
static TaskStatus_t* sysmon_status_previous                                = NULL;
static TaskStatus_t* sysmon_status_current                                 = NULL;
static UBaseType_t sysmon_status_previous_count                            = 0u;
static configRUN_TIME_COUNTER_TYPE sysmon_status_previous_time             = 0u;
static bool sysmon_overflow_logged                                         = false;

/* Percent; -1 until the first window completes. Written by the work_queue
 * task, read from any task. */
static volatile int8_t sysmon_load[SYSMON_CORE_COUNT] = {0};

/* Guarded by sysmon_tasks_mutex */
static SemaphoreHandle_t sysmon_tasks_mutex = NULL;
static size_t sysmon_tasks_count            = 0u;

/* Guarded by sysmon_tasks_mutex. PSRAM: never touch from an ISR or while the
 * flash cache is disabled. */
EXT_RAM_BSS_ATTR static sysmon_task_t sysmon_tasks[SYSMON_MAX_TASKS] = {0};


/* Private functions ------------------------------------------------------- */

static int sysmon_task_compare(const void* left, const void* right)
{
    const uint16_t left_load  = ((const sysmon_task_t*)left)->cpu_permille;
    const uint16_t right_load = ((const sysmon_task_t*)right)->cpu_permille;

    int result = 0;

    /* Descending */
    if (left_load < right_load)
    {
        result = 1;
    }
    else if (left_load > right_load)
    {
        result = -1;
    }
    else
    {
        /* Equal loads compare as 0 */
    }

    return result;
}

/* Matched by xTaskNumber: a deleted task's handle can be reused within a window.
 * A task created inside the window counts from 0. */
static configRUN_TIME_COUNTER_TYPE sysmon_previous_counter(const TaskStatus_t* status)
{
    configRUN_TIME_COUNTER_TYPE counter = 0u;

    for (UBaseType_t i = 0u; i < sysmon_status_previous_count; i++)
    {
        if (sysmon_status_previous[i].xTaskNumber == status->xTaskNumber)
        {
            counter = sysmon_status_previous[i].ulRunTimeCounter;
            break;
        }
    }

    return counter;
}

/* TaskStatus_t has no stack size on a downward-growing stack. pxEndOfStack is
 * the highest stack slot aligned down to portBYTE_ALIGNMENT, so round back up.
 * The task may have been deleted since uxTaskGetSystemState(): out-of-range
 * results are reported as unknown. */
static uint32_t sysmon_stack_size(const TaskStatus_t* status)
{
    uint32_t size           = 0u;
    TaskSnapshot_t snapshot = {0};
    const uintptr_t base    = (uintptr_t)status->pxStackBase;

    if (vTaskGetSnapshot(status->xHandle, &snapshot) == pdTRUE)
    {
        const uintptr_t end = (uintptr_t)snapshot.pxEndOfStack;

        if ((end > base) && ((end - base) < SYSMON_STACK_SIZE_MAX))
        {
            size =
                (uint32_t)((((end - base) / portBYTE_ALIGNMENT) + 1u) * portBYTE_ALIGNMENT);
        }
    }

    return size;
}

static void sysmon_fill_task(
    const TaskStatus_t* status,
    configRUN_TIME_COUNTER_TYPE window,
    sysmon_task_t* task)
{
    const configRUN_TIME_COUNTER_TYPE start = sysmon_previous_counter(status);
    const configRUN_TIME_COUNTER_TYPE delta =
        (status->ulRunTimeCounter > start) ? (status->ulRunTimeCounter - start) : 0u;
    const configRUN_TIME_COUNTER_TYPE permille = (delta * 1000u) / window;

    strlcpy(task->name, status->pcTaskName, sizeof(task->name));
    task->stack_free     = (uint32_t)status->usStackHighWaterMark;
    task->stack_size     = sysmon_stack_size(status);
    task->state          = status->eCurrentState;
    task->cpu_permille   = (uint16_t)MIN(permille, (configRUN_TIME_COUNTER_TYPE)1000u);
    task->priority       = (uint8_t)status->uxCurrentPriority;
    task->base_priority  = (uint8_t)status->uxBasePriority;
    task->stack_in_psram = esp_ptr_external_ram(status->pxStackBase);
    task->core           = (int8_t)SYSMON_TASK_NO_AFFINITY;

    if (status->xCoreID != tskNO_AFFINITY)
    {
        task->core = (int8_t)status->xCoreID;
    }
}

static void sysmon_publish_tasks(UBaseType_t count, configRUN_TIME_COUNTER_TYPE window)
{
    if (xSemaphoreTake(sysmon_tasks_mutex, pdMS_TO_TICKS(SYSMON_LOCK_TIMEOUT_MS)) == pdTRUE)
    {
        for (UBaseType_t i = 0u; i < count; i++)
        {
            sysmon_fill_task(&sysmon_status_current[i], window, &sysmon_tasks[i]);
        }

        qsort(sysmon_tasks, count, sizeof(sysmon_tasks[0]), sysmon_task_compare);
        sysmon_tasks_count = count;

        xSemaphoreGive(sysmon_tasks_mutex);
    }
}

static void sysmon_sample_tasks(void)
{
    configRUN_TIME_COUNTER_TYPE time = 0u;
    const UBaseType_t count =
        uxTaskGetSystemState(sysmon_status_current, SYSMON_MAX_TASKS, &time);

    if (count == 0u)
    {
        if (!sysmon_overflow_logged)
        {
            ESP_LOGW(
                sysmon_tag,
                "%u tasks exceed SYSMON_MAX_TASKS, per-task load paused",
                (unsigned)uxTaskGetNumberOfTasks());
            sysmon_overflow_logged = true;
        }
    }
    else if ((sysmon_status_previous_count > 0u) && (time > sysmon_status_previous_time))
    {
        sysmon_publish_tasks(count, time - sysmon_status_previous_time);
    }
    else
    {
        /* First snapshot: no window to diff against yet. */
    }

    TaskStatus_t* const swap     = sysmon_status_previous;
    sysmon_status_previous       = sysmon_status_current;
    sysmon_status_current        = swap;
    sysmon_status_previous_count = count;
    sysmon_status_previous_time  = time;
}

static void sysmon_sample_load(void)
{
    /* Same clock as the run-time counters (FREERTOS_RUN_TIME_STATS_USING_ESP_TIMER). */
    const int64_t now     = esp_timer_get_time();
    const int64_t elapsed = now - sysmon_previous_wall_us;

    for (BaseType_t core = 0; core < SYSMON_CORE_COUNT; core++)
    {
        const configRUN_TIME_COUNTER_TYPE idle = ulTaskGetIdleRunTimeCounterForCore(core);

        if (elapsed > 0)
        {
            const int64_t idle_delta = (int64_t)(idle - sysmon_previous_idle[core]);
            const int64_t load       = 100 - ((idle_delta * 100) / elapsed);

            /* Counter and wall-clock reads are not atomic together, so allow
             * for a few microseconds of skew at either end. */
            sysmon_load[core] = (int8_t)CLAMP(load, (int64_t)0, (int64_t)100);
        }

        sysmon_previous_idle[core] = idle;
    }

    sysmon_previous_wall_us = now;
}

/* The load uses the measured window, so a late run skews nothing.
 * uxTaskGetSystemState() suspends the scheduler while it scans every stack,
 * about a millisecond. */
static void sysmon_sample(void* context)
{
    ARG_UNUSED(context);

    sysmon_sample_load();
    sysmon_sample_tasks();

    work_queue_schedule(&sysmon_work, SYSMON_SAMPLE_PERIOD_MS);
}

static esp_err_t sysmon_tasks_init(void)
{
    esp_err_t error_code = ESP_OK;

    do
    {
        sysmon_tasks_mutex = xSemaphoreCreateMutex();

        if (sysmon_tasks_mutex == NULL)
        {
            error_code = ESP_ERR_NO_MEM;
            break;
        }

        sysmon_status_previous =
            heap_caps_calloc(SYSMON_MAX_TASKS, sizeof(TaskStatus_t), MALLOC_CAP_SPIRAM);
        sysmon_status_current =
            heap_caps_calloc(SYSMON_MAX_TASKS, sizeof(TaskStatus_t), MALLOC_CAP_SPIRAM);

        if ((sysmon_status_previous == NULL) || (sysmon_status_current == NULL))
        {
            error_code = ESP_ERR_NO_MEM;
        }
    } while (0);

    return error_code;
}


/* Exposed API ------------------------------------------------------------- */

esp_err_t sysmon_init(void)
{
    esp_err_t error_code = ESP_OK;

    do
    {
        error_code = sysmon_tasks_init();

        if (error_code != ESP_OK)
        {
            break;
        }

        for (BaseType_t core = 0; core < SYSMON_CORE_COUNT; core++)
        {
            sysmon_load[core]          = -1;
            sysmon_previous_idle[core] = ulTaskGetIdleRunTimeCounterForCore(core);
        }

        sysmon_previous_wall_us = esp_timer_get_time();

        error_code = work_queue_item_init(&sysmon_work, sysmon_sample, NULL);

        if (error_code != ESP_OK)
        {
            break;
        }

        error_code = work_queue_schedule(&sysmon_work, SYSMON_SAMPLE_PERIOD_MS);
    } while (0);

    if (error_code != ESP_OK)
    {
        ESP_LOGE(sysmon_tag, "Init failed: %s", esp_err_to_name(error_code));
    }

    return error_code;
}

int sysmon_cpu_load(int core)
{
    return ((core >= 0) && (core < SYSMON_CORE_COUNT)) ? sysmon_load[core] : -1;
}

esp_err_t sysmon_get_tasks(sysmon_task_t* tasks, size_t capacity, size_t* count)
{
    esp_err_t error_code = ESP_OK;

    *count = 0u;

    do
    {
        if (sysmon_tasks_mutex == NULL)
        {
            error_code = ESP_ERR_INVALID_STATE;
            break;
        }

        if (xSemaphoreTake(sysmon_tasks_mutex, pdMS_TO_TICKS(SYSMON_LOCK_TIMEOUT_MS))
            != pdTRUE)
        {
            error_code = ESP_ERR_TIMEOUT;
            break;
        }

        *count = MIN(sysmon_tasks_count, capacity);
        memcpy(tasks, sysmon_tasks, *count * sizeof(tasks[0]));

        xSemaphoreGive(sysmon_tasks_mutex);

        if (*count == 0u)
        {
            error_code = ESP_ERR_INVALID_STATE;
        }
    } while (0);

    return error_code;
}

/**** END OF FILE ****/
