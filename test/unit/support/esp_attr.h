/**
 ******************************************************************************
 * @file    esp_attr.h
 * @author  Daniel Dyringer
 * @date    2026-10-26
 * @brief   Host stand-in for the ESP-IDF esp_attr.h.
 ******************************************************************************
 */

/* Define to prevent recursive inclusion ----------------------------------- */
#ifndef ESP_ATTR_H_
#define ESP_ATTR_H_

/* Includes ---------------------------------------------------------------- */


/* Constants and macros ---------------------------------------------------- */
// clang-format off

#define IRAM_ATTR
#define DRAM_ATTR
#define FORCE_INLINE_ATTR   static inline __attribute__((always_inline))

/* A named section lets fake_nvs_rtc_lose() wipe every RTC variable. */
#define RTC_NOINIT_ATTR __attribute__((section("fake_rtc_noinit")))

// clang-format on

/* Types ------------------------------------------------------------------- */


/* Exposed API ------------------------------------------------------------- */


#endif /* ESP_ATTR_H_ */

/**** END OF FILE ****/
