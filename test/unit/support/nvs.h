/**
 ******************************************************************************
 * @file    nvs.h
 * @author  Daniel Dyringer
 * @date    2026-10-28
 * @brief   Host stand-in for the ESP-IDF nvs.h; implemented by fake_nvs.c.
 ******************************************************************************
 */

/* Define to prevent recursive inclusion ----------------------------------- */
#ifndef NVS_H_
#define NVS_H_

/* Includes ---------------------------------------------------------------- */
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

/* Constants and macros ---------------------------------------------------- */
// clang-format off

/* Same values as ESP-IDF. */
#define ESP_ERR_NVS_NOT_FOUND       0x1102
#define ESP_ERR_NVS_INVALID_LENGTH  0x110C

// clang-format on

/* Types ------------------------------------------------------------------- */

typedef uint32_t nvs_handle_t;

typedef enum nvs_open_mode_e
{
    NVS_READONLY  = 0,
    NVS_READWRITE = 1,
} nvs_open_mode_t;

/* Exposed API ------------------------------------------------------------- */

esp_err_t nvs_open(const char* name, nvs_open_mode_t open_mode, nvs_handle_t* out_handle);
void nvs_close(nvs_handle_t handle);
esp_err_t nvs_commit(nvs_handle_t handle);
esp_err_t nvs_get_blob(nvs_handle_t handle, const char* key, void* out_value, size_t* length);
esp_err_t nvs_set_blob(nvs_handle_t handle, const char* key, const void* value, size_t length);
esp_err_t nvs_get_u8(nvs_handle_t handle, const char* key, uint8_t* out_value);
esp_err_t nvs_set_u8(nvs_handle_t handle, const char* key, uint8_t value);
esp_err_t nvs_erase_key(nvs_handle_t handle, const char* key);

#endif /* NVS_H_ */

/**** END OF FILE ****/
