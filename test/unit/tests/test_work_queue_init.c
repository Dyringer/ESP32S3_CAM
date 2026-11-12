/**
 ******************************************************************************
 * @file    test_work_queue_init.c
 * @author  Daniel Dyringer
 * @date    2026-10-24
 * @brief   work_queue initialisation tests.
 *
 *          work_queue keeps its task and timer for the life of the program,
 *          so these tests share one executable state and run in file order:
 *          calls before init, failed inits, then the successful init.
 ******************************************************************************
 */


/* Includes ---------------------------------------------------------------- */
#include "fake_idf.h"
#include "unity.h"
#include "utils.h"
#include "work_queue.h"


/* Constants and macros ---------------------------------------------------- */
// clang-format off


// clang-format on

/* Private types ----------------------------------------------------------- */


/* Private variables ------------------------------------------------------- */

static work_queue_item_t work_queue_init_test_item = {0};


/* Private functions ------------------------------------------------------- */

static void work_queue_init_test_handler(void* context)
{
    ARG_UNUSED(context);
}


/* Exposed API ------------------------------------------------------------- */

void setUp(void)
{
    fake_idf_reset();
}

void tearDown(void) {}

void test_item_init_rejects_missing_item_or_handler(void)
{
    TEST_ASSERT_EQUAL(
        ESP_ERR_INVALID_ARG,
        work_queue_item_init(NULL, work_queue_init_test_handler, NULL));
    TEST_ASSERT_EQUAL(
        ESP_ERR_INVALID_ARG, work_queue_item_init(&work_queue_init_test_item, NULL, NULL));
}

void test_item_init_works_before_queue_init(void)
{
    TEST_ASSERT_EQUAL(
        ESP_OK,
        work_queue_item_init(
            &work_queue_init_test_item, work_queue_init_test_handler, NULL));
    TEST_ASSERT_FALSE(work_queue_init_test_item.queued);
    TEST_ASSERT_FALSE(work_queue_init_test_item.scheduled);
}

void test_calls_before_init_are_rejected(void)
{
    work_queue_item_t* const item = &work_queue_init_test_item;

    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_STATE, work_queue_push(item));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_STATE, work_queue_schedule(item, 100u));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_STATE, work_queue_reschedule(item, 100u));
    TEST_ASSERT_FALSE(item->queued);
    TEST_ASSERT_FALSE(item->scheduled);
    TEST_ASSERT_EQUAL_UINT32(0u, fake_idf_notify_count());
}

void test_init_fails_when_timer_cannot_be_created(void)
{
    fake_idf_fail_timer_create(true);

    TEST_ASSERT_EQUAL(ESP_ERR_NO_MEM, work_queue_init());
    TEST_ASSERT_EQUAL_UINT32(0u, fake_idf_task_create_count());
    TEST_ASSERT_EQUAL_UINT32(1u, fake_idf_error_count());
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_STATE, work_queue_push(&work_queue_init_test_item));
}

void test_init_fails_and_frees_timer_when_task_cannot_be_created(void)
{
    fake_idf_fail_task_create(true);

    TEST_ASSERT_EQUAL(ESP_ERR_NO_MEM, work_queue_init());
    TEST_ASSERT_EQUAL_UINT32(1u, fake_idf_timer_create_count());
    TEST_ASSERT_EQUAL_UINT32(1u, fake_idf_timer_delete_count());
    TEST_ASSERT_FALSE(fake_idf_task_created());
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_STATE, work_queue_push(&work_queue_init_test_item));
}

void test_init_succeeds_after_failures(void)
{
    TEST_ASSERT_EQUAL(ESP_OK, work_queue_init());
    TEST_ASSERT_TRUE(fake_idf_task_created());
    TEST_ASSERT_EQUAL_UINT32(1u, fake_idf_timer_create_count());
    TEST_ASSERT_EQUAL_UINT32(1u, fake_idf_task_create_count());
    TEST_ASSERT_EQUAL(ESP_OK, work_queue_push(&work_queue_init_test_item));
}

void test_second_init_is_a_no_op(void)
{
    TEST_ASSERT_EQUAL(ESP_OK, work_queue_init());
    TEST_ASSERT_EQUAL_UINT32(0u, fake_idf_timer_create_count());
    TEST_ASSERT_EQUAL_UINT32(0u, fake_idf_task_create_count());
}

/**** END OF FILE ****/
