/**
 ******************************************************************************
 * @file    camera_shell.h
 * @author  Daniel Dyringer
 * @date    2026-09-30
 * @brief   Camera shell commands.
 ******************************************************************************
 */

/* Define to prevent recursive inclusion ----------------------------------- */
#ifndef CAMERA_SHELL_H_
#define CAMERA_SHELL_H_

/* Includes ---------------------------------------------------------------- */

#include "esp_err.h"

/* Exposed API ------------------------------------------------------------- */

/**
 * @brief Register camera shell commands with app_shell.
 *
 *   camera status                  sensor, resolution and every setting
 *   camera set <name> <value>      apply one setting (until saved)
 *   camera save                    store the current settings in NVS
 *   camera defaults                restore boot defaults, erase saved set
 *   camera reg <reg> <mask> [val]  read, or read-modify-write, a raw sensor
 *                                  register (numbers accept 0x.. hex)
 *
 * @return ESP_OK on success.
 */
esp_err_t camera_shell_register(void);

#endif /* CAMERA_SHELL_H_ */

/**** END OF FILE ****/
