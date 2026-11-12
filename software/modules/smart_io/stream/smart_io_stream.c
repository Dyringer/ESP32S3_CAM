/**
 ******************************************************************************
 * @file    smart_io_stream.c
 * @author  Daniel Dyringer
 * @date    2026-10-31
 * @brief   Stream channels, sio_strm task, admission, backlog.
 ******************************************************************************
 */


/* Includes ---------------------------------------------------------------- */
#include <string.h>

#include "esp_attr.h"
#include "esp_event.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "smart_io.h" /* SMART_IO_EVENTS */
#include "smart_io_frame.h"
#include "smart_io_ring.h"
#include "smart_io_series.h"
#include "smart_io_stream_source.h"
#include "utils.h"


/* Constants and macros ---------------------------------------------------- */
// clang-format off

#define SMART_IO_STREAM_TASK_STACK          4096u
#define SMART_IO_STREAM_TASK_PRIO           10u
#define SMART_IO_STREAM_TASK_CORE           1

#define SMART_IO_STREAM_TICK_MS             10u
#define SMART_IO_STREAM_IDLE_FRAME_MS       100u
#define SMART_IO_STREAM_NOTIFY_MS           50u

/* Edges per tick over all streamed inputs (50k/s). */
#define SMART_IO_STREAM_EDGE_BUDGET         500u
#define SMART_IO_STREAM_FLOOD_PAUSE_MS      1000u

/* Ring depths, powers of two. */
#define SMART_IO_STREAM_DIGITAL_DEPTH       1024u
#define SMART_IO_STREAM_ADC_DEPTH           2048u
#define SMART_IO_STREAM_VALUE_DEPTH         256u

#define SMART_IO_STREAM_BACKLOG_SIZE        32768u
/* Holds a full tick: 1024 digital samples x 10 B, 2048 ADC samples x 3 B,
 * 256 values x 10 B and the gaps. BYTES take what is left (see below). */
#define SMART_IO_STREAM_SCRATCH_SIZE        24576u
/* BYTES read per pin and tick: 2x the 1 Mbaud rate, so a port catches up
 * after a stall (flash write). The rest waits in the driver's RX buffer. */
#define SMART_IO_STREAM_BYTES_TICK_MAX      2048u
/* Block id, dt and count varints. */
#define SMART_IO_STREAM_BYTES_HEADER        11u
/* Frames kept in the backlog, power of two. */
#define SMART_IO_STREAM_FRAME_DEPTH         512u

#define SMART_IO_STREAM_CHANNELS            32u
#define SMART_IO_STREAM_US_PER_MS           1000

// clang-format on

/* Private types ----------------------------------------------------------- */

typedef struct smart_io_stream_channel_s
{
    int64_t since_us;
    smart_io_stream_kind_t kind;
    /* Hooks, see smart_io_stream_source_t. */
    smart_io_stream_drain_fn_t drain;
    smart_io_stream_pause_fn_t pause;
    smart_io_stream_resume_fn_t resume;
    void* context;
} smart_io_stream_channel_t;

typedef struct smart_io_stream_frame_entry_s
{
    uint32_t offset;
    uint32_t length;
} smart_io_stream_frame_entry_t;

/* Frames never wrap: one that doesn't fit at the end starts at 0, so a run of
 * frames is always contiguous. */
typedef struct smart_io_stream_backlog_s
{
    uint8_t* buffer;
    smart_io_stream_frame_entry_t* frames; /* index = sequence & (depth - 1) */
    uint64_t first;                        /* sequence of the oldest frame */
    uint32_t count;
    uint32_t write_offset;
} smart_io_stream_backlog_t;

/* sio_strm only. */
typedef struct smart_io_stream_tick_s
{
    int64_t frame_start_us;
    int64_t last_frame_us;
    int64_t last_notify_us;
    int64_t flood_until_us; /* 0: not flooded */
    int64_t last_restate_us;
    uint32_t levels;       /* DIGITAL levels at frame_start_us */
    uint32_t sent_digital; /* pins of the last DIGITAL block */
    uint32_t gaps;
    uint32_t failed;      /* blocks of this tick that didn't fit: GAP in the next frame */
    uint32_t value_known; /* VALUE pins with a value in values[] */
    int32_t values[SMART_IO_STREAM_CHANNELS]; /* last VALUE sent per pin */
    bool unposted;
} smart_io_stream_tick_t;


/* Private variables ------------------------------------------------------- */

static const char* smart_io_stream_tag = "smart_io_stream";

/* Stream lock: channels, rings, scratch, stats and the tick state. */
static SemaphoreHandle_t smart_io_stream_lock   = NULL;
static SemaphoreHandle_t smart_io_stream_mutex  = NULL; /* backlog */
static TaskHandle_t smart_io_stream_task_handle = NULL;

static smart_io_stream_channel_t smart_io_stream_channels[SMART_IO_STREAM_CHANNELS];
/* Written under the stream lock, read lock-free by smart_io_stream_mask(). */
static uint32_t smart_io_stream_channel_mask = 0u;
/* Guarded by the smart_io mutex. */
static uint32_t smart_io_stream_client_count = 0u;

