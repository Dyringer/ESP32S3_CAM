/**
 ******************************************************************************
 * @file    data_manager_page.c
 * @author  Daniel Dyringer
 * @date    2026-10-13
 * @brief   Data manager page — view data entries, edit and save settings.
 *
 *          Persistent entries are settings: editable, resettable and saved
 *          with data_manager_save(). Volatile entries (state, measurements)
 *          are read-only here. Secret values never leave the device.
 *
 *          No auth/CSRF — same LAN-only trust model as the rest of
 *          http_panel. This page can change WiFi and upload credentials.
 ******************************************************************************
 */


/* Includes ---------------------------------------------------------------- */
#include "data_manager_page.h"

#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "data_manager.h"
#include "data_manager_text.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "http_panel_server.h"
#include "utils.h"


/* Constants and macros ---------------------------------------------------- */
// clang-format off

/* Fits the longest string setting as JSON, escapes included. */
#define DATA_MANAGER_PAGE_BODY_MAX      512u
#define DATA_MANAGER_PAGE_RECV_RETRIES  3u

// clang-format on

/* Private types ----------------------------------------------------------- */


/* Private variables ------------------------------------------------------- */

static const char* data_manager_page_tag = "data_manager_page";

extern const uint8_t data_manager_page_html_start[] asm(
    "_binary_data_manager_page_html_start");
extern const uint8_t data_manager_page_html_end[] asm(
    "_binary_data_manager_page_html_end");


/* Private functions ------------------------------------------------------- */

/* ---- List ---- */

static bool data_manager_page_has_value(const uint8_t* buffer, size_t size)
{
    bool has_value = false;

    for (size_t i = 0u; i < size; i++)
    {
        if (buffer[i] != 0u)
        {
            has_value = true;
        }
    }

    return has_value;
}

static void data_manager_page_add_value(cJSON* item, const data_manager_descriptor_t* descriptor)
{
    const size_t text_size = data_manager_text_size(descriptor);
    uint8_t* const buffer  = calloc(1u, descriptor->size);
    char* const text       = calloc(1u, text_size);
    esp_err_t error_code   = ESP_ERR_NO_MEM;

    if ((buffer != NULL) && (text != NULL))
    {
        error_code = data_manager_get(descriptor->id, buffer, descriptor->size);
    }

    if ((error_code == ESP_OK) && !descriptor->secret)
    {
        error_code = data_manager_text_format(descriptor, buffer, text, text_size);
    }

    if (error_code != ESP_OK)
    {
        cJSON_AddStringToObject(item, "error", esp_err_to_name(error_code));
    }
    else if (descriptor->secret)
    {
        cJSON_AddBoolToObject(
            item, "has_value", data_manager_page_has_value(buffer, descriptor->size));
    }
    else
    {
        cJSON_AddStringToObject(item, "value", text);
    }

    free(text);
    free(buffer);
}

static esp_err_t data_manager_page_add_entry(
    cJSON* entries,
    const data_manager_descriptor_t* descriptor)
{
    esp_err_t error_code = ESP_OK;
    cJSON* const item    = cJSON_CreateObject();

    if (item == NULL)
    {
        error_code = ESP_ERR_NO_MEM;
    }
    else
    {
        cJSON_AddStringToObject(item, "name", descriptor->name);
        cJSON_AddStringToObject(
            item, "type", data_manager_text_type_name(descriptor->type));
        cJSON_AddNumberToObject(item, "size", (double)descriptor->size);
        cJSON_AddBoolToObject(item, "persistent", descriptor->persistent);
        cJSON_AddBoolToObject(item, "secret", descriptor->secret);

        if (descriptor->minimum < descriptor->maximum)
        {
            cJSON_AddNumberToObject(item, "min", (double)descriptor->minimum);
            cJSON_AddNumberToObject(item, "max", (double)descriptor->maximum);
        }

        data_manager_page_add_value(item, descriptor);
        cJSON_AddItemToArray(entries, item);
    }

    return error_code;
}

