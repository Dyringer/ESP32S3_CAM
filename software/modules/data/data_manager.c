/**
 ******************************************************************************
 * @file    data_manager.c
 * @author  Daniel Dyringer
 * @date    2026-10-10
 * @brief   Registry of device settings with NVS persistence.
 ******************************************************************************
 */


/* Includes ---------------------------------------------------------------- */
#include "data_manager.h"

#include <stdlib.h>
#include <string.h>

#include "data_manager_storage.h"
#include "esp_attr.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "work_queue.h"


/* Constants and macros ---------------------------------------------------- */
// clang-format off

#define DATA_MANAGER_INDEX_NONE             SIZE_MAX
#define DATA_MANAGER_SIZE_VARIABLE          0u
#define DATA_MANAGER_SIZE_INVALID           SIZE_MAX

/* Open-addressing index over the entries, keyed by the id. Twice the capacity
 * keeps it at most half full, so probes stay short and always find a free
 * slot. Must be a power of two. A slot holds entry index + 1, 0 = empty. */
#define DATA_MANAGER_SLOT_COUNT             (2u * DATA_MANAGER_CAPACITY)
#define DATA_MANAGER_SLOT_MASK              (DATA_MANAGER_SLOT_COUNT - 1u)
#define DATA_MANAGER_SLOT_EMPTY             0u

// clang-format on

_Static_assert(
    (DATA_MANAGER_SLOT_COUNT & DATA_MANAGER_SLOT_MASK) == 0u,
    "Slot count must be a power of two");
_Static_assert(DATA_MANAGER_CAPACITY < UINT8_MAX, "Slots store index + 1 in a byte");

/* Private types ----------------------------------------------------------- */

typedef enum data_manager_storage_action_e
{
    DATA_MANAGER_STORAGE_ACTION_NONE  = 0,
    DATA_MANAGER_STORAGE_ACTION_SAVE  = 1,
    DATA_MANAGER_STORAGE_ACTION_ERASE = 2,
} data_manager_storage_action_t;

/* id is copied from the descriptor so lookups stay in RAM; descriptors live in
 * flash behind the cache the camera also uses for PSRAM. */
typedef struct data_manager_entry_s
{
    /* Runs data_manager_process for this entry. */
    work_queue_item_t work;
    const data_manager_descriptor_t* descriptor;
    data_manager_on_change_t on_change;
    uint32_t id;
    data_manager_storage_action_t storage_action;
    bool change_pending;
    bool unsaved;
} data_manager_entry_t;

/* Private variables ------------------------------------------------------- */

static const char* data_manager_tag = "data_manager";

ESP_EVENT_DEFINE_BASE(DATA_MANAGER_EVENTS);

static SemaphoreHandle_t data_manager_mutex = NULL;

/* Guarded by data_manager_mutex. Entries are only ever appended. The table is in
 * PSRAM: never touch it from an ISR or while the flash cache is disabled. */
EXT_RAM_BSS_ATTR static data_manager_entry_t data_manager_entries[DATA_MANAGER_CAPACITY];
static size_t data_manager_entry_count                     = 0u;
static uint8_t data_manager_slots[DATA_MANAGER_SLOT_COUNT] = {0};

static const size_t data_manager_type_sizes[DATA_MANAGER_TYPE_COUNT] = {
    [DATA_MANAGER_TYPE_BOOL]   = sizeof(bool),
    [DATA_MANAGER_TYPE_U8]     = sizeof(uint8_t),
    [DATA_MANAGER_TYPE_U16]    = sizeof(uint16_t),
    [DATA_MANAGER_TYPE_U32]    = sizeof(uint32_t),
    [DATA_MANAGER_TYPE_I8]     = sizeof(int8_t),
    [DATA_MANAGER_TYPE_I16]    = sizeof(int16_t),
    [DATA_MANAGER_TYPE_I32]    = sizeof(int32_t),
    [DATA_MANAGER_TYPE_FLOAT]  = sizeof(float),
    [DATA_MANAGER_TYPE_STRING] = DATA_MANAGER_SIZE_VARIABLE,
    [DATA_MANAGER_TYPE_BLOB]   = DATA_MANAGER_SIZE_VARIABLE,
};