/* Rings in internal RAM: producers run during flash writes. */
static smart_io_ring_t smart_io_stream_digital_ring = {0};
static smart_io_ring_t smart_io_stream_value_ring   = {0};

/* PSRAM, allocated with the first client. */
static uint8_t* smart_io_stream_scratch                                  = NULL;
static smart_io_stream_digital_sample_t* smart_io_stream_digital_samples = NULL;
static smart_io_stream_value_sample_t* smart_io_stream_value_samples     = NULL;
static smart_io_stream_value_sample_t* smart_io_stream_value_block       = NULL;
static uint8_t* smart_io_stream_bytes                                    = NULL;

static smart_io_stream_backlog_t smart_io_stream_backlog  = {0};
static smart_io_stream_tick_t smart_io_stream_tick_state  = {0};
static smart_io_stream_stats_t smart_io_stream_statistics = {0};

/* ADC1 continuous. Ring and series in internal RAM: the ISR runs during flash
 * writes. */
static smart_io_ring_t smart_io_stream_adc_ring = {0};
static smart_io_series_t smart_io_stream_series = {0};
/* PSRAM, with the other tick buffers. */
static smart_io_stream_adc_sample_t* smart_io_stream_adc_samples = NULL;
static int32_t* smart_io_stream_series_values                    = NULL;

/* Streamed GPIO ISR -> sio_strm. */
static uint32_t smart_io_stream_edges   = 0u;
static bool smart_io_stream_edges_flood = false;


/* Private functions ------------------------------------------------------- */

static uint32_t smart_io_stream_kind_mask(smart_io_stream_kind_t kind)
{
    uint32_t mask      = 0u;
    uint32_t remaining = smart_io_stream_channel_mask;

    while (remaining != 0u)
    {
        const uint32_t index = (uint32_t)__builtin_ctz(remaining);

        remaining &= remaining - 1u;

        if (smart_io_stream_channels[index].kind == kind)
        {
            mask |= (uint32_t)1u << index;
        }
    }

    return mask;
}

static int64_t smart_io_stream_widen(int64_t end_us, uint32_t time_us)
{
    return end_us - (int64_t)(uint32_t)((uint32_t)end_us - time_us);
}

/* ---- Memory ---- */

static void smart_io_stream_ring_free(smart_io_ring_t* ring)
{
    heap_caps_free(ring->buffer);
    memset(ring, 0, sizeof(smart_io_ring_t));
}

static void smart_io_stream_buffers_free(void)
{
    heap_caps_free(smart_io_stream_scratch);
    heap_caps_free(smart_io_stream_digital_samples);
    heap_caps_free(smart_io_stream_value_samples);
    heap_caps_free(smart_io_stream_value_block);
    heap_caps_free(smart_io_stream_adc_samples);
    heap_caps_free(smart_io_stream_series_values);
    heap_caps_free(smart_io_stream_bytes);
    smart_io_stream_bytes           = NULL;
    smart_io_stream_adc_samples     = NULL;
    smart_io_stream_series_values   = NULL;
    smart_io_stream_scratch         = NULL;
    smart_io_stream_digital_samples = NULL;
    smart_io_stream_value_samples   = NULL;
    smart_io_stream_value_block     = NULL;

    xSemaphoreTake(smart_io_stream_mutex, portMAX_DELAY);
    heap_caps_free(smart_io_stream_backlog.buffer);
    heap_caps_free(smart_io_stream_backlog.frames);
    memset(&smart_io_stream_backlog, 0, sizeof(smart_io_stream_backlog));
    xSemaphoreGive(smart_io_stream_mutex);
}

static esp_err_t smart_io_stream_buffers_alloc(void)
{
    esp_err_t error_code = ESP_OK;
    const uint32_t caps  = MALLOC_CAP_SPIRAM;

    smart_io_stream_scratch = heap_caps_malloc(SMART_IO_STREAM_SCRATCH_SIZE, caps);
    smart_io_stream_digital_samples = heap_caps_malloc(
        SMART_IO_STREAM_DIGITAL_DEPTH * sizeof(smart_io_stream_digital_sample_t), caps);
    smart_io_stream_value_samples = heap_caps_malloc(
        SMART_IO_STREAM_VALUE_DEPTH * sizeof(smart_io_stream_value_sample_t), caps);
    smart_io_stream_value_block = heap_caps_malloc(
        SMART_IO_STREAM_VALUE_DEPTH * sizeof(smart_io_stream_value_sample_t), caps);
    smart_io_stream_adc_samples = heap_caps_malloc(
        SMART_IO_STREAM_ADC_DEPTH * sizeof(smart_io_stream_adc_sample_t), caps);
    smart_io_stream_series_values =
        heap_caps_malloc(SMART_IO_STREAM_ADC_DEPTH * sizeof(int32_t), caps);
    smart_io_stream_bytes = heap_caps_malloc(SMART_IO_STREAM_BYTES_TICK_MAX, caps);

    uint8_t* const backlog = heap_caps_malloc(SMART_IO_STREAM_BACKLOG_SIZE, caps);
    smart_io_stream_frame_entry_t* const frames = heap_caps_malloc(
        SMART_IO_STREAM_FRAME_DEPTH * sizeof(smart_io_stream_frame_entry_t), caps);

    xSemaphoreTake(smart_io_stream_mutex, portMAX_DELAY);
    smart_io_stream_backlog.buffer       = backlog;
    smart_io_stream_backlog.frames       = frames;
    smart_io_stream_backlog.first        = 0u;
    smart_io_stream_backlog.count        = 0u;
    smart_io_stream_backlog.write_offset = 0u;
    xSemaphoreGive(smart_io_stream_mutex);

    if ((smart_io_stream_scratch == NULL) || (smart_io_stream_digital_samples == NULL)
        || (smart_io_stream_value_samples == NULL) || (smart_io_stream_value_block == NULL)
        || (smart_io_stream_adc_samples == NULL) || (smart_io_stream_series_values == NULL)
        || (smart_io_stream_bytes == NULL) || (backlog == NULL) || (frames == NULL))
    {
        smart_io_stream_buffers_free();
        error_code = ESP_ERR_NO_MEM;
    }

    return error_code;
}

