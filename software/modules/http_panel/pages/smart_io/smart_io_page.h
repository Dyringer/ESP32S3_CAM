/**
 ******************************************************************************
 * @file    smart_io_page.h
 * @author  Daniel Dyringer
 * @date    2026-10-26
 * @brief   IO page (/io) and its WebSocket (/io/ws), the web transport of the
 *          smart_io protocol.
 ******************************************************************************
 */

/* Define to prevent recursive inclusion ----------------------------------- */
#ifndef SMART_IO_PAGE_H_
#define SMART_IO_PAGE_H_

/* Includes ---------------------------------------------------------------- */
#include "esp_err.h"


/* Constants and macros ---------------------------------------------------- */
// clang-format off


// clang-format on

/* Types ------------------------------------------------------------------- */


/* Exposed API ------------------------------------------------------------- */

/* After smart_io_init(). */
esp_err_t smart_io_page_init(void);

#endif /* SMART_IO_PAGE_H_ */

/**** END OF FILE ****/
