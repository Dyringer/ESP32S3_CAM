/**
 ******************************************************************************
 * @file    uart_console.c
 * @author  Daniel Dyringer
 * @date    2026-02-17
 * @brief   UART console — init and REPL task pinned to Core 0.
 ******************************************************************************
 */


/* Includes ---------------------------------------------------------------- */
#include "uart_console.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "app_shell.h"
#include "driver/uart.h"
#include "driver/uart_vfs.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "linenoise/linenoise.h"


/* Constants and macros ---------------------------------------------------- */
// clang-format off

#define UART_CONSOLE_PROMPT       "esp32s3> "
#define UART_CONSOLE_HISTORY_LEN  20
#define UART_CONSOLE_UART_RX_BUF  256
#define UART_CONSOLE_TASK_STACK   4096
#define UART_CONSOLE_TASK_PRIO    2

// clang-format on


/* Private variables ------------------------------------------------------- */

static const char *uart_console_tag = "uart_console";


/* Private functions ------------------------------------------------------- */

/**
 * @brief printf-compatible output function passed to app_shell_dispatch.
 *        Routes all command output to stdout (UART).
 */
static void uart_console_print(const char *fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    vprintf(fmt, args);
    va_end(args);
}

static void uart_console_task(void *arg)
{
    (void)arg;

    while (true)
    {
        char *line = linenoise(UART_CONSOLE_PROMPT);

        if (!line)
        {
            continue;
        }

        if (*line)
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

    uart_vfs_dev_port_set_rx_line_endings(CONFIG_ESP_CONSOLE_UART_NUM,
                                          ESP_LINE_ENDINGS_CR);
    uart_vfs_dev_port_set_tx_line_endings(CONFIG_ESP_CONSOLE_UART_NUM,
                                          ESP_LINE_ENDINGS_CRLF);

    const uart_config_t uart_cfg = {
        .baud_rate  = CONFIG_ESP_CONSOLE_UART_BAUDRATE,
        .data_bits  = UART_DATA_8_BITS,
        .parity     = UART_PARITY_DISABLE,
        .stop_bits  = UART_STOP_BITS_1,
        .flow_ctrl  = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };

    ESP_ERROR_CHECK(uart_driver_install(CONFIG_ESP_CONSOLE_UART_NUM,
                                        UART_CONSOLE_UART_RX_BUF, 0, 0, NULL, 0));
    ESP_ERROR_CHECK(uart_param_config(CONFIG_ESP_CONSOLE_UART_NUM, &uart_cfg));
    uart_vfs_dev_use_driver(CONFIG_ESP_CONSOLE_UART_NUM);

    linenoiseSetMultiLine(1);
    linenoiseHistorySetMaxLen(UART_CONSOLE_HISTORY_LEN);
    linenoiseAllowEmpty(false);

    ESP_LOGI(uart_console_tag, "Console initialised");
    return ESP_OK;
}

esp_err_t uart_console_start(void)
{
    esp_err_t error_code;

    const BaseType_t task_created = xTaskCreatePinnedToCore(
        uart_console_task,
        "uart_console",
        UART_CONSOLE_TASK_STACK,
        NULL,
        UART_CONSOLE_TASK_PRIO,
        NULL,
        UART_CONSOLE_CORE);

    if (task_created != pdPASS)
    {
        ESP_LOGE(uart_console_tag, "Failed to create console task");
        error_code = ESP_FAIL;
    }
    else
    {
        ESP_LOGI(uart_console_tag,
                 "Console task running on Core %d", UART_CONSOLE_CORE);
        error_code = ESP_OK;
    }

    return error_code;
}


/**** END OF FILE ****/
