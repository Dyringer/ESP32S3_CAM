/**
 ******************************************************************************
 * @file    smart_io_json.h
 * @author  Daniel Dyringer
 * @date    2026-10-31
 * @brief   smart_io control protocol (SMART_IO2.md §8.1): JSON request ->
 *          smart_io API -> response, and update messages. Stateless: the
 *          transport keeps the per-client dirty masks.
 ******************************************************************************
 */

/* Define to prevent recursive inclusion ----------------------------------- */
#ifndef SMART_IO_JSON_H_
#define SMART_IO_JSON_H_

/* Includes ---------------------------------------------------------------- */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"


/* Constants and macros ---------------------------------------------------- */
// clang-format off

#define SMART_IO_JSON_REQUEST_MAX       2048u
/* hello with every pin configured is the largest message. */
#define SMART_IO_JSON_RESPONSE_SIZE     8192u


// clang-format on

/* Types ------------------------------------------------------------------- */

typedef struct smart_io_json_update_s
{
    uint32_t value_mask;  /* pool pins whose value goes out */
    uint32_t config_mask; /* pool pins whose configuration goes out */
    uint32_t flood_ms;
    bool streams;
    bool unsaved;
} smart_io_json_update_t;


/* Exposed API ------------------------------------------------------------- */

/* Calls the smart_io API, so it may block: sio_ws task only. Always writes a
 * response (an error response for a malformed request); ESP_ERR_INVALID_SIZE
 * when it doesn't fit the buffer. */
esp_err_t smart_io_json_handle(
    const char* request,
    size_t request_length,
    char* response,
    size_t size,
    size_t* length);

/* Error response carrying the request's id, without running it ("busy").
 * Doesn't touch smart_io, so any task may call it. */
esp_err_t smart_io_json_error(
    const char* request,
    size_t request_length,
    esp_err_t error,
    const char* reason,
    char* response,
    size_t size,
    size_t* length);

/* Built from the current state, not from the events. */
esp_err_t smart_io_json_update(
    const smart_io_json_update_t* update,
    char* buffer,
    size_t size,
    size_t* length);

#endif /* SMART_IO_JSON_H_ */

/**** END OF FILE ****/
