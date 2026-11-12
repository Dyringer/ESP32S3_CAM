/**
 ******************************************************************************
 * @file    esp_timer.h
 * @author  Daniel Dyringer
 * @date    2026-10-24
 * @brief   Host stand-in for the ESP-IDF esp_timer.h used by unit tests.
 *          Time and timer expiry are driven by fake_idf.
 ******************************************************************************
 */

/* Define to prevent recursive inclusion ----------------------------------- */
#ifndef ESP_TIMER_H_
#define ESP_TIMER_H_

/* Includes ---------------------------------------------------------------- */
#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

/* Constants and macros ---------------------------------------------------- */
// clang-format off


// clang-format on

/* Types ------------------------------------------------------------------- */

typedef struct esp_timer* esp_timer_handle_t;

typedef void (*esp_timer_cb_t)(void* arg);

typedef enum
{
    ESP_TIMER_TASK = 0,
    ESP_TIMER_ISR  = 1,
} esp_timer_dispatch_t;

typedef struct
{
    esp_timer_cb_t callback;
    void* arg;
    esp_timer_dispatch_t dispatch_method;
    const char* name;
    bool skip_unhandled_events;
} esp_timer_create_args_t;

/* Exposed API ------------------------------------------------------------- */

esp_err_t esp_timer_create(
    const esp_timer_create_args_t* create_args,
    esp_timer_handle_t* out_handle);
esp_err_t esp_timer_start_once(esp_timer_handle_t timer, uint64_t timeout_us);
esp_err_t esp_timer_stop(esp_timer_handle_t timer);
esp_err_t esp_timer_delete(esp_timer_handle_t timer);
int64_t esp_timer_get_time(void);

#endif /* ESP_TIMER_H_ */

/**** END OF FILE ****/
