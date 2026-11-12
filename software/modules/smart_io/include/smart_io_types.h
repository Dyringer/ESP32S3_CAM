/**
 ******************************************************************************
 * @file    smart_io_types.h
 * @author  Daniel Dyringer
 * @date    2026-10-31
 * @brief   smart_io public types.
 ******************************************************************************
 */

/* Define to prevent recursive inclusion ----------------------------------- */
#ifndef SMART_IO_TYPES_H_
#define SMART_IO_TYPES_H_

/* Includes ---------------------------------------------------------------- */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "smart_io_caps.h"
#include "soc/gpio_num.h"


/* Constants and macros ---------------------------------------------------- */
// clang-format off

#define SMART_IO_BUS_PIN_COUNT          2u
#define SMART_IO_PARAM_MAX              4u
#define SMART_IO_LABEL_SIZE             16u     /* [A-Za-z0-9_-], unique */
#define SMART_IO_REASON_SIZE            96u

// clang-format on

/* Types ------------------------------------------------------------------- */

/* ---- Enums ---- */

/* Posted under SMART_IO_EVENTS. CHANGED carries no data: the changes are kept
 * by smart_io until smart_io_changes_take(), so a dropped post loses nothing. */
typedef enum smart_io_event_e
{
    SMART_IO_EVENT_CHANGED      = 0,
    SMART_IO_EVENT_STREAM_FRAME = 1,
    SMART_IO_EVENT_STREAM_FLOOD = 2, /* uint32_t pause in ms */
} smart_io_event_t;

typedef enum smart_io_role_e
{
    SMART_IO_ROLE_NONE   = 0,
    SMART_IO_ROLE_INPUT  = 1,
    SMART_IO_ROLE_OUTPUT = 2,
    SMART_IO_ROLE_ADC    = 3,
    SMART_IO_ROLE_PWM    = 4,
    SMART_IO_ROLE_PULSE  = 5,
    SMART_IO_ROLE_I2C    = 6,
    SMART_IO_ROLE_UART   = 7,

    SMART_IO_ROLE_COUNT,
} smart_io_role_t;

typedef enum smart_io_stream_kind_e
{
    SMART_IO_STREAM_KIND_NONE    = 0,
    SMART_IO_STREAM_KIND_DIGITAL = 1,
    SMART_IO_STREAM_KIND_SERIES  = 2,
    SMART_IO_STREAM_KIND_BYTES   = 3,
    SMART_IO_STREAM_KIND_VALUE   = 4,
} smart_io_stream_kind_t;

typedef enum smart_io_param_type_e
{
    SMART_IO_PARAM_TYPE_U32  = 0,
    SMART_IO_PARAM_TYPE_BOOL = 1,
    SMART_IO_PARAM_TYPE_ENUM = 2,
} smart_io_param_type_t;

/* ---- Pins, reasons, transfers ---- */

typedef struct smart_io_pin_s
{
    uint32_t caps;
    gpio_num_t gpio;
} smart_io_pin_t;

typedef struct smart_io_reason_s
{
    char text[SMART_IO_REASON_SIZE];
} smart_io_reason_t;

typedef struct smart_io_transfer_s
{
    const uint8_t* tx;
    uint8_t* rx;
    size_t tx_length;
    size_t rx_length;
    uint16_t address;
} smart_io_transfer_t;

/* ---- Configs and roles ---- */

typedef struct smart_io_param_descriptor_s
{
    const char* name;
    const char* unit;
    const char* const* option_names;
    uint32_t min;
    uint32_t max;
    uint32_t default_value;
    smart_io_param_type_t type;
    bool shared; /* one value for every pin of the role, synced at configure */
} smart_io_param_descriptor_t;

/* Stored in NVS as is. */
typedef struct smart_io_config_s
{
    uint32_t param_values[SMART_IO_PARAM_MAX];
    smart_io_role_t role;
    gpio_num_t gpio[SMART_IO_BUS_PIN_COUNT];
    char label[SMART_IO_LABEL_SIZE];
} smart_io_config_t;

/* Defined by the role. param_descriptors: index = position in
 * config.param_values. */
typedef struct smart_io_role_description_s
{
    const char* name;
    const smart_io_param_descriptor_t* param_descriptors;
    size_t param_count;
    const char* value_unit; /* NULL: role has no value */
    int32_t value_min;
    int32_t value_max;
    smart_io_role_t role;
    smart_io_stream_kind_t stream_kind;
    smart_io_function_t pin_functions[SMART_IO_BUS_PIN_COUNT];
} smart_io_role_description_t;

/* A role as seen through the API: what it is and what it can do. The
 * capabilities are set by smart_io from the role's operations. */
typedef struct smart_io_role_info_s
{
    const smart_io_role_description_t* description;
    bool writable;
    bool transfers;
    bool discovers;
} smart_io_role_info_t;

#endif /* SMART_IO_TYPES_H_ */

/**** END OF FILE ****/
