/**
 ******************************************************************************
 * @file    smart_io_json.c
 * @author  Daniel Dyringer
 * @date    2026-10-31
 * @brief   smart_io control protocol (JSON).
 ******************************************************************************
 */


/* Includes ---------------------------------------------------------------- */
#include "smart_io_json.h"

#include <math.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "cJSON.h"
#include "esp_app_desc.h"
#include "esp_attr.h"
#include "sdkconfig.h"
#include "smart_io.h"
#include "smart_io_hex.h"
#include "smart_io_params.h"
#include "utils.h"


/* Constants and macros ---------------------------------------------------- */
// clang-format off

/* Version of the control messages and of the stream frames (SMART_IO2.md §8). */
#define SMART_IO_JSON_PROTOCOL          1u

#if CONFIG_COMPILER_OPTIMIZATION_DEBUG || CONFIG_COMPILER_OPTIMIZATION_NONE
#define SMART_IO_JSON_DEBUG_BUILD       true
#else
#define SMART_IO_JSON_DEBUG_BUILD       false
#endif

/* Bytes per transfer request: tx is bounded by the request text, rx by the
 * response (hex doubles it). */
#define SMART_IO_JSON_TX_TOTAL_MAX      (SMART_IO_JSON_REQUEST_MAX / 2u)
#define SMART_IO_JSON_RX_TOTAL_MAX      2048u
/* No transfer failed. */
#define SMART_IO_JSON_TRANSFER_NONE     SIZE_MAX

// clang-format on

/* Private types ----------------------------------------------------------- */

typedef enum smart_io_json_command_e
{
    SMART_IO_JSON_COMMAND_HELLO    = 0,
    SMART_IO_JSON_COMMAND_ROLES    = 1,
    SMART_IO_JSON_COMMAND_CONFIG   = 2,
    SMART_IO_JSON_COMMAND_RELEASE  = 3,
    SMART_IO_JSON_COMMAND_READ     = 4,
    SMART_IO_JSON_COMMAND_WRITE    = 5,
    SMART_IO_JSON_COMMAND_SAVE     = 6,
    SMART_IO_JSON_COMMAND_REVERT   = 7,
    SMART_IO_JSON_COMMAND_STREAM   = 8,
    SMART_IO_JSON_COMMAND_UNSTREAM = 9,
    SMART_IO_JSON_COMMAND_TRANSFER = 10,
    SMART_IO_JSON_COMMAND_DISCOVER = 11,

    SMART_IO_JSON_COMMAND_COUNT,
} smart_io_json_command_t;

typedef struct smart_io_json_writer_s
{
    char* buffer;
    size_t size;
    size_t length;
    bool overflow;
} smart_io_json_writer_t;


/* Private variables ------------------------------------------------------- */

static const char* const smart_io_json_command_names[SMART_IO_JSON_COMMAND_COUNT] = {
    [SMART_IO_JSON_COMMAND_HELLO]    = "hello",
    [SMART_IO_JSON_COMMAND_ROLES]    = "roles",
    [SMART_IO_JSON_COMMAND_CONFIG]   = "config",
    [SMART_IO_JSON_COMMAND_RELEASE]  = "release",
    [SMART_IO_JSON_COMMAND_READ]     = "read",
    [SMART_IO_JSON_COMMAND_WRITE]    = "write",
    [SMART_IO_JSON_COMMAND_SAVE]     = "save",
    [SMART_IO_JSON_COMMAND_REVERT]   = "revert",
    [SMART_IO_JSON_COMMAND_STREAM]   = "stream",
    [SMART_IO_JSON_COMMAND_UNSTREAM] = "unstream",
    [SMART_IO_JSON_COMMAND_TRANSFER] = "transfer",
    [SMART_IO_JSON_COMMAND_DISCOVER] = "discover",
};

static const char* const smart_io_json_param_type_names[] = {
    [SMART_IO_PARAM_TYPE_U32]  = "u32",
    [SMART_IO_PARAM_TYPE_BOOL] = "bool",
    [SMART_IO_PARAM_TYPE_ENUM] = "enum",
};

static const char* const smart_io_json_stream_kind_names[] = {
    [SMART_IO_STREAM_KIND_NONE]    = "none",
    [SMART_IO_STREAM_KIND_DIGITAL] = "digital",
    [SMART_IO_STREAM_KIND_SERIES]  = "series",
    [SMART_IO_STREAM_KIND_BYTES]   = "bytes",
    [SMART_IO_STREAM_KIND_VALUE]   = "value",
};

/* Transfer data of one request. Only the sio_ws task runs requests. */
EXT_RAM_BSS_ATTR static uint8_t smart_io_json_tx_data[SMART_IO_JSON_TX_TOTAL_MAX];
EXT_RAM_BSS_ATTR static uint8_t smart_io_json_rx_data[SMART_IO_JSON_RX_TOTAL_MAX];


/* Private functions ------------------------------------------------------- */

/* ---- Writer ---- */

static void smart_io_json_writer_init(smart_io_json_writer_t* writer, char* buffer, size_t size)
{
    writer->buffer   = buffer;
    writer->size     = size;
    writer->length   = 0u;
    writer->overflow = false;

    if (size > 0u)
    {
        buffer[0] = '\0';
    }
}

static void smart_io_json_append(smart_io_json_writer_t* writer, const char* fmt, ...)
    __attribute__((format(printf, 2, 3)));