/* ---- Backlog ---- */

static void smart_io_stream_backlog_evict(void)
{
    smart_io_stream_backlog.first++;
    smart_io_stream_backlog.count--;
}

static const smart_io_stream_frame_entry_t* smart_io_stream_backlog_oldest(void)
{
    const uint64_t slot =
        smart_io_stream_backlog.first & (SMART_IO_STREAM_FRAME_DEPTH - 1u);

    return &smart_io_stream_backlog.frames[slot];
}

static bool smart_io_stream_backlog_overlaps(uint32_t offset, uint32_t length)
{
    const smart_io_stream_frame_entry_t* const oldest = smart_io_stream_backlog_oldest();

    return (oldest->offset < (offset + length))
           && (offset < (oldest->offset + oldest->length));
}

static void smart_io_stream_backlog_append(const uint8_t* frame, uint32_t length)
{
    xSemaphoreTake(smart_io_stream_mutex, portMAX_DELAY);

    uint32_t offset = smart_io_stream_backlog.write_offset;

    /* Wrapping: the frames between the write offset and the end are the oldest. */
    if ((offset + length) > SMART_IO_STREAM_BACKLOG_SIZE)
    {
        while ((smart_io_stream_backlog.count > 0u)
               && (smart_io_stream_backlog_oldest()->offset >= offset))
        {
            smart_io_stream_backlog_evict();
        }

        offset = 0u;
    }

    while ((smart_io_stream_backlog.count > 0u)
           && ((smart_io_stream_backlog.count == SMART_IO_STREAM_FRAME_DEPTH)
               || smart_io_stream_backlog_overlaps(offset, length)))
    {
        smart_io_stream_backlog_evict();
    }

    const uint64_t sequence = smart_io_stream_backlog.first + smart_io_stream_backlog.count;
    smart_io_stream_frame_entry_t* const entry =
        &smart_io_stream_backlog.frames[sequence & (SMART_IO_STREAM_FRAME_DEPTH - 1u)];

    entry->offset = offset;
    entry->length = length;
    memcpy(&smart_io_stream_backlog.buffer[offset], frame, length);
    smart_io_stream_backlog.count++;
    smart_io_stream_backlog.write_offset = offset + length;

    xSemaphoreGive(smart_io_stream_mutex);
}

/* ---- Tick (sio_strm, stream lock held) ---- */

static void smart_io_stream_post(smart_io_event_t event, const void* data, size_t size)
{
    const esp_err_t error_code =
        esp_event_post(SMART_IO_EVENTS, (int32_t)event, data, size, 0);

    if (error_code != ESP_OK)
    {
        ESP_LOGD(
            smart_io_stream_tag,
            "Event %d dropped: %s",
            (int)event,
            esp_err_to_name(error_code));
    }
}

static void smart_io_stream_digital_pause(uint32_t digital)
{
    uint32_t remaining = digital;

    while (remaining != 0u)
    {
        const uint32_t index = (uint32_t)__builtin_ctz(remaining);
        const smart_io_stream_channel_t* const channel = &smart_io_stream_channels[index];

        remaining &= remaining - 1u;
        channel->pause(channel->context);
    }
}

/* Over budget: streamed interrupts off and a GAP; no DIGITAL blocks during the
 * pause. Re-armed after it with the levels read again and another GAP. */
