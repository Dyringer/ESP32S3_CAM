#include <string.h>

#include "smart_io_frame.h"
#include "smart_io_ring.h"
#include "smart_io_series.h"
#include "unity.h"

#define TEST_SERIES_DEPTH   64u
#define TEST_SERIES_CHANNEL 3u
#define TEST_SERIES_PIN     2u

static smart_io_series_t test_series;
static smart_io_ring_t test_ring;
static uint8_t test_ring_buffer[TEST_SERIES_DEPTH * sizeof(smart_io_stream_adc_sample_t)];
static smart_io_stream_adc_sample_t test_samples[TEST_SERIES_DEPTH];
static int32_t test_values[TEST_SERIES_DEPTH];
static uint8_t test_frame[256];

static void test_series_push(uint32_t channel, uint32_t raw)
{
    const smart_io_stream_adc_sample_t sample =
        (smart_io_stream_adc_sample_t)((channel << 12u) | raw);

    smart_io_ring_push(&test_ring, &sample, sizeof(sample));
}

static void test_series_stream(uint32_t period_us)
{
    smart_io_series_channel_t* const channel = &test_series.channels[TEST_SERIES_PIN];

    channel->since_us      = 0;
    channel->period_us     = period_us;
    channel->full_scale_mv = 4095u; /* mV = raw */
    channel->phase         = 0u;
    channel->adc_channel   = (uint8_t)TEST_SERIES_CHANNEL;
}

void setUp(void)
{
    smart_io_series_init(&test_series);
    smart_io_ring_init(&test_ring, test_ring_buffer, TEST_SERIES_DEPTH);
    memset(test_frame, 0, sizeof(test_frame));
}

void tearDown(void) {}

/* 10 conversions at 200 µs, the last at 10 ms: times count back from the sync. */
void test_smart_io_series_times_samples_from_the_sync(void)
{
    smart_io_series_drained_t drained = {0};
    int32_t raw                       = 0;

    smart_io_series_timing(&test_series, 0u, 200u, 1u);

    for (uint32_t i = 0u; i < 10u; i++)
    {
        test_series_push(TEST_SERIES_CHANNEL, i * 10u);
    }

    smart_io_series_sync_from_isr(&test_series, 10u, 0u, 10000);
    smart_io_series_drain(
        &test_series,
        &test_ring,
        1u << TEST_SERIES_PIN,
        20000,
        test_samples,
        TEST_SERIES_DEPTH,
        &drained);

    TEST_ASSERT_EQUAL_size_t(10u, drained.count);
    TEST_ASSERT_EQUAL_INT64(8200, drained.first_us);
    TEST_ASSERT_TRUE(drained.gap); /* first drain after a (re)start */
    TEST_ASSERT_EQUAL_UINT32(0u, drained.dropped);
    TEST_ASSERT_TRUE(
        smart_io_series_latest(&test_series, (uint8_t)TEST_SERIES_CHANNEL, &raw));
    TEST_ASSERT_EQUAL_INT32(90, raw);
}

void test_smart_io_series_leaves_unsynced_and_late_samples(void)
{
    smart_io_series_drained_t drained = {0};

    smart_io_series_timing(&test_series, 0u, 200u, 1u);

    for (uint32_t i = 0u; i < 10u; i++)
    {
        test_series_push(TEST_SERIES_CHANNEL, i);
    }

    /* Only 6 synced, the 6th at 10 ms: 9000 µs ends the window after 4. */
    smart_io_series_sync_from_isr(&test_series, 6u, 0u, 10000);
    smart_io_series_drain(
        &test_series,
        &test_ring,
        1u << TEST_SERIES_PIN,
        9400,
        test_samples,
        TEST_SERIES_DEPTH,
        &drained);

    TEST_ASSERT_EQUAL_size_t(2u, drained.count);
    TEST_ASSERT_EQUAL_INT64(9000, drained.first_us);
    TEST_ASSERT_EQUAL_UINT32(8u, smart_io_ring_count(&test_ring));
}

