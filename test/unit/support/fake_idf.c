/**
 ******************************************************************************
 * @file    fake_idf.c
 * @author  Daniel Dyringer
 * @date    2026-10-24
 * @brief   Host fakes of the ESP-IDF and FreeRTOS calls used by unit tests.
 ******************************************************************************
 */


/* Includes ---------------------------------------------------------------- */
#include "fake_idf.h"

#include <setjmp.h>
#include <stddef.h>

#include "esp_err.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "unity.h"


/* Constants and macros ---------------------------------------------------- */
// clang-format off


// clang-format on

/* Private types ----------------------------------------------------------- */

struct esp_timer
{
    int64_t deadline_us;
    esp_timer_cb_t callback;
    void* arg;
    bool armed;
};

struct tskTaskControlBlock
{
    uint32_t unused;
};


/* Private variables ------------------------------------------------------- */

/* Monotonic for the whole executable, like esp_timer_get_time(). */
static int64_t fake_idf_clock_us = 0;

static struct esp_timer fake_idf_timer  = {0};
static bool fake_idf_timer_exists       = false;
static bool fake_idf_timer_create_fails = false;
static uint32_t fake_idf_timer_creates  = 0u;
static uint32_t fake_idf_timer_deletes  = 0u;

static struct tskTaskControlBlock fake_idf_task = {0};
static TaskFunction_t fake_idf_task_function    = NULL;
static void* fake_idf_task_parameters           = NULL;
static bool fake_idf_task_create_fails          = false;
static uint32_t fake_idf_task_creates           = 0u;

static uint32_t fake_idf_notifications_pending = 0u;
static uint32_t fake_idf_notifications_total   = 0u;
static bool fake_idf_task_running              = false;
static uint32_t fake_idf_task_wait_ticks       = 0u;
static jmp_buf fake_idf_task_block;

static uint32_t fake_idf_critical_nesting = 0u;
static uint32_t fake_idf_errors           = 0u;
static fake_idf_log_hook_t fake_idf_hook  = NULL;


/* Private functions ------------------------------------------------------- */


/* Exposed API ------------------------------------------------------------- */

void fake_idf_reset(void)
{
    fake_idf_timer.armed           = false;
    fake_idf_timer_create_fails    = false;
    fake_idf_timer_creates         = 0u;
    fake_idf_timer_deletes         = 0u;
    fake_idf_task_create_fails     = false;
    fake_idf_task_creates          = 0u;
    fake_idf_notifications_pending = 0u;
    fake_idf_notifications_total   = 0u;
    fake_idf_critical_nesting      = 0u;
    fake_idf_errors                = 0u;
    fake_idf_hook                  = NULL;

    /* A failed assertion inside the task leaves through Unity's own jump. */
    fake_idf_task_running = false;
}

/* ---- Failure injection ---- */

void fake_idf_fail_timer_create(bool fail)
{
    fake_idf_timer_create_fails = fail;
}

void fake_idf_fail_task_create(bool fail)
{
    fake_idf_task_create_fails = fail;
}

/* ---- Clock and timer ---- */

int64_t fake_idf_now_us(void)
{
    return fake_idf_clock_us;
}

void fake_idf_advance_ms(uint32_t delay_ms)
{
    const int64_t target_us = fake_idf_clock_us + ((int64_t)delay_ms * 1000);

    while (fake_idf_timer.armed && (fake_idf_timer.deadline_us <= target_us))
    {
        if (fake_idf_timer.deadline_us > fake_idf_clock_us)
        {
            fake_idf_clock_us = fake_idf_timer.deadline_us;
        }

        fake_idf_timer.armed = false;
        fake_idf_timer.callback(fake_idf_timer.arg);
    }

    fake_idf_clock_us = target_us;
}

bool fake_idf_timer_armed(void)
{
    return fake_idf_timer.armed;
}

int64_t fake_idf_timer_deadline_us(void)
{
    return fake_idf_timer.deadline_us;
}