static void smart_io_stream_flood_check(int64_t end_us, uint32_t digital)
{
    smart_io_stream_tick_t* const tick = &smart_io_stream_tick_state;
    const uint32_t edges =
        __atomic_exchange_n(&smart_io_stream_edges, 0u, __ATOMIC_RELAXED);
    const bool flood =
        __atomic_exchange_n(&smart_io_stream_edges_flood, false, __ATOMIC_RELAXED);

    smart_io_stream_statistics.edges_per_tick_max =
        MAX(smart_io_stream_statistics.edges_per_tick_max, edges);

    if (flood && (tick->flood_until_us == 0))
    {
        const uint32_t pause_ms = SMART_IO_STREAM_FLOOD_PAUSE_MS;

        smart_io_stream_digital_pause(digital);
        tick->flood_until_us =
            end_us + ((int64_t)SMART_IO_STREAM_FLOOD_PAUSE_MS * SMART_IO_STREAM_US_PER_MS);
        tick->gaps |= digital;
        smart_io_stream_post(SMART_IO_EVENT_STREAM_FLOOD, &pause_ms, sizeof(pause_ms));
        ESP_LOGW(
            smart_io_stream_tag, "Edge flood: inputs paused %u ms", (unsigned)pause_ms);
    }
    else if ((tick->flood_until_us != 0) && (end_us >= tick->flood_until_us))
    {
        uint32_t remaining = digital;

        tick->flood_until_us = 0;
        __atomic_store_n(&smart_io_stream_edges, 0u, __ATOMIC_RELAXED);

        while (remaining != 0u)
        {
            const uint32_t index = (uint32_t)__builtin_ctz(remaining);
            const uint32_t bit   = (uint32_t)1u << index;
            const smart_io_stream_channel_t* const channel =
                &smart_io_stream_channels[index];

            remaining &= remaining - 1u;
            tick->levels &= ~bit;

            if (channel->resume(channel->context) != 0)
            {
                tick->levels |= bit;
            }
        }

        tick->gaps |= digital;
    }
    else
    {
        /* No flood, or still paused. */
    }
}

/* Only samples older than end_us: later ones belong to the next frame. Pins
 * whose stream started after a snapshot keep their initial level. */
static size_t smart_io_stream_drain_digital(int64_t end_us, uint32_t digital)
{
    smart_io_ring_t* const ring             = &smart_io_stream_digital_ring;
    smart_io_stream_tick_t* const tick      = &smart_io_stream_tick_state;
    smart_io_stream_digital_sample_t sample = {0};
    size_t count                            = 0u;

    while ((ring->buffer != NULL) && (count < SMART_IO_STREAM_DIGITAL_DEPTH)
           && smart_io_ring_peek(ring, &sample, sizeof(sample))
           && ((int32_t)(sample.time_us - (uint32_t)end_us) < 0))
    {
        const int64_t time_us = smart_io_stream_widen(end_us, sample.time_us);
        uint32_t valid        = 0u;
        uint32_t remaining    = digital;

        smart_io_ring_drop(ring);

        while (remaining != 0u)
        {
            const uint32_t index = (uint32_t)__builtin_ctz(remaining);

            remaining &= remaining - 1u;

            if (smart_io_stream_channels[index].since_us <= time_us)
            {
                valid |= (uint32_t)1u << index;
            }
        }

        const uint32_t levels = (sample.levels & valid) | (tick->levels & digital & ~valid);

        /* Pushed between a stream start and the first window of a new run. */
        const uint32_t sample_time_us = (time_us < tick->frame_start_us) ?
                                            (uint32_t)tick->frame_start_us :
                                            sample.time_us;

        if (levels != (tick->levels & digital))
        {
            smart_io_stream_digital_samples[count].time_us = sample_time_us;
            smart_io_stream_digital_samples[count].levels  = levels;
            tick->levels = (tick->levels & ~digital) | levels;
            count++;
        }
    }

    if (ring->buffer != NULL)
    {
        const uint32_t dropped = smart_io_ring_take_dropped(ring);

        if (dropped != 0u)
        {
            smart_io_stream_statistics.lost += dropped;
            tick->gaps |= digital;
        }
    }

    return count;
}

static size_t smart_io_stream_drain_values(int64_t end_us, uint32_t values)
{
    smart_io_ring_t* const ring           = &smart_io_stream_value_ring;
    smart_io_stream_value_sample_t sample = {0};
    size_t count                          = 0u;

    while ((ring->buffer != NULL) && (count < SMART_IO_STREAM_VALUE_DEPTH)
           && smart_io_ring_peek(ring, &sample, sizeof(sample))
           && ((int32_t)(sample.time_us - (uint32_t)end_us) < 0))
    {
        const uint32_t bit = (uint32_t)1u << sample.index;

        smart_io_ring_drop(ring);

        if (((values & bit) != 0u)
            && (smart_io_stream_widen(end_us, sample.time_us)
                >= smart_io_stream_channels[sample.index].since_us))
        {
            smart_io_stream_value_samples[count] = sample;
            count++;
        }
    }

    if (ring->buffer != NULL)
    {
        const uint32_t dropped = smart_io_ring_take_dropped(ring);

        if (dropped != 0u)
        {
            smart_io_stream_statistics.lost += dropped;
            smart_io_stream_tick_state.gaps |= values;
        }
    }

    return count;
}

static void smart_io_stream_check_block(esp_err_t error_code, uint32_t pins)
{
    if (error_code != ESP_OK)
    {
        smart_io_stream_tick_state.failed |= pins;
    }
}

/* Scratch holds a full tick (see SMART_IO_STREAM_SCRATCH_SIZE), but the block
 * count may run out; a block that doesn't fit is a GAP in the next frame.
 * restate_due: every pin
 * states its current level or value, so a client that joined late is right
 * again by the next restatement. */
