/**
 ******************************************************************************
 * @file    data_manager_shell.h
 * @author  Daniel Dyringer
 * @date    2026-10-11
 * @brief   Data manager shell commands.
 *
 *          Commands:
 *            data list                All settings with type, flags and value
 *            data get <name>          Print one setting
 *            data set <name> <value>  Change a setting (runtime only)
 *            data save                Save every changed setting to flash
 *
 *          Booleans accept 1/0, true/false, on/off. Integers accept 0x hex.
 *          Blobs are hex strings of the full size. Quote strings containing
 *          spaces: data set device_name "desk cam"
 ******************************************************************************
 */

/* Define to prevent recursive inclusion ----------------------------------- */
#ifndef DATA_MANAGER_SHELL_H_
#define DATA_MANAGER_SHELL_H_

/* Includes ---------------------------------------------------------------- */
#include "esp_err.h"


/* Constants and macros ---------------------------------------------------- */
// clang-format off


// clang-format on

/* Types ------------------------------------------------------------------- */


/* Exposed API ------------------------------------------------------------- */

esp_err_t data_manager_shell_register(void);

#endif /* DATA_MANAGER_SHELL_H_ */

/**** END OF FILE ****/