static void smart_io_json_append(smart_io_json_writer_t* writer, const char* fmt, ...)
{
    if (!writer->overflow)
    {
        const size_t room = writer->size - writer->length;
        va_list args;

        va_start(args, fmt);
        const int written = vsnprintf(&writer->buffer[writer->length], room, fmt, args);
        va_end(args);

        if ((written < 0) || ((size_t)written >= room))
        {
            writer->overflow = true;
        }
        else
        {
            writer->length += (size_t)written;
        }
    }
}

/* Drops everything written after length, overflow included. */
static void smart_io_json_rewind(smart_io_json_writer_t* writer, size_t length)
{
    writer->length         = length;
    writer->overflow       = false;
    writer->buffer[length] = '\0';
}

static void smart_io_json_append_string(smart_io_json_writer_t* writer, const char* text)
{
    smart_io_json_append(writer, "\"");

    for (size_t i = 0u; (text[i] != '\0') && !writer->overflow; i++)
    {
        const unsigned char character = (unsigned char)text[i];

        if ((character == (unsigned char)'"') || (character == (unsigned char)'\\'))
        {
            smart_io_json_append(writer, "\\%c", (char)character);
        }
        else if (character < 0x20u)
        {
            smart_io_json_append(writer, "\\u%04x", (unsigned)character);
        }
        else
        {
            smart_io_json_append(writer, "%c", (char)character);
        }
    }

    smart_io_json_append(writer, "\"");
}

static void smart_io_json_append_hex(
    smart_io_json_writer_t* writer,
    const uint8_t* bytes,
    size_t length)
{
    if (!writer->overflow)
    {
        char* const text = &writer->buffer[writer->length];

        if (smart_io_hex_encode(bytes, length, text, writer->size - writer->length)
            == ESP_OK)
        {
            writer->length += 2u * length;
        }
        else
        {
            writer->overflow = true;
        }
    }
}

/* ---- Request fields ---- */

static bool smart_io_json_get_integer(const cJSON* item, double min, double max, int64_t* value)
{
    bool valid = false;

    if (cJSON_IsNumber(item))
    {
        const double number = item->valuedouble;

        if ((number >= min) && (number <= max) && (floor(number) == number))
        {
            *value = (int64_t)number;
            valid  = true;
        }
    }

    return valid;
}

static esp_err_t smart_io_json_gpio_parse(
    const cJSON* item,
    gpio_num_t* gpio,
    smart_io_reason_t* reason)
{
    esp_err_t error_code = ESP_OK;
    int64_t number       = 0;

    if (smart_io_json_get_integer(item, 0.0, (double)(GPIO_NUM_MAX - 1), &number))
    {
        *gpio = (gpio_num_t)number;
    }
    else
    {
        error_code = ESP_ERR_INVALID_ARG;
        smart_io_reason_set(reason, "gpio must be a GPIO number");
    }

    return error_code;
}

/* Addressed by "gpio" or by "label". */
static esp_err_t smart_io_json_resolve_pin(
    const cJSON* root,
    gpio_num_t* gpio,
    smart_io_reason_t* reason)
{
    esp_err_t error_code     = ESP_OK;
    const cJSON* const label = cJSON_GetObjectItemCaseSensitive(root, "label");
    const cJSON* const pin   = cJSON_GetObjectItemCaseSensitive(root, "gpio");

    if (cJSON_IsString(label))
    {
        error_code = smart_io_lookup(label->valuestring, gpio);

        if (error_code != ESP_OK)
        {
            smart_io_reason_set(reason, "No pin labelled %s", label->valuestring);
        }
    }
    else if (pin != NULL)
    {
        error_code = smart_io_json_gpio_parse(pin, gpio, reason);
    }
    else
    {
        error_code = ESP_ERR_INVALID_ARG;
        smart_io_reason_set(reason, "gpio or label required");
    }

    return error_code;
}

static esp_err_t smart_io_json_param_parse(
    const cJSON* item,
    const smart_io_param_descriptor_t* descriptor,
    uint32_t* value,
    smart_io_reason_t* reason)
{
    esp_err_t error_code = ESP_OK;
    int64_t number       = 0;

    if (cJSON_IsString(item))
    {
        error_code = smart_io_params_parse(descriptor, item->valuestring, value, reason);
    }
    else if (cJSON_IsBool(item) && (descriptor->type == SMART_IO_PARAM_TYPE_BOOL))
    {
        *value = cJSON_IsTrue(item) ? 1u : 0u;
    }
    else if (
        smart_io_json_get_integer(item, 0.0, (double)UINT32_MAX, &number)
        && (descriptor->type == SMART_IO_PARAM_TYPE_U32))
    {
        *value = (uint32_t)number;
    }
    else
    {
        error_code = ESP_ERR_INVALID_ARG;
        smart_io_reason_set(
            reason,
            "%s must be a %s",
            descriptor->name,
            smart_io_json_param_type_names[descriptor->type]);
    }

    return error_code;
}

/* ---- Output pieces ---- */

static void smart_io_json_append_functions(smart_io_json_writer_t* writer, uint32_t caps)
{
    bool first = true;

    smart_io_json_append(writer, "[");

    for (uint32_t i = 1u; i < (uint32_t)SMART_IO_FUNCTION_COUNT; i++)
    {
        if ((caps & SMART_IO_CAP(i)) != 0u)
        {
            smart_io_json_append(writer, "%s", first ? "" : ",");
            smart_io_json_append_string(
                writer, smart_io_function_name((smart_io_function_t)i));
            first = false;
        }
    }

    smart_io_json_append(writer, "]");
}