uint32_t fake_idf_timer_create_count(void)
{
    return fake_idf_timer_creates;
}

uint32_t fake_idf_timer_delete_count(void)
{
    return fake_idf_timer_deletes;
}

esp_err_t esp_timer_create(
    const esp_timer_create_args_t* create_args,
    esp_timer_handle_t* out_handle)
{
    esp_err_t error_code = ESP_OK;

    fake_idf_timer_creates++;

    if (fake_idf_timer_create_fails)
    {
        error_code = ESP_ERR_NO_MEM;
    }
    else
    {
        TEST_ASSERT_FALSE_MESSAGE(fake_idf_timer_exists, "Fake supports one timer");
        TEST_ASSERT_NOT_NULL(create_args->callback);

        fake_idf_timer.callback = create_args->callback;
        fake_idf_timer.arg      = create_args->arg;
        fake_idf_timer.armed    = false;
        fake_idf_timer_exists   = true;
        *out_handle             = &fake_idf_timer;
    }

    return error_code;
}

/* Same errors as ESP-IDF: starting an armed timer is an invalid state. */
esp_err_t esp_timer_start_once(esp_timer_handle_t timer, uint64_t timeout_us)
{
    esp_err_t error_code = ESP_OK;

    TEST_ASSERT_EQUAL_PTR(&fake_idf_timer, timer);

    if (fake_idf_timer.armed)
    {
        error_code = ESP_ERR_INVALID_STATE;
    }
    else
    {
        fake_idf_timer.deadline_us = fake_idf_clock_us + (int64_t)timeout_us;
        fake_idf_timer.armed       = true;
    }

    return error_code;
}

esp_err_t esp_timer_stop(esp_timer_handle_t timer)
{
    esp_err_t error_code = ESP_OK;

    TEST_ASSERT_EQUAL_PTR(&fake_idf_timer, timer);

    if (!fake_idf_timer.armed)
    {
        error_code = ESP_ERR_INVALID_STATE;
    }

    fake_idf_timer.armed = false;

    return error_code;
}

esp_err_t esp_timer_delete(esp_timer_handle_t timer)
{
    TEST_ASSERT_EQUAL_PTR(&fake_idf_timer, timer);

    fake_idf_timer_deletes++;
    fake_idf_timer.armed  = false;
    fake_idf_timer_exists = false;

    return ESP_OK;
}

int64_t esp_timer_get_time(void)
{
    return fake_idf_clock_us;
}

/* ---- Task ---- */

bool fake_idf_task_created(void)
{
    return (fake_idf_task_function != NULL);
}

uint32_t fake_idf_task_create_count(void)
{
    return fake_idf_task_creates;
}

void fake_idf_task_run(void)
{
    TEST_ASSERT_NOT_NULL_MESSAGE(fake_idf_task_function, "No task created");
    TEST_ASSERT_FALSE_MESSAGE(fake_idf_task_running, "Task run is not reentrant");

    if (setjmp(fake_idf_task_block) == 0)
    {
        fake_idf_task_running = true;
        fake_idf_task_function(fake_idf_task_parameters);
        TEST_FAIL_MESSAGE("Task function returned");
    }

    fake_idf_task_running = false;
}

uint32_t fake_idf_notify_count(void)
{
    return fake_idf_notifications_total;
}

uint32_t fake_idf_task_last_wait_ticks(void)
{
    return fake_idf_task_wait_ticks;
}

BaseType_t xTaskCreatePinnedToCore(
    TaskFunction_t task_function,
    const char* name,
    uint32_t stack_depth,
    void* parameters,
    UBaseType_t priority,
    TaskHandle_t* created_task,
    BaseType_t core_id)
{
    BaseType_t result = pdPASS;

    TEST_ASSERT_NOT_NULL(name);
    TEST_ASSERT_TRUE(stack_depth > 0u);
    TEST_ASSERT_TRUE(priority > 0u);
    TEST_ASSERT_TRUE((core_id == 0) || (core_id == 1));

    fake_idf_task_creates++;

    if (fake_idf_task_create_fails)
    {
        result = pdFAIL;
    }
    else
    {
        fake_idf_task_function   = task_function;
        fake_idf_task_parameters = parameters;

        if (created_task != NULL)
        {
            *created_task = &fake_idf_task;
        }
    }

    return result;
}

