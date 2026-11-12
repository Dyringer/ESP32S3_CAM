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
#include "esp_err.h"
#include "sysmon.h"
#include "utils.h"


/* Constants and macros ---------------------------------------------------- */
// clang-format off


// clang-format on

/* Private types ----------------------------------------------------------- */


/* Private variables ------------------------------------------------------- */


/* Private functions ------------------------------------------------------- */

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
    sysmon_task_t* const tasks = malloc(SYSMON_MAX_TASKS * sizeof(sysmon_task_t));
    size_t count               = 0u;

    do
    {
        if (tasks == NULL)
        {
            print("Out of memory\n");
            break;
        }

        const esp_err_t error_code = sysmon_get_tasks(tasks, SYSMON_MAX_TASKS, &count);

        if (error_code != ESP_OK)
        {
            print("Task load unavailable: %s\n", esp_err_to_name(error_code));
            break;
        }

        print(
            "\nPer task, last %u ms (100 %% = one core fully busy):\n",
            (unsigned)SYSMON_SAMPLE_PERIOD_MS);
        print(
            "%-16s %6s  %4s  %4s  %17s\n",
            "Task",
            "CPU",
            "Prio",
            "Core",
            "Stack free/total");

        for (size_t i = 0u; i < count; i++)
        {
            const sysmon_task_t* const task = &tasks[i];
            char core[12]                   = {0};

            if (task->core == SYSMON_TASK_NO_AFFINITY)
            {
                strlcpy(core, "any", sizeof(core));
            }
            else
            {
                snprintf(core, sizeof(core), "%d", (int)task->core);
            }

            print(
                "%-16s %3u.%u%%  %4u  %4s  %6lu / %6lu B\n",
                task->name,
                (unsigned)(task->cpu_permille / 10u),
                (unsigned)(task->cpu_permille % 10u),
                (unsigned)task->priority,
                core,
                (unsigned long)task->stack_free,
                (unsigned long)task->stack_size);
        }
    } while (0);

    free(tasks);
}

static void sysmon_shell_cmd_cpu(int argc, char** argv, app_shell_print_fn_t print)
{
    ARG_UNUSED(argc);
    ARG_UNUSED(argv);

    sysmon_shell_print_cores(print);
    sysmon_shell_print_tasks(print);
}


/* Exposed API ------------------------------------------------------------- */

esp_err_t sysmon_shell_register(void)
{
    return app_shell_register_cmd(
        "cpu", "CPU load per core and per task (last 1 s window)", sysmon_shell_cmd_cpu);
}

/**** END OF FILE ****/