static void smart_io_json_param_value(
    smart_io_json_writer_t* writer,
    const smart_io_param_descriptor_t* descriptor,
    uint32_t value)
{
    char text[SMART_IO_PARAMS_TEXT_SIZE] = {0};

    smart_io_params_format(descriptor, value, text, sizeof(text));

    if (descriptor->type == SMART_IO_PARAM_TYPE_ENUM)
    {
        smart_io_json_append_string(writer, text);
    }
    else
    {
        smart_io_json_append(writer, "%s", text);
    }
}

/* {"gpio":[..],"role":..,"label":..,"params":{..}} without the closing brace,
 * so the caller can add fields. */
static void smart_io_json_append_config_begin(
    smart_io_json_writer_t* writer,
    const smart_io_config_t* config)
{
    const smart_io_role_info_t* const info = smart_io_role_info(config->role);

    smart_io_json_append(writer, "{\"gpio\":[%d", (int)config->gpio[0]);

    if (config->gpio[1] != GPIO_NUM_NC)
    {
        smart_io_json_append(writer, ",%d", (int)config->gpio[1]);
    }

    smart_io_json_append(writer, "],\"role\":");
    smart_io_json_append_string(writer, info->description->name);
    smart_io_json_append(writer, ",\"label\":");
    smart_io_json_append_string(writer, config->label);
    smart_io_json_append(writer, ",\"params\":{");

    for (size_t i = 0u; i < info->description->param_count; i++)
    {
        const smart_io_param_descriptor_t* const descriptor =
            &info->description->param_descriptors[i];

        smart_io_json_append(writer, "%s", (i == 0u) ? "" : ",");
        smart_io_json_append_string(writer, descriptor->name);
        smart_io_json_append(writer, ":");
        smart_io_json_param_value(writer, descriptor, config->param_values[i]);
    }

    smart_io_json_append(writer, "}");
}

/* True for the first pin of an instance, so a bus is listed once. */
static bool smart_io_json_first_pin_config(gpio_num_t gpio, smart_io_config_t* config)
{
    return (smart_io_config_get(gpio, config) == ESP_OK) && (config->gpio[0] == gpio);
}

static void smart_io_json_append_streams(smart_io_json_writer_t* writer)
{
    size_t count                     = 0u;
    const smart_io_pin_t* const pins = smart_io_pins(&count);
    const uint32_t streams           = smart_io_streams();
    const char* separator            = "";

    smart_io_json_append(writer, "\"streams\":[");

    for (size_t i = 0u; i < count; i++)
    {
        if ((streams & ((uint32_t)1u << i)) != 0u)
        {
            smart_io_json_append(writer, "%s%d", separator, (int)pins[i].gpio);
            separator = ",";
        }
    }

    smart_io_json_append(writer, "]");
}

/* ---- Commands ---- */

static esp_err_t smart_io_json_command_hello(smart_io_json_writer_t* writer)
{
    size_t count                     = 0u;
    const smart_io_pin_t* const pins = smart_io_pins(&count);
    bool first                       = true;

    smart_io_json_append(
        writer, ",\"protocol\":%u,\"version\":", (unsigned)SMART_IO_JSON_PROTOCOL);
    smart_io_json_append_string(writer, esp_app_get_description()->version);
    smart_io_json_append(
        writer, ",\"debug\":%s,\"pins\":[", SMART_IO_JSON_DEBUG_BUILD ? "true" : "false");

    for (size_t i = 0u; i < count; i++)
    {
        smart_io_json_append(
            writer, "%s{\"gpio\":%d,\"caps\":", (i == 0u) ? "" : ",", (int)pins[i].gpio);
        smart_io_json_append_functions(writer, pins[i].caps);
        smart_io_json_append(writer, "}");
    }

    smart_io_json_append(writer, "],\"instances\":[");

    for (size_t i = 0u; i < count; i++)
    {
        smart_io_config_t config = {0};
        int32_t value            = 0;

        if (smart_io_json_first_pin_config(pins[i].gpio, &config))
        {
            smart_io_json_append(writer, "%s", first ? "" : ",");
            smart_io_json_append_config_begin(writer, &config);

            if (smart_io_read(pins[i].gpio, &value, NULL) == ESP_OK)
            {
                smart_io_json_append(writer, ",\"value\":%ld", (long)value);
            }

            smart_io_json_append(writer, "}");
            first = false;
        }
    }

    smart_io_json_append(writer, "],");
    smart_io_json_append_streams(writer);
    smart_io_json_append(writer, ",\"unsaved\":%s", smart_io_unsaved() ? "true" : "false");

    return ESP_OK;
}