/* Private functions ------------------------------------------------------- */

/* ---- Index ---- */

/* Caller holds data_manager_mutex. */
static size_t data_manager_index_find(uint32_t id)
{
    size_t index = DATA_MANAGER_INDEX_NONE;
    size_t slot  = (size_t)(id & DATA_MANAGER_SLOT_MASK);

    while (data_manager_slots[slot] != DATA_MANAGER_SLOT_EMPTY)
    {
        const size_t candidate = (size_t)data_manager_slots[slot] - 1u;

        if (data_manager_entries[candidate].id == id)
        {
            index = candidate;
            break;
        }

        slot = (slot + 1u) & DATA_MANAGER_SLOT_MASK;
    }

    return index;
}

/* Caller holds data_manager_mutex and has checked that the id is new. */
static void data_manager_index_insert(uint32_t id, size_t index)
{
    size_t slot = (size_t)(id & DATA_MANAGER_SLOT_MASK);

    while (data_manager_slots[slot] != DATA_MANAGER_SLOT_EMPTY)
    {
        slot = (slot + 1u) & DATA_MANAGER_SLOT_MASK;
    }

    data_manager_slots[slot] = (uint8_t)(index + 1u);
}

/* ---- Values ---- */

static inline size_t data_manager_type_size(data_manager_type_t type)
{
    return ((uint32_t)type < (uint32_t)DATA_MANAGER_TYPE_COUNT) ?
               data_manager_type_sizes[type] :
               DATA_MANAGER_SIZE_INVALID;
}

static bool data_manager_descriptor_valid(const data_manager_descriptor_t* descriptor)
{
    bool valid = (descriptor->name != NULL) && (descriptor->value != NULL)
                 && (descriptor->default_value != NULL) && (descriptor->size > 0u);

    if (valid)
    {
        const size_t type_size = data_manager_type_size(descriptor->type);

        valid = (strnlen(descriptor->name, DATA_MANAGER_NAME_MAX_LENGTH + 1u)
                 <= DATA_MANAGER_NAME_MAX_LENGTH)
                && ((type_size == DATA_MANAGER_SIZE_VARIABLE)
                    || (type_size == descriptor->size));
    }

    return valid;
}

/* Value has already passed the size check. */
static bool data_manager_in_range(const data_manager_descriptor_t* descriptor, const void* value)
{
    bool ranged    = (descriptor->minimum < descriptor->maximum);
    int64_t number = 0;

    switch (descriptor->type)
    {
        case DATA_MANAGER_TYPE_U8:
        {
            uint8_t typed = 0u;
            memcpy(&typed, value, sizeof(typed));
            number = (int64_t)typed;
        }
        break;
        case DATA_MANAGER_TYPE_U16:
        {
            uint16_t typed = 0u;
            memcpy(&typed, value, sizeof(typed));
            number = (int64_t)typed;
        }
        break;
        case DATA_MANAGER_TYPE_U32:
        {
            uint32_t typed = 0u;
            memcpy(&typed, value, sizeof(typed));
            number = (int64_t)typed;
        }
        break;
        case DATA_MANAGER_TYPE_I8:
        {
            int8_t typed = 0;
            memcpy(&typed, value, sizeof(typed));
            number = (int64_t)typed;
        }
        break;
        case DATA_MANAGER_TYPE_I16:
        {
            int16_t typed = 0;
            memcpy(&typed, value, sizeof(typed));
            number = (int64_t)typed;
        }
        break;
        case DATA_MANAGER_TYPE_I32:
        {
            int32_t typed = 0;
            memcpy(&typed, value, sizeof(typed));
            number = (int64_t)typed;
        }
        break;
        default:
        {
            ranged = false;
        }
        break;
    }

    return (!ranged)
           || ((number >= (int64_t)descriptor->minimum)
               && (number <= (int64_t)descriptor->maximum));
}

