/**
 ******************************************************************************
 * @file    sysmon_shell.c
 * @author  Daniel Dyringer
 * @date    2026-09-30
 * @brief   System monitor shell commands — cpu.
 ******************************************************************************
 */


/* Includes ---------------------------------------------------------------- */
#include "sysmon_shell.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "app_shell.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sysmon.h"


/* Constants and macros ---------------------------------------------------- */
// clang-format off

/** @brief Per-task measurement window. */
#define SYSMON_SHELL_WINDOW_MS  1000u

/** @brief Headroom for tasks created between sizing and sampling. */
#define SYSMON_SHELL_SPARE      4u

// clang-format on


/* Private functions ------------------------------------------------------- */

/** @brief qsort comparator — ulRunTimeCounter holds the window delta here. */
static int sysmon_shell_task_cmp(const void *a, const void *b)
{
    const configRUN_TIME_COUNTER_TYPE da = ((const TaskStatus_t *)a)->ulRunTimeCounter;
    const configRUN_TIME_COUNTER_TYPE db = ((const TaskStatus_t *)b)->ulRunTimeCounter;

    return (da < db) - (da > db);   /* descending */
}

static void sysmon_shell_print_cores(app_shell_print_fn_t print)
{
    for (int core = 0; core < SYSMON_CORE_COUNT; core++)
    {
        const int load = sysmon_cpu_load(core);

        if (load < 0)
        {
            print("Core %d   : measuring...\n", core);
        }
        else
        {
            print("Core %d   : %3d %%\n", core, load);
        }
    }
}

static void sysmon_shell_print_tasks(app_shell_print_fn_t print)
{
    const UBaseType_t capacity = uxTaskGetNumberOfTasks() + SYSMON_SHELL_SPARE;
    TaskStatus_t     *before   = malloc(capacity * sizeof(TaskStatus_t));
    TaskStatus_t     *after    = malloc(capacity * sizeof(TaskStatus_t));

    do
    {
        if (!before || !after)
        {
            print("Out of memory\n");
            break;
        }

        configRUN_TIME_COUNTER_TYPE t0 = 0;
        configRUN_TIME_COUNTER_TYPE t1 = 0;

        const UBaseType_t n0 = uxTaskGetSystemState(before, capacity, &t0);
        vTaskDelay(pdMS_TO_TICKS(SYSMON_SHELL_WINDOW_MS));
        const UBaseType_t n1 = uxTaskGetSystemState(after, capacity, &t1);

        if (n0 == 0u || n1 == 0u || t1 <= t0)
        {
            print("Task snapshot failed\n");
            break;
        }

        const configRUN_TIME_COUNTER_TYPE window = t1 - t0;

        /* Turn each counter into its delta over the window, in place.
         * Match by handle; a task created inside the window counts from 0. */
        for (UBaseType_t i = 0u; i < n1; i++)
        {
            configRUN_TIME_COUNTER_TYPE start = 0;

            for (UBaseType_t j = 0u; j < n0; j++)
            {
                if (before[j].xHandle == after[i].xHandle)
                {
                    start = before[j].ulRunTimeCounter;
                    break;
                }
            }

            after[i].ulRunTimeCounter -= start;
        }

        qsort(after, n1, sizeof(after[0]), sysmon_shell_task_cmp);

        print("\nPer task, last %u ms (100 %% = one core fully busy):\n",
              (unsigned)SYSMON_SHELL_WINDOW_MS);
        print("%-16s %6s  %4s  %4s  %10s\n", "Task", "CPU", "Prio", "Core", "Stack free");

        for (UBaseType_t i = 0u; i < n1; i++)
        {
            const TaskStatus_t *task    = &after[i];
            const uint32_t      pct_x10 = (uint32_t)((task->ulRunTimeCounter * 1000u) / window);
            char                core[12];

            if (task->xCoreID == tskNO_AFFINITY)
            {
                strlcpy(core, "any", sizeof(core));
            }
            else
            {
                snprintf(core, sizeof(core), "%d", (int)task->xCoreID);
            }

            print("%-16s %3lu.%lu%%  %4u  %4s  %8lu B\n",
                  task->pcTaskName,
                  (unsigned long)(pct_x10 / 10u),
                  (unsigned long)(pct_x10 % 10u),
                  (unsigned)task->uxCurrentPriority,
                  core,
                  (unsigned long)task->usStackHighWaterMark);
        }
    } while (0);

    free(after);
    free(before);
}

static void sysmon_shell_cmd_cpu(int argc, char **argv, app_shell_print_fn_t print)
{
    (void)argc;
    (void)argv;

    sysmon_shell_print_cores(print);
    sysmon_shell_print_tasks(print);
}


/* Exposed API ------------------------------------------------------------- */

esp_err_t sysmon_shell_register(void)
{
    return app_shell_register_cmd(
        "cpu",
        "CPU load per core and per task (1 s sample)",
        sysmon_shell_cmd_cpu);
}


/**** END OF FILE ****/
