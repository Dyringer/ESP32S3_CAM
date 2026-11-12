/**
 ******************************************************************************
 * @file    i2c_types.h
 * @author  Daniel Dyringer
 * @date    2026-10-26
 * @brief   Host stand-in for the ESP-IDF hal/i2c_types.h.
 ******************************************************************************
 */

/* Define to prevent recursive inclusion ----------------------------------- */
#ifndef HAL_I2C_TYPES_H_
#define HAL_I2C_TYPES_H_

/* Includes ---------------------------------------------------------------- */


/* Constants and macros ---------------------------------------------------- */
// clang-format off


// clang-format on

/* Types ------------------------------------------------------------------- */

typedef enum
{
    I2C_NUM_0 = 0,
    I2C_NUM_1,
    I2C_NUM_MAX,
} i2c_port_t;


/* Exposed API ------------------------------------------------------------- */


#endif /* HAL_I2C_TYPES_H_ */

/**** END OF FILE ****/
