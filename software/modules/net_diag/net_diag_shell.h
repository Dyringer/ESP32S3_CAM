/**
 ******************************************************************************
 * @file    net_diag_shell.h
 * @author  Daniel Dyringer
 * @date    2026-10-17
 * @brief   Network diagnostics shell commands.
 *
 *          Commands:
 *            ping start <host> [-c count] [-s bytes] [-i ms] [-W ms]
 *            ping status
 ******************************************************************************
 */

/* Define to prevent recursive inclusion ----------------------------------- */
#ifndef NET_DIAG_SHELL_H_
#define NET_DIAG_SHELL_H_

/* Includes ---------------------------------------------------------------- */
#include "esp_err.h"

/* Constants and macros ---------------------------------------------------- */
// clang-format off


// clang-format on

/* Types ------------------------------------------------------------------- */


/* Exposed API ------------------------------------------------------------- */

esp_err_t net_diag_shell_register(void);

#endif /* NET_DIAG_SHELL_H_ */

/**** END OF FILE ****/