static void smart_io_json_append_role(
    smart_io_json_writer_t* writer,
    const smart_io_role_info_t* info)
{
    smart_io_json_append(writer, "{\"name\":");
    smart_io_json_append_string(writer, info->description->name);
    smart_io_json_append(writer, ",\"pins\":[");

    for (size_t i = 0u; i < SMART_IO_BUS_PIN_COUNT; i++)
    {
        if (info->description->pin_functions[i] != SMART_IO_FUNCTION_NONE)
        {
            smart_io_json_append(writer, "%s", (i == 0u) ? "" : ",");
            smart_io_json_append_string(
                writer, smart_io_function_name(info->description->pin_functions[i]));
        }
    }

    smart_io_json_append(writer, "],\"params\":[");

    for (size_t i = 0u; i < info->description->param_count; i++)
    {
        const smart_io_param_descriptor_t* const descriptor =
            &info->description->param_descriptors[i];

        smart_io_json_append(writer, "%s{\"name\":", (i == 0u) ? "" : ",");
        smart_io_json_append_string(writer, descriptor->name);
        smart_io_json_append(writer, ",\"type\":");
        smart_io_json_append_string(
            writer, smart_io_json_param_type_names[descriptor->type]);

        if (descriptor->type == SMART_IO_PARAM_TYPE_ENUM)
        {
            smart_io_json_append(writer, ",\"options\":[");

            for (uint32_t j = 0u; j <= descriptor->max; j++)
            {
                smart_io_json_append(writer, "%s", (j == 0u) ? "" : ",");
                smart_io_json_append_string(writer, descriptor->option_names[j]);
            }

            smart_io_json_append(writer, "]");
        }
        else
        {
            smart_io_json_append(
                writer,
                ",\"min\":%lu,\"max\":%lu",
                (unsigned long)descriptor->min,
                (unsigned long)descriptor->max);
        }

        smart_io_json_append(writer, ",\"default\":");
        smart_io_json_param_value(writer, descriptor, descriptor->default_value);

        if (descriptor->unit != NULL)
        {
            smart_io_json_append(writer, ",\"unit\":");
            smart_io_json_append_string(writer, descriptor->unit);
        }

        if (descriptor->shared)
        {
            smart_io_json_append(writer, ",\"shared\":true");
        }

        smart_io_json_append(writer, "}");
    }

    smart_io_json_append(writer, "],\"value\":");

    if (info->description->value_unit != NULL)
    {
        smart_io_json_append(writer, "{\"unit\":");
        smart_io_json_append_string(writer, info->description->value_unit);
        smart_io_json_append(
            writer,
            ",\"min\":%ld,\"max\":%ld}",
            (long)info->description->value_min,
            (long)info->description->value_max);
    }
    else
    {
        smart_io_json_append(writer, "null");
    }

    smart_io_json_append(
        writer,
        ",\"writable\":%s,\"streamable\":%s,\"transfer\":%s,\"discover\":%s,\"stream\":",
        info->writable ? "true" : "false",
        (info->description->stream_kind != SMART_IO_STREAM_KIND_NONE) ? "true" : "false",
        info->transfers ? "true" : "false",
        info->discovers ? "true" : "false");
    smart_io_json_append_string(
        writer, smart_io_json_stream_kind_names[info->description->stream_kind]);
    smart_io_json_append(writer, "}");
}

static esp_err_t smart_io_json_command_roles(smart_io_json_writer_t* writer)
{
    bool first = true;

    smart_io_json_append(writer, ",\"roles\":[");

    for (uint32_t i = 0u; i < (uint32_t)SMART_IO_ROLE_COUNT; i++)
    {
        const smart_io_role_info_t* const info = smart_io_role_info((smart_io_role_t)i);

        if (info != NULL)
        {
            smart_io_json_append(writer, "%s", first ? "" : ",");
            smart_io_json_append_role(writer, info);
            first = false;
        }
    }

    smart_io_json_append(writer, "]");

    return ESP_OK;
}

static esp_err_t smart_io_json_config_gpios(
    const cJSON* root,
    smart_io_config_t* config,
    smart_io_reason_t* reason)
{
    esp_err_t error_code   = ESP_OK;
    const cJSON* const pin = cJSON_GetObjectItemCaseSensitive(root, "gpio");

    config->gpio[0] = GPIO_NUM_NC;
    config->gpio[1] = GPIO_NUM_NC;

    if (cJSON_IsArray(pin))
    {
        const int count = cJSON_GetArraySize(pin);

        if ((count < 1) || (count > (int)SMART_IO_BUS_PIN_COUNT))
        {
            error_code = ESP_ERR_INVALID_ARG;
            smart_io_reason_set(
                reason, "gpio: 1 to %u pins", (unsigned)SMART_IO_BUS_PIN_COUNT);
        }

        for (int i = 0; (i < count) && (error_code == ESP_OK); i++)
        {
            error_code = smart_io_json_gpio_parse(
                cJSON_GetArrayItem(pin, i), &config->gpio[i], reason);
        }
    }
    else
    {
        error_code = smart_io_json_gpio_parse(pin, &config->gpio[0], reason);
    }

    return error_code;
}

/* Params not given take their defaults. */
static esp_err_t smart_io_json_config_params(
    const cJSON* root,
    const smart_io_role_info_t* info,
    smart_io_config_t* config,
    smart_io_reason_t* reason)
{
    esp_err_t error_code      = ESP_OK;
    const cJSON* const params = cJSON_GetObjectItemCaseSensitive(root, "params");
    const cJSON* param        = NULL;

    smart_io_params_defaults(info->description, config->param_values);

    if ((params != NULL) && !cJSON_IsObject(params))
    {
        error_code = ESP_ERR_INVALID_ARG;
        smart_io_reason_set(reason, "params must be an object");
    }

    cJSON_ArrayForEach(param, params)
    {
        size_t index = 0u;

        if (error_code != ESP_OK)
        {
            break;
        }

        error_code = smart_io_params_find(info->description, param->string, &index);

        if (error_code != ESP_OK)
        {
            error_code = ESP_ERR_INVALID_ARG;
            smart_io_reason_set(
                reason, "%s has no param %s", info->description->name, param->string);
            break;
        }

        error_code = smart_io_json_param_parse(
            param,
            &info->description->param_descriptors[index],
            &config->param_values[index],
            reason);
    }

    return error_code;
}

