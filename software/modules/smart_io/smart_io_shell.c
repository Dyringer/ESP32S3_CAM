/**
 ******************************************************************************
 * @file    smart_io_shell.c
 * @author  Daniel Dyringer
 * @date    2026-10-31
 * @brief   sio console commands.
 ******************************************************************************
 */


/* Includes ---------------------------------------------------------------- */
#include "smart_io_shell.h"

#include <errno.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "app_shell.h"
#include "esp_heap_caps.h"
#include "smart_io.h"
#include "smart_io_hex.h"
#include "smart_io_params.h"
#include "smart_io_stream.h"
#include "utils.h"


/* Constants and macros ---------------------------------------------------- */
// clang-format off

#define SMART_IO_SHELL_USAGE                                                    \
    "Usage:\n"                                                                  \
    "  sio list\n"                                                              \
    "  sio config <gpio>[,<gpio>] <role> [label=<label>] [<param>=<value> ...]\n" \
    "  sio release <gpio|label>\n"                                              \
    "  sio read <gpio|label>\n"                                                 \
    "  sio write <gpio|label> <value>\n"                                        \
    "  sio transfer <bus> [addr=<addr>] [w=<hex>] [r=<count>]\n"                 \
    "  sio discover <bus>\n"                                                    \
    "  sio save\n"                                                              \
    "  sio revert\n"                                                            \
    "  sio stats\n"

#define SMART_IO_SHELL_LABEL_KEY    "label="
#define SMART_IO_SHELL_ADDRESS_KEY  "addr="
#define SMART_IO_SHELL_WRITE_KEY    "w="
#define SMART_IO_SHELL_READ_KEY     "r="
/* tx, rx, then rx as hex text with its terminator. */
#define SMART_IO_SHELL_TRANSFER_SIZE    ((4u * SMART_IO_TRANSFER_DATA_MAX) + 1u)

// clang-format on

/* Private types ----------------------------------------------------------- */


/* Private variables ------------------------------------------------------- */


/* Private functions ------------------------------------------------------- */

static void smart_io_shell_print_error(
    app_shell_print_fn_t print,
    esp_err_t error_code,
    const smart_io_reason_t* reason)
{
    print(
        "Failed: %s\n",
        (reason->text[0] != '\0') ? reason->text : esp_err_to_name(error_code));
}

static esp_err_t smart_io_shell_parse_int(const char* text, long minimum, long maximum, long* value)
{
    esp_err_t error_code = ESP_ERR_INVALID_ARG;
    char* end            = NULL;

    errno             = 0;
    const long parsed = strtol(text, &end, 10);

    if ((errno == 0) && (end != text) && (*end == '\0') && (parsed >= minimum)
        && (parsed <= maximum))
    {
        *value     = parsed;
        error_code = ESP_OK;
    }

    return error_code;
}

/* A number is a GPIO, anything else a label. */
static esp_err_t smart_io_shell_target(
    const char* text,
    app_shell_print_fn_t print,
    gpio_num_t* gpio)
{
    long number          = 0;
    esp_err_t error_code = ESP_OK;

    if ((text[0] >= '0') && (text[0] <= '9'))
    {
        error_code = smart_io_shell_parse_int(text, 0, GPIO_NUM_MAX - 1, &number);
        *gpio      = (gpio_num_t)number;

        if (error_code != ESP_OK)
        {
            print("Invalid GPIO: %s\n", text);
        }
    }
    else
    {
        error_code = smart_io_lookup(text, gpio);

        if (error_code != ESP_OK)
        {
            print("Unknown label: %s\n", text);
        }
    }

    return error_code;
}

static void smart_io_shell_print_params(
    app_shell_print_fn_t print,
    const smart_io_role_info_t* info,
    const uint32_t param_values[SMART_IO_PARAM_MAX])
{
    for (size_t i = 0u; i < info->description->param_count; i++)
    {
        char text[SMART_IO_PARAMS_TEXT_SIZE] = {0};

        smart_io_params_format(
            &info->description->param_descriptors[i], param_values[i], text, sizeof(text));
        print(" %s=%s", info->description->param_descriptors[i].name, text);
    }
}

static void smart_io_shell_print_caps(app_shell_print_fn_t print, uint32_t caps)
{
    for (uint32_t i = 1u; i < (uint32_t)SMART_IO_FUNCTION_COUNT; i++)
    {
        if ((caps & SMART_IO_CAP(i)) != 0u)
        {
            print(" %s", smart_io_function_name((smart_io_function_t)i));
        }
    }
}