static esp_err_t data_manager_value_valid(
    const data_manager_descriptor_t* descriptor,
    const void* value,
    size_t size)
{
    esp_err_t error_code = ESP_OK;

    if (descriptor->type == DATA_MANAGER_TYPE_STRING)
    {
        const size_t length = strnlen((const char*)value, size);

        if ((length >= size) || (length >= descriptor->size))
        {
            error_code = ESP_ERR_INVALID_SIZE;
        }
    }
    else if (size != descriptor->size)
    {
        error_code = ESP_ERR_INVALID_SIZE;
    }
    else if (!data_manager_in_range(descriptor, value))
    {
        error_code = ESP_ERR_INVALID_ARG;
    }
    else
    {
        /* Size and range match the descriptor. */
    }

    if ((error_code == ESP_OK) && (descriptor->validator != NULL))
    {
        error_code = descriptor->validator(value, size);
    }

    return error_code;
}

/* Caller holds data_manager_mutex and has validated the value. Returns whether
 * the stored value changed. Strings are zero-padded so the saved blob does
 * not depend on stale bytes. */
static bool data_manager_store(
    const data_manager_descriptor_t* descriptor,
    const void* value,
    size_t size)
{
    bool changed = false;

    if (descriptor->type == DATA_MANAGER_TYPE_STRING)
    {
        const size_t length = strnlen((const char*)value, size);

        const char* const current = descriptor->value;

        changed = (strncmp(current, (const char*)value, length + 1u) != 0);
        memset(descriptor->value, 0, descriptor->size);
        memcpy(descriptor->value, value, length);
    }
    else
    {
        changed = (memcmp(descriptor->value, value, descriptor->size) != 0);
        memcpy(descriptor->value, value, descriptor->size);
    }

    return changed;
}

/* Caller holds data_manager_mutex. */
static esp_err_t data_manager_copy_out(
    const data_manager_descriptor_t* descriptor,
    void* value,
    size_t size)
{
    esp_err_t error_code = ESP_OK;

    if (descriptor->type == DATA_MANAGER_TYPE_STRING)
    {
        const size_t length = strnlen((const char*)descriptor->value, descriptor->size);

        if (length >= size)
        {
            error_code = ESP_ERR_INVALID_SIZE;
        }
        else
        {
            memcpy(value, descriptor->value, length);
            ((char*)value)[length] = '\0';
        }
    }
    else if (size != descriptor->size)
    {
        error_code = ESP_ERR_INVALID_SIZE;
    }
    else
    {
        memcpy(value, descriptor->value, size);
    }

    return error_code;
}

/* ---- Deferred work ---- */

/* Caller holds data_manager_mutex. A later storage action replaces a pending
 * one: save after erase saves, erase after save erases. Storage runs right
 * away; a change alone waits DATA_MANAGER_NOTIFY_DELAY_MS so a burst of
 * changes reaches on_change and the event once. If scheduling fails the
 * flags stay set and the next successful one handles them. */
static esp_err_t data_manager_schedule(
    size_t index,
    bool change,
    data_manager_storage_action_t storage_action)
{
    esp_err_t error_code              = ESP_OK;
    data_manager_entry_t* const entry = &data_manager_entries[index];

    entry->change_pending = entry->change_pending || change;

    if (storage_action != DATA_MANAGER_STORAGE_ACTION_NONE)
    {
        entry->storage_action = storage_action;
        error_code            = work_queue_push(&entry->work);
    }
    else if (change)
    {
        error_code = work_queue_schedule(&entry->work, DATA_MANAGER_NOTIFY_DELAY_MS);
    }
    else
    {
        /* Nothing to report. */
    }

    return error_code;
}

