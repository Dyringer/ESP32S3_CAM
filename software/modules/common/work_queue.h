/**
 ******************************************************************************
 * @file    work_queue.h
 * @author  Daniel Dyringer
 * @date    2026-10-11
 * @brief   System work queue: deferred work run by a single task, modelled on
 *          the Zephyr k_work API.
 *
 *          push       - run as soon as the work_queue task gets the CPU. A
 *                       scheduled item is moved forward to run now.
 *          schedule   - run after delay_ms. No-op if the item is already
 *                       scheduled or queued, so bursts collapse into one run
 *                       at the first deadline.
 *          reschedule - replace any pending run with one after delay_ms.
 *          cancel     - drop a scheduled or queued run. Does not wait for a
 *                       handler that is already running.
 *          push_from_isr - push from an ISR (IRAM, safe with the flash cache
 *                       off). The item must be in internal RAM.
 *
 *          An item is queued or scheduled at most once. The queued flag is
 *          cleared before the handler runs: a push from inside the handler
 *          queues it again. Queued items run in push order.
 *
 *          Every item shares one low-priority task: handlers must be short,
 *          must not block (I2C transfers, sockets, delays) and must tolerate
 *          some latency. Long or time-critical work needs its own task.
 ******************************************************************************
 */

/* Define to prevent recursive inclusion ----------------------------------- */
#ifndef WORK_QUEUE_H_
#define WORK_QUEUE_H_

/* Includes ---------------------------------------------------------------- */
#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

/* Constants and macros ---------------------------------------------------- */
// clang-format off


// clang-format on

/* Types ------------------------------------------------------------------- */

/* Runs in the work_queue task. */
typedef void (*work_queue_handler_t)(void* context);

/* Fields are owned by work_queue. Initialise once with work_queue_item_init;
 * the item must outlive every push and schedule. */
typedef struct work_queue_item_s
{
    struct work_queue_item_s* next;
    int64_t deadline_us;
    work_queue_handler_t handler;
    void* context;
    bool queued;
    bool scheduled;
} work_queue_item_t;

/* Exposed API ------------------------------------------------------------- */

esp_err_t work_queue_init(void);

esp_err_t work_queue_item_init(
    work_queue_item_t* item,
    work_queue_handler_t handler,
    void* context);

esp_err_t work_queue_push(work_queue_item_t* item);
esp_err_t work_queue_push_from_isr(work_queue_item_t* item);
esp_err_t work_queue_schedule(work_queue_item_t* item, uint32_t delay_ms);
esp_err_t work_queue_reschedule(work_queue_item_t* item, uint32_t delay_ms);
esp_err_t work_queue_cancel(work_queue_item_t* item);

#endif /* WORK_QUEUE_H_ */

/**** END OF FILE ****/
