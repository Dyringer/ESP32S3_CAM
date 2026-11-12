/**
 ******************************************************************************
 * @file    web_console.c
 * @author  Daniel Dyringer
 * @date    2026-03-09
 * @brief   Web console — ESP log capture and WebSocket push.
 ******************************************************************************
 */


/* Includes ---------------------------------------------------------------- */
#include "web_console.h"

#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "app_shell.h"
#include "esp_attr.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "http_panel_server.h"
#include "utils.h"


/* Constants and macros ---------------------------------------------------- */
// clang-format off

#define WEB_CONSOLE_PAGE_URI            "/console"

/* Oldest data is overwritten when full. Sized to hold a burst (e.g. a full
 * I2C scan) while the push task is throttled by httpd's send rate. */
#define WEB_CONSOLE_RING_BUFFER_SIZE    16384u

#define WEB_CONSOLE_LINE_MAX            256u
#define WEB_CONSOLE_WS_RX_MAX           128u

#define WEB_CONSOLE_PUSH_TASK_STACK     3584u
#define WEB_CONSOLE_PUSH_TASK_PRIO      4u

#define WEB_CONSOLE_PUSH_RECHECK_MS     1000u

/* Pinned to the network core. */
#define WEB_CONSOLE_PUSH_TASK_CORE      0

/* Commands run here, never in the httpd task: a command may block for seconds
 * (sio, sio watch). Pinned to the console core. */
#define WEB_CONSOLE_CMD_TASK_STACK      6144u
#define WEB_CONSOLE_CMD_TASK_PRIO       3u
#define WEB_CONSOLE_CMD_TASK_CORE       1

/* Waiting commands behind the running one. */
#define WEB_CONSOLE_CMD_QUEUE_DEPTH     2u

// clang-format on

/* Private types ----------------------------------------------------------- */

typedef struct web_console_command_s
{
    char line[WEB_CONSOLE_WS_RX_MAX];
} web_console_command_t;


/* Private variables ------------------------------------------------------- */

static const char* web_console_tag = "web_console";

/* PSRAM: never touch from an ISR or while the flash cache is disabled. */
EXT_RAM_BSS_ATTR static char web_console_ring[WEB_CONSOLE_RING_BUFFER_SIZE] = {0};

static volatile size_t web_console_ring_head  = 0u;
static volatile size_t web_console_ring_tail  = 0u;
static portMUX_TYPE web_console_ring_spinlock = portMUX_INITIALIZER_UNLOCKED;

static SemaphoreHandle_t web_console_semaphore = NULL;
static QueueHandle_t web_console_command_queue = NULL;

/* Touched only by the push task. */
static http_panel_push_slot_t web_console_push_slot = {0};

static vprintf_like_t web_console_original_vprintf = NULL;

extern const uint8_t web_console_html_start[] asm("_binary_web_console_html_start");
extern const uint8_t web_console_html_end[] asm("_binary_web_console_html_end");


/* Private functions ------------------------------------------------------- */

/* ---- Ring buffer ---- */

/* Caller holds web_console_ring_spinlock. */
static inline void web_console_ring_write_byte(char character)
{
    web_console_ring[web_console_ring_head] = character;
    web_console_ring_head = (web_console_ring_head + 1u) % WEB_CONSOLE_RING_BUFFER_SIZE;

    if (web_console_ring_head == web_console_ring_tail)
    {
        web_console_ring_tail = (web_console_ring_tail + 1u) % WEB_CONSOLE_RING_BUFFER_SIZE;
    }
}

/* Caller holds web_console_ring_spinlock. */
static size_t web_console_ring_read_line(char* line, size_t line_size)
{
    size_t length = 0u;

    while ((web_console_ring_tail != web_console_ring_head) && (length < (line_size - 1u)))
    {
        const char character = web_console_ring[web_console_ring_tail];

        web_console_ring_tail = (web_console_ring_tail + 1u) % WEB_CONSOLE_RING_BUFFER_SIZE;
        line[length] = character;
        length++;

        if (character == '\n')
        {
            break;
        }
    }

    line[length] = '\0';

    return length;
}