/* Caller holds data_manager_mutex. */
static esp_err_t data_manager_write(size_t index, const void* value, size_t size)
{
    data_manager_entry_t* const entry                 = &data_manager_entries[index];
    const data_manager_descriptor_t* const descriptor = entry->descriptor;
    esp_err_t error_code = data_manager_value_valid(descriptor, value, size);

    if (error_code == ESP_OK)
    {
        const bool changed = data_manager_store(descriptor, value, size);

        if (changed && descriptor->persistent)
        {
            entry->unsaved = true;
        }

        error_code =
            data_manager_schedule(index, changed, DATA_MANAGER_STORAGE_ACTION_NONE);
    }

    return error_code;
}

/* Caller holds data_manager_mutex. */
static esp_err_t data_manager_reset_index(size_t index)
{
    data_manager_entry_t* const entry                 = &data_manager_entries[index];
    const data_manager_descriptor_t* const descriptor = entry->descriptor;

    const bool changed =
        (memcmp(descriptor->value, descriptor->default_value, descriptor->size) != 0);

    memcpy(descriptor->value, descriptor->default_value, descriptor->size);
    entry->unsaved = false;

    return data_manager_schedule(
        index,
        changed,
        descriptor->persistent ? DATA_MANAGER_STORAGE_ACTION_ERASE :
                                 DATA_MANAGER_STORAGE_ACTION_NONE);
}

static void data_manager_persist(
    const data_manager_descriptor_t* descriptor,
    data_manager_storage_action_t storage_action,
    const void* snapshot)
{
    esp_err_t error_code = ESP_OK;

    if (storage_action == DATA_MANAGER_STORAGE_ACTION_ERASE)
    {
        error_code = data_manager_storage_erase(descriptor->id);
    }
    else if (snapshot == NULL)
    {
        error_code = ESP_ERR_NO_MEM;
    }
    else
    {
        error_code = data_manager_storage_save(descriptor->id, snapshot, descriptor->size);
    }

    if (error_code != ESP_OK)
    {
        ESP_LOGE(
            data_manager_tag,
            "Storing %s failed: %s",
            descriptor->name,
            esp_err_to_name(error_code));
    }
}

/* Work handler of one entry. */
static void data_manager_process(void* context)
{
    data_manager_entry_t* const entry = context;

    xSemaphoreTake(data_manager_mutex, portMAX_DELAY);

    const data_manager_descriptor_t* const descriptor  = entry->descriptor;
    const data_manager_on_change_t on_change           = entry->on_change;
    const bool changed                                 = entry->change_pending;
    const data_manager_storage_action_t storage_action = entry->storage_action;
    uint8_t* snapshot                                  = NULL;

    entry->change_pending = false;
    entry->storage_action = DATA_MANAGER_STORAGE_ACTION_NONE;

    /* Copied out so the flash write runs without the lock. */
    if (storage_action == DATA_MANAGER_STORAGE_ACTION_SAVE)
    {
        snapshot = malloc(descriptor->size);

        if (snapshot != NULL)
        {
            memcpy(snapshot, descriptor->value, descriptor->size);
        }
    }

    xSemaphoreGive(data_manager_mutex);

    if (storage_action != DATA_MANAGER_STORAGE_ACTION_NONE)
    {
        data_manager_persist(descriptor, storage_action, snapshot);
    }

    free(snapshot);

    if (changed)
    {
        if (on_change != NULL)
        {
            on_change(descriptor->id);
        }

        /* Zero timeout: a full event queue must not stall the work_queue task. */
        const esp_err_t post_error = esp_event_post(
            DATA_MANAGER_EVENTS,
            DATA_MANAGER_EVENT_CHANGED,
            &descriptor->id,
            sizeof(descriptor->id),
            0u);

        if (post_error != ESP_OK)
        {
            ESP_LOGW(
                data_manager_tag,
                "Change event for %s dropped: %s",
                descriptor->name,
                esp_err_to_name(post_error));
        }
    }
}

/* ---- Registration ---- */

