/**
 ******************************************************************************
 * @file    fake_nvs.h
 * @author  Daniel Dyringer
 * @date    2026-10-28
 * @brief   Control side of the host NVS fake (RAM key-value store with blobs
 *          and u8 values). Every test executable
 *          that links smart_io.c must include this header, because smart_io
 *          init reads NVS.
 ******************************************************************************
 */

/* Define to prevent recursive inclusion ----------------------------------- */
#ifndef FAKE_NVS_H_
#define FAKE_NVS_H_

/* Includes ---------------------------------------------------------------- */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

/* Constants and macros ---------------------------------------------------- */
// clang-format off


// clang-format on

/* Types ------------------------------------------------------------------- */


/* Exposed API ------------------------------------------------------------- */

/* Empties the store and clears failure injection and counters. */
void fake_nvs_reset(void);

/* ---- Content ---- */

void fake_nvs_blob_put(const char* name, const char* key, const void* value, size_t length);
/* Copies up to capacity bytes; returns the stored length, 0 when missing. */
size_t fake_nvs_blob_get(const char* name, const char* key, void* value, size_t capacity);
void fake_nvs_u8_put(const char* name, const char* key, uint8_t value);
/* False when the key is missing. */
bool fake_nvs_u8_get(const char* name, const char* key, uint8_t* value);

/* ---- Failure injection and counters ---- */

/* nvs_set_* and nvs_commit fail with code until fake_nvs_reset(); ESP_OK
 * turns injection off. */
void fake_nvs_fail_writes(esp_err_t code);
uint32_t fake_nvs_write_count(void);


#endif /* FAKE_NVS_H_ */

/**** END OF FILE ****/
