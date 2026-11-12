/**
 ******************************************************************************
 * @file    work_queue.c
 * @author  Daniel Dyringer
 * @date    2026-10-11
 * @brief   System work queue: deferred work run by a single task.
 ******************************************************************************
 */


/* Includes ---------------------------------------------------------------- */
#include "work_queue.h"

#include <stddef.h>

#include "esp_attr.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "utils.h"


/* Constants and macros ---------------------------------------------------- */
// clang-format off

#define WORK_QUEUE_TASK_STACK   4096u
#define WORK_QUEUE_TASK_PRIO    2u
#define WORK_QUEUE_TASK_CORE    1

// clang-format on

/* Private types ----------------------------------------------------------- */


/* Private variables ------------------------------------------------------- */

static const char* work_queue_tag = "work_queue";

static TaskHandle_t work_queue_task_handle = NULL;

/* One timer for every scheduled item, armed for the earliest deadline. Only
 * the work_queue task starts and stops it, so callers never race on it. */
static esp_timer_handle_t work_queue_timer = NULL;

/* Guards both lists and the next, deadline_us, queued and scheduled fields of
 * every item. Touched by callers and the work_queue task. An item is on at
 * most one list: queued on the run list or scheduled on the delayed list. */
static portMUX_TYPE work_queue_lock = portMUX_INITIALIZER_UNLOCKED;

static work_queue_item_t* work_queue_run_head = NULL;
static work_queue_item_t* work_queue_run_tail = NULL;

/* Sorted by deadline_us; equal deadlines keep their schedule order. */
static work_queue_item_t* work_queue_delayed_head = NULL;


/* Private functions ------------------------------------------------------- */

/* ---- Lists (caller holds work_queue_lock) ---- */

/* IRAM_ATTR on everything reachable from work_queue_push_from_isr(): its
 * callers' ISRs keep running while the flash cache is off. */
static void IRAM_ATTR work_queue_run_append(work_queue_item_t* item)
{
    item->next   = NULL;
    item->queued = true;

    if (work_queue_run_tail == NULL)
    {
        work_queue_run_head = item;
    }
    else
    {
        work_queue_run_tail->next = item;
    }

    work_queue_run_tail = item;
}

static work_queue_item_t* work_queue_run_pop(void)
{
    work_queue_item_t* const item = work_queue_run_head;

    if (item != NULL)
    {
        work_queue_run_head = item->next;

        if (work_queue_run_head == NULL)
        {
            work_queue_run_tail = NULL;
        }

        item->next   = NULL;
        item->queued = false;
    }

    return item;
}

static void work_queue_run_remove(work_queue_item_t* item)
{
    work_queue_item_t* previous = NULL;
    work_queue_item_t* current  = work_queue_run_head;

    while ((current != NULL) && (current != item))
    {
        previous = current;
        current  = current->next;
    }

    if (current != NULL)
    {
        if (previous == NULL)
        {
            work_queue_run_head = item->next;
        }
        else
        {
            previous->next = item->next;
        }

        if (work_queue_run_tail == item)
        {
            work_queue_run_tail = previous;
        }
    }

    item->next   = NULL;
    item->queued = false;
}

/* Returns whether the item became the earliest deadline. */
static bool work_queue_delayed_insert(work_queue_item_t* item, int64_t deadline_us)
{
    work_queue_item_t* previous = NULL;
    work_queue_item_t* current  = work_queue_delayed_head;

    while ((current != NULL) && (current->deadline_us <= deadline_us))
    {
        previous = current;
        current  = current->next;
    }

    item->deadline_us = deadline_us;
    item->next        = current;
    item->scheduled   = true;

    if (previous == NULL)
    {
        work_queue_delayed_head = item;
    }
    else
    {
        previous->next = item;
    }

    return (previous == NULL);
}

static void IRAM_ATTR work_queue_delayed_remove(work_queue_item_t* item)
{
    work_queue_item_t* previous = NULL;
    work_queue_item_t* current  = work_queue_delayed_head;

    while ((current != NULL) && (current != item))
    {
        previous = current;
        current  = current->next;
    }

    if (current != NULL)
    {
        if (previous == NULL)
        {
            work_queue_delayed_head = item->next;
        }
        else
        {
            previous->next = item->next;
        }
    }

    item->next      = NULL;
    item->scheduled = false;
}

/* Returns whether the item was added; false if it was already queued. */
static bool IRAM_ATTR work_queue_run_enqueue(work_queue_item_t* item)
{
    const bool added = !item->queued;

    if (added)
    {
        if (item->scheduled)
        {
            work_queue_delayed_remove(item);
        }

        work_queue_run_append(item);
    }

    return added;
}

