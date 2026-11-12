/**
 ******************************************************************************
 * @file    system.h
 * @author  Daniel Dyringer
 * @date    2026-10-07
 * @brief   Device-wide actions — reboot, factory reset.
 ******************************************************************************
 */

/* Define to prevent recursive inclusion ----------------------------------- */
#ifndef SYSTEM_H_
#define SYSTEM_H_

/* Includes ---------------------------------------------------------------- */
#include "esp_err.h"

/* Constants and macros ---------------------------------------------------- */
// clang-format off


// clang-format on

/* Types ------------------------------------------------------------------- */


/* Exposed API ------------------------------------------------------------- */

void system_reboot(void);
esp_err_t system_factory_reset(void);

#endif /* SYSTEM_H_ */

/**** END OF FILE ****/