static char* data_manager_page_build_list(void)
{
    char* json  = NULL;
    cJSON* root = NULL;

    do
    {
        root = cJSON_CreateObject();

        if (root == NULL)
        {
            break;
        }

        cJSON_AddBoolToObject(root, "unsaved", data_manager_unsaved());

        cJSON* const entries = cJSON_AddArrayToObject(root, "entries");

        if (entries == NULL)
        {
            break;
        }

        const size_t count   = data_manager_count();
        esp_err_t error_code = ESP_OK;

        for (size_t i = 0u; (error_code == ESP_OK) && (i < count); i++)
        {
            const data_manager_descriptor_t* const descriptor = data_manager_at(i);

            if (descriptor != NULL)
            {
                error_code = data_manager_page_add_entry(entries, descriptor);
            }
        }

        if (error_code != ESP_OK)
        {
            break;
        }

        json = cJSON_PrintUnformatted(root);
    } while (0);

    cJSON_Delete(root);

    return json; /* caller must free() */
}

/* ---- Requests ---- */

static esp_err_t data_manager_page_receive_json(httpd_req_t* request, cJSON** root)
{
    esp_err_t error_code = ESP_OK;
    char* body           = NULL;

    do
    {
        const size_t length = request->content_len;

        if ((length == 0u) || (length > DATA_MANAGER_PAGE_BODY_MAX))
        {
            error_code = ESP_ERR_INVALID_SIZE;
            break;
        }

        body = calloc(1u, length + 1u);

        if (body == NULL)
        {
            error_code = ESP_ERR_NO_MEM;
            break;
        }

        size_t received = 0u;
        uint8_t retries = 0u;

        while ((error_code == ESP_OK) && (received < length))
        {
            const int result = httpd_req_recv(request, &body[received], length - received);

            if ((result == HTTPD_SOCK_ERR_TIMEOUT)
                && (retries < DATA_MANAGER_PAGE_RECV_RETRIES))
            {
                retries++;
            }
            else if (result <= 0)
            {
                error_code = ESP_FAIL;
            }
            else
            {
                received += (size_t)result;
            }
        }

        if (error_code != ESP_OK)
        {
            break;
        }

        *root = cJSON_Parse(body);

        if (*root == NULL)
        {
            error_code = ESP_ERR_INVALID_ARG;
        }
    } while (0);

    free(body);

    return error_code;
}

static esp_err_t data_manager_page_find_editable(
    const cJSON* root,
    const data_manager_descriptor_t** descriptor)
{
    esp_err_t error_code         = ESP_OK;
    const cJSON* const json_name = cJSON_GetObjectItemCaseSensitive(root, "name");

    *descriptor = NULL;

    if (!cJSON_IsString(json_name))
    {
        error_code = ESP_ERR_INVALID_ARG;
    }
    else
    {
        *descriptor = data_manager_find(json_name->valuestring);

        if (*descriptor == NULL)
        {
            error_code = ESP_ERR_NOT_FOUND;
        }
        else if (!(*descriptor)->persistent)
        {
            error_code = ESP_ERR_NOT_ALLOWED;
        }
        else
        {
            /* Editable setting. */
        }
    }

    return error_code;
}

static esp_err_t data_manager_page_send_result(httpd_req_t* request, esp_err_t result)
{
    esp_err_t error_code = ESP_OK;

    if (result != ESP_OK)
    {
        ESP_LOGW(
            data_manager_page_tag, "%s failed: %s", request->uri, esp_err_to_name(result));
    }

    switch (result)
    {
        case ESP_OK:
        {
            error_code = httpd_resp_sendstr(request, "OK");
        }
        break;
        case ESP_ERR_NOT_FOUND:
        {
            error_code =
                httpd_resp_send_err(request, HTTPD_404_NOT_FOUND, "Unknown entry");
        }
        break;
        case ESP_ERR_NOT_ALLOWED:
        {
            error_code =
                httpd_resp_send_err(request, HTTPD_403_FORBIDDEN, "Read-only entry");
        }
        break;
        case ESP_ERR_INVALID_ARG:
        {
            error_code =
                httpd_resp_send_err(request, HTTPD_400_BAD_REQUEST, "Invalid value");
        }
        break;
        case ESP_ERR_INVALID_SIZE:
        {
            error_code = httpd_resp_send_err(
                request, HTTPD_400_BAD_REQUEST, "Value too long or wrong size");
        }
        break;
        default:
        {
            error_code = httpd_resp_send_err(
                request, HTTPD_500_INTERNAL_SERVER_ERROR, esp_err_to_name(result));
        }
        break;
    }

    return error_code;
}