static esp_err_t smart_io_json_command_config(const cJSON* root, smart_io_reason_t* reason)
{
    esp_err_t error_code     = ESP_OK;
    smart_io_config_t config = {0};
    const cJSON* const role  = cJSON_GetObjectItemCaseSensitive(root, "role");
    const cJSON* const label = cJSON_GetObjectItemCaseSensitive(root, "label");

    do
    {
        if (!cJSON_IsString(role)
            || (smart_io_role_find(role->valuestring, &config.role) != ESP_OK))
        {
            error_code = ESP_ERR_INVALID_ARG;
            smart_io_reason_set(reason, "Unknown role");
            break;
        }

        if ((label != NULL) && !cJSON_IsString(label))
        {
            error_code = ESP_ERR_INVALID_ARG;
            smart_io_reason_set(reason, "label must be a string");
            break;
        }

        if (label != NULL)
        {
            if (strlen(label->valuestring) >= sizeof(config.label))
            {
                error_code = ESP_ERR_INVALID_ARG;
                smart_io_reason_set(
                    reason,
                    "Label: up to %u characters",
                    (unsigned)(sizeof(config.label) - 1u));
                break;
            }

            strlcpy(config.label, label->valuestring, sizeof(config.label));
        }

        error_code = smart_io_json_config_gpios(root, &config, reason);

        if (error_code != ESP_OK)
        {
            break;
        }

        error_code = smart_io_json_config_params(
            root, smart_io_role_info(config.role), &config, reason);

        if (error_code != ESP_OK)
        {
            break;
        }

        error_code = smart_io_configure(&config, reason);
    } while (0);

    return error_code;
}

static esp_err_t smart_io_json_command_read(
    const cJSON* root,
    smart_io_json_writer_t* writer,
    smart_io_reason_t* reason)
{
    gpio_num_t gpio      = GPIO_NUM_NC;
    int32_t value        = 0;
    esp_err_t error_code = smart_io_json_resolve_pin(root, &gpio, reason);

    if (error_code == ESP_OK)
    {
        error_code = smart_io_read(gpio, &value, reason);
    }

    if (error_code == ESP_OK)
    {
        smart_io_json_append(writer, ",\"value\":%ld", (long)value);
    }

    return error_code;
}

static esp_err_t smart_io_json_command_write(
    const cJSON* root,
    smart_io_json_writer_t* writer,
    smart_io_reason_t* reason)
{
    gpio_num_t gpio      = GPIO_NUM_NC;
    int64_t value        = 0;
    int32_t applied      = 0;
    esp_err_t error_code = smart_io_json_resolve_pin(root, &gpio, reason);

    if ((error_code == ESP_OK)
        && !smart_io_json_get_integer(
            cJSON_GetObjectItemCaseSensitive(root, "value"),
            (double)INT32_MIN,
            (double)INT32_MAX,
            &value))
    {
        error_code = ESP_ERR_INVALID_ARG;
        smart_io_reason_set(reason, "value must be an integer");
    }

    if (error_code == ESP_OK)
    {
        error_code = smart_io_write(gpio, (int32_t)value, &applied, reason);
    }

    if (error_code == ESP_OK)
    {
        smart_io_json_append(writer, ",\"value\":%ld", (long)applied);
    }

    return error_code;
}

static esp_err_t smart_io_json_command_release(const cJSON* root, smart_io_reason_t* reason)
{
    gpio_num_t gpio      = GPIO_NUM_NC;
    esp_err_t error_code = smart_io_json_resolve_pin(root, &gpio, reason);

    if (error_code == ESP_OK)
    {
        error_code = smart_io_release(gpio, reason);
    }

    return error_code;
}

static esp_err_t smart_io_json_command_stream(
    const cJSON* root,
    bool start,
    smart_io_reason_t* reason)
{
    gpio_num_t gpio      = GPIO_NUM_NC;
    esp_err_t error_code = smart_io_json_resolve_pin(root, &gpio, reason);

    if (error_code == ESP_OK)
    {
        error_code =
            start ? smart_io_stream(gpio, reason) : smart_io_unstream(gpio, reason);
    }

    return error_code;
}

/* {"addr": n, "tx": "hex", "rx": n}. Data goes to the next free bytes of the
 * request's tx/rx buffers; tx_used and rx_used advance past it. */
