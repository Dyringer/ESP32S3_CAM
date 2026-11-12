/**
 ******************************************************************************
 * @file    test_work_queue.c
 * @author  Daniel Dyringer
 * @date    2026-10-24
 * @brief   work_queue behaviour tests: push, schedule, reschedule, cancel,
 *          ordering and the shared timer.
 *
 *          fake_idf_task_run() runs the real task loop until it would block,
 *          fake_idf_advance_ms() moves the clock and fires the timer.
 ******************************************************************************
 */


/* Includes ---------------------------------------------------------------- */
#include <stddef.h>

#include "fake_idf.h"
#include "unity.h"
#include "utils.h"
#include "work_queue.h"


/* Constants and macros ---------------------------------------------------- */
// clang-format off

#define WORK_QUEUE_TEST_ITEM_COUNT  3u
#define WORK_QUEUE_TEST_LOG_SIZE    32u
#define WORK_QUEUE_TEST_DELAY_MS    100u

// clang-format on

/* Private types ----------------------------------------------------------- */


/* Private variables ------------------------------------------------------- */

static work_queue_item_t work_queue_test_items[WORK_QUEUE_TEST_ITEM_COUNT] = {0};

static work_queue_item_t* const work_queue_test_a = &work_queue_test_items[0];
static work_queue_item_t* const work_queue_test_b = &work_queue_test_items[1];
static work_queue_item_t* const work_queue_test_c = &work_queue_test_items[2];

/* Items in the order their handlers ran. */
static const work_queue_item_t* work_queue_test_log[WORK_QUEUE_TEST_LOG_SIZE] = {0};
static uint32_t work_queue_test_run_count                                     = 0u;
static uint32_t work_queue_test_max_critical_depth                            = 0u;

/* Remaining self-pushes or self-schedules of the repeating handlers. */
static uint32_t work_queue_test_repeats = 0u;


/* Private functions ------------------------------------------------------- */

static void work_queue_test_record(void* context)
{
    if (work_queue_test_run_count < WORK_QUEUE_TEST_LOG_SIZE)
    {
        work_queue_test_log[work_queue_test_run_count] = context;
    }

    work_queue_test_run_count++;
    work_queue_test_max_critical_depth =
        MAX(work_queue_test_max_critical_depth, fake_idf_critical_depth());
}

static void work_queue_test_push_self(void* context)
{
    work_queue_test_record(context);

    if (work_queue_test_repeats > 0u)
    {
        work_queue_test_repeats--;
        work_queue_push(context);
    }
}

static void work_queue_test_schedule_self(void* context)
{
    work_queue_test_record(context);

    if (work_queue_test_repeats > 0u)
    {
        work_queue_test_repeats--;
        work_queue_schedule(context, WORK_QUEUE_TEST_DELAY_MS);
    }
}

static void work_queue_test_init_items(work_queue_handler_t handler)
{
    for (size_t i = 0u; i < WORK_QUEUE_TEST_ITEM_COUNT; i++)
    {
        TEST_ASSERT_EQUAL(
            ESP_OK,
            work_queue_item_init(
                &work_queue_test_items[i], handler, &work_queue_test_items[i]));
    }
}

static void work_queue_test_assert_log(const work_queue_item_t* const* expected, uint32_t count)
{
    TEST_ASSERT_EQUAL_UINT32(count, work_queue_test_run_count);
    TEST_ASSERT_EQUAL_PTR_ARRAY(expected, work_queue_test_log, count);
}

static void work_queue_test_assert_idle(const work_queue_item_t* item)
{
    TEST_ASSERT_FALSE(item->queued);
    TEST_ASSERT_FALSE(item->scheduled);
}


/* Exposed API ------------------------------------------------------------- */

void setUp(void)
{
    fake_idf_reset();
    TEST_ASSERT_EQUAL(ESP_OK, work_queue_init());

    work_queue_test_run_count          = 0u;
    work_queue_test_max_critical_depth = 0u;
    work_queue_test_repeats            = 0u;
    work_queue_test_init_items(work_queue_test_record);
}

