/**
 ******************************************************************************
 * @file    web_console.c
 * @author  Daniel Dyringer
 * @date    2026-03-09
 * @brief   Web console — ESP log capture and WebSocket push.
 *
 *          Responsibilities:
 *            - 4 KB spinlock-protected ring buffer.
 *            - ESP log vprintf hook: captures all ESP_LOG* output.
 *            - Push task: drains the ring buffer line-by-line and sends
 *              each line as a WS text frame to /console/ws clients.
 *              Discards the buffer when no clients are connected.
 *            - HTTP handler: serves web_console.html at GET /console.
 *            - WS handler: tracks client fds; forwards inbound frames to
 *              app_shell for dispatch via web_console_write() output.
 *
 *          This module has no knowledge of commands or the shell.
 *          It exposes web_console_write() for other modules to inject output.
 ******************************************************************************
 */


/* Includes ---------------------------------------------------------------- */
#include "web_console.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdbool.h>
#include <string.h>

#include "esp_http_server.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "app_shell.h"
#include "http_panel_server.h"


/* Constants and macros ---------------------------------------------------- */
// clang-format off

#define WEB_CONSOLE_PAGE_URI        "/console"

/** @brief Ring buffer capacity in bytes. Oldest data overwritten when full.
 *         Sized to hold a burst (e.g. a full I2C scan) until the push task,
 *         which is throttled by httpd's send rate, can drain it. */
#define WEB_CONSOLE_RING_BUF_SIZE   16384u

/** @brief Maximum length of one formatted line (stack-allocated). */
#define WEB_CONSOLE_LINE_MAX        256u

/** @brief Maximum length of an inbound WS frame (command line). */
#define WEB_CONSOLE_WS_RX_MAX       128u

#define WEB_CONSOLE_PUSH_TASK_STACK 4096u
#define WEB_CONSOLE_PUSH_TASK_PRIO  4u

/** @brief Max time to wait for the previous push to complete before sending
 *         the next frame anyway (self-heals a lost pending flag). */
#define WEB_CONSOLE_PUSH_TIMEOUT_MS 200u

/** @brief Core for the web console push task — pinned to the network core. */
#define WEB_CONSOLE_PUSH_TASK_CORE  0

// clang-format on


/* Private variables ------------------------------------------------------- */

static const char *web_console_tag = "web_console";

/* Ring buffer */
static char            web_console_ring[WEB_CONSOLE_RING_BUF_SIZE];
static volatile size_t web_console_ring_head = 0u;
static volatile size_t web_console_ring_tail = 0u;
static portMUX_TYPE    web_console_ring_mux  = portMUX_INITIALIZER_UNLOCKED;

/* Semaphore signalled when data is written to the ring buffer */
static SemaphoreHandle_t web_console_sem = NULL;

/* True while a push frame is queued in the httpd task; cleared by the push
 * work callback once the frame is delivered. The drain loop waits on this so
 * it never queues frames faster than httpd can send them (which would overflow
 * the bounded httpd work queue and silently drop log lines). */
static volatile bool web_console_push_pending = false;

/* Original vprintf saved before we install our hook — used to preserve UART */
static vprintf_like_t web_console_orig_vprintf = NULL;

/* Embedded HTML */
extern const uint8_t web_console_html_start[]
    asm("_binary_web_console_html_start");
extern const uint8_t web_console_html_end[]
    asm("_binary_web_console_html_end");


/* Private functions ------------------------------------------------------- */

/* ---- Ring buffer -------------------------------------------------------- */

/**
 * @brief Write one byte into the ring buffer, overwriting oldest if full.
 * @note  Must be called with web_console_ring_mux held.
 */
static inline void web_console_ring_write_byte(char c)
{
    web_console_ring[web_console_ring_head] = c;
    web_console_ring_head =
        (web_console_ring_head + 1u) % WEB_CONSOLE_RING_BUF_SIZE;

    if (web_console_ring_head == web_console_ring_tail)
    {
        web_console_ring_tail =
            (web_console_ring_tail + 1u) % WEB_CONSOLE_RING_BUF_SIZE;
    }
}

