/**
 ******************************************************************************
 * @file    data_manager_text.c
 * @author  Daniel Dyringer
 * @date    2026-10-13
 * @brief   Text conversion of data manager values.
 ******************************************************************************
 */


/* Includes ---------------------------------------------------------------- */
#include "data_manager_text.h"

#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>


/* Constants and macros ---------------------------------------------------- */
// clang-format off


// clang-format on

/* Private types ----------------------------------------------------------- */


/* Private variables ------------------------------------------------------- */

static const char* const data_manager_text_type_names[DATA_MANAGER_TYPE_COUNT] = {
    [DATA_MANAGER_TYPE_BOOL]   = "bool",
    [DATA_MANAGER_TYPE_U8]     = "u8",
    [DATA_MANAGER_TYPE_U16]    = "u16",
    [DATA_MANAGER_TYPE_U32]    = "u32",
    [DATA_MANAGER_TYPE_I8]     = "i8",
    [DATA_MANAGER_TYPE_I16]    = "i16",
    [DATA_MANAGER_TYPE_I32]    = "i32",
    [DATA_MANAGER_TYPE_FLOAT]  = "float",
    [DATA_MANAGER_TYPE_STRING] = "string",
    [DATA_MANAGER_TYPE_BLOB]   = "blob",
};


/* Private functions ------------------------------------------------------- */

/* ---- Parsing ---- */

static esp_err_t data_manager_text_parse_bool(const char* text, bool* value)
{
    esp_err_t error_code = ESP_OK;

    if ((strcmp(text, "1") == 0) || (strcasecmp(text, "true") == 0)
        || (strcasecmp(text, "on") == 0))
    {
        *value = true;
    }
    else if (
        (strcmp(text, "0") == 0) || (strcasecmp(text, "false") == 0)
        || (strcasecmp(text, "off") == 0))
    {
        *value = false;
    }
    else
    {
        error_code = ESP_ERR_INVALID_ARG;
    }

    return error_code;
}

/* Base 0 would read a leading zero as octal. */
static int data_manager_text_number_base(const char* text)
{
    const char* const digits = ((text[0] == '-') || (text[0] == '+')) ? &text[1] : text;

    return ((digits[0] == '0') && ((digits[1] == 'x') || (digits[1] == 'X'))) ? 16 : 10;
}

static esp_err_t data_manager_text_parse_unsigned(
    const char* text,
    uint32_t maximum,
    uint32_t* value)
{
    esp_err_t error_code = ESP_ERR_INVALID_ARG;
    char* end            = NULL;

    if (text[0] != '-')
    {
        errno = 0;

        const unsigned long parsed =
            strtoul(text, &end, data_manager_text_number_base(text));

        if ((end != text) && (*end == '\0') && (errno == 0) && (parsed <= maximum))
        {
            *value     = (uint32_t)parsed;
            error_code = ESP_OK;
        }
    }

    return error_code;
}

static esp_err_t data_manager_text_parse_signed(
    const char* text,
    int32_t minimum,
    int32_t maximum,
    int32_t* value)
{
    esp_err_t error_code = ESP_ERR_INVALID_ARG;
    char* end            = NULL;

    errno = 0;

    const long parsed = strtol(text, &end, data_manager_text_number_base(text));

    if ((end != text) && (*end == '\0') && (errno == 0) && (parsed >= minimum)
        && (parsed <= maximum))
    {
        *value     = (int32_t)parsed;
        error_code = ESP_OK;
    }

    return error_code;
}

static esp_err_t data_manager_text_parse_float(const char* text, float* value)
{
    esp_err_t error_code = ESP_ERR_INVALID_ARG;
    char* end            = NULL;

    errno = 0;

    const float parsed = strtof(text, &end);

    if ((end != text) && (*end == '\0') && (errno == 0))
    {
        *value     = parsed;
        error_code = ESP_OK;
    }

    return error_code;
}

static bool data_manager_text_hex_digit(char character, uint8_t* digit)
{
    bool valid = true;

    if ((character >= '0') && (character <= '9'))
    {
        *digit = (uint8_t)(character - '0');
    }
    else if ((character >= 'a') && (character <= 'f'))
    {
        *digit = (uint8_t)((character - 'a') + 10);
    }
    else if ((character >= 'A') && (character <= 'F'))
    {
        *digit = (uint8_t)((character - 'A') + 10);
    }
    else
    {
        valid = false;
    }

    return valid;
}