static esp_err_t smart_io_json_transfer_parse(
    const cJSON* item,
    size_t* tx_used,
    size_t* rx_used,
    smart_io_transfer_t* transfer,
    smart_io_reason_t* reason)
{
    esp_err_t error_code  = ESP_OK;
    int64_t address       = 0;
    int64_t rx_length     = 0;
    size_t tx_length      = 0u;
    const cJSON* const tx = cJSON_GetObjectItemCaseSensitive(item, "tx");
    const cJSON* const rx = cJSON_GetObjectItemCaseSensitive(item, "rx");

    if (!smart_io_json_get_integer(
            cJSON_GetObjectItemCaseSensitive(item, "addr"), 0.0, (double)UINT16_MAX, &address))
    {
        error_code = ESP_ERR_INVALID_ARG;
        smart_io_reason_set(reason, "addr must be an integer");
    }
    else if ((rx != NULL) && !smart_io_json_get_integer(rx, 0.0, (double)SMART_IO_TRANSFER_DATA_MAX, &rx_length))
    {
        error_code = ESP_ERR_INVALID_ARG;
        smart_io_reason_set(
            reason, "rx must be 0-%u", (unsigned)SMART_IO_TRANSFER_DATA_MAX);
    }
    else if ((size_t)rx_length > (SMART_IO_JSON_RX_TOTAL_MAX - *rx_used))
    {
        error_code = ESP_ERR_INVALID_SIZE;
        smart_io_reason_set(
            reason,
            "At most %u bytes read per request",
            (unsigned)SMART_IO_JSON_RX_TOTAL_MAX);
    }
    else if ((tx != NULL) && !cJSON_IsString(tx))
    {
        error_code = ESP_ERR_INVALID_ARG;
        smart_io_reason_set(reason, "tx must be a hex string");
    }
    else if (tx != NULL)
    {
        error_code = smart_io_hex_decode(
            tx->valuestring,
            &smart_io_json_tx_data[*tx_used],
            MIN(SMART_IO_TRANSFER_DATA_MAX, SMART_IO_JSON_TX_TOTAL_MAX - *tx_used),
            &tx_length);

        if (error_code != ESP_OK)
        {
            smart_io_reason_set(
                reason,
                (error_code == ESP_ERR_INVALID_SIZE) ? "tx too long" :
                                                       "tx must be hex, two digits per "
                                                       "byte");
        }
    }
    else
    {
        /* Read or probe only. */
    }

    if (error_code == ESP_OK)
    {
        transfer->tx        = (tx_length > 0u) ? &smart_io_json_tx_data[*tx_used] : NULL;
        transfer->rx        = (rx_length > 0) ? &smart_io_json_rx_data[*rx_used] : NULL;
        transfer->tx_length = tx_length;
        transfer->rx_length = (size_t)rx_length;
        transfer->address   = (uint16_t)address;
        *tx_used += tx_length;
        *rx_used += (size_t)rx_length;
    }

    return error_code;
}

static void smart_io_json_append_results(
    smart_io_json_writer_t* writer,
    const smart_io_transfer_t* transfers,
    size_t count)
{
    smart_io_json_append(writer, ",\"results\":[");

    for (size_t i = 0u; i < count; i++)
    {
        smart_io_json_append(writer, "%s{", (i == 0u) ? "" : ",");

        if (transfers[i].rx_length > 0u)
        {
            smart_io_json_append(writer, "\"rx\":\"");
            smart_io_json_append_hex(writer, transfers[i].rx, transfers[i].rx_length);
            smart_io_json_append(writer, "\"");
        }

        smart_io_json_append(writer, "}");
    }

    smart_io_json_append(writer, "]");
}

/* failed: index of the transfer that failed, SMART_IO_JSON_TRANSFER_NONE when
 * the error isn't about one transfer. */
static esp_err_t smart_io_json_command_transfer(
    const cJSON* root,
    smart_io_json_writer_t* writer,
    size_t* failed,
    smart_io_reason_t* reason)
{
    smart_io_transfer_t transfers[SMART_IO_TRANSFER_COUNT_MAX] = {0};
    const cJSON* const list = cJSON_GetObjectItemCaseSensitive(root, "transfers");
    const int list_size     = cJSON_GetArraySize(list);
    gpio_num_t gpio         = GPIO_NUM_NC;
    size_t count            = 0u;
    size_t tx_used          = 0u;
    size_t rx_used          = 0u;
    esp_err_t error_code    = smart_io_json_resolve_pin(root, &gpio, reason);

    if ((error_code == ESP_OK)
        && (!cJSON_IsArray(list) || (list_size <= 0)
            || ((size_t)list_size > SMART_IO_TRANSFER_COUNT_MAX)))
    {
        error_code = ESP_ERR_INVALID_ARG;
        smart_io_reason_set(
            reason,
            "transfers must be an array of 1-%u",
            (unsigned)SMART_IO_TRANSFER_COUNT_MAX);
    }

    for (const cJSON* item = (error_code == ESP_OK) ? list->child : NULL;
         (item != NULL) && (error_code == ESP_OK);
         item = item->next)
    {
        error_code = smart_io_json_transfer_parse(
            item, &tx_used, &rx_used, &transfers[count], reason);

        if (error_code != ESP_OK)
        {
            *failed = count;
        }

        count++;
    }

    if (error_code == ESP_OK)
    {
        error_code = smart_io_transfer(gpio, transfers, count, failed, reason);

        if (*failed >= count)
        {
            *failed = SMART_IO_JSON_TRANSFER_NONE;
        }
    }

    if (error_code == ESP_OK)
    {
        smart_io_json_append_results(writer, transfers, count);
    }

    return error_code;
}

