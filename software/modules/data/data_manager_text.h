/**
 ******************************************************************************
 * @file    data_manager_text.h
 * @author  Daniel Dyringer
 * @date    2026-10-13
 * @brief   Text conversion of data manager values, shared by the shell and
 *          the web panel.
 *
 *          Booleans accept 1/0, true/false, on/off. Integers accept 0x hex.
 *          Blobs are hex strings of the full size. Strings are raw text.
 ******************************************************************************
 */

/* Define to prevent recursive inclusion ----------------------------------- */
#ifndef DATA_MANAGER_TEXT_H_
#define DATA_MANAGER_TEXT_H_

/* Includes ---------------------------------------------------------------- */
#include <stddef.h>
#include <stdint.h>

#include "data_manager.h"
#include "esp_err.h"


/* Constants and macros ---------------------------------------------------- */
// clang-format off

/* Fits any number type, terminator included. */
#define DATA_MANAGER_TEXT_SCALAR_MAX    24u

// clang-format on

/* Types ------------------------------------------------------------------- */


/* Exposed API ------------------------------------------------------------- */

const char* data_manager_text_type_name(data_manager_type_t type);

/* Text buffer size that holds the full formatted value. */
size_t data_manager_text_size(const data_manager_descriptor_t* descriptor);

/* buffer holds descriptor->size bytes; size receives the length to pass to
 * data_manager_set(). */
esp_err_t data_manager_text_parse(
    const data_manager_descriptor_t* descriptor,
    const char* text,
    uint8_t* buffer,
    size_t* size);

/* Returns ESP_ERR_INVALID_SIZE when truncated; blobs are cut on a whole byte. */
esp_err_t data_manager_text_format(
    const data_manager_descriptor_t* descriptor,
    const uint8_t* buffer,
    char* text,
    size_t text_size);

#endif /* DATA_MANAGER_TEXT_H_ */

/**** END OF FILE ****/
