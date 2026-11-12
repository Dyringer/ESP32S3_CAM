/**
 ******************************************************************************
 * @file    fake_idf.h
 * @author  Daniel Dyringer
 * @date    2026-10-24
 * @brief   Control side of the host ESP-IDF and FreeRTOS fakes.
 *
 *          A simulated clock drives a single fake esp_timer: advancing the
 *          clock past its deadline runs its callback. The single fake task
 *          runs its real loop: ulTaskNotifyTake returns while a notification
 *          is pending and jumps back to fake_idf_task_run() where the real
 *          task would block.
 ******************************************************************************
 */

/* Define to prevent recursive inclusion ----------------------------------- */
#ifndef FAKE_IDF_H_
#define FAKE_IDF_H_

/* Includes ---------------------------------------------------------------- */
#include <stdbool.h>
#include <stdint.h>

/* Constants and macros ---------------------------------------------------- */
// clang-format off


// clang-format on

/* Types ------------------------------------------------------------------- */

typedef enum fake_idf_log_level_e
{
    FAKE_IDF_LOG_ERROR   = 0,
    FAKE_IDF_LOG_WARNING = 1,
    FAKE_IDF_LOG_INFO    = 2,
} fake_idf_log_level_t;

typedef void (*fake_idf_log_hook_t)(const char* tag, fake_idf_log_level_t level);

/* Exposed API ------------------------------------------------------------- */

/* Clears counters and failure injection. Keeps the created task and timer,
 * because the code under test creates them only once per executable. */
void fake_idf_reset(void);

/* ---- Failure injection ---- */

void fake_idf_fail_timer_create(bool fail);
void fake_idf_fail_task_create(bool fail);

/* ---- Clock and timer ---- */

int64_t fake_idf_now_us(void);
/* Fires the timer callback when the clock passes its deadline. */
void fake_idf_advance_ms(uint32_t delay_ms);
bool fake_idf_timer_armed(void);
int64_t fake_idf_timer_deadline_us(void);
uint32_t fake_idf_timer_create_count(void);
uint32_t fake_idf_timer_delete_count(void);

/* ---- Task ---- */

bool fake_idf_task_created(void);
uint32_t fake_idf_task_create_count(void);
/* Runs the task loop until it would block on an empty notification. */
void fake_idf_task_run(void);
uint32_t fake_idf_notify_count(void);
/* ticks_to_wait of the last ulTaskNotifyTake call. */
uint32_t fake_idf_task_last_wait_ticks(void);

/* ---- Critical sections and logs ---- */

void fake_idf_critical_enter(void* mux);
void fake_idf_critical_exit(void* mux);
uint32_t fake_idf_critical_depth(void);
void fake_idf_log(const char* tag, fake_idf_log_level_t level);
/* Called for every log line; fake_idf_reset() removes it. */
void fake_idf_set_log_hook(fake_idf_log_hook_t hook);
uint32_t fake_idf_error_count(void);

#endif /* FAKE_IDF_H_ */

/**** END OF FILE ****/