/**
 * @brief Read up to the next newline (or until empty) into @p out.
 *        Returns number of bytes written (excluding NUL).
 * @note  Must be called with web_console_ring_mux held.
 */
static size_t web_console_ring_read_line(char *out, size_t out_size)
{
    size_t n = 0u;

    while (web_console_ring_tail != web_console_ring_head
           && n < (out_size - 1u))
    {
        const char c = web_console_ring[web_console_ring_tail];
        web_console_ring_tail =
            (web_console_ring_tail + 1u) % WEB_CONSOLE_RING_BUF_SIZE;
        out[n++] = c;

        if (c == '\n')
        {
            break;
        }
    }

    out[n] = '\0';
    return n;
}

/**
 * @brief Write a formatted string into the ring buffer and signal the
 *        push task.  Safe from any task context (uses spinlock).
 */
static void web_console_ring_vprintf(const char *fmt, va_list args)
{
    char      line[WEB_CONSOLE_LINE_MAX];
    const int len = vsnprintf(line, sizeof(line), fmt, args);

    if (len <= 0)
    {
        return;
    }

    taskENTER_CRITICAL(&web_console_ring_mux);

    for (int i = 0; i < len && i < (int)(sizeof(line) - 1); i++)
    {
        web_console_ring_write_byte(line[i]);
    }

    taskEXIT_CRITICAL(&web_console_ring_mux);

    xSemaphoreGive(web_console_sem);
}

/* ---- ESP log vprintf hook ----------------------------------------------- */

static int web_console_log_hook(const char *fmt, va_list args)
{
    /* Copy args before vsnprintf consumes them — needed for UART passthrough */
    va_list args_uart;
    va_copy(args_uart, args);

    char      line[WEB_CONSOLE_LINE_MAX];
    const int len = vsnprintf(line, sizeof(line), fmt, args);

    if (len > 0)
    {
        taskENTER_CRITICAL(&web_console_ring_mux);

        for (int i = 0; i < len && i < (int)(sizeof(line) - 1); i++)
        {
            web_console_ring_write_byte(line[i]);
        }

        taskEXIT_CRITICAL(&web_console_ring_mux);

        xSemaphoreGive(web_console_sem);
    }

    /* Forward to UART so serial monitor output is preserved */
    if (web_console_orig_vprintf)
    {
        web_console_orig_vprintf(fmt, args_uart);
    }

    va_end(args_uart);

    return len;
}

/* ---- Push task ---------------------------------------------------------- */

static void web_console_push_task(void *pv)
{
    char line[WEB_CONSOLE_LINE_MAX];

    while (true)
    {
        xSemaphoreTake(web_console_sem, portMAX_DELAY);

        if (http_panel_server_ws_count(WEB_CONSOLE_WS_URI) == 0u)
        {
            /* No clients — discard buffer to prevent stale accumulation */
            taskENTER_CRITICAL(&web_console_ring_mux);
            web_console_ring_head = 0u;
            web_console_ring_tail = 0u;
            taskEXIT_CRITICAL(&web_console_ring_mux);
            continue;
        }

        while (true)
        {
            size_t n;

            taskENTER_CRITICAL(&web_console_ring_mux);
            n = web_console_ring_read_line(line, sizeof(line));
            taskEXIT_CRITICAL(&web_console_ring_mux);

            if (n == 0u)
            {
                break;
            }

            /* Drop the backlog if the last client went away mid-drain. */
            if (http_panel_server_ws_count(WEB_CONSOLE_WS_URI) == 0u)
            {
                break;
            }

            /* Wait for the previous frame to be delivered before queuing the
             * next, so we never overflow httpd's bounded work queue (which
             * silently drops frames). This throttles the drain to httpd's
             * actual send rate; the ring buffers the backlog meanwhile. The
             * bounded wait self-heals if the push callback never clears the
             * flag (e.g. the client disconnected before the frame was sent). */
            uint32_t wait_ms = 0u;

            while (web_console_push_pending && wait_ms < WEB_CONSOLE_PUSH_TIMEOUT_MS)
            {
                vTaskDelay(pdMS_TO_TICKS(2u));
                wait_ms += 2u;
            }

            web_console_push_pending = true;
            http_panel_server_push(WEB_CONSOLE_WS_URI, line, n,
                                   &web_console_push_pending);
        }
    }
}