/* Leaves both lists empty for the next test, whatever this one left. */
void tearDown(void)
{
    for (size_t i = 0u; i < WORK_QUEUE_TEST_ITEM_COUNT; i++)
    {
        work_queue_cancel(&work_queue_test_items[i]);
    }

    TEST_ASSERT_EQUAL_UINT32(0u, fake_idf_critical_depth());
}

/* ---- Push ---- */

void test_push_runs_handler_with_its_context(void)
{
    TEST_ASSERT_EQUAL(ESP_OK, work_queue_push(work_queue_test_a));
    TEST_ASSERT_TRUE(work_queue_test_a->queued);
    TEST_ASSERT_EQUAL_UINT32(0u, work_queue_test_run_count);

    fake_idf_task_run();

    const work_queue_item_t* const expected[] = {work_queue_test_a};
    work_queue_test_assert_log(expected, 1u);
    work_queue_test_assert_idle(work_queue_test_a);
}

void test_push_while_queued_runs_once_and_wakes_once(void)
{
    work_queue_push(work_queue_test_a);
    work_queue_push(work_queue_test_a);
    work_queue_push(work_queue_test_a);

    TEST_ASSERT_EQUAL_UINT32(1u, fake_idf_notify_count());

    fake_idf_task_run();

    TEST_ASSERT_EQUAL_UINT32(1u, work_queue_test_run_count);
}

void test_push_runs_items_in_push_order(void)
{
    work_queue_push(work_queue_test_c);
    work_queue_push(work_queue_test_a);
    work_queue_push(work_queue_test_b);

    fake_idf_task_run();

    const work_queue_item_t* const expected[] = {
        work_queue_test_c,
        work_queue_test_a,
        work_queue_test_b,
    };
    work_queue_test_assert_log(expected, 3u);
}

void test_push_from_own_handler_runs_again(void)
{
    work_queue_test_init_items(work_queue_test_push_self);
    work_queue_test_repeats = 2u;

    work_queue_push(work_queue_test_a);
    fake_idf_task_run();

    TEST_ASSERT_EQUAL_UINT32(3u, work_queue_test_run_count);
    work_queue_test_assert_idle(work_queue_test_a);
}

void test_handlers_run_outside_critical_sections(void)
{
    work_queue_push(work_queue_test_a);
    work_queue_schedule(work_queue_test_b, WORK_QUEUE_TEST_DELAY_MS);
    fake_idf_task_run();
    fake_idf_advance_ms(WORK_QUEUE_TEST_DELAY_MS);
    fake_idf_task_run();

    TEST_ASSERT_EQUAL_UINT32(2u, work_queue_test_run_count);
    TEST_ASSERT_EQUAL_UINT32(0u, work_queue_test_max_critical_depth);
}

void test_invalid_items_are_rejected(void)
{
    work_queue_item_t no_handler = {0};

    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, work_queue_push(NULL));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, work_queue_push(&no_handler));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, work_queue_schedule(NULL, 1u));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, work_queue_schedule(&no_handler, 1u));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, work_queue_reschedule(NULL, 1u));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, work_queue_reschedule(&no_handler, 1u));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, work_queue_cancel(NULL));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, work_queue_cancel(&no_handler));
    TEST_ASSERT_EQUAL_UINT32(0u, fake_idf_notify_count());
}

/* ---- Schedule ---- */