/* Reads into a scratch buffer so a bad record never touches the live value.
 * A record that no longer fits the descriptor (size or type changed in a
 * firmware update) is erased; only that value falls back to its default. */
static void data_manager_load(const data_manager_descriptor_t* descriptor)
{
    esp_err_t error_code  = ESP_OK;
    uint8_t* const buffer = malloc(descriptor->size);
    bool corrupted        = false;

    do
    {
        if (buffer == NULL)
        {
            error_code = ESP_ERR_NO_MEM;
            break;
        }

        error_code = data_manager_storage_load(descriptor->id, buffer, descriptor->size);

        if (error_code == ESP_ERR_INVALID_SIZE)
        {
            corrupted = true;
            break;
        }

        if (error_code != ESP_OK)
        {
            break;
        }

        error_code = data_manager_value_valid(descriptor, buffer, descriptor->size);

        if (error_code != ESP_OK)
        {
            corrupted = true;
            break;
        }

        memcpy(descriptor->value, buffer, descriptor->size);
    } while (0);

    free(buffer);

    if (corrupted)
    {
        ESP_LOGW(
            data_manager_tag,
            "Saved %s rejected (%s), using default",
            descriptor->name,
            esp_err_to_name(error_code));

        const esp_err_t erase_error = data_manager_storage_erase(descriptor->id);

        if (erase_error != ESP_OK)
        {
            ESP_LOGW(
                data_manager_tag,
                "Erasing saved %s failed: %s",
                descriptor->name,
                esp_err_to_name(erase_error));
        }
    }
    else if ((error_code != ESP_OK) && (error_code != ESP_ERR_NOT_FOUND))
    {
        ESP_LOGE(
            data_manager_tag,
            "Loading %s failed (%s), using default",
            descriptor->name,
            esp_err_to_name(error_code));
    }
    else
    {
        /* Loaded, or never saved. */
    }
}

/* Holds the mutex across the NVS read: registration only runs at boot. */
static esp_err_t data_manager_register_one(
    const data_manager_descriptor_t* descriptor,
    data_manager_on_change_t on_change)
{
    esp_err_t error_code = ESP_OK;

    xSemaphoreTake(data_manager_mutex, portMAX_DELAY);

    do
    {
        if (!data_manager_descriptor_valid(descriptor))
        {
            ESP_LOGE(
                data_manager_tag,
                "Invalid descriptor %s",
                (descriptor->name != NULL) ? descriptor->name : "(null)");
            error_code = ESP_ERR_INVALID_ARG;
            break;
        }

        /* Catches a DATA_MANAGER_ID() whose literal differs from the name. */
        if (descriptor->id != hash_string(descriptor->name))
        {
            ESP_LOGE(
                data_manager_tag,
                "Id of %s is not DATA_MANAGER_ID(\"%s\")",
                descriptor->name,
                descriptor->name);
            error_code = ESP_ERR_INVALID_ARG;
            break;
        }

        const size_t current = data_manager_index_find(descriptor->id);

        if (current != DATA_MANAGER_INDEX_NONE)
        {
            ESP_LOGE(
                data_manager_tag,
                "Id of %s already taken by %s",
                descriptor->name,
                data_manager_entries[current].descriptor->name);
            error_code = ESP_ERR_INVALID_STATE;
            break;
        }

        if (data_manager_entry_count >= DATA_MANAGER_CAPACITY)
        {
            ESP_LOGE(data_manager_tag, "No slot left for %s", descriptor->name);
            error_code = ESP_ERR_NO_MEM;
            break;
        }

        data_manager_entry_t* const entry = &data_manager_entries[data_manager_entry_count];

        error_code = work_queue_item_init(&entry->work, data_manager_process, entry);

        if (error_code != ESP_OK)
        {
            break;
        }

        memcpy(descriptor->value, descriptor->default_value, descriptor->size);

        if (descriptor->persistent)
        {
            data_manager_load(descriptor);
        }

        entry->descriptor     = descriptor;
        entry->on_change      = on_change;
        entry->id             = descriptor->id;
        entry->storage_action = DATA_MANAGER_STORAGE_ACTION_NONE;
        entry->change_pending = false;
        entry->unsaved        = false;

        data_manager_index_insert(descriptor->id, data_manager_entry_count);
        data_manager_entry_count++;
    } while (0);

    xSemaphoreGive(data_manager_mutex);

    return error_code;
}


