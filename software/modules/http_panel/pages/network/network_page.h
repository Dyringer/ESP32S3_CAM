/**
 ******************************************************************************
 * @file    network_page.h
 * @author  Daniel Dyringer
 * @date    2026-10-13
 * @brief   Network page — link details, WiFi AP scan, ping and iperf.
 ******************************************************************************
 */

/* Define to prevent recursive inclusion ----------------------------------- */
#ifndef NETWORK_PAGE_H_
#define NETWORK_PAGE_H_

/* Includes ---------------------------------------------------------------- */
#include "esp_err.h"

/* Constants and macros ---------------------------------------------------- */
// clang-format off


// clang-format on

/* Types ------------------------------------------------------------------- */


/* Exposed API ------------------------------------------------------------- */

esp_err_t network_page_init(void);

#endif /* NETWORK_PAGE_H_ */

/**** END OF FILE ****/