void test_schedule_runs_at_deadline_not_before(void)
{
    const int64_t start_us = fake_idf_now_us();

    TEST_ASSERT_EQUAL(
        ESP_OK, work_queue_schedule(work_queue_test_a, WORK_QUEUE_TEST_DELAY_MS));
    TEST_ASSERT_TRUE(work_queue_test_a->scheduled);

    fake_idf_task_run();

    TEST_ASSERT_TRUE(fake_idf_timer_armed());
    TEST_ASSERT_EQUAL_INT64(
        start_us + ((int64_t)WORK_QUEUE_TEST_DELAY_MS * 1000),
        fake_idf_timer_deadline_us());

    fake_idf_advance_ms(WORK_QUEUE_TEST_DELAY_MS - 1u);
    fake_idf_task_run();
    TEST_ASSERT_EQUAL_UINT32(0u, work_queue_test_run_count);

    fake_idf_advance_ms(1u);
    fake_idf_task_run();
    TEST_ASSERT_EQUAL_UINT32(1u, work_queue_test_run_count);
    work_queue_test_assert_idle(work_queue_test_a);
    TEST_ASSERT_FALSE(fake_idf_timer_armed());
}

void test_schedule_with_zero_delay_runs_now(void)
{
    work_queue_schedule(work_queue_test_a, 0u);

    TEST_ASSERT_TRUE(work_queue_test_a->queued);

    fake_idf_task_run();

    TEST_ASSERT_EQUAL_UINT32(1u, work_queue_test_run_count);
}

void test_schedule_while_scheduled_keeps_first_deadline(void)
{
    work_queue_schedule(work_queue_test_a, WORK_QUEUE_TEST_DELAY_MS);
    fake_idf_task_run();
    fake_idf_advance_ms(WORK_QUEUE_TEST_DELAY_MS / 2u);

    work_queue_schedule(work_queue_test_a, WORK_QUEUE_TEST_DELAY_MS);
    fake_idf_task_run();
    fake_idf_advance_ms(WORK_QUEUE_TEST_DELAY_MS / 2u);
    fake_idf_task_run();

    TEST_ASSERT_EQUAL_UINT32(1u, work_queue_test_run_count);

    fake_idf_advance_ms(WORK_QUEUE_TEST_DELAY_MS);
    fake_idf_task_run();

    TEST_ASSERT_EQUAL_UINT32(1u, work_queue_test_run_count);
}

void test_schedule_while_queued_is_ignored(void)
{
    work_queue_push(work_queue_test_a);
    work_queue_schedule(work_queue_test_a, WORK_QUEUE_TEST_DELAY_MS);

    TEST_ASSERT_FALSE(work_queue_test_a->scheduled);

    fake_idf_task_run();
    fake_idf_advance_ms(2u * WORK_QUEUE_TEST_DELAY_MS);
    fake_idf_task_run();

    TEST_ASSERT_EQUAL_UINT32(1u, work_queue_test_run_count);
}

/* The data_manager pattern: many changes inside one window, one run. */
void test_burst_of_schedules_runs_once_per_window(void)
{
    for (uint32_t i = 0u; i < (WORK_QUEUE_TEST_DELAY_MS - 1u); i++)
    {
        work_queue_schedule(work_queue_test_a, WORK_QUEUE_TEST_DELAY_MS);
        fake_idf_task_run();
        fake_idf_advance_ms(1u);
    }

    fake_idf_task_run();
    TEST_ASSERT_EQUAL_UINT32(0u, work_queue_test_run_count);

    fake_idf_advance_ms(1u);
    fake_idf_task_run();
    TEST_ASSERT_EQUAL_UINT32(1u, work_queue_test_run_count);

    work_queue_schedule(work_queue_test_a, WORK_QUEUE_TEST_DELAY_MS);
    fake_idf_task_run();
    fake_idf_advance_ms(WORK_QUEUE_TEST_DELAY_MS);
    fake_idf_task_run();
    TEST_ASSERT_EQUAL_UINT32(2u, work_queue_test_run_count);
}

void test_only_a_new_earliest_deadline_wakes_the_task(void)
{
    work_queue_schedule(work_queue_test_a, WORK_QUEUE_TEST_DELAY_MS);
    TEST_ASSERT_EQUAL_UINT32(1u, fake_idf_notify_count());

    work_queue_schedule(work_queue_test_b, 2u * WORK_QUEUE_TEST_DELAY_MS);
    TEST_ASSERT_EQUAL_UINT32(1u, fake_idf_notify_count());

    work_queue_schedule(work_queue_test_c, WORK_QUEUE_TEST_DELAY_MS / 2u);
    TEST_ASSERT_EQUAL_UINT32(2u, fake_idf_notify_count());
}