static void smart_io_stream_encode(
    smart_io_frame_writer_t* writer,
    uint32_t digital,
    uint32_t values,
    size_t digital_count,
    size_t value_count,
    uint32_t start_levels,
    bool restate_due)
{
    smart_io_stream_tick_t* const tick = &smart_io_stream_tick_state;
    uint32_t remaining                 = tick->gaps & smart_io_stream_channel_mask;

    while (remaining != 0u)
    {
        const uint32_t index = (uint32_t)__builtin_ctz(remaining);

        remaining &= remaining - 1u;

        const esp_err_t error_code = smart_io_frame_gap(writer, (uint8_t)index);

        smart_io_stream_check_block(error_code, (uint32_t)1u << index);

        if (error_code == ESP_OK)
        {
            smart_io_stream_statistics.gaps++;
        }
    }

    if ((digital != 0u)
        && ((digital_count > 0u) || (digital != tick->sent_digital) || restate_due))
    {
        smart_io_stream_check_block(
            smart_io_frame_digital(
                writer, digital, start_levels, smart_io_stream_digital_samples, digital_count),
            digital);
        tick->sent_digital = digital;
    }

    remaining = values;

    while (remaining != 0u)
    {
        const uint32_t index = (uint32_t)__builtin_ctz(remaining);
        size_t block_count   = 0u;

        remaining &= remaining - 1u;

        for (size_t i = 0u; i < value_count; i++)
        {
            if (smart_io_stream_value_samples[i].index == index)
            {
                smart_io_stream_value_block[block_count] = smart_io_stream_value_samples[i];
                block_count++;
            }
        }

        const uint32_t bit = (uint32_t)1u << index;

        if (block_count > 0u)
        {
            tick->values[index] = smart_io_stream_value_block[block_count - 1u].value;
            tick->value_known |= bit;
        }
        else if (restate_due && ((tick->value_known & bit) != 0u))
        {
            smart_io_stream_value_block[0].time_us = (uint32_t)writer->start_us;
            smart_io_stream_value_block[0].value   = tick->values[index];
            smart_io_stream_value_block[0].index   = (uint8_t)index;
            block_count                            = 1u;
        }
        else
        {
            /* Nothing new for this pin. */
        }

        if (block_count > 0u)
        {
            smart_io_stream_check_block(
                smart_io_frame_value(
                    writer, (uint8_t)index, smart_io_stream_value_block, block_count),
                bit);
        }
    }
}

/* ---- BYTES (sio_strm, stream lock held) ---- */

/* Reads only what fits the rest of the frame; the remainder stays in the
 * role's buffer for the next tick. A loss puts a GAP before the block. */
static void smart_io_stream_encode_bytes(
    smart_io_frame_writer_t* writer,
    uint32_t bytes,
    int64_t end_us)
{
    uint32_t remaining = bytes;

    while (remaining != 0u)
    {
        const uint32_t index = (uint32_t)__builtin_ctz(remaining);
        const smart_io_stream_channel_t* const channel = &smart_io_stream_channels[index];
        const size_t room                              = writer->size - writer->length;
        bool overflow                                  = false;
        size_t count                                   = 0u;

        remaining &= remaining - 1u;

        if (room > SMART_IO_STREAM_BYTES_HEADER)
        {
            count = channel->drain(
                channel->context,
                smart_io_stream_bytes,
                MIN(SMART_IO_STREAM_BYTES_TICK_MAX, room - SMART_IO_STREAM_BYTES_HEADER),
                &overflow);
        }

        if (overflow)
        {
            const esp_err_t error_code = smart_io_frame_gap(writer, (uint8_t)index);

            smart_io_stream_check_block(error_code, (uint32_t)1u << index);

            if (error_code == ESP_OK)
            {
                smart_io_stream_statistics.gaps++;
            }
        }

        if (count > 0u)
        {
            smart_io_stream_check_block(
                smart_io_frame_bytes(
                    writer, (uint8_t)index, (uint64_t)end_us, smart_io_stream_bytes, count),
                (uint32_t)1u << index);
        }
    }
}

