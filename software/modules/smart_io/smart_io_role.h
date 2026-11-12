/**
 ******************************************************************************
 * @file    smart_io_role.h
 * @author  Daniel Dyringer
 * @date    2026-11-06
 * @brief   Role interface: what smart_io calls on a role and what a role may
 *          call back. Private to the smart_io component (core and roles).
 ******************************************************************************
 */

/* Define to prevent recursive inclusion ----------------------------------- */
#ifndef SMART_IO_ROLE_H_
#define SMART_IO_ROLE_H_

/* Includes ---------------------------------------------------------------- */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "smart_io_ring.h"
#include "smart_io_stream_source.h"
#include "smart_io_types.h"
#include "work_queue.h"


/* Constants and macros ---------------------------------------------------- */
// clang-format off


// clang-format on

/* Types ------------------------------------------------------------------- */

typedef struct smart_io_state_s
{
    void* role_state;
    int32_t value;
    bool value_valid;
    bool busy; /* transfer or discover running outside the mutex */
} smart_io_state_t;

typedef struct smart_io_role_api_s smart_io_role_api_t;

/* Accessed under the smart_io mutex, except while busy. index: pool index of
 * gpio[0], set by smart_io before configure. */
typedef struct smart_io_instance_s
{
    smart_io_config_t config;
    const smart_io_role_api_t* role_api; /* NULL: free slot */
    smart_io_state_t state;
    work_queue_item_t work; /* runs role_api->deferred under the mutex */
    uint8_t index;
} smart_io_instance_t;

/* Called with the smart_io mutex held, except transfer and discover. */

typedef esp_err_t (*smart_io_role_validate_fn_t)(
    const smart_io_config_t* config,
    smart_io_reason_t* reason);

/* On failure the pins are left released. */
typedef esp_err_t (*smart_io_role_configure_fn_t)(
    smart_io_instance_t* instance,
    smart_io_reason_t* reason);

typedef void (*smart_io_role_release_fn_t)(smart_io_instance_t* instance);

typedef esp_err_t (*smart_io_role_read_fn_t)(
    smart_io_instance_t* instance,
    int32_t* value,
    smart_io_reason_t* reason);

typedef esp_err_t (*smart_io_role_write_fn_t)(
    smart_io_instance_t* instance,
    int32_t value,
    int32_t* applied,
    smart_io_reason_t* reason);

typedef esp_err_t (*smart_io_role_stream_start_fn_t)(
    smart_io_instance_t* instance,
    smart_io_ring_t* ring,
    smart_io_stream_source_t* source,
    smart_io_reason_t* reason);

typedef void (*smart_io_role_stream_stop_fn_t)(smart_io_instance_t* instance);

typedef esp_err_t (*smart_io_role_transfer_fn_t)(
    smart_io_instance_t* instance,
    const smart_io_transfer_t* transfers,
    size_t count,
    size_t* failed,
    smart_io_reason_t* reason);

typedef esp_err_t (*smart_io_role_discover_fn_t)(
    smart_io_instance_t* instance,
    uint16_t* found,
    size_t capacity,
    size_t* count,
    smart_io_reason_t* reason);

/* May be called spuriously: a call already taken by work_queue can't be
 * cancelled, so it may run after release, on the instance configured next in
 * the slot (any role). Act on the instance's current state only. */
typedef void (*smart_io_role_deferred_fn_t)(smart_io_instance_t* instance);

/* One const instance per role (smart_io_<role>_api). Capabilities come from
 * the pointers: write != NULL = writable, and so on. */
struct smart_io_role_api_s
{
    smart_io_role_description_t description;

    smart_io_role_validate_fn_t validate;
    smart_io_role_configure_fn_t configure;
    smart_io_role_release_fn_t release;
    smart_io_role_read_fn_t read;
    smart_io_role_write_fn_t write;
    smart_io_role_stream_start_fn_t stream_start;
    smart_io_role_stream_stop_fn_t stream_stop;
    smart_io_role_transfer_fn_t transfer;
    smart_io_role_discover_fn_t discover;
    smart_io_role_deferred_fn_t deferred;
};

/* Exposed API ------------------------------------------------------------- */

/* Called by roles with the mutex held. value_changed: the pin value (read,
 * updates). value_sample: one sample on the pin's VALUE stream, dropped when
 * the pin isn't streamed. A VALUE stream starts at source->initial. */
void smart_io_value_changed(smart_io_instance_t* instance, int32_t value);
void smart_io_value_sample(smart_io_instance_t* instance, int32_t value, int64_t time_us);

#endif /* SMART_IO_ROLE_H_ */

/**** END OF FILE ****/