/* Moves due items to the run list. Returns whether an item is still waiting,
 * with its delay in wait_us. */
static bool work_queue_promote_due(int64_t* wait_us)
{
    bool waiting = false;

    taskENTER_CRITICAL(&work_queue_lock);

    const int64_t now_us = esp_timer_get_time();

    while ((work_queue_delayed_head != NULL)
           && (work_queue_delayed_head->deadline_us <= now_us))
    {
        work_queue_item_t* const item = work_queue_delayed_head;

        work_queue_delayed_head = item->next;
        item->scheduled         = false;
        work_queue_run_append(item);
    }

    if (work_queue_delayed_head != NULL)
    {
        waiting  = true;
        *wait_us = work_queue_delayed_head->deadline_us - now_us;
    }

    taskEXIT_CRITICAL(&work_queue_lock);

    return waiting;
}

/* ---- Task and timer ---- */

static void work_queue_wake(void)
{
    xTaskNotifyGive(work_queue_task_handle);
}

/* esp_timer task: must not block. A fire left over from an earlier arming
 * only causes an extra pass that finds nothing due. */
static void work_queue_timer_callback(void* arg)
{
    ARG_UNUSED(arg);

    work_queue_wake();
}

static void work_queue_arm_timer(void)
{
    int64_t wait_us = 0;

    esp_timer_stop(work_queue_timer);

    if (work_queue_promote_due(&wait_us))
    {
        const esp_err_t error_code =
            esp_timer_start_once(work_queue_timer, (uint64_t)wait_us);

        if (error_code != ESP_OK)
        {
            ESP_LOGE(work_queue_tag, "Timer start failed: %s", esp_err_to_name(error_code));
        }
    }
}

static void work_queue_task(void* arg)
{
    ARG_UNUSED(arg);

    while (true)
    {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

        work_queue_arm_timer();

        work_queue_item_t* item = NULL;

        do
        {
            taskENTER_CRITICAL(&work_queue_lock);
            item = work_queue_run_pop();
            taskEXIT_CRITICAL(&work_queue_lock);

            if (item != NULL)
            {
                item->handler(item->context);
            }
        } while (item != NULL);
    }
}

static bool IRAM_ATTR work_queue_item_valid(const work_queue_item_t* item)
{
    return (item != NULL) && (item->handler != NULL);
}


/* Exposed API ------------------------------------------------------------- */

esp_err_t work_queue_init(void)
{
    esp_err_t error_code = ESP_OK;

    do
    {
        if (work_queue_task_handle != NULL)
        {
            break;
        }

        const esp_timer_create_args_t timer_arguments = {
            .callback        = work_queue_timer_callback,
            .arg             = NULL,
            .dispatch_method = ESP_TIMER_TASK,
            .name            = "work_queue",
        };

        error_code = esp_timer_create(&timer_arguments, &work_queue_timer);

        if (error_code != ESP_OK)
        {
            break;
        }

        const BaseType_t created = xTaskCreatePinnedToCore(
            work_queue_task,
            "work_queue",
            WORK_QUEUE_TASK_STACK,
            NULL,
            WORK_QUEUE_TASK_PRIO,
            &work_queue_task_handle,
            WORK_QUEUE_TASK_CORE);

        if (created != pdPASS)
        {
            work_queue_task_handle = NULL;
            error_code             = ESP_ERR_NO_MEM;
            break;
        }
    } while (0);

    if (error_code != ESP_OK)
    {
        if (work_queue_timer != NULL)
        {
            esp_timer_delete(work_queue_timer);
            work_queue_timer = NULL;
        }

        ESP_LOGE(work_queue_tag, "Init failed: %s", esp_err_to_name(error_code));
    }

    return error_code;
}

esp_err_t work_queue_item_init(work_queue_item_t* item, work_queue_handler_t handler, void* context)
{
    esp_err_t error_code = ESP_OK;

    if ((item == NULL) || (handler == NULL))
    {
        error_code = ESP_ERR_INVALID_ARG;
        ESP_LOGE(work_queue_tag, "Item init failed: %s", esp_err_to_name(error_code));
    }
    else
    {
        item->next        = NULL;
        item->deadline_us = 0;
        item->handler     = handler;
        item->context     = context;
        item->queued      = false;
        item->scheduled   = false;
    }

    return error_code;
}

