/**
 ******************************************************************************
 * @file    smart_io.h
 * @author  Daniel Dyringer
 * @date    2026-10-31
 * @brief   smart_io API. Every function may block on the smart_io mutex.
 ******************************************************************************
 */

/* Define to prevent recursive inclusion ----------------------------------- */
#ifndef SMART_IO_H_
#define SMART_IO_H_

/* Includes ---------------------------------------------------------------- */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "esp_event.h"
#include "smart_io_types.h"
#include "soc/gpio_num.h"

/* Constants and macros ---------------------------------------------------- */
// clang-format off

#define SMART_IO_TRANSFER_COUNT_MAX     8u
#define SMART_IO_TRANSFER_DATA_MAX      512u
#define SMART_IO_DISCOVER_MAX           128u

// clang-format on

/* Types ------------------------------------------------------------------- */

ESP_EVENT_DECLARE_BASE(SMART_IO_EVENTS);

typedef struct smart_io_stream_cursor_s smart_io_stream_cursor_t;

/* Pool masks: bit n = pool pin n. */
typedef struct smart_io_changes_s
{
    uint32_t values;
    uint32_t configs;
    bool streams;
    bool unsaved;
} smart_io_changes_t;

/* Exposed API ------------------------------------------------------------- */

esp_err_t smart_io_init(void);

/* Changes since the last call; SMART_IO_EVENT_CHANGED is posted when there are
 * new ones. One consumer only (the transport merges them per client). Doesn't
 * lock. */
void smart_io_changes_take(smart_io_changes_t* changes);

/* reason may be NULL. */
void smart_io_reason_set(smart_io_reason_t* reason, const char* fmt, ...)
    __attribute__((format(printf, 2, 3)));

/* ---- Pins and roles ---- */

const smart_io_pin_t* smart_io_pins(size_t* count);
esp_err_t smart_io_lookup(const char* label, gpio_num_t* gpio);
/* NULL for SMART_IO_ROLE_NONE and out of range. */
const smart_io_role_info_t* smart_io_role_info(smart_io_role_t role);
esp_err_t smart_io_role_find(const char* name, smart_io_role_t* role);
const char* smart_io_function_name(smart_io_function_t function);

/* ---- Configuration ---- */

esp_err_t smart_io_config_get(gpio_num_t gpio, smart_io_config_t* config);
esp_err_t smart_io_configure(const smart_io_config_t* config, smart_io_reason_t* reason);
esp_err_t smart_io_release(gpio_num_t gpio, smart_io_reason_t* reason);
esp_err_t smart_io_save(void);
esp_err_t smart_io_revert(smart_io_reason_t* reason);
bool smart_io_unsaved(void);

/* ---- Values ---- */

esp_err_t smart_io_read(gpio_num_t gpio, int32_t* value, smart_io_reason_t* reason);
esp_err_t smart_io_write(
    gpio_num_t gpio,
    int32_t value,
    int32_t* applied,
    smart_io_reason_t* reason);

/* ---- Buses ---- */

/* Runs in order and stops at the first failure: failed = its index, count
 * when no transfer failed. Both lengths 0 = probe. Blocks for the bus time, up to count x
 * the role's timeout. ESP_ERR_NOT_FOUND: no ACK; ESP_ERR_TIMEOUT: bus reset done. */
esp_err_t smart_io_transfer(
    gpio_num_t gpio,
    const smart_io_transfer_t* transfers,
    size_t count,
    size_t* failed,
    smart_io_reason_t* reason);
esp_err_t smart_io_discover(
    gpio_num_t gpio,
    uint16_t* found,
    size_t capacity,
    size_t* count,
    smart_io_reason_t* reason);

/* ---- Streams ---- */

esp_err_t smart_io_stream(gpio_num_t gpio, smart_io_reason_t* reason);
esp_err_t smart_io_unstream(gpio_num_t gpio, smart_io_reason_t* reason);
/* Pool mask of the streamed pins. Doesn't lock. */
uint32_t smart_io_streams(void);
/* One per transport client. The last detach stops every stream. */
esp_err_t smart_io_client_attach(smart_io_stream_cursor_t* cursor);
void smart_io_client_detach(void);

#endif /* SMART_IO_H_ */

/**** END OF FILE ****/