static void smart_io_shell_print_pin(app_shell_print_fn_t print, const smart_io_pin_t* pin)
{
    smart_io_config_t config = {0};

    print("%-4d ", (int)pin->gpio);

    if (smart_io_config_get(pin->gpio, &config) != ESP_OK)
    {
        print("%-8s%s", "free", " can be:");
        smart_io_shell_print_caps(print, pin->caps);
    }
    else if (config.gpio[0] != pin->gpio)
    {
        print("(pin 2 of GPIO %d)", (int)config.gpio[0]);
    }
    else
    {
        const smart_io_role_info_t* const info = smart_io_role_info(config.role);
        smart_io_reason_t reason               = {0};
        int32_t value                          = 0;

        print(
            "%-8s %-15s ",
            info->description->name,
            (config.label[0] != '\0') ? config.label : "-");

        if (smart_io_read(pin->gpio, &value, &reason) == ESP_OK)
        {
            print("%-6ld", (long)value);
        }
        else
        {
            print("%-6s", "-");
        }

        if (config.gpio[1] != GPIO_NUM_NC)
        {
            print(" pins=%d,%d", (int)config.gpio[0], (int)config.gpio[1]);
        }

        smart_io_shell_print_params(print, info, config.param_values);
    }

    print("\n");
}

/* ---- Commands ---- */

static void smart_io_shell_cmd_list(app_shell_print_fn_t print)
{
    size_t count                     = 0u;
    const smart_io_pin_t* const pins = smart_io_pins(&count);

    print("%-4s %-8s %-15s %-6s %s\n", "GPIO", "Role", "Label", "Value", "Params");

    for (size_t i = 0u; i < count; i++)
    {
        smart_io_shell_print_pin(print, &pins[i]);
    }

    if (smart_io_unsaved())
    {
        print("Unsaved changes: sio save / sio revert\n");
    }
}

static esp_err_t smart_io_shell_parse_pins(
    const char* text,
    app_shell_print_fn_t print,
    smart_io_config_t* config)
{
    esp_err_t error_code = ESP_ERR_INVALID_ARG;
    char buffer[16]      = {0};
    long first           = 0;
    long second          = 0;

    strlcpy(buffer, text, sizeof(buffer));

    char* const comma = strchr(buffer, ',');

    if (comma != NULL)
    {
        *comma     = '\0';
        error_code = smart_io_shell_parse_int(comma + 1, 0, GPIO_NUM_MAX - 1, &second);
    }
    else
    {
        second     = GPIO_NUM_NC;
        error_code = ESP_OK;
    }

    if (error_code == ESP_OK)
    {
        error_code = smart_io_shell_parse_int(buffer, 0, GPIO_NUM_MAX - 1, &first);
    }

    if (error_code == ESP_OK)
    {
        config->gpio[0] = (gpio_num_t)first;
        config->gpio[1] = (gpio_num_t)second;
    }
    else
    {
        print("Invalid pins: %s (expected <gpio> or <gpio>,<gpio>)\n", text);
    }

    return error_code;
}

static esp_err_t smart_io_shell_parse_setting(
    const char* text,
    const smart_io_role_info_t* info,
    app_shell_print_fn_t print,
    smart_io_config_t* config)
{
    esp_err_t error_code                 = ESP_ERR_INVALID_ARG;
    smart_io_reason_t reason             = {0};
    char name[SMART_IO_PARAMS_TEXT_SIZE] = {0};
    const char* const equals             = strchr(text, '=');
    size_t index                         = 0u;

    do
    {
        if (strncmp(text, SMART_IO_SHELL_LABEL_KEY, strlen(SMART_IO_SHELL_LABEL_KEY)) == 0)
        {
            const char* const label = &text[strlen(SMART_IO_SHELL_LABEL_KEY)];

            if (strlen(label) < sizeof(config->label))
            {
                strlcpy(config->label, label, sizeof(config->label));
                error_code = ESP_OK;
            }
            else
            {
                print("Label too long: %s\n", label);
            }

            break;
        }

        if ((equals == NULL) || ((size_t)(equals - text) >= sizeof(name)))
        {
            print("Expected <param>=<value>: %s\n", text);
            break;
        }

        memcpy(name, text, (size_t)(equals - text));

        if (smart_io_params_find(info->description, name, &index) != ESP_OK)
        {
            print("Unknown %s param: %s; params:", info->description->name, name);

            for (size_t i = 0u; i < info->description->param_count; i++)
            {
                print(" %s", info->description->param_descriptors[i].name);
            }

            print("\n");
            break;
        }

        error_code = smart_io_params_parse(
            &info->description->param_descriptors[index],
            equals + 1,
            &config->param_values[index],
            &reason);

        if (error_code != ESP_OK)
        {
            smart_io_shell_print_error(print, error_code, &reason);
        }
    } while (0);

    return error_code;
}

