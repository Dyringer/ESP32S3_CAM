/**
 ******************************************************************************
 * @file    smart_io_frame.h
 * @author  Daniel Dyringer
 * @date    2026-10-31
 * @brief   Binary stream frame encoder (SMART_IO2.md §8.2): varint, zigzag,
 *          deltas. Takes the ring samples as drained; the encoder makes the
 *          times relative and writes the deltas. No state outside the writer.
 ******************************************************************************
 */

/* Define to prevent recursive inclusion ----------------------------------- */
#ifndef SMART_IO_FRAME_H_
#define SMART_IO_FRAME_H_

/* Includes ---------------------------------------------------------------- */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "smart_io_stream_source.h"


/* Constants and macros ---------------------------------------------------- */
// clang-format off

/* Reported as "protocol" in hello. Bump on any frame format change. */
#define SMART_IO_FRAME_VERSION          1u
#define SMART_IO_FRAME_BLOCKS_MAX       UINT8_MAX

// clang-format on

/* Types ------------------------------------------------------------------- */

typedef struct smart_io_frame_writer_s
{
    uint8_t* buffer;
    size_t size;
    size_t length;
    size_t block_count_offset;
    uint64_t start_us;
    uint8_t block_count;
    bool overflow;
} smart_io_frame_writer_t;

/* Exposed API ------------------------------------------------------------- */

/* Every block function returns ESP_ERR_NO_MEM and leaves the frame unchanged
 * when the block doesn't fit (buffer full or SMART_IO_FRAME_BLOCKS_MAX
 * reached). */
esp_err_t smart_io_frame_begin(
    smart_io_frame_writer_t* writer,
    uint8_t* buffer,
    size_t size,
    uint64_t start_us,
    uint32_t duration_us);
esp_err_t smart_io_frame_digital(
    smart_io_frame_writer_t* writer,
    uint32_t pins,
    uint32_t levels,
    const smart_io_stream_digital_sample_t* samples,
    size_t count);
esp_err_t smart_io_frame_series(
    smart_io_frame_writer_t* writer,
    uint8_t index,
    uint64_t first_us,
    uint32_t period_us,
    const int32_t* values,
    size_t count);
esp_err_t smart_io_frame_value(
    smart_io_frame_writer_t* writer,
    uint8_t index,
    const smart_io_stream_value_sample_t* samples,
    size_t count);
esp_err_t smart_io_frame_bytes(
    smart_io_frame_writer_t* writer,
    uint8_t index,
    uint64_t time_us,
    const uint8_t* data,
    size_t count);
esp_err_t smart_io_frame_gap(smart_io_frame_writer_t* writer, uint8_t index);
size_t smart_io_frame_end(smart_io_frame_writer_t* writer);

#endif /* SMART_IO_FRAME_H_ */

/**** END OF FILE ****/
