/**
 ******************************************************************************
 * @file    fake_nvs.c
 * @author  Daniel Dyringer
 * @date    2026-10-28
 * @brief   Host NVS fake: nvs.h on a small RAM store, and the RTC memory
 *          wipe.
 ******************************************************************************
 */


/* Includes ---------------------------------------------------------------- */
#include "fake_nvs.h"

#include <string.h>

#include "nvs.h"
#include "unity.h"
#include "utils.h"


/* Constants and macros ---------------------------------------------------- */
// clang-format off

#define FAKE_NVS_ENTRIES        8u
#define FAKE_NVS_HANDLES        4u
#define FAKE_NVS_NAME_SIZE      16u
#define FAKE_NVS_BLOB_SIZE      1024u

// clang-format on

/* Private types ----------------------------------------------------------- */

typedef struct fake_nvs_entry_s
{
    char name[FAKE_NVS_NAME_SIZE];
    char key[FAKE_NVS_NAME_SIZE];
    uint8_t data[FAKE_NVS_BLOB_SIZE];
    size_t length;
    bool used;
    bool is_u8;
} fake_nvs_entry_t;

typedef struct fake_nvs_handle_s
{
    char name[FAKE_NVS_NAME_SIZE];
    bool used;
    bool writable;
} fake_nvs_handle_t;

/* Private variables ------------------------------------------------------- */

static fake_nvs_entry_t fake_nvs_entries[FAKE_NVS_ENTRIES];
static fake_nvs_handle_t fake_nvs_handles[FAKE_NVS_HANDLES];
static esp_err_t fake_nvs_write_error = ESP_OK;
static uint32_t fake_nvs_writes       = 0u;

/* GNU ld defines these for a section whose name is a C identifier; weak so
 * that executables without an RTC variable still link. */


/* Private functions ------------------------------------------------------- */

static fake_nvs_entry_t* fake_nvs_find(const char* name, const char* key)
{
    fake_nvs_entry_t* found = NULL;

    for (size_t i = 0u; i < FAKE_NVS_ENTRIES; i++)
    {
        if (fake_nvs_entries[i].used && (strcmp(fake_nvs_entries[i].name, name) == 0)
            && (strcmp(fake_nvs_entries[i].key, key) == 0))
        {
            found = &fake_nvs_entries[i];
        }
    }

    return found;
}

static fake_nvs_entry_t* fake_nvs_create(const char* name, const char* key)
{
    fake_nvs_entry_t* entry = fake_nvs_find(name, key);

    for (size_t i = 0u; (entry == NULL) && (i < FAKE_NVS_ENTRIES); i++)
    {
        if (!fake_nvs_entries[i].used)
        {
            entry = &fake_nvs_entries[i];
            memset(entry, 0, sizeof(*entry));
            entry->used = true;
            strlcpy(entry->name, name, sizeof(entry->name));
            strlcpy(entry->key, key, sizeof(entry->key));
        }
    }

    TEST_ASSERT_NOT_NULL(entry);

    return entry;
}

static fake_nvs_handle_t* fake_nvs_handle(nvs_handle_t handle)
{
    fake_nvs_handle_t* result = NULL;

    if ((handle >= 1u) && (handle <= FAKE_NVS_HANDLES) && fake_nvs_handles[handle - 1u].used)
    {
        result = &fake_nvs_handles[handle - 1u];
    }

    return result;
}

static bool fake_nvs_namespace_exists(const char* name)
{
    bool found = false;

    for (size_t i = 0u; i < FAKE_NVS_ENTRIES; i++)
    {
        if (fake_nvs_entries[i].used && (strcmp(fake_nvs_entries[i].name, name) == 0))
        {
            found = true;
        }
    }

    return found;
}


/* Exposed API ------------------------------------------------------------- */

void fake_nvs_reset(void)
{
    memset(fake_nvs_entries, 0, sizeof(fake_nvs_entries));
    memset(fake_nvs_handles, 0, sizeof(fake_nvs_handles));
    fake_nvs_write_error = ESP_OK;
    fake_nvs_writes      = 0u;
}

void fake_nvs_blob_put(const char* name, const char* key, const void* value, size_t length)
{
    fake_nvs_entry_t* const entry = fake_nvs_create(name, key);

    TEST_ASSERT_TRUE(length <= FAKE_NVS_BLOB_SIZE);
    memcpy(entry->data, value, length);
    entry->length = length;
    entry->is_u8  = false;
}

size_t fake_nvs_blob_get(const char* name, const char* key, void* value, size_t capacity)
{
    const fake_nvs_entry_t* const entry = fake_nvs_find(name, key);
    size_t length                       = 0u;

    if ((entry != NULL) && !entry->is_u8)
    {
        length = entry->length;
        memcpy(value, entry->data, MIN(length, capacity));
    }

    return length;
}

void fake_nvs_u8_put(const char* name, const char* key, uint8_t value)
{
    fake_nvs_entry_t* const entry = fake_nvs_create(name, key);

    entry->data[0] = value;
    entry->length  = 1u;
    entry->is_u8   = true;
}