static esp_err_t smart_io_json_command_discover(
    const cJSON* root,
    smart_io_json_writer_t* writer,
    smart_io_reason_t* reason)
{
    uint16_t found[SMART_IO_DISCOVER_MAX] = {0};
    gpio_num_t gpio                       = GPIO_NUM_NC;
    size_t count                          = 0u;
    esp_err_t error_code = smart_io_json_resolve_pin(root, &gpio, reason);

    if (error_code == ESP_OK)
    {
        error_code = smart_io_discover(gpio, found, ARRAY_SIZE(found), &count, reason);
    }

    if (error_code == ESP_OK)
    {
        smart_io_json_append(writer, ",\"found\":[");

        for (size_t i = 0u; i < count; i++)
        {
            smart_io_json_append(writer, "%s%u", (i == 0u) ? "" : ",", (unsigned)found[i]);
        }

        smart_io_json_append(writer, "]");
    }

    return error_code;
}

static esp_err_t smart_io_json_find_command(const cJSON* root, smart_io_json_command_t* command)
{
    esp_err_t error_code            = ESP_ERR_NOT_FOUND;
    const cJSON* const command_item = cJSON_GetObjectItemCaseSensitive(root, "cmd");

    for (uint32_t i = 0u; (i < (uint32_t)SMART_IO_JSON_COMMAND_COUNT)
                          && cJSON_IsString(command_item) && (error_code != ESP_OK);
         i++)
    {
        if (strcmp(command_item->valuestring, smart_io_json_command_names[i]) == 0)
        {
            *command   = (smart_io_json_command_t)i;
            error_code = ESP_OK;
        }
    }

    return error_code;
}

static esp_err_t smart_io_json_dispatch(
    const cJSON* root,
    smart_io_json_writer_t* writer,
    size_t* failed_transfer,
    smart_io_reason_t* reason)
{
    smart_io_json_command_t command = SMART_IO_JSON_COMMAND_HELLO;
    esp_err_t error_code            = smart_io_json_find_command(root, &command);

    if (error_code != ESP_OK)
    {
        error_code = ESP_ERR_NOT_SUPPORTED;
        smart_io_reason_set(reason, "Unknown cmd");
    }
    else
    {
        switch (command)
        {
            case SMART_IO_JSON_COMMAND_HELLO:
            {
                error_code = smart_io_json_command_hello(writer);
            }
            break;
            case SMART_IO_JSON_COMMAND_ROLES:
            {
                error_code = smart_io_json_command_roles(writer);
            }
            break;
            case SMART_IO_JSON_COMMAND_CONFIG:
            {
                error_code = smart_io_json_command_config(root, reason);
            }
            break;
            case SMART_IO_JSON_COMMAND_RELEASE:
            {
                error_code = smart_io_json_command_release(root, reason);
            }
            break;
            case SMART_IO_JSON_COMMAND_READ:
            {
                error_code = smart_io_json_command_read(root, writer, reason);
            }
            break;
            case SMART_IO_JSON_COMMAND_WRITE:
            {
                error_code = smart_io_json_command_write(root, writer, reason);
            }
            break;
            case SMART_IO_JSON_COMMAND_SAVE:
            {
                error_code = smart_io_save();
            }
            break;
            case SMART_IO_JSON_COMMAND_REVERT:
            {
                error_code = smart_io_revert(reason);
            }
            break;
            case SMART_IO_JSON_COMMAND_STREAM:
            {
                error_code = smart_io_json_command_stream(root, true, reason);
            }
            break;
            case SMART_IO_JSON_COMMAND_UNSTREAM:
            {
                error_code = smart_io_json_command_stream(root, false, reason);
            }
            break;
            case SMART_IO_JSON_COMMAND_TRANSFER:
            {
                error_code =
                    smart_io_json_command_transfer(root, writer, failed_transfer, reason);
            }
            break;
            case SMART_IO_JSON_COMMAND_DISCOVER:
            {
                error_code = smart_io_json_command_discover(root, writer, reason);
            }
            break;
            default:
            {
                error_code = ESP_ERR_NOT_SUPPORTED;
                smart_io_reason_set(reason, "Unknown cmd");
            }
            break;
        }
    }

    return error_code;
}

/* {"id":<id>, with id null when the request has none. */
static void smart_io_json_response_begin(smart_io_json_writer_t* writer, const cJSON* root)
{
    int64_t id = 0;

    if (smart_io_json_get_integer(
            cJSON_GetObjectItemCaseSensitive(root, "id"),
            -9007199254740991.0,
            9007199254740991.0,
            &id))
    {
        smart_io_json_append(writer, "{\"id\":%lld,", (long long)id);
    }
    else
    {
        smart_io_json_append(writer, "{\"id\":null,");
    }
}

static void smart_io_json_append_error(
    smart_io_json_writer_t* writer,
    esp_err_t error,
    const char* reason)
{
    smart_io_json_append(writer, "\"ok\":false,\"error\":");
    smart_io_json_append_string(writer, esp_err_to_name(error));
    smart_io_json_append(writer, ",\"reason\":");
    smart_io_json_append_string(writer, reason);
}

static esp_err_t smart_io_json_writer_result(smart_io_json_writer_t* writer, size_t* length)
{
    esp_err_t error_code = ESP_OK;

    if (writer->overflow)
    {
        error_code = ESP_ERR_INVALID_SIZE;
        *length    = 0u;
    }
    else
    {
        *length = writer->length;
    }

    return error_code;
}


