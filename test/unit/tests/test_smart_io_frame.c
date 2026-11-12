#include <string.h>

#include "smart_io_frame.h"
#include "unity.h"

#define TEST_FRAME_START_US 0x0000000102030405ull
#define TEST_FRAME_SIZE     64u

static uint8_t test_frame_buffer[TEST_FRAME_SIZE];
static smart_io_frame_writer_t test_frame_writer;

void setUp(void)
{
    memset(test_frame_buffer, 0xEE, sizeof(test_frame_buffer));
    TEST_ASSERT_EQUAL(
        ESP_OK,
        smart_io_frame_begin(
            &test_frame_writer,
            test_frame_buffer,
            sizeof(test_frame_buffer),
            TEST_FRAME_START_US,
            10000u));
}

void tearDown(void) {}

/* SMART_IO2.md §8.2: GPIO 4 and 5 streamed (pool 0, 1), ADC on pool 2. */
void test_smart_io_frame_matches_protocol_example(void)
{
    const uint32_t start                             = (uint32_t)TEST_FRAME_START_US;
    const smart_io_stream_digital_sample_t digital[] = {
        {.time_us = start + 100u, .levels = 0x1u},
        {.time_us = start + 300u, .levels = 0x2u},
    };
    const int32_t series[]   = {1650, 1653, 1651};
    const uint8_t expected[] = {
        0x05, 0x04, 0x03, 0x02, 0x01, 0x00, 0x00, 0x00, /* start_us */
        0x90, 0x4E,                                     /* 10000 us */
        0x02,                                           /* blocks */
        0x00, 0x03, 0x00, 0x02, 0x64, 0x01, 0xC8, 0x01, 0x03,
        0x22, 0x64, 0xC8, 0x01, 0x03, 0xE4, 0x19, 0x06, 0x03,
    };

    TEST_ASSERT_EQUAL(
        ESP_OK, smart_io_frame_digital(&test_frame_writer, 0x3u, 0x0u, digital, 2u));
    TEST_ASSERT_EQUAL(
        ESP_OK,
        smart_io_frame_series(
            &test_frame_writer, 2u, TEST_FRAME_START_US + 50u, 200u, series, 3u));

    TEST_ASSERT_EQUAL(sizeof(expected), smart_io_frame_end(&test_frame_writer));
    TEST_ASSERT_EQUAL_HEX8_ARRAY(expected, test_frame_buffer, sizeof(expected));
}

void test_smart_io_frame_value_allows_negative_time(void)
{
    const uint32_t start                          = (uint32_t)TEST_FRAME_START_US;
    const smart_io_stream_value_sample_t values[] = {
        {.time_us = start - 2u, .value = 1, .index = 3u},
        {.time_us = start + 1u, .value = 0, .index = 3u},
    };
    const uint8_t expected[] = {0x43, 0x02, 0x03, 0x02, 0x06, 0x01};

    TEST_ASSERT_EQUAL(ESP_OK, smart_io_frame_value(&test_frame_writer, 3u, values, 2u));
    TEST_ASSERT_EQUAL_HEX8_ARRAY(expected, &test_frame_buffer[11], sizeof(expected));
    TEST_ASSERT_EQUAL_UINT8(1u, test_frame_buffer[10]);
}

void test_smart_io_frame_block_that_does_not_fit_leaves_frame_unchanged(void)
{
    const uint8_t data[TEST_FRAME_SIZE] = {0};

    TEST_ASSERT_EQUAL(ESP_OK, smart_io_frame_gap(&test_frame_writer, 7u));
    TEST_ASSERT_EQUAL(
        ESP_ERR_NO_MEM,
        smart_io_frame_bytes(
            &test_frame_writer, 1u, TEST_FRAME_START_US, data, sizeof(data)));

    TEST_ASSERT_EQUAL(12u, smart_io_frame_end(&test_frame_writer));
    TEST_ASSERT_EQUAL_UINT8(1u, test_frame_buffer[10]);
    TEST_ASSERT_EQUAL_HEX8(0x87u, test_frame_buffer[11]);
}

/* block_count is one byte: block 256 must be refused, not wrap the count to 0. */
void test_smart_io_frame_refuses_block_past_count_limit(void)
{
    static uint8_t buffer[512];
    smart_io_frame_writer_t writer = {0};

    TEST_ASSERT_EQUAL(
        ESP_OK,
        smart_io_frame_begin(&writer, buffer, sizeof(buffer), TEST_FRAME_START_US, 10000u));

    for (uint32_t i = 0u; i < SMART_IO_FRAME_BLOCKS_MAX; i++)
    {
        TEST_ASSERT_EQUAL(ESP_OK, smart_io_frame_gap(&writer, (uint8_t)(i & 0x1Fu)));
    }

    const size_t length = smart_io_frame_end(&writer);

    TEST_ASSERT_EQUAL(ESP_ERR_NO_MEM, smart_io_frame_gap(&writer, 0u));
    TEST_ASSERT_EQUAL(length, smart_io_frame_end(&writer));
    TEST_ASSERT_EQUAL_UINT8(SMART_IO_FRAME_BLOCKS_MAX, buffer[10]);
}