static void web_console_ring_vprintf(const char* fmt, va_list args)
{
    char line[WEB_CONSOLE_LINE_MAX] = {0};
    const int length                = vsnprintf(line, sizeof(line), fmt, args);

    if (length > 0)
    {
        taskENTER_CRITICAL(&web_console_ring_spinlock);

        for (int i = 0; (i < length) && (i < (int)(sizeof(line) - 1u)); i++)
        {
            web_console_ring_write_byte(line[i]);
        }

        taskEXIT_CRITICAL(&web_console_ring_spinlock);

        xSemaphoreGive(web_console_semaphore);
    }
}

/* ---- ESP log vprintf hook ---- */

static int web_console_log_hook(const char* fmt, va_list args)
{
    /* vsnprintf consumes args; keep a copy for the UART passthrough. */
    va_list args_uart;
    va_copy(args_uart, args);

    char line[WEB_CONSOLE_LINE_MAX] = {0};
    const int length                = vsnprintf(line, sizeof(line), fmt, args);

    if (length > 0)
    {
        taskENTER_CRITICAL(&web_console_ring_spinlock);

        for (int i = 0; (i < length) && (i < (int)(sizeof(line) - 1u)); i++)
        {
            web_console_ring_write_byte(line[i]);
        }

        taskEXIT_CRITICAL(&web_console_ring_spinlock);

        xSemaphoreGive(web_console_semaphore);
    }

    if (web_console_original_vprintf != NULL)
    {
        web_console_original_vprintf(fmt, args_uart);
    }

    va_end(args_uart);

    return length;
}

/* ---- Push task ---- */

static void web_console_push_task(void* arg)
{
    ARG_UNUSED(arg);

    char line[WEB_CONSOLE_LINE_MAX] = {0};

    web_console_push_slot.waiter = xTaskGetCurrentTaskHandle();

    while (true)
    {
        xSemaphoreTake(web_console_semaphore, portMAX_DELAY);

        if (http_panel_server_ws_count(WEB_CONSOLE_WS_URI) == 0u)
        {
            taskENTER_CRITICAL(&web_console_ring_spinlock);
            web_console_ring_head = 0u;
            web_console_ring_tail = 0u;
            taskEXIT_CRITICAL(&web_console_ring_spinlock);
            continue;
        }

        while (true)
        {
            size_t length = 0u;

            taskENTER_CRITICAL(&web_console_ring_spinlock);
            length = web_console_ring_read_line(line, sizeof(line));
            taskEXIT_CRITICAL(&web_console_ring_spinlock);

            if (length == 0u)
            {
                break;
            }

            /* Drop the backlog if the last client went away mid-drain. */
            if (http_panel_server_ws_count(WEB_CONSOLE_WS_URI) == 0u)
            {
                break;
            }

            /* One frame in flight at a time; the ring holds the backlog. The
             * timeout only re-checks for a dropped push. */
            while (!http_panel_server_push_ready(&web_console_push_slot))
            {
                ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(WEB_CONSOLE_PUSH_RECHECK_MS));
            }

            http_panel_server_push(
                WEB_CONSOLE_WS_URI, line, length, &web_console_push_slot);
        }
    }
}

/* ---- Command task ---- */

/* Output goes through the ring buffer, so it reaches the page while a long
 * command is still running. */
static void web_console_ws_print(const char* fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    web_console_ring_vprintf(fmt, args);
    va_end(args);
}

static void web_console_command_task(void* arg)
{
    ARG_UNUSED(arg);

    web_console_command_t command = {0};

    while (true)
    {
        if (xQueueReceive(web_console_command_queue, &command, portMAX_DELAY) == pdTRUE)
        {
            app_shell_dispatch(command.line, web_console_ws_print);
        }
    }
}

/* ---- HTTP handlers ---- */

static esp_err_t web_console_h_page(httpd_req_t* request)
{
    const size_t length = (size_t)(web_console_html_end - web_console_html_start);

    httpd_resp_set_type(request, "text/html");

    return httpd_resp_send(request, (const char*)web_console_html_start, (ssize_t)length);
}

/* IDF 6.0 no longer invokes the URI handler on the handshake GET (commit
 * c9daea6, issue #18539), so the fd is registered here. */
