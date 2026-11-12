/**
 ******************************************************************************
 * @file    smart_io_stream_source.h
 * @author  Daniel Dyringer
 * @date    2026-10-31
 * @brief   smart_io data path, producer and control side: stream channels,
 *          rings, the sio_strm task. Private to the smart_io component.
 *
 *          Lock order: smart_io mutex -> stream lock -> backlog mutex.
 *          start/stop/attach/detach are called by smart_io with its
 *          mutex held.
 ******************************************************************************
 */

/* Define to prevent recursive inclusion ----------------------------------- */
#ifndef SMART_IO_STREAM_SOURCE_H_
#define SMART_IO_STREAM_SOURCE_H_

/* Includes ---------------------------------------------------------------- */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "smart_io_ring.h"
#include "smart_io_stream.h"
#include "smart_io_types.h"


/* Constants and macros ---------------------------------------------------- */
// clang-format off


// clang-format on

/* Types ------------------------------------------------------------------- */

typedef struct smart_io_stream_digital_sample_s
{
    uint32_t time_us;
    uint32_t levels; /* bit n = pool pin n */
} smart_io_stream_digital_sample_t;

/* channel << 12 | raw */
typedef uint16_t smart_io_stream_adc_sample_t;

typedef struct smart_io_stream_value_sample_s
{
    uint32_t time_us;
    int32_t value;
    uint8_t index;
} smart_io_stream_value_sample_t;

/* Called by sio_strm with the stream lock held; never block.
 * drain (BYTES): once per tick, returns the bytes copied; overflow: bytes were
 * lost since the last call.
 * pause / resume (DIGITAL): edge flood. pause stops the pin's interrupt;
 * resume re-arms it and returns the pin level (0/1). */
typedef size_t (*smart_io_stream_drain_fn_t)(
    void* context,
    uint8_t* buffer,
    size_t capacity,
    bool* overflow);
typedef void (*smart_io_stream_pause_fn_t)(void* context);
typedef int32_t (*smart_io_stream_resume_fn_t)(void* context);

/* since_us: samples older than this are ignored for the pin; set by smart_io,
 * a role may move it later. initial: DIGITAL level at since_us. period_us:
 * SERIES sample period the pin asks for. context: passed to the hooks. */
typedef struct smart_io_stream_source_s
{
    int64_t since_us;
    smart_io_stream_drain_fn_t drain;
    smart_io_stream_pause_fn_t pause;
    smart_io_stream_resume_fn_t resume;
    void* context;
    uint32_t period_us;
    int32_t initial;
    uint32_t full_scale_mv; /* SERIES: mV at raw 4095 */
    uint8_t adc_channel;
} smart_io_stream_source_t;

/* Exposed API ------------------------------------------------------------- */

esp_err_t smart_io_stream_init(void);

/* First attach allocates the backlog and scratch buffer, the last detach frees
 * them; smart_io stops every stream before the last detach. */
esp_err_t smart_io_stream_attach(smart_io_stream_cursor_t* cursor);
void smart_io_stream_detach(void);
uint32_t smart_io_stream_clients(void);

/* Allocates the kind's ring on first use, kept until the last detach. The
 * role producer pushes into it. BYTES has no ring (*ring = NULL): the stream
 * task drains the role. */
esp_err_t smart_io_stream_ring(smart_io_stream_kind_t kind, smart_io_ring_t** ring);

/* DIGITAL needs pause and resume, BYTES needs drain. */
esp_err_t smart_io_stream_start(
    uint8_t index,
    smart_io_stream_kind_t kind,
    const smart_io_stream_source_t* source);
/* Waits for the current tick to end. */
void smart_io_stream_stop(uint8_t index);
uint32_t smart_io_stream_mask(void);

/* ADC1 continuous, from the adc role. timing: on every driver start, with the
 * driver stopped; the streams get a GAP. frame_from_isr: after the ISR pushed
 * a conversion frame, head = ring head. latest: last raw value converted on
 * an ADC1 channel, false before the first. */
void smart_io_stream_adc_timing(uint32_t conversion_us, uint32_t pattern_length);
void smart_io_stream_adc_frame_from_isr(uint32_t head, uint32_t invalid);
bool smart_io_stream_adc_latest(uint8_t channel, int32_t* raw);

/* Streamed GPIO ISR, once per edge. True when the edge budget of this tick is
 * spent: the caller disables its pin interrupt, sio_strm pauses every
 * streamed input and resumes them after the flood pause. */
bool smart_io_stream_edge_from_isr(void);

#endif /* SMART_IO_STREAM_SOURCE_H_ */

/**** END OF FILE ****/
