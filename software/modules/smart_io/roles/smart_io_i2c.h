/**
 ******************************************************************************
 * @file    smart_io_i2c.h
 * @author  Daniel Dyringer
 * @date    2026-10-31
 * @brief   i2c role: I2C master bus, cached device handle, batch transfers,
 *          scan, bus recovery.
 ******************************************************************************
 */

/* Define to prevent recursive inclusion ----------------------------------- */
#ifndef SMART_IO_I2C_H_
#define SMART_IO_I2C_H_

/* Includes ---------------------------------------------------------------- */
#include "smart_io_role.h"


/* Constants and macros ---------------------------------------------------- */
// clang-format off


// clang-format on

/* Types ------------------------------------------------------------------- */


/* Exposed API ------------------------------------------------------------- */

extern const smart_io_role_api_t smart_io_i2c_api;

#endif /* SMART_IO_I2C_H_ */

/**** END OF FILE ****/
