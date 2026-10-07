/**
 ******************************************************************************
 * @file    sysmon.c
 * @author  Daniel Dyringer
 * @date    2026-09-30
 * @brief   System monitor — per-core CPU load.
 ******************************************************************************
 */


/* Includes ---------------------------------------------------------------- */
#include "sysmon.h"

#include <stdint.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "utils.h"


/* Private variables ------------------------------------------------------- */

static const char *sysmon_tag = "sysmon";

/* Previous sample — touched only by the esp_timer task */
static int64_t                     sysmon_prev_wall_us = 0;
static configRUN_TIME_COUNTER_TYPE sysmon_prev_idle[SYSMON_CORE_COUNT];

/** @brief Last completed window, percent; -1 until the first one. */
static volatile int8_t sysmon_load[SYSMON_CORE_COUNT];


/* Private functions ------------------------------------------------------- */

static void sysmon_sample(void *arg)
{
    (void)arg;

    /* Same clock as the run-time counters (FREERTOS_RUN_TIME_STATS_USING_ESP_TIMER). */
    const int64_t now     = esp_timer_get_time();
    const int64_t elapsed = now - sysmon_prev_wall_us;

    for (int core = 0; core < SYSMON_CORE_COUNT; core++)
    {
        const configRUN_TIME_COUNTER_TYPE idle = ulTaskGetIdleRunTimeCounterForCore(core);

        if (elapsed > 0)
        {
            const int64_t idle_delta = (int64_t)(idle - sysmon_prev_idle[core]);
            const int64_t load       = 100 - (idle_delta * 100) / elapsed;

            /* Counter and wall-clock reads are not atomic together, so allow
             * for a few microseconds of skew at either end. */
            sysmon_load[core] = (int8_t)CLAMP(load, (int64_t)0, (int64_t)100);
        }

        sysmon_prev_idle[core] = idle;
    }

    sysmon_prev_wall_us = now;
}


/* Exposed API ------------------------------------------------------------- */

esp_err_t sysmon_init(void)
{
    esp_err_t error_code = ESP_OK;

    do
    {
        for (int core = 0; core < SYSMON_CORE_COUNT; core++)
        {
            sysmon_load[core]      = -1;
            sysmon_prev_idle[core] = ulTaskGetIdleRunTimeCounterForCore(core);
        }

        sysmon_prev_wall_us = esp_timer_get_time();

        const esp_timer_create_args_t timer_args = {
            .callback = sysmon_sample,
            .name     = "sysmon",
        };

        esp_timer_handle_t timer = NULL;

        error_code = esp_timer_create(&timer_args, &timer);

        if (error_code != ESP_OK)
        {
            break;
        }

        error_code = esp_timer_start_periodic(timer,
                                              SYSMON_SAMPLE_PERIOD_MS * 1000ULL);
    } while (0);

    if (error_code != ESP_OK)
    {
        ESP_LOGE(sysmon_tag, "Init failed: %s", esp_err_to_name(error_code));
    }

    return error_code;
}

int sysmon_cpu_load(int core)
{
    return (core >= 0 && core < SYSMON_CORE_COUNT) ? sysmon_load[core] : -1;
}


/**** END OF FILE ****/
