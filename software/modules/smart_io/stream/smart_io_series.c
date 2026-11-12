/**
 ******************************************************************************
 * @file    smart_io_series.c
 * @author  Daniel Dyringer
 * @date    2026-11-06
 * @brief   SERIES streams from ADC1 continuous: sample clock, decimation,
 *          encoding.
 ******************************************************************************
 */


/* Includes ---------------------------------------------------------------- */
#include "smart_io_series.h"

#include <string.h>

#include "esp_attr.h"
#include "utils.h"


/* Constants and macros ---------------------------------------------------- */
// clang-format off

#define SMART_IO_SERIES_CHANNEL_SHIFT       12u
#define SMART_IO_SERIES_RAW_MASK            0x0FFFu
#define SMART_IO_SERIES_RAW_MAX             4095u
/* Clock of the samples: off from the syncs by more than this many conversions
 * it jumps, otherwise it moves 1/DIVISOR of the error per tick (ISR jitter). */
#define SMART_IO_SERIES_JUMP_CONVERSIONS    4
#define SMART_IO_SERIES_NUDGE_DIVISOR       16
/* Conversions between the two syncs the real conversion period is measured
 * from; until then the requested period is used. */
#define SMART_IO_SERIES_MEASURE_MIN         256u
#define SMART_IO_SERIES_NS_PER_US           1000

// clang-format on

/* Private types ----------------------------------------------------------- */


/* Private variables ------------------------------------------------------- */


/* Private functions ------------------------------------------------------- */

static bool smart_io_series_sync_read(
    const smart_io_series_t* series,
    uint32_t* head,
    int64_t* time_us)
{
    uint32_t before = 0u;
    uint32_t after  = 0u;

    do
    {
        before = series->sync.sequence;
        __atomic_thread_fence(__ATOMIC_ACQUIRE);
        *head    = series->sync.head;
        *time_us = series->sync.time_us;
        __atomic_thread_fence(__ATOMIC_ACQUIRE);
        after = series->sync.sequence;
    } while ((before != after) || ((before & 1u) != 0u));

    return before != 0u;
}

static void smart_io_series_skip_to(
    smart_io_series_t* series,
    smart_io_ring_t* ring,
    uint32_t head,
    uint32_t pins,
    smart_io_series_drained_t* drained)
{
    uint32_t remaining = pins;

    while ((ring->tail != head) && (smart_io_ring_count(ring) > 0u))
    {
        smart_io_ring_drop(ring);
    }

    while (remaining != 0u)
    {
        const uint32_t index = (uint32_t)__builtin_ctz(remaining);

        remaining &= remaining - 1u;
        series->channels[index].phase = 0u;
    }

    drained->gap      = true;
    series->measuring = false;
    series->anchored  = false;
}

static void smart_io_series_anchor(
    smart_io_series_t* series,
    const smart_io_ring_t* ring,
    uint32_t head,
    int64_t time_us)
{
    const int64_t synced_ns = (time_us * SMART_IO_SERIES_NS_PER_US)
                              - ((int64_t)(head - 1u - ring->tail) * series->conversion_ns);
    const int64_t error_ns = synced_ns - series->next_ns;
    const int64_t jump_ns  = SMART_IO_SERIES_JUMP_CONVERSIONS * series->conversion_ns;

    if (!series->anchored || (error_ns > jump_ns) || (error_ns < -jump_ns))
    {
        if (series->anchored)
        {
            series->clock_jumps++;
        }

        series->next_ns  = synced_ns;
        series->anchored = true;
    }
    else
    {
        series->next_ns += error_ns / SMART_IO_SERIES_NUDGE_DIVISOR;
    }
}

static void smart_io_series_measure(smart_io_series_t* series, uint32_t head, int64_t time_us)
{
    const uint32_t conversions = head - series->measure_head;

    if (!series->measuring)
    {
        series->measuring       = true;
        series->measure_head    = head;
        series->measure_time_us = time_us;
        series->conversion_ns = (int64_t)series->conversion_us * SMART_IO_SERIES_NS_PER_US;
    }
    else if (conversions >= SMART_IO_SERIES_MEASURE_MIN)
    {
        series->conversion_ns =
            ((time_us - series->measure_time_us) * SMART_IO_SERIES_NS_PER_US)
            / (int64_t)conversions;
    }
    else
    {
        /* Too few conversions yet for a better figure. */
    }
}

