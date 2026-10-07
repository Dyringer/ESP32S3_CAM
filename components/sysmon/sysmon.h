/**
 ******************************************************************************
 * @file    sysmon.h
 * @author  Daniel Dyringer
 * @date    2026-09-30
 * @brief   System monitor — per-core CPU load.
 *
 *          Load is derived from FreeRTOS run-time stats: once per
 *          SYSMON_SAMPLE_PERIOD_MS the idle-task run time of each core is
 *          compared with wall time, load = 100 % - idle share. Readers get
 *          the last completed window, so any number of them (dashboard,
 *          camera page, shell) see the same value without disturbing it.
 *
 * @note    Requires CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS (sdkconfig.defaults).
 ******************************************************************************
 */

/* Define to prevent recursive inclusion ----------------------------------- */
#ifndef SYSMON_H_
#define SYSMON_H_

/* Includes ---------------------------------------------------------------- */

#include "esp_err.h"
#include "sdkconfig.h"

/* Constants and macros ---------------------------------------------------- */
// clang-format off

#define SYSMON_CORE_COUNT           CONFIG_FREERTOS_NUMBER_OF_CORES

/** @brief Length of the CPU load measurement window. */
#define SYSMON_SAMPLE_PERIOD_MS     1000u

// clang-format on

/* Exposed API ------------------------------------------------------------- */

/**
 * @brief Start the periodic CPU load sampler. Call once at boot.
 */
esp_err_t sysmon_init(void);

/**
 * @brief CPU load of @p core over the last sample window.
 *
 * @return 0-100 percent, or -1 if @p core is invalid or no window has
 *         completed yet (first SYSMON_SAMPLE_PERIOD_MS after boot).
 */
int sysmon_cpu_load(int core);

#endif /* SYSMON_H_ */

/**** END OF FILE ****/
