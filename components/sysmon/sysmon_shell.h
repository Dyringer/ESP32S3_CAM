/**
 ******************************************************************************
 * @file    sysmon_shell.h
 * @author  Daniel Dyringer
 * @date    2026-09-30
 * @brief   System monitor shell commands.
 ******************************************************************************
 */

/* Define to prevent recursive inclusion ----------------------------------- */
#ifndef SYSMON_SHELL_H_
#define SYSMON_SHELL_H_

/* Includes ---------------------------------------------------------------- */

#include "esp_err.h"

/* Exposed API ------------------------------------------------------------- */

/**
 * @brief Register system monitor shell commands with app_shell.
 *
 *   cpu   per-core load, then a per-task breakdown measured over a fresh
 *         1 s window (blocks the calling console for that second)
 *
 * @return ESP_OK on success.
 */
esp_err_t sysmon_shell_register(void);

#endif /* SYSMON_SHELL_H_ */

/**** END OF FILE ****/