static uint32_t smart_io_series_flush(
    smart_io_frame_writer_t* writer,
    uint8_t index,
    int64_t first_us,
    uint32_t period_us,
    const int32_t* values,
    size_t count)
{
    uint32_t failed = 0u;

    if ((count > 0u)
        && (smart_io_frame_series(writer, index, (uint64_t)first_us, period_us, values, count)
            != ESP_OK))
    {
        failed = (uint32_t)1u << index;
    }

    return failed;
}


/* Exposed API ------------------------------------------------------------- */

void smart_io_series_init(smart_io_series_t* series)
{
    memset(series, 0, sizeof(*series));

    for (size_t i = 0u; i < SMART_IO_SERIES_ADC_CHANNELS; i++)
    {
        series->latest_raw[i] = -1;
    }
}

void smart_io_series_timing(
    smart_io_series_t* series,
    uint32_t head,
    uint32_t conversion_us,
    uint32_t pattern_length)
{
    series->conversion_us  = conversion_us;
    series->pattern_length = pattern_length;
    series->restart_head   = head;
    series->restarted      = true;
    series->measuring      = false;
    series->anchored       = false;
    series->conversion_ns  = (int64_t)conversion_us * SMART_IO_SERIES_NS_PER_US;
}

void IRAM_ATTR smart_io_series_sync_from_isr(
    smart_io_series_t* series,
    uint32_t head,
    uint32_t invalid,
    int64_t time_us)
{
    __atomic_fetch_add(&series->invalid, invalid, __ATOMIC_RELAXED);

    series->sync.sequence = series->sync.sequence + 1u;
    __atomic_thread_fence(__ATOMIC_RELEASE);
    series->sync.head    = head;
    series->sync.time_us = time_us;
    __atomic_thread_fence(__ATOMIC_RELEASE);
    series->sync.sequence = series->sync.sequence + 1u;
}

bool smart_io_series_latest(const smart_io_series_t* series, uint8_t channel, int32_t* raw)
{
    bool found = false;

    if (channel < SMART_IO_SERIES_ADC_CHANNELS)
    {
        *raw  = __atomic_load_n(&series->latest_raw[channel], __ATOMIC_RELAXED);
        found = (*raw >= 0);
    }

    return found;
}

/* Sample times follow from the sync: one conversion every conversion_ns up to
 * the last at sync.time_us. A drop or a driver restart breaks that, so the
 * ring is skipped up to the sync and the streams get a GAP. Samples pushed
 * but not synced yet (ahead of head) are left for the next drain. */
void smart_io_series_drain(
    smart_io_series_t* series,
    smart_io_ring_t* ring,
    uint32_t pins,
    int64_t end_us,
    smart_io_stream_adc_sample_t* samples,
    size_t capacity,
    smart_io_series_drained_t* drained)
{
    uint32_t head   = 0u;
    int64_t time_us = 0;

    memset(drained, 0, sizeof(*drained));

    if (series->restarted)
    {
        series->restarted = false;
        smart_io_series_skip_to(series, ring, series->restart_head, pins, drained);
    }

    const bool synced = (series->conversion_us > 0u)
                        && smart_io_series_sync_read(series, &head, &time_us);

    drained->dropped = smart_io_ring_take_dropped(ring);

    if (drained->dropped != 0u)
    {
        smart_io_series_skip_to(series, ring, head, pins, drained);
    }
    else if (synced && ((int32_t)(head - ring->tail) > 0))
    {
        smart_io_series_measure(series, head, time_us);
        smart_io_series_anchor(series, ring, head, time_us);
    }
    else
    {
        /* Not running, or no conversion frame yet. */
    }

    while (synced && (drained->dropped == 0u) && ((int32_t)(head - ring->tail) > 0)
           && (drained->count < capacity))
    {
        const int64_t sample_us             = series->next_ns / SMART_IO_SERIES_NS_PER_US;
        smart_io_stream_adc_sample_t sample = 0u;

        if ((sample_us >= end_us) || !smart_io_ring_peek(ring, &sample, sizeof(sample)))
        {
            break;
        }

        smart_io_ring_drop(ring);
        series->next_ns += series->conversion_ns;

        const uint32_t channel = (uint32_t)sample >> SMART_IO_SERIES_CHANNEL_SHIFT;

        __atomic_store_n(
            &series->latest_raw[channel],
            (int32_t)((uint32_t)sample & SMART_IO_SERIES_RAW_MASK),
            __ATOMIC_RELAXED);

        /* Reported after the window closed (the ISR syncs once per
         * millisecond): kept, with a negative dt. */
        if (drained->count == 0u)
        {
            drained->first_us = sample_us;
        }

        samples[drained->count] = sample;
        drained->count++;
    }
}

