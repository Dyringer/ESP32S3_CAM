/**
 ******************************************************************************
 * @file    smart_io_shell.h
 * @author  Daniel Dyringer
 * @date    2026-10-31
 * @brief   sio console commands.
 ******************************************************************************
 */

/* Define to prevent recursive inclusion ----------------------------------- */
#ifndef SMART_IO_SHELL_H_
#define SMART_IO_SHELL_H_

/* Includes ---------------------------------------------------------------- */
#include "esp_err.h"


/* Constants and macros ---------------------------------------------------- */
// clang-format off


// clang-format on

/* Types ------------------------------------------------------------------- */


/* Exposed API ------------------------------------------------------------- */

esp_err_t smart_io_shell_register(void);

#endif /* SMART_IO_SHELL_H_ */

/**** END OF FILE ****/
