/**
 ******************************************************************************
 * @file    smart_io_series.h
 * @author  Daniel Dyringer
 * @date    2026-11-06
 * @brief   SERIES streams from ADC1 continuous: the sample clock (times
 *          from the ISR syncs), decimation per pin and encoding. Plain C,
 *          state passed in, so it runs on the host.
 *
 *          Called by sio_strm with the stream lock held, except
 *          sync_from_isr (ADC ISR) and latest (any task, lock-free).
 ******************************************************************************
 */

/* Define to prevent recursive inclusion ----------------------------------- */
#ifndef SMART_IO_SERIES_H_
#define SMART_IO_SERIES_H_

/* Includes ---------------------------------------------------------------- */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "smart_io_frame.h"
#include "smart_io_ring.h"
#include "smart_io_stream_source.h"


/* Constants and macros ---------------------------------------------------- */
// clang-format off

#define SMART_IO_SERIES_PINS            32u     /* pool index, as the stream channels */
#define SMART_IO_SERIES_ADC_CHANNELS    16u     /* 4-bit channel in a sample */

// clang-format on

/* Types ------------------------------------------------------------------- */

/* One streamed ADC1 pin. since_us: samples older than this are not its own.
 * phase: conversions of this channel until the next kept one. */
typedef struct smart_io_series_channel_s
{
    int64_t since_us;
    uint32_t period_us;
    uint32_t full_scale_mv;
    uint32_t phase;
    uint8_t adc_channel;
} smart_io_series_channel_t;

/* Written by the ADC ISR after its pushes: sample head - 1 was converted at
 * time_us. Seqlock: sequence is odd while the ISR writes. */
typedef struct smart_io_series_sync_s
{
    volatile uint32_t sequence;
    volatile uint32_t head;
    volatile int64_t time_us; /* no 64-bit atomics: they are libcalls, not IRAM */
} smart_io_series_sync_t;

/* Must be in internal RAM: the ISR writes sync during flash writes.
 *
 * The S3 converts slower than requested (about 4.7 kHz for 5 kHz), so sample
 * times use the period measured from the syncs since the driver started.
 * next_ns: time of the sample at the ring tail, carried from tick to tick so
 * no sample falls between two windows; only nudged towards the syncs. */
typedef struct smart_io_series_s
{
    smart_io_series_sync_t sync;
    smart_io_series_channel_t channels[SMART_IO_SERIES_PINS];
    /* Latest raw per ADC1 channel, -1 before the first. */
    int32_t latest_raw[SMART_IO_SERIES_ADC_CHANNELS];
    uint32_t conversion_us; /* requested; 0: not running */
    uint32_t pattern_length;
    int64_t conversion_ns; /* measured */
    uint32_t measure_head;
    int64_t measure_time_us;
    int64_t next_ns;
    uint32_t restart_head;
    uint32_t invalid;     /* results the DMA marked invalid (ISR) */
    uint32_t clock_jumps; /* sample clock reset to the ISR time */
    bool measuring;
    bool anchored;
    bool restarted;
} smart_io_series_t;

/* What one drain took from the ring. */
typedef struct smart_io_series_drained_s
{
    size_t count;
    int64_t first_us; /* time of samples[0] */
    uint32_t dropped; /* lost in the ring since the last drain */
    bool gap;         /* the streams lost samples: GAP for every SERIES pin */
} smart_io_series_drained_t;

/* Exposed API ------------------------------------------------------------- */

void smart_io_series_init(smart_io_series_t* series);

/* On every driver start, with the driver stopped. head: the ring head now. */
void smart_io_series_timing(
    smart_io_series_t* series,
    uint32_t head,
    uint32_t conversion_us,
    uint32_t pattern_length);

/* ADC ISR, after pushing a conversion frame: head = ring head. */
void smart_io_series_sync_from_isr(
    smart_io_series_t* series,
    uint32_t head,
    uint32_t invalid,
    int64_t time_us);

bool smart_io_series_latest(const smart_io_series_t* series, uint8_t channel, int32_t* raw);

/* Samples older than end_us, in conversion order, into samples (capacity).
 * pins: the streamed SERIES pins (pool mask). */
void smart_io_series_drain(
    smart_io_series_t* series,
    smart_io_ring_t* ring,
    uint32_t pins,
    int64_t end_us,
    smart_io_stream_adc_sample_t* samples,
    size_t capacity,
    smart_io_series_drained_t* drained);

/* SERIES blocks for every pin in pins; values: scratch of drained->count.
 * Returns the pins whose block didn't fit the frame. */
uint32_t smart_io_series_encode(
    smart_io_series_t* series,
    smart_io_frame_writer_t* writer,
    uint32_t pins,
    const smart_io_stream_adc_sample_t* samples,
    const smart_io_series_drained_t* drained,
    int32_t* values);

#endif /* SMART_IO_SERIES_H_ */

/**** END OF FILE ****/