/* Per streamed pin: every decimation-th conversion of its channel, a pattern
 * length of conversions apart. The DMA marks some results invalid
 * (channel >= 10); they keep their time slot, so a missing slot ends the block
 * and the next valid sample starts a new one. The period is rounded to whole
 * µs; each block restarts from its own first sample time. */
uint32_t smart_io_series_encode(
    smart_io_series_t* series,
    smart_io_frame_writer_t* writer,
    uint32_t pins,
    const smart_io_stream_adc_sample_t* samples,
    const smart_io_series_drained_t* drained,
    int32_t* values)
{
    const size_t pattern_length = series->pattern_length;
    const int64_t channel_ns    = (int64_t)pattern_length * series->conversion_ns;
    uint32_t remaining          = pins;
    uint32_t failed             = 0u;

    while ((remaining != 0u) && (channel_ns > 0))
    {
        const uint32_t index                     = (uint32_t)__builtin_ctz(remaining);
        smart_io_series_channel_t* const channel = &series->channels[index];
        const int64_t wanted_ns = (int64_t)channel->period_us * SMART_IO_SERIES_NS_PER_US;
        const uint32_t decimation =
            MAX(1u, (uint32_t)((wanted_ns + (channel_ns / 2)) / channel_ns));
        const uint32_t period_us = (uint32_t)((((int64_t)decimation * channel_ns)
                                               + (SMART_IO_SERIES_NS_PER_US / 2))
                                              / SMART_IO_SERIES_NS_PER_US);
        int64_t kept_us          = 0;
        size_t kept              = 0u;
        size_t previous_slot     = 0u; /* last slot of this channel, this drain */
        bool matched             = false;

        remaining &= remaining - 1u;

        for (size_t i = 0u; i < drained->count; i++)
        {
            const uint32_t sample = samples[i];
            const int64_t sample_us =
                drained->first_us
                + (((int64_t)i * series->conversion_ns) / SMART_IO_SERIES_NS_PER_US);

            if (((sample >> SMART_IO_SERIES_CHANNEL_SHIFT) == channel->adc_channel)
                && (sample_us >= channel->since_us))
            {
                if (matched && ((i - previous_slot) != pattern_length))
                {
                    failed |= smart_io_series_flush(
                        writer, (uint8_t)index, kept_us, period_us, values, kept);
                    kept           = 0u;
                    channel->phase = 0u;
                }

                if (channel->phase == 0u)
                {
                    if (kept == 0u)
                    {
                        kept_us = sample_us;
                    }

                    values[kept] = (int32_t)(((sample & SMART_IO_SERIES_RAW_MASK)
                                              * channel->full_scale_mv)
                                             / SMART_IO_SERIES_RAW_MAX);
                    kept++;
                }

                channel->phase = (channel->phase + 1u) % decimation;
                previous_slot  = i;
                matched        = true;
            }
        }

        failed |= smart_io_series_flush(
            writer, (uint8_t)index, kept_us, period_us, values, kept);
    }

    return failed;
}

/**** END OF FILE ****/
