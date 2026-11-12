/**
 ******************************************************************************
 * @file    snapshot_upload_shell.h
 * @author  Daniel Dyringer
 * @date    2026-10-04
 * @brief   Snapshot uploader shell commands.
 *
 *          Commands:
 *            upload [status]               Configuration and last upload result
 *            upload url <url>              Set the http:// or https:// target
 *            upload auth <user> <password> Set HTTP Basic credentials
 *            upload auth off               Disable authentication
 *            upload interval <seconds>     Set the upload period
 *            upload on | off               Enable or disable uploading
 *
 *          Quote values containing spaces: upload auth cam "my secret"
 ******************************************************************************
 */

/* Define to prevent recursive inclusion ----------------------------------- */
#ifndef SNAPSHOT_UPLOAD_SHELL_H_
#define SNAPSHOT_UPLOAD_SHELL_H_

/* Includes ---------------------------------------------------------------- */
#include "esp_err.h"


/* Constants and macros ---------------------------------------------------- */
// clang-format off


// clang-format on

/* Types ------------------------------------------------------------------- */


/* Exposed API ------------------------------------------------------------- */

esp_err_t snapshot_upload_shell_register(void);

#endif /* SNAPSHOT_UPLOAD_SHELL_H_ */

/**** END OF FILE ****/