/* ---- WebSocket print callback ------------------------------------------- */

/**
 * @brief app_shell_print_fn_t implementation for the web console transport.
 *        Formats the string and writes it into the ring buffer so it is
 *        pushed to all connected /console/ws clients.
 */
static void web_console_ws_print(const char *fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    web_console_ring_vprintf(fmt, args);
    va_end(args);
}

/* ---- HTTP handlers ------------------------------------------------------ */

static esp_err_t web_console_h_page(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html");
    const size_t len =
        (size_t)(web_console_html_end - web_console_html_start);
    return httpd_resp_send(
        req, (const char *)web_console_html_start, (ssize_t)len);
}

/* Called by httpd right after the WS handshake completes. In ESP-IDF 6.0 the
 * URI handler is no longer invoked on the handshake GET (commit c9daea6,
 * issue #18539), so the client fd is registered here instead. */
static esp_err_t web_console_ws_connected(httpd_req_t *req)
{
    const int fd = httpd_req_to_sockfd(req);
    http_panel_server_ws_add(fd, WEB_CONSOLE_WS_URI);
    ESP_LOGI(web_console_tag, "Client connected fd=%d", fd);
    return ESP_OK;
}

static esp_err_t web_console_h_ws(httpd_req_t *req)
{
    /* In IDF 6.0 this handler is only invoked for inbound data frames; the
     * handshake GET is handled by web_console_ws_connected() above. */

    /* Inbound frame — hand off to the shell for dispatch */
    uint8_t          buf[WEB_CONSOLE_WS_RX_MAX] = {0};
    httpd_ws_frame_t frm = { .type    = HTTPD_WS_TYPE_TEXT,
                              .payload = buf };

    esp_err_t error_code =
        httpd_ws_recv_frame(req, &frm, sizeof(buf) - 1u);

    if (error_code == ESP_OK)
    {
        buf[frm.len] = '\0';
        app_shell_dispatch((char *)buf, web_console_ws_print);
    }

    return error_code;
}

/* ---- URI descriptors ---------------------------------------------------- */

static const httpd_uri_t web_console_uri_page = {
    .uri     = WEB_CONSOLE_PAGE_URI,
    .method  = HTTP_GET,
    .handler = web_console_h_page,
};

static const httpd_uri_t web_console_uri_ws = {
    .uri                 = WEB_CONSOLE_WS_URI,
    .method              = HTTP_GET,
    .handler             = web_console_h_ws,
    .is_websocket        = true,
    .ws_post_handshake_cb = web_console_ws_connected,
};


/* Exposed API ------------------------------------------------------------- */

void web_console_write(const char *fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    web_console_ring_vprintf(fmt, args);
    va_end(args);
}

esp_err_t web_console_init(void)
{
    esp_err_t error_code = ESP_OK;

    do
    {
        web_console_sem = xSemaphoreCreateBinary();

        if (!web_console_sem)
        {
            error_code = ESP_ERR_NO_MEM;
            break;
        }

        error_code = http_panel_server_register_uri(&web_console_uri_page);

        if (error_code != ESP_OK)
        {
            break;
        }

        error_code = http_panel_server_register_uri(&web_console_uri_ws);

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

        /* Install log hook last; save the original so UART output is preserved */
        web_console_orig_vprintf = esp_log_set_vprintf(web_console_log_hook);

        ESP_LOGI(web_console_tag, "Ready at %s", WEB_CONSOLE_PAGE_URI);
    } while (0);

    return error_code;
}


/**** END OF FILE ****/
