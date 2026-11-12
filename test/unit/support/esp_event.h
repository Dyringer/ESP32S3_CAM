/**
 ******************************************************************************
 * @file    esp_event.h
 * @author  Daniel Dyringer
 * @date    2026-10-26
 * @brief   Host stand-in for the ESP-IDF esp_event.h used by unit tests.
 *          Types, macros and prototypes; fake_smart_io implements the
 *          event loop calls smart_io uses.
 ******************************************************************************
 */

/* Define to prevent recursive inclusion ----------------------------------- */
#ifndef ESP_EVENT_H_
#define ESP_EVENT_H_

/* Includes ---------------------------------------------------------------- */
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "freertos/FreeRTOS.h"

/* Constants and macros ---------------------------------------------------- */
// clang-format off

#define ESP_EVENT_DECLARE_BASE(id)  extern esp_event_base_t const id
#define ESP_EVENT_DEFINE_BASE(id)   esp_event_base_t const id = #id
#define ESP_EVENT_ANY_ID            (-1)

// clang-format on

/* Types ------------------------------------------------------------------- */

typedef const char* esp_event_base_t;
typedef void* esp_event_loop_handle_t;

typedef struct
{
    int32_t queue_size;
    const char* task_name;
    UBaseType_t task_priority;
    uint32_t task_stack_size;
    BaseType_t task_core_id;
} esp_event_loop_args_t;

typedef void (*esp_event_handler_t)(
    void* event_handler_arg,
    esp_event_base_t event_base,
    int32_t event_id,
    void* event_data);

/* Exposed API ------------------------------------------------------------- */

/* Not implemented by fake_idf: tests that need them define them. */
esp_err_t esp_event_handler_register(
    esp_event_base_t event_base,
    int32_t event_id,
    esp_event_handler_t event_handler,
    void* event_handler_arg);
esp_err_t esp_event_handler_unregister(
    esp_event_base_t event_base,
    int32_t event_id,
    esp_event_handler_t event_handler);

esp_err_t esp_event_post(
    esp_event_base_t event_base,
    int32_t event_id,
    const void* event_data,
    size_t event_data_size,
    TickType_t ticks_to_wait);

esp_err_t esp_event_loop_create(
    const esp_event_loop_args_t* event_loop_args,
    esp_event_loop_handle_t* event_loop);
esp_err_t esp_event_loop_delete(esp_event_loop_handle_t event_loop);
esp_err_t esp_event_post_to(
    esp_event_loop_handle_t event_loop,
    esp_event_base_t event_base,
    int32_t event_id,
    const void* event_data,
    size_t event_data_size,
    TickType_t ticks_to_wait);
esp_err_t esp_event_handler_register_with(
    esp_event_loop_handle_t event_loop,
    esp_event_base_t event_base,
    int32_t event_id,
    esp_event_handler_t event_handler,
    void* event_handler_arg);
esp_err_t esp_event_handler_unregister_with(
    esp_event_loop_handle_t event_loop,
    esp_event_base_t event_base,
    int32_t event_id,
    esp_event_handler_t event_handler);

#endif /* ESP_EVENT_H_ */

/**** END OF FILE ****/
