/**
 ******************************************************************************
 * @file    i2c_tools.h
 * @author  Daniel Dyringer
 * @date    2026-02-18
 * @brief   I2C diagnostic shell commands — i2c scan / read / write.
 ******************************************************************************
 */

/* Define to prevent recursive inclusion ----------------------------------- */
#ifndef I2C_TOOLS_H_
#define I2C_TOOLS_H_

/* Includes ---------------------------------------------------------------- */
#include "driver/i2c_master.h"
#include "esp_err.h"


/* Constants and macros ---------------------------------------------------- */
// clang-format off


// clang-format on

/* Types ------------------------------------------------------------------- */

typedef esp_err_t (*i2c_tools_pause_fn_t)(void);
typedef esp_err_t (*i2c_tools_resume_fn_t)(void);


/* Exposed API ------------------------------------------------------------- */

/* pause_function/resume_function (may be NULL) bracket each read/write so a
 * polling bus user doesn't interleave with a manual register poke. Both run
 * in the calling shell task. */
esp_err_t i2c_tools_register(
    i2c_master_bus_handle_t bus,
    i2c_tools_pause_fn_t pause_function,
    i2c_tools_resume_fn_t resume_function);


#endif /* I2C_TOOLS_H_ */

/**** END OF FILE ****/