/* Exposed API ------------------------------------------------------------- */

esp_err_t smart_io_json_handle(
    const char* request,
    size_t request_length,
    char* response,
    size_t size,
    size_t* length)
{
    smart_io_json_writer_t writer = {0};
    smart_io_reason_t reason      = {0};
    cJSON* const root             = cJSON_ParseWithLength(request, request_length);
    esp_err_t error_code          = ESP_OK;
    size_t failed_transfer        = SMART_IO_JSON_TRANSFER_NONE;

    smart_io_json_writer_init(&writer, response, size);
    smart_io_json_response_begin(&writer, root);

    const size_t result_start = writer.length;

    if (!cJSON_IsObject(root))
    {
        error_code = ESP_ERR_INVALID_ARG;
        smart_io_reason_set(&reason, "Request is not a JSON object");
    }
    else
    {
        smart_io_json_append(&writer, "\"ok\":true");
        error_code = smart_io_json_dispatch(root, &writer, &failed_transfer, &reason);
        smart_io_json_append(&writer, "}");

        if ((error_code == ESP_OK) && writer.overflow)
        {
            error_code = ESP_ERR_INVALID_SIZE;
            smart_io_reason_set(&reason, "Response too large");
        }
    }

    if (error_code != ESP_OK)
    {
        if (reason.text[0] == '\0')
        {
            smart_io_reason_set(&reason, "%s", esp_err_to_name(error_code));
        }

        smart_io_json_rewind(&writer, result_start);
        smart_io_json_append_error(&writer, error_code, reason.text);

        if (failed_transfer != SMART_IO_JSON_TRANSFER_NONE)
        {
            smart_io_json_append(&writer, ",\"transfer\":%u", (unsigned)failed_transfer);
        }

        smart_io_json_append(&writer, "}");
    }

    cJSON_Delete(root);

    return smart_io_json_writer_result(&writer, length);
}

esp_err_t smart_io_json_error(
    const char* request,
    size_t request_length,
    esp_err_t error,
    const char* reason,
    char* response,
    size_t size,
    size_t* length)
{
    smart_io_json_writer_t writer = {0};
    cJSON* const root             = cJSON_ParseWithLength(request, request_length);

    smart_io_json_writer_init(&writer, response, size);
    smart_io_json_response_begin(&writer, root);
    smart_io_json_append_error(&writer, error, reason);
    smart_io_json_append(&writer, "}");
    cJSON_Delete(root);

    return smart_io_json_writer_result(&writer, length);
}

esp_err_t smart_io_json_update(
    const smart_io_json_update_t* update,
    char* buffer,
    size_t size,
    size_t* length)
{
    smart_io_json_writer_t writer    = {0};
    size_t count                     = 0u;
    const smart_io_pin_t* const pins = smart_io_pins(&count);
    const char* separator            = "";

    smart_io_json_writer_init(&writer, buffer, size);
    smart_io_json_append(&writer, "{\"update\":{");

    if (update->value_mask != 0u)
    {
        const char* value_separator = "";

        smart_io_json_append(&writer, "\"values\":{");

        for (size_t i = 0u; i < count; i++)
        {
            int32_t value = 0;

            if (((update->value_mask & ((uint32_t)1u << i)) != 0u)
                && (smart_io_read(pins[i].gpio, &value, NULL) == ESP_OK))
            {
                smart_io_json_append(
                    &writer, "%s\"%d\":%ld", value_separator, (int)pins[i].gpio, (long)value);
                value_separator = ",";
            }
        }

        smart_io_json_append(&writer, "}");
        separator = ",";
    }

    if (update->config_mask != 0u)
    {
        const char* config_separator = "";

        smart_io_json_append(&writer, "%s\"config\":[", separator);

        for (size_t i = 0u; i < count; i++)
        {
            smart_io_config_t config = {0};

            if ((update->config_mask & ((uint32_t)1u << i)) == 0u)
            {
                /* Unchanged pin. */
            }
            else if (smart_io_json_first_pin_config(pins[i].gpio, &config))
            {
                smart_io_json_append(&writer, "%s", config_separator);
                smart_io_json_append_config_begin(&writer, &config);
                smart_io_json_append(&writer, "}");
                config_separator = ",";
            }
            else if (smart_io_config_get(pins[i].gpio, &config) != ESP_OK)
            {
                smart_io_json_append(
                    &writer,
                    "%s{\"gpio\":[%d],\"role\":\"none\"}",
                    config_separator,
                    (int)pins[i].gpio);
                config_separator = ",";
            }
            else
            {
                /* Second pin of a bus: listed with its first pin. */
            }
        }

        smart_io_json_append(&writer, "]");
        separator = ",";
    }

    if (update->streams)
    {
        smart_io_json_append(&writer, "%s", separator);
        smart_io_json_append_streams(&writer);
        separator = ",";
    }

    if (update->unsaved)
    {
        smart_io_json_append(
            &writer, "%s\"unsaved\":%s", separator, smart_io_unsaved() ? "true" : "false");
        separator = ",";
    }

    if (update->flood_ms != 0u)
    {
        smart_io_json_append(
            &writer, "%s\"flood_ms\":%lu", separator, (unsigned long)update->flood_ms);
    }

    smart_io_json_append(&writer, "}}");

    return smart_io_json_writer_result(&writer, length);
}

/**** END OF FILE ****/