/* Exposed API ------------------------------------------------------------- */

esp_err_t data_manager_init(void)
{
    esp_err_t error_code = ESP_OK;

    do
    {
        if (data_manager_mutex != NULL)
        {
            break;
        }

        data_manager_mutex = xSemaphoreCreateMutex();

        if (data_manager_mutex == NULL)
        {
            error_code = ESP_ERR_NO_MEM;
            break;
        }
    } while (0);

    if (error_code != ESP_OK)
    {
        ESP_LOGE(data_manager_tag, "Init failed: %s", esp_err_to_name(error_code));
    }

    return error_code;
}

esp_err_t data_manager_register(
    const data_manager_descriptor_t* descriptors,
    size_t count,
    data_manager_on_change_t on_change)
{
    esp_err_t error_code = ESP_OK;

    do
    {
        if (data_manager_mutex == NULL)
        {
            error_code = ESP_ERR_INVALID_STATE;
            break;
        }

        if ((descriptors == NULL) || (count == 0u))
        {
            error_code = ESP_ERR_INVALID_ARG;
            break;
        }

        for (size_t i = 0u; i < count; i++)
        {
            error_code = data_manager_register_one(&descriptors[i], on_change);

            if (error_code != ESP_OK)
            {
                break;
            }
        }
    } while (0);

    if (error_code != ESP_OK)
    {
        ESP_LOGE(data_manager_tag, "Register failed: %s", esp_err_to_name(error_code));
    }

    return error_code;
}

/* ---- Settings access ---- */

esp_err_t data_manager_get(uint32_t id, void* value, size_t size)
{
    esp_err_t error_code = ESP_OK;

    do
    {
        if (data_manager_mutex == NULL)
        {
            error_code = ESP_ERR_INVALID_STATE;
            break;
        }

        if (value == NULL)
        {
            error_code = ESP_ERR_INVALID_ARG;
            break;
        }

        xSemaphoreTake(data_manager_mutex, portMAX_DELAY);

        const size_t index = data_manager_index_find(id);

        if (index == DATA_MANAGER_INDEX_NONE)
        {
            error_code = ESP_ERR_NOT_FOUND;
        }
        else
        {
            error_code =
                data_manager_copy_out(data_manager_entries[index].descriptor, value, size);
        }

        xSemaphoreGive(data_manager_mutex);
    } while (0);

    return error_code;
}

esp_err_t data_manager_set(uint32_t id, const void* value, size_t size)
{
    esp_err_t error_code = ESP_OK;

    do
    {
        if (data_manager_mutex == NULL)
        {
            error_code = ESP_ERR_INVALID_STATE;
            break;
        }

        if (value == NULL)
        {
            error_code = ESP_ERR_INVALID_ARG;
            break;
        }

        xSemaphoreTake(data_manager_mutex, portMAX_DELAY);

        const size_t index = data_manager_index_find(id);

        if (index == DATA_MANAGER_INDEX_NONE)
        {
            error_code = ESP_ERR_NOT_FOUND;
        }
        else
        {
            error_code = data_manager_write(index, value, size);
        }

        xSemaphoreGive(data_manager_mutex);
    } while (0);

    return error_code;
}

