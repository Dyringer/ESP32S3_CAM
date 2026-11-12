/**
 ******************************************************************************
 * @file    smart_io_registry.h
 * @author  Daniel Dyringer
 * @date    2026-10-31
 * @brief   Pin descriptions from boards/<board>/bsp_smart_io.h and the
 *          instance slots, lookups by gpio and name. Data only: the caller
 *          holds the smart_io mutex for instances.
 ******************************************************************************
 */

/* Define to prevent recursive inclusion ----------------------------------- */
#ifndef SMART_IO_REGISTRY_H_
#define SMART_IO_REGISTRY_H_

/* Includes ---------------------------------------------------------------- */
#include <stddef.h>
#include <stdint.h>

#include "bsp_smart_io.h"
#include "esp_err.h"
#include "smart_io_role.h"
#include "soc/gpio_num.h"


/* Constants and macros ---------------------------------------------------- */
// clang-format off

#define SMART_IO_REGISTRY_COUNT_PIN(gpio, caps)     +1u

#define SMART_IO_MAX_PINS   (0u BSP_SMART_IO_PINS(SMART_IO_REGISTRY_COUNT_PIN))


// clang-format on

/* Types ------------------------------------------------------------------- */

_Static_assert(SMART_IO_MAX_PINS <= 32u, "one uint32_t per pin set, 5-bit pool index");


/* Exposed API ------------------------------------------------------------- */

/* Narrows the board caps to what each pad supports. */
esp_err_t smart_io_registry_init(void);

const smart_io_pin_t* smart_io_registry_pins(size_t* count);
/* ESP_ERR_NOT_FOUND when gpio is not in the pool. */
esp_err_t smart_io_registry_pin_index(gpio_num_t gpio, uint8_t* index);

/* All SMART_IO_MAX_PINS slots; free ones have api == NULL. */
smart_io_instance_t* smart_io_registry_instances(size_t* count);
/* NULL when no instance uses gpio / has that label / no slot is free. */
smart_io_instance_t* smart_io_registry_find_gpio(gpio_num_t gpio);
smart_io_instance_t* smart_io_registry_find_label(const char* label);
smart_io_instance_t* smart_io_registry_alloc(void);
/* Back to a free slot; the work item is kept. */
void smart_io_registry_free(smart_io_instance_t* instance);

#endif /* SMART_IO_REGISTRY_H_ */

/**** END OF FILE ****/
