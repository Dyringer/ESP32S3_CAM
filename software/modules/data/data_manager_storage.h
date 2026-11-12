/**
 ******************************************************************************
 * @file    data_manager_storage.h
 * @author  Daniel Dyringer
 * @date    2026-10-11
 * @brief   NVS backend of data_manager: one blob per value, keyed by its id.
 ******************************************************************************
 */

/* Define to prevent recursive inclusion ----------------------------------- */
#ifndef DATA_MANAGER_STORAGE_H_
#define DATA_MANAGER_STORAGE_H_

/* Includes ---------------------------------------------------------------- */
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

/* Constants and macros ---------------------------------------------------- */
// clang-format off


// clang-format on

/* Types ------------------------------------------------------------------- */


/* Exposed API ------------------------------------------------------------- */

/* ESP_ERR_NOT_FOUND when never saved, ESP_ERR_INVALID_SIZE when the saved
 * blob has a different size. */
esp_err_t data_manager_storage_load(uint32_t id, void* value, size_t size);
esp_err_t data_manager_storage_save(uint32_t id, const void* value, size_t size);
esp_err_t data_manager_storage_erase(uint32_t id);

#endif /* DATA_MANAGER_STORAGE_H_ */

/**** END OF FILE ****/