bool fake_nvs_u8_get(const char* name, const char* key, uint8_t* value)
{
    const fake_nvs_entry_t* const entry = fake_nvs_find(name, key);
    const bool found                    = (entry != NULL) && entry->is_u8;

    if (found)
    {
        *value = entry->data[0];
    }

    return found;
}

void fake_nvs_fail_writes(esp_err_t code)
{
    fake_nvs_write_error = code;
}

uint32_t fake_nvs_write_count(void)
{
    return fake_nvs_writes;
}

esp_err_t nvs_open(const char* name, nvs_open_mode_t open_mode, nvs_handle_t* out_handle)
{
    esp_err_t error_code = ESP_ERR_NO_MEM;

    if ((open_mode == NVS_READONLY) && !fake_nvs_namespace_exists(name))
    {
        error_code = ESP_ERR_NVS_NOT_FOUND;
    }

    for (size_t i = 0u; (error_code == ESP_ERR_NO_MEM) && (i < FAKE_NVS_HANDLES); i++)
    {
        if (!fake_nvs_handles[i].used)
        {
            fake_nvs_handles[i].used     = true;
            fake_nvs_handles[i].writable = (open_mode == NVS_READWRITE);
            strlcpy(fake_nvs_handles[i].name, name, sizeof(fake_nvs_handles[i].name));
            *out_handle = (nvs_handle_t)(i + 1u);
            error_code  = ESP_OK;
        }
    }

    return error_code;
}

void nvs_close(nvs_handle_t handle)
{
    fake_nvs_handle_t* const open = fake_nvs_handle(handle);

    TEST_ASSERT_NOT_NULL(open);
    open->used = false;
}

esp_err_t nvs_commit(nvs_handle_t handle)
{
    const fake_nvs_handle_t* const open = fake_nvs_handle(handle);

    TEST_ASSERT_NOT_NULL(open);

    return fake_nvs_write_error;
}

esp_err_t nvs_get_blob(nvs_handle_t handle, const char* key, void* out_value, size_t* length)
{
    esp_err_t error_code                = ESP_OK;
    const fake_nvs_handle_t* const open = fake_nvs_handle(handle);

    TEST_ASSERT_NOT_NULL(open);

    const fake_nvs_entry_t* const entry = fake_nvs_find(open->name, key);

    if ((entry == NULL) || entry->is_u8)
    {
        error_code = ESP_ERR_NVS_NOT_FOUND;
    }
    else if (out_value == NULL)
    {
        *length = entry->length;
    }
    else if (*length < entry->length)
    {
        *length    = entry->length;
        error_code = ESP_ERR_NVS_INVALID_LENGTH;
    }
    else
    {
        memcpy(out_value, entry->data, entry->length);
        *length = entry->length;
    }

    return error_code;
}

esp_err_t nvs_set_blob(nvs_handle_t handle, const char* key, const void* value, size_t length)
{
    esp_err_t error_code                = fake_nvs_write_error;
    const fake_nvs_handle_t* const open = fake_nvs_handle(handle);

    TEST_ASSERT_NOT_NULL(open);
    TEST_ASSERT_TRUE(open->writable);

    if (error_code == ESP_OK)
    {
        fake_nvs_blob_put(open->name, key, value, length);
        fake_nvs_writes++;
    }

    return error_code;
}

esp_err_t nvs_get_u8(nvs_handle_t handle, const char* key, uint8_t* out_value)
{
    const fake_nvs_handle_t* const open = fake_nvs_handle(handle);

    TEST_ASSERT_NOT_NULL(open);

    return fake_nvs_u8_get(open->name, key, out_value) ? ESP_OK : ESP_ERR_NVS_NOT_FOUND;
}

esp_err_t nvs_set_u8(nvs_handle_t handle, const char* key, uint8_t value)
{
    esp_err_t error_code                = fake_nvs_write_error;
    const fake_nvs_handle_t* const open = fake_nvs_handle(handle);

    TEST_ASSERT_NOT_NULL(open);
    TEST_ASSERT_TRUE(open->writable);

    if (error_code == ESP_OK)
    {
        fake_nvs_u8_put(open->name, key, value);
        fake_nvs_writes++;
    }

    return error_code;
}

esp_err_t nvs_erase_key(nvs_handle_t handle, const char* key)
{
    esp_err_t error_code                = fake_nvs_write_error;
    const fake_nvs_handle_t* const open = fake_nvs_handle(handle);

    TEST_ASSERT_NOT_NULL(open);

    if (error_code == ESP_OK)
    {
        fake_nvs_entry_t* const entry = fake_nvs_find(open->name, key);

        if (entry != NULL)
        {
            entry->used = false;
        }
        else
        {
            error_code = ESP_ERR_NVS_NOT_FOUND;
        }
    }

    return error_code;
}

/**** END OF FILE ****/