static void smart_io_stream_tick(void)
{
    smart_io_stream_tick_t* const tick = &smart_io_stream_tick_state;
    const int64_t end_us               = esp_timer_get_time();
    const uint32_t digital = smart_io_stream_kind_mask(SMART_IO_STREAM_KIND_DIGITAL);
    const uint32_t values  = smart_io_stream_kind_mask(SMART_IO_STREAM_KIND_VALUE);
    const int64_t idle_us =
        (int64_t)SMART_IO_STREAM_IDLE_FRAME_MS * SMART_IO_STREAM_US_PER_MS;
    const bool idle_due    = (end_us - tick->last_frame_us) >= idle_us;
    const bool restate_due = (end_us - tick->last_restate_us) >= idle_us;

    smart_io_stream_flood_check(end_us, digital);

    const uint32_t start_levels = tick->levels & digital;
    const size_t digital_count  = smart_io_stream_drain_digital(end_us, digital);
    const size_t value_count    = smart_io_stream_drain_values(end_us, values);
    const uint32_t series       = smart_io_stream_kind_mask(SMART_IO_STREAM_KIND_SERIES);
    smart_io_series_drained_t drained = {0};
    smart_io_frame_writer_t writer    = {0};

    /* Before encoding: a loss puts the GAPs ahead of the blocks. */
    if (smart_io_stream_adc_ring.buffer != NULL)
    {
        smart_io_series_drain(
            &smart_io_stream_series,
            &smart_io_stream_adc_ring,
            series,
            end_us,
            smart_io_stream_adc_samples,
            SMART_IO_STREAM_ADC_DEPTH,
            &drained);
        smart_io_stream_statistics.lost += drained.dropped;

        if (drained.gap)
        {
            tick->gaps |= series;
        }
    }

    smart_io_frame_begin(
        &writer,
        smart_io_stream_scratch,
        SMART_IO_STREAM_SCRATCH_SIZE,
        (uint64_t)tick->frame_start_us,
        (uint32_t)(end_us - tick->frame_start_us));
    smart_io_stream_encode(
        &writer,
        (tick->flood_until_us != 0) ? 0u : digital,
        values,
        digital_count,
        value_count,
        start_levels,
        restate_due);
    tick->failed |= smart_io_series_encode(
        &smart_io_stream_series,
        &writer,
        series,
        smart_io_stream_adc_samples,
        &drained,
        smart_io_stream_series_values);
    smart_io_stream_encode_bytes(
        &writer, smart_io_stream_kind_mask(SMART_IO_STREAM_KIND_BYTES), end_us);

    if (restate_due)
    {
        tick->last_restate_us = end_us;
    }

    if ((writer.block_count > 0u) || idle_due)
    {
        smart_io_stream_backlog_append(
            smart_io_stream_scratch, (uint32_t)smart_io_frame_end(&writer));
        smart_io_stream_statistics.frames++;
        tick->frame_start_us = end_us;
        tick->last_frame_us  = end_us;
        tick->gaps           = tick->failed;
        tick->unposted       = true;
    }
    else
    {
        tick->gaps |= tick->failed;
    }

    tick->failed = 0u;

    if (tick->unposted
        && ((end_us - tick->last_notify_us)
            >= ((int64_t)SMART_IO_STREAM_NOTIFY_MS * SMART_IO_STREAM_US_PER_MS)))
    {
        smart_io_stream_post(SMART_IO_EVENT_STREAM_FRAME, NULL, 0u);
        tick->last_notify_us = end_us;
        tick->unposted       = false;
    }

    const uint32_t tick_us = (uint32_t)(esp_timer_get_time() - end_us);

    smart_io_stream_statistics.tick_us_max =
        MAX(smart_io_stream_statistics.tick_us_max, tick_us);
}

/* Blocked while nothing streams. A stream that starts after a pause begins a
 * new window, which clients see as a time jump. */
static void smart_io_stream_task(void* arg)
{
    ARG_UNUSED(arg);

    TickType_t wake = xTaskGetTickCount();

    while (true)
    {
        if (__atomic_load_n(&smart_io_stream_channel_mask, __ATOMIC_ACQUIRE) == 0u)
        {
            if (smart_io_stream_tick_state.unposted)
            {
                smart_io_stream_tick_state.unposted = false;
                smart_io_stream_post(SMART_IO_EVENT_STREAM_FRAME, NULL, 0u);
            }

            ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

            xSemaphoreTake(smart_io_stream_lock, portMAX_DELAY);
            const int64_t now_us                       = esp_timer_get_time();
            smart_io_stream_tick_state.frame_start_us  = now_us;
            smart_io_stream_tick_state.last_frame_us   = now_us;
            smart_io_stream_tick_state.last_restate_us = now_us;
            smart_io_stream_tick_state.flood_until_us  = 0;
            smart_io_stream_tick_state.sent_digital    = 0u;
            smart_io_stream_tick_state.gaps            = 0u;
            __atomic_store_n(&smart_io_stream_edges, 0u, __ATOMIC_RELAXED);
            __atomic_store_n(&smart_io_stream_edges_flood, false, __ATOMIC_RELAXED);
            xSemaphoreGive(smart_io_stream_lock);

            wake = xTaskGetTickCount();
        }

        xTaskDelayUntil(&wake, pdMS_TO_TICKS(SMART_IO_STREAM_TICK_MS));

        xSemaphoreTake(smart_io_stream_lock, portMAX_DELAY);

        if ((smart_io_stream_channel_mask != 0u) && (smart_io_stream_scratch != NULL))
        {
            smart_io_stream_tick();
        }

        xSemaphoreGive(smart_io_stream_lock);
    }
}


/* Exposed API ------------------------------------------------------------- */

esp_err_t smart_io_stream_init(void)
{
    esp_err_t error_code = ESP_OK;

    do
    {
        smart_io_series_init(&smart_io_stream_series);

        smart_io_stream_lock  = xSemaphoreCreateMutex();
        smart_io_stream_mutex = xSemaphoreCreateMutex();

        if ((smart_io_stream_lock == NULL) || (smart_io_stream_mutex == NULL))
        {
            error_code = ESP_ERR_NO_MEM;
            break;
        }

        const BaseType_t created = xTaskCreatePinnedToCore(
            smart_io_stream_task,
            "sio_strm",
            SMART_IO_STREAM_TASK_STACK,
            NULL,
            SMART_IO_STREAM_TASK_PRIO,
            &smart_io_stream_task_handle,
            SMART_IO_STREAM_TASK_CORE);

        if (created != pdPASS)
        {
            error_code = ESP_ERR_NO_MEM;
        }
    } while (0);

    if (error_code != ESP_OK)
    {
        ESP_LOGE(smart_io_stream_tag, "Init failed: %s", esp_err_to_name(error_code));
    }

    return error_code;
}

