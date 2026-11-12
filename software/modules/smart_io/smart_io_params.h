/**
 ******************************************************************************
 * @file    smart_io_params.h
 * @author  Daniel Dyringer
 * @date    2026-10-31
 * @brief   Generic operations on a role's param descriptors.
 ******************************************************************************
 */

/* Define to prevent recursive inclusion ----------------------------------- */
#ifndef SMART_IO_PARAMS_H_
#define SMART_IO_PARAMS_H_

/* Includes ---------------------------------------------------------------- */
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "smart_io_types.h"


/* Constants and macros ---------------------------------------------------- */
// clang-format off

#define SMART_IO_PARAMS_TEXT_SIZE   16u

// clang-format on

/* Types ------------------------------------------------------------------- */


/* Exposed API ------------------------------------------------------------- */

void smart_io_params_defaults(
    const smart_io_role_description_t* description,
    uint32_t param_values[SMART_IO_PARAM_MAX]);
esp_err_t smart_io_params_find(
    const smart_io_role_description_t* description,
    const char* name,
    size_t* index);
esp_err_t smart_io_params_check(
    const smart_io_role_description_t* description,
    const uint32_t param_values[SMART_IO_PARAM_MAX],
    smart_io_reason_t* reason);

esp_err_t smart_io_params_parse(
    const smart_io_param_descriptor_t* descriptor,
    const char* text,
    uint32_t* value,
    smart_io_reason_t* reason);
size_t smart_io_params_format(
    const smart_io_param_descriptor_t* descriptor,
    uint32_t value,
    char* buffer,
    size_t size);

#endif /* SMART_IO_PARAMS_H_ */

/**** END OF FILE ****/
