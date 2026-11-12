/**
 ******************************************************************************
 * @file    smart_io_stream.h
 * @author  Daniel Dyringer
 * @date    2026-10-31
 * @brief   smart_io stream frames for the transports: reading the backlog
 *          from a client's cursor, and the stream counters.
 ******************************************************************************
 */

/* Define to prevent recursive inclusion ----------------------------------- */
#ifndef SMART_IO_STREAM_H_
#define SMART_IO_STREAM_H_

/* Includes ---------------------------------------------------------------- */
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"


/* Constants and macros ---------------------------------------------------- */
// clang-format off


// clang-format on

/* Types ------------------------------------------------------------------- */

/* Owned by one transport client. position is the sequence number of the next
 * frame to read; a client behind by more than the backlog jumps to the oldest
 * frame. */
typedef struct smart_io_stream_cursor_s
{
    uint64_t position;
} smart_io_stream_cursor_t;

typedef struct smart_io_stream_stats_s
{
    uint32_t clients;
    uint32_t edges_per_tick_max;
    uint32_t lost;
    uint32_t gaps;
    uint32_t frames;
    uint32_t tick_us_max;
    uint32_t backlog_jumps;
    uint32_t adc_conversion_ns; /* measured ADC1 conversion period */
    uint32_t adc_invalid;       /* results the DMA marked invalid */
    uint32_t adc_clock_jumps;   /* sample clock reset to the ISR time */
} smart_io_stream_stats_t;

/* Exposed API ------------------------------------------------------------- */

/* Copies whole frames from the cursor; length 0 when nothing new. Any task. */
esp_err_t smart_io_stream_read(
    smart_io_stream_cursor_t* cursor,
    uint8_t* buffer,
    size_t size,
    size_t* length);

/* Clears the maxima. */
void smart_io_stream_stats(smart_io_stream_stats_t* stats);

#endif /* SMART_IO_STREAM_H_ */

/**** END OF FILE ****/