void test_handler_can_schedule_itself_periodically(void)
{
    work_queue_test_init_items(work_queue_test_schedule_self);
    work_queue_test_repeats = 2u;

    work_queue_push(work_queue_test_a);
    fake_idf_task_run();
    TEST_ASSERT_EQUAL_UINT32(1u, work_queue_test_run_count);

    fake_idf_advance_ms(WORK_QUEUE_TEST_DELAY_MS);
    fake_idf_task_run();
    TEST_ASSERT_EQUAL_UINT32(2u, work_queue_test_run_count);

    fake_idf_advance_ms(WORK_QUEUE_TEST_DELAY_MS);
    fake_idf_task_run();
    TEST_ASSERT_EQUAL_UINT32(3u, work_queue_test_run_count);

    fake_idf_advance_ms(WORK_QUEUE_TEST_DELAY_MS);
    fake_idf_task_run();
    TEST_ASSERT_EQUAL_UINT32(3u, work_queue_test_run_count);
    work_queue_test_assert_idle(work_queue_test_a);
}

/* ---- Push and schedule together ---- */

void test_push_moves_scheduled_item_to_run_now(void)
{
    work_queue_schedule(work_queue_test_a, WORK_QUEUE_TEST_DELAY_MS);
    fake_idf_task_run();

    TEST_ASSERT_EQUAL(ESP_OK, work_queue_push(work_queue_test_a));
    TEST_ASSERT_TRUE(work_queue_test_a->queued);
    TEST_ASSERT_FALSE(work_queue_test_a->scheduled);

    fake_idf_task_run();
    TEST_ASSERT_EQUAL_UINT32(1u, work_queue_test_run_count);

    fake_idf_advance_ms(2u * WORK_QUEUE_TEST_DELAY_MS);
    fake_idf_task_run();
    TEST_ASSERT_EQUAL_UINT32(1u, work_queue_test_run_count);
}

/* wifi_connection_forget() relies on this: a data_manager entry whose change
 * notification is scheduled is pushed for its NVS erase, and must still run
 * before the restart pushed after it. */
void test_pushed_scheduled_item_runs_before_later_push(void)
{
    work_queue_schedule(work_queue_test_a, WORK_QUEUE_TEST_DELAY_MS);
    work_queue_push(work_queue_test_a);
    work_queue_push(work_queue_test_b);

    fake_idf_task_run();

    const work_queue_item_t* const expected[] = {work_queue_test_a, work_queue_test_b};
    work_queue_test_assert_log(expected, 2u);
}

/* ---- Ordering of scheduled items ---- */

void test_due_items_run_in_deadline_order(void)
{
    work_queue_schedule(work_queue_test_a, 3u * WORK_QUEUE_TEST_DELAY_MS);
    work_queue_schedule(work_queue_test_b, WORK_QUEUE_TEST_DELAY_MS);
    work_queue_schedule(work_queue_test_c, 2u * WORK_QUEUE_TEST_DELAY_MS);
    fake_idf_task_run();

    fake_idf_advance_ms(3u * WORK_QUEUE_TEST_DELAY_MS);
    fake_idf_task_run();

    const work_queue_item_t* const expected[] = {
        work_queue_test_b,
        work_queue_test_c,
        work_queue_test_a,
    };
    work_queue_test_assert_log(expected, 3u);
}

void test_equal_deadlines_keep_schedule_order(void)
{
    work_queue_schedule(work_queue_test_b, WORK_QUEUE_TEST_DELAY_MS);
    work_queue_schedule(work_queue_test_c, WORK_QUEUE_TEST_DELAY_MS);
    work_queue_schedule(work_queue_test_a, WORK_QUEUE_TEST_DELAY_MS);
    fake_idf_task_run();

    fake_idf_advance_ms(WORK_QUEUE_TEST_DELAY_MS);
    fake_idf_task_run();

    const work_queue_item_t* const expected[] = {
        work_queue_test_b,
        work_queue_test_c,
        work_queue_test_a,
    };
    work_queue_test_assert_log(expected, 3u);
}