static esp_err_t data_manager_text_parse_blob(const char* text, size_t size, uint8_t* buffer)
{
    esp_err_t error_code = ESP_OK;

    if (strlen(text) != (size * 2u))
    {
        error_code = ESP_ERR_INVALID_SIZE;
    }

    for (size_t i = 0u; (error_code == ESP_OK) && (i < size); i++)
    {
        uint8_t high = 0u;
        uint8_t low  = 0u;

        if (data_manager_text_hex_digit(text[i * 2u], &high)
            && data_manager_text_hex_digit(text[(i * 2u) + 1u], &low))
        {
            buffer[i] = (uint8_t)((uint8_t)(high << 4u) | low);
        }
        else
        {
            error_code = ESP_ERR_INVALID_ARG;
        }
    }

    return error_code;
}

static esp_err_t data_manager_text_parse_integer(
    data_manager_type_t type,
    const char* text,
    uint8_t* buffer)
{
    esp_err_t error_code    = ESP_OK;
    uint32_t unsigned_value = 0u;
    int32_t signed_value    = 0;

    switch (type)
    {
        case DATA_MANAGER_TYPE_U8:
        {
            error_code = data_manager_text_parse_unsigned(text, UINT8_MAX, &unsigned_value);
            buffer[0] = (uint8_t)unsigned_value;
        }
        break;
        case DATA_MANAGER_TYPE_U16:
        {
            error_code =
                data_manager_text_parse_unsigned(text, UINT16_MAX, &unsigned_value);
            const uint16_t value = (uint16_t)unsigned_value;
            memcpy(buffer, &value, sizeof(value));
        }
        break;
        case DATA_MANAGER_TYPE_U32:
        {
            error_code =
                data_manager_text_parse_unsigned(text, UINT32_MAX, &unsigned_value);
            memcpy(buffer, &unsigned_value, sizeof(unsigned_value));
        }
        break;
        case DATA_MANAGER_TYPE_I8:
        {
            error_code =
                data_manager_text_parse_signed(text, INT8_MIN, INT8_MAX, &signed_value);
            const int8_t value = (int8_t)signed_value;
            memcpy(buffer, &value, sizeof(value));
        }
        break;
        case DATA_MANAGER_TYPE_I16:
        {
            error_code =
                data_manager_text_parse_signed(text, INT16_MIN, INT16_MAX, &signed_value);
            const int16_t value = (int16_t)signed_value;
            memcpy(buffer, &value, sizeof(value));
        }
        break;
        case DATA_MANAGER_TYPE_I32:
        {
            error_code =
                data_manager_text_parse_signed(text, INT32_MIN, INT32_MAX, &signed_value);
            memcpy(buffer, &signed_value, sizeof(signed_value));
        }
        break;
        default:
        {
            error_code = ESP_ERR_NOT_SUPPORTED;
        }
        break;
    }

    return error_code;
}

/* ---- Formatting ---- */

static esp_err_t data_manager_text_written(int written, size_t text_size)
{
    esp_err_t error_code = ESP_OK;

    if (written < 0)
    {
        error_code = ESP_FAIL;
    }
    else if ((size_t)written >= text_size)
    {
        error_code = ESP_ERR_INVALID_SIZE;
    }
    else
    {
        /* Fits with its terminator. */
    }

    return error_code;
}

static esp_err_t data_manager_text_format_blob(
    const uint8_t* buffer,
    size_t size,
    char* text,
    size_t text_size)
{
    esp_err_t error_code = ESP_OK;
    size_t i             = 0u;

    text[0] = '\0';

    while ((i < size) && (((i * 2u) + 2u) < text_size))
    {
        snprintf(&text[i * 2u], 3u, "%02x", buffer[i]);
        i++;
    }

    if (i < size)
    {
        error_code = ESP_ERR_INVALID_SIZE;
    }

    return error_code;
}

