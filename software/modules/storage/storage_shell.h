/**
 ******************************************************************************
 * @file    storage_shell.h
 * @author  Daniel Dyringer
 * @date    2026-10-25
 * @brief   Shell command "fs": ls, df, rm, mkdir, cat, format.
 ******************************************************************************
 */

/* Define to prevent recursive inclusion ----------------------------------- */
#ifndef STORAGE_SHELL_H_
#define STORAGE_SHELL_H_

/* Includes ---------------------------------------------------------------- */
#include "esp_err.h"

/* Constants and macros ---------------------------------------------------- */
// clang-format off


// clang-format on

/* Types ------------------------------------------------------------------- */


/* Exposed API ------------------------------------------------------------- */

esp_err_t storage_shell_register(void);

#endif /* STORAGE_SHELL_H_ */

/**** END OF FILE ****/
