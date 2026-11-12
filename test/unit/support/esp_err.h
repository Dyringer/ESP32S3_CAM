/**
 ******************************************************************************
 * @file    esp_err.h
 * @author  Daniel Dyringer
 * @date    2026-10-24
 * @brief   Host stand-in for the ESP-IDF esp_err.h used by unit tests.
 ******************************************************************************
 */

/* Define to prevent recursive inclusion ----------------------------------- */
#ifndef ESP_ERR_H_
#define ESP_ERR_H_

/* Includes ---------------------------------------------------------------- */
#include <stdint.h>

/* Constants and macros ---------------------------------------------------- */
// clang-format off

/* Same values as ESP-IDF. */
#define ESP_OK                  0
#define ESP_FAIL                (-1)
#define ESP_ERR_NO_MEM          0x101
#define ESP_ERR_INVALID_ARG     0x102
#define ESP_ERR_INVALID_STATE   0x103
#define ESP_ERR_INVALID_SIZE    0x104
#define ESP_ERR_NOT_FOUND       0x105
#define ESP_ERR_NOT_SUPPORTED   0x106
#define ESP_ERR_TIMEOUT         0x107
#define ESP_ERR_INVALID_VERSION 0x10A

// clang-format on

/* Types ------------------------------------------------------------------- */

typedef int32_t esp_err_t;

/* Exposed API ------------------------------------------------------------- */

const char* esp_err_to_name(esp_err_t code);

#endif /* ESP_ERR_H_ */

/**** END OF FILE ****/
