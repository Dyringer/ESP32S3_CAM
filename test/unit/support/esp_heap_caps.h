/**
 ******************************************************************************
 * @file    esp_heap_caps.h
 * @author  Daniel Dyringer
 * @date    2026-10-26
 * @brief   Host stand-in for the ESP-IDF esp_heap_caps.h used by unit tests.
 *          Not implemented by fake_idf: tests that need them define them.
 ******************************************************************************
 */

/* Define to prevent recursive inclusion ----------------------------------- */
#ifndef ESP_HEAP_CAPS_H_
#define ESP_HEAP_CAPS_H_

/* Includes ---------------------------------------------------------------- */
#include <stddef.h>
#include <stdint.h>

/* Constants and macros ---------------------------------------------------- */
// clang-format off

/* Same values as ESP-IDF. */
#define MALLOC_CAP_INTERNAL     (1u << 11u)
#define MALLOC_CAP_SPIRAM       (1u << 10u)

// clang-format on

/* Types ------------------------------------------------------------------- */


/* Exposed API ------------------------------------------------------------- */

void* heap_caps_malloc(size_t size, uint32_t caps);
void* heap_caps_calloc(size_t count, size_t size, uint32_t caps);
void heap_caps_free(void* pointer);

#endif /* ESP_HEAP_CAPS_H_ */

/**** END OF FILE ****/
