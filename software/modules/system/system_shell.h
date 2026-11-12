/**
 ******************************************************************************
 * @file    system_shell.h
 * @author  Daniel Dyringer
 * @date    2026-10-07
 * @brief   System shell commands — reboot, factory_reset.
 ******************************************************************************
 */

/* Define to prevent recursive inclusion ----------------------------------- */
#ifndef SYSTEM_SHELL_H_
#define SYSTEM_SHELL_H_

/* Includes ---------------------------------------------------------------- */
#include "esp_err.h"

/* Constants and macros ---------------------------------------------------- */
// clang-format off


// clang-format on

/* Types ------------------------------------------------------------------- */


/* Exposed API ------------------------------------------------------------- */

esp_err_t system_shell_register(void);

#endif /* SYSTEM_SHELL_H_ */

/**** END OF FILE ****/
