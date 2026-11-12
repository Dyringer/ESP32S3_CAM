/**
 ******************************************************************************
 * @file    smart_io_frame.c
 * @author  Daniel Dyringer
 * @date    2026-10-31
 * @brief   Binary stream frame encoder.
 ******************************************************************************
 */


/* Includes ---------------------------------------------------------------- */
#include "smart_io_frame.h"

#include <string.h>


/* Constants and macros ---------------------------------------------------- */
// clang-format off

#define SMART_IO_FRAME_KIND_SHIFT       5u
#define SMART_IO_FRAME_INDEX_MASK       0x1Fu
#define SMART_IO_FRAME_VARINT_BITS      7u
#define SMART_IO_FRAME_VARINT_MASK      0x7Fu
#define SMART_IO_FRAME_VARINT_MORE      0x80u
#define SMART_IO_FRAME_START_BYTES      8u
#define SMART_IO_FRAME_BITS_PER_BYTE    8u

// clang-format on

/* Private types ----------------------------------------------------------- */

typedef enum smart_io_frame_kind_e
{
    SMART_IO_FRAME_KIND_DIGITAL = 0,
    SMART_IO_FRAME_KIND_SERIES  = 1,
    SMART_IO_FRAME_KIND_VALUE   = 2,
    SMART_IO_FRAME_KIND_BYTES   = 3,
    SMART_IO_FRAME_KIND_GAP     = 4,
} smart_io_frame_kind_t;


/* Private variables ------------------------------------------------------- */


/* Private functions ------------------------------------------------------- */

/* ---- Primitives: set overflow instead of writing past the end ---- */

static void smart_io_frame_put_u8(smart_io_frame_writer_t* writer, uint8_t value)
{
    if (writer->length < writer->size)
    {
        writer->buffer[writer->length] = value;
        writer->length++;
    }
    else
    {
        writer->overflow = true;
    }
}

static void smart_io_frame_put_varint(smart_io_frame_writer_t* writer, uint32_t value)
{
    uint32_t remaining = value;

    while (remaining > SMART_IO_FRAME_VARINT_MASK)
    {
        smart_io_frame_put_u8(
            writer,
            (uint8_t)((remaining & SMART_IO_FRAME_VARINT_MASK)
                      | SMART_IO_FRAME_VARINT_MORE));
        remaining >>= SMART_IO_FRAME_VARINT_BITS;
    }

    smart_io_frame_put_u8(writer, (uint8_t)remaining);
}

static void smart_io_frame_put_zigzag(smart_io_frame_writer_t* writer, int32_t value)
{
    const uint32_t magnitude = (uint32_t)value;
    const uint32_t encoded = (value < 0) ? (((~magnitude) << 1u) | 1u) : (magnitude << 1u);

    smart_io_frame_put_varint(writer, encoded);
}

/* Wrapping difference, so the decoder's (previous + delta) | 0 gets value back. */
static int32_t smart_io_frame_delta(int32_t value, int32_t previous)
{
    return (int32_t)((uint32_t)value - (uint32_t)previous);
}

/* block_count is one byte in the frame: a block past the limit doesn't fit. */
static void smart_io_frame_block_begin(
    smart_io_frame_writer_t* writer,
    smart_io_frame_kind_t kind,
    uint8_t index)
{
    if (writer->block_count == SMART_IO_FRAME_BLOCKS_MAX)
    {
        writer->overflow = true;
    }

    smart_io_frame_put_u8(
        writer,
        (uint8_t)(((uint32_t)kind << SMART_IO_FRAME_KIND_SHIFT)
                  | ((uint32_t)index & SMART_IO_FRAME_INDEX_MASK)));
}

/* Commits the block, or rolls the frame back to length when it didn't fit. */
static esp_err_t smart_io_frame_block_end(smart_io_frame_writer_t* writer, size_t length)
{
    esp_err_t error_code = ESP_OK;

    if (writer->overflow)
    {
        writer->length   = length;
        writer->overflow = false;
        error_code       = ESP_ERR_NO_MEM;
    }
    else
    {
        writer->block_count++;
        writer->buffer[writer->block_count_offset] = writer->block_count;
    }

    return error_code;
}


/* Exposed API ------------------------------------------------------------- */

