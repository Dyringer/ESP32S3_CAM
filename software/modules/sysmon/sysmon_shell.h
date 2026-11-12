/**
 ******************************************************************************
 * @file    sysmon_shell.h
 * @author  Daniel Dyringer
 * @date    2026-09-30
 * @brief   System monitor shell commands.
 *
 *          Commands:
 *            cpu         CPU load per core and per task (last 1 s window)
 ******************************************************************************
 */

/* Define to prevent recursive inclusion ----------------------------------- */
#ifndef SYSMON_SHELL_H_
#define SYSMON_SHELL_H_

/* Includes ---------------------------------------------------------------- */
#include "esp_err.h"

/* Constants and macros ---------------------------------------------------- */
// clang-format off


// clang-format on

/* Types ------------------------------------------------------------------- */


/* Exposed API ------------------------------------------------------------- */

esp_err_t sysmon_shell_register(void);

#endif /* SYSMON_SHELL_H_ */

/**** END OF FILE ****/
