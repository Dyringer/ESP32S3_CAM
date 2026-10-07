/**
 ******************************************************************************
 * @file    http_panel_server.h
 * @author  Daniel Dyringer
 * @date    2026-03-09
 * @brief   HTTP panel core — server lifecycle, WebSocket client table, and
 *          push API used by individual page modules.
 *
 * @note    This header is private to the http_panel component.
 *          It must not be listed in CMakeLists INCLUDE_DIRS.
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

/* Constants and macros ---------------------------------------------------- */
// clang-format off

/** @brief Maximum number of simultaneous WebSocket clients across all pages. */
#define HTTP_PANEL_SERVER_MAX_WS_CLIENTS    4u

/**
 * @brief httpd open-socket budget. httpd reserves 3 sockets for itself; the
 *        remainder covers kept-alive page loads plus WS clients across pages.
 *        Must not exceed CONFIG_LWIP_MAX_SOCKETS (16).
 */
#define HTTP_PANEL_SERVER_MAX_OPEN_SOCKETS  13u

/** @brief HTTP server port. */
#define HTTP_PANEL_SERVER_PORT              80u

/** @brief httpd task stack size in bytes. */
#define HTTP_PANEL_SERVER_TASK_STACK        6144u

// clang-format on

/* Types ------------------------------------------------------------------- */

/**
 * @brief Server lifecycle callback.
 *
 * @param running  true right after the server has started and all URIs are
 *                 registered; false right BEFORE it is stopped, while the
 *                 handle is still valid. Invoked from the panel monitor task.
 *
 * @note  The "stopping" call is the last chance to finish async requests
 *        (httpd_req_async_handler_begin) — completing one after
 *        httpd_stop() touches freed server memory. Block until done.
 */
typedef void (*http_panel_server_lifecycle_cb_t)(bool running);

/* Exposed API ------------------------------------------------------------- */

/**
 * @brief Initialise the server core: create the WS mutex, start the monitor
 *        task.  Call once from http_panel_init().
 */
esp_err_t http_panel_server_init(void);

/**
 * @brief Register a URI handler with the running httpd instance.
 *        Safe to call before the server starts — handlers are queued and
 *        registered when the server comes up.
 *
 * @param uri  Fully populated httpd_uri_t (must remain valid for the
 *             lifetime of the server).
 */
esp_err_t http_panel_server_register_uri(const httpd_uri_t *uri);

/**
 * @brief Register a callback for server start/stop.
 *        Call from a page's init, before the server first starts.
 *
 * @return ESP_OK, or ESP_ERR_NO_MEM if the callback table is full.
 */
esp_err_t http_panel_server_register_lifecycle_cb(http_panel_server_lifecycle_cb_t cb);

/**
 * @brief Push a text frame to all currently connected WebSocket clients on
 *        a given URI path.
 *
 *        Queues one httpd_queue_work item per connected client.
 *        Removes stale fds on send failure.
 *
 * @param ws_uri   URI string used to filter clients (e.g. "/ws").
 *                 Pass NULL to push to all clients regardless of URI.
 * @param data     Text payload (need not be NUL-terminated beyond @p len).
 * @param len      Payload length in bytes.
 * @param pending  Optional caller-owned flag.  Set to true before calling;
 *                 cleared to false by the work callback once the send
 *                 completes (or fails).  Pass NULL if not needed.
 *
 * @note  The pending flag pattern prevents queuing a new push before the
 *        previous one has been delivered, avoiding httpd work-queue backlog
 *        and heap fragmentation under high push rates.
 */
void http_panel_server_push(const char  *ws_uri,
                             const char  *data,
                             size_t       len,
                             volatile bool *pending);

/**
 * @brief Register a WebSocket client fd when its handshake completes.
 *        Call from within a WebSocket URI handler on HTTP_GET.
 *
 * @param fd      Socket fd from httpd_req_to_sockfd().
 * @param ws_uri  The URI this client connected on (stored for filtering).
 */
void http_panel_server_ws_add(int fd, const char *ws_uri);

/**
 * @brief Remove a WebSocket client fd (e.g. on confirmed disconnect).
 *
 * @param fd  Socket fd to remove.
 */
void http_panel_server_ws_remove(int fd);

/**
 * @brief Return the number of WebSocket clients currently connected on
 *        @p ws_uri.  Pass NULL to count across all URIs.
 *
 * @note  Acquires the WS mutex briefly; safe to call from any task.
 */
uint8_t http_panel_server_ws_count(const char *ws_uri);

/**
 * @brief Return the active httpd handle, or NULL if the server is not running.
 */
httpd_handle_t http_panel_server_handle(void);

#endif /* HTTP_PANEL_SERVER_H_ */

/**** END OF FILE ****/