void test_timer_follows_the_earliest_deadline(void)
{
    const int64_t start_us = fake_idf_now_us();

    work_queue_schedule(work_queue_test_a, 3u * WORK_QUEUE_TEST_DELAY_MS);
    fake_idf_task_run();
    TEST_ASSERT_EQUAL_INT64(
        start_us + (3 * (int64_t)WORK_QUEUE_TEST_DELAY_MS * 1000),
        fake_idf_timer_deadline_us());

    work_queue_schedule(work_queue_test_b, WORK_QUEUE_TEST_DELAY_MS);
    fake_idf_task_run();
    TEST_ASSERT_EQUAL_INT64(
        start_us + ((int64_t)WORK_QUEUE_TEST_DELAY_MS * 1000),
        fake_idf_timer_deadline_us());

    fake_idf_advance_ms(WORK_QUEUE_TEST_DELAY_MS);
    fake_idf_task_run();

    const work_queue_item_t* const expected[] = {work_queue_test_b};
    work_queue_test_assert_log(expected, 1u);
    TEST_ASSERT_TRUE(fake_idf_timer_armed());
    TEST_ASSERT_EQUAL_INT64(
        start_us + (3 * (int64_t)WORK_QUEUE_TEST_DELAY_MS * 1000),
        fake_idf_timer_deadline_us());
}

void test_push_alone_leaves_timer_disarmed(void)
{
    work_queue_push(work_queue_test_a);
    fake_idf_task_run();

    TEST_ASSERT_FALSE(fake_idf_timer_armed());
}

/* ---- Reschedule ---- */

void test_reschedule_replaces_a_pending_deadline(void)
{
    work_queue_schedule(work_queue_test_a, WORK_QUEUE_TEST_DELAY_MS);
    fake_idf_task_run();
    fake_idf_advance_ms(WORK_QUEUE_TEST_DELAY_MS / 2u);

    TEST_ASSERT_EQUAL(
        ESP_OK, work_queue_reschedule(work_queue_test_a, WORK_QUEUE_TEST_DELAY_MS));
    fake_idf_task_run();

    fake_idf_advance_ms(WORK_QUEUE_TEST_DELAY_MS / 2u);
    fake_idf_task_run();
    TEST_ASSERT_EQUAL_UINT32(0u, work_queue_test_run_count);

    fake_idf_advance_ms(WORK_QUEUE_TEST_DELAY_MS / 2u);
    fake_idf_task_run();
    TEST_ASSERT_EQUAL_UINT32(1u, work_queue_test_run_count);
}

void test_reschedule_can_move_a_deadline_earlier(void)
{
    work_queue_schedule(work_queue_test_a, 5u * WORK_QUEUE_TEST_DELAY_MS);
    fake_idf_task_run();

    work_queue_reschedule(work_queue_test_a, WORK_QUEUE_TEST_DELAY_MS);
    fake_idf_task_run();

    fake_idf_advance_ms(WORK_QUEUE_TEST_DELAY_MS);
    fake_idf_task_run();
    TEST_ASSERT_EQUAL_UINT32(1u, work_queue_test_run_count);
}

void test_reschedule_delays_a_queued_item(void)
{
    work_queue_push(work_queue_test_a);
    work_queue_reschedule(work_queue_test_a, WORK_QUEUE_TEST_DELAY_MS);

    TEST_ASSERT_FALSE(work_queue_test_a->queued);
    TEST_ASSERT_TRUE(work_queue_test_a->scheduled);

    fake_idf_task_run();
    TEST_ASSERT_EQUAL_UINT32(0u, work_queue_test_run_count);

    fake_idf_advance_ms(WORK_QUEUE_TEST_DELAY_MS);
    fake_idf_task_run();
    TEST_ASSERT_EQUAL_UINT32(1u, work_queue_test_run_count);
}