static void smart_io_shell_cmd_config(int argc, char** argv, app_shell_print_fn_t print)
{
    smart_io_config_t config         = {0};
    smart_io_reason_t reason         = {0};
    const smart_io_role_info_t* info = NULL;
    esp_err_t error_code             = smart_io_shell_parse_pins(argv[2], print, &config);

    do
    {
        if (error_code != ESP_OK)
        {
            break;
        }

        error_code = smart_io_role_find(argv[3], &config.role);

        if (error_code != ESP_OK)
        {
            print("Unknown role: %s; roles:", argv[3]);

            for (uint32_t i = 0u; i < (uint32_t)SMART_IO_ROLE_COUNT; i++)
            {
                const smart_io_role_info_t* const known =
                    smart_io_role_info((smart_io_role_t)i);

                if (known != NULL)
                {
                    print(" %s", known->description->name);
                }
            }

            print("\n");
            break;
        }

        info = smart_io_role_info(config.role);
        smart_io_params_defaults(info->description, config.param_values);

        for (int i = 4; (i < argc) && (error_code == ESP_OK); i++)
        {
            error_code = smart_io_shell_parse_setting(argv[i], info, print, &config);
        }

        if (error_code != ESP_OK)
        {
            break;
        }

        error_code = smart_io_configure(&config, &reason);

        if (error_code != ESP_OK)
        {
            smart_io_shell_print_error(print, error_code, &reason);
            break;
        }

        size_t count                     = 0u;
        const smart_io_pin_t* const pins = smart_io_pins(&count);

        for (size_t i = 0u; i < count; i++)
        {
            if (pins[i].gpio == config.gpio[0])
            {
                smart_io_shell_print_pin(print, &pins[i]);
            }
        }
    } while (0);
}

static void smart_io_shell_cmd_release(const char* target, app_shell_print_fn_t print)
{
    gpio_num_t gpio          = GPIO_NUM_NC;
    smart_io_reason_t reason = {0};

    if (smart_io_shell_target(target, print, &gpio) == ESP_OK)
    {
        const esp_err_t error_code = smart_io_release(gpio, &reason);

        if (error_code == ESP_OK)
        {
            print("Released\n");
        }
        else
        {
            smart_io_shell_print_error(print, error_code, &reason);
        }
    }
}

static void smart_io_shell_cmd_read(const char* target, app_shell_print_fn_t print)
{
    gpio_num_t gpio          = GPIO_NUM_NC;
    smart_io_reason_t reason = {0};
    int32_t value            = 0;

    if (smart_io_shell_target(target, print, &gpio) == ESP_OK)
    {
        const esp_err_t error_code = smart_io_read(gpio, &value, &reason);

        if (error_code == ESP_OK)
        {
            print("%ld\n", (long)value);
        }
        else
        {
            smart_io_shell_print_error(print, error_code, &reason);
        }
    }
}

static void smart_io_shell_cmd_write(
    const char* target,
    const char* text,
    app_shell_print_fn_t print)
{
    gpio_num_t gpio          = GPIO_NUM_NC;
    smart_io_reason_t reason = {0};
    long value               = 0;
    int32_t applied          = 0;

    if (smart_io_shell_parse_int(text, INT32_MIN, INT32_MAX, &value) != ESP_OK)
    {
        print("Invalid value: %s\n", text);
    }
    else if (smart_io_shell_target(target, print, &gpio) == ESP_OK)
    {
        const esp_err_t error_code =
            smart_io_write(gpio, (int32_t)value, &applied, &reason);

        if (error_code == ESP_OK)
        {
            print("%ld\n", (long)applied);
        }
        else
        {
            smart_io_shell_print_error(print, error_code, &reason);
        }
    }
    else
    {
        /* Target error already printed. */
    }
}