esp_err_t work_queue_push(work_queue_item_t* item)
{
    esp_err_t error_code = ESP_OK;

    do
    {
        if (work_queue_task_handle == NULL)
        {
            error_code = ESP_ERR_INVALID_STATE;
            break;
        }

        if (!work_queue_item_valid(item))
        {
            error_code = ESP_ERR_INVALID_ARG;
            break;
        }

        taskENTER_CRITICAL(&work_queue_lock);
        const bool added = work_queue_run_enqueue(item);
        taskEXIT_CRITICAL(&work_queue_lock);

        if (added)
        {
            work_queue_wake();
        }
    } while (0);

    if (error_code != ESP_OK)
    {
        ESP_LOGE(work_queue_tag, "Push failed: %s", esp_err_to_name(error_code));
    }

    return error_code;
}

/* No logging: ISR context. */
esp_err_t IRAM_ATTR work_queue_push_from_isr(work_queue_item_t* item)
{
    esp_err_t error_code = ESP_OK;

    do
    {
        if (work_queue_task_handle == NULL)
        {
            error_code = ESP_ERR_INVALID_STATE;
            break;
        }

        if (!work_queue_item_valid(item))
        {
            error_code = ESP_ERR_INVALID_ARG;
            break;
        }

        taskENTER_CRITICAL_ISR(&work_queue_lock);
        const bool added = work_queue_run_enqueue(item);
        taskEXIT_CRITICAL_ISR(&work_queue_lock);

        if (added)
        {
            BaseType_t higher_priority_task_woken = pdFALSE;

            vTaskNotifyGiveFromISR(work_queue_task_handle, &higher_priority_task_woken);

            if (higher_priority_task_woken == pdTRUE)
            {
                portYIELD_FROM_ISR();
            }
        }
    } while (0);

    return error_code;
}

esp_err_t work_queue_schedule(work_queue_item_t* item, uint32_t delay_ms)
{
    esp_err_t error_code = ESP_OK;

    do
    {
        if (work_queue_task_handle == NULL)
        {
            error_code = ESP_ERR_INVALID_STATE;
            break;
        }

        if (!work_queue_item_valid(item))
        {
            error_code = ESP_ERR_INVALID_ARG;
            break;
        }

        if (delay_ms == 0u)
        {
            error_code = work_queue_push(item);
            break;
        }

        const int64_t deadline_us = esp_timer_get_time() + ((int64_t)delay_ms * 1000);
        bool earliest             = false;

        taskENTER_CRITICAL(&work_queue_lock);

        if ((!item->queued) && (!item->scheduled))
        {
            earliest = work_queue_delayed_insert(item, deadline_us);
        }

        taskEXIT_CRITICAL(&work_queue_lock);

        if (earliest)
        {
            work_queue_wake();
        }
    } while (0);

    if (error_code != ESP_OK)
    {
        ESP_LOGE(work_queue_tag, "Schedule failed: %s", esp_err_to_name(error_code));
    }

    return error_code;
}

esp_err_t work_queue_reschedule(work_queue_item_t* item, uint32_t delay_ms)
{
    esp_err_t error_code = ESP_OK;

    do
    {
        if (work_queue_task_handle == NULL)
        {
            error_code = ESP_ERR_INVALID_STATE;
            break;
        }

        if (!work_queue_item_valid(item))
        {
            error_code = ESP_ERR_INVALID_ARG;
            break;
        }

        if (delay_ms == 0u)
        {
            error_code = work_queue_push(item);
            break;
        }

        const int64_t deadline_us = esp_timer_get_time() + ((int64_t)delay_ms * 1000);

        taskENTER_CRITICAL(&work_queue_lock);

        if (item->queued)
        {
            work_queue_run_remove(item);
        }
        else if (item->scheduled)
        {
            work_queue_delayed_remove(item);
        }
        else
        {
            /* Idle item. */
        }

        const bool earliest = work_queue_delayed_insert(item, deadline_us);

        taskEXIT_CRITICAL(&work_queue_lock);

        if (earliest)
        {
            work_queue_wake();
        }
    } while (0);

    if (error_code != ESP_OK)
    {
        ESP_LOGE(work_queue_tag, "Reschedule failed: %s", esp_err_to_name(error_code));
    }

    return error_code;
}

/* A cancelled earliest deadline leaves the timer armed; its fire finds
 * nothing due and re-arms for the next one. */
esp_err_t work_queue_cancel(work_queue_item_t* item)
{
    esp_err_t error_code = ESP_OK;

    if (!work_queue_item_valid(item))
    {
        error_code = ESP_ERR_INVALID_ARG;
    }
    else
    {
        taskENTER_CRITICAL(&work_queue_lock);

        if (item->queued)
        {
            work_queue_run_remove(item);
        }
        else if (item->scheduled)
        {
            work_queue_delayed_remove(item);
        }
        else
        {
            /* Neither queued nor scheduled. */
        }

        taskEXIT_CRITICAL(&work_queue_lock);
    }

    return error_code;
}

/**** END OF FILE ****/
