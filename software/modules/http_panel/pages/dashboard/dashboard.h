/**
 ******************************************************************************
 * @file    dashboard.h
 * @author  Daniel Dyringer
 * @date    2026-03-09
 * @brief   Dashboard page — status, sensors and device actions.
 ******************************************************************************
 */

/* Define to prevent recursive inclusion ----------------------------------- */
#ifndef DASHBOARD_H_
#define DASHBOARD_H_

/* Includes ---------------------------------------------------------------- */
#include "esp_err.h"

/* Constants and macros ---------------------------------------------------- */
// clang-format off


// clang-format on

/* Types ------------------------------------------------------------------- */


/* Exposed API ------------------------------------------------------------- */

esp_err_t dashboard_init(void);

#endif /* DASHBOARD_H_ */

/**** END OF FILE ****/