void test_reschedule_keeps_other_queued_items(void)
{
    work_queue_push(work_queue_test_a);
    work_queue_push(work_queue_test_b);
    work_queue_push(work_queue_test_c);
    work_queue_reschedule(work_queue_test_b, WORK_QUEUE_TEST_DELAY_MS);

    fake_idf_task_run();

    const work_queue_item_t* const expected[] = {work_queue_test_a, work_queue_test_c};
    work_queue_test_assert_log(expected, 2u);
}

void test_reschedule_with_zero_delay_runs_now(void)
{
    work_queue_schedule(work_queue_test_a, WORK_QUEUE_TEST_DELAY_MS);
    work_queue_reschedule(work_queue_test_a, 0u);

    fake_idf_task_run();

    TEST_ASSERT_EQUAL_UINT32(1u, work_queue_test_run_count);
}

/* ---- Cancel ---- */

void test_cancel_drops_a_queued_item(void)
{
    work_queue_push(work_queue_test_a);

    TEST_ASSERT_EQUAL(ESP_OK, work_queue_cancel(work_queue_test_a));
    work_queue_test_assert_idle(work_queue_test_a);

    fake_idf_task_run();
    TEST_ASSERT_EQUAL_UINT32(0u, work_queue_test_run_count);
}

void test_cancel_drops_the_last_queued_item(void)
{
    work_queue_push(work_queue_test_a);
    work_queue_push(work_queue_test_b);
    work_queue_cancel(work_queue_test_b);
    work_queue_push(work_queue_test_c);

    fake_idf_task_run();

    const work_queue_item_t* const expected[] = {work_queue_test_a, work_queue_test_c};
    work_queue_test_assert_log(expected, 2u);
}

void test_cancel_drops_a_scheduled_item(void)
{
    work_queue_schedule(work_queue_test_a, WORK_QUEUE_TEST_DELAY_MS);
    work_queue_schedule(work_queue_test_b, 2u * WORK_QUEUE_TEST_DELAY_MS);
    fake_idf_task_run();

    TEST_ASSERT_EQUAL(ESP_OK, work_queue_cancel(work_queue_test_a));
    work_queue_test_assert_idle(work_queue_test_a);

    fake_idf_advance_ms(2u * WORK_QUEUE_TEST_DELAY_MS);
    fake_idf_task_run();

    const work_queue_item_t* const expected[] = {work_queue_test_b};
    work_queue_test_assert_log(expected, 1u);
}

/* The timer stays armed for a cancelled earliest deadline; that fire must
 * run nothing and leave the timer idle. */
void test_stale_timer_fire_runs_nothing(void)
{
    work_queue_schedule(work_queue_test_a, WORK_QUEUE_TEST_DELAY_MS);
    fake_idf_task_run();
    work_queue_cancel(work_queue_test_a);

    fake_idf_advance_ms(WORK_QUEUE_TEST_DELAY_MS);
    fake_idf_task_run();

    TEST_ASSERT_EQUAL_UINT32(0u, work_queue_test_run_count);
    TEST_ASSERT_FALSE(fake_idf_timer_armed());
}

void test_cancel_of_an_idle_item_is_ok(void)
{
    TEST_ASSERT_EQUAL(ESP_OK, work_queue_cancel(work_queue_test_a));
    work_queue_test_assert_idle(work_queue_test_a);
}

void test_cancelled_item_can_be_pushed_again(void)
{
    work_queue_schedule(work_queue_test_a, WORK_QUEUE_TEST_DELAY_MS);
    work_queue_cancel(work_queue_test_a);
    work_queue_push(work_queue_test_a);

    fake_idf_task_run();

    TEST_ASSERT_EQUAL_UINT32(1u, work_queue_test_run_count);
}

/**** END OF FILE ****/