/* Same bookkeeping as xTaskNotifyGive; the ISR variant may run inside the
 * work_queue critical section. */
void vTaskNotifyGiveFromISR(TaskHandle_t task_to_notify, BaseType_t* higher_priority_task_woken)
{
    TEST_ASSERT_EQUAL_PTR(&fake_idf_task, task_to_notify);

    fake_idf_notifications_pending++;
    fake_idf_notifications_total++;

    if (higher_priority_task_woken != NULL)
    {
        *higher_priority_task_woken = pdFALSE;
    }
}

BaseType_t xTaskNotifyGive(TaskHandle_t task_to_notify)
{
    TEST_ASSERT_EQUAL_PTR(&fake_idf_task, task_to_notify);
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(
        0u, fake_idf_critical_nesting, "Notify inside a critical section");

    fake_idf_notifications_pending++;
    fake_idf_notifications_total++;

    return pdPASS;
}

/* Where the real task would block, control returns to fake_idf_task_run(),
 * with or without a timeout: the next run stands for the wake-up. */
uint32_t ulTaskNotifyTake(BaseType_t clear_count_on_exit, TickType_t ticks_to_wait)
{
    TEST_ASSERT_TRUE_MESSAGE(fake_idf_task_running, "Called outside the task");

    fake_idf_task_wait_ticks = ticks_to_wait;

    if (fake_idf_notifications_pending == 0u)
    {
        longjmp(fake_idf_task_block, 1);
    }

    const uint32_t value = fake_idf_notifications_pending;

    fake_idf_notifications_pending =
        (clear_count_on_exit != pdFALSE) ? 0u : (fake_idf_notifications_pending - 1u);

    return value;
}

/* ---- Critical sections and logs ---- */

void fake_idf_critical_enter(void* mux)
{
    TEST_ASSERT_NOT_NULL(mux);

    fake_idf_critical_nesting++;
}

void fake_idf_critical_exit(void* mux)
{
    TEST_ASSERT_NOT_NULL(mux);
    TEST_ASSERT_TRUE_MESSAGE(fake_idf_critical_nesting > 0u, "Unbalanced exit");

    fake_idf_critical_nesting--;
}

uint32_t fake_idf_critical_depth(void)
{
    return fake_idf_critical_nesting;
}

void fake_idf_log(const char* tag, fake_idf_log_level_t level)
{
    TEST_ASSERT_NOT_NULL(tag);

    if (level == FAKE_IDF_LOG_ERROR)
    {
        fake_idf_errors++;
    }

    if (fake_idf_hook != NULL)
    {
        fake_idf_hook(tag, level);
    }
}

void fake_idf_set_log_hook(fake_idf_log_hook_t hook)
{
    fake_idf_hook = hook;
}

uint32_t fake_idf_error_count(void)
{
    return fake_idf_errors;
}

const char* esp_err_to_name(esp_err_t code)
{
    const char* name = "ESP_ERR_UNKNOWN";

    switch (code)
    {
        case ESP_OK:
        {
            name = "ESP_OK";
        }
        break;
        case ESP_ERR_NO_MEM:
        {
            name = "ESP_ERR_NO_MEM";
        }
        break;
        case ESP_ERR_INVALID_ARG:
        {
            name = "ESP_ERR_INVALID_ARG";
        }
        break;
        case ESP_ERR_INVALID_STATE:
        {
            name = "ESP_ERR_INVALID_STATE";
        }
        break;
        default:
        {
            name = "ESP_ERR_UNKNOWN";
        }
        break;
    }

    return name;
}

/**** END OF FILE ****/
