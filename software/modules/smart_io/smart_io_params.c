/**
 ******************************************************************************
 * @file    smart_io_params.c
 * @author  Daniel Dyringer
 * @date    2026-10-31
 * @brief   Role and parameter tables, parse, format, range checks.
 ******************************************************************************
 */


/* Includes ---------------------------------------------------------------- */
#include "smart_io_params.h"

#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "smart_io.h"
#include "utils.h"


/* Constants and macros ---------------------------------------------------- */
// clang-format off


// clang-format on

/* Private types ----------------------------------------------------------- */


/* Private variables ------------------------------------------------------- */


/* Private functions ------------------------------------------------------- */

static esp_err_t smart_io_params_parse_u32(const char* text, uint32_t* value)
{
    esp_err_t error_code = ESP_ERR_INVALID_ARG;
    char* end            = NULL;

    if ((text[0] >= '0') && (text[0] <= '9'))
    {
        errno                      = 0;
        const unsigned long parsed = strtoul(text, &end, 10);

        if ((errno == 0) && (*end == '\0') && (parsed <= UINT32_MAX))
        {
            *value     = (uint32_t)parsed;
            error_code = ESP_OK;
        }
    }

    return error_code;
}

static esp_err_t smart_io_params_parse_bool(const char* text, uint32_t* value)
{
    esp_err_t error_code = ESP_OK;

    if ((strcasecmp(text, "true") == 0) || (strcasecmp(text, "on") == 0)
        || (strcmp(text, "1") == 0))
    {
        *value = 1u;
    }
    else if (
        (strcasecmp(text, "false") == 0) || (strcasecmp(text, "off") == 0)
        || (strcmp(text, "0") == 0))
    {
        *value = 0u;
    }
    else
    {
        error_code = ESP_ERR_INVALID_ARG;
    }

    return error_code;
}

static esp_err_t smart_io_params_parse_enum(
    const smart_io_param_descriptor_t* descriptor,
    const char* text,
    uint32_t* value)
{
    esp_err_t error_code = ESP_ERR_INVALID_ARG;

    for (uint32_t i = 0u; (i <= descriptor->max) && (error_code != ESP_OK); i++)
    {
        if (strcasecmp(text, descriptor->option_names[i]) == 0)
        {
            *value     = i;
            error_code = ESP_OK;
        }
    }

    return error_code;
}

static void smart_io_params_options(
    const smart_io_param_descriptor_t* descriptor,
    char* buffer,
    size_t size)
{
    size_t length = 0u;

    buffer[0] = '\0';

    for (uint32_t i = 0u; (i <= descriptor->max) && (length < size); i++)
    {
        const int written = snprintf(
            &buffer[length],
            size - length,
            "%s%s",
            (i == 0u) ? "" : ", ",
            descriptor->option_names[i]);

        if (written > 0)
        {
            length += (size_t)written;
        }
    }
}


/* Exposed API ------------------------------------------------------------- */

void smart_io_params_defaults(
    const smart_io_role_description_t* description,
    uint32_t param_values[SMART_IO_PARAM_MAX])
{
    for (size_t i = 0u; i < SMART_IO_PARAM_MAX; i++)
    {
        param_values[i] = (i < description->param_count) ?
                              description->param_descriptors[i].default_value :
                              0u;
    }
}

esp_err_t smart_io_params_find(
    const smart_io_role_description_t* description,
    const char* name,
    size_t* index)
{
    esp_err_t error_code = ESP_ERR_NOT_FOUND;

    for (size_t i = 0u; (i < description->param_count) && (error_code != ESP_OK); i++)
    {
        if (strcmp(description->param_descriptors[i].name, name) == 0)
        {
            *index     = i;
            error_code = ESP_OK;
        }
    }

    return error_code;
}

esp_err_t smart_io_params_check(
    const smart_io_role_description_t* description,
    const uint32_t param_values[SMART_IO_PARAM_MAX],
    smart_io_reason_t* reason)
{
    esp_err_t error_code = ESP_OK;

    for (size_t i = 0u; (i < description->param_count) && (error_code == ESP_OK); i++)
    {
        const smart_io_param_descriptor_t* const descriptor =
            &description->param_descriptors[i];
        const uint32_t value = param_values[i];

        if ((value < descriptor->min) || (value > descriptor->max))
        {
            error_code = ESP_ERR_INVALID_ARG;

            if (descriptor->type == SMART_IO_PARAM_TYPE_U32)
            {
                smart_io_reason_set(
                    reason,
                    "%s must be %lu-%lu%s%s",
                    descriptor->name,
                    (unsigned long)descriptor->min,
                    (unsigned long)descriptor->max,
                    (descriptor->unit != NULL) ? " " : "",
                    (descriptor->unit != NULL) ? descriptor->unit : "");
            }
            else
            {
                smart_io_reason_set(
                    reason, "%s: invalid value %lu", descriptor->name, (unsigned long)value);
            }
        }
    }

    return error_code;
}

esp_err_t smart_io_params_parse(
    const smart_io_param_descriptor_t* descriptor,
    const char* text,
    uint32_t* value,
    smart_io_reason_t* reason)
{
    esp_err_t error_code = ESP_ERR_INVALID_ARG;

    switch (descriptor->type)
    {
        case SMART_IO_PARAM_TYPE_BOOL:
        {
            error_code = smart_io_params_parse_bool(text, value);

            if (error_code != ESP_OK)
            {
                smart_io_reason_set(reason, "%s must be true or false", descriptor->name);
            }
        }
        break;
        case SMART_IO_PARAM_TYPE_ENUM:
        {
            error_code = smart_io_params_parse_enum(descriptor, text, value);

            if (error_code != ESP_OK)
            {
                char options[SMART_IO_REASON_SIZE] = {0};

                smart_io_params_options(descriptor, options, sizeof(options));
                smart_io_reason_set(
                    reason, "%s must be one of: %s", descriptor->name, options);
            }
        }
        break;
        default:
        {
            error_code = smart_io_params_parse_u32(text, value);

            if (error_code != ESP_OK)
            {
                smart_io_reason_set(reason, "%s must be a number", descriptor->name);
            }
        }
        break;
    }

    return error_code;
}

size_t smart_io_params_format(
    const smart_io_param_descriptor_t* descriptor,
    uint32_t value,
    char* buffer,
    size_t size)
{
    int written = 0;

    if ((descriptor->type == SMART_IO_PARAM_TYPE_BOOL) && (value <= 1u))
    {
        written = snprintf(buffer, size, "%s", (value != 0u) ? "true" : "false");
    }
    else if ((descriptor->type == SMART_IO_PARAM_TYPE_ENUM) && (value <= descriptor->max))
    {
        written = snprintf(buffer, size, "%s", descriptor->option_names[value]);
    }
    else
    {
        written = snprintf(buffer, size, "%lu", (unsigned long)value);
    }

    return (written > 0) ? MIN((size_t)written, (size > 0u) ? (size - 1u) : 0u) : 0u;
}

/**** END OF FILE ****/