esp_err_t smart_io_stream_attach(smart_io_stream_cursor_t* cursor)
{
    esp_err_t error_code = ESP_OK;

    if (smart_io_stream_client_count == 0u)
    {
        xSemaphoreTake(smart_io_stream_lock, portMAX_DELAY);
        error_code = smart_io_stream_buffers_alloc();
        xSemaphoreGive(smart_io_stream_lock);
    }

    if (error_code == ESP_OK)
    {
        xSemaphoreTake(smart_io_stream_mutex, portMAX_DELAY);
        cursor->position = smart_io_stream_backlog.first + smart_io_stream_backlog.count;
        xSemaphoreGive(smart_io_stream_mutex);
        smart_io_stream_client_count++;
    }

    return error_code;
}

void smart_io_stream_detach(void)
{
    if (smart_io_stream_client_count > 0u)
    {
        smart_io_stream_client_count--;

        if (smart_io_stream_client_count == 0u)
        {
            xSemaphoreTake(smart_io_stream_lock, portMAX_DELAY);
            smart_io_stream_ring_free(&smart_io_stream_digital_ring);
            smart_io_stream_ring_free(&smart_io_stream_value_ring);
            smart_io_stream_ring_free(&smart_io_stream_adc_ring);
            smart_io_stream_buffers_free();
            xSemaphoreGive(smart_io_stream_lock);
        }
    }
}

uint32_t smart_io_stream_clients(void)
{
    return smart_io_stream_client_count;
}

esp_err_t smart_io_stream_ring(smart_io_stream_kind_t kind, smart_io_ring_t** ring)
{
    esp_err_t error_code   = ESP_OK;
    smart_io_ring_t* found = NULL;
    uint32_t depth         = 0u;
    uint32_t entry_size    = 0u;

    switch (kind)
    {
        case SMART_IO_STREAM_KIND_DIGITAL:
        {
            found      = &smart_io_stream_digital_ring;
            depth      = SMART_IO_STREAM_DIGITAL_DEPTH;
            entry_size = sizeof(smart_io_stream_digital_sample_t);
        }
        break;
        case SMART_IO_STREAM_KIND_SERIES:
        {
            found      = &smart_io_stream_adc_ring;
            depth      = SMART_IO_STREAM_ADC_DEPTH;
            entry_size = sizeof(smart_io_stream_adc_sample_t);
        }
        break;
        case SMART_IO_STREAM_KIND_VALUE:
        {
            found      = &smart_io_stream_value_ring;
            depth      = SMART_IO_STREAM_VALUE_DEPTH;
            entry_size = sizeof(smart_io_stream_value_sample_t);
        }
        break;
        default:
        {
            /* BYTES and NONE: no ring. */
        }
        break;
    }

    if (found != NULL)
    {
        xSemaphoreTake(smart_io_stream_lock, portMAX_DELAY);

        if (found->buffer == NULL)
        {
            uint8_t* const buffer = heap_caps_malloc(
                depth * entry_size, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);

            if (buffer != NULL)
            {
                smart_io_ring_init(found, buffer, depth);
            }
        }

        if (found->buffer == NULL)
        {
            error_code = ESP_ERR_NO_MEM;
            found      = NULL;
        }

        xSemaphoreGive(smart_io_stream_lock);
    }

    *ring = found;

    return error_code;
}

esp_err_t smart_io_stream_start(
    uint8_t index,
    smart_io_stream_kind_t kind,
    const smart_io_stream_source_t* source)
{
    esp_err_t error_code = ESP_OK;

    if ((index >= SMART_IO_STREAM_CHANNELS)
        || ((kind == SMART_IO_STREAM_KIND_BYTES) && (source->drain == NULL))
        || ((kind == SMART_IO_STREAM_KIND_DIGITAL)
            && ((source->pause == NULL) || (source->resume == NULL))))
    {
        error_code = ESP_ERR_INVALID_ARG;
    }
    else
    {
        const uint32_t bit = (uint32_t)1u << index;

        xSemaphoreTake(smart_io_stream_lock, portMAX_DELAY);

        smart_io_stream_channels[index].since_us = source->since_us;
        smart_io_stream_channels[index].kind     = kind;
        smart_io_stream_channels[index].drain    = source->drain;
        smart_io_stream_channels[index].pause    = source->pause;
        smart_io_stream_channels[index].resume   = source->resume;
        smart_io_stream_channels[index].context  = source->context;
        smart_io_stream_tick_state.value_known &= ~bit;

        if (kind == SMART_IO_STREAM_KIND_SERIES)
        {
            smart_io_series_channel_t* const channel =
                &smart_io_stream_series.channels[index];

            channel->since_us      = source->since_us;
            channel->period_us     = source->period_us;
            channel->full_scale_mv = source->full_scale_mv;
            channel->phase         = 0u;
            channel->adc_channel   = source->adc_channel;
        }

        if (kind == SMART_IO_STREAM_KIND_DIGITAL)
        {
            smart_io_stream_tick_state.levels &= ~bit;

            if (source->initial != 0)
            {
                smart_io_stream_tick_state.levels |= bit;
            }
        }

        __atomic_or_fetch(&smart_io_stream_channel_mask, bit, __ATOMIC_RELEASE);

        xSemaphoreGive(smart_io_stream_lock);
        xTaskNotifyGive(smart_io_stream_task_handle);
    }

    return error_code;
}

