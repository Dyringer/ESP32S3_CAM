/**
 ******************************************************************************
 * @file    esp_gpio_reserve.h
 * @author  Daniel Dyringer
 * @date    2026-10-27
 * @brief   Host stand-in for the ESP-IDF esp_private/esp_gpio_reserve.h. The
 *          reservation mask lives in fake_smart_io, where gpio_config()
 *          reserves output pins and counts conflicts like ESP-IDF 6.1.
 ******************************************************************************
 */

/* Define to prevent recursive inclusion ----------------------------------- */
#ifndef ESP_GPIO_RESERVE_H_
#define ESP_GPIO_RESERVE_H_

/* Includes ---------------------------------------------------------------- */
#include <stdbool.h>
#include <stdint.h>

/* Constants and macros ---------------------------------------------------- */
// clang-format off


// clang-format on

/* Types ------------------------------------------------------------------- */


/* Exposed API ------------------------------------------------------------- */

uint64_t esp_gpio_reserve(uint64_t gpio_mask);
uint64_t esp_gpio_revoke(uint64_t gpio_mask);
bool esp_gpio_is_reserved(uint64_t gpio_mask);

#endif /* ESP_GPIO_RESERVE_H_ */

/**** END OF FILE ****/
