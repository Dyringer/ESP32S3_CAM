/**
 ******************************************************************************
 * @file    data_manager.h
 * @author  Daniel Dyringer
 * @date    2026-10-10
 * @brief   Registry of device settings with NVS persistence.
 *
 *          Every setting is identified by DATA_MANAGER_ID("<module>_<name>"),
 *          a compile-time hash of its name. Each module owns a
 *          <module>_settings.c/h with the descriptor table, an init that
 *          registers it and a restore_defaults that resets it.
 *
 *          Runtime changes are not persistent until data_manager_save(),
 *          which saves every changed setting of every module.
 ******************************************************************************
 */

/* Define to prevent recursive inclusion ----------------------------------- */
#ifndef DATA_MANAGER_H_
#define DATA_MANAGER_H_

/* Includes ---------------------------------------------------------------- */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "esp_event.h"
#include "hash.h"

/* Constants and macros ---------------------------------------------------- */
// clang-format off

#define DATA_MANAGER_CAPACITY           64u
#define DATA_MANAGER_NAME_MAX_LENGTH    HASH_STRING_MAX_LENGTH
#define DATA_MANAGER_NOTIFY_DELAY_MS    100u

#define DATA_MANAGER_ID(name)           HASH_STRING(name)

// clang-format on

/* Types ------------------------------------------------------------------- */

ESP_EVENT_DECLARE_BASE(DATA_MANAGER_EVENTS);

typedef enum data_manager_event_e
{
    /* event_data is the uint32_t id of the changed entry. */
    DATA_MANAGER_EVENT_CHANGED = 0,
} data_manager_event_t;

typedef enum data_manager_type_e
{
    DATA_MANAGER_TYPE_BOOL  = 0,
    DATA_MANAGER_TYPE_U8    = 1,
    DATA_MANAGER_TYPE_U16   = 2,
    DATA_MANAGER_TYPE_U32   = 3,
    DATA_MANAGER_TYPE_I8    = 4,
    DATA_MANAGER_TYPE_I16   = 5,
    DATA_MANAGER_TYPE_I32   = 6,
    DATA_MANAGER_TYPE_FLOAT = 7,
    /* size is the buffer capacity, terminator included. */
    DATA_MANAGER_TYPE_STRING = 8,
    DATA_MANAGER_TYPE_BLOB   = 9,

    DATA_MANAGER_TYPE_COUNT = 10,
} data_manager_type_t;

/* Called with the data_manager lock held: must not call the data_manager API. */
typedef esp_err_t (*data_manager_validator_t)(const void* value, size_t size);

/* Runs in the work_queue task, at most once per DATA_MANAGER_NOTIFY_DELAY_MS
 * per changed entry; read the latest value with data_manager_get(). Keep it
 * cheap: apply one value or schedule work, never reinitialise a module here. */
typedef void (*data_manager_on_change_t)(uint32_t id);

typedef struct data_manager_descriptor_s
{
    const char* name;
    void* value;
    /* At least size bytes. */
    const void* default_value;
    data_manager_validator_t validator;
    size_t size;
    /* DATA_MANAGER_ID(name); checked against the name at registration. */
    uint32_t id;
    data_manager_type_t type;
    /* Inclusive range for integer types; enforced only when minimum < maximum. */
    int32_t minimum;
    int32_t maximum;
    bool persistent;
    /* Never printed by the shell or the panel. */
    bool secret;
} data_manager_descriptor_t;

/* Exposed API ------------------------------------------------------------- */

esp_err_t data_manager_init(void);

/* Descriptors must stay valid forever (static storage). Persistent values are
 * loaded from NVS before this returns. */
esp_err_t data_manager_register(
    const data_manager_descriptor_t* descriptors,
    size_t count,
    data_manager_on_change_t on_change);

/* ---- Settings access ---- */

esp_err_t data_manager_get(uint32_t id, void* value, size_t size);
esp_err_t data_manager_set(uint32_t id, const void* value, size_t size);

esp_err_t data_manager_save(void);
bool data_manager_unsaved(void);

/* Back to defaults with the saved records erased, so a later firmware with
 * new defaults applies them. Persistent right away, no save needed. */
esp_err_t data_manager_reset(const data_manager_descriptor_t* descriptors, size_t count);

/* ---- Discovery (shell, web panel) ---- */

const data_manager_descriptor_t* data_manager_descriptor(uint32_t id);
/* Also compares the name, so a typed name that collides with another
 * setting's hash is rejected. */
const data_manager_descriptor_t* data_manager_find(const char* name);
size_t data_manager_count(void);
const data_manager_descriptor_t* data_manager_at(size_t index);

#endif /* DATA_MANAGER_H_ */

/**** END OF FILE ****/
