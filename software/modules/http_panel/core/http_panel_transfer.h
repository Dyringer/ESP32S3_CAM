/**
 ******************************************************************************
 * @file    http_panel_transfer.h
 * @author  Daniel Dyringer
 * @date    2026-10-25
 * @brief   Shared upload/download worker for http_panel pages.
 *
 *          A page handler validates the request, then hands it over with
 *          http_panel_transfer_submit(). The request is detached with
 *          httpd_req_async_handler_begin() and the body is received (upload)
 *          or sent (download) by one worker task, so the httpd task never
 *          blocks on a transfer. One transfer at a time, server-wide: a
 *          second submit gets 409.
 *
 *          On server stop the module shuts the transfer socket down and waits
 *          for the worker to release the request (async requests must
 *          complete before httpd_stop() frees the server).
 ******************************************************************************
 */

/* Define to prevent recursive inclusion ----------------------------------- */
#ifndef HTTP_PANEL_TRANSFER_H_
#define HTTP_PANEL_TRANSFER_H_

/* Includes ---------------------------------------------------------------- */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "esp_http_server.h"

/* Constants and macros ---------------------------------------------------- */
// clang-format off

/* Largest context copied by http_panel_transfer_submit(). */
#define HTTP_PANEL_TRANSFER_CONTEXT_MAX     192u

// clang-format on

/* Types ------------------------------------------------------------------- */

/* All callbacks run in the transfer task, with context pointing at the
 * module's copy of the submitted context. Exactly one of write/read is set:
 * write = upload (request body is received), read = download (response body
 * is sent chunked).
 *
 * open:  may set response headers (type, Content-Disposition). Its error
 *        picks the HTTP status: ESP_ERR_INVALID_ARG 400, ESP_ERR_NOT_FOUND
 *        404, ESP_ERR_INVALID_STATE 409, ESP_ERR_INVALID_SIZE 413, else 500.
 * read:  *length = 0 means end of data.
 * close: always called once open succeeded; success = false on any failure
 *        (including a dropped client) — abort/clean up. Its error turns an
 *        upload into a 500.
 *
 * Upload success is answered with {"ok":true}; any error before the response
 * starts with {"ok":false,"error":"..."}. A download that fails mid-stream
 * just closes the socket. */
typedef struct http_panel_transfer_ops_s
{
    esp_err_t (*open)(void* context, httpd_req_t* request);
    esp_err_t (*write)(void* context, const uint8_t* data, size_t length);
    esp_err_t (*read)(void* context, uint8_t* buffer, size_t capacity, size_t* length);
    esp_err_t (*close)(void* context, bool success);
} http_panel_transfer_ops_t;

/* Exposed API ------------------------------------------------------------- */

/* Before any page init: registers the lifecycle callback. */
esp_err_t http_panel_transfer_init(void);

/* Call from an httpd handler and return its result. Copies context_size
 * (<= HTTP_PANEL_TRANSFER_CONTEXT_MAX) bytes of context; ops is kept by
 * pointer (pass a static). If the transfer can't be queued, the error
 * response (409 busy, 503 stopping, 500) is sent here. */
esp_err_t http_panel_transfer_submit(
    httpd_req_t* request,
    const http_panel_transfer_ops_t* ops,
    const void* context,
    size_t context_size);

#endif /* HTTP_PANEL_TRANSFER_H_ */

/**** END OF FILE ****/
