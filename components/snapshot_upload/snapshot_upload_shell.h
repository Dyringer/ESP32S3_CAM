/**
 ******************************************************************************
 * @file    snapshot_upload_shell.h
 * @author  Daniel Dyringer
 * @date    2026-10-04
 * @brief   Snapshot uploader shell commands.
 ******************************************************************************
 */

/* Define to prevent recursive inclusion ----------------------------------- */
#ifndef SNAPSHOT_UPLOAD_SHELL_H_
#define SNAPSHOT_UPLOAD_SHELL_H_

/* Includes ---------------------------------------------------------------- */

#include "esp_err.h"

/* Exposed API ------------------------------------------------------------- */

/**
 * @brief Register the "upload" shell command with app_shell.
 *
 *   upload status                  configuration and last result
 *   upload url <url>               set the endpoint (http:// or https://)
 *   upload auth <user> <password>  set HTTP Basic Auth credentials
 *   upload auth off                send no credentials
 *   upload interval <seconds>      set the upload period
 *   upload on | off                start / stop periodic uploads
 *
 *   Values with spaces go in double quotes: upload auth cam "my secret".
 *   Every change is saved to NVS immediately.
 *
 * @return ESP_OK on success.
 */
esp_err_t snapshot_upload_shell_register(void);

#endif /* SNAPSHOT_UPLOAD_SHELL_H_ */

/**** END OF FILE ****/
