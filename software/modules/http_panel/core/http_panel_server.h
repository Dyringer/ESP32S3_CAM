/**
 ******************************************************************************
 * @file    http_panel_server.h
 * @author  Daniel Dyringer
 * @date    2026-03-09
 * @brief   HTTP panel core — server lifecycle, WebSocket client table, and
 *          push API used by individual page modules.
 *
 *          URIs (stored by pointer, must stay valid) and lifecycle callbacks
 *          may be registered before the server starts; they are applied every
 *          time it comes up.
 ******************************************************************************
 */

/* Define to prevent recursive inclusion ----------------------------------- */
#ifndef HTTP_PANEL_SERVER_H_
#define HTTP_PANEL_SERVER_H_

/* Includes ---------------------------------------------------------------- */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "esp_http_server.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"

/* Constants and macros ---------------------------------------------------- */
// clang-format off

#define HTTP_PANEL_SERVER_MAX_WS_CLIENTS    4u

/* httpd uses 3 more sockets internally; CONFIG_LWIP_MAX_SOCKETS leaves the
 * rest for snapshot_upload and DNS. */
#define HTTP_PANEL_SERVER_MAX_OPEN_SOCKETS  16u

#define HTTP_PANEL_SERVER_PORT              80u
#define HTTP_PANEL_SERVER_TASK_STACK        6144u

// clang-format on

_Static_assert(
    (HTTP_PANEL_SERVER_MAX_OPEN_SOCKETS + 3u) <= CONFIG_LWIP_MAX_SOCKETS,
    "httpd needs 3 sockets on top of max_open_sockets");

/* Types ------------------------------------------------------------------- */

/* Called from the httpd task once the last in-flight push of a slot has been
 * sent (or dropped because the client is gone). Must not block. */
typedef void (*http_panel_server_push_done_cb_t)(void* context);

/* Called from the httpd task for every closed socket, whether or not it was a
 * registered WebSocket client. Must not block. */
typedef void (*http_panel_server_ws_close_cb_t)(int fd);

/* running = false is called just BEFORE httpd_stop(), from the monitor task,
 * while the handle is still valid: it is the last chance to finish async
 * requests (httpd_req_async_handler_begin) — completing one after httpd_stop()
 * touches freed server memory. Block until done. */
typedef void (*http_panel_server_lifecycle_cb_t)(bool running);

/* One per pushing page. Counts the pushes queued but not yet sent (one per
 * client for a broadcast), so a second push is not queued before the first is
 * out — httpd's work queue drops items silently on overflow. Incremented by the
 * pushing task, decremented by the httpd task. */
typedef struct http_panel_push_slot_s
{
    volatile uint32_t in_flight; /* accessed with atomic operations only */
    int64_t since_us;
    TaskHandle_t waiter;                   /* optional, notified after every send */
    http_panel_server_push_done_cb_t done; /* optional */
    void* context;                         /* passed to done */
} http_panel_push_slot_t;

/* Exposed API ------------------------------------------------------------- */

esp_err_t http_panel_server_init(void);
esp_err_t http_panel_server_enable(void);

esp_err_t http_panel_server_register_uri(const httpd_uri_t* uri);
esp_err_t http_panel_server_register_lifecycle_cb(http_panel_server_lifecycle_cb_t callback);
esp_err_t http_panel_server_register_ws_close_cb(http_panel_server_ws_close_cb_t callback);

/* False while any push of the slot is in flight. A push still in flight
 * after HTTP_PANEL_SERVER_PUSH_LOST_MS is treated as dropped, so a lost work
 * item can't freeze the page. */
bool http_panel_server_push_ready(http_panel_push_slot_t* slot);

/* ws_uri is kept by pointer until the send runs (pass a string literal);
 * NULL pushes to every WS client. */
void http_panel_server_push(
    const char* ws_uri,
    const char* data,
    size_t length,
    http_panel_push_slot_t* slot);

/* Same slot semantics as http_panel_server_push, for one client. The data is
 * copied. ESP_ERR_INVALID_STATE when the server is down or fd is not a client
 * of ws_uri (the slot is left untouched); ESP_ERR_TIMEOUT when the client table
 * lock was not obtained (transient, retry later). Any error leaves the slot
 * untouched. */
esp_err_t http_panel_server_push_fd(
    int fd,
    const char* ws_uri,
    const char* data,
    size_t length,
    http_panel_push_slot_t* slot);

/* Binary frame, otherwise the same as http_panel_server_push_fd. */
esp_err_t http_panel_server_push_binary_fd(
    int fd,
    const char* ws_uri,
    const uint8_t* data,
    size_t length,
    http_panel_push_slot_t* slot);

/* ESP_ERR_NO_MEM when the client table is full: the client would get no
 * pushes, so the caller should refuse the connection. */
esp_err_t http_panel_server_ws_add(int fd, const char* ws_uri);
void http_panel_server_ws_remove(int fd);
/* Asks httpd to close the socket (it runs the close callbacks afterwards), so
 * the page reconnects. ESP_ERR_INVALID_STATE when the server is down,
 * ESP_ERR_TIMEOUT on a lock timeout. Does not block on httpd. */
esp_err_t http_panel_server_close_client(int fd);
uint8_t http_panel_server_ws_count(const char* ws_uri);

httpd_handle_t http_panel_server_handle(void);

#endif /* HTTP_PANEL_SERVER_H_ */

/**** END OF FILE ****/