static esp_err_t data_manager_text_format_scalar(
    data_manager_type_t type,
    const uint8_t* buffer,
    char* text,
    size_t text_size)
{
    esp_err_t error_code = ESP_OK;
    int written          = 0;

    switch (type)
    {
        case DATA_MANAGER_TYPE_BOOL:
        {
            bool value = false;
            memcpy(&value, buffer, sizeof(value));
            written = snprintf(text, text_size, "%s", value ? "true" : "false");
        }
        break;
        case DATA_MANAGER_TYPE_U8:
        {
            uint8_t value = 0u;
            memcpy(&value, buffer, sizeof(value));
            written = snprintf(text, text_size, "%u", (unsigned)value);
        }
        break;
        case DATA_MANAGER_TYPE_U16:
        {
            uint16_t value = 0u;
            memcpy(&value, buffer, sizeof(value));
            written = snprintf(text, text_size, "%u", (unsigned)value);
        }
        break;
        case DATA_MANAGER_TYPE_U32:
        {
            uint32_t value = 0u;
            memcpy(&value, buffer, sizeof(value));
            written = snprintf(text, text_size, "%lu", (unsigned long)value);
        }
        break;
        case DATA_MANAGER_TYPE_I8:
        {
            int8_t value = 0;
            memcpy(&value, buffer, sizeof(value));
            written = snprintf(text, text_size, "%d", (int)value);
        }
        break;
        case DATA_MANAGER_TYPE_I16:
        {
            int16_t value = 0;
            memcpy(&value, buffer, sizeof(value));
            written = snprintf(text, text_size, "%d", (int)value);
        }
        break;
        case DATA_MANAGER_TYPE_I32:
        {
            int32_t value = 0;
            memcpy(&value, buffer, sizeof(value));
            written = snprintf(text, text_size, "%ld", (long)value);
        }
        break;
        case DATA_MANAGER_TYPE_FLOAT:
        {
            float value = 0.0f;
            memcpy(&value, buffer, sizeof(value));
            written = snprintf(text, text_size, "%g", (double)value);
        }
        break;
        default:
        {
            snprintf(text, text_size, "?");
            error_code = ESP_ERR_NOT_SUPPORTED;
        }
        break;
    }

    if (error_code == ESP_OK)
    {
        error_code = data_manager_text_written(written, text_size);
    }

    return error_code;
}


/* Exposed API ------------------------------------------------------------- */

const char* data_manager_text_type_name(data_manager_type_t type)
{
    return ((uint32_t)type < (uint32_t)DATA_MANAGER_TYPE_COUNT) ?
               data_manager_text_type_names[type] :
               "?";
}

size_t data_manager_text_size(const data_manager_descriptor_t* descriptor)
{
    size_t size = DATA_MANAGER_TEXT_SCALAR_MAX;

    if (descriptor->type == DATA_MANAGER_TYPE_STRING)
    {
        size = descriptor->size;
    }
    else if (descriptor->type == DATA_MANAGER_TYPE_BLOB)
    {
        size = (descriptor->size * 2u) + 1u;
    }
    else
    {
        /* Numbers and booleans. */
    }

    return size;
}

esp_err_t data_manager_text_parse(
    const data_manager_descriptor_t* descriptor,
    const char* text,
    uint8_t* buffer,
    size_t* size)
{
    esp_err_t error_code = ESP_OK;

    *size = descriptor->size;

    switch (descriptor->type)
    {
        case DATA_MANAGER_TYPE_BOOL:
        {
            bool value = false;
            error_code = data_manager_text_parse_bool(text, &value);
            memcpy(buffer, &value, sizeof(value));
        }
        break;
        case DATA_MANAGER_TYPE_FLOAT:
        {
            float value = 0.0f;
            error_code  = data_manager_text_parse_float(text, &value);
            memcpy(buffer, &value, sizeof(value));
        }
        break;
        case DATA_MANAGER_TYPE_STRING:
        {
            const size_t length = strlen(text);

            if (length >= descriptor->size)
            {
                error_code = ESP_ERR_INVALID_SIZE;
            }
            else
            {
                strlcpy((char*)buffer, text, descriptor->size);
                *size = length + 1u;
            }
        }
        break;
        case DATA_MANAGER_TYPE_BLOB:
        {
            error_code = data_manager_text_parse_blob(text, descriptor->size, buffer);
        }
        break;
        default:
        {
            error_code = data_manager_text_parse_integer(descriptor->type, text, buffer);
        }
        break;
    }

    return error_code;
}

esp_err_t data_manager_text_format(
    const data_manager_descriptor_t* descriptor,
    const uint8_t* buffer,
    char* text,
    size_t text_size)
{
    esp_err_t error_code = ESP_OK;

    if (text_size == 0u)
    {
        error_code = ESP_ERR_INVALID_SIZE;
    }
    else if (descriptor->type == DATA_MANAGER_TYPE_STRING)
    {
        const int written =
            snprintf(text, text_size, "%.*s", (int)descriptor->size, (const char*)buffer);

        error_code = data_manager_text_written(written, text_size);
    }
    else if (descriptor->type == DATA_MANAGER_TYPE_BLOB)
    {
        error_code =
            data_manager_text_format_blob(buffer, descriptor->size, text, text_size);
    }
    else
    {
        error_code =
            data_manager_text_format_scalar(descriptor->type, buffer, text, text_size);
    }

    return error_code;
}

/**** END OF FILE ****/
