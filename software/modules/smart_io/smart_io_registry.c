/**
 ******************************************************************************
 * @file    smart_io_registry.c
 * @author  Daniel Dyringer
 * @date    2026-10-31
 * @brief   smart_io pin pool and entries.
 ******************************************************************************
 */


/* Includes ---------------------------------------------------------------- */
#include "smart_io_registry.h"

#include <string.h>

#include "esp_attr.h"
#include "smart_io_pad.h"
#include "utils.h"


/* Constants and macros ---------------------------------------------------- */
// clang-format off

#define SMART_IO_REGISTRY_PIN_ENTRY(gpio, caps)     {(caps), (gpio)},

// clang-format on

/* Private types ----------------------------------------------------------- */


/* Private variables ------------------------------------------------------- */

static const smart_io_pin_t smart_io_registry_board[] = {
    BSP_SMART_IO_PINS(SMART_IO_REGISTRY_PIN_ENTRY)};

_Static_assert(ARRAY_SIZE(smart_io_registry_board) == SMART_IO_MAX_PINS, "pool count");

static smart_io_pin_t smart_io_registry_pins_pool[SMART_IO_MAX_PINS];

/* Internal RAM: the GPIO ISR reads the instance it was registered with. */
static DRAM_ATTR smart_io_instance_t smart_io_registry_slots[SMART_IO_MAX_PINS];


/* Private functions ------------------------------------------------------- */

static bool smart_io_registry_uses(const smart_io_instance_t* instance, gpio_num_t gpio)
{
    return (instance->role_api != NULL)
           && ((instance->config.gpio[0] == gpio) || (instance->config.gpio[1] == gpio));
}


/* Exposed API ------------------------------------------------------------- */

esp_err_t smart_io_registry_init(void)
{
    esp_err_t error_code = ESP_OK;

    for (size_t i = 0u; i < SMART_IO_MAX_PINS; i++)
    {
        smart_io_registry_pins_pool[i].gpio = smart_io_registry_board[i].gpio;
        smart_io_registry_pins_pool[i].caps = smart_io_pad_caps(
            smart_io_registry_board[i].gpio, smart_io_registry_board[i].caps);
        smart_io_registry_free(&smart_io_registry_slots[i]);

        for (size_t j = 0u; j < i; j++)
        {
            if (smart_io_registry_pins_pool[j].gpio == smart_io_registry_pins_pool[i].gpio)
            {
                error_code = ESP_ERR_INVALID_ARG;
            }
        }
    }

    return error_code;
}

const smart_io_pin_t* smart_io_registry_pins(size_t* count)
{
    if (count != NULL)
    {
        *count = SMART_IO_MAX_PINS;
    }

    return smart_io_registry_pins_pool;
}

esp_err_t smart_io_registry_pin_index(gpio_num_t gpio, uint8_t* index)
{
    esp_err_t error_code = ESP_ERR_NOT_FOUND;

    for (size_t i = 0u; i < SMART_IO_MAX_PINS; i++)
    {
        if ((error_code != ESP_OK) && (smart_io_registry_pins_pool[i].gpio == gpio))
        {
            error_code = ESP_OK;

            if (index != NULL)
            {
                *index = (uint8_t)i;
            }
        }
    }

    return error_code;
}

smart_io_instance_t* smart_io_registry_instances(size_t* count)
{
    if (count != NULL)
    {
        *count = SMART_IO_MAX_PINS;
    }

    return smart_io_registry_slots;
}

smart_io_instance_t* smart_io_registry_find_gpio(gpio_num_t gpio)
{
    smart_io_instance_t* found = NULL;

    for (size_t i = 0u; (i < SMART_IO_MAX_PINS) && (found == NULL); i++)
    {
        if ((gpio != GPIO_NUM_NC)
            && smart_io_registry_uses(&smart_io_registry_slots[i], gpio))
        {
            found = &smart_io_registry_slots[i];
        }
    }

    return found;
}

smart_io_instance_t* smart_io_registry_find_label(const char* label)
{
    smart_io_instance_t* found = NULL;

    for (size_t i = 0u; (i < SMART_IO_MAX_PINS) && (found == NULL); i++)
    {
        smart_io_instance_t* const instance = &smart_io_registry_slots[i];

        if ((instance->role_api != NULL) && (label != NULL) && (label[0] != '\0')
            && (strncmp(instance->config.label, label, SMART_IO_LABEL_SIZE) == 0))
        {
            found = instance;
        }
    }

    return found;
}

smart_io_instance_t* smart_io_registry_alloc(void)
{
    smart_io_instance_t* found = NULL;

    for (size_t i = 0u; (i < SMART_IO_MAX_PINS) && (found == NULL); i++)
    {
        if (smart_io_registry_slots[i].role_api == NULL)
        {
            found = &smart_io_registry_slots[i];
        }
    }

    return found;
}

void smart_io_registry_free(smart_io_instance_t* instance)
{
    if (instance != NULL)
    {
        memset(&instance->config, 0, sizeof(instance->config));
        memset(&instance->state, 0, sizeof(instance->state));
        instance->config.gpio[0] = GPIO_NUM_NC;
        instance->config.gpio[1] = GPIO_NUM_NC;
        instance->role_api       = NULL;
    }
}

/**** END OF FILE ****/
