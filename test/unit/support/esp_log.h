/**
 ******************************************************************************
 * @file    esp_log.h
 * @author  Daniel Dyringer
 * @date    2026-10-24
 * @brief   Host stand-in for the ESP-IDF esp_log.h used by unit tests.
 *          Messages are dropped; errors and warnings are counted by fake_idf.
 ******************************************************************************
 */

/* Define to prevent recursive inclusion ----------------------------------- */
#ifndef ESP_LOG_H_
#define ESP_LOG_H_

/* Includes ---------------------------------------------------------------- */
#include "fake_idf.h"

/* Constants and macros ---------------------------------------------------- */
// clang-format off

#define ESP_LOGE(tag, ...)      fake_idf_log(tag, FAKE_IDF_LOG_ERROR)
#define ESP_LOGW(tag, ...)      fake_idf_log(tag, FAKE_IDF_LOG_WARNING)
#define ESP_LOGI(tag, ...)      fake_idf_log(tag, FAKE_IDF_LOG_INFO)
#define ESP_LOGD(tag, ...)      fake_idf_log(tag, FAKE_IDF_LOG_INFO)

// clang-format on

/* Types ------------------------------------------------------------------- */


/* Exposed API ------------------------------------------------------------- */


#endif /* ESP_LOG_H_ */

/**** END OF FILE ****/
