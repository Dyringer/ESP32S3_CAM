/**
 ******************************************************************************
 * @file    storage.h
 * @author  Daniel Dyringer
 * @date    2026-10-25
 * @brief   LittleFS on the "storage" partition, mounted at STORAGE_MOUNT_POINT.
 *
 *          Files are accessed with plain POSIX/stdio calls on paths built by
 *          storage_resolve_path(). LittleFS serialises individual calls
 *          internally; holds (storage_hold) only guard storage_format().
 ******************************************************************************
 */

/* Define to prevent recursive inclusion ----------------------------------- */
#ifndef STORAGE_H_
#define STORAGE_H_

/* Includes ---------------------------------------------------------------- */
#include <stdbool.h>
#include <stddef.h>

#include "esp_err.h"

/* Constants and macros ---------------------------------------------------- */
// clang-format off

#define STORAGE_MOUNT_POINT         "/fs"
#define STORAGE_PARTITION_LABEL     "storage"

/* Full VFS path including STORAGE_MOUNT_POINT and the terminating NUL. */
#define STORAGE_PATH_MAX            128u

// clang-format on

/* Types ------------------------------------------------------------------- */


/* Exposed API ------------------------------------------------------------- */

/* Formats the partition if it can't be mounted (first boot after the
 * partition table change). */
esp_err_t storage_init(void);
bool storage_is_mounted(void);

esp_err_t storage_get_info(size_t* total_bytes, size_t* used_bytes);

/* relative_path is already URL-decoded, '/'-separated, leading '/' optional;
 * "" and "/" are the root. Rejects (ESP_ERR_INVALID_ARG) empty segments,
 * segments starting with '.' (covers "." and ".." and the hidden upload temp
 * file), control characters and results that don't fit full_path_size.
 * Writes e.g. "/fs/dir/a.txt" (no trailing '/'). */
esp_err_t storage_resolve_path(const char* relative_path, char* full_path, size_t full_path_size);

/* File or empty directory. ESP_ERR_NOT_FOUND, ESP_ERR_INVALID_STATE for a
 * non-empty directory. */
esp_err_t storage_remove(const char* full_path);

/* Single fixed temp path for uploads (one transfer at a time, see
 * http_panel_transfer): written, then rename()d over the target. A leftover
 * from a dropped upload or reboot is deleted by storage_init(). */
const char* storage_upload_temp_path(void);

/* Taken around any open file (upload, download, shell cat) so
 * storage_format() can refuse with ESP_ERR_INVALID_STATE instead of
 * formatting under an open handle. Non-blocking counter. */
void storage_hold(void);
void storage_release(void);
esp_err_t storage_format(void);

#endif /* STORAGE_H_ */

/**** END OF FILE ****/