esp_err_t smart_io_frame_begin(
    smart_io_frame_writer_t* writer,
    uint8_t* buffer,
    size_t size,
    uint64_t start_us,
    uint32_t duration_us)
{
    esp_err_t error_code = ESP_OK;

    writer->buffer             = buffer;
    writer->size               = size;
    writer->length             = 0u;
    writer->block_count_offset = 0u;
    writer->start_us           = start_us;
    writer->block_count        = 0u;
    writer->overflow           = false;

    for (uint32_t i = 0u; i < SMART_IO_FRAME_START_BYTES; i++)
    {
        smart_io_frame_put_u8(
            writer, (uint8_t)(start_us >> (i * SMART_IO_FRAME_BITS_PER_BYTE)));
    }

    smart_io_frame_put_varint(writer, duration_us);
    writer->block_count_offset = writer->length;
    smart_io_frame_put_u8(writer, 0u);

    if (writer->overflow)
    {
        error_code = ESP_ERR_NO_MEM;
    }

    return error_code;
}

esp_err_t smart_io_frame_digital(
    smart_io_frame_writer_t* writer,
    uint32_t pins,
    uint32_t levels,
    const smart_io_stream_digital_sample_t* samples,
    size_t count)
{
    const size_t length     = writer->length;
    uint32_t previous_time  = (uint32_t)writer->start_us;
    uint32_t previous_level = levels;

    smart_io_frame_block_begin(writer, SMART_IO_FRAME_KIND_DIGITAL, 0u);
    smart_io_frame_put_varint(writer, pins);
    smart_io_frame_put_varint(writer, levels);
    smart_io_frame_put_varint(writer, (uint32_t)count);

    for (size_t i = 0u; (i < count) && !writer->overflow; i++)
    {
        smart_io_frame_put_varint(writer, samples[i].time_us - previous_time);
        smart_io_frame_put_varint(writer, samples[i].levels ^ previous_level);
        previous_time  = samples[i].time_us;
        previous_level = samples[i].levels;
    }

    return smart_io_frame_block_end(writer, length);
}

esp_err_t smart_io_frame_series(
    smart_io_frame_writer_t* writer,
    uint8_t index,
    uint64_t first_us,
    uint32_t period_us,
    const int32_t* values,
    size_t count)
{
    const size_t length = writer->length;
    int32_t previous    = 0;

    smart_io_frame_block_begin(writer, SMART_IO_FRAME_KIND_SERIES, index);
    /* Negative when the conversions were reported after the window closed. */
    smart_io_frame_put_zigzag(writer, (int32_t)(int64_t)(first_us - writer->start_us));
    smart_io_frame_put_varint(writer, period_us);
    smart_io_frame_put_varint(writer, (uint32_t)count);

    for (size_t i = 0u; (i < count) && !writer->overflow; i++)
    {
        smart_io_frame_put_zigzag(writer, smart_io_frame_delta(values[i], previous));
        previous = values[i];
    }

    return smart_io_frame_block_end(writer, length);
}

esp_err_t smart_io_frame_value(
    smart_io_frame_writer_t* writer,
    uint8_t index,
    const smart_io_stream_value_sample_t* samples,
    size_t count)
{
    const size_t length    = writer->length;
    uint32_t previous_time = (uint32_t)writer->start_us;
    int32_t previous_value = 0;

    smart_io_frame_block_begin(writer, SMART_IO_FRAME_KIND_VALUE, index);
    smart_io_frame_put_varint(writer, (uint32_t)count);

    for (size_t i = 0u; (i < count) && !writer->overflow; i++)
    {
        smart_io_frame_put_zigzag(writer, (int32_t)(samples[i].time_us - previous_time));
        smart_io_frame_put_zigzag(
            writer, smart_io_frame_delta(samples[i].value, previous_value));
        previous_time  = samples[i].time_us;
        previous_value = samples[i].value;
    }

    return smart_io_frame_block_end(writer, length);
}

esp_err_t smart_io_frame_bytes(
    smart_io_frame_writer_t* writer,
    uint8_t index,
    uint64_t time_us,
    const uint8_t* data,
    size_t count)
{
    const size_t length = writer->length;

    smart_io_frame_block_begin(writer, SMART_IO_FRAME_KIND_BYTES, index);
    smart_io_frame_put_varint(writer, (uint32_t)(time_us - writer->start_us));
    smart_io_frame_put_varint(writer, (uint32_t)count);

    if ((writer->length + count) <= writer->size)
    {
        memcpy(&writer->buffer[writer->length], data, count);
        writer->length += count;
    }
    else
    {
        writer->overflow = true;
    }

    return smart_io_frame_block_end(writer, length);
}

esp_err_t smart_io_frame_gap(smart_io_frame_writer_t* writer, uint8_t index)
{
    const size_t length = writer->length;

    smart_io_frame_block_begin(writer, SMART_IO_FRAME_KIND_GAP, index);

    return smart_io_frame_block_end(writer, length);
}

size_t smart_io_frame_end(smart_io_frame_writer_t* writer)
{
    return writer->length;
}


/**** END OF FILE ****/
