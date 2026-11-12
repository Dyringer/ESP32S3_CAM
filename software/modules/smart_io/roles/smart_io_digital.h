/**
 ******************************************************************************
 * @file    smart_io_digital.h
 * @author  Daniel Dyringer
 * @date    2026-10-31
 * @brief   input and output roles: GPIO, debounce, digital stream producer on
 *          the IDF GPIO ISR service.
 ******************************************************************************
 */

/* Define to prevent recursive inclusion ----------------------------------- */
#ifndef SMART_IO_DIGITAL_H_
#define SMART_IO_DIGITAL_H_

/* Includes ---------------------------------------------------------------- */
#include "esp_err.h"
#include "smart_io_role.h"


/* Constants and macros ---------------------------------------------------- */
// clang-format off

/* GPIO ISR service core: edges can be frequent, so off the WiFi core. */
#define SMART_IO_DIGITAL_ISR_CORE       1


// clang-format on

/* Types ------------------------------------------------------------------- */


/* Exposed API ------------------------------------------------------------- */

extern const smart_io_role_api_t smart_io_digital_input_api;
extern const smart_io_role_api_t smart_io_digital_output_api;

/* Installs the GPIO ISR service on core 1 (IPC). From smart_io_init(). */
esp_err_t smart_io_digital_init(void);

#endif /* SMART_IO_DIGITAL_H_ */

/**** END OF FILE ****/