static void smart_io_shell_cmd_save(app_shell_print_fn_t print)
{
    const esp_err_t error_code = smart_io_save();

    if (error_code == ESP_OK)
    {
        print("Saved\n");
    }
    else
    {
        print("Failed: %s\n", esp_err_to_name(error_code));
    }
}

static void smart_io_shell_cmd_revert(app_shell_print_fn_t print)
{
    smart_io_reason_t reason   = {0};
    const esp_err_t error_code = smart_io_revert(&reason);

    if (error_code == ESP_OK)
    {
        smart_io_shell_cmd_list(print);
    }
    else
    {
        smart_io_shell_print_error(print, error_code, &reason);
    }
}

/* Decimal or 0x-prefixed hex. */
static esp_err_t smart_io_shell_parse_address(const char* text, uint16_t* address)
{
    esp_err_t error_code = ESP_ERR_INVALID_ARG;
    char* end            = NULL;

    errno             = 0;
    const long parsed = strtol(text, &end, 0);

    if ((errno == 0) && (end != text) && (*end == '\0') && (parsed >= 0)
        && (parsed <= (long)UINT16_MAX))
    {
        *address   = (uint16_t)parsed;
        error_code = ESP_OK;
    }

    return error_code;
}

/* addr=, w=, r= into transfer; tx points into data. */
static esp_err_t smart_io_shell_transfer_parse(
    int argc,
    char** argv,
    uint8_t* data,
    smart_io_transfer_t* transfer,
    app_shell_print_fn_t print)
{
    esp_err_t error_code = ESP_OK;
    long number          = 0;

    for (int i = 3; (i < argc) && (error_code == ESP_OK); i++)
    {
        const char* const argument = argv[i];

        if (strncmp(argument, SMART_IO_SHELL_ADDRESS_KEY, strlen(SMART_IO_SHELL_ADDRESS_KEY))
            == 0)
        {
            error_code = smart_io_shell_parse_address(
                &argument[strlen(SMART_IO_SHELL_ADDRESS_KEY)], &transfer->address);
        }
        else if (strncmp(argument, SMART_IO_SHELL_WRITE_KEY, strlen(SMART_IO_SHELL_WRITE_KEY)) == 0)
        {
            error_code = smart_io_hex_decode(
                &argument[strlen(SMART_IO_SHELL_WRITE_KEY)],
                data,
                SMART_IO_TRANSFER_DATA_MAX,
                &transfer->tx_length);
            transfer->tx = data;
        }
        else if (strncmp(argument, SMART_IO_SHELL_READ_KEY, strlen(SMART_IO_SHELL_READ_KEY)) == 0)
        {
            error_code = smart_io_shell_parse_int(
                &argument[strlen(SMART_IO_SHELL_READ_KEY)],
                0,
                (long)SMART_IO_TRANSFER_DATA_MAX,
                &number);
            transfer->rx_length = (size_t)number;
        }
        else
        {
            error_code = ESP_ERR_INVALID_ARG;
        }

        if (error_code != ESP_OK)
        {
            print("Invalid argument: %s\n", argument);
        }
    }

    return error_code;
}

static void smart_io_shell_cmd_transfer(int argc, char** argv, app_shell_print_fn_t print)
{
    gpio_num_t gpio              = GPIO_NUM_NC;
    smart_io_reason_t reason     = {0};
    smart_io_transfer_t transfer = {0};
    size_t failed                = 0u;
    esp_err_t error_code         = ESP_OK;
    uint8_t* const data = heap_caps_malloc(SMART_IO_SHELL_TRANSFER_SIZE, MALLOC_CAP_SPIRAM);

    do
    {
        if (data == NULL)
        {
            print("Out of memory\n");
            break;
        }

        uint8_t* const rx = &data[SMART_IO_TRANSFER_DATA_MAX];
        char* const text  = (char*)&data[2u * SMART_IO_TRANSFER_DATA_MAX];

        transfer.rx = rx;

        if ((smart_io_shell_target(argv[2], print, &gpio) != ESP_OK)
            || (smart_io_shell_transfer_parse(argc, argv, data, &transfer, print) != ESP_OK))
        {
            break;
        }

        error_code = smart_io_transfer(gpio, &transfer, 1u, &failed, &reason);

        if (error_code != ESP_OK)
        {
            smart_io_shell_print_error(print, error_code, &reason);
            break;
        }

        smart_io_hex_encode(
            rx, transfer.rx_length, text, (2u * SMART_IO_TRANSFER_DATA_MAX) + 1u);
        print("%s\n", (transfer.rx_length > 0u) ? text : "OK");
    } while (0);

    heap_caps_free(data);
}

