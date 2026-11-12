/**
 ******************************************************************************
 * @file    sysinfo_page.h
 * @author  Daniel Dyringer
 * @date    2026-10-13
 * @brief   System info page — FreeRTOS tasks, per-core load and heap usage.
 ******************************************************************************
 */

/* Define to prevent recursive inclusion ----------------------------------- */
#ifndef SYSINFO_PAGE_H_
#define SYSINFO_PAGE_H_

/* Includes ---------------------------------------------------------------- */
#include "esp_err.h"

/* Constants and macros ---------------------------------------------------- */
// clang-format off


// clang-format on

/* Types ------------------------------------------------------------------- */


/* Exposed API ------------------------------------------------------------- */

esp_err_t sysinfo_page_init(void);

#endif /* SYSINFO_PAGE_H_ */

/**** END OF FILE ****/