static esp_err_t web_console_ws_connected(httpd_req_t* request)
{
    const int fd = httpd_req_to_sockfd(request);

    /* An error makes httpd close the socket, so the page reconnects instead of
     * showing "Connected" without ever getting a push. */
    const esp_err_t error_code = http_panel_server_ws_add(fd, WEB_CONSOLE_WS_URI);

    if (error_code == ESP_OK)
    {
        ESP_LOGI(web_console_tag, "Client connected fd=%d", fd);
    }

    return error_code;
}

static esp_err_t web_console_h_ws(httpd_req_t* request)
{
    /* IDF 6.0: only invoked for inbound data frames. */
    uint8_t buffer[WEB_CONSOLE_WS_RX_MAX] = {0};
    httpd_ws_frame_t frame = {.type = HTTPD_WS_TYPE_TEXT, .payload = buffer};

    const esp_err_t error_code = httpd_ws_recv_frame(request, &frame, sizeof(buffer) - 1u);

    if (error_code == ESP_OK)
    {
        web_console_command_t command = {0};

        buffer[frame.len] = '\0';
        strlcpy(command.line, (const char*)buffer, sizeof(command.line));

        /* Never wait here: httpd serves every page. */
        if (xQueueSend(web_console_command_queue, &command, 0u) != pdTRUE)
        {
            web_console_write("busy: previous commands are still running\n");
        }
    }

    return error_code;
}


/* Exposed API ------------------------------------------------------------- */

void web_console_write(const char* fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    web_console_ring_vprintf(fmt, args);
    va_end(args);
}

esp_err_t web_console_init(void)
{
    esp_err_t error_code = ESP_OK;

    /* Static: http_panel_server keeps the URIs by pointer. */
    static const httpd_uri_t web_console_uris[] = {
        {
            .uri     = WEB_CONSOLE_PAGE_URI,
            .method  = HTTP_GET,
            .handler = web_console_h_page,
        },
        {
            .uri                  = WEB_CONSOLE_WS_URI,
            .method               = HTTP_GET,
            .handler              = web_console_h_ws,
            .is_websocket         = true,
            .ws_post_handshake_cb = web_console_ws_connected,
        },
    };

    do
    {
        web_console_semaphore = xSemaphoreCreateBinary();

        if (web_console_semaphore == NULL)
        {
            error_code = ESP_ERR_NO_MEM;
            break;
        }

        web_console_command_queue =
            xQueueCreate(WEB_CONSOLE_CMD_QUEUE_DEPTH, sizeof(web_console_command_t));

        if (web_console_command_queue == NULL)
        {
            error_code = ESP_ERR_NO_MEM;
            break;
        }

        for (size_t i = 0u; i < ARRAY_SIZE(web_console_uris); i++)
        {
            error_code = http_panel_server_register_uri(&web_console_uris[i]);

            if (error_code != ESP_OK)
            {
                break;
            }
        }

        if (error_code != ESP_OK)
        {
            break;
        }

        const BaseType_t task_created = xTaskCreatePinnedToCore(
            web_console_push_task,
            "web_console",
            WEB_CONSOLE_PUSH_TASK_STACK,
            NULL,
            WEB_CONSOLE_PUSH_TASK_PRIO,
            NULL,
            WEB_CONSOLE_PUSH_TASK_CORE);

        if (task_created != pdPASS)
        {
            error_code = ESP_FAIL;
            break;
        }

        const BaseType_t command_task_created = xTaskCreatePinnedToCore(
            web_console_command_task,
            "web_console_cmd",
            WEB_CONSOLE_CMD_TASK_STACK,
            NULL,
            WEB_CONSOLE_CMD_TASK_PRIO,
            NULL,
            WEB_CONSOLE_CMD_TASK_CORE);

        if (command_task_created != pdPASS)
        {
            error_code = ESP_FAIL;
            break;
        }

        /* Install the log hook last. */
        web_console_original_vprintf = esp_log_set_vprintf(web_console_log_hook);

        ESP_LOGI(web_console_tag, "Ready at %s", WEB_CONSOLE_PAGE_URI);
    } while (0);

    return error_code;
}

/**** END OF FILE ****/