/* ---- HTTP handlers ---- */

static esp_err_t data_manager_page_h_root(httpd_req_t* request)
{
    const size_t length =
        (size_t)(data_manager_page_html_end - data_manager_page_html_start);

    httpd_resp_set_type(request, "text/html");

    return httpd_resp_send(
        request, (const char*)data_manager_page_html_start, (ssize_t)length);
}

static esp_err_t data_manager_page_h_list(httpd_req_t* request)
{
    esp_err_t error_code = ESP_OK;
    char* json           = data_manager_page_build_list();

    if (json == NULL)
    {
        error_code = httpd_resp_send_500(request);
    }
    else
    {
        httpd_resp_set_type(request, "application/json");
        httpd_resp_set_hdr(request, "Cache-Control", "no-store");
        error_code = httpd_resp_sendstr(request, json);
        free(json);
    }

    return error_code;
}

static esp_err_t data_manager_page_h_set(httpd_req_t* request)
{
    cJSON* root                                 = NULL;
    uint8_t* buffer                             = NULL;
    const data_manager_descriptor_t* descriptor = NULL;
    esp_err_t result                            = ESP_OK;

    do
    {
        result = data_manager_page_receive_json(request, &root);

        if (result != ESP_OK)
        {
            break;
        }

        result = data_manager_page_find_editable(root, &descriptor);

        if (result != ESP_OK)
        {
            break;
        }

        const cJSON* const json_value = cJSON_GetObjectItemCaseSensitive(root, "value");

        if (!cJSON_IsString(json_value))
        {
            result = ESP_ERR_INVALID_ARG;
            break;
        }

        buffer = calloc(1u, descriptor->size);

        if (buffer == NULL)
        {
            result = ESP_ERR_NO_MEM;
            break;
        }

        size_t size = 0u;

        result =
            data_manager_text_parse(descriptor, json_value->valuestring, buffer, &size);

        if (result != ESP_OK)
        {
            break;
        }

        result = data_manager_set(descriptor->id, buffer, size);
    } while (0);

    free(buffer);
    cJSON_Delete(root);

    return data_manager_page_send_result(request, result);
}

static esp_err_t data_manager_page_h_reset(httpd_req_t* request)
{
    cJSON* root                                 = NULL;
    const data_manager_descriptor_t* descriptor = NULL;
    esp_err_t result                            = ESP_OK;

    do
    {
        result = data_manager_page_receive_json(request, &root);

        if (result != ESP_OK)
        {
            break;
        }

        result = data_manager_page_find_editable(root, &descriptor);

        if (result != ESP_OK)
        {
            break;
        }

        result = data_manager_reset(descriptor, 1u);
    } while (0);

    cJSON_Delete(root);

    return data_manager_page_send_result(request, result);
}

static esp_err_t data_manager_page_h_save(httpd_req_t* request)
{
    return data_manager_page_send_result(request, data_manager_save());
}


/* Exposed API ------------------------------------------------------------- */

esp_err_t data_manager_page_init(void)
{
    esp_err_t error_code = ESP_OK;

    /* Static: http_panel_server keeps the URIs by pointer. */
    static const httpd_uri_t data_manager_page_uris[] = {
        {
            .uri     = "/data",
            .method  = HTTP_GET,
            .handler = data_manager_page_h_root,
        },
        {
            .uri     = "/data/list",
            .method  = HTTP_GET,
            .handler = data_manager_page_h_list,
        },
        {
            .uri     = "/data/set",
            .method  = HTTP_POST,
            .handler = data_manager_page_h_set,
        },
        {
            .uri     = "/data/reset",
            .method  = HTTP_POST,
            .handler = data_manager_page_h_reset,
        },
        {
            .uri     = "/data/save",
            .method  = HTTP_POST,
            .handler = data_manager_page_h_save,
        },
    };

    for (size_t i = 0u; (error_code == ESP_OK) && (i < ARRAY_SIZE(data_manager_page_uris));
         i++)
    {
        error_code = http_panel_server_register_uri(&data_manager_page_uris[i]);
    }

    return error_code;
}

/**** END OF FILE ****/
