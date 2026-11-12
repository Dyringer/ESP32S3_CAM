/**
 ******************************************************************************
 * @file    uart_console.c
 * @author  Daniel Dyringer
 * @date    2026-02-17
 * @brief   UART console — init and REPL task.
 ******************************************************************************
 */


/* Includes ---------------------------------------------------------------- */
#include "uart_console.h"

#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <unistd.h>

#include "app_shell.h"
#include "driver/uart.h"
#include "driver/uart_vfs.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "linenoise/linenoise.h"
#include "utils.h"


/* Constants and macros ---------------------------------------------------- */
// clang-format off

#define UART_CONSOLE_PROMPT       "esp32s3> "
#define UART_CONSOLE_HISTORY_LEN  20u
#define UART_CONSOLE_UART_RX_BUF  256u
#define UART_CONSOLE_TASK_STACK   4096u
#define UART_CONSOLE_TASK_PRIO    2u
/* Core 0 is reserved for the network. */
#define UART_CONSOLE_TASK_CORE    1

// clang-format on

/* Private types ----------------------------------------------------------- */


/* Private variables ------------------------------------------------------- */

static const char* uart_console_tag = "uart_console";


/* Private functions ------------------------------------------------------- */

static void uart_console_print(const char* fmt, ...)
{
    va_list arguments;
    va_start(arguments, fmt);
    vprintf(fmt, arguments);
    va_end(arguments);
}

static void uart_console_task(void* arg)
{
    ARG_UNUSED(arg);

    while (true)
    {
        char* line = linenoise(UART_CONSOLE_PROMPT);

        if (line == NULL)
        {
            continue;
        }

        if (line[0] != '\0')
        {
            linenoiseHistoryAdd(line);
        }

        app_shell_dispatch(line, uart_console_print);

        linenoiseFree(line);
    }
}


/* Exposed API ------------------------------------------------------------- */

esp_err_t uart_console_init(void)
{
    fflush(stdout);
    fsync(fileno(stdout));
    setvbuf(stdin, NULL, _IONBF, 0);

    uart_vfs_dev_port_set_rx_line_endings(CONFIG_ESP_CONSOLE_UART_NUM, ESP_LINE_ENDINGS_CR);
    uart_vfs_dev_port_set_tx_line_endings(
        CONFIG_ESP_CONSOLE_UART_NUM, ESP_LINE_ENDINGS_CRLF);

    const uart_config_t uart_config = {
        .baud_rate  = CONFIG_ESP_CONSOLE_UART_BAUDRATE,
        .data_bits  = UART_DATA_8_BITS,
        .parity     = UART_PARITY_DISABLE,
        .stop_bits  = UART_STOP_BITS_1,
        .flow_ctrl  = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };

    ESP_ERROR_CHECK(uart_driver_install(
        CONFIG_ESP_CONSOLE_UART_NUM, (int)UART_CONSOLE_UART_RX_BUF, 0, 0, NULL, 0));
    ESP_ERROR_CHECK(uart_param_config(CONFIG_ESP_CONSOLE_UART_NUM, &uart_config));
    uart_vfs_dev_use_driver(CONFIG_ESP_CONSOLE_UART_NUM);

    linenoiseSetMultiLine(1);
    linenoiseHistorySetMaxLen((int)UART_CONSOLE_HISTORY_LEN);
    linenoiseAllowEmpty(false);

    ESP_LOGI(uart_console_tag, "Console initialised");
    return ESP_OK;
}

esp_err_t uart_console_start(void)
{
    esp_err_t error_code = ESP_OK;

    const BaseType_t task_created = xTaskCreatePinnedToCore(
        uart_console_task,
        "uart_console",
        UART_CONSOLE_TASK_STACK,
        NULL,
        UART_CONSOLE_TASK_PRIO,
        NULL,
        UART_CONSOLE_TASK_CORE);

    if (task_created != pdPASS)
    {
        ESP_LOGE(uart_console_tag, "Failed to create console task");
        error_code = ESP_FAIL;
    }
    else
    {
        ESP_LOGI(
            uart_console_tag, "Console task running on Core %d", UART_CONSOLE_TASK_CORE);
    }

    return error_code;
}

/**** END OF FILE ****/