esp_err_t data_manager_save(void)
{
    esp_err_t error_code = ESP_OK;

    do
    {
        if (data_manager_mutex == NULL)
        {
            error_code = ESP_ERR_INVALID_STATE;
            break;
        }

        xSemaphoreTake(data_manager_mutex, portMAX_DELAY);

        for (size_t i = 0u; i < data_manager_entry_count; i++)
        {
            if (data_manager_entries[i].unsaved)
            {
                data_manager_entries[i].unsaved = false;

                const esp_err_t schedule_error =
                    data_manager_schedule(i, false, DATA_MANAGER_STORAGE_ACTION_SAVE);

                if (error_code == ESP_OK)
                {
                    error_code = schedule_error;
                }
            }
        }

        xSemaphoreGive(data_manager_mutex);
    } while (0);

    if (error_code != ESP_OK)
    {
        ESP_LOGE(data_manager_tag, "Save failed: %s", esp_err_to_name(error_code));
    }

    return error_code;
}

bool data_manager_unsaved(void)
{
    bool unsaved = false;

    if (data_manager_mutex != NULL)
    {
        xSemaphoreTake(data_manager_mutex, portMAX_DELAY);

        for (size_t i = 0u; i < data_manager_entry_count; i++)
        {
            if (data_manager_entries[i].unsaved)
            {
                unsaved = true;
                break;
            }
        }

        xSemaphoreGive(data_manager_mutex);
    }

    return unsaved;
}

esp_err_t data_manager_reset(const data_manager_descriptor_t* descriptors, size_t count)
{
    esp_err_t error_code = ESP_OK;

    do
    {
        if (data_manager_mutex == NULL)
        {
            error_code = ESP_ERR_INVALID_STATE;
            break;
        }

        if (descriptors == NULL)
        {
            error_code = ESP_ERR_INVALID_ARG;
            break;
        }

        xSemaphoreTake(data_manager_mutex, portMAX_DELAY);

        for (size_t i = 0u; i < count; i++)
        {
            const size_t index = data_manager_index_find(descriptors[i].id);

            error_code = (index == DATA_MANAGER_INDEX_NONE) ?
                             ESP_ERR_NOT_FOUND :
                             data_manager_reset_index(index);

            if (error_code != ESP_OK)
            {
                break;
            }
        }

        xSemaphoreGive(data_manager_mutex);
    } while (0);

    if (error_code != ESP_OK)
    {
        ESP_LOGE(data_manager_tag, "Reset failed: %s", esp_err_to_name(error_code));
    }

    return error_code;
}

/* ---- Discovery ---- */

const data_manager_descriptor_t* data_manager_descriptor(uint32_t id)
{
    const data_manager_descriptor_t* descriptor = NULL;

    if (data_manager_mutex != NULL)
    {
        xSemaphoreTake(data_manager_mutex, portMAX_DELAY);

        const size_t index = data_manager_index_find(id);

        if (index != DATA_MANAGER_INDEX_NONE)
        {
            descriptor = data_manager_entries[index].descriptor;
        }

        xSemaphoreGive(data_manager_mutex);
    }

    return descriptor;
}

const data_manager_descriptor_t* data_manager_find(const char* name)
{
    const data_manager_descriptor_t* descriptor =
        (name != NULL) ? data_manager_descriptor(hash_string(name)) : NULL;

    if ((descriptor != NULL) && (strcmp(descriptor->name, name) != 0))
    {
        descriptor = NULL;
    }

    return descriptor;
}

size_t data_manager_count(void)
{
    size_t count = 0u;

    if (data_manager_mutex != NULL)
    {
        xSemaphoreTake(data_manager_mutex, portMAX_DELAY);
        count = data_manager_entry_count;
        xSemaphoreGive(data_manager_mutex);
    }

    return count;
}

const data_manager_descriptor_t* data_manager_at(size_t index)
{
    const data_manager_descriptor_t* descriptor = NULL;

    if (data_manager_mutex != NULL)
    {
        xSemaphoreTake(data_manager_mutex, portMAX_DELAY);

        if (index < data_manager_entry_count)
        {
            descriptor = data_manager_entries[index].descriptor;
        }

        xSemaphoreGive(data_manager_mutex);
    }

    return descriptor;
}

/**** END OF FILE ****/
