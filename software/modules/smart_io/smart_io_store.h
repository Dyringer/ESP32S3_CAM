/**
 ******************************************************************************
 * @file    smart_io_store.h
 * @author  Daniel Dyringer
 * @date    2026-10-31
 * @brief   Saved configuration: one versioned data_manager blob of
 *          smart_io_config_t. smart_io builds and applies it.
 ******************************************************************************
 */

/* Define to prevent recursive inclusion ----------------------------------- */
#ifndef SMART_IO_STORE_H_
#define SMART_IO_STORE_H_

/* Includes ---------------------------------------------------------------- */
#include <stddef.h>

#include "esp_err.h"
#include "smart_io.h"


/* Constants and macros ---------------------------------------------------- */
// clang-format off


// clang-format on

/* Types ------------------------------------------------------------------- */


/* Exposed API ------------------------------------------------------------- */

/* Registers the blob with data_manager. Before data_manager loads it. */
esp_err_t smart_io_store_init(void);

/* One config per configured pin set; streams are not saved. Flash write: call
 * without the smart_io mutex. */
esp_err_t smart_io_store_save(const smart_io_config_t* configs, size_t count);
/* ESP_ERR_NOT_FOUND when nothing is saved, ESP_ERR_INVALID_VERSION on a
 * version mismatch (logged, ignored). */
esp_err_t smart_io_store_load(smart_io_config_t* configs, size_t capacity, size_t* count);

#endif /* SMART_IO_STORE_H_ */

/**** END OF FILE ****/
