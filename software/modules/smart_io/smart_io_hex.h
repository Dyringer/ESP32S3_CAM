/**
 ******************************************************************************
 * @file    smart_io_hex.h
 * @author  Daniel Dyringer
 * @date    2026-11-04
 * @brief   Hex text <-> bytes for bus transfers (JSON and shell).
 ******************************************************************************
 */

/* Define to prevent recursive inclusion ----------------------------------- */
#ifndef SMART_IO_HEX_H_
#define SMART_IO_HEX_H_

/* Includes ---------------------------------------------------------------- */
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"


/* Constants and macros ---------------------------------------------------- */
// clang-format off


// clang-format on

/* Types ------------------------------------------------------------------- */


/* Exposed API ------------------------------------------------------------- */

/* Two digits per byte, either case, nothing else. ESP_ERR_INVALID_ARG: odd
 * length or not a hex digit; ESP_ERR_INVALID_SIZE: more than capacity bytes. */
esp_err_t smart_io_hex_decode(const char* text, uint8_t* bytes, size_t capacity, size_t* length);

/* Upper case, NUL-terminated. ESP_ERR_INVALID_SIZE: text needs 2 * length + 1. */
esp_err_t smart_io_hex_encode(const uint8_t* bytes, size_t length, char* text, size_t size);

#endif /* SMART_IO_HEX_H_ */

/**** END OF FILE ****/