void test_smart_io_series_ring_drop_is_a_gap(void)
{
    smart_io_series_drained_t drained = {0};

    smart_io_series_timing(&test_series, 0u, 200u, 1u);
    smart_io_series_drain(
        &test_series,
        &test_ring,
        1u << TEST_SERIES_PIN,
        20000,
        test_samples,
        TEST_SERIES_DEPTH,
        &drained);

    for (uint32_t i = 0u; i < (TEST_SERIES_DEPTH + 2u); i++)
    {
        test_series_push(TEST_SERIES_CHANNEL, i);
    }

    smart_io_series_sync_from_isr(&test_series, TEST_SERIES_DEPTH, 0u, 20000);
    smart_io_series_drain(
        &test_series,
        &test_ring,
        1u << TEST_SERIES_PIN,
        30000,
        test_samples,
        TEST_SERIES_DEPTH,
        &drained);

    TEST_ASSERT_EQUAL_UINT32(2u, drained.dropped);
    TEST_ASSERT_TRUE(drained.gap);
    TEST_ASSERT_EQUAL_size_t(0u, drained.count);
    TEST_ASSERT_EQUAL_UINT32(0u, smart_io_ring_count(&test_ring));
}

/* 5 kHz conversions, pin asks for 2.5 kHz: every 2nd sample, period 400 µs. */
void test_smart_io_series_encodes_decimated_block(void)
{
    smart_io_series_drained_t drained = {0};
    smart_io_frame_writer_t writer    = {0};
    const uint8_t expected[]          = {
        0x22, /* SERIES, pool 2 */
        0x90,
        0x03, /* dt +200 µs (zigzag 400) */
        0x90,
        0x03, /* period 400 µs */
        0x05, /* 5 samples */
        0x00,
        0x28,
        0x28, /* 0, +20, +20 */
        0x28,
        0x28, /* +20, +20 */
    };

    smart_io_series_timing(&test_series, 0u, 200u, 1u);
    test_series_stream(400u);

    for (uint32_t i = 0u; i < 10u; i++)
    {
        test_series_push(TEST_SERIES_CHANNEL, i * 10u);
    }

    smart_io_series_sync_from_isr(&test_series, 10u, 0u, 10000);
    smart_io_series_drain(
        &test_series,
        &test_ring,
        1u << TEST_SERIES_PIN,
        20000,
        test_samples,
        TEST_SERIES_DEPTH,
        &drained);
    smart_io_frame_begin(&writer, test_frame, sizeof(test_frame), 8000u, 10000u);

    TEST_ASSERT_EQUAL_UINT32(
        0u,
        smart_io_series_encode(
            &test_series, &writer, 1u << TEST_SERIES_PIN, test_samples, &drained, test_values));
    TEST_ASSERT_EQUAL_UINT8(1u, writer.block_count);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(expected, &test_frame[11], sizeof(expected));
}

/* Two channels alternate; the DMA marks one slot invalid (channel 15): the
 * pin's run breaks there and a second block starts. */
void test_smart_io_series_invalid_slot_splits_block(void)
{
    static const uint32_t channels[]  = {3u, 4u, 3u, 4u, 15u, 4u, 3u, 4u};
    smart_io_series_drained_t drained = {0};
    smart_io_frame_writer_t writer    = {0};

    smart_io_series_timing(&test_series, 0u, 100u, 2u);
    test_series_stream(200u);

    for (uint32_t i = 0u; i < 8u; i++)
    {
        test_series_push(channels[i], i);
    }

    smart_io_series_sync_from_isr(&test_series, 8u, 1u, 10000);
    smart_io_series_drain(
        &test_series,
        &test_ring,
        1u << TEST_SERIES_PIN,
        20000,
        test_samples,
        TEST_SERIES_DEPTH,
        &drained);
    smart_io_frame_begin(&writer, test_frame, sizeof(test_frame), 9000u, 10000u);

    TEST_ASSERT_EQUAL_size_t(8u, drained.count);
    TEST_ASSERT_EQUAL_UINT32(
        0u,
        smart_io_series_encode(
            &test_series, &writer, 1u << TEST_SERIES_PIN, test_samples, &drained, test_values));
    TEST_ASSERT_EQUAL_UINT8(2u, writer.block_count);
    TEST_ASSERT_EQUAL_UINT32(1u, test_series.invalid);
}
