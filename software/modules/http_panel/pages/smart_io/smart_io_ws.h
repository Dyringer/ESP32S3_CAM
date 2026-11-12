/**
 ******************************************************************************
 * @file    smart_io_ws.h
 * @author  Daniel Dyringer
 * @date    2026-10-31
 * @brief   WebSocket transport of smart_io (/io/ws): sio_ws task, request
 *          queue, per-client dirty masks, text and binary pushes.
 ******************************************************************************
 */

/* Define to prevent recursive inclusion ----------------------------------- */
#ifndef SMART_IO_WS_H_
#define SMART_IO_WS_H_

/* Includes ---------------------------------------------------------------- */
#include "esp_err.h"


/* Constants and macros ---------------------------------------------------- */
// clang-format off


// clang-format on

/* Types ------------------------------------------------------------------- */


/* Exposed API ------------------------------------------------------------- */

esp_err_t smart_io_ws_init(void);

#endif /* SMART_IO_WS_H_ */

/**** END OF FILE ****/