static void smart_io_shell_cmd_discover(const char* target, app_shell_print_fn_t print)
{
    gpio_num_t gpio                       = GPIO_NUM_NC;
    smart_io_reason_t reason              = {0};
    uint16_t found[SMART_IO_DISCOVER_MAX] = {0};
    size_t count                          = 0u;

    if (smart_io_shell_target(target, print, &gpio) == ESP_OK)
    {
        const esp_err_t error_code =
            smart_io_discover(gpio, found, ARRAY_SIZE(found), &count, &reason);

        if (error_code != ESP_OK)
        {
            smart_io_shell_print_error(print, error_code, &reason);
        }
        else if (count == 0u)
        {
            print("Nothing found\n");
        }
        else
        {
            for (size_t i = 0u; i < count; i++)
            {
                print("0x%02X%s", (unsigned)found[i], ((i + 1u) < count) ? " " : "\n");
            }
        }
    }
}

/* Maxima are reset by every call. */
static void smart_io_shell_cmd_stats(app_shell_print_fn_t print)
{
    smart_io_stream_stats_t stats = {0};

    smart_io_stream_stats(&stats);
    print("clients        %lu\n", (unsigned long)stats.clients);
    print("streams        0x%08lx\n", (unsigned long)smart_io_streams());
    print("edges/tick max %lu\n", (unsigned long)stats.edges_per_tick_max);
    print("tick us max    %lu\n", (unsigned long)stats.tick_us_max);
    print("frames         %lu\n", (unsigned long)stats.frames);
    print("lost           %lu\n", (unsigned long)stats.lost);
    print("gaps           %lu\n", (unsigned long)stats.gaps);
    print("backlog jumps  %lu\n", (unsigned long)stats.backlog_jumps);
    print("adc period ns  %lu\n", (unsigned long)stats.adc_conversion_ns);
    print("adc invalid    %lu\n", (unsigned long)stats.adc_invalid);
    print("adc clk jumps  %lu\n", (unsigned long)stats.adc_clock_jumps);
}

static void smart_io_shell_cmd(int argc, char** argv, app_shell_print_fn_t print)
{
    const char* const command = (argc >= 2) ? argv[1] : "";

    if ((argc == 2) && (strcmp(command, "list") == 0))
    {
        smart_io_shell_cmd_list(print);
    }
    else if ((argc >= 4) && (strcmp(command, "config") == 0))
    {
        smart_io_shell_cmd_config(argc, argv, print);
    }
    else if ((argc == 3) && (strcmp(command, "release") == 0))
    {
        smart_io_shell_cmd_release(argv[2], print);
    }
    else if ((argc == 3) && (strcmp(command, "read") == 0))
    {
        smart_io_shell_cmd_read(argv[2], print);
    }
    else if ((argc == 4) && (strcmp(command, "write") == 0))
    {
        smart_io_shell_cmd_write(argv[2], argv[3], print);
    }
    else if ((argc >= 3) && (strcmp(command, "transfer") == 0))
    {
        smart_io_shell_cmd_transfer(argc, argv, print);
    }
    else if ((argc == 3) && (strcmp(command, "discover") == 0))
    {
        smart_io_shell_cmd_discover(argv[2], print);
    }
    else if ((argc == 2) && (strcmp(command, "save") == 0))
    {
        smart_io_shell_cmd_save(print);
    }
    else if ((argc == 2) && (strcmp(command, "revert") == 0))
    {
        smart_io_shell_cmd_revert(print);
    }
    else if ((argc == 2) && (strcmp(command, "stats") == 0))
    {
        smart_io_shell_cmd_stats(print);
    }
    else
    {
        print(SMART_IO_SHELL_USAGE);
    }
}


/* Exposed API ------------------------------------------------------------- */

esp_err_t smart_io_shell_register(void)
{
    return app_shell_register_cmd(
        "sio",
        "Smart IO pins: sio <list | config | release | read | write | transfer | "
        "discover | save | revert | stats>",
        smart_io_shell_cmd);
}

/**** END OF FILE ****/