void smart_io_stream_stop(uint8_t index)
{
    if (index < SMART_IO_STREAM_CHANNELS)
    {
        xSemaphoreTake(smart_io_stream_lock, portMAX_DELAY);
        __atomic_and_fetch(
            &smart_io_stream_channel_mask, ~((uint32_t)1u << index), __ATOMIC_RELEASE);
        xSemaphoreGive(smart_io_stream_lock);
    }
}

uint32_t smart_io_stream_mask(void)
{
    return __atomic_load_n(&smart_io_stream_channel_mask, __ATOMIC_ACQUIRE);
}

esp_err_t smart_io_stream_read(
    smart_io_stream_cursor_t* cursor,
    uint8_t* buffer,
    size_t size,
    size_t* length)
{
    size_t total     = 0u;
    uint32_t run_end = 0u;
    bool done        = false;

    xSemaphoreTake(smart_io_stream_mutex, portMAX_DELAY);

    const smart_io_stream_backlog_t* const backlog = &smart_io_stream_backlog;
    const uint64_t next                            = backlog->first + backlog->count;

    if ((backlog->buffer != NULL)
        && ((cursor->position < backlog->first) || (cursor->position > next)))
    {
        cursor->position = backlog->first;
        __atomic_fetch_add(&smart_io_stream_statistics.backlog_jumps, 1u, __ATOMIC_RELAXED);
    }

    while ((backlog->buffer != NULL) && (cursor->position < next) && !done)
    {
        const smart_io_stream_frame_entry_t* const entry =
            &backlog->frames[cursor->position & (SMART_IO_STREAM_FRAME_DEPTH - 1u)];

        if ((total > 0u) && (entry->offset != run_end))
        {
            done = true;
        }
        else if ((total + entry->length) > size)
        {
            /* Never fits: skip it rather than stall the client. */
            if (total == 0u)
            {
                cursor->position++;
                __atomic_fetch_add(
                    &smart_io_stream_statistics.backlog_jumps, 1u, __ATOMIC_RELAXED);
            }

            done = true;
        }
        else
        {
            memcpy(&buffer[total], &backlog->buffer[entry->offset], entry->length);
            total += entry->length;
            run_end = entry->offset + entry->length;
            cursor->position++;
        }
    }

    xSemaphoreGive(smart_io_stream_mutex);

    *length = total;

    return ESP_OK;
}

void smart_io_stream_stats(smart_io_stream_stats_t* stats)
{
    xSemaphoreTake(smart_io_stream_lock, portMAX_DELAY);
    *stats                   = smart_io_stream_statistics;
    stats->clients           = smart_io_stream_client_count;
    stats->adc_conversion_ns = (uint32_t)smart_io_stream_series.conversion_ns;
    stats->adc_invalid = __atomic_load_n(&smart_io_stream_series.invalid, __ATOMIC_RELAXED);
    stats->adc_clock_jumps                        = smart_io_stream_series.clock_jumps;
    smart_io_stream_statistics.edges_per_tick_max = 0u;
    smart_io_stream_statistics.tick_us_max        = 0u;
    xSemaphoreGive(smart_io_stream_lock);
}

bool IRAM_ATTR smart_io_stream_edge_from_isr(void)
{
    const uint32_t edges = __atomic_add_fetch(&smart_io_stream_edges, 1u, __ATOMIC_RELAXED);
    const bool over = (edges > SMART_IO_STREAM_EDGE_BUDGET);

    if (over)
    {
        __atomic_store_n(&smart_io_stream_edges_flood, true, __ATOMIC_RELAXED);
    }

    return over;
}


/* ---- ADC1 continuous ---- */

void smart_io_stream_adc_timing(uint32_t conversion_us, uint32_t pattern_length)
{
    xSemaphoreTake(smart_io_stream_lock, portMAX_DELAY);
    smart_io_series_timing(
        &smart_io_stream_series,
        __atomic_load_n(&smart_io_stream_adc_ring.head, __ATOMIC_ACQUIRE),
        conversion_us,
        pattern_length);
    xSemaphoreGive(smart_io_stream_lock);
}

void IRAM_ATTR smart_io_stream_adc_frame_from_isr(uint32_t head, uint32_t invalid)
{
    smart_io_series_sync_from_isr(
        &smart_io_stream_series, head, invalid, esp_timer_get_time());
}

bool smart_io_stream_adc_latest(uint8_t channel, int32_t* raw)
{
    return smart_io_series_latest(&smart_io_stream_series, channel, raw);
}

/**** END OF FILE ****/
