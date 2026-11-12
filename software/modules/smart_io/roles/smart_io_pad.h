/**
 ******************************************************************************
 * @file    smart_io_pad.h
 * @author  Daniel Dyringer
 * @date    2026-11-06
 * @brief   Pool pins as pads: what the silicon allows and the released
 *          state. The only GPIO setup outside a role.
 ******************************************************************************
 */

/* Define to prevent recursive inclusion ----------------------------------- */
#ifndef SMART_IO_PAD_H_
#define SMART_IO_PAD_H_

/* Includes ---------------------------------------------------------------- */
#include <stdint.h>

#include "soc/gpio_num.h"


/* Constants and macros ---------------------------------------------------- */
// clang-format off


// clang-format on

/* Types ------------------------------------------------------------------- */


/* Exposed API ------------------------------------------------------------- */

/* The board caps narrowed to what the pad supports: never widened. */
uint32_t smart_io_pad_caps(gpio_num_t gpio, uint32_t board_caps);

/* Released state: input, no pull, no interrupt. GPIO_NUM_NC is ignored. */
void smart_io_pad_park(gpio_num_t gpio);

#endif /* SMART_IO_PAD_H_ */

/**** END OF FILE ****/
